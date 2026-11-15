"""Exercise actual v18 local-recovery and send-result bodies with C state stubs.

No network, serial, credentials, build staging or production edits. Extraction
keeps production conditionals; C++ enum/member syntax alone is adapted for TCC.
This does not validate GUI rendering, HTTP transport, or live QQ delivery.
"""
from pathlib import Path
import argparse, hashlib, json, re, subprocess, tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--evidence', default='evidence/console-v18-send-recovery-test-20261112.json')
args = parser.parse_args()

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'overlay-apps/components/hosino_console_client/hosino_console_client.cpp'
source = SRC.read_text('utf-8')
def block_at(start):
    pos = source.index('{', start); depth = 0
    for end in range(pos, len(source)):
        if source[end] == '{': depth += 1
        elif source[end] == '}':
            depth -= 1
            if not depth: return source[pos:end+1], end
    raise AssertionError('unterminated body')
call, _ = block_at(source.index('bool call_send('))
reset, _ = block_at(source.index('if (sending && !blocked_uncertain)'))
final, final_end = block_at(source.index('if (sending && !blocked_uncertain && current(g))'))
switch, switch_end = block_at(source.index('else switch (c.type)'))
assert switch_end < source.index('if (sending && !blocked_uncertain && current(g))'), 'Finalization must be outside switch'
resolve = source.split('case CommandType::ResolveUncertain:',1)[1].split('case CommandType::SendQQ:',1)[0]
assert not any(x in resolve for x in ['call(', 'call_send(', 'acknowledge_send(', '_draft'])
explicit, _ = block_at(source.index('bool explicit_post('))
assert 'ResolveUncertain' not in explicit
enum = re.search(r'enum class CommandType[^\{]*\{([^}]+)', (SRC.parent/'include/hosino_console_client.hpp').read_text('utf-8')).group(1)
enum = 'typedef enum {' + ','.join('CMD_'+v.strip() for v in enum.split(',') if v.strip()) + '} CommandType;'
def c(value):
    return value.replace('CommandType::','CMD_').replace('c.', 'cmd.')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_ERR_INVALID_STATE 259
#define ESP_LOGI(...) ((void)0)
typedef int hs_json;
typedef struct {bool selected,send_uncertain; int http_status,send_http_status,last_error; unsigned accepted_send_request_id;
 char selected_key[256],send_result_key[256],error_code[48],send_error_code[48],status[160],send_status[160];} Snapshot;
Snapshot state,*working=&state;
char uncertain_key[256],request[1024],draft[1024];
int calls; bool current_generation=true;
bool current(unsigned g){return current_generation;}
void text(char *dst,size_t n,const char *src){snprintf(dst,n,"%s",src);}
void hs_secure_zero(void *p,size_t n){memset(p,0,n);}
void set_status(const char *s,int error){text(working->status,sizeof(working->status),s);working->last_error=error;}
bool call(const char *route,bool post,const char *payload,unsigned g,hs_json *j){calls++;working->http_status=502;strcpy(working->error_code,"gateway_error");strcpy(working->status,"synthetic gateway failure");return false;}
'''+enum+r'''
typedef struct {CommandType type;bool confirm,deliver;char session_key[256];} Command;
Command cmd;
bool sending,blocked_uncertain;unsigned g=1;
bool call_send(const char *route,unsigned g,hs_json *json)
'''+c(call)+r'''
void initialize_send(void){sending=cmd.type==CMD_Send||cmd.type==CMD_SendQQ;blocked_uncertain=sending&&uncertain_key[0]&&!strcmp(uncertain_key,cmd.session_key);if(sending&&!blocked_uncertain)
'''+c(reset)+r'''}
void finalize_send(void){if(sending&&!blocked_uncertain&&current(g))
'''+c(final)+r'''}
void resolve_local(void){do{
'''+c(resolve).replace('set_status("已恢复发送；未重发任何消息")','set_status("已恢复发送；未重发任何消息",0)')+r'''
}while(0);}
void prepare(void){memset(&state,0,sizeof(state));memset(&cmd,0,sizeof(cmd));calls=0;current_generation=true;strcpy(uncertain_key,"session-a");strcpy(working->selected_key,"session-a");strcpy(cmd.session_key,"session-a");working->selected=true;working->send_uncertain=true;cmd.confirm=true;working->accepted_send_request_id=77;strcpy(draft,"preserve me");}
int main(void){hs_json j=0;
 prepare();resolve_local();assert(!uncertain_key[0]);assert(!working->send_uncertain);assert(calls==0);assert(working->accepted_send_request_id==77);assert(!strcmp(draft,"preserve me"));
 prepare();cmd.confirm=false;resolve_local();assert(uncertain_key[0]);assert(working->last_error==259);
 prepare();working->selected=false;resolve_local();assert(uncertain_key[0]);
 prepare();strcpy(cmd.session_key,"session-b");resolve_local();assert(!strcmp(uncertain_key,"session-a"));
 prepare();uncertain_key[0]=0;resolve_local();assert(working->last_error==259);
 prepare();strcpy(uncertain_key,"session-b");resolve_local();assert(!strcmp(uncertain_key,"session-b"));
 prepare();uncertain_key[0]=0;cmd.type=CMD_SendQQ;initialize_send();assert(!strcmp(working->send_result_key,"session-a"));assert(!call_send("/api/qq/send",g,&j));assert(calls==1);assert(working->send_http_status==502);assert(!strcmp(working->send_status,"synthetic gateway failure"));
 strcpy(working->status,"generic uncertainty");strcpy(working->error_code,"send_uncertain");finalize_send();assert(working->send_http_status==502);assert(!strcmp(working->send_status,"synthetic gateway failure"));assert(!strcmp(working->send_error_code,"gateway_error"));
 working->http_status=200;strcpy(working->status,"poll success");working->error_code[0]=0;cmd.type=CMD_RefreshHistory;initialize_send();finalize_send();assert(working->send_http_status==502);assert(!strcmp(working->send_error_code,"gateway_error"));
 strcpy(uncertain_key,"session-a");cmd.type=CMD_SendQQ;initialize_send();assert(blocked_uncertain);finalize_send();assert(working->send_http_status==502);assert(!strcmp(working->send_status,"synthetic gateway failure"));
 current_generation=false;working->http_status=0;call_send("/api/qq/send",g,&j);assert(working->send_http_status==502);
 puts("PASS state assertions: local recovery validation/no POST/no accepted ACK/no draft mutation; 502 details survive generic status, polls and blocked duplicate");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='hs-send-v18-',dir='C:/esp/hosino-qspi') as folder:
    path=Path(folder);(path/'test.c').write_text(code,'utf-8')
    compiler=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
    subprocess.run([str(compiler),str(path/'test.c'),'-o',str(path/'test.exe')],check=True)
    result=subprocess.run([str(path/'test.exe')],capture_output=True,text=True,check=True)
    assert SRC.read_text('utf-8')==source,'Source changed during test'
    report={'passed':True,'source_sha256':hashlib.sha256(SRC.read_bytes()).hexdigest(),'output':result.stdout.strip(),
      'static_checks':['Resolve case has no network/accepted-send ACK/draft operations','Resolve is not explicit_post','Send-result finalization outside command switch'],
      'scope':__doc__}
    (ROOT/args.evidence).write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
    print(result.stdout.strip())
