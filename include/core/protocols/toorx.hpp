#pragma once

// Toorx / TRX App Gate family (i-Console+, i-Running, Reebok, Adidas, DKN,
// Tunturi, VirtuFit, FitHiWay, Changyow, ...): treadmills, bikes,
// ellipticals and rowers sharing the "f0 <op> <addr> <addr> ... <sum>" BLE
// protocol on service 0xFFF0 or the ISSC transparent UART service.
//
// GPL-3.0 port of qdomyos-zwift, Copyright (C) Roberto Viola and
// contributors. Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.{cpp,h},
// src/devices/trxappgateusbbike/trxappgateusbbike.{cpp,h},
// src/devices/trxappgateusbelliptical/trxappgateusbelliptical.{cpp,h},
// src/devices/trxappgateusbrower/trxappgateusbrower.{cpp,h},
// src/devices/bluetooth.cpp (advertisement rules), src/qzsettings.h
// (setting defaults). This port is part of smartfitness-bridge and
// distributed under the same license.

#include <cstddef>
#include <cstdint>

#include "protocols/registry.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace toorx {

using Bytes = ble::Bytes;

extern const ble::Uuid kFff0Service;  // 0000fff0-0000-1000-8000-00805f9b34fb
extern const ble::Uuid kFff2Write;    // 0xfff2
extern const ble::Uuid kFff1Notify;   // 0xfff1
extern const ble::Uuid kIsscService;  // 49535343-fe7d-4ae5-8fa9-9fafd205e455
extern const ble::Uuid kIsscWrite;    // 49535343-8841-43f4-a8d4-ecbe34729bb3
extern const ble::Uuid kIsscNotify;   // 49535343-1e4d-4bd9-ba61-23c647249616
extern const ble::Uuid kIsscNotify2;  // 49535343-4c8a-39b3-2f49-511cff073b7e (bikes)
extern const ble::Uuid kIsscNotify3;  // 49535343-aca3-481c-91ec-d85e28a60318 (DKN Run)

constexpr int kMatchPriority = 80;
constexpr float kMaxIncline = 15.0F;

enum class Kind : std::uint8_t { Treadmill, Bike, Elliptical, Rower };
enum class Service : std::uint8_t { None, Fff0, Issc };
enum class Parse : std::uint8_t {
    Treadmill,      // 19-byte reassembled frame: speed, incline
    TreadmillDkn,   // 18-byte reassembled frame (DKN Run)
    TreadmillFlow,  // 19-byte f0 b0/b2 2e d3 frame (Flow Fitness)
    Bike,           // 21 bytes: speed, cadence, resistance, power, kcal, heart
    BikeSrx500,     // >= 19 bytes, same fields as Bike
    BikeTunturi,    // 20/21 bytes: cadence, resistance, power, heart
    BikeFal,        // >= 19 bytes, same fields as BikeTunturi
    BikeRaw19,      // 19 bytes (JLL IC400, Asviva, Hammer): cadence, heart
    BikeFytter,     // 19 bytes: cadence, power, heart
    Elliptical,     // 21 bytes: speed, cadence (raw), resistance, power
    EllipticalToorx,
    EllipticalJtx,  // 21 bytes: cadence (byte 5), resistance
    Rower,          // 23 bytes: speed, cadence, resistance, power
    RowerMearch     // 23 bytes, Mearch NovaRow R50 layout
};

// One wire frame, at most 16 bytes (the longest upstream init frame).
struct Frame {
    std::uint8_t length;
    std::uint8_t bytes[16];
};

struct Variant {
    const char* id;
    const char* label;
    Kind kind;
    Parse parse;
    Service primary;
    Service fallback;           // used when the primary notify subscription fails
    std::uint8_t fallbackIndex; // variant used with the fallback service; kSame keeps this one
    std::uint8_t notify2;       // 0 none, 1 kIsscNotify2, 2 kIsscNotify3 (on the primary service)
    std::uint8_t noNotify2Index;// variant used when notify2 is missing; kSame keeps this one
    const Frame* frames;        // distinct init frames
    const std::uint8_t* order;  // init sequence as indexes into frames
    std::uint8_t steps;
    std::uint16_t initSleepMs;  // extra upstream QThread::msleep between init writes
    Frame poll;
    std::uint16_t pollMs;
    bool pollCounter;           // JLL IC400 rolling counter poll
    std::uint8_t controlA, controlB;  // address bytes 2..3 of control frames
    std::uint8_t fallbackControlA;    // 0 keeps controlA with the fallback service
    std::uint8_t minLevel, maxLevel;  // resistance range (0 when not controllable)
    bool withResponse;
};

constexpr std::uint8_t kSame = 0xff;

std::size_t variantCount();  // includes internal fallback-only variants
const Variant& variantAt(std::size_t index);

// Appends the 8-bit sum of all bytes (upstream "sort of a checksum").
Bytes withSum(Bytes frame);
Bytes frameBytes(const Frame& frame);
// f0 a6 <a> <b> <level+1> <sum>
Bytes resistanceFrame(std::uint8_t a, std::uint8_t b, int level);
// f0 ac <a> <b> <incline+1> 64 64 <sum> (integer incline, truncated as upstream)
Bytes inclineFrame(std::uint8_t a, std::uint8_t b, float incline);
// JLL IC400 poll: f0 a2 <counter> c8 <crc>; counter starts at 0x0b, both +0x0c per poll.
Bytes jllPollFrame(unsigned pollIndex);

// Decodes one complete telemetry frame (after treadmill reassembly).
bool decode(Parse parse, const std::uint8_t* data, std::size_t length, MetricSet& out);

std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

}  // namespace toorx

class ToorxSource final : public SourceProtocol {
public:
    explicit ToorxSource(std::size_t variantIndex);
    const char* id() const override;
    MachineProfile profile() const override;
    std::uint32_t startupTimeoutMs() const override { return 30000; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

    bool initialized() const { return ready_; }
    // Variant actually driving the connection (after service/characteristic fallbacks).
    const toorx::Variant& activeVariant() const;
    bool usingFallbackService() const { return fallbackActive_; }

private:
    bool send(const ble::Bytes& payload);
    void acceptTreadmill(const std::uint8_t* data, std::size_t length, std::uint32_t nowMs);
    void publish(const MetricSet& metrics, std::uint32_t nowMs);

    std::size_t base_;
    std::size_t active_;
    std::size_t fallbackSub_ = 0xff, notify2Sub_ = 0xff;
    bool connected_ = false, ready_ = false, fallbackActive_ = false;
    bool notify2Active_ = false;
    std::size_t step_ = 0;
    bool responded_ = false;
    std::uint32_t sentMs_ = 0, pollMs_ = 0;
    unsigned pollIndex_ = 0;
    ble::Bytes buffer_;
};

}  // namespace protocols
}  // namespace smartfitness
