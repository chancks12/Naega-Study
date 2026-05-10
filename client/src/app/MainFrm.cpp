#include "pch.h"
#include "MainFrm.h"
#include "HomePanel.h"
#include "ClipsPanel.h"
#include "StatsPanel.h"
#include "SettingsPanel.h"
#include "ReviewDlg.h"
#include "StudySyncClientView.h"
#include "network/ClientTransportConfig.h"
#include "network/SessionApi.h"
#include "network/WinHttpClient.h"
#include "resource.h"

#include <sstream>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CMainFrame, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_CLOSE()
    ON_WM_SIZE()
    ON_NOTIFY(TCN_SELCHANGE, IDC_TAB_MAIN, OnTabSelChange)
    ON_BN_CLICKED(IDC_BTN_STOP_CAPTURE, OnBnClickedStop)
    ON_MESSAGE(WM_STOP_CAPTURE, OnStopCaptureMsg)
    ON_MESSAGE(WM_SESSION_STARTED, OnSessionStarted)
END_MESSAGE_MAP()

static constexpr int kTabH = 28;

CMainFrame::CMainFrame(int user_id) noexcept
    : user_id_(user_id)
{
}

CMainFrame::~CMainFrame()
{
    for (auto* p : panels_) delete p;
    delete capture_view_;
}

// ── 생성 ──────────────────────────────────────────────────────

int CMainFrame::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
    if (CFrameWnd::OnCreate(lpCreateStruct) == -1) return -1;

    CRect rc;
    GetClientRect(&rc);

    // ── 탭 컨트롤 ─────────────────────────────────────────────
    tab_ctrl_.Create(
        TCS_FIXEDWIDTH | WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        CRect(0, 0, rc.Width(), kTabH), this, IDC_TAB_MAIN);

    TCITEM ti{};
    ti.mask = TCIF_TEXT;
    auto insert = [&](int i, const wchar_t* text) {
        ti.pszText = const_cast<wchar_t*>(text);
        tab_ctrl_.InsertItem(i, &ti);
    };
    insert(TAB_HOME,     L"메인");
    insert(TAB_CLIPS,    L"클립 확인");
    insert(TAB_STATS,    L"학습 상태");
    insert(TAB_SETTINGS, L"설정");

    // ── 패널 생성 ─────────────────────────────────────────────
    const CString cls = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW);
    const CRect panel_rc(0, kTabH, rc.Width(), rc.Height());

    auto* home = new CHomePanel(this);
    home->Create(cls, nullptr,
                 WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
                 panel_rc, this, 0);
    panels_[TAB_HOME] = home;

    auto* clips = new CClipsPanel();
    clips->Create(cls, nullptr,
                  WS_CHILD | WS_CLIPSIBLINGS,
                  panel_rc, this, 0);
    panels_[TAB_CLIPS] = clips;

    auto* stats = new CStatsPanel();
    stats->Create(cls, nullptr,
                  WS_CHILD | WS_CLIPSIBLINGS,
                  panel_rc, this, 0);
    panels_[TAB_STATS] = stats;

    auto* settings = new CSettingsPanel(this);
    settings->Create(cls, nullptr,
                     WS_CHILD | WS_CLIPSIBLINGS,
                     panel_rc, this, 0);
    panels_[TAB_SETTINGS] = settings;

    active_tab_ = TAB_HOME;
    return 0;
}

// ── 탭 전환 ─────────────────────────────────────────────────

void CMainFrame::OnTabSelChange(NMHDR* /*pNMHDR*/, LRESULT* pResult)
{
    if (!capturing_) {
        show_tab(tab_ctrl_.GetCurSel());
    }
    *pResult = 0;
}

void CMainFrame::show_tab(int index)
{
    if (index < 0 || index >= TAB_COUNT) return;
    if (panels_[active_tab_]) panels_[active_tab_]->ShowWindow(SW_HIDE);
    active_tab_ = index;
    if (panels_[active_tab_]) panels_[active_tab_]->ShowWindow(SW_SHOW);
    tab_ctrl_.SetCurSel(active_tab_);
}

// ── 레이아웃 ────────────────────────────────────────────────

