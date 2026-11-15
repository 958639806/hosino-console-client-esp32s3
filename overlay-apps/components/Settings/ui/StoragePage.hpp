/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "storage_service.h"

namespace esp_brookesia::apps {

class StoragePage : public systems::phone::App {
public:
    static StoragePage *requestInstance(bool use_status_bar = false,
                                        bool use_navigation_bar = false);

    StoragePage(bool use_status_bar, bool use_navigation_bar);
    ~StoragePage() override;

    bool run() override;
    bool back() override;
    bool close() override;

private:
    enum class WorkerCommand : uint8_t {
        Refresh,
        Benchmark,
        Eject,
        ExportDiagnostics,
    };

    struct WorkerRequest {
        WorkerCommand command;
        uint32_t generation;
    };

    struct WorkerResult {
        StoragePage *page;
        WorkerCommand command;
        uint32_t generation;
        esp_err_t operation_result;
        esp_err_t info_result;
        storage_service_info_t info;
        storage_service_benchmark_t benchmark;
        char diagnostics_path[160];
        uint32_t flash_bytes;
        uint32_t app_partition_bytes;
        uint32_t app_image_bytes;
        uint32_t unallocated_flash_bytes;
        size_t nvs_used;
        size_t nvs_total;
        bool nvs_valid;
    };

    static StoragePage *_instance;

    static void actionEventCallback(lv_event_t *event);
    static void workerTask(void *argument);
    static void workerResultAsyncCallback(void *argument);

    bool ensureWorker();
    bool enqueue(WorkerCommand command);
    void createUi();
    void setBusy(bool busy, const char *message);
    void applyInfo(const storage_service_info_t &info, esp_err_t info_result);
    void applyWorkerResult(const WorkerResult &result);
    static void readInternalStorage(WorkerResult &result);
    void refreshMemory();
    void destroyUi();

    std::atomic_bool page_active{false};
    std::atomic<uint32_t> page_generation{0};
    QueueHandle_t worker_queue = nullptr;
    TaskHandle_t worker_task = nullptr;

    lv_obj_t *page_root = nullptr;
    lv_obj_t *list = nullptr;
    lv_obj_t *status_value = nullptr;
    lv_obj_t *card_value = nullptr;
    lv_obj_t *filesystem_value = nullptr;
    lv_obj_t *used_value = nullptr;
    lv_obj_t *flash_value = nullptr;
    lv_obj_t *app_size_value = nullptr;
    lv_obj_t *app_partition_value = nullptr;
    lv_obj_t *unallocated_value = nullptr;
    lv_obj_t *nvs_value = nullptr;
    lv_obj_t *internal_ram_value = nullptr;
    lv_obj_t *psram_value = nullptr;
    lv_timer_t *memory_timer = nullptr;
    lv_obj_t *capacity_value = nullptr;
    lv_obj_t *free_value = nullptr;
    lv_obj_t *bus_value = nullptr;
    lv_obj_t *frequency_value = nullptr;
    lv_obj_t *operation_value = nullptr;
    lv_obj_t *write_value = nullptr;
    lv_obj_t *read_value = nullptr;
    lv_obj_t *crc_value = nullptr;
    lv_obj_t *refresh_button = nullptr;
    lv_obj_t *benchmark_button = nullptr;
    lv_obj_t *export_button = nullptr;
    lv_obj_t *eject_button = nullptr;

    lv_style_t style_list = {};
    lv_style_t style_row = {};
    lv_style_t style_section = {};
    lv_style_t style_pressed = {};
};

} // namespace esp_brookesia::apps
