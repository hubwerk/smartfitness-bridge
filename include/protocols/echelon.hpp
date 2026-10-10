#pragma once

// Echelon Connect Sport bike protocol.
//
// Ported from qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift,
// src/devices/echelonconnectsport/echelonconnectsport.cpp), Copyright (C)
// Roberto Viola and contributors, licensed under GPL-3.0. This port is part
// of smartfitness-bridge and distributed under the same license.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace echelon {

using Bytes = ble::Bytes;

extern const ble::Uuid kService;         // 0bf669f1-45f2-11e7-9598-0800200c9a66
extern const ble::Uuid kWrite;           // ...f2
extern const ble::Uuid kNotify1;         // ...f3
extern const ble::Uuid kNotify2;         // ...f4
constexpr int kMinResistance = 1;
constexpr int kMaxResistance = 32;
constexpr std::uint32_t kPollIntervalMs = 2000;

// Frames sent once after subscribing (btinit()).
std::vector<Bytes> initFrames();
// f0 a0 01 <counter> <sum>; the counter runs 1..255 and wraps to 1.
Bytes pollFrame(std::uint8_t counter);
// f0 b1 01 <level> <sum>
Bytes resistanceFrame(std::uint8_t level);

struct Frame {
    enum class Kind : std::uint8_t { Unknown, Resistance, Data } kind = Kind::Unknown;
    int resistance = 0;
    float cadence = 0.0F;
};
bool decode(const std::uint8_t* data, std::size_t length, Frame& out);

// Upstream default watt table, interpolated over cadence.
float power(int resistance, float cadence);
inline float speedFromCadence(float cadence) { return 0.37497622F * cadence; }

}  // namespace echelon

class EchelonSource final : public SourceProtocol {
public:
    const char* id() const override { return "echelon"; }
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
};

}  // namespace protocols
}  // namespace smartfitness
