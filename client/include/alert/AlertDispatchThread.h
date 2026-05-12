#pragma once

#include "alert/AlertQueue.h"
#include "model/ToastBuffer.h"

#include <atomic>
#include <thread>

class AlertDispatchThread {
public:
    AlertDispatchThread(AlertQueue& alert_queue, ToastBuffer& toast_buffer);
    ~AlertDispatchThread();

    void start();
    void stop();

private:
    void run();
    void show_popup(const Alert& alert);

    AlertQueue&      alert_queue_;
    ToastBuffer&     toast_buffer_;
    std::atomic_bool running_{ false };
    std::thread      worker_;
};
