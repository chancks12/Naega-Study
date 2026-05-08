#include "pch.h"
#include "render/OverlayPainter.h"

#include <algorithm>
#include <chrono>
#include <cwchar>

#pragma comment(lib, "dwrite.lib")

namespace {
constexpr float kPanelW = 215.0f;
constexpr float kPanelH = 178.0f;
constexpr float kPanelMargin = 12.0f;
constexpr float kCornerR = 8.0f;
constexpr float kRowH = 28.0f;
constexpr float kPadX = 12.0f;
constexpr float kPadY = 10.0f;

std::wstring widen_state(const AnalysisResult& result)
{
    if (result.absent) return L"Absent";
    if (result.drowsy) return L"Drowsy";
    if (result.state == "distracted") return L"Distracted";
    if (result.state == "drowsy") return L"Drowsy";
    if (result.state == "absent") return L"Absent";
    if (result.state == "warning") return L"Warning";
    return L"Focus";
}
} // namespace

OverlayPainter::~OverlayPainter()
{
    release_resources();
}

void OverlayPainter::invalidate()
{
    release_resources();
    initialized_ = false;
}

void OverlayPainter::release_resources()
{
    brush_bg_.Reset();
    brush_white_.Reset();
    brush_gray_.Reset();
    brush_green_.Reset();
    brush_yellow_.Reset();
    brush_orange_.Reset();
    brush_red_.Reset();
    brush_bar_bg_.Reset();
    brush_toast_bg_.Reset();
    brush_timer_bg_.Reset();
    brush_calib_bg_.Reset();
    brush_calib_panel_.Reset();
    brush_chart_line_.Reset();
    brush_chart_bg_.Reset();
    brush_break_bg_.Reset();
    brush_break_panel_.Reset();
    fmt_label_.Reset();
    fmt_value_.Reset();
    fmt_score_.Reset();
    fmt_calib_count_.Reset();
    fmt_calib_msg_.Reset();
    fmt_break_title_.Reset();
    fmt_break_msg_.Reset();
}

bool OverlayPainter::ensure_resources(ID2D1RenderTarget* rt)
{
    if (initialized_) return true;
    if (!rt) return false;

    if (!dwrite_) {
        const HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        if (FAILED(hr)) return false;
    }

    const WCHAR* font = L"Segoe UI";
    dwrite_->CreateTextFormat(font, nullptr,
                              DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL,
                              11.0f,
                              L"en-US",
                              fmt_label_.GetAddressOf());

    dwrite_->CreateTextFormat(font, nullptr,
                              DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL,
                              12.0f,
                              L"en-US",
                              fmt_value_.GetAddressOf());

    dwrite_->CreateTextFormat(font, nullptr,
                              DWRITE_FONT_WEIGHT_BOLD,
                              DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL,
                              22.0f,
                              L"en-US",
                              fmt_score_.GetAddressOf());

    if (!fmt_label_ || !fmt_value_ || !fmt_score_) return false;

    fmt_label_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_value_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_score_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // Brushes are render-target resources, so they must be rebuilt after target recreation.
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f), brush_bg_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brush_white_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.55f, 0.55f), brush_gray_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.72f, 0.50f), brush_green_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.62f, 0.04f), brush_yellow_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.45f, 0.12f), brush_orange_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.20f, 0.20f), brush_red_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.22f, 0.22f), brush_bar_bg_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.12f, 0.12f, 0.88f), brush_toast_bg_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f,  0.0f,  0.0f,  0.60f), brush_timer_bg_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f,  0.0f,  0.0f,  0.55f), brush_calib_bg_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.12f, 0.20f, 0.92f), brush_calib_panel_.GetAddressOf()))) return false;

    // 캘리브레이션 큰 카운트다운 숫자 (80pt bold)
    dwrite_->CreateTextFormat(font, nullptr,
                              DWRITE_FONT_WEIGHT_BOLD,
                              DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL,
                              80.0f,
                              L"en-US",
                              fmt_calib_count_.GetAddressOf());
    if (fmt_calib_count_) fmt_calib_count_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // 캘리브레이션 안내 메시지 (16pt)
    dwrite_->CreateTextFormat(font, nullptr,
                              DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL,
                              16.0f,
                              L"en-US",
                              fmt_calib_msg_.GetAddressOf());
    if (fmt_calib_msg_) fmt_calib_msg_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // 휴식 알림 제목 (22pt bold)
    dwrite_->CreateTextFormat(font, nullptr,
                              DWRITE_FONT_WEIGHT_BOLD,
                              DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL,
                              22.0f,
                              L"en-US",
                              fmt_break_title_.GetAddressOf());
    if (fmt_break_title_) fmt_break_title_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // 휴식 알림 메시지 (14pt)
    dwrite_->CreateTextFormat(font, nullptr,
                              DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL,
                              14.0f,
                              L"en-US",
                              fmt_break_msg_.GetAddressOf());
    if (fmt_break_msg_) fmt_break_msg_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // 통계 / 휴식 알림 전용 브러시
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.60f, 1.00f, 1.0f),  brush_chart_line_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.05f, 0.10f, 0.82f), brush_chart_bg_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f,  0.0f,  0.0f,  0.60f), brush_break_bg_.GetAddressOf()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.16f, 0.28f, 0.95f), brush_break_panel_.GetAddressOf()))) return false;

    initialized_ = true;
    return true;
}

