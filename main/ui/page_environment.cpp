#include "page_environment.h"

#include <cstdint>
#include <cstdio>

#include "weather_icons.h"

namespace {

struct IaqCategory {
    float max_iaq;  // inclusive upper bound
    const char *name;
    uint32_t color;
};

// Bosch's IAQ index scale (BME68x/BME69x datasheets). Colors follow the
// usual green→red→purple air-quality convention, lightened where needed to
// stay readable on the dark page background.
constexpr IaqCategory kIaqCategories[] = {
    {50, "Excellent", 0x3DDC84},
    {100, "Good", 0x9BD94A},
    {150, "Lightly polluted", 0xF2D530},
    {200, "Moderately polluted", 0xFF9A2E},
    {250, "Heavily polluted", 0xFF4D4D},
    {350, "Severely polluted", 0xD05BA8},
    {500, "Extremely polluted", 0xB07040},
};

constexpr uint32_t kCardBg = 0x14283A;
constexpr uint32_t kTextMain = 0xE0F0FF;
constexpr uint32_t kTextMuted = 0x9AB4D0;
constexpr uint32_t kTextDim = 0x6A84A0;

struct Tile {
    lv_obj_t *value;
    lv_obj_t *unit;
};

}  // namespace

static bool s_fahrenheit = false;

static lv_obj_t *s_iaq_value;
static lv_obj_t *s_iaq_category;
static lv_obj_t *s_iaq_bar;
static lv_obj_t *s_status;
static Tile s_temp, s_hum, s_pres, s_co2, s_tvoc, s_gas;

static const IaqCategory &category_for(float iaq) {
    for (const IaqCategory &c : kIaqCategories) {
        if (iaq <= c.max_iaq) return c;
    }
    return kIaqCategories[sizeof(kIaqCategories) / sizeof(kIaqCategories[0]) - 1];
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t w, int32_t h) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCardBg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static Tile make_tile(lv_obj_t *parent, const char *title, const char *unit) {
    lv_obj_t *card = make_card(parent, 229, 127);
    make_label(card, title, &lv_font_montserrat_16, kTextMuted);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    Tile t;
    t.value = make_label(row, "--", &lv_font_montserrat_38, kTextMain);
    t.unit = make_label(row, unit, &lv_font_montserrat_20, kTextMuted);
    // Nudge the unit up so its baseline sits roughly on the big number's.
    lv_obj_set_style_pad_bottom(t.unit, 4, 0);
    return t;
}

static void set_tile(const Tile &t, bool valid, const char *fmt, float value) {
    if (!valid) {
        lv_label_set_text(t.value, "--");
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), fmt, value);
    lv_label_set_text(t.value, buf);
}

