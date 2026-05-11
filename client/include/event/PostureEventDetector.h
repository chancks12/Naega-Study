#pragma once

#include "event/EventShadowBuffer.h"
#include "model/AnalysisResult.h"
#include "model/PostureEvent.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>

class PostureEventDetector {
public:
    using EventCallback = std::function<void(PostureEvent)>;

    void set_callback(EventCallback cb);
    void feed(const AnalysisResult& result, const EventShadowBuffer& shadow);
    void reset_cooldown();

    // 캘리브레이션 결과로 런타임 임계값 설정
    void set_neck_threshold(double deg) { neck_threshold_ = deg; }
    void set_ear_threshold(float val)   { ear_threshold_  = val; }

    // 카메라 fps 설정 — 클립 윈도우(7초) 프레임 수 계산에 사용
    void set_camera_fps(int fps) { camera_fps_.store(fps > 0 ? fps : 30); }

private:
    // 이벤트 발생 시 post-roll 대기를 위해 보류 중인 이벤트 정보
    struct PendingEvent {
        PostureEventType type;
        std::uint64_t    timestamp_ms;
        std::string      reason;
        double           confidence;
        int              post_roll_remaining; // 남은 프레임 수
    };

    void schedule_event(PostureEventType type, const char* reason, const AnalysisResult& result);
    void flush_pending(const EventShadowBuffer& shadow);

    EventCallback callback_;

    int  bad_posture_streak_ = 0;
    int  drowsy_streak_ = 0;
    bool event_cooldown_ = false;
    std::string last_state_;

    double neck_threshold_ = 25.0;
    float  ear_threshold_  = 0.25f;

    std::atomic<int> camera_fps_{ 30 };

    std::optional<PendingEvent> pending_; // post-roll 대기 중인 이벤트

    // 이벤트 최소 발화 간격 (AI 서버 추론 주기 5초에 맞춤)
    std::uint64_t last_event_ms_ = 0;
    static constexpr std::uint64_t kMinEventIntervalMs = 5000;
};
