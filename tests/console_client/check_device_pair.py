"""Execute actual pairing state-machine bodies with deterministic C state stubs.

This verifies ordering/cancellation/error policy, not TLS, actual NVS or servers.
No credentials, serial ports or network are accessed; test credentials are fixed
synthetic values. Production method bodies are extracted, not reimplemented.
"""
from pathlib import Path
import hashlib,json,re,subprocess,tempfile
R=Path(__file__).resolve().parents[2];P=R/'overlay-apps/components/hosino_console_client/hosino_console_client.cpp';s=P.read_text('utf-8')
def body(marker):
    start=s.index('{',s.index(marker));depth=0
    for end in range(start,len(s)):
        if s[end]=='{':depth+=1
        elif s[end]=='}':
            depth-=1
            if not depth:return s[start:end+1]
    raise AssertionError(marker)
def convert(v):
    v=v.replace('PairState::','PAIR_').replace('DeviceStoreOp::','OP_').replace('nullptr','NULL').replace('clear_private(*working)','clear_private(working)')
    v=re.sub(r'\b(hs_json|hs_device_store_record_t|hs_device_store_status_t) (\w+)\{\}',r'\1 \2={0}',v)
    v=re.sub(r'char (\w+)\[(\d+)\]\{\}',r'char \1[\2]={0}',v)
    v=v.replace('g, j,','g, &j,').replace('g, j)','g, &j)').replace('std::max<uint32_t>','u_max').replace('std::min<uint32_t>','u_min')
    return v
header=(P.parent/'include/hosino_console_client.hpp').read_text('utf-8')
states=re.search(r'enum class PairState[^\{]*\{([^}]+)',header).group(1)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 259
#define ESP_ERR_INVALID_RESPONSE 264
#define HS_DEVICE_STORE_SCHEMA 1
typedef int esp_err_t;typedef int hs_json;
hs_json json;
typedef enum {OP_None,OP_Restore,OP_SaveNew,OP_SaveAck,OP_Forget} DeviceStoreOp;
'''+ 'typedef enum {'+','.join('PAIR_'+v.strip() for v in states.split(',') if v.strip())+'} PairState;\n'+r'''
typedef struct {uint32_t schema;bool pending_ack;char token[129],pair_id[33];} hs_device_store_record_t;
typedef struct {bool ready,busy,saved,saved_known;esp_err_t error;uint32_t epoch;} hs_device_store_status_t;
typedef struct {bool authed,auth_known,is_owner;uint32_t retry_after_ms;} Snapshot;
Snapshot state,*working=&state;
struct {int status;} response;
DeviceStoreOp device_store_op=OP_None;hs_device_store_record_t device_record,disk,write_record;
uint32_t device_ticket,device_epoch=1;bool device_restore_due,device_mode,device_saved,device_ready,device_suppressed;
bool pair_cancel_requested,pair_running,device_resume_due,restore_due,verify_restored,foreground=true,stopping;
bool initial_sync_due;int64_t next_sessions;
uint32_t count,generation=1;PairState pair_state=PAIR_Idle;
char pair_id[33],pair_secret[44],pair_user_code[10],pair_message[160],device_label[96],device_id[96],request[8192];
int64_t pair_expires,pair_next,now=10000000,next_me;uint32_t pair_interval=3;
uint32_t device_store_retry_ms=3000;
int ack_calls,poll_calls,me_calls,features_calls,sessions_calls,publishes,save_calls,forget_calls;
bool mock_ok=true,mock_ack=true,mock_activated=true,mock_authed=true,reply_ready;int mock_http=200,store_error,storage_op;
char mock_state[24]="pending",mock_code[48]="",mock_kind[24]="device";
const char *fake_token="hsd_0123456789ab.ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq";
const char *fake_pair="hsp_ABCDEFGHIJKLMNOPQRSTUV";
uint32_t mock_interval=3;
bool current(uint32_t g){return foreground&&!stopping&&generation==g;}
int64_t esp_timer_get_time(void){return now;}
void hs_secure_zero(void *p,size_t n){memset(p,0,n);}
void text(char *d,size_t n,const char *v){snprintf(d,n,"%s",v);}
void set_status(const char *v,...){}
void clear_private(Snapshot *p){memset(p,0,sizeof(*p));}
void publish(uint32_t g){publishes++;}
void wipe_response(void){}
void read_features(uint32_t g){features_calls++;}
void read_sessions(unsigned n,uint32_t g){sessions_calls++;}
bool invalidate_auth(bool forget,uint32_t g){return true;}
bool finish_cookie_validation(uint32_t g){return false;}
uint32_t u_max(uint32_t a,uint32_t b){return a>b?a:b;}uint32_t u_min(uint32_t a,uint32_t b){return a<b?a:b;}
int hs_json_member(hs_json *j,int obj,const char *name){if(!strcmp(name,"authed"))return 2;if(!strcmp(name,"isOwner"))return 3;if(!strcmp(name,"interval"))return 4;return 1;}
bool hs_json_bool(hs_json *j,int token,bool *out){*out=token==2?mock_authed:false;return true;}
uint32_t number(hs_json j,int token,...){return token==4?mock_interval:300;}
bool field(hs_json j,int obj,const char *name,char *dst,size_t cap,...){const char *v="";
 if(!strcmp(name,"status"))v=mock_state;else if(!strcmp(name,"token"))v=fake_token;else if(!strcmp(name,"kind"))v=mock_kind;
 else if(!strcmp(name,"id"))v="device-1";else if(!strcmp(name,"name"))v="test-device";else if(!strcmp(name,"code"))v=mock_code;
 snprintf(dst,cap,"%s",v);return true;}
