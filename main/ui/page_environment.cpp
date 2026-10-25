#include "page_environment.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "env_history.h"
#include "weather_icons.h"

namespace {

struct IaqCategory {
    float max_iaq;  // inclusive upper bound
    const char *name;
    uint32_t color;
};

// Bosch's IAQ index scale (BME68x/BME69x datasheets). Colors follow the
// usual green→red→purple air-quality convention, lightened where needed to
// stay readable on the dark page background.
constexpr IaqCategory kIaqCategories[] = {
    {50, "Excellent", 0x3DDC84},
    {100, "Good", 0x9BD94A},
    {150, "Lightly polluted", 0xF2D530},
    {200, "Moderately polluted", 0xFF9A2E},
    {250, "Heavily polluted", 0xFF4D4D},
    {350, "Severely polluted", 0xD05BA8},
    {500, "Extremely polluted", 0xB07040},
};

constexpr uint32_t kCardBg = 0x14283A;
constexpr uint32_t kTextMain = 0xE0F0FF;
constexpr uint32_t kTextMuted = 0x9AB4D0;
constexpr uint32_t kTextDim = 0x6A84A0;

struct Tile {
    lv_obj_t *value;
    lv_obj_t *unit;
};

// Graphable metrics, indexed by EnvMetric. Values are drawn as
// value*scale integers on the lv_chart; min_span keeps a flat line from
// being blown up into apparent noise.
struct MetricInfo {
    const char *unit;  // temperature's is chosen at runtime (°C/°F)
    int decimals;
    float scale;
    float min_span;
    uint32_t color;
};

constexpr MetricInfo kMetricInfo[] = {
    {"", 1, 10.0f, 2.0f, 0xFF9A2E},
    {"%", 0, 10.0f, 5.0f, 0x4FC3F7},
    {"hPa", 1, 10.0f, 2.0f, 0xB39DDB},
    {"", 0, 1.0f, 25.0f, 0x3DDC84},
    {"ppm", 0, 1.0f, 100.0f, 0xF2D530},
};

constexpr const char *kMetricButtons[] = {"Temp", "Humidity", "Pressure", "IAQ", "CO2", ""};
constexpr const char *kRangeButtons[] = {"3 h", "24 h", ""};
constexpr const char *kViewButtons[] = {"Now", "History", ""};

constexpr int32_t kBarH = 40;
constexpr int32_t kBodyY = kBarH + 8;
constexpr int32_t kBodyH = 410 - kBodyY;
constexpr size_t kMaxPoints = 288;  // EnvRange::Hours24
// The history view also refreshes on new readings (every 3s), but the
// buckets are 1-5 min wide, so redrawing that often is wasted work.
constexpr uint32_t kHistoryRefreshMs = 15000;

}  // namespace

static bool s_fahrenheit = false;

static lv_obj_t *s_iaq_value;
static lv_obj_t *s_iaq_category;
static lv_obj_t *s_iaq_bar;
static lv_obj_t *s_status;
static Tile s_temp, s_hum, s_pres, s_co2, s_tvoc, s_gas;

static lv_obj_t *s_now_view;
static lv_obj_t *s_hist_view;
static lv_obj_t *s_range_bm;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;
static lv_obj_t *s_y_labels[3];  // top, middle, bottom
static lv_obj_t *s_x_labels[3];  // left, middle, right
static lv_obj_t *s_hist_stats;
static EnvMetric s_metric = EnvMetric::Temperature;
static EnvRange s_range = EnvRange::Hours24;
static uint32_t s_last_hist_refresh_ms;
static float s_hist_vals[kMaxPoints];
static int32_t s_hist_pts[kMaxPoints];

