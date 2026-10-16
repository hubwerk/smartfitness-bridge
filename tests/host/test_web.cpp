// Host tests for the shared command layer (console + web API), the API
// exchange between the web task and the main loop, JSON views and the Wi-Fi
// policy.

#include <ArduinoJson.h>

#include <cassert>
#include <array>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "ant_output.h"
#include "ble_input_adapter.h"
#include "bridge/bridge_core.hpp"
#include "bridge_commands.h"
#include "config_store.h"
#include "fake_central.hpp"
#include "ftms_output.h"
#include "web_api.h"
#include "bridge_controller.h"
#include "web_json.h"
#include "web_request.h"
#include "wifi_policy.h"

using namespace smartfitness;
using smartfitness::testing::FakeCentral;
using smartfitness::testing::address;

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

JsonDocument parse(const std::string& json) {
    JsonDocument document;
    const auto error = deserializeJson(document, json);
    assert(!error);
    return document;
}

struct Rig {
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
    BridgeController ui{core, config, simulation, ant, ftms};
    NetworkStatus network;

    Rig() {
        config.begin();
        core.attach(selector, ant, ftms);
        core.begin();
        ui.attachBleInput(selector, ble);
        ui.begin();
    }

    std::string statusJson() const { return web::statusJson(ui.statusView(), network); }
    std::string scanResultsJson() const { return web::scanResultsJson(ui.scanView()); }
    std::string configJson() const { return web::configJson(ui.configView()); }
};

void testConsoleParsing() {
    Command command;
    std::string error;
    assert(parseConsoleCommand("scan", command, error) && command.type == CommandType::Scan && command.durationMs == 0);
    assert(parseConsoleCommand("scan 5", command, error) && command.durationMs == 5000);
    assert(!parseConsoleCommand("scan 0", command, error) && contains(error, "1-60"));
    assert(parseConsoleCommand("sim", command, error) && command.type == CommandType::Simulate);
    assert(parseConsoleCommand("pair c0:11:22:33:44:55", command, error) && command.type == CommandType::Pair);
    assert(command.adapterId.empty() && !command.addressTypeGiven);
    assert(command.address == address("c0:11:22:33:44:55"));
    assert(parseConsoleCommand("pair C0:11:22:33:44:55 echelon random", command, error));
    assert(command.adapterId == "echelon" && command.addressTypeGiven && command.address.type == 1);
    assert(parseConsoleCommand("pair c0:11:22:33:44:55 auto", command, error) && command.adapterId.empty());
    assert(!parseConsoleCommand("pair nope", command, error) && contains(error, "usage"));
    assert(!parseConsoleCommand("pair c0:11:22:33:44:55 ftms weird", command, error));
    assert(parseConsoleCommand("wifi Home secret-pass", command, error));
    assert(command.type == CommandType::Wifi && command.text == "Home" && command.text2 == "secret-pass");
    assert(parseConsoleCommand("resistance 40", command, error) && command.control == Control::Resistance);
    assert(command.value == 40.0F);
    assert(parseConsoleCommand("restart", command, error) && command.type == CommandType::Restart);
    assert(!parseConsoleCommand("bogus", command, error) && error == "unknown command; type help");
    assert(!parseConsoleCommand("bogus 1", command, error) && error == "unknown command; type help");
}

