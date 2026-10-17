#pragma once

// BLE input adapter (central role). Pure C++ on top of CentralTransport so the
// connection lifecycle (connect, reconnect with backoff, stale telemetry,
// advertisement-only devices) runs in host tests with a fake transport and a
// fake clock. Must only be used from the cooperative main loop.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ble/central_transport.hpp"
#include "bt_device_adapter.h"
#include "protocols/source_protocol.hpp"

struct PairedDevice {
    smartfitness::ble::BleAddress address;
    std::string adapterId;
    std::string name;
    bool valid = false;
    std::vector<smartfitness::ble::Uuid> advertisedServices;
};

struct BleSubscriptionStatus {
    smartfitness::ble::Uuid service;
    smartfitness::ble::Uuid characteristic;
    bool indicate = false;
    std::string state = "pending";  // pending, ok or failed
    // Why a subscription failed: not-found, unsupported, cccd-failed,
    // overflow, or the connection failure (connect-failed, timeout, ...).
    std::string reason;
};

struct ScanResult {
    smartfitness::ble::BleAddress address;
    std::string name;
    int rssi = 0;
    bool connectable = true;
    std::vector<smartfitness::ble::Uuid> services;
    smartfitness::ble::Bytes ftmsServiceData;
    smartfitness::ble::Bytes manufacturerData;
    std::string detectedAdapter;  // empty when no adapter matches
    std::uint32_t lastSeenMs = 0;
    std::uint32_t ftmsDataSeenMs = 0;
};

class BleInputAdapter final : public BtDeviceAdapter {
public:
    using Clock = std::function<std::uint32_t()>;

    struct Timing {
        std::uint32_t connectTimeoutMs = 15000;
        std::uint32_t connectMaxMs = 45000;
        std::uint32_t probeMs = 3000;
        std::uint32_t staleMs = 5000;
        // Reconnect when a connected device never delivers data this long.
        std::uint32_t noDataReconnectMs = 20000;
        std::uint32_t backoffInitialMs = 1000;
        std::uint32_t backoffMaxMs = 30000;
        std::uint32_t telemetryIntervalMs = 250;
    };

    enum class LinkState : std::uint8_t { Idle, Probing, Connecting, Ready, Backoff, Listening };

    static constexpr std::size_t kMaxScanResults = 24;
    static constexpr std::uint32_t kDefaultScanMs = 30000;

    BleInputAdapter(smartfitness::ble::CentralTransport& transport, Clock clock);
    BleInputAdapter(smartfitness::ble::CentralTransport& transport, Clock clock, const Timing& timing);

    // BtDeviceAdapter
    void begin() override;
    void loop() override;
    // True when an asynchronous connect/listen request was accepted.
    bool scanAndConnect() override;
    // User disconnect: closes the link and stops automatic reconnects.
    void disconnect() override;
    bool requestDisconnect();
    bool isConnected() const override;
    bool trainerDetected() const override;
    smartfitness::MachineProfile profile() const override;
    FitnessTelemetry telemetry() const override;
    smartfitness::TrainerCapabilities capabilities() const override;
    bool apply(const smartfitness::ControlRequest& request) override;

    // Pairing. setPairedDevice() does not connect by itself.
    void setPairedDevice(const PairedDevice& device);
    void clearPairedDevice();
    const PairedDevice& pairedDevice() const { return paired_; }
    const std::vector<smartfitness::ble::Uuid>& advertisedServices() const { return advertisedServices_; }
    const std::vector<BleSubscriptionStatus>& subscriptions() const { return subscriptions_; }
    // When enabled, begin() and link losses (re)connect automatically.
    void setAutoConnect(bool enabled) { autoConnect_ = enabled; }
    bool autoConnect() const { return autoConnect_; }

    // Scanning (results are deduplicated by address; when the list is full a
    // relevant device replaces the weakest irrelevant one, counted as dropped).
    bool startScan(std::uint32_t durationMs = kDefaultScanMs);
    bool stopScan();
    bool scanning() const { return userScan_; }
    const std::vector<ScanResult>& scanResults() const { return results_; }
    const ScanResult* findResult(const smartfitness::ble::BleAddress& address) const;
    std::uint32_t droppedScanResults() const { return droppedResults_; }
    std::uint32_t droppedScanReports() const { return transport_.droppedScanReports(); }
    std::uint32_t droppedEvents() const { return transport_.droppedEvents(); }
    std::uint32_t droppedNotifications() const { return transport_.droppedNotifications(); }
    std::uint32_t droppedLifecycleEvents() const { return transport_.droppedLifecycleEvents(); }
    std::uint32_t droppedCommands() const { return transport_.droppedCommands(); }

