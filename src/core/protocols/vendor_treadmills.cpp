// Vendor treadmill BLE protocols. GPL-3.0 port of qdomyos-zwift, Copyright
// Roberto Viola and contributors, revision
// ea70d7ff64bc8bf508fc6401fd056211e05311b3 (see vendor_treadmills.hpp for the
// list of upstream files). Part of smartfitness-bridge, GPL-3.0-or-later.

#include "protocols/vendor_treadmills.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include "ftms/ftms_codec.hpp"

namespace smartfitness {
namespace protocols {
namespace vendor_treadmills {
namespace {

using ble::Uuid;

std::string upper(const std::string& text) {
    std::string out = text;
    for (auto& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
bool starts(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

// Name followed by at least three trailing digits (upstream regex "\d{3,}$").
bool numberedName(const std::string& name, const char* prefix) {
    if (!starts(name, prefix)) return false;
    const std::size_t from = std::strlen(prefix);
    if (name.size() < from + 3) return false;
    for (std::size_t i = from; i < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

Bytes hex(const char* text) {
    Bytes out;
    auto nibble = [](char c) -> std::uint8_t {
        return static_cast<std::uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    };
    int pending = -1;
    for (; *text; ++text) {
        if (*text == ' ') continue;
        if (pending < 0) {
            pending = nibble(*text);
        } else {
            out.push_back(static_cast<std::uint8_t>(pending << 4 | nibble(*text)));
            pending = -1;
        }
    }
    return out;
}

// Upstream truncates double km/h * scale; floats need a tiny bias so that
// e.g. 0.7F * 10 encodes as 7 like upstream's 0.7 * 10.
int units(float value, double scale) { return static_cast<int>(std::floor(static_cast<double>(value) * scale + 1e-4)); }

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

enum Target : std::uint8_t { kMain, kWrite2, kFtmsCp, kUnlock };
enum Wait : std::uint8_t { kNone, kHandshake, kPair };

struct Step {
    const char* frame;  // nullptr: delay only
    std::uint16_t delayMs;
    std::uint8_t target;
    std::uint8_t wait;
};

constexpr std::uint16_t kGap = 300;

const Step kLifespanInit[] = {{"e9ff000000", kGap, kMain, kNone}, {"e400f40000", kGap, kMain, kNone}};
const Step kOctaneInit[] = {{"a504210501d9", 400, kMain, kNone}, {"a5042170016e", 400, kMain, kNone}};
const Step kZiproInit[] = {
    {"fb05a00001a6fc", 400, kMain, kNone},   {"fb05a00101a7fc", 400, kMain, kNone},
    {"fb05a00201a8fc", 400, kMain, kNone},   {"fb05a00301a9fc", 400, kMain, kNone},
    {"fb05a10001a7fc", 400, kMain, kNone},   {"fb06a1000000a7fc", 400, kMain, kNone},
    {"fb05a10001a7fc", 400, kMain, kNone}};
// Upstream init9 (a9 a3 01 01 0a) is the start-tape frame and is omitted.
const Step kEsangInit[] = {
    {"a9f2012f75", kGap, kMain, kNone},     {"a90a01c664", kGap, kMain, kNone},
    {"a9ae01fef8", kGap, kMain, kNone},     {"a9a0030000000a", kGap, kMain, kNone},
    {nullptr, 3000, kMain, kHandshake},     {nullptr, 2000, kMain, kNone},
    {nullptr, 3000, kMain, kPair},          {nullptr, 1000, kMain, kNone},
    {"a91e01fe48", kGap, kMain, kNone},     {"a9ae01fef8", 2000, kMain, kNone},
    {"a98e01092f", kGap, kMain, kNone},     {"a9b201fee4", 3000, kMain, kNone},
    {"a98e01092f", kGap, kMain, kNone},     {"a9ae01fef8", kGap, kMain, kNone}};
const Step kCostawayInit[] = {
    {"a9f2012f75", kGap, kMain, kNone},     {"a9080179d9", kGap, kMain, kNone},
    {"a9080405040401a1", kGap, kMain, kNone}, {"a91e01fe48", kGap, kMain, kNone},
    {"a9a0030000000a", kGap, kMain, kNone}, {"a90a0148ea", kGap, kMain, kNone},
    {"a9ae01fef8", kGap, kMain, kNone},     {"a9a0030000000a", kGap, kMain, kNone}};
const Step kSperaxInit[] = {{"f507000126d8fa", kGap, kMain, kNone},
                            {"f507000126d8fa", kGap, kMain, kNone},
                            {"f50900130100 89b8fa", kGap, kMain, kNone}};
#define F_I3 "f0c303000000b6"
#define F_I4 "f0c50a00000000000000000000bf"
#define F_I5 "f0c40500 3a01af4df0"
#define F_I6 "f0c60101b8"
const Step kFocusInit[] = {
    {"f0c000b0", 400, kMain, kNone}, {"f0c1020000b3", 400, kMain, kNone}, {F_I3, 400, kMain, kNone},
    {F_I3, 400, kMain, kNone},       {F_I3, 400, kMain, kNone},            {F_I3, 400, kMain, kNone},
    {F_I4, 400, kMain, kNone},       {F_I5, 400, kMain, kNone},            {F_I3, 400, kMain, kNone},
    {F_I3, 400, kMain, kNone},       {F_I6, 400, kMain, kNone},            {F_I5, 400, kMain, kNone},
    {F_I3, 400, kMain, kNone},       {F_I4, 400, kMain, kNone},            {F_I6, 400, kMain, kNone}};
// The upstream "pitpat start" frame is omitted.
const Step kPitPatInit[] = {{"6a05fdf843", kGap, kMain, kNone},
                            {"6b059d9843", kGap, kUnlock, kNone},
                            {"6a05d7d243", kGap, kMain, kNone}};
const Step kActivioInit[] = {{"88", kGap, kWrite2, kNone}};
const Step kTechnogymInit[] = {{"0093f051e81b42928e", kGap, kFtmsCp, kNone}};
const Step kKickrInit[] = {{"01", 100, kMain, kNone}, {"10", 100, kMain, kNone}, {"12", 100, kMain, kNone},
                           {"18", 100, kMain, kNone}, {"80", 100, kMain, kNone}, {"86", 100, kMain, kNone},
                           {"f0", 100, kMain, kNone}};

enum Flags : std::uint8_t { kAuto = 1, kSpeed = 2, kIncline = 4 };
enum Gatt : std::uint8_t { kGattTrue, kGattFtms, kGattFff0, kGattOctane, kGattPitPat, kGattActivio, kGattDiscover };

struct Spec {
    const char* id;
    const char* label;
    std::uint8_t flags;
    std::uint8_t gatt;
    const Step* script;
    std::uint8_t scriptLength;
    std::uint16_t pollMs;
    float minSpeed, maxSpeed, minIncline, maxIncline;
};

#define SCRIPT(s) s, static_cast<std::uint8_t>(sizeof(s) / sizeof(s[0]))
const Spec kSpecs[] = {
    {"true-tm", "TRUE treadmill", kAuto, kGattTrue, nullptr, 0, 0, 0, 0, 0, 0},
    {"assault-tm", "Assault treadmill", kAuto, kGattTrue, nullptr, 0, 0, 0, 0, 0, 0},
    {"wdway-tm", "WDWAY treadmill", kAuto, kGattTrue, nullptr, 0, 0, 0, 0, 0, 0},
    {"technogym-myrun", "Technogym MyRun (BLE)", kAuto | kSpeed | kIncline, kGattFtms, SCRIPT(kTechnogymInit), 0,
     0.1F, 22, 0, 15},
    {"lifespan-tm", "LifeSpan treadmill", kAuto, kGattFff0, SCRIPT(kLifespanInit), 500, 0, 0, 0, 0},
    {"octane-zr7", "Octane Zero Runner ZR7", kAuto, kGattOctane, SCRIPT(kOctaneInit), 0, 0, 0, 0, 0},
    {"octane-zr8", "Octane Zero Runner ZR8", kAuto, kGattOctane, SCRIPT(kOctaneInit), 0, 0, 0, 0, 0},
    {"zipro-tm", "ZiPro treadmill", kAuto | kSpeed | kIncline, kGattFff0, SCRIPT(kZiproInit), 500, 0.1F, 22, 0, 15},
    {"esanglinker", "ESangLinker treadmill", kAuto | kSpeed, kGattFff0, SCRIPT(kEsangInit), 0, 0.1F, 22, 0, 0},
    {"eslinker-costa", "ESLinker Costaway (manual)", kSpeed, kGattFff0, SCRIPT(kCostawayInit), 0, 0.1F, 22, 0, 0},
    {"sperax-tm", "Sperax walking pad", kAuto | kSpeed, kGattFff0, SCRIPT(kSperaxInit), 200, 0.1F, 6, 0, 0},
    {"focus-tm", "Focus treadmill", kAuto | kSpeed | kIncline, kGattFff0, SCRIPT(kFocusInit), 500, 0.1F, 22, 0, 15},
    {"deerrun-pitpat", "DeerRun PitPat", kAuto | kSpeed | kIncline, kGattPitPat, SCRIPT(kPitPatInit), 200, 0.1F, 22,
     0, 15},
    {"activio-tm", "Activio / RunnerT", kAuto | kSpeed | kIncline, kGattActivio, SCRIPT(kActivioInit), 0, 0.1F, 22,
     0, 15},
    {"activio-v460", "Activio: FitFiu MC-V460 (manual)", kSpeed, kGattActivio, SCRIPT(kActivioInit), 0, 0.1F, 22, 0,
     0},
    {"activio-zt2500", "Activio: Zero ZT2500 (manual)", kSpeed | kIncline, kGattActivio, SCRIPT(kActivioInit), 0,
     0.1F, 22, 0, 15},
    {"activio-umay", "Activio: Umay S100 (manual)", kSpeed | kIncline, kGattActivio, SCRIPT(kActivioInit), 0, 0.1F,
     22, 0, 15},
    {"kickr-run", "Wahoo KICKR RUN", kAuto | kSpeed | kIncline, kGattDiscover, SCRIPT(kKickrInit), 0, 0.1F, 22, -3,
     15},
};
#undef SCRIPT
static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == static_cast<std::size_t>(Model::Count), "spec table");

const Spec& spec(Model model) { return kSpecs[static_cast<std::size_t>(model)]; }

const char* const kSperaxSpeed[60] = {
    "f50b001502010056f000fa", "f50a0015010200be98fa",   "f50a00150103002b71fa",   "f50a00150104005e61fa",
    "f50a0015010500cb88fa",   "f50b00150106003bf004fa", "f50a0015010700ae1dfa",   "f50a0015010800d1d4fa",
    "f50a0015010900443dfa",   "f50a0015010a00b441fa",   "f50a0015010b0021a8fa",   "f50a0015010c0054b8fa",
    "f50a0015010d00c151fa",   "f50a0015010e00312dfa",   "f50a0015010f00a4c4fa",   "f50b001501100080f009fa",
    "f50a00150111001510fa",   "f50a0015011200e56cfa",   "f50a00150113007085fa",   "f50a00150114000595fa",
    "f50a0015011500907cfa",   "f50a00150116006000fa",   "f50b0015011700f005e9fa", "f50a00150118008a20fa",
    "f50a00150119001fc9fa",   "f50a0015011a00efb5fa",   "f50a0015011b007a5cfa",   "f50a0015011c000f4cfa",
    "f50a0015011d009aa5fa",   "f50a0015011e006ad9fa",   "f50b0015011f00f00f30fa", "f50a001501200022a3fa",
    "f50a0015012100b74afa",   "f50a00150122004736fa",   "f50a0015012300d2dffa",   "f50a0015012400a7cffa",
    "f50a00150125003226fa",   "f50a0015012600c25afa",   "f50a001501270057b3fa",   "f50a0015012800287afa",
    "f50a0015012900bd93fa",   "f50a0015012a004deffa",   "f50a0015012b00d806fa",   "f50a0015012c00ad16fa",
    "f50b0015012d0038f00ffa", "f50a0015012e00c883fa",   "f50a0015012f005d6afa",   "f50a00150130007957fa",
    "f50a0015013100ecbefa",   "f50a00150132001cc2fa",   "f50a0015013300892bfa",   "f50b0015013400f00c3bfa",
    "f50a001501350069d2fa",   "f50a001501360099aefa",   "f50a00150137000c47fa",   "f50a0015013800738efa",
    "f50a0015013900e667fa",   "f50a0015013a00161bfa",   "f50b0015013b0083f002fa", "f50b0015013c00f006e2fa"};

Uuid uuid16(std::uint16_t value) { return Uuid::from16(value); }

const Uuid kTrueService = Uuid::fromString("a026ee07-0a7d-4ab3-97fa-f1500f9feb8b");
const Uuid kTrueNotify = Uuid::fromString("a026e01d-0a7d-4ab3-97fa-f1500f9feb8b");
const Uuid kOctaneService = Uuid::fromString("96dc867d-7a83-4c22-b6be-6381d727aeda");
const Uuid kOctaneNotify = Uuid::fromString("96dc867e-7a83-4c22-b6be-6381d727aeda");
const Uuid kOctaneWrite = Uuid::fromString("96dc867f-7a83-4c22-b6be-6381d727aeda");
const Uuid kActivioService = Uuid::fromString("e54eaa50-371b-476c-99a3-74d267e3edae");
const Uuid kActivioWrite2 = Uuid::fromString("e54eaa55-371b-476c-99a3-74d267e3edae");
const Uuid kActivioNotify = Uuid::fromString("e54eaa56-371b-476c-99a3-74d267e3edae");
const Uuid kActivioWrite = Uuid::fromString("e54eaa57-371b-476c-99a3-74d267e3edae");
const Uuid kKickrCommand = Uuid::fromString("a026e03e-0a7d-4ab3-97fa-f1500f9feb8b");

// Activio low-nibble substitution: decoded value and whether the high
// nibble is decremented (activiotreadmill.cpp).
const std::uint8_t kActivioNibble[16] = {3, 8, 9, 6, 7, 0xC, 0xD, 0xA, 0xB, 0, 1, 0xE, 0xF, 4, 5, 2};
constexpr std::uint16_t kActivioNoBorrow = (1U << 0x9) | (1U << 0xA) | (1U << 0xF) | (1U << 0xD) | (1U << 0xE);
const std::uint8_t kActivioInclineRaw[16] = {0x49, 0x4A, 0x4F, 0x50, 0x4D, 0x4E, 0x53, 0x54,
                                             0x51, 0x52, 0x57, 0x58, 0x55, 0x56, 0x5B, 0x5C};

template <Model M>
bool modelMatch(const ble::Advertisement& a) { return matchesModel(M, a); }
template <Model M>
std::unique_ptr<SourceProtocol> modelFactory() { return std::unique_ptr<SourceProtocol>(new VendorTreadmillSource(M)); }

bool lifeFitnessMatch(const ble::Advertisement& a) { return matchesLifeFitness(a); }
bool shuaA5Match(const ble::Advertisement& a) { return matchesShuaA5(a); }
std::unique_ptr<SourceProtocol> lifeFitnessFactory() {
    FtmsSource::Options options;
    options.id = "lifefitness-tm";
    options.allowControl = false;
    return std::unique_ptr<SourceProtocol>(new VendorFtmsTreadmillSource(options));
}
std::unique_ptr<SourceProtocol> shuaA5Factory() {
    FtmsSource::Options options;
    options.id = "shuaa5-tm";
    options.allowControl = true;
    return std::unique_ptr<SourceProtocol>(new VendorFtmsTreadmillSource(options));
}

}  // namespace

bool matchesTrue(const ble::Advertisement& a) {
    if (a.hasService(0x1826)) return false;
    const std::string name = upper(a.name);
    if (starts(name, "TRUE"))
        return !numberedName(name, "TRUE TREADMILL ") && !numberedName(name, "TRUE ELLIPTICAL ") &&
               !starts(name, "TRUE 1000");
    // Generic "TREADMILL" names only without RSC/FTMS (gem-module and Flow
    // Fitness settings default off upstream).
    return starts(name, "TREADMILL") && !a.hasService(0x1814);
}
bool matchesAssault(const ble::Advertisement& a) {
    return !a.hasService(0x1826) && starts(upper(a.name), "ASSAULT TREADMILL ");
}
bool matchesWdway(const ble::Advertisement& a) {
    return !a.hasService(0x1826) && a.name.size() == 8 && starts(upper(a.name), "WDWAY");
}
bool matchesLifeFitness(const ble::Advertisement& a) {
    return starts(upper(a.name), "LF") && (a.name.size() == 15 || a.name.size() == 18);
}
bool matchesShuaA5(const ble::Advertisement& a) { return !a.hasService(0x1826) && starts(upper(a.name), "ZW-"); }

bool matchesModel(Model model, const ble::Advertisement& a) {
    const std::string name = upper(a.name);
    const bool ftms = a.hasService(0x1826);
    switch (model) {
        case Model::True: return matchesTrue(a);
        case Model::Assault: return matchesAssault(a);
        case Model::Wdway: return matchesWdway(a);
        // Upstream routes MYRUN/MERACH-U3 and KICKR RUN to their classes even
        // with FTMS (the classes drive the FTMS characteristics themselves).
        case Model::TechnogymMyRun: return starts(name, "MYRUN ") || starts(name, "MERACH-U3");
        case Model::KickrRun: return starts(name, "KICKR RUN");
        case Model::Lifespan: return !ftms && starts(name, "LIFESPAN");
        case Model::OctaneZr7: return !ftms && starts(name, "ZR7");
        case Model::OctaneZr8: return !ftms && starts(name, "ZR8");
        case Model::Zipro: return !ftms && starts(name, "RZ_TREADMIL");
        case Model::EsangLinker: return !ftms && starts(name, "ESANGLINKER");
        case Model::Sperax: return !ftms && (starts(name, "SPERAX_RM01") || starts(name, "SPERAX_RM-02"));
        case Model::Focus: return !ftms && starts(name, "EW-TM-");
        case Model::PitPat: return !ftms && starts(name, "PITPAT-T");
        case Model::Activio: return !ftms && starts(name, "RUNNERT");
        default: return false;
    }
}

std::uint8_t crc8(const std::uint8_t* data, std::size_t length) {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint8_t>(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

Bytes ziproPoll(bool force, std::uint8_t speedTenths, std::uint8_t incline) {
    const std::uint8_t f = force ? 1 : 0;
    return {0xfb, 0x07, 0xa1, 0x02, f, speedTenths, incline,
            static_cast<std::uint8_t>(0xaa + f + speedTenths + incline), 0xfc};
}
Bytes focusControl(std::uint8_t speedTenths, std::uint8_t incline) {
    return {0xf0, 0xc3, 0x03, 0x03, speedTenths, incline, static_cast<std::uint8_t>(0xb9 + speedTenths + incline)};
}
Bytes speraxSpeedFrame(int speedTenths) {
    if (speedTenths < 1 || speedTenths > 60) return {};
    return hex(kSperaxSpeed[speedTenths - 1]);
}
Bytes pitpatControl(std::uint16_t speedThousandths, std::uint8_t incline) {
    Bytes out = hex("6a170000000003e80108640c000000000000167a679643");
    out[6] = static_cast<std::uint8_t>(speedThousandths >> 8);
    out[7] = static_cast<std::uint8_t>(speedThousandths);
    out[9] = incline;
    std::uint8_t sum = 0;
    for (std::size_t i = 5; i <= out.size() - 3; ++i) sum ^= out[i];
    out[21] = static_cast<std::uint8_t>(sum ^ out[1]);
    return out;
}
Bytes activioSpeedFrame(std::uint8_t v, bool alternateChecksum) {
    static const std::int8_t offsets[4] = {5, 3, 1, -1};
    const std::uint8_t check =
        static_cast<std::uint8_t>(alternateChecksum ? v + offsets[v & 0x03] : v + 1);
    return {0x03, v, 0x00, 0x00, 0x00, static_cast<std::uint8_t>(0x28 + v), check};
}
Bytes activioInclineFrame(std::uint8_t incline, bool umay, std::uint8_t speedTenths) {
    if (!umay)
        return {0x04, 0x00, incline, 0x00, 0x00, static_cast<std::uint8_t>(0x29 + incline),
                static_cast<std::uint8_t>(0x06 + incline)};
    static const std::int8_t offsets[4] = {0, 1, -2, -1};
    return {0x04, speedTenths, incline, 0x00, 0x00,
            static_cast<std::uint8_t>(0x47 + (incline & 0xFC) + offsets[incline & 0x03]),
            static_cast<std::uint8_t>(0x1C + incline)};
}
Bytes esangSpeedFrame(float kmh) {
    Bytes out{0xa9, 0x01, 0x01, static_cast<std::uint8_t>(std::lround(kmh * 10.0 * 0.621371)), 0x00};
    out[4] = static_cast<std::uint8_t>(out[0] ^ out[1] ^ out[2] ^ out[3]);
    return out;
}
Bytes costawaySpeedFrame(float kmh) {
    Bytes out{0xa9, 0xa0, 0x03, 0x02, static_cast<std::uint8_t>(units(kmh, 10)), 0x00, 0x00};
    for (std::size_t i = 0; i < 6; ++i) out[6] ^= out[i];
    return out;
}
Bytes esangHandshakeReply(std::uint8_t random) { return {0xa9, 0x08, 0x01, random, crc8(&random, 1)}; }
Bytes esangPairReply(std::uint8_t selector, std::uint8_t n1, std::uint8_t n2) {
    const auto sum = static_cast<std::uint8_t>(n1 + n2);
    const auto product = static_cast<std::uint8_t>(n1 * n2);
    std::uint8_t c[4];
    switch (selector % 6) {
        case 0: c[0] = n1, c[1] = n2, c[2] = sum, c[3] = product; break;
        case 1: c[0] = n1, c[1] = n2, c[2] = product, c[3] = sum; break;
        case 2: c[0] = sum, c[1] = n1, c[2] = n2, c[3] = product; break;
        case 3: c[0] = product, c[1] = n1, c[2] = n2, c[3] = sum; break;
        case 4: c[0] = sum, c[1] = product, c[2] = n1, c[3] = n2; break;
        default: c[0] = product, c[1] = sum, c[2] = n1, c[3] = n2; break;
    }
    return {0xa9, 0x08, 0x04, c[0], c[1], c[2], c[3], crc8(c, 4)};
}
Bytes kickrRunSpeedFrame(float kmh, bool first) {
    const auto um = static_cast<std::uint32_t>(std::lround(kmh / 3.6 * 1e6));
    return {0x02, static_cast<std::uint8_t>(um), static_cast<std::uint8_t>(um >> 8),
            static_cast<std::uint8_t>(um >> 16), 0x00, static_cast<std::uint8_t>(first ? 0xFF : 0x0A)};
}
int activioDecode(std::uint8_t raw) {
    const auto v = static_cast<std::uint8_t>(raw - 0x40);
    std::uint8_t high = static_cast<std::uint8_t>(v >> 4);
    const std::uint8_t low = v & 0x0F;
    if (!(kActivioNoBorrow & (1U << low))) --high;
    return static_cast<std::uint8_t>((high << 4) + kActivioNibble[low]);
}
int activioIncline(std::uint8_t raw) {
    for (int i = 0; i < 16; ++i)
        if (kActivioInclineRaw[i] == raw) return i;
    return raw;
}

std::size_t descriptorCount() { return static_cast<std::size_t>(Model::Count) + 2; }

const ProtocolInfo& descriptorAt(std::size_t index) {
    static const ProtocolInfo table[] = {
#define ENTRY(M)                                                                                                   \
    {spec(M).id, spec(M).label, (spec(M).flags & kAuto) ? kMatchPriority : 0, modelMatch<M>, modelFactory<M>,     \
     (spec(M).flags & (kSpeed | kIncline)) != 0, false, MachineProfile::Treadmill}
        ENTRY(Model::True),        ENTRY(Model::Assault),      ENTRY(Model::Wdway),       ENTRY(Model::TechnogymMyRun),
        ENTRY(Model::Lifespan),    ENTRY(Model::OctaneZr7),    ENTRY(Model::OctaneZr8),   ENTRY(Model::Zipro),
        ENTRY(Model::EsangLinker), ENTRY(Model::Costaway),     ENTRY(Model::Sperax),      ENTRY(Model::Focus),
        ENTRY(Model::PitPat),      ENTRY(Model::Activio),      ENTRY(Model::ActivioV460), ENTRY(Model::ActivioZt2500),
        ENTRY(Model::ActivioUmay), ENTRY(Model::KickrRun),
#undef ENTRY
        {"lifefitness-tm", "Life Fitness treadmill (FTMS data, read-only)", kMatchPriority, lifeFitnessMatch,
         lifeFitnessFactory, false, false, MachineProfile::Treadmill},
        {"shuaa5-tm", "ShuaA5 treadmill (FTMS)", kMatchPriority, shuaA5Match, shuaA5Factory, true, false,
         MachineProfile::Treadmill},
    };
    static_assert(sizeof(table) / sizeof(table[0]) == static_cast<std::size_t>(Model::Count) + 2, "descriptors");
    return table[index < descriptorCount() ? index : 0];
}

}  // namespace vendor_treadmills

using namespace vendor_treadmills;

VendorTreadmillSource::VendorTreadmillSource(Model model) : model_(model) {}

const char* VendorTreadmillSource::id() const { return spec(model_).id; }

ble::ConnectPlan VendorTreadmillSource::plan() const {
    ble::ConnectPlan plan;
    switch (spec(model_).gatt) {
        case kGattTrue: plan.subscribe(kTrueService, kTrueNotify); break;
        case kGattFtms:
            plan.subscribe(uuid16(ftms::kServiceUuid), uuid16(ftms::kTreadmillDataUuid));
            plan.subscribe(uuid16(ftms::kServiceUuid), uuid16(ftms::kControlPointUuid), true);
            plan.subscribe(uuid16(0x1814), uuid16(0x2A53));
            break;
        case kGattFff0: plan.subscribe(uuid16(0xFFF0), uuid16(0xFFF1)); break;
        case kGattOctane: plan.subscribe(kOctaneService, kOctaneNotify); break;
        case kGattPitPat: plan.subscribe(uuid16(0xFBA0), uuid16(0xFBA2)); break;
        case kGattActivio: plan.subscribe(kActivioService, kActivioNotify); break;
        default:
            plan.discoverNotifications = true;
            plan.discoveryWriteCharacteristics[0] = kKickrCommand;
            break;
    }
    return plan;
}

void VendorTreadmillSource::reset() {
    SourceProtocol::reset();
    connected_ = ready_ = haveEndpoint_ = false;
    step_ = waitEvent_ = events_ = lastPoll_ = 0;
    nextStepMs_ = nextPollMs_ = 0;
    controlStarted_ = ftmsRequested_ = firstSpeedSent_ = paddlePending_ = deviceIdSeen_ = false;
    buffer_.clear();
    expected_ = 0;
    lowCadenceSinceMs_ = lastCadenceMs_ = 0;
}

void VendorTreadmillSource::onWriteEndpoint(const ble::Uuid& service, const ble::Uuid& characteristic) {
    if (characteristic != kKickrCommand) return;
    endpointService_ = service;
    haveEndpoint_ = true;
}

bool VendorTreadmillSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if (spec(model_).gatt == kGattDiscover) {
        if (!haveEndpoint_ || subscribedMask == 0) {
            fail("KICKR RUN command characteristic not found");
            return false;
        }
    } else if (!(subscribedMask & 1U)) {
        fail("treadmill data characteristic not available");
        return false;
    }
    connected_ = true;
    random_ ^= nowMs * 2654435761U;
    step_ = 0;
    nextStepMs_ = nowMs;
    runScript(nowMs);
    return true;
}

void VendorTreadmillSource::send(std::uint8_t target, const Bytes& payload) {
    switch (spec(model_).gatt) {
        case kGattFtms: queueWrite(uuid16(ftms::kServiceUuid), uuid16(ftms::kControlPointUuid), payload); break;
        case kGattOctane: queueWrite(kOctaneService, kOctaneWrite, payload); break;
        case kGattPitPat:
            if (target == kUnlock)
                queueWrite(uuid16(0x1801), uuid16(0x2B2A), payload);
            else
                queueWrite(uuid16(0xFBA0), uuid16(0xFBA1), payload);
            break;
        case kGattActivio: queueWrite(kActivioService, target == kWrite2 ? kActivioWrite2 : kActivioWrite, payload); break;
        case kGattDiscover:
            if (target == kFtmsCp)
                queueWrite(uuid16(ftms::kServiceUuid), uuid16(ftms::kControlPointUuid), payload);
            else
                queueWrite(endpointService_, kKickrCommand, payload, false);
            break;
        default: queueWrite(uuid16(0xFFF0), uuid16(0xFFF2), payload, model_ != Model::Sperax); break;
    }
}

void VendorTreadmillSource::runScript(std::uint32_t nowMs) {
    const Spec& s = spec(model_);
    while (!ready_) {
        if (waitEvent_ != kNone) {
            // Waiting for the device's handshake/pair request (or its timeout).
            if (!(events_ & waitEvent_) && static_cast<std::int32_t>(nowMs - nextStepMs_) < 0) return;
            waitEvent_ = kNone;
            nextStepMs_ = nowMs;
        } else if (static_cast<std::int32_t>(nowMs - nextStepMs_) < 0) {
            return;
        }
        if (step_ >= s.scriptLength) {
            ready_ = true;
            nextPollMs_ = nowMs;
            return;
        }
        const Step& step = s.script[step_++];
        nextStepMs_ = nowMs + step.delayMs;
        waitEvent_ = step.wait;
        if (step.frame) {
            send(step.target, hex(step.frame));
            return;  // one scripted write per tick keeps the outbox small
        }
    }
}

void VendorTreadmillSource::poll(std::uint32_t nowMs) {
    const Spec& s = spec(model_);
    if (!s.pollMs || static_cast<std::int32_t>(nowMs - nextPollMs_) < 0) return;
    nextPollMs_ = nowMs + s.pollMs;
    switch (model_) {
        case Model::Lifespan:
            lastPoll_ = lastPoll_ == 0x82 ? 0x88 : 0x82;
            send(kMain, {0xA1, lastPoll_, 0x00, 0x00, 0x00});
            break;
        case Model::Zipro: {
            const float speed = sample_.speed.valid ? sample_.speed.value : 0.0F;
            const float incline = sample_.incline.valid ? sample_.incline.value : 0.0F;
            send(kMain, ziproPoll(false, static_cast<std::uint8_t>(units(speed, 10)),
                                  static_cast<std::uint8_t>(static_cast<int>(incline))));
            break;
        }
        case Model::Sperax: send(kMain, hex("f5080019f00a59fa")); break;
        case Model::Focus: send(kMain, hex(F_I3)); break;
        case Model::PitPat: send(kMain, hex("6a05fdf843")); break;
        default: break;
    }
}

void VendorTreadmillSource::tick(std::uint32_t nowMs) {
    if (!connected_) return;
    if (!ready_) {
        runScript(nowMs);
        return;
    }
    poll(nowMs);
    // ZR8: upstream zeroes speed after 30 s without a valid cadence.
    if (model_ == Model::OctaneZr8 && lastCadenceMs_ && nowMs - lastCadenceMs_ > 30000 && sample_.speed.valid &&
        sample_.speed.value > 0) {
        sample_.speed.value = 0;
        sample_.cadence = {0.0F, true};
        lastCadenceMs_ = 0;
    }
}

void VendorTreadmillSource::onWriteFailed(std::uint32_t) { setError("treadmill write failed"); }

std::uint8_t VendorTreadmillSource::nextRandom() {
    random_ = random_ * 1664525U + 1013904223U;
    return static_cast<std::uint8_t>(random_ >> 24);
}

void VendorTreadmillSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length,
                                     std::uint32_t nowMs) {
    if (!connected_ || !data || length == 0) return;
    decode(index, data, length, nowMs);
}

void VendorTreadmillSource::decodeOctane(const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    const bool zr8 = model_ == Model::OctaneZr8;
    Bytes value;
    if (zr8 && data[0] == 0xa5) {
        if (length < 2) return;
        std::size_t expected = 0;
        switch (data[1]) {
            case 0x1d: expected = 29; break;
            case 0x26: expected = 38; break;
            case 0x23: expected = 35; break;
            case 0x20:
            case 0x21: expected = 32; break;
            default: return;
        }
        buffer_.assign(data, data + length);
        expected_ = expected;
    } else if (zr8 && expected_ > 0) {
        if (buffer_.size() + length > 64) {
            buffer_.clear();
            expected_ = 0;
            return;
        }
        buffer_.insert(buffer_.end(), data, data + length);
    } else {
        if (length != 20 || zr8) return;
        if (data[0] == 0xa5 && data[1] == 0x17) return;
        value.assign(data, data + length);
    }
    if (zr8) {
        if (buffer_.size() < expected_) return;
        value.assign(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(expected_));
        buffer_.clear();
        expected_ = 0;
        if (value.size() < 18 || value[0] != 0xa5 || value[2] != 0x06) return;
        if (value[1] != 0x20 && value[1] != 0x21 && value[1] != 0x23 && value[1] != 0x1d) return;
        bool updated = false;
        for (std::size_t i = 0; i + 1 < value.size(); ++i) {
            if (value[i] != 0x3A) continue;
            const std::uint8_t cadence = value[i + 1];
            if (cadence >= 20 && cadence <= 200) {
                sample_.cadence = {static_cast<float>(cadence), true};
                lastCadenceMs_ = nowMs;
                lowCadenceSinceMs_ = 0;
                updated = true;
            } else if (cadence > 0) {
                if (!lowCadenceSinceMs_) {
                    lowCadenceSinceMs_ = nowMs ? nowMs : 1;
                } else if (nowMs - lowCadenceSinceMs_ >= 5000 && sample_.speed.valid && sample_.speed.value > 0) {
                    sample_.speed.value = 0;
                    sample_.cadence = {0.0F, true};
                    updated = true;
                }
            } else if (sample_.speed.valid && sample_.speed.value > 0) {
                sample_.speed.value = 0;
                sample_.cadence = {0.0F, true};
                updated = true;
            }
            break;
        }
        if (updated) touch(nowMs);
    }
    static const std::uint8_t signs[3] = {0x02, 0x01, 0x00};
    for (const std::uint8_t sign : signs) {
        for (std::size_t i = 0; i + 1 < value.size(); ++i) {
            if (value[i] != sign || value[i + 1] != 0x23) continue;
            const std::size_t at = i + 2;
            if (at + 1 >= value.size()) return;
            const std::uint16_t pace = le16(&value[at]);
            if (pace == 0) return;
            sample_.speed = {static_cast<float>(3600.0 / pace), true};
            touch(nowMs);
            return;
        }
    }
}

void VendorTreadmillSource::decode(std::size_t index, const std::uint8_t* d, std::size_t len, std::uint32_t nowMs) {
    auto speed = [&](double kmh) { sample_.speed = {static_cast<float>(kmh), true}; };
    auto incline = [&](double percent) { sample_.incline = {static_cast<float>(percent), true}; };
    switch (model_) {
        case Model::True:
            if (len == 16) {
                speed(le16(d + 6) / 100.0);
                incline(static_cast<std::int8_t>(d[14]) / 10.0);
            } else if (len == 19) {
                speed(le16(d + 7) / 100.0);
            } else if (len == 4) {
                incline(static_cast<std::int8_t>(d[2]) / 10.0);
            } else {
                return;
            }
            break;
        case Model::Assault:
            if (len != 16) return;
            speed(le16(d + 5) / 100.0);
            break;
        case Model::Wdway:
            if (len != 18) return;
            speed(le16(d + 6) / 100.0);
            incline(static_cast<std::int8_t>(d[16]) / 10.0);
            break;
        case Model::TechnogymMyRun:
            if (index == 0) {
                MetricSet decoded;
                if (!ftms::decodeTreadmillData(d, len, decoded)) return;
                for (std::size_t m = 0; m < kMetricCount; ++m) {
                    const auto metric = static_cast<Metric>(m);
                    if (decoded[metric].valid) sample_[metric] = decoded[metric];
                }
            } else if (index == 2 && len >= 4) {
                sample_.cadence = {static_cast<float>(d[3]), true};
            } else {
                return;
            }
            break;
        case Model::Lifespan:
            if (len < 4) return;
            if (lastPoll_ == 0x82)
                speed(d[2] + d[3] / 100.0);
            else if (lastPoll_ == 0x88)
                sample_.stepCount = {static_cast<float>(be16(d + 2)), true};
            else
                return;
            break;
        case Model::OctaneZr7:
        case Model::OctaneZr8: decodeOctane(d, len, nowMs); return;
        case Model::Zipro:
            if (len != 18) return;
            speed(d[5] / 10.0);
            incline(static_cast<std::int8_t>(d[6]));
            if (d[15]) sample_.heartRate = {static_cast<float>(d[15]), true};
            break;
        case Model::EsangLinker:
            if (len >= 4 && d[0] == 0xa9 && d[1] == 0x08 && d[2] == 0x04) {
                const std::uint8_t n1 = static_cast<std::uint8_t>(1 + nextRandom() % 15);
                const std::uint8_t n2 = static_cast<std::uint8_t>(1 + nextRandom() % 15);
                send(kMain, esangPairReply(d[3], n1, n2));
                events_ |= kPair;
                runScript(nowMs);
                return;
            }
            if (len >= 5 && d[0] == 0xa9 && d[1] == 0x08 && d[2] == 0x01 && d[3] == 0xff && d[4] == 0x5f) {
                send(kMain, esangHandshakeReply(nextRandom()));
                events_ |= kHandshake;
                runScript(nowMs);
                return;
            }
            if (len != 5 || d[1] != 0xe0) return;
            speed(d[3] / 10.0 * 1.60934);
            break;
        case Model::Costaway:
            if (len != 5) return;
            speed(d[3] == 0xFF ? 0.0 : d[3] / 10.0 * 1.60934);
            break;
        case Model::Sperax:
            if (len < 24) return;
            speed(d[17 + len - 24] / 10.0);
            sample_.stepCount = {static_cast<float>(be16(d + 15)), true};
            break;
        case Model::Focus:
            if (len != 20) return;
            speed(d[10] / 10.0);
            incline(static_cast<std::int8_t>(d[11]));
            break;
        case Model::PitPat:
            if (len < 50) return;
            speed(be16(d + 3) / 1000.0);
            incline(d[11]);
            break;
        case Model::Activio:
        case Model::ActivioV460:
        case Model::ActivioZt2500:
        case Model::ActivioUmay: {
            if (len < 12) return;
            speed(activioDecode(d[1]) / 10.0);
            const int heart = activioDecode(d[8]);
            if (heart > 0) sample_.heartRate = {static_cast<float>(heart), true};
            if (model_ != Model::ActivioV460) incline(activioIncline(d[9]));
            break;
        }
        case Model::KickrRun:
            if (len >= 3 && d[0] == 0x80) return;  // FTMS control point response
            if (len >= 7 && d[0] == 0xFE && d[1] == 0x86 && d[2] == 0x01) {
                if (!deviceIdSeen_) {
                    deviceIdSeen_ = true;
                    send(kMain, {0x87, d[3], d[4], d[5], d[6]});
                }
                return;
            }
            if (len >= 3 && d[0] == 0xFD && d[1] == 0xE0) {
                if (d[2] == 0x01 && len >= 5) {
                    paddlePending_ = false;
                    send(kMain, {0xE0, d[3], d[4]});
                }
                return;
            }
            // Vendor telemetry (FF 01 + 24-bit belt speed in um/s). A full
            // FTMS treadmill record with flags 0x01FF is 23 bytes long.
            if (len >= 5 && len < 23 && d[0] == 0xFF && d[1] == 0x01) {
                if (paddlePending_) return;
                const std::uint32_t um = d[2] | d[3] << 8 | static_cast<std::uint32_t>(d[4]) << 16;
                speed(um / 1e6 * 3.6);
                break;
            } else {
                MetricSet decoded;
                if (!ftms::decodeTreadmillData(d, len, decoded) || !decoded.incline.valid) return;
                sample_.incline = decoded.incline;
            }
            break;
        default: return;
    }
    touch(nowMs);
}

bool VendorTreadmillSource::fresh(std::uint32_t nowMs) const {
    return connected_ && ready_ && hasData() && nowMs - lastDataMs() <= kFreshDataMs;
}
bool VendorTreadmillSource::running(std::uint32_t nowMs) const {
    return fresh(nowMs) && sample_.speed.valid && sample_.speed.value > 0.0F;
}

bool VendorTreadmillSource::control(const ControlRequest& request, std::uint32_t nowMs) {
    const Spec& s = spec(model_);
    const float value = request.value;
    if (!std::isfinite(value) || !running(nowMs)) return false;
    const float current = sample_.speed.value;
    const std::uint8_t currentIncline =
        static_cast<std::uint8_t>(sample_.incline.valid && sample_.incline.value > 0 ? static_cast<int>(sample_.incline.value) : 0);
    if (request.control == Control::Speed) {
        if (!(s.flags & kSpeed)) return false;
        const bool stop = value == 0.0F;
        if (!stop && (value < s.minSpeed || value > s.maxSpeed)) return false;
        switch (model_) {
            case Model::TechnogymMyRun:
                if (stop) {
                    send(kFtmsCp, {0x08, 0x01});
                } else {
                    const auto raw = static_cast<std::uint16_t>(units(value, 100));
                    send(kFtmsCp, {0x02, static_cast<std::uint8_t>(raw), static_cast<std::uint8_t>(raw >> 8)});
                }
                return true;
            case Model::Zipro:
                if (stop) {
                    const Bytes frame = hex("fb05a20401acfc");
                    for (int i = 0; i < 3; ++i) send(kMain, frame);
                } else {
                    send(kMain, ziproPoll(true, static_cast<std::uint8_t>(units(value, 10)), currentIncline));
                }
                return true;
            case Model::EsangLinker:
            case Model::Costaway:
                if (stop)
                    send(kMain, hex("a9a301000b"));
                else
                    send(kMain, model_ == Model::Costaway ? costawaySpeedFrame(value) : esangSpeedFrame(value));
                return true;
            case Model::Sperax:
                send(kMain, stop ? hex("f50a00150000 00d301fa") : speraxSpeedFrame(units(value, 10)));
                return true;
            case Model::Focus:
                if (stop) {
                    send(kMain, hex("f0c303020000b8"));
                    send(kMain, hex(F_I4));
                } else {
                    send(kMain, focusControl(static_cast<std::uint8_t>(units(value, 10)), currentIncline));
                }
                return true;
            case Model::PitPat:
                send(kMain, stop ? hex("6a170000000000000500 8a00020000000000122e0caa43")
                                 : pitpatControl(static_cast<std::uint16_t>(units(value, 1000)), currentIncline));
                return true;
            case Model::Activio:
            case Model::ActivioV460:
            case Model::ActivioZt2500:
            case Model::ActivioUmay:
                send(kMain, stop ? hex("02000000002704")
                                 : activioSpeedFrame(static_cast<std::uint8_t>(units(value, 10)),
                                                     model_ == Model::ActivioV460 || model_ == Model::ActivioZt2500));
                return true;
            case Model::KickrRun:
                if (!controlStarted_) {
                    controlStarted_ = true;
                    send(kMain, {0x19, 0x00});
                }
                send(kMain, kickrRunSpeedFrame(value, !firstSpeedSent_));
                firstSpeedSent_ = true;
                if (!stop) paddlePending_ = true;
                return true;
            default: return false;
        }
    }
    if (request.control != Control::Incline || !(s.flags & kIncline)) return false;
    if (value < s.minIncline || value > s.maxIncline) return false;
    const auto speedTenths = static_cast<std::uint8_t>(units(current, 10));
    switch (model_) {
        case Model::TechnogymMyRun:
        case Model::KickrRun: {
            if (model_ == Model::KickrRun && !ftmsRequested_) {
                // Upstream also sends FTMS Start/Resume here; omitted (never start the belt).
                ftmsRequested_ = true;
                send(kFtmsCp, {0x00});
            }
            const auto raw = static_cast<std::int16_t>(model_ == Model::KickrRun ? std::lround(value * 10.0)
                                                                                 : units(value, 10));
            send(kFtmsCp, {0x03, static_cast<std::uint8_t>(raw & 0xFF), static_cast<std::uint8_t>((raw >> 8) & 0xFF)});
            return true;
        }
        case Model::Zipro: send(kMain, ziproPoll(true, speedTenths, static_cast<std::uint8_t>(static_cast<int>(value)))); return true;
        case Model::Focus: send(kMain, focusControl(speedTenths, static_cast<std::uint8_t>(static_cast<int>(value)))); return true;
        case Model::PitPat:
            send(kMain, pitpatControl(static_cast<std::uint16_t>(units(current, 1000)),
                                      static_cast<std::uint8_t>(static_cast<int>(value))));
            return true;
        case Model::Activio:
        case Model::ActivioZt2500:
        case Model::ActivioUmay:
            send(kMain, activioInclineFrame(static_cast<std::uint8_t>(std::lround(value)), model_ == Model::ActivioUmay,
                                            static_cast<std::uint8_t>(std::lround(current * 10.0))));
            return true;
        default: return false;
    }
}

TrainerCapabilities VendorTreadmillSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    if (!connected_ || !ready_) return capabilities;
    capabilities.setControl(Control::Speed, (spec(model_).flags & kSpeed) != 0);
    capabilities.setControl(Control::Incline, (spec(model_).flags & kIncline) != 0);
    return capabilities;
}

}  // namespace protocols
}  // namespace smartfitness
