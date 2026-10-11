#pragma once

#include <algorithm>
#include <cstdint>

namespace hmi {

struct ScrollArc {
    bool visible = false;
    int start = 155;
    int end = 205;
};

// LVGL angles run clockwise: the top of the left edge is the larger angle.
inline ScrollArc scrollArc(std::int32_t content, std::int32_t viewport, std::int32_t offset) {
    if (viewport <= 0 || content <= viewport) return {};
    constexpr int sweep = 50;
    constexpr int minimum = 8;
    const int length = std::max(minimum, static_cast<int>(static_cast<std::int64_t>(sweep) * viewport / content));
    const auto range = content - viewport;
    const int travel = static_cast<int>(static_cast<std::int64_t>(sweep - length) *
                                        std::clamp(offset, 0, range) / range);
    return {true, 205 - travel - length, 205 - travel};
}

}  // namespace hmi
