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
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <winsock2.h>

// AI 서버 TCP 클라이언트
//
// 프로토콜 번호:
//   2000 (kProtoKeypointPush)  — 일반 추론용 프레임
//   2002 (kProtoCalibration)   — 캘리브레이션 기준값 계산용 (AI 서버 추론 제외)
//   2001 (kProtoAnalysisResult)— AI 서버 → 클라이언트 응답
//
// 캘리브레이션 완료 조건: 2002 프레임 150개 전송 시 on_calibration_complete_ 콜백 호출
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

    void update_session_id(long long session_id) { session_id_.store(session_id); }

    void set_camera_fps(int fps);

    // 캘리브레이션 모드 시작 (true) / 종료 (false)
    // true 시 카운터 리셋 — 150프레임 도달 시 자동으로 false로 전환 후 콜백 호출
    void set_calibration_mode(bool on);

    // 150프레임 전송 완료 시 호출되는 콜백 (워커 스레드에서 호출 — UI 스레드에서 PostMessage 권장)
    void set_on_calibration_complete(std::function<void()> cb);

    bool is_connected() const { return connected_.load(); }

    using ResultCallback = std::function<void(const AnalysisResult&)>;
    void set_result_callback(ResultCallback cb) { result_callback_ = std::move(cb); }

private:
    static constexpr int kCalibFrameTarget = 150;

    void run(std::string host, std::uint16_t port, int sample_interval);
    void recv_loop(SOCKET socket, std::atomic_bool& conn_alive);

    SOCKET connect_to(const std::string& host, std::uint16_t port);
    void close_socket(SOCKET& socket);

    bool send_keypoint_packet(SOCKET socket, const AnalysisResult& kp,
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
    EventShadowBuffer& shadow_buffer_;
    EventQueue& event_queue_;
    AnalysisResultBuffer& result_buffer_;
    PostureEventDetector detector_;

    LocalMediaPipePoseAnalyzer pose_analyzer_;

    ResultCallback result_callback_;

    std::mutex       result_mutex_;
    AnalysisResult   last_result_;
    bool             has_last_result_ = false;

    AnalysisResult   prev_kp_;

    std::mutex                calib_cb_mtx_;
    std::function<void()>     on_calibration_complete_;

    std::atomic<long long> session_id_{ 0 };
    std::atomic<int>       camera_fps_{ 30 };
    std::atomic<int>       calib_frames_sent_{ 0 };
    std::atomic_bool       calibration_mode_{ false };
    std::atomic_bool       running_{ false };
    std::atomic_bool       connected_{ false };
    std::thread            worker_;
};
