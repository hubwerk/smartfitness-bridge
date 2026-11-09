#pragma once

// Bluetooth SIG assigned 16-bit UUIDs shared by the BLE output services and
// the read-only sensor inputs. Pure C++.

#include <cstdint>

namespace smartfitness {
namespace ble {
namespace sig {

constexpr std::uint16_t kCyclingPowerService = 0x1818;
constexpr std::uint16_t kCyclingPowerMeasurement = 0x2A63;
constexpr std::uint16_t kHeartRateService = 0x180D;
constexpr std::uint16_t kHeartRateMeasurement = 0x2A37;
constexpr std::uint16_t kCyclingSpeedCadenceService = 0x1816;
constexpr std::uint16_t kCscMeasurement = 0x2A5B;

}  // namespace sig
}  // namespace ble
}  // namespace smartfitness
