// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{proformrower,bhfitnesselliptical,ypooelliptical,soleelliptical,
// octaneelliptical,smartrowrower,sunnyfitstepper,concept2skierg}/ (same-named
// .cpp/.h) and src/devices/bluetooth.cpp (advertisement rules). This port is
// part of smartfitness-bridge and distributed under the same license.

#include "protocols/vendor_cardio.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace smartfitness {
namespace protocols {
namespace vendor_cardio {
namespace {

ble::Uuid uuid(const char* text) { return ble::Uuid::fromString(text); }
ble::Uuid u16(std::uint16_t value) { return ble::Uuid::from16(value); }

const ble::Uuid kFtms = u16(0x1826);
const ble::Uuid kCrossData = u16(0x2ace);
const ble::Uuid kBikeData = u16(0x2ad2);
const ble::Uuid kControlPoint = u16(0x2ad9);
const ble::Uuid kHrService = u16(0x180d);
const ble::Uuid kHrMeasurement = u16(0x2a37);
const ble::Uuid kFff0 = u16(0xfff0);
const ble::Uuid kFff2 = u16(0xfff2);
const char* const kProformService = "00001533-1412-efde-1523-785feabcd123";
const char* const kProformWrite = "00001534-1412-efde-1523-785feabcd123";
const char* const kProformNotify = "00001535-1412-efde-1523-785feabcd123";
const char* const kSoleService = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
const char* const kSoleWrite = "49535343-8841-43f4-a8d4-ecbe34729bb3";
const char* const kSoleNotify = "49535343-1e4d-4bd9-ba61-23c647249616";
const char* const kOctaneService = "96dc867d-7a83-4c22-b6be-6381d727aeda";
const char* const kOctaneNotify = "96dc867e-7a83-4c22-b6be-6381d727aeda";
const char* const kOctaneWrite = "96dc867f-7a83-4c22-b6be-6381d727aeda";
const char* const kSunnyService = "fd710001-e950-458e-8a4d-a1cbc5aa4cce";
const char* const kSunnyWrite = "fd710002-e950-458e-8a4d-a1cbc5aa4cce";
const char* const kSunnyNotify1 = "fd710003-e950-458e-8a4d-a1cbc5aa4cce";
const char* const kSunnyNotify4 = "fd710006-e950-458e-8a4d-a1cbc5aa4cce";
const char* const kConcept2Service = "ce060030-43e5-11e4-916c-0800200c9a66";
const char* const kConcept2Multiplex = "ce060080-43e5-11e4-916c-0800200c9a66";

std::string upper(const std::string& value) {
    std::string out = value;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
bool prefix(const std::string& name, const char* p) { return name.compare(0, std::strlen(p), p) == 0; }
bool any(const std::string& name, std::initializer_list<const char*> prefixes) {
    for (const char* p : prefixes) if (prefix(name, p)) return true;
    return false;
}
bool ftms(const ble::Advertisement& a) { return a.hasService(0x1826); }
unsigned le16(const std::uint8_t* p) { return static_cast<unsigned>(p[0] | (p[1] << 8)); }
unsigned le24(const std::uint8_t* p) { return le16(p) | (static_cast<unsigned>(p[2]) << 16); }
void set(MetricSample& sample, float value) { sample = {value, true}; }
bool anyValid(const MetricSet& set) {
    for (std::size_t i = 0; i < kMetricCount; ++i) if (set[static_cast<Metric>(i)].valid) return true;
    return false;
}

// QString::toInt() of the text after "SF-": whole remainder must be a number.
bool positiveNumber(const std::string& text) {
    if (text.empty() || text.size() > 9) return false;
    long value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    return value > 0;
}

bool isYpooFtmsGroup(Kind kind) {
    return kind == Kind::YpooFtms || kind == Kind::YpooSch411 || kind == Kind::YpooFeier ||
           kind == Kind::YpooSoleE25 || kind == Kind::YpooTrue;
}

Bytes proformFrame(std::uint8_t b9, std::uint8_t b10, std::uint8_t lo, std::uint8_t hi, std::uint8_t sum) {
    return {0xff, 0x0d, 0x02, 0x04, 0x02, 0x09, 0x14, 0x09, 0x02, b9, b10, lo, hi, 0x00, sum,
            0x00, 0x00, 0x00, 0x00, 0x00};
}
// {byte 11, byte 12, byte 14} of each upstream 20-byte resistance frame.
constexpr std::uint8_t k750r[24][3] = {
    {0x9f, 0x01, 0xc4}, {0x3f, 0x03, 0x66}, {0xdf, 0x04, 0x07}, {0x7f, 0x06, 0xa9}, {0x1f, 0x08, 0x4b},
    {0xbf, 0x09, 0xec}, {0x5f, 0x0b, 0x8e}, {0xff, 0x0c, 0x2f}, {0x9f, 0x0e, 0xd1}, {0x3f, 0x10, 0x73},
    {0xdf, 0x11, 0x14}, {0x7f, 0x13, 0xb6}, {0x1f, 0x15, 0x58}, {0xbf, 0x16, 0xf9}, {0x5f, 0x18, 0x9b},
    {0xff, 0x19, 0x3c}, {0x9f, 0x1b, 0xde}, {0x3f, 0x1d, 0x80}, {0xdf, 0x1e, 0x21}, {0x7f, 0x20, 0xc3},
    {0x1f, 0x22, 0x65}, {0xbf, 0x23, 0x06}, {0x5f, 0x25, 0xa8}, {0xff, 0x26, 0x49}};
constexpr std::uint8_t kSportRl[24][3] = {
    {0x76, 0x01, 0x9b}, {0x17, 0x03, 0x3e}, {0xb8, 0x04, 0xe0}, {0x58, 0x06, 0x82}, {0xf9, 0x07, 0x24},
    {0x9a, 0x09, 0xc7}, {0x3a, 0x0b, 0x69}, {0xdb, 0x0c, 0x0b}, {0x7c, 0x0e, 0xae}, {0x1c, 0x10, 0x50},
    {0xbd, 0x11, 0xf2}, {0x5e, 0x13, 0x95}, {0xfe, 0x14, 0x36}, {0x9f, 0x16, 0xd9}, {0x40, 0x18, 0x7c},
    {0xe0, 0x19, 0x1d}, {0x81, 0x1b, 0xc0}, {0x22, 0x1d, 0x63}, {0xc2, 0x1e, 0x04}, {0x63, 0x20, 0xa7},
    {0x04, 0x22, 0x4a}, {0xa4, 0x23, 0xeb}, {0x45, 0x25, 0x8e}, {0xe6, 0x26, 0x30}};

Bytes pad20(std::initializer_list<std::uint8_t> head) {
    Bytes out(head);
    out.resize(20, 0);
    return out;
}

}  // namespace

// ---- proformrower --------------------------------------------------------
Bytes proformUnlockFrame() { return {0xfe, 0x02, 0x0d, 0x02}; }

Bytes proformResistanceFrame(Kind kind, int level) {
    if (level < 1 || level > 24) return {};
    const auto i = static_cast<std::size_t>(level - 1);
    if (kind == Kind::Proform750r) return proformFrame(0x01, 0x04, k750r[i][0], k750r[i][1], k750r[i][2]);
    if (kind == Kind::ProformRower && level <= 3) {
        constexpr std::uint8_t hi[3] = {0x01, 0x03, 0x04};
        constexpr std::uint8_t sum[3] = {0x32, 0x34, 0x35};
        return proformFrame(0x02, 0x00, 0x10, hi[i], sum[i]);
    }
    return proformFrame(0x01, 0x04, kSportRl[i][0], kSportRl[i][1], kSportRl[i][2]);
}

std::vector<Bytes> proformInitFrames(Kind kind) {
    const Bytes fe08{0xfe, 0x02, 0x08, 0x02};
    const Bytes ff08a = pad20({0xff, 0x08, 0x02, 0x04, 0x02, 0x04, 0x02, 0x04, 0x81, 0x87});
    const Bytes ff08b = pad20({0xff, 0x08, 0x02, 0x04, 0x02, 0x04, 0x14, 0x04, 0x80, 0x98});
    const Bytes ff08c = pad20({0xff, 0x08, 0x02, 0x04, 0x02, 0x04, 0x14, 0x04, 0x88, 0xa0});
    const Bytes ff08d = pad20({0xff, 0x08, 0x02, 0x04, 0x02, 0x04, 0x02, 0x04, 0x95, 0x9b});
    const Bytes fe0a{0xfe, 0x02, 0x0a, 0x02};
    const Bytes fe2c{0xfe, 0x02, 0x2c, 0x04};
    const Bytes fe19{0xfe, 0x02, 0x19, 0x03};
    const Bytes fe17{0xfe, 0x02, 0x17, 0x03};
    const Bytes p15a = pad20({0x00, 0x12, 0x02, 0x04, 0x02, 0x15, 0x14, 0x15, 0x02, 0x00, 0x0f, 0x00, 0x10, 0x00,
                              0xc0, 0x1c, 0x4c, 0x00, 0x00, 0xe0});
    const Bytes ff07a = pad20({0xff, 0x07, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x6a});
    const Bytes p13 = pad20({0x00, 0x12, 0x02, 0x04, 0x02, 0x13, 0x14, 0x13, 0x02, 0x0c});
    const Bytes ff05 = pad20({0xff, 0x05, 0x00, 0x80, 0x01, 0x00, 0xb6});
    const Bytes p15e = pad20({0x00, 0x12, 0x02, 0x04, 0x02, 0x15, 0x14, 0x15, 0x02, 0x0e});
    const Bytes ff07b = pad20({0xff, 0x07, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x4a});
    if (kind == Kind::Proform750r) {
        const Bytes fe0b{0xfe, 0x02, 0x0b, 0x02};
        const Bytes fe18{0xfe, 0x02, 0x18, 0x03};
        const Bytes fe11{0xfe, 0x02, 0x11, 0x02};
        const Bytes fe10{0xfe, 0x02, 0x10, 0x02};
        const Bytes p15f = pad20({0x00, 0x12, 0x02, 0x04, 0x02, 0x15, 0x14, 0x15, 0x02, 0x00, 0x0f, 0x80, 0x00, 0x40});
        const Bytes ff07c = pad20({0xff, 0x07, 0x00, 0x00, 0x00, 0x10, 0x80, 0x11, 0x9b});
        return {fe08, ff08a, fe08, ff08b, fe08, ff08c, fe0b,
                pad20({0xff, 0x0b, 0x02, 0x04, 0x02, 0x07, 0x02, 0x07, 0x82, 0x00, 0x00, 0x8b}), fe0a,
                pad20({0xff, 0x0a, 0x02, 0x04, 0x02, 0x06, 0x02, 0x06, 0x84, 0x00, 0x00, 0x8c}), fe08, ff08d, fe2c,
                {0x00, 0x12, 0x02, 0x04, 0x02, 0x28, 0x14, 0x28, 0x90, 0x07, 0x01, 0x1b, 0x20, 0x37, 0x44, 0x53, 0x60,
                 0x77, 0x9c, 0xbb},
                {0x01, 0x12, 0xc0, 0xe7, 0x04, 0x23, 0x40, 0x67, 0x8c, 0xdb, 0xe0, 0x37, 0x44, 0x93, 0xa0, 0xf7, 0x3c,
                 0x7b, 0x80, 0xc7},
                pad20({0xff, 0x08, 0x04, 0x43, 0x80, 0x98, 0x02, 0x00, 0x00, 0xcb}), fe19, p15e, ff07b, fe17, p13,
                ff05, fe19, p15a, ff07a, fe18,
                pad20({0x00, 0x12, 0x02, 0x04, 0x02, 0x14, 0x14, 0x14, 0x02, 0x00, 0x0e, 0x1c, 0x96, 0x31, 0x00, 0x10,
                       0x40, 0x40, 0x00, 0x80}),
                pad20({0xff, 0x06, 0x00, 0x00, 0x00, 0x81, 0x60, 0x0c}), fe11,
                {0xff, 0x11, 0x02, 0x04, 0x02, 0x0d, 0x14, 0x0d, 0x02, 0x05, 0x00, 0x00, 0x00, 0x00, 0x08, 0x58, 0x02,
                 0x00, 0x8a, 0x00},
                fe19, p15e, ff07b, fe10,
                pad20({0xff, 0x10, 0x02, 0x04, 0x02, 0x0c, 0x14, 0x0c, 0x02, 0x04, 0x00, 0x00, 0x00, 0x02, 0xe4, 0x1f,
                       0x00, 0x2b}),
                fe19, p15f, ff07c, fe10,
                pad20({0xff, 0x10, 0x02, 0x04, 0x02, 0x0c, 0x14, 0x0c, 0x02, 0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                       0x00, 0x37})};
    }
    const Bytes ff0a1 = pad20({0xff, 0x0a, 0x02, 0x04, 0x02, 0x06, 0x02, 0x06, 0x82, 0x00, 0x00, 0x8a});
    const Bytes ff0a2 = pad20({0xff, 0x0a, 0x02, 0x04, 0x02, 0x06, 0x02, 0x06, 0x84, 0x00, 0x00, 0x8c});
    std::vector<Bytes> out{fe08, ff08a, fe08, ff08b, fe08, ff08c, fe0a, ff0a1, fe0a, ff0a2, fe08, ff08d, fe2c};
    if (kind == Kind::ProformSportRl) {
        out.insert(out.end(),
                   {{0x00, 0x12, 0x02, 0x04, 0x02, 0x28, 0x14, 0x28, 0x90, 0x07, 0x01, 0xf2, 0xf4, 0xf4, 0xf2, 0xfe,
                     0x08, 0x00, 0x1e, 0x2a},
                    {0x01, 0x12, 0x3c, 0x4c, 0x5a, 0x66, 0x90, 0x88, 0xa6, 0xc2, 0xe4, 0x04, 0x22, 0x4e, 0x98, 0xb0,
                     0xce, 0x1a, 0x2c, 0x7c},
                    pad20({0xff, 0x08, 0x8a, 0xd6, 0x20, 0x98, 0x02}), fe17, p13, ff05, fe19, p15e, ff07b, fe19, p15a,
                    ff07a});
    } else {
        out.insert(out.end(),
                   {{0x00, 0x12, 0x02, 0x04, 0x02, 0x28, 0x14, 0x28, 0x90, 0x07, 0x01, 0xed, 0xe8, 0xe9, 0xe8, 0xf5,
                     0xf0, 0x09, 0x00, 0x1d},
                    {0x01, 0x12, 0x28, 0x39, 0x48, 0x55, 0x60, 0x99, 0xb0, 0xad, 0xc8, 0xe9, 0x08, 0x55, 0x70, 0x89,
                     0xa0, 0xfd, 0x08, 0x59},
                    pad20({0xff, 0x08, 0x68, 0xb5, 0xc0, 0x98, 0x02, 0x00, 0x00, 0x55}), fe17, p15a, ff07a, fe17, p13,
                    ff05, fe17});
    }
    return out;
}

Bytes proformPollFrame(Kind kind, unsigned slot) {
    slot %= 6;
    if (kind == Kind::Proform750r) {
        switch (slot) {
            case 0: return {0xfe, 0x02, 0x18, 0x03};
            case 1:
                return {0x00, 0x12, 0x02, 0x04, 0x02, 0x14, 0x14, 0x14, 0x02, 0x00,
                        0x0e, 0x1c, 0x96, 0x31, 0x00, 0x10, 0x40, 0x40, 0x00, 0x80};
            case 2: return pad20({0xff, 0x06, 0x00, 0x00, 0x00, 0x81, 0x60, 0x0c});
            case 3: return {0xfe, 0x02, 0x19, 0x03};
            case 4: return pad20({0x00, 0x12, 0x02, 0x04, 0x02, 0x15, 0x14, 0x15, 0x02, 0x00, 0x0f, 0x80, 0x00, 0x40});
            default: return pad20({0xff, 0x07, 0x00, 0x00, 0x00, 0x10, 0x80, 0x11, 0x9b});
        }
    }
    switch (slot) {
        case 0: return {0xfe, 0x02, 0x17, 0x03};
        case 1:
            return {0x00, 0x12, 0x02, 0x04, 0x02, 0x13, 0x14, 0x13, 0x02, 0x00,
                    0x0d, 0x1c, 0x9e, 0x31, 0x00, 0x00, 0x40, 0x40, 0x00, 0x80};
        case 2: return pad20({0xff, 0x05, 0x00, 0x00, 0x00, 0x81, 0xa2});
        case 3: return {0xfe, 0x02, 0x19, 0x03};
        case 4: return pad20({0x00, 0x12, 0x02, 0x04, 0x02, 0x15, 0x14, 0x15, 0x02, 0x00, 0x0f, 0x80, 0x00, 0x40});
        default: return pad20({0xff, 0x07, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x11, 0xeb});
    }
}

int proformResistanceFromByte(std::uint8_t r) {
    // Upstream switch: raw console byte -> level 1..24, anything else -> 1.
    constexpr std::uint8_t table[][2] = {
        {1, 1},     {3, 2},     {4, 3},     {6, 4},     {7, 5},     {8, 5},     {9, 6},     {0x0b, 7},
        {0x0c, 8},  {0x0d, 8},  {0x0e, 9},  {0x10, 10}, {0x11, 11}, {0x13, 12}, {0x14, 13}, {0x15, 13},
        {0x16, 14}, {0x18, 15}, {0x19, 16}, {0x1a, 16}, {0x1b, 17}, {0x1d, 18}, {0x1e, 19}, {0x20, 20},
        {0x22, 21}, {0x23, 22}, {0x25, 23}, {0x26, 24}, {0x27, 24}};
    for (const auto& entry : table) if (entry[0] == r) return entry[1];
    return 1;
}

// ---- bhfitnesselliptical -------------------------------------------------
Bytes bhResistanceFrame(int level) {
    const auto grade = static_cast<std::int16_t>((level - 6) * 33);
    const auto raw = static_cast<std::uint16_t>(grade);
    return {0x11, 0x00, 0x00, static_cast<std::uint8_t>(raw), static_cast<std::uint8_t>(raw >> 8), 0x21, 0x22};
}

// ---- ypooelliptical ------------------------------------------------------
std::vector<Bytes> ypooInitFrames(bool gymstick) {
    const Bytes i1{0x02, 0x42, 0x42, 0x03};
    const Bytes i2{0x02, 0x41, 0x02, 0x43, 0x03};
    const Bytes i3{0x02, 0x43, 0x01, 0x42, 0x03};
    const Bytes i4{0x02, 0x44, 0x01, 0x45, 0x03};
    const Bytes i5{0x02, 0x44, 0x05, 0x01, 0x00, 0x40, 0x03};
    if (gymstick) {
        return {i1, i2, i3, i1, i4,
                {0x02, 0x44, 0x0a, 0x00, 0x00, 0x00, 0x3c, 0xaa, 0x18, 0x00, 0xc0, 0x03},
                {0x02, 0x44, 0x0b, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4e, 0x03},
                {0x02, 0x44, 0x02, 0x46, 0x03}};
    }
    return {i1, i2, i3, i1, i4, i3, i5, i1, i5};
}

Bytes ypooPollFrame(unsigned counter) {
    if (counter % 2 == 0) return {0x02, 0x42, 0x42, 0x03};
    return {0x02, 0x43, 0x01, 0x42, 0x03};
}

Bytes ypooResistanceFrame(int level) {
    const auto l = static_cast<std::uint8_t>(level);
    return {0x02, 0x44, 0x05, l, 0x00, static_cast<std::uint8_t>(0x39 + l), 0x03};
}

Bytes gymstickResistanceFrame(int level, int current) {
    int delta = level - current;
    if (delta > 127) delta = 127;
    if (delta < -128) delta = -128;
    Bytes out{0x02, 0x44, 0x05, static_cast<std::uint8_t>(level), static_cast<std::uint8_t>(static_cast<std::int8_t>(delta)),
              0x00, 0x03};
    out[5] = static_cast<std::uint8_t>(out[1] ^ out[2] ^ out[3] ^ out[4]);
    return out;
}

Bytes ftmsResistanceFrame(int level) { return {0x04, static_cast<std::uint8_t>((level * 10) & 0xff)}; }

Bytes ftmsInclinationFrame(float percent) {
    // Upstream casts to uint16 before scaling, dropping the fraction.
    const auto value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(percent) * 10);
    return {0x03, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8)};
}

bool decodeYpooCross(Kind kind, const std::uint8_t* d, std::size_t length, CrossData& out) {
    out = {};
    if (!d || length < 3) return false;
    const bool group = isYpooFtmsGroup(kind);
    const bool isTrue = kind == Kind::YpooTrue;
    const std::uint32_t flags = d[0] | (d[1] << 8) | (static_cast<std::uint32_t>(d[2]) << 16);
    auto& m = out.metrics;
    std::size_t i = 3;
    auto has = [&](std::size_t n) { return i + n <= length; };
    if (!(flags & 0x0001)) {
        if (!has(2)) return anyValid(m);
        if (!isTrue && group) set(m.speed, le16(d + i) / 100.0F);
        i += 2;
    }
    if (flags & 0x0002) {
        if (!has(2)) return anyValid(m);
        const float average = le16(d + i) / 100.0F;
        // Generic/SCH411/FEIER keep the instantaneous speed; others report the average field.
        if (isTrue || !group || kind == Kind::YpooSoleE25) set(m.speed, average);
        i += 2;
    }
    if (flags & 0x0004) {
        if (!has(3)) return anyValid(m);
        if (!group) set(m.distance, static_cast<float>(le24(d + i)));
        i += 3;
    }
    if (flags & 0x0008) {
        if (!has(4)) return anyValid(m);
        if (group) {
            const unsigned raw = le16(d + i);
            if (!isTrue || raw != 0xffff) set(m.cadence, raw / (isTrue ? 1.0F : 2.0F));
        }
        i += 4;
    }
    if (flags & 0x0010) {
        if (!has(2)) return anyValid(m);
        out.hasStride = true;
        out.stride = static_cast<std::uint16_t>(le16(d + i));
        set(m.strideCount, out.stride);
        i += 2;
    }
    if (flags & 0x0020) i += 4;
    if (flags & 0x0040) {
        if (!has(4)) return anyValid(m);
        set(m.incline, le16(d + i) / 10.0F);
        i += 4;
    }
    if (i + 1 >= length) return anyValid(m) || out.hasStride;
    if (flags & 0x0080) {
        if (!has(2)) return anyValid(m);
        set(m.resistance, static_cast<float>(le16(d + i)) / (isTrue ? 10.0F : 1.0F));
        i += 2;
    }
    if (flags & 0x0100) {
        if (!has(2)) return anyValid(m);
        set(m.power, static_cast<float>(le16(d + i)) / (group ? 1.0F : 100.0F));
        i += 2;
    }
    // Upstream skips average power on the generic FTMS variants (console bug).
    if ((flags & 0x0200) && has(2) && (!group || isTrue)) i += 2;
    if ((flags & 0x0400) && has(5)) i += 5;
    if (kind == Kind::YpooSch411 && length > 23) {
        if (d[23]) set(m.heartRate, d[23]);
    } else if ((flags & 0x0800) && has(1)) {
        if (d[i] != 0xff && d[i] != 0) set(m.heartRate, d[i]);
    }
    if ((flags & 0x0800) && has(1)) ++i;
    if (flags & 0x1000) {
        if (!has(1)) return anyValid(m);
        if (group && kind != Kind::YpooSoleE25) set(m.metabolicEquivalent, d[i] / 10.0F);
        ++i;
    }
    return anyValid(m);
}

bool decodeIconsole(const std::uint8_t* d, std::size_t length, MetricSet& out) {
    out = {};
    if (!d || length != 15) return false;
    set(out.speed, static_cast<float>((d[10] << 8) | d[9]) / 100.0F);
    set(out.cadence, d[6]);
    return true;
}

bool decodeBikeData(bool bh, const std::uint8_t* d, std::size_t length, MetricSet& out) {
    out = {};
    if (!d || length < 2) return false;
    const unsigned flags = le16(d);
    std::size_t i = 2;
    auto has = [&](std::size_t n) { return i + n <= length; };
    if (!(flags & 0x0001)) {
        if (!has(2)) return anyValid(out);
        if (!bh) set(out.speed, le16(d + i) / 100.0F);
        i += 2;
    }
    if (flags & 0x0002) i += 2;
    if (flags & 0x0004) {
        if (!has(2)) return anyValid(out);
        const float cadence = le16(d + i) / 2.0F;
        set(out.cadence, cadence);
        if (bh) set(out.speed, cadence / 10.0F);  // upstream: Speed = Cadence / 10
        i += 2;
    }
    if (flags & 0x0008) i += 2;
    if (flags & 0x0010) {
        if (!has(3)) return anyValid(out);
        set(out.distance, static_cast<float>(le24(d + i)));
        i += 3;
    }
    if (flags & 0x0020) {
        if (!has(2)) return anyValid(out);
        set(out.resistance, static_cast<float>(le16(d + i)));
        i += 2;
    }
    if (flags & 0x0040) {
        if (!has(2)) return anyValid(out);
        set(out.power, static_cast<float>(le16(d + i)));
        i += 2;
    }
    if (flags & 0x0080) i += 2;
    if ((flags & 0x0100) && length > i + 1) {
        set(out.totalEnergy, static_cast<float>(le16(d + i)));
        i += 5;
    }
    if ((flags & 0x0200) && length > i && d[i]) set(out.heartRate, d[i]);
    return anyValid(out);
}

// ---- soleelliptical ------------------------------------------------------
std::vector<Bytes> soleInitFrames(bool e55) {
    const Bytes i1{0x5b, 0x01, 0xf0, 0x5d};
    const Bytes i2{0x5b, 0x02, 0x03, 0x01, 0x5d};
    if (!e55) {
        // initData3 bytes 4/6 carry upstream default age 35 and weight 75 kg in lb (165).
        return {i1, i1, i1, i2, i2,
                {0x5b, 0x06, 0x07, 0x01, 35, 0x00, 165, 0xaa, 0x5d},
                {0x5b, 0x03, 0x08, 0x10, 0x01, 0x5d},
                {0x5b, 0x05, 0x04, 0xff, 0x00, 0x00, 0x00, 0x5d},
                {0x5b, 0x02, 0x02, 0x02, 0x5d},
                {0x5b, 0x02, 0x03, 0x04, 0x5d}};
    }
    const Bytes i3{0x5b, 0x04, 0x00, 0x09, 0x4f, 0x4b, 0x5d};
    const Bytes i4{0x5b, 0x04, 0x00, 0x10, 0x4f, 0x4b, 0x5d};
    const Bytes i8{0x5b, 0x02, 0x02, 0x02, 0x5d};
    const Bytes i10{0x5b, 0x02, 0x03, 0x04, 0x5d};
    return {i1, i1, i1, i2, i2, i3, i3, i4, i4,
            {0x5b, 0x06, 0x07, 0x01, 0x23, 0x00, 0x9b, 0x43, 0x5d},
            {0x5b, 0x03, 0x08, 0x10, 0x01, 0x5d},
            {0x5b, 0x05, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x5d},
            i8, i8, i4, i4,
            {0x5b, 0x04, 0x00, 0x40, 0x4f, 0x4b, 0x5d},
            i10, i10, i10};
}

Bytes solePollFrame(bool e55, unsigned counter) {
    // Non-E55 upstream sends noOpData in both slots (sizeof(noOpData1) typo).
    if (!e55) return {0x5b, 0x04, 0x00, 0x10, 0x4f, 0x4b, 0x5d};
    constexpr std::uint8_t codes[7] = {0x10, 0x06, 0x12, 0x13, 0x06, 0x12, 0x13};
    return {0x5b, 0x04, 0x00, codes[counter % 7], 0x4f, 0x4b, 0x5d};
}

std::vector<Bytes> soleStepFrames(bool up) {
    const Bytes ack{0x5b, 0x04, 0x00, 0xf1, 0x4f, 0x4b, 0x5d};
    if (up) return {ack, {0x5b, 0x02, 0xf1, 0x02, 0x5d}};
    return {{0x5b, 0x02, 0xf1, 0x03, 0x5d}, ack};
}

bool decodeSole(bool xe88, const std::uint8_t* d, std::size_t length, SoleFrame& out) {
    out = {};
    if (!d) return false;
    if (length == 5 && d[1] == 0x02 && (d[2] == 0x13 || d[2] == 0x12)) {
        out.type = d[2] == 0x13 ? SoleFrame::Type::Resistance : SoleFrame::Type::Incline;
        out.level = d[3] + 1;
        if (d[2] == 0x13) set(out.metrics.resistance, static_cast<float>(out.level));
        else set(out.metrics.incline, static_cast<float>(out.level));
        return true;
    }
    if (length < 20) return false;
    out.type = SoleFrame::Type::Data;
    auto& m = out.metrics;
    // Upstream default sole_treadmill_miles = true: console speed is mph.
    set(m.speed, static_cast<float>((d[11] << 8) | d[12]) / 100.0F * 1.60934F);
    set(m.totalEnergy, static_cast<float>((d[7] << 8) | d[8]) / 10.0F);
    set(m.power, static_cast<float>((d[13] << 8) | d[14]));
    set(m.cadence, d[10]);
    const std::uint8_t hr = d[xe88 ? 9 : 18];
    if (hr) set(m.heartRate, hr);
    return true;
}

// ---- octaneelliptical ----------------------------------------------------
std::vector<Bytes> octaneInitFrames() {
    return {{0xa5, 0x04, 0x21, 0x05, 0x01, 0xd9}, {0xa5, 0x04, 0x21, 0x54, 0x01, 0x8a},
            {0xa5, 0x04, 0x21, 0x70, 0x01, 0x6e}};
}

namespace {
int indexOf(const std::uint8_t* d, std::size_t length, std::uint8_t a, std::uint8_t b) {
    for (std::size_t i = 0; i + 1 < length; ++i)
        if (d[i] == a && d[i + 1] == b) return static_cast<int>(i);
    return -1;
}
}  // namespace

bool decodeOctane(const std::uint8_t* d, std::size_t length, MetricSet& out) {
    out = {};
    if (!d || length != 20) return false;
    int at = indexOf(d, length, 0x01, 0x09);
    if (at >= 0 && static_cast<std::size_t>(at + 3) < length) set(out.resistance, d[at + 2]);
    at = indexOf(d, length, 0x02, 0x11);
    if (at >= 0 && static_cast<std::size_t>(at + 3) < length && d[at + 2]) set(out.heartRate, d[at + 2]);
    int i = indexOf(d, length, 0x01, 0x07) + 2;
    if (static_cast<std::size_t>(i + 1) >= length || i <= 1) {
        if (d[0] != 0xa5) return anyValid(out);
        if (d[6] == 0x07) i = 7;
        else if (d[15] == 0x07) i = 16;
        else return anyValid(out);
    }
    const float cadence = d[i];
    set(out.cadence, cadence);
    // Upstream estimate from cadence (stride length 52.07 cm, factor 1.1218).
    set(out.speed, (((cadence / 2.0F) * 52.07F * 60.0F) / 10000.0F) * 1.1218F);
    return true;
}

// ---- smartrowrower -------------------------------------------------------
Bytes smartRowInitBytes() { return {0x24, 0x0d, 0x56, 0x40, 0x0d}; }

Bytes smartRowChallengeResponse(const std::string& keylock) {
    const Bytes fallback{0x23};
    std::size_t first = 0;
    std::size_t last = keylock.size();
    while (first < last && std::isspace(static_cast<unsigned char>(keylock[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(keylock[last - 1]))) --last;
    std::string challenge = keylock.substr(first, last - first);
    const std::size_t at = challenge.find("KEYLOCK=");
    if (at != std::string::npos) challenge = challenge.substr(at);
    if (challenge.size() < 16) return fallback;
    unsigned sum = 0;
    for (std::size_t i = 0; i < 14; ++i) sum += static_cast<std::uint8_t>(challenge[i]);
    char text[16];
    std::snprintf(text, sizeof(text), "%04X", sum);
    const std::string calculated(text);
    const std::string checksum = upper(challenge.substr(14, 2));
    if (calculated.substr(calculated.size() - 2) != checksum) return fallback;
    unsigned seed = 0;
    for (std::size_t i = 10; i < 14; ++i) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(challenge[i])));
        if (c >= '0' && c <= '9') seed = seed * 16 + static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') seed = seed * 16 + static_cast<unsigned>(c - 'a' + 10);
        else return fallback;
    }
    const std::uint32_t response = (static_cast<std::uint32_t>(seed) * 17923U) / 256U;
    std::snprintf(text, sizeof(text), "%06x", static_cast<unsigned>(response));
    const std::string hex(text);
    Bytes out{0x0d};
    for (char c : hex.substr(hex.size() - 4)) out.push_back(static_cast<std::uint8_t>(c));
    out.push_back(0x0d);
    return out;
}

