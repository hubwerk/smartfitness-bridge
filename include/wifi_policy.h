#pragma once

// Wi-Fi mode policy (pure C++, host tested; the firmware WifiManager applies it).
//  * no credentials           -> SoftAP "SmartFitness-XXXX" (WPA2) + captive portal
//  * credentials              -> station; if not connected within fallbackMs,
//                                additionally open the SoftAP (AP+STA) while
//                                the station keeps retrying
//  * fallback AP is closed again once the station has been connected for
//    apHoldMs and no client is using the SoftAP.

#include <array>
#include <cstdint>
#include <string>

enum class WifiMode : std::uint8_t { Ap, Sta, ApSta };

const char* wifiModeName(WifiMode mode);

// "SmartFitness-XXXX" from the last two bytes of the MAC address.
std::string softApSsid(const std::array<std::uint8_t, 6>& mac);

class WifiPolicy {
public:
    struct Timing {
        std::uint32_t fallbackMs = 20000;
        std::uint32_t apHoldMs = 120000;
    };

    WifiPolicy() = default;
    explicit WifiPolicy(const Timing& timing) : timing_(timing) {}

    WifiMode begin(bool hasCredentials, std::uint32_t nowMs);
    // Returns the desired mode for the current station/AP state.
    WifiMode update(bool stationConnected, int apStations, std::uint32_t nowMs);
    WifiMode mode() const { return mode_; }
    bool hasCredentials() const { return hasCredentials_; }

private:
    Timing timing_;
    WifiMode mode_ = WifiMode::Ap;
    bool hasCredentials_ = false;
    std::uint32_t disconnectedSinceMs_ = 0;
    bool wasConnected_ = false;
    std::uint32_t connectedSinceMs_ = 0;
};
