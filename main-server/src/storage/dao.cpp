// ============================================================================
// dao.cpp — StudySync DAO 구현
// ============================================================================
// 책임:
//   StudySync DB 8개 테이블에 대한 SQL 래퍼. Prepared Statement 만 사용.
//   각 메서드는 성공/실패만 반환하고, 실패 사유는 log_err_db 로 기록.
//
// 시간 처리:
//   AI 서버는 ISO8601("2026-05-06T14:30:00.123+09:00") 로 보내지만 MySQL DATETIME
//   은 'T' 구분자/타임존을 거부하므로 iso8601_to_mysql 로 정규화한다.
//   timestamp_ms 는 그대로 BIGINT 로 저장 — 정밀 정렬/만료 계산용.
//
// NULL 처리:
//   - 빈 문자열 → NULL 로 바인딩 (defect_type, reason, clip_* 등)
//   - 숫자 NULL 컬럼은 Entry 구조체의 has_* 플래그로 표현
// ============================================================================
#include "storage/dao.h"
#include "storage/password_hash.h"
#include "core/logger.h"

#include <cstring>
#include <sstream>

namespace factory {

// ---------------------------------------------------------------------------
// 시간 정규화: ISO8601 → MySQL DATETIME
//   "2026-05-06T14:30:00.123+09:00" → "2026-05-06 14:30:00"
//   이미 MySQL 형식이거나 19자 미만이면 그대로 통과 (DB 가 거부하면 에러로).
// ---------------------------------------------------------------------------
static std::string iso8601_to_mysql(const std::string& ts) {
    if (ts.size() < 19) return ts;
    std::string out = ts.substr(0, 19);
    if (out.size() >= 11 && out[10] == 'T') out[10] = ' ';
    return out;
}

// ---------------------------------------------------------------------------
// MYSQL_BIND 헬퍼들 — bind 셋업의 보일러플레이트 축소
// 호출자는 length / null 플래그를 자기 스택에 들고 있어야 함 (포인터만 저장됨).
// ---------------------------------------------------------------------------
static void bind_long(MYSQL_BIND& b, int* v) {
    b.buffer_type = MYSQL_TYPE_LONG;
    b.buffer = v;
}

static void bind_longlong(MYSQL_BIND& b, long long* v) {
    b.buffer_type = MYSQL_TYPE_LONGLONG;
    b.buffer = v;
}

static void bind_double(MYSQL_BIND& b, double* v) {
    b.buffer_type = MYSQL_TYPE_DOUBLE;
    b.buffer = v;
}

static void bind_float(MYSQL_BIND& b, float* v) {
    b.buffer_type = MYSQL_TYPE_FLOAT;
    b.buffer = v;
}

// 문자열 바인딩 (NULL 허용). empty 가 true 이면 NULL 로 바인딩.
//  - len: 호출자 스택에 잡혀 있어야 함 (bind 가 포인터를 들고감)
//  - null: 호출자 스택에 잡혀 있어야 함 (1 바이트)
static void bind_string(MYSQL_BIND& b, const std::string& s,
                        unsigned long& len, my_bool& null,
                        bool nullable_when_empty = false) {
    len  = static_cast<unsigned long>(s.size());
    null = (nullable_when_empty && s.empty()) ? 1 : 0;
    b.buffer_type   = MYSQL_TYPE_STRING;
    b.buffer        = const_cast<char*>(s.c_str());
    b.buffer_length = len;
    b.length        = &len;
    if (nullable_when_empty) b.is_null = &null;
}

// ============================================================================
// UserDao — users 테이블
// ============================================================================

long long UserDao::insert(const std::string& email,
                          const std::string& password,
                          const std::string& name,
                          const std::string& role) {
    if (email.empty() || email.size() > 255) return -1;
    if (password.empty() || password.size() > 128) return -1;
    if (name.empty() || name.size() > 100) return -1;
    if (role.empty() || role.size() > 20) return -1;

    PooledConnection conn(pool_);
    if (!conn.get()) return -1;

    std::string hashed = PasswordHash::hash(password);
    if (hashed.empty()) {
        log_err_db("UserDao 비밀번호 해시 실패 | email=%s", email.c_str());
        return -1;
    }

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return -1;

    const char* sql =
        "INSERT INTO users (email, password_hash, name, role, created_at) "
        "VALUES (?, ?, ?, ?, NOW())";

    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("UserDao insert prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    MYSQL_BIND bind[4];
    std::memset(bind, 0, sizeof(bind));
    unsigned long l_email = 0, l_hash = 0, l_name = 0, l_role = 0;
    my_bool n_email = 0, n_hash = 0, n_name = 0, n_role = 0;
    bind_string(bind[0], email,  l_email, n_email);
    bind_string(bind[1], hashed, l_hash,  n_hash);
    bind_string(bind[2], name,   l_name,  n_name);
    bind_string(bind[3], role,   l_role,  n_role);

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        // ER_DUP_ENTRY (1062) → email UNIQUE 충돌
        unsigned int errno_ = mysql_stmt_errno(stmt);
        log_err_db("UserDao insert execute 실패 | errno=%u %s",
                   errno_, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    long long id = static_cast<long long>(mysql_stmt_insert_id(stmt));
    mysql_stmt_close(stmt);
    log_db("INSERT users | id=%lld email=%s name=%s role=%s",
           id, email.c_str(), name.c_str(), role.c_str());
    return id;
}

UserDao::UserInfo UserDao::find_by_email(const std::string& email) {
    UserInfo info;
    if (email.empty() || email.size() > 255) return info;

    PooledConnection conn(pool_);
    if (!conn.get()) return info;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return info;

    const char* sql = "SELECT id, email, name, role, password_hash FROM users WHERE email=? LIMIT 1";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("UserDao find prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return info;
    }

    MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
    unsigned long l_email = 0; my_bool n_email = 0;
    bind_string(bp[0], email, l_email, n_email);

    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("UserDao find execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return info;
    }

    MYSQL_BIND br[5]; std::memset(br, 0, sizeof(br));
    long long r_id = 0;
    char r_email[256]{}, r_name[128]{}, r_role[32]{}, r_hash[256]{};
    unsigned long l_e = 0, l_n = 0, l_r = 0, l_h = 0;

    bind_longlong(br[0], &r_id);
    br[1].buffer_type = MYSQL_TYPE_STRING; br[1].buffer = r_email;
    br[1].buffer_length = sizeof(r_email); br[1].length = &l_e;
    br[2].buffer_type = MYSQL_TYPE_STRING; br[2].buffer = r_name;
    br[2].buffer_length = sizeof(r_name);  br[2].length = &l_n;
    br[3].buffer_type = MYSQL_TYPE_STRING; br[3].buffer = r_role;
    br[3].buffer_length = sizeof(r_role);  br[3].length = &l_r;
    br[4].buffer_type = MYSQL_TYPE_STRING; br[4].buffer = r_hash;
    br[4].buffer_length = sizeof(r_hash);  br[4].length = &l_h;

    if (mysql_stmt_bind_result(stmt, br) != 0) {
        mysql_stmt_close(stmt);
        return info;
    }

    if (mysql_stmt_fetch(stmt) == 0) {
        info.id    = r_id;
        info.email.assign(r_email, l_e);
        info.name.assign(r_name, l_n);
        info.role.assign(r_role, l_r);
        info.password_hash.assign(r_hash, l_h);
        info.found = true;
    }
    mysql_stmt_close(stmt);
    return info;
}

bool UserDao::exists_by_email(const std::string& email) {
    return find_by_email(email).found;
}

// ============================================================================
// GoalDao — goals 테이블
// ============================================================================

bool GoalDao::upsert(long long user_id,
                     int daily_goal_min,
                     int rest_interval_min,
                     int rest_duration_min) {
    if (user_id <= 0) return false;
    if (daily_goal_min < 0 || rest_interval_min < 0 || rest_duration_min < 0) return false;

    PooledConnection conn(pool_);
    if (!conn.get()) return false;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return false;

    // user_id UNIQUE 라 ON DUPLICATE KEY 로 갱신.
    const char* sql =
        "INSERT INTO goals (user_id, daily_goal_min, rest_interval_min, rest_duration_min) "
        "VALUES (?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE "
        " daily_goal_min=VALUES(daily_goal_min), "
        " rest_interval_min=VALUES(rest_interval_min), "
        " rest_duration_min=VALUES(rest_duration_min)";

    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("GoalDao upsert prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    MYSQL_BIND bind[4]; std::memset(bind, 0, sizeof(bind));
    bind_longlong(bind[0], &user_id);
    bind_long(bind[1], &daily_goal_min);
    bind_long(bind[2], &rest_interval_min);
    bind_long(bind[3], &rest_duration_min);

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("GoalDao upsert execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    mysql_stmt_close(stmt);
    log_db("UPSERT goals | user_id=%lld daily=%d rest_iv=%d rest_dur=%d",
           user_id, daily_goal_min, rest_interval_min, rest_duration_min);
    return true;
}

GoalDao::GoalInfo GoalDao::find_by_user(long long user_id) {
    GoalInfo info;
    if (user_id <= 0) return info;

    PooledConnection conn(pool_);
    if (!conn.get()) return info;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return info;

    const char* sql =
        "SELECT daily_goal_min, rest_interval_min, rest_duration_min, "
        "       DATE_FORMAT(updated_at, '%Y-%m-%d %H:%i:%s') "
        "FROM goals WHERE user_id=? LIMIT 1";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        return info;
    }

    MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
    bind_longlong(bp[0], &user_id);

    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return info;
    }

    MYSQL_BIND br[4]; std::memset(br, 0, sizeof(br));
    int v_daily = 0, v_rest_iv = 0, v_rest_dur = 0;
    char r_updated[32]{}; unsigned long l_updated = 0;
    bind_long(br[0], &v_daily);
    bind_long(br[1], &v_rest_iv);
    bind_long(br[2], &v_rest_dur);
    br[3].buffer_type = MYSQL_TYPE_STRING; br[3].buffer = r_updated;
    br[3].buffer_length = sizeof(r_updated); br[3].length = &l_updated;

    mysql_stmt_bind_result(stmt, br);
    if (mysql_stmt_fetch(stmt) == 0) {
        info.daily_goal_min    = v_daily;
        info.rest_interval_min = v_rest_iv;
        info.rest_duration_min = v_rest_dur;
        info.updated_at.assign(r_updated, l_updated);
        info.found = true;
    }
    mysql_stmt_close(stmt);
    return info;
}

// ============================================================================
// SessionDao — sessions 테이블
// ============================================================================

long long SessionDao::start(long long user_id,
                            const std::string& start_time,
                            const std::string& date) {
    if (user_id <= 0) return -1;
    if (start_time.empty()) return -1;

    std::string ts_mysql = iso8601_to_mysql(start_time);
    // date 미지정 시 start_time 의 날짜 부분 사용
    std::string date_str = date.empty() ? ts_mysql.substr(0, 10) : date;
    if (date_str.size() != 10) return -1;

    PooledConnection conn(pool_);
    if (!conn.get()) return -1;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return -1;

    const char* sql =
        "INSERT INTO sessions (user_id, date, start_time) VALUES (?, ?, ?)";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("SessionDao start prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    MYSQL_BIND bind[3]; std::memset(bind, 0, sizeof(bind));
    unsigned long l_date = 0, l_ts = 0; my_bool n_date = 0, n_ts = 0;
    bind_longlong(bind[0], &user_id);
    bind_string(bind[1], date_str, l_date, n_date);
    bind_string(bind[2], ts_mysql, l_ts,   n_ts);

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("SessionDao start execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    long long id = static_cast<long long>(mysql_stmt_insert_id(stmt));
    mysql_stmt_close(stmt);
    log_db("INSERT sessions | id=%lld user_id=%lld date=%s start=%s",
           id, user_id, date_str.c_str(), ts_mysql.c_str());
    return id;
}

bool SessionDao::end(long long session_id,
                     const std::string& end_time,
                     double focus_min,
                     double avg_focus,
                     bool goal_achieved) {
    if (session_id <= 0) return false;
    if (end_time.empty()) return false;

    std::string ts_mysql = iso8601_to_mysql(end_time);

    PooledConnection conn(pool_);
    if (!conn.get()) return false;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return false;

    const char* sql =
        "UPDATE sessions SET end_time=?, focus_min=?, avg_focus=?, goal_achieved=? "
        "WHERE id=?";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("SessionDao end prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    MYSQL_BIND bind[5]; std::memset(bind, 0, sizeof(bind));
    unsigned long l_ts = 0; my_bool n_ts = 0;
    float v_focus_min = static_cast<float>(focus_min);
    float v_avg_focus = static_cast<float>(avg_focus);
    int   v_goal      = goal_achieved ? 1 : 0;

    bind_string(bind[0], ts_mysql, l_ts, n_ts);
    bind_float(bind[1], &v_focus_min);
    bind_float(bind[2], &v_avg_focus);
    bind_long (bind[3], &v_goal);
    bind_longlong(bind[4], &session_id);

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("SessionDao end execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    mysql_stmt_close(stmt);
    log_db("UPDATE sessions(end) | id=%lld end=%s focus_min=%.1f avg=%.1f goal=%d",
           session_id, ts_mysql.c_str(), focus_min, avg_focus, v_goal);
    return true;
}

long long SessionDao::find_active(long long user_id) {
    if (user_id <= 0) return 0;

    PooledConnection conn(pool_);
    if (!conn.get()) return 0;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return 0;

    // 가장 최근 시작 + 종료 미설정 1건
    const char* sql =
        "SELECT id FROM sessions WHERE user_id=? AND end_time IS NULL "
        "ORDER BY start_time DESC LIMIT 1";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        return 0;
    }

    MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
    bind_longlong(bp[0], &user_id);
    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return 0;
    }

    MYSQL_BIND br[1]; std::memset(br, 0, sizeof(br));
    long long sid = 0;
    bind_longlong(br[0], &sid);
    mysql_stmt_bind_result(stmt, br);
    long long out = (mysql_stmt_fetch(stmt) == 0) ? sid : 0;
    mysql_stmt_close(stmt);
    return out;
}

SessionDao::SessionInfo SessionDao::find_by_id(long long session_id) {
    SessionInfo info;
    if (session_id <= 0) return info;

    PooledConnection conn(pool_);
    if (!conn.get()) return info;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return info;

    const char* sql =
        "SELECT id, user_id, date, start_time, end_time, focus_min, avg_focus, goal_achieved "
        "FROM sessions WHERE id=? LIMIT 1";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        return info;
    }

    MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
    bind_longlong(bp[0], &session_id);
    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return info;
    }

    MYSQL_BIND br[8]; std::memset(br, 0, sizeof(br));
    long long r_id = 0, r_uid = 0;
    char r_date[16]{}, r_start[32]{}, r_end[32]{};
    unsigned long l_date = 0, l_start = 0, l_end = 0;
    my_bool null_end = 0;
    float r_focus = 0, r_avg = 0;
    int r_goal = 0;

    bind_longlong(br[0], &r_id);
    bind_longlong(br[1], &r_uid);
    br[2].buffer_type = MYSQL_TYPE_STRING; br[2].buffer = r_date;
    br[2].buffer_length = sizeof(r_date);  br[2].length = &l_date;
    br[3].buffer_type = MYSQL_TYPE_STRING; br[3].buffer = r_start;
    br[3].buffer_length = sizeof(r_start); br[3].length = &l_start;
    br[4].buffer_type = MYSQL_TYPE_STRING; br[4].buffer = r_end;
    br[4].buffer_length = sizeof(r_end);   br[4].length = &l_end;
    br[4].is_null = &null_end;
    bind_float(br[5], &r_focus);
    bind_float(br[6], &r_avg);
    bind_long (br[7], &r_goal);

    mysql_stmt_bind_result(stmt, br);
    if (mysql_stmt_fetch(stmt) == 0) {
        info.id      = r_id;
        info.user_id = r_uid;
        info.date.assign(r_date, l_date);
        info.start_time.assign(r_start, l_start);
        info.end_time = null_end ? "" : std::string(r_end, l_end);
        info.focus_min     = r_focus;
        info.avg_focus     = r_avg;
        info.goal_achieved = (r_goal != 0);
        info.found = true;
    }
    mysql_stmt_close(stmt);
    return info;
}

SessionDao::AggregateResult SessionDao::aggregate(long long session_id) {
    AggregateResult r;
    if (session_id <= 0) return r;

    PooledConnection conn(pool_);
    if (!conn.get()) return r;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return r;

    // 합의: state 값은 영어 'focus'/'distracted'/'drowsy' (ai_client_tcp_protocol.md §6).
    // focus_min 잠정 정의: state='focus' 행 수 × 0.2초 / 60 (5fps 가정).
    // AVG(focus_score) 는 0~100 → /100 으로 0~1 변환은 호출자가 처리.
    const char* sql =
        "SELECT "
        "  COUNT(*) AS sample_count,"
        "  AVG(focus_score) AS avg_score,"
        "  SUM(CASE WHEN state = 'focus' THEN 1 ELSE 0 END) AS focused_rows "
        "FROM focus_logs WHERE session_id=?";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("SessionDao aggregate prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return r;
    }

    MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
    bind_longlong(bp[0], &session_id);
    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return r;
    }

    MYSQL_BIND br[3]; std::memset(br, 0, sizeof(br));
    long long sample_cnt = 0, focused = 0;
    double avg_raw = 0.0;
    my_bool null_avg = 0;
    bind_longlong(br[0], &sample_cnt);
    bind_double  (br[1], &avg_raw); br[1].is_null = &null_avg;
    bind_longlong(br[2], &focused);

    mysql_stmt_bind_result(stmt, br);
    if (mysql_stmt_fetch(stmt) == 0) {
        r.sample_count = sample_cnt;
        r.avg_focus    = null_avg ? 0.0 : (avg_raw / 100.0);   // 0~1 비율
        r.focus_min    = static_cast<double>(focused) * 0.2 / 60.0;
    }
    mysql_stmt_close(stmt);
    return r;
}

// ---------------------------------------------------------------------------
// force_close_user_open_sessions — 새 /session/start 호출 시 같은 user 의
// 미종료 세션 (end_time IS NULL) 들을 강제 마감.
// 클라가 비정상 종료되어 이전 세션이 매달려 있는 경우 정리용.
// 마감 시 focus_logs / posture_logs 의 데이터로 집계해서 채움.
// ---------------------------------------------------------------------------
int SessionDao::force_close_user_open_sessions(long long user_id,
                                                const std::string& end_time) {
    if (user_id <= 0 || end_time.empty()) return 0;

    PooledConnection conn(pool_);
    if (!conn.get()) return 0;

    // 1) 미종료 세션 ID 들 수집
    std::vector<long long> orphan_ids;
    {
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        if (!stmt) return 0;
        const char* sel = "SELECT id FROM sessions WHERE user_id=? AND end_time IS NULL";
        if (mysql_stmt_prepare(stmt, sel, std::strlen(sel)) == 0) {
            MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
            bind_longlong(bp[0], &user_id);
            if (mysql_stmt_bind_param(stmt, bp) == 0 && mysql_stmt_execute(stmt) == 0) {
                MYSQL_BIND br[1]; std::memset(br, 0, sizeof(br));
                long long id = 0;
                bind_longlong(br[0], &id);
                mysql_stmt_bind_result(stmt, br);
                while (mysql_stmt_fetch(stmt) == 0) orphan_ids.push_back(id);
            }
        }
        mysql_stmt_close(stmt);
    }

    if (orphan_ids.empty()) return 0;

    // 2) 각 세션에 대해 집계 + UPDATE
    std::string ts_mysql = iso8601_to_mysql(end_time);
    int closed = 0;
    for (long long sid : orphan_ids) {
        AggregateResult agg = aggregate(sid);
        // goal_achieved 잠정: focus_min >= 1 분 이상이면 진행됐다고 봄.
        // (정확한 정의는 SessionService 가 GoalDao 와 비교하는 건데, 자동 마감은
        //  단순 처리 — 별도 cron 정리 수준이라 0 으로 둠)
        bool goal_achieved = false;
        if (end(sid, ts_mysql, agg.focus_min, agg.avg_focus, goal_achieved)) {
            ++closed;
        }
    }
    if (closed > 0) {
        log_main("force_close_user_open_sessions | user=%lld closed=%d",
                 user_id, closed);
    }
    return closed;
}

// ---------------------------------------------------------------------------
// force_close_stale_sessions — SessionCleanupWorker 가 주기 호출.
// start_time 후 stale_hours 시간 경과 + end_time NULL 인 세션 전체 강제 마감.
// ---------------------------------------------------------------------------
int SessionDao::force_close_stale_sessions(int stale_hours) {
    if (stale_hours <= 0) stale_hours = 6;

    PooledConnection conn(pool_);
    if (!conn.get()) return 0;

    // stale 세션 ID 수집
    std::vector<long long> stale_ids;
    {
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        if (!stmt) return 0;
        const char* sel =
            "SELECT id FROM sessions "
            "WHERE end_time IS NULL AND start_time < NOW() - INTERVAL ? HOUR";
        if (mysql_stmt_prepare(stmt, sel, std::strlen(sel)) == 0) {
            MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
            bind_long(bp[0], &stale_hours);
            if (mysql_stmt_bind_param(stmt, bp) == 0 && mysql_stmt_execute(stmt) == 0) {
                MYSQL_BIND br[1]; std::memset(br, 0, sizeof(br));
                long long id = 0;
                bind_longlong(br[0], &id);
                mysql_stmt_bind_result(stmt, br);
                while (mysql_stmt_fetch(stmt) == 0) stale_ids.push_back(id);
            }
        }
        mysql_stmt_close(stmt);
    }

    if (stale_ids.empty()) return 0;

    // 각 세션 마감 — end_time = NOW() (세션 길이 stale_hours 초과라 정확한 종료 시각 모름).
    // 더 정밀하게는 마지막 focus_log ts 사용 가능하나 비용 큼.
    int closed = 0;
    for (long long sid : stale_ids) {
        AggregateResult agg = aggregate(sid);
        // end() 가 ISO/DATETIME 둘 다 받으니 NOW() 형식 그대로 패스
        std::time_t now = std::time(nullptr);
        std::tm tm{}; localtime_r(&now, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        if (end(sid, std::string(buf), agg.focus_min, agg.avg_focus, /*goal_achieved=*/false)) {
            ++closed;
        }
    }
    log_main("force_close_stale_sessions | stale_hours=%d closed=%d",
             stale_hours, closed);
    return closed;
}

// ============================================================================
// FocusLogDao — focus_logs 테이블
// ============================================================================

long long FocusLogDao::insert(const Entry& e) {
    PooledConnection conn(pool_);
    if (!conn.get()) return -1;
    return insert_in_conn(conn, e);
}

long long FocusLogDao::insert_in_conn(MYSQL* conn, const Entry& e) {
    if (!conn) return -1;
    if (e.session_id <= 0) return -1;
    if (e.focus_score < 0 || e.focus_score > 100) return -1;
    if (e.ts.empty()) return -1;

    std::string ts_mysql = iso8601_to_mysql(e.ts);

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return -1;

    const char* sql =
        "INSERT INTO focus_logs "
        "(session_id, ts, timestamp_ms, focus_score, state, is_absent, is_drowsy, "
        "ear, neck_angle, shoulder_diff, head_yaw, head_pitch, face_detected, phone_detected) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("FocusLogDao prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    MYSQL_BIND bind[14]; std::memset(bind, 0, sizeof(bind));
    long long  v_sid   = e.session_id;
    long long  v_ts_ms = e.timestamp_ms;
    int        v_score = e.focus_score;
    int        v_absent = e.is_absent ? 1 : 0;
    int        v_drowsy = e.is_drowsy ? 1 : 0;
    float      v_ear = static_cast<float>(e.ear);
    float      v_neck = static_cast<float>(e.neck_angle);
    float      v_shoulder = static_cast<float>(e.shoulder_diff);
    float      v_yaw = static_cast<float>(e.head_yaw);
    float      v_pitch = static_cast<float>(e.head_pitch);
    int        v_face = e.face_detected;
    int        v_phone = e.phone_detected;
    unsigned long l_ts = 0, l_state = 0; my_bool n_ts = 0, n_state = 0;
    my_bool null_ts_ms = (e.timestamp_ms == 0) ? 1 : 0;
    my_bool null_ear = e.has_ear ? 0 : 1;
    my_bool null_neck = e.has_neck_angle ? 0 : 1;
    my_bool null_shoulder = e.has_shoulder_diff ? 0 : 1;
    my_bool null_yaw = e.has_head_yaw ? 0 : 1;
    my_bool null_pitch = e.has_head_pitch ? 0 : 1;
    my_bool null_face = e.has_face_detected ? 0 : 1;
    my_bool null_phone = e.has_phone_detected ? 0 : 1;

    bind_longlong(bind[0], &v_sid);
    bind_string(bind[1], ts_mysql, l_ts, n_ts);

    bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[2].buffer      = &v_ts_ms;
    bind[2].is_null     = &null_ts_ms;

    bind_long(bind[3], &v_score);
    bind_string(bind[4], e.state, l_state, n_state, /*nullable_when_empty=*/true);
    bind_long(bind[5], &v_absent);
    bind_long(bind[6], &v_drowsy);

    bind[7].buffer_type = MYSQL_TYPE_FLOAT;
    bind[7].buffer = &v_ear; bind[7].is_null = &null_ear;

    bind[8].buffer_type = MYSQL_TYPE_FLOAT;
    bind[8].buffer = &v_neck; bind[8].is_null = &null_neck;

    bind[9].buffer_type = MYSQL_TYPE_FLOAT;
    bind[9].buffer = &v_shoulder; bind[9].is_null = &null_shoulder;

    bind[10].buffer_type = MYSQL_TYPE_FLOAT;
    bind[10].buffer = &v_yaw; bind[10].is_null = &null_yaw;

    bind[11].buffer_type = MYSQL_TYPE_FLOAT;
    bind[11].buffer = &v_pitch; bind[11].is_null = &null_pitch;

    bind_long(bind[12], &v_face);
    bind[12].is_null = &null_face;

    bind_long(bind[13], &v_phone);
    bind[13].is_null = &null_phone;

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("FocusLogDao execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    long long id = static_cast<long long>(mysql_stmt_insert_id(stmt));
    mysql_stmt_close(stmt);
    return id;
}

// ============================================================================
// PostureLogDao — posture_logs 테이블
// ============================================================================

long long PostureLogDao::insert(const Entry& e) {
    PooledConnection conn(pool_);
    if (!conn.get()) return -1;
    return insert_in_conn(conn, e);
}

long long PostureLogDao::insert_in_conn(MYSQL* conn, const Entry& e) {
    if (!conn) return -1;
    if (e.session_id <= 0) return -1;
    if (e.ts.empty()) return -1;

    std::string ts_mysql = iso8601_to_mysql(e.ts);

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return -1;

    const char* sql =
        "INSERT INTO posture_logs "
        "(session_id, ts, timestamp_ms, neck_angle, shoulder_diff, posture_ok, vs_baseline) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("PostureLogDao prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    MYSQL_BIND bind[7]; std::memset(bind, 0, sizeof(bind));
    long long v_sid   = e.session_id;
    long long v_ts_ms = e.timestamp_ms;
    float v_neck = static_cast<float>(e.neck_angle);
    float v_sh   = static_cast<float>(e.shoulder_diff);
    float v_vsb  = static_cast<float>(e.vs_baseline);
    int   v_ok   = e.posture_ok ? 1 : 0;
    unsigned long l_ts = 0; my_bool n_ts = 0;
    my_bool null_ts_ms = (e.timestamp_ms == 0) ? 1 : 0;
    my_bool null_neck = e.has_neck_angle ? 0 : 1;
    my_bool null_sh   = e.has_shoulder_diff ? 0 : 1;
    my_bool null_vsb  = e.has_vs_baseline ? 0 : 1;

    bind_longlong(bind[0], &v_sid);
    bind_string(bind[1], ts_mysql, l_ts, n_ts);

    bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[2].buffer = &v_ts_ms; bind[2].is_null = &null_ts_ms;

    bind[3].buffer_type = MYSQL_TYPE_FLOAT;
    bind[3].buffer = &v_neck; bind[3].is_null = &null_neck;

    bind[4].buffer_type = MYSQL_TYPE_FLOAT;
    bind[4].buffer = &v_sh; bind[4].is_null = &null_sh;

    bind_long(bind[5], &v_ok);

    bind[6].buffer_type = MYSQL_TYPE_FLOAT;
    bind[6].buffer = &v_vsb; bind[6].is_null = &null_vsb;

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("PostureLogDao execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    long long id = static_cast<long long>(mysql_stmt_insert_id(stmt));
    mysql_stmt_close(stmt);
    return id;
}

// ============================================================================
// PostureEventDao — posture_events 테이블 (멱등 INSERT)
// ============================================================================

long long PostureEventDao::insert(const Entry& e) {
    PooledConnection conn(pool_);
    if (!conn.get()) return -1;
    return insert_in_conn(conn, e);
}

long long PostureEventDao::insert_in_conn(MYSQL* conn, const Entry& e) {
    if (!conn) return -1;
    if (e.event_id.empty() || e.event_id.size() > 128) return -1;
    if (e.session_id <= 0) return -1;
    if (e.event_type.empty()) return -1;
    if (e.ts.empty()) return -1;

    std::string ts_mysql = iso8601_to_mysql(e.ts);

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return -1;

    // event_id UNIQUE → 같은 ID 면 ON DUPLICATE KEY 로 NOOP (id=id 갱신).
    // mysql_stmt_insert_id 는 신규 INSERT 면 신규 id, 중복이면 0 반환.
    // 0 이면 별도 SELECT 로 기존 id 조회.
    const char* sql =
        "INSERT INTO posture_events "
        "(event_id, session_id, event_type, severity, reason, ts, timestamp_ms, "
        " clip_id, clip_access, clip_ref, clip_format, frame_count, retention_days, expires_at_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON DUPLICATE KEY UPDATE id=id";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("PostureEventDao prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    MYSQL_BIND bind[14]; std::memset(bind, 0, sizeof(bind));
    long long v_sid     = e.session_id;
    long long v_ts_ms   = e.timestamp_ms;
    long long v_exp_ms  = e.expires_at_ms;
    int       v_frames  = e.frame_count;
    int       v_retain  = e.retention_days;
    unsigned long l_eid = 0, l_etype = 0, l_sev = 0, l_reason = 0, l_ts = 0;
    unsigned long l_cid = 0, l_caccess = 0, l_cref = 0, l_cfmt = 0;
    my_bool n_eid = 0, n_etype = 0, n_sev = 0, n_reason = 0, n_ts = 0;
    my_bool n_cid = 0, n_caccess = 0, n_cref = 0, n_cfmt = 0;
    my_bool null_exp_ms = e.has_expires_at_ms ? 0 : 1;

    bind_string(bind[0], e.event_id,    l_eid,    n_eid);
    bind_longlong(bind[1], &v_sid);
    bind_string(bind[2], e.event_type,  l_etype,  n_etype);
    bind_string(bind[3], e.severity,    l_sev,    n_sev);
    bind_string(bind[4], e.reason,      l_reason, n_reason, /*nullable_when_empty=*/true);
    bind_string(bind[5], ts_mysql,      l_ts,     n_ts);

    bind[6].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[6].buffer = &v_ts_ms;

    bind_string(bind[7],  e.clip_id,     l_cid,     n_cid,     true);
    bind_string(bind[8],  e.clip_access, l_caccess, n_caccess);
    bind_string(bind[9],  e.clip_ref,    l_cref,    n_cref,    true);
    bind_string(bind[10], e.clip_format, l_cfmt,    n_cfmt,    true);

    bind_long(bind[11], &v_frames);
    bind_long(bind[12], &v_retain);

    bind[13].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[13].buffer = &v_exp_ms; bind[13].is_null = &null_exp_ms;

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("PostureEventDao execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    long long id = static_cast<long long>(mysql_stmt_insert_id(stmt));
    mysql_stmt_close(stmt);

    // 신규 INSERT → insert_id > 0. 중복(ON DUPLICATE) → insert_id == 0 → 기존 id 조회.
    if (id == 0) {
        MYSQL_STMT* q = mysql_stmt_init(conn);
        if (!q) return -1;
        const char* sel = "SELECT id FROM posture_events WHERE event_id=? LIMIT 1";
        if (mysql_stmt_prepare(q, sel, std::strlen(sel)) == 0) {
            MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
            unsigned long l = 0; my_bool n = 0;
            bind_string(bp[0], e.event_id, l, n);
            if (mysql_stmt_bind_param(q, bp) == 0 && mysql_stmt_execute(q) == 0) {
                MYSQL_BIND br[1]; std::memset(br, 0, sizeof(br));
                long long existing = 0;
                bind_longlong(br[0], &existing);
                mysql_stmt_bind_result(q, br);
                if (mysql_stmt_fetch(q) == 0) id = existing;
            }
        }
        mysql_stmt_close(q);
        log_db("posture_events 중복 (멱등) | event_id=%s id=%lld", e.event_id.c_str(), id);
    } else {
        log_db("INSERT posture_events | id=%lld event_id=%s type=%s",
               id, e.event_id.c_str(), e.event_type.c_str());
    }
    return id;
}

// ============================================================================
// TrainDataDao — train_data 테이블
// ============================================================================

long long TrainDataDao::insert(long long user_id,
                               const std::string& ts,
                               const std::string& landmarks_json,
                               const std::string& label) {
    if (user_id <= 0) return -1;
    if (ts.empty()) return -1;
    if (landmarks_json.empty()) return -1;
    if (label != "focus" && label != "distracted" && label != "drowsy") return -1;

    std::string ts_mysql = iso8601_to_mysql(ts);

    PooledConnection conn(pool_);
    if (!conn.get()) return -1;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return -1;

    const char* sql =
        "INSERT INTO train_data (user_id, ts, landmarks_json, label) VALUES (?,?,?,?)";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("TrainDataDao prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    MYSQL_BIND bind[4]; std::memset(bind, 0, sizeof(bind));
    unsigned long l_ts = 0, l_json = 0, l_label = 0;
    my_bool n_ts = 0, n_json = 0, n_label = 0;
    bind_longlong(bind[0], &user_id);
    bind_string(bind[1], ts_mysql,       l_ts,    n_ts);
    bind_string(bind[2], landmarks_json, l_json,  n_json);
    bind_string(bind[3], label,          l_label, n_label);

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("TrainDataDao execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    long long id = static_cast<long long>(mysql_stmt_insert_id(stmt));
    mysql_stmt_close(stmt);
    return id;
}

long long TrainDataDao::count_unused(long long user_id) {
    if (user_id <= 0) return 0;

    PooledConnection conn(pool_);
    if (!conn.get()) return 0;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return 0;

    const char* sql =
        "SELECT COUNT(*) FROM train_data WHERE user_id=? AND used_for_training=0";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        return 0;
    }

    MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
    bind_longlong(bp[0], &user_id);
    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return 0;
    }

    MYSQL_BIND br[1]; std::memset(br, 0, sizeof(br));
    long long cnt = 0;
    bind_longlong(br[0], &cnt);
    mysql_stmt_bind_result(stmt, br);
    long long out = (mysql_stmt_fetch(stmt) == 0) ? cnt : 0;
    mysql_stmt_close(stmt);
    return out;
}

bool TrainDataDao::mark_used(long long user_id, long long max_id) {
    if (user_id <= 0 || max_id <= 0) return false;

    PooledConnection conn(pool_);
    if (!conn.get()) return false;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return false;

    const char* sql =
        "UPDATE train_data SET used_for_training=1 "
        "WHERE user_id=? AND id<=? AND used_for_training=0";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("TrainDataDao mark prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    MYSQL_BIND bind[2]; std::memset(bind, 0, sizeof(bind));
    bind_longlong(bind[0], &user_id);
    bind_longlong(bind[1], &max_id);

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("TrainDataDao mark execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }

    my_ulonglong affected = mysql_stmt_affected_rows(stmt);
    mysql_stmt_close(stmt);
    log_db("UPDATE train_data(used_for_training=1) | user_id=%lld max_id=%lld affected=%llu",
           user_id, max_id, static_cast<unsigned long long>(affected));
    return true;
}

// ============================================================================
// ModelDao — models 테이블 (StudySync 단순화 버전)
// ============================================================================

long long ModelDao::insert(const std::string& model_type,
                           const std::string& version,
                           double accuracy,
                           const std::string& file_path) {
    if (model_type.empty() || model_type.size() > 50) return -1;
    if (version.empty() || version.size() > 50) return -1;

    PooledConnection conn(pool_);
    if (!conn.get()) return -1;

    // 같은 model_type 의 기존 활성 모델을 비활성화 (트랜잭션 없이 best-effort).
    {
        MYSQL_STMT* deact = mysql_stmt_init(conn);
        if (deact) {
            const char* upd = "UPDATE models SET is_active=0 WHERE model_type=? AND is_active=1";
            if (mysql_stmt_prepare(deact, upd, std::strlen(upd)) == 0) {
                MYSQL_BIND b[1]; std::memset(b, 0, sizeof(b));
                unsigned long l = 0; my_bool n = 0;
                bind_string(b[0], model_type, l, n);
                if (mysql_stmt_bind_param(deact, b) == 0) {
                    mysql_stmt_execute(deact);
                }
            }
            mysql_stmt_close(deact);
        }
    }

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return -1;

    // (model_type, version) UNIQUE → 같은 버전 재등록 시 ON DUPLICATE 로 갱신.
    const char* sql =
        "INSERT INTO models (model_type, version, accuracy, file_path, is_active) "
        "VALUES (?, ?, ?, ?, 1) "
        "ON DUPLICATE KEY UPDATE accuracy=VALUES(accuracy), file_path=VALUES(file_path), is_active=1";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("ModelDao prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    MYSQL_BIND bind[4]; std::memset(bind, 0, sizeof(bind));
    unsigned long l_mt = 0, l_ver = 0, l_path = 0;
    my_bool n_mt = 0, n_ver = 0, n_path = 0;
    float v_acc = static_cast<float>(accuracy);

    bind_string(bind[0], model_type, l_mt,  n_mt);
    bind_string(bind[1], version,    l_ver, n_ver);
    bind_float(bind[2], &v_acc);
    bind_string(bind[3], file_path,  l_path, n_path, /*nullable_when_empty=*/true);

    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0) {
        log_err_db("ModelDao execute 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    long long id = static_cast<long long>(mysql_stmt_insert_id(stmt));
    mysql_stmt_close(stmt);

    // ON DUPLICATE 갱신 시 insert_id == 0 → 기존 id 조회
    if (id == 0) {
        ModelInfo existing = get_active(model_type);
        if (existing.id > 0 && existing.version == version) id = existing.id;
    }

    log_db("UPSERT models | id=%lld type=%s version=%s acc=%.4f",
           id, model_type.c_str(), version.c_str(), accuracy);
    return id;
}

std::vector<ModelDao::ModelInfo> ModelDao::list_all() {
    std::vector<ModelInfo> result;
    PooledConnection conn(pool_);
    if (!conn.get()) return result;

    const char* sql =
        "SELECT id, model_type, version, accuracy, file_path, is_active, created_at "
        "FROM models ORDER BY id DESC";
    if (mysql_query(conn, sql) != 0) return result;

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) return result;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        ModelInfo m;
        m.id          = row[0] ? std::strtoll(row[0], nullptr, 10) : 0;
        m.model_type  = row[1] ? row[1] : "";
        m.version     = row[2] ? row[2] : "";
        m.accuracy    = row[3] ? std::atof(row[3]) : 0.0;
        m.file_path   = row[4] ? row[4] : "";
        m.is_active   = row[5] ? (std::atoi(row[5]) != 0) : false;
        m.created_at  = row[6] ? row[6] : "";
        result.push_back(m);
    }
    mysql_free_result(res);
    return result;
}

// ============================================================================
// StatsDao — 통계 집계
// ============================================================================
// TODO(spec) 가 다수 — 클라 코드와 의미 합의 후 SQL 정확히 다듬을 것.
// 현재는 잠정 정의로 동작하는 SQL.

StatsDao::TodayStats StatsDao::get_today(long long user_id, int daily_goal_min) {
    TodayStats r;
    if (user_id <= 0) return r;

    PooledConnection conn(pool_);
    if (!conn.get()) return r;

    // 오늘(로컬타임 기준) 사용자의 모든 세션의 focus_logs 집계 + posture warning 집계.
    // TODO(spec): warning_count 정의 = posture_logs(posture_ok=0) 카운트로 잠정.
    const char* sql =
        "SELECT "
        "  COALESCE(SUM(CASE WHEN fl.state='focus' THEN 1 ELSE 0 END),0) AS focused_rows,"
        "  COALESCE(AVG(fl.focus_score),0)                              AS avg_score,"
        "  ("
        "    SELECT COUNT(*) FROM posture_logs pl "
        "    JOIN sessions s2 ON s2.id = pl.session_id "
        "    WHERE s2.user_id=? AND s2.date=CURDATE() AND pl.posture_ok=0"
        "  ) AS warning_count "
        "FROM focus_logs fl "
        "JOIN sessions s ON s.id = fl.session_id "
        "WHERE s.user_id=? AND s.date=CURDATE()";

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return r;
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("StatsDao today prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return r;
    }

    MYSQL_BIND bp[2]; std::memset(bp, 0, sizeof(bp));
    long long uid1 = user_id, uid2 = user_id;
    bind_longlong(bp[0], &uid1);
    bind_longlong(bp[1], &uid2);
    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return r;
    }

    MYSQL_BIND br[3]; std::memset(br, 0, sizeof(br));
    long long focused = 0, warn = 0;
    double avg_raw = 0.0;
    bind_longlong(br[0], &focused);
    bind_double  (br[1], &avg_raw);
    bind_longlong(br[2], &warn);

    mysql_stmt_bind_result(stmt, br);
    if (mysql_stmt_fetch(stmt) == 0) {
        r.focus_min     = static_cast<double>(focused) * 0.2 / 60.0;
        r.avg_focus     = avg_raw / 100.0;
        r.warning_count = warn;
        r.goal_progress = (daily_goal_min > 0)
            ? r.focus_min / static_cast<double>(daily_goal_min)
            : 0.0;
    }
    mysql_stmt_close(stmt);
    return r;
}

std::vector<StatsDao::HourBucket>
StatsDao::get_hourly(long long user_id, const std::string& date) {
    std::vector<HourBucket> result;
    if (user_id <= 0 || date.size() != 10) return result;

    PooledConnection conn(pool_);
    if (!conn.get()) return result;

    // hour 별 AVG(focus_score)/100 → 0~1
    const char* sql =
        "SELECT HOUR(fl.ts) AS h, AVG(fl.focus_score)/100.0 AS avg_focus "
        "FROM focus_logs fl JOIN sessions s ON s.id=fl.session_id "
        "WHERE s.user_id=? AND s.date=? "
        "GROUP BY HOUR(fl.ts) ORDER BY h";

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return result;
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("StatsDao hourly prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return result;
    }

    MYSQL_BIND bp[2]; std::memset(bp, 0, sizeof(bp));
    unsigned long l_date = 0; my_bool n_date = 0;
    bind_longlong(bp[0], &user_id);
    bind_string(bp[1], date, l_date, n_date);
    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return result;
    }

    MYSQL_BIND br[2]; std::memset(br, 0, sizeof(br));
    int h = 0;
    double avg_focus = 0.0;
    bind_long  (br[0], &h);
    bind_double(br[1], &avg_focus);
    mysql_stmt_bind_result(stmt, br);
    while (mysql_stmt_fetch(stmt) == 0) {
        result.push_back({h, avg_focus});
    }
    mysql_stmt_close(stmt);
    return result;
}

StatsDao::PatternStats StatsDao::get_pattern(long long user_id) {
    PatternStats r;
    if (user_id <= 0) return r;

    PooledConnection conn(pool_);
    if (!conn.get()) return r;

    // TODO(spec): avg_focus_duration / best_hour / weekly_avg 의미 미정.
    //   잠정:
    //     avg_focus_duration = 세션당 focus_min 평균 (분).
    //     best_hour          = 오늘 hour 별 AVG(focus_score) 최대값의 hour.
    //     weekly_avg         = 최근 7일 AVG(focus_score) / 100.
    {
        const char* sql =
            "SELECT AVG(focus_min) FROM sessions "
            "WHERE user_id=? AND end_time IS NOT NULL "
            "  AND start_time >= NOW() - INTERVAL 7 DAY";
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        if (stmt && mysql_stmt_prepare(stmt, sql, std::strlen(sql)) == 0) {
            MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
            bind_longlong(bp[0], &user_id);
            if (mysql_stmt_bind_param(stmt, bp) == 0 && mysql_stmt_execute(stmt) == 0) {
                MYSQL_BIND br[1]; std::memset(br, 0, sizeof(br));
                double avg = 0.0; my_bool n = 0;
                bind_double(br[0], &avg); br[0].is_null = &n;
                mysql_stmt_bind_result(stmt, br);
                if (mysql_stmt_fetch(stmt) == 0 && !n) r.avg_focus_duration = avg;
            }
        }
        if (stmt) mysql_stmt_close(stmt);
    }

    {
        const char* sql =
            "SELECT HOUR(fl.ts) h FROM focus_logs fl "
            "JOIN sessions s ON s.id=fl.session_id "
            "WHERE s.user_id=? AND s.date=CURDATE() "
            "GROUP BY HOUR(fl.ts) ORDER BY AVG(fl.focus_score) DESC LIMIT 1";
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        if (stmt && mysql_stmt_prepare(stmt, sql, std::strlen(sql)) == 0) {
            MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
            bind_longlong(bp[0], &user_id);
            if (mysql_stmt_bind_param(stmt, bp) == 0 && mysql_stmt_execute(stmt) == 0) {
                MYSQL_BIND br[1]; std::memset(br, 0, sizeof(br));
                int h = -1; bind_long(br[0], &h);
                mysql_stmt_bind_result(stmt, br);
                if (mysql_stmt_fetch(stmt) == 0) r.best_hour = h;
            }
        }
        if (stmt) mysql_stmt_close(stmt);
    }

    {
        const char* sql =
            "SELECT AVG(fl.focus_score)/100.0 FROM focus_logs fl "
            "JOIN sessions s ON s.id=fl.session_id "
            "WHERE s.user_id=? AND s.date >= CURDATE() - INTERVAL 7 DAY";
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        if (stmt && mysql_stmt_prepare(stmt, sql, std::strlen(sql)) == 0) {
            MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
            bind_longlong(bp[0], &user_id);
            if (mysql_stmt_bind_param(stmt, bp) == 0 && mysql_stmt_execute(stmt) == 0) {
                MYSQL_BIND br[1]; std::memset(br, 0, sizeof(br));
                double v = 0.0; my_bool n = 0;
                bind_double(br[0], &v); br[0].is_null = &n;
                mysql_stmt_bind_result(stmt, br);
                if (mysql_stmt_fetch(stmt) == 0 && !n) r.weekly_avg = v;
            }
        }
        if (stmt) mysql_stmt_close(stmt);
    }
    return r;
}

std::vector<StatsDao::DayBucket> StatsDao::get_weekly(long long user_id) {
    std::vector<DayBucket> result;
    if (user_id <= 0) return result;

    PooledConnection conn(pool_);
    if (!conn.get()) return result;

    // 최근 7일 일별 합계.
    // focus_min 은 sessions.focus_min (이미 종료된 세션 합) 사용.
    // avg_focus 는 일별 focus_logs AVG/100.
    const char* sql =
        "SELECT DATE_FORMAT(s.date, '%Y-%m-%d') AS d, "
        "       COALESCE(SUM(s.focus_min),0)         AS focus_min, "
        "       COALESCE(("
        "         SELECT AVG(fl.focus_score)/100.0 FROM focus_logs fl "
        "         JOIN sessions s2 ON s2.id=fl.session_id "
        "         WHERE s2.user_id=s.user_id AND s2.date=s.date), 0) AS avg_focus "
        "FROM sessions s "
        "WHERE s.user_id=? AND s.date >= CURDATE() - INTERVAL 7 DAY "
        "GROUP BY s.date ORDER BY s.date";

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return result;
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        log_err_db("StatsDao weekly prepare 실패 | %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return result;
    }
    MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
    bind_longlong(bp[0], &user_id);
    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return result;
    }

    MYSQL_BIND br[3]; std::memset(br, 0, sizeof(br));
    char r_date[16]{}; unsigned long l_date = 0;
    double focus_min = 0.0, avg_focus = 0.0;
    br[0].buffer_type = MYSQL_TYPE_STRING; br[0].buffer = r_date;
    br[0].buffer_length = sizeof(r_date);  br[0].length = &l_date;
    bind_double(br[1], &focus_min);
    bind_double(br[2], &avg_focus);

    mysql_stmt_bind_result(stmt, br);
    while (mysql_stmt_fetch(stmt) == 0) {
        result.push_back({std::string(r_date, l_date), focus_min, avg_focus});
    }
    mysql_stmt_close(stmt);
    return result;
}

ModelDao::ModelInfo ModelDao::get_active(const std::string& model_type) {
    ModelInfo info;
    if (model_type.empty()) return info;

    PooledConnection conn(pool_);
    if (!conn.get()) return info;

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return info;

    const char* sql =
        "SELECT id, model_type, version, accuracy, file_path, is_active, created_at "
        "FROM models WHERE model_type=? AND is_active=1 ORDER BY id DESC LIMIT 1";
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        return info;
    }

    MYSQL_BIND bp[1]; std::memset(bp, 0, sizeof(bp));
    unsigned long l = 0; my_bool n = 0;
    bind_string(bp[0], model_type, l, n);
    if (mysql_stmt_bind_param(stmt, bp) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return info;
    }

    MYSQL_BIND br[7]; std::memset(br, 0, sizeof(br));
    long long r_id = 0;
    char r_mt[64]{}, r_ver[64]{}, r_path[512]{}, r_created[32]{};
    unsigned long l_mt = 0, l_ver = 0, l_path = 0, l_created = 0;
    my_bool null_path = 0;
    float r_acc = 0;
    int r_active = 0;

    bind_longlong(br[0], &r_id);
    br[1].buffer_type = MYSQL_TYPE_STRING; br[1].buffer = r_mt;
    br[1].buffer_length = sizeof(r_mt);    br[1].length = &l_mt;
    br[2].buffer_type = MYSQL_TYPE_STRING; br[2].buffer = r_ver;
    br[2].buffer_length = sizeof(r_ver);   br[2].length = &l_ver;
    bind_float(br[3], &r_acc);
    br[4].buffer_type = MYSQL_TYPE_STRING; br[4].buffer = r_path;
    br[4].buffer_length = sizeof(r_path);  br[4].length = &l_path;
    br[4].is_null = &null_path;
    bind_long (br[5], &r_active);
    br[6].buffer_type = MYSQL_TYPE_STRING; br[6].buffer = r_created;
    br[6].buffer_length = sizeof(r_created); br[6].length = &l_created;

    mysql_stmt_bind_result(stmt, br);
    if (mysql_stmt_fetch(stmt) == 0) {
        info.id         = r_id;
        info.model_type.assign(r_mt, l_mt);
        info.version.assign(r_ver, l_ver);
        info.accuracy   = r_acc;
        info.file_path  = null_path ? "" : std::string(r_path, l_path);
        info.is_active  = (r_active != 0);
        info.created_at.assign(r_created, l_created);
    }
    mysql_stmt_close(stmt);
    return info;
}

} // namespace factory
