#pragma once

#include "ftms_codec.hpp"

namespace smartfitness {
namespace ble {

constexpr std::uint8_t kFtms = 1;
constexpr std::uint8_t kCps = 2;
constexpr std::uint8_t kHr = 4;
constexpr std::uint8_t kCsc = 8;
constexpr std::uint8_t kRsc = 16;
constexpr double kWheelCircumferenceMetres = 2.105;

std::uint8_t effectiveServices(std::uint8_t raw, MachineProfile profile);

// Synthetic events use a fixed virtual wheel, not the trainer's unknown wheel.
// Rates are integrated between updates; invalid/disconnected samples never
// contribute. Event times stop at the last completed revolution, not each notify.
class RevolutionCounter {
public:
    void update(std::uint32_t nowMs, double revolutionsPerSecond, bool valid);
    void invalidate();
    void setRevolutions(std::uint32_t value);
    std::uint32_t revolutions() const { return revolutions_; }
    std::uint16_t eventTime(unsigned ticksPerSecond) const;

private:
    bool initialized_ = false;
    bool valid_ = false;
    std::uint32_t previousMs_ = 0;
    std::uint64_t timelineMs_ = 0;
    std::uint32_t revolutions_ = 0;
    double fraction_ = 0;
    double rate_ = 0;
    double lastEventMs_ = 0;
};

class SensorEncoder {
public:
    void reset() { wheel_ = {}; crank_ = {}; }
    void invalidate() { wheel_.invalidate(); crank_.invalidate(); }
    void update(const TrainerState& state, std::uint32_t nowMs);
    ftms::Bytes cyclingPower(const TrainerState& state) const;
    ftms::Bytes cyclingSpeedCadence(const TrainerState& state) const;
    ftms::Bytes heartRate(const TrainerState& state) const;
    ftms::Bytes runningSpeedCadence(const TrainerState& state) const;
    ftms::Bytes cyclingPowerFeature(const TrainerCapabilities& capabilities, MachineProfile profile) const;
    ftms::Bytes cyclingSpeedCadenceFeature(const TrainerCapabilities& capabilities, MachineProfile profile) const;
    // No distance/reset or calibration control-point functionality is claimed.
    static ftms::Bytes runningFeature() { return {0, 0}; }
    void setWheelRevolutions(std::uint32_t value) { wheel_.setRevolutions(value); }

private:
    RevolutionCounter wheel_;
    RevolutionCounter crank_;
};

}  // namespace ble
}  // namespace smartfitness