void OverlayPainter::draw(ID2D1RenderTarget* rt, const AnalysisResult& result)
{
    if (!rt) return;
    if (!ensure_resources(rt)) return;

    // ── 좌상단: 세션 타이머 (항상 표시) ──────────────────────
    constexpr float kLeft = 12.0f;
    float next_y = 12.0f;

    const std::uint64_t start_ms = session_start_ms_.load();
    if (start_ms > 0) {
        draw_timer(rt, kLeft, next_y);
        next_y += 36.0f;
    }

    // ── 좌상단: 토스트 알림 (활성 시 표시) ────────────────────
    if (toast_buffer_) {
        std::string toast_text;
        if (toast_buffer_->get_active(toast_text)) {
            draw_toast(rt, kLeft, next_y, toast_text);
        }
    }

    // ── 좌하단: 통계 다이어그램 ───────────────────────────────
    if (stats_history_) {
        draw_stats_panel(rt);
    }

    // ── 휴식 권장 오버레이 (활성 시 최우선) ──────────────────
    {
        using namespace std::chrono;
        const auto now_ms = static_cast<std::uint64_t>(
            duration_cast<milliseconds>(
                steady_clock::now().time_since_epoch()).count());
        const std::uint64_t expire = break_alert_expire_ms_.load();
        if (expire > 0 && now_ms < expire) {
            draw_break_alert(rt);
        } else if (expire > 0 && now_ms >= expire) {
            break_alert_expire_ms_.store(0);  // 만료 시 자동 숨김
        }
    }

    // ── 캘리브레이션 오버레이 (활성 시 최우선 표시) ──────────────
    {
        const int calib = calib_countdown_.load();
        if (calib >= 0) {
            draw_calibration(rt, calib);
            return;  // 캘리브레이션 중에는 분석 패널 숨김
        }
    }

    // ── 우상단: 상태 뱃지 (단순 3-클래스 표시) ───────────────────
    draw_status_badge(rt, result);

    // ── 우상단: 분석 수치 패널 (뱃지 아래) ─────────────────────
    const D2D1_SIZE_F size = rt->GetSize();
    const float px = size.width - kPanelW - kPanelMargin;
    const float py = kPanelMargin + 48.0f;  // 뱃지 높이(40) + 간격(8)
    const D2D1_RECT_F panel = D2D1::RectF(px, py, px + kPanelW, py + kPanelH);

    draw_panel_bg(rt, panel);

    float y = py + kPadY;

    draw_text(rt, L"Focus", px + kPadX, y, 50, 16, fmt_label_.Get(), brush_gray_.Get());
    {
        wchar_t value[16]{};
        swprintf_s(value, L"%d", result.focus_score);
        rt->DrawText(value,
                     static_cast<UINT32>(wcslen(value)),
                     fmt_score_.Get(),
                     D2D1::RectF(px + kPanelW - 50, y - 4, px + kPanelW - kPadX, y + 24),
                     brush_white_.Get());

        const float ratio = std::clamp(static_cast<float>(result.focus_score) / 100.0f, 0.0f, 1.0f);
        draw_bar(rt, px + kPadX + 52, y + 4, kPanelW - kPadX * 2 - 96, 10,
                 ratio, focus_bar_brush(static_cast<float>(result.focus_score)), brush_bar_bg_.Get());
    }
    y += kRowH;

    draw_text(rt, L"State", px + kPadX, y, 42, 16, fmt_label_.Get(), brush_gray_.Get());
    {
        ID2D1SolidColorBrush* dot = brush_green_.Get();
        if (result.absent || !result.posture_ok) dot = brush_red_.Get();
        else if (result.drowsy) dot = brush_orange_.Get();
        else if (result.state == "distracted" || result.state == "warning") dot = brush_yellow_.Get();

        draw_dot(rt, px + kPadX + 48, y + 8, 5.0f, dot);
        draw_text(rt, widen_state(result), px + kPadX + 60, y, 100, 16, fmt_value_.Get(), brush_white_.Get());
    }
    y += kRowH;

    draw_text(rt, L"Posture", px + kPadX, y, 50, 16, fmt_label_.Get(), brush_gray_.Get());
    draw_text(rt, result.posture_ok ? L"OK" : L"Warning",
              px + kPadX + 60, y, 90, 16, fmt_value_.Get(),
              result.posture_ok ? brush_green_.Get() : brush_red_.Get());
    y += kRowH;

    draw_text(rt, L"Neck", px + kPadX, y, 50, 16, fmt_label_.Get(), brush_gray_.Get());
    {
        wchar_t value[24]{};
        swprintf_s(value, L"%.1f deg", result.neck_angle);
        ID2D1SolidColorBrush* brush = (result.neck_angle > 30.0) ? brush_red_.Get()
                                      : (result.neck_angle > 15.0) ? brush_yellow_.Get()
                                                                   : brush_white_.Get();
        draw_text(rt, value, px + kPadX + 60, y, 100, 16, fmt_value_.Get(), brush);
    }
    y += kRowH;

    draw_text(rt, L"EAR", px + kPadX, y, 30, 16, fmt_label_.Get(), brush_gray_.Get());
    {
        const float ratio = std::clamp(static_cast<float>(result.ear) / 0.40f, 0.0f, 1.0f);
        ID2D1SolidColorBrush* fill = (result.ear < 0.20) ? brush_red_.Get()
                                    : (result.ear < 0.25) ? brush_orange_.Get()
                                                          : brush_green_.Get();
        draw_bar(rt, px + kPadX + 36, y + 4, kPanelW - kPadX * 2 - 80, 10, ratio, fill, brush_bar_bg_.Get());
        wchar_t value[16]{};
        swprintf_s(value, L"%.2f", result.ear);
        draw_text(rt, value, px + kPanelW - 50, y, 38, 16, fmt_label_.Get(), brush_gray_.Get());
    }
    y += kRowH;

    draw_text(rt, L"Shoulder", px + kPadX, y, 56, 16, fmt_label_.Get(), brush_gray_.Get());
    {
        wchar_t value[24]{};
        swprintf_s(value, L"%.1f deg", result.shoulder_diff);
        ID2D1SolidColorBrush* brush = (result.shoulder_diff > 20.0) ? brush_red_.Get()
                                      : (result.shoulder_diff > 10.0) ? brush_yellow_.Get()
                                                                      : brush_white_.Get();
        draw_text(rt, value, px + kPadX + 70, y, 90, 16, fmt_value_.Get(), brush);
    }
}

