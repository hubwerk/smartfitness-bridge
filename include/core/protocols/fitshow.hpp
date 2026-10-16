#pragma once

// FitShow-family BLE protocols: FitShow treadmills, FitPlus bikes/rowers
// (Virtufit Etappe, Merach MRK, H9110 Osaka, Sportstech SX600) and the
// S-Node FTMS bike quirk entry.
//
// GPL-3.0 port of qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift),
// Copyright (C) Roberto Viola and contributors. Pinned revision
// ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/fitshowtreadmill/fitshowtreadmill.{cpp,h},
// src/devices/fitplusbike/fitplusbike.{cpp,h},
// src/devices/fitplusrower/fitplusrower.{cpp,h},
// src/devices/snodebike/snodebike.{cpp,h},
// src/devices/bluetooth.cpp (advertisement rules), src/qzsettings.h (defaults).
// This port is part of smartfitness-bridge and distributed under GPL-3.0.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocols/registry.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace fitshow {

using Bytes = ble::Bytes;

constexpr int kMatchPriority = 80;
constexpr std::uint32_t kTickMs = 200;  // upstream pollDeviceTime / refresh timer

// 02 <payload> <xor(payload)> 03
Bytes frame(const Bytes& payload);
bool validFrame(const std::uint8_t* data, std::size_t length);

// ---- FitShow treadmill --------------------------------------------------
enum class TreadmillVariant : std::uint8_t {
    Standard,  // km/h, little-endian status counters
    Miles,     // QZ setting fitshow_treadmill_miles: belt reports/accepts mph
    AnyRun,    // QZ setting fitshow_anyrun: big-endian counters, short init
    NoblePro   // NOBLEPRO CONNECT: incline in 0.5 % units, 0.5 % steps
};

// Wall-clock fields of the 50 00 model request. The firmware has no clock,
// so the source sends the default (2000-01-01 00:00:00).
struct ClockTime {
    std::uint8_t yearSince2000 = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

constexpr std::uint8_t kSysInfo = 0x50, kSysStatus = 0x51, kSysData = 0x52, kSysControl = 0x53;
constexpr std::uint16_t kDefaultUserId = 0x13AA;  // QZSettings::default_fitshow_user_id
constexpr std::uint8_t kDefaultWeightKg = 75;     // QZSettings::default_weight

// Unframed payloads queued by upstream btinit(startTape = false).
std::vector<Bytes> treadmillInitPayloads(bool anyrun, const ClockTime& time = ClockTime());
Bytes statusPayload();                                            // 51
Bytes sportDataPayload();                                         // 52 00
Bytes targetPayload(std::uint8_t speedTenths, std::uint8_t incline);  // 53 02 <speed> <incline>

struct Status {
    std::uint8_t state = 0;        // FITSHOW_STATUS_*
    bool hasMetrics = false;       // long running/stop/paused/end frame
    std::uint8_t speedTenths = 0;  // raw belt unit (km/h or mph) x10
    std::int8_t incline = 0;       // raw device unit
    std::uint16_t elapsed = 0;     // s
    std::uint16_t steps = 0;
    std::uint8_t heartRate = 0;
};
constexpr std::uint8_t kStatusNormal = 0, kStatusEnd = 1, kStatusStart = 2, kStatusRunning = 3, kStatusStop = 4,
                       kStatusError = 5, kStatusSafety = 6, kStatusStudy = 7, kStatusPaused = 10;
// data/length is a validated full frame.
bool decodeStatus(const std::uint8_t* data, std::size_t length, bool anyrun, Status& out);

bool matchesTreadmill(const ble::Advertisement&);
bool matchesNoblePro(const ble::Advertisement&);

// ---- FitPlus bikes / rower -----------------------------------------------
enum class FitPlusModel : std::uint8_t {
    Generic,  // FS-... with QZ setting fitplus_bike (14-byte frames, read-only)
    Etappe,   // Virtufit Etappe 2.0i (X100-) or setting virtufit_etappe
    Merach,   // MRK-... bikes
    H9110,    // H9110 OSAKA
    SX600,    // Sportstech SX600 (QZ setting sportstech_sx600)
    Rower     // fitplusrower: MRK-R28-
};

std::vector<Bytes> fitplusInitFrames(FitPlusModel model);
Bytes fitplusPollFrame(FitPlusModel model, std::uint8_t counter);
std::uint8_t fitplusPollCycle(FitPlusModel model);
// Upstream lookup tables; empty for unsupported levels/models.
Bytes fitplusResistanceFrame(FitPlusModel model, int level);
int fitplusDefaultMaxResistance(FitPlusModel model);

struct FitPlusData {
    bool longLayout = false;  // 15-byte: resistance/cadence/power; 14-byte: speed/cadence
    int resistance = 0;
    float cadence = 0.0F;
    float power = 0.0F;  // W, before upstream's cadence==0 -> 0 W rule
    float speed = 0.0F;  // km/h (14-byte layout only)
};
bool decodeFitPlus(const std::uint8_t* data, std::size_t length, bool longLayout, FitPlusData& out);

bool matchesEtappe(const ble::Advertisement&);
bool matchesMerach(const ble::Advertisement&);
bool matchesH9110(const ble::Advertisement&);
bool matchesFitPlusRower(const ble::Advertisement&);
bool matchesSnode(const ble::Advertisement&);

std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

}  // namespace fitshow

