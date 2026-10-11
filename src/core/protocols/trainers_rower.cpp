// BLE ports derived from qdomyos-zwift, Copyright Roberto Viola and contributors,
// GPL-3.0, revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{tacxneo2/tacxneo2,stagesbike/stagesbike,
// wahookickrsnapbike/wahookickrsnapbike,concept2skierg/concept2skierg,
// ftmsrower/ftmsrower,bluetooth}.cpp. No ANT hardware transport is used.
#include "protocols/trainers_rower.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace smartfitness {
namespace protocols {
namespace {
using ble::Bytes;
ble::Uuid uuid(const char* value) { return ble::Uuid::fromString(value); }
std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
float bounded(float value, float low, float high) {
    return std::isfinite(value) ? std::max(low, std::min(high, value)) : low;
}
Bytes word(std::uint8_t opcode, unsigned value) {
    return {opcode, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8)};
}
std::string upper(const std::string& value) {
    std::string result = value;
    for (auto& c : result) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}
bool starts(const std::string& value, const char* prefix) { return value.find(prefix) == 0; }
TrainerCapabilities caps(std::uint32_t metrics, bool ready, bool resistance, bool incline, bool power) {
    TrainerCapabilities result;
    result.metrics = metrics;
    result.setControl(Control::Resistance, ready && resistance);
    result.setControl(Control::Incline, ready && incline);
    result.setControl(Control::Power, ready && power);
    return result;
}
bool validRequest(const ControlRequest& r) {
    if (!std::isfinite(r.value)) return false;
    if (r.control == Control::Power) return r.value >= 0 && r.value <= 2000;
    if (r.control == Control::Incline) return r.value >= -20 && r.value <= 25;
    if (r.control == Control::Resistance) return r.value >= 0 && r.value <= 100;
    return false;
}
}

namespace trainerrower {
const char* const kTacxService = "6e40fec1-b5a3-f393-e0a9-e50e24dcca9e";
const char* const kTacxNotify = "6e40fec2-b5a3-f393-e0a9-e50e24dcca9e";
const char* const kTacxWrite = "6e40fec3-b5a3-f393-e0a9-e50e24dcca9e";
const char* const kEliteService = "347b0001-7635-408b-8918-8ff3949ce592";
const char* const kEliteWrite = "347b0010-7635-408b-8918-8ff3949ce592";
const char* const kEliteConfig = "347b0018-7635-408b-8918-8ff3949ce592";
const char* const kWahooControl = "a026e005-0a7d-4ab3-97fa-f1500f9feb8b";
const char* const kConcept2Service = "ce060030-43e5-11e4-916c-0800200c9a66";

Bytes tacxPower(float watts) {
    Bytes frame{0xa4, 0x09, 0x4e, 0x05, 0x31, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0};
    const unsigned n = static_cast<unsigned>(bounded(watts, 0, 2000) * 4);
    frame[10] = static_cast<std::uint8_t>(n);
    frame[11] = static_cast<std::uint8_t>(n >> 8);
    for (std::size_t i = 0; i < 12; ++i) frame[12] ^= frame[i];
    return frame;
}
Bytes tacxGrade(float percent) {
    Bytes frame{0xa4, 0x09, 0x4e, 0x05, 0x33, 0xff, 0xff, 0xff, 0xff, 0, 0, 0xff, 0};
    const unsigned n = static_cast<unsigned>((bounded(percent, -20, 25) + 200) * 100);
    frame[9] = static_cast<std::uint8_t>(n);
    frame[10] = static_cast<std::uint8_t>(n >> 8);
    // Preserve upstream's additive grade checksum (power uses XOR).
    for (std::size_t i = 1; i < 12; ++i) frame[12] += frame[i];
    ++frame[12];
    return frame;
}
Bytes elitePower(float watts) { return word(0, static_cast<unsigned>(bounded(watts, 0, 2000))); }
Bytes eliteBrake(float percent) { return {1, static_cast<std::uint8_t>(std::round(bounded(percent, 0, 100) * 2))}; }
Bytes eliteSimulation(float percent) {
    // Upstream setSimulationMode takes fractional grade, then multiplies by 100.
    Bytes result = word(2, static_cast<unsigned>((bounded(percent, -20, 25) + 200) * 100));
    result.insert(result.end(), {100, 50, 127, 100});
    return result;
}
Bytes wahooPower(float watts) { return word(0x42, static_cast<unsigned>(bounded(watts, 0, 2000))); }
Bytes wahooBrake(float percent) {
    return word(0x40, static_cast<unsigned>((1 - bounded(percent, 0, 100) / 100.0F) * 16383));
}
Bytes wahooGrade(float percent) {
    return word(0x46, static_cast<unsigned>((bounded(percent, -20, 25) / 100.0 + 1) * 65535 / 2));
}
}

