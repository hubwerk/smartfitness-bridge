#pragma once

// Pure C++ decoders for the standard Bluetooth sensor services used as
// read-only inputs: Cycling Power (0x1818), Cycling Speed and Cadence (0x1816)
// and Heart Rate (0x180D).

#include <cstddef>
#include <cstdint>

namespace smartfitness {
namespace sensors {

constexpr std::uint16_t kCyclingPowerService = 0x1818;
constexpr std::uint16_t kCyclingPowerMeasurement = 0x2A63;
constexpr std::uint16_t kCscService = 0x1816;
constexpr std::uint16_t kCscMeasurement = 0x2A5B;
constexpr std::uint16_t kHeartRateService = 0x180D;
constexpr std::uint16_t kHeartRateMeasurement = 0x2A37;

struct RevolutionData {
    bool present = false;
    std::uint32_t revolutions = 0;  // cumulative, wraps at the field width
    std::uint16_t eventTime = 0;    // last event time, wraps at 65536 ticks
};

struct CyclingPowerMeasurement {
    std::int16_t power = 0;
    RevolutionData wheel;  // event time in 1/2048 s
    RevolutionData crank;  // event time in 1/1024 s
};

struct CscMeasurement {
    RevolutionData wheel;  // event time in 1/1024 s
    RevolutionData crank;  // event time in 1/1024 s
};

bool decodeCyclingPower(const std::uint8_t* data, std::size_t length, CyclingPowerMeasurement& out);
bool decodeCsc(const std::uint8_t* data, std::size_t length, CscMeasurement& out);
bool decodeHeartRate(const std::uint8_t* data, std::size_t length, std::uint16_t& bpm);

// Converts cumulative revolution counters and event times into a rate
// (revolutions per minute). Handles counter/timer wraparound, ignores repeated
// events and decays to zero when no new revolution arrives for `idleMs`.
class RevolutionRate {
public:
    // ticksPerSecond: 1024 or 2048. counterBits: 16 (crank) or 32 (wheel).
    RevolutionRate(std::uint32_t ticksPerSecond, std::uint8_t counterBits, std::uint32_t idleMs = 3000);

    // Returns the current rate in rpm after consuming the sample.
    float update(const RevolutionData& data, std::uint32_t nowMs);
    // Decays the rate to zero without a new sample (used by tick()).
    float current(std::uint32_t nowMs);
    void reset();

private:
    std::uint32_t ticksPerSecond_;
    std::uint32_t counterMask_;
    std::uint32_t idleMs_;
    bool hasPrevious_ = false;
    std::uint32_t previousRevolutions_ = 0;
    std::uint16_t previousEventTime_ = 0;
    std::uint32_t lastChangeMs_ = 0;
    float rpm_ = 0.0F;
};

constexpr float kDefaultWheelCircumferenceM = 2.105F;

// Wheel rpm to km/h for the given circumference.
inline float wheelSpeedKmh(float wheelRpm, float circumferenceM = kDefaultWheelCircumferenceM) {
    return wheelRpm * circumferenceM * 60.0F / 1000.0F;
}

}  // namespace sensors
}  // namespace smartfitness
