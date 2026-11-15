#ifndef HOSINO_TEST_SYNC_STUBS_H
#define HOSINO_TEST_SYNC_STUBS_H
#include <stdbool.h>
#include <stdint.h>
#define CONFIG_ESP_PLAYER_ENABLE_AUDIO 0
typedef int esp_player_err_t;
enum { ESP_PLAYER_ERR_OK=0, ESP_PLAYER_ERR_INVALID_ARG=-1, ESP_PLAYER_ERR_NO_MEM=-2, ESP_PLAYER_ERR_TIMEOUT=-3, ESP_PLAYER_ERR_FAIL=-4 };
typedef enum { ESP_PLAYER_SYNC_MODE_AUDIO, ESP_PLAYER_SYNC_MODE_VIDEO,
               ESP_PLAYER_SYNC_MODE_SYSTEM, ESP_PLAYER_SYNC_MODE_NONE,
               ESP_PLAYER_SYNC_MODE_MAX } esp_player_sync_mode_t;
typedef struct { bool _is_stop; uint32_t task_status; } esp_player_stream_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdTICKS_TO_MS(t) (t)
#define pdMS_TO_TICKS(t) ((uint32_t)(t))
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
extern int64_t hosino_test_now;
static inline int64_t esp_timer_get_time(void) { return hosino_test_now; }
static inline TickType_t xTaskGetTickCount(void) { return (TickType_t)(hosino_test_now/1000); }
static inline void vTaskDelay(TickType_t t) { hosino_test_now += (int64_t)t*1000; }
static inline void player_set_events(void *ctx,uint32_t bits) { (void)ctx;(void)bits; }
static inline void player_clear_events(void *ctx,uint32_t bits) { (void)ctx;(void)bits; }
static inline int player_wait_events(void *ctx,uint32_t bits,uint32_t timeout)
{ (void)ctx;(void)bits;(void)timeout;return ESP_PLAYER_ERR_OK; }
#endif
