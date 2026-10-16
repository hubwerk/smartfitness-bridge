// Host tests for the Echelon rower / Stride / stair climber protocols.
//
// GPL-3.0 port of qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift),
// Copyright (C) Roberto Viola and contributors. Pinned revision
// ea70d7ff64bc8bf508fc6401fd056211e05311b3: src/devices/echelonrower/,
// src/devices/echelonstride/, src/devices/echelonstairclimber/ and
// src/devices/bluetooth.cpp. Frames below are synthetic, built in the
// upstream layout; no hardware captures are available.

#include "protocols/echelon_ext.hpp"
#include "protocols/registry.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {
using namespace smartfitness;
using namespace smartfitness::protocols;
using ble::Bytes;

bool near(float a, float b) { return std::fabs(a - b) < 0.01F; }

ble::Advertisement advertisement(const char* name, bool ftms = false) {
    ble::Advertisement out;
    out.name = name;
    if (ftms) out.services.push_back(ble::Uuid::from16(0x1826));
    return out;
}

void notify(SourceProtocol& source, const Bytes& frame, std::uint32_t now) {
    source.onNotify(0, frame.data(), frame.size(), now);
}

std::vector<Bytes> drain(SourceProtocol& source) {
    std::vector<Bytes> out;
    ble::GattWrite write;
    while (source.takeWrite(write)) {
        assert(write.service == echelon::kService && write.characteristic == echelon::kWrite);
        assert(write.withResponse);
        out.push_back(write.payload());
    }
    return out;
}

const ProtocolInfo* localDetect(const ble::Advertisement& adv) {
    const ProtocolInfo* best = nullptr;
    for (std::size_t i = 0; i < echelon_ext::descriptorCount(); ++i) {
        const auto& info = echelon_ext::descriptorAt(i);
        if (info.matches(adv) && (!best || info.priority > best->priority)) best = &info;
    }
    return best;
}

bool resolves(const char* name, const char* id, bool ftms = false) {
    const auto* info = localDetect(advertisement(name, ftms));
    return id ? info && std::strcmp(info->id, id) == 0 : info == nullptr;
}

void frameTests() {
    assert(echelon_ext::withChecksum({0xf0, 0xb1, 0x01, 0x10}) == Bytes({0xf0, 0xb1, 0x01, 0x10, 0xb2}));
    const Bytes init1{0xf0, 0xa1, 0x00, 0x91}, init2{0xf0, 0xa3, 0x00, 0x93}, start{0xf0, 0xb0, 0x01, 0x01, 0xa2};
    assert(echelon_ext::rowerInitFrames() == std::vector<Bytes>({init1, init1, init1, init1, init2, init1, start}));
    const Bytes a4{0xf0, 0xa4, 0x00, 0x94};
    assert(echelon_ext::strideInitFrames(false) == std::vector<Bytes>({a4, init1, init1, init1, init1, init2, init1}));
    const Bytes s4{0xf0, 0xe0, 0xfd, 0x3e, 0x65, 0x48, 0xd5, 0x8d};
    assert(echelon_ext::strideInitFrames(true) == std::vector<Bytes>({a4, a4, s4, a4, init1, init1, init1, init1, init2}));
    const auto stair = echelon_ext::stairInitFrames();
    assert(stair.size() == 11);
    assert(stair[1] == Bytes({0xf0, 0xe0, 0x85, 0xf8, 0x03, 0x68, 0xd2, 0x8a}));
    assert(stair[3] == Bytes({0xf0, 0xe0, 0x38, 0x04, 0x03, 0xb5, 0x14, 0xd8}));
    assert(stair[9] == start && stair[10] == Bytes({0xf0, 0xa5, 0x00, 0x95}));
    assert(echelon_ext::strideInclineFrame(7) == Bytes({0xf0, 0xb1, 0x01, 0x07, 0xa9}));

    // Rower watt table (upstream wattsFromResistance(), truncated to uint16).
    assert(echelon_ext::rowerPower(10, 0.0F) == 0.0F);
    assert(echelon_ext::rowerPower(10, 24.0F) == 141.0F);  // 113 + (148-113)/5*4
    assert(echelon_ext::rowerPower(1, 60.0F) == 189.0F);   // 60/55 * 174
    assert(echelon_ext::rowerPower(99, 10.0F) == 90.0F);   // clamped to level 32
    assert(echelon_ext::rowerPower(-3, 7.0F) == 12.0F);    // level 0: 0 + (30-0)/5*2
    assert(echelon_ext::rowerPower(5, 4.0F) == 0.0F);      // Epsilon columns
}

