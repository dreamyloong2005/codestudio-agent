"""Copy this run's Linux evidence and verified delivery bytes without deleting."""
import hashlib
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
MIRROR = Path('/var/tmp/codestudio-agent-linux')
EVIDENCE = Path('research/cross-platform-regression')
SUFFIX = '-final' if '--final' in sys.argv[1:] else ''
assert (MIRROR / '.codestudio-build-mirror').read_text() == str(ROOT)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


copied = []
for source in sorted((MIRROR / EVIDENCE).rglob('*')):
    if not source.is_file() or source.suffix not in ('.json', '.log'):
        continue
    assert not source.is_symlink()
    relative = source.relative_to(MIRROR)
    destination = ROOT / relative
    if destination.exists():
        assert destination.is_file() and not destination.is_symlink()
        assert source.read_bytes() == destination.read_bytes(), str(relative)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
    copied.append(relative.as_posix())

archive_receipt = json.loads((MIRROR / EVIDENCE / ('archive-linux' + SUFFIX + '.json')).read_text())
assert archive_receipt['status'] == 'succeeded'
package = archive_receipt['packages'][0]
sources = [Path(package['archive']), Path(package['package']) / 'codestudio-agent']
destination_root = ROOT / 'build/linux/x86_64/release'
destination_root.mkdir(parents=True, exist_ok=True)
assert destination_root.resolve().is_relative_to((ROOT / 'build').resolve())
delivered = []
for source in sources:
    assert source.resolve().is_relative_to(MIRROR) and source.is_file() and not source.is_symlink()
    destination = destination_root / source.name
    assert not destination.is_symlink()
    if destination.exists():
        assert destination.is_file()
    shutil.copyfile(source, destination)
    assert digest(source) == digest(destination)
    delivered.append({'source': str(source), 'path': destination.relative_to(ROOT).as_posix(),
                      'sha256': digest(destination), 'bytes': destination.stat().st_size})
receipt = {'format': 2, 'status': 'succeeded', 'evidence_files': copied, 'delivered': delivered,
           'scope': 'Copied owned Linux receipts and named native delivery artifacts; no deletion or installation.'}
with (ROOT / EVIDENCE / ('linux-transfer' + SUFFIX + '.json')).open('x') as output:
    json.dump(receipt, output, indent=2)
    output.write('\n')
print(json.dumps({'evidence_files': len(copied), 'delivered': delivered}))
