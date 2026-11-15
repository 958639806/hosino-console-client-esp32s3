#include "provision_parser.h"
#include "hosino_bili_config_store.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int internal_task,task_count,open_count,set_count,commit_count;
static int commit_fail,blob_fail,task_fail,exists;
static unsigned char saved_blob[456];
static size_t saved_size;
struct fake_semaphore {int count;};
SemaphoreHandle_t xSemaphoreCreateMutex(void) {SemaphoreHandle_t p=calloc(1,sizeof(*p));p->count=1;return p;}
SemaphoreHandle_t xSemaphoreCreateBinary(void) {return calloc(1,sizeof(struct fake_semaphore));}
BaseType_t xSemaphoreTake(SemaphoreHandle_t p,unsigned t) {(void)t;assert(p->count>0);--p->count;return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t p) {++p->count;return pdTRUE;}
void vSemaphoreDelete(SemaphoreHandle_t p) {free(p);}
void *heap_caps_calloc(size_t n,size_t z,unsigned caps) {assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));return calloc(n,z);}
void heap_caps_free(void *p) {free(p);}
BaseType_t xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned prio,void *handle,unsigned caps) {
    (void)handle;assert(!strcmp(name,"bili_nvs"));assert(stack==4096&&prio==2);
    assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(task_fail) return 0;
    ++task_count;internal_task=1;fn(arg);internal_task=0;return pdPASS;
}
void vTaskDeleteWithCaps(void *p) {(void)p;assert(internal_task);}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *out) {
    assert(internal_task);assert(!strcmp(ns,"hosino_bili"));assert(mode==NVS_READONLY||mode==NVS_READWRITE);
    ++open_count;*out=1;if(!exists&&mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t length) {
    assert(internal_task&&h==1);assert(!strcmp(key,"bridge_v1"));assert(length==sizeof(saved_blob));++set_count;
    if(blob_fail)return ESP_FAIL;
    /* NVS may persist a complete set before commit. This fake deliberately
     * models that behavior; every update must still contain a coherent pair. */
    memcpy(saved_blob,value,length);saved_size=length;exists=1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n) {
    assert(internal_task&&h==1);assert(!strcmp(key,"bridge_v1"));
    if(*n<saved_size)return ESP_ERR_INVALID_RESPONSE;
    memcpy(out,saved_blob,saved_size);*n=saved_size;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {assert(internal_task&&h==1);++commit_count;if(commit_fail)return ESP_FAIL;exists=1;return ESP_OK;}
void nvs_close(nvs_handle_t h) {assert(internal_task&&h==1);}
static void make_line(char *out,const char *base,const char *token) {sprintf(out,"BILI_CONFIG {\"base_url\":\"%s\",\"device_token\":\"%s\"}",base,token);}
int main(void) {
    const char *token="0123456789abcdefghijklmnopqrstuvwxyzABCDEFG";
    char line[1200];bili_provision_config_t config;
    assert(hosino_bili_config_load(&config)==ESP_OK&&!config.base[0]&&!config.token[0]);
    make_line(line,"https://101.42.20.91/bili///",token);
    assert(bili_provision_parse(line,&config)==BILI_PROVISION_OK);
    assert(!strcmp(config.base,"https://101.42.20.91/bili"));
    assert(bili_provision_apply(line)==BILI_PROVISION_OK);
    assert(set_count==1&&commit_count==1&&task_count==2);
    assert(!strcmp(bili_provision_ack(BILI_PROVISION_OK),"BILI_CONFIG_OK"));
    assert(hosino_bili_config_load(&config)==ESP_OK&&!strcmp(config.token,token));
    const int writes=set_count;
    const char *bad[]={"http://host/bili","https://user@host/bili","https://host/a/../b","https://host/%2e%2e/b","https://host/bili?x=1","https://host/bili#x"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {make_line(line,bad[i],token);assert(bili_provision_apply(line)==BILI_PROVISION_INVALID);}
    make_line(line,"https://host/bili","short");assert(bili_provision_apply(line)==BILI_PROVISION_INVALID);
    make_line(line,"https://host/bili","0123456789abcdefghijklmno\\r\\nInjected");assert(bili_provision_apply(line)==BILI_PROVISION_INVALID);
    make_line(line,"https://host/bili","0123456789abcdefghijklmno\\u0000suffix");assert(bili_provision_apply(line)==BILI_PROVISION_INVALID);
    assert(bili_provision_apply("BILI_CONFIG {\"base_url\":\"https://host\",\"base_url\":\"https://host\"}")==BILI_PROVISION_INVALID);
    assert(bili_provision_apply("OTHER_CONFIG secret")==BILI_PROVISION_IGNORE);
    assert(bili_provision_ack(BILI_PROVISION_IGNORE)==NULL&&set_count==writes);
    char request_id[17];
    sprintf(line,"BILI_CONFIG {\"request_id\":\"0123456789abcdef\",\"device_token\":\"%s\",\"base_url\":\"https://host/bili\"}",token);
    assert(bili_provision_parse_request(line,&config,request_id)==BILI_PROVISION_OK&&!strcmp(request_id,"0123456789abcdef"));
    assert(bili_provision_sync_id("BILI_SYNC 0123456789abcdef",request_id)&&!strcmp(request_id,"0123456789abcdef"));
    assert(!bili_provision_sync_id("BILI_SYNC 0123456789abcdeg",request_id)&&!request_id[0]);
    assert(!bili_provision_sync_id("BILI_SYNC 0123456789abcdef extra",request_id));
    sprintf(line,"BILI_CONFIG {\"request_id\":\"0123456789abcdeg\",\"device_token\":\"%s\",\"base_url\":\"https://host/bili\"}",token);
    assert(bili_provision_parse_request(line,&config,request_id)==BILI_PROVISION_INVALID&&!request_id[0]);
    sprintf(line,"BILI_CONFIG {\"request_id\":\"short\",\"device_token\":\"%s\",\"base_url\":\"https://host/bili\"}",token);
    assert(bili_provision_parse_request(line,&config,request_id)==BILI_PROVISION_INVALID&&!request_id[0]);
    sprintf(line,"BILI_CONFIG {\"request_id\":\"0123456789abcdef\",\"device_token\":\"%s\",\"base_url\":\"http://host/bili\"}",token);
    assert(bili_provision_parse_request(line,&config,request_id)==BILI_PROVISION_INVALID&&!strcmp(request_id,"0123456789abcdef"));
    bili_provision_line_t accumulator={0};
    const char *prefix="BILI_CONFIG ";for(const char *p=prefix;*p;++p)bili_provision_feed(&accumulator,*p);
    for(unsigned i=0;i<1000;++i)assert(bili_provision_feed(&accumulator,'x')==BILI_LINE_NONE);
    assert(bili_provision_feed(&accumulator,'\n')==BILI_LINE_TOO_LONG&&accumulator.length==0);
    make_line(line,"https://host/bili",token);
    for(const char *p=line;*p;++p)assert(bili_provision_feed(&accumulator,*p)==BILI_LINE_NONE);
    assert(bili_provision_feed(&accumulator,'\n')==BILI_LINE_READY);
    assert(bili_provision_parse(accumulator.bytes,&config)==BILI_PROVISION_OK);
    bili_provision_clear(&accumulator,sizeof(accumulator));
    make_line(line,"https://host/bili",token);
    const unsigned char original_version=saved_blob[0];
    saved_blob[0]=2;assert(hosino_bili_config_load(&config)==ESP_ERR_INVALID_RESPONSE&&!config.base[0]&&!config.token[0]);saved_blob[0]=original_version;
    saved_size=455;assert(hosino_bili_config_load(&config)==ESP_ERR_INVALID_RESPONSE&&!config.base[0]);saved_size=456;
    saved_size=457;assert(hosino_bili_config_load(&config)==ESP_ERR_INVALID_RESPONSE&&!config.base[0]);saved_size=456;
    saved_blob[1]=1;assert(hosino_bili_config_load(&config)==ESP_ERR_INVALID_RESPONSE);saved_blob[1]=0;
    unsigned char before[456];memcpy(before,saved_blob,456);
    saved_blob[4]='x';assert(hosino_bili_config_load(&config)==ESP_ERR_INVALID_RESPONSE&&!config.base[0]);memcpy(saved_blob,before,456);
    saved_blob[260]='\n';assert(hosino_bili_config_load(&config)==ESP_ERR_INVALID_RESPONSE&&!config.token[0]);memcpy(saved_blob,before,456);
    saved_blob[260+strlen(token)+1]='x';assert(hosino_bili_config_load(&config)==ESP_ERR_INVALID_RESPONSE);memcpy(saved_blob,before,456);
    memset(saved_blob+4,'a',256);assert(hosino_bili_config_load(&config)==ESP_ERR_INVALID_RESPONSE);memcpy(saved_blob,before,456);
    make_line(line,"https://new.example/bili","ZYXWVUTSRQPONMLKJIHGFEDCBA9876543210");
    blob_fail=1;assert(bili_provision_apply(line)==BILI_PROVISION_STORAGE);blob_fail=0;
    assert(!memcmp(saved_blob,before,456));assert(hosino_bili_config_load(&config)==ESP_OK&&!strcmp(config.token,token));
    commit_fail=1;assert(bili_provision_apply(line)==BILI_PROVISION_STORAGE);
    assert(!strcmp(bili_provision_ack(BILI_PROVISION_STORAGE),"BILI_CONFIG_ERROR_STORAGE"));commit_fail=0;
    assert(hosino_bili_config_load(&config)==ESP_OK&&!strcmp(config.base,"https://new.example/bili")&&!strcmp(config.token,"ZYXWVUTSRQPONMLKJIHGFEDCBA9876543210"));
    task_fail=1;const int opens=open_count;assert(bili_provision_apply(line)==BILI_PROVISION_STORAGE&&open_count==opens);
    puts("PROVISION_REAL_PARSER_STORE_NAMESPACE_INTERNAL_STACK_AND_FAILURE_CHECKS_PASS");return 0;
}
