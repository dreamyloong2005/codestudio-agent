"""Local native Seatbelt characterization; creates no persistent system settings."""
from pathlib import Path
import datetime, hashlib, json, socket, subprocess
ROOT=Path(__file__).resolve().parent
results=[]
listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(8)
port=listener.getsockname()[1]
args=[str(ROOT/'package/probe'),str(ROOT/'package/input'),str(ROOT/'scratch/output'),str(ROOT/'host/sentinel'),str(port)]
for name,command in [('unrestricted',args),('deny_default',['/usr/bin/sandbox-exec','-f',str(ROOT/'root.sb'),*args])]:
 (ROOT/'package/input').write_text('readonly package input');(ROOT/'host/sentinel').write_text('host sentinel');(ROOT/'scratch/output').unlink(missing_ok=True)
 p=subprocess.run(command,capture_output=True,text=True,timeout=10)
 entry={'name':name,'command':command,'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr,'host_after':(ROOT/'host/sentinel').read_text(),'package_after':(ROOT/'package/input').read_text(),'scratch_created':(ROOT/'scratch/output').exists()}
 assert p.returncode==0,entry
 if name=='deny_default':
  for line in ['dependency value=37','package_read ok=1 errno=0','package_write ok=0 errno=1','scratch_write ok=1 errno=0','host_read ok=0 errno=1','host_write ok=0 errno=1','network_connect ok=0 errno=1','fork ok=0 errno=1','foreign_exec ok=0 errno=1']:assert line in p.stdout,entry
  assert entry['host_after']=='host sentinel' and entry['package_after']=='readonly package input',entry
 else:
  for line in ['package_read ok=1 errno=0','package_write ok=1 errno=0','scratch_write ok=1 errno=0','host_read ok=1 errno=0','host_write ok=1 errno=0','network_connect ok=1 errno=0','fork ok=1 errno=0']:assert line in p.stdout,entry
 results.append(entry)
listener.close()
for name in ['groups_plain','groups_deny_syscalls','groups_deny_spawn','groups_deny_fork']:
 command=['/usr/bin/sandbox-exec','-f',str(ROOT/(name+'.sb')),str(ROOT/'package/groups')]
 p=subprocess.run(command,capture_output=True,text=True,timeout=10)
 entry={'name':name,'command':command,'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
 assert p.returncode==0,entry
 if name=='groups_deny_syscalls':assert 'child setsid=-1 errno=1' in p.stdout and 'spawn newgroup result=0' in p.stdout,entry
 if name in ('groups_deny_spawn','groups_deny_fork'):assert 'spawn newgroup result=1' in p.stdout,entry
 results.append(entry)
report={'created_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'platform':subprocess.check_output(['sw_vers','-productVersion'],text=True).strip(),'arch':subprocess.check_output(['uname','-m'],text=True).strip(),'status':'characterization_passed_not_stage3_acceptance','scope':'Seatbelt deny-default file/network/process tests and process-group escape characterization','results':results,'profiles_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in ROOT.glob('*.sb')},'limitations':['sandbox-exec and sandbox.h are deprecated; custom SBPL is not a supported public API contract','Unrestricted process-fork permits setsid and posix_spawn process-group escape','Denying setsid/setpgid syscalls alone does not block posix_spawn process-group escape','CPU/memory/process-count aggregate limits and host-death containment are not proved by these probes']}
(ROOT/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'status':report['status'],'cases':len(results),'report':str(ROOT/'verification.json')},indent=2))