void CMainFrame::layout_content(int cx, int cy)
{
    if (capturing_ && capture_view_ && capture_view_->GetSafeHwnd()) {
        capture_view_->SetWindowPos(nullptr, 0, 0, cx, cy,
                                    SWP_NOZORDER | SWP_NOACTIVATE);

        constexpr int bw = 120, bh = 36, margin = 16;
        if (btn_stop_.GetSafeHwnd()) {
            btn_stop_.SetWindowPos(&CWnd::wndTop,
                                   cx - bw - margin, cy - bh - margin,
                                   bw, bh, SWP_NOACTIVATE);
        }
        return;
    }

    if (tab_ctrl_.GetSafeHwnd()) {
        tab_ctrl_.SetWindowPos(nullptr, 0, 0, cx, kTabH,
                               SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (int i = 0; i < TAB_COUNT; ++i) {
        if (panels_[i] && panels_[i]->GetSafeHwnd()) {
            panels_[i]->SetWindowPos(nullptr, 0, kTabH, cx, cy - kTabH,
                                     SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

void CMainFrame::OnSize(UINT nType, int cx, int cy)
{
    CFrameWnd::OnSize(nType, cx, cy);
    if (cx > 0 && cy > 0) layout_content(cx, cy);
}

// ── 캡처 시작 ───────────────────────────────────────────────

void CMainFrame::start_capture()
{
    if (capturing_) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char iso[32];
    snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02d+09:00",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    // 탭 UI 숨기기
    tab_ctrl_.ShowWindow(SW_HIDE);
    for (auto* p : panels_) if (p) p->ShowWindow(SW_HIDE);

    // 캡처 뷰 생성 (전체화면) — 세션 ID 없이 먼저 시작
    CRect rc;
    GetClientRect(&rc);
    const CString cls = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW);
    capture_view_ = new CStudySyncClientView({});
    capture_view_->Create(cls, nullptr,
                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                          rc, this, AFX_IDW_PANE_FIRST);

    // 학습 종료 버튼
    font_stop_.CreateFont(
        15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

    constexpr int bw = 120, bh = 36, margin = 16;
    btn_stop_.Create(_T("학습 종료"),
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT | WS_CLIPSIBLINGS,
                     CRect(rc.Width() - bw - margin, rc.Height() - bh - margin, rc.Width() - margin, rc.Height() - margin),
                     this, IDC_BTN_STOP_CAPTURE);
    btn_stop_.SetFont(&font_stop_);

    // 캘리브레이션은 session_id=0으로라도 반드시 시작 (알림 가드 보장)
    capture_view_->set_session_id(0, iso);

    capturing_ = true;

    // 세션 API는 백그라운드에서 호출 — 성공 시 session_id 주입
    const std::string iso_str = iso;
    const HWND view_hwnd = capture_view_->GetSafeHwnd();
    std::thread([this, iso_str, view_hwnd] {
        SessionApi session_api(WinHttpClient::instance());
        const SessionStartResult result = session_api.start(iso_str.c_str());
        if (result.success && result.session_id > 0) {
            // UI 스레드에서 session_id 갱신
            PostMessage(WM_SESSION_STARTED,
                        static_cast<WPARAM>(result.session_id), 0);
        } else {
            OutputDebugStringA("[StudySync] session/start failed — running offline\n");
        }
    }).detach();
}

// ── 캡처 종료 ───────────────────────────────────────────────

void CMainFrame::stop_capture()
{
    if (!capturing_ || !capture_view_) return;

    // 복기 이벤트 확인 (뷰 종료 전에)
    {
        ReviewEventStore& store = capture_view_->review_store();
        if (store.count() > 0) {
            ReviewDlg dlg(store, capture_view_->session_id(), this);
            dlg.DoModal();
        }
    }

    // 학습 종료 버튼 제거
    if (btn_stop_.GetSafeHwnd()) {
        btn_stop_.DestroyWindow();
        font_stop_.DeleteObject();
    }

    // capturing_ 먼저 해제 + 탭 UI 복원 → DestroyWindow 스레드 join 전에 화면 전환
    capturing_ = false;
    tab_ctrl_.ShowWindow(SW_SHOW);
    if (panels_[active_tab_]) panels_[active_tab_]->ShowWindow(SW_SHOW);
    {
        CRect rc;
        GetClientRect(&rc);
        layout_content(rc.Width(), rc.Height());
    }
    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    // 캡처 뷰 종료 (OnDestroy → 세션 종료 API + 스레드 정리)
    capture_view_->DestroyWindow();
    delete capture_view_;
    capture_view_ = nullptr;
}

// ── 버튼 핸들러 ─────────────────────────────────────────────

void CMainFrame::OnBnClickedStop()
{
    // PostMessage: 버튼 핸들러 반환 후 처리 — 동기 호출 시 스레드 join이 펌프를 블로킹함
    btn_stop_.EnableWindow(FALSE);
    PostMessage(WM_STOP_CAPTURE);
}

LRESULT CMainFrame::OnStopCaptureMsg(WPARAM, LPARAM)
{
    stop_capture();
    return 0;
}

// 세션 API 응답 도착 → capture_view_에 session_id 갱신
LRESULT CMainFrame::OnSessionStarted(WPARAM wParam, LPARAM)
{
    if (capturing_ && capture_view_) {
        const long long session_id = static_cast<long long>(wParam);
        capture_view_->update_session_id(session_id);

        // 세션별 클립 폴더 경로도 갱신
        capture_view_->set_clip_directory("event_clips/" + std::to_string(session_id));
    }
    return 0;
}

// ── 종료 ────────────────────────────────────────────────────

void CMainFrame::OnClose()
{
    if (capturing_) {
        stop_capture();
    }
    CFrameWnd::OnClose();
}
