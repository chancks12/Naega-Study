// ============================================================================
// main.cpp — StudySync 메인 운영 서버 진입점
// ============================================================================
// 구성:
//   EventBus (TCP 측)
//     ├─ TcpListener     : AI 추론서버 패킷 수신 → PACKET_RECEIVED
//     ├─ Router          : protocol_no 분기
//     ├─ FocusService    : FOCUS_LOG_PUSH (1000) → focus_logs INSERT + ACK
//     ├─ PostureService  : POSTURE_LOG_PUSH (1002) / EVENT / BASELINE → DB + ACK
//     ├─ TrainHandler    : TRAIN_COMPLETE → 모델 저장 + DB + 리로드
//     ├─ AckSender       : ACK_SEND_REQUESTED + MODEL_RELOAD_REQUESTED → AI 측
//     └─ HealthChecker   : 추론/학습 서버 생존 확인
//
//   HttpServer (클라 측)
//     ├─ AuthController     : POST /auth/register, /auth/login
//     ├─ GoalController     : POST/GET /goal
//     ├─ SessionController  : POST /session/start, /session/end
//     └─ StatsController    : GET /stats/today, /hourly, /pattern, /weekly
//
// 종료: SIGINT/SIGTERM → g_should_exit → 루프 탈출 → 역순 정리
// ============================================================================

