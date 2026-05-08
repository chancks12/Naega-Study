#include "pch.h"
#include "StudySyncClientView.h"
#include "network/WinHttpClient.h"

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

// ── 내부 유틸 ──────────────────────────────────────────────────
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

CStudySyncClientView::CStudySyncClientView()
    : alert_manager_(alert_queue_)
    , worker_pool_(3)
    , transport_config_()
    , transports_(make_client_transports(transport_config_))
    , stats_api_(WinHttpClient::instance())
    , capture_thread_(render_buffer_, send_buffer_, shadow_buffer_)
    , render_thread_(render_buffer_)
    , ai_tcp_client_(send_buffer_, shadow_buffer_, event_queue_, result_buffer_, transport_config_.jpeg_quality)
    , dummy_generator_(result_buffer_, shadow_buffer_, event_queue_)
    , event_upload_thread_(event_queue_, *transports_.clip_store, *transports_.log_sink)
    , alert_dispatch_thread_(alert_queue_, toast_buffer_)
    , ai_heartbeat_("AI Server",    transport_config_.ai_server_host + ":9100")
    , main_heartbeat_("Main Server", transport_config_.main_server_url)
    , clip_garbage_collector_(transport_config_.clip_directory,
                              transport_config_.local_clip_retention_days)
{
}

CStudySyncClientView::~CStudySyncClientView()
{
}

// ── 공개 인터페이스 ────────────────────────────────────────────

void CStudySyncClientView::set_session_id(long long session_id,
                                          const std::string& start_time)
{
    session_id_         = session_id;
    session_start_time_ = start_time;

    // JSONL 로그 라인에 session_id 포함
    if (transports_.log_sink) {
        transports_.log_sink->set_session_id(session_id);
    }

    // 세션 타이머 시작 (steady_clock 기준)
    session_start_steady_ms_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    render_thread_.set_session_start_ms(session_start_steady_ms_);

    // ── 캘리브레이션 시작 ─────────────────────────────────────────
    begin_calibration();
}

void CStudySyncClientView::begin_calibration()
{
    constexpr int kCalibSec = 5;
    {
        std::lock_guard<std::mutex> lock(calib_mtx_);
        calib_samples_.clear();
        calibrating_  = true;
        calib_tick_   = kCalibSec;
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
            neck_avg = 25.0; // 샘플 없으면 기본값
        }
    }

    // 기준 자세 + 여유 마진 10도를 임계값으로 설정
    constexpr double kMargin = 10.0;
    const double threshold = neck_avg + kMargin;

    if (transport_config_.use_dummy_ai) {
        dummy_generator_.set_neck_threshold(threshold);
    }
    // (실제 AI 파이프라인은 AiTcpClient 내부 detector에도 동일하게 적용 필요)

    char dbg[128];
    snprintf(dbg, sizeof(dbg),
        "[Calib] neck_avg=%.1f  threshold=%.1f  samples=%zu\n",
        neck_avg, threshold, calib_samples_.size());
    OutputDebugStringA(dbg);

    // 완료 메시지 표시 후 1.5초 뒤 오버레이 숨김
    render_thread_.set_calibration_countdown(0);
    SetTimer(IDT_CALIB_HIDE, 1500, nullptr);

    // 분석 데이터 주기적 flush 시작 (10초마다)
    SetTimer(IDT_LOG_FLUSH, 10'000, nullptr);
}

// ── 윈도우 메시지 ──────────────────────────────────────────────

void CStudySyncClientView::request_server_stats()
{
    // Keep HTTP outside the render thread. The HUD reads only the cached snapshot.
    if (stats_fetch_pending_.exchange(true)) {
        return;
    }

    worker_pool_.enqueue([this] {
        const ServerStatsSnapshot::Data data = stats_api_.today();
        if (data.valid) {
            server_stats_.update(data);
        }
        stats_fetch_pending_ = false;
    });
}

