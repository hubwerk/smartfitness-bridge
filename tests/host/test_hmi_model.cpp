// HMI variant view logic (pure C++): metric cycling, target editing with the
// never-auto-start rule, status texts, settings persistence, command building
// and queueing, confirmation sequences. The LVGL screens are exercised by the
// host simulator (sim/).
#include <cassert>
#include <cmath>
#include <string>

#include "ble_input_adapter.h"
#include "bridge_controller.h"
#include "ant_output.h"
#include "config_store.h"
#include "fake_central.hpp"
#include "ftms_output.h"
#include "hmi_model.h"
#include "hmi_strings.h"
#include "scroll_arc.h"

using namespace smartfitness;
using namespace hmi;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-4F; }

void testScrollArc() {
    assert(!scrollArc(0, 100, 0).visible);
    assert(!scrollArc(100, 100, 0).visible);
    assert(!scrollArc(200, 0, 0).visible);
    assert(!scrollArc(-1, 100, 0).visible);
    const auto top = scrollArc(400, 100, 0);
    const auto middle = scrollArc(400, 100, 150);
    const auto bottom = scrollArc(400, 100, 300);
    assert(top.visible && top.start == 193 && top.end == 205);
    assert(middle.start == 174 && middle.end == 186);
    assert(bottom.start == 155 && bottom.end == 167);
    assert(scrollArc(400, 100, -100).start == top.start);
    assert(scrollArc(400, 100, 900).end == bottom.end);
    const auto longList = scrollArc(INT32_MAX, 100, INT32_MAX);
    assert(longList.visible && longList.start == 155 && longList.end == 163);
    const auto almostFits = scrollArc(101, 100, 1);
    assert(almostFits.end - almostFits.start == 49);
}

void testStrings() {
    for (std::uint16_t index = 0; index < static_cast<std::uint16_t>(Str::Count); ++index) {
        const char* text = tr(static_cast<Str>(index));
        assert(text && *text && std::string(text) != "?");
    }
    assert(std::string(tr(Str::Dashboard)) == "Dashboard");
    assert(std::string(tr(Str::OutputServices)) == "Output services");
    setLanguage(Language::German);  // untranslated entries still fall back to English
    assert(std::string(tr(Str::OutputServices)) == "Ausgangsdienste");
    assert(std::string(tr(Str::ServiceNoData)) == "keine Daten");
    assert(std::string(tr(Str::Dashboard)) == "Dashboard");
    setLanguage(Language::English);
    assert(language() == Language::English);
    assert(std::string(tr(Str::Count)) == "?");
}

void testMetrics() {
    assert(dashboardMetrics(MachineProfile::IndoorBike).front() == Metric::Power);
    assert(dashboardMetrics(MachineProfile::Treadmill).front() == Metric::Speed);
    assert(formatValue(8.0F, 1) == "8.0" && formatValue(180.4F, 0) == "180" && formatValue(-0.01F, 1) == "0.0");
    assert(formatValue(NAN, 1) == "--");
    MetricView sample;
    assert(formatMetric(sample, Metric::Power) == "--");
    sample = {true, 181.6F};
    assert(formatMetric(sample, Metric::Power) == "182");
    assert(std::string(metricFormat(Metric::Speed).unit) == "km/h");

    MetricCycler cycler;
    assert(cycler.current(MachineProfile::IndoorBike) == Metric::Power);
    cycler.step(MachineProfile::IndoorBike, 1);
    assert(cycler.current(MachineProfile::IndoorBike) == Metric::Cadence);
    cycler.step(MachineProfile::IndoorBike, -2);
    assert(cycler.current(MachineProfile::IndoorBike) == Metric::Incline);  // wraps
    // A profile change keeps the selected metric when it exists there.
    assert(cycler.current(MachineProfile::Treadmill) == Metric::Incline);
}

