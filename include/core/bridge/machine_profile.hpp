#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace smartfitness {

// Values are persisted (FtmsOutputProfile) and index tables: append only.
enum class MachineProfile : std::uint8_t { IndoorBike, Treadmill, CrossTrainer, Rower, StepClimber, StairClimber };
enum class FtmsOutputProfile : std::uint8_t { Auto, IndoorBike, Treadmill, CrossTrainer, Rower, StepClimber, StairClimber };
constexpr std::size_t kMachineProfileCount = 6;

inline const char* machineProfileName(MachineProfile profile) {
    switch (profile) {
        case MachineProfile::Treadmill: return "treadmill";
        case MachineProfile::CrossTrainer: return "cross-trainer";
        case MachineProfile::Rower: return "rower";
        case MachineProfile::StepClimber: return "step-climber";
        case MachineProfile::StairClimber: return "stair-climber";
        default: return "bike";
    }
}

inline const char* ftmsOutputProfileName(FtmsOutputProfile profile) {
    if (profile == FtmsOutputProfile::Auto) return "auto";
    return machineProfileName(static_cast<MachineProfile>(static_cast<std::uint8_t>(profile) - 1));
}

inline bool parseFtmsOutputProfile(const std::string& value, FtmsOutputProfile& profile) {
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(FtmsOutputProfile::StairClimber); ++i) {
        const auto candidate = static_cast<FtmsOutputProfile>(i);
        if (value == ftmsOutputProfileName(candidate)) {
            profile = candidate;
            return true;
        }
    }
    return false;
}

inline MachineProfile selectedMachineProfile(FtmsOutputProfile overrideProfile, MachineProfile input) {
    return overrideProfile == FtmsOutputProfile::Auto
        ? input : static_cast<MachineProfile>(static_cast<std::uint8_t>(overrideProfile) - 1);
}

// Every FTMS machine type has a native output encoding, so the effective
// output profile is the selected one.
inline MachineProfile effectiveOutputProfile(FtmsOutputProfile overrideProfile, MachineProfile input) {
    return selectedMachineProfile(overrideProfile, input);
}

}  // namespace smartfitness
