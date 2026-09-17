#include "weather_icons.h"

#include <cstring>
#include <string>

static constexpr uint32_t kBgHex = WEATHER_ICON_BG_HEX;
static constexpr uint32_t kSunHex = 0xFFC107;
static constexpr uint32_t kCloudHex = 0xB0C4DE;
static constexpr uint32_t kCloudDarkHex = 0x7A8AA0;
static constexpr uint32_t kRainHex = 0x4FC3F7;
static constexpr uint32_t kSnowHex = 0xFFFFFF;
static constexpr uint32_t kBoltHex = 0xFFD54F;
static constexpr uint32_t kFogHex = 0x9AA7B5;

static lv_obj_t *shape(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color,
                        int32_t radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *circle(lv_obj_t *parent, int32_t cx, int32_t cy, int32_t r, uint32_t color) {
    return shape(parent, cx - r, cy - r, 2 * r, 2 * r, color, LV_RADIUS_CIRCLE);
}

static void draw_sun(lv_obj_t *cont, int32_t s, int32_t cx, int32_t cy, int32_t r) {
    circle(cont, cx, cy, r, kSunHex);
    const int32_t ray = LV_MAX(r / 3, 2);
    const int32_t d = (int32_t)(r * 1.6f);
    const int32_t offs[8][2] = {
        {0, -d}, {0, d}, {-d, 0}, {d, 0}, {-d, -d}, {d, -d}, {-d, d}, {d, d},
    };
    for (auto &o : offs) {
        shape(cont, cx + o[0] - ray / 2, cy + o[1] - ray / 2, ray, ray, kSunHex, ray / 2);
    }
    (void)s;
}

static void draw_moon(lv_obj_t *cont, int32_t cx, int32_t cy, int32_t r) {
    circle(cont, cx, cy, r, kSunHex);
    circle(cont, cx + (int32_t)(r * 0.55f), cy - (int32_t)(r * 0.35f), r, kBgHex);
}

static void draw_cloud(lv_obj_t *cont, int32_t s, int32_t cy_offset, uint32_t color) {
    const int32_t base_w = (int32_t)(s * 0.72f);
    const int32_t base_h = (int32_t)(s * 0.30f);
    const int32_t base_x = (s - base_w) / 2;
    const int32_t base_y = (int32_t)(s * 0.55f) + cy_offset;
    shape(cont, base_x, base_y, base_w, base_h, color, base_h / 2);
    circle(cont, base_x + base_w / 3, base_y + base_h / 4, (int32_t)(s * 0.20f), color);
    circle(cont, base_x + base_w * 2 / 3, base_y + base_h / 4, (int32_t)(s * 0.16f), color);
}

static void draw_rain(lv_obj_t *cont, int32_t s) {
    draw_cloud(cont, s, -(int32_t)(s * 0.15f), kCloudHex);
    const int32_t drop_w = LV_MAX((int32_t)(s * 0.06f), 2);
    const int32_t drop_h = (int32_t)(s * 0.16f);
    const int32_t y = (int32_t)(s * 0.82f);
    for (int i = 0; i < 3; i++) {
        int32_t x = (int32_t)(s * (0.30f + i * 0.20f));
        shape(cont, x, y, drop_w, drop_h, kRainHex, drop_w / 2);
    }
}

static void draw_snow(lv_obj_t *cont, int32_t s) {
    draw_cloud(cont, s, -(int32_t)(s * 0.15f), kCloudHex);
    const int32_t r = LV_MAX((int32_t)(s * 0.04f), 2);
    const int32_t y = (int32_t)(s * 0.85f);
    for (int i = 0; i < 3; i++) {
        int32_t x = (int32_t)(s * (0.32f + i * 0.20f));
        circle(cont, x, y, r, kSnowHex);
    }
}

static void draw_thunder(lv_obj_t *cont, int32_t s) {
    draw_cloud(cont, s, -(int32_t)(s * 0.18f), kCloudDarkHex);
    // Rough bolt: three small overlapping steps instead of a rotated polygon.
    int32_t w = (int32_t)(s * 0.10f);
    shape(cont, (int32_t)(s * 0.55f), (int32_t)(s * 0.62f), w, (int32_t)(s * 0.16f), kBoltHex, 1);
    shape(cont, (int32_t)(s * 0.45f), (int32_t)(s * 0.74f), w, (int32_t)(s * 0.16f), kBoltHex, 1);
    shape(cont, (int32_t)(s * 0.50f), (int32_t)(s * 0.86f), (int32_t)(w * 0.8f), (int32_t)(s * 0.10f),
          kBoltHex, 1);
}

static void draw_fog(lv_obj_t *cont, int32_t s) {
    const int32_t h = LV_MAX((int32_t)(s * 0.08f), 3);
    for (int i = 0; i < 3; i++) {
        int32_t w = (int32_t)(s * (0.75f - i * 0.12f));
        int32_t x = (s - w) / 2;
        int32_t y = (int32_t)(s * (0.32f + i * 0.20f));
        shape(cont, x, y, w, h, kFogHex, h / 2);
    }
}

static void draw_wind(lv_obj_t *cont, int32_t s) {
    const int32_t h = LV_MAX((int32_t)(s * 0.07f), 3);
    struct Line { float x, y, w; };
    const Line lines[3] = {{0.12f, 0.35f, 0.55f}, {0.20f, 0.55f, 0.68f}, {0.12f, 0.75f, 0.45f}};
    for (auto &l : lines) {
        shape(cont, (int32_t)(s * l.x), (int32_t)(s * l.y), (int32_t)(s * l.w), h, kFogHex, h / 2);
    }
}

enum class Category {
    ClearDay, ClearNight, PartlyDay, PartlyNight, Cloudy, Rain, Snow, Thunder, Fog, Wind,
};

static Category classify(const char *icon_code) {
    std::string s = icon_code ? icon_code : "";
    auto has = [&](const char *needle) { return s.find(needle) != std::string::npos; };

    if (has("thunder")) return Category::Thunder;
    if (has("snow")) return Category::Snow;
    if (has("rain") || has("sleet") || has("hail") || has("showers")) return Category::Rain;
    if (has("fog")) return Category::Fog;
    if (has("wind")) return Category::Wind;
    if (has("clear-night")) return Category::ClearNight;
    if (has("clear")) return Category::ClearDay;
    if (has("partly-cloudy-night")) return Category::PartlyNight;
    if (has("partly-cloudy")) return Category::PartlyDay;
    return Category::Cloudy;
}

static void build_icon(lv_obj_t *cont, int32_t s, const char *icon_code) {
    switch (classify(icon_code)) {
        case Category::ClearDay:
            draw_sun(cont, s, s / 2, s / 2, (int32_t)(s * 0.30f));
            break;
        case Category::ClearNight:
            draw_moon(cont, s / 2, s / 2, (int32_t)(s * 0.30f));
            break;
        case Category::PartlyDay:
            draw_sun(cont, s, (int32_t)(s * 0.32f), (int32_t)(s * 0.30f), (int32_t)(s * 0.18f));
            draw_cloud(cont, s, (int32_t)(s * 0.05f), kCloudHex);
            break;
        case Category::PartlyNight:
            draw_moon(cont, (int32_t)(s * 0.32f), (int32_t)(s * 0.30f), (int32_t)(s * 0.16f));
            draw_cloud(cont, s, (int32_t)(s * 0.05f), kCloudHex);
            break;
        case Category::Rain:
            draw_rain(cont, s);
            break;
        case Category::Snow:
            draw_snow(cont, s);
            break;
        case Category::Thunder:
            draw_thunder(cont, s);
            break;
        case Category::Fog:
            draw_fog(cont, s);
            break;
        case Category::Wind:
            draw_wind(cont, s);
            break;
        case Category::Cloudy:
        default:
            draw_cloud(cont, s, 0, kCloudHex);
            break;
    }
}

lv_obj_t *weather_icon_create(lv_obj_t *parent, const char *icon_code, int32_t size) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, size, size);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    build_icon(cont, size, icon_code);
    return cont;
}

void weather_icon_update(lv_obj_t *icon_obj, const char *icon_code) {
    lv_obj_clean(icon_obj);
    int32_t size = lv_obj_get_width(icon_obj);
    build_icon(icon_obj, size, icon_code);
}
