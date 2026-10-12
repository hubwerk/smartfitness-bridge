// BridgeController + view model: the variant-neutral front-end layer used by
// both the web variant (JSON) and the HMI variant (LVGL). Core only: no web
// or HMI header is included here.
#include <cassert>
#include <cmath>
#include <string>

#include "ant_output.h"
#include "ble_input_adapter.h"
#include "bridge/bridge_core.hpp"
#include "bridge_commands.h"
#include "bridge_controller.h"
#include "bridge_view_model.h"
#include "config_store.h"
#include "fake_central.hpp"
#include "ftms_output.h"
#include "protocols/registry.hpp"

using namespace smartfitness;
using smartfitness::testing::FakeCentral;
using smartfitness::testing::address;

namespace {

struct ControllerRig {
    std::uint32_t now = 1000;
    FakeCentral central;
    MemoryConfigBackend backend;
    BleInputAdapter ble{central, [this] { return now; }};
    SimulatedDeviceAdapter simulation;
    InputSelector selector{simulation, &ble};
    AntOutput ant;
    FtmsOutput ftms;
    ConfigStore config{backend};
    BridgeCore core;
    BridgeController controller{core, config, simulation, ant, ftms};

    ControllerRig() {
        config.begin();
        core.attach(selector, ant, ftms);
        core.begin();
        controller.attachBleInput(selector, ble);
        controller.begin();
    }
};

Command make(CommandType type) {
    Command command;
    command.type = type;
    return command;
}

Command control(Control which, float value) {
    Command command = make(CommandType::Control);
    command.control = which;
    command.value = value;
    return command;
}

void testRuntimeStateNames() {
    assert(std::string(runtimeStateName(RuntimeState::Disconnected)) == "disconnected");
    assert(std::string(runtimeStateName(RuntimeState::Connected)) == "connected/read-only");
    assert(std::string(runtimeStateName(RuntimeState::Trainer)) == "trainer");
    assert(std::string(runtimeStateName(RuntimeState::ControlReady)) == "control-ready");
}

void testDefaultViews() {
    ControllerRig rig;
    const StatusView status = rig.controller.statusView();
    assert(status.version == SMARTFITNESS_VERSION);
    assert(status.state == RuntimeState::Disconnected);
    assert(!status.metric(Metric::Power).valid && !status.hasTarget && !status.canControl);
    assert(!status.input.bleSelected && status.input.bleAvailable && status.input.link == "idle");
    assert(!status.input.paired.valid && !status.input.scanning);
    assert(status.ftmsOutput.enabled && status.ftmsOutput.name == FtmsOutput::kDefaultName);
    assert(status.ftmsOutput.override == FtmsOutputProfile::Auto && !status.unsaved);

    const ConfigView config = rig.controller.configView();
    assert(config.ftmsName == FtmsOutput::kDefaultName && config.ftmsOutput && !config.ftmsInput);
    assert(config.ant && config.antRequiresHardware && config.simulatedTrainer);
    assert(config.wifiSsid.empty() && !config.wifiPasswordSet && config.apPasswordDefault && !config.unsaved);

    const ScanView scan = rig.controller.scanView();
    assert(scan.bleAvailable && !scan.scanning && scan.devices.empty() && scan.dropped == 0);
}

void testSimulationTelemetryAndTargets() {
    ControllerRig rig;
    // Same Command objects the HMI submits; same results as the console.
    assert(!rig.controller.execute(control(Control::Power, 150.0F)).ok);
    const CommandResult sim = rig.controller.execute(make(CommandType::Simulate));
    assert(sim.ok && sim.message.find("simulation connected") == 0);
    const CommandResult applied = rig.controller.execute(control(Control::Power, 180.0F));
    assert(applied.ok && applied.message == "control: applied");
    const CommandResult rejected = rig.controller.execute(control(Control::Power, NAN));
    assert(!rejected.ok && rejected.message == "control: invalid value");

    const StatusView status = rig.controller.statusView();
    assert(status.state == RuntimeState::ControlReady && status.canControl && status.trainer);
    assert(status.metric(Metric::Power).valid && status.metric(Metric::Cadence).valid);
    assert(status.supports(Control::Power));
    assert(status.hasTarget && status.target.control == Control::Power && status.target.value == 180.0F);
    assert(!status.input.bleSelected && status.deviceName == rig.core.telemetry().deviceName);

    // A read-only simulation rejects control with the core reason.
    Command trainer = make(CommandType::Trainer);
    trainer.flag = false;
    assert(rig.controller.execute(trainer).ok);
    const CommandResult notTrainer = rig.controller.execute(control(Control::Incline, 5.0F));
    assert(!notTrainer.ok && notTrainer.message == "control: not a trainer");
    assert(rig.controller.statusView().state == RuntimeState::Connected);
}

void testScanAndPairViews() {
    ControllerRig rig;
    Command scan = make(CommandType::Scan);
    scan.durationMs = 5000;
    assert(rig.controller.execute(scan).ok);
    rig.central.advertise(address("aa:00:00:00:00:21"), "KICKR CORE 1234", {0x1826, 0x1818});
    rig.central.advertise(address("aa:00:00:00:00:22"), "Mystery", {});
    rig.core.loop();
    ScanView view = rig.controller.scanView();
    assert(view.scanning && view.devices.size() == 2);
    assert(view.devices[0].address == "aa:00:00:00:00:21" && view.devices[0].adapter == "ftms");
    assert(view.devices[0].name == "KICKR CORE 1234" && view.devices[0].services.size() == 2);
    assert(view.devices[1].adapter.empty());
    assert(rig.controller.statusView().input.scanning);

    // Pair through a Command with an adapter override (HMI adapter picker).
    Command pair = make(CommandType::Pair);
    assert(ble::BleAddress::parse("aa:00:00:00:00:22", pair.address));
    pair.adapterId = "echelon";
    const CommandResult paired = rig.controller.execute(pair);
    assert(paired.ok && paired.message.find("as echelon") != std::string::npos);
    const StatusView status = rig.controller.statusView();
    assert(status.input.bleSelected && status.input.paired.valid);
    assert(status.input.paired.address == "aa:00:00:00:00:22" && status.input.paired.adapter == "echelon");
    assert(status.input.paired.name == "Mystery" && status.input.link == "connecting");

    assert(rig.controller.execute(make(CommandType::Unpair)).ok);
    assert(!rig.controller.statusView().input.paired.valid);
}

void testScanSortingAndRelevance() {
    ControllerRig rig;
    Command scan = make(CommandType::Scan);
    scan.durationMs = 15000;
    assert(rig.controller.execute(scan).ok);

    // Dev 1: Detected adapter (echelon), RSSI -70
    rig.central.advertise(address("aa:00:00:00:00:01"), "ECH-SPORT", {}, {}, -70);
    // Dev 2: Detected adapter (ftms), RSSI -50
    rig.central.advertise(address("aa:00:00:00:00:02"), "KICKR CORE", {0x1826}, {}, -50);
    // Dev 3: Relevant by name (no adapter, no known service), RSSI -60
    rig.central.advertise(address("aa:00:00:00:00:03"), "Headphones", {}, {}, -60);
    // Dev 4: Relevant by known service (RSC 0x1814, no name, no adapter), RSSI -40
    rig.central.advertise(address("aa:00:00:00:00:04"), "", {0x1814}, {}, -40);
    // Dev 5: Non-relevant (no name, unknown service, Apple mfg data 0x004C), RSSI -30
    rig.central.advertise(address("aa:00:00:00:00:05"), "", {0x1234}, {0x4C, 0x00}, -30);
    // Dev 6: Non-relevant (no name, unknown mfg data 0xABCD), RSSI -20
    rig.central.advertise(address("aa:00:00:00:00:06"), "", {}, {0xCD, 0xAB}, -20);
    // Dev 7: Non-relevant (no name, no service, no mfg data), RSSI -10
    rig.central.advertise(address("aa:00:00:00:00:07"), "", {}, {}, -10);

    rig.core.loop();
    ScanView view = rig.controller.scanView();
    assert(view.devices.size() == 7);

    // Check order:
    // 1: Dev 2 (detected adapter ftms, -50)
    // 2: Dev 1 (detected adapter echelon, -70)
    // 3: Dev 4 (relevant RSC, -40)
    // 4: Dev 3 (relevant Headphones, -60)
    // 5: Dev 7 (non-relevant address only, -10)
    // 6: Dev 6 (non-relevant manufacturer 0xABCD, -20)
    // 7: Dev 5 (non-relevant Apple mfg, -30)

    assert(view.devices[0].address == "aa:00:00:00:00:02");
    assert(view.devices[0].adapter == "ftms");
    assert(view.devices[0].label == "KICKR CORE");
    assert(view.devices[0].relevant == true);

    assert(view.devices[1].address == "aa:00:00:00:00:01");
    assert(view.devices[1].adapter == "echelon");
    assert(view.devices[1].label == "ECH-SPORT");
    assert(view.devices[1].relevant == true);

    assert(view.devices[2].address == "aa:00:00:00:00:04");
    assert(view.devices[2].adapter.empty());
    assert(view.devices[2].label == "Running Speed/Cadence");
    assert(view.devices[2].relevant == true);

    assert(view.devices[3].address == "aa:00:00:00:00:03");
    assert(view.devices[3].adapter.empty());
    assert(view.devices[3].label == "Headphones");
    assert(view.devices[3].relevant == true);

    assert(view.devices[4].address == "aa:00:00:00:00:07");
    assert(view.devices[4].label == "aa:00:00:00:00:07");
    assert(view.devices[4].relevant == false);

    assert(view.devices[5].address == "aa:00:00:00:00:06");
    assert(view.devices[5].label == "Hersteller 0xABCD");
    assert(view.devices[5].relevant == false);

    assert(view.devices[6].address == "aa:00:00:00:00:05");
    assert(view.devices[6].label == "Apple");
    assert(view.devices[6].relevant == false);
}

void testConfigViewTracksCommands() {
    ControllerRig rig;
    Command name = make(CommandType::FtmsName);
    name.text = "Garage Bike";
    assert(rig.controller.execute(name).ok);
    Command profile = make(CommandType::FtmsProfile);
    profile.text = "treadmill";
    assert(rig.controller.execute(profile).ok);
    Command ant = make(CommandType::Ant);
    ant.flag = false;
    assert(rig.controller.execute(ant).ok);
    Command wifi = make(CommandType::Wifi);
    wifi.text = "Home";
    wifi.text2 = "secret-pass";
    assert(rig.controller.execute(wifi).ok);

    ConfigView config = rig.controller.configView();
    assert(config.ftmsName == "Garage Bike" && config.ftmsOutputProfile == FtmsOutputProfile::Treadmill);
    assert(!config.ant && config.wifiSsid == "Home" && config.wifiPasswordSet && config.unsaved);
    StatusView status = rig.controller.statusView();
    assert(status.ftmsOutput.name == "Garage Bike" && status.ftmsOutput.override == FtmsOutputProfile::Treadmill);
    assert(status.ftmsOutput.profile == MachineProfile::Treadmill && !status.ant && status.unsaved);

    assert(rig.controller.execute(make(CommandType::Save)).ok);
    assert(!rig.controller.configView().unsaved);
    const CommandResult reset = rig.controller.execute(make(CommandType::FactoryReset));
    assert(reset.ok && reset.message.find("factory defaults restored") == 0);
    config = rig.controller.configView();
    assert(config.ftmsName == FtmsOutput::kDefaultName && config.ant && config.wifiSsid.empty());
    assert(!rig.controller.restartRequested());
    assert(rig.controller.execute(make(CommandType::Restart)).ok && rig.controller.restartRequested());
}

void testAdapterViews() {
    const auto adapters = adapterViews();
    assert(adapters.size() == protocols::protocolCount() && !adapters.empty());
    bool ftms = false;
    bool ifitModel = false;
    for (const auto& adapter : adapters) {
        assert(adapter.id && adapter.label && *adapter.id && *adapter.label);
        assert(protocols::findProtocol(adapter.id) != nullptr);
        if (std::string(adapter.id) == "ftms") ftms = true;
        if (std::string(adapter.id).rfind("ifit-model-", 0) == 0) ifitModel = true;
    }
    assert(ftms && ifitModel);
}

void testWithoutBleInput() {
    MemoryConfigBackend backend;
    SimulatedDeviceAdapter simulation;
    AntOutput ant;
    FtmsOutput ftms;
    ConfigStore config{backend};
    BridgeCore core;
    BridgeController controller{core, config, simulation, ant, ftms};
    config.begin();
    core.attach(simulation, ant, ftms);
    core.begin();
    controller.begin();
    assert(!controller.statusView().input.bleAvailable && !controller.scanView().bleAvailable);
    assert(!controller.execute(make(CommandType::ScanStop)).ok);
    assert(controller.execute(make(CommandType::Scan)).message.find("simulation connected") == 0);
    assert(controller.statusView().state == RuntimeState::ControlReady);
}

}  // namespace

void runControllerTests() {
    testRuntimeStateNames();
    testDefaultViews();
    testSimulationTelemetryAndTargets();
    testScanAndPairViews();
    testScanSortingAndRelevance();
    testConfigViewTracksCommands();
    testAdapterViews();
    testWithoutBleInput();
}
