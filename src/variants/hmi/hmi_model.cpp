#include "hmi_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ftms_output.h"

using namespace smartfitness;

namespace hmi {
namespace {

constexpr MetricFormat kMetricFormats[] = {
    {Str::MetricSpeed, "km/h", 1, 0.0F, 25.0F},      // Speed
    {Str::MetricDistance, "km", 2, 0.0F, 50.0F},     // Distance (not on the gauge)
    {Str::MetricCadence, "rpm", 0, 0.0F, 150.0F},    // Cadence
    {Str::MetricPower, "W", 0, 0.0F, 500.0F},        // Power
    {Str::MetricHeartRate, "bpm", 0, 40.0F, 200.0F}, // HeartRate
    {Str::MetricIncline, "%", 1, -5.0F, 15.0F},      // Incline
    {Str::MetricElapsed, "s", 0, 0.0F, 3600.0F},     // ElapsedTime (not on the gauge)
    {Str::MetricResistance, "", 0, 0.0F, 100.0F},             // Resistance (device level)
    {Str::MetricAverageSpeed, "km/h", 1, 0.0F, 25.0F},        // AverageSpeed
    {Str::MetricAverageCadence, "rpm", 0, 0.0F, 150.0F},      // AverageCadence
    {Str::MetricAveragePower, "W", 0, 0.0F, 500.0F},          // AveragePower
    {Str::MetricTotalEnergy, "kcal", 0, 0.0F, 1000.0F},       // TotalEnergy
    {Str::MetricEnergyPerHour, "kcal/h", 0, 0.0F, 1500.0F},   // EnergyPerHour
    {Str::MetricEnergyPerMinute, "kcal/min", 0, 0.0F, 25.0F}, // EnergyPerMinute
    {Str::MetricMet, "MET", 1, 0.0F, 20.0F},                  // MetabolicEquivalent
    {Str::MetricRemaining, "s", 0, 0.0F, 3600.0F},            // RemainingTime
    {Str::MetricRampAngle, "deg", 1, -10.0F, 15.0F},          // RampAngle
    {Str::MetricElevationGain, "m", 1, 0.0F, 1000.0F},        // PositiveElevationGain
    {Str::MetricElevationLoss, "m", 1, 0.0F, 1000.0F},        // NegativeElevationGain
    {Str::MetricPace, "s/500m", 0, 0.0F, 300.0F},             // Pace
    {Str::MetricAveragePace, "s/500m", 0, 0.0F, 300.0F},      // AveragePace
    {Str::MetricStrokes, "", 0, 0.0F, 5000.0F},               // StrokeCount
    {Str::MetricSteps, "", 0, 0.0F, 20000.0F},                // StepCount
    {Str::MetricStrides, "", 0, 0.0F, 20000.0F},              // StrideCount
    {Str::MetricFloors, "", 0, 0.0F, 500.0F},                 // Floors
    {Str::MetricBeltForce, "N", 0, -500.0F, 500.0F},          // ForceOnBelt
};
static_assert(sizeof(kMetricFormats) / sizeof(kMetricFormats[0]) == static_cast<std::size_t>(Metric::Count),
              "one format per metric");

constexpr ControlSpec kControlSpecs[] = {
    {Control::Speed, Str::MetricSpeed, "km/h", 0.0F, 25.0F, 0.1F, 1},
    {Control::Incline, Str::MetricIncline, "%", -10.0F, 20.0F, 0.5F, 1},
    {Control::Resistance, Str::MetricResistance, "%", 0.0F, 100.0F, 1.0F, 0},
    {Control::Power, Str::MetricPower, "W", 0.0F, 1000.0F, 5.0F, 0},
};
static_assert(sizeof(kControlSpecs) / sizeof(kControlSpecs[0]) == static_cast<std::size_t>(Control::Count),
              "one spec per control");

constexpr std::uint8_t kBrightnessSteps[] = {10, 25, 50, 80, 100};
constexpr std::uint16_t kTimeoutSteps[] = {0, 30, 60, 120, 300, 600};

std::uint8_t checksum(const std::vector<std::uint8_t>& data, std::size_t length) {
    std::uint8_t sum = 0x5A;
    for (std::size_t index = 0; index < length; ++index) sum = static_cast<std::uint8_t>((sum << 1 | sum >> 7) ^ data[index]);
    return sum;
}

}  // namespace

const MetricFormat& metricFormat(Metric metric) {
    const auto index = static_cast<std::size_t>(metric);
    return kMetricFormats[index < static_cast<std::size_t>(Metric::Count) ? index : 0];
}

std::vector<Metric> dashboardMetrics(MachineProfile profile) {
    switch (profile) {
        case MachineProfile::Treadmill:
            return {Metric::Speed, Metric::Incline, Metric::HeartRate, Metric::Power, Metric::Cadence};
        case MachineProfile::Rower:
            return {Metric::Power, Metric::Cadence, Metric::HeartRate, Metric::Pace, Metric::Speed};
        case MachineProfile::CrossTrainer:
            return {Metric::Power, Metric::Cadence, Metric::HeartRate, Metric::Speed, Metric::Incline};
        case MachineProfile::StepClimber:
        case MachineProfile::StairClimber:
            return {Metric::Cadence, Metric::HeartRate, Metric::Floors, Metric::Power, Metric::TotalEnergy};
        case MachineProfile::IndoorBike:
        default:
            return {Metric::Power, Metric::Cadence, Metric::Speed, Metric::HeartRate, Metric::Incline};
    }
}

std::string formatValue(float value, int decimals) {
    if (!std::isfinite(value)) return "--";
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%.*f", std::max(0, std::min(decimals, 3)), static_cast<double>(value));
    // Avoid "-0" / "-0.0".
    std::string text = buffer;
    if (text[0] == '-' && text.find_first_not_of("-0.") == std::string::npos) text.erase(0, 1);
    return text;
}

std::string formatMetric(const MetricView& sample, Metric metric) {
    if (!sample.valid) return "--";
    return formatValue(sample.value, metricFormat(metric).decimals);
}

Metric MetricCycler::current(MachineProfile profile) const {
    const auto metrics = dashboardMetrics(profile);
    if (std::find(metrics.begin(), metrics.end(), metric_) != metrics.end()) return metric_;
    return metrics.front();
}

void MetricCycler::step(MachineProfile profile, int detents) {
    const auto metrics = dashboardMetrics(profile);
    const Metric now = current(profile);
    const auto count = static_cast<int>(metrics.size());
    int index = static_cast<int>(std::find(metrics.begin(), metrics.end(), now) - metrics.begin());
    index = ((index + detents) % count + count) % count;
    metric_ = metrics[static_cast<std::size_t>(index)];
}

const ControlSpec& controlSpec(Control control) {
    const auto index = static_cast<std::size_t>(control);
    return kControlSpecs[index < static_cast<std::size_t>(Control::Count) ? index : 0];
}

std::vector<Control> controlOrder(MachineProfile profile) {
    if (profile == MachineProfile::Treadmill) return {Control::Speed, Control::Incline, Control::Power, Control::Resistance};
    return {Control::Power, Control::Resistance, Control::Incline, Control::Speed};
}

void TargetEditor::begin(Control control, const StatusView& status) {
    control_ = control;
    float value = control == Control::Power ? 100.0F : control == Control::Resistance ? 20.0F : 0.0F;
    if (control == Control::Speed) {
        // Never auto-start: start from the measured belt speed only.
        const auto& speed = status.metric(Metric::Speed);
        value = speed.valid ? speed.value : 0.0F;
    } else if (status.hasTarget && status.target.control == control) {
        value = status.target.value;
    } else if (control == Control::Power && status.metric(Metric::Power).valid) {
        value = status.metric(Metric::Power).value;
    } else if (control == Control::Incline && status.metric(Metric::Incline).valid) {
        value = status.metric(Metric::Incline).value;
    }
    set(value);
}

void TargetEditor::set(float value) {
    const auto& spec = controlSpec(control_);
    if (!std::isfinite(value)) value = spec.minimum;
    value = std::round(value / spec.step) * spec.step;
    value_ = std::min(spec.maximum, std::max(spec.minimum, value));
    // Normalise binary rounding noise (e.g. 8.100001) to the display precision.
    const float scale = std::pow(10.0F, static_cast<float>(spec.decimals));
    value_ = std::round(value_ * scale) / scale;
    if (value_ == 0.0F) value_ = 0.0F;  // no negative zero
}

void TargetEditor::step(int detents) { set(value_ + static_cast<float>(detents) * controlSpec(control_).step); }

std::string TargetEditor::valueText() const { return formatValue(value_, controlSpec(control_).decimals); }

Command TargetEditor::command() const {
    Command command;
    command.type = CommandType::Control;
    command.control = control_;
    command.value = value_;
    return command;
}

Str stateLabel(RuntimeState state) {
    switch (state) {
        case RuntimeState::Connected: return Str::StateConnected;
        case RuntimeState::Trainer: return Str::StateTrainer;
        case RuntimeState::ControlReady: return Str::StateControlReady;
        case RuntimeState::Disconnected:
        default: return Str::StateDisconnected;
    }
}

const char* profileLabel(MachineProfile profile) {
    switch (profile) {
        case MachineProfile::Treadmill: return "Treadmill";
        case MachineProfile::CrossTrainer: return "Cross trainer";
        case MachineProfile::Rower: return "Rower";
        case MachineProfile::StepClimber: return "Step climber";
        case MachineProfile::StairClimber: return "Stair climber";
        case MachineProfile::IndoorBike:
        default: return "Bike";
    }
}

const char* ftmsProfileLabel(FtmsOutputProfile profile) {
    switch (profile) {
        case FtmsOutputProfile::IndoorBike: return profileLabel(MachineProfile::IndoorBike);
        case FtmsOutputProfile::Treadmill: return profileLabel(MachineProfile::Treadmill);
        case FtmsOutputProfile::CrossTrainer: return profileLabel(MachineProfile::CrossTrainer);
        case FtmsOutputProfile::Rower: return profileLabel(MachineProfile::Rower);
        case FtmsOutputProfile::StepClimber: return profileLabel(MachineProfile::StepClimber);
        case FtmsOutputProfile::StairClimber: return profileLabel(MachineProfile::StairClimber);
        case FtmsOutputProfile::Auto:
        default: return "Auto";
    }
}

std::string inputLabel(const StatusView& status) {
    const auto& input = status.input;
    if (!input.bleSelected) {
        return status.state == RuntimeState::Disconnected ? tr(Str::NoDevice) : tr(Str::Simulation);
    }
    std::string label = input.paired.name.empty() ? input.paired.address : input.paired.name;
    if (!input.paired.adapter.empty()) label += " (" + input.paired.adapter + ")";
    if (!input.link.empty()) label += " " + input.link;
    return label;
}

std::string ftmsLabel(const FtmsOutputView& ftms) {
    if (!ftms.enabled) return tr(Str::FtmsOff);
    if (ftms.client) return tr(Str::FtmsClientConnected);
    if (ftms.advertising) return tr(Str::FtmsAdvertising);
    return tr(Str::FtmsIdle);
}

std::string targetLabel(const StatusView& status) {
    if (!status.hasTarget) return tr(Str::NoTarget);
    const auto& spec = controlSpec(status.target.control);
    return std::string(tr(spec.name)) + " " + formatValue(status.target.value, spec.decimals) + " " + spec.unit;
}

std::string scanEntryLabel(const ScanDeviceView& device) {
    std::string label = device.name.empty() ? device.address : device.name;
    label += "\n" + std::to_string(device.rssi) + " dBm  ";
    label += device.adapter.empty() ? std::string("?") : device.adapter;
    return label;
}

std::string onOff(bool value) { return tr(value ? Str::On : Str::Off); }

std::uint8_t nextBrightness(std::uint8_t percent) {
    for (const auto step : kBrightnessSteps)
        if (step > percent) return step;
    return kBrightnessSteps[0];
}

std::uint16_t nextScreenTimeout(std::uint16_t seconds) {
    for (const auto step : kTimeoutSteps)
        if (step > seconds) return step;
    return kTimeoutSteps[0];
}

std::string screenTimeoutLabel(std::uint16_t seconds) {
    if (seconds == 0) return tr(Str::Never);
    if (seconds % 60 == 0) return std::to_string(seconds / 60) + " min";
    return std::to_string(seconds) + " s";
}

bool isValid(const HmiSettings& settings) {
    return settings.brightness >= 10 && settings.brightness <= 100 && settings.screenTimeoutS <= 3600;
}

std::vector<std::uint8_t> serialize(const HmiSettings& settings) {
    std::vector<std::uint8_t> data{kSettingsVersion, settings.brightness,
                                   static_cast<std::uint8_t>(settings.screenTimeoutS & 0xFF),
                                   static_cast<std::uint8_t>(settings.screenTimeoutS >> 8),
                                   static_cast<std::uint8_t>(settings.ledRing ? 1 : 0)};
    data.push_back(checksum(data, data.size()));
    return data;
}

bool deserialize(const std::vector<std::uint8_t>& data, HmiSettings& settings) {
    if (data.size() != 6 || data[0] != kSettingsVersion || data[4] > 1) return false;
    if (checksum(data, 5) != data[5]) return false;
    HmiSettings parsed;
    parsed.brightness = data[1];
    parsed.screenTimeoutS = static_cast<std::uint16_t>(data[2] | data[3] << 8);
    parsed.ledRing = data[4] != 0;
    if (!isValid(parsed)) return false;
    settings = parsed;
    return true;
}

FtmsOutputProfile nextFtmsProfile(FtmsOutputProfile profile) {
    const auto next = static_cast<std::uint8_t>(profile) + 1;
    return next > static_cast<std::uint8_t>(FtmsOutputProfile::StairClimber) ? FtmsOutputProfile::Auto
                                                                              : static_cast<FtmsOutputProfile>(next);
}

const std::vector<std::string>& ftmsNamePresets() {
    static const std::vector<std::string> presets{FtmsOutput::kDefaultName, "SF Bike", "SF Treadmill",
                                                  "SF Cross Trainer", "SF Rower", "Garage Bike", "Home Treadmill"};
    return presets;
}

Command makeCommand(CommandType type) {
    Command command;
    command.type = type;
    return command;
}

Command makeFlag(CommandType type, bool flag) {
    Command command = makeCommand(type);
    command.flag = flag;
    return command;
}

Command makeText(CommandType type, const std::string& text) {
    Command command = makeCommand(type);
    command.text = text;
    return command;
}

bool makePair(const ScanDeviceView& device, const std::string& adapterId, Command& command) {
    Command pair = makeCommand(CommandType::Pair);
    if (!ble::BleAddress::parse(device.address, pair.address, device.randomAddress ? 1 : 0)) return false;
    pair.addressTypeGiven = true;
    pair.adapterId = adapterId;
    command = pair;
    return true;
}

bool CommandQueue::push(const Command& command) {
    if (queue_.size() >= kCapacity) {
        ++dropped_;
        return false;
    }
    queue_.push_back(command);
    return true;
}

bool CommandQueue::pop(Command& command) {
    if (queue_.empty()) return false;
    command = queue_.front();
    queue_.pop_front();
    return true;
}

bool ConfirmSequence::confirm() {
    if (!active()) return false;
    if (step_ >= steps_) {
        step_ = 0;
        return true;
    }
    ++step_;
    return false;
}

Rgb statusColor(const StatusView& status) {
    if (status.input.scanning) return {0, 40, 160};
    switch (status.state) {
        case RuntimeState::ControlReady: return {0, 160, 40};
        case RuntimeState::Trainer: return {0, 90, 160};
        case RuntimeState::Connected: return {160, 100, 0};
        case RuntimeState::Disconnected:
        default: return {120, 0, 0};
    }
}

bool RefreshTimer::due(std::uint32_t nowMs) {
    if (forced_ || !started_ || nowMs - lastMs_ >= intervalMs_) {
        forced_ = false;
        started_ = true;
        lastMs_ = nowMs;
        return true;
    }
    return false;
}

}  // namespace hmi
