#pragma once

#include <cstdint>
#include <functional>

#include "app_config.h"

enum class EnvSensorState {
    Disabled,      // env_sensor.enabled = false in config
    NoLibrary,     // built without Bosch BSEC (see components/bsec/CMakeLists.txt)
    NotFound,      // nothing answered at 0x76/0x77 — wiring/power; keeps re-probing
    Error,         // sensor found but bme69x/BSEC init failed — see log
    WarmingUp,     // running, no BSEC output yet (first gas sample takes a few s)
    Running,       // fields below are valid
};

struct EnvReading {
    EnvSensorState state = EnvSensorState::NotFound;
    uint8_t i2c_address = 0;  // address the sensor was found at, 0 if not found

    float temperature_c = 0;     // heat-compensated
    float humidity_pct = 0;      // heat-compensated
    float pressure_hpa = 0;
    float gas_resistance_ohm = 0;
    float iaq = 0;               // 0-500, Bosch IAQ index (lower is better)
    float static_iaq = 0;
    float co2_ppm = 0;           // CO2 *equivalent* estimated from VOCs, not measured CO2
    float tvoc_ppb = 0;          // TVOC equivalent
    float gas_percentage = 0;
    // BSEC's IAQ accuracy: 0 = stabilizing/uncalibrated, 1 = low (calibrating),
    // 2 = medium, 3 = high (fully calibrated). Persisted calibration state
    // (NVS) brings this back quickly after a reboot.
    uint8_t iaq_accuracy = 0;
    bool stabilized = false;     // BSEC stabilization status
    bool run_in_done = false;    // BSEC run-in status (new sensor burn-in)
};

using EnvUpdateCb = std::function<void(const EnvReading &)>;

// Spawns a background task that finds the BME690 on the board's shared I2C
// bus (board_get_i2c_bus() — call after board_init()), runs it under Bosch
// BSEC in low-power mode (one sample every 3s) and invokes on_update after
// every BSEC output, plus on every state change (not found, error, ...) so
// the UI can explain why there's no data. on_update runs on that background
// task, not the LVGL task. BSEC calibration state is saved to NVS hourly and
// restored at boot (NVS is initialized here if wifi_manager_start() hasn't
// already).
void env_sensor_service_start(const EnvSensorConfig &cfg, EnvUpdateCb on_update);
