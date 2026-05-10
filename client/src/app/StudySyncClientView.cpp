#include "pch.h"
#include "StudySyncClientView.h"
#include "network/WinHttpClient.h"
#include "resource.h"

#include <chrono>
#include <windows.h>
#include <sstream>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CStudySyncClientView, CWnd)
    ON_WM_CREATE()
    ON_WM_DESTROY()
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_SIZE()
    ON_WM_TIMER()
END_MESSAGE_MAP()

namespace {
std::string current_iso8601()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d+09:00",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return buf;
}
} // namespace

// ── 생성 ───────────────────────────────────────────────────────

CStudySyncClientView::CStudySyncClientView(ClientTransportConfig config)
    : transport_config_(std::move(config))
    , transports_(make_client_transports(transport_config_))
    , stats_api_(WinHttpClient::instance())
    , alert_manager_(alert_queue_)
    , worker_pool_(3)
    , capture_thread_(render_buffer_, send_buffer_, shadow_buffer_)
    , render_thread_(render_buffer_)
    , ai_tcp_client_(send_buffer_, shadow_buffer_, event_queue_, result_buffer_, transport_config_.jpeg_quality)
    , dummy_generator_(result_buffer_, shadow_buffer_, event_queue_)
    , event_upload_thread_(event_queue_, *transports_.clip_store, *transports_.log_sink)
    , alert_dispatch_thread_(alert_queue_, toast_buffer_)
    , ai_heartbeat_("AI Server",    transport_config_.ai_server_host + ":9500")
    , main_heartbeat_("Main Server", transport_config_.main_server_url)
    , clip_garbage_collector_(transport_config_.clip_directory,
                              transport_config_.local_clip_retention_days)
{
}

CStudySyncClientView::~CStudySyncClientView()
{
}

// ── 공개 인터페이스 ────────────────────────────────────────────

void CStudySyncClientView::update_session_id(long long session_id)
{
    session_id_ = session_id;
    if (transports_.log_sink) {
        transports_.log_sink->set_session_id(session_id);
    }
    // AI TCP 클라이언트는 이미 동작 중이므로 재시작 없이 session_id만 갱신
    // 이후 전송 패킷부터 새 session_id가 반영됨
    ai_tcp_client_.update_session_id(session_id);
}

void CStudySyncClientView::set_clip_directory(const std::string& dir)
{
    transport_config_.clip_directory = dir;
}

void CStudySyncClientView::set_session_id(long long session_id,
                                          const std::string& start_time)
{
    session_id_         = session_id;
    session_start_time_ = start_time;
    last_ai_state_.clear();

    if (transports_.log_sink) {
        transports_.log_sink->set_session_id(session_id);
    }

    session_start_steady_ms_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    render_thread_.set_session_start_ms(session_start_steady_ms_);

    begin_calibration();
}

void CStudySyncClientView::begin_calibration()
{
    constexpr int kCalibSec = 5;
    {
        std::lock_guard<std::mutex> lock(calib_mtx_);
        calib_samples_.clear();
        calibrating_ = true;
        calib_tick_  = kCalibSec;
    }
    render_thread_.set_calibration_countdown(kCalibSec);
    SetTimer(IDT_CALIB, 1000, nullptr);
}

