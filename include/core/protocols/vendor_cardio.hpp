#pragma once

// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{proformrower,bhfitnesselliptical,ypooelliptical,soleelliptical,
// octaneelliptical,smartrowrower,sunnyfitstepper,concept2skierg}/ (same-named
// .cpp/.h) and src/devices/bluetooth.cpp (advertisement rules). This port is
// part of smartfitness-bridge and distributed under the same license.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "protocols/registry.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace vendor_cardio {

using Bytes = ble::Bytes;

// Persisted ids are bound to these values: append only.
enum class Kind : std::uint8_t {
    ProformRower, Proform750r, ProformSportRl,
    BhElliptical,
    YpooU3, YpooFtms, YpooSch411, YpooFeier, YpooSoleE25, YpooTrue, YpooIconsole, YpooGymstick,
    SoleElliptical, XterraXe88, SoleE55,
    OctaneQ37, SmartRow, SunnyFitStepper, Concept2SkiErg
};

constexpr int kMatchPriority = 80;

// ---- proformrower --------------------------------------------------------
// Frames of the three upstream resistance tables (levels 1..24); empty when
// the level has no table entry.
Bytes proformResistanceFrame(Kind kind, int level);
Bytes proformUnlockFrame();
std::vector<Bytes> proformInitFrames(Kind kind);
// Six-slot poll cycle (counterPoll 0..5).
Bytes proformPollFrame(Kind kind, unsigned slot);
// Upstream GetResistanceFromPacket() byte-11 lookup.
int proformResistanceFromByte(std::uint8_t value);

// ---- bhfitnesselliptical -------------------------------------------------
// FTMS indoor bike simulation 11 00 00 <grade lo hi> 21 22, grade = (level - 6) * 33.
Bytes bhResistanceFrame(int level);

// ---- ypooelliptical ------------------------------------------------------
std::vector<Bytes> ypooInitFrames(bool gymstick);
Bytes ypooPollFrame(unsigned counter);  // alternates 02 42 42 03 / 02 43 01 42 03
Bytes ypooResistanceFrame(int level);
Bytes gymstickResistanceFrame(int level, int current);
Bytes ftmsResistanceFrame(int level);       // 04 <level*10 & 0xff>
Bytes ftmsInclinationFrame(float percent);  // 03 <(uint16)percent*10 LE>
// Cross Trainer Data (0x2ACE) as interpreted by upstream for each variant.
// Fields parsed before a truncation are kept (upstream assigns progressively).
struct CrossData {
    MetricSet metrics;
    bool hasStride = false;
    std::uint16_t stride = 0;
};
bool decodeYpooCross(Kind kind, const std::uint8_t* data, std::size_t length, CrossData& out);
// iConsole 15-byte frame: speed, cadence.
bool decodeIconsole(const std::uint8_t* data, std::size_t length, MetricSet& out);
// Indoor Bike Data (0x2AD2) as read by ypooelliptical / bhfitnesselliptical.
bool decodeBikeData(bool bhSpeedFromCadence, const std::uint8_t* data, std::size_t length, MetricSet& out);

// ---- soleelliptical ------------------------------------------------------
std::vector<Bytes> soleInitFrames(bool e55);
Bytes solePollFrame(bool e55, unsigned counter);
std::vector<Bytes> soleStepFrames(bool up);
struct SoleFrame {
    enum class Type : std::uint8_t { Unknown, Resistance, Incline, Data } type = Type::Unknown;
    int level = 0;
    MetricSet metrics;
};
bool decodeSole(bool xe88, const std::uint8_t* data, std::size_t length, SoleFrame& out);

// ---- octaneelliptical ----------------------------------------------------
std::vector<Bytes> octaneInitFrames();
bool decodeOctane(const std::uint8_t* data, std::size_t length, MetricSet& out);

