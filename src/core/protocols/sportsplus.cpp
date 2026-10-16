// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/sportstechbike/sportstechbike.{cpp,h}
// src/devices/sportstechelliptical/sportstechelliptical.{cpp,h}
// src/devices/sportstechrower/sportstechrower.{cpp,h}
// src/devices/sportsplusbike/sportsplusbike.{cpp,h}
// src/devices/sportsplusrower/sportsplusrower.{cpp,h}
// src/devices/pafersbike/pafersbike.{cpp,h}
// src/devices/pafersrower/pafersrower.{cpp,h}
// src/devices/paferstreadmill/paferstreadmill.{cpp,h}
// src/devices/rower.cpp, src/devices/bluetooth.cpp, src/qzsettings.h
// This port is part of smartfitness-bridge and distributed under the same license.

#include "protocols/sportsplus.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace smartfitness {
namespace protocols {
namespace sportsplus {

const ble::Uuid kFff0 = ble::Uuid::from16(0xfff0);
const ble::Uuid kFff1 = ble::Uuid::from16(0xfff1);
const ble::Uuid kFff2 = ble::Uuid::from16(0xfff2);
const ble::Uuid kFff3 = ble::Uuid::from16(0xfff3);
const ble::Uuid kFfe2 = ble::Uuid::from16(0xffe2);
const ble::Uuid kPafersService = ble::Uuid::fromString("72d70001-501f-46f7-95f9-23846ee1aba3");
const ble::Uuid kPafersWrite = ble::Uuid::fromString("72d70002-501f-46f7-95f9-23846ee1aba3");
const ble::Uuid kPafersNotify = ble::Uuid::fromString("72d70003-501f-46f7-95f9-23846ee1aba3");

namespace {

std::string upper(const std::string& name) {
    std::string out = name;
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
bool startsWith(const std::string& name, const char* prefix) {
    return name.compare(0, std::strlen(prefix), prefix) == 0;
}
bool ftms(const ble::Advertisement& a) { return a.hasService(0x1826); }
int clampLevel(int level, int maximum) { return level < 0 ? 0 : (level > maximum ? maximum : level); }
unsigned be16(const std::uint8_t* d, std::size_t at) { return (static_cast<unsigned>(d[at]) << 8) | d[at + 1]; }
// Two hex nibbles read as decimal digits (upstream "hexint" decoding).
unsigned bcd(std::uint8_t value) { return ((value & 0xf0U) >> 4) * 10U + (value & 0x0fU); }
void heart(MetricSet& out, std::uint8_t value) {
    if (value) out.heartRate = {static_cast<float>(value), true};
}

// Upstream bluetooth.cpp else-if chain order (first match wins).
bool sportstechBikeRule(const std::string& u, std::size_t length) {
    return startsWith(u, "EW-BK") || (startsWith(u, "AF") && length == 7);
}
bool sportsplusBikeRule(const std::string& u, std::size_t length) {
    // HT (10 chars) only with upstream's default ftms_bike setting "Disabled".
    return startsWith(u, "CARDIOFIT") || (startsWith(u, "HT") && length == 10) ||
           (u.find("CARE") != std::string::npos && length == 11);
}
bool sportsplusRowerRule(const std::string& u, std::size_t length) {
    return (u.find("CARE") != std::string::npos && length >= 12) || startsWith(u, "VMAX");
}
enum class Rule { None, SportstechBike, SportstechElliptical, SportstechRower, SportsplusBike, SportsplusRower, Pafers };
Rule firstRule(const ble::Advertisement& a) {
    if (ftms(a)) return Rule::None;
    const std::string u = upper(a.name);
    const std::size_t n = a.name.size();
    if (sportstechBikeRule(u, n)) return Rule::SportstechBike;
    if (startsWith(u, "EW-EP-")) return Rule::SportstechElliptical;
    if (startsWith(u, "EW-ST-")) return Rule::SportstechRower;
    if (sportsplusBikeRule(u, n)) return Rule::SportsplusBike;
    if (sportsplusRowerRule(u, n)) return Rule::SportsplusRower;
    if (startsWith(u, "PAFERS_")) return Rule::Pafers;
    return Rule::None;
}
bool htVariantName(const std::string& name) {
    if (name.size() != 10 || !startsWith(upper(name), "HT")) return false;
    for (std::size_t i = 2; i < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    return true;
}
bool carefitnessName(const std::string& name) {
    return upper(name).find("CARE") != std::string::npos && name.size() >= 11;
}

constexpr float kCareWattTable[24][6] = {
    {13, 17, 22, 27, 33, 39},      {14, 19, 24, 30, 36, 42},      {15, 21, 26, 33, 39, 45},
    {16, 22, 28, 34, 42, 49},      {18, 24, 30, 37, 45, 53},      {20, 27, 34, 42, 51, 60},
    {22, 30, 38, 47, 57, 67},      {24, 33, 42, 53, 63, 75},      {27, 36, 46, 58, 70, 83},
    {31, 41, 53, 67, 81, 96},      {35, 47, 60, 76, 92, 110},     {39, 53, 67, 85, 104, 124},
    {33, 59, 75, 95, 116, 138},    {47, 63, 81, 103, 126, 151},   {50, 68, 88, 111, 136, 164},
    {54, 74, 95, 120, 147, 178},   {58, 80, 103, 129, 158, 192},  {63, 86, 111, 139, 169, 206},
    {68, 92, 119, 149, 181, 220},  {73, 98, 127, 159, 193, 234},  {77, 104, 134, 168, 205, 248},
    {81, 110, 141, 177, 217, 262}, {86, 116, 149, 187, 229, 276}, {91, 122, 157, 197, 240, 290}};

}  // namespace

bool matchesSportstechBike(const ble::Advertisement& a) { return firstRule(a) == Rule::SportstechBike; }
bool matchesSportstechElliptical(const ble::Advertisement& a) { return firstRule(a) == Rule::SportstechElliptical; }
bool matchesSportstechRower(const ble::Advertisement& a) { return firstRule(a) == Rule::SportstechRower; }
bool matchesSportsplusHt(const ble::Advertisement& a) {
    return firstRule(a) == Rule::SportsplusBike && htVariantName(a.name);
}
bool matchesCarefitnessBike(const ble::Advertisement& a) {
    return firstRule(a) == Rule::SportsplusBike && !htVariantName(a.name) && carefitnessName(a.name);
}
bool matchesSportsplusBike(const ble::Advertisement& a) {
    return firstRule(a) == Rule::SportsplusBike && !htVariantName(a.name) && !carefitnessName(a.name);
}
bool matchesSportsplusRower(const ble::Advertisement& a) { return firstRule(a) == Rule::SportsplusRower; }
// Upstream default settings (pafers_treadmill = pafers_rower = false) route PAFERS_ to the bike.
bool matchesPafersBike(const ble::Advertisement& a) { return firstRule(a) == Rule::Pafers; }

std::vector<Bytes> sportstechInitFrames() {
    return {{0xf2, 0xc0, 0x00, 0xb2},
            {0xf2, 0xc1, 0x05, 0x01, 0xff, 0xff, 0xff, 0xff, 0xb5},
            {0xf2, 0xc4, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xc0},
            {0xf2, 0xc3, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xbb}};
}
Bytes sportstechPollFrame(int level) {
    const auto l = static_cast<std::uint8_t>(clampLevel(level, kSportstechMaxLevel));
    return {0xf2, 0xc3, 0x07, 0x04, l, 0x00, 0x00, 0x00, 0xff, 0xff, static_cast<std::uint8_t>(0xbe + l)};
}
std::vector<Bytes> sportsplusInitFrames(Model model, bool xmFitness) {
    if (model == Model::SportsplusRower && xmFitness) return {{0x40, 0x00, 0xd1, 0x5f, 0x70}};
    if (model == Model::SportsplusBike || model == Model::SportsplusHt) return {{0x40, 0x00, 0x16, 0x0a, 0x60}};
    return std::vector<Bytes>(6, Bytes{0x40, 0x00, 0x9a, 0x56, 0x30});
}
Bytes sportsplusPollFrame(int level) {
    const auto l = static_cast<std::uint8_t>(clampLevel(level, kSportsplusMaxLevel));
    return {0x20, 0x01, l, 0x00, static_cast<std::uint8_t>(0x21 + l)};
}
std::vector<Bytes> pafersInitFrames(Model model) {
    if (model == Model::PafersBike || model == Model::PafersRower)
        return {{0x55, 0xbb, 0x01, 0xff}, {0x55, 0x0c, 0x01, 0xff}, {0x55, 0x25, 0x01, 0xff}, {0x55, 0x0a, 0x01, 0x01}};
    const bool iboxster = model == Model::PafersIboxster;
    std::vector<Bytes> out{{0x55, 0xbb, 0x01, 0xff}, {0x55, 0x0c, 0x01, 0xff}, {0x55, 0x1f, 0x01, 0xff}};
    if (iboxster) out.push_back(pafersPollFrame());
    out.push_back({0x55, 0x0a, 0x01, 0x02});
    out.push_back({0x55, 0x01, 0x06, 0x39, 0x01, 0x32, 0x19, 0xa5, 0x32});
    out.push_back({0x55, 0x15, 0x01, 0x00});
    out.push_back(iboxster ? Bytes{0x55, 0x0f, 0x02, 0x00, 0x0a} : Bytes{0x55, 0x0f, 0x02, 0x01, 0x00});
    out.push_back({0x55, 0x11, 0x01, 0x00});
    out.push_back({0x55, 0x08, 0x01, 0x01});
    return out;
}
Bytes pafersPollFrame() { return {0x55, 0x0d, 0x0a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}; }
Bytes pafersResistanceFrame(std::uint8_t level) { return {0x55, 0x11, 0x01, level}; }
Bytes pafersSpeedFrame(std::uint8_t kmh) { return {0x55, 0x0f, 0x02, kmh, 0x00}; }
Bytes pafersInclineFrame(std::uint8_t percent) { return {0x55, 0x11, 0x01, percent}; }
Bytes pafersStopFrame() { return {0x55, 0x0a, 0x01, 0x02}; }

bool decodeSportstech(const std::uint8_t* d, std::size_t length, MetricSet& out) {
    out = {};
    if (!d || length != 20) return false;
    const float cadence = static_cast<float>(d[17]);
    out.speed = {static_cast<float>(be16(d, 12)) / 10.0F, true};
    out.totalEnergy = {static_cast<float>(be16(d, 7)), true};
    out.power = {cadence > 0.0F ? static_cast<float>(be16(d, 9)) : 0.0F, true};
    out.resistance = {static_cast<float>(d[15]), true};
    out.cadence = {cadence, true};
    heart(out, d[11]);
    return true;
}

float carefitnessWatts(int level, float cadence) {
    if (!(cadence > 0.0F)) return 0.0F;
    const int row = level < 0 ? 0 : (level > 23 ? 23 : level);
    const float* watts = kCareWattTable[row];
    int step = static_cast<int>(cadence / 10.0F - 5.0F);
    if (step < 0) step = 0;
    if (step >= 5) return std::floor(cadence / 100.0F * watts[5]);
    const float base = watts[step];
    return std::floor((watts[step + 1] - base) / 10.0F * static_cast<float>(static_cast<int>(cadence) % 10) + base);
}

float rowerWattsFromSpeed(float kmh) {
    if (!(kmh > 0.0F)) return 0.0F;
    const double pace = 1800.0 / kmh;  // s per 500 m
    const double velocity = 500.0 / pace;
    return static_cast<float>(2.8 * velocity * velocity * velocity);
}

bool decodeSportsplusBike(Model model, const std::uint8_t* d, std::size_t length, int level, MetricSet& out) {
    out = {};
    if (!d || length != 12) return false;
    switch (model) {
        case Model::SportsplusHt:
            if (d[1] == 0x00) {
                const float speed = static_cast<float>(bcd(d[2]) * 100U + bcd(d[3])) / 10.0F;
                out.speed = {speed, true};
                // Upstream keeps the legacy raw-speed cadence mapping (+12 rpm offset).
                out.cadence = {speed > 0.0F ? static_cast<float>(be16(d, 2)) / 10.0F + 12.0F : 0.0F, true};
            } else if (d[1] == 0x10) {
                out.power = {static_cast<float>(bcd(d[9]) * 100U + bcd(d[10])), true};
            }
            break;
        case Model::CarefitnessBike:
            if (d[1] == 0x30) {
                const float cadence = static_cast<float>(bcd(d[3]));
                out.cadence = {cadence, true};
                out.speed = {0.37497622F * cadence, true};
                out.power = {carefitnessWatts(level, cadence), true};
            }
            break;
        case Model::SportsplusHt9600ie: {
            const float speed = static_cast<float>(d[2] * 100U + bcd(d[3])) / 10.0F;
            out.speed = {speed, true};
            out.cadence = {speed * 2.685185F, true};
            out.power = {static_cast<float>(d[9] * 100U + bcd(d[10])), true};
            out.totalEnergy = {static_cast<float>(be16(d, 6)), true};
            break;
        }
        default:
            if (d[1] == 0x20) {
                const float speed = static_cast<float>(be16(d, 2)) / 100.0F;
                out.speed = {speed, true};
                out.cadence = {speed > 0.0F ? speed * 10.0F + 12.0F : 0.0F, true};
            } else if (d[1] == 0x30) {
                out.power = {static_cast<float>(be16(d, 2)), true};
            }
            out.totalEnergy = {static_cast<float>(be16(d, 6)), true};
            break;
    }
    heart(out, d[8]);
    return true;
}

bool decodeSportsplusRower(const std::uint8_t* d, std::size_t length, MetricSet& out) {
    out = {};
    if (!d || length != 12) return false;
    if (d[1] == 0x10) {
        const float cadence = static_cast<float>(bcd(d[3]));
        const float speed = 0.37497622F * cadence;
        out.cadence = {cadence, true};
        out.speed = {speed, true};
        out.power = {rowerWattsFromSpeed(speed), true};
    }
    heart(out, d[8]);
    return true;
}

bool decodePafers(const std::uint8_t* d, std::size_t length, MetricSet& out) {
    out = {};
    if (!d || length != 10) return false;
    out.speed = {static_cast<float>(d[3]), true};
    out.resistance = {static_cast<float>(d[5]), true};
    out.cadence = {static_cast<float>(d[7]), true};
    out.power = {d[7] ? static_cast<float>(be16(d, 8)) : 0.0F, true};
    return true;
}

bool decodePafersTreadmill(const std::uint8_t* d, std::size_t length, MetricSet& out) {
    out = {};
    if (!d || length != 13) return false;
    float speed = static_cast<float>(d[9]) + static_cast<float>(d[10]) / 100.0F;
    if (speed == 1.0F) speed = 0.0F;  // upstream: a raw 1.00 means stopped
    out.speed = {speed, true};
    out.incline = {static_cast<float>(d[11]), true};
    return true;
}

namespace {

template <Model M>
std::unique_ptr<SourceProtocol> factory() {
    return std::unique_ptr<SourceProtocol>(new SportsplusSource(M));
}
bool manualOnly(const ble::Advertisement&) { return false; }

const ProtocolInfo kDescriptors[] = {
    {"sportstech-bike", "Sportstech bike (EW-BK, AF7019)", kMatchPriority, matchesSportstechBike,
     factory<Model::SportstechBike>, true, false, MachineProfile::IndoorBike},
    {"sportstech-ellip", "Sportstech elliptical (EW-EP)", kMatchPriority, matchesSportstechElliptical,
     factory<Model::SportstechElliptical>, true, false, MachineProfile::CrossTrainer},
    {"sportstech-rower", "Sportstech rower (EW-ST)", kMatchPriority, matchesSportstechRower,
     factory<Model::SportstechRower>, false, false, MachineProfile::Rower},
    {"sportsplus-bike", "SportsPlus bike (CARDIOFIT, HT)", kMatchPriority, matchesSportsplusBike,
     factory<Model::SportsplusBike>, true, false, MachineProfile::IndoorBike},
    {"sportsplus-ht", "SportsPlus HT bike (HT + 8 digits)", kMatchPriority, matchesSportsplusHt,
     factory<Model::SportsplusHt>, true, false, MachineProfile::IndoorBike},
    {"carefit-bike", "Carefitness bike (CV-351)", kMatchPriority, matchesCarefitnessBike,
     factory<Model::CarefitnessBike>, true, false, MachineProfile::IndoorBike},
    {"sp-ht-9600ie", "SportsPlus HT-9600iE bike (manual)", 0, manualOnly, factory<Model::SportsplusHt9600ie>, true,
     false, MachineProfile::IndoorBike},
    {"sportsplus-rower", "SportsPlus / Carefitness / VMAX rower", kMatchPriority, matchesSportsplusRower,
     factory<Model::SportsplusRower>, true, false, MachineProfile::Rower},
    {"pafers-bike", "Pafers bike", kMatchPriority, matchesPafersBike, factory<Model::PafersBike>, true, false,
     MachineProfile::IndoorBike},
    {"pafers-rower", "Pafers rower (manual)", 0, manualOnly, factory<Model::PafersRower>, true, false,
     MachineProfile::Rower},
    {"pafers-treadmill", "Pafers treadmill (manual)", 0, manualOnly, factory<Model::PafersTreadmill>, true, false,
     MachineProfile::Treadmill},
    {"pafers-iboxster", "BH i.Boxster Plus / Pafers treadmill (manual)", 0, manualOnly,
     factory<Model::PafersIboxster>, true, false, MachineProfile::Treadmill},
};
constexpr std::size_t kDescriptorCount = sizeof(kDescriptors) / sizeof(kDescriptors[0]);
// SportsplusSource::id() indexes this table by Model.
static_assert(kDescriptorCount == static_cast<std::size_t>(Model::PafersIboxster) + 1, "one descriptor per model");

}  // namespace

std::size_t descriptorCount() { return kDescriptorCount; }
const ProtocolInfo& descriptorAt(std::size_t index) {
    return kDescriptors[index < kDescriptorCount ? index : 0];
}

}  // namespace sportsplus

using sportsplus::Model;

SportsplusSource::SportsplusSource(Model model) : model_(model) { reset(); }

const char* SportsplusSource::id() const { return sportsplus::descriptorAt(static_cast<std::size_t>(model_)).id; }
MachineProfile SportsplusSource::profile() const {
    return sportsplus::descriptorAt(static_cast<std::size_t>(model_)).profile;
}

bool SportsplusSource::sportstech() const {
    return model_ == Model::SportstechBike || model_ == Model::SportstechElliptical ||
           model_ == Model::SportstechRower;
}
bool SportsplusSource::sportsplusFamily() const {
    return model_ == Model::SportsplusBike || model_ == Model::SportsplusHt || model_ == Model::CarefitnessBike ||
           model_ == Model::SportsplusHt9600ie || model_ == Model::SportsplusRower;
}
bool SportsplusSource::pafers() const { return !sportstech() && !sportsplusFamily(); }
bool SportsplusSource::treadmill() const {
    return model_ == Model::PafersTreadmill || model_ == Model::PafersIboxster;
}

std::uint32_t SportsplusSource::pollIntervalMs() const {
    if (treadmill()) return 500;
    if (pafers()) return 2400;  // every 6th 400 ms refresh
    return 200;
}
std::uint32_t SportsplusSource::responseTimeoutMs() const { return pafers() ? 400 : 300; }

ble::ConnectPlan SportsplusSource::plan() const {
    ble::ConnectPlan plan;
    if (sportstech()) {
        plan.subscribe(sportsplus::kFff0, sportsplus::kFff1);
    } else if (model_ == Model::SportsplusRower) {
        // Upstream writes FFF2, or FFE2 when FFF2 is missing (XM Fitness), and
        // subscribes every notifying FFF0 characteristic.
        plan.discoverNotifications = true;
        plan.discoveryWriteCharacteristics[0] = sportsplus::kFff2;
        plan.discoveryWriteCharacteristics[1] = sportsplus::kFfe2;
    } else if (sportsplusFamily()) {
        plan.subscribe(sportsplus::kFff0, sportsplus::kFff1);
        plan.subscribe(sportsplus::kFff0, sportsplus::kFff2);
        plan.subscribe(sportsplus::kFff0, sportsplus::kFff3);
    } else {
        plan.subscribe(sportsplus::kPafersService, sportsplus::kPafersNotify);
    }
    return plan;
}

void SportsplusSource::reset() {
    SourceProtocol::reset();
    queue_.clear();
    connected_ = ready_ = awaiting_ = awaitResponse_ = false;
    sentMs_ = pollMs_ = 0;
    // Upstream starts SportsPlus bikes at level 1; the others at -1 clamped to 0.
    level_ = (sportsplusFamily() && model_ != Model::SportsplusRower) ? 1 : 0;
    if (model_ == Model::SportsplusRower) {
        writeService_ = {};
        write_ = {};
        xmFitness_ = false;
    } else if (sportstech()) {
        writeService_ = sportsplus::kFff0;
        write_ = sportsplus::kFff2;
    } else if (sportsplusFamily()) {
        writeService_ = sportsplus::kFff0;
        write_ = model_ == Model::CarefitnessBike ? sportsplus::kFff2 : sportsplus::kFff1;
    } else {
        writeService_ = sportsplus::kPafersService;
        write_ = sportsplus::kPafersWrite;
    }
}

void SportsplusSource::onWriteEndpoint(const ble::Uuid& service, const ble::Uuid& characteristic) {
    if (model_ != Model::SportsplusRower || service != sportsplus::kFff0) return;
    if (characteristic != sportsplus::kFff2 && characteristic != sportsplus::kFfe2) return;
    if (!write_.isZero() && write_ == sportsplus::kFff2) return;
    writeService_ = service;
    write_ = characteristic;
    xmFitness_ = characteristic == sportsplus::kFfe2;
}

bool SportsplusSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if (model_ == Model::SportsplusRower ? subscribedMask == 0 : (subscribedMask & 1U) == 0) {
        setError("notify characteristic unavailable");
        return false;
    }
    if (write_.isZero()) {
        setError("FFF2/FFE2 write characteristic unavailable");
        return false;
    }
    queue_.clear();
    std::vector<ble::Bytes> init;
    if (sportstech()) init = sportsplus::sportstechInitFrames();
    else if (sportsplusFamily()) init = sportsplus::sportsplusInitFrames(model_, xmFitness_);
    else init = sportsplus::pafersInitFrames(model_);
    for (const auto& frame : init) push(frame);
    connected_ = true;
    tick(nowMs);
    return true;
}

void SportsplusSource::push(const ble::Bytes& bytes, bool waitResponse) {
    if (queue_.size() < kMaxOutbox) queue_.push_back({bytes, waitResponse});
}

void SportsplusSource::tick(std::uint32_t nowMs) {
    if (!connected_ || failed()) return;
    if (awaiting_ && nowMs - sentMs_ < responseTimeoutMs()) return;
    awaiting_ = false;
    if (!queue_.empty()) {
        const Step step = queue_.front();
        queue_.erase(queue_.begin());
        if (!queueWrite(writeService_, write_, step.bytes)) return;
        awaiting_ = true;
        awaitResponse_ = step.waitResponse;
        sentMs_ = nowMs;
        return;
    }
    if (!ready_) {
        ready_ = true;
        pollMs_ = nowMs;
        if (!pafers()) pollMs_ -= pollIntervalMs();
    }
    if (nowMs - pollMs_ < pollIntervalMs()) return;
    pollMs_ = nowMs;
    ble::Bytes poll;
    if (sportstech()) poll = sportsplus::sportstechPollFrame(model_ == Model::SportstechRower ? 0 : level_);
    else if (sportsplusFamily()) poll = sportsplus::sportsplusPollFrame(level_);
    else poll = sportsplus::pafersPollFrame();
    if (!queueWrite(writeService_, write_, poll)) return;
    awaiting_ = true;
    // The treadmill display poll is written without waiting for a reply.
    awaitResponse_ = !treadmill();
    sentMs_ = nowMs;
}

void SportsplusSource::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (!connected_ || !data) return;
    if (awaiting_ && awaitResponse_) awaiting_ = false;
    MetricSet frame;
    bool ok = false;
    if (sportstech()) {
        ok = sportsplus::decodeSportstech(data, length, frame);
    } else if (model_ == Model::SportsplusRower) {
        ok = sportsplus::decodeSportsplusRower(data, length, frame);
    } else if (sportsplusFamily()) {
        ok = sportsplus::decodeSportsplusBike(model_, data, length, level_, frame);
    } else if (treadmill()) {
        if (length == 4 && data[0] == 0x55 && data[1] == 0x09 && data[2] == 0x01) {
            // Console stop is acknowledged like upstream; the console start
            // event is not answered because the bridge never starts the belt.
            if (data[3] == 0x00) push(sportsplus::pafersStopFrame());
            return;
        }
        ok = sportsplus::decodePafersTreadmill(data, length, frame);
    } else {
        ok = sportsplus::decodePafers(data, length, frame);
    }
    if (!ok) return;
    bool any = false;
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        const auto metric = static_cast<Metric>(i);
        if (frame[metric].valid) {
            sample_[metric] = frame[metric];
            any = true;
        }
    }
    // Upstream reports 0 W while the cadence is 0.
    if (!treadmill() && sample_.cadence.valid && sample_.cadence.value <= 0.0F && sample_.power.valid)
        sample_.power.value = 0.0F;
    // A zero heart-rate byte means "no reading" (upstream falls back to other sensors).
    if (!frame.heartRate.valid) sample_.heartRate = {};
    if (any) touch(nowMs);
}

