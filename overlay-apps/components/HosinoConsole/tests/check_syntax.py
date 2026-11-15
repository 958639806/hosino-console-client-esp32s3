"""Isolated real Xtensa syntax check; no stage, build, serial or network writes."""
from pathlib import Path
import datetime, hashlib, json, re, shlex, shutil, subprocess
component=Path(__file__).resolve().parents[1]; root=component.parents[2]
scratch=Path('C:/esp/hosino-qspi/syntax-console-ui-v19-pair');scratch.mkdir(parents=True,exist_ok=True)
commands=Path('C:/esp/hosino-qspi/build-apps-80/compile_commands.json')
entry=next(e for e in json.loads(commands.read_text('utf-8')) if e['file'].replace(chr(92),'/').endswith('/HosinoConsole/HosinoConsole.cpp'))
header=component.parent/'hosino_console_client/include/hosino_console_client.hpp'
for source in [component/'HosinoConsole.cpp', component/'HosinoConsole.hpp',header]:shutil.copyfile(source,scratch/source.name)
args=shlex.split(entry['command']);args.pop(0);compiler=entry['command'].split(' ',1)[0].strip(chr(34));clean=[];i=0
while i<len(args):
 a=args[i]
 if a in ['-o','-c']:i+=2;continue
 if a.startswith('-fdiagnostics-color='):i+=1;continue
 clean.append(a);i+=1
args=['-I'+scratch.as_posix(),*clean,'-fdiagnostics-color=never','-fsyntax-only',(scratch/'HosinoConsole.cpp').as_posix()]
def quote(s):return json.dumps(s)
rsp=scratch/'ui.rsp';rsp.write_text('\n'.join(quote(a) for a in args)+'\n','utf-8')
result=subprocess.run([compiler,'@'+rsp.as_posix()],cwd=entry['directory'],capture_output=True)
log=scratch/'ui.log';log.write_bytes(result.stdout+result.stderr)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report={'exit_code':result.returncode,'diagnostic_bytes':log.stat().st_size,'diagnostic_log':str(log),'compiler':compiler,'compile_commands_sha256':sha(commands),'source_sha256':sha(component/'HosinoConsole.cpp'),'header_sha256':sha(component/'HosinoConsole.hpp'),'client_header_sha256':sha(header),'time_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Actual Xtensa syntax-only, current build flags and current overlay UI/client header in isolated ASCII scratch; no staging, linking, firmware or device validation.'}
(component/'tests/syntax-result.json').write_text(json.dumps(report,indent=2)+'\n','utf-8')
print(json.dumps(report));print(log.read_text('utf-8',errors='replace'))
raise SystemExit(result.returncode)
