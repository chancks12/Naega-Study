#include "pch.h"
#include "network/AiTcpClient.h"

#include <ws2tcpip.h>
#include <chrono>
#include <sstream>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace {
constexpr int kProtoKeypointPush   = 2000;
constexpr int kProtoAnalysisResult = 2001;
constexpr std::uint32_t kMaxJsonBytes = 256 * 1024; // 배치 전송으로 크기 증가

void log_ai_tcp(const char* message)
{
    std::ostringstream out;
    out << "[StudySync][AI-TCP] " << message << "\n";
    OutputDebugStringA(out.str().c_str());
}
} // namespace

AiTcpClient::AiTcpClient(CaptureThread::SendFrameBuffer& send_buffer,
                         EventShadowBuffer& shadow_buffer,
                         EventQueue& event_queue,
                         AnalysisResultBuffer& result_buffer,
                         int)
    : send_buffer_(send_buffer)
    , shadow_buffer_(shadow_buffer)
    , event_queue_(event_queue)
    , result_buffer_(result_buffer)
{
    detector_.set_callback([this](PostureEvent event) {
        event_queue_.push(std::move(event));
    });

    pose_analyzer_.initialize();

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_ai_tcp("WSAStartup failed");
    }
}

AiTcpClient::~AiTcpClient()
{
    stop();
    pose_analyzer_.shutdown();
    WSACleanup();
}

void AiTcpClient::start(const std::string& host, std::uint16_t port,
                        long long session_id, int sample_interval)
{
    if (running_.exchange(true)) return;
    worker_ = std::thread(&AiTcpClient::run, this, host, port, session_id, sample_interval);
}

