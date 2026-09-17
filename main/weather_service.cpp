#include "weather_service.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

static const char *TAG = "weather_service";

// Visual Crossing Timeline API. `include=days,current` is enough for both
// the clock page's current-conditions icon and the weather page's forecast
// list; we ask for a wide window and slice to forecast_days client-side
// rather than fighting the API's date-range syntax.
static std::string build_url(const WeatherConfig &cfg) {
    char url[384];
    snprintf(url, sizeof(url),
             "https://weather.visualcrossing.com/VisualCrossingWebServices/rest/services/"
             "timeline/%.6f,%.6f?unitGroup=%s&include=days,current&contentType=json&key=%s",
             cfg.location.latitude, cfg.location.longitude,
             cfg.units == "us" ? "us" : "metric", cfg.api_key.c_str());
    return std::string(url);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    // Note: no is_chunked_response() guard here on purpose. That check only
    // matters for a fixed-size preallocated buffer (sized from
    // Content-Length, which a chunked response doesn't have) — we append
    // into a dynamically-growing std::string, so chunked bodies are fine.
    // Visual Crossing's response is chunked in practice; excluding it here
    // used to silently drop the entire body, leaving parse_response() to
    // fail on an empty string.
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr) {
        auto *buf = static_cast<std::string *>(evt->user_data);
        buf->append(static_cast<const char *>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

static bool fetch_raw(const std::string &url, std::string &response) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = http_event_handler;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    config.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d (body: %.120s)", status, response.c_str());
        return false;
    }
    return true;
}

static std::string format_day_label(const char *iso_date) {
    struct tm t = {};
    if (!strptime(iso_date, "%Y-%m-%d", &t)) {
        return iso_date;
    }
    // strptime() with "%Y-%m-%d" only fills tm_year/tm_mon/tm_mday — tm_wday
    // stays at its zero-init value (Sunday) unless recomputed. mktime()
    // normalizes the struct and derives the real weekday from the date.
    t.tm_hour = 12;  // avoid landing on a DST transition at midnight
    mktime(&t);
    char buf[16];
    strftime(buf, sizeof(buf), "%a %d", &t);
    return std::string(buf);
}

static bool parse_response(const std::string &body, const WeatherConfig &cfg, WeatherData &out) {
    cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!root) {
        const char *err_at = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "Failed to parse weather JSON (body: %u bytes, near: \"%.60s\", starts: \"%.60s\")",
                 (unsigned)body.size(), err_at ? err_at : "(unknown)", body.c_str());
        return false;
    }

    out = WeatherData{};
    out.location_name = cfg.location.name;
    out.units_symbol = (cfg.units == "us") ? "F" : "C";

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "currentConditions");
    if (cJSON_IsObject(current)) {
        const cJSON *icon = cJSON_GetObjectItemCaseSensitive(current, "icon");
        const cJSON *conditions = cJSON_GetObjectItemCaseSensitive(current, "conditions");
        const cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temp");
        out.current_icon = cJSON_IsString(icon) ? icon->valuestring : "";
        out.current_conditions = cJSON_IsString(conditions) ? conditions->valuestring : "";
        out.current_temp = cJSON_IsNumber(temp) ? temp->valuedouble : 0.0;
    } else {
        ESP_LOGW(TAG, "Response has no currentConditions");
    }

    const cJSON *days = cJSON_GetObjectItemCaseSensitive(root, "days");
    if (cJSON_IsArray(days)) {
        int i = 0;
        const cJSON *day = nullptr;
        cJSON_ArrayForEach(day, days) {
            if (i >= cfg.forecast_days) break;
            WeatherDay wd;
            const cJSON *datetime = cJSON_GetObjectItemCaseSensitive(day, "datetime");
            const cJSON *icon = cJSON_GetObjectItemCaseSensitive(day, "icon");
            const cJSON *conditions = cJSON_GetObjectItemCaseSensitive(day, "conditions");
            const cJSON *tmax = cJSON_GetObjectItemCaseSensitive(day, "tempmax");
            const cJSON *tmin = cJSON_GetObjectItemCaseSensitive(day, "tempmin");
            wd.day_label = cJSON_IsString(datetime) ? format_day_label(datetime->valuestring) : "?";
            wd.icon = cJSON_IsString(icon) ? icon->valuestring : "";
            wd.conditions = cJSON_IsString(conditions) ? conditions->valuestring : "";
            wd.temp_max = cJSON_IsNumber(tmax) ? tmax->valuedouble : 0.0;
            wd.temp_min = cJSON_IsNumber(tmin) ? tmin->valuedouble : 0.0;
            out.forecast.push_back(wd);
            i++;
        }
    } else {
        ESP_LOGW(TAG, "Response has no days[] array");
    }

    cJSON_Delete(root);
    out.valid = true;
    return true;
}

struct TaskArgs {
    WeatherConfig cfg;
    WeatherUpdateCb cb;
};

static void weather_task(void *pvArgs) {
    auto *args = static_cast<TaskArgs *>(pvArgs);
    const std::string url = build_url(args->cfg);

    while (true) {
        while (!wifi_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        std::string body;
        if (fetch_raw(url, body)) {
            WeatherData data;
            if (parse_response(body, args->cfg, data)) {
                ESP_LOGI(TAG, "Weather updated: %.1f%s, %s, %d forecast day(s)",
                         data.current_temp, data.units_symbol.c_str(),
                         data.current_conditions.c_str(), (int)data.forecast.size());
                args->cb(data);
            }
        }

        uint32_t interval_min = args->cfg.update_interval_min > 0 ? args->cfg.update_interval_min : 30;
        vTaskDelay(pdMS_TO_TICKS(interval_min * 60 * 1000));
    }
}

void weather_service_start(const WeatherConfig &cfg, WeatherUpdateCb on_update) {
    if (cfg.api_key.empty()) {
        ESP_LOGW(TAG, "No api_key configured — weather service not started");
        return;
    }
    if (cfg.provider != "visualcrossing") {
        ESP_LOGW(TAG, "Provider \"%s\" not implemented, using visualcrossing request shape anyway",
                 cfg.provider.c_str());
    }

    auto *args = new TaskArgs{cfg, std::move(on_update)};
    xTaskCreate(weather_task, "weather_task", 8192, args, tskIDLE_PRIORITY + 3, nullptr);
}
