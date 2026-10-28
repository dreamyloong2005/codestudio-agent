from pathlib import Path
import json,subprocess,sys,time
root=Path(__file__).resolve().parent
ready=root/'scratch/guardian-crash-ready';report=root/'host/guardian-crash-report.json'
ready.unlink(missing_ok=True);report.unlink(missing_ok=True)
script='''import subprocess,os,sys
p=subprocess.Popen([sys.argv[1],str(os.getpid()),*sys.argv[2:]])
print("guardian="+str(p.pid),flush=True)
assert sys.stdin.readline().strip()=="crash"
p.kill();p.wait(timeout=5)
print("guardian_crashed",flush=True)
sys.stdin.readline()
'''
host=subprocess.Popen([sys.executable,'-c',script,str(root/'host/guardian'),str(root/'guardian.sb'),str(root/'package/guarded'),str(ready),str(report)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
 first,second=host.stdout.readline().strip(),host.stdout.readline().strip();lines=[first,second]
 child=int(next(x for x in lines if x.isdigit()));guardian=int(next(x for x in lines if x.startswith('guardian=')).split('=')[1]);deadline=time.monotonic()+5
 while not ready.exists() and time.monotonic()<deadline:time.sleep(.01)
 initial=json.loads(ready.read_text());assert initial['pid']==child,initial
 from native_process_watch import ProcessWatch
 watch=ProcessWatch(child);assert watch.executable==root/'package/guarded'
 host.stdin.write('crash\n');host.stdin.flush();assert host.stdout.readline().strip()=='guardian_crashed'
 time.sleep(.15);survived=watch.alive();assert survived
 deadline=time.monotonic()+12
 while watch.alive() and time.monotonic()<deadline:time.sleep(.05)
 assert not watch.alive();watch.close();assert not report.exists()
 host.stdin.close();host.stdin=None;output,error=host.communicate(timeout=5);assert host.returncode==0 and not error,(output,error)
 result={'case':'guardian_crash','guardian_pid':guardian,'child_pid':child,'child_survived_guardian_crash':survived,'child_self_terminated':True,'guardian_report_absent':True,'conclusion':'A user-space guardian cannot by itself provide kernel-owned lifetime containment when the guardian also fails.'}
 (root/'guardian-crash.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
finally:
 if host.poll() is None:host.kill();host.wait(timeout=5)
 if host.stdin:host.stdin.close()
 host.stdout.close();host.stderr.close()
