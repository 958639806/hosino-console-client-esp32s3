/*
 * Override only the board's display QSPI clock for the 40/80 MHz comparison.
 * This is the display bus clock, not the panel refresh rate or Flash/PSRAM clock.
 */
#include "sdkconfig.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#if defined(CONFIG_APP_LCD_QSPI_80MHZ)
#define APP_LCD_QSPI_REQUESTED_HZ 80000000
#elif defined(CONFIG_APP_LCD_QSPI_40MHZ)
#define APP_LCD_QSPI_REQUESTED_HZ 40000000
#else
#error "Select a display QSPI clock in the Screen QSPI clock test menu"
#endif

static const char *TAG = "lcd_qspi_clock";

esp_err_t __real_spi_bus_add_device(spi_host_device_t host_id,
                                  const spi_device_interface_config_t *dev_config,
                                  spi_device_handle_t *handle);

esp_err_t __wrap_spi_bus_add_device(spi_host_device_t host_id,
                                  const spi_device_interface_config_t *dev_config,
                                  spi_device_handle_t *handle)
{
    /* Forward every other SPI device, including invalid inputs, unchanged. */
    if (dev_config == NULL || host_id != SPI2_HOST || dev_config->spics_io_num != 12) {
        return __real_spi_bus_add_device(host_id, dev_config, handle);
    }

    const int baseline_hz = dev_config->clock_speed_hz;
    spi_device_interface_config_t display_config = *dev_config;
    display_config.clock_speed_hz = APP_LCD_QSPI_REQUESTED_HZ;

    const esp_err_t result = __real_spi_bus_add_device(host_id, &display_config, handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "SPI2/CS12 display registration failed: baseline_hz=%d requested_hz=%d error=%s",
                 baseline_hz, APP_LCD_QSPI_REQUESTED_HZ, esp_err_to_name(result));
        return result;
    }

    int actual_khz = 0;
    const esp_err_t frequency_result = spi_device_get_actual_freq(*handle, &actual_khz);
    if (frequency_result == ESP_OK) {
        ESP_LOGI(TAG, "SPI2/CS12 display QSPI: baseline_hz=%d requested_hz=%d actual_khz=%d",
                 baseline_hz, APP_LCD_QSPI_REQUESTED_HZ, actual_khz);
    } else {
        ESP_LOGW(TAG, "SPI2/CS12 display QSPI: baseline_hz=%d requested_hz=%d actual frequency unavailable: %s",
                 baseline_hz, APP_LCD_QSPI_REQUESTED_HZ, esp_err_to_name(frequency_result));
    }

    return result;
}