ble::ConnectPlan TrainerSensorSource::sensorPlan() {
    ble::ConnectPlan result;
    result.subscribe(ble::Uuid::from16(0x1818), ble::Uuid::from16(0x2a63));
    result.subscribe(ble::Uuid::from16(0x1816), ble::Uuid::from16(0x2a5b));
    result.subscribe(ble::Uuid::from16(0x180d), ble::Uuid::from16(0x2a37));
    // Upstream has no mandatory characteristic reads for these profiles.
    return result;
}
bool TrainerSensorSource::sensorNotify(std::size_t index, const std::uint8_t* data, std::size_t length,
                                       std::uint32_t nowMs) {
    expireSamples(nowMs, 3000);
    SourceSample record;
    if (index == 0) {
        sensors::CyclingPowerMeasurement value;
        if (!sensors::decodeCyclingPower(data, length, value)) return false;
        record.power = {static_cast<float>(value.power), true};
        if (!cscCadence_) {
            sample_.cadence = {};
            if (value.crank.present) {
                record.cadence = {crank_.update(value.crank, nowMs), crankKnown_};
                crankKnown_ = true;
            }
        }
        if (!cscSpeed_) {
            sample_.speed = {};
            if (value.wheel.present) {
                record.speed = {sensors::wheelSpeedKmh(powerWheel_.update(value.wheel, nowMs)), powerWheelKnown_};
                powerWheelKnown_ = true;
            }
        }
    } else if (index == 1) {
        sensors::CscMeasurement value;
        if (!sensors::decodeCsc(data, length, value)) return false;
        if (value.crank.present) {
            cscCadence_ = true;
            record.cadence = {crank_.update(value.crank, nowMs), crankKnown_};
            crankKnown_ = true;
        }
        if (value.wheel.present) {
            cscSpeed_ = true;
            record.speed = {sensors::wheelSpeedKmh(cscWheel_.update(value.wheel, nowMs)), cscWheelKnown_};
            cscWheelKnown_ = true;
        }
    } else if (index == 2) {
        std::uint16_t value;
        if (!sensors::decodeHeartRate(data, length, value)) return false;
        record.heartRate = {static_cast<float>(value), true};
    } else return false;
    commitFragment(record, nowMs, static_cast<std::uint8_t>(index));
    return true;
}
void TrainerSensorSource::reset() {
    SourceProtocol::reset();
    crank_.reset();
    cscWheel_.reset();
    powerWheel_.reset();
    cscCadence_ = cscSpeed_ = false;
    crankKnown_ = cscWheelKnown_ = powerWheelKnown_ = false;
}
void TrainerSensorSource::tick(std::uint32_t nowMs) {
    expireSamples(nowMs, 3000);
}

