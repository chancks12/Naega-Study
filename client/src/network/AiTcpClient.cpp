#include "pch.h"
#include "network/AiTcpClient.h"

#include <ws2tcpip.h>
#include <chrono>
#include <sstream>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace {
constexpr int kProtoKeypointPush   = 2000;
constexpr int kProtoCalibration    = 2002;
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
                         int /* jpeg_quality */)
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

void AiTcpClient::set_camera_fps(int fps)
{
    const int validated = fps > 0 ? fps : 30;
    camera_fps_.store(validated);
    detector_.set_camera_fps(validated);
}

void AiTcpClient::set_calibration_mode(bool on)
{
    if (on) calib_frames_sent_.store(0);
    calibration_mode_.store(on);
}

void AiTcpClient::set_on_calibration_complete(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lk(calib_cb_mtx_);
    on_calibration_complete_ = std::move(cb);
}

void AiTcpClient::start(const std::string& host,
                        std::uint16_t port,
                        long long session_id,
                        int sample_interval)
{
    if (running_.exchange(true)) return;
    session_id_.store(session_id);
    worker_ = std::thread(&AiTcpClient::run, this, host, port, sample_interval);
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
                      int sample_interval)
{
    if (sample_interval <= 0) sample_interval = 1;
    log_ai_tcp("worker started");

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

        std::atomic_bool conn_alive{ true };
        std::thread recv_th([this, socket, &conn_alive] {
            recv_loop(socket, conn_alive);
        });

        int frame_index = 0;
        while (running_ && conn_alive) {
            Frame frame;
            if (!send_buffer_.try_pop(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            // 대기 중 쌓인 프레임은 버리고 최신 프레임만 사용
            Frame newer;
            while (send_buffer_.try_pop(newer)) frame = std::move(newer);

            ++frame_index;
            if (frame_index < sample_interval) continue;
            frame_index = 0;

            const auto kp_opt = pose_analyzer_.analyze(frame);
            if (!kp_opt.has_value()) continue;
            const AnalysisResult kp = kp_opt.value();

            const bool is_calib = calibration_mode_.load();

            if (!is_calib) {
                // 일반 모드: 보간 프레임 삽입
                const int cam_fps = camera_fps_.load();
                constexpr int kTargetFps = 30;
                const int n_interp = (cam_fps > 0 && cam_fps < kTargetFps)
                                     ? (kTargetFps / cam_fps - 1) : 0;

                for (int i = 1; i <= n_interp; ++i) {
                    const float alpha = static_cast<float>(i) / (n_interp + 1);
                    AnalysisResult interp = prev_kp_;
                    interp.ear           = prev_kp_.ear           + alpha * (kp.ear           - prev_kp_.ear);
                    interp.neck_angle    = prev_kp_.neck_angle    + alpha * (kp.neck_angle    - prev_kp_.neck_angle);
                    interp.shoulder_diff = prev_kp_.shoulder_diff + alpha * (kp.shoulder_diff - prev_kp_.shoulder_diff);
                    interp.head_yaw      = prev_kp_.head_yaw      + alpha * (kp.head_yaw      - prev_kp_.head_yaw);
                    interp.head_pitch    = prev_kp_.head_pitch    + alpha * (kp.head_pitch    - prev_kp_.head_pitch);
                    interp.face_detected = kp.face_detected;
                    interp.timestamp_ms  = prev_kp_.timestamp_ms
                        + static_cast<std::uint64_t>(alpha * static_cast<float>(kp.timestamp_ms - prev_kp_.timestamp_ms));

                    if (!send_keypoint_packet(socket, interp, session_id_.load(), ++frame_id)) {
                        log_ai_tcp("send(interp) failed; reconnecting");
                        conn_alive = false;
                        break;
                    }
                }
                if (!conn_alive) break;
            }
            prev_kp_ = kp;

            if (!send_keypoint_packet(socket, kp, session_id_.load(), ++frame_id)) {
                log_ai_tcp("send failed; reconnecting");
                conn_alive = false;
                break;
            }

            if (is_calib) {
                // 캘리브레이션 프레임 카운트 증가; 150개 도달 시 자동 완료
                const int sent = calib_frames_sent_.fetch_add(1) + 1;
                if (sent >= kCalibFrameTarget) {
                    calibration_mode_.store(false);
                    std::function<void()> cb;
                    {
                        std::lock_guard<std::mutex> lk(calib_cb_mtx_);
                        cb = on_calibration_complete_;
                    }
                    if (cb) cb();
                }
                continue; // UI 갱신 / 이벤트 감지 생략
            }

            // 일반 모드: UI 갱신 + 이벤트 감지
            AnalysisResult display = kp;
            bool ai_has_responded = false;
            {
                std::lock_guard<std::mutex> lock(result_mutex_);
                if (has_last_result_) {
                    ai_has_responded      = true;
                    display               = last_result_;
                    display.ear           = kp.ear;
                    display.neck_angle    = kp.neck_angle;
                    display.shoulder_diff = kp.shoulder_diff;
                    display.head_yaw      = kp.head_yaw;
                    display.head_pitch    = kp.head_pitch;
                    display.face_detected = kp.face_detected;
                    display.timestamp_ms  = kp.timestamp_ms;
                }
            }

            result_buffer_.update(display);

            AnalysisResult ev = display;
            if (!ai_has_responded) ev.state.clear();
            detector_.feed(ev, shadow_buffer_);

            if (result_callback_) result_callback_(display);
        }

        conn_alive = false;
        close_socket(socket);
        recv_th.join();
        connected_ = false;
    }

    log_ai_tcp("worker stopped");
}

void AiTcpClient::recv_loop(SOCKET socket, std::atomic_bool& conn_alive)
{
    while (running_ && conn_alive) {
        AnalysisResult result;
        if (!recv_result_packet(socket, result)) {
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;
            if (err == 0) {
                log_ai_tcp("recv parse mismatch; ignoring packet");
                continue;
            }
            log_ai_tcp("recv socket error; signaling reconnect");
            conn_alive = false;
            return;
        }

        if (calibration_mode_.load()) continue;

        std::lock_guard<std::mutex> lock(result_mutex_);
        last_result_      = result;
        has_last_result_  = true;
        log_ai_tcp("state updated from server");
    }
}

SOCKET AiTcpClient::connect_to(const std::string& host, std::uint16_t port)
{
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return INVALID_SOCKET;

    DWORD rcv_timeout_ms = 10000;
    DWORD snd_timeout_ms = 8000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv_timeout_ms), sizeof(rcv_timeout_ms));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd_timeout_ms), sizeof(snd_timeout_ms));

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

