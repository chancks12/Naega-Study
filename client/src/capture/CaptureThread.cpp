#include "pch.h"
#include "capture/CaptureThread.h"

#include <chrono>
#include <opencv2/videoio.hpp>

namespace {
std::uint64_t now_ms()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}
}

CaptureThread::CaptureThread(RenderFrameBuffer& render_buffer, SendFrameBuffer& send_buffer, EventShadowBuffer& shadow_buffer)
    : render_buffer_(render_buffer)
    , send_buffer_(send_buffer)
    , shadow_buffer_(shadow_buffer)
{
}

CaptureThread::~CaptureThread()
{
    stop();
}

void CaptureThread::start(int camera_index, int fps)
{
    if (running_.exchange(true)) {
        return;
    }

    worker_ = std::thread(&CaptureThread::run, this, camera_index, fps);
}

void CaptureThread::stop()
{
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void CaptureThread::run(int camera_index, int fps)
{
    cv::VideoCapture capture(camera_index);
    if (!capture.isOpened()) {
        running_ = false;
        return;
    }

    // camera blocking read (capture >>) already paces at the camera's
    // native frame rate — no additional sleep needed
    while (running_) {
        Frame frame;
        capture >> frame.mat;
        if (frame.mat.empty()) {
            continue;
        }

        frame.timestamp_ms = now_ms();
        render_buffer_.push(frame);
        send_buffer_.push(frame);
        shadow_buffer_.push(frame);
    }
}
