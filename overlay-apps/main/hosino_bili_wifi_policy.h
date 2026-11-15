/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "esp_wifi.h"
#include "esp_log.h"

/* Used only by the serialized Bili managed-network media adapter. No local
 * player or global WiFi setting acquires this policy. No allocation/task/ISR.
 * Own a change only after successful set; preserve a failed restore for retry. */
typedef struct {
    wifi_ps_type_t previous;
    bool owned;
} hosino_bili_wifi_policy_t;

static esp_err_t hosino_bili_wifi_enter(hosino_bili_wifi_policy_t *policy)
{
    wifi_ps_type_t current;
    esp_err_t result=esp_wifi_get_ps(&current);
    if (result!=ESP_OK) {
        ESP_LOGW("bili_wifi","STREAM_WIFI stage=read_before result=0x%x",(unsigned)result);
        return result;
    }
    if (policy->owned) {
        if (current==WIFI_PS_NONE) return ESP_OK;
        /* Re-entry after an external change must preserve that newer policy
         * as the value belonging to this new streaming acquisition. */
        policy->owned=false;
    }
    if (current==WIFI_PS_NONE) {
        ESP_LOGI("bili_wifi","STREAM_WIFI stage=already_none owned=0");
        return ESP_OK;
    }
    result=esp_wifi_set_ps(WIFI_PS_NONE);
    if (result==ESP_OK) {policy->previous=current;policy->owned=true;}
    ESP_LOGI("bili_wifi","STREAM_WIFI stage=enter previous=%d result=0x%x owned=%d",
             (int)current,(unsigned)result,(int)policy->owned);
    return result;
}

static esp_err_t hosino_bili_wifi_leave(hosino_bili_wifi_policy_t *policy)
{
    if (!policy->owned) return ESP_OK;
    wifi_ps_type_t current;
    esp_err_t result=esp_wifi_get_ps(&current);
    if (result!=ESP_OK) {
        ESP_LOGW("bili_wifi","STREAM_WIFI stage=read_restore result=0x%x pending=1",(unsigned)result);
        return result;
    }
    /* An independent owner has changed policy since entry. Respect that value
     * instead of overwriting it with our previously cached setting. */
    if (current!=WIFI_PS_NONE) {
        policy->owned=false;
        ESP_LOGI("bili_wifi","STREAM_WIFI stage=external_change current=%d restored=0 pending=0",(int)current);
        return ESP_OK;
    }
    result=esp_wifi_set_ps(policy->previous);
    if (result==ESP_OK) policy->owned=false;
    ESP_LOGI("bili_wifi","STREAM_WIFI stage=restore previous=%d result=0x%x pending=%d",
             (int)policy->previous,(unsigned)result,(int)policy->owned);
    return result;
}
