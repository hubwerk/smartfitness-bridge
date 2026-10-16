// Host tests for the Sportstech (EW-BK/EP/ST), SportsPlus/Carefitness and
// Pafers protocols. GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and
// contributors (revision ea70d7ff64bc8bf508fc6401fd056211e05311b3). All
// notification frames below are synthetic, built from the upstream byte layout.

#include "protocols/sportsplus.hpp"
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
using namespace smartfitness::protocols::sportsplus;
using ble::Bytes;

bool near(float a, float b, float eps = 0.01F) { return std::fabs(a - b) < eps; }

ble::Advertisement ad(const char* name, bool ftms = false) {
    ble::Advertisement out;
    out.name = name;
    if (ftms) out.services.push_back(ble::Uuid::from16(0x1826));
    return out;
}

void notify(SourceProtocol& p, const Bytes& frame, std::uint32_t now) { p.onNotify(0, frame.data(), frame.size(), now); }

std::vector<ble::GattWrite> drain(SourceProtocol& p) {
    std::vector<ble::GattWrite> out;
    ble::GattWrite write;
    while (p.takeWrite(write)) out.push_back(write);
    return out;
}

// Runs the paced init: each write is answered by `reply` (when non-empty) or
// released by the timeout. Returns every payload written.
std::vector<Bytes> runInit(SportsplusSource& p, std::uint32_t& now, const Bytes& reply) {
    std::vector<Bytes> frames;
    for (int i = 0; i < 100 && !p.initialized(); ++i) {
        for (const auto& w : drain(p)) frames.push_back(w.payload());
        if (!reply.empty()) notify(p, reply, now);
        now += 400;
        p.tick(now);
    }
    for (const auto& w : drain(p)) frames.push_back(w.payload());
    return frames;
}

Bytes sportstechFrame(std::uint16_t speedTenths, std::uint16_t kcal, std::uint16_t watt, std::uint8_t heart,
                      std::uint8_t resistance, std::uint8_t cadence) {
    Bytes b(20, 0);
    b[0] = 0xf2;
    b[7] = static_cast<std::uint8_t>(kcal >> 8); b[8] = static_cast<std::uint8_t>(kcal);
    b[9] = static_cast<std::uint8_t>(watt >> 8); b[10] = static_cast<std::uint8_t>(watt);
    b[11] = heart;
    b[12] = static_cast<std::uint8_t>(speedTenths >> 8); b[13] = static_cast<std::uint8_t>(speedTenths);
    b[15] = resistance;
    b[17] = cadence;
    return b;
}

Bytes sp12(std::uint8_t type, std::uint8_t b2, std::uint8_t b3, std::uint8_t heart = 0, std::uint8_t b6 = 0,
           std::uint8_t b7 = 0, std::uint8_t b9 = 0, std::uint8_t b10 = 0) {
    Bytes b(12, 0);
    b[0] = 0x40; b[1] = type; b[2] = b2; b[3] = b3; b[6] = b6; b[7] = b7; b[8] = heart; b[9] = b9; b[10] = b10;
    return b;
}

