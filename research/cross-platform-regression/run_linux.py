from pathlib import Path
import subprocess,sys
root=Path(__file__).resolve().parents[2]
py=str(root/'build/contract-python/bin/python')
command=[py,'scripts/linux_test_delegation.py','--',py,'research/credentials/linux_secrets.py','--',py,'research/cross-platform-regression/with_credential_cleanup.py','--',py,'scripts/verify.py',*sys.argv[1:]]
raise SystemExit(subprocess.call(command,cwd=root))