bool boolean(hs_json j,int obj,const char *name){return !strcmp(name,"ok")?mock_ack:!strcmp(name,"activated")?mock_activated:false;}
bool call(const char *route,bool post,const char *payload,uint32_t g,hs_json *j,...){if(!current(g))return false;
 if(!strcmp(route,"/api/devices/pair/ack")){assert(post&&device_mode&&device_saved&&disk.token[0]);ack_calls++;}
 else if(!strcmp(route,"/api/devices/pair/poll")){assert(post);poll_calls++;}
 else if(!strcmp(route,"/api/me")){assert(!post);me_calls++;}
 else assert(0);response.status=mock_http;return mock_ok;}
void hs_device_store_status(hs_device_store_status_t *out){*out=(hs_device_store_status_t){true,false,disk.token[0]!=0,true,0,1};}
int hs_device_store_load(uint32_t epoch,uint32_t *ticket){*ticket=1;storage_op=1;reply_ready=false;return 0;}
int hs_device_store_save(uint32_t epoch,const hs_device_store_record_t *rec,uint32_t *ticket){save_calls++;write_record=*rec;*ticket=1;storage_op=2;reply_ready=false;return 0;}
int hs_device_store_forget(uint32_t epoch,uint32_t *ticket){forget_calls++;*ticket=1;storage_op=3;reply_ready=false;return 0;}
bool hs_device_store_take(uint32_t ticket,hs_device_store_record_t *out,uint32_t *epoch,int *err){if(!reply_ready)return false;reply_ready=false;*out=(hs_device_store_record_t){0};out->schema=1;*epoch=1;*err=store_error;
 if(!store_error){if(storage_op==1)*out=disk;else if(storage_op==2)disk=write_record;else memset(&disk,0,sizeof(disk));}return true;}
