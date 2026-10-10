#include "web_ui.h"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"
#include "ble_input_adapter.h"
#include "protocols/registry.hpp"
#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#ifdef ARDUINO
#include <Arduino.h>
#endif

using namespace smartfitness;
namespace {
const char* stateName(RuntimeState state) {
    switch (state) {
        case RuntimeState::Disconnected: return "disconnected";
        case RuntimeState::Connected: return "connected/read-only";
        case RuntimeState::Trainer: return "trainer";
        case RuntimeState::ControlReady: return "control-ready";
    }
    return "unknown";
}

const char* loadResultName(ConfigLoadResult result) {
    switch (result) {
        case ConfigLoadResult::Loaded: return "configuration loaded";
        case ConfigLoadResult::Missing: return "no saved configuration; defaults loaded";
        case ConfigLoadResult::Invalid: return "saved configuration invalid; defaults loaded";
    }
    return "unknown";
}

CommandResult ok(const std::string& message) { return {true, message}; }
CommandResult fail(const std::string& message) { return {false, message}; }

PairedSourceDevice toSource(const ble::BleAddress& address) {
    PairedSourceDevice source;
    source.valid = true;
    source.addressType = address.type;
    source.address = address.bytes;
    return source;
}

std::string serialized(const JsonDocument& document) {
    std::string out;
    serializeJson(document, out);
    return out;
}

void addMetric(JsonObject object, const char* key, const TrainerState& state, Metric metric) {
    const auto& sample = state.sample(metric);
    if (sample.valid) object[key] = std::round(static_cast<double>(sample.value) * 10.0) / 10.0;
    else object[key] = nullptr;
}
}  // namespace

WebUi::WebUi(BridgeCore& bridge, ConfigStore& config, SimulatedDeviceAdapter& adapter,
             AntOutput& ant, FtmsOutput& ftms)
    : bridge_(bridge), config_(config), adapter_(adapter), ant_(ant), ftms_(ftms) {}

void WebUi::attachBleInput(InputSelector& selector, BleInputAdapter& ble) {
    selector_ = &selector;
    ble_ = &ble;
}

bool WebUi::bleSelected() const {
    return selector_ && selector_->source() == InputSelector::Source::Ble;
}

void WebUi::applyConfig() {
    const auto& config = config_.config();
    adapter_.setTrainer(config.simulatedTrainer);
    ant_.setEnabled(config.antEnabled);
    ftms_.setDeviceName(config.ftmsName);
    ftms_.setProfileOverride(config.ftmsOutputProfile);
    ftms_.setEnabled(config.ftms.output);
    if (ble_ && selector_) {
        const bool paired = config.pairedSource.valid && !config.pairedAdapterId.empty();
        if (paired) {
            PairedDevice device;
            device.valid = true;
            device.address.bytes = config.pairedSource.address;
            device.address.type = config.pairedSource.addressType;
            device.adapterId = config.pairedAdapterId;
            device.name = config.pairedDeviceName;
            ble_->setPairedDevice(device);
        } else if (ble_->pairedDevice().valid) {
            ble_->clearPairedDevice();
        }
        selector_->select(config.ftms.input && paired ? InputSelector::Source::Ble
                                                      : InputSelector::Source::Simulation);
    }
    bridge_.loop();
}

void WebUi::begin() {
    applyConfig();
    // Auto-connect the paired BLE device on boot.
    if (bleSelected()) bridge_.scanAndConnect();
#ifdef ARDUINO
    Serial.println(ble_ ? "[WebUi] Serial console. Type help." : "[WebUi] Serial console; simulation only. Type help.");
#endif
}

