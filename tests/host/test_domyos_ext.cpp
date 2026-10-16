// Host tests for the Domyos treadmill / elliptical / rower port.
//
// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/domyostreadmill/, src/devices/domyoselliptical/,
// src/devices/domyosrower/ (same-named .cpp/.h), src/devices/bluetooth.cpp.
// Upstream ships no captured traces: status frames below are synthetic
// frames in the upstream 26-byte layout.

#include "protocols/domyos_ext.hpp"
#include "protocols/registry.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace smartfitness;
using namespace smartfitness::protocols;
using ble::Bytes;
using domyos_ext::Kind;

bool near(float a, float b) { return std::fabs(a - b) < 0.01F; }

ble::Advertisement advert(const std::string& name, bool ftms = false) {
    ble::Advertisement advertisement;
    advertisement.name = name;
    if (ftms) advertisement.services.push_back(ble::Uuid::from16(0x1826));
    return advertisement;
}

std::vector<Bytes> drain(SourceProtocol& source) {
    std::vector<Bytes> out;
    ble::GattWrite write;
    while (source.takeWrite(write)) {
        assert(write.service == domyos::kService && write.characteristic == domyos::kWrite);
        out.push_back(write.payload());
    }
    return out;
}

void notify(SourceProtocol& source, const Bytes& frame, std::uint32_t now) {
    source.onNotify(0, frame.data(), frame.size(), now);
}

const Bytes kAck{0xf0, 0xac, 0x9c};  // any non-20-byte notification

// Synthetic treadmill status (domyostreadmill::characteristicChanged layout).
Bytes treadmillFrame(std::uint8_t speedTenths, std::uint16_t inclineRaw, std::uint8_t heart = 0,
                     std::uint8_t button = 0) {
    Bytes frame(26, 0);
    frame[0] = 0xf0;
    frame[1] = 0xbc;
    frame[2] = static_cast<std::uint8_t>(inclineRaw >> 8);
    frame[3] = static_cast<std::uint8_t>(inclineRaw);
    frame[7] = speedTenths;
    frame[10] = 0x00;
    frame[11] = 0x2a;  // kcal (upstream only logs it)
    frame[18] = heart;
    frame[22] = button;
    return frame;
}

// Synthetic elliptical/rower status (domyoselliptical / domyosrower layout).
Bytes cardioFrame(std::uint16_t word2, std::uint16_t word6, std::uint8_t cadence, std::uint16_t kcal,
                  std::uint8_t resistance, std::uint8_t heart, std::uint8_t incline) {
    Bytes frame(26, 0);
    frame[0] = 0xf0;
    frame[1] = 0xbc;
    frame[2] = static_cast<std::uint8_t>(word2 >> 8);
    frame[3] = static_cast<std::uint8_t>(word2);
    frame[6] = static_cast<std::uint8_t>(word6 >> 8);
    frame[7] = static_cast<std::uint8_t>(word6);
    frame[9] = cadence;
    frame[10] = static_cast<std::uint8_t>(kcal >> 8);
    frame[11] = static_cast<std::uint8_t>(kcal);
    frame[14] = resistance;
    frame[18] = heart;
    frame[21] = incline;
    return frame;
}

// Walks the init handshake: every wait step is acknowledged by a notification.
void connect(DomyosExtSource& source, std::uint32_t& now) {
    source.reset();
    assert(source.onConnected(1, now));
    std::size_t guard = 0;
    while (!source.initialized()) {
        assert(++guard < 32);
        drain(source);
        now += 10;
        notify(source, kAck, now);
    }
    drain(source);
}