namespace {
// atoi(packet.mid(from, count)).
int atoiMid(const std::uint8_t* d, std::size_t from, std::size_t count) {
    char text[8] = {};
    for (std::size_t i = 0; i < count && i < sizeof(text) - 1; ++i) text[i] = static_cast<char>(d[from + i]);
    return std::atoi(text);
}
}  // namespace

bool decodeSmartRow(bool v3, const std::uint8_t* raw, std::size_t length, MetricSet& out) {
    out = {};
    if (!raw || length != 17) return false;
    std::uint8_t d[17];
    std::memcpy(d, raw, sizeof(d));
    if (v3 && std::strchr("abcdefxyz", d[0]) && d[0]) {
        for (std::size_t i = 1; i <= 5; ++i) d[i] = static_cast<std::uint8_t>((raw[i] & 0x0f) | 0x30);
    }
    // Upstream: Distance = atoi(mid(1,5)) / 1000 km for every 17-byte packet.
    set(out.distance, static_cast<float>(atoiMid(d, 1, 5)));
    switch (d[0]) {
        case 'c': set(out.power, static_cast<float>(atoiMid(d, 6, 3))); break;
        case 'd':
            set(out.cadence, atoiMid(d, 6, 3) / 10.0F);
            set(out.strokeCount, static_cast<float>(atoiMid(d, 9, 4)));
            break;
        case 'e': {
            const int pace = atoiMid(d, 6, 1) * 60 + atoiMid(d, 7, 2);
            set(out.pace, pace > 0 ? static_cast<float>(pace) : 0.0F);
            set(out.speed, pace > 0 ? (500.0F * 100.0F / static_cast<float>(pace)) * 0.036F : 0.0F);
            break;
        }
        default: break;
    }
    return true;
}

