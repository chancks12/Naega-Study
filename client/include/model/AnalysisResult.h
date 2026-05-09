#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Landmark2D {
    float x = 0.0f;
    float y = 0.0f;
    float visibility = 0.0f;
};

struct AnalysisResult {
    std::uint64_t timestamp_ms = 0;

    // ── keypoint 필드 (MediaPipe 클라이언트 추출) ──────────────
    double neck_angle    = 0.0;
    double shoulder_diff = 0.0;
    double ear           = 1.0;   // Eye Aspect Ratio (0.0=closed, 1.0=open)
    double head_yaw      = 0.0;   // 좌우 회전  -90~+90
    double head_pitch    = 0.0;   // 앞뒤 기울기 -90~+90
    int    face_detected = 1;     // 0 or 1

    // ── AI 서버 응답 필드 ───────────────────────────────────────
    int    focus_score = 0;
    double confidence  = 1.0;     // TCN 판정 신뢰도 (Stage 2 피드백 UI 사용)
    bool   posture_ok  = true;
    bool   drowsy      = false;
    bool   absent      = false;
    std::string state;

    std::vector<Landmark2D> landmarks;
};

