from pathlib import Path
import json,re,subprocess
root=Path(__file__).resolve().parent
results=[]
for mode in ['plain','group','session']:
 for name,flags in [('unrestricted',[]),('deny_fork',['/usr/bin/sandbox-exec','-f',str(root/'setexec_deny_fork.sb')]),('deny_setsid',['/usr/bin/sandbox-exec','-f',str(root/'setexec_deny_setsid.sb')]),('deny_spawn',['/usr/bin/sandbox-exec','-f',str(root/'setexec_deny_spawn.sb')])]:
  p=subprocess.run([*flags,str(root/'package/setexec'),mode],capture_output=True,text=True,timeout=5)
  result={'name':name,'mode':mode,'exit':p.returncode,'out':p.stdout,'err':p.stderr};assert p.returncode==0,result
  if name=='deny_spawn':assert 'setexec result=1' in p.stdout,result
  else:
   before=re.search(r'before_setexec pid=(\d+) pgid=(\d+) sid=(\d+)',p.stdout)
   after=re.search(r'after_setexec pid=(\d+) pgid=(\d+) sid=(\d+)',p.stdout)
   assert before and after and before[1]==after[1],result
   if mode in ('group','session'):assert after[1]==after[2],result
   if mode=='session':assert after[1]==after[3],result
  results.append(result)
(root/'setexec.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps({'status':'characterization_passed','cases':len(results),'report':str(root/'setexec.json')},indent=2))
