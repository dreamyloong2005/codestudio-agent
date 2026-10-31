"""Package twice and verify a fresh native archive extraction without installing.

Uses only the repository packager/installer. Keeps all outputs in build and
writes a new evidence receipt. No deletion or external publication.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = Path(__file__).resolve().parent


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verification', required=True, type=Path)
    parser.add_argument('--final', action='store_true')
    args = parser.parse_args()
    verification = json.loads(args.verification.read_text())
    target = 'windows' if sys.platform == 'win32' else 'linux'
    assert verification['platform'] == target and verification['mode'] == 'release'
    assert verification['status'] == 'succeeded'
    spec = importlib.util.spec_from_file_location('owned_installer', ROOT / 'scripts/install.py')
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    from_scripts = str(ROOT / 'scripts')
    sys.path.insert(0, from_scripts)
    import verify
    assert verify.source_fingerprint() == verification['source']
    destination = ROOT / 'build/cross-platform-regression' / target
    destination.mkdir(parents=True, exist_ok=True)
    suffix = '-final' if args.final else ''
    receipt_path = EVIDENCE / ('archive-' + target + suffix + '.json')
    assert not receipt_path.exists()
    outputs = []
    environment = os.environ.copy()
    if target == 'linux' and os.geteuid() == 0:
        environment['XMAKE_ROOT'] = 'y'
    for extra in ([], ['--output', str(destination / ('package-repeat' + suffix))]):
        command = [sys.executable, '-B', 'scripts/package.py', '--mode', 'release',
                   '--verification', str(args.verification.resolve()), *extra]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, env=environment)
        (EVIDENCE / f'packaging-{target}{suffix}-{len(outputs)+1}.log').write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr[-2000:]
        document = json.loads(result.stdout)
        assert document['status'] == 'packaged'
        assert not document.get('retained_path') and not document.get('lock_cleanup')
        outputs.append(document)
    assert outputs[0]['sha256'] == outputs[1]['sha256']
    archive = Path(outputs[0]['archive'])
    package_name = Path(outputs[0]['package']).name
    extraction = destination / ('archive-extracted' + suffix)
    extraction.mkdir(exist_ok=False)
    extracted = extraction / package_name
    extracted.mkdir()

    def member(name):
        path = PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts and '\\' not in name
        assert path.parts
        return extracted.joinpath(*path.parts)

    members = []
    if target == 'windows':
        with zipfile.ZipFile(archive) as source:
            assert source.testzip() is None
            assert len(source.namelist()) == len(set(source.namelist()))
            for item in source.infolist():
                path = member(item.filename)
                if item.is_dir():
                    path.mkdir(parents=True, exist_ok=True)
                    continue
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open('xb') as output:
                    output.write(source.read(item))
                members.append(item.filename)
    else:
        with tarfile.open(archive) as source:
            names = source.getnames()
            assert len(names) == len(set(names))
            for item in source.getmembers():
                path = member(item.name)
                assert item.isfile() or item.isdir()
                if item.isdir():
                    path.mkdir(parents=True, exist_ok=True)
                    continue
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open('xb') as output, source.extractfile(item) as payload:
                    output.write(payload.read())
                path.chmod(item.mode)
                assert (path.stat().st_mode & 0o777) == (item.mode & 0o777)
                members.append(item.name)
    original = Path(outputs[0]['package'])
    verified = installer.verify_package(extracted)
    assert verified['package_sha256'] == installer.verify_package(original)['package_sha256']
    original_files = {p.relative_to(original).as_posix(): digest(p) for p in original.rglob('*') if p.is_file()}
    extracted_files = {p.relative_to(extracted).as_posix(): digest(p) for p in extracted.rglob('*') if p.is_file()}
    assert original_files == extracted_files
    binary = extracted / ('codestudio-agent.exe' if target == 'windows' else 'codestudio-agent')
    assert digest(binary) == verification['artifact']['sha256']
    doctor = subprocess.run([str(binary), 'doctor'], capture_output=True, text=True, check=True)
    receipt = {'format': 2, 'platform': target, 'status': 'succeeded',
               'generated_at': datetime.now(timezone.utc).isoformat(),
               'source': verification['source'], 'verification': str(args.verification),
               'packages': outputs, 'archive_sha256': digest(archive), 'repeat_archive_identical': True,
               'fresh_extraction': str(extracted), 'file_count': len(extracted_files),
               'archive_members': members, 'files': extracted_files,
               'package_manifest_sha256': verified['package_sha256'], 'doctor': json.loads(doctor.stdout),
               'installation_verification': 'pending_separate_native_suite'}
    with receipt_path.open('x', encoding='utf-8') as output:
        json.dump(receipt, output, indent=2)
        output.write('\n')
    print(json.dumps({'receipt': str(receipt_path), 'package': str(extracted), 'sha256': digest(archive)}))


if __name__ == '__main__':
    main()
