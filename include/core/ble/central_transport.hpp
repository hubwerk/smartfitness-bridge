#pragma once

// Abstract BLE central (client) transport. The firmware implementation wraps
// NimBLE and does the blocking connect/discover/subscribe work on its own
// FreeRTOS task; host tests use a fake. Commands are non-blocking and every
// result comes back as an event that the cooperative main loop polls, so
// BridgeCore is never touched from a BLE callback.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ble/ble_types.hpp"
#include "ble/bounded_queue.hpp"

namespace smartfitness {
namespace ble {

constexpr std::size_t kMaxEventData = 64;
constexpr std::uint8_t kConnectFailure = 0;
constexpr std::uint8_t kServiceDiscoveryFailure = 1;

// SubscriptionResult outcome, carried in CentralEvent::reasonCode.
enum class SubscriptionOutcome : std::uint8_t {
    Ok,
    NotFound,     // service or characteristic not found
    Unsupported,  // characteristic lacks notify/indicate as requested
    CccdFailed,   // writing the client characteristic configuration failed
    Overflow      // discovered beyond kMaxSubscriptions; index = number skipped
};
constexpr std::uint32_t kSubscriptionIndicate = 1U;  // SubscriptionResult mask bit
enum class ConnectPhase : std::uint8_t { Connect, Discovery, Read, Subscribe };

enum class CentralEventType : std::uint8_t {
    Connected,      // mask = subscriptions that succeeded
    ConnectFailed,  // connection or service discovery failed
    Disconnected,   // link lost or closed
    Notification,   // index = subscription index
    ReadResult,     // index = read index (delivered before Connected)
    WriteEndpoint,  // discovered service + characteristic UUID (32 bytes)
    WriteFailed,
    ScanComplete,
    // Before Connected, one per effective subscription: index = subscription
    // index, data = service + characteristic UUID (32 bytes), mask =
    // kSubscriptionIndicate when indicating, reasonCode = SubscriptionOutcome.
    // Exception: a final Overflow event names the first characteristic that
    // did not fit and its index is the number skipped, not a list position.
    SubscriptionResult,
    // Before Connected when ConnectPlan::reportServices: one GATT primary
    // service UUID (16 bytes) per event.
    GattService,
    ConnectProgress  // index = ConnectPhase, before a blocking connect step
};

struct CentralEvent {
    CentralEventType type = CentralEventType::ScanComplete;
    std::uint32_t session = 0;
    std::uint8_t index = 0;
    std::uint16_t reasonCode = 0;
    std::uint32_t mask = 0;
    std::uint8_t length = 0;
    std::array<std::uint8_t, kMaxEventData> data{};
#if defined(ARDUINO) && defined(DEBUG_WEB)
    std::size_t originalLength = 0;
#endif

    void setData(const std::uint8_t* bytes, std::size_t size) {
#if defined(ARDUINO) && defined(DEBUG_WEB)
        originalLength = size;
#endif
        length = static_cast<std::uint8_t>(size > data.size() ? data.size() : size);
        for (std::size_t i = 0; i < length; ++i) data[i] = bytes[i];
    }
};
static_assert(kMaxEventData >= 32, "SubscriptionResult/WriteEndpoint carry two 128-bit UUIDs");

inline const char* subscriptionOutcomeName(SubscriptionOutcome outcome) {
    switch (outcome) {
        case SubscriptionOutcome::Ok: return "ok";
        case SubscriptionOutcome::NotFound: return "not-found";
        case SubscriptionOutcome::Unsupported: return "unsupported";
        case SubscriptionOutcome::CccdFailed: return "cccd-failed";
        case SubscriptionOutcome::Overflow: return "overflow";
    }
    return "unknown";
}

template <std::size_t Capacity>
class CentralEventQueue {
public:
    bool push(const CentralEvent& event) {
        bool evicted = false;
        const bool accepted = event.type == CentralEventType::Notification
            ? queue_.push(event)
            : queue_.pushOrEvict(event, [](const CentralEvent& queued) {
                  return queued.type == CentralEventType::Notification;
              }, evicted);
        if (evicted || (!accepted && event.type == CentralEventType::Notification)) ++notificationsDropped_;
        if (!accepted && event.type != CentralEventType::Notification) ++lifecycleDropped_;
        return accepted;
    }
    bool pop(CentralEvent& event) { return queue_.pop(event); }
    std::uint32_t dropped() const { return queue_.dropped(); }
    std::uint32_t droppedNotifications() const { return notificationsDropped_.load(); }
    std::uint32_t droppedLifecycleEvents() const { return lifecycleDropped_.load(); }

private:
    BoundedQueue<CentralEvent, Capacity> queue_;
    std::atomic<std::uint32_t> notificationsDropped_{0};
    std::atomic<std::uint32_t> lifecycleDropped_{0};
};

class CentralTransport {
public:
    virtual ~CentralTransport() = default;

    virtual bool begin() = 0;
    // Active scan; durationMs == 0 scans until stopScan(). With duplicates
    // every advertisement is reported (needed for broadcast-only devices).
    // highDuty uses a larger scan window during user-initiated pairing scans.
    virtual bool startScan(std::uint32_t durationMs, bool duplicates, bool highDuty = false) = 0;
    // High-duty reconnect scan; firmware filters reports to the paired address.
    virtual bool startProbe(const BleAddress&, std::uint32_t durationMs) {
        return startScan(durationMs, true, true);
    }
    // Also cancels a pending connection, but never closes a Ready link.
    // False reports enqueue failure even when out-of-band cancellation ran.
    virtual bool stopScan() = 0;
    virtual bool scanning() const = 0;
    // Starts an asynchronous connection identified by `session`.
    virtual bool connect(std::uint32_t session, const BleAddress& address, const ConnectPlan& plan) = 0;
    virtual bool disconnect(std::uint32_t session) = 0;
    virtual bool write(std::uint32_t session, const GattWrite& write) = 0;
    virtual bool pollScan(ScanReport& report) = 0;
    virtual bool pollEvent(CentralEvent& event) = 0;
    // Address of this bridge, used to never connect to ourselves.
    virtual BleAddress ownAddress() const = 0;
    virtual std::uint32_t droppedScanReports() const { return 0; }
    virtual std::uint32_t droppedEvents() const { return 0; }
    virtual std::uint32_t droppedNotifications() const { return 0; }
    virtual std::uint32_t droppedLifecycleEvents() const { return 0; }
    virtual std::uint32_t droppedCommands() const { return 0; }
};

}  // namespace ble
}  // namespace smartfitness
