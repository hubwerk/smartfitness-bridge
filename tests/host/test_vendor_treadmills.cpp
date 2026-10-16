// Host tests for the vendor treadmill ports (GPL-3.0 port of qdomyos-zwift,
// Copyright Roberto Viola and contributors, revision
// ea70d7ff64bc8bf508fc6401fd056211e05311b3; see
// include/core/protocols/vendor_treadmills.hpp for the upstream files).
// All device frames below are synthetic, built in the upstream layouts.

#include "protocols/vendor_treadmills.hpp"
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
using vendor_treadmills::Model;

bool near(float a, float b) { return std::fabs(a - b) < 0.01F; }

Bytes hexBytes(const char* text) {
    Bytes out;
    int pending = -1;
    for (; *text; ++text) {
        const char c = *text;
        if (c == ' ') continue;
        const int v = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        if (pending < 0) {
            pending = v;
        } else {
            out.push_back(static_cast<std::uint8_t>(pending << 4 | v));
            pending = -1;
        }
    }
    return out;
}

ble::Advertisement advertisement(const char* name, std::uint16_t service = 0) {
    ble::Advertisement a;
    a.name = name;
    if (service) a.services.push_back(ble::Uuid::from16(service));
    return a;
}

std::vector<ble::GattWrite> drain(SourceProtocol& source) {
    std::vector<ble::GattWrite> out;
    ble::GattWrite write;
    while (source.takeWrite(write)) out.push_back(write);
    return out;
}

std::vector<Bytes> payloads(SourceProtocol& source) {
    std::vector<Bytes> out;
    for (const auto& w : drain(source)) out.push_back(w.payload());
    return out;
}

void notify(SourceProtocol& source, const Bytes& frame, std::uint32_t now, std::size_t index = 0) {
    source.onNotify(index, frame.data(), frame.size(), now);
}

// Connects and runs the init script to completion, returning its writes.
std::vector<Bytes> connectAndInit(VendorTreadmillSource& source, std::uint32_t& now, std::uint32_t mask = 1) {
    assert(source.onConnected(mask, now));
    std::vector<Bytes> writes = payloads(source);
    for (int i = 0; i < 400 && !source.ready(); ++i) {
        now += 100;
        source.tick(now);
        for (auto& p : payloads(source)) writes.push_back(p);
    }
    assert(source.ready());
    return writes;
}

bool contains(const std::vector<Bytes>& writes, const Bytes& frame) {
    for (const auto& w : writes)
        if (w == frame) return true;
    return false;
}

ControlRequest speed(float kmh) { return {Control::Speed, kmh}; }
ControlRequest incline(float percent) { return {Control::Incline, percent}; }

std::vector<std::string> matchingIds(const ble::Advertisement& a) {
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < vendor_treadmills::descriptorCount(); ++i) {
        const auto& d = vendor_treadmills::descriptorAt(i);
        if (d.priority > 0 && d.matches(a)) ids.push_back(d.id);
    }
    return ids;
}

void expectMatch(const char* name, const char* id, std::uint16_t service = 0) {
    const auto ids = matchingIds(advertisement(name, service));
    if (!id) {
        assert(ids.empty());
    } else {
        assert(ids.size() == 1 && ids[0] == id);
    }
}