Bytes rowerData(std::uint8_t cadence, std::uint16_t pace) {
    Bytes frame(21, 0);
    frame[0] = 0xf0;
    frame[1] = 0xd1;
    frame[11] = cadence;
    frame[13] = static_cast<std::uint8_t>(pace >> 8);
    frame[14] = static_cast<std::uint8_t>(pace);
    return frame;
}

void rowerTests() {
    EchelonRowerSource rower;
    rower.reset();
    assert(rower.profile() == MachineProfile::Rower);
    const auto plan = rower.plan();
    assert(plan.subscriptionCount == 2 && plan.subscriptions[0].characteristic == echelon::kNotify1 &&
           plan.subscriptions[1].characteristic == echelon::kNotify2);
    assert(!rower.capabilities().supports(Control::Resistance));
    assert(!rower.control({Control::Resistance, 50.0F}, 0));
    assert(!rower.onConnected(0, 0));
    assert(rower.onConnected(0x1, 1000));
    assert(drain(rower) == echelon_ext::rowerInitFrames());
    rower.tick(2999);
    assert(rower.pendingWrites() == 0);
    rower.tick(3000);
    assert(drain(rower) == std::vector<Bytes>({{0xf0, 0xa0, 0x01, 0x01, 0x92}}));

    // Resistance report (synthetic f0 d2 01 <level> <sum>).
    notify(rower, echelon_ext::withChecksum({0xf0, 0xd2, 0x01, 0x0a}), 3100);
    assert(rower.sample().resistance.valid && rower.sample().resistance.value == 10.0F);
    assert(!rower.hasData());

    // Synthetic 21-byte data frame split over two notifications (10 + 11 bytes).
    const Bytes data = rowerData(24, 120);
    notify(rower, Bytes(data.begin(), data.begin() + 10), 3200);
    assert(!rower.hasData());
    notify(rower, Bytes(data.begin() + 10, data.end()), 3210);
    assert(rower.hasData() && rower.lastDataMs() == 3210);
    const auto& s = rower.sample();
    assert(s.cadence.value == 24.0F && s.pace.value == 120.0F && near(s.speed.value, 15.0F));
    assert(s.power.value == 141.0F);
    assert(!s.distance.valid && !s.elapsed.valid && !s.heartRate.valid && !s.strokeCount.valid);

    // Not moving: pace/speed 0.
    notify(rower, rowerData(0, 120), 3300);
    assert(rower.sample().speed.value == 0.0F && rower.sample().pace.value == 0.0F && rower.sample().power.value == 0.0F);

    // Malformed frames are ignored.
    notify(rower, Bytes{0xf0, 0xd2, 0x01}, 4000);
    notify(rower, Bytes(20, 0xf0), 4000);
    assert(rower.lastDataMs() == 3300);

    assert(rower.capabilities().supports(Control::Resistance));
    assert(!rower.capabilities().supports(Control::Speed));
    assert(rower.capabilities().supports(Metric::Power) && rower.capabilities().supports(Metric::Pace));
    assert(rower.control({Control::Resistance, 100.0F}, 0));
    assert(rower.control({Control::Resistance, 0.0F}, 0));
    assert(!rower.control({Control::Speed, 5.0F}, 0));
    assert(drain(rower) == std::vector<Bytes>({{0xf0, 0xb1, 0x01, 0x20, 0xc2}, {0xf0, 0xb1, 0x01, 0x01, 0xa3}}));

    rower.reset();
    assert(!rower.hasData() && !rower.sample().resistance.valid);
}

