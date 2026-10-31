import shutil,json,hashlib
from pathlib import Path
root=Path(__file__).resolve().parents[2]
mirror=Path('/var/tmp/codestudio-agent-linux')
assert (mirror/'.codestudio-build-mirror').read_text()==str(root)
relative=Path('research/durable-requests')
copied=[]
for source in sorted((mirror/relative).rglob('*')):
    if not source.is_file() or source.suffix not in ('.json','.log'): continue
    assert not source.is_symlink()
    destination=root/source.relative_to(mirror)
    if destination.exists(): assert destination.read_bytes()==source.read_bytes()
    else:
        destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(source,destination)
    copied.append({'path':destination.relative_to(root).as_posix(),'sha256':hashlib.sha256(destination.read_bytes()).hexdigest()})
(root/relative/'linux-evidence-copy.json').write_text(json.dumps(copied,indent=2)+'\n',encoding='utf-8')
print('Copied Linux evidence files:',len(copied))
