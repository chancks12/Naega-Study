#pragma once
// ============================================================================
// event_types.h — StudySync 이벤트 타입 + 페이로드
// ============================================================================
// 목적:
//   EventBus 에서 사용하는 EventType enum + 각 이벤트의 페이로드 구조체 정의.
//
// 설계:
//   - EventType: 카테고리만, 도메인 데이터는 페이로드에.
//   - 페이로드는 값 타입. EventBus 에 std::any 로 감싸 전달 → 핸들러에서 any_cast.
//
// 이벤트 흐름 (StudySync 도메인):
//
//   [AI 추론서버] --TCP--> TcpListener --> PACKET_RECEIVED
//      └─ Router            --> FOCUS_LOG_PUSH_RECEIVED
//                              POSTURE_LOG_PUSH_RECEIVED
//                              POSTURE_EVENT_PUSH_RECEIVED
//                              BASELINE_CAPTURE_RECEIVED
//                              TRAIN_PROGRESS_RECEIVED / TRAIN_COMPLETE_RECEIVED / TRAIN_FAIL_RECEIVED
//          └─ FocusService / PostureService / PostureEventService / TrainHandler
//                  ─DB INSERT─> ACK_SEND_REQUESTED ─AckSender─> AI 측 ACK
//
//   [HealthChecker] tick → SERVER_DOWN / SERVER_RECOVERED (HTTP 측 푸시는 옵션 B 범위 X)
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>

namespace factory {

// ---------------------------------------------------------------------------
// EventType
// ---------------------------------------------------------------------------
enum class EventType : int {
    // ── TCP 수신 계층 ──
    PACKET_RECEIVED = 0,        // TcpListener → Router 가 구독

    // ── 라우팅 후 (Router 가 protocol_no 로 분류해 재발행) ──
    FOCUS_LOG_PUSH_RECEIVED,    // 1000
    POSTURE_LOG_PUSH_RECEIVED,  // 1002
    POSTURE_EVENT_PUSH_RECEIVED,// 1004
    BASELINE_CAPTURE_RECEIVED,  // 1006

    // ── ACK / 에러 응답 ──
    ACK_SEND_REQUESTED,         // 도메인 서비스 → AckSender (성공/실패 모두 통일)

    // ── 학습 채널 ──
    TRAIN_PROGRESS_RECEIVED,    // 1102
    TRAIN_COMPLETE_RECEIVED,    // 1104
    TRAIN_FAIL_RECEIVED,        // 1106
    MODEL_RELOAD_REQUESTED,     // 학습 완료 후 추론서버 새 모델 송신

    // ── 헬스체크 ──
    HEALTH_CHECK_TICK,
    SERVER_DOWN,
    SERVER_RECOVERED,

