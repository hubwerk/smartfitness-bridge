#pragma once

// Echelon rower, Stride treadmill and stair climber BLE protocols.
//
// GPL-3.0 port of qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift),
// Copyright (C) Roberto Viola and contributors. Pinned revision
// ea70d7ff64bc8bf508fc6401fd056211e05311b3:
//   src/devices/echelonrower/echelonrower.{cpp,h}
//   src/devices/echelonstride/echelonstride.{cpp,h}
//   src/devices/echelonstairclimber/echelonstairclimber.{cpp,h}
//   src/devices/bluetooth.cpp (advertisement name rules)
//   src/qzsettings.h (default_sole_treadmill_miles = true)
// This port is part of smartfitness-bridge and distributed under the same
// license. All three devices share the Echelon GATT layout and the
// f0 <cmd> <len> <payload> <sum> framing of protocols/echelon.hpp.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocols/echelon.hpp"
#include "protocols/registry.hpp"

namespace smartfitness {
namespace protocols {
namespace echelon_ext {

using Bytes = ble::Bytes;

constexpr std::uint32_t kPollIntervalMs = 2000;
constexpr int kRowerMinResistance = 1;
constexpr int kRowerMaxResistance = 32;
constexpr std::size_t kRowerFrameLength = 21;
constexpr float kStrideMaxIncline = 15.0F;
// Upstream multiplies the reported Stride speed by 1.60934 with its default
// sole_treadmill_miles = true, i.e. the console reports mph.
constexpr float kMilesToKm = 1.60934F;

// f0 <bytes...> <8-bit sum>
Bytes withChecksum(Bytes frame);

// Rower: same init/poll/resistance frames as the Connect Sport bike.
std::vector<Bytes> rowerInitFrames();
// Watt table interpolated over cadence in 5 spm steps (upstream wattsFromResistance()).
float rowerPower(int resistance, float cadence);

struct RowerFrame {
    enum class Kind : std::uint8_t { Unknown, Resistance, Data } kind = Kind::Unknown;
    int resistance = 0;
    float cadence = 0.0F;      // strokes/min, byte 11
    std::uint16_t pace = 0;    // s/500 m, bytes 13..14 big endian
};
// Decodes a complete (already reassembled) frame.
bool decodeRower(const std::uint8_t* data, std::size_t length, RowerFrame& out);

// Stride: init sequence, with the extra handshake used by STRIDE4 consoles.
std::vector<Bytes> strideInitFrames(bool stride4);
// f0 b1 01 <incline %> <sum>
inline Bytes strideInclineFrame(std::uint8_t incline) { return echelon::resistanceFrame(incline); }

struct StrideFrame {
    enum class Kind : std::uint8_t { Unknown, Speed, Incline, Ack, Data } kind = Kind::Unknown;
    bool reply = false;        // upstream echoes d0/d2/d3 frames back to the console
    float speed = 0.0F;        // device units (mph with upstream defaults)
    float incline = 0.0F;      // %
    bool hasHeartRate = false;
    float heartRate = 0.0F;
};
bool decodeStride(const std::uint8_t* data, std::size_t length, bool stride4, StrideFrame& out);

// Stair climber
std::vector<Bytes> stairInitFrames();

struct StairFrame {
    std::uint16_t steps = 0;
    std::uint16_t elevation = 0;
    float cadence = 0.0F;  // steps/min
};
bool decodeStair(const std::uint8_t* data, std::size_t length, StairFrame& out);
inline float stairSpeedFromCadence(float cadence) { return cadence / 3.2F; }

std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

}  // namespace echelon_ext

class EchelonRowerSource final : public SourceProtocol {
public:
    const char* id() const override { return "echelon-rower"; }
    MachineProfile profile() const override { return MachineProfile::Rower; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

private:
    bool ready_ = false;
    std::uint8_t counter_ = 1;
    std::uint32_t lastPollMs_ = 0;
    int resistance_ = 0;
    ble::Bytes packet_;
};

// Speed control is not exposed: upstream reads the speed as mph by default but
// writes the speed target in km/h, so the commanded unit is ambiguous. The
// bridge only changes the incline (0..15 %) and never starts or stops the belt.
class EchelonStrideSource final : public SourceProtocol {
public:
    explicit EchelonStrideSource(bool stride4) : stride4_(stride4) {}
    const char* id() const override { return stride4_ ? "echelon-stride4" : "echelon-stride"; }
    MachineProfile profile() const override { return MachineProfile::Treadmill; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

private:
    bool stride4_;
    bool ready_ = false;
    std::uint8_t counter_ = 1;
    std::uint32_t lastPollMs_ = 0;
};

// Read-only: upstream's control code for this class is commented out.
class EchelonStairSource final : public SourceProtocol {
public:
    const char* id() const override { return "echelon-stair"; }
    MachineProfile profile() const override { return MachineProfile::StairClimber; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

private:
    bool ready_ = false;
    std::uint8_t counter_ = 1;
    std::uint32_t lastPollMs_ = 0;
};

}  // namespace protocols
}  // namespace smartfitness
