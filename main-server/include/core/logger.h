#pragma once
// ============================================================================
// logger.h — 상태 중심 + 흐름 인식 로그 유틸리티
// ============================================================================
// 설계 기준:
//   1. TAG = 서버 역할 (MAIN / AI / DB 등)
//   2. 이모지 = 상태 / 행동
//   3. 메시지 = 사람이 읽는 정보
//
// 이모지 규칙:
//   ➕ CONNECT        — 연결 생성
//   ➖ DISCONNECT     — 연결 종료
//   👤 LOGIN          — 로그인
//   📋 REGISTER       — 등록
//   🧠 INFER          — AI 추론
//   📊 RESULT         — 결과 출력
//   🟥 NG / 🟩 OK     — 결과 상태
//   🚀 START          — 시작
//   🔄 ROUTE          — 라우팅
//   🔁 RETRY          — 재시도
//   ➡️ SEND           — 데이터 전송
//   ⬅️ RECEIVE        — 데이터 수신
//   ⚠️ WARN           — 경고
//   ⏱ TIMEOUT        — 타임아웃
//   ❌ FAIL           — 최종 실패만 사용
// ============================================================================

#include <cstdio>      // fprintf, vfprintf, fflush, FILE, fopen, fclose
#include <cstdarg>     // va_list, va_copy, va_end (가변 인자 처리)
#include <ctime>       // std::time, std::tm, localtime_r, strftime (시간 함수)
#include <mutex>       // std::mutex, lock_guard (스레드 동기화)
#include <string>      // std::string (동적 문자열)
#include <memory>      // std::shared_ptr (spdlog 핸들 보유)
#include <sys/stat.h>  // mkdir (디렉터리 생성 POSIX API)

// ── spdlog 추가 백엔드 ──
// 기존 stdout / logs/YYYY-MM-DD.log 출력은 그대로 유지하면서, spdlog 가
// 별도 sink (에러 전용 / 사이즈 회전 백업) 로 같은 메시지를 추가 처리한다.
// async logger 라 디스크 I/O 가 핸들러를 블로킹하지 않음.
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

// ── 파일 로거 — 날짜별 로테이션 ──
// logs/YYYY-MM-DD.log 파일에 stdout과 동일한 내용을 tee한다.
// 서버 재시작/크래시 시에도 로그가 보존되어 사후 포렌식 가능.
inline FILE* log_file_get() {
    // static = 프로그램 수명 동안 단 1번만 초기화되고 호출 사이에 값 유지
    static FILE* cur_file = nullptr;          // 현재 열린 로그 파일 핸들 (nullptr=미열림)
    static std::string cur_date;              // 현재 파일이 속한 날짜 ("2026-04-20" 형식)
    static std::mutex mtx;                    // 멀티스레드 안전성 보장용 뮤텍스

    // lock_guard: 스코프 진입 시 lock, 빠져나갈 때 자동 unlock (RAII 패턴)
    // 여러 스레드가 동시에 이 함수 호출해도 파일 교체 경합 없음
    std::lock_guard<std::mutex> lock(mtx);

    // 현재 날짜 계산 (로컬타임)
    std::time_t now = std::time(nullptr);     // 현재 시각을 유닉스 타임스탬프(초 단위)로
    std::tm tm{};                             // 연/월/일/시/분/초로 분해된 구조체 (값 0 초기화)
    localtime_r(&now, &tm);                   // 로컬타임존으로 변환 (_r = 스레드 안전 버전)
    char date_buf[16];                        // "YYYY-MM-DD"(10자) + 여유 공간
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);  // "2026-04-20" 형식으로 포맷팅

    // 날짜가 바뀌었으면 파일 교체 (자정 로테이션)
    if (cur_date != date_buf) {
        if (cur_file) std::fclose(cur_file);  // 기존 파일 닫기 (열려있을 때만)
        ::mkdir("logs", 0755);                // logs/ 디렉터리 생성 (권한 rwxr-xr-x)
                                              // ::는 전역 네임스페이스 강제 (POSIX 버전 사용)
                                              // 이미 있으면 에러 리턴되지만 무시
        std::string path = std::string("logs/") + date_buf + ".log";  // "logs/2026-04-20.log"
        cur_file = std::fopen(path.c_str(), "a");  // "a"=append 모드 (기존 로그 보존)
        cur_date = date_buf;                  // 현재 날짜 갱신
    }
    return cur_file;  // 현재 활성 파일 핸들 반환 (실패 시 nullptr)
}