class FitshowTreadmillSource final : public SourceProtocol {
public:
    explicit FitshowTreadmillSource(fitshow::TreadmillVariant variant = fitshow::TreadmillVariant::Standard);
    const char* id() const override;
    MachineProfile profile() const override { return MachineProfile::Treadmill; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

    // Limits reported by the treadmill (raw x10 speed, raw incline).
    int maxSpeedTenths() const { return maxSpeed_; }
    int minSpeedTenths() const { return minSpeed_; }
    int maxIncline() const { return maxIncline_; }
    int minIncline() const { return minIncline_; }
    bool running(std::uint32_t nowMs) const;
    std::size_t queued() const { return queue_.size(); }

    static constexpr std::uint32_t kStatusFreshMs = 2000;
    static constexpr std::size_t kMaxQueue = 12;

private:
    void schedule(const fitshow::Bytes& payload);
    void dequeue();
    void send(const fitshow::Bytes& payload);
    void sportData();
    float unitFactor() const;
    void handleFrame(const std::uint8_t* data, std::size_t length, std::uint32_t nowMs);

    fitshow::TreadmillVariant variant_;
    ble::Uuid service_, write_;
    std::size_t active_ = 0;  // subscription index of the selected vendor service
    std::vector<fitshow::Bytes> queue_;
    unsigned retry_ = 0;
    bool ready_ = false;
    std::uint32_t lastTickMs_ = 0;
    std::uint32_t statusMs_ = 0;
    std::uint8_t state_ = 0xff;
    std::uint8_t speedTenths_ = 0;
    float rawIncline_ = 0.0F;
    int maxSpeed_ = 300, minSpeed_ = 0, maxIncline_ = 30, minIncline_ = 0;
};

class FitPlusSource final : public SourceProtocol {
public:
    explicit FitPlusSource(fitshow::FitPlusModel model);
    const char* id() const override;
    MachineProfile profile() const override;
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;
    int maxResistance() const { return maxResistance_; }

private:
    bool controllable() const;
    void send(const fitshow::Bytes& frame);

    fitshow::FitPlusModel model_;
    bool ready_ = false;
    bool ftmsData_ = false;
    bool validShortFrame_ = false;
    std::uint8_t counter_ = 0;
    std::uint32_t lastTickMs_ = 0;
    int maxResistance_ = 24;
};

std::unique_ptr<SourceProtocol> createSnodeBike();

}  // namespace protocols
}  // namespace smartfitness