void CStudySyncClientView::finish_calibration()
{
    double neck_avg = 0.0;
    {
        std::lock_guard<std::mutex> lock(calib_mtx_);
        calibrating_ = false;
        if (!calib_samples_.empty()) {
            for (double v : calib_samples_) neck_avg += v;
            neck_avg /= static_cast<double>(calib_samples_.size());
        } else {
            neck_avg = 25.0;
        }
    }

    constexpr double kMargin = 10.0;
    const double threshold = neck_avg + kMargin;

    if (transport_config_.use_dummy_ai) {
        dummy_generator_.set_neck_threshold(threshold);
    }

    char dbg[128];
    snprintf(dbg, sizeof(dbg),
        "[Calib] neck_avg=%.1f  threshold=%.1f  samples=%zu\n",
        neck_avg, threshold, calib_samples_.size());
    OutputDebugStringA(dbg);

    // 캘리브레이션 완료 → 기본 상태 "공부 중"으로 초기화 (AI 응답 전까지 표시)
    AnalysisResult default_result;
    default_result.state        = "focus";
    default_result.posture_ok   = true;
    default_result.face_detected = 1;
    result_buffer_.update(default_result);

    render_thread_.set_calibration_countdown(0);
    SetTimer(IDT_CALIB_HIDE, 1500, nullptr);
    SetTimer(IDT_LOG_FLUSH, 10'000, nullptr);
}

// ── 윈도우 메시지 ──────────────────────────────────────────────

void CStudySyncClientView::request_server_stats()
{
    if (stats_fetch_pending_.exchange(true)) return;

    worker_pool_.enqueue([this] {
        const ServerStatsSnapshot::Data data = stats_api_.today();
        if (data.valid) server_stats_.update(data);
        stats_fetch_pending_ = false;
    });
}

int CStudySyncClientView::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
    if (CWnd::OnCreate(lpCreateStruct) == -1) return -1;

    // ── 파이프라인 시작 ─────────────────────────────────────
    worker_pool_.start();
    capture_thread_.start(0, transport_config_.capture_fps);
    render_thread_.start(m_hWnd, result_buffer_);

    render_thread_.set_toast_buffer(&toast_buffer_);
    render_thread_.set_stats_history(&stats_history_);
    render_thread_.set_server_stats(&server_stats_);
    alert_dispatch_thread_.set_render_thread(&render_thread_);

    if (transport_config_.use_dummy_ai) {
        dummy_generator_.set_result_callback([this](const AnalysisResult& r) {
            {
                std::lock_guard<std::mutex> lock(calib_mtx_);
                if (calibrating_) { calib_samples_.push_back(r.neck_angle); return; }
            }
            stats_history_.push(r);
            if (session_id_ > 0 && transports_.log_sink)
                transports_.log_sink->append_analysis(r);
            alert_manager_.feed_local_analysis(r);
        });
        dummy_generator_.start(transport_config_.dummy_interval_ms);
    } else {
        ai_tcp_client_.set_result_callback([this](const AnalysisResult& r) {
            {
                std::lock_guard<std::mutex> lock(calib_mtx_);
                if (calibrating_) { calib_samples_.push_back(r.neck_angle); return; }
            }
            stats_history_.push(r);
            if (session_id_ > 0 && transports_.log_sink)
                transports_.log_sink->append_analysis(r);

            // AI 서버 state 변화 시에만 알림 (로컬 임계값 기반 알림 제거)
            if (!r.state.empty() && r.state != last_ai_state_) {
                last_ai_state_ = r.state;
                if (r.state == "drowsy") {
                    Alert alert;
                    alert.type         = AlertType::Drowsy;
                    alert.target       = AlertTarget::Popup;
                    alert.timestamp_ms = r.timestamp_ms;
                    alert.title        = "졸음 감지";
                    alert.message      = "잠깐 스트레칭을 해보세요.";
                    alert_manager_.feed_server_alert(alert);
                } else if (r.state == "distracted") {
                    Alert alert;
                    alert.type         = AlertType::BadPosture;
                    alert.target       = AlertTarget::Popup;
                    alert.timestamp_ms = r.timestamp_ms;
                    alert.title        = "집중력 저하 감지";
                    alert.message      = "다시 집중해봐요!";
                    alert_manager_.feed_server_alert(alert);
                } else if (r.state == "absent") {
                    Alert alert;
                    alert.type         = AlertType::BadPosture;
                    alert.target       = AlertTarget::Popup;
                    alert.timestamp_ms = r.timestamp_ms;
                    alert.title        = "자리 비움 감지";
                    alert.message      = "자리로 돌아오세요.";
                    alert_manager_.feed_server_alert(alert);
                }
            }
        });
        ai_tcp_client_.start(
            transport_config_.ai_server_host,
            transport_config_.ai_server_port,
            session_id_,
            transport_config_.frame_sample_interval);
    }

    event_upload_thread_.set_review_store(&review_store_);
    event_upload_thread_.start();
    alert_dispatch_thread_.start();

    ai_heartbeat_.start([this] { return ai_tcp_client_.is_connected(); });
    main_heartbeat_.start([this] {
        return transports_.log_sink && transports_.log_sink->health_check();
    });

    clip_garbage_collector_.start();
    request_server_stats();
    SetTimer(IDT_STATS_FETCH, 60'000, nullptr);
    return 0;
}

void CStudySyncClientView::OnDestroy()
{
    if (session_id_ > 0) {
        const std::string end_time = current_iso8601();
        const long long sid = session_id_;
        session_id_ = 0;
        std::thread([sid, end_time]() {
            SessionApi session_api(WinHttpClient::instance());
            const SessionEndResult result = session_api.end(sid, end_time);
            if (result.success) {
                std::ostringstream msg;
                msg << std::fixed;
                msg.precision(1);
                msg << "[StudySync] 세션 종료 — "
                    << "집중시간: "   << result.focus_min          << "분, "
                    << "평균집중도: " << result.avg_focus * 100.0f << "%\n";
                OutputDebugStringA(msg.str().c_str());
            }
        }).detach();
    }

    KillTimer(IDT_LOG_FLUSH);
    KillTimer(IDT_CALIB);
    KillTimer(IDT_CALIB_HIDE);
    KillTimer(IDT_STATS_FETCH);

    if (transports_.log_sink) transports_.log_sink->flush();

    clip_garbage_collector_.stop();
    main_heartbeat_.stop();
    ai_heartbeat_.stop();
    alert_dispatch_thread_.stop();
    event_upload_thread_.stop();
    if (transport_config_.use_dummy_ai) {
        dummy_generator_.stop();
    } else {
        ai_tcp_client_.stop();
    }
    render_thread_.stop();
    capture_thread_.stop();
    worker_pool_.stop();

    CWnd::OnDestroy();
}

void CStudySyncClientView::OnPaint()
{
    ValidateRect(nullptr);
}

BOOL CStudySyncClientView::OnEraseBkgnd(CDC* /*pDC*/)
{
    return TRUE;
}

void CStudySyncClientView::OnSize(UINT nType, int cx, int cy)
{
    CWnd::OnSize(nType, cx, cy);

    if (cx > 0 && cy > 0) {
        render_thread_.notify_resize(static_cast<UINT>(cx), static_cast<UINT>(cy));
    }
}

void CStudySyncClientView::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == IDT_CALIB) {
        int remaining = 0;
        {
            std::lock_guard<std::mutex> lock(calib_mtx_);
            --calib_tick_;
            remaining = calib_tick_;
        }
        if (remaining > 0) {
            render_thread_.set_calibration_countdown(remaining);
        } else {
            KillTimer(IDT_CALIB);
            finish_calibration();
        }
    } else if (nIDEvent == IDT_CALIB_HIDE) {
        KillTimer(IDT_CALIB_HIDE);
        render_thread_.set_calibration_countdown(-1);
    } else if (nIDEvent == IDT_LOG_FLUSH) {
        if (transports_.log_sink && session_id_ > 0) {
            auto* sink = transports_.log_sink.get();
            worker_pool_.enqueue([sink] { sink->flush(); });
        }
    } else if (nIDEvent == IDT_STATS_FETCH) {
        request_server_stats();
    }

    CWnd::OnTimer(nIDEvent);
}

