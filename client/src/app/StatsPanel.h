#pragma once

#include "model/ServerStatsSnapshot.h"
#include <atomic>

class CStatsPanel : public CWnd {
public:
    CStatsPanel() = default;

protected:
    afx_msg int     OnCreate(LPCREATESTRUCT lp);
    afx_msg BOOL    OnEraseBkgnd(CDC* pDC);
    afx_msg void    OnPaint();
    afx_msg void    OnTimer(UINT_PTR nIDEvent);
    afx_msg LRESULT OnStatsReady(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()

private:
    void fetch_async();
    static void draw_card(CDC& dc, const CRect& rc,
                          const wchar_t* label, const wchar_t* value,
                          COLORREF accent);

    static constexpr UINT_PTR IDT_REFRESH  = 1;
    static constexpr UINT     WM_STATS_READY = WM_APP + 2;

    ServerStatsSnapshot  stats_;
    std::atomic_bool     fetching_{ false };
};
