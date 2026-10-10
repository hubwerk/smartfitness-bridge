#pragma once

// Command model shared by the Serial console and the web API. Both front ends
// parse their input (text line or JSON) into a Command; the main loop executes
// it through WebUi::execute(), so the logic and the BridgeCore gating are
// identical for both. Pure C++ (host tested).

#include <cstdint>
#include <string>

#include "ble/ble_types.hpp"
#include "bridge/bridge_core.hpp"

enum class CommandType : std::uint8_t {
    Help,
    Status,
    Simulate,     // connect the simulated device (former "scan")
    Scan,         // BLE scan (falls back to the simulation without BLE input)
    ScanStop,
    ScanResults,
    Pair,         // address [adapter]
    Connect,      // connect the paired device
    Disconnect,
    Unpair,
    Trainer,      // simulation trainer on/off
    Ant,
    FtmsInput,
    FtmsOutput,
    FtmsName,
    FtmsProfile,
    Control,
    Save,
    Load,
    FactoryReset,
    Wifi,         // ssid + password (password may be empty)
    WifiClear,
    ApPassword,
    Restart,
};

struct Command {
    CommandType type = CommandType::Help;
    bool flag = false;
    smartfitness::Control control = smartfitness::Control::Power;
    float value = 0.0F;
    std::string text;   // ftms name/profile, wifi ssid, ap password
    std::string text2;  // wifi password
    smartfitness::ble::BleAddress address;
    bool addressTypeGiven = false;
    std::string adapterId;  // empty: use the detected adapter
    std::uint32_t durationMs = 0;
};

struct CommandResult {
    bool ok = false;
    std::string message;
};

constexpr std::size_t kMaxCommandLength = 96;

// Parses one console line. On failure returns false and sets `error` to the
// message printed on the console.
bool parseConsoleCommand(const std::string& line, Command& command, std::string& error);

// "power" / "speed" / "incline" / "resistance".
bool parseControlName(const std::string& name, smartfitness::Control& control);
const char* controlName(smartfitness::Control control);
const char* controlResultName(smartfitness::ControlResult result);
