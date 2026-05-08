#include "pch.h"
#include "network/JsonlBatchUploader.h"
#include "network/WinHttpClient.h"

#include <sstream>

JsonlBatchUploader::JsonlBatchUploader(std::size_t flush_threshold)
    : flush_threshold_(flush_threshold)
{
}

void JsonlBatchUploader::set_session_id(long long session_id)
{
    std::lock_guard<std::mutex> lock(mtx_);
    session_id_ = session_id;
}

void JsonlBatchUploader::append_analysis(const AnalysisResult& result)
{
    std::lock_guard<std::mutex> lock(mtx_);
    lines_.push_back(to_jsonl(result));
}

void JsonlBatchUploader::append_event_metadata(const PostureEvent& event)
{
    std::lock_guard<std::mutex> lock(mtx_);
    lines_.push_back(to_jsonl(event));
}

void JsonlBatchUploader::append_event_metadata(const PostureEvent& event, const ClipRef& clip_ref)
{
    std::lock_guard<std::mutex> lock(mtx_);
    lines_.push_back(to_jsonl(event, clip_ref));
}

std::string JsonlBatchUploader::drain_jsonl()
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::ostringstream out;
    for (const auto& line : lines_) {
        out << line << '\n';
    }
    lines_.clear();
    return out.str();
}

// ── 응답 파싱 헬퍼 (1-depth 평탄 JSON 전용) ──────────────────
static int extract_int(const std::string& j, const std::string& key, int fallback = 0)
{
    const std::string pat = "\"" + key + "\":";
    auto pos = j.find(pat);
    if (pos == std::string::npos) return fallback;
    pos += pat.size();
    while (pos < j.size() && j[pos] == ' ') ++pos;
    std::string digits;
    for (; pos < j.size() && (std::isdigit(static_cast<unsigned char>(j[pos])) || j[pos] == '-'); ++pos)
        digits += j[pos];
    return digits.empty() ? fallback : std::stoi(digits);
}

static const char* event_type_name(PostureEventType type)
{
    switch (type) {
    case PostureEventType::Drowsy: return "drowsy";
    case PostureEventType::Absent: return "absent";
    case PostureEventType::BadPosture:
    default: return "distracted";
    }
}

void JsonlBatchUploader::flush_to_http(const std::string& endpoint)
{
    const std::string body = drain_jsonl();
    if (body.empty()) return;

    const HttpResponse resp = WinHttpClient::instance().post_ndjson(endpoint, body);

    // 결과 디버그 출력
    std::ostringstream log;
    log << "[StudySync][JSONL] POST " << endpoint
        << " -> HTTP " << resp.status_code;

    if (resp.ok()) {
        // {"accepted":{"analysis":25,"event":5},"skipped":0}
        const int skipped          = extract_int(resp.body, "skipped");
        const int accepted_analysis = extract_int(resp.body, "analysis");
        const int accepted_event    = extract_int(resp.body, "event");
        log << "  accepted(analysis=" << accepted_analysis
            << " event=" << accepted_event
            << ") skipped=" << skipped;
    } else {
        log << "  body=" << resp.body;
    }
    log << "\n";
    OutputDebugStringA(log.str().c_str());
}

std::string JsonlBatchUploader::escape_json(const std::string& value)
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

std::string JsonlBatchUploader::to_jsonl(const AnalysisResult& result) const
{
    std::ostringstream out;
    out << "{\"kind\":\"analysis\""
        << ",\"session_id\":"    << session_id_
        << ",\"timestamp_ms\":"  << result.timestamp_ms
        << ",\"focus_score\":"   << result.focus_score
        << ",\"state\":\""       << escape_json(result.state) << "\""
        << ",\"ear\":"           << result.ear
        << ",\"neck_angle\":"    << result.neck_angle
        << ",\"shoulder_diff\":" << result.shoulder_diff
        << ",\"head_yaw\":"      << result.head_yaw
        << ",\"head_pitch\":"    << result.head_pitch
        << ",\"face_detected\":" << result.face_detected
        << ",\"posture_ok\":"    << (result.posture_ok ? "true" : "false")
        << ",\"drowsy\":"        << (result.drowsy     ? "true" : "false")
        << ",\"absent\":"        << (result.absent     ? "true" : "false")
        << ",\"confidence\":"    << result.confidence
        << "}";
    return out.str();
}

std::string JsonlBatchUploader::to_jsonl(const PostureEvent& event) const
{
    std::ostringstream out;
    out << "{\"kind\":\"event\""
        << ",\"session_id\":"   << session_id_
        << ",\"event_id\":\""   << escape_json(event.event_id) << "\""
        << ",\"event_type\":\"" << event_type_name(event.type) << "\""
        << ",\"timestamp_ms\":" << event.timestamp_ms
        << ",\"reason\":\""     << escape_json(event.reason) << "\""
        << ",\"confidence\":"   << event.confidence
        << ",\"frame_count\":"  << event.frames.size()
        << "}";
    return out.str();
}

std::string JsonlBatchUploader::to_jsonl(const PostureEvent& event, const ClipRef& clip_ref) const
{
    std::ostringstream out;
    out << "{\"kind\":\"event\""
        << ",\"session_id\":"   << session_id_
        << ",\"event_id\":\""   << escape_json(event.event_id) << "\""
        << ",\"event_type\":\"" << event_type_name(event.type) << "\""
        << ",\"timestamp_ms\":" << event.timestamp_ms
        << ",\"reason\":\""     << escape_json(event.reason) << "\""
        << ",\"frame_count\":"  << clip_ref.frame_count
        << ",\"clip_id\":\""    << escape_json(clip_ref.clip_id) << "\""
        << ",\"clip_ref\":\""   << escape_json(clip_ref.uri) << "\""
        << ",\"clip_access\":\"" << escape_json(clip_ref.access_kind) << "\""
        << ",\"clip_format\":\"" << escape_json(clip_ref.format) << "\""
        << ",\"retention_days\":" << clip_ref.retention_days
        << ",\"created_at_ms\":" << clip_ref.created_at_ms
        << ",\"expires_at_ms\":" << clip_ref.expires_at_ms
        << "}";
    return out.str();
}

