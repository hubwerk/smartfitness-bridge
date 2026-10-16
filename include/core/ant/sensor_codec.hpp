#pragma once

#include "ant/ant_message.hpp"
#include "bridge/bridge_core.hpp"

namespace smartfitness {
namespace ant {
namespace sensor {

constexpr double kWheelCircumferenceM = 2.105;

// Synthetic event clocks retain their last event while stopped or unavailable.
class Encoder {
public:
    void reset();
    void update(const TrainerState& state, std::uint32_t nowMs);
    Page bicyclePower();
    Page heartRate();
    Page speedCadence() const;
    Page stride() const;

private:
    struct Events {
        double fraction = 0;
        std::uint16_t count = 0;
        std::uint16_t time = 0;
        void advance(double rate, std::uint32_t elapsed, std::uint64_t now);
    };
    TrainerState state_;
    Events crank_, wheel_, beat_, stride_;
    bool started_ = false;
    std::uint32_t lastMs_ = 0;
    std::uint64_t clockMs_ = 0;
    double elapsedSeconds_ = 0;
    double distanceM_ = 0;
    std::uint8_t powerEvents_ = 0;
    std::uint16_t accumulatedPower_ = 0;
    std::uint8_t heartPages_ = 0;
};

}  // namespace sensor
}  // namespace ant
}  // namespace smartfitness
