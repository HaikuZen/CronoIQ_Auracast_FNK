#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app_config.h"

struct HourForecast {
    std::string time_label;  // "HH:MM", local time as reported by the provider
    std::string icon;
    double temp = 0.0;
};

struct WeatherDay {
    std::string iso_date;    // "YYYY-MM-DD", needed to fetch this day's hours on demand
    std::string day_label;   // e.g. "Mon 15"
    std::string icon;        // Visual Crossing icon code, e.g. "partly-cloudy-day"
    std::string conditions;  // e.g. "Partially cloudy"
    double temp_min = 0.0;
    double temp_max = 0.0;
    // Empty until fetched on demand — see weather_service_fetch_hours().
    // The periodic poll deliberately does NOT request hourly data for
    // every day (that used to cost ~24x more per poll); it's fetched only
    // for one day at a time, when the UI actually needs it.
    std::vector<HourForecast> hours;
};

struct WeatherData {
    bool valid = false;
    std::string location_name;
    std::string current_icon;
    std::string current_conditions;
    double current_temp = 0.0;
    std::string units_symbol;   // "C" or "F"
    std::vector<WeatherDay> forecast;
};

using WeatherUpdateCb = std::function<void(const WeatherData &)>;

// Spawns a background task that periodically fetches the forecast from the
// configured provider (Visual Crossing in this release) and invokes
// on_update with the parsed result every time a fetch succeeds. Does
// nothing (logs a warning) if no api_key is configured.
void weather_service_start(const WeatherConfig &cfg, WeatherUpdateCb on_update);

// success=false means the fetch failed or another weather request was
// already in flight (see the in-flight guard in weather_service.cpp) —
// hours will be empty in that case.
using HourlyForecastCb = std::function<void(bool success, const std::vector<HourForecast> &hours)>;

// One-shot, on-demand fetch of a single day's hourly breakdown (used by the
// Weather page's tap-to-drill-down). Spawns its own short-lived FreeRTOS
// task and calls on_result from that task — NOT the LVGL task — so the
// callback must take the esp_lvgl_port lock itself before touching any
// LVGL object, exactly like weather_service_start()'s callback.
void weather_service_fetch_hours(const WeatherConfig &cfg, const std::string &iso_date, HourlyForecastCb on_result);
