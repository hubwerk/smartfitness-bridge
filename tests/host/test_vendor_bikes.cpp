// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{flywheelbike,solebike,kettlerc12bike,technogymbike,inspirebike,
// keepbike,mcfbike,xcxbike,chronobike,volavabike,ultrasportbike,skandikawiribike,
// pitpatbike,freebeatboombike,mepanelbike,apexbike,npecablebike,
// cycleopsphantombike,renphobike,lifespanbike,horizongr7bike,cscbike}/
// (same-named .cpp/.h); src/devices/bluetooth.cpp (advertisement rules).
//
// All notification frames below are SYNTHETIC: built by hand following the
// byte layout read by the upstream parsers, not captured from hardware.

#include "protocols/vendor_bikes.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "protocols/registry.hpp"

namespace {
using namespace smartfitness;
using namespace smartfitness::protocols;
using namespace smartfitness::protocols::vendor_bikes;
using ble::Bytes;

bool near(float a, float b, float eps = 0.01F) { return std::fabs(a - b) <= eps; }

ble::Advertisement advertisement(const char* name, bool ftms = false) {
    ble::Advertisement out;
    out.name = name;
    if (ftms) out.services.push_back(ble::Uuid::from16(0x1826));
    return out;
}

const ProtocolInfo& descriptor(const char* id) {
    for (std::size_t i = 0; i < descriptorCount(); ++i)
        if (std::string(descriptorAt(i).id) == id) return descriptorAt(i);
    assert(false && "unknown id");
    return descriptorAt(0);
}

// Ids of every descriptor in this family that matches the advertisement.
std::vector<std::string> matches(const ble::Advertisement& a) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < descriptorCount(); ++i)
        if (descriptorAt(i).matches(a)) out.push_back(descriptorAt(i).id);
    return out;
}
bool matchesOnly(const char* name, const char* id, bool ftms = false) {
    const auto m = matches(advertisement(name, ftms));
    return m.size() == 1 && m[0] == id;
}

std::vector<ble::GattWrite> drain(SourceProtocol& source) {
    std::vector<ble::GattWrite> out;
    ble::GattWrite write;
    while (source.takeWrite(write)) out.push_back(write);
    return out;
}

void notify(SourceProtocol& p, const Bytes& frame, std::uint32_t now, std::size_t index = 0) {
    p.onNotify(index, frame.data(), frame.size(), now);
}

std::unique_ptr<SourceProtocol> connected(Model model, std::uint32_t now = 1000, const char* name = nullptr) {
    auto source = createVendorBike(model);
    source->reset();
    if (name) source->onAdvertisement(advertisement(name), now);
    assert(source->onConnected(0xff, now));
    return source;
}

void builderTests() {
    assert(skandikaPoll(5) == Bytes({0x20, 0x01, 0x05, 0x00, 0x26}));
    assert(skandikaPoll(0) == Bytes({0x20, 0x01, 0x01, 0x00, 0x22}));
    assert(pitpatResistance(10) == Bytes({0x6a, 0x06, 0x51, 0x82, 0x01, 0x0a, 0xde, 0x43}));
    assert(freebeatResistance(50) == Bytes({0x25, 0x03, 0x32, 0x35, 0xa0}));
    assert(freebeatResistance(200) == Bytes({0x25, 0x03, 0x64, 0x67, 0xa0}));
    assert(technogymResistance(60) == Bytes({0xf9, 0x01, 0x14, 0x00, 0xe4, 0x02, 0x18, 0x01, 0x03, 0x19, 0x00}));
    assert(technogymPower(250) == Bytes({0xf8, 0x01, 0xfa, 0x00, 0xe4, 0x02, 0x18, 0x01, 0x03, 0x19, 0x00}));
    assert(cycleopsPower(300) == Bytes({0x00, 0x10, 0x01, 0x2c, 0x01, 0, 0, 0, 0, 0}));
    assert(bcd(0x42) == 42 && bcd(0x09) == 9 && bcd(0x4a) == 0);
    assert(apexPower(10, 60) == 139);
    assert(apexPower(10, 65) == 150);  // 139 + 0.5 * (162 - 139)
    assert(apexPower(1, 5) == 6);
    assert(apexPower(19, 160) == 546);
    assert(apexPower(0, 60) == 0 && apexPower(5, 0) == 0);
    assert(inspirePower(10, 60) == 34);
    assert(inspirePower(10, 0) == 0);

    FlywheelDecoder decoder;
    const Bytes frame{0xff, 3, 12, 0xaa, 0x00, 0x55};
    assert(decoder.feed(frame.data(), frame.size()) && decoder.id() == 12 && decoder.size() == 1);
    const Bytes badEnd{0xff, 3, 12, 0xaa, 0x00, 0x54};
    assert(!decoder.feed(badEnd.data(), badEnd.size()));
}

