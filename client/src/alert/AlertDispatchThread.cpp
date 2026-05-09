#include "pch.h"
#include "alert/AlertDispatchThread.h"

#include <chrono>

namespace {
std::uint64_t now_steady_ms()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
} // namespace

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
    // 1) 좌상단 토스트 (4초)
    const std::string text = alert.title + ": " + alert.message;
    toast_buffer_.post(text, 4000);

    // 2) 졸음 / 자세 불량 → 중앙 휴식 권장 오버레이 (8초)
    if (render_thread_ &&
        (alert.type == AlertType::Drowsy || alert.type == AlertType::BadPosture))
    {
        const std::uint64_t expire = now_steady_ms() + 8000;
        render_thread_->set_break_alert(expire);
    }
}


