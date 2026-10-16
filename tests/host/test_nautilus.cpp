// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3 (b1824db5379891d571a7ef2e219d20e6c7ec2c09
// for the SCHWINN T/570T names and 688a15a0 service):
// src/devices/{nautilusbike,nautiluselliptical,nautilustreadmill,bowflextreadmill}/, src/devices/bluetooth.cpp.
// All frames below are SYNTHETIC vectors in the upstream parser layouts, not hardware captures.

#include "protocols/nautilus.hpp"
#include "protocols/registry.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace smartfitness;
using namespace smartfitness::protocols;
using ble::Bytes;
using nautilus::Layout;

bool near(float a, float b) { return std::fabs(a - b) < 0.01F; }

ble::Advertisement advertisement(const std::string& name, bool ftms = false) {
    ble::Advertisement a;
    a.name = name;
    if (ftms) a.services.push_back(ble::Uuid::from16(0x1826));
    return a;
}

std::size_t indexOf(const char* id) {
    for (std::size_t i = 0; i < nautilus::descriptorCount(); ++i)
        if (std::string(nautilus::descriptorAt(i).id) == id) return i;
    assert(false);
    return 0;
}

std::unique_ptr<SourceProtocol> make(const char* id) { return nautilus::descriptorAt(indexOf(id)).create(); }

void notify(SourceProtocol& source, std::size_t index, const Bytes& frame, std::uint32_t now) {
    source.onNotify(index, frame.data(), frame.size(), now);
}

Bytes sized(std::size_t length) { return Bytes(length, 0); }

