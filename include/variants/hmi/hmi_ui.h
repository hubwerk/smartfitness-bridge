#pragma once

// LVGL screens of the HMI variant (round 240x240, rotary knob + touch).
// Board independent: used by the CrowPanel firmware and by the host
// simulator (sim/). The UI never calls into the bridge core directly; it
// reads view-model snapshots (setStatus/setScan/setConfig) and hands
// Commands to HmiActions::submit, which the variant queues and executes on
// the main loop through BridgeController::execute.
//
// Knob: turn = move focus / change value, press = select, hold = back.
// Touch: tap = select, swipe right = back, swipe left/right on the
// dashboard = next/previous metric.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hmi_model.h"
#include "lcars_theme.h"
#include "lvgl.h"

namespace hmi {

enum class Screen : std::uint8_t {
    Dashboard,
    Menu,
    Devices,
    DeviceDetail,
    AdapterPicker,
    Control,
    Adjust,
    Confirm,
    Settings,
    FtmsName,
    Keyboard,
};

struct HmiActions {
    std::function<void(const Command&)> submit;
    std::function<void(const HmiSettings&)> settingsChanged;
};

class HmiUi {
public:
    // `encoder` may be null (touch only); it gets the UI's group.
    void begin(lv_indev_t* encoder, const HmiSettings& settings, HmiActions actions);

    void setStatus(const smartfitness::StatusView& status);
    void setScan(const smartfitness::ScanView& scan);
    void setConfig(const smartfitness::ConfigView& config);
    void setSettings(const HmiSettings& settings);
    // Toast with the CommandResult (rejection reasons from the core).
    void showResult(const Command& command, const CommandResult& result);

    // Applies deferred navigation / rebuilds. Call once per loop before
    // lv_timer_handler(); never from inside an LVGL event.
    void update();

    Screen current() const { return screen_; }
    // True while scan results are visible (refresh them at 1 Hz).
    bool wantsScan() const { return screen_ == Screen::Devices || screen_ == Screen::DeviceDetail; }

    // Navigation entry points (also used by the simulator for screenshots).
    void navigate(Screen screen);
    void back();
    void openDevice(std::size_t index);
    void openAdjust(smartfitness::Control control);
    void openFactoryReset();
    void openRestart();

private:
    struct ConfirmState {
        Str title = Str::Confirm;
        std::string text;
        Str confirmLabel = Str::Confirm;
        Screen returnTo = Screen::Dashboard;
        std::function<void()> action;
    };

    static void eventTrampoline(lv_event_t* event);
    static void backOnLongPress(lv_event_t* event);
    static void hideToast(lv_timer_t* timer);

    void build(bool keepFocus);
    lv_obj_t* makeScreen(const std::string& title);
    lv_obj_t* makeList(lv_obj_t* screen);
    lv_obj_t* addButton(lv_obj_t* parent, const std::string& text, std::function<void()> action, bool focusable = true);
    lv_obj_t* addLabel(lv_obj_t* parent, const std::string& text, const lv_font_t* font = nullptr);
    void bind(lv_obj_t* obj, lv_event_code_t code, std::function<void(lv_event_t*)> action);
    void returnTo(Screen screen);

    void buildDashboard();
    void buildMenu();
    void buildDevices();
    void buildDeviceDetail();
    void buildAdapterPicker();
    void buildControl();
    void buildAdjust();
    void buildConfirm();
    void buildSettings();
    void buildFtmsName();
    void buildKeyboard();

    void updateDashboard();
    void updateAdjust();
    void applyAdjust();
    void confirm(ConfirmState state);
    void submit(const Command& command);
    void changeSettings(const HmiSettings& settings);
    void cycleMetric(int detents);
    std::string signature() const;
    bool inputBusy() const;

    lv_group_t* group_ = nullptr;
    lv_indev_t* encoder_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* toast_ = nullptr;
    lv_timer_t* toastTimer_ = nullptr;
    HmiActions actions_;
    HmiSettings settings_;
    smartfitness::StatusView status_;
    smartfitness::ScanView scan_;
    smartfitness::ConfigView config_;

    Screen screen_ = Screen::Dashboard;
    Screen pending_ = Screen::Dashboard;
    bool navigatePending_ = false;
    bool rebuildPending_ = false;
    std::string builtSignature_;
    std::uint32_t defaultFocus_ = 0;  // focus index after a fresh build
    std::vector<Screen> history_;
    std::vector<std::function<void(lv_event_t*)>> callbacks_;

    MetricCycler metric_;
    struct DashboardWidgets {
        lcars::Gauge segments{};
        lv_obj_t* name = nullptr;
        lv_obj_t* value = nullptr;
        lv_obj_t* unit = nullptr;
        lv_obj_t* state = nullptr;
        lv_obj_t* target = nullptr;
        lv_obj_t* info = nullptr;
    } dash_;

    smartfitness::ScanDeviceView device_;
    std::vector<smartfitness::AdapterView> adapters_;
    lv_obj_t* roller_ = nullptr;
    lv_obj_t* rollerInfo_ = nullptr;

    TargetEditor editor_;
    lv_obj_t* adjustValue_ = nullptr;
    lcars::Gauge adjustSegments_{};

    ConfirmState confirm_;
    bool confirmReplaced_ = false;
    ConfirmSequence resetSequence_{2};
    lv_obj_t* textArea_ = nullptr;
};

}  // namespace hmi