// ---- smartrowrower -------------------------------------------------------
Bytes smartRowInitBytes();
// Upstream calculateSmartRowV3ChallengeResponse(): 0d <4 hex chars> 0d or 23.
Bytes smartRowChallengeResponse(const std::string& keylock);
// Upstream decodeSmartRowPacket() + field parsing of a 17-byte ASCII packet.
bool decodeSmartRow(bool v3, const std::uint8_t* data, std::size_t length, MetricSet& out);

// ---- sunnyfitstepper -----------------------------------------------------
std::vector<Bytes> sunnyFitInitFrames();
bool decodeSunnyFit(const std::uint8_t* frame, std::size_t length, MetricSet& out);

// ---- concept2skierg ------------------------------------------------------
// Multiplexed 0x0080 page; active tracks upstream isActive (rowing state).
bool decodeSkiErg(const std::uint8_t* data, std::size_t length, bool& active, MetricSet& out);

bool matchesProformRower(const ble::Advertisement&);
bool matchesBhElliptical(const ble::Advertisement&);
bool matchesYpooU3(const ble::Advertisement&);
bool matchesYpooFtms(const ble::Advertisement&);
bool matchesYpooSch411(const ble::Advertisement&);
bool matchesYpooFeier(const ble::Advertisement&);
bool matchesYpooSoleE25(const ble::Advertisement&);
bool matchesYpooTrue(const ble::Advertisement&);
bool matchesSoleElliptical(const ble::Advertisement&);
bool matchesXterraXe88(const ble::Advertisement&);
bool matchesOctane(const ble::Advertisement&);
bool matchesSmartRow(const ble::Advertisement&);
bool matchesSunnyFit(const ble::Advertisement&);
bool matchesSkiErg(const ble::Advertisement&);

std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

}  // namespace vendor_cardio

// One source class for the family; the Kind fixes model and variant so each
// persisted id maps to exactly one upstream behaviour.
class VendorCardioSource final : public SourceProtocol {
public:
    explicit VendorCardioSource(vendor_cardio::Kind kind);
    const char* id() const override;
    MachineProfile profile() const override;
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t mask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;
    vendor_cardio::Kind kind() const { return kind_; }
    std::size_t backlog() const { return backlog_.size(); }

private:
    struct Pending {
        ble::Uuid service;
        ble::Uuid characteristic;
        ble::Bytes payload;
        bool withResponse;
    };
    void push(const ble::Bytes& payload, bool withResponse = true);
    void pushTo(const ble::Uuid& service, const ble::Uuid& characteristic, const ble::Bytes& payload,
                bool withResponse = true);
    void flush(std::uint32_t nowMs);
    void merge(const MetricSet& values, std::uint32_t nowMs);
    void ypooNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs);
    void ypooStrideCadence(std::uint16_t stride, std::uint32_t nowMs);
    void soleStep(std::uint32_t nowMs);
    void proformPoll();
    void smartRowNotify(const std::uint8_t* data, std::size_t length, std::uint32_t nowMs);
    bool ypooFtmsGroup() const;
    bool ypooProprietary() const;

    vendor_cardio::Kind kind_;
    std::vector<Pending> backlog_;
    std::uint32_t spacingMs_ = 0;
    std::uint32_t lastSendMs_ = 0;
    bool sentOnce_ = false;
    bool ready_ = false;
    bool initQueued_ = false;
    std::uint32_t lastTickMs_ = 0;
    unsigned counter_ = 0;
    unsigned sec1_ = 0;
    // Resistance targets: -1 = none.
    int target_ = -1;
    int current_ = -1;
    bool awaitingStatus_ = false;
    std::uint32_t stepMs_ = 0;
    // Ypoo split frames / stride cadence / gymstick delta base.
    ble::Bytes partial_;
    bool strideSeen_ = false;
    std::uint16_t lastStride_ = 0;
    std::uint32_t lastStrideMs_ = 0;
    int requested_ = 6;
    // SmartRow V3 handshake state; SkiErg active flag; SunnyFit buffer.
    bool v3_ = false;
    bool challengeRequested_ = false;
    bool active_ = true;
    ble::Bytes frame_;
};

}  // namespace protocols
}  // namespace smartfitness
