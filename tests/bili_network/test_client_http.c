/* Compiled around verbatim production HTTP/error/notice helpers by runner.
 * Models the pinned HTTP API boundary, not a device or real TLS connection. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=0x103,ESP_ERR_INVALID_SIZE=0x104,
      ESP_ERR_TIMEOUT=0x107,ESP_ERR_HTTP_EAGAIN=0x7007};
typedef struct {int value;int64_t advance;bool complete,cancel;} Event;
typedef struct {
    Event headers[16],reads[24];unsigned header_n,read_n,hi,ri;
    unsigned opens,writes;size_t sent,offset,write_limit;
    int status,open_error;bool complete,active;
    char post[256];const char *response;
} FakeHttp;
typedef FakeHttp *esp_http_client_handle_t;
static int64_t clock_us;
static int64_t esp_timer_get_time(void){return clock_us;}
static bool active(void *ctx){return ((FakeHttp *)ctx)->active;}
static int esp_http_client_open(FakeHttp *h,int length){h->opens++;assert(length>=0);return h->open_error;}
static int esp_http_client_write(FakeHttp *h,const char *p,int n){
    h->writes++;if(h->write_limit && (size_t)n>h->write_limit)n=(int)h->write_limit;
    assert(h->sent+(size_t)n<sizeof(h->post));memcpy(h->post+h->sent,p,(size_t)n);h->sent+=(size_t)n;return n;
}
static int64_t esp_http_client_fetch_headers(FakeHttp *h){
    assert(h->hi<h->header_n);Event e=h->headers[h->hi++];clock_us+=e.advance;h->complete=e.complete;
    if(e.cancel)h->active=false;return e.value;
}
static int esp_http_client_get_status_code(FakeHttp *h){return h->status;}
static int esp_http_client_read(FakeHttp *h,char *p,int n){
    assert(h->ri<h->read_n);Event e=h->reads[h->ri++];clock_us+=e.advance;h->complete=e.complete;
    if(e.cancel)h->active=false;
    if(e.value>0){assert(e.value<=n);memcpy(p,h->response+h->offset,(size_t)e.value);h->offset+=(size_t)e.value;}
    return e.value;
}
static bool esp_http_client_is_complete_data_received(FakeHttp *h){return h->complete;}

/* PRODUCTION_HELPERS */

