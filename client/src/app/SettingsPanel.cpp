#include "pch.h"
#include "SettingsPanel.h"
#include "FpsDialog.h"
#include "MainFrm.h"
#include "resource.h"
#include "network/AuthApi.h"
#include "network/FpsStore.h"
#include "network/TokenStore.h"
#include "network/WinHttpClient.h"

BEGIN_MESSAGE_MAP(CSettingsPanel, CWnd)
    ON_WM_CREATE()
    ON_WM_ERASEBKGND()
    ON_WM_PAINT()
    ON_WM_SIZE()
    ON_BN_CLICKED(IDC_BTN_FPS_SETTING, OnBnClickedFpsSetting)
    ON_BN_CLICKED(IDC_BTN_LOGOUT,      OnBnClickedLogout)
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

    btn_fps_.Create(_T("FPS 설정"),
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                    CRect(0, 0, 160, 44), this, IDC_BTN_FPS_SETTING);
    btn_fps_.SetFont(&font_btn_);

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
    label_rc.bottom = label_rc.top + label_rc.Height() / 2 - 110;
    dc.DrawText(_T("설정"), &label_rc, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);

    // 현재 FPS 표시
    const int current_fps = FpsStore{}.load();
    CString fps_label;
    fps_label.Format(_T("현재 카메라 FPS: %d"), current_fps);

    CFont small_font;
    small_font.CreateFont(
        13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
    dc.SelectObject(&small_font);
    dc.SetTextColor(RGB(140, 140, 170));

    CRect fps_label_rc(0, rc.Height() / 2 - 60, rc.Width(), rc.Height() / 2 - 40);
    dc.DrawText(fps_label, &fps_label_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    dc.SelectObject(old);
}

void CSettingsPanel::OnSize(UINT, int cx, int cy)
{
    constexpr int bw = 160, bh = 44, gap = 12;
    const int base_y = cy / 2;

    if (btn_fps_.GetSafeHwnd()) {
        btn_fps_.SetWindowPos(nullptr,
                              (cx - bw) / 2, base_y,
                              bw, bh, SWP_NOZORDER);
    }
    if (btn_logout_.GetSafeHwnd()) {
        btn_logout_.SetWindowPos(nullptr,
                                 (cx - bw) / 2, base_y + bh + gap,
                                 bw, bh, SWP_NOZORDER);
    }
}

void CSettingsPanel::OnBnClickedFpsSetting()
{
    const int current_fps = FpsStore{}.load();
    CFpsDialog dlg(current_fps, this);

    if (dlg.DoModal() == IDOK) {
        const int new_fps = dlg.get_fps();
        FpsStore{}.save(new_fps);
        frame_->update_camera_fps(new_fps);
        Invalidate(); // FPS 표시 라벨 갱신
    }
}

void CSettingsPanel::OnBnClickedLogout()
{
    AuthApi(WinHttpClient::instance()).logout();
    TokenStore{}.clear();
    WinHttpClient::instance().clear_token();
    GetParentFrame()->PostMessage(WM_CLOSE);
}
