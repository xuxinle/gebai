# 客卿（Keqing）

客卿——战国之制：六国士人入秦，不问出身、拜为卿客，与本土重臣同殿议政。此处指多语言子代理（任意语言实现的边车进程，与 TS 子代理同构同殿）：**放置即自动发现 → 启动边车进程 → 握手拉取工具清单 → 注册为标准子代理**（`agent_list` 可见、`agent_load` 装载、`subsession_run` 委派——与 TS 子代理完全同构）。Python/C++/Go/Rust 等任何能读写 stdio 的语言均可接入。

**设计原则：实现语言对模型透明**——子代理 = 工具 + 提示词（能力导向命名与描述），语言仅是工程组织维度；一种语言可派生任意多个子代理项目（共享基础框架驱动，项目只写专属工具）。

## 目录结构（按语言组织）

```
keqing/                    # 仓库根（构建复制到 dist/，二进制形态物化 {GEBAI_HOME}/vendor/keqing/）
├── README.md
├── python/                       # Python 语言目录
│   ├── driver.py                 # 基础框架：协议 + REPL 引擎 + pip + tools.py 合并
│   ├── venv/                     # 语言 venv（{agent}_pip 自动创建/维护；gitignore）
│   ├── requirements.txt          # 依赖清单（pip freeze 维护；gitignore）
│   └── vision/                   # 子代理项目：本地视觉识别（tools.py 合并模式：ocr/locate/locate_image/detect
│       ├── agent.json            #   + 基础 run/pip/status；与 TS 侧 vision 跨语言合并）
│       ├── PROMPT.md
│       └── tools.py              # onnxruntime 原生推理（OCR/文字定位/模板匹配/目标检测）
├── cpp/                          # C++ 语言目录
│   ├── framework.hpp             # 基础框架：头文件式（协议 + 迷你 JSON + 工具注册表）
│   ├── build.bat / build.sh      # 构建脚本（vswhere→cl / g++；带 stb include 路径）
│   ├── stb/                      # stb 单头库 vendor（stb_image / stb_image_write / stb_image_resize2）
│   └── imgproc/                  # 子代理项目：图像处理（info/grayscale/resize/stats）
├── rust/                         # Rust 语言目录（cargo workspace 统一管理）
│   ├── Cargo.toml                # workspace 根（members: framework, nsight, torch）
│   ├── framework/               # 库 crate gebai-keqing-framework（协议实现共享）
│   │   └── src/lib.rs
│   ├── nsight/                   # bin crate 子代理项目：Nsight 报告分析（nsys 时间线 + ncu 单内核）
│   │   ├── Cargo.toml
│   │   ├── agent.json            # command → {lang_dir}/target/release/nsight{exe}
│   │   └── src/main.rs           # 只写工具逻辑（依赖 framework crate）
│   └── torch/                    # bin crate 子代理项目：PyTorch Profiler trace 原生聚合（分块并行）
│       ├── Cargo.toml
│       ├── agent.json            # command → {lang_dir}/target/release/torch{exe}
│       └── src/main.rs
└── go/                           # Go 语言目录（go module 统一管理）
    ├── go.mod                    # module gebai/keqing-framework
    ├── framework/framework.go    # 基础框架包（标准库 encoding/json，零手写 JSON）
    └── disk/                     # 子代理项目：磁盘使用分析与清理（tree/du/top/depth/volumes/scan/clean/trash）
        ├── agent.json            # command → {agent_dir}/driver{exe}；build → go build（目录构建，支持多文件）
        ├── PROMPT.md
        ├── main.go               # 分析工具与注册（import fw "gebai/keqing-framework/framework"）
        ├── clean.go              # 清理：候选扫描 / 执行（预览·隔离·删除）/ 隔离区管理
        ├── volumes*.go           # 容量总览（平台实现按构建标签分文件）
        └── main_test.go          # 单元测试（go test ./disk）

{GEBAI_HOME}/agents/               # 用户自建（放置即生效；manifest 同名去重时用户自建胜出；与 TS 子代理同名则跨语言合并）
└── my-agent/
    ├── agent.json
    ├── main.py / main.exe / …     # 任意语言驱动
    └── PROMPT.md                  # 可选（缺失即本侧不贡献提示词，交由合并层）
```