void testTargetEditor() {
    StatusView status;
    TargetEditor editor;
    editor.begin(Control::Power, status);
    assert(editor.value() == 100.0F && editor.valueText() == "100" && !editor.requiresConfirmation());
    editor.step(3);
    assert(editor.value() == 115.0F);
    editor.step(-1000);
    assert(editor.value() == 0.0F);
    editor.set(5000.0F);
    assert(editor.value() == controlSpec(Control::Power).maximum);
    editor.set(NAN);
    assert(editor.value() == 0.0F);

    // Existing target of the same control is the starting point.
    status.hasTarget = true;
    status.target = {Control::Incline, 4.5F};
    editor.begin(Control::Incline, status);
    assert(editor.value() == 4.5F);
    editor.step(1);
    assert(near(editor.value(), 5.0F) && editor.valueText() == "5.0");
    editor.set(-30.0F);
    assert(editor.value() == controlSpec(Control::Incline).minimum);

    // Never auto-start: speed starts from the measured belt speed, not from a
    // remembered target, and always requires an explicit confirmation.
    status.target = {Control::Speed, 9.0F};
    editor.begin(Control::Speed, status);
    assert(editor.value() == 0.0F && editor.requiresConfirmation());
    status.telemetry[static_cast<std::size_t>(Metric::Speed)] = {true, 6.04F};
    editor.begin(Control::Speed, status);
    assert(near(editor.value(), 6.0F));
    for (int i = 0; i < 21; ++i) editor.step(1);
    assert(near(editor.value(), 8.1F) && editor.valueText() == "8.1");
    const Command command = editor.command();
    assert(command.type == CommandType::Control && command.control == Control::Speed && near(command.value, 8.1F));
    editor.set(99.0F);
    assert(editor.value() == controlSpec(Control::Speed).maximum);

    for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(Control::Count); ++index) {
        const auto& spec = controlSpec(static_cast<Control>(index));
        assert(spec.control == static_cast<Control>(index) && spec.step > 0.0F && spec.minimum < spec.maximum);
    }
    assert(controlOrder(MachineProfile::Treadmill).front() == Control::Speed);
    assert(controlOrder(MachineProfile::IndoorBike).front() == Control::Power);
    assert(controlOrder(MachineProfile::Rower).size() == static_cast<std::size_t>(Control::Count));
}

void testStatusTexts() {
    StatusView status;
    assert(inputLabel(status) == "No device");
    status.state = RuntimeState::ControlReady;
    assert(inputLabel(status) == "Simulation");
    status.input.bleSelected = true;
    status.input.paired = {true, "aa:00:00:00:00:21", false, "KICKR", "ftms", {}, {}};
    status.input.link = "ready";
    assert(inputLabel(status) == "KICKR (ftms) ready");
    assert(std::string(tr(stateLabel(RuntimeState::ControlReady))) == "Control ready");
    assert(std::string(profileLabel(MachineProfile::Treadmill)) == "Treadmill");
    assert(std::string(ftmsProfileLabel(FtmsOutputProfile::Auto)) == "Auto");
    assert(std::string(ftmsProfileLabel(FtmsOutputProfile::Rower)) == "Rower");

    FtmsOutputView ftms;
    assert(ftmsLabel(ftms) == "FTMS off");
    ftms.enabled = true;
    assert(ftmsLabel(ftms) == "FTMS idle");
    ftms.advertising = true;
    assert(ftmsLabel(ftms) == "Advertising");
    ftms.client = true;
    assert(ftmsLabel(ftms) == "App connected");

    StatusView output;
    assert(outputLabel(output) == "FTMS off");  // default BLE FTMS, output not yet enabled
    output.ftmsOutput.enabled = true;
    output.ftmsOutput.advertising = true;
    assert(outputLabel(output) == "Advertising");
    output.ftmsOutput.client = true;
    assert(outputLabel(output) == "APP LINK");
    output.outputProtocol = OutputProtocol::Off;
    assert(outputLabel(output) == "Output off");
    output.outputProtocol = OutputProtocol::AntFec;
    assert(outputLabel(output) == "No ANT+ radio");
    output.antOutput.available = true;
    assert(outputLabel(output) == "No ANT+ stick");
    output.antOutput.stick = true;
    assert(outputLabel(output) == "ANT+ on");
    output.antOutput.display = true;
    assert(outputLabel(output) == "ANT+ link");

    assert(nextOutputProtocol(OutputProtocol::Off, false) == OutputProtocol::BleFtms);
    assert(nextOutputProtocol(OutputProtocol::BleFtms, false) == OutputProtocol::Off);
    assert(nextOutputProtocol(OutputProtocol::BleFtms, true) == OutputProtocol::AntFec);
    assert(nextOutputProtocol(OutputProtocol::AntFec, true) == OutputProtocol::Off);
    assert(nextOutputProtocol(OutputProtocol::AntFec, false) == OutputProtocol::Off);

    assert(targetLabel(status) == "No target");
    status.hasTarget = true;
    status.target = {Control::Speed, 8.0F};
    assert(targetLabel(status) == "Speed 8.0 km/h");

    ScanDeviceView device;
    device.address = "aa:00:00:00:00:22";
    device.rssi = -61;
    assert(scanEntryLabel(device) == "aa:00:00:00:00:22\n?");
    assert(signalLevel(device.rssi) == 3);
    assert(signalLevel(-40) == 4 && signalLevel(-60) == 4 && signalLevel(-70) == 3 && signalLevel(-71) == 2 &&
           signalLevel(-80) == 2 && signalLevel(-81) == 1 && signalLevel(-110) == 1);
    device.label = "Apple";
    device.relevant = false;
    assert(scanEntryLabel(device) == "Apple\n?");
    assert(scanEntryMuted(device));
    device.label = "FTMS";
    device.relevant = true;  // known service, but no advertised name
    assert(scanEntryMuted(device));
    device.name = "WalkingPad";
    device.label = "WalkingPad";
    device.relevant = true;
    device.adapter = "walkingpad-r1";
    assert(scanEntryLabel(device) == "WalkingPad\nwalkingpad-r1");
    assert(!scanEntryMuted(device));
    assert(onOff(true) == "on" && onOff(false) == "off");
}

