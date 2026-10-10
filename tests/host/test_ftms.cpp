#include "bridge/bridge_core.hpp"
#include "ftms/ftms_codec.hpp"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"
#include "config_store.h"
#include "web_ui.h"

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
    WebUi ui(bridge, config, adapter, ant, ftms);
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

}  // namespace

void runFtmsTests() {
    testFtmsClientAddress();
    testIndoorBikeDataEncoding();
    testStaticCharacteristics();
    testControlPointDecoding();
    testResponsesAndStatus();
    testFtmsOutputControlFlow();
    testFtmsNameAndConsole();
}