void codecTests() {
    // Upstream frames for the shared builders.
    assert(domyos_ext::checksum(Bytes({0xf0, 0xc8, 0x01}).data(), 3) == 0xb9);
    assert(domyos_ext::inclinationFrame(5) == Bytes({0xf0, 0xe3, 0x06, 0xd9}));
    assert(domyos_ext::ellipticalInclinationLevel(0.0F) == 1);
    assert(domyos_ext::ellipticalInclinationLevel(20.0F) == 15);
    assert(domyos_ext::ellipticalInclinationLevel(7.0F) == 7);

    // forceSpeedOrIncline(10.0, 2.5): speed 0x0064, incline 0x0019, sum byte 22.
    const auto frames = domyos_ext::speedInclineFrames(10.0F, 2.5F);
    assert(frames.size() == 2 && frames[0].size() == 20 && frames[1].size() == 3);
    Bytes full = frames[0];
    full.insert(full.end(), frames[1].begin(), frames[1].end());
    const Bytes expected{0xf0, 0xad, 0xff, 0xff, 0x00, 0x64, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                         0xff, 0x00, 0x19, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    Bytes withSum = expected;
    withSum[22] = domyos_ext::checksum(expected.data(), 22);
    assert(full == withSum);
    assert(withSum[22] == static_cast<std::uint8_t>(0xf0 + 0xad + 0x64 + 0x19 + 0xff * 16));

    // Init sequences: no start-tape frames, never the stop command.
    const auto treadmill = domyos_ext::initSteps(Kind::Treadmill);
    assert(treadmill.size() == 6);
    assert(treadmill[0].bytes == Bytes({0xf0, 0xc9, 0xb9}) && treadmill[0].wait);
    assert(treadmill[5].bytes == Bytes({0xf0, 0xc4, 0x03, 0xb7}));
    for (const auto& step : treadmill) assert(step.bytes != Bytes({0xf0, 0xc8, 0x01, 0xb9}));
    const auto elliptical = domyos_ext::initSteps(Kind::Elliptical);
    assert(elliptical.size() == 12);
    assert(elliptical[8].bytes == Bytes({0xff, 0xff, 0x8b}) && elliptical[8].wait && !elliptical[7].wait);
    assert(elliptical[11].bytes.size() == 20 && !elliptical[11].wait && elliptical[11].bytes[5] == 0x05);
    for (Kind kind : {Kind::Treadmill, Kind::Elliptical, Kind::Rower}) {
        for (const auto& step : domyos_ext::initSteps(kind)) {
            // "start tape" frames initDataStart11 / initDataStart13.
            assert(step.bytes != Bytes({0xff, 0xff, 0x94}));
            assert(step.bytes != Bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xbd}));
            assert(step.bytes != Bytes({0xf0, 0xc8, 0x00, 0xb8}));
            assert(!(step.bytes.size() > 1 && step.bytes[0] == 0xf0 && step.bytes[1] == 0xcb && step.bytes[2] == 0xff));
        }
    }
    for (std::size_t i = 0; i < treadmill.size(); ++i) {
        const auto& bytes = treadmill[i].bytes;
        assert(domyos_ext::checksum(bytes.data(), bytes.size() - 1) == bytes.back());
    }

    // Treadmill decode: speed byte 7, incline (raw - 1000) / 10, HR byte 18.
    domyos_ext::TreadmillStatus tm;
    assert(domyos_ext::decodeTreadmill(treadmillFrame(85, 1000 + 45, 120), tm));
    assert(near(tm.speed, 8.5F) && near(tm.incline, 4.5F) && tm.heartRate == 120);
    assert(domyos_ext::decodeTreadmill(treadmillFrame(0, 900), tm) && near(tm.incline, 0.0F));
    assert(domyos_ext::decodeTreadmill(treadmillFrame(0, 65522), tm) && near(tm.incline, 1.0F));
    Bytes truncated = treadmillFrame(85, 1000);
    truncated.pop_back();
    assert(!domyos_ext::decodeTreadmill(truncated, tm));

    domyos_ext::EllipticalStatus el;
    assert(domyos_ext::decodeElliptical(cardioFrame(0, 123, 60, 0x0102, 7, 99, 4), el));
    assert(near(el.speed, 12.3F) && near(el.cadence, 60) && near(el.calories, 258) && el.resistance == 7 &&
           el.heartRate == 99 && near(el.incline, 4));
    assert(domyos_ext::decodeElliptical(cardioFrame(0, 0, 0, 0, 0, 0, 16), el));
    assert(el.resistance == 1 && near(el.incline, 0));
    assert(domyos_ext::decodeElliptical(cardioFrame(0, 0, 0, 0, 0x90, 0, 0xff), el));
    assert(el.resistance == 1 && near(el.incline, 0));
    assert(!domyos_ext::decodeElliptical(Bytes(20, 0xf0), el));

    domyos_ext::RowerStatus row;
    assert(domyos_ext::decodeRower(cardioFrame(345, 120, 24, 50, 3, 0, 0), row));
    assert(near(row.strokes, 345) && near(row.pace, 120) && near(row.speed, 15.0F) && near(row.cadence, 24) &&
           row.resistance == 3 && near(row.calories, 50));
    assert(domyos_ext::decodeRower(cardioFrame(1, 120, 0, 0, 3, 0, 0), row) && near(row.speed, 0) && near(row.pace, 0));
    assert(domyos_ext::decodeRower(cardioFrame(1, 65100, 20, 0, 3, 0, 0), row) && near(row.speed, 0));
    assert(!domyos_ext::decodeRower(Bytes(27, 0), row));
}

