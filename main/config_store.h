#pragma once

#include "app_config.h"

#define FLASH_MOUNT_POINT "/flash"

enum class ConfigSource {
    SdCard,     // SD card present; config is in sync between card and flash
    FlashCopy,  // SD missing/unreadable/invalid — loaded the last synced copy
    Defaults,   // neither available — built-in AppConfig defaults
};

// Loads the app configuration. The SD card's config_crono.json and a copy
// in the internal-flash LittleFS partition ("storage", mounted at
// FLASH_MOUNT_POINT) are kept in sync, and whichever side changed last
// wins:
//
//   - Card edited since the last sync (on a PC) -> the card wins: it's
//     loaded and copied to flash.
//   - Card unchanged since the last sync -> the flash copy wins (it may
//     hold changes made on the Settings screen while the card was out),
//     and is written back to the card if it differs.
//   - No card / card file missing or invalid -> the flash copy.
//   - Neither -> built-in defaults.
//
// "Changed since the last sync" is decided by comparing the card's bytes
// with a snapshot of what was last read from / written to the card
// (/flash/config_sd_snapshot.json). A config file that fails to parse is
// never synced, so a broken card edit can't clobber the good copy. Every
// write goes to a temp file first, then replaces the original. Call after
// sd_card_init(); never fails hard.
ConfigSource config_store_load(AppConfig &out_config);

// Where the running config came from (result of config_store_load()).
ConfigSource config_store_source(void);

// Saves the user-editable settings from `cfg` (Wi-Fi credentials, timezone,
// weather units/interval, env sensor enabled/offset) by patching just those
// keys into the stored JSON — every other key, including ones this firmware
// doesn't know about, is preserved. Writes the flash copy, and the SD card
// too when one is mounted (keeping the snapshot in step). Returns false if
// the flash copy couldn't be written. Takes effect on the next boot.
bool config_store_save(const AppConfig &cfg);
