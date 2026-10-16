#include "platform/ant_uart_serial.h"

#if defined(ARDUINO) && defined(SF_ANT_UART_TX) && defined(SF_ANT_UART_RX)
#include <Arduino.h>
#include <HardwareSerial.h>

#include <algorithm>

#ifndef SF_ANT_UART_BAUD
#define SF_ANT_UART_BAUD 57600
#endif

static_assert(SF_ANT_UART_BAUD == 57600 || SF_ANT_UART_BAUD == 50000 ||
              SF_ANT_UART_BAUD == 38400 || SF_ANT_UART_BAUD == 19200 ||
              SF_ANT_UART_BAUD == 4800, "Unsupported ANT UART baud rate");

namespace {

class ArduinoAntSerial final : public smartfitness::ant::AntSerial {
public:
    bool begin() override {
        if (!initialized_) {
#ifdef SF_ANT_UART_RTS
            pinMode(SF_ANT_UART_RTS, INPUT_PULLUP);
#endif
            serial_.begin(SF_ANT_UART_BAUD, SERIAL_8N1, SF_ANT_UART_RX, SF_ANT_UART_TX);
#ifdef SF_ANT_UART_RTS
            // Hardware CTS also pauses bytes already queued in the TX FIFO.
            if (!serial_.setPins(SF_ANT_UART_RX, SF_ANT_UART_TX, SF_ANT_UART_RTS, -1) ||
                !serial_.setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS)) return false;
#endif
#ifdef SF_ANT_UART_RESET
            digitalWrite(SF_ANT_UART_RESET, LOW);
            pinMode(SF_ANT_UART_RESET, OUTPUT);
            resetAt_ = millis();
#endif
            initialized_ = true;
        }
#ifdef SF_ANT_UART_RESET
        if (!ready_) {
            if (static_cast<std::uint32_t>(millis() - resetAt_) < 1) return false;
            digitalWrite(SF_ANT_UART_RESET, HIGH);
            ready_ = true;
        }
#else
        ready_ = true;
#endif
        return ready_;
    }

    std::size_t read(std::uint8_t* data, std::size_t capacity) override {
        if (!ready_ || !data) return 0;
        const auto limit = std::min<std::size_t>(capacity, 128);
        std::size_t count = 0;
        while (count < limit && serial_.available() > 0) {
            const int byte = serial_.read();
            if (byte < 0) break;
            data[count++] = static_cast<std::uint8_t>(byte);
        }
        return count;
    }

    std::size_t write(const std::uint8_t* data, std::size_t length) override {
        if (!ready_ || !data || !length) return 0;
#ifdef SF_ANT_UART_RTS
        if (digitalRead(SF_ANT_UART_RTS) != LOW) return 0;
#endif
        const int available = serial_.availableForWrite();
        if (available <= 0) return 0;
        return serial_.write(data, std::min(length, static_cast<std::size_t>(available)));
    }

private:
    HardwareSerial serial_{1};
    bool initialized_ = false;
    bool ready_ = false;
#ifdef SF_ANT_UART_RESET
    std::uint32_t resetAt_ = 0;
#endif
};

}  // namespace

smartfitness::ant::AntUartTransport& antUartTransport() {
    static ArduinoAntSerial serial;
    static smartfitness::ant::AntUartTransport transport(serial, [] { return millis(); });
    return transport;
}

#endif