void matcherTests() {
    assert(domyos_ext::matchesTreadmill(advert("Domyos-TC-1234")));
    assert(domyos_ext::matchesTreadmill(advert("Domyos")));
    assert(!domyos_ext::matchesTreadmill(advert("Domyos-TC-1234", true)));
    assert(!domyos_ext::matchesTreadmill(advert("DOMYOS-TC-1234")));  // upstream is case-sensitive
    assert(domyos_ext::matchesElliptical(advert("Domyos-EL-5678")));
    assert(!domyos_ext::matchesElliptical(advert("Domyos-EL-5678", true)));
    assert(!domyos_ext::matchesElliptical(advert("DOMYOS-EL-5678")));
    assert(domyos_ext::matchesRower(advert("Domyos-Row-0001")));
    assert(domyos_ext::matchesRower(advert("DOMYOS-ROW-0001")));
    assert(!domyos_ext::matchesRower(advert("Domyos-Row-0001", true)));
    // Never steal the bike entry, bridges or each other's names.
    for (const char* name : {"Domyos-Bike", "Domyos-Bike-1234", "DOMYOS-BIKE-1", "Domyos-Biking-2", "DomyosBridge",
                             "DomyosBr", "Lamp", ""}) {
        assert(!domyos_ext::matchesTreadmill(advert(name)));
        assert(!domyos_ext::matchesElliptical(advert(name)));
        assert(!domyos_ext::matchesRower(advert(name)));
    }
    assert(!domyos_ext::matchesTreadmill(advert("Domyos-EL-5678")));
    assert(!domyos_ext::matchesTreadmill(advert("Domyos-Row-0001")));
    assert(!domyos_ext::matchesElliptical(advert("Domyos-TC-1")));
    assert(!domyos_ext::matchesRower(advert("Domyos-TC-1")));
}