static FakeHttp fresh(const char *response){FakeHttp h={0};h.active=true;h.status=200;h.response=response;clock_us=0;return h;}
static RequestIo run(FakeHttp *h,char *buffer,size_t capacity){
    RequestIo io={0};io.declared=-1;io.result=ESP_FAIL;
    const char *post="{\"test\":true}";
    transferResponse(h,post,strlen(post),buffer,capacity,12000000,active,h,&io);
    assert(h->opens<=1);if(h->opens)assert(h->sent==strlen(post)&&!memcmp(h->post,post,h->sent));return io;
}
static Event event(int n,int64_t advance,bool done,bool cancel){Event e={n,advance,done,cancel};return e;}
int main(void){
    char buffer[4097];const char *json="{\"ok\":true,\"data\":{\"items\":[]}}";size_t len=strlen(json);
    FakeHttp h=fresh(json);h.write_limit=5;
    h.headers[0]=event(-ESP_ERR_HTTP_EAGAIN,1800000,false,false);
    h.headers[1]=event(-ESP_ERR_HTTP_EAGAIN,1800000,false,false);
    h.headers[2]=event((int)len,1000,false,false);h.header_n=3;
    h.reads[0]=event(5,1000,false,false);h.reads[1]=event(-ESP_ERR_HTTP_EAGAIN,1800000,false,false);
    h.reads[2]=event((int)len-5,1000,true,false);h.read_n=3;
    RequestIo io=run(&h,buffer,4096);assert(io.result==0&&io.complete&&io.used==len&&io.header_waits==2&&io.body_waits==1);
    assert(h.opens==1&&h.hi==3&&h.ri==3&&h.writes>1);assert(!memcmp(buffer,json,len));
    cJSON *parsed=cJSON_ParseWithLength(buffer,io.used);assert(parsed);cJSON_Delete(parsed);
    /* Header read may already parse complete body into its cache: still drain it. */
    h=fresh(json);h.headers[0]=event((int)len,1000,true,false);h.header_n=1;
    h.reads[0]=event((int)len,1000,true,false);h.read_n=1;
    io=run(&h,buffer,4096);assert(io.result==0&&io.used==len&&h.ri==1);
    /* Endless header EAGAIN stops at total deadline, without reopening POST. */
    h=fresh(json);for(unsigned i=0;i<8;i++)h.headers[i]=event(-ESP_ERR_HTTP_EAGAIN,1800000,false,false);h.header_n=8;
    io=run(&h,buffer,4096);assert(io.result==ESP_ERR_TIMEOUT&&h.opens==1&&h.hi==7&&h.ri==0);
    /* Preserve partial bytes across waits, reject an incomplete deadline. */
    h=fresh(json);h.headers[0]=event((int)len,0,false,false);h.header_n=1;
    h.reads[0]=event(5,1000,false,false);
    for(unsigned i=1;i<9;i++)h.reads[i]=event(-ESP_ERR_HTTP_EAGAIN,1800000,false,false);h.read_n=9;
    io=run(&h,buffer,4096);assert(io.result==ESP_ERR_TIMEOUT&&!io.complete&&io.used==5&&h.opens==1);
    /* Cancellation, non-EAGAIN failures and EOF are never retried. */
    h=fresh(json);h.headers[0]=event((int)len,0,false,false);h.header_n=1;h.reads[0]=event(5,1,false,true);h.read_n=1;
    io=run(&h,buffer,4096);assert(io.result==ESP_ERR_INVALID_STATE&&io.used==5&&h.ri==1);
    h=fresh(json);h.headers[0]=event(-1,1,false,false);h.header_n=1;
    io=run(&h,buffer,4096);assert(io.result==ESP_FAIL&&h.hi==1&&h.ri==0);
    h=fresh(json);h.headers[0]=event((int)len,0,false,false);h.header_n=1;h.reads[0]=event(-1,1,false,false);h.read_n=1;
    io=run(&h,buffer,4096);assert(io.result==ESP_FAIL&&h.ri==1);
    h=fresh(json);h.headers[0]=event((int)len,0,false,false);h.header_n=1;h.reads[0]=event(0,1,false,false);h.read_n=1;
    io=run(&h,buffer,4096);assert(io.result==ESP_FAIL&&!strcmp(io.phase,"body_truncated")&&h.ri==1);
    /* Exact capacity completion succeeds; unknown-length overflow stays bounded. */
    char large[2048];memset(large,'x',sizeof(large));
    h=fresh(large);h.headers[0]=event(2048,0,false,false);h.header_n=1;h.reads[0]=event(2048,1,true,false);h.read_n=1;
    io=run(&h,buffer,2048);assert(io.result==0&&io.used==2048&&h.ri==1);
    h=fresh(large);h.headers[0]=event(0,0,false,false);h.header_n=1;h.reads[0]=event(2048,1,false,false);h.read_n=1;buffer[2048]='!';
    io=run(&h,buffer,2048);assert(io.result==ESP_ERR_INVALID_SIZE&&buffer[2048]=='!'&&h.ri==1);
    h=fresh(json);h.headers[0]=event(4097,0,false,false);h.header_n=1;
    io=run(&h,buffer,4096);assert(io.result==ESP_ERR_INVALID_SIZE&&h.ri==0);
    h=fresh(json);h.status=302;h.headers[0]=event((int)len,0,false,false);h.header_n=1;
    io=run(&h,buffer,4096);assert(io.result==ESP_FAIL&&h.opens==1&&h.ri==0);
    h=fresh(json);h.headers[0]=event((int)len,0,false,false);h.header_n=1;h.reads[0]=event((int)len,12000001,true,false);h.read_n=1;
    io=run(&h,buffer,4096);assert(io.result==ESP_ERR_TIMEOUT&&io.complete);
    h=fresh(json);h.active=false;io=run(&h,buffer,4096);assert(io.result==ESP_ERR_INVALID_STATE&&h.opens==0);
    /* Actual server object, legacy string and client-only deadline remain distinct. */
    cJSON *error=cJSON_Parse("{\"code\":\"source_transfer\",\"message\":\"上游视频读取失败\"}");assert(error);
    char message[192];preparationErrorText(message,sizeof(message),error,2);
    assert(strstr(message,"source_transfer")&&strstr(message,"上游视频读取失败")&&!strstr(message,"超时"));
    assert(!strcmp(bridgeLogCode(error),"source_transfer"));
    assert(preparationOutcome("error",true)==2&&preparationOutcome("failed",false)==2);
    assert(preparationOutcome("ready",true)==1&&preparationOutcome("preparing",false)==0&&preparationOutcome("preparing",true)==3);
    preparationErrorText(message,sizeof(message),error,3);assert(strstr(message,"75")&&!strstr(message,"source_transfer"));cJSON_Delete(error);
    error=cJSON_CreateString("旧接口错误说明");preparationErrorText(message,sizeof(message),error,2);assert(!strcmp(message,"旧接口错误说明"));assert(!strcmp(bridgeLogCode(error),"server_error"));cJSON_Delete(error);
    error=cJSON_Parse("{\"code\":\"secret-token-or-url\\n123\",\"message\":\"private\"}");assert(!strcmp(bridgeLogCode(error),"server_error"));cJSON_Delete(error);
    preparationErrorText(message,sizeof(message),NULL,2);assert(strstr(message,"失败")&&!strstr(message,"超时"));
    assert(shouldClearOldNotice("旧网络错误","旧网络错误"));
    assert(!shouldClearOldNotice("新的操作提示","旧网络错误"));
    assert(!shouldClearOldNotice("",""));
    const char *success[]={"设置已保存，返回后刷新首页","已退出B站登录","已从收藏夹移除","已添加到收藏夹"};
    for(unsigned i=0;i<4;i++)assert(!shouldClearOldNotice(success[i],success[i]));
    puts("PRODUCTION_HTTP_EAGAIN_COMPLETION_SINGLE_POST_BOUNDS_ERRORS_AND_NOTICE_PASS");return 0;
}
