// SPDX-License-Identifier: Apache-2.0
#include "BiliClient.hpp"
#include "BiliDns.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_tls_errors.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "lwip/tcpip.h"
#include "hosino_bili_config_store.h"
#include "hosino_network_policy.h"

namespace esp_brookesia::apps::bili {
namespace {
constexpr size_t ResponseLimit=48*1024;
constexpr const char *RequestTag="bili_request";
const char *requestRoute(const char *path) {
    struct Route { const char *path; const char *name; };
    static constexpr Route routes[]={
        {"/v1/feed","feed"},{"/v1/search","search"},{"/v1/video","detail"},
        {"/v1/me","me"},{"/v1/folders","folders"},{"/v1/favorites","favorites"},
        {"/v1/favorite","favorite_write"},{"/v1/auth/qr","qr"},
        {"/v1/auth/logout","logout"},{"/v1/playbacks","playback"}
    };
    for(const auto &route:routes) {
        const size_t n=strlen(route.path);
        if(!strncmp(path,route.path,n)&&(!path[n]||path[n]=='?')) return route.name;
    }
    return "other";
}
// HOST_HTTP_BOUNDARY_BEGIN: exercised verbatim with a deterministic HTTP client.
typedef struct RequestIo {
    int status;
    esp_err_t result;
    int64_t declared;
    size_t used;
    bool complete;
    const char *phase;
    unsigned header_waits,body_waits;
} RequestIo;
static bool requestBudget(RequestIo *io,bool (*active)(void *),void *context,int64_t deadline) {
    if(!active(context)) { io->result=ESP_ERR_INVALID_STATE; return false; }
    if(esp_timer_get_time()>=deadline) { io->result=ESP_ERR_TIMEOUT; return false; }
    return true;
}
static void transferResponse(esp_http_client_handle_t http,const char *body,size_t length,
        char *buffer,size_t capacity,int64_t deadline,bool (*active)(void *),void *context,RequestIo *io) {
    size_t sent=0;
    io->phase="open";
    if(!requestBudget(io,active,context,deadline)) return;
    io->result=esp_http_client_open(http,(int)length);
    if(io->result!=ESP_OK) return;
    while(sent<length) {
        io->phase="write";
        if(!requestBudget(io,active,context,deadline)) return;
        const int n=esp_http_client_write(http,body+sent,(int)(length-sent));
        if(n<=0 || (size_t)n>length-sent) { io->result=ESP_FAIL; return; }
        sent+=(size_t)n;
    }
    io->phase="headers";
    for(;;) {
        if(!requestBudget(io,active,context,deadline)) return;
        io->declared=esp_http_client_fetch_headers(http);
        if(io->declared==-ESP_ERR_HTTP_EAGAIN) { ++io->header_waits; continue; }
        if(io->declared<0) { io->result=ESP_FAIL; return; }
        io->status=esp_http_client_get_status_code(http);
        if(io->declared>(int64_t)capacity) { io->result=ESP_ERR_INVALID_SIZE; return; }
        if(io->status<200 || io->status>=600 || (io->status>=300 && io->status<400)) {
            io->result=ESP_FAIL; return;
        }
        break;
    }
    io->phase="body";
    for(;;) {
        if(!requestBudget(io,active,context,deadline)) return;
        if(io->used==capacity) { io->result=ESP_ERR_INVALID_SIZE; return; }
        const size_t left=capacity-io->used;
        const int wanted=(int)(left<2048?left:2048);
        const int n=esp_http_client_read(http,buffer+io->used,wanted);
        if(n==-ESP_ERR_HTTP_EAGAIN) { ++io->body_waits; continue; }
        if(n<0 || n>wanted) { io->result=ESP_FAIL; return; }
        io->used+=(size_t)n;
        /* Do not require another read that can time out after the final bytes.
         * Fetch-headers may cache body bytes: therefore this check belongs AFTER
         * the first body read, never before it. Production capacity/read size
         * cover the HTTP client's <=2048-byte header/body cache. */
        io->complete=esp_http_client_is_complete_data_received(http);
        if(io->complete) {
            if(requestBudget(io,active,context,deadline)) io->result=ESP_OK;
            return;
        }
        if(n==0) { io->result=ESP_FAIL; io->phase="body_truncated"; return; }
    }
}
// HOST_HTTP_BOUNDARY_END
struct RequestTrace : RequestIo {
    const Worker &worker;
    const char *route;
    esp_http_client_method_t method;
    bool cleanup;
    bool json=false,api_ok=false;
    int64_t started=esp_timer_get_time(),deadline_us=0;
    RequestTrace(const Worker &w,const char *path,esp_http_client_method_t m,bool c):
        worker(w),route(requestRoute(path)),method(m),cleanup(c) {
        status=0;result=ESP_FAIL;declared=-1;used=0;complete=false;phase="initial";header_waits=body_waits=0;
    }
    ~RequestTrace() {
        const int64_t now=esp_timer_get_time();
        const char *verb=method==HTTP_METHOD_GET?"GET":method==HTTP_METHOD_POST?"POST":method==HTTP_METHOD_DELETE?"DELETE":"OTHER";
        const bool cancelled=!cleanup&&(worker.stop.load()||!worker.foreground.load());
        ESP_LOGI(RequestTag,"API route=%s method=%s phase=%s status=%d result=0x%x declared=%lld used=%u complete=%d json=%d api_ok=%d elapsed_ms=%lld deadline=%d cancel=%d cleanup=%d header_waits=%u body_waits=%u",
            route,verb,phase,status,static_cast<unsigned>(result),static_cast<long long>(declared),static_cast<unsigned>(used),
            complete,json,api_ok,static_cast<long long>((now-started)/1000),deadline_us&&now>=deadline_us,cancelled,cleanup,header_waits,body_waits);
    }
};
void text(char *to, size_t cap, const char *from) { if(cap) strlcpy(to,from?from:"",cap); }
const char *str(cJSON *object,const char *key) { auto *v=cJSON_GetObjectItemCaseSensitive(object,key); return cJSON_IsString(v)?v->valuestring:""; }
double number(cJSON *object,const char *key) { auto *v=cJSON_GetObjectItemCaseSensitive(object,key); return cJSON_IsNumber(v)&&std::isfinite(v->valuedouble)?v->valuedouble:0; }
bool flag(cJSON *object,const char *key) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object,key)); }
// HOST_ERROR_BOUNDARY_BEGIN: actual backend error formatting and safe log codes.
static const char *bridgeErrorField(const cJSON *object,const char *name) {
    const cJSON *item=cJSON_GetObjectItemCaseSensitive(object,name);
    return cJSON_IsString(item)&&item->valuestring?item->valuestring:"";
}
static void formatBridgeError(char *output,size_t capacity,const cJSON *error,const char *fallback) {
    const char *code=bridgeErrorField(error,"code"),*message=bridgeErrorField(error,"message");
    if(cJSON_IsString(error)&&error->valuestring&&error->valuestring[0]) snprintf(output,capacity,"%s",error->valuestring);
    else if(code[0]&&message[0]) snprintf(output,capacity,"%s: %s",code,message);
    else if(message[0]) snprintf(output,capacity,"%s",message);
    else if(code[0]) snprintf(output,capacity,"%s",code);
    else snprintf(output,capacity,"%s",fallback);
}
static const char *bridgeLogCode(const cJSON *error) {
    const char *code=bridgeErrorField(error,"code");
    static const char *known[]={"invalid_request","unauthorized","login_required","playback_busy",
        "playback_stopped","playback_not_found","playback_prepare","cid_mismatch","source_transfer",
        "source_codec_unavailable","source_host_rejected","source_missing","source_drain_timeout",
        "previous_source_drain_timeout","sample_failed","sample_timeout","sample_output_limit",
        "sample_media_errors","sample_incomplete","sample_timestamps","sample_duration",
        "prepare_timeout","bitrate_unattainable","upstream_network","upstream_cooldown","upstream_json",
        "upstream_too_large","transcode_failed","curl_impersonation","wbi_key_invalid"};
    for(size_t i=0;i<sizeof(known)/sizeof(known[0]);++i) if(!strcmp(code,known[i])) return known[i];
    /* Families are fixed literals too: never log an arbitrary server string. */
    if(!strncmp(code,"upstream_http_",14)) return "upstream_http";
    if(!strncmp(code,"upstream_code_",14)) return "upstream_code";
    return "server_error";
}
static int preparationOutcome(const char *status,bool expired) {
    if(!strcmp(status,"ready")) return 1;
    if(!strcmp(status,"error")||!strcmp(status,"failed")) return 2;
    return expired?3:0;
}
static void preparationErrorText(char *output,size_t capacity,const cJSON *error,int outcome) {
    if(outcome==3) snprintf(output,capacity,"等待服务器准备超过75秒，请重试");
    else formatBridgeError(output,capacity,error,"服务器播放准备失败，请重试");
}
// HOST_ERROR_BOUNDARY_END
void networkSnapshot(void *) {
    // Run in the tcpip task: public esp_netif getters then execute directly,
    // without a synchronous IPC wait in either the GUI or control worker.
    auto *netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!netif) { ESP_LOGW(RequestTag,"NETDIAG sta_missing=1"); return; }
    esp_netif_ip_info_t ip{};
    esp_netif_dhcp_status_t dhcp=ESP_NETIF_DHCP_STATUS_MAX;
    const esp_err_t ipResult=esp_netif_get_ip_info(netif,&ip);
    const esp_err_t dhcpResult=esp_netif_dhcpc_get_status(netif,&dhcp);
    ESP_LOGW(RequestTag,"NETDIAG sta_up=%d ip_rc=0x%x ip=" IPSTR " gateway=" IPSTR " dhcp_rc=0x%x dhcp=%d",
        esp_netif_is_netif_up(netif),static_cast<unsigned>(ipResult),IP2STR(&ip.ip),IP2STR(&ip.gw),
        static_cast<unsigned>(dhcpResult),static_cast<int>(dhcp));
    const esp_netif_dns_type_t types[]={ESP_NETIF_DNS_MAIN,ESP_NETIF_DNS_BACKUP,ESP_NETIF_DNS_FALLBACK};
    const char *names[]={"main","backup","fallback"};
    for(unsigned i=0;i<3;++i) {
        esp_netif_dns_info_t dns{};
        const esp_err_t result=esp_netif_get_dns_info(netif,types[i],&dns);
        char address[48]="unavailable";
        if(result==ESP_OK) {
            if(dns.ip.type==ESP_IPADDR_TYPE_V4) snprintf(address,sizeof(address),IPSTR,IP2STR(&dns.ip.u_addr.ip4));
            else if(dns.ip.type==ESP_IPADDR_TYPE_V6) snprintf(address,sizeof(address),IPV6STR,IPV62STR(dns.ip.u_addr.ip6));
        }
        ESP_LOGW(RequestTag,"NETDIAG dns_%s rc=0x%x address=%s",names[i],static_cast<unsigned>(result),address);
    }
}
void reportNetworkFailure(esp_err_t tlsError,int tlsCode,int tlsFlags,int status) {
    // Only scalar errors and local interface state enter this diagnostic.
    // Do not pass a request URL, credentials, QR data or Worker lifetime here.
    ESP_LOGW(RequestTag,"request_failed http=%d tls=0x%x tls_code=%d tls_flags=0x%x",
        status,static_cast<unsigned>(tlsError),tlsCode,static_cast<unsigned>(tlsFlags));
    if(tcpip_try_callback(networkSnapshot,nullptr)!=ERR_OK)
        ESP_LOGW(RequestTag,"NETDIAG skipped=queue_unavailable");
}
bool validBase(const char *base) {
    if(!base||strlen(base)>240) return false;
    char normalized[256],probe[320]; text(normalized,sizeof(normalized),base);
    while(strlen(normalized)>8&&normalized[strlen(normalized)-1]=='/') normalized[strlen(normalized)-1]=0;
    snprintf(probe,sizeof(probe),"%s/v1/streams/probe",normalized);
    return hosino_network_url_allowed(probe,normalized);
}
void commit(Worker &w,Model &m) {
    if(xSemaphoreTake(w.mutex,portMAX_DELAY)==pdTRUE) { ++m.revision; w.model=m; xSemaphoreGive(w.mutex); }
}
void readVideo(Video &v,cJSON *data) {
    text(v.bvid,sizeof(v.bvid),str(data,"bvid")); text(v.title,sizeof(v.title),str(data,"title"));
    text(v.author,sizeof(v.author),str(data,"author")); v.cid=static_cast<uint64_t>(number(data,"cid")); v.duration=number(data,"duration");
}
void loadConfig(Worker &w,Model &m) {
    bili_provision_config_t config{};
    if(hosino_bili_config_load(&config)==ESP_OK) { text(m.base,sizeof(m.base),config.base);text(w.token,sizeof(w.token),config.token); }
    bili_provision_clear(&config,sizeof(config));
    m.token_present=w.token[0]; m.configured=validBase(m.base)&&hosino_network_token_valid(w.token); m.ready=true;
}
bool saveConfig(Worker &w,Model &m,const Command &cmd) {
    if(!validBase(cmd.text)) { text(m.error,sizeof(m.error),"地址须为HTTPS，不能包含用户信息、查询串或重定向地址"); return false; }
    const char *token=cmd.token[0]?cmd.token:w.token;
    if(strlen(token)<24||strlen(token)>192) { text(m.error,sizeof(m.error),"设备令牌须为24至192字符；不是B站密码"); return false; }
    for(const unsigned char *p=reinterpret_cast<const unsigned char *>(token);*p;++p)
        if(*p<33||*p>126) { text(m.error,sizeof(m.error),"设备令牌须为无空格的ASCII字符"); return false; }
    char base[256]; text(base,sizeof(base),cmd.text);
    while(strlen(base)>8&&base[strlen(base)-1]=='/') base[strlen(base)-1]=0;
    bili_provision_config_t config{};text(config.base,sizeof(config.base),base);text(config.token,sizeof(config.token),token);
    const esp_err_t result=hosino_bili_config_save(&config);bili_provision_clear(&config,sizeof(config));
    if(result!=ESP_OK) { text(m.error,sizeof(m.error),"保存连接设置失败，请重试"); return false; }
    text(m.base,sizeof(m.base),base);
    text(w.token,sizeof(w.token),token); m.token_present=true; m.configured=true;
    return true;
}
void encode(const char *input,char *output,size_t cap) {
    static const char hex[]="0123456789ABCDEF"; size_t n=0;
    for(const unsigned char *p=reinterpret_cast<const unsigned char *>(input);*p&&n+4<cap;++p) {
        if((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("-._~",*p)) output[n++]=*p;
        else { output[n++]='%'; output[n++]=hex[*p>>4]; output[n++]=hex[*p&15]; }
    }
    output[n]=0;
}
struct RequestActivity { const Worker *worker; bool cleanup; };
bool requestActive(void *context) {
    const auto &activity=*static_cast<RequestActivity *>(context);
    return activity.cleanup||(!activity.worker->stop.load()&&activity.worker->foreground.load());
}
cJSON *request(Worker &w,Model &m,const char *path,esp_http_client_method_t method=HTTP_METHOD_GET,const char *body=nullptr,bool cleanup=false) {
    RequestTrace trace(w,path,method,cleanup);
    RequestActivity activity{&w,cleanup};
    const auto active=[&](){return requestActive(&activity);};
    if(!active()) return nullptr;
    if(!m.configured) { trace.result=ESP_ERR_INVALID_STATE; text(m.error,sizeof(m.error),"请先配置HTTPS桥接地址和设备令牌"); return nullptr; }
    char url[1400],auth[288];
    if(snprintf(url,sizeof(url),"%s%s",m.base,path)>=static_cast<int>(sizeof(url))) { trace.result=ESP_ERR_INVALID_SIZE; text(m.error,sizeof(m.error),"请求路径过长"); return nullptr; }
    snprintf(auth,sizeof(auth),"Bearer %s",w.token);
    esp_http_client_config_t cfg{};
    cfg.url=url; cfg.method=method; cfg.timeout_ms=1800; cfg.buffer_size=2048; cfg.buffer_size_tx=1024;
    cfg.crt_bundle_attach=esp_crt_bundle_attach; cfg.disable_auto_redirect=true;
    auto http=esp_http_client_init(&cfg);
    if(!http) { trace.result=ESP_ERR_NO_MEM; text(m.error,sizeof(m.error),"网络内存不足，请先关闭其他应用"); return nullptr; }
    auto *buffer=static_cast<char *>(heap_caps_malloc(ResponseLimit+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!buffer) { trace.result=ESP_ERR_NO_MEM; esp_http_client_cleanup(http); text(m.error,sizeof(m.error),"响应缓冲区不足"); return nullptr; }
    esp_http_client_set_header(http,"Authorization",auth);
    esp_http_client_set_header(http,"Accept","application/json");
    esp_http_client_set_header(http,"Accept-Encoding","identity");
    if(body) esp_http_client_set_header(http,"Content-Type","application/json");
    const int64_t deadline=esp_timer_get_time()+(cleanup?4:12)*1000000LL;
    trace.deadline_us=deadline;
    const size_t length=body?strlen(body):0;
    trace.phase="dns";
    trace.result=bili_dns_prepare(m.base,deadline,requestActive,&activity);
    const bool attemptedHttp=trace.result==ESP_OK;
    if(attemptedHttp) transferResponse(http,body,length,buffer,ResponseLimit,deadline,requestActive,&activity,&trace);
    const size_t used=trace.used;
    const int status=trace.status;
    // The error handle belongs to this client and disappears during cleanup.
    // The return value is the ESP-TLS failure; out args are backend/verify data.
    int tlsCode=0,tlsFlags=0;
    const esp_err_t tlsError=attemptedHttp?esp_http_client_get_and_clear_last_tls_error(http,&tlsCode,&tlsFlags):trace.result;
    esp_http_client_close(http); esp_http_client_cleanup(http);
    memset(auth,0,sizeof(auth)); buffer[used]=0;
    cJSON *root=nullptr;
    if(trace.result==ESP_OK&&trace.complete&&active()) { trace.phase="json"; root=cJSON_ParseWithLength(buffer,used); }
    trace.json=root!=nullptr;
    heap_caps_free(buffer);
    if(!root) {
        if(!w.stop.load()&&w.foreground.load()) {
            if(tlsError==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME)
                text(m.error,sizeof(m.error),"域名解析失败，请检查网络或稍后重试");
            else if(!attemptedHttp&&trace.result==ESP_ERR_INVALID_STATE)
                text(m.error,sizeof(m.error),"网络尚未就绪或DNS解析仍在进行，请稍后刷新");
            else if(!attemptedHttp&&trace.result==ESP_ERR_TIMEOUT)
                text(m.error,sizeof(m.error),"域名解析等待超时，请稍后重试");
            else snprintf(m.error,sizeof(m.error),"网络请求失败 HTTP %d（检查Wi-Fi、证书、令牌和服务端）",status);
            if(!cleanup) reportNetworkFailure(tlsError,tlsCode,tlsFlags,status);
        }
        return nullptr;
    }
    trace.api_ok=flag(root,"ok");
    if(!trace.api_ok) {
        trace.phase="api_error";
        auto *error=cJSON_GetObjectItemCaseSensitive(root,"error");
        formatBridgeError(m.error,sizeof(m.error),error,"服务端拒绝请求");
        ESP_LOGW(RequestTag,"API_ERROR route=%s code=%s",trace.route,bridgeLogCode(error));
        cJSON_Delete(root); return nullptr;
    }
    if(status<200||status>=300) { snprintf(m.error,sizeof(m.error),"服务端返回HTTP %d",status); cJSON_Delete(root); return nullptr; }
    trace.phase="done";
    return root;
}
void stopMedia(Worker &w,Model &m,bool deleteSession=true) {
    w.frames.store(false);
    if(w.media_active&&w.hooks.stop) {
        const esp_err_t e=w.hooks.stop(w.hooks.context);
        if(e!=ESP_OK) { text(m.error,sizeof(m.error),"播放器尚未完成停止，请等待后重试"); return; }
    }
    w.media_active=false; m.playing=false; m.paused=false;
    if(deleteSession&&m.session_id[0]) {
        char path[192]; snprintf(path,sizeof(path),"/v1/playbacks?session_id=%s",m.session_id);
        cJSON *root=request(w,m,path,HTTP_METHOD_DELETE,nullptr,true); cJSON_Delete(root);
    }
    m.session_id[0]=0; m.media_state=HOSINO_MEDIA_STOPPED;
}
// HOST_PREPARE_DIAGNOSTICS_BEGIN
static int prepareNumber(const cJSON *object,const char *key) {
    const cJSON *item=cJSON_GetObjectItemCaseSensitive(object,key);
    return cJSON_IsNumber(item)&&item->valuedouble>=0&&item->valuedouble<=1000000 ? (int)item->valuedouble:-1;
}
static bool playbackIdentityMatches(const char *requested,const char *selected) {
    return requested&&selected&&requested[0]&&!strcmp(requested,selected);
}
static void logPreparation(const cJSON *data,const char *status,int profile,int requested) {
    const cJSON *diagnostics=cJSON_GetObjectItemCaseSensitive(data,"diagnostics");
    const cJSON *attempts=cJSON_GetObjectItemCaseSensitive(diagnostics,"sample_attempts");
    const int count=cJSON_IsArray(attempts)?cJSON_GetArraySize(attempts):0;
    const cJSON *last=count>0&&count<=8?cJSON_GetArrayItem(attempts,count-1):nullptr;
    ESP_LOGI(RequestTag,"PREPARE_PROFILE status=%s profile=%s requested_kbps=%d target_kbps=%d sample_kbps=%d quality=%d attempts=%d last_quality=%d",
        status,profile?"clear":"standard",requested,prepareNumber(data,"target_video_kbps"),prepareNumber(data,"sample_video_kbps"),
        prepareNumber(data,"selected_quality"),count<=8?count:-1,prepareNumber(last,"quality"));
}
// HOST_PREPARE_DIAGNOSTICS_END
struct Pending { char stream_path[512]{}; bool active=false; int64_t next=0,deadline=0; };
bool startMedia(Worker &w,Model &m,Pending &pending) {
    if(!w.hooks.open||!w.hooks.stop||!w.hooks.snapshot) { text(m.error,sizeof(m.error),"播放器适配器尚未安装"); return false; }
    if(pending.stream_path[0]!='/'||pending.stream_path[1]=='/'||strpbrk(pending.stream_path,"?#\\\r\n")||strstr(pending.stream_path,"..")) {
        text(m.error,sizeof(m.error),"服务端返回了不安全的播放路径"); return false;
    }
    char url[800];
    if(snprintf(url,sizeof(url),"%s%s",m.base,pending.stream_path)>=static_cast<int>(sizeof(url))) return false;
    BiliPlaybackRequest r{url,m.base,w.token,m.session_id,m.source_start,m.duration,
        static_cast<uint16_t>(m.profile?480:320),static_cast<uint16_t>(m.profile?270:180),static_cast<uint16_t>(m.profile?15:20)};
    w.frames.store(true); w.media_active=true;
    const esp_err_t error=w.hooks.open(w.hooks.context,r,Worker::frame,&w);
    if(error!=ESP_OK) {
        w.frames.store(false);
        if(w.hooks.stop&&w.hooks.stop(w.hooks.context)==ESP_OK) w.media_active=false;
        snprintf(m.error,sizeof(m.error),"打开媒体失败：%s",esp_err_to_name(error)); return false;
    }
    if(w.hooks.volume) (void)w.hooks.volume(w.hooks.context,m.volume);
    w.media_active=true; m.playing=true; m.paused=false; m.media_state=HOSINO_MEDIA_PLAYING;
    return true;
}
void beginPlayback(Worker &w,Model &m,Pending &pending,double position) {
    pending.active=false; stopMedia(w,m);
    if(w.media_active||w.stop.load()||!w.foreground.load()) return;
    if(!m.selected.bvid[0]||!m.selected.cid) { text(m.error,sizeof(m.error),"请先选择一个视频及分P"); return; }
    char body[320];
    snprintf(body,sizeof(body),"{\"bvid\":\"%s\",\"cid\":%llu,\"profile\":\"%s\",\"video_kbps\":%d,\"position_seconds\":%.3f}",
        m.selected.bvid,static_cast<unsigned long long>(m.selected.cid),m.profile?"clear":"standard",m.bitrate,std::max(0.0,position));
    cJSON *root=request(w,m,"/v1/playbacks",HTTP_METHOD_POST,body);
    if(!root) return;
    auto *data=cJSON_GetObjectItemCaseSensitive(root,"data");
    text(m.session_id,sizeof(m.session_id),str(data,"session_id"));
    text(pending.stream_path,sizeof(pending.stream_path),str(data,"stream_path"));
    m.source_start=number(data,"position_seconds"); m.position=m.source_start; m.duration=number(data,"duration");
    if(!m.duration) m.duration=m.selected.duration;
    if(!m.session_id[0]||!pending.stream_path[0]) text(m.error,sizeof(m.error),"播放会话响应不完整");
    else if(preparationOutcome(str(data,"status"),false)==1) {logPreparation(data,"ready",m.profile,m.bitrate);startMedia(w,m,pending);}
    else if(preparationOutcome(str(data,"status"),false)==2) {
        logPreparation(data,"error",m.profile,m.bitrate);
        auto *error=cJSON_GetObjectItemCaseSensitive(data,"error");
        preparationErrorText(m.error,sizeof(m.error),error,2);m.media_state=HOSINO_MEDIA_ERROR;
        ESP_LOGW(RequestTag,"PREPARE status=error code=%s",bridgeLogCode(error));
    }
    else { pending.active=true; pending.next=esp_timer_get_time()+500000; pending.deadline=esp_timer_get_time()+75*1000000LL; m.media_state=HOSINO_MEDIA_OPENING; }
    cJSON_Delete(root);
}
void pollPlayback(Worker &w,Model &m,Pending &pending) {
    pending.next=esp_timer_get_time()+1000000;
    char path[192]; snprintf(path,sizeof(path),"/v1/playbacks?session_id=%s",m.session_id);
    cJSON *root=request(w,m,path);
    if(!root) { pending.active=false; m.media_state=HOSINO_MEDIA_ERROR; return; }
    auto *data=cJSON_GetObjectItemCaseSensitive(root,"data");
    const char *status=str(data,"status");
    const int outcome=preparationOutcome(status,esp_timer_get_time()>pending.deadline);
    if(outcome==1) { pending.active=false;logPreparation(data,"ready",m.profile,m.bitrate);startMedia(w,m,pending); }
    else if(outcome==2||outcome==3) {
        logPreparation(data,outcome==3?"client_deadline":"error",m.profile,m.bitrate);
        pending.active=false;
        auto *error=cJSON_GetObjectItemCaseSensitive(data,"error");
        preparationErrorText(m.error,sizeof(m.error),error,outcome);
        ESP_LOGW(RequestTag,"PREPARE status=%s code=%s",outcome==3?"client_deadline":!strcmp(status,"failed")?"failed":"error",
            outcome==3?"client_prepare_deadline":bridgeLogCode(error));
        m.media_state=HOSINO_MEDIA_ERROR;
    }
    cJSON_Delete(root);
}
void execute(Worker &w,Model &m,Pending &pending,const Command &cmd) {
    m.error[0]=0;
    if(cmd.op==Op::Configure) { pending.active=false; stopMedia(w,m); if(saveConfig(w,m,cmd)) text(m.error,sizeof(m.error),"设置已保存，返回后刷新首页"); return; }
    if(cmd.op==Op::Stop) { pending.active=false; stopMedia(w,m); return; }
    if(cmd.op==Op::Pause) {
        if(m.paused) {
            const double target=m.duration>0?std::min(m.position,std::max(0.0,m.duration-0.1)):m.position;
            beginPlayback(w,m,pending,target);
        } else if(w.media_active||pending.active) {
            // Keeping a paused HTTP socket eventually blocks the server's
            // writer timeout. Save source time, stop both sides, and resume
            // through a fresh transcode session rather than a dead socket.
            const double target=m.position;
            pending.active=false; stopMedia(w,m);
            if(!w.media_active) { m.position=target; m.paused=true; m.media_state=HOSINO_MEDIA_PAUSED; }
        }
        return;
    }
    if(cmd.op==Op::Volume) { m.volume=std::max(0,std::min(100,cmd.value)); if(w.media_active&&w.hooks.volume&&w.hooks.volume(w.hooks.context,m.volume)!=ESP_OK) text(m.error,sizeof(m.error),"音量调节失败"); return; }
    if(cmd.op==Op::Quality||cmd.op==Op::Rate) {
        if(cmd.op==Op::Quality) m.profile=cmd.value?1:0; else m.bitrate=cmd.value;
        if(w.media_active||pending.active) beginPlayback(w,m,pending,m.position);
        return;
    }
    if(cmd.op==Op::Play||cmd.op==Op::Seek) {
        if(cmd.op==Op::Play&&!playbackIdentityMatches(cmd.bvid,m.selected.bvid)) {
            text(m.error,sizeof(m.error),"页面内容已更新，请返回后重新选择");return;
        }
        if(cmd.cid) m.selected.cid=cmd.cid;
        beginPlayback(w,m,pending,cmd.op==Op::Seek?cmd.position:0); return;
    }
    char path[1100]{}, encoded[780]{};
    cJSON *root=nullptr;
    switch(cmd.op) {
    case Op::Feed: snprintf(path,sizeof(path),"/v1/feed?page=%d",std::max(1,cmd.page)); break;
    case Op::Search: encode(cmd.text,encoded,sizeof(encoded)); snprintf(path,sizeof(path),"/v1/search?q=%s&page=%d",encoded,std::max(1,cmd.page)); break;
    case Op::Detail: snprintf(path,sizeof(path),"/v1/video?bvid=%s",cmd.bvid); break;
    case Op::Me: text(path,sizeof(path),"/v1/me"); break;
    case Op::Folders: snprintf(path,sizeof(path),"/v1/folders?page=%d",std::max(1,cmd.page)); break;
    case Op::Favorites: snprintf(path,sizeof(path),"/v1/favorites?folder_id=%llu&page=%d",static_cast<unsigned long long>(cmd.folder),std::max(1,cmd.page)); break;
    case Op::PollQr: snprintf(path,sizeof(path),"/v1/auth/qr?login_id=%s",m.login_id); break;
    case Op::Qr: root=request(w,m,"/v1/auth/qr",HTTP_METHOD_POST,"{}"); break;
    case Op::Logout: root=request(w,m,"/v1/auth/logout",HTTP_METHOD_POST,"{}"); break;
    case Op::Favorite: {
        char body[160]; snprintf(body,sizeof(body),"{\"bvid\":\"%s\",\"folder_id\":%llu,\"action\":\"%s\"}",cmd.bvid,static_cast<unsigned long long>(cmd.folder),cmd.value?"remove":"add");
        root=request(w,m,"/v1/favorite",HTTP_METHOD_POST,body); break;
    }
    default: break;
    }
    if(path[0]) root=request(w,m,path);
    if(!root) return;
    auto *data=cJSON_GetObjectItemCaseSensitive(root,"data");
    if(cmd.op==Op::Feed||cmd.op==Op::Search||cmd.op==Op::Favorites||cmd.op==Op::Folders) {
        m.count=0; m.page=std::max(1,static_cast<int>(number(data,"page"))); m.more=flag(data,"has_more");
        auto *items=cJSON_GetObjectItemCaseSensitive(data,"items");
        cJSON *item;
        cJSON_ArrayForEach(item,items) {
            if(m.count==PageSize) break;
            auto &row=m.rows[m.count++]; row={};
            if(cmd.op==Op::Folders) { row.folder=static_cast<uint64_t>(number(item,"id")); row.count=static_cast<unsigned>(number(item,"count")); text(row.video.title,sizeof(row.video.title),str(item,"title")); }
            else readVideo(row.video,item);
        }
    } else if(cmd.op==Op::Detail) {
        readVideo(m.selected,data); m.part_count=0;
        auto *parts=cJSON_GetObjectItemCaseSensitive(data,"pages"); cJSON *item;
        cJSON_ArrayForEach(item,parts) {
            if(m.part_count==PartCount) break;
            auto &part=m.parts[m.part_count++]; part={}; part.cid=static_cast<uint64_t>(number(item,"cid")); part.duration=number(item,"duration"); text(part.title,sizeof(part.title),str(item,"title"));
        }
    } else if(cmd.op==Op::Me) {
        m.logged_in=flag(data,"logged_in"); m.mid=static_cast<uint64_t>(number(data,"mid")); m.level=static_cast<int>(number(data,"level")); m.coins=number(data,"coins");
        text(m.name,sizeof(m.name),str(data,"name")); text(m.vip,sizeof(m.vip),str(data,"vip_label"));
    } else if(cmd.op==Op::Qr) {
        text(m.login_id,sizeof(m.login_id),str(data,"login_id")); text(m.qr_url,sizeof(m.qr_url),str(data,"qr_url")); text(m.qr_status,sizeof(m.qr_status),"waiting");
    } else if(cmd.op==Op::PollQr) {
        text(m.qr_status,sizeof(m.qr_status),str(data,"status"));
        if(!strcmp(m.qr_status,"confirmed")) m.logged_in=true;
    } else if(cmd.op==Op::Logout) { m.logged_in=false; m.login_id[0]=m.qr_url[0]=m.name[0]=0; text(m.error,sizeof(m.error),"已退出B站登录"); }
    else if(cmd.op==Op::Favorite) text(m.error,sizeof(m.error),cmd.value?"已从收藏夹移除":"已添加到收藏夹");
    cJSON_Delete(root);
}
} // namespace

Worker::~Worker() {
    if(queue) vQueueDelete(queue);
    if(mutex) vSemaphoreDelete(mutex);
    for(auto *p:pixels) heap_caps_free(p);
    volatile char *p=token; for(size_t i=0;i<sizeof(token);++i) p[i]=0;
}
std::shared_ptr<Worker> Worker::create(const BiliMediaHooks &hooks) {
    void *raw=heap_caps_calloc(1,sizeof(Worker),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!raw) return {};
    auto state=std::shared_ptr<Worker>(new(raw) Worker,[](Worker *w){w->~Worker();heap_caps_free(w);});
    state->hooks=hooks; state->mutex=xSemaphoreCreateMutex(); state->queue=xQueueCreate(2,sizeof(Command));
    if(!state->mutex||!state->queue) return {};
    void *holder=heap_caps_malloc(sizeof(std::shared_ptr<Worker>),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!holder) return {};
    new(holder) std::shared_ptr<Worker>(state);
    if(xTaskCreateWithCaps(task,"bili_control",12288,holder,3,nullptr,MALLOC_CAP_SPIRAM)!=pdPASS) {
        static_cast<std::shared_ptr<Worker> *>(holder)->~shared_ptr(); heap_caps_free(holder); return {};
    }
    return state;
}
bool Worker::submit(const Command &cmd) { return !stop.load()&&queue&&xQueueSend(queue,&cmd,0)==pdTRUE; }
bool Worker::copy(Model &out) { if(xSemaphoreTake(mutex,0)!=pdTRUE) return false; out=model; xSemaphoreGive(mutex); return true; }
esp_err_t Worker::frame(const hosino_media_video_frame_t *frame,void *context) {
    auto &w=*static_cast<Worker *>(context);
    if(!frame||!w.frames.load()||w.stop.load()||!w.foreground.load()) return ESP_OK;
    if(!frame->data) return ESP_ERR_INVALID_ARG;
    if(frame->width>480||frame->height>480||!frame->width||!frame->height||frame->stride_bytes!=frame->width*2U||frame->bytes<frame->width*frame->height*2U) return ESP_ERR_INVALID_SIZE;
    if(xSemaphoreTake(w.mutex,pdMS_TO_TICKS(10))!=pdTRUE) return ESP_OK;
    const int target=w.displayed==0?1:0;
    const size_t bytes=frame->width*frame->height*2U;
    if(w.capacity[target]<bytes) {
        auto *replacement=static_cast<uint8_t *>(heap_caps_aligned_alloc(16,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!replacement) { xSemaphoreGive(w.mutex); return ESP_ERR_NO_MEM; }
        heap_caps_free(w.pixels[target]); w.pixels[target]=replacement; w.capacity[target]=bytes;
    }
    memcpy(w.pixels[target],frame->data,bytes); w.width[target]=frame->width; w.height[target]=frame->height; w.pending=target;
    xSemaphoreGive(w.mutex); return ESP_OK;
}
void Worker::task(void *argument) {
    auto *holder=static_cast<std::shared_ptr<Worker> *>(argument);
    std::shared_ptr<Worker> state=*holder; holder->~shared_ptr(); heap_caps_free(holder);
    auto &w=*state;
    auto *m=static_cast<Model *>(heap_caps_calloc(1,sizeof(Model),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!m) { w.done.store(true); state.reset(); vTaskDeleteWithCaps(nullptr); return; }
    new(m) Model; loadConfig(w,*m); commit(w,*m);
    Pending pending; Command cmd; int64_t lastPoll=0;
    while(!w.stop.load()) {
        if(!w.foreground.load()) {
            xQueueReset(w.queue);
            pending.active=false;
            if(w.media_active||m->session_id[0]) { stopMedia(w,*m); commit(w,*m); }
            if(!w.media_active&&xSemaphoreTake(w.mutex,pdMS_TO_TICKS(10))==pdTRUE) {
                if(w.displayed<0) for(unsigned i=0;i<2;++i) { heap_caps_free(w.pixels[i]);w.pixels[i]=nullptr;w.capacity[i]=0; }
                xSemaphoreGive(w.mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        if(xQueueReceive(w.queue,&cmd,pdMS_TO_TICKS(80))==pdTRUE) {
            if(cmd.op==Op::Feed||cmd.op==Op::Search||cmd.op==Op::Folders||cmd.op==Op::Favorites) { m->count=0; m->more=false; }
            m->busy=true; commit(w,*m); execute(w,*m,pending,cmd); memset(cmd.token,0,sizeof(cmd.token));
            m->completed_serial=cmd.serial; m->busy=false; commit(w,*m);
        }
        if(pending.active&&esp_timer_get_time()>=pending.next) { pollPlayback(w,*m,pending); commit(w,*m); }
        if(w.media_active&&esp_timer_get_time()-lastPoll>=200000&&w.hooks.snapshot) {
            BiliMediaSnapshot snapshot;
            if(w.hooks.snapshot(w.hooks.context,snapshot)==ESP_OK) {
                m->media_state=snapshot.state; m->position=m->source_start+snapshot.position_ms/1000.0;
                if(snapshot.error!=ESP_OK) snprintf(m->error,sizeof(m->error),"播放错误：%s",esp_err_to_name(snapshot.error));
                m->paused=snapshot.state==HOSINO_MEDIA_PAUSED;
                if(snapshot.state==HOSINO_MEDIA_FINISHED||snapshot.state==HOSINO_MEDIA_ERROR) m->playing=false;
                commit(w,*m);
            }
            lastPoll=esp_timer_get_time();
        }
    }
    pending.active=false;
    do { stopMedia(w,*m); if(w.media_active) vTaskDelay(pdMS_TO_TICKS(100)); } while(w.media_active);
    if(xSemaphoreTake(w.mutex,portMAX_DELAY)==pdTRUE) {
        if(w.displayed<0) for(unsigned i=0;i<2;++i) { heap_caps_free(w.pixels[i]);w.pixels[i]=nullptr;w.capacity[i]=0; }
        xSemaphoreGive(w.mutex);
    }
    m->~Model(); heap_caps_free(m);
    w.done.store(true); state.reset(); vTaskDeleteWithCaps(nullptr);
}
} // namespace esp_brookesia::apps::bili
