// FitShow-family BLE protocols (see protocols/fitshow.hpp).
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

#include "protocols/fitshow.hpp"

#include <cctype>
#include <cmath>
#include <string>

#include "ftms/ftms_codec.hpp"
#include "protocols/ftms_source.hpp"

namespace smartfitness {
namespace protocols {
namespace fitshow {
namespace {

bool starts(const std::string& value, const char* prefix) { return value.rfind(prefix, 0) == 0; }

std::string upper(const std::string& value) {
    std::string out = value;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool hasFtms(const ble::Advertisement& a) { return a.hasService(ftms::kServiceUuid); }

ble::Uuid uuid16(std::uint16_t value) { return ble::Uuid::from16(value); }

// Bytes 4..5 of the 02 44 05 <level> .. .. 03 resistance command, levels 1..32
// (fitplusrower resistanceCommands / fitplusbike Etappe-Merach-H9110 branch).
constexpr std::uint8_t kResistanceTable[32][2] = {
    {0xf9, 0xb9}, {0xf9, 0xba}, {0xfa, 0xb8}, {0xfb, 0xbe}, {0xfc, 0xb8}, {0xfd, 0xba}, {0xfe, 0xb8},
    {0xff, 0xb6}, {0x00, 0x48}, {0x00, 0x4b}, {0x01, 0x4b}, {0x03, 0x4e}, {0x03, 0x4f}, {0x05, 0x4a},
    {0x05, 0x4b}, {0x06, 0x57}, {0x07, 0x57}, {0x08, 0x5b}, {0x09, 0x5b}, {0x0a, 0x5f}, {0x0b, 0x5f},
    {0x0c, 0x5b}, {0x0d, 0x5b}, {0x0e, 0x57}, {0x00, 0x58}, {0x00, 0x5b}, {0x00, 0x5a}, {0x00, 0x5d},
    {0x00, 0x5c}, {0x00, 0x5f}, {0x00, 0x5e}, {0x00, 0x61}};

// Bytes 3..5 of the Sportstech SX600 table (fitplusbike sportstech_sx600 branch).
constexpr std::uint8_t kSx600Table[32][3] = {
    {0x00, 0x00, 0x41}, {0x02, 0x00, 0x43}, {0x03, 0xfa, 0xb8}, {0x04, 0xfb, 0xbe}, {0x05, 0x00, 0x44},
    {0x06, 0xfd, 0xba}, {0x07, 0xfe, 0xb8}, {0x08, 0x00, 0x49}, {0x09, 0x00, 0x48}, {0x0a, 0x00, 0x4b},
    {0x0b, 0x00, 0x4a}, {0x0c, 0x03, 0x4e}, {0x0d, 0x03, 0x4f}, {0x0e, 0x00, 0x4f}, {0x0f, 0x05, 0x4b},
    {0x10, 0x06, 0x57}, {0x11, 0x00, 0x50}, {0x12, 0x08, 0x5b}, {0x13, 0x00, 0x52}, {0x14, 0x00, 0x55},
    {0x15, 0x00, 0x54}, {0x16, 0x00, 0x57}, {0x17, 0x00, 0x56}, {0x18, 0x00, 0x59}, {0x19, 0x00, 0x58},
    {0x1a, 0x00, 0x5b}, {0x1b, 0x00, 0x5a}, {0x1c, 0x00, 0x5d}, {0x1d, 0x00, 0x5c}, {0x1e, 0x00, 0x5f},
    {0x1f, 0x00, 0x5e}, {0x20, 0x00, 0x61}};

const Bytes kPollData{0x02, 0x42, 0x42, 0x03};
const Bytes kPollData1{0x02, 0x43, 0x01, 0x42, 0x03};
const Bytes kSx600Poll1{0x02, 0x02, 0x43, 0x01, 0x42, 0x03, 0x01, 0x03};
const Bytes kSx600Status{0x02, 0x53, 0x9f, 0x02, 0x00, 0x00, 0xce, 0x03};

}  // namespace

Bytes frame(const Bytes& payload) {
    Bytes out;
    out.reserve(payload.size() + 3);
    out.push_back(0x02);
    std::uint8_t x = 0;
    for (auto b : payload) {
        out.push_back(b);
        x ^= b;
    }
    out.push_back(x);
    out.push_back(0x03);
    return out;
}

bool validFrame(const std::uint8_t* data, std::size_t length) {
    if (!data || length < 4 || data[0] != 0x02 || data[length - 1] != 0x03) return false;
    std::uint8_t x = 0;
    for (std::size_t i = 1; i < length - 2; ++i) x ^= data[i];
    return x == data[length - 2];
}

std::vector<Bytes> treadmillInitPayloads(bool anyrun, const ClockTime& t) {
    Bytes user{kSysControl, 0x00, static_cast<std::uint8_t>(kDefaultUserId & 0xff),
               static_cast<std::uint8_t>(kDefaultUserId >> 8), 110, 30, kDefaultWeightKg, 170};
    if (!anyrun) user.pop_back();  // upstream drops the height byte
    std::vector<Bytes> out{user};
    if (anyrun) {
        out.push_back({kSysInfo, 0x05});
        return out;
    }
    out.push_back({kSysInfo, 0x00, t.yearSince2000, t.month, t.day, t.hour, t.minute, t.second});
    for (std::uint8_t info : {std::uint8_t{2}, std::uint8_t{3}, std::uint8_t{4}, std::uint8_t{1}})
        out.push_back({kSysInfo, info});
    return out;
}

Bytes statusPayload() { return {kSysStatus}; }
Bytes sportDataPayload() { return {kSysData, 0x00}; }
Bytes targetPayload(std::uint8_t speedTenths, std::uint8_t incline) {
    return {kSysControl, 0x02, speedTenths, incline};
}

bool decodeStatus(const std::uint8_t* data, std::size_t length, bool anyrun, Status& out) {
    out = {};
    if (!validFrame(data, length) || length < 5 || data[1] != kSysStatus) return false;
    const std::uint8_t* a = data + 1;
    out.state = a[1];
    const bool metricsState = out.state == kStatusRunning || out.state == kStatusStop ||
                              out.state == kStatusPaused || out.state == kStatusEnd;
    if (!metricsState || length < 16) return true;
    out.hasMetrics = true;
    out.speedTenths = a[2];
    out.incline = static_cast<std::int8_t>(a[3]);
    out.elapsed = anyrun ? static_cast<std::uint16_t>(a[4] * 60 + a[5])
                         : static_cast<std::uint16_t>(a[4] | (a[5] << 8));
    out.steps = anyrun ? static_cast<std::uint16_t>(a[11] | (a[10] << 8))
                       : static_cast<std::uint16_t>(a[10] | (a[11] << 8));
    out.heartRate = a[12];
    return true;
}

std::vector<Bytes> fitplusInitFrames(FitPlusModel model) {
    switch (model) {
        case FitPlusModel::Merach:
        case FitPlusModel::Rower: return {{0xaa, 0x01, 0x00, 0x01, 0x55}};
        case FitPlusModel::H9110: {
            const Bytes init5{0x02, 0x44, 0x02, 0x46, 0x03};
            return {{0x02, 0xab, 0x3b, 0x30, 0x2a, 0x0d, 0x60, 0x01, 0x14, 0x11, 0xe3, 0x03},
                    kPollData1, kPollData, {0x02, 0x44, 0x01, 0x45, 0x03}, init5, init5,
                    {0x02, 0x41, 0x02, 0x43, 0x03}};
        }
        case FitPlusModel::Etappe:
            return {kPollData, kPollData, {0x02, 0x41, 0x02, 0x43, 0x03}, {0x02, 0x41, 0x03, 0x42, 0x03},
                    {0x02, 0x44, 0x01, 0x45, 0x03},
                    {0x02, 0x44, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x3c, 0xaa, 0x18, 0x00, 0xc0, 0x03}, kPollData,
                    {0x02, 0x44, 0x0b, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4e, 0x03},
                    {0x02, 0x44, 0x02, 0x46, 0x03}};
        case FitPlusModel::SX600:
            // Upstream sends the last two 02 42 42 03 frames with sizeof(initData2)
            // (an 8-byte over-read); only the 4 defined bytes are sent here.
            return {kPollData, kSx600Status, {0x02, 0x44, 0x01, 0x45, 0x03}, kSx600Status,
                    {0x02, 0x44, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x3c, 0xaa, 0x18, 0x00, 0xc0, 0x03}, kSx600Status,
                    {0x02, 0x44, 0x0b, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4e, 0x03}, kPollData,
                    {0x02, 0x44, 0x02, 0x46, 0x03}, kPollData};
        case FitPlusModel::Generic:
        default:
            return {{0x02, 0x44, 0x01, 0x45, 0x03},
                    {0x02, 0x44, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4f, 0x03},
                    {0x02, 0x44, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x46, 0xaa, 0x19, 0x00, 0xbb, 0x03},
                    {0x02, 0x44, 0x02, 0x46, 0x03},
                    {0x02, 0x41, 0x02, 0x43, 0x03}};
    }
}

std::uint8_t fitplusPollCycle(FitPlusModel model) { return model == FitPlusModel::SX600 ? 5 : 2; }

Bytes fitplusPollFrame(FitPlusModel model, std::uint8_t counter) {
    if (model == FitPlusModel::SX600) {
        switch (counter) {
            case 0: return kPollData;
            case 1: return kSx600Poll1;
            case 3: return kPollData1;
            default: return kSx600Status;
        }
    }
    return counter == 0 ? kPollData : kPollData1;
}

Bytes fitplusResistanceFrame(FitPlusModel model, int level) {
    if (level < 1 || level > 32 || model == FitPlusModel::Generic) return {};
    if (model == FitPlusModel::SX600) {
        const auto& row = kSx600Table[level - 1];
        return {0x02, 0x44, 0x05, row[0], row[1], row[2], 0x03};
    }
    const auto& row = kResistanceTable[level - 1];
    return {0x02, 0x44, 0x05, static_cast<std::uint8_t>(level), row[0], row[1], 0x03};
}

int fitplusDefaultMaxResistance(FitPlusModel model) {
    return model == FitPlusModel::H9110 || model == FitPlusModel::SX600 ? 32 : 24;
}

bool decodeFitPlus(const std::uint8_t* data, std::size_t length, bool longLayout, FitPlusData& out) {
    out = {};
    if (!data) return false;
    out.longLayout = longLayout;
    if (longLayout) {
        if (length != 15) return false;
        out.resistance = data[5];
        out.cadence = static_cast<float>(data[6]);
        out.power = static_cast<float>((data[10] << 8) | data[9]) / 10.0F;
        return true;
    }
    if (length != 14) return false;
    out.cadence = static_cast<float>(data[8]);
    out.speed = static_cast<float>((data[7] << 8) | data[6]) / 10.0F;
    return true;
}

// bluetooth.cpp fitshowtreadmill branch. Earlier upstream branches take
// FS-YK- (FTMS bike) and SWALK LITE- (Horizon/FTMS treadmill). FS-, TR510-T
// and TUNTURI T80- stay on FitShow even with FTMS (fs_connected /
// tunturi_t80_connected); for the other names upstream switches to FTMS when
// the 0x1826 service exists, so FTMS advertisements are excluded.
bool matchesTreadmill(const ble::Advertisement& a) {
    const std::string& n = a.name;
    const std::string u = upper(n);
    if (starts(u, "FS-YK-") || starts(u, "SWALK LITE-")) return false;
    if (starts(n, "FS-") || starts(u, "TR510-T") || starts(u, "TUNTURI T80-")) return true;
    if (hasFtms(a)) return false;
    if (starts(n, "SW") && n.size() == 14 && n.find('(') == std::string::npos && n.find(')') == std::string::npos)
        return true;
    if (starts(n, "SW") && n.size() == 15 && n.find('-') == 10) return true;
    return starts(u, "WINFITA") || starts(u, "SW-BLE") || starts(n, "BF70");
}

bool matchesNoblePro(const ble::Advertisement& a) {
    return !hasFtms(a) && starts(upper(a.name), "NOBLEPRO CONNECT");
}

bool matchesEtappe(const ble::Advertisement& a) { return !hasFtms(a) && starts(a.name, "X100-"); }

// MRK- names claimed by earlier upstream branches (elliptical, treadmill,
// FTMS bikes, rowers) never reach fitplusbike.
bool matchesMerach(const ble::Advertisement& a) {
    if (hasFtms(a) || !starts(a.name, "MRK-")) return false;
    const std::string u = upper(a.name);
    for (const char* p : {"MRK-R11S-", "MRK-E", "MRK-T", "MRK-S26S-", "MRK-S26C-", "MRK-S28-", "MRK-S38-",
                          "MRK-R28-", "MRK-CRYDN-", "MRK-R06-", "MRK-R15-"})
        if (starts(u, p)) return false;
    return true;
}

bool matchesH9110(const ble::Advertisement& a) { return !hasFtms(a) && starts(upper(a.name), "H9110 OSAKA"); }

// Upstream documents that the R28 advertises FTMS but works with FFF0.
bool matchesFitPlusRower(const ble::Advertisement& a) { return starts(upper(a.name), "MRK-R28-"); }

// snodebike is an FTMS (0x1826/0x2AD2) client; TF- goes there regardless of
// FTMS advertising (TF-T is a Toorx treadmill).
bool matchesSnode(const ble::Advertisement& a) {
    const std::string u = upper(a.name);
    return starts(u, "TF-") && !starts(u, "TF-T");
}

namespace {

bool manualOnly(const ble::Advertisement&) { return false; }

template <TreadmillVariant V>
std::unique_ptr<SourceProtocol> makeTreadmill() {
    return std::unique_ptr<SourceProtocol>(new FitshowTreadmillSource(V));
}
template <FitPlusModel M>
std::unique_ptr<SourceProtocol> makeFitPlus() {
    return std::unique_ptr<SourceProtocol>(new FitPlusSource(M));
}

const ProtocolInfo kDescriptors[] = {
    {"fitshow-tm", "FitShow treadmill", kMatchPriority, matchesTreadmill,
     makeTreadmill<TreadmillVariant::Standard>, true, false, MachineProfile::Treadmill},
    {"fitshow-noblepro", "NoblePro Connect treadmill (FitShow)", kMatchPriority, matchesNoblePro,
     makeTreadmill<TreadmillVariant::NoblePro>, true, false, MachineProfile::Treadmill},
    {"fitshow-tm-mi", "FitShow treadmill, mph console (manual)", 0, manualOnly,
     makeTreadmill<TreadmillVariant::Miles>, true, false, MachineProfile::Treadmill},
    {"fitshow-anyrun", "FitShow treadmill, AnyRun layout (manual)", 0, manualOnly,
     makeTreadmill<TreadmillVariant::AnyRun>, true, false, MachineProfile::Treadmill},
    {"snode-bike", "S-Node / TF- FTMS bike (read-only)", kMatchPriority, matchesSnode, createSnodeBike, false,
     false, MachineProfile::IndoorBike},
    {"fitplus-bike", "FitPlus FS- bike (manual)", 0, manualOnly, makeFitPlus<FitPlusModel::Generic>, false, false,
     MachineProfile::IndoorBike},
    {"fitplus-etappe", "Virtufit Etappe 2.0i (FitPlus)", kMatchPriority, matchesEtappe,
     makeFitPlus<FitPlusModel::Etappe>, true, false, MachineProfile::IndoorBike},
    {"fitplus-merach", "Merach MRK bike (FitPlus)", kMatchPriority, matchesMerach,
     makeFitPlus<FitPlusModel::Merach>, true, false, MachineProfile::IndoorBike},
    {"fitplus-h9110", "H9110 Osaka bike (FitPlus)", kMatchPriority, matchesH9110, makeFitPlus<FitPlusModel::H9110>,
     true, false, MachineProfile::IndoorBike},
    {"fitplus-sx600", "Sportstech SX600 (FitPlus, manual)", 0, manualOnly, makeFitPlus<FitPlusModel::SX600>, true,
     false, MachineProfile::IndoorBike},
    {"fitplus-rower", "Merach R28 rower (FitPlus)", kMatchPriority, matchesFitPlusRower,
     makeFitPlus<FitPlusModel::Rower>, true, false, MachineProfile::Rower},
};

}  // namespace

std::size_t descriptorCount() { return sizeof(kDescriptors) / sizeof(kDescriptors[0]); }
const ProtocolInfo& descriptorAt(std::size_t index) {
    return kDescriptors[index < descriptorCount() ? index : 0];
}

}  // namespace fitshow

using namespace fitshow;

std::unique_ptr<SourceProtocol> createSnodeBike() {
    FtmsSource::Options options;
    options.id = "snode-bike";
    options.allowControl = false;
    options.bikeDataOnly = true;
    return std::unique_ptr<SourceProtocol>(new FtmsSource(options));
}

// ---- FitShow treadmill ------------------------------------------------------

FitshowTreadmillSource::FitshowTreadmillSource(TreadmillVariant variant) : variant_(variant) {}

const char* FitshowTreadmillSource::id() const {
    switch (variant_) {
        case TreadmillVariant::Miles: return "fitshow-tm-mi";
        case TreadmillVariant::AnyRun: return "fitshow-anyrun";
        case TreadmillVariant::NoblePro: return "fitshow-noblepro";
        default: return "fitshow-tm";
    }
}

ble::ConnectPlan FitshowTreadmillSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(uuid16(0xfff0), uuid16(0xfff1));
    plan.subscribe(uuid16(0xffe0), uuid16(0xffe4));
    plan.subscribe(uuid16(0xae00), uuid16(0xae02));
    plan.subscribe(uuid16(0x1814), uuid16(0x2a53));  // RSC cadence
    return plan;
}

void FitshowTreadmillSource::reset() {
    SourceProtocol::reset();
    service_ = write_ = ble::Uuid();
    active_ = 0;
    queue_.clear();
    retry_ = 0;
    ready_ = false;
    lastTickMs_ = statusMs_ = 0;
    state_ = 0xff;
    speedTenths_ = 0;
    rawIncline_ = 0.0F;
    maxSpeed_ = 300;
    minSpeed_ = 0;
    maxIncline_ = 30;
    minIncline_ = 0;
}

bool FitshowTreadmillSource::onConnected(std::uint32_t mask, std::uint32_t nowMs) {
    // Upstream prefers FFF0, else the first of FFE0 / AE00 (NoblePro).
    static const std::uint16_t kServices[3][2] = {{0xfff0, 0xfff2}, {0xffe0, 0xffe1}, {0xae00, 0xae01}};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(mask & (1U << i))) continue;
        active_ = i;
        service_ = uuid16(kServices[i][0]);
        write_ = uuid16(kServices[i][1]);
        for (const auto& payload : treadmillInitPayloads(variant_ == TreadmillVariant::AnyRun)) schedule(payload);
        ready_ = true;
        lastTickMs_ = nowMs;
        return true;
    }
    setError("FitShow FFF0/FFE0/AE00 notify characteristic unavailable");
    return false;
}

