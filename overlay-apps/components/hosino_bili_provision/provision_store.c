/* SPDX-License-Identifier: Apache-2.0 */
#include "provision_parser.h"
#include "hosino_bili_config_store.h"
bili_provision_result_t bili_provision_apply(const char *line)
{
    char request_id[17];
    return bili_provision_apply_request(line,request_id);
}
bili_provision_result_t bili_provision_apply_request(const char *line,char request_id[17])
{
    bili_provision_config_t config;
    const bili_provision_result_t parsed=bili_provision_parse_request(line,&config,request_id);
    if(parsed!=BILI_PROVISION_OK) return parsed;
    const esp_err_t err=hosino_bili_config_save(&config);
    bili_provision_clear(&config,sizeof(config));
    return err==ESP_OK?BILI_PROVISION_OK:BILI_PROVISION_STORAGE;
}
