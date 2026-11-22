// Minimal HTTPS GET: libcurl on macOS/Linux (part of the OS on macOS), WinHTTP on Windows. One client per thread.
#pragma once

#include <string>

struct HttpResult {
    int status = 0;     // HTTP status, 0 when the request failed before a response
    std::string body;
    std::string error;  // transport error text when status == 0
};

class Http {
public:
    Http();
    ~Http();
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;
    // GET https://<host><path> with the given extra header ("Name: value").
    HttpResult get(const std::string& host, const std::string& path, const std::string& header);

private:
    void* h_ = nullptr;  // CURL* or WinHTTP session
    void* c_ = nullptr;  // WinHTTP connection
    std::string host_;
};

void http_global_init();  // call once, before any thread creates an Http