void matcherTests() {
    assert(matchesOnly("Flywheel 1", "flywheel"));
    assert(matchesOnly("LCB-1234", "sole-bike"));
    assert(matchesOnly("lcr", "sole-bike"));
    assert(matchesOnly("R92-1", "sole-r92"));
    assert(matchesOnly("KETTLER C12 01", "kettler-c12"));
    assert(matchesOnly("ERGO C12", "kettler-c12"));
    assert(matchesOnly("MyCycling 1", "technogym"));
    assert(matchesOnly("IC123456", "inspire"));
    assert(matchesOnly("KEEP_BIKE_01", "keep-bike"));
    assert(matchesOnly("KEEP_CC_02", "keep-bike"));
    assert(matchesOnly("MCF-0001", "mcf-bike"));
    assert(matchesOnly("XCX-1", "xcx-bike"));
    assert(matchesOnly("XCX-1", "xcx-bike", true));  // upstream routes XCX even with FTMS
    assert(matchesOnly("CHRONO BIKE", "chrono-bike"));
    assert(matchesOnly("XQ0123456789", "volava"));
    assert(matchesOnly("X-BIKE 7", "ultrasport"));
    assert(matchesOnly("BFCP-1", "skandika-wiri"));
    assert(matchesOnly("HT123456789A", "skandika-wiri"));
    assert(matchesOnly("HT123456789", "skandika-x2000"));
    assert(matchesOnly("PITPAT-S1", "pitpat-bike"));
    assert(matchesOnly("FEAQ51A_123", "freebeat-boom"));
    assert(matchesOnly("MEPANEL-1", "mepanel"));
    assert(matchesOnly("WLT8266BM12", "apex-bike"));
    assert(matchesOnly(">CABLE 1", "npe-cable"));
    assert(matchesOnly("MD12345", "npe-cable"));
    assert(matchesOnly("BIKE 1", "npe-cable"));
    assert(matchesOnly("IndoorCycle 9", "cycleops"));
    assert(matchesOnly("MAGNUS 1", "cycleops"));
    assert(matchesOnly("JOROTO-BK-1", "csc-bike"));
    assert(matchesOnly("BGYM1234", "csc-bike"));
    assert(matchesOnly("RQ123", "renpho"));
    assert(matchesOnly("R-Q12345", "renpho"));
    assert(matchesOnly("SCH130 1", "renpho"));
    assert(matchesOnly("JFIC1234", "horizon-gr7"));

    // FTMS advertisers stay with the generic adapter.
    const char* names[] = {"Flywheel 1", "LCB-1234", "IC123456", "MCF-0001", "HT123456789A", "PITPAT-S1",
                           "BIKE 1", "JOROTO-BK-1", "RQ123", "JFIC1234", "MyCycling 1", "MAGNUS 1"};
    for (const char* name : names) assert(matches(advertisement(name, true)).empty());

    // Generic or other-family names are not claimed.
    const char* foreign[] = {"IC BIKE 12", "IC12345", "IC1234567", "TOORX 1", "PITPAT-T1", "FS-12345",
                             "TF-12345", "LIFESPAN", "LIFESPAN-1", "HT12", "MD1234", "MD123456", "BIKE 12",
                             "BIKE", "BGYM12345", "RQ1234", "R-Q12", "XQ012", "ZWIFT HUB", "Kinetic", "BKOOL",
                             "HORIZON", ""};
    for (const char* name : foreign) assert(matches(advertisement(name)).empty());

    // Manual-only entries never match.
    assert(descriptor("lifefit-ic5").priority == 0 && descriptor("lifespan-bike").priority == 0);
    assert(!descriptor("lifespan-bike").matches(advertisement("LIFESPAN")));
}

