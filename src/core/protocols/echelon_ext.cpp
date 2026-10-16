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
// license.

#include "protocols/echelon_ext.hpp"

#include <cctype>
#include <cmath>
#include <memory>
#include <string>

#include "ftms/ftms_codec.hpp"

namespace smartfitness {
namespace protocols {
namespace echelon_ext {

Bytes withChecksum(Bytes frame) {
    std::uint8_t sum = 0;
    for (auto byte : frame) sum = static_cast<std::uint8_t>(sum + byte);
    frame.push_back(sum);
    return frame;
}

namespace {

// Upstream echelonrower::wattsFromResistance() table: 33 levels x 0..55 spm
// in 5 spm steps; the first two columns are upstream's Epsilon, i.e. 0 W.
constexpr std::uint16_t kRowerWattTable[33][12] = {
    {0, 0, 300, 620, 880, 1080, 1190, 1280, 1410, 1530, 1620, 1740},
    {0, 0, 300, 620, 880, 1080, 1190, 1280, 1410, 1530, 1620, 1740},
    {0, 0, 310, 640, 900, 1130, 1230, 1330, 1440, 1550, 1640, 1760},
    {0, 0, 315, 650, 930, 1170, 1290, 1370, 1480, 1580, 1670, 1800},
    {0, 0, 320, 660, 960, 1220, 1340, 1430, 1530, 1610, 1690, 1820},
    {0, 0, 325, 670, 980, 1270, 1380, 1510, 1610, 1680, 1700, 1840},
    {0, 0, 330, 680, 1030, 1330, 1450, 1570, 1670, 1720, 1740, 1880},
    {0, 0, 335, 690, 1050, 1360, 1510, 1600, 1740, 1790, 1800, 1930},
    {0, 0, 340, 700, 1070, 1400, 1590, 1640, 1770, 1840, 1860, 1980},
    {0, 0, 345, 710, 1100, 1440, 1640, 1680, 1820, 1880, 1900, 2050},
    {0, 0, 350, 720, 1130, 1480, 1690, 1780, 1940, 2020, 2090, 2200},
    {0, 0, 355, 735, 1170, 1550, 1740, 1850, 1990, 2090, 2170, 2270},
    {0, 0, 360, 750, 1200, 1580, 1790, 1880, 2050, 2170, 2250, 2340},
    {0, 0, 370, 765, 1220, 1620, 1860, 2020, 2160, 2250, 2310, 2430},
    {0, 0, 380, 770, 1240, 1650, 1890, 2170, 2240, 2320, 2400, 2540},
    {0, 0, 385, 780, 1280, 1680, 2010, 2210, 2310, 2430, 2490, 2620},
    {0, 0, 395, 790, 1320, 1750, 2080, 2280, 2390, 2510, 2610, 2720},
    {0, 0, 405, 805, 1380, 1800, 2190, 2340, 2460, 2590, 2710, 2810},
    {0, 0, 410, 820, 1450, 1870, 2290, 2460, 2520, 2640, 2780, 2910},
    {0, 0, 415, 850, 1480, 2020, 2390, 2530, 2610, 2700, 2840, 2990},
    {0, 0, 420, 870, 1530, 2050, 2490, 2590, 2660, 2780, 2910, 3100},
    {0, 0, 470, 920, 1600, 2080, 2550, 2660, 2720, 2830, 2960, 3150},
    {0, 0, 480, 950, 1680, 2150, 2690, 2760, 2820, 2980, 3070, 3250},
    {0, 0, 510, 1000, 1730, 2180, 2790, 2830, 2900, 3050, 3200, 3410},
    {0, 0, 560, 1020, 1770, 2280, 2890, 2940, 3010, 3170, 3300, 3510},
    {0, 0, 590, 1080, 1840, 2360, 2980, 3030, 3120, 3230, 3370, 3670},
    {0, 0, 610, 1150, 1880, 2380, 3090, 3170, 3250, 3390, 3530, 3800},
    {0, 0, 630, 1250, 1930, 2480, 3190, 3290, 3380, 3470, 3640, 3890},
    {0, 0, 700, 1320, 1960, 2560, 3240, 3400, 3510, 3750, 3890, 4080},
    {0, 0, 750, 1360, 2080, 2580, 3290, 3500, 3600, 3820, 4020, 4210},
    {0, 0, 820, 1420, 2130, 2620, 3350, 3570, 3670, 3960, 4140, 4310},
    {0, 0, 860, 1520, 2160, 2660, 3390, 3620, 3720, 4150, 4300, 4450},
    {0, 0, 900, 1580, 2230, 2680, 3440, 3680, 3990, 4300, 4440, 4600}};

const ble::Uuid& svc() { return echelon::kService; }
const ble::Uuid& wr() { return echelon::kWrite; }

ble::ConnectPlan echelonPlan() {
    ble::ConnectPlan plan;
    plan.subscribe(echelon::kService, echelon::kNotify1);
    plan.subscribe(echelon::kService, echelon::kNotify2);
    return plan;
}

std::uint16_t be16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(data[0]) << 8) | data[1]);
}

void nextCounter(std::uint8_t& counter) {
    counter = static_cast<std::uint8_t>(counter + 1);
    if (counter == 0) counter = 1;
}

}  // namespace