void FitshowTreadmillSource::schedule(const Bytes& payload) {
    if (queue_.size() < kMaxQueue) queue_.push_back(payload);
}

void FitshowTreadmillSource::dequeue() {
    if (!queue_.empty()) queue_.erase(queue_.begin());
    retry_ = 0;
}

void FitshowTreadmillSource::send(const Bytes& payload) { queueWrite(service_, write_, frame(payload), false); }

void FitshowTreadmillSource::sportData() {
    if (variant_ == TreadmillVariant::AnyRun) return;
    const Bytes payload = sportDataPayload();
    for (const auto& queued : queue_)
        if (queued == payload) return;
    schedule(payload);
}

float FitshowTreadmillSource::unitFactor() const { return variant_ == TreadmillVariant::Miles ? 1.60934F : 1.0F; }

bool FitshowTreadmillSource::running(std::uint32_t nowMs) const {
    return ready_ && state_ == kStatusRunning && nowMs - statusMs_ <= kStatusFreshMs;
}

void FitshowTreadmillSource::tick(std::uint32_t nowMs) {
    if (!ready_ || nowMs - lastTickMs_ < kTickMs) return;
    lastTickMs_ = nowMs;
    if (retry_ >= 6) dequeue();  // 3 attempts, each followed by one idle tick
    if (!queue_.empty()) {
        ++retry_;
        if (retry_ % 2) send(queue_.front());
    } else {
        send(statusPayload());
    }
}

void FitshowTreadmillSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length,
                                      std::uint32_t nowMs) {
    if (!ready_ || !data) return;
    if (index == 3) {
        if (length < 4) return;
        sample_.cadence = {static_cast<float>(data[3]), true};
        touch(nowMs);
        return;
    }
    if (index != active_ || !validFrame(data, length)) return;
    handleFrame(data, length, nowMs);
}

void FitshowTreadmillSource::handleFrame(const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    const std::uint8_t cmd = data[1];
    const std::uint8_t par = data[2];
    if (cmd != kSysStatus && !queue_.empty() && queue_.front().size() >= 2 && queue_.front()[0] == cmd &&
        queue_.front()[1] == par)
        dequeue();

    if (cmd == kSysInfo) {
        if (par == 2 && length > 6) {
            maxSpeed_ = data[3];
            minSpeed_ = data[4];
        } else if (par == 5 && length >= 9) {
            maxSpeed_ = data[3];
            minSpeed_ = data[4];
            maxIncline_ = data[5];
            minIncline_ = data[6];
        } else if (par == 3) {
            if (length < 7) {
                maxIncline_ = 0;
            } else {
                maxIncline_ = data[3];
                minIncline_ = data[4];
            }
        }
        return;
    }

    if (cmd == kSysStatus) {
        Status status;
        if (!decodeStatus(data, length, variant_ == TreadmillVariant::AnyRun, status)) return;
        state_ = status.state;
        statusMs_ = nowMs;
        const bool metricsState = par == kStatusRunning || par == kStatusStop || par == kStatusPaused ||
                                  par == kStatusEnd;
        if (par == kStatusStart) return;
        if (metricsState) {
            if (!status.hasMetrics) return;
            float incline = maxIncline_ == 0 ? 0.0F : static_cast<float>(status.incline);
            if (variant_ == TreadmillVariant::NoblePro) incline /= 2.0F;
            rawIncline_ = incline;
            speedTenths_ = status.speedTenths;
            const bool isRunning = par == kStatusRunning;
            sample_.speed = {isRunning ? status.speedTenths / 10.0F * unitFactor() : 0.0F, true};
            sample_.incline = {incline, true};
            sample_.elapsed = {static_cast<float>(status.elapsed), true};
            if (status.steps > 0) sample_.stepCount = {static_cast<float>(status.steps), true};
            sample_.heartRate = {static_cast<float>(status.heartRate), status.heartRate != 0};
            touch(nowMs);
            if (!isRunning) sportData();
            return;
        }
        if (par == kStatusNormal || par == kStatusSafety || (par == kStatusError && length > 5)) sportData();
        speedTenths_ = 0;
        rawIncline_ = 0.0F;
        sample_.speed = {0.0F, true};
        sample_.incline = {0.0F, true};
        touch(nowMs);
        return;
    }

    if (cmd == kSysData && par == 0x00 && length >= 13) {
        const std::uint8_t* a = data + 1;
        const auto elapsed = static_cast<std::uint16_t>(a[2] | (a[3] << 8));
        const auto steps = static_cast<std::uint16_t>(a[8] | (a[9] << 8));
        sample_.elapsed = {static_cast<float>(elapsed), true};
        if (steps > 0) sample_.stepCount = {static_cast<float>(steps), true};
        touch(nowMs);
    }
}