    // ── 시스템 ──
    SYSTEM_SHUTDOWN,
};

// ---------------------------------------------------------------------------
// PACKET_RECEIVED — TcpListener 가 발행
// ---------------------------------------------------------------------------
// StudySync 에서 영상 본체는 메인서버에 저장 안 함. 다만 학습 모델 바이너리만
// 동봉(MODEL_RELOAD/TRAIN_COMPLETE)되므로 image_bytes 필드는 그대로 둔다
// (학습 측은 옵션 B 범위에서 그대로 유지).
struct PacketReceivedEvent {
    std::string  json_payload;       // JSON 본문
    std::vector<uint8_t> image_bytes; // 학습 모델 바이너리 등 (도메인 메시지에선 비어 있음)
    std::string  remote_addr;        // 송신자 "IP:PORT"
};

// ---------------------------------------------------------------------------
// AckSendEvent — 도메인 서비스 → AckSender (성공/실패 통일)
// ---------------------------------------------------------------------------
// StudySync 는 클라/AI 측이 모두 "request_id" 로 요청-응답을 매칭한다.
// (옛 공장 코드의 "inspection_id" 명칭 대체)
struct AckSendEvent {
    int         protocol_no = 0;   // 송신할 ACK 번호 (1001/1003/1005/1007 등)
    std::string request_id;        // 요청측이 발급한 매칭 키
    std::string sender_addr;       // 회신 대상 "IP:PORT"
    bool        ack_ok       = true;
    std::string error_message;     // ack_ok=false 일 때 사유
};

// ---------------------------------------------------------------------------
// 도메인 페이로드 — AI 추론서버 → 메인서버 (StudySync)
// ---------------------------------------------------------------------------

// FOCUS_LOG_PUSH (1000) — 5fps 집중도 분석 결과
struct FocusLogPushEvent {
    std::string request_id;        // ACK 매칭 키 (없으면 빈 문자열 → server-side 식별)
    long long   session_id   = 0;
    std::string ts;                 // ISO8601 또는 MySQL DATETIME
    long long   timestamp_ms = 0;
    int         focus_score  = 0;   // 0~100 (DB CHECK)
    std::string state;              // "focus" / "distracted" / "drowsy" 등
    bool        is_absent    = false;
    bool        is_drowsy    = false;
    bool        has_ear = false;
    double      ear = 0.0;
    bool        has_neck_angle = false;
    double      neck_angle = 0.0;
    bool        has_shoulder_diff = false;
    double      shoulder_diff = 0.0;
    bool        has_head_yaw = false;
    double      head_yaw = 0.0;
    bool        has_head_pitch = false;
    double      head_pitch = 0.0;
    bool        has_face_detected = false;
    int         face_detected = 0;
    bool        has_phone_detected = false;
    int         phone_detected = 0;
    std::string sender_addr;
};

// POSTURE_LOG_PUSH (1002)
struct PostureLogPushEvent {
    std::string request_id;
    long long   session_id        = 0;
    std::string ts;
    long long   timestamp_ms      = 0;
    bool        has_neck_angle    = false;
    double      neck_angle        = 0.0;
    bool        has_shoulder_diff = false;
    double      shoulder_diff     = 0.0;
    bool        posture_ok        = true;
    bool        has_vs_baseline   = false;
    double      vs_baseline       = 0.0;
    std::string sender_addr;
};

// POSTURE_EVENT_PUSH (1004) — 멱등 (event_id 가 매칭 키)
struct PostureEventPushEvent {
    std::string event_id;          // ACK 매칭 키이자 멱등 키
    long long   session_id    = 0;
    std::string event_type;        // "bad_posture" / "drowsy" / "absent" / "rest_required"
    std::string severity      = "warning";
    std::string reason;
    std::string ts;
    long long   timestamp_ms  = 0;

    std::string clip_id;
    std::string clip_access   = "local_only";
    std::string clip_ref;
    std::string clip_format;
    int         frame_count       = 0;
    int         retention_days    = 3;
    bool        has_expires_at_ms = false;
    long long   expires_at_ms     = 0;

    std::string sender_addr;
};

// BASELINE_CAPTURE_PUSH (1006)
struct BaselineCaptureEvent {
    std::string request_id;
    long long   user_id       = 0;
    long long   session_id    = 0;
    std::string ts;
    double      neck_angle    = 0.0;
    double      shoulder_diff = 0.0;
    std::string sender_addr;
};

// ---------------------------------------------------------------------------
// 학습 채널 페이로드 (StudySync 정리 — station_id 제거)
// ---------------------------------------------------------------------------

struct TrainProgressEvent {
    std::string request_id;
    std::string model_type;     // 자유 라벨 (예: "focus_classifier")
    int         progress     = 0;  // 0~100%
    int         epoch        = 0;
    double      loss         = 0.0;
    std::string status;
    std::string sender_addr;
};

struct TrainCompleteEvent {
    std::string request_id;
    std::string model_type;
    std::string model_path;     // 학습서버 측 원본 경로 (확장자 추출용)
    std::string version;
    double      accuracy     = 0.0;
    std::string message;
    std::string sender_addr;
    std::vector<uint8_t> model_bytes;  // 모델 파일 바이너리
};

struct TrainFailEvent {
    std::string request_id;
    std::string model_type;
    std::string error_code;
    std::string message;
    std::string version;
    std::string sender_addr;
};

// 학습 완료 후 추론서버에 새 모델 전송 요청 — station_id 제거.
struct ModelReloadEvent {
    std::string model_type;
    std::string model_path;     // 메인서버 로컬 저장 경로
    std::string version;
    std::vector<uint8_t> model_bytes;
};

// ---------------------------------------------------------------------------
// 헬스체크
// ---------------------------------------------------------------------------
struct ServerStatusEvent {
    std::string server_name;   // 예: "ai_inference"
    std::string ip;
    uint16_t    port = 0;
};

} // namespace factory