void testConsoleBlePairing() {
    Rig rig;
    assert(contains(rig.ui.status(), "input=sim link=idle"));
    assert(rig.ui.handleCommand("connect") == "no paired device; scan and pair first");
    assert(rig.ui.handleCommand("scan 5") == "scanning for 5 s");
    assert(rig.central.lastScanDuration == 5000);
    rig.central.advertise(address("aa:00:00:00:00:21"), "KICKR CORE 1234", {0x1826, 0x1818});
    rig.central.advertise(address("aa:00:00:00:00:22"), "Mystery", {});
    rig.core.loop();
    const std::string list = rig.ui.handleCommand("scan-results");
    assert(contains(list, "2 device(s)") && contains(list, "type=ftms") && contains(list, "type=?"));

    // Unknown device type needs an explicit adapter; unknown adapters are refused.
    assert(rig.ui.handleCommand("pair aa:00:00:00:00:22") == "device type not detected; choose an adapter");
    assert(rig.ui.handleCommand("pair aa:00:00:00:00:22 nosuch") == "unknown adapter: nosuch");
    assert(rig.backend.writes() == 0);

    const std::string paired = rig.ui.handleCommand("pair aa:00:00:00:00:21");
    assert(paired == "paired KICKR CORE 1234 (aa:00:00:00:00:21) as ftms; connecting");
    // Pairing persists immediately, enables the BLE input and connects.
    assert(rig.backend.writes() == 1);
    assert(rig.config.config().ftmsInput && rig.config.config().pairedAdapterId == "ftms");
    assert(rig.config.config().pairedDeviceName == "KICKR CORE 1234");
    assert(rig.selector.source() == InputSelector::Source::Ble);
    assert(rig.central.connects.size() == 1 && rig.central.connects.back().address == address("aa:00:00:00:00:21"));
    assert(contains(rig.ui.status(), "input=ble link=connecting paired=aa:00:00:00:00:21/ftms"));

    // Control is gated by BridgeCore until the trainer grants control.
    assert(rig.ui.handleCommand("power 200") == "control: disconnected");
    rig.central.pushEvent(ble::CentralEventType::ReadResult, 0, 0, {0x02, 0x44, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00});
    rig.central.pushEvent(ble::CentralEventType::Connected, 0x11);
    rig.central.pushEvent(ble::CentralEventType::Notification, 0, 4, {0x80, 0x00, 0x01});
    rig.central.pushEvent(ble::CentralEventType::Notification, 0, 0,
                          {0x44, 0x02, 0xC4, 0x09, 0xA0, 0x00, 0x96, 0x00, 0x78});
    rig.core.loop();
    assert(rig.ui.handleCommand("power 200") == "control: applied");
    assert(rig.ui.handleCommand("resistance 20") == "control: unsupported control");

    // Reboot: the paired device is restored and auto-connected.
    {
        std::uint32_t now = 5000;
        FakeCentral central;
        BleInputAdapter ble(central, [&now] { return now; });
        SimulatedDeviceAdapter simulation;
        InputSelector selector(simulation, &ble);
        AntOutput ant;
        FtmsOutput ftms;
        ConfigStore config(rig.backend);
        BridgeCore core;
        BridgeController ui(core, config, simulation, ant, ftms);
        assert(config.begin() == ConfigLoadResult::Loaded);
        core.attach(selector, ant, ftms);
        core.begin();
        ui.attachBleInput(selector, ble);
        ui.begin();
        assert(selector.source() == InputSelector::Source::Ble);
        assert(central.connects.size() == 1 && central.connects.back().address == address("aa:00:00:00:00:21"));
        assert(ble.pairedDevice().adapterId == "ftms" && ble.pairedDevice().name == "KICKR CORE 1234");
    }

    // "sim" switches to the simulation without forgetting the pairing.
    assert(contains(rig.ui.handleCommand("sim"), "simulation connected"));
    assert(rig.selector.source() == InputSelector::Source::Simulation);
    assert(rig.ble.linkState() == BleInputAdapter::LinkState::Idle);
    assert(rig.config.config().pairedSource.valid && !rig.config.config().ftmsInput);
    assert(contains(rig.ui.handleCommand("connect"), "connecting"));
    assert(rig.selector.source() == InputSelector::Source::Ble);

    // Unpair clears and persists, back to the simulation source.
    rig.ui.handleCommand("unpair");
    assert(!rig.config.config().pairedSource.valid && rig.config.config().pairedAdapterId.empty());
    assert(!rig.ble.pairedDevice().valid);
    assert(rig.selector.source() == InputSelector::Source::Simulation);
    ConfigStore rebooted(rig.backend);
    rebooted.begin();
    assert(!rebooted.config().pairedSource.valid);
}

void testPairOverrideAndExclusion() {
    Rig rig;
    // Not in the scan results: works with an explicit adapter and address type.
    assert(rig.ui.handleCommand("pair aa:00:00:00:00:31") == "device not in scan results; choose an adapter");
    assert(contains(rig.ui.handleCommand("pair aa:00:00:00:00:31 hrs random"), "as hrs"));
    assert(rig.config.config().pairedSource.addressType == 1);
    // Override of a detected type.
    rig.ui.handleCommand("scan");
    rig.central.advertise(address("aa:00:00:00:00:32"), "KICKR", {0x1826});
    rig.core.loop();
    assert(contains(rig.ui.handleCommand("pair aa:00:00:00:00:32 cps"), "as cps"));
    assert(rig.ble.pairedDevice().adapterId == "cps");
    // The FTMS output's own client is never paired.
    rig.ble.setExcludedPeer(address("aa:00:00:00:00:33"), true);
    assert(contains(rig.ui.handleCommand("pair aa:00:00:00:00:33 ftms"), "refusing"));
    rig.central.own = address("aa:00:00:00:00:34");
    assert(contains(rig.ui.handleCommand("pair aa:00:00:00:00:34 ftms"), "refusing"));
    // Storage failure is reported and nothing changes.
    rig.backend.failWrites = true;
    assert(rig.ui.handleCommand("pair aa:00:00:00:00:35 hrs") == "pairing not persisted (storage write failed)");
    assert(rig.ble.pairedDevice().adapterId == "cps");
}