lv_obj_t *page_environment_create(lv_obj_t *parent, const std::string &units) {
    s_fahrenheit = (units == "us");

    lv_obj_set_style_bg_color(parent, lv_color_hex(WEATHER_ICON_BG_HEX), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 10, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // --- Left: air quality card ---
    lv_obj_t *iaq_card = make_card(parent, 300, 410);
    lv_obj_align(iaq_card, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(iaq_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(iaq_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(iaq_card, 10, 0);

    make_label(iaq_card, "Air Quality", &lv_font_montserrat_20, kTextMuted);

    s_iaq_value = make_label(iaq_card, "--", &lv_font_montserrat_38, kTextMain);
    lv_obj_set_style_pad_top(s_iaq_value, 16, 0);

    s_iaq_category = make_label(iaq_card, "", &lv_font_montserrat_24, kTextMain);
    lv_obj_set_width(s_iaq_category, LV_PCT(100));
    lv_label_set_long_mode(s_iaq_category, LV_LABEL_LONG_WRAP);

    s_iaq_bar = lv_bar_create(iaq_card);
    lv_obj_set_size(s_iaq_bar, LV_PCT(100), 14);
    lv_bar_set_range(s_iaq_bar, 0, 500);
    lv_bar_set_value(s_iaq_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_iaq_bar, lv_color_hex(0x0D1B2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_iaq_bar, LV_OPA_COVER, LV_PART_MAIN);

    make_label(iaq_card, "IAQ index 0-500, lower is better", &lv_font_montserrat_14, kTextDim);

    s_status = make_label(iaq_card, "Waiting for sensor...", &lv_font_montserrat_16, kTextMuted);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(s_status, 12, 0);

    // --- Right: 2x3 reading tiles ---
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 470, 410);
    lv_obj_align(grid, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    s_temp = make_tile(grid, "Temperature", s_fahrenheit ? "°F" : "°C");
    s_hum = make_tile(grid, "Humidity", "%");
    s_pres = make_tile(grid, "Pressure", "hPa");
    s_co2 = make_tile(grid, "CO2 equivalent", "ppm");
    s_tvoc = make_tile(grid, "TVOC equivalent", "ppb");
    s_gas = make_tile(grid, "Gas resistance", "kOhm");

    return parent;
}

void page_environment_update(const EnvReading &r) {
    const bool running = (r.state == EnvSensorState::Running);

    set_tile(s_temp, running, "%.1f", s_fahrenheit ? r.temperature_c * 9.0f / 5.0f + 32.0f : r.temperature_c);
    set_tile(s_hum, running, "%.0f", r.humidity_pct);
    set_tile(s_pres, running, "%.0f", r.pressure_hpa);
    set_tile(s_co2, running, "%.0f", r.co2_ppm);
    set_tile(s_tvoc, running, "%.0f", r.tvoc_ppb);
    set_tile(s_gas, running, r.gas_resistance_ohm < 100000.0f ? "%.1f" : "%.0f", r.gas_resistance_ohm / 1000.0f);

    if (!running) {
        lv_label_set_text(s_iaq_value, "--");
        lv_label_set_text(s_iaq_category, "");
        lv_bar_set_value(s_iaq_bar, 0, LV_ANIM_OFF);
    } else {
        const IaqCategory &cat = category_for(r.iaq);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", r.iaq);
        lv_label_set_text(s_iaq_value, buf);
        lv_label_set_text(s_iaq_category, cat.name);
        // Accuracy 0 means BSEC is still stabilizing and the index is just a
        // placeholder (typically sits at 50) — show it dimmed.
        uint32_t color = r.iaq_accuracy == 0 ? kTextDim : cat.color;
        lv_obj_set_style_text_color(s_iaq_value, lv_color_hex(r.iaq_accuracy == 0 ? kTextDim : kTextMain), 0);
        lv_obj_set_style_text_color(s_iaq_category, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(s_iaq_bar, lv_color_hex(color), LV_PART_INDICATOR);
        lv_bar_set_value(s_iaq_bar, (int32_t)(r.iaq > 500 ? 500 : r.iaq), LV_ANIM_OFF);
    }

    char status[128];
    switch (r.state) {
        case EnvSensorState::Disabled:
            snprintf(status, sizeof(status), "Sensor disabled in config (env_sensor.enabled)");
            break;
        case EnvSensorState::NoLibrary:
            snprintf(status, sizeof(status), "Firmware built without the Bosch BSEC library");
            break;
        case EnvSensorState::NotFound:
            snprintf(status, sizeof(status), "BME690 not found.\nCheck wiring: P4 IO19 (SDA) / IO20 (SCL), P5 3.3V / GND");
            break;
        case EnvSensorState::Error:
            snprintf(status, sizeof(status), "Sensor error at 0x%02x - see serial log", r.i2c_address);
            break;
        case EnvSensorState::WarmingUp:
            snprintf(status, sizeof(status), "BME690 at 0x%02x - warming up...", r.i2c_address);
            break;
        case EnvSensorState::Running: {
            const char *acc;
            switch (r.iaq_accuracy) {
                case 0: acc = "Stabilizing - IAQ not reliable yet"; break;
                case 1: acc = "Calibrating (accuracy 1/3)"; break;
                case 2: acc = "Calibrating (accuracy 2/3)"; break;
                default: acc = "Calibrated (accuracy 3/3)"; break;
            }
            snprintf(status, sizeof(status), "%s%s", acc, r.run_in_done ? "" : "\nNew-sensor run-in in progress");
            break;
        }
    }
    lv_label_set_text(s_status, status);
}
