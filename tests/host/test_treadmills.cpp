#include "protocols/treadmills.hpp"
#include "protocols/sportstech_f15.hpp"
#include "ble/central_transport.hpp"
#include "ble_input_adapter.h"
#include "fake_central.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace smartfitness;
using namespace smartfitness::protocols;
using namespace smartfitness::protocols::treadmills;
using ble::Bytes;

ble::Advertisement advertisement(const char* name, bool ftms = false) {
    ble::Advertisement out;
    out.name = name;
    if (ftms) out.services.push_back(ble::Uuid::from16(0x1826));
    return out;
}
void notify(SourceProtocol& p, const Bytes& frame, std::uint32_t now) {
    p.onNotify(0, frame.data(), frame.size(), now);
}
Bytes bracketStatus(float speed = 6, float incline = 2, bool sole = false) {
    Bytes b(18, 0);
    b[0] = 0x5b; b[1] = 15; b[2] = 0x10; b[9] = 125;
    b[10] = static_cast<std::uint8_t>(std::lround(speed * 10));
    b[11] = static_cast<std::uint8_t>(std::lround(incline * (sole ? 1 : 10)));
    b[17] = 0x5d;
    return b;
}
Bytes r1Status(float speed = 4) {
    Bytes payload(16, 0);
    payload[0] = 1; payload[1] = static_cast<std::uint8_t>(speed * 10);
    auto b = r1Frame(0xa2, payload);
    b[0] = 0xf8;
    return b;
}
Bytes horizonStatus(std::uint8_t command = 0x17, bool paragon = false) {
    Bytes payload(70, 0);
    if (command == 0x17) { payload[14] = 0x58; payload[15] = 2; payload[20] = 35; }
    else { payload[51] = 0x70; payload[52] = 0x17; payload[53] = 35; }
    return horizonFrame(1, 2, command, payload, paragon);
}
Bytes ack(const TreadmillSource& source, bool paragon = false) {
    const std::string id = source.id();
    if (id.compare(0, 7, "horizon") == 0) return horizonFrame(1, 2, 1, {}, paragon);
    if (source.plan().subscriptions[0].service == ble::Uuid::from16(0xfe00)) return r1Frame(0xa2, {0,0});
    if (id.compare(0, 3, "wp2") == 0 || id == "walkingpad-r2" || id.compare(0,13,"walkingpad-g1") == 0) return r2Encode("shake 00");
    return {0x5b,4,0,0x10,0x4f,0x4b,0x5d};
}
std::vector<Bytes> initialize(TreadmillSource& source, std::uint32_t& now,
                              bool paragon = false, Cipher cipher = Cipher::Classic) {
    assert(source.onConnected(1, now));
    assert(!source.control({Control::Speed, 5}, now));
    std::vector<Bytes> frames;
    Bytes frame;
    for (unsigned iteration = 0; iteration < 2000 && !source.initialized(); ++iteration) {
        ble::GattWrite write;
        while (source.takeWrite(write)) {
            assert(write.service == source.plan().subscriptions[0].service);
            const bool r2 = std::string(source.id()).compare(0,3,"wp2") == 0 ||
                std::string(source.id()) == "walkingpad-r2" || std::string(source.id()).compare(0,13,"walkingpad-g1") == 0;
            assert(write.withResponse == !r2);
            const Bytes bytes = write.payload();
            frame.insert(frame.end(), bytes.begin(), bytes.end());
            const std::string id = source.id();
            bool complete = false;
            if (id.compare(0,7,"horizon") == 0 && frame.size() >= 10) {
                const auto len = frame[6] | (static_cast<unsigned>(frame[7]) << 8);
                complete = frame.size() == len + 10 + (paragon ? 2 : 0);
                if (complete) assert((frame[8] | (frame[9] << 8)) == horizonCrc(frame.data() + 10, len));
            } else if (r2) complete = frame.back() == 0x0d;
            else if (source.plan().subscriptions[0].service == ble::Uuid::from16(0xfe00)) complete = frame.back() == 0xfd;
            else if (frame.size() >= 2) complete = frame.size() == static_cast<unsigned>(frame[1]) + 3;
            if (complete) {
                frames.push_back(frame);
                Bytes response = ack(source, paragon);
                if (r2) {
                    std::string command;
                    assert(r2Decode(frame, command, cipher));
                    const auto verb = command.substr(0, command.find(' '));
                    response = r2Encode(verb.empty() ? "format error" :
                        (verb == "servers" ? "props CurrentSpeed 0" : verb + " 00"), cipher);
                } else if (source.plan().subscriptions[0].service == ble::Uuid::from16(0xfe00)) {
                    response = frame;
                }
                notify(source, response, now);
                frame.clear();
            }
        }
        now += 30;
        if (!source.initialized()) source.tick(now);
    }
    assert(source.initialized());
    assert(!source.failed());
    return frames;
}
void matchTests() {
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < descriptorCount(); ++i) {
        const auto& descriptor = descriptorAt(i);
        assert(std::string(descriptor.id).size() <= 16);
        assert(std::find(ids.begin(), ids.end(), descriptor.id) == ids.end());
        ids.push_back(descriptor.id);
        auto source = descriptor.create();
        assert(source && std::string(source->id()) == descriptor.id);
        assert(!descriptor.matches(advertisement("KS-R1AC", true)));
    }
    for (const auto& pair : {std::pair<const char*,const char*>{"XT385","spirit-treadmill"},
                            {"XT485","spirit-xt485"},{"XT800","spirit-xt800"},{"F63","sole-f63"},
                            {"F65","sole-f65"},{"TT8","sole-tt8"},{"PARAGON X","horizon-paragon"},
                            {"RE","walkingpad-re"},{"KS-BLC","walkingpad-flat"},
                            {"KS-R1AC","walkingpad-r2"},{"KS-NGCH-G1C","walkingpad-g1"}}) {
        unsigned matches = 0;
        for (std::size_t i = 0; i < descriptorCount(); ++i) {
            const auto& descriptor = descriptorAt(i);
            if (descriptor.matches(advertisement(pair.first))) {
                assert(std::string(descriptor.id) == pair.second);
                ++matches;
            }
        }
        assert(matches == 1);
    }
    for (const char* name : {"horizon 7.8", "YPTM-123", "HZ_T101-001", "HZ_7.0AT-2", "AFG SPORT",
                             "WLT2541", "JFTMPARAGON", "PARAGON X"})
        assert(matchesHorizon(advertisement(name)));
    for (const char* name : {"XT385-12", "xt485", "XT800", "XT900"})
        assert(matchesSpirit(advertisement(name)));
    for (const char* name : {"F80", "F89", "F65", "TT8", "F63", "ST90", "TRX7.5", "S77", "F85"})
        assert(matchesSole(advertisement(name)));
    for (const char* name : {"R1 PRO", "KINGSMITH", "DYNAMAX", "WALKINGPAD", "KS-ST-A1P", "KS-SC-BLR2C",
                             "RE", "KS-HCR1AA", "KS-F0", "KS-BLC", "KS-BLR"})
        assert(matchesWalkingPadR1(advertisement(name)));
    for (const char* name : {"KS-ST-K12PRO", "KS-R1AC", "KS-HC-R1AA", "KS-HC-R1AC", "KS-X21",
                             "KS-HDSC-X21C", "KS-HDSY-X21C", "KS-NACH-X21C", "KS-NGCH-X21C",
                             "KS-NACH-MXG", "KS-NGCH-G1C"}) {
        assert(matchesWalkingPadR2(advertisement(name)));
        assert(!matchesWalkingPadR1(advertisement(name)));
    }
    for (const char* name : {"SPIRIT", "SOLE", "KS-unknown", "KS-HD-Z1D", "CT800", "TRX3500", "MYRUN",
                             "PELOTON", "DFC", "KS-S1"}) assert(!createTreadmill(advertisement(name)));
    for (const char* name : {"HORIZON", "XT385", "TT8", "F80", "R1 PRO", "KS-R1AC"})
        assert(!createTreadmill(advertisement(name, true)));
    auto g1 = createTreadmill(advertisement("KS-NGCH-G1C"));
    assert(static_cast<TreadmillSource*>(g1.get())->maximumSpeed() == 6);
    assert(g1->plan().subscriptions[0].characteristic ==
           ble::Uuid::fromString("0001fed8-0000-1000-8000-00805f9b34fb"));
    auto g1Advertisement = advertisement("KS-NGCH-G1C");
    g1Advertisement.services.push_back(ble::Uuid::fromString("00021234-0000-1000-8000-00805f9b34fb"));
    unsigned g1Matches = 0;
    for (std::size_t i = 0; i < descriptorCount(); ++i) {
        const auto& descriptor = descriptorAt(i);
        if (!descriptor.matches(g1Advertisement)) continue;
        ++g1Matches;
        auto fallback = descriptor.create();
        assert(std::string(fallback->id()) == "walkingpad-g1b");
        assert(fallback->plan().subscriptions[0].service ==
               ble::Uuid::fromString("00021234-0000-1000-8000-00805f9b34fb"));
        assert(fallback->plan().subscriptions[0].characteristic ==
               ble::Uuid::fromString("0002fed8-0000-1000-8000-00805f9b34fb"));
    }
    assert(g1Matches == 1);
    g1 = createTreadmill(g1Advertisement);
    assert(g1->plan().subscriptions[0].characteristic ==
           ble::Uuid::fromString("0002fed8-0000-1000-8000-00805f9b34fb"));
    auto pad = createTreadmill(advertisement("KS-BLC"));
    assert(static_cast<TreadmillSource*>(pad.get())->maximumSpeed() == 6);
    auto re = createTreadmill(advertisement("RE"));
    assert(static_cast<TreadmillSource*>(re.get())->maximumSpeed() == 6);
    auto paragon = createTreadmill(advertisement("PARAGON X"));
    assert(static_cast<TreadmillSource*>(paragon.get())->maximumSpeed() == 20);
    assert(createHorizonTreadmill() && createSpiritTreadmill() && createSoleTreadmill() &&
           createWalkingPadR1() && createWalkingPadR2());
    auto nach = advertisement("KS-NACH-X21C");
    auto n = createTreadmill(nach);
    assert(n->plan().subscriptions[0].service == ble::Uuid::fromString("00021234-0000-1000-8000-00805f9b34fb"));
    assert(n->plan().subscriptions[0].characteristic == ble::Uuid::fromString("0002fed8-0000-1000-8000-00805f9b34fb"));
    nach.services.push_back(ble::Uuid::fromString("00011234-0000-1000-8000-00805f9b34fb"));
    n = createTreadmill(nach);
    assert(n->plan().subscriptions[0].characteristic == ble::Uuid::fromString("0001fed8-0000-1000-8000-00805f9b34fb"));
}
void codecTests() {
    const std::string check = "123456789";
    assert(horizonCrc(reinterpret_cast<const std::uint8_t*>(check.data()), check.size()) == 0x29b1);
    assert(horizonFrame(17,3,6,{10,0,0}) == Bytes({0x55,0xaa,17,0,3,6,3,0,0x5d,0x0b,10,0,0}));
    assert(r1Frame(0xa2, {1,25}) == Bytes({0xf7,0xa2,1,25,0xbc,0xfd}));
    assert(r2Encode("shake") == Bytes({'D','A','E','E','O','A','U','=',0x0d}));
    for (Cipher cipher : {Cipher::Classic, Cipher::V2, Cipher::V3, Cipher::V4, Cipher::V5, Cipher::G1, Cipher::MXG}) {
        for (const std::string text : {"", "a", "ab", "abc", "shake", "props CurrentSpeed 5.5 spm 92"}) {
            std::string decoded;
            assert(r2Decode(r2Encode(text, cipher), decoded, cipher));
            assert(decoded == text);
        }
    }
    std::string decoded;
    assert(!r2Decode({0x0d,0x0d}, decoded));
    assert(!r2Decode({'!','!','!','!',0x0d}, decoded));
    assert(!r2Decode({'=','=','=','=','\r'}, decoded));
    assert(!r2Decode({'S','S','=','S','\r'}, decoded));
    assert(!r2Decode({'S','a','=','=', '\r'}, decoded));
}
void lifecycleTests() {
    for (Model model : {Model::Horizon, Model::HorizonParagon, Model::SpiritXT385, Model::SpiritXT485,
                        Model::SpiritXT800, Model::SoleF80, Model::SoleF63, Model::SoleF65, Model::SoleTT8,
                        Model::WalkingPadR1, Model::WalkingPadRE, Model::WalkingPadR2}) {
        TreadmillSource source(model);
        assert(source.startupTimeoutMs() == 90000);
        assert(std::string(source.id()).size() <= 16);
        assert(source.plan().subscriptionCount == 1);
        assert(!source.onConnected(0, 0));
        assert(source.pendingWrites() == 0);
        std::uint32_t now = 100;
        const bool paragon = model == Model::HorizonParagon;
        auto frames = initialize(source, now, paragon);
        for (const auto& frame : frames)
            assert(frame != Bytes({0x5b,2,3,4,0x5d})); // never auto-start
        if (model == Model::Horizon) {
            assert(frames.size() == 16);
            for (unsigned i = 1; i <= 7; ++i) assert(frames[i].size() == 741);
        }
        assert(source.capabilities().supports(Control::Speed));
        assert(!source.capabilities().supports(Control::Resistance));
        assert(!source.control({Control::Speed, std::numeric_limits<float>::infinity()}, now));
        assert(!source.control({Control::Speed, std::numeric_limits<float>::quiet_NaN()}, now));
        assert(!source.control({Control::Speed, 100.1F}, now));
        assert(!source.control({Control::Speed, -0.1F}, now));
        assert(!source.control({Control::Incline, 25.1F}, now));
        assert(!source.control({Control::Speed, source.maximumSpeed() + 1}, now));
        const Bytes status = (model == Model::Horizon || paragon) ? horizonStatus(0x17, paragon) :
            ((model == Model::WalkingPadR1 || model == Model::WalkingPadRE) ? r1Status() :
            (model == Model::WalkingPadR2 ? r2Encode("props CurrentSpeed 4 spm 90") :
             bracketStatus(6,2,model >= Model::SoleF80 && model <= Model::SoleTT8)));
        for (std::size_t i = 0; i < status.size(); ++i) source.onNotify(0, &status[i], 1, ++now);
        assert(source.hasData());
        assert(source.sample().speed.valid);
        assert(source.capabilities().supports(Metric::Speed));
        if (model == Model::WalkingPadR1 || model == Model::WalkingPadRE || model == Model::WalkingPadR2) {
            assert(!source.sample().incline.valid);
            assert(!source.capabilities().supports(Control::Incline));
            assert(!source.control({Control::Incline, 0}, now));
        }
        const auto freshness = source.lastDataMs();
        source.onNotify(1, status.data(), status.size(), now + 1);
        source.onNotify(0, nullptr, 100, now + 1);
        source.onNotify(0, status.data(), 0, now + 1);
        assert(source.lastDataMs() == freshness);
        assert(source.control({Control::Speed, 5}, now + 300));
        ble::GattWrite write;
        unsigned writes = 0;
        while (source.takeWrite(write)) { assert(write.length > 0); ++writes; }
        assert(writes > 0);
        source.onWriteFailed(now + 500);
        assert(source.failed());
        assert(!source.capabilities().supports(Control::Speed));
        assert(!source.control({Control::Speed, 5}, now + 600));
        source.reset();
        assert(!source.failed() && !source.hasData() && !source.initialized() && source.pendingWrites() == 0);
        assert(!source.capabilities().supports(Control::Speed));
    }
    HorizonSource silent;
    assert(silent.onConnected(1, 0));
    ble::GattWrite write;
    for (unsigned i = 0; i < 5; ++i) {
        while (silent.takeWrite(write)) {}
        silent.tick(i * 8001);
    }
    assert(silent.failed());
    WalkingPadR2Source unrelated;
    assert(unrelated.onConnected(1,0));
    while (unrelated.takeWrite(write)) {}
    notify(unrelated, r2Encode("unrelated valid"),1);
    unrelated.tick(31);
    assert(unrelated.pendingWrites() == 0 && !unrelated.initialized());
    for (Cipher cipher : {Cipher::V2, Cipher::V3, Cipher::V4, Cipher::V5, Cipher::G1, Cipher::MXG}) {
        WalkingPadR2Source variant(cipher, R2Service::Service2Chars2);
        std::uint32_t now = 0;
        initialize(variant, now, false, cipher);
        notify(variant, r2Encode("props CurrentSpeed 6 spm 85", cipher), ++now);
        assert(variant.sample().speed.value == 6);
        assert(variant.sample().cadence.value == 85);
        assert(variant.control({Control::Speed, 5}, ++now));
    }
}
void telemetryTests() {
    HorizonSource horizon;
    std::uint32_t now = 0;
    initialize(horizon, now);
    Bytes b = horizonStatus();
    b[8] ^= 1;
    notify(horizon, b, ++now);
    assert(!horizon.hasData());
    b = horizonStatus();
    horizon.onNotify(0, b.data(), 20, ++now);
    horizon.tick(now + 2001);
    horizon.onNotify(0, b.data() + 20, b.size() - 20, now + 2002);
    assert(!horizon.hasData());
    now += 2003;
    Bytes noise{0x00,0x55,0,0x55,0xaa,0,0,0,0,0xff,0xff,0,0};
    notify(horizon, noise, now);
    notify(horizon, horizonStatus(), ++now);
    assert(horizon.hasData());
    assert(std::fabs(horizon.sample().speed.value - 9.65604F) < 0.001F);
    assert(horizon.sample().incline.value == 3.5F);
    notify(horizon, horizonStatus(0x12), ++now);
    assert(std::fabs(horizon.sample().speed.value - 9.65604F) < 0.001F);
    const auto transported = horizonStatus(0x12);
    for (std::size_t offset = 0; offset < transported.size(); offset += ble::kMaxEventData) {
        ble::CentralEvent event;
        event.setData(transported.data() + offset, std::min(ble::kMaxEventData, transported.size() - offset));
        horizon.onNotify(0, event.data.data(), event.length, ++now);
    }
    assert(horizon.lastDataMs() == now);
    assert(horizon.control({Control::Incline, 5}, ++now));
    ble::GattWrite write;
    assert(horizon.takeWrite(write));
    const Bytes incline = write.payload();
    assert(incline[5] == 6 && incline[10] == 50 && incline[11] == 0);
    assert(horizon.control({Control::Speed, 10}, ++now));
    assert(horizon.takeWrite(write));
    const Bytes speed = write.payload();
    assert(speed[5] == 5 && speed[10] == 0 && speed[11] == 62 && speed[12] == 0);

    SoleSource sole;
    initialize(sole, now);
    notify(sole, bracketStatus(6,2,true), ++now);
    assert(std::fabs(sole.sample().speed.value - 9.65604F) < 0.001F);
    assert(sole.sample().incline.value == 2);
    assert(sole.sample().heartRate.value == 125);
    assert(sole.control({Control::Incline, 5}, now + 300));
    assert(sole.takeWrite(write) && write.payload() == Bytes({0x5b,2,0xf1,4,0x5d}));
    const auto time = sole.lastDataMs();
    b = bracketStatus(); b.back() = 0;
    notify(sole, b, now + 500);
    assert(sole.lastDataMs() == time);
    sole.tick(now + 2301);
    while (sole.takeWrite(write)) assert(write.payload()[2] != 0xf1);

    WalkingPadR1Source pad;
    initialize(pad, now);
    b = r1Status(); b[18] ^= 1;
    notify(pad, b, ++now);
    assert(!pad.hasData());
    b = r1Status();
    const auto duplicate = b;
    b.insert(b.end(), duplicate.begin(), duplicate.end());
    notify(pad, b, ++now);
    assert(pad.sample().speed.value == 4);
    assert(pad.control({Control::Speed, 2.5F}, ++now));
    assert(pad.takeWrite(write) && write.payload() == Bytes({0xf7,0xa2,1,25,0xbc,0xfd}));

    WalkingPadR2Source r2;
    initialize(r2, now);
    b = r2Encode("props CurrentSpeed 5.5 spm 92");
    b.insert(b.end(), {0x0d});
    notify(r2, b, ++now);
    assert(r2.sample().speed.value == 5.5F && r2.sample().cadence.value == 92);
    const auto last = r2.lastDataMs();
    for (const char* invalid : {"props", "props CurrentSpeed", "props CurrentSpeed nan",
                                "props CurrentSpeed inf", "props CurrentSpeed 999",
                                "props CurrentSpeed 6 spm -1", "props Error -5000", "props CurrentSpeed 5junk"})
        notify(r2, r2Encode(invalid), ++now);
    assert(r2.lastDataMs() == last);
    assert(r2.sample().speed.value == 5.5F);
    assert(r2.control({Control::Speed, 4.5F}, ++now));
    Bytes joined;
    while (r2.takeWrite(write)) {
        assert(write.length <= 16 && !write.withResponse);
        const auto payload = write.payload(); joined.insert(joined.end(), payload.begin(), payload.end());
    }
    std::string decoded;
    assert(r2Decode(joined, decoded) && decoded == "props CurrentSpeed 4.5");
    r2.tick(now + 1001);
    assert(r2.pendingWrites() > 0 && r2.pendingWrites() <= 16);
}
void targetTests() {
    for (Model model : {Model::SpiritXT385, Model::SpiritXT485, Model::SpiritXT800}) {
        SpiritSource passive(model);
        std::uint32_t timestamp = 100;
        initialize(passive, timestamp);
        for (unsigned poll = 0; poll < 8; ++poll) {
            timestamp += 301;
            passive.tick(timestamp);
            ble::GattWrite output;
            assert(passive.takeWrite(output));
            assert(output.payload() == Bytes({0x5b,4,0,0x10,0x4f,0x4b,0x5d}));
            assert(!passive.takeWrite(output));
        }
    }
    SpiritSource source;
    std::uint32_t now = 100;
    initialize(source, now);
    notify(source, bracketStatus(6,2), ++now);
    assert(source.control({Control::Speed, 7}, now + 300));
    ble::GattWrite write;
    assert(source.takeWrite(write) && write.payload() == Bytes({0x5b,2,0xf1,2,0x5d}));
    assert(source.takeWrite(write) && write.payload() == Bytes({0x5b,4,0,6,0x4f,0x4b,0x5d}));
    source.tick(now + 600);
    while (source.takeWrite(write)) assert(write.payload()[2] != 0xf1);
    notify(source, bracketStatus(7.2F,2), now + 601);
    source.tick(now + 902);
    while (source.takeWrite(write)) assert(write.payload()[2] != 0xf1);
    assert(source.control({Control::Speed, 0}, now + 903));
    assert(source.takeWrite(write) && write.payload() == Bytes({0x5b,2,0xf1,6,0x5d}));
}
}  // namespace

