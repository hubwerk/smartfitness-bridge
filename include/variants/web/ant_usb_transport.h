#pragma once

// ANT+ USB stick on the ESP32-S3 USB-OTG port (DevKitC-1 "USB" connector)
// through the ESP-IDF USB Host Library. Supports the Dynastream ANTUSB2
// (0fcf:1008) and ANTUSB-m (0fcf:1009) stick class, which COOSPO and most
// other ANT+ USB sticks report: one vendor-specific interface with a bulk
// IN and a bulk OUT endpoint carrying ANT serial messages.
//
// USB host work runs in two FreeRTOS tasks (library + client); the main loop
// exchanges bytes through FreeRTOS stream/message buffers, so every
// AntTransport call is non-blocking. The port needs 5 V on VBUS: bridge the
// DevKitC-1 "USB-OTG" solder jumper and power the board via the UART port.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ant/ant_transport.hpp"

class AntUsbTransport : public smartfitness::ant::AntTransport {
public:
    static constexpr std::uint16_t kVendorDynastream = 0x0FCF;
    static constexpr std::uint16_t kProductAntUsb2 = 0x1008;
    static constexpr std::uint16_t kProductAntUsbM = 0x1009;

    static constexpr bool supported(std::uint16_t vendor, std::uint16_t product) {
        return vendor == kVendorDynastream && (product == kProductAntUsb2 || product == kProductAntUsbM);
    }

    bool begin() override;
    bool connected() const override;
    bool write(const std::uint8_t* data, std::size_t length) override;
    std::size_t read(std::uint8_t* data, std::size_t capacity) override;
    const char* description() const override;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool failed_ = false;
};
