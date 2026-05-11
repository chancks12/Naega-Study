// ============================================================================
// connection_pool.cpp — MariaDB 커넥션 풀 구현
// ============================================================================
// 목적:
//   모든 DAO 가 공유하는 MariaDB 커넥션 풀. acquire()/release() 로 빌려주고
//   반환받는 고전적인 pool 패턴. 각 연결마다 별도 mysql_init() 비용(100ms+)을
//   지불하지 않아도 됨.
//
// 설계 포인트:
//   - 풀 크기는 config.database.pool_size 로 고정 (기본 4) — 일반적으로 동시
//     발생하는 DB 작업 수(InspectionService + TrainService + GuiService) 에 맞춰
//     튜닝.
//   - 가용 커넥션이 없으면 condition_variable 로 5초 대기 후 타임아웃 → nullptr.
//   - mysql_ping 으로 "dead connection" 감지 → 자동 재연결 시도 + double-close 방지.
//   - MYSQL_OPT_RECONNECT 는 옵션으로 켜져 있으나, 일부 버전에서 무시되는
//     경우가 있어 ping 기반 재연결을 추가로 둠 (이중 안전망).
//
// 스레드 안전성:
//   모든 public 메서드는 mutex_ 로 보호. acquire 는 condition_variable 사용.
// ============================================================================
#include "storage/connection_pool.h"
#include "core/logger.h"

#include <chrono>

namespace factory {

ConnectionPool::ConnectionPool(const std::string& host,
                               const std::string& user,
                               const std::string& password,
                               const std::string& schema,
                               unsigned int port,
                               int pool_size)
    : host_(host), user_(user), password_(password),
      schema_(schema), port_(port), pool_size_(pool_size) {
}

ConnectionPool::~ConnectionPool() {
    shutdown();
}

// ---------------------------------------------------------------------------
// create_connection — 새 MYSQL* 한 개 생성 + 옵션 설정 + 접속
//   utf8mb4  : 이모지/한글 보존을 위해 utf8 대신 utf8mb4 지정
//   RECONNECT: 클라 라이브러리가 자체 재접속 시도 (보조 — ping 보완용)
// 실패 시 nullptr 반환. 호출자는 반드시 체크해야 한다.
// ---------------------------------------------------------------------------
MYSQL* ConnectionPool::create_connection() {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        log_err_db("mysql_init 실패 (풀)");
        return nullptr;
    }

    my_bool reconnect = 1;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (!mysql_real_connect(conn, host_.c_str(), user_.c_str(),
                            password_.c_str(), schema_.c_str(),
                            port_, nullptr, 0)) {
        log_err_db("커넥션 풀 연결 실패 | %s", mysql_error(conn));
        mysql_close(conn);
        return nullptr;
    }

    return conn;
}

// ---------------------------------------------------------------------------
// init — 서버 부팅 시 1회 호출. pool_size_ 개의 커넥션을 미리 생성.
//   pool_      : 가용 커넥션 큐 (acquire/release 대상)
//   all_conns_ : 생성된 모든 커넥션의 "마스터 목록" — shutdown 시 일괄 해제용
// 두 자료구조를 분리하는 이유: acquire 중인 커넥션(= pool_ 에 없음) 도
// shutdown 때 닫아야 하므로 별도 리스트가 필요.
// ---------------------------------------------------------------------------
bool ConnectionPool::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < pool_size_; ++i) {
        MYSQL* conn = create_connection();
        if (!conn) {
            log_err_db("커넥션 풀 초기화 실패 | %d/%d", i, pool_size_);
            return false;  // 하나라도 실패하면 전체 중단 — DB 없이 서비스 불가
        }
        pool_.push(conn);
        all_conns_.push_back(conn);
    }
    log_db("커넥션 풀 초기화 완료 | %d개 연결 | %s:%d/%s",
           pool_size_, host_.c_str(), port_, schema_.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// acquire — 가용 커넥션 하나를 빌려받는다. 없으면 5초 대기.
//
// 반환된 커넥션은 mysql_ping 으로 live 상태를 보장한다. dead 라면 내부에서
// 재연결하고 all_conns_ 슬롯을 교체 (double-close 방지).
//
// 타임아웃(5초):
//   풀이 고갈된 상태에서 무한 대기하면 요청 스레드가 쌓여 서버 전체가 마비.
//   5초 안에 못 얻으면 nullptr 반환 → 호출자가 상황에 맞게 재시도/실패처리.
//
// shutdown 경합 처리:
//   is_shutdown_ 체크 + notify_all 로 대기 중인 모든 스레드를 깨움.
// ---------------------------------------------------------------------------
MYSQL* ConnectionPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::seconds(5),
                      [this] { return !pool_.empty() || is_shutdown_; })) {
        log_err_db("커넥션 풀 acquire 타임아웃 (5초)");
        return nullptr;
    }
    if (is_shutdown_) return nullptr;

    MYSQL* old_conn = pool_.front();
    pool_.pop();

    // Dead connection 감지 (서버 재시작, 네트워크 순단, wait_timeout 초과 등)
    if (mysql_ping(old_conn) != 0) {
        log_err_db("커넥션 끊어짐 → 재연결 시도");
        mysql_close(old_conn);

        MYSQL* new_conn = create_connection();
        // 핵심: all_conns_ 내 old → new 슬롯 교체
        // 만약 이걸 안 하면 shutdown 시 이미 해제된 old 를 다시 close → double-free
        // new 가 nullptr 이어도 교체 — shutdown 의 `if (conn) mysql_close(conn)` 가
        // nullptr 방어하므로 double-free 발생하지 않음. 오히려 old 를 그대로 두면
        // shutdown 시 이미 해제된 포인터를 close 하게 되어 위험.
        for (auto& c : all_conns_) {
            if (c == old_conn) {
                c = new_conn;
                break;
            }
        }

        if (!new_conn) {
            log_err_db("재연결 실패 — 풀 크기 축소");
            return nullptr;  // 풀 유효 크기가 실질적으로 하나 줄어듦
        }
        return new_conn;
    }

    return old_conn;
}

// ---------------------------------------------------------------------------
// release — 커넥션을 풀에 반환. DAO 가 작업 끝나면 반드시 호출해야 함.
// RAII 가드(ConnectionGuard 등) 가 있으면 더 안전 — 지금은 수동 관리.
// ---------------------------------------------------------------------------
void ConnectionPool::release(MYSQL* conn) {
    if (!conn) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pool_.push(conn);
    cv_.notify_one();  // acquire 에서 대기 중인 스레드가 있으면 깨움
}

// ---------------------------------------------------------------------------
// shutdown — 서버 종료 시 모든 커넥션 정리
//
// 순서:
//   1) is_shutdown_ 플래그 set + notify_all → acquire 대기 스레드 일괄 해제
//   2) all_conns_ 순회하며 mysql_close (acquire 중인 것 포함 마스터 목록)
//   3) 큐/목록 비우기
// ---------------------------------------------------------------------------
void ConnectionPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        is_shutdown_ = true;
    }
    cv_.notify_all();

    // all_conns_ 는 acquire 실패로 nullptr 이 섞일 수 있으므로 체크 필수
    for (MYSQL* conn : all_conns_) {
        if (conn) mysql_close(conn);
    }
    all_conns_.clear();
    while (!pool_.empty()) pool_.pop();
    log_db("커넥션 풀 종료");
}

} // namespace factory
