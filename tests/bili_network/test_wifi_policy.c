#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef int esp_err_t;
typedef enum {WIFI_PS_NONE,WIFI_PS_MIN_MODEM,WIFI_PS_MAX_MODEM} wifi_ps_type_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=259};
static wifi_ps_type_t power=WIFI_PS_MIN_MODEM;
static unsigned ps_get_calls,ps_set_calls,get_fail_at,set_fail_at;
static bool get_fail_always;
static int last_set;
static char logs[4096];
static void fake_log(const char *tag,const char *fmt,...) {
 (void)tag;va_list v;va_start(v,fmt);vsnprintf(logs+strlen(logs),sizeof(logs)-strlen(logs),fmt,v);va_end(v);
}
#define ESP_LOGI fake_log
#define ESP_LOGW fake_log
static esp_err_t esp_wifi_get_ps(wifi_ps_type_t *p) {++ps_get_calls;if(get_fail_always||ps_get_calls==get_fail_at)return ESP_FAIL;*p=power;return 0;}
static esp_err_t esp_wifi_set_ps(wifi_ps_type_t p) {++ps_set_calls;last_set=p;if(ps_set_calls==set_fail_at)return ESP_FAIL;power=p;return 0;}
/* PRODUCTION_POLICY */

typedef void *hosino_media_player_handle_t;
typedef int (*hosino_media_video_cb_t)(const void *,void *);
typedef struct {int owner;bool enable_video,allow_video_without_audio;int max_width,max_height,render_fps,video_decoder;hosino_media_video_cb_t video_cb;void *video_ctx;int network_video_width,network_video_height,network_video_fps;} hosino_media_player_config_t;
typedef struct {const char *stream_url,*allowed_base_url,*device_token,*session_id;double position_seconds,duration_seconds;int video_width,video_height,video_fps;} BiliPlaybackRequest;
typedef struct {int state,error;uint64_t position_ms,duration_ms;} BiliMediaSnapshot;
typedef struct {int state,last_error;uint64_t position_ms,duration_ms;} hosino_media_player_snapshot_t;
typedef struct {void *handle;hosino_bili_wifi_policy_t wifi;} MediaContext;
enum {HOSINO_MEDIA_OWNER_VIDEO=1,HOSINO_MEDIA_DECODER_ESPRESSIF=0,HOSINO_MEDIA_PLAYING=2,HOSINO_MEDIA_PAUSED=3,HOSINO_MEDIA_FINISHED=4,HOSINO_MEDIA_ERROR=5};
#define CONFIG_LWIP_TCP_WND_DEFAULT 32768
#define CONFIG_LWIP_TCP_RECVMBOX_SIZE 24
static unsigned creates,destroys,starts,opens;
static int fail_create,fail_open,fail_start,fail_destroy,fail_pause,fail_resume,fail_poll,fail_copy;
static hosino_media_player_snapshot_t media_value;
static int media_handle;
static esp_err_t hosino_media_player_create(const hosino_media_player_config_t *c,void **out) {++creates;assert(c->enable_video);*out=fail_create?NULL:&media_handle;return fail_create?ESP_FAIL:0;}
static esp_err_t hosino_media_player_destroy(void *p) {(void)p;++destroys;return fail_destroy?ESP_FAIL:0;}
static esp_err_t hosino_media_player_open_network(void *p,const char *u,const char *b,const char *t) {(void)p;(void)u;(void)b;(void)t;++opens;return fail_open?ESP_FAIL:0;}
static esp_err_t hosino_media_player_start(void *p) {(void)p;++starts;return fail_start?ESP_FAIL:0;}
static esp_err_t hosino_media_player_pause(void *p) {(void)p;return fail_pause?ESP_FAIL:0;}
static esp_err_t hosino_media_player_resume(void *p) {(void)p;return fail_resume?ESP_FAIL:0;}
static esp_err_t hosino_media_player_set_volume(void *p,int v) {(void)p;(void)v;return 0;}
static esp_err_t hosino_media_player_poll(void *p) {(void)p;return fail_poll?ESP_FAIL:0;}
static esp_err_t hosino_media_player_get_snapshot(void *p,hosino_media_player_snapshot_t *o) {(void)p;*o=media_value;return fail_copy?ESP_FAIL:0;}
/* PRODUCTION_ADAPTER_BODIES */

