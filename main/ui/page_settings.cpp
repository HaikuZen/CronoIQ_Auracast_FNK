#include "page_settings.h"

#include <cstdio>
#include <string>
#include <vector>

#include "config_store.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "env_sensor_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "weather_icons.h"
#include "wifi_manager.h"

static const char *TAG = "page_settings";

namespace {

struct TzOption {
    const char *name;
    const char *posix;
};

// POSIX TZ strings (with DST rules) for common zones. A config whose
// timezone isn't one of these gets an extra "Current: ..." entry at index
// 0 so opening and saving Settings never silently changes it.
constexpr TzOption kTimezones[] = {
    {"UTC", "UTC0"},
    {"London, Dublin (GMT/BST)", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Lisbon (WET/WEST)", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Rome, Paris, Berlin, Madrid (CET)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Athens, Helsinki, Kyiv (EET)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Istanbul, Moscow (UTC+3)", "<+03>-3"},
    {"Dubai (UTC+4)", "<+04>-4"},
    {"India (IST)", "IST-5:30"},
    {"Bangkok, Jakarta (UTC+7)", "<+07>-7"},
    {"Beijing, Singapore (UTC+8)", "<+08>-8"},
    {"Tokyo, Seoul (UTC+9)", "<+09>-9"},
    {"Sydney, Melbourne (AEST)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Auckland (NZST)", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Sao Paulo (UTC-3)", "<-03>3"},
    {"New York (Eastern)", "EST5EDT,M3.2.0,M11.1.0"},
    {"Chicago (Central)", "CST6CDT,M3.2.0,M11.1.0"},
    {"Denver (Mountain)", "MST7MDT,M3.2.0,M11.1.0"},
    {"Phoenix (MST, no DST)", "MST7"},
    {"Los Angeles (Pacific)", "PST8PDT,M3.2.0,M11.1.0"},
};

constexpr uint32_t kIntervals[] = {30, 60, 120, 180, 360};

constexpr float kOffsetMin = -10.0f;
constexpr float kOffsetMax = 10.0f;
constexpr float kOffsetStep = 0.1f;

constexpr int32_t kScreenH = 480;
constexpr int32_t kHeaderH = 56;
constexpr int32_t kFooterH = 64;
constexpr int32_t kKeyboardH = 210;

constexpr uint32_t kHeaderBg = 0x081018;
constexpr uint32_t kCardBg = 0x14283A;
constexpr uint32_t kTextMain = 0xE0F0FF;
constexpr uint32_t kTextMuted = 0x9AB4D0;
constexpr uint32_t kTextDim = 0x6A84A0;
constexpr uint32_t kOk = 0x4CD964;
constexpr uint32_t kErr = 0xFF5A5A;

}  // namespace

static AppConfig s_cfg;

static lv_obj_t *s_overlay;
static lv_obj_t *s_content;
static lv_obj_t *s_footer;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_status;
static lv_obj_t *s_ssid_ta;
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_tz_dd;
static lv_obj_t *s_units_dd;
static lv_obj_t *s_interval_dd;
static lv_obj_t *s_env_sw;
static lv_obj_t *s_offset_label;
static float s_offset;
// Option index -> value, for the dropdowns whose option list depends on
// whether the current config value is one of the presets.
static std::vector<std::string> s_tz_values;
static std::vector<uint32_t> s_interval_values;

void page_settings_init(const AppConfig &cfg) {
    s_cfg = cfg;
}

// --- Small builders ---------------------------------------------------------

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *make_section(const char *title) {
    lv_obj_t *card = lv_obj_create(s_content);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCardBg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    make_label(card, title, &lv_font_montserrat_20, kTextMuted);
    return card;
}

// Label on the left, control(s) added by the caller on the right.
static lv_obj_t *make_row(lv_obj_t *parent, const char *label) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    make_label(row, label, &lv_font_montserrat_16, kTextMain);
    return row;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

static lv_obj_t *make_dropdown(lv_obj_t *parent, const std::string &options, uint32_t selected) {
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options.c_str());
    lv_dropdown_set_selected(dd, selected);
    lv_obj_set_width(dd, 440);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_16, 0);
    return dd;
}