ble::ConnectPlan TacxFecSource::plan() const {
    auto result = sensorPlan();
    result.subscribe(uuid(trainerrower::kTacxService), uuid(trainerrower::kTacxNotify));
    return result;
}
bool TacxFecSource::onConnected(std::uint32_t mask, std::uint32_t) {
    connected_ = (mask & 3U) != 0;
    if (!connected_) setError("Tacx CPS/CSC telemetry unavailable");
    return connected_;
}
void TacxFecSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    // Upstream tacxneo2 uses standard CPS/CSC/HR for telemetry; the custom
    // FE-C characteristic carries the control tunnel, not guessed ANT pages.
    if (index < 3) sensorNotify(index, data, length, nowMs);
}
bool TacxFecSource::control(const ControlRequest& r, std::uint32_t) {
    if (!connected_ || failed() || !validRequest(r)) return false;
    Bytes frame;
    if (r.control == Control::Power) frame = trainerrower::tacxPower(r.value);
    else if (r.control == Control::Incline) frame = trainerrower::tacxGrade(r.value);
    // tacxneo2::update converts its 0..100 resistance request to grade / 10.
    else if (r.control == Control::Resistance) frame = trainerrower::tacxGrade(r.value / 10);
    else return false;
    return queueWrite(uuid(trainerrower::kTacxService), uuid(trainerrower::kTacxWrite), frame);
}
TrainerCapabilities TacxFecSource::capabilities() const { return caps(observedMetrics(), connected_ && !failed(), true, true, true); }
void TacxFecSource::reset() { TrainerSensorSource::reset(); connected_ = false; }
void TacxFecSource::onWriteFailed(std::uint32_t) { connected_ = false; fail("Tacx FE-C write failed; FTMS-only variants need FTMS"); }

ble::ConnectPlan EliteTrainerSource::plan() const { return sensorPlan(); }
bool EliteTrainerSource::onConnected(std::uint32_t mask, std::uint32_t nowMs) {
    connected_ = (mask & 3U) != 0;
    if (!connected_) { setError("Elite CPS/CSC telemetry unavailable"); return false; }
    initMs_ = nowMs;
    ready_ = brakeOnly_ || passive_;
    if (!ready_) return queueWrite(uuid(trainerrower::kEliteService), uuid(trainerrower::kEliteWrite), {1, 0xad});
    return true;
}
void EliteTrainerSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    sensorNotify(index, data, length, nowMs);
}
void EliteTrainerSource::tick(std::uint32_t nowMs) {
    TrainerSensorSource::tick(nowMs);
    if (connected_ && !ready_ && !failed() && nowMs - initMs_ >= 2000)
        ready_ = queueWrite(uuid(trainerrower::kEliteService), uuid(trainerrower::kEliteConfig), {0x45, 0x0a});
}
bool EliteTrainerSource::control(const ControlRequest& r, std::uint32_t) {
    if (!capabilities().supports(r.control) || !validRequest(r)) return false;
    Bytes frame;
    if (r.control == Control::Power) frame = trainerrower::elitePower(r.value);
    else if (r.control == Control::Resistance) frame = trainerrower::eliteBrake(r.value);
    else if (r.control == Control::Incline) frame = trainerrower::eliteSimulation(r.value);
    else return false;
    return queueWrite(uuid(trainerrower::kEliteService), uuid(trainerrower::kEliteWrite), frame);
}
TrainerCapabilities EliteTrainerSource::capabilities() const {
    return caps(observedMetrics(), connected_ && ready_ && !passive_ && !failed(), true, !brakeOnly_, true);
}
void EliteTrainerSource::reset() { TrainerSensorSource::reset(); connected_ = ready_ = false; initMs_ = 0; }
void EliteTrainerSource::onWriteFailed(std::uint32_t) { ready_ = false; fail("Elite proprietary control unavailable"); }