void codecTests() {
    const auto check = hexBytes("313233343536373839");
    assert(vendor_treadmills::crc8(check.data(), check.size()) == 0xF4);  // CRC-8 check value

    assert(vendor_treadmills::ziproPoll(false, 0, 0) == hexBytes("fb07a102000000aafc"));
    assert(vendor_treadmills::ziproPoll(true, 50, 3) == hexBytes("fb07a1020132 03 e0fc"));
    assert(vendor_treadmills::focusControl(30, 2) == hexBytes("f0c303031e02d9"));
    assert(vendor_treadmills::speraxSpeedFrame(1) == hexBytes("f50b001502010056f000fa"));
    assert(vendor_treadmills::speraxSpeedFrame(2) == hexBytes("f50a0015010200be98fa"));
    assert(vendor_treadmills::speraxSpeedFrame(60) == hexBytes("f50b0015013c00f006e2fa"));
    assert(vendor_treadmills::speraxSpeedFrame(0).empty() && vendor_treadmills::speraxSpeedFrame(61).empty());
    // Upstream template (1.0 km/h, incline 8) and its checksum.
    assert(vendor_treadmills::pitpatControl(1000, 8) == hexBytes("6a170000000003e80108640c000000000000167a679643"));
    const Bytes pit = vendor_treadmills::pitpatControl(5000, 3);
    assert(pit.size() == 23 && pit[6] == 0x13 && pit[7] == 0x88 && pit[9] == 3);
    std::uint8_t sum = 0;
    for (std::size_t i = 5; i <= 20; ++i) sum ^= pit[i];
    assert(pit[21] == static_cast<std::uint8_t>(sum ^ 0x17));
    assert(vendor_treadmills::activioSpeedFrame(50, false) == hexBytes("0332000000 5a 33"));
    assert(vendor_treadmills::activioSpeedFrame(50, true) == hexBytes("0332000000 5a 33"));
    assert(vendor_treadmills::activioSpeedFrame(51, true) == hexBytes("0333000000 5b 32"));
    assert(vendor_treadmills::activioSpeedFrame(48, true) == hexBytes("0330000000 58 35"));
    assert(vendor_treadmills::activioInclineFrame(5, false, 0) == hexBytes("04000500002e0b"));
    assert(vendor_treadmills::activioInclineFrame(5, true, 30) == hexBytes("041e0500004c21"));
    assert(vendor_treadmills::esangSpeedFrame(10.0F) == hexBytes("a901013e97"));
    assert(vendor_treadmills::costawaySpeedFrame(5.0F) == hexBytes("a9a0030232 00 3a"));
    const std::uint8_t r = 0x2f;
    assert(vendor_treadmills::esangHandshakeReply(r) == (Bytes{0xa9, 0x08, 0x01, 0x2f, vendor_treadmills::crc8(&r, 1)}));
    const std::uint8_t c0[4] = {3, 4, 7, 12};
    assert(vendor_treadmills::esangPairReply(6, 3, 4) == (Bytes{0xa9, 0x08, 0x04, 3, 4, 7, 12, vendor_treadmills::crc8(c0, 4)}));
    const Bytes pair5 = vendor_treadmills::esangPairReply(5, 3, 4);
    assert(pair5[3] == 12 && pair5[4] == 7 && pair5[5] == 3 && pair5[6] == 4);
    assert(vendor_treadmills::kickrRunSpeedFrame(3.6F, true) == hexBytes("0240420f00ff"));
    assert(vendor_treadmills::kickrRunSpeedFrame(3.6F, false) == hexBytes("0240420f000a"));
    assert(vendor_treadmills::activioDecode(0x7F) == 0x32);
    assert(vendor_treadmills::activioDecode(0xC1) == 0x78);
    assert(vendor_treadmills::activioDecode(0x59) == 0x10);
    assert(vendor_treadmills::activioIncline(0x49) == 0 && vendor_treadmills::activioIncline(0x5C) == 15);
    assert(vendor_treadmills::activioIncline(0x30) == 0x30);
}

void matcherTests() {
    expectMatch("TRUE TM30", "true-tm");
    expectMatch("True Treadmill", "true-tm");
    expectMatch("TRUE TREADMILL 400", nullptr);
    expectMatch("TRUE ELLIPTICAL 123", nullptr);
    expectMatch("TRUE 1000", nullptr);
    expectMatch("TREADMILL", "true-tm");
    expectMatch("TREADMILL", nullptr, 0x1814);
    expectMatch("TREADMILL", nullptr, 0x1826);
    expectMatch("TRUE TM30", nullptr, 0x1826);
    expectMatch("ASSAULT TREADMILL 01", "assault-tm");
    expectMatch("ASSAULTRUNNER", nullptr);
    expectMatch("WDWAY123", "wdway-tm");
    expectMatch("WDWAY1234", nullptr);
    expectMatch("LF1234567890ABC", "lifefitness-tm");
    expectMatch("LF1234567890ABCDEF", "lifefitness-tm");
    expectMatch("LF1234567890ABCD", nullptr);
    expectMatch("LF", nullptr);
    expectMatch("MYRUN 1234", "technogym-myrun");
    expectMatch("MERACH-U3", "technogym-myrun", 0x1826);
    expectMatch("LIFESPAN-TR", "lifespan-tm");
    expectMatch("ZR7-1234", "octane-zr7");
    expectMatch("ZR8-1234", "octane-zr8");
    expectMatch("RZ_TREADMILL", "zipro-tm");
    expectMatch("ESANGLINKER", "esanglinker");
    expectMatch("ESLINKER", nullptr);  // upstream default type (Cadenza) not ported
    expectMatch("ZW-A5", "shuaa5-tm");
    expectMatch("ZW-A5", nullptr, 0x1826);
    expectMatch("SPERAX_RM01", "sperax-tm");
    expectMatch("SPERAX_RM-02", "sperax-tm");
    expectMatch("SPERAX_RM-01", nullptr);
    expectMatch("EW-TM-1234", "focus-tm");
    expectMatch("PITPAT-T01", "deerrun-pitpat");
    expectMatch("PITPAT-T01", nullptr, 0x1826);
    expectMatch("RUNNERT", "activio-tm");
    expectMatch("KICKR RUN 1234", "kickr-run", 0x1826);
    // Existing ports keep their devices.
    for (const char* name : {"F80", "HORIZON 7.0AT", "KS-H", "BOWFLEX T216", "I_TL", "XT485", "RE", "WALKINGPAD",
                             "SCHWINN 170/270", "KICKR SNAP", "PARAGON X", "WAHOO KICKR", "F15"})
        expectMatch(name, nullptr);
}

