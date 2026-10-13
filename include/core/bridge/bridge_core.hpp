#pragma once

#include <array>
#include <cstdint>
#include "bridge_types.h"
#include "bridge/metric.hpp"
#include "bridge/machine_profile.hpp"

class BtDeviceAdapter;
class AntOutput;
class FtmsOutput;

namespace smartfitness {

enum class Control : std::uint8_t {
    Speed,
    Incline,
    Resistance,
    Power,
    RunState,  // 0 = stop, 1 = start/resume, 2 = pause
    Count
};

struct TrainerCapabilities {
    std::uint32_t metrics = 0;
    std::uint32_t controls = 0;

    void setMetric(Metric metric, bool supported);
    void setControl(Control control, bool supported);
    bool supports(Metric metric) const;
    bool supports(Control control) const;
};

struct TrainerState {
    bool connected = false;
    bool trainerDetected = false;
    TrainerCapabilities capabilities;
    MachineProfile profile = MachineProfile::IndoorBike;
    std::array<MetricSample, static_cast<std::size_t>(Metric::Count)> telemetry{};

    const MetricSample& sample(Metric metric) const;
    void setSample(Metric metric, float value, bool valid = true);
};

struct ControlBounds {
    float minimum;
    float maximum;
};

// Core safety bounds in normalized units; adapters may enforce tighter limits.
constexpr ControlBounds kSpeedBounds{0.0F, 100.0F};
constexpr ControlBounds kInclineBounds{-20.0F, 25.0F};
constexpr ControlBounds kResistanceBounds{0.0F, 100.0F};
constexpr ControlBounds kPowerBounds{0.0F, 2000.0F};

struct ControlRequest {
    Control control;
    float value;
};

enum class ControlResult : std::uint8_t {
    Applied,
    Disconnected,
    NotTrainer,
    Unsupported,
    InvalidValue,
    NoAdapter,
    AdapterRejected
};

enum class RuntimeState : std::uint8_t {
    Disconnected,
    Connected,
    Trainer,
    ControlReady
};

class TrainerControlAdapter {
public:
    virtual ~TrainerControlAdapter() = default;
    virtual bool apply(const ControlRequest& request) = 0;
};

class BridgeCore {
public:
    explicit BridgeCore(TrainerControlAdapter* adapter = nullptr);
    ~BridgeCore();
    BridgeCore(const BridgeCore&) = delete;
    BridgeCore& operator=(const BridgeCore&) = delete;

    void setAdapter(TrainerControlAdapter* adapter);
    void attach(BtDeviceAdapter& adapter, AntOutput& ant, FtmsOutput& ftms);
    void begin();
    void loop();
    bool scanAndConnect();
    void disconnect();
    void setTrainerState(const TrainerState& state);
    const TrainerState& trainerState() const;
    ControlResult routeControl(const ControlRequest& request);
    void setTelemetry(const FitnessTelemetry& telemetry);
    const FitnessTelemetry& telemetry() const;
    const ControlRequest& controlTarget() const;
    bool hasControlTarget() const;
    bool canControl() const;
    RuntimeState state() const;

private:
    static bool isValidValue(const ControlRequest& request);
    void refreshDeviceState();
    void publish();

    TrainerControlAdapter* adapter_;
    TrainerState trainer_;
    BtDeviceAdapter* device_ = nullptr;
    AntOutput* ant_ = nullptr;
    FtmsOutput* ftms_ = nullptr;
    FitnessTelemetry telemetry_;
    ControlRequest target_{Control::Power, 0.0F};
    bool hasTarget_ = false;
    bool dirty_ = true;
    bool antEnabled_ = false;
    bool ftmsEnabled_ = false;
};

}  // namespace smartfitness
