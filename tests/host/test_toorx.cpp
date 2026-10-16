#include "protocols/toorx.hpp"
#include "protocols/registry.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace smartfitness;
using namespace smartfitness::protocols;
using ble::Bytes;
using toorx::Parse;

bool near(float a, float b) { return std::fabs(a - b) < 0.01F; }

std::uint8_t sum(const Bytes& bytes, std::size_t count) {
    std::uint8_t s = 0;
    for (std::size_t i = 0; i < count; ++i) s = static_cast<std::uint8_t>(s + bytes[i]);
    return s;
}

std::size_t indexOf(const char* id) {
    for (std::size_t i = 0; i < toorx::descriptorCount(); ++i)
        if (std::strcmp(toorx::descriptorAt(i).id, id) == 0) return i;
    assert(false && "unknown toorx id");
    return 0;
}

std::unique_ptr<ToorxSource> source(const char* id) {
    return std::unique_ptr<ToorxSource>(new ToorxSource(indexOf(id)));
}

std::vector<ble::GattWrite> drain(SourceProtocol& s) {
    std::vector<ble::GattWrite> out;
    ble::GattWrite w;
    while (s.takeWrite(w)) out.push_back(w);
    return out;
}

// Runs the init handshake with a console that never answers.
std::vector<ble::GattWrite> runInit(ToorxSource& s, std::uint32_t& now, std::uint32_t mask = 0xff) {
    s.reset();
    assert(s.onConnected(mask, now));
    std::vector<ble::GattWrite> writes = drain(s);
    for (int i = 0; i < 64 && !s.initialized(); ++i) {
        now += 1000;
        s.tick(now);
        for (const auto& w : drain(s)) writes.push_back(w);
    }
    assert(s.initialized());
    return writes;
}

// Value encoded as upstream "(hi - 1) * 100 + (lo - 1)".
void put100(Bytes& f, std::size_t hi, int value) {
    f[hi] = static_cast<std::uint8_t>(value / 100 + 1);
    f[hi + 1] = static_cast<std::uint8_t>(value % 100 + 1);
}

Bytes treadmillFrame(int speedTenths, int incline, std::size_t length = 19, std::uint8_t type = 0xb2,
                     std::uint8_t a = 0x01) {
    Bytes f(length, 0x01);
    f[0] = 0xf0;
    f[1] = type;
    f[2] = a;
    f[3] = 0xd3;
    put100(f, 12, speedTenths);
    f[14] = static_cast<std::uint8_t>(incline + 1);
    f[length - 1] = sum(f, length - 1);
    return f;
}

Bytes bikeFrame(std::size_t length = 21) {
    Bytes f(length, 0x01);
    f[0] = 0xf0;
    f[1] = 0xb2;
    put100(f, 6, 253);   // 25.3 km/h
    put100(f, 8, 85);    // 85 rpm
    put100(f, 12, 123);  // 123 kcal
    put100(f, 14, 140);  // 140 bpm
    f[16] = 16;          // 150.0 W
    f[17] = 1;
    f[18] = 9;  // resistance 8
    return f;
}

