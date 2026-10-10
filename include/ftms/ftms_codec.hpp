#pragma once

// Pure C++ encoder/decoder for the Bluetooth Fitness Machine Service (FTMS 1.0).
// No Arduino or NimBLE dependencies so it can be exercised by host tests.

#include <cstddef>
#include <cstdint>
#include <vector>
#include "bridge/bridge_core.hpp"

namespace smartfitness {
namespace ftms {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t kServiceUuid = 0x1826;
constexpr std::uint16_t kFeatureUuid = 0x2ACC;
constexpr std::uint16_t kIndoorBikeDataUuid = 0x2AD2;
constexpr std::uint16_t kTrainingStatusUuid = 0x2AD3;
constexpr std::uint16_t kSupportedInclinationRangeUuid = 0x2AD5;
constexpr std::uint16_t kSupportedResistanceRangeUuid = 0x2AD6;
constexpr std::uint16_t kSupportedPowerRangeUuid = 0x2AD8;
constexpr std::uint16_t kControlPointUuid = 0x2AD9;
constexpr std::uint16_t kMachineStatusUuid = 0x2ADA;

// Indoor Bike Data flags (bit 0 "More Data" is inverted: 0 means speed is present).
constexpr std::uint16_t kBikeFlagMoreData = 1U << 0;
constexpr std::uint16_t kBikeFlagCadence = 1U << 2;
constexpr std::uint16_t kBikeFlagPower = 1U << 6;
constexpr std::uint16_t kBikeFlagHeartRate = 1U << 9;

// Fitness Machine Features field.
constexpr std::uint32_t kFeatureCadence = 1U << 1;
constexpr std::uint32_t kFeatureInclination = 1U << 3;
constexpr std::uint32_t kFeatureHeartRate = 1U << 10;
constexpr std::uint32_t kFeaturePower = 1U << 14;
// Target Setting Features field.
constexpr std::uint32_t kTargetInclination = 1U << 1;
constexpr std::uint32_t kTargetResistance = 1U << 2;
constexpr std::uint32_t kTargetPower = 1U << 3;
constexpr std::uint32_t kTargetIndoorBikeSimulation = 1U << 13;

enum class OpCode : std::uint8_t {
    RequestControl = 0x00,
    Reset = 0x01,
    SetTargetInclination = 0x03,
    SetTargetResistance = 0x04,
    SetTargetPower = 0x05,
    StartOrResume = 0x07,
    StopOrPause = 0x08,
    SetIndoorBikeSimulation = 0x11,
    ResponseCode = 0x80
};

enum class ResultCode : std::uint8_t {
    Success = 0x01,
    NotSupported = 0x02,
    InvalidParameter = 0x03,
    OperationFailed = 0x04,
    ControlNotPermitted = 0x05
};

enum class StatusCode : std::uint8_t {
    Reset = 0x01,
    StoppedOrPaused = 0x02,
    StartedOrResumed = 0x04,
    TargetInclineChanged = 0x06,
    TargetResistanceChanged = 0x07,
    TargetPowerChanged = 0x08,
    IndoorBikeSimulationChanged = 0x12
};

enum class TrainingStatus : std::uint8_t {
    Other = 0x00,
    Idle = 0x01,
    ManualMode = 0x0D
};

struct IndoorBikeData {
    MetricSample speed;      // km/h
    MetricSample cadence;    // rpm
    MetricSample power;      // W
    MetricSample heartRate;  // bpm
};

// Uses only samples the core marked valid (connected and supported by the device).
IndoorBikeData indoorBikeDataFrom(const TrainerState& state);
Bytes encodeIndoorBikeData(const IndoorBikeData& data);
Bytes encodeFeature(const TrainerCapabilities& capabilities);
Bytes encodeSupportedInclinationRange();
Bytes encodeSupportedResistanceRange();
Bytes encodeSupportedPowerRange();
Bytes encodeTrainingStatus(TrainingStatus status);
// FTMS advertising Service Data payload: machine available, indoor bike supported.
Bytes encodeAdvertisingServiceData();

struct ControlPointCommand {
    std::uint8_t opcode = 0;
    // Success when the opcode is known and its parameters are well-formed.
    ResultCode status = ResultCode::NotSupported;
    // True for opcodes that translate to a core control request.
    bool hasRequest = false;
    ControlRequest request{Control::Power, 0.0F};
    // Normalized parameter bytes echoed in the Fitness Machine Status notification.
    Bytes statusParameters;
};

// Returns false for an empty write, which carries no opcode to respond to.
bool decodeControlPoint(const std::uint8_t* data, std::size_t length, ControlPointCommand& command);
Bytes encodeControlPointResponse(std::uint8_t opcode, ResultCode result);
// Status notification for an acknowledged command; empty when the opcode has none.
Bytes encodeMachineStatus(const ControlPointCommand& command);
ResultCode resultCodeFor(ControlResult result);

// ---------------------------------------------------------------------------
// Client (input) side: decoding data from an FTMS machine and encoding the
// Control Point commands sent to it.
// ---------------------------------------------------------------------------

constexpr std::uint16_t kTreadmillDataUuid = 0x2ACD;
constexpr std::uint16_t kCrossTrainerDataUuid = 0x2ACE;
constexpr std::uint16_t kRowerDataUuid = 0x2AD1;

// Additional Fitness Machine Features / Target Setting Features bits.
constexpr std::uint32_t kFeatureResistanceLevel = 1U << 7;
constexpr std::uint32_t kFeaturePowerAndForce = 1U << 15;
constexpr std::uint32_t kTargetSpeed = 1U << 0;

enum class MachineType : std::uint8_t { IndoorBike, Treadmill, CrossTrainer, Rower };

// Normalized sample decoded from one data notification. Only fields whose
// flag was present are marked valid.
struct MachineData {
    MetricSample speed;       // km/h
    MetricSample cadence;     // rpm (steps/min for cross trainers, strokes/min for rowers)
    MetricSample power;       // W
    MetricSample heartRate;   // bpm
    MetricSample incline;     // %
    MetricSample resistance;  // device resistance level (unitless, as sent)
    MetricSample distance;    // m
};

// Return false when the payload is truncated for the flags it announces.
bool decodeIndoorBikeData(const std::uint8_t* data, std::size_t length, MachineData& out);
bool decodeTreadmillData(const std::uint8_t* data, std::size_t length, MachineData& out);
bool decodeCrossTrainerData(const std::uint8_t* data, std::size_t length, MachineData& out);
bool decodeRowerData(const std::uint8_t* data, std::size_t length, MachineData& out);

struct Features {
    std::uint32_t machine = 0;
    std::uint32_t targets = 0;
};
bool decodeFeature(const std::uint8_t* data, std::size_t length, Features& out);

struct SupportedRange {
    float minimum = 0.0F;
    float maximum = 0.0F;
    float increment = 0.0F;
    bool valid = false;
};
// Supported Inclination/Resistance ranges use 0.1 resolution, power uses 1 W.
bool decodeSupportedRange(const std::uint8_t* data, std::size_t length, float resolution, SupportedRange& out);

struct ControlPointResponse {
    std::uint8_t requestOpcode = 0;
    ResultCode result = ResultCode::OperationFailed;
};
// Decodes an indication "0x80, request opcode, result".
bool decodeControlPointResponse(const std::uint8_t* data, std::size_t length, ControlPointResponse& out);

Bytes encodeRequestControl();
Bytes encodeStartOrResume();
Bytes encodeSetTargetPower(float watts);
// Resistance level in device units with 0.1 resolution, sent as SINT16 like
// most trainers (and qdomyos-zwift) expect.
Bytes encodeSetTargetResistance(float level);
Bytes encodeSetTargetInclination(float percent);
Bytes encodeSetTargetSpeed(float kmh);
// Grade in %, wind speed in m/s, rolling resistance coefficient and wind
// resistance coefficient (kg/m) are quantized per the FTMS resolutions.
Bytes encodeIndoorBikeSimulation(float gradePercent, float windSpeed = 0.0F, float crr = 0.004F, float cw = 0.51F);

}  // namespace ftms
}  // namespace smartfitness
