/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "StoragePage.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "../Settings.hpp"
#include "SettingsUI.hpp"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "system_status.hpp"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:StoragePage"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps {

namespace {

constexpr size_t BENCHMARK_BYTES = 4U * 1024U * 1024U;
constexpr char DIAGNOSTICS_DIRECTORY[] = STORAGE_SERVICE_MOUNT_POINT "/Waveshare/Diagnostics";
constexpr char DIAGNOSTICS_PATH[] =
    STORAGE_SERVICE_MOUNT_POINT "/Waveshare/Diagnostics/device-diagnostics.txt";

lv_obj_t *getValueLabel(lv_obj_t *row)
{
    if (row == nullptr) {
        return nullptr;
    }
    const uint32_t child_count = lv_obj_get_child_count(row);
    return child_count > 0 ? lv_obj_get_child(row, static_cast<int32_t>(child_count - 1)) : nullptr;
}

const char *storageStateText(storage_service_state_t state)
{
    switch (state) {
    case STORAGE_SERVICE_STATE_UNINITIALIZED: return "未初始化";
    case STORAGE_SERVICE_STATE_UNMOUNTED: return "未挂载";
    case STORAGE_SERVICE_STATE_MOUNTING: return "正在读取";
    case STORAGE_SERVICE_STATE_MOUNTED: return "已就绪";
    case STORAGE_SERVICE_STATE_EJECTING: return "正在弹出";
    case STORAGE_SERVICE_STATE_ERROR: return "未能读取";
    default: return "未知";
    }
}

void formatBytes(uint64_t bytes, char *buffer, size_t buffer_size)
{
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buffer, buffer_size, "%.2f GiB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buffer, buffer_size, "%.1f MiB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buffer, buffer_size, "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(buffer, buffer_size, "%llu B", static_cast<unsigned long long>(bytes));
    }
}

lv_obj_t *addActionButton(lv_obj_t *list, const char *text, lv_style_t &row_style,
                          lv_style_t &pressed_style, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_list_add_button(list, nullptr, text);
    lv_obj_add_style(button, &row_style, LV_PART_MAIN);
    lv_obj_add_style(button, &pressed_style, LV_STATE_PRESSED);
    settings_ui::use_ellipsis_for_button_label(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    return button;
}

esp_err_t ensureDirectory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t exportDiagnostics(char *path, size_t path_size)
{
    storage_service_lease_t lease = {};
    esp_err_t result = storage_service_acquire(&lease);
    if (result != ESP_OK) {
        return result;
    }

    result = ensureDirectory(STORAGE_SERVICE_MOUNT_POINT "/Waveshare");
    if (result == ESP_OK) {
        result = ensureDirectory(DIAGNOSTICS_DIRECTORY);
    }

    storage_service_info_t storage = {};
    if (result == ESP_OK) {
        result = storage_service_get_info(&storage);
    }

    FILE *file = nullptr;
    if (result == ESP_OK) {
        file = fopen(DIAGNOSTICS_PATH, "wb");
        if (file == nullptr) {
            result = ESP_FAIL;
        }
    }

    if (file != nullptr) {
        esp_chip_info_t chip = {};
        esp_chip_info(&chip);
        brookesia::system_status::Snapshot status = {};
        const bool status_valid = brookesia::system_status::get_snapshot(status);

        bool ok = fprintf(file,
                          "Waveshare ESP32-S3-Touch-AMOLED-2.16 diagnostics\n"
                          "idf_version=%s\n"
                          "uptime_ms=%lld\n"
                          "reset_reason=%d\n"
                          "chip_cores=%u\n"
                          "chip_revision=%u\n"
                          "heap_free=%u\n"
                          "heap_minimum=%u\n"
                          "psram_free=%u\n"
                          "sd_state=%s\n"
                          "sd_error=%s\n"
                          "sd_card=%s\n"
                          "sd_capacity_bytes=%llu\n"
                          "sd_free_bytes=%llu\n"
                          "sd_bus_width=%lu\n"
                          "sd_frequency_khz=%lu\n"
                          "sd_generation=%lu\n"
                          "sd_active_leases=%lu\n",
                          esp_get_idf_version(),
                          static_cast<long long>(esp_timer_get_time() / 1000),
                          static_cast<int>(esp_reset_reason()),
                          static_cast<unsigned>(chip.cores),
                          static_cast<unsigned>(chip.revision),
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                          static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                          storage_service_state_name(storage.state),
                          esp_err_to_name(storage.last_error),
                          storage.card_name[0] ? storage.card_name : "unknown",
                          static_cast<unsigned long long>(storage.capacity_bytes),
                          static_cast<unsigned long long>(storage.free_bytes),
                          static_cast<unsigned long>(storage.bus_width),
                          static_cast<unsigned long>(storage.frequency_khz),
                          static_cast<unsigned long>(storage.generation),
                          static_cast<unsigned long>(storage.active_leases)) >= 0;

        if (status_valid) {
            ok = ok && fprintf(file,
                               "battery_valid=%d\n"
                               "battery_present=%d\n"
                               "battery_percent=%d\n"
                               "battery_charging=%d\n"
                               "wifi_enabled=%d\n"
                               "wifi_connected=%d\n"
                               "wifi_rssi=%d\n",
                               status.battery_valid,
                               status.battery_present,
                               status.battery_percent,
                               status.charging,
                               status.wifi_enabled,
                               status.wifi_connected,
                               status.wifi_rssi) >= 0;
        } else {
            ok = ok && fputs("system_status=unavailable\n", file) != EOF;
        }
        ok = ok && fflush(file) == 0;
        ok = ok && fsync(fileno(file)) == 0;
        ok = fclose(file) == 0 && ok;
        file = nullptr;
        if (!ok) {
            result = ESP_FAIL;
        }
    }

    if (file != nullptr) {
        (void)fclose(file);
    }
    storage_service_release(&lease);

    if (result == ESP_OK) {
        strlcpy(path, DIAGNOSTICS_PATH, path_size);
    }
    return result;
}

} // namespace