void frameTests() {
    // Every transcribed init/poll frame carries the 8-bit sum, except the
    // upstream DKN Motion "40 00 9a 46 20" frame and the upstream rower a4
    // frame, which ends in 0x83 instead of 0x86 (kept verbatim).
    const Bytes dknOdd{0x40, 0x00, 0x9a, 0x46, 0x20};
    const Bytes rowerOdd{0xf0, 0xa4, 0x01, 0xe7, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x83};
    std::size_t checked = 0;
    for (std::size_t v = 0; v < toorx::variantCount(); ++v) {
        const auto& variant = toorx::variantAt(v);
        assert(variant.steps > 0);
        for (std::size_t s = 0; s < variant.steps; ++s) {
            const Bytes f = toorx::frameBytes(variant.frames[variant.order[s]]);
            assert(f.size() >= 5 && f.size() <= 16);
            if (f == dknOdd || f == rowerOdd) continue;
            assert(f[0] == 0xf0);
            assert(sum(f, f.size() - 1) == f.back());
            ++checked;
        }
        const Bytes poll = toorx::frameBytes(variant.poll);
        assert(poll.size() == 5 && poll[0] == 0xf0 && poll[1] == 0xa2 && sum(poll, 4) == poll[4]);
    }
    assert(checked > 200);

    assert(toorx::withSum({0xf0, 0xa2, 0x01, 0xd3}) == Bytes({0xf0, 0xa2, 0x01, 0xd3, 0x66}));
    assert(toorx::resistanceFrame(0x01, 0x01, 5) == Bytes({0xf0, 0xa6, 0x01, 0x01, 0x06, 0x9e}));
    assert(toorx::resistanceFrame(0x5b, 0x01, 1) == Bytes({0xf0, 0xa6, 0x5b, 0x01, 0x02, 0xf4}));
    // Upstream DKN init "f0 a6 02 01 02 9b" is the level-1 resistance frame.
    assert(toorx::resistanceFrame(0x02, 0x01, 1) == Bytes({0xf0, 0xa6, 0x02, 0x01, 0x02, 0x9b}));
    // Upstream TM_DKN "f0 ac 01 d3 01 64 64 39" is the 0 % incline frame.
    assert(toorx::inclineFrame(0x01, 0xd3, 0.0F) == Bytes({0xf0, 0xac, 0x01, 0xd3, 0x01, 0x64, 0x64, 0x39}));
    assert(toorx::inclineFrame(0x01, 0xd3, 5.7F) == Bytes({0xf0, 0xac, 0x01, 0xd3, 0x06, 0x64, 0x64, 0x3e}));
    assert(toorx::jllPollFrame(0) == Bytes({0xf0, 0xa2, 0x0b, 0xc8, 0x65}));
    assert(toorx::jllPollFrame(1) == Bytes({0xf0, 0xa2, 0x17, 0xc8, 0x71}));
    for (unsigned i = 0; i < 40; ++i) {
        const Bytes f = toorx::jllPollFrame(i);
        assert(sum(f, 4) == f[4]);
    }
}