void testWifiCommands() {
    Rig rig;
    assert(rig.ui.handleCommand("wifi Home short") == "password must be empty or 8-63 printable characters");
    assert(rig.ui.handleCommand("wifi Home secret-pass") == "wifi: Home; save and restart to apply");
    assert(rig.config.config().wifiSsid == "Home" && rig.config.config().wifiPassword == "secret-pass");
    assert(rig.ui.handleCommand("ap-pass 1234") == "access point password must be 8-63 printable ASCII characters");
    assert(rig.ui.handleCommand("ap-pass new-ap-pass") == "access point password set; save and restart to apply");
    assert(rig.ui.handleCommand("save") == "configuration saved");
    assert(rig.ui.handleCommand("wifi-clear") == "wifi cleared (access point only); save and restart to apply");
    assert(rig.config.config().wifiSsid.empty() && rig.config.hasUnsavedChanges());
    assert(!rig.ui.restartRequested());
    assert(rig.ui.handleCommand("restart") == "restarting" && rig.ui.restartRequested());
}

void testOutputProfiles() {
    Rig rig;
    assert(parse(rig.configJson())["ftmsOutputProfile"] == "auto");
    auto status = parse(rig.statusJson());
    assert(status["input"]["profile"] == "bike");
    assert(status["ftmsOutput"]["profile"] == "bike");
    assert(status["ftmsOutput"]["override"] == "auto");
    assert(status["telemetry"]["elapsed"].isNull());
    assert(contains(rig.ui.handleCommand("help"), "ftms-profile"));
    for (const std::string value : {"auto", "bike", "treadmill", "cross-trainer", "rower"}) {
        Command command;
        std::string error;
        assert(parseConsoleCommand("ftms-profile " + value, command, error));
        assert(command.type == CommandType::FtmsProfile && command.text == value);
        assert(rig.ui.execute(command).ok);
        FtmsOutputProfile profile;
        assert(parseFtmsOutputProfile(value, profile));
        assert(rig.config.config().ftmsOutputProfile == profile);
        assert(rig.ftms.profile() == effectiveOutputProfile(profile, rig.core.trainerState().profile));
        status = parse(rig.statusJson());
        assert(status["ftmsOutput"]["override"].as<std::string>() == value);
        assert(status["ftmsOutput"]["profile"].as<std::string>() == machineProfileName(rig.ftms.profile()));
        assert(contains(rig.ui.status(), std::string("input-profile=bike output-profile=") +
                        machineProfileName(rig.ftms.profile())));
        assert(parse(rig.configJson())["ftmsOutputProfile"].as<std::string>() == value);
    }
    for (const std::string value : {"", "AUTO", "elliptical", "bike ", "treadmill extra"}) {
        Command command;
        std::string error;
        assert(!parseConsoleCommand("ftms-profile " + value, command, error));
        command.type = CommandType::FtmsProfile;
        command.text = value;
        assert(!rig.ui.execute(command).ok);
        assert(rig.config.config().ftmsOutputProfile == FtmsOutputProfile::Rower);
    }
    assert(contains(rig.ui.handleCommand("ftms-profile treadmill"), "output-profile=treadmill"));
    assert(rig.config.hasUnsavedChanges());
    assert(rig.ui.handleCommand("save") == "configuration saved");
    rig.ui.handleCommand("ftms-profile bike");
    assert(contains(rig.ui.handleCommand("load"), "output-profile=treadmill"));
    assert(rig.ftms.profile() == MachineProfile::Treadmill);
    assert(contains(rig.ui.handleCommand("factory-reset"), "ftms-profile=auto"));
    assert(rig.ftms.profile() == MachineProfile::IndoorBike);

    rig.ui.handleCommand("output off");
    ApiExchange api;
    for (const std::string value : {"auto", "bike", "treadmill", "cross-trainer", "rower"}) {
        const auto response = api.submit("config", "{\"ftmsOutputProfile\":\"" + value + "\",\"save\":true}");
        assert(response.status == 202);
        const auto id = parse(response.body)["id"].as<std::uint32_t>();
        assert(api.process(rig.ui).size() == 1);
        assert(parse(api.result(id).body)["ok"] == true);
        ConfigStore rebooted(rig.backend);
        assert(rebooted.begin() == ConfigLoadResult::Loaded);
        assert(std::string(ftmsOutputProfileName(rebooted.config().ftmsOutputProfile)) == value);
        api.publish(ApiExchange::Snapshot::Status, rig.statusJson());
        assert(parse(api.snapshot(ApiExchange::Snapshot::Status))["ftmsOutput"]["override"].as<std::string>() == value);
    }
    for (const std::string body : {R"({"ftmsOutputProfile":"invalid"})", R"({"ftmsOutputProfile":3})",
                                   R"({"ftmsOutputProfile":null})", R"({"ftmsOutputProfile":false})",
                                   R"({"ftmsOutputProfile":"bike\u0000"})", R"({"ftmsOutputProfile":""})",
                                   R"({"ftmsName":"Changed","ftmsOutputProfile":"invalid","save":true})"}) {
        const auto previous = rig.config.config();
        assert(api.submit("config", body).status == 400);
        assert(rig.config.config() == previous);
    }
    const auto omitted = api.submit("config", R"({"outputProtocol":"off"})");
    assert(omitted.status == 202);
    api.process(rig.ui);
    assert(rig.config.config().ftmsOutputProfile == FtmsOutputProfile::Rower);
    rig.ui.handleCommand("ftms-profile auto");
    for (const auto profile : {MachineProfile::Treadmill, MachineProfile::CrossTrainer, MachineProfile::Rower}) {
        TrainerState input;
        input.connected = true;
        input.profile = profile;
        input.capabilities.setMetric(Metric::ElapsedTime, true);
        input.setSample(Metric::ElapsedTime, 321, true);
        rig.core.setTrainerState(input);
        rig.ftms.publishTrainerState(input);
        status = parse(rig.statusJson());
        assert(status["input"]["profile"].as<std::string>() == machineProfileName(profile));
        assert(status["telemetry"]["elapsed"] == 321);
        assert(status["ftmsOutput"]["profile"].as<std::string>() ==
               machineProfileName(effectiveOutputProfile(FtmsOutputProfile::Auto, profile)));
        assert(contains(rig.ui.status(), std::string("input-profile=") + machineProfileName(profile)));
    }
}

