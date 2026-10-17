// Host tests for the BLE input side: value types, bounded queue, FTMS input
// codec, sensor codecs, vendor protocol ports, adapter registry and the
// BleInputAdapter connection lifecycle (fake transport + fake clock).
//
// Vendor byte samples: qdomyos-zwift ships no captured traces, so frames are
// built from the field layouts its parsers read and from the literal init and
// command bytes in its sources (cited per test).

#include <cassert>
#include <cmath>
#include <map>
#include <optional>
#include <thread>

#include "ant_output.h"
#include "ble/bounded_queue.hpp"
#include "ble/connection_session.hpp"
#include "ble_input_adapter.h"
#include "bridge/bridge_core.hpp"
#include "config_store.h"
#include "fake_central.hpp"
#include "ftms/ftms_codec.hpp"
#include "ftms_output.h"
#include "protocols/domyos.hpp"
#include "protocols/echelon.hpp"
#include "protocols/ftms_source.hpp"
#include "protocols/ifit_bowflex.hpp"
#include "protocols/registry.hpp"
#include "protocols/sensor_codec.hpp"
#include "protocols/sensor_sources.hpp"
#include "protocols/treadmills.hpp"
#include "protocols/yesoul_keiser.hpp"
#include "scan_label.h"
#include "service_names.h"

using namespace smartfitness;
using ble::Bytes;
using testing::FakeCentral;
using testing::address;

