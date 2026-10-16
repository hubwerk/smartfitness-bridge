// HMI variant hooks: local LVGL HMI on the CrowPanel 1.28" rotary display
// instead of Wi-Fi + web UI. LVGL runs on the main loop only; UI events only
// queue Commands, which are executed here through the shared
// BridgeController (same Commands as the console and the web API).
#ifdef ARDUINO

#include <Arduino.h>

#include "bridge_controller.h"
#include "board_crowpanel128.h"
#include "config_store.h"
#include "hmi_hal.h"
#include "hmi_model.h"
#include "hmi_ui.h"
#include "lcars_theme.h"
#include "lvgl.h"
#include "platform/ant_uart_serial.h"
#include "variant.h"

namespace {

hmi::CrowPanelHal hal;
hmi::HmiUi ui;
hmi::CommandQueue commands;
hmi::HmiSettings settings;
hmi::RefreshTimer statusTimer(hmi::kStatusRefreshMs);
hmi::RefreshTimer scanTimer(hmi::kScanRefreshMs);
bool ready = false;
// Brightness/timeout/LED changes are written to NVS once the user stops
// changing them, not on every step.
constexpr std::uint32_t kSettingsSaveDelayMs = 2000;
bool settingsDirty = false;
std::uint32_t settingsChangedAt = 0;

void applySettings(const hmi::HmiSettings& next) {
    if (next != settings) {
        settingsDirty = true;
        settingsChangedAt = millis();
    }
    settings = next;
    hal.setBrightness(settings.brightness);
}

}  // namespace

namespace variant {

const char* name() { return "hmi"; }

smartfitness::ant::AntTransport* antTransport() {
#if defined(SF_ANT_UART_TX) && defined(SF_ANT_UART_RX)
    // main starts ANT before hal.begin(); power the external module first.
    pinMode(board::kExt5vEnable, OUTPUT);
    digitalWrite(board::kExt5vEnable, HIGH);
    return &antUartTransport();
#else
    // The CrowPanel's USB-C port is USB-Serial/JTAG, not an ANT USB host.
    return nullptr;
#endif
}

void begin(BridgeController& controller, const BridgeConfig& config) {
    (void)config;  // Wi-Fi fields stay in the shared schema; the HMI never starts Wi-Fi.
    settings = hmi::CrowPanelHal::loadSettings();
    lv_init();
    if (!hal.begin()) {
        Serial.println("[HMI] display init failed; Serial console only");
        return;
    }
    hmi::lcars::init(lv_display_get_default());

    hmi::HmiActions actions;
    actions.submit = [](const Command& command) {
        if (!commands.push(command)) Serial.println("[HMI] command queue full; input dropped");
    };
    actions.settingsChanged = applySettings;
    ui.begin(hal.encoder(), settings, actions);
    ui.setStatus(controller.statusView());
    ui.setScan(controller.scanView());
    ui.setConfig(controller.configView());
    lv_timer_handler();
    hal.setBrightness(settings.brightness);
    ready = true;
    Serial.println("[HMI] CrowPanel 1.28\" HMI ready (knob: turn/press/hold, touch: tap/swipe)");
}

void loop(BridgeController& controller) {
    if (!ready) return;
    Command command;
    while (commands.pop(command)) {
        const CommandResult result = controller.execute(command);
        Serial.printf("[HMI] %s\n", result.message.c_str());
        if (command.type == CommandType::FactoryReset && result.ok) {
            hmi::CrowPanelHal::clearSettings();
            applySettings(hmi::HmiSettings{});
            settingsDirty = false;
            ui.setSettings(settings);
        }
        ui.setConfig(controller.configView());
        ui.showResult(command, result);
        statusTimer.force();
        scanTimer.force();
    }

    const std::uint32_t now = millis();
    if (settingsDirty && now - settingsChangedAt >= kSettingsSaveDelayMs) {
        settingsDirty = false;
        hmi::CrowPanelHal::saveSettings(settings);
    }
    if (statusTimer.due(now)) {
        const auto status = controller.statusView();
        ui.setStatus(status);
        hal.setLedRing(settings.ledRing, hmi::statusColor(status));
    }
    if (scanTimer.due(now)) {
        ui.setConfig(controller.configView());
        if (ui.wantsScan()) ui.setScan(controller.scanView());
    }
    ui.update();
    lv_timer_handler();
    hal.updateScreenTimeout(settings.screenTimeoutS);
}

}  // namespace variant

#endif  // ARDUINO
