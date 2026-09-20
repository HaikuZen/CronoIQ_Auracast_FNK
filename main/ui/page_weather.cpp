#include "page_weather.h"

#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "weather_icons.h"

static const char *TAG = "page_weather";

static lv_obj_t *s_location_label;
static lv_obj_t *s_current_icon;
static lv_obj_t *s_current_temp_label;
static lv_obj_t *s_current_conditions_label;
static lv_obj_t *s_forecast_row;
static WeatherConfig s_cfg;
static std::vector<WeatherDay> s_forecast;
// -1 = showing the day-card list; otherwise an index into s_forecast whose
// hourly breakdown is currently drilled into.
static int s_selected_day = -1;
static bool s_hours_loading = false;
static bool s_hours_failed = false;
// Guards against a stale/late on-demand fetch reply overwriting state for a
// day the user has since navigated away from (tapped Back, tapped a
// different day, or a periodic refresh replaced s_forecast).
static std::string s_pending_fetch_date;

static void rebuild_forecast_view();

static void fetch_hours_for_selected_day() {
    if (s_selected_day < 0 || s_selected_day >= (int)s_forecast.size()) return;
    WeatherDay &day = s_forecast[s_selected_day];
    if (!day.hours.empty()) {
        ESP_LOGI(TAG, "Day %s already cached (%u hours) — not re-fetching", day.iso_date.c_str(),
                 (unsigned)day.hours.size());
        return;
    }

    s_hours_loading = true;
    s_hours_failed = false;
    s_pending_fetch_date = day.iso_date;
    ESP_LOGI(TAG, "Requesting hourly fetch for %s", day.iso_date.c_str());

    weather_service_fetch_hours(s_cfg, day.iso_date, [](bool success, const std::vector<HourForecast> &hours) {
        ESP_LOGI(TAG, "Hourly callback fired: success=%d, %u hour(s), pending_date=%s", (int)success,
                 (unsigned)hours.size(), s_pending_fetch_date.c_str());

        // Called from weather_service's own background task, not the LVGL
        // task — must take the lock before touching any LVGL object or
        // the page's LVGL-facing state, exactly like
        // ui_manager_update_weather() does for the periodic poll. A timeout
        // of 0 here means "wait indefinitely" (esp_lvgl_port's convention,
        // matching every other lvgl_port_lock() call in this codebase) so
        // this should never actually fail — but if it somehow does, don't
        // silently leave s_hours_loading stuck true forever: log it loudly
        // instead of swallowing it.
        if (!lvgl_port_lock(0)) {
            ESP_LOGE(TAG, "lvgl_port_lock() failed in hourly fetch callback for %s — UI will stay on "
                          "\"Loading...\" until the next tap",
                     s_pending_fetch_date.c_str());
            return;
        }

        if (s_selected_day >= 0 && s_selected_day < (int)s_forecast.size() &&
            s_forecast[s_selected_day].iso_date == s_pending_fetch_date) {
            s_hours_loading = false;
            if (success) {
                s_forecast[s_selected_day].hours = hours;
            } else {
                s_hours_failed = true;
            }
            rebuild_forecast_view();
        } else {
            ESP_LOGW(TAG, "Dropping stale hourly reply for %s (selection has since changed)",
                     s_pending_fetch_date.c_str());
        }

        lvgl_port_unlock();
    });
}

static void day_card_event_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)s_forecast.size()) return;
    ESP_LOGI(TAG, "Day card %d tapped (%s)", idx, s_forecast[idx].iso_date.c_str());
    s_selected_day = idx;
    fetch_hours_for_selected_day();
    rebuild_forecast_view();
}

static void back_to_days_event_cb(lv_event_t *e) {
    (void)e;
    s_selected_day = -1;
    s_hours_loading = false;
    s_hours_failed = false;
    rebuild_forecast_view();
}

static lv_obj_t *make_forecast_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 140, 220);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x14283A), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return card;
}

