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

// AI 서버 TCP 클라이언트 (Stage 1 변경 후)
//
// 변경 전: JPEG 이미지(~100KB) → AI 서버 (5fps 한계)
// 변경 후: keypoint JSON (~100B) → AI 서버 (30fps 가능)
//
// 흐름:
//   CaptureThread → send_buffer_
//     → LocalMediaPipePoseAnalyzer (클라이언트 로컬 keypoint 추출)
//       → send_keypoint_packet() (JSON only, 바이너리 없음)
//         → AI 서버 (TCN 시계열 분석)
//           → recv_result_packet() (state / confidence / focus_score)
class AiTcpClient {
public:
    AiTcpClient(CaptureThread::SendFrameBuffer& send_buffer,
                EventShadowBuffer& shadow_buffer,
                EventQueue& event_queue,
                AnalysisResultBuffer& result_buffer,
                int /* jpeg_quality — Stage 1 이후 미사용 */);
    ~AiTcpClient();

    void start(const std::string& host, std::uint16_t port, long long session_id, int sample_interval);
    void stop();

    bool is_connected() const { return connected_.load(); }

    using ResultCallback = std::function<void(const AnalysisResult&)>;
    void set_result_callback(ResultCallback cb) { result_callback_ = std::move(cb); }

private:
    void run(std::string host, std::uint16_t port, long long session_id, int sample_interval);

    SOCKET connect_to(const std::string& host, std::uint16_t port);
    void close_socket(SOCKET& socket);

    // 배치 keypoint JSON 전송 (150프레임 단위)
    bool send_batch_packet(SOCKET socket,
                           const std::vector<AnalysisResult>& batch,
                           long long session_id, long long batch_id);

    // AI 서버 응답 수신 (confidence 포함)
    bool recv_result_packet(SOCKET socket, AnalysisResult& out);

    static bool send_all(SOCKET socket, const char* data, int length);
    static bool recv_all(SOCKET socket, char* data, int length);
    static bool send_json_only(SOCKET socket, const std::string& json);

    static std::string now_iso8601();
    static std::string extract_string(const std::string& json, const std::string& key);
    static double extract_number(const std::string& json, const std::string& key, double fallback = 0.0);
    static bool extract_bool(const std::string& json, const std::string& key, bool fallback = false);

    CaptureThread::SendFrameBuffer& send_buffer_;
    EventShadowBuffer& shadow_buffer_;
    EventQueue& event_queue_;
    AnalysisResultBuffer& result_buffer_;
    PostureEventDetector detector_;

    LocalMediaPipePoseAnalyzer pose_analyzer_;   // 클라이언트 로컬 keypoint 추출기

    ResultCallback result_callback_;

    static constexpr int kBatchSize = 150;

    std::vector<AnalysisResult> batch_buffer_;
    long long batch_id_ = 0;

    AnalysisResult   last_result_;
    bool             has_last_result_     = false;
    int              consecutive_failures_ = 0;
    static constexpr int kMaxConsecutiveFailures = 5;

    std::atomic_bool running_{ false };
    std::atomic_bool connected_{ false };
    std::thread worker_;
};
