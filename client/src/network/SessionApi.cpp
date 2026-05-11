#include "pch.h"
#include "network/SessionApi.h"

#include <sstream>
#include <windows.h>

SessionApi::SessionApi(WinHttpClient& http)
    : http_(http)
{
}

SessionStartResult SessionApi::start(const std::string& iso_start_time)
{
    std::ostringstream body;
    body << "{\"start_time\":\"" << iso_start_time << "\"}";

    const HttpResponse resp = http_.post_json("/session/start", body.str());

    SessionStartResult result;
    result.success    = resp.ok();
    result.session_id = extract_int64(resp.body, "session_id");
    result.message    = extract_str(resp.body, "message");
    return result;
}

SessionEndResult SessionApi::end(long long session_id, const std::string& iso_end_time)
{
    std::ostringstream body;
    body << "{\"session_id\":" << session_id
         << ",\"end_time\":\"" << iso_end_time << "\"}";

    const HttpResponse resp = http_.post_json("/session/end", body.str());

    SessionEndResult result;
    result.success       = resp.ok();
    result.focus_min     = extract_int  (resp.body, "focus_min");     // INT
    result.avg_focus     = extract_float(resp.body, "avg_focus");     // float (0.0~1.0)
    result.goal_achieved = extract_bool (resp.body, "goal_achieved"); // bool (true/false 리터럴)
    result.message       = extract_str  (resp.body, "message");
    return result;
}

// ── 유틸리티 ─────────────────────────────────────────────────

std::string SessionApi::now_iso8601()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d+09:00",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return buf;
}

long long SessionApi::extract_int64(const std::string& json, const std::string& key)
{
    const std::string pat = "\"" + key + "\":";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return 0;

    pos += pat.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;

    std::string digits;
    for (auto i = pos; i < json.size(); ++i) {
        if (std::isdigit(json[i]) || json[i] == '-') digits += json[i];
        else break;
    }
    return digits.empty() ? 0LL : std::stoll(digits);
}

int SessionApi::extract_int(const std::string& json, const std::string& key)
{
    return static_cast<int>(extract_int64(json, key));
}

float SessionApi::extract_float(const std::string& json, const std::string& key)
{
    const std::string pat = "\"" + key + "\":";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return 0.0f;

    pos += pat.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;

    std::string val;
    for (auto i = pos; i < json.size(); ++i) {
        const char c = json[i];
        if (std::isdigit(c) || c == '-' || c == '.' || c == 'e' || c == 'E') val += c;
        else break;
    }
    return val.empty() ? 0.0f : std::stof(val);
}

bool SessionApi::extract_bool(const std::string& json, const std::string& key)
{
    // "key":true  또는  "key":false  형태 파싱 (따옴표 없음)
    const std::string pat = "\"" + key + "\":";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return false;

    pos += pat.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    return json.compare(pos, 4, "true") == 0;
}

std::string SessionApi::extract_str(const std::string& json, const std::string& key)
{
    const std::string pat = "\"" + key + "\":";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return {};

    pos += pat.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;

    std::string result;
    for (auto i = pos; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) result += json[++i];
        else if (json[i] == '"') break;
        else result += json[i];
    }
    return result;
}
