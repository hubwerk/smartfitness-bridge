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
#include "scroll_arc.h"

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
lv_indev_t* g_knob = nullptr;
std::vector<Command> g_commands;

void checkLcars() {
    assert(lcars::uppercase("Bridge äöü ß") == "BRIDGE ÄÖÜ ß");
    for (const auto* font : {&sf_antonio_16, &sf_antonio_20}) {
        lv_font_glyph_dsc_t glyph{};
        for (const auto letter : {0x30U, 0x41U, 0xC4U, 0xD6U, 0xDCU, 0xDFU}) {
            assert(font->get_glyph_dsc(font, &glyph, letter, 0));
        }
        lv_font_glyph_dsc_t digit{};
        assert(sf_antonio_48.get_glyph_dsc(&sf_antonio_48, &digit, '8', 0));
    }
    lv_obj_t* button = lv_button_create(lv_screen_active());
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(button, LV_PART_MAIN)) ==
           lv_color_to_u32(lv_color_hex(lcars::Lavender)));
    assert(lv_obj_get_style_shadow_width(button, LV_PART_MAIN) == 0);
    lv_obj_add_state(button, LV_STATE_FOCUSED);
    // Focus colors switch immediately: no fading highlight on the old item.
    assert(lv_obj_get_style_border_width(button, LV_PART_MAIN) == 2);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(button, LV_PART_MAIN)) ==
           lv_color_to_u32(lv_color_hex(lcars::Peach)));
    lv_obj_remove_state(button, LV_STATE_FOCUSED);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(button, LV_PART_MAIN)) ==
           lv_color_to_u32(lv_color_hex(lcars::Lavender)));
    lv_obj_add_state(button, LV_STATE_FOCUSED);
    for (const auto state : {LV_STATE_FOCUS_KEY, LV_STATE_EDITED}) {
        lv_style_value_t width{}, pad{};
        assert(lv_obj_get_local_style_prop(button, LV_STYLE_OUTLINE_WIDTH, &width, LV_PART_MAIN | state) == LV_STYLE_RES_FOUND);
        assert(lv_obj_get_local_style_prop(button, LV_STYLE_OUTLINE_PAD, &pad, LV_PART_MAIN | state) == LV_STYLE_RES_FOUND);
        assert(width.num + pad.num == lcars::FocusRing);
    }
    lv_obj_delete(button);
    std::printf("LCARS palette, focus, and Latin-1 fonts passed\n");
}

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

void checkScrollbar() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_t* list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 174, 100);
    lv_obj_t* child = lv_obj_create(list);
    lv_obj_set_pos(child, 0, 0);
    lv_obj_set_size(child, 150, 50);
    lcars::scrollbar(list);
    lv_obj_t* arc = lv_obj_get_child(screen, 1);
    assert(lv_obj_get_scrollbar_mode(list) == LV_SCROLLBAR_MODE_OFF);
    assert(lv_obj_has_flag(arc, LV_OBJ_FLAG_HIDDEN));
    lv_obj_set_height(child, 400);
    lv_obj_update_layout(list);
    assert(!lv_obj_has_flag(arc, LV_OBJ_FLAG_HIDDEN));
    assert(lv_arc_get_angle_end(arc) == 205);
    lv_obj_scroll_to_y(list, 150, LV_ANIM_OFF);
    assert(lv_arc_get_angle_start(arc) == 174);
    lv_obj_scroll_to_y(list, 300, LV_ANIM_OFF);
    assert(lv_arc_get_angle_start(arc) == 155);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    lv_obj_set_height(child, 50);
    lv_obj_update_layout(list);
    assert(lv_obj_has_flag(arc, LV_OBJ_FLAG_HIDDEN));
    lv_obj_delete(list);  // removes its sibling arc too
    assert(lv_obj_get_child_count(screen) == 0);
    lv_obj_delete(screen);

    screen = lv_obj_create(nullptr);
    auto* roller = lv_roller_create(screen);
    lv_roller_set_options(roller, "ONE\nTWO\nTHREE\nFOUR\nFIVE\nSIX", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, 2);
    lcars::scrollbar(roller);
    arc = lv_obj_get_child(screen, 1);
    assert(!lv_obj_has_flag(arc, LV_OBJ_FLAG_HIDDEN));
    assert(lv_arc_get_angle_end(arc) == 205);
    lv_roller_set_selected(roller, 5, LV_ANIM_OFF);
    lv_obj_update_layout(roller);
    assert(lv_arc_get_angle_start(arc) == 155);
    lv_obj_delete(screen);
    std::printf("curved scrollbar visibility, scrolling, resizing, roller and lifecycle passed\n");
}

