"""Read-only Windows cleanup receipt. Enumerates only synthetic test targets."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]


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
count, values = wintypes.DWORD(), array_type()
entries = []
if library.CredEnumerateW('CodeStudio-Agent/credentials/test-credential-*', 0, ctypes.byref(count), ctypes.byref(values)):
    try:
        entries = [values[index].contents.TargetName for index in range(count.value)]
    finally:
        library.CredFree(values)
else:
    assert ctypes.get_last_error() == 1168, 'Could not verify synthetic credential cleanup'

command = r'''
$ErrorActionPreference = 'Stop'
$rootPath = (Resolve-Path '.').Path.TrimEnd('\') + '\'
$processes = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($rootPath, [StringComparison]::OrdinalIgnoreCase) -and $_.Name -ne 'python.exe' } | Select-Object ProcessId, Name, ExecutablePath)
$scheduler = New-Object -ComObject 'Schedule.Service'
$scheduler.Connect()
$registrations = @($scheduler.GetFolder('\').GetTasks(1) | Where-Object { $_.Name -like 'CodeStudio-Agent-*' } | Select-Object Name, Path, State)
[ordered]@{ ProjectProcesses = $processes; AutostartRegistrations = $registrations; ReleaseConfigured = (Get-Content '.xmake/windows/x64/xmake.conf' -Raw).Contains('mode = "release"') } | ConvertTo-Json -Depth 5
'''
response = subprocess.run(['powershell', '-NoProfile', '-Command', command], cwd=root, capture_output=True, check=True)
receipt = json.loads(response.stdout.decode('utf-8-sig'))
receipt['CredentialTestEntries'] = entries
(root / 'research/remote-mcp/final-static-windows.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
print(json.dumps(receipt, indent=2))
assert receipt['ReleaseConfigured']
assert not any((entries, receipt['ProjectProcesses'], receipt['AutostartRegistrations']))
