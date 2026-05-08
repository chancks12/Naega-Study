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
    const HttpResponse resp = http_.post_json("/auth/logout", "{}");
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
    auth.token   = extract_json_string(resp.body, "access_token");
    auth.message = extract_json_string(resp.body, "message");
    auth.user_id = extract_json_int(resp.body, "user_id", 0);

    if (auth.message.empty() && !auth.success) {
        auth.message = extract_json_string(resp.body, "detail");
    }

    return auth;
}

std::string AuthApi::extract_json_string(const std::string& json, const std::string& key)
{
    std::string pattern = "\"" + key + "\":\"";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) {
        return {};
    }

    auto start = pos + pattern.size();
    std::string result;
    for (auto i = start; i < json.size(); ++i) {
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
