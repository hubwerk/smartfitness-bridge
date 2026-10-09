#pragma once

#include <array>
#include <cstdint>

namespace smartfitness {

enum class Metric : std::uint8_t {
    Speed,
    Distance,
    Cadence,
    Power,
    HeartRate,
    Incline,
    Count
};

enum class Control : std::uint8_t {
    Speed,
    Incline,
    Resistance,
    Count
};

struct MetricSample {
    float value = 0.0F;
    bool valid = false;
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
    TrainerCapabilities capabilities;
    std::array<MetricSample, static_cast<std::size_t>(Metric::Count)> telemetry{};

    const MetricSample& sample(Metric metric) const;
    void setSample(Metric metric, float value, bool valid = true);
};

struct ControlRequest {
    Control control;
    float value;
};

enum class ControlResult : std::uint8_t {
    Applied,
    Disconnected,
    Unsupported,
    InvalidValue,
    NoAdapter,
    AdapterRejected
};

class TrainerControlAdapter {
public:
    virtual ~TrainerControlAdapter() = default;
    virtual bool apply(const ControlRequest& request) = 0;
};

class BridgeCore {
public:
    explicit BridgeCore(TrainerControlAdapter* adapter = nullptr);

    void setAdapter(TrainerControlAdapter* adapter);
    void setTrainerState(const TrainerState& state);
    const TrainerState& trainerState() const;
    ControlResult routeControl(const ControlRequest& request) const;

private:
    static bool isValidValue(const ControlRequest& request);

    TrainerControlAdapter* adapter_;
    TrainerState trainer_;
};

}  // namespace smartfitness
