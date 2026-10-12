#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "lvgl.h"

extern "C" {
LV_FONT_DECLARE(sf_antonio_16);
LV_FONT_DECLARE(sf_antonio_20);
LV_FONT_DECLARE(sf_antonio_48);
}

namespace hmi::lcars {

constexpr std::uint32_t Black = 0x000000;
constexpr std::uint32_t Orange = 0xFF9900;
constexpr std::uint32_t Golden = 0xFFAA00;
constexpr std::uint32_t Peach = 0xFFCC99;
constexpr std::uint32_t Tan = 0xCC9966;
constexpr std::uint32_t Lavender = 0xCC99CC;
constexpr std::uint32_t Violet = 0x9999FF;
constexpr std::uint32_t LightBlue = 0x99CCFF;
constexpr std::uint32_t Blue = 0x5566FF;
constexpr std::uint32_t Gray = 0x888888;
constexpr std::uint32_t Alert = 0xCC4444;
constexpr std::size_t GaugeSegments = 24;
// Knob focus cursor drawn around the focused widget (outline width + gap).
// Containers of focusable widgets keep at least this much padding so the
// cursor is never clipped.
constexpr int FocusRingWidth = 2;
constexpr int FocusRingGap = 1;
constexpr int FocusRing = FocusRingWidth + FocusRingGap;
using Gauge = std::array<lv_obj_t*, GaugeSegments>;

void init(lv_display_t* display);
std::string uppercase(std::string text);
lv_obj_t* frame(lv_obj_t* screen, const std::string& title, bool alert);
void scrollbar(lv_obj_t* container);
void pill(lv_obj_t* object, std::uint32_t color = Lavender);
Gauge gauge(lv_obj_t* parent);
void setGauge(const Gauge& segments, float ratio, bool alert = false);

}  // namespace hmi::lcars
