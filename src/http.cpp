#include "http.h"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string last_error(const char* what) { return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")"; }
}  // namespace

void http_global_init() {}

Http::Http() {
    h_ = WinHttpOpen(L"nq-replay-trader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h_)  // AUTOMATIC_PROXY needs Windows 8.1+
        h_ = WinHttpOpen(L"nq-replay-trader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0);
    if (h_) WinHttpSetTimeouts(h_, 30000, 30000, 120000, 120000);
}

Http::~Http() {
    if (c_) WinHttpCloseHandle(c_);
    if (h_) WinHttpCloseHandle(h_);
}

HttpResult Http::get(const std::string& host, const std::string& path, const std::string& header) {
    HttpResult r;
    if (!h_) return r.error = last_error("WinHttpOpen"), r;
    if (!c_ || host != host_) {
        if (c_) WinHttpCloseHandle(c_);
        c_ = WinHttpConnect(h_, widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        host_ = host;
        if (!c_) return r.error = last_error("WinHttpConnect"), r;
    }
    HINTERNET req = WinHttpOpenRequest(c_, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) return r.error = last_error("WinHttpOpenRequest"), r;
    std::wstring hdr = widen(header);
    bool ok = WinHttpSendRequest(req, hdr.c_str(), static_cast<DWORD>(hdr.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        r.error = last_error("HTTPS request");
        WinHttpCloseHandle(req);
        return r;
    }
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            r.status = 0, r.error = last_error("reading the response");
            break;
        }
        if (avail == 0) break;
        size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, &r.body[at], avail, &got)) {
            r.status = 0, r.error = last_error("reading the response");
            break;
        }
        r.body.resize(at + got);
    }
    WinHttpCloseHandle(req);
    return r;
}

#else

#include <curl/curl.h>

namespace {
size_t on_data(char* p, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(p, size * n);
    return size * n;
}
}  // namespace

void http_global_init() { curl_global_init(CURL_GLOBAL_DEFAULT); }

Http::Http() { h_ = curl_easy_init(); }

Http::~Http() {
    if (h_) curl_easy_cleanup(static_cast<CURL*>(h_));
}

HttpResult Http::get(const std::string& host, const std::string& path, const std::string& header) {
    HttpResult r;
    CURL* c = static_cast<CURL*>(h_);
    if (!c) return r.error = "curl_easy_init failed", r;
    std::string url = "https://" + host + path;
    curl_slist* hdrs = curl_slist_append(nullptr, header.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "nq-replay-trader/1.0");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);  // timeouts from worker threads
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        r.status = static_cast<int>(status);
    } else {
        r.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(hdrs);
    return r;
}

#endif
