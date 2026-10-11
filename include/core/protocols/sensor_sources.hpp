#pragma once

// Read-only sources for the standard Bluetooth sensor profiles.

#include "protocols/sensor_codec.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {

class SensorSourceBase : public SourceProtocol {
public:
    TrainerCapabilities capabilities() const override;
};

class CyclingPowerSource final : public SensorSourceBase {
public:
    const char* id() const override { return "cps"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    void reset() override;

private:
    sensors::RevolutionRate crank_{1024, 16};
    sensors::RevolutionRate wheel_{2048, 32};
};

class CscSource final : public SensorSourceBase {
public:
    const char* id() const override { return "csc"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    void reset() override;

private:
    sensors::RevolutionRate crank_{1024, 16};
    sensors::RevolutionRate wheel_{1024, 32};
};

class HeartRateSource final : public SensorSourceBase {
public:
    const char* id() const override { return "hrs"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
};

}  // namespace protocols
}  // namespace smartfitness
