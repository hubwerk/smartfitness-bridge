#pragma once

// Fake ANT radio for host tests: records every written ANT message and, in
// auto-respond mode, answers like an ANT USB stick (startup after reset,
// RESPONSE_NO_ERROR for channel configuration commands).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "ant/ant_message.hpp"
#include "ant/ant_transport.hpp"

class FakeAntTransport : public smartfitness::ant::AntTransport {
public:
    bool attached = true;
    bool autoRespond = true;
    bool failWrites = false;
    unsigned int begins = 0;
    std::uint8_t channels = 8;
    std::vector<smartfitness::ant::Message> written;
    std::deque<std::uint8_t> incoming;

    bool begin() override {
        ++begins;
        return true;
    }
    bool connected() const override { return attached; }
    bool write(const std::uint8_t* data, std::size_t length) override {
        if (!attached || failWrites) return false;
        smartfitness::ant::MessageParser parser;
        smartfitness::ant::Message message;
        for (std::size_t index = 0; index < length; ++index) {
            if (!parser.feed(data[index], message)) continue;
            written.push_back(message);
            if (autoRespond) respond(message);
        }
        return true;
    }
    std::size_t read(std::uint8_t* data, std::size_t capacity) override {
        std::size_t count = 0;
        while (count < capacity && !incoming.empty()) {
            data[count++] = incoming.front();
            incoming.pop_front();
        }
        return count;
    }
    const char* description() const override { return "fake"; }

    void inject(std::uint8_t id, const smartfitness::ant::Bytes& payload) {
        const auto bytes = smartfitness::ant::encodeMessage(id, payload);
        incoming.insert(incoming.end(), bytes.begin(), bytes.end());
    }
    void injectEventTx() { inject(smartfitness::ant::msg::kChannelEvent, {0x00, 0x01, smartfitness::ant::code::kEventTx}); }
    void injectPage(const smartfitness::ant::Page& page, bool acknowledged = true) {
        smartfitness::ant::Bytes payload{0x00};
        payload.insert(payload.end(), page.begin(), page.end());
        inject(acknowledged ? smartfitness::ant::msg::kAcknowledgedData : smartfitness::ant::msg::kBroadcastData, payload);
    }
    // Broadcast data pages written so far.
    std::vector<smartfitness::ant::Page> pages() const {
        std::vector<smartfitness::ant::Page> out;
        for (const auto& message : written) {
            if (message.id != smartfitness::ant::msg::kBroadcastData || message.payload.size() != 9) continue;
            smartfitness::ant::Page page;
            for (std::size_t index = 0; index < page.size(); ++index) page[index] = message.payload[index + 1];
            out.push_back(page);
        }
        return out;
    }
    std::size_t count(std::uint8_t id) const {
        std::size_t total = 0;
        for (const auto& message : written) total += message.id == id;
        return total;
    }

private:
    void respond(const smartfitness::ant::Message& message) {
        using namespace smartfitness::ant;
        switch (message.id) {
            case msg::kResetSystem: inject(msg::kStartup, {0x20}); break;
            case 0x4D:
                if (message.payload.size() >= 2 && message.payload[1] == 0x54)
                    inject(0x54, {channels, 1, 0, 0, 0});
                break;
            case msg::kSetNetworkKey:
            case msg::kAssignChannel:
            case msg::kChannelId:
            case msg::kChannelPeriod:
            case msg::kChannelRfFrequency:
            case msg::kOpenChannel:
            case msg::kCloseChannel:
                inject(msg::kChannelEvent, {message.payload.empty() ? std::uint8_t{0} : message.payload[0], message.id,
                                            code::kResponseNoError});
                break;
            default: break;
        }
    }
};
