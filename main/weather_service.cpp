#include "weather_service.h"

#include <atomic>
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
#include "time_manager.h"
#include "wifi_manager.h"

static const char *TAG = "weather_service";

// Visual Crossing bills by *records returned*, not by call count. This
// requires an explicit [today, today+forecast_days-1] date range (rather
// than Visual Crossing's open-ended ~15-day default, which costs ~15x more
// per poll) plus `include=days,current` — hourly detail is fetched
// separately, on demand, only for the one day the user actually taps (see
// weather_service_fetch_hours()). See README.md's "Visual Crossing API
// cost" section for concrete numbers.
//
// The date range needs a roughly-correct local clock — callers must only
// invoke this once time_manager_is_synced() is true (weather_task() waits
// for it below). There is deliberately no undated fallback for the
// unsynced case: that used to cost dramatically more per poll (Visual
// Crossing's full ~15-day window vs. just forecast_days) every single time
// it ran before NTP happened to land, which on a device with a slow/stuck
// NTP sync could mean many costly polls in a row instead of a few cheap
// ones deferred by a short wait.
static std::string build_periodic_url(const WeatherConfig &cfg) {
    time_t now = time(nullptr);
    struct tm start_tm;
    localtime_r(&now, &start_tm);
    start_tm.tm_hour = 12;  // avoid landing on a DST transition at midnight
    start_tm.tm_min = 0;
    start_tm.tm_sec = 0;
    mktime(&start_tm);

    struct tm end_tm = start_tm;
    end_tm.tm_mday += (cfg.forecast_days - 1);
    mktime(&end_tm);  // normalizes month/year rollover

    char start_buf[11];
    char end_buf[11];
    strftime(start_buf, sizeof(start_buf), "%Y-%m-%d", &start_tm);
    strftime(end_buf, sizeof(end_buf), "%Y-%m-%d", &end_tm);

    char url[448];
    snprintf(url, sizeof(url),
             "https://weather.visualcrossing.com/VisualCrossingWebServices/rest/services/"
             "timeline/%.6f,%.6f/%s/%s?unitGroup=%s&include=days,current&contentType=json&key=%s",
             cfg.location.latitude, cfg.location.longitude, start_buf, end_buf,
             cfg.units == "us" ? "us" : "metric", cfg.api_key.c_str());
    return std::string(url);
}

// Single day, hours only — the cheapest request that gets the drill-down
// what it needs (~24 records, once, only when the user taps a day).
static std::string build_hourly_url(const WeatherConfig &cfg, const std::string &iso_date) {
    char url[448];
    snprintf(url, sizeof(url),
             "https://weather.visualcrossing.com/VisualCrossingWebServices/rest/services/"
             "timeline/%.6f,%.6f/%s?unitGroup=%s&include=days,hours&contentType=json&key=%s",
             cfg.location.latitude, cfg.location.longitude, iso_date.c_str(),
             cfg.units == "us" ? "us" : "metric", cfg.api_key.c_str());
    return std::string(url);
}

static std::string redact_key(const std::string &url) {
    size_t key_pos = url.find("&key=");
    return key_pos == std::string::npos ? url : url.substr(0, key_pos) + "&key=***";
}

// At most one weather HTTP/TLS request in flight at a time, system-wide —
// the periodic poll and an on-demand hourly fetch could otherwise overlap
// (e.g. the user taps a day right as the periodic poll fires), and two
// simultaneous mbedTLS handshakes competing for internal RAM is a
// plausible contributor to a real `ESP_ERR_NO_MEM` crash seen during WiFi
// PHY re-enable shortly after a weather fetch. A skipped periodic poll
// just retries next interval; a skipped on-demand fetch reports failure
// and the UI lets the user retry.
static std::atomic<bool> s_fetch_in_flight{false};

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
    bool expected = false;
    if (!s_fetch_in_flight.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "Skipping fetch — another weather request is already in flight");
        return false;
    }
    // Released on every return path below, success or failure.
    struct InFlightGuard {
        ~InFlightGuard() { s_fetch_in_flight.store(false); }
    } in_flight_guard;

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

