// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{flywheelbike,solebike,kettlerc12bike,technogymbike,inspirebike,
// keepbike,mcfbike,xcxbike,chronobike,volavabike,ultrasportbike,skandikawiribike,
// pitpatbike,freebeatboombike,mepanelbike,apexbike,npecablebike,
// cycleopsphantombike,renphobike,lifespanbike,horizongr7bike,cscbike}/
// (same-named .cpp/.h); src/devices/bluetooth.cpp (advertisement rules).

#include "protocols/vendor_bikes.hpp"

#include <algorithm>
#include <cmath>

#include "ftms/ftms_codec.hpp"
#include "protocols/ftms_source.hpp"

namespace smartfitness {
namespace protocols {
namespace vendor_bikes {
namespace {

using ble::Advertisement;
using ble::Bytes;
using ble::Uuid;

// Row flags.
enum : std::uint16_t {
    kNoResponse = 1U << 0,   // upstream prefers write-without-response
    kFtms = 1U << 1,         // subscribe FTMS Indoor Bike Data
    kCps = 1U << 2,          // subscribe Cycling Power Measurement
    kCsc = 1U << 3,          // subscribe CSC Measurement
    kHr = 1U << 4,           // subscribe Heart Rate Measurement
    kNus = 1U << 5,          // NPE "BIKE " resistance frames on the NUS notify
    kKettler = 1U << 6,      // Kettler C12 per-metric characteristics
    kCpsPower = 1U << 7,     // CPS power is used (otherwise cadence only)
    kCpsWheel = 1U << 8,     // CPS wheel-only revolutions / 2 as cadence
    kCscWheel = 1U << 9,     // CSC wheel revolutions as cadence when no crank
    kZeroWatts = 1U << 10,   // upstream watts() is 0 while cadence is 0
    kCtlResistance = 1U << 11,
    kCtlPower = 1U << 12,
};

// Encoded frame lists: {length, delay after (10 ms units), bytes...}..., 0.
// clang-format off
const std::uint8_t kFlywheelInit[] = {5, 0, 0xf5, 0x20, 0x20, 0x40, 0xf6, 0};
const std::uint8_t kIc5Init[] = {
    19, 0, 0xff, 0x10, 0x05, 0x55, 0x6e, 0x6b, 0x6e, 0x6f, 0x77, 0x6e, 0x20, 0x44, 0x65, 0x76, 0x69, 0x63, 0x65, 0x45, 0x55,
    15, 0, 0xff, 0x0c, 0x02, 0x00, 0x2a, 0x5b, 0x02, 0x00, 0xe2, 0xb8, 0x01, 0x4a, 0x4a, 0x26, 0x55, 0};
#define SOLE_F0 4, 0, 0x5b, 0x01, 0xf0, 0x5d
#define SOLE_0301 5, 0, 0x5b, 0x02, 0x03, 0x01, 0x5d
#define SOLE_07 9, 0, 0x5b, 0x06, 0x07, 0x01, 0x23, 0x00, 0x9b, 0x43, 0x5d
#define SOLE_08 6, 0, 0x5b, 0x03, 0x08, 0x10, 0x01, 0x5d
#define SOLE_04 8, 0, 0x5b, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x5d
#define SOLE_0202 5, 0, 0x5b, 0x02, 0x02, 0x02, 0x5d
#define SOLE_OK(x) 7, 0, 0x5b, 0x04, 0x00, x, 0x4f, 0x4b, 0x5d
const std::uint8_t kSoleInit[] = {SOLE_F0, SOLE_0301, SOLE_07, SOLE_08, SOLE_04, SOLE_0202, SOLE_0202, SOLE_0202,
                                  5, 0, 0x5b, 0x02, 0x03, 0x04, 0x5d, 0};
const std::uint8_t kR92Init[] = {SOLE_F0, SOLE_F0, SOLE_OK(0x13), SOLE_0301, SOLE_OK(0x14), SOLE_0301, SOLE_OK(0x14),
                                 SOLE_OK(0x14), SOLE_07, SOLE_08, SOLE_04, SOLE_0202, SOLE_OK(0x09), SOLE_OK(0x10), 0};
const std::uint8_t kSolePoll[] = {SOLE_OK(0x10), SOLE_OK(0x06), SOLE_OK(0x13), 0};
const std::uint8_t kR92Poll[] = {SOLE_OK(0x10), SOLE_OK(0x06), SOLE_OK(0x14), 0};
const std::uint8_t kKettlerInit[] = {
    12, 0, 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    13, 0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
    2, 0, 0x25, 0x00, 2, 0, 0x25, 0x01, 2, 0, 0x25, 0x02, 2, 0, 0x25, 0x03, 2, 0, 0x25, 0x04,
    4, 0, 0x25, 0x05, 0x02, 0x00,
    13, 0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
    13, 0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0};
#define KEEP_SET 6, 0, 0x04, 0x53, 0x22, 0x00, 0x00, 0x80
const std::uint8_t kKeepInit[] = {
    20, 0, 0x01, 0x4b, 0xf6, 0x00, 0x2e, 0x35, 0x36, 0x64, 0x61, 0x34, 0x32, 0x61, 0x64, 0x64, 0x30, 0x36, 0x63, 0x39, 0x39, 0x38,
    18, 0, 0x01, 0x45, 0x01, 0x35, 0x31, 0x30, 0x39, 0x35, 0x31, 0x39, 0x34, 0x64, 0x00, 0x36, 0x62, 0x33, 0x21, 0xc8,
    18, 0, 0x01, 0x45, 0x02, 0x63, 0x38, 0x66, 0x65, 0x63, 0x65, 0x35, 0x37, 0x36, 0x38, 0x63, 0x66, 0x33, 0x33, 0x30,
    4, 0, 0x01, 0x50, 0x03, 0x34, 5, 0, 0x01, 0x53, 0x21, 0x00, 0x00, KEEP_SET, KEEP_SET, KEEP_SET,
    5, 0, 0x01, 0x53, 0x2a, 0x00, 0x00, 0};
const std::uint8_t kKeepPoll[] = {5, 0, 0x01, 0x53, 0x23, 0x00, 0x00, 0};
#define MCF(b1, b2, b3) 20, 0, 0xf9, b1, b2, b3
#define Z4 0x00, 0x00, 0x00, 0x00
const std::uint8_t kMcfInit[] = {
    MCF(0xd0, 0x00, 0xc9), Z4, Z4, Z4, Z4,
    MCF(0xd1, 0x05, 0x01), Z4, 0xd0, 0x00, 0x00, 0x00, Z4, Z4,
    MCF(0xd3, 0x0d, 0x01), 0x00, 0x00, 0x34, 0x01, 0x00, 0x63, 0x00, 0xb7, Z4, 0x29, 0x00, 0x00, 0x00,
    MCF(0xd0, 0x00, 0xc9), Z4, Z4, Z4, Z4,
    MCF(0xd0, 0x00, 0xc9), Z4, Z4, Z4, Z4,
    MCF(0xd4, 0x0f, 0x00), Z4, Z4, Z4, 0x1f, 0x0f, 0x0a, 0x00,
    MCF(0xd5, 0x0d, 0x00), Z4, Z4, Z4, 0xdb, 0x00, 0x00, 0x00,
    MCF(0xd5, 0x0d, 0x01), Z4, Z4, Z4, 0xdc, 0x00, 0x00, 0x00,
    MCF(0xd5, 0x0d, 0x00), Z4, Z4, Z4, 0xdb, 0x00, 0x00, 0x00, 0};
const std::uint8_t kMcfPoll[] = {MCF(0xd5, 0x0d, 0x01), Z4, Z4, Z4, 0xdc, 0x00, 0x00, 0x00, 0};
const std::uint8_t kVolavaInit[] = {
    15, 20, 0xaa, 0x0f, 0x8a, 0x03, 0x01, 0x05, 0x02, 0x09, 0x06, 0x20, 0x50, 0x30, 0x80, 0x20, 0x9d,
    6, 0, 0xaa, 0x06, 0x80, 0xe1, 0x00, 0x11, 0};
#define US_A 5, 0, 0xff, 0xfd, 0x03, 0x00, 0xec
#define US_B 3, 0, 0xff, 0xaa, 0xec
const std::uint8_t kUltrasportInit[] = {US_A, US_B, US_A, US_A, US_A, US_A, US_A, US_A, US_A, US_B, 0};
const std::uint8_t kUltrasportPoll[] = {US_A, 0};
const std::uint8_t kSkandikaInit[] = {5, 0, 0x40, 0x00, 0x9a, 0x24, 0xfe, 0};
const std::uint8_t kX2000Init[] = {5, 0, 0x40, 0x00, 0x9a, 0x38, 0x12, 5, 0, 0x40, 0x00, 0x9a, 0x38, 0x12, 0};
const std::uint8_t kPitpatPoll[] = {5, 0, 0x6a, 0x05, 0xfd, 0xf8, 0x43, 0};
const std::uint8_t kFreebeatPoll[] = {5, 0, 0x25, 0x08, 0x00, 0x08, 0xa0, 0};
const std::uint8_t kMepanelInit[] = {4, 0, 0x1e, 0x11, 0x00, 0x11, 0};
#define APEX_POLL 10, 200, 0xaa, 0x55, 0xe0, 0x00, 0x00, 0x00, 0x00, 0xe0, 0xc3, 0x3c
#define APEX_INIT 10, 50, 0xeb, 0x50, 0x51, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa2
const std::uint8_t kApexInit[] = {APEX_POLL, APEX_POLL, APEX_INIT, APEX_INIT, APEX_INIT, 0};
const std::uint8_t kApexPoll[] = {APEX_POLL, 0};
const std::uint8_t kLifespanInit[] = {5, 0, 0x02, 0x00, 0x00, 0x00, 0x00, 0};
#define LS(x) 5, 0, 0xa1, x, 0x00, 0x00, 0x00
const std::uint8_t kLifespanPoll[] = {LS(0x91), LS(0x81), LS(0x8a), LS(0x86), LS(0x87), LS(0x85), LS(0x89), LS(0x82),
                                      LS(0x84), 0};
// clang-format on

const char kNusService[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const char kNusWrite[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
const char kNusNotify[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
const char kKettlerService[] = "638af000-7bde-3e25-ffc5-9de9b2a0197a";
const char kTechnogymService[] = "a913bfc0-929e-11e5-b928-0002a5d5c51b";
const char kCycleOpsService[] = "c0f4013a-a837-4165-bab9-654ef70747c6";

struct Row {
    const char* service;
    const char* notify;
    const char* write;
    const std::uint8_t* init;
    const std::uint8_t* poll;
    std::uint16_t pollMs;
    std::uint8_t pollSkip;  // every Nth poll tick is skipped (upstream sec1Update gap)
    std::uint16_t flags;
    std::uint8_t minLevel, maxLevel;
};

// Indexed by Model.
const Row kRows[] = {
    {kNusService, kNusNotify, kNusWrite, kFlywheelInit, nullptr, 0, 0, kZeroWatts, 0, 0},
    {kNusService, kNusNotify, kNusWrite, kIc5Init, nullptr, 0, 0, kZeroWatts, 0, 0},
    {"49535343-fe7d-4ae5-8fa9-9fafd205e455", "49535343-1e4d-4bd9-ba61-23c647249616",
     "49535343-8841-43f4-a8d4-ecbe34729bb3", kSoleInit, kSolePoll, 300, 4, kZeroWatts, 0, 0},
    {"49535343-fe7d-4ae5-8fa9-9fafd205e455", "49535343-1e4d-4bd9-ba61-23c647249616",
     "49535343-8841-43f4-a8d4-ecbe34729bb3", kR92Init, kR92Poll, 300, 4, kZeroWatts, 0, 0},
    {kKettlerService, nullptr, "638a100e-7bde-3e25-ffc5-9de9b2a0197a", kKettlerInit, nullptr, 0, 0,
     kNoResponse | kKettler | kCps | kCsc | kCpsPower | kCscWheel, 0, 0},
    {kTechnogymService, nullptr, "58094966-498c-470d-8051-37e617a13895", nullptr, nullptr, 0, 0,
     kCps | kCpsPower | kCpsWheel | kZeroWatts | kCtlResistance | kCtlPower, 1, 100},
    {"7ee7e1fa-6fd8-4cbd-b648-a2cdcfa4e7f4", "7ee72ad1-6fd8-4cbd-b648-a2cdcfa4e7f4", nullptr, nullptr, nullptr, 0,
     0, 0, 0, 0},
    {"00ff", "ff01", "ff02", kKeepInit, kKeepPoll, 1200, 0, 0, 0, 0},
    {kNusService, kNusNotify, kNusWrite, kMcfInit, kMcfPoll, 300, 0, kZeroWatts, 0, 0},
    {"fff0", "fff6", nullptr, nullptr, nullptr, 0, 0, kZeroWatts, 0, 0},
    {"a026ee07-0a7d-4ab3-97fa-f1500f9feb8b", "a026e01d-0a7d-4ab3-97fa-f1500f9feb8b", nullptr, nullptr, nullptr, 0,
     0, kZeroWatts, 0, 0},
    {"00010203-0405-0607-0809-0a0b0c0d1910", "00010203-0405-0607-0809-0a0b0c0d2b10",
     "00010203-0405-0607-0809-0a0b0c0d2b11", kVolavaInit, nullptr, 0, 0, kNoResponse | kZeroWatts, 0, 0},
    {"fff0", "fff6", "fff6", kUltrasportInit, kUltrasportPoll, 1200, 0, 0, 0, 0},
    {"fff0", "fff1", "fff2", kSkandikaInit, nullptr, 300, 4, kZeroWatts | kCtlResistance, 1, 32},
    {"fff0", "fff1", "fff2", kX2000Init, nullptr, 300, 4, kCtlResistance, 1, 32},
    {"fbb0", "fbb2", "fbb1", nullptr, kPitpatPoll, 1200, 0, kCtlResistance, 1, 32},
    {"fff0", "fff1", "fff2", nullptr, kFreebeatPoll, 200, 0, kNoResponse | kCtlResistance, 1, 100},
    {"fff0", "fff2", "fff1", kMepanelInit, nullptr, 0, 0, kZeroWatts, 0, 0},
    {"fff0", "fff1", "fff2", kApexInit, kApexPoll, 5000, 0, 0, 0, 0},
    {nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, kFtms | kCps | kCsc | kHr | kNus | kCpsPower | kCscWheel, 0,
     0},
    {kCycleOpsService, nullptr, "ca31a533-a858-4dc7-a650-fdeb6dad4c14", nullptr, nullptr, 0, 0,
     kCsc | kHr | kCps | kFtms | kCpsPower | kCpsWheel | kCtlPower, 0, 0},
    {nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, kCsc | kCps | kCscWheel, 0, 0},
    {"fff0", "fff1", "fff2", kLifespanInit, kLifespanPoll, 500, 0, 0, 0, 0},
};
static_assert(sizeof(kRows) / sizeof(kRows[0]) == static_cast<std::size_t>(Model::Count), "row per model");

const char* const kIds[] = {"flywheel", "lifefit-ic5", "sole-bike", "sole-r92", "kettler-c12", "technogym",
                            "inspire", "keep-bike", "mcf-bike", "xcx-bike", "chrono-bike", "volava", "ultrasport",
                            "skandika-wiri", "skandika-x2000", "pitpat-bike", "freebeat-boom", "mepanel",
                            "apex-bike", "npe-cable", "cycleops", "csc-bike", "lifespan-bike"};
static_assert(sizeof(kIds) / sizeof(kIds[0]) == static_cast<std::size_t>(Model::Count), "id per model");

const Row& row(Model model) { return kRows[static_cast<std::size_t>(model)]; }

std::string upper(std::string text) {
    for (auto& c : text)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return text;
}
bool starts(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }
std::uint16_t be16(const std::uint8_t* d) { return static_cast<std::uint16_t>((d[0] << 8) | d[1]); }
std::uint16_t le16(const std::uint8_t* d) { return static_cast<std::uint16_t>(d[0] | (d[1] << 8)); }
MetricSample value(float v) { return {v, true}; }

// Every rule mirrors bluetooth.cpp and leaves FTMS advertisers to the generic
// adapter unless upstream explicitly routes them (XCX).
bool noFtms(const Advertisement& a) { return !a.hasService(0x1826); }
bool matchFlywheel(const Advertisement& a) { return noFtms(a) && starts(a.name, "Flywheel"); }
bool matchSoleBike(const Advertisement& a) {
    const auto n = upper(a.name);
    return noFtms(a) && (starts(n, "LCB") || starts(n, "LCR"));
}
bool matchSoleR92(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "R92"); }
bool matchKettler(const Advertisement& a) {
    const auto n = upper(a.name);
    return noFtms(a) && (starts(n, "ERGO C12") || starts(n, "KETTLER C12"));
}
bool matchTechnogym(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "MYCYCLING"); }
bool matchInspire(const Advertisement& a) {
    const auto n = upper(a.name);
    return noFtms(a) && starts(n, "IC") && n.size() == 8 && !starts(n, "IC BIKE");
}
bool matchKeep(const Advertisement& a) {
    const auto n = upper(a.name);
    return noFtms(a) && (starts(n, "KEEP_BIKE_") || starts(n, "KEEP_CC_"));
}
bool matchMcf(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "MCF-"); }
bool matchXcx(const Advertisement& a) { return starts(upper(a.name), "XCX-"); }
bool matchChrono(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "CHRONO "); }
bool matchVolava(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "XQ") && a.name.size() == 12; }
bool matchUltrasport(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "X-BIKE"); }
bool matchSkandika(const Advertisement& a) {
    const auto n = upper(a.name);
    return noFtms(a) && (starts(n, "BFCP") || (starts(n, "HT") && n.size() == 12));
}
bool matchX2000(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "HT") && a.name.size() == 11; }
bool matchPitpat(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "PITPAT-S"); }
bool matchFreebeat(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "FEAQ51A_"); }
bool matchMepanel(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "MEPANEL"); }
bool matchApex(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "WLT8266BM"); }
bool matchNpe(const Advertisement& a) {
    const auto n = upper(a.name);
    return noFtms(a) && (starts(n, ">CABLE") || (starts(n, "MD") && n.size() == 7) || (starts(n, "BIKE") && n.size() == 6));
}
bool matchCycleOps(const Advertisement& a) {
    const auto n = upper(a.name);
    return noFtms(a) && (starts(n, "INDOORCYCLE") || starts(n, "MAGNUS "));
}
bool matchCscBike(const Advertisement& a) {
    const auto n = upper(a.name);
    return noFtms(a) && (starts(n, "JOROTO-BK-") || (starts(n, "BGYM") && n.size() == 8));
}
bool matchRenpho(const Advertisement& a) {
    const auto n = upper(a.name);
    return noFtms(a) && ((starts(n, "RQ") && n.size() == 5) || (starts(n, "R-Q") && n.size() > 6) || starts(n, "SCH130"));
}
bool matchHorizonGr7(const Advertisement& a) { return noFtms(a) && starts(upper(a.name), "JFIC"); }
bool manualOnly(const Advertisement&) { return false; }

