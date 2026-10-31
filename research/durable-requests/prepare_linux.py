from pathlib import Path
import shutil
source=Path('/mnt/d/Dev/Projects/codestudio-agent')
mirror=Path('/var/tmp/codestudio-agent-linux')
assert (mirror/'.codestudio-build-mirror').read_text()==str(source)
for relative in ('research/credentials/linux_secrets.py','research/credentials/unlock_test_keyring.c','research/durable-requests/with_credential_cleanup.py'):
    target=mirror/relative
    target.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source/relative,target)
print('Private Linux credential fixture ready')