## manifest（agent.json）

| 字段 | 必填 | 说明 |
|------|------|------|
| `name` | ✓ | 子代理名（`[a-z0-9_]+`，工具注册为 `{name}_{tool}`） |
| `description` | | 一句话能力描述（agent_list/系统提示词注入用；能力导向，语言仅作次要说明）。**可省略/留空**：留空即「本侧不贡献」，与同名 TS 子代理跨语言合并时由另一侧提供或合并层兜底 |
| `protocol` | ✓ | 协议版本（当前 `2`——tool.call 携带请求级 ctx） |
| `command` | ✓ | 启动命令（字符串数组；占位符见下表） |
| `driver` | | command 引用的驱动脚本文件名（相对 manifest 目录；`{driver}` 占位解析用） |
| `prompt` | | 系统提示词文件名（缺省 `PROMPT.md`；正文注入子代理系统提示词，frontmatter 剥离；**缺失/留空即本侧不贡献**，与 description 同语义——交给合并层兜底） |
| `cwd` | | 工作目录（缺省 manifest 目录；支持 `{GEBAI_HOME}` 占位） |
| `env` | | 附加环境变量（值支持 `{GEBAI_HOME}` 占位；`GEBAI_HOME`/`GEBAI_AGENT_DIR` 总是注入） |
| `build` | | 编译型语言构建引导（见下节） |
| `requiresApproval` | | 全部工具审批策略：缺省 `true`（任意代码执行面恒需审批）；只读识别类子代理可声明 `false`（如 vision） |

### command 占位符

| 占位符 | 展开 |
|--------|------|
| `{python}` | 解释器命令（`GEBAI_PYTHON_DIR` → `keqing/python/venv` → 系统 PATH） |
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

## 边车协议 v2（NDJSON over stdio）

stdin/stdout 各一行一个 JSON 对象（UTF-8）。**stdout 只写协议行**（程序自身的 print 全部捕获，不得直写 stdout）；stderr 自由文本（宿主环形缓冲，排障用）；**stdin EOF → 立即退出**（父进程已死，防孤儿进程）。

```
→ {"id":1,"op":"init"}
← {"id":1,"ok":true,"result":{"name":"myagent","protocol":2, ...自由扩展字段}}

→ {"id":2,"op":"tools.list"}
← {"id":2,"ok":true,"result":[{"name":"echo","description":"回显","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}]}

→ {"id":3,"op":"tool.call","tool":"echo","args":{"text":"hi"},
   "ctx":{"sessionId":"a1b2","user":"admin","cwd":"C:/…/session/tmp","env":{"GEBAI_VISION_MODEL":"…"},"sandboxed":false}}
← {"id":3,"ok":true,"result":{"output":"回显：hi","data":{...可选结构化...}}}

错误：← {"id":3,"ok":false,"error":"原因文本"}
```

约定：

- `init` 的 `result.name` 必须与 manifest `name` 一致，`result.protocol` 必须与宿主一致（当前 `2`；锁步升级不做历史兼容，不匹配即注册失败记 loadErrors）
- **请求级 ctx（v2 核心）**：边车为**进程单例、跨会话共享**，会话上下文只能随 `tool.call` 请求传递：
  - `ctx.cwd`：会话工作区绝对路径——驱动内相对路径的解析基准（框架提供 `ctx_resolve(path)` 助手；宿主发来的路径参数通常已解析为绝对路径，直接用即可）
  - `ctx.env`：任务级环境变量覆盖（随消息变、随调用传）——框架提供 `ctx_env(key)` 助手（先查它、再回落进程 env）；**禁止写入 os.environ**（并发请求不同会话会互踩，进程全局态承载不了请求级数据）
  - `ctx.sessionId` / `ctx.user` / `ctx.sandboxed`：会话标识（会话态隔离键，如 REPL 命名空间分桶）、用户、沙箱标记
  - 分发循环串行执行，框架在循环内设「当前请求 ctx」供工具函数读取
