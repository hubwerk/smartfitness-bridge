#pragma once

// Centralised HMI strings. English is the default; a second language only
// needs a column in kTable (src/variants/hmi/hmi_strings.cpp). Missing
// translations fall back to English. Note: the built-in LVGL Montserrat fonts
// cover ASCII only, so German umlauts need a font with Latin-1 glyphs
// (see lv_conf.h) before a German table is enabled.

#include <cstdint>

namespace hmi {

enum class Language : std::uint8_t { English, German, Count };

enum class Str : std::uint16_t {
    // Navigation
    AppTitle,
    Dashboard,
    Devices,
    Control,
    Settings,
    Back,
    Menu,
    // Dashboard / states
    StateDisconnected,
    StateConnected,
    StateTrainer,
    StateControlReady,
    NoDevice,
    Simulation,
    FtmsClientConnected,
    FtmsAdvertising,
    FtmsOff,
    FtmsIdle,
    NoTarget,
    Target,
    HintDashboard,
    // Metrics
    MetricPower,
    MetricCadence,
    MetricSpeed,
    MetricHeartRate,
    MetricIncline,
    MetricResistance,
    MetricDistance,
    MetricElapsed,
    // Devices
    Scan,
    StopScan,
    Scanning,
    Connect,
    Disconnect,
    Unpair,
    StartSimulation,
    SimTrainer,
    Paired,
    NotPaired,
    Results,
    NoResults,
    Pair,
    PairAs,
    ChooseAdapter,
    Detected,
    Unknown,
    BleUnavailable,
    ConfirmUnpairTitle,
    ConfirmUnpairText,
    // Control
    ControlUnavailable,
    Apply,
    HintAdjust,
    Unsupported,
    ConfirmSpeedTitle,
    ConfirmSpeedText,
    ConfirmStopText,
    // Settings
    FtmsName,
    FtmsOutput,
    FtmsProfile,
    Ant,
    AntRequiresHardware,
    Brightness,
    ScreenTimeout,
    Never,
    LedRing,
    Save,
    Load,
    FactoryReset,
    Restart,
    Firmware,
    Unsaved,
    SavedState,
    On,
    Off,
    Presets,
    Keyboard,
    Ok,
    Cancel,
    Continue,
    Confirm,
    Erase,
    ConfirmResetTitle,
    ConfirmResetText1,
    ConfirmResetText2,
    ConfirmRestartTitle,
    ConfirmRestartText,
    Count
};

void setLanguage(Language language);
Language language();
// Never returns nullptr.
const char* tr(Str id);

}  // namespace hmi
