#include "pch.h"
#include "HomePanel.h"
#include "MainFrm.h"
#include "resource.h"

BEGIN_MESSAGE_MAP(CHomePanel, CWnd)
    ON_WM_CREATE()
    ON_WM_ERASEBKGND()
    ON_WM_PAINT()
    ON_WM_SIZE()
    ON_BN_CLICKED(IDC_BTN_START_CAPTURE, OnBnClickedStart)
END_MESSAGE_MAP()

CHomePanel::CHomePanel(CMainFrame* frame) noexcept
    : frame_(frame)
{
}

int CHomePanel::OnCreate(LPCREATESTRUCT lp)
{
    if (CWnd::OnCreate(lp) == -1) return -1;

    font_btn_.CreateFont(
        18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

    btn_start_.Create(_T("분석 시작하기"),
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                      CRect(0, 0, 200, 52), this, IDC_BTN_START_CAPTURE);
    btn_start_.SetFont(&font_btn_);

    return 0;
}

BOOL CHomePanel::OnEraseBkgnd(CDC*) { return TRUE; }

void CHomePanel::OnPaint()
{
    CPaintDC dc(this);
    CRect rc;
    GetClientRect(&rc);

    dc.FillSolidRect(&rc, RGB(22, 22, 34));

    // 앱 제목
    CFont title_font;
    title_font.CreateFont(
        28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

    CFont* old = dc.SelectObject(&title_font);
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(RGB(220, 220, 240));

    CRect title_rc(rc.left, rc.top + rc.Height() / 2 - 100,
                   rc.right, rc.top + rc.Height() / 2 - 60);
    dc.DrawText(_T("StudySync"), &title_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    CFont sub_font;
    sub_font.CreateFont(
        14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
    dc.SelectObject(&sub_font);
    dc.SetTextColor(RGB(140, 140, 160));

    CRect sub_rc(rc.left, title_rc.bottom + 8, rc.right, title_rc.bottom + 32);
    dc.DrawText(_T("AI 기반 학습 자세 분석"), &sub_rc, DT_CENTER | DT_SINGLELINE);

    dc.SelectObject(old);
}

void CHomePanel::OnSize(UINT, int cx, int cy)
{
    if (btn_start_.GetSafeHwnd()) {
        constexpr int bw = 200, bh = 52;
        btn_start_.SetWindowPos(nullptr,
                                (cx - bw) / 2, cy / 2 - bh / 2 + 20,
                                bw, bh, SWP_NOZORDER);
    }
}

void CHomePanel::OnBnClickedStart()
{
    frame_->start_capture();
}
