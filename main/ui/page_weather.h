#pragma once

#include "lvgl.h"
#include "weather_service.h"

// cfg is copied (location/units/api_key/forecast_days are needed later for
// on-demand hourly fetches triggered by tapping a day card).
lv_obj_t *page_weather_create(lv_obj_t *parent, const WeatherConfig &cfg);

// Rebuilds the current-conditions header and the forecast day cards.
// Must be called with the LVGL port already locked.
void page_weather_update(const WeatherData &data);