void decodeTests() {
    MetricSet m;
    // Treadmill: 8.5 km/h, 3 %.
    Bytes tm = treadmillFrame(85, 3);
    assert(toorx::decode(Parse::Treadmill, tm.data(), tm.size(), m));
    assert(near(m.speed.value, 8.5F) && near(m.incline.value, 3) && !m.heartRate.valid && !m.power.valid);
    tm[14] = 0;  // negative incline clamps to 0
    assert(toorx::decode(Parse::Treadmill, tm.data(), tm.size(), m) && near(m.incline.value, 0));
    assert(!toorx::decode(Parse::Treadmill, tm.data(), 18, m));
    Bytes dkn = treadmillFrame(120, 2, 18);
    assert(toorx::decode(Parse::TreadmillDkn, dkn.data(), dkn.size(), m) && near(m.speed.value, 12.0F));
    assert(!toorx::decode(Parse::TreadmillDkn, tm.data(), tm.size(), m));
    Bytes flow = treadmillFrame(60, 1, 19, 0xb0, 0x2e);
    assert(toorx::decode(Parse::TreadmillFlow, flow.data(), flow.size(), m) && near(m.speed.value, 6.0F));
    flow[2] = 0x01;
    assert(!toorx::decode(Parse::TreadmillFlow, flow.data(), flow.size(), m));

    // Bike (21 bytes).
    Bytes bike = bikeFrame();
    assert(toorx::decode(Parse::Bike, bike.data(), bike.size(), m));
    assert(near(m.speed.value, 25.3F) && near(m.cadence.value, 85) && near(m.totalEnergy.value, 123));
    assert(near(m.heartRate.value, 140) && m.heartRate.valid && near(m.power.value, 150) &&
           near(m.resistance.value, 8));
    assert(!toorx::decode(Parse::Bike, bike.data(), 20, m));
    Bytes idle = bike;
    put100(idle, 8, 0);
    assert(toorx::decode(Parse::Bike, idle.data(), idle.size(), m) && near(m.power.value, 0) && m.power.valid);
    assert(toorx::decode(Parse::BikeSrx500, bike.data(), 19, m) && near(m.cadence.value, 85));
    assert(!toorx::decode(Parse::BikeSrx500, bike.data(), 18, m));

    // Tunturi / FAL: speed estimated from cadence, no kcal.
    assert(toorx::decode(Parse::BikeTunturi, bike.data(), 20, m));
    assert(near(m.speed.value, 85 * 0.37407407F) && !m.totalEnergy.valid && near(m.power.value, 150) &&
           near(m.heartRate.value, 140));
    assert(!toorx::decode(Parse::BikeTunturi, bike.data(), 19, m));
    assert(toorx::decode(Parse::BikeFal, bike.data(), 19, m) && near(m.resistance.value, 8));

    // Raw 19-byte layout (JLL IC400 / Asviva / Hammer).
    Bytes raw(19, 0);
    raw[8] = 0;
    raw[9] = 72;
    raw[16] = 1;
    raw[17] = 10;  // 10 + 83 = 93 bpm
    assert(toorx::decode(Parse::BikeRaw19, raw.data(), raw.size(), m));
    assert(near(m.cadence.value, 72) && near(m.heartRate.value, 93) && !m.power.valid);
    assert(!toorx::decode(Parse::BikeRaw19, raw.data(), 21, m));
    Bytes fytter(19, 0);
    fytter[9] = 60;
    fytter[6] = 2;
    fytter[7] = 20;  // 120 W
    put100(fytter, 14, 110);
    assert(toorx::decode(Parse::BikeFytter, fytter.data(), fytter.size(), m));
    assert(near(m.cadence.value, 60) && near(m.power.value, 120) && near(m.heartRate.value, 110));
    fytter[6] = 0;
    assert(toorx::decode(Parse::BikeFytter, fytter.data(), fytter.size(), m) && near(m.power.value, 0));

    // Ellipticals.
    Bytes ell = bikeFrame();
    ell[8] = 0;
    ell[9] = 45;
    assert(toorx::decode(Parse::Elliptical, ell.data(), ell.size(), m));
    assert(near(m.cadence.value, 45) && near(m.speed.value, 25.3F) && near(m.power.value, 150) &&
           near(m.resistance.value, 8) && !m.heartRate.valid);
    assert(toorx::decode(Parse::EllipticalToorx, bike.data(), bike.size(), m) && near(m.cadence.value, 85));
    ell[5] = 50;
    assert(toorx::decode(Parse::EllipticalJtx, ell.data(), ell.size(), m));
    assert(near(m.cadence.value, 50) && near(m.speed.value, 16.5F) && !m.power.valid && near(m.resistance.value, 8));
    assert(!toorx::decode(Parse::Elliptical, ell.data(), 19, m));

    // Rowers (23 bytes).
    Bytes row(23, 0x01);
    put100(row, 6, 105);
    row[17] = 2;  // power ((b17-1)*100 + (b18-1)) / 10 = (100 + 7) / 10
    row[18] = 8;  // resistance 7
    row[20] = 25;
    assert(toorx::decode(Parse::Rower, row.data(), row.size(), m));
    assert(near(m.speed.value, 10.5F) && near(m.resistance.value, 7) && near(m.cadence.value, 24) &&
           near(m.power.value, 10.7F));
    assert(!toorx::decode(Parse::Rower, row.data(), 21, m));
    Bytes mrk(23, 0x01);
    mrk[4] = 3;    // 2 min
    mrk[5] = 11;   // 10 s
    mrk[7] = 51;   // 50 strokes
    mrk[9] = 23;   // 22 spm
    mrk[10] = 2;   // 99 m
    mrk[11] = 2;   // +1 m
    mrk[13] = 31;  // 30 kcal
    mrk[15] = 121; // 120 bpm
    mrk[16] = 2;
    mrk[17] = 2;  // 100 W
    mrk[20] = 6;  // resistance 5
    assert(toorx::decode(Parse::RowerMearch, mrk.data(), mrk.size(), m));
    assert(near(m.elapsed.value, 130) && near(m.strokeCount.value, 50) && near(m.cadence.value, 22) &&
           near(m.distance.value, 100) && near(m.totalEnergy.value, 30) && near(m.heartRate.value, 120) &&
           near(m.power.value, 100) && near(m.resistance.value, 5) && !m.speed.valid);
    assert(!toorx::decode(Parse::RowerMearch, mrk.data(), 22, m));
    assert(!toorx::decode(Parse::Bike, nullptr, 21, m));
}