template <Model M>
std::unique_ptr<SourceProtocol> make() {
    return std::unique_ptr<SourceProtocol>(new VendorBikeSource(M));
}
std::unique_ptr<SourceProtocol> makeRenpho() {
    FtmsSource::Options options;
    options.id = "renpho";
    options.bikeDataOnly = true;
    return std::unique_ptr<SourceProtocol>(new FtmsSource(options));
}
std::unique_ptr<SourceProtocol> makeHorizonGr7() {
    FtmsSource::Options options;
    options.id = "horizon-gr7";
    options.bikeDataOnly = true;
    return std::unique_ptr<SourceProtocol>(new FtmsSource(options));
}

constexpr auto kBike = MachineProfile::IndoorBike;
constexpr int P = kMatchPriority;

// Append only: the order is persisted through the registry.
const ProtocolInfo kDescriptors[] = {
    {"flywheel", "Flywheel / Life Fitness IC8 bike", P, matchFlywheel, make<Model::Flywheel>, false, false, kBike},
    {"lifefit-ic5", "Life Fitness IC5 bike (manual)", 0, manualOnly, make<Model::LifeFitnessIc5>, false, false, kBike},
    {"sole-bike", "Sole LCB/LCR bike", P, matchSoleBike, make<Model::SoleBike>, false, false, kBike},
    {"sole-r92", "Sole R92 bike", P, matchSoleR92, make<Model::SoleR92>, false, false, kBike},
    {"kettler-c12", "Kettler C12 ergometer", P, matchKettler, make<Model::KettlerC12>, false, false, kBike},
    {"technogym", "Technogym MyCycling / Bike", P, matchTechnogym, make<Model::Technogym>, true, false, kBike},
    {"inspire", "Inspire IC bike", P, matchInspire, make<Model::Inspire>, false, false, kBike},
    {"keep-bike", "Keep bike", P, matchKeep, make<Model::KeepBike>, false, false, kBike},
    {"mcf-bike", "MCF bike", P, matchMcf, make<Model::Mcf>, false, false, kBike},
    {"xcx-bike", "XCX bike", P, matchXcx, make<Model::Xcx>, false, false, kBike},
    {"chrono-bike", "Chrono bike", P, matchChrono, make<Model::Chrono>, false, false, kBike},
    {"volava", "Volava bike", P, matchVolava, make<Model::Volava>, false, false, kBike},
    {"ultrasport", "Ultrasport X-Bike", P, matchUltrasport, make<Model::Ultrasport>, false, false, kBike},
    {"skandika-wiri", "Skandika Wiri bike", P, matchSkandika, make<Model::SkandikaWiri>, true, false, kBike},
    {"skandika-x2000", "Skandika X-2000 bike", P, matchX2000, make<Model::SkandikaX2000>, true, false, kBike},
    {"pitpat-bike", "PitPat bike", P, matchPitpat, make<Model::PitPat>, true, false, kBike},
    {"freebeat-boom", "Freebeat Boom bike", P, matchFreebeat, make<Model::FreebeatBoom>, true, false, kBike},
    {"mepanel", "MEPANEL bike", P, matchMepanel, make<Model::Mepanel>, false, false, kBike},
    {"apex-bike", "Apex bike", P, matchApex, make<Model::Apex>, false, false, kBike},
    {"npe-cable", "NPE CABLE bike", P, matchNpe, make<Model::NpeCable>, false, false, kBike},
    {"cycleops", "CycleOps Phantom / Magnus", P, matchCycleOps, make<Model::CycleOps>, true, false, kBike},
    {"csc-bike", "Joroto / BGYM CSC bike", P, matchCscBike, make<Model::CscBike>, false, false, kBike},
    {"lifespan-bike", "LifeSpan bike (manual)", 0, manualOnly, make<Model::LifespanBike>, false, false, kBike},
    {"renpho", "Renpho bike (FTMS)", P, matchRenpho, makeRenpho, true, false, kBike},
    {"horizon-gr7", "Horizon GR7 bike (FTMS)", P, matchHorizonGr7, makeHorizonGr7, true, false, kBike},
};

