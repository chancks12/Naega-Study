#pragma once

#include "model/PostureEvent.h"

#include <cstdint>
#include <string>

// 복기 화면에서 한 행을 표현하는 뷰모델
struct ReviewEvent {
    std::string       event_id;
    std::uint64_t     timestamp_ms = 0;
    PostureEventType  type         = PostureEventType::BadPosture;
    std::string       clip_dir;      // 로컬 클립 디렉터리 경로

    // 피드백 상태
    enum class Feedback { None, Correct, Wrong };
    Feedback feedback = Feedback::None;
};

