#pragma once

#include <cstdarg>
#include <cstdio>

enum esp_log_level_t { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE };
using vprintf_like_t = int (*)(const char*, va_list);

namespace fake {
inline vprintf_like_t activeVprintf = nullptr;
inline int logLevel = -1;
inline bool emitNestedLog = false;
inline int emit(const char* format, ...);
inline int uartVprintf(const char* format, va_list args) {
    char buffer[1024];
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    if (length > 0) Serial.write(reinterpret_cast<const std::uint8_t*>(buffer),
        std::min(static_cast<std::size_t>(length), sizeof(buffer) - 1));
    if (emitNestedLog && activeVprintf) {
        emitNestedLog = false;
        emit("nested logger message\n");
    }
    return length;
}
inline int emit(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const auto function = activeVprintf ? activeVprintf : uartVprintf;
    const int result = function(format, args);
    va_end(args);
    return result;
}
}  // namespace fake

inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t function) {
    const auto previous = fake::activeVprintf ? fake::activeVprintf : fake::uartVprintf;
    fake::activeVprintf = function;
    return previous;
}

inline void esp_log_level_set(const char*, esp_log_level_t level) { fake::logLevel = level; }
