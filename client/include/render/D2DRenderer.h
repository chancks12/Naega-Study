#pragma once

#include "model/AnalysisResultBuffer.h"
#include "model/ServerStatsSnapshot.h"
#include "model/SessionStatsHistory.h"
#include "model/ToastBuffer.h"
#include "render/OverlayPainter.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <mutex>
#include <opencv2/core/mat.hpp>
#include <wrl/client.h>

class D2DRenderer {
public:
    ~D2DRenderer() = default;

    bool init(HWND hwnd, AnalysisResultBuffer& result_buffer);
    void upload_and_render(const cv::Mat& bgr);
    void render_blank();
    void notify_resize(UINT w, UINT h);

    // 세션 시작 이후 set_session_start_ms / set_toast_buffer 호출 (렌더 스레드 안전)
    void set_session_start_ms(std::uint64_t ms)      { overlay_.set_session_start_ms(ms); }
    void set_toast_buffer(ToastBuffer* tb)            { overlay_.set_toast_buffer(tb); }
    void set_calibration_countdown(int sec)           { overlay_.set_calibration_countdown(sec); }
    void set_stats_history(SessionStatsHistory* h)    { overlay_.set_stats_history(h); }
    void set_server_stats(ServerStatsSnapshot* s)      { overlay_.set_server_stats(s); }
    void set_break_alert(std::uint64_t expire_ms)     { overlay_.set_break_alert(expire_ms); }

private:
    bool recreate_target();
    void apply_pending_resize();
    bool update_bgra_buffer(const cv::Mat& bgr);
    bool ensure_bitmap(UINT w, UINT h);

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory>          factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap>           bitmap_;
    D2D1_SIZE_U                                   bitmap_size_{};
    cv::Mat                                       bgra_buffer_;

    std::mutex resize_mtx_;
    UINT       pending_w_      = 0;
    UINT       pending_h_      = 0;
    bool       resize_pending_ = false;

    AnalysisResultBuffer* result_buffer_ = nullptr;   // 공유 버퍼 (비소유)
    OverlayPainter        overlay_;
};
