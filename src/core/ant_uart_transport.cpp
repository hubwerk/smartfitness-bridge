#include "ant/ant_uart_transport.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace smartfitness {
namespace ant {
namespace {
constexpr std::uint8_t kRequest = 0x4D, kCapabilities = 0x54, kVersion = 0x3E;
}

AntUartTransport::AntUartTransport(AntSerial& serial, Clock clock)
    : serial_(serial), clock_(std::move(clock)) {}

std::uint32_t AntUartTransport::now() const {
    if (clock_) return clock_();
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool AntUartTransport::Ring::push(const std::uint8_t* data, std::size_t length) {
    if (length > bytes.size() - size) return false;
    for (std::size_t i = 0; i < length; ++i) bytes[(head + size + i) % bytes.size()] = data[i];
    size += length;
    return true;
}

std::size_t AntUartTransport::Ring::pop(std::uint8_t* data, std::size_t capacity) {
    const auto count = std::min(capacity, size);
    for (std::size_t i = 0; i < count; ++i) data[i] = bytes[(head + i) % bytes.size()];
    head = (head + count) % bytes.size();
    size -= count;
    return count;
}

bool AntUartTransport::queue(std::uint8_t id, const Bytes& payload) {
    const auto bytes = encodeMessage(id, payload);
    return tx_.push(bytes.data(), bytes.size());
}

void AntUartTransport::flush() {
    // One bounded write; the adapter never waits for UART FIFO space or nRTS.
    const auto count = std::min(tx_.size, tx_.bytes.size() - tx_.head);
    if (!count) return;
    const auto sent = std::min(count, serial_.write(tx_.bytes.data() + tx_.head, count));
    tx_.head = (tx_.head + sent) % tx_.bytes.size();
    tx_.size -= sent;
}

void AntUartTransport::probe(std::uint32_t timestamp) {
    probeMs_ = timestamp;
    fallbackSent_ = false;
    queue(msg::kResetSystem, Bytes{0});
}

void AntUartTransport::describe() {
    description_ = "UART ANT module";
    if (!version_.empty()) description_ += " · " + version_;
    if (channels_) description_ += " (" + std::to_string(channels_) + " ch)";
}

void AntUartTransport::handle(const Message& message, std::uint32_t timestamp) {
    const bool capabilities = message.id == kCapabilities && message.payload.size() >= 4 && message.payload[0] != 0;
    const bool startup = message.id == msg::kStartup && message.payload.size() == 1;
    if (startup || capabilities) {
        responseMs_ = timestamp;
        if (!connected_) {
            connected_ = true;
            probeMs_ = timestamp;
            queue(kRequest, Bytes{0, kCapabilities});
            queue(kRequest, Bytes{0, kVersion});
        }
    }
    if (capabilities) {
        channels_ = message.payload[0];
        describe();
    }
    if (connected_ && message.id == kVersion && !message.payload.empty()) {
        version_.clear();
        for (auto byte : message.payload) {
            if (byte == 0) break;
            if (byte >= 32 && byte <= 126) version_ += static_cast<char>(byte);
        }
        describe();
    }
    if (connected_) {
        const auto frame = encodeMessage(message.id, message.payload);
        // Drop whole frames rather than delivering a truncated ANT message.
        rx_.push(frame.data(), frame.size());
    }
}

bool AntUartTransport::begin() {
    const auto timestamp = now();
    if (!started_) {
        if (!serial_.begin()) return false;
        started_ = true;
        byteMs_ = timestamp;
        probe(timestamp);
    }
    if (timestamp - byteMs_ >= kInterByteTimeoutMs) parser_.reset();
    std::uint8_t bytes[128];
    const auto count = std::min(sizeof(bytes), serial_.read(bytes, sizeof(bytes)));
    Message message;
    for (std::size_t i = 0; i < count; ++i) {
        byteMs_ = timestamp;
        if (parser_.feed(bytes[i], message)) handle(message, timestamp);
    }
    if (connected_ && timestamp - responseMs_ >= kDisconnectMs) {
        connected_ = false;
        channels_ = 0;
        version_.clear();
        describe();
        rx_.clear();
        tx_.clear();
        parser_.reset();
        probe(timestamp);
    } else if (timestamp - probeMs_ >= kRetryMs) {
        if (connected_) {
            if (queue(kRequest, Bytes{0, kCapabilities})) probeMs_ = timestamp;
        } else {
            tx_.clear();
            parser_.reset();
            probe(timestamp);
        }
    } else if (!connected_ && !fallbackSent_ && timestamp - probeMs_ >= 1000) {
        fallbackSent_ = queue(kRequest, Bytes{0, kCapabilities});
    }
    flush();
    return true;
}

bool AntUartTransport::write(const std::uint8_t* data, std::size_t length) {
    return connected_ && data && length != 0 && tx_.push(data, length);
}

std::size_t AntUartTransport::read(std::uint8_t* data, std::size_t capacity) {
    return data ? rx_.pop(data, capacity) : 0;
}

}  // namespace ant
}  // namespace smartfitness
