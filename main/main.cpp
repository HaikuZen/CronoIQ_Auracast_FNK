#include "app_config.h"
#include "board.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "sd_card.h"
#include "time_manager.h"
#include "ui/ui_manager.h"
#include "weather_service.h"
#include "wifi_manager.h"

static const char *TAG = "app";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "CronoIQ starting");

    board_init();

    AppConfig cfg;
    if (sd_card_init() == ESP_OK) {
        app_config_load(SD_MOUNT_POINT "/config_crono.json", cfg);
    } else {
        ESP_LOGW(TAG, "SD card not available — running with built-in defaults "
                      "(no Wi-Fi credentials, no weather API key)");
    }

    if (lvgl_port_lock(0)) {
        ui_manager_create(cfg);
        lvgl_port_unlock();
    }

    wifi_manager_start(cfg.wifi);
    time_manager_start(cfg.ntp);
    weather_service_start(cfg.weather, ui_manager_update_weather);

    ESP_LOGI(TAG, "Init complete");
}
