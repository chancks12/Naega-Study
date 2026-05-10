#pragma once

#include "alert/AlertDispatchThread.h"
#include "alert/AlertManager.h"
#include "model/Alert.h"
#include "analysis/DummyAnalysisGenerator.h"
#include "model/ReviewEventStore.h"
#include "model/ServerStatsSnapshot.h"
#include "model/SessionStatsHistory.h"
#include "model/ToastBuffer.h"
#include "capture/CaptureThread.h"
#include "core/WorkerThreadPool.h"
#include "event/EventQueue.h"
#include "event/EventShadowBuffer.h"
#include "model/AnalysisResultBuffer.h"
#include "network/AiTcpClient.h"
#include "network/ClientTransportConfig.h"
#include "network/ClientTransportFactory.h"
#include "network/EventUploadThread.h"
#include "network/HeartbeatClient.h"
#include "network/LocalClipGarbageCollector.h"
#include "network/SessionApi.h"
#include "network/StatsApi.h"
#include "render/RenderThread.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class CStudySyncClientView : public CWnd {
public:
    explicit CStudySyncClientView(ClientTransportConfig config = {});
    ~CStudySyncClientView() override;

    // 세션 ID + 시작 시각 주입 (MainFrm::start_capture() 에서 호출)
    void set_session_id(long long session_id, const std::string& start_time);

    // 세션 API 응답 도착 후 session_id만 갱신 (비동기 세션 시작 시 사용)
    void update_session_id(long long session_id);

    // 세션별 클립 저장 경로 갱신
    void set_clip_directory(const std::string& dir);

    // 학습 종료 콜백 — "학습 종료" 버튼 클릭 시 MainFrm::stop_capture() 호출
    void set_stop_callback(std::function<void()> cb) { stop_cb_ = std::move(cb); }

    ReviewEventStore& review_store() { return review_store_; }
    long long session_id() const     { return session_id_; }

protected:
    afx_msg int  OnCreate(LPCREATESTRUCT lpCreateStruct);
    afx_msg void OnDestroy();
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    DECLARE_MESSAGE_MAP()

    static constexpr UINT_PTR IDT_CALIB       = 1;
    static constexpr UINT_PTR IDT_CALIB_HIDE  = 2;
    static constexpr UINT_PTR IDT_LOG_FLUSH   = 3;
    static constexpr UINT_PTR IDT_STATS_FETCH = 4;

    void begin_calibration();
    void finish_calibration();
    void request_server_stats();

    std::mutex          calib_mtx_;
    std::vector<double> calib_samples_;
    bool                calibrating_ = false;
    int                 calib_tick_  = 0;

private:
    // 멤버 선언 순서 = 초기화 순서 (의존 관계 유지)
    CaptureThread::RenderFrameBuffer render_buffer_;
    CaptureThread::SendFrameBuffer   send_buffer_;
    EventShadowBuffer                shadow_buffer_;
    EventQueue                       event_queue_;
    AlertQueue                       alert_queue_;
    AnalysisResultBuffer             result_buffer_;
    ToastBuffer                      toast_buffer_;
    SessionStatsHistory              stats_history_;
    ServerStatsSnapshot              server_stats_;
    ReviewEventStore                 review_store_;

    ClientTransportConfig transport_config_;
    ClientTransports      transports_;
    StatsApi              stats_api_;
    std::atomic_bool      stats_fetch_pending_{ false };
    long long             session_id_ = 0;
    std::string           session_start_time_;
    std::uint64_t         session_start_steady_ms_ = 0;

    AlertManager            alert_manager_;
    WorkerThreadPool        worker_pool_;
    CaptureThread           capture_thread_;
    RenderThread            render_thread_;
    AiTcpClient             ai_tcp_client_;
    DummyAnalysisGenerator  dummy_generator_;
    EventUploadThread       event_upload_thread_;
    AlertDispatchThread     alert_dispatch_thread_;
    HeartbeatClient         ai_heartbeat_;
    HeartbeatClient         main_heartbeat_;
    LocalClipGarbageCollector clip_garbage_collector_;

    std::function<void()> stop_cb_;
    std::string           last_ai_state_;   // AI 서버 마지막 state (중복 알림 방지)
};
