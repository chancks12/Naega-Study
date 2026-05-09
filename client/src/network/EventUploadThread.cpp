#include "pch.h"
#include "network/EventUploadThread.h"

#include <chrono>

EventUploadThread::EventUploadThread(EventQueue& event_queue,
                                     IEventClipStore& clip_store,
                                     ILogSink& log_sink)
    : event_queue_(event_queue)
    , clip_store_(clip_store)
    , log_sink_(log_sink)
{
}

EventUploadThread::~EventUploadThread()
{
    stop();
}

void EventUploadThread::start()
{
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&EventUploadThread::run, this);
}

void EventUploadThread::stop()
{
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void EventUploadThread::run()
{
    while (running_) {
        PostureEvent event;
        if (!event_queue_.try_pop(event)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        upload_event(event);
    }
}

void EventUploadThread::upload_event(const PostureEvent& event)
{
    // 로컬 클립 저장 + 메인서버 메타데이터 전송
    const ClipRef clip_ref = clip_store_.store_clip(event);
    log_sink_.append_event_metadata(event, clip_ref);
    log_sink_.flush();

    // 복기 화면 저장소에 추가 (set_review_store가 호출된 경우에만)
    if (review_store_) {
        ReviewEvent re;
        re.event_id     = event.event_id;
        re.timestamp_ms = event.timestamp_ms;
        re.type         = event.type;
        re.clip_dir     = clip_ref.uri;
        re.feedback    = ReviewEvent::Feedback::None;
        review_store_->push(re);
    }
}
