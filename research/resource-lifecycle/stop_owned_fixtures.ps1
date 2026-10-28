$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class OwnedFixtureProcess {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint rights, bool inherit, uint pid);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetProcessTimes(IntPtr h, out long created, out long exited, out long kernel, out long user);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool TerminateProcess(IntPtr h, uint code);
    [DllImport("kernel32.dll")] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
'@
$expected = @(
    @(20708,'2026-10-25T11:08:32.9161757Z','debug'),
    @(54728,'2026-10-25T11:08:41.8780668Z','debug'),
    @(51892,'2026-10-25T12:18:11.4863588Z','debug'),
    @(62928,'2026-10-25T12:19:28.8124786Z','debug'),
    @(57068,'2026-10-25T12:22:21.8547311Z','release'),
    @(59584,'2026-10-25T12:23:54.5493726Z','release')
)
$receipts = foreach ($item in $expected) {
    $targetPid = [uint32]$item[0]
    $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$targetPid"
    if (!$proc) { [pscustomobject]@{pid=$targetPid; state='absent'}; continue }
    $exe = 'D:\Dev\Projects\codestudio-agent\build\windows\x64\' + $item[2] + '\process-fixture.exe'
    if ($proc.CommandLine -cne ('"' + $exe + '" "sleep" "10000"')) { throw "Command mismatch for $targetPid" }
    if (Get-Process -Id $proc.ParentProcessId -ErrorAction SilentlyContinue) { throw "Parent still exists for $targetPid" }
    $expectedBirth = [datetime]::Parse($item[1]).ToUniversalTime().ToFileTimeUtc()
    $handle = [OwnedFixtureProcess]::OpenProcess(0x100001 -bor 0x1000, $false, $targetPid)
    if ($handle -eq [IntPtr]::Zero) { throw "Open failed for $targetPid : $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    try {
        [long]$created=0; [long]$exited=0; [long]$kernel=0; [long]$user=0
        if (![OwnedFixtureProcess]::GetProcessTimes($handle,[ref]$created,[ref]$exited,[ref]$kernel,[ref]$user) -or $created -ne $expectedBirth) { throw "Birth mismatch for $targetPid" }
        if (![OwnedFixtureProcess]::TerminateProcess($handle,1)) { throw "Termination failed for $targetPid" }
        if ([OwnedFixtureProcess]::WaitForSingleObject($handle,5000) -ne 0) { throw "Exit not confirmed for $targetPid" }
        [pscustomobject]@{pid=$targetPid; birth=$item[1]; command=$proc.CommandLine; state='terminated'; handle_identity_verified=$true}
    } finally { [void][OwnedFixtureProcess]::CloseHandle($handle) }
}
$receipts | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath 'research/resource-lifecycle/old-fixture-processes.json'
$receipts | ConvertTo-Json -Depth 4
