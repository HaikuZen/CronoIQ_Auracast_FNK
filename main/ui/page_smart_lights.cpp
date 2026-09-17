#include "page_smart_lights.h"

#include <cstdint>
#include <cstdio>

#include "weather_icons.h"

namespace {

struct ColorPreset {
    const char *name;
    uint8_t r, g, b;
};

constexpr ColorPreset kColorPresets[] = {
    {"Warm", 255, 214, 170}, {"White", 255, 255, 255}, {"Red", 255, 0, 0},
    {"Green", 0, 255, 0},    {"Blue", 0, 120, 255},     {"Purple", 180, 0, 255},
};

// WiZ's built-in dynamic scenes, in scene-ID order (1-32; the well-known
// mapping shared by WiZ's own app and third-party integrations like
// pywizlight). Line 0 is a sentinel meaning "not running a scene" — it is
// never sent to the light, only shown/selected to represent that state.
// The list is deliberately \n-joined into one lv_dropdown options string
// where option index == scene_id (index 0 is the sentinel).
constexpr const char *kSceneNames =
    "Color / Custom\n"
    "Ocean\nRomance\nSunset\nParty\nFireplace\nCozy\nForest\nPastel Colors\n"
    "Wake up\nBedtime\nWarm White\nDaylight\nCool white\nNight light\nFocus\nRelax\n"
    "True colors\nTV time\nPlantgrowth\nSpring\nSummer\nFall\nDeepdive\nJungle\n"
    "Mojito\nClub\nChristmas\nHalloween\nCandlelight\nGolden white\nPulse\nSteampunk";

}  // namespace

static lv_obj_t *s_lights_container;
static lv_obj_t *s_panel;
static std::vector<LightStatus> s_lights;
static std::vector<lv_obj_t *> s_cards;
static int s_selected_index = -1;

static void rebuild_control_panel();
static void highlight_selected_card();

static void set_grid_placeholder(const char *text) {
    lv_obj_clean(s_lights_container);
    lv_obj_t *label = lv_label_create(s_lights_container);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9AB4D0), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
}

static void select_light(int idx) {
    if (idx < 0 || idx >= (int)s_lights.size()) return;
    s_selected_index = idx;
    highlight_selected_card();
    rebuild_control_panel();
}

static void light_card_event_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    select_light(idx);
}

static void close_panel_event_cb(lv_event_t *e) {
    (void)e;
    s_selected_index = -1;
    highlight_selected_card();
    rebuild_control_panel();
}

static void power_switch_event_cb(lv_event_t *e) {
    if (s_selected_index < 0 || s_selected_index >= (int)s_lights.size()) return;
    auto *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const LightStatus &light = s_lights[s_selected_index];
    wiz_set_power(light.ip, light.udp_port, lv_obj_has_state(sw, LV_STATE_CHECKED));
}

// Only updates the live "NN%" label while dragging — the actual command is
// sent once on release (brightness_slider_released_cb) so we don't flood
// the light with a UDP packet per pixel of drag.
static void brightness_slider_changed_cb(lv_event_t *e) {
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    auto *value_label = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)lv_slider_get_value(slider));
    lv_label_set_text(value_label, buf);
}

static void brightness_slider_released_cb(lv_event_t *e) {
    if (s_selected_index < 0 || s_selected_index >= (int)s_lights.size()) return;
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const LightStatus &light = s_lights[s_selected_index];
    wiz_set_brightness(light.ip, light.udp_port, (uint8_t)lv_slider_get_value(slider));
}

static void color_btn_event_cb(lv_event_t *e) {
    if (s_selected_index < 0 || s_selected_index >= (int)s_lights.size()) return;
    const LightStatus &light = s_lights[s_selected_index];
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const ColorPreset &c = kColorPresets[idx];
    wiz_set_color(light.ip, light.udp_port, c.r, c.g, c.b);
}

static void scene_dropdown_event_cb(lv_event_t *e) {
    if (s_selected_index < 0 || s_selected_index >= (int)s_lights.size()) return;
    auto *dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
    uint32_t selected = lv_dropdown_get_selected(dd);
    if (selected == 0) return;  // sentinel "Color / Custom" — nothing to send
    const LightStatus &light = s_lights[s_selected_index];
    wiz_set_scene(light.ip, light.udp_port, (uint8_t)selected);
}

static lv_obj_t *make_row(lv_obj_t *parent, int32_t height) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