void registryShapeTests() {
    std::set<std::string> ids;
    for (std::size_t i = 0; i < descriptorCount(); ++i) {
        const auto& d = descriptorAt(i);
        const std::string id = d.id;
        assert(!id.empty() && id.size() <= 16);
        for (char c : id) assert((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
        assert(ids.insert(id).second);
        assert(d.profile == MachineProfile::IndoorBike);
        assert(!d.advertisementOnly);
        assert(d.priority == 0 || d.priority == kMatchPriority);
        auto source = d.create();
        assert(source);
        assert(std::string(source->id()) == id);
        assert(source->profile() == d.profile);
        assert(source->plan().subscriptionCount > 0);
    }
    assert(descriptorCount() == static_cast<std::size_t>(Model::Count) + 2);
    for (std::size_t m = 0; m < static_cast<std::size_t>(Model::Count); ++m)
        assert(std::string(descriptorAt(m).id) == modelId(static_cast<Model>(m)));
}

Bytes flywheelFrame(std::uint16_t power, std::uint8_t cadence, std::uint16_t speed10, std::uint8_t brake) {
    Bytes data(13, 0);
    data[0] = static_cast<std::uint8_t>(power >> 8);
    data[1] = static_cast<std::uint8_t>(power);
    data[9] = cadence;
    data[10] = static_cast<std::uint8_t>(speed10 >> 8);
    data[11] = static_cast<std::uint8_t>(speed10);
    data[12] = brake;
    Bytes frame{0xff, static_cast<std::uint8_t>(data.size() + 2), 12};
    frame.insert(frame.end(), data.begin(), data.end());
    frame.push_back(0x00);  // checksum (not verified upstream)
    frame.push_back(0x55);
    return frame;
}

void flywheelTests() {
    auto source = connected(Model::Flywheel);
    auto writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == Bytes({0xf5, 0x20, 0x20, 0x40, 0xf6}));
    assert(writes[0].characteristic == ble::Uuid::fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e"));
    // Synthetic stream frame split across two notifications.
    const auto frame = flywheelFrame(200, 80, 300, 7);
    notify(*source, Bytes(frame.begin(), frame.begin() + 8), 1100);
    assert(!source->hasData());
    notify(*source, Bytes(frame.begin() + 8, frame.end()), 1101);
    assert(source->hasData());
    const auto& s = source->sample();
    assert(s.power.value == 200 && s.cadence.value == 80 && near(s.speed.value, 30) && s.resistance.value == 7);
    // Two zero frames are filtered, the third is accepted (watts forced 0).
    const auto zero = flywheelFrame(150, 0, 0, 7);
    notify(*source, zero, 1200);
    notify(*source, zero, 1201);
    assert(source->sample().power.value == 200);
    notify(*source, zero, 1202);
    assert(source->sample().power.value == 0 && source->sample().cadence.value == 0);
    assert(!source->capabilities().controls);

    auto ic5 = connected(Model::LifeFitnessIc5);
    writes = drain(*ic5);
    assert(writes.size() == 2 && writes[0].length == 19 && writes[1].length == 15);
    assert(writes[1].payload()[0] == 0xff && writes[1].payload()[14] == 0x55);
}

void soleTests() {
    auto source = connected(Model::SoleBike);
    auto writes = drain(*source);
    assert(writes.size() == 9 && writes[0].payload() == Bytes({0x5b, 0x01, 0xf0, 0x5d}));
    assert(writes[8].payload() == Bytes({0x5b, 0x02, 0x03, 0x04, 0x5d}));
    source->tick(1300);
    writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == Bytes({0x5b, 0x04, 0x00, 0x10, 0x4f, 0x4b, 0x5d}));
    source->tick(1600);
    source->tick(1900);
    source->tick(2200);  // 4th tick skipped
    source->tick(2500);
    writes = drain(*source);
    assert(writes.size() == 3 && writes[0].payload()[3] == 0x06 && writes[1].payload()[3] == 0x13 &&
           writes[2].payload()[3] == 0x10);

    Bytes data(20, 0);  // synthetic workout frame
    data[0] = 0x5b; data[1] = 0x13; data[10] = 70; data[11] = 0x05; data[12] = 0xdc; data[13] = 0; data[14] = 150;
    notify(*source, data, 3000);
    const auto& s = source->sample();
    assert(s.cadence.value == 70 && s.power.value == 150 && near(s.speed.value, 15 * 1.60934F));
    notify(*source, {0x5b, 0x02, 0x13, 0x08, 0x5d}, 3001);
    assert(s.resistance.value == 8);
    notify(*source, {0x5b, 0x02, 0x13, 0x00, 0x5d}, 3002);
    assert(s.resistance.value == 1);
    data[10] = 99;
    notify(*source, Bytes(data.begin(), data.begin() + 19), 3003);  // truncated
    assert(s.cadence.value == 70);
    assert(!source->capabilities().controls);

    auto r92 = connected(Model::SoleR92);
    writes = drain(*r92);
    assert(writes.size() == 14 && writes[2].payload() == Bytes({0x5b, 0x04, 0x00, 0x13, 0x4f, 0x4b, 0x5d}));
    r92->tick(1300); r92->tick(1600); r92->tick(1900);
    writes = drain(*r92);
    assert(writes.size() == 3 && writes[2].payload()[3] == 0x14);
}

void keepTests() {
    auto source = connected(Model::KeepBike);
    auto writes = drain(*source);
    assert(writes.size() == 9 && writes[0].length == 20 && writes[0].payload()[1] == 0x4b);
    assert(writes[0].characteristic == ble::Uuid::fromString("ff02"));
    source->tick(1600);
    assert(drain(*source).empty());
    source->tick(2200);
    writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == Bytes({0x01, 0x53, 0x23, 0x00, 0x00}));
    source->onWriteFailed(2201);  // falls back to the notify characteristic
    assert(!source->failed());
    source->tick(3400);
    writes = drain(*source);
    assert(writes.size() == 1 && writes[0].characteristic == ble::Uuid::fromString("ff01"));

    Bytes data(20, 0);
    data[14] = 12; data[16] = 85; data[17] = 0x00; data[18] = 0xb4;
    notify(*source, data, 3500);
    const auto& s = source->sample();
    assert(s.resistance.value == 12 && s.cadence.value == 85 && s.power.value == 180);
    data[0] = data[1] = 0xa5; data[16] = 10;
    notify(*source, data, 3501);
    notify(*source, Bytes(19, 0), 3502);
    assert(s.cadence.value == 85);
}

