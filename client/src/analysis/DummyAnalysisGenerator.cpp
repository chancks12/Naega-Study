#include "pch.h"
#include "analysis/DummyAnalysisGenerator.h"

#include <chrono>
#include <cmath>
#include <thread>

// ── 더미 시나리오 상수 ──────────────────────────────────────────────
namespace {

// 각 구간 몇 틱 지속할지 (200ms 기준)
constexpr int TICKS_FOCUS_1      = 50;   // 10s  focus
constexpr int TICKS_DROWSY       = 25;   // 5s   drowsy
constexpr int TICKS_BAD_POSTURE  = 25;   // 5s   bad posture
constexpr int TICKS_ABSENT       = 25;   // 5s   absent
constexpr int TICKS_FOCUS_2      = 25;   // 5s   focus 복귀
constexpr int TICKS_TOTAL = TICKS_FOCUS_1 + TICKS_DROWSY + TICKS_BAD_POSTURE
                          + TICKS_ABSENT + TICKS_FOCUS_2;

std::uint64_t now_ms()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// tick 번호에 따라 AnalysisResult를 생성한다.
AnalysisResult make_result(int tick)
{
    const int phase = tick % TICKS_TOTAL;

    AnalysisResult r;
    r.timestamp_ms = now_ms();

    // 미세한 흔들림 (sin 파형으로 자연스럽게)
    const double wobble = std::sin(tick * 0.4) * 1.5;

    if (phase < TICKS_FOCUS_1) {
        // ── 정상 집중 ──────────────────────────────
        r.state        = "focus";
        r.focus_score  = 82 + static_cast<int>(wobble);
        r.ear          = 0.35 + wobble * 0.01;
        r.neck_angle   = 10.0 + wobble;
        r.shoulder_diff= 3.0 + std::abs(wobble) * 0.5;
        r.head_yaw     = wobble * 2.0;
        r.head_pitch   = wobble;
        r.face_detected= 1;
        r.posture_ok   = true;
        r.drowsy       = false;
        r.absent       = false;

    } else if (phase < TICKS_FOCUS_1 + TICKS_DROWSY) {
        // ── 졸음 ───────────────────────────────────
        const double t = (phase - TICKS_FOCUS_1) / static_cast<double>(TICKS_DROWSY);
        r.state        = "drowsy";
        r.focus_score  = 50 - static_cast<int>(t * 20);
        r.ear          = 0.28 - t * 0.13;   // 점점 감김
        r.neck_angle   = 12.0 + t * 5.0;
        r.shoulder_diff= 4.0;
        r.head_yaw     = wobble;
        r.head_pitch   = t * 8.0;           // 점점 고개 숙임
        r.face_detected= 1;
        r.posture_ok   = true;
        r.drowsy       = (t > 0.4);         // 후반부터 drowsy flag
        r.absent       = false;

    } else if (phase < TICKS_FOCUS_1 + TICKS_DROWSY + TICKS_BAD_POSTURE) {
        // ── 목 각도 초과 (자세 불량) ────────────────
        r.state        = "distracted";
        r.focus_score  = 60 + static_cast<int>(wobble);
        r.ear          = 0.32;
        r.neck_angle   = 35.0 + wobble * 2.0;   // 임계값(보통 25) 초과
        r.shoulder_diff= 8.0 + std::abs(wobble);
        r.head_yaw     = wobble * 3.0;
        r.head_pitch   = 15.0 + wobble;
        r.face_detected= 1;
        r.posture_ok   = false;
        r.drowsy       = false;
        r.absent       = false;

    } else if (phase < TICKS_FOCUS_1 + TICKS_DROWSY + TICKS_BAD_POSTURE + TICKS_ABSENT) {
        // ── 자리 비움 ───────────────────────────────
        r.state        = "absent";
        r.focus_score  = 0;
        r.ear          = 0.0;
        r.neck_angle   = 0.0;
        r.shoulder_diff= 0.0;
        r.head_yaw     = 0.0;
        r.head_pitch   = 0.0;
        r.face_detected= 0;
        r.posture_ok   = false;
        r.drowsy       = false;
        r.absent       = true;

    } else {
        // ── 복귀 / 정상 집중 ────────────────────────
        r.state        = "focus";
        r.focus_score  = 75 + static_cast<int>(wobble * 2);
        r.ear          = 0.34;
        r.neck_angle   = 11.0 + wobble;
        r.shoulder_diff= 3.5;
        r.head_yaw     = wobble * 1.5;
        r.head_pitch   = wobble * 0.5;
        r.face_detected= 1;
        r.posture_ok   = true;
        r.drowsy       = false;
        r.absent       = false;
    }

    r.confidence = 1.0;  // 더미: 신뢰도 최대값
    return r;
}

} // namespace

// ── 생성 / 소멸 ────────────────────────────────────────────────────

DummyAnalysisGenerator::DummyAnalysisGenerator(AnalysisResultBuffer& result_buffer,
                                               EventShadowBuffer&    shadow_buffer,
                                               EventQueue&           event_queue)
    : result_buffer_(result_buffer)
    , shadow_buffer_(shadow_buffer)
    , event_queue_(event_queue)
{
    // PostureEvent가 발생하면 EventQueue에 push
    detector_.set_callback([&](PostureEvent evt) {
        event_queue_.push(std::move(evt));
    });
}

DummyAnalysisGenerator::~DummyAnalysisGenerator()
{
    stop();
}

// ── 공개 인터페이스 ────────────────────────────────────────────────

void DummyAnalysisGenerator::start(int interval_ms)
{
    if (running_.exchange(true)) return;
    worker_ = std::thread(&DummyAnalysisGenerator::run, this, interval_ms);
}

void DummyAnalysisGenerator::stop()
{
    running_.store(false);
    if (worker_.joinable()) worker_.join();
}

// ── 워커 루프 ──────────────────────────────────────────────────────

void DummyAnalysisGenerator::run(int interval_ms)
{
    int tick = 0;
    while (running_.load()) {
        const AnalysisResult result = make_result(tick++);

        // 1) HUD 렌더러에 전달
        result_buffer_.update(result);

        // 2) 자세 이벤트 감지기에 전달 (이벤트 발생 시 callback → EventQueue)
        detector_.feed(result, shadow_buffer_);

        // 3) AlertManager 등 외부 콜백 (set_result_callback으로 등록)
        if (result_callback_) result_callback_(result);

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}