// Rebuilt from scratch on every selection change AND on every periodic
// status refresh (so externally-changed state is reflected) — a side
// effect is that dragging the brightness slider at the exact moment a 5s
// poll refresh lands will interrupt the drag. Accepted tradeoff over the
// complexity of a partial in-place update.
static void rebuild_control_panel() {
    lv_obj_clean(s_panel);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 10, 0);

    if (s_selected_index < 0 || s_selected_index >= (int)s_lights.size()) {
        lv_obj_t *placeholder = lv_label_create(s_panel);
        lv_label_set_text(placeholder, "Tap a light above to control it");
        lv_obj_set_style_text_color(placeholder, lv_color_hex(0x6A84A0), 0);
        lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_16, 0);
        return;
    }

    const LightStatus &light = s_lights[s_selected_index];

    lv_obj_t *header = make_row(s_panel, 30);
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, light.name.c_str());
    lv_obj_set_style_text_color(title, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    lv_obj_t *close_btn = lv_label_create(header);
    lv_label_set_text(close_btn, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_btn, lv_color_hex(0x9AB4D0), 0);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, close_panel_event_cb, LV_EVENT_CLICKED, nullptr);

    if (light.brand != "WiZ") {
        char msg[96];
        snprintf(msg, sizeof(msg), "No controls available yet for brand \"%s\"", light.brand.c_str());
        lv_obj_t *unsupported = lv_label_create(s_panel);
        lv_label_set_text(unsupported, msg);
        lv_obj_set_style_text_color(unsupported, lv_color_hex(0x9AB4D0), 0);
        lv_obj_set_style_text_font(unsupported, &lv_font_montserrat_16, 0);
        return;
    }

    lv_obj_t *power_row = make_row(s_panel, 40);
    lv_obj_t *power_label = lv_label_create(power_row);
    lv_label_set_text(power_label, "Power");
    lv_obj_set_style_text_color(power_label, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(power_label, &lv_font_montserrat_16, 0);

    lv_obj_t *sw = lv_switch_create(power_row);
    if (light.power) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, power_switch_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *bright_row = make_row(s_panel, 40);
    lv_obj_t *bright_label = lv_label_create(bright_row);
    lv_label_set_text(bright_label, "Brightness");
    lv_obj_set_style_text_color(bright_label, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(bright_label, &lv_font_montserrat_16, 0);

    lv_obj_t *slider = lv_slider_create(bright_row);
    lv_obj_set_width(slider, 240);
    lv_slider_set_range(slider, 10, 100);  // WiZ's practical dimming floor is 10, not 0
    lv_slider_set_value(slider, light.brightness > 0 ? light.brightness : 100, LV_ANIM_OFF);

    lv_obj_t *value_label = lv_label_create(bright_row);
    lv_obj_set_width(value_label, 46);
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%u%%", (unsigned)light.brightness);
    lv_label_set_text(value_label, vbuf);
    lv_obj_set_style_text_color(value_label, lv_color_hex(0x9AB4D0), 0);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_16, 0);

    lv_obj_add_event_cb(slider, brightness_slider_changed_cb, LV_EVENT_VALUE_CHANGED, value_label);
    lv_obj_add_event_cb(slider, brightness_slider_released_cb, LV_EVENT_RELEASED, nullptr);

    lv_obj_t *scene_row = make_row(s_panel, 40);
    lv_obj_t *scene_label = lv_label_create(scene_row);
    lv_label_set_text(scene_label, "Scene");
    lv_obj_set_style_text_color(scene_label, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(scene_label, &lv_font_montserrat_16, 0);

    lv_obj_t *scene_dd = lv_dropdown_create(scene_row);
    lv_dropdown_set_options(scene_dd, kSceneNames);
    lv_dropdown_set_selected(scene_dd, light.scene_id);
    lv_obj_set_width(scene_dd, 220);
    lv_obj_add_event_cb(scene_dd, scene_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *color_row = make_row(s_panel, 44);
    lv_obj_set_flex_align(color_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < (int)(sizeof(kColorPresets) / sizeof(kColorPresets[0])); i++) {
        lv_obj_t *btn = lv_obj_create(color_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 36, 36);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        uint32_t hex = ((uint32_t)kColorPresets[i].r << 16) | ((uint32_t)kColorPresets[i].g << 8) |
                       (uint32_t)kColorPresets[i].b;
        lv_obj_set_style_bg_color(btn, lv_color_hex(hex), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x0D1B2A), 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, color_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void highlight_selected_card() {
    for (size_t i = 0; i < s_cards.size(); i++) {
        bool selected = ((int)i == s_selected_index);
        lv_obj_set_style_border_width(s_cards[i], selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(s_cards[i], lv_color_hex(0x00E5FF), 0);
    }
}

static lv_obj_t *make_light_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 200, 170);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x14283A), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return card;
}

lv_obj_t *page_smart_lights_create(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(WEATHER_ICON_BG_HEX), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 10, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_lights_container = lv_obj_create(parent);
    lv_obj_remove_style_all(s_lights_container);
    lv_obj_set_size(s_lights_container, LV_PCT(100), 175);
    lv_obj_align(s_lights_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_row(s_lights_container, 14, 0);
    lv_obj_set_style_pad_column(s_lights_container, 14, 0);
    lv_obj_set_flex_flow(s_lights_container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_lights_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_lights_container, LV_DIR_VER);

    // Control panel for whichever light is currently selected (or a
    // placeholder when none is). Scrolls vertically as a safety margin in
    // case a brand's control set (power/brightness/scene/color for WiZ)
    // doesn't quite fit the allotted height rather than clipping/overlapping.
    s_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, LV_PCT(100), 225);
    lv_obj_align_to(s_panel, s_lights_container, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x14283A), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 12, 0);
    lv_obj_set_style_pad_all(s_panel, 14, 0);
    lv_obj_set_scroll_dir(s_panel, LV_DIR_VER);

    set_grid_placeholder("Waiting for lights...");
    rebuild_control_panel();
    return parent;
}

void page_smart_lights_update(const std::vector<LightStatus> &lights) {
    s_lights = lights;
    if (s_selected_index >= (int)s_lights.size()) {
        s_selected_index = -1;
    }

    if (s_lights.empty()) {
        set_grid_placeholder("No smart lights configured");
        s_cards.clear();
        rebuild_control_panel();
        return;
    }

    lv_obj_clean(s_lights_container);
    s_cards.clear();
    s_cards.reserve(s_lights.size());

    for (size_t i = 0; i < s_lights.size(); i++) {
        const LightStatus &light = s_lights[i];
        lv_obj_t *card = make_light_card(s_lights_container);
        lv_obj_add_event_cb(card, light_card_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_cards.push_back(card);

        lv_obj_t *name_label = lv_label_create(card);
        lv_label_set_text(name_label, light.name.c_str());
        lv_obj_set_style_text_color(name_label, lv_color_hex(0xE0F0FF), 0);
        lv_obj_set_style_text_font(name_label, &lv_font_montserrat_20, 0);

        lv_obj_t *swatch = lv_obj_create(card);
        lv_obj_remove_style_all(swatch);
        lv_obj_set_size(swatch, 48, 48);
        lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);

        uint32_t swatch_hex;
        if (!light.online) {
            swatch_hex = 0x3A3A3A;
        } else if (!light.power) {
            swatch_hex = 0x22303E;
        } else {
            swatch_hex = ((uint32_t)light.r << 16) | ((uint32_t)light.g << 8) | (uint32_t)light.b;
        }
        lv_obj_set_style_bg_color(swatch, lv_color_hex(swatch_hex), 0);
        if (light.online && light.power) {
            lv_obj_set_style_shadow_color(swatch, lv_color_hex(swatch_hex), 0);
            lv_obj_set_style_shadow_width(swatch, 18, 0);
            lv_obj_set_style_shadow_opa(swatch, LV_OPA_60, 0);
        }

        char status_buf[32];
        if (!light.online) {
            snprintf(status_buf, sizeof(status_buf), "Offline");
        } else if (!light.power) {
            snprintf(status_buf, sizeof(status_buf), "Off");
        } else {
            snprintf(status_buf, sizeof(status_buf), "On - %u%%", (unsigned)light.brightness);
        }
        lv_obj_t *status_label = lv_label_create(card);
        lv_label_set_text(status_label, status_buf);
        lv_obj_set_style_text_color(status_label, lv_color_hex(light.online ? 0xE0F0FF : 0x808080), 0);
        lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);

        char meta_buf[64];
        snprintf(meta_buf, sizeof(meta_buf), "%s - %s", light.brand.c_str(), light.ip.c_str());
        lv_obj_t *meta_label = lv_label_create(card);
        lv_label_set_text(meta_label, meta_buf);
        lv_obj_set_style_text_color(meta_label, lv_color_hex(0x6A84A0), 0);
        lv_obj_set_style_text_font(meta_label, &lv_font_montserrat_14, 0);
    }

    highlight_selected_card();
    rebuild_control_panel();
}