// Apex wattage table: resistance 1..19 x rpm 10..150.
const std::uint16_t kApexTable[19][15] = {
    {12, 24, 36, 48, 61, 73, 85, 97, 109, 121, 133, 145, 157, 170, 182},
    {13, 27, 40, 53, 67, 80, 93, 107, 120, 133, 147, 160, 173, 187, 200},
    {15, 29, 44, 58, 73, 87, 102, 117, 131, 146, 160, 175, 189, 204, 219},
    {16, 32, 48, 64, 80, 95, 111, 127, 143, 159, 175, 191, 207, 223, 239},
    {17, 34, 51, 68, 85, 102, 118, 135, 152, 169, 186, 203, 220, 237, 254},
    {18, 37, 55, 74, 92, 110, 129, 147, 165, 184, 202, 221, 239, 257, 276},
    {19, 39, 58, 77, 97, 116, 136, 155, 174, 194, 213, 232, 252, 271, 291},
    {21, 42, 62, 83, 104, 125, 146, 166, 187, 208, 229, 250, 271, 291, 312},
    {22, 44, 66, 88, 110, 132, 154, 176, 198, 220, 242, 264, 286, 308, 330},
    {23, 46, 69, 92, 116, 139, 162, 185, 208, 231, 254, 277, 300, 324, 347},
    {24, 49, 73, 98, 122, 146, 171, 195, 219, 244, 268, 293, 317, 341, 366},
    {26, 51, 77, 102, 128, 153, 179, 204, 230, 255, 281, 307, 332, 358, 383},
    {27, 54, 80, 107, 134, 161, 188, 214, 241, 268, 295, 322, 348, 375, 402},
    {28, 56, 83, 111, 139, 167, 195, 222, 250, 278, 306, 334, 362, 389, 417},
    {29, 58, 87, 117, 146, 175, 204, 233, 262, 292, 321, 350, 379, 408, 437},
    {30, 61, 91, 121, 152, 182, 212, 242, 273, 303, 333, 364, 394, 424, 455},
    {32, 63, 95, 126, 158, 189, 221, 253, 284, 316, 347, 379, 410, 442, 473},
    {33, 66, 99, 132, 165, 198, 231, 264, 297, 330, 363, 396, 429, 462, 495},
    {34, 68, 102, 136, 171, 205, 239, 273, 307, 341, 375, 409, 443, 478, 512}};

