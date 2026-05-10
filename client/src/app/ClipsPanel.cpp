#include "pch.h"
#include "ClipsPanel.h"

#include <filesystem>
#include <algorithm>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace fs = std::filesystem;

BEGIN_MESSAGE_MAP(CClipsPanel, CWnd)
    ON_WM_CREATE()
    ON_WM_ERASEBKGND()
    ON_WM_PAINT()
    ON_WM_VSCROLL()
    ON_WM_MOUSEWHEEL()
    ON_WM_SHOWWINDOW()
    ON_WM_LBUTTONDOWN()
END_MESSAGE_MAP()

static constexpr COLORREF kBg      = RGB(22, 22, 34);
static constexpr COLORREF kRowBg   = RGB(30, 30, 46);
static constexpr COLORREF kRowAlt  = RGB(26, 26, 40);
static constexpr COLORREF kAccent  = RGB(80, 150, 240);

int CClipsPanel::OnCreate(LPCREATESTRUCT lp)
{
    if (CWnd::OnCreate(lp) == -1) return -1;
    scan_clips();
    return 0;
}

BOOL CClipsPanel::OnEraseBkgnd(CDC*) { return TRUE; }

void CClipsPanel::scan_clips()
{
    sessions_.clear();
    scroll_pos_ = 0;

    const fs::path base = fs::path(".") / "event_clips";
    std::error_code ec;
    if (!fs::exists(base, ec)) return;

    for (const auto& entry : fs::directory_iterator(base, ec)) {
        if (!entry.is_directory()) continue;

        SessionEntry se;
        se.name      = entry.path().filename().wstring();
        se.full_path = fs::absolute(entry.path(), ec).wstring();

        for (const auto& ev_entry : fs::directory_iterator(entry.path(), ec)) {
            if (!ev_entry.is_directory()) continue;
            const fs::path clip = ev_entry.path() / L"clip.mp4";
            if (fs::exists(clip, ec)) ++se.clip_count;
        }

        sessions_.push_back(std::move(se));
    }

    // 최신 세션(폴더 이름 내림차순) 먼저
    std::sort(sessions_.begin(), sessions_.end(),
              [](const SessionEntry& a, const SessionEntry& b) {
                  return a.name > b.name;
              });

    // 스크롤바 설정
    const int total_h = static_cast<int>(sessions_.size()) * kRowH;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_POS | SIF_PAGE;
    si.nMin   = 0;
    si.nMax   = total_h;
    CRect rc;
    GetClientRect(&rc);
    si.nPage  = rc.Height() - kHeaderH;
    si.nPos   = 0;
    SetScrollInfo(SB_VERT, &si, TRUE);
}

