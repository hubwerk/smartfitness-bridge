// BridgeController + view model: the variant-neutral front-end layer used by
// both the web variant (JSON) and the HMI variant (LVGL). Core only: no web
// or HMI header is included here.
#include <cassert>
#include <cmath>
#include <string>

#include "ant_output.h"
#include "ble_input_adapter.h"
#include "bridge/bridge_core.hpp"
#include "bridge/output_services.hpp"
#include "bridge_commands.h"
#include "bridge_controller.h"
#include "bridge_view_model.h"
#include "config_store.h"
#include "fake_ant.hpp"
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
    FakeAntTransport antRadio;

    explicit ControllerRig(bool withAnt = false) {
        if (withAnt) ant.setTransport(&antRadio);
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
    assert(config.ftmsName == FtmsOutput::kDefaultName && !config.ftmsInput);
    assert(config.outputProtocol == OutputProtocol::BleFtms && !config.antAvailable && config.simulatedTrainer);
    assert(status.outputProtocol == OutputProtocol::BleFtms && !status.antOutput.enabled);
    assert(!status.antOutput.available && status.antOutput.link == "disabled");
    assert(status.antOutput.error.empty() && status.antOutput.channelLimit == 0);
    assert(config.wifiSsid.empty() && !config.wifiPasswordSet && config.apPasswordDefault && !config.unsaved);
    assert(config.ftmsOutputServices == 0 && config.antOutputServices == 0);
    assert(status.ftmsOutput.services.size() == 5 && status.antOutput.channels.size() == 5);
    assert(status.ftmsOutput.services[0].id == "ftms" && status.ftmsOutput.services[0].enabled &&
           status.ftmsOutput.services[0].available);
    for (std::size_t i = 1; i < 5; ++i) {
        assert(!status.ftmsOutput.services[i].enabled && !status.ftmsOutput.services[i].available);
        assert(!status.antOutput.channels[i].enabled && !status.antOutput.channels[i].open);
    }
    assert(status.antOutput.channels[0].id == "fec" && status.antOutput.channels[0].deviceType == 17);
    assert(status.antOutput.channels[1].deviceType == 11 && status.antOutput.channels[2].deviceType == 120 &&
           status.antOutput.channels[3].deviceType == 121 && status.antOutput.channels[4].deviceType == 124);

    const ScanView scan = rig.controller.scanView();
    assert(scan.bleAvailable && !scan.scanning && scan.devices.empty() && scan.dropped == 0);
}

