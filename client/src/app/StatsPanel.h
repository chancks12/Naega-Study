#pragma once

class CStatsPanel : public CWnd {
public:
    CStatsPanel() = default;

protected:
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnPaint();
    DECLARE_MESSAGE_MAP()
};
