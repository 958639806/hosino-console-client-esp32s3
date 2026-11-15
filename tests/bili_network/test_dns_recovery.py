"""Compile the entire production DNS module plus actual HTTP transfer helper.

Deterministic tcpip/native-DNS/WiFi boundary, including late callbacks. Native
RCODE/NODATA behavior is modeled from pinned lwIP; not real radio/DNS acceptance.
"""
from pathlib import Path
import hashlib,json,re,subprocess,tempfile

ROOT=Path(__file__).resolve().parents[2]
TCC=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
PLATFORM=r'''
#ifndef TEST_PLATFORM_H
#define TEST_PLATFORM_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
typedef int esp_err_t;typedef int err_t;typedef unsigned char u8_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=0x103,ESP_ERR_INVALID_ARG=0x102,ESP_ERR_INVALID_SIZE=0x104,
 ESP_ERR_TIMEOUT=0x107,ESP_ERR_NO_MEM=0x101,ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME=0x8001,ESP_ERR_HTTP_EAGAIN=0x7007,
 ERR_OK=0,ERR_MEM=-1,ERR_INPROGRESS=-5,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,ESP_IPADDR_TYPE_V4=0,ESP_IPADDR_TYPE_V6=6,LWIP_DNS_ADDRTYPE_IPV4=0};
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void host_lock(void);void host_unlock(void);
#define portENTER_CRITICAL(x) host_lock()
#define portEXIT_CRITICAL(x) host_unlock()
#define pdMS_TO_TICKS(x) (x)
void host_log(const char *,const char *,...);
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_IP4TOADDR(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
#define ESP_IP4ADDR_INIT(a,b,c,d) {.u_addr={.ip4={.addr=ESP_IP4TOADDR(a,b,c,d)}},.type=0}
typedef struct {uint32_t addr;} Ip4;
typedef struct {uint32_t addr[4];} Ip6;
typedef struct {union {Ip4 ip4;Ip6 ip6;}u_addr;unsigned char type;} esp_ip_addr_t;
typedef struct {esp_ip_addr_t ip;} esp_netif_dns_info_t;
typedef struct {Ip4 ip,gw,netmask;} esp_netif_ip_info_t;
typedef struct {int unused;} esp_netif_t;
typedef struct {int unused;} wifi_ap_record_t;
typedef int esp_netif_dns_type_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(x) ((x)->addr&255),(((x)->addr>>8)&255),(((x)->addr>>16)&255),(((x)->addr>>24)&255)
typedef struct {uint32_t addr;} ip_addr_t;
#define IP_IS_V4(x) true
#define ip_addr_isany(x) ((x)->addr==0)
typedef void (*dns_found_callback)(const char*,const ip_addr_t*,void*);
void *heap_caps_calloc(size_t,size_t,unsigned);void heap_caps_free(void*);int64_t esp_timer_get_time(void);void vTaskDelay(unsigned);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char*);bool esp_netif_is_netif_up(esp_netif_t*);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*);esp_err_t esp_netif_get_ip_info(esp_netif_t*,esp_netif_ip_info_t*);
esp_err_t esp_netif_get_dns_info(esp_netif_t*,esp_netif_dns_type_t,esp_netif_dns_info_t*);
esp_err_t esp_netif_set_dns_info(esp_netif_t*,esp_netif_dns_type_t,esp_netif_dns_info_t*);
err_t tcpip_try_callback(void(*)(void*),void*);
err_t dns_gethostbyname_addrtype(const char*,ip_addr_t*,dns_found_callback,void*,u8_t);
typedef void *esp_http_client_handle_t;
esp_err_t esp_http_client_open(esp_http_client_handle_t,int);int esp_http_client_write(esp_http_client_handle_t,const char*,int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char*,int);bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
#endif
'''
def main():
    module=(ROOT/'overlay-apps/components/BiliBili/BiliDns.c').read_text('utf-8')
    client=(ROOT/'overlay-apps/components/BiliBili/BiliClient.cpp').read_text('utf-8')
    pump=client.split('// HOST_HTTP_BOUNDARY_BEGIN',1)[1].split('// HOST_HTTP_BOUNDARY_END',1)[0].split('\n',1)[1]
    code=(ROOT/'tests/bili_network/test_dns_recovery.c').read_text('utf-8').replace('/* PRODUCTION_MODULE */',module).replace('/* PRODUCTION_HTTP */',pump)
    assert client.index('trace.result=bili_dns_prepare(')<client.index('if(attemptedHttp) transferResponse(')
    assert 'cfg.url=url' in client and 'getaddrinfo(' not in module and 'dns_clear_cache(' not in module
    with tempfile.TemporaryDirectory(prefix='bili_dns_recovery_') as temp:
        work=Path(temp);(work/'platform.h').write_text(PLATFORM,'utf-8')
        for header in re.findall(r'#include "([^"]+)"',module):
            target=work/header;target.parent.mkdir(exist_ok=True,parents=True);target.write_text('#include "platform.h"\n','utf-8')
        (work/'BiliDns.h').write_text((ROOT/'overlay-apps/components/BiliBili/BiliDns.h').read_text('utf-8'),'utf-8')
        (work/'esp_err.h').write_text('#include "platform.h"\n','utf-8');(work/'test.c').write_text(code,'utf-8')
        build=subprocess.run([str(TCC),'-Wall','-I.','test.c','-o','test.exe'],cwd=work,capture_output=True,text=True)
        run=None if build.returncode else subprocess.run([str(work/'test.exe')],cwd=work,capture_output=True,text=True,timeout=10)
    report={'scope':'Entire production DNS module + actual HTTP transfer helper; deterministic platform/native API boundary, no real DNS/radio acceptance',
       'module_sha256':hashlib.sha256(module.encode()).hexdigest(),'client_sha256':hashlib.sha256(client.encode()).hexdigest(),
       'compile':{'exit_code':build.returncode,'stdout':build.stdout,'stderr':build.stderr},
       'run':None if run is None else {'exit_code':run.returncode,'stdout':run.stdout,'stderr':run.stderr},
       'passed':build.returncode==0 and run is not None and run.returncode==0}
    target=ROOT/'evidence/bili-dns-recovery-20261115/host.json';target.parent.mkdir(parents=True,exist_ok=True);target.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
    print(json.dumps(report,ensure_ascii=False,indent=2));return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