lv_obj_t* currentList() {
    lv_obj_t* focused = lv_group_get_focused(lv_indev_get_group(g_knob));
    assert(focused && lv_obj_check_type(focused, &lv_button_class));
    return lv_obj_get_parent(focused);
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

// --- layout check -------------------------------------------------------------
// Geometric checks on the rendered object tree: the focus cursor must never be
// clipped, drawn elements and text must not overlap each other or the LCARS
// arcs, and everything has to stay on the round panel.

struct Element {
    lv_obj_t* obj;
    lv_area_t full;     // drawn extent (incl. outline / text ink)
    lv_area_t visible;  // after clipping by the ancestors
    std::string name;
};

struct Ring {
    lv_obj_t* obj;
    float cx, cy, outer, inner, start, span, cap;
};

bool isAncestor(lv_obj_t* ancestor, lv_obj_t* obj) {
    for (obj = lv_obj_get_parent(obj); obj; obj = lv_obj_get_parent(obj))
        if (obj == ancestor) return true;
    return false;
}

bool intersects(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

bool empty(const lv_area_t& a) { return a.x1 > a.x2 || a.y1 > a.y2; }

bool sameArea(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

std::string describe(lv_obj_t* obj) {
    std::string name = lv_obj_check_type(obj, &lv_label_class)      ? "label"
                       : lv_obj_check_type(obj, &lv_button_class)   ? "button"
                       : lv_obj_check_type(obj, &lv_roller_class)   ? "roller"
                       : lv_obj_check_type(obj, &lv_textarea_class) ? "textarea"
                       : lv_obj_check_type(obj, &lv_keyboard_class) ? "keyboard"
                                                                    : "obj";
    lv_obj_t* label = lv_obj_check_type(obj, &lv_label_class) ? obj : nullptr;
    for (std::uint32_t i = 0; !label && i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t* child = lv_obj_get_child(obj, static_cast<int>(i));
        if (lv_obj_check_type(child, &lv_label_class)) label = child;
    }
    if (label) {
        std::string text = lv_label_get_text(label);
        for (auto& c : text)
            if (c == '\n') c = ' ';
        name += " '" + text.substr(0, 28) + "'";
    }
    return name;
}

// Vertical extent of the glyphs (not the line box) of a label.
lv_area_t textInk(lv_obj_t* label) {
    lv_area_t coords{};
    lv_obj_get_coords(label, &coords);
    const char* text = lv_label_get_text(label);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int lineSpace = lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    const int letterSpace = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const int line = lv_font_get_line_height(font);
    int top = line, bottom = 0;
    for (const char* c = text; *c; ++c) {
        if (*c == '\n' || *c == ' ') continue;
        lv_font_glyph_dsc_t glyph{};
        if (!lv_font_get_glyph_dsc(font, &glyph, static_cast<std::uint8_t>(*c), 0)) continue;
        const int baseline = line - font->base_line;
        top = std::min(top, baseline - glyph.ofs_y - glyph.box_h);
        bottom = std::max(bottom, baseline - glyph.ofs_y);
    }
    if (top >= bottom) return {1, 1, 0, 0};
    const int content = lv_obj_get_content_width(label);
    lv_point_t size{};
    lv_text_get_size(&size, text, font, letterSpace, lineSpace, content, LV_TEXT_FLAG_NONE);
    const int lines = std::max(1, (lv_obj_get_content_height(label) + lineSpace) / (line + lineSpace));
    lv_area_t ink{};
    const int left = coords.x1 + lv_obj_get_style_pad_left(label, LV_PART_MAIN);
    const auto align = lv_obj_get_style_text_align(label, LV_PART_MAIN);
    const int width = std::min<int>(size.x, content);
    ink.x1 = align == LV_TEXT_ALIGN_CENTER  ? left + (content - width) / 2
             : align == LV_TEXT_ALIGN_RIGHT ? left + content - width
                                            : left;
    ink.x2 = ink.x1 + width - 1;
    ink.y1 = coords.y1 + lv_obj_get_style_pad_top(label, LV_PART_MAIN) + top;
    ink.y2 = coords.y1 + lv_obj_get_style_pad_top(label, LV_PART_MAIN) + (lines - 1) * (line + lineSpace) + bottom - 1;
    return ink;
}

void collect(lv_obj_t* obj, std::vector<Element>& elements, std::vector<Ring>& rings) {
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t* child = lv_obj_get_child(obj, static_cast<int>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        collect(child, elements, rings);
    }
    if (lv_obj_get_parent(obj) == nullptr) return;  // screen / layer
    lv_area_t full{};
    lv_obj_get_coords(obj, &full);
    if (lv_obj_check_type(obj, &lv_arc_class)) {
        Ring ring{obj, 0, 0, 0, 0, 0, 0, 0};
        ring.cx = (full.x1 + full.x2 + 1) / 2.0F;
        ring.cy = (full.y1 + full.y2 + 1) / 2.0F;
        ring.outer = lv_area_get_width(&full) / 2.0F;
        float width = static_cast<float>(lv_obj_get_style_arc_width(obj, LV_PART_MAIN));
        if (lv_obj_get_style_arc_opa(obj, LV_PART_INDICATOR) > LV_OPA_TRANSP)
            width = std::max<float>(width, static_cast<float>(lv_obj_get_style_arc_width(obj, LV_PART_INDICATOR)));
        ring.inner = ring.outer - width;
        ring.start = static_cast<float>(lv_arc_get_bg_angle_start(obj));
        ring.span = static_cast<float>(lv_arc_get_bg_angle_end(obj)) - ring.start;
        if (ring.span < 0) ring.span += 360.0F;
        ring.cap = lv_obj_get_style_arc_rounded(obj, LV_PART_MAIN)
                       ? std::asin(width / 2.0F / (ring.outer - width / 2.0F)) * 180.0F / 3.14159265F
                       : 0.0F;
        rings.push_back(ring);
        return;
    }
    lv_obj_t* parent = lv_obj_get_parent(obj);
    if (lv_obj_check_type(obj, &lv_label_class)) {
        // Roller options are checked row by row in checkRoller().
        if (lv_obj_check_type(parent, &lv_roller_class)) return;
        if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) == LV_OPA_TRANSP) full = textInk(obj);
    } else {
        const bool drawn = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) > LV_OPA_TRANSP ||
                           (lv_obj_get_style_border_width(obj, LV_PART_MAIN) > 0 &&
                            lv_obj_get_style_border_opa(obj, LV_PART_MAIN) > LV_OPA_TRANSP);
        if (!drawn) return;  // transparent layout container
    }
    if (empty(full)) return;
    const int outline = lv_obj_get_style_outline_width(obj, LV_PART_MAIN);
    if (outline > 0 && lv_obj_get_style_outline_opa(obj, LV_PART_MAIN) > LV_OPA_TRANSP)
        lv_area_increase(&full, outline + lv_obj_get_style_outline_pad(obj, LV_PART_MAIN),
                         outline + lv_obj_get_style_outline_pad(obj, LV_PART_MAIN));
    Element element{obj, full, full, describe(obj)};
    for (lv_obj_t* ancestor = parent; ancestor; ancestor = lv_obj_get_parent(ancestor)) {
        lv_area_t clip{};
        lv_obj_get_coords(ancestor, &clip);
        element.visible = {std::max(element.visible.x1, clip.x1), std::max(element.visible.y1, clip.y1),
                           std::min(element.visible.x2, clip.x2), std::min(element.visible.y2, clip.y2)};
        if (empty(element.visible)) break;
    }
    if (!empty(element.visible)) elements.push_back(element);
}