// ---- sunnyfitstepper -----------------------------------------------------
std::vector<Bytes> sunnyFitInitFrames() {
    return {{0x5a, 0x02, 0x00, 0x08, 0x07, 0xa0, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0xe6, 0xa5},
            {0x5a, 0x02, 0x00, 0x03, 0x02, 0xa3, 0x00, 0xaa, 0xa5},
            {0x5a, 0x02, 0x00, 0x03, 0x02, 0xb4, 0x00, 0xbb, 0xa5},
            {0x5a, 0x04, 0x00, 0x03, 0x02, 0xf1, 0x00, 0xfa, 0xa5}};
}

bool decodeSunnyFit(const std::uint8_t* d, std::size_t length, MetricSet& out) {
    out = {};
    if (!d || length != 32 || d[0] != 0x5a) return false;
    set(out.cadence, static_cast<float>(le16(d + 6)));
    const unsigned steps = le24(d + 10);
    set(out.stepCount, static_cast<float>(steps));
    // Upstream elevationAcc = steps * 0.20 m.
    set(out.positiveElevationGain, static_cast<float>(steps) * 0.20F);
    return true;
}

// ---- concept2skierg ------------------------------------------------------
bool decodeSkiErg(const std::uint8_t* d, std::size_t length, bool& active, MetricSet& out) {
    out = {};
    if (!d || !length) return false;
    switch (d[0]) {
        case 0x31:
            if (length < 20) return false;
            set(out.distance, static_cast<float>(le24(d + 4)) / 10.0F);
            active = d[10] != 0;
            if (!active) {
                set(out.speed, 0);
                set(out.cadence, 0);
                set(out.power, 0);  // upstream watts() is 0 when cadence is 0
            }
            set(out.resistance, d[19]);  // drag factor
            return true;
        case 0x32:
            if (length < 19) return false;
            if (active) {
                set(out.speed, static_cast<float>(le16(d + 4)) * 0.0036F);
                set(out.cadence, d[6]);
            }
            if (d[7] != 0xff && d[7] != 0) set(out.heartRate, d[7]);
            return true;
        case 0x33:
            if (length < 19) return false;
            set(out.totalEnergy, static_cast<float>(le16(d + 5)));
            return true;
        case 0x35:
            if (length < 19) return false;
            set(out.strokeCount, static_cast<float>(le16(d + 17)));
            return true;
        case 0x36:
            if (length < 16) return false;
            if (active) set(out.power, static_cast<float>(le16(d + 4)));
            return true;
        default: return false;
    }
}

