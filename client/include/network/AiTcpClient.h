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
// 전송: 매 프레임마다 keypoint JSON을 서버로 전송 (단방향, recv 대기 없음)
// 수신: 별도 스레드에서 서버 응답 대기. 서버는 150프레임 추론 후 state가
//       바뀔 때만 응답을 보내므로, 응답이 없는 구간에는 마지막 state 유지.
//
// 흐름:
//   CaptureThread → send_buffer_
//     → LocalMediaPipePoseAnalyzer (keypoint 추출)
//       → send_keypoint_packet() ──────────────────→ AI 서버 (매 프레임)
//       ← recv_loop() (별도 스레드, state 변화 시만 수신) ←
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

    // 카메라 실제 fps 설정 — keypoint 보간 비율 및 클립 윈도우(7초) 계산에 사용
    void set_camera_fps(int fps);

    bool is_connected() const { return connected_.load(); }

    using ResultCallback = std::function<void(const AnalysisResult&)>;
    void set_result_callback(ResultCallback cb) { result_callback_ = std::move(cb); }

private:
    // 전송 루프 (worker_ 스레드)
    void run(std::string host, std::uint16_t port, int sample_interval);

    // 수신 루프 (run 내부에서 별도 스레드로 실행)
    // conn_alive가 false가 되거나 수신 오류 발생 시 종료
    void recv_loop(SOCKET socket, std::atomic_bool& conn_alive);

    SOCKET connect_to(const std::string& host, std::uint16_t port);
    void close_socket(SOCKET& socket);

    // 단일 프레임 keypoint JSON 전송
    bool send_keypoint_packet(SOCKET socket, const AnalysisResult& kp,
                              long long session_id, long long frame_id);

    // AI 서버 응답 수신 (state 변화 시에만 서버가 전송)
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

    AnalysisResult   prev_kp_;           // 선형 보간 기준점 (직전 실측 keypoint)

    std::atomic<long long> session_id_{ 0 };
    std::atomic<int>       camera_fps_{ 30 }; // 사용자 설정 카메라 fps
    std::atomic_bool       running_{ false };
    std::atomic_bool       connected_{ false };
    std::thread            worker_;
};