// A partially visible element is fine only inside a scrolled list and only
// when it does not carry the focus cursor.
bool scrolledAway(lv_obj_t* obj) {
    if (lv_obj_has_state(obj, LV_STATE_FOCUSED)) return false;
    for (lv_obj_t* ancestor = lv_obj_get_parent(obj); ancestor; ancestor = lv_obj_get_parent(ancestor)) {
        if (lv_obj_get_scroll_top(ancestor) + lv_obj_get_scroll_bottom(ancestor) > 0) return true;
        if (lv_obj_has_state(ancestor, LV_STATE_FOCUSED)) return false;
    }
    return false;
}

bool onPanel(const lv_area_t& area, int radius) {
    const float inset = static_cast<float>(radius) * (1.0F - 0.7071F);
    for (const float x : {area.x1 + inset, area.x2 + 1 - inset}) {
        for (const float y : {area.y1 + inset, area.y2 + 1 - inset}) {
            const float dx = x - kSize / 2.0F;
            const float dy = y - kSize / 2.0F;
            if (dx * dx + dy * dy > kSize * kSize / 4.0F + 0.5F) return false;
        }
    }
    return true;
}

bool touchesRing(const lv_area_t& area, int radius, const Ring& ring) {
    auto inside = [&ring](float x, float y) {
        const float dx = x - ring.cx;
        const float dy = y - ring.cy;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < ring.inner || distance > ring.outer) return false;
        float angle = std::atan2(dy, dx) * 180.0F / 3.14159265F;
        float offset = std::fmod(angle - ring.start + ring.cap + 720.0F, 360.0F);
        return offset <= ring.span + 2 * ring.cap;
    };
    // Outline of the rounded rectangle.
    const float r = static_cast<float>(radius);
    const float left = area.x1 + r, right = area.x2 + 1 - r, top = area.y1 + r, bottom = area.y2 + 1 - r;
    for (float x = left; x <= right; x += 1.0F)
        if (inside(x, static_cast<float>(area.y1)) || inside(x, static_cast<float>(area.y2 + 1))) return true;
    for (float y = top; y <= bottom; y += 1.0F)
        if (inside(static_cast<float>(area.x1), y) || inside(static_cast<float>(area.x2 + 1), y)) return true;
    for (int step = 0; step <= 90 && radius > 0; ++step) {
        const float angle = static_cast<float>(step) * 3.14159265F / 180.0F;
        const float dx = r * std::cos(angle), dy = r * std::sin(angle);
        if (inside(right + dx, bottom + dy) || inside(left - dx, bottom + dy) || inside(right + dx, top - dy) ||
            inside(left - dx, top - dy))
            return true;
    }
    return false;
}

int checkRoller(lv_obj_t* roller, const char* screen) {
    int issues = 0;
    lv_obj_t* label = lv_obj_get_child(roller, 0);
    const lv_font_t* font = lv_obj_get_style_text_font(roller, LV_PART_MAIN);
    const int line = lv_font_get_line_height(font);
    const int step = line + lv_obj_get_style_text_line_space(roller, LV_PART_MAIN);
    // Children are clipped by the roller's border box, not its padding.
    lv_area_t box{};
    lv_obj_get_coords(roller, &box);
    const int border = lv_obj_get_style_border_width(roller, LV_PART_MAIN);
    lv_area_increase(&box, -border, -border);
    lv_area_t text{};
    lv_obj_get_coords(label, &text);
    for (std::uint32_t row = 0; row < lv_roller_get_option_count(roller); ++row) {
        const int top = text.y1 + static_cast<int>(row) * step;
        const int bottom = top + line - 1;
        if (bottom < box.y1 || top > box.y2) continue;
        if (top < box.y1 || bottom > box.y2) {
            std::printf("  %s: roller row %u cut (%d..%d outside %d..%d)\n", screen, row, top, bottom, box.y1, box.y2);
            ++issues;
        }
    }
    return issues;
}

