"""Validate this milestone's receipts and write new consolidated evidence."""
import ast
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'scripts'))
from verify import source_fingerprint


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write(name, value):
    with (EVIDENCE / name).open('x', encoding='utf-8') as output:
        json.dump(value, output, indent=2)
        output.write('\n')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def receipt(platform, mode):
    paths = sorted((EVIDENCE / platform).glob(mode + '-*/verification.json'))
    paths = [p for p in paths if read(p).get('status') == 'succeeded' and read(p).get('source') == SOURCE]
    assert len(paths) == 1, (platform, mode, paths)
    path = paths[0]
    value = read(path)
    assert value['status'] == 'succeeded' and value['source'] == SOURCE
    assert all(step['status'] == 'succeeded' for step in value['steps'])
    native = next((s.get('native_groups', 0) for s in value['steps'] if s['name'] == 'native'), 0)
    return value, {'receipt': path.relative_to(EVIDENCE).as_posix(),
                   'native_groups': native, 'python_cases': sum(s.get('cases', 0) for s in value['steps']),
                   'python_passed': sum(s.get('passed', 0) for s in value['steps']),
                   'python_skipped': sum(s.get('skipped', 0) for s in value['steps']),
                   'selected_suites': value['selected_suites'], 'omitted_suites': value['omitted_suites'],
                   'unsupported_suites': value['unsupported_suites'], 'artifact': value['artifact']}


SOURCE = source_fingerprint()
assert SOURCE == read(EVIDENCE / 'source-freeze-final.json')
configurations, release, installed, packages = {}, {}, {}, {}
for platform in ('windows', 'linux'):
    for mode in ('debug', 'release'):
        value, summary = receipt(platform, mode)
        assert value['build_requested'] and summary['native_groups'] == 22
        assert value['omitted_suites'] == ['delivery']
        configurations[platform + '-' + mode] = summary
        if mode == 'release':
            release[platform] = value
    delivery, summary = receipt('installed-' + platform, 'release')
    assert delivery['selected_suites'] == ['delivery']
    installed[platform] = summary
    archive = read(EVIDENCE / ('archive-' + platform + '-final.json'))
    assert archive['status'] == 'succeeded' and archive['source'] == SOURCE
    assert archive['repeat_archive_identical']
    assert delivery['delivery_package']['manifest_sha256'] == archive['package_manifest_sha256']
    packages[platform] = {'receipt': 'archive-' + platform + '-final.json', 'sha256': archive['archive_sha256'],
                          'payload_files': archive['file_count'], 'repeat_archive_identical': True,
                          'fresh_archive_extraction': True, 'installed_verification': summary}
    if platform == 'windows':
        path = Path(archive['packages'][0]['archive'])
        assert digest(path) == archive['archive_sha256']
        assert digest(ROOT / release[platform]['artifact']['path']) == release[platform]['artifact']['sha256']
        packages[platform]['path'] = path.relative_to(ROOT).as_posix()
    else:
        transfer = read(EVIDENCE / 'linux-transfer-final.json')
        assert transfer['status'] == 'succeeded'
        for item in transfer['delivered']:
            assert digest(ROOT / item['path']) == item['sha256']
            if item['path'].endswith('.tar.gz'):
                assert item['sha256'] == archive['archive_sha256']
                packages[platform]['path'] = item['path']
            else:
                assert item['sha256'] == release[platform]['artifact']['sha256']

residue = {}
for platform in ('windows', 'linux'):
    residue_file = 'residue-after-windows-consistent.json' if platform == 'windows' else 'residue-after-linux-final.json'
    data = read(EVIDENCE / residue_file)
    assert data['errors'] == [] and data['no_new_residue_gate'] == 'passed'
    residue[platform] = {'receipt': residue_file,
                         'no_new_residue_gate': data['no_new_residue_gate'],
                         'zero_total_residue_gate': data['zero_total_residue_gate'],
                         'observed_counts': {k: len(v) for k, v in data['observed'].items()}}
credential_receipts = sorted(EVIDENCE.glob('credential-cleanup-linux-*.json'))
assert len(credential_receipts) >= 2
for path in credential_receipts:
    data = read(path)
    assert data['status'] == 'succeeded'
    assert not data['ordinary_test_entries'] and not data['oauth_test_entries']

links, python_files = 0, 0
for path in [ROOT / 'CONTEXT.md', ROOT / 'README.md', *ROOT.glob('docs/*.md'), *ROOT.glob('protocol/*.md')]:
    for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)', path.read_text(encoding='utf-8')):
        target = target.split('#', 1)[0]
        if not target or '://' in target or target.startswith('mailto:'):
            continue
        assert (path.parent / target).exists(), (path, target)
        links += 1
for parent in ('scripts', 'tests', 'examples', 'research/cross-platform-regression'):
    for path in (ROOT / parent).glob('*.py'):
        ast.parse(path.read_text(encoding='utf-8'), filename=str(path))
        python_files += 1
subprocess.run(['git', '-c', 'core.whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol',
                'diff', '--check'], cwd=ROOT, check=True)
base = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
changed = subprocess.check_output(['git', 'diff', '--name-only'], cwd=ROOT, text=True).splitlines()
assert set(changed) <= {'CONTEXT.md', 'docs/macos.md', 'docs/validation.md', 'docs/roadmap.md', 'docs/delivery.md', 'tests/delivery_integration.py'}
now = datetime.now(timezone.utc).isoformat()
common = {'format': 2, 'generated_at': now, 'base_commit': base, 'source': SOURCE}
write('verification.json', {**common, 'status': 'succeeded', 'configurations': configurations})
write('delivery.json', {**common, 'status': 'succeeded', 'packages': packages,
                       'scope': 'Existing Windows and Arch WSL hosts, temporary owned installation prefixes; no clean-machine or login acceptance.'})