std::vector<Bytes> rowerInitFrames() { return echelon::initFrames(); }

float rowerPower(int resistance, float cadence) {
    if (!(cadence > 0.0F)) return 0.0F;
    int level = resistance < 0 ? 0 : resistance;
    if (level > 32) level = 32;
    const std::uint16_t* row = kRowerWattTable[level];
    const auto watts = [row](int column) { return static_cast<double>(row[column]) / 10.0; };
    const int step = static_cast<int>(cadence / 5.0F);
    double result;
    if (step >= 11) {
        result = static_cast<double>(cadence) / 55.0 * watts(11);
    } else {
        const double base = watts(step);
        result = (watts(step + 1) - base) / 5.0 * static_cast<double>(static_cast<int>(cadence) % 5) + base;
    }
    // Upstream returns uint16_t.
    return static_cast<float>(static_cast<std::uint16_t>(result));
}

bool decodeRower(const std::uint8_t* data, std::size_t length, RowerFrame& out) {
    out = {};
    if (!data) return false;
    if (length == 5 && data[0] == 0xf0 && data[1] == 0xd2) {
        out.kind = RowerFrame::Kind::Resistance;
        out.resistance = static_cast<std::int8_t>(data[3]);
        return true;
    }
    if (length != kRowerFrameLength) return false;
    out.kind = RowerFrame::Kind::Data;
    out.cadence = static_cast<float>(data[11]);
    out.pace = be16(data + 13);
    return true;
}

std::vector<Bytes> strideInitFrames(bool stride4) {
    const Bytes init0{0xf0, 0xa4, 0x00, 0x94};
    const Bytes init1{0xf0, 0xa1, 0x00, 0x91};
    const Bytes init2{0xf0, 0xa3, 0x00, 0x93};
    if (stride4) {
        const Bytes stride4Init{0xf0, 0xe0, 0xfd, 0x3e, 0x65, 0x48, 0xd5, 0x8d};
        return {init0, init0, stride4Init, init0, init1, init1, init1, init1, init2};
    }
    return {init0, init1, init1, init1, init1, init2, init1};
}

bool decodeStride(const std::uint8_t* data, std::size_t length, bool stride4, StrideFrame& out) {
    out = {};
    if (!data || length < 2) return false;
    if (data[0] == 0xf0) {
        switch (data[1]) {
            case 0xd3:
                if (length < 5) return false;
                out.kind = StrideFrame::Kind::Speed;
                out.reply = true;
                out.speed = static_cast<float>(be16(data + 3)) / 1000.0F;
                return true;
            case 0xd2:
                if (length < 4) return false;
                out.kind = StrideFrame::Kind::Incline;
                out.reply = true;
                out.incline = static_cast<float>(data[3]);
                return true;
            case 0xd0:
                out.kind = StrideFrame::Kind::Ack;
                out.reply = true;
                return true;
            case 0xd1:
                if (!stride4) break;
                if (length < 9) return false;
                out.kind = StrideFrame::Kind::Speed;
                out.speed = static_cast<float>(be16(data + 7)) / 100.0F;
                return true;
            default:
                break;
        }
    }
    // Upstream only checks byte 1 here; STRIDE4 d1 frames returned above.
    if (data[1] == 0xd1 && length > 11) {
        out.kind = StrideFrame::Kind::Data;
        out.hasHeartRate = data[11] != 0;
        out.heartRate = static_cast<float>(data[11]);
        return true;
    }
    return false;
}