- 工具名为**裸名**（无 `{agent}_` 前缀——注册表注册时自动加前缀）
- `parameters` 为 JSON Schema（`type`/`properties`/`required`），原样透传给模型
- `tool.call` 的 `result.output` 为给模型看的文本；`data` 可选结构化输出
- 不设内部超时（宿主侧超时：默认 300s，tool.call 可按调用参数 `timeout` 秒延长）
- 进程意外退出由宿主自动重启（崩溃自愈，在途请求重发一次）；连续快速退出 3 次放弃自动重启（防抖动风暴），下次调用再拉起

## 跨语言同名合并（TS + 客卿 共同贡献一个子代理）

TS 侧（`packages/agents/src/{name}.ts`）与 客卿 侧（manifest 目录）可**同名共存**：两侧定义作为「贡献集」经 `mergeSubAgentDefs`（`core/agents/merge.ts`，纯函数）合并为同一个子代理，不再同名覆盖。合并语义：

| 字段 | 合并规则 |
|------|----------|
| `description` | 非空项依次拼接（`；`分隔）；全空生成兜底描述 |
| `systemPrompt` | 非空项依次拼接（空行分隔）；全空生成兜底引导句 |
| `tools` | 并集（同名工具保留 TS 侧并告警）；装载后两侧工具同一 `{name}_` 命名空间 |
| `dependencies` / `envVars` | 并集（envVars 同名取首个） |
| `requiresApproval` | 对象合并 |
| `preload` | 取或（任一侧 true 即 true） |
| `projectRoot` / `writeGuard` | 取首个非空（TS 优先） |

配套约定——**「只在一处定义、其他地方留空」**：客卿 manifest 的 `description` 可省略/留空、`PROMPT.md` 可缺失（留空即本侧不贡献该字段，不再生成占位文本）；TS 侧 def 同样可留空 description/systemPrompt。两侧全空时合并层生成兜底描述与引导句（agent_list 恒有可读条目）。

分工样例（内置 `vision`）：TS 侧 `packages/agents/src/agents/vision/vision.ts` 贡献多模态 `analyze`（描述/提示词留空），本地识别四工具（`ocr`/`locate`/`locate_image`/`detect`）与描述/提示词由 Python 边车（`keqing/python/vision/`）贡献——「LLM 耦合的写 TS 侧、重计算的写其他语言侧」，两侧合并为同一子代理。

热加载：任一侧目录签名变化 → 重扫该侧贡献集 → 重算合并视图（未装载会话与新会话生效；已装载会话沿用装载时定义，与 TS 热加载同语义）。卸载时两侧合并工具一并注销。

## 宿主行为（src/core/agents/sidecar.ts）

- 惰性启动、请求 id 配对（并发复用同一进程）、启动串行化
- 请求级 ctx：每次 `tool.call` 携带 `ctx`（sessionId/user/cwd/env/sandboxed，见协议 v2）——进程单例跨会话共享，会话上下文随请求传递
- 请求超时：杀进程重启、该次请求拒绝（下次调用用新进程）
- **惰性生命周期**：发现阶段拿完工具清单即 `stop()`（进程收起，不维持常驻）；进程退出/崩溃**不自动重启**——拒绝在途请求、状态收起，等下次工具调用自动拉起（`request → ensureStarted`）
- 服务退出清理（exit hook）+ 驱动侧 stdin EOF 自杀 双保险
- stderr 环形缓冲 16KB（错误信息附因）

## 生命周期与门控

- **发现即停**：`discoverKeqing` 启动每个边车完成握手（init/tools.list——工具清单必须在进程内才能拿到），**拿完即 `stop()`**——常驻不再存在，进程只在真实工具调用期间存在（`sidecarTool.execute → request → ensureStarted`）
- **仅本地形态**：沙箱启用（服务端部署）或 `GEBAI_KEQING=off` 时整体禁用（对 `agent_list` 完全不可见）
- **失败安全**：manifest 损坏/边车启动失败/握手失败/构建失败 → 该项记入 loadErrors（模型可见根因），不影响其他子代理与主流程
- 工具调用恒需审批（任意代码执行面）

