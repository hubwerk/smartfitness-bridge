#pragma once
#include <cstdint>
#include <string>
#include "bridge/bridge_core.hpp"
#include "bridge_commands.h"
#include "bridge_view_model.h"
#include "config_store.h"

class SimulatedDeviceAdapter;
class AntOutput;
class FtmsOutput;
class BleInputAdapter;
class InputSelector;

#ifndef SMARTFITNESS_VERSION
#define SMARTFITNESS_VERSION "0.3.0"
#endif

// Variant-neutral front-end controller: Serial console and shared command
// executor. Console lines, web API requests (web variant) and HMI actions
// (HMI variant) are all turned into Commands and run through execute() on the
// main loop, so validation and BridgeCore gating are identical for every
// front end. The *View() methods publish the view model rendered by the
// variants (JSON in the web variant, LVGL screens in the HMI variant).
class BridgeController {
public:
    BridgeController(smartfitness::BridgeCore& bridge, ConfigStore& config, SimulatedDeviceAdapter& adapter,
                     AntOutput& ant, FtmsOutput& ftms);
    // Enables real BLE input. BridgeCore must be attached to `selector`.
    void attachBleInput(InputSelector& selector, BleInputAdapter& ble);
    void begin();
    // Polls the Serial console (firmware only).
    void loop();
    std::string handleCommand(const std::string& command);
    CommandResult execute(const Command& command);
    std::string status() const;

    // View model snapshots (built on the main loop).
    smartfitness::StatusView statusView() const;
    smartfitness::ScanView scanView() const;
    smartfitness::ConfigView configView() const;

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
    bool restartRequested_ = false;
    std::string input_;
    bool overflow_ = false;
};
