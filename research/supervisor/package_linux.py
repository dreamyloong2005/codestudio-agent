"""Refresh the local Linux delivery after the verified supervision milestone."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

root = Path(__file__).resolve().parents[2]
mirror = Path('/var/tmp/codestudio-agent-linux')
delivery = root / 'build/linux/x86_64/release'
source = mirror / 'build/linux/x86_64/release/codestudio-agent'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

for directory in ('include', 'src', 'tests', 'scripts', 'protocol'):
    for path in (root / directory).rglob('*'):
        if path.is_file() and '__pycache__' not in path.parts:
            assert path.read_bytes() == (mirror / path.relative_to(root)).read_bytes(), path
assert (root / 'xmake.lua').read_bytes() == (mirror / 'xmake.lua').read_bytes()
shutil.copyfile(source, delivery / 'codestudio-agent')
for name in ('protocol/task-service.schema.json', 'examples/task_client.py'):
    target = delivery / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / name, target)
readme = delivery / 'README.txt'
text = readme.read_text(encoding='utf-8').split('\nBounded worker supervision update')[0].rstrip()
text += '''

Bounded worker supervision update (2026-10-24)

Explicit foreground supervision:
  ./codestudio-agent scheduler-supervise --workspace /existing/workspace --max-restarts 3 --restart-delay-ms 1000
Explicit detached supervision:
  ./codestudio-agent scheduler-start --supervise --workspace /existing/workspace
Stop either with scheduler-stop for the same workspace/state database.
The restart cap is per supervisor lifetime, with exponential backoff up to 60s.
Only registered abnormal child exits restart; task grants/budgets remain fixed
and unknown tool effects block recovery. The supervisor itself is not restarted.
Autostart registrations still run an ordinary worker. scheduler-status is the
last worker record and is not proof the supervisor is alive.
Schema 6 and protocol 2 remain current. New protected lock paths participate in
exact resume bindings; mismatched pre-update checkpoints are rejected, not migrated.
'''
readme.write_text(text, encoding='utf-8')
packages = subprocess.check_output(['pacman', '-Q', 'glibc', 'gcc-libs', 'curl', 'sqlite', 'openssl', 'bubblewrap', 'tzdata'], text=True)
libraries = subprocess.check_output(['ldd', str(source)], text=True)
(delivery / 'runtime-dependencies.txt').write_text('Build-host packages:\n' + packages + '\nBuild-host dynamic resolution:\n' + libraries, encoding='utf-8')
names = ['codestudio-agent', 'README.txt', 'runtime-dependencies.txt', 'protocol/task-service.schema.json', 'examples/task_client.py']
(delivery / 'SHA256SUMS').write_text(''.join(f'{digest(delivery / name)}  {name}\n' for name in names), encoding='utf-8')
archive = delivery / 'codestudio-agent-linux-x86_64.tar.gz'
with tarfile.open(archive, 'w:gz') as bundle:
    for name in [*names, 'SHA256SUMS']:
        item = bundle.gettarinfo(str(delivery / name), arcname=name)
        item.mode = 0o755 if name == 'codestudio-agent' else 0o644
        item.uid = item.gid = 0
        item.uname = item.gname = ''
        with (delivery / name).open('rb') as contents:
            bundle.addfile(item, contents)
with tarfile.open(archive) as bundle:
    assert set(bundle.getnames()) == {*names, 'SHA256SUMS'}
    for name in [*names, 'SHA256SUMS']:
        assert bundle.extractfile(name).read() == (delivery / name).read_bytes()
    assert bundle.getmember('codestudio-agent').mode == 0o755
assert digest(source) == digest(delivery / 'codestudio-agent')
with tempfile.TemporaryDirectory(prefix='codestudio-supervisor-delivery-') as workspace:
    requests = [{'jsonrpc':'2.0', 'id':1, 'method':'initialize', 'params':{'protocol_version':2}},
                {'jsonrpc':'2.0', 'id':2, 'method':'shutdown'}]
    result = subprocess.run([str(delivery / 'codestudio-agent'), 'serve', '--workspace', workspace],
                            input=''.join(json.dumps(frame)+'\n' for frame in requests), text=True, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
    frames = [json.loads(line) for line in result.stdout.splitlines()]
    initialized = next(frame['result'] for frame in frames if frame.get('id') == 1)
    assert initialized['capabilities']['worker_supervision'] is True
    assert not (Path(workspace) / '.codestudio').exists()
receipt = {'date':'2026-10-24', 'source_tests_and_schema_match_tested_mirror':True,
           'native_copy_and_archive_match':True, 'archive_checksums_verified':True,
           'delivered_initialization':initialized}
for key, path in [('linux_release', delivery / 'codestudio-agent'),
                  ('windows_release', root / 'build/windows/x64/release/codestudio-agent.exe'),
                  ('archive', archive), ('schema', root / 'protocol/task-service.schema.json')]:
    receipt[key] = {'path':str(path.relative_to(root)), 'bytes':path.stat().st_size, 'sha256':digest(path)}
(root / 'research/supervisor/delivery.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
print(json.dumps({key:receipt[key] for key in ('linux_release','windows_release','archive','schema')}, indent=2))
