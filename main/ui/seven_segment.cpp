#include "seven_segment.h"

#include <cstring>

struct SsegStyle {
    lv_color_t on;
    lv_color_t off;
    int32_t height;
};

// Segment order: a=top, b=top-right, c=bottom-right, d=bottom,
// e=bottom-left, f=top-left, g=middle.
static const uint8_t kDigitSegments[10][7] = {
    {1, 1, 1, 1, 1, 1, 0},  // 0
    {0, 1, 1, 0, 0, 0, 0},  // 1
    {1, 1, 0, 1, 1, 0, 1},  // 2
    {1, 1, 1, 1, 0, 0, 1},  // 3
    {0, 1, 1, 0, 0, 1, 1},  // 4
    {1, 0, 1, 1, 0, 1, 1},  // 5
    {1, 0, 1, 1, 1, 1, 1},  // 6
    {1, 1, 1, 0, 0, 0, 0},  // 7
    {1, 1, 1, 1, 1, 1, 1},  // 8
    {1, 1, 1, 1, 0, 1, 1},  // 9
};

static lv_obj_t *make_seg(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                           lv_color_t color) {
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_set_pos(seg, x, y);
    lv_obj_set_size(seg, w, h);
    lv_obj_set_style_radius(seg, LV_MIN(w, h) / 3, 0);
    lv_obj_set_style_bg_color(seg, color, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    return seg;
}

static int32_t create_digit(lv_obj_t *cont, int32_t x, int32_t h, char digit, lv_color_t on,
                                lv_color_t off) {
    const int32_t t = LV_MAX((int32_t)(h * 0.14f), 3);
    const int32_t w = (int32_t)(h * 0.55f);
    const int32_t vert_len = LV_MAX((h / 2) - (int32_t)(t * 1.5f), 4);
    // Adjacent segments are placed to exactly abut, which on real hardware
    // showed as a visible 1-2px seam at every joint (int32_t truncation in
    // the h/2, t/2, t*1.5f math above means the touching edges don't
    // reliably land on the same pixel). Overlapping each joint by `ov`
    // pixels — at least as large as the corner radius below — hides both
    // the rounding error and the rounded-corner notch that would otherwise
    // show where a horizontal and vertical segment meet.
    const int32_t ov = LV_MAX(t / 3, 2);

    const uint8_t *segs = kDigitSegments[9];  // '8'-shape fallback when blank/unknown
    bool blank = (digit < '0' || digit > '9');
    if (!blank) segs = kDigitSegments[digit - '0'];

    auto seg_color = [&](int idx) { return (!blank && segs[idx]) ? on : off; };

    // a (top)
    make_seg(cont, x + t - ov, 0, w - 2 * t + 2 * ov, t, seg_color(0));
    // b (top-right)
    make_seg(cont, x + w - t, t - ov, t, vert_len + 2 * ov, seg_color(1));
    // c (bottom-right)
    make_seg(cont, x + w - t, h / 2 + t / 2 - ov, t, vert_len + 2 * ov, seg_color(2));
    // d (bottom)
    make_seg(cont, x + t - ov, h - t, w - 2 * t + 2 * ov, t, seg_color(3));
    // e (bottom-left)
    make_seg(cont, x, h / 2 + t / 2 - ov, t, vert_len + 2 * ov, seg_color(4));
    // f (top-left)
    make_seg(cont, x, t - ov, t, vert_len + 2 * ov, seg_color(5));
    // g (middle)
    make_seg(cont, x + t - ov, h / 2 - t / 2, w - 2 * t + 2 * ov, t, seg_color(6));

    return w;
}

static int32_t create_colon(lv_obj_t *cont, int32_t x, int32_t h, lv_color_t on) {
    const int32_t t = LV_MAX((int32_t)(h * 0.14f), 3);
    const int32_t colon_w = t;
    make_seg(cont, x, (int32_t)(h * 0.30f), colon_w, t, on);
    make_seg(cont, x, (int32_t)(h * 0.62f), colon_w, t, on);
    return colon_w;
}

lv_obj_t *sseg_time_create(lv_obj_t *parent, int32_t digit_height, lv_color_t on_color, lv_color_t off_color) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cont, 1, digit_height);

    auto *style = new SsegStyle{on_color, off_color, digit_height};
    lv_obj_set_user_data(cont, style);
    lv_obj_add_event_cb(
        cont,
        [](lv_event_t *e) {
            auto *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
            auto *s = static_cast<SsegStyle *>(lv_obj_get_user_data(target));
            delete s;
        },
        LV_EVENT_DELETE, nullptr);

    return cont;
}

void sseg_time_set_text(lv_obj_t *cont, const char *text) {
    auto *style = static_cast<SsegStyle *>(lv_obj_get_user_data(cont));
    if (!style) return;

    lv_obj_clean(cont);

    const int32_t h = style->height;
    const int32_t gap = LV_MAX((int32_t)(h * 0.10f), 2);
    int32_t x = 0;

    for (const char *p = text; *p; p++) {
        if (*p == ':') {
            x += create_colon(cont, x, h, style->on) + gap;
        } else {
            x += create_digit(cont, x, h, *p, style->on, style->off) + gap;
        }
    }
    if (x > gap) x -= gap;
    lv_obj_set_size(cont, x, h);
}
