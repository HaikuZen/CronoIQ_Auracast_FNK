#include "env_history.h"

#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "env_history";

static constexpr size_t kMetrics = (size_t)EnvMetric::Count;

namespace {

struct Ring {
    size_t cap;
    int64_t bucket_us;
    float *data;           // cap * kMetrics, bucket-major
    size_t head;           // next slot to write
    size_t count;          // filled buckets (<= cap)
    int64_t cur_start_us;  // start of the bucket being accumulated, 0 = none yet
    double sum[kMetrics];
    uint32_t n[kMetrics];
};

Ring s_rings[2] = {
    {180, 60LL * 1000 * 1000, nullptr, 0, 0, 0, {}, {}},       // Hours3: 1 min
    {288, 5LL * 60 * 1000 * 1000, nullptr, 0, 0, 0, {}, {}},   // Hours24: 5 min
};

SemaphoreHandle_t s_lock = nullptr;

}  // namespace

static float partial_avg(const Ring &r, size_t m) {
    return r.n[m] ? (float)(r.sum[m] / r.n[m]) : NAN;
}

static void push_bucket(Ring &r) {
    float *slot = &r.data[r.head * kMetrics];
    for (size_t m = 0; m < kMetrics; m++) slot[m] = partial_avg(r, m);
    r.head = (r.head + 1) % r.cap;
    if (r.count < r.cap) r.count++;
    memset(r.sum, 0, sizeof(r.sum));
    memset(r.n, 0, sizeof(r.n));
}

// Closes every bucket whose time span has fully elapsed — including empty
// ones while the sensor was missing, so gaps show as gaps at the right
// place rather than the graph silently compressing time.
static void advance(Ring &r, int64_t now_us) {
    if (r.cur_start_us == 0) {
        r.cur_start_us = now_us;
        return;
    }
    size_t pushed = 0;
    while (now_us >= r.cur_start_us + r.bucket_us) {
        if (pushed >= r.cap) {  // gap longer than the whole window
            r.cur_start_us = now_us;
            break;
        }
        push_bucket(r);
        r.cur_start_us += r.bucket_us;
        pushed++;
    }
}

void env_history_init(void) {
    s_lock = xSemaphoreCreateMutex();
    for (Ring &r : s_rings) {
        r.data = static_cast<float *>(
            heap_caps_malloc_prefer(r.cap * kMetrics * sizeof(float), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_8BIT));
        if (!r.data) {
            ESP_LOGE(TAG, "Out of memory for history ring (%u points)", (unsigned)r.cap);
        }
    }
}

void env_history_add(const EnvReading &reading) {
    if (!s_lock || reading.state != EnvSensorState::Running) return;

    float v[kMetrics];
    v[(size_t)EnvMetric::Temperature] = reading.temperature_c;
    v[(size_t)EnvMetric::Humidity] = reading.humidity_pct;
    v[(size_t)EnvMetric::Pressure] = reading.pressure_hpa;
    const bool iaq_valid = reading.iaq_accuracy >= 1;
    v[(size_t)EnvMetric::Iaq] = iaq_valid ? reading.iaq : NAN;
    v[(size_t)EnvMetric::Co2] = iaq_valid ? reading.co2_ppm : NAN;

    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (Ring &r : s_rings) {
        if (!r.data) continue;
        advance(r, now);
        for (size_t m = 0; m < kMetrics; m++) {
            if (std::isnan(v[m])) continue;
            r.sum[m] += v[m];
            r.n[m]++;
        }
    }
    xSemaphoreGive(s_lock);
}

size_t env_history_points(EnvRange range) {
    return s_rings[(size_t)range].cap;
}

void env_history_get(EnvMetric metric, EnvRange range, float *out) {
    Ring &r = s_rings[(size_t)range];
    const size_t m = (size_t)metric;
    for (size_t i = 0; i < r.cap; i++) out[i] = NAN;
    if (!s_lock || !r.data) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Catch up first so a sensor that stopped reporting shows as a growing
    // gap at the right edge instead of a frozen graph.
    if (r.cur_start_us != 0) advance(r, esp_timer_get_time());

    // Last cap-1 closed buckets, then the current partial bucket as "now".
    const size_t closed = r.count < r.cap - 1 ? r.count : r.cap - 1;
    for (size_t i = 0; i < closed; i++) {
        size_t idx = (r.head + r.cap - closed + i) % r.cap;
        out[r.cap - 1 - closed + i] = r.data[idx * kMetrics + m];
    }
    out[r.cap - 1] = partial_avg(r, m);
    xSemaphoreGive(s_lock);
}
