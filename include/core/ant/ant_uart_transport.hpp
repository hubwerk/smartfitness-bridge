#pragma once

#include <array>
#include <functional>
#include <string>
#include "ant_message.hpp"
#include "ant_transport.hpp"

namespace smartfitness {
namespace ant {

// Non-blocking serial port; write() may accept fewer bytes than requested.
class AntSerial {
public:
    virtual ~AntSerial() = default;
    virtual bool begin() = 0;
    virtual std::size_t read(std::uint8_t* data, std::size_t capacity) = 0;
    virtual std::size_t write(const std::uint8_t* data, std::size_t length) = 0;
};

class AntUartTransport : public AntTransport {
public:
    using Clock = std::function<std::uint32_t()>;
    explicit AntUartTransport(AntSerial& serial, Clock clock = {});
    // Also services detection and the bounded RX/TX queues on each loop.
    bool begin() override;
    bool connected() const override { return connected_; }
    bool write(const std::uint8_t* data, std::size_t length) override;
    std::size_t read(std::uint8_t* data, std::size_t capacity) override;
    const char* description() const override { return description_.c_str(); }
    std::uint8_t channelLimit() const { return channels_; }

    static constexpr std::uint32_t kRetryMs = 2000;
    static constexpr std::uint32_t kDisconnectMs = 6000;
    static constexpr std::uint32_t kInterByteTimeoutMs = 100;

private:
    struct Ring {
        std::array<std::uint8_t, 512> bytes{};
        std::size_t head = 0, size = 0;
        bool push(const std::uint8_t* data, std::size_t length);
        std::size_t pop(std::uint8_t* data, std::size_t capacity);
        void clear() { head = size = 0; }
    };
    std::uint32_t now() const;
    void probe(std::uint32_t timestamp);
    void handle(const Message& message, std::uint32_t timestamp);
    bool queue(std::uint8_t id, const Bytes& payload);
    void describe();
    void flush();

    AntSerial& serial_;
    Clock clock_;
    Ring rx_, tx_;
    MessageParser parser_;
    bool started_ = false, connected_ = false, fallbackSent_ = false;
    std::uint32_t probeMs_ = 0, responseMs_ = 0, byteMs_ = 0;
    std::uint8_t channels_ = 0;
    std::string version_;
    std::string description_ = "UART ANT module";
};

}  // namespace ant
}  // namespace smartfitness
