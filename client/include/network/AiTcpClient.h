#pragma once

#include "analysis/LocalMediaPipePoseAnalyzer.h"
#include "capture/CaptureThread.h"
#include "event/EventQueue.h"
#include "event/EventShadowBuffer.h"
#include "event/PostureEventDetector.h"
#include "model/AnalysisResult.h"
#include "model/AnalysisResultBuffer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <winsock2.h>

// AI 서버 TCP 클라이언트 (배치 전송 모드)
//
// 흐름:
//   CaptureThread → send_buffer_
//     → LocalMediaPipePoseAnalyzer (로컬 keypoint 추출, 매 sample_interval 프레임)
//       → 로컬 keypoint 즉시 AlertManager 반영 (서버 응답 불필요)
//       → 5초(kBatchIntervalMs)마다 keypoint 배치를 AI 서버로 전송
//         → 응답 수신: state / focus_score 갱신
//         → 응답 없음: 이전 state 유지, kMaxConsecutiveFailures 초과 시 재접속
class AiTcpClient {
public:
    AiTcpClient(CaptureThread::SendFrameBuffer& send_buffer,
                EventShadowBuffer& shadow_buffer,
                EventQueue& event_queue,
                AnalysisResultBuffer& result_buffer,
                int /* jpeg_quality — 미사용 */);
    ~AiTcpClient();

    void start(const std::string& host, std::uint16_t port, long long session_id, int sample_interval);
    void stop();

    bool is_connected() const { return connected_.load(); }

    using ResultCallback = std::function<void(const AnalysisResult&)>;
    void set_result_callback(ResultCallback cb) { result_callback_ = std::move(cb); }

private:
    static constexpr int kBatchIntervalMs        = 5000; // 5초마다 배치 전송
    static constexpr int kMaxConsecutiveFailures = 3;    // 연속 수신 실패 허용 횟수
    static constexpr int kRecvTimeoutMs          = 8000; // 수신 타임아웃 (배치 처리 여유)

    void run(std::string host, std::uint16_t port, long long session_id, int sample_interval);

    SOCKET connect_to(const std::string& host, std::uint16_t port);
    void close_socket(SOCKET& socket);

    bool send_batch_packet(SOCKET socket, const std::vector<AnalysisResult>& batch,
                           long long session_id, long long frame_id);
    bool recv_result_packet(SOCKET socket, AnalysisResult& out);

    static bool send_all(SOCKET socket, const char* data, int length);
    static bool recv_all(SOCKET socket, char* data, int length);
    static bool send_json_only(SOCKET socket, const std::string& json);

    static std::string now_iso8601();
    static std::string extract_string(const std::string& json, const std::string& key);
    static double extract_number(const std::string& json, const std::string& key, double fallback = 0.0);
    static bool extract_bool(const std::string& json, const std::string& key, bool fallback = false);

    CaptureThread::SendFrameBuffer& send_buffer_;
    EventShadowBuffer&              shadow_buffer_;
    EventQueue&                     event_queue_;
    AnalysisResultBuffer&           result_buffer_;
    PostureEventDetector            detector_;
    LocalMediaPipePoseAnalyzer      pose_analyzer_;

    ResultCallback result_callback_;

    AnalysisResult last_server_result_;
    bool           has_server_result_ = false;

    std::atomic_bool running_{ false };
    std::atomic_bool connected_{ false };
    std::thread      worker_;
};
