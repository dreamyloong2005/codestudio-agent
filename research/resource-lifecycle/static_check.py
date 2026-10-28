"""Read-only final workspace/resource checks; writes only its JSON receipt."""
import ast
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from datetime import datetime, timezone
from finalize_delivery import verify_linux_credential_cleanup

root = Path(__file__).resolve().parents[2]
mirror = Path('/var/tmp/codestudio-agent-linux')
receipt = root / 'research/resource-lifecycle/final-static.json'
receipt.write_text('{}\n', encoding='utf-8')
python_files = [*root.glob('tests/*.py'), *root.glob('scripts/*.py'), *root.glob('examples/*.py'), *root.glob('research/resource-lifecycle/*.py')]
for path in python_files:
    ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
for name in ('verify-linux.sh', 'install.sh'):
    subprocess.run(['bash', '-n', str(root / 'scripts' / name)], check=True)
links, missing = 0, []
for path in [root / 'README.md', *root.glob('docs/*.md'), *root.glob('protocol/*.md')]:
    for value in re.findall(r'\[[^\]]*\]\(([^)]+)\)', path.read_text(encoding='utf-8')):
        target = value.split('#', 1)[0]
        if not target or '://' in target or target.startswith('mailto:'):
            continue
        links += 1
        if not (path.parent / target).exists():
            missing.append(f'{path.relative_to(root)}: {target}')
processes, unresolved_processes, fixture_processes = [], [], []
executable_names = ('codestudio-agent', 'agent-tests', 'process-fixture', 'mcp-fixture', 'build-fixture',
                    'sandbox-fixture', 'line-process-fixture', 'provider-fixture', 'plugin-sandbox-fixture',
                    'sandbox-resource-fixture')
candidate_comm_names = {name[:15] for name in (*executable_names, 'gnome-keyring-daemon')}
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():
        continue
    try:
        executable = (proc / 'exe').resolve(strict=True)
    except (OSError, RuntimeError):
        try:
            name = (proc / 'comm').read_text().strip()
        except FileNotFoundError:
            continue  # This process exited during the read-only scan.
        if name in candidate_comm_names and proc.exists():
            unresolved_processes.append({'pid': int(proc.name), 'name': name, 'executable': None,
                                         'reason': 'Executable path unavailable; ownership not established'})
        continue
    # Exclude the currently running Python validation environment itself.
    if executable.name in executable_names and (executable.is_relative_to(root) or executable.is_relative_to(mirror) or any(part.startswith(('codestudio-delivery-', 'codestudio-isolation-', 'codestudio-package-interface-', 'codestudio-resume-', 'codestudio-resource-test-')) for part in executable.parts)):
        processes.append({'pid':int(proc.name), 'executable':str(executable)})
    if executable.is_relative_to(Path('/var/tmp/codestudio-credential-fixture')):
        fixture_processes.append({'pid': int(proc.name), 'executable': str(executable)})
cgroups = [str(path) for path in Path('/sys/fs/cgroup').glob('codestudio-test-*')]
package_cgroups = [str(path) for path in Path('/sys/fs/cgroup').rglob('codestudio-isolation-*')]
isolations = [str(path) for path in Path('/tmp').glob('codestudio-isolation-*')]
package_temporary_resources = [str(path) for path in Path(tempfile.gettempdir()).iterdir()
    if path.name.startswith(('codestudio-package-interface-', 'codestudio-plugin-sandbox-', 'codestudio-linux-sandbox-tests-',
                             'codestudio-resource-test-'))]
registrations = [str(path) for path in (Path.home() / '.config/systemd/user').glob('**/codestudio-agent-*.service')]
private_keyrings = [str(path) for path in Path('/tmp').glob('codestudio-secret-service-*')]
oauth_lock_prefix = '@codestudio-oauth.' + str(os.geteuid()) + '.'
oauth_abstract_locks = [line.split()[-1] for line in Path('/proc/net/unix').read_text().splitlines()
                        if line.split() and line.split()[-1].startswith(oauth_lock_prefix)]