void checkLayout(const char* screen) {
    std::vector<Element> elements;
    std::vector<Ring> rings;
    collect(lv_screen_active(), elements, rings);
    const std::size_t screenElements = elements.size();
    collect(lv_layer_top(), elements, rings);
    int issues = 0;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const Element& a = elements[i];
        if (!sameArea(a.full, a.visible) && !scrolledAway(a.obj)) {
            std::printf("  %s: %s clipped (%d,%d %d,%d visible %d,%d %d,%d)\n", screen, a.name.c_str(), a.full.x1,
                        a.full.y1, a.full.x2, a.full.y2, a.visible.x1, a.visible.y1, a.visible.x2, a.visible.y2);
            ++issues;
        }
        const bool text = lv_obj_check_type(a.obj, &lv_label_class) &&
                          lv_obj_get_style_bg_opa(a.obj, LV_PART_MAIN) == LV_OPA_TRANSP;
        const int radius =
            text ? 0
                 : std::min<int>(lv_obj_get_style_radius(a.obj, LV_PART_MAIN),
                                 std::min(lv_area_get_width(&a.visible), lv_area_get_height(&a.visible)) / 2);
        if (!onPanel(a.visible, radius)) {
            std::printf("  %s: %s leaves the round panel (%d,%d %d,%d r%d)\n", screen, a.name.c_str(), a.visible.x1,
                        a.visible.y1, a.visible.x2, a.visible.y2, radius);
            ++issues;
        }
        for (const Ring& ring : rings) {
            if (isAncestor(ring.obj, a.obj) || isAncestor(a.obj, ring.obj)) continue;
            if (i >= screenElements) continue;  // toast overlays are temporary
            if (touchesRing(a.visible, radius, ring)) {
                std::printf("  %s: %s overlaps arc %.0f..%.0f\n", screen, a.name.c_str(), ring.start,
                            ring.start + ring.span);
                ++issues;
                break;
            }
        }
        for (std::size_t j = i + 1; j < screenElements; ++j) {
            const Element& b = elements[j];
            if (isAncestor(a.obj, b.obj) || isAncestor(b.obj, a.obj)) continue;
            if (intersects(a.visible, b.visible)) {
                std::printf("  %s: %s overlaps %s\n", screen, a.name.c_str(), b.name.c_str());
                ++issues;
            }
        }
        if (lv_obj_check_type(a.obj, &lv_roller_class)) issues += checkRoller(a.obj, screen);
    }
    if (issues) std::printf("layout check %s: %d issue(s)\n", screen, issues);
    assert(issues == 0);
}


void shot(const char* name, bool checkFocus = true) {
    pump(400);  // let animations (scroll on focus) settle
    bool hasScrollbar = false;
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(lv_screen_active()); ++i) {
        auto* child = lv_obj_get_child(lv_screen_active(), static_cast<int>(i));
        if (lv_obj_check_type(child, &lv_arc_class) && lv_arc_get_bg_angle_start(child) == 155) {
            hasScrollbar = true;
        }
    }
    if (g_ui.current() == Screen::Dashboard) assert(!hasScrollbar);
    if (g_ui.current() == Screen::Confirm) {
        assert(hasScrollbar);
        for (std::uint32_t i = 0; i < lv_obj_get_child_count(lv_screen_active()); ++i) {
            auto* child = lv_obj_get_child(lv_screen_active(), static_cast<int>(i));
            if (lv_obj_check_type(child, &lv_arc_class)) assert(lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN));
            if (i == 0)  // header band
                assert(lv_color_to_u32(lv_obj_get_style_bg_color(child, LV_PART_MAIN)) ==
                       lv_color_to_u32(lv_color_hex(lcars::Alert)));
        }
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
    lv_obj_t* focused = lv_group_get_focused(lv_indev_get_group(g_knob));
    if (checkFocus && focused && lv_obj_check_type(focused, &lv_button_class)) {
        assert(lv_obj_get_height(focused) >= 36);
        lv_area_t area{};
        lv_obj_get_coords(focused, &area);
        for (const int x : {area.x1, area.x2}) {
            for (const int y : {area.y1, area.y2}) {
                const int dx = x - kSize / 2;
                const int dy = y - kSize / 2;
                assert(dx * dx + dy * dy <= kSize * kSize / 4);
            }
        }
    }
    checkLayout(name);
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
    status.input.paired = {true, "aa:00:00:00:00:21", false, "KICKR CORE", "ftms", {}, {}};
    status.input.paired.advertisedServices = {"1826", "1818", "180d"};
    status.input.paired.subscriptions = {{"1826", "2ad2", "ok", false, ""},
                                        {"1826", "2ad9", "ok", true, ""},
                                        {"1818", "2a63", "failed", false, "cccd-failed"},
                                        {"180d", "2a37", "pending", false, ""}};
    status.ftmsOutput.enabled = true;
    status.ftmsOutput.name = "SmartFitness Bridge";
    status.ftmsOutput.advertising = true;
    status.ftmsOutput.client = true;
    status.ftmsOutput.control = true;
    status.ftmsOutput.services = {{"ftms", "FTMS", true, true, true},
                                  {"cps", "CPS", true, true, true},
                                  {"hr", "HR", true, true, true},
                                  {"csc", "CSC", false, true, false},
                                  {"rsc", "RSC", false, false, false}};
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
    status.input.paired = {true, "aa:00:00:00:00:22", false, "WalkingPad", "walkingpad-r1", {}, {}};
    status.input.paired.advertisedServices = {"1826", "180d"};
    status.input.paired.subscriptions = {{"1826", "2acd", "ok", false, ""},
                                        {"1826", "2ad9", "failed", true, "cccd-failed"},
                                        {"180d", "2a37", "pending", false, ""}};
    status.ftmsOutput.enabled = true;
    status.ftmsOutput.profile = MachineProfile::Treadmill;
    status.ftmsOutput.name = "SmartFitness Bridge";
    status.ftmsOutput.advertising = true;
    status.ftmsOutput.services = {{"ftms", "FTMS", true, true, true},
                                  {"cps", "CPS", false, false, false},
                                  {"hr", "HR", true, true, true},
                                  {"csc", "CSC", false, false, false},
                                  {"rsc", "RSC", false, true, false}};
    return status;
}

StatusView antStatus() {
    auto status = bikeStatus();
    status.outputProtocol = OutputProtocol::AntFec;
    status.antOutput.enabled = true;
    status.antOutput.available = true;
    status.antOutput.link = "open";
    status.antOutput.stick = true;
    status.antOutput.open = true;
    status.antOutput.hardware = "nRF52840 UART";
    status.antOutput.deviceNumber = 12345;
    status.antOutput.channelLimit = 8;
    status.antOutput.display = true;
    status.antOutput.control = true;
    status.antOutput.channels = {{"fec", "FE-C", 17, true, true, true},
                                 {"hr", "Heart rate", 120, true, true, true},
                                 {"power", "Bike power", 11, false, false, true}};
    return status;
}

