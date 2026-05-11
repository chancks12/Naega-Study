// ============================================================================
// focus_service.cpp — FOCUS_LOG_PUSH 처리 (DB INSERT + ACK)
// ============================================================================
#include "service/focus_service.h"
#include "Protocol.h"
#include "core/logger.h"

namespace factory {

FocusService::FocusService(EventBus& bus, ConnectionPool& pool)
    : event_bus_(bus), dao_(pool) {
}

void FocusService::register_handlers() {
    event_bus_.subscribe(EventType::FOCUS_LOG_PUSH_RECEIVED,
                         [this](const std::any& p) { this->on_focus_log_push(p); });
}

void FocusService::on_focus_log_push(const std::any& payload) {
    const auto& ev = std::any_cast<const FocusLogPushEvent&>(payload);

    FocusLogDao::Entry e{};
    e.session_id   = ev.session_id;
    e.ts           = ev.ts;
    e.timestamp_ms = ev.timestamp_ms;
    e.focus_score  = ev.focus_score;
    e.state        = ev.state;
    e.is_absent    = ev.is_absent;
    e.is_drowsy    = ev.is_drowsy;
    e.has_ear      = ev.has_ear;
    e.ear          = ev.ear;
    e.has_neck_angle = ev.has_neck_angle;
    e.neck_angle   = ev.neck_angle;
    e.has_shoulder_diff = ev.has_shoulder_diff;
    e.shoulder_diff = ev.shoulder_diff;
    e.has_head_yaw = ev.has_head_yaw;
    e.head_yaw     = ev.head_yaw;
    e.has_head_pitch = ev.has_head_pitch;
    e.head_pitch   = ev.head_pitch;
    e.has_face_detected = ev.has_face_detected;
    e.face_detected = ev.face_detected;
    e.has_phone_detected = ev.has_phone_detected;
    e.phone_detected = ev.phone_detected;

    long long row_id = dao_.insert(e);

    AckSendEvent ack{};
    ack.protocol_no = static_cast<int>(ProtocolNo::FOCUS_LOG_ACK);
    ack.request_id  = ev.request_id;
    ack.sender_addr = ev.sender_addr;
    ack.ack_ok      = (row_id > 0);
    if (!ack.ack_ok) ack.error_message = "focus_log_insert_failed";
    if (!event_bus_.publish_critical(EventType::ACK_SEND_REQUESTED, ack,
                                     std::chrono::milliseconds(200), true)) {
        log_err_ack("FOCUS_LOG_ACK 큐잉 실패 | req=%s", ack.request_id.c_str());
    }

    if (row_id > 0) {
        log_db("focus_log INSERT | id=%lld session=%lld score=%d state=%s",
               row_id, ev.session_id, ev.focus_score, ev.state.c_str());
    }
}

} // namespace factory
