#include "pch.h"
#include "StatsPanel.h"

BEGIN_MESSAGE_MAP(CStatsPanel, CWnd)
    ON_WM_ERASEBKGND()
    ON_WM_PAINT()
END_MESSAGE_MAP()

BOOL CStatsPanel::OnEraseBkgnd(CDC*) { return TRUE; }

void CStatsPanel::OnPaint()
{
    CPaintDC dc(this);
    CRect rc;
    GetClientRect(&rc);

    dc.FillSolidRect(&rc, RGB(22, 22, 34));

    CFont font;
    font.CreateFont(
        16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

    CFont* old = dc.SelectObject(&font);
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(RGB(120, 120, 140));
    dc.DrawText(_T("학습 상태 (준비 중)"), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    dc.SelectObject(old);
}