namespace {

bool near(float actual, float expected, float tolerance = 0.01F) { return std::fabs(actual - expected) <= tolerance; }
// Optional telemetry values only match when they were actually transmitted.
bool near(const std::optional<float>& actual, float expected, float tolerance = 0.01F) {
    return actual.has_value() && near(*actual, expected, tolerance);
}

void testBleTypes() {
    ble::Uuid uuid;
    assert(ble::Uuid::parse("0x1826", uuid) && uuid.isShort() && uuid.shortValue() == 0x1826);
    assert(uuid.toString() == "0x1826");
    assert(uuid.toCanonicalString() == "00001826-0000-1000-8000-00805f9b34fb");
    assert(ble::Uuid::parse("00001826-0000-1000-8000-00805F9B34FB", uuid) && uuid == ble::Uuid::from16(0x1826));
    assert(ble::Uuid::parse("0bf669f1-45f2-11e7-9598-0800200c9a66", uuid) && !uuid.isShort());
    assert(uuid.toString() == "0bf669f1-45f2-11e7-9598-0800200c9a66");
    assert(!ble::Uuid::parse("0bf669f1-45f2-11e7-9598-0800200c9a6", uuid));
    assert(!ble::Uuid::parse("18x6", uuid));

    ble::BleAddress addr;
    assert(ble::BleAddress::parse("AA:bb:01:02:03:04", addr, 1));
    assert(addr.toString() == "aa:bb:01:02:03:04" && addr.type == 1);
    assert(!ble::BleAddress::parse("aa:bb:01:02:03", addr));
    assert(!ble::BleAddress::parse("aa-bb-01-02-03-04", addr));
    assert(!ble::BleAddress::parse("aa:bb:01:02:03:04", addr, 4));

    ble::ScanReport report;
    report.setName(std::string("Bike\x01name-that-is-definitely-longer-than-31"));
    assert(report.nameLength == ble::kMaxAdvertisedName);
    assert(report.nameString()[4] == '?');
    for (std::uint16_t value = 0; value < 10; ++value) report.addService(ble::Uuid::from16(value));
    assert(report.serviceCount == ble::kMaxAdvertisedServices);
    const Bytes serviceData{1, 1, 0};
    report.setFtmsServiceData(serviceData.data(), serviceData.size());
    assert(report.ftmsServiceDataLength == 3);
    report.setFtmsServiceData(nullptr, 3);
    assert(report.ftmsServiceDataLength == 0 && ble::Advertisement::from(report).ftmsServiceData.empty());

    ble::ConnectPlan plan;
    for (std::size_t index = 0; index < ble::kMaxSubscriptions; ++index)
        assert(plan.subscribe(ble::Uuid::from16(1), ble::Uuid::from16(2)));
    assert(!plan.subscribe(ble::Uuid::from16(1), ble::Uuid::from16(2)));
}

void testBoundedQueue() {
    BoundedQueue<int, 3> queue;
    assert(queue.push(1) && queue.push(2) && queue.push(3));
    assert(!queue.push(4));
    assert(queue.dropped() == 1 && queue.size() == 3);
    int value = 0;
    assert(queue.pop(value) && value == 1);
    assert(queue.push(5));
    assert(queue.pop(value) && value == 2);
    assert(queue.pop(value) && value == 3);
    assert(queue.pop(value) && value == 5);
    assert(!queue.pop(value));

    // Concurrent producers never exceed the capacity and never block.
    BoundedQueue<int, 64> shared;
    std::thread first([&shared] {
        for (int i = 0; i < 1000; ++i) shared.push(i);
    });
    std::thread second([&shared] {
        for (int i = 0; i < 1000; ++i) shared.push(i);
    });
    first.join();
    second.join();
    assert(shared.size() == 64 && shared.dropped() == 2000 - 64);
}

void testFtmsInputCodec() {
    ftms::MachineData data;
    // Indoor Bike Data, flags 0x0274: speed, cadence, distance, resistance, power, HR.
    const Bytes bike{0x74, 0x02, 0xC4, 0x09, 0xA0, 0x00, 0x10, 0x27, 0x00, 0x14, 0x00, 0x96, 0x00, 0x78};
    assert(ftms::decodeIndoorBikeData(bike.data(), bike.size(), data));
    assert(near(data.speed.value, 25.0F) && near(data.cadence.value, 80.0F) && near(data.power.value, 150.0F));
    assert(near(data.heartRate.value, 120.0F) && near(data.resistance.value, 20.0F) && near(data.distance.value, 10000));
    assert(!data.incline.valid);
    assert(!ftms::decodeIndoorBikeData(bike.data(), bike.size() - 1, data));
    assert(!data.power.valid);
    // "More Data" set: no speed field.
    const Bytes moreData{0x41, 0x00, 0xC8, 0x00};
    assert(ftms::decodeIndoorBikeData(moreData.data(), moreData.size(), data));
    assert(!data.speed.valid && near(data.power.value, 200.0F));
    // Round trip with the FTMS output encoder.
    ftms::IndoorBikeData out;
    out.instantaneousSpeed = 3150;
    out.instantaneousPower = -5;
    const Bytes encoded = ftms::encodeIndoorBikeData(out);
    assert(ftms::decodeIndoorBikeData(encoded.data(), encoded.size(), data));
    assert(near(data.speed.value, 31.5F) && near(data.power.value, -5.0F));

    const Bytes treadmill{0x08, 0x00, 0xB0, 0x04, 0xFF, 0xFF, 0x00, 0x00};
    assert(ftms::decodeTreadmillData(treadmill.data(), treadmill.size(), data));
    assert(near(data.speed.value, 12.0F) && near(data.incline.value, -0.1F));

    const Bytes cross{0x08, 0x01, 0x00, 0xE8, 0x03, 0x3C, 0x00, 0x00, 0x00, 0x64, 0x00};
    assert(ftms::decodeCrossTrainerData(cross.data(), cross.size(), data));
    assert(near(data.speed.value, 10.0F) && near(data.cadence.value, 60.0F) && near(data.power.value, 100.0F));

    const Bytes rower{0x28, 0x00, 0x30, 0x10, 0x00, 0x78, 0x00, 0xC8, 0x00};
    assert(ftms::decodeRowerData(rower.data(), rower.size(), data));
    assert(near(data.cadence.value, 24.0F) && near(data.speed.value, 15.0F) && near(data.power.value, 200.0F));

    ftms::Features features;
    const Bytes feature{0x02, 0x44, 0x00, 0x00, 0x0C, 0x20, 0x00, 0x00};
    assert(ftms::decodeFeature(feature.data(), feature.size(), features));
    assert(features.machine == 0x4402 && features.targets == 0x200C);
    assert(!ftms::decodeFeature(feature.data(), 7, features));

    ftms::SupportedRange range;
    const Bytes rangeBytes{0x00, 0x00, 0xE8, 0x03, 0x0A, 0x00};
    assert(ftms::decodeSupportedRange(rangeBytes.data(), rangeBytes.size(), 0.1F, range));
    assert(near(range.minimum, 0) && near(range.maximum, 100.0F) && near(range.increment, 1.0F));
    // The FTMS output's own range encodings decode back to the core bounds.
    const Bytes ownRange = ftms::encodeSupportedInclinationRange();
    assert(ftms::decodeSupportedRange(ownRange.data(), ownRange.size(), 0.1F, range));
    assert(near(range.minimum, kInclineBounds.minimum) && near(range.maximum, kInclineBounds.maximum));

    ftms::ControlPointResponse response;
    const Bytes ok{0x80, 0x00, 0x01};
    assert(ftms::decodeControlPointResponse(ok.data(), ok.size(), response));
    assert(response.requestOpcode == 0x00 && response.result == ftms::ResultCode::Success);
    const Bytes notResponse{0x05, 0x00, 0x01};
    assert(!ftms::decodeControlPointResponse(notResponse.data(), notResponse.size(), response));

    assert(ftms::encodeRequestControl() == Bytes{0x00});
    assert(ftms::encodeStartOrResume() == Bytes{0x07});
    assert(ftms::encodeSetTargetPower(200) == (Bytes{0x05, 0xC8, 0x00}));
    assert(ftms::encodeSetTargetResistance(25.0F) == (Bytes{0x04, 0xFA, 0x00}));
    assert(ftms::encodeSetTargetInclination(-2.5F) == (Bytes{0x03, 0xE7, 0xFF}));
    assert(ftms::encodeSetTargetSpeed(12.0F) == (Bytes{0x02, 0xB0, 0x04}));
    assert(ftms::encodeIndoorBikeSimulation(1.5F) == (Bytes{0x11, 0x00, 0x00, 0x96, 0x00, 0x28, 0x33}));
    // The bridge's own control point decoder accepts what the client encodes.
    const Bytes simulation = ftms::encodeIndoorBikeSimulation(1.5F);
    ftms::ControlPointCommand command;
    assert(ftms::decodeControlPoint(simulation.data(), simulation.size(), command));
    assert(command.status == ftms::ResultCode::Success && near(command.request.value, 1.5F));
}

void testSensorCodecs() {
    sensors::CyclingPowerMeasurement power;
    const Bytes first{0x20, 0x00, 0xFA, 0x00, 0x10, 0x00, 0x00, 0x04};
    const Bytes second{0x20, 0x00, 0xFA, 0x00, 0x11, 0x00, 0x00, 0x08};
    assert(sensors::decodeCyclingPower(first.data(), first.size(), power));
    assert(power.power == 250 && power.crank.present && power.crank.revolutions == 0x10);
    assert(!power.wheel.present);
    assert(!sensors::decodeCyclingPower(first.data(), first.size() - 1, power));
    // Pedal balance (b0) and accumulated torque (b2) precede the crank data.
    const Bytes withOptional{0x25, 0x00, 0x64, 0x00, 0x32, 0x00, 0x01, 0x05, 0x00, 0x00, 0x04};
    assert(sensors::decodeCyclingPower(withOptional.data(), withOptional.size(), power));
    assert(power.power == 100 && power.crank.revolutions == 5 && power.crank.eventTime == 0x0400);

    sensors::RevolutionRate crank(1024, 16);
    sensors::CyclingPowerMeasurement a;
    sensors::CyclingPowerMeasurement b;
    sensors::decodeCyclingPower(first.data(), first.size(), a);
    sensors::decodeCyclingPower(second.data(), second.size(), b);
    assert(crank.update(a.crank, 0) == 0.0F);
    assert(near(crank.update(b.crank, 1000), 60.0F));
    assert(near(crank.update(b.crank, 1500), 60.0F));  // repeated event keeps the rate
    assert(crank.current(4100) == 0.0F);                 // idle > 3 s
    // Counter and timer wraparound: 0xFFFF -> 0x0001 (2 revs) in 2 s.
    crank.reset();
    crank.update({true, 0xFFFF, 0xFC00}, 0);
    assert(near(crank.update({true, 0x0001, 0x0400}, 2000), 60.0F));

    sensors::CscMeasurement csc;
    const Bytes wheel1{0x01, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00};
    const Bytes wheel2{0x01, 0x66, 0x00, 0x00, 0x00, 0x00, 0x04};
    sensors::RevolutionRate wheel(1024, 32);
    assert(sensors::decodeCsc(wheel1.data(), wheel1.size(), csc));
    wheel.update(csc.wheel, 0);
    assert(sensors::decodeCsc(wheel2.data(), wheel2.size(), csc) && !csc.crank.present);
    const float rpm = wheel.update(csc.wheel, 1000);
    assert(near(rpm, 120.0F) && near(sensors::wheelSpeedKmh(rpm), 15.156F));
    const Bytes truncatedCsc{0x03, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    assert(!sensors::decodeCsc(truncatedCsc.data(), truncatedCsc.size(), csc));

    std::uint16_t bpm = 0;
    const Bytes hr8{0x00, 0x48};
    const Bytes hr16{0x01, 0x2C, 0x01};
    assert(sensors::decodeHeartRate(hr8.data(), hr8.size(), bpm) && bpm == 72);
    assert(sensors::decodeHeartRate(hr16.data(), hr16.size(), bpm) && bpm == 300);
    assert(!sensors::decodeHeartRate(hr16.data(), 2, bpm));
    assert(!sensors::decodeHeartRate(nullptr, 0, bpm));
}

void testEchelon() {
    // Frames from echelonconnectsport.cpp btinit()/sendPoll()/forceResistance().
    const auto init = protocols::echelon::initFrames();
    assert(init.size() == 7);
    assert(init[0] == (Bytes{0xf0, 0xa1, 0x00, 0x91}) && init[4] == (Bytes{0xf0, 0xa3, 0x00, 0x93}));
    assert(init[6] == (Bytes{0xf0, 0xb0, 0x01, 0x01, 0xa2}));
    assert(protocols::echelon::pollFrame(1) == (Bytes{0xf0, 0xa0, 0x01, 0x01, 0x92}));
    assert(protocols::echelon::resistanceFrame(10) == (Bytes{0xf0, 0xb1, 0x01, 0x0a, 0xac}));

    protocols::echelon::Frame frame;
    const Bytes resistance{0xf0, 0xd2, 0x01, 0x0a, 0xcd};
    assert(protocols::echelon::decode(resistance.data(), resistance.size(), frame));
    assert(frame.kind == protocols::echelon::Frame::Kind::Resistance && frame.resistance == 10);
    Bytes data(13, 0x00);
    data[0] = 0xf0;
    data[1] = 0xd1;
    data[10] = 85;
    assert(protocols::echelon::decode(data.data(), data.size(), frame) && near(frame.cadence, 85.0F));
    assert(!protocols::echelon::decode(data.data(), 12, frame));

    assert(protocols::echelon::power(10, 0.0F) == 0.0F);
    assert(near(protocols::echelon::power(10, 85.0F), 64.85F));   // 56.2 + (73.5 - 56.2) / 10 * 5
    assert(near(protocols::echelon::power(10, 120.0F), 100.32F)); // 120 / 100 * 83.6
    assert(near(protocols::echelon::power(99, 50.0F), 242.0F));   // clamped to level 32

    protocols::EchelonSource source;
    assert(source.plan().subscriptionCount == 2);
    assert(!source.onConnected(0, 0));
    assert(source.onConnected(0x3, 0));
    assert(source.pendingWrites() == 7);
    ble::GattWrite write;
    while (source.takeWrite(write)) assert(write.characteristic == protocols::echelon::kWrite);
    source.tick(1999);
    assert(source.pendingWrites() == 0);
    source.tick(2000);
    assert(source.takeWrite(write) && write.payload() == protocols::echelon::pollFrame(1));
    source.tick(4000);
    assert(source.takeWrite(write) && write.payload() == protocols::echelon::pollFrame(2));
    assert(source.isTrainer() && source.capabilities().supports(Control::Resistance));
    assert(!source.capabilities().supports(Control::Power));
    assert(source.control({Control::Resistance, 50.0F}, 0));
    assert(source.takeWrite(write) && write.payload() == protocols::echelon::resistanceFrame(17));
    assert(!source.control({Control::Power, 100.0F}, 0));
    source.onNotify(1, resistance.data(), resistance.size(), 10);
    source.onNotify(0, data.data(), data.size(), 20);
    assert(source.hasData() && near(source.sample().cadence.value, 85.0F) && near(source.sample().power.value, 64.85F));
    assert(near(source.sample().speed.value, 0.37497622F * 85.0F) && near(source.sample().resistance.value, 10.0F));
}

Bytes domyosStatus() {
    Bytes frame(26, 0x00);
    frame[0] = 0xf0;
    frame[1] = 0xbc;
    frame[6] = 0x00;
    frame[7] = 0xFA;  // 25.0 km/h
    frame[9] = 80;    // rpm
    frame[12] = 0x00;
    frame[13] = 0x64;  // 10.0 km
    frame[14] = 5;     // resistance
    frame[18] = 130;   // bpm
    return frame;
}

void testDomyos() {
    // Bytes from domyosbike.cpp btinit_changyow() and forceResistance().
    const auto steps = protocols::domyos::initSteps();
    assert(steps.size() == 10);
    assert(steps[0].size() == 1 && steps[0][0] == (Bytes{0xf0, 0xc8, 0x01, 0xb9}));
    assert(steps[7].size() == 2 && steps[7][0].size() == 20 && steps[7][1] == (Bytes{0xff, 0xff, 0x8b}));
    assert(steps[9][1] == (Bytes{0xff, 0xff, 0x94}));
    const auto frames = protocols::domyos::resistanceFrames(5);
    assert(frames.size() == 2 && frames[0].size() == 20 && frames[1].size() == 3);
    assert(frames[0][10] == 5 && frames[0][17] == 0x00 && frames[0][18] == 0x01);
    assert(frames[1] == (Bytes{0xff, 0xff, 0x92}));

    protocols::domyos::FrameAssembler assembler;
    const Bytes full = domyosStatus();
    Bytes frame;
    assert(!assembler.feed(full.data(), 20, frame) && assembler.incomplete());
    assert(assembler.feed(full.data() + 20, 6, frame) && frame == full && !assembler.incomplete());
    assert(assembler.feed(full.data(), full.size(), frame));
    const Bytes ack{0xf0, 0xc8, 0xb8};
    assert(!assembler.feed(ack.data(), ack.size(), frame));

    protocols::domyos::Status status;
    assert(protocols::domyos::decodeStatus(full, status));
    assert(near(status.speed, 25.0F) && near(status.cadence, 80.0F) && status.resistance == 5);
    assert(status.heartRate == 130 && near(status.distance, 10.0F));
    assert(near(protocols::domyos::power(5, 80.0F), 16.19F * std::exp(2.24F), 0.05F));
    assert(protocols::domyos::power(5, 0.0F) == 0.0F);

    protocols::DomyosSource source;
    assert(source.onConnected(1, 0));
    ble::GattWrite write;
    assert(source.takeWrite(write) && write.payload() == steps[0][0]);
    // A 20-byte notification does not acknowledge an init step; retries after 300 ms.
    source.onNotify(0, full.data(), 20, 100);
    assert(source.pendingWrites() == 0);
    source.tick(300);
    assert(source.takeWrite(write) && write.payload() == steps[0][0]);
    for (std::size_t step = 1; step < steps.size(); ++step) {
        source.onNotify(0, ack.data(), ack.size(), 400);
        for (const auto& expected : steps[step]) assert(source.takeWrite(write) && write.payload() == expected);
    }
    assert(!source.initialized() && !source.isTrainer());
    source.onNotify(0, ack.data(), ack.size(), 500);
    assert(source.initialized() && source.isTrainer());
    source.tick(800);
    assert(source.takeWrite(write) && write.payload() == protocols::domyos::noOpFrame());
    source.onNotify(0, full.data(), full.size(), 900);
    assert(near(source.sample().power.value, protocols::domyos::power(5, 80.0F)));
    assert(near(source.sample().heartRate.value, 130.0F));
    assert(source.control({Control::Resistance, 100.0F}, 900));
    assert(source.takeWrite(write) && write.payload()[10] == 15);
    assert(source.takeWrite(write) && write.length == 3);

    protocols::DomyosSource silent;
    assert(silent.onConnected(1, 0));
    for (std::uint32_t now = 300; now <= 3000 && !silent.failed(); now += 300) silent.tick(now);
    assert(silent.failed() && silent.lastError() == "Domyos init step not acknowledged");
    silent.reset();
    assert(!silent.failed());
}

void testYesoulAndKeiser() {
    const Bytes yesoul{0x00, 0x00, 0x03, 0xE8, 0x08, 0x00, 0x5A, 0x00, 0xC8, 0x00, 0x00, 0x00};
    protocols::yesoul::Frame frame;
    assert(protocols::yesoul::decode(yesoul.data(), yesoul.size(), frame));
    assert(frame.resistance == 8 && near(frame.cadence, 90.0F) && near(frame.power, 200.0F) && near(frame.distance, 10.0F));
    assert(!protocols::yesoul::decode(yesoul.data(), 11, frame));
    protocols::YesoulSource source;
    assert(source.onConnected(1, 0));
    ble::GattWrite write;
    assert(source.takeWrite(write) && write.payload() == (Bytes{0xf5, 0x20, 0x20, 0x40, 0xf6}));
    source.onNotify(0, yesoul.data(), yesoul.size(), 5);
    assert(near(source.sample().power.value, 200.0F) && !source.isTrainer());

    // Keiser M3i manufacturer data including the company identifier 0x0102.
    const Bytes keiser{0x02, 0x01, 0x06, 0x30, 0x00, 0x07, 0x52, 0x03, 0x14, 0x05, 0xD2, 0x00,
                       0x10, 0x00, 0x05, 0x1E, 0x64, 0x80, 0x0C};
    protocols::keiser::Broadcast broadcast;
    assert(protocols::keiser::parse(keiser.data(), keiser.size(), broadcast));
    assert(broadcast.systemId == 7 && near(broadcast.cadence, 85.0F) && near(broadcast.heartRate, 130.0F));
    assert(near(broadcast.power, 210.0F) && broadcast.seconds == 330 && near(broadcast.distanceKm, 10.0F));
    assert(broadcast.gear == 12);
    Bytes miles = keiser;
    miles[17] = 0x00;  // 10.0 miles
    assert(protocols::keiser::parse(miles.data(), miles.size(), broadcast) && near(broadcast.distanceKm, 16.0934F));
    Bytes wrongMajor = keiser;
    wrongMajor[2] = 0x05;
    assert(!protocols::keiser::parse(wrongMajor.data(), wrongMajor.size(), broadcast));
    assert(!protocols::keiser::parse(keiser.data(), 17, broadcast));
    Bytes tooLong = keiser;
    tooLong.push_back(0);
    assert(!protocols::keiser::parse(tooLong.data(), tooLong.size(), broadcast));
}

ble::Advertisement advert(const std::string& name, std::vector<std::uint16_t> services = {}, Bytes manufacturer = {}) {
    ble::Advertisement advertisement;
    advertisement.name = name;
    for (auto service : services) advertisement.services.push_back(ble::Uuid::from16(service));
    advertisement.manufacturerData = manufacturer;
    return advertisement;
}

std::string detected(const ble::Advertisement& advertisement) {
    const auto* info = protocols::detectProtocol(advertisement);
    return info ? info->id : "";
}

void testRegistry() {
    assert(detected(advert("ECH-SPORT-1")) == "echelon");
    assert(detected(advert("ECH-SPORT-1", {0x1826})) == "ftms");
    assert(detected(advert("ECH-ROW-1")) == "echelon-rower");
    assert(detected(advert("IC Bike", {0x1826})) == "schwinn-ic4");
    assert(detected(advert("C7-0123456789AB", {0x1826})) == "schwinn-ic4");
    assert(detected(advert("C7-0123456789ABCD", {0x1826})) == "ftms");  // 17 characters
    assert(detected(advert("Domyos-Bike-1234")) == "domyos");
    assert(detected(advert("Domyos-Bike-1234", {0x1826})) == "ftms");
    assert(detected(advert("DomyosBridge")).empty());
    assert(detected(advert("YESOUL S3")) == "yesoul");
    assert(detected(advert("", {}, {0x7d, 0x02, 0x01, 0x05, 0x00, 0xff, 0xff})) == "yesoul");
    const Bytes keiser{0x02, 0x01, 0x06, 0x30, 0x00, 0x07, 0x52, 0x03, 0x14, 0x05, 0xD2, 0x00,
                       0x10, 0x00, 0x05, 0x1E, 0x64, 0x80, 0x0C};
    assert(detected(advert("M3", {}, keiser)) == "keiser-m3i");
    assert(detected(advert("M3")).empty());
    assert(detected(advert("Strap", {0x180D})) == "hrs");
    assert(detected(advert("Pedals", {0x1816, 0x1818})) == "cps");
    assert(detected(advert("Sensor", {0x1816})) == "csc");
    assert(detected(advert("Lamp", {0x1234})).empty());
    // Native FTMS firmware must not be stolen by a legacy vendor adapter.
    for (const char* name : {"Tacx Neo 2T", "TACX FLUX S", "KICKR CORE", "ELITETRAINER",
                             "MISURO B+", "PM5 123 ROW"}) {
        assert(detected(advert(name, {0x1826, 0x1818})) == "ftms");
    }
    assert(detected(advert("Tacx Neo 2T", {0x1818})) == "tacx-fec");
    assert(detected(advert("KICKR CORE", {0x1818})) == "wahoo-legacy");
    assert(detected(advert("ELITETRAINER", {0x1818})) == "elite-legacy");
    assert(detected(advert("DR", {0x1818})) == "elite-dr");
    assert(detected(advert("MISURO B+", {0x1818})) == "elite-misuro");
    assert(detected(advert("PM5 123 ROW")) == "concept2-pm5");
    assert(detected(advert("KICKR RUN")) == "kickr-run");
    assert(detected(advert("PM5 123 SKI")) == "concept2-skierg");
    assert(detected(advert("Peloton Bike")).empty());
    assert(detected(advert("DFC")).empty());
    for (const char* name : {"I_EB123", "I_TL123", "I_EL123", "I_FS123", "I_VE123",
                             "BOWFLEX T216", "SCHWINN 170/270", "XT385", "F80", "R1 PRO", "KS-X21"}) {
        assert(detected(advert(name, {0x1826})) == "ftms");
    }
    auto proprietaryIfit = advert("I_EB123", {0x1826});
    proprietaryIfit.services.push_back(ble::Uuid::fromString("00001533-1412-efde-1523-785feabcd123"));
    assert(detected(proprietaryIfit) == "ifit-bike");
    auto proprietaryBowflex = advert("BOWFLEX T216", {0x1826});
    proprietaryBowflex.services.push_back(ble::Uuid::fromString("edff9e80-cad7-11e5-ab63-0002a5d5c51b"));
    assert(detected(proprietaryBowflex) == "bowflex-t216");

    for (std::size_t index = 0; index < protocols::protocolCount(); ++index) {
        const auto& info = protocols::protocolAt(index);
        auto protocol = info.create();
        assert(protocol && std::string(protocol->id()) == info.id);
        assert(std::string(info.id).size() <= ConfigStore::kMaxAdapterIdLength);
        assert(ConfigStore::isValidAdapterId(info.id));
        assert(protocol->advertisementOnly() == info.advertisementOnly);
        assert(protocols::findProtocol(info.id) == &info);
        const auto plan = protocol->plan();
        assert(plan.subscriptionCount <= ble::kMaxSubscriptions);
        assert(plan.readCount <= ble::kMaxReads);
        for (std::size_t other = index + 1; other < protocols::protocolCount(); ++other)
            assert(std::string(info.id) != protocols::protocolAt(other).id);
    }
    assert(!protocols::createProtocol("unknown"));
}

void testProtocolProfiles() {
    std::map<std::string, MachineProfile> expected{
        {"keiser-m3i", MachineProfile::IndoorBike}, {"echelon", MachineProfile::IndoorBike},
        {"domyos", MachineProfile::IndoorBike}, {"schwinn-ic4", MachineProfile::IndoorBike},
        {"yesoul", MachineProfile::IndoorBike}, {"tacx-fec", MachineProfile::IndoorBike},
        {"elite-legacy", MachineProfile::IndoorBike}, {"elite-dr", MachineProfile::IndoorBike},
        {"elite-misuro", MachineProfile::IndoorBike}, {"wahoo-legacy", MachineProfile::IndoorBike},
        {"concept2-pm5", MachineProfile::Rower}, {"ftms", MachineProfile::IndoorBike},
        {"cps", MachineProfile::IndoorBike}, {"csc", MachineProfile::IndoorBike},
        {"hrs", MachineProfile::IndoorBike}, {"ifit-bike", MachineProfile::IndoorBike},
        {"ifit-treadmill", MachineProfile::Treadmill}, {"ifit-freestride", MachineProfile::CrossTrainer},
        {"ifit-elliptical", MachineProfile::CrossTrainer}, {"ifit-el-trainer", MachineProfile::CrossTrainer},
        {"bowflex-t216", MachineProfile::Treadmill}, {"schwinn-170", MachineProfile::IndoorBike},
        {"sportstech-f15", MachineProfile::Treadmill}
    };
    assert(protocols::ifit_bowflex::modelCount() == 97);
    for (unsigned ordinal = 1; ordinal <= 97; ++ordinal) {
        const auto profile = ordinal <= 23 || ordinal == 97 ? MachineProfile::IndoorBike :
                             ordinal <= 83 || (ordinal >= 91 && ordinal <= 96) ? MachineProfile::Treadmill :
                             MachineProfile::CrossTrainer;
        expected.emplace("ifit-model-" + std::to_string(ordinal), profile);
    }
    for (const char* id : {
             "horizon-tm", "horizon-paragon", "spirit-treadmill", "spirit-xt485", "spirit-xt800",
             "sole-treadmill", "sole-f63", "sole-f65", "sole-tt8", "walkingpad-r1", "walkingpad-flat",
             "walkingpad-re", "walkingpad-g1", "walkingpad-g1b", "walkingpad-r2",
             "wp2-c0-s2c2", "wp2-c0-s1c1",
             "wp2-c2-s0", "wp2-c2-s2c2", "wp2-c2-s1c1",
             "wp2-c3-s0", "wp2-c3-s2c2", "wp2-c3-s1c1",
             "wp2-c4-s0", "wp2-c4-s2c2", "wp2-c4-s1c1",
             "wp2-c5-s0", "wp2-c5-s2c2", "wp2-c5-s1c1",
             "wp2-c6-s0", "wp2-c6-s2c2", "wp2-c6-s1c1",
             "wp2-c7-s0", "wp2-c7-s2c2", "wp2-c7-s1c1"})
        expected.emplace(id, MachineProfile::Treadmill);
    // Ported vendor families (fitshow, toorx, domyos_ext, echelon_ext, nautilus,
    // sportsplus, vendor_treadmills, vendor_bikes, vendor_cardio).
    for (const char* id : {
             "fitshow-tm", "fitshow-noblepro", "fitshow-tm-mi", "fitshow-anyrun", "trx-tm",
             "trx-tm-irun", "trx-tm-reebok", "trx-tm-adidas", "trx-tm-dknrun", "trx-tm-30",
             "trx-tm-jtx", "trx-tm-dkn", "trx-tm-flow", "trx-tm-fr30", "domyos-tm",
             "echelon-stride", "echelon-stride4", "nautilus-tm", "nautilus-t628", "nautilus-tm-688a",
             "bowflex-tm", "pafers-treadmill", "pafers-iboxster", "true-tm", "assault-tm",
             "wdway-tm", "technogym-myrun", "lifespan-tm", "octane-zr7", "octane-zr8",
             "zipro-tm", "esanglinker", "eslinker-costa", "sperax-tm", "focus-tm",
             "deerrun-pitpat", "activio-tm", "activio-v460", "activio-zt2500", "activio-umay",
             "kickr-run", "lifefitness-tm", "shuaa5-tm"})
        expected.emplace(id, MachineProfile::Treadmill);
    for (const char* id : {
             "snode-bike", "fitplus-bike", "fitplus-etappe", "fitplus-merach", "fitplus-h9110",
             "fitplus-sx600", "trx-bike-tc", "trx-bike-tun", "trx-bike-fhw", "trx-bike-vfit",
             "trx-bike-irun", "trx-bike-adc1", "trx-bike-chy", "trx-bike-icon", "trx-bike-casall",
             "trx-bike-reebok", "trx-bike-dkn", "trx-bike-fal", "trx-bike-hertz", "trx-bike-enerfit",
             "trx-bike-hammer", "trx-bike-jll", "trx-bike-fytter", "trx-bike-asviva", "trx-bike-hop",
             "trx-bike-srx500", "trx-bike-taurua", "nautilus-bike", "nautilus-b616-mi", "sportstech-bike",
             "sportsplus-bike", "sportsplus-ht", "carefit-bike", "sp-ht-9600ie", "pafers-bike",
             "flywheel", "lifefit-ic5", "sole-bike", "sole-r92", "kettler-c12",
             "technogym", "inspire", "keep-bike", "mcf-bike", "xcx-bike",
             "chrono-bike", "volava", "ultrasport", "skandika-wiri", "skandika-x2000",
             "pitpat-bike", "freebeat-boom", "mepanel", "apex-bike", "npe-cable",
             "cycleops", "csc-bike", "lifespan-bike", "renpho", "horizon-gr7"})
        expected.emplace(id, MachineProfile::IndoorBike);
    for (const char* id : {
             "fitplus-rower", "trx-row", "trx-row-mrk", "domyos-row", "echelon-rower",
             "sportstech-rower", "sportsplus-rower", "pafers-rower", "proform-rower", "proform-750r",
             "proform-sport-rl", "smartrow", "concept2-skierg"})
        expected.emplace(id, MachineProfile::Rower);
    for (const char* id : {
             "trx-ell", "trx-ell-toorx", "trx-ell-dct", "domyos-el", "nautilus-ellip",
             "nautilus-m7", "nautilus-el-v2", "sportstech-ellip", "bh-elliptical", "ypoo-u3",
             "ypoo-ftms-el", "ypoo-sch411", "ypoo-feier", "ypoo-sole-e25", "ypoo-true-el",
             "ypoo-iconsole", "gymstick-gx60", "sole-elliptical", "xterra-xe88", "sole-e55",
             "octane-q37"})
        expected.emplace(id, MachineProfile::CrossTrainer);
    for (const char* id : {
             "echelon-stair"})
        expected.emplace(id, MachineProfile::StairClimber);
    for (const char* id : {
             "sunnyfit-stepper"})
        expected.emplace(id, MachineProfile::StepClimber);
    assert(expected.size() == protocols::protocolCount());
    for (std::size_t index = 0; index < protocols::protocolCount(); ++index) {
        const auto& info = protocols::protocolAt(index);
        assert(expected.at(info.id) == info.profile);
        auto source = protocols::createProtocol(info.id);
        assert(source && source->profile() == info.profile);
        source->reset();
        assert(source->profile() == info.profile);
        // Enumeration and factory calls must not modify descriptor identities.
        assert(protocols::findProtocol(info.id) == &info);
    }
}

void testFtmsProfiles() {
    using Source = protocols::FtmsSource;
    const std::array<MachineProfile, 6> profiles{{MachineProfile::IndoorBike, MachineProfile::Treadmill,
                                                MachineProfile::CrossTrainer, MachineProfile::Rower,
                                                MachineProfile::StepClimber, MachineProfile::StairClimber}};
    const std::array<std::size_t, 6> subscriptions{{Source::SubBike, Source::SubTreadmill, Source::SubCrossTrainer,
                                                    Source::SubRower, Source::SubStepClimber,
                                                    Source::SubStairClimber}};
    const std::array<Bytes, 6> frames{{{0, 0, 0xC4, 9}, {0, 0, 0xB0, 4},
                                      {0, 0, 0, 0xE8, 3}, {0, 0, 0x30, 0x10, 0},
                                      {0, 0, 5, 0, 0x64, 0}, {0, 0, 3, 0}}};
    const std::array<std::uint8_t, 6> typeBits{{0x20, 1, 2, 0x10, 4, 8}};
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        const std::size_t subscription = subscriptions[index];
        Source source;
        assert(source.onConnected(1U << subscription, 100));
        assert(source.profile() == profiles[index]);
        source.onNotify(subscription, frames[index].data(), frames[index].size(), 200);
        assert(source.profile() == profiles[index] && source.hasData());
        source.reset();
        assert(source.profile() == MachineProfile::IndoorBike && !source.sample().distance.valid);
        ble::Advertisement advertisement;
        advertisement.ftmsServiceData = {1, typeBits[index], 0};
        source.onAdvertisement(advertisement, 300);
        assert(source.profile() == profiles[index]);
        assert(source.onConnected(0x7f, 300));  // Some machines expose every data characteristic.
        assert(source.profile() == profiles[index]);
        for (std::size_t other = 0; other < frames.size(); ++other) {
            source.onNotify(subscriptions[other], frames[other].data(), frames[other].size(), 400);
            assert(source.profile() == profiles[index]);
        }
    }
    Source source;
    ble::Advertisement advertisement;
    for (const Bytes& invalid : {Bytes{1, 1}, Bytes{0, 1, 0}, Bytes{1, 0, 0}, Bytes{1, 3, 0},
                                 Bytes{1, 0x40, 0}, Bytes{1, 0x0c, 0}}) {
        advertisement.ftmsServiceData = invalid;
        source.onAdvertisement(advertisement, 100);
        assert(source.profile() == MachineProfile::IndoorBike);
    }
    // A sole available characteristic overrides contradictory advertising.
    advertisement.ftmsServiceData = {1, 0x20, 0};
    source.onAdvertisement(advertisement, 100);
    assert(source.onConnected(1U << Source::SubTreadmill, 100));
    assert(source.profile() == MachineProfile::Treadmill);
    source.reset();
    assert(source.onConnected(0x0f, 200));
    source.onNotify(Source::SubRower, frames[3].data(), frames[3].size(), 300);
    assert(source.profile() == MachineProfile::Rower);
    const Bytes distance{0x10, 0, 0xC4, 9, 0x10, 0x27, 0};
    source.onNotify(Source::SubBike, distance.data(), distance.size(), 400);
    assert(source.profile() == MachineProfile::Rower && !source.sample().distance.valid);
    assert(near(source.sample().cadence.value, 24));
    advertisement.ftmsServiceData = {1, 0x20, 0};
    source.onAdvertisement(advertisement, 400);
    assert(source.profile() == MachineProfile::IndoorBike && !source.hasData());
    assert(!source.sample().cadence.valid && source.capabilities().metrics == 0);
    source.onNotify(Source::SubBike, distance.data(), distance.size(), 400);
    assert(source.sample().distance.valid && near(source.sample().distance.value, 10000));
    const Bytes moreData{1, 0};
    source.onNotify(Source::SubBike, moreData.data(), moreData.size(), 500);
    assert(near(source.sample().distance.value, 10000));
    auto schwinn = protocols::createProtocol("schwinn-ic4");
    advertisement.ftmsServiceData = {1, 1, 0};
    schwinn->onAdvertisement(advertisement, 100);
    assert(schwinn->onConnected(1, 100));
    assert(schwinn->profile() == MachineProfile::IndoorBike);
}

void testFtmsControlDoesNotStartTreadmill() {
    using Source = protocols::FtmsSource;
    const Bytes feature{0, 0, 0, 0, 0x0c, 0, 0, 0};
    const Bytes granted{0x80, 0, 1};
    for (std::size_t index = Source::SubBike; index <= Source::SubRower; ++index) {
        Source source;
        source.onRead(Source::ReadFeature, feature.data(), feature.size());
        assert(source.onConnected((1U << index) | (1U << Source::SubControlPoint), 100));
        ble::GattWrite write;
        assert(source.pendingWrites() == 1 && source.takeWrite(write));
        assert(write.payload() == ftms::encodeRequestControl());
        assert(!source.takeWrite(write));
        source.onNotify(Source::SubControlPoint, granted.data(), granted.size(), 200);
        assert(source.controlState() == Source::ControlState::Granted);
        if (index == Source::SubBike)
            assert(source.takeWrite(write) && write.payload() == ftms::encodeStartOrResume());
        assert(source.pendingWrites() == 0 && !source.takeWrite(write));
    }
    // Advertising selects treadmill even if the device subscribes every data UUID.
    Source source;
    ble::Advertisement advertisement;
    advertisement.ftmsServiceData = {1, 1, 0};
    source.onAdvertisement(advertisement, 100);
    source.onRead(Source::ReadFeature, feature.data(), feature.size());
    assert(source.onConnected(0x1f, 100));
    ble::GattWrite write;
    assert(source.takeWrite(write) && write.payload() == ftms::encodeRequestControl());
    source.onNotify(Source::SubControlPoint, granted.data(), granted.size(), 200);
    assert(source.profile() == MachineProfile::Treadmill && !source.takeWrite(write));
    // The bike compatibility default is not proof that an ambiguous machine is a bike.
    source.reset();
    source.onRead(Source::ReadFeature, feature.data(), feature.size());
    assert(source.onConnected(0x1f, 300));
    assert(source.takeWrite(write) && write.payload() == ftms::encodeRequestControl());
    source.onNotify(Source::SubControlPoint, granted.data(), granted.size(), 400);
    assert(!source.takeWrite(write));
}

struct Harness {
    std::uint32_t now = 1000;
    FakeCentral central;
    BleInputAdapter adapter{central, [this] { return now; }};
    int emitted = 0;
    FitnessTelemetry last;