std::string WebUi::status() const {
    char buffer[320];
    const auto& telemetry = bridge_.telemetry();
    std::snprintf(buffer, sizeof(buffer),
                  "state=%s speed=%.1fkm/h cadence=%.1frpm power=%.1fW hr=%.1fbpm "
                  "target=%s ANT=%d FTMS(input=%d,output=%d) "
                  "ftms-out(advertising=%d,client=%d,control=%d) unsaved=%d",
                  stateName(bridge_.state()), telemetry.speed, telemetry.cadence,
                  telemetry.power, telemetry.heartRate, bridge_.hasControlTarget() ? "set" : "none",
                  config_.config().antEnabled, config_.config().ftms.input, config_.config().ftms.output,
                  ftms_.advertising(), ftms_.clientConnected(), ftms_.controlGranted(), config_.hasUnsavedChanges());
    std::string text = buffer;
    text += std::string(" input-profile=") + machineProfileName(bridge_.trainerState().profile) +
            " output-profile=" + machineProfileName(ftms_.profile()) +
            " ftms-profile=" + ftmsOutputProfileName(config_.config().ftmsOutputProfile);
    if (ble_) {
        text += std::string(" input=") + (bleSelected() ? "ble" : "sim") +
                " link=" + BleInputAdapter::linkStateName(ble_->linkState());
        if (ble_->pairedDevice().valid)
            text += " paired=" + ble_->pairedDevice().address.toString() + "/" + ble_->pairedDevice().adapterId;
        if (ble_->stale()) text += " stale=1";
        if (!ble_->lastError().empty()) text += " error=\"" + ble_->lastError() + "\"";
        text += " dropped(scans=" + std::to_string(ble_->droppedScanReports()) +
                ",events=" + std::to_string(ble_->droppedEvents()) +
                ",notifications=" + std::to_string(ble_->droppedNotifications()) +
                ",commands=" + std::to_string(ble_->droppedCommands()) + ")";
    }
    return text;
}

std::string WebUi::handleCommand(const std::string& line) {
    Command command;
    std::string error;
    if (!parseConsoleCommand(line, command, error)) return error;
    return execute(command).message;
}

CommandResult WebUi::pair(const Command& command) {
    if (!ble_ || !selector_) return fail("BLE input not available");
    if (ble_->isExcluded(command.address)) return fail("refusing to pair with the bridge itself or its FTMS client");
    PairedDevice device;
    device.valid = true;
    device.address = command.address;
    const ScanResult* result = ble_->findResult(command.address);
    if (result) {
        if (!command.addressTypeGiven) device.address.type = result->address.type;
        device.name = result->name;
    }
    device.adapterId = command.adapterId.empty() ? (result ? result->detectedAdapter : std::string()) : command.adapterId;
    if (device.adapterId.empty())
        return fail(result ? "device type not detected; choose an adapter" : "device not in scan results; choose an adapter");
    if (!protocols::findProtocol(device.adapterId)) return fail("unknown adapter: " + device.adapterId);
    if (!config_.pairSource(toSource(device.address), device.name, device.adapterId))
        return fail("pairing not persisted (storage write failed)");
    applyConfig();
    if (!ble_->scanAndConnect()) return fail("paired, but connection request rejected: " + ble_->lastError());
    bridge_.loop();
    const std::string label = device.name.empty() ? device.address.toString() : device.name + " (" + device.address.toString() + ")";
    return ok("paired " + label + " as " + device.adapterId + "; connecting");
}

