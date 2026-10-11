#pragma once

// HMI variant: view logic shared by the LVGL screens, the firmware glue and
// the host simulator. Pure C++ (no LVGL, no Arduino), host tested. All bridge
// actions are expressed as the shared Commands; the HMI never touches the core
// directly.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "bridge_commands.h"
#include "bridge_view_model.h"
#include "hmi_strings.h"

namespace hmi {

// Telemetry refresh <= 5 Hz; scan list and settings at 1 Hz.
constexpr std::uint32_t kStatusRefreshMs = 200;
constexpr std::uint32_t kScanRefreshMs = 1000;

// ---- Metrics (dashboard) ----------------------------------------------------

struct MetricFormat {
    Str name;
    const char* unit;
    int decimals;
    float gaugeMin;
    float gaugeMax;
};

const MetricFormat& metricFormat(smartfitness::Metric metric);
// Metrics offered on the dashboard gauge, most relevant first for the profile.
std::vector<smartfitness::Metric> dashboardMetrics(smartfitness::MachineProfile profile);
// "123" / "8.5"; "--" when the sample is invalid.
std::string formatValue(float value, int decimals);
std::string formatMetric(const smartfitness::MetricView& sample, smartfitness::Metric metric);

// Knob/swipe selection of the gauge metric; keeps the metric when the profile
// (and therefore the order) changes.
class MetricCycler {
public:
    smartfitness::Metric current(smartfitness::MachineProfile profile) const;
    void step(smartfitness::MachineProfile profile, int detents);

private:
    smartfitness::Metric metric_ = smartfitness::Metric::Count;  // Count: first of the profile
};

// ---- Control targets --------------------------------------------------------

struct ControlSpec {
    smartfitness::Control control;
    Str name;
    const char* unit;
    float minimum;  // HMI range (inside the BridgeCore safety bounds)
    float maximum;
    float step;     // one knob detent
    int decimals;
};

const ControlSpec& controlSpec(smartfitness::Control control);
// Control order on the Control screen, most relevant first for the profile.
std::vector<smartfitness::Control> controlOrder(smartfitness::MachineProfile profile);

// Edits one target value with knob detents / touch steps. Nothing is sent
// until command() is executed after an explicit press (and, for treadmill
// speed, an explicit confirmation). Never auto-start: the initial speed is
// the measured belt speed (0 when unknown), never a remembered target.
class TargetEditor {
public:
    void begin(smartfitness::Control control, const smartfitness::StatusView& status);
    void step(int detents);
    void set(float value);
    smartfitness::Control control() const { return control_; }
    float value() const { return value_; }
    std::string valueText() const;
    // Treadmill speed changes need a second, explicit confirmation.
    bool requiresConfirmation() const { return control_ == smartfitness::Control::Speed; }
    Command command() const;

private:
    smartfitness::Control control_ = smartfitness::Control::Power;
    float value_ = 0.0F;
};

// ---- Status texts -------------------------------------------------------------

Str stateLabel(smartfitness::RuntimeState state);
const char* profileLabel(smartfitness::MachineProfile profile);
const char* ftmsProfileLabel(smartfitness::FtmsOutputProfile profile);
// "KICKR (ftms) ready" / "Simulation" / "No device".
std::string inputLabel(const smartfitness::StatusView& status);
// "App connected" / "Advertising" / "FTMS off".
std::string ftmsLabel(const smartfitness::FtmsOutputView& ftms);
// "Power 180 W" / "No target".
std::string targetLabel(const smartfitness::StatusView& status);
// Two-line scan list entry: name (or address) / "-60 dBm  ftms".
std::string scanEntryLabel(const smartfitness::ScanDeviceView& device);
std::string onOff(bool value);

// ---- Settings -----------------------------------------------------------------

// HMI-only preferences (stored in their own NVS namespace, so the shared v3
// bridge configuration schema is unchanged).
struct HmiSettings {
    std::uint8_t brightness = 80;        // percent, 10..100
    std::uint16_t screenTimeoutS = 120;  // 0: never
    bool ledRing = true;

    bool operator==(const HmiSettings& other) const {
        return brightness == other.brightness && screenTimeoutS == other.screenTimeoutS && ledRing == other.ledRing;
    }
    bool operator!=(const HmiSettings& other) const { return !(*this == other); }
};

constexpr std::uint8_t kSettingsVersion = 1;
std::uint8_t nextBrightness(std::uint8_t percent);
std::uint16_t nextScreenTimeout(std::uint16_t seconds);
std::string screenTimeoutLabel(std::uint16_t seconds);
bool isValid(const HmiSettings& settings);
std::vector<std::uint8_t> serialize(const HmiSettings& settings);
// False (settings unchanged) for missing, corrupt or invalid records.
bool deserialize(const std::vector<std::uint8_t>& data, HmiSettings& settings);

smartfitness::FtmsOutputProfile nextFtmsProfile(smartfitness::FtmsOutputProfile profile);
// FTMS name presets for knob-only entry; the first one is the default name.
const std::vector<std::string>& ftmsNamePresets();

// ---- Commands -------------------------------------------------------------------

Command makeCommand(CommandType type);
Command makeFlag(CommandType type, bool flag);
Command makeText(CommandType type, const std::string& text);
// Pair with the detected adapter (adapterId empty) or an override.
bool makePair(const smartfitness::ScanDeviceView& device, const std::string& adapterId, Command& command);

// Bounded FIFO between LVGL event callbacks and the main loop: callbacks only
// enqueue (never block, never execute), the loop executes.
class CommandQueue {
public:
    static constexpr std::size_t kCapacity = 8;
    bool push(const Command& command);
    bool pop(Command& command);
    std::size_t size() const { return queue_.size(); }
    std::uint32_t dropped() const { return dropped_; }

private:
    std::deque<Command> queue_;
    std::uint32_t dropped_ = 0;
};

// Multi-step confirmation (factory reset: two steps).
class ConfirmSequence {
public:
    explicit ConfirmSequence(int steps) : steps_(steps) {}
    void start() { step_ = 1; }
    void cancel() { step_ = 0; }
    bool active() const { return step_ > 0; }
    int step() const { return step_; }
    // Returns true when the final step has been confirmed.
    bool confirm();

private:
    int steps_;
    int step_ = 0;
};

// ---- LED ring / pacing --------------------------------------------------------

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    bool operator==(const Rgb& other) const { return r == other.r && g == other.g && b == other.b; }
};

// Status colour: red disconnected, amber read-only, blue trainer, green
// control ready; scanning shows blue.
Rgb statusColor(const smartfitness::StatusView& status);

class RefreshTimer {
public:
    explicit RefreshTimer(std::uint32_t intervalMs) : intervalMs_(intervalMs) {}
    bool due(std::uint32_t nowMs);
    void force() { forced_ = true; }

private:
    std::uint32_t intervalMs_;
    std::uint32_t lastMs_ = 0;
    bool started_ = false;
    bool forced_ = false;
};

}  // namespace hmi
