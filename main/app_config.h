#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Mirrors /sdcard/config_crono.json. See README.md / sdcard/config_crono.json
// for the documented schema and an example file.

struct WifiConfig {
    std::string ssid;
    std::string password;
};

struct NtpConfig {
    std::string server = "pool.ntp.org";
    // POSIX TZ string (handles DST rules), e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
    // for Central Europe. Defaults to UTC if not set in the config file.
    std::string posix_tz = "UTC0";
};

struct WeatherLocation {
    std::string name = "Unknown";
    double latitude = 0.0;
    double longitude = 0.0;
};

struct WeatherConfig {
    // Only "visualcrossing" is implemented in this first release; the field
    // exists so additional providers can be added later without a schema
    // change.
    std::string provider = "visualcrossing";
    std::string api_key;
    WeatherLocation location;
    int forecast_days = 5;
    // Visual Crossing bills by records returned, not by call count — see
    // the cost note above weather_service.cpp's build_url() and the config
    // table in README.md before lowering this. 30 minutes was the original
    // default but is too aggressive now that hourly detail is fetched too;
    // 60 is a more conservative starting point, still tunable per-plan.
    uint32_t update_interval_min = 60;
    // "metric" (°C) or "us" (°F) — passed straight through as Visual
    // Crossing's unitGroup query parameter.
    std::string units = "metric";
};

struct DisplayConfig {
    uint8_t brightness_pct = 100;
};

struct SmartLightConfig {
    std::string name;
    // Only "WiZ" is implemented in this first release; the field exists so
    // other brands (e.g. "Philips Hue") can be added later without a schema
    // change — mirrors WeatherConfig.provider.
    std::string brand = "WiZ";
    std::string ip;
    uint16_t udp_port = 38899;
};

struct EnvSensorConfig {
    // BME690 on the shared touch I2C bus (connector P4 IO19/IO20). Set false
    // to skip probing entirely (e.g. no sensor fitted).
    bool enabled = true;
    // 0x76 (SDO to GND) or 0x77 (SDO to 3.3V); 0 = probe both, in that order.
    uint8_t i2c_address = 0;
    // Degrees C subtracted from the sensor's temperature to compensate for
    // heat from the board/display around it (fed to BSEC as
    // BSEC_INPUT_HEATSOURCE, so humidity is corrected consistently too).
    // Bosch's own figure for a bare sensor in LP mode is ~0.15; a sensor
    // mounted on/near the FNK0115 will need more — calibrate against a
    // reference thermometer.
    float temp_offset_c = 0.15f;
};

struct AppConfig {
    WifiConfig wifi;
    NtpConfig ntp;
    WeatherConfig weather;
    DisplayConfig display;
    std::vector<SmartLightConfig> smart_lights;
    EnvSensorConfig env_sensor;
};

// Reads and parses the JSON config file at `path`. On any failure (missing
// file, unmounted card, malformed JSON) logs the reason, leaves `out_config`
// at its built-in defaults, and returns false — the caller should keep
// booting rather than treat this as fatal.
bool app_config_load(const char *path, AppConfig &out_config);
