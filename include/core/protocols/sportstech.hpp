#pragma once

// Sportstech / FitHome treadmill BLE protocol (F15, F31, WP100, WP200).
//
// Ported from syssi/esphome-treadmill-f15
// (https://github.com/syssi/esphome-treadmill-f15), Copyright syssi and
// contributors, licensed under the Apache License 2.0.
// Pinned revision 61172d6448e00d0a4eb5d70c3c39a3427c292041:
// components/treadmill_f15/treadmill_f15.{cpp,h} (frame layout, CRC, status
// decoding, poll and speed/incline command), components/treadmill_f15/button/
// __init__.py (stop command), components/treadmill_f15/number/ (speed/incline
// limits) and docs/protocol.md (captured frames). This port is part of
// smartfitness-bridge and distributed under GPL-3.0-or-later.
// Also cross-checked against deniskie/ha-sportstech-walkingpad-integration,
// MIT-licensed, revision f2f102710f3a7ca8c0671ea32316cb18b35274fd
// (FitHome/F37 status states, metrics, and limit queries).

#include <cstddef>
#include <cstdint>

#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace sportstech {

using Bytes = ble::Bytes;

extern const ble::Uuid kService;  // 0xFFF0
extern const ble::Uuid kNotify;   // 0xFFF1
extern const ble::Uuid kWrite;    // 0xFFF2 (write without response)

// Upstream number entity defaults (speed 0.8–12 km/h, incline 0–15 %).
constexpr float kMinSpeed = 0.8F;
constexpr float kMaxSpeed = 12.0F;
constexpr float kMaxIncline = 15.0F;
constexpr float kHardMinSpeed = 0.1F;
constexpr float kHardMaxSpeed = 25.5F;
constexpr float kHardMaxIncline = 25.0F;
constexpr std::uint32_t kPollIntervalMs = 1000;
constexpr std::size_t kMaxFrameLength = 108;

enum class Mode : std::uint8_t {
    Stopped = 0,
    Finish = 1,
    Starting = 2,
    Running = 3,
    Stopping = 4,
    Error = 5,
    Safety = 6,
    Sleep = 7,
    Paused = 10
};

// XOR over every byte between the 0x02 start and the checksum.
std::uint8_t checksum(const std::uint8_t* data, std::size_t length);
// 02 <register> <payload...> <xor> 03
Bytes frame(std::uint8_t reg, const Bytes& payload);
// Status poll 02 51 51 03 (upstream sends it without a checksum).
Bytes pollFrame();
// Request the device speed/incline limits (02 50 02/03 <xor> 03).
Bytes limitsFrame(std::uint8_t subcommand);
// 02 53 03 <xor> 03
Bytes stopFrame();
// 02 53 02 <speed 0.1 km/h> <incline %> 00*7 <xor> 03
Bytes speedInclineFrame(std::uint8_t speedTenths, std::uint8_t inclinePercent);

struct Status {
    Mode mode = Mode::Stopped;
    bool full = false;           // 17-byte frame with measurements
    float speed = 0.0F;          // km/h
    float incline = 0.0F;        // %
    std::uint16_t elapsed = 0;   // s
    std::uint16_t distance = 0;  // m
    float calories = 0.0F;       // kcal
    std::uint16_t steps = 0;
    std::uint16_t heartRate = 0; // bpm, 0 = no reading
};
// False for malformed frames, checksum errors and non-status frames.
bool decodeStatus(const std::uint8_t* data, std::size_t length, Status& out);

}  // namespace sportstech

// Manual-only input (no advertisement rule is documented upstream). Speed and
// incline are sent together; controls are accepted only while the belt is
// running and a speed of 0 sends the stop command. The bridge never starts the
// belt.
class SportstechF15Source final : public SourceProtocol {
public:
    const char* id() const override { return "sportstech-f15"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

    sportstech::Mode mode() const { return mode_; }

private:
    bool fresh(std::uint32_t nowMs) const;

    bool connected_ = false;
    sportstech::Mode mode_ = sportstech::Mode::Stopped;
    std::uint32_t pollMs_ = 0;
    // Requested targets until the status reports them (-1 = none): speed in
    // 0.1 km/h, incline in integer percent as sent on the wire.
    int speedTarget_ = -1;
    int inclineTarget_ = -1;
    float minSpeed_ = sportstech::kMinSpeed;
    float maxSpeed_ = sportstech::kMaxSpeed;
    float minIncline_ = 0.0F;
    float maxIncline_ = sportstech::kMaxIncline;
    std::uint32_t elapsedOffset_ = 0;
    std::uint16_t lastElapsed_ = 0;
    bool haveElapsed_ = false;
};

}  // namespace protocols
}  // namespace smartfitness
