/* SPDX-License-Identifier: Apache-2.0
 * Apps-only LVGL CLIB allocator policy. Link with --wrap=lv_malloc_core and
 * --wrap=lv_realloc_core; do not wrap malloc/realloc/free process-wide.
 *
 * LVGL's lv_mem.c keeps ownership of zero_mem, calloc/zero initialization and
 * reallocf semantics. Its original lv_free_core continues to call IDF free(),
 * which accepts allocations from heap_caps_* in either internal RAM or PSRAM.
 * Display DMA remains owned by the explicit fixed internal staging allocator.
 */
#include <stdbool.h>
#include <stddef.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CLIB
#error "hosino_lvgl_memory requires LVGL's CLIB allocator"
#endif

static const char *TAG = "lvgl_memory";
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_logged_first;
static bool s_logged_failure;

extern void *__real_lv_malloc_core(size_t size);
extern void *__real_lv_realloc_core(void *pointer, size_t new_size);

#define EXTERNAL_CAPS (MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)
#define INTERNAL_CAPS (MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)

static void report_result(bool reallocation, size_t size, const void *result)
{
    const bool success = size != 0 && result != NULL;
    const bool external = success && esp_ptr_external_ram(result);
    bool log_first = false;
    bool log_failure = false;

    portENTER_CRITICAL(&s_log_lock);
    if (success && !s_logged_first) {
        s_logged_first = true;
        log_first = true;
    } else if (!success && size != 0 && !s_logged_failure) {
        s_logged_failure = true;
        log_failure = true;
    }
    portEXIT_CRITICAL(&s_log_lock);

    /* Only first success/failure are logged. Heap inspection stays outside the
     * short log-once lock; neither path calls an LVGL allocator. */
    if (log_first) {
        ESP_LOGI(TAG, "LVGL CLIB external-first active: first=%u bytes region=%s "
                 "internal_free=%u psram_free=%u",
                 (unsigned)size, external ? "PSRAM" : "internal",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (log_failure) {
        ESP_LOGE(TAG, "First LVGL allocation failure: %s %u bytes, "
                 "internal_largest=%u psram_largest=%u",
                 reallocation ? "realloc" : "malloc", (unsigned)size,
                 (unsigned)heap_caps_get_largest_free_block(INTERNAL_CAPS),
                 (unsigned)heap_caps_get_largest_free_block(EXTERNAL_CAPS));
    }
}

void *__wrap_lv_malloc_core(size_t size)
{
    /* lv_malloc(0) is intercepted by LVGL before reaching this function.
     * Preserve the original implementation for any direct core zero call. */
    void *result = size == 0 ? __real_lv_malloc_core(size) :
        heap_caps_malloc_prefer(size, 2, EXTERNAL_CAPS, INTERNAL_CAPS);
    report_result(false, size, result);
    return result;
}

void *__wrap_lv_realloc_core(void *pointer, size_t new_size)
{
    /* For nonzero sizes, IDF realloc_prefer also implements realloc(NULL,n).
     * Each unsuccessful preference leaves pointer and its bytes unchanged;
     * the old block is released only after a successful move. A direct n=0
     * call retains the real core's free/NULL semantics without a second try. */
    void *result = new_size == 0 ? __real_lv_realloc_core(pointer, new_size) :
        heap_caps_realloc_prefer(pointer, new_size, 2, EXTERNAL_CAPS, INTERNAL_CAPS);
    report_result(true, new_size, result);
    return result;
}