void planTests() {
    auto tm = source("trx-tm");
    auto plan = tm->plan();
    assert(plan.subscriptionCount == 1);
    assert(plan.subscriptions[0].service == toorx::kFff0Service && plan.subscriptions[0].characteristic == toorx::kFff1Notify);

    auto irun = source("trx-bike-irun");
    plan = irun->plan();
    assert(plan.subscriptionCount == 3);
    assert(plan.subscriptions[0].service == toorx::kIsscService && plan.subscriptions[0].characteristic == toorx::kIsscNotify);
    assert(plan.subscriptions[1].service == toorx::kFff0Service && plan.subscriptions[1].characteristic == toorx::kFff1Notify);
    assert(plan.subscriptions[2].service == toorx::kIsscService && plan.subscriptions[2].characteristic == toorx::kIsscNotify2);

    auto dkn = source("trx-tm-dknrun");
    plan = dkn->plan();
    assert(plan.subscriptionCount == 2 && plan.subscriptions[1].characteristic == toorx::kIsscNotify3);

    auto dknBike = source("trx-bike-dkn");
    plan = dknBike->plan();
    assert(plan.subscriptionCount == 2 && plan.subscriptions[0].service == toorx::kFff0Service &&
           plan.subscriptions[1].service == toorx::kIsscService);
    assert(!dknBike->onConnected(0, 0) && !dknBike->lastError().empty());
}

void initTests() {
    // Every descriptor: the full upstream sequence, then the poll frame.
    for (std::size_t i = 0; i < toorx::descriptorCount(); ++i) {
        ToorxSource s(i);
        const auto& v = toorx::variantAt(i);
        std::uint32_t now = 1000;
        const auto writes = runInit(s, now);
        assert(writes.size() == v.steps);
        for (std::size_t k = 0; k < v.steps; ++k) {
            assert(writes[k].payload() == toorx::frameBytes(v.frames[v.order[k]]));
            assert(writes[k].withResponse == v.withResponse);
            const bool issc = v.primary == toorx::Service::Issc;
            assert(writes[k].service == (issc ? toorx::kIsscService : toorx::kFff0Service));
            assert(writes[k].characteristic == (issc ? toorx::kIsscWrite : toorx::kFff2Write));
        }
        now += v.pollMs;
        s.tick(now);
        const auto poll = drain(s);
        assert(poll.size() == 1);
        assert(poll[0].payload() == (v.pollCounter ? toorx::jllPollFrame(0) : toorx::frameBytes(v.poll)));
        assert(std::strlen(s.id()) <= 16);
    }

    // Pacing: a silent console gets 300 ms + sleep, an answer shortens it to the sleep.
    auto tm = source("trx-tm");
    assert(tm->onConnected(1, 0));
    assert(drain(*tm).size() == 1);
    tm->tick(200);
    assert(drain(*tm).empty());
    tm->tick(300);
    assert(drain(*tm).size() == 1);
    const Bytes ack{0xf0, 0xb5, 0x01, 0xd3, 0x01};
    tm->onNotify(0, ack.data(), ack.size(), 310);
    tm->tick(320);
    assert(drain(*tm).size() == 1);

    auto irun = source("trx-tm-irun");
    assert(irun->onConnected(1, 0));
    drain(*irun);
    irun->onNotify(0, ack.data(), ack.size(), 10);
    irun->tick(300);
    assert(drain(*irun).empty());
    irun->tick(400);
    assert(drain(*irun).size() == 1);

    // Polls wait for the outbox and follow pollMs; JLL increments its counter.
    auto jll = source("trx-bike-jll");
    std::uint32_t now = 0;
    runInit(*jll, now);
    now += 500;
    jll->tick(now);
    assert(drain(*jll)[0].payload() == toorx::jllPollFrame(0));
    jll->tick(now + 100);
    assert(drain(*jll).empty());
    jll->tick(now + 500);
    assert(drain(*jll)[0].payload() == toorx::jllPollFrame(1));
    jll->reset();
    now = 0;
    runInit(*jll, now);
    now += 500;
    jll->tick(now);
    assert(drain(*jll)[0].payload() == toorx::jllPollFrame(0));
}

