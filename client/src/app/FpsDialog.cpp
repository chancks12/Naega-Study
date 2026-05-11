#include "pch.h"
#include "FpsDialog.h"
#include "resource.h"

BEGIN_MESSAGE_MAP(CFpsDialog, CDialog)
END_MESSAGE_MAP()

CFpsDialog::CFpsDialog(int current_fps, CWnd* parent)
    : CDialog(IDD_FPS, parent)
    , fps_(current_fps)
{
}

BOOL CFpsDialog::OnInitDialog()
{
    CDialog::OnInitDialog();

    edit_fps_.SubclassDlgItem(IDC_EDIT_FPS, this);

    CString text;
    text.Format(_T("%d"), fps_);
    edit_fps_.SetWindowText(text);
    edit_fps_.SetSel(0, -1);
    edit_fps_.SetFocus();

    return FALSE; // SetFocus 직접 호출했으므로 FALSE
}

void CFpsDialog::OnOK()
{
    CString text;
    edit_fps_.GetWindowText(text);

    const int val = _ttoi(text);
    if (val < 1 || val > 120) {
        MessageBox(_T("FPS는 1 ~ 120 사이 값이어야 합니다."),
                   _T("입력 오류"), MB_OK | MB_ICONWARNING);
        edit_fps_.SetSel(0, -1);
        edit_fps_.SetFocus();
        return;
    }

    fps_ = val;
    CDialog::OnOK();
}
