// ============================================================================
// router.cpp — StudySync 패킷 라우팅 (protocol_no 별 이벤트 분기)
// ============================================================================
// AI 추론서버 / 학습서버로부터 수신한 패킷을 protocol_no 기준으로 분류하여
// 도메인 EventType 으로 재발행한다. Router 자체는 비즈니스 로직 없음.
//
// 흐름:
//   PacketReader → PACKET_RECEIVED → [Router] → FOCUS_LOG_PUSH_RECEIVED
//                                              → POSTURE_LOG_PUSH_RECEIVED
//                                              → POSTURE_EVENT_PUSH_RECEIVED
//                                              → BASELINE_CAPTURE_RECEIVED
//                                              → TRAIN_PROGRESS_RECEIVED
//                                              → TRAIN_COMPLETE_RECEIVED
//                                              → TRAIN_FAIL_RECEIVED
// ============================================================================
#include "handler/router.h"
#include "Protocol.h"
#include "monitor/connection_registry.h"

#include "core/logger.h"

#include <cstdlib>

namespace factory {

Router::Router(EventBus& bus)
    : event_bus_(bus) {
}

void Router::register_handlers() {
    event_bus_.subscribe(EventType::PACKET_RECEIVED,
                         [this](const std::any& p) { this->on_packet_received(p); });
}

namespace {

// 대부분의 메시지에서 ts 는 ISO8601 또는 epoch ms 둘 다 가능.
// epoch ms 가 별도 필드("timestamp_ms") 로 오면 그대로 long long 으로 추출.
long long extract_ll(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    auto colon = json.find(':', pos);
    if (colon == std::string::npos) return 0;
    return std::strtoll(json.c_str() + colon + 1, nullptr, 10);
}

// 불리언 추출 — JSON 의 true/false 또는 숫자 0/1 모두 수용.
bool extract_bool(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    auto colon = json.find(':', pos);
    if (colon == std::string::npos) return false;
    auto p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size()) return false;
    if (json.compare(p, 4, "true") == 0) return true;
    if (json.compare(p, 5, "false") == 0) return false;
    return std::strtol(json.c_str() + p, nullptr, 10) != 0;
}

// 키 존재 여부 — has_neck_angle 식 NULL 가능 컬럼 매핑용.
bool has_key(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\"") != std::string::npos;
}

} // namespace

