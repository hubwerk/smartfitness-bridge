#include "hmi_strings.h"

#include <cstddef>

namespace hmi {
namespace {

struct Entry {
    Str id;
    const char* text[static_cast<std::size_t>(Language::Count)];  // English, German (nullptr: fallback)
};

// Keep in enum order (host tests verify it). Untranslated German entries
// fall back to English.
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
    {Str::OutputOff, {"Output off", nullptr}},
    {Str::AntDisplay, {"ANT+ link", "ANT+ verbunden"}},
    {Str::AntOpen, {"ANT+ on", "ANT+ an"}},
    {Str::AntNoStick, {"No ANT+ stick", "Kein ANT+ Modul"}},
    {Str::AntUnavailable, {"No ANT+ radio", "Kein ANT+ Funk"}},
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
    {Str::OutputProtocol, {"Output", nullptr}},
    {Str::FtmsProfile, {"Output profile", nullptr}},
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
    {Str::InputServices, {"Input services", "Eingangsdienste"}},
    {Str::OutputServices, {"Output services", "Ausgangsdienste"}},
    {Str::NoServices, {"None reported", "Keine gemeldet"}},
    {Str::ServiceOk, {"OK", "OK"}},
    {Str::ServiceFail, {"FAIL", "FEHLER"}},
    {Str::ServicePending, {"PENDING", "AUSSTEHEND"}},
    {Str::ServiceAvailable, {"available", "verfügbar"}},
    {Str::ServiceNoData, {"no data", "keine Daten"}},
    {Str::ServiceActive, {"active", "aktiv"}},
    {Str::ServiceMandatory, {"required", "Pflicht"}},
    {Str::ServiceAuto, {"Auto: baseline only", "Auto: nur Basis"}},
    {Str::BleRestartHint, {"BLE: save + restart to apply", "BLE: speichern + neu starten"}},
    {Str::RestartRequired, {"Save + restart BLE", "Speichern + BLE-Neustart"}},
    {Str::AntStatus, {"ANT+ status", "ANT+ Status"}},
    {Str::AntLink, {"Link", "Verbindung"}},
    {Str::AntHardware, {"Hardware", "Hardware"}},
    {Str::AntDeviceNumber, {"Device number", "Gerätenummer"}},
    {Str::AntChannels, {"Channels", "Kanäle"}},
    {Str::AntDisplayStatus, {"Display", "Anzeige"}},
    {Str::AntControlStatus, {"Control", "Steuerung"}},
    {Str::AntNoModule, {"No ANT+ module", "Kein ANT+ Modul"}},
    {Str::AntWaiting, {"Waiting for ANT+", "Warte auf ANT+"}},
    {Str::AntBroadcasting, {"Broadcasting", "Sendet"}},
    {Str::AntClosed, {"Closed", "Geschlossen"}},
    {Str::AntDetected, {"Module detected", "Modul erkannt"}},
    {Str::AntDuplicatePower, {"FE-C + Power duplicates power.\nPair only one power source.",
                              "FE-C + Power doppelt.\nNur eine Leistungsquelle koppeln."}},
    {Str::AntSelectChannel, {"Select a channel or choose Auto.", "Kanal oder Auto wählen."}},
    {Str::AntPower, {"Power", "Leistung"}},
    {Str::AntSpeedCadence, {"Speed / cadence", "Tempo / Trittfrequenz"}},
    {Str::AntFootpod, {"Footpod", "Laufsensor"}},
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
