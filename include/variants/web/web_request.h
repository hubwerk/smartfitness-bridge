#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace web {

// The web UI is only served over HTTPS (port 443); port 80 just redirects.
inline bool trustedOrigin(std::string_view origin, std::string_view staIp, std::string_view apIp) {
    for (const auto host : {std::string_view("smartfitness.local"), staIp, apIp}) {
        if (host.empty() || host == "0.0.0.0") continue;
        const std::string expected = "https://" + std::string(host);
        if (origin == expected || origin == expected + ":443") return true;
    }
    return false;
}

// Host header value without an optional ":port" suffix.
inline std::string_view hostName(std::string_view host) {
    const std::size_t colon = host.find(':');
    return colon == std::string_view::npos ? host : host.substr(0, colon);
}

// True if `host` (Host header, optional port) names this bridge.
inline bool ownHost(std::string_view host, std::string_view staIp, std::string_view apIp) {
    const std::string_view name = hostName(host);
    for (const auto own : {std::string_view("smartfitness.local"), staIp, apIp}) {
        if (!own.empty() && own != "0.0.0.0" && name == own) return true;
    }
    return false;
}

// Target of the plain-HTTP redirect: the same path on HTTPS for the bridge's own
// names; captive-portal probes and foreign hosts land on the UI root of the
// SoftAP (or, without an AP, the station) address.
inline std::string httpsRedirect(std::string_view host, std::string_view uri, std::string_view staIp,
                                 std::string_view apIp) {
    bool cleanPath = !uri.empty() && uri.front() == '/' && (uri.size() < 2 || uri[1] != '/');
    for (const char c : uri) {
        if (static_cast<unsigned char>(c) <= ' ' || c == 0x7f) cleanPath = false;
    }
    if (ownHost(host, staIp, apIp)) return "https://" + std::string(hostName(host)) + (cleanPath ? std::string(uri) : "/");
    const std::string_view fallback = !apIp.empty() && apIp != "0.0.0.0" ? apIp : staIp;
    return "https://" + std::string(fallback) + "/";
}

// Status line for esp_http_server (it needs the reason phrase).
inline const char* statusLine(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 202: return "202 Accepted";
        case 302: return "302 Found";
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 413: return "413 Payload Too Large";
        case 415: return "415 Unsupported Media Type";
        case 503: return "503 Service Unavailable";
        default: return "500 Internal Server Error";
    }
}

inline int postRequestStatus(std::string_view contentType, bool hasOrigin, std::string_view origin,
                             std::string_view staIp, std::string_view apIp, bool transferEncoding = false) {
    contentType = contentType.substr(0, contentType.find(';'));
    while (!contentType.empty() && (contentType.front() == ' ' || contentType.front() == '\t'))
        contentType.remove_prefix(1);
    while (!contentType.empty() && (contentType.back() == ' ' || contentType.back() == '\t'))
        contentType.remove_suffix(1);
    std::string mediaType(contentType);
    for (char& c : mediaType) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    if (mediaType != "application/json") return 415;
    if (hasOrigin && !trustedOrigin(origin, staIp, apIp)) return 403;
    // Bodies are read by Content-Length; chunked requests are not supported.
    if (transferEncoding) return 400;
    return 200;
}

// Tracks a request body received in pieces into a zero-initialized total+1 byte buffer.
struct BodyReceipt {
    std::size_t total;
    std::size_t received;
    bool invalid;

    void append(char* buffer, const void* data, std::size_t length, std::size_t index, std::size_t expected) {
        if (invalid) return;
        if (expected != total || index != received || received > total || length > total - received ||
            (length && (!data || std::memchr(data, '\0', length)))) {
            invalid = true;
            return;
        }
        if (length) std::memcpy(buffer + received, data, length);
        received += length;
    }
};

inline int bodyStatus(const BodyReceipt* receipt, std::size_t contentLength, bool allocationFailed) {
    if (allocationFailed) return 500;
    if (!receipt) return contentLength == 0 ? 200 : 400;
    return !receipt->invalid && receipt->total == contentLength && receipt->received == receipt->total ? 200 : 400;
}

inline bool canBroadcast(std::size_t clients, bool available) { return clients > 0 && available; }

}  // namespace web
