#pragma once

#include <string>
#include <vector>

class CClipsPanel : public CWnd {
public:
    CClipsPanel() = default;

protected:
    afx_msg int  OnCreate(LPCREATESTRUCT lp);
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnPaint();
    afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pBar);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    DECLARE_MESSAGE_MAP()

private:
    struct SessionEntry {
        std::wstring name;
        int          clip_count = 0;
    };

    void scan_clips();

    std::vector<SessionEntry> sessions_;
    int scroll_pos_ = 0;

    static constexpr int kRowH  = 56;
    static constexpr int kHeaderH = 56;
};
