# 本地模型接入与能力检查

CodeStudio-Agent 可连接已经启动的本地推理服务。此阶段增加模型列表查询和两轮工具调用能力检查；模型下载、服务安装/启动、显存管理和内嵌推理引擎仍未实现。

## 查看模型

```powershell
xmake run codestudio-agent models
# 非默认端口或服务：
xmake run codestudio-agent models --base-url http://127.0.0.1:1234/v1
```

`models` 默认访问 `http://127.0.0.1:11434/v1/models`，发送 HTTP GET，读取 OpenAI 兼容的 `data[].id`。输出只包含 endpoint、按 id 排序的 models 和 count。空列表表示服务未报告模型，不代表服务失联。列表最多 4096 项，重复 id、非法 JSON 和不符合结构的条目会报错。

兼容 Ollama 0.30.10 在尚未安装模型时返回的 `{"object":"list","data":null}`，将其归一化为 `count:0`、`models:[]`。其他缺失或格式错误的列表仍会拒绝。模型列表为空时，需要先在服务中安装模型，才能运行 `model-check`。

服务必须自己提供 `/models` 接口。列表出现某个模型仅证明服务报告了它；是否已加载、是否需要服务内部自动加载，以及上下文容量都不能从这个最小接口确认。

## 检查模型是否可用于工具调用

```powershell
xmake run codestudio-agent model-check --model '实际模型名'
xmake run codestudio-agent model-check --model '实际模型名' `
  --api responses --base-url http://127.0.0.1:1234/v1
# 某些服务支持非流式工具调用，可以单独检查：
xmake run codestudio-agent model-check --model '实际模型名' --no-stream
```

支持 `--api chat-completions|responses|messages`，默认 Chat Completions；默认启用 SSE 流式。模型必须通过 `--model` 显式指定。默认查询超时 5000 ms、检查每轮请求超时 60000 ms，可通过 `--timeout-ms` 调整。

检查复用正常推理的协议适配器，执行以下模型请求：

1. 要求模型调用唯一的合成函数 `connection_probe`，参数必须精确匹配指定 token。
2. 将合成函数结果放入下一轮上下文，检查模型是否准确返回该结果中的 acknowledgment。

不会运行模型要求的任何工具，不会读取项目内容，不会创建任务数据库。请求可能触发本地服务自己的模型加载和推理资源消耗。

输出的 `status` 为：

| 状态 | 含义 |
| --- | --- |
| `compatible` | 此次两轮协议和合成工具往返通过 |
| `unsupported` | 返回了有效模型响应，但未按要求调用工具或确认工具结果 |
| `error` | 请求或协议解析失败，包括格式错误、超时、缺少完整流式结束事件 |
| `cancelled` | 用户取消了请求 |

`capabilities.inference`、`tool_call`、`tool_result` 记录已经通过的检查；`streaming` 在流式模式下记录有效 SSE 响应是否完成，非流式检查为 `null`。兼容性通过只说明该模型和服务在此次简单检查中的表现，不保证复杂编码质量、长上下文稳定性、每次都遵循工具协议或所有可选 API 功能。一次 `unsupported` 也不等于模型永远无法使用工具。

`compatible` 退出码为 0，其他检查结果为 1；命令参数、网络地址或模型列表查询错误通常为 2。输出不会包含模型的原始回答，错误仅使用安全的原因代码。

## 地址和密钥

这两个诊断命令只接受字面地址 `127.0.0.1` 或 `[::1]`，支持 HTTP/HTTPS。`localhost` 和其他域名也会拒绝，避免把本地诊断请求发到远端。不会跟随重定向，字面回环连接绕过代理；HTTPS 仍验证证书。

命令忽略 `CODESTUDIO_BASE_URL`、`CODESTUDIO_MODEL`、`CODESTUDIO_API` 等云端配置，默认也不会发送 `CODESTUDIO_API_KEY` 或 `OPENAI_API_KEY`。需要认证的本地服务可显式加 `--use-api-key`，此时仅使用 `CODESTUDIO_API_KEY`：Chat Completions/Responses 和模型列表使用 Bearer，Messages 使用 `x-api-key`。不要为了本地无认证服务加入此选项。

通过检查后，可使用现有 `run` 进行真实任务：

```powershell
# run 使用常规模型配置；无认证本地服务需要清空当前 shell 的密钥。
$env:CODESTUDIO_API_KEY = ''
xmake run codestudio-agent run --workspace . --model '实际模型名' `
  --api chat-completions --base-url http://127.0.0.1:11434/v1 `
  --allow-http-loopback --prompt '阅读 README.md，说明构建步骤。'
```

Ollama、LM Studio、llama.cpp server 可以作为对接目标，但具体版本、启动配置、模型模板和工具调用能力必须逐一确认。本仓库自动化测试使用回环模拟服务验证三种 API 的流式和非流式合成往返；未据此宣称任何已安装本地模型、GPU 或真实推理服务已经通过验证。