// ---- matchers ------------------------------------------------------------
bool matchesProformRower(const ble::Advertisement& a) {
    // Same vendor evidence rule as the other iFit ports.
    return prefix(upper(a.name), "I_RW") && (!ftms(a) || a.hasService(uuid(kProformService)));
}
bool matchesBhElliptical(const ble::Advertisement& a) { return prefix(upper(a.name), "B01_"); }
bool matchesYpooU3(const ble::Advertisement& a) { return any(upper(a.name), {"YPOO-U3-", "MRK-E"}); }
bool matchesYpooFtms(const ble::Advertisement& a) {
    const auto name = upper(a.name);
    return any(name, {"SCH_590E", "KETTLER ", "CARDIOPOWER EEGO", "MYELLIPTICAL ", "MX-AS "}) ||
           (prefix(name, "SF-") && positiveNumber(a.name.substr(3))) || (prefix(name, "E35") && ftms(a));
}
bool matchesYpooSch411(const ble::Advertisement& a) { return prefix(upper(a.name), "SCH411/510E"); }
bool matchesYpooFeier(const ble::Advertisement& a) { return prefix(upper(a.name), "FEIER-EM-"); }
bool matchesYpooSoleE25(const ble::Advertisement& a) { return prefix(upper(a.name), "E25") && ftms(a); }
bool matchesYpooTrue(const ble::Advertisement& a) {
    const auto name = upper(a.name);
    const char* p = "TRUE ELLIPTICAL ";
    if (!prefix(name, p)) return false;
    const std::string rest = name.substr(std::strlen(p));
    if (rest.size() < 3) return false;
    for (char c : rest) if (c < '0' || c > '9') return false;
    return true;
}
bool matchesSoleElliptical(const ble::Advertisement& a) {
    return !ftms(a) && any(upper(a.name), {"E95S", "E25", "E35", "E55", "E95", "E98", "XG400"});
}
bool matchesXterraXe88(const ble::Advertisement& a) { return !ftms(a) && prefix(upper(a.name), "XE88"); }
bool matchesOctane(const ble::Advertisement& a) { return !ftms(a) && prefix(upper(a.name), "Q37"); }
bool matchesSmartRow(const ble::Advertisement& a) {
    const auto name = upper(a.name);
    return !ftms(a) && prefix(name, "SMARTROW") && !prefix(name, "SMARTROWER");
}
bool matchesSunnyFit(const ble::Advertisement& a) { return !ftms(a) && prefix(upper(a.name), "SF-S"); }
bool matchesSkiErg(const ble::Advertisement& a) {
    const auto name = upper(a.name);
    return !ftms(a) && prefix(name, "PM5") && name.size() >= 6 && name.compare(name.size() - 3, 3, "SKI") == 0;
}

