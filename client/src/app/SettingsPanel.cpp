#include "pch.h"
#include "SettingsPanel.h"
#include "MainFrm.h"
#include "resource.h"
#include "network/AuthApi.h"
#include "network/TokenStore.h"
#include "network/WinHttpClient.h"

BEGIN_MESSAGE_MAP(CSettingsPanel, CWnd)
    ON_WM_CREATE()
    ON_WM_ERASEBKGND()
    ON_WM_PAINT()
    ON_WM_SIZE()
    ON_BN_CLICKED(IDC_BTN_LOGOUT, OnBnClickedLogout)
END_MESSAGE_MAP()

CSettingsPanel::CSettingsPanel(CMainFrame* frame) noexcept
    : frame_(frame)
{
}

int CSettingsPanel::OnCreate(LPCREATESTRUCT lp)
{
    if (CWnd::OnCreate(lp) == -1) return -1;

    font_btn_.CreateFont(
        16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

    btn_logout_.Create(_T("로그아웃"),
                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                       CRect(0, 0, 160, 44), this, IDC_BTN_LOGOUT);
    btn_logout_.SetFont(&font_btn_);

    return 0;
}

BOOL CSettingsPanel::OnEraseBkgnd(CDC*) { return TRUE; }

void CSettingsPanel::OnPaint()
{
    CPaintDC dc(this);
    CRect rc;
    GetClientRect(&rc);

    dc.FillSolidRect(&rc, RGB(22, 22, 34));

    CFont font;
    font.CreateFont(
        18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

    CFont* old = dc.SelectObject(&font);
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(RGB(200, 200, 220));

    CRect label_rc;
    GetClientRect(&label_rc);
    label_rc.bottom = label_rc.top + label_rc.Height() / 2 - 60;
    dc.DrawText(_T("설정"), &label_rc, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);

    dc.SelectObject(old);
}

void CSettingsPanel::OnSize(UINT, int cx, int cy)
{
    if (btn_logout_.GetSafeHwnd()) {
        constexpr int bw = 160, bh = 44;
        btn_logout_.SetWindowPos(nullptr,
                                 (cx - bw) / 2, cy / 2,
                                 bw, bh, SWP_NOZORDER);
    }
}

void CSettingsPanel::OnBnClickedLogout()
{
    // 토큰 삭제 후 앱 종료 (재실행 시 로그인 화면)
    AuthApi(WinHttpClient::instance()).logout();
    TokenStore{}.clear();
    WinHttpClient::instance().clear_token();
    GetParentFrame()->PostMessage(WM_CLOSE);
}
