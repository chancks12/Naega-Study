#include "pch.h"
#include "analysis/LocalMediaPipePoseAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

namespace {

// ── face mesh eye landmark 인덱스 (AI 서버 코드와 동일) ────────────────
const int kLeftEye[]  = {33, 160, 158, 133, 153, 144};
const int kRightEye[] = {362, 385, 387, 263, 373, 380};

// ── head pose solvePnP 기준점 (3D, mm 단위 표준 얼굴 모델) ────────────
const cv::Point3f k3d[] = {
    {  0.0f,   0.0f,   0.0f},   // 코끝      (idx 1)
    {  0.0f, -63.6f, -12.5f},   // 턱        (idx 152)
    {-43.3f,  32.7f, -26.0f},   // 왼눈 외각  (idx 263)
    { 43.3f,  32.7f, -26.0f},   // 오른눈 외각 (idx 33)
    {-28.9f, -28.9f, -24.1f},   // 왼 입꼬리  (idx 287)
    { 28.9f, -28.9f, -24.1f},   // 오른 입꼬리 (idx 57)
};
const int k2dIdx[] = {1, 152, 263, 33, 287, 57};

std::wstring model_path(const wchar_t* name)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    path.resize(path.rfind(L'\\') + 1);
    path += L"models\\";
    path += name;
    return path;
}

double euclidean(float x1, float y1, float x2, float y2)
{
    const float dx = x1 - x2, dy = y1 - y2;
    return std::sqrt(static_cast<double>(dx * dx + dy * dy));
}

} // namespace

// ── 초기화 ─────────────────────────────────────────────────────────────

bool LocalMediaPipePoseAnalyzer::initialize()
{
    ort_opts_.SetIntraOpNumThreads(2);
    ort_opts_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    try {
        face_session_ = std::make_unique<Ort::Session>(
            ort_env_, model_path(L"face_landmark.onnx").c_str(), ort_opts_);
        pose_session_ = std::make_unique<Ort::Session>(
            ort_env_, model_path(L"pose_landmark.onnx").c_str(), ort_opts_);
    } catch (const Ort::Exception& e) {
        OutputDebugStringA(("[LocalPose] ONNX load failed: " + std::string(e.what()) + "\n").c_str());
        return false;
    }

    // 얼굴 위치 감지용 Haar (landmark 정확도에는 영향 없음)
    face_cascade_.load("C:/opencv/build/etc/haarcascades/haarcascade_frontalface_default.xml");
    if (face_cascade_.empty()) {
        OutputDebugStringA("[LocalPose] face cascade not found — full-frame crop fallback\n");
    }

    initialized_ = true;
    OutputDebugStringA("[LocalPose] ONNX models loaded OK\n");
    return true;
}

// ── 메인 분석 ──────────────────────────────────────────────────────────

