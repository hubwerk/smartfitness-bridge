#pragma once

// GATT layout of the BLE output services (FtmsOutput). Shared by the NimBLE
// transport and the views that list what the bridge sends. Pure C++.

#include <cstdint>
#include <vector>
#include "bridge/output_services.hpp"
#include "ftms/ftms_codec.hpp"

namespace smartfitness {

namespace gatt {
constexpr std::uint16_t kCyclingPowerService = 0x1818;
constexpr std::uint16_t kCyclingPowerMeasurement = 0x2A63;
constexpr std::uint16_t kCyclingPowerFeature = 0x2A65;
constexpr std::uint16_t kHeartRateService = 0x180D;
constexpr std::uint16_t kHeartRateMeasurement = 0x2A37;
constexpr std::uint16_t kCyclingSpeedCadenceService = 0x1816;
constexpr std::uint16_t kCscMeasurement = 0x2A5B;
constexpr std::uint16_t kCscFeature = 0x2A5C;
constexpr std::uint16_t kRunningSpeedCadenceService = 0x1814;
constexpr std::uint16_t kRscMeasurement = 0x2A53;
constexpr std::uint16_t kRscFeature = 0x2A54;
constexpr std::uint16_t kSensorLocation = 0x2A5D;
constexpr std::uint16_t kScControlPoint = 0x2A55;
}  // namespace gatt

struct OutputCharacteristic {
    std::uint16_t uuid = 0;
    const char* properties = "";  // "read", "notify", "read/notify", "write/indicate"
};

// 16-bit service UUID of a BLE output service bit (0 when unknown).
constexpr std::uint16_t outputServiceUuid(std::uint8_t bit) {
    return bit == kOutputServiceDefault ? ftms::kServiceUuid :
           bit == kOutputServicePower ? gatt::kCyclingPowerService :
           bit == kOutputServiceHeartRate ? gatt::kHeartRateService :
           bit == kOutputServiceSpeedCadence ? gatt::kCyclingSpeedCadenceService :
           bit == kOutputServiceRunningSpeedCadence ? gatt::kRunningSpeedCadenceService : 0;
}

// Characteristics the bridge serves for an output service. For FTMS only the
// data characteristic of the effective machine profile carries notifications.
inline std::vector<OutputCharacteristic> outputCharacteristics(std::uint8_t bit, MachineProfile profile) {
    switch (bit) {
        case kOutputServiceDefault:
            return {{ftms::kFeatureUuid, "read"},
                    {ftms::machineDataUuid(profile), "notify"},
                    {ftms::kTrainingStatusUuid, "read/notify"},
                    {ftms::kSupportedInclinationRangeUuid, "read"},
                    {ftms::kSupportedSpeedRangeUuid, "read"},
                    {ftms::kSupportedResistanceRangeUuid, "read"},
                    {ftms::kSupportedPowerRangeUuid, "read"},
                    {ftms::kControlPointUuid, "write/indicate"},
                    {ftms::kMachineStatusUuid, "notify"}};
        case kOutputServicePower:
            return {{gatt::kCyclingPowerMeasurement, "notify"},
                    {gatt::kCyclingPowerFeature, "read"},
                    {gatt::kSensorLocation, "read"}};
        case kOutputServiceHeartRate:
            return {{gatt::kHeartRateMeasurement, "notify"}};
        case kOutputServiceSpeedCadence:
            return {{gatt::kCscMeasurement, "notify"},
                    {gatt::kCscFeature, "read"},
                    {gatt::kSensorLocation, "read"},
                    {gatt::kScControlPoint, "write/indicate"}};
        case kOutputServiceRunningSpeedCadence:
            return {{gatt::kRscMeasurement, "notify"},
                    {gatt::kRscFeature, "read"}};
        default:
            return {};
    }
}

}  // namespace smartfitness
