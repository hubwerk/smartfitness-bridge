#pragma once

// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{sportstechbike,sportstechelliptical,sportstechrower,
// sportsplusbike,sportsplusrower,pafersbike,pafersrower,paferstreadmill}/
// (same-named .cpp/.h), src/devices/rower.cpp (pace->watts formula),
// src/devices/bluetooth.cpp (advertisement rules) and src/qzsettings.h
// (setting defaults). This port is part of smartfitness-bridge and
// distributed under the same license.
//
// Three small vendor protocol families sharing one paced write engine:
//  - Sportstech EW-BK/AF bike, EW-EP elliptical, EW-ST rower (FFF0, f2 c? frames)
//  - SportsPlus / Carefitness bikes and rowers (FFF0, 12-byte frames)
//  - Pafers bike, rower and treadmill (72d70001 service, 55 xx frames)
// The existing "sportstech-f15" treadmill (protocols/sportstech.hpp) is an
// unrelated protocol.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocols/registry.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace sportsplus {

using Bytes = ble::Bytes;

enum class Model : std::uint8_t {
    SportstechBike,
    SportstechElliptical,
    SportstechRower,
    SportsplusBike,     // CARDIOFIT / HT (10 chars, non-numeric) default layout
    SportsplusHt,       // HT + 8 digits (BCD payload)
    CarefitnessBike,    // CARE..., 11 characters (Carefitness CV-351)
    SportsplusHt9600ie, // upstream sp_ht_9600ie setting (manual only)
    SportsplusRower,    // CARE... >= 12 characters or VMAX...
    PafersBike,
    PafersRower,        // upstream pafers_rower setting (manual only)
    PafersTreadmill,    // upstream pafers_treadmill setting (manual only)
    PafersIboxster      // upstream pafers_treadmill_bh_iboxster_plus (manual only)
};

extern const ble::Uuid kFff0;
extern const ble::Uuid kFff1;
extern const ble::Uuid kFff2;
extern const ble::Uuid kFff3;
extern const ble::Uuid kFfe2;
extern const ble::Uuid kPafersService;  // 72d70001-501f-46f7-95f9-23846ee1aba3
extern const ble::Uuid kPafersWrite;    // 72d70002-...
extern const ble::Uuid kPafersNotify;   // 72d70003-...

constexpr int kMatchPriority = 80;
constexpr int kSportstechMaxLevel = 23;  // poll frame clamp
constexpr int kSportsplusMaxLevel = 24;
constexpr int kPafersMinLevel = 1;
constexpr int kPafersMaxLevel = 24;
constexpr float kPafersMaxSpeed = 22.0F;
constexpr float kPafersMinSpeed = 1.0F;
constexpr float kPafersMaxIncline = 15.0F;

bool matchesSportstechBike(const ble::Advertisement&);
bool matchesSportstechElliptical(const ble::Advertisement&);
bool matchesSportstechRower(const ble::Advertisement&);
bool matchesSportsplusBike(const ble::Advertisement&);
bool matchesSportsplusHt(const ble::Advertisement&);
bool matchesCarefitnessBike(const ble::Advertisement&);
bool matchesSportsplusRower(const ble::Advertisement&);
bool matchesPafersBike(const ble::Advertisement&);

std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

// --- frame builders (exact upstream bytes) ---
std::vector<Bytes> sportstechInitFrames();
// f2 c3 07 04 <level> 00 00 00 ff ff <0xbe + level>, level clamped to 0..23.
Bytes sportstechPollFrame(int level);
// 40 00 16 0a 60 once (default / HT layout) or 40 00 9a 56 30 six times
// (Carefitness, sp_ht_9600ie, SportsPlus rower); XM Fitness rower: 40 00 d1 5f 70.
std::vector<Bytes> sportsplusInitFrames(Model model, bool xmFitness = false);
// 20 01 <level> 00 <0x21 + level>, level clamped to 0..24.
Bytes sportsplusPollFrame(int level);
std::vector<Bytes> pafersInitFrames(Model model);
Bytes pafersPollFrame();                         // 55 0d 0a ff*10
Bytes pafersResistanceFrame(std::uint8_t level); // 55 11 01 <level>
Bytes pafersSpeedFrame(std::uint8_t kmh);        // 55 0f 02 <km/h> 00
Bytes pafersInclineFrame(std::uint8_t percent);  // 55 11 01 <percent>
Bytes pafersStopFrame();                         // 55 0a 01 02

// --- decoders: fill only the fields the frame carries ---
// 20-byte Sportstech frame. Power is upstream's packet value (0 when cadence is 0).
bool decodeSportstech(const std::uint8_t* data, std::size_t length, MetricSet& out);
// 12-byte SportsPlus bike frame; `level` is the requested resistance used by
// upstream's Carefitness watt table estimate.
bool decodeSportsplusBike(Model model, const std::uint8_t* data, std::size_t length, int level, MetricSet& out);
bool decodeSportsplusRower(const std::uint8_t* data, std::size_t length, MetricSet& out);
// 10-byte Pafers bike/rower frame.
bool decodePafers(const std::uint8_t* data, std::size_t length, MetricSet& out);
// 13-byte Pafers treadmill frame (speed, incline).
bool decodePafersTreadmill(const std::uint8_t* data, std::size_t length, MetricSet& out);

// Upstream estimates.
float carefitnessWatts(int level, float cadence);
float rowerWattsFromSpeed(float kmh);

}  // namespace sportsplus

// One source for every model above. Writes are paced like upstream: one frame
// at a time, released by the next notification or a 300/400 ms timeout.
// Treadmill (Pafers) speed/incline are sent only to a belt that is already
// running with fresh data; the bridge never sends the start command.
class SportsplusSource final : public SourceProtocol {
public:
    explicit SportsplusSource(sportsplus::Model model);

    const char* id() const override;
    MachineProfile profile() const override;
    ble::ConnectPlan plan() const override;
    void onWriteEndpoint(const ble::Uuid& service, const ble::Uuid& characteristic) override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

    sportsplus::Model model() const { return model_; }
    bool initialized() const { return ready_; }
    int requestedLevel() const { return level_; }

private:
    struct Step {
        ble::Bytes bytes;
        bool waitResponse;
    };
    bool sportstech() const;
    bool sportsplusFamily() const;
    bool pafers() const;
    bool treadmill() const;
    void push(const ble::Bytes& bytes, bool waitResponse = true);
    std::uint32_t pollIntervalMs() const;
    std::uint32_t responseTimeoutMs() const;

    sportsplus::Model model_;
    ble::Uuid writeService_, write_;
    bool xmFitness_ = false;
    std::vector<Step> queue_;
    bool connected_ = false, ready_ = false, awaiting_ = false, awaitResponse_ = false;
    std::uint32_t sentMs_ = 0, pollMs_ = 0;
    int level_ = 0;
};

}  // namespace protocols
}  // namespace smartfitness
