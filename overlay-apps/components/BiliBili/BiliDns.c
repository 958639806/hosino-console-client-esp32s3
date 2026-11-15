/* SPDX-License-Identifier: Apache-2.0 */
#include "BiliDns.h"
#include <stddef.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

#define RECOVERY_COOLDOWN_US 60000000LL
static const char *TAG="bili_dns";
typedef struct {
    uint32_t ip,gateway;
    esp_ip_addr_t main;
} DnsLease;
typedef struct Lookup {
    unsigned refs;
    bool abandoned,done,recover;
    esp_err_t result;
    int64_t deadline;
    char host[254];
    DnsLease lease;
} Lookup;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static Lookup *s_inflight;
static bool s_owner;
/* Only tcpip callbacks access recovery epoch fields. */
static DnsLease s_epoch;
static bool s_epoch_valid,s_recovered;
static int64_t s_last_recovery;

static bool address_equal(const esp_ip_addr_t *a,const esp_ip_addr_t *b) {
    if(a->type!=b->type) return false;
    return a->type==ESP_IPADDR_TYPE_V4 ? a->u_addr.ip4.addr==b->u_addr.ip4.addr :
        memcmp(a->u_addr.ip6.addr,b->u_addr.ip6.addr,sizeof(a->u_addr.ip6.addr))==0;
}
static bool address_empty(const esp_ip_addr_t *a) {
    static const uint32_t zero[4]={0};
    return a->type==ESP_IPADDR_TYPE_V4 ? !a->u_addr.ip4.addr :
        memcmp(a->u_addr.ip6.addr,zero,sizeof(zero))==0;
}
static bool public_dns(const esp_ip_addr_t *a) {
    return a->type==ESP_IPADDR_TYPE_V4 &&
        (a->u_addr.ip4.addr==ESP_IP4TOADDR(223,5,5,5) || a->u_addr.ip4.addr==ESP_IP4TOADDR(223,6,6,6));
}
static bool same_lease(const DnsLease *a,const DnsLease *b) {
    return a->ip==b->ip && a->gateway==b->gateway && address_equal(&a->main,&b->main);
}
static bool abandoned(Lookup *q) {
    portENTER_CRITICAL(&s_lock);bool value=q->abandoned;portEXIT_CRITICAL(&s_lock);return value;
}
static void release_lookup(Lookup *q) {
    portENTER_CRITICAL(&s_lock);bool destroy=--q->refs==0;portEXIT_CRITICAL(&s_lock);
    if(destroy) heap_caps_free(q);
}
static void finish_lookup(Lookup *q,esp_err_t result) {
    portENTER_CRITICAL(&s_lock);
    q->result=result;q->done=true;
    if(s_inflight==q) s_inflight=NULL;
    portEXIT_CRITICAL(&s_lock);
    release_lookup(q); /* The tcpip/native-DNS reference. */
}
static void found(const char *name,const ip_addr_t *ip,void *argument) {
    (void)name;
    Lookup *q=argument;
    finish_lookup(q,ip && IP_IS_V4(ip) && !ip_addr_isany(ip) ? ESP_OK : ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME);
    /* Do not start another lookup here: native NODATA cleanup occurs AFTER
     * this callback returns. The worker queues a separate tcpip callback. */
}
static void log_servers(esp_netif_t *netif,const char *stage) {
    for(unsigned i=0;i<3;i++) {
        esp_netif_dns_info_t value={0};
        if(esp_netif_get_dns_info(netif,(esp_netif_dns_type_t)i,&value)!=ESP_OK) {
            ESP_LOGW(TAG,"DNS_STATE stage=%s slot=%u unavailable=1",stage,i);continue;
        }
        if(value.ip.type==ESP_IPADDR_TYPE_V4)
            ESP_LOGI(TAG,"DNS_STATE stage=%s slot=%u address=" IPSTR,stage,i,IP2STR(&value.ip.u_addr.ip4));
        else ESP_LOGI(TAG,"DNS_STATE stage=%s slot=%u family=ipv6",stage,i);
    }
}
static bool connected_lease(esp_netif_t *netif,DnsLease *lease,esp_netif_dns_info_t dns[3]) {
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip={0};
    if(!netif || !esp_netif_is_netif_up(netif) || esp_wifi_sta_get_ap_info(&ap)!=ESP_OK ||
       esp_netif_get_ip_info(netif,&ip)!=ESP_OK || !ip.ip.addr || !ip.gw.addr) return false;
    for(unsigned i=0;i<3;i++) if(esp_netif_get_dns_info(netif,(esp_netif_dns_type_t)i,&dns[i])!=ESP_OK) return false;
    lease->ip=ip.ip.addr;lease->gateway=ip.gw.addr;lease->main=dns[0].ip;return true;
}
static void begin_lookup(void *argument) {
    Lookup *q=argument;
    if(abandoned(q) || esp_timer_get_time()>=q->deadline) { finish_lookup(q,ESP_ERR_TIMEOUT);return; }
    esp_netif_t *netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    DnsLease current={0};esp_netif_dns_info_t dns[3]={0};
    if(!connected_lease(netif,&current,dns)) {
        ESP_LOGW(TAG,"DNS_PREPARE unavailable=wifi_or_ip");finish_lookup(q,ESP_ERR_INVALID_STATE);return;
    }
    if(q->recover) {
        /* Never overwrite freshly renewed DHCP data or promote repeatedly while
         * a public resolver is already primary. Retain all nonzero servers. */
        if(!same_lease(&current,&q->lease)) { finish_lookup(q,ESP_ERR_INVALID_STATE);return; }
        if(public_dns(&dns[0].ip)) {
            ESP_LOGW(TAG,"DNS_RECOVERY skipped=already_public");
            finish_lookup(q,ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME);return;
        }
        if(s_recovered && esp_timer_get_time()-s_last_recovery<RECOVERY_COOLDOWN_US) {
            ESP_LOGW(TAG,"DNS_RECOVERY skipped=cooldown remaining_ms=%lld",
                (long long)((RECOVERY_COOLDOWN_US-(esp_timer_get_time()-s_last_recovery))/1000));
            finish_lookup(q,ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME);return;
        }
        esp_netif_dns_info_t ordered[3]={0};unsigned n=0;
        for(unsigned i=0;i<3;i++) if(public_dns(&dns[i].ip)) ordered[n++]=dns[i];
        if(!n) { ESP_LOGW(TAG,"DNS_RECOVERY skipped=no_preconfigured_public");finish_lookup(q,ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME);return; }
        for(unsigned i=0;i<3;i++) if(!public_dns(&dns[i].ip) && !address_empty(&dns[i].ip)) ordered[n++]=dns[i];
        if(abandoned(q)) { finish_lookup(q,ESP_ERR_INVALID_STATE);return; }
        for(unsigned i=0;i<3;i++) {
            if(esp_netif_set_dns_info(netif,(esp_netif_dns_type_t)i,&ordered[i])!=ESP_OK) {
                finish_lookup(q,ESP_FAIL);return;
            }
            dns[i]=ordered[i];
        }
        s_recovered=true;s_last_recovery=esp_timer_get_time();s_epoch=current;s_epoch.main=dns[0].ip;s_epoch_valid=true;
        ESP_LOGW(TAG,"DNS_RECOVERY promoted=1 cooldown_ms=60000");log_servers(netif,"promoted");
    } else {
        if(!s_epoch_valid || current.ip!=s_epoch.ip || current.gateway!=s_epoch.gateway) {
            s_epoch=current;s_epoch_valid=true;s_recovered=false;s_last_recovery=0;
        } else if(!address_equal(&current.main,&s_epoch.main)) {
            /* A DHCP refresh may replace MAIN with the router again. Update
             * its identity, but retain the short recovery cooldown so another
             * owner cannot make us flip the global resolver on every request. */
            s_epoch.main=current.main;
        }
        esp_netif_dns_info_t public_servers[2]={
            {.ip=ESP_IP4ADDR_INIT(223,5,5,5)},{.ip=ESP_IP4ADDR_INIT(223,6,6,6)}
        };
        for(unsigned i=1;i<3;i++) if(address_empty(&dns[i].ip)) {
            if(abandoned(q)) { finish_lookup(q,ESP_ERR_INVALID_STATE);return; }
            if(esp_netif_set_dns_info(netif,(esp_netif_dns_type_t)i,&public_servers[i-1])!=ESP_OK) {
                finish_lookup(q,ESP_FAIL);return;
            }
            dns[i]=public_servers[i-1];
        }
        q->lease=current;log_servers(netif,"initial");
    }
    if(abandoned(q) || esp_timer_get_time()>=q->deadline) { finish_lookup(q,ESP_ERR_TIMEOUT);return; }
    ip_addr_t address;
    const err_t result=dns_gethostbyname_addrtype(q->host,&address,found,q,LWIP_DNS_ADDRTYPE_IPV4);
    if(result==ERR_OK) finish_lookup(q,IP_IS_V4(&address)&&!ip_addr_isany(&address)?ESP_OK:ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME);
    else if(result!=ERR_INPROGRESS) finish_lookup(q,result==ERR_MEM?ESP_ERR_NO_MEM:ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME);
}
static esp_err_t one_lookup(const char *host,bool recover,DnsLease *lease,int64_t deadline,
                            bool (*active)(void *),void *context) {
    if(!active(context)) return ESP_ERR_INVALID_STATE;
    if(esp_timer_get_time()>=deadline) return ESP_ERR_TIMEOUT;
    Lookup *q=heap_caps_calloc(1,sizeof(*q),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!q) return ESP_ERR_NO_MEM;
    q->refs=2;q->recover=recover;q->lease=*lease;q->deadline=deadline;
    memcpy(q->host,host,strlen(host)+1);
    portENTER_CRITICAL(&s_lock);
    if(s_inflight) { portEXIT_CRITICAL(&s_lock);heap_caps_free(q);return ESP_ERR_INVALID_STATE; }
    s_inflight=q;portEXIT_CRITICAL(&s_lock);
    if(tcpip_try_callback(begin_lookup,q)!=ERR_OK) finish_lookup(q,ESP_ERR_NO_MEM);
    esp_err_t result;
    for(;;) {
        portENTER_CRITICAL(&s_lock);bool done=q->done;result=q->result;portEXIT_CRITICAL(&s_lock);
        if(done) { *lease=q->lease;break; }
        const bool running=active(context);
        if(!running || esp_timer_get_time()>=deadline) {
            result=running?ESP_ERR_TIMEOUT:ESP_ERR_INVALID_STATE;
            portENTER_CRITICAL(&s_lock);q->abandoned=true;portEXIT_CRITICAL(&s_lock);break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    release_lookup(q); /* Native callback retains its reference after timeout. */
    return result;
}
static bool configured_host(const char *base,char host[254]) {
    if(!base || strncmp(base,"https://",8)) return false;
    const char *start=base+8,*end=start;
    while(*end && *end!='/' && *end!=':') end++;
    const size_t n=(size_t)(end-start);
    if(!n || n>=254) return false;
    for(size_t i=0;i<n;i++) {
        const char c=start[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-')) return false;
    }
    memcpy(host,start,n);host[n]=0;return true;
}
esp_err_t bili_dns_prepare(const char *base,int64_t deadline,bool (*active)(void *),void *context) {
    char host[254];
    if(!active || !configured_host(base,host)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if(s_owner || s_inflight) { portEXIT_CRITICAL(&s_lock);ESP_LOGW(TAG,"DNS_PREPARE busy=inflight");return ESP_ERR_INVALID_STATE; }
    s_owner=true;portEXIT_CRITICAL(&s_lock);
    DnsLease lease={0};
    esp_err_t result=one_lookup(host,false,&lease,deadline,active,context);
    if(result==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME && active(context) && esp_timer_get_time()<deadline)
        result=one_lookup(host,true,&lease,deadline,active,context);
    portENTER_CRITICAL(&s_lock);s_owner=false;portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG,"DNS_PREPARE result=0x%x",(unsigned)result);
    return result;
}