ble::ConnectPlan WahooKickrSource::plan() const {
    auto result = sensorPlan();
    result.subscribe(ble::Uuid::from16(0x1818), uuid(trainerrower::kWahooControl), true);
    return result;
}
bool WahooKickrSource::onConnected(std::uint32_t mask, std::uint32_t nowMs) {
    if ((mask & 9U) != 9U) { setError("KICKR CPS/proprietary control unavailable"); return false; }
    stage_ = 1;
    stageMs_ = nowMs;
    acknowledged_ = false;
    return queueWrite(ble::Uuid::from16(0x1818), uuid(trainerrower::kWahooControl), {0x20, 0xee, 0xfc});
}
void WahooKickrSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (index < 3) { sensorNotify(index, data, length, nowMs); return; }
    if (index != 3 || !data || length < 3 || data[0] != 1) return;
    const unsigned expected = stage_ == 1 ? 0x20 : stage_ == 2 ? 0x43 : 0;
    if (expected && data[1] == expected) {
        if (data[2] != 1) { stage_ = 0; fail("KICKR initialization rejected"); }
        else acknowledged_ = true;
    } else if (stage_ == 3 && data[2] != 1) {
        stage_ = 0;
        fail("KICKR control rejected");
    }
}
void WahooKickrSource::tick(std::uint32_t nowMs) {
    TrainerSensorSource::tick(nowMs);
    if (stage_ != 1 && stage_ != 2) return;
    if (!acknowledged_ && nowMs - stageMs_ >= 3000) { stage_ = 0; fail("KICKR initialization timeout"); return; }
    if (!acknowledged_ || nowMs - stageMs_ < 700) return;
    if (stage_ == 1) {
        // Default 75 kg, CRR .004, wind coefficient .4, as upstream init.
        if (!queueWrite(ble::Uuid::from16(0x1818), uuid(trainerrower::kWahooControl), {0x43, 0x4c, 0x1d, 4, 0, 0x90, 1})) return;
        stage_ = 2;
        stageMs_ = nowMs;
        acknowledged_ = false;
    } else {
        stage_ = 3;
        simulation_ = true;
        // Keep the physical factory wheel circumference: upstream's extra
        // circumference write is a user-configured virtual gearing override.
    }
}
bool WahooKickrSource::control(const ControlRequest& r, std::uint32_t) {
    if (stage_ != 3 || failed() || !validRequest(r)) return false;
    Bytes frame;
    if (r.control == Control::Power) frame = trainerrower::wahooPower(r.value);
    else if (r.control == Control::Resistance) frame = trainerrower::wahooBrake(r.value);
    else if (r.control == Control::Incline) {
        if (!simulation_) {
            if (pendingWrites() > kMaxOutbox - 2) return false;
            if (!queueWrite(ble::Uuid::from16(0x1818), uuid(trainerrower::kWahooControl), {0x43, 0x4c, 0x1d, 4, 0, 0x90, 1})) return false;
        }
        frame = trainerrower::wahooGrade(r.value);
    } else return false;
    if (!queueWrite(ble::Uuid::from16(0x1818), uuid(trainerrower::kWahooControl), frame)) return false;
    simulation_ = r.control == Control::Incline;
    return true;
}
TrainerCapabilities WahooKickrSource::capabilities() const { return caps(observedMetrics(), stage_ == 3 && !failed(), true, true, true); }
void WahooKickrSource::reset() { TrainerSensorSource::reset(); stage_ = stageMs_ = 0; acknowledged_ = simulation_ = false; }
void WahooKickrSource::onWriteFailed(std::uint32_t) { stage_ = 0; fail("KICKR proprietary write failed"); }

ble::ConnectPlan Concept2Pm5Source::plan() const {
    ble::ConnectPlan result;
    for (const char* characteristic : {"ce060031-43e5-11e4-916c-0800200c9a66",
                                     "ce060032-43e5-11e4-916c-0800200c9a66",
                                     "ce060036-43e5-11e4-916c-0800200c9a66",
                                     "ce060080-43e5-11e4-916c-0800200c9a66"})
        result.subscribe(uuid(trainerrower::kConcept2Service), uuid(characteristic));
    return result;
}
bool Concept2Pm5Source::onConnected(std::uint32_t mask, std::uint32_t) {
    multiplex_ = (mask & 8U) != 0;
    if (!multiplex_ && (mask & 7U) != 7U) { setError("PM5 rowing characteristics unavailable"); return false; }
    return true;
}
void Concept2Pm5Source::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (!data || !length || index > 3 || (multiplex_ && index != 3)) return;
    std::uint8_t page;
    bool multiplex = index == 3;
    if (multiplex) {
        page = data[0];
        const std::size_t minimum = page == 0x31 ? 20 : page == 0x32 ? 19 : page == 0x36 ? 16 : 0;
        if (!minimum || length < minimum) return;
        ++data;
        --length;
    } else {
        page = index == 0 ? 0x31 : index == 1 ? 0x32 : 0x36;
        const std::size_t minimum = index == 0 ? 10 : index == 1 ? 7 : 9;
        if (length < minimum) return;
    }
    expireSamples(nowMs, 3000);
    SourceSample record;
    if (page == 0x31) {
        active_ = data[9] != 0;
        sample_.resistance = {};
        if (multiplex && length >= 19) record.resistance = {static_cast<float>(data[18]), true};
    } else if (page == 0x32) {
        record.speed = {u16(data + 3) * 0.0036F, true};
        record.cadence = {static_cast<float>(data[5]), true};
        sample_.heartRate = {};
        if (data[6] != 0xff) record.heartRate = {static_cast<float>(data[6]), true};
    } else {
        record.power = {static_cast<float>(u16(data + 3)), true};
    }
    commitFragment(record, nowMs, page == 0x31 ? 0 : page == 0x32 ? 1 : 2);
}
TrainerCapabilities Concept2Pm5Source::capabilities() const { return caps(observedMetrics(), false, false, false, false); }
void Concept2Pm5Source::reset() { SourceProtocol::reset(); multiplex_ = false; active_ = true; }

