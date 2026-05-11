// ============================================================================
// dao.h — StudySync 테이블별 데이터 접근 객체 (DAO)
// ============================================================================
// 목적:
//   StudySync DB(8개 테이블)에 대한 SQL 래퍼.
//   ConnectionPool 에서 PooledConnection(RAII) 으로 커넥션을 빌려쓰고 반납한다.
//
// 매핑:
//   UserDao         → users
//   GoalDao         → goals
//   SessionDao      → sessions
//   FocusLogDao     → focus_logs
//   PostureLogDao   → posture_logs
//   PostureEventDao → posture_events  (event_id UNIQUE — 멱등 업로드)
//   TrainDataDao    → train_data
//   ModelDao        → models
//
// 공통 규칙:
//   - Prepared Statement 만 사용 (SQL Injection 차단).
//   - 실패 시 -1 / 0 / false / 빈 객체 반환 + log_err_db 기록.
//   - 시간 입력은 ISO8601("YYYY-MM-DDTHH:MM:SS...") 또는 MySQL DATETIME 둘 다 받음.
//     dao.cpp 의 iso8601_to_mysql 헬퍼가 자동 변환.
// ============================================================================
#pragma once

#include "storage/connection_pool.h"

#include <cstdint>
#include <string>
#include <vector>

namespace factory {

// ── UserDao — users 테이블 ──
// StudySync 정책: 로그인 키는 email. employee_id/role 같은 공장 컬럼 없음.
class UserDao {
public:
    explicit UserDao(ConnectionPool& pool) : pool_(pool) {}

    struct UserInfo {
        long long   id            = 0;
        std::string email;
        std::string name;
        std::string role;             // "user" 등 — JWT role claim 용
        std::string password_hash;
        bool        found         = false;
    };

    /// 회원가입. 평문 password 는 내부에서 bcrypt 해시로 변환.
    /// role 미지정 시 'user' (스키마 DEFAULT).
    /// 성공 시 신규 user_id, 실패 시 -1 (이메일 중복 포함).
    long long insert(const std::string& email,
                     const std::string& password,
                     const std::string& name,
                     const std::string& role = "user");

    /// email 로 사용자 조회.
    UserInfo find_by_email(const std::string& email);

    /// email 중복 확인.
    bool exists_by_email(const std::string& email);

private:
    ConnectionPool& pool_;
};

// ── GoalDao — goals 테이블 ──
// user_id UNIQUE 라 INSERT ... ON DUPLICATE KEY UPDATE 로 upsert.
class GoalDao {
public:
    explicit GoalDao(ConnectionPool& pool) : pool_(pool) {}

    struct GoalInfo {
        int         daily_goal_min   = 120;   // schema DEFAULT
        int         rest_interval_min = 50;
        int         rest_duration_min = 10;
        std::string updated_at;               // YYYY-MM-DD HH:MM:SS (없으면 빈 문자열)
        bool        found              = false;
    };

    /// 목표 저장/갱신. 성공 시 true.
    bool upsert(long long user_id,
                int daily_goal_min,
                int rest_interval_min,
                int rest_duration_min);

    /// user_id 로 목표 조회. 없으면 found=false (스키마 DEFAULT 값으로 채움).
    GoalInfo find_by_user(long long user_id);

private:
    ConnectionPool& pool_;
};

// ── SessionDao — sessions 테이블 ──
class SessionDao {
public:
    explicit SessionDao(ConnectionPool& pool) : pool_(pool) {}

    struct SessionInfo {
        long long   id           = 0;
        long long   user_id      = 0;
        std::string date;            // YYYY-MM-DD
        std::string start_time;      // YYYY-MM-DD HH:MM:SS
        std::string end_time;        // 진행 중이면 빈 문자열
        double      focus_min     = 0.0;
        double      avg_focus     = 0.0;
        bool        goal_achieved = false;
        bool        found         = false;
    };

    /// 세션 시작. start_time 은 ISO8601 또는 MySQL DATETIME 형식.
    /// date 가 비어있으면 start_time 의 날짜 부분을 사용.
    /// 성공 시 신규 session_id, 실패 시 -1.
    long long start(long long user_id,
                    const std::string& start_time,
                    const std::string& date = "");

    /// 세션 종료. end_time / focus_min / avg_focus / goal_achieved 갱신.
    /// 성공 시 true.
    bool end(long long session_id,
             const std::string& end_time,
             double focus_min,
             double avg_focus,
             bool goal_achieved);

    /// 사용자의 진행 중(end_time IS NULL) 세션 id. 없으면 0.
    long long find_active(long long user_id);

