#pragma once

class CClipsPanel : public CWnd {
public:
    CClipsPanel() = default;

protected:
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnPaint();
    DECLARE_MESSAGE_MAP()
};
