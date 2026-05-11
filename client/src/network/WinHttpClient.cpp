#include "pch.h"
#include "network/WinHttpClient.h"

#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

WinHttpClient& WinHttpClient::instance()
{
    static WinHttpClient inst;
    return inst;
}

WinHttpClient::WinHttpClient()
{
    session_ = WinHttpOpen(
        L"StudySyncClient/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,   // 시스템 프록시 무시 — LAN 서버 직접 접속
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (session_) {
        // 타임아웃 명시: resolve=5s, connect=10s, send=30s, receive=60s
        WinHttpSetTimeouts(session_, 5000, 10000, 30000, 60000);
    }
}

WinHttpClient::~WinHttpClient()
{
    if (session_) {
        WinHttpCloseHandle(session_);
    }
}

void WinHttpClient::set_base_url(const std::string& base_url)
{
    std::lock_guard<std::mutex> lock(mtx_);
    base_url_ = base_url;
}

void WinHttpClient::set_token(const std::string& jwt)
{
    std::lock_guard<std::mutex> lock(mtx_);
    token_ = jwt;
}

void WinHttpClient::clear_token()
{
    std::lock_guard<std::mutex> lock(mtx_);
    token_.clear();
}

void WinHttpClient::set_unauthorized_callback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(mtx_);
    unauthorized_cb_ = std::move(cb);
}

HttpResponse WinHttpClient::get(const std::string& path)
{
    return send_request(L"GET", path, {}, nullptr);
}

HttpResponse WinHttpClient::post_json(const std::string& path, const std::string& json_body)
{
    return send_request(L"POST", path, json_body, L"application/json");
}

HttpResponse WinHttpClient::post_ndjson(const std::string& path, const std::string& ndjson_body)
{
    return send_request(L"POST", path, ndjson_body, L"application/x-ndjson");
}

HttpResponse WinHttpClient::post_multipart(
    const std::string& path,
    const std::vector<MultipartField>& fields)
{
    // RFC 2046 §5.1.1 — boundary는 1~70자, 공백 금지
    const std::string boundary = "----StudySyncBoundary7MA4YWxkTrZu0gW";
    const std::string body     = build_multipart_body(fields, boundary);
    const std::string ct_str   = "multipart/form-data; boundary=" + boundary;
    const std::wstring wide_ct = to_wide(ct_str);
    return send_request(L"POST", path, body, wide_ct.c_str());
}

// static
std::string WinHttpClient::build_multipart_body(
    const std::vector<MultipartField>& fields,
    const std::string& boundary)
{
    const std::string crlf          = "\r\n";
    const std::string dash_boundary = "--" + boundary;

    std::string body;
    body.reserve(4096);

    for (const auto& f : fields) {
        body += dash_boundary + crlf;

        if (f.filename.empty()) {
            // ── 텍스트 필드 ─────────────────────────────────────
            body += "Content-Disposition: form-data; name=\"" + f.name + "\"" + crlf;
            body += crlf;
            body += f.value;
            body += crlf;
        } else {
            // ── 파일 파트 ───────────────────────────────────────
            body += "Content-Disposition: form-data; name=\"" + f.name
                  + "\"; filename=\"" + f.filename + "\"" + crlf;
            const std::string ct = f.content_type.empty()
                                   ? "application/octet-stream"
                                   : f.content_type;
            body += "Content-Type: " + ct + crlf;
            body += crlf;
            // binary data — std::string은 null 바이트 포함 가능
            body.append(reinterpret_cast<const char*>(f.data.data()), f.data.size());
            body += crlf;
        }
    }

    body += dash_boundary + "--" + crlf;   // closing boundary
    return body;
}

WinHttpClient::UrlParts WinHttpClient::parse_base_url() const
{
    UrlParts parts;
    std::string url = base_url_;

    if (url.rfind("https://", 0) == 0) {
        parts.https = true;
        url = url.substr(8);
        parts.port = 443;
    } else if (url.rfind("http://", 0) == 0) {
        url = url.substr(7);
        parts.port = 80;
    }

    // host:port 분리
    auto colon = url.find(':');
    if (colon != std::string::npos) {
        parts.host = to_wide(url.substr(0, colon));
        parts.port = static_cast<std::uint16_t>(std::stoi(url.substr(colon + 1)));
    } else {
        parts.host = to_wide(url);
    }

    return parts;
}

