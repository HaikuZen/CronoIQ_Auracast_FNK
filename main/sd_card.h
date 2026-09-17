#pragma once

#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"

// SPI pin assignment for the SD/TF slot. These are the four otherwise-free
// GPIOs on the FNK0115 (everything else is committed to the RGB panel, GT911
// touch bus, or the backlight) — CONFIRM against the board schematic /
// physical continuity before relying on this in production, this repo's
// bring-up guide does not document the card-slot wiring.
#define SD_PIN_MISO 13
#define SD_PIN_MOSI 11
#define SD_PIN_SCLK 12
#define SD_PIN_CS   10

// Mounts the SD card (SPI mode, FAT) at SD_MOUNT_POINT. Logs and returns an
// error on failure; the caller should keep running with a default
// configuration rather than treat this as fatal.
esp_err_t sd_card_init(void);

bool sd_card_is_mounted(void);
