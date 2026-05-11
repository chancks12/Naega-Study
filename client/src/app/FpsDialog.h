#pragma once

#include <afxwin.h>

// 카메라 FPS 입력 다이얼로그.
// DoModal() 후 get_fps()로 사용자가 입력한 값을 읽는다.
class CFpsDialog : public CDialog {
public:
    explicit CFpsDialog(int current_fps, CWnd* parent = nullptr);

    int get_fps() const { return fps_; }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    DECLARE_MESSAGE_MAP()

private:
    int     fps_;
    CEdit   edit_fps_;
};