bool FitshowTreadmillSource::control(const ControlRequest& request, std::uint32_t nowMs) {
    if (!running(nowMs) || maxSpeed_ <= 0) return false;
    const float inclineScale = variant_ == TreadmillVariant::NoblePro ? 2.0F : 1.0F;
    auto clampF = [](float v, float lo, float hi) { return v >= hi ? hi : (v <= lo ? lo : v); };
    float speed = static_cast<float>(speedTenths_);
    float incline = rawIncline_;
    if (request.control == Control::Speed) {
        speed = request.value / unitFactor() * 10.0F;
    } else if (request.control == Control::Incline) {
        if (maxIncline_ <= minIncline_) return false;
        const float steps = 1.0F / (variant_ == TreadmillVariant::NoblePro ? 0.5F : 1.0F);
        incline = std::round(request.value * steps) / steps;
        if (incline == rawIncline_) return true;
    } else {
        return false;
    }
    speed = clampF(speed, static_cast<float>(minSpeed_), static_cast<float>(maxSpeed_));
    incline = clampF(incline, static_cast<float>(minIncline_), static_cast<float>(maxIncline_));
    const auto speedByte = static_cast<std::uint8_t>(speed + 0.5F);
    if (request.control == Control::Speed && speedByte == speedTenths_) return true;
    schedule(targetPayload(speedByte, static_cast<std::uint8_t>(incline * inclineScale)));
    return true;
}

