#include "bridge/bridge_core.hpp"
#include "bridge/ftms_roles.hpp"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"
#include "config_store.h"
#include "web_ui.h"

#include <cassert>
#include <cmath>

using namespace smartfitness;

namespace {

class FakeAdapter final : public TrainerControlAdapter {
public:
    bool accept = true;
    unsigned int calls = 0;
    ControlRequest lastRequest{};

    bool apply(const ControlRequest& request) override {
        ++calls;
        lastRequest = request;
        return accept;
    }
};

class RuntimeAdapter final : public BtDeviceAdapter {
public:
    bool connected = false;
    bool trainer = false;
    bool disconnectOnApply = false;
    bool accept = true;
    unsigned int calls = 0;
    TrainerCapabilities supported;

    void begin() override {}
    void loop() override {}
    bool scanAndConnect() override { connected = true; return true; }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    bool trainerDetected() const override { return trainer; }
    TrainerCapabilities capabilities() const override { return supported; }
    FitnessTelemetry telemetry() const override {
        FitnessTelemetry value;
        value.connected = connected;
        value.trainerDetected = trainer;
        value.power = 180;
        return value;
    }
    bool apply(const ControlRequest&) override {
        ++calls;
        if (disconnectOnApply) connected = false;
        return accept;
    }
    void emit(const FitnessTelemetry& value) { emitTelemetry(value); }
};

void testCapabilitiesAndSamples() {
    TrainerState state;
    state.capabilities.setMetric(Metric::Power, true);
    state.capabilities.setMetric(Metric::HeartRate, true);
    state.capabilities.setControl(Control::Incline, true);
    state.setSample(Metric::Power, 175.0F);
    state.setSample(Metric::HeartRate, 142.0F);

    assert(state.capabilities.supports(Metric::Power));
    assert(!state.capabilities.supports(Metric::Speed));
    assert(state.capabilities.supports(Control::Incline));
    assert(!state.capabilities.supports(Control::Resistance));
    assert(state.sample(Metric::Power).valid);
    assert(state.sample(Metric::Power).value == 175.0F);
    state.capabilities.setMetric(static_cast<Metric>(255), true);
    state.setSample(static_cast<Metric>(255), 99.0F);
    assert(!state.capabilities.supports(static_cast<Metric>(255)));
    assert(!state.sample(static_cast<Metric>(255)).valid);
}

void testFtmsRolesAreIndependent() {
    constexpr FtmsRoles inputOnly{true, false};
    constexpr FtmsRoles outputOnly{false, true};
    constexpr FtmsRoles both{true, true};
    constexpr FtmsRoles disabled{};

    static_assert(inputOnly.enabled());
    static_assert(outputOnly.enabled());
    static_assert(both.enabled());
    static_assert(!disabled.enabled());
}

void testControlGating() {
    FakeAdapter adapter;
    BridgeCore bridge(&adapter);
    TrainerState state;
    state.connected = true;
    state.trainerDetected = true;
    state.capabilities.setControl(Control::Speed, true);
    state.capabilities.setControl(Control::Incline, true);
    state.capabilities.setControl(Control::Resistance, true);
    bridge.setTrainerState(state);

    assert(bridge.routeControl({Control::Speed, 5.0F}) == ControlResult::Applied);
    assert(adapter.calls == 1);
    assert(adapter.lastRequest.value == 5.0F);
    assert(bridge.routeControl({Control::Speed, -1.0F}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({Control::Speed, INFINITY}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({Control::Incline, 25.1F}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({Control::Resistance, 101.0F}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({Control::Count, 0.0F}) == ControlResult::Unsupported);
    assert(adapter.calls == 1);

    adapter.accept = false;
    assert(bridge.routeControl({Control::Speed, 0.0F}) == ControlResult::AdapterRejected);
    assert(adapter.calls == 2);
}

void testDisconnectedAndMissingAdapter() {
    FakeAdapter adapter;
    BridgeCore bridge(&adapter);
    TrainerState state;
    state.capabilities.setControl(Control::Speed, true);
    bridge.setTrainerState(state);
    assert(bridge.routeControl({Control::Speed, 2.0F}) == ControlResult::Disconnected);

    state.connected = true;
    state.trainerDetected = true;
    state.capabilities.setControl(Control::Speed, false);
    bridge.setTrainerState(state);
    assert(bridge.routeControl({Control::Speed, 2.0F}) == ControlResult::Unsupported);

    state.capabilities.setControl(Control::Speed, true);
    bridge.setTrainerState(state);
    bridge.setAdapter(nullptr);
    assert(bridge.routeControl({Control::Speed, 2.0F}) == ControlResult::NoAdapter);
}

void testRuntimeRouting() {
    SimulatedDeviceAdapter adapter;
    AntOutput ant;
    FtmsOutput ftms;
    BridgeCore bridge;
    bridge.attach(adapter, ant, ftms);
    bridge.begin();
    assert(bridge.state() == RuntimeState::Disconnected);
    assert(!ant.telemetry().connected);
    assert(bridge.scanAndConnect());
    bridge.loop();
    assert(bridge.state() == RuntimeState::ControlReady);
    assert(ant.telemetry().power == 150);
    assert(ftms.telemetry().cadence == 80);
    assert(bridge.trainerState().sample(Metric::Power).valid);

    assert(ftms.requestControl({Control::Power, 220}) == ControlResult::Applied);
    bridge.loop();
    assert(bridge.hasControlTarget());
    assert(ant.controlTarget().value == 220);
    assert(ftms.controlTarget().control == Control::Power);
    assert(ant.telemetry().power == 220);
    assert(ant.requestControl({Control::Power, NAN}) == ControlResult::InvalidValue);
    assert(bridge.controlTarget().value == 220);

    assert(bridge.scanAndConnect());
    bridge.loop();
    assert(!bridge.hasControlTarget() && !ant.hasControlTarget() && !ftms.hasControlTarget());
    assert(ant.telemetry().power == 150 && ftms.telemetry().targetPower == 0);
    assert(ftms.requestControl({Control::Power, 220}) == ControlResult::Applied);
    bridge.loop();

    adapter.setTrainer(false);
    bridge.loop();
    assert(bridge.state() == RuntimeState::Connected);
    assert(!bridge.canControl());
    assert(!ant.hasControlTarget() && !ftms.hasControlTarget());
    assert(ftms.requestControl({Control::Power, 300}) == ControlResult::NotTrainer);
    assert(ant.telemetry().power == 220);

    adapter.setTrainer(true);
    assert(bridge.routeControl({Control::Power, 100}) == ControlResult::Applied);
    adapter.disconnect();
    assert(bridge.routeControl({Control::Power, 200}) == ControlResult::Disconnected);
    bridge.loop();
    assert(bridge.state() == RuntimeState::Disconnected);
    assert(!bridge.hasControlTarget() && !ant.hasControlTarget());
    assert(!ftms.telemetry().connected && ftms.telemetry().power == 0);
    assert(!bridge.trainerState().sample(Metric::Power).valid);
}

void testStatesAndBoundaries() {
    FakeAdapter adapter;
    BridgeCore bridge(&adapter);
    TrainerState state;
    state.connected = true;
    state.capabilities.setControl(Control::Power, true);
    bridge.setTrainerState(state);
    assert(bridge.routeControl({Control::Power, 100}) == ControlResult::NotTrainer);
    assert(adapter.calls == 0);
    state.trainerDetected = true;
    state.capabilities.controls = 0;
    bridge.setTrainerState(state);
    assert(bridge.state() == RuntimeState::Trainer);
    state.capabilities.setControl(Control::Power, true);
    bridge.setTrainerState(state);
    assert(bridge.routeControl({Control::Power, 0}) == ControlResult::Applied);
    assert(bridge.routeControl({Control::Power, 2000}) == ControlResult::Applied);
    assert(bridge.routeControl({Control::Power, 2001}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({Control::Power, -1}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({static_cast<Control>(255), 1}) == ControlResult::Unsupported);
    adapter.accept = false;
    assert(bridge.routeControl({Control::Power, 150}) == ControlResult::AdapterRejected);
    assert(bridge.controlTarget().value == 2000);
}

void testConsoleAndConfig() {
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
    assert(ui.handleCommand("power 100") == "control: disconnected");
    assert(ui.handleCommand("scan").find("simulation connected") == 0);
    assert(config.hasPairedDevice());
    assert(ui.handleCommand("power 175") == "control: applied");
    assert(ui.handleCommand("power nan") == "control: invalid value");
    assert(ui.handleCommand("power 1garbage") == "invalid value");
    assert(ui.handleCommand(std::string("power 1\0garbage", 15)) == "invalid value");
    assert(ui.handleCommand("power ") == "invalid value");
    assert(ui.handleCommand("trainer maybe") == "expected on or off");
    assert(ui.handleCommand(std::string(97, 'a')) == "command too long");
    ui.handleCommand("ant off");
    assert(!ant.enabled());
    assert(!ant.hasControlTarget());
    assert(ant.requestControl({Control::Power, 150}) == ControlResult::NoAdapter);
    assert(adapter.telemetry().targetPower == 175);
    ui.handleCommand("ant on");
    assert(ant.telemetry().power == 175);
    assert(ant.hasControlTarget() && ant.controlTarget().value == 175);
    ui.handleCommand("ftms-input on");
    ui.handleCommand("ftms-output off");
    assert(config.config().ftms.input && !config.config().ftms.output);
    assert(!ftms.enabled());
    ui.handleCommand("save");
    ui.handleCommand("unpair");
    ui.handleCommand("trainer off");
    ui.handleCommand("ftms-input off");
    assert(!config.hasPairedDevice());
    ui.handleCommand("load");
    assert(!config.hasPairedDevice());  // unpair is persisted immediately
    assert(config.config().simulatedTrainer);
    assert(config.config().ftms.input && !config.config().ftms.output);
    assert(bridge.state() == RuntimeState::Disconnected);
    ui.handleCommand("scan");
    ui.handleCommand("trainer off");
    assert(ui.handleCommand("incline 5") == "control: not a trainer");
    assert(ui.status().find("connected/read-only") != std::string::npos);

    ConfigStore freshStore;
    freshStore.begin();
    assert(!freshStore.hasPairedDevice());
    assert(!freshStore.config().ftms.input && freshStore.config().ftms.output);
}

void testAuthoritativeAdapterState() {
    RuntimeAdapter adapter;
    AntOutput ant;
    FtmsOutput ftms;
    BridgeCore bridge;
    bridge.attach(adapter, ant, ftms);
    bridge.begin();
    FitnessTelemetry spoofed;
    spoofed.connected = true;
    spoofed.trainerDetected = true;
    adapter.emit(spoofed);
    assert(!bridge.telemetry().connected);
    assert(bridge.routeControl({Control::Power, 100}) == ControlResult::Disconnected);
    assert(adapter.calls == 0);

    adapter.connected = true;
    adapter.trainer = true;
    adapter.supported.setControl(Control::Power, true);
    adapter.supported.setMetric(Metric::Power, true);
    adapter.emit(adapter.telemetry());
    assert(bridge.routeControl({Control::Power, 200}) == ControlResult::Applied);
    bridge.loop();
    assert(ant.hasControlTarget());
    adapter.supported.setControl(Control::Power, false);
    adapter.supported.setMetric(Metric::Power, false);
    bridge.loop();
    assert(bridge.state() == RuntimeState::Trainer);
    assert(!bridge.trainerState().sample(Metric::Power).valid);
    assert(!bridge.hasControlTarget() && !ant.hasControlTarget() && !ftms.hasControlTarget());
    assert(bridge.routeControl({Control::Power, 200}) == ControlResult::Unsupported);

    adapter.supported.setControl(Control::Power, true);
    adapter.disconnectOnApply = true;
    assert(bridge.routeControl({Control::Power, 150}) == ControlResult::AdapterRejected);
    bridge.loop();
    assert(!bridge.hasControlTarget() && !ant.telemetry().connected);
}

void testReattachmentAndCallbackCleanup() {
    SimulatedDeviceAdapter first;
    SimulatedDeviceAdapter second(false);
    AntOutput firstAnt;
    AntOutput secondAnt;
    FtmsOutput firstFtms;
    FtmsOutput secondFtms;
    {
        BridgeCore bridge;
        bridge.attach(first, firstAnt, firstFtms);
        bridge.begin();
        bridge.scanAndConnect();
        assert(firstAnt.requestControl({Control::Power, 180}) == ControlResult::Applied);
        bridge.attach(second, secondAnt, secondFtms);
        assert(bridge.state() == RuntimeState::Disconnected);
        assert(!bridge.hasControlTarget());
        assert(firstAnt.requestControl({Control::Power, 180}) == ControlResult::NoAdapter);
        first.scanAndConnect();
        assert(bridge.state() == RuntimeState::Disconnected);
        bridge.scanAndConnect();
        bridge.loop();
        assert(bridge.state() == RuntimeState::Connected);
        assert(!secondFtms.hasControlTarget());
    }
    assert(secondAnt.requestControl({Control::Power, 180}) == ControlResult::NoAdapter);
    second.scanAndConnect();
}

}  // namespace

void runFtmsTests();
void runConfigStoreTests();
void runBleInputTests();
void runWebTests();
void runIfitBowflexTests();
void runTrainersRowerTests();
void runTreadmillTests();

int main() {
    testCapabilitiesAndSamples();
    testFtmsRolesAreIndependent();
    testControlGating();
    testDisconnectedAndMissingAdapter();
    testRuntimeRouting();
    testStatesAndBoundaries();
    testConsoleAndConfig();
    testAuthoritativeAdapterState();
    testReattachmentAndCallbackCleanup();
    runFtmsTests();
    runConfigStoreTests();
    runBleInputTests();
    runWebTests();
    runIfitBowflexTests();
    runTrainersRowerTests();
    runTreadmillTests();
}
