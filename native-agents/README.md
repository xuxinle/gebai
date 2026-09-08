# 多语言子代理（Native Agents）

任意语言实现的子代理：**放置即自动发现 → 启动边车进程 → 握手拉取工具清单 → 注册为标准子代理**（`agent_list` 可见、`agent_load` 装载、`agent_run` 委派——与 TS 子代理完全同构）。Python/C++/Go/Rust 等任何能读写 stdio 的语言均可接入。

**设计原则：实现语言对模型透明**——子代理 = 工具 + 提示词（能力导向命名与描述），语言仅是工程组织维度；一种语言可派生任意多个子代理项目（共享基础框架驱动，项目只写专属工具）。

## 目录结构（按语言组织）

```
native-agents/                    # 仓库根（构建复制到 dist/，二进制形态物化 {GEBAI_HOME}/vendor/native-agents/）
├── README.md
├── python/                       # Python 语言目录
│   ├── driver.py                 # 基础框架：协议 v1 + REPL 引擎 + pip + tools.py 合并
│   ├── venv/                     # 语言 venv（python_pip 自动创建/维护；gitignore）
│   ├── requirements.txt          # 依赖清单（pip freeze 维护；gitignore）
│   ├── python/                   # 子代理项目：Python 生态执行（run/pip/status）
│   └── pyregex/                  # 子代理项目：正则工具（tools.py 合并示例：run/pip/status + match/findall/sub）
├── cpp/                          # C++ 语言目录
│   ├── framework.hpp             # 基础框架：头文件式（协议 + 迷你 JSON + 工具注册表）
│   ├── build.bat / build.sh      # 构建脚本（vswhere→cl / g++）
│   └── mathx/                    # 子代理项目：数学表达式求值（main.cpp + agent.json + PROMPT.md）
└── rust/                         # Rust 语言目录
    ├── framework.rs              # 基础框架：单文件零依赖（协议 + 迷你 JSON + 注册表）
    └── codec/                    # 子代理项目：base64/CRC-32 编解码（main.rs + agent.json + PROMPT.md）

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
| `description` | ✓ | 一句话能力描述（agent_list/系统提示词注入用；能力导向，语言仅作次要说明） |
| `protocol` | ✓ | 协议版本（当前 `1`） |
| `command` | ✓ | 启动命令（字符串数组；占位符见下表） |
| `driver` | | command 引用的驱动脚本文件名（相对 manifest 目录；`{driver}` 占位解析用） |
| `prompt` | | 系统提示词文件名（缺省 `PROMPT.md`；正文注入子代理系统提示词，frontmatter 剥离） |
| `cwd` | | 工作目录（缺省 manifest 目录；支持 `{GEBAI_HOME}` 占位） |
| `env` | | 附加环境变量（值支持 `{GEBAI_HOME}` 占位；`GEBAI_HOME`/`GEBAI_AGENT_DIR` 总是注入） |
| `build` | | 编译型语言构建引导（见下节） |

### command 占位符

| 占位符 | 展开 |
|--------|------|
| `{python}` | 解释器命令（`GEBAI_PYTHON_DIR` → `native-agents/python/venv` → 系统 PATH） |
| `{driver}` | driver 字段声明的脚本绝对路径 |
| `{agent_dir}` | manifest 所在目录（子代理项目目录） |
| `{lang_dir}` | 语言目录（agent_dir 上一级——基础框架驱动所在） |
| `{agent_name}` | manifest name |
| `{exe}` | Windows 展开 `.exe`，其余平台空串（跨平台可执行体引用） |
| `{GEBAI_HOME}` | 数据根 |

## 构建引导（编译型语言开箱即用）

manifest `build` 字段声明编译命令，command 首元素指向的可执行文件**不存在时自动执行**（占位符同 command，cwd 为 manifest 目录），产物落盘后正常启动：

```json
{
  "name": "codec",
  "command": ["{agent_dir}/driver{exe}"],
  "build": {
    "command": ["rustc", "--edition", "2021", "-O", "-o", "{agent_dir}/driver{exe}", "{agent_dir}/main.rs"]
  }
}
```

- `command`（跨平台通用）/ `windows` / `unix` 平台分支任选；缺编译器的环境记 loadErrors（模型可见根因），不阻断其他子代理
- 构建超时 300s；失败如实报错（含编译输出尾部 30 行）
- 可执行体已存在则跳过构建（增量：改源码需手动删可执行体或重跑构建脚本）

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

- **发现即启动**：`SubAgentManager.discover()` 尾部并行启动全部 native 边车（manifest/驱动/提示词文件变化触发重扫重注册——热加载；venv/__pycache__/编译产物不触发）
- **仅本地形态**：沙箱启用（服务端部署）或 `GEBAI_NATIVE_AGENTS=off` 时整体禁用（对 `agent_list` 完全不可见）
- **失败安全**：manifest 损坏/边车启动失败/握手失败/构建失败 → 该项记入 loadErrors（模型可见根因），不影响其他子代理与主流程
- 工具调用恒需审批（任意代码执行面）

## 内置子代理（4 个）

| 子代理 | 语言 | 工具 |
|--------|------|------|
| `python` | Python | `python_run`（常驻命名空间 REPL）/ `python_pip`（依赖管理）/ `python_status` |
| `pyregex` | Python | `pyregex_match` / `pyregex_findall` / `pyregex_sub`（+ 基础 run/pip/status，tools.py 合并） |
| `mathx` | C++ | `mathx_eval`（表达式求值）/ `mathx_eval_batch` / `mathx_stats` |
| `codec` | Rust | `codec_b64_encode` / `codec_b64_decode` / `codec_crc32` |

## 三语言基础框架

### Python（native-agents/python/driver.py）

语言目录共享驱动：协议 + REPL 引擎（末行表达式求值 repr 回显、session 命名空间保持）+ pip 工具（venv/requirements 落语言目录）。子代理项目可选携带 `tools.py`（导出 `AGENT_NAME` + `TOOLS` + `TOOL_IMPLS`），启动时自动加载与基础工具合并（同名覆盖）——pyregex 即此模式。

### C++（native-agents/cpp/framework.hpp）

头文件式框架：手写迷你 JSON（解析/序列化，含 `\uXXXX` 与代理对）、NDJSON 行循环、工具注册表。子代理项目 `#include "../framework.hpp"`，工具写普通函数后 `main()` 前集中注册（`REGISTER_TOOL` 宏对含逗号的 schema/lambda 有预处理器拆参陷阱，集中注册最稳）。构建：Windows 用 `build.bat`（vswhere 定位 MSVC）/ unix 用 `build.sh`（g++/clang++）。