void OverlayPainter::draw_panel_bg(ID2D1RenderTarget* rt, const D2D1_RECT_F& panel)
{
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(panel, kCornerR, kCornerR);
    rt->FillRoundedRectangle(rr, brush_bg_.Get());
    rt->DrawRoundedRectangle(rr, brush_gray_.Get(), 0.8f);
}

void OverlayPainter::draw_bar(ID2D1RenderTarget* rt,
                              float x,
                              float y,
                              float w,
                              float h,
                              float ratio,
                              ID2D1SolidColorBrush* fill,
                              ID2D1SolidColorBrush* bg)
{
    ratio = std::clamp(ratio, 0.0f, 1.0f);

    const D2D1_ROUNDED_RECT bg_rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), h / 2.0f, h / 2.0f);
    rt->FillRoundedRectangle(bg_rr, bg);

    if (ratio <= 0.0f) return;

    const float raw_fill_w = w * ratio;
    const float fill_w = raw_fill_w < h ? h : raw_fill_w;
    const D2D1_ROUNDED_RECT fill_rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + fill_w, y + h), h / 2.0f, h / 2.0f);
    rt->FillRoundedRectangle(fill_rr, fill);
}

void OverlayPainter::draw_text(ID2D1RenderTarget* rt,
                               const std::wstring& text,
                               float x,
                               float y,
                               float w,
                               float h,
                               IDWriteTextFormat* fmt,
                               ID2D1SolidColorBrush* brush)
{
    rt->DrawText(text.c_str(),
                 static_cast<UINT32>(text.size()),
                 fmt,
                 D2D1::RectF(x, y, x + w, y + h),
                 brush);
}

