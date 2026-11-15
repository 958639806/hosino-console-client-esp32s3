# BLE settings service

This local service uses the ESP-IDF 5.5.5 bundled NimBLE public API. Lifecycle
ordering is based on `examples/bluetooth/nimble/blecent/main/main.c` and verified
against `components/bt/host/nimble/nimble/porting/nimble/src/nimble_port.c`.
The upstream NimBLE implementation is Apache-2.0; new local files are Apache-2.0.
No upstream Bluetooth implementation is copied or modified.

## Integration

Add `ble_settings` to the Settings component's `REQUIRES`. The service depends
on IDF components `bt`, `nvs_flash`, and `esp_timer`; it has no registry dependency.
Add the `BluetoothPage` entry to Settings navigation, and forward Settings
pause/resume to the active page. Closing or pausing the page cancels discovery;
only the user's switch controls whether the idle BLE host remains enabled.

Required configuration:

```text
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
# CONFIG_BT_BLUEDROID_ENABLED is not set
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
# CONFIG_BT_NIMBLE_ROLE_CENTRAL is not set
# CONFIG_BT_NIMBLE_ROLE_PERIPHERAL is not set
# CONFIG_BT_NIMBLE_ROLE_BROADCASTER is not set
# CONFIG_BT_NIMBLE_SECURITY_ENABLE is not set
# CONFIG_BT_NIMBLE_EXT_ADV is not set
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
CONFIG_BT_NIMBLE_PINNED_TO_CORE_0=y
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
```

`CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y` is an optional memory optimization in this
IDF version. External allocation applies to host dynamic storage. The controller
and FreeRTOS task stacks still require internal memory.

## Behavior and ownership

- Starts disabled, without initializing the host or controller at boot.
- Lazily starts one 4 KiB service task. It owns all init, scan, cancellation,
  stop and deinit calls and remains idle after the radio is disabled.
- Uses its own checked host-task creation because IDF's void
  `nimble_port_freertos_init` does not report task allocation failure.
- Disabling waits for `nimble_port_stop` success and a host-exited signal before
  deinitializing. The host makes no NimBLE calls after signaling. Uncertain stop
  or deinit failures retain resources and report that a restart is needed.
- `nimble_port_stop` itself has unbounded internal waits. Snapshot reads observe
  a five-second overall shutdown deadline and report an error without killing
  tasks or releasing resources. If shutdown subsequently completes and deinit
  succeeds, the worker clears this uncertainty. Host reset/sync callbacks cannot
  overwrite a shutdown timeout. The two-second exit-signal wait is only a join
  check after `nimble_port_stop` returns, not a bound on the stop API itself.
- NVS initialization failures allow a later retry. A failed `nimble_port_init`
  requires a restart because its partial host/NPL allocations may survive the
  SDK's controller rollback; the service does not reinitialize uncertain state.
- No BLE callback refers to the page or to LVGL. The page polls a bounded snapshot
  every 300 ms, deletes that timer when closed, and pauses it in the background.
- Active legacy discovery lasts at most 10 seconds, at 100 ms intervals with a
  30 ms scan window. It requests scan-response names and deduplicates by address
  plus address type. At most 20 devices are retained; RSSI is the latest report.
- Advertisement names are length-limited and validated as UTF-8. Actual display
  coverage depends on the configured UI font's glyph set.
- No pairing, connections, advertising, NVS erasure, classic Bluetooth audio,
  or network-sharing implementation is claimed. No observed device is contacted
  beyond standard BLE active-scan requests.

Hardware behavior and repeated switching require device validation after the
NimBLE-enabled firmware is built. A successful syntax check alone does not verify
radio coexistence, available internal RAM, or the attached environment's devices.