int CStudySyncClientView::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
    if (CWnd::OnCreate(lpCreateStruct) == -1) return -1;

    worker_pool_.start();
    capture_thread_.start(0, transport_config_.capture_fps);
    render_thread_.start(m_hWnd, result_buffer_);

    // 토스트 버퍼 / 통계 히스토리 연결 (렌더 스레드 시작 직후)
    render_thread_.set_toast_buffer(&toast_buffer_);
    render_thread_.set_stats_history(&stats_history_);
    render_thread_.set_server_stats(&server_stats_);

    // 휴식 알림 오버레이용 렌더 스레드 연결
    alert_dispatch_thread_.set_render_thread(&render_thread_);

    if (transport_config_.use_dummy_ai) {
        // AI 서버 없이 더미 분석결과로 전체 파이프라인 테스트
        // AlertManager 콜백 등록 → 자세/졸음 감지 시 toast_buffer_에 기록
        dummy_generator_.set_result_callback([this](const AnalysisResult& r) {
            // 캘리브레이션 중: neck_angle 샘플 수집, 알림/로그 억제
            {
                std::lock_guard<std::mutex> lock(calib_mtx_);
                if (calibrating_) {
                    calib_samples_.push_back(r.neck_angle);
                    return;
                }
            }

            // 통계 히스토리 누적 (그래프용)
            stats_history_.push(r);

            // 분석 데이터 로그 누적 (세션 활성 시에만)
            if (session_id_ > 0 && transports_.log_sink) {
                transports_.log_sink->append_analysis(r);
            }

            alert_manager_.feed_local_analysis(r);
        });
        dummy_generator_.start(transport_config_.dummy_interval_ms);
    } else {
        ai_tcp_client_.set_result_callback([this](const AnalysisResult& r) {
            {
                std::lock_guard<std::mutex> lock(calib_mtx_);
                if (calibrating_) {
                    calib_samples_.push_back(r.neck_angle);
                    return;
                }
            }

            stats_history_.push(r);

            if (session_id_ > 0 && transports_.log_sink) {
                transports_.log_sink->append_analysis(r);
            }

            alert_manager_.feed_local_analysis(r);
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

    ai_heartbeat_.start([this] {
        return ai_tcp_client_.is_connected();
    });
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
    // ── 세션 종료 먼저 (스레드 종료 전에 호출) ──────────────
    if (session_id_ > 0) {
        const std::string end_time = current_iso8601();
        SessionApi session_api(WinHttpClient::instance());
        const SessionEndResult result = session_api.end(session_id_, end_time);

        if (result.success) {
            // 종료 통계 디버그 출력 (추후 UI 표시 가능)
            std::ostringstream msg;
            msg << std::fixed;
            msg.precision(1);
            msg << "[StudySync] 세션 종료 — "
                << "집중시간: "  << result.focus_min               << "분, "
                << "평균집중도: " << result.avg_focus * 100.0f      << "%, "
                << "목표달성: "  << (result.goal_achieved ? "Y" : "N") << "\n";
            OutputDebugStringA(msg.str().c_str());
        } else {
            std::ostringstream msg;
            msg << "[StudySync] 세션 종료 실패 (HTTP " << result.success << ") " << result.message << "\n";
            OutputDebugStringA(msg.str().c_str());
        }
        session_id_ = 0;
    }

    // ── 타이머 정리 + 마지막 분석 데이터 flush ───────────────
    KillTimer(IDT_LOG_FLUSH);
    KillTimer(IDT_CALIB);
    KillTimer(IDT_CALIB_HIDE);
    KillTimer(IDT_STATS_FETCH);
    if (transports_.log_sink) {
        transports_.log_sink->flush();  // 미전송 데이터 마지막으로 전송
    }

    // ── 스레드 종료 (시작 역순) ──────────────────────────────
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
        render_thread_.set_calibration_countdown(-1);  // 오버레이 숨김, 세션 시작

    } else if (nIDEvent == IDT_LOG_FLUSH) {
        // 10초마다 누적된 분석 데이터를 서버로 전송
        if (transports_.log_sink && session_id_ > 0) {
            transports_.log_sink->flush();
        }
    } else if (nIDEvent == IDT_STATS_FETCH) {
        request_server_stats();
    }

    CWnd::OnTimer(nIDEvent);
}

void CStudySyncClientView::OnSize(UINT nType, int cx, int cy)
{
    CWnd::OnSize(nType, cx, cy);
    if (cx > 0 && cy > 0) {
        render_thread_.notify_resize(static_cast<UINT>(cx), static_cast<UINT>(cy));
    }
}