void descriptorTests() {
    assert(vendor_treadmills::descriptorCount() == 20);
    for (std::size_t i = 0; i < vendor_treadmills::descriptorCount(); ++i) {
        const auto& d = vendor_treadmills::descriptorAt(i);
        const std::string id = d.id;
        assert(!id.empty() && id.size() <= 16);
        for (char c : id) assert((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
        for (std::size_t j = 0; j < i; ++j) assert(id != vendor_treadmills::descriptorAt(j).id);
        assert(d.profile == MachineProfile::Treadmill && !d.advertisementOnly);
        auto source = d.create();
        assert(source && id == source->id() && source->profile() == MachineProfile::Treadmill);
        assert(source->capabilities().controls == 0);
        if (d.priority == 0) assert(!d.matches(advertisement(d.id)));
        else assert(d.priority == vendor_treadmills::kMatchPriority);
    }
}

void trueTests() {
    VendorTreadmillSource source(Model::True);
    const auto plan = source.plan();
    assert(plan.subscriptionCount == 1 && !plan.discoverNotifications);
    assert(!source.onConnected(0, 0) && source.failed());
    source.reset();
    std::uint32_t now = 0;
    assert(connectAndInit(source, now).empty());
    Bytes frame(16, 0);
    frame[6] = 0x52;  // 850 -> 8.50 km/h
    frame[7] = 0x03;
    frame[14] = 20;
    notify(source, Bytes(frame.begin(), frame.begin() + 15), now);  // truncated
    assert(!source.hasData());
    notify(source, frame, now);
    assert(near(source.sample().speed.value, 8.5F) && near(source.sample().incline.value, 2.0F));
    assert(!source.control(speed(5.0F), now) && source.capabilities().controls == 0);
    Bytes incline4{0, 0, 0xF6, 0};
    notify(source, incline4, now);
    assert(near(source.sample().incline.value, -1.0F));

    VendorTreadmillSource assault(Model::Assault);
    now = 0;
    connectAndInit(assault, now);
    Bytes a(16, 0);
    a[5] = 0xe8;
    a[6] = 0x03;
    notify(assault, a, now);
    assert(near(assault.sample().speed.value, 10.0F) && !assault.sample().incline.valid);

    VendorTreadmillSource wdway(Model::Wdway);
    now = 0;
    connectAndInit(wdway, now);
    Bytes w(18, 0);
    w[6] = 0xf4;
    w[7] = 0x01;
    w[16] = 15;
    notify(wdway, w, now);
    assert(near(wdway.sample().speed.value, 5.0F) && near(wdway.sample().incline.value, 1.5F));
}

void ziproTests() {
    VendorTreadmillSource source(Model::Zipro);
    std::uint32_t now = 0;
    const auto init = connectAndInit(source, now);
    assert(init.size() == 7 && init[0] == hexBytes("fb05a00001a6fc") && init[6] == hexBytes("fb05a10001a7fc"));
    now += 100;
    source.tick(now);
    assert(payloads(source) == std::vector<Bytes>{hexBytes("fb07a102000000aafc")});
    // Not running and no data: rejected.
    assert(!source.control(speed(5.0F), now));
    Bytes frame(18, 0);
    frame[5] = 0;  // belt stopped
    notify(source, frame, now);
    assert(near(source.sample().speed.value, 0.0F) && !source.sample().heartRate.valid);
    assert(!source.control(speed(5.0F), now) && !source.control(speed(0.0F), now));
    assert(!source.control(incline(2.0F), now));
    frame[5] = 50;
    frame[6] = 2;
    frame[15] = 120;
    notify(source, frame, now);
    assert(near(source.sample().speed.value, 5.0F) && near(source.sample().incline.value, 2.0F));
    assert(near(source.sample().heartRate.value, 120.0F));
    assert(source.capabilities().supports(Control::Speed) && source.capabilities().supports(Control::Incline));
    assert(source.control(speed(6.0F), now));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::ziproPoll(true, 60, 2)});
    assert(source.control(incline(4.0F), now));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::ziproPoll(true, 50, 4)});
    assert(!source.control(speed(25.0F), now) && !source.control(incline(16.0F), now));
    assert(!source.control(speed(-1.0F), now) && !source.control(speed(NAN), now));
    assert(source.control(speed(0.0F), now));
    const auto stop = payloads(source);
    assert(stop.size() == 3 && stop[0] == hexBytes("fb05a20401acfc"));
    // Stale data disables controls and nothing is sent.
    assert(!source.control(speed(5.0F), now + 2500) && drain(source).empty());
    source.reset();
    assert(!source.control(speed(0.0F), now) && drain(source).empty());
    source.tick(now + 1000);
    assert(drain(source).empty());
}

