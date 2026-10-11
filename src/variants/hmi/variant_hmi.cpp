// HMI variant hooks: local LVGL HMI on the CrowPanel 1.28" rotary display
// instead of Wi-Fi + web UI. LVGL runs on the main loop only; UI events only
// queue Commands, which are executed here through the shared
// BridgeController (same Commands as the console and the web API).
#ifdef ARDUINO

#include <Arduino.h>

#include "bridge_controller.h"
#include "config_store.h"
#include "hmi_hal.h"
#include "hmi_model.h"
#include "hmi_ui.h"
#include "lvgl.h"
#include "variant.h"

namespace {

hmi::CrowPanelHal hal;
hmi::HmiUi ui;
hmi::CommandQueue commands;
hmi::HmiSettings settings;
hmi::RefreshTimer statusTimer(hmi::kStatusRefreshMs);
hmi::RefreshTimer scanTimer(hmi::kScanRefreshMs);
bool ready = false;

void applySettings(const hmi::HmiSettings& next) {
    settings = next;
    hal.setBrightness(settings.brightness);
    hmi::CrowPanelHal::saveSettings(settings);
}

}  // namespace

namespace variant {

const char* name() { return "hmi"; }

void begin(BridgeController& controller, const BridgeConfig& config) {
    (void)config;  // Wi-Fi fields stay in the shared schema; the HMI never starts Wi-Fi.
    settings = hmi::CrowPanelHal::loadSettings();
    lv_init();
    if (!hal.begin()) {
        Serial.println("[HMI] display init failed; Serial console only");
        return;
    }
    lv_theme_t* theme = lv_theme_default_init(lv_display_get_default(), lv_palette_main(LV_PALETTE_BLUE),
                                              lv_palette_main(LV_PALETTE_TEAL), true, &lv_font_montserrat_14);
    lv_display_set_theme(lv_display_get_default(), theme);

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
            ui.setSettings(settings);
        }
        ui.setConfig(controller.configView());
        ui.showResult(command, result);
        statusTimer.force();
        scanTimer.force();
    }

    const std::uint32_t now = millis();
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