void testApiParsing() {
    std::vector<Command> commands;
    std::string error;
    assert(parseApiRequest("scan", "", commands, error) && commands[0].type == CommandType::Scan);
    assert(parseApiRequest("scan", R"({"seconds":15})", commands, error) && commands[0].durationMs == 15000);
    assert(parseApiRequest("scan", R"({"action":"stop"})", commands, error) &&
           commands[0].type == CommandType::ScanStop);
    assert(!parseApiRequest("scan", R"({"seconds":"15"})", commands, error) && contains(error, "seconds"));
    assert(!parseApiRequest("scan", R"({"seconds":600})", commands, error));
    assert(!parseApiRequest("scan", R"({"action":"go"})", commands, error));

    assert(parseApiRequest("pair", R"({"address":"aa:00:00:00:00:01","adapter":"echelon","addressType":"random"})",
                           commands, error));
    assert(commands[0].type == CommandType::Pair && commands[0].adapterId == "echelon" && commands[0].address.type == 1);
    assert(parseApiRequest("pair", R"({"address":"aa:00:00:00:00:01","adapter":"auto"})", commands, error) &&
           commands[0].adapterId.empty());
    assert(!parseApiRequest("pair", R"({"adapter":"ftms"})", commands, error) && contains(error, "address"));
    assert(!parseApiRequest("pair", R"({"address":"aa:00:00:00:00:01","addressType":"x"})", commands, error));
    assert(!parseApiRequest("pair", R"({"address":42})", commands, error) && error == "address must be a string");

    assert(parseApiRequest("control", R"({"control":"power","value":250})", commands, error));
    assert(commands[0].type == CommandType::Control && commands[0].value == 250.0F);
    assert(parseApiRequest("control", R"({"control":"incline","value":-2.5})", commands, error));
    assert(commands[0].control == Control::Incline && commands[0].value == -2.5F);
    assert(!parseApiRequest("control", R"({"control":"power","value":"250"})", commands, error));
    assert(!parseApiRequest("control", R"({"control":"turbo","value":1})", commands, error));
    assert(!parseApiRequest("control", R"({"control":"power"})", commands, error));

    assert(parseApiRequest("config",
                           R"({"ftmsName":"Garage","outputProtocol":"ant-fec","ftmsInput":true,"wifiSsid":"Home",)"
                           R"("wifiPassword":"secret-pass","apPassword":"ap-password","save":true})",
                           commands, error));
    assert(commands.size() == 6);
    assert(commands[0].type == CommandType::FtmsName && commands[0].text == "Garage");
    assert(commands[1].type == CommandType::OutputProtocol && commands[1].text == "ant-fec");
    assert(commands[2].type == CommandType::FtmsInput && commands[2].flag);
    assert(commands[3].type == CommandType::Wifi && commands[3].text2 == "secret-pass" && !commands[3].flag);
    assert(commands[4].type == CommandType::ApPassword);
    assert(commands[5].type == CommandType::Save);
    assert(parseApiRequest("config", R"({"apPassword":"ap-password","https":false,"save":true})", commands, error));
    assert(commands.size() == 3 && commands[1].type == CommandType::Https && !commands[1].flag);
    assert(commands[2].type == CommandType::Save);
    assert(parseApiRequest("config", R"({"https":true})", commands, error) && commands[0].flag);
    assert(!parseApiRequest("config", R"({"https":"off"})", commands, error) && contains(error, "true or false"));
    assert(parseApiRequest("config", R"({"wifiSsid":"Home"})", commands, error) && commands[0].flag);  // keep password
    assert(parseApiRequest("config", R"({"wifiSsid":""})", commands, error) &&
           commands[0].type == CommandType::WifiClear);
    assert(!parseApiRequest("config", R"({"wifiPassword":"secret-pass"})", commands, error));
    for (const auto& password : {std::string("short"), std::string(64, 'a'), std::string("invalid\npass")}) {
        JsonDocument body;
        body["apPassword"] = password;
        std::string json;
        serializeJson(body, json);
        assert(!parseApiRequest("config", json, commands, error));
        assert(contains(error, "8-63 printable ASCII"));
    }
    assert(parseApiRequest("config", R"({"apPassword":"12345678"})", commands, error));
    assert(!parseApiRequest("config", R"({"apPassword":"12345678\u0000bad"})", commands, error));
    assert(contains(error, "NUL"));
    assert(!parseApiRequest("pair", R"({"address":"aa:00:00:00:00:01\u0000bad"})", commands, error));
    assert(!parseApiRequest("config", R"({"ftmsInput":"yes"})", commands, error) && contains(error, "true or false"));
    assert(!parseApiRequest("config", R"({"outputProtocol":"usb"})", commands, error));
    assert(error == "outputProtocol must be off, ble-ftms or ant-fec");
    assert(!parseApiRequest("config", R"({"outputProtocol":true})", commands, error));
    assert(!parseApiRequest("config", R"({"ant":true})", commands, error) && error == "unknown field: ant");
    assert(!parseApiRequest("config", R"({"ftmsOutput":true})", commands, error));
    assert(!parseApiRequest("config", R"({"unknown":1})", commands, error) && error == "unknown field: unknown");
    assert(!parseApiRequest("config", "{}", commands, error) && error == "no settings given");
    assert(!parseApiRequest("config", R"({"ftmsName":")" + std::string(80, 'x') + "\"}", commands, error));

    assert(parseApiRequest("unpair", "{}", commands, error) && commands[0].type == CommandType::Unpair);
    assert(parseApiRequest("factory-reset", "", commands, error) && commands[0].type == CommandType::FactoryReset);
    assert(!parseApiRequest("unpair", R"({"x":1})", commands, error));
    assert(!parseApiRequest("control", "[1,2]", commands, error) && error == "expected a JSON object");
    assert(!parseApiRequest("control", "{bad json", commands, error) && contains(error, "invalid JSON"));
    assert(!parseApiRequest("config", R"({"ftmsName":{"a":{"b":1}}})", commands, error));
    assert(!parseApiRequest("nope", "{}", commands, error) && error == "unknown endpoint");
}