void lifespanTests() {
    VendorTreadmillSource source(Model::Lifespan);
    std::uint32_t now = 0;
    const auto init = connectAndInit(source, now);
    assert(init == (std::vector<Bytes>{hexBytes("e9ff000000"), hexBytes("e400f40000")}));
    source.tick(now);
    assert(payloads(source) == std::vector<Bytes>{hexBytes("a182000000")});
    notify(source, Bytes{0xa1, 0x82, 5, 25, 0}, now);
    assert(near(source.sample().speed.value, 5.25F));
    now += 500;
    source.tick(now);
    assert(payloads(source) == std::vector<Bytes>{hexBytes("a188000000")});
    notify(source, Bytes{0xa1, 0x88, 0x01, 0x02, 0}, now);
    assert(near(source.sample().stepCount.value, 258.0F));
    assert(!source.control(speed(3.0F), now) && source.capabilities().controls == 0);
}

void focusTests() {
    VendorTreadmillSource source(Model::Focus);
    std::uint32_t now = 0;
    const auto init = connectAndInit(source, now);
    assert(init.size() == 15 && init[0] == hexBytes("f0c000b0") && init[7] == hexBytes("f0c405003a01af4df0"));
    drain(source);
    Bytes frame(20, 0);
    frame[10] = 40;
    frame[11] = 3;
    notify(source, frame, now);
    assert(near(source.sample().speed.value, 4.0F) && near(source.sample().incline.value, 3.0F));
    assert(source.control(speed(5.0F), now));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::focusControl(50, 3)});
    assert(source.control(incline(6.0F), now));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::focusControl(40, 6)});
    assert(source.control(speed(0.0F), now));
    const auto stop = payloads(source);
    assert(stop.size() == 2 && stop[0] == hexBytes("f0c303020000b8"));
    for (const auto& w : init) assert(w != hexBytes("f0c303010000b7"));  // never the start frame
}

void speraxTests() {
    VendorTreadmillSource source(Model::Sperax);
    std::uint32_t now = 0;
    const auto writes = connectAndInit(source, now);
    assert(writes.size() == 3 && writes[2] == hexBytes("f50900130100 89b8fa"));
    source.tick(now);
    const auto poll = drain(source);
    assert(poll.size() == 1 && poll[0].payload() == hexBytes("f5080019f00a59fa") && !poll[0].withResponse);
    Bytes frame(24, 0);
    frame[15] = 0x01;
    frame[16] = 0x02;
    frame[17] = 30;
    notify(source, frame, now);
    assert(near(source.sample().speed.value, 3.0F) && near(source.sample().stepCount.value, 258.0F));
    assert(!source.sample().incline.valid);
    assert(source.control(speed(4.5F), now));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::speraxSpeedFrame(45)});
    assert(!source.control(speed(6.5F), now) && !source.control(incline(1.0F), now));
    assert(source.control(speed(0.0F), now));
    assert(payloads(source) == std::vector<Bytes>{hexBytes("f50a0015000000d301fa")});
}

