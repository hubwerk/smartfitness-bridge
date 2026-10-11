#pragma once

namespace smartfitness {

struct FtmsRoles {
    bool input = false;
    bool output = false;

    constexpr bool enabled() const {
        return input || output;
    }
};

}  // namespace smartfitness
