#pragma once

#include "lvgl.h"
#include "weather_service.h"

lv_obj_t *page_weather_create(lv_obj_t *parent, int forecast_days);

// Rebuilds the current-conditions header and the forecast day cards.
// Must be called with the LVGL port already locked.
void page_weather_update(const WeatherData &data);
