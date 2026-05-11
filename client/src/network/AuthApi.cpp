#include "pch.h"
#include "network/AuthApi.h"

#include <cctype>
#include <sstream>

AuthApi::AuthApi(WinHttpClient& http)
    : http_(http)
{
}

AuthResponse AuthApi::login(const LoginRequest& req)
{
    std::ostringstream body;
    body << "{\"email\":\"" << escape_json(req.email)
         << "\",\"password\":\"" << escape_json(req.password) << "\"}";

    HttpResponse resp = http_.post_json("/auth/login", body.str());
    return parse_auth_response(resp);
}

bool AuthApi::logout()
{
    // POST /auth/logout — body 없음, Authorization 헤더는 WinHttpClient가 자동 주입
    const HttpResponse resp = http_.post_json("/auth/logout", "");
    return resp.ok();
}

AuthResponse AuthApi::register_user(const RegisterRequest& req)
{
    std::ostringstream body;
    body << "{\"email\":\"" << escape_json(req.email)
         << "\",\"password\":\"" << escape_json(req.password)
         << "\",\"name\":\"" << escape_json(req.name) << "\"}";

    HttpResponse resp = http_.post_json("/auth/register", body.str());
    return parse_auth_response(resp);
}

AuthResponse AuthApi::parse_auth_response(const HttpResponse& resp)
{
    AuthResponse auth;
    auth.success = resp.ok();

    // 서버가 "token" 또는 "access_token" 둘 중 하나를 사용할 수 있으므로 양쪽 시도
    auth.token = extract_json_string(resp.body, "token");
    if (auth.token.empty())
        auth.token = extract_json_string(resp.body, "access_token");

    // "name" 또는 "message" 양쪽 시도 (서버 구현마다 다를 수 있음)
    auth.message = extract_json_string(resp.body, "name");
    if (auth.message.empty())
        auth.message = extract_json_string(resp.body, "message");

    auth.user_id = extract_json_int(resp.body, "user_id", 0);

    if (auth.message.empty() && !auth.success)
        auth.message = extract_json_string(resp.body, "detail");

    // 디버그: 응답 body와 추출 결과 로깅
    OutputDebugStringA(("[Auth] parse_auth_response: success=" +
        std::to_string(auth.success) +
        " token_len=" + std::to_string(auth.token.size()) +
        " body_preview=" + resp.body.substr(0, 200) + "\n").c_str());

    return auth;
}

std::string AuthApi::extract_json_string(const std::string& json, const std::string& key)
{
    const std::string pattern = "\"" + key + "\":";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) return {};

    pos += pattern.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos; // 여는 따옴표 건너뜀

    std::string result;
    for (auto i = pos; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            result += json[++i];
        } else if (json[i] == '"') {
            break;
        } else {
            result += json[i];
        }
    }
    return result;
}

int AuthApi::extract_json_int(const std::string& json, const std::string& key, int fallback)
{
    std::string pattern = "\"" + key + "\":";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) {
        return fallback;
    }

    auto start = pos + pattern.size();
    while (start < json.size() && json[start] == ' ') {
        ++start;
    }

    std::string digits;
    for (auto i = start; i < json.size() && (std::isdigit(json[i]) || json[i] == '-'); ++i) {
        digits += json[i];
    }

    if (digits.empty()) {
        return fallback;
    }

    return std::stoi(digits);
}

std::string AuthApi::escape_json(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '"':  escaped += "\\\""; break;
        case '\n': escaped += "\\n";  break;
        case '\r': escaped += "\\r";  break;
        case '\t': escaped += "\\t";  break;
        default:   escaped += ch;     break;
        }
    }
    return escaped;
}