void simpleVendorTests() {
    {   // MCF
        auto source = connected(Model::Mcf);
        assert(drain(*source).size() == 9);
        source->tick(1300);
        auto writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload()[1] == 0xd5 && writes[0].payload()[3] == 0x01);
        Bytes d(20, 0);
        d[1] = 0xe5; d[9] = 0x00; d[10] = 0x96; d[11] = 0x00; d[12] = 0xfa; d[13] = 6; d[15] = 72;
        notify(*source, d, 1400);
        const auto& s = source->sample();
        assert(s.power.value == 150 && near(s.speed.value, 25) && s.resistance.value == 6 && s.cadence.value == 72);
        d[1] = 0xe4; d[15] = 1;
        notify(*source, d, 1401);
        assert(s.cadence.value == 72);
    }
    {   // XCX
        auto source = connected(Model::Xcx);
        assert(drain(*source).empty() && source->capabilities().controls == 0);
        Bytes d(20, 0);
        d[0] = 0xfa; d[1] = 0x05; d[6] = 0xfa; d[7] = 0x00; d[8] = 0x0c; d[11] = 0x2a; d[13] = 75; d[17] = 140;
        d[18] = 9;
        notify(*source, d, 1100);
        const auto& s = source->sample();
        assert(near(s.speed.value, 25) && s.distance.value == 1200 && s.totalEnergy.value == 42 &&
               s.cadence.value == 75 && s.power.value == 140 && s.resistance.value == 9);
        d.resize(21);
        d[13] = 1;
        notify(*source, d, 1101);
        assert(s.cadence.value == 75);
    }
    {   // Chrono
        auto source = connected(Model::Chrono);
        Bytes d(19, 0);
        d[6] = 0xc4; d[7] = 0x09; d[8] = 160; d[17] = 0xc8; d[18] = 0x00;
        notify(*source, d, 1100);
        const auto& s = source->sample();
        assert(near(s.speed.value, 25) && s.cadence.value == 80 && s.power.value == 200);
        notify(*source, Bytes(18, 0), 1101);
        assert(s.cadence.value == 80);
    }
    {   // Volava
        auto source = connected(Model::Volava);
        auto writes = drain(*source);
        assert(writes.size() == 1 && writes[0].length == 15 && !writes[0].withResponse);
        source->tick(1100);
        assert(drain(*source).empty());
        source->tick(1200);
        writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload() == Bytes({0xaa, 0x06, 0x80, 0xe1, 0x00, 0x11}));
        Bytes d(16, 0);
        d[0] = 0xaa; d[3] = 0x80; d[6] = 0x00; d[7] = 0x78; d[11] = 90; d[14] = 15;
        notify(*source, d, 1300);
        const auto& s = source->sample();
        assert(s.power.value == 120 && s.cadence.value == 90 && s.resistance.value == 15 &&
               near(s.speed.value, 34.2F));
        d[3] = 0x81;
        notify(*source, d, 1301);
        notify(*source, Bytes(d.begin(), d.begin() + 15), 1302);
        assert(s.power.value == 120);
    }
    {   // Ultrasport
        auto source = connected(Model::Ultrasport);
        assert(drain(*source).size() == 10);
        source->tick(2200);
        auto writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload() == Bytes({0xff, 0xfd, 0x03, 0x00, 0xec}));
        Bytes d(18, 0);
        d[5] = 0x00; d[6] = 0x6e; d[9] = 130;
        notify(*source, d, 2300);
        assert(source->sample().power.value == 110 && source->sample().heartRate.value == 130);
        notify(*source, Bytes(17, 0), 2301);
        assert(source->sample().power.value == 110);
    }
    {   // Inspire
        auto source = connected(Model::Inspire);
        Bytes d(8, 0);
        d[3] = 60; d[6] = 10;
        notify(*source, d, 1100);
        const auto& s = source->sample();
        assert(s.cadence.value == 60 && s.resistance.value == 10 && s.power.value == 34 &&
               near(s.speed.value, 0.37497622F * 60));
        notify(*source, Bytes(9, 0), 1101);
        assert(s.cadence.value == 60);
    }
    {   // Mepanel
        auto source = connected(Model::Mepanel);
        auto writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload() == Bytes({0x1e, 0x11, 0x00, 0x11}));
        notify(*source, {30, 0x09, 0xc4}, 1100);
        notify(*source, {45, 0x00, 0x50}, 1101);
        notify(*source, {105, 0x07, 0xd0}, 1102);
        notify(*source, {120, 140, 0}, 1103);
        const auto& s = source->sample();
        assert(near(s.speed.value, 25) && s.cadence.value == 80 && s.power.value == 200 && s.heartRate.value == 140);
        notify(*source, {45, 0x00, 0x00}, 1104);
        assert(s.cadence.value == 0 && s.power.value == 0);
        notify(*source, {99, 1, 2}, 1105);
        notify(*source, {30, 1}, 1106);
        assert(near(s.speed.value, 25));
    }
    {   // Apex
        auto source = connected(Model::Apex);
        auto writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload()[0] == 0xaa);
        source->tick(2999);
        assert(drain(*source).empty());
        source->tick(3000);
        writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload()[0] == 0xaa);
        source->tick(5000);
        source->tick(5500);
        source->tick(6000);
        writes = drain(*source);
        assert(writes.size() == 3 && writes[0].payload() == Bytes({0xeb, 0x50, 0x51, 0x01, 0, 0, 0, 0, 0, 0xa2}));
        Bytes d(10, 0);
        d[2] = 0x31; d[4] = 30; d[5] = 23;
        notify(*source, d, 6100);
        const auto& s = source->sample();
        assert(s.cadence.value == 60 && s.resistance.value == 10 && s.power.value == 139);
        d[2] = 0x30; d[4] = 10;
        notify(*source, d, 6101);
        assert(s.cadence.value == 60);
        source->tick(6500);  // init done; poll every 5 s
        drain(*source);
        source->tick(11500);
        writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload()[0] == 0xaa);
    }
    {   // Freebeat
        auto source = connected(Model::FreebeatBoom);
        assert(drain(*source).empty());
        source->tick(1200);
        auto writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload() == Bytes({0x25, 0x08, 0x00, 0x08, 0xa0}) &&
               !writes[0].withResponse);
        Bytes d(13, 0);
        d[0] = 0x55; d[1] = 0x15; d[7] = 40; d[8] = 80; d[12] = 0x3f;
        notify(*source, d, 1300);
        const auto& s = source->sample();
        assert(s.cadence.value == 80 && s.resistance.value == 40 && near(s.power.value, 145.92F));
        d[12] = 0x3e; d[8] = 1;
        notify(*source, d, 1301);
        assert(s.cadence.value == 80);
        assert(source->capabilities().supports(Control::Resistance));
        assert(source->control({Control::Resistance, 50}, 1400));
        writes = drain(*source);
        const auto level = static_cast<std::uint8_t>(percentToLevel(50, 1, 100));
        assert(writes.size() == 1 && writes[0].payload() == freebeatResistance(level));
        assert(!source->control({Control::Power, 100}, 1401));
    }
}

