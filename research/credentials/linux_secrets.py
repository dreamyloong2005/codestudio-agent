"""Run a command against an owned, temporary Secret Service on a private bus.

Test daemon and gcr libraries are extracted from signed Arch packages under
/var/tmp/codestudio-credential-fixture/root; no desktop service is installed.
Never uses or changes the user's bus, home, credentials or login configuration.
"""
import os
import shlex
from pathlib import Path
import subprocess
import sys
import tempfile
import time

daemon = Path('/var/tmp/codestudio-credential-fixture/root/usr/bin/gnome-keyring-daemon')
password = b'codestudio-test-keyring-password'
args = sys.argv[1:]
if args[:1] != ['--inside']:
    if args[:1] == ['--']:
        args.pop(0)
    if not args or not daemon.is_file():
        raise SystemExit('Expected command and extracted signed test daemon')
    unlock = daemon.parents[3] / 'unlock-test-keyring'
    unlock_source = Path(__file__).with_name('unlock_test_keyring.c')
    if not unlock.exists() or unlock.stat().st_mtime < unlock_source.stat().st_mtime:
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gio-2.0'], text=True))
        subprocess.run(['gcc', str(unlock_source), '-o', str(unlock), *flags], check=True)
    with tempfile.TemporaryDirectory(prefix='codestudio-secret-service-') as temporary:
        env = dict(os.environ)
        env.pop('DBUS_SESSION_BUS_ADDRESS', None)
        env.pop('DBUS_STARTER_ADDRESS', None)
        env.pop('DBUS_STARTER_BUS_TYPE', None)
        for name in ('XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR'):
            directory = Path(temporary) / name.lower()
            directory.mkdir(mode=0o700)
            env[name] = str(directory)
        env['LD_LIBRARY_PATH'] = str(daemon.parents[1] / 'lib')
        env['LC_ALL'] = 'C'
        env['CODESTUDIO_TEST_PRIVATE_SECRET_SERVICE'] = '1'
        env['CODESTUDIO_TEST_KEYRING_DAEMON'] = str(daemon)
        env['CODESTUDIO_TEST_KEYRING_UNLOCK'] = str(unlock)
        control = Path(env['XDG_RUNTIME_DIR']) / 'keyring'
        control.mkdir(mode=0o700)
        env['GNOME_KEYRING_CONTROL'] = str(control)
        result = subprocess.run(['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()),
                                 '--inside', *args], env=env)
        raise SystemExit(result.returncode)

assert os.environ.get('CODESTUDIO_TEST_PRIVATE_SECRET_SERVICE') == '1'
assert os.environ['DBUS_SESSION_BUS_ADDRESS'].startswith('unix:')
with tempfile.TemporaryFile() as output:
    process = subprocess.Popen([str(daemon), '--foreground', '--unlock', '--components=secrets',
                                '--control-directory', os.environ['GNOME_KEYRING_CONTROL']],
                               stdin=subprocess.PIPE, stdout=output, stderr=output)
    try:
        process.stdin.write(password)
        process.stdin.close()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if process.poll() is not None:
                output.seek(0)
                raise RuntimeError('Private credential fixture exited: ' + output.read().decode(errors='replace'))
            ready = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.secrets',
                '--object-path', '/org/freedesktop/secrets', '--method',
                'org.freedesktop.Secret.Service.ReadAlias', 'default'], capture_output=True, timeout=2)
            if ready.returncode == 0 and b'/org/freedesktop/secrets/collection/' in ready.stdout:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError('Private credential fixture did not become ready')
        result = subprocess.run(args[1:])
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
raise SystemExit(result.returncode)
