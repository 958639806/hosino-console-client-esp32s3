# Console login persistence

An opt-in, bounded NVS service for the existing console client. New source under
Apache-2.0. The default is disabled. Only an explicit `setRememberLogin(true)`
authorizes saving a validated 64-hex `hs_sid`; passwords are never accepted by
this component. The saved preference and cookie use a versioned 66-byte blob at
NVS namespace `hs_auth`, key `session`.

All NVS reads/writes run on one 4096-byte **internal RAM stack** worker. The
12 KiB PSRAM HTTP worker and GUI only submit bounded requests. A latest-state
mailbox coalesces pending writes; a preference/forget epoch rejects late saves
from older login attempts. An in-flight save finishes before a later clear on
the same worker. `busy` remains observable until the latest operation finishes.
An erase is asynchronous: power loss or an NVS error before successful clearing
can leave the old on-flash record. The UI must show pending/error state and offer
retry; this implementation does not claim synchronous secure erasure.

Restore results are one-shot and checked against both their ticket and epoch.
Cancellation and consumption wipe the component-owned cookie buffers. Mailbox
copies and worker stack records are wiped after use; there is no permanent
component RAM cache of the token. ESP-IDF's NVS internals are not modified.
The worker remains alive for later app opens and blocks on a notification when
idle. Its memory cost includes the 4 KiB internal stack and task control block.

The client restores only when a new Client opens, then validates with GET `/api/me`
before reporting authenticated or enabling authenticated operations. Returning
from the background retains the existing RAM login behavior. Closing wipes the
Client RAM cookie without calling server logout. Explicit logout, HTTP 401, and
`/api/me` reporting `authed:false` clear the saved token; disabling remember also
removes the preference/record. Logout preserves a previously selected remember
preference but clears its token. No POST is retried by persistence.

The provided API contract advertises `Max-Age=604800` (seven days), and restarting
the server invalidates its in-process sessions. Saving a token does not extend
its server lifetime. NVS encryption is not enabled in this firmware: this is
convenience persistence, not a claim of encrypted credential storage.

## Validation

The production C service passes deterministic host tests with mocked NVS and
FreeRTOS scheduling: default off, cold persisted restore, invalid record rejection,
late saves after logout/opt-out, logout during an in-flight write, invalidation
during/after a read, cancellation, failed writes and retry. The mock verifies the
worker requests `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` with a 4096-byte stack.
These tests do not execute a real FreeRTOS scheduler or real flash transactions.

On 2026-11-12 the service C and updated Client C++ passed actual Xtensa GCC 14.2.0 /
ESP-IDF 5.5.5 `-fsyntax-only` with no diagnostics. Full firmware build, device
reopen/restore/logout tests, internal heap/stack watermark, and power-loss behavior
remain for device validation.

Host command (existing executable output directory required):

```powershell
& tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe `
  -DHOSINO_AUTH_STORE_HOST_TEST `
  -Ioverlay-apps/components/hosino_console_auth_store/include `
  -Ioverlay-apps/components/hosino_console_auth_store/tests/stubs `
  -o C:/esp/hosino-qspi/host-console-client/test_auth_store.exe `
  overlay-apps/components/hosino_console_auth_store/tests/test_auth_store.c `
  overlay-apps/components/hosino_console_auth_store/hosino_console_auth_store.c
& C:/esp/hosino-qspi/host-console-client/test_auth_store.exe
& C:/esp/hosino-qspi/host-console-client/test_auth_store.exe restore
& C:/esp/hosino-qspi/host-console-client/test_auth_store.exe corrupt
& C:/esp/hosino-qspi/host-console-client/test_auth_store.exe startup-cancel
& C:/esp/hosino-qspi/host-console-client/test_auth_store.exe retry
```

## Device pairing credentials

`hosino_console_device_store.h` adds a separate device credential store. Pairing
itself is explicit persistence authorization; the cookie remember checkbox does
not control this record. Both services share the existing 4096-byte internal
stack worker. Each loop gives both mailboxes a turn; no new task or caller-side
NVS access is introduced. Cookie API and `hs_auth/session` serialization remain
unchanged.

The device record lives at `hs_auth/device_v1`. Its fixed 167-byte wire format is
schema1 little-endian (4 bytes), a 0/1 pending-ACK byte, token[129], pairId[33]. All
unused bytes are zero. It is written as one blob and committed before a successful
save completion can be consumed. Public structs are serialized explicitly, so
compiler padding is never stored. Tokens must match `hsd_` +12 lowercase hex+`.`
+43 base64url characters (60 total), pair IDs `hsp_` +22 base64url (26 total).
Invalid size, schema, flag, format or nonzero tail fails closed without erasing.
No token, pair ID or credential body is logged or embedded in firmware.

Call init, await ready, then obtain the current device epoch from status. Load
and save are asynchronous single-operation requests returning a nonzero ticket;
call take with that exact ticket. Successful load of an absent record returns
schema1 with empty token/pair ID. Save/forget completion returns an empty record;
retain and separately wipe the caller's token. **Only matching save completion
with ESP_OK permits the client to ACK pairing to the server.** A pending_ack
credential survives reopen, enabling retry of an idempotent server ACK. After
server ACK success, atomically save the same record with pending_ack=false and
await that save too. Network errors alone never erase stored credentials.

Forget is for explicit unbinding or confirmed invalid/revoked authentication. A
matching epoch is required; UINT32_MAX is reserved for explicit force-forget.
It advances the device epoch and supersedes queued work. An already executing
write completes first, then this worker erases and commits. Await the forget
ticket before reporting deletion. Cancel also advances epoch, discards queued
work and wipes pending results, but does not erase flash or undo an executing
write. Therefore a canceled pairing save may already be durable; reopening can
restore its pending_ack state. Stale tickets cannot authorize server ACK.

On NVS write/commit failure the worker reads back presence, without returning a
successful save result. Status.saved is meaningful only when saved_known=true;
otherwise it is the last observed value. This avoids assuming a failed commit
necessarily preserved or erased the old record. NVS encryption remains disabled;
this does not claim secret extraction protection or secure flash erasure.

Validation: `evidence/device-store-host-20261112.json` records five unchanged
cookie scenarios and five device-store runs. Fake NVS enforces worker-only I/O
and transactional set/commit, including failure before/after persistence,
queued/in-flight cancellation, epoch changes, startup forget, cold pending ACK
restore, invalid records, format rejection and independent cookie/device keys.
`evidence/device-store-syntax-20261112.json` records actual Xtensa syntax success.
These are production-code host tests and syntax checks, not real flash power-loss
or multicore scheduling validation. The reproducible runner is
`python overlay-apps/components/hosino_console_auth_store/tests/run_tests.py`
from the workspace (existing local TCC dependency retained).