namespace {
bool manualOnly(const ble::Advertisement&) { return false; }

template <Kind K>
std::unique_ptr<SourceProtocol> make() {
    return std::unique_ptr<SourceProtocol>(new VendorCardioSource(K));
}

constexpr MachineProfile kCross = MachineProfile::CrossTrainer;
constexpr MachineProfile kRower = MachineProfile::Rower;

// Order is persisted (ids) and indexed by Kind: append only.
const ProtocolInfo kDescriptors[] = {
    {"proform-rower", "ProForm / NordicTrack iFit rower", kMatchPriority, matchesProformRower, make<Kind::ProformRower>, true, false, kRower},
    {"proform-750r", "ProForm 750R rower (manual)", 0, manualOnly, make<Kind::Proform750r>, true, false, kRower},
    {"proform-sport-rl", "ProForm Sport RL rower (manual)", 0, manualOnly, make<Kind::ProformSportRl>, true, false, kRower},
    {"bh-elliptical", "BH Fitness elliptical", kMatchPriority, matchesBhElliptical, make<Kind::BhElliptical>, true, false, kCross},
    {"ypoo-u3", "YPOO U3 / Merach elliptical", kMatchPriority, matchesYpooU3, make<Kind::YpooU3>, true, false, kCross},
    {"ypoo-ftms-el", "FTMS elliptical (E35, Kettler, Schwinn 590E...)", kMatchPriority, matchesYpooFtms, make<Kind::YpooFtms>, true, false, kCross},
    {"ypoo-sch411", "Schwinn 411/510E elliptical", kMatchPriority, matchesYpooSch411, make<Kind::YpooSch411>, true, false, kCross},
    {"ypoo-feier", "FEIER elliptical (FTMS start)", kMatchPriority, matchesYpooFeier, make<Kind::YpooFeier>, true, false, kCross},
    {"ypoo-sole-e25", "Sole E25 FTMS elliptical", kMatchPriority, matchesYpooSoleE25, make<Kind::YpooSoleE25>, true, false, kCross},
    {"ypoo-true-el", "TRUE elliptical", kMatchPriority, matchesYpooTrue, make<Kind::YpooTrue>, true, false, kCross},
    {"ypoo-iconsole", "iConsole elliptical (manual)", 0, manualOnly, make<Kind::YpooIconsole>, true, false, kCross},
    {"gymstick-gx60", "Gymstick GX6.0 elliptical (manual)", 0, manualOnly, make<Kind::YpooGymstick>, true, false, kCross},
    {"sole-elliptical", "Sole elliptical (E25/E35/E55/E95/E98)", kMatchPriority, matchesSoleElliptical, make<Kind::SoleElliptical>, true, false, kCross},
    {"xterra-xe88", "Xterra XE88 elliptical", kMatchPriority, matchesXterraXe88, make<Kind::XterraXe88>, true, false, kCross},
    {"sole-e55", "Sole E55 elliptical (manual)", 0, manualOnly, make<Kind::SoleE55>, true, false, kCross},
    {"octane-q37", "Octane Q37 elliptical", kMatchPriority, matchesOctane, make<Kind::OctaneQ37>, false, false, kCross},
    {"smartrow", "SmartRow rower", kMatchPriority, matchesSmartRow, make<Kind::SmartRow>, false, false, kRower},
    {"sunnyfit-stepper", "Sunny Health stepper", kMatchPriority, matchesSunnyFit, make<Kind::SunnyFitStepper>, false, false, MachineProfile::StepClimber},
    {"concept2-skierg", "Concept2 PM5 SkiErg", kMatchPriority, matchesSkiErg, make<Kind::Concept2SkiErg>, false, false, kRower},
};
constexpr std::size_t kDescriptorCount = sizeof(kDescriptors) / sizeof(kDescriptors[0]);
static_assert(kDescriptorCount == static_cast<std::size_t>(Kind::Concept2SkiErg) + 1, "descriptor per Kind");

}  // namespace