void testServiceDisplays() {
    assert(serviceUuidLabel("1826") == "FTMS");
    assert(serviceUuidLabel("2ad2") == "2ad2");
    assert(serviceUuidLabel("12345678-1234-5678-1234-123456789abc") == "12345678...");
    assert(advertisedServicesLabel({}) == "ADV -");
    assert(advertisedServicesLabel({"1826", "1818", "180d"}) == "ADV FTMS CPS HR");
    assert(subscriptionStateLabel("ok") == Str::ServiceOk);
    assert(subscriptionStateLabel("fail") == Str::ServiceFail);
    assert(subscriptionStateLabel("pending") == Str::ServicePending);
    assert(subscriptionStateLabel("unknown") == Str::ServicePending);
    StatusView status;
    assert(outputServicesLabel(status) == "OUT -");
    status.ftmsOutput.services = {{"ftms", "FTMS", true, true, true},
                                   {"cps", "CPS", true, true, false},
                                   {"hr", "HR", false, true, false}};
    assert(outputServicesLabel(status) == "OUT FTMS CPS");
    status.outputProtocol = OutputProtocol::Off;
    assert(outputServicesLabel(status) == "OUT -");
    status.outputProtocol = OutputProtocol::AntFec;
    assert(outputServicesLabel(status) == "OUT ANT+ -");
    status.antOutput.available = true;
    status.antOutput.channels = {{"fec", "FE-C", 17, true, true}, {"hr", "HR", 120, false, false}};
    assert(outputServicesLabel(status) == "OUT FE-C");
    auto command = toggleOutputService("ftms", 0, 2);
    assert(command.type == CommandType::SetOutputServices && command.text == "ftms" && command.serviceMask == 3);
    assert(toggleOutputService("ftms", 7, 2).serviceMask == 5);
    assert(toggleOutputService("ftms", 1, 1).serviceMask == 1);
    assert(toggleOutputService("ftms", 0, 16).serviceMask == 17);
    assert(toggleOutputService("ant", 1, 4).serviceMask == 5);
}

void testSettings() {
    HmiSettings settings;
    assert(isValid(settings));
    assert(nextBrightness(80) == 100 && nextBrightness(100) == 10 && nextBrightness(42) == 50);
    assert(nextScreenTimeout(0) == 30 && nextScreenTimeout(600) == 0 && nextScreenTimeout(120) == 300);
    assert(screenTimeoutLabel(0) == "never" && screenTimeoutLabel(30) == "30 s" && screenTimeoutLabel(120) == "2 min");

    settings.brightness = 50;
    settings.screenTimeoutS = 300;
    settings.ledRing = false;
    auto data = serialize(settings);
    HmiSettings loaded;
    assert(deserialize(data, loaded) && loaded == settings);
    data[1] ^= 0x01;  // checksum mismatch
    HmiSettings untouched;
    assert(!deserialize(data, untouched) && untouched == HmiSettings{});
    assert(!deserialize({}, untouched));
    settings.brightness = 5;  // invalid record is refused
    assert(!deserialize(serialize(settings), untouched) && untouched == HmiSettings{});

    assert(nextFtmsProfile(FtmsOutputProfile::Auto) == FtmsOutputProfile::IndoorBike);
    assert(nextFtmsProfile(FtmsOutputProfile::Rower) == FtmsOutputProfile::StepClimber);
    assert(nextFtmsProfile(FtmsOutputProfile::StepClimber) == FtmsOutputProfile::StairClimber);
    assert(nextFtmsProfile(FtmsOutputProfile::StairClimber) == FtmsOutputProfile::Auto);
    assert(ftmsNamePresets().front() == FtmsOutput::kDefaultName);
    for (const auto& name : ftmsNamePresets())
        assert(!name.empty() && name.size() <= FtmsOutput::kMaxNameLength && ConfigStore::isValidFtmsName(name));
}