ScanView scanView() {
    ScanView scan;
    scan.scanning = true;
    scan.bleAvailable = true;
    // As built by BridgeController::scanView(): one entry per address, label
    // falls back to manufacturer/service, irrelevant devices sorted last.
    auto device = [](const char* address, const char* name, int rssi, const char* adapter, const char* label,
                     bool relevant) {
        ScanDeviceView view;
        view.address = address;
        view.name = name;
        view.rssi = rssi;
        view.adapter = adapter;
        view.label = label;
        view.relevant = relevant;
        if (relevant) view.services = {"1826", "180d"};
        return view;
    };
    scan.devices = {device("aa:00:00:00:00:22", "WalkingPad", -58, "walkingpad-r1", "WalkingPad", true),
                    device("aa:00:00:00:00:21", "KICKR CORE", -64, "ftms", "KICKR CORE", true),
                    device("aa:00:00:00:00:30", "I_TL", -71, "ifit-treadmill", "I_TL", true),
                    device("aa:00:00:00:00:32", "", -75, "", "Heart Rate", true),
                    device("aa:00:00:00:00:31", "", -60, "", "Apple", false),
                    device("aa:00:00:00:00:33", "", -83, "", "aa:00:00:00:00:33", false)};
    return scan;
}

ConfigView configView() {
    ConfigView config;
    config.version = "0.3.0";
    config.ftmsName = "SmartFitness Bridge";
    config.outputProtocol = OutputProtocol::BleFtms;
    config.ftmsOutputProfile = FtmsOutputProfile::Auto;
    config.antAvailable = false;
    config.unsaved = true;
    config.ftmsOutputServices = 7;
    return config;
}

// Every available dashboard metric of every machine profile at its widest value.
void checkDashboardMetrics() {
    g_ui.navigate(Screen::Dashboard);
    for (std::size_t p = 0; p < kMachineProfileCount; ++p) {
        auto status = bikeStatus();
        status.input.profile = static_cast<MachineProfile>(p);
        for (std::size_t m = 0; m < kMetricCount; ++m) {
            const MetricFormat& format = metricFormat(static_cast<Metric>(m));
            const float widest = std::fabs(format.gaugeMin) > format.gaugeMax ? format.gaugeMin : format.gaugeMax;
            setMetric(status, static_cast<Metric>(m), widest * 1.999F);
        }
        g_ui.setStatus(status);
        pump(100);
        for (std::size_t i = 0; i < dashboardMetrics(status.input.profile, status).size(); ++i) {
            turn(1);
            pump(50);
            const std::string name = "dashboard-" + std::to_string(p) + "-" + std::to_string(i);
            checkLayout(name.c_str());
        }
    }
    std::printf("dashboard layout of all profiles and metrics passed\n");
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
    turn(1);  // Output: BLE FTMS -> Off (fixture has no ANT transport)
    press();
    assert(lastCommand(CommandType::OutputProtocol) && g_commands.back().text == "off");
    turn(2);  // Output -> profile -> output services
    press();
    assert(g_ui.current() == Screen::OutputServices);
    press();  // Auto is an explicit zero mask, not all services
    assert(lastCommand(CommandType::SetOutputServices) && g_commands.back().text == "ftms" &&
           g_commands.back().serviceMask == 0);
    turn(1);  // FTMS stays mandatory
    const auto commandsBefore = g_commands.size();
    press();
    assert(g_commands.size() == commandsBefore);
    turn(1);  // CPS enabled in fixture -> off, keeping FTMS + HR
    press();
    assert(lastCommand(CommandType::SetOutputServices) && g_commands.back().serviceMask == 5);
    checkLayout("output-services-knob");
    turn(2);  // CSC has no data on this treadmill: readable, but cannot be enabled.
    const auto beforeUnavailable = g_commands.size();
    press();
    assert(g_commands.size() == beforeUnavailable);
    checkLayout("output-services-unavailable");
    hold();
    assert(g_ui.current() == Screen::Settings);
    turn(9);  // name -> factory reset (new services entry included)
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
    press();  // first button: input service diagnostics
    assert(g_ui.current() == Screen::InputServices);
    turn(2);  // first subscription, then fail and pending
    checkLayout("input-subscription-ok");
    turn(1);
    checkLayout("input-subscription-fail");
    turn(1);
    checkLayout("input-subscription-pending");
    hold();
    turn(1);  // input services -> stop scan
    press();
    assert(lastCommand(CommandType::ScanStop));
    hold();
    hold();
    assert(g_ui.current() == Screen::Dashboard);
    g_commands.clear();
    std::printf("knob-only walkthrough passed\n");
}

// The first and last focusable entries reveal leading/trailing labels, so the
// scrollbar thumb reaches both ends of its track; only a label too tall to fit
// next to the focused entry may stay partly scrolled out.
void checkListEnd(lv_obj_t* list, lv_obj_t* arc, bool top) {
    lv_area_t content;
    lv_obj_get_content_coords(list, &content);
    lv_area_t focused;
    lv_obj_get_coords(lv_group_get_focused(lv_indev_get_group(g_knob)), &focused);
    if (top) {
        assert(lv_obj_get_scroll_top(list) == 0 ? lv_arc_get_angle_end(arc) == 205 : focused.y2 == content.y2);
    } else {
        assert(lv_obj_get_scroll_bottom(list) == 0 ? lv_arc_get_angle_start(arc) == 155 : focused.y1 == content.y1);
    }
}