void codecTests() {
    assert(nautilus::initFrame() == Bytes({0x07, 0x01, 0xd3, 0x00, 0x1f, 0x05, 0x01}));
    nautilus::Reading r;

    // nautilusbike 14-byte data: cadence b1, speed LE b3-4 (0.01 mph), watts LE b6-7 (0.01 W).
    Bytes bike = sized(14);
    bike[1] = 80;
    bike[3] = 0xe8; bike[4] = 0x03;  // 1000 -> 10.00 mph
    bike[6] = 0x10; bike[7] = 0x27;  // 10000 -> 100 W
    assert(nautilus::decode({Layout::BikeMiles, false, false}, bike.data(), bike.size(), r));
    assert(near(r.speed.value, 16.0934F) && near(r.power.value, 100) && r.cadence.value == 80);
    assert(!r.heartRate.valid && !r.resistance.valid && !r.incline.valid);
    assert(nautilus::decode({Layout::BikeB616, false, false}, bike.data(), bike.size(), r));
    assert(near(r.speed.value, 10.0F));
    bike[1] = 0x90;  // negative signed cadence upstream: dropped
    assert(nautilus::decode({Layout::BikeMiles, false, false}, bike.data(), bike.size(), r) && !r.cadence.valid);
    // 20-byte status: HR b16 (nonzero), resistance b18.
    Bytes status = sized(20);
    status[16] = 120; status[18] = 12;
    assert(nautilus::decode({Layout::BikeMiles, false, false}, status.data(), status.size(), r));
    assert(r.heartRate.value == 120 && r.resistance.value == 12 && !r.speed.valid);
    status[16] = 0;
    assert(nautilus::decode({Layout::BikeMiles, false, false}, status.data(), status.size(), r) && !r.heartRate.valid);
    // Truncated / wrong lengths ignored.
    for (std::size_t len : {0U, 1U, 12U, 13U, 15U, 19U, 21U}) {
        Bytes f = sized(len);
        assert(!nautilus::decode({Layout::BikeMiles, false, false}, f.data(), f.size(), r));
    }
    assert(!nautilus::decode({Layout::BikeMiles, false, false}, nullptr, 14, r));

    // nautiluselliptical v0 (14 bytes): speed LE b3-4 /100 km/h, cadence b5.
    Bytes ell = sized(14);
    ell[3] = 0xd0; ell[4] = 0x07;  // 20.00
    ell[5] = 60; ell[1] = 33;
    assert(nautilus::decode({Layout::EllipticalV0, false, false}, ell.data(), ell.size(), r));
    assert(near(r.speed.value, 20.0F) && r.cadence.value == 60 && !r.power.valid);
    // v2 (14 bytes): mph and cadence b1.
    assert(nautilus::decode({Layout::EllipticalV2, false, false}, ell.data(), ell.size(), r));
    assert(near(r.speed.value, 32.1868F) && r.cadence.value == 33);
    // v1 needs 12 bytes: speed LE b7-8 /10.
    assert(!nautilus::decode({Layout::EllipticalV1, false, false}, ell.data(), ell.size(), r));
    Bytes ell12 = sized(12);
    ell12[7] = 0x7b; ell12[8] = 0x00; ell12[5] = 55;  // 12.3 km/h
    assert(nautilus::decode({Layout::EllipticalV1, false, false}, ell12.data(), ell12.size(), r));
    assert(near(r.speed.value, 12.3F) && r.cadence.value == 55);
    assert(!nautilus::decode({Layout::EllipticalV0, false, false}, ell12.data(), ell12.size(), r));
    // 20-byte: HR b16, resistance b18 (non-M7).
    status = sized(20);
    status[16] = 99; status[17] = 98; status[18] = 7; status[19] = 9;
    assert(nautilus::decode({Layout::EllipticalV1, false, false}, status.data(), status.size(), r));
    assert(r.heartRate.value == 99 && r.resistance.value == 7);
    // M7 on a v0 service: HR from b16 suppressed, resistance b18.
    assert(nautilus::decode({Layout::EllipticalV0, true, false}, status.data(), status.size(), r));
    assert(!r.heartRate.valid && r.resistance.value == 7);
    // M7 split frames on v1: 0x31 status, 0x5e data, other types ignored.
    status[1] = 0x31;
    assert(nautilus::decode({Layout::EllipticalV1, true, false}, status.data(), status.size(), r));
    assert(r.heartRate.value == 98 && r.resistance.value == 9 && !r.speed.valid);
    Bytes m7 = sized(20);
    m7[1] = 0x5e;
    m7[2] = 0x4c; m7[3] = 0x04;  // 1100 -> 11.00 km/h
    m7[5] = 70;
    m7[7] = 0xa8; m7[8] = 0x61;  // 25000 -> 250 W
    assert(nautilus::decode({Layout::EllipticalV1, true, false}, m7.data(), m7.size(), r));
    assert(near(r.speed.value, 11.0F) && near(r.power.value, 250.0F) && r.cadence.value == 70);
    assert(!r.heartRate.valid && !r.resistance.valid);
    m7[1] = 0x10;
    assert(!nautilus::decode({Layout::EllipticalV1, true, false}, m7.data(), m7.size(), r));

    // nautilustreadmill (12 bytes): speed LE b3-4 * 1.60934 / 100, incline b6.
    Bytes tm = sized(12);
    tm[3] = 0xf4; tm[4] = 0x01;  // 500 -> 5 mph
    tm[6] = 4;
    assert(nautilus::decode({Layout::Treadmill, false, false}, tm.data(), tm.size(), r));
    assert(near(r.speed.value, 8.0467F) && r.incline.value == 4);
    assert(nautilus::decode({Layout::Treadmill, false, true}, tm.data(), tm.size(), r));
    assert(near(r.speed.value, 8.0467F) && !r.incline.valid);  // T628: incline not reported
    tm[6] = 0xff;
    assert(nautilus::decode({Layout::Treadmill, false, false}, tm.data(), tm.size(), r) && !r.incline.valid);
    for (std::size_t len : {0U, 11U, 13U, 14U, 20U}) {
        Bytes f = sized(len);
        assert(!nautilus::decode({Layout::Treadmill, false, false}, f.data(), f.size(), r));
    }

    // bowflextreadmill (17 bytes): speed b14/10, incline b11, kcal (b7<<8)|b8.
    Bytes bow = sized(17);
    bow[14] = 65; bow[11] = 3; bow[7] = 0x01; bow[8] = 0x02;
    assert(nautilus::decode({Layout::Bowflex, false, false}, bow.data(), bow.size(), r));
    assert(near(r.speed.value, 6.5F) && r.incline.value == 3 && r.totalEnergy.value == 258);
    bow[8] = 0x80;
    assert(nautilus::decode({Layout::Bowflex, false, false}, bow.data(), bow.size(), r) && !r.totalEnergy.valid);
    Bytes bow16 = sized(16);
    assert(!nautilus::decode({Layout::Bowflex, false, false}, bow16.data(), bow16.size(), r));
}

