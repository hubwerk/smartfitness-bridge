// Headless LVGL simulator of the HMI variant.
//
// Renders every HMI screen from fake view-model data into an in-memory
// 240x240 frame buffer and writes PNG screenshots (the round panel outline
// is drawn as a mask). Before taking the screenshots it drives the UI with a
// virtual rotary encoder only (turn / press / hold) and checks that the
// expected Commands are produced, i.e. that the HMI is operable with the
// knob alone and never starts a treadmill without confirmation.
//
//   make hmi-screenshots LVGL_DIR=<lvgl v9.2.2 checkout>
//   build/hmi-sim <output dir>

#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "hmi_strings.h"
#include "hmi_ui.h"
#include "lvgl.h"

using namespace smartfitness;
using namespace hmi;

namespace {

constexpr int kSize = 240;
std::uint32_t g_now = 0;
std::vector<std::uint16_t> g_frame(kSize * kSize);

// --- virtual devices ----------------------------------------------------------

struct VirtualEncoder {
    int diff = 0;
    bool pressed = false;
} g_encoder;

void encoderRead(lv_indev_t*, lv_indev_data_t* data) {
    data->enc_diff = static_cast<std::int16_t>(g_encoder.diff);
    g_encoder.diff = 0;
    data->state = g_encoder.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* pixels) {
    const auto* source = reinterpret_cast<const std::uint16_t*>(pixels);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) g_frame[static_cast<std::size_t>(y * kSize + x)] = *source++;
    lv_display_flush_ready(display);
}

std::uint32_t tick() { return g_now; }

HmiUi g_ui;
std::vector<Command> g_commands;

void pump(std::uint32_t ms) {
    for (std::uint32_t t = 0; t < ms; t += 5) {
        g_now += 5;
        g_ui.update();
        lv_timer_handler();
    }
}

void turn(int detents) {
    g_encoder.diff += detents;
    pump(80);
}

void press() {
    g_encoder.pressed = true;
    pump(80);
    g_encoder.pressed = false;
    pump(120);
}

void hold() {
    g_encoder.pressed = true;
    pump(700);
    g_encoder.pressed = false;
    pump(120);
}

// --- PNG ----------------------------------------------------------------------

std::uint32_t crc32(const std::uint8_t* data, std::size_t length, std::uint32_t crc = 0) {
    crc = ~crc;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()));
}

// Uncompressed (stored) zlib stream: no zlib dependency needed.
std::vector<std::uint8_t> zlibStored(const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> out{0x78, 0x01};
    std::size_t offset = 0;
    do {
        const std::size_t length = std::min<std::size_t>(65535, raw.size() - offset);
        const bool last = offset + length == raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(length & 0xFF));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        out.push_back(static_cast<std::uint8_t>(~length & 0xFF));
        out.push_back(static_cast<std::uint8_t>((~length >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                   raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    } while (offset < raw.size());
    std::uint32_t a = 1, b = 0;
    for (const auto byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    put32(out, (b << 16) | a);
    return out;
}

void writePng(const std::string& path) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(kSize * (kSize * 3 + 1)));
    const float radius = kSize / 2.0F;
    for (int y = 0; y < kSize; ++y) {
        raw.push_back(0);  // filter: none
        for (int x = 0; x < kSize; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - radius;
            const float dy = static_cast<float>(y) + 0.5F - radius;
            if (std::sqrt(dx * dx + dy * dy) > radius) {
                // Outside the round panel.
                raw.insert(raw.end(), {48, 48, 48});
                continue;
            }
            const std::uint16_t pixel = g_frame[static_cast<std::size_t>(y * kSize + x)];
            raw.push_back(static_cast<std::uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31));
            raw.push_back(static_cast<std::uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63));
            raw.push_back(static_cast<std::uint8_t>((pixel & 0x1F) * 255 / 31));
        }
    }
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    put32(header, kSize);
    put32(header, kSize);
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8 bit RGB
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", zlibStored(raw));
    chunk(png, "IEND", {});
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(1);
    }
    std::fwrite(png.data(), 1, png.size(), file);
    std::fclose(file);
    std::printf("wrote %s\n", path.c_str());
}

std::string g_outDir = "screenshots";

