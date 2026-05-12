#include "pch.h"
#include "analysis/LocalMediaPipePoseAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opencv2/imgproc.hpp>

namespace {

const int kLeftEye[]  = {33, 160, 158, 133, 153, 144};
const int kRightEye[] = {362, 385, 387, 263, 373, 380};

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

std::string wpath_to_str(const std::wstring& wp)
{
    if (wp.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, wp.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, wp.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

double euclidean(float x1, float y1, float x2, float y2)
{
    const float dx = x1 - x2, dy = y1 - y2;
    return std::sqrt(static_cast<double>(dx * dx + dy * dy));
}

} // namespace

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

    {
        Ort::AllocatorWithDefaultOptions alloc;
        const size_t n_out = pose_session_->GetOutputCount();
        for (size_t i = 0; i < n_out; ++i) {
            auto name_alloc = pose_session_->GetOutputNameAllocated(i, alloc);
            const std::string name(name_alloc.get());
            if (i == 0) pose_out0_name_ = name;
            else if (i == 1) pose_out1_name_ = name;
        }
        if (pose_out0_name_.empty()) pose_out0_name_ = "Identity";
        if (pose_out1_name_.empty()) pose_out1_name_ = "Identity_1";
    }

    {
        const std::string local_cascade = wpath_to_str(model_path(L"haarcascade_frontalface_default.xml"));
        if (!local_cascade.empty()) face_cascade_.load(local_cascade);
        if (face_cascade_.empty())
            face_cascade_.load("C:/opencv/build/etc/haarcascades/haarcascade_frontalface_default.xml");
        if (face_cascade_.empty())
            OutputDebugStringA("[LocalPose] face cascade not found — full-frame crop fallback\n");
    }

    initialized_ = true;
    OutputDebugStringA("[LocalPose] ONNX models loaded OK\n");
    return true;
}

std::optional<AnalysisResult> LocalMediaPipePoseAnalyzer::analyze(const Frame& frame)
{
    if (!initialized_ || frame.mat.empty()) return std::nullopt;

    AnalysisResult result;
    result.timestamp_ms = frame.timestamp_ms;

    const int W = frame.mat.cols;
    const int H = frame.mat.rows;

    Ort::MemoryInfo mem_info =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // ── 1. 얼굴 bbox 감지 + EMA ──────────────────────────────────────────────
    cv::Rect face_rect;
    {
        // Haar cascade는 5프레임마다 한 번 실행. 중간 프레임은 EMA 값 유지.
        // detectMultiScale이 ~15ms를 써서 5프레임 평균 ~3ms로 줄임.
        const bool run_haar = (!face_cascade_.empty()) && ((haar_frame_count_++ % 5) == 0);
        if (run_haar) {
            cv::Mat gray;
            cv::cvtColor(frame.mat, gray, cv::COLOR_BGR2GRAY);
            cv::equalizeHist(gray, gray);

            std::vector<cv::Rect> faces;
            face_cascade_.detectMultiScale(gray, faces, 1.1, 3, 0, {80, 80});
            if (!faces.empty()) {
                const cv::Rect best = *std::max_element(faces.begin(), faces.end(),
                    [](const cv::Rect& a, const cv::Rect& b){ return a.area() < b.area(); });
                const cv::Rect2f bf(static_cast<float>(best.x),   static_cast<float>(best.y),
                                    static_cast<float>(best.width), static_cast<float>(best.height));
                constexpr float kA = 0.25f;
                if (ema_face_rect_.area() == 0.0f) {
                    ema_face_rect_ = bf;
                } else {
                    ema_face_rect_.x      = kA * bf.x      + (1.f - kA) * ema_face_rect_.x;
                    ema_face_rect_.y      = kA * bf.y      + (1.f - kA) * ema_face_rect_.y;
                    ema_face_rect_.width  = kA * bf.width  + (1.f - kA) * ema_face_rect_.width;
                    ema_face_rect_.height = kA * bf.height + (1.f - kA) * ema_face_rect_.height;
                }
            }
        }

        if (ema_face_rect_.area() > 0.0f) {
            const int rx = std::max(0, static_cast<int>(ema_face_rect_.x));
            const int ry = std::max(0, static_cast<int>(ema_face_rect_.y));
            const int rw = std::min(W - rx, static_cast<int>(ema_face_rect_.width));
            const int rh = std::min(H - ry, static_cast<int>(ema_face_rect_.height));
            face_rect = (rw > 0 && rh > 0) ? cv::Rect(rx, ry, rw, rh)
                                             : cv::Rect(W / 4, 0, W / 2, H / 2);
        } else {
            face_rect = cv::Rect(W / 4, 0, W / 2, H / 2);
        }
    }

    // ── 2. 상체 크롭 ─────────────────────────────────────────────────────────
    cv::Rect body_crop;
    {
        const int face_cx = face_rect.x + face_rect.width / 2;
        const int bw      = std::min(W, face_rect.width * 4);
        const int bh      = std::min(H, face_rect.height * 4);
        const int bx      = std::max(0, face_cx - bw / 2);
        const int by      = std::max(0, face_rect.y - face_rect.height / 2);
        body_crop = cv::Rect(bx, by,
                             std::min(W - bx, bw),
                             std::min(H - by, bh));
    }

    // ── 3. Pose Landmark (256×256) ──────────────────────────────────────────
    bool pose_detected = false;
    {
        cv::Mat inp;
        cv::resize(frame.mat(body_crop), inp, {256, 256});
        cv::cvtColor(inp, inp, cv::COLOR_BGR2RGB);
        inp.convertTo(inp, CV_32F, 1.0 / 255.0);

        std::vector<float> flat(256 * 256 * 3);
        std::memcpy(flat.data(), inp.data, flat.size() * sizeof(float));

        const std::array<int64_t, 4> shape = {1, 256, 256, 3};
        auto tensor = Ort::Value::CreateTensor<float>(
            mem_info, flat.data(), flat.size(), shape.data(), 4);

        const char* in_names[] = {"input_1"};
        const char* out_names[2] = { pose_out0_name_.c_str(), pose_out1_name_.c_str() };

        try {
            auto outs = pose_session_->Run(
                Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 2);

            const float raw_flag = outs[1].GetTensorData<float>()[0];
            const float conf = 1.0f / (1.0f + std::exp(-raw_flag));

            if (conf > 0.5f) {
                const float* lm_ptr = outs[0].GetTensorData<float>();
                const size_t elem_count = outs[0].GetTensorTypeAndShapeInfo().GetElementCount();

                int stride = 3;
                if      (elem_count % 39 == 0) stride = static_cast<int>(elem_count / 39);
                else if (elem_count % 33 == 0) stride = static_cast<int>(elem_count / 33);
                else if (elem_count % 5 == 0 && elem_count / 5 >= 33) stride = 5;

                const size_t min_needed = static_cast<size_t>(12 * stride + stride);
                if (elem_count >= min_needed) {
                    const std::vector<float> lm(lm_ptr, lm_ptr + elem_count);

                    bool vis_ok = true;
                    if (stride >= 4) {
                        const float vis_ear  = lm[7  * stride + 3];
                        const float vis_shl  = lm[11 * stride + 3];
                        const float vis_shr  = lm[12 * stride + 3];
                        vis_ok = (vis_ear >= 0.5f && vis_shl >= 0.5f && vis_shr >= 0.5f);
                    }

                    if (vis_ok) {
                        const double raw_neck  = compute_neck_angle(lm, stride, body_crop.width, body_crop.height);
                        const double raw_sdiff = compute_shoulder_diff(lm, stride, body_crop.height);
                        if (raw_neck <= 160.0) {
                            constexpr double kB = 0.2;
                            if (!has_ema_) {
                                ema_neck_angle_    = raw_neck;
                                ema_shoulder_diff_ = raw_sdiff;
                                has_ema_           = true;
                            } else {
                                ema_neck_angle_    = kB * raw_neck  + (1.0 - kB) * ema_neck_angle_;
                                ema_shoulder_diff_ = kB * raw_sdiff + (1.0 - kB) * ema_shoulder_diff_;
                            }
                            result.neck_angle    = ema_neck_angle_;
                            result.shoulder_diff = ema_shoulder_diff_;
                            pose_detected        = true;
                        }
                    }
                }
            }
        } catch (const Ort::Exception& e) {
            OutputDebugStringA(("[LocalPose] pose run: " + std::string(e.what()) + "\n").c_str());
        }

        if (!pose_detected) {
            result.neck_angle    = 0.0;
            result.shoulder_diff = 0.0;
            has_ema_             = false;
        }
    }

    // ── 4. Face Landmark (192×192) ──────────────────────────────────────────
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
                compute_head_pose(lm468, padded.width, padded.height, W,
                                  result.head_yaw, result.head_pitch);
            } else {
                result.face_detected = 0;
            }
        } catch (const Ort::Exception& e) {
            OutputDebugStringA(("[LocalPose] face run: " + std::string(e.what()) + "\n").c_str());
            result.face_detected = 0;
        }
    }

    if (result.face_detected == 0) {
        result.neck_angle    = 0.0;
        result.shoulder_diff = 0.0;
        has_ema_             = false;
        pose_detected        = false;
    }

    // ── 5. 최종 판정 ─────────────────────────────────────────────
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