void sourceTests() {
    const auto writeUuid = ble::Uuid::fromString("1717b3c0-9803-11e3-90e1-0002a5d5c51b");
    // Bike: three services x two notifications; second service (B616) selected.
    auto bike = make("nautilus-bike");
    auto plan = bike->plan();
    assert(plan.subscriptionCount == 6 && !plan.discoverNotifications);
    assert(plan.subscriptions[0].characteristic == ble::Uuid::fromString("a46a4a80-9803-11e3-8f3c-0002a5d5c51b"));
    assert(plan.subscriptions[1].characteristic == ble::Uuid::fromString("6be8f580-9803-11e3-ab03-0002a5d5c51b"));
    assert(!bike->onConnected(0, 0) && !bike->lastError().empty());
    assert(bike->onConnected(0x0c, 10));
    auto* source = static_cast<NautilusSource*>(bike.get());
    assert(source->selectedService() == 1 && source->layout() == Layout::BikeB616);
    ble::GattWrite write;
    assert(bike->takeWrite(write) && !bike->takeWrite(write));
    assert(write.service == ble::Uuid::fromString("f755c9cf-e1fc-4ecd-8d90-f2d7ebf56b81"));
    assert(write.characteristic == writeUuid && write.payload() == nautilus::initFrame());
    Bytes frame = sized(14);
    frame[3] = 0xe8; frame[4] = 0x03; frame[1] = 90; frame[6] = 0x64;
    notify(*bike, 0, frame, 20);  // other service: ignored
    assert(!bike->hasData());
    notify(*bike, 3, frame, 30);
    assert(bike->hasData() && bike->lastDataMs() == 30);
    assert(near(bike->sample().speed.value, 10.0F) && near(bike->sample().power.value, 1.0F));
    assert(bike->sample().cadence.value == 90 && !bike->sample().heartRate.valid && !bike->sample().distance.valid);
    assert(!bike->sample().elapsed.valid && !bike->sample().totalEnergy.valid);
    Bytes truncated = sized(13);
    notify(*bike, 3, truncated, 40);
    assert(bike->lastDataMs() == 30);
    auto caps = bike->capabilities();
    assert(caps.controls == 0 && !bike->isTrainer());
    assert(caps.supports(Metric::Speed) && caps.supports(Metric::Power) && caps.supports(Metric::Cadence));
    assert(!caps.supports(Metric::HeartRate) && !caps.supports(Metric::Distance));
    assert(!bike->control({Control::Resistance, 50}, 50) && !bike->control({Control::Power, 100}, 50));
    assert(bike->pendingWrites() == 0);

    // Manual B616 mph variant: same frame, miles factor.
    auto bikeMi = make("nautilus-b616-mi");
    assert(bikeMi->onConnected(0x04, 0));
    notify(*bikeMi, 2, frame, 1);
    assert(near(bikeMi->sample().speed.value, 16.0934F));

    // Elliptical: four services = 8 subscriptions; first available wins.
    auto ell = make("nautilus-ellip");
    assert(ell->plan().subscriptionCount == 8);
    assert(ell->onConnected(0xff, 0));
    assert(static_cast<NautilusSource*>(ell.get())->selectedService() == 0);
    assert(ell->onConnected(0x30, 0));
    assert(static_cast<NautilusSource*>(ell.get())->layout() == Layout::EllipticalV1);
    Bytes m7 = sized(20);
    m7[1] = 0x5e; m7[2] = 0x4c; m7[3] = 0x04; m7[5] = 70;
    notify(*ell, 4, m7, 5);  // not M7: generic 20-byte resistance/HR path
    assert(ell->sample().resistance.valid && !ell->sample().speed.valid);
    auto m7source = make("nautilus-m7");
    assert(m7source->onConnected(0x04, 0));
    notify(*m7source, 2, m7, 5);
    assert(near(m7source->sample().speed.value, 11.0F) && m7source->sample().cadence.value == 70);
    assert(m7source->profile() == MachineProfile::CrossTrainer);

    auto v2 = make("nautilus-el-v2");
    assert(v2->plan().subscriptionCount == 2);
    assert(v2->plan().subscriptions[0].service == ble::Uuid::fromString("7df7a3f7-f013-492f-a58e-68a8f078ab96"));

    // Treadmill: read-only, never sends anything beyond init.
    auto tm = make("nautilus-tm");
    assert(tm->plan().subscriptionCount == 8);
    assert(tm->onConnected(0x40, 0));
    assert(tm->takeWrite(write) &&
           write.service == ble::Uuid::fromString("4b2387a0-be8e-11e3-8039-0002a5d5c51b"));
    Bytes t = sized(12);
    t[3] = 0xf4; t[4] = 0x01; t[6] = 2;
    notify(*tm, 7, t, 3);
    assert(near(tm->sample().speed.value, 8.0467F) && tm->sample().incline.value == 2);
    assert(tm->capabilities().controls == 0);
    assert(!tm->control({Control::Speed, 0}, 4) && !tm->control({Control::Incline, 3}, 4));
    assert(tm->pendingWrites() == 0);
    tm->reset();
    assert(!tm->hasData() && static_cast<NautilusSource*>(tm.get())->selectedService() == -1);
    notify(*tm, 7, t, 5);
    assert(!tm->hasData());

    auto t628 = make("nautilus-t628");
    assert(t628->onConnected(0x01, 0));
    notify(*t628, 0, t, 3);
    assert(t628->sample().speed.valid && !t628->sample().incline.valid);

    auto t688 = make("nautilus-tm-688a");
    assert(t688->plan().subscriptionCount == 2 &&
           t688->plan().subscriptions[0].service == ble::Uuid::fromString("688a15a0-3a30-11e6-b3f8-0002a5d5c51b"));

    // Bowflex treadmill: five notify characteristics on edff9e80.
    auto bow = make("bowflex-tm");
    plan = bow->plan();
    assert(plan.subscriptionCount == 5);
    assert(plan.subscriptions[0].service == ble::Uuid::fromString("edff9e80-cad7-11e5-ab63-0002a5d5c51b"));
    assert(plan.subscriptions[4].characteristic == ble::Uuid::fromString("d57cda20-9803-11e3-8426-0002a5d5c51b"));
    assert(bow->onConnected(0x10, 0));
    assert(bow->takeWrite(write) && write.payload() == nautilus::initFrame());
    Bytes b = sized(17);
    b[14] = 30; b[11] = 1;
    notify(*bow, 4, b, 9);
    assert(near(bow->sample().speed.value, 3.0F) && bow->sample().incline.value == 1);
    assert(bow->sample().totalEnergy.valid && bow->sample().totalEnergy.value == 0);
}

