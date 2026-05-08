#include "pch.h"
#include "render/RenderThread.h"

#include <chrono>

RenderThread::RenderThread(CaptureThread::RenderFrameBuffer& frame_buffer)
    : frame_buffer_(frame_buffer)
{
}

RenderThread::~RenderThread()
{
    stop();
}

void RenderThread::start(HWND hwnd, AnalysisResultBuffer& result_buffer)
{
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&RenderThread::run, this, hwnd, &result_buffer);
}

void RenderThread::stop()
{
    running_ = false;
    frame_buffer_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void RenderThread::notify_resize(UINT w, UINT h)
{
    renderer_.notify_resize(w, h);
}

void RenderThread::run(HWND hwnd, AnalysisResultBuffer* result_buffer)
{
    if (!renderer_.init(hwnd, *result_buffer)) {
        running_ = false;
        return;
    }

    while (running_) {
        auto frame_opt = frame_buffer_.wait_pop_for(std::chrono::milliseconds(33));
        if (!running_) break;

        if (!frame_opt || frame_opt->mat.empty()) {
            renderer_.render_blank();
            continue;
        }

        renderer_.upload_and_render(frame_opt->mat);
    }
}