## 边界原则：能力外置、权力内守（TS 引擎核心地位）

客卿把**能力**外置到任意语言，但**权力**一律留在 TS 引擎——实现驱动与二开工具前必读（DESIGN「核心不变式：能力外置、权力内守」）。

- **能做的**：`tools.list` 上报工具清单与 JSON Schema（声明）；`tool.call` 内只读宿主随请求下发的 `ctx`（相对路径经 `ctx_resolve`、环境经 `ctx_env`，禁止写 `os.environ`）；以 `{output, data?}` 回传结果
- **不能做的（权力归引擎）**：
  - **自证授权**——运行时报「本次免审/我有权限」无效：审批姿态恒由宿主判定（`manifest.requiresApproval` 是静态声明，运行期不可自证）
  - **自取工具**——不得自行取用宿主工具，不得自行指定会话 ctx（sessionId/user/cwd/env 一律由宿主按当前请求组装）；会话真相（chat.json/todo/cron 等）不在驱动侧
  - **自定注册名与可见性**——工具名由宿主加 `{agent}_` 前缀，装载门控在宿主 `registry.resolve`
- **当前协议是单向下行**：宿主 → 驱动（init/tools.list/tool.call），驱动只回响应，**无反向调用通道**。驱动内需要宿主能力时由组合解决——TS 侧贡献工具（跨语言同名合并，两侧进同一 `{name}_` 命名空间，见上节）或模型层 `js` 编排
- **未来扩展（反向调用，若做）**：必须是**委托式**——驱动只发意图，宿主按该次 `tool.call` 绑定的会话 ctx 与调用者审批姿态重新裁决，并需①来源可证且不可提权（宿主签发 token，不得转交）②回调白名单（默认只读）。**自取式回调（驱动自主取用工具、自带授权声明）不予准入**——那会把引擎降为消息总线

## 内置子代理（4 个）

| 子代理 | 语言 | 工具 | 典型场景 |
|--------|------|------|----------|
| `vision` | Python + TS | `vision_ocr` / `vision_locate` / `vision_locate_image` / `vision_detect`（+ 基础 run/pip/status，tools.py 合并） | 本地视觉识别（onnxruntime 原生推理；TS 侧贡献多模态 `analyze`，跨语言合并） |
| `imgproc` | C++ | `imgproc_info` / `imgproc_grayscale` / `imgproc_resize` / `imgproc_stats` | 图像处理（stb 单头库） |
| `disk` | Go | `disk_tree` / `disk_du` / `disk_top` / `disk_depth` / `disk_volumes` / `disk_scan` / `disk_clean` / `disk_trash` | 磁盘使用分析与清理（并发遍历；容量总览、清理候选扫描、预览/隔离/删除（范围护栏 + 审计留痕）、隔离区还原与彻底清除） |

Python 语言目录只保留 `vision` 一个项目：基础能力（REPL/pip/status）经 `tools.py` 合并模式与其共存（`vision_run`/`vision_pip`/`vision_status`）。

另有一类形态：**TS 子Agent + 可选原生后端**——主能力（工具集与提示词）由 TS 定义，重计算部分由客卿边车以**同名子代理合并**贡献一个聚合工具，TS 侧调用它并在不可用时**自动回退自己的实现**（回退原因如实回报，返回值经严格校验——字段缺失即回退，不让半份事实进入诊断）。当前两例：`nsight`（Rust + 内嵌 SQLite，聚合事件库）与 `torch`（Rust，聚合 PyTorch trace；实测 2.1× 于 JS 且逐字段一致）。这类后端**不单独列出**（子代理本体在 TS 侧，原生只是加速路径），构建/未构建均可用。

## 四语言基础框架

### Python（keqing/python/driver.py）

语言目录共享驱动：协议 + REPL 引擎（末行表达式求值 repr 回显、session 命名空间保持）+ pip 工具（venv/requirements 落语言目录）。子代理项目可选携带 `tools.py`（导出 `AGENT_NAME` + `TOOLS` + `TOOL_IMPLS`），启动时自动加载与基础工具合并（同名覆盖）——`vision` 即此模式（项目工具 + 基础 run/pip/status 共存于一个子代理）。

