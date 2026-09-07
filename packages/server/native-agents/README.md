# 多语言子代理（Native Agents）

任意语言实现的子代理：**放置即自动发现 → 启动边车进程 → 握手拉取工具清单 → 注册为标准子代理**（`agent_list` 可见、`agent_load` 装载、`agent_run` 委派——与 TS 子代理完全同构）。Python/C++/Go/Rust 等任何能读写 stdio 的语言均可接入。

## 目录结构

```
packages/server/native-agents/     # 内置源（构建复制到 dist/，二进制形态物化 {GEBAI_HOME}/vendor/native-agents/）
└── python/                        # 内置：Python 生态子代理
    ├── agent.json                 # manifest
    ├── driver.py                  # 边车协议实现
    └── PROMPT.md                  # 系统提示词

{GEBAI_HOME}/agents/               # 用户自建（放置即生效；同名覆盖内置）
└── my-agent/
    ├── agent.json
    ├── main.py / main.exe / …     # 任意语言驱动
    └── PROMPT.md
```

## manifest（agent.json）

| 字段 | 必填 | 说明 |
|------|------|------|
| `name` | ✓ | 子代理名（`[a-z0-9_]+`，工具注册为 `{name}_{tool}`） |
| `description` | ✓ | 一句话能力描述（agent_list/系统提示词注入用） |
| `protocol` | ✓ | 协议版本（当前 `1`） |
| `command` | ✓ | 启动命令（字符串数组；占位符：`{python}`=解释器命令、`{driver}`=driver 脚本绝对路径、`{GEBAI_HOME}`） |
| `driver` | | command 引用的驱动脚本文件名（相对 manifest 目录；`{driver}` 占位解析用） |
| `prompt` | | 系统提示词文件名（缺省 `PROMPT.md`；正文注入子代理系统提示词，frontmatter 剥离） |
| `cwd` | | 工作目录（缺省 manifest 目录；支持 `{GEBAI_HOME}` 占位） |
| `env` | | 附加环境变量（值支持 `{GEBAI_HOME}` 占位；`GEBAI_HOME` 总是注入） |

## 边车协议 v1（NDJSON over stdio）

stdin/stdout 各一行一个 JSON 对象（UTF-8）。**stdout 只写协议行**（程序自身的 print 全部捕获，不得直写 stdout）；stderr 自由文本（宿主环形缓冲，排障用）；**stdin EOF → 立即退出**（父进程已死，防孤儿进程）。

```
→ {"id":1,"op":"init"}
← {"id":1,"ok":true,"result":{"name":"myagent","protocol":1, ...自由扩展字段}}

→ {"id":2,"op":"tools.list"}
← {"id":2,"ok":true,"result":[{"name":"echo","description":"回显","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}]}

→ {"id":3,"op":"tool.call","args":{"tool":"echo","args":{"text":"hi"}}}
← {"id":3,"ok":true,"result":{"output":"回显：hi","data":{...可选结构化...}}}

错误：← {"id":3,"ok":false,"error":"原因文本"}
```

约定：

- `init` 的 `result.name` 必须与 manifest `name` 一致（不一致拒绝注册）
- 工具名为**裸名**（无 `{agent}_` 前缀——注册表注册时自动加前缀）
- `parameters` 为 JSON Schema（`type`/`properties`/`required`），原样透传给模型
- `tool.call` 的 `result.output` 为给模型看的文本；`data` 可选结构化输出
- 不设内部超时（宿主侧超时：默认 120s，tool.call 可按调用参数 `timeout` 秒延长）
- 进程意外退出由宿主自动重启（崩溃自愈，在途请求重发一次）；连续快速退出 3 次放弃自动重启（防抖动风暴），下次调用再拉起

## 宿主行为（src/core/agents/sidecar.ts）

- 惰性启动、请求 id 配对（并发复用同一进程）、启动串行化
- 请求超时：杀进程重启、该次请求拒绝
- 崩溃自愈：进程意外退出自动重启一次 + 在途请求重发一次
- 服务退出清理（exit hook）+ 驱动侧 stdin EOF 自杀 双保险
- stderr 环形缓冲 16KB（错误信息附因）

## 生命周期与门控

- **发现即启动**：`SubAgentManager.discover()` 尾部并行启动全部 native 边车（manifest/驱动/提示词文件变化触发重扫重注册——热加载）
- **仅本地形态**：沙箱启用（服务端部署）或 `GEBAI_NATIVE_AGENTS=off` 时整体禁用（对 `agent_list` 完全不可见）
- **失败安全**：manifest 损坏/边车启动失败/握手失败 → 该项记入 loadErrors（模型可见根因），不影响其他子代理与主流程
- 工具调用恒需审批（任意代码执行面）

## 任意语言接入示例（C++ 心算代理）

```json
{ "name": "cppmath", "description": "C++ 高性能计算", "protocol": "1",
  "command": ["{agent_dir}/cppmath.exe"], "cwd": "{agent_dir}" }
```

实现 4 个 op（init/tools.list/tool.call + 错误响应），按行读写 stdio 即可。构建产物放入 `{GEBAI_HOME}/agents/cppmath/`。

## 内置 Python 子代理

- `python_run`：常驻命名空间执行（REPL 语义——import 一次多次复用，AI 库秒级导入成本只付一次）
- `python_pip`：依赖管理（venv 自动创建于 `{GEBAI_HOME}/venv`，requirements.txt 快照/安装；装完边车自动重启加载新依赖）
- `python_status`：边车/venv/包状态报告
- 解释器解析：`GEBAI_PYTHON_DIR` → `{GEBAI_HOME}/venv` → 系统 PATH；协议层零 pip 依赖（纯标准库）
