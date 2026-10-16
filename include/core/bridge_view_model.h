#pragma once

// Variant-neutral view model of the bridge. BridgeController fills these
// snapshots on the main loop; the web variant renders them as JSON and the
// HMI variant shows them on the LVGL screens. Plain values only (no pointers
// into the core), so a snapshot can be copied and rendered later or faked by
// the HMI simulator. Pure C++ (host tested).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bridge/bridge_core.hpp"
#include "bridge/machine_profile.hpp"
#include "bridge/output_protocol.hpp"

namespace smartfitness {

const char* runtimeStateName(RuntimeState state);

struct MetricView {
    bool valid = false;
    float value = 0.0F;
};

struct DroppedCounters {
    std::uint32_t scans = 0;
    std::uint32_t events = 0;
    std::uint32_t notifications = 0;
    std::uint32_t commands = 0;
};

struct SubscriptionView {
    std::string service;
    std::string characteristic;
    std::string state;
    bool indicate = false;
};

struct PairedView {
    bool valid = false;
    std::string address;  // "aa:bb:cc:dd:ee:ff"
    bool randomAddress = false;
    std::string name;
    std::string adapter;
    std::vector<std::string> advertisedServices;
    std::vector<SubscriptionView> subscriptions;
};

struct InputView {
    bool bleSelected = false;  // false: simulation
    MachineProfile profile = MachineProfile::IndoorBike;
    bool bleAvailable = false;
    // Valid only when bleAvailable:
    std::string link;          // BleInputAdapter::linkStateName()
    std::string protocol;
    bool stale = false;
    bool scanning = false;
    std::string error;
    std::uint32_t reconnectAttempts = 0;
    std::uint32_t nextAttemptMs = 0;
    bool autoConnect = false;
    DroppedCounters dropped;
    PairedView paired;
};

struct OutputServiceView {
    std::string id;
    std::string name;
    bool enabled = false;
    bool available = false;
    bool active = false;
};

struct AntChannelView {
    std::string id;
    std::string name;
    std::uint8_t deviceType = 0;
    bool enabled = false;
    bool open = false;
};

struct FtmsOutputView {
    bool enabled = false;
    MachineProfile profile = MachineProfile::IndoorBike;
    FtmsOutputProfile override = FtmsOutputProfile::Auto;
    std::string name;
    bool advertising = false;
    bool client = false;
    bool control = false;
    bool restartRequired = false;
    std::vector<OutputServiceView> services;
};

struct AntOutputView {
    bool enabled = false;
    bool available = false;    // an ANT radio transport exists (web variant)
    std::string link;          // AntOutput::linkName()
    bool stick = false;        // ANT+ USB stick attached
    bool open = false;         // FE-C channel broadcasting
    std::uint16_t deviceNumber = 0;
    MachineProfile profile = MachineProfile::IndoorBike;
    bool display = false;      // a display (app/head unit) talked recently
    bool control = false;      // the display controls the source
    std::string hardware;
    std::string error;
    std::uint8_t channelLimit = 0;
    std::vector<AntChannelView> channels;
};

struct StatusView {
    std::string version;
    RuntimeState state = RuntimeState::Disconnected;
    std::array<MetricView, static_cast<std::size_t>(Metric::Count)> telemetry{};
    std::string deviceName;
    bool trainer = false;
    bool canControl = false;
    std::array<bool, static_cast<std::size_t>(Control::Count)> controls{};  // supported controls
    bool hasTarget = false;
    ControlRequest target{Control::Power, 0.0F};
    InputView input;
    OutputProtocol outputProtocol = OutputProtocol::BleFtms;
    FtmsOutputView ftmsOutput;
    AntOutputView antOutput;
    bool unsaved = false;

    const MetricView& metric(Metric which) const { return telemetry[static_cast<std::size_t>(which)]; }
    bool supports(Control control) const { return controls[static_cast<std::size_t>(control)]; }
};

struct ScanDeviceView {
    std::string address;
    bool randomAddress = false;
    std::string name;
    int rssi = 0;
    bool connectable = true;
    std::string adapter;  // detected adapter id; empty when unknown
    std::vector<std::string> services;
    std::string label;
    bool relevant = false;
};

struct AdapterView {
    const char* id = "";
    const char* label = "";
    bool controllable = false;
    bool advertisementOnly = false;
    MachineProfile profile = MachineProfile::IndoorBike;
};

struct ScanView {
    bool scanning = false;
    bool bleAvailable = false;
    std::uint32_t dropped = 0;
    std::vector<ScanDeviceView> devices;
};

struct ConfigView {
    std::string version;
    std::string ftmsName;  // effective name (default when unset)
    OutputProtocol outputProtocol = OutputProtocol::BleFtms;
    bool antAvailable = false;  // ANT+ FE-C selectable (ANT radio transport present)
    FtmsOutputProfile ftmsOutputProfile = FtmsOutputProfile::Auto;  // output profile (FTMS and FE-C)
    bool ftmsInput = false;
    bool simulatedTrainer = false;
    std::string wifiSsid;
    bool wifiPasswordSet = false;  // passwords are never exposed
    bool apPasswordDefault = true;
    bool https = true;  // web interface over HTTPS (applied after restart)
    bool unsaved = false;
    std::uint8_t ftmsOutputServices = 0;
    std::uint8_t antOutputServices = 0;
};

// All selectable input adapters (static registry data).
std::vector<AdapterView> adapterViews();

}  // namespace smartfitness
