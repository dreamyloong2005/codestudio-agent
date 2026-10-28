"""Read-only Windows cleanup receipt. Enumerates only synthetic test targets."""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from datetime import datetime, timezone

root = Path(__file__).resolve().parents[2]
evidence = Path(__file__).resolve().parent
baseline_path = root / 'research/plugin-isolation/final-static-windows.json'


class Credential(ctypes.Structure):
    _fields_ = [('Flags', wintypes.DWORD), ('Type', wintypes.DWORD), ('TargetName', wintypes.LPWSTR),
                ('Comment', wintypes.LPWSTR), ('LastWritten', wintypes.FILETIME),
                ('CredentialBlobSize', wintypes.DWORD), ('CredentialBlob', ctypes.c_void_p),
                ('Persist', wintypes.DWORD), ('AttributeCount', wintypes.DWORD),
                ('Attributes', ctypes.c_void_p), ('TargetAlias', wintypes.LPWSTR), ('UserName', wintypes.LPWSTR)]


library = ctypes.WinDLL('advapi32', use_last_error=True)
array_type = ctypes.POINTER(ctypes.POINTER(Credential))
library.CredEnumerateW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(array_type)]
library.CredEnumerateW.restype = wintypes.BOOL
library.CredFree.argtypes = [ctypes.c_void_p]
def targets(pattern):
    # Inspect target names only. Credential blobs are never read or logged.
    count, values = wintypes.DWORD(), array_type()
    if library.CredEnumerateW(pattern, 0, ctypes.byref(count), ctypes.byref(values)):
        try:
            return [values[index].contents.TargetName for index in range(count.value)]
        finally:
            library.CredFree(values)
    assert ctypes.get_last_error() == 1168, 'Could not verify synthetic credential cleanup'
    return []


entries = targets('CodeStudio-Agent/credentials/test-credential-*')
oauth_entries = targets('CodeStudio-Agent/oauth/test-credential-oauth-*')

command = r'''
$ErrorActionPreference = 'Stop'
$rootPath = (Resolve-Path '.').Path.TrimEnd('\') + '\'
$knownNames = @('codestudio-agent.exe', 'agent-tests.exe', 'provider-fixture.exe', 'process-fixture.exe', 'line-process-fixture.exe', 'mcp-fixture.exe', 'sandbox-fixture.exe', 'build-fixture.exe', 'plugin-sandbox-fixture.exe', 'sandbox-resource-fixture.exe')
$allProcesses = @(Get-CimInstance Win32_Process)
$processes = @($allProcesses | Where-Object { $_.ExecutablePath -and ($_.ExecutablePath.StartsWith($rootPath, [StringComparison]::OrdinalIgnoreCase) -or $_.ExecutablePath -match '\\codestudio-(delivery|sandbox|package-interface|resume|resource-test)-[^\\]+\\') -and $_.Name -ne 'python.exe' } | Select-Object ProcessId, Name, ExecutablePath)
$unresolved = @($allProcesses | Where-Object { -not $_.ExecutablePath -and $_.Name -in $knownNames } | Select-Object ProcessId, Name, ExecutablePath)
$scheduler = New-Object -ComObject 'Schedule.Service'
$scheduler.Connect()
$registrations = @($scheduler.GetFolder('\').GetTasks(1) | Where-Object { $_.Name -like 'CodeStudio-Agent-*' } | Select-Object Name, Path, State)
[ordered]@{ ProjectProcesses = $processes; UnresolvedProjectProcessCandidates = $unresolved; AutostartRegistrations = $registrations; ReleaseConfigured = (Get-Content '.xmake/windows/x64/xmake.conf' -Raw).Contains('mode = "release"') } | ConvertTo-Json -Depth 5
'''
response = subprocess.run(['powershell', '-NoProfile', '-Command', command], cwd=root, capture_output=True, check=True)
receipt = json.loads(response.stdout.decode('utf-8-sig'))
receipt['format'] = 2
receipt['generated_at'] = datetime.now(timezone.utc).isoformat()
receipt['ProcessScanScope'] = 'Project/temp image paths plus known executable names with unavailable image paths; PID/name/path only'
receipt['CredentialTestEntries'] = entries
receipt['OAuthTestEntries'] = oauth_entries
receipt['CredentialScanScope'] = 'Synthetic ordinary and OAuth native namespaces; target names only'
receipt['SandboxStagingDirectories'] = [str(path) for path in Path(tempfile.gettempdir()).glob('codestudio-sandbox-*')
    if re.fullmatch(r'codestudio-sandbox-[0-9a-f]{32}', path.name)]
