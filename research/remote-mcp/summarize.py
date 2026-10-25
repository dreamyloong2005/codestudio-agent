"""Read completed logs; count each final suite once, keeping failures separate."""
import json
from pathlib import Path
import re

logs = Path(__file__).resolve().parent


def unittest_result(path):
    content = path.read_text(encoding='utf-8', errors='replace')
    matches = re.findall(r'^Ran (\d+) tests? in .+$', content, re.MULTILINE)
    assert len(matches) == 1, path
    outcome = re.search(r'^OK(?: \(skipped=(\d+)\))?\s*\Z', content, re.MULTILINE)
    assert outcome, path
    count, skipped = int(matches[0]), int(outcome[1] or 0)
    return {'cases': count, 'passed': count - skipped, 'skipped': skipped}


result = {'date': '2026-10-25', 'state_schema': 7, 'protocol_version': 2, 'configurations': {}}
for platform in ('windows', 'linux'):
    for mode in ('debug', 'release'):
        prefix = f'{platform}-{mode}'
        native = (logs / f'{prefix}-native-final.log').read_text(encoding='utf-8')
        groups = len(re.findall(r'^PASS ', native, re.MULTILINE))
        assert groups == 22 and 'FAIL' not in native, prefix
        schema = unittest_result(logs / f'{prefix}-schema-final.log')
        assert schema == {'cases': 12, 'passed': 12, 'skipped': 0}
        suites = {name: unittest_result(logs / f'{prefix}-{name}-final.log') for name in (
            'credentials', 'provider', 'streaming', 'cli', 'mcp', 'mcp_http', 'resume', 'scheduler',
            'supervisor', 'cron', 'autostart', 'build_proposals', 'contract', 'service')}
        result['configurations'][prefix] = {
            'native_groups': groups, 'static_schema_tests': schema['passed'], 'suites': suites,
            'python_cases': sum(s['cases'] for s in suites.values()),
            'passed': sum(s['passed'] for s in suites.values()),
            'skipped': sum(s['skipped'] for s in suites.values())}
result['scope'] = [
    'Final related regression on Windows and Arch Linux WSL, Debug and Release; earlier full-platform runs are historical.',
    'Synthetic Windows native credential entries and private signed gnome-keyring test service; no real user secret or inference.',
    'Linux private collection lock/unlock/readback and unavailable-session cases run; Windows skips these two Linux cases.',
    'The existing 25-case approval suite is Windows-only; cross-platform credentials tests independently exercise approved build output redaction.',
    'No actual login/reboot or permanently registered workspace; outer Windows Job still limits detached-success verification.',
    'Linux native timeout cancellation reproduced and repaired; uncertain effects remain unknown.',
    'Explicit session reload on 404, no OAuth, Windows ASCII SSE cursors; synthetic loopback transport tests, not live third-party TLS services.',
    'Initial HTTP fixture ping and empty-state expectation failures are retained separately; repeated cases count once.']
(logs / 'verification.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps({key: {name: value[name] for name in ('native_groups', 'static_schema_tests', 'passed', 'skipped')}
                  for key, value in result['configurations'].items()}, indent=2))
