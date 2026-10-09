// Host tests for the shared command layer (console + web API), the API
// exchange between the web task and the main loop, JSON views and the Wi-Fi
// policy.

#include <ArduinoJson.h>

#include <cassert>
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
#include "web_ui.h"
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
    WebUi ui{core, config, simulation, ant, ftms};

    Rig() {
        config.begin();
        core.attach(selector, ant, ftms);
        core.begin();
        ui.attachBleInput(selector, ble);
        ui.begin();
    }
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
    assert(rig.config.config().ftms.input && rig.config.config().pairedAdapterId == "ftms");
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
        WebUi ui(core, config, simulation, ant, ftms);
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
    assert(rig.config.config().pairedSource.valid && !rig.config.config().ftms.input);
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
    assert(rig.ui.handleCommand("ap-pass 1234") == "access point password must be 8-63 printable characters");
    assert(rig.ui.handleCommand("ap-pass new-ap-pass") == "access point password set; save and restart to apply");
    assert(rig.ui.handleCommand("save") == "configuration saved");
    assert(rig.ui.handleCommand("wifi-clear") == "wifi cleared (access point only); save and restart to apply");
    assert(rig.config.config().wifiSsid.empty() && rig.config.hasUnsavedChanges());
    assert(!rig.ui.restartRequested());
    assert(rig.ui.handleCommand("restart") == "restarting" && rig.ui.restartRequested());
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
                           R"({"ftmsName":"Garage","ftmsOutput":false,"ant":true,"wifiSsid":"Home",)"
                           R"("wifiPassword":"secret-pass","apPassword":"ap-password","save":true})",
                           commands, error));
    assert(commands.size() == 6);
    assert(commands[0].type == CommandType::FtmsName && commands[0].text == "Garage");
    assert(commands[1].type == CommandType::FtmsOutput && !commands[1].flag);
    assert(commands[2].type == CommandType::Ant && commands[2].flag);
    assert(commands[3].type == CommandType::Wifi && commands[3].text2 == "secret-pass" && !commands[3].flag);
    assert(commands[4].type == CommandType::ApPassword);
    assert(commands[5].type == CommandType::Save);
    assert(parseApiRequest("config", R"({"wifiSsid":"Home"})", commands, error) && commands[0].flag);  // keep password
    assert(parseApiRequest("config", R"({"wifiSsid":""})", commands, error) &&
           commands[0].type == CommandType::WifiClear);
    assert(!parseApiRequest("config", R"({"wifiPassword":"secret-pass"})", commands, error));
    assert(!parseApiRequest("config", R"({"ant":"yes"})", commands, error) && contains(error, "true or false"));
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
    api.publish(ApiExchange::Snapshot::Status, rig.ui.statusJson());
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
        api.publish(ApiExchange::Snapshot::Status, rig.ui.statusJson());
    }
    web.join();
    processed += api.process(rig.ui, 100).size();
    assert(processed == ids.size());
    assert(ids.size() + api.droppedRequests() == 200);
}

void testJsonViews() {
    Rig rig;
    rig.ui.handleCommand("wifi Home secret-pass");
    rig.ui.handleCommand("ap-pass super-secret-ap");
    const std::string config = rig.ui.configJson();
    assert(!contains(config, "secret-pass") && !contains(config, "super-secret-ap"));
    auto document = parse(config);
    assert(document["wifiSsid"] == "Home" && document["wifiPasswordSet"] == true);
    assert(document["apPasswordDefault"] == false && document["ftmsName"] == FtmsOutput::kDefaultName);
    assert(document["antRequiresHardware"] == true && document["unsaved"] == true);

    rig.ui.handleCommand("scan");
    rig.central.advertise(address("aa:00:00:00:00:41"), "ECH-SPORT-1", {});
    rig.core.loop();
    document = parse(rig.ui.scanResultsJson());
    assert(document["scanning"] == true && document["devices"].size() == 1);
    assert(document["devices"][0]["address"] == "aa:00:00:00:00:41");
    assert(document["devices"][0]["adapter"] == "echelon");
    assert(document["adapters"].size() > 5);

    rig.ui.handleCommand("sim");
    rig.ui.handleCommand("power 180");
    rig.ui.setNetworkStatus({"ap", "SmartFitness-ABCD", "192.168.4.1", "smartfitness", 1});
    document = parse(rig.ui.statusJson());
    assert(document["state"] == "control-ready" && document["input"]["source"] == "simulation");
    assert(document["telemetry"]["power"].is<double>() && document["telemetry"]["heartRate"].is<double>());
    assert(document["target"]["control"] == "power" && document["target"]["value"] == 180);
    assert(document["controls"].size() == 4 && document["canControl"] == true);
    assert(document["network"]["ip"] == "192.168.4.1" && document["input"]["paired"].isNull());
    assert(document["ftmsOutput"]["advertising"] == true);
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

}  // namespace

void runWebTests() {
    testConsoleParsing();
    testConsoleBlePairing();
    testPairOverrideAndExclusion();
    testWifiCommands();
    testApiParsing();
    testApiExchange();
    testApiExchangeThreads();
    testJsonViews();
    testWifiPolicy();
}
