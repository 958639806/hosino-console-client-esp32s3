import json,hashlib,subprocess,tempfile,shutil
from pathlib import Path
root=Path(__file__).resolve().parents[4];c=root/'overlay-apps/components/hosino_console_auth_store';cc=root/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
rows=[]
with tempfile.TemporaryDirectory(prefix='hs-device-store-') as d:
 d=Path(d);shutil.copytree(c,d/'component');p=d/'component'
 for name,scenarios in [('test_auth_store',['','restore','corrupt','startup-cancel','retry']),('test_device_store',['','restore','corrupt','startup-forget','retry'])]:
  exe=d/(name+'.exe');cmd=[str(cc),'-DHOSINO_AUTH_STORE_HOST_TEST','-I'+str(p/'include'),'-I'+str(p/'tests/stubs'),'-o',str(exe),str(p/'tests'/f'{name}.c'),str(p/'hosino_console_auth_store.c')]
  r=subprocess.run(cmd,capture_output=True,text=True);assert r.returncode==0,r.stdout+r.stderr
  for scenario in scenarios:
   r=subprocess.run([str(exe)]+([scenario]if scenario else[]),capture_output=True,text=True)
   rows.append({'test':name,'scenario':scenario or 'main','exit_code':r.returncode,'output':r.stdout+r.stderr});print(name,scenario or 'main',r.returncode,r.stdout.strip());assert r.returncode==0
result={'passed':True,'source_sha256':hashlib.sha256((c/'hosino_console_auth_store.c').read_bytes()).hexdigest(),'header_sha256':hashlib.sha256((c/'include/hosino_console_device_store.h').read_bytes()).hexdigest(),'tests':rows,'scope':'Real production C with fake NVS transaction commits and deterministic RTOS interleavings. Asserts NVS runs only in worker test steps and a single 4096B internal stack creation. No real flash/power-loss or RTOS cross-core validation.'}
(root/'evidence/device-store-host-20261112.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n','utf-8')