bool matchesTacxFec(const ble::Advertisement& a) {
    if (a.hasService(0x1826)) return false;
    const auto name = upper(a.name);
    return starts(name, "TACX NEO") || starts(name, "TACX FLUX") ||
           a.hasService(uuid(trainerrower::kTacxService));
}
bool matchesEliteLegacy(const ble::Advertisement& a) {
    if (a.hasService(0x1826)) return false;
    const auto name = upper(a.name);
    return !matchesEliteDr(a) && !matchesEliteMisuro(a) &&
           (starts(name, "ELITETRAINER") || name == "KU" || name == "QD" || name == "RM" ||
            a.hasService(uuid(trainerrower::kEliteService)));
}
bool matchesEliteDr(const ble::Advertisement& a) { return !a.hasService(0x1826) && upper(a.name) == "DR"; }
bool matchesEliteMisuro(const ble::Advertisement& a) { return !a.hasService(0x1826) && starts(upper(a.name), "MISURO B+"); }
bool matchesWahooKickr(const ble::Advertisement& a) {
    if (a.hasService(0x1826)) return false;
    const auto name = upper(a.name);
    if (starts(name, "KICKR RUN") || starts(name, "WAHOO KICKR RUN") ||
        starts(name, "KICKR HEADWIND") || starts(name, "WAHOO KICKR HEADWIND")) return false;
    return starts(name, "KICKR SNAP") || starts(name, "KICKR BIKE") ||
           starts(name, "KICKR ROLLR") || starts(name, "KICKR CORE") ||
           starts(name, "KICKR MOVE ") || starts(name, "WAHOO KICKR");
}
bool matchesConcept2Pm5(const ble::Advertisement& a) {
    if (a.hasService(0x1826)) return false;
    const auto name = upper(a.name);
    if (name.find("SKI") != std::string::npos || name.find("BIKE") != std::string::npos) return false;
    const bool rowName = starts(name, "PM5") && name.size() >= 3 && name.compare(name.size() - 3, 3, "ROW") == 0;
    return rowName || (name.empty() && a.hasService(uuid(trainerrower::kConcept2Service)));
}
std::unique_ptr<SourceProtocol> makeTacxFec() { return std::unique_ptr<SourceProtocol>(new TacxFecSource); }
std::unique_ptr<SourceProtocol> makeEliteLegacy() { return std::unique_ptr<SourceProtocol>(new EliteTrainerSource); }
std::unique_ptr<SourceProtocol> makeEliteDr() { return std::unique_ptr<SourceProtocol>(new EliteTrainerSource(true)); }
std::unique_ptr<SourceProtocol> makeEliteMisuro() { return std::unique_ptr<SourceProtocol>(new EliteTrainerSource(false, true)); }
std::unique_ptr<SourceProtocol> makeWahooKickr() { return std::unique_ptr<SourceProtocol>(new WahooKickrSource); }
std::unique_ptr<SourceProtocol> makeConcept2Pm5() { return std::unique_ptr<SourceProtocol>(new Concept2Pm5Source); }

}  // namespace protocols
}  // namespace smartfitness
