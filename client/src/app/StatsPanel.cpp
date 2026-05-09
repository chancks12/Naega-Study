#include "pch.h"
#include "StatsPanel.h"
#include "network/StatsApi.h"
#include "network/WinHttpClient.h"

#include <thread>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CStatsPanel, CWnd)
    ON_WM_CREATE()
    ON_WM_ERASEBKGND()
    ON_WM_PAINT()
    ON_WM_TIMER()
    ON_MESSAGE(WM_STATS_READY, OnStatsReady)
END_MESSAGE_MAP()

static constexpr COLORREF kBg     = RGB(22, 22, 34);
static constexpr COLORREF kCardBg = RGB(32, 32, 50);

int CStatsPanel::OnCreate(LPCREATESTRUCT lp)
{
    if (CWnd::OnCreate(lp) == -1) return -1;
    fetch_async();
    SetTimer(IDT_REFRESH, 60'000, nullptr);
    return 0;
}

BOOL CStatsPanel::OnEraseBkgnd(CDC*) { return TRUE; }

void CStatsPanel::OnPaint()
{
    CPaintDC dc(this);
    CRect rc;
    GetClientRect(&rc);
    dc.FillSolidRect(&rc, kBg);
    dc.SetBkMode(TRANSPARENT);

    // ── 제목 ──────────────────────────────────────────────────
    CFont title_font;
    title_font.CreateFont(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
    CFont* old = dc.SelectObject(&title_font);
    dc.SetTextColor(RGB(200, 200, 220));
    CRect title_rc(rc.left + 24, rc.top + 20, rc.right - 24, rc.top + 56);
    dc.DrawText(_T("오늘의 학습 통계"), &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    dc.SelectObject(old);

    // ── 데이터 없을 때 ────────────────────────────────────────
    const ServerStatsSnapshot::Data data = stats_.snapshot();
    if (!data.valid) {
        CFont font;
        font.CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
        old = dc.SelectObject(&font);
        dc.SetTextColor(RGB(100, 100, 120));
        CRect msg_rc(rc.left, rc.top + 80, rc.right, rc.bottom);
        dc.DrawText(_T("데이터를 불러오는 중..."), &msg_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);
        dc.SelectObject(old);
        return;
    }

    // ── 4개 통계 카드 (2×2 그리드) ──────────────────────────
    constexpr int margin = 24;
    constexpr int gap    = 12;
    const int card_w = (rc.Width() - margin * 2 - gap) / 2;
    const int card_h = 110;
    const int top    = 68;

    wchar_t val[64];

    swprintf_s(val, L"%d분", data.focus_min);
    draw_card(dc, CRect(margin,              top,              margin + card_w,      top + card_h),
              L"집중 시간", val, RGB(80, 200, 120));

    swprintf_s(val, L"%.0f%%", data.avg_focus * 100.0);
    draw_card(dc, CRect(margin + card_w + gap, top,            rc.right - margin,    top + card_h),
              L"평균 집중도", val, RGB(80, 150, 240));

    swprintf_s(val, L"%d분", data.break_min);
    draw_card(dc, CRect(margin,              top + card_h + gap, margin + card_w,    top + card_h * 2 + gap),
              L"휴식 시간", val, RGB(240, 180, 60));

    swprintf_s(val, L"%d회", data.warning_count);
    draw_card(dc, CRect(margin + card_w + gap, top + card_h + gap, rc.right - margin, top + card_h * 2 + gap),
              L"경고 횟수", val, RGB(240, 90, 90));
}

void CStatsPanel::draw_card(CDC& dc, const CRect& rc,
                             const wchar_t* label, const wchar_t* value,
                             COLORREF accent)
{
    dc.FillSolidRect(&rc, kCardBg);
    dc.FillSolidRect(CRect(rc.left, rc.top, rc.left + 4, rc.bottom), accent);

    CFont lf;
    lf.CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
    CFont* old = dc.SelectObject(&lf);
    dc.SetTextColor(RGB(130, 130, 155));
    CRect lr(rc.left + 16, rc.top + 14, rc.right - 8, rc.top + 36);
    dc.DrawText(label, &lr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    CFont vf;
    vf.CreateFont(30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
    dc.SelectObject(&vf);
    dc.SetTextColor(RGB(215, 215, 235));
    CRect vr(rc.left + 16, rc.top + 38, rc.right - 8, rc.bottom - 10);
    dc.DrawText(value, &vr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    dc.SelectObject(old);
}

void CStatsPanel::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == IDT_REFRESH) fetch_async();
    CWnd::OnTimer(nIDEvent);
}

LRESULT CStatsPanel::OnStatsReady(WPARAM, LPARAM)
{
    Invalidate();
    return 0;
}

void CStatsPanel::fetch_async()
{
    if (fetching_.exchange(true)) return;

    const HWND hwnd = GetSafeHwnd();
    std::thread([this, hwnd] {
        StatsApi api(WinHttpClient::instance());
        const ServerStatsSnapshot::Data data = api.today();
        if (data.valid) stats_.update(data);
        fetching_ = false;
        if (::IsWindow(hwnd))
            ::PostMessage(hwnd, WM_STATS_READY, 0, 0);
    }).detach();
}