// Shared by the periodic per-day loop (normally a no-op there now, since
// the periodic request no longer includes "hours" — see build_periodic_url)
// and the on-demand single-day fetch (hourly_fetch_task), so both paths
// parse a day's "hours" array identically.
static void parse_hours_array(const cJSON *hours_json, std::vector<HourForecast> &out) {
    if (!cJSON_IsArray(hours_json)) return;
    const cJSON *hour = nullptr;
    cJSON_ArrayForEach(hour, hours_json) {
        const cJSON *hdatetime = cJSON_GetObjectItemCaseSensitive(hour, "datetime");
        const cJSON *hicon = cJSON_GetObjectItemCaseSensitive(hour, "icon");
        const cJSON *htemp = cJSON_GetObjectItemCaseSensitive(hour, "temp");
        HourForecast hf;
        // Visual Crossing's hour "datetime" is already "HH:MM:SS" in the
        // location's local time — just take "HH:MM".
        hf.time_label = (cJSON_IsString(hdatetime) && strlen(hdatetime->valuestring) >= 5)
                            ? std::string(hdatetime->valuestring, 5)
                            : "--:--";
        hf.icon = cJSON_IsString(hicon) ? hicon->valuestring : "";
        hf.temp = cJSON_IsNumber(htemp) ? htemp->valuedouble : 0.0;
        out.push_back(hf);
    }
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
            wd.iso_date = cJSON_IsString(datetime) ? datetime->valuestring : "";
            wd.day_label = cJSON_IsString(datetime) ? format_day_label(datetime->valuestring) : "?";
            wd.icon = cJSON_IsString(icon) ? icon->valuestring : "";
            wd.conditions = cJSON_IsString(conditions) ? conditions->valuestring : "";
            wd.temp_max = cJSON_IsNumber(tmax) ? tmax->valuedouble : 0.0;
            wd.temp_min = cJSON_IsNumber(tmin) ? tmin->valuedouble : 0.0;
            // Normally a no-op here — the periodic request doesn't include
            // "hours" (see build_periodic_url) — but harmless to parse
            // generically in case it's ever present.
            parse_hours_array(cJSON_GetObjectItemCaseSensitive(day, "hours"), wd.hours);

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

    while (true) {
        while (!wifi_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        // build_periodic_url() requires a roughly-correct clock for its
        // date range and has no undated fallback (see its comment) — wait
        // here instead, so a slow/stuck NTP sync means a short deferred
        // wait rather than repeated costly polls.
        while (!time_manager_is_synced()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        // Rebuilt every cycle, not cached: the requested date range needs
        // to slide forward a day at a time.
        const std::string url = build_periodic_url(args->cfg);
        // Logged at INFO (with the API key redacted — this ends up in
        // serial logs people paste into chat/bug reports) specifically so
        // the requested date range is visible without a debug build: the
        // easiest way to confirm it's actually anchored to today, not just
        // that some plausible-looking data came back.
        ESP_LOGI(TAG, "Fetching: %s", redact_key(url).c_str());

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
    if (xTaskCreate(weather_task, "weather_task", 8192, args, tskIDLE_PRIORITY + 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed for the periodic weather task (out of memory?) — weather will never update");
        delete args;
    }
}

struct HourlyFetchArgs {
    WeatherConfig cfg;
    std::string iso_date;
    HourlyForecastCb cb;
};

static void hourly_fetch_task(void *pvArgs) {
    auto *args = static_cast<HourlyFetchArgs *>(pvArgs);
    std::vector<HourForecast> hours;
    bool ok = false;

    ESP_LOGI(TAG, "Hourly fetch task started for %s", args->iso_date.c_str());

    // Bounded wait, not the open-ended one weather_task uses: this is a
    // one-shot fetch the user is actively waiting on (they just tapped a
    // day card), so hanging indefinitely on a dead Wi-Fi connection would
    // just leave the UI stuck on "Loading..." forever instead of reporting
    // failure.
    for (int attempt = 0; attempt < 25 && !wifi_manager_is_connected(); attempt++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "Hourly fetch for %s: Wi-Fi not connected after waiting, giving up", args->iso_date.c_str());
    } else {
        const std::string url = build_hourly_url(args->cfg, args->iso_date);
        ESP_LOGI(TAG, "Fetching hourly for %s: %s", args->iso_date.c_str(), redact_key(url).c_str());

        std::string body;
        bool fetched = fetch_raw(url, body);
        ESP_LOGI(TAG, "Hourly fetch_raw for %s: %s, %u byte body", args->iso_date.c_str(),
                 fetched ? "OK" : "FAILED", (unsigned)body.size());
        if (!body.empty()) {
            // Full response is usually only a few KB for one day of hours;
            // logged in chunks since ESP_LOG truncates very long lines.
            ESP_LOGI(TAG, "Hourly response body for %s (%u bytes):", args->iso_date.c_str(), (unsigned)body.size());
            for (size_t offset = 0; offset < body.size(); offset += 400) {
                ESP_LOGI(TAG, "  %.400s", body.c_str() + offset);
            }
        }

        if (fetched) {
            cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
            if (root) {
                const cJSON *days = cJSON_GetObjectItemCaseSensitive(root, "days");
                if (cJSON_IsArray(days) && cJSON_GetArraySize(days) > 0) {
                    const cJSON *day = cJSON_GetArrayItem(days, 0);
                    parse_hours_array(cJSON_GetObjectItemCaseSensitive(day, "hours"), hours);
                    ok = true;
                    ESP_LOGI(TAG, "Hourly fetch for %s parsed %u hour(s)", args->iso_date.c_str(),
                             (unsigned)hours.size());
                } else {
                    ESP_LOGW(TAG, "Hourly response for %s has no days[0]", args->iso_date.c_str());
                }
                cJSON_Delete(root);
            } else {
                const char *err_at = cJSON_GetErrorPtr();
                ESP_LOGE(TAG, "Failed to parse hourly JSON for %s (near: \"%.60s\")", args->iso_date.c_str(),
                         err_at ? err_at : "(unknown)");
            }
        }
    }

    ESP_LOGI(TAG, "Hourly fetch for %s: invoking callback (ok=%d, %u hour(s))", args->iso_date.c_str(), (int)ok,
             (unsigned)hours.size());
    args->cb(ok, hours);
    ESP_LOGI(TAG, "Hourly fetch for %s: callback returned, task exiting", args->iso_date.c_str());
    delete args;
    vTaskDelete(nullptr);
}

void weather_service_fetch_hours(const WeatherConfig &cfg, const std::string &iso_date, HourlyForecastCb on_result) {
    auto *args = new HourlyFetchArgs{cfg, iso_date, std::move(on_result)};
    BaseType_t created = xTaskCreate(hourly_fetch_task, "wx_hourly_task", 8192, args, tskIDLE_PRIORITY + 3, nullptr);
    if (created != pdPASS) {
        // If this silently fails (most likely cause: not enough internal
        // RAM for another 8KB task stack — the same scarce resource
        // implicated in a real ESP_ERR_NO_MEM crash seen elsewhere in this
        // app), hourly_fetch_task() never runs, args->cb() never fires,
        // and the caller (page_weather.cpp) is left showing "Loading..."
        // forever with no way to know it should stop. Report failure
        // immediately instead.
        ESP_LOGE(TAG, "xTaskCreate failed for hourly fetch of %s (out of memory?) — reporting failure immediately",
                 iso_date.c_str());
        HourlyForecastCb cb = std::move(args->cb);
        delete args;
        cb(false, {});
    }
}
