#include "pch.h"
#include "network/LocalClaimCheckClipStore.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <sstream>
#include <vector>

namespace {
std::string escape_json(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += ch; break;
        }
    }
    return escaped;
}

std::uint64_t now_ms()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

const char* event_type_name(PostureEventType type)
{
    switch (type) {
    case PostureEventType::Drowsy:    return "drowsy";
    case PostureEventType::Absent:    return "absent";
    case PostureEventType::Focus:     return "focus";
    case PostureEventType::BadPosture:
    default:                          return "distracted";
    }
}
}

LocalClaimCheckClipStore::LocalClaimCheckClipStore(std::string clip_directory, std::uint32_t retention_days)
    : clip_directory_(std::move(clip_directory))
    , retention_days_(retention_days)
{
}

ClipRef LocalClaimCheckClipStore::store_clip(const PostureEvent& event)
{
    namespace fs = std::filesystem;

    const std::string clip_id = "local:event_" + std::to_string(event.timestamp_ms);
    const std::uint64_t created_at = now_ms();
    const std::uint64_t expires_at = created_at + static_cast<std::uint64_t>(retention_days_) * 24ULL * 60ULL * 60ULL * 1000ULL;

    char time_str[8]{};
    {
        const time_t ts = static_cast<time_t>(event.timestamp_ms / 1000);
        struct tm ltm{};
        localtime_s(&ltm, &ts);
        strftime(time_str, sizeof(time_str), "%H%M%S", &ltm);
    }
    const std::string event_dir_name = std::string(time_str) + "_" + event_type_name(event.type);
    fs::path event_dir = fs::path(clip_directory_) / event_dir_name;
    fs::create_directories(event_dir);

    std::size_t written = 0;
    const fs::path mp4_path = event_dir / "clip.mp4";

    const cv::Mat* first_valid = nullptr;
    for (const auto& f : event.frames) {
        if (!f.mat.empty()) { first_valid = &f.mat; break; }
    }

    if (first_valid) {
        const cv::Size frame_size(first_valid->cols, first_valid->rows);
        const double write_fps = event.camera_fps > 0 ? static_cast<double>(event.camera_fps) : 30.0;
        cv::VideoWriter writer(
            mp4_path.string(),
            cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
            write_fps,
            frame_size);

        if (writer.isOpened()) {
            for (const auto& f : event.frames) {
                if (!f.mat.empty()) {
                    writer.write(f.mat);
                    ++written;
                }
            }
        }
    }

    const fs::path manifest_path = event_dir / "manifest.jsonl";
    std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
    manifest << "{\"kind\":\"event_clip\""
             << ",\"clip_id\":\"" << escape_json(clip_id) << "\""
             << ",\"clip_access\":\"local_only\""
             << ",\"clip_format\":\"mp4\""
             << ",\"event_type\":\"" << event_type_name(event.type) << "\""
             << ",\"timestamp_ms\":" << event.timestamp_ms
             << ",\"reason\":\"" << escape_json(event.reason) << "\""
             << ",\"confidence\":" << event.confidence
             << ",\"frame_count\":" << written
             << ",\"retention_days\":" << retention_days_
             << ",\"created_at_ms\":" << created_at
             << ",\"expires_at_ms\":" << expires_at
             << "}\n";

    ClipRef ref;
    ref.clip_id = clip_id;
    ref.uri = mp4_path.string();
    ref.access_kind = "local_only";
    ref.format = "mp4";
    ref.frame_count = written;
    ref.retention_days = retention_days_;
    ref.created_at_ms = created_at;
    ref.expires_at_ms = expires_at;
    return ref;
}