    Harness() {
        adapter.onTelemetry([this](const FitnessTelemetry& telemetry) {
            ++emitted;
            last = telemetry;
        });
    }
    void pair(const char* text, const char* adapterId, const char* name = "Device") {
        PairedDevice device;
        device.address = address(text);
        device.adapterId = adapterId;
        device.name = name;
        device.valid = true;
        adapter.setPairedDevice(device);
    }
    void advance(std::uint32_t ms) {
        now += ms;
        adapter.loop();
    }
};

void testPairedServiceDiagnostics() {
    Harness h;
    assert(h.adapter.startScan());
    const auto peer = address("aa:00:00:00:00:01");
    h.central.advertise(peer, "Bike", {0x1826, 0x1818});
    h.central.advertise(peer, "", {0x180d});
    h.adapter.loop();
    h.pair("aa:00:00:00:00:01", "ftms");
    assert(h.adapter.scanAndConnect());
    assert(h.adapter.advertisedServices() ==
           (std::vector<ble::Uuid>{ble::Uuid::from16(0x1826), ble::Uuid::from16(0x1818), ble::Uuid::from16(0x180d)}));
    const auto& plan = h.central.connects.back().plan;
    assert(h.adapter.subscriptions().size() == plan.subscriptionCount);
    for (std::size_t index = 0; index < plan.subscriptionCount; ++index) {
        const auto& subscription = h.adapter.subscriptions()[index];
        assert(subscription.service == plan.subscriptions[index].service);
        assert(subscription.characteristic == plan.subscriptions[index].characteristic);
        assert(subscription.indicate == plan.subscriptions[index].indicate);
        assert(subscription.state == "pending");
    }
    h.central.pushEvent(ble::CentralEventType::Connected, 0x11);
    h.adapter.loop();
    assert(h.adapter.isConnected());
    for (std::size_t index = 0; index < plan.subscriptionCount; ++index)
        assert(h.adapter.subscriptions()[index].state == ((0x11U & (1U << index)) ? "ok" : "failed"));
    h.central.advertise(peer, "", {0x180f});
    h.adapter.loop();
    assert(h.adapter.advertisedServices().size() == 4);
    assert(h.adapter.advertisedServices().back() == ble::Uuid::from16(0x180f));
    h.adapter.disconnect();
    assert(h.adapter.subscriptions().empty() && h.adapter.advertisedServices().empty());
    // Applying unrelated configuration must not discard the latest paired advertisement.
    PairedDevice reapplied = h.adapter.pairedDevice();
    reapplied.advertisedServices.clear();
    h.adapter.setPairedDevice(reapplied);
    // A new scan clears scan results, but not the paired device's last advertisement.
    assert(h.adapter.startScan());
    assert(h.adapter.scanAndConnect());
    assert(h.adapter.advertisedServices().size() == 4);
    assert(h.adapter.subscriptions()[0].state == "pending");
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.adapter.loop();
    assert(h.adapter.subscriptions()[4].state == "failed");
    h.central.pushEvent(ble::CentralEventType::Disconnected);
    h.adapter.loop();
    assert(h.adapter.subscriptions().empty() && h.adapter.advertisedServices().empty());
    h.adapter.clearPairedDevice();
    assert(h.adapter.pairedDevice().advertisedServices.empty());

    // Boot reconnect can restore the persisted advertisement without a user scan.
    PairedDevice saved;
    saved.address = peer;
    saved.valid = true;
    saved.adapterId = "ftms";
    saved.advertisedServices = {ble::Uuid::from16(0x1826)};
    h.adapter.setPairedDevice(saved);
    assert(h.adapter.scanAndConnect());
    assert(h.adapter.advertisedServices() == saved.advertisedServices);
    assert(!h.central.connects.back().plan.reportServices);
    h.central.pushEvent(ble::CentralEventType::Connected, 0);
    h.adapter.loop();
    assert(!h.adapter.isConnected());
    // A rejected connection keeps the failed subscriptions visible during the backoff.
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Backoff);
    assert(!h.adapter.subscriptions().empty() && h.adapter.advertisedServices().empty());
    for (const auto& subscription : h.adapter.subscriptions()) assert(subscription.state == "failed");
    h.adapter.disconnect();
    assert(h.adapter.subscriptions().empty());
}

