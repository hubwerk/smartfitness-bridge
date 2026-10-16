#include "protocols/vendor_cardio.hpp"
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
using namespace smartfitness::protocols::vendor_cardio;
using ble::Bytes;

bool near(float a, float b, float eps = 0.01F) { return std::fabs(a - b) < eps; }

ble::Advertisement adv(const std::string& name, bool ftms = false) {
    ble::Advertisement a;
    a.name = name;
    if (ftms) a.services.push_back(ble::Uuid::from16(0x1826));
    return a;
}

std::vector<Bytes> drain(SourceProtocol& s) {
    std::vector<Bytes> out;
    ble::GattWrite w;
    while (s.takeWrite(w)) out.push_back(w.payload());
    return out;
}

std::vector<ble::GattWrite> drainWrites(SourceProtocol& s) {
    std::vector<ble::GattWrite> out;
    ble::GattWrite w;
    while (s.takeWrite(w)) out.push_back(w);
    return out;
}

void notify(SourceProtocol& s, std::size_t index, const Bytes& frame, std::uint32_t now) {
    s.onNotify(index, frame.data(), frame.size(), now);
}

std::unique_ptr<SourceProtocol> create(const char* id) {
    for (std::size_t i = 0; i < descriptorCount(); ++i)
        if (std::strcmp(descriptorAt(i).id, id) == 0) return descriptorAt(i).create();
    assert(false);
    return nullptr;
}

Bytes ascii(const char* text) { return Bytes(text, text + std::strlen(text)); }