void testApiExchange() {
    Rig rig;
    ApiExchange api;
    assert(api.submit("control", "{bad").status == 400);
    assert(api.submit("control", std::string(ApiExchange::kMaxBodySize + 1, ' ')).status == 413);

    const auto accepted = api.submit("control", R"({"control":"power","value":150})");
    assert(accepted.status == 202);
    const auto id = parse(accepted.body)["id"].as<std::uint32_t>();
    assert(api.result(id).status == 202);  // pending until the loop runs
    assert(api.result(9999).status == 404);

    // Nothing touched the core yet; the loop executes the job.
    const auto messages = api.process(rig.ui);
    assert(messages.size() == 1);
    const auto result = api.result(id);
    assert(result.status == 200 && result.body == messages[0]);
    auto document = parse(result.body);
    assert(document["type"] == "result" && document["ok"] == false);
    assert(document["message"] == "control: disconnected");  // rejection reason from BridgeCore

    rig.ui.handleCommand("sim");
    const auto applied = parse(api.submit("control", R"({"control":"power","value":150})").body)["id"].as<std::uint32_t>();
    api.process(rig.ui);
    document = parse(api.result(applied).body);
    assert(document["ok"] == true && document["message"] == "control: applied");

    // A rejected command stops the job (save is not executed).
    const auto writes = rig.backend.writes();
    const auto rejected = parse(api.submit("config", R"({"ftmsName":"bad\u0001","save":true})").body)["id"].as<std::uint32_t>();
    api.process(rig.ui);
    document = parse(api.result(rejected).body);
    assert(document["ok"] == false && document["message"] == "expected printable ASCII");
    assert(rig.backend.writes() == writes);

    const auto saved = parse(api.submit("config", R"({"ftmsName":"Garage","save":true})").body)["id"].as<std::uint32_t>();
    api.process(rig.ui);
    document = parse(api.result(saved).body);
    assert(document["ok"] == true && document["message"] == "ftms name: Garage; configuration saved");

    // Bounded queue: overflow returns 503 and is counted; the loop drains at most maxJobs per call.
    for (std::size_t index = 0; index < ApiExchange::kQueueCapacity; ++index)
        assert(api.submit("save", "").status == 202);
    const auto busy = api.submit("save", "");
    assert(busy.status == 503 && api.droppedRequests() == 1);
    assert(api.process(rig.ui, 2).size() == 2);
    assert(api.process(rig.ui, 100).size() == ApiExchange::kQueueCapacity - 2);

    // Result ring keeps the latest kResultCapacity entries.
    assert(api.result(id).status == 200);
    for (std::size_t index = 0; index < ApiExchange::kResultCapacity; ++index) {
        assert(api.submit("save", "").status == 202);
        api.process(rig.ui);
    }
    assert(api.result(id).status == 404);

    // Snapshots.
    assert(api.snapshot(ApiExchange::Snapshot::Status) == "{}");
    api.publish(ApiExchange::Snapshot::Status, rig.statusJson());
    assert(parse(api.snapshot(ApiExchange::Snapshot::Status))["type"] == "status");
}

