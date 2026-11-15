#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hosino_console_auth_store.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "nvs.h"
/* All cookie values in this test are deliberately synthetic. */
static unsigned char disk[66];
static size_t disk_size;
static unsigned writes;
static esp_err_t write_error;
static void (*write_hook)(void), (*read_hook)(void);
static uint32_t cancel_ticket;
static bool create_fail_once;
static char cookie_a[65], cookie_b[65];
BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack,
    void *arg, unsigned priority, TaskHandle_t *out, unsigned caps) {
    (void)fn;(void)name;(void)arg;(void)priority;
    assert(stack == 4096 && caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if(create_fail_once){create_fail_once=false;return 0;}
    *out = (TaskHandle_t)1; return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t handle) { assert(handle); }
uint32_t ulTaskNotifyTake(BaseType_t clear, uint32_t wait) { (void)clear;(void)wait;return 0; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(!strcmp(name,"hs_auth"));assert(mode==NVS_READONLY||mode==NVS_READWRITE);*handle=1;return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size) {
    assert(handle==1&&!strcmp(key,"session"));
    if(!disk_size)return ESP_ERR_NVS_NOT_FOUND;
    if(*size<disk_size){*size=disk_size;return ESP_ERR_NVS_INVALID_LENGTH;}
    memcpy(data,disk,disk_size);*size=disk_size;
    if(read_hook){void(*hook)(void)=read_hook;read_hook=NULL;hook();}
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle,const char *key,const void *data,size_t size) {
    assert(handle==1&&!strcmp(key,"session")&&size==66);++writes;
    if(write_error)return write_error;
    memcpy(disk,data,size);disk_size=size;
    if(write_hook){void(*hook)(void)=write_hook;write_hook=NULL;hook();}
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle,const char *key){assert(handle==1&&!strcmp(key,"session"));++writes;if(write_error)return write_error;memset(disk,0,sizeof(disk));disk_size=0;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t handle){assert(handle==1);return ESP_OK;}
void nvs_close(nvs_handle_t handle){assert(handle==1);}
static hs_auth_store_status_t status(void){hs_auth_store_status_t s;hs_auth_store_status(&s);return s;}
static void forget_hook(void){hs_auth_store_forget(UINT32_MAX);}
static void cancel_hook(void){hs_auth_store_cancel_restore(cancel_ticket);}
static void expect_empty_restore(void){
    char out[65];uint32_t epoch;esp_err_t error;uint32_t t=hs_auth_store_restore();assert(t);
    assert(hs_auth_store_test_step());assert(hs_auth_store_take_restore(t,out,&epoch,&error));
    assert(error==ESP_OK&&!out[0]);assert(!hs_auth_store_take_restore(t,out,&epoch,&error));
}
int main(int argc,char **argv){
    memset(cookie_a,'a',64);memset(cookie_b,'b',64);
    if(argc>1){disk_size=66;disk[0]=1;disk[1]=1;memcpy(disk+2,cookie_a,64);if(!strcmp(argv[1],"corrupt"))disk[0]=9;}
    if(argc>1&&!strcmp(argv[1],"retry")){create_fail_once=true;assert(hs_auth_store_init()==ESP_ERR_NO_MEM);assert(status().error==ESP_ERR_NO_MEM);}
    assert(hs_auth_store_init()==ESP_OK);assert(!status().ready);
    if(argc>1&&!strcmp(argv[1],"startup-cancel"))assert(hs_auth_store_set_remember(false,NULL)==ESP_OK);
    hs_auth_store_test_boot();
    assert(status().ready);
    if(argc>1){
        if(!strcmp(argv[1],"startup-cancel")){assert(!status().remember);assert(hs_auth_store_test_step());assert(!disk_size);puts("PASS explicit startup opt-out wins over saved preference");return 0;}
        if(!strcmp(argv[1],"corrupt")){assert(!status().remember&&status().error==ESP_ERR_INVALID_RESPONSE);puts("PASS corrupt stored record fails closed");return 0;}
        assert(status().remember&&status().saved);char out[65];uint32_t ep;esp_err_t err;uint32_t t=hs_auth_store_restore();
        assert(hs_auth_store_test_step()&&hs_auth_store_take_restore(t,out,&ep,&err));assert(err==ESP_OK&&!strcmp(out,cookie_a));
        assert(writes==0);puts("PASS cold-start restore of a valid persisted record; no writes");return 0;
    }
    assert(!status().remember&&!status().saved&&!writes);
    assert(hs_auth_store_set_remember(true,NULL)==ESP_OK);assert(writes==0);assert(hs_auth_store_test_step());
    uint32_t old=status().epoch;assert(hs_auth_store_save(old,cookie_a)==ESP_OK);
    hs_auth_store_forget(UINT32_MAX);assert(hs_auth_store_save(old,cookie_b)==ESP_ERR_INVALID_STATE);
    assert(hs_auth_store_test_step());assert(!status().saved);expect_empty_restore();
    assert(hs_auth_store_save(status().epoch,"short")==ESP_ERR_INVALID_ARG);
    assert(hs_auth_store_save(status().epoch,cookie_a)==ESP_OK);assert(hs_auth_store_test_step());assert(status().saved);
    char out[65];uint32_t ep;esp_err_t err;uint32_t t=hs_auth_store_restore();assert(hs_auth_store_test_step());
    assert(hs_auth_store_take_restore(t,out,&ep,&err)&&err==ESP_OK&&!strcmp(out,cookie_a));
    assert(!hs_auth_store_take_restore(t,out,&ep,&err));
    /* Logout arrives while the NVS save is already in flight. */
    assert(hs_auth_store_save(status().epoch,cookie_b)==ESP_OK);write_hook=forget_hook;
    assert(hs_auth_store_test_step()&&status().busy);assert(hs_auth_store_test_step()&&!status().busy&&!status().saved);expect_empty_restore();
    assert(hs_auth_store_save(status().epoch,cookie_a)==ESP_OK);assert(hs_auth_store_test_step());
    /* An already completed restore cannot escape a subsequent clear. */
    t=hs_auth_store_restore();assert(hs_auth_store_test_step());hs_auth_store_forget(UINT32_MAX);
    assert(hs_auth_store_take_restore(t,out,&ep,&err)&&err==ESP_ERR_INVALID_STATE&&!out[0]);assert(hs_auth_store_test_step());
    assert(hs_auth_store_save(status().epoch,cookie_a)==ESP_OK);assert(hs_auth_store_test_step());
    t=hs_auth_store_restore();read_hook=forget_hook;assert(hs_auth_store_test_step());
    assert(hs_auth_store_take_restore(t,out,&ep,&err)&&err==ESP_ERR_INVALID_STATE&&!out[0]);assert(hs_auth_store_test_step());
    /* Cancel a read both before dispatch and during its NVS read. */
    t=hs_auth_store_restore();hs_auth_store_cancel_restore(t);assert(!hs_auth_store_test_step());assert(!hs_auth_store_take_restore(t,out,&ep,&err));
    cancel_ticket=hs_auth_store_restore();read_hook=cancel_hook;assert(hs_auth_store_test_step());assert(!hs_auth_store_take_restore(cancel_ticket,out,&ep,&err));
    old=status().epoch;assert(hs_auth_store_save(old,cookie_a)==ESP_OK);assert(hs_auth_store_set_remember(false,NULL)==ESP_OK);
    assert(hs_auth_store_save(old,cookie_b)==ESP_ERR_INVALID_STATE);assert(hs_auth_store_test_step());assert(!disk_size&&!status().remember&&!status().saved);
    write_error=ESP_FAIL;assert(hs_auth_store_set_remember(true,cookie_a)==ESP_OK);assert(hs_auth_store_test_step());assert(status().error==ESP_FAIL&&!status().busy);
    write_error=ESP_OK;assert(hs_auth_store_set_remember(true,cookie_a)==ESP_OK);assert(hs_auth_store_test_step());assert(status().error==ESP_OK&&status().saved);
    puts("PASS async mailbox; old-save rejection, queued/inflight logout, cold preference, restore cancellation, invalidation, write failure and retry");return 0;
}
