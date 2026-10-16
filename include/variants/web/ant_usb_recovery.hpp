#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace smartfitness {
namespace usb_transport {

class AntUsbRecovery {
public:
    enum class Endpoint : std::uint8_t { In, Out };

    void reset() { pending_ = 0; }
    void request(Endpoint endpoint) { pending_ |= mask(endpoint); }
    bool pending(Endpoint endpoint) const { return (pending_ & mask(endpoint)) != 0; }
    bool available() const { return pending_ == 0; }

    template <typename Halt, typename Flush, typename Clear, typename Resubmit>
    bool service(Endpoint endpoint, bool busy, Halt&& halt, Flush&& flush, Clear&& clear, Resubmit&& resubmit) {
        if (!pending(endpoint) || busy || !halt() || !flush() || !clear()) return false;
        if (endpoint == Endpoint::In && !resubmit()) return false;
        pending_ &= static_cast<std::uint8_t>(~mask(endpoint));
        return true;
    }

private:
    static constexpr std::uint8_t mask(Endpoint endpoint) {
        return endpoint == Endpoint::In ? 0x01 : 0x02;
    }

    std::uint8_t pending_ = 0;
};

class AntUsbPendingWrite {
public:
    static constexpr std::size_t kCapacity = 64;

    std::uint8_t* buffer() { return bytes_.data(); }
    const std::uint8_t* data() const { return bytes_.data(); }
    std::size_t size() const { return size_; }
    bool pending() const { return size_ != 0; }
    void stage(std::size_t size) { size_ = size <= kCapacity ? size : 0; }
    void complete() { size_ = 0; }

private:
    std::array<std::uint8_t, kCapacity> bytes_{};
    std::size_t size_ = 0;
};

}  // namespace usb_transport
}  // namespace smartfitness