void fallbackTests() {
    // ISSC notify missing: i-Running bike moves to service 0xFFF0.
    auto irun = source("trx-bike-irun");
    std::uint32_t now = 0;
    auto writes = runInit(*irun, now, 0x2);
    assert(irun->usingFallbackService() && std::strcmp(irun->activeVariant().id, "trx-bike-irun") == 0);
    assert(writes[0].service == toorx::kFff0Service && writes[0].characteristic == toorx::kFff2Write);
    Bytes bike = bikeFrame();
    irun->onNotify(0, bike.data(), bike.size(), now);
    assert(!irun->hasData());
    irun->onNotify(1, bike.data(), bike.size(), now);
    assert(irun->hasData() && near(irun->sample().cadence.value, 85));

    // notify2 missing: upstream switches IRUNNING to ICONSOLE (no init sleeps).
    now = 0;
    writes = runInit(*irun, now, 0x1);
    assert(!irun->usingFallbackService() && std::strcmp(irun->activeVariant().id, "trx-bike-icon") == 0);
    assert(std::strcmp(irun->id(), "trx-bike-irun") == 0 && writes[0].service == toorx::kIsscService);
    now = 0;
    runInit(*irun, now, 0x5);
    assert(std::strcmp(irun->activeVariant().id, "trx-bike-irun") == 0);
    irun->onNotify(2, bike.data(), bike.size(), now);
    assert(irun->hasData());

    auto chy = source("trx-bike-chy");
    now = 0;
    runInit(*chy, now, 0x1);
    assert(std::strcmp(chy->activeVariant().id, "trx-bike-icon") == 0);

    // Generic elliptical without 0xFFF0 becomes the JTX layout on ISSC.
    auto ell = source("trx-ell");
    now = 0;
    writes = runInit(*ell, now, 0x2);
    assert(ell->usingFallbackService() && ell->activeVariant().parse == Parse::EllipticalJtx);
    assert(writes[0].service == toorx::kIsscService && writes[0].characteristic == toorx::kIsscWrite);
    assert(writes[0].withResponse);
    assert(ell->profile() == MachineProfile::CrossTrainer);

    // DKN Motion on ISSC uses address 0x02 for resistance.
    auto dkn = source("trx-bike-dkn");
    now = 0;
    runInit(*dkn, now, 0x2);
    drain(*dkn);
    assert(dkn->control({Control::Resistance, 0.0F}, now));
    assert(drain(*dkn)[0].payload() == toorx::resistanceFrame(0x02, 0x01, 1));
    now = 0;
    runInit(*dkn, now, 0x1);
    assert(dkn->control({Control::Resistance, 0.0F}, now));
    assert(drain(*dkn)[0].payload() == toorx::resistanceFrame(0x01, 0x01, 1));
}

