param(
    [ValidateSet('debug', 'release', 'all')]
    [string]$Mode = 'debug',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Options
)
$ErrorActionPreference = 'Stop'
$verificationPython = if ($env:CODESTUDIO_CONTRACT_PYTHON) { $env:CODESTUDIO_CONTRACT_PYTHON } else { 'python' }
& $verificationPython (Join-Path $PSScriptRoot 'verify.py') --mode $Mode @Options
exit $LASTEXITCODE