void testFtmsClientView() {
    ControllerRig rig;
    StatusView status = rig.controller.statusView();
    assert(!status.ftmsOutput.client && status.ftmsOutput.clientAddress.empty());
    rig.ftms.setClientAddress({0xAA, 0xBB, 0xCC, 0x00, 0x11, 0x22}, 1);
    rig.ftms.notifyClientConnected();
    rig.ftms.loop();
    status = rig.controller.statusView();
    assert(status.ftmsOutput.client && status.ftmsOutput.clientAddress == "aa:bb:cc:00:11:22");
    assert(status.ftmsOutput.clientRandomAddress);
    rig.ftms.clearClientAddress();
    rig.ftms.notifyClientDisconnected();
    rig.ftms.loop();
    status = rig.controller.statusView();
    assert(!status.ftmsOutput.client && status.ftmsOutput.clientAddress.empty());
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

void testPairedServiceAndSubscriptionViews() {
    ControllerRig rig;
    assert(rig.controller.execute(make(CommandType::Scan)).ok);
    const auto source = address("aa:00:00:00:00:31");
    rig.central.advertise(source, "Bike", {0x1826, 0x180D});
    rig.core.loop();
    Command pair = make(CommandType::Pair);
    pair.address = source;
    pair.adapterId = "ftms";
    assert(rig.controller.execute(pair).ok);
    auto paired = rig.controller.statusView().input.paired;
    assert(paired.advertisedServices == std::vector<std::string>({"1826", "180d"}));
    assert(!paired.subscriptions.empty());
    assert(paired.subscriptions.size() == rig.ble.subscriptions().size());
    bool sawIndication = false;
    for (std::size_t i = 0; i < paired.subscriptions.size(); ++i) {
        const auto& sourceSubscription = rig.ble.subscriptions()[i];
        const auto& view = paired.subscriptions[i];
        assert(view.service == "1826");
        assert(view.characteristic == sourceSubscription.characteristic.toString().substr(2));
        assert(view.state == "pending" && view.indicate == sourceSubscription.indicate);
        sawIndication = sawIndication || view.indicate;
    }
    assert(sawIndication);
    const auto& planned = rig.central.connects.back().plan.subscriptions[4];
    rig.central.pushSubscriptionResult(4, planned.service, planned.characteristic, planned.indicate,
                                       ble::SubscriptionOutcome::NotFound);
    rig.central.pushEvent(ble::CentralEventType::Connected, 1);
    rig.core.loop();
    paired = rig.controller.statusView().input.paired;
    assert(paired.subscriptions[0].state == "ok" && paired.subscriptions[0].reason.empty());
    assert(paired.subscriptions[1].reason == "not-found");
    assert(paired.subscriptions.size() == 2);  // absent optional data UUIDs stay hidden
    for (std::size_t i = 1; i < paired.subscriptions.size(); ++i)
        assert(paired.subscriptions[i].state == "failed");
    assert(rig.config.config().pairedAdvertisedServices == paired.advertisedServices);
    assert(rig.controller.execute(make(CommandType::Disconnect)).ok);
    assert(rig.controller.statusView().input.paired.advertisedServices.empty());
    ControllerRig rebooted;
    rebooted.backend.setRecord(rig.backend.record());
    assert(rebooted.controller.execute(make(CommandType::Load)).ok);
    assert(rebooted.ble.pairedDevice().advertisedServices ==
           std::vector<ble::Uuid>({ble::Uuid::from16(0x1826), ble::Uuid::from16(0x180D)}));
    assert(rebooted.controller.execute(make(CommandType::Connect)).ok);
    assert(rebooted.controller.statusView().input.paired.advertisedServices ==
           std::vector<std::string>({"1826", "180d"}));
    assert(rig.controller.execute(make(CommandType::Unpair)).ok);
    paired = rig.controller.statusView().input.paired;
    assert(!paired.valid && paired.advertisedServices.empty() && paired.subscriptions.empty());
}

void testConfigViewTracksCommands() {
    ControllerRig rig;
    Command name = make(CommandType::FtmsName);
    name.text = "Garage Bike";
    assert(rig.controller.execute(name).ok);
    Command profile = make(CommandType::FtmsProfile);
    profile.text = "treadmill";
    assert(rig.controller.execute(profile).ok);
    Command output = make(CommandType::OutputProtocol);
    output.text = "off";
    assert(rig.controller.execute(output).ok);
    Command wifi = make(CommandType::Wifi);
    wifi.text = "Home";
    wifi.text2 = "secret-pass";
    assert(rig.controller.execute(wifi).ok);

    ConfigView config = rig.controller.configView();
    assert(config.ftmsName == "Garage Bike" && config.ftmsOutputProfile == FtmsOutputProfile::Treadmill);
    assert(config.outputProtocol == OutputProtocol::Off && config.wifiSsid == "Home" && config.wifiPasswordSet && config.unsaved);
    StatusView status = rig.controller.statusView();
    assert(status.ftmsOutput.name == "Garage Bike" && status.ftmsOutput.override == FtmsOutputProfile::Treadmill);
    assert(status.ftmsOutput.profile == MachineProfile::Treadmill && !status.ftmsOutput.enabled && status.unsaved);

    assert(rig.controller.execute(make(CommandType::Save)).ok);
    assert(!rig.controller.configView().unsaved);
    const CommandResult reset = rig.controller.execute(make(CommandType::FactoryReset));
    assert(reset.ok && reset.message.find("factory defaults restored") == 0);
    config = rig.controller.configView();
    assert(config.ftmsName == FtmsOutput::kDefaultName && config.outputProtocol == OutputProtocol::BleFtms &&
           config.wifiSsid.empty());
    assert(!rig.controller.restartRequested());
    assert(rig.controller.execute(make(CommandType::Restart)).ok && rig.controller.restartRequested());
}

void testOutputProtocolSelection() {
    {
        ControllerRig rig;
        Command output = make(CommandType::OutputProtocol);
        output.text = "ant-fec";
        const CommandResult rejected = rig.controller.execute(output);
        assert(!rejected.ok && rig.controller.configView().outputProtocol == OutputProtocol::BleFtms);
        output.text = "usb";
        assert(!rig.controller.execute(output).ok);
    }
    ControllerRig rig(true);
    assert(rig.controller.configView().antAvailable);
    assert(rig.ftms.enabled() && !rig.ant.enabled());
    Command output = make(CommandType::OutputProtocol);
    output.text = "ant-fec";
    assert(rig.controller.execute(output).ok);
    assert(!rig.ftms.enabled() && rig.ant.enabled());
    rig.ant.setClock([&rig] { return rig.now; });
    for (int index = 0; index < 16 && !rig.ant.channelOpen(); ++index) rig.core.loop();
    StatusView status = rig.controller.statusView();
    assert(status.outputProtocol == OutputProtocol::AntFec && status.antOutput.enabled && status.antOutput.available);
    assert(status.antOutput.link == "open" && status.antOutput.stick && status.antOutput.open);
    assert(status.antOutput.hardware == "fake" && !status.antOutput.display && !status.antOutput.control);
    assert(rig.controller.handleCommand("status").find("output=ant-fec") != std::string::npos);

    // The simulated trainer's data reaches the FE-C pages.
    assert(rig.controller.execute(make(CommandType::Simulate)).ok);
    Command power = control(Control::Power, 220.0F);
    assert(rig.controller.execute(power).ok);
    for (int index = 0; index < 8; ++index) {
        rig.antRadio.injectEventTx();
        rig.core.loop();
    }
    bool sawTrainerPage = false;
    for (const auto& page : rig.antRadio.pages()) sawTrainerPage = sawTrainerPage || page[0] == 25;
    assert(sawTrainerPage);

    // A display's target power page is applied through the core gate.
    rig.antRadio.injectPage(smartfitness::ant::Page{49, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x03});  // 200 W
    rig.core.loop();
    assert(rig.core.controlTarget().control == Control::Power && rig.core.controlTarget().value == 200.0F);
    assert(rig.controller.statusView().antOutput.control);

    output.text = "ble-ftms";
    assert(rig.controller.execute(output).ok);
    assert(rig.ftms.enabled() && !rig.ant.enabled() && rig.antRadio.count(smartfitness::ant::msg::kCloseChannel) == 1);
    output.text = "off";
    assert(rig.controller.execute(output).ok);
    assert(!rig.ftms.enabled() && !rig.ant.enabled());
    assert(rig.controller.statusView().antOutput.link == "disabled");
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

void testOutputServicesCommands() {
    ControllerRig rig(true);
    Command services = make(CommandType::SetOutputServices);
    services.text = "ftms";
    services.serviceMask = 3;
    assert(!rig.controller.execute(services).ok);  // No source power yet.
    services.serviceMask = 2;
    assert(!rig.controller.execute(services).ok);  // FTMS is mandatory.
    services.text = "usb";
    services.serviceMask = 0;
    assert(!rig.controller.execute(services).ok);
    services.text = "ant";
    services.serviceMask = 32;
    assert(!rig.controller.execute(services).ok);
    services.serviceMask = 0;
    assert(rig.controller.execute(services).ok);  // Auto works without source data.
    assert(rig.controller.execute(make(CommandType::Simulate)).ok);
    services.text = "ftms";
    services.serviceMask = 15;
    const auto selected = rig.controller.execute(services);
    assert(selected.ok && selected.message.find("save and restart") != std::string::npos);
    assert(rig.ftms.configuredServices() == 15);
    assert(rig.controller.statusView().ftmsOutput.restartRequired);
    services.text = "ant";
    services.serviceMask = 6;  // Sensor-only ANT does not require FE-C.
    assert(rig.controller.execute(services).ok);
    assert(rig.ant.configuredServices() == 6 && rig.ant.services() == 6);
    auto config = rig.controller.configView();
    assert(config.ftmsOutputServices == 15 && config.antOutputServices == 6 && config.unsaved);
    assert(rig.backend.writes() == 0);  // Same explicit-save convention as profiles.
    const auto status = rig.controller.statusView();
    assert(status.ftmsOutput.services[1].enabled && status.ftmsOutput.services[1].available);
    assert(status.ftmsOutput.services[2].enabled && status.ftmsOutput.services[2].available);
    assert(status.ftmsOutput.services[3].enabled && status.ftmsOutput.services[3].available);
    assert(!status.ftmsOutput.services[4].available);
    assert(!status.antOutput.channels[0].enabled && status.antOutput.channels[1].enabled &&
           status.antOutput.channels[2].enabled);
    assert(rig.controller.execute(make(CommandType::Save)).ok);
    services.serviceMask = 0;
    assert(rig.controller.execute(services).ok);
    assert(rig.controller.execute(make(CommandType::Load)).ok);
    assert(rig.ant.configuredServices() == 6 && rig.ftms.configuredServices() == 15);
    assert(rig.controller.execute(make(CommandType::Disconnect)).ok);
    services.text = "ftms";
    services.serviceMask = 15;
    assert(rig.controller.execute(services).ok);  // Retaining saved services needs no live samples.
    assert(rig.controller.execute(make(CommandType::Save)).ok);
    services.serviceMask = 13;
    assert(rig.controller.execute(services).ok);  // Unavailable services can still be disabled.
    services.serviceMask = 15;
    assert(!rig.controller.execute(services).ok);  // Re-enabling power still requires data.
    assert(rig.controller.execute(make(CommandType::Simulate)).ok);
    services.text = "ftms";
    services.serviceMask = 17;
    assert(!rig.controller.execute(services).ok);  // Bike is not a running profile.
    Command profile = make(CommandType::FtmsProfile);
    profile.text = "treadmill";
    assert(rig.controller.execute(profile).ok);
    assert(!rig.controller.execute(services).ok);  // A bike override does not invent running cadence.
    assert(!rig.controller.statusView().ftmsOutput.services[4].available);
    services.text = "ant";
    assert(rig.controller.execute(services).ok);  // SDM can represent virtual treadmill speed.
    assert(rig.controller.execute(make(CommandType::FactoryReset)).ok);
    assert(rig.ftms.configuredServices() == 0 && rig.ant.configuredServices() == 0);
    assert(!rig.controller.execute(control(Control::Power, 100)).ok);  // Control gate unchanged.
    ControllerRig noAnt;
    services.serviceMask = 0;
    assert(!noAnt.controller.execute(services).ok);
}

void testServiceAvailabilityUsesCapabilitiesWithoutInventingSamples() {
    TrainerState state;
    assert(availableOutputServices(state) == 1);
    state.capabilities.setMetric(Metric::Power, true);
    state.capabilities.setMetric(Metric::HeartRate, true);
    state.capabilities.setMetric(Metric::Cadence, true);
    assert(availableOutputServices(state) == 15);
    assert(!state.sample(Metric::Power).valid);
    assert(availableOutputServices(state, MachineProfile::Treadmill) == 7);  // RSC needs speed, not cadence alone.
    state.capabilities.setMetric(Metric::Speed, true);
    assert(availableOutputServices(state, MachineProfile::Treadmill) == 7);
    state.profile = MachineProfile::Treadmill;
    assert(availableOutputServices(state, MachineProfile::Treadmill) == 23);
    assert(availableOutputServices(state, MachineProfile::IndoorBike) == 7);
    assert(availableOutputServices(state, MachineProfile::Rower) == 7);
    assert(outputServicesAvailable(0, TrainerState{}, MachineProfile::IndoorBike));
    assert(!outputServicesAvailable(2, TrainerState{}, MachineProfile::IndoorBike));
    TrainerState sampled;
    sampled.setSample(Metric::Speed, 0.0F);
    assert(availableOutputServices(sampled) == 9);  // Real zero is still valid.
    assert(availableOutputServices(sampled, MachineProfile::Treadmill) == 1);  // BLE RSC cadence is mandatory.
    assert(availableOutputServices(sampled, MachineProfile::Treadmill, true) == 17);  // SDM supports missing cadence.
    sampled.setSample(Metric::Cadence, 0.0F);
    sampled.profile = MachineProfile::Treadmill;
    assert(availableOutputServices(sampled, MachineProfile::Treadmill) == 17);
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
    testFtmsClientView();
    testSimulationTelemetryAndTargets();
    testScanAndPairViews();
    testScanSortingAndRelevance();
    testPairedServiceAndSubscriptionViews();
    testConfigViewTracksCommands();
    testOutputProtocolSelection();
    testOutputServicesCommands();
    testServiceAvailabilityUsesCapabilitiesWithoutInventingSamples();
    testAdapterViews();
    testWithoutBleInput();
}
