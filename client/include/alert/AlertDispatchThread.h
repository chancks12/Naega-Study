#pragma once

#include "alert/AlertQueue.h"
#include "model/ToastBuffer.h"
#include "render/RenderThread.h"

#include <atomic>
#include <thread>

class AlertDispatchThread {
public:
    AlertDispatchThread(AlertQueue& alert_queue, ToastBuffer& toast_buffer);
    ~AlertDispatchThread();

    void start();
    void stop();

    // 렌더 스레드 연결 — 휴식 알림 오버레이 표시용 (start() 전 호출)
    void set_render_thread(RenderThread* rt) { render_thread_ = rt; }

private:
    void run();
    void show_popup(const Alert& alert);

    AlertQueue&   alert_queue_;
    ToastBuffer&  toast_buffer_;
    RenderThread* render_thread_ = nullptr;
    std::atomic_bool running_{ false };
    std::thread worker_;
};