void testCommandsQueueAndConfirm() {
    CommandQueue queue;
    for (std::size_t i = 0; i < CommandQueue::kCapacity; ++i) assert(queue.push(makeCommand(CommandType::Status)));
    assert(!queue.push(makeCommand(CommandType::Save)) && queue.dropped() == 1);
    Command command;
    assert(queue.pop(command) && command.type == CommandType::Status && queue.size() == CommandQueue::kCapacity - 1);
    while (queue.pop(command)) {
    }
    assert(queue.size() == 0 && !queue.pop(command));

    assert(makeFlag(CommandType::FtmsInput, true).flag);
    assert(makeText(CommandType::OutputProtocol, "ant-fec").text == "ant-fec");
    assert(makeText(CommandType::FtmsName, "SF Bike").text == "SF Bike");

    ScanDeviceView device;
    device.address = "aa:00:00:00:00:22";
    device.randomAddress = true;
    assert(makePair(device, "ifit-model-24", command));
    assert(command.type == CommandType::Pair && command.adapterId == "ifit-model-24");
    assert(command.address.type == 1 && command.addressTypeGiven);
    device.address = "not-an-address";
    assert(!makePair(device, "", command));

    ConfirmSequence reset(2);
    assert(!reset.active() && !reset.confirm());
    reset.start();
    assert(reset.active() && reset.step() == 1 && !reset.confirm());
    assert(reset.step() == 2 && reset.confirm() && !reset.active());
    reset.start();
    reset.cancel();
    assert(!reset.active() && !reset.confirm());
}

void testLedAndRefresh() {
    StatusView status;
    assert((statusColor(status) == Rgb{120, 0, 0}));
    status.state = RuntimeState::ControlReady;
    assert((statusColor(status) == Rgb{0, 160, 40}));
    status.input.scanning = true;
    assert((statusColor(status) == Rgb{0, 40, 160}));

    RefreshTimer timer(kStatusRefreshMs);
    assert(timer.due(1000) && !timer.due(1100) && timer.due(1200));
    timer.force();
    assert(timer.due(1201) && !timer.due(1300));
    assert(kStatusRefreshMs >= 200);  // <= 5 Hz
}

// The HMI drives the same BridgeController with the same Commands as the
// console and the web API.
void testHmiCommandsOnController() {
    using smartfitness::testing::FakeCentral;
    std::uint32_t now = 1000;
    FakeCentral central;
    MemoryConfigBackend backend;
    BleInputAdapter ble{central, [&now] { return now; }};
    SimulatedDeviceAdapter simulation;
    InputSelector selector{simulation, &ble};
    AntOutput ant;
    FtmsOutput ftms;
    ConfigStore config{backend};
    BridgeCore core;
    BridgeController controller{core, config, simulation, ant, ftms};
    config.begin();
    core.attach(selector, ant, ftms);
    core.begin();
    controller.attachBleInput(selector, ble);
    controller.begin();

    CommandQueue queue;
    queue.push(makeCommand(CommandType::Simulate));
    TargetEditor editor;
    editor.begin(Control::Power, controller.statusView());
    editor.step(10);
    queue.push(editor.command());
    queue.push(makeText(CommandType::FtmsName, ftmsNamePresets()[1]));
    queue.push(makeText(CommandType::FtmsProfile,
                        ftmsOutputProfileName(nextFtmsProfile(controller.configView().ftmsOutputProfile))));
    queue.push(toggleOutputService("ftms", 0, 2));
    Command command;
    std::vector<CommandResult> results;
    while (queue.pop(command)) results.push_back(controller.execute(command));
    assert(results.size() == 5);
    for (const auto& result : results) assert(result.ok);
    const StatusView status = controller.statusView();
    assert(status.hasTarget && status.target.control == Control::Power && status.target.value == 150.0F);
    assert(status.ftmsOutput.name == "SF Bike" && status.ftmsOutput.override == FtmsOutputProfile::IndoorBike);
    assert(controller.configView().ftmsOutputServices == 3);
    assert(status.ftmsOutput.services.size() == 5 && status.ftmsOutput.services[1].enabled);

    // A rejected command reports the core reason for the HMI toast.
    assert(controller.execute(makeCommand(CommandType::Disconnect)).ok);
    TargetEditor power;
    power.begin(Control::Power, controller.statusView());
    const CommandResult rejected = controller.execute(power.command());
    assert(!rejected.ok && !rejected.message.empty());
}

}  // namespace

void runHmiModelTests() {
    testScrollArc();
    testStrings();
    testMetrics();
    testTargetEditor();
    testStatusTexts();
    testServiceDisplays();
    testSettings();
    testCommandsQueueAndConfirm();
    testLedAndRefresh();
    testHmiCommandsOnController();
}
