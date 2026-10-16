#pragma once

#include <cstdint>
#include "bridge/bridge_core.hpp"

namespace smartfitness {

constexpr std::uint8_t kOutputServiceAuto = 0;
constexpr std::uint8_t kOutputServiceDefault = 1;  // FTMS / FE-C
constexpr std::uint8_t kOutputServicePower = 2;  // CPS / Bicycle Power
constexpr std::uint8_t kOutputServiceHeartRate = 4;
constexpr std::uint8_t kOutputServiceSpeedCadence = 8;  // CSC / Bicycle Speed and Cadence
constexpr std::uint8_t kOutputServiceRunningSpeedCadence = 16;  // RSC / SDM
constexpr std::uint8_t kOutputServiceAll = 31;

constexpr std::uint8_t effectiveOutputServices(std::uint8_t selection) {
    return selection == kOutputServiceAuto ? kOutputServiceDefault : selection;
}

constexpr bool validOutputServices(std::uint8_t selection, bool ftms = false) {
    return selection <= kOutputServiceAll &&
           (!ftms || selection == kOutputServiceAuto || (selection & kOutputServiceDefault) != 0);
}

inline bool outputMetricAvailable(const TrainerState& state, Metric metric) {
    return state.capabilities.supports(metric) || state.sample(metric).valid;
}

// Capabilities remain useful before the first sample and during stale periods.
// Encoders still require valid samples, so an available service never invents data.
inline std::uint8_t availableOutputServices(const TrainerState& state, MachineProfile profile, bool ant = false) {
    std::uint8_t mask = kOutputServiceDefault;
    if (outputMetricAvailable(state, Metric::Power)) mask |= kOutputServicePower;
    if (outputMetricAvailable(state, Metric::HeartRate)) mask |= kOutputServiceHeartRate;
    if (outputMetricAvailable(state, Metric::Speed) || outputMetricAvailable(state, Metric::Cadence)) {
        if (profile == MachineProfile::IndoorBike && (ant || state.profile == MachineProfile::IndoorBike))
            mask |= kOutputServiceSpeedCadence;
    }
    // RSC requires cadence; ANT SDM has an explicit unavailable-cadence value.
    if (profile == MachineProfile::Treadmill && outputMetricAvailable(state, Metric::Speed) &&
        (ant || (state.profile == MachineProfile::Treadmill && outputMetricAvailable(state, Metric::Cadence))))
        mask |= kOutputServiceRunningSpeedCadence;
    return mask;
}

inline std::uint8_t availableOutputServices(const TrainerState& state) {
    return availableOutputServices(state, state.profile);
}

inline bool outputServicesAvailable(std::uint8_t selection, const TrainerState& state, MachineProfile profile,
                                    bool ant = false) {
    return validOutputServices(selection) &&
           (selection == kOutputServiceAuto || (selection & availableOutputServices(state, profile, ant)) == selection);
}

inline const char* outputServiceId(std::uint8_t bit, bool ant = false) {
    switch (bit) {
        case kOutputServiceDefault: return ant ? "fec" : "ftms";
        case kOutputServicePower: return ant ? "power" : "cps";
        case kOutputServiceHeartRate: return "hr";
        case kOutputServiceSpeedCadence: return ant ? "speed-cadence" : "csc";
        case kOutputServiceRunningSpeedCadence: return ant ? "sdm" : "rsc";
        default: return "unknown";
    }
}

inline const char* outputServiceName(std::uint8_t bit, bool ant = false) {
    switch (bit) {
        case kOutputServiceDefault: return ant ? "Fitness Equipment (FE-C)" : "Fitness Machine (FTMS)";
        case kOutputServicePower: return ant ? "Bicycle Power" : "Cycling Power (CPS)";
        case kOutputServiceHeartRate: return "Heart Rate";
        case kOutputServiceSpeedCadence: return ant ? "Bicycle Speed/Cadence" : "Cycling Speed/Cadence (CSC)";
        case kOutputServiceRunningSpeedCadence: return ant ? "Stride Speed/Distance (SDM)" : "Running Speed/Cadence (RSC)";
        default: return "Unknown";
    }
}

constexpr std::uint8_t antOutputDeviceType(std::uint8_t bit) {
    return bit == kOutputServiceDefault ? 17 : bit == kOutputServicePower ? 11 :
           bit == kOutputServiceHeartRate ? 120 : bit == kOutputServiceSpeedCadence ? 121 :
           bit == kOutputServiceRunningSpeedCadence ? 124 : 0;
}

}  // namespace smartfitness
