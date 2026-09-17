#include "ui_manager.h"

#include "esp_lvgl_port.h"
#include "page_clock.h"
#include "page_weather.h"
#include "weather_icons.h"

static void clock_tick_timer_cb(lv_timer_t *timer) {
    (void)timer;
    page_clock_tick();
}

void ui_manager_create(const AppConfig &cfg) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(WEATHER_ICON_BG_HEX), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *tabview = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(tabview, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tabview, 50);
    lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));

    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(0x081018), 0);
    lv_obj_set_style_text_color(tab_bar, lv_color_hex(0xE0F0FF), 0);

    lv_obj_t *tab_clock = lv_tabview_add_tab(tabview, "Clock");
    lv_obj_t *tab_weather = lv_tabview_add_tab(tabview, "Weather");

    page_clock_create(tab_clock);
    page_weather_create(tab_weather, cfg.weather.forecast_days);

    page_clock_tick();
    lv_timer_create(clock_tick_timer_cb, 1000, nullptr);
}

void ui_manager_update_weather(const WeatherData &data) {
    if (lvgl_port_lock(0)) {
        page_weather_update(data);
        page_clock_set_weather_icon(data.current_icon.c_str());
        lvgl_port_unlock();
    }
}
