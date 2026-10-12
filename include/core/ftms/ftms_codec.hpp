#pragma once

// Pure C++ encoder/decoder for the Bluetooth Fitness Machine Service (FTMS 1.0).
// No Arduino or NimBLE dependencies so it can be exercised by host tests.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "bridge/bridge_core.hpp"

namespace smartfitness {
namespace ftms {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t kServiceUuid = 0x1826;
constexpr std::uint16_t kFeatureUuid = 0x2ACC;
constexpr std::uint16_t kIndoorBikeDataUuid = 0x2AD2;
constexpr std::uint16_t kTrainingStatusUuid = 0x2AD3;
constexpr std::uint16_t kSupportedSpeedRangeUuid = 0x2AD4;
constexpr std::uint16_t kSupportedInclinationRangeUuid = 0x2AD5;
constexpr std::uint16_t kSupportedResistanceRangeUuid = 0x2AD6;
constexpr std::uint16_t kSupportedPowerRangeUuid = 0x2AD8;
constexpr std::uint16_t kControlPointUuid = 0x2AD9;
constexpr std::uint16_t kMachineStatusUuid = 0x2ADA;

constexpr std::uint16_t kTreadmillDataUuid = 0x2ACD;
constexpr std::uint16_t kCrossTrainerDataUuid = 0x2ACE;
constexpr std::uint16_t kStepClimberDataUuid = 0x2ACF;
constexpr std::uint16_t kStairClimberDataUuid = 0x2AD0;
constexpr std::uint16_t kRowerDataUuid = 0x2AD1;

// Indoor Bike Data flags (bit 0 "More Data" is inverted: 0 means speed is present).
constexpr std::uint16_t kBikeFlagMoreData = 1U << 0;
constexpr std::uint16_t kBikeFlagCadence = 1U << 2;
constexpr std::uint16_t kBikeFlagPower = 1U << 6;
constexpr std::uint16_t kBikeFlagHeartRate = 1U << 9;

// Fitness Machine Features field.
constexpr std::uint32_t kFeatureAverageSpeed = 1U << 0;
constexpr std::uint32_t kFeatureCadence = 1U << 1;
constexpr std::uint32_t kFeatureTotalDistance = 1U << 2;
constexpr std::uint32_t kFeatureInclination = 1U << 3;
constexpr std::uint32_t kFeatureElevationGain = 1U << 4;
constexpr std::uint32_t kFeaturePace = 1U << 5;
constexpr std::uint32_t kFeatureStepCount = 1U << 6;
constexpr std::uint32_t kFeatureResistanceLevel = 1U << 7;
constexpr std::uint32_t kFeatureStrideCount = 1U << 8;
constexpr std::uint32_t kFeatureExpendedEnergy = 1U << 9;
constexpr std::uint32_t kFeatureHeartRate = 1U << 10;
constexpr std::uint32_t kFeatureMetabolicEquivalent = 1U << 11;
constexpr std::uint32_t kFeatureElapsedTime = 1U << 12;
constexpr std::uint32_t kFeatureRemainingTime = 1U << 13;
constexpr std::uint32_t kFeaturePower = 1U << 14;
constexpr std::uint32_t kFeaturePowerAndForce = 1U << 15;
// Target Setting Features field.
constexpr std::uint32_t kTargetSpeed = 1U << 0;
constexpr std::uint32_t kTargetInclination = 1U << 1;
constexpr std::uint32_t kTargetResistance = 1U << 2;
constexpr std::uint32_t kTargetPower = 1U << 3;
constexpr std::uint32_t kTargetIndoorBikeSimulation = 1U << 13;

// Expended Energy sub-fields use these values for "data not available".
constexpr std::uint16_t kEnergyNotAvailable16 = 0xFFFF;
constexpr std::uint8_t kEnergyNotAvailable8 = 0xFF;

enum class OpCode : std::uint8_t {
    RequestControl = 0x00,
    Reset = 0x01,
    SetTargetSpeed = 0x02,
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
    TargetSpeedChanged = 0x05,
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

// ---------------------------------------------------------------------------
// Machine data characteristics. Every FTMS field of each profile is modelled
// with its native wire type and resolution; a field is std::nullopt when the
// device did not send it (flag clear, or the FTMS "not available" value) so
// it is never mistaken for 0.
//
// Some FTMS fields share one flag. When encoding, a group is sent if any of
// its members is present; a missing companion is sent as the "not available"
// value where FTMS defines one (expended energy) and as 0 otherwise.
// ---------------------------------------------------------------------------

// 0x2AD2, 16-bit flags.
struct IndoorBikeData {
    std::optional<std::uint16_t> instantaneousSpeed;    // 0.01 km/h (flag bit 0 inverted)
    std::optional<std::uint16_t> averageSpeed;          // 0.01 km/h
    std::optional<std::uint16_t> instantaneousCadence;  // 0.5 rpm
    std::optional<std::uint16_t> averageCadence;        // 0.5 rpm
    std::optional<std::uint32_t> totalDistance;         // m, UINT24
    std::optional<std::int16_t> resistanceLevel;        // unitless, 1
    std::optional<std::int16_t> instantaneousPower;     // W
    std::optional<std::int16_t> averagePower;           // W
    std::optional<std::uint16_t> totalEnergy;           // kcal
    std::optional<std::uint16_t> energyPerHour;         // kcal
    std::optional<std::uint8_t> energyPerMinute;        // kcal
    std::optional<std::uint8_t> heartRate;              // bpm
    std::optional<std::uint8_t> metabolicEquivalent;    // 0.1 MET
    std::optional<std::uint16_t> elapsedTime;           // s
    std::optional<std::uint16_t> remainingTime;         // s
};

// 0x2ACD, 16-bit flags.
struct TreadmillData {
    std::optional<std::uint16_t> instantaneousSpeed;     // 0.01 km/h (flag bit 0 inverted)
    std::optional<std::uint16_t> averageSpeed;           // 0.01 km/h
    std::optional<std::uint32_t> totalDistance;          // m, UINT24
    std::optional<std::int16_t> inclination;             // 0.1 %   } one flag
    std::optional<std::int16_t> rampAngleSetting;        // 0.1 deg }
    std::optional<std::uint16_t> positiveElevationGain;  // 0.1 m } one flag
    std::optional<std::uint16_t> negativeElevationGain;  // 0.1 m }
    std::optional<std::uint8_t> instantaneousPace;       // 0.1 km/min
    std::optional<std::uint8_t> averagePace;             // 0.1 km/min
    std::optional<std::uint16_t> totalEnergy;            // kcal
    std::optional<std::uint16_t> energyPerHour;          // kcal
    std::optional<std::uint8_t> energyPerMinute;         // kcal
    std::optional<std::uint8_t> heartRate;               // bpm
    std::optional<std::uint8_t> metabolicEquivalent;     // 0.1 MET
    std::optional<std::uint16_t> elapsedTime;            // s
    std::optional<std::uint16_t> remainingTime;          // s
    std::optional<std::int16_t> forceOnBelt;             // N } one flag
    std::optional<std::int16_t> powerOutput;             // W }
};

// 0x2ACE, 24-bit flags.
struct CrossTrainerData {
    std::optional<std::uint16_t> instantaneousSpeed;     // 0.01 km/h (flag bit 0 inverted)
    std::optional<std::uint16_t> averageSpeed;           // 0.01 km/h
    std::optional<std::uint32_t> totalDistance;          // m, UINT24
    std::optional<std::uint16_t> stepPerMinute;          // 1/min } one flag
    std::optional<std::uint16_t> averageStepRate;        // 1/min }
    std::optional<std::uint16_t> strideCount;            // 0.1
    std::optional<std::uint16_t> positiveElevationGain;  // m } one flag
    std::optional<std::uint16_t> negativeElevationGain;  // m }
    std::optional<std::int16_t> inclination;             // 0.1 %   } one flag
    std::optional<std::int16_t> rampAngleSetting;        // 0.1 deg }
    std::optional<std::int16_t> resistanceLevel;         // 0.1
    std::optional<std::int16_t> instantaneousPower;      // W
    std::optional<std::int16_t> averagePower;            // W
    std::optional<std::uint16_t> totalEnergy;            // kcal
    std::optional<std::uint16_t> energyPerHour;          // kcal
    std::optional<std::uint8_t> energyPerMinute;         // kcal
    std::optional<std::uint8_t> heartRate;               // bpm
    std::optional<std::uint8_t> metabolicEquivalent;     // 0.1 MET
    std::optional<std::uint16_t> elapsedTime;            // s
    std::optional<std::uint16_t> remainingTime;          // s
    bool movementBackward = false;                       // flag bit 15 (no field)
};

// 0x2ACF, 16-bit flags.
struct StepClimberData {
    std::optional<std::uint16_t> floors;                 // } flag bit 0 inverted
    std::optional<std::uint16_t> stepCount;              // }
    std::optional<std::uint16_t> stepPerMinute;          // 1/min
    std::optional<std::uint16_t> averageStepRate;        // 1/min
    std::optional<std::uint16_t> positiveElevationGain;  // m
    std::optional<std::uint16_t> totalEnergy;            // kcal
    std::optional<std::uint16_t> energyPerHour;          // kcal
    std::optional<std::uint8_t> energyPerMinute;         // kcal
    std::optional<std::uint8_t> heartRate;               // bpm
    std::optional<std::uint8_t> metabolicEquivalent;     // 0.1 MET
    std::optional<std::uint16_t> elapsedTime;            // s
    std::optional<std::uint16_t> remainingTime;          // s
};

// 0x2AD0, 16-bit flags.
struct StairClimberData {
    std::optional<std::uint16_t> floors;                 // flag bit 0 inverted
    std::optional<std::uint16_t> stepPerMinute;          // 1/min
    std::optional<std::uint16_t> averageStepRate;        // 1/min
    std::optional<std::uint16_t> positiveElevationGain;  // m
    std::optional<std::uint16_t> strideCount;            // 1
    std::optional<std::uint16_t> totalEnergy;            // kcal
    std::optional<std::uint16_t> energyPerHour;          // kcal
    std::optional<std::uint8_t> energyPerMinute;         // kcal
    std::optional<std::uint8_t> heartRate;               // bpm
    std::optional<std::uint8_t> metabolicEquivalent;     // 0.1 MET
    std::optional<std::uint16_t> elapsedTime;            // s
    std::optional<std::uint16_t> remainingTime;          // s
};

// 0x2AD1, 16-bit flags.
struct RowerData {
    std::optional<std::uint8_t> strokeRate;            // 0.5 /min } flag bit 0 inverted
    std::optional<std::uint16_t> strokeCount;          // 1        }
    std::optional<std::uint8_t> averageStrokeRate;     // 0.5 /min
    std::optional<std::uint32_t> totalDistance;        // m, UINT24
    std::optional<std::uint16_t> instantaneousPace;    // s per 500 m
    std::optional<std::uint16_t> averagePace;          // s per 500 m
    std::optional<std::int16_t> instantaneousPower;    // W
    std::optional<std::int16_t> averagePower;          // W
    std::optional<std::int16_t> resistanceLevel;       // unitless, 1
    std::optional<std::uint16_t> totalEnergy;          // kcal
    std::optional<std::uint16_t> energyPerHour;        // kcal
    std::optional<std::uint8_t> energyPerMinute;       // kcal
    std::optional<std::uint8_t> heartRate;             // bpm
    std::optional<std::uint8_t> metabolicEquivalent;   // 0.1 MET
    std::optional<std::uint16_t> elapsedTime;          // s
    std::optional<std::uint16_t> remainingTime;        // s
};

Bytes encodeIndoorBikeData(const IndoorBikeData& data);
Bytes encodeTreadmillData(const TreadmillData& data);
Bytes encodeCrossTrainerData(const CrossTrainerData& data);
Bytes encodeStepClimberData(const StepClimberData& data);
Bytes encodeStairClimberData(const StairClimberData& data);
Bytes encodeRowerData(const RowerData& data);

// Return false (and leave out empty) when the payload is truncated for the
// flags it announces.
bool decodeIndoorBikeData(const std::uint8_t* data, std::size_t length, IndoorBikeData& out);
bool decodeTreadmillData(const std::uint8_t* data, std::size_t length, TreadmillData& out);
bool decodeCrossTrainerData(const std::uint8_t* data, std::size_t length, CrossTrainerData& out);
bool decodeStepClimberData(const std::uint8_t* data, std::size_t length, StepClimberData& out);
bool decodeStairClimberData(const std::uint8_t* data, std::size_t length, StairClimberData& out);
bool decodeRowerData(const std::uint8_t* data, std::size_t length, RowerData& out);

// Output mapping: core metrics (normalized units, see bridge/metric.hpp) to
// the profile's fields. Only samples the core marked valid are used; derived
// companions are filled where FTMS needs them (ramp angle from inclination,
// rower pace from speed).
IndoorBikeData indoorBikeDataFrom(const TrainerState& state);
TreadmillData treadmillDataFrom(const TrainerState& state);
CrossTrainerData crossTrainerDataFrom(const TrainerState& state);
StepClimberData stepClimberDataFrom(const TrainerState& state);
StairClimberData stairClimberDataFrom(const TrainerState& state);
RowerData rowerDataFrom(const TrainerState& state);

// Data characteristic UUID used by a machine profile.
std::uint16_t machineDataUuid(MachineProfile profile);
// Data notification for the profile, built from the core state.
Bytes encodeMachineData(MachineProfile profile, const TrainerState& state);

// True when FTMS lets the profile accept the control (independent of the
// source's capabilities, which the core still enforces).
bool profileAcceptsControl(MachineProfile profile, Control control);
Bytes encodeFeature(const TrainerCapabilities& capabilities, MachineProfile profile = MachineProfile::IndoorBike);
Bytes encodeSupportedSpeedRange();
Bytes encodeSupportedInclinationRange();
Bytes encodeSupportedResistanceRange();
Bytes encodeSupportedPowerRange();
Bytes encodeTrainingStatus(TrainingStatus status);
// FTMS advertising Service Data payload: machine available and selected type.
Bytes encodeAdvertisingServiceData(MachineProfile profile = MachineProfile::IndoorBike);

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
bool decodeControlPoint(const std::uint8_t* data, std::size_t length, ControlPointCommand& command,
                        MachineProfile profile = MachineProfile::IndoorBike);
Bytes encodeControlPointResponse(std::uint8_t opcode, ResultCode result);
// Status notification for an acknowledged command; empty when the opcode has none.
Bytes encodeMachineStatus(const ControlPointCommand& command);
ResultCode resultCodeFor(ControlResult result);

// ---------------------------------------------------------------------------
// Client (input) side: decoding data from an FTMS machine and encoding the
// Control Point commands sent to it.
// ---------------------------------------------------------------------------

using MachineType = MachineProfile;

// Input mapping: one data notification decoded into normalized core metrics.
// Only fields present in the notification are marked valid.
using MachineData = MetricSet;

MachineData toMachineData(const IndoorBikeData& data);
MachineData toMachineData(const TreadmillData& data);
MachineData toMachineData(const CrossTrainerData& data);
MachineData toMachineData(const StepClimberData& data);
MachineData toMachineData(const StairClimberData& data);
MachineData toMachineData(const RowerData& data);

// Decode + map in one step. Return false when the payload is truncated.
bool decodeIndoorBikeData(const std::uint8_t* data, std::size_t length, MachineData& out);
bool decodeTreadmillData(const std::uint8_t* data, std::size_t length, MachineData& out);
bool decodeCrossTrainerData(const std::uint8_t* data, std::size_t length, MachineData& out);
bool decodeStepClimberData(const std::uint8_t* data, std::size_t length, MachineData& out);
bool decodeStairClimberData(const std::uint8_t* data, std::size_t length, MachineData& out);
bool decodeRowerData(const std::uint8_t* data, std::size_t length, MachineData& out);
bool decodeMachineData(MachineProfile profile, const std::uint8_t* data, std::size_t length, MachineData& out);

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
