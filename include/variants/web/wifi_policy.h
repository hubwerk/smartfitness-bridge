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

// Wi-Fi network scan requested from the web UI: repeated sweeps over all
// channels during a fixed window, merged by SSID (strongest signal wins,
// hidden networks are skipped). Each sweep scans one channel at a time so the
// radio returns to the home channel in between: the access point keeps
// beaconing and serving its clients. While web clients depend on the radio
// (station connected or clients on the access point) a single sweep is run.
// The firmware drives the channel scans; this class only keeps the state.
struct WifiNetwork {
    std::string ssid;
    int rssi = 0;  // dBm
    bool secure = false;
};

class WifiScan {
public:
    static constexpr std::uint32_t kDurationMs = 15000;
    static constexpr std::size_t kMaxNetworks = 32;
    static constexpr std::uint8_t kChannels = 13;

    // A single sweep whenever someone reaches the bridge over Wi-Fi.
    static bool singlePassFor(bool stationConnected, unsigned apStations) {
        return stationConnected || apStations > 0;
    }
    // Starts a new window at channel 1 and clears previous results; false while one runs.
    // singlePass closes the window after the first complete sweep.
    bool start(std::uint32_t nowMs, bool singlePass = false);
    // True while further channel scans may be started.
    bool windowOpen(std::uint32_t nowMs) const {
        return scanning_ && !(singlePass_ && sweeps_ > 0) && nowMs - startedMs_ < kDurationMs;
    }
    // Channel (1..kChannels) the next scan has to cover.
    std::uint8_t channel() const { return channel_; }
    void add(const WifiNetwork& network);
    // The scan of channel() finished with the given number of access points;
    // advances to the next channel. True when this completed a sweep.
    bool channelCompleted(unsigned found);
    // Access points seen during the last complete sweep.
    unsigned lastSweepFound() const { return lastSweepFound_; }
    void finish() { scanning_ = false; }
    bool scanning() const { return scanning_; }
    // Sorted by signal strength, strongest first.
    const std::vector<WifiNetwork>& networks() const { return networks_; }

private:
    bool scanning_ = false;
    bool singlePass_ = false;
    std::uint8_t channel_ = 1;
    unsigned sweeps_ = 0;
    unsigned sweepFound_ = 0;
    unsigned lastSweepFound_ = 0;
    std::uint32_t startedMs_ = 0;
    std::vector<WifiNetwork> networks_;
};
