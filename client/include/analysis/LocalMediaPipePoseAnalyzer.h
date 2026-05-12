#pragma once

#include "analysis/IPoseAnalyzer.h"

#include <onnxruntime_cxx_api.h>
#include <opencv2/objdetect.hpp>
#include <memory>
#include <string>
#include <vector>

// MediaPipe Pose + Face Mesh 기반 keypoint 추출기.
// ONNX Runtime으로 pose_landmark.onnx, face_landmark.onnx를 직접 실행한다.
//
// 추출 항목:
//   ear          - Eye Aspect Ratio (468 face landmark 기반)
//   neck_angle   - 귀~어깨 수직각 (pose landmark 7,8,11,12)
//   shoulder_diff- 어깨 y좌표 차이 (pose landmark 11,12)
//   head_yaw     - 좌우 회전
//   head_pitch   - 앞뒤 기울기
//   face_detected- 얼굴 감지 여부

class LocalMediaPipePoseAnalyzer final : public IPoseAnalyzer {
public:
    bool initialize() override;
    std::optional<AnalysisResult> analyze(const Frame& frame) override;
    void shutdown() override;

private:
    double compute_ear(const std::vector<float>& lm468) const;
    void   compute_head_pose(const std::vector<float>& lm468,
                             int crop_w, int crop_h,
                             int frame_w,
                             double& yaw, double& pitch) const;
    double compute_neck_angle(const std::vector<float>& lm,
                              int stride, int frame_w, int frame_h) const;
    double compute_shoulder_diff(const std::vector<float>& lm,
                                 int stride, int frame_h) const;

    Ort::Env            ort_env_{ ORT_LOGGING_LEVEL_WARNING, "StudySync" };
    Ort::SessionOptions ort_opts_;
    std::unique_ptr<Ort::Session> face_session_;
    std::unique_ptr<Ort::Session> pose_session_;

    std::string pose_out0_name_;
    std::string pose_out1_name_;

    cv::CascadeClassifier face_cascade_;

    cv::Rect2f ema_face_rect_;
    double     ema_neck_angle_    = 0.0;
    double     ema_shoulder_diff_ = 0.0;
    bool       has_ema_           = false;

    // Haar cascade는 5프레임마다 한 번만 실행 (중간은 EMA로 유지)
    int  haar_frame_count_ = 0;

    bool initialized_ = false;
};
