#include "web_json.h"

#include <ArduinoJson.h>
#include <cmath>

#include "bridge_commands.h"

using namespace smartfitness;

namespace {
std::string serialized(const JsonDocument& document) {
    std::string out;
    serializeJson(document, out);
    return out;
}

void addMetric(JsonObject object, const char* key, const StatusView& status, Metric metric) {
    const auto& sample = status.metric(metric);
    if (sample.valid) object[key] = std::round(static_cast<double>(sample.value) * 10.0) / 10.0;
    else object[key] = nullptr;
}
}  // namespace

namespace web {

std::string statusJson(const StatusView& status, const NetworkStatus& network) {
    JsonDocument document;
    document["type"] = "status";
    document["version"] = status.version;
    document["state"] = runtimeStateName(status.state);
    JsonObject telemetry = document["telemetry"].to<JsonObject>();
    addMetric(telemetry, "power", status, Metric::Power);
    addMetric(telemetry, "cadence", status, Metric::Cadence);
    addMetric(telemetry, "speed", status, Metric::Speed);
    addMetric(telemetry, "heartRate", status, Metric::HeartRate);
    addMetric(telemetry, "incline", status, Metric::Incline);
    addMetric(telemetry, "distance", status, Metric::Distance);
    addMetric(telemetry, "elapsed", status, Metric::ElapsedTime);
    // Further FTMS metrics are only listed when the device actually sends them.
    for (std::size_t index = static_cast<std::size_t>(Metric::Resistance); index < kMetricCount; ++index) {
        const auto metric = static_cast<Metric>(index);
        if (status.metric(metric).valid) addMetric(telemetry, metricKey(metric), status, metric);
    }
    document["deviceName"] = status.deviceName;
    document["trainer"] = status.trainer;
    document["canControl"] = status.canControl;
    JsonArray controls = document["controls"].to<JsonArray>();
    for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(Control::Count); ++index) {
        const auto control = static_cast<Control>(index);
        if (status.supports(control)) controls.add(controlName(control));
    }
    if (status.hasTarget) {
        JsonObject target = document["target"].to<JsonObject>();
        target["control"] = controlName(status.target.control);
        target["value"] = status.target.value;
    } else {
        document["target"] = nullptr;
    }
    const auto& in = status.input;
    JsonObject input = document["input"].to<JsonObject>();
    input["source"] = in.bleSelected ? "ble" : "simulation";
    input["profile"] = machineProfileName(in.profile);
    input["bleAvailable"] = in.bleAvailable;
    if (in.bleAvailable) {
        input["link"] = in.link;
        input["protocol"] = in.protocol;
        input["stale"] = in.stale;
        input["scanning"] = in.scanning;
        input["error"] = in.error;
        input["reconnectAttempts"] = in.reconnectAttempts;
        input["nextAttemptMs"] = in.nextAttemptMs;
        input["autoConnect"] = in.autoConnect;
        JsonObject dropped = input["dropped"].to<JsonObject>();
        dropped["scans"] = in.dropped.scans;
        dropped["events"] = in.dropped.events;
        dropped["notifications"] = in.dropped.notifications;
        dropped["commands"] = in.dropped.commands;
    }
    if (in.paired.valid) {
        JsonObject paired = input["paired"].to<JsonObject>();
        paired["address"] = in.paired.address;
        paired["addressType"] = in.paired.randomAddress ? "random" : "public";
        paired["name"] = in.paired.name;
        paired["adapter"] = in.paired.adapter;
        paired["rssi"] = in.paired.rssi;
        paired["signal"] = signalLevel(in.paired.rssi);
    } else {
        input["paired"] = nullptr;
    }
    const auto& out = status.ftmsOutput;
    JsonObject ftms = document["ftmsOutput"].to<JsonObject>();
    ftms["enabled"] = out.enabled;
    ftms["profile"] = machineProfileName(out.profile);
    ftms["override"] = ftmsOutputProfileName(out.override);
    ftms["name"] = out.name;
    ftms["advertising"] = out.advertising;
    ftms["client"] = out.client;
    ftms["control"] = out.control;
    document["ant"] = status.ant;
    document["unsaved"] = status.unsaved;
    JsonObject net = document["network"].to<JsonObject>();
    net["mode"] = network.mode;
    net["ssid"] = network.ssid;
    net["ip"] = network.ip;
    net["hostname"] = network.hostname;
    net["stations"] = network.stations;
    return serialized(document);
}

std::string scanResultsJson(const ScanView& scan) {
    JsonDocument document;
    document["type"] = "scan";
    document["scanning"] = scan.scanning;
    document["bleAvailable"] = scan.bleAvailable;
    document["dropped"] = scan.dropped;
    JsonArray devices = document["devices"].to<JsonArray>();
    for (const auto& result : scan.devices) {
        JsonObject device = devices.add<JsonObject>();
        device["address"] = result.address;
        device["addressType"] = result.randomAddress ? "random" : "public";
        device["name"] = result.name;
        device["rssi"] = result.rssi;
        device["signal"] = signalLevel(result.rssi);
        device["connectable"] = result.connectable;
        device["adapter"] = result.adapter;
        JsonArray services = device["services"].to<JsonArray>();
        for (const auto& uuid : result.services) services.add(uuid);
    }
    JsonArray adapters = document["adapters"].to<JsonArray>();
    for (const auto& info : adapterViews()) {
        JsonObject adapter = adapters.add<JsonObject>();
        adapter["id"] = info.id;
        adapter["label"] = info.label;
        adapter["controllable"] = info.controllable;
        adapter["advertisementOnly"] = info.advertisementOnly;
    }
    return serialized(document);
}

std::string configJson(const ConfigView& config) {
    JsonDocument document;
    document["type"] = "config";
    document["version"] = config.version;
    document["ftmsName"] = config.ftmsName;
    document["ftmsOutput"] = config.ftmsOutput;
    document["ftmsOutputProfile"] = ftmsOutputProfileName(config.ftmsOutputProfile);
    document["ftmsInput"] = config.ftmsInput;
    document["ant"] = config.ant;
    document["antRequiresHardware"] = config.antRequiresHardware;
    document["simulatedTrainer"] = config.simulatedTrainer;
    // Passwords are never returned, only whether they are set.
    document["wifiSsid"] = config.wifiSsid;
    document["wifiPasswordSet"] = config.wifiPasswordSet;
    document["apPasswordDefault"] = config.apPasswordDefault;
    document["unsaved"] = config.unsaved;
    return serialized(document);
}

}  // namespace web