'''
for decl,marker in [
 ('bool base64url(const char *s,size_t length)','static bool base64url('),
 ('bool valid_pair_id(const char *s)','static bool valid_pair_id('),
 ('bool valid_device_token(const char *s)','static bool valid_device_token('),
 ('void clear_pair_secret(void)','void clear_pair_secret('),
 ('void pair_status(PairState state,const char *message)','void pair_status('),
 ('void stop_pair(bool cancelled)','void stop_pair('),
 ('void forget_device(uint32_t g,bool explicit_user)','void forget_device('),
 ('bool read_me(uint32_t g)','bool read_me('),
 ('void poll_pair(uint32_t g)','void poll_pair('),
 ('bool service_device(uint32_t g)','bool service_device(')]:
    code+=decl+'\n'+convert(body(marker))+'\n'
code+='bool handle_device_error(uint32_t g){if(response.status==401)'+convert(body('if (response.device_auth)'))+'return true;}\n'
code+=r'''
void reset(void){memset(&state,0,sizeof(state));memset(&device_record,0,sizeof(device_record));memset(&disk,0,sizeof(disk));memset(&write_record,0,sizeof(write_record));memset(pair_secret,0,sizeof(pair_secret));memset(pair_id,0,sizeof(pair_id));device_store_op=OP_None;device_ticket=0;device_epoch=1;device_restore_due=device_mode=device_saved=device_ready=device_suppressed=pair_cancel_requested=pair_running=device_resume_due=restore_due=verify_restored=stopping=reply_ready=false;foreground=true;generation=1;count=0;pair_state=PAIR_Idle;now=10000000;pair_next=0;pair_expires=now+300000000;pair_interval=3;device_store_retry_ms=3000;ack_calls=poll_calls=me_calls=features_calls=sessions_calls=publishes=save_calls=forget_calls=store_error=storage_op=0;mock_ok=mock_ack=mock_activated=mock_authed=true;mock_http=200;mock_interval=3;strcpy(mock_state,"pending");strcpy(mock_kind,"device");}
void waiting(void){pair_state=PAIR_Waiting;pair_running=true;strcpy(pair_id,fake_pair);memset(pair_secret,'s',43);pair_secret[43]=0;}
void approved(void){waiting();strcpy(mock_state,"approved");assert(service_device(1));assert(device_store_op==OP_SaveNew&&save_calls==1&&ack_calls==0);}
int main(void){
 assert(valid_device_token(fake_token));assert(!valid_device_token("hsd_invalid"));assert(valid_pair_id(fake_pair));assert(!valid_pair_id("hsp_short"));
 reset();approved();assert(!device_saved);service_device(1);assert(ack_calls==0);reply_ready=true;service_device(1);assert(ack_calls==1&&device_saved&&disk.pending_ack&&pair_state==PAIR_Verifying);
 service_device(1);assert(me_calls==1&&device_store_op==OP_SaveAck&&!state.authed&&!device_ready);service_device(1);assert(me_calls==1);reply_ready=true;service_device(1);assert(!disk.pending_ack&&state.authed&&device_ready&&pair_state==PAIR_Ready);assert(features_calls==0&&sessions_calls==0&&initial_sync_due&&next_sessions==0);
 reset();approved();store_error=1;reply_ready=true;service_device(1);assert(ack_calls==0&&pair_state==PAIR_Error&&!state.authed);
 reset();approved();reply_ready=true;service_device(1);service_device(1);assert(device_store_op==OP_SaveAck);store_error=1;reply_ready=true;service_device(1);assert(device_record.pending_ack&&!state.authed&&!device_ready&&pair_state==PAIR_Activating&&disk.pending_ack);now=pair_next;store_error=0;service_device(1);service_device(1);assert(device_store_op==OP_SaveAck&&!state.authed);reply_ready=true;service_device(1);assert(!disk.pending_ack&&state.authed&&pair_state==PAIR_Ready);
 reset();disk.schema=1;disk.pending_ack=true;strcpy(disk.token,fake_token);strcpy(disk.pair_id,fake_pair);device_restore_due=true;service_device(1);assert(device_store_op==OP_Restore&&ack_calls==0);reply_ready=true;service_device(1);assert(ack_calls==1&&poll_calls==0);
 reset();disk.schema=1;disk.pending_ack=true;strcpy(disk.token,fake_token);strcpy(disk.pair_id,fake_pair);device_restore_due=true;service_device(1);reply_ready=true;foreground=false;pair_cancel_requested=true;assert(!service_device(1));assert(device_store_op==OP_Restore&&ack_calls==0&&reply_ready);foreground=true;generation=2;device_resume_due=true;service_device(2);assert(ack_calls==1&&device_saved);
 reset();approved();reply_ready=true;mock_ok=false;mock_http=503;service_device(1);assert(disk.pending_ack&&ack_calls==1&&forget_calls==0);now+=3000000;mock_ok=true;mock_http=200;service_device(1);assert(ack_calls==2&&pair_state==PAIR_Verifying);
 reset();approved();reply_ready=true;service_device(1);mock_activated=false;service_device(1);assert(!state.authed&&!device_ready&&disk.pending_ack&&save_calls==1&&pair_state==PAIR_Activating);
 reset();approved();reply_ready=true;service_device(1);strcpy(mock_kind,"password");service_device(1);assert(!state.authed&&disk.pending_ack);
 reset();waiting();pair_cancel_requested=true;service_device(1);assert(pair_state==PAIR_Cancelled&&!pair_secret[0]&&!pair_id[0]&&poll_calls==0);
 reset();waiting();pair_cancel_requested=true;foreground=false;service_device(1);assert(!pair_secret[0]&&!pair_running&&poll_calls==0);
 reset();approved();pair_cancel_requested=true;service_device(1);reply_ready=true;service_device(1);assert(device_saved&&pair_state==PAIR_Cancelled&&ack_calls==0);device_resume_due=true;service_device(1);assert(ack_calls==1);
 reset();approved();device_suppressed=true;pair_state=PAIR_Idle;reply_ready=true;service_device(1);assert(device_saved&&!device_mode&&ack_calls==0);
 reset();waiting();mock_ok=false;mock_http=429;mock_interval=9;service_device(1);assert(pair_interval>=9&&pair_next>=now+9000000&&forget_calls==0);
 reset();waiting();mock_ok=false;mock_http=410;service_device(1);assert(pair_state==PAIR_Expired&&!pair_secret[0]&&forget_calls==0);
 reset();waiting();strcpy(mock_state,"denied");service_device(1);assert(pair_state==PAIR_Denied&&!pair_secret[0]);
 reset();device_mode=true;device_saved=true;strcpy(device_record.token,fake_token);forget_device(1,true);assert(forget_calls==1&&!device_mode&&device_saved);reply_ready=true;service_device(1);assert(!device_saved&&!device_record.token[0]);
 reset();response.status=401;strcpy(mock_code,"unauthorized");handle_device_error(1);assert(forget_calls==0);strcpy(mock_code,"device_token_invalid");handle_device_error(1);assert(forget_calls==1);
 reset();response.status=401;strcpy(mock_code,"device_token_expired");handle_device_error(1);assert(forget_calls==1);
 reset();strcpy(mock_code,"device_token_invalid");response.status=403;handle_device_error(1);response.status=429;handle_device_error(1);response.status=500;handle_device_error(1);assert(forget_calls==0);
 puts("PASS pairing actual state machine: persist-before-ACK, strict activated identity, pendingAck persistence, reboot, retries, cancellation, password fallback, rate limits and local forget");return 0;}
