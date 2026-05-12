#include "pch.h"
#include "alert/AlertDispatchThread.h"

AlertDispatchThread::AlertDispatchThread(AlertQueue& alert_queue, ToastBuffer& toast_buffer)
    : alert_queue_(alert_queue)
    , toast_buffer_(toast_buffer)
{
}

AlertDispatchThread::~AlertDispatchThread()
{
    stop();
}

void AlertDispatchThread::start()
{
    if (running_.exchange(true)) {
        return;
    }

    worker_ = std::thread(&AlertDispatchThread::run, this);
}

void AlertDispatchThread::stop()
{
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AlertDispatchThread::run()
{
    while (running_) {
        Alert alert;
        if (!alert_queue_.try_pop(alert)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        show_popup(alert);
    }
}

void AlertDispatchThread::show_popup(const Alert& alert)
{
    const std::string text = alert.title + ": " + alert.message;
    toast_buffer_.post(text, 4000);
}