bool AiTcpClient::send_keypoint_packet(SOCKET socket,
                                       const AnalysisResult& kp,
                                       long long session_id,
                                       long long frame_id)
{
    const int proto = calibration_mode_.load() ? kProtoCalibration : kProtoKeypointPush;

    std::ostringstream json;
    json << "{"
         << "\"protocol_no\":"    << proto
         << ",\"session_id\":"    << session_id
         << ",\"frame_id\":"      << frame_id
         << ",\"timestamp_ms\":"  << kp.timestamp_ms
         << ",\"ear\":"           << kp.ear
         << ",\"neck_angle\":"    << kp.neck_angle
         << ",\"shoulder_diff\":" << kp.shoulder_diff
         << ",\"head_yaw\":"      << kp.head_yaw
         << ",\"head_pitch\":"    << kp.head_pitch
         << ",\"face_detected\":" << kp.face_detected
         << "}";

    return send_json_only(socket, json.str());
}

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

    if (static_cast<int>(extract_number(json, "protocol_no")) != kProtoAnalysisResult)
        return false;

    out.timestamp_ms = static_cast<std::uint64_t>(extract_number(json, "timestamp_ms", static_cast<double>(out.timestamp_ms)));
    out.focus_score  = static_cast<int>(extract_number(json, "focus_score"));
    out.confidence   = extract_number(json, "confidence", 1.0);
    out.state        = extract_string(json, "state");
    if (out.state.empty()) out.state = "focus";
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
    const std::string pattern = "\"" + key + "\":";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) return {};

    pos += pattern.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;

    std::string value;
    for (std::size_t i = pos; i < json.size(); ++i) {
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
