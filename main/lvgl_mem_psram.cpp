// LVGL heap hooks (CONFIG_LV_USE_CUSTOM_MALLOC=y): every lv_malloc() goes to
// the ESP-IDF heap, preferring PSRAM and falling back to internal RAM.
//
// Why: LVGL's default builtin allocator is a fixed 64KB pool carved out of
// internal RAM, shared by every object on every page. Four tabs plus the
// Settings overlay outgrew it on real hardware — lv_malloc() returned NULL
// and LV_ASSERT_MALLOC's default handler (`while(1);`) froze taskLVGL
// (task watchdog on IDLE0, touch dead). PSRAM has ~6.5MB free; internal RAM
// is the scarce resource Wi-Fi/mbedTLS need, so LVGL shouldn't grow there.

#include "esp_heap_caps.h"
#include "lvgl.h"

static constexpr uint32_t kPsram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
static constexpr uint32_t kAny = MALLOC_CAP_8BIT;

extern "C" {

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    (void)mem;
    (void)bytes;
    return nullptr;  // not supported — the system heap is used directly
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    (void)pool;
}

void *lv_malloc_core(size_t size) {
    return heap_caps_malloc_prefer(size, 2, kPsram, kAny);
}

void *lv_realloc_core(void *p, size_t new_size) {
    return heap_caps_realloc_prefer(p, new_size, 2, kPsram, kAny);
}

void lv_free_core(void *p) {
    heap_caps_free(p);
}

// Reports the PSRAM heap, where LVGL's data actually lives.
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, kPsram);
    mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->free_cnt = info.free_blocks;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->max_used = mon_p->total_size - info.minimum_free_bytes;
    mon_p->used_pct = mon_p->total_size ? (uint8_t)(100 * info.total_allocated_bytes / mon_p->total_size) : 0;
    mon_p->frag_pct = info.total_free_bytes
                          ? (uint8_t)(100 - 100 * info.largest_free_block / info.total_free_bytes)
                          : 0;
}

lv_result_t lv_mem_test_core(void) {
    return heap_caps_check_integrity(kPsram, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

}  // extern "C"
