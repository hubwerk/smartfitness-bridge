#include "hmi_strings.h"

#include <cstddef>

namespace hmi {
namespace {

struct Entry {
    Str id;
    const char* text[static_cast<std::size_t>(Language::Count)];  // English, German (nullptr: fallback)
};

// Keep in enum order (host tests verify it). German column intentionally
// empty until translations are available; tr() falls back to English.
constexpr Entry kTable[] = {
    {Str::AppTitle, {"SmartFitness", nullptr}},
    {Str::Dashboard, {"Dashboard", nullptr}},
    {Str::Devices, {"Devices", nullptr}},
    {Str::Control, {"Control", nullptr}},
    {Str::Settings, {"Settings", nullptr}},
    {Str::Back, {"Back", nullptr}},
    {Str::Menu, {"Menu", nullptr}},
    {Str::StateDisconnected, {"Disconnected", nullptr}},
    {Str::StateConnected, {"Connected (read-only)", nullptr}},
    {Str::StateTrainer, {"Trainer", nullptr}},
    {Str::StateControlReady, {"Control ready", nullptr}},
    {Str::NoDevice, {"No device", nullptr}},
    {Str::Simulation, {"Simulation", nullptr}},
    {Str::FtmsClientConnected, {"App connected", nullptr}},
    {Str::FtmsAdvertising, {"Advertising", nullptr}},
    {Str::FtmsOff, {"FTMS off", nullptr}},
    {Str::FtmsIdle, {"FTMS idle", nullptr}},
    {Str::NoTarget, {"No target", nullptr}},
    {Str::Target, {"Target", nullptr}},
    {Str::HintDashboard, {"Turn: metric  Press: menu", nullptr}},
    {Str::HintBack, {"Hold knob or swipe right: back", nullptr}},
    {Str::MetricPower, {"Power", nullptr}},
    {Str::MetricCadence, {"Cadence", nullptr}},
    {Str::MetricSpeed, {"Speed", nullptr}},
    {Str::MetricHeartRate, {"Heart rate", nullptr}},
    {Str::MetricIncline, {"Incline", nullptr}},
    {Str::MetricResistance, {"Resistance", nullptr}},
    {Str::MetricDistance, {"Distance", nullptr}},
    {Str::MetricElapsed, {"Time", nullptr}},
    {Str::MetricAverageSpeed, {"Avg speed", nullptr}},
    {Str::MetricAverageCadence, {"Avg cadence", nullptr}},
    {Str::MetricAveragePower, {"Avg power", nullptr}},
    {Str::MetricTotalEnergy, {"Energy", nullptr}},
    {Str::MetricEnergyPerHour, {"Energy/h", nullptr}},
    {Str::MetricEnergyPerMinute, {"Energy/min", nullptr}},
    {Str::MetricMet, {"METs", nullptr}},
    {Str::MetricRemaining, {"Remaining", nullptr}},
    {Str::MetricRampAngle, {"Ramp angle", nullptr}},
    {Str::MetricElevationGain, {"Elevation +", nullptr}},
    {Str::MetricElevationLoss, {"Elevation -", nullptr}},
    {Str::MetricPace, {"Pace", nullptr}},
    {Str::MetricAveragePace, {"Avg pace", nullptr}},
    {Str::MetricStrokes, {"Strokes", nullptr}},
    {Str::MetricSteps, {"Steps", nullptr}},
    {Str::MetricStrides, {"Strides", nullptr}},
    {Str::MetricFloors, {"Floors", nullptr}},
    {Str::MetricBeltForce, {"Belt force", nullptr}},
    {Str::Scan, {"Scan", nullptr}},
    {Str::StopScan, {"Stop scan", nullptr}},
    {Str::Scanning, {"Scanning...", nullptr}},
    {Str::Connect, {"Connect paired", nullptr}},
    {Str::Disconnect, {"Disconnect", nullptr}},
    {Str::Unpair, {"Unpair", nullptr}},
    {Str::StartSimulation, {"Simulation", nullptr}},
    {Str::SimTrainer, {"Sim is trainer", nullptr}},
    {Str::Paired, {"Paired", nullptr}},
    {Str::NotPaired, {"No paired device", nullptr}},
    {Str::Results, {"Results", nullptr}},
    {Str::NoResults, {"No devices found", nullptr}},
    {Str::Pair, {"Pair", nullptr}},
    {Str::PairAs, {"Pair as", nullptr}},
    {Str::ChooseAdapter, {"Choose adapter", nullptr}},
    {Str::Detected, {"detected", nullptr}},
    {Str::Adapter, {"Adapter", nullptr}},
    {Str::Unknown, {"unknown", nullptr}},
    {Str::BleUnavailable, {"BLE input not available", nullptr}},
    {Str::ConfirmUnpairTitle, {"Unpair?", nullptr}},
    {Str::ConfirmUnpairText, {"Forget the paired device.", nullptr}},
    {Str::ControlUnavailable, {"Control not available", nullptr}},
    {Str::Apply, {"Apply", nullptr}},
    {Str::HintAdjust, {"Turn +/-  Press set  Hold back", nullptr}},
    {Str::Unsupported, {"unsupported", nullptr}},
    {Str::StopBelt, {"Stop belt", nullptr}},
    {Str::ConfirmSpeedTitle, {"Set speed?", nullptr}},
    {Str::ConfirmSpeedText, {"The belt will move.\nStand on the side rails.", nullptr}},
    {Str::ConfirmStopText, {"The belt will stop.", nullptr}},
    {Str::FtmsName, {"FTMS name", nullptr}},
    {Str::FtmsOutput, {"FTMS output", nullptr}},
    {Str::FtmsProfile, {"FTMS profile", nullptr}},
    {Str::Ant, {"ANT+", nullptr}},
    {Str::AntRequiresHardware, {"requires hardware", nullptr}},
    {Str::Brightness, {"Brightness", nullptr}},
    {Str::ScreenTimeout, {"Screen off", nullptr}},
    {Str::Never, {"never", nullptr}},
    {Str::LedRing, {"LED ring", nullptr}},
    {Str::Save, {"Save", nullptr}},
    {Str::Load, {"Load saved", nullptr}},
    {Str::FactoryReset, {"Factory reset", nullptr}},
    {Str::Restart, {"Restart", nullptr}},
    {Str::Firmware, {"Firmware", nullptr}},
    {Str::Unsaved, {"unsaved changes", nullptr}},
    {Str::SavedState, {"saved", nullptr}},
    {Str::On, {"on", nullptr}},
    {Str::Off, {"off", nullptr}},
    {Str::Presets, {"Presets", nullptr}},
    {Str::Keyboard, {"Keyboard", nullptr}},
    {Str::Ok, {"OK", nullptr}},
    {Str::Cancel, {"Cancel", nullptr}},
    {Str::Continue, {"Continue", nullptr}},
    {Str::Confirm, {"Confirm", nullptr}},
    {Str::Erase, {"Erase all", nullptr}},
    {Str::ConfirmResetTitle, {"Factory reset?", nullptr}},
    {Str::ConfirmResetText1, {"Erases all settings\nand the pairing.", nullptr}},
    {Str::ConfirmResetText2, {"Really erase\neverything?", nullptr}},
    {Str::ConfirmRestartTitle, {"Restart?", nullptr}},
    {Str::ConfirmRestartText, {"Unsaved changes are lost.", nullptr}},
    {Str::BeltStopped, {"Stopped", nullptr}},
    {Str::BeltStarting, {"Starting", nullptr}},
    {Str::BeltRunning, {"Running", nullptr}},
    {Str::BeltStopping, {"Stopping", nullptr}},
    {Str::BeltStart, {"Start belt", nullptr}},
    {Str::BeltPause, {"Pause belt", nullptr}},
    {Str::OperationMode, {"Operation mode", nullptr}},
};

static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<std::size_t>(Str::Count),
              "every Str needs an entry in kTable");

Language currentLanguage = Language::English;

}  // namespace

void setLanguage(Language value) {
    if (value < Language::Count) currentLanguage = value;
}

Language language() { return currentLanguage; }

const char* tr(Str id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= static_cast<std::size_t>(Str::Count) || kTable[index].id != id) return "?";
    const char* text = kTable[index].text[static_cast<std::size_t>(currentLanguage)];
    return text ? text : kTable[index].text[0];
}

}  // namespace hmi