void treadmillTests() {
    auto tm = source("trx-tm");
    std::uint32_t now = 0;
    runInit(*tm, now, 0x1);
    drain(*tm);
    const auto caps = tm->capabilities();
    assert(caps.supports(Control::Incline) && !caps.supports(Control::Speed) && !caps.supports(Control::Resistance));
    // No telemetry yet: incline refused.
    assert(!tm->control({Control::Incline, 2.0F}, now));
    assert(!tm->control({Control::Speed, 8.0F}, now));

    // A 19-byte frame split over two notifications (20-byte MTU fragments).
    const Bytes frame = treadmillFrame(85, 3);
    tm->onNotify(0, frame.data(), 10, now);
    assert(!tm->hasData());
    tm->onNotify(0, frame.data() + 10, 9, now);
    assert(tm->hasData() && near(tm->sample().speed.value, 8.5F) && near(tm->sample().incline.value, 3));
    assert(!tm->sample().heartRate.valid);

    // Command replies are dropped and do not corrupt the next frame.
    const Bytes reply{0xf0, 0xbc, 0x01, 0xd3, 0x01, 0x92};
    tm->onNotify(0, reply.data(), reply.size(), now + 10);
    const Bytes faster = treadmillFrame(100, 4);
    tm->onNotify(0, faster.data(), faster.size(), now + 20);
    assert(near(tm->sample().speed.value, 10.0F) && near(tm->sample().incline.value, 4));
    // Oversized notifications are discarded.
    Bytes big = treadmillFrame(50, 0);
    big.push_back(0);
    tm->onNotify(0, big.data(), big.size(), now + 30);
    assert(near(tm->sample().speed.value, 10.0F));
    // "xx f0 b2" tail keeps the telemetry start.
    const Bytes tail{0x01, 0xf0, 0xb2};
    tm->onNotify(0, tail.data(), 1, now + 40);
    tm->onNotify(0, tail.data() + 1, 2, now + 40);
    const Bytes slow = treadmillFrame(30, 0);
    tm->onNotify(0, slow.data() + 2, slow.size() - 2, now + 50);
    assert(near(tm->sample().speed.value, 3.0F));

    assert(tm->control({Control::Incline, 5.7F}, now + 60));
    assert(drain(*tm)[0].payload() == Bytes({0xf0, 0xac, 0x01, 0xd3, 0x06, 0x64, 0x64, 0x3e}));
    assert(!tm->control({Control::Incline, 15.5F}, now + 60));
    assert(!tm->control({Control::Incline, -1.0F}, now + 60));
    assert(!tm->control({Control::Incline, std::numeric_limits<float>::quiet_NaN()}, now + 60));
    assert(!tm->control({Control::Incline, 2.0F}, now + 3000));  // stale data

    // Adidas uses its address bytes and write-with-response.
    auto adidas = source("trx-tm-adidas");
    now = 0;
    runInit(*adidas, now, 0x1);
    drain(*adidas);
    const Bytes af = treadmillFrame(60, 0);
    adidas->onNotify(0, af.data(), af.size(), now);
    assert(adidas->control({Control::Incline, 1.0F}, now));
    const auto w = drain(*adidas);
    assert(w[0].withResponse && w[0].payload() == toorx::inclineFrame(0x5b, 0xd3, 1.0F));

    // DKN Run: 18-byte frames on notify or notify2.
    auto dkn = source("trx-tm-dknrun");
    now = 0;
    runInit(*dkn, now, 0x3);
    const Bytes df = treadmillFrame(72, 1, 18);
    dkn->onNotify(1, df.data(), df.size(), now);
    assert(dkn->hasData() && near(dkn->sample().speed.value, 7.2F));

    // Flow Fitness: ACKs dropped, b0 telemetry reassembled.
    auto flow = source("trx-tm-flow");
    now = 0;
    runInit(*flow, now, 0x1);
    const Bytes flowAck{0xf0, 0xb0, 0x2e, 0xd3, 0xa1};
    const Bytes ff = treadmillFrame(64, 2, 19, 0xb0, 0x2e);
    flow->onNotify(0, flowAck.data(), flowAck.size(), now);
    flow->onNotify(0, ff.data(), 8, now);
    flow->onNotify(0, ff.data() + 8, 11, now);
    assert(flow->hasData() && near(flow->sample().speed.value, 6.4F));
    flow->onNotify(0, flowAck.data(), flowAck.size(), now);
    flow->onNotify(0, ff.data(), ff.size(), now);
    assert(near(flow->sample().incline.value, 2));
    assert(flow->profile() == MachineProfile::Treadmill);
}