void testSubscriptionResultsFixedPlan() {
    Harness h;
    h.pair("aa:00:00:00:00:02", "ftms");
    assert(h.adapter.scanAndConnect());
    // Nothing advertised is known: the central reports the GATT services.
    assert(h.central.connects.back().plan.reportServices);
    const auto plan = h.central.connects.back().plan;
    assert(plan.subscriptionCount >= 3);
    const auto& subs = plan.subscriptions;
    h.central.pushSubscriptionResult(0, subs[0].service, subs[0].characteristic, subs[0].indicate,
                                     ble::SubscriptionOutcome::Ok);
    h.central.pushSubscriptionResult(1, subs[1].service, subs[1].characteristic, subs[1].indicate,
                                     ble::SubscriptionOutcome::NotFound);
    h.central.pushSubscriptionResult(2, subs[2].service, subs[2].characteristic, subs[2].indicate,
                                     ble::SubscriptionOutcome::CccdFailed);
    // Results of an older session are ignored.
    h.central.pushSubscriptionResult(3, subs[3].service, subs[3].characteristic, subs[3].indicate,
                                     ble::SubscriptionOutcome::Ok, h.central.lastSession() - 1);
    h.central.pushGattService(ble::Uuid::from16(0x1826));
    h.central.pushGattService(ble::Uuid::from16(0x180a));
    h.central.pushGattService(ble::Uuid::from16(0x1826));
    h.adapter.loop();
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Connecting);
    const auto& list = h.adapter.subscriptions();
    assert(list.size() == plan.subscriptionCount);
    assert(list[0].state == "ok" && list[0].reason.empty());
    assert(list[1].state == "failed" && list[1].reason == "not-found");
    assert(list[2].state == "failed" && list[2].reason == "cccd-failed");
    assert(list[3].state == "pending");
    // GATT services are only taken over after a successful connection.
    assert(h.adapter.advertisedServices().empty());
    h.central.pushEvent(ble::CentralEventType::Connected, 0x9);  // bit 3 comes from the mask fallback
    h.adapter.loop();
    assert(h.adapter.isConnected());
    assert(list[0].state == "ok" && list[1].reason == "not-found" && list[2].state == "failed");
    assert(list[3].state == "ok");
    for (std::size_t index = 4; index < list.size(); ++index) assert(list[index].state == "failed");
    assert(h.adapter.advertisedServices() ==
           (std::vector<ble::Uuid>{ble::Uuid::from16(0x1826), ble::Uuid::from16(0x180a)}));
    assert(h.adapter.pairedDevice().advertisedServices == h.adapter.advertisedServices());
    // Late results after Connected do not change the list.
    h.central.pushSubscriptionResult(0, subs[0].service, subs[0].characteristic, subs[0].indicate,
                                     ble::SubscriptionOutcome::Unsupported);
    h.adapter.loop();
    assert(h.adapter.subscriptions()[0].state == "ok");
    // A reconnect reuses the known services and does not ask for discovery again.
    h.central.pushEvent(ble::CentralEventType::Disconnected);
    h.adapter.loop();
    h.advance(2000);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Connecting);
    assert(!h.central.connects.back().plan.reportServices);
    assert(h.adapter.advertisedServices().size() == 2);
}

