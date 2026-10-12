// CrowPanel 1.28" drivers for the HMI variant. Mirrors the official ELECROW
// factory firmware (see board_crowpanel128.h for sources), ported to Arduino
// core 2.0.x (ledcSetup/ledcAttachPin instead of core 3.x ledcAttach) and
// made non-blocking (single I2C transaction per touch sample, no retries).
#ifdef ARDUINO

#include "hmi_hal.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include <vector>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#if SF_HMI_LED_RING
#include <Adafruit_NeoPixel.h>
#endif

#include "board_crowpanel128.h"

namespace hmi {
namespace {

class Gc9a01Panel : public lgfx::LGFX_Device {
public:
    Gc9a01Panel() {
        auto bus = bus_.config();
        bus.spi_host = SPI2_HOST;
        bus.spi_mode = 0;
        bus.freq_write = board::kLcdWriteHz;
        bus.freq_read = 20000000;
        bus.spi_3wire = true;
        bus.use_lock = true;
        bus.dma_channel = SPI_DMA_CH_AUTO;
        bus.pin_sclk = board::kLcdSclk;
        bus.pin_mosi = board::kLcdMosi;
        bus.pin_miso = board::kLcdMiso;
        bus.pin_dc = board::kLcdDc;
        bus_.config(bus);
        panel_.setBus(&bus_);

        auto panel = panel_.config();
        panel.pin_cs = board::kLcdCs;
        panel.pin_rst = board::kLcdRst;
        panel.pin_busy = -1;
        panel.memory_width = board::kWidth;
        panel.memory_height = board::kHeight;
        panel.panel_width = board::kWidth;
        panel.panel_height = board::kHeight;
        panel.offset_x = 0;
        panel.offset_y = 0;
        panel.offset_rotation = 0;
        panel.readable = false;
        panel.invert = board::kLcdInvert;
        panel.rgb_order = false;
        panel.dlen_16bit = false;
        panel.bus_shared = false;
        panel_.config(panel);
        setPanel(&panel_);
    }

private:
    lgfx::Panel_GC9A01 panel_;
    lgfx::Bus_SPI bus_;
};

Gc9a01Panel gfx;
CrowPanelHal* g_hal = nullptr;

#if SF_HMI_LED_RING
Adafruit_NeoPixel ring(board::kLedCount, board::kLedRing, NEO_GRB + NEO_KHZ800);
#endif

// Partial render buffer: 40 lines, in internal
// DMA-capable RAM (PSRAM fallback).
constexpr std::size_t kBufferLines = 40;
constexpr std::size_t kBufferBytes = board::kWidth * kBufferLines * 2;

void* allocBuffer() {
    void* buffer = heap_caps_malloc(kBufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buffer) buffer = heap_caps_malloc(kBufferBytes, MALLOC_CAP_SPIRAM);
    return buffer;
}

// --- encoder (ISR only counts; LVGL reads on the main loop) ------------------

DRAM_ATTR const std::int8_t kTransitions[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
volatile std::uint8_t encoderState = 0;
volatile std::int8_t encoderQuarter = 0;
volatile std::int32_t encoderDetents = 0;
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR encoderIsr() {
    const std::uint8_t state =
        static_cast<std::uint8_t>((digitalRead(board::kEncoderA) << 1) | digitalRead(board::kEncoderB));
    portENTER_CRITICAL_ISR(&encoderMux);
    const std::int8_t movement = kTransitions[(encoderState << 2) | state];
    encoderState = state;
    encoderQuarter = static_cast<std::int8_t>(encoderQuarter + movement);
    if (encoderQuarter >= board::kEncoderStepsPerDetent) {
        ++encoderDetents;
        encoderQuarter = 0;
    } else if (encoderQuarter <= -board::kEncoderStepsPerDetent) {
        --encoderDetents;
        encoderQuarter = 0;
    }
    portEXIT_CRITICAL_ISR(&encoderMux);
}

std::int32_t takeDetents() {
    portENTER_CRITICAL(&encoderMux);
    const std::int32_t detents = encoderDetents;
    encoderDetents = 0;
    portEXIT_CRITICAL(&encoderMux);
    return detents;
}

// --- CST816D touch ------------------------------------------------------------

bool touchReady = false;

void touchBegin() {
    Wire1.begin(board::kTouchSda, board::kTouchScl, board::kTouchI2cHz);
    Wire1.setTimeOut(10);
    pinMode(board::kTouchInt, OUTPUT);
    digitalWrite(board::kTouchInt, HIGH);
    delay(1);
    digitalWrite(board::kTouchInt, LOW);
    delay(1);
    pinMode(board::kTouchInt, INPUT);
    pinMode(board::kTouchRst, OUTPUT);
    digitalWrite(board::kTouchRst, LOW);
    delay(10);
    digitalWrite(board::kTouchRst, HIGH);
    delay(300);
    Wire1.beginTransmission(board::kTouchAddress);
    Wire1.write(0xFE);  // disable auto sleep (factory firmware)
    Wire1.write(0xFF);
    touchReady = Wire1.endTransmission() == 0;
}

// Registers 0x01..0x06: gesture, finger count, X high/low, Y high/low.
bool touchRead(std::uint16_t& x, std::uint16_t& y) {
    if (!touchReady) return false;
    Wire1.beginTransmission(board::kTouchAddress);
    Wire1.write(0x01);
    if (Wire1.endTransmission(false) != 0) return false;
    std::uint8_t data[6];
    if (Wire1.requestFrom(static_cast<std::uint16_t>(board::kTouchAddress), static_cast<std::uint8_t>(6)) != 6)
        return false;
    for (auto& byte : data) byte = static_cast<std::uint8_t>(Wire1.read());
    if (data[1] == 0) return false;
    x = static_cast<std::uint16_t>(((data[2] & 0x0F) << 8) | data[3]);
    y = static_cast<std::uint16_t>(((data[4] & 0x0F) << 8) | data[5]);
    return x < board::kWidth && y < board::kHeight;
}

std::uint32_t tick() { return millis(); }

}  // namespace

bool CrowPanelHal::begin() {
    g_hal = this;
    // Power rails and power LED as in the factory firmware.
    pinMode(board::kPowerLed, OUTPUT);
    digitalWrite(board::kPowerLed, LOW);
    pinMode(board::kLcdPowerEnable, OUTPUT);
    digitalWrite(board::kLcdPowerEnable, HIGH);
    pinMode(board::kExt5vEnable, OUTPUT);
    digitalWrite(board::kExt5vEnable, HIGH);

    ledcSetup(board::kBacklightPwmChannel, board::kBacklightPwmHz, board::kBacklightPwmBits);
    ledcAttachPin(board::kBacklight, board::kBacklightPwmChannel);
    ledcWrite(board::kBacklightPwmChannel, 0);

    touchBegin();
    if (!touchReady) Serial.println("[HMI] touch controller not responding (knob still works)");

    gfx.init();
    gfx.initDMA();
    gfx.fillScreen(TFT_BLACK);

    pinMode(board::kEncoderA, INPUT);
    pinMode(board::kEncoderB, INPUT);
    pinMode(board::kEncoderButton, INPUT_PULLUP);
    encoderState = static_cast<std::uint8_t>((digitalRead(board::kEncoderA) << 1) | digitalRead(board::kEncoderB));
    attachInterrupt(digitalPinToInterrupt(board::kEncoderA), encoderIsr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(board::kEncoderB), encoderIsr, CHANGE);

#if SF_HMI_LED_RING
    ring.begin();
    ring.clear();
    ring.show();
#endif

    lv_tick_set_cb(tick);
    // One partial buffer: the flush waits for DMA, so a second buffer would not overlap.
    void* first = allocBuffer();
    if (!first) {
        Serial.println("[HMI] no memory for the LVGL draw buffer");
        return false;
    }
    display_ = lv_display_create(board::kWidth, board::kHeight);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display_, flush);
    lv_display_set_buffers(display_, first, nullptr, kBufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    encoder_ = lv_indev_create();
    lv_indev_set_type(encoder_, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(encoder_, readEncoder);
    lv_indev_set_display(encoder_, display_);

    touch_ = lv_indev_create();
    lv_indev_set_type(touch_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_, readTouch);
    lv_indev_set_display(touch_, display_);
    return true;
}

void CrowPanelHal::flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* pixels) {
    // Same transfer as the factory firmware: LovyanGFX swaps RGB565 for the
    // panel and sends it by DMA.
    gfx.pushImageDMA(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1,
                     reinterpret_cast<lgfx::rgb565_t*>(pixels));
    gfx.waitDMA();
    lv_display_flush_ready(display);
}

bool CrowPanelHal::swallowWhileAsleep(bool active) {
    if (waitRelease_) {
        if (!active) waitRelease_ = false;
        return true;
    }
    if (asleep_ && active) {
        wake();
        waitRelease_ = true;
        return true;
    }
    return false;
}

void CrowPanelHal::readEncoder(lv_indev_t*, lv_indev_data_t* data) {
    const std::int32_t detents = takeDetents();
    const bool pressed = digitalRead(board::kEncoderButton) == LOW;
    data->enc_diff = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    if (g_hal && g_hal->swallowWhileAsleep(pressed || detents != 0)) return;
    data->enc_diff = static_cast<std::int16_t>(detents);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void CrowPanelHal::readTouch(lv_indev_t*, lv_indev_data_t* data) {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    const bool touched = touchRead(x, y);
    data->state = LV_INDEV_STATE_RELEASED;
    if (g_hal && g_hal->swallowWhileAsleep(touched)) return;
    if (touched) {
        data->point.x = static_cast<lv_coord_t>(x);
        data->point.y = static_cast<lv_coord_t>(y);
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

void CrowPanelHal::setBrightness(std::uint8_t percent) {
    brightness_ = percent;
    if (!asleep_) ledcWrite(board::kBacklightPwmChannel, static_cast<std::uint32_t>(percent) * 255U / 100U);
}

void CrowPanelHal::wake() {
    asleep_ = false;
    lv_display_trigger_activity(display_);
    setBrightness(brightness_);
}

void CrowPanelHal::updateScreenTimeout(std::uint16_t timeoutS) {
    if (asleep_ || timeoutS == 0 || !display_) return;
    if (lv_display_get_inactive_time(display_) >= static_cast<std::uint32_t>(timeoutS) * 1000U) {
        asleep_ = true;
        ledcWrite(board::kBacklightPwmChannel, 0);
    }
}

void CrowPanelHal::setLedRing(bool enabled, const Rgb& color) {
#if SF_HMI_LED_RING
    const Rgb target = enabled ? color : Rgb{};
    if (ledValid_ && target == led_) return;
    led_ = target;
    ledValid_ = true;
    for (int index = 0; index < board::kLedCount; ++index) ring.setPixelColor(index, ring.Color(target.r, target.g, target.b));
    ring.show();
#else
    (void)enabled;
    (void)color;
#endif
}

// HMI-only preferences live in their own NVS namespace so the shared bridge
// configuration (schema v3) is unchanged.
HmiSettings CrowPanelHal::loadSettings() {
    HmiSettings settings;
    Preferences prefs;
    if (!prefs.begin("sfhmi", false)) return settings;
    if (!prefs.isKey("ui")) {
        prefs.end();
        return settings;
    }
    const std::size_t length = prefs.getBytesLength("ui");
    if (length > 0 && length <= 16) {
        std::vector<std::uint8_t> data(length);
        if (prefs.getBytes("ui", data.data(), length) == length) {
            deserialize(data, settings);  // keeps the defaults when invalid
        }
    }
    prefs.end();
    return settings;
}

void CrowPanelHal::saveSettings(const HmiSettings& settings) {
    Preferences prefs;
    if (!prefs.begin("sfhmi", false)) return;
    const auto data = serialize(settings);
    prefs.putBytes("ui", data.data(), data.size());
    prefs.end();
}

void CrowPanelHal::clearSettings() {
    Preferences prefs;
    if (!prefs.begin("sfhmi", false)) return;
    prefs.clear();
    prefs.end();
}

}  // namespace hmi

#endif  // ARDUINO
