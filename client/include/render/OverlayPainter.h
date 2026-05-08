#pragma once

#include "model/AnalysisResult.h"
#include "model/ServerStatsSnapshot.h"
#include "model/SessionStatsHistory.h"
#include "model/ToastBuffer.h"

#include <atomic>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <wrl/client.h>

// Draws posture/focus HUD data on top of the current Direct2D frame.
// The renderer owns frame upload; this class owns only overlay resources and drawing.
class OverlayPainter {
public:
    ~OverlayPainter();

    // Called when the render target is recreated so D2D target-bound resources are rebuilt.
    void invalidate();

    // 세션 시작 시각 설정 (steady_clock 기준 ms). 0이면 타이머 미표시.
    void set_session_start_ms(std::uint64_t ms) { session_start_ms_.store(ms); }

    // 토스트 버퍼 연결 (렌더 스레드 시작 전 1회 호출).
    void set_toast_buffer(ToastBuffer* tb) { toast_buffer_ = tb; }

    // 캘리브레이션 카운트다운 설정
    //  5~1: 카운트다운 표시,  0: "완료!" 표시,  -1: 오버레이 숨김
    void set_calibration_countdown(int sec) { calib_countdown_.store(sec); }

    // 통계 히스토리 연결 (렌더 스레드 시작 전 1회)
    void set_stats_history(SessionStatsHistory* h) { stats_history_ = h; }
    void set_server_stats(ServerStatsSnapshot* s) { server_stats_ = s; }

    // 휴식 권장 알림 트리거 (AlertDispatchThread → 렌더 스레드)
    //  expire_ms: steady_clock 기준 만료 시각 (0이면 숨김)
    void set_break_alert(std::uint64_t expire_ms) { break_alert_expire_ms_.store(expire_ms); }

    // Draws the latest AI analysis result over the camera frame.
    void draw(ID2D1RenderTarget* rt, const AnalysisResult& result);

private:
    bool ensure_resources(ID2D1RenderTarget* rt);
    void release_resources();

    void draw_panel_bg(ID2D1RenderTarget* rt, const D2D1_RECT_F& panel);
    void draw_bar(ID2D1RenderTarget* rt,
                  float x,
                  float y,
                  float w,
                  float h,
                  float ratio,
                  ID2D1SolidColorBrush* fill,
                  ID2D1SolidColorBrush* bg);
    void draw_text(ID2D1RenderTarget* rt,
                   const std::wstring& text,
                   float x,
                   float y,
                   float w,
                   float h,
                   IDWriteTextFormat* fmt,
                   ID2D1SolidColorBrush* brush);
    void draw_dot(ID2D1RenderTarget* rt,
                  float cx,
                  float cy,
                  float r,
                  ID2D1SolidColorBrush* brush);

    ID2D1SolidColorBrush* focus_bar_brush(float score) const;
    void draw_timer(ID2D1RenderTarget* rt, float x, float y);
    void draw_toast(ID2D1RenderTarget* rt, float x, float y, const std::string& text);
    void draw_calibration(ID2D1RenderTarget* rt, int countdown);
    void draw_stats_panel(ID2D1RenderTarget* rt);
    void draw_break_alert(ID2D1RenderTarget* rt);
    void draw_status_badge(ID2D1RenderTarget* rt, const AnalysisResult& result);

    // 세션 타이머 / 토스트 / 캘리브레이션 / 통계 / 휴식알림
    std::atomic<std::uint64_t> session_start_ms_{ 0 };
    ToastBuffer*               toast_buffer_ = nullptr;
    std::atomic<int>           calib_countdown_{ -1 };
    SessionStatsHistory*       stats_history_ = nullptr;
    ServerStatsSnapshot*       server_stats_ = nullptr;
    std::atomic<std::uint64_t> break_alert_expire_ms_{ 0 };

    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_label_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_value_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_score_;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_bg_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_white_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_gray_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_green_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_yellow_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_orange_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_red_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_bar_bg_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_toast_bg_;    // 토스트 배경
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_timer_bg_;    // 타이머 배경
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_calib_bg_;    // 캘리브레이션 반투명 배경
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_calib_panel_; // 캘리브레이션 패널
    Microsoft::WRL::ComPtr<IDWriteTextFormat>     fmt_calib_count_;   // 큰 카운트다운 숫자
    Microsoft::WRL::ComPtr<IDWriteTextFormat>     fmt_calib_msg_;     // 안내 메시지

    // 통계 패널 / 휴식 알림 전용 브러시
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_chart_line_;  // 꺾은선 그래프
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_chart_bg_;    // 통계 패널 배경
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_break_bg_;    // 휴식 알림 배경
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_break_panel_; // 휴식 알림 패널
    Microsoft::WRL::ComPtr<IDWriteTextFormat>     fmt_break_title_;   // 휴식 알림 제목
    Microsoft::WRL::ComPtr<IDWriteTextFormat>     fmt_break_msg_;     // 휴식 알림 메시지

    bool initialized_ = false;
};
