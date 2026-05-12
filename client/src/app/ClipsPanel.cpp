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

static constexpr COLORREF kBg         = RGB(22, 22, 34);
static constexpr COLORREF kSessionBg  = RGB(34, 34, 52);
static constexpr COLORREF kClipBg     = RGB(28, 28, 44);
static constexpr COLORREF kClipAltBg  = RGB(24, 24, 38);
static constexpr COLORREF kAccent     = RGB(80, 150, 240);
static constexpr COLORREF kAccentSub  = RGB(60, 110, 200);

namespace {

// "20250511_143022" → L"2025-05-11 14:30:22"
std::wstring parse_session_name(const std::wstring& s)
{
    if (s.size() < 15) return s;
    return s.substr(0, 4) + L"-" + s.substr(4, 2) + L"-" + s.substr(6, 2)
         + L" " + s.substr(9, 2) + L":" + s.substr(11, 2) + L":" + s.substr(13, 2);
}

// "143205_drowsy" → L"14:32:05 · 졸음"
std::wstring parse_clip_label(const std::wstring& s)
{
    if (s.size() < 6) return s;
    const auto sep = s.find(L'_');
    const std::wstring state_en = (sep != std::wstring::npos) ? s.substr(sep + 1) : L"";

    std::wstring state_kr;
    if      (state_en == L"drowsy")     state_kr = L"졸음";
    else if (state_en == L"absent")     state_kr = L"자리 비움";
    else if (state_en == L"focus")      state_kr = L"공부 시작";
    else                                state_kr = L"집중력 저하";

    return s.substr(0, 2) + L":" + s.substr(2, 2) + L":" + s.substr(4, 2) + L" · " + state_kr;
}

} // namespace

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

    for (const auto& sess_entry : fs::directory_iterator(base, ec)) {
        if (!sess_entry.is_directory()) continue;

        SessionEntry se;
        const std::wstring dir_name = sess_entry.path().filename().wstring();
        se.display_name = parse_session_name(dir_name);

        std::vector<CClipsPanel::ClipEntry> clips;
        for (const auto& ev_entry : fs::directory_iterator(sess_entry.path(), ec)) {
            if (!ev_entry.is_directory()) continue;

            const fs::path mp4 = ev_entry.path() / L"clip.mp4";
            if (!fs::exists(mp4, ec)) continue;

            ClipEntry ce;
            ce.label    = parse_clip_label(ev_entry.path().filename().wstring());
            ce.mp4_path = fs::absolute(mp4, ec).wstring();
            clips.push_back(std::move(ce));
        }

        std::sort(clips.begin(), clips.end(),
                  [](const ClipEntry& a, const ClipEntry& b) {
                      return a.label < b.label;
                  });
        se.clips = std::move(clips);

        sessions_.push_back(std::move(se));
    }

    std::sort(sessions_.begin(), sessions_.end(),
              [](const SessionEntry& a, const SessionEntry& b) {
                  return a.display_name > b.display_name;
              });

    const int content_h = total_rows_height();
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_POS | SIF_PAGE;
    si.nMin   = 0;
    si.nMax   = content_h;
    CRect rc;
    GetClientRect(&rc);
    si.nPage = max(1, rc.Height() - kHeaderH);
    si.nPos  = 0;
    SetScrollInfo(SB_VERT, &si, TRUE);
}

int CClipsPanel::total_rows_height() const
{
    int h = 0;
    for (const auto& se : sessions_) {
        h += kSessionRowH;
        h += static_cast<int>(se.clips.size()) * kClipRowH;
    }
    return h;
}