void strideTests() {
    EchelonStrideSource stride(false);
    stride.reset();
    assert(std::strcmp(stride.id(), "echelon-stride") == 0 && stride.profile() == MachineProfile::Treadmill);
    assert(!stride.control({Control::Incline, 3.0F}, 0));
    assert(stride.onConnected(0x2, 0));
    assert(drain(stride) == echelon_ext::strideInitFrames(false));
    assert(!stride.capabilities().supports(Control::Incline));

    // Synthetic speed frame f0 d3 02 0b b8 <sum>: 3000/1000 mph -> km/h; echoed back.
    const Bytes speed = echelon_ext::withChecksum({0xf0, 0xd3, 0x02, 0x0b, 0xb8});
    notify(stride, speed, 100);
    assert(near(stride.sample().speed.value, 3.0F * 1.60934F) && stride.hasData());
    assert(drain(stride) == std::vector<Bytes>({speed}));

    const Bytes incline = echelon_ext::withChecksum({0xf0, 0xd2, 0x01, 0x05});
    notify(stride, incline, 200);
    assert(stride.sample().incline.value == 5.0F);
    assert(drain(stride) == std::vector<Bytes>({incline}));

    const Bytes ack{0xf0, 0xd0, 0x01, 0x00, 0xc1};
    notify(stride, ack, 300);
    assert(stride.lastDataMs() == 200);
    assert(drain(stride) == std::vector<Bytes>({ack}));

    // Non-STRIDE4 d1 frame: heart rate in byte 11; 0 means no reading.
    Bytes d1(13, 0);
    d1[0] = 0xf0;
    d1[1] = 0xd1;
    d1[7] = 0x01;
    d1[8] = 0xf4;
    d1[11] = 120;
    notify(stride, d1, 400);
    assert(stride.sample().heartRate.valid && stride.sample().heartRate.value == 120.0F);
    assert(near(stride.sample().speed.value, 3.0F * 1.60934F));  // d1 speed only on STRIDE4
    assert(stride.pendingWrites() == 0);
    d1[11] = 0;
    notify(stride, d1, 500);
    assert(!stride.sample().heartRate.valid);

    // Truncated frames are ignored and not echoed.
    notify(stride, Bytes{0xf0, 0xd3, 0x02, 0x0b}, 600);
    notify(stride, Bytes{0xf0, 0xd2, 0x01}, 600);
    notify(stride, Bytes{0xf0}, 600);
    assert(stride.lastDataMs() == 500 && stride.pendingWrites() == 0);

    // Controls: incline only, upstream bytes, 0..15 %.
    const auto caps = stride.capabilities();
    assert(caps.supports(Control::Incline) && !caps.supports(Control::Speed) && !caps.supports(Control::Resistance));
    assert(caps.supports(Metric::Speed) && caps.supports(Metric::Incline) && caps.supports(Metric::HeartRate));
    assert(!stride.control({Control::Speed, 5.0F}, 700));
    assert(!stride.control({Control::Incline, 15.5F}, 700));
    assert(!stride.control({Control::Incline, std::nanf("")}, 700));
    assert(stride.pendingWrites() == 0);
    assert(stride.control({Control::Incline, 7.6F}, 700));
    assert(stride.control({Control::Incline, -2.0F}, 700));
    assert(stride.control({Control::Incline, 15.0F}, 700));
    assert(drain(stride) == std::vector<Bytes>({{0xf0, 0xb1, 0x01, 0x07, 0xa9},
                                                {0xf0, 0xb1, 0x01, 0x00, 0xa2},
                                                {0xf0, 0xb1, 0x01, 0x0f, 0xb1}}));
    stride.tick(2000);
    assert(drain(stride) == std::vector<Bytes>({{0xf0, 0xa0, 0x01, 0x01, 0x92}}));
    // Disconnect: nothing is sent.
    stride.reset();
    assert(stride.pendingWrites() == 0 && !stride.capabilities().supports(Control::Incline));

    EchelonStrideSource stride4(true);
    stride4.reset();
    assert(std::strcmp(stride4.id(), "echelon-stride4") == 0);
    assert(stride4.onConnected(0x3, 0));
    assert(drain(stride4) == echelon_ext::strideInitFrames(true));
    d1[11] = 120;
    notify(stride4, d1, 100);
    assert(near(stride4.sample().speed.value, 5.0F * 1.60934F) && !stride4.sample().heartRate.valid);
    assert(stride4.pendingWrites() == 0);
    notify(stride4, Bytes{0xf0, 0xd1, 0, 0, 0, 0, 0, 1}, 200);
    assert(stride4.lastDataMs() == 100);
}

void stairTests() {
    EchelonStairSource stair;
    stair.reset();
    assert(stair.profile() == MachineProfile::StairClimber);
    assert(stair.onConnected(0x1, 0));
    assert(drain(stair) == echelon_ext::stairInitFrames());
    // Synthetic d1 frame: steps 300 (bytes 5..6), elevation 10 (7..8), 64 steps/min (9).
    const Bytes frame{0xf0, 0xd1, 0x07, 0, 0, 0x01, 0x2c, 0x00, 0x0a, 64, 0, 0};
    notify(stair, frame, 50);
    const auto& s = stair.sample();
    assert(s.stepCount.value == 300.0F && s.positiveElevationGain.value == 10.0F && s.cadence.value == 64.0F);
    assert(near(s.speed.value, 20.0F) && !s.heartRate.valid && !s.power.valid);
    notify(stair, Bytes(frame.begin(), frame.begin() + 9), 60);
    notify(stair, Bytes{0xf0, 0xd2, 0x01, 0x05, 0xc8}, 60);
    assert(stair.lastDataMs() == 50 && stair.pendingWrites() == 0);
    assert(stair.capabilities().controls == 0 && !stair.isTrainer());
    assert(!stair.control({Control::Incline, 2.0F}, 70) && !stair.control({Control::Resistance, 50.0F}, 70));
    stair.tick(2000);
    assert(drain(stair) == std::vector<Bytes>({{0xf0, 0xa0, 0x01, 0x01, 0x92}}));
}

