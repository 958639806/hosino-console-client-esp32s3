/* SPDX-License-Identifier: Apache-2.0 */
#include "hosino_bili_config_store.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <string.h>

/* All byte fields: fixed layout, no compiler-dependent padding. Updating one
 * NVS blob keeps its origin and token in the same complete record. */
typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    char base[256];
    char token[193];
    uint8_t reserved_tail[3];
} record_t;
typedef char bili_record_size_must_be_456[(sizeof(record_t)==456)?1:-1];
static bool zero_bytes(const void *memory,size_t length)
{
    const unsigned char *p=memory;
    for(size_t i=0;i<length;++i) if(p[i]) return false;
    return true;
}
static bool canonical_string(const char *text,size_t capacity)
{
    size_t used=0;while(used<capacity&&text[used]) ++used;
    return used<capacity&&zero_bytes(text+used,capacity-used);
}

typedef struct {
    bool save;
    bili_provision_config_t config;
    esp_err_t result;
    SemaphoreHandle_t done;
} request_t;
static portMUX_TYPE s_mux=portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_serial;

/* Only this function reaches NVS, exclusively from the internal-stack worker. */
static esp_err_t access_nvs(bool save,bili_provision_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err=nvs_open(BILI_PROVISION_NAMESPACE,save?NVS_READWRITE:NVS_READONLY,&handle);
    if(!save&&err==ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if(err!=ESP_OK) return err;
    record_t record={0};
    if(save) {
        record.version=1;
        memcpy(record.base,config->base,strlen(config->base)+1);
        memcpy(record.token,config->token,strlen(config->token)+1);
        err=nvs_set_blob(handle,BILI_PROVISION_BLOB_KEY,&record,sizeof(record));
        if(err==ESP_OK) err=nvs_commit(handle);
    } else {
        size_t n=sizeof(record);
        err=nvs_get_blob(handle,BILI_PROVISION_BLOB_KEY,&record,&n);
        if(err==ESP_OK) {
            if(n!=sizeof(record)||record.version!=1||!zero_bytes(record.reserved,sizeof(record.reserved))||
               !zero_bytes(record.reserved_tail,sizeof(record.reserved_tail))||
               !canonical_string(record.base,sizeof(record.base))||!canonical_string(record.token,sizeof(record.token))) err=ESP_ERR_INVALID_RESPONSE;
            else {
                memcpy(config->base,record.base,sizeof(config->base));
                memcpy(config->token,record.token,sizeof(config->token));
                if(!bili_provision_validate_config(config)) err=ESP_ERR_INVALID_RESPONSE;
            }
        }
        if(err!=ESP_OK) bili_provision_clear(config,sizeof(*config));
        if(err==ESP_ERR_NVS_NOT_FOUND) err=ESP_OK;
    }
    bili_provision_clear(&record,sizeof(record));nvs_close(handle);return err;
}
static void store_task(void *argument)
{
    request_t *request=argument;
    bili_provision_config_t config=request->config; /* INTERNAL stack source during flash writes. */
    const esp_err_t result=access_nvs(request->save,&config);
    request->config=config;request->result=result;
    bili_provision_clear(&config,sizeof(config));
    const SemaphoreHandle_t done=request->done;
    xSemaphoreGive(done); /* No request/semaphore access after publishing completion. */
    vTaskDeleteWithCaps(NULL);
}
static SemaphoreHandle_t serial_mutex(void)
{
    portENTER_CRITICAL(&s_mux);SemaphoreHandle_t mutex=s_serial;portEXIT_CRITICAL(&s_mux);
    if(mutex) return mutex;
    SemaphoreHandle_t created=xSemaphoreCreateMutex();
    if(!created) return NULL;
    portENTER_CRITICAL(&s_mux);
    if(!s_serial) {s_serial=created;created=NULL;}
    mutex=s_serial;
    portEXIT_CRITICAL(&s_mux);
    if(created) vSemaphoreDelete(created);
    return mutex;
}
static esp_err_t operation(bool save,bili_provision_config_t *config)
{
    if(!config) return ESP_ERR_INVALID_ARG;
    if(save&&!bili_provision_validate_config(config)) return ESP_ERR_INVALID_ARG;
    SemaphoreHandle_t serial=serial_mutex();
    if(!serial) return ESP_ERR_NO_MEM;
    if(xSemaphoreTake(serial,portMAX_DELAY)!=pdTRUE) return ESP_FAIL;
    request_t *request=heap_caps_calloc(1,sizeof(*request),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    esp_err_t result=ESP_ERR_NO_MEM;
    if(request) {
        request->save=save;if(save) request->config=*config;
        request->done=xSemaphoreCreateBinary();
        if(request->done&&xTaskCreateWithCaps(store_task,"bili_nvs",4096,request,2,NULL,
                MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)==pdPASS) {
            xSemaphoreTake(request->done,portMAX_DELAY);
            result=request->result;
            if(!save) *config=request->config;
        }
        if(request->done) vSemaphoreDelete(request->done);
        bili_provision_clear(request,sizeof(*request));heap_caps_free(request);
    }
    xSemaphoreGive(serial);return result;
}
esp_err_t hosino_bili_config_load(bili_provision_config_t *config)
{
    if(config) bili_provision_clear(config,sizeof(*config));
    return operation(false,config);
}
esp_err_t hosino_bili_config_save(const bili_provision_config_t *config)
{
    if(!config) return ESP_ERR_INVALID_ARG;
    bili_provision_config_t copy=*config;
    const esp_err_t result=operation(true,&copy);
    bili_provision_clear(&copy,sizeof(copy));return result;
}