void testApiExchangeThreads() {
    Rig rig;
    ApiExchange api;
    std::vector<std::uint32_t> ids;
    std::thread web([&] {
        for (int index = 0; index < 200; ++index) {
            const auto response = api.submit("control", R"({"control":"power","value":100})");
            if (response.status == 202) ids.push_back(parse(response.body)["id"].as<std::uint32_t>());
            (void)api.snapshot(ApiExchange::Snapshot::Status);
        }
    });
    std::size_t processed = 0;
    for (int index = 0; index < 2000; ++index) {
        processed += api.process(rig.ui).size();
        api.publish(ApiExchange::Snapshot::Status, rig.statusJson());
    }
    web.join();
    processed += api.process(rig.ui, 100).size();
    assert(processed == ids.size());
    assert(ids.size() + api.droppedRequests() == 200);
}

void testApiBleQueueRejections() {
    Rig rig;
    ApiExchange api;
    const auto execute = [&](const std::string& endpoint, const std::string& body) {
        const auto response = api.submit(endpoint, body);
        assert(response.status == 202);
        const auto id = parse(response.body)["id"].as<std::uint32_t>();
        api.process(rig.ui);
        return parse(api.result(id).body);
    };
    rig.ui.handleCommand("scan");
    rig.central.acceptStop = false;
    auto result = execute("scan", R"({"action":"stop"})");
    assert(result["ok"] == false);
    assert(contains(result["message"].as<std::string>(), "rejected"));

    rig.central.acceptStop = true;
    rig.central.acceptConnect = false;
    result = execute("pair", R"({"address":"aa:00:00:00:00:21","adapter":"ftms"})");
    assert(result["ok"] == false);
    assert(contains(result["message"].as<std::string>(), "connection request rejected"));
    assert(rig.config.config().pairedSource.valid);  // Pairing remains persisted for a later retry.

    result = execute("connect", "{}");
    assert(result["ok"] == false);
    rig.central.acceptConnect = true;
    result = execute("connect", "{}");
    assert(result["ok"] == true);
    rig.central.acceptDisconnect = false;
    result = execute("disconnect", "{}");
    assert(result["ok"] == false);
    assert(contains(result["message"].as<std::string>(), "disconnect request rejected"));
    assert(parse(rig.statusJson())["input"]["dropped"]["commands"].as<unsigned>() >= 4);
}

void testJsonViews() {
    Rig rig;
    rig.ui.handleCommand("wifi Home secret-pass");
    rig.ui.handleCommand("ap-pass super-secret-ap");
    const std::string config = rig.configJson();
    assert(!contains(config, "secret-pass") && !contains(config, "super-secret-ap"));
    auto document = parse(config);
    assert(document["wifiSsid"] == "Home" && document["wifiPasswordSet"] == true);
    assert(document["apPasswordDefault"] == false && document["ftmsName"] == FtmsOutput::kDefaultName);
    assert(document["outputProtocol"] == "ble-ftms" && document["antAvailable"] == false);
    assert(document["unsaved"] == true);
    assert(document["https"] == true);
    rig.ui.handleCommand("https off");
    assert(parse(rig.configJson())["https"] == false && !rig.config.config().httpsEnabled);
    rig.ui.handleCommand("https on");
    assert(parse(rig.configJson())["https"] == true);

    rig.ui.handleCommand("scan");
    rig.central.advertise(address("aa:00:00:00:00:41"), "ECH-SPORT-1", {});
    rig.core.loop();
    document = parse(rig.scanResultsJson());
    assert(document["scanning"] == true && document["devices"].size() == 1);
    assert(document["devices"][0]["address"] == "aa:00:00:00:00:41");
    assert(document["devices"][0]["adapter"] == "echelon");
    assert(document["devices"][0]["label"] == "ECH-SPORT-1");
    assert(document["devices"][0]["relevant"] == true);
    assert(document["adapters"].size() > 5);

    rig.ui.handleCommand("sim");
    rig.ui.handleCommand("power 180");
    rig.network = {"ap", "SmartFitness-ABCD", "192.168.4.1", "smartfitness", 1};
    document = parse(rig.statusJson());
    assert(document["state"] == "control-ready" && document["input"]["source"] == "simulation");
    assert(document["telemetry"]["power"].is<double>() && document["telemetry"]["heartRate"].is<double>());
    assert(document["target"]["control"] == "power" && document["target"]["value"] == 180);
    assert(document["controls"].size() == 4 && document["canControl"] == true);
    assert(document["network"]["ip"] == "192.168.4.1" && document["input"]["paired"].isNull());
    assert(document["ftmsOutput"]["advertising"] == true && document["outputProtocol"] == "ble-ftms");
    assert(document["antOutput"]["enabled"] == false && document["antOutput"]["available"] == false);
    assert(document["antOutput"]["link"] == "disabled");
    assert(document["input"]["dropped"]["scans"] == 0);
    assert(document["input"]["dropped"]["events"] == 0);
    assert(document["input"]["dropped"]["notifications"] == 0);
    assert(document["input"]["dropped"]["commands"] == 0);
    assert(contains(rig.ui.status(), "dropped(scans=0,events=0,notifications=0,commands=0)"));
}