void framesTests() {
    const auto init = sportstechInitFrames();
    assert(init.size() == 4);
    assert(init[0] == Bytes({0xf2, 0xc0, 0x00, 0xb2}));
    assert(init[3] == Bytes({0xf2, 0xc3, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xbb}));
    assert(init[2].size() == 17 && init[2].back() == 0xc0);
    assert(sportstechPollFrame(0) == Bytes({0xf2, 0xc3, 0x07, 0x04, 0, 0, 0, 0, 0xff, 0xff, 0xbe}));
    assert(sportstechPollFrame(10) == Bytes({0xf2, 0xc3, 0x07, 0x04, 10, 0, 0, 0, 0xff, 0xff, 0xc8}));
    assert(sportstechPollFrame(99)[4] == 23 && sportstechPollFrame(-5)[4] == 0);

    assert(sportsplusPollFrame(9) == Bytes({0x20, 0x01, 0x09, 0x00, 0x2a}));
    assert(sportsplusPollFrame(1) == Bytes({0x20, 0x01, 0x01, 0x00, 0x22}));
    assert(sportsplusPollFrame(40)[2] == 24 && sportsplusPollFrame(40)[4] == 0x39);
    assert(sportsplusInitFrames(Model::SportsplusBike) == std::vector<Bytes>({{0x40, 0x00, 0x16, 0x0a, 0x60}}));
    assert(sportsplusInitFrames(Model::SportsplusHt).size() == 1);
    const auto care = sportsplusInitFrames(Model::CarefitnessBike);
    assert(care.size() == 6 && care[5] == Bytes({0x40, 0x00, 0x9a, 0x56, 0x30}));
    assert(sportsplusInitFrames(Model::SportsplusHt9600ie).size() == 6);
    assert(sportsplusInitFrames(Model::SportsplusRower).size() == 6);
    assert(sportsplusInitFrames(Model::SportsplusRower, true) == std::vector<Bytes>({{0x40, 0x00, 0xd1, 0x5f, 0x70}}));

    const auto pb = pafersInitFrames(Model::PafersBike);
    assert(pb.size() == 4 && pb[2] == Bytes({0x55, 0x25, 0x01, 0xff}) && pb[3] == Bytes({0x55, 0x0a, 0x01, 0x01}));
    const auto pt = pafersInitFrames(Model::PafersTreadmill);
    assert(pt.size() == 9);
    assert(pt[3] == Bytes({0x55, 0x0a, 0x01, 0x02}));
    assert(pt[4] == Bytes({0x55, 0x01, 0x06, 0x39, 0x01, 0x32, 0x19, 0xa5, 0x32}));
    assert(pt[6] == Bytes({0x55, 0x0f, 0x02, 0x01, 0x00}));
    assert(pt[8] == Bytes({0x55, 0x08, 0x01, 0x01}));
    const auto pi = pafersInitFrames(Model::PafersIboxster);
    assert(pi.size() == 10 && pi[3] == pafersPollFrame() && pi[7] == Bytes({0x55, 0x0f, 0x02, 0x00, 0x0a}));
    assert(pafersPollFrame().size() == 13 && pafersPollFrame()[2] == 0x0a && pafersPollFrame()[12] == 0xff);
    assert(pafersResistanceFrame(7) == Bytes({0x55, 0x11, 0x01, 0x07}));
    assert(pafersSpeedFrame(8) == Bytes({0x55, 0x0f, 0x02, 0x08, 0x00}));
    assert(pafersInclineFrame(6) == Bytes({0x55, 0x11, 0x01, 0x06}));
    assert(pafersStopFrame() == Bytes({0x55, 0x0a, 0x01, 0x02}));
}

