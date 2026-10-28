#!/usr/bin/env python3
"""Rebuild local Seatbelt characterization fixtures and run the bounded probes.

Only the chosen fresh work directory receives executables, scratch files and
reports. No product build cache, OS configuration, user credentials or unrelated
process is modified. Negative probes deliberately demonstrate containment gaps;
their successful assertions do not mean macOS stage 3 is accepted.
"""
import argparse
from datetime import datetime,timezone
import hashlib,json,shutil,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--work-dir',type=Path,required=True)
args=parser.parse_args()
if sys.platform!='darwin':raise SystemExit('This characterization requires native macOS.')
work=args.work_dir.resolve()
if work.exists():raise SystemExit('Use a new work directory; existing evidence is not overwritten.')
work.mkdir(parents=True,mode=0o700)
for directory in ['host','package','scratch']:(work/directory).mkdir(mode=0o700)
for pattern in ['*.c','*.py']:
 for path in source.glob(pattern):
  if path.name!='reproduce.py':shutil.copyfile(path,work/path.name)
for path in source.glob('*.sb'):
 (work/path.name).write_text(path.read_text().replace('@PROBE_ROOT@',str(work)))
commands=[['xcrun','clang','-dynamiclib',str(work/'dependency.c'),'-Wl,-install_name,@loader_path/libdependency.dylib','-o',str(work/'package/libdependency.dylib')]]
for name in ['probe','groups','setexec','orphan','ipc','ipc_target','guardian','guarded']:
 target='host' if name in ('ipc_target','guardian') else 'package'
 command=['xcrun','clang','-O2',str(work/(name+'.c'))]
 if name=='probe':command+=['-L'+str(work/'package'),'-ldependency']
 commands.append([*command,'-o',str(work/target/name)])
scripts=['run_probes','setexec_probe','ipc_probe','host_crash_probe','guardian_probe','guardian_crash_probe']
commands += [[sys.executable,str(work/(name+'.py'))] for name in scripts]
receipt={'status':'not_passed','started_at':datetime.now(timezone.utc).isoformat(),'scope':'Seatbelt characterization, including expected containment failures','source_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir() if p.suffix in ('.c','.py','.sb')},'steps':[]}
try:
 for index,command in enumerate(commands):
  result=subprocess.run(command,capture_output=True,text=True,timeout=30)
  log=f'{index:02}.log';(work/log).write_text(result.stdout+result.stderr)
  receipt['steps'].append({'command':command,'returncode':result.returncode,'log':log})
  if result.returncode:raise RuntimeError(f'Probe step {index} failed; see {work/log}')
 receipt['status']='characterization_passed_not_stage3_acceptance'
finally:
 receipt['finished_at']=datetime.now(timezone.utc).isoformat()
 (work/'reproduction.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({'status':receipt['status'],'report':str(work/'reproduction.json')},indent=2))
