#include "page_clock.h"

#include "seven_segment.h"
#include "time_manager.h"
#include "weather_icons.h"
#include "wifi_manager.h"

static lv_obj_t *s_weekday_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_time_widget;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_ntp_label;
static lv_obj_t *s_weather_icon;

lv_obj_t *page_clock_create(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(WEATHER_ICON_BG_HEX), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 8, 0);

    // Row 1: weekday
    s_weekday_label = lv_label_create(parent);
    lv_label_set_text(s_weekday_label, "--");
    lv_obj_set_style_text_color(s_weekday_label, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(s_weekday_label, &lv_font_montserrat_38, 0);
    lv_obj_align(s_weekday_label, LV_ALIGN_TOP_MID, 0, 4);

    // Row 2: date
    s_date_label = lv_label_create(parent);
    lv_label_set_text(s_date_label, "--");
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0x9AB4D0), 0);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_32, 0);
    lv_obj_align_to(s_date_label, s_weekday_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    // Current-conditions icon, top-right corner
    s_weather_icon = weather_icon_create(parent, "", 64);
    lv_obj_align(s_weather_icon, LV_ALIGN_TOP_RIGHT, -4, 4);

    // Middle: 7-segment HH:MM:ss
    s_time_widget = sseg_time_create(parent, 150, lv_color_hex(0x00E5FF), lv_color_hex(0x143040));
    sseg_time_set_text(s_time_widget, "00:00:00");
    lv_obj_align(s_time_widget, LV_ALIGN_CENTER, 0, 10);

    // Bottom status row: Wi-Fi + NTP
    lv_obj_t *status_row = lv_obj_create(parent);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, LV_PCT(100), 30);
    lv_obj_align(status_row, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_label = lv_label_create(status_row);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI " Wi-Fi: off");
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0xAAAAAA), 0);

    s_ntp_label = lv_label_create(status_row);
    lv_label_set_text(s_ntp_label, LV_SYMBOL_REFRESH " NTP: not synced");
    lv_obj_set_style_text_font(s_ntp_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_ntp_label, lv_color_hex(0xAAAAAA), 0);

    return parent;
}

void page_clock_tick(void) {
    char weekday[24];
    char date[32];
    char time_str[16];
    time_manager_get_weekday_str(weekday, sizeof(weekday));
    time_manager_get_date_str(date, sizeof(date));
    time_manager_get_time_str(time_str, sizeof(time_str));

    lv_label_set_text(s_weekday_label, weekday);
    lv_label_set_text(s_date_label, date);
    sseg_time_set_text(s_time_widget, time_str);

    bool wifi_ok = wifi_manager_is_connected();
    lv_label_set_text(s_wifi_label, wifi_ok ? LV_SYMBOL_WIFI " Wi-Fi: on" : LV_SYMBOL_WIFI " Wi-Fi: off");
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(wifi_ok ? 0x4CD964 : 0xFF5A5A), 0);

    bool ntp_ok = time_manager_is_synced();
    lv_label_set_text(s_ntp_label, ntp_ok ? LV_SYMBOL_REFRESH " NTP: synced" : LV_SYMBOL_REFRESH " NTP: not synced");
    lv_obj_set_style_text_color(s_ntp_label, lv_color_hex(ntp_ok ? 0x4CD964 : 0xFF5A5A), 0);
}

void page_clock_set_weather_icon(const char *icon_code) {
    if (s_weather_icon) weather_icon_update(s_weather_icon, icon_code);
}
