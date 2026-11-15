"""Run actual login-validation helpers under deterministic host state stubs.

Tests read-only validation retries, persistence epochs and stale rejection
fencing. This is not an HTTP/device test and does not diagnose a live failure.
"""
from pathlib import Path
import hashlib,json,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'overlay-apps/components/hosino_console_client/hosino_console_client.cpp'
source=SRC.read_text('utf-8')
def body(marker):
    start=source.index('{',source.index(marker));depth=0
    for end in range(start,len(source)):
        if source[end]=='{':depth+=1
        elif source[end]=='}':
            depth-=1
            if not depth:return source[start:end+1]
    raise AssertionError(marker)
def adapt(s):
    return s.replace('char saved_cookie[65]{}','char saved_cookie[65]={0}').replace('hs_auth_store_status_t auth{}','hs_auth_store_status_t auth={0}').replace('hs_json j{}','hs_json j=0').replace('char kind[24]{}','char kind[24]={0}').replace('PairState::','PAIR_').replace('clear_private(*working)','clear_private(working)').replace('g, j)','g, &j)').replace('static_cast<unsigned>(id)','((unsigned)id)')
assert 'if (enabled && p->cookie_verified)' in source
assert 'if (!invalidate_auth(true, g))' in source
assert source.count('call("/api/login", true,')==1
poll=source[source.index('if (foreground) {',source.index('static void worker_task')):source.index('p->wipe_response(); p->invalidate_auth();')]
assert 'CommandType::Login' not in poll
login=slice(source.index('case CommandType::Login:'),source.index('case CommandType::Logout:'))
assert 'read_features(' not in source[login] and 'read_sessions(' not in source[login]
assert 'if (finish_login_request(g, c.request_id)) c.request_id = 0;' in source[login]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_OK 0
#define ESP_ERR_INVALID_RESPONSE 264
#define ESP_ERR_INVALID_STATE 259
#define nullptr NULL
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;typedef int hs_json;typedef int Session;
typedef struct {bool authed,auth_known,is_owner,busy;unsigned completed_request_id;int completed_request_error,completed_request_http_status,last_error,http_status;char status[160],completed_request_status[160];} Snapshot;
typedef struct {bool remember;uint32_t epoch;} hs_auth_store_status_t;
Snapshot state,*working=&state;
struct {char cookie[65];} response;
uint32_t generation=7,cookie_store_epoch=10,store_epoch=10;
bool stopping,foreground=true,cookie_verified,verify_restored,pending_login_save,qq_send_enabled,cache_valid=true;
bool active_post,initial_sync_due;int published;int64_t next_sessions;
bool device_mode=false,device_ready;struct {bool pending_ack;} device_record;int pair_state;enum{PAIR_Activating,PAIR_Ready};char device_id[96],device_label[96],pair_message[160];
char cookie[65],uncertain_key[256];void *session_cache;int kServerSessionPage=1;
int64_t next_me,retry_reads_at,now=100000000;
int saves,save_attempts,forgets,wipes,get_calls,race_mode;uint32_t queued_epoch;bool remember=true,transport_ok,me_authed=true,valid_me=true;
bool current(uint32_t g){return foreground&&!stopping&&generation==g;}
void hs_secure_zero(void *p,size_t n){memset(p,0,n);}
void clear_private(Snapshot *s){memset(s,0,sizeof(*s));}
void wipe_queue_locked(void){wipes++;}
void hs_auth_store_forget(uint32_t epoch){if(epoch==store_epoch){if(race_mode==5){generation++;queued_epoch=store_epoch;}store_epoch++;forgets++;}}
esp_err_t hs_auth_store_save(uint32_t epoch,const char *s){save_attempts++;if(!remember||epoch!=store_epoch)return 259;saves++;return 0;}
void hs_auth_store_status(hs_auth_store_status_t *s){s->remember=remember;s->epoch=store_epoch;
 if(race_mode==1){generation++;memset(cookie,0,sizeof(cookie));}
 if(race_mode==2)cookie[0]='z';
 if(race_mode==3){store_epoch++;remember=false;}
 if(race_mode==4)store_epoch++;
}
void text(char *dst,size_t n,const char *src){snprintf(dst,n,"%s",src);}
void set_status(const char *s,...){text(working->status,sizeof(working->status),s);working->last_error=0;}
void publish(uint32_t g){published++;}
bool field(hs_json j,int index,const char *name,char *dst,size_t n){return false;}
bool boolean(hs_json j,int index,const char *name){return false;}
int64_t esp_timer_get_time(void){return now;}
bool call(const char *route,bool post,const char *payload,uint32_t g,hs_json *j){assert(!post);assert(!strcmp(route,"/api/me"));get_calls++;return transport_ok;}
int hs_json_member(hs_json *j,int index,const char *name){return strcmp(name,"authed")?1:0;}
bool hs_json_bool(hs_json *j,int field,bool *out){if(!valid_me)return false;*out=field?true:me_authed;return true;}
'''
for declaration,marker in [
 ('bool acknowledge_request(uint32_t g,uint32_t id)','bool acknowledge_request('),
 ('bool invalidate_auth(bool forget_saved,uint32_t expected_generation)','bool invalidate_auth('),
 ('bool accept_login_cookie(uint32_t g,uint32_t auth_epoch)','bool accept_login_cookie('),
 ('bool finish_cookie_validation(uint32_t g)','bool finish_cookie_validation('),
 ('bool finish_login_request(uint32_t g,uint32_t id)','bool finish_login_request('),
 ('bool read_me(uint32_t g)','bool read_me(')]:
    code+=declaration+'\n'+adapt(body(marker))+'\n'
code+=r'''
void reset(void){memset(&state,0,sizeof(state));memset(cookie,0,sizeof(cookie));memset(&response,0,sizeof(response));memset(response.cookie,'a',64);generation=7;cookie_store_epoch=10;store_epoch=10;stopping=false;foreground=true;cookie_verified=false;verify_restored=false;pending_login_save=false;remember=true;transport_ok=false;me_authed=true;valid_me=true;saves=save_attempts=forgets=wipes=get_calls=race_mode=0;queued_epoch=0;}
int main(void){
 reset();assert(accept_login_cookie(7,10));assert(verify_restored&&pending_login_save&&!cookie_verified);assert(!read_me(7));assert(verify_restored&&pending_login_save);assert(next_me==now+3000000);assert(saves==0&&get_calls==1);
 transport_ok=true;assert(read_me(7));assert(state.authed&&cookie_verified&&!verify_restored&&!pending_login_save);assert(saves==1);assert(read_me(7));assert(saves==1&&save_attempts==1);
 reset();memset(cookie,'b',64);verify_restored=true;transport_ok=true;assert(read_me(7));assert(saves==0&&save_attempts==0);assert(cookie_verified);
 reset();assert(accept_login_cookie(7,10));generation=8;assert(!finish_cookie_validation(7));assert(!cookie_verified&&saves==0);assert(!invalidate_auth(true,7));assert(cookie[0]&&wipes==0&&forgets==0&&pending_login_save);
 assert(invalidate_auth(true,8));assert(!cookie[0]&&!pending_login_save&&!verify_restored&&!cookie_verified);assert(wipes==1&&forgets==1);
 reset();assert(accept_login_cookie(7,10));foreground=false;assert(!finish_cookie_validation(7));assert(!invalidate_auth(true,7));assert(cookie[0]);foreground=true;generation=8;transport_ok=true;assert(read_me(8));assert(saves==1&&state.authed);
 reset();assert(!accept_login_cookie(6,10));assert(!cookie[0]&&!verify_restored);stopping=true;assert(!accept_login_cookie(7,10));
 reset();assert(accept_login_cookie(7,10));remember=false;transport_ok=true;assert(read_me(7));assert(saves==0&&!pending_login_save&&cookie_verified);
 reset();assert(accept_login_cookie(7,10));store_epoch=11;transport_ok=true;assert(read_me(7));assert(saves==1&&!pending_login_save&&cookie_store_epoch==11);assert(read_me(7));assert(saves==1);
 reset();assert(accept_login_cookie(7,10));race_mode=1;assert(!finish_cookie_validation(7));assert(saves==0&&save_attempts==0);
 reset();assert(accept_login_cookie(7,10));race_mode=2;finish_cookie_validation(7);assert(saves==0&&save_attempts==0);
 reset();assert(accept_login_cookie(7,10));race_mode=3;assert(finish_cookie_validation(7));assert(saves==0&&!remember&&pending_login_save);race_mode=0;assert(finish_cookie_validation(7));assert(saves==0&&!remember&&!pending_login_save);
 reset();assert(accept_login_cookie(7,10));race_mode=4;assert(finish_cookie_validation(7));assert(saves==0&&pending_login_save);race_mode=0;assert(finish_cookie_validation(7));assert(saves==1&&!pending_login_save&&cookie_store_epoch==11);
 reset();assert(accept_login_cookie(7,10));race_mode=5;assert(invalidate_auth(true,7));assert(generation==8&&queued_epoch==10&&store_epoch==11);race_mode=0;assert(accept_login_cookie(8,queued_epoch));transport_ok=true;assert(read_me(8));assert(saves==1&&!pending_login_save&&cookie_store_epoch==11);
 reset();assert(accept_login_cookie(7,10));transport_ok=true;me_authed=false;assert(read_me(7));assert(!state.authed&&!cookie[0]&&!verify_restored&&!pending_login_save&&saves==0&&forgets==1);
 reset();published=0;active_post=true;initial_sync_due=false;next_sessions=999;state.authed=true;state.busy=true;assert(finish_login_request(7,12));assert(!active_post&&!state.busy&&initial_sync_due&&next_sessions==0);assert(state.completed_request_id==12&&state.completed_request_error==0&&state.completed_request_status[0]&&published==1&&get_calls==0);state.last_error=259;strcpy(state.status,"later poll");assert(!acknowledge_request(7,0));assert(state.completed_request_id==12&&state.completed_request_error==0&&strcmp(state.completed_request_status,"later poll"));
 reset();active_post=true;assert(!finish_login_request(7,13));assert(active_post&&state.completed_request_id==0);state.authed=true;generation=8;assert(!finish_login_request(7,13));assert(active_post&&state.completed_request_id==0);
 reset();state.last_error=259;state.http_status=503;strcpy(state.status,"specific auth transport failure");assert(acknowledge_request(7,14));strcpy(state.status,"background changed");state.http_status=200;assert(!acknowledge_request(7,0));assert(state.completed_request_error==259&&state.completed_request_http_status==503&&!strcmp(state.completed_request_status,"specific auth transport failure"));
 puts("PASS login validation: GET-only retry, single save, restored-cookie no-save, generation/logout/pause fences, opt-out, epoch rebinding and interleavings, definite denial");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='hs-login-v19-',dir='C:/esp/hosino-qspi') as folder:
    p=Path(folder);(p/'test.c').write_text(code,'utf-8');compiler=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
    subprocess.run([str(compiler),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
    result=subprocess.run([str(p/'test.exe')],capture_output=True,text=True,check=True)
    assert SRC.read_text('utf-8')==source
    report={'passed':True,'source_sha256':hashlib.sha256(SRC.read_bytes()).hexdigest(),'output':result.stdout.strip(),'runtime_assertions':code.count('assert('),'scope':__doc__,'static_checks':['Only Login command contains login POST','Background poll contains no Login','Remember saves only verified cookie','401 invalidation passes generation']}
    (ROOT/'evidence/console-v19-login-revalidation-test-20261112.json').write_text(json.dumps(report,indent=2)+'\n','utf-8');print(result.stdout.strip())
