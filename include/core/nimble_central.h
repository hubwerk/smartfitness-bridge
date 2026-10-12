#pragma once

// NimBLE (1.4.x) implementation of the BLE central transport. Firmware only.
//
// Central GAP/GATT operations run sequentially on a dedicated FreeRTOS
// worker task fed by a command queue (connect, discovery, subscribe and
// read/write with response are blocking calls in NimBLE-Arduino). NimBLE
// callbacks (scan results, notifications, disconnects) run on the NimBLE host
// task and only push into bounded queues. The cooperative main loop polls the
// queues through pollScan()/pollEvent(); BridgeCore is never touched from a
// BLE task. The peripheral role (FtmsOutput) shares the same NimBLE stack.
// Cancellation sets a flag immediately and may terminate an established link
// from the caller's task to release a worker blocked in GATT discovery.

#ifdef ARDUINO

#include <atomic>
#include <cstdint>

#include "ble/bounded_queue.hpp"
#include "ble/central_transport.hpp"
#include "ble/connection_session.hpp"

class NimbleCentral final : public smartfitness::ble::CentralTransport {
public:
    static constexpr std::size_t kScanQueue = 24;
    static constexpr std::size_t kEventQueue = 32;
    static constexpr std::size_t kCommandQueue = 8;
    static constexpr std::uint8_t kConnectTimeoutS = 5;
    static constexpr std::uint32_t kScanWindowNormal = 37;
    static constexpr std::uint32_t kScanWindowHighDuty = 80;
    static constexpr std::uint32_t kScanInterval = 97;

    NimbleCentral() = default;
    NimbleCentral(const NimbleCentral&) = delete;
    NimbleCentral& operator=(const NimbleCentral&) = delete;

    bool begin() override;
    bool startScan(std::uint32_t durationMs, bool duplicates, bool highDuty = false) override;
    bool stopScan() override;
    bool scanning() const override { return scanning_.load(); }
    bool connect(std::uint32_t session, const smartfitness::ble::BleAddress& address,
                 const smartfitness::ble::ConnectPlan& plan) override;
    bool disconnect(std::uint32_t session) override;
    bool write(std::uint32_t session, const smartfitness::ble::GattWrite& write) override;
    bool pollScan(smartfitness::ble::ScanReport& report) override;
    bool pollEvent(smartfitness::ble::CentralEvent& event) override;
    smartfitness::ble::BleAddress ownAddress() const override { return own_; }

    std::uint32_t droppedScanReports() const override { return scans_.dropped(); }
    std::uint32_t droppedEvents() const override { return events_.dropped(); }
    std::uint32_t droppedNotifications() const override { return events_.droppedNotifications(); }
    std::uint32_t droppedLifecycleEvents() const override { return events_.droppedLifecycleEvents(); }
    std::uint32_t droppedCommands() const override { return droppedCommands_.load(); }

    // Used by the NimBLE callbacks (host task); not part of the public API.
    struct Impl;

private:
    enum class Op : std::uint8_t { StartScan, StopScan, Connect, Disconnect, Write };
    struct Cmd {
        Op op = Op::StopScan;
        std::uint32_t session = 0;
        std::uint32_t durationMs = 0;
        bool duplicates = false;
        bool highDuty = false;
        smartfitness::ble::BleAddress address;
        smartfitness::ble::ConnectPlan plan;
        smartfitness::ble::GattWrite write;
    };

    static void workerEntry(void* self);
    void worker();
    bool send(const Cmd& cmd);
    void doStartScan(const Cmd& cmd);
    void doConnect(const Cmd& cmd);
    void doDisconnect();
    void doWrite(const Cmd& cmd);
    bool cancelled(std::uint32_t session) const { return requestedSession_.load() != session; }
    void pushEvent(smartfitness::ble::CentralEventType type, std::uint32_t session, std::uint8_t index = 0,
                   std::uint32_t mask = 0, const std::uint8_t* data = nullptr, std::size_t length = 0);

    friend struct Impl;
    Impl* impl_ = nullptr;
    void* queue_ = nullptr;  // QueueHandle_t
    std::atomic<bool> scanning_{false};
    std::uint32_t scanGeneration_ = 0;  // protected by commandMutex_
    std::atomic<std::uint32_t> session_{0};
    std::atomic<std::uint32_t> requestedSession_{0};
    smartfitness::ble::PendingConnection pendingConnection_;
    std::atomic<std::uint32_t> droppedCommands_{0};
    std::mutex commandMutex_;
    smartfitness::ble::BleAddress own_;
    smartfitness::BoundedQueue<smartfitness::ble::ScanReport, kScanQueue> scans_;
    smartfitness::ble::CentralEventQueue<kEventQueue> events_;
};

#endif  // ARDUINO