CommandResult WebUi::execute(const Command& command) {
    switch (command.type) {
        case CommandType::Help:
            return ok("status | sim | scan [s] | scan-stop | scan-results | pair <addr> [adapter|auto] [public|random] | "
                      "connect | disconnect | unpair | trainer on/off | "
                      "power W | speed km/h | incline % | resistance % | "
                      "ant on/off | ftms-input on/off | ftms-output on/off | ftms-name <name> | "
                      "ftms-profile auto/bike/treadmill/cross-trainer/rower | "
                      "wifi <ssid> [password] | wifi-clear | ap-pass <password> | "
                      "save | load | factory-reset | restart");
        case CommandType::Status: return ok(status());
        case CommandType::Scan:
            if (ble_) {
                const std::uint32_t duration = command.durationMs ? command.durationMs : BleInputAdapter::kDefaultScanMs;
                if (!ble_->startScan(duration)) return fail("scan not started: " + ble_->lastError());
                char buffer[48];
                std::snprintf(buffer, sizeof(buffer), "scanning for %u s", static_cast<unsigned>(duration / 1000));
                return ok(buffer);
            }
            // Without BLE input "scan" keeps its original meaning: connect the simulation.
            [[fallthrough]];
        case CommandType::Simulate: {
            if (ble_ && config_.config().ftms.input) {
                config_.config().ftms.input = false;
                applyConfig();
            }
            if (!bridge_.scanAndConnect()) return fail("connection failed");
            if (!ble_) config_.pairDevice(bridge_.telemetry().deviceName);
            bridge_.loop();
            return ok("simulation connected; " + status());
        }
        case CommandType::ScanStop:
            if (!ble_) return fail("BLE input not available");
            if (!ble_->stopScan()) return fail("scan stop rejected: " + ble_->lastError());
            return ok("scan stopped");
        case CommandType::ScanResults: {
            if (!ble_) return fail("BLE input not available");
            std::string text = ble_->scanning() ? "scanning; " : "";
            char line[160];
            std::snprintf(line, sizeof(line), "%u device(s)", static_cast<unsigned>(ble_->scanResults().size()));
            text += line;
            for (const auto& result : ble_->scanResults()) {
                std::snprintf(line, sizeof(line), "\n  %s %s rssi=%d type=%s name=\"%s\"",
                              result.address.toString().c_str(), result.address.type ? "random" : "public",
                              result.rssi, result.detectedAdapter.empty() ? "?" : result.detectedAdapter.c_str(),
                              result.name.c_str());
                text += line;
            }
            return ok(text);
        }
        case CommandType::Pair: return pair(command);
        case CommandType::Connect:
            if (!ble_) return fail("BLE input not available");
            if (!config_.config().pairedSource.valid) return fail("no paired device; scan and pair first");
            config_.config().ftms.input = true;
            applyConfig();
            if (!ble_->scanAndConnect()) return fail("connection request rejected: " + ble_->lastError());
            bridge_.loop();
            return ok("connecting; " + status());
        case CommandType::Disconnect:
        case CommandType::Unpair: {
            const bool accepted = !bleSelected() || ble_->requestDisconnect();
            bridge_.disconnect();
            if (!accepted) return fail("disconnect request rejected: " + ble_->lastError());
            const bool persisted = command.type != CommandType::Unpair || config_.unpair();
            if (command.type == CommandType::Unpair) applyConfig();
            bridge_.loop();
            return persisted ? ok(status()) : fail("unpair not persisted (storage write failed); " + status());
        }
        case CommandType::Save:
            switch (config_.save()) {
                case ConfigSaveResult::Written: return ok("configuration saved");
                case ConfigSaveResult::Unchanged: return ok("configuration unchanged; nothing written");
                case ConfigSaveResult::Invalid: return fail("configuration invalid; not saved");
                case ConfigSaveResult::Failed: return fail("save failed (storage write failed)");
            }
            return fail("save failed");
        case CommandType::Load: {
            const auto result = config_.load();
            applyConfig();
            return {result != ConfigLoadResult::Invalid, std::string(loadResultName(result)) + "; " + status()};
        }
        case CommandType::FactoryReset: {
            bridge_.disconnect();
            const bool erased = config_.factoryReset();
            applyConfig();
            return {erased, std::string(erased ? "factory defaults restored; " : "erase failed; defaults restored in RAM; ") +
                                status()};
        }
        case CommandType::FtmsName: {
            const auto& value = command.text;
            if (value.empty() || value.size() > FtmsOutput::kMaxNameLength) return fail("expected 1-20 characters");
            if (!ConfigStore::isValidFtmsName(value)) return fail("expected printable ASCII");
            config_.config().ftmsName = value;
            applyConfig();
            return ok("ftms name: " + ftms_.deviceName());
        }
        case CommandType::FtmsProfile: {
            FtmsOutputProfile profile;
            if (!parseFtmsOutputProfile(command.text, profile))
                return fail("profile must be auto, bike, treadmill, cross-trainer or rower");
            config_.config().ftmsOutputProfile = profile;
            applyConfig();
            return ok(status());
        }
        case CommandType::Trainer:
        case CommandType::Ant:
        case CommandType::FtmsInput:
        case CommandType::FtmsOutput: {
            auto& config = config_.config();
            bool* setting = command.type == CommandType::Trainer ? &config.simulatedTrainer
                            : command.type == CommandType::Ant   ? &config.antEnabled
                            : command.type == CommandType::FtmsInput ? &config.ftms.input
                                                                     : &config.ftms.output;
            *setting = command.flag;
            applyConfig();
            return ok(status());
        }
        case CommandType::Wifi:
            if (command.text.empty() || !ConfigStore::isValidSsid(command.text)) return fail("expected SSID of 1-32 bytes");
            if (!ConfigStore::isValidPassword(command.text2, true))
                return fail("password must be empty or 8-63 printable characters");
            config_.config().wifiSsid = command.text;
            if (!command.flag) config_.config().wifiPassword = command.text2;  // flag: keep the stored password
            return ok("wifi: " + command.text + "; save and restart to apply");
        case CommandType::WifiClear:
            config_.config().wifiSsid.clear();
            config_.config().wifiPassword.clear();
            return ok("wifi cleared (access point only); save and restart to apply");
        case CommandType::ApPassword:
            if (!ConfigStore::isValidApPassword(command.text))
                return fail("access point password must be 8-63 printable ASCII characters");
            config_.config().apPassword = command.text;
            return ok("access point password set; save and restart to apply");
        case CommandType::Restart:
            restartRequested_ = true;
            return ok("restarting");
        case CommandType::Control: {
            const auto result = bridge_.routeControl({command.control, command.value});
            bridge_.loop();
            return {result == ControlResult::Applied, std::string("control: ") + controlResultName(result)};
        }
    }
    return fail("unknown command; type help");
}

