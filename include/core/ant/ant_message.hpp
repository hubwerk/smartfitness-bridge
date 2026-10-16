#pragma once

// ANT serial message protocol (ANT Message Protocol and Usage, rev 5.1):
//   SYNC (0xA4) | LENGTH | MESSAGE ID | PAYLOAD[LENGTH] | XOR CHECKSUM
// Pure C++ (host tested).

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace smartfitness {
namespace ant {

using Bytes = std::vector<std::uint8_t>;
using Page = std::array<std::uint8_t, 8>;

constexpr std::uint8_t kSync = 0xA4;
constexpr std::size_t kMaxPayload = 32;

namespace msg {
constexpr std::uint8_t kChannelEvent = 0x40;  // channel response / event
constexpr std::uint8_t kUnassignChannel = 0x41;
constexpr std::uint8_t kAssignChannel = 0x42;
constexpr std::uint8_t kChannelPeriod = 0x43;
constexpr std::uint8_t kChannelRfFrequency = 0x45;
constexpr std::uint8_t kSetNetworkKey = 0x46;
constexpr std::uint8_t kResetSystem = 0x4A;
constexpr std::uint8_t kOpenChannel = 0x4B;
constexpr std::uint8_t kCloseChannel = 0x4C;
constexpr std::uint8_t kBroadcastData = 0x4E;
constexpr std::uint8_t kAcknowledgedData = 0x4F;
constexpr std::uint8_t kChannelId = 0x51;
constexpr std::uint8_t kStartup = 0x6F;
}  // namespace msg

namespace code {
constexpr std::uint8_t kResponseNoError = 0x00;
constexpr std::uint8_t kEventTx = 0x03;
constexpr std::uint8_t kTransferTxCompleted = 0x05;
constexpr std::uint8_t kTransferTxFailed = 0x06;
constexpr std::uint8_t kChannelClosed = 0x07;
}  // namespace code

struct Message {
    std::uint8_t id = 0;
    Bytes payload;
};

Bytes encodeMessage(std::uint8_t id, const Bytes& payload);
Bytes encodeMessage(std::uint8_t id, std::uint8_t channel, const Page& page);

// Incremental parser: resynchronizes on SYNC and drops frames with a bad
// checksum or an oversized length.
class MessageParser {
public:
    // Returns true when `message` holds a complete frame.
    bool feed(std::uint8_t byte, Message& message);
    std::uint32_t dropped() const { return dropped_; }
    void reset();

private:
    enum class State : std::uint8_t { Sync, Length, Id, Payload, Checksum };
    State state_ = State::Sync;
    std::uint8_t length_ = 0;
    std::uint8_t checksum_ = 0;
    Message current_;
    std::uint32_t dropped_ = 0;
};

}  // namespace ant
}  // namespace smartfitness