### Rust（native-agents/rust/framework.rs）

单文件零依赖（纯标准库）：迷你 JSON（char 流解析，代理对支持）、`register_tool` 注册（Mutex 内部可变性）、NDJSON 行循环。子代理项目 `#[path = "../framework.rs"] mod framework;` 引入。构建：`rustc --edition 2021 -O` 直编（无需 cargo 工程）。

## 任意语言接入示例（Go 心算代理）

```json
{ "name": "gomath", "description": "高性能计算", "protocol": "1",
  "command": ["{agent_dir}/gomath{exe}"],
  "build": { "unix": ["go", "build", "-o", "{agent_dir}/driver{exe}", "{agent_dir}/main.go"] } }
```

实现 4 个 op（init/tools.list/tool.call + 错误响应），按行读写 stdio 即可。放一个目录（agent.json + 驱动 + PROMPT.md）即成一个子代理。

## venv 与依赖位置（迁移说明）

Python venv 与 requirements.txt 原位于 `{GEBAI_HOME}/venv`（dev 模式即仓库根），现归位语言目录 `native-agents/python/`（与驱动同居）：

- 解释器解析顺序：`GEBAI_PYTHON_DIR` → 语言目录 venv（`native-agents/python/venv`）→ PATH
- `python_pip` install/freeze 均操作语言目录（freeze 统一写回 `native-agents/python/requirements.txt`）
- 构建复制 dist 时过滤 venv/__pycache__/编译产物——部署产物只带源码与 manifest