void testSubscriptionResultsDiscoveryPlan() {
    Harness h;
    h.pair("aa:00:00:00:00:03", "kickr-run");
    assert(h.adapter.scanAndConnect());
    assert(h.central.connects.back().plan.discoverNotifications);
    assert(h.adapter.subscriptions().empty());
    const auto service = ble::Uuid::fromString("a026ee0b-0a7d-4ab3-97fa-f1500f9feb8b");
    const auto data = ble::Uuid::fromString("a026e01d-0a7d-4ab3-97fa-f1500f9feb8b");
    const auto status = ble::Uuid::fromString("a026e01f-0a7d-4ab3-97fa-f1500f9feb8b");
    h.central.pushSubscriptionResult(0, service, data, false, ble::SubscriptionOutcome::Ok);
    h.central.pushSubscriptionResult(1, service, status, true, ble::SubscriptionOutcome::CccdFailed);
    h.central.pushSubscriptionResult(3, service, status, false, ble::SubscriptionOutcome::Overflow);
    h.adapter.loop();
    const auto& list = h.adapter.subscriptions();
    assert(list.size() == 3);
    assert(list[0].service == service && list[0].characteristic == data && !list[0].indicate);
    assert(list[0].state == "ok");
    assert(list[1].characteristic == status && list[1].indicate);
    assert(list[1].state == "failed" && list[1].reason == "cccd-failed");
    assert(list[2].state == "failed" && list[2].reason == "overflow");
    h.central.pushEvent(ble::CentralEventType::Connected, 0x1);
    h.adapter.loop();
    assert(h.adapter.subscriptions().size() == 3);
    assert(h.adapter.subscriptions()[0].state == "ok" && h.adapter.subscriptions()[1].state == "failed");
}

void testSubscriptionsAfterConnectFailure() {
    Harness h;
    h.pair("aa:00:00:00:00:04", "ftms");
    assert(h.adapter.scanAndConnect());
    const auto& first = h.central.connects.back().plan.subscriptions[0];
    h.central.pushSubscriptionResult(0, first.service, first.characteristic, first.indicate,
                                     ble::SubscriptionOutcome::Ok);
    h.central.pushEvent(ble::CentralEventType::ConnectFailed);
    h.adapter.loop();
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Backoff);
    auto list = h.adapter.subscriptions();
    assert(list.size() > 1);
    assert(list[0].state == "ok");
    for (std::size_t index = 1; index < list.size(); ++index)
        assert(list[index].state == "failed" && list[index].reason == "connect-failed");
    // The next attempt starts again with pending entries.
    h.advance(2000);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Connecting);
    for (const auto& subscription : h.adapter.subscriptions())
        assert(subscription.state == "pending" && subscription.reason.empty());
    // Disconnect while connecting.
    h.central.pushEvent(ble::CentralEventType::Disconnected);
    h.adapter.loop();
    for (const auto& subscription : h.adapter.subscriptions())
        assert(subscription.state == "failed" && subscription.reason == "disconnected");
    // Timeout while connecting.
    h.advance(5000);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Connecting);
    h.advance(16000);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Backoff);
    for (const auto& subscription : h.adapter.subscriptions())
        assert(subscription.state == "failed" && subscription.reason == "timeout");
    // Service discovery failure.
    h.advance(10000);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Connecting);
    h.central.pushEvent(ble::CentralEventType::ConnectFailed, 0, ble::kServiceDiscoveryFailure);
    h.adapter.loop();
    assert(h.adapter.subscriptions()[0].reason == "discovery-failed");
    // A user disconnect clears the list.
    h.adapter.disconnect();
    assert(h.adapter.subscriptions().empty());
}

void testServiceShortNames() {
    assert(serviceShortName("1826") == "FTMS");
    assert(serviceShortName("0x1818") == "CPS");
    assert(serviceShortName("00001816-0000-1000-8000-00805F9B34FB") == "CSC");
    assert(serviceShortName("180D") == "HR");
    assert(serviceShortName("1814") == "RSC");
    assert(serviceShortName("180f") == "BAT");
    assert(serviceShortName("180a") == "DIS");
    assert(serviceShortName("6E400001-B5A3-F393-E0A9-E50E24DCCA9E") == "NUS");
    assert(serviceShortName("1234") == "1234");
    assert(serviceShortName("0bf669f1-45f2-11e7-9598-0800200c9a66") == "0bf669f1\xe2\x80\xa6");
    assert(serviceShortName("invalid") == "invalid");
}

void testAdapterProfiles() {
    Harness h;
    assert(h.adapter.profile() == MachineProfile::IndoorBike);
    h.adapter.startScan();
    ble::ScanReport report;
    report.address = address("aa:00:00:00:00:01");
    report.addService(ble::Uuid::from16(0x1826));
    const Bytes treadmillType{1, 1, 0};
    report.setFtmsServiceData(treadmillType.data(), treadmillType.size());
    assert(ble::Advertisement::from(report).ftmsServiceData == treadmillType);
    h.central.reports.push_back(report);
    h.adapter.loop();
    h.pair("aa:00:00:00:00:01", "ftms");
    assert(h.adapter.scanAndConnect());
    assert(h.adapter.profile() == MachineProfile::Treadmill);
    h.central.pushEvent(ble::CentralEventType::Connected, 0x1f);
    h.adapter.loop();
    assert(h.adapter.profile() == MachineProfile::Treadmill);
    SimulatedDeviceAdapter simulation;
    InputSelector selector(simulation, &h.adapter);
    assert(selector.profile() == MachineProfile::IndoorBike);
    selector.select(InputSelector::Source::Ble);
    assert(selector.profile() == MachineProfile::Treadmill);
    h.adapter.clearPairedDevice();
    assert(h.adapter.profile() == MachineProfile::IndoorBike);
    h.pair("aa:00:00:00:00:02", "ifit-model-24");
    assert(h.adapter.scanAndConnect());
    assert(h.adapter.profile() == MachineProfile::Treadmill);
    h.adapter.disconnect();
    h.pair("aa:00:00:00:00:03", "ifit-model-84");
    assert(h.adapter.scanAndConnect());
    assert(h.adapter.profile() == MachineProfile::CrossTrainer);
    h.adapter.disconnect();
    h.pair("aa:00:00:00:00:04", "concept2-pm5");
    assert(h.adapter.scanAndConnect());
    assert(h.adapter.profile() == MachineProfile::Rower);
}

void testFtmsDistanceTelemetry() {
    Harness h;
    h.pair("aa:00:00:00:00:01", "ftms");
    assert(h.adapter.scanAndConnect());
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.adapter.loop();
    assert(!h.adapter.telemetry().distance.has_value());
    assert(!h.adapter.capabilities().supports(Metric::Distance));
    h.advance(300);
    h.central.pushEvent(ble::CentralEventType::Notification, 0, 0,
                        {0x10, 0, 0xC4, 9, 0x10, 0x27, 0});
    h.adapter.loop();
    assert(near(h.adapter.telemetry().distance, 10000));
    assert(h.adapter.capabilities().supports(Metric::Distance));
    h.advance(300);
    h.central.pushEvent(ble::CentralEventType::Notification, 0, 0, {1, 0});
    h.adapter.loop();
    assert(near(h.adapter.telemetry().distance, 10000));
    h.advance(5001);
    assert(!h.adapter.telemetry().distance.has_value());
    assert(!h.adapter.capabilities().supports(Metric::Distance));
    h.adapter.disconnect();
    assert(!h.adapter.telemetry().distance);
}

void testFtmsElapsedTelemetry() {
    Harness h;
    h.pair("aa:00:00:00:00:01", "ftms");
    assert(h.adapter.scanAndConnect());
    h.central.pushEvent(ble::CentralEventType::Connected, 1U << protocols::FtmsSource::SubTreadmill);
    h.adapter.loop();
    assert(!h.adapter.telemetry().elapsed.has_value());
    assert(!h.adapter.capabilities().supports(Metric::ElapsedTime));
    h.advance(300);
    h.central.pushEvent(ble::CentralEventType::Notification, 0, protocols::FtmsSource::SubTreadmill,
                        {0x04, 0x04, 0xB0, 4, 0x10, 0x27, 0, 0x2c, 1});
    h.adapter.loop();
    const auto telemetry = h.adapter.telemetry();
    assert(near(telemetry.elapsed, 300));
    assert(near(telemetry.distance, 10000));
    assert(h.adapter.capabilities().supports(Metric::ElapsedTime));
    assert(h.adapter.capabilities().supports(Metric::Distance));
    h.advance(300);
    h.central.pushEvent(ble::CentralEventType::Notification, 0, protocols::FtmsSource::SubTreadmill, {1, 0});
    h.adapter.loop();
    assert(near(h.adapter.telemetry().elapsed, 300));
    h.advance(5001);
    assert(!h.adapter.telemetry().elapsed && !h.adapter.telemetry().distance);
    assert(!h.adapter.capabilities().supports(Metric::ElapsedTime));
    h.adapter.disconnect();
    assert(!h.adapter.telemetry().elapsed);
}

void testStaleFtmsTreadmillControlSafety() {
    Harness h;
    using Source = protocols::FtmsSource;
    h.pair("aa:00:00:00:00:01", "ftms");
    assert(h.adapter.scanAndConnect());
    h.central.pushEvent(ble::CentralEventType::ReadResult, 0, Source::ReadFeature,
                        {0, 0, 0, 0, 3, 0, 0, 0});
    h.central.pushEvent(ble::CentralEventType::Connected,
                        (1U << Source::SubTreadmill) | (1U << Source::SubControlPoint));
    h.adapter.loop();
    assert(h.central.writes.size() == 1 && h.central.writes.back().payload() == ftms::encodeRequestControl());
    h.central.pushEvent(ble::CentralEventType::Notification, 0, Source::SubControlPoint, {0x80, 0, 1});
    h.central.pushEvent(ble::CentralEventType::Notification, 0, Source::SubTreadmill, {0, 0, 0xB0, 4});
    h.adapter.loop();
    assert(h.adapter.profile() == MachineProfile::Treadmill && h.central.writes.size() == 1);
    assert(h.adapter.capabilities().supports(Control::Speed));
    assert(h.adapter.apply({Control::Speed, 8}));
    const std::size_t writes = h.central.writes.size();
    h.advance(5001);
    assert(h.adapter.stale() && h.adapter.capabilities().metrics == 0 && h.adapter.capabilities().controls == 0);
    assert(!h.adapter.apply({Control::Speed, 9}));
    assert(!h.adapter.apply({Control::Incline, 2}));
    assert(h.central.writes.size() == writes);
    assert(h.adapter.profile() == MachineProfile::Treadmill);
    h.central.pushEvent(ble::CentralEventType::Notification, 0, Source::SubTreadmill, {0, 0, 0xB0, 4});
    h.adapter.loop();
    assert(!h.adapter.stale() && h.adapter.capabilities().supports(Control::Speed));
    assert(h.adapter.apply({Control::Speed, 9}));
    h.central.pushEvent(ble::CentralEventType::Disconnected);
    h.adapter.loop();
    assert(h.adapter.capabilities().metrics == 0 && h.adapter.capabilities().controls == 0);
    assert(!h.adapter.apply({Control::Speed, 10}));
}

