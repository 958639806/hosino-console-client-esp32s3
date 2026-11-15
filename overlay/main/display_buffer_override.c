/* Software-only experiment; no board wiring, scan registers or TE changes.
 * Both hooks are startup-only. The adapter owns the draw buffers and the
 * existing completion callback retains ownership until DMA finishes. */
#include "sdkconfig.h"

#if CONFIG_HOSINO_DISPLAY_BUFFERED

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_psram.h"
#if CONFIG_HOSINO_DISPLAY_CORNERS
#include "hosino_corner_clip.h"
#endif

static const char *TAG = "lcd_buffered";
static esp_lcd_panel_io_handle_t s_display_io;

esp_err_t __real_esp_lv_adapter_init(const esp_lv_adapter_config_t *cfg);

esp_err_t __wrap_esp_lv_adapter_init(const esp_lv_adapter_config_t *cfg)
{
    if (cfg == NULL) return __real_esp_lv_adapter_init(cfg);
    esp_lv_adapter_config_t adjusted = *cfg;
    if (adjusted.task_stack_size < 16384) adjusted.task_stack_size = 16384;
    ESP_LOGI(TAG, "LVGL task stack=%u bytes (was %u), stack_in_psram=%d",
             (unsigned)adjusted.task_stack_size, (unsigned)cfg->task_stack_size,
             adjusted.stack_in_psram);
    return __real_esp_lv_adapter_init(&adjusted);
}

esp_err_t __real_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                       const esp_lcd_panel_io_spi_config_t *cfg,
                                       esp_lcd_panel_io_handle_t *ret_io);

esp_err_t __wrap_esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                       const esp_lcd_panel_io_spi_config_t *cfg,
                                       esp_lcd_panel_io_handle_t *ret_io)
{
    if (cfg == NULL || ret_io == NULL ||
            bus != (esp_lcd_spi_bus_handle_t)SPI2_HOST || cfg->cs_gpio_num != 12) {
        return __real_esp_lcd_new_panel_io_spi(bus, cfg, ret_io);
    }

    esp_lcd_panel_io_spi_config_t lcd_cfg = *cfg;
    /* This board produced DMA TX underflow at 80 MHz with direct PSRAM DMA.
     * Keep the PSRAM draw buffers, but use IDF's internal DMA copy
     * path. The LCD IO splits transfers at SPI2's 32768-byte hardware limit;
     * it does not allocate a 460800-byte internal full-frame bounce buffer. */
    lcd_cfg.flags.psram_dma_direct = 0;
    const esp_err_t err = __real_esp_lcd_new_panel_io_spi(bus, &lcd_cfg, ret_io);
    if (err == ESP_OK) {
        s_display_io = *ret_io;
        ESP_LOGI(TAG, "SPI2/CS12 color transfers: internal DMA staging, direct PSRAM DMA disabled");
    }
    return err;
}

lv_display_t *__real_esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *cfg);

lv_display_t *__wrap_esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *cfg)
{
    if (cfg == NULL || s_display_io == NULL || cfg->panel_io != s_display_io) {
        return __real_esp_lv_adapter_register_display(cfg);
    }
    if (cfg->profile.interface != ESP_LV_ADAPTER_PANEL_IF_OTHER ||
            cfg->profile.hor_res != 480 || cfg->profile.ver_res != 480 ||
            cfg->tear_avoid_mode != ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE) {
        ESP_LOGE(TAG, "Unexpected LCD profile; refusing buffered display setup");
        return NULL;
    }

    esp_lv_adapter_display_config_t buffered = *cfg;
    buffered.profile.buffer_height = CONFIG_HOSINO_DISPLAY_BUFFER_HEIGHT;
    buffered.profile.use_psram = true;
    buffered.profile.require_double_buffer = true;
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM total=%u free=%u largest=%u; draw buffers=2x%u bytes",
             (unsigned)esp_psram_get_size(), (unsigned)before,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)(480 * CONFIG_HOSINO_DISPLAY_BUFFER_HEIGHT * 2));

    /* NONE keeps the adapter's LVGL PARTIAL render mode. The default returns
     * to the proven 50-row pipeline after full-height buffers slowed scrolling.
     * Do not use DIRECT: the SPI adapter
     * packs stride and swaps RGB565 in-place before submission. */
    lv_display_t *disp = __real_esp_lv_adapter_register_display(&buffered);
    if (disp != NULL) {
#if CONFIG_HOSINO_DISPLAY_CORNERS
        const esp_err_t corner_err = hosino_corner_clip_attach(&buffered, disp);
        if (corner_err != ESP_OK) {
            ESP_LOGE(TAG, "Corner clipping unavailable; original display path retained: %s", esp_err_to_name(corner_err));
        }
#endif
        ESP_LOGI(TAG, "Buffered display ready: height=%u count=2 mode=PARTIAL free PSRAM=%u",
                 (unsigned)CONFIG_HOSINO_DISPLAY_BUFFER_HEIGHT,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    return disp;
}

#endif