void Router::on_packet_received(const std::any& payload) {
    const auto& packet = std::any_cast<const PacketReceivedEvent&>(payload);

    int protocol_no = extract_int(packet.json_payload, "protocol_no");
    auto no = static_cast<ProtocolNo>(protocol_no);

    // ConnectionRegistry server_type 태깅 (HealthChecker 가 사용).
    // StudySync 에선 station_id 분기 없음 — 단일 추론서버 / 학습서버.
    {
        std::string stype;
        switch (no) {
            case ProtocolNo::TRAIN_PROGRESS:
            case ProtocolNo::TRAIN_COMPLETE:
            case ProtocolNo::TRAIN_FAIL:
                stype = "ai_training";
                break;
            case ProtocolNo::HEALTH_PONG: {
                std::string srv = extract_str(packet.json_payload, "server_type");
                stype = !srv.empty() ? srv : "ai_inference";
                break;
            }
            case ProtocolNo::FOCUS_LOG_PUSH:
            case ProtocolNo::POSTURE_LOG_PUSH:
            case ProtocolNo::POSTURE_EVENT_PUSH:
            case ProtocolNo::BASELINE_CAPTURE_PUSH:
                stype = "ai_inference";
                break;
            default:
                break;
        }
        if (!stype.empty() && !packet.remote_addr.empty()) {
            ConnectionRegistry::instance().set_server_type(packet.remote_addr, stype);
        }
    }

    switch (no) {
        // ── 집중도 분석 (5fps) ─────────────────────────────────────────
        case ProtocolNo::FOCUS_LOG_PUSH: {
            FocusLogPushEvent ev{};
            ev.request_id   = extract_str(packet.json_payload, "request_id");
            ev.session_id   = extract_ll (packet.json_payload, "session_id");
            ev.ts           = extract_str(packet.json_payload, "ts");
            ev.timestamp_ms = extract_ll (packet.json_payload, "timestamp_ms");
            ev.focus_score  = extract_int(packet.json_payload, "focus_score");
            ev.state        = extract_str(packet.json_payload, "state");
            ev.is_absent    = extract_bool(packet.json_payload, "is_absent");
            ev.is_drowsy    = extract_bool(packet.json_payload, "is_drowsy");
            ev.has_ear      = has_key(packet.json_payload, "ear");
            ev.ear          = extract_double(packet.json_payload, "ear");
            ev.has_neck_angle = has_key(packet.json_payload, "neck_angle");
            ev.neck_angle   = extract_double(packet.json_payload, "neck_angle");
            ev.has_shoulder_diff = has_key(packet.json_payload, "shoulder_diff");
            ev.shoulder_diff = extract_double(packet.json_payload, "shoulder_diff");
            ev.has_head_yaw = has_key(packet.json_payload, "head_yaw");
            ev.head_yaw     = extract_double(packet.json_payload, "head_yaw");
            ev.has_head_pitch = has_key(packet.json_payload, "head_pitch");
            ev.head_pitch   = extract_double(packet.json_payload, "head_pitch");
            ev.has_face_detected = has_key(packet.json_payload, "face_detected");
            ev.face_detected = extract_int(packet.json_payload, "face_detected");
            ev.has_phone_detected = has_key(packet.json_payload, "phone_detected");
            ev.phone_detected = extract_int(packet.json_payload, "phone_detected");
            ev.sender_addr  = packet.remote_addr;
            if (!event_bus_.publish_critical(EventType::FOCUS_LOG_PUSH_RECEIVED, ev)) {
                log_err_route("FOCUS_LOG_PUSH 큐잉 실패 | req=%s", ev.request_id.c_str());
            }
            break;
        }

        // ── 자세 분석 ───────────────────────────────────────────────
        case ProtocolNo::POSTURE_LOG_PUSH: {
            PostureLogPushEvent ev{};
            ev.request_id        = extract_str(packet.json_payload, "request_id");
            ev.session_id        = extract_ll (packet.json_payload, "session_id");
            ev.ts                = extract_str(packet.json_payload, "ts");
            ev.timestamp_ms      = extract_ll (packet.json_payload, "timestamp_ms");
            ev.has_neck_angle    = has_key   (packet.json_payload, "neck_angle");
            ev.neck_angle        = extract_double(packet.json_payload, "neck_angle");
            ev.has_shoulder_diff = has_key   (packet.json_payload, "shoulder_diff");
            ev.shoulder_diff     = extract_double(packet.json_payload, "shoulder_diff");
            ev.posture_ok        = extract_bool(packet.json_payload, "posture_ok");
            ev.has_vs_baseline   = has_key   (packet.json_payload, "vs_baseline");
            ev.vs_baseline       = extract_double(packet.json_payload, "vs_baseline");
            ev.sender_addr       = packet.remote_addr;
            if (!event_bus_.publish_critical(EventType::POSTURE_LOG_PUSH_RECEIVED, ev)) {
                log_err_route("POSTURE_LOG_PUSH 큐잉 실패 | req=%s", ev.request_id.c_str());
            }
            break;
        }

        // ── 자세 이벤트 (멱등 — event_id 키) ─────────────────────────
        case ProtocolNo::POSTURE_EVENT_PUSH: {
            PostureEventPushEvent ev{};
            ev.event_id      = extract_str(packet.json_payload, "event_id");
            ev.session_id    = extract_ll (packet.json_payload, "session_id");
            ev.event_type    = extract_str(packet.json_payload, "event_type");
            std::string sev  = extract_str(packet.json_payload, "severity");
            if (!sev.empty()) ev.severity = sev;
            ev.reason        = extract_str(packet.json_payload, "reason");
            ev.ts            = extract_str(packet.json_payload, "ts");
            ev.timestamp_ms  = extract_ll (packet.json_payload, "timestamp_ms");
            ev.clip_id       = extract_str(packet.json_payload, "clip_id");
            std::string ca   = extract_str(packet.json_payload, "clip_access");
            if (!ca.empty()) ev.clip_access = ca;
            ev.clip_ref      = extract_str(packet.json_payload, "clip_ref");
            ev.clip_format   = extract_str(packet.json_payload, "clip_format");
            ev.frame_count   = extract_int(packet.json_payload, "frame_count");
            int retention    = extract_int(packet.json_payload, "retention_days");
            if (retention > 0) ev.retention_days = retention;
            if (has_key(packet.json_payload, "expires_at_ms")) {
                ev.has_expires_at_ms = true;
                ev.expires_at_ms     = extract_ll(packet.json_payload, "expires_at_ms");
            }
            ev.sender_addr   = packet.remote_addr;
            if (!event_bus_.publish_critical(EventType::POSTURE_EVENT_PUSH_RECEIVED, ev)) {
                log_err_route("POSTURE_EVENT_PUSH 큐잉 실패 | event_id=%s", ev.event_id.c_str());
            }
            break;
        }

        // ── 기준 자세 캡처 통지 ─────────────────────────────────────
        case ProtocolNo::BASELINE_CAPTURE_PUSH: {
            BaselineCaptureEvent ev{};
            ev.request_id    = extract_str(packet.json_payload, "request_id");
            ev.user_id       = extract_ll (packet.json_payload, "user_id");
            ev.session_id    = extract_ll (packet.json_payload, "session_id");
            ev.ts            = extract_str(packet.json_payload, "ts");
            ev.neck_angle    = extract_double(packet.json_payload, "neck_angle");
            ev.shoulder_diff = extract_double(packet.json_payload, "shoulder_diff");
            ev.sender_addr   = packet.remote_addr;
            if (!event_bus_.publish_critical(EventType::BASELINE_CAPTURE_RECEIVED, ev)) {
                log_err_route("BASELINE_CAPTURE 큐잉 실패 | req=%s", ev.request_id.c_str());
            }
            break;
        }

        // ── 헬스체크 응답 ────────────────────────────────────────────
        case ProtocolNo::HEALTH_PONG:
            break;

        // ── 모델 리로드 응답 ─────────────────────────────────────────
        case ProtocolNo::MODEL_RELOAD_RES:
            log_ai("모델 리로드 응답 수신");
            break;

        // ── 학습 진행률 ─────────────────────────────────────────────
        case ProtocolNo::TRAIN_PROGRESS: {
            TrainProgressEvent ev{};
            ev.request_id  = extract_str(packet.json_payload, "request_id");
            ev.model_type  = extract_str(packet.json_payload, "model_type");
            ev.progress    = extract_int(packet.json_payload, "progress");
            ev.epoch       = extract_int(packet.json_payload, "epoch");
            ev.loss        = extract_double(packet.json_payload, "loss");
            ev.status      = extract_str(packet.json_payload, "status");
            ev.sender_addr = packet.remote_addr;
            event_bus_.publish(EventType::TRAIN_PROGRESS_RECEIVED, ev);
            break;
        }

        // ── 학습 완료 (모델 바이너리 동봉) ────────────────────────────
        case ProtocolNo::TRAIN_COMPLETE: {
            TrainCompleteEvent ev{};
            ev.request_id  = extract_str(packet.json_payload, "request_id");
            ev.model_type  = extract_str(packet.json_payload, "model_type");
            ev.model_path  = extract_str(packet.json_payload, "model_path");
            ev.version     = extract_str(packet.json_payload, "version");
            ev.accuracy    = extract_double(packet.json_payload, "accuracy");
            ev.message     = extract_str(packet.json_payload, "message");
            ev.sender_addr = packet.remote_addr;
            ev.model_bytes = packet.image_bytes;  // 모델 파일 바이너리 = 동봉된 바이트
            if (!event_bus_.publish_critical(EventType::TRAIN_COMPLETE_RECEIVED, ev)) {
                log_err_route("TRAIN_COMPLETE 큐잉 실패 | req=%s", ev.request_id.c_str());
            }
            break;
        }

        // ── 학습 실패 ───────────────────────────────────────────────
        case ProtocolNo::TRAIN_FAIL: {
            TrainFailEvent ev{};
            ev.request_id  = extract_str(packet.json_payload, "request_id");
            ev.model_type  = extract_str(packet.json_payload, "model_type");
            ev.error_code  = extract_str(packet.json_payload, "error_code");
            ev.message     = extract_str(packet.json_payload, "message");
            ev.version     = extract_str(packet.json_payload, "version");
            ev.sender_addr = packet.remote_addr;
            if (!event_bus_.publish_critical(EventType::TRAIN_FAIL_RECEIVED, ev)) {
                log_err_route("TRAIN_FAIL 큐잉 실패 | req=%s", ev.request_id.c_str());
            }
            break;
        }

        default:
            log_err_route("미처리 프로토콜 | no=%d", protocol_no);
            break;
    }
}

// ===========================================================================
// 경량 JSON 필드 추출 — 1-depth 평탄 JSON 전용
// ===========================================================================

std::string Router::extract_str(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    auto colon = json.find(':', pos);
    if (colon == std::string::npos) return "";
    auto first_quote = json.find('"', colon);
    if (first_quote == std::string::npos) return "";
    auto last_quote = json.find('"', first_quote + 1);
    if (last_quote == std::string::npos) return "";
    return json.substr(first_quote + 1, last_quote - first_quote - 1);
}

int Router::extract_int(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    auto colon = json.find(':', pos);
    if (colon == std::string::npos) return 0;
    return static_cast<int>(std::strtol(json.c_str() + colon + 1, nullptr, 10));
}

double Router::extract_double(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0.0;
    auto colon = json.find(':', pos);
    if (colon == std::string::npos) return 0.0;
    return std::strtod(json.c_str() + colon + 1, nullptr);
}

} // namespace factory
