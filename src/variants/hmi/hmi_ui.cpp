#include "hmi_ui.h"
#include "hmi_icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ble_input_adapter.h"
#include "ftms_output.h"

using namespace smartfitness;

namespace hmi {
namespace {

HmiUi* g_ui = nullptr;

constexpr lv_coord_t kScreenSize = 240;
constexpr lv_coord_t kListWidth = 174;
constexpr lv_coord_t kListHeight = 136;
constexpr std::uint32_t kToastMs = 2500;
// History chart: as wide as the round panel allows, with the axis labels in
// the chart's own padding so they never overlap the plot.
constexpr lv_coord_t kChartWidth = 208;
constexpr lv_coord_t kChartHeight = 88;
constexpr lv_coord_t kChartTop = 76;
constexpr lv_coord_t kChartPadLeft = 30;
constexpr lv_coord_t kChartPadBottom = 16;
constexpr lv_coord_t kChartPad = 3;
constexpr lv_coord_t kAxisLabelWidth = 48;
// Below the plot's pixel width: LVGL's dense-column renderer can draw
// spurious vertical extrema at gaps. Fewer samples use fewer columns.
constexpr std::size_t kHistoryChartPoints = 145;
// Normalised chart range; the real values are shown on the Y axis labels.
constexpr std::int32_t kChartScale = 1000;

std::string withValue(Str label, const std::string& value) { return std::string(tr(label)) + ": " + value; }

// Compact time-axis label: "M:SS" below an hour, "Hh MM" above.
std::string durationLabel(std::uint32_t milliseconds) {
    const unsigned total = milliseconds / 1000;
    char text[16];
    if (total < 3600) std::snprintf(text, sizeof(text), "%u:%02u", total / 60, total % 60);
    else std::snprintf(text, sizeof(text), "%uh%02u", total / 3600, (total / 60) % 60);
    return text;
}

std::string percent(std::uint8_t value) { return std::to_string(value) + " %"; }

std::uint32_t sportColor(MachineProfile profile) {
    switch (profile) {
        case MachineProfile::Treadmill: return 0xFF912F;
        case MachineProfile::Rower: return 0x35D77A;
        case MachineProfile::CrossTrainer: return 0xAE88FF;
        case MachineProfile::StepClimber: return 0x25C9C0;
        case MachineProfile::StairClimber: return 0xF3BE4F;
        default: return 0x3982F6;
    }
}

const lv_image_dsc_t* sportIcon(MachineProfile profile) {
    switch (profile) {
        case MachineProfile::IndoorBike: return &sf_icon_sport_bike;
        case MachineProfile::Treadmill: return &sf_icon_sport_treadmill;
        case MachineProfile::Rower: return &sf_icon_sport_rower;
        case MachineProfile::CrossTrainer: return &sf_icon_sport_cross_trainer;
        case MachineProfile::StepClimber: return &sf_icon_sport_step_climber;
        case MachineProfile::StairClimber: return &sf_icon_sport_stair_climber;
        default: return &sf_icon_sport_generic;
    }
}

// Keep complete numbers inside their reserved lanes, including wide counters.
void fitText(lv_obj_t* label, const std::string& text, int width, int maxSize) {
    const lv_font_t* fonts[] = {&lv_font_montserrat_40, &lv_font_montserrat_28, &lv_font_montserrat_20,
                               &lv_font_montserrat_16, &lv_font_montserrat_14, &lv_font_montserrat_12};
    const int sizes[] = {40, 28, 20, 16, 14, 12};
    lv_label_set_text(label, text.c_str());
    for (std::size_t i = 0; i < 6; ++i) {
        if (sizes[i] > maxSize) continue;
        lv_point_t size{};
        lv_text_get_size(&size, text.c_str(), fonts[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_style_text_font(label, fonts[i], 0);
        if (size.x <= width) break;
    }
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

std::string compactMetric(const MetricView& sample, Metric metric) {
    if (!sample.valid || !std::isfinite(sample.value)) return "--";
    if (metric == Metric::ElapsedTime || metric == Metric::RemainingTime) return formatMetric(sample, metric);
    const float value = metric == Metric::Distance ? sample.value / 1000.0F : sample.value;
    if (std::fabs(value) < 10000) return formatMetric(sample, metric);
    char text[16];
    std::snprintf(text, sizeof(text), "%.1e", static_cast<double>(value));
    std::string result = text;
    const auto exponent = result.find('e');
    if (exponent != std::string::npos) {
        if (result[exponent + 1] == '+') result.erase(exponent + 1, 1);
        if (result[exponent + 1] == '0') result.erase(exponent + 1, 1);
    }
    return result;
}

Str secondaryName(Metric metric) {
    switch (metric) {
        case Metric::Cadence: return Str::ShortCadence;
        case Metric::Distance: return Str::ShortDistance;
        case Metric::Incline: return Str::ShortIncline;
        case Metric::PositiveElevationGain: return Str::ShortElevation;
        default: return metricFormat(metric).name;
    }
}

bool focusableBeyond(lv_obj_t* item, int step) {
    lv_obj_t* list = lv_obj_get_parent(item);
    const auto count = static_cast<int>(lv_obj_get_child_count(list));
    for (int i = static_cast<int>(lv_obj_get_index(item)) + step; i >= 0 && i < count; i += step) {
        lv_obj_t* sibling = lv_obj_get_child(list, i);
        if (lv_obj_get_group(sibling) && !lv_obj_has_flag(sibling, LV_OBJ_FLAG_HIDDEN)) return true;
    }
    return false;
}

// Labels before the first or after the last focusable entry would otherwise
// stay scrolled out, so the scrollbar never reached the end of its track.
void revealListEnds(lv_obj_t* item) {
    lv_obj_t* list = lv_obj_get_parent(item);
    if (!lv_obj_has_flag(list, LV_OBJ_FLAG_SCROLLABLE)) return;
    lv_area_t content;
    lv_obj_get_content_coords(list, &content);
    lv_area_t area;
    lv_obj_get_coords(item, &area);
    // The top wins: while a list is built its first entry is also the last.
    if (!focusableBeyond(item, 1)) {
        const int32_t dy = std::min(lv_obj_get_scroll_bottom(list), area.y1 - content.y1);
        if (dy > 0) lv_obj_scroll_by(list, 0, -dy, LV_ANIM_OFF);
        lv_obj_get_coords(item, &area);
    }
    if (!focusableBeyond(item, -1)) {
        const int32_t dy = std::min(lv_obj_get_scroll_top(list), content.y2 - area.y2);
        if (dy > 0) lv_obj_scroll_by(list, 0, dy, LV_ANIM_OFF);
    }
}

}  // namespace

void HmiUi::begin(lv_indev_t* encoder, const HmiSettings& settings, HmiActions actions) {
    g_ui = this;
    encoder_ = encoder;
    settings_ = settings;
    actions_ = std::move(actions);
    adapters_ = adapterViews();
    group_ = lv_group_create();
    lv_group_set_wrap(group_, true);
    if (encoder_) {
        lv_indev_set_group(encoder_, group_);
        lv_indev_add_event_cb(encoder_, backOnLongPress, LV_EVENT_LONG_PRESSED, this);
    }

    toast_ = lv_label_create(lv_layer_top());
    lv_obj_set_width(toast_, 176);
    lv_label_set_long_mode(toast_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(toast_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(toast_, &sf_antonio_16, 0);
    lv_obj_set_style_text_color(toast_, lv_color_hex(lcars::LightBlue), 0);
    lv_obj_set_style_bg_opa(toast_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast_, 18, 0);
    lv_obj_set_style_pad_all(toast_, 6, 0);
    lv_obj_align(toast_, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_flag(toast_, LV_OBJ_FLAG_HIDDEN);
    toastTimer_ = lv_timer_create(hideToast, kToastMs, this);
    lv_timer_pause(toastTimer_);

    screen_ = Screen::Dashboard;
    build(false);
}

// --- data -------------------------------------------------------------------

void HmiUi::setStatus(const StatusView& status) {
    if (trainingSource(status_) != trainingSource(status)) {
        metric_ = {};
        metricSelected_ = false;
        if (screen_ == Screen::History) rebuildPending_ = true;
    }
    status_ = status;
    telemetryHistory_.record(status_, lv_tick_get());
    if (screen_ == Screen::Dashboard && dash_.value) updateDashboard();
    if (screen_ == Screen::History && chart_ && lv_tick_get() - chartUpdated_ >= 1000) updateHistory();
}

void HmiUi::setScan(const ScanView& scan) { scan_ = scan; }

void HmiUi::setConfig(const ConfigView& config) { config_ = config; }

void HmiUi::setSettings(const HmiSettings& settings) {
    settings_ = settings;
    if (screen_ == Screen::Settings) rebuildPending_ = true;
}

void HmiUi::showResult(const Command&, const CommandResult& result) {
    toastAlert_ = !result.ok;
    std::string text = result.message.empty() ? std::string(tr(Str::Ok)) : result.message;
    if (text.size() > 120) text = text.substr(0, 117) + "...";
    lv_label_set_text(toast_, lcars::uppercase(text).c_str());
    lv_obj_set_style_bg_color(toast_, lv_color_hex(toastAlert_ ? lcars::Alert : lcars::Lavender), 0);
    lv_obj_remove_flag(toast_, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(toastTimer_);
    lv_timer_resume(toastTimer_);
}

void HmiUi::hideToast(lv_timer_t* timer) {
    auto* ui = static_cast<HmiUi*>(lv_timer_get_user_data(timer));
    lv_obj_add_flag(ui->toast_, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

// --- navigation -------------------------------------------------------------

void HmiUi::navigate(Screen screen) {
    const Screen from = navigatePending_ ? pending_ : screen_;
    if (screen == Screen::Dashboard) {
        history_.clear();
    } else if (screen != from) {
        history_.push_back(from);
    }
    pending_ = screen;
    navigatePending_ = true;
}

void HmiUi::back() {
    if (screen_ == Screen::Confirm) resetSequence_.cancel();
    if (history_.empty()) {
        pending_ = Screen::Dashboard;
    } else {
        pending_ = history_.back();
        history_.pop_back();
    }
    navigatePending_ = true;
}

void HmiUi::returnTo(Screen screen) {
    while (!history_.empty() && history_.back() != screen) history_.pop_back();
    if (!history_.empty()) history_.pop_back();
    pending_ = screen;
    navigatePending_ = true;
}

void HmiUi::openDevice(std::size_t index) {
    if (index >= scan_.devices.size()) return;
    device_ = scan_.devices[index];
    navigate(Screen::DeviceDetail);
}

void HmiUi::openAdjust(Control control) {
    editor_.begin(control, status_);
    navigate(Screen::Adjust);
}

void HmiUi::openFactoryReset() {
    resetSequence_.start();
    ConfirmState state;
    state.title = Str::ConfirmResetTitle;
    state.text = tr(Str::ConfirmResetText1);
    state.confirmLabel = Str::Continue;
    state.returnTo = Screen::Advanced;
    state.action = [this] {
        resetSequence_.confirm();
        ConfirmState second;
        second.title = Str::ConfirmResetTitle;
        second.text = tr(Str::ConfirmResetText2);
        second.confirmLabel = Str::Erase;
        second.returnTo = Screen::Advanced;
        second.action = [this] {
            if (resetSequence_.confirm()) submit(makeCommand(CommandType::FactoryReset));
        };
        confirm(second);
    };
    confirm(state);
}

void HmiUi::openRestart() {
    ConfirmState state;
    state.title = Str::ConfirmRestartTitle;
    state.text = tr(Str::ConfirmRestartText);
    state.confirmLabel = Str::Restart;
    state.returnTo = Screen::Advanced;
    state.action = [this] { submit(makeCommand(CommandType::Restart)); };
    confirm(state);
}

void HmiUi::confirm(ConfirmState state) {
    confirm_ = std::move(state);
    confirmReplaced_ = true;
    // Fresh build (focus back on Cancel) even when a confirmation follows a
    // confirmation; navigate() does not stack the same screen twice.
    navigate(Screen::Confirm);
}

void HmiUi::submit(const Command& command) {
    if (actions_.submit) actions_.submit(command);
}

void HmiUi::changeSettings(const HmiSettings& settings) {
    settings_ = settings;
    if (actions_.settingsChanged) actions_.settingsChanged(settings_);
    rebuildPending_ = true;
}

bool HmiUi::inputBusy() const {
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) return true;
    }
    return false;
}

void HmiUi::backOnLongPress(lv_event_t* event) {
    auto* ui = static_cast<HmiUi*>(lv_event_get_user_data(event));
    // Registered on the encoder only (the event parameter is the focused
    // object, not the indev).
    lv_indev_t* indev = ui->encoder_;
    // Swallow the rest of this press (no click on release, no edit toggle).
    lv_indev_wait_release(indev);
    lv_indev_reset(indev, nullptr);
    if (ui->screen_ != Screen::Dashboard) ui->back();
}

void HmiUi::eventTrampoline(lv_event_t* event) {
    if (!g_ui) return;
    const auto index = reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event));
    if (index < g_ui->callbacks_.size() && g_ui->callbacks_[index]) {
        auto callback = g_ui->callbacks_[index];  // may rebuild the table
        callback(event);
    }
}

void HmiUi::bind(lv_obj_t* obj, lv_event_code_t code, std::function<void(lv_event_t*)> action) {
    callbacks_.push_back(std::move(action));
    lv_obj_add_event_cb(obj, eventTrampoline, code, reinterpret_cast<void*>(callbacks_.size() - 1));
}

// --- rebuild ----------------------------------------------------------------

std::string HmiUi::signature() const {
    std::string sig;
    switch (screen_) {
        case Screen::Devices:
        case Screen::InputServices:
            sig += std::to_string(scan_.scanning) + std::to_string(scan_.bleAvailable) +
                   std::to_string(static_cast<int>(status_.state)) + status_.input.link + status_.input.paired.address +
                   status_.input.paired.adapter + std::to_string(status_.input.bleSelected) +
                   std::to_string(config_.simulatedTrainer);
            for (const auto& device : scan_.devices)
                sig += "|" + device.address + device.name + device.adapter + std::to_string(signalLevel(device.rssi));
            for (const auto& service : status_.input.paired.advertisedServices) sig += "|ADV:" + service;
            for (const auto& sub : status_.input.paired.subscriptions)
                sig += "|SUB:" + sub.service + ":" + sub.characteristic + ":" + sub.state + std::to_string(sub.indicate);
            sig += outputServicesLabel(status_);
            break;
        case Screen::Control:
            sig += std::to_string(static_cast<int>(status_.state)) + std::to_string(status_.canControl) +
                   std::to_string(static_cast<int>(status_.input.profile)) + targetLabel(status_);
            for (const bool supported : status_.controls) sig += supported ? "1" : "0";
            break;
        case Screen::Settings:
        case Screen::Output:
        case Screen::Advanced:
        case Screen::OutputServices:
        case Screen::AntStatus:
            sig += config_.ftmsName + std::to_string(static_cast<int>(config_.outputProtocol)) +
                   std::to_string(static_cast<int>(config_.ftmsOutputProfile)) + std::to_string(config_.antAvailable) +
                   std::to_string(config_.unsaved) + std::to_string(settings_.brightness) +
                   std::to_string(settings_.screenTimeoutS) + std::to_string(settings_.ledRing) +
                   std::to_string(config_.ftmsOutputServices) + ":" + std::to_string(config_.antOutputServices) +
                   std::to_string(status_.ftmsOutput.restartRequired);
            for (const auto& service : status_.ftmsOutput.services)
                sig += "|" + service.id + ":" + service.name + std::to_string(service.enabled) +
                       std::to_string(service.available) + std::to_string(service.active);
            for (const auto& channel : status_.antOutput.channels)
                sig += "|" + channel.id + ":" + channel.name + std::to_string(channel.deviceType) +
                       std::to_string(channel.enabled) + std::to_string(channel.open) +
                       std::to_string(channel.available);
            {
                const auto& ant = status_.antOutput;
                sig += "|ANT:" + std::to_string(ant.available) + std::to_string(ant.enabled) +
                       std::to_string(ant.stick) + std::to_string(ant.open) + ":" + ant.link + ":" +
                       ant.hardware + ":" + ant.error + ":" + std::to_string(ant.deviceNumber) + ":" +
                       std::to_string(ant.channelLimit) + std::to_string(ant.display) +
                       std::to_string(ant.control) + ":" + std::to_string(static_cast<int>(ant.profile));
            }
            break;
        case Screen::Receiver:
            sig = outputLabel(status_) + status_.ftmsOutput.clientAddress +
                  std::to_string(status_.ftmsOutput.control) + std::to_string(status_.antOutput.control);
            break;
        default:
            break;
    }
    return sig;
}

void HmiUi::update() {
    if (header_) {
        const bool alert = screen_ == Screen::Confirm || status_.input.stale ||
                           status_.state == RuntimeState::Disconnected ||
                           (screen_ == Screen::AntStatus &&
                            (!status_.antOutput.available || !status_.antOutput.stick ||
                             (status_.antOutput.enabled && !status_.antOutput.open) ||
                             !status_.antOutput.error.empty())) ||
                           (toastAlert_ && !lv_obj_has_flag(toast_, LV_OBJ_FLAG_HIDDEN));
        lcars::band(header_, alert ? lcars::Alert : lcars::Orange);
    }
    if (inputBusy()) return;
    if (navigatePending_) {
        navigatePending_ = false;
        rebuildPending_ = false;
        screen_ = pending_;
        build(false);
        return;
    }
    if (rebuildPending_ || signature() != builtSignature_) {
        rebuildPending_ = false;
        build(true);
    }
}

void HmiUi::build(bool keepFocus) {
    std::uint32_t focus = 0;
    if (keepFocus) {
        lv_obj_t* focused = lv_group_get_focused(group_);
        for (std::uint32_t index = 0; index < lv_group_get_obj_count(group_); ++index) {
            if (lv_group_get_obj_by_index(group_, index) == focused) focus = index;
        }
    }
    lv_obj_t* old = root_;
    header_ = nullptr;
    defaultFocus_ = 0;
    lv_group_remove_all_objs(group_);
    callbacks_.clear();
    dash_ = {};
    roller_ = rollerInfo_ = adjustValue_ = textArea_ = nullptr;
    chart_ = chartRange_ = chartEmpty_ = chartYMin_ = chartYMax_ = nullptr;
    chartX_ = {};
    series_ = nullptr;
    adjustSegments_ = {};

    switch (screen_) {
        case Screen::Dashboard: buildDashboard(); break;
        case Screen::Menu: buildMenu(); break;
        case Screen::Devices: buildDevices(); break;
        case Screen::DeviceDetail: buildDeviceDetail(); break;
        case Screen::AdapterPicker: buildAdapterPicker(); break;
        case Screen::Control: buildControl(); break;
        case Screen::Adjust: buildAdjust(); break;
        case Screen::Confirm: buildConfirm(); break;
        case Screen::Settings: buildSettings(); break;
        case Screen::InputServices: buildInputServices(); break;
        case Screen::OutputServices: buildOutputServices(); break;
        case Screen::AntStatus: buildAntStatus(); break;
        case Screen::FtmsName: buildFtmsName(); break;
        case Screen::Keyboard: buildKeyboard(); break;
        case Screen::Training: buildTraining(); break;
        case Screen::Connections: buildConnections(); break;
        case Screen::Output: buildOutput(); break;
        case Screen::Receiver: buildReceiver(); break;
        case Screen::Advanced: buildAdvanced(); break;
        case Screen::History: buildHistory(); break;
    }
    builtSignature_ = signature();
    if (!keepFocus) focus = defaultFocus_;

    lv_screen_load(root_);
    if (old) lv_obj_delete(old);

    const std::uint32_t count = lv_group_get_obj_count(group_);
    if (count > 0) lv_group_focus_obj(lv_group_get_obj_by_index(group_, std::min(focus, count - 1)));
    // Dashboard, Adjust and Keyboard take the knob rotation directly.
    lv_group_set_editing(group_, screen_ == Screen::Dashboard || screen_ == Screen::Adjust ||
                                     screen_ == Screen::Keyboard);
}

// --- widgets ----------------------------------------------------------------

lv_obj_t* HmiUi::makeScreen(const std::string& title) {
    root_ = lv_obj_create(nullptr);
    lcars::background(root_);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    if (!title.empty()) header_ = lcars::frame(root_, title, screen_ == Screen::Confirm);
    if (screen_ != Screen::Dashboard) {
        bind(root_, LV_EVENT_GESTURE, [this](lv_event_t*) {
            lv_indev_t* indev = lv_indev_active();
            if (indev && lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
                lv_indev_wait_release(indev);
                back();
            }
        });
    }
    return root_;
}

lv_obj_t* HmiUi::makeList(lv_obj_t* screen) {
    lv_obj_t* list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, kListWidth, kListHeight);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_style_pad_all(list, lcars::FocusRing, 0);
    lv_obj_set_style_clip_corner(list, true, 0);
    lv_obj_set_style_radius(list, 18, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lcars::scrollbar(list);
    return list;
}

lv_obj_t* HmiUi::addButton(lv_obj_t* parent, const std::string& text, std::function<void()> action, bool focusable,
                           const lv_image_dsc_t* icon) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_width(button, lv_pct(100));
    lv_obj_set_height(button, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(button, 36, 0);
    lv_obj_set_style_pad_ver(button, 6, 0);
    lv_obj_set_style_pad_hor(button, 12, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, lcars::uppercase(text).c_str());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(lcars::LightBlue), 0);
    lv_obj_center(label);
    if (icon) {
        // Icon in the widened left padding, clear of the text.
        constexpr lv_coord_t kIconGap = 10;
        const lv_coord_t padLeft = lv_obj_get_style_pad_left(button, LV_PART_MAIN);
        lv_obj_set_style_pad_left(button, padLeft + static_cast<lv_coord_t>(icon->header.w) + kIconGap, 0);
        lv_obj_align(lcars::icon(button, icon, lcars::Blue), LV_ALIGN_LEFT_MID,
                     -static_cast<lv_coord_t>(icon->header.w + kIconGap), 0);
    }
    bind(button, LV_EVENT_FOCUSED, [button](lv_event_t*) {
        lv_obj_update_layout(button);
        lv_anim_delete(lv_obj_get_parent(button), nullptr);
        lv_obj_scroll_to_view_recursive(button, LV_ANIM_OFF);
        revealListEnds(button);
    });
    if (action) {
        bind(button, LV_EVENT_CLICKED, [action](lv_event_t*) { action(); });
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    if (focusable && action) lv_group_add_obj(group_, button);
    return button;
}

lv_obj_t* HmiUi::addLabel(lv_obj_t* parent, const std::string& text, const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, lcars::uppercase(text).c_str());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font ? font : &sf_antonio_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(lcars::LightBlue), 0);
    return label;
}

// A label the knob can stop on. Lists that open with an info block taller than
// the focus ring reveal would otherwise keep that block out of reach, because
// the encoder scrolls a list only by moving the focus between group members.
lv_obj_t* HmiUi::addInfoRow(lv_obj_t* list, const std::string& text, const lv_font_t* font) {
    lv_obj_t* label = addLabel(list, text, font);
    lv_obj_add_flag(label, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    bind(label, LV_EVENT_FOCUSED, [label](lv_event_t*) {
        lv_obj_update_layout(label);
        lv_anim_delete(lv_obj_get_parent(label), nullptr);
        lv_obj_scroll_to_view_recursive(label, LV_ANIM_OFF);
        revealListEnds(label);
    });
    // Keep the default focus on the first real entry.
    if (lv_group_get_obj_count(group_) == defaultFocus_) ++defaultFocus_;
    lv_group_add_obj(group_, label);
    return label;
}

// --- dashboard --------------------------------------------------------------

void HmiUi::buildDashboard() {
    makeScreen("");
    lv_obj_t* pad = lv_obj_create(root_);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, kScreenSize, kScreenSize);
    lv_obj_center(pad);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    // The whole round screen is the only knob target here (turn = metric,
    // press = menu), so it gets no focus ring on the panel edge.
    lv_obj_set_style_radius(pad, LV_RADIUS_CIRCLE, 0);
    lv_group_add_obj(group_, pad);

    dash_.glow = lcars::accentGlow(pad, sportColor(status_.input.profile));
    dash_.segments = lcars::gauge(pad);
    header_ = lcars::frame(pad, profileLabel(status_.input.profile), false);
    lv_obj_set_y(header_, 34);
    dash_.icon = lcars::icon(header_, sportIcon(status_.input.profile), sportColor(status_.input.profile));
    auto label = [pad](const lv_font_t* font, lv_coord_t y, lv_color_t color) {
        lv_obj_t* obj = lv_label_create(pad);
        lv_obj_set_style_text_font(obj, font, 0);
        lv_obj_set_style_text_color(obj, color, 0);
        lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
        return obj;
    };
    const lv_color_t data = lv_color_hex(lcars::LightBlue);
    dash_.state = label(&lv_font_montserrat_12, 62, lv_color_hex(lcars::Muted));
    dash_.name = label(&lv_font_montserrat_12, 81, data);
    dash_.value = label(&lv_font_montserrat_40, 103, data);
    dash_.unit = label(&lv_font_montserrat_12, 146, data);
    dash_.target = label(&sf_antonio_16, 167, lv_color_hex(lcars::Muted));
    dash_.info = label(&lv_font_montserrat_12, 190, lv_color_hex(lcars::Muted));
    lv_obj_set_width(dash_.info, 132);
    lv_label_set_long_mode(dash_.info, LV_LABEL_LONG_WRAP);
    for (std::size_t i = 0; i < dash_.secondary.size(); ++i) {
        auto& side = dash_.secondary[i];
        const int x = i == 0 ? -68 : 68;  // centres x=52 and x=188
        side.name = label(&lv_font_montserrat_12, 94, lv_color_hex(lcars::Muted));
        side.value = label(&lv_font_montserrat_20, 111, data);
        side.unit = label(&lv_font_montserrat_12, 136, lv_color_hex(lcars::Muted));
        lv_obj_align(side.name, LV_ALIGN_TOP_MID, x, 94);
        lv_obj_align(side.value, LV_ALIGN_TOP_MID, x, 111);
        lv_obj_align(side.unit, LV_ALIGN_TOP_MID, x, 136);
    }

    bind(pad, LV_EVENT_KEY, [this](lv_event_t* event) {
        const std::uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) cycleMetric(1);
        if (key == LV_KEY_LEFT || key == LV_KEY_UP) cycleMetric(-1);
    });
    bind(pad, LV_EVENT_CLICKED, [this](lv_event_t*) { navigate(Screen::Menu); });
    bind(pad, LV_EVENT_GESTURE, [this](lv_event_t*) {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
            lv_indev_wait_release(indev);
            cycleMetric(dir == LV_DIR_LEFT ? 1 : -1);
        }
    });
    updateDashboard();
}

void HmiUi::cycleMetric(int detents) {
    metric_.step(status_, detents);
    metricSelected_ = true;
    updateDashboard();
}

void HmiUi::updateDashboard() {
    if (!dash_.value) return;
    const auto profile = status_.input.profile;
    const auto metrics = trainingMetrics(profile);
    Metric metric = metricSelected_ ? metric_.current(status_) : metrics[0];
    if (metric == Metric::Count) metric = metrics[0];
    const auto format = trainingFormat(metric, profile);
    const bool fresh = !status_.input.stale && status_.state != RuntimeState::Disconnected;
    auto sample = status_.metric(metric);
    sample.valid = sample.valid && fresh;
    fitText(dash_.name, tr(format.name), 110, 12);
    fitText(dash_.value, compactMetric(sample, metric), 80, 40);
    fitText(dash_.unit, format.unit, 80, 12);
    lv_obj_update_layout(dash_.value);
    lv_obj_align(dash_.value, LV_ALIGN_TOP_MID, 0, 123 - lv_obj_get_height(dash_.value) / 2);
    float ratio = 0.0F;
    if (sample.valid && std::isfinite(sample.value) && format.gaugeMax > format.gaugeMin) {
        const float value = metric == Metric::Distance ? sample.value / 1000.0F : sample.value;
        ratio = (value - format.gaugeMin) / (format.gaugeMax - format.gaugeMin);
    }
    const auto accent = sportColor(profile);
    lcars::setGauge(dash_.segments, ratio, false, accent);
    lv_obj_set_style_text_color(dash_.value, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_color(dash_.glow, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_grad_color(dash_.glow, lv_color_hex(accent), 0);
    placeHeaderIcon(profile, accent);
    fitText(dash_.state, tr(status_.input.stale ? Str::Stale : stateLabel(status_.state)), 156, 12);
    std::size_t sideIndex = 0;
    for (const auto secondary : metrics) {
        if (secondary == metric || sideIndex == dash_.secondary.size()) continue;
        auto& side = dash_.secondary[sideIndex++];
        auto value = status_.metric(secondary);
        value.valid = value.valid && fresh;
        fitText(side.name, tr(secondaryName(secondary)), 44, 12);
        fitText(side.value, compactMetric(value, secondary), 46, 20);
        fitText(side.unit, trainingFormat(secondary, profile).unit, 44, 12);
    }
    const auto& elapsed = status_.metric(Metric::ElapsedTime);
    const std::string time = elapsed.valid && fresh ? formatMetric(elapsed, Metric::ElapsedTime) : "--";
    fitText(dash_.target, std::string(tr(Str::MetricElapsed)) + "  " + time, 132, 14);
    if (metric == Metric::ElapsedTime) lv_obj_add_flag(dash_.target, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(dash_.target, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(dash_.info, tr(Str::HintDashboard));
}

// Sport icon and name share the header band as one centred group.
void HmiUi::placeHeaderIcon(MachineProfile profile, std::uint32_t accent) {
    constexpr lv_coord_t kGap = 5;
    constexpr lv_coord_t kInset = 10;
    const lv_image_dsc_t* source = sportIcon(profile);
    lv_image_set_src(dash_.icon, source);
    lv_obj_set_style_image_recolor(dash_.icon, lv_color_hex(accent), 0);
    lv_obj_t* label = lv_obj_get_child(header_, 0);
    const std::string text = lcars::uppercase(profileLabel(profile));
    lv_label_set_text(label, text.c_str());
    const lv_coord_t band = lv_obj_get_width(header_) - 2 * kInset;
    auto widthOf = [&](const lv_font_t* f) { return lv_text_get_width(text.c_str(), text.size(), f, 0); };
    // Prefer icon + Antonio 20, then icon + 16; drop the icon before truncating the name.
    lv_coord_t iconWidth = static_cast<lv_coord_t>(source->header.w);
    const lv_font_t* font = &sf_antonio_20;
    if (widthOf(font) > band - iconWidth - kGap) {
        font = &sf_antonio_16;
        if (widthOf(font) > band - iconWidth - kGap) iconWidth = 0;
    }
    if (iconWidth == 0 && widthOf(&sf_antonio_20) <= band) font = &sf_antonio_20;
    const lv_coord_t gap = iconWidth ? kGap : 0;
    if (iconWidth) lv_obj_remove_flag(dash_.icon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(dash_.icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(label, font, 0);
    const lv_coord_t textWidth = std::min<lv_coord_t>(band - iconWidth - gap, widthOf(font));
    lv_obj_set_size(label, textWidth, lv_font_get_line_height(font));
    const lv_coord_t left = (lv_obj_get_width(header_) - iconWidth - gap - textWidth) / 2;
    lv_obj_align(dash_.icon, LV_ALIGN_LEFT_MID, left, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, left + iconWidth + gap, font == &sf_antonio_20 ? -1 : 0);
}

// --- menu -------------------------------------------------------------------

void HmiUi::buildMenu() {
    makeScreen(tr(Str::Menu));
    lv_obj_t* list = makeList(root_);
    addButton(list, tr(Str::Training), [this] { navigate(Screen::Training); }, true, &sf_icon_nav_training);
    addButton(list, tr(Str::Connections), [this] { navigate(Screen::Connections); }, true,
              &sf_icon_nav_connections);
    addButton(list, tr(Str::Settings), [this] { navigate(Screen::Settings); }, true, &sf_icon_nav_system);
    // Back from the top level returns to the live values, never to a deeper screen.
    addButton(list, tr(Str::Back), [this] { navigate(Screen::Dashboard); }, true, &sf_icon_nav_back);
}

void HmiUi::buildTraining() {
    makeScreen(tr(Str::Training));
    auto* list = makeList(root_);
    addButton(list, tr(Str::Dashboard), [this] { navigate(Screen::Dashboard); });
    addButton(list, tr(Str::Control), [this] { navigate(Screen::Control); });
    addButton(list, tr(Str::History), [this] { navigate(Screen::History); });
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildConnections() {
    makeScreen(tr(Str::Connections));
    auto* list = makeList(root_);
    addButton(list, tr(Str::Devices), [this] { navigate(Screen::Devices); });
    addButton(list, tr(Str::OutputProtocol), [this] { navigate(Screen::Output); });
    addButton(list, tr(Str::Receiver), [this] { navigate(Screen::Receiver); });
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildReceiver() {
    makeScreen(tr(Str::Receiver));
    auto* list = makeList(root_);
    addButton(list, outputLabel(status_), [] {});
    if (status_.outputProtocol == OutputProtocol::BleFtms && status_.ftmsOutput.client) {
        addButton(list, status_.ftmsOutput.clientAddress.empty() ? tr(Str::Unknown) :
                  status_.ftmsOutput.clientAddress, [] {});
        addButton(list, withValue(Str::AntControlStatus, onOff(status_.ftmsOutput.control)), [] {});
    }
    if (status_.outputProtocol == OutputProtocol::AntFec)
        addButton(list, tr(Str::AntStatus), [this] { navigate(Screen::AntStatus); });
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildHistory() {
    makeScreen(tr(Str::History));
    const auto metric = trainingMetrics(status_.input.profile)[0];
    const auto format = trainingFormat(metric, status_.input.profile);
    // Y-axis caption: the plotted metric with its unit.
    std::string caption = tr(format.name);
    if (format.unit[0] != '\0') caption += " (" + std::string(format.unit) + ")";
    auto* title = addLabel(root_, caption, &lv_font_montserrat_12);
    lv_obj_set_width(title, 180);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    chart_ = lv_chart_create(root_);
    lv_obj_set_size(chart_, kChartWidth, kChartHeight);
    lv_obj_align(chart_, LV_ALIGN_TOP_MID, 0, kChartTop);
    lv_obj_set_style_bg_color(chart_, lv_color_hex(lcars::Lavender), 0);
    lv_obj_set_style_border_width(chart_, 0, 0);
    lv_obj_set_style_pad_all(chart_, kChartPad, 0);
    // Axis labels live in the padding, outside the plotted area.
    lv_obj_set_style_pad_left(chart_, kChartPadLeft, 0);
    lv_obj_set_style_pad_bottom(chart_, kChartPadBottom, 0);
    lv_obj_set_style_line_color(chart_, lv_color_hex(lcars::Border), 0);
    lv_obj_set_style_size(chart_, 2, 2, LV_PART_INDICATOR);
    lv_obj_remove_flag(chart_, LV_OBJ_FLAG_SCROLLABLE);
    lv_chart_set_div_line_count(chart_, 3, 3);
    lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, 0, kChartScale);
    lv_chart_set_point_count(chart_, 2);
    series_ = lv_chart_add_series(chart_, lv_color_hex(sportColor(status_.input.profile)), LV_CHART_AXIS_PRIMARY_Y);

    auto axisLabel = [this](lv_align_t align, lv_coord_t x, lv_coord_t y, lv_coord_t width,
                            lv_text_align_t textAlign) {
        lv_obj_t* label = lv_label_create(chart_);
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(lcars::Muted), 0);
        lv_obj_set_style_text_align(label, textAlign, 0);
        lv_obj_align(label, align, x, y);
        lv_label_set_text(label, "");
        return label;
    };
    // Negative offsets reach out of the content area into the chart padding.
    chartYMax_ = axisLabel(LV_ALIGN_TOP_LEFT, -kChartPadLeft, 0, kChartPadLeft - 4, LV_TEXT_ALIGN_RIGHT);
    chartYMin_ = axisLabel(LV_ALIGN_BOTTOM_LEFT, -kChartPadLeft, -2, kChartPadLeft - 4, LV_TEXT_ALIGN_RIGHT);
    chartX_[0] = axisLabel(LV_ALIGN_BOTTOM_LEFT, 0, kChartPadBottom - 1, kAxisLabelWidth, LV_TEXT_ALIGN_LEFT);
    chartX_[1] = axisLabel(LV_ALIGN_BOTTOM_MID, 0, kChartPadBottom - 1, kAxisLabelWidth, LV_TEXT_ALIGN_CENTER);
    chartX_[2] = axisLabel(LV_ALIGN_BOTTOM_RIGHT, 0, kChartPadBottom - 1, kAxisLabelWidth, LV_TEXT_ALIGN_RIGHT);

    chartEmpty_ = lv_label_create(chart_);
    lv_obj_set_style_text_font(chartEmpty_, &lv_font_montserrat_12, 0);
    lv_label_set_text(chartEmpty_, tr(Str::HistoryEmpty));
    lv_obj_center(chartEmpty_);

    // X-axis caption: the session covered by the plot.
    chartRange_ = addLabel(root_, "", &lv_font_montserrat_12);
    lv_obj_set_width(chartRange_, 164);
    lv_obj_align(chartRange_, LV_ALIGN_TOP_MID, 0, 166);
    auto* button = addButton(root_, tr(Str::Back), [this] { back(); });
    lv_obj_set_width(button, 100);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 182);
    updateHistory();
}

void HmiUi::updateHistory() {
    if (!chart_ || !series_) return;
    chartUpdated_ = lv_tick_get();
    const std::size_t count = telemetryHistory_.size();
    float minimum = 0, maximum = 0;
    bool any = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& point = telemetryHistory_.at(i);
        if (!point.valid) continue;
        minimum = any ? std::min(minimum, point.value) : point.value;
        maximum = any ? std::max(maximum, point.value) : point.value;
        any = true;
    }
    // One column per sample until the plot is full; the X axis always spans
    // the complete session, so it is rescaled on every refresh.
    const auto points = static_cast<std::uint32_t>(
        std::min(std::max<std::size_t>(count, 2), kHistoryChartPoints));
    if (lv_chart_get_point_count(chart_) != points) lv_chart_set_point_count(chart_, points);
    lv_chart_set_all_value(chart_, series_, LV_CHART_POINT_NONE);
    auto* values = lv_chart_get_y_array(chart_, series_);

    std::array<float, kHistoryChartPoints> sums{};
    std::array<std::uint8_t, kHistoryChartPoints> samples{};
    std::array<bool, kHistoryChartPoints> gaps{};
    const std::uint32_t span = telemetryHistory_.span();
    const std::uint32_t first = count ? telemetryHistory_.at(0).time : 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& point = telemetryHistory_.at(i);
        const auto column = static_cast<std::uint32_t>(
            span > 0 ? static_cast<std::uint64_t>(point.time - first) * (points - 1) / span
                     : (count > 1 ? i * (points - 1) / (count - 1) : 0));
        if (column >= points) continue;
        if (!point.valid) {
            gaps[column] = true;
            continue;
        }
        sums[column] += point.value;
        if (samples[column] < 255) ++samples[column];
    }
    for (std::uint32_t column = 0; column < points; ++column) {
        if (gaps[column] || samples[column] == 0) continue;
        const double value = static_cast<double>(sums[column]) / samples[column];
        values[column] = maximum > minimum
            ? static_cast<std::int32_t>((value - minimum) * kChartScale / (maximum - minimum))
            : kChartScale / 2;
    }
    if (any) lv_obj_add_flag(chartEmpty_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(chartEmpty_, LV_OBJ_FLAG_HIDDEN);

    const auto format = trainingFormat(trainingMetrics(status_.input.profile)[0], status_.input.profile);
    lv_label_set_text(chartYMax_, any ? formatValue(maximum, format.decimals).c_str() : "--");
    lv_label_set_text(chartYMin_, any ? formatValue(minimum, format.decimals).c_str() : "--");
    for (std::size_t i = 0; i < chartX_.size(); ++i)
        lv_label_set_text(chartX_[i], durationLabel(static_cast<std::uint32_t>(span * i / 2)).c_str());
    fitText(chartRange_, std::string(tr(Str::HistorySession)) + " " + durationLabel(span), 164, 12);
    lv_chart_refresh(chart_);
}

// --- devices ----------------------------------------------------------------

void HmiUi::buildDevices() {
    makeScreen(tr(Str::Devices));
    lv_obj_t* list = makeList(root_);
    std::string paired = tr(Str::NotPaired);
    if (status_.input.paired.valid) {
        paired = std::string(tr(Str::Paired)) + ": " +
                 (status_.input.paired.name.empty() ? status_.input.paired.address : status_.input.paired.name);
        if (!status_.input.paired.adapter.empty()) paired += " (" + status_.input.paired.adapter + ")";
    }
    addLabel(list, paired + "\n" + inputLabel(status_) + " - " + tr(stateLabel(status_.state)));
    if (status_.input.paired.valid)
        lcars::pill(addButton(list, tr(Str::InputServices), [this] { navigate(Screen::InputServices); }), lcars::Violet);
    if (!scan_.bleAvailable) addLabel(list, tr(Str::BleUnavailable));

    if (scan_.scanning) {
        lcars::pill(addButton(list, tr(Str::StopScan), [this] { submit(makeCommand(CommandType::ScanStop)); }), lcars::Alert);
    } else {
        addButton(list, tr(Str::Scan), [this] {
            Command scan = makeCommand(CommandType::Scan);
            scan.durationMs = BleInputAdapter::kDefaultScanMs;
            submit(scan);
        });
    }
    if (status_.input.paired.valid) {
        addButton(list, tr(Str::Connect), [this] { submit(makeCommand(CommandType::Connect)); });
    }
    addButton(list, tr(Str::Disconnect), [this] { submit(makeCommand(CommandType::Disconnect)); });
    if (status_.input.paired.valid) {
        addButton(list, tr(Str::Unpair), [this] {
            ConfirmState state;
            state.title = Str::ConfirmUnpairTitle;
            state.text = tr(Str::ConfirmUnpairText);
            state.confirmLabel = Str::Unpair;
            state.returnTo = Screen::Devices;
            state.action = [this] { submit(makeCommand(CommandType::Unpair)); };
            confirm(state);
        });
    }

    std::string results = std::string(tr(Str::Results)) + " (" + std::to_string(scan_.devices.size()) + ")";
    if (scan_.scanning) results += " - " + std::string(tr(Str::Scanning));
    addLabel(list, results, &sf_antonio_20);
    if (scan_.devices.empty()) addLabel(list, tr(Str::NoResults));
    for (std::size_t index = 0; index < scan_.devices.size(); ++index) {
        const auto& dev = scan_.devices[index];
        const std::uint32_t color = scanEntryMuted(dev) ? lcars::Gray : lcars::Violet;
        lv_obj_t* entry = addButton(list, scanEntryLabel(dev), [this, index] { openDevice(index); });
        lcars::pill(entry, color);
        // Signal pictogram in the widened left padding, clear of the text.
        constexpr lv_coord_t kIconGap = 6;
        const lv_coord_t padLeft = lv_obj_get_style_pad_left(entry, LV_PART_MAIN);
        lv_obj_set_style_pad_left(entry, padLeft + lcars::SignalBarsWidth + kIconGap, 0);
        lv_obj_align(lcars::signalBars(entry, signalLevel(dev.rssi), lcars::LightBlue), LV_ALIGN_LEFT_MID,
                     -(lcars::SignalBarsWidth + kIconGap), 0);
    }
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildDeviceDetail() {
    const std::string title = !device_.label.empty() ? device_.label : (device_.name.empty() ? device_.address : device_.name);
    makeScreen(title);
    lv_obj_t* list = makeList(root_);
    bool head = true;
    auto info = [&](const std::string& text) {
        lv_obj_t* row = head ? addInfoRow(list, text) : addLabel(list, text);
        head = false;
        return row;
    };
    if (!device_.name.empty()) info(device_.name);
    info(device_.address);
    lcars::signalBars(list, signalLevel(device_.rssi), lcars::LightBlue);
    info(std::string(tr(Str::Adapter)) + ": " +
         (device_.adapter.empty() ? std::string(tr(Str::Unknown))
                                  : device_.adapter + " (" + tr(Str::Detected) + ")"));
    if (!device_.services.empty()) {
        lv_obj_t* adv = info(advertisedServicesLabel(device_.services));
        lv_obj_set_style_text_color(adv, lv_color_hex(lcars::Muted), 0);
    }
    if (!device_.adapter.empty()) {
        addButton(list, tr(Str::Pair), [this] {
            Command pair;
            if (makePair(device_, "", pair)) submit(pair);
            returnTo(Screen::Devices);
        });
    }
    addButton(list, std::string(tr(Str::PairAs)) + "...", [this] { navigate(Screen::AdapterPicker); });
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildAdapterPicker() {
    makeScreen(tr(Str::ChooseAdapter));
    std::string options;
    std::uint32_t selected = 0;
    for (std::size_t index = 0; index < adapters_.size(); ++index) {
        if (index) options += "\n";
        options += lcars::uppercase(adapters_[index].id);
        if (device_.adapter == adapters_[index].id) selected = static_cast<std::uint32_t>(index);
    }
    roller_ = lv_roller_create(root_);
    lv_roller_set_options(roller_, options.c_str(), LV_ROLLER_MODE_NORMAL);
    lv_obj_set_width(roller_, 176);
    lv_obj_set_style_text_font(roller_, &sf_antonio_16, 0);
    // Odd row count: the neighbours above and below are shown in full.
    lv_obj_set_style_text_line_space(roller_, 4, 0);
    lv_roller_set_visible_row_count(roller_, 3);
    lv_obj_align(roller_, LV_ALIGN_TOP_MID, 0, 60 + lcars::FocusRing);
    lv_roller_set_selected(roller_, selected, LV_ANIM_OFF);
    lcars::scrollbar(roller_);
    lv_group_add_obj(group_, roller_);

    rollerInfo_ = lv_label_create(root_);
    lv_obj_set_width(rollerInfo_, 170);
    lv_label_set_long_mode(rollerInfo_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(rollerInfo_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(rollerInfo_, &sf_antonio_16, 0);
    lv_obj_align_to(rollerInfo_, roller_, LV_ALIGN_OUT_BOTTOM_MID, 0, lcars::FocusRing);
    auto showInfo = [this] {
        const auto index = lv_roller_get_selected(roller_);
        if (index < adapters_.size()) {
            const auto& adapter = adapters_[index];
            const std::string text = std::string(adapter.label) + " - " + profileLabel(adapter.profile) +
                                     (adapter.controllable ? "" : " (read-only)");
            lv_label_set_text(rollerInfo_, lcars::uppercase(text).c_str());
        }
    };
    showInfo();
    bind(roller_, LV_EVENT_VALUE_CHANGED, [showInfo](lv_event_t*) { showInfo(); });

    lv_obj_t* row = lv_obj_create(root_);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 140 + 2 * lcars::FocusRing, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, lcars::FocusRing, 0);
    lv_obj_align_to(row, rollerInfo_, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t* pair = addButton(row, tr(Str::Pair), [this] {
        const auto index = lv_roller_get_selected(roller_);
        Command command;
        if (index < adapters_.size() && makePair(device_, adapters_[index].id, command)) submit(command);
        returnTo(Screen::Devices);
    });
    lv_obj_set_flex_grow(pair, 1);
    lv_obj_t* cancel = addButton(row, tr(Str::Back), [this] { back(); });
    lv_obj_set_flex_grow(cancel, 1);
}

// --- control ----------------------------------------------------------------

void HmiUi::buildControl() {
    makeScreen(tr(Str::Control));
    lv_obj_t* list = makeList(root_);
    std::string state = std::string(tr(stateLabel(status_.state))) + "\n" + targetLabel(status_);
    if (!status_.canControl) state += "\n" + std::string(tr(Str::ControlUnavailable));
    addLabel(list, state);
    for (const Control control : controlOrder(status_.input.profile)) {
        const ControlSpec& spec = controlSpec(control);
        std::string text = tr(spec.name);
        if (!status_.supports(control)) {
            text += " - " + std::string(tr(Str::Unsupported));
            // Focusable read-only row: an unreachable entry would leave the
            // list stuck for the knob, which only scrolls by moving the focus.
            lv_obj_t* row = addButton(list, text, [] {});
            lcars::pill(row, lcars::Gray);
            lv_obj_set_style_text_color(lv_obj_get_child(row, 0), lv_color_hex(lcars::Muted), 0);
            continue;
        }
        if (status_.hasTarget && status_.target.control == control)
            text += ": " + formatValue(status_.target.value, spec.decimals) + " " + spec.unit;
        addButton(list, text, [this, control] { openAdjust(control); });
    }
    if (status_.supports(Control::Speed)) {
        // Stopping never needs a confirmation.
        lcars::pill(addButton(list, tr(Str::StopBelt), [this] {
            Command stop = makeCommand(CommandType::Control);
            stop.control = Control::Speed;
            stop.value = 0.0F;
            submit(stop);
        }), lcars::Alert);
    }
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildAdjust() {
    const ControlSpec& spec = controlSpec(editor_.control());
    makeScreen(tr(spec.name));
    lv_obj_set_y(header_, 34);
    adjustSegments_ = lcars::gauge(root_);
    for (auto* segment : adjustSegments_) lv_obj_move_to_index(segment, 0);

    lv_obj_t* pad = lv_obj_create(root_);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 112, 84);
    lv_obj_align(pad, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(pad, 12, 0);
    lv_obj_set_style_border_color(pad, lv_color_white(), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(pad, 2, LV_STATE_FOCUSED);
    lv_group_add_obj(group_, pad);

    adjustValue_ = lv_label_create(pad);
    lv_obj_set_style_text_font(adjustValue_, &sf_antonio_48, 0);
    lv_obj_align(adjustValue_, LV_ALIGN_CENTER, 0, -10);
    lv_obj_t* unit = lv_label_create(pad);
    lv_label_set_text(unit, lcars::uppercase(spec.unit).c_str());
    lv_obj_align(unit, LV_ALIGN_BOTTOM_MID, 0, -4);

    bind(pad, LV_EVENT_KEY, [this](lv_event_t* event) {
        const std::uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_RIGHT || key == LV_KEY_UP) editor_.step(1);
        if (key == LV_KEY_LEFT || key == LV_KEY_DOWN) editor_.step(-1);
        updateAdjust();
    });
    bind(pad, LV_EVENT_CLICKED, [this](lv_event_t*) { applyAdjust(); });

    // Touch helpers (not in the knob group).
    auto stepButton = [this](const char* text, lv_align_t align, lv_coord_t x, int detents) {
        lv_obj_t* button = lv_button_create(root_);
        lv_obj_set_size(button, 44, 44);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(button, align, x, -18);
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(lcars::LightBlue), 0);
        lv_obj_center(label);
        bind(button, LV_EVENT_CLICKED, [this, detents](lv_event_t*) {
            editor_.step(detents);
            updateAdjust();
        });
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    };
    // Inside the gauge ring and clear of the value pad.
    stepButton("-", LV_ALIGN_LEFT_MID, 19, -1);
    stepButton("+", LV_ALIGN_RIGHT_MID, -19, 1);

    lv_obj_t* hint = lv_label_create(root_);
    lv_label_set_text(hint, lcars::uppercase(tr(Str::HintAdjust)).c_str());
    lv_obj_set_style_text_font(hint, &sf_antonio_16, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(lcars::Muted), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 147);

    lv_obj_t* row = lv_obj_create(root_);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 136, 36);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_grow(addButton(row, tr(Str::Apply), [this] { applyAdjust(); }, false), 1);
    lv_obj_set_flex_grow(addButton(row, tr(Str::Back), [this] { back(); }, false), 1);
    updateAdjust();
}

void HmiUi::updateAdjust() {
    if (adjustValue_) {
        lv_label_set_text(adjustValue_, editor_.valueText().c_str());
        const auto& spec = controlSpec(editor_.control());
        lcars::setGauge(adjustSegments_, (editor_.value() - spec.minimum) / (spec.maximum - spec.minimum));
    }
}

void HmiUi::applyAdjust() {
    const Command command = editor_.command();
    if (!editor_.requiresConfirmation()) {
        submit(command);
        returnTo(Screen::Control);
        return;
    }
    ConfirmState state;
    state.title = Str::ConfirmSpeedTitle;
    state.text = editor_.valueText() + " " + controlSpec(editor_.control()).unit + "\n" +
                 tr(command.value > 0.0F ? Str::ConfirmSpeedText : Str::ConfirmStopText);
    state.confirmLabel = Str::Confirm;
    state.returnTo = Screen::Control;
    state.action = [this, command] { submit(command); };
    confirm(state);
}

// --- confirm ----------------------------------------------------------------

void HmiUi::buildConfirm() {
    makeScreen(tr(confirm_.title));
    lv_obj_t* list = makeList(root_);
    addLabel(list, confirm_.text, &sf_antonio_16);
    // Cancel first: it gets the default focus.
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, lcars::FocusRing, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 2 * lcars::FocusRing, 0);
    lv_obj_t* cancel = addButton(row, tr(Str::Cancel), [this] {
        resetSequence_.cancel();
        returnTo(confirm_.returnTo);
    });
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* ok = addButton(row, tr(confirm_.confirmLabel), [this] {
        ConfirmState state = confirm_;
        confirmReplaced_ = false;
        if (state.action) state.action();
        if (!confirmReplaced_) returnTo(state.returnTo);
    });
    lv_obj_set_flex_grow(ok, 1);
    lcars::pill(ok, lcars::Alert);
}

// --- settings ---------------------------------------------------------------

void HmiUi::buildSettings() {
    makeScreen(tr(Str::Settings));
    lv_obj_t* list = makeList(root_);
    addLabel(list, tr(config_.unsaved ? Str::Unsaved : Str::SavedState));
    addButton(list, withValue(Str::Firmware, config_.version), [] {});
    addButton(list, withValue(Str::Brightness, percent(settings_.brightness)), [this] {
        HmiSettings next = settings_;
        next.brightness = nextBrightness(settings_.brightness);
        changeSettings(next);
    });
    addButton(list, withValue(Str::ScreenTimeout, screenTimeoutLabel(settings_.screenTimeoutS)), [this] {
        HmiSettings next = settings_;
        next.screenTimeoutS = nextScreenTimeout(settings_.screenTimeoutS);
        changeSettings(next);
    });
    addButton(list, withValue(Str::LedRing, onOff(settings_.ledRing)), [this] {
        HmiSettings next = settings_;
        next.ledRing = !settings_.ledRing;
        changeSettings(next);
    });
    addButton(list, tr(Str::Save), [this] { submit(makeCommand(CommandType::Save)); });
    addButton(list, tr(Str::Advanced), [this] { navigate(Screen::Advanced); });
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildOutput() {
    makeScreen(tr(Str::OutputProtocol));
    auto* list = makeList(root_);
    addButton(list, withValue(Str::FtmsName, config_.ftmsName), [this] { navigate(Screen::FtmsName); });
    addButton(list, withValue(Str::OutputProtocol, outputProtocolLabel(config_.outputProtocol)), [this] {
        submit(makeText(CommandType::OutputProtocol,
                        outputProtocolName(nextOutputProtocol(config_.outputProtocol, status_.antOutput.available))));
    });
    addButton(list, withValue(Str::FtmsProfile, ftmsProfileLabel(config_.ftmsOutputProfile)), [this] {
        submit(makeText(CommandType::FtmsProfile, ftmsOutputProfileName(nextFtmsProfile(config_.ftmsOutputProfile))));
    });
    addButton(list, tr(Str::OutputServices), [this] { navigate(Screen::OutputServices); });
    if (status_.antOutput.available)
        addButton(list, tr(Str::AntStatus), [this] { navigate(Screen::AntStatus); });
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildAdvanced() {
    makeScreen(tr(Str::Advanced));
    auto* list = makeList(root_);
    addButton(list, tr(Str::StartSimulation), [this] { submit(makeCommand(CommandType::Simulate)); });
    addButton(list, withValue(Str::SimTrainer, onOff(config_.simulatedTrainer)),
              [this] { submit(makeFlag(CommandType::Trainer, !config_.simulatedTrainer)); });
    addButton(list, tr(Str::Load), [this] { submit(makeCommand(CommandType::Load)); });
    lcars::pill(addButton(list, tr(Str::FactoryReset), [this] { openFactoryReset(); }), lcars::Alert);
    addButton(list, tr(Str::Restart), [this] { openRestart(); });
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildInputServices() {
    makeScreen(tr(Str::InputServices));
    lv_obj_t* list = makeList(root_);
    const auto& paired = status_.input.paired;
    // Read-only pills remain focusable, making every wrapped row reachable by knob.
    if (paired.advertisedServices.empty()) addLabel(list, "ADV -");
    for (const auto& uuid : paired.advertisedServices)
        lcars::pill(addButton(list, "ADV " + serviceUuidLabel(uuid), [] {}), lcars::Violet);
    if (paired.subscriptions.empty()) addLabel(list, "SUB -");
    for (const auto& sub : paired.subscriptions) {
        const Str state = subscriptionStateLabel(sub.state);
        const auto color = state == Str::ServiceOk ? lcars::Golden :
                           state == Str::ServiceFail ? lcars::Alert : lcars::Tan;
        lcars::pill(addButton(list, subscriptionLabel(sub), [] {}), color);
    }
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildOutputServices() {
    makeScreen(tr(Str::OutputServices));
    lv_obj_t* list = makeList(root_);
    if (status_.ftmsOutput.restartRequired) addLabel(list, tr(Str::RestartRequired));
    addLabel(list, tr(Str::BleRestartHint));
    lcars::pill(addButton(list, std::string("BLE ") + tr(Str::ServiceAuto) + "\n" +
                         onOff(config_.ftmsOutputServices == 0), [this] {
        Command command = makeText(CommandType::SetOutputServices, "ftms");
        command.serviceMask = 0;
        submit(command);
    }), config_.ftmsOutputServices == 0 ? lcars::Golden : lcars::Tan);
    constexpr const char* ids[] = {"ftms", "cps", "hr", "csc", "rsc"};
    constexpr const char* names[] = {"FTMS", "CPS", "HR", "CSC", "RSC"};
    for (std::uint8_t index = 0; index < 5; ++index) {
        const auto bit = static_cast<std::uint8_t>(1U << index);
        const bool enabled = index == 0 || (config_.ftmsOutputServices & bit) != 0;
        bool available = index == 0;
        bool active = false;
        for (const auto& service : status_.ftmsOutput.services) {
            if (service.id == ids[index]) {
                available = service.available;
                active = service.active;
            }
        }
        std::string text = std::string("BLE ") + names[index] + " " + onOff(enabled) + "\n";
        text += tr(index == 0 ? Str::ServiceMandatory :
                   active ? Str::ServiceActive : available ? Str::ServiceAvailable : Str::ServiceNoData);
        lcars::pill(addButton(list, text, [this, bit, index, enabled, available] {
            if (index != 0 && (enabled || available))
                submit(toggleOutputService("ftms", config_.ftmsOutputServices, bit));
        }), enabled ? (available ? lcars::Golden : lcars::Alert) : lcars::Tan);
    }
    if (status_.antOutput.available) {
        lcars::pill(addButton(list, std::string("ANT+ ") + tr(Str::ServiceAuto) + "\n" +
                             onOff(config_.antOutputServices == 0), [this] {
            Command command = makeText(CommandType::SetOutputServices, "ant");
            command.serviceMask = 0;
            submit(command);
        }), config_.antOutputServices == 0 ? lcars::Golden : lcars::Tan);
        constexpr const char* antIds[] = {"fec", "power", "hr", "speed-cadence", "sdm"};
        const char* antNames[] = {"FE-C", tr(Str::AntPower), "HR", tr(Str::AntSpeedCadence), tr(Str::AntFootpod)};
        const auto mask = config_.antOutputServices == 0 ? 1 : config_.antOutputServices;
        for (std::uint8_t index = 0; index < 5; ++index) {
            const auto bit = static_cast<std::uint8_t>(1U << index);
            const bool enabled = (mask & bit) != 0;
            bool available = index == 0;
            bool open = false;
            for (const auto& channel : status_.antOutput.channels) {
                if (channel.id == antIds[index]) {
                    available = channel.available;
                    open = channel.open;
                }
            }
            std::string text = std::string("ANT+ ") + antNames[index] + " " + onOff(enabled) + "\n";
            text += tr(open ? Str::AntBroadcasting : available ? Str::ServiceAvailable : Str::ServiceNoData);
            lcars::pill(addButton(list, text, [this, bit, enabled, available] {
                if (!enabled && !available) return;
                const auto command = toggleOutputService("ant", config_.antOutputServices, bit);
                if (command.serviceMask == 0)
                    showResult(command, {false, tr(Str::AntSelectChannel)});
                else
                    submit(command);
            }), enabled ? (available ? lcars::Golden : lcars::Alert) : lcars::Tan);
        }
        if ((mask & 3) == 3) {
            auto* warning = addButton(list, tr(Str::AntDuplicatePower), [] {});
            lcars::pill(warning, lcars::Alert);
            lv_obj_set_style_bg_color(warning, lv_color_hex(lcars::Alert), LV_STATE_FOCUSED);
        }
        addButton(list, tr(Str::AntStatus), [this] { navigate(Screen::AntStatus); });
    } else {
        addLabel(list, tr(Str::AntUnavailable));
    }
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildAntStatus() {
    makeScreen(tr(Str::AntStatus));
    lv_obj_t* list = makeList(root_);
    const auto& ant = status_.antOutput;
    const Str state = !ant.available ? Str::AntUnavailable :
                      !ant.stick ? Str::AntNoModule :
                      !ant.enabled ? Str::OutputOff :
                      !ant.open ? Str::AntWaiting : Str::AntBroadcasting;
    // Focusable readouts let the encoder reach every diagnostic without touch.
    auto readout = [this, list](const std::string& text, bool alert = false) {
        auto* row = addButton(list, text, [] {});
        lcars::pill(row, alert ? lcars::Alert : lcars::Lavender);
        if (alert) lv_obj_set_style_bg_color(row, lv_color_hex(lcars::Alert), LV_STATE_FOCUSED);
    };
    std::string stateText = tr(state);
    if (ant.stick) stateText += "\n" + std::string(tr(Str::AntDetected));
    readout(stateText, state == Str::AntNoModule || state == Str::AntWaiting ||
                       state == Str::AntUnavailable || !ant.error.empty());
    readout(withValue(Str::AntLink, ant.link.empty() ? "-" : ant.link));
    readout(withValue(Str::AntHardware, ant.hardware.empty() ? "-" : ant.hardware));
    readout(withValue(Str::AntDeviceNumber, std::to_string(ant.deviceNumber)));
    std::size_t open = 0;
    for (const auto& channel : ant.channels)
        if (channel.open) ++open;
    readout(withValue(Str::AntChannels, std::to_string(open) + "/" + std::to_string(ant.channelLimit)));
    for (const auto& channel : ant.channels) {
        readout(channel.name + " " + onOff(channel.enabled) + "\n" +
                tr(channel.open ? Str::AntBroadcasting : Str::AntClosed) + " / " +
                tr(channel.available ? Str::ServiceAvailable : Str::ServiceNoData));
    }
    readout(withValue(Str::AntDisplayStatus, onOff(ant.display)));
    readout(withValue(Str::AntControlStatus, onOff(ant.control)));
    if (!ant.error.empty()) readout(ant.error);
    addButton(list, tr(Str::Back), [this] { back(); });
}

void HmiUi::buildFtmsName() {
    makeScreen(tr(Str::FtmsName));
    lv_obj_t* list = makeList(root_);
    addLabel(list, tr(Str::Presets));
    std::uint32_t current = 0;
    const auto& presets = ftmsNamePresets();
    for (std::size_t index = 0; index < presets.size(); ++index) {
        const std::string& name = presets[index];
        if (name == config_.ftmsName) current = static_cast<std::uint32_t>(index);
        addButton(list, (name == config_.ftmsName ? std::string(LV_SYMBOL_OK " ") : std::string()) + name, [this, name] {
            submit(makeText(CommandType::FtmsName, name));
            returnTo(Screen::Output);
        });
    }
    addButton(list, std::string(LV_SYMBOL_KEYBOARD " ") + tr(Str::Keyboard), [this] { navigate(Screen::Keyboard); });
    addButton(list, tr(Str::Back), [this] { back(); });
    defaultFocus_ = current;
}

void HmiUi::buildKeyboard() {
    makeScreen(tr(Str::FtmsName));
    textArea_ = lv_textarea_create(root_);
    lv_textarea_set_one_line(textArea_, true);
    lv_textarea_set_max_length(textArea_, static_cast<std::uint32_t>(FtmsOutput::kMaxNameLength));
    lv_textarea_set_text(textArea_, config_.ftmsName.c_str());
    lv_obj_set_width(textArea_, 176);
    lv_obj_set_style_pad_ver(textArea_, 4, 0);
    lv_obj_set_style_pad_hor(textArea_, 14, 0);
    lv_obj_align(textArea_, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t* keyboard = lv_keyboard_create(root_);
    lv_obj_set_size(keyboard, 168, 100);
    lv_obj_align_to(keyboard, textArea_, LV_ALIGN_OUT_BOTTOM_MID, 0, 2 + lcars::FocusRing);
    lv_obj_set_style_text_font(keyboard, &sf_antonio_16, 0);
    lv_obj_set_style_pad_all(keyboard, 2, 0);
    lv_obj_set_style_pad_gap(keyboard, 2, 0);
    lv_keyboard_set_textarea(keyboard, textArea_);
    lv_group_add_obj(group_, keyboard);
    bind(keyboard, LV_EVENT_READY, [this](lv_event_t*) {
        submit(makeText(CommandType::FtmsName, lv_textarea_get_text(textArea_)));
        returnTo(Screen::Output);
    });
    bind(keyboard, LV_EVENT_CANCEL, [this](lv_event_t*) { back(); });
}

}  // namespace hmi
