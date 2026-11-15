/* Original test fixtures; exclusive creation, never overwrite card contents. */
#include "hosino_media_samples.h"
#include "storage_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const uint8_t motion_start[] asm("_binary_generated_motion480_avi_start");
extern const uint8_t motion_end[] asm("_binary_generated_motion480_avi_end");
extern const uint8_t tone_start[] asm("_binary_generated_tone_flac_start");
extern const uint8_t tone_end[] asm("_binary_generated_tone_flac_end");
extern const uint8_t tone24_start[] asm("_binary_generated_tone24_flac_start");
extern const uint8_t tone24_end[] asm("_binary_generated_tone24_flac_end");
static const char *TAG = "media_samples";

static void install_sample(const char *name, const uint8_t *data, size_t size)
{
    char path[160];
    snprintf(path, sizeof(path), STORAGE_SERVICE_MOUNT_POINT "/hosino_samples/%s", name);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        ESP_LOGI(TAG, "%s: %s", name, errno == EEXIST ? "already exists, preserved" : "cannot create");
        return;
    }
    size_t done = 0;
    bool okay = true;
    while (done < size) {
        size_t count = size - done;
        if (count > 4096) count = 4096;
        const ssize_t written = write(fd, data + done, count);
        if (written <= 0) { okay = false; break; }
        done += (size_t)written;
    }
    if (okay && fsync(fd) != 0) okay = false;
    if (close(fd) != 0) okay = false;
    // Verify exact bytes through the actual card/file-system read path.
    uint8_t *verify = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    fd = okay && verify ? open(path, O_RDONLY) : -1;
    if (fd < 0) okay = false;
    done = 0;
    while (okay && done < size) {
        size_t count = size - done;
        if (count > 4096) count = 4096;
        const ssize_t read_bytes = read(fd, verify, count);
        if (read_bytes <= 0 || memcmp(verify, data + done, (size_t)read_bytes) != 0) { okay = false; break; }
        done += (size_t)read_bytes;
    }
    if (fd >= 0) close(fd);
    heap_caps_free(verify);
    bool removed = false;
    if (!okay) removed = unlink(path) == 0; // Only the file exclusively created by this invocation.
    ESP_LOGI(TAG, "%s: %s bytes=%u", name, okay ? "installed and verified" :
        (removed ? "failed, partial removed" : "failed, could not remove partial"), (unsigned)size);
}

static void install_task(void *arg)
{
    (void)arg;
    storage_service_lease_t lease = {0};
    esp_err_t result = storage_service_acquire(&lease);
    if (result == ESP_OK) {
        if (mkdir(STORAGE_SERVICE_MOUNT_POINT "/hosino_samples", 0700) == 0 || errno == EEXIST) {
            install_sample("generated-motion480.avi", motion_start, motion_end - motion_start);
            install_sample("generated-tone.flac", tone_start, tone_end - tone_start);
            install_sample("generated-tone24.flac", tone24_start, tone24_end - tone24_start);
        }
        storage_service_release(&lease);
    } else {
        ESP_LOGI(TAG, "No mounted card; sample install skipped: %s", esp_err_to_name(result));
    }
    vTaskDeleteWithCaps(NULL);
}

void hosino_media_samples_start(void)
{
    if (xTaskCreatePinnedToCoreWithCaps(install_task, "media_samples", 8192, NULL,
        2, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) ESP_LOGW(TAG, "Sample installer could not start");
}