void OverlayPainter::draw_dot(ID2D1RenderTarget* rt,
                              float cx,
                              float cy,
                              float r,
                              ID2D1SolidColorBrush* brush)
{
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush);
}

ID2D1SolidColorBrush* OverlayPainter::focus_bar_brush(float score) const
{
    if (score >= 70.0f) return brush_green_.Get();
    if (score >= 40.0f) return brush_yellow_.Get();
    return brush_red_.Get();
}

void OverlayPainter::draw_timer(ID2D1RenderTarget* rt, float x, float y)
{
    using namespace std::chrono;
    const long long now = duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
    const long long elapsed_s = (now - static_cast<long long>(session_start_ms_.load())) / 1000;
    const long long h = elapsed_s / 3600;
    const long long m = (elapsed_s % 3600) / 60;
    const long long s = elapsed_s % 60;

    wchar_t buf[32];
    swprintf_s(buf, L"%02lld:%02lld:%02lld", h, m, s);

    constexpr float kW = 90.0f;
    constexpr float kH = 26.0f;
    const D2D1_ROUNDED_RECT bg = D2D1::RoundedRect(
        D2D1::RectF(x, y, x + kW, y + kH), 5.0f, 5.0f);
    rt->FillRoundedRectangle(bg, brush_timer_bg_.Get());

    draw_text(rt, buf, x + 8.0f, y + 5.0f, kW - 10.0f, kH, fmt_value_.Get(), brush_white_.Get());
}