void pitpatTests() {
    VendorTreadmillSource source(Model::PitPat);
    const auto plan = source.plan();
    assert(plan.subscriptionCount == 1 && plan.subscriptions[0].characteristic == ble::Uuid::from16(0xFBA2));
    std::uint32_t now = 0;
    assert(source.onConnected(1, now));
    std::vector<ble::GattWrite> init = drain(source);
    while (!source.ready()) {
        now += 100;
        source.tick(now);
        for (auto& w : drain(source)) init.push_back(w);
    }
    assert(init.size() == 3);
    assert(init[1].payload() == hexBytes("6b059d9843") && init[1].service == ble::Uuid::from16(0x1801) &&
           init[1].characteristic == ble::Uuid::from16(0x2B2A));
    assert(init[2].characteristic == ble::Uuid::from16(0xFBA1));
    Bytes frame(50, 0);
    frame[3] = 0x13;
    frame[4] = 0x88;
    frame[11] = 3;
    notify(source, Bytes(frame.begin(), frame.begin() + 49), now);
    assert(!source.hasData());
    notify(source, frame, now);
    assert(near(source.sample().speed.value, 5.0F) && near(source.sample().incline.value, 3.0F));
    source.tick(now);
    assert(payloads(source) == std::vector<Bytes>{hexBytes("6a05fdf843")});
    assert(source.control(speed(6.0F), now));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::pitpatControl(6000, 3)});
    assert(source.control(incline(5.0F), now));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::pitpatControl(5000, 5)});
    assert(source.control(speed(0.0F), now));
    assert(payloads(source) == std::vector<Bytes>{hexBytes("6a1700000000000005008a00020000000000122e0caa43")});
}

void activioTests() {
    VendorTreadmillSource source(Model::Activio);
    std::uint32_t now = 0;
    assert(source.onConnected(1, now));
    const auto init = drain(source);
    assert(init.size() == 1 && init[0].payload() == Bytes{0x88} &&
           init[0].characteristic == ble::Uuid::fromString("e54eaa55-371b-476c-99a3-74d267e3edae"));
    now += 300;
    source.tick(now);
    assert(source.ready());
    Bytes frame(12, 0x40);
    frame[1] = 0x7F;  // 5.0 km/h
    frame[8] = 0xC1;  // 120 bpm
    frame[9] = 0x50;  // 3 %
    notify(source, Bytes(frame.begin(), frame.begin() + 11), now);
    assert(!source.hasData());
    notify(source, frame, now);
    assert(near(source.sample().speed.value, 5.0F) && near(source.sample().heartRate.value, 120.0F));
    assert(near(source.sample().incline.value, 3.0F));
    assert(source.control(speed(6.0F), now));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::activioSpeedFrame(60, false)});
    assert(source.control(incline(4.0F), now));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::activioInclineFrame(4, false, 0)});
    assert(source.control(speed(0.0F), now));
    assert(payloads(source) == std::vector<Bytes>{hexBytes("02000000002704")});

    VendorTreadmillSource v460(Model::ActivioV460);
    now = 0;
    connectAndInit(v460, now);
    notify(v460, frame, now);
    assert(!v460.sample().incline.valid && !v460.capabilities().supports(Control::Incline));
    assert(!v460.control(incline(2.0F), now));
    assert(v460.control(speed(6.0F), now));
    assert(payloads(v460) == std::vector<Bytes>{vendor_treadmills::activioSpeedFrame(60, true)});

    VendorTreadmillSource umay(Model::ActivioUmay);
    now = 0;
    connectAndInit(umay, now);
    notify(umay, frame, now);
    assert(umay.control(incline(5.0F), now));
    assert(payloads(umay) == std::vector<Bytes>{vendor_treadmills::activioInclineFrame(5, true, 50)});
}