void shot(const char* name) {
    pump(400);  // let animations (scroll on focus) settle
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
    writePng(g_outDir + "/" + name + ".png");
}

// --- fake data ---------------------------------------------------------------

void setMetric(StatusView& status, Metric metric, float value) {
    status.telemetry[static_cast<std::size_t>(metric)] = {true, value};
}

StatusView bikeStatus() {
    StatusView status;
    status.version = "0.3.0";
    status.state = RuntimeState::ControlReady;
    status.deviceName = "KICKR CORE";
    status.trainer = true;
    status.canControl = true;
    status.controls = {false, false, true, true};  // Speed, Incline, Resistance, Power
    setMetric(status, Metric::Power, 182.0F);
    setMetric(status, Metric::Cadence, 88.0F);
    setMetric(status, Metric::Speed, 31.4F);
    setMetric(status, Metric::HeartRate, 132.0F);
    status.hasTarget = true;
    status.target = {Control::Power, 180.0F};
    status.input.bleSelected = true;
    status.input.bleAvailable = true;
    status.input.profile = MachineProfile::IndoorBike;
    status.input.link = "ready";
    status.input.paired = {true, "aa:00:00:00:00:21", false, "KICKR CORE", "ftms"};
    status.ftmsOutput = {true, MachineProfile::IndoorBike, FtmsOutputProfile::Auto, "SmartFitness Bridge", true, true, true};
    return status;
}

StatusView treadmillStatus() {
    StatusView status;
    status.version = "0.3.0";
    status.state = RuntimeState::ControlReady;
    status.deviceName = "WalkingPad";
    status.trainer = true;
    status.canControl = true;
    status.controls = {true, true, false, false};
    setMetric(status, Metric::Speed, 3.0F);
    setMetric(status, Metric::Incline, 1.5F);
    setMetric(status, Metric::HeartRate, 104.0F);
    status.input.bleSelected = true;
    status.input.bleAvailable = true;
    status.input.profile = MachineProfile::Treadmill;
    status.input.link = "ready";
    status.input.paired = {true, "aa:00:00:00:00:22", false, "WalkingPad", "walkingpad-r1"};
    status.ftmsOutput = {true, MachineProfile::Treadmill, FtmsOutputProfile::Auto, "SmartFitness Bridge", true, false, false};
    return status;
}

ScanView scanView() {
    ScanView scan;
    scan.scanning = true;
    scan.bleAvailable = true;
    auto device = [](const char* address, const char* name, int rssi, const char* adapter) {
        ScanDeviceView view;
        view.address = address;
        view.name = name;
        view.rssi = rssi;
        view.adapter = adapter;
        return view;
    };
    scan.devices = {device("aa:00:00:00:00:22", "WalkingPad", -58, "walkingpad-r1"),
                    device("aa:00:00:00:00:21", "KICKR CORE", -64, "ftms"),
                    device("aa:00:00:00:00:30", "I_TL", -71, "ifit-treadmill"),
                    device("aa:00:00:00:00:31", "", -83, "")};
    return scan;
}

ConfigView configView() {
    ConfigView config;
    config.version = "0.3.0";
    config.ftmsName = "SmartFitness Bridge";
    config.ftmsOutput = true;
    config.ftmsOutputProfile = FtmsOutputProfile::Auto;
    config.ant = false;
    config.antRequiresHardware = true;
    config.unsaved = true;
    return config;
}

bool lastCommand(CommandType type) { return !g_commands.empty() && g_commands.back().type == type; }

// --- knob-only walkthrough -------------------------------------------------------