static const IaqCategory &category_for(float iaq) {
    for (const IaqCategory &c : kIaqCategories) {
        if (iaq <= c.max_iaq) return c;
    }
    return kIaqCategories[sizeof(kIaqCategories) / sizeof(kIaqCategories[0]) - 1];
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t w, int32_t h) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCardBg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static Tile make_tile(lv_obj_t *parent, const char *title, const char *unit) {
    lv_obj_t *card = make_card(parent, 229, (kBodyH - 28) / 3);
    make_label(card, title, &lv_font_montserrat_16, kTextMuted);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    Tile t;
    t.value = make_label(row, "--", &lv_font_montserrat_38, kTextMain);
    t.unit = make_label(row, unit, &lv_font_montserrat_20, kTextMuted);
    // Nudge the unit up so its baseline sits roughly on the big number's.
    lv_obj_set_style_pad_bottom(t.unit, 4, 0);
    return t;
}

static void set_tile(const Tile &t, bool valid, const char *fmt, float value) {
    if (!valid) {
        lv_label_set_text(t.value, "--");
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), fmt, value);
    lv_label_set_text(t.value, buf);
}

// --- History view -----------------------------------------------------------

static float to_display(EnvMetric m, float v) {
    return (m == EnvMetric::Temperature && s_fahrenheit) ? v * 9.0f / 5.0f + 32.0f : v;
}

static const char *unit_for(EnvMetric m) {
    if (m == EnvMetric::Temperature) return s_fahrenheit ? "°F" : "°C";
    return kMetricInfo[(size_t)m].unit;
}

static void set_value_label(lv_obj_t *label, EnvMetric m, float v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.*f", kMetricInfo[(size_t)m].decimals, v);
    lv_label_set_text(label, buf);
}

static void refresh_history() {
    if (!s_chart || lv_obj_has_flag(s_hist_view, LV_OBJ_FLAG_HIDDEN)) return;
    s_last_hist_refresh_ms = lv_tick_get();

    const MetricInfo &info = kMetricInfo[(size_t)s_metric];
    const size_t n = env_history_points(s_range);
    env_history_get(s_metric, s_range, s_hist_vals);

    float lo = INFINITY, hi = -INFINITY, last = NAN;
    size_t valid = 0;
    for (size_t i = 0; i < n; i++) {
        if (std::isnan(s_hist_vals[i])) continue;
        float v = to_display(s_metric, s_hist_vals[i]);
        s_hist_vals[i] = v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        last = v;
        valid++;
    }

    // Y range: data min/max widened to at least min_span, plus 10% margin.
    float mid = valid ? (lo + hi) / 2.0f : 0.0f;
    float span = valid ? hi - lo : 0.0f;
    if (span < info.min_span) span = info.min_span;
    span *= 1.2f;
    const float y_min = mid - span / 2.0f, y_max = mid + span / 2.0f;

    for (size_t i = 0; i < n; i++) {
        s_hist_pts[i] = std::isnan(s_hist_vals[i]) ? LV_CHART_POINT_NONE
                                                    : (int32_t)lroundf(s_hist_vals[i] * info.scale);
    }
    // The series reads s_hist_pts directly (ext array, set at creation), so
    // this is one invalidation. lv_chart_set_series_values() would
    // invalidate per point — ~0.2ms each, 54ms for 288 points on hardware.
    lv_chart_set_point_count(s_chart, n);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)floorf(y_min * info.scale),
                            (int32_t)ceilf(y_max * info.scale));
    lv_chart_set_series_color(s_chart, s_series, lv_color_hex(info.color));
    lv_chart_refresh(s_chart);

    set_value_label(s_y_labels[0], s_metric, y_max);
    set_value_label(s_y_labels[1], s_metric, mid);
    set_value_label(s_y_labels[2], s_metric, y_min);
    lv_label_set_text(s_x_labels[0], s_range == EnvRange::Hours3 ? "-3 h" : "-24 h");
    lv_label_set_text(s_x_labels[1], s_range == EnvRange::Hours3 ? "-1.5 h" : "-12 h");

    char stats[160];
    if (valid == 0) {
        if (s_metric == EnvMetric::Iaq || s_metric == EnvMetric::Co2) {
            snprintf(stats, sizeof(stats), "No data yet - recorded once IAQ calibration starts (accuracy >= 1)");
        } else {
            snprintf(stats, sizeof(stats), "No data yet - a point is added every %s; history restarts on reboot",
                     s_range == EnvRange::Hours3 ? "minute" : "5 minutes");
        }
    } else {
        const int d = info.decimals;
        const char *u = unit_for(s_metric);
        snprintf(stats, sizeof(stats), "Now %.*f %s     Min %.*f     Max %.*f", d, last, u, d, lo, d, hi);
    }
    lv_label_set_text(s_hist_stats, stats);
}