void OverlayPainter::draw_stats_panel(ID2D1RenderTarget* rt)
{
    if (!stats_history_ || !brush_chart_bg_ || !brush_chart_line_) return;

    const SessionStatsHistory::Snapshot snap = stats_history_->snapshot();
    if (snap.total < 2) return;  // 데이터 부족

    const D2D1_SIZE_F size = rt->GetSize();

    // ── 패널 크기 / 위치 (좌하단) ─────────────────────────────
    constexpr float kPW   = 420.0f;
    constexpr float kPH   = 110.0f;
    constexpr float kMarg = 12.0f;
    const float px = kMarg;
    const float py = size.height - kPH - kMarg;

    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
        D2D1::RectF(px, py, px + kPW, py + kPH), 8.0f, 8.0f);
    rt->FillRoundedRectangle(panel, brush_chart_bg_.Get());
    rt->DrawRoundedRectangle(panel, brush_gray_.Get(), 0.6f);

    // ── 라벨 ──────────────────────────────────────────────────
    draw_text(rt, L"Session Stats",
              px + 10.0f, py + 6.0f, 120.0f, 16.0f,
              fmt_label_.Get(), brush_gray_.Get());

    // ── 우측 통계 수치 ────────────────────────────────────────
    constexpr float kChartW = 260.0f;
    const float sx = px + kChartW + 16.0f;
    float sy = py + 8.0f;

    wchar_t buf[64];
    swprintf_s(buf, L"Avg Focus : %d", snap.avg_focus);
    draw_text(rt, buf, sx, sy, 140.0f, 16.0f, fmt_label_.Get(), brush_white_.Get());
    sy += 20.0f;

    swprintf_s(buf, L"Posture OK: %.0f%%", snap.posture_ok_pct * 100.0f);
    ID2D1SolidColorBrush* posture_brush =
        (snap.posture_ok_pct >= 0.8f) ? brush_green_.Get() :
        (snap.posture_ok_pct >= 0.5f) ? brush_yellow_.Get() : brush_red_.Get();
    draw_text(rt, buf, sx, sy, 140.0f, 16.0f, fmt_label_.Get(), posture_brush);
    sy += 20.0f;

    swprintf_s(buf, L"Events    : %d", snap.event_count);
    draw_text(rt, buf, sx, sy, 140.0f, 16.0f, fmt_label_.Get(),
              snap.event_count > 0 ? brush_orange_.Get() : brush_white_.Get());
    sy += 20.0f;

    swprintf_s(buf, L"Samples   : %d", snap.total);
    draw_text(rt, buf, sx, sy, 140.0f, 16.0f, fmt_label_.Get(), brush_gray_.Get());

    if (server_stats_) {
        const ServerStatsSnapshot::Data server = server_stats_->snapshot();
        if (server.valid) {
            sy += 18.0f;
            swprintf_s(buf, L"Today    : %d min", server.focus_min);
            draw_text(rt, buf, sx, sy, 140.0f, 16.0f, fmt_label_.Get(), brush_white_.Get());

            sy += 18.0f;
            swprintf_s(buf, L"Srv Avg  : %.0f%%", server.avg_focus * 100.0);
            draw_text(rt, buf, sx, sy, 140.0f, 16.0f, fmt_label_.Get(), brush_green_.Get());
        }
    }

    // ── 꺾은선 그래프 영역 ────────────────────────────────────
    constexpr float kChartH    = 72.0f;
    const float chart_x = px + 10.0f;
    const float chart_y = py + 26.0f;
    const float chart_r = px + kChartW;
    const float chart_b = py + kPH - 10.0f;

    // 그리드 기준선 (50% 점선 느낌으로 회색 선)
    const float mid_y = chart_y + kChartH * 0.5f;
    rt->DrawLine(D2D1::Point2F(chart_x, mid_y),
                 D2D1::Point2F(chart_r, mid_y),
                 brush_gray_.Get(), 0.5f);

    // 데이터 포인트 → 꺾은선
    const auto& ch = snap.chart;
    const int   n  = static_cast<int>(ch.size());
    if (n < 2) return;

    const float step = (chart_r - chart_x) / static_cast<float>(n - 1);

    for (int i = 1; i < n; ++i) {
        const float x0 = chart_x + step * (i - 1);
        const float x1 = chart_x + step * i;
        const float y0 = chart_b - (ch[i - 1] / 100.0f) * kChartH;
        const float y1 = chart_b - (ch[i]     / 100.0f) * kChartH;

        // 집중도에 따라 선 색상 변경
        ID2D1SolidColorBrush* line_brush =
            (ch[i] >= 70) ? brush_green_.Get() :
            (ch[i] >= 40) ? brush_yellow_.Get() : brush_red_.Get();

        rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1),
                     line_brush, 1.5f);
    }

    // 현재 값 강조 (마지막 점)
    const float lx = chart_x + step * (n - 1);
    const float ly = chart_b - (ch.back() / 100.0f) * kChartH;
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(lx, ly), 3.5f, 3.5f),
                    brush_chart_line_.Get());
}

