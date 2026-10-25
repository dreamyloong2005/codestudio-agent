# XMake 隔离构建测试夹具

此目录仅供 [真实构建集成测试](../../xmake_build_integration.py) 使用，验证通用执行器可以在 Windows LPAC 中配置、编译、运行和导出一个 C++26 工程。

- `build.windows.json` 保存测试所需的 XMake 显式步骤；不是产品默认模板。
- `@XMAKE_DIRECTORY@` 和 `@LLVM_BIN_DIRECTORY@` 仅为测试占位符。测试脚本将其替换为已安装工具的目录，产品不会解析它们。
- `xmake.lua` 和 `main.cpp` 是无需 SDK/STL 下载的最小工程，通过 C++26 pack indexing 验证语言特性。

从仓库根目录运行：

```powershell
python tests/xmake_build_integration.py build/windows/x64/release/codestudio-agent.exe
```

脚本从 `PATH` 查找 `xmake` 和 `clang-cl`；也可在 CLI 路径后依次传入两个工具的可执行文件路径。脚本在临时工作区读取 v2 清单，分别验证夹具内声明的缓存目录和显式自定义缓存目录，不下载工具或依赖。所有 XMake 环境配置均来自夹具，宿主不注入工具默认值。