void testScanDedupAndExclusion() {
    Harness h;
    h.central.own = address("00:00:00:00:00:01");
    h.adapter.setExcludedPeer(address("00:00:00:00:00:02"), true);
    assert(h.adapter.startScan(10000) && h.adapter.scanning());
    assert(h.central.scanStarts == 1 && h.central.lastScanDuration == 10000 && h.central.lastScanDuplicates && h.central.lastScanHighDuty);
    h.central.advertise(address("aa:00:00:00:00:01"), "ECH-SPORT", {}, {}, -70);
    h.central.advertise(address("aa:00:00:00:00:01"), "", {0x1826}, {}, -50);  // scan response
    h.central.advertise(address("aa:00:00:00:00:02"), "HRM", {0x180D});
    h.central.advertise(address("00:00:00:00:00:01"), "Self", {0x1826});
    h.central.advertise(address("00:00:00:00:00:02"), "Phone", {0x1826});
    h.advance(10);
    const auto& results = h.adapter.scanResults();
    assert(results.size() == 2);
    const ScanResult* bike = h.adapter.findResult(address("aa:00:00:00:00:01"));
    assert(bike && bike->name == "ECH-SPORT" && bike->rssi == -50 && bike->services.size() == 1);
    assert(bike->detectedAdapter == "ftms");  // merged name + FTMS service
    assert(h.adapter.findResult(address("aa:00:00:00:00:02"))->detectedAdapter == "hrs");
    assert(!h.adapter.findResult(address("00:00:00:00:00:01")));

    for (int index = 0; index < 30; ++index) {
        ble::BleAddress addr;
        addr.bytes[5] = static_cast<std::uint8_t>(0x40 + index);
        h.central.advertise(addr, "X");
    }
    h.advance(10);
    assert(h.adapter.scanResults().size() == BleInputAdapter::kMaxScanResults);
    assert(h.adapter.droppedScanResults() == 30 + 2 - BleInputAdapter::kMaxScanResults);

    h.advance(10000);
    assert(!h.adapter.scanning() && h.central.scanStops == 1);
    h.central.advertise(address("aa:00:00:00:00:09"), "Late");
    h.advance(10);
    assert(!h.adapter.findResult(address("aa:00:00:00:00:09")));
    // A new scan clears old results.
    assert(h.adapter.startScan());
    assert(h.adapter.scanResults().empty());
    h.central.pushEvent(ble::CentralEventType::ScanComplete);
    h.advance(1);
    assert(!h.adapter.scanning());
    // An old completion queued before a new scan must not stop the new scan.
    h.central.pushEvent(ble::CentralEventType::ScanComplete);
    assert(h.adapter.startScan());
    h.advance(1);
    assert(h.adapter.scanning());
    assert(h.adapter.stopScan());

    // Never connect to the bridge itself or the FTMS output's client.
    h.pair("00:00:00:00:00:02", "ftms");
    assert(!h.adapter.scanAndConnect());
    assert(h.central.connects.empty() && h.adapter.linkState() == BleInputAdapter::LinkState::Idle);
    assert(h.adapter.lastError().find("refusing") == 0);
    h.pair("aa:00:00:00:00:01", "nope");
    h.adapter.scanAndConnect();
    assert(h.central.connects.empty() && h.adapter.lastError() == "unknown adapter 'nope'");
}

void testFtmsLifecycle() {
    Harness h;
    assert(!h.adapter.scanAndConnect() && h.adapter.lastError() == "no paired device");
    h.pair("aa:00:00:00:00:01", "ftms", "KICKR");
    h.adapter.startScan();
    assert(h.adapter.scanAndConnect());
    assert(!h.adapter.scanning() && !h.central.scanning());  // scanning stops before connecting
    assert(h.central.connects.size() == 1 && h.adapter.linkState() == BleInputAdapter::LinkState::Connecting);
    const auto& plan = h.central.connects[0].plan;
    assert(plan.subscriptionCount == protocols::FtmsSource::SubCount && plan.readCount == 4);
    assert(plan.subscriptions[protocols::FtmsSource::SubControlPoint].indicate);
    assert(!h.adapter.startScan());  // busy

    const Bytes feature{0x02, 0x44, 0x00, 0x00, 0x0E, 0x20, 0x00, 0x00};
    const Bytes resistanceRange{0x00, 0x00, 0xC8, 0x00, 0x0A, 0x00};  // 0..20.0
    h.central.pushEvent(ble::CentralEventType::ReadResult, 0, 0, feature);
    h.central.pushEvent(ble::CentralEventType::ReadResult, 0, 1, resistanceRange);
    h.central.pushEvent(ble::CentralEventType::Connected, 0x11);
    h.advance(10);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Ready);
    assert(h.adapter.isConnected() && !h.adapter.trainerDetected());
    assert(h.central.writes.size() == 1 && h.central.writes[0].payload() == Bytes{0x00});
    assert(h.last.connected && h.last.deviceName == "KICKR" && h.last.deviceType == "ftms");

    h.central.pushEvent(ble::CentralEventType::Notification, 0, 4, {0x80, 0x00, 0x01});
    h.advance(10);
    assert(h.central.writes.size() == 2 && h.central.writes[1].payload() == Bytes{0x07});
    assert(h.adapter.trainerDetected());
    auto capabilities = h.adapter.capabilities();
    assert(capabilities.supports(Control::Power) && capabilities.supports(Control::Resistance));
    assert(capabilities.supports(Control::Incline) && !capabilities.supports(Control::Speed));
    assert(capabilities.metrics == 0);

    const Bytes bike{0x44, 0x02, 0xC4, 0x09, 0xA0, 0x00, 0x96, 0x00, 0x78};
    h.central.pushEvent(ble::CentralEventType::Notification, 0, 0, bike);
    h.advance(300);
    assert(h.adapter.capabilities().supports(Metric::Power) && h.adapter.capabilities().supports(Metric::HeartRate));
    assert(near(h.last.power, 150.0F) && near(h.last.heartRate, 120.0F));

    // Controls are translated to FTMS control point writes.
    assert(h.adapter.apply({Control::Power, 200.0F}));
    assert(h.central.writes.back().payload() == (Bytes{0x05, 0xC8, 0x00}));
    assert(near(h.adapter.telemetry().targetPower, 200.0F));
    assert(h.adapter.apply({Control::Resistance, 50.0F}));  // 50 % of 0..20 -> 10.0
    assert(h.central.writes.back().payload() == (Bytes{0x04, 0x64, 0x00}));
    assert(h.adapter.apply({Control::Incline, 2.0F}));  // inclination target supported
    assert(h.central.writes.back().payload() == (Bytes{0x03, 0x14, 0x00}));
    assert(!h.adapter.apply({Control::Speed, 10.0F}));

    // Stale telemetry: samples invalidated after 5 s without data, controls stay.
    const int emittedBefore = h.emitted;
    h.advance(5001);
    assert(h.adapter.stale() && h.adapter.capabilities().metrics == 0 && h.adapter.trainerDetected());
    assert(h.emitted == emittedBefore + 1);
    h.central.pushEvent(ble::CentralEventType::Notification, 0, 0, bike);
    h.advance(10);
    assert(!h.adapter.stale() && h.adapter.capabilities().supports(Metric::Power));

    // Link loss -> reconnect with exponential backoff (1, 2, 4 s ...).
    const std::uint32_t session = h.central.lastSession();
    h.central.pushEvent(ble::CentralEventType::Disconnected);
    h.advance(1);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Backoff && !h.adapter.isConnected());
    assert(h.adapter.lastError() == "link lost" && h.adapter.nextAttemptInMs() == 1000);
    assert(!h.last.connected);
    assert(h.central.disconnects.back() == session);
    h.advance(999);
    assert(h.central.connects.size() == 1);
    h.advance(1);
    assert(h.central.connects.size() == 2 && h.adapter.linkState() == BleInputAdapter::LinkState::Connecting);
    h.central.pushEvent(ble::CentralEventType::ConnectFailed);
    h.advance(1);
    assert(h.adapter.nextAttemptInMs() == 2000);
    h.advance(2000);
    assert(h.central.connects.size() == 3);
    h.advance(15001);  // connect timeout
    assert(h.adapter.lastError() == "connect timeout" && h.adapter.nextAttemptInMs() == 4000);
    for (std::uint32_t expected : {8000U, 16000U, 30000U, 30000U}) {
        h.advance(h.adapter.nextAttemptInMs());
        h.central.pushEvent(ble::CentralEventType::ConnectFailed);
        h.advance(0);
        assert(h.adapter.nextAttemptInMs() == expected);
    }
    // Events from an old session are ignored.
    h.advance(30000);
    ble::CentralEvent late;
    late.type = ble::CentralEventType::Connected;
    late.session = session;
    h.central.events.push(late);
    h.advance(1);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Connecting);
    // Successful connection resets the backoff.
    h.central.pushEvent(ble::CentralEventType::ReadResult, 0, 0, feature);
    h.central.pushEvent(ble::CentralEventType::Connected, 0x11);
    h.advance(1);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Ready && h.adapter.reconnectAttempts() == 0);
    // A device that never sends data is reconnected.
    h.advance(20001);
    assert(h.adapter.lastError() == "no data from device" && h.adapter.nextAttemptInMs() == 1000);

    // Explicit disconnect stops reconnecting.
    h.adapter.disconnect();
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Idle && !h.adapter.autoConnect());
    const std::size_t connects = h.central.connects.size();
    h.advance(60000);
    assert(h.central.connects.size() == connects);

    // Auto connect on begin() only when enabled.
    h.adapter.begin();
    assert(h.central.connects.size() == connects);
    h.adapter.setAutoConnect(true);
    h.adapter.begin();
    assert(h.central.connects.size() == connects + 1);
    h.adapter.clearPairedDevice();
    assert(!h.adapter.pairedDevice().valid && h.adapter.linkState() == BleInputAdapter::LinkState::Idle);
}

void testProtocolFailureAndControlDenied() {
    Harness h;
    h.pair("aa:00:00:00:00:03", "ftms");
    h.adapter.scanAndConnect();
    h.central.pushEvent(ble::CentralEventType::Connected, 0);  // no data characteristic
    h.advance(1);
    assert(h.adapter.lastError() == "no FTMS data characteristic");
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Backoff);

    h.advance(1000);
    const Bytes feature{0x02, 0x44, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
    h.central.pushEvent(ble::CentralEventType::ReadResult, 0, 0, feature);
    h.central.pushEvent(ble::CentralEventType::Connected, 0x11);
    h.advance(1);
    h.central.pushEvent(ble::CentralEventType::Notification, 0, 4, {0x80, 0x00, 0x05});
    h.advance(1);
    assert(!h.adapter.trainerDetected());
    assert(h.adapter.protocol()->lastError() == "FTMS Request Control refused: control not permitted");
    assert(!h.adapter.apply({Control::Power, 100.0F}));

    // Request Control retried three times without response, then denied.
    protocols::FtmsSource source;
    source.onRead(protocols::FtmsSource::ReadFeature, feature.data(), feature.size());
    assert(source.onConnected(0x11, 0));
    source.tick(3000);
    source.tick(6000);
    assert(source.controlState() == protocols::FtmsSource::ControlState::Requesting);
    assert(source.pendingWrites() == 3);
    source.tick(9000);
    assert(source.controlState() == protocols::FtmsSource::ControlState::Denied);
    // Rejected targets are reported.
    protocols::FtmsSource granted;
    granted.onRead(protocols::FtmsSource::ReadFeature, feature.data(), feature.size());
    granted.onConnected(0x11, 0);
    const Bytes ok{0x80, 0x00, 0x01};
    const Bytes rejected{0x80, 0x05, 0x03};
    granted.onNotify(protocols::FtmsSource::SubControlPoint, ok.data(), ok.size(), 1);
    granted.onNotify(protocols::FtmsSource::SubControlPoint, rejected.data(), rejected.size(), 2);
    assert(granted.lastError() == "FTMS op 0x05 rejected: invalid parameter");

    // Schwinn IC4 variant is read-only and only subscribes Indoor Bike Data.
    auto schwinn = protocols::createProtocol("schwinn-ic4");
    assert(schwinn->plan().subscriptionCount == 1 && schwinn->plan().readCount == 0);
    assert(schwinn->onConnected(1, 0) && !schwinn->isTrainer() && schwinn->pendingWrites() == 0);
}

void testSessionReplacementAndCancellation() {
    ble::PendingConnection pendingLink;
    pendingLink.start(1);
    pendingLink.start(2);
    pendingLink.complete(1);  // an old worker must not clear the new request
    assert(pendingLink.active() == 2 && pendingLink.cancel() == 2);
    assert(pendingLink.active() == 0);
    pendingLink.start(3);
    pendingLink.complete(3);  // Ready links are not cancelled by scan stop
    assert(pendingLink.cancel() == 0);

    FakeCentral transport;
    const auto peer = address("aa:00:00:00:00:01");
    assert(transport.connect(1, peer, {}));
    transport.acceptStop = false;
    assert(!transport.stopScan());  // cancellation survives queue rejection
    assert(transport.pendingConnection.active() == 0);
    assert(transport.cancelledConnects.back() == 1 && transport.droppedCommands() == 1);
    ble::CentralEvent cancelled;
    assert(transport.pollEvent(cancelled) && cancelled.type == ble::CentralEventType::ConnectFailed);
    assert(cancelled.session == 1);
    transport.acceptStop = true;
    assert(transport.connect(2, peer, {}));
    transport.pushEvent(ble::CentralEventType::Connected, 1);
    assert(transport.stopScan() && transport.cancelledConnects.size() == 1);

    Harness h;
    ble::ConnectionSession link;
    h.pair("aa:00:00:00:00:01", "hrs", "A");
    assert(h.adapter.scanAndConnect());
    assert(h.central.lastSession() == 1);
    link.connected(1);
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.advance(1);
    assert(h.adapter.isConnected());

    // GAP releases A now, but its event is delayed until after B connects.
    const std::uint32_t oldSession = link.disconnected();
    h.pair("aa:00:00:00:00:02", "hrs", "B");
    assert(h.adapter.scanAndConnect());
    assert(h.central.lastSession() == 2);
    link.connected(2);
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    ble::CentralEvent stale;
    stale.type = ble::CentralEventType::Disconnected;
    stale.session = oldSession;
    assert(h.central.events.push(stale));
    h.advance(1);
    assert(h.adapter.isConnected() && h.adapter.telemetry().deviceName == "B");
    assert(link.active() == 2 && h.adapter.lastError().empty());

    assert(h.adapter.requestDisconnect());
    assert(h.adapter.scanAndConnect());  // pending connection, not connected
    const std::uint32_t pending = h.central.lastSession();
    assert(h.adapter.requestDisconnect());
    assert(h.central.disconnects.back() == pending);
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.advance(60000);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Idle);
    assert(!h.adapter.isConnected() && !h.adapter.autoConnect());
    assert(h.adapter.scanAndConnect());
    const std::uint32_t stopped = h.central.lastSession();
    assert(h.adapter.stopScan());
    assert(h.central.disconnects.back() == stopped);
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.advance(1);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Idle);
    assert(!h.adapter.autoConnect());

    assert(h.adapter.scanAndConnect());
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.advance(1);
    assert(h.adapter.isConnected());
    const std::size_t disconnects = h.central.disconnects.size();
    assert(h.adapter.startScan() && h.adapter.stopScan());
    assert(h.adapter.isConnected() && h.central.disconnects.size() == disconnects);
    assert(h.adapter.requestDisconnect());
    assert(h.adapter.scanAndConnect());
    h.central.acceptDisconnect = false;
    assert(!h.adapter.stopScan());
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Idle);
    assert(h.adapter.lastError() == "disconnect/stop request rejected");
}

