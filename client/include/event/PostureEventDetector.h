#pragma once

#include "event/EventShadowBuffer.h"
#include "model/AnalysisResult.h"
#include "model/PostureEvent.h"

#include <functional>
#include <string>

class PostureEventDetector {
public:
    using EventCallback = std::function<void(PostureEvent)>;

    void set_callback(EventCallback cb);
    void feed(const AnalysisResult& result, const EventShadowBuffer& shadow);
    void reset_cooldown();

    // 캘리브레이션 결과로 런타임 임계값 설정 (기본: neck=25.0, ear=0.25)
    void set_neck_threshold(double deg)  { neck_threshold_ = deg; }
    void set_ear_threshold(float val)    { ear_threshold_  = val; }

private:
    void emit_event(PostureEventType type, const char* reason, const AnalysisResult& result, const EventShadowBuffer& shadow);

    EventCallback callback_;
    int bad_posture_streak_ = 0;
    int drowsy_streak_ = 0;
    bool event_cooldown_ = false;
    std::string last_state_;

    double neck_threshold_ = 25.0;   // 캘리브레이션으로 덮어씀
    float  ear_threshold_  = 0.25f;
};

