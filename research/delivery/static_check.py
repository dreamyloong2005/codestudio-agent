"""Read-only final workspace/resource checks; writes only its JSON receipt."""
import ast
import json
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
mirror = Path('/var/tmp/codestudio-agent-linux')
receipt = root / 'research/delivery/final-static.json'
receipt.write_text('{}\n', encoding='utf-8')
python_files = [*root.glob('tests/*.py'), *root.glob('scripts/*.py'), *root.glob('examples/*.py'), *root.glob('research/delivery/*.py')]
for path in python_files:
    ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
subprocess.run(['bash', '-n', str(root / 'scripts/verify-linux.sh')], check=True)
links, missing = 0, []
for path in [root / 'README.md', *root.glob('docs/*.md'), *root.glob('protocol/*.md')]:
    for value in re.findall(r'\[[^\]]*\]\(([^)]+)\)', path.read_text(encoding='utf-8')):
        target = value.split('#', 1)[0]
        if not target or '://' in target or target.startswith('mailto:'):
            continue
        links += 1
        if not (path.parent / target).exists():
            missing.append(f'{path.relative_to(root)}: {target}')
processes = []
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():
        continue
    try:
        executable = (proc / 'exe').resolve(strict=True)
    except (OSError, RuntimeError):
        continue
    # Exclude the currently running Python validation environment itself.
    if executable.name.startswith(('codestudio-agent', 'agent-tests', 'process-fixture', 'mcp-fixture', 'build-fixture', 'sandbox-fixture', 'line-process-fixture', 'provider-fixture')) and (executable.is_relative_to(root) or executable.is_relative_to(mirror) or any(part.startswith('codestudio-delivery-') for part in executable.parts)):
        processes.append({'pid':int(proc.name), 'executable':str(executable)})
cgroups = [str(path) for path in Path('/sys/fs/cgroup').glob('codestudio-test-*')]
isolations = [str(path) for path in Path('/tmp').glob('codestudio-isolation-*')]
registrations = [str(path) for path in (Path.home() / '.config/systemd/user').glob('**/codestudio-agent-*.service')]
private_keyrings = [str(path) for path in Path('/tmp').glob('codestudio-secret-service-*')]
fixture_processes = []
for proc in Path('/proc').iterdir():
    if proc.name.isdigit():
        try:
            executable = (proc / 'exe').resolve(strict=True)
        except (OSError, RuntimeError):
            continue
        if executable.is_relative_to(Path('/var/tmp/codestudio-credential-fixture')):
            fixture_processes.append({'pid': int(proc.name), 'executable': str(executable)})
databases = []
for path in root.rglob('*'):
    if path.is_file():
        with path.open('rb') as source:
            if source.read(16) == b'SQLite format 3\0' or path.name.endswith(('.sqlite3', '.sqlite3-wal', '.sqlite3-shm')):
                databases.append(str(path.relative_to(root)))
windows = json.loads((root / 'research/delivery/final-static-windows.json').read_text(encoding='utf-8-sig'))
delivery_temporary_resources = [str(path) for parent in (mirror / 'build', mirror / 'build/packages', Path(tempfile.gettempdir()))
    if parent.exists() for path in parent.iterdir()
    if path.name.startswith(('codestudio-delivery-', 'codestudio-package-test-', 'codestudio-verification-',
                             '.codestudio-package-stage-', '.codestudio-package-backup-', '.verify-'))
    or path.name.endswith('.codestudio-package.lock')]
linux_release_configured = 'mode = "release"' in (mirror / '.xmake/linux/x86_64/xmake.conf').read_text()
result = {'date':'2026-10-25', 'linux_release_configured':linux_release_configured, 'python_ast_files':len(python_files), 'relative_document_links':links,
          'missing_document_links':missing, 'linux_verification_script_syntax':'valid',
          'linux_project_processes':processes, 'linux_temporary_cgroups':cgroups,
          'linux_isolation_directories':isolations, 'linux_autostart_entries':registrations,
          'linux_private_keyring_directories':private_keyrings, 'linux_private_keyring_processes':fixture_processes,
          'workspace_databases':databases, 'linux_delivery_temporary_resources':delivery_temporary_resources, 'windows':windows}
receipt.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
print(json.dumps(result, indent=2))
assert linux_release_configured
assert windows['ReleaseConfigured']
assert not any((missing, processes, cgroups, isolations, registrations, databases, private_keyrings, fixture_processes,
                delivery_temporary_resources, windows['ProjectProcesses'], windows['AutostartRegistrations'],
                windows['CredentialTestEntries'], windows['DeliveryTemporaryResources']))