TrainerCapabilities FitshowTreadmillSource::capabilities() const {
    TrainerCapabilities caps;
    caps.metrics = observedMetrics();
    const bool active = ready_ && state_ == kStatusRunning && maxSpeed_ > 0;
    caps.setControl(Control::Speed, active);
    caps.setControl(Control::Incline, active && maxIncline_ > minIncline_);
    return caps;
}

// ---- FitPlus bikes / rower ----------------------------------------------------

FitPlusSource::FitPlusSource(FitPlusModel model) : model_(model), maxResistance_(fitplusDefaultMaxResistance(model)) {}

const char* FitPlusSource::id() const {
    switch (model_) {
        case FitPlusModel::Etappe: return "fitplus-etappe";
        case FitPlusModel::Merach: return "fitplus-merach";
        case FitPlusModel::H9110: return "fitplus-h9110";
        case FitPlusModel::SX600: return "fitplus-sx600";
        case FitPlusModel::Rower: return "fitplus-rower";
        default: return "fitplus-bike";
    }
}

MachineProfile FitPlusSource::profile() const {
    return model_ == FitPlusModel::Rower ? MachineProfile::Rower : MachineProfile::IndoorBike;
}

ble::ConnectPlan FitPlusSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(uuid16(0xfff0), uuid16(0xfff1));
    if (model_ == FitPlusModel::SX600) plan.subscribe(uuid16(ftms::kServiceUuid), uuid16(ftms::kIndoorBikeDataUuid));
    return plan;
}

