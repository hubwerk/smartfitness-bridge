#pragma once

// Wi-Fi + HTTPS/WebSocket front end (firmware only).
//  * Wi-Fi: SoftAP "SmartFitness-XXXX" (WPA2) with captive portal when no
//    credentials are stored, station mode otherwise with AP fallback
//    (WifiPolicy). mDNS "smartfitness.local".
//  * ESP-IDF esp_https_server on port 443 with the device's self-signed,
//    never-expiring certificate (tls_cert.h) serves the UI, REST API and
//    wss://.../ws. Port 80 only redirects to HTTPS (captive portal included).
//  * Handlers run on the HTTPS server task and only read snapshots or
//    enqueue requests in ApiExchange; the main loop executes them through
//    BridgeController and publishes fresh snapshots via loop(). WebSocket
//    broadcasts are queued onto the server task (httpd_queue_work).
//  * Wi-Fi network scan (POST /api/wifi-scan): the main loop runs async scan
//    passes for WifiScan::kDurationMs and publishes the merged networks.

#ifdef ARDUINO

#include <atomic>
#include <cstdint>
#include <string>

#include "web_api.h"
#include "bridge_controller.h"
#include "web_json.h"
#include "wifi_policy.h"

class WebPortal {
public:
    static constexpr const char* kHostname = "smartfitness";
    static constexpr std::uint16_t kHttpsPort = 443;
    static constexpr std::uint16_t kHttpPort = 80;  // redirects to HTTPS
    static constexpr std::uint32_t kStatusIntervalMs = 250;  // WebSocket push <= 4 Hz
    static constexpr std::uint32_t kScanIntervalMs = 1000;
    static constexpr std::uint32_t kWifiScanRetryMs = 500;

    WebPortal(BridgeController& ui, ApiExchange& api);
    // Starts Wi-Fi and the server; credentials come from the configuration.
    void begin(const std::string& ssid, const std::string& password, const std::string& apPassword);
    // Main loop: run queued API jobs, publish snapshots, Wi-Fi policy, DNS.
    void loop();

    const std::string& apSsid() const { return apSsid_; }

private:
    struct Impl;
    void applyMode(WifiMode mode);
    void publish(std::uint32_t now);
    NetworkStatus networkStatus() const;
    void beginStation();
    void updateWifiScan(std::uint32_t now);

    BridgeController& ui_;
    ApiExchange& api_;
    Impl* impl_ = nullptr;
    WifiPolicy policy_;
    WifiMode applied_ = WifiMode::Ap;
    bool started_ = false;
    std::string ssid_;
    std::string password_;
    std::string apSsid_;
    std::string apPassword_;
    std::uint32_t lastStatusMs_ = 0;
    std::uint32_t lastScanMs_ = 0;
    std::uint32_t lastPolicyMs_ = 0;
    std::string lastScanJson_;
    std::string lastConfigJson_;
    std::atomic<bool> wifiScanRequested_{false};
    WifiScan wifiScan_;
    std::uint32_t lastWifiScanAttemptMs_ = 0;
};

#endif  // ARDUINO