void skandikaTests() {
    auto source = connected(Model::SkandikaWiri);
    auto writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == Bytes({0x40, 0x00, 0x9a, 0x24, 0xfe}));
    source->tick(1300);
    writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == skandikaPoll(1));
    Bytes d(12, 0);
    d[1] = 0x00; d[2] = 0x01; d[3] = 0x25; d[6] = 0x00; d[7] = 0x42; d[8] = 120;
    notify(*source, d, 1400);
    const auto& s = source->sample();
    assert(near(s.speed.value, 12.5F) && s.totalEnergy.value == 42 && s.heartRate.value == 120);
    d[1] = 0x10; d[2] = 0x00; d[3] = 0x75; d[9] = 0x01; d[10] = 0x50;
    notify(*source, d, 1401);
    assert(s.cadence.value == 75 && s.power.value == 150);
    notify(*source, {0x00, 0x00, 0x09, 0x00, 0x00}, 1402);
    assert(s.resistance.value == 9);
    notify(*source, {0x00, 0x00, 0x40, 0x00, 0x00}, 1403);
    notify(*source, Bytes(11, 0), 1404);
    assert(s.resistance.value == 9 && s.cadence.value == 75);
    assert(source->control({Control::Resistance, 50}, 1500));
    const auto level = static_cast<std::uint8_t>(percentToLevel(50, 1, 32));
    writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == skandikaPoll(level));
    source->tick(1600);
    writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == skandikaPoll(level));

    auto x2000 = connected(Model::SkandikaX2000);
    writes = drain(*x2000);
    assert(writes.size() == 2 && writes[1].payload() == Bytes({0x40, 0x00, 0x9a, 0x38, 0x12}));
    Bytes e(12, 0);
    e[1] = 0x30; e[2] = 0x00; e[3] = 0x88; e[9] = 0x09; e[10] = 0x99;
    notify(*x2000, e, 1100);
    assert(x2000->sample().cadence.value == 88 && !x2000->sample().power.valid);
    notify(*x2000, {0, 0, 12, 0, 0, 0}, 1101);
    assert(x2000->sample().resistance.value == 12);
}

