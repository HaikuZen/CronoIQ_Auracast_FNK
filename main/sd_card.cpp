#include "sd_card.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_card";
static bool s_mounted = false;
static sdmmc_card_t *s_card = nullptr;

esp_err_t sd_card_init(void) {
    ESP_LOGI(TAG, "Mounting SD card (SPI: MISO=%d MOSI=%d SCLK=%d CS=%d)",
             SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_SCLK, SD_PIN_CS);

    // VFS_FAT_MOUNT_DEFAULT_CONFIG() rather than a hand-written designated
    // initializer, so this doesn't rot into a -Werror=missing-field-
    // initializers build break if the IDF version adds another field.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_PIN_MOSI;
    bus_cfg.miso_io_num = SD_PIN_MISO;
    bus_cfg.sclk_io_num = SD_PIN_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;

    esp_err_t err = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)SD_PIN_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem — is the card formatted FAT32?");
        } else {
            ESP_LOGE(TAG, "Failed to init SD card (%s) — check wiring", esp_err_to_name(err));
        }
        spi_bus_free((spi_host_device_t)host.slot);
        return err;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool sd_card_is_mounted(void) {
    return s_mounted;
}
