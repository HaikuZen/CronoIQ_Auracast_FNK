#pragma once

#include <string>

#include "env_sensor_service.h"
#include "lvgl.h"

// "Indoor" page. A "Now | History" switch at the top selects between:
//   Now     — BME690 air quality (BSEC IAQ index + category + calibration
//             status) on the left, temperature/humidity/pressure/CO2 eq/
//             TVOC eq/gas resistance tiles on the right.
//   History — a line graph of one metric (Temp/Humidity/Pressure/IAQ/CO2)
//             over the last 3h or 24h, from env_history (RAM only). `units` is the weather config's unit group
// ("metric" or "us") so indoor temperature matches the Weather page's °C/°F.
lv_obj_t *page_environment_create(lv_obj_t *parent, const std::string &units);

// Updates the page in place (no object churn — this runs every 3s). Must be
// called with the LVGL port already locked.
void page_environment_update(const EnvReading &reading);
