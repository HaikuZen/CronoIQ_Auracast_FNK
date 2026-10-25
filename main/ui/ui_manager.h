#pragma once

#include "app_config.h"
#include "env_sensor_service.h"
#include "smart_lights_service.h"
#include "weather_service.h"

// Builds the top-level tabview (tabs on top: "Clock", "Weather", "Smart
// Lights", "Indoor") and starts the 1 Hz clock refresh timer. Call once, after
// board_init(), with the LVGL port already locked by the caller.
void ui_manager_create(const AppConfig &cfg);

// Thread-safe: takes the LVGL port lock itself, so it can be called directly
// from the weather_service background task's callback.
void ui_manager_update_weather(const WeatherData &data);

// Thread-safe: takes the LVGL port lock itself, so it can be called directly
// from the smart_lights_service background task's callback.
void ui_manager_update_smart_lights(const std::vector<LightStatus> &lights);

// Thread-safe: takes the LVGL port lock itself, so it can be called directly
// from the env_sensor_service background task's callback.
void ui_manager_update_env(const EnvReading &reading);