// Walks an encoded frame list; returns the record at `index` or nullptr.
const std::uint8_t* record(const std::uint8_t* list, std::size_t index) {
    if (!list) return nullptr;
    for (std::size_t i = 0; *list; ++i) {
        if (i == index) return list;
        list += 2 + list[0];
    }
    return nullptr;
}
std::size_t recordCount(const std::uint8_t* list) {
    std::size_t n = 0;
    while (record(list, n)) ++n;
    return n;
}

}  // namespace

std::size_t descriptorCount() { return sizeof(kDescriptors) / sizeof(kDescriptors[0]); }
const ProtocolInfo& descriptorAt(std::size_t index) { return kDescriptors[index]; }
const char* modelId(Model model) { return kIds[static_cast<std::size_t>(model)]; }

Bytes skandikaPoll(std::uint8_t level) {
    if (level < 1) level = 1;
    return {0x20, 0x01, level, 0x00, static_cast<std::uint8_t>(0x21 + level)};
}
Bytes pitpatResistance(std::uint8_t level) {
    Bytes frame{0x6a, 0x06, 0x51, 0x82, 0x01, level, 0x00, 0x43};
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < 6; ++i) crc ^= frame[i];
    frame[6] = static_cast<std::uint8_t>(crc ^ 0x6a);
    return frame;
}
Bytes freebeatResistance(std::uint8_t level) {
    if (level < 1) level = 1;
    if (level > 100) level = 100;
    return {0x25, 0x03, level, static_cast<std::uint8_t>(0x03 + level), 0xa0};
}
Bytes technogymResistance(int level) {
    const auto v = static_cast<std::uint16_t>(level / 3);
    return {0xf9, 0x01, static_cast<std::uint8_t>(v & 0xff), static_cast<std::uint8_t>(v >> 8), 0xe4, 0x02, 0x18,
            0x01, 0x03, 0x19, 0x00};
}
Bytes technogymPower(int watts) {
    const auto v = static_cast<std::uint16_t>(watts);
    return {0xf8, 0x01, static_cast<std::uint8_t>(v & 0xff), static_cast<std::uint8_t>(v >> 8), 0xe4, 0x02, 0x18,
            0x01, 0x03, 0x19, 0x00};
}
Bytes cycleopsPower(int watts) {
    const auto v = static_cast<std::uint16_t>(watts);
    return {0x00, 0x10, 0x01, static_cast<std::uint8_t>(v & 0xff), static_cast<std::uint8_t>(v >> 8), 0, 0, 0, 0, 0};
}

