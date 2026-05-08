#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <winhttp.h>

struct HttpResponse {
    int status_code = 0;
    std::string body;
    bool ok() const { return status_code >= 200 && status_code < 300; }
};

// multipart/form-data 필드 하나를 표현
// filename이 비어 있으면 텍스트 필드, 비어 있지 않으면 파일 파트
struct MultipartField {
    std::string name;                   // form field name (필수)
    std::string value;                  // 텍스트 값 (텍스트 필드용)
    std::string filename;               // 파일명 (파일 파트용)
    std::string content_type;           // MIME 타입 (기본: application/octet-stream)
    std::vector<uint8_t> data;          // 바이너리 데이터 (파일 파트용)
};

class WinHttpClient {
public:
    static WinHttpClient& instance();
    WinHttpClient();
    ~WinHttpClient();

    void set_base_url(const std::string& base_url);
    void set_token(const std::string& jwt);
    void clear_token();
    void set_unauthorized_callback(std::function<void()> cb);

    HttpResponse get(const std::string& path);
    HttpResponse post_json   (const std::string& path, const std::string& json_body);
    HttpResponse post_ndjson (const std::string& path, const std::string& ndjson_body);
    HttpResponse post_multipart(const std::string& path,
                                const std::vector<MultipartField>& fields);

private:
    WinHttpClient(const WinHttpClient&) = delete;
    WinHttpClient& operator=(const WinHttpClient&) = delete;

    struct UrlParts {
        bool https = false;
        std::wstring host;
        std::uint16_t port = 80;
    };

    UrlParts parse_base_url() const;
    HttpResponse send_request(const wchar_t* verb, const std::string& path,
                              const std::string& body, const wchar_t* content_type);
    static std::string build_multipart_body(const std::vector<MultipartField>& fields,
                                            const std::string& boundary);
    static std::wstring to_wide(const std::string& s);
    static std::string to_utf8(const wchar_t* ws, int len);

    std::mutex mtx_;
    HINTERNET session_ = nullptr;
    std::string base_url_;
    std::string token_;
    std::function<void()> unauthorized_cb_;
};
