# CodeStudio Agent macOS 阶段 4

本轮交付 macOS arm64 打包、随包运行库、用户级安装/升级/卸载、独立包验收脚本，以及 Developer ID 签名、公证和 DMG 发布工具。**工具实现与本机测试不等于正式发布验收。** 当前选择继续原生 macOS，阶段 3 未满足契约的隔离能力保持禁用；阶段 2 的真实登录验收仍单列。

用户本轮确认先完成本地交付，不使用本机检测到的 Developer ID 身份；暂无 notarytool profile 和独立干净 Mac。本地交付已完成，正式签名、公证、独立机器验收暂缓，`stage4_accepted` 保持 false。

最终 Debug/Release 分别通过 22 组原生测试、376 项 Python 测试，11 项跳过。两次开发归档字节一致；实际归档解包后的交付套件和最终 DMG 挂载内容中的独立交付套件分别通过 33/33。DMG 使用 ad-hoc 签名，仅作为开发测试产物。

## 实现与验证层次

- `package.py` 在副本上重定位专用 libc++/libc++abi，拒绝未知非系统依赖，去除构建机 RPATH，随附依赖许可和完整摘要。原生构建字节与重定位后字节分别记录。
- `install.py` 提供 Darwin 原生身份观察、大小写/Unicode/firmlink 路径保护、独占发布、继承 ACL 清除和 quarantine 保留。安装不修改默认 PATH、自启或凭据；未知或修改过的文件拒绝删除。
- 包内 `verify-delivery.py --standalone` 可在无源码/编译工具的环境验证临时安装、运行、升级、卸载与失败边界。它核对测试前后整包字节，记录选中测试范围，不自动证明独立干净机器。
- `release-macos.py prepare` 只修改新目录内的包副本。正式分支使用明确的 Developer ID、固定代码身份、同 Team 与 hardened runtime；开发 ad-hoc 分支只用于本地运行测试。
- `notarize` 才上传 Apple。先保存提交意图，未知结果禁止盲目重传，接受日志须绑定原 DMG 摘要。装订操作作用于独立副本，最终验证后独占发布；原提交字节保持不变。失败、保留候选与恢复结果分别记录。

[verification.json](verification.json) 分别记录原生 Debug/Release、打包与归档重现、实际归档安装、ad-hoc DMG、正式签名/公证以及独立机器验收状态。逐套件原始日志在 `debug/`、`release/` 和 `delivery/`。归档记录见 [package/packaging.json](package/packaging.json)，最终 DMG 挂载和独立验证见 [dmg-development/dmg-mounted-verification.json](dmg-development/dmg-mounted-verification.json) 与 [dmg-development/dmg-delivery.json](dmg-development/dmg-delivery.json)。完整隔离套件仍明确跳过，不能计为成功。

正式公证需要明确选定的 Developer ID 发布者身份和已保存的 notarytool Keychain profile；独立干净 Mac 需要额外运行环境证据。未执行的步骤一律为 pending/not performed，不从测试替身、ad-hoc 签名、版本输出、构建机换目录运行或 CI 定义推导通过。完整操作说明见[交付文档](../../docs/delivery.md)。

## 证据边界

本轮本机为 macOS 26.5.2 arm64。15.0 是部署目标，Intel、Rosetta、Universal 和另一系统版本没有由本机回执覆盖。开发 tar.gz 在同一输入和元数据下比较字节重现；带时间戳签名、公证票据和磁盘镜像不承诺跨次字节相同。

初轮探针曾发现 ad-hoc 程序启用 hardened runtime 后无法加载无 Team 的私有运行库，因此开发分支改为不启用该选项，并增加签名后的真实 version/doctor 检查。该负例保留为失败证据，不计入最终可运行产物。签名/公证测试中的 Apple 响应替身仅验证错误处理和恢复状态机，不代表向 Apple 提交过产物。

测试仅使用自有临时安装、工作区和进程，真实后台 worker 由产品停止命令收敛；挂载的自有 DMG 已核对并卸载。[残留观察](residue-after.json) 在声明范围内发现 0 个测试目录、0 个任务镜像挂载和 0 个 Agent/夹具进程候选。研究产物、失败候选和开发包保留在 `build/`，不计为产品资源回收。该快照不证明全系统或历史残留为空。

最终挂载研究脚本第一次将 `hdiutil attach/info` 的整个记录比较，因可选字段不同而在运行测试前拒绝。随后核对具体镜像、设备和挂载点后安全卸载，修正研究脚本，最终 33/33 通过；[初次失败记录](dmg-development/dmg-mount-harness-initial-failure.json) 保留。产品源码未因此改变。

`package/` 与 `dmg-development/` 中的研究脚本是当时在 `build/macos-stage4-final/` 执行的存档，保留原始路径假设；复现产品流程请使用交付文档中的正式入口。

原始阶段 2 登录夹具继续使用固定的 `build/macos-stage2-login-runtime/` 副本；本轮没有自动注销、登录、重启或替换其待验收字节。