static void set_status(const char *text, uint32_t color) {
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

// --- Keyboard ---------------------------------------------------------------

static void hide_keyboard() {
    if (!s_keyboard || lv_obj_has_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_t *ta = lv_keyboard_get_textarea(s_keyboard);
    if (ta) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(s_keyboard, nullptr);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_content, kScreenH - kHeaderH - kFooterH);
}

// Shrinks the scrollable form to the space above the keyboard and scrolls
// the field being edited into view, so it's never hidden behind the keys.
static void textarea_event_cb(lv_event_t *e) {
    auto *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_content, kScreenH - kHeaderH - kKeyboardH);
    lv_obj_update_layout(s_overlay);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
}

static void keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) hide_keyboard();
}

static lv_obj_t *make_textarea(lv_obj_t *parent, const std::string &text, const char *placeholder) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, text.c_str());
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, 440);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(ta, textarea_event_cb, LV_EVENT_CLICKED, nullptr);
    return ta;
}

// --- Event callbacks --------------------------------------------------------

static void close_overlay() {
    if (!s_overlay) return;
    // Async: this usually runs inside an event callback of one of the
    // overlay's own children.
    lv_obj_delete_async(s_overlay);
    s_overlay = nullptr;
    s_keyboard = nullptr;
}

static void cancel_event_cb(lv_event_t *e) {
    (void)e;
    close_overlay();
}

static void show_password_event_cb(lv_event_t *e) {
    auto *cb = static_cast<lv_obj_t *>(lv_event_get_target(e));
    lv_textarea_set_password_mode(s_pass_ta, !lv_obj_has_state(cb, LV_STATE_CHECKED));
}

static void update_offset_label() {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f °C", s_offset);
    lv_label_set_text(s_offset_label, buf);
}

static void offset_step_event_cb(lv_event_t *e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    s_offset += dir * kOffsetStep;
    if (s_offset < kOffsetMin) s_offset = kOffsetMin;
    if (s_offset > kOffsetMax) s_offset = kOffsetMax;
    // Snap to the 0.1 grid so repeated float adds don't drift (0.30000001).
    s_offset = (float)(int)(s_offset * 10.0f + (s_offset >= 0 ? 0.5f : -0.5f)) / 10.0f;
    update_offset_label();
}

static void restart_timer_cb(lv_timer_t *t) {
    (void)t;
    esp_restart();
}

// Saves BSEC's air-quality calibration before rebooting — otherwise every
// settings change would throw away up to an hour of calibration progress
// (it's otherwise only saved periodically). Runs on its own task, not the
// LVGL task: the save happens on the sensor task, which needs the LVGL
// lock to publish readings and could otherwise be stuck behind us.
static void restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));  // let "Saved - restarting..." render
    if (env_sensor_service_save_state_now(3000)) {
        ESP_LOGI(TAG, "Air-quality calibration saved before restart");
    }
    esp_restart();
}

static void save_event_cb(lv_event_t *e) {
    (void)e;
    AppConfig edited = s_cfg;
    edited.wifi.ssid = lv_textarea_get_text(s_ssid_ta);
    edited.wifi.password = lv_textarea_get_text(s_pass_ta);
    edited.ntp.posix_tz = s_tz_values[lv_dropdown_get_selected(s_tz_dd)];
    edited.weather.units = lv_dropdown_get_selected(s_units_dd) == 1 ? "us" : "metric";
    edited.weather.update_interval_min = s_interval_values[lv_dropdown_get_selected(s_interval_dd)];
    edited.env_sensor.enabled = lv_obj_has_state(s_env_sw, LV_STATE_CHECKED);
    edited.env_sensor.temp_offset_c = s_offset;

    const bool changed = edited.wifi.ssid != s_cfg.wifi.ssid || edited.wifi.password != s_cfg.wifi.password ||
                         edited.ntp.posix_tz != s_cfg.ntp.posix_tz || edited.weather.units != s_cfg.weather.units ||
                         edited.weather.update_interval_min != s_cfg.weather.update_interval_min ||
                         edited.env_sensor.enabled != s_cfg.env_sensor.enabled ||
                         edited.env_sensor.temp_offset_c != s_cfg.env_sensor.temp_offset_c;
    if (!changed) {
        close_overlay();
        return;
    }

    if (!config_store_save(edited)) {
        set_status("Couldn't save - flash storage error (see log)", kErr);
        return;
    }
    ESP_LOGI(TAG, "Settings saved, restarting to apply them");
    set_status("Saved - restarting...", kOk);
    if (xTaskCreate(restart_task, "restart_task", 3072, nullptr, tskIDLE_PRIORITY + 3, nullptr) != pdPASS) {
        // No memory for the helper task: restart anyway (losing at most an
        // hour of calibration) rather than leaving "restarting..." on screen.
        ESP_LOGW(TAG, "xTaskCreate(restart_task) failed — restarting without saving calibration");
        lv_timer_t *t = lv_timer_create(restart_timer_cb, 1200, nullptr);
        lv_timer_set_repeat_count(t, 1);
    }
}