std::vector<Bytes> stairInitFrames() {
    const Bytes init0{0xf0, 0xa4, 0x00, 0x94};
    const Bytes init1{0xf0, 0xe0, 0x85, 0xf8, 0x03, 0x68, 0xd2, 0x8a};
    const Bytes init2{0xf0, 0xe0, 0x38, 0x04, 0x03, 0xb5, 0x14, 0xd8};
    const Bytes init3{0xf0, 0xa1, 0x00, 0x91};
    const Bytes init4{0xf0, 0xa3, 0x00, 0x93};
    const Bytes init5{0xf0, 0xb0, 0x01, 0x01, 0xa2};
    const Bytes init6{0xf0, 0xa5, 0x00, 0x95};
    return {init0, init1, init0, init2, init3, init3, init3, init3, init4, init5, init6};
}

bool decodeStair(const std::uint8_t* data, std::size_t length, StairFrame& out) {
    out = {};
    if (!data || length < 10 || data[0] != 0xf0 || data[1] != 0xd1) return false;
    out.steps = be16(data + 5);
    out.elevation = be16(data + 7);
    out.cadence = static_cast<float>(data[9]);
    return true;
}

}  // namespace echelon_ext

// ---------------------------------------------------------------- rower

ble::ConnectPlan EchelonRowerSource::plan() const { return echelon_ext::echelonPlan(); }

void EchelonRowerSource::reset() {
    SourceProtocol::reset();
    ready_ = false;
    counter_ = 1;
    lastPollMs_ = 0;
    resistance_ = 0;
    packet_.clear();
}

bool EchelonRowerSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if ((subscribedMask & 0x3U) == 0) {
        setError("Echelon rower notify characteristics unavailable");
        return false;
    }
    for (const auto& frame : echelon_ext::rowerInitFrames()) queueWrite(echelon_ext::svc(), echelon_ext::wr(), frame);
    ready_ = true;
    lastPollMs_ = nowMs;
    return true;
}

void EchelonRowerSource::tick(std::uint32_t nowMs) {
    if (!ready_ || nowMs - lastPollMs_ < echelon_ext::kPollIntervalMs) return;
    lastPollMs_ = nowMs;
    queueWrite(echelon_ext::svc(), echelon_ext::wr(), echelon::pollFrame(counter_));
    echelon_ext::nextCounter(counter_);
}

void EchelonRowerSource::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (!data || length == 0) return;
    // Upstream joins a split 21-byte data frame (both notify characteristics).
    if (!packet_.empty() && packet_.size() + length == echelon_ext::kRowerFrameLength && packet_[0] == 0xf0)
        packet_.insert(packet_.end(), data, data + length);
    else
        packet_.assign(data, data + length);

    echelon_ext::RowerFrame frame;
    if (!echelon_ext::decodeRower(packet_.data(), packet_.size(), frame)) return;
    if (frame.kind == echelon_ext::RowerFrame::Kind::Resistance) {
        resistance_ = frame.resistance;
        sample_.resistance = {static_cast<float>(resistance_), true};
        return;
    }
    const bool moving = frame.cadence > 0.0F && frame.pace > 0;
    sample_.cadence = {frame.cadence, true};
    sample_.pace = {moving ? static_cast<float>(frame.pace) : 0.0F, true};
    sample_.speed = {moving ? 1800.0F / static_cast<float>(frame.pace) : 0.0F, true};
    sample_.power = {echelon_ext::rowerPower(resistance_, frame.cadence), true};
    touch(nowMs);
}