void bikeTests() {
    auto bike = source("trx-bike-tc");
    std::uint32_t now = 0;
    assert(!bike->capabilities().supports(Control::Resistance));
    runInit(*bike, now, 0x1);
    drain(*bike);
    assert(bike->capabilities().supports(Control::Resistance) && !bike->capabilities().supports(Control::Incline));
    const Bytes f = bikeFrame();
    bike->onNotify(0, f.data(), f.size(), now);
    const auto& s = bike->sample();
    assert(near(s.power.value, 150) && near(s.cadence.value, 85) && near(s.heartRate.value, 140) &&
           near(s.resistance.value, 8) && near(s.speed.value, 25.3F));
    // A zero heart rate invalidates the previous one.
    Bytes noHr = f;
    put100(noHr, 14, 0);
    bike->onNotify(0, noHr.data(), noHr.size(), now);
    assert(!bike->sample().heartRate.valid);
    // Truncated frames ignored.
    Bytes cut = f;
    cut[9] = 50;
    bike->onNotify(0, cut.data(), 20, now);
    assert(near(bike->sample().cadence.value, 85));
    // Unsubscribed index ignored.
    bike->onNotify(3, cut.data(), cut.size(), now);
    assert(near(bike->sample().cadence.value, 85));

    assert(bike->control({Control::Resistance, 50.0F}, now));
    assert(drain(*bike)[0].payload() == toorx::resistanceFrame(0x01, 0x01, percentToLevel(50.0F, 1, 32)));
    assert(bike->control({Control::Resistance, 100.0F}, now));
    assert(drain(*bike)[0].payload() == toorx::resistanceFrame(0x01, 0x01, 32));
    assert(!bike->control({Control::Incline, 1.0F}, now));
    assert(bike->profile() == MachineProfile::IndoorBike);

    auto adc = source("trx-bike-adc1");
    now = 0;
    runInit(*adc, now, 0x1);
    drain(*adc);
    assert(adc->control({Control::Resistance, 0.0F}, now));
    assert(drain(*adc)[0].payload() == toorx::resistanceFrame(0x5b, 0x01, 1));

    auto row = source("trx-row");
    now = 0;
    runInit(*row, now, 0x1);
    drain(*row);
    assert(row->profile() == MachineProfile::Rower);
    assert(row->control({Control::Resistance, 100.0F}, now));
    const auto w = drain(*row);
    assert(w[0].withResponse && w[0].payload() == toorx::resistanceFrame(0x35, 0x01, 15));

    // reset forgets everything.
    row->reset();
    assert(!row->initialized() && !row->hasData() && !row->capabilities().supports(Control::Resistance));
}

ble::Advertisement advert(const std::string& name, std::uint8_t firstAddressByte = 0x11, bool ftms = false) {
    ble::Advertisement a;
    a.name = name;
    a.address.bytes = {firstAddressByte, 0x22, 0x33, 0x44, 0x55, 0x66};
    if (ftms) a.services.push_back(ble::Uuid::from16(0x1826));
    return a;
}

// Best toorx descriptor for an advertisement ("" when none claims it).
std::string matched(const ble::Advertisement& a) {
    const ProtocolInfo* best = nullptr;
    for (std::size_t i = 0; i < toorx::descriptorCount(); ++i) {
        const auto& info = toorx::descriptorAt(i);
        if (!info.matches(a)) continue;
        assert(!best);  // descriptors never overlap
        best = &info;
    }
    return best ? best->id : "";
}