// 로그 1줄을 파일에 타임스탬프 prefix와 함께 기록
// prefix: 이모지 (예: "🔄"), tag: 역할 (예: "MAIN"), fmt: printf 형식 문자열
inline void log_file_write(const char* prefix, const char* tag, const char* fmt, va_list args) {
    FILE* f = log_file_get();                 // 오늘 로그 파일 핸들 획득
    if (!f) return;                           // 파일 못 열었으면 조용히 실패 (stdout은 찍혔음)

    // 시각 문자열 생성 ("HH:MM:SS")
    std::time_t now = std::time(nullptr);     // 현재 시각
    std::tm tm{};                             // 시간 분해 구조체
    localtime_r(&now, &tm);                   // 로컬타임 변환
    char ts[32];                              // 타임스탬프 버퍼
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);  // "14:23:05" 형식

    // 파일에 한 줄 기록: "[14:23:05] 🔄 [MAIN ] <메시지>\n"
    std::fprintf(f, "[%s] %s [%-5s] ", ts, prefix, tag);  // %-5s = 왼쪽 정렬 5자 고정폭
    std::vfprintf(f, fmt, args);              // 가변 인자를 형식 문자열에 적용하여 출력
    std::fprintf(f, "\n");                    // 줄바꿈
    std::fflush(f);                           // 버퍼 즉시 디스크로 flush (크래시 직전 로그도 보존)
}

// ── spdlog extra logger ──
// 첫 호출 시 lazy init. async + 다음 sink:
//   1) logs/error.log         — warn 이상만 (운영 시 tail -f 용)
//   2) logs/main-rotating.log — 50MB × 5 회전 백업 (자체 일자별 파일과 별개)
// 기존 stdout / logs/YYYY-MM-DD.log 흐름은 손대지 않으므로, 이 sink 들이
// 실패해도 터미널/주 로그는 정상 동작.
inline std::shared_ptr<spdlog::logger>& spd_extra() {
    static std::shared_ptr<spdlog::logger> lg = []{
        // logs/ 디렉토리 보장 — log_file_get 이 보통 먼저 만들지만 안전장치.
        ::mkdir("logs", 0755);

        spdlog::init_thread_pool(8192, 1);  // 큐 8192, 워커 1

        auto err_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
            "logs/error.log", 0, 0);
        err_sink->set_level(spdlog::level::warn);

        auto big_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/main-rotating.log",
            50ULL * 1024 * 1024,  // 50MB
            5);                    // 백업 5개
        big_sink->set_level(spdlog::level::trace);

        auto l = std::make_shared<spdlog::async_logger>(
            "extra",
            spdlog::sinks_init_list{err_sink, big_sink},
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
        l->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        l->set_level(spdlog::level::info);
        return l;
    }();
    return lg;
}

// 현재 시각 "HH:MM:SS" 문자열 생성 (stdout/stderr prefix 용)
inline void log_now_hms(char* buf, size_t n) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(buf, n, "%H:%M:%S", &tm);
}

// ── 공통 출력 ──
// stdout, 자체 일자별 파일, spdlog extra sink 의 3곳에 같은 메시지 fan-out.
inline void log_impl(const char* emoji, const char* tag, const char* fmt, va_list args) {
    // va_list 는 1회 소비 → 사용처마다 별도 va_copy 필요.
    va_list args_for_file, args_for_spd;
    va_copy(args_for_file, args);
    va_copy(args_for_spd,  args);

    char ts[16];
    log_now_hms(ts, sizeof(ts));

    // ── ① stdout (터미널 즉시) ──
    fprintf(stdout, "[%s] %s [%-5s] ", ts, emoji, tag);
    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");
    fflush(stdout);

    // ── ② 자체 일자별 파일 ──
    log_file_write(emoji, tag, fmt, args_for_file);
    va_end(args_for_file);

    // ── ③ spdlog extra sink (async) ──
    char buf[2048];
    std::vsnprintf(buf, sizeof(buf), fmt, args_for_spd);
    va_end(args_for_spd);
    spd_extra()->info("{} [{:<5}] {}", emoji, tag, buf);
}