void testWifiPolicy() {
    assert(softApSsid({0x24, 0x6F, 0x28, 0x01, 0xAB, 0x0C}) == "SmartFitness-AB0C");
    WifiPolicy policy;
    assert(policy.begin(false, 0) == WifiMode::Ap);
    assert(policy.update(false, 0, 100000) == WifiMode::Ap);

    assert(policy.begin(true, 0) == WifiMode::Sta);
    assert(policy.update(false, 0, 19999) == WifiMode::Sta);
    assert(policy.update(false, 0, 20000) == WifiMode::ApSta);  // fallback AP
    assert(policy.update(true, 1, 30000) == WifiMode::ApSta);
    assert(policy.update(true, 1, 200000) == WifiMode::ApSta);  // AP still in use
    assert(policy.update(true, 0, 200000) == WifiMode::Sta);    // held long enough, unused
    // Link loss: reconnect window before falling back again.
    assert(policy.update(false, 0, 210000) == WifiMode::Sta);
    assert(policy.update(false, 0, 229999) == WifiMode::Sta);
    assert(policy.update(false, 0, 230000) == WifiMode::ApSta);
    assert(std::string(wifiModeName(WifiMode::ApSta)) == "ap+sta");
}

void testWifiScan() {
    WifiScan scan;
    auto document = parse(web::wifiScanJson(scan));
    assert(document["type"] == "wifi-scan");
    assert(document["scanning"] == false);
    assert(document["networks"].as<JsonArrayConst>().size() == 0);

    assert(scan.start(1000));
    assert(!scan.start(2000));  // already running
    assert(scan.windowOpen(1000 + WifiScan::kDurationMs - 1));
    assert(!scan.windowOpen(1000 + WifiScan::kDurationMs));
    scan.add({"Home", -70, true});
    scan.add({"Cafe", -50, false});
    scan.add({"", -30, false});                     // hidden network
    scan.add({std::string(33, 'x'), -30, false});   // invalid SSID length
    scan.add({"Home", -80, false});                 // weaker duplicate ignored
    scan.add({"Home", -40, true});                  // stronger duplicate replaces
    document = parse(web::wifiScanJson(scan));
    assert(document["scanning"] == true);
    JsonArrayConst networks = document["networks"].as<JsonArrayConst>();
    assert(networks.size() == 2);
    assert(networks[0]["ssid"] == "Home" && networks[0]["rssi"] == -40 && networks[0]["secure"] == true);
    assert(networks[1]["ssid"] == "Cafe" && networks[1]["secure"] == false);

    for (int index = 0; index < 40; ++index) scan.add({"net" + std::to_string(index), -90 + index, true});
    assert(scan.networks().size() == WifiScan::kMaxNetworks);
    assert(scan.networks().front().ssid == "Home");
    assert(scan.networks().back().rssi >= -90 + 40 - static_cast<int>(WifiScan::kMaxNetworks));
    scan.finish();
    assert(!scan.scanning() && !scan.windowOpen(1001));
    assert(scan.start(50000) && scan.networks().empty());  // a new window clears old results

    ApiExchange api;
    assert(api.snapshot(ApiExchange::Snapshot::WifiScan) == "{}");
    api.publish(ApiExchange::Snapshot::WifiScan, web::wifiScanJson(scan));
    assert(parse(api.snapshot(ApiExchange::Snapshot::WifiScan))["type"] == "wifi-scan");
}

void testRequestProtection() {
    const auto policy = [](const std::string& type, const std::string& origin, bool present = true) {
        return web::postRequestStatus(type, present, origin, "192.168.1.12", "192.168.4.1");
    };
    assert(policy("application/json", "", false) == 200);
    for (const auto* origin : {"https://smartfitness.local", "https://smartfitness.local:443",
                              "https://192.168.1.12", "https://192.168.4.1"})
        assert(policy(" Application/JSON; charset=utf-8", origin) == 200);
    for (const auto* origin : {"", "null", "http://smartfitness.local", "http://smartfitness.local:80",
                              "http://192.168.4.1", "https://smartfitness.local.evil",
                              "https://smartfitness.local@evil", "https://smartfitness.local:8443",
                              "https://smartfitness.local/", "https://192.168.1.13", "https://0.0.0.0"})
        assert(policy("application/json", origin) == 403);
    for (const auto* type : {"", "text/plain", "application/x-www-form-urlencoded", "multipart/form-data",
                            "application/jsonp"})
        assert(policy(type, "", false) == 415);
    assert(web::postRequestStatus("application/json", false, "", "", "", true) == 400);

    // HTTPS switched off: only plain-HTTP origins are trusted.
    const auto plain = [](const std::string& origin) {
        return web::postRequestStatus("application/json", true, origin, "192.168.1.12", "192.168.4.1", false, false);
    };
    for (const auto* origin : {"http://smartfitness.local", "http://smartfitness.local:80", "http://192.168.1.12",
                              "http://192.168.4.1"})
        assert(plain(origin) == 200);
    for (const auto* origin : {"https://smartfitness.local", "https://192.168.4.1", "http://smartfitness.local:443",
                              "http://smartfitness.local:8080", "http://192.168.1.13", "null"})
        assert(plain(origin) == 403);

    assert(!web::canBroadcast(0, true));
    assert(!web::canBroadcast(1, false));
    assert(web::canBroadcast(2, true));
}

