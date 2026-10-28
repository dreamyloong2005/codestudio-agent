from pathlib import Path
import json,subprocess,sys,time
root=Path(__file__).resolve().parent
(root/'guardian.sb').write_text((root/'root.sb').read_text().replace(str(root/'package/probe'),str(root/'package/guarded')))
ready=root/'scratch/guarded-ready';report=root/'host/guardian-report.json'
ready.unlink(missing_ok=True);report.unlink(missing_ok=True)
script='''import subprocess,os,sys,time
p=subprocess.Popen([sys.argv[1],str(os.getpid()),*sys.argv[2:]])
time.sleep(20)
'''
host=subprocess.Popen([sys.executable,'-c',script,str(root/'host/guardian'),str(root/'guardian.sb'),str(root/'package/guarded'),str(ready),str(report)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
 child=int(host.stdout.readline());deadline=time.monotonic()+5
 while not ready.exists() and time.monotonic()<deadline:time.sleep(.01)
 initial=json.loads(ready.read_text());assert initial['pid']==initial['pgid']==initial['sid']==child,initial
 host.kill();assert host.wait(timeout=5)==-9
 deadline=time.monotonic()+5
 while not report.exists() and time.monotonic()<deadline:time.sleep(.01)
 result=json.loads(report.read_text());assert result['host_exited'] and result['child_pid']==child and result['child_signal']==9,result
 output,error=host.communicate(timeout=5);assert not error,(output,error)
 result['initial']=initial;result['status']='passed_host_exit_only';result['limitation']='This trusted guardian handles the original host death but is not a kernel lifetime boundary; killing/crashing the guardian itself is not covered.'
 (root/'guardian.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
finally:
 if host.poll() is None:host.kill();host.wait(timeout=5)
 host.stdout.close();host.stderr.close()