// Rebuilds s_forecast_row as either the tappable day-card list (default) or,
// once a card has been tapped, a "< Back" row plus a horizontally
// scrollable hour-by-hour breakdown for that one day (loading/failed/loaded
// states — hours are fetched on demand, not with the periodic poll). Both
// views reuse the same fixed-size container so no other layout on the page
// needs to move.
static void rebuild_forecast_view() {
    lv_obj_clean(s_forecast_row);

    if (s_selected_day < 0 || s_selected_day >= (int)s_forecast.size()) {
        lv_obj_set_scroll_dir(s_forecast_row, LV_DIR_HOR);
        lv_obj_set_flex_flow(s_forecast_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_forecast_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        for (size_t i = 0; i < s_forecast.size() && (int)i < s_cfg.forecast_days; i++) {
            const WeatherDay &day = s_forecast[i];
            lv_obj_t *card = make_forecast_card(s_forecast_row);
            lv_obj_add_event_cb(card, day_card_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

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
        }
        return;
    }

    const WeatherDay &day = s_forecast[s_selected_day];

    lv_obj_set_scroll_dir(s_forecast_row, LV_DIR_NONE);
    lv_obj_set_flex_flow(s_forecast_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_forecast_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *back_row = lv_obj_create(s_forecast_row);
    lv_obj_remove_style_all(back_row);
    lv_obj_set_size(back_row, LV_PCT(100), 30);
    lv_obj_set_flex_flow(back_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(back_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_label = lv_label_create(back_row);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_label, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_add_flag(back_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_label, back_to_days_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *title_label = lv_label_create(back_row);
    char title_buf[48];
    snprintf(title_buf, sizeof(title_buf), "   %s hourly forecast", day.day_label.c_str());
    lv_label_set_text(title_label, title_buf);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);

    lv_obj_t *hours_row = lv_obj_create(s_forecast_row);
    lv_obj_remove_style_all(hours_row);
    lv_obj_set_width(hours_row, LV_PCT(100));
    lv_obj_set_flex_grow(hours_row, 1);
    lv_obj_set_flex_flow(hours_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hours_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hours_row, 8, 0);
    lv_obj_set_scroll_dir(hours_row, LV_DIR_HOR);

    if (day.hours.empty()) {
        lv_obj_t *placeholder = lv_label_create(hours_row);
        if (s_hours_loading) {
            lv_label_set_text(placeholder, "Loading hourly forecast...");
        } else if (s_hours_failed) {
            lv_label_set_text(placeholder, "Couldn't load hourly forecast — go back and try again");
        } else {
            lv_label_set_text(placeholder, "No hourly data for this day");
        }
        lv_obj_set_style_text_color(placeholder, lv_color_hex(0x9AB4D0), 0);
        lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_16, 0);
        return;
    }

    // Uses weather_icon_dot_create() (one object) rather than
    // weather_icon_create() (up to ~9 objects for a sun) — creating this
    // many icons in one synchronous burst under the esp_lvgl_port lock,
    // from a background task, was observed hanging hard enough to trip the
    // task watchdog repeatedly (~288 objects total for 24 full vector
    // icons). See CLAUDE.md.
    int card_index = 0;
    for (const auto &hour : day.hours) {
        lv_obj_t *card = lv_obj_create(hours_row);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 78, LV_PCT(100));
        lv_obj_set_style_bg_color(card, lv_color_hex(0x14283A), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *time_label = lv_label_create(card);
        lv_label_set_text(time_label, hour.time_label.c_str());
        lv_obj_set_style_text_color(time_label, lv_color_hex(0xE0F0FF), 0);
        lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);

        weather_icon_dot_create(card, hour.icon.c_str(), 20);

        char temp_buf[16];
        snprintf(temp_buf, sizeof(temp_buf), "%.0f°", hour.temp);
        lv_obj_t *temp_label = lv_label_create(card);
        lv_label_set_text(temp_label, temp_buf);
        lv_obj_set_style_text_color(temp_label, lv_color_hex(0x9AB4D0), 0);
        lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_14, 0);

        // Defensive: even at the reduced per-card cost, yield periodically
        // so other tasks (notably the idle tasks that feed the task
        // watchdog) get a chance to run during this still-nontrivial
        // synchronous burst.
        if (++card_index % 6 == 0) {
            vTaskDelay(1);
        }
    }
}

lv_obj_t *page_weather_create(lv_obj_t *parent, const WeatherConfig &cfg) {
    s_cfg = cfg;

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

    // Forecast: either the tappable day-card list or a drilled-into day's
    // hourly breakdown — see rebuild_forecast_view().
    s_forecast_row = lv_obj_create(parent);
    lv_obj_remove_style_all(s_forecast_row);
    lv_obj_set_size(s_forecast_row, LV_PCT(100), 230);
    lv_obj_align_to(s_forecast_row, header, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

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

    // The periodic poll never carries hours (they're fetched on demand —
    // see fetch_hours_for_selected_day()), so a naive `s_forecast =
    // data.forecast` would drop the currently-viewed day's hours on every
    // refresh. Carry them over when the same date is still present.
    std::string previously_selected_date;
    std::vector<HourForecast> previously_selected_hours;
    if (s_selected_day >= 0 && s_selected_day < (int)s_forecast.size()) {
        previously_selected_date = s_forecast[s_selected_day].iso_date;
        previously_selected_hours = s_forecast[s_selected_day].hours;
    }

    s_forecast = data.forecast;
    s_selected_day = -1;
    if (!previously_selected_date.empty()) {
        for (size_t i = 0; i < s_forecast.size(); i++) {
            if (s_forecast[i].iso_date == previously_selected_date) {
                s_selected_day = (int)i;
                s_forecast[i].hours = previously_selected_hours;
                break;
            }
        }
    }
    // If a fetch was in flight for a day that just got deselected (dropped
    // out of the refreshed window, or none was selected to begin with),
    // stop showing its loading/failed state.
    if (s_selected_day < 0) {
        s_hours_loading = false;
        s_hours_failed = false;
    }

    rebuild_forecast_view();
}
