#include "protocols/treadmills.hpp"
#include "ble/central_transport.hpp"

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

void runTreadmillTests() {
    matchTests();
    codecTests();
    lifecycleTests();
    telemetryTests();
    targetTests();
}