void pitpatTests() {
    // Normal (30-byte) protocol: ready after 1.5 s, then polls.
    auto source = connected(Model::PitPat);
    assert(drain(*source).empty());
    assert(!source->control({Control::Resistance, 50}, 1100));
    source->tick(2500);
    assert(source->capabilities().supports(Control::Resistance));
    assert(drain(*source).empty());
    source->tick(3700);
    auto writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == Bytes({0x6a, 0x05, 0xfd, 0xf8, 0x43}));
    Bytes d(30, 0);
    d[5] = 8; d[23] = 0x00; d[24] = 0x96; d[25] = 70;
    notify(*source, d, 3800);
    const auto& s = source->sample();
    assert(s.resistance.value == 8 && s.power.value == 150 && s.cadence.value == 70);
    notify(*source, Bytes(29, 0), 3801);
    assert(s.cadence.value == 70);
    assert(source->control({Control::Resistance, 100}, 3900));
    writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == pitpatResistance(32));

    // Legacy protocol: status frame -> handshake -> ack -> ready.
    auto legacy = connected(Model::PitPat);
    Bytes status(34, 0);
    status[0] = 0x6a; status[3] = 0x2c;
    notify(*legacy, status, 1100);
    writes = drain(*legacy);
    assert(writes.size() == 1 && writes[0].payload() == Bytes({0x6a, 0x05, 0x50, 0x1b, 0x01, 0x4f, 0x43}));
    legacy->tick(3000);
    assert(!legacy->capabilities().controls);
    notify(*legacy, {0x6a, 0x06, 0xb0, 0x1b, 0x01, 0x00, 0xac, 0x43}, 3100);
    assert(legacy->capabilities().supports(Control::Resistance));
    Bytes data(28, 0);
    data[0] = 0x6a; data[3] = 0x02; data[5] = 10; data[10] = 0x01; data[11] = 0xf4; data[12] = 0x01;
    data[13] = 0xa4; data[18] = 110; data[23] = 0x61; data[24] = 0xa8; data[25] = 60;
    notify(*legacy, data, 3200);
    const auto& l = legacy->sample();
    assert(l.cadence.value == 60 && l.power.value == 87 && l.distance.value == 500 && near(l.totalEnergy.value, 42) &&
           l.heartRate.value == 110 && near(l.speed.value, 25) && l.resistance.value == 10);
}

void lifespanTests() {
    auto source = connected(Model::LifespanBike);
    auto writes = drain(*source);
    assert(writes.size() == 1 && writes[0].payload() == Bytes({0x02, 0x00, 0x00, 0x00, 0x00}));
    notify(*source, {0x02, 0x00, 0x05, 0x00}, 1100);  // no command pending
    assert(!source->hasData());
    const std::uint8_t commands[] = {0x91, 0x81, 0x8a, 0x86, 0x87, 0x85, 0x89, 0x82, 0x84};
    const Bytes responses[] = {{0xa1, 0x91, 5, 0}, {0xa1, 0x81, 0, 0}, {0xa1, 0x8a, 0, 72}, {0xa1, 0x86, 0, 0},
                               {0xa1, 0x87, 0x00, 0x2a}, {0xa1, 0x85, 0x00, 0x0c}, {0xa1, 0x89, 0, 0},
                               {0xa1, 0x82, 25, 50}, {0xa1, 0x84, 0x00, 0x0c}};
    std::uint32_t now = 1500;
    for (std::size_t i = 0; i < 9; ++i, now += 500) {
        source->tick(now);
        writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload() == Bytes({0xa1, commands[i], 0, 0, 0}));
        notify(*source, responses[i], now + 10);
    }
    const auto& s = source->sample();
    assert(s.resistance.value == 5 && s.cadence.value == 72 && s.totalEnergy.value == 42 &&
           s.distance.value == 1200 && near(s.speed.value, 25.5F) && s.power.value == 72);
    notify(*source, {0xa1, 0x84, 0x00, 0x99}, now);  // reply without a pending command
    assert(s.power.value == 72);
}