float apexPower(int resistance, float cadence) {
    if (cadence <= 0 || resistance <= 0) return 0;
    const int res = resistance > 19 ? 19 : resistance;
    const auto* table = kApexTable[res - 1];
    const double rpm = cadence < 1 ? 1.0 : cadence;
    if (rpm <= 10.0) return static_cast<float>(static_cast<std::uint16_t>(table[0] * rpm / 10.0));
    if (rpm >= 150.0) return static_cast<float>(static_cast<std::uint16_t>(table[14] * rpm / 150.0));
    int lower = (static_cast<int>(rpm) - 1) / 10;
    if (lower > 13) lower = 13;
    const double lowRpm = (lower + 1) * 10.0;
    const double w = table[lower] + (rpm - lowRpm) / 10.0 * (table[lower + 1] - table[lower]);
    return w < 0 ? 0.0F : static_cast<float>(static_cast<std::uint16_t>(w));
}

float inspirePower(int resistance, float cadence) {
    if (cadence == 0) return 0;
    const double k = cadence < 90 ? 0.095 : 0.088;
    return static_cast<float>(static_cast<std::uint16_t>(3.59 * std::exp(0.0217 * cadence) * std::exp(k * resistance)));
}

int bcd(std::uint8_t value) {
    const int hi = value >> 4, lo = value & 0x0f;
    if (hi > 9 || lo > 9) return 0;
    return hi * 10 + lo;
}

void FlywheelDecoder::reset() {
    state_ = Sync;
    remaining_ = 0;
    size_ = 0;
}

bool FlywheelDecoder::feed(const std::uint8_t* data, std::size_t length) {
    bool complete = false;
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint8_t b = data[i];
        switch (state_) {
            case Sync:
                if (b == 0xff) {
                    size_ = 0;
                    state_ = Length;
                }
                break;
            case Length:
                if (b == 0) {
                    state_ = Sync;
                } else {
                    remaining_ = static_cast<std::int8_t>(b);
                    state_ = Id;
                }
                break;
            case Id:
                id_ = b;
                --remaining_;
                state_ = remaining_ > 1 ? Data : (remaining_ == 1 ? Checksum : End);
                break;
            case Data:
                if (size_ < sizeof(buffer_)) buffer_[size_++] = b;
                if (--remaining_ == 1) state_ = Checksum;
                break;
            case Checksum:
                state_ = End;
                break;
            case End:
                if (b == 0x55) complete = true;
                state_ = Sync;
                break;
        }
    }
    return complete;
}

}  // namespace vendor_bikes

using namespace vendor_bikes;

VendorBikeSource::VendorBikeSource(Model model) : model_(model) {}

const char* VendorBikeSource::id() const { return vendor_bikes::modelId(model_); }

std::size_t VendorBikeSource::buildPlan(ble::ConnectPlan& plan, Kind* kinds) const {
    const auto& r = row(model_);
    std::size_t n = 0;
    auto add = [&](const ble::Uuid& service, const ble::Uuid& characteristic, Kind kind) {
        if (plan.subscribe(service, characteristic)) kinds[n++] = kind;
    };
    auto u = [](const char* text) { return ble::Uuid::fromString(text); };
    if (r.notify) add(u(r.service), u(r.notify), Vendor);
    if (r.flags & vendor_bikes::kKettler) {
        const ble::Uuid s = u(r.service);
        add(s, u("638a1001-7bde-3e25-ffc5-9de9b2a0197a"), KResistance);
        add(s, u("638a1003-7bde-3e25-ffc5-9de9b2a0197a"), KPower);
        add(s, u("638a1004-7bde-3e25-ffc5-9de9b2a0197a"), KSpeed);
        add(s, u("638a1005-7bde-3e25-ffc5-9de9b2a0197a"), KResistance2);
        add(s, u("638a100a-7bde-3e25-ffc5-9de9b2a0197a"), KResistance2);
        add(s, u("638a1008-7bde-3e25-ffc5-9de9b2a0197a"), KDistance);
    }
    if (r.flags & vendor_bikes::kFtms) add(ble::Uuid::from16(0x1826), ble::Uuid::from16(ftms::kIndoorBikeDataUuid), FtmsBike);
    if (r.flags & vendor_bikes::kCps)
        add(ble::Uuid::from16(sensors::kCyclingPowerService), ble::Uuid::from16(sensors::kCyclingPowerMeasurement), Power);
    if (r.flags & vendor_bikes::kCsc)
        add(ble::Uuid::from16(sensors::kCscService), ble::Uuid::from16(sensors::kCscMeasurement), Csc);
    if (r.flags & vendor_bikes::kHr)
        add(ble::Uuid::from16(sensors::kHeartRateService), ble::Uuid::from16(sensors::kHeartRateMeasurement), HeartRate);
    if (r.flags & vendor_bikes::kNus) add(u(vendor_bikes::kNusService), u(vendor_bikes::kNusNotify), Nus);
    return n;
}