void matcherTests() {
    struct Case { const char* name; const char* id; };
    const Case cases[] = {
        {"NAUTILUS B628", "nautilus-bike"},     {"Nautilus B616", "nautilus-bike"},
        {"NAUTILUS E616", "nautilus-ellip"},    {"Nautilus E618", "nautilus-ellip"},
        {"NAUTILUS M9", "nautilus-ellip"},      {"NAUTILUS 616", "nautilus-ellip"},
        {"Nautilus M7", "nautilus-m7"},         {"NAUTILUS M7 1234", "nautilus-m7"},
        {"NAUTILUS T616", "nautilus-tm"},       {"Schwinn T7", "nautilus-tm"},
        {"SCHWINN 570T", "nautilus-tm"},        {"NAUTILUS T628", "nautilus-t628"},
        {"nautilus t628 x", "nautilus-t628"}};
    for (const auto& c : cases) {
        std::size_t matches = 0;
        for (std::size_t i = 0; i < nautilus::descriptorCount(); ++i) {
            const auto& d = nautilus::descriptorAt(i);
            if (!d.matches(advertisement(c.name))) continue;
            ++matches;
            assert(std::string(d.id) == c.id);
            assert(d.priority == nautilus::kMatchPriority);
            assert(!d.matches(advertisement(c.name, true)));  // FTMS advertisers excluded
        }
        assert(matches == 1);
    }
    const char* negatives[] = {"",           "NAUTILUS",        "NAUTILUS ",     "NAUTILUS X1", "NAUTILU B",
                               "BOWFLEX T216", "BOWFLEX T22",   "SCHWINN 170/270", "SCHWINN 510T", "SCHWINN 810",
                               "SCHWINN IC4", "IC BIKE",        "SCHWINN",       "XNAUTILUS B"};
    for (const char* name : negatives)
        for (std::size_t i = 0; i < nautilus::descriptorCount(); ++i)
            assert(!nautilus::descriptorAt(i).matches(advertisement(name)));
}

