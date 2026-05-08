#pragma once

#include "analysis/IPoseAnalyzer.h"

#include <opencv2/objdetect.hpp>
#include <opencv2/imgproc.hpp>

// OpenCV Haar-cascade 기반 keypoint 추출기.
// MediaPipe C++ SDK 의존성이 확정되면 이 구현을 MediaPipe Pose Landmarker로 교체한다.
//
// 추출 항목:
//   ear          - Eye Aspect Ratio (눈 감김 정도)
//   neck_angle   - 고개 수직 기울기 추정치
//   shoulder_diff- 좌우 어깨 높이 차 추정치 (픽셀)
//   head_yaw     - 좌우 회전 추정치 (도)
//   head_pitch   - 앞뒤 기울기 추정치 (도)
//   face_detected- 얼굴 감지 여부

class LocalMediaPipePoseAnalyzer final : public IPoseAnalyzer {
public:
    bool initialize() override;
    std::optional<AnalysisResult> analyze(const Frame& frame) override;
    void shutdown() override;

private:
    // Target MediaPipe feature contract:
    // Pose: neck_angle, shoulder_diff.
    // Face Mesh: ear, head_yaw, head_pitch, face_detected.
    // Phone detection is intentionally excluded from the client feature set.
    double detect_ear(const cv::Mat& gray, const cv::Rect& face_rect);

    cv::CascadeClassifier face_cascade_;
    cv::CascadeClassifier eye_cascade_;
    bool initialized_ = false;
};