StoragePage *StoragePage::_instance = nullptr;

StoragePage *StoragePage::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new StoragePage(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

StoragePage::StoragePage(bool use_status_bar, bool use_navigation_bar)
    : App("存储", nullptr, true, use_status_bar, use_navigation_bar)
{
}

StoragePage::~StoragePage()
{
    page_active = false;
}

bool StoragePage::run()
{
    ESP_UTILS_LOGD("StoragePage Run");
    page_active = false;
    const uint32_t generation = page_generation.fetch_add(1) + 1;
    (void)generation;
    destroyUi();
    lv_obj_clean(lv_screen_active());
    createUi();
    page_active = true;

    if (!ensureWorker()) {
        setBusy(false, "服务暂不可用");
        return false;
    }
    setBusy(true, "正在检查存储卡…");
    if (!enqueue(WorkerCommand::Refresh)) {
        setBusy(false, "请求繁忙，请稍后再试");
    }
    return true;
}

bool StoragePage::back()
{
    ESP_UTILS_LOGD("StoragePage Back");
    Settings::requestInstance()->showRootPage();
    return true;
}

bool StoragePage::close()
{
    ESP_UTILS_LOGD("StoragePage Close");
    page_active = false;
    page_generation.fetch_add(1);
    destroyUi();
    return true;
}

bool StoragePage::ensureWorker()
{
    if (worker_task != nullptr && worker_queue != nullptr) {
        return true;
    }
    if (worker_queue == nullptr) {
        worker_queue = xQueueCreate(6, sizeof(WorkerRequest));
        if (worker_queue == nullptr) {
            return false;
        }
    }
    if (xTaskCreate(workerTask, "settings_sd", 7 * 1024, this, 3, &worker_task) != pdPASS) {
        vQueueDelete(worker_queue);
        worker_queue = nullptr;
        worker_task = nullptr;
        return false;
    }
    return true;
}

bool StoragePage::enqueue(WorkerCommand command)
{
    if (worker_queue == nullptr) {
        return false;
    }
    const WorkerRequest request = {
        .command = command,
        .generation = page_generation.load(),
    };
    return xQueueSend(worker_queue, &request, 0) == pdTRUE;
}

void StoragePage::createUi()
{
    page_root = settings_ui::create_page(lv_screen_active());
    settings_ui::create_header(page_root, "存储", [](lv_event_t *event) {
        (void)event;
        Settings::requestInstance()->requestRootPage();
    });

    settings_ui::init_list_styles(style_list, style_row, style_section, style_pressed);
    list = settings_ui::create_content_list(page_root);
    lv_obj_add_style(list, &style_list, LV_PART_MAIN);

    settings_ui::add_section(list, "本机存储 · Flash", style_section);
    flash_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "物理容量", "正在读取…", style_row));
    app_size_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "固件占用", "--", style_row));
    app_partition_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "程序分区", "--", style_row));
    unallocated_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "尾部未分配", "--", style_row));
    nvs_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "NVS 条目", "--", style_row));
    settings_ui::add_detail_row(list, "本机文件空间", "当前未建立本机文件存储区，照片、音乐和录音保存到内存卡。", style_row);
    settings_ui::add_section(list, "运行内存", style_section);
    internal_ram_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "内部可用", "--", style_row));
    psram_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "外部可用", "--", style_row));
    char ram_text[48];
    formatBytes(esp_psram_get_size(), ram_text, sizeof(ram_text));
    settings_ui::add_info_row(list, nullptr, "PSRAM 容量", ram_text, style_row);
    refreshMemory();
    memory_timer = lv_timer_create([](lv_timer_t *timer) {
        auto *page = static_cast<StoragePage *>(lv_timer_get_user_data(timer));
        if (page && page->page_active) page->refreshMemory();
    }, 1000, this);

    settings_ui::add_section(list, "内存卡", style_section);
    status_value = getValueLabel(settings_ui::add_info_row(list, LV_SYMBOL_SD_CARD,
                                                            "状态", "正在检查…", style_row));
    card_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "存储卡", "--", style_row));
    filesystem_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "文件系统", "--", style_row));
    used_value = getValueLabel(settings_ui::add_info_row(list, nullptr, "已用空间", "--", style_row));
    capacity_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "总容量", "--", style_row));
    free_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "可用空间", "--", style_row));
    bus_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "总线", "SDMMC 1-bit", style_row));
    frequency_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "时钟频率", "--", style_row));
    (void)settings_ui::add_info_row(list, nullptr, "检测方式", "手动重新扫描", style_row);

    settings_ui::add_section(list, "读写测试", style_section);
    write_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "连续写入", "--", style_row));
    read_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "连续读取", "--", style_row));
    crc_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "数据校验", "尚未测试", style_row));

    settings_ui::add_section(list, "操作", style_section);
    refresh_button = addActionButton(list, LV_SYMBOL_REFRESH "  重新扫描存储卡", style_row,
                                     style_pressed, actionEventCallback, this);
    benchmark_button = addActionButton(list, LV_SYMBOL_PLAY "  运行 4 MiB 测试", style_row,
                                       style_pressed, actionEventCallback, this);
    export_button = addActionButton(list, LV_SYMBOL_UPLOAD "  导出诊断信息", style_row,
                                    style_pressed, actionEventCallback, this);
    eject_button = addActionButton(list, LV_SYMBOL_EJECT "  安全弹出", style_row,
                                   style_pressed, actionEventCallback, this);
    operation_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "最近操作", "就绪", style_row));
}

