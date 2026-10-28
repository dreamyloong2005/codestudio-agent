from pathlib import Path
import json,os,signal,socket,subprocess,time,uuid
root=Path(__file__).resolve().parent
profile=(root/'root.sb').read_text().replace(str(root/'package/probe'),str(root/'package/ipc'))
(root/'ipc.sb').write_text(profile)
name=uuid.uuid4().hex[:12];service='com.codestudio.stage3.'+name;shm='/cs'+name
# Unix socket paths are short and live only in our private fixture directory.
unix=str(root/'host/sock');Path(unix).unlink(missing_ok=True);listener=socket.socket(socket.AF_UNIX);listener.bind(unix);listener.listen(8)
target=subprocess.Popen([str(root/'host/ipc_target'),service,shm],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
 ready=target.stdout.readline().split();assert ready==[str(target.pid),'0'],ready
 results=[]
 for label,command in [('unrestricted',[]),('deny_default',['/usr/bin/sandbox-exec','-f',str(root/'ipc.sb')])]:
  p=subprocess.run([*command,str(root/'package/ipc'),str(target.pid),shm,service,unix],capture_output=True,text=True,timeout=5)
  record={'case':label,'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr};assert p.returncode==0,record
  for fragment in (['signal_delivery result=0 errno=0','task_name_for_pid result=0','mach_lookup result=0','shared_memory result=1 errno=0','unix_connect result=0 errno=0'] if label=='unrestricted' else ['signal_delivery result=-1 errno=1','task_name_for_pid result=5 port=0','mach_lookup result=1100 port=0','shared_memory result=0 errno=1','unix_connect result=-1 errno=1']):assert fragment in p.stdout,record
  results.append(record)
 report={'status':'passed','results':results,'task_for_pid_note':'task_for_pid is denied in both cases by host OS policy; this probe does not attribute that baseline refusal to Seatbelt.'}
 (root/'ipc.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
finally:
 target.terminate();target.wait(timeout=5);target.stdout.close();target.stderr.close();listener.close();Path(unix).unlink(missing_ok=True)
