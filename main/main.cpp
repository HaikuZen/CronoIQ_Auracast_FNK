#include "app_config.h"
#include "board.h"
#include "config_store.h"
#include "env_sensor_service.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "sd_card.h"
#include "smart_lights_service.h"
#include "time_manager.h"
#include "ui/ui_manager.h"
#include "weather_service.h"
#include "wifi_manager.h"

static const char *TAG = "app";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "CronoIQ starting");

    board_init();

    // SD card is the source of truth; config_store keeps a copy of the last
    // good one in internal flash so the device still boots configured
    // without a card. A failed SD mount is just logged here.
    sd_card_init();
    AppConfig cfg;
    config_store_load(cfg);

    if (lvgl_port_lock(0)) {
        ui_manager_create(cfg);
        lvgl_port_unlock();
    }

    wifi_manager_start(cfg.wifi);
    time_manager_start(cfg.ntp);
    weather_service_start(cfg.weather, ui_manager_update_weather);
    smart_lights_service_start(cfg.smart_lights, ui_manager_update_smart_lights);
    env_sensor_service_start(cfg.env_sensor, ui_manager_update_env);

    ESP_LOGI(TAG, "Init complete");
}