**子进程编码坑**：Windows 上 `subprocess.run(..., text=True)` 默认按 GBK 解码，而驱动按 UTF-8 输出 JSON——不显式 `encoding="utf-8"` 会在第一个中文字节上令读线程抛 `UnicodeDecodeError`，表现为 `proc.stdout is None`，报错成「'NoneType' object has no attribute 'splitlines'」，与根因相距极远。`driver_protocol_test.py` 已修正并注明。

### C++（keqing/cpp/framework.hpp）

头文件式框架：手写迷你 JSON（解析/序列化，含 `\uXXXX` 与代理对）、NDJSON 行循环、工具注册表。子代理项目 `#include "../framework.hpp"`，工具写普通函数后 `main()` 前集中注册（`REGISTER_TOOL` 宏对含逗号的 schema/lambda 有预处理器拆参陷阱，集中注册最稳）。构建：Windows 用 `build.bat`（vswhere 定位 MSVC）/ unix 用 `build.sh`（g++/clang++）——两者均带 `stb` 头文件 include 路径（`cpp/stb/`，`imgproc` 的单头库依赖）。

### Rust（keqing/rust/——cargo workspace）

语言目录即一个 cargo workspace：`framework/` 库 crate（gebai-keqing-framework：迷你 JSON + 注册表 + NDJSON 协议循环）与各子代理 bin crate（`nsight/`、`torch/`，`src/main.rs` 只写工具逻辑，依赖 `gebai-keqing-framework = { path = "../framework" }`）。产物统一落 `target/release/{crate}{exe}`——manifest 的 command/build 指向它（`cargo build --release --manifest-path {lang_dir}/Cargo.toml`）；新增子代理 = workspace members 加一行 + 新 crate 目录。依赖按需引：`framework` 本身零第三方依赖（纯标准库），bin crate 可引（`nsight` 以 bundled 特性内嵌 SQLite、`torch` 引 flate2 与 rayon），构建只需 cargo（+ 需编译 C 源码时的 C 编译器），运行期零外部依赖。

### Go（keqing/go/——go module）

语言目录即一个 go module（`gebai/keqing-framework`）：`framework/framework.go` 基础框架包（标准库 encoding/json + bufio，无需手写 JSON——注册/参数助手/panic 兜底/主循环）与各子代理项目（`disk/` 等，`main.go` import 后 `fw.RegisterTool` 注册工具 + `main()` 调 `fw.Run()`）。产物落项目目录 `driver{exe}`（manifest build → `go build -o {agent_dir}/driver{exe} .`——目录构建，多文件与平台构建标签可用）；新增子代理 = 新目录 + agent.json（module 内多 main 包用目录级构建，互不干扰）。

## 任意语言接入示例（Go 心算代理）

```json
{ "name": "gomath", "description": "高性能计算", "protocol": "1",
  "command": ["{agent_dir}/gomath{exe}"],
  "build": { "unix": ["go", "build", "-o", "{agent_dir}/driver{exe}", "{agent_dir}/main.go"] } }
```

实现 4 个 op（init/tools.list/tool.call + 错误响应），按行读写 stdio 即可。放一个目录（agent.json + 驱动 + PROMPT.md）即成一个子代理。

## venv 与依赖位置（迁移说明）

Python venv 与 requirements.txt 原位于 `{GEBAI_HOME}/venv`（dev 模式即仓库根），现归位语言目录 `keqing/python/`（与驱动同居）：

- 解释器解析顺序：`GEBAI_PYTHON_DIR` → 语言目录 venv（`keqing/python/venv`）→ PATH
- 语言目录子代理的 `{agent}_pip`（现为 `vision_pip`）install/freeze 均操作语言目录（freeze 统一写回 `keqing/python/requirements.txt`）
- 构建复制 dist 时过滤 venv/__pycache__/编译产物——部署产物只带源码与 manifest