std::string WebUi::statusJson() const {
    JsonDocument document;
    document["type"] = "status";
    document["version"] = SMARTFITNESS_VERSION;
    document["state"] = stateName(bridge_.state());
    const auto& state = bridge_.trainerState();
    JsonObject telemetry = document["telemetry"].to<JsonObject>();
    addMetric(telemetry, "power", state, Metric::Power);
    addMetric(telemetry, "cadence", state, Metric::Cadence);
    addMetric(telemetry, "speed", state, Metric::Speed);
    addMetric(telemetry, "heartRate", state, Metric::HeartRate);
    addMetric(telemetry, "incline", state, Metric::Incline);
    addMetric(telemetry, "distance", state, Metric::Distance);
    addMetric(telemetry, "elapsed", state, Metric::ElapsedTime);
    document["deviceName"] = bridge_.telemetry().deviceName;
    document["trainer"] = state.trainerDetected;
    document["canControl"] = bridge_.canControl();
    JsonArray controls = document["controls"].to<JsonArray>();
    for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(Control::Count); ++index) {
        const auto control = static_cast<Control>(index);
        if (state.capabilities.supports(control)) controls.add(controlName(control));
    }
    if (bridge_.hasControlTarget()) {
        JsonObject target = document["target"].to<JsonObject>();
        target["control"] = controlName(bridge_.controlTarget().control);
        target["value"] = bridge_.controlTarget().value;
    } else {
        document["target"] = nullptr;
    }
    JsonObject input = document["input"].to<JsonObject>();
    input["source"] = bleSelected() ? "ble" : "simulation";
    input["profile"] = machineProfileName(state.profile);
    input["bleAvailable"] = ble_ != nullptr;
    if (ble_) {
        input["link"] = BleInputAdapter::linkStateName(ble_->linkState());
        input["protocol"] = ble_->protocolId();
        input["stale"] = ble_->stale();
        input["scanning"] = ble_->scanning();
        input["error"] = ble_->lastError();
        input["reconnectAttempts"] = ble_->reconnectAttempts();
        input["nextAttemptMs"] = ble_->nextAttemptInMs();
        input["autoConnect"] = ble_->autoConnect();
        JsonObject dropped = input["dropped"].to<JsonObject>();
        dropped["scans"] = ble_->droppedScanReports();
        dropped["events"] = ble_->droppedEvents();
        dropped["notifications"] = ble_->droppedNotifications();
        dropped["commands"] = ble_->droppedCommands();
    }
    const auto& config = config_.config();
    if (config.pairedSource.valid) {
        JsonObject paired = input["paired"].to<JsonObject>();
        ble::BleAddress address;
        address.bytes = config.pairedSource.address;
        paired["address"] = address.toString();
        paired["addressType"] = config.pairedSource.addressType ? "random" : "public";
        paired["name"] = config.pairedDeviceName;
        paired["adapter"] = config.pairedAdapterId;
    } else {
        input["paired"] = nullptr;
    }
    JsonObject ftms = document["ftmsOutput"].to<JsonObject>();
    ftms["enabled"] = config.ftms.output;
    ftms["profile"] = machineProfileName(ftms_.profile());
    ftms["override"] = ftmsOutputProfileName(config.ftmsOutputProfile);
    ftms["name"] = ftms_.deviceName();
    ftms["advertising"] = ftms_.advertising();
    ftms["client"] = ftms_.clientConnected();
    ftms["control"] = ftms_.controlGranted();
    document["ant"] = config.antEnabled;
    document["unsaved"] = config_.hasUnsavedChanges();
    JsonObject network = document["network"].to<JsonObject>();
    network["mode"] = network_.mode;
    network["ssid"] = network_.ssid;
    network["ip"] = network_.ip;
    network["hostname"] = network_.hostname;
    network["stations"] = network_.stations;
    return serialized(document);
}