receipt['SandboxProfileDirectories'] = [str(path) for path in (Path(os.environ['LOCALAPPDATA']) / 'Packages').glob('codestudio-sandbox-*')
    if re.fullmatch(r'codestudio-sandbox-[0-9a-f]{32}', path.name)]
receipt['PackageInterfaceTemporaryResources'] = [str(path) for path in Path(tempfile.gettempdir()).iterdir()
    if path.name.startswith(('codestudio-package-interface-', 'codestudio-plugin-sandbox-', 'codestudio-sandbox-tests-',
                             'codestudio-resource-test-'))]
receipt['DeliveryTemporaryResources'] = [str(path) for parent in (root / 'build', root / 'build/packages', Path(tempfile.gettempdir()))
    if parent.exists() for path in parent.iterdir()
    if path.name.startswith(('codestudio-delivery-', 'codestudio-package-test-', 'codestudio-verification-',
                             '.codestudio-package-stage-', '.codestudio-package-backup-', '.verify-'))
    or path.name.endswith('.codestudio-package.lock')]
registry_paths = [Path.home() / '.codestudio-agent/sandbox-resources']
if os.environ.get('CODESTUDIO_SANDBOX_REGISTRY'):
    registry_paths.append(Path(os.environ['CODESTUDIO_SANDBOX_REGISTRY']))
receipt['SandboxResourceRegistries'] = [
    {'path': str(path), 'exists': path.exists(),
     'entries': sorted(str(entry) for entry in path.iterdir()) if path.exists() else []}
    for path in dict.fromkeys(registry_paths)]

# Historical paths are classified by exact canonical path, never by a permitted
# count or a prefix. Resolving expands 8.3 user names used in the earlier receipt.
baseline = json.loads(baseline_path.read_text(encoding='utf-8-sig'))
resource_fields = ('SandboxStagingDirectories', 'SandboxProfileDirectories', 'PackageInterfaceTemporaryResources')
historical, new, no_longer_observed = {}, {}, {}
for field in resource_fields:
    old = {str(Path(path).resolve()).casefold(): path for path in baseline[field]}
    observed = {str(Path(path).resolve()).casefold(): path for path in receipt[field]}
    historical[field] = sorted(path for key, path in observed.items() if key in old)
    new[field] = sorted(path for key, path in observed.items() if key not in old)
    no_longer_observed[field] = sorted(path for key, path in old.items() if key not in observed)
receipt['HistoricalBaselineReceipt'] = baseline_path.relative_to(root).as_posix()
receipt['ObservedHistoricalResources'] = historical
receipt['NewResources'] = new
receipt['HistoricalResourcesNoLongerObserved'] = no_longer_observed
receipt['HistoricalResidueScope'] = 'Unregistered historical resources are only enumerated; no adoption, cleanup or all-resource-clean claim'
other_residue = any((entries, oauth_entries, receipt['ProjectProcesses'], receipt['UnresolvedProjectProcessCandidates'],
                     receipt['AutostartRegistrations'], receipt['DeliveryTemporaryResources'],
                     any(item['entries'] for item in receipt['SandboxResourceRegistries'])))
receipt['NoNewResidueGate'] = 'failed' if other_residue or any(new.values()) else 'passed'
receipt['ZeroResidueGate'] = 'failed' if other_residue or any(receipt[field] for field in resource_fields) else 'passed'
(evidence / 'final-static-windows.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
print(json.dumps(receipt, indent=2))
assert receipt['ReleaseConfigured']
assert receipt['NoNewResidueGate'] == 'passed', 'Current milestone has new or unresolved resource residue; inspect receipt'
