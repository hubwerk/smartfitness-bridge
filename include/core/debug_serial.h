#pragma once

#ifdef DEBUG_WEB
#include <cstddef>
#include <cstdint>

namespace debugweb {
constexpr std::size_t kMaxLogBytes = 512 * 1024;
void begin();
struct Chunk {
    std::size_t size = 0;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    bool reset = false;
};
Chunk readChunk(std::uint64_t offset, char* data, std::size_t capacity);
using LogSink = bool (*)(const char*, std::size_t, std::uint64_t, void*);
bool streamLog(LogSink sink, void* context);
}  // namespace debugweb

#ifdef ARDUINO
#include <Arduino.h>

// Healthy output is queued; initialization failure falls back to task-context
// Serial. Producers never touch LittleFS.
class DebugSerial final : public Print {
public:
    using Print::write;
    std::size_t write(std::uint8_t byte) override;
    std::size_t write(const std::uint8_t* data, std::size_t size) override;
    std::size_t printf(const char* format, ...) __attribute__((format(printf, 2, 3)));
};
extern DebugSerial debugSerial;
#define DBG_SERIAL debugSerial
#endif
#else
#define DBG_SERIAL Serial
#endif