void FitPlusSource::reset() {
    SourceProtocol::reset();
    ready_ = ftmsData_ = validShortFrame_ = false;
    counter_ = 0;
    lastTickMs_ = 0;
    maxResistance_ = fitplusDefaultMaxResistance(model_);
}

bool FitPlusSource::controllable() const { return model_ != FitPlusModel::Generic; }

void FitPlusSource::send(const Bytes& bytes) { queueWrite(uuid16(0xfff0), uuid16(0xfff2), bytes, false); }

bool FitPlusSource::onConnected(std::uint32_t mask, std::uint32_t nowMs) {
    if (!(mask & 1U)) {
        // Upstream switches such devices to its FTMS driver.
        setError("FitPlus FFF1 unavailable; use the ftms adapter");
        return false;
    }
    ftmsData_ = model_ == FitPlusModel::SX600 && (mask & 2U);
    for (const auto& bytes : fitplusInitFrames(model_)) send(bytes);
    ready_ = true;
    lastTickMs_ = nowMs;
    return true;
}

void FitPlusSource::tick(std::uint32_t nowMs) {
    if (!ready_ || nowMs - lastTickMs_ < kTickMs) return;
    lastTickMs_ = nowMs;
    if (pendingWrites() != 0) return;
    send(fitplusPollFrame(model_, counter_));
    counter_ = static_cast<std::uint8_t>((counter_ + 1) % fitplusPollCycle(model_));
}

void FitPlusSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (!ready_ || !data) return;
    if (index == 1) {
        if (!ftmsData_) return;
        ftms::IndoorBikeData bike;
        if (!ftms::decodeIndoorBikeData(data, length, bike)) return;
        if (bike.instantaneousSpeed) sample_.speed = {*bike.instantaneousSpeed / 100.0F, true};
        if (bike.instantaneousCadence) sample_.cadence = {*bike.instantaneousCadence / 2.0F, true};
        if (bike.resistanceLevel) sample_.resistance = {*bike.resistanceLevel / 10.0F, true};
        if (bike.instantaneousPower) {
            const bool idle = sample_.cadence.valid && sample_.cadence.value == 0.0F;
            sample_.power = {idle ? 0.0F : static_cast<float>(*bike.instantaneousPower), true};
        }
        if (bike.totalEnergy) sample_.totalEnergy = {static_cast<float>(*bike.totalEnergy), true};
        if (bike.heartRate) sample_.heartRate = {static_cast<float>(*bike.heartRate), true};
        touch(nowMs);
        return;
    }
    if (index != 0) return;
    if (model_ == FitPlusModel::Etappe && length >= 9 && data[0] == 0x02 && data[1] == 0x41 && data[2] == 0x02 &&
        data[3] >= 8 && data[3] <= 32)
        maxResistance_ = data[3];
    if (ftmsData_) return;  // SX600 reads its data from FTMS when available

    FitPlusData decoded;
    if (model_ == FitPlusModel::Generic) {
        if (!decodeFitPlus(data, length, false, decoded)) {
            if (!validShortFrame_ && length == 15 && data[0] == 0x02 && data[1] == 0x42 && data[2] == 0x02)
                setError("Virtufit Etappe data layout detected: pair as fitplus-etappe");
            return;
        }
        validShortFrame_ = true;
        sample_.cadence = {decoded.cadence, true};
        sample_.speed = {decoded.speed, true};
        touch(nowMs);
        return;
    }
    if (!decodeFitPlus(data, length, true, decoded)) return;
    sample_.resistance = {static_cast<float>(decoded.resistance), true};
    sample_.cadence = {decoded.cadence, true};
    sample_.power = {decoded.cadence == 0.0F ? 0.0F : decoded.power, true};
    touch(nowMs);
}

bool FitPlusSource::control(const ControlRequest& request, std::uint32_t) {
    if (!ready_ || !controllable() || request.control != Control::Resistance) return false;
    const int level = percentToLevel(request.value, 1, maxResistance_);
    const Bytes bytes = fitplusResistanceFrame(model_, level);
    if (bytes.empty()) return false;
    if (sample_.resistance.valid && static_cast<int>(sample_.resistance.value) == level) return true;
    send(bytes);
    return true;
}

TrainerCapabilities FitPlusSource::capabilities() const {
    TrainerCapabilities caps;
    caps.metrics = observedMetrics();
    caps.setControl(Control::Resistance, ready_ && controllable());
    return caps;
}

}  // namespace protocols
}  // namespace smartfitness