void CClipsPanel::OnPaint()
{
    CPaintDC dc(this);
    CRect rc;
    GetClientRect(&rc);
    dc.FillSolidRect(&rc, kBg);
    dc.SetBkMode(TRANSPARENT);

    CFont title_font;
    title_font.CreateFont(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
    CFont* old = dc.SelectObject(&title_font);
    dc.SetTextColor(RGB(200, 200, 220));
    CRect title_rc(rc.left + 24, rc.top + 14, rc.right - 24, rc.top + kHeaderH);
    dc.DrawText(_T("클립 확인"), &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    dc.SelectObject(old);

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

    CFont sess_font;
    sess_font.CreateFont(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
    CFont clip_font;
    clip_font.CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

    dc.IntersectClipRect(rc.left, rc.top + kHeaderH, rc.right, rc.bottom);

    int y = kHeaderH - scroll_pos_;

    for (const auto& se : sessions_) {
        if (y + kSessionRowH >= kHeaderH && y <= rc.bottom) {
            dc.FillSolidRect(CRect(rc.left, y, rc.right, y + kSessionRowH), kSessionBg);
            dc.FillSolidRect(CRect(rc.left, y, rc.left + 4, y + kSessionRowH), kAccent);

            old = dc.SelectObject(&sess_font);
            dc.SetTextColor(RGB(200, 210, 240));
            CRect name_rc(rc.left + 20, y, rc.right - 60, y + kSessionRowH);
            dc.DrawText(se.display_name.c_str(),
                        static_cast<int>(se.display_name.size()),
                        &name_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            CFont badge_font;
            badge_font.CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
            dc.SelectObject(&badge_font);
            dc.SetTextColor(RGB(130, 160, 220));
            CRect badge_rc(rc.right - 60, y, rc.right - 8, y + kSessionRowH);
            CString badge;
            badge.Format(_T("%d개"), static_cast<int>(se.clips.size()));
            dc.DrawText(badge, &badge_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            dc.SelectObject(old);
        }
        y += kSessionRowH;

        for (int ci = 0; ci < static_cast<int>(se.clips.size()); ++ci) {
            if (y + kClipRowH >= kHeaderH && y <= rc.bottom) {
                const COLORREF bg = (ci % 2 == 0) ? kClipBg : kClipAltBg;
                dc.FillSolidRect(CRect(rc.left, y, rc.right, y + kClipRowH), bg);
                dc.FillSolidRect(CRect(rc.left + 16, y + 8, rc.left + 18, y + kClipRowH - 8), kAccentSub);

                old = dc.SelectObject(&clip_font);
                dc.SetTextColor(RGB(180, 190, 220));
                CRect label_rc(rc.left + 28, y, rc.right - 16, y + kClipRowH);
                dc.DrawText(se.clips[ci].label.c_str(),
                            static_cast<int>(se.clips[ci].label.size()),
                            &label_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                dc.SetTextColor(RGB(80, 140, 220));
                CRect play_rc(rc.right - 48, y, rc.right - 8, y + kClipRowH);
                dc.DrawText(_T("▶"), &play_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

                dc.SelectObject(old);
            }
            y += kClipRowH;
        }
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
    case SB_TOP:        new_pos = si.nMin; break;
    case SB_BOTTOM:     new_pos = si.nMax; break;
    case SB_LINEUP:     new_pos -= kClipRowH; break;
    case SB_LINEDOWN:   new_pos += kClipRowH; break;
    case SB_PAGEUP:     new_pos -= si.nPage; break;
    case SB_PAGEDOWN:   new_pos += si.nPage; break;
    case SB_THUMBTRACK: new_pos = static_cast<int>(nPos); break;
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
    const int delta = (zDelta > 0 ? -kClipRowH * 3 : kClipRowH * 3);
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

void CClipsPanel::OnShowWindow(BOOL bShow, UINT /*nStatus*/)
{
    if (bShow) {
        scan_clips();
        Invalidate();
    }
}

void CClipsPanel::OnLButtonDown(UINT /*nFlags*/, CPoint point)
{
    const ClipEntry* clip = hit_test_clip(point);
    if (!clip || clip->mp4_path.empty()) return;
    ShellExecuteW(nullptr, L"open", clip->mp4_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const CClipsPanel::ClipEntry* CClipsPanel::hit_test_clip(CPoint pt) const
{
    if (pt.y < kHeaderH) return nullptr;

    int y = kHeaderH - scroll_pos_;
    for (const auto& se : sessions_) {
        y += kSessionRowH;
        for (const auto& clip : se.clips) {
            if (pt.y >= y && pt.y < y + kClipRowH) return &clip;
            y += kClipRowH;
        }
    }
    return nullptr;
}