static lv_obj_t *make_segmented(lv_obj_t *parent, const char *const *map, uint32_t checked, int32_t w) {
    lv_obj_t *bm = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(bm, map);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    lv_buttonmatrix_set_button_ctrl(bm, checked, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(bm, w, kBarH);
    lv_obj_set_style_pad_all(bm, 3, 0);
    lv_obj_set_style_pad_gap(bm, 4, 0);
    lv_obj_set_style_bg_color(bm, lv_color_hex(kCardBg), 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_radius(bm, 10, 0);
    lv_obj_set_style_text_font(bm, &lv_font_montserrat_16, 0);
    lv_obj_set_style_bg_color(bm, lv_color_hex(0x1E3A52), LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, lv_color_hex(kTextMuted), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
    // Built as a selector integer: C++26 rejects `lv_part_t | lv_state_t`.
    const lv_style_selector_t items_checked = (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED;
    lv_obj_set_style_bg_color(bm, lv_color_hex(0x2E7DBA), items_checked);
    lv_obj_set_style_text_color(bm, lv_color_hex(kTextMain), items_checked);
    return bm;
}

static void view_event_cb(lv_event_t *e) {
    auto *bm = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const bool history = lv_buttonmatrix_get_selected_button(bm) == 1;
    if (history) {
        lv_obj_add_flag(s_now_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_hist_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_range_bm, LV_OBJ_FLAG_HIDDEN);
        refresh_history();
    } else {
        lv_obj_add_flag(s_hist_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_range_bm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_now_view, LV_OBJ_FLAG_HIDDEN);
    }
}

static void metric_event_cb(lv_event_t *e) {
    auto *bm = static_cast<lv_obj_t *>(lv_event_get_target(e));
    uint32_t sel = lv_buttonmatrix_get_selected_button(bm);
    if (sel >= (uint32_t)EnvMetric::Count) return;
    s_metric = (EnvMetric)sel;
    refresh_history();
}

static void range_event_cb(lv_event_t *e) {
    auto *bm = static_cast<lv_obj_t *>(lv_event_get_target(e));
    s_range = lv_buttonmatrix_get_selected_button(bm) == 0 ? EnvRange::Hours3 : EnvRange::Hours24;
    refresh_history();
}

static void build_history_view(lv_obj_t *parent) {
    s_hist_view = lv_obj_create(parent);
    lv_obj_remove_style_all(s_hist_view);
    lv_obj_set_size(s_hist_view, LV_PCT(100), kBodyH);
    lv_obj_align(s_hist_view, LV_ALIGN_TOP_LEFT, 0, kBodyY);
    lv_obj_clear_flag(s_hist_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_hist_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *metric_bm = make_segmented(s_hist_view, kMetricButtons, 0, LV_PCT(100));
    lv_obj_align(metric_bm, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(metric_bm, metric_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    const int32_t card_h = kBodyH - kBarH - 8;
    lv_obj_t *card = make_card(s_hist_view, LV_PCT(100), card_h);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, kBarH + 8);

    // Plot area: y-axis labels in a 64px gutter on the left, x-axis labels
    // and the stats line underneath.
    const int32_t gutter = 64, x_label_h = 24, stats_h = 26;
    const int32_t chart_h = card_h - 28 - x_label_h - stats_h;
    s_chart = lv_chart_create(card);
    lv_obj_set_size(s_chart, 780 - 28 - gutter, chart_h);
    lv_obj_align(s_chart, LV_ALIGN_TOP_LEFT, gutter, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 5, 9);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x24384C), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    // No point markers — 180-288 dots would just smear the line.
    lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    s_series = lv_chart_add_series(s_chart, lv_color_hex(kMetricInfo[0].color), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_point_count(s_chart, kMaxPoints);
    for (size_t i = 0; i < kMaxPoints; i++) s_hist_pts[i] = LV_CHART_POINT_NONE;
    lv_chart_set_series_ext_y_array(s_chart, s_series, s_hist_pts);

    const lv_align_t y_align[3] = {LV_ALIGN_OUT_LEFT_TOP, LV_ALIGN_OUT_LEFT_MID, LV_ALIGN_OUT_LEFT_BOTTOM};
    for (int i = 0; i < 3; i++) {
        s_y_labels[i] = make_label(card, "", &lv_font_montserrat_14, kTextDim);
        lv_obj_set_width(s_y_labels[i], gutter - 8);
        lv_obj_set_style_text_align(s_y_labels[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align_to(s_y_labels[i], s_chart, y_align[i], -8, 0);
    }
    const lv_align_t x_align[3] = {LV_ALIGN_OUT_BOTTOM_LEFT, LV_ALIGN_OUT_BOTTOM_MID, LV_ALIGN_OUT_BOTTOM_RIGHT};
    for (int i = 0; i < 3; i++) {
        s_x_labels[i] = make_label(card, i == 2 ? "now" : "", &lv_font_montserrat_14, kTextDim);
        lv_obj_align_to(s_x_labels[i], s_chart, x_align[i], 0, 4);
    }
    s_hist_stats = make_label(card, "", &lv_font_montserrat_16, kTextMain);
    lv_obj_align(s_hist_stats, LV_ALIGN_BOTTOM_LEFT, gutter, 0);
}

lv_obj_t *page_environment_create(lv_obj_t *parent, const std::string &units) {
    s_fahrenheit = (units == "us");

    lv_obj_set_style_bg_color(parent, lv_color_hex(WEATHER_ICON_BG_HEX), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 10, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // --- Top bar: Now | History, plus the range picker in History ---
    lv_obj_t *view_bm = make_segmented(parent, kViewButtons, 0, 240);
    lv_obj_align(view_bm, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(view_bm, view_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_range_bm = make_segmented(parent, kRangeButtons, 1, 180);
    lv_obj_align(s_range_bm, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(s_range_bm, range_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_flag(s_range_bm, LV_OBJ_FLAG_HIDDEN);

    s_now_view = lv_obj_create(parent);
    lv_obj_remove_style_all(s_now_view);
    lv_obj_set_size(s_now_view, LV_PCT(100), kBodyH);
    lv_obj_align(s_now_view, LV_ALIGN_TOP_LEFT, 0, kBodyY);
    lv_obj_clear_flag(s_now_view, LV_OBJ_FLAG_SCROLLABLE);

    // --- Left: air quality card ---
    lv_obj_t *iaq_card = make_card(s_now_view, 300, kBodyH);
    lv_obj_align(iaq_card, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(iaq_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(iaq_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(iaq_card, 10, 0);

    make_label(iaq_card, "Air Quality", &lv_font_montserrat_20, kTextMuted);

    s_iaq_value = make_label(iaq_card, "--", &lv_font_montserrat_38, kTextMain);
    lv_obj_set_style_pad_top(s_iaq_value, 4, 0);

    s_iaq_category = make_label(iaq_card, "", &lv_font_montserrat_24, kTextMain);
    lv_obj_set_width(s_iaq_category, LV_PCT(100));
    lv_label_set_long_mode(s_iaq_category, LV_LABEL_LONG_WRAP);

    s_iaq_bar = lv_bar_create(iaq_card);
    lv_obj_set_size(s_iaq_bar, LV_PCT(100), 14);
    lv_bar_set_range(s_iaq_bar, 0, 500);
    lv_bar_set_value(s_iaq_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_iaq_bar, lv_color_hex(0x0D1B2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_iaq_bar, LV_OPA_COVER, LV_PART_MAIN);

    make_label(iaq_card, "IAQ index 0-500, lower is better", &lv_font_montserrat_14, kTextDim);

    s_status = make_label(iaq_card, "Waiting for sensor...", &lv_font_montserrat_16, kTextMuted);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(s_status, 6, 0);

    // --- Right: 2x3 reading tiles ---
    lv_obj_t *grid = lv_obj_create(s_now_view);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 470, kBodyH);
    lv_obj_align(grid, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    s_temp = make_tile(grid, "Temperature", s_fahrenheit ? "°F" : "°C");
    s_hum = make_tile(grid, "Humidity", "%");
    s_pres = make_tile(grid, "Pressure", "hPa");
    s_co2 = make_tile(grid, "CO2 equivalent", "ppm");
    s_tvoc = make_tile(grid, "TVOC equivalent", "ppb");
    s_gas = make_tile(grid, "Gas resistance", "kOhm");

    build_history_view(parent);
    return parent;
}

void page_environment_update(const EnvReading &r) {
    const bool running = (r.state == EnvSensorState::Running);

    set_tile(s_temp, running, "%.1f", s_fahrenheit ? r.temperature_c * 9.0f / 5.0f + 32.0f : r.temperature_c);
    set_tile(s_hum, running, "%.0f", r.humidity_pct);
    set_tile(s_pres, running, "%.0f", r.pressure_hpa);
    set_tile(s_co2, running, "%.0f", r.co2_ppm);
    set_tile(s_tvoc, running, "%.0f", r.tvoc_ppb);
    set_tile(s_gas, running, r.gas_resistance_ohm < 100000.0f ? "%.1f" : "%.0f", r.gas_resistance_ohm / 1000.0f);

    if (!running) {
        lv_label_set_text(s_iaq_value, "--");
        lv_label_set_text(s_iaq_category, "");
        lv_bar_set_value(s_iaq_bar, 0, LV_ANIM_OFF);
    } else {
        const IaqCategory &cat = category_for(r.iaq);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", r.iaq);
        lv_label_set_text(s_iaq_value, buf);
        lv_label_set_text(s_iaq_category, cat.name);
        // Accuracy 0 means BSEC is still stabilizing and the index is just a
        // placeholder (typically sits at 50) — show it dimmed.
        uint32_t color = r.iaq_accuracy == 0 ? kTextDim : cat.color;
        lv_obj_set_style_text_color(s_iaq_value, lv_color_hex(r.iaq_accuracy == 0 ? kTextDim : kTextMain), 0);
        lv_obj_set_style_text_color(s_iaq_category, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(s_iaq_bar, lv_color_hex(color), LV_PART_INDICATOR);
        lv_bar_set_value(s_iaq_bar, (int32_t)(r.iaq > 500 ? 500 : r.iaq), LV_ANIM_OFF);
    }

    char status[128];
    switch (r.state) {
        case EnvSensorState::Disabled:
            snprintf(status, sizeof(status), "Sensor disabled in config (env_sensor.enabled)");
            break;
        case EnvSensorState::NoLibrary:
            snprintf(status, sizeof(status), "Firmware built without the Bosch BSEC library");
            break;
        case EnvSensorState::NotFound:
            snprintf(status, sizeof(status), "BME690 not found.\nCheck wiring: P4 IO19 (SDA) / IO20 (SCL), P5 3.3V / GND");
            break;
        case EnvSensorState::Error:
            snprintf(status, sizeof(status), "Sensor error at 0x%02x - see serial log", r.i2c_address);
            break;
        case EnvSensorState::WarmingUp:
            snprintf(status, sizeof(status), "BME690 at 0x%02x - warming up...", r.i2c_address);
            break;
        case EnvSensorState::Running: {
            const char *acc;
            switch (r.iaq_accuracy) {
                case 0: acc = "Stabilizing - IAQ not reliable yet"; break;
                case 1: acc = "Calibrating (accuracy 1/3)"; break;
                case 2: acc = "Calibrating (accuracy 2/3)"; break;
                default: acc = "Calibrated (accuracy 3/3)"; break;
            }
            snprintf(status, sizeof(status), "%s%s", acc, r.run_in_done ? "" : "\nNew-sensor run-in in progress");
            break;
        }
    }
    lv_label_set_text(s_status, status);

    if (lv_tick_elaps(s_last_hist_refresh_ms) >= kHistoryRefreshMs) refresh_history();
}
