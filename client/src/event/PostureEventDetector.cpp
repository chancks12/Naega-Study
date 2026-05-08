#include "pch.h"
#include "event/PostureEventDetector.h"

#include <chrono>
#include <string>

namespace {
PostureEventType event_type_from_state(const std::string& state)
{
    if (state == "drowsy") return PostureEventType::Drowsy;
    if (state == "absent") return PostureEventType::Absent;
    return PostureEventType::BadPosture;
}

std::string reason_from_state(const std::string& previous, const std::string& current)
{
    return "state changed from " + (previous.empty() ? "unknown" : previous) + " to " + current;
}
}

void PostureEventDetector::set_callback(EventCallback cb)
{
    callback_ = std::move(cb);
}

void PostureEventDetector::feed(const AnalysisResult& result, const EventShadowBuffer& shadow)
{
    if (!result.state.empty()) {
        const std::string previous = last_state_;
        if (previous != result.state) {
            last_state_ = result.state;
            event_cooldown_ = false;

            if (result.state == "drowsy" || result.state == "distracted" || result.state == "absent") {
                const std::string reason = reason_from_state(previous, result.state);
                emit_event(event_type_from_state(result.state), reason.c_str(), result, shadow);
            }
        }
        return;
    }

    bad_posture_streak_ = (result.neck_angle > neck_threshold_ || !result.posture_ok) ? bad_posture_streak_ + 1 : 0;
    drowsy_streak_ = (result.ear < ear_threshold_ || result.drowsy) ? drowsy_streak_ + 1 : 0;

    if (event_cooldown_) {
        if (bad_posture_streak_ == 0 && drowsy_streak_ == 0) {
            event_cooldown_ = false;
        }
        return;
    }

    if (bad_posture_streak_ >= 5) {
        emit_event(PostureEventType::BadPosture, "neck_angle over threshold", result, shadow);
        return;
    }

    if (drowsy_streak_ >= 5) {
        emit_event(PostureEventType::Drowsy, "EAR below threshold", result, shadow);
    }
}

void PostureEventDetector::reset_cooldown()
{
    event_cooldown_ = false;
    last_state_.clear();
}

void PostureEventDetector::emit_event(PostureEventType type, const char* reason, const AnalysisResult& result, const EventShadowBuffer& shadow)
{
    event_cooldown_ = true;

    if (!callback_) {
        return;
    }

    PostureEvent event;
    event.type = type;
    event.timestamp_ms = result.timestamp_ms > 0
        ? result.timestamp_ms
        : static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count());
    event.event_id  = "evt-" + std::to_string(event.timestamp_ms);
    event.reason    = reason;
    event.confidence = result.confidence;
    event.frames    = shadow.snapshot(30);
    callback_(std::move(event));
}