#include "core/event_bus.h"
#include "core/tcp_listener.h"
#include "core/config.h"
#include "core/logger.h"
#include "handler/router.h"
#include "handler/ack_sender.h"
#include "handler/train_handler.h"
#include "service/focus_service.h"
#include "service/posture_service.h"
#include "service/train_service.h"
#include "service/user_service.h"
#include "service/goal_service.h"
#include "service/session_service.h"
#include "service/stats_service.h"
#include "service/log_service.h"
#include "service/session_cleanup_worker.h"
#include "storage/connection_pool.h"
#include "monitor/health_checker.h"
#include "http/http_server.h"
#include "http/jwt_middleware.h"
#include "http/controllers/auth_controller.h"
#include "http/controllers/goal_controller.h"
#include "http/controllers/session_controller.h"
#include "http/controllers/stats_controller.h"
#include "http/controllers/log_controller.h"
#include "http/controllers/log_ingest_controller.h"
#include "Protocol.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> g_should_exit{false};
void on_signal(int) { g_should_exit.store(true); }
} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    using namespace factory;

    // 0) 설정 로드
    std::string config_path = "../../config/config.json";
    if (argc > 1) config_path = argv[1];
    else if (const char* env = std::getenv("CONFIG_PATH")) config_path = env;

    if (!Config::instance().load(config_path)) {
        log_err_main("설정 로드 실패: %s", config_path.c_str());
        return 1;
    }

    auto& cfg = Config::instance();

    // 네트워크 / DB
    uint16_t    ai_port      = static_cast<uint16_t>(cfg.get_int("network.main_server_ai_port", 9000));
    int         http_port    = cfg.get_int("network.http_port", 8080);
    std::string http_host    = cfg.get_str("network.http_host", "0.0.0.0");
    std::string db_host      = cfg.get_str("database.host",     "10.10.10.100");
    std::string db_user      = cfg.get_str("database.user",     "admin");
    std::string db_password  = cfg.get_str("database.password", "1234");
    std::string db_schema    = cfg.get_str("database.schema",   "StudySync");
    int         db_pool_size = cfg.get_int("database.pool_size", 4);
    int         worker_count = cfg.get_int("limits.event_bus_workers", 4);
    int         event_queue_max = cfg.get_int("limits.event_queue_max", 10000);
    int         critical_event_queue_max = cfg.get_int("limits.critical_event_queue_max", 1024);

    // JWT
    std::string jwt_secret      = cfg.get_str("auth.jwt_secret",
        // TODO(secret): 실배포에서는 환경변수/시크릿 매니저로. 임시 기본값.
        "studysync-dev-secret-please-change");
    int         jwt_expires_sec = cfg.get_int("auth.jwt_expires_sec", 86400);
    if (const char* env = std::getenv("JWT_SECRET")) jwt_secret = env;

    // 1) EventBus
    EventBus event_bus(worker_count,
                       static_cast<std::size_t>(event_queue_max),
                       static_cast<std::size_t>(critical_event_queue_max));
    event_bus.start();

    // 2) Router (TCP 측)
    Router router(event_bus);
    router.register_handlers();

    // 3) DB 풀
    ConnectionPool db_pool(db_host, db_user, db_password, db_schema, 3306, db_pool_size);
    if (!db_pool.init()) {
        log_err_main("DB 커넥션 풀 초기화 실패");
        return 1;
    }

    // 4) TCP 도메인 서비스
    FocusService   focus_service(event_bus, db_pool);   focus_service.register_handlers();
    PostureService posture_service(event_bus, db_pool); posture_service.register_handlers();

    // 5) 학습 채널
    TrainService  train_service(db_pool);
    TrainHandler  train_handler(event_bus, train_service);
    train_handler.register_handlers();

    // 6) ACK 송신
    AckSender ack_sender(event_bus);
    ack_sender.register_handlers();

    // 7) AI 측 TCP 리스너
    TcpListener tcp_listener(event_bus, ai_port);
    tcp_listener.start();

    // 8) HTTP 서버 + 컨트롤러
    http::JwtMiddleware jwt_mw(jwt_secret);
    http::HttpServer    http_server(http_host, http_port);

    UserService    user_service(db_pool, jwt_mw, jwt_expires_sec);
    GoalService    goal_service(db_pool);
    SessionService session_service(db_pool);
    StatsService   stats_service(db_pool);
    LogService     log_service(db_pool);

    http::AuthController    auth_ctrl(http_server, user_service);
    http::GoalController    goal_ctrl(http_server, jwt_mw, goal_service);
    http::SessionController session_ctrl(http_server, jwt_mw, session_service);
    http::StatsController   stats_ctrl(http_server, jwt_mw, stats_service);
    http::LogController       log_ctrl(http_server, jwt_mw, log_service);
    http::LogIngestController ingest_ctrl(http_server, jwt_mw, log_service);

    auth_ctrl.register_routes();
    goal_ctrl.register_routes();
    session_ctrl.register_routes();
    stats_ctrl.register_routes();
    log_ctrl.register_routes();
    ingest_ctrl.register_routes();

    http_server.start();

    // 9) 헬스체크
    std::vector<HealthTarget> targets;
    for (const auto& tc : cfg.get_health_targets()) {
        targets.push_back({tc.name, tc.ip, static_cast<uint16_t>(tc.port)});
    }
    HealthChecker health_checker(event_bus, targets);
    health_checker.start();

    // 10) 미종료 세션 cron 정리기 — 클라 비정상 종료 후 매달린 sessions row 정리
    //     기본 30분마다 깨어나 6시간 이상 stale 한 세션 강제 마감.
    int cleanup_interval = cfg.get_int("limits.session_cleanup_interval_min", 30);
    int cleanup_stale_hr = cfg.get_int("limits.session_stale_hours", 6);
    SessionCleanupWorker session_cleanup(db_pool, cleanup_interval, cleanup_stale_hr);
    session_cleanup.start();

    log_main("StudySync 메인서버 시작 완료 | TCP=%d HTTP=%d | Ctrl+C 종료",
             ai_port, http_port);

    auto last_event_bus_log = std::chrono::steady_clock::now();
    while (!g_should_exit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto now = std::chrono::steady_clock::now();
        if (now - last_event_bus_log >= std::chrono::seconds(30)) {
            auto s = event_bus.stats();
            log_main("EventBus stats | normal=%zu/%d critical=%zu/%d "
                     "pub=%llu crit_pub=%llu drop=%llu crit_drop=%llu "
                     "hwm=%zu crit_hwm=%zu",
                     s.queue_size, event_queue_max,
                     s.critical_queue_size, critical_event_queue_max,
                     static_cast<unsigned long long>(s.published),
                     static_cast<unsigned long long>(s.critical_published),
                     static_cast<unsigned long long>(s.dropped),
                     static_cast<unsigned long long>(s.critical_dropped),
                     s.high_water_mark,
                     s.critical_high_water_mark);
            last_event_bus_log = now;
        }
    }

    log_main("서버 종료 중...");
    session_cleanup.stop();
    health_checker.stop();
    http_server.stop();
    tcp_listener.stop();
    db_pool.shutdown();
    event_bus.stop();
    return 0;
}