void StoragePage::destroyUi()
{
    if (memory_timer) { lv_timer_delete(memory_timer); memory_timer = nullptr; }
    if (page_root != nullptr) {
        lv_obj_delete(page_root);
        page_root = nullptr;
        list = nullptr;
        status_value = nullptr;
        card_value = filesystem_value = used_value = nullptr;
        flash_value = app_size_value = app_partition_value = unallocated_value = nvs_value = nullptr;
        internal_ram_value = psram_value = nullptr;
        capacity_value = nullptr;
        free_value = nullptr;
        bus_value = nullptr;
        frequency_value = nullptr;
        operation_value = nullptr;
        write_value = nullptr;
        read_value = nullptr;
        crc_value = nullptr;
        refresh_button = nullptr;
        benchmark_button = nullptr;
        export_button = nullptr;
        eject_button = nullptr;
        settings_ui::reset_list_styles(style_list, style_row, style_section, style_pressed);
    }
}

void StoragePage::setBusy(bool busy, const char *message)
{
    lv_obj_t *controls[] = {
        refresh_button, benchmark_button, export_button, eject_button,
    };
    for (lv_obj_t *control : controls) {
        if (control != nullptr) {
            if (busy) {
                lv_obj_add_state(control, LV_STATE_DISABLED);
            } else {
                lv_obj_remove_state(control, LV_STATE_DISABLED);
            }
        }
    }
    if (operation_value != nullptr && message != nullptr) {
        lv_label_set_text(operation_value, message);
    }
}

void StoragePage::actionEventCallback(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    auto *page = static_cast<StoragePage *>(lv_event_get_user_data(event));
    if (page == nullptr || !page->page_active) {
        return;
    }

    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    WorkerCommand command = WorkerCommand::Refresh;
    const char *message = "正在检查存储卡…";
    if (target == page->benchmark_button) {
        command = WorkerCommand::Benchmark;
        message = "正在测试 4 MiB…";
    } else if (target == page->export_button) {
        command = WorkerCommand::ExportDiagnostics;
        message = "正在导出…";
    } else if (target == page->eject_button) {
        command = WorkerCommand::Eject;
        message = "正在弹出…";
    }

    page->setBusy(true, message);
    if (!page->enqueue(command)) {
        page->setBusy(false, "请求繁忙，请稍后再试");
    }
}