void CClipsPanel::OnPaint()
{
    CPaintDC dc(this);
    CRect rc;
    GetClientRect(&rc);
    dc.FillSolidRect(&rc, kBg);
    dc.SetBkMode(TRANSPARENT);

    // ── 제목 ─────────────────────────────────────────────────
    CFont title_font;
    title_font.CreateFont(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
    CFont* old = dc.SelectObject(&title_font);
    dc.SetTextColor(RGB(200, 200, 220));
    CRect title_rc(rc.left + 24, rc.top + 14, rc.right - 24, rc.top + kHeaderH);
    dc.DrawText(_T("클립 확인"), &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    dc.SelectObject(old);

    // ── 클립 없을 때 ─────────────────────────────────────────
    if (sessions_.empty()) {
        CFont font;
        font.CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
        old = dc.SelectObject(&font);
        dc.SetTextColor(RGB(100, 100, 120));
        CRect msg_rc(rc.left, rc.top + kHeaderH, rc.right, rc.bottom);
        dc.DrawText(_T("저장된 클립이 없습니다"), &msg_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);
        dc.SelectObject(old);
        return;
    }

    // ── 행 목록 ──────────────────────────────────────────────
    CFont row_font;
    row_font.CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
    CFont sub_font;
    sub_font.CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

    dc.IntersectClipRect(rc.left, rc.top + kHeaderH, rc.right, rc.bottom);

    for (int i = 0; i < static_cast<int>(sessions_.size()); ++i) {
        const int y = kHeaderH + i * kRowH - scroll_pos_;
        if (y + kRowH < kHeaderH) continue;
        if (y > rc.bottom) break;

        const COLORREF bg = (i % 2 == 0) ? kRowBg : kRowAlt;
        dc.FillSolidRect(CRect(rc.left, y, rc.right, y + kRowH), bg);

        // accent bar
        dc.FillSolidRect(CRect(rc.left, y, rc.left + 3, y + kRowH), kAccent);

        const auto& se = sessions_[i];

        old = dc.SelectObject(&row_font);
        dc.SetTextColor(RGB(210, 210, 230));
        CRect name_rc(rc.left + 20, y + 8, rc.right - 80, y + kRowH / 2 + 2);
        dc.DrawText(se.name.c_str(), static_cast<int>(se.name.size()),
                    &name_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        dc.SelectObject(&sub_font);
        dc.SetTextColor(RGB(110, 110, 140));
        CRect sub_rc(rc.left + 20, y + kRowH / 2 + 2, rc.right - 80, y + kRowH - 6);
        CString sub;
        sub.Format(_T("클립 %d개"), se.clip_count);
        dc.DrawText(sub, &sub_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        dc.SelectObject(old);
    }
}

void CClipsPanel::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* /*pBar*/)
{
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_ALL;
    GetScrollInfo(SB_VERT, &si);

    int new_pos = si.nPos;
    switch (nSBCode) {
    case SB_TOP:          new_pos = si.nMin; break;
    case SB_BOTTOM:       new_pos = si.nMax; break;
    case SB_LINEUP:       new_pos -= kRowH; break;
    case SB_LINEDOWN:     new_pos += kRowH; break;
    case SB_PAGEUP:       new_pos -= si.nPage; break;
    case SB_PAGEDOWN:     new_pos += si.nPage; break;
    case SB_THUMBTRACK:   new_pos = static_cast<int>(nPos); break;
    default: return;
    }

    new_pos = max(si.nMin, min(new_pos, static_cast<int>(si.nMax - si.nPage)));
    if (new_pos == scroll_pos_) return;
    scroll_pos_ = new_pos;
    SetScrollPos(SB_VERT, scroll_pos_, TRUE);
    Invalidate();
}

BOOL CClipsPanel::OnMouseWheel(UINT /*nFlags*/, short zDelta, CPoint /*pt*/)
{
    const int delta = (zDelta > 0 ? -kRowH : kRowH);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_ALL;
    GetScrollInfo(SB_VERT, &si);
    const int new_pos = max(si.nMin, min(scroll_pos_ + delta,
                                          static_cast<int>(si.nMax - si.nPage)));
    if (new_pos == scroll_pos_) return TRUE;
    scroll_pos_ = new_pos;
    SetScrollPos(SB_VERT, scroll_pos_, TRUE);
    Invalidate();
    return TRUE;
}

// 탭이 표시될 때마다 클립 목록 갱신 (세션 종료 후 새 클립 반영)
void CClipsPanel::OnShowWindow(BOOL bShow, UINT /*nStatus*/)
{
    if (bShow) {
        scan_clips();
        Invalidate();
    }
}

// 행 클릭 → Windows 탐색기로 세션 폴더 열기
void CClipsPanel::OnLButtonDown(UINT /*nFlags*/, CPoint point)
{
    const int idx = hit_test(point);
    if (idx < 0) return;

    const std::wstring& path = sessions_[static_cast<std::size_t>(idx)].full_path;
    if (!path.empty()) {
        ShellExecuteW(nullptr, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

int CClipsPanel::hit_test(CPoint pt) const
{
    if (pt.y < kHeaderH) return -1;

    const int idx = (pt.y - kHeaderH + scroll_pos_) / kRowH;
    if (idx < 0 || idx >= static_cast<int>(sessions_.size())) return -1;
    return idx;
}
