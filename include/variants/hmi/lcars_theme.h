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

// Historical names are retained for the existing diagnostic widgets.
constexpr std::uint32_t Black = 0x07111D;  // also baked into sf_tex_sky
constexpr std::uint32_t Orange = 0x142E43;
constexpr std::uint32_t Golden = 0x16364A;
constexpr std::uint32_t Peach = 0x204560;
constexpr std::uint32_t Tan = 0x112638;
constexpr std::uint32_t Lavender = 0x0D1E2E;
constexpr std::uint32_t Violet = 0x102638;
constexpr std::uint32_t LightBlue = 0xF1F6FC;
constexpr std::uint32_t Blue = 0x53B9FF;
constexpr std::uint32_t Gray = 0x16232D;
constexpr std::uint32_t Alert = 0x78313B;
constexpr std::uint32_t Muted = 0x9BB5CA;
constexpr std::uint32_t Border = 0x2B465D;
// Top of the screen background gradient (fades into Black).
constexpr std::uint32_t Sky = 0x10283C;  // baked into sf_tex_sky (scripts/gen_hmi_icons.py)
// Purely decorative layers (gradients, glow); never focusable or interactive.
// Layout checks in the simulator skip them.
constexpr lv_obj_flag_t Decor = LV_OBJ_FLAG_USER_1;
// Progress arc plus two soft glow arcs behind it.
constexpr std::size_t GaugeSegments = 3;
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
// Vertical gradient fill derived from `color` (header bands, pills).
void band(lv_obj_t* object, std::uint32_t color);
// Screen background: black with a navy sky fading in from the top.
void background(lv_obj_t* screen);
// Soft accent glow rising from the bottom of the round screen.
lv_obj_t* accentGlow(lv_obj_t* parent, std::uint32_t color);
// Recoloured A8 icon.
lv_obj_t* icon(lv_obj_t* parent, const lv_image_dsc_t* source, std::uint32_t color);
Gauge gauge(lv_obj_t* parent);
void setGauge(const Gauge& segments, float ratio, bool alert = false, std::uint32_t color = Blue);
// Signal pictogram with `level` (0..SignalBarCount) lit bars in `color`.
lv_obj_t* signalBars(lv_obj_t* parent, int level, std::uint32_t color);

}  // namespace hmi::lcars