std::string WebUi::scanResultsJson() const {
    JsonDocument document;
    document["type"] = "scan";
    document["scanning"] = ble_ && ble_->scanning();
    document["bleAvailable"] = ble_ != nullptr;
    document["dropped"] = ble_ ? ble_->droppedScanResults() : 0U;
    JsonArray devices = document["devices"].to<JsonArray>();
    if (ble_) {
        for (const auto& result : ble_->scanResults()) {
            JsonObject device = devices.add<JsonObject>();
            device["address"] = result.address.toString();
            device["addressType"] = result.address.type ? "random" : "public";
            device["name"] = result.name;
            device["rssi"] = result.rssi;
            device["connectable"] = result.connectable;
            device["adapter"] = result.detectedAdapter;
            JsonArray services = device["services"].to<JsonArray>();
            for (const auto& uuid : result.services) services.add(uuid.toString());
        }
    }
    JsonArray adapters = document["adapters"].to<JsonArray>();
    for (std::size_t index = 0; index < protocols::protocolCount(); ++index) {
        const auto& info = protocols::protocolAt(index);
        JsonObject adapter = adapters.add<JsonObject>();
        adapter["id"] = info.id;
        adapter["label"] = info.label;
        adapter["controllable"] = info.controllable;
        adapter["advertisementOnly"] = info.advertisementOnly;
    }
    return serialized(document);
}

std::string WebUi::configJson() const {
    const auto& config = config_.config();
    JsonDocument document;
    document["type"] = "config";
    document["version"] = SMARTFITNESS_VERSION;
    document["ftmsName"] = config.ftmsName.empty() ? std::string(FtmsOutput::kDefaultName) : config.ftmsName;
    document["ftmsOutput"] = config.ftms.output;
    document["ftmsOutputProfile"] = ftmsOutputProfileName(config.ftmsOutputProfile);
    document["ftmsInput"] = config.ftms.input;
    document["ant"] = config.antEnabled;
    document["antRequiresHardware"] = true;
    document["simulatedTrainer"] = config.simulatedTrainer;
    // Passwords are never returned, only whether they are set.
    document["wifiSsid"] = config.wifiSsid;
    document["wifiPasswordSet"] = !config.wifiPassword.empty();
    document["apPasswordDefault"] = config.apPassword == BridgeConfig{}.apPassword;
    document["unsaved"] = config_.hasUnsavedChanges();
    return serialized(document);
}

void WebUi::loop() {
#ifdef ARDUINO
    // Bound work and line length so a noisy console cannot starve the bridge.
    for (unsigned int count = 0; count < 64 && Serial.available(); ++count) {
        const char character = static_cast<char>(Serial.read());
        if (character == '\r') continue;
        if (character == '\n') {
            if (overflow_) Serial.println("[WebUi] command too long");
            else if (!input_.empty()) Serial.println(handleCommand(input_).c_str());
            input_.clear();
            overflow_ = false;
        } else if (!overflow_) {
            if (input_.size() < kMaxCommandLength) input_ += character;
            else overflow_ = true;
        }
    }
#endif
}
