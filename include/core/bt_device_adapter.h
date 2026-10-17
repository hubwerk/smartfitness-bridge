#pragma once
#include <functional>
#include "bridge/bridge_core.hpp"
#include "bridge/machine_profile.hpp"

class BtDeviceAdapter : public smartfitness::TrainerControlAdapter {
public:
    virtual ~BtDeviceAdapter() = default;

    virtual void begin() = 0;
    virtual void loop() = 0;

    virtual bool scanAndConnect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual bool trainerDetected() const = 0;
#if defined(ARDUINO) && defined(DEBUG_WEB)
    virtual bool debugStale() const { return false; }
#endif
    virtual smartfitness::MachineProfile profile() const { return smartfitness::MachineProfile::IndoorBike; }

    virtual FitnessTelemetry telemetry() const = 0;
    virtual smartfitness::TrainerCapabilities capabilities() const = 0;

    using TelemetryCallback = std::function<void(const FitnessTelemetry&)>;
    void onTelemetry(TelemetryCallback callback);

protected:
    void emitTelemetry(const FitnessTelemetry& telemetry);

private:
    TelemetryCallback callback_;
};

// Explicit simulation for exercising the runtime; this is not a BLE implementation.
class SimulatedDeviceAdapter final : public BtDeviceAdapter {
public:
    explicit SimulatedDeviceAdapter(bool trainer = true);
    void begin() override;
    void loop() override;
    bool scanAndConnect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool trainerDetected() const override;
    FitnessTelemetry telemetry() const override;
    smartfitness::TrainerCapabilities capabilities() const override;
    bool apply(const smartfitness::ControlRequest& request) override;
    void setTrainer(bool trainer);

private:
    bool trainer_;
    FitnessTelemetry telemetry_;
};
