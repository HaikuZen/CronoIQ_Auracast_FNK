#pragma once

#include "lvgl.h"

lv_obj_t *page_clock_create(lv_obj_t *parent);

// Called once a second (from a lv_timer, already on the LVGL thread) to
// refresh weekday/date/time and the Wi-Fi/NTP status row.
void page_clock_tick(void);

// Updates the small current-conditions icon in the page's top-right corner.
void page_clock_set_weather_icon(const char *icon_code);
