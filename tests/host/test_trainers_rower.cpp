// Synthetic fixtures follow qdomyos-zwift revision
// ea70d7ff64bc8bf508fc6401fd056211e05311b3, cited in trainers_rower.cpp.
// No upstream captured trainer traces are claimed.
#include <cassert>
#include <cmath>
#include <limits>

#include "protocols/trainers_rower.hpp"

using namespace smartfitness;
using namespace smartfitness::protocols;
using ble::Bytes;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 0.01F; }
Bytes take(SourceProtocol& source) {
    ble::GattWrite write;
    assert(source.takeWrite(write));
    return write.payload();
}
void notify(SourceProtocol& source, std::size_t index, const Bytes& value, std::uint32_t now = 1000) {
    source.onNotify(index, value.data(), value.size(), now);
}
void testMatches() {
    ble::Advertisement a;
    for (const auto name : {"Tacx Neo 2T", "TACX FLUX S"}) {
        a.name = name;
        assert(matchesTacxFec(a));
    }
    a.name = "TACX SATORI";
    assert(!matchesTacxFec(a));
    for (const auto name : {"ELITETRAINER", "KU", "QD", "RM"}) {
        a.name = name;
        assert(matchesEliteLegacy(a));
    }
    a.name = "DR";
    assert(matchesEliteDr(a) && !matchesEliteLegacy(a));
    a.name = "MISURO B+";
    assert(matchesEliteMisuro(a) && !matchesEliteLegacy(a));
    a.name = "DFC";
    assert(!matchesEliteLegacy(a) && !matchesWahooKickr(a));
    for (const auto name : {"KICKR SNAP", "KICKR CORE", "KICKR BIKE", "WAHOO KICKR", "KICKR MOVE 123"}) {
        a.name = name;
        assert(matchesWahooKickr(a));
    }
    for (const auto name : {"KICKR RUN", "KICKR HEADWIND", "WAHOO KICKR RUN", "WAHOO KICKR HEADWIND", "KICKR UNKNOWN"}) {
        a.name = name;
        assert(!matchesWahooKickr(a));
    }
    a.name = "PM5 123 ROW";
    assert(matchesConcept2Pm5(a));
    for (const auto name : {"PM5 123 SKI", "PM5 123 BIKE", "PM5 123"}) {
        a.name = name;
        assert(!matchesConcept2Pm5(a));
    }
    a.name = "PM5 123 ROW";
    a.services.push_back(ble::Uuid::from16(0x1826));
    assert(!matchesConcept2Pm5(a));
    a.name = "Tacx Neo";
    assert(!matchesTacxFec(a));
    a.name = "KICKR CORE";
    assert(!matchesWahooKickr(a));
    a.name = "ELITETRAINER";
    assert(!matchesEliteLegacy(a));
    assert(std::string(makeTacxFec()->id()) == "tacx-fec");
    assert(std::string(makeEliteLegacy()->id()) == "elite-legacy");
    assert(std::string(makeEliteDr()->id()) == "elite-dr");
    assert(std::string(makeEliteMisuro()->id()) == "elite-misuro");
    assert(std::string(makeWahooKickr()->id()) == "wahoo-legacy");
    assert(std::string(makeConcept2Pm5()->id()) == "concept2-pm5");
}
void testEncoders() {
    assert(trainerrower::elitePower(250) == (Bytes{0, 0xfa, 0}));
    assert(trainerrower::eliteBrake(25) == (Bytes{1, 50}));
    assert(trainerrower::eliteBrake(200) == (Bytes{1, 200}));
    assert(trainerrower::eliteSimulation(0) == (Bytes{2, 0x20, 0x4e, 100, 50, 127, 100}));
    assert(trainerrower::eliteSimulation(5) == (Bytes{2, 0x14, 0x50, 100, 50, 127, 100}));
    assert(trainerrower::wahooPower(250) == (Bytes{0x42, 0xfa, 0}));
    assert(trainerrower::wahooBrake(0) == (Bytes{0x40, 0xff, 0x3f}));
    assert(trainerrower::wahooBrake(100) == (Bytes{0x40, 0, 0}));
    assert(trainerrower::wahooGrade(0) == (Bytes{0x46, 0xff, 0x7f}));
    assert(trainerrower::tacxPower(133) == (Bytes{0xa4, 9, 0x4e, 5, 0x31, 0xff, 0xff, 0xff, 0xff, 0xff, 0x14, 2, 0x3e}));
    const auto grade = trainerrower::tacxGrade(0);
    assert(grade[9] == 0x20 && grade[10] == 0x4e);
    unsigned sum = 1;
    for (unsigned i = 1; i < 12; ++i) sum += grade[i];
    assert(grade[12] == static_cast<std::uint8_t>(sum));
}
void testTacxAndSensors() {
    TacxFecSource source;
    assert(!source.isTrainer());
    assert(source.plan().subscriptionCount == 4 && source.plan().readCount == 0);
    assert(!source.onConnected(0, 0));
    assert(!source.onConnected(8, 0)); // FE-C control tunnel alone is not telemetry
    assert(source.onConnected(1, 0));
    assert(source.control({Control::Power, 250}, 0));
    ble::GattWrite write;
    assert(source.takeWrite(write));
    assert(write.service == ble::Uuid::fromString(trainerrower::kTacxService));
    assert(write.characteristic == ble::Uuid::fromString(trainerrower::kTacxWrite));
    assert(write.withResponse && write.payload() == trainerrower::tacxPower(250));
    assert(source.control({Control::Resistance, 50}, 0));
    assert(take(source) == trainerrower::tacxGrade(5));
    assert(!source.control({Control::Speed, 10}, 0));
    assert(!source.control({Control::Power, std::numeric_limits<float>::quiet_NaN()}, 0));
    assert(!source.control({Control::Power, 2001}, 0));
    notify(source, 0, {0x20, 0, 0xfa, 0, 0x10, 0, 0, 4}, 0);
    assert(near(source.sample().power.value, 250));
    notify(source, 0, {0x20, 0, 0xfa, 0, 0x11, 0, 0, 8}, 1000);
    assert(near(source.sample().cadence.value, 60));
    assert(!source.sample().speed.valid);
    notify(source, 2, {0, 120}, 1001);
    assert(source.sample().heartRate.value == 120);
    const auto last = source.lastDataMs();
    notify(source, 0, {0x20, 0, 1, 0, 1}, 2000);
    notify(source, 99, {0, 1}, 2000);
    source.onNotify(0, nullptr, 10, 2000);
    assert(source.lastDataMs() == last);
    source.tick(4100);
    assert(source.sample().cadence.value == 0);
    source.onWriteFailed(4200);
    assert(source.failed() && !source.isTrainer() && !source.control({Control::Power, 100}, 0));
    source.reset();
    assert(!source.failed() && !source.hasData() && source.pendingWrites() == 0 && !source.isTrainer());
}
void testElite() {
    EliteTrainerSource source;
    assert(source.onConnected(1, 10));
    assert(take(source) == (Bytes{1, 0xad}));
    assert(!source.isTrainer() && !source.control({Control::Power, 200}, 0));
    source.tick(2009);
    assert(source.pendingWrites() == 0);
    source.tick(2010);
    ble::GattWrite write;
    assert(source.takeWrite(write) && write.characteristic == ble::Uuid::fromString(trainerrower::kEliteConfig));
    assert(write.payload() == (Bytes{0x45, 0x0a}));
    assert(source.isTrainer());
    assert(source.control({Control::Incline, 5}, 2011));
    assert(take(source) == trainerrower::eliteSimulation(5));
    assert(source.control({Control::Power, 200}, 2011));
    assert(take(source) == (Bytes{0, 0xc8, 0}));
    for (unsigned i = 0; i < 16; ++i) assert(source.control({Control::Resistance, 50}, 0));
    assert(!source.control({Control::Resistance, 50}, 0));
    source.onWriteFailed(0);
    assert(!source.isTrainer() && source.failed());
    source.reset();
    assert(source.pendingWrites() == 0 && !source.hasData());
    EliteTrainerSource dr(true);
    assert(dr.onConnected(1, 0) && dr.pendingWrites() == 0);
    assert(dr.capabilities().supports(Control::Power) && dr.capabilities().supports(Control::Resistance));
    assert(!dr.capabilities().supports(Control::Incline) && !dr.control({Control::Incline, 2}, 0));
    EliteTrainerSource passive(false, true);
    assert(passive.onConnected(2, 0) && !passive.isTrainer() && passive.pendingWrites() == 0);
    notify(passive, 1, {2, 1, 0, 0, 4}, 0);
    notify(passive, 1, {2, 2, 0, 0, 8}, 1000);
    assert(near(passive.sample().cadence.value, 60) && !passive.sample().power.valid);
}
void readyWahoo(WahooKickrSource& source, std::uint32_t start = 0) {
    assert(source.onConnected(9, start));
    assert(take(source) == (Bytes{0x20, 0xee, 0xfc}));
    notify(source, 3, {1, 0x20, 1}, start + 100);
    source.tick(start + 699);
    assert(source.pendingWrites() == 0);
    source.tick(start + 700);
    assert(take(source) == (Bytes{0x43, 0x4c, 0x1d, 4, 0, 0x90, 1}));
    notify(source, 3, {1, 0x43, 1}, start + 800);
    source.tick(start + 1400);
    assert(source.isTrainer());
}
void testWahoo() {
    WahooKickrSource source;
    assert(source.plan().subscriptions[3].indicate);
    assert(!source.onConnected(1, 0));
    readyWahoo(source);
    assert(source.control({Control::Power, 250}, 1500));
    assert(take(source) == trainerrower::wahooPower(250));
    assert(source.control({Control::Incline, -10}, 1600));
    assert(take(source)[0] == 0x43);
    assert(take(source) == trainerrower::wahooGrade(-10));
    notify(source, 3, {1, 0x46, 2}, 1700);
    assert(source.failed() && !source.isTrainer());
    source.reset();
    readyWahoo(source, 0xfffffc00U);
    source.onWriteFailed(0);
    assert(source.failed() && !source.isTrainer());
    source.reset();
    assert(source.onConnected(9, 0));
    take(source);
    notify(source, 0, {0, 0, 100, 0}, 100);
    source.tick(3000);
    assert(source.failed() && !source.isTrainer()); // telemetry is not unlock ACK
    source.reset();
    assert(source.onConnected(9, 0));
    take(source);
    notify(source, 3, {1, 0x20, 2});
    assert(source.failed());
}
void testPm5() {
    Concept2Pm5Source source;
    assert(!source.onConnected(2, 0));
    assert(source.onConnected(7, 0));
    assert(source.pendingWrites() == 0 && !source.isTrainer());
    assert(!source.control({Control::Resistance, 50}, 0));
    Bytes status(10);
    status[9] = 1;
    notify(source, 0, status);
    Bytes additional{0, 0, 0, 0xe8, 3, 24, 140};
    notify(source, 1, additional, 1100);
    assert(near(source.sample().speed.value, 3.6F) && source.sample().cadence.value == 24);
    assert(source.sample().heartRate.value == 140);
    Bytes power{0, 0, 0, 0xfa, 0, 0, 0, 1, 0};
    notify(source, 2, power, 1200);
    assert(source.sample().power.value == 250);
    additional[6] = 0xff;
    notify(source, 1, additional, 1300);
    assert(!source.sample().heartRate.valid);
    status[9] = 0;
    notify(source, 0, status, 1400);
    assert(source.sample().power.value == 0 && source.sample().speed.value == 0 && source.sample().cadence.value == 0);
    notify(source, 2, power, 1500);
    assert(source.sample().power.value == 0);
    const auto last = source.lastDataMs();
    notify(source, 1, {0, 0}, 1600);
    source.onNotify(0, nullptr, 20, 1600);
    assert(source.lastDataMs() == last);
    source.reset();
    assert(!source.hasData() && source.onConnected(8, 0));
    Bytes multiplexStatus(20);
    multiplexStatus[0] = 0x31;
    multiplexStatus[10] = 1;
    multiplexStatus[19] = 120;
    notify(source, 3, multiplexStatus);
    assert(source.sample().resistance.value == 120);
    Bytes multiplexAdditional(19);
    multiplexAdditional[0] = 0x32;
    multiplexAdditional[4] = 0xd0;
    multiplexAdditional[5] = 7;
    multiplexAdditional[6] = 30;
    multiplexAdditional[7] = 150;
    notify(source, 3, multiplexAdditional, 1100);
    assert(near(source.sample().speed.value, 7.2F) && source.sample().cadence.value == 30);
    Bytes multiplexPower(16);
    multiplexPower[0] = 0x36;
    multiplexPower[4] = 0x2c;
    multiplexPower[5] = 1;
    notify(source, 3, multiplexPower, 1200);
    assert(source.sample().power.value == 300);
    notify(source, 2, power, 1300); // don't mix simultaneous independent and multiplex data
    assert(source.sample().power.value == 300 && source.lastDataMs() == 1200);
    for (std::size_t n = 0; n < multiplexPower.size(); ++n) {
        source.onNotify(3, multiplexPower.data(), n, 1400);
        assert(source.lastDataMs() == 1200);
    }
    assert(source.capabilities().supports(Metric::Power) && source.capabilities().controls == 0);
}
}

void runTrainersRowerTests() {
    testMatches();
    testEncoders();
    testTacxAndSensors();
    testElite();
    testWahoo();
    testPm5();
}
