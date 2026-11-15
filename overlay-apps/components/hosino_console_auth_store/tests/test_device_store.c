#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hosino_console_auth_store.h"
#include "hosino_console_device_store.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "nvs.h"
/* Deliberately synthetic credentials; never print the record or token. */
enum { BLOB=167 };
static unsigned char disk[2][200], pending[200];
static size_t disk_size[2], pending_size;
static int staged_key=-1;
static unsigned calls, writes, commits, created;
static bool in_worker, fail_create, commit_applies;
static esp_err_t set_error, commit_error, read_error;
static void (*set_hook)(void), (*get_hook)(void), (*commit_hook)(void);
static uint32_t canceled, forget_ticket;
static hs_device_store_record_t a,b;
static int key_index(const char *key) { if(!strcmp(key,"session"))return 0; assert(!strcmp(key,"device_v1"));return 1; }
BaseType_t xTaskCreateWithCaps(void(*fn)(void*),const char*name,uint32_t stack,void*arg,unsigned priority,TaskHandle_t*out,unsigned caps){
 (void)fn;(void)arg;(void)priority;assert(!strcmp(name,"hs_auth_nvs"));assert(stack==4096&&caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
 if(fail_create){fail_create=false;return 0;}++created;*out=(TaskHandle_t)1;return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t t){assert(t);}
uint32_t ulTaskNotifyTake(BaseType_t c,uint32_t w){(void)c;(void)w;return 0;}
esp_err_t nvs_open(const char*n,int mode,nvs_handle_t*h){assert(in_worker&&!strcmp(n,"hs_auth"));assert(mode==0||mode==1);++calls;*h=1;staged_key=-1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*out,size_t*size){
 assert(in_worker&&h==1);int i=key_index(k);++calls;if(read_error)return read_error;
 if(!disk_size[i])return ESP_ERR_NVS_NOT_FOUND;if(*size<disk_size[i]){*size=disk_size[i];return ESP_ERR_NVS_INVALID_LENGTH;}
 memcpy(out,disk[i],disk_size[i]);*size=disk_size[i];if(get_hook){void(*f)(void)=get_hook;get_hook=NULL;f();}return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*data,size_t size){
 assert(in_worker&&h==1);int i=key_index(k);assert(size==(i?BLOB:66));++writes;if(set_error)return set_error;
 staged_key=i;pending_size=size;memcpy(pending,data,size);if(set_hook){void(*f)(void)=set_hook;set_hook=NULL;f();}return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h,const char*k){assert(in_worker&&h==1);++writes;if(set_error)return set_error;staged_key=key_index(k);pending_size=0;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(in_worker&&h==1);++commits;if(commit_hook){void(*f)(void)=commit_hook;commit_hook=NULL;f();}
 if((!commit_error||commit_applies)&&staged_key>=0){memset(disk[staged_key],0,200);memcpy(disk[staged_key],pending,pending_size);disk_size[staged_key]=pending_size;}return commit_error;}
void nvs_close(nvs_handle_t h){assert(in_worker&&h==1);staged_key=-1;memset(pending,0,sizeof(pending));pending_size=0;}
static hs_device_store_status_t status(void){hs_device_store_status_t s;hs_device_store_status(&s);return s;}
static bool step(void){in_worker=true;bool v=hs_device_store_test_step();in_worker=false;return v;}
static void boot(void){in_worker=true;hs_auth_store_test_boot();hs_device_store_test_boot();in_worker=false;}
static void take(uint32_t t,esp_err_t expected){hs_device_store_record_t out;uint32_t ep;esp_err_t e;memset(&out,0xa5,sizeof(out));assert(hs_device_store_take(t,&out,&ep,&e));assert(e==expected&&ep==status().epoch);assert(out.schema==1&&!out.token[0]&&!out.pair_id[0]);assert(!hs_device_store_take(t,&out,&ep,&e));}
static uint32_t save(const hs_device_store_record_t*r){uint32_t t=0;assert(hs_device_store_save(status().epoch,r,&t)==ESP_OK&&t);return t;}
static void expect_record(const hs_device_store_record_t*r){
 uint32_t t=0,ep=0;esp_err_t e=ESP_FAIL;hs_device_store_record_t out;
 assert(hs_device_store_load(status().epoch,&t)==ESP_OK&&t);assert(step());assert(hs_device_store_take(t,&out,&ep,&e));
 assert(e==ESP_OK&&ep==status().epoch&&out.schema==1);
 if(r){assert(!strcmp(r->token,out.token)&&!strcmp(r->pair_id,out.pair_id)&&r->pending_ack==out.pending_ack);}else assert(!out.token[0]&&!out.pair_id[0]&&!out.pending_ack);
 memset(&out,0,sizeof(out));
}
static void cancel_hook(void){hs_device_store_cancel(canceled);}
static void forget_hook(void){assert(hs_device_store_forget(status().epoch,&forget_ticket)==ESP_OK);}
static void write_disk(const hs_device_store_record_t*r){memset(disk[1],0,200);disk_size[1]=BLOB;disk[1][0]=1;disk[1][4]=r->pending_ack;memcpy(disk[1]+5,r->token,60);memcpy(disk[1]+134,r->pair_id,26);}
static void init_record(hs_device_store_record_t*r,char c){memset(r,0,sizeof(*r));r->schema=1;r->pending_ack=true;memcpy(r->token,"hsd_0123456789ab.",17);memset(r->token+17,c,43);memcpy(r->pair_id,"hsp_",4);memset(r->pair_id+4,c,22);}
static void rejected(hs_device_store_record_t*r){uint32_t t=123;unsigned before=calls;assert(hs_device_store_save(status().epoch,r,&t)==ESP_ERR_INVALID_ARG&&t==0);assert(calls==before&&!status().busy);}
int main(int argc,char**argv){
 init_record(&a,'a');init_record(&b,'B');
 if(argc>1&&!strcmp(argv[1],"retry")){fail_create=true;assert(hs_device_store_init()==ESP_ERR_NO_MEM);assert(!status().ready&&status().error==ESP_ERR_NO_MEM);}
 if(argc>1&&(!strcmp(argv[1],"restore")||!strcmp(argv[1],"corrupt")))write_disk(&a);
 if(argc>1&&!strcmp(argv[1],"corrupt"))disk[1][4]=2;
 assert(hs_device_store_init()==ESP_OK&&hs_auth_store_init()==ESP_OK&&created==1);assert(!status().ready);
 uint32_t t=0,old;
 if(argc>1&&!strcmp(argv[1],"startup-forget")){write_disk(&a);old=status().epoch;assert(hs_device_store_forget(old,&t)==ESP_OK&&t);boot();assert(status().busy);assert(step());take(t,ESP_OK);expect_record(NULL);assert(!disk_size[1]);puts("PASS startup forget supersedes stored device credential");return 0;}
 boot();
 if(argc>1&&!strcmp(argv[1],"corrupt")){assert(status().ready&&!status().busy&&!status().saved_known&&status().error==ESP_ERR_INVALID_RESPONSE&&!writes);puts("PASS corrupt cold device record fails closed without erase");return 0;}
 assert(status().ready&&!status().busy&&status().saved_known);
 if(argc>1&&!strcmp(argv[1],"restore")){assert(status().saved&&!writes);expect_record(&a);assert(!writes);puts("PASS cold-start pending ACK credential is restored without writes");return 0;}
 assert(!status().saved&&!writes);expect_record(NULL);
 t=save(&a);assert(status().busy&&!disk_size[1]);hs_device_store_record_t out;uint32_t ep;esp_err_t err;
 assert(!hs_device_store_take(t,&out,&ep,&err));uint32_t blocked=9;assert(hs_device_store_load(status().epoch,&blocked)==ESP_ERR_INVALID_STATE&&blocked==0);
 assert(step());assert(disk_size[1]==BLOB&&commits&&status().saved&&status().saved_known);take(t,ESP_OK);expect_record(&a);
 hs_device_store_record_t bad=a;bad.schema=2;rejected(&bad);bad=a;bad.token[4]='A';rejected(&bad);bad=a;bad.token[16]='-';rejected(&bad);bad=a;bad.token[30]='+';rejected(&bad);bad=a;bad.token[60]='x';rejected(&bad);bad=a;bad.pair_id[10]='/';rejected(&bad);bad=a;bad.pair_id[26]='x';rejected(&bad);bad=a;bad.token[18]=0;rejected(&bad);
 /* Caller tail bytes are normalized away, not copied into flash. */
 bad=a;memset(bad.token+61,0xa5,HS_DEVICE_TOKEN_CAP-61);t=save(&bad);assert(step());take(t,ESP_OK);expect_record(&a);for(unsigned i=66;i<134;++i)assert(!disk[1][i]);
 /* Cancel before dispatch prevents writes and invalidates the old epoch. */
 old=status().epoch;unsigned before=writes;t=save(&b);hs_device_store_cancel(t);assert(!step()&&writes==before);assert(hs_device_store_save(old,&a,&blocked)==ESP_ERR_INVALID_STATE);expect_record(&a);
 /* Cancel during NVS set cannot undo persistence, but cannot authorize ACK. */
 canceled=save(&b);set_hook=cancel_hook;assert(step());assert(!hs_device_store_take(canceled,&out,&ep,&err));assert(status().saved&&status().saved_known);expect_record(&b);
 /* Explicit forget serializes after the already executing save. */
 t=save(&a);set_hook=forget_hook;assert(step()&&status().busy);assert(!hs_device_store_take(t,&out,&ep,&err));assert(step());take(forget_ticket,ESP_OK);expect_record(NULL);
 /* A pending save is superseded without being written. */
 before=writes;t=save(&a);old=status().epoch;assert(hs_device_store_forget(old,&forget_ticket)==ESP_OK);assert(step());assert(writes==before+1);take(forget_ticket,ESP_OK);assert(!hs_device_store_take(t,&out,&ep,&err));assert(hs_device_store_load(old,&blocked)==ESP_ERR_INVALID_STATE);
 t=save(&a);assert(step());take(t,ESP_OK);
 /* Cancellation and forget both prevent a stale loaded secret escaping. */
 assert(hs_device_store_load(status().epoch,&canceled)==ESP_OK);get_hook=cancel_hook;assert(step());assert(!hs_device_store_take(canceled,&out,&ep,&err));expect_record(&a);
 assert(hs_device_store_load(status().epoch,&t)==ESP_OK);get_hook=forget_hook;assert(step());assert(!hs_device_store_take(t,&out,&ep,&err));assert(step());take(forget_ticket,ESP_OK);
 t=save(&a);assert(step());take(t,ESP_OK);
 assert(hs_device_store_load(status().epoch,&canceled)==ESP_OK);assert(step());hs_device_store_cancel(canceled);assert(!hs_device_store_take(canceled,&out,&ep,&err));expect_record(&a);
 /* Errors never erase on their own, and failed commit cannot authorize ACK. */
 commit_error=ESP_FAIL;t=save(&b);assert(step());take(t,ESP_FAIL);assert(status().saved&&status().saved_known);commit_error=ESP_OK;expect_record(&a);
 commit_error=ESP_FAIL;commit_applies=true;t=save(&b);assert(step());take(t,ESP_FAIL);assert(status().saved&&status().saved_known);commit_error=ESP_OK;commit_applies=false;expect_record(&b);
 set_error=ESP_FAIL;read_error=ESP_FAIL;t=save(&a);assert(step());take(t,ESP_FAIL);assert(!status().saved_known);set_error=read_error=ESP_OK;expect_record(&b);assert(status().saved_known);
 set_error=ESP_FAIL;assert(hs_device_store_forget(status().epoch,&t)==ESP_OK);assert(step());take(t,ESP_FAIL);set_error=ESP_OK;expect_record(&b);
 /* Mark ACK complete only by another durable atomic save. */
 b.pending_ack=false;t=save(&b);assert(step());take(t,ESP_OK);expect_record(&b);
 /* Cookie opt-out, cookie logout and device operations are independent keys. */
 assert(hs_auth_store_set_remember(false,NULL)==ESP_OK);in_worker=true;assert(hs_auth_store_test_step());in_worker=false;expect_record(&b);
 char cookie[65]={0};memset(cookie,'c',64);assert(hs_auth_store_set_remember(true,cookie)==ESP_OK);in_worker=true;assert(hs_auth_store_test_step());in_worker=false;
 unsigned char cookie_disk[66];memcpy(cookie_disk,disk[0],66);assert(hs_device_store_forget(UINT32_MAX,&t)==ESP_OK);assert(step());take(t,ESP_OK);assert(disk_size[0]==66&&!memcmp(cookie_disk,disk[0],66));expect_record(NULL);
 /* Strict stored blob validation, no silent erase or exposure of invalid data. */
 for(unsigned n=0;n<8;++n){write_disk(&a);if(n==0)disk[1][0]=2;if(n==1)disk[1][4]=2;if(n==2)disk[1][65]='x';if(n==3)disk[1][160]='x';if(n==4)disk[1][70]='x';if(n==5)disk_size[1]=BLOB-1;if(n==6)disk_size[1]=BLOB+1;if(n==7)disk[1][22]='+';
  before=writes;assert(hs_device_store_load(status().epoch,&t)==ESP_OK);assert(step());take(t,ESP_ERR_INVALID_RESPONSE);assert(writes==before&&!status().saved_known);}
 puts("PASS device async ticket/epoch/commit, cancellation, cold restore, pending ACK, errors, strict formats, independent cookie and one internal worker");return 0;
}
