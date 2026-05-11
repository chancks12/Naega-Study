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
} // namespace

void PostureEventDetector::set_callback(EventCallback cb)
{
    callback_ = std::move(cb);
}

void PostureEventDetector::feed(const AnalysisResult& result, const EventShadowBuffer& shadow)
{
    // ── post-roll 카운트다운 ────────────────────────────────────────
    // pending_ 이 있으면 매 프레임마다 남은 카운트를 줄인다.
    // 0이 되면 현재 shadow 상태로 클립 스냅샷을 찍고 콜백을 호출한다.
    if (pending_.has_value()) {
        --pending_->post_roll_remaining;
        if (pending_->post_roll_remaining <= 0) {
            flush_pending(shadow);
        }
    }

    // ── 상태 기반 이벤트 트리거 ────────────────────────────────────
    if (!result.state.empty()) {
        const std::string previous = last_state_;
        if (previous != result.state) {
            last_state_ = result.state;
            event_cooldown_ = false;

            if (result.state == "drowsy" || result.state == "distracted" || result.state == "absent") {
                const std::string reason = reason_from_state(previous, result.state);
                schedule_event(event_type_from_state(result.state), reason.c_str(), result);
            }
        }
        return;
    }

    // ── 로컬 임계값 기반 이벤트 트리거 ────────────────────────────
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

// ── 이벤트 예약 (post-roll 대기 시작) ─────────────────────────────────────
// 쿨다운을 즉시 설정해 중복 이벤트를 막고, post-roll 카운터를 설정한다.
// 이미 pending_ 이 있으면 덮어쓴다 (최신 상태가 더 관련성 높음).

void PostureEventDetector::schedule_event(PostureEventType type, const char* reason, const AnalysisResult& result)
{
    event_cooldown_ = true;

    const int fps = camera_fps_.load();
    const int post_roll = fps; // 1초

    const std::uint64_t ts = result.timestamp_ms > 0
        ? result.timestamp_ms
        : static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count());

    pending_ = PendingEvent{ type, ts, reason, result.confidence, post_roll };
}

// ── post-roll 완료 → 클립 스냅샷 + 콜백 호출 ──────────────────────────────
// 윈도우 = (1초 pre + 5초 main + 1초 post) * camera_fps = 7 * fps 프레임

void PostureEventDetector::flush_pending(const EventShadowBuffer& shadow)
{
    if (!pending_.has_value() || !callback_) {
        pending_.reset();
        return;
    }

    const int fps = camera_fps_.load();
    const std::size_t window = static_cast<std::size_t>(7 * fps); // 7초치

    PostureEvent event;
    event.type         = pending_->type;
    event.timestamp_ms = pending_->timestamp_ms;
    event.event_id     = "evt-" + std::to_string(event.timestamp_ms);
    event.reason       = pending_->reason;
    event.confidence   = pending_->confidence;
    event.camera_fps   = fps;
    // snapshot()이 내부적으로 clone 처리 — 별도 clone 불필요
    event.frames = shadow.snapshot(window);

    pending_.reset();
    callback_(std::move(event));
}
