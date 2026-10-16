#pragma once

// Domyos / Decathlon treadmill, elliptical and rower (Chang Yow consoles).
//
// GPL-3.0 port of qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift),
// Copyright (C) Roberto Viola and contributors. This port is part of
// smartfitness-bridge and distributed under the same license.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/domyostreadmill/domyostreadmill.{cpp,h},
// src/devices/domyoselliptical/domyoselliptical.{cpp,h},
// src/devices/domyosrower/domyosrower.{cpp,h},
// src/devices/bluetooth.cpp (advertisement rules), src/qzsettings.h (defaults).
// The GATT UUIDs and the resistance frame are shared with the Domyos bike
// port (protocols/domyos.hpp).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocols/domyos.hpp"
#include "protocols/registry.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace domyos_ext {

using Bytes = ble::Bytes;

enum class Kind : std::uint8_t { Treadmill, Elliptical, Rower };

constexpr int kMatchPriority = 80;
// Upstream write queue: a write flagged wait_for_response is complete when a
// notification whose length is not 20 arrives, or after 300 ms.
constexpr std::uint32_t kWriteTimeoutMs = 300;
constexpr std::uint32_t kTreadmillPollMs = 200;  // bluetooth.cpp pollDeviceTime default
constexpr std::uint32_t kPollMs = 300;           // elliptical / rower refresh timer
constexpr std::uint32_t kFreshMs = 2000;
constexpr int kMinResistance = 1;
constexpr int kMaxResistance = 15;
constexpr float kMaxSpeed = 22.0F;        // domyostreadmill update(): requestSpeed <= 22
constexpr float kMaxIncline = 20.0F;      // domyostreadmill update(): requestInclination <= 20
constexpr float kMaxEllipticalIncline = 15.0F;

struct Step {
    Bytes bytes;
    bool wait;  // upstream wait_for_response
};

// btinit(false) / btinit_changyow(false): the connect-time sequence without
// the "start tape" frames. Never contains a start or stop command.
std::vector<Step> initSteps(Kind kind);
// Sum of all bytes (upstream "sort of a checksum").
std::uint8_t checksum(const std::uint8_t* data, std::size_t length);
// domyostreadmill::forceSpeedOrIncline(): 23-byte frame sent as 20 + 3 bytes.
std::vector<Bytes> speedInclineFrames(float speedKmh, float inclinePercent);
// domyoselliptical::forceInclination(): f0 e3 <level+1> <sum>.
Bytes inclinationFrame(std::int8_t level);
// Upstream clamps for the elliptical inclination request (>15 -> 15, 0 -> 1).
std::int8_t ellipticalInclinationLevel(float percent);

struct TreadmillStatus {
    float speed = 0.0F;    // km/h
    float incline = 0.0F;  // %
    int heartRate = 0;     // 0 = no reading
};
struct EllipticalStatus {
    float speed = 0.0F;
    float cadence = 0.0F;
    int resistance = 1;
    float incline = 0.0F;
    int heartRate = 0;
    float calories = 0.0F;
};
struct RowerStatus {
    float speed = 0.0F;  // km/h, derived from the reported 500 m split
    float pace = 0.0F;   // s/500 m, 0 = not moving
    float cadence = 0.0F;
    int resistance = 1;
    int heartRate = 0;
    float calories = 0.0F;
    float strokes = 0.0F;
};
// All decoders accept only the 26-byte status frame.
bool decodeTreadmill(const Bytes& frame, TreadmillStatus& out);
bool decodeElliptical(const Bytes& frame, EllipticalStatus& out);
bool decodeRower(const Bytes& frame, RowerStatus& out);

bool matchesTreadmill(const ble::Advertisement& advertisement);
bool matchesElliptical(const ble::Advertisement& advertisement);
bool matchesRower(const ble::Advertisement& advertisement);

std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

}  // namespace domyos_ext

// Treadmill safety: the connect-time init never starts the belt, console
// start/stop buttons are ignored, and speed/incline frames are only sent to a
// belt that is reported running with fresh data. Nothing is sent on
// disconnect or stale data.
class DomyosExtSource final : public SourceProtocol {
public:
    explicit DomyosExtSource(domyos_ext::Kind kind) : kind_(kind) {}

    const char* id() const override;
    MachineProfile profile() const override;
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

    domyos_ext::Kind kind() const { return kind_; }
    bool initialized() const { return ready(); }
    bool waiting() const { return waiting_; }
    std::size_t queuedSteps() const { return pending_.size(); }

private:
    static constexpr std::size_t kMaxPending = 24;

    bool ready() const { return connected_ && initDone_; }
    bool enqueue(const std::vector<domyos_ext::Step>& steps, std::uint32_t nowMs);
    void flush(std::uint32_t nowMs);
    bool treadmillControl(const ControlRequest& request, std::uint32_t nowMs);

    domyos_ext::Kind kind_;
    std::vector<domyos_ext::Step> pending_;
    domyos::FrameAssembler assembler_;
    std::uint32_t sentMs_ = 0;
    std::uint32_t pollMs_ = 0;
    float speedTarget_ = -1.0F;
    float inclineTarget_ = -1.0F;
    bool connected_ = false;
    bool initQueued_ = false;
    bool initDone_ = false;  // every init step has been written
    bool waiting_ = false;
};

}  // namespace protocols
}  // namespace smartfitness
