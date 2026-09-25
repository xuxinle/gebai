<div align="center">

# 歌白智能体（GEBAI Agent）

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Bun ≥1.2](https://img.shields.io/badge/Bun-%E2%89%A51.2-f472b6)](https://bun.sh)
![Tests](https://img.shields.io/badge/tests-2650%2B-blue)

</div>

> **融合旧世界IT的所有技术，打造新世界智能的躯体。** 极致动态扩展能力的人工智能体：**一个 TS 文件就能定义一个子 Agent**，Agent 还能**修改自己的代码来改进自己**。支持桌面应用、本地浏览器、服务端部署三端合一，一个二进制文件走天下。
>
> **Fuse every technology of the old-world IT into a body for the new-world intelligence.** An AI agent built for extreme dynamic extensibility: **one TS file defines a sub-agent**, and the agent can **improve itself by modifying its own code**. Desktop app, local browser, and server deployment in one codebase — a single binary everywhere.

GEBAI 是围绕一条稳定的「**对话 → 工具调用 → 审批 → 执行**」主循环构建的 Agent 引擎。它不引入 memory / skill 等运行时注入的「不稳定能力」：**智能（模型）负责思考、无状态、可替换；记忆与责任长在智体（Agent 本体）**——需跨轮次/跨会话保留的结论写入文件或会话记录；模型的每次调用都是一张白纸，上下文由智体奉上；每一次工具调用都是智体的行为，经审批执行、留痕可审计。能力的扩展与改进一律沉淀为**可审查、可测试、可回滚的代码变更**。

核心不变式是「**能力外置、权力内守**」：计算、协议、工具实现可以外置到任意语言、任意进程（TS 子 Agent、Python/C++/Rust/Go 客卿边车、js 沙箱、运行时定义工具），但**裁决权力留在 TS 引擎**——工具可见性、审批姿态、安全模式裁决、嵌套与深度规则、会话真相（`chat.json`/动态工具/待办/任务）、进程生命周期全由引擎判定。边缘执行体只有两种合法姿态：**声明**（上报自己具备什么，注册名/审批姿态/可见性由引擎决定）与**委托**（表达想做什么、由引擎按该次调用的会话上下文与审批姿态裁决执行），并遵守两条红线：不得自证授权、不得自取工具。

**English.** GEBAI is an agent engine built around one stable core loop: **conversation → tool calls → approval → execution**. It deliberately avoids runtime-injected "unstable capabilities" (memory, skill files): the **model reasons and is stateless and replaceable, while memory and accountability live in the agent body** — anything that must survive across turns or sessions is written to files or session records, and every tool call is an act of the agent body, approval-gated and auditable. Extensions and improvements always land as **reviewable, testable, revertible code changes**.

Its core invariant is "**capabilities may be externalized, authority stays inside**": computation, protocols and tool implementations can live in any language or process (TS sub-agents, Python/C++/Rust/Go sidecars, the js sandbox, runtime-defined tools), while **adjudication stays in the TS engine** — tool visibility, approval posture, safe-mode rulings, nesting/depth rules, session truth and process lifecycle are all decided there. External executors may only *declare* (report what they offer) or *delegate* (express intent for the engine to adjudicate); they may never self-authorize nor take tools on their own.

## 能力掠影 | Capability Gallery

> 以下截图全部取自 GEBAI 的真实运行会话（本地模式 Web UI 实拍）——画面上的每一张卡片都是引擎真实产出的工具调用、审批与产物留痕。
> All screenshots are captured from real GEBAI sessions (local-mode Web UI): every card shown is a genuine tool call, approval or artifact record produced by the engine.

**① 计划审批：先方案后执行** — 多步骤任务先提交计划（标题 + 步骤清单），批准后逐条执行并回写状态，全程留痕可审计；工具级审批、选项卡片、环境变量填值在同一链路上。

<img src="docs/screenshots/plan-approval.png" width="820" alt="计划审批卡：计划已批准 + 执行计划清单 | Plan approved with a step checklist">

*① Plan approval — multi-step work starts with a reviewable plan, then runs step by step under audit.*

**② 子 Agent 扩展：一个调用派生四路并行子会话** — `subsession_run` 同时启动 `facts-agents + facts-tools + facts-numbers + facts-design`（隔离上下文各自干活），报告自动合入父会话，后台任务统一由 `bg_task` 管理。

<img src="docs/screenshots/subsessions-parallel.png" width="820" alt="subsession_run 一次派生四个并行子会话 | One subsession_run call forks four parallel children">

*② Parallel sub-sessions — one call forks four isolated children; their reports merge back and `bg_task` tracks them.*

**③ 单文件子 Agent：装载即用 `{agent}_` 工具** — `agent_load` 把子 Agent 的工具并入当前工具集、系统提示词写入会话；随后就地调用 `vision_ocr` 读图取字并返回像素坐标（本地 onnxruntime 推理，离线、不耗模型配额）。

<img src="docs/screenshots/subagent-load.png" width="820" alt="agent_load 装载 vision 后直接调用 vision_ocr 读图取字并返回像素坐标 | After agent_load, vision_ocr reads text and returns pixel coordinates">

*③ Load and use — `agent_load` merges `{agent}_` tools into the session; here `vision_ocr` reads text with pixel coordinates, locally.*

**④ 代码级自我优化：改自己的代码，然后用测试证明** — `self_optimize` 修改歌白自身源码后跑 `test` / `typecheck` / `lint` 三件套（测试是唯一准入凭证，失败自动回滚），设计变更回写 `DESIGN.md` 并记入 journal。

<img src="docs/screenshots/self-optimize.png" width="820" alt="self_optimize 改自己的代码后跑 test/typecheck/lint 三件套 | self_optimize edits its own code, then runs test/typecheck/lint">

*④ Self-optimization — the agent edits its own source, then proves it with tests/typecheck/lint; failures auto-roll back.*

**⑤ 从代码到成片：富内容与产品视频** — `reel` 渲染作业回报档位、进度与耗时，静帧回读做视觉自检，成片以文件卡交付；同一条富内容链路也产出图表、沙箱 HTML、PDF 与 Office 文档。

<img src="docs/screenshots/reel-video.png" width="820" alt="reel 渲染作业状态与静帧回读自检 | reel render job status and still-frame read-back">

*⑤ Render to video — the `reel` job reports profile, progress and timing, frames are read back for QA, and the result ships as a file card.*

**⑥ 文件工作台** — `/files`：Monaco 编辑器 + IDEA 风格 Git 工具窗（变更/日志/分支/标签/暂存/远程）+ 目录树、终端、任意两端差异对比与三窗格冲突合并；编辑器里打开的正是「一个 TS 文件定义一个子 Agent」的源码。

<img src="docs/screenshots/file-workbench.png" width="820" alt="文件工作台：Monaco 编辑器 + IDEA 风格 Git 工具窗 | File workbench: Monaco editor plus an IDEA-style Git tool window">

*⑥ File workbench — Monaco, an IDEA-style Git tool window, diffing and three-way merge; the file on screen is a one-file sub-agent definition.*

---

## 快速开始 | Quick Start

### 环境要求 | Prerequisites

- [Bun](https://bun.sh) ≥ 1.2（仓库 `packageManager: bun@1.2.0`；桌面端启动器另需 Rust 工具链，仅构建启动器时需要）
- **English.** [Bun](https://bun.sh) ≥ 1.2 — that's the only requirement (a Rust toolchain is needed only to build the desktop launcher).

可选能力按需补充（不装则相关工具不可用，引擎与其余能力不受影响）：`py` 工具需要 Python；`playwright`/`reverse_site` 的浏览器桥需要宿主机 `node` 与浏览器；客卿子Agent（vision/imgproc/disk，以及 nsight/torch 的原生加速后端）需要对应语言的边车构建（Python/C++/Go/Rust）。

### 开发模式 | Development

```bash
# 1. 安装依赖
#    1. Install dependencies
bun install

# 1.5 （可选）下载本地识别资源（PP-OCR 模型、GPU 推理依赖）——本地视觉识别用，不装则相关工具不可用
#     1.5 (optional) Fetch local-vision resources (PP-OCR models, GPU runtime deps)
bun run resources:download

# 2. 配置模型（复制 .env.example 为 .env 并填入 LLM 配置）
#    2. Configure a model (copy .env.example to .env and fill in LLM settings)
cp .env.example .env

# 3. 启动（脚本调试模式，GEBAI_HOME 为项目根目录）
#    3. Start (script debug mode; GEBAI_HOME is the repo root)
bun run dev

# 3.1 带前端热刷新（Web 源码改动自动重建并广播页面刷新）
#     3.1 With frontend hot rebuild (web source changes rebuild and auto-reload the page)
bun run dev --reload
```

然后访问 `http://127.0.0.1:3000` 即可开始对话。

**English.** Then open `http://127.0.0.1:3000` and start chatting. `bun run dev --reload` watches and rebuilds the Web UI on change (`GEBAI_DEV_RELOAD=1` is the env equivalent).

- `bun run dev:web` 单独起前端（Vite dev server）；`bun run dev:desktop` 起桌面宿主
- **English.** `bun run dev:web` runs the Vite dev server alone; `bun run dev:desktop` runs the desktop host.

### 构建与分发 | Build & Distribution

```bash
bun run build                 # 全量构建（Web UI + 子 Agent 打包 + 桌面端）；底层为 turbo run build
                              # Full build (Web UI + sub-agent bundling + desktop), via turbo run build
bun run build:code            # 裁剪构建示例：code 场景精简单文件二进制
                              # Trimmed build example: lean single-file binary for the code scenario
GEBAI_BUILD_SUBAGENTS=a,b bun run --cwd packages/server build   # 仅打包指定子 Agent（构建期裁剪）
                                                                # Bundle only the listed sub-agents
```

- 桌面端 / Desktop: `packages/desktop/dist/gebai-desktop.exe`（tao/wry 原生 WebView 启动器，`include_bytes!` 内嵌服务端二进制，物化后拉起，关窗回收）
- 桌面端 / Desktop: `packages/desktop/dist/gebai-desktop.exe`（tao/wry 原生 WebView 启动器，`include_bytes!` 内嵌服务端二进制，物化后拉起，关窗回收）
- 纯服务端 / Server-only: `packages/desktop/dist/gebai.exe`（Bun `--compile` 单文件，零运行时依赖——Bun 已内嵌；`--server` 切服务模式）
- 容器镜像 / Container image: `docker/build.sh`（Windows 用 `pwsh -File docker/build.ps1`）——Ubuntu 24.04 基础镜像，构建阶段完成上述构建链后 `--compile` 出 Linux 单文件，运行阶段不含 node_modules 与 bun；用法、构建参数、隔离前提与能力边界见 `docker/README.md`

### 服务模式（多用户）| Server Mode (Multi-user)

```bash
# 生成 admin 密码哈希并启动
# Generate the admin password hash, then start
bun run --cwd packages/server hash-password -- '你的密码'
GEBAI_MODE=server GEBAI_ADMIN_PASSWORD_HASH=salt:hash ./gebai.exe
```

**模型配置可以完全由前端完成**：服务端不配任何模型变量，用户在浏览器「设置 → 环境变量」面板填入密钥（**仅存 localStorage，服务端零留存**），随消息临时注入；用户环境变量在服务端不落盘、重启即空，用户之间互不可见。

**English.** **Model configuration can be done entirely in the frontend**: the server needs no model variables at all — users enter keys in the browser's Settings → Environment Variables panel (stored only in localStorage; the server retains nothing), injected per-message on the fly.

### 测试与检查 | Tests & Checks

```bash
bun run test                                  # 全量测试（turbo 编排；server 包自动分片并行）
                                              # Full suite (turbo-orchestrated; the server package auto-shards)
bun run --cwd packages/server test:serial     # server 串行测试（调试单进程行为时用）
                                              # Serial server tests (for debugging single-process behavior)
bun run typecheck                             # 类型检查（各包 + custom/ + scripts/）
bun run lint                                  # Lint
bun run --cwd packages/server test:coverage   # 覆盖率报告（核心引擎行覆盖率目标 ≥90%）
```

仓库现有 **172 个测试文件、约 2650 个用例**（`packages/server` 77 个文件、`packages/agents` 45、`packages/web` 47、`packages/sdk` 2、`packages/desktop` 1）。`packages/server` 的 `test` 走分片并行（按文件字节数降序装箱，默认 `min(8, max(2, CPU 核数))` 片），分片失败会**单进程串行复验**以区分并行抖动与真失败。覆盖率门槛（核心引擎 ≥90%、工具函数 ≥80%、整体 ≥70%）是仓库的**人工约定目标**，无 CI 门禁。

**English.** The repo currently ships **172 test files and ~2650 cases**. `packages/server`'s `test` script shards and runs in parallel (LPT bin-packing, default `min(8, max(2, cores))` shards) and re-verifies a failed shard serially to tell flakiness from a real failure. The coverage targets (core engine ≥90%, tool functions ≥80%, repo ≥70%) are hand-agreed goals, not enforced by CI.

---

## 为什么值得关注 | Why It's Worth Your Attention

### 🧩 极致的子 Agent 扩展制：单文件定义、零注册

在 `packages/agents/src/agents/` 下放一个目录（`{name}.ts` 定义 + 可选 `.md` 提示词）即完成一个子 Agent——构建期自动扫描收集，**没有注册表、没有配置、没有代码登记**：

```ts
export const name = "my_agent"
export const description = "何时该装载本子 Agent 的一句话描述"
export const systemPrompt = "你是……（完整系统提示词）"
export const tools: ToolSet = { read, write, sh, ... }   // 复用全局工具或新建独有工具
export const requiresApproval = { write: true }
export const dependencies = ["code"]                     // 可选：依赖自动连带装载
export const writeGuard = (env, absPaths) => null        // 可选：写范围守卫
```

关键设计是「**装载 vs 子会话运行**」两种语义的精确区分（截图见「能力掠影」② 子会话并行 / ③ 装载即用）：

| 能力 | 装载（`agent_load`，模块语义） | 子会话运行（`subsession_run`，会话语义） |
|------|------|------|
| 类比 | `import` 子模块 | 父子会话（进程）：fork 继承父上下文 / spawn 隔离新上下文 |
| 上下文 | 不创建新上下文：工具以 `{agent}_` 全名并入当前工具集，**完整系统提示词作为 system 消息写入会话**（持久化，历史重建前置到最前） | 默认隔离新上下文（`inherit_context:false`）；`true` 则 fork 父消息历史/提示词/工具面 |
| 交付 | 幂等、会话级持久 | 继承形态报告**自动合入父上下文**（`merge=full`/`summary`）；隔离形态结果经工具结果返回，异步经 `bg_task`（`s`+8 位 hex）管理 |
| 适用 | 默认方式：装完直接用其工具 | 需要干净上下文、防止上下文膨胀；或同一任务的并行多路推进（异步子会话可主动合入阶段性成果） |

工具以 `{agent}_{tool}`（**单下划线**）命名空间透明路由：注册期做碰撞检查（全局工具名不得以任何 `{agent}_` 开头、子 Agent 名不得互为前缀），路由按「全局精确匹配 → 子 Agent 最长前缀匹配」两步解析，并对弱模型做容错（分隔符/驼峰偏差归一）。

其余扩展机制（见 DESIGN.md「子Agent（扩展机制）」章）：

- **依赖自动装载**：`dependencies` 级联展开（依赖在前、传递递归、共享去重、循环即抛错），装载 / `subsession_run` 预加载 / 子会话内装载 / 构建期清单四条路径语义一致
- **装载可见性按会话建模**：`{agent}_` 工具只对装载过该子 Agent 的会话可见；**路由自愈**——模型直接调用未装载子 Agent 的工具时，按 `agent_load` 同路径自动装载后执行
- **热加载（脚本调试模式）**：`src/agents/` 目录签名（递归 `路径:mtime`）变化即重扫，新增/修改/删除在下一次装载或新任务前生效；已装载会话沿用旧定义（防运行中行为漂移）；二进制形态无源码目录、注册表不可变
- **写范围守卫**：`writeGuard(env, absPaths)` 由引擎注入，文件写类工具写入前以解析后的绝对路径调用，返回非空即拒绝（不抛错、不落盘）——`self_optimize` 的「核心引擎源码默认只读」即由此代码级强制
- **客卿（多语言子 Agent）**：任意语言的子 Agent 放目录即被发现、启动边车、握手拉取工具清单后注册为标准子 Agent（`agent_list`/`agent_load`/`subsession_run` 完全同构）；TS 与客卿**同名合并**（工具并集、提示词拼接，`vision` 即跨语言合并样例；`nsight`/`torch` 的原生后端同机制）

**English.** Drop a directory under `packages/agents/src/agents/` (`{name}.ts` + optional `.md` prompt) and the build collects it — **no registry, no config, no code registration**. The two extension semantics are deliberately distinct: **load** (`agent_load`, module semantics — tools join the current toolset and the full system prompt is persisted into the session) versus **run in a sub-session** (`subsession_run`, session semantics — fork the parent context or spawn an isolated one, with auto-merge for forked children and `bg_task` (`s…`) management for async ones). Tools route transparently through the single-underscore `{agent}_{tool}` namespace with build-time collision checks; automatic dependency cascade loading, per-session load visibility with routing self-heal, dev-mode hot reload, code-level write guards and multi-language sidecar sub-agents round out the mechanism.

### 🔁 代码级自我优化：改代码，而不是改权重

Agent 通过**修改自身代码**持续改进自己——子 Agent 定义、工具实现、系统提示词、默认配置全是可 diff、可测试、可回滚的代码。闭环：发现改进点（读用户反馈、失败案例、日志复盘；不便立即优化的先 `backlog add` 暂存）→ 方案 → 审批 → 改代码 → **验证（测试是唯一准入凭证）** → 设计变更回写 `DESIGN.md` → 用户验证（UI 类 `page_capture` 抓真实渲染 + 视觉分析，服务端类临时端口验证服务）→ `journal` 沉淀 → 构建/重启生效。

- **写范围守卫代码级强制**：默认只读模式只允许修改子 Agent 包（`packages/agents/src/`）与仓库级文档/配置，核心引擎源码默认只读（`GEBAI_SELF_MODIFY=true` 可放开）
- **失败即回滚**：`run_tests` 先跑相关测试文件，确认后 `test`/`typecheck`/`lint` 三件套；失败用 `rollback` 恢复修改并清理本次新建文件
- **为什么不用 memory / skill**：记忆注入与技能文件依赖运行时状态与外部文件——内容漂移、不可复现、难以审计、无法 diff/测试/回滚，且与「多用户隔离」「单一真相源」相悖。**代码即一切**：子 Agent 文件、工具实现、系统提示词本身就是能力载体。

**English.** The agent keeps improving itself **by modifying its own code** — sub-agent definitions, tool implementations, system prompts and defaults are all diffable, testable, revertible code. Loop: feedback/backlog → plan → approval → code change → **tests as the only admission ticket** (`run_tests`; failures auto-`rollback`) → design changes written back to `DESIGN.md` → user verification (real rendered-page capture + vision analysis for UI, a temp-port preview server for server-side changes) → journal. Write scope is enforced **at the code level** (sub-agent package + repo-level docs by default; core engine source read-only unless `GEBAI_SELF_MODIFY=true`). Memory/skill-style runtime injection is rejected on purpose: it drifts, cannot be diffed, tested or rolled back, and conflicts with multi-user isolation.

### 💻 一套代码，三种形态，一个二进制

`bun build --compile` 产出单个可执行文件，**零运行时依赖**（Bun 已内嵌）：

- **桌面应用**：`gebai-desktop.exe`——tao/wry 原生 WebView 启动器（Windows WebView2 / macOS WKWebView / Linux WebKitGTK），内嵌服务端二进制，固定端口 47896，关窗回收侧车
- **本地浏览器**：`gebai.exe` 开箱即用，自动打开系统默认浏览器（本地模式免登录，以 admin 身份工作）
- **服务端部署**：`--server` 服务模式，同一端口承载静态 Web UI、WebSocket（`/ws`）与 REST（`/api/*`）

Web UI、全部子 Agent、tree-sitter 语法、图表引擎（Mermaid/PlantUML/D2/ECharts）、playwright 驱动、内置 ripgrep 全部内嵌；`GEBAI_BUILD_SUBAGENTS=a,b` 按需裁剪二进制体积。

**English.** `bun build --compile` produces a single executable with **zero runtime dependencies** (Bun is embedded): a native-WebView desktop app (`gebai-desktop.exe`, tao/wry launcher with the server embedded, fixed port 47896), a local-browser binary (`gebai.exe`, no login in local mode), and a multi-user server (`--server`) serving the static UI, WebSocket and REST on one port. The Web UI, all sub-agents, tree-sitter grammars, diagram engines, the playwright driver and an embedded ripgrep are all bundled; `GEBAI_BUILD_SUBAGENTS=a,b` trims the binary on demand.

### 🛡️ 多用户隔离与安全

- **用户 → 会话两级隔离**：数据与执行环境按用户隔离（`{GEBAI_HOME}/users/{user}/`），任何会话操作都校验归属；服务模式账号密码登录（scrypt 加盐哈希），admin 唯一入口是 `GEBAI_ADMIN_PASSWORD_HASH`
- **路径沙箱**：服务端部署下文件类工具统一以会话 `tmp/` 为基准，拒绝 `../`、绝对路径、符号链接逃逸；服务模式一律沙箱（admin 不豁免），`GEBAI_SANDBOX=off` 与服务模式互斥
- **执行隔离与环境脱敏**：脚本子进程 cwd 限定会话目录，沙箱/安全模式下**剔除敏感环境变量**（`*_KEY`/`*_TOKEN`/`*_SECRET`/`*_CREDENTIAL`/`DATABASE_URL` 等）；桌面类工具在服务端沙箱下整体拒绝
- **SSRF 防护**：`fetch_url` 与 Webhook 投递对沙箱约束用户仅允许公网，拒绝 RFC1918/ULA 与各类绕过写法（IPv4-mapped IPv6、整数/十六进制/八进制 IP、尾点 FQDN），并做重定向逐跳校验与 DNS 复查
- **`GEBAI_SAFE_MODE`：风险能力降级而非一刀切禁用**——`sh` 只读白名单 + 重定向限范围（fail-closed）、`py` 审计钩子（仅保留文件读取）、`js` 词元静态扫描 + 运行时 shim、写类工具限定安全写范围、任务调度（`task_*`）维持硬阻断；安全模式变量**仅在启动时从环境加载**，无法被会话/任务级 env 或模型改写
- **服务模式防线**：管理端点校验管理员角色；无交互通道（REST 单次请求）下本地模式需审批工具自动通过、**服务模式直接拒绝**（防普通用户经 REST 免审批执行敏感工具）；登录失败锁定 + 令牌桶限流，令牌 HMAC 签名 7 天 TTL 并持久化

**English.**

- **Two-level isolation** (user → session) for data and execution; every session operation verifies ownership; scrypt-salted passwords with a single admin entry point (`GEBAI_ADMIN_PASSWORD_HASH`)
- **Path sandbox**: file tools resolve relative paths under the session `tmp/`, rejecting `../`, absolute paths and symlink escapes; server mode always sandboxes (even for admin)
- **Execution isolation & env scrubbing**: script subprocesses are confined to the session directory and run with sensitive variables stripped; desktop-class tools are refused under server-side sandboxing
- **SSRF protection** for `fetch_url` and webhook delivery (public addresses only, per-hop redirect validation, DNS re-checks, bypass forms rejected)
- **`GEBAI_SAFE_MODE` degrades rather than disables**: read-only whitelist for `sh`, audited read-only runtime for `py`, static scan + runtime shim for `js`, narrowed write scope for write tools, hard block on scheduling — and it can only be set from the environment at startup
- **Server-mode defenses**: admin-only management endpoints; in no-interaction channels (REST) approval-gated tools auto-pass in local mode but are **refused in server mode**; login lockout and token-bucket rate limiting; HMAC-signed tokens with a 7-day TTL

### 🎨 多模态与富内容

- **三类模型接口原生支持**（不依赖第三方 AI SDK，自行解析 SSE 流）：OpenAI 兼容 `chat/completions`、OpenAI Responses、Anthropic `messages`；`capabilities()` 统一声明 `streaming`/`toolCalling`/`multimodal`/`maxContextTokens`/`maxOutputTokens`
- **附件与图片直读**：消息附件先落会话 `tmp/`（`AttachmentRef.path` 为会话根相对逻辑路径），图片在主模型声明多模态且不超限时压缩后内联为统一 `image` 块；`read` 读图片同样内联进工具结果（轻量引用随消息落盘，历史重建时重读内联）；接口实际拒绝图片块时一次性降级为文本说明后重试
- **原图保存、发送时压缩**：粘贴/上传/URL 引用一律先按原图存盘，仅在发送给模型前压缩（长边 >1280px 或 >2MB 等比缩放，GIF 不压缩，压缩不回写文件并附尺寸说明）
- **富内容块**（随消息持久化，历史会话同样可查看）：`code` 文件内容卡、`image` 内嵌图（点击全屏）、`file` 统一文件卡（图片/音视频/PDF/沙箱 HTML/二进制占位，进入视口才加载）、`diagram` 交互式图表、`diff` 并排对比（仅历史回放）、`html` 沙箱页面（iframe 域隔离，脚本可执行但无法触达宿主页面）
- **四种图表语言交互式创作**：Mermaid / PlantUML / D2 / ECharts，默认**前端本地渲染**（SVG，零服务端开销），需要图片时 `render=backend` 服务端渲染 PNG；渲染成功工具才返回成功，渲染报错把错误文本回传模型修正
- **语音合成与朗读（离线）**：`tts` 子Agent把文本合成音频文件（音色/语速/音调可调，`play=true` 还在本机扬声器播报）；Web 端每条助手回复带**朗读按钮**——`POST /api/v1/tts` 取回 WAV 即听，不必先落盘；引擎为本机系统语音（Windows WinRT OneCore 优先、SAPI5 回退），**不联网、不耗额度、无需安装**，非 Windows 平台如实报错而不回落在线服务
- **10 套 UI 主题**：`acrylic`（默认，黑白可切）/ `matrix` 矩阵 / `tokyo-night` 东京夜 / `cyberpunk` 赛博 / `synthwave` 浪潮 / `aether` 以太 / `aurora` 极光 / `ink` 水墨 / `cny` 人民币 / `qinhan` 秦汉，运行时热切换（`GEBAI_UI_STYLE` 可指定服务端默认）
- **文件工作台**（独立页面 `/files`）：目录树 + Monaco 编辑器 + IDEA 风格 Git 工具窗（变更/日志/分支/标签/暂存/远程）+ 任意两端差异对比 + 三窗格冲突合并；所有 fs/git 接口只接受 `(root, 相对路径)`，根分为 `sess:`/`proj:`/`bind:`/`user:`/`abs:` 并做三层路径防护；面向用户本人直操（不走工具审批但落审计），让 Agent 去改仍走审批链路；`GEBAI_FS_ENABLED=false` 时页面与端点整体 404（截图见「能力掠影 · 文件工作台」）

**English.** Native support for all three LLM API families (SSE parsing implemented from scratch, no third-party AI SDK), attachment/`read` image inlining with send-time compression and automatic downgrade when an endpoint rejects image blocks; persistent rich content blocks (code / image / file / diagram / diff / sandboxed HTML) viewable in history; interactive authoring for Mermaid, PlantUML, D2 and ECharts (frontend rendering by default, backend PNG on demand); 10 hot-swappable UI themes; and a standalone file workbench at `/files` with Monaco editing, an IDEA-style Git pane, arbitrary two-revision diffing and three-way merge — rooted abstractions with three-layer path defenses, user-operated (no tool approval) but audited.

### ⚙️ 面向工程的能力

- **审批流**：命令行式人机协作——工具级审批（含参数展示）+ 键盘 Y/N、会话级 `/approval-skip`（写会话内存态 env，运行中开启即时生效）、请求级 `autoApprove`；`ask` 是向用户询问的统一入口，按参数三选一：**选项询问**（`prompt`+`options`）/ **环境变量填值**（`name`，前端弹窗，仅存 localStorage）/ **计划审批**（`title`+`steps`）
- **脚本工具的三种用法**：`sh`（shell 命令，Windows 经 PowerShell、POSIX 经 bash，长命令可 `async:true` 转后台任务，用 `bg_task` 查/等/停）、`py`（本地模式带工具桥：工具名即函数、`tools.call`、`ctx`/`input` 注入）、`js`（Bun 运行时，脚本内可直接 `await read(...)` 调用工具，`defineTool` 可把能力固化为**会话级动态工具**并持久化恢复）
- **待办跟踪**：`todo` 清单拆解 → 执行 → 失败恢复续做；用户级待办（`users/{user}/todos.json`）与任务清单相互独立，可随时手动执行（入队按序跑一次），开 ⚡ 闲时自动执行的条目绑定一个闲时任务——队列空闲且无运行中会话时自动按序执行，成功自动勾选、失败 3 次停用
- **统一任务管理**：用户级任务（`users/{user}/tasks.json`，会话删除后仍按期执行）——**定时 / 普通 / 闲时**三类共用一条队列（定时到期插队首、普通入队按序执行、闲时在队列空闲时串行执行），每用户并发额度 5（`GEBAI_TASK_MAX_CONCURRENT`）；脚本运行或提示词运行 Agent，支持 5 段 cron / `@every` / `@daily` / `@at`、IANA 时区、错过补跑、超时、连续失败自动停用、飞书群与 Webhook 通知、任务资源目录（脚本/文档）；由 `GEBAI_TASKS_ENABLED` 统一开关（默认 true，显式 false 时子 Agent 与调度器整体不可见）
- **飞书机器人**：`GEBAI_FEISHU_BOT_ENABLED=true` 启用，**长连接模式**（服务端主动出站，无需公网回调地址），协议为自研极简 protobuf 帧实现；文本/图片双向、任务完成回卡片、`show` 图表由桥接后端渲染 PNG 上传、审批与选择用交互卡片、`/help` `/new` `/sessions` `/cancel` `/approve` `/reject` 等命令；飞书身份按 `open_id` 映射用户，单聊/群聊各关联一个独立会话
- **业务系统集成**：官方 TS SDK（`@gebai/sdk`，WS/REST 双通道）、`/api/docs` OpenAPI 文档（端点表由路由注册自动生成）、Webhook（事件推送，HMAC-SHA256 签名 + 失败指数退避重试 3 次）、外部身份兑换（`POST /api/v1/auth/exchange`，HMAC 或 HTTP 回调验证器可插拔）、iframe 嵌入与同源登录态复用、URL 携带提示词直接起任务（`gb_prompt`，自动建会话运行并重定向到会话地址）、前端独立配置文件（`gebai.config.js`，环境变量与本地存储的扩展点）
- **数据生命周期**：会话 90 天闲置归档到 `trash/`、`trash/` 7 天物理删除、反馈 180 天清理（`GEBAI_GC_DISABLED` 可关）

**English.** CLI-style human-in-the-loop approvals (per-tool with argument display, Y/N shortcuts, session-level `/approval-skip`, request-level `autoApprove`; `ask` merges choice prompts, env-var filling and plan approval); three ways to run scripts (`sh` with background tasks via `bg_task`, `py` with a local-mode tool bridge, `js` with first-class `await read(...)` calls and `defineTool`-registered session-scoped dynamic tools); todo tracking with idle-time execution; user-level unattended cron jobs (5-field cron / `@every` / `@daily` / `@at`, time zones, misfire policies, Feishu and webhook notifications); a Feishu bot over a long-lived outbound connection (no public callback URL needed) with interactive approval/render cards; business integration through the official TS SDK, auto-generated OpenAPI docs, HMAC-signed webhooks with retries, external-identity exchange and iframe embedding; plus automatic data lifecycle GC.

---

## 架构一览 | Architecture at a Glance

```
┌──────────────┐        ┌──────────────┐        ┌────────────┐
│   API 层     │        │  AgentEngine │        │ ToolRegistry│
│ Hono 路由    │───────►│  核心主循环   │◄──────►│  命名空间解析 │
│ WS/REST/     │        │ 工具/审批/压缩 │        └────────────┘
│ Webhook/飞书 │        └──────┬───────┘
└──────────────┘   ┌──────────┼───────────┐
            ┌──────▼───┐ ┌────▼────┐ ┌────▼──────┐
            │SessionStore│  │EnvManager│  │ EventBus  │
            └────────────┘  └─────────┘  └───────────┘
```

| 包 / 目录 | 职责 |
|-----------|------|
| `@gebai/server`（`packages/server`） | 服务端核心：Hono 单端口服务、Agent 引擎、会话管理、子 Agent 装载与子会话运行、REST/WS/Webhook |
| `@gebai/agents`（`packages/agents`） | TS 子 Agent 包：`src/agents/`（定义）+ `src/core/`（共享基建，零 `import @gebai/server`） |
| `@gebai/sdk`（`packages/sdk`） | 客户端 SDK：WS/REST 连接管理、类型定义、API 契约（双入口 `.` / `./node`） |
| `@gebai/web`（`packages/web`） | Web UI：Vite 构建，打包进二进制作为内置前端（多入口 `/` 与 `/files`） |
| `@gebai/desktop`（`packages/desktop`） | 桌面宿主：`dist/gebai.exe`（Bun `--compile` 单文件）+ `launcher/`（tao/wry 原生 WebView） |
| `custom/` | 二开域（与 `packages/` 平级）：放置自定义子 Agent 与扩展 |
| `keqing/{python,cpp,rust,go}/` | 客卿子 Agent（多语言边车）与各语言基础框架 |

Monorepo（Bun workspaces + Turborepo）；核心模块全部接口化 + 依赖注入，可独立实例化测试。

**English.** Monorepo (Bun workspaces + Turborepo) with five packages — `@gebai/server` (engine + single-port service), `@gebai/agents` (TS sub-agent definitions + shared infra, zero imports from the server package), `@gebai/sdk`, `@gebai/web` (multi-entry UI bundled into the binary) and `@gebai/desktop` (compiled server + tao/wry launcher) — plus a `custom/` extension area and multi-language sidecar sub-agents under `keqing/`. All core modules are interface-first with dependency injection, so each can be instantiated and tested in isolation.

### 主循环与关键机制 | Core Loop & Key Mechanisms

```
用户消息 → 组装上下文（历史 + 系统提示词 + 临时文件提示）
        → LLM 流式生成（文本/推理增量实时推送）
        → 解析工具调用（必填参数校验）
        → 门控：重复检测 / 参数抢救 / 路由解析（含自动装载）/ 审批姿态（按调用顺序串行，
                缺参/未知工具/禁用/安全拦截等说明性结果直接回传并推送 call+result 事件对）
        → 同批并行执行（每个调用独立走 审批等待 → 执行 → 落盘，并发上限 8，超出排队）
        → 结果注入上下文 → 循环回 LLM（轮次不设上限）
```

- **全程可观测**：文本增量、工具调用/结果/心跳、审批请求、子会话过程、任务开始/结束、上下文占用都以 `event.*` 实时推送（带 `seq`，断线重连按序重放）
- **中断与取消**：停止按钮即时打断进行中的工具执行（脚本子进程按进程树终止），被中断的调用其**工具结果统一带 `[interrupted by user]` 标记**——工具自身交回的中断返回（含中断前已产生的输出）在宽限期（300ms）内并入结果，未交回的以统一标记即时收口；未执行/未完成的调用补写带标记的占位（保持 `assistant`/`tool` 配对完整）；取消统一解开全部挂起等待（审批/选择/画图/捕获），不会出现「中断后要发两次才能继续」
- **失控防护**：背靠背重复调用相同工具（最近 8 次签名窗口尾部连续第 3 次）中断该次并注入引导提示，连续超限终止循环；脚本工具自身超时（默认 300 秒、上限 540）先杀进程，引擎层 9 分钟兜底；REST 单次请求与飞书通道为无交互通道，按审批姿态分级处理
- **上下文保护**：以模型返回的**真实 `input_tokens`** 为口径（不以窗口百分比触发），窗口剩余不足以支撑一次回复时自动压缩——超长输入落盘、工具大输出截断落盘（`tmp/truncated/`）、旧消息摘要（原消息完全抛弃、摘要置于最前）、溢出硬护栏（最新一条用户消息永不裁剪）；滑动窗口保留最近 12 条；接口以上下文长度错误拒绝时压缩后重试至多 3 次
- **同批并行与严格串行的边界**：单次响应内的多个工具调用并行（免审批项不等同批审批项、慢工具不阻塞快工具，结果按完成先后落盘、配对按 `toolCallId`）；需严格串行的操作由模型用 `js` 脚本按序编排或拆分多轮

**English.** The loop is a single observable state machine: build context → stream the model → parse tool calls → gate them serially (repeat detection, argument salvage, route resolution with auto-load, approval posture) → execute the batch in parallel (each call independently going approval → execute → persist, concurrency capped at 8) → feed results back and loop. Cancellation interrupts in-flight tools immediately and marks their tool results with `[interrupted by user]` (a tool's own interrupted return — including partial output — is folded into the result within a 300 ms grace window; anything unreturned is closed out with the unified marker), unresolved calls get marked placeholders so `assistant`/`tool` pairing stays intact, and every pending wait is released. Context protection keys off the model's **real `input_tokens`** (never a window percentage) with a four-stage compaction ladder, and a repeat-detection guard plus a 9-minute engine fallback keep runaway loops bounded.

---

## 全局工具 | Global Tools

共 **20 个**全局工具（子 Agent 的独有工具以 `{agent}_{tool}` 命名空间另计）。文件/路径类工具统一带 `project` 参数（项目根路由：预置项目名 / 项目根路径 / `tmp` 会话工作区）。

| 工具 | 能力 | 审批 |
|------|------|------|
| `read` / `write` / `ls` | 读文件（带行号，大文件分段）/ 写文件（`append` 分段续写）/ 列目录 | 免审 |
| `grep` / `glob` | 正则递归内容搜索（内置 ripgrep 优先，不可用自动回退内置引擎）/ 文件名模式查找 | 免审 |
| `file` | 文件管理多动作：`copy`/`rename`/`move`/`mkdir`/`delete`/`info`（按内容探测类型） | `delete` 动态需审批 |
| `edit` / `patch` | 定点替换（`old_string`/正则，多处原子落盘）/ unified diff 补丁（可多文件、可 `dry_run`） | 免审 |
| `sh` | 执行 shell 命令（Windows 经 PowerShell、POSIX 经 bash），`async:true` 转后台任务，`timeout` 调整个别超时 | 默认需审批（只读/测试类白名单可免审） |
| `py` | 执行 Python 代码（本地模式带工具桥：工具名即函数、`tools.call`、`ctx`/`input` 注入） | 恒需审批 |
| `js` | 执行 JS/TS 脚本（Bun 运行时）：脚本内工具即函数（`await read(...)`）、`defineTool` 注册会话级动态工具、`bg_task` 调度后台任务 | 默认需审批（词元扫描通过可免审） |
| `fetch_url` | 抓取 URL 内容（网页/API/文档；沙箱模式禁公网外地址并逐跳校验） | 免审 |
| `show` | 内容展示统一入口：Markdown 文档 / 源码高亮 / 纯文本 / 四语言图表 / 沙箱 HTML 页面（`content` 直给或 `path` 直显，原 `draw`/`render_html`/`show_file` 合并） | 免审 |
| `ask` | 向用户询问并阻塞等待的统一入口：选项询问 / 环境变量填值 / 计划审批（原 `ask_user`/`ask_env`/`plan` 合并） | 免审 |
| `todo` | 待办清单（新增/更新/删除/查询，entries 批量操作） | 免审 |
| `tool_schemas` | 批量查询工具的输入参数 schema 与结构化输出（data）schema | 免审 |
| `agent_load` | 装载子 Agent（工具并入当前工具集 + 完整提示词注入会话） | 免审 |
| `subsession_run` | 子会话运行（隔离 / 继承上下文，同步 / 异步，多路并行，模型路由） | 免审 |
| `bg_task` | 统一管理后台异步任务（命令任务 `t…` 与子会话运行 `s…`：status/wait/stop/list） | 免审 |
| `restart_server` | 重启服务进程（仅本地模式；可带续跑提示词；dev-reload 能力随重启继承） | 恒需审批 |

`agent_list` 与 `subsession_merge` 不是全局工具——前者由引擎在「可委派」场景注入，后者仅异步子会话运行上下文可见。

**English.** Twenty global tools ship with the engine: file read/write/ls, ripgrep-backed grep and glob, a multi-action `file` manager, `edit`/`patch`, the three script tools (`sh`/`py`/`js`), `fetch_url`, `show`, `ask`, `todo`, `tool_schemas`, `agent_load`, `subsession_run`, `bg_task` and `restart_server`. Approval posture: `py` and `restart_server` always require approval, `sh`/`js`/`file delete` decide per call, the rest are approval-free; file/path tools accept a `project` parameter for project-root routing.

## 内置子 Agent | Built-in Sub-Agents

`packages/agents/src/agents/` 下 15 个 TS 子 Agent，另在 `keqing/` 下 5 个客卿目录（其中 `vision` 与 TS 侧同名合并、`nsight`/`torch` 以同名合并贡献原生加速后端，`imgproc`/`disk` 仅客卿侧，运行时可见子 Agent 共 17 个）。全部**按需装载**（`preload=false`），`GEBAI_PRELOAD_SUB_AGENTS` 可指定启动预加载名单。

| 子 Agent | 能力 | 独有工具 | 外部依赖 / 凭证 |
|-----------|------|----------|------------------|
| `code` | 代码编写与源码分析（探索→方案→修改→验证） | 6：`search_symbols` `analyze` `git` `preview_server` `env_detect` `system_info` | 无；项目内置（`CODE_PROJECTS`） |
| `explore` | 只读代码探索（大范围摸底 / 架构梳理） | 3：`search_symbols` `analyze` `git`（全只读） | 无 |
| `self_optimize` | 优化歌白自身（反馈/测试准入/回滚/写范围守卫） | 6：`read_feedback` `run_tests` `rollback` `journal` `backlog` `page_capture`；连带装载 `code`+`vision` | 无（`SELF_OPTIMIZE_PROJECT` 定优化根） |
| `playwright` | 浏览器自动化（导航/表单/截图/登录态/网络录制） | 30：导航交互 20 + 会话 7（`pdf`/`downloads`/`dialogs`/`emulate`/`cookies`/`local_storage`/`storage_state`）+ 本地识别 3 | 需宿主机 `node` + playwright 包 + 浏览器 |
| `reverse_site` | 网站/接口逆向（录制还原接口、改参重放、拦截 mock） | 11：`capture_*`（8）+ `route` + `http_request`；`dependencies: ["playwright"]` 自动连带 | 继承 playwright |
| `desktop` | 桌面控制（截图/窗口/键鼠/剪贴板/界面等待） | 20 | **仅本地模式**（服务端沙箱一律拒绝） |
| `vision` | 视觉能力：多模态语义分析 + 本地 OCR/定位/模板/检测 | TS 侧 `analyze` + 客卿 4（`ocr` `locate` `locate_image` `detect`）+ 语言基础 3 | `analyze` 需多模态模型；本地识别需 Python 边车依赖 |
| `wps` | Office/PDF 文档处理（Word/Excel/PPT 生成与编辑、PDF 合并拆分） | 13：`word_*` `excel_*` `ppt_*` `pdf_*` | 无（库内置） |
| `feishu_docs` | 飞书云文档：文档/表格/多维表格/知识库/云空间/权限 | 42 | **需飞书应用凭证**（`FEISHU_DOCS_APP_ID/SECRET` 或全局 `GEBAI_FEISHU_*`）；可选 `FEISHU_DOCS_FOLDER_URL`（用户文件夹 URL 或 folder token）让创建的资源落该文件夹下、用户自动有全部权限 |
| `feishu_group` | 飞书群基础能力：群/成员查询、发消息、建群改群 | 10 | **需飞书应用凭证**（`FEISHU_GROUP_*` 或全局 `GEBAI_FEISHU_*`） |
| `task` | 统一任务管理（定时/普通/闲时三类 + 排队执行） | 7：`add` `list` `update` `run` `cancel` `remove` `files` | 由 `GEBAI_TASKS_ENABLED` 统一开关（默认 true） |
| `reel` | 产品视频制作（电影感宣传片 / demo reel / 动效复刻） | 3：`setup` `project` `render`（12 动作） | Remotion 运行时 + 浏览器 + ffmpeg/ffprobe（可配目录；有 GPU 自动硬件编码） |
| `tts` | 语音合成（文本转语音：音色/语速/音调/音量 + 本机扬声器播报，纯本机离线） | 2：`speak` `voices` | 无（Windows 系统内置语音 WinRT/SAPI；非 Windows 平台不可用） |
| `imgproc`（客卿） | 图像处理（尺寸/灰度/缩放/像素统计） | 4：`info` `grayscale` `resize` `stats` | 需 C++ 边车构建 |
| `disk`（客卿） | 磁盘使用分析与清理（目录分析 + 容量总览 + 候选扫描 + 预览/隔离/删除 + 隔离区还原） | 8：`tree` `du` `top` `depth` `volumes` `scan` `clean` `trash` | 需 Go 边车构建 |

**English.** Fifteen TS sub-agents live under `packages/agents/src/agents/`, plus five multi-language sidecar projects under `keqing/` (`vision` merges with its TS counterpart, `nsight`/`torch` contribute native acceleration backends through the same same-name merge, while `imgproc`/`disk` are sidecar-only — 17 visible at runtime). All are loaded on demand. `code`/`explore`/`self_optimize` are the engineering workhorses, `playwright`/`reverse_site`/`desktop` cover browser and desktop automation, `wps`/`reel`/`tts`/`imgproc`/`disk`/`vision` cover documents, video, speech and images, while `feishu_docs`/`feishu_group`/`cron` integrate Feishu and unattended scheduling.

## 通信协议与集成 | Protocols & Integration

- **单端口**：静态 Web UI、WebSocket（`/ws`）与 REST（`/api/*`）由同一 Hono 服务在同一端口按路径路由；`/api/docs` 为 OpenAPI 文档（端点表由路由注册自动生成，保证与代码一致）
- **WS 请求**：`auth.login/logout`、`session.*`（list/create/get/delete/rename/pin/switch/restore/cancel/attach/compact/prompt/files/env/todo）、`sub_agent.*`、`approval.decide`/`choice.decide`/`env.decide`/`draw.result`/`capture.result`、`user.*`、`feedback.*`、`state.snapshot`、`sync.request`
- **WS 事件**（带 `seq`，断线重放；每用户环形日志 1000 条）：`event.message.*`、`event.tool.*`（call/result/result.start/alive）、`event.subsession.*`、`event.approval.request`/`choice`/`env`/`draw`/`capture`、`event.task.*`、`event.todo.*`、`event.session.ctx`、`event.cron.*`
- **REST**：`POST /api/v1/sessions/:id/prompt`（同步调用，支持 `autoApprove`/`stream`/附件）、`POST /api/v1/chat`（一站式：建会话→执行→返回结果）、会话文件四端点、`cancel`、`approval`、`compact`、`/api/health` 等；认证为 `Authorization: Bearer <token>` 或 HTTP Basic
- **Webhook**：事件推送（任务完成/审批请求/工具失败，可自定义白名单），配置 `secret` 时头带 `X-Gebai-Signature: sha256=<HMAC>`，失败指数退避重试 3 次
- **SDK**：`@gebai/sdk` 提供 WebSocket 连接管理 + REST 封装（`sendPrompt` 流式消费、`attachStream` 断线续流等）

**English.** One port serves the static UI, `/ws` and `/api/*`; the OpenAPI endpoint table is generated from route registration. WebSocket carries session/sub-agent/approval/choice/env/draw/capture requests plus `seq`-numbered `event.*` streams (1000-entry per-user journal, replay on reconnect). REST offers synchronous prompt calls, a one-shot `/api/v1/chat`, session-file endpoints and more, authenticated by bearer token or Basic auth. Webhooks are HMAC-SHA256 signed and retried three times with exponential backoff, and the official SDK wraps both channels.

## 测试与自测工具 | Tests & Self-Test Tooling

- **分层**：单元（工具函数/命名空间解析/环境合并/路径沙箱/截断与压缩算法，核心模块必须单测、零外部依赖）、集成（AgentEngine 主循环、审批与重试状态机、子 Agent、持久化——用 mock LLM Provider 跑完整流程、不触真实网络）、契约（WS/REST 消息格式与 SDK 一致性）、E2E
- **测试环境净化**：`bunfig.toml` 的 `[test] preload` 清除全部 `GEBAI_`/`CODE_` 前缀变量（防仓库 `.env` 与宿主 shell 残留改变审批/写守卫/Provider 断言），测试态不读仓库 `.env`
- **`fake-llm`**（`bun run --cwd packages/server fake-llm`）：无真实模型 Key 的手工端到端自测——按场景回放 OpenAI 流式响应（文本/tool_calls/usage），场景含 `text` 冒烟、`subsession_run` 子会话过程、`ask` 卡片流转、`error` 错误气泡
- **`e2e-subsession`**（`bun run --cwd packages/server e2e:subsession`）：服务级全链路——真实服务进程（独立 `GEBAI_HOME` + 独立端口 + 内嵌假模型）+ SDK，7 组场景 54 项断言；另有 `e2e:subsession:real`（真实模型）与 `e2e:subsession:ui`（常驻假模型供浏览器实测）
- **`compact-e2e`**：用真实模型量「上下文压缩后能不能接着干活」（单测只覆盖摘要输入构造，接续质量只能真模型跑）

**English.** Tests are layered into unit / integration / contract / E2E tiers (integration runs the full engine loop against a mock provider, never real networks), with `[test] preload` scrubbing `GEBAI_*`/`CODE_*` variables so host state cannot skew assertions. Three self-test tools ship in the repo: `fake-llm` (scripted OpenAI streaming without a real key), `e2e-subsession` (service-level end-to-end over a real server process plus the SDK, 7 scenarios / 54 assertions, with real-model and UI variants) and `compact-e2e` (measures post-compaction task continuation with a real model).

## 文档 | Documentation

- **`DESIGN.md`**：权威设计文档——定位与概念模型、核心不变式、模块接口、协议、安全模型、常量参考、子 Agent 规范等全部细节（README 与代码行为冲突时以 DESIGN 为准，代码变更必须同步回写）
- **`AGENTS.md`**：编码约定（子 Agent 新增指南、提交规范、测试要求）
- `docs/file-workbench-design.md`、`docs/file-workbench-implementation.md`：文件工作台设计与实现细节
- `docs/reel-render-performance.md`：视频渲染提速的调研与实测

**English.** `DESIGN.md` is the authoritative design document (positioning, invariants, module interfaces, protocols, security model, constants, sub-agent specs); `AGENTS.md` holds coding conventions; `docs/` contains the file-workbench design/implementation notes and the reel render-performance study.

## 路线图 | Roadmap

已实现的能力见上文各节（核心主循环、单文件子 Agent 与装载/子会话运行、客卿多语言边车、代码级自我优化、多用户隔离与沙箱、单二进制三形态、飞书机器人与统一任务管理、文件工作台、富内容与图表创作、Webhook/SDK/外部身份）。**尚未实现（DESIGN 明列的已知项）**：

- **服务端消息分页**：会话消息目前一次性全量返回（前端已做 DOM 窗口化，渲染开销不随历史增长），待实现 `session.get` 的窗口/游标参数与上滚按需拉取，以及极端长会话已渲染节点的 LRU 释放
- **子 Agent 选择性打包的黑名单形态**：构建期目前只有白名单 `GEBAI_BUILD_SUBAGENTS`，运行时黑名单为 `GEBAI_SUB_AGENTS_DISABLE`
- **飞书机器人 Webhook 回调模式**（现为长连接模式）
- **OIDC 身份对接**（现为 HMAC / HTTP 回调两种外部身份验证器）

**English.** Everything described above is implemented. The known open items explicitly listed in `DESIGN.md` are: server-side message pagination (window/cursor parameters plus LRU release of rendered DOM), a blacklist form of sub-agent build trimming (build time currently only supports the `GEBAI_BUILD_SUBAGENTS` whitelist; the runtime blacklist is `GEBAI_SUB_AGENTS_DISABLE`), a webhook-callback mode for the Feishu bot (long connection today) and OIDC identity integration (HMAC / HTTP callback validators today).

## 参与贡献 | Contributing

欢迎 Issue 与 PR！提交前请：

1. 读 `DESIGN.md` 与 `AGENTS.md`——设计变更必须同步回写 `DESIGN.md`（文档与代码保持一致）
2. 新增子 Agent？就是一个目录的事——在 `packages/agents/src/agents/` 下照格式写即可，记得同步 `DESIGN.md` 的「命名与预加载总览」表
3. 跑 `bun run test`、`bun run typecheck`、`bun run lint` 三件套，全绿后再提交（测试是唯一准入凭证）

**English.** Issues and PRs are welcome. Before opening a PR: read `DESIGN.md` and `AGENTS.md` (design changes must be written back into `DESIGN.md`), follow the sub-agent format when adding one under `packages/agents/src/agents/` and update the overview table in `DESIGN.md`, and make sure `bun run test` / `bun run typecheck` / `bun run lint` all pass — tests are the only admission ticket.

## 许可证 | License

[MIT](LICENSE) © GEBAI Contributors