'''
# Auth header assembly is mutually exclusive, and only explicit credential errors erase.
auth=s[s.index('if (override_cookie)'):s.index('esp_http_client_config_t cfg')]
assert 'else if (device_mode && device_record.token[0])' in auth and 'else {' in auth
deny=s[s.index('if (response.status == 401)'):s.index('if (result != ESP_OK)')]
assert 'device_token_invalid' in deny and 'device_token_expired' in deny and 'forget_device(g, false)' in deny
assert 'https://hosino.ume.wiki/pair.html' in s
with tempfile.TemporaryDirectory(prefix='hs-pair-v19-',dir='C:/esp/hosino-qspi') as folder:
    p=Path(folder);(p/'test.c').write_text(code,'utf-8');compiler=R/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
    subprocess.run([str(compiler),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
    run=subprocess.run([str(p/'test.exe')],capture_output=True,text=True,check=True);assert P.read_text('utf-8')==s
    report={'passed':True,'source_sha256':hashlib.sha256(P.read_bytes()).hexdigest(),'runtime_assertions':code.count('assert('),'output':run.stdout.strip(),'scope':__doc__,'static_checks':['Cookie and Bearer mutually exclusive','401 token-invalid/expired erase gate','Fixed approval URL']}
    (R/'evidence/console-v19-device-pair-test-20261112.json').write_text(json.dumps(report,indent=2)+'\n','utf-8');print(run.stdout.strip())