void testHttpsRedirect() {
    const auto redirect = [](const std::string& host, const std::string& uri, const std::string& sta = "192.168.1.12",
                             const std::string& ap = "192.168.4.1") {
        return web::httpsRedirect(host, uri, sta, ap);
    };
    assert(web::hostName("192.168.4.1:80") == "192.168.4.1");
    assert(web::ownHost("smartfitness.local:80", "", "192.168.4.1"));
    assert(!web::ownHost("0.0.0.0", "0.0.0.0", ""));
    assert(redirect("smartfitness.local", "/") == "https://smartfitness.local/");
    assert(redirect("192.168.1.12:80", "/api/status?x=1") == "https://192.168.1.12/api/status?x=1");
    assert(redirect("192.168.4.1", "/generate_204") == "https://192.168.4.1/generate_204");
    // Captive-portal probes for foreign hosts and odd paths land on the UI root.
    assert(redirect("connectivitycheck.gstatic.com", "/generate_204") == "https://192.168.4.1/");
    assert(redirect("", "/") == "https://192.168.4.1/");
    assert(redirect("evil.example", "/", "192.168.1.12", "0.0.0.0") == "https://192.168.1.12/");
    assert(redirect("smartfitness.local", "//evil.example/") == "https://smartfitness.local/");
    assert(redirect("smartfitness.local", "/a\r\nSet-Cookie: x") == "https://smartfitness.local/");
    assert(redirect("smartfitness.local", "") == "https://smartfitness.local/");

    assert(std::string(web::statusLine(202)) == "202 Accepted");
    assert(std::string(web::statusLine(415)) == "415 Unsupported Media Type");
    assert(std::string(web::statusLine(418)) == "500 Internal Server Error");
}

void testBodyReceipt() {
    std::array<char, ApiExchange::kMaxBodySize + 1> bytes{};
    web::BodyReceipt body{4, 0, false};
    body.append(bytes.data(), "{\"", 2, 0, 4);
    assert(web::bodyStatus(&body, 4, false) == 400);
    body.append(bytes.data(), "\"}", 2, 2, 4);
    assert(web::bodyStatus(&body, 4, false) == 200);
    assert(bytes[4] == '\0');
    assert(web::bodyStatus(nullptr, 4, true) == 500);
    assert(web::bodyStatus(nullptr, 0, true) == 500);  // Never execute a bodyless reset on allocation failure.
    assert(web::bodyStatus(nullptr, 4, false) == 400);
    assert(web::bodyStatus(nullptr, 0, false) == 200);
    assert(web::bodyStatus(&body, 3, false) == 400);
    for (const auto index : {std::size_t(0), std::size_t(3), std::numeric_limits<std::size_t>::max()}) {
        body = {4, 2, false};
        body.append(bytes.data(), "ab", 2, index, 4);
        assert(web::bodyStatus(&body, 4, false) == 400);  // Overlap, gap, overflow.
    }
    body = {4, 0, false};
    body.append(bytes.data(), "abcd", 4, 0, 5);
    assert(body.invalid);  // Changing total.
    body = {4, 0, false};
    body.append(bytes.data(), "abcd", std::numeric_limits<std::size_t>::max(), 0, 4);
    assert(body.invalid);
    body = {4, 0, false};
    body.append(bytes.data(), "a\0bc", 4, 0, 4);
    assert(body.invalid);
    body = {ApiExchange::kMaxBodySize, 0, false};
    const std::string full(ApiExchange::kMaxBodySize, ' ');
    body.append(bytes.data(), full.data(), full.size(), 0, full.size());
    assert(web::bodyStatus(&body, full.size(), false) == 200);
    assert(bytes.back() == '\0');
    body.append(bytes.data(), "x", 1, full.size(), full.size());
    assert(body.invalid);
}

}  // namespace

void runWebTests() {
    testConsoleParsing();
    testConsoleBlePairing();
    testPairOverrideAndExclusion();
    testWifiCommands();
    testOutputProfiles();
    testApiParsing();
    testApiExchange();
    testApiExchangeThreads();
    testApiBleQueueRejections();
    testJsonViews();
    testWifiPolicy();
    testWifiScan();
    testRequestProtection();
    testBodyReceipt();
    testHttpsRedirect();
}
