#include "lcars_theme.h"

#include <algorithm>

#include "src/themes/lv_theme_private.h"

namespace hmi::lcars {
namespace {

lv_theme_t theme;

void apply(lv_theme_t*, lv_obj_t* object) {
    lv_obj_set_style_shadow_width(object, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(object, &sf_antonio_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(object, lv_color_hex(LightBlue), LV_PART_MAIN);
    if (lv_obj_check_type(object, &lv_button_class)) pill(object);
    if (lv_obj_check_type(object, &lv_roller_class) || lv_obj_check_type(object, &lv_textarea_class)) {
        lv_obj_set_style_bg_color(object, lv_color_hex(Black), 0);
        lv_obj_set_style_border_color(object, lv_color_hex(Lavender), 0);
        lv_obj_set_style_radius(object, 16, 0);
        lv_obj_set_style_border_color(object, lv_color_white(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(object, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(object, lv_color_hex(Peach), LV_PART_SELECTED);
        lv_obj_set_style_text_color(object, lv_color_black(), LV_PART_SELECTED);
        lv_obj_set_style_text_font(object, &sf_antonio_20, LV_PART_SELECTED);
    }
    if (lv_obj_check_type(object, &lv_keyboard_class)) {
        lv_obj_set_style_bg_color(object, lv_color_black(), 0);
        lv_obj_set_style_bg_color(object, lv_color_hex(Lavender), LV_PART_ITEMS);
        lv_obj_set_style_text_color(object, lv_color_black(), LV_PART_ITEMS);
        lv_obj_set_style_radius(object, 12, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(object, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(object, lv_color_hex(Peach), LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_border_color(object, lv_color_white(), LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(object, 2, LV_PART_ITEMS | LV_STATE_FOCUSED);
    }
}

void bar(lv_obj_t* parent, int x, int y, int width, int height, std::uint32_t color, int radius) {
    lv_obj_t* object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_radius(object, radius, 0);
}

}  // namespace

void init(lv_display_t* display) {
    lv_theme_t* base = lv_theme_default_init(display, lv_color_hex(Orange), lv_color_hex(Lavender),
                                             true, &sf_antonio_16);
    theme = *base;
    theme.parent = base;
    theme.apply_cb = apply;
    lv_display_set_theme(display, &theme);
}

std::string uppercase(std::string text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto c = static_cast<unsigned char>(text[i]);
        if (c >= 'a' && c <= 'z') text[i] = static_cast<char>(c - 'a' + 'A');
        // Latin-1 accented letters encoded as UTF-8; preserve all other bytes.
        if (c == 0xC3 && i + 1 < text.size()) {
            c = static_cast<unsigned char>(text[++i]);
            if (c >= 0xA0 && c <= 0xBE && c != 0xB7) text[i] = static_cast<char>(c - 0x20);
        }
    }
    return text;
}

void pill(lv_obj_t* object, std::uint32_t color) {
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_text_color(object, lv_color_black(), 0);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_border_width(object, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(object, lv_color_white(), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(object, lv_color_hex(Peach), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(object, lv_color_hex(Golden), LV_STATE_PRESSED);
    lv_obj_set_style_opa(object, LV_OPA_50, LV_STATE_DISABLED);
}

void frame(lv_obj_t* screen, const std::string& title, bool alert) {
    const auto color = alert ? Alert : Orange;
    // A swept elbow with a small inner cutout, entirely inside the circular safe area.
    bar(screen, 33, 34, 174, 22, color, 11);
    bar(screen, 33, 34, 20, 72, color, 10);
    bar(screen, 45, 56, 162, 50, Black, 6);
    bar(screen, 33, 110, 8, 28, Peach, 4);
    bar(screen, 33, 142, 8, 28, Lavender, 4);
    bar(screen, 57, 210, 126, 4, color, 2);
    bar(screen, 67, 217, 38, 3, Violet, 1);
    bar(screen, 109, 217, 64, 3, Peach, 1);

    lv_obj_t* label = lv_label_create(screen);
    lv_obj_set_width(label, 144);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, uppercase(title).c_str());
    lv_obj_set_style_text_font(label, &sf_antonio_20, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(label, 54, 34);
}

Gauge gauge(lv_obj_t* parent) {
    Gauge segments{};
    for (std::size_t i = 0; i < segments.size(); ++i) {
        lv_obj_t* arc = lv_arc_create(parent);
        lv_obj_remove_style_all(arc);
        lv_obj_remove_flag(arc, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_size(arc, 232, 232);
        lv_obj_center(arc);
        const auto start = static_cast<std::uint32_t>(135 + i * 270 / segments.size());
        const auto end = static_cast<std::uint32_t>(135 + (i + 1) * 270 / segments.size() - 3);
        lv_arc_set_bg_angles(arc, start, end);
        lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        segments[i] = arc;
    }
    setGauge(segments, 0);
    return segments;
}

void setGauge(const Gauge& segments, float ratio, bool alert) {
    const auto lit = static_cast<std::size_t>(std::min(1.0F, std::max(0.0F, ratio)) * segments.size());
    for (std::size_t i = 0; i < segments.size(); ++i) {
        lv_obj_set_style_arc_color(segments[i], lv_color_hex(alert ? Alert : (i < lit ? LightBlue : Tan)),
                                   LV_PART_MAIN);
        lv_obj_set_style_arc_opa(segments[i], alert || i < lit ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    }
}

}  // namespace hmi::lcars
