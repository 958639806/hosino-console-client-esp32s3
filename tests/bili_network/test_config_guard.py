"""Compare this narrow candidate against the exact observed 4254 inputs."""
from pathlib import Path
import hashlib,json,re,zipfile

ROOT=Path(__file__).resolve().parents[2]
IDF=Path('C:/Users/95863/.codex/builds/hosino-esp32-qspi/esp-idf-v5.5.5')
baseline=ROOT/'evidence/apps-v23-touch-release-observed'
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def config(text):
 result=dict(re.findall(r'^(CONFIG_\w+)=(.*)$',text,re.M))
 result.update({k:'n' for k in re.findall(r'^# (CONFIG_\w+) is not set$',text,re.M)})
 return result

defaults=ROOT/'overlay-apps/sdkconfig.apps.defaults'
old=config((baseline/'80mhz/sdkconfig').read_text('utf-8'))
new=config(defaults.read_text('utf-8'))
changed={k:{'old':old.get(k),'new':v} for k,v in new.items() if old.get(k)!=v}
assert changed=={'CONFIG_LWIP_TCP_WND_DEFAULT':{'old':'5760','new':'32768'},'CONFIG_LWIP_TCP_RECVMBOX_SIZE':{'old':'6','new':'24'}},changed
source_differences=[]
with zipfile.ZipFile(baseline/'staged-source-inputs.zip') as archive:
 for name in archive.namelist():
  path=ROOT/'overlay-apps'/name
  assert path.is_file(),name
  if path.read_bytes()!=archive.read(name): source_differences.append(name)
 assert len(archive.namelist())==397
assert source_differences==['main/hosino_bili_media.cpp'],source_differences
assert new['CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL']=='256'
assert new['CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL']=='98304'
assert 'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP' not in new
assert old['CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP']=='n'
assert 32768//int(old['CONFIG_LWIP_TCP_MSS'])+2==24
kconfig=(IDF/'components/lwip/Kconfig').read_text('utf-8')
assert 'range 2440 65535 if !LWIP_WND_SCALE' in kconfig
assert 'range 6 64 if !LWIP_WND_SCALE' in kconfig
assert 'LWIP_TCP_WND_DEFAULT/TCP_MSS + 2' in kconfig
main=(ROOT/'overlay-apps/main/CMakeLists.txt').read_text('utf-8')
assert not re.search(r'^\s*(?:PRIV_)?REQUIRES\b',main,re.M)
source=ROOT/'overlay-apps/main/hosino_bili_media.cpp'
policy=ROOT/'overlay-apps/main/hosino_bili_wifi_policy.h'
report={'passed':True,'scope':'Configuration and complete 397 observed-source comparison; no build, runtime throughput, heap, USB or BLE performance claim.',
 'baseline_combined_sha256':'4254eb8dfb01764f4668f4d7dca3306904e69a129ed3511a8f09b9894839ce39',
 'configuration_differences':changed,'baseline_inputs_compared':397,'source_differences':source_differences,
 'new_source':'main/hosino_bili_wifi_policy.h','production_hashes':{str(p.relative_to(ROOT)):digest(p) for p in (source,policy,defaults)},
 'mailbox_extra_pointer_bytes_per_socket':(24-6)*4,'extra_rx_window_capacity_bytes':32768-5760,
 'runtime_limits':['Receive payload residency grows dynamically and may fall back to internal heap.','Existing >256-byte default malloc prefers PSRAM; explicit driver internal allocations retain their constraints.','Actual internal heap low-water and repeated stream stop/start require hardware validation.','WiFi PS_NONE retains configured BLE coexistence time slicing; BLE performance and power consumption are not validated here.']}
out=ROOT/'evidence/bili-network-window-20261115/config-guard.json'
out.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
print(json.dumps(report,ensure_ascii=False))
