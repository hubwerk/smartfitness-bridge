#pragma once
#include <cstdint>
#include <string>
#include "bridge/bridge_core.hpp"
#include "bridge_commands.h"
#include "config_store.h"

class SimulatedDeviceAdapter;
class AntOutput;
class FtmsOutput;
class BleInputAdapter;
class InputSelector;

#ifndef SMARTFITNESS_VERSION
#define SMARTFITNESS_VERSION "0.3.0"
#endif

// Network state reported by the Wi-Fi manager (firmware) for status views.
struct NetworkStatus {
    std::string mode = "off";  // off | ap | sta | ap+sta
    std::string ssid;          // connected network or SoftAP name
    std::string ip;
    std::string hostname;
    int stations = 0;          // SoftAP clients
};

// Serial console and shared command executor. Console lines and web API
// requests are both turned into Commands and run through execute() on the main
// loop, so validation and BridgeCore gating are identical for both front ends.
class WebUi {
public:
    WebUi(smartfitness::BridgeCore& bridge, ConfigStore& config, SimulatedDeviceAdapter& adapter,
          AntOutput& ant, FtmsOutput& ftms);
    // Enables real BLE input. BridgeCore must be attached to `selector`.
    void attachBleInput(InputSelector& selector, BleInputAdapter& ble);
    void begin();
    void loop();
    std::string handleCommand(const std::string& command);
    CommandResult execute(const Command& command);
    std::string status() const;

    // JSON views for the web API (built on the main loop).
    std::string statusJson() const;
    std::string scanResultsJson() const;
    std::string configJson() const;

    void setNetworkStatus(const NetworkStatus& status) { network_ = status; }
    const NetworkStatus& networkStatus() const { return network_; }
    // Set by "restart" or a saved Wi-Fi change; the firmware reboots when idle.
    bool restartRequested() const { return restartRequested_; }

private:
    void applyConfig();
    bool bleSelected() const;
    CommandResult pair(const Command& command);
    smartfitness::BridgeCore& bridge_;
    ConfigStore& config_;
    SimulatedDeviceAdapter& adapter_;
    AntOutput& ant_;
    FtmsOutput& ftms_;
    InputSelector* selector_ = nullptr;
    BleInputAdapter* ble_ = nullptr;
    NetworkStatus network_;
    bool restartRequested_ = false;
    std::string input_;
    bool overflow_ = false;
};
