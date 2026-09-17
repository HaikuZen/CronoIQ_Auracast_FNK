#include "time_manager.h"

#include <atomic>
#include <ctime>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

static const char *TAG = "time_manager";
static std::atomic<bool> s_synced{false};

static void on_time_sync(struct timeval *tv) {
    (void)tv;
    ESP_LOGI(TAG, "NTP time synced");
    s_synced = true;
}

static void time_manager_task(void *pvArgs) {
    auto *cfg = static_cast<NtpConfig *>(pvArgs);

    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    setenv("TZ", cfg->posix_tz.c_str(), 1);
    tzset();

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(cfg->server.c_str());
    sntp_config.start = true;
    sntp_config.sync_cb = on_time_sync;
    esp_err_t err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SNTP started (server=%s tz=%s)", cfg->server.c_str(), cfg->posix_tz.c_str());
    }

    delete cfg;
    vTaskDelete(nullptr);
}

void time_manager_start(const NtpConfig &cfg) {
    // Waits for wifi_manager_is_connected() on a background task rather than
    // blocking app_main — esp_netif_sntp_init() needs a live network route,
    // and starting it before one exists just means silently-failing sync
    // attempts until Wi-Fi comes up anyway.
    auto *cfg_copy = new NtpConfig(cfg);
    xTaskCreate(time_manager_task, "time_manager_task", 4096, cfg_copy, tskIDLE_PRIORITY + 2, nullptr);
}

bool time_manager_is_synced(void) {
    return s_synced.load();
}

static void get_local_tm(struct tm *out_tm) {
    time_t now = time(nullptr);
    localtime_r(&now, out_tm);
}

void time_manager_get_time_str(char *buf, size_t len) {
    struct tm t;
    get_local_tm(&t);
    strftime(buf, len, "%H:%M:%S", &t);
}

void time_manager_get_date_str(char *buf, size_t len) {
    struct tm t;
    get_local_tm(&t);
    strftime(buf, len, "%d %B %Y", &t);
}

void time_manager_get_weekday_str(char *buf, size_t len) {
    struct tm t;
    get_local_tm(&t);
    strftime(buf, len, "%A", &t);
}