write('final-static.json', {**common, 'status': 'succeeded', 'document_links': links,
                           'python_syntax_files': python_files, 'changed_tracked_files': changed,
                           'residue': residue, 'expanded_scan_audit': 'windows-expanded-scan-audit.json',
                           'credential_cleanup_receipts': [p.name for p in credential_receipts]})
completion = {**common, 'status': 'completed', 'configurations': configurations,
              'packages': packages, 'residue': residue, 'expanded_scan_audit': 'windows-expanded-scan-audit.json',
              'runtime_or_installer_changes': False,
              'validation_code_changed': True,
              'validation_fix': {'file': 'tests/delivery_integration.py',
                                 'cause': 'Windows lstat and fstat ctime/mode representations differ; compare each metadata API against itself while preserving file identity and end-path checks.',
                                 'red': 'snapshot-regression-red.log',
                                 'focused_green': ['snapshot-regression-green-windows.log', 'snapshot-regression-green-linux.log'],
                                 'initial_delivery_failure': 'installed-windows/release-20261028T102624Z-185d69ff/verification.json'},
              'initial_harness_failures': [
                  {'receipt': 'residue-before-linux.json', 'reason': 'Absent optional systemd user directory was initially treated as a scan error; corrected and rescanned.'},
                  {'receipt': 'packaging-linux-initial-root-refusal.log', 'reason': 'Package harness initially omitted XMAKE_ROOT for the existing root WSL test host; corrected test environment.'}],
              'boundaries': [
                  'Platform-specific skips and enclosing Windows Job restrictions remain explicit in per-suite logs.',
                  'Historical Windows staging/profile directories and the previously policy-blocked fixture were observed only; no historical adoption or deletion.',
                  'Existing Windows and WSL hosts; no independent clean machine, real login/reboot, hosted CI, or permanent installation.',
                  'No real inference, external OAuth login, signing/notarization, Git commit/push, or external publication.',
                  'macOS prior receipts retain their original scope; this run neither revalidates macOS nor enables its isolation.']}
write('completion.json', completion)
rows = '\n'.join(f"| {name} | {value['native_groups']} | {value['python_passed']} | {value['python_skipped']} |"
                 for name, value in configurations.items())
(EVIDENCE / 'README.md').write_text(f'''# Windows / WSL Linux 回归与交付验收

基于 `{base[:7]}` 的本轮验证已完成。源码为 {SOURCE['files']} 个文件，SHA256 `{SOURCE['sha256']}`。本轮修正文档，并修复 Windows 交付验收脚本的元数据比较；Agent 运行时及安装器实现未修改。详细结论见 [完成回执](completion.json)、[完整验证](verification.json)、[交付记录](delivery.json) 与 [静态及资源检查](final-static.json)。旧平台及 macOS 回执保留原有历史范围。

| 平台与配置 | 原生测试组 | Python 通过 | Python 跳过 |
|---|---:|---:|---:|
{rows}

完整套件包括真实隔离构建、插件隔离、资源生命周期、恢复、后台 worker/监督、cron、自启登记、模型/MCP 合成协议与验证/打包工具。Windows 每配置另通过两个真实 XMake 隔离构建变体。跳过包括异平台专用用例；当前 Windows 外层 Job 的独立后台启动限制在原日志中保留，不计为成功。

两个平台的新包均重复生成两次，归档字节一致；新目录解包后核对完整文件集合、摘要和 Linux 执行权限，再运行实际安装/升级/卸载与运行验证。Windows 交付套件通过 {installed['windows']['python_passed']} 项、跳过 {installed['windows']['python_skipped']} 项；Linux 通过 {installed['linux']['python_passed']} 项、跳过 {installed['linux']['python_skipped']} 项。Linux 归档和程序已复制回 Windows 工作区，摘要核对见 [传输回执](linux-transfer-final.json)。开发产物位置见交付记录；归档不提交到 Git。

按与基线一致的扫描范围，两端无新增残留检查通过。Windows 原有 69 组沙箱 stage/profile 和一个此前拒绝清理的夹具仍保留，未接管或删除。结束扫描一度扩大前缀范围，额外发现 7 个创建/修改时间早于本轮的旧测试目录，详见[扩展范围审计](windows-expanded-scan-audit.json)；它们未进入原始基线，不能宣称本轮前后内容未变，已保留原始失败扫描和这些目录。Linux 本轮扫描范围内无残留。私有 Linux Secret Service 与 Windows 合成凭据命名空间已检查。本机临时安装通过不等于独立干净机器或真实登录验收。

Windows 首次交付套件因包快照跨 API 比较误判而失败，修复前后的最小回归及最终完整安装回执均保留；新增实际同长度修改拒绝用例，未放宽包完整性约束。

验收辅助脚本的两次初始失败分别保留：Linux 资源扫描把不存在的可选目录记为错误，以及 root WSL 打包入口缺少 XMake 环境设置。均修正后通过，不涉及产品源码，未覆盖失败记录。

macOS 真实登录仍待单独验收，完整隔离继续禁用，正式签名、公证和独立干净 Mac 验收仍暂缓。本轮未运行真实模型、真实账号 OAuth、托管 CI、永久安装或发布操作。
''', encoding='utf-8', newline='\n')
print(json.dumps({'status': 'completed', 'source': SOURCE,
                  'configurations': {k: {f: v[f] for f in ('native_groups', 'python_passed', 'python_skipped')} for k, v in configurations.items()},
                  'delivery': {k: {'passed': v['python_passed'], 'skipped': v['python_skipped']} for k, v in installed.items()},
                  'document_links': links, 'python_syntax_files': python_files}))