void checkListEnds() {
    for (Screen screen : {Screen::Menu, Screen::Devices, Screen::Settings, Screen::FtmsName,
                          Screen::InputServices, Screen::OutputServices}) {
        g_ui.navigate(screen);
        pump(400);
        lv_obj_t* list = currentList();
        lv_obj_t* arc = lv_obj_get_child(lv_obj_get_parent(list), lv_obj_get_index(list) + 1);
        assert(lv_obj_check_type(arc, &lv_arc_class));
        assert(lv_obj_get_scroll_top(list) + lv_obj_get_scroll_bottom(list) > 0);
        checkListEnd(list, arc, true);
        turn(static_cast<int>(lv_group_get_obj_count(lv_indev_get_group(g_knob))) - 1);
        pump(400);
        checkListEnd(list, arc, false);
        turn(1);  // wraps to the first entry
        pump(400);
        checkListEnd(list, arc, true);
    }
    g_ui.navigate(Screen::Menu);
    pump(400);
    lv_obj_t* list = currentList();
    turn(-1);  // Dashboard wraps to Settings; the back hint below it comes into view
    pump(400);
    assert(lv_obj_get_scroll_bottom(list) == 0);
    g_ui.navigate(Screen::Dashboard);
    pump(100);
    std::printf("list scrollbar reaches both ends passed\n");
}

void checkServiceRefresh() {
    auto status = bikeStatus();
    g_ui.setStatus(status);
    g_ui.navigate(Screen::InputServices);
    pump(100);
    turn(5);  // failed CPS notification
    auto* focused = lv_group_get_focused(lv_indev_get_group(g_knob));
    assert(std::string(lv_label_get_text(lv_obj_get_child(focused, 0))).find("FAIL") != std::string::npos);
    lv_style_value_t subscriptionFill{};
    assert(lv_obj_get_local_style_prop(focused, LV_STYLE_BG_COLOR, &subscriptionFill, LV_PART_MAIN) ==
           LV_STYLE_RES_FOUND);
    assert(lv_color_to_u32(subscriptionFill.color) == lv_color_to_u32(lv_color_hex(lcars::Alert)));
    status.input.paired.subscriptions[2].state = "ok";
    g_ui.setStatus(status);
    pump(100);
    focused = lv_group_get_focused(lv_indev_get_group(g_knob));
    assert(std::string(lv_label_get_text(lv_obj_get_child(focused, 0))).find("OK") != std::string::npos);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(focused, LV_PART_MAIN)) ==
           lv_color_to_u32(lv_color_hex(lcars::Peach)));  // retained knob focus
    status.input.paired.advertisedServices.push_back("180f");
    g_ui.setStatus(status);
    pump(100);
    assert(lv_group_get_obj_count(lv_indev_get_group(g_knob)) == 9);

    g_ui.navigate(Screen::OutputServices);
    pump(100);
    turn(5);
    focused = lv_group_get_focused(lv_indev_get_group(g_knob));
    assert(std::string(lv_label_get_text(lv_obj_get_child(focused, 0))).find("NO DATA") != std::string::npos);
    status.ftmsOutput.services[4].available = true;
    g_ui.setStatus(status);
    pump(100);
    focused = lv_group_get_focused(lv_indev_get_group(g_knob));
    assert(std::string(lv_label_get_text(lv_obj_get_child(focused, 0))).find("AVAILABLE") != std::string::npos);
    lv_style_value_t fill{};
    assert(lv_obj_get_local_style_prop(focused, LV_STYLE_BG_COLOR, &fill, LV_PART_MAIN) == LV_STYLE_RES_FOUND);
    assert(lv_color_to_u32(fill.color) == lv_color_to_u32(lv_color_hex(lcars::Tan)));
    auto config = configView();
    config.ftmsOutputServices = 31;
    g_ui.setConfig(config);
    pump(100);
    focused = lv_group_get_focused(lv_indev_get_group(g_knob));
    assert(std::string(lv_label_get_text(lv_obj_get_child(focused, 0))).find("ON") != std::string::npos);
    assert(lv_obj_get_local_style_prop(focused, LV_STYLE_BG_COLOR, &fill, LV_PART_MAIN) == LV_STYLE_RES_FOUND);
    assert(lv_color_to_u32(fill.color) == lv_color_to_u32(lv_color_hex(lcars::Golden)));
    status.ftmsOutput.services[4].available = false;
    g_ui.setStatus(status);
    pump(100);
    focused = lv_group_get_focused(lv_indev_get_group(g_knob));
    assert(lv_obj_get_local_style_prop(focused, LV_STYLE_BG_COLOR, &fill, LV_PART_MAIN) == LV_STYLE_RES_FOUND);
    assert(lv_color_to_u32(fill.color) == lv_color_to_u32(lv_color_hex(lcars::Alert)));
    status.ftmsOutput.restartRequired = true;
    g_ui.setStatus(status);
    pump(100);
    assert(std::string(lv_label_get_text(lv_obj_get_child(currentList(), 0))) == "SAVE + RESTART BLE");
    for (const auto lang : {Language::English, Language::German}) {
        setLanguage(lang);
        // Leave and re-enter because language changes are not live firmware settings.
        g_ui.navigate(Screen::Dashboard);
        pump(50);
        for (const auto screen : {Screen::InputServices, Screen::OutputServices}) {
            g_ui.navigate(screen);
            pump(100);
            const auto count = lv_group_get_obj_count(lv_indev_get_group(g_knob));
            for (std::uint32_t index = 0; index < count; ++index) {
                checkLayout(lang == Language::English ? "services-en" : "services-de");
                turn(1);
            }
        }
    }
    setLanguage(Language::English);
    for (auto& service : status.ftmsOutput.services) service.enabled = true;
    g_ui.setStatus(status);
    g_ui.navigate(Screen::Dashboard);
    pump(100);
    checkLayout("dashboard-all-output-services");
    g_ui.setConfig(configView());
    g_ui.setStatus(treadmillStatus());
    pump(100);
    std::printf("live service diagnostics, availability and mask redraw passed\n");
}