std::size_t descriptorCount() { return kDescriptorCount; }
const ProtocolInfo& descriptorAt(std::size_t index) { return kDescriptors[index < kDescriptorCount ? index : 0]; }

}  // namespace vendor_cardio

using vendor_cardio::Kind;
namespace vc = vendor_cardio;

VendorCardioSource::VendorCardioSource(Kind kind) : kind_(kind) {}

const char* VendorCardioSource::id() const { return vc::descriptorAt(static_cast<std::size_t>(kind_)).id; }
MachineProfile VendorCardioSource::profile() const {
    return vc::descriptorAt(static_cast<std::size_t>(kind_)).profile;
}

bool VendorCardioSource::ypooFtmsGroup() const { return vc::isYpooFtmsGroup(kind_); }
bool VendorCardioSource::ypooProprietary() const {
    return kind_ == Kind::YpooU3 || kind_ == Kind::YpooIconsole || kind_ == Kind::YpooGymstick;
}

ble::ConnectPlan VendorCardioSource::plan() const {
    ble::ConnectPlan p;
    switch (kind_) {
        case Kind::ProformRower: case Kind::Proform750r: case Kind::ProformSportRl:
            p.subscribe(vc::uuid(vc::kProformService), vc::uuid(vc::kProformNotify));
            break;
        case Kind::BhElliptical:
            p.subscribe(vc::kFtms, vc::kBikeData);
            p.subscribe(vc::kFtms, vc::kControlPoint, true);
            p.subscribe(vc::kHrService, vc::kHrMeasurement);
            break;
        case Kind::YpooIconsole: case Kind::YpooGymstick:
            // Upstream subscribes every notifying characteristic of service 0xFFF0.
            for (std::uint16_t c : {0xfff1, 0xfff3, 0xfff4}) p.subscribe(vc::kFff0, vc::u16(c));
            break;
        case Kind::SoleElliptical: case Kind::XterraXe88: case Kind::SoleE55:
            p.subscribe(vc::uuid(vc::kSoleService), vc::uuid(vc::kSoleNotify));
            break;
        case Kind::OctaneQ37: p.subscribe(vc::uuid(vc::kOctaneService), vc::uuid(vc::kOctaneNotify)); break;
        case Kind::SmartRow: p.subscribe(vc::u16(0x1234), vc::u16(0x1236)); break;
        case Kind::SunnyFitStepper:
            p.subscribe(vc::uuid(vc::kSunnyService), vc::uuid(vc::kSunnyNotify1));
            p.subscribe(vc::uuid(vc::kSunnyService), vc::uuid(vc::kSunnyNotify4));
            break;
        case Kind::Concept2SkiErg: p.subscribe(vc::uuid(vc::kConcept2Service), vc::uuid(vc::kConcept2Multiplex)); break;
        default:  // ypoo FTMS-layout variants and U3
            p.subscribe(vc::kFtms, vc::kCrossData);
            p.subscribe(vc::kFtms, vc::kBikeData);
            p.subscribe(vc::kHrService, vc::kHrMeasurement);
            p.subscribe(vc::kFtms, vc::kControlPoint, true);
            break;
    }
    return p;
}

void VendorCardioSource::reset() {
    SourceProtocol::reset();
    backlog_.clear();
    spacingMs_ = lastSendMs_ = lastTickMs_ = stepMs_ = lastStrideMs_ = 0;
    sentOnce_ = ready_ = initQueued_ = awaitingStatus_ = strideSeen_ = v3_ = challengeRequested_ = false;
    counter_ = sec1_ = 0;
    target_ = current_ = -1;
    partial_.clear();
    frame_.clear();
    lastStride_ = 0;
    requested_ = 6;
    active_ = true;
}

void VendorCardioSource::pushTo(const ble::Uuid& service, const ble::Uuid& characteristic, const ble::Bytes& payload,
                                bool withResponse) {
    if (payload.empty() || backlog_.size() >= 64) return;
    backlog_.push_back({service, characteristic, payload, withResponse});
}

