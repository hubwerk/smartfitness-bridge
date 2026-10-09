#pragma once

// Abstract BLE central (client) transport. The firmware implementation wraps
// NimBLE and does the blocking connect/discover/subscribe work on its own
// FreeRTOS task; host tests use a fake. Commands are non-blocking and every
// result comes back as an event that the cooperative main loop polls, so
// BridgeCore is never touched from a BLE callback.

#include <array>
#include <cstddef>
#include <cstdint>

#include "ble/ble_types.hpp"

namespace smartfitness {
namespace ble {

constexpr std::size_t kMaxEventData = 64;

enum class CentralEventType : std::uint8_t {
    Connected,      // mask = subscriptions that succeeded
    ConnectFailed,  // connection or service discovery failed
    Disconnected,   // link lost or closed
    Notification,   // index = subscription index
    ReadResult,     // index = read index (delivered before Connected)
    WriteFailed,
    ScanComplete
};

struct CentralEvent {
    CentralEventType type = CentralEventType::ScanComplete;
    std::uint32_t session = 0;
    std::uint8_t index = 0;
    std::uint32_t mask = 0;
    std::uint8_t length = 0;
    std::array<std::uint8_t, kMaxEventData> data{};

    void setData(const std::uint8_t* bytes, std::size_t size) {
        length = static_cast<std::uint8_t>(size > data.size() ? data.size() : size);
        for (std::size_t i = 0; i < length; ++i) data[i] = bytes[i];
    }
};

class CentralTransport {
public:
    virtual ~CentralTransport() = default;

    virtual bool begin() = 0;
    // Active scan; durationMs == 0 scans until stopScan(). With duplicates
    // every advertisement is reported (needed for broadcast-only devices).
    virtual bool startScan(std::uint32_t durationMs, bool duplicates) = 0;
    virtual void stopScan() = 0;
    virtual bool scanning() const = 0;
    // Starts an asynchronous connection identified by `session`.
    virtual bool connect(std::uint32_t session, const BleAddress& address, const ConnectPlan& plan) = 0;
    virtual void disconnect(std::uint32_t session) = 0;
    virtual bool write(std::uint32_t session, const GattWrite& write) = 0;
    virtual bool pollScan(ScanReport& report) = 0;
    virtual bool pollEvent(CentralEvent& event) = 0;
    // Address of this bridge, used to never connect to ourselves.
    virtual BleAddress ownAddress() const = 0;
};

}  // namespace ble
}  // namespace smartfitness