void matcherTests() {
    assert(matched(advert("TOORX-1234")) == "trx-tm");
    assert(matched(advert("toorx-1234")).empty());  // upstream is case sensitive here
    assert(matched(advert("TOORX-1234", 0x11, true)).empty());
    assert(matched(advert("V-RUN 01")) == "trx-tm");
    assert(matched(advert("k80_1234")) == "trx-tm");
    assert(matched(advert("i-Running 1")) == "trx-tm-irun");
    assert(matched(advert("I-CONSOLE+1")) == "trx-tm-irun");
    assert(matched(advert("iConsole+ 9")) == "trx-tm-irun");
    assert(matched(advert("Reebok ZR8")) == "trx-tm-reebok");
    assert(matched(advert("ADIDAS T19")) == "trx-tm-adidas");
    assert(matched(advert("ADIDAS1234")) == "trx-bike-adc1");
    assert(matched(advert("adidas00001")) == "trx-bike-adc1");
    assert(matched(advert("ADIDAS123")).empty());
    assert(matched(advert("ADIDAS12X4")).empty());
    assert(matched(advert("DKN RUN")) == "trx-tm-dknrun");
    assert(matched(advert("DKN RUN 1234")).empty());
    assert(matched(advert("TC123")) == "trx-bike-tc");
    assert(matched(advert("tc123", 0xe8)) == "trx-bike-chy");
    assert(matched(advert("TC1234")).empty());
    assert(matched(advert("TCX23")).empty());
    assert(matched(advert("TUN 500")) == "trx-bike-tun");
    assert(matched(advert("FITHIWAY 1")) == "trx-bike-fhw");
    assert(matched(advert("Fit Hi Way", 0xe8)) == "trx-bike-fhw");
    assert(matched(advert("VIRTUFIT X")) == "trx-bike-vfit");
    assert(matched(advert("iBiking+ 2")) == "trx-bike-irun");
    assert(matched(advert("XX-RI009R-1")) == "trx-bike-irun");
    assert(matched(advert("IBIKING+", 0xe8)) == "trx-bike-chy");
    assert(matched(advert("PASYOU-1", 0xe8)) == "trx-bike-chy");
    assert(matched(advert("PASYOU-1")).empty());
    assert(matched(advert("IBIKING+", 0x11, true)).empty());
    assert(matched(advert("FAL-SPORTS 5")) == "trx-ell");
    assert(matched(advert("Unrelated")).empty());

    std::size_t automatic = 0;
    for (std::size_t i = 0; i < toorx::descriptorCount(); ++i) {
        const auto& info = toorx::descriptorAt(i);
        assert(std::strlen(info.id) <= 16 && std::strncmp(info.id, "trx-", 4) == 0);
        for (std::size_t j = 0; j < i; ++j) assert(std::strcmp(info.id, toorx::descriptorAt(j).id) != 0);
        assert(info.controllable && !info.advertisementOnly);
        assert(info.priority == 0 || info.priority == toorx::kMatchPriority);
        if (info.priority) ++automatic;
        auto created = info.create();
        assert(created && std::strcmp(created->id(), info.id) == 0 && created->profile() == info.profile);
        assert(created->startupTimeoutMs() == 30000);
    }
    assert(automatic == 13);
}

}  // namespace

void runToorxTests() {
    frameTests();
    decodeTests();
    planTests();
    initTests();
    fallbackTests();
    treadmillTests();
    bikeTests();
    matcherTests();
    assert(toorx::descriptorCount() == 36);
    std::printf("toorx host tests passed (%zu protocols)\n", toorx::descriptorCount());
}

// Needs the toorx descriptors registered in protocols/registry.cpp.
void runToorxRegistryTests() {
    for (std::size_t i = 0; i < toorx::descriptorCount(); ++i) {
        const auto* info = findProtocol(toorx::descriptorAt(i).id);
        assert(info && info->profile == toorx::descriptorAt(i).profile);
    }
    const auto detect = [](const ble::Advertisement& a) {
        const auto* info = detectProtocol(a);
        return std::string(info ? info->id : "");
    };
    assert(detect(advert("TOORX-1234")) == "trx-tm");
    assert(detect(advert("I-CONSOLE+1")) == "trx-tm-irun");
    assert(detect(advert("Reebok ZR8")) == "trx-tm-reebok");
    assert(detect(advert("DKN RUN")) == "trx-tm-dknrun");
    assert(detect(advert("TC123")) == "trx-bike-tc");
    assert(detect(advert("TC123", 0xe8)) == "trx-bike-chy");
    assert(detect(advert("iBiking+ 2")) == "trx-bike-irun");
    assert(detect(advert("ADIDAS1234")) == "trx-bike-adc1");
    assert(detect(advert("FAL-SPORTS 5")) == "trx-ell");
    assert(detect(advert("TOORX-1234", 0x11, true)) != "trx-tm");
    std::printf("toorx registry tests passed\n");
}
