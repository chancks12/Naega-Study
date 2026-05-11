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
//   head_yaw     - 좌우 회전 (solvePnP)
//   head_pitch   - 앞뒤 기울기 (solvePnP)
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
                             double& yaw, double& pitch) const;
    // stride: 랜드마크당 float 수 (3=x,y,z / 4=x,y,z,vis / 5=x,y,z,vis,pres)
    double compute_neck_angle(const std::vector<float>& lm,
                              int stride, int frame_w, int frame_h) const;
    double compute_shoulder_diff(const std::vector<float>& lm,
                                 int stride, int frame_h) const;

    // OrtEnv는 Session보다 먼저 생성되고 나중에 소멸되어야 한다
    Ort::Env            ort_env_{ ORT_LOGGING_LEVEL_WARNING, "StudySync" };
    Ort::SessionOptions ort_opts_;
    std::unique_ptr<Ort::Session> face_session_;
    std::unique_ptr<Ort::Session> pose_session_;

    // initialize()에서 열거한 실제 포즈 모델 출력 노드 이름
    std::string pose_out0_name_;
    std::string pose_out1_name_;

    // 얼굴 위치 감지 (Haar cascade) → crop 영역 계산용
    cv::CascadeClassifier face_cascade_;

    // EMA-smoothed 얼굴 위치 (Haar 성공 프레임마다 갱신, 실패 시 유지)
    // → body_crop 안정화 → 포즈 모델 입력 안정화
    cv::Rect2f ema_face_rect_;
    double     ema_neck_angle_    = 0.0;
    double     ema_shoulder_diff_ = 0.0;
    bool       has_ema_           = false; // EMA 초기화 완료 여부

    bool initialized_ = false;
};
