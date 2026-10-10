#pragma once

#include <atomic>
#include <cstdint>

namespace smartfitness {
namespace ble {

// The GAP link identity is independent of the latest queued connect command.
class ConnectionSession {
public:
    void connected(std::uint32_t session) { session_ = session; }
    std::uint32_t disconnected() { return session_.exchange(0); }
    std::uint32_t active() const { return session_.load(); }

private:
    std::atomic<std::uint32_t> session_{0};
};

class PendingConnection {
public:
    void start(std::uint32_t session) { session_ = session; }
    void complete(std::uint32_t session) {
        session_.compare_exchange_strong(session, 0);
    }
    std::uint32_t cancel() { return session_.exchange(0); }
    std::uint32_t active() const { return session_.load(); }

private:
    std::atomic<std::uint32_t> session_{0};
};

}  // namespace ble
}  // namespace smartfitness