bool EchelonRowerSource::control(const ControlRequest& request, std::uint32_t) {
    if (!ready_ || request.control != Control::Resistance) return false;
    const int level =
        percentToLevel(request.value, echelon_ext::kRowerMinResistance, echelon_ext::kRowerMaxResistance);
    return queueWrite(echelon_ext::svc(), echelon_ext::wr(), echelon::resistanceFrame(static_cast<std::uint8_t>(level)));
}

TrainerCapabilities EchelonRowerSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    capabilities.setControl(Control::Resistance, ready_);
    return capabilities;
}

// ---------------------------------------------------------------- stride

ble::ConnectPlan EchelonStrideSource::plan() const { return echelon_ext::echelonPlan(); }

void EchelonStrideSource::reset() {
    SourceProtocol::reset();
    ready_ = false;
    counter_ = 1;
    lastPollMs_ = 0;
}

bool EchelonStrideSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if ((subscribedMask & 0x3U) == 0) {
        setError("Echelon Stride notify characteristics unavailable");
        return false;
    }
    for (const auto& frame : echelon_ext::strideInitFrames(stride4_))
        queueWrite(echelon_ext::svc(), echelon_ext::wr(), frame);
    ready_ = true;
    lastPollMs_ = nowMs;
    return true;
}

void EchelonStrideSource::tick(std::uint32_t nowMs) {
    if (!ready_ || nowMs - lastPollMs_ < echelon_ext::kPollIntervalMs) return;
    lastPollMs_ = nowMs;
    queueWrite(echelon_ext::svc(), echelon_ext::wr(), echelon::pollFrame(counter_));
    echelon_ext::nextCounter(counter_);
}

void EchelonStrideSource::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    echelon_ext::StrideFrame frame;
    if (!echelon_ext::decodeStride(data, length, stride4_, frame)) return;
    if (frame.reply) queueWrite(echelon_ext::svc(), echelon_ext::wr(), ble::Bytes(data, data + length));
    switch (frame.kind) {
        case echelon_ext::StrideFrame::Kind::Speed:
            sample_.speed = {frame.speed * echelon_ext::kMilesToKm, true};
            break;
        case echelon_ext::StrideFrame::Kind::Incline:
            sample_.incline = {frame.incline, true};
            break;
        case echelon_ext::StrideFrame::Kind::Data:
            sample_.heartRate = frame.hasHeartRate ? MetricSample{frame.heartRate, true} : MetricSample{};
            break;
        default:
            return;
    }
    touch(nowMs);
}

bool EchelonStrideSource::control(const ControlRequest& request, std::uint32_t) {
    if (!capabilities().supports(request.control) || !std::isfinite(request.value)) return false;
    float incline = request.value < 0.0F ? 0.0F : request.value;  // upstream clamps negatives to 0
    if (incline > echelon_ext::kStrideMaxIncline) return false;
    // Upstream assigns the double to a uint8_t (truncation).
    return queueWrite(echelon_ext::svc(), echelon_ext::wr(),
                      echelon_ext::strideInclineFrame(static_cast<std::uint8_t>(incline)));
}

TrainerCapabilities EchelonStrideSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    capabilities.setControl(Control::Incline, ready_ && hasData() && !failed());
    return capabilities;
}

// ---------------------------------------------------------------- stair climber

ble::ConnectPlan EchelonStairSource::plan() const { return echelon_ext::echelonPlan(); }

void EchelonStairSource::reset() {
    SourceProtocol::reset();
    ready_ = false;
    counter_ = 1;
    lastPollMs_ = 0;
}

bool EchelonStairSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if ((subscribedMask & 0x3U) == 0) {
        setError("Echelon stair climber notify characteristics unavailable");
        return false;
    }
    for (const auto& frame : echelon_ext::stairInitFrames()) queueWrite(echelon_ext::svc(), echelon_ext::wr(), frame);
    ready_ = true;
    lastPollMs_ = nowMs;
    return true;
}

void EchelonStairSource::tick(std::uint32_t nowMs) {
    if (!ready_ || nowMs - lastPollMs_ < echelon_ext::kPollIntervalMs) return;
    lastPollMs_ = nowMs;
    queueWrite(echelon_ext::svc(), echelon_ext::wr(), echelon::pollFrame(counter_));
    echelon_ext::nextCounter(counter_);
}