void descriptorTests() {
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < nautilus::descriptorCount(); ++i) {
        const auto& d = nautilus::descriptorAt(i);
        const std::string id(d.id);
        assert(!id.empty() && id.size() <= 16);
        for (char c : id) assert((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
        for (const auto& other : ids) assert(other != id);
        ids.push_back(id);
        assert(!d.controllable && !d.advertisementOnly);
        auto source = d.create();
        assert(source && std::string(source->id()) == id);
        assert(source->profile() == d.profile);
        assert(!source->advertisementOnly() && source->capabilities().controls == 0);
        const auto plan = source->plan();
        assert(plan.subscriptionCount > 0 && plan.subscriptionCount <= ble::kMaxSubscriptions);
        assert(plan.readCount == 0 && !plan.discoverNotifications);
        if (d.priority == 0) {
            // Manual-only entries never auto-match.
            assert(!d.matches(advertisement("NAUTILUS T616")) && !d.matches(advertisement("BOWFLEX TREADMILL")));
        }
    }
    assert(nautilus::descriptorCount() == 9);
    assert(std::string(nautilus::descriptorAt(99).id) == nautilus::descriptorAt(0).id);
}

}  // namespace

void runNautilusTests() {
    codecTests();
    sourceTests();
    matcherTests();
    descriptorTests();
    std::printf("nautilus host tests passed (%zu protocols)\n", nautilus::descriptorCount());
}

// Requires registry integration; intentionally not called by runNautilusTests().
void runNautilusRegistryTests() {
    struct Case { const char* name; const char* id; };
    const Case cases[] = {{"NAUTILUS B628", "nautilus-bike"}, {"NAUTILUS E616", "nautilus-ellip"},
                          {"Nautilus M7", "nautilus-m7"},     {"NAUTILUS T616", "nautilus-tm"},
                          {"SCHWINN 570T", "nautilus-tm"},    {"NAUTILUS T628", "nautilus-t628"}};
    for (const auto& c : cases) {
        const auto* info = detectProtocol(advertisement(c.name));
        assert(info && std::string(info->id) == c.id);
    }
    // Existing ports keep their names.
    const auto* bowflex = detectProtocol(advertisement("BOWFLEX T216"));
    assert(bowflex && std::string(bowflex->id) == "bowflex-t216");
    const auto* schwinn = detectProtocol(advertisement("SCHWINN 170/270"));
    assert(schwinn && std::string(schwinn->id) == "schwinn-170");
    for (std::size_t i = 0; i < nautilus::descriptorCount(); ++i) {
        const auto& d = nautilus::descriptorAt(i);
        const auto* info = findProtocol(d.id);
        assert(info && info->profile == d.profile);
        auto created = createProtocol(d.id);
        assert(created && std::string(created->id()) == d.id);
    }
    std::printf("nautilus registry tests passed\n");
}
