// Host tests for the BLE input side: value types, bounded queue, FTMS input
// codec, sensor codecs, vendor protocol ports, adapter registry and the
// BleInputAdapter connection lifecycle (fake transport + fake clock).
//
// Vendor byte samples: qdomyos-zwift ships no captured traces, so frames are
// built from the field layouts its parsers read and from the literal init and
// command bytes in its sources (cited per test).

#include <cassert>
#include <cmath>
#include <thread>

#include "ant_output.h"
#include "ble/bounded_queue.hpp"
#include "ble_input_adapter.h"
#include "bridge/bridge_core.hpp"
#include "fake_central.hpp"
#include "ftms/ftms_codec.hpp"
#include "ftms_output.h"
#include "protocols/domyos.hpp"
#include "protocols/echelon.hpp"
#include "protocols/ftms_source.hpp"
#include "protocols/registry.hpp"
#include "protocols/sensor_codec.hpp"
#include "protocols/sensor_sources.hpp"
#include "protocols/yesoul_keiser.hpp"

using namespace smartfitness;
using ble::Bytes;
using testing::FakeCentral;
using testing::address;

namespace {

bool near(float actual, float expected, float tolerance = 0.01F) { return std::fabs(actual - expected) <= tolerance; }

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
    out.speed = {31.5F, true};
    out.power = {-5.0F, true};
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
    assert(detected(advert("ECH-ROW-1")).empty());
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

    for (std::size_t index = 0; index < protocols::protocolCount(); ++index) {
        const auto& info = protocols::protocolAt(index);
        auto protocol = info.create();
        assert(protocol && std::string(protocol->id()) == info.id);
        assert(protocol->advertisementOnly() == info.advertisementOnly);
        assert(protocols::findProtocol(info.id) == &info);
    }
    assert(!protocols::createProtocol("unknown"));
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

void testScanDedupAndExclusion() {
    Harness h;
    h.central.own = address("00:00:00:00:00:01");
    h.adapter.setExcludedPeer(address("00:00:00:00:00:02"), true);
    assert(h.adapter.startScan(10000) && h.adapter.scanning());
    assert(h.central.scanStarts == 1 && h.central.lastScanDuration == 10000 && !h.central.lastScanDuplicates);
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
    assert(!h.adapter.scanAndConnect());
    assert(!h.adapter.scanning() && !h.central.scanning());  // scanning stops before connecting
    assert(h.central.connects.size() == 1 && h.adapter.linkState() == BleInputAdapter::LinkState::Connecting);
    const auto& plan = h.central.connects[0].plan;
    assert(plan.subscriptionCount == 5 && plan.subscriptions[4].indicate && plan.readCount == 4);
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
    h.central.events.push_back(late);
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

}  // namespace

void runBleInputTests() {
    testBleTypes();
    testBoundedQueue();
    testFtmsInputCodec();
    testSensorCodecs();
    testEchelon();
    testDomyos();
    testYesoulAndKeiser();
    testRegistry();
    testScanDedupAndExclusion();
    testFtmsLifecycle();
    testProtocolFailureAndControlDenied();
    testAdvertisementOnlyDevice();
    testCoreIntegration();
}
