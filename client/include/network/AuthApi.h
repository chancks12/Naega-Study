#pragma once

#include "model/User.h"
#include "network/WinHttpClient.h"

class AuthApi {
public:
    explicit AuthApi(WinHttpClient& http);

    AuthResponse login(const LoginRequest& req);
    AuthResponse register_user(const RegisterRequest& req);
    bool         logout();

private:
    static std::string escape_json(const std::string& value);
    static AuthResponse parse_auth_response(const HttpResponse& resp);
    static std::string extract_json_string(const std::string& json, const std::string& key);
    static int extract_json_int(const std::string& json, const std::string& key, int fallback);

    WinHttpClient& http_;
};
