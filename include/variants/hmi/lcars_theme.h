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

// Legacy token names keep the screen code stable; values follow Momentum.
constexpr std::uint32_t Black = 0x111512;
constexpr std::uint32_t Orange = 0xD2F65A;
constexpr std::uint32_t Golden = 0xE1FF87;
constexpr std::uint32_t Peach = 0xE1FF87;
constexpr std::uint32_t Tan = 0xA8B3AA;
constexpr std::uint32_t Lavender = 0xBAC8BD;
constexpr std::uint32_t Violet = 0xBAC8BD;
constexpr std::uint32_t LightBlue = 0xF3F4ED;
constexpr std::uint32_t Blue = 0xD2F65A;
constexpr std::uint32_t Gray = 0xA8B3AA;
constexpr std::uint32_t Alert = 0xFF9990;
constexpr std::size_t GaugeSegments = 24;
// Knob focus cursor drawn around the focused widget (outline width + gap).
// Containers of focusable widgets keep at least this much padding so the
// cursor is never clipped.
constexpr int FocusRingWidth = 2;
constexpr int FocusRingGap = 1;
constexpr int FocusRing = FocusRingWidth + FocusRingGap;
using Gauge = std::array<lv_obj_t*, GaugeSegments>;
// Four-step signal-strength pictogram (rising bars, see hmi::signalLevel).
constexpr int SignalBarCount = 4;
constexpr int SignalBarWidth = 4;
constexpr int SignalBarGap = 2;
constexpr int SignalBarsWidth = SignalBarCount * SignalBarWidth + (SignalBarCount - 1) * SignalBarGap;
constexpr int SignalBarsHeight = 16;

void init(lv_display_t* display);
std::string uppercase(std::string text);
lv_obj_t* frame(lv_obj_t* screen, const std::string& title, bool alert);
void scrollbar(lv_obj_t* container);
void pill(lv_obj_t* object, std::uint32_t color = Lavender);
Gauge gauge(lv_obj_t* parent);
void setGauge(const Gauge& segments, float ratio, bool alert = false);
// Signal pictogram with `level` (0..SignalBarCount) lit bars in `color`.
lv_obj_t* signalBars(lv_obj_t* parent, int level, std::uint32_t color);

}  // namespace hmi::lcars
