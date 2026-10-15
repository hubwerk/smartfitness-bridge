#pragma once

// Web variant: JSON rendering of the shared view model (status, scan, config)
// and Wi-Fi network scans for the REST API and the WebSocket push. Pure C++ (host tested).

#include <string>

#include "bridge_view_model.h"
#include "wifi_policy.h"

// Network state reported by the Wi-Fi manager (firmware) for status views.
struct NetworkStatus {
    std::string mode = "off";  // off | ap | sta | ap+sta
    std::string ssid;          // connected network or SoftAP name
    std::string ip;
    std::string hostname;
    int stations = 0;          // SoftAP clients
};

namespace web {

std::string statusJson(const smartfitness::StatusView& status, const NetworkStatus& network);
std::string scanResultsJson(const smartfitness::ScanView& scan);
std::string configJson(const smartfitness::ConfigView& config);
// {"type":"wifi-scan","scanning":bool,"networks":[{"ssid","rssi","secure"}]}
std::string wifiScanJson(const WifiScan& scan);

}  // namespace web