void EchelonStairSource::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    echelon_ext::StairFrame frame;
    if (!echelon_ext::decodeStair(data, length, frame)) return;
    sample_.stepCount = {static_cast<float>(frame.steps), true};
    sample_.positiveElevationGain = {static_cast<float>(frame.elevation), true};
    sample_.cadence = {frame.cadence, true};
    sample_.speed = {echelon_ext::stairSpeedFromCadence(frame.cadence), true};
    touch(nowMs);
}

TrainerCapabilities EchelonStairSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    return capabilities;
}

// ---------------------------------------------------------------- registry descriptors

namespace echelon_ext {
namespace {

std::string upper(const std::string& value) {
    std::string out = value;
    for (char& character : out) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return out;
}

bool startsWith(const std::string& value, const char* prefix) { return value.rfind(prefix, 0) == 0; }

bool hasFtms(const ble::Advertisement& advertisement) { return advertisement.hasService(ftms::kServiceUuid); }

// Name rules follow bluetooth.cpp (all compare toUpper() names).
bool matchesRower(const ble::Advertisement& advertisement) {
    if (hasFtms(advertisement)) return false;
    const std::string name = upper(advertisement.name);
    return startsWith(name, "ECH-ROW") || startsWith(name, "ROWSPORT") || startsWith(name, "ROW-7S-") ||
           startsWith(name, "ROW-S");
}

bool matchesStride4(const ble::Advertisement& advertisement) {
    return !hasFtms(advertisement) && startsWith(upper(advertisement.name), "STRIDE4");
}

bool matchesStride(const ble::Advertisement& advertisement) {
    if (hasFtms(advertisement)) return false;
    const std::string name = upper(advertisement.name);
    // ECH-EC-SPT is checked before the stride rule upstream (stair climber).
    if (startsWith(name, "STRIDE4") || startsWith(name, "ECH-EC-SPT")) return false;
    return startsWith(name, "ECH-STRIDE") || startsWith(name, "ECH-UK-") || startsWith(name, "ECH-FR-") ||
           startsWith(name, "STRIDE") || startsWith(name, "ECH-SD-SPT");
}

bool matchesStair(const ble::Advertisement& advertisement) {
    return !hasFtms(advertisement) && startsWith(upper(advertisement.name), "ECH-EC-SPT");
}

std::unique_ptr<SourceProtocol> makeRower() { return std::unique_ptr<SourceProtocol>(new EchelonRowerSource()); }
std::unique_ptr<SourceProtocol> makeStride() { return std::unique_ptr<SourceProtocol>(new EchelonStrideSource(false)); }
std::unique_ptr<SourceProtocol> makeStride4() { return std::unique_ptr<SourceProtocol>(new EchelonStrideSource(true)); }
std::unique_ptr<SourceProtocol> makeStair() { return std::unique_ptr<SourceProtocol>(new EchelonStairSource()); }

// Priority 85/86: wins over the generic "echelon" bike matcher (80, any "ECH" name).
const ProtocolInfo kDescriptors[] = {
    {"echelon-rower", "Echelon Row", 85, matchesRower, makeRower, true, false, MachineProfile::Rower},
    {"echelon-stride", "Echelon Stride", 85, matchesStride, makeStride, true, false, MachineProfile::Treadmill},
    {"echelon-stride4", "Echelon Stride 4", 86, matchesStride4, makeStride4, true, false, MachineProfile::Treadmill},
    {"echelon-stair", "Echelon stair climber", 85, matchesStair, makeStair, false, false,
     MachineProfile::StairClimber},
};

}  // namespace

std::size_t descriptorCount() { return sizeof(kDescriptors) / sizeof(kDescriptors[0]); }
const ProtocolInfo& descriptorAt(std::size_t index) {
    return kDescriptors[index < descriptorCount() ? index : descriptorCount() - 1];
}

}  // namespace echelon_ext

}  // namespace protocols
}  // namespace smartfitness
