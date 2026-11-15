/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "boost/thread.hpp"
#include "esp_lv_adapter.h"
#include "esp_heap_caps.h"
#include "hosino_ui_fonts.h"
#include "hosino_sdcard_override.h"
#include "nvs_flash.h"
#include "system_status.hpp"
#include "Drawpanel.hpp"
#include "Gallery.hpp"
#include "Settings.hpp"
#include "Recorder.hpp"
#include "SpecAnalyzer.hpp"
#include "MusicPlayer.hpp"
#include "VideoPlayer.hpp"
#include "BiliBili.hpp"
#include "hosino_bili_provision.h"
void hosino_bili_install_media_hooks(esp_brookesia::apps::BiliBili *);
#if CONFIG_HOSINO_PERF_DEMO
#include "hosino_perf_overlay.h"
#endif
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "Main"
#include "esp_lib_utils.h"

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

constexpr bool EXAMPLE_SHOW_MEM_INFO = false;

// Run under the existing GUI lock/timer; no extra task or display access.
static void update_recents_memory(Phone *phone)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t external_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_UTILS_CHECK_FALSE_EXIT(phone->getDisplay().getRecentsScreen()->setMemoryLabel(
        internal_free / 1024, internal_total / 1024, external_free / 1024, external_total / 1024
    ), "Set live memory label failed");
    static unsigned samples;
    if (samples++ % 5 == 0) {
        ESP_UTILS_LOGI("Live memory: internal_free=%u internal_total=%u external_free=%u external_total=%u bytes",
                      (unsigned)internal_free, (unsigned)internal_total,
                      (unsigned)external_free, (unsigned)external_total);
    }
}

#ifdef HOSINO_INSTALL_MEDIA_SAMPLES
#include "hosino_media_samples.h"
#endif

extern "C" void app_main(void)
{
    ESP_UTILS_LOGI("Xingye localized application pack");
    // Allocate native Wi-Fi control buffers before GUI objects fragment SRAM.
    const esp_err_t nvs_result = nvs_flash_init();
    // Keep existing user settings intact if NVS needs recovery.
    esp_err_t wifi_result = nvs_result == ESP_OK
        ? brookesia::system_status::init_wifi_stack() : nvs_result;
    ESP_UTILS_LOGI("Wi-Fi initialization: %s", esp_err_to_name(wifi_result));
    // Reserve SD's bounce buffer once, before GUI allocations fragment SRAM.
    const esp_err_t sd_dma_result = hosino_sdcard_dma_init();
    ESP_UTILS_LOGI("SD DMA initialization: %s", esp_err_to_name(sd_dma_result));
    if (nvs_result == ESP_OK) {
        const esp_err_t config_result = hosino_bili_provision_start();
        ESP_UTILS_LOGI("Bili USB configuration: %s", esp_err_to_name(config_result));
    }
    ESP_UTILS_CHECK_NULL_EXIT(bsp_display_start(), "Start display failed");
    ESP_UTILS_LOGI("Display started; preparing desktop");

    /* Configure GUI lock */
    LvLock::registerCallbacks([](int timeout_ms) {
        esp_err_t ret = esp_lv_adapter_lock(timeout_ms);
        ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, false, "Lock failed (timeout_ms: %d)", timeout_ms);

        return true;
    }, []() {
        bsp_display_unlock();
        return true;
    });

    /* Create a phone object */
    Phone *phone = new (std::nothrow) Phone();
    ESP_UTILS_CHECK_NULL_EXIT(phone, "Create phone failed");
    ESP_UTILS_LOGI("Phone created; acquiring GUI lock");

    {
        // When operating on non-GUI tasks, should acquire a lock before operating on LVGL
        LvLockGuard gui_guard;
        ESP_UTILS_LOGI("GUI lock acquired; configuring display brightness");
        // The adapter has already started its refresh task. Serialize this
        // panel command with LVGL flushes; LCD SPI IO has shared transaction
        // bookkeeping and must not be accessed concurrently by these tasks.
        ESP_UTILS_CHECK_ERROR_EXIT(bsp_display_backlight_on(), "Turn on display backlight failed");

        // Keep the stable default layout. Only move status-bar content inward
        // and give app names and the recents memory label a Chinese font.
        static Stylesheet desktop_style = STYLESHEET_DEFAULT_DARK;
        desktop_style.core.name = "Xingye Chinese";
        desktop_style.display.status_bar.data.area.data[0].layout_column_start_offset = 40;
        desktop_style.display.status_bar.data.area.data[1].layout_column_start_offset = 40;
        desktop_style.display.status_bar.data.flags.enable_battery_label = 1;
        desktop_style.display.app_launcher.data.icon.label.text_font =
            StyleFont::CUSTOM_SIZE(16, &hosino_font_16);
        auto &recents_style = desktop_style.display.recents_screen.data;
        recents_style.snapshot_table.snapshot.title.text_font =
            StyleFont::CUSTOM_SIZE(16, &hosino_font_16);
        recents_style.memory.label_text_font = StyleFont::CUSTOM_SIZE(16, &hosino_font_16);
        recents_style.main.layout_top_pad = 40;
        recents_style.memory.main_size = StyleSize::RECT(400, hosino_font_16.line_height);
        recents_style.memory.main_layout_x_right_offset = 0;
        // The original title and memory containers are only 16 px high.
        // Accommodate the font's actual line height without scaling its glyphs.
        if (hosino_font_16.line_height > 16) {
            recents_style.snapshot_table.snapshot.title.main_size.height = hosino_font_16.line_height;
            recents_style.snapshot_table.snapshot.title.icon_size = StyleSize::SQUARE(16);
            recents_style.memory.main_size.height = hosino_font_16.line_height;
        }
        ESP_UTILS_CHECK_FALSE_EXIT(phone->addStylesheet(desktop_style), "Add desktop stylesheet failed");
        ESP_UTILS_CHECK_FALSE_EXIT(phone->activateStylesheet(desktop_style), "Activate desktop stylesheet failed");

        /* Begin the phone */
        ESP_UTILS_CHECK_FALSE_EXIT(phone->begin(), "Begin failed");
        ESP_UTILS_LOGI("Brookesia desktop initialized");
        ESP_UTILS_LOGI("Chinese UI enabled; status bar left/right inset=40px");
        // assert(phone->getDisplay().showContainerBorder() && "Show container border failed");

        /* Init and install apps from registry */
        std::vector<systems::base::Manager::RegistryAppInfo> inited_apps;
        ESP_UTILS_CHECK_FALSE_EXIT(phone->initAppFromRegistry(inited_apps), "Init app registry failed");
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installAppFromRegistry(inited_apps), "Install app registry failed");
        ESP_UTILS_LOGI("Application registry installed (%u apps)", (unsigned)inited_apps.size());
        auto install_app = [phone](systems::base::App *app, const char *name) {
            if (app == nullptr) { ESP_UTILS_LOGE("Allocation failed: %s", name); return; }
            const int id = phone->getManager().installApp(app);
            ESP_UTILS_LOGI("Installed %s id=%d", name, id);
        };
        install_app(apps::Drawpanel::requestInstance(true, false), "Drawpanel");
        install_app(apps::Gallery::requestInstance(true, false), "Gallery");
        install_app(apps::Settings::requestInstance(true, false), "Settings");
        install_app(apps::Recorder::requestInstance(true, false), "Recorder");
        install_app(apps::SpecAnalyzer::requestInstance(true, false), "SpecAnalyzer");
        install_app(apps::MusicPlayer::requestInstance(true, false), "MusicPlayer");
        install_app(apps::VideoPlayer::requestInstance(true, false), "VideoPlayer");
        auto *bilibili = apps::BiliBili::requestInstance(true, false);
        hosino_bili_install_media_hooks(bilibili);
        install_app(bilibili, "BiliBili");
        const esp_err_t status_result = brookesia::system_status::start(phone->getDisplay().getStatusBar());
        ESP_UTILS_LOGI("Real system status: %s", esp_err_to_name(status_result));

