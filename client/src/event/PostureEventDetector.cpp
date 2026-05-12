#include "pch.h"
#include "event/PostureEventDetector.h"

#include <chrono>
#include <string>

namespace {
PostureEventType event_type_from_state(const std::string& state)
{
    if (state == "drowsy")    return PostureEventType::Drowsy;
    if (state == "absent")    return PostureEventType::Absent;
    if (state == "focus")     return PostureEventType::Focus;
    return PostureEventType::BadPosture;
}

std::string reason_from_state(const std::string& previous, const std::string& current)
{
    return "state changed from " + (previous.empty() ? "unknown" : previous) + " to " + current;
}
} // namespace

void PostureEventDetector::set_callback(EventCallback cb)
{
    callback_ = std::move(cb);
}

void PostureEventDetector::feed(const AnalysisResult& result, const EventShadowBuffer& shadow)
{
    // ── post-roll 카운트다운 ────────────────────────────────
    if (pending_.has_value()) {
        --pending_->post_roll_remaining;
        if (pending_->post_roll_remaining <= 0) {
            flush_pending(shadow);
        }
    }

    // ── 상태 기반 이벤트 트리거 ─────────────────────────────
    if (!result.state.empty()) {
        const std::string previous = last_state_;
        if (previous != result.state) {
            last_state_ = result.state;

            if (result.state == "drowsy" || result.state == "distracted" || result.state == "absent") {
                schedule_event(event_type_from_state(result.state),
                               reason_from_state(previous, result.state).c_str(), result);
            } else if (result.state == "focus") {
                event_cooldown_ = false;
                // 비정상 상태에서 복규시만 공부 시작 로그 기록
                // (초기 "" → focus 전환은 서비스 시작이므로 제외)
                if (!previous.empty() && previous != "focus") {
                    schedule_event(PostureEventType::Focus,
                                   reason_from_state(previous, result.state).c_str(), result);
                }
            }
        }
        return;
    }

    // ── 로컈 임계값 기반 이벤트 트리거 (AI 서버 응답 전) ────────
    bad_posture_streak_ = (result.neck_angle > neck_threshold_ || !result.posture_ok) ? bad_posture_streak_ + 1 : 0;
    drowsy_streak_ = (result.ear < ear_threshold_ || result.drowsy) ? drowsy_streak_ + 1 : 0;

    if (event_cooldown_) {
        if (bad_posture_streak_ == 0 && drowsy_streak_ == 0)
            event_cooldown_ = false;
        return;
    }

    if (bad_posture_streak_ >= 5)
        schedule_event(PostureEventType::BadPosture, "neck_angle over threshold", result);
    else if (drowsy_streak_ >= 5)
        schedule_event(PostureEventType::Drowsy, "EAR below threshold", result);
}

void PostureEventDetector::reset_cooldown()
{
    event_cooldown_ = false;
    last_state_.clear();
}

void PostureEventDetector::schedule_event(PostureEventType type, const char* reason, const AnalysisResult& result)
{
    const std::uint64_t ts = result.timestamp_ms > 0
        ? result.timestamp_ms
        : static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count());

    if (last_event_ms_ > 0 && ts - last_event_ms_ < kMinEventIntervalMs)
        return;

    event_cooldown_ = true;
    last_event_ms_  = ts;

    const int fps = camera_fps_.load();
    const int post_roll = fps; // 1초

    pending_ = PendingEvent{ type, ts, reason, result.confidence, post_roll };
}

void PostureEventDetector::flush_pending(const EventShadowBuffer& shadow)
{
    if (!pending_.has_value() || !callback_) {
        pending_.reset();
        return;
    }

    const int fps = camera_fps_.load();
    const std::size_t window = static_cast<std::size_t>(7 * fps);

    PostureEvent event;
    event.type         = pending_->type;
    event.timestamp_ms = pending_->timestamp_ms;
    event.event_id     = "evt-" + std::to_string(event.timestamp_ms);
    event.reason       = pending_->reason;
    event.confidence   = pending_->confidence;
    event.camera_fps   = fps;
    event.frames = shadow.snapshot(window);

    pending_.reset();
    callback_(std::move(event));
}
