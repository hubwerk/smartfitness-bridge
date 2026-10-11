#include "bridge/bridge_core.hpp"
#include "ftms/ftms_codec.hpp"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"
#include "config_store.h"
#include "bridge_controller.h"

#include <cassert>
#include <cmath>
#include <string>

using namespace smartfitness;
using ftms::Bytes;
using ftms::ResultCode;

namespace {

MetricSample valid(float value) { return {value, true}; }

ftms::ControlPointCommand decode(const Bytes& bytes) {
    ftms::ControlPointCommand command;
    assert(ftms::decodeControlPoint(bytes.data(), bytes.size(), command));
    return command;
}

void testIndoorBikeDataEncoding() {
    ftms::IndoorBikeData data;
    data.speed = valid(25.0F);
    data.cadence = valid(80.0F);
    data.power = valid(150.0F);
    data.heartRate = valid(120.0F);
    // Flags 0x0244: speed present (More Data clear), cadence, power, heart rate.
    assert(ftms::encodeIndoorBikeData(data) == (Bytes{0x44, 0x02, 0xC4, 0x09, 0xA0, 0x00, 0x96, 0x00, 0x78}));

    assert(ftms::encodeIndoorBikeData({}) == (Bytes{0x01, 0x00}));

    ftms::IndoorBikeData powerOnly;
    powerOnly.power = valid(-5.0F);
    powerOnly.speed = {25.0F, false};
    powerOnly.heartRate = {NAN, true};
    assert(ftms::encodeIndoorBikeData(powerOnly) == (Bytes{0x41, 0x00, 0xFB, 0xFF}));

    ftms::IndoorBikeData clamped;
    clamped.speed = valid(1000.0F);
    clamped.cadence = valid(90.5F);
    clamped.heartRate = valid(300.0F);
    assert(ftms::encodeIndoorBikeData(clamped) == (Bytes{0x04, 0x02, 0xFF, 0xFF, 0xB5, 0x00, 0xFF}));

    TrainerState state;
    state.setSample(Metric::Power, 200.0F);
    assert(!ftms::indoorBikeDataFrom(state).power.valid);
    state.connected = true;
    assert(ftms::indoorBikeDataFrom(state).power.valid);
    assert(!ftms::indoorBikeDataFrom(state).speed.valid);
}

void testStaticCharacteristics() {
    TrainerCapabilities capabilities;
    assert(ftms::encodeFeature(capabilities) == Bytes(8, 0x00));
    for (Metric metric : {Metric::Speed, Metric::Cadence, Metric::Power, Metric::HeartRate, Metric::Incline})
        capabilities.setMetric(metric, true);
    for (Control control : {Control::Speed, Control::Incline, Control::Resistance, Control::Power})
        capabilities.setControl(control, true);
    assert(ftms::encodeFeature(capabilities) == (Bytes{0x0A, 0x44, 0x00, 0x00, 0x0E, 0x20, 0x00, 0x00}));
    capabilities.setControl(Control::Incline, false);
    assert(ftms::encodeFeature(capabilities) == (Bytes{0x0A, 0x44, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00}));

    assert(ftms::encodeSupportedInclinationRange() == (Bytes{0x38, 0xFF, 0xFA, 0x00, 0x01, 0x00}));
    assert(ftms::encodeSupportedResistanceRange() == (Bytes{0x00, 0x00, 0xE8, 0x03, 0x0A, 0x00}));
    assert(ftms::encodeSupportedPowerRange() == (Bytes{0x00, 0x00, 0xD0, 0x07, 0x01, 0x00}));
    assert(ftms::encodeTrainingStatus(ftms::TrainingStatus::Idle) == (Bytes{0x00, 0x01}));
    assert(ftms::encodeAdvertisingServiceData() == (Bytes{0x01, 0x20, 0x00}));
}

void testControlPointDecoding() {
    ftms::ControlPointCommand command;
    assert(!ftms::decodeControlPoint(nullptr, 0, command));
    const std::uint8_t empty[1] = {0};
    assert(!ftms::decodeControlPoint(empty, 0, command));

    for (std::uint8_t opcode : {0x00, 0x01, 0x07}) {
        command = decode({opcode});
        assert(command.opcode == opcode && command.status == ResultCode::Success && !command.hasRequest);
        assert(decode({opcode, 0x01}).status == ResultCode::InvalidParameter);
    }

    command = decode({0x03, 0x32, 0x00});
    assert(command.status == ResultCode::Success && command.hasRequest);
    assert(command.request.control == Control::Incline && command.request.value == 5.0F);
    assert(decode({0x03, 0xCE, 0xFF}).request.value == -5.0F);
    assert(decode({0x03, 0x32}).status == ResultCode::InvalidParameter);
    assert(!decode({0x03, 0x32}).hasRequest);

    command = decode({0x04, 0x64});
    assert(command.status == ResultCode::Success);
    assert(command.request.control == Control::Resistance && command.request.value == 10.0F);
    assert(decode({0x04, 0xE8, 0x03}).request.value == 100.0F);
    assert(decode({0x04}).status == ResultCode::InvalidParameter);
    assert(decode({0x04, 0x01, 0x02, 0x03}).status == ResultCode::InvalidParameter);

    command = decode({0x05, 0xC8, 0x00});
    assert(command.status == ResultCode::Success);
    assert(command.request.control == Control::Power && command.request.value == 200.0F);
    assert(decode({0x05, 0x9C, 0xFF}).request.value == -100.0F);
    assert(decode({0x05, 0xC8}).status == ResultCode::InvalidParameter);
    assert(decode({0x05, 0xC8, 0x00, 0x00}).status == ResultCode::InvalidParameter);

    assert(decode({0x08, 0x01}).status == ResultCode::Success);
    assert(decode({0x08, 0x02}).status == ResultCode::Success);
    assert(decode({0x08, 0x03}).status == ResultCode::InvalidParameter);
    assert(decode({0x08}).status == ResultCode::InvalidParameter);

    // Wind 0, grade 5.00 %, Crr 0.004, Cw 0.51.
    command = decode({0x11, 0x00, 0x00, 0xF4, 0x01, 0x28, 0x33});
    assert(command.status == ResultCode::Success && command.hasRequest);
    assert(command.request.control == Control::Incline && command.request.value == 5.0F);
    assert(decode({0x11, 0x00, 0x00, 0x0C, 0xFE, 0x28, 0x33}).request.value == -5.0F);
    assert(decode({0x11, 0x00, 0x00, 0xF4, 0x01, 0x28}).status == ResultCode::InvalidParameter);

    assert(decode({0x02, 0x10, 0x27}).status == ResultCode::NotSupported);
    assert(decode({0x99}).status == ResultCode::NotSupported);
}

void testResponsesAndStatus() {
    assert(ftms::encodeControlPointResponse(0x05, ResultCode::Success) == (Bytes{0x80, 0x05, 0x01}));
    assert(ftms::encodeControlPointResponse(0x11, ResultCode::ControlNotPermitted) == (Bytes{0x80, 0x11, 0x05}));
    assert(ftms::encodeMachineStatus(decode({0x00})).empty());
    assert(ftms::encodeMachineStatus(decode({0x01})) == (Bytes{0x01}));
    assert(ftms::encodeMachineStatus(decode({0x03, 0x32, 0x00})) == (Bytes{0x06, 0x32, 0x00}));
    assert(ftms::encodeMachineStatus(decode({0x04, 0x64})) == (Bytes{0x07, 0x64}));
    assert(ftms::encodeMachineStatus(decode({0x05, 0xC8, 0x00})) == (Bytes{0x08, 0xC8, 0x00}));
    assert(ftms::encodeMachineStatus(decode({0x07})) == (Bytes{0x04}));
    assert(ftms::encodeMachineStatus(decode({0x08, 0x02})) == (Bytes{0x02, 0x02}));
    assert(ftms::encodeMachineStatus(decode({0x11, 0x00, 0x00, 0xF4, 0x01, 0x28, 0x33})) ==
           (Bytes{0x12, 0x00, 0x00, 0xF4, 0x01, 0x28, 0x33}));

    assert(ftms::resultCodeFor(ControlResult::Applied) == ResultCode::Success);
    assert(ftms::resultCodeFor(ControlResult::Unsupported) == ResultCode::NotSupported);
    assert(ftms::resultCodeFor(ControlResult::InvalidValue) == ResultCode::InvalidParameter);
    assert(ftms::resultCodeFor(ControlResult::AdapterRejected) == ResultCode::OperationFailed);
    assert(ftms::resultCodeFor(ControlResult::Disconnected) == ResultCode::ControlNotPermitted);
    assert(ftms::resultCodeFor(ControlResult::NotTrainer) == ResultCode::ControlNotPermitted);
    assert(ftms::resultCodeFor(ControlResult::NoAdapter) == ResultCode::ControlNotPermitted);
}

Bytes write(FtmsOutput& ftms, BridgeCore& bridge, const Bytes& bytes) {
    assert(ftms.enqueueControlPoint(bytes.data(), bytes.size()));
    bridge.loop();
    return ftms.lastControlResponse();
}

void testFtmsOutputControlFlow() {
    SimulatedDeviceAdapter adapter;
    AntOutput ant;
    FtmsOutput ftms;
    BridgeCore bridge;
    std::uint32_t clock = 10000;
    ftms.setClock([&clock] { return clock; });
    bridge.attach(adapter, ant, ftms);
    bridge.begin();
    bridge.loop();
    assert(ftms.advertising() && !ftms.clientConnected() && !ftms.controlGranted());
    assert(ftms.feature() == Bytes(8, 0x00));

    assert(bridge.scanAndConnect());
    bridge.loop();
    assert(ftms.feature() == (Bytes{0x0A, 0x44, 0x00, 0x00, 0x0E, 0x20, 0x00, 0x00}));
    assert(ftms.lastIndoorBikeData().empty());  // nothing is sent without a client

    ftms.notifyClientConnected();
    bridge.loop();
    assert(!ftms.advertising() && ftms.clientConnected() && !ftms.controlGranted());
    assert(ftms.lastIndoorBikeData() == (Bytes{0x44, 0x02, 0xC4, 0x09, 0xA0, 0x00, 0x96, 0x00, 0x78}));

    // Set commands require Request Control first.
    assert(write(ftms, bridge, {0x05, 0xC8, 0x00}) == (Bytes{0x80, 0x05, 0x05}));
    assert(adapter.telemetry().power == 150);
    assert(write(ftms, bridge, {0x00}) == (Bytes{0x80, 0x00, 0x01}));
    assert(ftms.controlGranted());

    clock += 100;
    assert(write(ftms, bridge, {0x05, 0xC8, 0x00}) == (Bytes{0x80, 0x05, 0x01}));
    assert(ftms.lastMachineStatus() == (Bytes{0x08, 0xC8, 0x00}));
    assert(adapter.telemetry().power == 200);
    assert(bridge.hasControlTarget() && bridge.controlTarget().value == 200);
    bridge.loop();
    // Rate limited: the change is not notified within 250 ms of the previous notification.
    assert(ftms.lastIndoorBikeData()[6] == 0x96);
    clock += 200;
    bridge.loop();
    assert(ftms.lastIndoorBikeData()[6] == 0xC8);
    clock += 300;
    const Bytes before = ftms.lastIndoorBikeData();
    bridge.loop();
    assert(ftms.lastIndoorBikeData() == before);

    // Out-of-range values are rejected by the core gate and leave the target unchanged.
    assert(write(ftms, bridge, {0x05, 0xC4, 0x09}) == (Bytes{0x80, 0x05, 0x03}));
    assert(write(ftms, bridge, {0x05, 0x9C, 0xFF}) == (Bytes{0x80, 0x05, 0x03}));
    assert(write(ftms, bridge, {0x03, 0x2C, 0x01}) == (Bytes{0x80, 0x03, 0x03}));
    assert(bridge.controlTarget().value == 200);
    assert(ftms.lastMachineStatus() == (Bytes{0x08, 0xC8, 0x00}));

    assert(write(ftms, bridge, {0x11, 0x00, 0x00, 0xF4, 0x01, 0x28, 0x33}) == (Bytes{0x80, 0x11, 0x01}));
    assert(adapter.telemetry().incline == 5.0F);
    assert(ftms.lastMachineStatus()[0] == 0x12);
    assert(write(ftms, bridge, {0x04, 0x64}) == (Bytes{0x80, 0x04, 0x01}));
    assert(adapter.telemetry().resistance == 10.0F);
    assert(write(ftms, bridge, {0x07}) == (Bytes{0x80, 0x07, 0x01}));
    assert(ftms.lastMachineStatus() == (Bytes{0x04}));
    assert(write(ftms, bridge, {0x08, 0x02}) == (Bytes{0x80, 0x08, 0x01}));
    assert(write(ftms, bridge, {0x02, 0x10, 0x27}) == (Bytes{0x80, 0x02, 0x02}));
    assert(write(ftms, bridge, {0x05, 0xC8}) == (Bytes{0x80, 0x05, 0x03}));
    const Bytes oversized(FtmsOutput::kMaxWriteLength + 5, 0x05);
    assert(write(ftms, bridge, oversized) == (Bytes{0x80, 0x05, 0x03}));

    // Read-only device: control requests are refused by the core gate.
    adapter.setTrainer(false);
    bridge.loop();
    assert(ftms.feature() == (Bytes{0x8A, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    assert(write(ftms, bridge, {0x05, 0xFA, 0x00}) == (Bytes{0x80, 0x05, 0x05}));
    assert(write(ftms, bridge, {0x11, 0x00, 0x00, 0xF4, 0x01, 0x28, 0x33}) == (Bytes{0x80, 0x11, 0x05}));
    assert(adapter.telemetry().power == 200);
    adapter.setTrainer(true);
    bridge.disconnect();
    bridge.loop();
    assert(write(ftms, bridge, {0x05, 0xFA, 0x00}) == (Bytes{0x80, 0x05, 0x05}));
    assert(bridge.scanAndConnect());
    bridge.loop();
    assert(write(ftms, bridge, {0x00}) == (Bytes{0x80, 0x00, 0x01}));

    // Reset releases control.
    assert(write(ftms, bridge, {0x01}) == (Bytes{0x80, 0x01, 0x01}));
    assert(!ftms.controlGranted());
    assert(ftms.lastMachineStatus() == (Bytes{0x01}));

    // Disconnect clears control and restarts advertising; stale writes are dropped.
    assert(write(ftms, bridge, {0x00}) == (Bytes{0x80, 0x00, 0x01}));
    const Bytes stale{0x05, 0x2C, 0x01};
    assert(ftms.enqueueControlPoint(stale.data(), stale.size()));
    ftms.notifyClientDisconnected();
    bridge.loop();
    assert(!ftms.clientConnected() && !ftms.controlGranted() && ftms.advertising());
    assert(ftms.lastControlResponse() == (Bytes{0x80, 0x00, 0x01}));
    assert(adapter.telemetry().power == 150);

    ftms.notifyClientConnected();
    bridge.loop();
    assert(write(ftms, bridge, {0x05, 0xFA, 0x00}) == (Bytes{0x80, 0x05, 0x05}));

    // Bounded queue.
    const Bytes request{0x00};
    for (std::size_t index = 0; index < FtmsOutput::kQueueCapacity; ++index)
        assert(ftms.enqueueControlPoint(request.data(), request.size()));
    assert(!ftms.enqueueControlPoint(request.data(), request.size()));
    assert(!ftms.enqueueControlPoint(request.data(), 0));
    bridge.loop();
    assert(ftms.controlGranted());

    // Disabling stops advertising, drops the client and rejects controls.
    ftms.setEnabled(false);
    assert(!ftms.advertising() && !ftms.clientConnected() && !ftms.controlGranted());
    assert(ftms.requestControl({Control::Power, 100}) == ControlResult::NoAdapter);
    bridge.loop();
    assert(!ftms.advertising());
    // A connection/write callback may arrive before the asynchronous disconnect.
    ftms.notifyClientConnected();
    assert(ftms.enqueueControlPoint(request.data(), request.size()));
    bridge.loop();
    assert(!ftms.clientConnected() && !ftms.controlGranted() && !ftms.advertising());
    ftms.notifyClientDisconnected();
    ftms.setEnabled(true);
    bridge.loop();
    assert(ftms.advertising() && !ftms.clientConnected());
}

void testFtmsNameAndConsole() {
    FtmsOutput standalone;
    assert(standalone.deviceName() == FtmsOutput::kDefaultName);
    standalone.setDeviceName("A very long bridge name for testing");
    assert(standalone.deviceName().size() == FtmsOutput::kMaxNameLength);
    standalone.setDeviceName("");
    assert(standalone.deviceName() == FtmsOutput::kDefaultName);

    SimulatedDeviceAdapter adapter;
    AntOutput ant;
    FtmsOutput ftms;
    ConfigStore config;
    BridgeCore bridge;
    BridgeController ui(bridge, config, adapter, ant, ftms);
    config.begin();
    bridge.attach(adapter, ant, ftms);
    bridge.begin();
    ui.begin();
    assert(ui.status().find("ftms-out(advertising=1,client=0,control=0)") != std::string::npos);
    assert(ui.handleCommand("ftms-name Garage Bike") == "ftms name: Garage Bike");
    assert(ftms.deviceName() == "Garage Bike");
    assert(ui.handleCommand("ftms-name " + std::string(21, 'x')) == "expected 1-20 characters");
    assert(ui.handleCommand("ftms-name bad\tname") == "expected printable ASCII");
    ui.handleCommand("scan");
    ftms.notifyClientConnected();
    write(ftms, bridge, {0x00});
    assert(ui.status().find("ftms-out(advertising=0,client=1,control=1)") != std::string::npos);
    ui.handleCommand("ftms-output off");
    assert(ui.status().find("ftms-out(advertising=0,client=0,control=0)") != std::string::npos);
    ui.handleCommand("ftms-output on");
    assert(ui.status().find("ftms-out(advertising=1,client=0,control=0)") != std::string::npos);
}

void testFtmsClientAddress() {
    FtmsOutput ftms;
    std::array<std::uint8_t, 6> address{};
    std::uint8_t type = 0;
    assert(!ftms.clientAddress(address, type));
    ftms.setClientAddress({0xaa, 0xbb, 0xcc, 0x01, 0x02, 0x03}, 1);
    assert(ftms.clientAddress(address, type));
    assert(address[0] == 0xaa && address[5] == 0x03 && type == 1);
    ftms.clearClientAddress();
    assert(!ftms.clientAddress(address, type));
}

void testTreadmillCodec() {
    ftms::TreadmillData data;
    data.speed = valid(8.5F);
    data.incline = valid(-5);
    data.rampAngle = valid(-2.9F);
    data.distance = valid(1234);
    data.heartRate = valid(120);
    const Bytes payload{0x0C, 0x01, 0x52, 0x03, 0xD2, 0x04, 0x00, 0xCE, 0xFF, 0xE3, 0xFF, 0x78};
    assert(ftms::encodeTreadmillData(data) == payload);
    ftms::MachineData decoded;
    assert(ftms::decodeTreadmillData(payload.data(), payload.size(), decoded));
    assert(decoded.speed.value == 8.5F && decoded.incline.value == -5);
    assert(decoded.distance.value == 1234 && decoded.heartRate.value == 120);
    assert(ftms::encodeTreadmillData({}) == (Bytes{1, 0}));
    data = {};
    data.distance = valid(1.0e30F);
    data.incline = valid(1.0e30F);
    data.rampAngle = valid(1.0e30F);
    assert(ftms::encodeTreadmillData(data) ==
           (Bytes{0x0D, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0x7F}));
    data = {};
    data.elapsed = valid(42);
    data.speed = {NAN, true};
    assert(ftms::encodeTreadmillData(data) == (Bytes{1, 4, 42, 0}));
    const auto timed = ftms::encodeTreadmillData(data);
    assert(ftms::decodeTreadmillData(timed.data(), timed.size(), decoded));
    assert(decoded.elapsed.valid && decoded.elapsed.value == 42);
    TrainerState state;
    state.connected = true;
    state.setSample(Metric::ElapsedTime, 42);
    state.setSample(Metric::Distance, 100);
    assert(ftms::encodeTreadmillData(ftms::treadmillDataFrom(state)) == (Bytes{5, 4, 100, 0, 0, 42, 0}));
    state.connected = false;
    assert(ftms::encodeTreadmillData(ftms::treadmillDataFrom(state)) == (Bytes{1, 0}));
    assert(ftms::encodeAdvertisingServiceData(MachineProfile::Treadmill) == (Bytes{1, 1, 0}));
    assert(ftms::encodeSupportedSpeedRange() == (Bytes{0, 0, 0x10, 0x27, 1, 0}));
    ftms::ControlPointCommand command;
    const Bytes speed{2, 0x52, 3};
    assert(ftms::decodeControlPoint(speed.data(), speed.size(), command, MachineProfile::Treadmill));
    assert(command.status == ResultCode::Success && command.hasRequest &&
           command.request.control == Control::Speed && command.request.value == 8.5F);
    assert(ftms::encodeMachineStatus(command) == (Bytes{5, 0x52, 3}));
    assert(ftms::decodeControlPoint(speed.data(), 2, command, MachineProfile::Treadmill));
    assert(command.status == ResultCode::InvalidParameter);
    const Bytes outOfRange{2, 0x11, 0x27};
    assert(ftms::decodeControlPoint(outOfRange.data(), outOfRange.size(), command, MachineProfile::Treadmill));
    assert(command.status == ResultCode::InvalidParameter && !command.hasRequest);
    for (const Bytes& bytes : {Bytes{5, 100, 0}, Bytes{4, 10}, Bytes{0x11, 0, 0, 0, 0, 0, 0}}) {
        assert(ftms::decodeControlPoint(bytes.data(), bytes.size(), command, MachineProfile::Treadmill));
        assert(command.status == ResultCode::NotSupported && !command.hasRequest);
    }
}

class FakeTreadmill final : public BtDeviceAdapter {
public:
    FakeTreadmill() {
        value.speedValid = true;
        value.inclineValid = true;
        value.heartRateValid = true;
    }
    void begin() override {}
    void loop() override { emitTelemetry(value); }
    bool scanAndConnect() override { value.connected = true; return true; }
    void disconnect() override { value.connected = false; }
    bool isConnected() const override { return value.connected; }
    bool trainerDetected() const override { return writable; }
    MachineProfile profile() const override { return MachineProfile::Treadmill; }
    FitnessTelemetry telemetry() const override { return value; }
    TrainerCapabilities capabilities() const override {
        TrainerCapabilities result;
        for (Metric metric : {Metric::Speed, Metric::Incline, Metric::HeartRate}) result.setMetric(metric, true);
        if (writable) {
            result.setControl(Control::Speed, true);
            result.setControl(Control::Incline, true);
        }
        return result;
    }
    bool apply(const ControlRequest& request) override {
        if (!writable || reject) return false;
        ++writes;
        if (request.control == Control::Speed) value.speed = request.value;
        else if (request.control == Control::Incline) value.incline = request.value;
        else return false;
        if (afterApply) afterApply();
        return true;
    }
    FitnessTelemetry value;
    bool writable = true;
    bool reject = false;
    unsigned writes = 0;
    std::function<void()> afterApply;
};

void testTreadmillOutput() {
    FakeTreadmill adapter;
    AntOutput ant;
    FtmsOutput output;
    BridgeCore bridge;
    std::uint32_t clock = 10000;
    output.setClock([&] { return clock; });
    bridge.attach(adapter, ant, output);
    bridge.begin();
    assert(bridge.scanAndConnect());
    bridge.loop();
    assert(output.profile() == MachineProfile::Treadmill);
    assert(output.feature() == (Bytes{8, 4, 0, 0, 3, 0, 0, 0}));
    assert(adapter.writes == 0);
    output.notifyClientConnected();
    bridge.loop();
    assert(output.lastIndoorBikeData().empty());
    ftms::MachineData data;
    auto payload = output.lastTreadmillData();
    assert(ftms::decodeTreadmillData(payload.data(), payload.size(), data));
    assert(data.speed.valid && data.speed.value == 0);
    assert(write(output, bridge, {2, 0x20, 3}) == (Bytes{0x80, 2, 5}));
    assert(write(output, bridge, {0}) == (Bytes{0x80, 0, 1}));
    assert(write(output, bridge, {7}) == (Bytes{0x80, 7, 4}));
    assert(adapter.writes == 0); // start never manufactures motion
    assert(write(output, bridge, {2, 0x20, 3}) == (Bytes{0x80, 2, 1}));
    assert(adapter.value.speed == 8);
    assert(output.lastMachineStatus() == (Bytes{5, 0x20, 3}));
    clock += 300;
    bridge.loop();
    assert(output.lastMachineStatus() == (Bytes{4}));
    payload = output.lastTreadmillData();
    assert(ftms::decodeTreadmillData(payload.data(), payload.size(), data));
    assert(data.speed.value == 8);
    assert(write(output, bridge, {2, 0x20}) == (Bytes{0x80, 2, 3}));
    assert(write(output, bridge, {2, 0x11, 0x27}) == (Bytes{0x80, 2, 3}));
    assert(write(output, bridge, {3, 50, 0}) == (Bytes{0x80, 3, 1}));
    assert(adapter.value.incline == 5);
    for (Bytes bytes : {Bytes{5, 100, 0}, Bytes{4, 10}, Bytes{0x11, 0, 0, 0, 0, 0, 0}})
        assert(write(output, bridge, bytes) == (Bytes{0x80, bytes[0], 2}));
    adapter.reject = true;
    assert(write(output, bridge, {8, 1}) == (Bytes{0x80, 8, 4}));
    assert(adapter.value.speed == 8);
    adapter.reject = false;
    assert(write(output, bridge, {8, 1}) == (Bytes{0x80, 8, 1}));
    assert(adapter.value.speed == 0);
    assert(output.lastMachineStatus() == (Bytes{2, 1}));
    clock += 300;
    bridge.loop();
    assert(output.lastMachineStatus() == (Bytes{2, 1}));
    const Bytes stale{2, 0xB0, 4};
    assert(output.enqueueControlPoint(stale.data(), stale.size()));
    const auto writes = adapter.writes;
    output.setProfileOverride(FtmsOutputProfile::IndoorBike);
    bridge.loop();
    assert(!output.controlGranted() && !output.clientConnected() && output.advertising());
    assert(adapter.writes == writes && adapter.value.speed == 0);
    assert(output.profile() == MachineProfile::IndoorBike);
    assert(output.lastTreadmillData().empty());
    output.setProfileOverride(FtmsOutputProfile::Auto);
    bridge.loop();
    output.notifyClientConnected();
    bridge.loop();
    assert(write(output, bridge, stale) == (Bytes{0x80, 2, 5}));
    assert(write(output, bridge, {0}) == (Bytes{0x80, 0, 1}));
    assert(output.enqueueControlPoint(stale.data(), stale.size()));
    output.notifyClientDisconnected();
    bridge.loop();
    assert(adapter.writes == writes && adapter.value.speed == 0);
    output.notifyClientConnected();
    bridge.loop();
    assert(write(output, bridge, stale) == (Bytes{0x80, 2, 5}));
    assert(write(output, bridge, {0}) == (Bytes{0x80, 0, 1}));
    adapter.writable = false;
    bridge.loop();
    assert(write(output, bridge, stale) == (Bytes{0x80, 2, 5}));
    assert(output.feature() == (Bytes{8, 4, 0, 0, 0, 0, 0, 0}));
    assert(adapter.writes == writes);
    assert(write(output, bridge, {0}) == (Bytes{0x80, 0, 1}));
    assert(write(output, bridge, {8, 2}) == (Bytes{0x80, 8, 4}));
    bridge.disconnect();
    bridge.loop();
    assert(!output.controlGranted());
    output.setProfileOverride(FtmsOutputProfile::Rower);
    assert(output.profile() == MachineProfile::Rower);
    output.setProfileOverride(FtmsOutputProfile::CrossTrainer);
    assert(output.profile() == MachineProfile::CrossTrainer);
    output.setProfileOverride(FtmsOutputProfile::Auto);
    bridge.scanAndConnect();
    bridge.loop();
    output.notifyClientConnected();
    bridge.loop();
    assert(write(output, bridge, {0}) == (Bytes{0x80, 0, 1}));
    assert(output.enqueueControlPoint(stale.data(), stale.size()));
    output.setProfileOverride(FtmsOutputProfile::Treadmill);
    bridge.loop();
    assert(output.profile() == MachineProfile::Treadmill && output.clientConnected());
    assert(!output.controlGranted() && adapter.writes == writes);
    adapter.value.speed = 6;
    output.notifyClientDisconnected();
    bridge.loop();
    assert(output.lastTreadmillData().empty());
    output.notifyClientConnected();
    bridge.loop();
    assert(output.lastMachineStatus() == (Bytes{4}));
    assert(adapter.writes == writes);
    adapter.writable = true;
    bridge.loop();
    assert(write(output, bridge, {0}) == (Bytes{0x80, 0, 1}));
    adapter.afterApply = [&] { output.notifyClientDisconnected(); };
    const Bytes first{2, 100, 0};
    const Bytes second{2, 200, 0};
    assert(output.enqueueControlPoint(first.data(), first.size()));
    assert(output.enqueueControlPoint(second.data(), second.size()));
    bridge.loop();
    assert(adapter.writes == writes + 1 && adapter.value.speed == 1);
    adapter.afterApply = {};
    bridge.loop();
    assert(!output.clientConnected() && !output.controlGranted());
}

using Member = MetricSample FitnessMetrics::*;

const std::vector<Member>& allFields() {
    static const std::vector<Member> fields{
        &FitnessMetrics::speed, &FitnessMetrics::cadence, &FitnessMetrics::power,
        &FitnessMetrics::heartRate, &FitnessMetrics::incline, &FitnessMetrics::resistance,
        &FitnessMetrics::distance, &FitnessMetrics::elapsed, &FitnessMetrics::averageSpeed,
        &FitnessMetrics::averageCadence, &FitnessMetrics::averagePower, &FitnessMetrics::rampAngle,
        &FitnessMetrics::positiveElevationGain, &FitnessMetrics::negativeElevationGain,
        &FitnessMetrics::pace, &FitnessMetrics::averagePace, &FitnessMetrics::totalEnergy,
        &FitnessMetrics::energyPerHour, &FitnessMetrics::energyPerMinute,
        &FitnessMetrics::metabolicEquivalent, &FitnessMetrics::remaining,
        &FitnessMetrics::forceOnBelt, &FitnessMetrics::stepRate, &FitnessMetrics::averageStepRate,
        &FitnessMetrics::strideCount, &FitnessMetrics::strokeRate, &FitnessMetrics::averageStrokeRate,
        &FitnessMetrics::strokeCount, &FitnessMetrics::floors, &FitnessMetrics::stepCount,
        &FitnessMetrics::movementDirection
    };
    return fields;
}

std::vector<Member> profileFields(MachineProfile profile) {
    using M = FitnessMetrics;
    std::vector<Member> fields{&M::totalEnergy, &M::energyPerHour, &M::energyPerMinute,
        &M::heartRate, &M::metabolicEquivalent, &M::elapsed, &M::remaining};
    const auto add = [&](std::initializer_list<Member> extra) { fields.insert(fields.end(), extra); };
    switch (profile) {
        case MachineProfile::IndoorBike:
            add({&M::speed, &M::averageSpeed, &M::cadence, &M::averageCadence,
                 &M::distance, &M::resistance, &M::power, &M::averagePower});
            break;
        case MachineProfile::Treadmill:
            add({&M::speed, &M::averageSpeed, &M::distance, &M::incline, &M::rampAngle,
                 &M::positiveElevationGain, &M::negativeElevationGain, &M::pace, &M::averagePace,
                 &M::forceOnBelt, &M::power});
            break;
        case MachineProfile::CrossTrainer:
            add({&M::speed, &M::averageSpeed, &M::distance, &M::stepRate, &M::averageStepRate,
                 &M::strideCount, &M::positiveElevationGain, &M::negativeElevationGain,
                 &M::incline, &M::rampAngle, &M::resistance, &M::power, &M::averagePower,
                 &M::movementDirection});
            break;
        case MachineProfile::Rower:
            add({&M::strokeRate, &M::strokeCount, &M::averageStrokeRate, &M::distance,
                 &M::pace, &M::averagePace, &M::power, &M::averagePower, &M::resistance});
            break;
        case MachineProfile::StepClimber:
            add({&M::floors, &M::stepCount, &M::stepRate, &M::averageStepRate, &M::positiveElevationGain});
            break;
        case MachineProfile::StairClimber:
            add({&M::floors, &M::stepRate, &M::averageStepRate, &M::positiveElevationGain, &M::strideCount});
            break;
    }
    return fields;
}

void assertTruncated(MachineProfile profile, const Bytes& bytes) {
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        FitnessMetrics out;
        for (const auto field : allFields()) out.*field = valid(99);
        assert(!ftms::decodeMachineData(profile, bytes.data(), length, out));
        for (const auto field : allFields()) assert(!(out.*field).valid);
    }
    FitnessMetrics out;
    assert(!ftms::decodeMachineData(profile, nullptr, bytes.size(), out));
}

void testAllProfileFields() {
    const std::array<MachineProfile, 6> profiles{MachineProfile::IndoorBike, MachineProfile::Treadmill,
        MachineProfile::CrossTrainer, MachineProfile::Rower, MachineProfile::StepClimber, MachineProfile::StairClimber};
    const std::array<Bytes, 6> headers{Bytes{0xFE, 0x1F}, Bytes{0xFE, 0x1F}, Bytes{0xFE, 0xFF, 0},
        Bytes{0xFE, 0x1F}, Bytes{0xFE, 1}, Bytes{0xFE, 3}};
    const std::array<std::size_t, 6> lengths{30, 34, 41, 30, 23, 23};
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const auto profile = profiles[i];
        FitnessMetrics input;
        for (const auto field : profileFields(profile)) input.*field = valid(10);
        input.power = valid(-125);
        input.averagePower = valid(-87);
        input.incline = valid(-5.5F);
        input.rampAngle = valid(-2.9F);
        input.forceOnBelt = valid(-230);
        input.distance = valid(0xFEDCBA);
        input.metabolicEquivalent = valid(6.3F);
        input.movementDirection = valid(1);
        input.pace = valid(profile == MachineProfile::Rower ? 123 : 12.3F);
        input.averagePace = valid(profile == MachineProfile::Rower ? 134 : 13.4F);
        // Set irrelevant fields valid as well; the codec must not emit them.
        const auto encoded = ftms::encodeMachineData(profile, input);
        assert(encoded.size() == lengths[i]);
        for (std::size_t j = 0; j < headers[i].size(); ++j) assert(encoded[j] == headers[i][j]);
        FitnessMetrics output;
        assert(ftms::decodeMachineData(profile, encoded.data(), encoded.size(), output));
        for (const auto field : profileFields(profile)) {
            assert((output.*field).valid);
            assert(std::fabs((output.*field).value - (input.*field).value) < 0.001F);
        }
        assert(ftms::encodeMachineData(profile, output) == encoded);
        assertTruncated(profile, encoded);
        assert(effectiveOutputProfile(FtmsOutputProfile::Auto, profile) == profile);
        FtmsOutputProfile parsed;
        assert(parseFtmsOutputProfile(machineProfileName(profile), parsed));
        assert(effectiveOutputProfile(parsed, MachineProfile::IndoorBike) == profile);
        const auto type = profile == MachineProfile::IndoorBike ? 0x20 :
            profile == MachineProfile::Treadmill ? 1 : profile == MachineProfile::CrossTrainer ? 2 :
            profile == MachineProfile::Rower ? 0x10 : profile == MachineProfile::StepClimber ? 4 : 8;
        assert(ftms::encodeAdvertisingServiceData(profile) == (Bytes{1, static_cast<std::uint8_t>(type), 0}));

        for (std::size_t mtu : {8U, 9U, 20U, 100U}) {
            const auto packets = ftms::splitMachineData(profile, input, mtu);
            assert(!packets.empty());
            FitnessMetrics merged;
            for (std::size_t p = 0; p < packets.size(); ++p) {
                assert(packets[p].size() <= mtu);
                assert((packets[p][0] & 1U) == (p + 1 == packets.size() ? 0U : 1U));
                FitnessMetrics fragment;
                assert(ftms::decodeMachineData(profile, packets[p].data(), packets[p].size(), fragment));
                assertTruncated(profile, packets[p]);
                for (const auto field : allFields())
                    if ((fragment.*field).valid) merged.*field = fragment.*field;
            }
            assert(ftms::encodeMachineData(profile, merged) == encoded);
        }
        assert(ftms::splitMachineData(profile, input, 1).empty());
        assert(ftms::splitMachineData(profile, input, 6).empty()); // 5-byte energy group + flags
        for (const auto field : allFields()) input.*field = valid(0);
        const auto zero = ftms::encodeMachineData(profile, input);
        assert(ftms::decodeMachineData(profile, zero.data(), zero.size(), output));
        for (const auto field : profileFields(profile)) assert((output.*field).valid && (output.*field).value == 0);
        assertTruncated(profile, zero);
    }
}

void testAtomicGroupsAndMapping() {
    FitnessMetrics input;
    input.floors = valid(0x1234);
    input.stepCount = valid(0x5678);
    assert(ftms::encodeStepClimberData(input) == (Bytes{0, 0, 0x34, 0x12, 0x78, 0x56}));
    assert(ftms::encodeStairClimberData(input) == (Bytes{0, 0, 0x34, 0x12}));
    input = {};
    input.strokeRate = valid(32.5F);
    input.strokeCount = valid(0x1234);
    assert(ftms::encodeRowerData(input) == (Bytes{0, 0, 65, 0x34, 0x12}));
    input = {};
    input.resistance = valid(-5);
    assert(ftms::encodeIndoorBikeData(input) == (Bytes{0x21, 0, 0xFB, 0xFF}));
    assert(ftms::encodeRowerData(input) == (Bytes{0x81, 0, 0xFB, 0xFF}));
    assert(ftms::encodeCrossTrainerData(input) == (Bytes{0x81, 0, 0, 0xCE, 0xFF}));
    input = {};
    input.incline = valid(5);
    input.power = valid(100);
    input.totalEnergy = valid(10);
    input.floors = valid(3);
    input.strokeRate = valid(20);
    assert(ftms::encodeTreadmillData(input) == (Bytes{1, 0}));
    assert(ftms::encodeStepClimberData(input) == (Bytes{1, 0}));
    assert(ftms::encodeRowerData(input) == (Bytes{0x21, 0, 100, 0}));
    input.rampAngle = {NAN, true};
    assert(ftms::encodeTreadmillData(input) == (Bytes{1, 0}));
    input = {};
    input.speed = valid(18);
    input.cadence = valid(45);
    input.averageSpeed = valid(20);
    input.averageCadence = valid(50);
    const auto rower = ftms::mapMachineData(input, MachineProfile::IndoorBike, MachineProfile::Rower);
    assert(rower.pace.valid && rower.pace.value == 100);
    assert(rower.strokeRate.valid && rower.strokeRate.value == 45 && !rower.strokeCount.valid);
    assert(!rower.averagePace.valid && !rower.averageStrokeRate.valid);
    assert(!rower.rampAngle.valid && !rower.power.valid);
    auto bike = ftms::mapMachineData(rower, MachineProfile::Rower, MachineProfile::IndoorBike);
    assert(bike.speed.valid && bike.speed.value == 18 && !bike.pace.valid);
    input = {};
    input.pace = valid(0);
    input.strokeRate = valid(0);
    bike = ftms::mapMachineData(input, MachineProfile::Rower, MachineProfile::IndoorBike);
    assert(!bike.speed.valid && bike.cadence.valid && bike.cadence.value == 0);
    const auto stoppedRower = ftms::mapMachineData(input, MachineProfile::Rower, MachineProfile::Rower);
    assert(stoppedRower.pace.valid && stoppedRower.pace.value == 0 && !stoppedRower.speed.valid);
    input = {};
    input.speed = valid(0);
    const auto zeroSpeed = ftms::mapMachineData(input, MachineProfile::IndoorBike, MachineProfile::Rower);
    assert(zeroSpeed.speed.valid && zeroSpeed.speed.value == 0 && !zeroSpeed.pace.valid);
    input = {};
    input.stepRate = valid(70);
    bike = ftms::mapMachineData(input, MachineProfile::CrossTrainer, MachineProfile::IndoorBike);
    assert(bike.cadence.valid && bike.cadence.value == 70);
    const auto cross = ftms::mapMachineData(bike, MachineProfile::IndoorBike, MachineProfile::CrossTrainer);
    assert(cross.stepRate.valid && cross.stepRate.value == 70 && !cross.averageStepRate.valid);
    input = {};
    for (const auto field : allFields()) input.*field = {INFINITY, true};
    for (MachineProfile profile : {MachineProfile::IndoorBike, MachineProfile::Treadmill,
         MachineProfile::CrossTrainer, MachineProfile::Rower, MachineProfile::StepClimber, MachineProfile::StairClimber}) {
        const auto encoded = ftms::encodeMachineData(profile, input);
        assert(encoded == (profile == MachineProfile::CrossTrainer ? Bytes{1, 0, 0} : Bytes{1, 0}));
    }
    input = {};
    input.distance = valid(-1);
    auto bytes = ftms::encodeIndoorBikeData(input);
    assert(bytes == (Bytes{0x11, 0, 0, 0, 0}));
    input.distance = valid(1e30F);
    assert(ftms::encodeIndoorBikeData(input) == (Bytes{0x11, 0, 0xFF, 0xFF, 0xFF}));
}

void testProfileFeaturesAndControls() {
    TrainerCapabilities caps;
    for (Control control : {Control::Speed, Control::Incline, Control::Resistance, Control::Power}) caps.setControl(control, true);
    for (MachineProfile profile : {MachineProfile::IndoorBike, MachineProfile::Treadmill,
         MachineProfile::CrossTrainer, MachineProfile::Rower, MachineProfile::StepClimber, MachineProfile::StairClimber}) {
        FitnessMetrics input;
        for (const auto field : profileFields(profile)) input.*field = valid(1);
        ftms::Features features;
        auto bytes = ftms::encodeFeature(caps, profile, input);
        assert(ftms::decodeFeature(bytes.data(), bytes.size(), features));
        assert((features.machine & ftms::kFeatureExpendedEnergy) && (features.machine & ftms::kFeatureHeartRate));
        const bool climbing = profile == MachineProfile::StepClimber || profile == MachineProfile::StairClimber;
        if (climbing) assert(features.targets == 0);
        if (profile == MachineProfile::Rower) assert(features.targets == (ftms::kTargetResistance | ftms::kTargetPower));
        if (profile != MachineProfile::IndoorBike) assert(!(features.targets & ftms::kTargetIndoorBikeSimulation));
        ftms::ControlPointCommand command;
        const Bytes simulation{0x11, 0, 0, 0, 0, 0, 0};
        assert(ftms::decodeControlPoint(simulation.data(), simulation.size(), command, profile));
        assert(command.status == (profile == MachineProfile::IndoorBike ? ResultCode::Success : ResultCode::NotSupported));
        if (climbing) for (const Bytes& request : {Bytes{2, 0, 0}, Bytes{3, 0, 0}, Bytes{4, 0}, Bytes{5, 0, 0}}) {
            assert(ftms::decodeControlPoint(request.data(), request.size(), command, profile));
            assert(command.status == ResultCode::NotSupported && !command.hasRequest);
        }
    }
}

void testSixProfileOutputs() {
    for (MachineProfile profile : {MachineProfile::IndoorBike, MachineProfile::Treadmill,
         MachineProfile::CrossTrainer, MachineProfile::Rower, MachineProfile::StepClimber, MachineProfile::StairClimber}) {
        FtmsOutput output;
        output.setClock([] { return 10000U; });
        output.begin();
        TrainerState state;
        state.connected = true;
        state.profile = profile;
        for (const auto field : profileFields(profile)) state.metrics.*field = valid(10);
        state.metrics.movementDirection = valid(1);
        output.publishTrainerState(state);
        output.notifyClientConnected();
        output.loop();
        assert(output.profile() == profile);
        const auto mapped = ftms::mapMachineData(state.metrics, profile, profile);
        assert(output.lastMachineData() == ftms::encodeMachineData(profile, mapped));
        assert(!output.lastMachineDataPackets().empty());
        FitnessMetrics merged;
        for (const auto& packet : output.lastMachineDataPackets()) {
            assert(packet.size() <= 20);
            FitnessMetrics fragment;
            assert(ftms::decodeMachineData(profile, packet.data(), packet.size(), fragment));
            merged.merge(fragment);
        }
        assert(ftms::encodeMachineData(profile, merged) == output.lastMachineData());
        if (profile != MachineProfile::IndoorBike) assert(output.lastIndoorBikeData().empty());
        if (profile != MachineProfile::Treadmill) assert(output.lastTreadmillData().empty());
        if (profile == MachineProfile::CrossTrainer) assert(output.lastCrossTrainerData() == output.lastMachineData());
        if (profile == MachineProfile::Rower) assert(output.lastRowerData() == output.lastMachineData());
        if (profile == MachineProfile::StepClimber) assert(output.lastStepClimberData() == output.lastMachineData());
        if (profile == MachineProfile::StairClimber) assert(output.lastStairClimberData() == output.lastMachineData());
        const Bytes pending{0x05, 0, 0};
        assert(output.enqueueControlPoint(pending.data(), pending.size()));
        const auto replacement = profile == MachineProfile::IndoorBike ? FtmsOutputProfile::Rower : FtmsOutputProfile::IndoorBike;
        output.setProfileOverride(replacement);
        output.loop();
        assert(!output.clientConnected() && !output.controlGranted());
        assert(output.lastMachineData().empty() && output.lastMachineDataPackets().empty());
    }
}

}  // namespace

void runFtmsCodecTests() {
    testAllProfileFields();
    testAtomicGroupsAndMapping();
    testProfileFeaturesAndControls();
    testTreadmillCodec();
    testIndoorBikeDataEncoding();
    testStaticCharacteristics();
    testControlPointDecoding();
    testResponsesAndStatus();
}

void runFtmsTests() {
    runFtmsCodecTests();
    testSixProfileOutputs();
    testTreadmillOutput();
    testFtmsClientAddress();
    testFtmsOutputControlFlow();
    testFtmsNameAndConsole();
}