ble::ConnectPlan VendorBikeSource::plan() const {
    ble::ConnectPlan plan;
    Kind kinds[ble::kMaxSubscriptions];
    buildPlan(plan, kinds);
    return plan;
}

void VendorBikeSource::onAdvertisement(const ble::Advertisement& advertisement, std::uint32_t) {
    name_ = advertisement.name;
}

void VendorBikeSource::reset() {
    SourceProtocol::reset();
    stream_.reset();
    crank_.reset();
    wheel_.reset();
    cscCrank_.reset();
    cscWheel_.reset();
    step_ = pollIndex_ = 0;
    nextStepMs_ = lastPollMs_ = readyAtMs_ = lastCpsPowerMs_ = 0;
    pollTicks_ = 0;
    lastCommand_ = zeroFilter_ = 0;
    target_ = 1;
    resistance_ = 0;
    cadence_ = 0;
    connected_ = ready_ = legacy_ = alternateWrite_ = cpsPowerSeen_ = false;
    cadenceRate_ = nullptr;
    cadenceDivisor_ = 1.0F;
}

void VendorBikeSource::useRate(sensors::RevolutionRate& rate, float divisor) {
    cadenceRate_ = &rate;
    cadenceDivisor_ = divisor;
}

bool VendorBikeSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    ble::ConnectPlan plan;
    kindCount_ = buildPlan(plan, kinds_);
    const auto& r = row(model_);
    const bool usable = r.notify ? (subscribedMask & 1U) != 0 : subscribedMask != 0;
    if (!usable) {
        setError(std::string(id()) + ": notify characteristic unavailable");
        return false;
    }
    connected_ = true;
    step_ = 0;
    nextStepMs_ = nowMs;
    lastPollMs_ = nowMs;
    readyAtMs_ = nowMs + (model_ == Model::PitPat ? 1500U : 0U);
    if (!r.init && model_ != Model::PitPat) ready_ = true;
    tick(nowMs);
    return true;
}

bool VendorBikeSource::send(const std::uint8_t* data, std::size_t length) {
    const auto& r = row(model_);
    if (!r.write || !connected_) return false;
    const char* characteristic = alternateWrite_ ? r.notify : r.write;
    return queueWrite(ble::Uuid::fromString(r.service), ble::Uuid::fromString(characteristic),
                      Bytes(data, data + length), (r.flags & vendor_bikes::kNoResponse) == 0);
}

void VendorBikeSource::onWriteFailed(std::uint32_t nowMs) {
    // keepbike falls back to writing the notify characteristic (FF01).
    if (model_ == Model::KeepBike && !alternateWrite_) {
        alternateWrite_ = true;
        return;
    }
    SourceProtocol::onWriteFailed(nowMs);
}

void VendorBikeSource::sendPoll(std::uint32_t nowMs) {
    const auto& r = row(model_);
    lastPollMs_ = nowMs;
    ++pollTicks_;
    if (r.pollSkip && pollTicks_ % r.pollSkip == 0) return;
    if (model_ == Model::SkandikaWiri || model_ == Model::SkandikaX2000) {
        send(vendor_bikes::skandikaPoll(target_));
        return;
    }
    const auto count = vendor_bikes::recordCount(r.poll);
    if (!count) return;
    const auto* rec = vendor_bikes::record(r.poll, pollIndex_ % count);
    pollIndex_ = (pollIndex_ + 1) % count;
    if (model_ == Model::LifespanBike) lastCommand_ = rec[3];
    send(rec + 2, rec[0]);
}

void VendorBikeSource::tick(std::uint32_t nowMs) {
    if (!connected_) return;
    const auto& r = row(model_);
    if (r.init && !ready_) {
        while (static_cast<std::int32_t>(nowMs - nextStepMs_) >= 0) {
            const auto* rec = vendor_bikes::record(r.init, step_);
            if (!rec) {
                ready_ = true;
                lastPollMs_ = nowMs;
                break;
            }
            send(rec + 2, rec[0]);
            ++step_;
            nextStepMs_ = nowMs + rec[1] * 10U;
        }
        if (!ready_) return;
    }
    if (model_ == Model::PitPat && !ready_ && !legacy_ && static_cast<std::int32_t>(nowMs - readyAtMs_) >= 0) {
        ready_ = true;
        lastPollMs_ = nowMs;
    }
    if (ready_ && r.pollMs && nowMs - lastPollMs_ >= r.pollMs) sendPoll(nowMs);
    // Decay revolution-based cadence when the sensor goes quiet.
    if (cadenceRate_ && sample_.cadence.valid) sample_.cadence.value = cadenceRate_->current(nowMs) / cadenceDivisor_;
}

void VendorBikeSource::setPowerCadence(float power, float cadence) {
    const auto& r = row(model_);
    sample_.cadence = value(cadence);
    cadence_ = cadence;
    if ((r.flags & vendor_bikes::kZeroWatts) && cadence == 0) power = 0;
    sample_.power = value(power);
}

void VendorBikeSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (!data || index >= kindCount_) return;
    if (kinds_[index] == Vendor) {
        decodeVendor(data, length, nowMs);
    } else {
        decodeStandard(kinds_[index], data, length, nowMs);
    }
}

