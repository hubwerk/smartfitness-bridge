#pragma once

// Wi-Fi mode policy (pure C++, host tested; the firmware WifiManager applies it).
//  * no credentials           -> SoftAP "SmartFitness-XXXX" (WPA2) + captive portal
//  * credentials              -> station; if not connected within fallbackMs,
//                                additionally open the SoftAP (AP+STA) while
//                                the station keeps retrying
//  * fallback AP is closed again once the station has been connected for
//    apHoldMs and no client is using the SoftAP.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

// Wi-Fi network scan requested from the web UI: repeated scan passes during
// a fixed window, merged by SSID (strongest signal wins, hidden networks are
// skipped). While the station is connected a single pass is run instead:
// every pass takes the radio off the home channel, and back-to-back passes
// would cut off the web clients reaching the bridge through that network.
// The firmware drives the passes; this class only keeps the state.
struct WifiNetwork {
    std::string ssid;
    int rssi = 0;  // dBm
    bool secure = false;
};

class WifiScan {
public:
    static constexpr std::uint32_t kDurationMs = 15000;
    static constexpr std::size_t kMaxNetworks = 32;

    // Starts a new window and clears previous results; false while one runs.
    // singlePass closes the window after the first completed pass.
    bool start(std::uint32_t nowMs, bool singlePass = false);
    // True while further scan passes may be started.
    bool windowOpen(std::uint32_t nowMs) const {
        return scanning_ && !(singlePass_ && passCompleted_) && nowMs - startedMs_ < kDurationMs;
    }
    void add(const WifiNetwork& network);
    // A scan pass finished (successfully, even without results).
    void passCompleted() { passCompleted_ = true; }
    void finish() { scanning_ = false; }
    bool scanning() const { return scanning_; }
    // Sorted by signal strength, strongest first.
    const std::vector<WifiNetwork>& networks() const { return networks_; }

private:
    bool scanning_ = false;
    bool singlePass_ = false;
    bool passCompleted_ = false;
    std::uint32_t startedMs_ = 0;
    std::vector<WifiNetwork> networks_;
};