Bytes cpsCrank(std::int16_t power, std::uint16_t revs, std::uint16_t time) {
    return {0x20, 0x00, static_cast<std::uint8_t>(power), static_cast<std::uint8_t>(power >> 8),
            static_cast<std::uint8_t>(revs), static_cast<std::uint8_t>(revs >> 8), static_cast<std::uint8_t>(time),
            static_cast<std::uint8_t>(time >> 8)};
}
Bytes cscCrank(std::uint16_t revs, std::uint16_t time) {
    return {0x02, static_cast<std::uint8_t>(revs), static_cast<std::uint8_t>(revs >> 8),
            static_cast<std::uint8_t>(time), static_cast<std::uint8_t>(time >> 8)};
}

void standardTests() {
    {   // Kettler C12: per-metric characteristics + CPS/CSC.
        auto source = createVendorBike(Model::KettlerC12);
        assert(source->plan().subscriptionCount == 8);
        assert(source->onConnected(0x01, 1000));
        auto writes = drain(*source);
        assert(writes.size() == 10 && !writes[0].withResponse);
        assert(writes[0].characteristic == ble::Uuid::fromString("638a100e-7bde-3e25-ffc5-9de9b2a0197a"));
        assert(writes[2].payload() == Bytes({0x25, 0x00}));
        notify(*source, {0x96, 0x00}, 1100, 1);
        notify(*source, {0xfa, 0x00}, 1101, 2);
        notify(*source, {0x10, 0x27, 0x00, 0x00}, 1102, 5);
        notify(*source, {0x0c, 0x00}, 1103, 0);
        const auto& s = source->sample();
        assert(s.power.value == 150 && near(s.speed.value, 25) && s.distance.value == 10000 &&
               s.resistance.value == 12);
        notify(*source, {0xff, 0x00}, 1104, 3);  // > 250 ignored on the alternative characteristic
        assert(s.resistance.value == 12);
        notify(*source, cpsCrank(210, 10, 0), 1200, 6);
        notify(*source, {0x96, 0x00}, 1300, 1);  // CPS power wins for 3 s
        assert(s.power.value == 210);
        notify(*source, cscCrank(5, 0), 1400, 7);
        notify(*source, cscCrank(6, 1024), 2400, 7);
        assert(near(s.cadence.value, 60, 0.5F));
        notify(*source, {0x96}, 4300, 1);  // truncated
        assert(s.power.value == 210);
    }
    {   // Technogym: CPS telemetry + vendor control characteristic.
        auto source = connected(Model::Technogym);
        assert(source->plan().subscriptionCount == 1);
        assert(source->capabilities().supports(Control::Resistance) && source->capabilities().supports(Control::Power));
        notify(*source, cpsCrank(180, 100, 0), 1100);
        notify(*source, cpsCrank(180, 101, 1024), 2100);
        assert(source->sample().power.value == 180 && near(source->sample().cadence.value, 60, 0.5F));
        notify(*source, cpsCrank(180, 101, 1024), 2200);  // no new revolution: cadence 0 -> watts 0
        assert(source->control({Control::Power, 250}, 2300));
        auto writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload() == technogymPower(250));
        assert(writes[0].service == ble::Uuid::fromString("a913bfc0-929e-11e5-b928-0002a5d5c51b"));
        assert(writes[0].characteristic == ble::Uuid::fromString("58094966-498c-470d-8051-37e617a13895"));
        assert(source->control({Control::Resistance, 100}, 2400));
        writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload() == technogymResistance(100));
        notify(*source, {0x20}, 2500);
        assert(source->sample().power.valid);
    }
    {   // CycleOps: FTMS/CPS/CSC/HR telemetry, proprietary ERG.
        auto source = connected(Model::CycleOps);
        assert(source->plan().subscriptionCount == 4);
        assert(source->capabilities().supports(Control::Power) && !source->capabilities().supports(Control::Resistance));
        notify(*source, cpsCrank(240, 1, 0), 1100, 1);
        notify(*source, {0x00, 0x8c}, 1101, 3);
        assert(source->sample().power.value == 240 && source->sample().heartRate.value == 140);
        assert(source->control({Control::Power, 300}, 1200));
        auto writes = drain(*source);
        assert(writes.size() == 1 && writes[0].payload() == cycleopsPower(300));
        assert(writes[0].characteristic == ble::Uuid::fromString("ca31a533-a858-4dc7-a650-fdeb6dad4c14"));
        assert(!source->control({Control::Resistance, 50}, 1201));
    }
    {   // NPE CABLE: FTMS + CPS + CSC + HR, "BIKE " NUS resistance frames.
        auto source = connected(Model::NpeCable, 1000, "BIKE 1");
        assert(source->plan().subscriptionCount == 5);
        notify(*source, {0x04, 0x00, 0xc4, 0x09, 0xa0, 0x00}, 1100, 0);  // FTMS speed 25 km/h, cadence 80
        assert(near(source->sample().speed.value, 25) && source->sample().cadence.value == 80);
        Bytes nus(20, 0);
        nus[0] = 0xff; nus[1] = 0x1f; nus[15] = 14;
        notify(*source, nus, 1101, 4);
        assert(source->sample().resistance.value == 14);
        nus[1] = 0x1e; nus[15] = 3;
        notify(*source, nus, 1102, 4);
        assert(source->sample().resistance.value == 14);
        notify(*source, cpsCrank(190, 1, 0), 1103, 1);
        assert(source->sample().power.value == 190);
        assert(!source->capabilities().controls);

        auto other = connected(Model::NpeCable, 1000, ">CABLE 1");
        nus[1] = 0x1f;
        notify(*other, nus, 1100, 4);
        assert(!other->sample().resistance.valid);
    }
    {   // Joroto / BGYM: CSC cadence (CPS cadence only, no power).
        auto source = connected(Model::CscBike);
        assert(source->plan().subscriptionCount == 2);
        notify(*source, cscCrank(10, 0), 1100, 1);
        notify(*source, cscCrank(11, 1024), 2100, 1);
        assert(near(source->sample().cadence.value, 60, 0.5F));
        notify(*source, cpsCrank(500, 1, 0), 2200, 0);
        assert(!source->sample().power.valid);
        source->tick(9000);  // sensor idle: cadence decays
        assert(source->sample().cadence.value == 0);
    }
    {   // Missing notify characteristic is fatal for vendor rows.
        auto source = createVendorBike(Model::Mcf);
        assert(!source->onConnected(0, 1000) && !source->lastError().empty());
    }
}

}  // namespace

