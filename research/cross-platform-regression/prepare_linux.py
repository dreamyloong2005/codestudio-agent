from pathlib import Path
import shutil
import json
source=Path('/mnt/d/Dev/Projects/codestudio-agent')
mirror=Path('/var/tmp/codestudio-agent-linux')
assert (mirror/'.codestudio-build-mirror').read_text()==str(source)
for relative in ('research/credentials/linux_secrets.py','research/credentials/unlock_test_keyring.c','research/cross-platform-regression/with_credential_cleanup.py'):
    dst=mirror/relative
    dst.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source/relative,dst)
print('Private credential wrapper copied without deleting prior receipts')
