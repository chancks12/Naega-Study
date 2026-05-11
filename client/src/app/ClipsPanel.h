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
    struct ClipEntry {
        std::wstring label;    // "14:32 · 졸음"
        std::wstring mp4_path; // clip.mp4 절대 경로
    };

    struct SessionEntry {
        std::wstring         display_name; // "2025-05-11 14:30:22"
        std::vector<ClipEntry> clips;
    };

    void scan_clips();
    int  total_rows_height() const;
    // 클릭 좌표 → clip (nullptr = 세션 헤더 또는 빈 영역)
    const ClipEntry* hit_test_clip(CPoint pt) const;

    std::vector<SessionEntry> sessions_;
    int scroll_pos_ = 0;

    static constexpr int kHeaderH    = 56; // 패널 제목 높이
    static constexpr int kSessionRowH = 48; // 세션 행 높이
    static constexpr int kClipRowH    = 40; // 클립 행 높이
};
