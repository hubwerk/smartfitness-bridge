#include "lcars_theme.h"

#include <algorithm>

#include "scroll_arc.h"
#include "src/themes/lv_theme_private.h"

namespace hmi::lcars {
namespace {

lv_theme_t theme;

// Replaces the default theme's wide outline (4 px + 4 px gap), which got
// clipped by lists, rows and the frame.
void focusRing(lv_obj_t* object, lv_style_selector_t part) {
    for (const auto state : {LV_STATE_FOCUS_KEY, LV_STATE_EDITED}) {
        lv_obj_set_style_outline_width(object, FocusRingWidth, part | state);
        lv_obj_set_style_outline_pad(object, FocusRingGap, part | state);
        lv_obj_set_style_outline_opa(object, LV_OPA_COVER, part | state);
    }
    lv_obj_set_style_outline_color(object, lv_color_hex(Orange), part | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(object, lv_color_hex(LightBlue), part | LV_STATE_EDITED);
}

void apply(lv_theme_t*, lv_obj_t* object) {
    lv_obj_set_style_shadow_width(object, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(object, &sf_antonio_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(object, lv_color_hex(LightBlue), LV_PART_MAIN);
    if (lv_obj_check_type(object, &lv_button_class) || lv_obj_check_type(object, &lv_roller_class) ||
        lv_obj_check_type(object, &lv_textarea_class) || lv_obj_check_type(object, &lv_keyboard_class))
        focusRing(object, LV_PART_MAIN);
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
        lv_obj_set_style_radius(object, 12, 0);
        lv_obj_set_style_bg_color(object, lv_color_hex(Lavender), LV_PART_ITEMS);
        lv_obj_set_style_text_color(object, lv_color_black(), LV_PART_ITEMS);
        lv_obj_set_style_radius(object, 12, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(object, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(object, lv_color_hex(Peach), LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_border_color(object, lv_color_white(), LV_PART_ITEMS | LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(object, 2, LV_PART_ITEMS | LV_STATE_FOCUSED);
        // Keys sit 2 px apart: their cursor is the white border only.
        for (const auto state : {LV_STATE_FOCUS_KEY, LV_STATE_EDITED})
            lv_obj_set_style_outline_width(object, 0, LV_PART_ITEMS | state);
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

lv_obj_t* arc(lv_obj_t* parent, int size, int width, std::uint32_t color, int start, int end) {
    lv_obj_t* object = lv_arc_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(object, size, size);
    lv_obj_center(object);
    lv_arc_set_bg_angles(object, start, end);
    lv_obj_set_style_arc_width(object, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(object, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(object, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(object, LV_OPA_TRANSP, LV_PART_INDICATOR);
    return object;
}

void updateScrollbar(lv_event_t* event) {
    auto* container = lv_event_get_current_target_obj(event);
    auto* object = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        lv_obj_delete(object);
        return;
    }
    if (code != LV_EVENT_SCROLL && code != LV_EVENT_SCROLL_END && code != LV_EVENT_SIZE_CHANGED &&
        code != LV_EVENT_LAYOUT_CHANGED && code != LV_EVENT_CHILD_CHANGED && code != LV_EVENT_VALUE_CHANGED)
        return;

    const int viewport = lv_obj_get_content_height(container);
    int offset = lv_obj_get_scroll_top(container);
    int content = viewport + offset + lv_obj_get_scroll_bottom(container);
    if (lv_obj_check_type(container, &lv_roller_class)) {
        // Rollers move their label rather than scrolling the container.
        const auto* font = lv_obj_get_style_text_font(container, LV_PART_MAIN);
        const int line = lv_font_get_line_height(font);
        const int step = line + lv_obj_get_style_text_line_space(container, LV_PART_MAIN);
        const int count = static_cast<int>(lv_roller_get_option_count(container));
        content = count * step;
        const int range = std::max(0, content - viewport);
        const int labelOffset = viewport / 2 - line / 2 - lv_obj_get_y(lv_obj_get_child(container, 0));
        offset = count > 1 && step > 0
                     ? static_cast<int>(static_cast<std::int64_t>(labelOffset) * range / ((count - 1) * step))
                     : 0;
    }
    const auto thumb = scrollArc(content, viewport, offset);
    if (!thumb.visible) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_arc_set_angles(object, thumb.start, thumb.end);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
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

lv_obj_t* frame(lv_obj_t* screen, const std::string& title, bool alert) {
    const auto color = alert ? Alert : Orange;
    lv_obj_t* header = arc(screen, 232, 12, color, 225, 315);
    bar(screen, 48, 57, 144, 3, Peach, 1);

    // One line between the header arc and the bar; long titles get dots.
    lv_obj_t* label = lv_label_create(screen);
    lv_obj_set_size(label, 120, lv_font_get_line_height(&sf_antonio_20));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, uppercase(title).c_str());
    lv_obj_set_style_text_font(label, &sf_antonio_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(Peach), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, 60, 31);
    return header;
}

void scrollbar(lv_obj_t* container) {
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t* object = arc(lv_obj_get_parent(container), 232, 4, Lavender, 155, 205);
    lv_obj_set_style_arc_opa(object, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(object, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(object, lv_color_hex(Orange), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(object, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(object, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(container, updateScrollbar, LV_EVENT_ALL, object);
    lv_obj_update_layout(container);
    lv_obj_send_event(container, LV_EVENT_LAYOUT_CHANGED, nullptr);
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
