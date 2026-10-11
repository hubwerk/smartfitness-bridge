#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace web {

inline bool trustedOrigin(std::string_view origin, std::string_view staIp, std::string_view apIp) {
    for (const auto host : {std::string_view("smartfitness.local"), staIp, apIp}) {
        if (host.empty() || host == "0.0.0.0") continue;
        const std::string expected = "http://" + std::string(host);
        if (origin == expected || origin == expected + ":80") return true;
    }
    return false;
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
    // The server's body callback does not provide a final decoded total for chunked requests.
    if (transferEncoding) return 400;
    return 200;
}

// Trivially destructible metadata followed by a zero-initialized total+1 byte buffer.
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
