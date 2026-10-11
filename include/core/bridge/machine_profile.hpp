#pragma once

#include <cstdint>
#include <string>

namespace smartfitness {

enum class MachineProfile : std::uint8_t { IndoorBike, Treadmill, CrossTrainer, Rower };
enum class FtmsOutputProfile : std::uint8_t { Auto, IndoorBike, Treadmill, CrossTrainer, Rower };

inline const char* machineProfileName(MachineProfile profile) {
    switch (profile) {
        case MachineProfile::Treadmill: return "treadmill";
        case MachineProfile::CrossTrainer: return "cross-trainer";
        case MachineProfile::Rower: return "rower";
        default: return "bike";
    }
}

inline const char* ftmsOutputProfileName(FtmsOutputProfile profile) {
    if (profile == FtmsOutputProfile::Auto) return "auto";
    return machineProfileName(static_cast<MachineProfile>(static_cast<std::uint8_t>(profile) - 1));
}

inline bool parseFtmsOutputProfile(const std::string& value, FtmsOutputProfile& profile) {
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(FtmsOutputProfile::Rower); ++i) {
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

// Cross Trainer and Rower encoding are follow-up work; preserve the bike output.
inline MachineProfile effectiveOutputProfile(FtmsOutputProfile overrideProfile, MachineProfile input) {
    const auto selected = selectedMachineProfile(overrideProfile, input);
    return selected == MachineProfile::Treadmill ? selected : MachineProfile::IndoorBike;
}

}  // namespace smartfitness
