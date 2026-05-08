#pragma once

class CMainFrame;

class CHomePanel : public CWnd {
public:
    explicit CHomePanel(CMainFrame* frame) noexcept;

protected:
    afx_msg int  OnCreate(LPCREATESTRUCT lp);
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnPaint();
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnBnClickedStart();
    DECLARE_MESSAGE_MAP()

private:
    CMainFrame* frame_;
    CFont       font_btn_;
    CButton     btn_start_;
};
