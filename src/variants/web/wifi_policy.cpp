#include "wifi_policy.h"

#include <algorithm>
#include <cstdio>

const char* wifiModeName(WifiMode mode) {
    switch (mode) {
        case WifiMode::Ap: return "ap";
        case WifiMode::Sta: return "sta";
        case WifiMode::ApSta: return "ap+sta";
    }
    return "unknown";
}

std::string softApSsid(const std::array<std::uint8_t, 6>& mac) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "SmartFitness-%02X%02X", mac[4], mac[5]);
    return buffer;
}

WifiMode WifiPolicy::begin(bool hasCredentials, std::uint32_t nowMs) {
    hasCredentials_ = hasCredentials;
    mode_ = hasCredentials ? WifiMode::Sta : WifiMode::Ap;
    disconnectedSinceMs_ = nowMs;
    wasConnected_ = false;
    connectedSinceMs_ = nowMs;
    return mode_;
}

WifiMode WifiPolicy::update(bool stationConnected, int apStations, std::uint32_t nowMs) {
    if (!hasCredentials_) return mode_ = WifiMode::Ap;
    if (stationConnected) {
        if (!wasConnected_) connectedSinceMs_ = nowMs;
        wasConnected_ = true;
        if (mode_ == WifiMode::ApSta && apStations == 0 && nowMs - connectedSinceMs_ >= timing_.apHoldMs)
            mode_ = WifiMode::Sta;
        return mode_;
    }
    if (wasConnected_) disconnectedSinceMs_ = nowMs;
    wasConnected_ = false;
    if (mode_ == WifiMode::Sta && nowMs - disconnectedSinceMs_ >= timing_.fallbackMs) mode_ = WifiMode::ApSta;
    return mode_;
}

bool WifiScan::start(std::uint32_t nowMs, bool singlePass) {
    if (scanning_) return false;
    scanning_ = true;
    singlePass_ = singlePass;
    passCompleted_ = false;
    startedMs_ = nowMs;
    networks_.clear();
    return true;
}

void WifiScan::add(const WifiNetwork& network) {
    if (network.ssid.empty() || network.ssid.size() > 32) return;
    auto existing = std::find_if(networks_.begin(), networks_.end(),
                                 [&](const WifiNetwork& known) { return known.ssid == network.ssid; });
    if (existing != networks_.end()) {
        if (network.rssi <= existing->rssi) return;
        networks_.erase(existing);
    } else if (networks_.size() >= kMaxNetworks) {
        if (network.rssi <= networks_.back().rssi) return;
        networks_.pop_back();
    }
    const auto position = std::find_if(networks_.begin(), networks_.end(),
                                       [&](const WifiNetwork& known) { return known.rssi < network.rssi; });
    networks_.insert(position, network);
}