void decodeTests() {
    MetricSet m;
    // Sportstech 20-byte layout.
    auto f = sportstechFrame(253, 42, 150, 120, 8, 75);
    assert(decodeSportstech(f.data(), f.size(), m));
    assert(near(m.speed.value, 25.3F) && near(m.totalEnergy.value, 42) && near(m.power.value, 150));
    assert(near(m.heartRate.value, 120) && near(m.resistance.value, 8) && near(m.cadence.value, 75));
    f = sportstechFrame(0, 0, 150, 0, 0, 0);
    assert(decodeSportstech(f.data(), f.size(), m) && m.power.valid && m.power.value == 0 && !m.heartRate.valid);
    assert(!decodeSportstech(f.data(), 19, m) && !decodeSportstech(nullptr, 20, m));

    // SportsPlus default layout.
    auto s = sp12(0x20, 0x07, 0xd0, 99, 0x01, 0x02);  // 2000 / 100 = 20 km/h
    assert(decodeSportsplusBike(Model::SportsplusBike, s.data(), s.size(), 1, m));
    assert(near(m.speed.value, 20) && near(m.cadence.value, 212) && near(m.totalEnergy.value, 258) &&
           near(m.heartRate.value, 99) && !m.power.valid);
    s = sp12(0x30, 0x00, 0x96);
    assert(decodeSportsplusBike(Model::SportsplusBike, s.data(), s.size(), 1, m) && near(m.power.value, 150));
    assert(!m.speed.valid && !m.heartRate.valid);
    assert(!decodeSportsplusBike(Model::SportsplusBike, s.data(), 11, 1, m));

    // HT variant: BCD speed 0x02 0x55 -> 25.5 km/h; raw cadence 0x0255/10 + 12.
    s = sp12(0x00, 0x02, 0x55, 80);
    assert(decodeSportsplusBike(Model::SportsplusHt, s.data(), s.size(), 1, m));
    assert(near(m.speed.value, 25.5F) && near(m.cadence.value, 597 / 10.0F + 12) && near(m.heartRate.value, 80));
    assert(!m.totalEnergy.valid);
    s = sp12(0x10, 0, 0, 0, 0, 0, 0x01, 0x23);
    assert(decodeSportsplusBike(Model::SportsplusHt, s.data(), s.size(), 1, m) && near(m.power.value, 123));

    // Carefitness: BCD cadence, upstream watt table and speed estimate.
    s = sp12(0x30, 0x00, 0x65);
    assert(decodeSportsplusBike(Model::CarefitnessBike, s.data(), s.size(), 1, m));
    assert(near(m.cadence.value, 65) && near(m.speed.value, 0.37497622F * 65));
    assert(near(m.power.value, carefitnessWatts(1, 65)));
    assert(near(carefitnessWatts(1, 65), 21));  // row 1, step 1: floor(19 + (24 - 19) / 10 * 5)
    s = sp12(0x20, 0x00, 0x65);
    assert(decodeSportsplusBike(Model::CarefitnessBike, s.data(), s.size(), 1, m) && !m.cadence.valid);
    assert(near(carefitnessWatts(0, 100), 39) && carefitnessWatts(5, 0) == 0);

    // HT-9600iE: speed b2*100 + BCD(b3), power b9*100 + BCD(b10).
    s = sp12(0x00, 0x01, 0x25, 0, 0x00, 0x10, 0x02, 0x34);
    assert(decodeSportsplusBike(Model::SportsplusHt9600ie, s.data(), s.size(), 1, m));
    assert(near(m.speed.value, 12.5F) && near(m.cadence.value, 12.5F * 2.685185F) && near(m.power.value, 234) &&
           near(m.totalEnergy.value, 16));

    // SportsPlus rower.
    s = sp12(0x10, 0x00, 0x28, 110);
    assert(decodeSportsplusRower(s.data(), s.size(), m));
    const float speed = 0.37497622F * 28;
    assert(near(m.cadence.value, 28) && near(m.speed.value, speed) && near(m.heartRate.value, 110));
    assert(near(m.power.value, rowerWattsFromSpeed(speed)) && near(rowerWattsFromSpeed(18), 350, 0.5F));
    s = sp12(0x20, 0x00, 0x28);
    assert(decodeSportsplusRower(s.data(), s.size(), m) && !m.cadence.valid && !m.power.valid);

    // Pafers bike/rower: speed b3, resistance b5, cadence b7, watt b8..9.
    Bytes p{0, 0, 0, 27, 0, 12, 0, 85, 0x01, 0x0e};
    assert(decodePafers(p.data(), p.size(), m));
    assert(near(m.speed.value, 27) && near(m.resistance.value, 12) && near(m.cadence.value, 85) &&
           near(m.power.value, 270));
    assert(!decodePafers(p.data(), 9, m));

    // Pafers treadmill: speed b9 + b10/100, incline b11; raw 1.00 = stopped.
    Bytes t(13, 0);
    t[9] = 6; t[10] = 50; t[11] = 3;
    assert(decodePafersTreadmill(t.data(), t.size(), m) && near(m.speed.value, 6.5F) && near(m.incline.value, 3));
    t[9] = 1; t[10] = 0;
    assert(decodePafersTreadmill(t.data(), t.size(), m) && m.speed.value == 0);
    assert(!decodePafersTreadmill(t.data(), 12, m));
}

