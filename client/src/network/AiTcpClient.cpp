#include "pch.h"
#include "network/AiTcpClient.h"

#include <ws2tcpip.h>
#include <chrono>
#include <sstream>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace {
// Stage 1: keypoint 전송 프로토콜 번호
constexpr int kProtoKeypointPush  = 2000;
constexpr int kProtoAnalysisResult = 2001;
constexpr std::uint32_t kMaxJsonBytes = 64 * 1024;

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
                         int /* jpeg_quality — Stage 1 이후 미사용 */)
    : send_buffer_(send_buffer)
    , shadow_buffer_(shadow_buffer)
    , event_queue_(event_queue)
    , result_buffer_(result_buffer)
{
    detector_.set_callback([this](PostureEvent event) {
        event_queue_.push(std::move(event));
    });

    // 클라이언트 로컬 MediaPipe 분석기 초기화
    pose_analyzer_.initialize();

    WSADATA wsa{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        log_ai_tcp("WSAStartup failed");
    }
}

AiTcpClient::~AiTcpClient()
{
    stop();
    pose_analyzer_.shutdown();
    WSACleanup();
}

void AiTcpClient::start(const std::string& host,
                        std::uint16_t port,
                        long long session_id,
                        int sample_interval)
{
    if (running_.exchange(true)) return;
    worker_ = std::thread(&AiTcpClient::run, this, host, port, session_id, sample_interval);
}

void AiTcpClient::stop()
{
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AiTcpClient::run(std::string host,
                      std::uint16_t port,
                      long long session_id,
                      int sample_interval)
{
    if (sample_interval <= 0) sample_interval = 1;

    log_ai_tcp("worker started (Stage1 keypoint mode)");

    long long frame_id = 0;

    while (running_) {
        SOCKET socket = connect_to(host, port);
        if (socket == INVALID_SOCKET) {
            connected_ = false;
            log_ai_tcp("connect failed; retrying");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        connected_ = true;
        log_ai_tcp("connected");

        int frame_index = 0;
        while (running_) {
            Frame frame;
            if (!send_buffer_.try_pop(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            // 오래된 프레임을 버리고 가장 최신 프레임만 처리
            Frame newer;
            while (send_buffer_.try_pop(newer)) {
                frame = std::move(newer);
            }

            ++frame_index;
            if (frame_index < sample_interval) {
                continue;
            }
            frame_index = 0;

            // ── Stage 1: 클라이언트에서 keypoint 추출 ────────────
            const auto kp_opt = pose_analyzer_.analyze(frame);
            if (!kp_opt.has_value()) {
                continue;   // 빈 프레임이면 스킵
            }
            const AnalysisResult kp = kp_opt.value();

            // ── keypoint JSON 전송 (JPEG 바이너리 없음) ───────────
            if (!send_keypoint_packet(socket, kp, session_id, ++frame_id)) {
                log_ai_tcp("send failed; reconnecting");
                break;
            }

            // ── AI 서버 응답 수신 (TCN 시계열 판정) ──────────────
            AnalysisResult result = kp;   // keypoint는 클라이언트 값 그대로 보존
            if (!recv_result_packet(socket, result)) {
                log_ai_tcp("receive failed; reconnecting");
                break;
            }

            result_buffer_.update(result);
            detector_.feed(result, shadow_buffer_);
            if (result_callback_) {
                result_callback_(result);
            }
        }

        close_socket(socket);
        connected_ = false;
    }

    log_ai_tcp("worker stopped");
}

SOCKET AiTcpClient::connect_to(const std::string& host, std::uint16_t port)
{
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return INVALID_SOCKET;

    DWORD timeout_ms = 3000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
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

// ── Stage 1: keypoint JSON 전송 (바이너리 없음) ─────────────────────────

bool AiTcpClient::send_keypoint_packet(SOCKET socket,
                                       const AnalysisResult& kp,
                                       long long session_id,
                                       long long frame_id)
{
    std::ostringstream json;
    json << "{"
         << "\"protocol_no\":"   << kProtoKeypointPush
         << ",\"session_id\":"   << session_id
         << ",\"frame_id\":"     << frame_id
         << ",\"timestamp_ms\":" << kp.timestamp_ms
         << ",\"ear\":"          << kp.ear
         << ",\"neck_angle\":"   << kp.neck_angle
         << ",\"shoulder_diff\":" << kp.shoulder_diff
         << ",\"head_yaw\":"     << kp.head_yaw
         << ",\"head_pitch\":"   << kp.head_pitch
         << ",\"face_detected\":" << kp.face_detected
         << "}";

    return send_json_only(socket, json.str());
}

// ── AI 서버 응답 파싱 (confidence 포함) ─────────────────────────────────

bool AiTcpClient::recv_result_packet(SOCKET socket, AnalysisResult& out)
{
    unsigned char header[4]{};
    if (!recv_all(socket, reinterpret_cast<char*>(header), 4)) return false;

    const std::uint32_t json_len =
        (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8)  |
        static_cast<std::uint32_t>(header[3]);

    if (json_len == 0 || json_len > kMaxJsonBytes) return false;

    std::string json(json_len, '\0');
    if (!recv_all(socket, json.data(), static_cast<int>(json.size()))) return false;

    const int protocol_no = static_cast<int>(extract_number(json, "protocol_no"));
    if (protocol_no != kProtoAnalysisResult) {
        return false;
    }

    // AI 서버가 반환하는 필드만 덮어씀. keypoint는 send 시 이미 채워진 상태.
    out.timestamp_ms  = static_cast<std::uint64_t>(extract_number(json, "timestamp_ms", static_cast<double>(out.timestamp_ms)));
    out.focus_score   = static_cast<int>(extract_number(json, "focus_score"));
    out.confidence    = extract_number(json, "confidence", 1.0);
    out.state         = extract_string(json, "state");
    out.posture_ok    = extract_bool(json, "posture_ok", true);
    out.drowsy        = extract_bool(json, "is_drowsy") || extract_bool(json, "drowsy");
    out.absent        = extract_bool(json, "is_absent")  || extract_bool(json, "absent");

    return true;
}

// ── 전송 헬퍼 ──────────────────────────────────────────────────────────

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

// ── JSON 파싱 유틸 ──────────────────────────────────────────────────────

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
        if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E') {
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