void OverlayPainter::draw_break_alert(ID2D1RenderTarget* rt)
{
    if (!fmt_break_title_ || !fmt_break_msg_) return;

    const D2D1_SIZE_F size = rt->GetSize();
    const float cx = size.width  * 0.5f;
    const float cy = size.height * 0.5f;

    // 반투명 전체 덮기
    rt->FillRectangle(D2D1::RectF(0, 0, size.width, size.height),
                      brush_break_bg_.Get());

    // 중앙 패널 (400 × 200)
    constexpr float kPW = 400.0f;
    constexpr float kPH = 200.0f;
    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
        D2D1::RectF(cx - kPW * 0.5f, cy - kPH * 0.5f,
                    cx + kPW * 0.5f, cy + kPH * 0.5f),
        14.0f, 14.0f);
    rt->FillRoundedRectangle(panel, brush_break_panel_.Get());
    rt->DrawRoundedRectangle(panel, brush_orange_.Get(), 1.5f);

    // 아이콘 (느낌표 원)
    rt->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(cx, cy - 60.0f), 20.0f, 20.0f),
        brush_orange_.Get());
    draw_text(rt, L"!",
              cx - 20.0f, cy - 74.0f, 40.0f, 30.0f,
              fmt_calib_count_.Get(), brush_white_.Get());

    // 제목
    draw_text(rt, L"Time for a Break!",
              cx - kPW * 0.5f, cy - 26.0f,
              kPW, 28.0f, fmt_break_title_.Get(), brush_orange_.Get());

    // 메시지
    draw_text(rt, L"You've been studying hard. Please rest your eyes",
              cx - kPW * 0.5f, cy + 12.0f,
              kPW, 20.0f, fmt_break_msg_.Get(), brush_gray_.Get());
    draw_text(rt, L"and take a short stretch break.",
              cx - kPW * 0.5f, cy + 34.0f,
              kPW, 20.0f, fmt_break_msg_.Get(), brush_gray_.Get());

    // 하단 안내
    draw_text(rt, L"(This message will disappear automatically)",
              cx - kPW * 0.5f, cy + 72.0f,
              kPW, 16.0f, fmt_label_.Get(), brush_gray_.Get());
}

void OverlayPainter::draw_calibration(ID2D1RenderTarget* rt, int countdown)
{
    if (!fmt_calib_count_ || !fmt_calib_msg_) return;

    const D2D1_SIZE_F size = rt->GetSize();
    const float cx = size.width  * 0.5f;
    const float cy = size.height * 0.5f;

    // 반투명 전체 화면 덮기
    rt->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), brush_calib_bg_.Get());

    // 중앙 패널 (360 x 260)
    constexpr float kPW = 360.0f;
    constexpr float kPH = 260.0f;
    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
        D2D1::RectF(cx - kPW * 0.5f, cy - kPH * 0.5f, cx + kPW * 0.5f, cy + kPH * 0.5f),
        14.0f, 14.0f);
    rt->FillRoundedRectangle(panel, brush_calib_panel_.Get());
    rt->DrawRoundedRectangle(panel, brush_gray_.Get(), 1.0f);

    if (countdown > 0) {
        // 상단 안내 문구
        draw_text(rt, L"Sit in your study posture",
                  cx - kPW * 0.5f, cy - kPH * 0.5f + 22.0f,
                  kPW, 22.0f, fmt_calib_msg_.Get(), brush_gray_.Get());
        draw_text(rt, L"This will be your baseline",
                  cx - kPW * 0.5f, cy - kPH * 0.5f + 48.0f,
                  kPW, 22.0f, fmt_calib_msg_.Get(), brush_gray_.Get());

        // 큰 카운트다운 숫자
        wchar_t num[4]{};
        swprintf_s(num, L"%d", countdown);
        rt->DrawText(num,
                     static_cast<UINT32>(wcslen(num)),
                     fmt_calib_count_.Get(),
                     D2D1::RectF(cx - 60.0f, cy - 60.0f, cx + 60.0f, cy + 60.0f),
                     brush_white_.Get());

        // 하단 보조 설명
        draw_text(rt, L"Calibrating your posture...",
                  cx - kPW * 0.5f, cy + kPH * 0.5f - 44.0f,
                  kPW, 20.0f, fmt_calib_msg_.Get(), brush_gray_.Get());
    } else {
        // countdown == 0 → 완료 메시지
        draw_text(rt, L"Calibration complete!",
                  cx - kPW * 0.5f, cy - 60.0f,
                  kPW, 24.0f, fmt_calib_msg_.Get(), brush_green_.Get());
        draw_text(rt, L"Your posture baseline has been set.",
                  cx - kPW * 0.5f, cy - 28.0f,
                  kPW, 20.0f, fmt_calib_msg_.Get(), brush_gray_.Get());
        draw_text(rt, L"Starting session...",
                  cx - kPW * 0.5f, cy + 8.0f,
                  kPW, 20.0f, fmt_calib_msg_.Get(), brush_gray_.Get());
    }
}

