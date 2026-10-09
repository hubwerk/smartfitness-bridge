#include "web_ui.h"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"
#include <cstdio>
#include <cstdlib>
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

const char* resultName(ControlResult result) {
    switch (result) {
        case ControlResult::Applied: return "applied";
        case ControlResult::Disconnected: return "disconnected";
        case ControlResult::NotTrainer: return "not a trainer";
        case ControlResult::Unsupported: return "unsupported control";
        case ControlResult::InvalidValue: return "invalid value";
        case ControlResult::NoAdapter: return "no adapter";
        case ControlResult::AdapterRejected: return "adapter rejected";
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
}

WebUi::WebUi(BridgeCore& bridge, ConfigStore& config, SimulatedDeviceAdapter& adapter,
             AntOutput& ant, FtmsOutput& ftms)
    : bridge_(bridge), config_(config), adapter_(adapter), ant_(ant), ftms_(ftms) {}

void WebUi::applyConfig() {
    adapter_.setTrainer(config_.config().simulatedTrainer);
    ant_.setEnabled(config_.config().antEnabled);
    ftms_.setDeviceName(config_.config().ftmsName);
    ftms_.setEnabled(config_.config().ftms.output);
    bridge_.loop();
}

void WebUi::begin() {
    applyConfig();
#ifdef ARDUINO
    Serial.println("[WebUi] Serial console; simulation only. Type help.");
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
    return buffer;
}

std::string WebUi::handleCommand(const std::string& command) {
    if (command.size() > 96) return "command too long";
    if (command == "help")
        return "status | scan (simulation) | disconnect | trainer on/off | "
               "power W | speed km/h | incline % | resistance % | "
               "ant on/off | ftms-input on/off | ftms-output on/off | ftms-name <name> | "
               "save | load | unpair | factory-reset";
    if (command == "status") return status();
    if (command == "scan") {
        if (!bridge_.scanAndConnect()) return "connection failed";
        config_.pairDevice(bridge_.telemetry().deviceName);
        bridge_.loop();
        return "simulation connected; " + status();
    }
    if (command == "disconnect" || command == "unpair") {
        bridge_.disconnect();
        const bool persisted = command != "unpair" || config_.unpair();
        bridge_.loop();
        return persisted ? status() : "unpair not persisted (storage write failed); " + status();
    }
    if (command == "save") {
        switch (config_.save()) {
            case ConfigSaveResult::Written: return "configuration saved";
            case ConfigSaveResult::Unchanged: return "configuration unchanged; nothing written";
            case ConfigSaveResult::Invalid: return "configuration invalid; not saved";
            case ConfigSaveResult::Failed: return "save failed (storage write failed)";
        }
        return "save failed";
    }
    if (command == "load") {
        const auto result = config_.load();
        applyConfig();
        return std::string(loadResultName(result)) + "; " + status();
    }
    if (command == "factory-reset") {
        bridge_.disconnect();
        const bool erased = config_.factoryReset();
        applyConfig();
        return std::string(erased ? "factory defaults restored; " : "erase failed; defaults restored in RAM; ") +
               status();
    }
    const auto separator = command.find(' ');
    if (separator == std::string::npos) return "unknown command; type help";
    const auto key = command.substr(0, separator);
    const auto value = command.substr(separator + 1);
    if (key == "ftms-name") {
        if (value.empty() || value.size() > FtmsOutput::kMaxNameLength) return "expected 1-20 characters";
        if (!ConfigStore::isValidFtmsName(value)) return "expected printable ASCII";
        config_.config().ftmsName = value;
        applyConfig();
        return "ftms name: " + ftms_.deviceName();
    }
    bool* setting = nullptr;
    if (key == "trainer") setting = &config_.config().simulatedTrainer;
    else if (key == "ant") setting = &config_.config().antEnabled;
    else if (key == "ftms-input") setting = &config_.config().ftms.input;
    else if (key == "ftms-output") setting = &config_.config().ftms.output;
    if (setting) {
        if (value != "on" && value != "off") return "expected on or off";
        *setting = value == "on";
        applyConfig();
        return status();
    }
    Control control;
    if (key == "power") control = Control::Power;
    else if (key == "speed") control = Control::Speed;
    else if (key == "incline") control = Control::Incline;
    else if (key == "resistance") control = Control::Resistance;
    else return "unknown command; type help";
    char* end = nullptr;
    const float target = std::strtof(value.c_str(), &end);
    if (value.empty() || end == value.c_str() || end != value.c_str() + value.size())
        return "invalid value";
    const auto result = bridge_.routeControl({control, target});
    bridge_.loop();
    return std::string("control: ") + resultName(result);
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
            if (input_.size() < 96) input_ += character;
            else overflow_ = true;
        }
    }
#endif
}
