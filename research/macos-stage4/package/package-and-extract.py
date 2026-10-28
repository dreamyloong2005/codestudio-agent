from pathlib import Path, PurePosixPath
from datetime import datetime, timezone
import argparse, hashlib, json, subprocess, sys, tarfile
root=Path(__file__).resolve().parents[2]
work=Path(__file__).resolve().parent
parser=argparse.ArgumentParser();parser.add_argument('--verification',type=Path,required=True);args=parser.parse_args()
receipt={'started_at':datetime.now(timezone.utc).isoformat(),'status':'failed','scope':'Final native macOS development archive, not publisher-verified or notarized','runs':[]}
try:
 for index in (1,2):
  command=[sys.executable,'-B',str(root/'scripts/package.py'),'--mode','release','--verification',str(args.verification),'--output',str(work/f'package-{index}')]
  run=subprocess.run(command,cwd=root,capture_output=True,text=True,timeout=120)
  (work/f'package-{index}.log').write_text(run.stdout+run.stderr)
  if run.returncode:raise RuntimeError(f'Packaging {index} failed: see log')
  result=json.loads(run.stdout)
  receipt['runs'].append({'command':command,'returncode':run.returncode,'result':result})
 first,second=[Path(item['result']['archive']) for item in receipt['runs']]
 hash_file=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
 if hash_file(first)!=hash_file(second):raise RuntimeError('Repeated native archives are not byte identical')
 output=work/'extracted';output.mkdir(exist_ok=False)
 with tarfile.open(first,'r:gz') as archive:
  members=archive.getmembers();names=set()
  for member in members:
   name=PurePosixPath(member.name)
   if not member.isfile() or name.is_absolute() or '..' in name.parts or member.name in names:
    raise RuntimeError('Unexpected archive member')
   names.add(member.name)
  archive.extractall(output,filter='data')
 receipt.update(status='succeeded',archive_sha256=hash_file(first),archive_size=first.stat().st_size,
                repeated_archive_byte_identical=True,extracted=str(output),archive_members=len(names),
                source_verification=str(args.verification),signing='ad_hoc_not_publisher_verified',notarized=False)
finally:
 receipt['finished_at']=datetime.now(timezone.utc).isoformat()
 (work/'packaging.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({'status':receipt['status'],'archive_sha256':receipt['archive_sha256'],'extracted':receipt['extracted']}))