void descriptorTests() {
    assert(domyos_ext::descriptorCount() == 3);
    const MachineProfile profiles[] = {MachineProfile::Treadmill, MachineProfile::CrossTrainer, MachineProfile::Rower};
    for (std::size_t i = 0; i < domyos_ext::descriptorCount(); ++i) {
        const auto& info = domyos_ext::descriptorAt(i);
        const std::size_t length = std::strlen(info.id);
        assert(length > 0 && length <= 16);
        for (std::size_t c = 0; c < length; ++c) {
            const char ch = info.id[c];
            assert((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_');
        }
        for (std::size_t j = 0; j < i; ++j) assert(std::strcmp(info.id, domyos_ext::descriptorAt(j).id) != 0);
        assert(info.priority == domyos_ext::kMatchPriority && info.controllable && !info.advertisementOnly);
        assert(info.profile == profiles[i]);
        auto source = info.create();
        assert(source && std::strcmp(source->id(), info.id) == 0 && source->profile() == info.profile);
        assert(source->plan().subscriptionCount == 1);
        assert(!source->isTrainer());
    }
    assert(std::strcmp(domyos_ext::descriptorAt(0).id, "domyos-tm") == 0);
    assert(std::strcmp(domyos_ext::descriptorAt(1).id, "domyos-el") == 0);
    assert(std::strcmp(domyos_ext::descriptorAt(2).id, "domyos-row") == 0);
}

void handshakeTests() {
    DomyosExtSource source(Kind::Elliptical);
    assert(!source.onConnected(0, 0));
    source.reset();
    std::uint32_t now = 1000;
    assert(source.onConnected(1, now));
    // One wait step at a time.
    auto writes = drain(source);
    assert(writes.size() == 1 && writes[0] == Bytes({0xf0, 0xc8, 0x01, 0xb9}));
    source.tick(now + 100);
    assert(drain(source).empty());
    // A 20-byte notification is not an acknowledgement.
    notify(source, Bytes(20, 0xf0), now + 150);
    assert(drain(source).empty());
    // Timeout advances like upstream's 300 ms write timer.
    source.tick(now + 300);
    writes = drain(source);
    assert(writes.size() == 1 && writes[0] == Bytes({0xf0, 0xc9, 0xb9}));
    // Remaining steps acknowledged by notifications; grouped non-wait frames.
    std::vector<Bytes> all;
    for (int i = 0; i < 20 && !source.initialized(); ++i) {
        notify(source, kAck, now + 400 + i);
        for (auto& w : drain(source)) all.push_back(w);
    }
    assert(source.initialized());
    assert(all.size() == 10);
    assert(all.back().size() == 20 && all.back()[5] == 0x05);  // trailing head without "start tape" tail
    assert(source.capabilities().supports(Control::Resistance) && source.capabilities().supports(Control::Incline));

    // Poll: no-op every 300 ms, paced by the acknowledgement.
    now += 2000;
    source.tick(now);
    writes = drain(source);
    assert(writes.size() == 1 && writes[0] == domyos::noOpFrame());
    source.tick(now + 100);
    assert(drain(source).empty());

    // Treadmill init has 6 steps (no zero-speed frame) and stops before any start-tape frame.
    DomyosExtSource treadmill(Kind::Treadmill);
    std::uint32_t t = 0;
    assert(treadmill.onConnected(1, t));
    std::size_t count = drain(treadmill).size();
    while (!treadmill.initialized()) {
        notify(treadmill, kAck, ++t);
        count += drain(treadmill).size();
    }
    assert(count == 6);
}

void treadmillTests() {
    DomyosExtSource source(Kind::Treadmill);
    std::uint32_t now = 0;
    connect(source, now);
    assert(!source.capabilities().supports(Control::Speed));  // no data yet
    // Stopped belt: telemetry yes, controls rejected (never start the belt).
    notify(source, treadmillFrame(0, 1000), now);
    assert(source.hasData() && near(source.sample().speed.value, 0));
    assert(!source.sample().heartRate.valid);
    assert(source.capabilities().supports(Control::Speed) && source.capabilities().supports(Control::Incline));
    assert(!source.control({Control::Speed, 5.0F}, now));
    assert(!source.control({Control::Incline, 2.0F}, now));
    assert(drain(source).empty());

    // Console start/stop buttons never trigger writes.
    notify(source, treadmillFrame(0, 1000, 0, 0x06), now);
    notify(source, treadmillFrame(0, 1000, 0, 0x07), now);
    assert(drain(source).empty());

    // Running belt: speed request sends forceSpeedOrIncline(speed, current incline).
    notify(source, treadmillFrame(60, 1020, 130), ++now);
    assert(near(source.sample().incline.value, 2.0F) && near(source.sample().heartRate.value, 130));
    assert(source.control({Control::Speed, 10.0F}, now));
    auto writes = drain(source);
    const auto expected = domyos_ext::speedInclineFrames(10.0F, 2.0F);
    assert(writes.size() == 2 && writes[0] == expected[0] && writes[1] == expected[1]);
    // The incline request keeps the pending speed target (belt still ramping).
    notify(source, kAck, ++now);
    notify(source, treadmillFrame(70, 1020), ++now);
    assert(source.control({Control::Incline, 3.3F}, now));
    writes = drain(source);
    const auto expected2 = domyos_ext::speedInclineFrames(10.0F, 3.5F);
    assert(writes.size() == 2 && writes[0] == expected2[0] && writes[1] == expected2[1]);
    notify(source, kAck, ++now);

    // Limits and invalid values.
    assert(!source.control({Control::Speed, 0.0F}, now));
    assert(!source.control({Control::Speed, 22.5F}, now));
    assert(!source.control({Control::Incline, 20.5F}, now));
    assert(!source.control({Control::Resistance, 50.0F}, now));
    assert(!source.control({Control::Speed, std::nanf("")}, now));
    assert(drain(source).empty());
    // Negative grade clamps to 0 like upstream.
    assert(source.control({Control::Incline, -3.0F}, now));
    writes = drain(source);
    assert(writes.size() == 2 && writes[0] == domyos_ext::speedInclineFrames(10.0F, 0.0F)[0]);
    notify(source, kAck, ++now);

    // Stale data: rejected and nothing sent automatically.
    now += 5000;
    assert(!source.control({Control::Speed, 8.0F}, now));
    assert(drain(source).empty());

    // Split status frame (20 + 6) is reassembled; polls pause meanwhile.
    const Bytes split = treadmillFrame(50, 1000);
    notify(source, Bytes(split.begin(), split.begin() + 20), now);
    source.tick(now + 1000);
    assert(drain(source).empty());
    notify(source, Bytes(split.begin() + 20, split.end()), now + 1001);
    assert(near(source.sample().speed.value, 5.0F) && source.lastDataMs() == now + 1001);
    source.tick(now + 1400);
    writes = drain(source);
    assert(writes.size() == 1 && writes[0] == domyos::noOpFrame());

    // Disconnect: reset sends nothing.
    source.reset();
    source.tick(now + 5000);
    assert(drain(source).empty() && !source.isTrainer());

    // Malformed input ignored.
    DomyosExtSource fresh(Kind::Treadmill);
    std::uint32_t t = 0;
    connect(fresh, t);
    notify(fresh, Bytes(25, 0xf0), t);
    notify(fresh, Bytes(27, 0xf0), t);
    fresh.onNotify(0, nullptr, 26, t);
    assert(!fresh.hasData());
}

void ellipticalTests() {
    DomyosExtSource source(Kind::Elliptical);
    std::uint32_t now = 0;
    connect(source, now);
    notify(source, cardioFrame(0, 85, 55, 30, 6, 0, 3), ++now);
    const auto& sample = source.sample();
    assert(near(sample.speed.value, 8.5F) && near(sample.cadence.value, 55) && near(sample.resistance.value, 6) &&
           near(sample.incline.value, 3) && near(sample.totalEnergy.value, 30) && !sample.heartRate.valid);
    assert(!sample.power.valid && !sample.distance.valid);
    assert(source.capabilities().supports(Metric::Cadence) && !source.capabilities().supports(Control::Speed));

    // Resistance 100 % -> level 15, upstream forceResistance bytes.
    assert(source.control({Control::Resistance, 100.0F}, now));
    auto writes = drain(source);
    const auto expected = domyos::resistanceFrames(15);
    assert(writes.size() == 2 && writes[0] == expected[0] && writes[1] == expected[1]);
    assert(writes[0][10] == 15);
    assert(source.control({Control::Resistance, 0.0F}, now));
    writes = drain(source);
    assert(writes.size() == 2 && writes[0][10] == 1);

    assert(source.control({Control::Incline, 4.0F}, now));
    writes = drain(source);
    assert(writes.size() == 1 && writes[0] == Bytes({0xf0, 0xe3, 0x05, 0xd8}));
    assert(source.control({Control::Incline, 0.0F}, now));
    writes = drain(source);
    assert(writes.size() == 1 && writes[0] == domyos_ext::inclinationFrame(1));
    assert(!source.control({Control::Incline, -1.0F}, now));
    assert(!source.control({Control::Speed, 5.0F}, now));
    // Split frames are not reassembled by the elliptical upstream.
    const Bytes frame = cardioFrame(0, 99, 1, 1, 1, 0, 0);
    notify(source, Bytes(frame.begin(), frame.begin() + 20), now + 5);
    notify(source, Bytes(frame.begin() + 20, frame.end()), now + 6);
    assert(source.lastDataMs() == now);
}

void rowerTests() {
    DomyosExtSource source(Kind::Rower);
    std::uint32_t now = 0;
    connect(source, now);
    notify(source, cardioFrame(12, 150, 22, 40, 4, 101, 9), ++now);
    const auto& sample = source.sample();
    assert(near(sample.strokeCount.value, 12) && near(sample.pace.value, 150) && near(sample.speed.value, 12.0F) &&
           near(sample.cadence.value, 22) && near(sample.resistance.value, 4) && near(sample.totalEnergy.value, 40) &&
           near(sample.heartRate.value, 101));
    assert(!sample.incline.valid && !sample.power.valid);
    assert(source.capabilities().supports(Control::Resistance));
    assert(!source.capabilities().supports(Control::Incline));
    assert(source.control({Control::Resistance, 50.0F}, now));
    const auto writes = drain(source);
    const auto expected = domyos::resistanceFrames(8);
    assert(writes.size() == 2 && writes[0] == expected[0] && writes[1] == expected[1]);
    assert(!source.control({Control::Incline, 5.0F}, now));
}

}  // namespace

void runDomyosExtTests() {
    codecTests();
    matcherTests();
    descriptorTests();
    handshakeTests();
    treadmillTests();
    ellipticalTests();
    rowerTests();
    std::printf("domyos_ext host tests passed (%u protocols)\n", static_cast<unsigned>(domyos_ext::descriptorCount()));
}

// Needs registry integration (domyos_ext descriptors appended to kFamilies).
void runDomyosExtRegistryTests() {
    auto detected = [](const ble::Advertisement& advertisement) {
        const auto* info = detectProtocol(advertisement);
        return std::string(info ? info->id : "");
    };
    assert(detected(advert("Domyos-TC-1234")) == "domyos-tm");
    assert(detected(advert("Domyos-EL-5678")) == "domyos-el");
    assert(detected(advert("Domyos-Row-0001")) == "domyos-row");
    assert(detected(advert("DOMYOS-ROW-0001")) == "domyos-row");
    assert(detected(advert("Domyos-Bike-1234")) == "domyos");
    assert(detected(advert("Domyos-TC-1234", true)) == "ftms");
    assert(detected(advert("Domyos-EL-5678", true)) == "ftms");
    assert(detected(advert("DomyosBridge")).empty());
    for (const char* id : {"domyos-tm", "domyos-el", "domyos-row"}) {
        const auto* info = findProtocol(id);
        assert(info && std::strcmp(info->id, id) == 0);
    }
    std::printf("domyos_ext registry tests passed\n");
}
