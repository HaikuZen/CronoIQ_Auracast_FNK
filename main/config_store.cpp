#include "config_store.h"

#include <cmath>
#include <cstdio>
#include <string>

#include <sys/stat.h>

#include "cJSON.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "sd_card.h"

static const char *TAG = "config_store";

static constexpr const char *kSdConfigPath = SD_MOUNT_POINT "/config_crono.json";
static constexpr const char *kFlashConfigPath = FLASH_MOUNT_POINT "/config_crono.json";
// What the card's config_crono.json contained the last time we read it into,
// or wrote it from, flash. Card bytes != snapshot means it was edited
// elsewhere (on a PC) since then, so the card should win.
static constexpr const char *kSnapshotPath = FLASH_MOUNT_POINT "/config_sd_snapshot.json";

static bool s_flash_ok = false;
static ConfigSource s_source = ConfigSource::Defaults;

static bool mount_flash() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = FLASH_MOUNT_POINT;
    conf.partition_label = "storage";
    // First boot after adding the partition finds it blank/unformatted.
    conf.format_if_mount_failed = 1;
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mounting LittleFS partition \"storage\" failed: %s — no config fallback "
                      "available (partition table flashed?)",
                 esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s (%u/%u KB used)", FLASH_MOUNT_POINT, (unsigned)(used / 1024),
             (unsigned)(total / 1024));
    return true;
}

static bool read_whole_file(const char *path, std::string &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    out.clear();
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// Writes `data` to `<path>.tmp`, then swaps it into place. On LittleFS
// rename() atomically replaces the destination, so the file is always
// either the complete old or complete new version. FAT's rename() refuses
// to overwrite, so on the SD card the old file is removed first — a power
// cut in that instant loses the card's copy, but the flash copy still
// holds the config.
static bool write_file_replace(const char *path, const std::string &data) {
    std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "Can't create %s", tmp.c_str());
        return false;
    }
    size_t written = fwrite(data.data(), 1, data.size(), f);
    bool ok = (written == data.size()) && fflush(f) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        ESP_LOGE(TAG, "Writing %s failed (%u/%u bytes)", tmp.c_str(), (unsigned)written, (unsigned)data.size());
        remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path) != 0) {
        remove(path);
        if (rename(tmp.c_str(), path) != 0) {
            ESP_LOGE(TAG, "Renaming %s -> %s failed", tmp.c_str(), path);
            remove(tmp.c_str());
            return false;
        }
    }
    return true;
}

// Writes only when the bytes differ, so an unchanged config costs no flash
// or card writes at boot.
static bool write_if_changed(const char *path, const std::string &data) {
    std::string current;
    if (read_whole_file(path, current) && current == data) return true;
    return write_file_replace(path, data);
}

static ConfigSource set_source(ConfigSource src) {
    s_source = src;
    return src;
}

ConfigSource config_store_load(AppConfig &out_config) {
    s_flash_ok = mount_flash();

    // Upgrade from firmware that synced SD -> flash without a snapshot: back
    // then the flash copy was always byte-identical to the card, so it *is*
    // the snapshot. Without this, settings changed on-screen before the old
    // card is reinserted would be overwritten by it as if it were newer.
    std::string flash_seed;
    struct stat st;
    if (s_flash_ok && stat(kSnapshotPath, &st) != 0 && read_whole_file(kFlashConfigPath, flash_seed) &&
        !flash_seed.empty()) {
        if (write_file_replace(kSnapshotPath, flash_seed)) {
            ESP_LOGI(TAG, "Created SD sync snapshot from existing flash copy");
        }
    }

    std::string sd_bytes;
    const bool sd_readable = sd_card_is_mounted() && read_whole_file(kSdConfigPath, sd_bytes) && !sd_bytes.empty();

    if (sd_readable && s_flash_ok) {
        std::string snapshot, flash_bytes;
        const bool card_unchanged = read_whole_file(kSnapshotPath, snapshot) && snapshot == sd_bytes;
        if (card_unchanged && read_whole_file(kFlashConfigPath, flash_bytes)) {
            AppConfig flash_cfg;
            if (app_config_load(kFlashConfigPath, flash_cfg)) {
                out_config = flash_cfg;
                if (flash_bytes != sd_bytes) {
                    // Settings were changed on-device while the card was out
                    // — bring the card up to date.
                    if (write_file_replace(kSdConfigPath, flash_bytes)) {
                        write_if_changed(kSnapshotPath, flash_bytes);
                        ESP_LOGI(TAG, "Card unchanged since last sync — wrote on-device settings back to it");
                    }
                } else {
                    ESP_LOGI(TAG, "SD card and flash config in sync");
                }
                return set_source(ConfigSource::SdCard);
            }
        }
    }

    if (sd_readable) {
        AppConfig sd_cfg;
        if (app_config_load(kSdConfigPath, sd_cfg)) {
            out_config = sd_cfg;
            if (s_flash_ok) {
                if (write_if_changed(kFlashConfigPath, sd_bytes) && write_if_changed(kSnapshotPath, sd_bytes)) {
                    ESP_LOGI(TAG, "Using config from SD card (synced to flash)");
                } else {
                    ESP_LOGW(TAG, "Using config from SD card, but syncing it to flash failed");
                }
            }
            return set_source(ConfigSource::SdCard);
        }
        ESP_LOGW(TAG, "%s is invalid — trying the flash copy", kSdConfigPath);
    } else if (sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD card has no %s — trying the flash copy", kSdConfigPath);
    } else {
        ESP_LOGW(TAG, "No SD card — trying the flash copy of the last config");
    }

    if (s_flash_ok) {
        AppConfig flash_cfg;
        if (app_config_load(kFlashConfigPath, flash_cfg)) {
            out_config = flash_cfg;
            ESP_LOGI(TAG, "Using last synced config from flash (%s)", kFlashConfigPath);
            return set_source(ConfigSource::FlashCopy);
        }
        ESP_LOGW(TAG, "No usable flash copy either (never synced from an SD card yet?)");
    }

    ESP_LOGW(TAG, "Running with built-in defaults (no Wi-Fi credentials, no weather API key)");
    return set_source(ConfigSource::Defaults);
}