bool SportsplusSource::control(const ControlRequest& request, std::uint32_t nowMs) {
    if (!capabilities().supports(request.control) || !std::isfinite(request.value)) return false;
    if (request.control == Control::Resistance) {
        if (sportstech()) {
            level_ = percentToLevel(request.value, 0, sportsplus::kSportstechMaxLevel);
            return true;  // carried by the next poll frame
        }
        if (sportsplusFamily()) {
            level_ = percentToLevel(request.value, 0, sportsplus::kSportsplusMaxLevel);
            return true;
        }
        const int level = percentToLevel(request.value, sportsplus::kPafersMinLevel, sportsplus::kPafersMaxLevel);
        level_ = level;
        if (sample_.resistance.valid && static_cast<int>(sample_.resistance.value) == level) return true;
        if (queue_.size() >= kMaxOutbox) return false;
        push(sportsplus::pafersResistanceFrame(static_cast<std::uint8_t>(level)));
        return true;
    }
    // Treadmill: only a running belt with fresh telemetry.
    if (!treadmill() || !hasData() || nowMs - lastDataMs() > 2000 || !sample_.speed.valid ||
        sample_.speed.value < sportsplus::kPafersMinSpeed || queue_.size() + 2 > kMaxOutbox)
        return false;
    float speed = sample_.speed.value;
    float incline = sample_.incline.valid ? sample_.incline.value : 0.0F;
    if (request.control == Control::Speed) {
        if (request.value < sportsplus::kPafersMinSpeed || request.value > sportsplus::kPafersMaxSpeed) return false;
        speed = request.value;
    } else if (request.control == Control::Incline) {
        if (request.value > sportsplus::kPafersMaxIncline) return false;
        incline = request.value < 0.0F ? 0.0F : request.value;
    } else {
        return false;
    }
    if (speed > sportsplus::kPafersMaxSpeed || incline < 0.0F || incline > sportsplus::kPafersMaxIncline) return false;
    // Upstream: forceSpeed, 400 ms pause, forceIncline (values truncated to integers).
    push(sportsplus::pafersSpeedFrame(static_cast<std::uint8_t>(speed)), false);
    push(sportsplus::pafersInclineFrame(static_cast<std::uint8_t>(incline)), false);
    return true;
}

TrainerCapabilities SportsplusSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    const bool active = connected_ && ready_ && !failed();
    if (treadmill()) {
        capabilities.setControl(Control::Speed, active && hasData());
        capabilities.setControl(Control::Incline, active && hasData());
    } else {
        capabilities.setControl(Control::Resistance, active && model_ != Model::SportstechRower);
    }
    return capabilities;
}

}  // namespace protocols
}  // namespace smartfitness