// 에러 로그 — stderr + 자체 파일 + spdlog (warn 이상이라 error.log 에도 자동 박힘)
inline void log_err_impl(const char* tag, const char* fmt, va_list args) {
    va_list args_for_file, args_for_spd;
    va_copy(args_for_file, args);
    va_copy(args_for_spd,  args);

    char ts[16];
    log_now_hms(ts, sizeof(ts));

    // ── ① stderr ──
    fprintf(stderr, "[%s] ❌ [%-5s] ", ts, tag);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);

    // ── ② 자체 파일 ──
    log_file_write("❌", tag, fmt, args_for_file);
    va_end(args_for_file);

    // ── ③ spdlog (error 레벨 → error.log 에 자동) ──
    char buf[2048];
    std::vsnprintf(buf, sizeof(buf), fmt, args_for_spd);
    va_end(args_for_spd);
    spd_extra()->error("❌ [{:<5}] {}", tag, buf);
}

// ============================================================================
// 역할별 로그 (TAG 고정)
// ============================================================================
// 각 함수는 역할 전용 이모지와 태그를 미리 지정해서 호출 편의성 제공.
// va_start/va_end 패턴이 반복되지만 inline 함수라 컴파일러가 인라인화함.

// ── MAIN (메인 서버 일반) ──
inline void log_main(const char* fmt, ...) {
    va_list args;            // 가변 인자 목록 구조체
    va_start(args, fmt);     // fmt 이후의 인자들을 args에 연결 (C 스타일 가변 인자)
    log_impl("🔄", "MAIN", fmt, args);  // 공용 구현에 위임
    va_end(args);            // 사용 종료 선언 (undefined behavior 방지)
}
inline void log_err_main(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_err_impl("MAIN", fmt, args);    // stderr 전용 버전
    va_end(args);
}

// ── AI (AI 추론/학습 관련) ──
inline void log_ai(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("🧠", "AI", fmt, args);    // 🧠 = 인공 지능 상징
    va_end(args);
}
inline void log_err_ai(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_err_impl("AI", fmt, args);
    va_end(args);
}

// ── DB (데이터베이스 작업) ──
inline void log_db(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("📊", "DB", fmt, args);    // 📊 = 데이터 시각화
    va_end(args);
}
inline void log_err_db(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_err_impl("DB", fmt, args);
    va_end(args);
}

// ── CLIENT ──
inline void log_clt(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("👤", "CLT", fmt, args);
    va_end(args);
}
inline void log_err_clt(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_err_impl("CLT", fmt, args);
    va_end(args);
}

// ── TRAIN ──
inline void log_train(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("🚀", "TRAIN", fmt, args);
    va_end(args);
}
inline void log_err_train(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_err_impl("TRAIN", fmt, args);
    va_end(args);
}

// ── IMG ──
inline void log_img(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("🟩", "IMG", fmt, args);
    va_end(args);
}
inline void log_err_img(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_err_impl("IMG", fmt, args);
    va_end(args);
}

// ── PUSH ──
inline void log_push(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("➡️", "PUSH", fmt, args);
    va_end(args);
}
inline void log_err_push(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_err_impl("PUSH", fmt, args);
    va_end(args);
}

// ── ACK ──
inline void log_ack(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("🟩", "ACK", fmt, args);
    va_end(args);
}
inline void log_err_ack(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_err_impl("ACK", fmt, args);
    va_end(args);
}

// ============================================================================
// 행동 기반 로그 (TAG를 인자로 받음)
// ============================================================================

// 전송 / 수신
inline void log_send(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("➡️", tag, fmt, args);
    va_end(args);
}

inline void log_recv(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("⬅️", tag, fmt, args);
    va_end(args);
}

// 재시도 / 라우팅
inline void log_retry(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("🔁", tag, fmt, args);
    va_end(args);
}

inline void log_route(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("🔄", "ROUTE", fmt, args);
    va_end(args);
}
inline void log_err_route(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_err_impl("ROUTE", fmt, args);
    va_end(args);
}

// 상태
inline void log_warn(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("⚠️", tag, fmt, args);
    va_end(args);
}

inline void log_timeout(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("⏱", tag, fmt, args);
    va_end(args);
}

// 결과
inline void log_ok(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("🟩", tag, fmt, args);
    va_end(args);
}

inline void log_ng(const char* tag, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    log_impl("🟥", tag, fmt, args);
    va_end(args);
}
