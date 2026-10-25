"""Reproduce the credential milestone regression, one configuration at a time."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--mode', choices=('debug', 'release'), required=True)
parser.add_argument('--logs', type=Path, required=True)
parser.add_argument('--suite', action='append')
parser.add_argument('--skip-build', action='store_true')
args = parser.parse_args()
root = Path.cwd()
logs = args.logs.resolve()
logs.mkdir(parents=True, exist_ok=True)
platform, arch, compiler = ('windows', 'x64', 'clang-cl') if os.name == 'nt' else ('linux', 'x86_64', 'gcc')
suffix = '.exe' if os.name == 'nt' else ''
binary = root / 'build' / platform / arch / args.mode
agent = str(binary / ('codestudio-agent' + suffix))
provider = str(binary / ('provider-fixture' + suffix))
env = dict(os.environ)
if os.name != 'nt' and os.getuid() == 0:
    env['XMAKE_ROOT'] = 'y'


def run(name, command):
    path = logs / f'{platform}-{args.mode}-{name}-final.log'
    with path.open('wb') as output:
        result = subprocess.run(command, cwd=root, env=env, stdout=output, stderr=subprocess.STDOUT)
    print(f'{platform}/{args.mode} {name}: {result.returncode}', flush=True)
    if result.returncode:
        print(path.read_text(encoding='utf-8', errors='replace')[-9000:], flush=True)
        raise SystemExit(result.returncode)


if not args.skip_build:
    run('config', ['xmake', 'f', '-p', platform, '-a', arch, f'--toolchain={compiler}', '-m', args.mode,
                   *(['--system_deps=y'] if platform == 'linux' else []), '-y'])
    run('build', ['xmake', 'build', '-a', '-j', '6'])
commands = {
    'native': [str(binary / ('agent-tests' + suffix))],
    'schema': [sys.executable, 'tests/contract_schema.py'],
    'credentials': [sys.executable, 'tests/credentials_integration.py', agent],
    'provider': [sys.executable, 'tests/provider_integration.py', provider],
    'streaming': [sys.executable, 'tests/streaming_integration.py', provider, agent],
    **{name: [sys.executable, f'tests/{name}_integration.py', agent] for name in
       ('cli', 'mcp', 'resume', 'scheduler', 'supervisor', 'cron', 'autostart', 'build_proposals')},
    'contract': [sys.executable, 'tests/client_contract_integration.py', agent],
    'service': [sys.executable, 'tests/client_contract_integration.py', agent, '--service-suite'],
}
for name in (args.suite or commands):
    run(name, commands[name])