void StoragePage::workerTask(void *argument)
{
    auto *page = static_cast<StoragePage *>(argument);
    WorkerRequest request = {};
    while (xQueueReceive(page->worker_queue, &request, portMAX_DELAY) == pdTRUE) {
        auto *result = static_cast<WorkerResult *>(calloc(1, sizeof(WorkerResult)));
        if (result == nullptr) {
            ESP_UTILS_LOGE("Allocate Storage worker result failed");
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                if (page->page_active && page->page_generation.load() == request.generation && page->page_root) {
                    page->setBusy(false, "内存不足，请重试");
                }
                esp_lv_adapter_unlock();
            }
            continue;
        }
        result->page = page;
        result->command = request.command;
        result->generation = request.generation;
        result->operation_result = ESP_OK;

        switch (request.command) {
        case WorkerCommand::Refresh:
            result->operation_result = storage_service_mount();
            break;
        case WorkerCommand::Benchmark:
            result->operation_result =
                storage_service_run_benchmark(BENCHMARK_BYTES, &result->benchmark);
            break;
        case WorkerCommand::Eject:
            result->operation_result = storage_service_safe_eject();
            break;
        case WorkerCommand::ExportDiagnostics:
            result->operation_result =
                exportDiagnostics(result->diagnostics_path, sizeof(result->diagnostics_path));
            break;
        }

        readInternalStorage(*result);
        result->info_result = storage_service_get_info(&result->info);
        lv_result_t async_result = LV_RESULT_INVALID;
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            async_result = lv_async_call(workerResultAsyncCallback, result);
            if (async_result != LV_RESULT_OK && page->page_active &&
                    page->page_generation.load() == request.generation && page->page_root) {
                page->setBusy(false, "结果显示失败，请重试");
            }
            esp_lv_adapter_unlock();
        }
        if (async_result != LV_RESULT_OK) {
            free(result);
        }
    }
    page->worker_task = nullptr;
    vTaskDelete(nullptr);
}

void StoragePage::workerResultAsyncCallback(void *argument)
{
    auto *result = static_cast<WorkerResult *>(argument);
    if (result == nullptr) {
        return;
    }
    StoragePage *page = result->page;
    if (page != nullptr && page->page_active &&
            page->page_generation.load() == result->generation && page->page_root != nullptr) {
        page->applyWorkerResult(*result);
    }
    free(result);
}

void StoragePage::applyInfo(const storage_service_info_t &info, esp_err_t info_result)
{
    if (status_value == nullptr) {
        return;
    }

    char text[64] = {};
    if (info.last_error != ESP_OK && info.state == STORAGE_SERVICE_STATE_ERROR) {
        snprintf(text, sizeof(text), "%s: %s", storageStateText(info.state),
                 esp_err_to_name(info.last_error));
    } else {
        strlcpy(text, storageStateText(info.state), sizeof(text));
    }
    lv_label_set_text(status_value, text);

    const bool has_card = info.capacity_bytes != 0;
    lv_label_set_text(card_value, has_card && info.card_name[0] ? info.card_name : "--");
    lv_label_set_text(filesystem_value, has_card && info.filesystem[0] ? info.filesystem : "--");
    if (has_card) {
        const bool space_valid = info_result == ESP_OK && info.filesystem_bytes > 0 &&
                                 info.filesystem_bytes >= info.free_bytes;
        if (space_valid) {
            formatBytes(info.filesystem_bytes - info.free_bytes, text, sizeof(text));
            lv_label_set_text(used_value, text);
            formatBytes(info.free_bytes, text, sizeof(text));
            lv_label_set_text(free_value, text);
        } else {
            lv_label_set_text(used_value, "不可用");
            lv_label_set_text(free_value, "不可用");
        }
        formatBytes(info.capacity_bytes, text, sizeof(text));
        lv_label_set_text(capacity_value, text);
        lv_label_set_text_fmt(bus_value, "SDMMC %lu-bit",
                              static_cast<unsigned long>(info.bus_width));
        lv_label_set_text_fmt(frequency_value, "%.1f MHz",
                              static_cast<double>(info.frequency_khz) / 1000.0);
    } else {
        lv_label_set_text(used_value, "--");
        lv_label_set_text(capacity_value, "--");
        lv_label_set_text(free_value, "--");
        lv_label_set_text(bus_value, "SDMMC 1-bit");
        lv_label_set_text(frequency_value, "--");
    }

    if (info_result != ESP_OK && info.state != STORAGE_SERVICE_STATE_ERROR) {
        ESP_UTILS_LOGW("Read SD information failed: %s", esp_err_to_name(info_result));
    }
}

