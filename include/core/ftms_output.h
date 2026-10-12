#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "bridge_output.h"
#include "ftms/ftms_codec.hpp"

// Bluetooth LE FTMS peripheral output. Protocol handling is portable C++;
// the NimBLE GATT transport is only compiled for the firmware (ARDUINO).
class FtmsOutput : public BridgeOutput {
public:
    static constexpr const char* kDefaultName = "SmartFitness Bridge";
    static constexpr std::size_t kMaxNameLength = 20;
    static constexpr std::size_t kQueueCapacity = 8;
    static constexpr std::size_t kMaxWriteLength = 20;
    static constexpr std::uint32_t kBikeDataMinIntervalMs = 250;
    static constexpr std::uint32_t kBikeDataMaxIntervalMs = 1000;

    FtmsOutput();
    ~FtmsOutput() override;
    FtmsOutput(const FtmsOutput&) = delete;
    FtmsOutput& operator=(const FtmsOutput&) = delete;

    void begin() override;
    void loop() override;
    void setEnabled(bool enabled) override;
    void publishTrainerState(const smartfitness::TrainerState& state) override;

    // Advertised (pairing) name; longer names are truncated to kMaxNameLength.
    void setDeviceName(const std::string& name);
    const std::string& deviceName() const;
    void setProfileOverride(smartfitness::FtmsOutputProfile profile);
    smartfitness::MachineProfile profile() const;

    // Safe to call from the BLE host task: events are queued and drained in loop().
    bool enqueueControlPoint(const std::uint8_t* data, std::size_t length);
    void notifyClientConnected();
    void notifyClientDisconnected();
    // Address of the connected client in display order (most significant byte
    // first), so the BLE input never connects back to it. Thread-safe.
    void setClientAddress(const std::array<std::uint8_t, 6>& address, std::uint8_t type);
    void clearClientAddress();
    bool clientAddress(std::array<std::uint8_t, 6>& address, std::uint8_t& type) const;

    bool advertising() const;
    bool clientConnected() const;
    bool controlGranted() const;

    // Last payloads handed to the transport (diagnostics and host tests).
    const smartfitness::ftms::Bytes& lastIndoorBikeData() const;
    const smartfitness::ftms::Bytes& lastTreadmillData() const;
    // Last data notification of the given machine profile's characteristic.
    const smartfitness::ftms::Bytes& lastMachineData(smartfitness::MachineProfile profile) const;
    const smartfitness::ftms::Bytes& lastControlResponse() const;
    const smartfitness::ftms::Bytes& lastMachineStatus() const;
    const smartfitness::ftms::Bytes& feature() const;
    smartfitness::ftms::Bytes speedRange() const;

    using Clock = std::function<std::uint32_t()>;
    void setClock(Clock clock);

private:
    struct PendingWrite {
        std::array<std::uint8_t, kMaxWriteLength> data{};
        std::uint8_t length = 0;
        bool truncated = false;
        std::uint32_t generation = 0;
    };
    struct Transport;

    void handleConnectionChanges();
    void drainControlPoint();
    void handleControlPoint(const PendingWrite& write);
    void publishBikeData();
    void clearSessionData();
    void updateTrainingStatus();
    void startAdvertising();
    void stopAdvertising();
    void resetSession();
    void refreshProfile();
    std::uint32_t now() const;

    // Transport hooks; no-ops besides bookkeeping on the host.
    bool transportBegin();
    bool transportStartAdvertising();
    void transportStopAdvertising();
    void transportDisconnectClients();
    void transportApplyName();
    void transportSetFeature();
    void transportApplyProfile();
    void transportNotifyMachineData();
    void transportNotifyTrainingStatus();
    void transportIndicateResponse();
    void transportNotifyStatus();

    std::unique_ptr<Transport> transport_;
    std::string name_ = kDefaultName;
    bool begun_ = false;
    bool advertising_ = false;
    bool advertiseFailed_ = false;
    std::uint32_t advertiseFailedMs_ = 0;
    bool granted_ = false;
    bool connected_ = false;
    bool profileDisconnectPending_ = false;
    smartfitness::FtmsOutputProfile profileOverride_ = smartfitness::FtmsOutputProfile::Auto;
    smartfitness::MachineProfile profile_ = smartfitness::MachineProfile::IndoorBike;
    std::uint32_t seenGeneration_ = 0;
    std::uint32_t lastBikeDataMs_ = 0;
    bool bikeDataSent_ = false;
    bool trainingStatusSent_ = false;
    smartfitness::ftms::TrainingStatus trainingStatus_ = smartfitness::ftms::TrainingStatus::Idle;
    smartfitness::ftms::Bytes feature_;
    // Indexed by MachineProfile.
    std::array<smartfitness::ftms::Bytes, smartfitness::kMachineProfileCount> machineData_{};
    smartfitness::ftms::Bytes response_;
    smartfitness::ftms::Bytes status_;
    Clock clock_;

    mutable std::mutex mutex_;
    std::array<PendingWrite, kQueueCapacity> queue_{};
    std::array<std::uint8_t, 6> clientAddress_{};
    std::uint8_t clientAddressType_ = 0;
    bool clientAddressValid_ = false;
    std::size_t queueHead_ = 0;
    std::size_t queueCount_ = 0;
    std::atomic<std::uint32_t> generation_{0};
    std::atomic<int> connections_{0};
};
