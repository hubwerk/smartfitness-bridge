#pragma once

// CrowPanel 1.28" hardware for the HMI variant (firmware only): GC9A01
// display through LovyanGFX, CST816D touch, rotary encoder + button,
// backlight PWM, optional WS2812 LED ring and the HMI preferences in NVS.
// Pins: board_crowpanel128.h (official ELECROW sources).
#ifdef ARDUINO

#include <cstdint>

#include "hmi_model.h"
#include "lvgl.h"

namespace hmi {

class CrowPanelHal {
public:
    // Powers the panel, starts display/touch/encoder/backlight/LEDs and
    // registers the LVGL display and input devices. Call after lv_init().
    bool begin();
    lv_indev_t* encoder() const { return encoder_; }
    lv_indev_t* touch() const { return touch_; }

    void setBrightness(std::uint8_t percent);
    // Turns the backlight off after `timeoutS` seconds without input (0 =
    // never). The first input after that only wakes the screen.
    void updateScreenTimeout(std::uint16_t timeoutS);
    bool asleep() const { return asleep_; }
    void wake();

    // LED ring (SF_HMI_LED_RING): status colour or off.
    void setLedRing(bool enabled, const Rgb& color);

    static HmiSettings loadSettings();
    static void saveSettings(const HmiSettings& settings);
    static void clearSettings();

private:
    static void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* pixels);
    static void readEncoder(lv_indev_t* indev, lv_indev_data_t* data);
    static void readTouch(lv_indev_t* indev, lv_indev_data_t* data);
    bool swallowWhileAsleep(bool active);

    lv_display_t* display_ = nullptr;
    lv_indev_t* encoder_ = nullptr;
    lv_indev_t* touch_ = nullptr;
    std::uint8_t brightness_ = 80;
    bool asleep_ = false;
    bool waitRelease_ = false;
    bool ledValid_ = false;
    Rgb led_{};
};

}  // namespace hmi

#endif  // ARDUINO