#if CONFIG_HOSINO_PERF_DEMO
        hosino_perf_overlay_init(lv_display_get_default());
#endif
        update_recents_memory(phone);

        /* Create a timer to update the clock */
        lv_timer_create([](lv_timer_t *t) {
            time_t now;
            struct tm timeinfo;
            Phone *phone = (Phone *)t->user_data;

            ESP_UTILS_CHECK_NULL_EXIT(phone, "Invalid phone");

            time(&now);
            localtime_r(&now, &timeinfo);

            ESP_UTILS_CHECK_FALSE_EXIT(
                phone->getDisplay().getStatusBar()->setClock(timeinfo.tm_hour, timeinfo.tm_min),
                "Refresh status bar failed"
            );
            if (phone->getDisplay().getRecentsScreen()->checkVisible()) {
                update_recents_memory(phone);
            }
        }, 1000, phone);
    }

#ifdef HOSINO_INSTALL_MEDIA_SAMPLES
    hosino_media_samples_start();
#endif

    if constexpr (EXAMPLE_SHOW_MEM_INFO) {
        esp_utils::thread_config_guard thread_config({
            .name = "mem_info",
            .stack_size = 4096,
        });
        boost::thread([ = ]() {
            char buffer[128];    /* Make sure buffer is enough for `sprintf` */
            size_t internal_free = 0;
            size_t internal_total = 0;
            size_t external_free = 0;
            size_t external_total = 0;

            while (1) {
                internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
                external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                external_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                sprintf(buffer,
                        "\t           Biggest /     Free /    Total\n"
                        "\t  SRAM : [%8d / %8d / %8d]\n"
                        "\t PSRAM : [%8d / %8d / %8d]",
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), internal_free, internal_total,
                        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), external_free, external_total);
                ESP_UTILS_LOGI("\n%s", buffer);

                {
                    LvLockGuard gui_guard;
                    ESP_UTILS_CHECK_FALSE_EXIT(
                        phone->getDisplay().getRecentsScreen()->setMemoryLabel(
                            internal_free / 1024, internal_total / 1024, external_free / 1024, external_total / 1024
                        ), "Set memory label failed"
                    );
                }

                boost::this_thread::sleep_for(boost::chrono::seconds(5));
            }
        }).detach();
    }
}