void VendorBikeSource::decodeStandard(Kind kind, const std::uint8_t* d, std::size_t n, std::uint32_t nowMs) {
    const auto flags = row(model_).flags;
    switch (kind) {
        case FtmsBike: {
            ftms::MachineData data;
            if (!ftms::decodeIndoorBikeData(d, n, data)) return;
            for (std::size_t m = 0; m < kMetricCount; ++m)
                if (data[static_cast<Metric>(m)].valid) sample_[static_cast<Metric>(m)] = data[static_cast<Metric>(m)];
            break;
        }
        case Power: {
            sensors::CyclingPowerMeasurement m;
            if (!sensors::decodeCyclingPower(d, n, m)) return;
            if (flags & vendor_bikes::kCpsPower) {
                sample_.power = value(m.power < 0 ? 0.0F : static_cast<float>(m.power));
                lastCpsPowerMs_ = nowMs;
                cpsPowerSeen_ = true;
            }
            if (m.crank.present) {
                sample_.cadence = value(crank_.update(m.crank, nowMs));
                useRate(crank_, 1.0F);
            } else if (m.wheel.present && (flags & vendor_bikes::kCpsWheel)) {
                sample_.cadence = value(wheel_.update(m.wheel, nowMs) / 2.0F);
                useRate(wheel_, 2.0F);
            }
            if ((flags & vendor_bikes::kZeroWatts) && sample_.cadence.valid && sample_.cadence.value == 0)
                sample_.power = value(0);
            if (!(flags & vendor_bikes::kCpsPower) && !sample_.cadence.valid) return;
            break;
        }
        case Csc: {
            sensors::CscMeasurement m;
            if (!sensors::decodeCsc(d, n, m)) return;
            if (m.crank.present && (m.crank.revolutions != 0 || !(m.wheel.present && (flags & vendor_bikes::kCscWheel)))) {
                sample_.cadence = value(cscCrank_.update(m.crank, nowMs));
                useRate(cscCrank_, 1.0F);
            } else if (m.wheel.present && (flags & vendor_bikes::kCscWheel)) {
                sample_.cadence = value(cscWheel_.update(m.wheel, nowMs));
                useRate(cscWheel_, 1.0F);
            } else {
                return;
            }
            break;
        }
        case HeartRate: {
            std::uint16_t bpm = 0;
            if (!sensors::decodeHeartRate(d, n, bpm)) return;
            sample_.heartRate = value(bpm);
            break;
        }
        case Nus:
            if (!starts(upper(name_), "BIKE ") || n != 20 || d[0] != 0xff || d[1] != 0x1f) return;
            sample_.resistance = value(d[15]);
            break;
        case KResistance:
        case KResistance2:
        case KPower:
        case KSpeed:
        case KDistance: {
            if (n < 2) return;
            const auto v = le16(d);
            if (kind == KDistance) {
                if (n < 4) return;
                sample_.distance = value(static_cast<float>(v | (static_cast<std::uint32_t>(le16(d + 2)) << 16)));
            } else if (kind == KPower) {
                if (cpsPowerSeen_ && nowMs - lastCpsPowerMs_ < 3000) return;
                sample_.power = value(v);
            } else if (kind == KSpeed) {
                sample_.speed = value(v / 10.0F);
            } else if (kind == KResistance || v <= 250) {
                sample_.resistance = value(v);
            } else {
                return;
            }
            break;
        }
        default:
            return;
    }
    touch(nowMs);
}

