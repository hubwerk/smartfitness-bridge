#pragma once

// Protocol layer for BLE input devices. A SourceProtocol is pure C++: it
// declares what to subscribe/read after connecting, turns notifications into
// samples and control requests into GATT writes (queued in an outbox that the
// BLE adapter drains). The NimBLE transport is kept separate so every
// protocol can be exercised by host tests with captured byte sequences.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ble/ble_types.hpp"
#include "bridge/bridge_core.hpp"
#include "bridge/machine_profile.hpp"

namespace smartfitness {
namespace protocols {

using SourceSample = FitnessMetrics;

class SourceProtocol {
public:
    virtual ~SourceProtocol() = default;

    virtual const char* id() const = 0;
    virtual MachineProfile profile() const;
    // Advertisement-only devices (e.g. Keiser M3i) are never connected.
    virtual bool advertisementOnly() const { return false; }
    virtual ble::ConnectPlan plan() const = 0;
    // Called once the transport finished subscribing; bit N of subscribedMask
    // is set when plan().subscriptions[N] succeeded. Return false when the
    // device is unusable (lastError() explains why).
    virtual bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) = 0;
    virtual void onRead(std::size_t index, const std::uint8_t* data, std::size_t length);
    virtual void onWriteEndpoint(const ble::Uuid&, const ble::Uuid&) {}
    virtual void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) = 0;
    virtual void onAdvertisement(const ble::Advertisement& advertisement, std::uint32_t nowMs);
    virtual void onWriteFailed(std::uint32_t nowMs);
    // Periodic work: polls, init sequences and retries.
    virtual void tick(std::uint32_t nowMs);
    // Some vendor handshakes take longer than the normal no-data timeout.
    // Zero keeps the adapter default; this is a bounded startup grace only.
    virtual std::uint32_t startupTimeoutMs() const { return 0; }
    // Translate a request already validated by BridgeCore into writes.
    virtual bool control(const ControlRequest& request, std::uint32_t nowMs);
    virtual TrainerCapabilities capabilities() const = 0;
    // Trainer == the device currently accepts at least one control.
    bool isTrainer() const { return capabilities().controls != 0; }
    // Forget all per-connection state (called before connecting and on loss).
    virtual void reset();

    const SourceSample& sample() const { return sample_; }
    SourceSample freshSample(std::uint32_t nowMs, std::uint32_t lifetimeMs) const;
    bool expireSamples(std::uint32_t nowMs, std::uint32_t lifetimeMs);
    bool hasData() const { return hasData_; }
    std::uint32_t lastDataMs() const { return lastDataMs_; }
    bool takeWrite(ble::GattWrite& out);
    std::size_t pendingWrites() const { return outbox_.size(); }
    const std::string& lastError() const { return lastError_; }
    // A fatal protocol error (e.g. init handshake never answered): the adapter
    // drops the connection and retries with backoff.
    bool failed() const { return failed_; }

protected:
    static constexpr std::size_t kMaxOutbox = 16;

    bool queueWrite(const ble::Uuid& service, const ble::Uuid& characteristic, const ble::Bytes& payload,
                    bool withResponse = true);
    // Record that a fresh sample arrived; also accumulates observed metrics.
    void touch(std::uint32_t nowMs);
    void commitSample(const SourceSample& record, std::uint32_t nowMs, bool replace = true);
    void commitFragment(const SourceSample& record, std::uint32_t nowMs, std::uint8_t group);
    void clearSample();
    std::uint32_t observedMetrics() const { return observedMetrics_; }
    void setError(const std::string& error) { lastError_ = error; }
    void fail(const std::string& error) {
        lastError_ = error;
        failed_ = true;
    }

    SourceSample sample_;

private:
    std::vector<ble::GattWrite> outbox_;
    std::string lastError_;
    std::uint32_t lastDataMs_ = 0;
    std::uint32_t observedMetrics_ = 0;
    bool hasData_ = false;
    bool failed_ = false;
    std::vector<std::uint32_t> fieldTimes_;
    std::vector<std::uint8_t> fieldOwners_;
};

// Maps a 0..100 % resistance request onto an integer device level.
int percentToLevel(float percent, int minimum, int maximum);

}  // namespace protocols
}  // namespace smartfitness
