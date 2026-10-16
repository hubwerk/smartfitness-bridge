// Host tests for the FitShow-family protocols (protocols/fitshow.hpp).
//
// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{fitshowtreadmill,fitplusbike,fitplusrower,snodebike}/ (same-named
// .cpp/.h) and src/devices/bluetooth.cpp. Frames below are synthetic, built in
// the upstream byte layouts; no hardware captures are used.

#include "protocols/fitshow.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "ftms/ftms_codec.hpp"
#include "protocols/registry.hpp"

namespace {
using namespace smartfitness;
using namespace smartfitness::protocols;
using ble::Bytes;
using fitshow::FitPlusModel;
using fitshow::TreadmillVariant;

bool near(float a, float b) { return std::fabs(a - b) < 0.01F; }

ble::Advertisement ad(const std::string& name, bool ftms = false) {
    ble::Advertisement a;
    a.name = name;
    if (ftms) a.services.push_back(ble::Uuid::from16(0x1826));
    return a;
}

std::vector<Bytes> drain(SourceProtocol& source) {
    std::vector<Bytes> out;
    ble::GattWrite write;
    while (source.takeWrite(write)) out.push_back(write.payload());
    return out;
}

void notify(SourceProtocol& source, std::size_t index, const Bytes& bytes, std::uint32_t now) {
    source.onNotify(index, bytes.data(), bytes.size(), now);
}

// Synthetic 16-byte status frame in the upstream (non-AnyRun) layout.
Bytes status(std::uint8_t state, std::uint8_t speed, std::uint8_t incline, std::uint16_t elapsed,
             std::uint16_t steps, std::uint8_t heart) {
    return fitshow::frame({0x51, state, speed, incline, static_cast<std::uint8_t>(elapsed),
                           static_cast<std::uint8_t>(elapsed >> 8), 0x0c, 0x00, 0x05, 0x00,
                           static_cast<std::uint8_t>(steps), static_cast<std::uint8_t>(steps >> 8), heart, 0x00});
}

// Connects, then acknowledges every init command: speed 1.0..18.0 km/h, incline 0..15.
std::uint32_t connectTreadmill(FitshowTreadmillSource& source, std::uint32_t mask = 1) {
    source.reset();
    assert(source.onConnected(mask, 0));
    std::uint32_t now = 0;
    for (int guard = 0; guard < 40 && source.queued(); ++guard) {
        now += fitshow::kTickMs;
        source.tick(now);
        const auto writes = drain(source);
        if (writes.empty()) continue;
        const Bytes& sent = writes.front();
        Bytes reply{sent[1], sent[2]};
        if (sent[1] == 0x50 && sent[2] == 0x02) reply = {0x50, 0x02, 180, 10};
        if (sent[1] == 0x50 && sent[2] == 0x03) reply = {0x50, 0x03, 15, 0};
        notify(source, 0, fitshow::frame(reply), now);
    }
    assert(source.queued() == 0);
    return now;
}

void testFraming() {
    assert((fitshow::frame({0x51}) == Bytes{0x02, 0x51, 0x51, 0x03}));
    assert((fitshow::frame(fitshow::targetPayload(80, 2)) == Bytes{0x02, 0x53, 0x02, 0x50, 0x02, 0x03, 0x03}));
    const Bytes ok = fitshow::frame({0x50, 0x02, 180, 10});
    assert(fitshow::validFrame(ok.data(), ok.size()));
    Bytes bad = ok;
    bad[3] ^= 1;
    assert(!fitshow::validFrame(bad.data(), bad.size()));
    assert(!fitshow::validFrame(ok.data(), 3));
    assert(!fitshow::validFrame(nullptr, 0));

    const auto init = fitshow::treadmillInitPayloads(false);
    assert(init.size() == 6);
    assert((init[0] == Bytes{0x53, 0x00, 0xaa, 0x13, 110, 30, 75}));
    assert((init[1] == Bytes{0x50, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00}));
    assert((init[2] == Bytes{0x50, 0x02}) && (init[3] == Bytes{0x50, 0x03}));
    assert((init[4] == Bytes{0x50, 0x04}) && (init[5] == Bytes{0x50, 0x01}));
    const auto anyrun = fitshow::treadmillInitPayloads(true);
    assert(anyrun.size() == 2);
    assert((anyrun[0] == Bytes{0x53, 0x00, 0xaa, 0x13, 110, 30, 75, 170}));
    assert((anyrun[1] == Bytes{0x50, 0x05}));
    fitshow::ClockTime t;
    t.yearSince2000 = 26;
    t.month = 10;
    t.day = 16;
    t.hour = 11;
    t.minute = 33;
    t.second = 16;
    assert((fitshow::treadmillInitPayloads(false, t)[1] == Bytes{0x50, 0x00, 26, 10, 16, 11, 33, 16}));
}

void testTreadmill() {
    FitshowTreadmillSource source;
    assert(std::string(source.id()) == "fitshow-tm" && source.profile() == MachineProfile::Treadmill);
    const auto plan = source.plan();
    assert(plan.subscriptionCount == 4);
    assert(plan.subscriptions[0].service == ble::Uuid::from16(0xfff0));
    assert(plan.subscriptions[1].characteristic == ble::Uuid::from16(0xffe4));
    assert(plan.subscriptions[2].characteristic == ble::Uuid::from16(0xae02));
    assert(plan.subscriptions[3].characteristic == ble::Uuid::from16(0x2a53));

    source.reset();
    assert(!source.onConnected(0x8, 0));  // RSC only
    assert(!source.lastError().empty());

    // Write path, retries and drop after three unanswered attempts.
    source.reset();
    assert(source.onConnected(0x1, 0));
    source.tick(100);
    assert(source.pendingWrites() == 0);
    source.tick(200);
    ble::GattWrite write;
    assert(source.takeWrite(write));
    assert(write.service == ble::Uuid::from16(0xfff0) && write.characteristic == ble::Uuid::from16(0xfff2));
    assert(!write.withResponse);
    assert((write.payload() == fitshow::frame({0x53, 0x00, 0xaa, 0x13, 110, 30, 75})));
    source.tick(400);
    assert(source.pendingWrites() == 0);  // even retry ticks wait
    source.tick(600);
    source.tick(800);
    source.tick(1000);
    source.tick(1200);
    assert(drain(source).size() == 2);
    assert(source.queued() == 6);
    source.tick(1400);  // sixth tick: dropped, next command sent
    auto writes = drain(source);
    assert(source.queued() == 5);
    assert(writes.size() == 1 && writes[0][1] == 0x50 && writes[0][2] == 0x00);

    // ffe0 selected when fff0 is missing; ae00 notifications ignored.
    FitshowTreadmillSource ffe;
    connectTreadmill(ffe, 0x6);
    ffe.tick(100000);
    assert(ffe.takeWrite(write) && write.characteristic == ble::Uuid::from16(0xffe1));
    assert((write.payload() == Bytes{0x02, 0x51, 0x51, 0x03}));  // status poll
    notify(ffe, 2, status(3, 50, 0, 1, 0, 0), 100000);
    assert(!ffe.hasData());

    // Running frame.
    std::uint32_t now = connectTreadmill(source);
    assert(source.maxSpeedTenths() == 180 && source.minSpeedTenths() == 10);
    assert(source.maxIncline() == 15 && source.minIncline() == 0);
    assert(!source.capabilities().supports(Control::Speed));
    assert(!source.control({Control::Speed, 5.0F}, now));  // never while not running
    now += 10;
    notify(source, 0, status(3, 65, 2, 0x0102, 345, 120), now);
    assert(source.hasData() && source.running(now));
    const auto& s = source.sample();
    assert(near(s.speed.value, 6.5F) && near(s.incline.value, 2.0F));
    assert(near(s.elapsed.value, 258.0F) && near(s.stepCount.value, 345.0F));
    assert(near(s.heartRate.value, 120.0F) && s.heartRate.valid);
    assert(!s.distance.valid && !s.totalEnergy.valid && !s.power.valid);
    auto caps = source.capabilities();
    assert(caps.supports(Control::Speed) && caps.supports(Control::Incline));

    // Speed request keeps the current incline; clamps to the reported max.
    assert(source.control({Control::Speed, 8.0F}, now));
    assert(source.queued() == 1);
    source.tick(now + 200);
    writes = drain(source);
    assert(writes.size() == 1 && (writes[0] == fitshow::frame({0x53, 0x02, 80, 2})));
    notify(source, 0, fitshow::frame({0x53, 0x02, 80, 2}), now + 210);  // ack
    assert(source.queued() == 0);
    assert(source.control({Control::Speed, 25.0F}, now + 220));
    source.tick(now + 400);
    writes = drain(source);
    assert(writes.size() == 1 && (writes[0] == fitshow::frame({0x53, 0x02, 180, 2})));
    notify(source, 0, fitshow::frame({0x53, 0x02}), now + 410);

    // Incline request keeps the current speed, rounds to 1 % and clamps.
    assert(source.control({Control::Incline, 4.4F}, now + 420));
    source.tick(now + 600);
    writes = drain(source);
    assert(writes.size() == 1 && (writes[0] == fitshow::frame({0x53, 0x02, 65, 4})));
    notify(source, 0, fitshow::frame({0x53, 0x02}), now + 610);
    assert(source.control({Control::Incline, 40.0F}, now + 620));
    source.tick(now + 800);
    writes = drain(source);
    assert(writes.size() == 1 && (writes[0] == fitshow::frame({0x53, 0x02, 65, 15})));
    notify(source, 0, fitshow::frame({0x53, 0x02}), now + 810);
    assert(!source.control({Control::Resistance, 50.0F}, now + 820));
    assert(!source.control({Control::Speed, 5.0F}, now + 10 + FitshowTreadmillSource::kStatusFreshMs + 1));

    // Truncated and corrupted frames are ignored.
    const auto before = source.sample();
    Bytes truncated = fitshow::frame({0x51, 0x03, 90, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    notify(source, 0, truncated, now + 900);
    Bytes corrupt = status(3, 90, 0, 0, 0, 0);
    corrupt[3] ^= 0xff;
    notify(source, 0, corrupt, now + 900);
    assert(near(source.sample().speed.value, before.speed.value));

    // Stop frame: speed 0, sport data requested once.
    notify(source, 0, status(4, 65, 2, 300, 0, 0), now + 1000);
    assert(near(source.sample().speed.value, 0.0F) && !source.sample().heartRate.valid);
    assert(source.queued() == 1);
    notify(source, 0, status(4, 65, 2, 300, 0, 0), now + 1010);
    assert(source.queued() == 1);
    source.tick(now + 1200);
    writes = drain(source);
    assert(writes.size() == 1 && (writes[0] == fitshow::frame({0x52, 0x00})));
    notify(source, 0, fitshow::frame({0x52, 0x00, 0x2c, 0x01, 0, 0, 0, 0, 0x10, 0x02, 0, 0}), now + 1210);
    assert(source.queued() == 0);
    assert(near(source.sample().elapsed.value, 300.0F) && near(source.sample().stepCount.value, 528.0F));
    assert(!source.capabilities().supports(Control::Speed));

    // Short normal status zeroes speed and incline.
    notify(source, 0, status(3, 30, 3, 0, 0, 0), now + 1300);
    notify(source, 0, fitshow::frame({0x51, 0x00}), now + 1310);
    assert(near(source.sample().speed.value, 0.0F) && near(source.sample().incline.value, 0.0F));

    // RSC cadence (index 3): flags, speed u16, cadence.
    notify(source, 3, Bytes{0x00, 0x00, 0x01, 150}, now + 1400);
    assert(near(source.sample().cadence.value, 150.0F));
    notify(source, 3, Bytes{0x00, 0x00, 0x01}, now + 1400);
}

void testTreadmillVariants() {
    FitshowTreadmillSource miles(TreadmillVariant::Miles);
    std::uint32_t now = connectTreadmill(miles);
    notify(miles, 0, status(3, 50, 0, 0, 0, 0), now);
    assert(near(miles.sample().speed.value, 8.0467F));
    assert(miles.control({Control::Speed, 16.0934F}, now));
    miles.tick(now + 200);
    auto writes = drain(miles);
    assert(writes.size() == 1 && (writes[0] == fitshow::frame({0x53, 0x02, 100, 0})));

    FitshowTreadmillSource anyrun(TreadmillVariant::AnyRun);
    anyrun.reset();
    assert(anyrun.onConnected(1, 0));
    assert(anyrun.queued() == 2);
    notify(anyrun, 0, fitshow::frame({0x51, 0x03, 40, 0, 2, 5, 0, 0, 0, 0, 0x01, 0x02, 0, 0}), 10);
    assert(near(anyrun.sample().elapsed.value, 125.0F) && near(anyrun.sample().stepCount.value, 258.0F));
    notify(anyrun, 0, fitshow::frame({0x51, 0x04, 0, 0, 2, 5, 0, 0, 0, 0, 0x01, 0x02, 0, 0}), 20);
    assert(anyrun.queued() == 2);  // no sport-data request in AnyRun mode

    FitshowTreadmillSource noble(TreadmillVariant::NoblePro);
    now = connectTreadmill(noble);
    notify(noble, 0, status(3, 40, 6, 0, 0, 0), now);
    assert(near(noble.sample().incline.value, 3.0F));
    assert(noble.control({Control::Incline, 4.3F}, now));
    noble.tick(now + 200);
    writes = drain(noble);
    assert(writes.size() == 1 && (writes[0] == fitshow::frame({0x53, 0x02, 40, 9})));

    // Incline not supported (short 50 03 answer): incline reported as 0, no control.
    FitshowTreadmillSource flat;
    flat.reset();
    assert(flat.onConnected(1, 0));
    notify(flat, 0, fitshow::frame({0x50, 0x03}), 1);
    assert(flat.maxIncline() == 0);
    notify(flat, 0, status(3, 30, 5, 0, 0, 0), 2);
    assert(near(flat.sample().incline.value, 0.0F));
    assert(!flat.capabilities().supports(Control::Incline));
    assert(!flat.control({Control::Incline, 2.0F}, 3));
}

void testFitPlusFrames() {
    assert((fitshow::fitplusInitFrames(FitPlusModel::Merach) == std::vector<Bytes>{{0xaa, 0x01, 0x00, 0x01, 0x55}}));
    assert(fitshow::fitplusInitFrames(FitPlusModel::Rower) == fitshow::fitplusInitFrames(FitPlusModel::Merach));
    assert(fitshow::fitplusInitFrames(FitPlusModel::Generic).size() == 5);
    assert(fitshow::fitplusInitFrames(FitPlusModel::H9110).size() == 7);
    assert(fitshow::fitplusInitFrames(FitPlusModel::Etappe).size() == 9);
    assert(fitshow::fitplusInitFrames(FitPlusModel::SX600).size() == 10);
    assert((fitshow::fitplusInitFrames(FitPlusModel::Generic)[2] ==
            Bytes{0x02, 0x44, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x46, 0xaa, 0x19, 0x00, 0xbb, 0x03}));
    assert((fitshow::fitplusPollFrame(FitPlusModel::Merach, 0) == Bytes{0x02, 0x42, 0x42, 0x03}));
    assert((fitshow::fitplusPollFrame(FitPlusModel::Merach, 1) == Bytes{0x02, 0x43, 0x01, 0x42, 0x03}));
    assert((fitshow::fitplusPollFrame(FitPlusModel::SX600, 1) == Bytes{0x02, 0x02, 0x43, 0x01, 0x42, 0x03, 0x01, 0x03}));
    assert((fitshow::fitplusPollFrame(FitPlusModel::SX600, 4) == Bytes{0x02, 0x53, 0x9f, 0x02, 0x00, 0x00, 0xce, 0x03}));
    assert((fitshow::fitplusResistanceFrame(FitPlusModel::Merach, 1) == Bytes{0x02, 0x44, 0x05, 0x01, 0xf9, 0xb9, 0x03}));
    assert((fitshow::fitplusResistanceFrame(FitPlusModel::Rower, 9) == Bytes{0x02, 0x44, 0x05, 0x09, 0x00, 0x48, 0x03}));
    assert((fitshow::fitplusResistanceFrame(FitPlusModel::Etappe, 32) == Bytes{0x02, 0x44, 0x05, 0x20, 0x00, 0x61, 0x03}));
    assert((fitshow::fitplusResistanceFrame(FitPlusModel::SX600, 1) == Bytes{0x02, 0x44, 0x05, 0x00, 0x00, 0x41, 0x03}));
    assert((fitshow::fitplusResistanceFrame(FitPlusModel::SX600, 17) == Bytes{0x02, 0x44, 0x05, 0x11, 0x00, 0x50, 0x03}));
    assert(fitshow::fitplusResistanceFrame(FitPlusModel::Merach, 0).empty());
    assert(fitshow::fitplusResistanceFrame(FitPlusModel::Merach, 33).empty());
    assert(fitshow::fitplusResistanceFrame(FitPlusModel::Generic, 5).empty());
    assert(fitshow::fitplusDefaultMaxResistance(FitPlusModel::Merach) == 24);
    assert(fitshow::fitplusDefaultMaxResistance(FitPlusModel::H9110) == 32);
}

// Synthetic 15-byte FitPlus data frame (resistance @5, cadence @6, power x10 @9..10).
Bytes longFrame(std::uint8_t resistance, std::uint8_t cadence, std::uint16_t powerTenths) {
    return {0x02, 0x42, 0x02, 0x00, 0x00, resistance, cadence, 0x00, 0x00,
            static_cast<std::uint8_t>(powerTenths), static_cast<std::uint8_t>(powerTenths >> 8), 0x00, 0x00, 0x00, 0x03};
}

void testFitPlusSources() {
    FitPlusSource merach(FitPlusModel::Merach);
    assert(std::string(merach.id()) == "fitplus-merach" && merach.profile() == MachineProfile::IndoorBike);
    assert(merach.plan().subscriptionCount == 1);
    merach.reset();
    assert(!merach.onConnected(0, 0) && !merach.lastError().empty());
    merach.reset();
    assert(merach.onConnected(1, 0));
    ble::GattWrite write;
    assert(merach.takeWrite(write));
    assert(write.service == ble::Uuid::from16(0xfff0) && write.characteristic == ble::Uuid::from16(0xfff2));
    assert((write.payload() == Bytes{0xaa, 0x01, 0x00, 0x01, 0x55}) && !write.withResponse);
    merach.tick(200);
    merach.tick(400);  // writes pending: no extra poll
    assert(drain(merach).size() == 1);
    merach.tick(600);
    auto writes = drain(merach);
    assert(writes.size() == 1 && (writes[0] == Bytes{0x02, 0x43, 0x01, 0x42, 0x03}));

    notify(merach, 0, longFrame(10, 80, 1234), 700);
    assert(merach.hasData());
    assert(near(merach.sample().resistance.value, 10) && near(merach.sample().cadence.value, 80));
    assert(near(merach.sample().power.value, 123.4F) && !merach.sample().speed.valid);
    notify(merach, 0, longFrame(10, 0, 1234), 800);
    assert(near(merach.sample().power.value, 0.0F));
    const auto before = merach.sample();
    Bytes thirteen(13, 0x02);
    notify(merach, 0, thirteen, 900);
    Bytes fourteen(14, 0x05);
    notify(merach, 0, fourteen, 900);
    assert(near(merach.sample().resistance.value, before.resistance.value));

    assert(merach.capabilities().supports(Control::Resistance));
    const int level = percentToLevel(50.0F, 1, 24);
    assert(merach.control({Control::Resistance, 50.0F}, 1000));
    writes = drain(merach);
    assert(writes.size() == 1 && (writes[0] == fitshow::fitplusResistanceFrame(FitPlusModel::Merach, level)));
    notify(merach, 0, longFrame(static_cast<std::uint8_t>(level), 60, 900), 1100);
    assert(merach.control({Control::Resistance, 50.0F}, 1200) && merach.pendingWrites() == 0);
    assert(!merach.control({Control::Speed, 10.0F}, 1200));

    FitPlusSource etappe(FitPlusModel::Etappe);
    etappe.reset();
    assert(etappe.onConnected(1, 0));
    assert(etappe.maxResistance() == 24);
    notify(etappe, 0, Bytes{0x02, 0x41, 0x02, 0x20, 0x00, 0x02, 0x00, 0x61, 0x03}, 10);
    assert(etappe.maxResistance() == 32 && !etappe.hasData());
    drain(etappe);
    assert(etappe.control({Control::Resistance, 100.0F}, 20));
    writes = drain(etappe);
    assert(writes.size() == 1 && (writes[0] == Bytes{0x02, 0x44, 0x05, 0x20, 0x00, 0x61, 0x03}));

    FitPlusSource generic(FitPlusModel::Generic);
    generic.reset();
    assert(generic.onConnected(1, 0));
    assert(!generic.capabilities().supports(Control::Resistance));
    assert(!generic.control({Control::Resistance, 50.0F}, 1));
    Bytes shortFrame{0x02, 0x42, 0x01, 0x00, 0x00, 0x00, 0xfa, 0x00, 75, 0x00, 0x00, 0x00, 0x00, 0x03};
    notify(generic, 0, shortFrame, 5);
    assert(near(generic.sample().speed.value, 25.0F) && near(generic.sample().cadence.value, 75.0F));
    assert(!generic.sample().resistance.valid && !generic.sample().power.valid);
    FitPlusSource generic2(FitPlusModel::Generic);
    generic2.reset();
    assert(generic2.onConnected(1, 0));
    notify(generic2, 0, longFrame(5, 70, 1000), 5);
    assert(!generic2.hasData() && !generic2.lastError().empty());

    FitPlusSource sx600(FitPlusModel::SX600);
    assert(sx600.plan().subscriptionCount == 2);
    sx600.reset();
    assert(sx600.onConnected(3, 0));
    notify(sx600, 0, longFrame(10, 80, 1234), 10);
    assert(!sx600.hasData());
    ftms::IndoorBikeData bike;
    bike.instantaneousSpeed = 2550;
    bike.instantaneousCadence = 170;
    bike.resistanceLevel = 120;
    bike.instantaneousPower = 150;
    const Bytes ftmsFrame = ftms::encodeIndoorBikeData(bike);
    notify(sx600, 1, ftmsFrame, 20);
    assert(near(sx600.sample().speed.value, 25.5F) && near(sx600.sample().cadence.value, 85.0F));
    assert(near(sx600.sample().resistance.value, 12.0F) && near(sx600.sample().power.value, 150.0F));
    notify(sx600, 1, Bytes{0x44}, 30);  // truncated FTMS frame
    FitPlusSource sx600Fff(FitPlusModel::SX600);
    sx600Fff.reset();
    assert(sx600Fff.onConnected(1, 0));
    notify(sx600Fff, 0, longFrame(10, 80, 1234), 10);
    assert(sx600Fff.hasData() && near(sx600Fff.sample().power.value, 123.4F));

    FitPlusSource rower(FitPlusModel::Rower);
    assert(rower.profile() == MachineProfile::Rower && std::string(rower.id()) == "fitplus-rower");
    rower.reset();
    assert(rower.onConnected(1, 0));
    notify(rower, 0, longFrame(4, 24, 1500), 10);
    assert(near(rower.sample().cadence.value, 24.0F) && near(rower.sample().power.value, 150.0F));
}

void testMatchers() {
    using namespace fitshow;
    assert(matchesTreadmill(ad("FS-A1B2C3")) && matchesTreadmill(ad("FS-A1B2C3", true)));
    assert(!matchesTreadmill(ad("fs-a1b2c3")) && !matchesTreadmill(ad("FS-YK-123")));
    assert(matchesTreadmill(ad("TR510-T 01")) && matchesTreadmill(ad("Tunturi T80-12", true)));
    assert(matchesTreadmill(ad("SW1234567890AB")) && !matchesTreadmill(ad("SW1234567890AB", true)));
    assert(!matchesTreadmill(ad("SW12(4567890AB")) && !matchesTreadmill(ad("SW123456789")));
    assert(matchesTreadmill(ad("SW5731CAXI-0061")) && !matchesTreadmill(ad("SW5731CAX-I0061")));
    assert(!matchesTreadmill(ad("SWALK LITE-123")));
    assert(matchesTreadmill(ad("Winfita 3")) && !matchesTreadmill(ad("WINFITA 3", true)));
    assert(matchesTreadmill(ad("SW-BLE 1")) && matchesTreadmill(ad("BF70 X")) && !matchesTreadmill(ad("bf70")));
    assert(!matchesTreadmill(ad("NOBLEPRO CONNECT")) && !matchesTreadmill(ad("")));
    assert(matchesNoblePro(ad("NoblePro Connect 9")) && !matchesNoblePro(ad("NOBLEPRO CONNECT", true)));
    assert(matchesSnode(ad("TF-769DF2")) && matchesSnode(ad("tf-1", true)) && !matchesSnode(ad("TF-T100")));
    assert(matchesEtappe(ad("X100-1234")) && !matchesEtappe(ad("X100-1234", true)) && !matchesEtappe(ad("x100-1")));
    assert(matchesMerach(ad("MRK-B12-77")) && !matchesMerach(ad("MRK-B12-77", true)) && !matchesMerach(ad("mrk-b1")));
    for (const char* other : {"MRK-R28-1", "MRK-S26S-1", "MRK-S26C-1", "MRK-S28-1", "MRK-S38-1", "MRK-T1",
                              "MRK-E1", "MRK-R11S-1", "MRK-CRYDN-1", "MRK-R06-1", "MRK-R15-1"})
        assert(!matchesMerach(ad(other)));
    assert(matchesH9110(ad("H9110 Osaka 1")) && !matchesH9110(ad("H9110 OSAKA", true)));
    assert(matchesFitPlusRower(ad("MRK-R28-001")) && matchesFitPlusRower(ad("mrk-r28-1", true)));
}

void testDescriptors() {
    std::set<std::string> ids;
    const std::vector<ble::Advertisement> samples{ad("FS-1"), ad("TF-1"), ad("X100-1"), ad("MRK-1"),
                                                  ad("H9110 OSAKA"), ad("MRK-R28-1"), ad("NOBLEPRO CONNECT")};
    for (std::size_t i = 0; i < fitshow::descriptorCount(); ++i) {
        const auto& info = fitshow::descriptorAt(i);
        const std::string id = info.id;
        assert(!id.empty() && id.size() <= 16 && ids.insert(id).second);
        for (char c : id) assert((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
        auto source = info.create();
        assert(source && id == source->id() && source->profile() == info.profile);
        assert(source->advertisementOnly() == info.advertisementOnly);
        assert(info.priority == 0 || info.priority == fitshow::kMatchPriority);
        if (info.priority == 0)
            for (const auto& sample : samples) assert(!info.matches(sample));
    }
    assert(fitshow::descriptorCount() == 11);
    assert(std::string(fitshow::descriptorAt(0).id) == "fitshow-tm");
    assert(std::string(fitshow::descriptorAt(4).id) == "snode-bike");
    auto snode = createSnodeBike();
    assert(snode->plan().subscriptionCount == 1 && !snode->isTrainer());
}

}  // namespace

void runFitshowTests() {
    testFraming();
    testTreadmill();
    testTreadmillVariants();
    testFitPlusFrames();
    testFitPlusSources();
    testMatchers();
    testDescriptors();
    std::printf("fitshow host tests passed (%u protocols)\n", static_cast<unsigned>(fitshow::descriptorCount()));
}

// Needs fitshow::descriptorAt() registered in protocols/registry.cpp.
void runFitshowRegistryTests() {
    const struct {
        const char* name;
        bool ftms;
        const char* id;
    } cases[] = {
        {"FS-A1B2C3", false, "fitshow-tm"},       {"FS-A1B2C3", true, "fitshow-tm"},
        {"SW5731CAXI-0061", false, "fitshow-tm"}, {"TUNTURI T80-1", true, "fitshow-tm"},
        {"NOBLEPRO CONNECT", false, "fitshow-noblepro"}, {"TF-769DF2", false, "snode-bike"},
        {"X100-1234", false, "fitplus-etappe"},   {"MRK-B12-1", false, "fitplus-merach"},
        {"H9110 OSAKA", false, "fitplus-h9110"},  {"MRK-R28-1", true, "fitplus-rower"},
    };
    for (const auto& c : cases) {
        const ProtocolInfo* info = detectProtocol(ad(c.name, c.ftms));
        assert(info && std::string(info->id) == c.id);
    }
    for (const char* manual : {"fitshow-tm-mi", "fitshow-anyrun", "fitplus-bike", "fitplus-sx600"}) {
        const ProtocolInfo* info = findProtocol(manual);
        assert(info && info->priority == 0);
        assert(createProtocol(manual));
    }
    std::printf("fitshow registry tests passed\n");
}