void VendorBikeSource::decodeVendor(const std::uint8_t* d, std::size_t n, std::uint32_t nowMs) {
    switch (model_) {
        case Model::Flywheel:
        case Model::LifeFitnessIc5: {
            if (!stream_.feed(d, n) || stream_.id() != 12 || stream_.size() < 13) return;
            const auto* p = stream_.data();
            const float power = be16(p), speed = be16(p + 10) / 10.0F, cadence = p[9];
            if (zeroFilter_ < 2 && (cadence == 0 || speed == 0 || power == 0)) {
                ++zeroFilter_;
                return;
            }
            zeroFilter_ = 0;
            sample_.resistance = value(p[12]);
            sample_.speed = value(speed);
            setPowerCadence(power, cadence);
            break;
        }
        case Model::SoleBike:
        case Model::SoleR92:
            if (n == 5 && d[0] == 0x5b && d[1] == 0x02 && d[2] == 0x13) {
                sample_.resistance = value(d[3] > 0 ? d[3] : 1);
                break;
            }
            if (n < 20 || d[1] != 0x13) return;
            sample_.speed = value(be16(d + 11) / 100.0F * 1.60934F);
            setPowerCadence(be16(d + 13), d[10]);
            break;
        case Model::Inspire:
            if (n != 8) return;
            resistance_ = d[6];
            sample_.resistance = value(d[6]);
            sample_.speed = value(0.37497622F * d[3]);
            setPowerCadence(vendor_bikes::inspirePower(d[6], d[3]), d[3]);
            break;
        case Model::KeepBike:
            if (n != 20 || (d[0] == 0xa5 && d[1] == 0xa5)) return;
            sample_.resistance = value(d[14]);
            setPowerCadence(be16(d + 17), d[16]);
            break;
        case Model::Mcf:
            if (n != 20 || d[1] != 0xe5) return;
            sample_.resistance = value(d[13]);
            sample_.speed = value(be16(d + 11) / 10.0F);
            setPowerCadence(be16(d + 9), d[15]);
            break;
        case Model::Xcx:
            if ((n != 20 && n != 23) || d[0] != 0xfa || d[1] != 0x05) return;
            sample_.speed = value(le16(d + 6) / 10.0F);
            sample_.distance = value(static_cast<float>(d[8] | (d[9] << 8) | (d[10] << 16)) * 100.0F);
            sample_.totalEnergy = value(le16(d + 11));
            sample_.resistance = value(le16(d + 18));
            setPowerCadence(d[17], le16(d + 13));
            break;
        case Model::Chrono:
            if (n != 19) return;
            sample_.speed = value(le16(d + 6) / 100.0F);
            setPowerCadence(le16(d + 17), static_cast<float>(d[8] / 2));
            break;
        case Model::Volava:
            if (n < 16 || d[0] != 0xaa || d[2] != 0x00 || d[3] != 0x80) return;
            sample_.resistance = value(d[14]);
            sample_.speed = value(d[11] * 0.38F);
            setPowerCadence(be16(d + 6), d[11]);
            break;
        case Model::Ultrasport:
            if (n != 18) return;
            sample_.power = value(be16(d + 5));
            if (d[9]) sample_.heartRate = value(d[9]);
            break;
        case Model::SkandikaWiri:
        case Model::SkandikaX2000: {
            const bool x2000 = model_ == Model::SkandikaX2000;
            if (n == (x2000 ? 6U : 5U)) {
                if (d[2] >= 33) return;
                sample_.resistance = value(d[2]);
                break;
            }
            if (n != 12) return;
            if (d[1] == (x2000 ? 0x20 : 0x00)) {
                sample_.speed = value((vendor_bikes::bcd(d[2]) * 100 + vendor_bikes::bcd(d[3])) / 10.0F);
            } else if (d[1] == (x2000 ? 0x30 : 0x10)) {
                cadence_ = static_cast<float>(vendor_bikes::bcd(d[2]) * 100 + vendor_bikes::bcd(d[3]));
                sample_.cadence = value(cadence_);
            }
            sample_.totalEnergy = value(static_cast<float>(vendor_bikes::bcd(d[6]) * 100 + vendor_bikes::bcd(d[7])));
            sample_.heartRate = value(d[8]);
            if (!x2000)
                setPowerCadence(static_cast<float>(vendor_bikes::bcd(d[9]) * 100 + vendor_bikes::bcd(d[10])), cadence_);
            break;
        }
        case Model::PitPat:
            if (n >= 4 && d[0] == 0x6a && d[3] == 0x2c) {
                legacy_ = true;
                if (n >= 34 && !ready_) {
                    const std::uint8_t handshake[] = {0x6a, 0x05, 0x50, 0x1b, 0x01, 0x4f, 0x43};
                    send(handshake, sizeof(handshake));
                }
                return;
            }
            if (legacy_) {
                static const std::uint8_t kAck[] = {0x6a, 0x06, 0xb0, 0x1b, 0x01, 0x00, 0xac, 0x43};
                if (n == sizeof(kAck) && std::equal(d, d + n, kAck)) {
                    ready_ = true;
                    lastPollMs_ = nowMs;
                    return;
                }
                if (n < 28 || d[0] != 0x6a || d[3] != 0x02) return;
                if (!ready_) {
                    ready_ = true;
                    lastPollMs_ = nowMs;
                }
                const float cadence = d[25];
                float level = d[5] < 1 ? 1.0F : (d[5] > 32 ? 32.0F : d[5]);
                sample_.resistance = value(d[5]);
                sample_.speed = value(be16(d + 23) / 1000.0F);
                sample_.distance = value(be16(d + 10));
                sample_.totalEnergy = value(be16(d + 12) / 10.0F);
                sample_.heartRate = value(d[18]);
                setPowerCadence(cadence > 0 ? std::floor(cadence * 4.0F * (0.10F + 0.90F * (level - 1.0F) / 31.0F) + 0.5F)
                                            : 0.0F,
                                cadence);
                break;
            }
            if (n != 30) return;
            sample_.resistance = value(d[5]);
            sample_.speed = value(0.37497622F * d[25]);
            setPowerCadence(be16(d + 23), d[25]);
            break;
        case Model::FreebeatBoom: {
            if (n < 13 || d[0] != 0x55 || d[12] != 0x3f || (d[1] != 0x15 && d[1] != 0x03 && d[1] != 0x25)) return;
            const float rpm = le16(d + 8);
            sample_.resistance = value(d[7]);
            sample_.speed = value(static_cast<float>(rpm * 3.14159265358979 * 0.129 * 2.0 * 7.39 * 3.6 * 0.62137 / 60.0));
            setPowerCadence(rpm * 4.56F * d[7] / 100.0F, rpm);
            break;
        }
        case Model::Mepanel:
            if (n < 3) return;
            switch (d[0]) {
                case 30: sample_.speed = value(be16(d + 1) / 100.0F); break;
                case 45:
                    cadence_ = be16(d + 1);
                    sample_.cadence = value(cadence_);
                    if (cadence_ == 0 && sample_.power.valid) sample_.power = value(0);
                    break;
                case 105: setPowerCadence(be16(d + 1) / 10.0F, cadence_); break;
                case 120: sample_.heartRate = value(d[1]); break;
                default: return;
            }
            break;
        case Model::Apex: {
            if (n != 10 || d[2] != 0x31) return;
            const int res = 33 - d[5];
            const float cadence = d[4] * 2.0F;
            sample_.resistance = value(static_cast<float>(res));
            sample_.speed = value(0.37497622F * cadence);
            setPowerCadence(vendor_bikes::apexPower(res, cadence), cadence);
            break;
        }
        case Model::LifespanBike: {
            const auto command = lastCommand_;
            lastCommand_ = 0;
            if (command == 0x91 && n >= 3) {
                resistance_ = d[2];
                sample_.resistance = value(d[2]);
            } else if (command == 0x8a && n >= 4) {
                cadence_ = d[3];
                sample_.cadence = value(cadence_);
            } else if (command == 0x87 && n >= 4) {
                sample_.totalEnergy = value(be16(d + 2));
            } else if (command == 0x85 && n >= 4) {
                sample_.distance = value(be16(d + 2) * 100.0F);
            } else if (command == 0x82 && n >= 4) {
                sample_.speed = value(d[2] + d[3] / 100.0F);
            } else if (command == 0x84 && n >= 4) {
                const float raw = be16(d + 2);
                sample_.power = value(raw > 0 && resistance_ > 0 && cadence_ > 0
                                          ? std::round(raw * resistance_ * cadence_ / 60.0F)
                                          : raw);
            } else {
                return;
            }
            break;
        }
        default:
            return;
    }
    touch(nowMs);
}

bool VendorBikeSource::control(const ControlRequest& request, std::uint32_t) {
    const auto& r = row(model_);
    if (!ready_) return false;
    if (request.control == Control::Resistance && (r.flags & vendor_bikes::kCtlResistance)) {
        const int level = percentToLevel(request.value, r.minLevel, r.maxLevel);
        switch (model_) {
            case Model::FreebeatBoom: return send(vendor_bikes::freebeatResistance(static_cast<std::uint8_t>(level)));
            case Model::PitPat: return send(vendor_bikes::pitpatResistance(static_cast<std::uint8_t>(level)));
            case Model::Technogym: return send(vendor_bikes::technogymResistance(level));
            case Model::SkandikaWiri:
            case Model::SkandikaX2000:
                target_ = static_cast<std::uint8_t>(level);
                return send(vendor_bikes::skandikaPoll(target_));
            default: return false;
        }
    }
    if (request.control == Control::Power && (r.flags & vendor_bikes::kCtlPower)) {
        float watts = request.value;
        if (!std::isfinite(watts) || watts < 0) watts = 0;
        if (watts > 4000) watts = 4000;
        const int w = static_cast<int>(std::lround(watts));
        return send(model_ == Model::Technogym ? vendor_bikes::technogymPower(w) : vendor_bikes::cycleopsPower(w));
    }
    return false;
}

TrainerCapabilities VendorBikeSource::capabilities() const {
    const auto& r = row(model_);
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    capabilities.setControl(Control::Resistance, ready_ && (r.flags & vendor_bikes::kCtlResistance));
    capabilities.setControl(Control::Power, ready_ && (r.flags & vendor_bikes::kCtlPower));
    return capabilities;
}

std::unique_ptr<SourceProtocol> createVendorBike(Model model) {
    return std::unique_ptr<SourceProtocol>(new VendorBikeSource(model));
}

}  // namespace protocols
}  // namespace smartfitness
