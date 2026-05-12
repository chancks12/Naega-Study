#pragma once

#include "model/Frame.h"

#include <cstdint>
#include <string>
#include <vector>

enum class PostureEventType {
    BadPosture,
    Drowsy,
    Absent,
    Focus   // 비정상 상태에서 공부 재개시 로그 기록용
};

struct PostureEvent {
    PostureEventType type = PostureEventType::BadPosture;
    std::uint64_t timestamp_ms = 0;
    std::string event_id;   // 서버 멱등 처리 키 (emit 시 자동 생성)
    std::string reason;
    std::vector<Frame> frames;
    double confidence = 1.0; // AI 판정 신뢰도 (복기 화면 피드백 우선순위 결정용)
    int camera_fps = 30;     // 클립 VideoWriter fps 및 윈도우 크기 계산용
};
