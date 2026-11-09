#pragma once

// Physical constants shared by the ANT+, FTMS and protocol codecs. Pure C++.

namespace smartfitness {
namespace units {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kMilesToKm = 1.60934F;

// Fixed virtual wheel for synthetic speed/cadence events; the trainer's real
// wheel is unknown. The codecs integrate in double, the protocol decoders in float.
constexpr double kWheelCircumferenceM = 2.105;
constexpr float kWheelCircumferenceMF = static_cast<float>(kWheelCircumferenceM);

}  // namespace units
}  // namespace smartfitness
