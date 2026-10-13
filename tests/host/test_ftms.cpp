#include "bridge/bridge_core.hpp"
#include "ftms/ftms_codec.hpp"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"
#include "config_store.h"
#include "bridge_controller.h"
#include "ble_input_adapter.h"
#include "fake_central.hpp"

#include <cassert>
#include <cmath>
#include <string>

using namespace smartfitness;
using ftms::Bytes;
using ftms::ResultCode;

namespace {

ftms::ControlPointCommand decode(const Bytes& bytes) {
    ftms::ControlPointCommand command;
    assert(ftms::decodeControlPoint(bytes.data(), bytes.size(), command));
    return command;
}

void testIndoorBikeDataEncoding() {
    ftms::IndoorBikeData data;
    data.instantaneousSpeed = 2500;
    data.instantaneousCadence = 160;
    data.instantaneousPower = 150;
    data.heartRate = 120;
    // Flags 0x0244: speed present (More Data clear), cadence, power, heart rate.
    assert(ftms::encodeIndoorBikeData(data) == (Bytes{0x44, 0x02, 0xC4, 0x09, 0xA0, 0x00, 0x96, 0x00, 0x78}));

    assert(ftms::encodeIndoorBikeData({}) == (Bytes{0x01, 0x00}));

    ftms::IndoorBikeData powerOnly;
    powerOnly.instantaneousPower = -5;
    assert(ftms::encodeIndoorBikeData(powerOnly) == (Bytes{0x41, 0x00, 0xFB, 0xFF}));

    // Mapping from core state: invalid / non-finite metrics stay absent and
    // out-of-range values clamp to the field's native type.
    TrainerState clamped;
    clamped.connected = true;
    clamped.setSample(Metric::Speed, 1000.0F);
    clamped.setSample(Metric::Cadence, 90.5F);
    clamped.setSample(Metric::HeartRate, 300.0F);
    clamped.setSample(Metric::Power, NAN);
    assert(ftms::encodeIndoorBikeData(ftms::indoorBikeDataFrom(clamped)) ==
           (Bytes{0x04, 0x02, 0xFF, 0xFF, 0xB5, 0x00, 0xFF}));

    TrainerState state;
    state.setSample(Metric::Power, 200.0F);
    assert(!ftms::indoorBikeDataFrom(state).instantaneousPower);
    state.connected = true;
    assert(ftms::indoorBikeDataFrom(state).instantaneousPower == std::int16_t{200});
    assert(!ftms::indoorBikeDataFrom(state).instantaneousSpeed);
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
    assert(ftms.feature() == (Bytes{0x0A, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    assert(write(ftms, bridge, {0x05, 0xFA, 0x00}) == (Bytes{0x80, 0x05, 0x05}));
    assert(write(ftms, bridge, {0x11, 0x00, 0x00, 0xF4, 0x01, 0x28, 0x33}) == (Bytes{0x80, 0x11, 0x05}));
    assert(adapter.telemetry().power == 200);
    adapter.setTrainer(true);
    bridge.disconnect();
    bridge.loop();
    assert(write(ftms, bridge, {0x05, 0xFA, 0x00}) == (Bytes{0x80, 0x05, 0x05}));
    assert(bridge.scanAndConnect());

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
    data.instantaneousSpeed = 850;
    data.inclination = -50;
    data.rampAngleSetting = -29;
    data.totalDistance = 1234;
    data.heartRate = 120;
    const Bytes payload{0x0C, 0x01, 0x52, 0x03, 0xD2, 0x04, 0x00, 0xCE, 0xFF, 0xE3, 0xFF, 0x78};
    assert(ftms::encodeTreadmillData(data) == payload);
    ftms::MachineData decoded;
    assert(ftms::decodeTreadmillData(payload.data(), payload.size(), decoded));
    assert(decoded.speed.value == 8.5F && decoded.incline.value == -5);
    assert(decoded.distance.value == 1234 && decoded.heartRate.value == 120);
    assert(!decoded.power.valid && !decoded.elapsed.valid && !decoded.totalEnergy.valid);
    assert(ftms::encodeTreadmillData({}) == (Bytes{1, 0}));
    TrainerState huge;
    huge.connected = true;
    huge.setSample(Metric::Distance, 1.0e30F);
    huge.setSample(Metric::Incline, 1.0e30F);
    assert(ftms::encodeTreadmillData(ftms::treadmillDataFrom(huge)) ==
           (Bytes{0x0D, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x84, 0x03}));
    data = {};
    data.elapsedTime = 42;
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
            result.setControl(Control::RunState, nativeRunState);
        }
        return result;
    }
    bool apply(const ControlRequest& request) override {
        if (!writable || reject) return false;
        ++writes;
        lastRequest = request;
        if (request.control == Control::Speed) value.speed = request.value;
        else if (request.control == Control::Incline) value.incline = request.value;
        else if (request.control != Control::RunState || !nativeRunState) return false;
        if (afterApply) afterApply();
        return true;
    }
    FitnessTelemetry value;
    bool writable = true;
    bool reject = false;
    bool nativeRunState = false;
    ControlRequest lastRequest{};
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
    assert(!data.speed.valid);  // never transmitted, so never reported as 0
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
    assert(write(output, bridge, {8, 2}) == (Bytes{0x80, 8, 1}));
    assert(adapter.lastRequest.control == Control::Speed && adapter.lastRequest.value == 0);
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
    // Every FTMS profile is served natively (no indoor bike fallback).
    output.setProfileOverride(FtmsOutputProfile::Rower);
    assert(output.profile() == MachineProfile::Rower);
    output.setProfileOverride(FtmsOutputProfile::CrossTrainer);
    assert(output.profile() == MachineProfile::CrossTrainer);
    output.setProfileOverride(FtmsOutputProfile::StepClimber);
    assert(output.profile() == MachineProfile::StepClimber);
    output.setProfileOverride(FtmsOutputProfile::StairClimber);
    assert(output.profile() == MachineProfile::StairClimber);
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

void testNativeTreadmillRunState() {
    FakeTreadmill adapter;
    adapter.nativeRunState = true;
    adapter.value.speed = 8;
    adapter.value.treadmillOperation = TreadmillOperation::Stopped;
    AntOutput ant;
    FtmsOutput output;
    BridgeCore bridge;
    bridge.attach(adapter, ant, output);
    bridge.begin();
    assert(bridge.scanAndConnect());
    output.notifyClientConnected();
    bridge.loop();
    // Confirmed stopped overrides a nonzero speed.
    assert(output.lastMachineStatus() == (Bytes{2, 1}));
    assert(write(output, bridge, {7}) == (Bytes{0x80, 7, 5}));
    assert(adapter.writes == 0);
    assert(write(output, bridge, {0}) == (Bytes{0x80, 0, 1}));
    assert(write(output, bridge, {7}) == (Bytes{0x80, 7, 1}));
    assert(adapter.lastRequest.control == Control::RunState && adapter.lastRequest.value == 1);
    assert(adapter.value.speed == 8 && output.lastMachineStatus() == (Bytes{2, 1}));
    adapter.value.speed = 0;
    adapter.value.treadmillOperation = TreadmillOperation::Starting;
    bridge.loop();
    assert(output.lastMachineStatus() == (Bytes{4}));
    adapter.value.treadmillOperation = TreadmillOperation::Running;
    bridge.loop();
    assert(output.lastMachineStatus() == (Bytes{4})); // running can report zero speed
    assert(write(output, bridge, {8, 2}) == (Bytes{0x80, 8, 1}));
    assert(adapter.lastRequest.control == Control::RunState && adapter.lastRequest.value == 2);
    assert(output.lastMachineStatus() == (Bytes{4})); // queued pause is not confirmed
    adapter.value.speed = 8;
    adapter.value.treadmillOperation = TreadmillOperation::Stopping;
    bridge.loop();
    assert(output.lastMachineStatus() == (Bytes{2, 1}));
    assert(write(output, bridge, {8, 1}) == (Bytes{0x80, 8, 1}));
    assert(adapter.lastRequest.control == Control::RunState && adapter.lastRequest.value == 0);
    assert(adapter.value.speed == 8);
    adapter.value.treadmillOperation = TreadmillOperation::Stopped;
    bridge.loop();
    assert(output.lastMachineStatus() == (Bytes{2, 1}));
    const auto writes = adapter.writes;
    assert(write(output, bridge, {8, 3}) == (Bytes{0x80, 8, 3}));
    assert(adapter.writes == writes);
    adapter.reject = true;
    assert(write(output, bridge, {7}) == (Bytes{0x80, 7, 4}));
    assert(write(output, bridge, {8, 1}) == (Bytes{0x80, 8, 4}));
    assert(adapter.writes == writes);
    adapter.reject = false;
    adapter.value.treadmillOperation.reset();
    bridge.loop();
    assert(output.lastMachineStatus() == (Bytes{4})); // legacy speed fallback
    adapter.writable = false;
    bridge.loop();
    assert(write(output, bridge, {7}) == (Bytes{0x80, 7, 5}));
    assert(write(output, bridge, {0}) == (Bytes{0x80, 0, 1}));
    assert(write(output, bridge, {7}) == (Bytes{0x80, 7, 4}));
    assert(write(output, bridge, {8, 2}) == (Bytes{0x80, 8, 4}));
    assert(adapter.writes == writes);
    bridge.disconnect();
    bridge.loop();
    assert(!output.controlGranted() && !output.telemetry().treadmillOperation);
}

void testF15BridgeToFtms() {
    testing::FakeCentral central;
    std::uint32_t clock = 100;
    BleInputAdapter adapter(central, [&] { return clock; });
    PairedDevice paired;
    paired.address = testing::address("aa:00:00:00:00:15");
    paired.adapterId = "sportstech-f15";
    paired.name = "FS-001";
    paired.valid = true;
    adapter.setPairedDevice(paired);
    AntOutput ant;
    FtmsOutput output;
    output.setClock([&] { return clock; });
    BridgeCore bridge;
    bridge.attach(adapter, ant, output);
    bridge.begin();
    assert(bridge.scanAndConnect() == false); // asynchronous connection
    const auto service = ble::Uuid::from16(0xfff0);
    const auto characteristic = ble::Uuid::from16(0xfff2);
    Bytes endpoint(service.bytes.begin(), service.bytes.end());
    endpoint.insert(endpoint.end(), characteristic.bytes.begin(), characteristic.bytes.end());
    central.pushEvent(ble::CentralEventType::WriteEndpoint, 0, 0, endpoint);
    central.pushEvent(ble::CentralEventType::Connected, 1);
    bridge.loop();
    output.notifyClientConnected();
    const auto confirm = [&](TreadmillOperation operation) {
        Bytes frame{2, 0x51, static_cast<std::uint8_t>(operation),
                    85, 35, 0x55, 2, 0x84, 1, 0x34, 2, 0, 0, 132, 0};
        std::uint8_t checksum = 0;
        for (std::size_t index = 1; index < frame.size(); ++index) checksum ^= frame[index];
        frame.push_back(checksum);
        frame.push_back(3);
        clock += 300;
        central.pushEvent(ble::CentralEventType::Notification, 0, 0, frame);
        bridge.loop();
        assert(output.telemetry().treadmillOperation == operation);
    };
    confirm(TreadmillOperation::Stopped);
    assert(write(output, bridge, {0}) == (Bytes{0x80, 0, 1}));
    assert(write(output, bridge, {7}) == (Bytes{0x80, 7, 1}));
    assert(central.writes.back().payload() == (Bytes{2, 0x53, 1, 0, 0, 0, 0, 0, 0x52, 3}));
    assert(output.lastMachineStatus() == (Bytes{2, 1}));
    assert(write(output, bridge, {7}) == (Bytes{0x80, 7, 4})); // duplicate start
    confirm(TreadmillOperation::Starting);
    assert(output.lastMachineStatus() == (Bytes{4}));
    assert(write(output, bridge, {8, 2}) == (Bytes{0x80, 8, 1}));
    assert(central.writes.back().payload() == (Bytes{2, 0x53, 3, 0x50, 3}));
    assert(output.lastMachineStatus() == (Bytes{4}));
    confirm(TreadmillOperation::Stopping);
    assert(output.lastMachineStatus() == (Bytes{2, 1}));
    confirm(TreadmillOperation::Running);
    ftms::MachineData decoded;
    const auto& payload = output.lastTreadmillData();
    assert(ftms::decodeTreadmillData(payload.data(), payload.size(), decoded));
    assert(decoded.speed.valid && decoded.speed.value == 8.5F);
    assert(decoded.incline.valid && decoded.incline.value == 3.5F);
    assert(decoded.elapsed.valid && decoded.elapsed.value == 597);
    assert(decoded.distance.valid && decoded.distance.value == 388);
    assert(decoded.totalEnergy.valid && decoded.totalEnergy.value == 56);
    assert(decoded.heartRate.valid && decoded.heartRate.value == 132);
    assert(!decoded.averageSpeed.valid && !decoded.power.valid && !decoded.remainingTime.valid);
    assert(write(output, bridge, {2, 0x20, 3}) == (Bytes{0x80, 2, 1}));
    assert(central.writes.back().payload()[2] == 2 && central.writes.back().payload()[3] == 80);
    assert(write(output, bridge, {3, 50, 0}) == (Bytes{0x80, 3, 1}));
    assert(central.writes.back().payload()[4] == 5);
    assert(write(output, bridge, {8, 1}) == (Bytes{0x80, 8, 1}));
    assert(central.writes.back().payload() == (Bytes{2, 0x53, 3, 0x50, 3}));
    clock += 5001;
    bridge.loop();
    assert(!output.telemetry().treadmillOperation && !bridge.canControl());
    assert(write(output, bridge, {7}) == (Bytes{0x80, 7, 5}));
    bridge.disconnect();
    bridge.loop();
    assert(!output.telemetry().connected && !output.telemetry().treadmillOperation);
}

bool near(const MetricSample& sample, float expected) {
    return sample.valid && std::fabs(sample.value - expected) < 0.01F;
}

// Encodes, decodes back into the profile struct (re-encoding must be
// identical), rejects the truncated payload and returns the mapped metrics.
template <typename Data, typename Encode, typename Decode>
ftms::MachineData roundTrip(const Data& data, Encode encode, Decode decode) {
    const Bytes payload = encode(data);
    Data raw;
    assert(decode(payload.data(), payload.size(), raw));
    assert(encode(raw) == payload);
    if (payload.size() > 3) assert(!decode(payload.data(), payload.size() - 1, raw));
    ftms::MachineData mapped;
    assert(decode(payload.data(), payload.size(), mapped));
    return mapped;
}

std::size_t validCount(const ftms::MachineData& data) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < kMetricCount; ++index) count += data[static_cast<Metric>(index)].valid;
    return count;
}

template <typename Data>
void fillCommon(Data& data) {
    data.totalEnergy = 50;
    data.energyPerHour = 600;
    data.energyPerMinute = 10;
    data.heartRate = 130;
    data.metabolicEquivalent = 85;
    data.elapsedTime = 300;
    data.remainingTime = 600;
}

void checkCommon(const ftms::MachineData& data) {
    assert(near(data.totalEnergy, 50) && near(data.energyPerHour, 600) && near(data.energyPerMinute, 10));
    assert(near(data.heartRate, 130) && near(data.metabolicEquivalent, 8.5F));
    assert(near(data.elapsed, 300) && near(data.remainingTime, 600));
}

void testAllProfileCodecs() {
    const auto bikeEncode = [](const ftms::IndoorBikeData& d) { return ftms::encodeIndoorBikeData(d); };
    const auto bikeDecode = [](const std::uint8_t* d, std::size_t n, auto& out) { return ftms::decodeIndoorBikeData(d, n, out); };
    ftms::IndoorBikeData bike;
    bike.instantaneousSpeed = 2500;
    bike.averageSpeed = 2400;
    bike.instantaneousCadence = 161;
    bike.averageCadence = 150;
    bike.totalDistance = 12345;
    bike.resistanceLevel = 12;
    bike.instantaneousPower = 250;
    bike.averagePower = 230;
    fillCommon(bike);
    auto data = roundTrip(bike, bikeEncode, bikeDecode);
    assert(near(data.speed, 25) && near(data.averageSpeed, 24) && near(data.cadence, 80.5F));
    assert(near(data.averageCadence, 75) && near(data.distance, 12345) && near(data.resistance, 12));
    assert(near(data.power, 250) && near(data.averagePower, 230));
    checkCommon(data);
    assert(validCount(data) == 15);

    const auto treadmillEncode = [](const ftms::TreadmillData& d) { return ftms::encodeTreadmillData(d); };
    const auto treadmillDecode = [](const std::uint8_t* d, std::size_t n, auto& out) { return ftms::decodeTreadmillData(d, n, out); };
    ftms::TreadmillData treadmill;
    treadmill.instantaneousSpeed = 1200;
    treadmill.averageSpeed = 1000;
    treadmill.totalDistance = 5000;
    treadmill.inclination = 25;
    treadmill.rampAngleSetting = 14;
    treadmill.positiveElevationGain = 105;
    treadmill.negativeElevationGain = 50;
    treadmill.instantaneousPace = 2;  // 0.2 km/min = 12 km/h
    treadmill.averagePace = 1;
    treadmill.forceOnBelt = -30;
    treadmill.powerOutput = 180;
    fillCommon(treadmill);
    data = roundTrip(treadmill, treadmillEncode, treadmillDecode);
    assert(near(data.speed, 12) && near(data.averageSpeed, 10) && near(data.distance, 5000));
    assert(near(data.incline, 2.5F) && near(data.rampAngle, 1.4F));
    assert(near(data.positiveElevationGain, 10.5F) && near(data.negativeElevationGain, 5));
    assert(near(data.pace, 150) && near(data.averagePace, 300));  // s per 500 m
    assert(near(data.forceOnBelt, -30) && near(data.power, 180));
    checkCommon(data);

    const auto crossEncode = [](const ftms::CrossTrainerData& d) { return ftms::encodeCrossTrainerData(d); };
    const auto crossDecode = [](const std::uint8_t* d, std::size_t n, auto& out) { return ftms::decodeCrossTrainerData(d, n, out); };
    ftms::CrossTrainerData cross;
    cross.instantaneousSpeed = 1000;
    cross.averageSpeed = 900;
    cross.totalDistance = 2000;
    cross.stepPerMinute = 60;
    cross.averageStepRate = 55;
    cross.strideCount = 1234;
    cross.positiveElevationGain = 10;
    cross.negativeElevationGain = 5;
    cross.inclination = 25;
    cross.rampAngleSetting = 14;
    cross.resistanceLevel = 55;
    cross.instantaneousPower = 150;
    cross.averagePower = 140;
    cross.movementBackward = true;
    fillCommon(cross);
    data = roundTrip(cross, crossEncode, crossDecode);
    assert(ftms::encodeCrossTrainerData(cross)[1] & 0x80);  // movement direction (bit 15)
    assert(near(data.speed, 10) && near(data.averageSpeed, 9) && near(data.distance, 2000));
    assert(near(data.cadence, 60) && near(data.averageCadence, 55) && near(data.strideCount, 123.4F));
    assert(near(data.positiveElevationGain, 10) && near(data.negativeElevationGain, 5));
    assert(near(data.incline, 2.5F) && near(data.rampAngle, 1.4F) && near(data.resistance, 5.5F));
    assert(near(data.power, 150) && near(data.averagePower, 140));
    checkCommon(data);

    const auto stepEncode = [](const ftms::StepClimberData& d) { return ftms::encodeStepClimberData(d); };
    const auto stepDecode = [](const std::uint8_t* d, std::size_t n, auto& out) { return ftms::decodeStepClimberData(d, n, out); };
    ftms::StepClimberData step;
    step.floors = 12;
    step.stepCount = 345;
    step.stepPerMinute = 70;
    step.averageStepRate = 65;
    step.positiveElevationGain = 30;
    fillCommon(step);
    data = roundTrip(step, stepEncode, stepDecode);
    assert(near(data.floors, 12) && near(data.stepCount, 345) && near(data.cadence, 70));
    assert(near(data.averageCadence, 65) && near(data.positiveElevationGain, 30));
    assert(!data.power.valid && !data.speed.valid);
    checkCommon(data);

    const auto stairEncode = [](const ftms::StairClimberData& d) { return ftms::encodeStairClimberData(d); };
    const auto stairDecode = [](const std::uint8_t* d, std::size_t n, auto& out) { return ftms::decodeStairClimberData(d, n, out); };
    ftms::StairClimberData stair;
    stair.floors = 8;
    stair.stepPerMinute = 50;
    stair.averageStepRate = 45;
    stair.positiveElevationGain = 24;
    stair.strideCount = 400;
    fillCommon(stair);
    data = roundTrip(stair, stairEncode, stairDecode);
    assert(near(data.floors, 8) && near(data.cadence, 50) && near(data.averageCadence, 45));
    assert(near(data.positiveElevationGain, 24) && near(data.strideCount, 400));
    checkCommon(data);

    const auto rowerEncode = [](const ftms::RowerData& d) { return ftms::encodeRowerData(d); };
    const auto rowerDecode = [](const std::uint8_t* d, std::size_t n, auto& out) { return ftms::decodeRowerData(d, n, out); };
    ftms::RowerData rower;
    rower.strokeRate = 48;
    rower.strokeCount = 300;
    rower.averageStrokeRate = 46;
    rower.totalDistance = 1000;
    rower.instantaneousPace = 120;
    rower.averagePace = 125;
    rower.instantaneousPower = 200;
    rower.averagePower = 190;
    rower.resistanceLevel = 7;
    fillCommon(rower);
    data = roundTrip(rower, rowerEncode, rowerDecode);
    assert(near(data.cadence, 24) && near(data.strokeCount, 300) && near(data.averageCadence, 23));
    assert(near(data.distance, 1000) && near(data.pace, 120) && near(data.averagePace, 125));
    assert(near(data.speed, 15) && near(data.averageSpeed, 14.4F));
    assert(near(data.power, 200) && near(data.averagePower, 190) && near(data.resistance, 7));
    checkCommon(data);

    // Only transmitted fields become metrics: a single field yields exactly one.
    ftms::RowerData sparse;
    sparse.heartRate = 99;
    data = roundTrip(sparse, rowerEncode, rowerDecode);
    assert(validCount(data) == 1 && near(data.heartRate, 99));
    ftms::StairClimberData floorsOnly;
    floorsOnly.floors = 3;
    data = roundTrip(floorsOnly, stairEncode, stairDecode);
    assert(validCount(data) == 1 && near(data.floors, 3));

    // Energy fields not available are sent as 0xFFFF / 0xFF and decode as absent.
    ftms::IndoorBikeData energy;
    energy.totalEnergy = 42;
    assert(ftms::encodeIndoorBikeData(energy) == (Bytes{0x01, 0x01, 42, 0, 0xFF, 0xFF, 0xFF}));
    data = roundTrip(energy, bikeEncode, bikeDecode);
    assert(validCount(data) == 1 && near(data.totalEnergy, 42));
    assert(!data.energyPerHour.valid && !data.energyPerMinute.valid);

    // Mapping by profile.
    const Bytes stepFrame{0, 0, 5, 0, 0x64, 0};
    assert(ftms::decodeMachineData(MachineProfile::StepClimber, stepFrame.data(), stepFrame.size(), data));
    assert(near(data.floors, 5) && near(data.stepCount, 100) && validCount(data) == 2);
    for (std::size_t index = 0; index < kMachineProfileCount; ++index) {
        const auto profile = static_cast<MachineProfile>(index);
        static const std::uint16_t uuids[] = {0x2AD2, 0x2ACD, 0x2ACE, 0x2AD1, 0x2ACF, 0x2AD0};
        static const std::uint8_t typeBits[] = {0x20, 0x01, 0x02, 0x10, 0x04, 0x08};
        assert(ftms::machineDataUuid(profile) == uuids[index]);
        assert(ftms::encodeAdvertisingServiceData(profile) == (Bytes{0x01, typeBits[index], 0x00}));
    }
}

void testMoreDataSplitting() {
    ftms::RowerData rower;
    rower.strokeRate = 48;
    rower.strokeCount = 300;
    rower.averageStrokeRate = 46;
    rower.totalDistance = 1000;
    rower.instantaneousPace = 120;
    rower.averagePace = 125;
    rower.instantaneousPower = 200;
    rower.averagePower = 190;
    rower.resistanceLevel = 7;
    fillCommon(rower);
    const Bytes full = ftms::encodeRowerData(rower);
    assert(full.size() > ftms::kMaxNotificationPayload);
    const auto records = ftms::encodeRecords(rower);
    assert(records.size() >= 2);
    ftms::MachineData merged;
    ftms::MachineData expected;
    assert(ftms::decodeRowerData(full.data(), full.size(), expected));
    for (std::size_t index = 0; index < records.size(); ++index) {
        const Bytes& record = records[index];
        assert(record.size() <= ftms::kMaxNotificationPayload);
        // "More Data" on every record but the last, which carries the stroke fields.
        assert(((record[0] & 1) != 0) == (index + 1 < records.size()));
        ftms::MachineData part;
        assert(ftms::decodeRowerData(record.data(), record.size(), part));
        for (std::size_t metric = 0; metric < kMetricCount; ++metric) {
            const auto key = static_cast<Metric>(metric);
            if (part[key].valid) {
                assert(!merged[key].valid);  // every field is sent exactly once
                merged[key] = part[key];
            }
        }
    }
    for (std::size_t metric = 0; metric < kMetricCount; ++metric) {
        const auto key = static_cast<Metric>(metric);
        assert(merged[key].valid == expected[key].valid && merged[key].value == expected[key].value);
    }
    // Records that fit are sent unchanged.
    ftms::IndoorBikeData bike;
    bike.instantaneousSpeed = 2500;
    bike.instantaneousPower = 100;
    assert(ftms::encodeRecords(bike) == std::vector<Bytes>{ftms::encodeIndoorBikeData(bike)});
    assert(ftms::encodeRecords(ftms::StepClimberData()) == (std::vector<Bytes>{Bytes{1, 0}}));
    ftms::CrossTrainerData backward;
    backward.movementBackward = true;
    backward.instantaneousSpeed = 500;
    fillCommon(backward);
    backward.averageSpeed = 400;
    backward.totalDistance = 10;
    backward.instantaneousPower = 1;
    for (const auto& record : ftms::encodeRecords(backward, 12)) {
        assert(record.size() <= 12 && (record[1] & 0x80));  // status flag in every record
    }
}

class FakeMachine final : public BtDeviceAdapter {
public:
    explicit FakeMachine(MachineProfile machine) : machine_(machine) {}
    void begin() override {}
    void loop() override { emitTelemetry(value); }
    bool scanAndConnect() override { value.connected = true; return true; }
    void disconnect() override { value.connected = false; }
    bool isConnected() const override { return value.connected; }
    bool trainerDetected() const override { return false; }
    MachineProfile profile() const override { return machine_; }
    FitnessTelemetry telemetry() const override { return value; }
    TrainerCapabilities capabilities() const override {
        TrainerCapabilities result;
        for (std::size_t index = 0; index < kMetricCount; ++index) result.setMetric(static_cast<Metric>(index), true);
        return result;
    }
    bool apply(const ControlRequest&) override { return false; }
    FitnessTelemetry value;

private:
    MachineProfile machine_;
};

void testNativeProfileOutput() {
    for (std::size_t index = 0; index < kMachineProfileCount; ++index) {
        const auto profile = static_cast<MachineProfile>(index);
        FakeMachine adapter(profile);
        adapter.value.cadence = 30;
        adapter.value.heartRate = 140;
        adapter.value.totalEnergy = 77;
        AntOutput ant;
        FtmsOutput output;
        BridgeCore bridge;
        std::uint32_t clock = 10000;
        output.setClock([&] { return clock; });
        bridge.attach(adapter, ant, output);
        bridge.begin();
        assert(bridge.scanAndConnect());
        bridge.loop();
        assert(output.profile() == profile);
        output.notifyClientConnected();
        bridge.loop();
        const Bytes& payload = output.lastMachineData(profile);
        assert(!payload.empty());
        for (std::size_t other = 0; other < kMachineProfileCount; ++other)
            if (other != index && other != static_cast<std::size_t>(MachineProfile::IndoorBike))
                assert(output.lastMachineData(static_cast<MachineProfile>(other)).empty());
        ftms::MachineData data;
        assert(ftms::decodeMachineData(profile, payload.data(), payload.size(), data));
        assert(near(data.heartRate, 140) && near(data.totalEnergy, 77));
        // Never transmitted by the machine, so never sent (or decoded) as 0.
        assert(!data.power.valid && !data.distance.valid && !data.elapsed.valid);
        assert(!data.energyPerHour.valid && !data.metabolicEquivalent.valid);
        if (profile != MachineProfile::Treadmill) assert(near(data.cadence, 30));
        for (const auto& record : output.lastMachineDataNotifications())
            assert(record.size() <= ftms::kMaxNotificationPayload);
    }
}

void testProfileControlsAndFeatures() {
    using P = MachineProfile;
    assert(ftms::profileAcceptsControl(P::Treadmill, Control::Speed));
    assert(ftms::profileAcceptsControl(P::CrossTrainer, Control::Speed));
    assert(!ftms::profileAcceptsControl(P::IndoorBike, Control::Speed));
    assert(!ftms::profileAcceptsControl(P::Rower, Control::Incline));
    assert(!ftms::profileAcceptsControl(P::StepClimber, Control::Incline));
    assert(!ftms::profileAcceptsControl(P::Treadmill, Control::Power));
    for (P profile : {P::IndoorBike, P::CrossTrainer, P::Rower, P::StepClimber, P::StairClimber})
        assert(ftms::profileAcceptsControl(profile, Control::Resistance) &&
               ftms::profileAcceptsControl(profile, Control::Power));
    // Speed target on a rower is refused; resistance is accepted.
    ftms::ControlPointCommand command;
    const Bytes speed{2, 0x52, 3};
    assert(ftms::decodeControlPoint(speed.data(), speed.size(), command, P::Rower));
    assert(command.status == ResultCode::NotSupported && !command.hasRequest);
    const Bytes resistance{4, 50};
    assert(ftms::decodeControlPoint(resistance.data(), resistance.size(), command, P::StairClimber));
    assert(command.status == ResultCode::Success && command.hasRequest &&
           command.request.control == Control::Resistance);
    // Feature bits only announce fields the profile's data can carry.
    TrainerCapabilities capabilities;
    capabilities.setMetric(Metric::Floors, true);
    capabilities.setMetric(Metric::Pace, true);
    capabilities.setMetric(Metric::Cadence, true);
    const auto stepFeature = ftms::encodeFeature(capabilities, P::StepClimber);
    const auto rowerFeature = ftms::encodeFeature(capabilities, P::Rower);
    ftms::Features step;
    ftms::Features row;
    assert(ftms::decodeFeature(stepFeature.data(), stepFeature.size(), step));
    assert(ftms::decodeFeature(rowerFeature.data(), rowerFeature.size(), row));
    assert(step.machine & ftms::kFeatureStepCount);
    assert(!(step.machine & ftms::kFeaturePace));
    assert(row.machine & ftms::kFeaturePace);
    assert(row.machine & ftms::kFeatureCadence);
}

}  // namespace

void runFtmsTests() {
    testAllProfileCodecs();
    testMoreDataSplitting();
    testNativeProfileOutput();
    testProfileControlsAndFeatures();
    testTreadmillCodec();
    testTreadmillOutput();
    testNativeTreadmillRunState();
    testF15BridgeToFtms();
    testFtmsClientAddress();
    testIndoorBikeDataEncoding();
    testStaticCharacteristics();
    testControlPointDecoding();
    testResponsesAndStatus();
    testFtmsOutputControlFlow();
    testFtmsNameAndConsole();
}