std::string readoutText(std::uint32_t index) {
    return lv_label_get_text(lv_obj_get_child(lv_obj_get_child(currentList(), index), 0));
}

void checkAntUi() {
    auto status = antStatus();
    auto config = configView();
    // The live transport, not the independently refreshed config, determines
    // whether ANT+ is offered (including while no physical module is detected).
    config.antAvailable = false;
    for (const auto protocol : {OutputProtocol::BleFtms, OutputProtocol::AntFec, OutputProtocol::Off}) {
        config.outputProtocol = protocol;
        g_ui.setStatus(status);
        g_ui.setConfig(config);
        g_ui.navigate(Screen::Dashboard);
        pump(50);
        g_ui.navigate(Screen::Settings);
        pump(100);
        turn(1);
        press();
        assert(lastCommand(CommandType::OutputProtocol));
        assert(g_commands.back().text == outputProtocolName(nextOutputProtocol(protocol, true)));
    }
    status.antOutput.available = false;
    config.antAvailable = true;
    config.outputProtocol = OutputProtocol::BleFtms;
    g_ui.setConfig(config);
    g_ui.setStatus(status);
    pump(100);
    press();  // retained Output focus: BLE -> Off, despite stale config availability
    assert(g_commands.back().text == "off");
    status = antStatus();
    g_ui.setStatus(status);
    pump(100);
    turn(3);  // Output -> profile -> output services -> ANT status
    press();
    assert(g_ui.current() == Screen::AntStatus);
    assert(readoutText(0) == "BROADCASTING\nMODULE DETECTED");
    auto* header = lv_obj_get_child(lv_screen_active(), 0);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(header, LV_PART_MAIN)) ==
           lv_color_to_u32(lv_color_hex(lcars::Orange)));
    status.antOutput.stick = false;
    status.antOutput.open = false;
    status.antOutput.link = "wait-stick";
    status.antOutput.hardware.clear();
    g_ui.setStatus(status);
    pump(100);
    assert(readoutText(0) == "NO ANT+ MODULE");
    assert(readoutText(1) == "LINK: WAIT-STICK");
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(lv_obj_get_child(currentList(), 0), LV_PART_MAIN)) ==
           lv_color_to_u32(lv_color_hex(lcars::Alert)));
    header = lv_obj_get_child(lv_screen_active(), 0);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(header, LV_PART_MAIN)) ==
           lv_color_to_u32(lv_color_hex(lcars::Alert)));
    status.antOutput.stick = true;
    g_ui.setStatus(status);
    pump(100);
    assert(readoutText(0) == "WAITING FOR ANT+\nMODULE DETECTED");
    status.antOutput.enabled = false;
    g_ui.setStatus(status);
    pump(100);
    assert(readoutText(0) == "OUTPUT OFF\nMODULE DETECTED");
    status = antStatus();
    status.antOutput.hardware = "nRF51422 UART";
    status.antOutput.deviceNumber = 42;
    status.antOutput.channelLimit = 4;
    status.antOutput.channels[1].open = false;
    status.antOutput.channels[2].available = false;
    status.antOutput.display = false;
    status.antOutput.control = false;
    g_ui.setStatus(status);
    pump(100);
    assert(readoutText(2) == "HARDWARE: NRF51422 UART");
    assert(readoutText(3) == "DEVICE NUMBER: 42");
    assert(readoutText(4) == "CHANNELS: 1/4");
    assert(readoutText(7).find("NO DATA") != std::string::npos);
    assert(readoutText(8) == "DISPLAY: OFF");
    assert(readoutText(9) == "CONTROL: OFF");
    for (const auto lang : {Language::English, Language::German}) {
        setLanguage(lang);
        g_ui.navigate(Screen::Dashboard);
        pump(50);
        g_ui.navigate(Screen::AntStatus);
        pump(100);
        const auto count = lv_group_get_obj_count(lv_indev_get_group(g_knob));
        for (std::uint32_t index = 0; index < count; ++index) {
            checkLayout("ant-diagnostics");
            turn(1);
        }
    }
    setLanguage(Language::English);
    status = antStatus();
    config.antOutputServices = 3;
    g_ui.setStatus(status);
    g_ui.setConfig(config);
    g_ui.navigate(Screen::OutputServices);
    pump(100);
    turn(6);  // BLE Auto + five BLE services -> ANT Auto
    press();
    assert(lastCommand(CommandType::SetOutputServices) && g_commands.back().text == "ant" &&
           g_commands.back().serviceMask == 0);
    turn(1);  // FE-C can be disabled while Power remains selected
    press();
    assert(g_commands.back().serviceMask == 2);
    config.antOutputServices = 2;
    g_ui.setConfig(config);
    pump(100);
    press();
    assert(g_commands.back().serviceMask == 3);
    config.antOutputServices = 3;
    g_ui.setConfig(config);
    pump(100);
    assert(readoutText(13) == lcars::uppercase(tr(Str::AntDuplicatePower)));
    turn(1);  // Power -> off
    press();
    assert(g_commands.back().serviceMask == 1);
    config.antOutputServices = 1;
    g_ui.setConfig(config);
    pump(100);
    assert(lv_group_get_obj_count(lv_indev_get_group(g_knob)) == 14);  // warning removed live
    turn(-1);  // last selected channel must not silently become Auto
    auto commandsBefore = g_commands.size();
    press();
    assert(g_commands.size() == commandsBefore);
    pump(2600);  // clear the selection warning toast before geometry checks
    config.antOutputServices = 0;
    g_ui.setConfig(config);
    pump(100);
    turn(2);  // HR: Auto baseline FE-C -> FE-C + HR
    press();
    assert(g_commands.back().serviceMask == 5);
    turn(1);  // unavailable speed/cadence: readable, not selectable
    commandsBefore = g_commands.size();
    press();
    assert(g_commands.size() == commandsBefore);
    status.antOutput.channels.push_back({"speed-cadence", "Speed / cadence", 121, false, false, true});
    g_ui.setStatus(status);
    pump(100);
    press();
    assert(g_commands.back().serviceMask == 9);
    for (const auto lang : {Language::English, Language::German}) {
        setLanguage(lang);
        config.antOutputServices = 3;
        g_ui.setConfig(config);
        g_ui.navigate(Screen::Dashboard);
        pump(50);
        g_ui.navigate(Screen::OutputServices);
        pump(100);
        const auto count = lv_group_get_obj_count(lv_indev_get_group(g_knob));
        for (std::uint32_t index = 0; index < count; ++index) {
            checkLayout("ant-services");
            turn(1);
        }
    }
    setLanguage(Language::English);
    g_ui.setStatus(treadmillStatus());
    g_ui.setConfig(configView());
    g_ui.navigate(Screen::Dashboard);
    pump(100);
    g_commands.clear();
    std::printf("ANT output cycling, no-module alert, live status redraw and EN/DE layout passed\n");
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
    lcars::init(display);

    lv_indev_t* encoder = lv_indev_create();
    g_knob = encoder;
    lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(encoder, encoderRead);

    HmiActions actions;
    actions.submit = [](const Command& command) { g_commands.push_back(command); };
    g_ui.begin(encoder, HmiSettings{}, actions);
    g_ui.setStatus(treadmillStatus());
    g_ui.setScan(scanView());
    g_ui.setConfig(configView());
    pump(100);

    checkLcars();
    checkScrollbar();
    knobWalkthrough();
    checkListEnds();
    checkServiceRefresh();
    checkAntUi();

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
    turn(7);  // first scan result (input diagnostics plus six device actions)
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
    pump(2600);
    auto disconnected = bikeStatus();
    disconnected.state = RuntimeState::Disconnected;
    disconnected.canControl = false;
    disconnected.input.link = "disconnected";
    disconnected.input.stale = true;
    disconnected.ftmsOutput.client = false;
    for (auto& metric : disconnected.telemetry) metric.valid = false;
    g_ui.setStatus(disconnected);
    shot("20-error-disconnected");
    g_ui.setStatus(treadmillStatus());
    g_ui.navigate(Screen::Devices);
    pump(400);
    auto* list = currentList();
    lv_anim_delete(list, nullptr);
    // No off-screen focus rectangle in the swipe-position screenshots.
    lv_obj_remove_state(lv_group_get_focused(lv_indev_get_group(g_knob)),
                        static_cast<lv_state_t>(LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY));
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    shot("21-devices-scroll-top", false);
    const int range = lv_obj_get_scroll_top(list) + lv_obj_get_scroll_bottom(list);
    assert(range > 0);
    lv_obj_scroll_to_y(list, range / 2, LV_ANIM_OFF);
    shot("22-devices-scroll-middle", false);
    lv_obj_scroll_to_y(list, range, LV_ANIM_OFF);
    shot("23-devices-scroll-bottom", false);
    g_ui.setStatus(bikeStatus());
    g_ui.navigate(Screen::InputServices);
    shot("24-input-services-adv");
    turn(3);
    shot("25-input-services-sub-ok");
    turn(2);
    shot("26-input-services-sub-fail");
    turn(1);
    shot("27-input-services-sub-pending");
    g_ui.navigate(Screen::OutputServices);
    shot("28-output-services");
    turn(5);
    shot("29-output-services-no-data");
    auto restart = bikeStatus();
    restart.ftmsOutput.restartRequired = true;
    g_ui.setStatus(restart);
    g_ui.navigate(Screen::Settings);
    pump(50);
    g_ui.navigate(Screen::OutputServices);
    shot("30-output-services-restart");
    auto ant = antStatus();
    g_ui.setStatus(ant);
    g_ui.navigate(Screen::AntStatus);
    shot("31-ant-detected");
    turn(4);
    shot("32-ant-channels");
    ant.antOutput.stick = false;
    ant.antOutput.open = false;
    ant.antOutput.link = "wait-stick";
    ant.antOutput.hardware.clear();
    for (auto& channel : ant.antOutput.channels) channel.open = false;
    ant.antOutput.display = false;
    ant.antOutput.control = false;
    g_ui.setStatus(ant);
    g_ui.navigate(Screen::Settings);
    pump(50);
    g_ui.navigate(Screen::AntStatus);
    shot("33-ant-no-module");
    ant = antStatus();
    auto antConfig = configView();
    antConfig.antOutputServices = 3;
    g_ui.setStatus(ant);
    g_ui.setConfig(antConfig);
    g_ui.navigate(Screen::OutputServices);
    pump(100);
    turn(6);
    shot("34-ant-services");
    turn(6);
    shot("35-ant-duplicate-power");
    g_ui.setConfig(configView());
    g_ui.setStatus(bikeStatus());
    auto longScan = scanView();
    longScan.devices[0].name = "A VERY LONG FITNESS DEVICE NAME THAT MUST WRAP WITHOUT CLIPPING";
    g_ui.setScan(longScan);
    g_ui.openDevice(0);
    pump(400);
    list = currentList();
    auto* name = lv_obj_get_child(list, 0);
    assert(std::string(lv_label_get_text(name)) == longScan.devices[0].name);
    assert(lv_obj_get_width(name) <= lv_obj_get_content_width(list));
    assert(lv_obj_get_height(name) > lv_font_get_line_height(&sf_antonio_16));
    checkLayout("device-detail-long-name");
    checkDashboardMetrics();
    std::printf("hmi simulator done\n");
    return 0;
}