void AiTcpClient::stop()
{
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void AiTcpClient::run(std::string host, std::uint16_t port,
                      long long session_id, int sample_interval)
{
    if (sample_interval <= 0) sample_interval = 1;
    log_ai_tcp("worker started (batch mode, 5s interval)");

    long long batch_id = 0;

    while (running_) {
        SOCKET socket = connect_to(host, port);
        if (socket == INVALID_SOCKET) {
            connected_ = false;
            log_ai_tcp("connect failed; retrying in 2s");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        connected_ = true;
        log_ai_tcp("connected");

        int frame_index = 0;
        int consecutive_failures = 0;
        std::vector<AnalysisResult> batch;
        batch.reserve(64);

        using Clock = std::chrono::steady_clock;
        auto batch_start = Clock::now();

        while (running_) {
            Frame frame;
            if (!send_buffer_.try_pop(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            // 최신 프레임만 사용
            Frame newer;
            while (send_buffer_.try_pop(newer)) frame = std::move(newer);

            ++frame_index;
            if (frame_index < sample_interval) continue;
            frame_index = 0;

            // 로컬 MediaPipe 분석
            auto kp_opt = pose_analyzer_.analyze(frame);
            if (!kp_opt.has_value()) continue;

            const AnalysisResult kp = kp_opt.value();
            batch.push_back(kp);

            // ── 로컬 keypoint를 즉시 콜백 (서버 응답과 무관하게 Alert 작동) ──
            if (result_callback_) {
                AnalysisResult local = has_server_result_ ? last_server_result_ : kp;
                local.ear           = kp.ear;
                local.neck_angle    = kp.neck_angle;
                local.shoulder_diff = kp.shoulder_diff;
                local.head_yaw      = kp.head_yaw;
                local.head_pitch    = kp.head_pitch;
                local.face_detected = kp.face_detected;
                local.timestamp_ms  = kp.timestamp_ms;
                result_buffer_.update(local);
                detector_.feed(local, shadow_buffer_);
                result_callback_(local);
            }

            // ── 5초 경과 시 배치 전송 ──────────────────────────────────
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - batch_start).count();

            if (elapsed_ms < kBatchIntervalMs || batch.empty()) continue;

            if (!send_batch_packet(socket, batch, session_id, ++batch_id)) {
                log_ai_tcp("send failed; reconnecting");
                break;
            }
            batch.clear();
            batch_start = Clock::now();

            // ── 서버 응답 수신 ────────────────────────────────────────
            AnalysisResult server_result = has_server_result_ ? last_server_result_ : AnalysisResult{};
            if (recv_result_packet(socket, server_result)) {
                last_server_result_ = server_result;
                has_server_result_  = true;
                consecutive_failures = 0;
                log_ai_tcp("batch response received");
            } else {
                ++consecutive_failures;
                if (consecutive_failures >= kMaxConsecutiveFailures) {
                    log_ai_tcp("too many recv failures; reconnecting");
                    break;
                }
                log_ai_tcp("recv timeout; using last state");
                // 이전 state 유지 — 루프 계속 (재접속 없음)
            }
        }

        close_socket(socket);
        connected_ = false;
        if (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    log_ai_tcp("worker stopped");
}

SOCKET AiTcpClient::connect_to(const std::string& host, std::uint16_t port)
{
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return INVALID_SOCKET;

    DWORD recv_timeout = kRecvTimeoutMs;
    DWORD send_timeout = 3000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    if (connect(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    return socket;
}

void AiTcpClient::close_socket(SOCKET& socket)
{
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
        socket = INVALID_SOCKET;
    }
}

bool AiTcpClient::send_batch_packet(SOCKET socket,
                                    const std::vector<AnalysisResult>& batch,
                                    long long session_id,
                                    long long batch_id)
{
    std::ostringstream json;
    json << "{"
         << "\"protocol_no\":"  << kProtoKeypointPush
         << ",\"session_id\":"  << session_id
         << ",\"batch_id\":"    << batch_id
         << ",\"count\":"       << batch.size()
         << ",\"keypoints\":[";

    for (std::size_t i = 0; i < batch.size(); ++i) {
        const auto& kp = batch[i];
        if (i > 0) json << ",";
        json << "{"
             << "\"timestamp_ms\":"  << kp.timestamp_ms
             << ",\"ear\":"          << kp.ear
             << ",\"neck_angle\":"   << kp.neck_angle
             << ",\"shoulder_diff\":" << kp.shoulder_diff
             << ",\"head_yaw\":"     << kp.head_yaw
             << ",\"head_pitch\":"   << kp.head_pitch
             << ",\"face_detected\":" << kp.face_detected
             << "}";
    }
    json << "]}";

    return send_json_only(socket, json.str());
}

bool AiTcpClient::recv_result_packet(SOCKET socket, AnalysisResult& out)
{
    unsigned char header[4]{};
    if (!recv_all(socket, reinterpret_cast<char*>(header), 4)) return false;

    const std::uint32_t json_len =
        (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) <<  8) |
         static_cast<std::uint32_t>(header[3]);

    if (json_len == 0 || json_len > kMaxJsonBytes) return false;

    std::string json(json_len, '\0');
    if (!recv_all(socket, json.data(), static_cast<int>(json.size()))) return false;

    const int protocol_no = static_cast<int>(extract_number(json, "protocol_no"));
    if (protocol_no != kProtoAnalysisResult) return false;

    out.timestamp_ms = static_cast<std::uint64_t>(
        extract_number(json, "timestamp_ms", static_cast<double>(out.timestamp_ms)));
    out.focus_score  = static_cast<int>(extract_number(json, "focus_score"));
    out.confidence   = extract_number(json, "confidence", 1.0);
    out.state        = extract_string(json, "state");
    out.posture_ok   = extract_bool(json, "posture_ok", true);
    out.drowsy       = extract_bool(json, "is_drowsy") || extract_bool(json, "drowsy");
    out.absent       = extract_bool(json, "is_absent")  || extract_bool(json, "absent");

    return true;
}

bool AiTcpClient::send_json_only(SOCKET socket, const std::string& json)
{
    const std::uint32_t len = static_cast<std::uint32_t>(json.size());
    unsigned char header[4] = {
        static_cast<unsigned char>((len >> 24) & 0xFF),
        static_cast<unsigned char>((len >> 16) & 0xFF),
        static_cast<unsigned char>((len >>  8) & 0xFF),
        static_cast<unsigned char>( len        & 0xFF),
    };

    if (!send_all(socket, reinterpret_cast<const char*>(header), 4)) return false;
    if (!send_all(socket, json.data(), static_cast<int>(json.size()))) return false;
    return true;
}

bool AiTcpClient::send_all(SOCKET socket, const char* data, int length)
{
    int sent = 0;
    while (sent < length) {
        const int n = send(socket, data + sent, length - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool AiTcpClient::recv_all(SOCKET socket, char* data, int length)
{
    int received = 0;
    while (received < length) {
        const int n = recv(socket, data + received, length - received, 0);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

std::string AiTcpClient::now_iso8601()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buffer[32]{};
    snprintf(buffer, sizeof(buffer),
             "%04d-%02d-%02dT%02d:%02d:%02d+09:00",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::string AiTcpClient::extract_string(const std::string& json, const std::string& key)
{
    const std::string pattern = "\"" + key + "\":\"";
    const auto pos = json.find(pattern);
    if (pos == std::string::npos) return {};

    std::string value;
    for (std::size_t i = pos + pattern.size(); i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            value += json[++i];
        } else if (json[i] == '"') {
            break;
        } else {
            value += json[i];
        }
    }
    return value;
}

double AiTcpClient::extract_number(const std::string& json, const std::string& key, double fallback)
{
    const std::string pattern = "\"" + key + "\":";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) return fallback;

    pos += pattern.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;

    std::string value;
    for (std::size_t i = pos; i < json.size(); ++i) {
        const char ch = json[i];
        if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' ||
            ch == '.' || ch == 'e' || ch == 'E') {
            value += ch;
        } else {
            break;
        }
    }
    return value.empty() ? fallback : std::stod(value);
}

bool AiTcpClient::extract_bool(const std::string& json, const std::string& key, bool fallback)
{
    const std::string pattern = "\"" + key + "\":";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) return fallback;

    pos += pattern.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;

    if (json.compare(pos, 4, "true")  == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return fallback;
}
