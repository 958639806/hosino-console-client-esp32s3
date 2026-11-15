#include "platform.h"
/* PRODUCTION_MODULE */
/* PRODUCTION_HTTP */

enum {HEALTHY,NXDOMAIN,NODATA,ALL_FAIL,HANG,CACHED};
static int mode;
static bool station_up,associated,allowed,core,native_active,queue_fail;
static uint32_t station_ip,gateway;
static esp_netif_dns_info_t servers[3];
static void (*queued)(void *);static void *queued_arg;
static dns_found_callback native_found;static void *native_arg;
static unsigned queries,sets,allocations,frees,open_count,write_count;
static int64_t clock_us,cancel_at;
static bool change_lease_after_failure;
static char logs[32768];static size_t log_used;
static unsigned lock_depth;
static esp_netif_t station;
typedef struct Allocation { struct Allocation *next;bool live;size_t size; } Allocation;
static Allocation *blocks;

void host_log(const char *tag,const char *format,...) {
    (void)tag;va_list args;va_start(args,format);
    int n=vsnprintf(logs+log_used,sizeof(logs)-log_used,format,args);va_end(args);
    assert(n>=0&&(size_t)n<sizeof(logs)-log_used);log_used+=(size_t)n;logs[log_used++]='\n';logs[log_used]=0;
}
void host_lock(void){assert(lock_depth++==0);}
void host_unlock(void){assert(lock_depth--==1);}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps) {
    assert((caps&(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    Allocation *a=calloc(1,sizeof(*a)+n*size);assert(a);a->live=true;a->size=n*size;a->next=blocks;blocks=a;allocations++;return a+1;
}
void heap_caps_free(void *p){if(!p)return;Allocation *a=((Allocation *)p)-1;assert(a->live);a->live=false;memset(p,0xdd,a->size);frees++;}
static void require_live(void *p){assert((((Allocation *)p)-1)->live);}
int64_t esp_timer_get_time(void){return clock_us;}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *name){assert(core&&!strcmp(name,"WIFI_STA_DEF"));return &station;}
bool esp_netif_is_netif_up(esp_netif_t *n){assert(core&&n==&station);return station_up;}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *a){assert(core);(void)a;return associated?ESP_OK:ESP_FAIL;}
esp_err_t esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *out){assert(core&&n==&station);memset(out,0,sizeof(*out));out->ip.addr=station_ip;out->gw.addr=gateway;return 0;}
esp_err_t esp_netif_get_dns_info(esp_netif_t *n,esp_netif_dns_type_t slot,esp_netif_dns_info_t *out){assert(core&&n==&station&&slot<3);*out=servers[slot];return 0;}
esp_err_t esp_netif_set_dns_info(esp_netif_t *n,esp_netif_dns_type_t slot,esp_netif_dns_info_t *value){assert(core&&n==&station&&slot<3);servers[slot]=*value;sets++;return 0;}
err_t tcpip_try_callback(void (*fn)(void *),void *arg){if(queue_fail)return ERR_MEM;assert(!queued);queued=fn;queued_arg=arg;return ERR_OK;}
err_t dns_gethostbyname_addrtype(const char *host,ip_addr_t *out,dns_found_callback fn,void *arg,u8_t type){
    assert(core&&!native_active&&type==LWIP_DNS_ADDRTYPE_IPV4&&!strcmp(host,"jiexi.ume.wiki"));queries++;
    if(mode==CACHED){out->addr=1;return ERR_OK;}
    native_active=true;native_found=fn;native_arg=arg;return ERR_INPROGRESS;
}
static void pump(void){
    if(queued){void(*fn)(void*)=queued;void *arg=queued_arg;queued=NULL;require_live(arg);core=true;fn(arg);core=false;return;}
    if(native_active&&mode!=HANG){
        bool success=mode==HEALTHY || (mode==NXDOMAIN&&public_dns(&servers[1].ip)) ||
                     (mode==NODATA&&public_dns(&servers[0].ip));
        ip_addr_t answer={1};require_live(native_arg);core=true;
        native_found("ignored",success?&answer:NULL,native_arg);
        /* Match pinned NODATA cleanup order: the old entry is only retired
         * AFTER found returns. A recursive second lookup would assert above. */
        native_active=false;core=false;
        if(!success&&change_lease_after_failure){station_ip++;change_lease_after_failure=false;}
    }
}
void vTaskDelay(unsigned ticks){
    clock_us+=ticks*1000;if(cancel_at>=0&&clock_us>=cancel_at)allowed=false;
    /* Let the worker observe cancellation before a queued (not running) tcpip
     * callback, then explicitly deliver that late callback in the test. */
    if(!allowed&&queued)return;
    pump();
}
static bool active(void *ctx){(void)ctx;return allowed;}
/* HTTP API boundary used by the actual production transfer helper. */
esp_err_t esp_http_client_open(esp_http_client_handle_t h,int n){(void)h;(void)n;open_count++;return 0;}
int esp_http_client_write(esp_http_client_handle_t h,const char *p,int n){(void)h;assert(!strncmp(p,"{}",n));write_count++;return n;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h){(void)h;return 2;}
int esp_http_client_get_status_code(esp_http_client_handle_t h){(void)h;return 200;}
int esp_http_client_read(esp_http_client_handle_t h,char *p,int n){(void)h;assert(n>=2);memcpy(p,"{}",2);return 2;}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h){(void)h;return true;}
static uint32_t main_address(void){return servers[0].ip.u_addr.ip4.addr;}
static void reset(void){
    assert(!native_active&&!queued&&!s_inflight&&!s_owner&&allocations==frees);
    while(blocks){Allocation *a=blocks;blocks=a->next;free(a);}
    station_up=associated=allowed=true;core=false;queue_fail=false;station_ip=ESP_IP4TOADDR(192,168,30,178);gateway=ESP_IP4TOADDR(192,168,30,1);
    memset(servers,0,sizeof(servers));servers[0].ip.u_addr.ip4.addr=gateway;
    queries=sets=allocations=frees=open_count=write_count=0;clock_us=0;cancel_at=-1;log_used=0;logs[0]=0;mode=HEALTHY;
    s_epoch_valid=s_recovered=false;s_last_recovery=0;memset(&s_epoch,0,sizeof(s_epoch));change_lease_after_failure=false;
}
static esp_err_t prepare(int64_t budget){return bili_dns_prepare("https://jiexi.ume.wiki/bili",clock_us+budget,active,NULL);}
static esp_err_t send_once(int64_t budget){
    int64_t deadline=clock_us+budget;esp_err_t rc=bili_dns_prepare("https://jiexi.ume.wiki/bili",deadline,active,NULL);
    if(rc==0){char b[2048];RequestIo io={0};transferResponse((void*)1,"{}",2,b,sizeof(b),deadline,active,NULL,&io);rc=io.result;}
    return rc;
}
int main(void){
    reset();assert(send_once(12000000)==0&&queries==1&&sets==2&&main_address()==gateway&&open_count==1&&write_count==1);
    assert(servers[1].ip.u_addr.ip4.addr==ESP_IP4TOADDR(223,5,5,5)&&servers[2].ip.u_addr.ip4.addr==ESP_IP4TOADDR(223,6,6,6));
    reset();mode=NXDOMAIN;assert(send_once(12000000)==0&&queries==1&&main_address()==gateway&&sets==2&&open_count==1);
    reset();mode=NODATA;assert(send_once(12000000)==0&&queries==2&&open_count==1&&write_count==1);
    assert(main_address()==ESP_IP4TOADDR(223,5,5,5)&&servers[1].ip.u_addr.ip4.addr==ESP_IP4TOADDR(223,6,6,6)&&servers[2].ip.u_addr.ip4.addr==gateway);
    assert(strstr(logs,"promoted=1")&&allocations==frees);
    reset();station_up=false;assert(send_once(12000000)==ESP_ERR_INVALID_STATE&&sets==0&&queries==0&&open_count==0);
    reset();associated=false;assert(prepare(12000000)==ESP_ERR_INVALID_STATE&&sets==0&&queries==0);
    reset();station_ip=0;assert(prepare(12000000)==ESP_ERR_INVALID_STATE&&sets==0&&queries==0);
    reset();gateway=0;assert(prepare(12000000)==ESP_ERR_INVALID_STATE&&sets==0&&queries==0);
    reset();mode=ALL_FAIL;servers[0].ip.u_addr.ip4.addr=ESP_IP4TOADDR(223,5,5,5);
    assert(send_once(12000000)==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME&&queries==1&&open_count==0);
    unsigned before=sets;assert(prepare(12000000)==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME&&queries==2&&sets==before);
    reset();mode=ALL_FAIL;assert(send_once(12000000)==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME&&queries==2&&open_count==0);
    reset();mode=HANG;assert(send_once(50000)==ESP_ERR_TIMEOUT&&native_active&&s_inflight&&allocations-frees==1&&open_count==0);
    before=queries;assert(prepare(50000)==ESP_ERR_INVALID_STATE&&queries==before);mode=ALL_FAIL;pump();assert(!s_inflight&&allocations==frees);
    /* Queue not yet executed when deadline/cancel happens: no later mutation. */
    reset();assert(send_once(1)==ESP_ERR_TIMEOUT&&sets==0&&queries==0&&allocations==frees&&open_count==0);
    reset();cancel_at=0;assert(send_once(12000000)==ESP_ERR_INVALID_STATE&&sets==0&&queries==0&&allocations-frees==1&&queued);
    pump();assert(allocations==frees&&!s_inflight);
    reset();mode=HANG;cancel_at=40000;assert(send_once(12000000)==ESP_ERR_INVALID_STATE&&native_active&&s_inflight&&open_count==0);
    mode=ALL_FAIL;pump();assert(!s_inflight&&allocations==frees);
    reset();queue_fail=true;assert(send_once(12000000)==ESP_ERR_NO_MEM&&sets==0&&queries==0&&allocations==frees&&open_count==0);
    reset();mode=NODATA;change_lease_after_failure=true;
    assert(prepare(12000000)==ESP_ERR_INVALID_STATE&&queries==1&&main_address()==gateway&&sets==2);
    /* Another owner restores router MAIN: don't flip it every failed request. */
    reset();mode=NODATA;assert(prepare(12000000)==0);servers[0].ip.u_addr.ip4.addr=gateway;before=sets;
    assert(prepare(12000000)==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME&&sets==before&&strstr(logs,"skipped=cooldown"));
    station_ip++;assert(prepare(12000000)==0&&sets>before);
    reset();mode=NODATA;servers[1].ip.u_addr.ip4.addr=ESP_IP4TOADDR(1,1,1,1);servers[2].ip.u_addr.ip4.addr=ESP_IP4TOADDR(8,8,8,8);
    assert(prepare(12000000)==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME&&sets==0&&queries==1&&main_address()==gateway);
    reset();mode=CACHED;assert(send_once(12000000)==0&&queries==1&&open_count==1&&allocations==frees);
    assert(!strstr(logs,"https://")&&!strstr(logs,"Bearer")&&!strstr(logs,"jiexi.ume.wiki"));
    reset();puts("PRODUCTION_DNS_PRIMARY_BACKUP_NODATA_RECOVERY_LIFETIME_COOLDOWN_SINGLE_POST_PASS");return 0;
}