void LocalMediaPipePoseAnalyzer::compute_head_pose(
    const std::vector<float>& lm, int crop_w, int /*crop_h*/,
    int frame_w, double& yaw, double& pitch) const
{
    yaw = pitch = 0.0;
    if (frame_w <= 0) return;
    auto lx = [&](int i) -> double { return lm[i * 3]; };
    auto ly = [&](int i) -> double { return lm[i * 3 + 1]; };
    yaw   = (lx(454) - lx(234)) * 100.0 * static_cast<double>(crop_w)
            / (192.0 * static_cast<double>(frame_w));
    const double dy_p = ly(152) - ly(1);
    const double dx_p = lx(152) - lx(1);
    pitch = std::atan2(dy_p, dx_p) * 180.0 / CV_PI - 90.0;
}

double LocalMediaPipePoseAnalyzer::compute_neck_angle(
    const std::vector<float>& lm, int stride, int frame_w, int frame_h) const
{
    const float sx = static_cast<float>(frame_w) / 256.0f;
    const float sy = static_cast<float>(frame_h) / 256.0f;
    const float ear_x = lm[7 * stride]      * sx;
    const float ear_y = lm[7 * stride + 1]  * sy;
    const float sh_x  = lm[11 * stride]     * sx;
    const float sh_y  = lm[11 * stride + 1] * sy;
    const float dx = std::abs(ear_x - sh_x);
    const float dy = std::abs(ear_y - sh_y);
    return static_cast<double>(std::atan2(dx, dy)) * 180.0 / CV_PI;
}

double LocalMediaPipePoseAnalyzer::compute_shoulder_diff(
    const std::vector<float>& lm, int stride, int frame_h) const
{
    const float sy = static_cast<float>(frame_h) / 256.0f;
    return std::abs(lm[11 * stride + 1] - lm[12 * stride + 1]) * sy;
}

void LocalMediaPipePoseAnalyzer::shutdown()
{
    face_session_.reset();
    pose_session_.reset();
}