    SessionInfo find_by_id(long long session_id);

    // 세션 종료 시 집계용 헬퍼 — focus_logs 에서 평균/지속 시간 산출.
    // TODO(spec): focus_min 의 정확한 정의(집중 상태 분 vs 전체 분) 미정.
    //   잠정: focus_min = COUNT(state='집중') * 0.2초 / 60.
    //         avg_focus = AVG(focus_score) / 100  (0~1 비율).
    struct AggregateResult {
        double focus_min = 0.0;      // "집중" 으로 분류된 시간 (분)
        double avg_focus = 0.0;      // 0~1 비율
        long long sample_count = 0;  // focus_logs 행 수 (참고용)
    };
    AggregateResult aggregate(long long session_id);

    /// 사용자의 미종료 세션 (end_time IS NULL) 을 강제 마감.
    /// 새 /session/start 호출 시 호출 — 끊긴 이전 세션을 정리.
    /// end_time 은 인자로 전달되는 ISO8601 또는 MySQL DATETIME 사용.
    /// focus_min/avg_focus 는 각 세션 별로 aggregate() 결과로 계산.
    /// @return 마감된 세션 개수
    int force_close_user_open_sessions(long long user_id, const std::string& end_time);

    /// 모든 사용자에 대해 stale 세션(시작 후 N시간 경과 + end_time NULL) 강제 마감.
    /// SessionCleanupWorker 가 주기적으로 호출.
    /// @param stale_hours 시작 후 이 시간 경과한 세션을 stale 로 판단 (기본 6h)
    /// @return 마감된 세션 개수
    int force_close_stale_sessions(int stale_hours);

private:
    ConnectionPool& pool_;
};

// ── FocusLogDao — focus_logs 테이블 ──
// AI 서버에서 5fps 로 푸시되는 집중도 분석 결과 저장.
class FocusLogDao {
public:
    explicit FocusLogDao(ConnectionPool& pool) : pool_(pool) {}

    struct Entry {
        long long   session_id   = 0;
        std::string ts;                 // ISO8601 또는 MySQL DATETIME
        long long   timestamp_ms = 0;   // 에포크 ms (정밀 정렬용)
        int         focus_score  = 0;   // 0~100 (DB CHECK)
        std::string state;              // "focus" / "distracted" / "drowsy" 등 자유
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
    };

    /// 단건 INSERT. 성공 시 row id, 실패 시 -1.
    long long insert(const Entry& e);

    /// 외부 커넥션으로 INSERT (트랜잭션 묶기용). 풀에서 새로 acquire 안 함.
    /// 호출자가 mysql_autocommit / commit / rollback 책임.
    long long insert_in_conn(MYSQL* conn, const Entry& e);

private:
    ConnectionPool& pool_;
};

// ── PostureLogDao — posture_logs 테이블 ──
class PostureLogDao {
public:
    explicit PostureLogDao(ConnectionPool& pool) : pool_(pool) {}

    struct Entry {
        long long   session_id    = 0;
        std::string ts;
        long long   timestamp_ms  = 0;
        // NULL 허용 컬럼은 has_* 플래그로 표현
        bool        has_neck_angle    = false;
        double      neck_angle        = 0.0;
        bool        has_shoulder_diff = false;
        double      shoulder_diff     = 0.0;
        bool        posture_ok    = true;
        bool        has_vs_baseline   = false;
        double      vs_baseline       = 0.0;
    };

    long long insert(const Entry& e);
    long long insert_in_conn(MYSQL* conn, const Entry& e);

private:
    ConnectionPool& pool_;
};

// ── PostureEventDao — posture_events 테이블 ──
// 정책: 영상 본체는 메인서버에 저장하지 않음. 클립 메타데이터만 보관.
//      event_id UNIQUE → 같은 event_id 재업로드 시 ON DUPLICATE KEY 로 멱등.
class PostureEventDao {
public:
    explicit PostureEventDao(ConnectionPool& pool) : pool_(pool) {}

    struct Entry {
        std::string event_id;            // AI 서버 발급 고유 ID (idempotent key)
        long long   session_id    = 0;
        std::string event_type;          // "bad_posture" / "drowsy" / "absent" / "rest_required"
        std::string severity      = "warning"; // "info" / "warning" / "critical"
        std::string reason;              // NULL 가능 → 빈 문자열이면 NULL
        std::string ts;
        long long   timestamp_ms  = 0;

        // 클립 메타 (전부 옵션)
        std::string clip_id;
        std::string clip_access   = "local_only"; // "none" / "local_only" / "uploaded_url"
        std::string clip_ref;
        std::string clip_format;
        int         frame_count   = 0;
        int         retention_days = 3;
        bool        has_expires_at_ms = false;
        long long   expires_at_ms = 0;
    };