void testLifecycleQueueOverflow() {
    ble::CentralEventQueue<4> queue;
    ble::CentralEvent notification;
    notification.type = ble::CentralEventType::Notification;
    for (int i = 0; i < 4; ++i) assert(queue.push(notification));
    assert(!queue.push(notification));
    ble::CentralEvent read;
    read.type = ble::CentralEventType::ReadResult;
    ble::CentralEvent connected;
    connected.type = ble::CentralEventType::Connected;
    ble::CentralEvent disconnected;
    disconnected.type = ble::CentralEventType::Disconnected;
    ble::CentralEvent complete;
    complete.type = ble::CentralEventType::ScanComplete;
    assert(queue.push(read) && queue.push(connected) && queue.push(disconnected) && queue.push(complete));
    assert(queue.dropped() == 5 && queue.droppedNotifications() == 5);
    ble::CentralEvent event;
    assert(queue.pop(event) && event.type == ble::CentralEventType::ReadResult);
    assert(queue.pop(event) && event.type == ble::CentralEventType::Connected);
    assert(queue.pop(event) && event.type == ble::CentralEventType::Disconnected);
    assert(queue.pop(event) && event.type == ble::CentralEventType::ScanComplete);
    assert(!queue.pop(event));
    for (int i = 0; i < 4; ++i) assert(queue.push(read));
    assert(!queue.push(disconnected));
    assert(queue.droppedLifecycleEvents() == 1 && queue.droppedNotifications() == 5);

    Harness h;
    h.pair("aa:00:00:00:00:03", "hrs");
    assert(h.adapter.scanAndConnect());
    for (int i = 0; i < 40; ++i) h.central.pushEvent(ble::CentralEventType::Notification);
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.advance(1);
    assert(h.adapter.isConnected());
    assert(h.adapter.droppedEvents() == 9 && h.adapter.droppedNotifications() == 9);
    for (int i = 0; i < 40; ++i) h.central.pushEvent(ble::CentralEventType::Notification);
    h.central.pushEvent(ble::CentralEventType::Disconnected);
    h.advance(1);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Backoff);
    assert(h.adapter.lastError() == "link lost" && h.adapter.droppedEvents() == 18);

    h.advance(1000);
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.advance(1);
    assert(h.adapter.isConnected());
    // Even an all-critical overflow is observable and forces resynchronization.
    for (int i = 0; i < 32; ++i) h.central.pushEvent(ble::CentralEventType::ReadResult);
    h.central.pushEvent(ble::CentralEventType::Disconnected);
    h.advance(1);
    assert(h.adapter.droppedLifecycleEvents() == 1);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Backoff);
    assert(h.adapter.lastError() == "BLE lifecycle event overflow");
}

void testTransportRejectionPropagation() {
    Harness h;
    h.central.scanDrops = 7;
    assert(h.adapter.droppedScanReports() == 7);
    h.central.acceptScan = false;
    assert(!h.adapter.startScan() && !h.adapter.scanning());
    h.pair("aa:00:00:00:00:04", "keiser-m3i");
    assert(!h.adapter.scanAndConnect());
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Backoff);
    assert(h.adapter.lastError() == "listening scan request rejected");

    h.adapter.requestDisconnect();
    h.pair("aa:00:00:00:00:05", "hrs");
    h.central.acceptConnect = false;
    assert(!h.adapter.scanAndConnect());
    assert(h.adapter.lastError() == "connect request rejected");
    h.central.acceptConnect = true;
    h.central.acceptScan = true;
    assert(h.adapter.startScan());
    h.central.acceptStop = false;
    assert(!h.adapter.stopScan() && h.adapter.scanning());
    assert(!h.adapter.scanAndConnect());
    assert(h.adapter.lastError() == "stop scan request rejected");
    h.central.acceptStop = true;
    assert(h.adapter.stopScan());
    assert(h.adapter.scanAndConnect());
    h.central.acceptDisconnect = false;
    assert(!h.adapter.requestDisconnect());
    assert(h.adapter.lastError() == "disconnect/stop request rejected");
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Idle);
    assert(h.adapter.droppedCommands() == 6);

    Harness trainer;
    trainer.pair("aa:00:00:00:00:06", "ftms");
    assert(trainer.adapter.scanAndConnect());
    trainer.central.pushEvent(ble::CentralEventType::ReadResult, 0, 0,
                             {0x02, 0x44, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00});
    trainer.central.pushEvent(ble::CentralEventType::Connected, 0x11);
    trainer.central.pushEvent(ble::CentralEventType::Notification, 0, 4, {0x80, 0x00, 0x01});
    trainer.advance(1);
    trainer.central.acceptWrite = false;
    assert(!trainer.adapter.apply({Control::Power, 200}));
    assert(trainer.adapter.lastError() == "write request rejected");
    assert(!trainer.adapter.telemetry().targetPower);
    assert(trainer.adapter.droppedCommands() == 1);
}

void testAdvertisementOnlyDevice() {
    Harness h;
    const Bytes keiser{0x02, 0x01, 0x06, 0x30, 0x00, 0x07, 0x52, 0x03, 0x14, 0x05, 0xD2, 0x00,
                       0x10, 0x00, 0x05, 0x1E, 0x64, 0x80, 0x0C};
    h.pair("aa:00:00:00:00:04", "keiser-m3i", "M3");
    h.adapter.scanAndConnect();
    assert(h.central.connects.empty());
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Listening);
    assert(h.central.scanning() && h.central.lastScanDuration == 0 && h.central.lastScanDuplicates);
    assert(!h.adapter.isConnected());
    h.central.advertise(address("aa:00:00:00:00:05"), "M3", {}, keiser);  // another bike
    h.advance(10);
    assert(!h.adapter.isConnected());
    h.central.advertise(address("aa:00:00:00:00:04"), "M3", {}, keiser);
    h.advance(10);
    assert(h.adapter.isConnected() && !h.adapter.trainerDetected());
    assert(near(h.adapter.telemetry().power, 210.0F) && near(h.adapter.telemetry().cadence, 85.0F));
    // A user scan reuses the continuous listening scan.
    const int starts = h.central.scanStarts;
    assert(h.adapter.startScan(1000) && h.central.scanStarts == starts);
    h.advance(1001);
    assert(!h.adapter.scanning() && h.central.scanning());
    h.advance(4000);
    assert(h.adapter.stale() && h.adapter.capabilities().metrics == 0);
    h.advance(15000);
    assert(!h.adapter.isConnected());
    h.adapter.disconnect();
    assert(!h.central.scanning());
}

void testCoreIntegration() {
    std::uint32_t now = 1000;
    FakeCentral central;
    BleInputAdapter ble(central, [&now] { return now; });
    SimulatedDeviceAdapter simulation;
    InputSelector selector(simulation, &ble);
    AntOutput ant;
    FtmsOutput ftms;
    BridgeCore core;
    core.attach(selector, ant, ftms);
    core.begin();

    PairedDevice device;
    device.address = address("aa:00:00:00:00:06");
    device.adapterId = "ftms";
    device.valid = true;
    ble.setPairedDevice(device);
    selector.select(InputSelector::Source::Ble);
    core.scanAndConnect();
    const Bytes feature{0x02, 0x44, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
    central.pushEvent(ble::CentralEventType::ReadResult, 0, 0, feature);
    central.pushEvent(ble::CentralEventType::Connected, 0x11);
    central.pushEvent(ble::CentralEventType::Notification, 0, 4, {0x80, 0x00, 0x01});
    central.pushEvent(ble::CentralEventType::Notification, 0, 0, {0x44, 0x02, 0xC4, 0x09, 0xA0, 0x00, 0x96, 0x00, 0x78});
    core.loop();
    assert(core.state() == RuntimeState::ControlReady);
    assert(core.trainerState().sample(Metric::Power).valid);
    assert(core.routeControl({Control::Power, 220.0F}) == ControlResult::Applied);
    assert(central.writes.back().payload() == (Bytes{0x05, 0xDC, 0x00}));
    assert(core.routeControl({Control::Resistance, 20.0F}) == ControlResult::Unsupported);
    assert(core.routeControl({Control::Power, 5000.0F}) == ControlResult::InvalidValue);
    assert(core.hasControlTarget());

    // Stale data invalidates samples and the control target.
    now += 6000;
    core.loop();
    assert(!core.trainerState().sample(Metric::Power).valid && !core.hasControlTarget());
    assert(core.state() == RuntimeState::ControlReady);

    // Switching to the simulation drops the BLE link and stops reconnecting.
    selector.select(InputSelector::Source::Simulation);
    assert(ble.linkState() == BleInputAdapter::LinkState::Idle);
    core.scanAndConnect();
    core.loop();
    assert(core.telemetry().deviceType == "simulation");
    // BLE scanning keeps working while the simulation is active.
    assert(ble.startScan());
    central.advertise(address("aa:00:00:00:00:07"), "HRM", {0x180D});
    core.loop();
    assert(ble.scanResults().size() == 1 && core.telemetry().deviceType == "simulation");
}

void testVendorControlRouting() {
    std::uint32_t now = 1000;
    FakeCentral central;
    BleInputAdapter input{central, [&] { return now; }};
    AntOutput ant;
    FtmsOutput ftms;
    BridgeCore core;
    core.attach(input, ant, ftms);
    PairedDevice device;
    device.address = address("aa:00:00:00:00:08");
    device.adapterId = "tacx-fec";
    device.valid = true;
    input.setPairedDevice(device);
    core.begin();
    core.scanAndConnect();
    central.pushEvent(ble::CentralEventType::Connected, 1);
    central.pushEvent(ble::CentralEventType::Notification, 0, 0, {0, 0, 0xfa, 0});
    core.loop();
    assert(core.trainerState().sample(Metric::Power).value == 250);
    const auto writes = central.writes.size();
    assert(core.routeControl({Control::Power, 250}) == ControlResult::Applied);
    assert(central.writes.size() == writes + 1);
    assert(central.writes.back().data[4] == 0x31);
    assert(core.routeControl({Control::Power, 2001}) == ControlResult::InvalidValue);
    assert(core.routeControl({Control::Speed, 10}) == ControlResult::Unsupported);

    // The same input lifecycle must not promote a mechanical rower to trainer.
    core.disconnect();
    device.adapterId = "concept2-pm5";
    input.setPairedDevice(device);
    core.scanAndConnect();
    central.pushEvent(ble::CentralEventType::Connected, 7);
    core.loop();
    assert(!input.trainerDetected());
    assert(core.routeControl({Control::Resistance, 50}) == ControlResult::NotTrainer);
}

void testVendorStartupGrace() {
    Harness h;
    h.pair("aa:00:00:00:00:09", "ifit-bike");
    assert(h.adapter.scanAndConnect());
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.adapter.loop();
    // A paced vendor handshake must not be cut off by the normal 20s timeout.
    for (unsigned iteration = 0; iteration < 55; ++iteration) h.advance(400);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Ready);
    assert(!h.adapter.capabilities().metrics);
    h.advance(100000);
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Backoff);

    Harness standard;
    standard.pair("aa:00:00:00:00:10", "cps");
    assert(standard.adapter.scanAndConnect());
    standard.central.pushEvent(ble::CentralEventType::Connected, 1);
    standard.adapter.loop();
    standard.advance(20001);
    assert(standard.adapter.linkState() == BleInputAdapter::LinkState::Backoff);
}

