#pragma once

#include <cstdint>
#include <string>

namespace smartfitness {

// The single selectable output of the bridge. Persisted: append only.
enum class OutputProtocol : std::uint8_t { Off, BleFtms, AntFec };
constexpr std::uint8_t kOutputProtocolCount = 3;

// Stable id (console, web API, persisted JSON).
inline const char* outputProtocolName(OutputProtocol protocol) {
    switch (protocol) {
        case OutputProtocol::BleFtms: return "ble-ftms";
        case OutputProtocol::AntFec: return "ant-fec";
        case OutputProtocol::Off: break;
    }
    return "off";
}

// Display label (web UI, HMI).
inline const char* outputProtocolLabel(OutputProtocol protocol) {
    switch (protocol) {
        case OutputProtocol::BleFtms: return "BLE FTMS";
        case OutputProtocol::AntFec: return "ANT+ FE-C";
        case OutputProtocol::Off: break;
    }
    return "Off";
}

inline bool parseOutputProtocol(const std::string& value, OutputProtocol& protocol) {
    for (std::uint8_t index = 0; index < kOutputProtocolCount; ++index) {
        const auto candidate = static_cast<OutputProtocol>(index);
        if (value == outputProtocolName(candidate)) {
            protocol = candidate;
            return true;
        }
    }
    return false;
}

}  // namespace smartfitness
