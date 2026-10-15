#include "ant/ant_message.hpp"

namespace smartfitness {
namespace ant {

Bytes encodeMessage(std::uint8_t id, const Bytes& payload) {
    Bytes out;
    if (payload.size() > kMaxPayload) return out;
    out.reserve(payload.size() + 4);
    out.push_back(kSync);
    out.push_back(static_cast<std::uint8_t>(payload.size()));
    out.push_back(id);
    out.insert(out.end(), payload.begin(), payload.end());
    std::uint8_t checksum = 0;
    for (const std::uint8_t byte : out) checksum ^= byte;
    out.push_back(checksum);
    return out;
}

Bytes encodeMessage(std::uint8_t id, std::uint8_t channel, const Page& page) {
    Bytes payload{channel};
    payload.insert(payload.end(), page.begin(), page.end());
    return encodeMessage(id, payload);
}

void MessageParser::reset() {
    state_ = State::Sync;
    current_ = {};
}

bool MessageParser::feed(std::uint8_t byte, Message& message) {
    switch (state_) {
        case State::Sync:
            if (byte == kSync) {
                checksum_ = byte;
                state_ = State::Length;
            }
            return false;
        case State::Length:
            if (byte > kMaxPayload) {
                ++dropped_;
                // A SYNC byte here may start the next frame.
                checksum_ = kSync;
                state_ = byte == kSync ? State::Length : State::Sync;
                return false;
            }
            length_ = byte;
            checksum_ ^= byte;
            state_ = State::Id;
            return false;
        case State::Id:
            current_.id = byte;
            current_.payload.clear();
            checksum_ ^= byte;
            state_ = length_ ? State::Payload : State::Checksum;
            return false;
        case State::Payload:
            current_.payload.push_back(byte);
            checksum_ ^= byte;
            if (current_.payload.size() == length_) state_ = State::Checksum;
            return false;
        case State::Checksum:
            state_ = State::Sync;
            if (byte != checksum_) {
                ++dropped_;
                return false;
            }
            message = current_;
            return true;
    }
    return false;
}

}  // namespace ant
}  // namespace smartfitness
