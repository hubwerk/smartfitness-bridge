#pragma once

// ELECROW CrowPanel 1.28" ESP32-S3 Rotary Display (240x240 round IPS, knob).
// ESP32-S3R8: 512 KB SRAM, 8 MB octal PSRAM, 16 MB flash.
//
// All pins are taken from official ELECROW sources and were cross-checked
// against each other (not verified on hardware):
//   [F] Factory firmware, factory_soucecode/RotaryScreen_1_28/RotaryScreen_1_28.ino
//   [E] Arduino examples, example/Arduino/Examples/README.md and Lesson03-Rotate.ino
//   [S] Schematic, Eagle_SCH&PCB/ESP32 Display-1.28-V1.0.sch (net names below)
// Repository: https://github.com/Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen
// Revision:   commit 63e407f67ceb4fafaa25cf48da5bf0c068c9e532 (2026-10-08), board "ESP32 Display-1.28-V1.0".

#include <cstdint>

namespace board {

constexpr int kWidth = 240;
constexpr int kHeight = 240;

// GC9A01 display, SPI (3-wire, write only) [F][E][S]
constexpr int kLcdSclk = 10;  // [S] LCD_SCK
constexpr int kLcdMosi = 11;  // [S] LCD_SDA
constexpr int kLcdMiso = -1;
constexpr int kLcdDc = 3;     // [S] LCD_DC
constexpr int kLcdCs = 9;     // [S] LCD_CS
constexpr int kLcdRst = 14;   // [S] LCD_RST
constexpr std::uint32_t kLcdWriteHz = 80000000;  // [F] freq_write
constexpr bool kLcdInvert = true;                // [F] cfg.invert

// Backlight PWM [F][S] (net GPIO46_BL)
constexpr int kBacklight = 46;
constexpr std::uint32_t kBacklightPwmHz = 5000;  // [F]
constexpr int kBacklightPwmBits = 8;
constexpr int kBacklightPwmChannel = 0;

// Power rails [F][S]
// GPIO1 HIGH switches the LCD 3.3 V rail (LCD VDD/LEDA, touch pull-ups) via
// Q11/Q10. Required for the display.
constexpr int kLcdPowerEnable = 1;
// GPIO2 HIGH switches OUT_5V on the expansion connectors via Q7/Q4; the
// factory firmware sets it HIGH at boot.
// TODO(verify): the schematic only shows the external 5 V output on this
// switch; whether the LED ring level shifter depends on it was not verifiable.
constexpr int kExt5vEnable = 2;
// Green power LED cathode, LOW = on [F][S]
constexpr int kPowerLed = 40;

// CST816D capacitive touch, I2C (Wire1 in [F]) [F][S]
constexpr int kTouchSda = 6;  // [S] TP_SDA
constexpr int kTouchScl = 7;  // [S] TP_SCL
constexpr int kTouchInt = 5;  // [S] TP_INT
constexpr int kTouchRst = 13; // [S] TP_RST
constexpr std::uint8_t kTouchAddress = 0x15;
constexpr std::uint32_t kTouchI2cHz = 400000;

// Rotary encoder with push button [F][E][S]
// A/B have external pull-ups (R20/R19); the button (R21) is active LOW.
constexpr int kEncoderA = 45;
constexpr int kEncoderB = 42;
constexpr int kEncoderButton = 41;
constexpr int kEncoderStepsPerDetent = 4;  // quarter steps per click

// WS2812 RGB LEDs around the knob (RGB1..RGB5), via level shifter [F][S]
constexpr int kLedRing = 48;
// TODO(verify): 5 LEDs per [F] (NUM_LEDS 5) and [S] (RGB1..RGB5); colour
// order GRB per [F] (NEO_GRB + NEO_KHZ800).
constexpr int kLedCount = 5;

// On-board DHT20 sensor I2C (unused by the bridge) [F]
constexpr int kSensorSda = 38;
constexpr int kSensorScl = 39;

}  // namespace board