void testDescriptors() {
    std::set<std::string> ids;
    assert(descriptorCount() == 19);
    for (std::size_t i = 0; i < descriptorCount(); ++i) {
        const ProtocolInfo& info = descriptorAt(i);
        const std::string id = info.id;
        assert(id.size() <= 16);
        for (char c : id) assert((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
        assert(ids.insert(id).second);
        auto source = info.create();
        assert(source && std::strcmp(source->id(), info.id) == 0);
        assert(source->profile() == info.profile);
        assert(!source->advertisementOnly());
        assert(info.priority == 0 || info.priority == kMatchPriority);
        if (info.priority == 0) assert(!info.matches(adv("")));
    }
    assert(create("proform-rower")->profile() == MachineProfile::Rower);
    assert(create("bh-elliptical")->profile() == MachineProfile::CrossTrainer);
    assert(create("sunnyfit-stepper")->profile() == MachineProfile::StepClimber);
    assert(create("concept2-skierg")->profile() == MachineProfile::Rower);
}

void testMatchers() {
    assert(matchesProformRower(adv("I_RW1234")) && matchesProformRower(adv("i_rw00")));
    assert(!matchesProformRower(adv("I_RW1234", true)));
    ble::Advertisement ifit = adv("I_RW1", true);
    ifit.services.push_back(ble::Uuid::fromString("00001533-1412-efde-1523-785feabcd123"));
    assert(matchesProformRower(ifit));
    assert(!matchesProformRower(adv("I_EL123")) && !matchesProformRower(adv("I_TL")));

    assert(matchesBhElliptical(adv("B01_1234")) && matchesBhElliptical(adv("b01_x", true)));
    assert(matchesYpooU3(adv("YPOO-U3-1")) && matchesYpooU3(adv("MRK-E09")));
    for (const char* n : {"SCH_590E", "KETTLER 1", "CARDIOPOWER EEGO1", "MYELLIPTICAL 2", "MX-AS 9", "SF-123"})
        assert(matchesYpooFtms(adv(n)));
    assert(!matchesYpooFtms(adv("SF-0")) && !matchesYpooFtms(adv("SF-S1")) && !matchesYpooFtms(adv("SF-RW")));
    assert(matchesYpooSch411(adv("SCH411/510E")) && matchesYpooFeier(adv("FEIER-EM-1")));
    assert(matchesYpooTrue(adv("TRUE ELLIPTICAL 123")) && !matchesYpooTrue(adv("TRUE ELLIPTICAL 12")));
    assert(!matchesYpooTrue(adv("TRUE ELLIPTICAL 12A")));

    // E25/E35: FTMS advertisers go to the ypoo FTMS path, others to soleelliptical.
    assert(matchesYpooFtms(adv("E35-1", true)) && !matchesYpooFtms(adv("E35-1")));
    assert(matchesYpooSoleE25(adv("E25-1", true)) && !matchesYpooSoleE25(adv("E25-1")));
    assert(matchesSoleElliptical(adv("E35-1")) && !matchesSoleElliptical(adv("E35-1", true)));
    assert(matchesSoleElliptical(adv("E25")) && !matchesSoleElliptical(adv("E25", true)));
    for (const char* n : {"E95S", "E55", "E95", "E98", "XG400", "E98S"}) assert(matchesSoleElliptical(adv(n)));
    assert(!matchesSoleElliptical(adv("XE88")) && matchesXterraXe88(adv("XE88-1")));
    assert(!matchesSoleElliptical(adv("F80")) && !matchesSoleElliptical(adv("F85")));

    assert(matchesOctane(adv("Q37xx")) && !matchesOctane(adv("Q37", true)));
    assert(matchesSmartRow(adv("SMARTROW 1")) && !matchesSmartRow(adv("SMARTROWER")));
    assert(matchesSunnyFit(adv("SF-S123")) && !matchesSunnyFit(adv("SF-S1", true)));
    assert(matchesSkiErg(adv("PM5 431 SKI")) && matchesSkiErg(adv("pm5 1 ski")));
    assert(!matchesSkiErg(adv("PM5 431 ROW")) && !matchesSkiErg(adv("PM5 SKI", true)));
}

void testProform() {
    const Bytes r1 = proformResistanceFrame(Kind::ProformRower, 1);
    const Bytes e1{0xff, 0x0d, 0x02, 0x04, 0x02, 0x09, 0x14, 0x09, 0x02, 0x02,
                   0x00, 0x10, 0x01, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(r1 == e1);
    const Bytes r4 = proformResistanceFrame(Kind::ProformRower, 4);
    assert(r4.size() == 20 && r4[9] == 0x01 && r4[10] == 0x04 && r4[11] == 0x58 && r4[12] == 0x06 && r4[14] == 0x82);
    assert(proformResistanceFrame(Kind::ProformSportRl, 4) == r4);
    assert(proformResistanceFrame(Kind::ProformSportRl, 1)[11] == 0x76);
    const Bytes r750 = proformResistanceFrame(Kind::Proform750r, 24);
    assert(r750[11] == 0xff && r750[12] == 0x26 && r750[14] == 0x49);
    assert(proformResistanceFrame(Kind::ProformRower, 0).empty() && proformResistanceFrame(Kind::ProformRower, 25).empty());
    assert(proformInitFrames(Kind::Proform750r).size() == 40);
    assert(proformInitFrames(Kind::ProformRower).size() == 23);
    assert(proformInitFrames(Kind::ProformSportRl).size() == 25);
    for (Kind k : {Kind::ProformRower, Kind::Proform750r, Kind::ProformSportRl})
        for (const auto& f : proformInitFrames(k)) assert(f.size() == 4 || f.size() == 20);
    assert(proformResistanceFromByte(0x26) == 24 && proformResistanceFromByte(0x0d) == 8 &&
           proformResistanceFromByte(0x02) == 1);

    auto s = create("proform-rower");
    assert(s->plan().subscriptionCount == 1);
    assert(s->onConnected(1, 0));
    assert(!s->capabilities().supports(Control::Resistance));
    // Init is paced at 400 ms per write.
    auto first = drain(*s);
    assert(first.size() == 1 && first[0] == (Bytes{0xfe, 0x02, 0x08, 0x02}));
    s->tick(100);
    assert(drain(*s).empty());
    std::uint32_t now = 0;
    std::size_t sent = 1;
    while (sent < 23) {
        now += 400;
        s->tick(now);
        sent += drain(*s).size();
    }
    now += 400;
    s->tick(now);
    assert(s->capabilities().supports(Control::Resistance));
    assert(s->control({Control::Resistance, 100.0F}, now));
    std::vector<Bytes> polls;
    for (int i = 0; i < 6; ++i) {
        now += 200;
        s->tick(now);
        for (auto& b : drain(*s)) polls.push_back(b);
    }
    // Default model: resistance after fe 02 19 03 (slot 3), no unlock.
    assert(polls.size() == 7);
    assert(polls[3] == (Bytes{0xfe, 0x02, 0x19, 0x03}));
    assert(polls[4] == proformResistanceFrame(Kind::ProformRower, 24));

    Bytes data{0x00, 0x12, 0x01, 0x04, 0x02, 0x30, 0, 0, 0, 0, 0, 0x26, 0x2c, 0x01, 0xe8, 0x03, 0, 0, 0, 0};
    notify(*s, 0, data, now);
    assert(near(s->sample().power.value, 300) && near(s->sample().resistance.value, 24));
    assert(near(s->sample().distance.value, 1000));
    Bytes blank = data;
    for (std::size_t i = 12; i < 20; ++i) blank[i] = 0xff;
    blank[11] = 0x01;
    notify(*s, 0, blank, now);
    assert(near(s->sample().resistance.value, 24));
    Bytes stroke(20, 0);
    stroke[0] = 0xff;
    stroke[1] = 0x11;
    stroke[12] = 24;
    stroke[13] = 120;  // 120 s / 500 m
    notify(*s, 0, stroke, now);
    assert(near(s->sample().cadence.value, 24) && near(s->sample().speed.value, 15) && near(s->sample().pace.value, 120));
    stroke[12] = 0;
    notify(*s, 0, stroke, now);
    assert(near(s->sample().cadence.value, 24));
    stroke[3] = 1;
    stroke[12] = 30;
    notify(*s, 0, stroke, now);
    assert(near(s->sample().cadence.value, 24));

    // Sport RL: unlock + resistance before slot 3.
    auto sport = create("proform-sport-rl");
    assert(sport->onConnected(1, 0));
    now = 0;
    while (!sport->capabilities().supports(Control::Resistance)) {
        now += 400;
        sport->tick(now);
        drain(*sport);
    }
    assert(sport->control({Control::Resistance, 0.0F}, now));
    polls.clear();
    for (int i = 0; i < 6; ++i) {
        now += 200;
        sport->tick(now);
        for (auto& b : drain(*sport)) polls.push_back(b);
    }
    assert(polls.size() == 8);
    assert(polls[3] == proformUnlockFrame() && polls[4] == proformResistanceFrame(Kind::ProformSportRl, 1));
    assert(polls[5] == (Bytes{0xfe, 0x02, 0x19, 0x03}));
}

void testBh() {
    assert(bhResistanceFrame(6) == (Bytes{0x11, 0x00, 0x00, 0x00, 0x00, 0x21, 0x22}));
    assert(bhResistanceFrame(1) == (Bytes{0x11, 0x00, 0x00, 0x5b, 0xff, 0x21, 0x22}));  // -165
    auto s = create("bh-elliptical");
    assert(s->onConnected(1, 0));
    assert(drain(*s).empty());
    // flags: moreData off (speed present), cadence, distance, resistance, power, energy, HR
    Bytes frame{0x74, 0x03, 0xe8, 0x03, 0x78, 0x00, 0x10, 0x27, 0x00, 0x05, 0x00, 0x96, 0x00,
                0x2a, 0x00, 0x00, 0x00, 0x00, 0x8c};
    notify(*s, 0, frame, 10);
    const auto& m = s->sample();
    assert(near(m.cadence.value, 60) && near(m.speed.value, 6));
    assert(near(m.distance.value, 10000) && near(m.resistance.value, 5) && near(m.power.value, 150));
    assert(near(m.totalEnergy.value, 42) && near(m.heartRate.value, 140));
    notify(*s, 2, Bytes{0x00, 0x50}, 20);
    assert(near(s->sample().heartRate.value, 80));
    assert(s->control({Control::Resistance, 0.0F}, 30));
    auto w = drainWrites(*s);
    assert(w.size() == 1 && w[0].characteristic == ble::Uuid::from16(0x2ad9) && w[0].payload() == bhResistanceFrame(1));
    assert(!s->capabilities().supports(Control::Incline));
    MetricSet out;
    assert(!decodeBikeData(true, frame.data(), 1, out));
}

void testYpoo() {
    assert(ypooResistanceFrame(10) == (Bytes{0x02, 0x44, 0x05, 0x0a, 0x00, 0x43, 0x03}));
    const Bytes g = gymstickResistanceFrame(8, 6);
    assert(g[3] == 8 && g[4] == 2 && g[5] == (0x44 ^ 0x05 ^ 0x08 ^ 0x02));
    assert(gymstickResistanceFrame(4, 6)[4] == 0xfe);
    assert(ftmsResistanceFrame(12) == (Bytes{0x04, 120}));
    assert(ftmsInclinationFrame(5.7F) == (Bytes{0x03, 50, 0}));
    assert(ypooInitFrames(false).size() == 9 && ypooInitFrames(true).size() == 8);
    assert(ypooPollFrame(0).size() == 4 && ypooPollFrame(1).size() == 5);

    // FTMS group variant: request control on connect, speed/cadence from 2ACE.
    auto s = create("ypoo-ftms-el");
    assert(s->plan().subscriptionCount == 4);
    assert(s->onConnected(1, 0));
    assert(drain(*s) == std::vector<Bytes>{Bytes{0x00}});
    // flags 0x00008c 0x?? : speed, stepCount(0x08), resistance(0x80), power(0x100), HR(0x800)
    const std::uint32_t flags = 0x0008 | 0x0080 | 0x0100 | 0x0800;
    Bytes cross{static_cast<std::uint8_t>(flags), static_cast<std::uint8_t>(flags >> 8), 0,
                0xe8, 0x03,              // 10 km/h
                0x78, 0x00, 0x00, 0x00,  // step rate 120 -> 60
                0x07, 0x00,              // resistance 7
                0x64, 0x00,              // power 100
                0x82};                   // hr 130
    notify(*s, 0, cross, 10);
    const auto& m = s->sample();
    assert(near(m.speed.value, 10) && near(m.cadence.value, 60) && near(m.resistance.value, 7));
    assert(near(m.power.value, 100) && near(m.heartRate.value, 130));
    assert(s->capabilities().supports(Control::Resistance) && s->capabilities().supports(Control::Incline));
    assert(s->control({Control::Resistance, 100.0F}, 20));
    assert(drain(*s) == std::vector<Bytes>{ftmsResistanceFrame(25)});
    assert(s->control({Control::Incline, 0.0F}, 20));
    assert(drain(*s) == std::vector<Bytes>{ftmsInclinationFrame(1.0F)});

    // Truncated frame keeps the fields parsed so far.
    CrossData partial;
    assert(decodeYpooCross(Kind::YpooFtms, cross.data(), 6, partial));
    assert(near(partial.metrics.speed.value, 10) && !partial.metrics.cadence.valid);
    assert(!decodeYpooCross(Kind::YpooFtms, cross.data(), 2, partial));

    // FEIER also sends start/resume; TRUE has no incline control.
    auto feier = create("ypoo-feier");
    assert(feier->onConnected(1, 0));
    assert(drain(*feier) == (std::vector<Bytes>{Bytes{0x00}, Bytes{0x07}}));
    auto tr = create("ypoo-true-el");
    assert(tr->onConnected(1, 0));
    assert(!tr->capabilities().supports(Control::Incline));

    // U3: proprietary init, split 18+17 frame, power /100, raw distance.
    auto u3 = create("ypoo-u3");
    assert(u3->onConnected(3, 0));
    auto init = drain(*u3);
    assert(init.size() == 8);
    u3->tick(1);
    init = drain(*u3);
    assert(init.size() == 1 && init[0] == ypooInitFrames(false)[8]);
    const std::uint32_t uf = 0x0002 | 0x0004 | 0x0010 | 0x0080 | 0x0100;
    Bytes full{static_cast<std::uint8_t>(uf), static_cast<std::uint8_t>(uf >> 8), 0,
               0x00, 0x00,              // speed (ignored for U3)
               0xd0, 0x07,              // avg speed 20 km/h
               0x10, 0x27, 0x00,        // 10000 m
               0x0a, 0x00,              // stride 10
               0x03, 0x00,              // resistance 3
               0x10, 0x27};             // power 10000/100
    full.resize(35, 0);
    notify(*u3, 0, Bytes(full.begin(), full.begin() + 18), 100);
    assert(!u3->hasData());
    notify(*u3, 0, Bytes(full.begin() + 18, full.end()), 100);
    assert(near(u3->sample().speed.value, 20) && near(u3->sample().distance.value, 10000));
    assert(near(u3->sample().power.value, 100) && near(u3->sample().strideCount.value, 10));
    // Stride cadence: 30 strides in 15 s -> 120/min /2 = 60.
    full[10] = 40;
    notify(*u3, 0, Bytes(full.begin(), full.begin() + 18), 15100);
    notify(*u3, 0, Bytes(full.begin() + 18, full.end()), 15100);
    assert(near(u3->sample().cadence.value, 60));
    // Poll alternates every 200 ms.
    u3->tick(15300);
    auto poll = drain(*u3);
    assert(poll.size() == 1 && poll[0] == ypooPollFrame(0));
    assert(u3->control({Control::Resistance, 0.0F}, 15400));
    assert(drain(*u3) == std::vector<Bytes>{ypooResistanceFrame(1)});

    // iConsole: init only after the first notification.
    auto ic = create("ypoo-iconsole");
    assert(ic->plan().subscriptionCount == 3);
    assert(ic->onConnected(4, 0));
    assert(drain(*ic).empty() && !ic->capabilities().supports(Control::Resistance));
    Bytes icFrame(15, 0);
    icFrame[6] = 55;
    icFrame[9] = 0xe8;
    icFrame[10] = 0x03;
    notify(*ic, 2, icFrame, 10);
    assert(drain(*ic).size() == 8);
    assert(near(ic->sample().speed.value, 10) && near(ic->sample().cadence.value, 55));
    assert(ic->capabilities().supports(Control::Resistance));

    auto gs = create("gymstick-gx60");
    assert(gs->onConnected(1, 0));
    notify(*gs, 0, icFrame, 10);
    drain(*gs);
    for (int i = 0; i < 4; ++i) {
        gs->tick(10 + 200 * (i + 1));
        drain(*gs);
    }
    assert(gs->control({Control::Resistance, 0.0F}, 2000));
    assert(drain(*gs) == std::vector<Bytes>{gymstickResistanceFrame(1, 6)});

    // SCH411 heart rate at byte 23.
    Bytes sch(24, 0);
    sch[0] = 0x01;  // no instantaneous speed
    sch[23] = 99;
    CrossData d;
    assert(decodeYpooCross(Kind::YpooSch411, sch.data(), sch.size(), d) && near(d.metrics.heartRate.value, 99));
}

void testSole() {
    auto init = soleInitFrames(false);
    assert(init.size() == 10 && init[5] == (Bytes{0x5b, 0x06, 0x07, 0x01, 0x23, 0x00, 0xa5, 0xaa, 0x5d}));
    assert(soleInitFrames(true).size() == 20);
    assert(solePollFrame(false, 1) == (Bytes{0x5b, 0x04, 0x00, 0x10, 0x4f, 0x4b, 0x5d}));
    assert(solePollFrame(true, 2)[3] == 0x12);
    assert(soleStepFrames(true)[1] == (Bytes{0x5b, 0x02, 0xf1, 0x02, 0x5d}));
    assert(soleStepFrames(false)[0] == (Bytes{0x5b, 0x02, 0xf1, 0x03, 0x5d}));

    auto s = create("sole-elliptical");
    assert(s->onConnected(1, 0));
    assert(drain(*s).size() == 8);
    s->tick(1);
    assert(drain(*s).size() == 2);
    Bytes data(20, 0);
    data[7] = 0x01; data[8] = 0xf4;    // 50 kcal
    data[10] = 70;                      // cadence
    data[11] = 0x01; data[12] = 0xf4;  // 5.00 mph
    data[13] = 0x00; data[14] = 0x96;  // 150 W
    data[18] = 120;
    notify(*s, 0, data, 10);
    const auto& m = s->sample();
    assert(near(m.speed.value, 8.0467F) && near(m.totalEnergy.value, 50) && near(m.cadence.value, 70));
    assert(near(m.power.value, 150) && near(m.heartRate.value, 120));
    notify(*s, 0, Bytes{0x5b, 0x02, 0x13, 0x02, 0x5d}, 20);
    assert(near(s->sample().resistance.value, 3));
    notify(*s, 0, Bytes{0x5b, 0x02, 0x12, 0x04, 0x5d}, 20);
    assert(near(s->sample().incline.value, 5));
    assert(!s->capabilities().supports(Control::Incline));

    // Stepping: one step per status ack, towards level 4.
    assert(s->control({Control::Resistance, 16.0F}, 30));
    s->tick(400);
    auto w = drain(*s);
    assert(w.size() == 3 && w[1] == soleStepFrames(true)[0] && w[2] == soleStepFrames(true)[1]);
    s->tick(700);
    assert(drain(*s).size() == 1);  // waits for status
    notify(*s, 0, Bytes{0x5b, 0x02, 0x13, 0x03, 0x5d}, 800);
    s->tick(1000);
    assert(drain(*s).size() == 1);  // target reached: poll only
    s->tick(1300);
    assert(drain(*s).empty());  // every 4th 300 ms slot is skipped (sec1Update)

    SoleFrame f;
    Bytes xe(20, 0);
    xe[9] = 101;
    assert(decodeSole(true, xe.data(), xe.size(), f) && near(f.metrics.heartRate.value, 101));
    assert(!decodeSole(false, xe.data(), 19, f));
}

void testOctane() {
    assert(octaneInitFrames().size() == 3);
    Bytes f(20, 0);
    f[0] = 0x01; f[1] = 0x09; f[2] = 0x07;   // resistance 7
    f[4] = 0x02; f[5] = 0x11; f[6] = 0x8c;   // hr 140
    f[8] = 0x01; f[9] = 0x07; f[10] = 100;   // cadence 100
    MetricSet m;
    assert(decodeOctane(f.data(), f.size(), m));
    assert(near(m.resistance.value, 7) && near(m.heartRate.value, 140) && near(m.cadence.value, 100));
    assert(near(m.speed.value, 50 * 52.07F * 60 / 10000 * 1.1218F));
    Bytes g(20, 0);
    g[0] = 0xa5; g[6] = 0x07; g[7] = 80;
    assert(decodeOctane(g.data(), g.size(), m) && near(m.cadence.value, 80));
    assert(!decodeOctane(g.data(), 19, m));
    auto s = create("octane-q37");
    assert(s->onConnected(1, 0) && drain(*s).size() == 3);
    assert(!s->capabilities().supports(Control::Resistance));
}

void testSmartRow() {
    // seed 0x1234 -> 4660*17923/256 = 326254 = 0x04fa6e -> "fa6e"
    const std::string challenge = "KEYLOCK=011234";
    unsigned sum = 0;
    for (char c : challenge) sum += static_cast<unsigned char>(c);
    char check[8];
    std::snprintf(check, sizeof(check), "%02X", sum & 0xff);
    const Bytes resp = smartRowChallengeResponse(" " + challenge + check + "\r\n");
    assert(resp == (Bytes{0x0d, 'f', 'a', '6', 'e', 0x0d}));
    assert(smartRowChallengeResponse(challenge + "00") == Bytes{0x23});
    assert(smartRowChallengeResponse("KEYLOCK=1") == Bytes{0x23});

    MetricSet m;
    assert(decodeSmartRow(false, ascii("c01234150xxxxxxxx").data(), 17, m));
    assert(near(m.distance.value, 1234) && near(m.power.value, 150));
    assert(decodeSmartRow(false, ascii("d01234250012300xx").data(), 17, m));
    assert(near(m.cadence.value, 25) && near(m.strokeCount.value, 123));
    assert(decodeSmartRow(false, ascii("e01234200xxxxxxxx").data(), 17, m));
    assert(near(m.pace.value, 120) && near(m.speed.value, 15));
    Bytes v3 = ascii("c\x41\x42\x43\x44\x45" "150xxxxxxxx");
    assert(decodeSmartRow(true, v3.data(), 17, m) && near(m.distance.value, 12345));
    assert(!decodeSmartRow(false, v3.data(), 16, m));

    auto s = create("smartrow");
    assert(s->onConnected(1, 0));
    std::vector<Bytes> sent = drain(*s);
    for (std::uint32_t t = 200; t <= 1000; t += 200) {
        s->tick(t);
        for (auto& b : drain(*s)) sent.push_back(b);
    }
    assert(sent.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) assert(sent[i] == Bytes{smartRowInitBytes()[i]});
    const Bytes hello = ascii("V3.1");
    notify(*s, 0, hello, 1100);
    s->tick(1200);
    assert(drain(*s) == std::vector<Bytes>{Bytes{0x23}});
    notify(*s, 0, ascii("e01234200xxxxxxxx"), 1300);
    assert(near(s->sample().speed.value, 15));
}

void testSunnyFit() {
    assert(sunnyFitInitFrames().size() == 4);
    auto s = create("sunnyfit-stepper");
    assert(s->plan().subscriptionCount == 2);
    assert(!s->onConnected(1, 0));
    s->reset();
    assert(s->onConnected(2, 0) && drain(*s).size() == 4);
    Bytes a(20, 0), b(12, 0);
    a[0] = 0x5a;
    a[6] = 90;
    a[10] = 0xe8; a[11] = 0x03;  // 1000 steps
    notify(*s, 1, a, 10);
    assert(!s->hasData());
    notify(*s, 1, b, 20);
    assert(near(s->sample().cadence.value, 90) && near(s->sample().stepCount.value, 1000));
    assert(near(s->sample().positiveElevationGain.value, 200));
    notify(*s, 1, b, 30);  // stray continuation ignored
    MetricSet m;
    a.insert(a.end(), b.begin(), b.end());
    a[0] = 0x00;
    assert(!decodeSunnyFit(a.data(), a.size(), m));
}

void testSkiErg() {
    auto s = create("concept2-skierg");
    assert(s->plan().subscriptionCount == 1);
    assert(s->onConnected(1, 0) && drain(*s).empty());
    Bytes p31(20, 0);
    p31[0] = 0x31; p31[4] = 0x10; p31[5] = 0x27; p31[10] = 1; p31[19] = 110;  // 1000 m
    notify(*s, 0, p31, 10);
    assert(near(s->sample().distance.value, 1000) && near(s->sample().resistance.value, 110));
    Bytes p32(19, 0);
    p32[0] = 0x32; p32[4] = 0x10; p32[5] = 0x0e; p32[6] = 40; p32[7] = 150;  // 3600 * .0036
    notify(*s, 0, p32, 20);
    assert(near(s->sample().speed.value, 12.96F) && near(s->sample().cadence.value, 40));
    assert(near(s->sample().heartRate.value, 150));
    Bytes p36(16, 0);
    p36[0] = 0x36; p36[4] = 200;
    notify(*s, 0, p36, 30);
    assert(near(s->sample().power.value, 200));
    Bytes p33(19, 0);
    p33[0] = 0x33; p33[5] = 42;
    notify(*s, 0, p33, 40);
    assert(near(s->sample().totalEnergy.value, 42));
    Bytes p35(19, 0);
    p35[0] = 0x35; p35[17] = 9;
    notify(*s, 0, p35, 50);
    assert(near(s->sample().strokeCount.value, 9));
    p31[10] = 0;
    notify(*s, 0, p31, 60);
    assert(near(s->sample().speed.value, 0) && near(s->sample().power.value, 0));
    notify(*s, 0, p36, 70);
    assert(near(s->sample().power.value, 0));
    bool active = true;
    MetricSet m;
    assert(!decodeSkiErg(p31.data(), 19, active, m));
    assert(!s->capabilities().supports(Control::Resistance));
}

}  // namespace

void runVendorCardioTests() {
    testDescriptors();
    testMatchers();
    testProform();
    testBh();
    testYpoo();
    testSole();
    testOctane();
    testSmartRow();
    testSunnyFit();
    testSkiErg();
    std::printf("vendor_cardio host tests passed (%zu protocols)\n", descriptorCount());
}

void runVendorCardioRegistryTests() {
    for (std::size_t i = 0; i < descriptorCount(); ++i) assert(findProtocol(descriptorAt(i).id));
    assert(std::strcmp(detectProtocol(adv("I_RW123"))->id, "proform-rower") == 0);
    assert(std::strcmp(detectProtocol(adv("E35-1"))->id, "sole-elliptical") == 0);
    assert(std::strcmp(detectProtocol(adv("E35-1", true))->id, "ypoo-ftms-el") == 0);
    assert(std::strcmp(detectProtocol(adv("PM5 430 SKI"))->id, "concept2-skierg") == 0);
    assert(std::strcmp(detectProtocol(adv("PM5 430 ROW"))->id, "concept2-pm5") == 0);
    assert(std::strcmp(detectProtocol(adv("SF-S1"))->id, "sunnyfit-stepper") == 0);
}