void VendorCardioSource::push(const ble::Bytes& payload, bool withResponse) {
    switch (kind_) {
        case Kind::ProformRower: case Kind::Proform750r: case Kind::ProformSportRl:
            pushTo(vc::uuid(vc::kProformService), vc::uuid(vc::kProformWrite), payload, withResponse);
            break;
        case Kind::SoleElliptical: case Kind::XterraXe88: case Kind::SoleE55:
            pushTo(vc::uuid(vc::kSoleService), vc::uuid(vc::kSoleWrite), payload, withResponse);
            break;
        case Kind::OctaneQ37: pushTo(vc::uuid(vc::kOctaneService), vc::uuid(vc::kOctaneWrite), payload, withResponse); break;
        case Kind::SmartRow: pushTo(vc::u16(0x1234), vc::u16(0x1235), payload, withResponse); break;
        case Kind::SunnyFitStepper: pushTo(vc::uuid(vc::kSunnyService), vc::uuid(vc::kSunnyWrite), payload, withResponse); break;
        case Kind::YpooU3: case Kind::YpooIconsole: case Kind::YpooGymstick:
            pushTo(vc::kFff0, vc::kFff2, payload, withResponse);
            break;
        default: pushTo(vc::kFtms, vc::kControlPoint, payload, withResponse); break;
    }
}

void VendorCardioSource::flush(std::uint32_t nowMs) {
    if (backlog_.empty()) return;
    auto send = [this]() {
        const Pending& next = backlog_.front();
        queueWrite(next.service, next.characteristic, next.payload, next.withResponse);
        backlog_.erase(backlog_.begin());
    };
    if (spacingMs_ == 0) {
        while (!backlog_.empty() && pendingWrites() < 8) send();
        return;
    }
    if (pendingWrites() != 0 || (sentOnce_ && nowMs - lastSendMs_ < spacingMs_)) return;
    send();
    sentOnce_ = true;
    lastSendMs_ = nowMs;
}

void VendorCardioSource::merge(const MetricSet& values, std::uint32_t nowMs) {
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        const auto metric = static_cast<Metric>(i);
        if (values[metric].valid) sample_[metric] = values[metric];
    }
    touch(nowMs);
}

bool VendorCardioSource::onConnected(std::uint32_t mask, std::uint32_t nowMs) {
    lastTickMs_ = nowMs;
    std::uint32_t required = 1U;
    if (ypooFtmsGroup() || kind_ == Kind::YpooU3) required = 3U;
    if (kind_ == Kind::YpooIconsole || kind_ == Kind::YpooGymstick) required = 7U;
    if (kind_ == Kind::SunnyFitStepper) required = 2U;
    if ((mask & required) == 0) {
        setError("vendor data characteristic unavailable");
        return false;
    }
    switch (kind_) {
        case Kind::ProformRower: case Kind::Proform750r: case Kind::ProformSportRl:
            spacingMs_ = 400;  // upstream sleeps 400 ms between init writes
            for (const auto& frame : vc::proformInitFrames(kind_)) push(frame);
            break;
        case Kind::BhElliptical: ready_ = true; break;  // upstream sends no init
        case Kind::YpooU3: case Kind::YpooGymstick:
            for (const auto& frame : vc::ypooInitFrames(kind_ == Kind::YpooGymstick)) push(frame);
            ready_ = initQueued_ = kind_ == Kind::YpooU3;
            if (kind_ == Kind::YpooGymstick) {
                // Gymstick implies iconsole: init waits for the first notification.
                backlog_.clear();
            }
            break;
        case Kind::YpooIconsole: break;
        case Kind::SoleElliptical: case Kind::XterraXe88: case Kind::SoleE55:
            for (const auto& frame : vc::soleInitFrames(kind_ == Kind::SoleE55)) push(frame);
            ready_ = true;
            break;
        case Kind::OctaneQ37: for (const auto& frame : vc::octaneInitFrames()) push(frame); break;
        case Kind::SmartRow:
            spacingMs_ = 200;  // one byte per upstream 200 ms update
            for (auto byte : vc::smartRowInitBytes()) push({byte}, false);
            break;
        case Kind::SunnyFitStepper: for (const auto& frame : vc::sunnyFitInitFrames()) push(frame); break;
        case Kind::Concept2SkiErg: break;
        default:  // ypoo FTMS variants: request control (+ start/resume for FEIER)
            push({0x00});
            if (kind_ == Kind::YpooFeier) push({0x07});
            ready_ = true;
            break;
    }
    flush(nowMs);
    return true;
}

void VendorCardioSource::ypooStrideCadence(std::uint16_t stride, std::uint32_t nowMs) {
    if (stride == lastStride_) return;
    if (!strideSeen_ || lastStride_ == 0) {
        strideSeen_ = true;
        lastStride_ = stride;
        lastStrideMs_ = nowMs;
        return;
    }
    float diff = static_cast<std::uint16_t>(stride - lastStride_);
    if (kind_ == Kind::YpooTrue) diff /= 10.0F;
    const float minutes = static_cast<float>(nowMs - lastStrideMs_) / 60000.0F;
    const bool enough = kind_ == Kind::YpooTrue ? diff >= 0.5F : diff >= 5.0F;
    if (minutes <= 0 || !enough || diff >= 1000) return;
    const float rpm = diff / minutes / 2.0F;
    if (rpm < 120) sample_.cadence = {rpm, true};
    lastStride_ = stride;
    lastStrideMs_ = nowMs;
}

void VendorCardioSource::ypooNotify(std::size_t index, const std::uint8_t* data, std::size_t length,
                                    std::uint32_t nowMs) {
    if (kind_ == Kind::YpooIconsole || kind_ == Kind::YpooGymstick) {
        if (!initQueued_) {
            initQueued_ = ready_ = true;
            for (const auto& frame : vc::ypooInitFrames(kind_ == Kind::YpooGymstick)) push(frame);
            flush(nowMs);
        }
        MetricSet values;
        if (vc::decodeIconsole(data, length, values)) merge(values, nowMs);
        return;
    }
    if (index == 2) {
        if (length > 1 && data[1]) {
            sample_.heartRate = {static_cast<float>(data[1]), true};
            touch(nowMs);
        }
        return;
    }
    if (index == 1) {
        MetricSet values;
        if (vc::decodeBikeData(false, data, length, values)) merge(values, nowMs);
        return;
    }
    if (index != 0) return;
    const std::uint8_t* frame = data;
    std::size_t frameLength = length;
    if (kind_ == Kind::YpooU3) {
        // Proprietary firmware splits one Cross Trainer frame into 18 + 17 bytes.
        if (length == 18) {
            partial_.assign(data, data + length);
            return;
        }
        if (length != 17 || partial_.size() != 18) {
            partial_.clear();
            return;
        }
        partial_.insert(partial_.end(), data, data + length);
        frame = partial_.data();
        frameLength = partial_.size();
    }
    vc::CrossData cross;
    const bool decoded = vc::decodeYpooCross(kind_, frame, frameLength, cross);
    if (kind_ == Kind::YpooU3) partial_.clear();
    if (!decoded) return;
    merge(cross.metrics, nowMs);
    if (cross.hasStride && !cross.metrics.cadence.valid) ypooStrideCadence(cross.stride, nowMs);
}

void VendorCardioSource::smartRowNotify(const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    const std::string text(reinterpret_cast<const char*>(data), length);
    if (text.find("V3.") != std::string::npos) {
        v3_ = true;
        if (!challengeRequested_) {
            challengeRequested_ = true;
            push({0x23}, false);
        }
    } else if (text.find("KEYLOCK") != std::string::npos) {
        v3_ = true;
        for (auto byte : vc::smartRowChallengeResponse(text)) push({byte}, false);
    }
    MetricSet values;
    if (vc::decodeSmartRow(v3_, data, length, values)) merge(values, nowMs);
}

void VendorCardioSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length,
                                  std::uint32_t nowMs) {
    if (!data || !length) return;
    MetricSet values;
    switch (kind_) {
        case Kind::ProformRower: case Kind::Proform750r: case Kind::ProformSportRl: {
            if (length != 20) return;
            if (data[0] == 0xff && data[1] == 0x11) {
                for (std::size_t i = 2; i <= 9; ++i) if (data[i]) return;
                const std::uint8_t cadence = data[12];
                const unsigned split = vc::le16(data + 13);
                if (cadence == 0 && split > 0) return;
                vc::set(values.cadence, cadence);
                vc::set(values.pace, static_cast<float>(split));
                vc::set(values.speed, split > 0 ? (60.0F / static_cast<float>(split)) * 30.0F : 0.0F);
                merge(values, nowMs);
                return;
            }
            if (data[0] != 0x00 || data[1] != 0x12 || data[2] != 0x01 || data[3] != 0x04 || data[4] != 0x02 ||
                (data[5] != 0x2e && data[5] != 0x30 && data[5] != 0x31))
                return;
            bool blank = true;
            for (std::size_t i = 12; i < 20; ++i) blank = blank && data[i] == 0xff;
            if (blank) return;
            current_ = vc::proformResistanceFromByte(data[11]);
            vc::set(values.power, static_cast<float>(vc::le16(data + 12)));
            vc::set(values.resistance, static_cast<float>(current_));
            vc::set(values.distance, static_cast<float>(vc::le16(data + 14)));
            merge(values, nowMs);
            return;
        }
        case Kind::BhElliptical:
            if (index == 2) {
                if (length > 1 && data[1]) {
                    sample_.heartRate = {static_cast<float>(data[1]), true};
                    touch(nowMs);
                }
            } else if (index == 0 && vc::decodeBikeData(true, data, length, values)) {
                merge(values, nowMs);
            }
            return;
        case Kind::SoleElliptical: case Kind::XterraXe88: case Kind::SoleE55: {
            vc::SoleFrame frame;
            if (!vc::decodeSole(kind_ == Kind::XterraXe88, data, length, frame)) return;
            if (frame.type == vc::SoleFrame::Type::Resistance) {
                current_ = frame.level;
                awaitingStatus_ = false;
                if (current_ == target_) target_ = -1;
            } else if (frame.type == vc::SoleFrame::Type::Data && current_ < 1) {
                current_ = 1;  // upstream defaults the unknown level to 1
            }
            merge(frame.metrics, nowMs);
            return;
        }
        case Kind::OctaneQ37:
            if (vc::decodeOctane(data, length, values)) merge(values, nowMs);
            return;
        case Kind::SmartRow: smartRowNotify(data, length, nowMs); return;
        case Kind::SunnyFitStepper:
            if (index != 1) return;
            if (length == 20 && data[0] == 0x5a) {
                frame_.assign(data, data + length);
                return;
            }
            if (length == 12 && frame_.size() == 20) {
                frame_.insert(frame_.end(), data, data + length);
                if (vc::decodeSunnyFit(frame_.data(), frame_.size(), values)) merge(values, nowMs);
            }
            frame_.clear();
            return;
        case Kind::Concept2SkiErg:
            if (vc::decodeSkiErg(data, length, active_, values)) merge(values, nowMs);
            return;
        default: ypooNotify(index, data, length, nowMs); return;
    }
}

void VendorCardioSource::soleStep(std::uint32_t nowMs) {
    if (awaitingStatus_ && nowMs - stepMs_ > 3000) {
        // No resistance status after a step: stop instead of stepping blindly.
        awaitingStatus_ = false;
        target_ = -1;
    }
    if (target_ < 0 || current_ < 1 || awaitingStatus_ || target_ == current_) return;
    for (const auto& frame : vc::soleStepFrames(current_ < target_)) push(frame);
    awaitingStatus_ = true;
    stepMs_ = nowMs;
}

void VendorCardioSource::proformPoll() {
    const unsigned slot = counter_;
    // Upstream drops the request at its slot even when it equals the current level.
    auto resistance = [this]() {
        if (target_ >= 0 && target_ != current_) {
            if (kind_ != Kind::ProformRower) push(vc::proformUnlockFrame());
            push(vc::proformResistanceFrame(kind_, target_));
        }
        target_ = -1;
    };
    if (kind_ == Kind::ProformSportRl && slot == 3) resistance();
    push(vc::proformPollFrame(kind_, slot));
    if ((kind_ == Kind::ProformRower && slot == 3) || (kind_ == Kind::Proform750r && slot == 5)) resistance();
    counter_ = (counter_ + 1) % 6;
}

void VendorCardioSource::tick(std::uint32_t nowMs) {
    switch (kind_) {
        case Kind::ProformRower: case Kind::Proform750r: case Kind::ProformSportRl:
            if (!ready_) {
                flush(nowMs);
                if (backlog_.empty() && pendingWrites() == 0) {
                    ready_ = true;
                    spacingMs_ = 0;
                    lastTickMs_ = nowMs;
                }
                return;
            }
            if (nowMs - lastTickMs_ >= 200 && backlog_.empty()) {
                lastTickMs_ = nowMs;
                proformPoll();
            }
            break;
        case Kind::SoleElliptical: case Kind::XterraXe88: case Kind::SoleE55:
            if (nowMs - lastTickMs_ >= 300 && backlog_.empty()) {
                lastTickMs_ = nowMs;
                if (sec1_++ == 3) {
                    sec1_ = 0;
                } else {
                    push(vc::solePollFrame(kind_ == Kind::SoleE55, counter_));
                    counter_ = kind_ == Kind::SoleE55 ? (counter_ + 1) % 7 : (counter_ + 1) % 2;
                }
                soleStep(nowMs);
            }
            break;
        case Kind::YpooU3: case Kind::YpooIconsole: case Kind::YpooGymstick:
            if (nowMs - lastTickMs_ >= 200 && backlog_.empty()) {
                lastTickMs_ = nowMs;
                push(vc::ypooPollFrame(counter_));
                counter_ = (counter_ + 1) % 2;
            }
            break;
        case Kind::SmartRow:
            if (nowMs - lastTickMs_ < 200) break;
            lastTickMs_ = nowMs;
            if (!backlog_.empty()) break;
            ready_ = true;
            if (sec1_++ >= 10) {
                sec1_ = 0;
                push({0x24}, false);
            }
            break;
        default: break;
    }
    flush(nowMs);
}

bool VendorCardioSource::control(const ControlRequest& request, std::uint32_t nowMs) {
    if (!std::isfinite(request.value) || !capabilities().supports(request.control)) return false;
    if (request.control == Control::Incline) {
        float percent = request.value < 1.0F ? 1.0F : request.value;  // upstream minimum (E35)
        if (percent > kInclineBounds.maximum) percent = kInclineBounds.maximum;
        push(vc::ftmsInclinationFrame(percent));
        flush(nowMs);
        return true;
    }
    switch (kind_) {
        case Kind::ProformRower: case Kind::Proform750r: case Kind::ProformSportRl:
            target_ = percentToLevel(request.value, 1, 24);
            return true;
        case Kind::BhElliptical: push(vc::bhResistanceFrame(percentToLevel(request.value, 1, 72))); break;
        case Kind::YpooU3: case Kind::YpooIconsole:
            push(vc::ypooResistanceFrame(percentToLevel(request.value, 1, 72)));
            break;
        case Kind::YpooGymstick: {
            const int level = percentToLevel(request.value, 1, 72);
            push(vc::gymstickResistanceFrame(level, requested_));
            requested_ = level;
            break;
        }
        case Kind::SoleElliptical: case Kind::SoleE55:
            target_ = percentToLevel(request.value, 1, 20);
            return true;
        case Kind::XterraXe88:
            target_ = percentToLevel(request.value, 1, 32);
            return true;
        default:
            // Upstream writes level * 10 into one byte; levels above 25 would wrap.
            push(vc::ftmsResistanceFrame(percentToLevel(request.value, 1, 25)));
            break;
    }
    flush(nowMs);
    return true;
}

TrainerCapabilities VendorCardioSource::capabilities() const {
    TrainerCapabilities caps;
    caps.metrics = observedMetrics();
    const bool resistance = ready_ && vc::descriptorAt(static_cast<std::size_t>(kind_)).controllable;
    caps.setControl(Control::Resistance, resistance);
    // FTMS inclination only where upstream inclinationAvailableByHardware() is true.
    const bool incline = ypooFtmsGroup() && kind_ != Kind::YpooTrue;
    caps.setControl(Control::Incline, ready_ && incline);
    return caps;
}

}  // namespace protocols
}  // namespace smartfitness