HttpResponse WinHttpClient::send_request(
    const wchar_t* verb,
    const std::string& path,
    const std::string& body,
    const wchar_t* content_type)
{
    HttpResponse resp;

    if (!session_) {
        return resp;
    }

    UrlParts parts;
    std::string token_copy;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        parts = parse_base_url();
        token_copy = token_;
    }

    DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;

    // 방어: path에 절대 URL이 넘어왔다면 path 부분만 추출
    // 예) "http://10.10.10.130:8081/log/ingest" → "/log/ingest"
    std::string clean_path = path;
    {
        const auto http_pos = clean_path.find("://");
        if (http_pos != std::string::npos) {
            const auto slash_pos = clean_path.find('/', http_pos + 3);
            clean_path = (slash_pos != std::string::npos)
                         ? clean_path.substr(slash_pos)
                         : "/";
        }
    }
    std::wstring wide_path = to_wide(clean_path);

    // 연결 대상 디버그 출력 (요청이 어디로 가는지 확인용)
    {
        char dbg[256];
        std::string host_utf8 = to_utf8(parts.host.c_str(), static_cast<int>(parts.host.size()));
        snprintf(dbg, sizeof(dbg),
            "[WinHttp] -> %s:%d%s\n",
            host_utf8.c_str(), parts.port, clean_path.c_str());
        OutputDebugStringA(dbg);
    }

    HINTERNET connect = WinHttpConnect(
        session_, parts.host.c_str(), parts.port, 0);
    if (!connect) {
        char dbg[256];
        std::string host_utf8 = to_utf8(parts.host.c_str(), static_cast<int>(parts.host.size()));
        snprintf(dbg, sizeof(dbg),
            "[WinHttp] Connect FAILED  host=%s  port=%d  err=0x%08X\n",
            host_utf8.c_str(), parts.port, static_cast<unsigned>(GetLastError()));
        OutputDebugStringA(dbg);
        return resp;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect, verb, wide_path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        return resp;
    }

    // JWT Bearer 토큰 주입
    if (!token_copy.empty()) {
        std::wstring auth_header = L"Authorization: Bearer " + to_wide(token_copy);
        WinHttpAddRequestHeaders(request, auth_header.c_str(),
            static_cast<DWORD>(auth_header.size()), WINHTTP_ADDREQ_FLAG_ADD);
    }

    // Content-Type 헤더
    if (content_type) {
        std::wstring ct_header = std::wstring(L"Content-Type: ") + content_type;
        WinHttpAddRequestHeaders(request, ct_header.c_str(),
            static_cast<DWORD>(ct_header.size()), WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL sent = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);

    if (!sent) {
        char dbg[256];
        snprintf(dbg, sizeof(dbg),
            "[WinHttp] SendRequest FAILED  path=%s  err=0x%08X\n",
            path.c_str(), static_cast<unsigned>(GetLastError()));
        OutputDebugStringA(dbg);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        return resp;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        char dbg[256];
        snprintf(dbg, sizeof(dbg),
            "[WinHttp] ReceiveResponse FAILED  path=%s  err=0x%08X\n",
            path.c_str(), static_cast<unsigned>(GetLastError()));
        OutputDebugStringA(dbg);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        return resp;
    }

    // 상태 코드 읽기
    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size,
        WINHTTP_NO_HEADER_INDEX);
    resp.status_code = static_cast<int>(status_code);

    if (resp.status_code == 401) {
        std::function<void()> cb;
        { std::lock_guard<std::mutex> lock(mtx_); cb = unauthorized_cb_; }
        if (cb) cb();
    }

    // 응답 본문 읽기
    std::ostringstream response_body;
    DWORD bytes_available = 0;
    while (WinHttpQueryDataAvailable(request, &bytes_available) && bytes_available > 0) {
        std::vector<char> buf(bytes_available);
        DWORD bytes_read = 0;
        if (WinHttpReadData(request, buf.data(), bytes_available, &bytes_read)) {
            response_body.write(buf.data(), bytes_read);
        }
    }
    resp.body = response_body.str();

    // 응답 디버그 출력 (status + body 앞 300자)
    {
        char dbg[512];
        const std::string preview = resp.body.substr(0, 300);
        snprintf(dbg, sizeof(dbg),
            "[WinHttp] <- %d  body=%s\n",
            resp.status_code, preview.c_str());
        OutputDebugStringA(dbg);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    return resp;
}

std::wstring WinHttpClient::to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), ws.data(), len);
    return ws;
}

std::string WinHttpClient::to_utf8(const wchar_t* ws, int len)
{
    if (!ws || len == 0) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, ws, len, nullptr, 0, nullptr, nullptr);
    std::string s(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws, len, s.data(), size, nullptr, nullptr);
    return s;
}
