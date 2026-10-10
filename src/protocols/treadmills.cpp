// GPL-3.0, Copyright Roberto Viola and qdomyos-zwift contributors.
// Ported from https://github.com/cagnulein/qdomyos-zwift at
// ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/horizontreadmill/horizontreadmill.{cpp,h}
// src/devices/spirittreadmill/spirittreadmill.cpp
// src/devices/solef80treadmill/solef80treadmill.cpp
// src/devices/kingsmithr1protreadmill/kingsmithr1protreadmill.cpp
// src/devices/kingsmithr2treadmill/kingsmithr2treadmill.{cpp,h}
// src/devices/bluetooth.cpp

#include "protocols/treadmills.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace smartfitness {
namespace protocols {
namespace {
using ble::Bytes;
using treadmills::Model;
using treadmills::Cipher;
using treadmills::R2Service;

std::string upper(std::string name) {
    for (char& c : name) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return name;
}
bool prefix(const std::string& name, const char* p) { return name.compare(0, std::char_traits<char>::length(p), p) == 0; }
bool any(const std::string& name, std::initializer_list<const char*> prefixes) {
    for (const char* p : prefixes) if (prefix(name, p)) return true;
    return false;
}
unsigned le16(const Bytes& b, std::size_t at) { return b[at] | (static_cast<unsigned>(b[at + 1]) << 8); }
void put16(Bytes& b, std::size_t at, unsigned value) {
    b[at] = static_cast<std::uint8_t>(value);
    b[at + 1] = static_cast<std::uint8_t>(value >> 8);
}
Bytes bracket(std::uint8_t command, const Bytes& payload = {}) {
    Bytes b{0x5b, static_cast<std::uint8_t>(payload.size() + 1), command};
    b.insert(b.end(), payload.begin(), payload.end());
    b.push_back(0x5d);
    return b;
}
Bytes consoleAck(std::uint8_t code) { return bracket(0, {code, 0x4f, 0x4b}); }
const char* cipherTable(Cipher c) {
    switch (c) {
        case Cipher::V2: return "ZaCw4FGHIJqLhN+P9RMTU/WcY6ObDdefgEijklmnopQrsBuvVxXz1yA2t5078KS3=";
        case Cipher::V3: return "0aCw4FGHIJqLhN+P9RVTU/WcY6ObDdefgEijklmnopQrsBuvMxXz1yA2t5Z78KS3=";
        case Cipher::V4: return "ZaCw4FGHIJqLhN9P+RVTU/WcY6ObDdefgEijklmnopQrsBuvMxXz1yA2t5078KS3=";
        case Cipher::V5: return "iaCw4FGHIJqLhN+P9RVTU/WcY6ObDdefgEZjklmnopQrsBuvMxXz1yA2t5078KS3=";
        case Cipher::G1: return "ZaCw4FGHIJqLhN+P8RVTU/WcY6ObDdefgEijklmnopQrsBuvMxXz1yA2t5079KS3=";
        case Cipher::MXG: return "baCw4FGHIJqLhN+P9RVTU/WcY6OZDdefgEijklmnopQrsBuvMxXz1yA2t5078KS3=";
        default: return "SaCw4FGHIJqLhN+P9RVTU/WcY6ObDdefgEijklmnopQrsBuvMxXz1yA2t5078KZ3=";
    }
}
int charIndex(const char* table, std::uint8_t value) {
    for (int i = 0; i < 65; ++i) if (static_cast<std::uint8_t>(table[i]) == value) return i;
    return -1;
}
bool finiteNumber(const std::string& text, float& value) {
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    return input && input.eof() && std::isfinite(value);
}
std::string tenths(float value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}
}  // namespace

namespace treadmills {
bool matchesHorizon(const ble::Advertisement& a) {
    return !a.hasService(0x1826) &&
        any(upper(a.name), {"HORIZON", "YPTM-", "HZ_T101-", "HZ_7.0AT-", "AFG SPORT", "WLT2541",
                            "JFTMPARAGON", "PARAGON X"});
}
bool matchesSpirit(const ble::Advertisement& a) {
    return !a.hasService(0x1826) && any(upper(a.name), {"XT385", "XT485", "XT800", "XT900"});
}
bool matchesSole(const ble::Advertisement& a) {
    return !a.hasService(0x1826) && any(upper(a.name), {"F80", "F89", "F65", "TT8", "F63", "ST90",
                                                    "TRX7.5", "S77", "F85"});
}
bool matchesWalkingPadR2(const ble::Advertisement& a) {
    return !a.hasService(0x1826) &&
        any(upper(a.name), {"KS-ST-K12PRO", "KS-R1AC", "KS-HC-R1AA", "KS-HC-R1AC", "KS-X21",
                            "KS-HDSC-X21C", "KS-HDSY-X21C", "KS-NACH-X21C", "KS-NGCH-X21C",
                            "KS-NACH-MXG", "KS-NGCH-G1C"});
}
bool matchesWalkingPadR1(const ble::Advertisement& a) {
    const auto name = upper(a.name);
    return !a.hasService(0x1826) && !matchesWalkingPadR2(a) && !prefix(name, "KS-HD-Z1D") &&
        (name == "RE" ||
         any(name, {"R1 PRO", "KINGSMITH", "DYNAMAX", "WALKINGPAD", "KS-ST-A1P", "KS-SC-BLR2C",
                    "KS-H", "KS-F0", "KS-BLC", "KS-BLR"}));
}
std::uint16_t horizonCrc(const std::uint8_t* data, std::size_t length) {
    if (!length) return 0;
    std::uint16_t crc = 0xffff;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
Bytes horizonFrame(std::uint16_t seq, std::uint8_t type, std::uint8_t cmd, const Bytes& payload, bool paragon) {
    if (payload.size() > 800) return {};
    Bytes out{0x55, 0xaa, 0, 0, type, cmd, 0, 0, 0, 0};
    put16(out, 2, seq);
    put16(out, 6, static_cast<unsigned>(payload.size()));
    put16(out, 8, horizonCrc(payload.data(), payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    if (paragon) out.insert(out.end(), {0x0d, 0x0a});
    return out;
}
Bytes r1Frame(std::uint8_t cmd, const Bytes& payload) {
    Bytes out{0xf7, cmd};
    out.insert(out.end(), payload.begin(), payload.end());
    std::uint8_t sum = cmd;
    for (auto byte : payload) sum = static_cast<std::uint8_t>(sum + byte);
    out.push_back(sum);
    out.push_back(0xfd);
    return out;
}
Bytes r2Encode(const std::string& text, Cipher cipher) {
    Bytes out;
    const char* table = cipherTable(cipher);
    for (std::size_t i = 0; i < text.size(); i += 3) {
        const auto a = static_cast<std::uint8_t>(text[i]);
        const auto b = i + 1 < text.size() ? static_cast<std::uint8_t>(text[i + 1]) : 0;
        const auto c = i + 2 < text.size() ? static_cast<std::uint8_t>(text[i + 2]) : 0;
        out.push_back(table[a >> 2]);
        out.push_back(table[((a & 3) << 4) | (b >> 4)]);
        out.push_back(table[i + 1 < text.size() ? ((b & 15) << 2) | (c >> 6) : 64]);
        out.push_back(table[i + 2 < text.size() ? c & 63 : 64]);
    }
    out.push_back(0x0d);
    return out;
}
bool r2Decode(const Bytes& bytes, std::string& out, Cipher cipher) {
    out.clear();
    if (bytes.empty() || bytes.back() != 0x0d || (bytes.size() - 1) % 4) return false;
    const char* table = cipherTable(cipher);
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 4) {
        int v[4];
        for (int j = 0; j < 4; ++j) {
            v[j] = charIndex(table, bytes[i + j]);
            if (v[j] < 0) return false;
        }
        const bool last = i + 5 == bytes.size();
        if (v[0] == 64 || v[1] == 64 || (!last && (v[2] == 64 || v[3] == 64)) ||
            (v[2] == 64 && v[3] != 64) || (v[2] == 64 && (v[1] & 15)) ||
            (v[3] == 64 && v[2] != 64 && (v[2] & 3))) return false;
        out.push_back(static_cast<char>((v[0] << 2) | (v[1] >> 4)));
        if (v[2] != 64) out.push_back(static_cast<char>((v[1] << 4) | (v[2] >> 2)));
        if (v[3] != 64) out.push_back(static_cast<char>((v[2] << 6) | v[3]));
    }
    return true;
}
}  // namespace treadmills

TreadmillSource::TreadmillSource(Model model, Cipher cipher, R2Service r2Service, float speedLimit)
    : model_(model), cipher_(cipher), r2Service_(r2Service),
      speedLimit_(std::isfinite(speedLimit) && speedLimit > 0 ? speedLimit : 0) {
    if (horizon()) {
        service_ = ble::Uuid::from16(0xfff0);
        write_ = ble::Uuid::from16(0xfff3);
        notify_ = ble::Uuid::from16(0xfff4);
    } else if (spirit() || sole()) {
        service_ = ble::Uuid::fromString("49535343-fe7d-4ae5-8fa9-9fafd205e455");
        write_ = ble::Uuid::fromString("49535343-8841-43f4-a8d4-ecbe34729bb3");
        notify_ = ble::Uuid::fromString("49535343-1e4d-4bd9-ba61-23c647249616");
    } else if (r1()) {
        service_ = ble::Uuid::from16(0xfe00);
        write_ = ble::Uuid::from16(0xfe02);
        notify_ = ble::Uuid::from16(0xfe01);
    } else {
        service_ = ble::Uuid::from16(0x1234);
        write_ = ble::Uuid::from16(0xfed7);
        notify_ = ble::Uuid::from16(0xfed8);
        if (r2Service == R2Service::Service1 || r2Service == R2Service::Service1Chars2 ||
            r2Service == R2Service::Service1Chars1)
            service_ = ble::Uuid::fromString("00011234-0000-1000-8000-00805f9b34fb");
        if (r2Service == R2Service::Service2 || r2Service == R2Service::Service2Chars2)
            service_ = ble::Uuid::fromString("00021234-0000-1000-8000-00805f9b34fb");
        if (r2Service == R2Service::Service1Chars2 || r2Service == R2Service::Service2Chars2) {
            write_ = ble::Uuid::fromString("0002fed7-0000-1000-8000-00805f9b34fb");
            notify_ = ble::Uuid::fromString("0002fed8-0000-1000-8000-00805f9b34fb");
        }
        if (r2Service == R2Service::Service1Chars1) {
            write_ = ble::Uuid::fromString("0001fed7-0000-1000-8000-00805f9b34fb");
            notify_ = ble::Uuid::fromString("0001fed8-0000-1000-8000-00805f9b34fb");
        }
    }
}
bool TreadmillSource::horizon() const { return model_ == Model::Horizon || model_ == Model::HorizonParagon; }
bool TreadmillSource::spirit() const {
    return model_ == Model::SpiritXT385 || model_ == Model::SpiritXT485 || model_ == Model::SpiritXT800;
}
bool TreadmillSource::sole() const {
    return model_ == Model::SoleF80 || model_ == Model::SoleF63 || model_ == Model::SoleF65 || model_ == Model::SoleTT8;
}
bool TreadmillSource::r1() const { return model_ == Model::WalkingPadR1 || model_ == Model::WalkingPadRE; }
bool TreadmillSource::r2() const { return model_ == Model::WalkingPadR2; }
const char* TreadmillSource::id() const {
    switch (model_) {
        case Model::Horizon: return "horizon-tm";
        case Model::HorizonParagon: return "horizon-paragon";
        case Model::SpiritXT385: return "spirit-treadmill";
        case Model::SpiritXT485: return "spirit-xt485";
        case Model::SpiritXT800: return "spirit-xt800";
        case Model::SoleF80: return "sole-treadmill";
        case Model::SoleF63: return "sole-f63";
        case Model::SoleF65: return "sole-f65";
        case Model::SoleTT8: return "sole-tt8";
        case Model::WalkingPadRE: return "walkingpad-re";
        case Model::WalkingPadR1: return maximumSpeed() <= 6 ? "walkingpad-flat" : "walkingpad-r1";
        case Model::WalkingPadR2: break;
    }
    if (cipher_ == Cipher::Classic && r2Service_ == R2Service::Standard && maximumSpeed() > 6)
        return "walkingpad-r2";
    if (maximumSpeed() <= 6 && cipher_ == Cipher::Classic && r2Service_ == R2Service::Service1Chars1)
        return "walkingpad-g1";
    if (maximumSpeed() <= 6 && cipher_ == Cipher::Classic && r2Service_ == R2Service::Service2Chars2)
        return "walkingpad-g1b";
    static const char* ids[7][6] = {
        {"wp2-c0-s0","wp2-c0-s1","wp2-c0-s2","wp2-c0-s1c2","wp2-c0-s2c2","wp2-c0-s1c1"},
        {"wp2-c2-s0","wp2-c2-s1","wp2-c2-s2","wp2-c2-s1c2","wp2-c2-s2c2","wp2-c2-s1c1"},
        {"wp2-c3-s0","wp2-c3-s1","wp2-c3-s2","wp2-c3-s1c2","wp2-c3-s2c2","wp2-c3-s1c1"},
        {"wp2-c4-s0","wp2-c4-s1","wp2-c4-s2","wp2-c4-s1c2","wp2-c4-s2c2","wp2-c4-s1c1"},
        {"wp2-c5-s0","wp2-c5-s1","wp2-c5-s2","wp2-c5-s1c2","wp2-c5-s2c2","wp2-c5-s1c1"},
        {"wp2-c6-s0","wp2-c6-s1","wp2-c6-s2","wp2-c6-s1c2","wp2-c6-s2c2","wp2-c6-s1c1"},
        {"wp2-c7-s0","wp2-c7-s1","wp2-c7-s2","wp2-c7-s1c2","wp2-c7-s2c2","wp2-c7-s1c1"}
    };
    return ids[static_cast<unsigned>(cipher_)][static_cast<unsigned>(r2Service_)];
}
float TreadmillSource::maximumSpeed() const {
    float maximum = 19.3F;
    if (r2() && cipher_ == Cipher::G1) return 6;
    if (model_ == Model::WalkingPadRE) return 6;
    if (r1()) maximum = 10;
    if (r2()) maximum = 12;
    if (model_ == Model::Horizon) maximum = 16;
    if (model_ == Model::HorizonParagon) maximum = 20;
    return speedLimit_ > 0 ? std::min(maximum, speedLimit_) : maximum;
}
float TreadmillSource::maximumIncline() const {
    if (r1() || r2() || model_ == Model::SpiritXT485 || model_ == Model::SpiritXT800) return 0;
    if (model_ == Model::Horizon) return 10;
    if (model_ == Model::HorizonParagon) return 15;
    return 15;
}
ble::ConnectPlan TreadmillSource::plan() const {
    ble::ConnectPlan out;
    out.subscribe(service_, notify_);
    return out;
}
void TreadmillSource::reset() {
    SourceProtocol::reset();
    steps_.clear();
    receive_.clear();
    step_ = groupStart_ = 0;
    receiveMs_ = sentMs_ = pollMs_ = targetMs_ = targetStartedMs_ = 0;
    speedDirection_ = inclineDirection_ = 0;
    sequence_ = 16;
    attempts_ = poll_ = 0;
    connected_ = ready_ = waiting_ = sent_ = targetWaiting_ = false;
    speedTarget_ = inclineTarget_ = -1;
}
bool TreadmillSource::send(const Bytes& bytes) {
    return queueWrite(service_, write_, bytes, !r2());
}
void TreadmillSource::append(const Bytes& frame, bool acknowledge) {
    for (std::size_t i = 0; i < frame.size(); i += r2() ? 16 : 20) {
        const std::size_t end = std::min(frame.size(), i + (r2() ? 16 : 20));
        steps_.push_back({Bytes(frame.begin() + i, frame.begin() + end), acknowledge && end == frame.size()});
    }
}
void TreadmillSource::appendText(const std::string& text) { append(treadmills::r2Encode(text, cipher_)); }

void TreadmillSource::buildInit() {
    steps_.clear();
    if (horizon()) {
        if (model_ == Model::HorizonParagon) {
            append(treadmills::horizonFrame(0, 2, 0x20, {}, true));
            append(treadmills::horizonFrame(0, 3, 2, {0,0,0,0,0,0,0,0,1,0,0,0,0,0}, true));
        } else {
            append(treadmills::horizonFrame(1, 1, 2, {}));
            // Seven 731-byte profiles from upstream. Anonymous default user
            // records avoid importing personal names; checksum covers payload.
            for (unsigned profile = 0; profile < 7; ++profile) {
                Bytes payload(731, 0);
                payload[0] = profile > 1 ? static_cast<std::uint8_t>(profile - 1) : static_cast<std::uint8_t>(profile);
                const std::string name = profile == 0 ? "Guest" : "user";
                std::copy(name.begin(), name.end(), payload.begin() + 1);
                static const Bytes records[] = {
                    {0xdc,5,0xc2,7,1,1,1,0xd3,0x8a,0x0c,0,1,1,2,0x38,0,0},
                    {0xa4,6,0xc4,7,9,0x1c,1,0x9f,0xef,0x0c,0,1,1,2,0x38,0,0},
                    {0xa4,6,0xc4,7,9,0x1c,1,0x9f,0xef,0x0c,0,1,1,2,0x38,0,0},
                    {0xa4,6,0xc5,7,0x0b,0x0f,1,0x4b,0x40,0x0c,0,1,1,2,0x38,0,0},
                    {8,7,0xca,7,5,0x1c,1,7,0x25,0x0c,0,1,1,2,0x38,0,0},
                    {0xb0,4,0xcc,7,1,8,1,0xc2,0x0f,0x0c,0,1,1,2,0x38,0,0},
                    {0xdc,5,0xc2,7,1,1,1,0x8e,0x6a,0x0c,0,1,1,2,0x38,0,0}
                };
                const Bytes& metadata = records[profile];
                std::copy(metadata.begin(), metadata.end(), payload.begin() + 46);
                std::fill(payload.begin() + 63, payload.begin() + 433, 0x30);
                std::fill(payload.begin() + 716, payload.end(), 0x30);
                append(treadmills::horizonFrame(static_cast<std::uint16_t>(profile + 2), 1, 0x16, payload));
            }
            for (unsigned seq = 9; seq <= 13; ++seq)
                append(treadmills::horizonFrame(static_cast<std::uint16_t>(seq), 1, 0, {0x1a,0}));
            append(treadmills::horizonFrame(14, 3, 0x14, {0xe5,0x07,2,8,0x13,0x12,0x21,0}));
            append(treadmills::horizonFrame(15, 3, 1, {1}));
            append(treadmills::horizonFrame(16, 3, 0x10, {0}));
        }
    } else if (r1()) {
        append(treadmills::r1Frame(0xa5, model_ == Model::WalkingPadRE
            ? Bytes{0x61,0x2b,0xe9,0x19,0xc7,0xd2} : Bytes{0x61,1,0x6a,0x23,0x1d,0xbe}));
        append(treadmills::r1Frame(0xa2, {0,0}));
        append(treadmills::r1Frame(0xa6, {0,0,0,0,0}));
        append(treadmills::r1Frame(0xb1, {5,7,0x15,0x16,8,0x18}));
        append(treadmills::r1Frame(0xb3, {2,0x1d,0}));
    } else if (r2()) {
        for (const char* text : {"", "shake", "net", "get_dn", "get_pk", "version", "servers getProp 1 2 7 12 23 24 31"})
            appendText(text);
        // time_posix requires a wall clock, unavailable in SourceProtocol's
        // monotonic API. It is not needed for speed/status; never send uptime
        // as a fabricated Unix timestamp.
    } else {
        const auto repeat = [this](const Bytes& b, unsigned count) {
            while (count--) append(b);
        };
        const Bytes discover = bracket(0xf0), user = bracket(3, {1});
        const Bytes ack10 = consoleAck(0x10), ack09 = consoleAck(9);
        const Bytes program = bracket(0x22, {9}), stream = bracket(2, {2});
        if (spirit()) {
            if (model_ == Model::SpiritXT385) {
                append(discover); repeat(user, 3); repeat(ack09, 2); repeat(ack10, 10);
                repeat(bracket(7, {1,0x23,0,0x9b,0xaa}), 2);
                append(bracket(8, {0x10,1})); repeat(bracket(4, {0x62,0,0,0}), 2);
                append(program); append(stream);
                // Upstream starts the belt with 03 04 here. Omitted: connecting
                // must never cause motion without an explicit control request.
            } else {
                append(discover); append(ack10);
                repeat(user, model_ == Model::SpiritXT485 ? 2 : 3);
                append(ack10); append(user); append(ack09); append(ack10); repeat(ack09, 2);
                append(model_ == Model::SpiritXT485 ? bracket(7, {1,0x2c,0,0x61,0x59})
                                                    : bracket(7, {0,0x23,0,0x84,0x40}));
                append(model_ == Model::SpiritXT485 ? bracket(8, {0x20,4}) : bracket(8, {0x10,1}));
                append(bracket(4, {static_cast<std::uint8_t>(model_ == Model::SpiritXT485 ? 0x0a : 0),0,0,0}));
                append(program); append(stream);
            }
        } else {
            append(discover);
            if (model_ == Model::SoleF65) append(ack10);
            repeat(user, 4);
            if (model_ != Model::SoleF65) append(ack09);
            if (model_ == Model::SoleF63) {
                repeat(ack10, 7); append(ack09); repeat(bracket(3, {0}), 3);
                append(ack10); append(consoleAck(0xf1)); repeat(ack10, 4); repeat(user, 2); repeat(ack10, 2);
                append(bracket(7, {0,0x23,0,0x84,0x40})); append(bracket(8, {0x30,7}));
                append(bracket(4, {0,0,0,0}));
                Bytes profile(21, 0);
                profile[0] = 7;
                repeat(bracket(0x24, profile), 3);
                repeat(program, 2); append(stream); repeat(ack10, 4); repeat(consoleAck(0x40), 2);
                repeat(ack10, 3); repeat(consoleAck(0x40), 2);
                // Upstream's four 03 04 frames start the belt; intentionally absent.
            } else if (model_ == Model::SoleTT8) {
                repeat(ack10, 4); append(bracket(7, {1,0x23,0,0x9b,0x43}));
                repeat(bracket(8, {0x20,2}), 4); repeat(bracket(4, {0x0a,0,0,0}), 2);
                repeat(program, 2); append(stream);
            } else if (model_ == Model::SoleF65) {
                append(ack10); append(user); append(ack09); append(ack10);
                append(bracket(7, {1,0x23,0,0x9b,0xaa})); repeat(bracket(8, {0x10,1}), 2);
                append(bracket(4, {0,0,0,0})); append(program); repeat(stream, 4);
                append(ack10); append(consoleAck(0x40));
            } else {
                repeat(ack10, 2); append(bracket(7, {1,0x23,0,0x9b,0xaa}));
                repeat(bracket(8, {0x10,1}), 2); append(bracket(4, {0,0,0,0}));
                append(program); repeat(stream, 4); repeat(ack10, 4); repeat(consoleAck(0x40), 2);
            }
        }
    }
}

bool TreadmillSource::onConnected(std::uint32_t mask, std::uint32_t now) {
    reset();
    if (!(mask & 1)) { setError("Treadmill notification subscription required"); return false; }
    connected_ = true;
    buildInit();
    sentMs_ = pollMs_ = now;
    tick(now);
    return true;
}
void TreadmillSource::acknowledged() {
    if (!waiting_) return;
    waiting_ = false;
    groupStart_ = step_;
    attempts_ = 0;
    if (step_ == steps_.size()) ready_ = true;
}
void TreadmillSource::tick(std::uint32_t now) {
    if (!connected_ || failed()) return;
    if (!receive_.empty() && now - receiveMs_ > 2000) receive_.clear();
    if (!ready_) {
        if (waiting_) {
            if (now - sentMs_ < (horizon() ? 8000U : 2000U)) return;
            if (attempts_ >= 3) { fail("Treadmill initialization not acknowledged"); return; }
            waiting_ = false;
            step_ = groupStart_;
        }
        if (step_ < steps_.size() && (!sent_ || now - sentMs_ >= 30) && pendingWrites() == 0) {
            const Step& step = steps_[step_];
            if (!send(step.bytes)) return;
            sent_ = true;
            sentMs_ = now;
            ++step_;
            if (step.acknowledge) { waiting_ = true; ++attempts_; }
        }
        return;
    }
    if (pendingWrites()) return;
    if ((spirit() || sole()) && (speedTarget_ >= 0 || inclineTarget_ >= 0)) {
        driveTarget(now);
        if (pendingWrites()) return;
    }
    if (now - pollMs_ < (r2() ? 1000U : 300U)) return;
    pollMs_ = now;
    if (r1()) send(treadmills::r1Frame(0xa2, {0,0}));
    else if (r2()) {
        const auto frame = treadmills::r2Encode("servers getProp 1 2 7 12 23 24 31", cipher_);
        for (std::size_t i = 0; i < frame.size(); i += 16)
            send(Bytes(frame.begin() + i, frame.begin() + std::min(frame.size(), i + 16)));
    } else if (spirit() || sole()) {
        const bool second = (poll_++ & 1) != 0;
        // Spirit upstream always writes noOpData (0x10), including the
        // misleading sizeof(noOpData1) branches. 0x06 is a speed-up command
        // on non-XT385 models, never a safe background poll.
        send(consoleAck(spirit() ? 0x10 : (second ? 6 : 0x10)));
    }
    // Horizon's custom channel streams autonomously after initialization.
}
void TreadmillSource::onWriteFailed(std::uint32_t) {
    ready_ = connected_ = false;
    speedTarget_ = inclineTarget_ = -1;
    fail("Treadmill GATT write failed");
}

void TreadmillSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t now) {
    if (!connected_ || failed() || index != 0 || !data || !length) return;
    if (!receive_.empty() && now - receiveMs_ > 2000) receive_.clear();
    receiveMs_ = now;
    for (std::size_t i = 0; i < length; ++i) {
        if (receive_.size() >= 1024) receive_.clear();
        receive_.push_back(data[i]);
        if (r2()) {
            if (data[i] == 0x0d) {
                std::string text;
                if (treadmills::r2Decode(receive_, text, cipher_)) {
                    // Replies must name the command being initialized; a
                    // checksummed but unrelated message is not a handshake.
                    bool reply = false;
                    if (waiting_ && groupStart_ < steps_.size()) {
                        Bytes command;
                        for (std::size_t j = groupStart_; j < step_; ++j)
                            command.insert(command.end(), steps_[j].bytes.begin(), steps_[j].bytes.end());
                        std::string plain;
                        if (treadmills::r2Decode(command, plain, cipher_)) {
                            const std::string verb = plain.substr(0, plain.find(' '));
                            reply = verb.empty() ? prefix(text, "format") :
                                (verb == "servers" ? prefix(text, "props ") :
                                 (text == verb || prefix(text, (verb + " ").c_str())));
                        }
                    }
                    if (reply) acknowledged();
                    acceptText(text, now);
                }
                receive_.clear();
            }
            continue;
        }
        const std::uint8_t head = horizon() ? 0x55 : (r1() ? 0xf8 : 0x5b);
        while (!receive_.empty() && receive_[0] != head && !(r1() && receive_[0] == 0xf7))
            receive_.erase(receive_.begin());
        if (receive_.size() < 2) continue;
        std::size_t expected = 0;
        if (horizon()) {
            if (receive_[1] != 0xaa) { receive_.erase(receive_.begin()); continue; }
            if (receive_.size() < 10) continue;
            expected = le16(receive_, 6) + 10 + (model_ == Model::HorizonParagon ? 2 : 0);
            if (expected > 812) { receive_.clear(); continue; }
        } else if (r1()) {
            if (receive_[0] == 0xf8 && receive_[1] == 0xa2) expected = 20;
            else {
                switch (receive_[1]) {
                    case 0xa2: expected = 6; break;
                    case 0xa5: expected = 10; break;
                    case 0xa6: expected = 9; break;
                    case 0xb1: expected = 10; break;
                    case 0xb3: expected = 7; break;
                    default: receive_.clear(); continue;
                }
            }
        } else {
            expected = receive_[1] + 3;
            if (expected < 4 || expected > 64) { receive_.clear(); continue; }
        }
        if (receive_.size() < expected) continue;
        Bytes frame(receive_.begin(), receive_.begin() + expected);
        receive_.erase(receive_.begin(), receive_.begin() + expected);
        bool valid = true;
        if (horizon()) {
            const std::size_t payload = le16(frame, 6);
            valid = le16(frame, 8) == treadmills::horizonCrc(frame.data() + 10, payload);
            if (model_ == Model::HorizonParagon)
                valid = valid && frame[expected - 2] == 0x0d && frame.back() == 0x0a;
        } else if (r1()) {
            std::uint8_t sum = 0;
            for (std::size_t j = 1; j + 2 < frame.size(); ++j) sum = static_cast<std::uint8_t>(sum + frame[j]);
            valid = frame.back() == 0xfd && frame[frame.size() - 2] == sum;
        } else valid = frame.back() == 0x5d;
        if (valid) {
            bool reply = false;
            if (waiting_ && groupStart_ < steps_.size()) {
                const Bytes& request = steps_[groupStart_].bytes;
                if (horizon()) {
                    reply = frame[4] == 2 && request.size() >= 6 &&
                        (frame[5] == request[5] || frame[5] == 1);
                } else if (r1()) {
                    reply = request.size() >= 2 && frame[1] == request[1];
                } else {
                    reply = frame.size() == 18 || (request.size() >= 3 && frame[2] == request[2]) ||
                        (frame.size() == 7 && frame[2] == 0 && frame[4] == 0x4f && frame[5] == 0x4b);
                }
            }
            if (reply) acknowledged();
            acceptFrame(frame, now);
        }
    }
}
void TreadmillSource::acceptFrame(const Bytes& b, std::uint32_t now) {
    float speed = -1, incline = -1, heart = -1;
    if (horizon()) {
        if (b.size() > 70 && b[5] == 0x17) {
            speed = le16(b, 24) / 100.0F * 1.60934F;
            incline = b[30] / 10.0F;
        } else if (b.size() > 70 && b[5] == 0x12) {
            speed = le16(b, 61) / 1000.0F * 1.60934F;
            incline = b[63] / 10.0F;
        } else if (b.size() == 29) speed = le16(b, 14) / 10.0F;
        else if (b[4] == 3 && b[5] == 3 && le16(b, 6) == 1 && b[10] == 0) speed = 0;
    } else if (r1()) {
        if (b.size() != 20 || b[0] != 0xf8 || b[1] != 0xa2) return;
        speed = b[3] / 10.0F;
        // Flat WalkingPads have no incline telemetry. Byte 17 is not incline.
    } else if (b.size() == 18) {
        speed = b[10] / 10.0F * (sole() ? 1.60934F : 1.0F);
        incline = b[11] / (spirit() ? 10.0F : 1.0F);
        if (sole() && b[9] > 0 && b[9] <= 240) heart = b[9];
    }
    if (speed < 0 || speed > (r1() || r2() ? 22.0F : 25.0F) ||
        (incline >= 0 && incline > 15)) return;
    sample_.speed = {speed, true};
    if (incline >= 0) sample_.incline = {incline, true};
    if (heart >= 0) sample_.heartRate = {heart, true};
    touch(now);
    // One hardware key press per fresh status, never a blind burst. Stop on
    // target crossing instead of reversing into oscillation.
    if (targetWaiting_ && now != targetMs_) targetWaiting_ = false;
}
void TreadmillSource::acceptText(const std::string& text, std::uint32_t now) {
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    std::string key, value;
    if (!(input >> key) || key != "props") return;
    MetricSample speed = sample_.speed, cadence = sample_.cadence;
    bool changed = false;
    while (input >> key) {
        if (!(input >> value) || key == "Error") return;
        if (key == "CurrentSpeed" || key == "spm") {
            float number;
            if (!finiteNumber(value, number) || number < 0 ||
                number > (key == "CurrentSpeed" ? maximumSpeed() : 250.0F)) return;
            (key == "CurrentSpeed" ? speed : cadence) = {number, true};
            changed = true;
        }
    }
    if (!changed) return;
    sample_.speed = speed;
    sample_.cadence = cadence;
    touch(now);
}
TrainerCapabilities TreadmillSource::capabilities() const {
    TrainerCapabilities out;
    out.metrics = observedMetrics();
    const bool controls = connected_ && ready_ && !failed();
    out.setControl(Control::Speed, controls);
    out.setControl(Control::Incline, controls && maximumIncline() > 0);
    return out;
}
bool TreadmillSource::control(const ControlRequest& req, std::uint32_t now) {
    if (!capabilities().supports(req.control) || !std::isfinite(req.value)) return false;
    const float max = req.control == Control::Speed ? maximumSpeed() : maximumIncline();
    const ControlBounds bounds = req.control == Control::Speed ? kSpeedBounds : kInclineBounds;
    if (req.value < std::max(0.0F, bounds.minimum) || req.value > std::min(max, bounds.maximum)) return false;
    const float target = std::round(req.value * 10) / 10;
    if (pendingWrites()) return false;
    if (horizon()) {
        Bytes payload;
        const bool paragon = model_ == Model::HorizonParagon;
        if (req.control == Control::Speed) {
            const unsigned wire = static_cast<unsigned>(std::lround(target * (paragon ? 1.0F : 0.621371F) * 10));
            payload = paragon ? Bytes{0,0,1} : Bytes{0,0,0,0};
            put16(payload, paragon ? 0 : 1, wire);
        } else {
            payload = paragon ? Bytes{0,0} : Bytes{0,0,0};
            put16(payload, 0, static_cast<unsigned>(std::lround(target * 10)));
        }
        return send(treadmills::horizonFrame(++sequence_, 3, req.control == Control::Speed ? 5 : 6, payload, paragon));
    }
    if (r1()) return send(treadmills::r1Frame(0xa2, {1, static_cast<std::uint8_t>(std::lround(target * 10))}));
    if (r2()) {
        const Bytes frame = treadmills::r2Encode("props CurrentSpeed " + tenths(target), cipher_);
        for (std::size_t i = 0; i < frame.size(); i += 16)
            if (!send(Bytes(frame.begin() + i, frame.begin() + std::min(frame.size(), i + 16)))) return false;
        return true;
    }
    if (!hasData() || now - lastDataMs() > 2000 ||
        !(req.control == Control::Speed ? sample_.speed.valid : sample_.incline.valid)) return false;
    if (req.control == Control::Speed && target == 0) {
        speedTarget_ = inclineTarget_ = -1;
        if (spirit() && model_ != Model::SpiritXT800) return send(bracket(0xf1, {6}));
        if (sole()) return send(bracket(3, {6}));
        // XT800/XT900 upstream provides no working stop command.
        return false;
    }
    const float current = req.control == Control::Speed ? sample_.speed.value : sample_.incline.value;
    (req.control == Control::Speed ? speedTarget_ : inclineTarget_) = target;
    (req.control == Control::Speed ? speedDirection_ : inclineDirection_) = target > current ? 1 : -1;
    targetWaiting_ = false;
    targetStartedMs_ = now;
    driveTarget(now);
    return !failed();
}
void TreadmillSource::driveTarget(std::uint32_t now) {
    if (!hasData() || now - lastDataMs() > 2000 || now - targetStartedMs_ > 30000 ||
        (targetWaiting_ && now - targetMs_ > 2000)) {
        speedTarget_ = inclineTarget_ = -1;
        targetWaiting_ = false;
        return;
    }
    if (targetWaiting_ || pendingWrites() || now - targetMs_ < 300) return;
    const bool speed = speedTarget_ >= 0;
    float& target = speed ? speedTarget_ : inclineTarget_;
    const float current = speed ? sample_.speed.value : sample_.incline.value;
    const float tolerance = speed ? (sole() ? 0.09F : 0.06F) : (sole() ? 0.51F : 0.06F);
    const int direction = speed ? speedDirection_ : inclineDirection_;
    if (std::fabs(target - current) <= tolerance || (direction > 0 && current > target) ||
        (direction < 0 && current < target)) { target = -1; return; }
    const bool up = target > current;
    bool ok;
    if (model_ == Model::SpiritXT485 || model_ == Model::SpiritXT800)
        ok = send(consoleAck(up ? 6 : 0x32));
    else {
        ok = send(bracket(0xf1, {static_cast<std::uint8_t>(speed ? (up ? 2 : 3) : (up ? 4 : 5))}));
        if (ok && model_ == Model::SpiritXT385) ok = send(consoleAck(6));
    }
    if (!ok) { speedTarget_ = inclineTarget_ = -1; return; }
    targetMs_ = now;
    targetWaiting_ = true;
}

std::unique_ptr<SourceProtocol> createHorizonTreadmill() { return std::unique_ptr<SourceProtocol>(new HorizonSource); }
std::unique_ptr<SourceProtocol> createSpiritTreadmill() { return std::unique_ptr<SourceProtocol>(new SpiritSource); }
std::unique_ptr<SourceProtocol> createSoleTreadmill() { return std::unique_ptr<SourceProtocol>(new SoleSource); }
std::unique_ptr<SourceProtocol> createWalkingPadR1() { return std::unique_ptr<SourceProtocol>(new WalkingPadR1Source); }
std::unique_ptr<SourceProtocol> createWalkingPadR2() { return std::unique_ptr<SourceProtocol>(new WalkingPadR2Source); }
std::unique_ptr<SourceProtocol> createTreadmill(const ble::Advertisement& a) {
    const std::string name = upper(a.name);
    if (treadmills::matchesHorizon(a))
        return std::unique_ptr<SourceProtocol>(new HorizonSource(any(name, {"PARAGON X", "JFTMPARAGON"})));
    if (treadmills::matchesSpirit(a))
        return std::unique_ptr<SourceProtocol>(new SpiritSource(prefix(name, "XT385") ? Model::SpiritXT385 :
            (prefix(name, "XT485") ? Model::SpiritXT485 : Model::SpiritXT800)));
    if (treadmills::matchesSole(a))
        return std::unique_ptr<SourceProtocol>(new SoleSource(prefix(name, "F63") ? Model::SoleF63 :
            (prefix(name, "F65") ? Model::SoleF65 : (prefix(name, "TT8") ? Model::SoleTT8 : Model::SoleF80))));
    if (treadmills::matchesWalkingPadR2(a)) {
        R2Service service = R2Service::Standard;
        if (prefix(name, "KS-NACH-X21C")) service = R2Service::Service2Chars2;
        if (prefix(name, "KS-NGCH-G1C") || prefix(name, "KS-NACH-MXG")) service = R2Service::Service1Chars1;
        if (a.hasService(ble::Uuid::fromString("00021234-0000-1000-8000-00805f9b34fb")))
            service = R2Service::Service2Chars2;
        else if (a.hasService(ble::Uuid::fromString("00011234-0000-1000-8000-00805f9b34fb"))) {
            service = R2Service::Service1Chars1;
        }
        const Cipher cipher = prefix(name, "KS-NACH-MXG") ? Cipher::MXG : Cipher::Classic;
        return std::unique_ptr<SourceProtocol>(new WalkingPadR2Source(cipher, service,
            prefix(name, "KS-NGCH-G1C") ? 6 : 12));
    }
    if (treadmills::matchesWalkingPadR1(a))
        return std::unique_ptr<SourceProtocol>(new WalkingPadR1Source(name == "RE" || name == "KS-S1",
            any(name, {"KS-ST-A1P", "KS-F0", "KS-BLC", "KINGSMITH", "WALKINGPAD", "DYNAMAX"}) ? 6 : 10));
    return nullptr;
}

namespace treadmills {
namespace {
template<Model M, int Limit = 0>
std::unique_ptr<SourceProtocol> modelFactory() {
    return std::unique_ptr<SourceProtocol>(new TreadmillSource(M, Cipher::Classic, R2Service::Standard, Limit));
}
template<Model M, int Limit = 0>
bool modelMatch(const ble::Advertisement& a) {
    const auto name = upper(a.name);
    if (M == Model::HorizonParagon) return matchesHorizon(a) && any(name, {"PARAGON X", "JFTMPARAGON"});
    if (M == Model::Horizon) return matchesHorizon(a) && !any(name, {"PARAGON X", "JFTMPARAGON"});
    if (M == Model::SpiritXT385) return matchesSpirit(a) && prefix(name, "XT385");
    if (M == Model::SpiritXT485) return matchesSpirit(a) && prefix(name, "XT485");
    if (M == Model::SpiritXT800) return matchesSpirit(a) && any(name, {"XT800", "XT900"});
    if (M == Model::SoleF63) return matchesSole(a) && prefix(name, "F63");
    if (M == Model::SoleF65) return matchesSole(a) && prefix(name, "F65");
    if (M == Model::SoleTT8) return matchesSole(a) && prefix(name, "TT8");
    if (M == Model::SoleF80) return matchesSole(a) && !any(name, {"F63","F65","TT8"});
    if (M == Model::WalkingPadRE) return matchesWalkingPadR1(a) && name == "RE";
    if (M == Model::WalkingPadR1) {
        const bool flat = any(name, {"KS-ST-A1P", "KS-F0", "KS-BLC", "KINGSMITH", "WALKINGPAD", "DYNAMAX"});
        return matchesWalkingPadR1(a) && name != "RE" && (Limit == 6 ? flat : !flat);
    }
    return false;
}
template<Cipher C, R2Service S>
std::unique_ptr<SourceProtocol> r2Factory() {
    return std::unique_ptr<SourceProtocol>(new WalkingPadR2Source(C,S));
}
template<Cipher C, R2Service S>
bool r2Match(const ble::Advertisement& a) {
    if (!matchesWalkingPadR2(a)) return false;
    const auto name = upper(a.name);
    const auto one = ble::Uuid::fromString("00011234-0000-1000-8000-00805f9b34fb");
    const auto two = ble::Uuid::fromString("00021234-0000-1000-8000-00805f9b34fb");
    if (C == Cipher::MXG && S == R2Service::Service1Chars1)
        return prefix(name, "KS-NACH-MXG") && !a.hasService(two);
    if (C != Cipher::Classic) return false; // Cipher settings cannot be inferred from an advertisement.
    if (prefix(name, "KS-NACH-MXG") || prefix(name, "KS-NGCH-G1C")) return false;
    if (prefix(name, "KS-NACH-X21C")) {
        if (S == R2Service::Service1Chars1) return a.hasService(one);
        if (S == R2Service::Standard) return a.hasService(0x1234) && !a.hasService(one) && !a.hasService(two);
        return S == R2Service::Service2Chars2 && !a.hasService(one) && !a.hasService(0x1234);
    }
    if (prefix(name, "KS-HDSY-X21C"))
        return S == R2Service::Service2Chars2 ? a.hasService(two) :
            (S == R2Service::Standard && !a.hasService(two));
    return S == R2Service::Standard && !a.hasService(one) && !a.hasService(two);
}
std::unique_ptr<SourceProtocol> g1Factory() {
    return std::unique_ptr<SourceProtocol>(new WalkingPadR2Source(Cipher::Classic, R2Service::Service1Chars1, 6));
}
bool g1Match(const ble::Advertisement& a) {
    return matchesWalkingPadR2(a) && prefix(upper(a.name), "KS-NGCH-G1C") &&
        !a.hasService(ble::Uuid::fromString("00021234-0000-1000-8000-00805f9b34fb"));
}
std::unique_ptr<SourceProtocol> g1FallbackFactory() {
    return std::unique_ptr<SourceProtocol>(new WalkingPadR2Source(Cipher::Classic, R2Service::Service2Chars2, 6));
}
bool g1FallbackMatch(const ble::Advertisement& a) {
    return matchesWalkingPadR2(a) && prefix(upper(a.name), "KS-NGCH-G1C") &&
        a.hasService(ble::Uuid::fromString("00021234-0000-1000-8000-00805f9b34fb"));
}
template<Model M, int Limit = 0>
void addModel(std::vector<ProtocolInfo>& out, const char* label) {
    TreadmillSource source(M, Cipher::Classic, R2Service::Standard, Limit);
    out.push_back({source.id(), label, kMatchPriority, modelMatch<M,Limit>, modelFactory<M,Limit>, true, false});
}
template<Cipher C, R2Service S>
void addR2(std::vector<ProtocolInfo>& out) {
    WalkingPadR2Source source(C,S);
    static const char* cipherNames[] = {"classic", "v2", "v3", "v4", "v5", "G1", "MXG"};
    static const char* serviceNames[] = {"standard", "service1 / standard chars", "service2 / standard chars",
                                        "service1 / chars2", "service2 / chars2", "service1 / chars1"};
    // Each template instantiation owns its label permanently; ProtocolInfo's
    // const char* must survive registry enumeration.
    static const std::string label = std::string("WalkingPad R2: ") + cipherNames[static_cast<unsigned>(C)] +
        ", " + serviceNames[static_cast<unsigned>(S)] +
        (C != Cipher::Classic && C != Cipher::MXG ? " (manual)" : "");
    out.push_back({source.id(), label.c_str(),
                   kR2MatchPriority, r2Match<C,S>, r2Factory<C,S>, true, false});
}
const std::vector<ProtocolInfo>& descriptors() {
    static const std::vector<ProtocolInfo> values = [] {
        std::vector<ProtocolInfo> out;
        addModel<Model::Horizon>(out,"Horizon custom BLE");
        addModel<Model::HorizonParagon>(out,"Horizon Paragon X");
        addModel<Model::SpiritXT385>(out,"Spirit XT385");
        addModel<Model::SpiritXT485>(out,"Spirit XT485");
        addModel<Model::SpiritXT800>(out,"Spirit XT800/XT900");
        addModel<Model::SoleF80>(out,"Sole F80/F85/F89/S77/ST90/TRX7.5 custom BLE");
        addModel<Model::SoleF63>(out,"Sole F63");
        addModel<Model::SoleF65>(out,"Sole F65");
        addModel<Model::SoleTT8>(out,"Sole TT8");
        addModel<Model::WalkingPadR1>(out,"WalkingPad R1 Pro");
        addModel<Model::WalkingPadR1,6>(out,"WalkingPad flat A1/C2");
        addModel<Model::WalkingPadRE>(out,"WalkingPad RE");
        out.push_back({"walkingpad-g1","WalkingPad G1",kR2MatchPriority,g1Match,g1Factory,true,false});
        out.push_back({"walkingpad-g1b","WalkingPad G1 alternate service",kR2MatchPriority,g1FallbackMatch,g1FallbackFactory,true,false});
#define ADD_CIPHER(c) \
        addR2<c,R2Service::Standard>(out); \
        addR2<c,R2Service::Service2Chars2>(out); \
        addR2<c,R2Service::Service1Chars1>(out)
        ADD_CIPHER(Cipher::Classic);
        ADD_CIPHER(Cipher::V2);
        ADD_CIPHER(Cipher::V3);
        ADD_CIPHER(Cipher::V4);
        ADD_CIPHER(Cipher::V5);
        ADD_CIPHER(Cipher::G1);
        ADD_CIPHER(Cipher::MXG);
#undef ADD_CIPHER
        return out;
    }();
    return values;
}
}  // namespace
std::size_t descriptorCount() { return descriptors().size(); }
const ProtocolInfo& descriptorAt(std::size_t index) { return descriptors().at(index); }
}  // namespace treadmills
}  // namespace protocols
}  // namespace smartfitness