void OverlayPainter::draw_toast(ID2D1RenderTarget* rt, float x, float y, const std::string& text)
{
    // ASCII 변환 (DirectWrite는 wstring)
    const std::wstring wtext(text.begin(), text.end());

    constexpr float kW = 240.0f;
    constexpr float kH = 48.0f;

    const D2D1_ROUNDED_RECT bg = D2D1::RoundedRect(
        D2D1::RectF(x, y, x + kW, y + kH), 6.0f, 6.0f);
    rt->FillRoundedRectangle(bg, brush_toast_bg_.Get());
    rt->DrawRoundedRectangle(bg, brush_red_.Get(), 1.0f);

    // 경고 아이콘 (느낌표)
    draw_text(rt, L"!", x + 10.0f, y + 8.0f, 16.0f, 32.0f, fmt_score_.Get(), brush_white_.Get());

    // 알림 텍스트 (title : message 구조, ':' 기준으로 두 줄로 분리)
    const auto colon = text.find(": ");
    if (colon != std::string::npos) {
        const std::wstring title(text.begin(), text.begin() + colon);
        const std::wstring msg(text.begin() + colon + 2, text.end());
        draw_text(rt, title, x + 30.0f, y +  8.0f, kW - 40.0f, 18.0f, fmt_value_.Get(), brush_white_.Get());
        draw_text(rt, msg,   x + 30.0f, y + 27.0f, kW - 40.0f, 16.0f, fmt_label_.Get(), brush_gray_.Get());
    } else {
        draw_text(rt, wtext, x + 30.0f, y + 14.0f, kW - 40.0f, 20.0f, fmt_value_.Get(), brush_white_.Get());
    }
}

// ── 우상단 상태 뱃지: 모서리 둥근 가로 박스 + 색 원 + 한글 상태 텍스트 ──

void OverlayPainter::draw_status_badge(ID2D1RenderTarget* rt, const AnalysisResult& result)
{
    const D2D1_SIZE_F size = rt->GetSize();
    constexpr float kBW = 215.0f, kBH = 40.0f, kR = 8.0f;
    const float bx = size.width - kBW - kPanelMargin;
    const float by = kPanelMargin;

    // 배경 rounded rect
    const D2D1_RECT_F bg = D2D1::RectF(bx, by, bx + kBW, by + kBH);
    rt->FillRoundedRectangle(D2D1::RoundedRect(bg, kR, kR), brush_bg_.Get());
    rt->DrawRoundedRectangle(D2D1::RoundedRect(bg, kR, kR), brush_gray_.Get(), 0.8f);

    // 상태 색상 및 한글 라벨 결정
    ID2D1SolidColorBrush* circle_brush = brush_gray_.Get();
    const wchar_t* label = L"대기 중";

    if (result.absent) {
        circle_brush = brush_red_.Get();
        label = L"다른 행동 중";
    } else if (result.drowsy || result.state == "drowsy") {
        circle_brush = brush_orange_.Get();
        label = L"졸음";
    } else if (result.state == "distracted") {
        circle_brush = brush_yellow_.Get();
        label = L"다른 행동 중";
    } else if (result.state == "focus") {
        circle_brush = brush_green_.Get();
        label = L"공부 중";
    }

    // 색 원
    const float cx = bx + 20.0f;
    const float cy = by + kBH / 2.0f;
    draw_dot(rt, cx, cy, 8.0f, circle_brush);

    // 상태 텍스트
    draw_text(rt, label,
              cx + 16.0f, by + 2.0f,
              kBW - 44.0f, kBH - 4.0f,
              fmt_value_.Get(), brush_white_.Get());
}
