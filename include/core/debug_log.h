#pragma once

#if defined(ARDUINO) && defined(DEBUG_WEB)
#include "debug_serial.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>

#define SF_DEBUG_LOG(...) DBG_SERIAL.printf(__VA_ARGS__)

namespace debuglog {

class Hex {
public:
    Hex(const std::uint8_t* data, std::size_t length, std::size_t originalLength = 0) {
        const std::size_t count = length < 64 ? length : 64;
        std::size_t used = 0;
        for (std::size_t i = 0; i < count; ++i)
            used += std::snprintf(buffer_ + used, sizeof(buffer_) - used, "%s%02x", i ? " " : "", data[i]);
        const std::size_t total = originalLength > length ? originalLength : length;
        if (total > count)
            std::snprintf(buffer_ + used, sizeof(buffer_) - used, " …(+%zu)", total - count);
    }
    const char* c_str() const { return buffer_; }
private:
    char buffer_[224]{};
};

class Value {
public:
    explicit Value(const std::optional<float>& value) {
        if (value) std::snprintf(buffer_, sizeof(buffer_), "%.2f", static_cast<double>(*value));
        else std::snprintf(buffer_, sizeof(buffer_), "-");
    }
    const char* c_str() const { return buffer_; }
private:
    char buffer_[48]{};
};

}  // namespace debuglog
#else
#define SF_DEBUG_LOG(...) do {} while (0)
#endif