namespace {
Bytes f15Frame(Bytes bytes) {
    std::uint8_t crc = 0;
    for (std::size_t i = 1; i < bytes.size(); ++i) crc ^= bytes[i];
    bytes.push_back(crc);
    bytes.push_back(3);
    return bytes;
}
Bytes f15Status(std::uint8_t mode = 3) {
    // Captured status layout, with nonzero high bytes exercising every LE16.
    return f15Frame({2,0x51,mode,0x08,0x05,0x55,0x02,0x84,0x01,0x34,0x02,0,0,0x80,0x01});
}
Bytes f15Target(unsigned speed, unsigned incline) {
    return f15Frame({2,0x53,2,static_cast<std::uint8_t>(speed),static_cast<std::uint8_t>(incline),
                     0,0,0,0,0,0,0});
}
void f15Connect(SportstechF15Treadmill& source, std::uint32_t now = 0) {
    source.onWriteEndpoint(ble::Uuid::from16(0xfff0), ble::Uuid::from16(0xfff2));
    assert(source.onConnected(1, now));
    ble::GattWrite write;
    assert(source.takeWrite(write) && write.payload() == Bytes({2,0x51,0x51,3}));
    assert(write.service == ble::Uuid::from16(0xfff0));
    assert(write.characteristic == ble::Uuid::from16(0xfff2));
    assert(!write.withResponse);
    assert(!source.takeWrite(write));
}
void f15MatchAndPlanTests() {
    const auto* info = findProtocol("sportstech-f15");
    assert(info && std::string(info->label) == "Sportstech F15 treadmill");
    assert(info->priority == 80 && info->profile == MachineProfile::Treadmill);
    auto source = createProtocol(info->id);
    assert(source && std::string(source->id()) == info->id);
    assert(std::string(info->id).size() <= 16 && source->profile() == MachineProfile::Treadmill);
    for (const char* name : {"FS-123", "fs-001", "Sportstech F15", "SPORTSTECH F15-001"}) {
        assert(detectProtocol(advertisement(name)) == info);
        assert(!info->matches(advertisement(name, true)));
        assert(std::string(detectProtocol(advertisement(name, true))->id) == "ftms");
    }
    for (const char* name : {"", "Other treadmill", "Sportstech F150", "Sportstech F17", "Smart bike"}) {
        auto adv = advertisement(name);
        adv.services.push_back(ble::Uuid::from16(0xfff0));
        assert(!info->matches(adv));
    }
    auto compatible = advertisement("F15-123");
    assert(!info->matches(compatible));
    compatible.services.push_back(ble::Uuid::from16(0xfff0));
    assert(info->matches(compatible));
    const auto plan = source->plan();
    assert(!plan.discoverNotifications && plan.subscriptionCount == 1 && plan.readCount == 0);
    assert(plan.subscriptions[0].service == ble::Uuid::from16(0xfff0));
    assert(plan.subscriptions[0].characteristic == ble::Uuid::from16(0xfff1));
    assert(!plan.subscriptions[0].indicate);
    assert(plan.writeEndpoints[0].service == ble::Uuid::from16(0xfff0));
    assert(plan.writeEndpoints[0].characteristic == ble::Uuid::from16(0xfff2));
    assert(!plan.writeEndpoints[0].withResponse);
    assert(!source->onConnected(1, 0));
    source->onWriteEndpoint(ble::Uuid::from16(0xfff0), ble::Uuid::from16(0xfff3));
    assert(!source->onConnected(1, 0));
    source->onWriteEndpoint(ble::Uuid::from16(0xffe0), ble::Uuid::from16(0xfff2));
    assert(!source->onConnected(1, 0));
    source->onWriteEndpoint(ble::Uuid::from16(0xfff0), ble::Uuid::from16(0xfff2));
    assert(!source->onConnected(0, 0));
    assert(!source->capabilities().controls && source->pendingWrites() == 0);
}
void f15TelemetryTests() {
    SportstechF15Treadmill source;
    f15Connect(source);
    assert(!source.control({Control::Speed, 5}, 1) && !source.capabilities().controls);
    notify(source, f15Frame({2,0x51,1,1}), 1);
    assert(!source.hasData() && !source.treadmillOperation());
    auto full = f15Status();
    for (std::size_t i = 0; i < full.size(); ++i) source.onNotify(0, &full[i], 1, 2);
    assert(source.hasData() && source.lastDataMs() == 2);
    assert(source.treadmillOperation() == TreadmillOperation::Running);
    assert(std::abs(source.sample().speed.value - 0.8F) < 0.001F);
    assert(source.sample().incline.value == 0.5F);
    assert(source.sample().elapsed.value == 597);
    assert(source.sample().distance.value == 388);
    assert(std::abs(source.sample().totalEnergy.value - 56.4F) < 0.001F);
    assert(source.sample().heartRate.value == 384);
    assert(!source.sample().stepCount.valid && !source.sample().cadence.valid);
    assert(source.capabilities().supports(Control::RunState));
    assert(source.capabilities().supports(Metric::TotalEnergy));
    for (std::uint8_t mode : {0,2,3,4}) {
        notify(source, f15Status(mode), 3);
        assert(source.treadmillOperation() == static_cast<TreadmillOperation>(mode));
    }
    std::uint32_t shortMs = 100;
    for (std::uint8_t mode : {0,2,3,4}) {
        notify(source, mode == 2 ? f15Frame({2,0x51,2,3}) : f15Frame({2,0x51,mode}), shortMs);
        source.tick(shortMs + 1000);
        assert(source.treadmillOperation() == static_cast<TreadmillOperation>(mode));
        assert(source.sample().speed.value == 0 && source.sample().heartRate.value == 0);
        assert(source.sample().incline.value == 0.5F && source.sample().elapsed.value == 597);
        assert(source.sample().distance.value == 388);
        assert(std::abs(source.sample().totalEnergy.value - 56.4F) < 0.001F);
        shortMs += 2000;
    }
    // A valid short-looking prefix inside a fragmented full frame is not EOF.
    full = f15Status();
    full[3] = 0x52;
    full[4] = 3;
    full.resize(15);
    full = f15Frame(full);
    source.onNotify(0, full.data(), 5, 8000);
    source.tick(8100);
    assert(source.lastDataMs() == 7100);
    source.onNotify(0, full.data() + 5, full.size() - 5, 8200);
    assert(source.lastDataMs() == 8200 && source.sample().speed.value == 8.2F);
    auto joined = f15Status(0);
    const auto second = f15Status(3);
    joined.insert(joined.end(), second.begin(), second.end());
    notify(source, joined, 8300);
    assert(source.treadmillOperation() == TreadmillOperation::Running && source.lastDataMs() == 8300);
    source.reset();
    f15Connect(source);
    for (Bytes bad : {Bytes{}, Bytes{2}, Bytes{2,0x51,3}, Bytes{2,0x51,3,0},
                      f15Frame({2,0x52,3}), f15Frame({2,0x51,5}), Bytes(109,0x03)}) {
        notify(source, bad, 50);
        source.tick(1050);
        assert(!source.hasData());
        source.reset();
        f15Connect(source);
    }
    full = f15Status();
    full[15] ^= 1;
    notify(source, full, 80);
    assert(!source.hasData());
    notify(source, f15Status(), 81);
    assert(source.hasData() && source.lastDataMs() == 81);
    source.reset();
    f15Connect(source);
    full = f15Status();
    source.onNotify(0, full.data(), 10, 90);
    source.tick(120);
    assert(!source.hasData());
    source.onNotify(0, full.data() + 10, 7, 121);
    assert(source.hasData());
    source.reset();
    f15Connect(source);
    source.onNotify(1, full.data(), full.size(), 1);
    source.onNotify(0, nullptr, 17, 1);
    assert(!source.hasData());
    auto oversized = Bytes(109,0);
    oversized[0] = 2;
    oversized[1] = 0x51;
    oversized[2] = 3;
    oversized[108] = 3;
    for (std::size_t i = 0; i < oversized.size(); i += 7) {
        source.onNotify(0, oversized.data() + i, std::min<std::size_t>(7, oversized.size() - i), 2);
    }
    source.tick(1002);
    assert(!source.hasData());
    notify(source, f15Status(), 1003);
    assert(source.hasData() && source.lastDataMs() == 1003);
    source.reset();
    f15Connect(source);
    const auto shortStarting = f15Frame({2,0x51,2,6});
    for (auto byte : shortStarting) source.onNotify(0, &byte, 1, 24);
    assert(!source.hasData());
    source.tick(1024);
    assert(source.hasData() && source.treadmillOperation() == TreadmillOperation::Starting);
    assert(source.sample().speed.value == 0 && source.sample().heartRate.value == 0);
    assert(!source.sample().incline.valid && !source.sample().elapsed.valid);
}
void f15ControlsAndLifecycleTests() {
    SportstechF15Treadmill source;
    f15Connect(source, 100);
    ble::GattWrite write;
    source.tick(1099);
    assert(!source.takeWrite(write));
    source.tick(1100);
    assert(source.takeWrite(write) && write.payload() == Bytes({2,0x51,0x51,3}));
    assert(!source.takeWrite(write));
    notify(source, f15Status(0), 1101);
    for (const ControlRequest request : {
        ControlRequest{Control::Speed,-1}, {Control::Speed,0.7F}, {Control::Speed,12.1F},
        {Control::Incline,-0.1F}, {Control::Incline,15.1F}, {Control::RunState,-1},
        {Control::RunState,0.5F}, {Control::RunState,3}, {Control::Resistance,1},
        {Control::Speed,std::numeric_limits<float>::quiet_NaN()},
        {Control::Incline,std::numeric_limits<float>::infinity()}}) {
        assert(!source.control(request, 1102));
        assert(!source.takeWrite(write));
    }
    assert(source.control({Control::Incline,15}, 1102));
    assert(source.takeWrite(write) && write.payload() == f15Target(8,15) && !write.withResponse);
    assert(source.control({Control::Speed,12}, 1103));
    assert(source.takeWrite(write) && write.payload() == f15Target(120,15));
    notify(source, f15Status(), 1104); // Measured incline must not replace desired target.
    assert(source.control({Control::Speed,5.14F}, 1105));
    assert(source.takeWrite(write) && write.payload() == f15Target(51,15));
    assert(source.control({Control::Incline,4.9F}, 1106));
    assert(source.takeWrite(write) && write.payload() == f15Target(51,4));
    assert(source.control({Control::Incline,0}, 1107));
    assert(source.takeWrite(write) && write.payload() == f15Target(51,0));
    assert(source.control({Control::Speed,0.8F}, 1108));
    assert(source.takeWrite(write) && write.payload() == f15Target(8,0));
    assert(!source.control({Control::RunState,1}, 1109)); // Already running.
    notify(source, f15Status(0), 1110);
    assert(source.control({Control::RunState,1}, 1111));
    assert(source.takeWrite(write) && write.payload() == Bytes({2,0x53,1,0,0,0,0,0,0x52,3}));
    assert(!source.control({Control::RunState,1}, 1112));
    assert(!source.control({Control::Speed,6}, 1112));
    notify(source, f15Status(2), 6000);
    source.tick(7110);
    while (source.takeWrite(write)) assert(write.payload() == Bytes({2,0x51,0x51,3}));
    source.tick(7111);
    assert(source.takeWrite(write) && write.payload() == f15Target(8,0));
    source.tick(7112);
    assert(!source.takeWrite(write));
    for (std::uint8_t hardwareStop : {0,4}) {
        source.reset();
        f15Connect(source);
        notify(source, f15Status(0), 1);
        assert(source.control({Control::RunState,1}, 2));
        assert(source.takeWrite(write));
        notify(source, f15Status(2), 1000);
        notify(source, f15Status(hardwareStop), 5999);
        source.tick(6002);
        while (source.takeWrite(write)) assert(write.payload() == Bytes({2,0x51,0x51,3}));
    }
    for (std::uint8_t confirmedMode : {2,3}) {
        source.reset();
        f15Connect(source);
        notify(source, f15Status(0), 1);
        source.tick(1000); // Poll queued before the explicit start.
        assert(source.control({Control::RunState,1}, 1001));
        assert(source.takeWrite(write) && write.payload() == Bytes({2,0x51,0x51,3}));
        assert(source.takeWrite(write) && write.payload() == Bytes({2,0x53,1,0,0,0,0,0,0x52,3}));
        notify(source, f15Status(0), 1002); // Old poll response does not cancel an unobserved start.
        notify(source, f15Status(confirmedMode), 6000);
        source.tick(7001);
        assert(source.takeWrite(write) && write.payload() == f15Target(8,0));
        while (source.takeWrite(write)) assert(write.payload() == Bytes({2,0x51,0x51,3}));
    }
    source.reset();
    f15Connect(source);
    notify(source, f15Status(0), 1);
    assert(source.control({Control::RunState,1}, 2));
    assert(source.takeWrite(write));
    notify(source, f15Status(0), 5999); // No confirmed successful start.
    source.tick(6002);
    while (source.takeWrite(write)) assert(write.payload() == Bytes({2,0x51,0x51,3}));
    for (const ControlRequest stopping : {ControlRequest{Control::RunState,0},
                                         {Control::RunState,2}, {Control::Speed,0}}) {
        source.reset();
        f15Connect(source);
        notify(source, f15Status(0), 1);
        assert(source.control({Control::RunState,1}, 2));
        assert(source.control(stopping, 3));
        assert(source.takeWrite(write) && write.payload() == Bytes({2,0x53,3,0x50,3}));
        assert(!source.takeWrite(write)); // Pending start was discarded.
        notify(source, f15Status(0), 5999);
        source.tick(6002);
        while (source.takeWrite(write)) assert(write.payload() == Bytes({2,0x51,0x51,3}));
    }
    source.reset();
    assert(!source.hasData() && !source.treadmillOperation() && !source.capabilities().controls);
    assert(!source.onConnected(1, 1)); // Endpoint discovery is per connection.
    f15Connect(source);
    notify(source, f15Status(0), 1);
    assert(source.control({Control::RunState,1}, 2));
    source.onWriteFailed(3);
    assert(source.failed() && !source.capabilities().controls && !source.pendingWrites());
    notify(source, f15Status(), 6001);
    source.tick(6002);
    assert(!source.takeWrite(write) && !source.control({Control::RunState,1}, 6003));
    source.reset();
    f15Connect(source);
    notify(source, f15Status(0), 1);
    assert(source.control({Control::RunState,1}, 2));
    source.reset();
    f15Connect(source);
    notify(source, f15Status(0), 6001);
    source.tick(6002);
    while (source.takeWrite(write)) assert(write.payload() == Bytes({2,0x51,0x51,3}));
    assert(source.control({Control::Incline,2}, 6003));
    assert(source.takeWrite(write) && write.payload() == f15Target(8,2)); // Targets reset.
    assert(!source.control({Control::Speed,5}, 11002));
    source.tick(11002);
    assert(!source.capabilities().controls);
    notify(source, f15Status(0), 11003);
    assert(source.capabilities().supports(Control::Speed));
    // Full queue rejects requests, never claims an unqueued target was applied.
    for (unsigned i = 0; i < 16; ++i) source.tick(11003 + i * 1000);
    notify(source, f15Status(0), 27003);
    assert(!source.control({Control::Speed,7}, 27003));
    assert(source.control({Control::RunState,0}, 27003));
    assert(source.takeWrite(write) && write.payload() == Bytes({2,0x53,3,0x50,3}));
    assert(!source.takeWrite(write));
}
void f15AdapterStatusTests() {
    std::uint32_t now = 1000;
    testing::FakeCentral central;
    BleInputAdapter adapter(central, [&now] { return now; });
    PairedDevice paired;
    paired.address = testing::address("aa:00:00:00:00:15");
    paired.adapterId = "sportstech-f15";
    paired.name = "FS-001";
    paired.valid = true;
    adapter.setPairedDevice(paired);
    adapter.begin();
    assert(adapter.scanAndConnect());
    assert(!adapter.telemetry().treadmillOperation);
    const auto service = ble::Uuid::from16(0xfff0);
    const auto characteristic = ble::Uuid::from16(0xfff2);
    Bytes endpoint(service.bytes.begin(), service.bytes.end());
    endpoint.insert(endpoint.end(), characteristic.bytes.begin(), characteristic.bytes.end());
    central.pushEvent(ble::CentralEventType::WriteEndpoint, 0, 0, endpoint);
    central.pushEvent(ble::CentralEventType::Connected, 1);
    adapter.loop();
    assert(adapter.isConnected() && !adapter.telemetry().treadmillOperation);
    central.pushEvent(ble::CentralEventType::Notification, 0, 0, f15Frame({2,0x51,1,1}));
    adapter.loop();
    assert(!adapter.telemetry().treadmillOperation); // ACK cannot invent a mode.
    for (TreadmillOperation mode : {TreadmillOperation::Stopped, TreadmillOperation::Starting,
                                    TreadmillOperation::Running, TreadmillOperation::Stopping}) {
        ++now;
        central.pushEvent(ble::CentralEventType::Notification, 0, 0,
                          f15Status(static_cast<std::uint8_t>(mode)));
        adapter.loop();
        assert(adapter.telemetry().connected);
        assert(adapter.telemetry().treadmillOperation == mode);
    }
    now += 5001;
    adapter.loop();
    assert(adapter.isConnected() && adapter.stale());
    assert(!adapter.telemetry().treadmillOperation);
    central.pushEvent(ble::CentralEventType::Notification, 0, 0, f15Status(3));
    adapter.loop();
    assert(!adapter.stale() && adapter.telemetry().treadmillOperation == TreadmillOperation::Running);
    central.pushEvent(ble::CentralEventType::Disconnected);
    adapter.loop();
    assert(!adapter.isConnected() && !adapter.telemetry().treadmillOperation);
}
}  // namespace

void runTreadmillTests() {
    matchTests();
    codecTests();
    lifecycleTests();
    telemetryTests();
    targetTests();
    f15MatchAndPlanTests();
    f15TelemetryTests();
    f15ControlsAndLifecycleTests();
    f15AdapterStatusTests();
}
