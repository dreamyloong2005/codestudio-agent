from pathlib import Path
import json,os,subprocess,sys,time
root=Path(__file__).resolve().parent
ready=root/'scratch/orphan-ready';done=root/'scratch/orphan-done'
ready.unlink(missing_ok=True);done.unlink(missing_ok=True)
script='''import subprocess,os,sys,json,time
p=subprocess.Popen(sys.argv[1:],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
print(json.dumps({"child_pid":p.pid}),flush=True)
os._exit(73)
'''
command=['/usr/bin/sandbox-exec','-f',str(root/'orphan.sb'),str(root/'package/orphan'),str(ready),str(done)]
host=subprocess.run([sys.executable,'-c',script,*command],capture_output=True,text=True,timeout=5)
assert host.returncode==73,(host.stdout,host.stderr)
start=time.monotonic()
while not ready.exists() and time.monotonic()-start<5:time.sleep(.01)
from native_process_watch import ProcessWatch
worker=ProcessWatch(json.loads(host.stdout)['child_pid'])
descendant=ProcessWatch(int(ready.read_text()))
assert worker.executable==root/'package/orphan' and descendant.executable==root/'package/orphan'
assert worker.alive() and descendant.alive()
while (worker.alive() or descendant.alive()) and time.monotonic()-start<8:time.sleep(.05)
assert not worker.alive() and not descendant.alive()
worker.close();descendant.close()
assert done.read_text().startswith('survived host exit')
result={'case':'host_crash_with_permitted_descendant','host_exit_code':host.returncode,'original_child':json.loads(host.stdout),'escaped_child_pid':int(ready.read_text()),'survived_host_exit':True,'descendant_self_terminated':True,'intent':'negative evidence: sandbox-exec and POSIX groups alone do not bind descendant lifetime to host'}
(root/'host-crash.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
