# CodeStudio Agent macOS 阶段 2 验证

本目录保存本机 macOS 26.5.2 arm64 的系统集成验证证据。`verification.json` 汇总完整 Debug/Release 回执；每个配置的原始日志、源码指纹、产物摘要和运行库摘要保存在同名子目录。

系统集成覆盖非交互 Keychain、独立 OAuth 记录/刷新锁、后台 worker、有界监督、LaunchAgent 登记和真实后台 cron。隔离与正式交付仍属于阶段 3/4，不将跳过项计为成功。macOS 15.0 只是部署目标，本轮未验证该版本或 Intel。

真实 GUI 注销/登录验收单列为 pending。`scripts/verify-macos-login.py` 的 prepare 只登记普通/监督两个独立 fixture，不启动 worker；重新登录后运行 verify，核对新会话、launchd 父子关系、固定产物、禁用不停止现有进程及停止后不重启。静态 plist、手动 bootstrap、相同会话的后台启动均不替代此验收。探针要求操作者遵守不手动启动的流程，事后进程信息本身不能证明触发来源。

测试仅使用本地模型/OAuth 夹具及随机自有凭据。锁定测试使用独立临时 Keychain，不锁定用户登录库，不改变默认库/搜索列表。未请求真实账号、未自动注销用户、未修改系统 Disabled override。
