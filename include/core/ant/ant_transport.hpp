#pragma once

#include <cstddef>
#include <cstdint>

namespace smartfitness {
namespace ant {

// Byte stream to an ANT radio speaking the ANT serial message protocol
// (USB stick, UART module). All calls are made from the main loop.
class AntTransport {
public:
    virtual ~AntTransport() = default;
    // Starts the transport (idempotent); false when it cannot run at all.
    virtual bool begin() = 0;
    // True while a radio is attached and ready for writes.
    virtual bool connected() const = 0;
    // Queues one complete message; false when not connected or full.
    virtual bool write(const std::uint8_t* data, std::size_t length) = 0;
    // Returns up to `capacity` received bytes (0 when none).
    virtual std::size_t read(std::uint8_t* data, std::size_t capacity) = 0;
    // Short hardware description for diagnostics (e.g. "USB 0fcf:1008").
    virtual const char* description() const = 0;
};

}  // namespace ant
}  // namespace smartfitness