ConfigSource config_store_source(void) {
    return s_source;
}

// --- Saving -----------------------------------------------------------------

static cJSON *ensure_object(cJSON *parent, const char *key) {
    cJSON *obj = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (cJSON_IsObject(obj)) return obj;
    cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
    return cJSON_AddObjectToObject(parent, key);
}

static void set_string(cJSON *obj, const char *key, const std::string &value) {
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddStringToObject(obj, key, value.c_str());
}

static void set_number(cJSON *obj, const char *key, double value) {
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddNumberToObject(obj, key, value);
}

static void set_bool(cJSON *obj, const char *key, bool value) {
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddBoolToObject(obj, key, value);
}

bool config_store_save(const AppConfig &cfg) {
    if (!s_flash_ok) {
        ESP_LOGE(TAG, "Can't save settings — flash storage isn't mounted");
        return false;
    }

    // Patch the stored file rather than serializing AppConfig from scratch,
    // so keys this firmware doesn't model (or ignores) survive a save.
    std::string current;
    cJSON *root = nullptr;
    if (read_whole_file(kFlashConfigPath, current)) root = cJSON_ParseWithLength(current.c_str(), current.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }

    cJSON *wifi = ensure_object(root, "wifi");
    set_string(wifi, "ssid", cfg.wifi.ssid);
    set_string(wifi, "password", cfg.wifi.password);

    cJSON *ntp = ensure_object(root, "ntp");
    set_string(ntp, "timezone", cfg.ntp.posix_tz);

    cJSON *weather = ensure_object(root, "weather");
    set_string(weather, "units", cfg.weather.units);
    set_number(weather, "update_interval_min", cfg.weather.update_interval_min);

    cJSON *env = ensure_object(root, "env_sensor");
    set_bool(env, "enabled", cfg.env_sensor.enabled);
    // Round to 0.01 so the file doesn't show float noise like 1.2000000477.
    set_number(env, "temp_offset_c", std::round(cfg.env_sensor.temp_offset_c * 100.0) / 100.0);

    char *printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) {
        ESP_LOGE(TAG, "Out of memory serializing settings");
        return false;
    }
    std::string json(printed);
    json += '\n';
    cJSON_free(printed);

    if (!write_file_replace(kFlashConfigPath, json)) {
        ESP_LOGE(TAG, "Saving settings to flash failed");
        return false;
    }
    ESP_LOGI(TAG, "Settings saved to flash (%u bytes)", (unsigned)json.size());

    // Card present: update it now and keep the snapshot in step, so the next
    // boot sees the card as unchanged rather than as a newer PC edit. Card
    // absent: the snapshot keeps the old card contents, so when that card is
    // reinserted it's recognized as unchanged and these settings are written
    // back to it.
    if (sd_card_is_mounted()) {
        if (write_file_replace(kSdConfigPath, json)) {
            write_if_changed(kSnapshotPath, json);
            ESP_LOGI(TAG, "Settings also written to the SD card");
        } else {
            ESP_LOGW(TAG, "Couldn't write settings to the SD card — saved in flash only");
        }
    }
    return true;
}