void matcherTests() {
    assert(resolves("ECH-ROW-1234", "echelon-rower"));
    assert(resolves("ech-row", "echelon-rower"));
    assert(resolves("ROWSPORT-1", "echelon-rower"));
    assert(resolves("ROW-7S-0001", "echelon-rower"));
    assert(resolves("Row-S123", "echelon-rower"));
    assert(resolves("ECH-STRIDE-55", "echelon-stride"));
    assert(resolves("ECH-UK-1", "echelon-stride"));
    assert(resolves("ECH-FR-1", "echelon-stride"));
    assert(resolves("STRIDE6S-1234", "echelon-stride"));
    assert(resolves("Stride-1", "echelon-stride"));
    assert(resolves("ECH-SD-SPT-1", "echelon-stride"));
    assert(resolves("STRIDE4-0001", "echelon-stride4"));
    assert(!echelon_ext::descriptorAt(1).matches(advertisement("STRIDE4-0001")));
    assert(resolves("ECH-EC-SPT-1", "echelon-stair"));
    assert(!echelon_ext::descriptorAt(1).matches(advertisement("ECH-EC-SPT-1")));
    // Negatives: other Echelon bikes, generic rower names and FTMS advertisements.
    assert(resolves("ECH-SPORT-1", nullptr));
    assert(resolves("ECH-SC-1", nullptr));
    assert(resolves("ROWER 1", nullptr));
    assert(resolves("PM5 123 ROW", nullptr));
    assert(resolves("XSTRIDE", nullptr));
    assert(resolves("", nullptr));
    assert(resolves("ECH-ROW-1", nullptr, true));
    assert(resolves("ECH-STRIDE-1", nullptr, true));
    assert(resolves("STRIDE4-1", nullptr, true));
    assert(resolves("ECH-EC-SPT-1", nullptr, true));
}

void descriptorTests() {
    assert(echelon_ext::descriptorCount() == 4);
    std::set<std::string> ids;
    for (std::size_t i = 0; i < echelon_ext::descriptorCount(); ++i) {
        const auto& info = echelon_ext::descriptorAt(i);
        const std::string id = info.id;
        assert(!id.empty() && id.size() <= 16 && ids.insert(id).second);
        for (char c : id) assert((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
        assert(info.priority >= 85 && !info.advertisementOnly);
        auto source = info.create();
        assert(source && id == source->id() && source->profile() == info.profile);
        source->reset();
        assert(source->onConnected(0x3, 0) && source->pendingWrites() > 0 && source->pendingWrites() <= 16);
        assert(source->plan().subscriptionCount == 2);
    }
    assert(echelon_ext::descriptorAt(0).profile == MachineProfile::Rower);
    assert(echelon_ext::descriptorAt(1).profile == MachineProfile::Treadmill);
    assert(echelon_ext::descriptorAt(2).profile == MachineProfile::Treadmill);
    assert(echelon_ext::descriptorAt(3).profile == MachineProfile::StairClimber && !echelon_ext::descriptorAt(3).controllable);
}

void expectGlobal(const char* name, const char* id) {
    const auto* info = detectProtocol(advertisement(name));
    assert(info && std::strcmp(info->id, id) == 0);
    assert(findProtocol(id) != nullptr);
}

}  // namespace

void runEchelonExtTests() {
    frameTests();
    rowerTests();
    strideTests();
    stairTests();
    matcherTests();
    descriptorTests();
    std::printf("echelon_ext host tests passed (%u protocols)\n", static_cast<unsigned>(echelon_ext::descriptorCount()));
}

// Requires the descriptors to be registered in protocols/registry.cpp.
void runEchelonExtRegistryTests() {
    expectGlobal("ECH-ROW-1234", "echelon-rower");
    expectGlobal("ROWSPORT-1", "echelon-rower");
    expectGlobal("ECH-STRIDE-55", "echelon-stride");
    expectGlobal("ECH-UK-1", "echelon-stride");
    expectGlobal("ECH-SD-SPT-1", "echelon-stride");
    expectGlobal("STRIDE4-0001", "echelon-stride4");
    expectGlobal("ECH-EC-SPT-1", "echelon-stair");
    const auto* bike = detectProtocol(advertisement("ECH-SPORT-1"));
    assert(bike && std::strcmp(bike->id, "echelon") == 0);
    for (std::size_t i = 0; i < echelon_ext::descriptorCount(); ++i) {
        const auto* info = findProtocol(echelon_ext::descriptorAt(i).id);
        assert(info && info->profile == echelon_ext::descriptorAt(i).profile);
    }
}
