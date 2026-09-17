#include "page_weather.h"

#include <cstdio>

#include "weather_icons.h"

static lv_obj_t *s_location_label;
static lv_obj_t *s_current_icon;
static lv_obj_t *s_current_temp_label;
static lv_obj_t *s_current_conditions_label;
static lv_obj_t *s_forecast_row;
static int s_forecast_days = 5;

static lv_obj_t *make_forecast_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 140, 220);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x14283A), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return card;
}

lv_obj_t *page_weather_create(lv_obj_t *parent, int forecast_days) {
    s_forecast_days = forecast_days > 0 ? forecast_days : 5;

    lv_obj_set_style_bg_color(parent, lv_color_hex(WEATHER_ICON_BG_HEX), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 10, 0);

    // Header: current conditions
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), 120);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    s_current_icon = weather_icon_create(header, "", 96);
    lv_obj_set_style_pad_right(s_current_icon, 16, 0);

    lv_obj_t *text_col = lv_obj_create(header);
    lv_obj_remove_style_all(text_col);
    lv_obj_set_size(text_col, 500, 100);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);

    s_location_label = lv_label_create(text_col);
    lv_label_set_text(s_location_label, "--");
    lv_obj_set_style_text_color(s_location_label, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(s_location_label, &lv_font_montserrat_24, 0);

    s_current_temp_label = lv_label_create(text_col);
    lv_label_set_text(s_current_temp_label, "--°");
    lv_obj_set_style_text_color(s_current_temp_label, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(s_current_temp_label, &lv_font_montserrat_32, 0);

    s_current_conditions_label = lv_label_create(text_col);
    lv_label_set_text(s_current_conditions_label, "Waiting for data...");
    lv_obj_set_style_text_color(s_current_conditions_label, lv_color_hex(0x9AB4D0), 0);
    lv_obj_set_style_text_font(s_current_conditions_label, &lv_font_montserrat_16, 0);

    // Forecast: horizontally scrollable row of day cards
    s_forecast_row = lv_obj_create(parent);
    lv_obj_remove_style_all(s_forecast_row);
    lv_obj_set_size(s_forecast_row, LV_PCT(100), 230);
    lv_obj_align_to(s_forecast_row, header, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lv_obj_set_flex_flow(s_forecast_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_forecast_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_forecast_row, LV_DIR_HOR);

    return parent;
}

void page_weather_update(const WeatherData &data) {
    if (!data.valid) return;

    lv_label_set_text(s_location_label, data.location_name.c_str());
    weather_icon_update(s_current_icon, data.current_icon.c_str());

    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f°%s", data.current_temp, data.units_symbol.c_str());
    lv_label_set_text(s_current_temp_label, buf);
    lv_label_set_text(s_current_conditions_label, data.current_conditions.c_str());

    lv_obj_clean(s_forecast_row);
    int count = 0;
    for (const auto &day : data.forecast) {
        if (count >= s_forecast_days) break;
        lv_obj_t *card = make_forecast_card(s_forecast_row);

        lv_obj_t *day_label = lv_label_create(card);
        lv_label_set_text(day_label, day.day_label.c_str());
        lv_obj_set_style_text_color(day_label, lv_color_hex(0xE0F0FF), 0);
        lv_obj_set_style_text_font(day_label, &lv_font_montserrat_16, 0);

        weather_icon_create(card, day.icon.c_str(), 56);

        lv_obj_t *conditions_label = lv_label_create(card);
        lv_label_set_text(conditions_label, day.conditions.c_str());
        lv_obj_set_width(conditions_label, LV_PCT(100));
        lv_label_set_long_mode(conditions_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(conditions_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(conditions_label, lv_color_hex(0x9AB4D0), 0);
        lv_obj_set_style_text_font(conditions_label, &lv_font_montserrat_14, 0);

        char temps[24];
        snprintf(temps, sizeof(temps), "%.0f° / %.0f°", day.temp_max, day.temp_min);
        lv_obj_t *temp_label = lv_label_create(card);
        lv_label_set_text(temp_label, temps);
        lv_obj_set_style_text_color(temp_label, lv_color_hex(0x9AB4D0), 0);
        lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_16, 0);

        count++;
    }
}