std::optional<AnalysisResult> LocalMediaPipePoseAnalyzer::analyze(const Frame& frame)
{
    if (!initialized_ || frame.mat.empty()) return std::nullopt;

    AnalysisResult result;
    result.timestamp_ms = frame.timestamp_ms;

    const int W = frame.mat.cols;
    const int H = frame.mat.rows;

    Ort::MemoryInfo mem_info =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ── 1. Pose Landmark (전체 프레임 → 256×256) ─────────────────
    {
        cv::Mat inp;
        cv::resize(frame.mat, inp, {256, 256});
        cv::cvtColor(inp, inp, cv::COLOR_BGR2RGB);
        inp.convertTo(inp, CV_32F, 1.0 / 255.0);

        std::vector<float> flat(256 * 256 * 3);
        std::memcpy(flat.data(), inp.data, flat.size() * sizeof(float));

        const std::array<int64_t, 4> shape = {1, 256, 256, 3};
        auto tensor = Ort::Value::CreateTensor<float>(
            mem_info, flat.data(), flat.size(), shape.data(), 4);

        const char* in_names[]  = {"input_1"};
        const char* out_names[] = {"Identity", "Identity_1"};

        try {
            auto outs = pose_session_->Run(
                Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 2);

            const float conf = outs[1].GetTensorData<float>()[0];
            if (conf > 0.5f) {
                const float* lm = outs[0].GetTensorData<float>();
                const std::vector<float> lm195(lm, lm + 195);
                result.neck_angle    = compute_neck_angle(lm195, W, H);
                result.shoulder_diff = compute_shoulder_diff(lm195, H);
            }
        } catch (const Ort::Exception& e) {
            OutputDebugStringA(("[LocalPose] pose run: " + std::string(e.what()) + "\n").c_str());
        }
    }

    // ── 2. 얼굴 bbox 감지 ────────────────────────────────────────
    cv::Rect face_rect;
    {
        cv::Mat gray;
        cv::cvtColor(frame.mat, gray, cv::COLOR_BGR2GRAY);
        cv::equalizeHist(gray, gray);

        if (!face_cascade_.empty()) {
            std::vector<cv::Rect> faces;
            face_cascade_.detectMultiScale(gray, faces, 1.1, 3, 0, {80, 80});
            if (!faces.empty()) {
                face_rect = *std::max_element(faces.begin(), faces.end(),
                    [](const cv::Rect& a, const cv::Rect& b){
                        return a.area() < b.area();
                    });
            }
        }

        // Haar 실패 시 화면 중앙 상단을 얼굴 영역으로 사용
        if (face_rect.empty()) {
            face_rect = cv::Rect(W / 4, 0, W / 2, H / 2);
        }
    }

    // ── 3. Face Landmark (얼굴 crop → 192×192) ───────────────────
    {
        const int pad = static_cast<int>(face_rect.width * 0.25);
        const int rx  = std::max(0, face_rect.x - pad);
        const int ry  = std::max(0, face_rect.y - pad);
        const int rw  = std::min(W - rx, face_rect.width  + 2 * pad);
        const int rh  = std::min(H - ry, face_rect.height + 2 * pad);
        const cv::Rect padded(rx, ry, rw, rh);

        cv::Mat crop = frame.mat(padded);
        cv::Mat inp;
        cv::resize(crop, inp, {192, 192});
        cv::cvtColor(inp, inp, cv::COLOR_BGR2RGB);
        inp.convertTo(inp, CV_32F, 1.0 / 255.0);

        std::vector<float> flat(192 * 192 * 3);
        std::memcpy(flat.data(), inp.data, flat.size() * sizeof(float));

        const std::array<int64_t, 4> shape = {1, 192, 192, 3};
        auto tensor = Ort::Value::CreateTensor<float>(
            mem_info, flat.data(), flat.size(), shape.data(), 4);

        const char* in_names[]  = {"input_1"};
        const char* out_names[] = {"conv2d_21", "conv2d_31"};

        try {
            auto outs = face_session_->Run(
                Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 2);

            const float raw_flag = outs[1].GetTensorData<float>()[0];
            const float face_conf = 1.0f / (1.0f + std::exp(-raw_flag));

            if (face_conf > 0.5f) {
                result.face_detected = 1;
                const float* lm_raw = outs[0].GetTensorData<float>();
                const std::vector<float> lm468(lm_raw, lm_raw + 1404);
                result.ear = compute_ear(lm468);
                compute_head_pose(lm468, padded.width, padded.height,
                                  result.head_yaw, result.head_pitch);
            } else {
                result.face_detected = 0;
            }
        } catch (const Ort::Exception& e) {
            OutputDebugStringA(("[LocalPose] face run: " + std::string(e.what()) + "\n").c_str());
            result.face_detected = 0;
        }
    }

    // ── 4. 최종 판정 ─────────────────────────────────────────────
    result.absent     = (result.face_detected == 0);
    result.drowsy     = (result.face_detected == 1 && result.ear > 0.0 && result.ear < 0.25);
    result.posture_ok = (result.neck_angle < 25.0);

    if (result.absent) {
        result.state       = "absent";
        result.focus_score = 0;
    } else if (result.drowsy) {
        result.state       = "drowsy";
        result.focus_score = std::max(0, 30 + static_cast<int>(result.ear * 100.0));
    } else if (!result.posture_ok) {
        result.state       = "distracted";
        result.focus_score = 55;
    } else {
        result.state       = "focus";
        result.focus_score = static_cast<int>(
            80.0 - std::abs(result.head_yaw) * 0.4 - std::abs(result.head_pitch) * 0.3);
    }
    result.focus_score = std::clamp(result.focus_score, 0, 100);

    return result;
}

// ── EAR 계산 ───────────────────────────────────────────────────────────