void runVendorBikesTests() {
    builderTests();
    matcherTests();
    registryShapeTests();
    flywheelTests();
    soleTests();
    keepTests();
    simpleVendorTests();
    skandikaTests();
    pitpatTests();
    lifespanTests();
    standardTests();
    std::printf("vendor_bikes host tests passed (%u protocols)\n", static_cast<unsigned>(descriptorCount()));
}

// Needs registry integration; intentionally not called from runVendorBikesTests().
void runVendorBikesRegistryTests() {
    const struct {
        const char* name;
        const char* id;
    } cases[] = {{"Flywheel 1", "flywheel"},     {"LCB-1234", "sole-bike"},        {"R92-1", "sole-r92"},
                 {"KETTLER C12 01", "kettler-c12"}, {"MyCycling 1", "technogym"},    {"IC123456", "inspire"},
                 {"KEEP_BIKE_01", "keep-bike"},  {"MCF-0001", "mcf-bike"},         {"XCX-1", "xcx-bike"},
                 {"CHRONO BIKE", "chrono-bike"}, {"XQ0123456789", "volava"},       {"X-BIKE 7", "ultrasport"},
                 {"BFCP-1", "skandika-wiri"},    {"HT123456789", "skandika-x2000"}, {"PITPAT-S1", "pitpat-bike"},
                 {"FEAQ51A_123", "freebeat-boom"}, {"MEPANEL-1", "mepanel"},       {"WLT8266BM12", "apex-bike"},
                 {">CABLE 1", "npe-cable"},      {"MAGNUS 1", "cycleops"},         {"JOROTO-BK-1", "csc-bike"},
                 {"RQ123", "renpho"},            {"JFIC1234", "horizon-gr7"}};
    for (const auto& c : cases) {
        const auto* found = detectProtocol(advertisement(c.name));
        assert(found && std::string(found->id) == c.id);
    }
    const char* manual[] = {"lifefit-ic5", "lifespan-bike"};
    for (const char* id : manual) {
        const auto* found = findProtocol(id);
        assert(found && found->priority == 0 && found->profile == MachineProfile::IndoorBike);
    }
}