    /// 멱등 INSERT. 같은 event_id 가 있으면 기존 row id 반환 (DB 변경 없음).
    /// 신규 INSERT 면 신규 id, 실패 시 -1.
    long long insert(const Entry& e);
    long long insert_in_conn(MYSQL* conn, const Entry& e);

private:
    ConnectionPool& pool_;
};

// ── TrainDataDao — train_data 테이블 ──
// 사용자별 자세 랜드마크 + 라벨 (자세 분류 모델 학습용).
class TrainDataDao {
public:
    explicit TrainDataDao(ConnectionPool& pool) : pool_(pool) {}

    /// landmarks_json 은 호출자가 검증된 JSON 문자열로 전달 (security/json_safety 사용).
    /// label 은 'focus' / 'distracted' / 'drowsy' 만 허용 (DB CHECK).
    long long insert(long long user_id,
                     const std::string& ts,
                     const std::string& landmarks_json,
                     const std::string& label);

    /// 미사용 학습 데이터 개수 (used_for_training=0).
    long long count_unused(long long user_id);

    /// 사용 표시 (학습 완료 시 mark — id 범위 단순 처리).
    bool mark_used(long long user_id, long long max_id);

private:
    ConnectionPool& pool_;
};

// ── ModelDao — models 테이블 ──
// StudySync 스키마: station_id / trained_by 컬럼 없음.
// (model_type, version) UNIQUE → 같은 버전 재등록 차단.
class ModelDao {
public:
    explicit ModelDao(ConnectionPool& pool) : pool_(pool) {}

    struct ModelInfo {
        long long   id          = 0;
        std::string model_type;        // 자세 분류 모델 등 (자유 라벨)
        std::string version;
        double      accuracy    = 0.0;
        std::string file_path;
        bool        is_active   = false;
        std::string created_at;
    };

    /// 모델 등록 + 활성화. 같은 model_type 의 다른 row 들은 is_active=0 으로 내림.
    /// 같은 (model_type, version) 이 이미 있으면 갱신(UPDATE).
    /// 성공 시 row id, 실패 시 -1.
    long long insert(const std::string& model_type,
                     const std::string& version,
                     double accuracy,
                     const std::string& file_path);

    /// 전체 모델 목록 (id DESC).
    std::vector<ModelInfo> list_all();

    /// 특정 model_type 의 활성 모델 (없으면 found=false).
    ModelInfo get_active(const std::string& model_type);

private:
    ConnectionPool& pool_;
};

// ── StatsDao — 통계 집계 (focus_logs / posture_logs / sessions 종합) ──
// 클라 스펙 §3-7 ~ §3-10 응답을 만들기 위한 집계 SQL 모음.
//
// TODO(spec) 다수: 정확한 의미 미정 항목들은 잠정 구현 + 주석으로 표시.
//   - warning_count: 잠정 = posture_logs(posture_ok=0) 의 일별 카운트
//   - focus_min: 잠정 = COUNT(state='집중') * 0.2초 / 60
//   - avg_focus_duration: 잠정 = 세션당 focus_min 평균 (분)
//   - best_hour: 잠정 = 오늘 hour 별 AVG(focus_score) 최대 시간
//   - weekly_avg: 잠정 = 최근 7일 평균 focus_score / 100
class StatsDao {
public:
    explicit StatsDao(ConnectionPool& pool) : pool_(pool) {}

    // GET /stats/today — 오늘 누적
    struct TodayStats {
        double focus_min     = 0.0;   // 분
        double avg_focus     = 0.0;   // 0~1
        long long warning_count = 0;
        double goal_progress = 0.0;   // focus_min / daily_goal_min (0~1+)
    };
    TodayStats get_today(long long user_id, int daily_goal_min);

    // GET /stats/hourly?date=YYYY-MM-DD
    struct HourBucket { int hour; double avg_focus; };
    std::vector<HourBucket> get_hourly(long long user_id, const std::string& date);

    // GET /stats/pattern
    struct PatternStats {
        double avg_focus_duration = 0.0;  // 분
        int    best_hour          = -1;    // 0~23, 데이터 없으면 -1
        double weekly_avg         = 0.0;   // 0~1
    };
    PatternStats get_pattern(long long user_id);

    // GET /stats/weekly — 최근 7일
    struct DayBucket { std::string date; double focus_min; double avg_focus; };
    std::vector<DayBucket> get_weekly(long long user_id);

private:
    ConnectionPool& pool_;
};

} // namespace factory