double LocalMediaPipePoseAnalyzer::compute_ear(const std::vector<float>& lm) const
{
    auto px = [&](int i){ return lm[i * 3]; };
    auto py = [&](int i){ return lm[i * 3 + 1]; };

    auto ear_one = [&](const int* eye) -> double {
        const double v1 = euclidean(px(eye[1]), py(eye[1]), px(eye[5]), py(eye[5]));
        const double v2 = euclidean(px(eye[2]), py(eye[2]), px(eye[4]), py(eye[4]));
        const double h  = euclidean(px(eye[0]), py(eye[0]), px(eye[3]), py(eye[3]));
        return (h > 0.0) ? (v1 + v2) / (2.0 * h) : 0.32;
    };

    return std::clamp((ear_one(kLeftEye) + ear_one(kRightEye)) / 2.0, 0.0, 1.0);
}

// ── head_yaw / head_pitch (solvePnP) ───────────────────────────────────

void LocalMediaPipePoseAnalyzer::compute_head_pose(
    const std::vector<float>& lm, int crop_w, int crop_h,
    double& yaw, double& pitch) const
{
    yaw = pitch = 0.0;

    std::vector<cv::Point2f> pts2d;
    pts2d.reserve(6);
    for (int idx : k2dIdx) {
        pts2d.push_back({
            lm[idx * 3]     * static_cast<float>(crop_w) / 192.0f,
            lm[idx * 3 + 1] * static_cast<float>(crop_h) / 192.0f
        });
    }

    const std::vector<cv::Point3f> pts3d(std::begin(k3d), std::end(k3d));
    const float focal = static_cast<float>(crop_w);
    const cv::Mat cam_mat = (cv::Mat_<double>(3, 3) <<
        focal, 0,     crop_w / 2.0,
        0,     focal, crop_h / 2.0,
        0,     0,     1.0);
    const cv::Mat dist_coeffs = cv::Mat::zeros(4, 1, CV_64F);

    cv::Mat rvec, tvec;
    if (!cv::solvePnP(pts3d, pts2d, cam_mat, dist_coeffs, rvec, tvec,
                      false, cv::SOLVEPNP_ITERATIVE)) {
        return;
    }

    cv::Mat rmat;
    cv::Rodrigues(rvec, rmat);

    pitch = std::atan2( rmat.at<double>(2, 1),  rmat.at<double>(2, 2)) * 180.0 / CV_PI;
    yaw   = std::atan2(-rmat.at<double>(2, 0),
                        std::sqrt(rmat.at<double>(2, 1) * rmat.at<double>(2, 1) +
                                  rmat.at<double>(2, 2) * rmat.at<double>(2, 2))) * 180.0 / CV_PI;
}

// ── neck_angle ─────────────────────────────────────────────────────────

double LocalMediaPipePoseAnalyzer::compute_neck_angle(
    const std::vector<float>& lm, int frame_w, int frame_h) const
{
    const float sx = static_cast<float>(frame_w) / 256.0f;
    const float sy = static_cast<float>(frame_h) / 256.0f;

    const float ear_x = (lm[7 * 3]      + lm[8 * 3])      / 2.0f * sx;
    const float ear_y = (lm[7 * 3 + 1]  + lm[8 * 3 + 1])  / 2.0f * sy;
    const float sh_x  = (lm[11 * 3]     + lm[12 * 3])     / 2.0f * sx;
    const float sh_y  = (lm[11 * 3 + 1] + lm[12 * 3 + 1]) / 2.0f * sy;

    const float dx = ear_x - sh_x;
    const float dy = sh_y  - ear_y;  // y축 반전 (이미지 좌표계)
    return std::abs(std::atan2(dx, dy)) * 180.0 / CV_PI;
}

// ── shoulder_diff ───────────────────────────────────────────────────────

double LocalMediaPipePoseAnalyzer::compute_shoulder_diff(
    const std::vector<float>& lm, int frame_h) const
{
    const float sy = static_cast<float>(frame_h) / 256.0f;
    return std::abs(lm[11 * 3 + 1] - lm[12 * 3 + 1]) * sy;
}

// ── 종료 ───────────────────────────────────────────────────────────────

void LocalMediaPipePoseAnalyzer::shutdown()
{
    face_session_.reset();
    pose_session_.reset();
}