    // Address of the client currently connected to the FTMS output; it is
    // never connected to or listed (the bridge must not loop onto itself).
    void setExcludedPeer(const smartfitness::ble::BleAddress& address, bool valid);
    bool isExcluded(const smartfitness::ble::BleAddress& address) const;

    LinkState linkState() const { return state_; }
    static const char* linkStateName(LinkState state);
    bool stale() const;
    const std::string& lastError() const { return lastError_; }
    std::uint32_t reconnectAttempts() const { return attempts_; }
    std::uint32_t nextAttemptInMs() const;
    const char* protocolId() const;
    const smartfitness::protocols::SourceProtocol* protocol() const { return protocol_.get(); }

private:
    bool startConnection();
    bool connectAfterProbe();
    void connectTimedOut(std::uint32_t now);
    // A non-null subscriptionReason marks pending subscriptions as failed
    // and keeps the list visible during the backoff.
    void scheduleReconnect(const std::string& reason, const char* subscriptionReason = nullptr);
    void applySubscriptionResult(const smartfitness::ble::CentralEvent& event);
    bool closeLink();
    void handleEvent(const smartfitness::ble::CentralEvent& event);
    void handleScanReport(const smartfitness::ble::ScanReport& report);
    bool flushWrites();
    void maybeEmit(bool force);
    bool ensureProtocol();

    smartfitness::ble::CentralTransport& transport_;
    Clock clock_;
    Timing timing_;
    PairedDevice paired_;
    std::vector<smartfitness::ble::Uuid> advertisedServices_;
    std::vector<BleSubscriptionStatus> subscriptions_;
    std::vector<std::uint8_t> subscriptionIndices_;
    std::vector<bool> subscriptionOptional_;
    bool gattServicesRequested_ = false;
    std::vector<smartfitness::ble::Uuid> gattServices_;
    bool autoConnect_ = false;
    std::unique_ptr<smartfitness::protocols::SourceProtocol> protocol_;
    LinkState state_ = LinkState::Idle;
    std::uint32_t session_ = 0;
    std::uint32_t stateSinceMs_ = 0;
    std::uint32_t progressMs_ = 0;
    smartfitness::ble::ConnectPhase connectPhase_ = smartfitness::ble::ConnectPhase::Connect;
    std::uint32_t progressResults_ = 0;
    std::uint32_t expectedResults_ = 0;
    smartfitness::ble::Advertisement pairedAdvertisement_;
    bool hasPairedAdvertisement_ = false;
    std::uint32_t pairedAdvertisementMs_ = 0;
    std::uint32_t pairedServiceDataMs_ = 0;
    bool probeFound_ = false;
    std::uint32_t probeFoundMs_ = 0;
    std::uint32_t nextAttemptMs_ = 0;
    std::uint32_t backoffMs_ = 0;
    std::uint32_t attempts_ = 0;
    std::uint32_t lastReconnectLogMs_ = 0;
    bool hasReconnectLog_ = false;
    bool reconnecting_ = false;
    std::string lastError_;
    bool userScan_ = false;
    std::uint32_t scanUntilMs_ = 0;
    std::vector<ScanResult> results_;
    std::uint32_t droppedResults_ = 0;
    std::uint32_t observedLifecycleDrops_ = 0;
    smartfitness::ble::BleAddress excludedPeer_;
    bool excludedPeerValid_ = false;
    float targetPower_ = 0.0F;
    bool hasTargetPower_ = false;
    std::uint32_t lastEmitMs_ = 0;
    bool lastEmittedConnected_ = false;
    bool lastEmittedStale_ = false;
    smartfitness::TrainerCapabilities lastEmittedCaps_;
};

// Forwards to either the simulation or the BLE input. Both keep running their
// loop() (scan results are collected even while simulating), but only the
// active source reaches BridgeCore.
class InputSelector final : public BtDeviceAdapter {
public:
    enum class Source : std::uint8_t { Simulation, Ble };

    InputSelector(SimulatedDeviceAdapter& simulation, BtDeviceAdapter* ble);
    ~InputSelector() override;

    void select(Source source);
    Source source() const { return source_; }
    BtDeviceAdapter& active();
    const BtDeviceAdapter& active() const;

    void begin() override;
    void loop() override;
    bool scanAndConnect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool trainerDetected() const override;
    smartfitness::MachineProfile profile() const override;
    FitnessTelemetry telemetry() const override;
    smartfitness::TrainerCapabilities capabilities() const override;
    bool apply(const smartfitness::ControlRequest& request) override;

private:
    SimulatedDeviceAdapter& simulation_;
    BtDeviceAdapter* ble_;
    Source source_ = Source::Simulation;
};
