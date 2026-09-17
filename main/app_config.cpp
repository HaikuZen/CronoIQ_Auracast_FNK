#include "app_config.h"

#include <cstdio>
#include <cstdlib>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "app_config";

static std::string read_file(const char *path, bool &ok) {
    ok = false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return {};
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        ESP_LOGE(TAG, "%s is empty", path);
        fclose(f);
        return {};
    }
    std::string content(static_cast<size_t>(size), '\0');
    size_t read = fread(content.data(), 1, static_cast<size_t>(size), f);
    fclose(f);
    if (read != static_cast<size_t>(size)) {
        ESP_LOGE(TAG, "Short read on %s (%u/%ld bytes)", path, (unsigned)read, size);
        return {};
    }
    ok = true;
    return content;
}

static const char *json_get_string(const cJSON *obj, const char *key, const char *fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return fallback;
}

static double json_get_number(const cJSON *obj, const char *key, double fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return fallback;
}

bool app_config_load(const char *path, AppConfig &out_config) {
    bool read_ok = false;
    std::string content = read_file(path, read_ok);
    if (!read_ok) {
        ESP_LOGW(TAG, "Falling back to built-in defaults");
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(content.c_str(), content.size());
    if (!root) {
        const char *err_at = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON parse error near: %.40s", err_at ? err_at : "(unknown)");
        return false;
    }

    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        out_config.wifi.ssid = json_get_string(wifi, "ssid", "");
        out_config.wifi.password = json_get_string(wifi, "password", "");
    } else {
        ESP_LOGW(TAG, "config_crono.json: missing \"wifi\" object");
    }

    const cJSON *ntp = cJSON_GetObjectItemCaseSensitive(root, "ntp");
    if (cJSON_IsObject(ntp)) {
        out_config.ntp.server = json_get_string(ntp, "server", out_config.ntp.server.c_str());
        out_config.ntp.posix_tz = json_get_string(ntp, "timezone", out_config.ntp.posix_tz.c_str());
    }

    const cJSON *weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    if (cJSON_IsObject(weather)) {
        out_config.weather.provider = json_get_string(weather, "provider", out_config.weather.provider.c_str());
        out_config.weather.api_key = json_get_string(weather, "api_key", "");
        out_config.weather.units = json_get_string(weather, "units", out_config.weather.units.c_str());
        out_config.weather.forecast_days =
            (int)json_get_number(weather, "forecast_days", out_config.weather.forecast_days);
        out_config.weather.update_interval_min =
            (uint32_t)json_get_number(weather, "update_interval_min", out_config.weather.update_interval_min);

        if (out_config.weather.provider != "visualcrossing") {
            ESP_LOGW(TAG, "weather.provider \"%s\" is not implemented yet — "
                          "falling back to visualcrossing behaviour",
                     out_config.weather.provider.c_str());
        }
        if (out_config.weather.forecast_days < 1) out_config.weather.forecast_days = 1;
        if (out_config.weather.forecast_days > 15) out_config.weather.forecast_days = 15;

        const cJSON *location = cJSON_GetObjectItemCaseSensitive(weather, "location");
        if (cJSON_IsObject(location)) {
            out_config.weather.location.name = json_get_string(location, "name", "Unknown");
            out_config.weather.location.latitude = json_get_number(location, "latitude", 0.0);
            out_config.weather.location.longitude = json_get_number(location, "longitude", 0.0);
        } else {
            ESP_LOGW(TAG, "config_crono.json: missing \"weather.location\" object");
        }
    } else {
        ESP_LOGW(TAG, "config_crono.json: missing \"weather\" object");
    }

    const cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (cJSON_IsObject(display)) {
        out_config.display.brightness_pct =
            (uint8_t)json_get_number(display, "brightness_pct", out_config.display.brightness_pct);
    }

    cJSON_Delete(root);

    if (out_config.wifi.ssid.empty()) {
        ESP_LOGW(TAG, "No wifi.ssid configured — device will stay offline");
    }
    if (out_config.weather.api_key.empty()) {
        ESP_LOGW(TAG, "No weather.api_key configured — weather page will stay empty");
    }

    ESP_LOGI(TAG, "Config loaded: ssid=\"%s\" tz=\"%s\" weather=%s@%.4f,%.4f days=%d",
             out_config.wifi.ssid.c_str(), out_config.ntp.posix_tz.c_str(),
             out_config.weather.location.name.c_str(), out_config.weather.location.latitude,
             out_config.weather.location.longitude, out_config.weather.forecast_days);
    return true;
}