void esangTests() {
    VendorTreadmillSource source(Model::EsangLinker);
    std::uint32_t now = 0;
    assert(source.onConnected(1, now));
    std::vector<Bytes> writes = payloads(source);
    bool handshake = false, pair = false;
    for (int i = 0; i < 300 && !source.ready(); ++i) {
        now += 100;
        if (now == 1500) {
            notify(source, hexBytes("a90801ff5f"), now);
            handshake = true;
        }
        if (now == 4000) {
            notify(source, hexBytes("a908040b"), now);
            pair = true;
        }
        source.tick(now);
        for (auto& p : payloads(source)) writes.push_back(p);
    }
    assert(handshake && pair && source.ready());
    assert(!contains(writes, hexBytes("a9a301010a")));  // start tape never sent
    assert(writes[0] == hexBytes("a9f2012f75") && writes.back() == hexBytes("a9ae01fef8"));
    std::size_t replies = 0;
    for (const auto& w : writes) {
        if (w.size() == 5 && w[1] == 0x08 && w[2] == 0x01) {
            assert(w[4] == vendor_treadmills::crc8(&w[3], 1));
            ++replies;
        }
        if (w.size() == 8 && w[1] == 0x08 && w[2] == 0x04) {
            // selector 0x0b % 6 == 5: n1*n2, n1+n2, n1, n2
            assert(w[3] == static_cast<std::uint8_t>(w[5] * w[6]) && w[4] == w[5] + w[6]);
            assert(w[5] >= 1 && w[5] <= 15 && w[6] >= 1 && w[6] <= 15);
            assert(w[7] == vendor_treadmills::crc8(&w[3], 4));
            ++replies;
        }
    }
    assert(replies == 2);
    notify(source, Bytes{0xa9, 0x10, 0, 31, 0}, now);  // not an e0 frame
    assert(!source.hasData());
    notify(source, Bytes{0xa9, 0xe0, 0, 31, 0}, now);
    assert(near(source.sample().speed.value, 3.1F * 1.60934F));
    assert(source.control(speed(10.0F), now));
    assert(payloads(source) == std::vector<Bytes>{hexBytes("a901013e97")});
    assert(!source.control(incline(2.0F), now) && !source.capabilities().supports(Control::Incline));
    assert(source.control(speed(0.0F), now));
    assert(payloads(source) == std::vector<Bytes>{hexBytes("a9a301000b")});

    VendorTreadmillSource costa(Model::Costaway);
    now = 0;
    assert(connectAndInit(costa, now).size() == 8);
    notify(costa, Bytes{0xa9, 0x00, 0, 0xFF, 0}, now);
    assert(near(costa.sample().speed.value, 0.0F) && !costa.control(speed(4.0F), now));
    notify(costa, Bytes{0xa9, 0x00, 0, 20, 0}, now);
    assert(costa.control(speed(5.0F), now));
    assert(payloads(costa) == std::vector<Bytes>{vendor_treadmills::costawaySpeedFrame(5.0F)});
}

void octaneTests() {
    VendorTreadmillSource zr7(Model::OctaneZr7);
    std::uint32_t now = 0;
    assert(connectAndInit(zr7, now) == (std::vector<Bytes>{hexBytes("a504210501d9"), hexBytes("a5042170016e")}));
    Bytes frame(20, 0x11);
    frame[5] = 0x02;
    frame[6] = 0x23;
    frame[7] = 0x68;  // 360 s/km -> 10 km/h
    frame[8] = 0x01;
    notify(zr7, Bytes(frame.begin(), frame.begin() + 19), now);
    assert(!zr7.hasData());
    Bytes ignored = frame;
    ignored[0] = 0xa5;
    ignored[1] = 0x17;
    notify(zr7, ignored, now);
    assert(!zr7.hasData());
    notify(zr7, frame, now);
    assert(near(zr7.sample().speed.value, 10.0F) && zr7.capabilities().controls == 0);

    VendorTreadmillSource zr8(Model::OctaneZr8);
    now = 0;
    connectAndInit(zr8, now);
    Bytes packet(32, 0x11);
    packet[0] = 0xa5;
    packet[1] = 0x20;
    packet[2] = 0x06;
    packet[10] = 0x3A;
    packet[11] = 80;
    packet[20] = 0x02;
    packet[21] = 0x23;
    packet[22] = 0xb4;  // 180 s/km -> 20 km/h
    packet[23] = 0x00;
    notify(zr8, frame, now);  // 20-byte non-a5 frame rejected on ZR8
    assert(!zr8.hasData());
    notify(zr8, Bytes(packet.begin(), packet.begin() + 20), now);
    assert(!zr8.hasData());
    notify(zr8, Bytes(packet.begin() + 20, packet.end()), now);
    assert(near(zr8.sample().speed.value, 20.0F) && near(zr8.sample().cadence.value, 80.0F));
    packet[11] = 0;  // explicit zero cadence stops, then the pace field applies again
    packet[20] = 0x00;
    packet[21] = 0x00;
    notify(zr8, packet, now + 100);
    assert(near(zr8.sample().speed.value, 0.0F) && near(zr8.sample().cadence.value, 0.0F));
}