void knobWalkthrough() {
    assert(g_ui.current() == Screen::Dashboard);
    turn(1);  // next metric, stays on the dashboard
    assert(g_ui.current() == Screen::Dashboard);
    press();
    assert(g_ui.current() == Screen::Menu);
    turn(2);  // Dashboard -> Devices -> Control
    press();
    assert(g_ui.current() == Screen::Control);
    press();  // first control of a treadmill: speed
    assert(g_ui.current() == Screen::Adjust);
    turn(5);  // 3.0 -> 3.5 km/h
    press();
    assert(g_ui.current() == Screen::Confirm);
    assert(g_commands.empty());  // nothing sent before the confirmation
    press();  // default focus is Cancel
    assert(g_ui.current() == Screen::Control && g_commands.empty());
    press();
    turn(5);
    press();
    turn(1);  // Cancel -> Confirm
    press();
    assert(g_ui.current() == Screen::Control);
    assert(lastCommand(CommandType::Control) && g_commands.back().control == Control::Speed &&
           std::fabs(g_commands.back().value - 3.5F) < 1e-4F);
    hold();
    assert(g_ui.current() == Screen::Menu);
    hold();
    assert(g_ui.current() == Screen::Dashboard);

    // Settings -> factory reset needs two confirmations.
    press();
    turn(3);
    press();
    assert(g_ui.current() == Screen::Settings);
    turn(9);
    press();
    assert(g_ui.current() == Screen::Confirm);
    turn(1);
    press();
    assert(g_ui.current() == Screen::Confirm && !lastCommand(CommandType::FactoryReset));
    turn(1);
    press();
    assert(g_ui.current() == Screen::Settings && lastCommand(CommandType::FactoryReset));

    // Devices: start a scan, open a result, pair with an adapter override.
    hold();
    turn(1);  // Dashboard -> Devices
    press();
    assert(g_ui.current() == Screen::Devices);
    press();  // first button: stop scan (scan running)
    assert(lastCommand(CommandType::ScanStop));
    hold();
    hold();
    assert(g_ui.current() == Screen::Dashboard);
    g_commands.clear();
    std::printf("knob-only walkthrough passed\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_outDir = argv[1];

    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t* display = lv_display_create(kSize, kSize);
    static std::vector<std::uint8_t> buffer(kSize * kSize * 2);
    lv_display_set_buffers(display, buffer.data(), nullptr, static_cast<std::uint32_t>(buffer.size()),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_theme_t* theme = lv_theme_default_init(display, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_TEAL),
                                              true, &lv_font_montserrat_14);
    lv_display_set_theme(display, theme);

    lv_indev_t* encoder = lv_indev_create();
    lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(encoder, encoderRead);

    HmiActions actions;
    actions.submit = [](const Command& command) { g_commands.push_back(command); };
    g_ui.begin(encoder, HmiSettings{}, actions);
    g_ui.setStatus(treadmillStatus());
    g_ui.setScan(scanView());
    g_ui.setConfig(configView());
    pump(100);

    knobWalkthrough();

    g_ui.setStatus(bikeStatus());
    pump(100);
    turn(1);  // Incline (selected during the walkthrough) -> Power
    shot("01-dashboard-bike");
    g_ui.setStatus(treadmillStatus());
    turn(-3);  // Power -> Speed in the treadmill order
    shot("02-dashboard-treadmill");
    g_ui.navigate(Screen::Menu);
    shot("03-menu");
    g_ui.navigate(Screen::Devices);
    shot("04-devices");
    turn(5);
    shot("05-devices-results");
    g_ui.openDevice(0);
    shot("06-device-detail");
    g_ui.navigate(Screen::AdapterPicker);
    shot("07-adapter-picker");
    g_ui.navigate(Screen::Control);
    shot("08-control-treadmill");
    g_ui.openAdjust(Control::Speed);
    pump(50);
    turn(25);
    shot("09-adjust-speed");
    press();
    shot("10-confirm-speed");
    g_ui.setStatus(bikeStatus());
    g_ui.navigate(Screen::Control);
    shot("11-control-bike");
    g_ui.openAdjust(Control::Power);
    shot("12-adjust-power");
    g_ui.navigate(Screen::Settings);
    shot("13-settings");
    turn(7);
    shot("14-settings-scrolled");
    g_ui.navigate(Screen::FtmsName);
    shot("15-ftms-name");
    g_ui.navigate(Screen::Keyboard);
    shot("16-keyboard");
    g_ui.openFactoryReset();
    shot("17-factory-reset-1");
    turn(1);
    press();
    shot("18-factory-reset-2");
    g_ui.navigate(Screen::Dashboard);
    pump(50);
    g_ui.showResult(makeCommand(CommandType::Control), {false, "control: stale telemetry"});
    shot("19-toast-rejected");
    std::printf("hmi simulator done\n");
    return 0;
}