void matcherTests() {
    assert(matchesSportstechBike(ad("EW-BK-1234")) && matchesSportstechBike(ad("ew-bk1")));
    assert(matchesSportstechBike(ad("AF7019E")) && !matchesSportstechBike(ad("AF7019")) &&
           !matchesSportstechBike(ad("AFG SPORT")));
    assert(!matchesSportstechBike(ad("EW-BK-1234", true)));
    assert(matchesSportstechElliptical(ad("EW-EP-55")) && !matchesSportstechElliptical(ad("EW-TM-55")));
    assert(matchesSportstechRower(ad("EW-ST-55")) && !matchesSportstechRower(ad("EW-ST-55", true)));
    assert(!matchesSportstechRower(ad("EW-TM-1")) && !matchesSportstechBike(ad("EW-TM-1")));

    assert(matchesSportsplusBike(ad("CARDIOFIT")) && matchesSportsplusBike(ad("CARDIOFIT 2")));
    assert(matchesSportsplusBike(ad("HT-ABC-123")) && !matchesSportsplusHt(ad("HT-ABC-123")));
    assert(matchesSportsplusHt(ad("HT12345678")) && matchesSportsplusHt(ad("ht12345678")));
    assert(!matchesSportsplusBike(ad("HT12345678")));
    assert(!matchesSportsplusBike(ad("HT1234567")) && !matchesSportsplusHt(ad("HT123456789")));
    assert(!matchesSportsplusHt(ad("HT12345678", true)) && !matchesSportsplusBike(ad("CARDIOFIT", true)));
    assert(matchesCarefitnessBike(ad("CARE9040177")) && !matchesSportsplusBike(ad("CARE9040177")));
    assert(!matchesCarefitnessBike(ad("CARE904017")));
    assert(matchesSportsplusRower(ad("CARE968300122")) && matchesSportsplusRower(ad("CARE10692135")));
    assert(matchesSportsplusRower(ad("VMAX-R1")) && !matchesCarefitnessBike(ad("CARE968300122")));
    assert(!matchesSportsplusRower(ad("CARE968300122", true)));
    // Upstream chain order: EW-BK/AF names are the Sportstech bike first.
    assert(matchesSportstechBike(ad("AFCARE1")) && !matchesSportsplusRower(ad("EW-BKCARE123")));
    assert(matchesPafersBike(ad("PAFERS_1234")) && matchesPafersBike(ad("pafers_x")));
    assert(!matchesPafersBike(ad("PAFERS")) && !matchesPafersBike(ad("PAFERS_1", true)));
    assert(!matchesPafersBike(ad("Echelon")) && !matchesSportsplusBike(ad("")));

    std::size_t automatic = 0;
    for (std::size_t i = 0; i < descriptorCount(); ++i) {
        const auto& info = descriptorAt(i);
        assert(std::strlen(info.id) <= 16);
        for (const char* c = info.id; *c; ++c)
            assert((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_');
        for (std::size_t j = 0; j < i; ++j) assert(std::strcmp(info.id, descriptorAt(j).id) != 0);
        auto source = info.create();
        assert(source && std::strcmp(source->id(), info.id) == 0 && source->profile() == info.profile);
        assert(!source->advertisementOnly() && !info.advertisementOnly);
        if (info.priority == 0) {
            assert(!info.matches(ad("PAFERS_1")) && !info.matches(ad("HT12345678")));
        } else {
            assert(info.priority == kMatchPriority);
            ++automatic;
        }
    }
    assert(descriptorCount() == 12 && automatic == 8);
    // Exactly one automatic descriptor claims each representative name.
    for (const char* name : {"EW-BK-1", "AF7019E", "EW-EP-1", "EW-ST-1", "CARDIOFIT", "HT-ABC-123", "HT12345678",
                             "CARE9040177", "CARE968300122", "VMAX1", "PAFERS_1"}) {
        int hits = 0;
        for (std::size_t i = 0; i < descriptorCount(); ++i) hits += descriptorAt(i).matches(ad(name)) ? 1 : 0;
        assert(hits == 1);
    }
}

void sportstechSourceTests() {
    SportsplusSource bike(Model::SportstechBike);
    assert(std::string(bike.id()) == "sportstech-bike" && bike.profile() == MachineProfile::IndoorBike);
    const auto plan = bike.plan();
    assert(plan.subscriptionCount == 1 && plan.subscriptions[0].service == kFff0 &&
           plan.subscriptions[0].characteristic == kFff1 && !plan.discoverNotifications);
    assert(!bike.onConnected(0, 0));
    assert(!bike.control({Control::Resistance, 50}, 0));
    bike.reset();
    std::uint32_t now = 1000;
    assert(bike.onConnected(1, now));
    // Paced: only the first init frame is written until a reply or timeout.
    auto writes = drain(bike);
    assert(writes.size() == 1 && writes[0].payload() == sportstechInitFrames()[0]);
    assert(writes[0].service == kFff0 && writes[0].characteristic == kFff2 && writes[0].withResponse);
    bike.tick(now + 100);
    assert(drain(bike).empty());
    bike.tick(now + 300);
    assert(drain(bike).size() == 1);
    now += 300;
    const auto rest = runInit(bike, now, sportstechFrame(0, 0, 0, 0, 0, 0));
    assert(rest.size() >= 2 && rest[0] == sportstechInitFrames()[2] && rest[1] == sportstechInitFrames()[3]);
    assert(bike.initialized() && bike.capabilities().supports(Control::Resistance));
    assert(!bike.capabilities().supports(Control::Power) && !bike.capabilities().supports(Control::Speed));
    assert(rest.back() == sportstechPollFrame(0));

    assert(bike.control({Control::Resistance, 100}, now));
    assert(!bike.control({Control::Power, 100}, now));
    notify(bike, sportstechFrame(0, 0, 0, 0, 0, 0), now);  // response releases the pacer
    now += 200;
    bike.tick(now);
    writes = drain(bike);
    assert(writes.size() == 1 && writes[0].payload() == sportstechPollFrame(23));
    assert(bike.control({Control::Resistance, 50}, now) && bike.requestedLevel() == 12);

    notify(bike, sportstechFrame(253, 42, 150, 120, 8, 75), now);
    assert(bike.hasData() && near(bike.sample().speed.value, 25.3F) && near(bike.sample().power.value, 150));
    assert(near(bike.sample().heartRate.value, 120));
    notify(bike, sportstechFrame(253, 42, 150, 0, 8, 75), now + 10);
    assert(!bike.sample().heartRate.valid);
    const std::uint32_t last = bike.lastDataMs();
    notify(bike, Bytes(19, 0), now + 50);
    assert(bike.lastDataMs() == last);
    assert(bike.capabilities().supports(Metric::Cadence) && !bike.capabilities().supports(Metric::Distance));
    assert(!bike.capabilities().supports(Metric::ElapsedTime));

    SportsplusSource rower(Model::SportstechRower);
    assert(rower.profile() == MachineProfile::Rower);
    now = 0;
    assert(rower.onConnected(1, now));
    runInit(rower, now, {});
    assert(rower.initialized() && !rower.capabilities().supports(Control::Resistance));
    assert(!rower.control({Control::Resistance, 50}, now));
    now += 300;
    rower.tick(now);
    writes = drain(rower);
    assert(writes.size() == 1 && writes[0].payload() == sportstechPollFrame(0));

    SportsplusSource ellip(Model::SportstechElliptical);
    assert(ellip.profile() == MachineProfile::CrossTrainer && std::string(ellip.id()) == "sportstech-ellip");
    now = 0;
    assert(ellip.onConnected(1, now));
    runInit(ellip, now, {});
    assert(ellip.control({Control::Resistance, 0}, now) && ellip.requestedLevel() == 0);
}

void sportsplusSourceTests() {
    SportsplusSource bike(Model::SportsplusBike);
    const auto plan = bike.plan();
    assert(plan.subscriptionCount == 3 && plan.subscriptions[1].characteristic == kFff2);
    assert(!bike.onConnected(6, 0));
    bike.reset();
    std::uint32_t now = 0;
    assert(bike.onConnected(1, now));
    auto frames = runInit(bike, now, {});
    assert(frames.size() >= 2 && frames[0] == Bytes({0x40, 0x00, 0x16, 0x0a, 0x60}));
    assert(frames[1] == sportsplusPollFrame(1));  // upstream starts at level 1
    assert(bike.control({Control::Resistance, 100}, now) && bike.requestedLevel() == 24);
    now += 300;
    bike.tick(now);
    auto writes = drain(bike);
    assert(writes.size() == 1 && writes[0].characteristic == kFff1 && writes[0].payload() == sportsplusPollFrame(24));
    notify(bike, sp12(0x20, 0x07, 0xd0, 99), now);
    notify(bike, sp12(0x30, 0x00, 0x96, 99), now + 10);
    assert(near(bike.sample().speed.value, 20) && near(bike.sample().power.value, 150));
    assert(near(bike.sample().cadence.value, 212));
    assert(!bike.capabilities().supports(Metric::Resistance));

    SportsplusSource care(Model::CarefitnessBike);
    now = 0;
    assert(care.onConnected(1, now));
    frames = runInit(care, now, {});
    assert(frames.size() >= 6);
    for (int i = 0; i < 6; ++i) assert(frames[i] == Bytes({0x40, 0x00, 0x9a, 0x56, 0x30}));
    care.reset();
    now = 0;
    assert(care.onConnected(1, now));
    assert(drain(care)[0].characteristic == kFff2);
    runInit(care, now, {});
    assert(care.control({Control::Resistance, 0}, now) && care.requestedLevel() == 0);
    notify(care, sp12(0x30, 0x00, 0x65), now);
    assert(near(care.sample().power.value, carefitnessWatts(0, 65)));

    SportsplusSource rower(Model::SportsplusRower);
    const auto rplan = rower.plan();
    assert(rplan.discoverNotifications && rplan.discoveryWriteCharacteristics[0] == kFff2 &&
           rplan.discoveryWriteCharacteristics[1] == kFfe2);
    assert(!rower.onConnected(1, 0));  // no write endpoint discovered
    rower.reset();
    rower.onWriteEndpoint(ble::Uuid::from16(0xffe0), kFfe2);  // wrong service ignored
    assert(!rower.onConnected(1, 0));
    rower.reset();
    rower.onWriteEndpoint(kFff0, kFfe2);
    now = 0;
    assert(rower.onConnected(4, now));
    auto w = drain(rower);
    assert(w.size() == 1 && w[0].characteristic == kFfe2 && w[0].payload() == Bytes({0x40, 0x00, 0xd1, 0x5f, 0x70}));
    rower.reset();
    rower.onWriteEndpoint(kFff0, kFff2);
    rower.onWriteEndpoint(kFff0, kFfe2);  // FFF2 is preferred like upstream
    now = 0;
    assert(rower.onConnected(1, now));
    frames = runInit(rower, now, {});
    assert(frames.size() == 7 && frames[6] == sportsplusPollFrame(0));
    assert(rower.capabilities().supports(Control::Resistance));
    notify(rower, sp12(0x10, 0x00, 0x28, 110), now);
    assert(near(rower.sample().cadence.value, 28) && rower.sample().power.value > 0);
    assert(rower.profile() == MachineProfile::Rower);
}

void pafersSourceTests() {
    SportsplusSource bike(Model::PafersBike);
    const auto plan = bike.plan();
    assert(plan.subscriptionCount == 1 && plan.subscriptions[0].service == kPafersService &&
           plan.subscriptions[0].characteristic == kPafersNotify);
    std::uint32_t now = 0;
    assert(bike.onConnected(1, now));
    auto first = drain(bike);
    assert(first.size() == 1 && first[0].characteristic == kPafersWrite);
    bike.tick(now + 300);
    assert(drain(bike).empty());  // 400 ms timeout
    auto frames = runInit(bike, now, {});
    assert(frames.size() == 3 && bike.initialized());
    now += 2399;
    bike.tick(now);
    assert(drain(bike).empty());
    now += 400;
    bike.tick(now);
    auto w = drain(bike);
    assert(w.size() == 1 && w[0].payload() == pafersPollFrame());
    notify(bike, {0, 0, 0, 27, 0, 12, 0, 85, 0x01, 0x0e}, now);
    assert(near(bike.sample().resistance.value, 12) && near(bike.sample().power.value, 270));
    // Same level as reported: nothing to send; otherwise 55 11 01 <level>.
    assert(bike.control({Control::Resistance, 47.826F}, now));
    now += 400;
    bike.tick(now);
    assert(drain(bike).empty());
    assert(bike.control({Control::Resistance, 100}, now));
    now += 400;
    bike.tick(now);
    w = drain(bike);
    assert(w.size() == 1 && w[0].payload() == pafersResistanceFrame(24));
    assert(bike.control({Control::Resistance, 0}, now) && bike.requestedLevel() == 1);

    SportsplusSource rower(Model::PafersRower);
    assert(rower.profile() == MachineProfile::Rower && std::string(rower.id()) == "pafers-rower");
}

void pafersTreadmillTests() {
    SportsplusSource tm(Model::PafersTreadmill);
    assert(tm.profile() == MachineProfile::Treadmill);
    std::uint32_t now = 0;
    assert(tm.onConnected(1, now));
    assert(!tm.capabilities().supports(Control::Speed));
    const auto frames = runInit(tm, now, {0x55, 0x00, 0x00, 0x00});
    assert(frames.size() >= 9 && frames[6] == Bytes({0x55, 0x0f, 0x02, 0x01, 0x00}));
    for (const auto& f : frames) assert(f != Bytes({0x55, 0x0a, 0x01, 0x01}));  // never start
    assert(!tm.capabilities().supports(Control::Speed));  // no telemetry yet
    Bytes status(13, 0);
    // Belt stopped: controls are rejected (the bridge never starts the belt).
    notify(tm, status, now);
    assert(tm.capabilities().supports(Control::Speed) && tm.capabilities().supports(Control::Incline));
    assert(!tm.capabilities().supports(Control::Resistance));
    assert(!tm.control({Control::Speed, 5}, now) && !tm.control({Control::Incline, 2}, now));
    // Console start event is not answered with a start command.
    notify(tm, {0x55, 0x09, 0x01, 0x01}, now);
    now += 600;
    tm.tick(now);
    for (const auto& w : drain(tm)) assert(w.payload() != Bytes({0x55, 0x0a, 0x01, 0x01}));

    status[9] = 6; status[10] = 50; status[11] = 3;
    notify(tm, status, now);
    assert(near(tm.sample().speed.value, 6.5F) && near(tm.sample().incline.value, 3));
    assert(!tm.control({Control::Speed, 23}, now) && !tm.control({Control::Speed, 0.5F}, now));
    assert(!tm.control({Control::Incline, 16}, now));
    assert(!tm.control({Control::Resistance, 10}, now));
    assert(!tm.control({Control::Speed, 8}, now + 2500));  // stale data
    assert(tm.control({Control::Speed, 8.7F}, now));
    now += 600;
    tm.tick(now);
    auto w = drain(tm);
    assert(w.size() == 1 && w[0].payload() == Bytes({0x55, 0x0f, 0x02, 0x08, 0x00}));
    tm.tick(now + 100);
    assert(drain(tm).empty());  // upstream pauses 400 ms between speed and incline
    notify(tm, status, now + 150);
    tm.tick(now + 200);
    assert(drain(tm).empty());
    now += 400;
    tm.tick(now);
    w = drain(tm);
    assert(w.size() == 1 && w[0].payload() == Bytes({0x55, 0x11, 0x01, 0x03}));
    assert(tm.control({Control::Incline, -2}, now));
    now += 400;
    tm.tick(now);
    w = drain(tm);
    assert(w.size() == 1 && w[0].payload() == pafersSpeedFrame(6));
    now += 400;
    tm.tick(now);
    w = drain(tm);
    assert(w.size() == 1 && w[0].payload() == pafersInclineFrame(0));

    // Console stop is acknowledged with the upstream stop frame.
    notify(tm, {0x55, 0x09, 0x01, 0x00}, now);
    now += 400;
    tm.tick(now);
    w = drain(tm);
    assert(w.size() == 1 && w[0].payload() == pafersStopFrame());

    SportsplusSource ibox(Model::PafersIboxster);
    now = 0;
    assert(ibox.onConnected(1, now));
    const auto iframes = runInit(ibox, now, {});
    assert(iframes.size() >= 10 && iframes[3] == pafersPollFrame() && iframes[7] == Bytes({0x55, 0x0f, 0x02, 0x00, 0x0a}));
    assert(std::string(ibox.id()) == "pafers-iboxster");
}

}  // namespace

void runSportsplusTests() {
    framesTests();
    decodeTests();
    matcherTests();
    sportstechSourceTests();
    sportsplusSourceTests();
    pafersSourceTests();
    pafersTreadmillTests();
    std::printf("sportsplus host tests passed (%u protocols)\n", static_cast<unsigned>(descriptorCount()));
}

// Requires the family to be registered in protocols/registry.cpp.
void runSportsplusRegistryTests() {
    const struct {
        const char* name;
        const char* id;
    } cases[] = {{"EW-BK-1234", "sportstech-bike"},    {"AF7019E", "sportstech-bike"},
                 {"EW-EP-1234", "sportstech-ellip"},   {"EW-ST-1234", "sportstech-rower"},
                 {"CARDIOFIT", "sportsplus-bike"},     {"HT12345678", "sportsplus-ht"},
                 {"CARE9040177", "carefit-bike"},      {"CARE968300122", "sportsplus-rower"},
                 {"VMAX-R1", "sportsplus-rower"},      {"PAFERS_1234", "pafers-bike"}};
    for (const auto& c : cases) {
        const auto* info = detectProtocol(ad(c.name));
        assert(info && std::strcmp(info->id, c.id) == 0);
    }
    for (std::size_t i = 0; i < descriptorCount(); ++i) {
        const auto* info = findProtocol(descriptorAt(i).id);
        assert(info && info->profile == descriptorAt(i).profile);
    }
    const auto* ftms = detectProtocol(ad("EW-BK-1234", true));
    assert(ftms && std::strcmp(ftms->id, "sportstech-bike") != 0);
}