void technogymTests() {
    VendorTreadmillSource source(Model::TechnogymMyRun);
    const auto plan = source.plan();
    assert(plan.subscriptionCount == 3 && plan.subscriptions[1].indicate);
    std::uint32_t now = 0;
    assert(source.onConnected(1, now));
    const auto init = drain(source);
    assert(init.size() == 1 && init[0].payload() == hexBytes("0093f051e81b42928e") &&
           init[0].characteristic == ble::Uuid::from16(0x2AD9));
    now += 300;
    source.tick(now);
    assert(source.ready());
    notify(source, hexBytes("0800e80314000000"), now);  // FTMS: 10.00 km/h, 2.0 %
    assert(near(source.sample().speed.value, 10.0F) && near(source.sample().incline.value, 2.0F));
    notify(source, Bytes{0x00, 0x00, 0x00, 85}, now, 2);  // RSC cadence byte 3
    assert(near(source.sample().cadence.value, 85.0F));
    assert(source.control(speed(8.2F), now));
    assert(payloads(source) == std::vector<Bytes>{hexBytes("023403")});
    assert(source.control(incline(3.5F), now));
    assert(payloads(source) == std::vector<Bytes>{hexBytes("032300")});
    assert(!source.control(speed(23.0F), now));
    assert(source.control(speed(0.0F), now));
    assert(payloads(source) == std::vector<Bytes>{hexBytes("0801")});
}

void kickrTests() {
    const ble::Uuid command = ble::Uuid::fromString("a026e03e-0a7d-4ab3-97fa-f1500f9feb8b");
    const ble::Uuid service = ble::Uuid::fromString("a026ee0d-0a7d-4ab3-97fa-f1500f9feb8b");
    VendorTreadmillSource missing(Model::KickrRun);
    assert(missing.plan().discoverNotifications && missing.plan().discoveryWriteCharacteristics[0] == command);
    assert(!missing.onConnected(3, 0) && missing.failed());

    VendorTreadmillSource source(Model::KickrRun);
    source.onWriteEndpoint(service, command);
    std::uint32_t now = 0;
    assert(source.onConnected(3, now));
    std::vector<ble::GattWrite> init = drain(source);
    while (!source.ready()) {
        now += 100;
        source.tick(now);
        for (auto& w : drain(source)) init.push_back(w);
    }
    assert(init.size() == 7 && init[0].payload() == Bytes{0x01} && init[6].payload() == Bytes{0xf0});
    assert(init[0].service == service && init[0].characteristic == command && !init[0].withResponse);
    notify(source, hexBytes("fe860111223344"), now, 4);
    assert(payloads(source) == std::vector<Bytes>{hexBytes("8711223344")});
    notify(source, hexBytes("fe860155667788"), now, 4);
    assert(drain(source).empty());
    notify(source, hexBytes("ff0140420f"), now, 4);
    assert(near(source.sample().speed.value, 3.6F));
    notify(source, hexBytes("0800e8031e000000"), now, 1);  // FTMS treadmill data: incline 3.0 %
    assert(near(source.sample().incline.value, 3.0F) && near(source.sample().speed.value, 3.6F));
    assert(source.control(speed(5.0F), now));
    const auto writes = payloads(source);
    assert(writes.size() == 2 && writes[0] == hexBytes("1900") &&
           writes[1] == vendor_treadmills::kickrRunSpeedFrame(5.0F, true));
    // Telemetry is ignored until the paddle confirms the new target.
    notify(source, hexBytes("ff0180841e"), now + 100, 4);
    assert(near(source.sample().speed.value, 3.6F) && source.lastDataMs() == now);
    notify(source, hexBytes("fde001abcd"), now + 200, 4);
    assert(payloads(source) == std::vector<Bytes>{hexBytes("e0abcd")});
    notify(source, hexBytes("ff0180841e"), now + 300, 4);
    assert(near(source.sample().speed.value, 7.2F));
    assert(source.control(incline(-2.0F), now + 300));
    const auto inclineWrites = drain(source);
    assert(inclineWrites.size() == 2 && inclineWrites[0].payload() == Bytes{0x00} &&
           inclineWrites[1].payload() == hexBytes("03ecff") &&
           inclineWrites[1].characteristic == ble::Uuid::from16(0x2AD9));
    for (const auto& w : inclineWrites) assert(w.payload() != Bytes{0x07});  // never FTMS Start/Resume
    assert(source.control(speed(0.0F), now + 300));
    assert(payloads(source) == std::vector<Bytes>{vendor_treadmills::kickrRunSpeedFrame(0.0F, false)});
}

