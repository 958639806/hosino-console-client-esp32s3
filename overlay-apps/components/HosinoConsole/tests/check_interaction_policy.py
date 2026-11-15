"""Run the current UI policy bodies with host C/LVGL-state stubs.

No copied policy implementation: function bodies are extracted from the actual
cpp, with only namespaces/member-call syntax mechanically adapted to C. This is
not a renderer/device test. No network, serial, or firmware build is involved.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import tempfile

COMPONENT = Path(__file__).resolve().parents[1]
ROOT = COMPONENT.parents[2]
SOURCE = COMPONENT / "HosinoConsole.cpp"
source = SOURCE.read_text(encoding="utf-8")
header = (COMPONENT / "HosinoConsole.hpp").read_text(encoding="utf-8")
client = (COMPONENT.parent / "hosino_console_client/include/hosino_console_client.hpp").read_text(encoding="utf-8")
compiler = ROOT / "tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"
assert compiler.is_file(), "Existing host TCC is required; no download attempted"


def body(marker):
    start = source.index("{", source.index(marker))
    depth = 0
    for end in range(start, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if not depth:
                return source[start:end + 1]
    raise AssertionError(marker)


def enum(text, name, prefix):
    values = re.search(r"enum class " + name + r"[^\{]*\{([^}]+)\}", text).group(1)
    return "typedef enum {" + ",".join(prefix + value.strip() for value in values.split(",") if value.strip()) + "} " + name + ";\n"


def translate(value):
    value = re.sub(r"using hosino_console::[A-Za-z_]+;", "", value)
    value = value.replace("hosino_console::", "")
    for name, prefix in [("CommandType", "CMD_"), ("PairState", "PAIR_"), ("Action", "ACT_"), ("View", "VIEW_"), ("EditKind", "EDIT_")]:
        value = value.replace(name + "::", prefix)
    value = value.replace("pendingWrite()", "pendingWrite(u)").replace("selectionReady()", "selectionReady(u)").replace("canStartPairing()", "canStartPairing(u)").replace("canForgetDevice()", "canForgetDevice(u)")
    value = value.replace("_client->cancelPairing()", "cancelPairingClient(_client)")
    return value.replace("static_cast<unsigned char>(src[n])", "((unsigned char)src[n])")


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
'''
for text, name, prefix in [(client, "CommandType", "CMD_"), (client,"PairState","PAIR_"), (header, "Action", "ACT_"),
                           (header, "View", "VIEW_"), (header, "EditKind", "EDIT_")]:
    code += enum(text, name, prefix)
code += r'''
typedef struct {bool busy,remember_busy,selected,authed,send_uncertain,device_saved,device_credential_busy; PairState pairing_state; char device_id[96]; char status[160]; uint32_t accepted_send_request_id; char selected_key[256];} Snapshot;
typedef struct {char text[1024];} Textarea;
typedef struct {unsigned cancel_calls;} Client;
static void cancelPairingClient(Client *c){++c->cancel_calls;}
typedef struct {
 Snapshot *snapshot; Client *client; char confirm_device[96]; bool request_pending,view_pending; CommandType pending_type;
 uint32_t sent_id; char selecting[256],confirm_key[256],sent_text[1024],sent_key[256],draft[1024],edit_key[256];
 View view; EditKind edit; Textarea *textarea;
} UI;
#define _snapshot (u->snapshot)
#define _client (u->client)
#define _confirmDeviceId (u->confirm_device)
#define _requestPending (u->request_pending)
#define _pendingType (u->pending_type)
#define _selectingKey (u->selecting)
#define _confirmSession (u->confirm_key)
#define _sentRequestId (u->sent_id)
#define _sentDraft (u->sent_text)
#define _sentSession (u->sent_key)
#define _draft (u->draft)
#define _view (u->view)
#define _viewPending (u->view_pending)
#define _edit (u->edit)
#define _editSession (u->edit_key)
#define _textarea (u->textarea)
static const char *lv_textarea_get_text(Textarea *t){return t->text;}
static void lv_textarea_set_text(Textarea *t,const char *s){assert(strlen(s)<sizeof(t->text));strcpy(t->text,s);}
'''
functions = [
    ("void erase_text(", "static void erase_text(char *text,size_t size)"),
    ("void copy_text(", "static void copy_text(char *dst,size_t capacity,const char *src)"),
    ("bool writes_state(", "static bool writes_state(CommandType type)"),
    ("bool HosinoConsole::pendingWrite()", "static bool pendingWrite(UI *u)"),
    ("const char *HosinoConsole::pendingWriteStatus()", "static const char *pendingWriteStatus(UI *u)"),
    ("bool HosinoConsole::selectionReady()", "static bool selectionReady(UI *u)"),
    ("bool HosinoConsole::canResolveSend()", "static bool canResolveSend(UI *u)"),
    ("bool HosinoConsole::canStartPairing()", "static bool canStartPairing(UI *u)"),
    ("bool HosinoConsole::canForgetDevice()", "static bool canForgetDevice(UI *u)"),
    ("void HosinoConsole::cancelPairingUi()", "static void cancelPairingUi(UI *u)"),
    ("bool HosinoConsole::actionBlocked(", "static bool actionBlocked(UI *u,Action action)"),
    ("void HosinoConsole::acceptSentDraft()", "static void acceptSentDraft(UI *u)"),
]
for marker, signature in functions:
    code += signature + translate(body(marker)) + "\n"
code += r'''
static unsigned checks;
#define CHECK(expr) do{++checks;assert(expr);}while(0)
static void setup(UI *u,Snapshot *s,Textarea *t){
 memset(u,0,sizeof(*u));memset(s,0,sizeof(*s));memset(t,0,sizeof(*t));
 u->snapshot=s;u->textarea=t;s->selected=true;s->authed=true;strcpy(s->selected_key,"one");
 strcpy(u->sent_key,"one");strcpy(u->edit_key,"one");strcpy(u->sent_text,"old");strcpy(u->draft,"old");u->sent_id=7;
 u->view=VIEW_Chat;u->edit=EDIT_Message;
}
int main(void){
 UI u;Snapshot s;Textarea t;setup(&u,&s,&t);
 for(unsigned busy=0;busy<2;++busy){s.busy=busy;
  CHECK(!actionBlocked(&u,ACT_SelectSession));CHECK(!actionBlocked(&u,ACT_Compose));CHECK(!actionBlocked(&u,ACT_Voice));
  u.request_pending=true;u.pending_type=CMD_RefreshSessions;
  CHECK(!actionBlocked(&u,ACT_SelectSession));CHECK(!actionBlocked(&u,ACT_Compose));CHECK(!actionBlocked(&u,ACT_Voice));
 }
 u.pending_type=CMD_Send;
 CHECK(actionBlocked(&u,ACT_SelectSession));CHECK(actionBlocked(&u,ACT_Voice));CHECK(actionBlocked(&u,ACT_EditAccept));CHECK(actionBlocked(&u,ACT_ConfirmSend));
 CHECK(!actionBlocked(&u,ACT_Compose));CHECK(!actionBlocked(&u,ACT_Back));
 u.pending_type=CMD_SendQQ;CHECK(actionBlocked(&u,ACT_ConfirmSend));CHECK(!actionBlocked(&u,ACT_Compose));
 u.pending_type=CMD_CreateSession;CHECK(actionBlocked(&u,ACT_Compose));
 u.pending_type=CMD_SelectSession;strcpy(u.selecting,"two");
 CHECK(!selectionReady(&u));CHECK(actionBlocked(&u,ACT_Compose));CHECK(actionBlocked(&u,ACT_Voice));CHECK(actionBlocked(&u,ACT_Abort));
 CHECK(actionBlocked(&u,ACT_EditAccept));CHECK(actionBlocked(&u,ACT_ConfirmSend));CHECK(!actionBlocked(&u,ACT_SelectSession));
 strcpy(s.selected_key,"two");CHECK(selectionReady(&u));CHECK(!actionBlocked(&u,ACT_Compose));CHECK(!actionBlocked(&u,ACT_Voice));
 setup(&u,&s,&t);acceptSentDraft(&u);CHECK(!strcmp(u.draft,"old")&&u.sent_id==7);
 s.accepted_send_request_id=8;acceptSentDraft(&u);CHECK(!strcmp(u.draft,"old")&&u.sent_id==7);
 s.accepted_send_request_id=7;acceptSentDraft(&u);CHECK(!u.draft[0]&&!u.sent_id&&!u.sent_text[0]&&!u.sent_key[0]);
 setup(&u,&s,&t);s.accepted_send_request_id=7;strcpy(u.draft,"next message");acceptSentDraft(&u);CHECK(!strcmp(u.draft,"next message"));
 setup(&u,&s,&t);s.accepted_send_request_id=7;u.view=VIEW_Edit;strcpy(t.text,"edited next message");acceptSentDraft(&u);
 CHECK(!strcmp(u.draft,"edited next message"));CHECK(!strcmp(t.text,"edited next message"));
 setup(&u,&s,&t);s.accepted_send_request_id=7;u.view=VIEW_Edit;strcpy(t.text,"old");acceptSentDraft(&u);CHECK(!u.draft[0]&&!t.text[0]);
 setup(&u,&s,&t);s.accepted_send_request_id=7;u.view=VIEW_Edit;u.view_pending=true;strcpy(u.draft,"saved before wiping editor");
 acceptSentDraft(&u);CHECK(!strcmp(u.draft,"saved before wiping editor"));
 setup(&u,&s,&t);s.accepted_send_request_id=7;strcpy(s.selected_key,"another session");acceptSentDraft(&u);CHECK(!strcmp(u.draft,"old"));
 setup(&u,&s,&t);s.send_uncertain=true;
 CHECK(!actionBlocked(&u,ACT_Compose));CHECK(!actionBlocked(&u,ACT_Voice));
 CHECK(!actionBlocked(&u,ACT_Sessions));CHECK(!actionBlocked(&u,ACT_Back));
 CHECK(!actionBlocked(&u,ACT_ReviewSend));
 u.view=VIEW_ReviewSend;strcpy(u.confirm_key,"one");CHECK(canResolveSend(&u));
 strcpy(u.confirm_key,"two");CHECK(!canResolveSend(&u));strcpy(u.confirm_key,"one");
 s.authed=false;CHECK(!canResolveSend(&u));s.authed=true;
 s.send_uncertain=false;CHECK(!canResolveSend(&u));s.send_uncertain=true;
 u.request_pending=true;u.pending_type=CMD_SendQQ;
 CHECK(!canResolveSend(&u));CHECK(actionBlocked(&u,ACT_ResolveSend));CHECK(!actionBlocked(&u,ACT_ReviewSend));
 u.pending_type=CMD_ResolveUncertain;CHECK(!canResolveSend(&u));CHECK(actionBlocked(&u,ACT_EditAccept));
 CHECK(!actionBlocked(&u,ACT_Back));u.request_pending=false;
 strcpy(u.selecting,"other");CHECK(!canResolveSend(&u));CHECK(actionBlocked(&u,ACT_ResolveSend));
 setup(&u,&s,&t);CHECK(canStartPairing(&u));
 for(PairState state=PAIR_Starting;state<=PAIR_Verifying;++state){s.pairing_state=state;CHECK(!canStartPairing(&u));CHECK(!actionBlocked(&u,ACT_PairCancel));}
 s.pairing_state=PAIR_Expired;CHECK(canStartPairing(&u));s.device_credential_busy=true;CHECK(!canStartPairing(&u));s.device_credential_busy=false;
 s.pairing_state=PAIR_Error;CHECK(canStartPairing(&u));s.pairing_state=PAIR_Cancelled;CHECK(canStartPairing(&u));
 u.request_pending=true;u.pending_type=CMD_PairStart;CHECK(!canStartPairing(&u));CHECK(!actionBlocked(&u,ACT_PairCancel));
 Client c={0};u.client=&c;cancelPairingUi(&u);CHECK(c.cancel_calls==1&&!u.request_pending);
 u.request_pending=true;u.pending_type=CMD_Send;cancelPairingUi(&u);CHECK(c.cancel_calls==2&&u.request_pending);u.request_pending=false;
 u.view=VIEW_ForgetDevice;s.device_saved=true;strcpy(s.device_id,"device-a");strcpy(u.confirm_device,"device-a");CHECK(canForgetDevice(&u));
 strcpy(s.device_id,"device-b");CHECK(!canForgetDevice(&u));strcpy(s.device_id,"device-a");
 s.device_credential_busy=true;CHECK(!canForgetDevice(&u));s.device_credential_busy=false;
 u.view=VIEW_DeviceIdentity;CHECK(!canForgetDevice(&u));u.view=VIEW_ForgetDevice;
 s.device_saved=false;CHECK(!canForgetDevice(&u));s.device_saved=true;
 u.request_pending=true;u.pending_type=CMD_ForgetDevice;CHECK(!canForgetDevice(&u));CHECK(!actionBlocked(&u,ACT_Back));
 setup(&u,&s,&t);u.pending_type=CMD_Login;
 strcpy(s.status,"password accepted; verifying session");CHECK(!strcmp(pendingWriteStatus(&u),s.status));
 s.busy=true;CHECK(!strcmp(pendingWriteStatus(&u),s.status));
 s.status[0]=0;CHECK(strlen(pendingWriteStatus(&u))>0);
 u.pending_type=CMD_Send;strcpy(s.status,"background GET");CHECK(strcmp(pendingWriteStatus(&u),s.status)!=0);
 u.pending_type=CMD_TranscribeMic;CHECK(!strcmp(pendingWriteStatus(&u),s.status));
 printf("PASS %u assertions: polling, explicit writes, selected target, matching ACK, edited/navigation drafts, uncertain recovery, device pairing/local forgetting and login phase guards\n",checks);return 0;
}
'''
pair = body("void HosinoConsole::renderPairing()")
assert "hosino.ume.wiki/pair.html" in pair and "lv_font_montserrat_40" in pair
assert "user_code" in pair and "verification_uri" not in pair
assert not re.search(r"pollSecret|poll_secret|pair_id|pairId|device_token|bearer_token", source)
assert "_client->cancelPairing()" in body("void HosinoConsole::cancelPairingUi()")
assert "CommandType::PairStart" in body("void HosinoConsole::onAction(")
refresh = body("void HosinoConsole::refreshControls()")
assert "else if (pendingWrite()) message = pendingWriteStatus()" in refresh
assert "_snapshot->authed && _snapshot->busy" in refresh
update = body("void HosinoConsole::update()")
assert "_snapshot->completed_request_status" in update
assert "!_authedBefore && _snapshot->authed && _noticeIsLogin" in update
assert '"可上滑返回桌面"' in body("void HosinoConsole::render()")
signature = body("uint64_t HosinoConsole::contentSignature()")
assert not re.search(r"\b(?:s\.(?:busy|revision|status|voice_elapsed_ms)|_snapshot->busy)\b", signature)
assert "pendingWrite()" not in signature
assert "_snapshot->busy" not in body("void HosinoConsole::renderChat(")
assert "_snapshot->busy" not in body("void HosinoConsole::renderSessions(")
review = body("void HosinoConsole::renderSendReview()")
assert '"已核对，恢复发送"' in review and '"返回查看会话历史"' in review
assert "_snapshot->send_http_status" in review and "_snapshot->send_status" in review
assert "!_snapshot->send_uncertain" not in body("void HosinoConsole::renderChat(")
assert "_snapshot->send_uncertain" not in body("void HosinoConsole::onVoiceRequested()")
action = body("void HosinoConsole::onAction(")
resolve = action.split("case Action::ResolveSend:",1)[1].split("case Action::RememberLogin:",1)[0]
assert "canResolveSend() && submit(CommandType::ResolveUncertain)" in resolve
assert "CommandType::Send," not in resolve and "CommandType::SendQQ" not in resolve
assert "erase_text(_draft" not in resolve
assert "else if (writes_state(type))" in body("bool HosinoConsole::submit(")
with tempfile.TemporaryDirectory(prefix="hs-ui-policy-") as directory:
    d = Path(directory)
    (d / "policy.c").write_text(code, encoding="utf-8")
    build = subprocess.run([str(compiler.resolve()), "-Wall", str(d / "policy.c"), "-o", str(d / "policy.exe")], capture_output=True, text=True)
    if build.returncode:
        raise SystemExit(build.stdout + build.stderr)
    result = subprocess.run([str(d / "policy.exe")], capture_output=True, text=True)
    print(result.stdout, end="")
    if result.returncode:
        raise SystemExit(result.stderr or result.returncode)
    report = {
        "passed": True, "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        "functions": [marker for marker, _ in functions], "output": result.stdout.strip(),
        "static_content_checks": "busy/revision/status/elapsed excluded from content signature; chat/list busy gating absent; superseded selection intent cleared",
        "scope": "Actual extracted production policy bodies with mechanical C syntax adaptation and state-only LVGL stubs; not a render, firmware or device test",
        "hardware_or_network_access": False,
    }
    (COMPONENT / "tests/interaction-policy-result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