void testDiscoveredVendorEndpoint() {
    Harness h;
    h.pair("aa:00:00:00:00:11", "schwinn-170");
    assert(h.adapter.scanAndConnect());
    const auto& plan = h.central.connects.back().plan;
    assert(plan.discoverNotifications && !plan.discoveryWriteCharacteristics[0].isZero());
    const auto service = ble::Uuid::from16(0xfff0);
    Bytes endpoint(service.bytes.begin(), service.bytes.end());
    endpoint.insert(endpoint.end(), plan.discoveryWriteCharacteristics[0].bytes.begin(),
                    plan.discoveryWriteCharacteristics[0].bytes.end());
    h.central.pushEvent(ble::CentralEventType::WriteEndpoint, 0, 0, endpoint);
    h.central.pushEvent(ble::CentralEventType::Connected, 1);
    h.adapter.loop();
    assert(h.adapter.linkState() == BleInputAdapter::LinkState::Ready);
    assert(!h.central.writes.empty());
    assert(h.central.writes.back().service == service);
    assert(h.central.writes.back().payload() == (Bytes{7, 1, 0xd3, 0, 0x1f, 5, 1}));
    h.advance(400);
    h.central.pushEvent(ble::CentralEventType::Notification, 0, 0,
                        {0, 0, 0, 0x58, 2, 0, 0x10, 0x27, 0, 0, 0, 0, 0, 0});
    h.adapter.loop();
    assert(h.adapter.capabilities().supports(Metric::Speed));
    assert(!h.adapter.trainerDetected());
}

void testScanMergingAndConsolidation() {
    Harness h;
    assert(h.adapter.startScan() && h.adapter.scanning());
    assert(h.central.scanStarts == 1 && h.central.lastScanDuration == BleInputAdapter::kDefaultScanMs);
    assert(h.central.lastScanDuplicates && h.central.lastScanHighDuty);

    const auto addr = address("aa:11:22:33:44:55");

    // Report 1: Advertisement without name, with Apple manufacturer data (0x004C)
    const Bytes appleMfg{0x4C, 0x00, 0x02, 0x15};
    h.central.advertise(addr, "", {}, appleMfg, -75);
    h.advance(10);

    const auto* dev = h.adapter.findResult(addr);
    assert(dev != nullptr);
    assert(dev->name.empty());
    assert(dev->manufacturerData == appleMfg);
    assert(dev->services.empty());
    assert(dev->connectable);
    assert(dev->rssi == -75);
    assert(h.adapter.scanResults().size() == 1);

    // Report 2: Scan response with name, no manufacturer data, non-connectable
    ble::ScanReport report2;
    report2.address = addr;
    report2.setName("Smart Trainer");
    report2.connectable = false;
    report2.rssi = -60;
    h.central.reports.push_back(report2);
    h.advance(10);

    dev = h.adapter.findResult(addr);
    assert(dev != nullptr);
    assert(h.adapter.scanResults().size() == 1);  // exactly one consolidated entry
    assert(dev->name == "Smart Trainer");
    assert(dev->manufacturerData == appleMfg);  // manufacturer data retained
    assert(dev->connectable);                   // connectable status retained
    assert(dev->rssi == -60);

    // Report 3: Packet with FTMS service (0x1826), empty name and empty manufacturer
    h.central.advertise(addr, "", {0x1826}, {}, -50);
    h.advance(10);

    dev = h.adapter.findResult(addr);
    assert(dev != nullptr);
    assert(h.adapter.scanResults().size() == 1);
    assert(dev->name == "Smart Trainer");       // name retained
    assert(dev->manufacturerData == appleMfg);  // manufacturer retained
    assert(dev->services.size() == 1 && dev->services[0] == ble::Uuid::from16(0x1826));
    assert(dev->detectedAdapter == "ftms");
    assert(dev->rssi == -50);

    // Report 4: Duplicate report with no new data must not duplicate or clear
    h.central.advertise(addr, "", {}, {}, -48);
    h.advance(10);

    dev = h.adapter.findResult(addr);
    assert(dev != nullptr);
    assert(h.adapter.scanResults().size() == 1);
    assert(dev->name == "Smart Trainer");
    assert(dev->manufacturerData == appleMfg);
    assert(dev->services.size() == 1);
    assert(dev->detectedAdapter == "ftms");
    assert(dev->rssi == -48);

    // Filling queue does not drop existing entry
    for (int i = 0; i < 25; ++i) {
        ble::BleAddress other;
        other.bytes[5] = static_cast<std::uint8_t>(0x60 + i);
        h.central.advertise(other, "Other");
    }
    h.advance(10);
    assert(h.adapter.scanResults().size() == BleInputAdapter::kMaxScanResults);
    assert(h.adapter.droppedScanResults() == (1 + 25) - BleInputAdapter::kMaxScanResults);
    // Duplicate for existing device updates even when queue is full
    h.central.advertise(addr, "Smart Trainer Updated", {}, {}, -40);
    h.advance(10);
    dev = h.adapter.findResult(addr);
    assert(dev != nullptr);
    assert(dev->name == "Smart Trainer Updated");
    assert(dev->rssi == -40);
}

void testScanListKeepsRelevantDevices() {
    Harness h;
    assert(h.adapter.startScan());
    // Fill the list with anonymous devices (no name, service or protocol).
    for (std::size_t i = 0; i < BleInputAdapter::kMaxScanResults; ++i) {
        ble::BleAddress anonymous;
        anonymous.bytes[5] = static_cast<std::uint8_t>(0x10 + i);
        h.central.advertise(anonymous, "", {}, {0x4C, 0x00}, -40 - static_cast<int>(i));
    }
    h.advance(10);
    assert(h.adapter.scanResults().size() == BleInputAdapter::kMaxScanResults);
    // Another anonymous device is dropped.
    ble::BleAddress late;
    late.bytes[5] = 0x70;
    h.central.advertise(late, "", {}, {}, -20);
    h.advance(10);
    assert(!h.adapter.findResult(late) && h.adapter.droppedScanResults() == 1);
    // A fitness device replaces the weakest anonymous entry.
    ble::BleAddress weakest;
    weakest.bytes[5] = static_cast<std::uint8_t>(0x10 + BleInputAdapter::kMaxScanResults - 1);
    const ble::BleAddress bike = address("aa:00:00:00:00:71");
    h.central.advertise(bike, "", {0x1826}, {}, -80);
    h.advance(10);
    assert(h.adapter.scanResults().size() == BleInputAdapter::kMaxScanResults);
    assert(!h.adapter.findResult(weakest) && h.adapter.droppedScanResults() == 2);
    const ScanResult* entry = h.adapter.findResult(bike);
    assert(entry && entry->detectedAdapter == "ftms" && entry->rssi == -80 && entry->name.empty());
    // Its scan response is merged into the same entry.
    h.central.advertise(bike, "KICKR", {}, {}, -78);
    h.advance(10);
    entry = h.adapter.findResult(bike);
    assert(entry && entry->name == "KICKR" && entry->services.size() == 1);
}

void testScanLabelHelpers() {
    // lookupCompany
    assert(lookupCompany(0x004C) == "Apple");
    assert(lookupCompany(0x0006) == "Microsoft");
    assert(lookupCompany(0x0075) == "Samsung");
    assert(lookupCompany(0x00E0) == "Google");
    assert(lookupCompany(0x0087) == "Garmin");
    assert(lookupCompany(0x0157) == "Huami/Xiaomi");
    assert(lookupCompany(0x0059) == "Nordic");
    assert(lookupCompany(0x02E5) == "Espressif");
    assert(lookupCompany(0x0499) == "Ruuvi");
    assert(lookupCompany(0x01C7) == "Wahoo");
    assert(lookupCompany(0x0089) == "Tacx");
    assert(lookupCompany(0x02CD) == "Elite");
    assert(lookupCompany(0xFFFF) == "");

    // manufacturerLabel
    assert(manufacturerLabel({}) == "");
    assert(manufacturerLabel({0x4C}) == "");
    assert(manufacturerLabel({0x4C, 0x00}) == "Apple");
    assert(manufacturerLabel({0x4C, 0x00, 0x01, 0x02}) == "Apple");
    assert(manufacturerLabel({0xCD, 0xAB}) == "Hersteller 0xABCD");

    // serviceLabel
    assert(serviceLabel(ble::Uuid::from16(0x1826)) == "FTMS");
    assert(serviceLabel(ble::Uuid::from16(0x180D)) == "Heart Rate");
    assert(serviceLabel(ble::Uuid::from16(0x1818)) == "Cycling Power");
    assert(serviceLabel(ble::Uuid::from16(0x1816)) == "CSC");
    assert(serviceLabel(ble::Uuid::from16(0x1234)) == "");

    // isKnownService
    assert(isKnownService(ble::Uuid::from16(0x1826)));
    assert(isKnownService(ble::Uuid::from16(0x180D)));
    assert(!isKnownService(ble::Uuid::from16(0x1234)));

    // bestServiceLabel
    assert(bestServiceLabel({}) == "");
    assert(bestServiceLabel({ble::Uuid::from16(0x1826)}) == "FTMS");
    assert(bestServiceLabel({ble::Uuid::from16(0x1234), ble::Uuid::from16(0x1826)}) == "FTMS");
    assert(bestServiceLabel({ble::Uuid::from16(0x1234)}) == "0x1234");

    // isRelevantDevice
    assert(isRelevantDevice("My Device", "", {}));
    assert(isRelevantDevice("", "ftms", {}));
    assert(isRelevantDevice("", "", {ble::Uuid::from16(0x1826)}));
    assert(!isRelevantDevice("", "", {}));
    assert(!isRelevantDevice("", "", {ble::Uuid::from16(0x1234)}));

    // scanDeviceLabel priority: Name -> Manufacturer -> Service -> Address
    const auto testAddr = address("aa:bb:cc:dd:ee:ff");
    assert(scanDeviceLabel("Name", {0x4C, 0x00}, {ble::Uuid::from16(0x1826)}, testAddr) == "Name");
    assert(scanDeviceLabel("", {0x4C, 0x00}, {ble::Uuid::from16(0x1826)}, testAddr) == "Apple");
    assert(scanDeviceLabel("", {0xCD, 0xAB}, {ble::Uuid::from16(0x1826)}, testAddr) == "Hersteller 0xABCD");
    assert(scanDeviceLabel("", {}, {ble::Uuid::from16(0x1826)}, testAddr) == "FTMS");
    assert(scanDeviceLabel("", {}, {ble::Uuid::from16(0x1234)}, testAddr) == "0x1234");
    assert(scanDeviceLabel("", {}, {}, testAddr) == "aa:bb:cc:dd:ee:ff");
}

}  // namespace

void runBleInputTests() {
    testBleTypes();
    testPairedServiceDiagnostics();
    testSubscriptionResultsFixedPlan();
    testSubscriptionResultsDiscoveryPlan();
    testSubscriptionsAfterConnectFailure();
    testServiceShortNames();
    testBoundedQueue();
    testFtmsInputCodec();
    testSensorCodecs();
    testEchelon();
    testDomyos();
    testYesoulAndKeiser();
    testRegistry();
    testProtocolProfiles();
    testFtmsProfiles();
    testFtmsControlDoesNotStartTreadmill();
    testAdapterProfiles();
    testFtmsDistanceTelemetry();
    testFtmsElapsedTelemetry();
    testStaleFtmsTreadmillControlSafety();
    testScanDedupAndExclusion();
    testScanMergingAndConsolidation();
    testScanListKeepsRelevantDevices();
    testScanLabelHelpers();
    testFtmsLifecycle();
    testProtocolFailureAndControlDenied();
    testSessionReplacementAndCancellation();
    testLifecycleQueueOverflow();
    testTransportRejectionPropagation();
    testAdvertisementOnlyDevice();
    testCoreIntegration();
    testVendorControlRouting();
    testVendorStartupGrace();
    testDiscoveredVendorEndpoint();
}