// --- Sections ---------------------------------------------------------------

static void build_wifi_section() {
    lv_obj_t *sec = make_section(LV_SYMBOL_WIFI "  Wi-Fi");
    s_ssid_ta = make_textarea(make_row(sec, "Network (SSID)"), s_cfg.wifi.ssid, "Network name");

    s_pass_ta = make_textarea(make_row(sec, "Password"), s_cfg.wifi.password, "Empty for an open network");
    lv_textarea_set_password_mode(s_pass_ta, true);

    lv_obj_t *show_row = make_row(sec, "");
    lv_obj_t *show_cb = lv_checkbox_create(show_row);
    lv_checkbox_set_text(show_cb, "Show password");
    lv_obj_set_style_text_font(show_cb, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(show_cb, lv_color_hex(kTextMuted), 0);
    lv_obj_add_event_cb(show_cb, show_password_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

static void build_time_weather_section() {
    lv_obj_t *sec = make_section(LV_SYMBOL_REFRESH "  Time & weather");

    s_tz_values.clear();
    std::string tz_options;
    uint32_t tz_selected = 0;
    bool tz_known = false;
    for (const TzOption &tz : kTimezones) {
        if (s_cfg.ntp.posix_tz == tz.posix) tz_known = true;
    }
    if (!tz_known) {
        tz_options = "Current: " + s_cfg.ntp.posix_tz;
        s_tz_values.push_back(s_cfg.ntp.posix_tz);
    }
    for (const TzOption &tz : kTimezones) {
        if (s_cfg.ntp.posix_tz == tz.posix) tz_selected = s_tz_values.size();
        if (!tz_options.empty()) tz_options += '\n';
        tz_options += tz.name;
        s_tz_values.push_back(tz.posix);
    }
    s_tz_dd = make_dropdown(make_row(sec, "Timezone"), tz_options, tz_selected);

    s_units_dd = make_dropdown(make_row(sec, "Units"), "Metric (°C)\nUS (°F)",
                               s_cfg.weather.units == "us" ? 1 : 0);

    s_interval_values.clear();
    std::string iv_options;
    uint32_t iv_selected = 0;
    bool iv_known = false;
    for (uint32_t m : kIntervals) {
        if (m == s_cfg.weather.update_interval_min) iv_known = true;
    }
    char buf[40];
    if (!iv_known) {
        snprintf(buf, sizeof(buf), "Current: %lu min", (unsigned long)s_cfg.weather.update_interval_min);
        iv_options = buf;
        s_interval_values.push_back(s_cfg.weather.update_interval_min);
    }
    for (uint32_t m : kIntervals) {
        if (m == s_cfg.weather.update_interval_min) iv_selected = s_interval_values.size();
        if (m < 60) {
            snprintf(buf, sizeof(buf), "Every %lu min", (unsigned long)m);
        } else if (m == 60) {
            snprintf(buf, sizeof(buf), "Every hour");
        } else {
            snprintf(buf, sizeof(buf), "Every %lu hours", (unsigned long)(m / 60));
        }
        if (!iv_options.empty()) iv_options += '\n';
        iv_options += buf;
        s_interval_values.push_back(m);
    }
    s_interval_dd = make_dropdown(make_row(sec, "Weather refresh"), iv_options, iv_selected);
    make_label(sec, "More frequent refreshes use more of your Visual Crossing API quota.", &lv_font_montserrat_14,
               kTextDim);
}

static void build_sensor_section() {
    lv_obj_t *sec = make_section(LV_SYMBOL_EYE_OPEN "  Indoor sensor (BME690)");

    s_env_sw = lv_switch_create(make_row(sec, "Enabled"));
    if (s_cfg.env_sensor.enabled) lv_obj_add_state(s_env_sw, LV_STATE_CHECKED);

    lv_obj_t *row = make_row(sec, "Temperature offset");
    lv_obj_t *stepper = lv_obj_create(row);
    lv_obj_remove_style_all(stepper);
    lv_obj_set_size(stepper, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(stepper, 12, 0);
    lv_obj_clear_flag(stepper, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_size(make_button(stepper, LV_SYMBOL_MINUS, offset_step_event_cb, (void *)(intptr_t)-1), 56, 44);
    s_offset = s_cfg.env_sensor.temp_offset_c;
    s_offset_label = make_label(stepper, "", &lv_font_montserrat_20, kTextMain);
    lv_obj_set_width(s_offset_label, 100);
    lv_obj_set_style_text_align(s_offset_label, LV_TEXT_ALIGN_CENTER, 0);
    update_offset_label();
    lv_obj_set_size(make_button(stepper, LV_SYMBOL_PLUS, offset_step_event_cb, (void *)(intptr_t)1), 56, 44);

    lv_obj_t *hint = make_label(sec,
                                "Subtracted from the measured temperature, to make up for heat from the "
                                "board. Compare with a reference thermometer.",
                                &lv_font_montserrat_14, kTextDim);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

static void build_info_section() {
    lv_obj_t *sec = make_section(LV_SYMBOL_LIST "  Info");

    const esp_app_desc_t *app = esp_app_get_description();
    char ip[20];
    wifi_manager_get_ip_str(ip, sizeof(ip));

    const char *source = "Built-in defaults (no config found)";
    switch (config_store_source()) {
        case ConfigSource::SdCard: source = "SD card (synced to flash)"; break;
        case ConfigSource::FlashCopy: source = "Flash copy (no SD card)"; break;
        case ConfigSource::Defaults: break;
    }

    uint64_t up_s = (uint64_t)(esp_timer_get_time() / 1000000);
    char info[400];
    snprintf(info, sizeof(info),
             "Firmware:  %s (built %s %s)\n"
             "IP address:  %s\n"
             "Config source:  %s\n"
             "Free memory:  %u KB internal, %u KB PSRAM\n"
             "Uptime:  %ud %02uh %02um",
             app->version, app->date, app->time, ip, source,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned)(up_s / 86400),
             (unsigned)((up_s / 3600) % 24), (unsigned)((up_s / 60) % 60));
    lv_obj_t *label = make_label(sec, info, &lv_font_montserrat_16, kTextMain);
    lv_obj_set_style_text_line_space(label, 6, 0);
}

// --- Open -------------------------------------------------------------------

void page_settings_open(void) {
    if (s_overlay) return;

    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(WEATHER_ICON_BG_HEX), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    // Swallow touches so nothing underneath (the tabview) reacts.
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    // Header: title + Cancel
    lv_obj_t *header = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), kHeaderH);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kHeaderBg), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(header, 16, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    make_label(header, LV_SYMBOL_SETTINGS "  Settings", &lv_font_montserrat_24, kTextMain);
    lv_obj_t *cancel = make_button(header, LV_SYMBOL_CLOSE "  Cancel", cancel_event_cb, nullptr);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x2A3A4A), 0);

    // Scrollable form
    s_content = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, LV_PCT(100), kScreenH - kHeaderH - kFooterH);
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, kHeaderH);
    lv_obj_set_style_pad_all(s_content, 12, 0);
    lv_obj_set_style_pad_row(s_content, 12, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);

    build_wifi_section();
    build_time_weather_section();
    build_sensor_section();
    build_info_section();

    // Footer: status + Save
    s_footer = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_footer);
    lv_obj_set_size(s_footer, LV_PCT(100), kFooterH);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_footer, lv_color_hex(kHeaderBg), 0);
    lv_obj_set_style_bg_opa(s_footer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_footer, 16, 0);
    lv_obj_set_flex_flow(s_footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_footer, LV_OBJ_FLAG_SCROLLABLE);
    s_status = make_label(s_footer, "Changes apply after the board restarts.", &lv_font_montserrat_16, kTextMuted);
    lv_obj_t *save = make_button(s_footer, LV_SYMBOL_SAVE "  Save & restart", save_event_cb, nullptr);
    lv_obj_set_height(save, 44);

    // On-screen keyboard, shown while a text field is being edited.
    s_keyboard = lv_keyboard_create(s_overlay);
    lv_obj_set_size(s_keyboard, LV_PCT(100), kKeyboardH);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}