void ftmsVendorTests() {
    for (const char* id : {"lifefitness-tm", "shuaa5-tm"}) {
        const ProtocolInfo* info = nullptr;
        for (std::size_t i = 0; i < vendor_treadmills::descriptorCount(); ++i)
            if (std::string(vendor_treadmills::descriptorAt(i).id) == id) info = &vendor_treadmills::descriptorAt(i);
        assert(info);
        auto source = info->create();
        assert(source->profile() == MachineProfile::Treadmill);
        assert(!source->control(speed(5.0F), 0) && drain(*source).empty());
    }
}

}  // namespace

void runVendorTreadmillsTests() {
    codecTests();
    matcherTests();
    descriptorTests();
    trueTests();
    ziproTests();
    lifespanTests();
    focusTests();
    speraxTests();
    pitpatTests();
    activioTests();
    esangTests();
    octaneTests();
    technogymTests();
    kickrTests();
    ftmsVendorTests();
    std::printf("vendor_treadmills host tests passed (%zu protocols)\n", vendor_treadmills::descriptorCount());
}

// Requires registry integration; intentionally not called by runVendorTreadmillsTests().
void runVendorTreadmillsRegistryTests() {
    struct Case {
        const char* name;
        std::uint16_t service;
        const char* id;
    };
    const Case cases[] = {{"TRUE TM30", 0, "true-tm"},          {"ASSAULT TREADMILL 1", 0, "assault-tm"},
                          {"WDWAY123", 0, "wdway-tm"},          {"LF1234567890ABC", 0x1826, "lifefitness-tm"},
                          {"MYRUN 123", 0x1826, "technogym-myrun"}, {"LIFESPAN-TR", 0, "lifespan-tm"},
                          {"ZR7-1", 0, "octane-zr7"},           {"ZR8-1", 0, "octane-zr8"},
                          {"RZ_TREADMILL", 0, "zipro-tm"},      {"ESANGLINKER", 0, "esanglinker"},
                          {"ZW-A5", 0, "shuaa5-tm"},            {"SPERAX_RM01", 0, "sperax-tm"},
                          {"EW-TM-1", 0, "focus-tm"},           {"PITPAT-T01", 0, "deerrun-pitpat"},
                          {"RUNNERT", 0, "activio-tm"},         {"KICKR RUN", 0x1826, "kickr-run"}};
    for (const auto& c : cases) {
        const auto* info = detectProtocol(advertisement(c.name, c.service));
        assert(info && std::string(info->id) == c.id);
    }
    // Existing ports keep their devices.
    const auto* bowflex = detectProtocol(advertisement("BOWFLEX T216"));
    assert(bowflex && std::string(bowflex->id) == "bowflex-t216");
    const auto* ftms = detectProtocol(advertisement("ZW-A5", 0x1826));
    assert(ftms && std::string(ftms->id) == "ftms");
    for (std::size_t i = 0; i < vendor_treadmills::descriptorCount(); ++i) {
        const auto& d = vendor_treadmills::descriptorAt(i);
        const auto* info = findProtocol(d.id);
        assert(info && info->profile == d.profile);
        auto created = createProtocol(d.id);
        assert(created && std::string(created->id()) == d.id);
    }
    std::printf("vendor_treadmills registry tests passed\n");
}
