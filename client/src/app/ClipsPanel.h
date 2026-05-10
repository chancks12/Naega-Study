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
    afx_msg void OnShowWindow(BOOL bShow, UINT nStatus);
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    DECLARE_MESSAGE_MAP()

private:
    struct SessionEntry {
        std::wstring name;
        int          clip_count = 0;
        std::wstring full_path; // 탐색기에서 열 절대 경로
    };

    void scan_clips();
    int  hit_test(CPoint pt) const; // 클릭 좌표 → 세션 인덱스 (-1 = 없음)

    std::vector<SessionEntry> sessions_;
    int scroll_pos_ = 0;

    static constexpr int kRowH    = 56;
    static constexpr int kHeaderH = 56;
};