void StoragePage::readInternalStorage(WorkerResult &result)
{
    // Flash metadata and NVS are queried on the existing SD worker, never on LVGL.
    uint32_t bytes = 0;
    if (esp_flash_get_physical_size(nullptr, &bytes) == ESP_OK) result.flash_bytes = bytes;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        result.app_partition_bytes = running->size;
        const esp_partition_pos_t partition = {running->address, running->size};
        esp_image_metadata_t metadata = {};
        if (esp_image_get_metadata(&partition, &metadata) == ESP_OK) result.app_image_bytes = metadata.image_len;
    }
    uint32_t last_end = 0;
    esp_partition_iterator_t item = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (item) {
        const esp_partition_t *part = esp_partition_get(item);
        const uint32_t end = part->address + part->size;
        if (end > last_end) last_end = end;
        item = esp_partition_next(item);
    }
    if (result.flash_bytes > last_end) result.unallocated_flash_bytes = result.flash_bytes - last_end;
    nvs_stats_t stats = {};
    if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
        result.nvs_valid = true;
        result.nvs_used = stats.used_entries;
        result.nvs_total = stats.total_entries;
    }
}

void StoragePage::refreshMemory()
{
    char value[48];
    if (internal_ram_value) {
        formatBytes(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), value, sizeof(value));
        lv_label_set_text(internal_ram_value, value);
    }
    if (psram_value) {
        formatBytes(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), value, sizeof(value));
        lv_label_set_text(psram_value, value);
    }
}

void StoragePage::applyWorkerResult(const WorkerResult &result)
{
    applyInfo(result.info, result.info_result);
    char value[64];
    auto bytes_label = [&](lv_obj_t *label, uint32_t bytes) {
        if (!label) return;
        if (bytes) { formatBytes(bytes, value, sizeof(value)); lv_label_set_text(label, value); }
        else lv_label_set_text(label, "--");
    };
    bytes_label(flash_value, result.flash_bytes);
    bytes_label(app_size_value, result.app_image_bytes);
    bytes_label(app_partition_value, result.app_partition_bytes);
    bytes_label(unallocated_value, result.unallocated_flash_bytes);
    if (nvs_value) {
        if (result.nvs_valid) lv_label_set_text_fmt(nvs_value, "%u / %u 项", (unsigned)result.nvs_used, (unsigned)result.nvs_total);
        else lv_label_set_text(nvs_value, "--");
    }


    char message[176] = {};
    if (result.operation_result != ESP_OK) {
        if ((result.command == WorkerCommand::Eject ||
             result.command == WorkerCommand::Refresh) &&
                result.operation_result == ESP_ERR_INVALID_STATE && result.info.active_leases > 0) {
            snprintf(message, sizeof(message), "请先在最近任务关闭媒体应用（%lu 个占用）",
                     static_cast<unsigned long>(result.info.active_leases));
        } else {
            snprintf(message, sizeof(message), "操作失败：%s",
                     esp_err_to_name(result.operation_result));
        }
    } else {
        switch (result.command) {
        case WorkerCommand::Refresh:
            strlcpy(message, "存储卡扫描完成", sizeof(message));
            break;
        case WorkerCommand::Benchmark:
            lv_label_set_text_fmt(write_value, "%.2f MiB/s", result.benchmark.write_mib_per_s);
            lv_label_set_text_fmt(read_value, "%.2f MiB/s", result.benchmark.read_mib_per_s);
            lv_label_set_text(crc_value,
                              result.benchmark.actual_crc32 == result.benchmark.expected_crc32
                                  ? "通过"
                                  : "不一致");
            snprintf(message, sizeof(message), "4 MiB 测试完成（%08lx）",
                     static_cast<unsigned long>(result.benchmark.actual_crc32));
            break;
        case WorkerCommand::Eject:
            strlcpy(message, "可以安全拔出存储卡", sizeof(message));
            break;
        case WorkerCommand::ExportDiagnostics:
            snprintf(message, sizeof(message), "已保存：%s", result.diagnostics_path);
            break;
        }
    }
    setBusy(false, message);
}

} // namespace esp_brookesia::apps