state_home = Path(os.environ.get('XDG_STATE_HOME') or Path.home() / '.local/state')
registry_paths = [state_home / 'CodeStudio-Agent/sandbox-resources']
if os.environ.get('CODESTUDIO_SANDBOX_REGISTRY'):
    registry_paths.append(Path(os.environ['CODESTUDIO_SANDBOX_REGISTRY']))
resource_registries = [
    {'path': str(path), 'exists': path.exists(),
     'entries': sorted(str(entry) for entry in path.iterdir()) if path.exists() else []}
    for path in dict.fromkeys(registry_paths)]
databases = []
for parent, folders, files in os.walk(root, followlinks=False):
    folders[:] = [name for name in folders if name not in ('.git', '__pycache__')]
    for name in files:
        path = Path(parent) / name
        if path.is_symlink():
            continue
        with path.open('rb') as source:
            if source.read(16) == b'SQLite format 3\0' or path.name.endswith(('.sqlite3', '.sqlite3-wal', '.sqlite3-shm')):
                databases.append(str(path.relative_to(root)))
windows = json.loads((root / 'research/resource-lifecycle/final-static-windows.json').read_text(encoding='utf-8-sig'))
delivery = json.loads((root / 'research/resource-lifecycle/delivery.json').read_text(encoding='utf-8'))
linux_credential_cleanup = verify_linux_credential_cleanup([
    delivery['full_verification']['linux-debug'], delivery['full_verification']['linux-release'],
    delivery['installed_verification']['linux']['receipt']])
assert linux_credential_cleanup == delivery['linux_private_native_credential_cleanup']
delivery_temporary_resources = [str(path) for parent in (mirror / 'build', mirror / 'build/packages', Path(tempfile.gettempdir()))
    if parent.exists() for path in parent.iterdir()
    if path.name.startswith(('codestudio-delivery-', 'codestudio-package-test-', 'codestudio-verification-',
                             '.codestudio-package-stage-', '.codestudio-package-backup-', '.verify-'))
    or path.name.endswith('.codestudio-package.lock')]
linux_release_configured = 'mode = "release"' in (mirror / '.xmake/linux/x86_64/xmake.conf').read_text()
result = {'format': 2, 'generated_at': datetime.now(timezone.utc).isoformat(), 'linux_release_configured':linux_release_configured, 'python_ast_files':len(python_files), 'relative_document_links':links,
          'missing_document_links':missing, 'linux_verification_script_syntax':'valid',
          'linux_project_processes':processes, 'linux_temporary_cgroups':cgroups,
          'linux_unresolved_project_process_candidates':unresolved_processes,
          'linux_sandbox_resource_registries':resource_registries,
          'linux_package_cgroups':package_cgroups, 'linux_package_temporary_resources':package_temporary_resources,
          'linux_isolation_directories':isolations, 'linux_autostart_entries':registrations,
          'linux_private_keyring_directories':private_keyrings, 'linux_private_keyring_processes':fixture_processes,
          'linux_oauth_abstract_locks':oauth_abstract_locks,
          'linux_private_native_credential_cleanup':linux_credential_cleanup,
          'workspace_databases':databases, 'linux_delivery_temporary_resources':delivery_temporary_resources, 'windows':windows}
linux_residue = any((processes, unresolved_processes, cgroups, package_cgroups, package_temporary_resources,
                    isolations, registrations, databases, private_keyrings, fixture_processes, oauth_abstract_locks,
                    delivery_temporary_resources, any(item['entries'] for item in resource_registries)))
result['no_new_residue_gate'] = 'passed' if not linux_residue and windows['NoNewResidueGate'] == 'passed' else 'failed'
result['zero_residue_gate'] = 'passed' if not linux_residue and windows['ZeroResidueGate'] == 'passed' else 'failed'
result['residue_scope'] = ('Read-only observation; all historical Windows arrays remain visible. No-new-residue success '
                           'does not claim zero total residue, adoption or cleanup of historical resources.')
receipt.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
print(json.dumps(result, indent=2))
assert linux_release_configured
assert windows['ReleaseConfigured']
assert not missing
assert result['no_new_residue_gate'] == 'passed', 'New or unresolved resource residue remains; inspect receipt'
