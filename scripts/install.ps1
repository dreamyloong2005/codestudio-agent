# User-level package installer. Requires Python 3.10+; never changes PATH.
$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot 'install.py'
& python $installer @args
exit $LASTEXITCODE
