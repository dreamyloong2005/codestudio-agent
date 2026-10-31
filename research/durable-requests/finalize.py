"""Consolidate this milestone's receipts without rewriting historical evidence."""
import ast
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EVIDENCE = ROOT / "research/durable-requests"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write(name, value):
    (EVIDENCE / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


spec = importlib.util.spec_from_file_location("verification", ROOT / "scripts/verify.py")
verification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verification)
frozen = json.loads((EVIDENCE / "source-freeze.json").read_text())
assert verification.source_fingerprint() == frozen
reports = []
for path in sorted(EVIDENCE.glob("*/*/verification.json")):
    report = json.loads(path.read_text())
    assert report["status"] == "succeeded" and report["source"] == frozen
    steps = report["steps"]
    reports.append({
        "platform": report["platform"], "mode": report["mode"], "receipt": path.relative_to(ROOT).as_posix(),
        "native_groups": next(step["native_groups"] for step in steps if step["name"] == "native"),
        "python_passed": sum(step.get("passed", 0) for step in steps),
        "skipped": sum(step.get("skipped", 0) for step in steps),
        "durable_requests": next(step["passed"] for step in steps if step["name"] == "durable_requests"),
        "artifact": report["artifact"], "omitted_suites": report["omitted_suites"],
    })
assert {(r["platform"], r["mode"]) for r in reports} == {
    (platform, mode) for platform in ("windows", "linux") for mode in ("debug", "release")}
for report in reports:
    if report["platform"] == "windows":
        assert sha((ROOT / report["artifact"]["path"]).read_bytes()) == report["artifact"]["sha256"]
write("verification.json", {"status": "succeeded", "tested_source": frozen, "configurations": reports})

# Two final presentation-only corrections, recorded separately from tested bytes.
review = json.loads((EVIDENCE / "static-review.json").read_text())
readme = ROOT / "README.md"
before_readme = readme.read_bytes()
replacement = review["findings"][0]["suggested_replacement"]
old = review["schema_consistency"]["stale_method_count_occurrences_excluding_historical_validation"][0]["text"]
assert before_readme.count(old.encode()) == 1
readme.write_bytes(before_readme.replace(old.encode(), replacement.encode()))
script = ROOT / "scripts/verify.py"
before_script = script.read_bytes()
after_script = before_script.replace(b"\r\n", b"\n")
assert ast.dump(ast.parse(before_script)) == ast.dump(ast.parse(after_script))
script.write_bytes(after_script)
baseline = json.loads((EVIDENCE / "implementation-freeze.json").read_text())
differences = [name for name, digest in baseline.items() if sha((ROOT / name).read_bytes()) != digest]
assert differences == ["scripts/verify.py"]
write("post-verification-adjustments.json", {
    "tested_source": frozen, "final_source": verification.source_fingerprint(),
    "changes": [
        {"path": "README.md", "before": sha(before_readme), "after": sha(readme.read_bytes()),
         "reason": "Correct current method count and document durable client entry points"},
        {"path": "scripts/verify.py", "before": sha(before_script), "after": sha(after_script),
         "reason": "Restore original LF line endings", "python_ast_identical": True,
         "only_crlf_to_lf": before_script.replace(b"\r\n", b"\n") == after_script},
    ],
    "all_other_implementation_files_match_tested_snapshot": True,
    "scope": "Full suites certify tested_source. Final source differs only by this documented README edit and Python newline normalization; runtime, tests, protocol and example bytes are unchanged.",
})
rows = "\n".join(f"| {r['platform']} {r['mode']} | {r['native_groups']} | {r['python_passed']} | {r['skipped']} |" for r in reports)
text = f"""# 持久请求准入与跨重启回执

实现 tasks/start 与 tasks/resume 的持久请求去重、显式数据库初始化、数据库/工作区身份校验、跨重启回执查询及原子任务关联。状态库为 schema 8，旧库拒绝且不迁移；stdio 协议与检查点仍为 2。请求不自动过期，最多保留 10,000 条；未完成回执不表示进程仍活跃，也不授权重放未知效果。

完整回归使用同一 {frozen['files']} 文件源码快照 `{frozen['sha256']}`，运行实际 PowerShell/Bash 验证入口。汇总见 [verification.json](verification.json)。

| 配置 | 原生测试组 | Python 通过 | 跳过 |
|---|---:|---:|---:|
{rows}

每个配置均通过 23 项新增持久请求集成测试，包括丢失接受响应、并发重复请求、缓存淘汰后查询、服务崩溃、原子任务/检查点/续跑关联、准入与最终回执 SQL 故障、凭据脱敏、跨工作区冲突和数据库替换。原生新增测试覆盖事务回滚、保留上限、并发登记及内核执行前关联。其他套件覆盖模型/MCP 合成协议、隔离、插件、恢复、调度、cron、自启登记及打包/验证工具；Windows 每配置另通过两个真实 XMake 隔离构建变体。跳过项保持原始回执，不计为成功。

首次原生失败为测试临时目录别名未规范化；修正夹具后通过，生产路径等值校验未放宽。复核发现跨工作区 requests/get 查询缺少绑定校验；[修复前复现](workspace-query-red.log) 与 [修复后完整专项](durable-windows-debug-final.log) 保留。SQL 故障通过测试触发器注入，无生产故障开关。

完整验证之后仅修正 README 方法数量/客户端说明，并将 verify.py 恢复原 LF；该脚本 Python AST 完全相同，其余实现、协议、测试和示例字节未变。精确前后摘要和区别见 [post-verification-adjustments.json](post-verification-adjustments.json)，不将后续文档字节冒充已测试源码快照。

资源检查沿用本轮起始扫描范围；Windows 的历史沙箱资源与此前拒绝清理的夹具保持原样，未接管或删除。不宣称总残留为零。最终扫描和客户端示例结果见 [完成回执](completion.json)。Linux 私有凭据测试服务完成名称扫描后退出，未使用桌面凭据会话。

本轮没有新建安装归档或执行安装交付套件，旧包仍对应历史源码；也没有真实模型/OAuth、永久自启登记、真实登录/重启、托管 CI、macOS 原生验收、签名、公证、提交或推送。macOS 的历史原生验收不能替代 schema 8 的实际 macOS 验证。
"""
(EVIDENCE / "README.md").write_text(text, encoding="utf-8")
print(json.dumps({"configurations": len(reports), "final_source": verification.source_fingerprint()}))