typedef int cJSON;
typedef struct {bool frames,media_active;struct {esp_err_t (*stop)(void *);void *context;} hooks;} Worker;
typedef struct {char error[192],session_id[64];bool playing,paused;int media_state;} Model;
enum {HTTP_METHOD_DELETE=3,HOSINO_MEDIA_STOPPED=0};
static unsigned deletes;
static void text(char *out,size_t capacity,const char *s) {snprintf(out,capacity,"%s",s);}
static cJSON *request(Worker *w,Model *m,const char *p,int method,const char *body,bool quiet) {
 (void)w;(void)m;(void)body;(void)quiet;assert(method==HTTP_METHOD_DELETE);assert(strstr(p,"/v1/playbacks?session_id=")==p);++deletes;return NULL;
}
static void cJSON_Delete(cJSON *p) {(void)p;}
/* PRODUCTION_WORKER_STOP */

static void reset(MediaContext *ctx) {
 memset(ctx,0,sizeof(*ctx));power=WIFI_PS_MIN_MODEM;ps_get_calls=ps_set_calls=get_fail_at=set_fail_at=0;get_fail_always=false;deletes=0;
 creates=destroys=opens=starts=0;fail_create=fail_open=fail_start=fail_destroy=0;
 fail_pause=fail_resume=fail_poll=fail_copy=0;logs[0]=0;media_value=(hosino_media_player_snapshot_t){0};
}
int main(void) {
 MediaContext ctx;BiliPlaybackRequest req={"https://example.invalid/private","https://example.invalid","PRIVATE_TOKEN","PRIVATE_SESSION",0,30,480,270,15};
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);assert(power==WIFI_PS_NONE&&ctx.wifi.owned&&ctx.wifi.previous==WIFI_PS_MIN_MODEM);
 assert(stop(&ctx)==0&&power==WIFI_PS_MIN_MODEM&&!ctx.wifi.owned&&!ctx.handle);
 reset(&ctx);power=WIFI_PS_MAX_MODEM;assert(open(&ctx,&req,NULL,NULL)==0);assert(open(&ctx,&req,NULL,NULL)==0);assert(ctx.wifi.previous==WIFI_PS_MAX_MODEM);assert(stop(&ctx)==0&&power==WIFI_PS_MAX_MODEM);
 reset(&ctx);power=WIFI_PS_NONE;assert(open(&ctx,&req,NULL,NULL)==0);assert(!ctx.wifi.owned&&ps_set_calls==0);assert(stop(&ctx)==0&&ps_set_calls==0);
 reset(&ctx);fail_create=1;assert(open(&ctx,&req,NULL,NULL)==ESP_FAIL&&ps_get_calls==0&&ps_set_calls==0&&!ctx.handle);
 reset(&ctx);fail_open=1;assert(open(&ctx,&req,NULL,NULL)==ESP_FAIL&&ps_get_calls==0&&ps_set_calls==0&&!ctx.handle);
 reset(&ctx);fail_start=1;assert(open(&ctx,&req,NULL,NULL)==ESP_FAIL&&ps_set_calls==2&&power==WIFI_PS_MIN_MODEM&&!ctx.wifi.owned&&!ctx.handle);
 reset(&ctx);get_fail_at=1;assert(open(&ctx,&req,NULL,NULL)==ESP_FAIL&&ps_set_calls==0&&!ctx.wifi.owned&&!ctx.handle);
 reset(&ctx);set_fail_at=1;assert(open(&ctx,&req,NULL,NULL)==ESP_FAIL&&ps_set_calls==1&&!ctx.wifi.owned&&!ctx.handle);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);fail_destroy=1;assert(stop(&ctx)==ESP_FAIL&&ctx.handle&&power==WIFI_PS_MIN_MODEM&&!ctx.wifi.owned);fail_destroy=0;assert(stop(&ctx)==0&&!ctx.handle);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);get_fail_at=ps_get_calls+1;assert(stop(&ctx)==0&&ctx.wifi.owned&&!ctx.handle);get_fail_at=0;assert(stop(&ctx)==0&&!ctx.wifi.owned&&power==WIFI_PS_MIN_MODEM);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);set_fail_at=ps_set_calls+1;assert(stop(&ctx)==0&&ctx.wifi.owned&&!ctx.handle);unsigned before=creates;get_fail_always=true;assert(open(&ctx,&req,NULL,NULL)==ESP_FAIL&&creates==before&&ctx.wifi.previous==WIFI_PS_MIN_MODEM);get_fail_always=false;assert(stop(&ctx)==0);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);power=WIFI_PS_MAX_MODEM;assert(stop(&ctx)==0&&power==WIFI_PS_MAX_MODEM&&ps_set_calls==1&&!ctx.wifi.owned);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);BiliMediaSnapshot out={0};media_value.state=HOSINO_MEDIA_FINISHED;assert(snapshot(&ctx,&out)==0&&out.state==HOSINO_MEDIA_FINISHED&&!ctx.wifi.owned&&power==WIFI_PS_MIN_MODEM);assert(stop(&ctx)==0);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);media_value.state=HOSINO_MEDIA_ERROR;media_value.last_error=ESP_FAIL;assert(snapshot(&ctx,&out)==0&&out.error==ESP_FAIL&&!ctx.wifi.owned);assert(stop(&ctx)==0);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);fail_copy=1;assert(snapshot(&ctx,&out)==ESP_FAIL&&!ctx.wifi.owned);assert(stop(&ctx)==0);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);fail_poll=1;assert(snapshot(&ctx,&out)==ESP_FAIL&&!ctx.wifi.owned);assert(stop(&ctx)==0);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);assert(pause(&ctx,true)==0&&!ctx.wifi.owned&&power==WIFI_PS_MIN_MODEM);assert(pause(&ctx,false)==0&&ctx.wifi.owned&&power==WIFI_PS_NONE);assert(stop(&ctx)==0);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);fail_pause=1;assert(pause(&ctx,true)==ESP_FAIL&&ctx.wifi.owned&&power==WIFI_PS_NONE);fail_pause=0;fail_resume=1;assert(pause(&ctx,false)==ESP_FAIL&&!ctx.wifi.owned&&power==WIFI_PS_MIN_MODEM);assert(stop(&ctx)==0);
 /* Permanent restore failure cannot keep the actual Worker close loop active
  * after the player was destroyed, nor prevent DELETE of its server session. */
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);get_fail_always=true;
 Worker w={true,true,{stop,&ctx}};Model m={0};m.playing=true;strcpy(m.session_id,"fixture-session");
 unsigned loops=0;do {stopMedia(&w,&m,true);++loops;} while(w.media_active&&loops<5);
 assert(loops==1&&!w.media_active&&!w.frames&&!m.playing&&!m.session_id[0]&&deletes==1&&!ctx.handle&&ctx.wifi.owned);
 assert(snapshot(&ctx,&out)==0&&ctx.wifi.owned);assert(stop(&ctx)==0&&ctx.wifi.owned);
 before=creates;assert(open(&ctx,&req,NULL,NULL)==ESP_FAIL&&creates==before&&!ctx.handle);
 get_fail_always=false;assert(stop(&ctx)==0&&!ctx.wifi.owned&&power==WIFI_PS_MIN_MODEM);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);fail_destroy=1;w=(Worker){true,true,{stop,&ctx}};m=(Model){0};strcpy(m.session_id,"fixture-session");
 stopMedia(&w,&m,true);assert(w.media_active&&ctx.handle&&deletes==0&&m.session_id[0]);fail_destroy=0;stopMedia(&w,&m,true);assert(!w.media_active&&!ctx.handle&&deletes==1);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);get_fail_always=true;assert(pause(&ctx,true)==0&&ctx.wifi.owned);get_fail_always=false;assert(pause(&ctx,false)==0&&ctx.wifi.owned);assert(stop(&ctx)==0);
 reset(&ctx);assert(open(&ctx,&req,NULL,NULL)==0);get_fail_at=ps_get_calls+1;assert(pause(&ctx,true)==0&&ctx.wifi.owned&&power==WIFI_PS_NONE);media_value.state=HOSINO_MEDIA_PAUSED;assert(snapshot(&ctx,&out)==0&&out.state==HOSINO_MEDIA_PAUSED&&!ctx.wifi.owned&&power==WIFI_PS_MIN_MODEM);assert(stop(&ctx)==0);
 /* Re-entering a held lease revalidates current PS and preserves newer
  * independent policy as the value to restore after this new acquisition. */
 reset(&ctx);assert(hosino_bili_wifi_enter(&ctx.wifi)==0);before=ps_set_calls;assert(hosino_bili_wifi_enter(&ctx.wifi)==0&&ps_set_calls==before&&ctx.wifi.previous==WIFI_PS_MIN_MODEM);
 power=WIFI_PS_MAX_MODEM;assert(hosino_bili_wifi_enter(&ctx.wifi)==0&&ctx.wifi.previous==WIFI_PS_MAX_MODEM&&power==WIFI_PS_NONE);assert(hosino_bili_wifi_leave(&ctx.wifi)==0&&power==WIFI_PS_MAX_MODEM);
 assert(!strstr(logs,"PRIVATE_TOKEN")&&!strstr(logs,"PRIVATE_SESSION")&&!strstr(logs,"https://"));
 puts("PRODUCTION_WIFI_LEASE_ADAPTER_AND_WORKER_PERMANENT_RESTORE_FAILURE_NO_CLOSE_LOOP_PASS");return 0;
}
