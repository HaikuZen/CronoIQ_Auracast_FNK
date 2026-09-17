#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app_config.h"

struct WeatherDay {
    std::string day_label;   // e.g. "Mon 15"
    std::string icon;        // Visual Crossing icon code, e.g. "partly-cloudy-day"
    std::string conditions;  // e.g. "Partially cloudy"
    double temp_min = 0.0;
    double temp_max = 0.0;
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
