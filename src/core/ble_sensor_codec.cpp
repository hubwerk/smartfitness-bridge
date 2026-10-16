#include "ftms/sensor_codec.hpp"

#include <algorithm>
#include <cmath>

namespace smartfitness {
namespace ble {
namespace {
using ftms::Bytes;
void u16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}
void u32(Bytes& bytes, std::uint32_t value) {
    u16(bytes, static_cast<std::uint16_t>(value));
    u16(bytes, static_cast<std::uint16_t>(value >> 16));
}
bool usable(const TrainerState& state, Metric metric) {
    return state.connected && state.capabilities.supports(metric) &&
        state.sample(metric).valid && std::isfinite(state.sample(metric).value);
}
bool cycling(MachineProfile profile) { return profile == MachineProfile::IndoorBike; }
bool wheelSupported(const TrainerCapabilities& capabilities, MachineProfile profile) {
    return cycling(profile) && capabilities.supports(Metric::Speed);
}
bool crankSupported(const TrainerCapabilities& capabilities, MachineProfile profile) {
    return cycling(profile) && capabilities.supports(Metric::Cadence);
}
double bounded(double value, double maximum) { return std::max(0.0, std::min(value, maximum)); }
}  // namespace

std::uint8_t effectiveServices(std::uint8_t raw, MachineProfile profile) {
    return static_cast<std::uint8_t>(((raw ? raw : kFtms) | kFtms) &
        (profile == MachineProfile::Treadmill ? 31 : 15));
}

void RevolutionCounter::update(std::uint32_t nowMs, double rate, bool valid) {
    valid = valid && std::isfinite(rate) && rate >= 0 && rate <= 1000;
    if (initialized_) {
        timelineMs_ += static_cast<std::uint32_t>(nowMs - previousMs_);
        // Requiring both endpoints valid prevents bridging a source outage.
        if (valid_ && valid && rate_ > 0) {
            const double total = fraction_ + rate_ * static_cast<std::uint32_t>(nowMs - previousMs_) / 1000.0;
            const auto completed = static_cast<std::uint64_t>(std::floor(total));
            fraction_ = total - completed;
            if (completed) {
                revolutions_ += static_cast<std::uint32_t>(completed);
                lastEventMs_ = static_cast<double>(timelineMs_) - fraction_ * 1000.0 / rate_;
            }
        } else if (!valid) {
            fraction_ = 0;
        }
    } else {
        timelineMs_ = nowMs;
        initialized_ = true;
    }
    previousMs_ = nowMs;
    valid_ = valid;
    rate_ = valid ? rate : 0;
}

void RevolutionCounter::setRevolutions(std::uint32_t value) {
    revolutions_ = value;
    fraction_ = 0;
}

void RevolutionCounter::invalidate() {
    initialized_ = false;
    valid_ = false;
    fraction_ = 0;
    rate_ = 0;
}

std::uint16_t RevolutionCounter::eventTime(unsigned ticksPerSecond) const {
    return static_cast<std::uint16_t>(static_cast<std::uint64_t>(lastEventMs_ * ticksPerSecond / 1000.0) & 0xFFFF);
}

void SensorEncoder::update(const TrainerState& state, std::uint32_t nowMs) {
    wheel_.update(nowMs, bounded(state.sample(Metric::Speed).value / 3.6 / kWheelCircumferenceMetres, 1000.0),
        cycling(state.profile) && usable(state, Metric::Speed) && state.sample(Metric::Speed).value >= 0);
    crank_.update(nowMs, bounded(state.sample(Metric::Cadence).value / 60.0, 1000.0),
        cycling(state.profile) && usable(state, Metric::Cadence) && state.sample(Metric::Cadence).value >= 0);
}

Bytes SensorEncoder::cyclingPowerFeature(const TrainerCapabilities& capabilities, MachineProfile profile) const {
    // Crank data only: no wheel/control-point or calibration capabilities claimed.
    return {static_cast<std::uint8_t>(crankSupported(capabilities, profile) ? 8 : 0), 0, 0, 0};
}

Bytes SensorEncoder::cyclingSpeedCadenceFeature(const TrainerCapabilities& capabilities, MachineProfile profile) const {
    return {static_cast<std::uint8_t>((wheelSupported(capabilities, profile) ? 1 : 0) |
        (crankSupported(capabilities, profile) ? 2 : 0)), 0};
}

Bytes SensorEncoder::cyclingPower(const TrainerState& state) const {
    if (!usable(state, Metric::Power)) return {};
    const bool crank = crankSupported(state.capabilities, state.profile) && usable(state, Metric::Cadence) &&
        state.sample(Metric::Cadence).value >= 0;
    Bytes bytes;
    u16(bytes, crank ? 0x20 : 0);
    const double power = std::max(-32768.0, std::min(32767.0, static_cast<double>(state.sample(Metric::Power).value)));
    u16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(power))));
    if (crank) {
        u16(bytes, static_cast<std::uint16_t>(crank_.revolutions()));
        u16(bytes, crank_.eventTime(1024));
    }
    return bytes;
}

Bytes SensorEncoder::cyclingSpeedCadence(const TrainerState& state) const {
    const bool wheel = wheelSupported(state.capabilities, state.profile) && usable(state, Metric::Speed) &&
        state.sample(Metric::Speed).value >= 0;
    const bool crank = crankSupported(state.capabilities, state.profile) && usable(state, Metric::Cadence) &&
        state.sample(Metric::Cadence).value >= 0;
    if (!wheel && !crank) return {};
    Bytes bytes{static_cast<std::uint8_t>((wheel ? 1 : 0) | (crank ? 2 : 0))};
    if (wheel) {
        u32(bytes, wheel_.revolutions());
        u16(bytes, wheel_.eventTime(1024));
    }
    if (crank) {
        u16(bytes, static_cast<std::uint16_t>(crank_.revolutions()));
        u16(bytes, crank_.eventTime(1024));
    }
    return bytes;
}

Bytes SensorEncoder::heartRate(const TrainerState& state) const {
    if (!usable(state, Metric::HeartRate) || state.sample(Metric::HeartRate).value < 0) return {};
    const auto bpm = static_cast<std::uint16_t>(std::lround(bounded(state.sample(Metric::HeartRate).value, 65535)));
    Bytes bytes{static_cast<std::uint8_t>(bpm > 255 ? 1 : 0)};
    if (bpm > 255) u16(bytes, bpm);
    else bytes.push_back(static_cast<std::uint8_t>(bpm));
    return bytes;
}

Bytes SensorEncoder::runningSpeedCadence(const TrainerState& state) const {
    // Both fields are mandatory in RSC. Do not invent cadence from speed.
    if (state.profile != MachineProfile::Treadmill || !usable(state, Metric::Speed) ||
        !usable(state, Metric::Cadence) || state.sample(Metric::Speed).value < 0 ||
        state.sample(Metric::Cadence).value < 0) return {};
    Bytes bytes{0};  // Running/walking status detection is not supported.
    u16(bytes, static_cast<std::uint16_t>(std::lround(bounded(state.sample(Metric::Speed).value / 3.6 * 256, 65535))));
    bytes.push_back(static_cast<std::uint8_t>(std::lround(bounded(state.sample(Metric::Cadence).value, 255))));
    return bytes;
}

}  // namespace ble
}  // namespace smartfitness
