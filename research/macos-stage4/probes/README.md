# 阶段 4 初轮探针

这些记录为正式冻结源码之前的开发联调，部分包仍引用阶段 3 原生构建回执，不是最终阶段 4 产物验收。

- runtime-relocation：同一输入两次副本重定位字节相同，缺失私有库时在 dyld 阶段拒绝；未改源程序或运行库。
- mapped-identity：自有 C 夹具被重命名并在旧路径创建新文件后，内核返回的映射 vnode 仍绑定原文件。夹具通过 stdin EOF 退出，未对无关进程发信号；附原探针源码。
- installer-kqueue-failure：32 项中一项因本机 Python 的 kqueue 不支持 context manager 失败，后续改为显式 finally close。该失败独立保留。
- installer-development：修复后真实 32 项临时安装联调通过，但原开发包先于最终独立快照/CLI 小修。最终结果以顶层正式回执为准。
- receipt-alias：物理卷别名指向包内时在运行前拒绝，父目录被移入包/原位置被替换也不会写入；定向结果保留。
- receipt-mutation：选中的一个测试虽通过，结束时原包字节变动仍导致整体回执 failed。初始摘要和选中测试 ID 被保留。

所有成功仅覆盖本机开发流程，不表示 Developer ID、Apple 公证、独立干净机器或真实登录通过。
