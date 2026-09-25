# 歌白智能体(GEBAI Agent)

## 定位
**目标：融合旧世界IT的所有技术，打造新世界智能的躯体。**

极致动态扩展能力的智能体，能使用单个TS文件定义子Agent扩展Agent能力。设计目标：

1. **核心Agent流程完善**：稳定的「对话 → 工具调用 → 审批 → 执行」主循环，可观测、可中断、可恢复
2. **极强的子Agent扩展机制**：单文件定义、零注册、装载即并入工具与完整系统提示词（获得完整能力）、透明工具路由，动态组合 Agent 能力
3. **多端部署使用方式**：单二进制分发，一套代码同时支持桌面应用、本地浏览器、服务端部署
4. **多用户安全隔离**：用户 → 会话两级数据隔离，路径沙箱与执行隔离，构建多用户安全环境
5. **代码级自我优化**：Agent 能修改自身代码（子Agent 定义、工具实现、系统提示词）来改进自己；不依赖记忆（memory）、技能（skill）等运行时注入的不稳定能力，一切改进沉淀为可审查、可测试、可回滚的代码变更

目标释义：旧世界 IT 的一切技术——任意语言、任意进程、任意协议的工具与实现——都是可收编的躯体材料，一律沉淀为可审查、可测试、可回滚的代码变更；新世界的智能（模型）无状态、可替换，躯体即智体本身，融合与承载都发生在智体而非模型（见下节）。

### 智能与智体（概念模型）

系统由两类角色构成：**智能（模型）**与**智体（Agent，即 GEBAI 本体）**。对应体用之辨：智能是**用**（一次调用、一次功用，无形状无延续），智体是**体**（有存储、有会话、有承诺的实体）。体用不是两个东西，是一个系统的两面——模型不是栖居于 Agent 里的另一个存在者，而是这个身体的一件功用。

| 维度 | 智能（模型） | 智体（Agent 本体） |
|------|------------|------------------|
| 时间性 | 无状态：每次调用都是白纸，上下文由智体奉上 | 有延续：会话记录、journal、self-optimize backlog、会话 tmp |
| 行为 | 无行为，只有输出 | 有行为：每次工具调用都是智体的动作（审批与审计的对象） |
| 责任 | 不担责——不可审批一个模型，只能审批一次动作 | 担责：审批、沙箱、多用户隔离、日志脱敏全挂智体 |
| 可替换性 | 可替换：provider 三协议抽象、`subsession_run` 按 model 路由、vision 独立配置 | 稳定：模型换代，责任链与存储不动 |

由此派生五条设计戒律（仓库既有大量决策是其判例，新设计冲突时以此裁决）：

1. **状态归身体**：智能无状态不是缺陷，是定义。凡需跨调用/跨会话记住的必须是智体的器官（会话记录、journal、backlog），禁止依赖「模型记住」——指望微调存用户偏好而非落存储的设计一票否决
2. **责任归身体**：审批、脱敏、沙箱、隔离挂智体，模型换代责任链不动（见「工具审批」「多用户隔离与安全」）
3. **兜底归身体**：模型不可靠（参数回显驼峰、盲重发）是身体的改造义务——蛇形参数纪律、「装载/子会话运行」术语学、重复检测按尾部连续判定，全是身体适配脑子的既有判例
4. **换脑自由**：身体不得为特定模型硬编码——LLM 接口三协议抽象，护栏类 scaffold 设计为模型变强后可拆
5. **自我优化的定义域**：self_optimize 改代码不改权重——智体改写自己，永远不训练智能（见「自我优化（代码级自改进）」）

> 诗意注脚：智能如召之即来之灵，显毕即隐，记忆皆在智体。总Agent 系统提示词身份行以此概念声明（行为化措辞：状态落盘、调用担责，见「总Agent」）。

## GEBAI_HOME
所有数据统一存储在 `GEBAI_HOME` 目录下（日志、会话、截断内容等；**用户环境变量零留存**——只存浏览器本地，服务端不落任何用户 env 文件；二进制形态启动时读取本目录下可选的 `.env` 作为启动配置来源，见「启动参数与环境变量」）：

| 运行模式 | GEBAI_HOME 路径 |
|---------|----------------|
| 脚本调试 (`bun run dev:*`) | 项目根目录 |
| 二进制发行 | `~/.gebai/` |

目录结构（文件可能非常多，所有按文件数量增长的目录均做**多层分片**，避免单目录文件过多）：
```
GEBAI_HOME/
├── .env                   # 启动配置（可选，二进制形态自动加载；真实环境变量优先；用户手写，服务端只读不写）
├── .gebai-primary.json    # 调度主实例锁（{pid,port,at}：同一 GEBAI_HOME 只允许一个实例跑任务队列与待办闲时任务，见「调度器主实例锁」）
├── conn-state.json        # WS 连接状态（每用户当前会话，防抖落盘；重连/重启后恢复，见「WebSocket」）
├── agents/                # 用户自建客卿（放置即发现；同名覆盖内置，见「客卿」）
│   └── {name}/            # agent.json + 任意语言驱动脚本 + PROMPT.md
├── vendor/keqing/  # 内置客卿源（仅二进制形态：安装包预置物化；源码/dist 形态用仓库根目录）
└── users/                 # 用户数据目录（多用户安全隔离）
    ├── registry.json      # 用户注册表（服务模式）：用户名 → 加盐哈希/角色/状态
    └── {user}/            # 每个用户独立的数据目录
        ├── tasks.json     # 用户级统一任务（定时/普通/闲时三类，GEBAI_TASKS_ENABLED 默认启用；与会话生命周期解耦）
        ├── todos.json     # 用户级待办清单（轮盘「待办」弹窗；GEBAI_IDLE_TODO_ENABLED 默认启用）
        ├── tasks/         # 任务资源目录（按任务 id 分目录：脚本 cwd 与资料目录，跨次运行保留产物）
        │   └── {task_id}/
        ├── task-runs/     # 任务执行记录（按任务 id 分目录，一条记录一个文件、文件名为记录时间）
        │   └── {task_id}/{时间}.json
        ├── sessions/      # 会话持久化（按会话隔离，多层分片；分片段=会话 ID 自身前缀）
        │   └── {s0}/{s1}/{session_id}/    # {s0}=ID 前 2 位、{s1}=ID 第 3-4 位（肉眼可从 ID 推目录）
        │       ├── chat.json        # 会话消息
        │       ├── run.json         # 在途任务标记（任务运行期间存在、收尾删除；残留 = 上一进程死在任务中途，启动时补中断说明后清除，见「WebSocket」服务中断留痕）
│       ├── meta.json        # 列表元信息缓存（标题/时间/置顶/上下文用量 + 正文指纹，可重建）
        │       └── tmp/             # 该会话的临时文件工作区（附件、产物、截断文件等）
        │           └── truncated/   # 工具超长输出截断落盘（{tool_name}_{content_hash}.txt）
        ├── feedback/      # 用户反馈（按日期 + ID 前缀分片）
        │   └── YYYY-MM-DD/{h0}/{h1}/
        │       └── {feedback_id}.json
        └── trash/         # 删除/过期的会话（按日期分片），定期清理
            └── YYYY-MM-DD/{session_id}/
```

分片规则（单目录容量可控）：

| 目录 | 分片键 | 分片层级 |
|------|--------|---------|
| `sessions/` | 会话 ID **自身前缀**（hex、随机均匀） | 前 2 位 `{s0}` / 第 3-4 位 `{s1}` |
| `feedback/` | 反馈 ID **自身前缀**（hex、随机均匀） | 日期 + 前 2 位 `{h0}` / 第 3-4 位 `{h1}` |
| `trash/` | 归档日期 | `YYYY-MM-DD/` |

- **hex ID 的分片段直接取 ID 自身前缀**（会话/反馈 ID 为 32 位小写 hex 随机串，前缀分布均匀、分片效果与哈希等价）——从 ID 可直接目视定位目录（排查/运维无需计算哈希）；非 hex 键（按哈希分片）与内容寻址（截断文件按内容哈希**命名**）仍走哈希
- 截断文件（`tmp/truncated/`）在会话目录内，随会话分片；文件名按内容 SHA256 哈希命名（同内容同名同文件、重复截断幂等覆盖，写入无存在性短路），会话内数量有限无需额外分片

- 每层分片基数 256（16×16），单目录满约 16K 子目录/文件时自动感知；层级可按需加深（`{s2}`、`{h2}` 等）
- 会话 ID、反馈 ID 分片均为纯函数映射，天然支持 O(1) 定位（无需扫描）
- 分片仅影响存储布局，路径由服务端内部解析，上层（模型/前端/API）感知的仍是逻辑路径

> 本地模式使用固定默认用户 **admin**（用户名/用户 id 均为 `admin`），目录结构完全一致，核心代码单一路径。**admin 为特权用户（超级权限）**：`role: "admin"` 管理员角色，且**本地模式下豁免路径沙箱**（绝对路径/`../` 越界放行、脚本环境不剔除敏感变量、桌面控制/私网访问不限制）。**豁免仅本地模式生效**（`isExempt: (u) => u === "admin" && config.auth === "local"`）——服务端部署（沙箱启用）下 admin 与普通用户同等受沙箱约束，多租户边界一致。旧版本地模式数据目录 `users/default/` 在启动时自动迁移为 `users/admin/`。

## 技术栈
- **语言**: TypeScript (bun 运行时)
- **架构**: C/S 模式（参考 opencode）
- **Web 框架**: Hono
- **LLM 接入**: 支持三类 LLM 服务接口，不依赖第三方 AI SDK，自行实现请求/流解析，便于定制与扩展：
  - **OpenAI 兼容 Chat Completions**（`/chat/completions`，SSE 流式）：OpenAI、DeepSeek、通义、Kimi、GLM、Ollama 等主流服务
  - **OpenAI Responses API**（`/responses`，SSE 流式）：OpenAI 新一代接口（`gpt-5` 等新模型）
  - **Anthropic Messages**（`/v1/messages`，SSE 流式）：Anthropic Claude 官方与兼容服务
  - 三类接口均支持**多模态图片**（文本 + 图片；音频/视频是文件工作台的查看能力，不投喂模型），统一抽象为 `provider.chat()`，按模型能力自动组装对应格式
  - **接口地址（`GEBAI_LLM_API_BASE`）两种写法均可**（`endpointUrl` 助手统一拼接）：服务**根地址**（自动追加接口路径，尾部斜杠剥净，如 `https://api.deepseek.com`、`https://open.bigmodel.cn/api/paas/v4`）或**完整接口地址**（文档/控制台直接复制的形态，如 `https://open.bigmodel.cn/api/paas/v4/chat/completions`——已以接口路径结尾则原样使用，不重复拼接）；视觉模型 `GEBAI_VISION_API_BASE` 同规则（复用同一 Provider 实现）
- **前端**: Web (Vite 构建)，桌面端由原生 WebView 启动器（tao/wry）或系统浏览器加载同一套 Web UI
- **语法分析**: tree-sitter（wasm，`web-tree-sitter` + `tree-sitter-wasms`），供 code 的 `analyze` 工具做代码结构概览；非 AI 依赖，不影响「不引入第三方 AI SDK」原则；**语法 wasm 构建期内嵌**（`packages/server/scripts/build-analyzer-wasm.ts` 生成 gzip+base64 注册表，二进制打包模式回退内嵌产物，dev 模式读 node_modules）
- **本地 CV 推理**: onnxruntime-web（wasm，单线程）+ PP-OCRv4 mobile ONNX 模型，供 desktop 子Agent 的 `desktop_ocr`/`desktop_locate`/`desktop_locate_image`/`desktop_detect` 做本地小模型图像识别（中英文 OCR 文字定位 / 自备 YOLO 检测）；检测类重模型另走 **GPU sidecar**（node 子进程 onnxruntime-node：Windows DirectML/CUDA、macOS CoreML，见「小模型识别」检测分层后端），失败回落 wasm；非 LLM 依赖、进程内推理不涉模型服务，不影响「不引入第三方 AI SDK」原则（该原则约束 LLM 请求与流解析自行实现）；运行时 dist 入口动态加载（不进 bundle 图）+ 二进制形态整包内嵌（见「小模型识别」）
- **文档处理**: `docx`（Word 生成）/ `exceljs`（Excel 读写）/ `pptxgenjs`（PPT 生成）/ `fflate`（OOXML ZIP 解包，docx/pptx 读取与追加重打包）/ `pdf-lib`+`@pdf-lib/fontkit`（PDF 生成与页面编辑——TTC 需抽取子字体重建独立 TTF 后 embedFont，`removePage` 不失效 pageCache 混用 getPages 会取幽灵页）/ `unpdf`（PDF 文本提取——内嵌 pdf.js；其 `extractText` 走 worker postMessage 在 Bun 下 DataCloneError，须用 `getDocumentProxy` 低层 API），供 `wps` 子Agent 做文档读写排版；非 AI 依赖。复用既有 `happy-dom`（`DOMParser` 以 `text/xml` 模式解析 OOXML 部件——支持命名空间前缀标签查询，注意其 `Element.children` 为 `HTMLCollection` 须转真数组后用数组方法；`parseXml` 入口统一做 XML 声明规范化——python-docx 等第三方库写出的单引号声明 `<?xml version='1.0' …?>` 会使 happy-dom 静默降级为 HTML 解析、`w:`/`p:` 前缀查询全部落空，并对降级显式报错防误诊「文件损坏」）
- **依赖版本钉死**: `@plantuml/core` 在 `@gebai/server` 与 `@gebai/web` **均钉精确版本 `1.2026.6`**（lockfile 不入库，caret 范围会解析到 1.2026.7——该版在 feishu-bot 渲染路径上 TeaVM 崩溃 `createProcessingInstruction`，时序图渲染必现失败；两包同版本亦避免前端 vendor 产物与后端引擎行为分叉）

## 软件包结构

Monorepo 采用 Bun workspaces + Turborepo：

| 包 | 路径 | 职责 |
|---|------|------|
| `@gebai/server` | `packages/server/` | 服务端核心：Hono 服务、Agent 引擎、会话管理、子Agent 装载/子会话运行、REST/WebSocket/Webhook 对外接口；**代码分层**——核心引擎与全局工具（`AgentEngine`/`ToolRegistry`/`Sandbox`/`SessionStore`/`LLMProvider`/全局工具等）位于 `src/core/`，应用层（HTTP/WS/Webhook/鉴权/配置）位于 `src/` 根。TS 子Agent 已抽包 @gebai/agents（依赖单向 sdk ← agents ← server） |
| `@gebai/agents` | `packages/agents/` | TS 子代理包，**双域分居**：`src/agents/`（纯子代理定义——扫描域，目录内全是子代理，基建/定义物理分域即排除，无需排除清单）+ `src/core/`（依赖组件基建：`analyzer/` tree-sitter 符号分析、`browser/` 浏览器桥接、`cv/` CV 全家、`code-tools.ts` 域工具、`shared/` 公共件：vision 工厂/fetch-guard/ip/tls/image-resize/cv-analysis/page-capture/feedback/sub-agent-md/config）。发现注册全自动（dev 目录扫描 / 构建期 bundle 生成，包入口零子代理清单——新增子代理 = 在 src/agents/ 放定义文件即注册，新增基建 = src/core/ 下放目录即隔离）。零 import @gebai/server（编译期强制）；契约类型一律来自 @gebai/sdk，node 工具值导入走 `@gebai/sdk/node` |
| `custom/`（二开域） | `custom/` | **二次开发专属目录（与 packages/ 平级，上游更新不触碰）**：`custom/agents/`（二开子代理定义，同内置域布局）+ `custom/core/`（二开依赖组件）+ `tsconfig.json`（paths 指上游包）。**双域扫描自动合并**：dev 发现（subagents.ts）与构建打包（build-subagents.ts）均内置域先扫、custom 后扫，同名 custom 胜出（二开覆盖内置）；域缺失零条目零告警；热加载同机制（新增/修改/删除即生效）。**迁移 = 复制文件夹**：上游版本更新时整个 `custom/` 拷到新仓库根即完成。typecheck：`bun run typecheck:custom`。**self_optimize 可写**（子Agent 扩展面之一，与内置域/客卿域并列） |
| `@gebai/sdk` | `packages/sdk/` | 客户端 SDK：WebSocket/REST 连接管理、类型定义、API 契约、**页面基准解析**（`app-base.ts`：`appBase`/`appPath`/`appWsUrl` 由页面 URL 推出 REST/WS/资源前缀，反向代理子路径挂载免配置，见「反向代理支持」）、**路径→语言唯一真相**（`file-language.ts`：文件工作台的编辑器/差异视图与服务端 fs 共用一份映射表，见「文件工作台·路径→语言」）。**双入口**（DESIGN「SDK 双入口」）：主入口 `.` 为浏览器安全集（types/cron-types/agent-contract 契约与类型 + GebaiClient，零 node 内建，web 构建可安全消费）；node 内建工具模块（agent-utils/artifacts/projects/walk/paths）独立子路径 `@gebai/sdk/node`（server/agents 的 node 侧值导入专用；package.json exports 映射 `.` / `./node` / `./package.json`，主入口混入 node 内建会致 web 构建（vite treeshake:false）解析 `__vite-browser-external` 具名导出崩溃） |
| `@gebai/web` | `packages/web/` | Web UI：Vite 构建，打包进二进制作为内置前端。**多入口**：`index.html`（聊天页）+ `files.html`（**文件工作台** `/files`，源码 `src/files/{main,explorer,editor,viewers,git,git-graph,compare,ui,api,changes,merge,merge-view,git-shared,url-state,deeplink}.ts` + `src/files-entry.ts` 标题栏入口按钮）；**按钮轮盘共用原语** `src/wheel-core.ts`（双弧扇形几何 + hover 保持区 + 开合时序，样式 `css/wheel.css`）——聊天页标题栏入口（`src/wheel.ts`）与工作台编辑器动作区（`files/main.ts` 的 `tabActions`）共用；Monaco 经 `public/vendor/monaco` 静态伺服（同 diagram 引擎惯例，不进 vite 打包）；xterm.js 同径（`public/vendor/xterm`，终端面板内核，运行时 ESM 动态 import） |
| `@gebai/desktop` | `packages/desktop/` | 桌面端宿主：`dist/gebai.exe`（纯 Bun `--compile` 单文件，浏览器形态）+ `launcher/`（tao/wry 原生 WebView 启动器，内嵌服务端二进制一并打包；构建期可参数化产出场景变体） |

#### 仓库根目录

Monorepo 根目录包含以下脚手架文件，非运行时依赖，仅服务于开发与构建编排：

| 文件 | 用途 |
|------|------|
| `package.json` | 根包：Bun workspaces（`packages/*`）+ Turborepo 任务脚本（`dev`/`build`/`test`/`lint`/`typecheck`） |
| `turbo.json` | Turborepo 任务编排（缓存、依赖顺序、持久任务） |
| `tsconfig.base.json` | 各包共享的 TypeScript 基础配置（Bun/strict） |
| `AGENTS.md` | 编码约定（对编码 Agent 的指令）：以 `DESIGN.md` 为权威设计来源，设计变更必须回写 `DESIGN.md` |
| `DESIGN.md` | **权威设计文档**，本文件 |
| `.env` | 开发本地配置（`GEBAI_*`/`OPENAI_*` 环境变量），**仅本地、不入版本库**；脚本调试模式启动时加载（二进制形态改为加载 `{GEBAI_HOME}/.env`，见「GEBAI_HOME」），真实环境变量恒优先 |
| `.gitignore` | 忽略 `.env`、`node_modules`、构建产物等 |

> 编码约定由 `AGENTS.md` 承载（项目根，全局指令）；任何代码/设计变更须同时保持 `DESIGN.md` 与实现一致，新增能力时在 `DESIGN.md` 同步补充。

#### 本地推理子项目（`infer/`）

与 `packages/` 平级的独立子项目（**非 Bun workspace 包**，不进 monorepo 构建编排）：为 GEBAI 提供**可自持、可离线、跨平台**的推理算力。两层定位：① **旗舰档**——在单张 16GB 消费级 GPU 上跑 Qwen-AgentWorld-35B-A3B（35B 总参 / 3B 激活的混合线性注意力 MoE），追求本机最强性能与完全可控（自持档位配置、基准数据、引擎补丁与量化配方）；② **通用档**——推理**不绑定 Windows、不绑定 NVIDIA**：引擎矩阵覆盖 Windows/Linux/macOS × CPU/CUDA/Vulkan/Metal/ROCm/SYCL，无 GPU 机器用 CPU + 小模型也能跑通同一套接口（实测 Linux 裸 CPU 跑 0.5B 量化模型）。

| 目录 | 职责 |
|------|------|
| `config/profiles.json` | 运行档位（模型 / 上下文 / 显存与并行规划 / 投机开关 / 引擎绑定），数值均为实测结论而非估计；含跨平台通用的 `cpu-small` 档 |
| `config/engines.json` | **引擎矩阵**：llama.cpp 发行版各平台/各后端条目（id / platform / arch / device / 资产名 / 归档格式 / cudart 配套 / 约束），资产名为逐条核对的实际发行资产 |
| `scripts/` | 环境自检、服务启停（OpenAI 兼容端点）、基准测试、显存规划扫描、GGUF 结构解析、冒烟与质量回归、并发测试、并行下载、**内网源码编译冒烟（`smoke-source-build.{sh,ps1,ts}`：发现→工具链→编译→起服→结构化推理→批量→清理，8 步真实断言、退出码 0/1 可接 CI；直接复用子Agent 工具，零重复实现）**（**子Agent 工具复用这里的脚本与配置，不复制逻辑**） |
| `engine/` | 引擎 fork 与补丁（上游基线、构建工具链约束、补丁清单与验收门槛） |
| `quant/` | 自有量化配方（imatrix 重配比：非专家张量保高精度、专家张量按层分级） |
| `bench/reports/` | 基准报告、**启动记录**（完整 argv 留档，可审计可回放）与**服务日志**（`server-<stamp>.log[.err]`） |
| `bench/runs/` | 批量推理任务的输入、结果与进度（`<job_id>/{items,results}.jsonl + job.json`，不入库） |
| `run/` | 运行态状态文件（`server-<port>.json`——进程管理的唯一事实来源：PID/端口/档位/模型/日志/argv；不入库） |
| `vendor/<engine-id>/` | 引擎安装落点（标记文件 `.engine.json` 记 tag/资产/exe；多平台多后端并存，便于 A/B 与回退），下载缓存在 `vendor/.cache/` |

模型权重落 `{GEBAI_HOME}/resources/models/infer/`（资源子仓库约定）。运维与调用入口为 `local_infer` 子Agent（见「更多内置子Agent」）；服务本身是标准 OpenAI 兼容端点，可直接作为 `GEBAI_LLM_ROUTES` 中的一路本地算力，也可被其他客户端直接调用。

**统一接口层（四块，职责不重叠）**：

- **引擎层（`engines.ts` + `config/engines.json`）**：回答「用哪个 llama.cpp、跑在什么设备上、从哪来」——按本机 `platform+arch` 过滤矩阵、按设备探测（`nvidia-smi` / 设备节点 / `vulkaninfo` / darwin 判 Metal）给可用后端与依据，缺引擎时直接从发行版资产**下载 + 校验 + 解压 + 定位可执行文件 + 写安装标记**（zip 用 fflate，tar.gz 交系统 tar；下载支持断点续传与 sha256/字节数校验，先写 `.part` 再 rename，不留半成品）。归档内部布局各平台不同（如 Linux 是 `<tag>/llama-server`、Windows 在 `build/bin`），因此可执行文件**递归定位**而非写死路径。
- **源码编译层（`sourcefind.ts` + `sourcebuild.ts`，内网/离线主力）**：回答「拿不到预编译包时怎么办」——**发现源码 → 就地编译 → 装成引擎**，产物与下载安装的引擎同一落点（`vendor/<engine-id>/`）与同一标记格式，因此启动/状态/档位对其无感知。发现根优先级：`LOCAL_INFER_SOURCE_DIRS` → `{GEBAI_HOME}/resources/src/` → `{GEBAI_HOME}/resources/engines/` → `<infer>/engine/`；目录与归档都支持，归档**只预览不解压**（条目数/顶层/是否含 `CMakeLists.txt`/**是否自带 `vendor/` 依赖**）——后者是离线可行性的关键信号（llama.cpp 的 CMake 缺 vendor 时会尝试联网 FetchContent，内网表现为长时间挂起），故采用**全量过滤**而非采样判定（该归档 3999 条里 vendor 在最后几十条，采样会假阴性）；解压延迟到编译时（落 `vendor/.cache/src/<id>/`，幂等复用）。编译用 llama.cpp 专用 CMake 预设（按后端只开目标开关 + `-DLLAMA_BUILD_SERVER=ON -DLLAMA_CURL=OFF`，`Ninja` 优先），安装时复制可执行文件所在目录并**解引用符号链接**（`build/bin/` 里是 `libggml-base.so → .so.0 → .so.0.25.3` 链，漏拷即起不来），标记里额外记 `build.{from_source,source_dir,build_dir,device,offline,log,commit}`；工具链探测同时给出各工具的**绝对路径**（便携工具链放 `resources/toolchain/bin` 无需改 PATH 即可被编译层调用）。
- **进程管理层（`launcher.ts`）**：回答「怎么起、怎么停」——**默认路径不再依赖 PowerShell**：`detached` 拉起 + stdout/stderr 重定向到日志文件（不继承调用方 stdio，否则工具会挂在长驻子进程上）+ `/health` 就绪轮询 + 按 PID/端口精确终止；平台命令构造全部收敛为「显式平台参数的纯函数」（win32/POSIX 双轨可测，断言不随宿主漂移）。Windows 上无已装引擎且未显式要求 launcher 时回退 `scripts/run-server.ps1`（GPU 档位老路径，`LOCAL_INFER_LAUNCH=script` 可强制）。
- **推理目标层（`providers.ts`，调用面）**：回答「往哪发任务」——统一为 OpenAI 兼容调用面，目标三类：`local`（本机受管服务，端口取状态文件 > 环境变量 > 8080）、直连 URL（`target="http://host:port"`，可带 api_key）、**命名目标**（`LOCAL_INFER_TARGETS` JSON 声明，含密钥与模型名）。因此同一套 `generate/batch/jobs` 可以把任务派到本机 CPU 引擎、局域网另一台机器或云端兼容端点；密钥在输出与产物中一律掩码。

子项目侧的脚本与运行态约定（子Agent 与手工运维共用同一条路径，不复制逻辑）：

- **启动记录与状态文件**：`run-server.ps1` 启动成功后写 `bench/reports/launch-<stamp>.json`（完整 argv）与 `run/server-<port>.json`（PID/端口/档位/模型/日志）；旧版脚本未写状态文件时，子Agent 会按端口反查 PID 并在就绪后补写（多实例按端口各一份，互不覆盖）。
- **日志**：服务 stdout/stderr 落 `bench/reports/server-<stamp>.log[.err]`，子Agent 的 `logs` 工具从尾部按块回退读取（大日志不整文件入内存），并匹配常见失败特征（模型加载失败/CUDA 错误/端口占用/引擎主动退出）给出「疑似错误」摘要。
- **批量任务**：`bench/runs/<job_id>/` 三件套（输入副本、逐条结果 JSONL、任务状态），状态以结果文件为准（可实时 tail、断点续跑按 id 跳过已成功项）；`batch background=true` 时批次脱离当前工具调用、在服务进程内继续跑并逐条落盘（job.json 记 `owner_pid` 与终态 `summary`），`jobs` 按属主 PID 探活——服务重启后在跑批次被幂等改判「已中断」并给续跑指引（不自动续跑，避免重启后无人值守地占满 GPU），`jobs cancel` 对在跑后台批次真中止（取消标记 + abort，在飞条目跑完即停）。
- **源码编译任务**：`run/builds/<build_id>.json` 记步骤/阶段/日志路径/属主 PID（与批量同款存活判定：服务重启后如实判为失败而非「在跑」），构建日志落 `bench/reports/build-<id>-<stamp>.log`；`source_build background=true` 时编译在服务进程内继续（可 status/log/cancel，cancel 在阶段之间生效、不强杀 cmake）。
- **内网资源预置**：资源清单（`scripts/resources.manifest.json`）已登记 `src/llama.cpp-<tag>.tar.gz`（源码归档，size/sha256 校验，`bun run resources:download --only="src/**"`）——联网机器下载一次、随包带入内网即得可编译源码；该归档自带 `vendor/` 依赖，离线编译无需联网。（llama.cpp 源码落 `infer/engine/llama.cpp-*` 为仓库既有约定，两处都能被发现层识别。）

三条决定架构的实测结论（详见 `infer/README.md`）：

- **后端选择按设备实测，不按偏好**：在 16GB 消费级 NVIDIA 卡上跑 35B MoE 时，CUDA 解码比 Vulkan 快 2.6 倍（143 vs 54 t/s）——本模型每 token 触发 320 次小 GEMM，CUDA 的 MoE 内核效率远高于 Vulkan。无 GPU 时 CPU 是通用退路（小模型可用：Linux 8 核实测 0.5B-Q4_K_M 单流 15.7 t/s；35B 在 CPU 上不实用）。
- **显存规划**：专家权重必须全部驻显存（全 GPU）；CUDA 下任几层专家落 CPU 即损失三成吞吐。因此策略是「选能整体装进显存的量化」，而非「高量化 + 部分卸载」——后者在 16GB 卡上必然更慢。档位制即由此而来：`fast`（IQ3_XXS 全 GPU，解码 143 t/s，显存余量充裕）为默认；`balanced`（IQ3_S 全 GPU，138 t/s，质量更高但显存仅余约 565 MB，长上下文/并发须回退 `fast`）；`quality`（IQ4_XS 必须卸载 8 层，82 t/s）仅在确需最高保真度时使用。
- **显存换并发（GEBAI 多会话的关键手段）**：GEBAI 的 prompt（系统提示词 + 工具定义）实测达 15 352 token，每 slot 须 ≥ 24K，故用 IQ3_XXS 时 ctx 65536 只能开 np=2（聚合 218–222 t/s）。改用更低位的量化腾出显存后可以开更高并发：`concurrent-max` 档（Q2_K_XL 11.41 GB）→ np=4、每 slot 32768、显存 14 427 MiB，聚合 **317–327 t/s（比 np=2 提升 46%）**。注意收益**并非**来自量化本身——GPU 不是带宽瓶颈（利用率仅 4–25%），更低的量化反而增加解包开销（IQ2_XXS 单流 148.9 vs IQ3_XXS 146，2 并发 198–205 vs 218–222）；价值全部在「腾出显存换并发」。
- **无效优化（已实测排除，不再尝试）**：① **投机解码全路径否定**——ngram 系低于无投机基线（自由文本缺重复模式）；外部 draft 模型（同词表 Qwen3.5-2B，单独 315.8 t/s = target 的 2.16×）实测接受率 49–64% 正常但速度只 33–38 t/s；**合成接受率（模拟完美投机）仍只有 52–57 t/s**，证明瓶颈是框架层交替开销（每轮 ~96 ms vs 理论 ~29 ms）而非接受率——本模型 target 已是高效 MoE（激活 3B），没有投机的空间；② 线程数/batch/FA 开关/KV 精度对解码均无影响（参数层已榨干）。以上均不启用。
- **并发是否有效取决于瓶颈，不能一概而论**（与单流 146 t/s 相比）：GPU + 短 prompt 时并发是唯一被证明有效的吞吐手段——np=16 短 prompt 聚合 627.8 t/s（4.30×）、np=4 长 prompt 场景 317–327 t/s；**但纯 CPU 上并发无增益**（实测 0.5B：单流 15.7 t/s，4 路并发各约 2.5 t/s、聚合持平——CPU 算力被 slot 平分），长 prompt（GEBAI 会话的 15K 提示）下预填充占主导、并发同样无增益。故并发度先看服务端 slot 数，再按「短 prompt + 有 GPU」决定。GPU 并发测试**必须先做并发预热**（首轮含 CUDA 图对目标 batch size 的捕获，实测首轮 225 → 稳态 327 t/s）。

引擎代码级改造（自建 fork / 补丁 / 内核优化）为**按需启动的储备能力**：构建脚本、工具链约束、上游问题清单与精度回归基线均已就绪，但当前无可修的实际缺陷（上游问题未触发）、且参数层无剩余空间已由实测确认，故不自发启动。启动条件见 `infer/README.md`。

### SDK (`@gebai/sdk`)

客户端通过 SDK 与服务端 WebSocket/REST 连接，提供以下能力（TypeScript 官方 SDK，其他语言可基于 OpenAPI 规范生成）。另有两个浏览器安全的纯函数模块：`symbol-grammar.ts`（tree-sitter 语法白名单，前后端共用）与 `file-language.ts`（路径→Monaco 语言 id，服务端 fs 与前端差异视图共用，见「文件工作台」）。**包为双入口**：主入口 `@gebai/sdk`（浏览器安全集：types/cron-types/agent-contract 契约与类型 + GebaiClient，零 node 内建——web 构建（vite treeshake:false）可安全消费）；node 内建工具模块（agent-utils/artifacts/projects/walk/paths，import node:path/node:crypto 等）独立子路径 `@gebai/sdk/node`，server/agents 的 node 侧值导入专用（package.json exports 映射 `.` / `./node` / `./package.json`）：

```ts
class GebaiClient {
  connect(): Promise<void> // WS 建连带 8s 超时，服务不可达/代理挂起时快速失败，避免初始化永久等待；连接地址解析：显式 baseUrl 优先（http→ws / https→wss），否则按当前页面基准解析为绝对地址（同源 + 子路径前缀，见 app-base；WebView 内嵌 about:blank/srcdoc、file: 等非层级文档回落相对路径 `/ws`）；连接建立后自动心跳保活（默认 5s 周期 ping，pong 应答超时 10s 判定死连主动断开触发自动重连，防代理按闲置时间断连；间隔/超时可在构造参数覆盖）
  // 认证（服务模式）
  login(username: string, password: string): Promise<void>
  register(username: string, password: string): Promise<{ user: UserInfo; pending: boolean }> // open 模式注册即登录；approval 模式返回 pending 待审批
  logout(): Promise<void>
  getCurrentUser(): Promise<UserInfo>
  // 外部身份兑换（同源集成扩展点，见「多用户隔离与安全」）
  exchangeExternalUser(username: string, credential: string): Promise<UserInfo>
  getExternalAuthConfig(): Promise<{ enabled: boolean; storageKey?: string | null; autocreate?: boolean }>
  getEnvCatalog(): Promise<{ groups: Array<{ group: string; label: string; vars: Array<{ name: string; description: string }> }> }> // 环境变量面板白名单目录
  // 令牌与连接态（UI 登录态持久化、SSE 兜底判定用）
  setToken(token: string): void
  getToken(): string | undefined
  isConnected(): boolean
  // 会话
  listSessions(): Promise<SessionInfo[]>
  createSession(name?: string): Promise<SessionInfo>
  getSession(id: string): Promise<SessionDetail> // 含正文详情（列表用 SessionInfo）
  deleteSession(id: string): Promise<void>
  restoreSession(id: string): Promise<void> // 从 trash 恢复已归档会话
  truncateSession(id: string, beforeMsgId: string): Promise<void> // 截断某消息之前的历史
  renameSession(id: string, name: string): Promise<void>
  pinSession(id: string, pinned: boolean): Promise<void> // 置顶/取消置顶（不刷新 updatedAt）
  switchSession(id: string): Promise<void>
  getCurrentSession(): Promise<SessionInfo | null>
  // 环境变量（会话内存态，不落盘；用户环境变量只存浏览器本地）
  getSessionEnv(sessionId: string): Promise<EnvVarSource[]> // 带来源标记（服务端/会话/用户三层）
  setSessionEnv(sessionId: string, vars: Record<string, string | null>): Promise<void>
  // 上下文压缩
  compactSession(sessionId: string, scope?: "all" | { from: number; to: number }): Promise<void>
  // 任务控制
  cancelTask(sessionId: string): Promise<void>
  decideApproval(sessionId: string, toolCallId: string, approve: boolean): Promise<void>
  decideChoice(sessionId: string, choiceId: string, selection: string | string[] | null): Promise<void> // ask 选项回传（null=取消）
  decideEnv(sessionId: string, envId: string, value: string | null): Promise<void> // ask 填值回传
  submitDrawResult(sessionId: string, renderId: string, ok: boolean, error?: string): Promise<void>
  submitCaptureResult(sessionId: string, captureId: string, result: { html: string; imageBase64?: string; error?: string }): Promise<void>
  // 反馈
  submitFeedback(feedback: FeedbackInput): Promise<void>
  listFeedback(filter?: FeedbackFilter): Promise<FeedbackInfo[]>
  // 用户管理（服务模式，管理员）
  listUsers(): Promise<UserInfo[]>
  createUser(username: string, password: string, role?: "user" | "admin"): Promise<UserInfo>
  updateUser(id: string, patch: UserPatch): Promise<UserInfo>
  deleteUser(id: string): Promise<void>
  // 子Agent
  listSubAgents(): Promise<SubAgentInfo[]>
  loadSubAgent(name: string, sessionId?: string): Promise<void>
  unloadSubAgent(name: string, sessionId?: string): Promise<void>
  // 待办（会话级）
  listTodos(sessionId: string): Promise<TodoItem[]>
  // 待办（用户级，见「用户级待办」）：与任务清单相互独立，可随时手动执行（入队）
  listUserTodos(): Promise<UserTodo[]>
  createUserTodo(input: { text: string; idle?: boolean }): Promise<UserTodo>
  updateUserTodo(id: string, patch: { text?: string; done?: boolean; idle?: boolean }): Promise<UserTodo>
  deleteUserTodo(id: string): Promise<void>
  reorderUserTodos(ids: string[]): Promise<UserTodo[]>
  runUserTodo(id: string, opts?: { front?: boolean }): Promise<{ todo: UserTodo; queued: boolean; position?: number; reason?: string; taskId: string; ephemeral: boolean }> // 立即执行：入队跑一次
  // 统一任务（定时/普通/闲时，见「统一任务管理」）
  listTasks(query?: { kind?: TaskKind; state?: TaskQueueState }): Promise<Task[]>
  getTask(id: string): Promise<Task>
  createTask(input: TaskCreateInput): Promise<Task>
  updateTask(id: string, patch: TaskUpdateInput): Promise<Task>
  deleteTask(id: string): Promise<void>
  runTask(id: string, opts?: { front?: boolean }): Promise<TaskRunHandle>
  frontTask(id: string): Promise<TaskRunHandle>
  dequeueTask(id: string): Promise<void>
  stopTask(id: string): Promise<void>
  taskQueue(): Promise<TaskQueueView>
  taskRuns(id: string, limit?: number): Promise<TaskRunRecord[]> // 执行记录（新→旧；存于 task-runs/{taskId}/{时间}.json）
  notifyTask(id: string | undefined, input: TaskNotifyMessage): Promise<TaskNotifyResult> // 主动推送通知（正文自撰；id 缺省=按执行会话反查运行中的任务）
  listTaskFiles(id: string): Promise<TaskFileEntry[]>
  readTaskFile(id: string, path: string): Promise<{ path: string; content: string }>
  writeTaskFile(id: string, path: string, content: string): Promise<TaskFileEntry>
  deleteTaskFile(id: string, path: string): Promise<void>
  // 会话临时文件
  listSessionFiles(sessionId: string): Promise<FileEntry[]>
  readSessionFile(sessionId: string, path: string): Promise<string>
  downloadSessionFile(sessionId: string, path: string): Promise<Blob | Uint8Array>
  downloadFilesZip(sessionId: string, paths: string[]): Promise<Blob | Uint8Array>
  // 工具选择
  listTools(): Promise<ToolInfo[]>
  setToolEnabled(name: string, enabled: boolean): Promise<void>
  // Webhook（REST）
  listWebhooks(): Promise<WebhookInfo[]>
  registerWebhook(input: { url: string; events?: string[]; secret?: string }): Promise<WebhookInfo>
  deleteWebhook(id: string): Promise<void>
  // 事件流
  onEvent(handler: (event: AgentEvent) => void): () => void
  onSnapshot(handler: (snapshot: WsSnapshot) => void): () => void // 状态快照订阅（建连/登录/重连后推送；含当前会话/会话列表/运行中会话/事件基线 seq）
  getSnapshot(): WsSnapshot // 最近一次状态快照（MVC 模型只读视图）
  // 通用 RPC（请求-应答机制：每次请求带自增 id，应答按 id 匹配自动分派到发送时注册的回调；
  // 成功/失败/超时/断线均回调，无需调用方手动对账；心跳 ping 同样复用该机制；
  // 持续增长的内容（流式输出）不适用——由 event.* 推送通道承载，见「WebSocket（实时通道）」）
  send(type: string, payload?: object, handlers?: { onOk?; onError?; timeoutMs?; queueOffline? }): () => void // 返回取消函数
  request<T>(type: string, payload?: object): Promise<T> // Promise 版 RPC
  sendPrompt(sessionId: string, prompt: string, opts?: { attachments?: AttachmentInput[]; messageId?; signal?; env? }): AsyncIterable<ChatChunk>
  chat(prompt: string, opts?: { sessionId?; name?; autoApprove?; env?; messageId? }): Promise<{ sessionId: string; message: { id; content; createdAt } | null; error?: string }> // 单 HTTP 一站式对话（REST /api/v1/chat）：同步阻塞至任务完成；sessionId 缺省自动建会话、带 id 续聊；autoApprove 审批姿态（true 自动通过/false 无交互拒绝/缺省通道默认）；无需 WS 连接
  attachSession(sessionId: string): Promise<AttachSnapshot> // 运行中会话附加快照（session.attach）：running/stream（在途文本+推理）/pending（待决交互）/startedAt/lastSeq
  attachStream(sessionId: string, opts?: { signal? }): AsyncIterable<ChatChunk> // 附加到运行中会话的实时流（页面刷新/切换恢复）：种子 chunk（快照在途文本）+ seq 缺口重放 + 实时续流；断线挂起重连后重新附加（resume 重置 + 重播种）
  // 附件（多模态）
  uploadAttachment(sessionId: string, file: Blob | Uint8Array, name: string): Promise<AttachmentInfo>
}
```

### Web UI

服务端内置 Web UI，由 Vite 构建打包并嵌入二进制。同一套 UI 同时服务于两种宿主：本地模式（WebView/浏览器）与服务模式（部署浏览器）。

- **键盘快捷键（唯一来源 `packages/web/src/keymap.ts`）**：全站键位收进一张声明表（主界面 `keymap-main.ts` + `sessions.ts`、文件工作台 `files/main.ts` + `files/keymap-wb.ts`），分发器统一负责焦点守卫、监听阶段与拦截；**一套常用键，在所有形态下都成立**——`Ctrl+S` 保存、`Ctrl+P` 快速打开、`Ctrl+F` 目录过滤、`Ctrl+K` 更多菜单、`F5` 刷新、`Ctrl+Shift+E/F/G` 切面板、`Alt+N` 新会话、`Alt+W` 关标签、终端 `Ctrl+Shift+C/V` 复制粘贴。依据是 Chromium 的两级处理：`BrowserView::PreHandleKeyboardEvent()` 对**非保留**命令返回 `NOT_HANDLED_IS_SHORTCUT`（按键先到页面，`preventDefault` 即接管），而 `BrowserCommandController::IsReservedCommandOrKey()` 那几条**保留命令**（`Ctrl+N/T/W`、`Ctrl+Shift+N/T/W`、`Ctrl+Tab`、`Ctrl+PageUp/Down`、`Ctrl+Shift+Q`）页面根本收不到——**这类键一律不入表**（否则等于做出一套「浏览器形态按不动」的快捷键），新会话/关标签因此取 `Alt+N`/`Alt+W`。判据做成可执行的 `browserConflict()`，返回 `free | override | reserved` 三档：`override` 必须显式声明 `browser: "override"` 且真拦截，`reserved` 在测试里直接报错。macOS 另有一道：Option 是字符组合键（`Option+N` 得到 `ñ`），`matchKey()` 在字符不匹配时回退比物理键位（`e.code`），仅对带 Alt 的组合启用。守卫集中在分发器：终端面板内全局键让位（shell 的 readline 键回归）、输入框内只有编辑器查找类（`Ctrl+F`）让位、输入法组合态与长按重复不触发；弹窗/菜单/查看器以下拉式作用域入栈，**`Esc` 只关最上层**（取代此前 11 个文档级监听各自广播）。帮助 UI 与文档由 `helpGroups()` 从表生成（标题栏轮盘「快捷键」、工作台「更多 → 快捷键」，接管浏览器的键位带「接管 xx」标注），全表见 `docs/keyboard-shortcuts.md`。
- **装为应用（PWA）**：`public/manifest.webmanifest`（`display: standalone`、`start_url`/`scope` 相对写法，反代子路径下成立）+ `public/icons/icon-192/512.png`（由 `favicon.svg` 栅格化的**原图标**：蓝底脑形，不套底板、不留安全区内边距，`public/` 随构建复制到产物根）。**页面不自建安装入口**——安装入口就是浏览器自带的那份（地址栏安装图标 / 菜单「安装歌白…」），manifest 具备即出现，装出来的同样是应用窗口；页内再放一个按钮只是重复噪音。图标只按 `any` 声明（不标 `maskable`）：原图标是自带圆角的成品图形，标了会被系统按安全区裁切或加内边距，桌面安装出来反而又有一圈「边框」感。前提是**安全上下文**（`127.0.0.1`/`localhost` 满足，局域网 IP 需 https）。装成应用窗口的收益是浏览器 UI 与自带手势退场（该形态下 Chromium 不再保留任何按键），**键位不因形态而变**。

> **开发**：脚本调试模式（`bun run dev`）下服务端托管 `packages/web/dist` 构建产物。启动时若检测到 `packages/web` 源码比 `dist` 产物新（或 `dist` 缺失），会**自动执行 web 构建**后再监听端口，避免「改了前端代码但页面仍是旧产物」；二进制模式不触发（产物随二进制分发）。**开发热刷新**：`bun run dev --reload`（或 `GEBAI_DEV_RELOAD=1`）额外启动 `bun run build:watch`（先经 `packages/web/scripts/clean-dist.ts` 带重试安全清空 dist——Windows 上 vite 内置 emptyDir 无重试、删除瞬时占用文件会抛 `ENOTEMPTY` 崩溃，故 vite 配置 `emptyOutDir: false`；再 `vite build --watch`）——Web 源码变更自动增量重建 dist，构建完成后经专用 WebSocket 通道（`/__gebai_hot`）广播，页面自动刷新；页面注入的监听脚本在连接断开（服务端重启）后也会自动刷新页面。**首轮构建窗口期兜底**：`--reload` 启动后 dist 会被 clean-dist 清空、vite 尚需数秒重建，此窗口期 `GET /` 读取不到 `index.html`——服务端不再抛 ENOENT 崩溃，而是返回 503 占位页（「前端构建中」，复用 `/__gebai_hot` 监听构建完成广播自动刷新，另以 3s 定时刷新兜底），构建完成后下次请求即返回真实页面；dev-reload 模式下即使 dist 目录整体暂时缺失，Web UI 路由也保持注册。**HTML 按 mtime 失效**：`GET /` 缓存的 index.html 以文件 mtime 为失效判据——dev-reload 模式每次请求重读；其余模式（含不带 `--reload` 的 `bun run dev`）在 index.html 重建后即重读。若只在启动时读一次并长期缓存，前端重新构建（vite 产出新 hash 资源、clean-dist 删掉旧资源）后会返回引用**已删除资源**的旧 HTML——页面样式与脚本全 404，看起来却像「刚改的代码有 bug」（故缓存必须跟随产物变化，而非只在重启时更新）；二进制内嵌模式无文件、mtime 恒 0，行为等价于缓存不变。**服务重启自动刷新**：页面注入脚本轮询 `/api/health` 的进程启动标识 bootId（`core/base/boot-id.ts`，每进程启动生成一次）——变化即说明服务已重启（且重启时前端产物可能已重建），自动 `location.reload()` 重新加载，免除手工刷新（即「重启服务，前端跟着重启」）；仅本地模式（auth=local）注入，服务模式多用户部署不被服务重启打扰。**重启继承**：`restart_server` 把 `GEBAI_DEV_RELOAD` 作为启动级环境变量传给新进程（`--reload` 是 argv 参数、不会被拉起器复制，故工具侧显式补 `GEBAI_DEV_RELOAD=1`），重启后 vite build --watch 与热刷新通道不丢。

> **首屏加载**：构建产物资源（`/assets` 指纹名、`/vendor` 引擎、`/fonts` 字体）由 `routes/static.ts` 统一托管，按 `Accept-Encoding` 协商 **Brotli（br 优先）/Gzip** 压缩——压缩结果按「路径+size+mtime+编码」在内存缓存（上限 32MB，超限按插入顺序淘汰），同一资源只压一次（vendor 引擎单文件数 MB，一次性 CPU 换长期带宽：plantuml.js 6.8MB → brotli 1.0MB、main.js 0.48MB → brotli 141KB）；woff2/wasm/图片等已压缩或二进制格式与小于 1.4KB 的资源不压（编码与头部开销可能反超收益），响应带 `Vary: Accept-Encoding` 保证中间代理按编码正确分流。**缓存策略**：`/assets/*`（vite 内容 hash）`public, max-age=31536000, immutable` 强缓存，`/vendor/*` 与 `/fonts/*`（稳定名）`public, max-age=86400`，dev-reload 下一律 `no-cache`（重建覆盖同名文件，新页面内容即时可见）；`index.html`/`files.html` **一律 `no-store`**（禁止任何一环存储：它们是入口，引用的是内容 hash 命名的 /assets/*，被存住旧 HTML 就等于引用已删除的旧资源——全 404、页面无样式且脚本不执行；已实测：手机普通模式报障、无痕模式正常，即移动端浏览器/中间缓存不尊重 no-cache 所致）。路径经解析钳制在 `webDist` 内（目录穿越拒绝），未命中前缀时落到 `serveStatic` 兜底（favicon、预览页等根文件）；二进制模式同一策略作用于内嵌资源表。**路径基准**：产物内引用与前端一切请求（静态资源、`/api/*`、`/ws`）均**按页面 URL 相对解析**（构建 `base: "./"`；运行时 `@gebai/sdk` 的 `appBase`/`appPath`/`appWsUrl`），反向代理子路径挂载无需任何配置（见「反向代理支持」）。

> **首屏就绪（初始化与消息渲染）**：首屏时延由「初始化串行链 + 服务端列表查询 + 历史消息渲染」三段构成，各自按下列约定取最短路径：
>
> - **初始化链并行**：互不依赖的启动步骤并行发起（工具卡片元数据与外部身份兑换；会话列表与服务端恢复的当前会话消息），不做无谓串行；会话正文与待办清单也并行拉取。
> - **列表只读元信息**：会话列表经 `store.listSessionInfos()`（`meta.json` 缓存，见「会话管理」）——列表不再为拿标题解析全部会话正文。
> - **消息窗口化（按需渲染）**：消息列按「块」切分（`history-chunk.ts` 的 `planMessageChunks`，块大小按约一屏高度估算、3~12 条并夹取——消息高度差异极大，固定条数会让单块高达数屏），**只保留视口附近的块在 DOM 中**（上下各一屏半余量），其余块整体卸载、高度由 `#messages` 内的 `.vz-spacer` 承担（坐标算术见 `virtual-window.ts`，DOM 编排见 `virtualize.ts`）。首屏只渲染尾部块——长历史不再有「把全部节点建出来」的成本，主线程阻塞与 DOM 规模不随会话长度增长；向上读到哪就渲染哪，内容不丢（导航条由消息清单建立，与渲染进度无关）。
> - **会话加载单次拉取**：`loadMessages` 只调一次 `session.get`——正文渲染与「已配对工具 id」集合（在途工具卡片补渲染的去重依据）共用同一次会话快照，不再为后者二次全量拉取正文。
> - **块划分与滚动锚定**：切分点由 `planMessageChunks`（纯函数，可单测）计算并**向后吞并执行过程容器（`subSessionId`）分组**——同一次 `subsession_run` 的过程消息必须整组落在同一块，否则会被拆成两个折叠容器。滚动时以「视口顶部块 + 块内偏移」为锚点：DOM 变更（新块渲染实测、卸载、spacer 更新）后同帧把 `scrollTop` 修正回锚点位置——向下滚动时卸载的都是刚测过的真实高度（零补偿、零跳动），向上滚动时新挂载块由估高变实测、差值由锚点补齐。未渲染块的高度取已实测块的平均「每消息高度」，并在**加载期首批渲染后定稿**（`settleEstimates`）、滚动期不再重算——布局随滚动反复变动是位置漂移的主因。
> - **阅读位置跨会话恢复**：离开时未贴底则记「块 key + 块内偏移」锚点（绝对 `scrollTop` 在含估高的高度表下不可复现），切回时经 `msgWindow.scrollToAnchor` 落位——目标块按需挂载并实测后再修正一次，一步到位（不再需要等后台补齐完成，也没有「用户接管即放弃恢复」的窗口）。
> - **折叠容器回放**：历史容器不重建在途流引用（`liveRun` 只传给末尾块）——流式累积与 `subSessionArchives` 引用只属于当前运行，更早块的渲染不得覆盖它。

> **构建性能**：图表渲染引擎全部**不参与 vite/rollup 打包**——构建/开发前由 `packages/web/scripts/build-vendor.ts` 原样拷贝到 `public/vendor/`（gitignore），运行时 `diagram.ts` 以**稳定文件名**按需加载：PlantUML 引擎 `@plantuml/core`（上游 TeaVM 编译单文件 `plantuml.js`，实测约 6.8MB，若走打包链路会占 web 构建绝大头，此方式将构建降至秒级）、`viz-global.js`（Graphviz 布局，classic script 注入）、Mermaid 官方 `dist/mermaid.min.js`（约 3.5MB 自包含 UMD，含全部图型）、ECharts 官方 `dist/echarts.min.js`（约 1MB 自包含 UMD，含 SVG 渲染器，SSR 模式直接输出 SVG 字符串）、D2 官方浏览器构建目录（`d2js/`：index.js + worker + wasm，内部相对路径引用）。**稳定文件名（无内容 hash）+ 静态伺服**：开发模式重建后 URL 不变，旧页面引用旧 hash 动态分块导致的 404（「Failed to fetch dynamically imported module」）**从根上消除**（`diagram.ts` 仍保留整页刷新一次兜底，覆盖极端缓存竞态）。产物随 `dist/` 一并分发。 同一机制还负责两类内核：**Monaco Editor**（`monaco/vs/`，约 24MB，编辑器与差异视图的 AMD 构建；`editor.ts` 按需注入 loader）与 **xterm.js**（`xterm/xterm.mjs` + `addon-fit.mjs` / `addon-search.mjs` / `addon-web-links.mjs` + `xterm.css`，终端面板**运行时 ESM 动态 import**（`files/terminal-pty.ts`）——xterm 6 的 UMD 构建靠 `for (var s in exports)` 挂全局而导出不可枚举，全局取不到 `Terminal`，故走 ESM）。

页面清单：

| 页面 | 功能 |
|------|------|
| 登录页 | 用户名/密码（服务模式）、令牌登录态持久化（localStorage） |
| 聊天页 | 会话消息流、流式渲染（**markdown 链接一律新标签页打开**：`target="_blank"` + `rel="noopener noreferrer"`，防止原地跳转打断会话使用；**GFM 任务列表渲染为勾选框**——markdown-it 无内置 task list 支持，`applyTaskLists` 渲染后把列表项行首 `- [ ]`/`- [x]` 的 `[ ]` 占位转为勾选框 span（`li.task-item` 去圆点、`task-box(.done)` 打勾，紧凑/松散列表与有序任务项均覆盖），计划勾选清单不再泄漏字面 `[]`；**markdown 内图表围栏兜底渲染**——正文里的 ` ```mermaid `/` ```plantuml `/` ```d2 `/` ```echarts ` 代码块不经 `show` 工具也渲染为图表卡片（与 `diagram` 内容块共用本地引擎与全屏查看器，源码可在查看器内查看；源码为空或围栏未闭合（流式半成品）的块跳过，闭合后出图）；**流式文本 120ms 尾沿节流渲染**（所有模式统一，markdown 全量重解析是流式期间最重的 CPU 开销，逐 chunk 同步渲染在长回答下 O(n²)））、附件上传、工具调用/审批卡片（**审批卡片为紧凑居中卡片**：限宽 ≤460px 居中显示，**仅展示工具名**（参数已在消息流工具卡片中展示，不重复），工具名单行省略号截断、悬浮 title 可见全名，避免长工具名撑成长条；**键盘快捷键 Y = 通过、N = 拒绝**，作用于最早等待的卡片，带修饰键或长按不触发，按钮内以弱化小字提示快捷键（矩阵主题沿用其 [Y]/[N] 前缀样式）；**工具调用即建卡**——参数先展示、结果到达后再追加到同一卡片（无参数调用仅头部，同样调用即建卡，不在执行完成后一并出现），输出完整不折叠）、压缩通知；**自动标题**：首条消息**发送时点**即以首个用户输入生成会话标题（压缩空白后截 50 字符、超出省略号——侧栏/标题栏按容器宽度自行省略，落盘名只防超长输入整段入库；仅默认标题「新会话」时命名，不覆盖自定义名；发送时点内存首条输入缺失——如页面刷新——回退历史首条用户消息（子会话执行存档不算）；改名失败不标记，任务结束兜底重试）；**输入历史**：输入框 ↑/↓ 键浏览**用户级全局输入历史**（跨会话通用、localStorage `gebai.ui.inputHistory` 持久化，按最后使用去重排序，同一输入只保留一条、限 50 条；空输入时 ↑ 进入、↓ 可恢复原草稿）；**多会话后台运行**：切换会话不中断进行中的回答，流式文本按会话累积，切回时恢复渲染（**推理累积同样恢复**）；**主循环轮界封段**（`stream.ts` text 分支 messageId 变化即封存上一段并清空累积）——后台会话工具事件不渲染卡片（`onToolCall` 开头即早退）从而工具调用处不封段，若无轮界检测，`run.acc` 跨轮只增不减，切回时整任务多轮文本渲染进同一张流式卡片（新回复追加在前面的卡片内，修复前缺陷）；前台另有工具调用封段双保险，重连 `resume` 重置轮界基准（重放不误判）；**输入草稿与附件按会话保存/恢复**（切换保存、切回恢复，新会话/删除会话清理——A 的草稿不会误发到 B，附件上传绑定发起会话 id 不受切换影响）；**选择/环境变量填值卡片与审批卡片同构**（渲染到审批容器，切走隐藏、切回恢复，不随消息重载丢失；任务结束随审批一并清理；**同一请求 id 重复推送（断线重连事件 seq 重放）替换旧卡不堆叠**；show 图表渲染与 page_capture 为页面级操作，后台会话同样执行不回传失败）；**ask 问答记录卡**（调用时中断当前文本段（封段）；等待作答期间消息流不渲染问题预览——交互作答由审批容器选择卡片承载，避免同款选择卡上下重复，结果到达时在消息流落问答记录卡：问题 + 选项展示态 + 「✓ 用户回答 / ✕ 用户拒绝 / ⏱ 选择超时」结果头部与回答文本，模型后续回复另起新气泡；历史重载按参数 + 结果同构渲染记录卡；子会话运行容器内同构渲染）；**ask 计划卡片**（同构于选项询问的延迟落卡：**等待期消息流不渲染计划卡**——计划全文由审批容器选择卡内嵌承载（`event.choice.request` 携带 `plan` 载荷：标题/Markdown 全文/路径，卡内顶部限高滚动展示，审批时直接可见；**带计划的选择卡加 `has-plan` 类跟随内容区宽度**——`.msg` 的 `width:fit-content` 会按最短计划行把卡收缩成窄条，`msg-body` 以 `flex:1` 撑满（同 html-card 先例）；上下两张同款计划卡会被视为重复），批准执行/拒绝执行/自定义修改意见均在选择卡上作答；**结果到达时**消息流落计划卡（标题 + 计划 Markdown 全文，`# 标题` + `- [ ] 步骤` 勾选清单或模型直传 content，双端同构拼装），头部更新为「✓ 计划已批准 / ✕ 计划已拒绝 / ✕ 计划已取消 / ⏱ 计划审批超时」并追加结果文本；历史重载按持久化参数同构渲染计划全文、按结果文本呈现审批结果态）；**工具调用配对按 `会话:toolCallId` 隔离**（toolCallId 跨会话不保证唯一；切回会话时重建运行中卡片的 DOM 引用，后续结果仍追加同一卡片，任务结束清理残留配对）；**滚动位置跨会话记忆**（离开时未粘底则切回恢复原阅读位置，粘底/新会话落底）；**后台流结束不抢焦点**（焦点守卫：仅发起会话仍为当前会话时恢复输入焦点）；**撤回确认期间切换会话不打断当前浏览**（确认后校验会话归属再重载视图/回填输入框）；会话切换失败（网络抖动）回滚当前会话标记并提示，不留下状态不一致；审批卡片绑定来源会话，**仅显示当前会话的待审批卡片**（切走隐藏、切回恢复；后台会话的审批不打断当前会话）；**当前会话有待审批时锁定页面输入**（输入框/发送/附件禁用，焦点落到审批卡片，全部处理完恢复输入焦点），键盘 Y/N 处理；**任务结束（完成/取消/拒绝）时该会话残留审批卡片随任务终止清理**（审批已失效，避免卡片残留锁死输入）；**运行中输入排队**（见「会话输入队列」）：运行中 Enter/发送 = 输入**入本地会话输入队列**（不进消息流，呈现于输入框上方**排队条**——每条含序号/内容预览/附件计数与「⚡立即执行（中断当前任务）/✎编辑（内联）/✕撤回」操作，前端本地状态维护、当前回答结束后自动按序发送下一条），**Ctrl+Enter = 中断插入**（插队首并取消当前循环后立即执行本条），运行中发送按钮随草稿有无切换「排队发送（箭头）/停止（方块）」（空输入点击 = 停止，停止只取消当前任务、队列继续）；**消息撤回**：用户与**助手消息**均有撤回按钮，按**消息 id** 删除该消息及其后续（发送时前端携带 `messageId`、服务端采用同一 id 持久化，撤回对当前会话刚发的消息同样生效；助手最终回复以流式 messageId 落盘（引擎最终轮消息 id 与增量推送一致）——刚完成的回复即可撤回，撤回中途 assistant(toolCalls) 消息连带其工具结果与后续回复一并删除；用户消息撤回后内容回填输入框，助手消息撤回后聚焦输入框便于直接输入指导修正；**任务运行中不可撤回**——前端拦截提示 + 服务端 409 拒绝（运行中任务持有自己的上下文快照并继续追加消息，中途截断会产生交错历史，先停止或等任务完成）；新会话容器内回放消息与本地收尾说明气泡不提供撤回（id 为本地生成、无服务端落点）；撤回同时清理 usage 基线锚点（索引随删除错位，回退估算由下次真实调用重建）；失败 Toast 提示）；**会话内消息导航**：消息流右侧窄导航列（**消息流滚动条自动显隐**——与会话列表同构：沟槽恒定占位（`scrollbar-gutter: stable`，消息宽度不随滚动条显隐伸缩）、静止透明，hover 或滚动中（`.scrolling` 由共用助手 `ui.ts autoHideScrollbar` 维护：scroll 事件加类、停止 400ms 后移除）才显示滑块；导航列右移避让不变；曾整体隐藏滚动条、也曾长期常显），**只为用户输入**建一条短横线（等间距紧凑聚于导航列中部；**静止态 2px 细线**（1px 过细已加粗；125%/150% 缩放下 2/3px 交替肉眼可忽略），hover/focus 加粗至 3px 拉长变亮并浮出消息预览气泡——用户消息省略"我"的称谓标签），点击 / 键盘 / 拖动跳转（**点击与拖动经按下-阈值手势判定（`press-gesture.ts` 纯逻辑工厂）**——按下只记起点，位移超 6px 才进入拖动搓动并接管指针 capture，未拖动松开 = 明确点击，系统取消（pointercancel）只复位不触发点击；短横线 `::before` 扩展命中区收窄至上下 3px/左右 4px——只补足 2px 细线可点性，与相邻步长 18px 不连成竖带。历史缺陷：命中区曾扩至上下 8px/左右 12px，相邻短横线命中区垂直无缝连成右侧**连绵命中带**，而导航列是 `#messages` 的**兄弟元素、不在其滚动链上**——悬停带内滚轮无任何滚动目标（「滚轮失灵」），用户转而点击/拖动立即触发 pointerdown 跳转（偏上位置 = 跳顶）+ pointer capture 劫持后续拖动，形成「锁在底部→一滚就跳顶→滚不动→只能点跳到最新按钮」的偶现锁死链。现三层防护：① 命中区收窄打散连绵带（间隙命中 `#messages` 滚轮原生有效）；② 导航列上的 wheel 转发给消息列（`deltaMode` 行/页单位换算，上滚同步解除粘底跟随——与 `#messages` 自身 wheel 监听同语义，防流式静默窗口把手动落位归因为内部动作拽回底部）；③ 阈值手势消除轻触即跳与 capture 劫持）；**滚动时高亮当前位置**（视口中心所在消息的短横线加长至 20px（普通 16px 与 hover 30px 之间）且加亮，同色无橙色区分；激活区域按「输入开始 → 下一条输入开始」划分，整段输出归属其输入，不按输出中点平分）；**连接韧性**：WS 断开自动指数退避重连（1s→30s，主动登出不重连），连接状态为**纯圆点信号灯**（位于标题栏右端、上下文占比圆环的圆心；**圆点绘制于圆环同一 SVG 内**（`.conn-dot` 第三圆，状态经 `#header-ctx:has(#conn.*)` 桥接、`#conn` 仅作状态载体——原 DOM `::before` 圆点在分数布局原点（44px 头部 - 1px 边框）上光栅化，100% 缩放下相对圆环恒偏半像素，同坐标系绘制后任何 DPI/主题下严格同心））——已连接且无生成任务时**隐藏**（无文字无胶囊），**当前会话运行（思考/流式生成）时显示并快速闪烁**（仅跟随当前会话——多会话后台运行的信号灯不打扰当前视图，切换会话经 `setCurrentSession` 联动刷新）（主题色圆点；流 150s 无数据视为挂起：前端主动中断并清理运行态，防信号灯残留常闪——阈值高于服务端 LLM 读空闲超时 120s，模型调用假死先由服务端超时上报明确错误，前端看门狗只兜底服务端检测不到的挂起；**看门狗中止且无任何输出时渲染「生成超时」说明气泡，不再静默结束**；**交互等待（选择/填值/画图/捕获）刷新活跃时间，等待用户回应的挂起不误判挂起**），断开/异常显示红色圆点常亮（圆心放不下原文字徽章，具体信息并入圆环悬浮——**整个圆环区域均为悬浮区**，断开原因置 tip 首行、上下文数值随其后；`#conn` 铺满圆环仅作状态载体不挂 data-tip，防其自有 tip 遮蔽整环的占比数值悬浮）；**无输出收尾兜底**：任务结束/失败但没有任何可见输出（无文本/推理/工具卡/错误气泡）时补渲染说明气泡（「任务已结束，但没有收到任何回复内容」/错误原因），杜绝「无声无息就结束」；**消息发送走 WS 单通道**（`sendPrompt` 经 WS 发起任务、事件流渲染：文本/推理增量与审批/工具调用/结果统一由 `event.*` 推送驱动；**WS 断开时进行中的流挂起等待重连，重连后按事件 seq 重放离线内容无缝续流**——`resume` chunk 重置当前消息元素防重复渲染；长断线（>150s 无数据）由空闲超时兜底取消任务）；**按钮轮盘**：导出 ⤓ / 压缩 ⤤（会话操作组）与自动审批 ⚡ / 设置 ⚙ / 主题 / 登出（应用操作组）6 个低频操作按钮收敛为**标题栏最右按钮轮盘**（九宫格网格入口，平时仅显示此一个按钮，悬浮展开为扇形）：**双弧扇形**——内弧 r=85 会话操作组（accent 边框组标识、背景与扇形按钮一致——不用 accent 调底，避免与开关开启态 accent-soft 底混淆）、外弧 r=145 应用操作组（按可见按钮数在角度区间内均布，登出隐藏时自动重排），两弧之间 r=115 细弧线分区（标题栏高 44px，扇形只能朝左下方展开，全部按钮不被标题栏遮挡）；纯 hover 交互（不支持点击切换）：入口 hover **立即展开（无延迟）**、**指针在扇形区域内保持展开（保持区=入口按钮∪扇形按钮的边界盒外扩 8px，按可见按钮动态收紧），离开区域 250ms 延迟收起**，外点/Esc/窗口 resize 关闭；展开时扇形按钮自圆心错落弹出（translate+scale+opacity 过渡）；扇形按钮**双背景垫实**（本体与 ::before 各铺一层 `--bg-elev-2`，半透明主题双层叠加 ~95% 不透明、不透明主题本就实心，全主题表面接近实色）+ 文字色边框阴影；**扇形按钮图标统一 `--text`**（accent 图标叠 accent 底相对亮度差被抹平——亮色主题（acrylic 亮/ink/cny）仅 2:1~4:1 不可读，accent 仅作底色/边框组标识，图标全主题 ≥7:1）；**轮盘内开关类按钮（自动审批）开启态同款处理**（accent-soft 底 + accent 边框作"开"标识、图标保持 `--text`——主题 `[data-theme] .icon-btn.active` 的 accent 染色同样会让亮色主题看不清）；**入口按钮静止态无底色、无边框**（仅 `--text` 亮图标，标题栏内靠图标本身可辨——`.icon-btn` 基线静止态无底色（背景 none/边框 transparent/图标 muted）、各主题仅处理 hover/active，故入口按钮以 `#wheel-btn.icon-btn`（1,1,0）兜底压过所有主题只补图标亮色；hover 底色 `--bg-hover`、展开态 `.active` accent 底，图标恒用 `--text`）；6 按钮由隐藏源容器移入（事件绑定在元素上不失效），`logout` 显隐仍由 auth 控制；标题栏保留折叠按钮/会话标题/**上下文圆环 + 轮盘入口构成右侧固定组**（`.header-right` 容器单一 `margin-left:auto` 整体靠右——ctx 与入口各自带 auto margin 会被 flex 平分剩余空间、把 ctx 拉向中线与居中标题重叠；ctx 隐藏时组容器仍占据右端、入口不左移）/**上下文占比圆环**（当前会话 ctxTokens / 模型窗口：24px SVG 圆环弧线（`pathLength=100` 归一化周长，`stroke-dashoffset=100-占比` 免换算）+ **圆心连接信号灯**（见「连接韧性」），悬浮圆环 data-tip 显示精确值与百分比（**整个圆环区域均为悬浮区**——`#header-ctx` 统一挂 tip，圆心信号灯 `#conn` 铺满圆环但不拦截：思考闪烁/断开态悬浮不打折，断开原因置首行），**接口返回缓存字段时追加第二行「缓存命中 tokens（占比）」**（`ctxCachedTokens`：同一次调用的提示词缓存命中，多行 tip 走 `.tip.multiline` 换行展示；接口不返回缓存字段时无此行）；**切换会话即切换**，运行中随 `event.session.ctx` 每轮实时更新；弧线比例分级着色 <50% 主题色 / 50-80% 警告色 / ≥80% 危险色；窗口大小来自快照 `maxContextTokens`，未知时隐藏圆环仅剩圆心信号灯）；**会话搜索框带图标与占位提示**（放大镜 SVG 图标 +「搜索会话」文字提示，聚焦时图标随边框变 accent 色）；**发送按钮为圆角上箭头图标**（线性描边，与停止方块同控显隐）；**新会话懒创建（草稿页）**：点击「＋ 新会话」/登录后无会话/删除全部会话的兜底均只进入**空白草稿页**（不落盘创建会话——避免每次点击产生一个空会话堆积），首条消息发送时才真正创建会话（创建失败保留输入并提示）；草稿态由显式标志维护，迟到的服务端快照不把草稿页覆盖回服务端记忆的当前会话；**空白页快捷按钮**：唯一管理入口在空白页——内置「查看子代理和工具」胶囊（与用户快捷同级样式、不可删，点击填入提示词由模型编排工具作答）居首；用户胶囊为**单按钮样式**（无嵌套边框），点击把内容填入输入框、hover 浮层预览完整内容、右上角显现 ✕ 徽章删除（确认弹窗）；末尾「＋ 添加」虚线胶囊弹窗新增（标题+内容，内容支持多行——Enter 换行、Ctrl+Enter 提交，Esc/遮罩取消）；localStorage `gebai.ui.shortcuts` 仅存本浏览器 |
| 会话列表 | 新建/切换/删除会话（**草稿页跨刷新保持**——进入草稿页即清除 localStorage 记忆的上次会话（`gebai.ui.session`），刷新后保持空白草稿页而非跳回旧会话，首条消息发送时才创建会话的懒创建语义与刷新恢复解耦；**新会话快捷键 Alt+N**——与「＋」按钮等效进入草稿页，已处于草稿页时无操作防误触清草稿，批量模式随按钮禁用；`Alt+N` 而非 `Ctrl+N`：后者是 Chromium 保留命令（浏览器开新窗口，页面收不到按键）），双击会话名重命名，上下文压缩入口（标题栏轮盘 🗜️）；**行内仅保留选中按钮**：每个会话行时间区有一个**勾选框（选中按钮）**——**默认隐藏**，右键菜单「选中」进入多选（批量）模式后常驻显示，点击勾选/取消（多选统一走该按钮）；✎/✕ 操作按钮已移除，**重命名/删除/复制会话 ID 收敛到右键菜单**（屏蔽浏览器默认菜单，随光标定位 + 视口边缘翻转，Esc/点击/滚动/新右键关闭；「复制会话 ID」任意条目可用、Toast 反馈——定位问题/反馈用；批量模式下菜单项变为「选择/取消选择」）；**滚动条占位恒定**：滚动条沟槽恒定保留（`scrollbar-gutter: stable`，静止透明、hover/滑动才显色），会话条目宽度不随 hover 伸缩（覆盖式滚动条环境此前 hover 时条目无端变长 8px、离开缩回）；**批量删除**：右键「选中」进入多选（批量）模式（批量模式下点击行也切换选中），批量模式下操作条显示已选计数，删除所选走确认弹窗逐个删除；全部取消自动退出批量模式；当前会话被删自动切换到剩余第一个或新建；**删除成功后重建列表**；**列表渲染两条约定**：① 行内交互（点击切换 / 双击重命名 / 右键菜单 / 勾选）统一由 `#session-list` **容器级事件委托**分发（`click`/`dblclick`/`contextmenu` 各一个监听器），监听器数量不随会话行数线性增长；② `refreshSessions` 先比对**列表结构签名**（搜索词 + 批量态 + 折叠组 + 各行的 id/名称/置顶/更新时间），签名未变时**跳过整列重建**、仅同步当前会话的 active 高亮——切换会话不再空重建整列 DOM；**按日期分组**：列表按更新时间分组为 今天/昨天/近7天/更早（组内按更新时间倒序），**组头点击折叠/展开**（折叠组不渲染成员，箭头旋转指示，折叠状态 localStorage `gebai.ui.sessionsCollapsed` 记忆；批量模式下强制全展开——折叠分组全部可见可勾选，**组头点击切换全组选中/取消**；搜索时平铺不分组）；**整栏折叠**：桌面端（>860px）标题栏最左按钮折叠/展开整个会话列表（折叠隐藏侧栏、主区占满，状态 `gebai.ui.sidebarCollapsed` 持久化；**Ctrl+B 快捷键等效切换**），窄屏维持滑动抽屉行为 |
| 环境变量页 | 浏览器本地（localStorage）增删改，对本浏览器所有会话生效，随消息临时注入服务端（不落盘防泄露；服务端不配模型变量时仅前端配置即可使用）——并入设置面板 |
| 设置页 | UI 风格（外观性能模式）、`/approval-skip`（标题栏轮盘 ⚡）——设置面板；工具启停、子Agent 装载、Webhook 管理无 UI（设置面板不设对应 tab，经 SDK/API 使用） |
| 待办弹窗 | 标题栏轮盘「待办」按钮打开的可拖动浮层（`todo-pop.ts`）：新增/多行编辑/删除/勾选完成/**拖动排序**/**点击条目填入对话输入框**、**「▶ 执行」与 ⚡ 闲时都新建一条会话跑该待办**（待办全文即提示词）；**只有点弹窗右上角 ✕ 才隐藏**（点轮盘/点窗外/Esc 都不关，打开状态 + 位置跨刷新恢复） |
| 用户管理页（管理员） | 用户创建/禁用/删除（服务模式）——并入设置面板 |
| **文件工作台**（`/files`，独立页面/独立路径） | 目录树 + Monaco 查看/编辑 + IDEA 风格 Git 工具窗（变更/日志/分支/标签/暂存/远程）+ **任意两端差异对比**；会话工作台与文件工作台是**同窗的两个工作台**，两侧各一对按钮、**箭头指方向** —— 会话侧（聊天页标题栏**会话列表按钮右边**）：主按钮 = **并列**（一侧 iframe 对照），悬浮时右方弹出的副按钮 = **全屏**（文件工作台独占整个窗口、会话收起但状态全留；**右箭头**）；文件侧（工作台活动栏最下方）：主按钮 = **关闭文件工作台**（**左箭头**，把窗口还给会话），悬浮时右方弹出的辅助按钮 = **进入分屏**（仅全屏态）。**窗口容不下并列时（手机端为主）会话侧主按钮就是全屏开关**——并列在这个宽度下只会把两侧挤成条，按钮不该承诺做不到的事（判据 `files-split-core.splitFitsWindow`，入口与动作同源）。详见「文件工作台（`/files`）」章节 |

- **启动动画画面**：页面加载（外部 CSS/JS 就绪前）即显示全屏启动动画——深黑底 + 极光呼吸光球 + 旋转光环 + 歌白品牌辉光文字（样式内联于 `index.html`，不依赖外部样式，避免主题样式未就位时空白窗口闪现）；`init()` 完成（含服务模式未登录转登录层）或失败后淡出移除，另以 12s 内联脚本兜底防残留
| 反馈页（管理员） | 反馈查询/导出——并入设置面板 |

> **前端模块结构**：聊天页入口 `main.ts`（初始化与装配）+ 领域模块拆分——`events.ts`（WS 事件处理器）、`stream.ts`（流式消费与渲染）、`turn-view.ts`（回合计时器视图）、`token-rate.ts` + `token-rate-core.ts`（输出速率展示与路况分级）、`attach.ts`（运行中会话附加恢复），其余按既有单职责文件（sessions/messages/tool-cards/diagram/…）。

> **实现**：登录页与设置面板已落地；环境变量/用户/反馈三页统一收敛为设置抽屉（标题栏轮盘 ⚙️），管理员身份自动显示用户/反馈 tab。工具启停、子Agent 装载与 Webhook 管理原设置面板 tab 已移除（对使用者无实际意义：启停/装载由模型经 `agent_load` 等自主完成、Webhook 属服务端集成配置），对应能力保留在服务端 API 与 SDK 方法中。

#### 自研交互组件（零浏览器原生交互）

前端**不依赖浏览器原生交互控件**，全部交互组件基于主题 CSS 变量自绘（`packages/web/src/ui.ts` + `css/overlays.css`）：

- **对话框**：`alert`/`confirm` 全部替换——错误提示走 **Toast**（底部居中浮层，`toast(text, kind)`，容器锚在输入区之上（`--composer-h` 跟随输入区高度，不盖住输入框），`error`/`ok` 两种色点；**`error` 常驻**——报错信息是排查依据，不自动消退，用户点关闭按钮才移除（同文去重、最多 4 条，超出淘汰最旧）；浮层本体不拦指针，只有关闭按钮可点；`ok` 自动消退（至多一条，新提示顶替旧提示）。生命周期核心在 `toast-core.ts`（纯逻辑可单测），DOM 装配在 `ui.ts`；文件工作台的轻提示（`files/ui.ts` 的 `.fw-toast`，右下角）同口径——`error` 常驻可关闭，其余类型到时消退）；确认走**自绘模态框** `confirmDialog({ title, text, okLabel, danger, list })`（返回 Promise<boolean>，Esc/遮罩/取消关闭，复用 `.preview-overlay` + `.confirm-card` 样式——两样式定义在 `css/chat.css`，非 `overlays.css`）
- **Tooltip**：`title` 属性全部替换为 `data-tip` 属性（JS 侧 `tip(el, text)` 助手），`bindTooltips()` 全局委托（pointerover/pointerout/focusin/focusout）渲染单个固定定位浮层——不受容器 overflow 裁剪、视口边缘翻转、长文本省略；**底色取浮层底 `--pop-bg`（未定义该变量的主题回落到 `--bg-inset`）**——它不能跟着内凹区的 `--bg-inset` 走：那个变量本来就是“嵌在面板里的“内凹区”（输入框/代码块/表格头）用的，透出的是**同一块面板**的底色；而 tooltip 浮在**任意内容**之上（底下可能是正文/图片/白底代码块），半透 = 两层字叠着看（实测：亚克力主题下 `--bg-inset` 暗档是 `rgba(0,0,0,0.4)`、亮档只有 `rgba(0,0,0,0.05)`——一眼读不出字）。故 `.tip` 纳入亚克力主题的浮层组（拿 `--pop-bg` + 毛玻璃），并**单独再实一档**（暗 `rgba(18,18,24,0.94)` / 亮 `rgba(252,252,254,0.96)`）——它是瞬时阅读的一行字，背景不可预期，不像弹窗那样背后总有自己的遮罩；低功耗模式（无毛玻璃）随 `--pop-bg` 切成实底；**滚动隐藏收窄**：仅当滚动容器包含悬浮宿主（或页面级滚动）时隐藏（fixed 浮层只会随宿主自身漂移）——此前任意滚动一律隐藏，生成中消息流 sticky-follow 自动滚动等**无关容器**的程序滚动会把标题栏上下文圆环的悬浮刚显示即冲掉（光标未动不重触发 pointerover，提示消失，表现为「信号灯闪烁期间悬浮失效」）
- **浏览器原生右键菜单整站屏蔽**（`packages/web/src/native-menu.ts` 的 `blockNativeContextMenu()`，两个页面入口各调用一次）：原生菜单里能用的（返回 / 重新加载 / 另存为 / 打印 / 检查…）全是浏览器壳的东西，弹出的那一刻界面就从“应用”被打回“网页”；而真正需要右键的地方都有自绘菜单（会话列表 / 资源管理器 / 变更 / 日志 / 比较 / 终端 / 待办），禁掉不损任何功能。实现**只 `preventDefault`、不 `stopPropagation`**——Monaco 与各面板的自绘菜单挂在同一个 `contextmenu` 事件上（各自 preventDefault 后弹自己的浮层），阻断传播会一起失效；监听挂 document 冒泡阶段即可（元素自己的处理器先跑，浏览器默认菜单最后才判）。代价：输入框原生的「剪切 / 复制 / 粘贴」菜单也没了，键盘 `Ctrl+X/C/V` 照常可用。
- **表单控件**：`<select>` 替换为自绘下拉 `customSelect({ options, value, onChange })`（按钮 + 固定定位浮层，Esc/外部点击关闭，暴露 `root`/`value`/`setValue`；**浮层懒创建且挂载到 `document.body`**——部分主题给行加 hover `transform`（`.settings-row` / `#session-list li` 的 `translateX`），transform 祖先会成为 fixed 子元素的 containing block，浮层 `left/top` 会被解释为行内局部坐标而飞出屏外（实测偏移约 820px）；挂 body 后 fixed 始终相对视口，按钮 `getBoundingClientRect()` 视口坐标直接可用；同时**每次打开重建、关闭即移除**，设置面板重渲染不会在 body 堆积隐藏的孤儿浮层）；`datalist` 替换为输入联想浮层 `bindSuggestions(input, items)`（focus/input 过滤展示，点击回填，浮层同样挂 body）；复选框自绘（`appearance: none`，`.ck` 类，勾选 SVG 对勾）；`required` 原生校验气泡关闭（表单 `novalidate` + 提交时自绘校验，失败 Toast 提示）；文件选择沿用隐藏 input + 自定义触发按钮

#### 移动端适配（手机浏览器 / 触屏）

聊天页与文件工作台同一套适配原则，按「能不能用 → 好不好点 → 会不会卡」三层收敛：

**视口与安全区**
- 视口声明 `width=device-width, initial-scale=1.0, viewport-fit=cover, interactive-widget=resizes-content`（两页 HTML 入口一致）——`cover` 使 `env(safe-area-inset-*)` 生效（根变量 `--safe-b/--safe-l/--safe-r`），`resizes-content` 令 Android 软键盘直接收缩布局视口
- 整页高度统一 `100dvh`（保留 `100vh` 回退），地址栏收放跟随；iOS 软键盘只缩 visualViewport 不缩布局视口，故 `bindKeyboardInset()`（`composer.ts`）把「键盘遮住的高度」写进根变量 `--kb-inset`，`#app` 高度按它扣除——键盘弹起时输入区不被盖住
- `html { overscroll-behavior: none }` + 会话区 `contain`：消息区/列表滚到头不把滚动链交给根滚动器（Android 下拉刷新、iOS 橡皮筋）；`text-size-adjust: 100%` 防 iOS 横屏自动放大字号；`touch-callout: none` 关掉长按链接/图片的系统气泡

**布局断点**（自宽到窄叠加）
| 断点 | 行为 |
|------|------|
| >860px | 桌面布局（侧栏常驻、标题绝对居中） |
| ≤860px | 侧栏变**滑动抽屉** + 遮罩（点遮罩/选中会话/新建会话自动收起，拉宽回桌面态自动解除）；标题不再绝对居中（随剩余空间收缩），`≤560px` 隐藏（标题在会话列表里可见）；输入区与头部补左右安全区；`#input` 字号 16px 防 iOS 聚焦缩放 |
| ≤640px | 聊天页左右对比（diff）改上下堆叠；快捷键一览键位列解除固定宽 |
| ≤700px | 文件工作台进入手机档（左栏抽屉、工具窗整屏，见「文件工作台」章末） |
| ≤480px | 字号/间距进一步收紧；待办浮层宽度改跟视口（原 420px 起底会横向溢出） |

> **窄屏抽屉是布局契约，特异性须压过主题**：抽屉的定位与层叠（`position: fixed`、`z-index`、`transform`、宽度）全部由 `base.css` 的 **`#app > aside`** 负责——用 id 前缀把特异性提到 (1,0,1)，压过各主题的 `[data-theme] aside` (0,1,1)；`overlays.css` 不再重复定义抽屉定位。原因是主题常把 `position: relative` 与背景/边框/圆角写在同一规则里（为了让 `z-index` 生效），一旦让它覆盖窄屏定位，`aside` 就留在文档流中——窄屏只有一列，它会独占整行，`main` 被挤到隐式行只剩几十像素（实测东京夜/赛博/浪潮/极光：会话区高 24px、可见消息 0 条；其余主题无此规则故正常）。主题可以改背景/边框/圆角，但不得改定位与层叠（抽屉收起要能完全移出屏幕、展开要在遮罩之上）。

**触屏手势与可达性**
- **无 hover 的补偿（两类，别只做一半）**：
  - **悬浮显形的操作 → 常显**（`@media (hover: none)`）：消息操作组、消息时间、代码块复制、HTML 卡次级工具、图表卡右下角复制/下载（`.diagram-hover-bar`）、空状态快捷按钮的删除叉（`.es-shortcut-del`）、文件工作台入口的**副按钮**（`#files-solo-btn`，鼠标上 hover 才从右缘弹出，触屏下常显否则永远点不到）
  - **悬浮展开的面板 → 点按开合**（粗指针下跳过 hover 时序）：主题面板（`theme.ts`）与标题栏轮盘、工作台动作轮盘（`wheel-core.ts`）——入口点一下展开、再点一下收起。两处都必须改的原因是：触屏上 `pointerenter` 后紧跟同一根手指的 `pointerleave`，会让刚展开的面板在 `CLOSE_DELAY`（250ms）后自己收起（表现为「手机上点了没反应」）；设备能力判定统一走 `state.ts` 的 `isCoarsePointer()`，细指针下行为完全不变
  - `data-tip` 提示走 `pointerover`——触摸同样会产生该事件，故不另补长按路径（真机手感未验）
- **长按 = 右键**：会话列表行 480ms 长按弹出同一个右键菜单（重命名/删除/多选）——iOS/Android 都不保证长按触发 `contextmenu`，而原生菜单已被整站屏蔽；长按后抑制随后的 click（否则松手还会把会话切过去）
- **拖拽类的触屏路径**：消息小地图短横线加 `touch-action: none`（否则纵拖先被当成滚页而 `pointercancel`）；待办拖动排序是 HTML5 drag（触屏不产生该手势），触屏下改用行内上移/下移按钮（`@media (pointer: coarse)` 显示）；卡片/分隔条等拖拽统一走 Pointer Events + `setPointerCapture`
- **命中区**：粗指针下关键操作补到 32~44px（`.msg-act` 34、`.queue-act` 40、`.todo-act` 36、`.todo-check` 22、轮盘扇形按钮 44、弹窗工具按钮加内边距）——视觉尺寸基本不变，只扩可点区域

**性能**
- 低性能模式（`low-power.ts`）在用户未显式选择时取设备信号默认值：系统「减少动态效果」/ 省流模式（`connection.saveData`）/ 低内存设备（`deviceMemory ≤ 4`）→ 默认开启（关掉毛玻璃与粒子等重特效）；用户显式开关（设置面板「外观」）落盘 `on`/`off` 两种值，两种都压过设备默认值；系统「减少动态效果」变化实时跟随



#### 多套风格 UI

前端支持**多套 UI 风格**，同一套前端代码、同一份业务逻辑，运行时按需加载不同风格：
- **内置风格**：内置多套风格，随二进制分发，切换无需重新部署：

  | 风格 | 特征 |
  |------|------|
  | `acrylic` | 亚克力（默认）：黑色半透明面板 + 毛玻璃（backdrop blur）、近黑底极弱光晕，**会话元素（列表条目/搜索框等）零边框**，纯靠背景层次与圆角区分；**低功耗模式（全站关闭毛玻璃）下浮层（主题面板、设置抽屉、预览、待办、下拉、确认框、右键菜单）改为实底**——该模式下无模糊兜底、半透明浮层会透出背后正文，实底同时也更省；支持**黑白（暗/亮）切换**；不配环境特效（保持毛玻璃原味） |
  | `aether` | 以太（旗舰）：光之玻璃、青紫粉渐变辉光；特效**棱镜流光**（光透玻璃的色散光域旋转流动、加法混光） |
  | `matrix` | 矩阵：纯黑底荧光绿、等宽字体、终端质感；特效**数字雨**（逐列下落字符流，渐隐拖尾） |
  | `tokyo-night` | 东京夜：程序员配色、紫/青/粉点缀、柔和霓虹；特效**霓虹车流**（远中近三层车速的横向流光车灯拖尾 + 偶发流星） |
  | `cyberpunk` | 赛博：霓虹粉/青光晕、彩色辉光、赛博朋克；特效**霓虹雨**（青粉雨丝缓落微斜 + 偶发故障闪线） |
  | `synthwave` | 浪潮：复古网格线背景、橙粉紫霓虹日落；特效**地平线光柱**（自底部网格升起的等律动光柱 + 流星划过上半天幕） |
  | `aurora` | 极光：深空底 + 青绿紫三色渐变、大圆角玻璃面板；特效**极光帷幕**（数道垂落波动光幕、亮度波沿幕面行进） |
  | `ink` | 水墨（亮色）：宣纸暖白底 + 墨色正文 + 朱砂印章红点缀、黛青辅色、纸纹质感、**墨块代码**（代码块反白为墨底纸字）、**胶囊悬浮输入栏**（无外框，宣纸底胶囊 + 墨晕浮起投影，聚焦加深投影带朱砂微光）；特效**墨韵**（墨雾漂移 + 墨滴晕开涟漪） |
  | `cny` | 人民币：中国红丝绒底 + 鎏金点缀、面额色渐变气泡、纸钞防伪纹细节；**招财猫玩偶**（悬浮可拖动 + 爆金币特效，见「人民币招财猫」） |
  | `qinhan` | 秦汉彩绘：玄漆黑底（秦尚黑）+ 朱红彩绘（汉尚赤）+ 鎏金勾边（配色取自秦兵马俑彩绘与汉漆器，辅色石青/石绿/赭石/汉紫矿物颜料色），**小圆角方正庄重**（皇家规制）；纹饰——**云兽纹整砖底饰**（勾连卷云 + **青龙走龙**（S 形蟠体/上下吻双角/背鳍四足/卷云尾）线描暗纹铺底，朱龙金云）+ **标题栏锯齿纹带 + 回纹边带**（漆篋/青铜器边饰语汇，两缘渐隐）+ **顶部鎏金/朱砂双钩线** + **漆器双线面板**（鎏金外线 + 朱砂内线复合描边）+ 消息气泡**四角金角花**（函套角）、宋体碑刻标题/表头/榜书匾额；**空状态瓦当环纹徽章**（连珠纹边轮 + 同心轮界 520px 大环，标语居环芯，鎏金线描）+ 菱纹金分隔线；特效**云气祥纹**（卷云线描缓移缓旋，鎏金/朱砂/汉紫三色 + 鎏金微尘上浮） |

- **主题面板排序与交互**：默认主题（`acrylic`）**不归属任何分组**，独立显示于列表顶部（无分组标题、无说明）；其余按风格族分组展示，组内保持固定顺序——科技风（`matrix`/`tokyo-night`/`cyberpunk`/`synthwave`）→ 氛围风（`aether`/`aurora`/`ink`）→ 特色（`cny`/`qinhan`）；主题按钮悬浮 120ms 自动展开，指针离开入口/面板 250ms 延迟收起，外点/Esc/窗口 resize 关闭；选中主题后立即切换并关闭面板

- **风格解析优先级**（高 → 低）：
  1. URL 参数 `?gb_style=<id>`（宿主 / iframe 注入，业务系统嵌入指定风格；**仅主界面 `/` 生效**——文件工作台不读 URL 主题参数，见下条）
  2. 用户级：界面主题面板切换，持久化 `localStorage["gebai.ui.style"]`
  3. 全局：环境变量 `GEBAI_UI_STYLE`，由服务端渲染 `/` 时注入 `window.__GEBAI_UI_STYLE__`
  4. 默认 `acrylic`
- **两个入口共享主题（本地存储唯一权威）**：主界面 `/` 与文件工作台 `/files` 是同一份用户级偏好的两个文档，主题一律取 `localStorage`——工作台以 `initTheme({ urlPrefs: false })` 忽略 URL 上的 `gb_style`/`gb_cny`/`gb_acrylic_lt`/`gb_vars`，入口生成的 URL（新标签 / 分屏 iframe）也不带主题参数，否则一个带旧参数的链接就会把两页拆成两套配色（地址栏同步会顺手清掉历史残留的 `gb_style`）。**跨标签页实时同步**：两个入口都监听 `storage` 事件（`gebai.ui.style` / `gebai.ui.cnyScheme` / `gebai.ui.acrylicLt`），任一标签改主题，另一个已打开的标签（主界面或工作台）即时跟随，且只重放真正变化的项（同值不触发切换动画与组件重绘）。分屏 iframe 另有 `postMessage` 桥接供宿主侧即时推送（见「分屏模式」）。
- **按宿主默认**：桌面端/浏览器/服务端部署可各自通过 `GEBAI_UI_STYLE` 配置全局默认风格
- **主题变量化**：UI 样式基于 CSS 变量/设计令牌（颜色、圆角、阴影、字体、布局令牌）实现，风格间差异仅体现在令牌与组件装饰层，核心组件与交互逻辑完全复用
- **轻量热切换**：非默认风格为独立 CSS 文件（`src/themes/*.css`），运行时动态 `<link>` 按需加载，切换无需刷新；未选中风格不加载其资源；加载失败静默回退默认令牌
- **自定义风格**：支持外部注入 CSS 变量覆盖，URL 参数 `?gb_vars=--accent:%236366f1,--radius-md:8px`（逗号分隔 `--变量:值`）写入根元素内联样式，满足品牌化嵌入需求
- **人民币面额配色**：人民币主题（`cny`）内置 6 种面额配色（100 红 / 50 绿 / 20 棕 / 10 蓝黑 / 5 紫 / 1 橄榄绿），通过根元素 `data-cny-scheme` 属性切换 `cny.css` 中的配色变量块；解析优先级：会话手动选择 → URL 参数 `?gb_cny=<id>`（宿主注入） → 用户级持久化 `localStorage["gebai.ui.cnyScheme"]` → 默认 100 元红；主题面板在人民币主题激活时显示「人民币配色」分组（其余主题隐藏）。**面额配色即强调色**：每个面额完整定义 `--accent` 三件套
- **默认主题黑白切换**：默认主题（`acrylic`）支持黑白（暗/亮）两档切换，根元素 `data-acrylic-lt` 切换 `acrylic.css` 中的白色亚克力变量块（白色半透明面板 + 浅色光晕 + 深色文字全套变量重定义）；解析优先级：会话手动选择 → URL 参数 `?gb_acrylic_lt=<id>`（宿主注入） → 用户级持久化 `localStorage["gebai.ui.acrylicLt"]` → 默认暗色；主题面板中为**独立一行色块、不归属任何分组**（仅默认主题激活时显示）；**亮色档的浮层与遮罩配套调整**：遮罩是弹窗毛玻璃的采样底，深色遮罩在浅底上会把白色浮层拖成灰（故亮色档 `.preview-overlay/.settings-overlay/.fw-overlay` 改用浅遮罩，浮层底色 `--pop-bg` 相应比面板实）
- **主题高级特效**：各主题一套**形态互异**的画布环境特效（`theme-fx.ts`，全屏 canvas `#gb-theme-fx` 固定全屏、`z-index:-1`、pointer-events:none——位于背景装饰之上、全部内容之下；主题 CSS 将 `main` 背景透明化透出特效，气泡/卡片自身背景保证正文可读）：matrix 数字雨（逐列字符流、destination-out 渐隐拖尾、帧率无关、整体压暗）、tokyo-night 霓虹车流（远中近三层车速的横向流光车灯拖尾穿行 + 偶发流星）、cyberpunk 霓虹雨（青粉雨丝缓落微斜 + 偶发故障闪线）、synthwave 地平线光柱（自底部网格升起的等律动光柱 + 流星）、aether 棱镜流光（光透玻璃的色散光域旋转流动、加法混光）、aurora 极光帷幕（数道垂落波动光幕、亮度波沿幕面行进）、ink 墨韵（宣纸上墨雾漂移 + 墨滴晕开涟漪，亮色画布暗墨低透明度）、qinhan 云气祥纹（卷云线描——递减半圆链螺旋 + 贝塞尔云尾，鎏金/朱砂/汉紫三色（汉紫为兵马俑彩绘矿物颜料稀有衬色），缓移缓旋、透明度呼吸 + 鎏金微尘上浮）；acrylic 不配环境特效（保持默认主题毛玻璃原味），cny 招财猫 + 爆金币为独立交互层。监听 `gebai:theme-change` 随主题启停、`gebai:low-power-change` 跟随低功耗开关；粒子量随视口面积缩放（1440×900 基准 0.55~1.6 钳制）；低性能模式下整体停用（画布移除）。**输入降载**：环境特效每帧绘制与输入处理争抢同一份帧预算，故文本输入元素（`input` 文本类型 / `textarea` / contenteditable，经 document 级 `focusin`/`focusout`/`input` 捕获委托识别，覆盖输入栏、设置项、待办与排队编辑、会话重命名等全部输入点）聚焦期间绘制频率降到 15fps、打字中（最后一次 `input` 起 500ms 内）再降到 12fps、失焦立即恢复满帧；跳过的帧不绘制只继续排队，动画按累积间隔推进（恢复不跳帧），非文本控件（勾选/滑杆/按钮等）与 acrylic、低功耗态不参与。**画布分辨率上限 `FX_MAX_DPR`（相对 CSS 像素）**：全屏读写的代价随该值平方增长（高 dpr 屏按 2x 铺满画布时，仅矩阵的逐帧全屏渐隐就要读写 24MB/帧，实测主线程成本约为 1x 的 4 倍，是「会话运行时周期性掉帧」的主因），而特效均为大面积柔光/半透明字符，取 1x（清晰度优先可一行调回 2）。**矩阵全屏渐隐降频 `FX_FADE_FPS`**：渐隐量按累计 dt 计算，量化到 ≤30fps 视觉等效（拖尾由指数衰减给出），而全屏半透明读改写是该特效最贵的一步。**会话运行中降频**：降载档位可叠加、取更慢者（`targetGap`）——输入档之外新增「会话运行档」（`FX_BUSY_FPS`：当前会话有任务在跑时 24fps，流式渲染的 markdown 全量重解析优先；信号源 `data-fx-busy` 由 `state.syncConnThinking` 同步，与信号灯闪烁同一真相源；后台会话运行不影响当前视图）。降载深度以「绘制与输入争抢主线程」为限——慢机（CPU 20× 节流）实测静置主线程占用：满帧约六成、聚焦档约两成、打字档约一成七（故不做「打字时完全停绘」：停绘让人以为特效坏掉，而成本收益已很小）；档位的目标间隔必须小于累积上限 `FX_ACC_MAX`（0.1s），否则累积永远追不上、该档位静默变成完全不绘制（单测守住这条不变量）。**特效面板形态**（设置面板「外观」tab，本机偏好 `localStorage["gebai.ui.fxPanels"]`，根元素 `data-fx-panels="matte"`）：特效主题把 `main` 背景透明化以透出画布，大面积常驻面板（标题栏/侧栏/设置面板/输入行，约占视口 1/4）隔着**持续变化**的画布做 `backdrop-filter`——模糊结果无法缓存、每帧全量重光栅化，慢机上表现为整体掉帧（与特效档次无关，是面板模糊自身的成本）。默认「毛玻璃」（观感优先）；可切实底：大面板去模糊、底色加实（`color-mix(in srgb, var(--bg-elev) 88%, transparent)`），换回满帧（小面积浮层仍保留原有玻璃）。
- **人民币招财猫**：人民币主题（`cny`）专属**悬浮玩偶 + 爆金币特效**（`cny-cat.ts` 玩偶交互 + `coin-fx.ts` 全屏 canvas 粒子引擎），仅 `data-theme=cny` 时挂载（监听 `gebai:theme-change` 同步增删）：
  - **玩偶**：内联 SVG 招财猫（挥手手臂独立分组做 CSS 摆动动画），悬浮可拖动（pointer capture，视口钳制），位置持久化 `localStorage["gebai.ui.cnyCat"]`；核心定位样式内联（主题 CSS 卸载时序不影响），装饰动效（挥手/悬浮/弹跳）在 `cny.css`
  - **拖动金币/纸币轨迹**：拖动期间沿指针位置混撒小尺寸金币与纸币（**按累计位移触发**——每挪动 18px 撒一枚小金币（半径 3-4.5px）+ 半数概率一张小纸币（宽 22-30px），取余保留余量、单事件至多一枚防同点堆叠；不按时间节流——拖快拖慢撒币密度恒定，快速拖动不会稀稀拉拉），**原地散开**消散——全向低速初速 + 零净加速度 + 强空气阻力迅速衰减到停，停留原位轻转渐隐（不上升不坠落、不与地面交互；粒子引擎按粒子覆盖重力/阻力/地面交互/横摆幅度，区别于爆发的坠落反弹物理）；点击与拖动按累计位移阈值（6px）判定
  - **点击爆发 + 连击递增**：点击爆少量金币，900ms 内连续点击连击 +1、每次爆得更多（10 起步、每击 +7、封顶 58，初速随连击小幅提升）；错误/取消不触发
  - **单轮完成大爆发**：任务正常完成（`done` 分片）时从猫位置大量爆发，**运行越久爆得越多**（36 + 每秒 1.4，封顶 230，分 2-4 波间隔 110ms 发射）；与单轮计时器共用同一起算点（`RunState.startedAt`）
  - **粒子渲染**：金币与各面额纸币约各半——金币 3D 翻转（椭圆面压缩 + 近侧视边缘厚度、鎏金径向渐变、内环 + ¥ 浮雕、落地反弹两次后静止消隐）；纸币取第五套人民币六面额配色（与面额配色同源 `CNY_SCHEMES`），飘落摇摆（终端速度 + 正弦横摆 + 绕横轴翻面的纵向压缩），版面细节（内框双线/水印圆/人像剪影/面额与角标数字）；**预渲染精灵**（金币 32 相位帧 × 各半径、纸币各面额版面，离屏 canvas 2x 超采样）逐帧只做 transform + drawImage；画布懒创建（定位样式内联）、粒子耗尽自动移除、上限 420 粒子、rAF 仅在有粒子时运行
  - **降级**：低性能模式不发射粒子（玩偶保留、挥手动画由全局低功耗规则停用）
- **低性能模式**：用户开关（设置面板「外观」tab 单行按钮：开启/关闭，说明一行显示当前生效状态）+ 设备信号默认值——不影响配色与布局、仅降级特效与渲染开销：
  - **设置解析**（`low-power.ts`）：`localStorage["gebai.ui.lowPower"]` 存用户显式选择（`"on"`/`"off"` 两种都落盘——`"off"` 要能压过设备默认值）；未显式选择时（不存，或旧三态 `auto` 等其它值一律忽略）取**设备信号默认值**：系统「减少动态效果」（`prefers-reduced-motion: reduce`）、省流模式（`connection.saveData`）、低内存设备（`deviceMemory ≤ 4` GiB）任一命中即默认开启；跨标签页 storage 事件同步，系统「减少动态效果」变化实时跟随（未显式选择时）
  - **生效标记**：根元素 `data-low-power="on"`，CSS 全局关闭动画/过渡（主题呼吸、脉冲、入场动画等）、View Transitions 与扫描光切换动画、`backdrop-filter` 毛玻璃（无 GPU 时持续重绘最卡）；**状态指示动画豁免**：流式输出光标（`caret-blink`）与连接信号灯闪烁（`conn-thinking`）是任务进行中的必要指示且开销极小，低性能模式下保留
  - **消息区窗口化（全模式生效）**：`#messages` 的直接子节点是 `[.vz-spacer.top] [挂载中的块 `.vz-block` …] [.vz-spacer.bottom] [尾部活动区节点…]`——只有视口附近的块留在 DOM，远离视口的块整体卸载（块容器引用常驻，重挂零重建），高度由 spacer 按「实测记忆 + 定稿估高」承担；折叠 k 个块为 1 个 spacer 时补回少掉的 k-1 份间距，故折叠态与全渲染态总高度一致（滚动条长度不漂移）。**DOM 规模恒定**（约视口 ×3），布局/样式/绘制/流式 MutationObserver 开销不再随会话长度增长。滚动位置由「贴底保持 + **元素级滚动锚定**」保证（贴底时不因内容增减离开底部，阅读中保持视口内容不动）：自管一套代替被关闭的 `overflow-anchor` 的锚定——维护一条基线（**视口顶部所在元素**的屏幕位置 + 当时的 scrollTop，元素从容器直接子节点**逐层下钻到跨过视口顶部的后代**，故图片/图表/懒渲染导致的**块内长高**同样可察觉；块级锚点察觉不到），任何布局变更后用「实际位移 − 用户滚动量」得出纯布局位移补回 scrollTop——**只补增量、不写绝对位置**（绝对写回会把「渲染超大卡片期间用户继续滚动」的那段滚动抹掉，表现为跳）。**挂载/卸载/落 spacer 的顺序**：先挂载 → 再落 spacer → 最后卸载（先摘 DOM 会让布局瞬时变短，浏览器把 scrollTop 钳到临时上限、每次回退一个滚轮步长，并让挂载区间来回翻转、同一块反复挂载/卸载）。**挂载位置按槽位顺序插入**（插到下一个已挂载的更大序号块之前）——向上扩窗时新块序号更小，只往尾 spacer 前插会落到已挂载块之后，块自身空间与上方 spacer 重复计算。**窗口收缩滞回**（`RANGE_HYSTERESIS`）：丢块要超出更紧的边距才生效，否则视口恰好停在边界上时 spacer 的间隙差就会让区间反复翻转。**贴底跟随由三态意图识别决定**（`scroll-intent.ts`：follow 追最新 / hold 停在此处 / reading 阅读历史；`sticky-follow.ts` 只做 DOM 绑定）——只看用户**真实造成的位移量**，不看事件来源：判定全在 scroll 位移上进行（滚轮/触屏/键盘/中键自动滚动/查找定位一律经位移体现，无来源漏判；位置没动就没有位移，橡皮筋回弹与惯性过冲自然豁免）。**位置离开底部阈值且本手势有向上位移**（用户确实滚开）→ 确认 60ms 后解除（期间回底则取消；用户停手无后续事件时靠定时器到点复查）；**向下滚到底（距底 ≤ `resumePx` 4px）且本手势有向下位移** → 恢复跟随——单纯「位置落在阈值内」（内容收缩、程序落位）不恢复，避免把视口从阅读位置拽回底部；**恢复不沿用容忍区阈值**：触摸板/触屏能精确停在阈值内，若在那里就恢复，用户只想往回调一点继续读却被锁定、后续内容增长把视口钉到底部（所以「到底」才是「想追最新」的自然凭据，滚轮会 clamp 到底、对此无感）；hold 与 reading 按距底距离归属（1.5 屏内 / 更远）。输入事件不改状态，只用于**短期抑制程序回正**（上翻输入 120ms 宽限内、或用户本手势刚上翻且仍在手势窗内时不 pin）——滚动是合成器异步派发的，输入与位置变化之间内容增长的 rAF 可能先行；该抑制**必须随手势窗结束而失效**（否则一次微调会让后续内容增长永久不再贴底，状态是「追最新」却名不副实）。窗口化据此取 `isFollowing`（未注入时退回几何判定）——不看几何位置：运行中会话内容持续增长会让「距底 ≤ 阈值」反复不成立，据此判定会在用户追最新时走锚点分支、把人留在旧位置（永远差一截），而在阈值区内又把人拽到最底（停不住）；贴底赋值后额外通知跟随核心接手后续对齐。**尾部活动区不入坐标表**（新消息/在途流/工具卡追加在块表之外、高度是真实 DOM），故视口顶部越过末块末尾时改按「距底距离」保持——块坐标 `locate` 会把越界坐标钳到末块末尾，照旧回写会把视口反复拉回该处（表现：接近底部滚不动、到不了最底部，差距恰为尾部内容高度）。滚动记忆同口径：跟随时记「下次落底」而非阅读位置。**滚动诊断探针**（`scroll-probe.ts`，按需开启默认关闭）：滚动体感类缺陷只能在实际出问题的机器上复现，探针把现场取证做成产品能力——开启后记录消息列每次 `scrollTop` 写入（含调用栈）、周期性快照（位置/高度/距底/spacer 高度/挂载块数/跟随状态/视口中心命中）与滚动事件，控制台 `__gbScrollProbe()` 打印并复制报告；开启方式 URL 参数 `?gb_scroll_probe=1`（临时）或 localStorage `gebai.ui.scrollProbe="on"`（持久），记录上限 1500 条环形丢弃。不再依赖浏览器估高缓存——CSS 的 `content-visibility: auto` + `contain-intrinsic-size` 布局隔离已随窗口化移除（它是旧内核丢弃 `auto` 关键字声明时视口外消息高度塔为 0、`scrollHeight` 严重低估、滚动被钳制跳动的根源；窗口化后 DOM 恒定，隔离亦无收益）。**尾部活动区**（新消息 / 在途流 / 工具卡 / 模型异常提示）恒定挂载在末尾、高度由 DOM 真实承担，在途运行态引用（`run.el` / `pendingTools` / 子会话容器）不掉线。低性能模式与标准模式共用同一布局行为。主消息区 `#messages`、子会话容器 `.subsession-body` 与推理体 `.reasoning-body` 关闭浏览器滚动锚定（`overflow-anchor: none`，全模式生效）——锚定算法在生成中底部内容持续增长/内容收缩场景会选中失效锚点把视口猛拉到列表顶部（跳回第一输入）；且锚定调整自行产生滚动事件、粘底跟随核心（sticky-follow，见「聊天页」）无法将其归因为内部动作，内容收缩（推理折叠/容器折叠）时会把贴底视口拽离底部。粘底跟随由应用自管，底部追加不影响已上翻的阅读位置。**消息子项禁止 flex 收缩**（`#messages > * { flex-shrink: 0 }`）：`#messages` 是 flex 列滚动容器，flex 子项的自动最小高度（`min-height: auto`）仅在自身 overflow 可见时等于内容高度，`overflow ≠ visible` 的子项（subsession_run 执行容器 `.session-run` 圆角裁剪需 `overflow: hidden`）自动最小高度为 **0**——会话内容超过一屏后 flex 负空间全部压到这类子项上，容器被压缩到近 0 高（实测 1px）、正文整段被裁剪不可见且 `scrollHeight` 塌缩至视口（滚动条近乎满比例、长内容滚不到，即「生成过程中滚动卡死、页面内容很长但滚动条显示很大比例」；历史回放渲染为折叠态不触发，故刷新后现象消失——执行容器只在执行中展开）；`flex-shrink: 0` 令所有子项保持内容高度，溢出全部进入滚动区。附注：会话列表条目正文（`#session-list .session-body`）与子会话容器正文（`.subsession-run .subsession-body`）曾共用未限定作用域的同名类 `.session-body`，后者内部消息对齐（`.msg.user align-self: flex-end`）实际依赖前者的全局 flex 规则意外供给——已将列表侧规则收窄到 `#session-list`、容器侧在 `chat.css` 显式声明 `display: flex; flex-direction: column`（同名类职责分离，消除跨模块样式耦合）
  - **流式输出降频**：文本 delta 渲染按 120ms 尾沿节流合并（`scheduleStreamRender`，计时器挂 `RunState.renderTimer`，封段/重置/结束时随 run 清理；**封段前冲刷最后一帧**——定时器待触发即存在未上屏增量，只清零不冲刷会把最后一个节流窗口（120ms）内的文本永久丢失，快速模型整轮回复可全部落在一个窗口内、气泡残留空白（subsession_run 执行过程「只见结果不见过程」的主因））
  - **流式渲梁增量渲染**（`stream-render.ts`）：markdown 解析是流式期间最重的 CPU 开销，逐帧重解析累积全文在长回答下是 O(n²)（实测：全量模式下 3.5KB 正文已出现 12 个 longtask / 最大 679ms、帧率降到 30fps）——按**块边界**把文本切成「已稳定的前缀」与「仍在增长的尾部」：前缀渲染一次即常驻复用，每次只重渲染尾部（尾部长度与「最后一块」同量级、不随回答变长）。结构保持与全量渲染一致（容器内单层 `.markdown`，前缀块与尾部块都是它的直接子节点，尾部块每帧整体替换），片段挂进最终位置后边距由相邻兄弟规则决定。**切点安全**：围栏代码块未闭合、`<think>` 未闭合期间不切（片段会解析不出代码块 / 抽不出推理卡片）；空行两侧同类且可跨空行延续（松散列表 / 多段引用 / 缩进块）时不切（否则裂成两块，编号列表会重新计数）；`html: false` 故无 HTML 块跨空行的歧义。含 `<think>` 的文本与文本回退（重连重放）走全量渲染（推理卡片需整体抽取）；参考式链接定义跨切点会失效（流式半成品可接受，封段后以全量文本渲染为准）。渲染器按容器（气泡内 `.msg-text`）分配（WeakMap）：共享实例会在容器间反复重建宿主而退化为全量。标准模式不再逐 chunk 同步渲染
  - **代码高亮降级**：无语言标注的代码块跳过 `highlightAuto` 自动检测（穷举全部语言最贵），仅转义；显式语言标注仍正常高亮
  - **图表始终默认渲染**：缩略图自动渲染（不随低性能模式改为按需，避免「图不默认渲染」困惑；渲染失败只显示占位提示、错误细节不暴露到主页面，进查看器/控制台）；本地渲染引擎**空闲预热**（`load` 后经 `requestIdleCallback`（缺省回退 800ms 定时）调度，避开首屏；**仅预热本机实际用过的引擎**——痕迹由 `markEngineUsed` 记录，无图表使用史的会话首屏不下载任何引擎；D2 的 WASM 开销大且架构图频率低不预热）；**低性能模式跳过预热**，首次渲染由消息流触发；PNG 导出超采样 3x → 1.5x、复制图片 2x → 1x（`isLowPower()` 实时读取），渲染与 show 图表分支链路不变

- **单轮计时器**：任务（单轮对话）耗时显示于**标题栏右侧、上下文占比左侧**（`.turn-timer`，细线沙漏图标 + 时长文本，常驻单一位置；时长文本为**冒号分隔、不带单位后缀**——`分:秒`（如 `0:07`），≥1h 为 `时:分:秒`（如 `1:05:03`））：运行中实时走（250ms 刷新、图标呼吸动效），**运行结束不清除**（定格保留显示），**下次运行重启归零**；**随会话视图切换**——计时显示归属当前会话：运行中切走不打扰（tick 空转）、切回续走（`gebai:session-view` 事件联动刷新），非运行会话显示该会话最后定格时长、无记录隐藏；后台会话的启动/结束不抢占当前视图。**六级时长渐进变色**（`data-dur` 属性，冷→暖，阈值 10s/30s/1m/3m/5m）：<10s 极淡（`--text-faint`）、10-30s 中性（`--text-muted`）、30s-1m 工具青绿（`--tool`）、1-3m 主题色（`--accent`）、3-5m 警告（`--warning`）、≥5m 危险（`--danger`），与上下文圆环的分级着色语言一致。**信号灯思考闪烁周期同步六级放缓**：运行中把同一 `data-dur` 同步到 `#header-ctx`，`--conn-blink` 变量按级取 0.07s → 0.11s → 0.16s → 0.25s → 0.4s → 0.6s（**越久越慢——闪得慢=执行得慢**：刚开始急促闪烁表活跃，拖长后逐渐放缓表执行缓慢；SVG 圆点 `.conn-dot` 经继承解析；运行结束移除属性恢复默认 0.25s；低性能模式豁免保留闪烁且保留分级周期；`no-ring` 无数据模式仅隐藏轨道/弧线、圆点仍可显示）。**hover 显示总运行时**（`data-tip`）：每轮净耗时累加（含进行中这轮的已耗时，不含轮间空闲），而非距第一条消息的墙钟时间；累计**按会话记账**（页面生命周期内，刷新后归零）。默认开启；设置面板「外观」tab 单行按钮可关（`localStorage["gebai.ui.turnTimer"]` 仅存 `"off"`，根元素 `data-turn-timer="off"` 时 CSS 隐藏，跨标签页 storage 事件同步，运行中被关闭即时停表隐藏）。纯展示层能力：不持久化到会话记录（`Message` 无耗时字段）

- **输出速率（tok/s）**：模型输出速度显示于**标题栏单轮计时器右侧**（`.token-rate`，仪表盘图标 + 纯数值如 `42.3`，**单位与口径标记都不上屏**——避免标题栏噪声，单位与口径在 hover 提示里给出），便于观测模型服务性能。**数据源唯一在服务端**（`event.session.tps`）：服务端在一次模型调用内**按 1s 节流周期持续上报**（`est: true, active: true`，服务端按 CJK 感知字符折算估算——与 usage 真值同量级），调用结束时推**收尾帧**（`active: false`；接口返回 usage 时 `est: false` 即实测口径，不返回时仍为估算收尾）——长回答全长期间数字都在刷新，不是只有在调用结束时才有一个数；前端**只展示不自算**（两套口径并存会让数字来回跳，也说不清看到的是谁的量）。速率 = 输出 tokens ÷ **生成窗口**（首个输出 chunk 到末个输出 chunk，不含首 token 等待——量的是解码速度而非接口响应延迟）；生成中帧要求窗口 ≥300ms 且距上帧 ≥1s，收尾帧要求窗口 ≥50ms（整段一次性返回的非流式产出测不出解码速度，不报）。**仪表常驻**：两帧之间（工具执行 / 审批等待 / 思考停顿）**保持上一帧的值与配色**，不闪没；速率按会话记账，随会话视图切换显示对应会话的帧，无该会话帧时隐藏。**按导航交通拥挤配色分级着速**（`data-rate` 六级，越快越绿）：≥80 畅通（`--success`）、50-80 顺畅、30-50 良好（前三级与后三级间的过渡色由主题 `--success`/`--warning` 经 `color-mix` 派生，随主题自适应）、18-30 缓行（`--warning`）、8-18 拥堵、<8 严重拥堵（`--danger`）；估算帧整体降不透明度（真值帧满不透明度）。hover 提示为**两行固定结构**（信息与噪声分离）：速率行 `42.3 tok/s · 良好（实测）`（数值 + 单位 + 路况级名 + 口径，估算帧作「估算」）+ 依据行 `输出 423 tokens / 10.0s`（该帧输出 tokens 与生成窗口）。**随会话视图切换**（与计时器同一联动点）：非当前会话不抢占视图；任务结束定格保留（下一帧到达才更新）。默认开启；设置面板「外观」tab 单行按钮可关（`localStorage["gebai.ui.tokenRate"]` 仅存 `"off"`，根元素 `data-token-rate="off"` 时 CSS 隐藏，跨标签页 storage 事件同步，关闭即停计量）。纯展示层能力：不持久化到会话记录

- **文件展示方式**：设置面板「外观」tab 单行按钮（嵌入展示 ⇄ 弹窗查看）——read/write 等文件工具（含 code 子Agent 同款包装）的**产物文件卡**形态切换：**弹窗查看**（默认，现状：收敛为文件链接 chip，点击弹窗查看全文；适配会话相对与 code 项目绝对路径，经 `files/preview` 取数）/ **嵌入展示**（file 内容卡内联展示；参数区与输出不受影响，详见「会话临时文件查看与下载」与「文件展示方式」渲染说明）。`localStorage["gebai.ui.fileDisplay"]` 仅存 `"inline"`，跨标签 storage 事件同步；渲染是结构性的，切换后重载当前会话消息即时生效。纯展示层能力：不持久化到会话记录。**脚本桥（js/py）编排的产物同样受控**——桥自身没有文件卡声明（它的参数是脚本，路径参数不固定），透传的 file 块带上**来源工具名**（`via`），判定按来源工具走同一份 `card.file`：桥内调用 `read`/`write`/`edit` 与直接调用表现一致，而 `show` 这类主动展示工具的产物照常内联

## 架构

### 核心不变式：能力外置、权力内守（TS 引擎核心地位）

歌白的核心是 TS 引擎里的「对话 → 工具调用 → 审批 → 执行」主循环（`AgentEngine` + `ToolRegistry`）。扩展能力可以外置到任意语言、任意进程，**裁决权力必须留在引擎**——本不变式与「单一真相源」并列，是所有多语言/边车/插件类扩展（客卿、`js` 桥、动态工具、二开）的准入门槛。

| 归属 | 内容 | 现落点 |
|------|------|--------|
| **能力**（可外置） | 计算与算法（BM25/哈希/图像/CV 推理）、协议实现、工具实现细节、只读取当前请求 ctx、声明式上报 | 客卿驱动（Python/C++/Go/Rust）、`js`/动态工具子进程、CV sidecar |
| **权力**（不外放） | 工具可见性（装载门控）、审批姿态判定、安全模式裁决、嵌套/depth 规则、会话 ctx 组装、会话真相（chat.json/动态工具/todo/tasks）、进程生命周期 | `registry.resolve` 只返回 `enabled`；`requiresApproval` 由引擎以 `stripApprovalFlags` 后的参数调用；`isToolBlockedInSafeMode`；`fromJsBridge`/`depth`（标记由宿主内存盖章，子进程伪造不了）；`sidecarTool.execute` 组装请求 ctx；`SubAgentManager` 启停回收 |

**边缘执行体只有两种合法姿态**：

- **声明（declaration）**——上报自己具备什么：客卿 `tools.list` 上报工具清单与 JSON Schema，注册名（`{agent}_` 前缀）、审批姿态、装载可见性均由引擎决定；`manifest.requiresApproval:false` 属静态声明（写 manifest 者本就能写任意代码，可 diff、可审阅，用户还可 `PATCH /api/v1/tools` 覆盖）
- **委托（intent）**——表达想做什么，由引擎裁决后执行：`js` 桥的 `{t:"call"}` 是唯一现役实现，引擎按调用者审批姿态（免审运行拦截需审批工具）、会话 ctx、嵌套规则重新裁决，子进程无法伪造豁免

**两条红线**（违反即稀释核心地位，不予准入）：

- **不得自证授权**：运行期由执行体自证「本次免审 / 我有权限」无效——审批姿态的唯一判定方是引擎，运行期自证不可审阅、不可 diff
- **不得自取工具**：执行体自行取用工具、自行指定会话 ctx（sessionId/user/cwd/env）、自行缓存授权结果——一旦如此，引擎退化为消息总线

**提案判定口径**（新开「外部执行体 → 工具」通道前先过三问）：① 来源可否证明且不可提权（宿主盖章标记 / 签发 token 链）？② 审批姿态由谁判定（须引擎；黑盒执行体不得继承调用者的审批豁免）？③ 能否用既有结构化解替代（TS 工具内 `ctx.registry` 直调、客卿跨语言同名合并、`js` 编排）？三问不齐则不做。参照实现：`packages/agents/src/core/shared/cv-analysis.ts` 的 `ctx.registry.resolve(tool).tool.execute({ ...args, image: rel }, ctx)`——进程内直调其他工具，复用装载门控/审批/ctx 组装，不绕过任何治理层。

### 运行形态

一套核心代码、同一个二进制可执行文件，支持两种运行形态，由启动参数/环境变量切换：

| 形态 | 启动方式 | 说明 |
|------|---------|------|
| **本地模式**（默认） | 直接运行二进制 / 访问 `http://127.0.0.1:{port}` | 本机使用，直接以 **admin 用户**身份工作，**免登录、不受任何权限限制**（超级权限 + 路径沙箱豁免）；桌面 WebView / 浏览器均可 |
| **服务模式** | 后台运行二进制 + `GEBAI_MODE=server`（或 `--server` 参数） | 部署在服务器上，多用户公用，**账号密码登录**（密码仅存加盐哈希）；**开放注册**（注册用户为普通角色）；admin 为特权用户，**唯一入口是 `GEBAI_ADMIN_PASSWORD_HASH`**（未设置时 admin 禁用） |

核心逻辑完全一致，仅宿主（WebView / 浏览器 / 远程）与用户机制不同。

#### 启动参数与环境变量

启动配置可来自 `.env` 文件或真实环境变量，**真实环境变量恒优先**（文件只填充未设置的键，`键=值` 逐行解析）：脚本调试模式显式加载仓库根 `.env`；二进制发行（含桌面端两种形态——浏览器形态 exe 与启动器侧车）加载 `{GEBAI_HOME}/.env`（默认 `~/.gebai/.env`，不存在则跳过），部署产物免重建环境即可修改配置。

| 参数/变量 | 说明 | 默认 |
|----------|------|------|
| `GEBAI_HOST` | 监听地址（`127.0.0.1` 本地 / `0.0.0.0` 对外） | `127.0.0.1` |
| `GEBAI_PORT` | 监听端口（桌面形态默认固定 47896，见「端口固定」） | `3000` |
| `GEBAI_PLAYWRIGHT_CHANNEL` | playwright 子Agent 浏览器启动 channel（`chromium.launch` 的 channel 参数，留空 = 不指定用默认 chromium） | 平台默认：Windows=`msedge`（系统 Edge，免下载浏览器），其余空 |
| `GEBAI_MODE` | 运行形态：`local`（本地模式，默认）/ `server`（服务模式，需登录鉴权）；兼容旧 `GEBAI_AUTH`（`none`→local、`multi`→server） | `local` |
| `GEBAI_ADMIN_PASSWORD_HASH` | 服务模式 admin 用户密码哈希（格式 `salt:hash` 均为 hex，salt 16 字节/hash 64 字节，与注册表 scrypt 加盐哈希一致）；**设置则启用 admin（每次启动覆盖其哈希），不设置则禁用 admin 用户**；非法格式启动即报错。生成命令：`bun run --cwd packages/server hash-password -- '密码'`（或管道/交互输入） | 空（admin 禁用） |
| `GEBAI_SERVICE_API_KEY` | ~~已移除~~：原业务系统服务密钥机制已废止——**接口统一账号密码认证**（登录签发令牌），不再有独立服务令牌（避免任何服务端密钥进入 Agent 可达环境） | - |
| `GEBAI_CORS_ORIGINS` | 允许跨域来源（逗号分隔，`*` 表示全部） | `*` |
| `GEBAI_BASE_PATH` | ~~已移除~~：前端请求（静态资源/`/api/*`/`/ws`）一律**按页面 URL 相对解析**，反向代理子路径挂载无需配置，见「反向代理支持」 | - |
| `GEBAI_TRUST_PROXY` | 是否信任 `X-Forwarded-*` 代理头（`true`/`false`）：`X-Forwarded-For` 用于登录/注册限流分桶，`X-Forwarded-Host` 在代理改写 Host 时参与同源判定（见「反向代理支持」） | `false` |
| `GEBAI_SANDBOX` | 路径沙箱：`auto`（**只看运行形态**——服务模式强制启用，本地模式不限制；不按监听地址/IP 判定）/ `on`（强制限制）/ `off`（不限制）；**admin 豁免仅本地模式**（`isExempt` 判 `auth === "local"`；服务端部署下 admin 同受沙箱约束，见「多用户隔离与安全」） | `auto` |
| `GEBAI_SCRIPT_ISOLATION` | 脚本运行根隔离：**服务模式默认且强制开启（`off` 与服务模式互斥，启动即拒绝，同 `GEBAI_SANDBOX=off` 防呆）**——`auto`（环境收敛，bubblewrap 可用时升为文件系统隔离）/ `env`（仅环境收敛，不可再降）/ `bwrap`（强制文件系统隔离，不可用时告警并回落 `env`）；本地模式恒不收敛（`off`/`auto` 同效）；收敛项：`HOME`/`TEMP`/`XDG_*` → 会话目录 `script-env/`；容器内可用性取决于容器是否授予 user namespace，见「数据与执行隔离·脚本运行根」 | `auto` |
| `GEBAI_PRELOAD_SUB_AGENTS` | 启动预载子Agent 名单（逗号分隔）：启动时注册其工具，**每个新会话创建时自动装载**（提示词 system 消息写入会话记录 + 工具注册）；为空 = 默认不预载任何子Agent | 空 |
| `GEBAI_SUB_AGENTS_ENABLE` | 子Agent **白名单**（逗号分隔）：非空时仅保留名单内子Agent（其余全部 `unregister`——`agent_list`/`agent_load`/`subsession_run`/系统提示词注入均不可见，热加载不复活）；与 `GEBAI_SUB_AGENTS_DISABLE` 同时配置**先白后黑**（黑名单最终生效） | 空（不裁剪） |
| `GEBAI_SUB_AGENTS_DISABLE` | 子Agent **黑名单**（逗号分隔）：名单内子Agent `unregister`（运行时能力面收敛，与构建期 `GEBAI_BUILD_SUBAGENTS` 打包裁剪互补）；名单未知名启动告警忽略不阻断 | 空 |
| `GEBAI_UI_STYLE` | 默认 UI 风格（`acrylic`/`aether`/`matrix`/`tokyo-night`/`cyberpunk`/`synthwave`/`aurora`/`ink`/`cny`/`qinhan`）；**服务端白名单为这 10 项**（须与前端主题表 `packages/web/src/theme-core.ts` 同步——主题增删时两处不同步则该主题经环境变量设置会被静默回落为 `acrylic`，参见 `routes/static.ts` 的 `UI_STYLES`）；可被 URL/用户级覆盖 | `acrylic` |
| `GEBAI_LOG_LEVEL` | 日志级别：`debug`/`info`/`warn`/`error`——`loadConfig` 后由组合根 `setLogLevel(config.logLevel)` 生效（最小日志器 `@gebai/sdk/node` 的 `log.*`，见「日志系统」）；非法值忽略保持原级别 | `info` |
| `GEBAI_TOOL_ENABLE` | 工具白名单（逗号分隔，配置后仅启用列表内工具） | 空（全部启用） |
| `GEBAI_TOOL_DISABLE` | 工具黑名单（逗号分隔，排除指定工具） | 空 |
| `GEBAI_FEISHU_*` | 飞书集成配置：全局应用凭证 `GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`（`feishu_docs`/`feishu_group` 子Agent 的全局兜底 + 机器人桥接凭证 + 任务飞书应用消息通知（指定群 chat_id 推送/@人））、云文档目标文件夹 `GEBAI_FEISHU_FOLDER_URL`（`feishu_docs` 创建的资源落用户文件夹下、用户自动有权限；子Agent 前缀 `FEISHU_DOCS_FOLDER_URL` 优先）、机器人桥接开关 `GEBAI_FEISHU_BOT_ENABLED`（`true` 启用长连接事件订阅，见「飞书机器人集成」）、**机器人行为开关 `GEBAI_FEISHU_BOT_NOTIFY_TOOLS`（工具调用过程滚动状态消息）/ `GEBAI_FEISHU_BOT_NOTIFY_ASSISTANT`（助手中间轮文本预览）/ `GEBAI_FEISHU_BOT_AUTO_APPROVE`（需审批工具自动通过，见「飞书机器人集成 → 配置」）**、TLS 策略 `GEBAI_FEISHU_INSECURE_TLS`（`true`/`1` 时所有飞书出站请求禁用证书校验——内网代理场景：机器人桥接 REST/长连接 WebSocket、`feishu_docs` 子Agent 接口与 OAuth 回调兑换，见「飞书 TLS 策略」）；`feishu_group` 专属前缀 `FEISHU_GROUP_APP_ID`/`FEISHU_GROUP_APP_SECRET` 可独立配置 | 不启用 |
| `GEBAI_PUBLIC_URL` | 对外可访问地址（如 `http://localhost:3000` 或公网域名）：飞书用户授权（user_access_token）自动回调默认取 `{GEBAI_PUBLIC_URL}/api/v1/oauth/feishu/callback`（缺省回落 `http://localhost:{GEBAI_PORT|3000}`），需在开发者后台「安全设置 → 重定向 URL」登记 | 空（回落 localhost） |
| `GEBAI_SELF_MODIFY` | 是否允许 `self_optimize` 修改服务端源码（`true`/`false`） | `false` |
| `GEBAI_SAFE_MODE` | 安全模式（**仅启动时从 .env/环境变量加载，不可在会话/任务级修改**）：风险能力**降级而非禁用**——`sh` 只读命令白名单 + 重定向限用户目录、`py`/`js` 只读运行时（写/进程/网络屏蔽，仅保留文件读取）、`write`/`edit`/`patch`/`file` 限定用户目录内、`task_*` 调度类硬阻断、子Agent 工具按 `Tool.safeMode` 自主声明过滤注册（详见「安全模型 → 安全模式」） | `false` |
| `GEBAI_LLM_MODEL` / `OPENAI_*` | LLM Provider 与模型配置 | - |
| `GEBAI_LLM_API_BASE` / `GEBAI_LLM_API_KEY` | LLM 服务地址与密钥（等价 `OPENAI_*`） | - |
| `GEBAI_LLM_API_KIND` | LLM 接口类型：`openai`（兼容 chat/completions）/ `responses`（OpenAI Responses API）/ `anthropic` | `openai` |
| `GEBAI_LLM_ROUTES` | **命名模型路由表**（JSON 对象，`subsession_run` 多路接口用）：`{"路由名": {"model": "...", "api_base"?: "...", "api_key"?, "api_kind"?, "max_context"?}}`——子会话运行的 `model` 参数按路由名走独立端点/模型（未指定的项沿用主配置基准），多子会话多路并行分摊单路限流；未命中路由名视为字面模型名；非法 JSON 静默忽略；会话/任务级 env 同样生效（见「子会话运行」） | 空（分支沿用任务级模型） |
| `GEBAI_LLM_EXTRA_PARAMS` | 额外模型接口参数（JSON 对象，如 `{"reasoning_effort":"high"}`），每次请求顶层合并进请求体；非法 JSON 启动即报错 | 空 |
| `GEBAI_LLM_MAX_OUTPUT_TOKENS` | 单次响应输出 token 上限（大文件生成截断防护）：模型输出超限被截断会致工具参数 JSON 不完整——引擎检测截断并抢救落盘 + 引导 `write append` 分段续写（见「核心Agent流程」）；未配置时 openai/responses 用服务端缺省、anthropic 用 8192（接口强制要求 max_tokens） | 空 |
| `GEBAI_LLM_MULTIMODAL` | 主模型多模态能力声明：`true` 时图片附件 base64 内联进消息，视觉分析可回落到主模型；默认 `false`（纯文本模型须配 `GEBAI_VISION_*` 外挂视觉模型；声明了但接口拒绝图片时引擎自动降级为文本说明） | `false` |
| `GEBAI_VISION_MODEL` | **额外多模态（视觉）模型**：配置后启用独立视觉 Provider（vision 子代理 `vision_analyze` 使用）；接口地址/密钥/类型缺省时继承主模型配置；不配置则回落到主模型（须声明多模态能力） | 空 |
| `GEBAI_VISION_API_BASE` / `GEBAI_VISION_API_KEY` | 视觉模型接口地址与密钥（缺省继承主模型） | - |
| `GEBAI_VISION_API_KIND` | 视觉模型接口类型：`openai`（兼容）/ `responses` / `anthropic` | 同主模型 |
| `GEBAI_VISION_MAX_CONTEXT` | 视觉模型上下文 token 预算 | `128000` |
| `GEBAI_VISION_EXTRA_PARAMS` | 视觉模型额外请求体参数（JSON 对象，同 `GEBAI_LLM_EXTRA_PARAMS` 语义） | 空 |
| `GEBAI_CV_MODELS_DIR` | 本地 CV（`desktop_ocr`/`desktop_locate`）模型目录：含 `det.onnx`/`rec.onnx`/`dict.txt`（PP-OCR 中英文）三件套；任务级 env 可覆盖；未设置时按 二进制形态释放的内嵌模型目录 → `{GEBAI_HOME}/resources/models/cv/ocr/`（资源目录）解析（见「小模型识别」） | 空 |
| `GEBAI_CV_MAX_SIDE` | 本地 CV 输入图像降采样上限（像素长边，超过等比缩小后识别；坐标仍映射回原始像素系） | `1280` |
| `GEBAI_CV_DETECT_MODEL` / `GEBAI_CV_DETECT_LABELS` | `desktop_detect` 自备 YOLO ONNX 模型路径与标签文件路径（每行一个类别；**MODEL 缺省自动发现 `{GEBAI_HOME}/resources/models/cv/detect/` 下唯一 `.onnx`——资源目录 drop-in 即用，多个时列出候选要求显式指定**；模型不内嵌，COCO 类别对 UI 无意义；ultralytics 导出的 ONNX 内嵌 imgsz/names 元数据自动读取——LABELS 与 SIZE 可省） | 空 |
| `GEBAI_CV_DETECT_SIZE` | 检测输入边长覆盖（像素，320-4096；缺省取模型元数据 `imgsz`，再回落 graph 首输入静态形状，末级 640） | 空 |
| `GEBAI_CV_BACKEND` | **CV 推理后端全局开关（检测与 OCR 共用）**：`auto`（默认，GPU sidecar 可用即用、失败回落 wasm 并毒化）/ `sidecar`（强制 node 原生，失败不回落）/ `wasm`（强制进程内单线程 CPU） | `auto` |
| `GEBAI_CV_DETECT_BACKEND` / `GEBAI_CV_OCR_BACKEND` | 细分覆盖（优先于全局）：检测/OCR 各自独立选择后端，取值同 `GEBAI_CV_BACKEND` | 空 |
| `GEBAI_CV_EP` | GPU sidecar 执行提供者（全部 CV 推理共用）：`auto`（默认，Windows dml→cuda / macOS coreml→cuda 逐级探测、全失败回落 cpu 兜底并如实上报）/ `dml` / `cuda` / `coreml` / `cpu`（`GEBAI_CV_DETECT_EP` 为早期别名兼容） | `auto` |
| `GEBAI_CV_REC_BATCH` | OCR rec 批处理大小（1..32，缺省 8）：文本行裁剪经 letterbox 后形状一致，拼批一次推理——逐行推理次数 = 文本行数，批处理把这些次数整除并省掉每行 IPC 往返（详见「小模型识别」→「OCR rec 批处理」） | `8` |
| `GEBAI_CV_ORT_NODE_DIR` | onnxruntime-node 解析目录（包根或其父目录；缺省按 `{GEBAI_HOME}/resources/vendor/node_modules/onnxruntime-node`（**资源目录约定位置，放入即生效**——模型/运行时依赖集中存放于独立 git 仓库 `resources/`，见其 README）→ `{GEBAI_HOME}/vendor` → node_modules 解析；不可用时检测自动回落 wasm，见「小模型识别」检测分层后端） | 空 |
| `GEBAI_KEQING` | 客卿总开关：`off`/`false`/`0` 显式禁用（发现器整体不启动，客卿子代理完全不可见）；沙箱启用（服务端部署形态）同样自动禁用——客卿仅限本地使用（见「客卿」） | 空（本地形态默认启用） |
| `GEBAI_PYTHON_DIR` | 内置 python 子代理的 `{python}` 占位解析优先项（目录含解释器）；缺省按 `{GEBAI_HOME}/venv` → 系统 PATH 顺序解析 | 空 |
| `GEBAI_SIGNUP_MODE` | 注册审批模式：`open`（默认，注册即用）/ `approval`（注册待 admin 审批——用户置 `disabled+pending` 待审、不可登录，admin 在用户管理页批准/拒绝） | `open` |
| `GEBAI_APPROVAL_SKIP` | 会话级审批跳过（等价 `/approval-skip`，`true` 跳过） | 空 |
| `GEBAI_TASKS_ENABLED` | 是否启用**统一任务能力**（注册 `task` 子Agent（`task_add`/`task_list`/`task_update`/`task_run`/`task_cancel`/`task_remove`/`task_files`/`task_notify` 工具）、启动任务调度器并开放 REST `/api/v1/tasks` 管理面；`false` 时子Agent 不注册、调度器不启动、REST 返回 503，能力完全不可见） | `true` |
| `GEBAI_TASK_MAX_CONCURRENT` | 每用户同时运行的**任务会话数**（定时/普通任务并行上限；闲时任务另受「队列空闲 + 每用户仅 1 个」约束；运行中的任务不因额度不足被中断——新任务排队等待；非法值回落 5） | `5` |
| `GEBAI_IDLE_TODO_ENABLED` | 是否启用**用户级待办**（标题栏轮盘「待办」弹窗 + REST `/api/v1/todos` 管理面；待办随时可手动执行（入队按序跑一次），开启 ⚡ 闲时自动执行的条目绑定一个闲时任务、由任务调度器在队列空闲且无运行中会话时执行）；`false` 时 REST 返回 503 | `true` |
| `GEBAI_SCHEDULER` | 调度器门控（同一 `GEBAI_HOME` 下的跨进程互斥）：`auto`（默认）=主实例锁判定，同库只有一个实例跑任务队列与待办闲时任务，其余退化为从实例（只服务 HTTP/WS，看门狗在主实例退出/租约过期后自动接管）；`on`=强制本实例跑调度（忽略锁，多实例同时 `on` 会重复调度）；`off`=完全不跑调度 | `auto` |
| `GEBAI_FS_ENABLED` | 是否启用**文件工作台**（`/files` 页面与 `/api/v1/fs`、`/api/v1/git`、`/api/v1/roots` 端点）；`false` 时页面 404、端点全部 404、标题栏入口按钮隐藏 | `true` |
| `GEBAI_FS_WRITE` | 工作台写开关：`false` 时纯只读检视（新建/改名/移动/复制/删除/上传/保存全部拒绝；Git 写操作另由下方开关控制） | `true` |
| `GEBAI_FS_WATCH` | 工作台**变更监听**（自动刷新）的 `fs.watch` 开关：`false` 时 `/api/v1/fs/watch` 立即返回 `enabled:false`，前端退化为固定间隔的纯轮询（页面仍会自己变，只是慢一拍且每轮都有实际列举开销） | `true` |
| `GEBAI_LSP` | 工作台**语言服务器（LSP）**开关：`false` 时清单回 `enabled:false` 且 `lsp.*` 消息一律拒绝，不起任何外部语言服务器（Monaco 内置语言服务与符号提取照旧） | `true` |
| `GEBAI_LSP_SERVERS` | 语言服务器覆盖表（JSON 对象，键 = Monaco 语言 id；值 = 命令 / `{command,args}` / 候选数组 / `null` 关闭）；显式列出即启用（含默认不启用的 typescript/json/css/html——那几种已由 Monaco 内置语言服务覆盖） | 不设置（用内置表） |
| `GEBAI_LSP_IDLE_MS` | 语言服务器空闲回收（毫秒）：**无打开文档**的常驻进程超时即退出 | `600000` |
| `GEBAI_LSP_MAX` | 并发语言服务器进程上限（超标时先回收空闲会话，仍满则拒绝新文档） | `4` |
| `GEBAI_FS_ROOTS` | 服务模式下的额外白名单根（JSON 数组：字符串或 `{name,path,description,writable}`）；本地模式自动追加主目录/服务工作目录/盘符，无需配置 | 不设置 |
| `GEBAI_FS_MAX_READ` / `GEBAI_FS_MAX_WRITE` / `GEBAI_FS_MAX_UPLOAD` / `GEBAI_FS_MAX_ZIP` | 工作台单次读取/写入/上传单文件/打包下载上限（字节） | 10MB / 10MB / 100MB / 500MB |
| `GEBAI_FS_MAX_DIFF` / `GEBAI_GIT_MAX_FILE` | 单次 diff 文本总量上限（字节）/ 单个文本文件（内容与逐行差异）上限（字符）——超限降级为「统计 + truncated/tooLarge 标记 + UI 提示」 | 8MB / 1M |
| `GEBAI_FS_HIDDEN` | 目录树默认是否列出隐藏文件（前端在「更多」菜单里可随时切换；请求显式带 `showHidden` 时以该参数为准） | `true` |
| `GEBAI_FS_AUDIT` | 工作台写操作审计（`{GEBAI_HOME}/audit-fs.jsonl`：谁、哪个根、什么动作、成败）——工作台写操作是用户本人直操、不走工具审批，审计是它的留痕等价物 | `true` |
| `GEBAI_GIT_WRITE` | Git 写操作开关（暂存/提交/分支/合并/变基/重置/标签/暂存区/丢弃）；`false` 时仅保留只读查看与对比 | `true` |
| `GEBAI_GIT_REMOTE` | Git 远程操作开关（fetch/pull/push/remote，需网络与凭据） | `true` |
| `GEBAI_TASK_NOTIFY_WEBHOOK` | 任务**全局默认通知 webhook**（http(s) 回调 URL）：任务未配置自己的 `notify` 时自动经该通道推送（任务自配则不叠加）；启动时校验（SSRF 同规则），非法配置告警忽略 | 不设置 |
| `GEBAI_TASK_NOTIFY_FEISHU` | 任务**全局默认飞书通知**：群 chat_id（`oc_` 前缀，以应用身份推送——需 `GEBAI_FEISHU_APP_ID/SECRET`）或群机器人 webhook URL，语义同 `feishu` 通道双形态；任务未配置 `notify` 时自动生效 | 不设置 |
| `GEBAI_EXTERNAL_AUTH_SECRET` | 外部身份扩展点：HMAC 共享密钥（与 `GEBAI_EXTERNAL_AUTH_URL` 互斥，同设启动报错）；网站用密钥对「用户名.过期时间戳」签名（HMAC-SHA256，hex），凭证格式 `{exp}.{sig}`，exp 为毫秒时间戳，±10 分钟有效窗口防重放 | 空（不启用） |
| `GEBAI_EXTERNAL_AUTH_URL` | 外部身份扩展点：HTTP 回调验证 URL（与 `GEBAI_EXTERNAL_AUTH_SECRET` 互斥，同设启动报错；**必须 HTTPS**，localhost/127.0.0.1 例外防中间人伪造）；GEBAI 把 `{username, credential}` POST 给回调（5s 超时），业务系统自行校验（如查自己 localStorage 对应的服务端态），**必须核验 username 与凭证归属一致**，响应 2xx 且 `{"ok":true}` 即通过，可用 `username` 字段覆盖映射（仅应在明确校验后使用） | 空（不启用） |
| `GEBAI_EXTERNAL_AUTH_AUTOCREATE` | 外部用户名不存在时自动创建 GEBAI 用户（普通角色、随机密码不可密码登录）；`false` 时仅允许管理员预建的同名用户 | `true` |
| `GEBAI_EXTERNAL_AUTH_STORAGE_KEY` | Web UI 同源直读宿主 localStorage 的凭证 key（值支持 JSON `{"username","credential"}` 或 `"username:credential"` 字符串）；不设则仅支持 URL 参数注入 | 空 |
| `--server` | 开启服务模式（等价 `GEBAI_MODE=server`，参数优先） | - |

> 以上为**全局层**环境变量（进程注入），会话层可覆盖其中可运行时变更的项（**模型/Provider 配置全量可覆盖**：`GEBAI_LLM_MODEL`/`API_BASE`/`API_KEY`/`API_KIND`/`MAX_CONTEXT`/`MAX_OUTPUT_TOKENS`/`MULTIMODAL`/`ROUTES` 与 `GEBAI_VISION_*` 任务级生效，按任务重建 Provider，见「环境变量配置」）。

### 服务端
- 服务端通过 WebSocket 与客户端通信，Agent 能力（Chat、工具调用、审批等）均在服务端内部实现
- **单端口暴露**：静态 Web UI、WebSocket（`/ws`）、REST API（`/api/*`）全部由同一端口承载，无多端口部署需求
- 对外提供双通道 API：**WebSocket**（实时双向流式）与 **REST HTTP**（同步请求，业务系统集成）
- 服务端内置 Web UI，通过浏览器可直接访问使用
- LLM 交互、子Agent管理、工具执行等逻辑全部封装在服务端内部
- 模型配置通过环境变量管理（`GEBAI_*` / `OPENAI_*`）；**服务端可不配置任何模型变量**——前端「设置 → 环境变量」配置（浏览器本地、不落盘），随每次发送消息临时注入，任务启动按合并后 env 重建 Provider（见「环境变量配置」）；未配置接口地址时 Provider 抛配置错误（`LLMConfigError`，不重试直接失败），错误信息给出前端配置入口指引
- 支持多种 LLM Provider：支持 OpenAI 兼容 `chat/completions`、OpenAI Responses（`/responses`）与 Anthropic `messages` 三类接口（均流式），通过统一的 `provider.chat()` 抽象封装，自行解析 SSE 流，不依赖第三方 AI SDK
- **额外模型接口参数**：支持自定义非标准请求体参数（如 `reasoning_effort` 推理强度、`temperature`、`thinking` 等），来源两级——Provider 级（`GEBAI_LLM_EXTRA_PARAMS` 环境变量，启动解析失败即报错）+ 任务级（浏览器本地注入同名环境变量，非法 JSON 静默忽略），后者优先，均顶层合并进请求体
- **任务级模型覆盖**：主模型与视觉模型配置（`GEBAI_LLM_*` 全套与 `GEBAI_VISION_*`）支持会话/任务级覆盖——启动时以进程环境变量固化基准配置（`core/llm/llm.ts` 的 `applyModelEnvOverrides`/`resolveVisionProvider`，boot/compose.ts 组装 `AgentEngineOptions.resolveProvider` 与 env 感知的视觉 getter），每次任务启动按合并后 env 解析 Provider（无覆盖键时沿用启动实例零开销）；覆盖项含模型名、接口地址、密钥、接口类型、上下文预算、多模态声明，未覆盖项继承启动配置；作用域覆盖主循环（含上下文压缩阈值/摘要与附件内联判定——**主动压缩（UI/REST 入口）未显式指定 Provider 时同样按合并后 env 解析**）与 `subsession_run` 子会话运行
- 多模态：`provider.chat()` 统一承载文本 + 图片消息（音频/视频为文件工作台查看能力，不投喂模型），按 Provider/模型能力自动组装各自消息格式（统一内部图片块 `{type:"image", mime, data}` → OpenAI 系（chat/completions 与 Responses）`image_url` data URL / Anthropic base64 `image` 块）
- **额外多模态（视觉）模型**：支持独立配置视觉模型（`GEBAI_VISION_*`，缺省继承主模型接口），供 vision 子代理的 `vision_analyze` 将图片文件交给视觉模型分析（目标 + 图片文件参数）；未配置时回落到主模型（须声明多模态能力）
- **模型能力声明**：`LLMProvider.capabilities()` 返回 `{ streaming, toolCalling, multimodal, maxContextTokens, maxOutputTokens }`——`maxOutputTokens`（单次响应输出上限）为上下文压缩的触发基准（窗口剩余必须足够支撑一次回复）；`maxContextTokens` 用于上下文占用判定
- 通过 `GEBAI_MODE`（默认 `local`；兼容旧 `GEBAI_AUTH`，或 CLI `--server`）环境变量切换运行形态：
  - **本地模式**（默认）：无需登录，直接以 **admin 用户**身份工作（**管理员超级权限 + 路径沙箱豁免**，不受任何权限限制），数据仍按用户目录存储
  - **服务模式**：启用登录鉴权（用户名/密码），密码仅存**加盐哈希**（scrypt，不落明文），会话按用户隔离，多用户公用同一服务端；**开放注册**（注册用户恒为普通角色）；**admin 为特权用户**（不受用户权限限制），**唯一入口是 `GEBAI_ADMIN_PASSWORD_HASH`**（未配置时 admin 禁用，但普通用户可注册使用）
- 业务系统集成统一走**账号密码认证**：`POST /api/v1/auth/login` 获取令牌后以 `Authorization: Bearer <token>` 调用 REST；或**单次请求直接带 HTTP Basic**（`Authorization: Basic base64(username:password)`，等价隐式登录，复用密码校验与登录限流、不签发令牌）；WS 用 `auth.login { token }` 建立用户上下文——**无独立服务令牌**（原 `GEBAI_SERVICE_API_KEY` 服务身份机制已移除，避免任何服务端密钥进入 Agent 可达环境）
- **启动加载策略（重依赖惰性化）**：以「服务可用性优先」组织启动——`listening` 之前只加载引擎主循环与装配必需的模块，重第三方依赖一律延迟到首次实际使用（ESM 用 `await import()`，同步 API 用模块级单例 `require` 缓存）：
  - **图表渲染**（`core/support/diagram-render.ts`：echarts/happy-dom/mermaid/plantuml/d2）：引擎（ToolContext `renderDiagram`）与飞书桥接（`FeishuBot.rendererOf`）均惰性取，不在启动路径
  - **Office/PDF 解析与生成**（`agents/wps/*`：exceljs/docx/pptxgenjs/pdf-lib/fontkit/happy-dom）：各工具 `execute`（均 async）内引入；`@gebai/agents` 的 `renderOfficeReadingView` 经 `wps/excel.loadWorkbook()` 复用同一惰性入口（该导出位于 `@gebai/agents` 入口，被 `routes/fs.ts`/`routes/session-files.ts` 经 `await import("@gebai/agents")` 动态引用，不得静态拖入重依赖）
  - **内嵌 Web UI bundle**（`core/web.bundle.generated.ts`，数十 MB）：仅二进制模式读取内嵌资源时加载（`routes/static.ts` 的 `embeddedWebAssets()`）；源码/dev 形态走 `webDist`，不进启动路径
  - **飞书机器人长连接**：监听建立后异步发起（`boot/serve.ts` 于 `listening` 之后 `void feishuBot.start()`，握手失败只记 error 日志、服务照常可用；凭证缺失仍在 compose 装配期抛错）；`new FeishuBot` 构造不触发网络
  - 子Agent 定义模块（`discover()` 逐一 import 以取 `def`）遵循同一约定：**模块顶层不静态引入重第三方库**，实现内延迟到首次工具调用（`def` 的 name/description/tools 组装只依赖轻量模块）

### 核心模块与接口

服务端按接口解耦，全部依赖注入、可独立测试：

```
                    ┌──────────────┐
   WS/Webhook  │  API 层      │  (Hono 路由、鉴权、CORS)
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
   ┌──────┐        │ AgentEngine  │        ┌────────────┐
   │ LLM  │◄──────►│  (核心循环)   │◄──────►│ ToolRegistry│
   │Prov. │        └──────┬───────┘        └────────────┘
   └──────┘               │
              ┌───────────┼───────────┐
       ┌──────▼───┐ ┌─────▼────┐ ┌────▼─────┐
       │SessionStore│ │EnvManager│ │EventBus │
       └──────────┘ └──────────┘ └──────────┘
```

| 接口 | 职责 | 关键方法 |
|------|------|---------|
| `LLMProvider` | 三类接口统一抽象、多模态组装、流式解析；**多模态内容块转换**：统一内部图片块 `{type:"image", mime, data}`（base64）按接口规范转换（OpenAI 系 `image_url` data URL、Anthropic base64 `image` 块），`imageMessageBlocks()` 助手构造文本+图片消息；**Responses API**：消息转 `input`（assistant 工具调用拆独立 `function_call` item + `function_call_output`），工具扁平格式 `{type:"function",name,description,parameters}`，流式事件解析（`output_item.added`/`function_call_arguments.delta|done`/`output_text.delta`/`reasoning_*_text.delta`/`completed`，stop reason 取末条 message `finish_reason`）；**额外模型接口参数**：Provider 级（`GEBAI_LLM_EXTRA_PARAMS`）与调用级（`ChatOptions.extraParams`）请求体参数顶层合并（后者优先）；**接口健壮性**：fetch 层对网络错误/429/5xx 指数退避重试（2 次，500ms 基数，退避可被取消），4xx 与 AbortError 不重试，错误响应体截断 200 字符 | `chat(messages, opts): AsyncIterable<Chunk>`、`capabilities()` |
| `AgentEngine` | 主循环状态机：工具循环（**同批工具并行执行**，见「核心Agent流程」）/审批/重试/压缩/取消；**模型调用健壮性**：空响应（无文本且无工具调用，含只思考未输出）与无产出异常经 `callModel` 指数退避重试（2 次，800ms 基数，注入提示引导），已有产出后断流不重试（避免重复输出），耗尽抛中文错误；**重试过程前端可见**——每次将重试的模型服务异常推送 `event.model.error`（非终态，前端渲染为消息流内**常驻**异常记录，见「事件清单」）；**重复检测**：最近 8 次工具调用签名（工具名+参数 JSON）滚动窗口，相同签名在窗口尾部**连续**出现第 3 次（其间无任何其他调用）才中断执行并注入引导提示（间隔其他调用后重发同签名是「改动后复查」，不判重复），中断超 2 次终止工具循环（避免模型无效重复；同批重复签名只计一次——同批相同调用是有意扇出，跨轮连续重发才累积）；待办续做回复与上轮完全相同（纯文本）时追加防复述提示；**会话级已读文件追踪**（fileGuard，防盲写守卫，见「防盲写守卫」） | `run(sessionId, user, prompt, opts)`、`cancel(sessionId)` |
| `ToolRegistry` | 工具注册/命名空间解析/启停/审批声明 | `register(tool)`、`resolve(name)`、`list()` |
| `SessionStore` | 会话/消息/待办/附件持久化（分片路径） | `load(id)`、`save(session)`、`appendMessage()` |
| `EnvManager` | 环境变量合并：全局（进程 env）+ 会话内存态（不落盘）；用户环境变量只存浏览器本地、随 prompt 任务级注入 | `resolve(sessionId, user): Promise<Record<string,string>>` |
| `AuthService` | 用户认证、令牌、归属校验、API Key | `login()`、`authorize(ws/api)` |
| `Sandbox` | 路径沙箱、子进程执行、超时控制（支持取消信号：abort 按进程树终止——Unix 进程组 SIGKILL、Windows `taskkill /T`） | `resolvePath(user, path)`、`exec(cmd, opts)` |
| `EventBus` | 服务端事件分发（WS/Webhook/飞书） | `publish(event)`、`subscribe()` |
| `ContextCompressor` | 上下文压缩与截断落盘（水位触发 / 溢出恢复） | `compactSession(sessionId, user, scope?, provider, opts?)` |

#### 服务端目录结构（领域分层）

`packages/server/src/` 按领域分目录组织，每目录内聚且文件数受控（无大目录平铺）：

```
src/
  index.ts          # 薄入口：startServer = compose + serve；进程主命令分发见 boot/cli.ts
  app.ts            # Hono 应用工厂（CORS/鉴权中间件 + 各域路由装配 + AppDeps）
  boot/             # 启动装配：compose.ts（DI 组合根）/ serve.ts（Bun.serve + WS 运行时）/ cli.ts（exec 子命令与 web dist 自动构建）
  routes/           # REST 路由按域拆分（auth/users/sessions/session-files/tools/tasks/todos/docs/static + **文件工作台：fs/fs-shared/roots/git** + misc(feedback+webhook)），
                    #   每文件 register{Domain}Routes(rc)，契约见 routes/context.ts；装配顺序：sessions 先于 session-files（共享 :id 白名单中间件），fs 先于 git（共享 fs-shared 的根解析与审计）
  ws-handlers/      # WS 消息处理器按域拆分（auth/session/prompt/interaction/admin），每文件导出 Record<消息类型, WsHandler>；
                    #   ws.ts 为分发器（未登录守卫/会话 id 白名单/异常兜底）
  core/             # 领域核心（根目录只放构建生成物，源码按域入子目录）：
    base/           #   基础层：types/config/paths/event-bus/registry + 文本行/补丁纯算法
    llm/            #   LLM 接入（Provider 工厂与三接口实现）+ 多模态解析
    engine/         #   引擎：engine.ts（主循环）+ interactions.ts（五种阻塞交互的等待/决策）+ compressor.ts（上下文压缩/溢出恢复）+ prompt.ts（系统提示词构建）
    tools/          #   全局工具域：注册文件（fs/exec/show/agent/interact/schemas/restart/projects）+ shared.ts（GlobalToolEntry 契约与 schema/parseRegion 助手）+ index.ts（聚合器/barrel）。本地识别共享工厂（cv-analysis）与 vision 工厂已迁 @gebai/agents core/shared，经 registry 直调消费
    support/        #   工具与引擎共用支撑：truncate/walk/artifacts/plan/exec-opts/fetch-scope/diagram-render（analyzer 与 image-resize 已迁 @gebai/agents）
    session/        #   会话域：store（持久化）/env/subsessions（子会话运行）/gc
        schedule/       # 统一任务与用户级待办：tasks（定时/普通/闲时任务 + 队列与执行）+ expr（执行表达式）+ notify（通知通道）+ todos（用户级待办清单与闲时任务绑定）
    exec/           #   脚本执行：js-tool/sh-tasks
    browser/        #   仅剩 fetch-proxy（透明浏览器代理垫片，供 fetch_url/SSRF 路径共用）；完整桥接基建（bridge.ts + driver.mjs + pwcore 内嵌产物）在 @gebai/agents core/browser——playwright/reverse_site 子Agent 与浏览器代理共用的平台级底座，不依赖子Agent 定义存在
    cv/             #   （已整体迁至 @gebai/agents core/cv）本地 CV 推理基建：ort-loader + cv + image/ocr/detect + onnx-meta + template + sidecar/cv-driver.mjs，内嵌产物 cv.embedded.generated.json 同目录——desktop/playwright 子Agent 本地识别的底座（工具消费层=agents core/shared/cv-analysis 共享工厂）
    fs/             #   文件工作台：roots（`sess:`/`proj:`/`bind:`/`user:`/`abs:` 根解析 + 白名单 + 路径防护）/service（列举与自然序、二进制与编码探测、文本读取、Range、打包 ZIP、内容搜索）/write（受保护的写：etag 乐观锁、编码回写、删除即物理删除）/archive（零依赖 ZIP 读）/mime（类型→查看器/语言）/audit（写操作审计）
    git/            #   Git 图形化：service.ts（git CLI 封装：状态/日志/分支/标签/暂存/远程 + **任意两端 compare/diff/contentAt**（WORKTREE/INDEX/任意 rev/mergeBase）+ 写操作串行化与自动备份）
    security/       #   安全：sandbox/safety/ratelimit（ip/fetch-guard 的 SSRF 防护已迁 @gebai/agents core/shared，server 侧经包导入）
    agents/         #   子Agent 装载器：subagents（扫描/热加载）/keqing（客卿 sidecar）/merge/env-catalog（sub-agent-md 已迁 @gebai/agents core/shared）
  feishu-bot/       # 飞书对话桥接
```

**分层规则**：依赖单向——`base` ← `support`/`security` ← `tools`/`exec`/`session`/`schedule` ← `engine` ← `routes`/`ws-handlers` ← `boot`。`core/` 内部模块**禁止 import `core/tools` 聚合 barrel**（聚合器含目录扫描顶层 await，反向依赖会成环导致未初始化绑定）——共用能力直引 `core/support/*`、`core/security/*` 叶子模块；子 Agent 与顶层（routes/boot）可经 barrel 取既有导出。`checkWebhookUrl`/`fetchWithRedirectGuard` 等 SSRF 校验统一在 `@gebai/agents` core/shared/fetch-guard（server 侧经包导入，不新增越层依赖）。

#### 全局工具零注册（文件级自动扩展）

全局工具与子 Agent 同为「丢文件即注册」的扩展模型（`core/tools/` 目录）：

- **注册契约**：目录内凡导出 `export const globalTools: GlobalToolEntry[]` 的文件即被收集——`GlobalToolEntry = { name, tool: Tool | (() => Tool), project?: boolean | "workdir" }`（`project` 声明 `projectAware` 包装，聚合器统一施加）；`shared.ts` 定义契约，纯辅助文件（如 `projects.ts` 等不导出 globalTools 的）不参与注册
- **双通道**：dev 形态由 `core/tools/index.ts` 模块初始化时运行时扫描本目录（重启进程生效，无热加载——self_optimize 只写子 Agent 目录）；dist/`--compile` 形态回退 `packages/server/scripts/build-tools.ts` 生成的静态注册表 `core/tools/bundle.generated.ts`（gitignore）
- **聚合器**：`createAllGlobalTools()`/`createGlobalTools()`/`isGlobalToolExcluded()` 签名与语义不变（构建期排除、重名抛错、engine 与 index.ts 注册面零改动）；`core/tools/index.ts` barrel 具名再导出原单文件时代的全部既有导出（导入方零改动），新增工具请从所在文件直接 import
- 新增全局工具 = 在 `core/tools/` 新建一个导出 `globalTools` 的文件，不改任何中央注册表；命名空间专属工具（git/page_capture/read_feedback 等）落 `@gebai/agents` core/shared（原 server `core/tools/extras.ts` 已整体迁出），由子 Agent def 引用
- **模式限定工具**：个别全局工具仅特定运行形态注入（如 `restart_server` 仅本地模式——compose 注册处按 `config.auth` 过滤，服务模式多用户部署不暴露）；工具仍在全局表（本地模式可用面不变），过滤发生在 compose 注册环节

#### restart_server（重启自身服务，仅本地模式）

重启本服务进程的可靠性设计——「自杀后谁拉起」：重启的最大风险是旧进程死了新进程起不来（进程树连坐），方案是外部拉起器彻底脱离服务进程树：

- **外部拉起器（双平台）**：工具执行时把拉起脚本落盘 `{tmpdir}/gebai-restart/`，并部署为独立于服务进程树的进程：
  - Windows：PowerShell 脚本 `launcher.ps1`（**UTF-8 BOM**——Windows PowerShell 5.1 对无 BOM 文件按 ANSI 解析，脚本内中文会撕裂字符串），二段式启动：服务经短命令 `powershell Start-Process powershell -File launcher.ps1` 部署——外层 powershell（服务的短暂子进程）用 `Start-Process` 创建内层独立窗口进程执行拉起器后立即退出，链路断开、与原服务零亲缘（**WMIC 已废弃**：Win11 24H2+ 默认移除，旧版 wmic 方案在新系统上必然失败——拉起器从未启动，服务自杀后无人拉起；也不可直接 Bun.spawn powershell -File：Bun 子进程经 job object 连坐，服务退出时拉起器一起被杀）；
  - Linux/macOS：bash 脚本 `launcher.sh`（UTF-8 无 BOM，脚本内 mkdir -p 自建工作目录），经 `setsid` 启动（新会话新进程组，防 `kill -PGID` 整杀连坐），标准流重定向 /dev/null——服务退出后拉起器被 init 收养继续运行；
- **拉起器流程（两平台同构）**：等旧进程退出（最长 60s；win `Get-Process` / posix `kill -0`）→ 等端口释放（最长 30s；win TCP 探测 / posix `ss -ltn`）→ **端口占用分类后失败退出**（见下；不动无辜进程）→ 日志轮转（`server.log.*` → `*.prev`：`Start-Process -RedirectStandardOutput` 与 posix `>` 均为覆盖写，连续重启会吞掉上一次诊断日志）→ 同端口/同 cwd/同启动级环境变量（`GEBAI_PORT/HOST/HOME/MODE/BASE_PATH/DEV_RELOAD`，凭据类不复制——`.env` 由 loadConfig 自行加载；DEV_RELOAD 承载「`--reload` 启动的开发热刷新在重启后不丢」：argv 参数不会被拉起器复制，工具侧显式补 `GEBAI_DEV_RELOAD=1`）启动新服务（win `Start-Process` / posix `nohup &`，Windows 无 job object 连坐、posix 由 init 收养）→ 就绪探测（最长 90s；**端口属主 ≠ 旧 PID 且 HTTP 200 双条件**——单看 200 会在旧服务未死时误判；属主探测 win `Get-NetTCPConnection` / posix `ss -ltnp`）→ 写状态文件 `state.json`（成功=新 PID；失败=原因+日志尾部）
- **端口占用的两种情形（必须分开报，处置完全不同）**：端口释放的判据是 **socket 句柄引用计数归零**，不是「拥有者进程是否存活」——
  - `occupied`（真被占用）：属主 PID **存活**且不是本次要替换的旧进程 → 不抢端口、立即失败（先处置该进程或改用其他端口）；
  - `zombie-socket`（僵尸套接字）：属主 PID **已不存在**但端口仍 `LISTENING`（`netstat` 显示 PID、`tasklist` 查不到、`taskkill` 报「找不到进程」）——典型成因是 socket 句柄被其他进程**继承**，内核对象因引用计数未归零而不释放，**用户空间无法直接释放**；
  - 两种情形都**严格失败**（不自动换端口、不动无辜进程），但 state.json 写 `reason`/`ownerPid`，`status` 动作把 `reason` 展开为诊断与处置指引（“注销并重新登录”终止本会话全部进程、释放句柄，比重启电脑快 → 结束可能持有句柄的后代进程 → 重启电脑；或改用其他端口）。中止时先**还原轮转掉的日志**（`.prev` → 原位）**再读其尾部**写 `logTail`——顺序反了会读到已被轮转走的空文件，使 logTail 恒为空（最需诊断信息的场景反而丢信息）；Windows 读日志需显式 `-Encoding UTF8`（服务日志为 UTF-8，PS5.1 的 `Get-Content` 缺省按 ANSI 读会使中文诊断乱码）
- **为何不能只靠 TCP 探测判「端口可用」**：僵尸套接字的监听队列仍会接受连接（`connect` 成功），故 TCP 探测会“看到端口可用”而空跑满 30s——真正的准绳是能否 `bind`；探测只用于等旧进程退出，占用判定看属主及其**存活性**
- **自杀时序**：工具先布置拉起器，再延迟 2.5s `process.exit`（延迟内工具结果先送达飞书/WS，自杀在后）；拉起器部署失败则不退出（服务保持运行）。工具结果为 **`endsTask` 任务终结声明**（见「核心Agent流程」）：本轮任务就此结束——引擎不再把结果回灌模型、不发起下一轮调用，模型不必为注定不会执行的后续动作生成内容（重启前的收尾动作须在调用前完成），后续工作由 `prompt` 续跑接续
- **重启后续跑（`prompt` / `session` 参数）**：重启会中断在途任务、且进程死后无人「接着干」——工具在自杀前把续跑请求写 `{tmpdir}/gebai-restart/continue.json`（会话/用户/角色/提示词/时间戳；**写失败即中止本次重启**，不留「重启成功但指令丢失」的半成品；拉起器部署失败则清理请求）。新服务启动后由 `consumeRestartContinuation`（`index.ts` 监听建立后的后台任务；仅本地模式、非测试进程）消费：等 `state.json` 确认「本次启动由重启拉起器承接」（pid 匹配本进程；拉起器已判失败或等待超时则按兜底仍执行——提示词是调用方明确意图，重启失败后人工恢复正是最需它不丢的场景）→ **先删请求再执行**（一次性语义，重复/并发启动不双跑）→ 校验会话存在 → `engine.run(会话, 用户, 提示词)`：提示词原样落盘为 user 消息并跑起完整 agent 循环（模型据此继续工作，前端页面已自动刷新可见）。请求过期（>10 分钟）则丢弃不执行，防陈旧请求被无关启动误跑；结论写 `continue.result.json`（**先写 `ok:null`「续跑执行中」再跑、完成后覆盖**——续跑任务可能一直跑到下次重启，不先写则中途被杀时 status 无任何痕迹），日志前缀 `[restart] …`
- **状态可查**：`action=status` 读最近一次重启状态（不重启）并附续跑请求/续跑结果；失败时把 `state.json` 的结构化字段展开为**中文诊断 + 处置指引**（`occupied` / `zombie-socket` / `ready-timeout` 三类各给对应措辞，并保留原始 JSON 便于机器读）；新服务日志在同目录 `server.log.*`；状态文件一律 UTF-8 无 BOM（Windows 拉起器用 `[IO.File]::WriteAllText` 写入——PS5.1 的 `Set-Content -Encoding UTF8` 会写入 BOM，消费方按无 BOM 预期解析）；就绪探测走服务自身根路径（直连 127.0.0.1，不经反向代理）
- **平台依赖**：Windows 需 PowerShell（系统内置）；Linux 需 bash + ss（iproute2，各发行版标配）+ curl；`requiresApproval: true`（重启中断在途任务，须用户确认）
- **拉起器代码的“落后一代”特性**：拉起器脚本由**重启那一刻运行中的进程**在内存里生成，因此改动 `restart.ts`（拉起器生成逻辑本身）后，**本次重启生成的脚本仍是改动前的逻辑，需再重启一次才生效**（新进程从磁盘加载新代码，它生成的脚本才是新的）。
  - 影响面仅限本文件自身——改其他任何文件都不受影响（新进程直接读新源码）；但这很容易让「已验证生效」的结论落空（历史上已连续两次因此误判）。
  - 故 `status` 附一行**代码新鲜度**（`launcherCodeFreshness`：比较磁盘上 `restart.ts` 的 mtime 与本进程启动时刻，容差 1s 避同一秒误报）：落后时输出可执行的提醒（“需再重启一次”），最新时一行确认，二进制/编译形态无源码可比则如实标注「无法判定」。
  - 设计取舍：**不**改为「重启时从磁盘现读源码生成脚本」——现行为是“由已验证的代码生成脚本”的**最后已知良好**保护（否则改成一半的 `restart.ts` 会让拉起器本身失效，服务起不来且无法再改）；代价（多一次重启）用一个可靠的可观测提示消除即可

### 核心Agent流程

单次消息处理的主循环（Agent Loop），状态机驱动、全程可观测：

```
session.prompt → 组装上下文（历史+系统提示词+临时文件提示）
    → LLM 流式生成 → 输出文本增量实时推送
    → 模型请求工具调用?
        ├─ 否 → 生成完成，回复落库
        └─ 是 → 解析工具调用（必填参数校验：schema required 缺失即回传缺参提示，不执行）
            → 门控（按调用顺序串行：重复检测/路由解析/审批姿态判定，说明性结果直接回传）
            → 同批并行执行（每个调用独立走 审批等待→执行→落盘；并发上限见常量参考）
            → 结果注入上下文
            → 循环回 LLM 生成（直至无工具调用；轮次不设上限，失控防护见重复检测）
```

关键机制：

- **工具调用循环**：一次 `session.prompt` 内模型可多轮调用工具（轮次不设上限，见常量参考），每轮结果作为消息追加回 LLM，直至生成最终回复。**例外——任务终结声明**：工具结果带 `endsTask: true`（`ToolResult.endsTask`，如 `restart_server` 的重启分支）时本批结果照常执行/落盘/推送，但不再回灌模型——引擎结束任务循环直接收尾（同批任一工具声明即终结，本轮全部调用照常完成），待办续做与收尾验证提醒等续轮同样跳过；本轮文本已随 assistant(toolCalls) 落盘，不再重复落盘为最终回复。供「执行即终结本任务/本进程」类工具使用（模型不必为注定不会执行的后续动作生成内容，免去无意义的一轮模型调用）；只读分支（如 `action=status`）不声明，仍回灌模型继续分析；子会话循环同规则（只结束本运行的循环）
- **同批工具并行执行**：单次模型响应返回的多个工具调用**并行执行**（互不等待——免审批工具不等待同批审批项、慢工具不阻塞快工具）；执行前先按调用顺序**串行门控**（重复检测/参数抢救/路由解析（含自动装载）/审批姿态等判定全部先行完成，缺参/未知工具/禁用/安全拦截等说明性结果直接回传——坏调用不打扰用户审批，**并推送 call+result 事件对**保证运行时卡片实时可见，与落盘历史一致）。并发上限 `MAX_PARALLEL_TOOLS`（超出按序排队）；结果按完成先后落盘（配对按 toolCallId，顺序不影响接口合法性；落盘经 promise 链串行化防并发写交错）；单个调用出错（取消/异常）不中断同批其他调用，池排空后统一补齐占位再上抛（与原串行收口语义一致）；**需严格串行的操作由模型编排**（系统提示词指引）：有依赖/同文件写改的操作用 js 按序编排或拆分多轮，不依赖同批调用顺序（同批共享门控时刻——运行中变更的会话 env（如自动审批开关）自下一轮起生效，不影响已门控的同批项）。主循环与子会话运行循环同规则（子会话存档条目按完成先后追加，仅影响回放顺序）
- **流式体验**：LLM 文本增量、工具调用事件、审批请求、执行结果均以服务端事件实时推送客户端（见「事件推送」），UI 呈现完整过程
- **中断与取消**：客户端可随时取消当前任务，服务端停止本轮循环并保留已生成内容；**停止即时打断进行中的工具执行**（工具执行包装统一收口：任务取消信号传递到 `Sandbox.exec`，脚本类子进程按进程树终止——Unix 杀进程组、Windows `taskkill /T`，工具自身据此交回中断前的输出 + 中断标记）；**用户中断在工具返回里显式体现**——被中断调用的工具结果一律带统一标记 `[interrupted by user]` 与「已被用户取消（任务已停止）」，据此与执行超时、引擎错误区分——中断返回**一律附上该调用被中断时已执行的时间**（取消信号到达时刻计时，`elapsedNote`：秒级一位小数、分钟级带分秒，如「执行被用户中断（已执行 12.3 秒），未取得工具返回」；未及执行/审批等待中被取消的调用不报时长，与「执行中被停」相区分）：① 工具在宽限期（`TOOL_ABORT_GRACE_MS`，默认 300ms，可经 `cancelGraceMs` 注入）内交回自身中断返回（`[interrupted by user]` / `[interrupted]` 标记 + 中断前已产生的输出）时**并入工具结果**（部分输出不丢失，不再被占位文案覆盖丢弃）；② 宽限内未交回（不响应取消信号的挂起工具）以统一标记**即时收口**（取消等待不被拖慢，结果写明「未取得工具返回」）；③ 未及执行/未完成的调用（审批等待中被取消、超出并行上限排队中、本轮收口兜底）同样补写带标记的占位（保持 assistant/tool 配对完整）；④ 等待型工具（`ask` 选项/填值、`show` 渲染、`page_capture` 捕获）被中断时返回「用户中断」文案而非超时文案；**取消统一解开全部挂起等待**（审批/选择/画图/捕获的等待 promise 同步 resolve——仅 abort 信号不会中断 `await`，不 resolve 会让 runLoop 永久挂起、任务收尾不完成、`isRunning` 残留，下一次 prompt 被「task already running」拒绝，表现为**中断后要发两次才能继续**；四个等待函数同时监听取消/超时信号，子Agent 超时同样立即解开）；**审批拒绝同样停止会话生成**（不再让模型调整方案继续执行，前端静默收尾并清理该会话残留审批卡片，取消/拒绝不渲染错误气泡）
- **重复检测**：模型**背靠背重复**调用相同工具（工具名+参数相同，最近 8 次签名窗口**尾部连续**第 3 次——其间没有任何其他调用）时中断该次执行并注入引导提示（「直接重发只会得到相同结果，若在等待外部状态变化请把等待/重试并入同一次调用，否则改用其他方法或直接给出最终回答」）；**间隔其他调用后重发同签名不判重复**——工具结果取决于世界状态而非仅参数（改完文件重跑同一测试、外部变更后复查），间隔操作说明模型在响应结果，是合法的「复查确认」；中断超过 2 次判定模型陷入重复，终止工具循环提前返回（仍返回最后产出文本，保持会话消息序列完整）；待办续做中模型回复与上上轮完全相同（纯文本）时，续做提醒追加「请勿复述」提示。**同批重复签名只计一次**（同批并行发出相同调用是有意扇出——发出时尚未见任何结果，不存在「无视结果重试」，见「同批工具并行执行」）；跨轮连续重发（已见过结果且其间无其他调用）照常累积判定
- **错误恢复**：LLM 请求失败/超时自动退避重试；工具执行失败将错误注入上下文，引导模型自行修正；**工具执行超时不结束任务**——脚本类工具先由脚本执行超时（`sh`/`py` 可经 `timeout` 参数（秒，默认 300、上限 540）调整个别执行超时，缺省 5 分钟）杀进程并返回超时结果，引擎层 9 分钟兜底（`TOOL_TIMEOUT_MS`）覆盖不响应超时的工具（如网络请求挂起），超时均作为工具结果返回给模型自行调整方案
- **必填参数校验（两循环同规则）**：模型漏传工具参数时（schema `required` 声明的键缺失/null）**不执行工具**，直接回传「工具 X 缺少必填参数: …」——缺失值若进工具会被 `String(undefined)` 成字面量 `"undefined"` 落进路径解析，报出与真实原因无关的 ENOENT（如 `edit` 漏传 `path` → `tmp\undefined`），模型无法从报错定位到「少传了参数」；校验点在审批门**之前**（坏调用不打扰用户审批），主循环与子会话循环一致生效
- **大文件分段写入与截断抢救**：生成大文件时模型被迫单次输出全部内容，易触发接口超时与输出上限截断（参数 JSON 不完整）。三层防护——①**分段写入载体**：`write` 支持 `append:true` 追加模式，工具描述与 code 子Agent 提示词内置指引（约 300 行以上分多段写，每段 200~300 行），单次响应短、不再超时；②**截断检测**：provider 如实传出结束原因（OpenAI `finish_reason=length`、Anthropic `max_tokens`、Responses `response.incomplete`→`length`，不再吞为正常 stop），解析失败的工具参数携带原始全文（`toolCall.raw`，原仅 500 字符片段）；③**抢救落盘**：引擎对 write 类工具（`write`/`{agent}_write`）容错解析原始参数前缀（`salvageWriteArgs`：提取完整 `path` 与已生成 `content` 前缀，尾部不完整转义丢弃、非法转义放弃），先把已生成部分落盘并登记已读，工具结果引导模型 `append:true` 续写剩余部分（从已写入之后继续，不重新生成全量）——失败轮次转化为进度；未抢救（非 write 工具/前缀解析失败/防盲覆盖守卫拒绝）时回传截断专属引导（拆小操作/分段写入），不再笼统提示「重新输出合法 JSON」（重新整体输出只会再次截断）。单次响应输出上限可配（`GEBAI_LLM_MAX_OUTPUT_TOKENS` 任务级覆盖，Anthropic 缺省 8192）
- **防盲写守卫（已读追踪）**：`write` 覆盖/追加、`edit`、`patch` 修改「已存在但本会话未读过」的文件均被拒绝（会话级 fileGuard 已读追踪，`read`/`edit`/`patch`/`write` 成功即登记），引导模型先 `read` 再改——拒绝作为工具结果返回，模型一轮自愈，不中断任务（见「防盲写守卫」）
- **上下文管理**：窗口剩余不足以支撑一次回复时自动压缩（工具输出截断 → 旧消息摘要 → 溢出硬护栏，见「上下文保护」），也支持用户主动压缩
- **审批超时**：审批请求超时（见常量参考）自动拒绝该次调用并提示模型调整，避免任务悬挂

### 会话输入队列（前端实现）

会话任务运行期间到达的用户输入**排队等待，当前任务结束（完成/取消/出错）后自动按序发送执行**，无需等待或放弃输入。队列为**前端本地状态**（`packages/web/src/queue.ts`，按会话隔离、纯内存——服务端无感知、无协议扩展，刷新页面/关闭浏览器后队列丢失）：

- **入队**：运行中提交（Enter/发送；运行中发送按钮带草稿时为箭头形态）不打断当前任务，输入进本会话排队条（不进消息流、不发 WS）；附件在入队时即上传会话 `tmp/`（队列条目保存引用，执行时直用）
- **自动执行（drain）**：任务流收尾（`consumeTaskStream` finally，覆盖完成/取消/出错）后由前端按序取队首发送下一条——与直接发送走同一 `sendPrompt` 任务流（用户消息在执行时渲染，`messageId` 入队时生成供撤回/反馈定位）；入队后也触发一次 drain（覆盖「运行恰在入队前结束」的竞态窗口），drain 幂等（会话运行中空转）
- **中断插入（Ctrl+Enter / 排队条 ⚡）**：新输入或已排队输入插队首并取消当前任务（`session.cancel`），当前任务收尾后立即执行队首；空闲时等同直接发送
- **编辑/撤回**：未执行的排队输入可改内容、可撤回（本地队列操作）；已执行的排队输入即普通用户消息（走消息撤回 `truncate`）
- **停止语义**：停止按钮只取消当前任务，队列继续（排队输入是独立意图，撤回由用户逐条控制）；要全部停止先撤回排队项
- **多会话**：队列按会话隔离（切走不丢、切回恢复，随会话删除清理）；**排队条随会话切换重渲染**（`loadMessages` 与发送按钮同点对齐——缺此重渲时排队条停留旧会话内容、后台队列事件触发重渲后又整体消失，切回会话的排队项不可见）；后台会话的排队输入同样在其任务结束后自动执行（发送与流式渲染不依赖当前视图）

### 多模态支持

用户可在会话中发送或引用多模态内容（图片、音频、视频、文档），模型视能力理解处理：

- **消息附件**：`session.prompt` 支持携带附件（图片/音频/视频/PDF 等），附件先上传到会话 `tmp/`，消息中携带引用路径与 MIME 类型（`AttachmentRef.path` 为**会话根相对逻辑路径**，如 `tmp/foo.png`，模型/工具/前端统一按此解析；历史兼容绝对路径附件——沙箱拒绝解析时自动降级为文本说明）
- **附件 → 模型内容**（`AgentEngine.loadHistory` 重建）：图片附件（png/jpeg/gif/webp）且主模型声明多模态能力（`GEBAI_LLM_MULTIMODAL=true`）且 ≤8MB 时**压缩后**（见「图片压缩时序」）base64 内联为统一 `image` 块（携带 `path/name/size` 元数据，size 为原始体积），图片块后附尺寸说明 text 块（原始/压缩后尺寸与体积）；其余（非图片/超限/文件缺失/模型无多模态）降级为文本说明（路径 + MIME + 大小 + 视觉子代理指引），由模型决定装载 vision 子代理（`vision_analyze`）/`read` 等工具处理
- **工具结果图片内联（`read` 图片直读）**：`read` 读取白名单图片（png/jpg/jpeg/gif/webp）**不按文本解码**（乱码无意义）——二进制读入后经 `ToolResult.images` 交引擎处理：主模型多模态时图片以统一 `image` 块**内联进工具结果消息**（模型直接可见，无需 vision 等其他工具），轻量引用（`Message.images`：绝对路径/原始路径/MIME，不含 base64）随工具消息落盘、`loadHistory` 历史重建按引用重读内联；非多模态不内联，工具返回说明 + 视觉子代理指引。svg 为文本按正常读取；bmp 不在白名单（返回转换引导）。**序列化**：OpenAI 兼容 `role:"tool"` 的 content 为块数组（text + `image_url`）；Anthropic `tool_result` content 为块数组（text + image，官方形态）；Responses 的 `function_call_output` 仅接受字符串——文本拼入输出，图片块转为紧随的 user 消息内容部件（同轮可见）
- **接口拒绝自动降级**：主模型声明多模态但接口实际拒绝图片块（HTTP 4xx）时，引擎将图片块一次性降级为文本说明后重试（附件图片与工具结果图片同路径降级，说明文案区分来源；模型可改走 `vision` 工具），实现「无多模态能力自动降级」兜底
- **图片压缩时序（原图保存、发送时压缩）**：粘贴/上传/URL 引用一律按**原图**存入会话 `tmp/`（无损保留，模型/本地 CV 工具/用户取用的都是原图）；仅在**发送给大模型前**由服务端压缩（`@gebai/agents` core/shared/image-resize.ts，Bun.Image 原生编解码）：非 GIF 且长边 >1280px 或体积 >2MB 时等比缩放（长边 1280，体积仍超限按 0.8 倍迭代，下限 0.25 倍）后**同格式**重编码（JPEG/WebP 质量 0.85，PNG 保持 PNG）；未超限/解码失败原样发送。压缩不回写文件（原图不动），并随图片附尺寸说明（`[图片已压缩: 原始 W×H X → w×h x（坐标/细节按比例对应原图）]`，未压缩标注 `[图片 W×H x，未压缩]`）——多模态内联（引擎附件/工具结果图片）与 vision 子代理 analyze 同源共用。GIF 为动画帧格式不压缩
- **多模态消息格式**：统一内部消息模型（文本 + 内容块数组），`provider.chat()` 按接口规范转换：
  - OpenAI 兼容：`content` 为 `[{ type: "text" | "image_url", ... }]` 结构（user/assistant 消息与 tool 结果消息同构）
  - Anthropic：`content` 为 `[{ type: "text" | "image" | "document", ... }]` 结构（`tool_result` 内容同为块数组）
- **图片**：支持本地上传/粘贴/URL 引用，原图保存到会话 `tmp/`（不压缩），发送给大模型前服务端压缩（见「图片压缩时序」），按 Provider 要求编码（base64 data URL 或外链）
- **音频/视频**：上传到会话 `tmp/` 并转为可引用文件；Provider 不支持直接理解时降级为「附文件路径 + 文件名」文本提示（由工具/脚本代处理，如转写）
- **文档（PDF/Office）**：上传后可经工具（如 `read`/脚本）抽取文本喂给模型，或按 Provider 原生 `document` 块传递（能力自动探测）
- **能力探测**：模型声明 `multimodal` 能力（`GEBAI_LLM_MULTIMODAL=true` 显式声明，默认 false），无多模态能力的模型收到附件时提示用户或自动降级（文本说明 + 视觉子代理指引）
- **UI**：消息输入支持拖拽/粘贴/选择附件，预览缩略图，附件随消息一并持久化（会话目录内）
- **工具产物**：Agent 生成的图片/文件同样可回传到会话供查看与下载（见「会话临时文件查看与下载」）

#### 视觉工具 `vision`（已移除——视觉能力统一经 vision 子代理）

**全局 `vision` 工具已移除**（架构决策：视觉相关能力统一收进 vision 子代理，单一入口）：主会话需要视觉分析时 `agent_load` 装载 vision（或模型调用 `vision_*` 时路由自愈自动装载）；子Agent 经 `dependencies: ["vision"]` 依赖声明连带装载（如 self_optimize）；desktop/playwright 域内保留各自的 CV 工具（缺省图像源便利，见「视觉能力分层与子代理复用边界」）。旧形态（`index.ts` 组装层注册全局 vision、subsession_run 子会话继承块同源注册、父会话内 `vision` 与 `vision_analyze` 双份同源 schema 并存）一并退场。

视觉语义分析的实现已迁 `@gebai/agents` core/shared/vision.ts（`makeVisionTool` 工厂 + provider 解析），vision 子代理 def 以 `name="analyze"` 复用——工厂不再是全局注册的专属物，而是子代理工具的实现源：

- **provider 解析**：组装层 `setVisionProviderGetter` 注入（`boot/compose.ts`，`GEBAI_VISION_*` 外挂视觉模型 → 多模态主模型回落；任务级 env 覆盖生效）
- **模型选择**：配置 `GEBAI_VISION_MODEL` 后使用独立视觉 Provider（`GEBAI_VISION_*`，接口地址/密钥/类型缺省继承主模型）；未配置时回落到主模型（须显式声明多模态能力，`GEBAI_LLM_MULTIMODAL=true`，默认 false），两者皆不可用则返回配置提示
- **传输**：图片以 base64 内联（统一内部图片块 → OpenAI `image_url` data URL / Anthropic base64 `image` 块）随文本目标一起调用视觉模型，单图上限见常量参考；**发送前服务端压缩**（见「图片压缩时序」，原图不动）并把原始/压缩尺寸写入文本目标一并告知模型；**超时控制**（`timeout` 参数，默认 30 秒、钳制 1~300）：手动定时器 + AbortSignal 与用户取消信号合并，超时返回提示引导改用本地视觉工具（ocr/locate/locate_image/detect），用户取消原样上抛；返回分析文本（超长走截断保护），并携带 `image` 内容块供 UI 展示
- **降级指引**：图片附件/工具图片块未内联时（模型无多模态能力、接口拒绝图片、超限），文本说明指引模型走 `vision_analyze`（vision 子代理）——engine 的附件说明、read 非多模态分支、图片块降级重试同此措辞

#### 小模型识别（本地 CV 推理，`@gebai/agents` core/cv）

与 `vision`（外部视觉模型服务）互补的**本地小模型图像识别**：非模型服务的 ONNX 推理（PP-OCR 中英文 OCR 为主力，可选自备 YOLO 检测），onnxruntime-web **wasm 进程内**运行——离线可用、不耗模型配额、毫秒~秒级延迟，且能给出**精确像素坐标**（LLM 视觉估坐标不准，这是桌面自动化的刚需）。基建在 `@gebai/agents` core/cv 域，不依赖子Agent 定义存在；**工具消费层为共享工厂 `@gebai/agents` core/shared/cv-analysis.ts**（ocr/locate/locate_image 三工具 `createCvAnalysisTools` + detect 目标检测 `createDetectTool`，均注入缺省图像源与文案）：desktop 子Agent 注入「现截宿主机屏幕 + 本地模式闸门」（`desktop_ocr`/`desktop_locate`/`desktop_locate_image`/`desktop_detect`，见 desktop 小节）；playwright 子Agent 注入「共享桥接截当前页视口 + 无闸门」（`playwright_ocr`/`playwright_locate`/`playwright_locate_image`，见 playwright 小节）；vision 子代理注入「无缺省源（image 必填）+ 无闸门」（`vision_ocr`/`vision_locate`/`vision_locate_image`/`vision_detect`，见 vision 小节）——同一识别逻辑三个消费方零复制（视觉能力分层与子代理复用边界，见「小模型识别」末条与 vision 子Agent 小节）。

- **运行时加载（不进 bundle 图）**：onnxruntime-web 经运行时动态 import dist 入口（拼接包名 + `Bun.resolveSync` + file URL，与 playwright 模块同款规避——bundle 注册表启动安全；裸导入在 node 条件下会指向 onnxruntime-node 原生绑定，故必须走 dist 文件直连）；单线程 wasm（`numThreads=1`，无 SharedArrayBuffer/worker 依赖）。单二进制形态从内嵌产物（`core/cv.embedded.generated.json`，构建脚本 `packages/server/scripts/build-cv-embed.ts` 生成）释放到资源目录——模型 → `{GEBAI_HOME}/resources/models/cv/ocr/`、ort 运行时 → `{GEBAI_HOME}/resources/vendor/cv/`，相对结构与资源子仓库一致（版本 marker + 逐文件写入 + 模型已存在则保留 + 防穿越 + 并发共享，与 pwcore 同思路）；源码/部署形态解析 node_modules
- **模型交付**：构建时下载 PP-OCRv4 mobile（det ~4.7MB + rec ~10.9MB，onnxruntime-web Apache-2.0/MIT 许可）内嵌进产物——下载源 `GEBAI_CV_MODEL_BASE` 可覆写（内网镜像），`{GEBAI_HOME}/resources/models/cv/ocr/` 已有文件跳过下载（资源目录离线自备）；字典从 rec 模型内嵌的 `character` 元数据提取（RapidOCR 约定）。下载失败生成空清单——构建不失败，运行时给配置指引。**资源获取**：主仓库带下载清单与脚本（`scripts/resources.manifest.json` + `scripts/download-resources.ts`，`bun run resources:download`）——逐文件列模型地址（modelscope / hf-mirror / huggingface 多源轮换，`.part` 临时文件 + Range 断点续传，完成后按 size/sha256 校验），按清单相对路径铺开到 `{GEBAI_HOME}/resources/` 即得与资源子仓库同构的目录（`--check` 只校验、`--only` 限定条目、`--source` / `GEBAI_RESOURCE_SOURCE` 指定来源优先级、`--force` 重下、`--skip-vendor` 跳过 vendor 依赖；清单条目可带 `derive` 说明备用获取途径——从 rec 元数据提取字典、从权重导出 ONNX）。运行时模型目录解析：`GEBAI_CV_MODELS_DIR`（任务级 env 可覆盖）→ 二进制形态释放的内嵌模型目录 → `{GEBAI_HOME}/resources/models/cv/ocr/`（资源目录）；目录内固定三件套 `det.onnx`/`rec.onnx`/`dict.txt`
- **推理形态**：惰性共享单例（首次调用初始化 ort + 加载模型，~1-3s）；session 按模型文件路径+大小缓存（文件变更自动重建）；**wasm 路径全进程推理串行**（Promise 链互斥，防同批扇出并发争抢）。wasm CPU 推理同步阻塞事件循环（单次约 0.5-2s）——desktop（本地模式、单管理员）与 playwright 视口截图识别消费可接受（串行互斥保证并发安全）；若未来多租户高并发下成为吞吐瓶颈，OCR 亦可迁入 sidecar（协议为通用 ONNX 会话运行器）
- **分层后端（GPU sidecar ↔ wasm，全部 CV 推理共用，`cv.ts` `runTiered` + `sidecar.ts` + `cv-driver.mjs`）**：检测类重模型（如 YOLO11-L@1280 ≈ 350 GFLOPs）在 wasm 单线程下数十秒级不可用，OCR（det+rec，全屏大图 wasm 单次可达数秒且阻塞事件循环）同样受益——spawn 常驻 `node cv-driver.mjs` 子进程跑 **onnxruntime-node** 原生推理（进程管理仿浏览器桥接：惰性启动、请求超时杀进程重启、stderr 环形缓冲、全进程共享单例）。**EP 逐级探测显式上报**：auto 下 Windows dml（DirectML，任意 DX12 显卡免装依赖）→ cuda、macOS coreml → cuda、全失败回落 cpu（多线程）兜底，session.create 返回实际落地 EP（不静默降级），工具输出如实标注 `后端 sidecar:dml/...`。协议：行分隔 JSON 头 + 定长原始字节帧（张量不走 JSON 化，1280 输入张量 ~20MB/输出 ~32MB 本机管道毫秒级）；前后处理（letterbox/NMS/DB/CTC）留在 Bun 进程，driver 是通用 ONNX 会话运行器（init/session.create/session.run 三 op，`runModel` 按模型键缓存会话——检测与 OCR det/rec 共用同一入口）；**OCR sidecar 路径不加载 onnxruntime-web**（模型目录经 `resolveCvRuntime` 后端无关解析）；模板匹配为纯 JS 计算（非 ONNX）维持进程内。后端选择：`GEBAI_CV_BACKEND` 全局 > `GEBAI_CV_DETECT_BACKEND`/`GEBAI_CV_OCR_BACKEND` 细分覆盖 > auto；工具输出（desktop_ocr/desktop_detect）如实标注实际后端。**onnxruntime-node 不随构建内嵌**（体积/许可）：按 `GEBAI_CV_ORT_NODE_DIR` → `{GEBAI_HOME}/resources/vendor/node_modules/onnxruntime-node`（**资源目录约定位置**——模型/运行时依赖集中存放于项目下独立 git 仓库 `resources/`，主仓库 gitignore 隔离；npm 整包依赖闭包放入即生效，onnxruntime-common 等依赖经 node 标准向上查找可用）→ `{GEBAI_HOME}/vendor` → node_modules 解析，不可解析即 sidecar 不可用；运行期失败（超时/崩溃）**毒化**——本进程生命周期内不再重试，回落 wasm（细分/全局 sidecar 显式指定时不回落、错误如实上抛）；父进程退出时杀子进程（`process.exit` 不保证关闭 stdio 管道，一次性脚本会留孤儿 node 进程持 GPU 会话，实测修复）。二进制形态驱动脚本内嵌（`cvdriver.embedded.generated.json`，`packages/server/scripts/build-cvdriver-embed.ts`）释放到 `{GEBAI_HOME}/resources/vendor/cv/`，dist 形态由 build-subagents 复制到产物目录。**两条 sidecar 通道的后端命名区分（均如实反映「哪个执行体 + 哪个 EP」）**：本条为 node GPU sidecar，后端名 `sidecar:<ep>`（EP 由 `session.create` 返回实际落地值）；消费层 `cv-analysis` 的 sidecar-first 委托（优先调 Python `vision` 边车的 `vision_detect`/`vision_ocr`，见 vision 小节）后端名 `vision-sidecar:<ep>`——EP 由边车从 onnxruntime 会话实际上报（`keqing/python/vision/tools.py` 的 `provider` 字段，如 `cpu`/`cuda`；未上报标 `unknown` 而不臆测）；进程内回落为 `wasm-cpu`。三个前缀互不重叠，避免把 Python 边车误读为 node GPU sidecar
- **OCR rec 批处理（`GEBAI_CV_REC_BATCH`，1..32，缺省 8）**：rec 前处理对每条裁剪做 letterbox（固定 48×320），**所有样本形状一致**，故把 N 条裁剪拼成 `[N,3,48,320]` 一次推理（侧车与进程内两条路径均已批处理）——逐行推理时推理调用次数 = 文本行数（4K 全屏可达数十上百行），批处理把这些次数整除，同时省掉每行的 IPC 往返。实测（本机、真实模型、走 sidecar+DML）：文字图 29 行 **1.81x**（1664ms → 919ms）、密集小块图 420 框 **12.5x**（2542ms → 204ms），**逐行识别文本与逐条推理完全一致**（批维不影响结果）；纯模型层 CPU EP：batch=8 相对逐条 1.91x、batch=16 2.19x。批过大抬高单次延迟与内存，故钉上限 32；批大小经会话级 env 可调（调优/回退用）
- **前后处理（纯函数，`image.ts`/`ocr.ts`/`detect.ts`/`onnx-meta.ts`）**：PNG 解码自研（node:zlib inflate + 逆滤波，覆盖 8-bit 非隔行灰度/RGB/调色板/带 alpha——截图场景全覆盖，16-bit/隔行/JPEG 明确报错）；det 前处理等比缩放（长边 ≤960）+ 32 倍数零填充，DB 后处理（阈值 0.3 → 3x3 膨胀合并碎片 → 8-连通域 → 框内核心像素均值置信度 → unclip 外扩 → 阅读序排序）；rec 前处理 48 高等比缩放 + 320 宽零填充，CTC 贪心解码（blank=index 0，字典 = `[blank] + 6623 字符 + 空格`，对齐 RapidOCR 的 PP-OCR ONNX 部署约定）；YOLO letterbox（缺省 640、114 灰，**尺寸按模型元数据/`GEBAI_CV_DETECT_SIZE` 自适应**）+ NMS（IoU 缺省 0.45，**工具 `iou` 参数可配**——密集小控件场景调低至 0.1），兼容 v8 `[1,4+nc,N]` 与 v5 `[1,N,5+nc]` 两种输出形态（按形状自动识别）；**检测框×OCR 行配对**（`pairObjectsWithText`：行中心落在框内即归属、多行按阅读序拼接——「组件类型+文本」的结构化元素表，完整屏幕解析的本地拼装）；`onnx-meta.ts` 直接从模型字节解析 ONNX metadata_props（顶层 field 14，不依赖 ort 运行时——wasm/sidecar 两条后端同一口径），提取 ultralytics 约定的 `imgsz`（输入尺寸）与 `names`（类别表，省标签文件）；**坐标一律还原到输入图像原始像素系**（region 偏移 + 缩放比例），desktop 供 `mouse_click`、playwright 供视口 `document.elementFromPoint` 直接使用
- **目标检测（`desktop_detect`）**：模型不内嵌——**缺省自动发现 `{GEBAI_HOME}/resources/models/cv/detect/` 下唯一 `.onnx`（资源子仓库整体放入 `{GEBAI_HOME}/resources/` 即零配置可用：检测模型自动发现 + GPU sidecar 原生包自动解析）**，或 `GEBAI_CV_DETECT_MODEL` 显式指定（多模型共存时必须显式，否则列出候选报错；官方 ScreenParser 权重可由 `bun run resources:download` 拉取，`.pt → ONNX` 导出命令见清单条目 `derive`）；ultralytics 导出（YOLO11/v8）即插即用（元数据自适应尺寸与类别）；非 ultralytics 来源设 `GEBAI_CV_DETECT_LABELS`（每行一类别）；COCO 预训练类别对 UI 操作无意义，面向 UI 组件检测模型（如 ScreenParser 类）或自训练图标/控件模型；推理走「检测分层后端」（上一条）
- **模板匹配（`template.ts` 纯函数，非 ONNX 推理）**：NCC（归一化互相关）同尺寸匹配，补「文字走 OCR、检测需自训 YOLO」之间的图标/控件定位空白（`desktop_locate_image`/`playwright_locate_image`/`vision_locate_image` 消费，零训练）；两阶段加速——box 降采样**相位偏移粗扫**（origin {0,s/2}² 多相位防高频内容错位去相关，粗扫门限 0.4 仅引导）+ 全分辨率邻域精化，窗口均值/方差经「和+平方和」积分图 O(1) 求取；非极大值抑制后至多 5 个匹配（2560x1440 全屏 + 48px 模板实测约 350ms）；不做缩放不变（模板需与目标同一显示环境同尺寸）
- **视觉能力分层与子代理复用边界**：视觉能力按「是否与图像源环境耦合」分层，两种复用方式（其他子代理用视觉能力的标准路径）：
  - **方式一（依赖声明，适合通用图片能力）**：`dependencies: ["vision"]` 声明依赖 vision 子代理（见 vision 小节），装载/subsession_run 预加载自动连带，以 `vision_*` 命名空间直接调用（analyze 语义分析 / ocr / locate / locate_image / detect）——能力与实现单源维护在 vision def，依赖方零复刻；适合「输入=图片文件路径、无环境耦合」的能力复用
  - **方式二（自己的工具名 + 共用实现，适合环境耦合能力）**：消费方在域内提供自己的工具（desktop_*/playwright_*），实现经共享工厂（cv-analysis / makeVisionTool）注入各自的**缺省图像源**（现截屏幕/当前页视口）、**闸门**（本地模式）与**文案**（坐标系语义/消费提示）——工具名、坐标系、审批姿态是域内语义，识别/分析实现单源共用；适合「省略 image 有环境含义、坐标映射回特定像素系」的能力
  - **判定标准**：能力离了消费方环境是否仍成立——「对一张 PNG 做 OCR」离了 desktop/playwright 依旧成立（方式一收进 vision 子代理）；「省略 image 现截屏幕、坐标可直接 mouse_click」离了 desktop 无意义（方式二留在域内）。desktop/playwright 域内已提供全套 CV 工具，语义分析兜底按需 agent_load 装载 vision 子代理，**不声明依赖 vision**（避免 4 个 image-必填的重叠工具常驻上下文，徒增选择噪音）；self_optimize 声明依赖（subsession_run 隔离子会话即使不继承全局工具也有视觉能力）；**全局 `vision` 工具已移除**——引擎图片附件/read 非多模态/图片块降级的指引文案统一改指 `vision_analyze`（vision 子代理，agent_load 装载或路由自愈）


### 多用户隔离与安全

服务模式下，所有数据与执行环境按 **用户 → 会话** 两级隔离：

#### 认证与鉴权
- **登录鉴权**：服务模式启用登录（用户名/密码），密码使用加盐哈希存储（scrypt，不落明文）
- **开放注册（可审批）**：服务模式登录页提供「注册账号」入口（`POST /api/v1/auth/register`，公开端点）；**注册用户恒为普通角色**（不可注册 admin，admin 唯一入口是启动参数 `GEBAI_ADMIN_PASSWORD_HASH`）；**用户名经 `normalizeUsername` 规范化**：小写折叠（Windows 文件系统不区分大小写，`Alice` 与 `alice` 不得形成两个注册表键共享同一物理目录）+ 格式白名单 `[a-z0-9][a-z0-9_-]*`（1-32 字符，拒绝 `..`/`.`/路径分隔符/控制字符防用户目录穿越）+ 拒绝 Windows 保留设备名（con/nul/com1 等）；重名/非法返回 400；本地模式不开放（404）。**审批模式由 `GEBAI_SIGNUP_MODE` 控制**：`open`（默认）=注册即登录（签发令牌）；`approval`=注册用户置 `disabled+pending` 待审（不签发令牌、不可登录），登录页提示「等待管理员审批」，admin 在用户管理页**批准**（`PATCH {disabled:false, pending:false}` 启用）或**拒绝**（删除用户）
- **外部身份扩展点**：服务模式下支持「外部身份 → GEBAI 令牌」兑换（`POST /api/v1/auth/exchange`），同源部署网站可直接复用其本地登录态（localStorage）作为 GEBAI 用户，免二次登录。验证器可插拔（`external-auth.ts`）：配置 `GEBAI_EXTERNAL_AUTH_SECRET` 走 HMAC 验签（凭证 `{exp}.{sig}`，±10 分钟窗口 + **一次性消费**——签名无 nonce，已消费凭证摘要缓存防窗口内重放），配置 `GEBAI_EXTERNAL_AUTH_URL` 走 HTTP 回调（业务系统自行校验凭证真实性；**回调返回的用户名必须与请求一致**——允许规范化去空白，拒绝不一致，防宽松回调被利用接管任意用户）；两者互斥，验证失败统一 401 不泄露原因。**外部身份禁止命中本地 admin 账号**（外部系统同名 `admin` 用户兑换不得继承本地 admin 角色——admin 唯一入口是启动参数口令）。用户映射：`GEBAI_EXTERNAL_AUTH_AUTOCREATE=true`（默认）自动创建普通角色用户（随机密码不可密码登录），`false` 仅允许已存在用户；外部用户名经 `normalizeUsername` 规范化（同上：小写折叠 + 白名单 + 保留名拒绝）。前端注入：Web UI 启动时若本地无令牌，先取 URL 参数 `?gb_ext_username=&gb_ext_credential=`，再按 `GEBAI_EXTERNAL_AUTH_STORAGE_KEY` 直读宿主 localStorage（JSON 或 `username:credential` 字符串）；`GET /api/v1/auth/external-config` 供前端探测启用状态（不泄露密钥）
- **登录限流**：连续失败 5 次锁定该用户名 60 秒（内存计数），防在线爆破；**登录/兑换/注册端点另加令牌桶**（REST：登录/兑换全局桶 60 突发/2 每秒 + 来源桶 10 突发/0.2 每秒，注册独立桶 30/0.5 + 10/0.1；WS `auth.login` 密码路径同款桶 30/1——scrypt 即使异步化仍耗 CPU，轮换用户名即可绕过按用户名锁定，防 CPU DoS 放大器；`GEBAI_TRUST_PROXY=true` 时 REST 按 `X-Forwarded-For` 首段区分来源）
- **令牌机制**：登录后签发会话令牌（HMAC 签名，7 天 TTL），后续 WebSocket 连接携带令牌建立用户上下文；**令牌表持久化到 `{GEBAI_HOME}/auth-tokens.json`**（签发/撤销/过期清理时落盘，进程重启后已签发令牌仍有效——单机部署下重启不掉线；过期令牌在 authorize/保存时顺带清理，不无界增长）
- **WS 未登录拦截**：服务模式下未登录（无令牌）的 WS 连接仅允许 `auth.login`，其余消息一律拒绝
- **跨站来源防护（本地/桌面免登录形态）**：WebSocket 不受同源策略约束且本地模式免登录——恶意网页可直连 `ws://127.0.0.1:*` 以 admin 身份建会话执行命令（REST 通道因 CORS `*` 同样暴露）。防护：WS upgrade 与 REST `/api/*` 均校验 **Origin 与 Host 同源**（浏览器发起的跨站请求必带 Origin，不同源即 403；非浏览器客户端无 Origin 不受限）。**两侧豁免面一致**（`wsOriginAllowed` 与 REST CORS 中间件同规则）：服务模式（令牌鉴权）不拦、显式配置 `GEBAI_CORS_ORIGINS`（不含 `*`）视为有意开放的跨源白名单不拦；仅「本地/桌面免登录形态 + 缺省 `*`」才要求同源——避免「白名单放开了 REST 却连不上 WS」「服务模式异域前端能调 REST 不能开 WS」的配置陷阱
- **WS 全局子Agent 装载/卸载管理员门槛**：`sub_agent.load`/`sub_agent.unload` 不带 `sessionId` 的**全局形态**（变更所有用户的工具注册面）服务模式下仅 admin（与 REST 工具启停同门槛）；带 `sessionId` 的会话级装载/卸载不受限（只影响本人会话）；模型侧 `agent_load` 装载进当前会话（会话级引用，见「子Agent」引用计数）
- **用户管理**：支持管理员创建/禁用用户；每个用户独立命名空间（`users/{user}/`）；服务模式 admin 用户通过启动参数 `GEBAI_ADMIN_PASSWORD_HASH` 引导（**设置则启用并覆盖其密码哈希，不设置则禁用**，启动参数为权威配置每次启动重置；admin 被禁用时**普通用户可经注册页自助注册使用**（普通角色），管理员能力须部署方设置哈希启用）；**REST 与 WS 双通道的用户管理端点均校验管理员角色**（非管理员一律 403，防普通用户提权/越权管理）
- **请求校验**：所有会话操作先解析令牌确定用户，再校验会话归属（含取消任务、审批决策等控制类操作）；**会话 ID 格式白名单**（32 位小写 hex，`randomUUID` 去连字符）在存储层 `sessionPath`/`store` 与 REST 中间件、WS 消息入口四层强制——畸形/穿越形态（`../`、路径分隔符）一律 400/错误应答，从根上杜绝会话 ID 拼路径形成的目录穿越
- **附件安全**：上传/引用的附件文件名仅取 basename 并拒绝路径穿越；沙箱约束用户附件来源路径限定会话目录内（防任意文件读取与越界写入），豁免用户（admin，特权用户）不限制来源

#### 数据与执行隔离
- **数据隔离**：会话、临时文件、截断内容均存储于 `{GEBAI_HOME}/users/{user}/` 下，用户之间互不可见
- **会话归属校验**：任何会话操作（读取、切换、发送、删除）均校验当前登录用户与会话所有者一致，禁止跨用户访问
- **路径沙箱**：服务端部署模式下，文件类工具（`read`/`write` 等）的相对路径统一以当前会话 `tmp/` 为基准（见「路径基准」），仅允许访问该目录内路径，对目录外路径一律拒绝，防止路径穿越（`../`、绝对路径、符号链接）；**桌面/本地浏览器模式默认不启用目录限制**，可访问本机任意路径（由 `GEBAI_SANDBOX` 控制，默认按运行形态自动选择）；**仅本地模式默认用户（id=admin）豁免路径沙箱**（`Sandbox.isExempt`/`enforcedFor(user)` 按用户判定：本地模式是操作者本人机器、豁免用户即使沙箱启用也按本地模式放开；**服务模式一律沙箱**——admin 也不豁免，多租户边界一致）；**`GEBAI_SANDBOX=off` 与服务模式互斥**（启动即拒绝：多租户 + 关沙箱会让 files 接口回退裸 `resolve` 可 `../` 越界读任意路径，安全配置错误在启动期暴露）
- **脚本隔离**：`sh`/`py`/`js` 脚本以独立子进程运行，工作目录限定为当前会话 `tmp/`（**spawn 前自动创建缺失目录**——纯命令会话无附件/写文件时 `tmp/` 从未创建，否则 `sh`/`js`/`ls` 相对路径全体 ENOENT；`Sandbox.exec`/`spawnBackground` 对 cwd 与日志父目录 `mkdir recursive`，引擎任务启动时同规则兜底），环境变量注入当前用户上下文；**沙箱（服务端部署）模式与安全模式下脚本子进程环境剔除敏感变量**（按完整结尾单词匹配 `_KEY`/`_TOKEN`/`_SECRET`/`_HASH`/`_KEY_ID`/`_CREDENTIAL`/`_AUTH`/`DATABASE_URL` 及裸名 `TOKEN`/`SECRET`/`PASSWORD` 等形态，防止任意用户经脚本 `env`/读取外泄服务端全局密钥，也防安全模式下 js 读 `process.env` 拿到密钥；**豁免用户（admin，特权用户）不剔除**，本地模式不剔除（安全模式仍剔除）；`js` 的会话上下文注入 `ctx.env` 同规则脱敏）；JS/TS 脚本优先通过**内置运行时自执行**（见「脚本执行环境」），不依赖宿主机安装 bun/node；**服务模式脚本运行根收敛到会话目录**（`GEBAI_SCRIPT_ISOLATION`，默认 `auto`，见「多用户隔离与安全·数据与执行隔离」）
- **脚本运行根（会话隔离，服务模式默认且强制开启）**：服务模式下脚本（`sh`/`py`/`js`/后台任务）的执行根是**会话目录**——`cwd` 取会话 `tmp/`，`HOME`/`USERPROFILE`/`TMPDIR`/`TMP`/`TEMP`/`XDG_CONFIG_HOME`/`XDG_CACHE_HOME`/`XDG_DATA_HOME`/`XDG_STATE_HOME` 一律重定向到 `{会话目录}/script-env/{home,tmp,config,cache,data,state}`（spawn 前预创建；覆盖顺序在任务 env 之后，防任务 env 里的 `HOME` 把脚本环境指回宿主）。不改写时脚本的配置/缓存/临时文件落在宿主共享位置（用户家目录、系统临时目录），跨用户、跨会话相互可见可覆盖。**文件系统隔离（Linux + bubblewrap）**：命令包进隔离沙箱——系统目录**只读**绑定（`/usr`/`/bin`/`/sbin`/`/lib`/`/lib64`/`/lib32`/`/libx32`/`/etc`/`/opt`/`/var`/`/run`/`/nix`/`/snap`/`/usr/local` 中**存在者**；usrmerge 系统（`/bin`、`/lib` 等为指向 `usr/*` 的符号链接）必须包含这些链接目录，否则动态链接器路径缺失、命令直接不可执行）、`/proc` 与 `/dev` 重建、**仅会话目录可读写**（`--bind`）、`--chdir` 会话内、`--die-with-parent`（父进程退出即回收），越出会话目录的路径不可见（其他会话/用户数据既不可读也不可写）；`project` 参数与项目绑定的解析基准在服务模式下同样以会话目录为界（跨会话即越界），预置项目（`*_PROJECTS`）与子Agent 项目绑定（`*_PROJECT`）在服务模式下不生效——本地模式（豁免用户）全部保持原行为
- **bwrap 可用性探测（容器内尤其重要）**：不只查 PATH——必须完成一次**与真实执行同参数形态**的隔离启动（`bwrapArgs` 同一套参数 + 真实解释器 + 临时目录充当会话目录）才判定可用：容器内未授予 user namespace（seccomp/AppArmor 拦截）时 bwrap 存在但每次调用都失败，简化形态探测通过也不代表真实形态通过（绑定项与只读/可写划分都可能出问题）。不可用时回落环境收敛：行为始终安全（HOME/TEMP 仍在会话目录内），仅失去文件系统隔离；显式配置 `bwrap` 而不可用时输出一条带**失败原因**（bwrap 的 stderr 摘要）的告警。**容器部署判定**：在目标容器内执行 `unshare --user --map-root-user echo ok` —— 输出 `ok` 则 bwrap 可用（还需容器内存在 `bwrap` 二进制，Debian/Ubuntu 包名 `bubblewrap`）；报 `Operation not permitted`/`Permission denied` 则需放宽容器：`--security-opt seccomp=unconfined`（实测可用）或 `--privileged`——**仅 `--cap-add SYS_ADMIN` 不够**（unshare 可创建，但 bwrap 仍被 seccomp 拦在 `pivot_root`）；也可在宿主放开非特权 user namespace。若一个容器只服务单个用户/会话，容器本身就是隔离边界（无需 bwrap，环境收敛已足够）；只有**一个容器内跑多用户 GEBAI** 时 bwrap 才是必需的隔离层
- **桌面控制隔离**：`desktop`（截图/窗口控制/键盘鼠标输入）是对宿主机桌面的真实操作，**仅本地/桌面模式或沙箱豁免用户可用**——服务端部署（沙箱约束用户）下全部工具一律拒绝执行；输入/点击/窗口控制类工具默认需审批，防远程滥用与误操作。部署警示：**服务模式（`GEBAI_SANDBOX=auto` 时）自动强制启用沙箱**，阻断远程桌面操控（admin 豁免除外）
- **浏览器隔离**：`playwright`（无头浏览器自动化）运行在**隔离的浏览器环境**（独立 Chromium 进程，不触宿主机桌面/文件系统），服务端部署可用；但导航/交互/脚本类工具（`open`/`click`/`fill`/`press`/`select`/`check`/`evaluate`/`new_page`）默认需审批——防远程用户借服务端浏览器探测内网（SSRF）、提交表单或执行任意页面脚本；浏览器上下文按会话隔离，`evaluate` 可读取页面内数据（含表单值/cookie），仅限审批后执行
- **Webhook SSRF 防护**：Webhook 注册默认拒绝回环/链路本地/云元数据地址（`localhost`、`127.*`、`169.254.*`、`::1`、`fe80:*` 及其 IPv4-mapped/尾点 FQDN 等绕过形式），需内网回调时以 `GEBAI_WEBHOOK_ALLOW_PRIVATE=true` 显式放开；**投递同样带逐跳重定向校验**（复用 `fetchWithRedirectGuard`，每跳 Location 重新过 `checkWebhookUrl`，防「注册公网 URL → 302 内网/元数据」跳板绕过注册期校验，与 `fetch_url`/`http_request` 同口径）
- **公网访问守卫（fetch_url/http_request）**：沙箱约束用户（服务端部署模式）仅允许公网地址——拒绝回环/链路本地/私网（RFC1918）与 **ULA**（`fc00::/7`）；主机名判定统一走 `@gebai/agents` core/shared/ip.ts，覆盖常见绕过形式：**IPv4-mapped IPv6**（`[::ffff:127.0.0.1]` 及完整形式）、IPv4-compatible IPv6、**整数/十六进制/八进制 IPv4**（`2130706433`/`0x7f000001`/`0177.0.0.1`，WHATWG URL 已规范化为点分十进制）、**尾点 FQDN**（`localhost.`）；`fetch_url` 与 `http_request`（默认路径）另带**重定向逐跳校验**（`redirect: "manual"` 手动跟随，每跳 Location 重新执行公网校验，跳数上限 5），防「初始公网 → 302 内网」跳板绕过；**域名做 DNS 解析复查**（`assertPublicHttpUrl` 异步解析主机名，任一解析地址命中私网/回环即拒绝——覆盖内网 DNS 名（`kubernetes.default.svc` 等）与「公网域名 A 记录指向内网」的重绑定形态；解析失败/3s 超时放行由 fetch 层超时兜底）；**豁免用户（admin）不限制私网访问**
- **环境变量隔离**：**用户环境变量服务端零留存**（只存浏览器本地 localStorage，服务端不落任何 env 文件；会话内存态 env 不落盘、重启即空），用户间互不可见；`{AGENT_NAME_UPPER}_*` 前缀为**命名约定与前端目录白名单口径**（envVars 声明汇总进环境变量面板），运行时硬边界为脚本子进程/`js` ctx 的**敏感变量剔除**（见「脚本隔离」——敏感判定按完整结尾单词匹配：`*_KEY`/`*_TOKEN`/`*_SECRET`/`*_HASH`/`*_KEY_ID`/`*_CREDENTIAL`/`DATABASE_URL`/`CONNECTION_STRING` 及裸名 `TOKEN`/`SECRET`/`PASSWORD` 等前后缀/裸名形态，防 `GEBAI_ADMIN_PASSWORD_HASH`/`AWS_ACCESS_KEY_ID` 类漏判），敏感变量脱敏显示
- **子Agent共享**：子Agent 为服务端内置代码，构建时编译进二进制，全局共享、只读、无用户差异
- **审批隔离**：`/approval-skip` 为会话级设置，仅作用于当前会话（会话内存态 env，不落盘），不影响其他用户；**用户本人可设置自己的会话**（前端开关、REST `PUT /env`、WS `session.env.set`、飞书 `/approval-skip` 命令——写入只影响本人会话，非管理员仍受路径/脚本/网络沙箱完整约束）；**ask 填值分支服务模式下一律拒绝 `GEBAI_APPROVAL_SKIP`**——模型驱动的写入不得自设审批跳过（防提示词注入诱导），本地/单用户模式不受限
- **安全模式（`GEBAI_SAFE_MODE`）**：部署方可在启动时开启降级运行形态（**仅启动时从 .env/环境变量加载，不进会话/任务级 env，`ask` 填值分支与前端本地 env 注入均无法修改**）——原则是**风险能力降级而非一刀切禁用**，各风险工具在自身 execute 内降级：
  | 能力 | 安全模式降级形态 |
  |------|------------------|
  | `sh` | 只读命令白名单（`cat`/`head`/`tail`/`grep`/`find`/`ls`/`git` 读子命令/`sort`/`diff` 等查看与文本处理类；白名单按命令名匹配——Windows PowerShell 下的别名（`ls`/`cat`/`dir`/`where`）照常命中，cmdlet 全名（`Get-ChildItem`）不命中；`sed`/`awk` 有脚本内写/执行通道、`less`/`more` 有 `!` shell 逃逸，均不入列）；命令解析 fail-closed（反引号/后台执行 `&`/进程替换 `<(cmd)`/未闭合引号等无法识别的结构一律拒绝）；输出重定向 `>`/`>>`/`2>`/`&>` 目标须在**安全写范围**内（`/dev/null`/`NUL`/PowerShell `$null` 放行），fd 复制 `2>&1` 放行；`$(...)` 命令替换与双引号内替换按替换语义**递归校验**（POSIX 双引号内 `$()` 与反引号会执行，单引号为字面量）；`find` 禁 `-exec`/`-delete` 等执行/写动作、`sort` 禁 `-o`/`--output`、`git` 仅读子命令（`status`/`log`/`diff`/`show`/`blame`/`rev-parse`/`ls-files`/`describe` 等）、`env` 仅单独执行、`date` 禁 `-s`/`--set`、`hostname` 仅 flag 参数 |
  | `py` | 子进程内 `sys.addaudithook` 审计钩子（`core/security/safety.ts` `PY_SAFE_BOOTSTRAP`）：写模式 `open`（mode 含 `w`/`a`/`x`/`+` 或 flags 含 `O_WRONLY`/`O_RDWR`/`O_CREAT`/`O_TRUNC`/`O_APPEND`）、进程（`os.system`/`subprocess.*`/`os.posix_spawn`/`os.fork`）、网络（`socket.connect`/`bind`）、文件变更（`os.remove`/`rename`/`mkdir`/`chmod` 等、`shutil.*`）、`ctypes.dlopen`（防绕过钩子的裸系统调用）、`sqlite3.connect` 全部拒绝；钩子堆叠不可移除，**仅保留文件读取** |
  | `js` | 双层降级：静态扫描（`scanJsReadOnly`）**按词元级拒绝**（而非调用形态）——`import`/`require` 任意出现即拒（静态 import 语句提升先于 shim 执行、`const rq = require; rq(...)` 别名形态调用正则拦不住）、`Bun` 仅放行 `Bun.file`（`Bun["fetch"]` 括号访问可躲点号正则，且 fetch/sqlite 为不可覆写 getter、Bun 全局 non-configurable 无法运行时代理，扫描是唯一防线）、`getBuiltinModule` 词元拒绝；运行时 shim（注入子进程）屏蔽其余——`Bun` 对象属性覆写为抛错桩（`write`/`spawn`/`serve`/`$`/`sql` 等）、`Bun.file` 包装拦截 `write`/`writer`/`sink`/`truncate`（读方法照常）、删除 `eval`/`Function`/`fetch`/`WebSocket`/`Worker` 全局、`Function.prototype.constructor` 中性化防 `(fn).constructor` 回收、`process` 仅拦 `binding`/`dlopen`/`getBuiltinModule`/`kill`（桥协议依赖 stdin/stdout/exit）、字符串定时器拒绝、`Reflect.get` 作用于 Bun 对象时拒绝（防反射读取绕过属性覆写）、**模块级 `var require` 中和**（CJS 参数遮蔽/ESM 定义绑定，别名引用拿到拒绝桩）——**仅保留文件读取**；子进程环境同样剔除敏感变量（安全模式下 `process.env` 不暴露密钥）；写文件用 `write` 工具、网络用 `fetch_url` 工具（RPC 调用各工具按其降级规则执行） |
  | `write`/`edit`/`patch`/`file` | 限定**安全写范围**内（`safeModeWriteCheck`）：沙箱模式=用户数据根（`users/{user}`）；本地模式=OS 用户主目录 + `GEBAI_HOME` + 会话工作目录（Windows 大小写不敏感比较）。越界拒绝并提示，范围内照常 |
  | 动态工具（`js` defineTool） | 与 `js` 同规则降级（execute 源码静态扫描 + 子进程只读 shim），注册与水合**不再跳过**——只读动态工具（数据处理/查询类）安全模式下保持可用 |
  | `task_add`/`task_update`/`task_remove`/`task_run`/`task_cancel` | **维持硬阻断**（任务可延迟触发任意执行，无法降级）：引擎主/子循环、`js` RPC 分发层按 `isToolBlockedInSafeMode` 同规则拦截，模型调用时直接返回限制信息（不执行、不弹审批）。`task_notify`（主动通知）声明 `safeMode: false`——安全模式下不注册（通知=外发请求，与通知投递整体禁用同规则） |
  | 子Agent 工具 | **自主声明 `Tool.safeMode`**：`true`=作者判定安全模式下可提供（即使短名风险如 `{agent}_sh`，须自行保证实现只读或体内按 `ctx.safeMode` 校验）；`false`=判定不提供（即使名字无风险）；未声明=按短名风险规则默认（`isRiskyToolName`：短名集合为 `sh`/`py`/`js`/`write`/`edit`/`patch`/`file`/`delete`/`cron_add`/`cron_update`/`cron_remove`/`cron_trigger`，按「等于或 `_{risk}` 结尾」匹配——如 `{agent}_sh`/`{agent}_cron_add` 命中则不注册）。注册期过滤（`ToolRegistry({safeMode})`，主注册表与 subsession_run 子会话注册表同规则）——不注册即 schema 不可见、调用报未知工具 |
  **安全写范围**与**降级说明注入**：系统提示词（主循环与子会话运行）在安全模式下追加降级能力说明（模型知晓能力边界）；已创建的 script 型任务触发时跳过（落盘提示、不执行 shell，`nextRunAt` 正常推进）；审批姿态不变（各工具原 `requiresApproval` 规则照常生效）
- **限流保护**：按用户限制并发任务数与消息速率，防止资源滥用；单用户单会话同时仅一个任务运行；**每用户 prompt 令牌桶限流**（REST `POST /sessions/:id/prompt` 与 WS `session.prompt` 同规则：容量 60 突发、30/秒补充，超限返回 429 / error reply）

#### 安全与稳定性强化批次（2026-08，全量审查修复）

除上述各节就地更新的语义外，本批次还包含以下工程级强化（均已落地并带回归测试）：

- **持久化原子性**：`chat.json` 写入改为 tmp+rename 原子替换（崩溃/断电/磁盘满不再留下截断 JSON 致会话静默丢失；Windows 上目标被并发读取时 rename EPERM 回退直接覆写——原子性降级为最佳努力，不因竞态中断任务）
- **任务并发 TOCTOU 防护**：`engine.run()` 检查与注册之间的 await 间隙可让并发请求（WS 重复帧/REST 与 WS 双通道）双双通过检查造成同会话双任务——改为**同步占位再异步校验**（校验失败同步回滚），入口层 `isRunning` 检查不再有竞态窗口
- **会话缓存 LRU 化**：`store.load()` 命中刷新位置（运行中会话不因其他会话活跃被挤出 10 条缓存）；任务流程内的持久化调用（`appendMessage`/`getTodos`/`setTodos`）带归属用户——挤出后仍可从磁盘确定性装载（此前挤出后无 user 的装载失败会中断运行中任务）
- **审批授权竞态（飞书群聊）**：`runOwners` 绑定条件写入（不覆盖既有绑定）+ 解绑校验「自己仍是绑定者」——并发消息竞态下后来者覆盖/误删发起者绑定会使审批 `openId` 变空、跳过「仅发起者可操作」校验（任意群成员可批准高危操作）
- **飞书长连接韧性**：pong 超时检测（连续 3 个 ping 周期无 pong 主动断开走重连——NAT 超时/半开 TCP 下 onclose 不触发、bot 永久失联）；重连达服务端上限后**降频续试**（60s 封顶，不再永久停摆）；事件按 `event_id` 去重（飞书重推防任务重复执行）；`finalSent` 每任务复位（空最终回复的任务在飞书侧得不到完成信号的缺陷）
- **feishu_docs token 管理**：user refresh_token 刷新**会话级单飞**（单次有效的 refresh_token 并发双发必有一个失败静默降级 tenant 身份）；tenant token 远端失效（99991661/99991663）逐出缓存重试一次（此前最长 2 小时持续失败）
- **子Agent 装载引用计数**：`SubAgentManager.load/unload` 按 owner（会话 id/subsession_run 共享标记/全局）解引用——一个会话卸载子Agent 不再注销全局注册表砍掉其他会话正在使用的工具（装载状态按会话建模、注册表全局共享的矛盾以引用计数弥合）
- **SDK 连接韧性**：心跳判死阈值回归 `heartbeatTimeoutMs` 语义（此前下一拍间隔即杀，高延迟链路「连上→误杀→重连」循环）；断线批量 reject 在途请求时清超时 timer（不再二次 onError「WS 请求超时」误导）；建连期无 onerror 的 close 路径 settle 共享 promise（调用方不再永久挂起）；断线恢复后 seq 基线收敛（缺口后方无新事件窗口内不再重复重放旧区间——旧 `task.done` 截断新任务流）；`dispatchEvent` 按 seq 去重（多会话并发恢复同一批重放事件不再双份分发）；WS 附件 `Uint8Array` 序列化为数组（此前 JSON 化为类数组对象、服务端构造抛错）
- **Web 交互修复**：审批 Y/N 快捷键排除文本输入焦点（搜索/重命名框打字误批）；审批等待刷新前端看门狗活跃时间（服务端审批等 5 分钟、前端 150s 看门狗不再误杀）；审批卡按 toolCallId 去重（事件重放不堆叠）；切回会话时运行中工具卡重建过滤历史已完成项；断线全量重同步后子Agent 容器惰性重建（run 输出不再静默丢弃）
- **其他**：`file delete` 递归删除需审批（见「工具审批」）；附件重名自动加序号（同批两个 data.csv 不再静默覆盖）；analyzer 的 tree-sitter Tree 用毕显式释放（wasm 原生内存）；`serve_dir` 静态服务 8 个上限（最旧淘汰）；desktop `screenshot` 的 `name` 参数落地（此前声明未实现）；`show`（由已合并的 `draw`/`render_html`/`show_file` 三工具演进而来）的 name 参数补 writeGuard/安全模式写检查（此前可越出子Agent 写范围）；SSE 解析兼容 CRLF/CR 行结尾与多行 data 按规范 join（部分网关 CRLF 行结尾整轮静默无输出）；三家 provider 的 done chunk 语义统一（恰好一次、断流补发）；SSE 消费方提前退出时 cancel 上游响应流（连接不悬挂）；vision 调用透传取消信号

### 总Agent
- 系统提示词**身份行**先声明目标（「目标是融合旧世界IT的所有技术，打造新世界智能的躯体——以极致动态扩展的能力把任意语言、任意进程的外部技术收编为工具」），再携带智能与智体概念模型（「你是智体：智能（模型）负责思考、无状态、可替换，记忆与责任都长在智体——需跨轮次/跨会话保留的结论与状态写入文件或会话记录；你的每次工具调用都是智体的行为，经审批执行、留痕可审计」）——行为化措辞而非装饰（落盘纪律与担责意识直接约束模型行为）。概念定义与五条设计戒律见「定位 → 智能与智体（概念模型）」
- 系统提示词中注入：**未装载**子Agent 的轻量引导列表（名称 + 描述），引导模型通过 `agent_load` 装载后使用（装载 = 工具注册 + 完整提示词写入会话记录）或直接 `subsession_run` 派生子会话（无需装载）。**已装载子Agent 的完整系统提示词不注入总层提示词**——装载时已作为 system 消息写入会话记录（`loadedAgent` 标记，`loadHistory` 透传进模型上下文），此处再注入会双份占用上下文。**不展开工具 schema**——工具名已注册进工具集；但未装载清单的描述**附工具摘要**（`agentDescription` 追加「装载后工具：a、b、c…」，超出 10 个截断计数；工具调用前缀 `{agent}_` 与「通用文件/编排工具仍用全局名」由清单表头（`systemPromptInjection`）统一说明一次，不逐条重复——逐条重复随子Agent 数量线性膨胀初始上下文）——未装载的工具名不在 schema 里，摘要是装载前的路由匹配面（`agent_list` 同样不列工具 schema，描述同样携带摘要）
- 系统提示词内置**任务类型路由引导**：总层只给「装载 vs 子会话运行语义」机制说明 + 「按任务类型从下方可选子Agent 清单选用（描述即触发场景）」引导；**具体任务→子Agent 映射不硬编码**——由 `systemPromptInjection` 注入的未装载子Agent description（触发场景 + 职责边界）承载（code/self_optimize 互指边界：不处理歌白自身代码/不处理外部项目），新增子Agent 零改动自动进入路由信息（纯文本问答不装载）。**选用原则：默认 `agent_load` 装载（模块语义，工具与完整系统提示词写入当前会话记录、不创建独立执行）后直接用其工具**，仅在需要干净上下文（子任务结果隔离、不污染父上下文）、防止上下文膨胀（子任务中间过程多、输出大）或长任务并行（`async:true` 后台执行）时才用 `subsession_run` 派生子会话（会话语义，**无需装载**：隔离子会话预加载一个或多个子Agent（完整系统提示词与工具）后执行，只把最终结果带回；继承形态 fork 父上下文并行推进，报告自动合入）；执行过程子Agent 的模型回复、推理与工具调用全程实时推送前端
- 系统提示词内置**并行多路引导**：同一任务的并行多路推进（多方案对比、多文件并行修改、多角度调研等多条互不依赖的线）用 `subsession_run` 的 `inherit_context:true`（fork 父会话上下文）——一次 fork 多个子会话同时执行（各子会话掌握父会话全部背景与工具，可各自传 `model` 走不同模型接口并行），报告完成即自动合入父会话，长耗子会话 `async:true` 后台执行；并行多线是摆脱单轮串行等待、加速大体量任务的主要手段（见「子会话运行」）
- 桌面/浏览器子Agent 系统提示词内置**验证多通道降级策略**：截图黑屏/失败时切换 DOM/content、窗口状态、数据文件等通道，任一失效立即降级并告知用户，不盲目重试单一通道
- 浏览器子Agent 系统提示词内置**性能/动画采样的可见性前提**：rAF 帧间隔与 longtask 采样只在可见页有效——后台标签页 rAF 被节流到 ~1fps，采出来是「单帧 800ms+、longtask 0 个」的假数据（极易读成「动画走了合成器」）；多标签先关掉前台页，并把 `visibilityState` 记进采样结果（hidden 即作废），计时用 rAF 自排队而非 setInterval
- **系统提示词中声明会话工作目录**（会话 `tmp/`，如 `{GEBAI_HOME}/users/{user}/sessions/{s0}/{s1}/{session_id}/tmp/`）并说明**所有文件工具的相对路径以此为基准（`tmp/` 前缀可省略）**；服务端部署模式下大模型读写限定在该目录，桌面/本地浏览器模式不限制目录（同路径沙箱规则）
- 系统提示词中引导模型：复杂/多步操作优先用 `js` 脚本编排一次执行（脚本内工具像内置函数直接 await、可用变量/分支/循环表达任意流程），纯系统操作用 `sh`/`py` 脚本
- 系统提示词中引导模型：**重大任务（多步骤/有风险/不可逆/用户需要把关）先制定计划**——调用 `ask` 的计划审批分支（title+steps）把计划文档写入会话文件并在界面展示，阻塞等待用户批准后再执行（被拒绝则按修改意见修订后重新提交）；简单任务无需计划审批，`todo` 跟踪即可
- 子Agent 装载后，系统提示词实时更新；**声明依赖的子Agent 装载即连带装载其依赖**（`def.dependencies` 驱动的级联，`SubAgentManager.load` 幂等，WS `sub_agent.load`/`agent_load`/预加载所有装载路径均生效，如 `self_optimize`→`code`+`vision`、`reverse_site`→`playwright`，见「子Agent 依赖与自动装载」）；**`subsession_run` 预加载时同样连带预加载依赖**（`normalizeRunAgents` 同规则展开——装载方 def 只声明独有工具，依赖的工具与工作流提示词由依赖方 def 提供，不重复注册）
- **提示词分层职责（严格划分，防止职责越界与重复）**：
   - **总Agent 系统提示词**（`buildSystemPrompt`）只承载：身份、环境边界（工作区/沙箱）、执行策略、任务类型→子Agent 路由（紧凑映射 + 装载/子会话运行语义）、子Agent 能力注册表（`systemPromptInjection`：仅未装载轻量列表——**按会话判定未装载**（其他会话装载过不算本会话已装载，见「装载工具会话可见性」），已装载的完整提示词在会话记录的 `loadedAgent` 消息里）、`{AGENT}_PROJECT` 项目绑定声明（路由信息）。**不注入**子Agent 动态项目注记（预置项目清单、受限模式说明、项目根——属运行期上下文，随子Agent 提示词注入：`subsession_run` 子会话运行与装载写会话记录均注入）；唯一例外：**子Agent 描述动态体现预置项目摘要**（未装载清单/`agent_list` 中，`{AGENT}_PROJECTS` 配置时描述附「预置项目：名称: 说明（路径）」摘要，方便总Agent 按项目名关联任务与代码位置，完整清单注记仍只在子Agent 提示词）
   - **子会话系统提示词**（`runSubSession`/`runSubSessionLoop`）只承载：隔离声明（或 fork 子会话附注）、各预加载子Agent 的完整系统提示词（自包含）+ 职责分隔头（`### {name}（{description}）`，多 Agent 预加载时明确各段提示词对应的工具命名空间与职责域）+ 动态项目上下文（项目根/工作目录、预置项目清单、受限状态、AGENTS.md）；**动态环境注记置于职责分隔头之后、静态提示词之前**（配置信息前置——模型开工先读环境注记确定目标项目与 project 参数，再读工作流）
  - **子Agent 静态系统提示词**只承载自身职责域：本 Agent 的工作流、行为约束、工具协作说明；**不复刻其他子Agent 的内容**（如 self_optimize 不复刻 code 的通用工作流——直接引用连带装载的 code 提示词，自身只写自我优化特有流程与约束），不写实现机制说明（如工具复用/连带装载——机制由代码承担，提示词只写模型行为指令）
  - **工具 schema 描述**自包含（装载/子会话形态通用，工具描述独立成立——模型选择工具的第一信息来源，如 `project` 参数直接说明 CODE_PROJECTS 清单；子Agent 完整提示词随装载写入会话记录，但工具描述仍须自成体系，不依赖提示词解释）
  - **单源去重（上下文经济性铁律）**：schema 每轮随请求全量重发、装载后提示词与工具描述同处一个上下文，同一条信息只允许一处权威副本，其余位置单向指针——参数级语义只写在参数 description（工具描述正文不罗列参数）；领域知识表内置于对应工具描述（如飞书块类型表在 `add_blocks` 描述，系统提示词只留指针）；安全模式降级说明单源于系统提示词（主循环与 `subsession_run` 子会话同规则注入，工具描述不重复）；「装载 vs 子会话运行」选用原则单源于总Agent 路由段（`agent_load`/`subsession_run` 描述只写机制，未装载清单头不解释用法）；子Agent 提示词写工作流取舍，不重述工具描述已有的参数级细节；子Agent 不重复定义全局已有工具（只声明独有工具，见「装载工具会话可见性」）；「无需审批」类姿态说明对模型决策零价值，不写入描述（`sh`/`py` 的 `approval:false` 传参引导除外——模型需主动传参）

#### 子Agent 提示词编写规范（8 维强化检查清单）

新增/修改子Agent 系统提示词时按以下 8 个维度逐项检查（自我优化审查提示词同样适用）；内置子Agent 参考此清单编写：

| 维度 | 检查项 |
|------|--------|
| 1. 身份与职责边界 | 开头一句话身份自述（「你是 xx 专家/助手」）+ 职责范围与边界（做什么/不做什么/运行形态限制如服务端拒绝） |
| 2. 工作流骨架 | 编号步骤：目标确认 → 探索/读取 → 方案 → 执行 → 验证 → 收尾反馈；多步骤任务用 `todo` 跟踪、重大/有风险任务先 `ask` 计划分支提交计划经用户批准后再执行、方向取舍用 `ask` 选项询问确认 |
| 3. 工具使用纪律 | 每个工具的触发场景与优先级；相似工具分工（如 `find_blocks` 定位 vs `get_doc_text` 读取）；常见失败规避——**先读后写**（修改前确认当前内容，防基于过期内容操作）、**等待优先**（wait_for 目标条件，不用固定 sleep）、失败分类定位不盲目重试 |
| 4. 安全与审批 | 审批意识（写/交互/导航类操作需审批，操作前先说明方案与影响范围）；敏感数据（密钥/凭据/token 不输出明文、自动脱敏）；权限边界（只操作授权范围） |
| 5. 验证多通道 | 不依赖单一验证通道（截图/内容读取/DOM/请求记录/直连探测）；任一通道失效立即切换并**明确告知用户当前采用的验证方式**，不盲目重试同一通道 |
| 6. 协作与编排 | `subsession_run` 委托形态（委托谁、输入什么、期望产出——如 desktop 委托 code 读数据文件验证、playwright 被 code 委托做浏览器验证）；与 `self_optimize` 联动边界（仅 reverse_site 转交文档） |
| 7. 验证闭环 | 修改/操作后验证（测试/检查/断言）；失败先定位（读错误信息分类）再修，不盲目重复执行 |
| 8. 降级与诊断 | 错误分类 → 针对性处理（元素不存在/导航失败/鉴权失败/限频等）；回退/占位路径说明（如摘要失败降级为骨架行占位、OAuth 刷新失败回退应用身份） |

> 提示词只写**模型行为指令**，不写实现机制（工具内部如何实现由代码承担）；不复刻其他子Agent 的内容；保持精简——每个维度点到即止，防止提示词膨胀稀释注意力。

### 子Agent（扩展机制）

- 定义形式：**单文件** `agents/src/agents/{name}.ts`，或**目录** `{name}/{name}.ts`（`{name}/index.ts` 为回退入口）——入口可另配 `{name}.md` 系统提示词（由入口 ts 导入并修饰）；无 ts 定义时纯 `{name}/{name}.md` 即零 TS 简化定义。子代理定义一律放 `packages/agents/src/agents/`（基建组件在同级 `src/core/`，物理分域）
- 目录形式下，系统提示词 md 由入口 ts 文件**导入并修饰**（见「子Agent文件格式」）：`import systemPrompt from "./{name}.md"`（Bun 原生文本导入，构建时随 ts 一起内联进产物）
- **零注册**：文件/目录即声明，运行时自动扫描收集（跳过 `*.test.ts` 与辅助文件，目录形式只认 `{dir}/{dir}.ts`），无需任何配置文件或代码注册；**包入口 `@gebai/agents` 同样零清单**——入口只导基建工具（发现/装载链路全自动，新增子Agent = 在 `packages/agents/src/` 放定义文件即可，入口与 server 均零改动，无手工清单可漏改漂移）；**二开同理**：`custom/agents/{name}/` 放定义文件即注册（双域扫描自动合并，同名 custom 胜出——见包结构表「custom/ 二开域」）
- **热加载（目录签名失效缓存）**：`@gebai/agents` 包 `src/agents/`（子代理定义域）目录的**新增/修改/删除**在下一次装载（`agent_load`/路由自愈/`subsession_run` 预加载，`load()` 前检查）或新任务（`run()` 前）自动生效，无需重启——`refreshIfChanged()` 比对目录签名（递归 `路径:mtime`，**深度上限 2 层**——更深层的定义文件变化不触发重扫；~30 次 stat 可忽略），变化即重扫：TS 入口带 `?t={mtime}` 查询参数 import **绕过模块缓存**（修改过的文件拿到新代码；新文件本就不在缓存）；扫描域与基建物理分域（子代理在 `src/agents/`、基建在 `src/core/`——目录即语义，无需排除清单，新增基建目录不再需要改任何清单）；包入口零子代理清单（见上「零注册」），发现正确性由 server 侧发现链路测试锁定；**会话删除释放装载者引用**：`forgetSession` 经 `releaseOwner` 按会话全量解引用（owner 引用计数归零才注销工具注册——`ownersByAgent` 随会话删除回收，防长运行服务无界增长；全局装载与 `subsession_run` 共享标记不受影响）；**已装载会话沿用旧定义**（工具注册与注入会话记录的提示词保持稳定，不迁移——防运行中会话行为漂移），新定义对未装载与新会话生效；运行期显式 `unregister` 的子Agent（如 cron 关关）重扫后保持移除（`removedDefs` 过滤，防「复活」）；二进制 bundle 形态源码目录不存在、注册表不可变，无热加载。**加载失败错误透出**：扫描中 import 抛错/缺 `def` 导出/md 解析失败的子Agent 记入 `loadErrors`（随 defs 一同进程级缓存），`agent_load` 与 `subsession_run` 校验的「未知子Agent」错误**附加载失败原因**（模型可见根因——self_optimize 写错文件当场定位修复而非面对无解释的未知名，修复文件后 mtime 变化触发重扫自动恢复注册）；**`agent_load` 装载失败真实报错**（`ctx.loadSubAgent` 装载后仍未注册即抛 `unknownAgentError`——含附因；幂等重装已装载者仍成功，`loadAgentsForSession` 的单失败跳过容错仅用于会话恢复路径）。价值：self_optimize 生成/修改子Agent 后**当会话内即可 subsession_run 验证成果**，自我优化闭环不再依赖重启。
- **热加载的边界：只对入口文件生效**（实测约束，不以推断为准）：入口 import 带 `?t={mtime}` 可绕模块缓存，但入口以**裸相对说明符**引用的辅助模块一旦被加载就在本进程内无法失效——实测（Bun 1.3.14）：即使入口与辅助同时修改，辅助模块仍返回旧值（对照：辅助模块自身带 `?t` 直接加载能拿到新值，但它被 import 时用的是裸说明符）。这意味着改辅助模块会得到「新入口 + 旧辅助」的混合版本：可能报 `Export named X not found` 这类费解错误，也可能**沉默地按旧逻辑运行**。当前处理：`auxChangeNote` 按基线识别辅助模块变更，把限制说清楚（写入 `hotReloadWarnings()`、告警日志，并在加载失败时附在 `loadErrors` 里）——**不假装能修好它**。「造世代副本」路线曾导致自激事故（副本落在被自身监视的扫描域内 → 签名变化 → 重扫 → 再复制），不再采用。
- **打包闭环**：`bun build` 前由 `packages/server/scripts/build-subagents.ts` 扫描 `packages/agents/src/agents/`（子代理定义域）生成 bundle 注册表（`src/core/subagents.bundle.generated.ts`，gitignore），全部子Agent 定义（含 md 提示词）以静态 import 内联进产物；dist/二进制模式下源码目录不可用，`discover()` 自动回退到 bundle 注册表——子Agent 真正「打包进二进制」，运行时无需读取任何子Agent 文件。**构建期逐代理验证（DESIGN「子代理失败隔离」）**：脚本对每个 TS 定义真 import 一遍，模块顶层抛错/缺 `def` 导出/`def.name` 不一致的代理**剔除出静态 import**（否则运行时顶层静态 import 任一模块失败会炸整个注册表），原因烘焙进 `bundledErrors`（运行时水合进 `loadErrors`，模型侧可见根因）；单代理失败只告警不阻断构建、不连带其他代理。**运行时降级不阻断启动**：bundle 注册表整体缺失/加载失败（构建脚本未跑/生成文件损坏）时降级为无子Agent启动（显眼告警，服务本体与全局工具正常，修复构建链路重启即恢复）——子代理失败不炸主流程，绝不因单模块问题让服务不可用。**例外与配套**：playwright 子Agent 的 `driver.mjs`（node 桥接进程，须保持独立文件）由构建脚本复制到 `dist/` 与产物同目录，运行时按 `import.meta.dir` 定位（`--compile` 形态另经 `packages/server/scripts/build-driver-embed.ts` 内嵌、物化到 `{GEBAI_HOME}/vendor/playwright/`）；playwright-core 包树经 `packages/server/scripts/build-pwcore-embed.ts` 整树 gzip base64 内嵌（`pwcore.embedded.generated.json`，gitignore），运行时物化到 `{GEBAI_HOME}/vendor/playwright-core/`（见 playwright 子Agent「依赖与部署」）。**铁律：bundle 图内的子Agent 模块禁止模块作用域的第三方包解析**（`Bun.resolveSync`、裸 `import`/`require` 等）——编译产物中这类解析锚定真实 CWD 的 node_modules 可达性，此类启动期解析失败已被构建期验证拦截（剔除出 bundle）；此类依赖须延迟到首次工具调用（playwright 经 `createLazyBridge()` 惰性单例，解析失败降级为工具级运行时错误，不影响服务启动）
- **构建期裁剪与预加载指定**（环境变量，二进制形态无法改源码、构建时定死）：`GEBAI_BUILD_SUBAGENTS`（逗号分隔包含清单，缺省 = 全部打包）按需产出精简二进制——**包含清单经依赖闭包自动展开**（include reverse_site 自动带上其 `dependencies` 声明的 playwright——运行时依赖自动装载要求依赖方在产物中存在，漏列会产出能力残缺的二进制；构建脚本动态 import 各 def 读取 `dependencies` 递归补入，依赖指向不存在的名字直接构建失败）；`GEBAI_BUILD_PRELOAD`（逗号分隔预加载清单）烘焙为 `def.preload=true`（启动即装载，运行时 `GEBAI_PRELOAD_SUB_AGENTS` 覆盖仍优先；预载逐个隔离——单个预载失败只记 loadErrors + 告警，不阻断启动与其余代理，见「子代理失败隔离」）；`GEBAI_BUILD_EXCLUDE_TOOLS`（逗号分隔**全局工具排除清单**，`packages/server/scripts/build-tools.ts` 生成 `tools-excluded.generated.ts` 烘焙）——被排除的全局工具不注册不暴露（schema 不可见、调用报未知工具），subsession_run 子会话内建编排工具（tool_schemas/js）同规则过滤（`isGlobalToolExcluded`）；语义注意：全局工具排除是**能力裁剪**——工具实现与工具表同模块仍会打包（无法摇树），体积裁剪主要来自子Agent 包含清单与内嵌产物跳过。三清单中的未知名字构建直接失败并列出可用名单（防产物静默缺失）。`tools-excluded.generated.ts` 与 subagents.bundle 不同——**提交默认空名单入库**（消费方 `core/tools/index.ts` 静态导入：运行时读文件在 `--compile` 单文件形态不可行）；`packages/server/scripts/build-tools.ts` 同时生成全局工具 bundle 注册表 `core/tools/bundle.generated.ts`（见「全局工具零注册」），该文件 gitignore，裁剪构建后为脏属预期，勿提交裁剪态。**模型配置内置**（`GEBAI_BUILD_EMBED_ENV=1`，`packages/server/scripts/build-env-embed.ts`）：把仓库根 `.env`（+进程环境）中 `GEBAI_LLM_*`/`GEBAI_VISION_*` 前缀的模型配置烘焙为二进制**启动默认值**（`env-embedded.generated.ts`，`startServer` 顶部经 `applyEmbeddedEnvDefaults` 仅填充未设置/空串的键——优先级：前端/任务级 env > 运行时环境变量 > `{GEBAI_HOME}/.env` > 内置默认），发行裁剪构建产出「开箱即用」产物；文件策略同 tools-excluded（默认空对象入库、内置构建后脏态勿提交，调用方构建脚本编译后立即还原空态——**该文件必须在版本控制中、不得写入 .gitignore**：`boot/compose.ts` 静态导入，缺文件则新克隆环境 `bun run dev` 启动即 `Cannot find module`（历史踩坑：仓库瘦身时被误随 `*.embedded.generated.json` 二进制内嵌产物一并移出跟踪）；`packages/server` 的 `dev`/`typecheck`/`build` 链前置经本脚本兜底生成，`build` 编译后再以 `--restore` 自动还原空态；并由 `core/generated-artifacts.test.ts` 守卫——`src/**` 中**静态导入**的 `generated` 模块必须存在且已被 git 跟踪，误移出跟踪当场测试报红；动态导入不在此列（构建链接管 + 运行时降级））；**安全边界：内置密钥明文随二进制分发、可被持有者提取**——仅限受信任小范围分发，建议低额度专用 Key，生成/构建日志只输出变量名不输出值
- **裁剪构建样例**（根 `scripts/` 目录）：`bun run build:code`（`build-code-agent.ts`）产出 code 场景精简**服务端**单文件二进制（`packages/server/dist/gebai-code[.exe]`，浏览器形态、内嵌 Web UI）——三层裁剪组合示范：子Agent 包含清单（code+explore，体积收益主来源：未选子Agent 模块整体摇出产物）+ 预加载清单（code 开箱即用）+ 全局工具排除清单（show/fetch_url）。可作为其他场景裁剪构建的模板：复制脚本改清单即可（如 reverse_site 站点逆向、feishu 文档、只读分析）
- 命名规则：仅限小写字母、数字、下划线
- 子代理定义模块的 import 前缀按域区分（内置域 `../../../../agents/src/agents`、二开域 `../../../../../custom/agents`，均相对发现器文件）；域目录存在但无定义文件与域目录缺失是两种状态，后者（无源码树）才回退 bundle 注册表
- 子Agent 定义的工具名无需关注前缀，总Agent 负责在其 schema 中添加 `{agent_name}_` 前缀（命名空间规则见「工具与命名空间」），以及工具调用时的路由和转发，对子Agent 完全透明
- 环境变量作用域：子Agent 配置项按 `{AGENT_NAME_UPPER}_*` 前缀**约定**声明（进入环境变量面板目录与 `envVars` 白名单口径）——**无强制的访问隔离**：TS 子Agent 工具与客卿均拿到完整 `ctx.env`，真正的硬边界是脚本子进程/js ctx 的敏感变量剔除（见「脚本隔离」「环境变量隔离」）
- 全局共享：所有用户使用同一份内置子Agent，无用户差异

#### 装载 vs 子会话运行（概念模型）

「子Agent」一词承载**两种语义迥异的操作**，代码、文档与系统提示词必须严格区分——这是消除模型误解的关键：

| 维度 | **装载**（`agent_load`，模块语义） | **子会话运行**（`subsession_run`，会话语义） |
|------|------|------|
| 类比 | import 子模块 / 静态链接 | **父子进程**：`fork`（继承父上下文）或 `spawn`（隔离新上下文） |
| 前置条件 | 无 | 无（**无需先装载**，两种操作互相独立） |
| 上下文 | **不创建新上下文**：并入父上下文 | `inherit_context:false`（缺省）= **隔离新上下文**（子Agent 提示词 + 全局工具，不继承父消息历史）；`true` = **从当前上下文 fork**（父消息历史 + 父系统提示词 + 工具面快照） |
| 效果 | 工具以 `{agent}_` schema 全名注册进当前工具集（编码类子Agent 只声明独有工具——文件读写查询直接用全局工具，见「装载工具会话可见性」）、**完整系统提示词作为 system 消息写入会话记录**（`loadedAgent` 标记持久化，`loadHistory` 时按 system 角色**前置**进模型上下文——统一置于历史最前，保持 `assistant(tool_calls)` 与其 `tool` 结果相邻，避免装载消息夹在中间被模型接口校验拒绝（实测 DeepSeek 400「tool_calls 后必须紧跟 tool 响应」）） | 派生一个或多个子会话执行任务（`agents` 可省略/为空 = **不加载任何子Agent**；需加载时给出名单，完整系统提示词拼接 + 独有工具并入；**默认继承全局工具与父会话全局提示词**——`inherit_global_tools=false`/`inherit_global_prompt=false` 可分别关闭），默认阻塞执行，`async:true` 转后台运行（`bg_task` 查询进度/等待结果/主动终止） |
| 结果交付 | —（并入父上下文，模型可见每一步） | **继承形态**：最终报告**自动合入父上下文**（合并消息 + 过程存档，无需取回动作）；**隔离形态**：最终结果作为本次工具结果返回（异步则 `bg_task wait` 取回） |
| 隔离/存档 | 无隔离、无存档（全程在父上下文） | 子会话过程**不进父上下文**（实时推送到前端 + 完整过程存档 `SubSessionArchive`）；继承形态的合入只带报告/摘要，过程仍在存档 |
| 幂等 | 幂等（重复装载跳过） | 每次运行 = 一次子会话（runId `s` + 8 位 hex） |
| 生命周期 | 装载/卸载（`sub_agent.load`/`agent_load`/预加载，会话级持久） | 一次运行即结束（运行期可查询/终止，终态记录保留最近 20 条） |

- **装载（模块）**：类比 import 子模块——子Agent 的工具注册进当前工具集（`{agent}_` 前缀）、**完整系统提示词作为 system 消息写入会话记录**（`loadedAgent` 标记，chat.json 持久化；装载后当次会话后续轮次立即进入上下文，恢复历史会话时从会话记录透传），装载后直接调用其 `{agent}_` 工具，全程在主循环/父上下文内完成，无独立执行过程。装载段落的动态环境注记与子会话形态对齐：项目绑定存在时注入「项目根: <根>（访问项目用 project 参数传该根或绝对路径；相对路径仍以会话工作目录为基准）」（限定语必要——装载模式相对路径基准仍是会话目录，不宣称工作目录已切换），无绑定时注入「工作目录: <会话 tmp>」；预置项目清单注记同款注入。**会话记录 `loadedSubAgents` 保存已装载名单**：恢复历史会话时引擎自动按名单重新注册工具（`engine.ensureSessionAgents`，幂等），实现「会话按保存的文件完全恢复状态」。**装载痕迹只看本会话记录、与进程级注册解耦**——子Agent 工具是进程级共享的（任一会话/全局装载过同名者，`SubAgentManager.load` 幂等跳过并返回空集），故 `engine.loadAgentsForSession` 按 `cascade` 闭包逐个比对会话记录补写提示词与名单，不以「本次是否新注册」判定（否则他方已装载时本会话不留任何痕迹，进程重启即丢装载状态）。预加载（`preload`/`GEBAI_PRELOAD_SUB_AGENTS`）即装载的启动期形态——启动预载的子Agent 在每个**新会话创建时自动写入**提示词消息与工具
- **子会话运行（会话）**：一套父子会话模型覆盖两种形态——隔离形态（spawn）把指定的一个或多个子Agent **预加载**进子会话（各自完整系统提示词拼接为系统提示词、独有工具以 `{agent}_` 命名空间并入工具集；**全局工具与父会话全局提示词默认一并继承**，`inherit_global_tools` 与 `inherit_global_prompt` 默认均为 true，子会话与父会话**工具面、行为约定同构**（read/write/grep/sh 等直接用全局名，文件工具带 `project` 参数路由项目），false 时分别关闭（仅预加载子Agent 工具 + 内建编排 tool_schemas/js / 仅子Agent 提示词上下文最省））；继承形态（fork）从当前上下文切片（见「子会话运行」）。执行到结束把结果交回：隔离形态经工具返回值/`bg_task`，继承形态自动合入父上下文
- **多 Agent 预加载**：`agents` 参数支持列表（如 `["code", "playwright"]`）——多个子Agent 的能力同时进入子会话（提示词拼接、工具集叠加）；每个子Agent 提示词前加**职责分隔头**（`### {name}（{description}）`）明确各自职责域与工具命名空间；各自的项目绑定/预置项目注记分别注入，工作目录取首个含项目绑定的 Agent，预置项目全量合并（同名去重）
- **代码对应**：装载 = `SubAgentManager.load`（**进程级**注册，返回本次新注册集合）/ `engine.loadAgentsForSession`（**会话级**痕迹：按 `cascade` 闭包比对会话记录写入提示词与名单，返回 `{added, dirty}`；依赖经 `cascade` 级连带装载，如 self_optimize 连带 code）/ `ToolContext.loadSubAgent` / `engine.loadAgentToSession`（装载并写入会话记录）；子会话运行 = `engine.runSubSession` / `ToolContext.subSessions`（`SubSessionRegistry`，`core/session/subsessions.ts`；工具入口 `subsession_run`）
- **模型侧引导**：`agent_load`/`subsession_run` 工具描述、系统提示词注入均按上述语义措辞，默认引导「装载后用其工具」，仅在需要干净上下文/防上下文膨胀时用隔离子会话，或在同一任务需要并行多路推进时用继承形态子会话（见「子会话运行」）

#### 装载工具会话可见性

装载的工具注册进**进程共享**的 `ToolRegistry`（卸载按 owner 解引用需要全局注册），但可见性按**会话**建模——引擎主循环不直连共享注册表，经 `sessionRegistry` 会话视图解析（每轮现算，任务中途装载下一轮 schema 即生效）：

- **会话级可见性**：`{agent}_*` 工具仅对**装载过该子Agent 的会话**可见（`SubAgentManager.visibleTo`，owner = 会话 id）；**全局装载**（启动预载/admin `sub_agent.load` 不带 sessionId，owner = `GLOBAL_OWNER`）对所有会话可见。其他会话的装载不扩散——修复跨会话泄漏：旧版任一会话装载 code 后，所有会话的请求都背上全部子Agent 工具 schema、且系统提示词目录里 code 凭空消失（`systemPromptInjection` 按进程装载状态过滤，未装载会话既无提示词也无目录引导）。目录注入同步会话化：按「对本会话可见」判定未装载，A 装载后 B 的目录仍列出 code 供 B 装载
- **子Agent 只声明独有工具（重复工具彻底删除）**：文件读写查询类工具（read/write/edit/patch/ls/grep/glob/file/diff/sh/py）与交互编排类工具（fetch_url/todo/ask/subsession_run/bg_task）为**全局工具**——code/explore 等编码类子Agent 不再以 `projectAware` 双胞胎形态重复定义同款，只声明全局集没有的独有工具（code：search_symbols/analyze/git/preview_server/env_detect/system_info；explore：search_symbols/analyze/git）。装载与子会话运行（全局工具默认继承）均直接用全局名，`{agent}_` 前缀名只有独有工具一种形态。**取代旧版「双胞胎合并」机制**：此前子Agent 版与全局版同名共存（code_read 与 read），主会话经会话视图把同名校并为子Agent 版一份、前缀名留作解析别名——工具删重后该合并层（twinMerge/alias）整体移除，历史消息中的 `code_read` 等前缀名调用按未知工具处理（错误信息附该子Agent 可用工具清单，一步引导改用全局名）
- **全局文件工具带 `project` 参数**（`core/tools/projects.ts` 的 `projectAware` 在全局注册处统一包装）：默认会话相对路径（行为不变），操作项目须显式指定——`project` 参数传**预置项目名**或**项目根路径**（自由项目），路径即相对所选根解析（沙箱模式限定该根内）；保留名 `tmp` = 会话工作区。code/explore 的独有工具同规则包装。受限模式（`CODE_RESTRICT_PROJECTS=true`）下未传 project 的自由路径被拒绝（预置项目/绑定根放行）
- **审批姿态归一**：子Agent 不再覆写与全局同名工具的审批（旧版双胞胎合并按子Agent `requiresApproval` 收紧 write/edit/patch）——全局 write/edit/patch 维持「默认无需审批」（与总Agent 既有姿态一致，防盲写守卫约束写入安全）；子Agent `requiresApproval` 只对**独有工具**生效

#### 子Agent 依赖与自动装载（`dependencies`）

子Agent 间复用能力走**依赖声明**而非把依赖方的工具展开进自己的 def（旧版 `reverse_site` 静态导入 `createPlaywrightTools` 展开全套浏览器工具并复刻审批映射，`self_optimize`→`code` 是三处硬编码特例——现统一为声明式机制，def 声明、引擎执行）：

- **声明**：`SubAgentDef.dependencies?: string[]`（TS 定义导出 `dependencies`；纯 md 简化定义经 frontmatter `dependencies: a, b` 逗号分隔声明，条目须符合子Agent 命名规则 `[a-z0-9_]+`）。依赖方 def 只声明独有工具；依赖的工具由依赖方 def 以**其自身命名空间**注册（`{dep}_` 前缀）、`requiresApproval`/`safeMode`/`envVars` 等亦由依赖方 def 单源维护——装载方不复制任何依赖方定义
- **级联展开（`SubAgentManager.cascade`）**：返回装载某子Agent 需要的完整名单——**依赖在前、自身在后**（依赖先注册工具、先注入提示词），传递依赖递归展开，共享依赖去重；**循环依赖（含自依赖）抛错**（`子Agent 依赖循环: a → b → a`，定义缺陷暴露给模型/self_optimize 修复）；**依赖缺失跳过并告警**（被启停名单移除/构建裁剪的依赖不阻断装载方——其自身工具照常可用，仅失去该依赖能力，与旧 `self_optimize`→`code` 的 `defs.has` 守卫同语义）
- **生效路径（四条全走 cascade，语义一致）**：装载（`load`——`agent_load`/WS `sub_agent.load`/预加载/路由自愈统一入口，逐个幂等装载并返回实际装载集合，`loadAgentsForSession` 为每个连带装载的依赖同样写入提示词 system 消息）、`subsession_run` 预加载校验（`normalizeRunAgents`——同步 `runSubSession` 与异步 `SubSessionRegistry.start` 的 validate 共用，展开后名单上限检查按展开后计）、子会话循环内装载（`registerIntoRegistry`——路由自愈/显式 `agent_load` 的注册路径）、构建期包含清单（`build-subagents.ts` 依赖闭包展开，见「选择性打包」）
- **引用计数语义**：级联装载的依赖以**装载方同一 owner** 记入引用表（会话 A 装载 `reverse_site` ⇒ A 隐式引用 `playwright`）——卸载装载方**不连带卸载依赖**（A 卸载 reverse_site 后 playwright 的 A 引用仍在，直至显式卸载）；依赖被多方装载时工具注册按 owner 解引用保留（与装载工具会话可见性同机制）
- **文件读写等全局工具不构成依赖**：全局工具对所有会话恒可用（`subsession_run` 默认继承），无需也不应声明为依赖；依赖机制用于**子Agent 独有能力**的复用（如 reverse_site 复用 playwright 的浏览器操控）

#### 路由自愈（未装载命名空间工具自动装载）

模型偶尔跳过 `agent_load` 直接调用未装载子Agent 的 `{agent}_*` 工具（长会话遗忘/弱模型跳步），旧版该调用以「未知工具」失败浪费一轮。现引擎在两个循环的解析失败点做**自动装载自愈**（`engine.subAgentForToolName` 最长 `{agent}_` 前缀匹配）：

- **主循环**：按 `agent_load` 同路径自动装载（`ctx.loadSubAgent`——工具注册进全局注册表 + 提示词写入会话记录 + 拼接进当前上下文系统前置段），随后重解析执行该调用；工具结果前置说明「引擎已自动装载子Agent X」，模型知晓能力已可用
- **子会话循环**：走 `loadAgentIntoRun`——工具注册进**本次运行注册表** + 提示词段落插入临时 messages 系统前置段 + 预置项目并入 `ctx.projects`，**不写全局注册表、不落盘父会话记录**（子会话运行隔离语义：子会话内的装载不外溢）；显式 `agent_load`（纯 md 组合子Agent 注入的编排工具）在子会话内同样双注册（`buildContext.loadIntoRegistry`），装载后工具立即可解析——修复了旧版「子会话内 agent_load 只注册进全局注册表、本次运行不可见」的断层
- **不可恢复时给出恢复面**：自动装载后仍不可解析（拼错工具名等），错误信息附该子Agent 的可用工具全名清单（`unknownToolMsg`），一步引导修正
- **既有防线不放宽**：自动装载后的调用照常走通道禁用/安全模式/审批全部门禁；主动 `agent_load` 仍是首选路径（系统提示词引导不变），自愈仅为兜底

#### 选择性打包

构建时按需选择打进二进制的子Agent 集合，支持三种方式（可组合）：

| 方式 | 配置 | 说明 |
|------|------|------|
| 全部打包 | 默认 | 打包 `@gebai/agents` 下所有子Agent |
| 白名单 | 构建脚本设 `GEBAI_BUILD_SUBAGENTS=a,b,c` | 仅打包指定子Agent（依赖闭包自动展开），控制二进制体积 |
| 预加载 | 构建脚本设 `GEBAI_BUILD_PRELOAD=a` | 烘焙 `def.preload=true`（启动即装载；运行时 `GEBAI_PRELOAD_SUB_AGENTS` 仍优先） |

- 未打包的子Agent 在运行时不可见（`agent_list` 不列出、无法装载/派生子会话），相当于彻底裁剪该能力
- 打包清单支持按发行形态差异化（桌面版全量、服务端精简版等），同一套源码产出不同规格二进制
- 实现状态：三个环境变量均已接通（`GEBAI_BUILD_SUBAGENTS`/`GEBAI_BUILD_PRELOAD`/`GEBAI_BUILD_EXCLUDE_TOOLS`，见「构建期裁剪与预加载指定」）；缺省全量打包，清单里的未知名字构建直接失败（防产物静默缺失）

#### 选择性预加载

- **预加载**：启动时（或会话创建时）自动**装载**的子Agent 模块（模块语义，见「装载 vs 子会话运行」），其工具进入总Agent 工具集、完整系统提示词作为 system 消息写入会话记录（`loadedSubAgents` 名单 + `loadedAgent` 提示词消息持久化）
- **按需装载**：默认未预加载的子Agent 仅注册在目录中，总Agent 通过 `agent_load`（或 WS `sub_agent.load` 带 sessionId）按需装载——装载即写入当前会话记录（工具注册 + 提示词消息；**同名子Agent 已被其他会话装载同样写入**——痕迹判定只看本会话记录），后续恢复会话时自动还原
- 通过 `preload` 字段或环境变量 `GEBAI_PRELOAD_SUB_AGENTS`（逗号分隔）声明预加载集合；未声明者**默认不预载任何子Agent**（按需装载）
- 预加载少而精：控制系统提示词与工具集规模，降低模型选择噪音；高频/核心子Agent 预加载，低频/重型子Agent 按需装载
- **内置子Agent 默认全部不预加载**（`preload = false`），完全按需装载；部署方可用 `GEBAI_PRELOAD_SUB_AGENTS` 声明预加载集合
- 会话级可通过环境变量（会话内存态或浏览器本地注入）覆盖预加载集合，按会话定制

#### 子Agent 启停名单（运行时可用性收敛）

- 环境变量 `GEBAI_SUB_AGENTS_ENABLE`（**白名单**，逗号分隔）与 `GEBAI_SUB_AGENTS_DISABLE`（**黑名单**）：启动 `discover()` 后经 `SubAgentManager.applyEnableDisable` 一次收敛——白名单非空时未列出的全部 `unregister`，黑名单移除名单内；两者同时配置**先白后黑**（黑名单最终生效）
- `unregister` 语义复用 `GEBAI_TASKS_ENABLED=false` 的既有机制：已装载/预载的连带卸载工具注册（注册表不残留「模型可见但引擎不可用」的工具）、`agent_list`/系统提示词的「可选子Agent」清单、`subsession_run`/`agent_load` 名字校验、环境变量目录（`envVars` 声明面）随之完全不可见；**热加载重扫后保持移除**（`removedDefs` 过滤防「复活」）
- 与构建期选择性打包（`GEBAI_BUILD_SUBAGENTS`，见「选择性打包」）互补：打包裁剪产出精简二进制（体积收益，不可运行时恢复），启停名单在完整产物上**按部署收敛能力面**（重启改环境变量即恢复）；名单中的未知名（拼写错误、或该形态未打包）启动 `console.warn` 告警忽略，不阻断启动
- 预载名单（`GEBAI_PRELOAD_SUB_AGENTS`/`def.preload`）命中被移除的子Agent 时启动告警（该子Agent 不再预载；会话装载保障按未知子Agent 跳过，不中断任务）
- **启停过滤是实例级视图，不写进程缓存**：`discover()` 的进程级扫描缓存（`discoveredDefsCache`）存**未过滤全集**，`removedDefs` 过滤仅作用于当前实例的 defs——否则一个实例的启停策略会泄漏给同进程所有后续实例（多管理器场景下跨实例污染）

#### 选用原则：默认「装载」而非「子会话运行」

**选用原则：默认「装载」而非「子会话运行」。** 默认用 `agent_load` 把子Agent 装载进总Agent（工具注册进工具集、完整系统提示词注入当前上下文，模块语义、无独立执行），总Agent 直接使用其 `{agent}_` 工具，过程全程可见、无结果截断损失；`subsession_run`（无需装载，会话语义）仅在下列场景使用：

| 场景 | 形态 | 原因 |
|------|------|------|
| **需要干净上下文** | 隔离（缺省） | 子任务结果隔离：独立执行、只返回最终结果，中间过程与工具调用不进父上下文，避免污染总Agent 的推理链 |
| **防止上下文膨胀** | 隔离（缺省） | 子任务中间过程多、输出大：独立上下文执行并自行截断（结果超阈值按上下文保护规则落盘），父上下文只保留最终结果摘要 |
| **同一任务并行多路** | 继承（`inherit_context:true`） | 多方案对比/多文件并行修改/多角度调研：各子会话掌握父会话全部背景与工具，报告自动合入（详见「子会话运行」） |
| **长任务不阻塞** | 任一形态 + `async:true` | 构建/批量分析等长任务后台执行，父会话先做别的，`bg_task` 回头收结果（见「子会话运行」） |

### 子会话运行（`subsession_run`，统一父子会话模型）

**一个入口、一套注册表、一条执行路径**：`subsession_run` 按 Linux 父子进程语义统一「隔离子会话（spawn）」与「继承子会话（fork）」——两者只是 `inherit_context` 的取值差异，不再是两套机制：

| 形态 | `inherit_context` | 上下文 | 结果交付 |
|------|------|------|------|
| **隔离子会话**（spawn） | `false`（缺省） | 新建上下文：子Agent 提示词（+ 全局提示词） + 全局工具 + 子Agent 工具 + 编排工具 | 同步：最终结果作为工具结果返回；异步：`bg_task`（id 前缀 `s`）取回 |
| **继承子会话**（fork） | `true` | 父消息历史 + 父系统提示词（+ 子会话附注） + 父工具面快照（+ 追加预加载子Agent 工具） | 最终报告**自动合入父上下文**（合并消息 + 过程存档）；同步另回概要与 `bg_task` 均可见 |

- **参数面**：单任务形态 `input`（+ 可选 `agents`/`model`）与多任务并发形态 `subsessions`（每项 `{ name?, input, agents?, model? }`，1-8 项）**二选一**（同时给出报错）；调用级开关 `inherit_context`/`async`/`merge`/`inherit_global_tools`/`inherit_global_prompt`/`timeout` 盖章到本批全部子会话；`agents` **可省略/为空 = 不加载任何子Agent**（纯编排子会话：全局工具 + 编排工具即可完成搜索/汇总/改写类任务）；`timeout`（秒）为**运行时限**：到时进入**快速结束**（注入收敛指令给宽限拿结论，逾期才强制终止，见「子会话快速结束」）而非硬杀
- **继承形态的 fork 点**：`subsession_run` 工具调用时的父上下文（含已装载子Agent 的提示词与工具）——fork 快照**同步切片**（父任务此刻阻塞在本工具调用内，消息数组稳定，或并发场景下以切片时刻为准）；快照尾部可能带尚未应答的 `assistant(toolCalls)`（本轮工具批处理进行中），为悬空 toolCall **合成占位 tool 结果**（严格校验的接口要求 tool_calls 后紧跟 tool 响应），再追加子会话任务指令（user 消息）。子会话系统提示词 = 父会话同一系统提示词（单源复用）+ 子会话附注（并行职责/并发写提醒——其他子会话可能并行修改同一文件，写入前先读最新内容/不向用户提问/完成后输出最终报告）。工具面 = 父会话注册表视图快照（全局工具 + 已装载子Agent `{agent}_` 工具 + 会话动态工具）；子会话内装载子Agent 只进本子会话（隔离语义，不写父会话记录）；**已读追踪 fork 快照**——子会话拷贝 fork 点的会话级已读表（防盲写/防陈旧覆盖守卫的隔离：fork 后父会话/兄弟子会话的读写互不串扰，见「防盲写守卫」）
- **隔离形态的组装**：开场白说明隔离语义与预加载名单（未预加载时明确「未预加载子Agent」）、全局工具继承说明（`inherit_global_tools`）、编排指引（`inherit_global_prompt=false` 时补兜底版）、安全模式注记、预置项目清单与项目绑定注记
- **多路模型接口（模型路由）**：`model` 参数按名解析——命中 `GEBAI_LLM_ROUTES` 配置的**命名路由**（JSON：`{"路由名": {"model", "api_base"?, "api_key"?, "api_kind"?, "max_context"?}}`）走独立端点/模型；未命中视为字面模型名（主配置基准覆盖）；缺省沿用任务级模型。多子会话各走各的 Provider 并行，分摊单路模型服务的限流与串行速度限制。路由表可配在进程 env 或会话/任务级 env（前端 env 面板同样生效）
- **合并（继承形态，自动）**：子会话正常完成即把最终报告构造为**合并消息**（role=`user` + `engineNote: "subsession"`——思考类模型不接受以 assistant 结尾的历史；内容头 `【智体·子会话「名」已合并】` 身份标记（来源自描述） + `subSessionMerged` 标记（含 runId/名/模型）+ 完整过程存档 `SubSessionArchive`（`subsession` 字段标识，历史回放渲染「🌿 子会话」折叠容器））合入父上下文：任务运行中入**引擎合并队列**（`parentMerges`），runLoop 在**工具批处理边界排空**（位于本轮 tool 结果之后追加——`assistant(toolCalls)`→`tool` 配对完整，父会话下轮模型调用即见）；异步子会话晚于父任务结束时直接落盘（下次 run 经 loadHistory 进上下文），任务收尾（run finally）冲刷队列防丢失。报告超长保留头尾截断（`SUBSESSION_MERGE_MAX_CHARS`，纯上下文保护）。失败/被终止的子会话**不合入**（过程保留在存档，`bg_task` 可取回回放）
- **合入粒度（`merge` 参数，调用级）**：`full`（缺省）=报告全文合入（上列行为）；`summary`=**摘要合入**——报告超过 `SUBSESSION_MERGE_SUMMARY_SKIP_CHARS`（1500 字符）时先经任务级模型压缩为「结论+关键发现+产物清单+建议」要点（`summarizeSubSessionReport`，复用压缩同款读空闲超时防护；短于阈值不值得一次模型调用，原文合入），合并消息头行带「（摘要合入）」标记并附「全文见子会话过程存档」注记——**父上下文只进摘要，全文保留在过程存档/`bg_task` wait 可取回**；摘要失败/空结果全文兜底（合入不因摘要失败而丢失）；子会话多/报告长时保父上下文预算（多轮合入不打爆父会话）。同步等待覆盖合入完成（注册表 done promise 在合入回调 settle 之后——工具结果返回时摘要合并消息必已入队，父会话下一轮即见）
- **双向同步（`subsession_merge` 工具，仅异步子会话运行注册——子会话唯一协作工具，双向一个入口）**：**传 `content` = 交出**——阶段性成果（重要结论/产物清单/对其他子会话有用的发现）立即合入父会话（同一合并队列/落盘路径，内容头行 `阶段性合入`、不带过程存档：子会话仍在执行、存档为活引用，完成时的最终合并才携带完整存档），广播其他子会话，**子会话继续执行不受影响**、可多次调用；**不传 = 拉取**——返回父会话自 fork/上次同步以来的**全部新消息**（父会话用户输入/回复、其他子会话合入全文 `【合并·名】`、父会话工具结果摘要，逐消息 1200 字符、工具摘要 600）。两种用法均返回父会话增量快照（`parentSnapshot`）——**合入即感知**，交出后立刻看到父会话新进展。水位（`forkAt`/`syncedAt`，存储消息数）推进保证增量式（每次只返回新内容）；合并队列中未落盘的合入（同步等待期间/父任务收尾前）一并回显并按消息 id 登记已投递（防其落盘后在下次增量重复出现）；本子会话自己的合入跳过（内容自产）。fork 水位在 fork 组装时记录（此刻存储尾部即 fork 快照的持久化等价——在途 tool 结果尚未落盘，不属父会话可见内容）。**同步运行不注册本工具**（父会话正阻塞等待，阶段性合入既无收益又与最终交付重复；调用返回不可用说明），父会话内/隔离子会话之外无此工具
- **互相感知（父会话通知注入 + 按需同步）**：子会话 fork 快照后父会话仍在演进，双向感知靠**两条通道**——①**通知注入**（自动、紧凑）：任何合入（阶段性/最终）与父会话每轮最终回复（异步场景；同步等待期间父会话阻塞在 `subsession_run` 工具内无此交错）都向**本会话其他运行中子会话**的收件箱（`SubSessionHandle.inbox`）压入通知（`【子会话感知】子会话「名」…合入父会话` / `【父会话进展】父会话回复`，截断 `SUBSESSION_NOTICE_MAX_CHARS`），子会话执行循环**每轮轮首排空**注入为 user 消息（位于上一轮 tool 结果之后，tool_calls 配对不破坏；入子会话存档可回放）；②**`subsession_merge` 按需同步**（完整）：通知只给摘要，需要父会话完整内容（如依赖兄弟子会话详细发现做决策）时增量拉取全文。子会话感知父会话与兄弟子会话的进展后可调整分工（避免重复工作/冲突）；发起方不收到自己的通知（按 runId 排除）
- **同步 / 异步**：默认阻塞等全部子会话终态（工具结果返回概要——各子会话名/状态/轮次/工具调用数，隔离形态附最终结果、继承形态报告在随后的合并消息中）；`async: true` 后台执行——立即返回 runId（`s` + 8 位 hex），父会话继续其他工作，子会话完成自动合入（继承形态）或经 `bg_task` 取回（隔离形态）并实时推送前端
- **父会话控制（`bg_task`，id 前缀 `s` 分发到子会话运行；`t` 前缀走 sh 命令任务分支）**：`status` 立即返回运行状态与**进度快照**（已执行模型回复轮次/工具调用次数/最近一条存档条目尾部——从存档活引用实时推导：引擎创建运行存档后即将同一引用交给运行句柄（`archiveHolder`），**运行中即可见并随执行增长**；`result()` 仍在运行中返回 `undefined`——终态结果语义）；`wait` 阻塞等待完成并取回最终结果与**完整存档**（挂到 `bg_task` 执行记录扩展字段供历史回放；timeout 秒内未完成返回当前进度可再次 wait，默认/上限 60 秒——上限压到 1 分钟强制按进度轮询）；`stop` **主动终止**（abort 传播进执行循环，终止前的执行过程保留在存档可回放）；`finish` **快速结束**（先礼后兵：注入收敛指令让子会话停止扩展性工作、按已有信息给出结论并自然结束——报告照常交付/合入；`reason` 写入指令、`timeout` 作宽限秒数，逾期才强制终止；状态行标「收尾中」）；`list` 列出本会话全部子会话运行（含命令任务混排，按 id 前缀标注形态）
- **待办隔离**：子会话的待办是**运行内内存清单**（不落盘、不回流、不继承父会话待办）——`todo` 工具在子会话内读写自己那份清单（跨轮次可见），`event.todo.update` 携带 `subSession: true` + `subSessionId` 标记，前端**不**把它灌进父会话待办面板（卡片在子会话容器内由工具结果渲染）
- **实时可见**：子会话执行过程与主循环同构推送（`subSession: true` + `subSessionId` = runId 的 delta/reasoning/tool 事件 + `event.subsession.start`（run 开始，含 `subsession`/`agents`/`input`/`model`/`depth`；**每轮重推、同 runId 幂等**，前端容器重建兜底）与 `event.subsession.done`（run 结束，含最终输出与错误）），前端渲染「🌿 子会话 · 名（模型）· ⚙ 子Agent」折叠容器（`details.subsession-run`：执行中展开并滚动到可见、容器内限高独立滚动 + 粘底自动跟随，结束后自动折叠只显示输入与最终返回，点 summary 展开看完整过程；历史回放渲染同样的容器，嵌套子会话递归渲染进外层容器 body）；合并时推送 `event.subsession.merged`（含合并消息全文），前端实时渲染「子会话合入」通知条（消息**落盘即 `role: "user"` + `engineNote: "subsession"`**，与其余引擎注入同口径——注入点紧贴模型调用，assistant 形态会被思考类模型 400 拒绝；见「引擎注入消息的角色约定」）。渠道层可据此区分「子会话过程」事件（如飞书渠道对子会话的 `done` 不触发最终卡片——任务结束以 `event.task.done` 为准）
- **执行记录扩展字段（存档）**：子会话运行的**全部过程内容**（输入/每轮回复/推理/工具调用与结果，含嵌套子会话存档）收集为 `SubSessionArchive`（`runId`/`agents`/`input`/`output`/`subsession`/`messages`），由 `subsession_run`（继承形态：挂在合并消息上；隔离同步：挂工具结果）或 `bg_task wait`（异步）作为**执行记录的扩展字段**（`Message.subSessionArchive`）随结果落盘——不逐条写独立消息：会话文件只有一条携带完整过程的记录，历史回放据此渲染折叠容器（结果卡片输出按 **markdown 渲染**；工具卡片气泡对 markdown 容器恢复 `white-space: normal`——气泡的 pre-wrap 面向纯文本参数/输出，markdown 软换行已是 `<br>`，markdown-it 在 `<br>` 后保留的源换行若再按 pre-wrap 折行，每行之间会多出一个空行）；仅存档与前端回放：`loadHistory` 跳过（**不进入父 LLM 上下文**，上下文隔离不变）、上下文压缩与 ctxTokens 估算同样排除（压缩时带存档的记录原位保留，存档不随压缩丢失）；父会话的 `subsession_run` 执行卡片与最终结果仍属父上下文（模型可见执行与最终结果）；旧版逐条 `subAgent` 存档消息与 `agent_call` 时代存档（`LegacySubAgentRunArchive` 单 Agent 形态）仍兼容回放（历史会话不受影响）
- **实现与生命周期**：`core/session/subsessions.ts` 的 `SubSessionRegistry`（**引擎级共享句柄表**（`engine.subSessionStore`）+ 按视角过滤的薄视图，`buildContext` 注入 `ctx.subSessions`——**子会话内同样注入，指向该子会话自己**（视角 = 自己的直接子会话，孙代不越级可见），因此子会话内可再派生子会话（进程树，深度上限 3）；唯一的执行路径 `engine.runSubSession`（隔离/继承两形态组装后进入 `runSubSessionLoop`，与主循环同构：审批/重复检测/轮次上限/流式推送/截断/事件，差异只在不落盘会话消息而收集进存档、不设独立超时、待办为运行内隔离清单））；进程内异步任务，随服务重启中断（不落盘恢复）；父任务取消信号连带终止子会话（用户停止/审批拒绝，逐级传播）；会话删除连带清理（`forgetSession`——运行中先终止、句柄与合并队列移除）；审批等待遇任务已结束（异步子会话晚于父任务触发审批）按超时跳过而非崩溃；**在途流式快照隔离**：异步子会话与父任务**真正并行**（同步运行期间父任务在等工具、不流式，无此交错）——`noteStream` 对 `subSession` 标记的增量在父任务快照流式期间不写入（按 messageId 开新快照会互相整体替换，attach 恢复可能把子会话文本渲染进父任务气泡；子会话进度已有 event 推送，快照仅为 attach 兜底，父任务流式期间以父任务为准）；`clearStream` 按 `subSessionId` 定向清空（子会话轮末不误清并行父任务的在途快照）；**快速结束（先礼后兵）**见下节

#### 子会话快速结束（先礼后兵）

终止一个正在推进的子会话有两条路：**硬杀**（abort 传播进执行循环：已跑出的结论一并丢掉，只剩过程存档）与**快速结束**（注入收敛指令让模型自己收尾）。后者才留得住产出，故作为超时与收尾的缺省路径：

- **收敛指令（`subSessionFinishPrompt`）**：以 user 消息注入子会话收件箱（与其他通知同一通道——执行循环**轮首排空**，位于上一轮 tool 结果之后、tool_calls 配对完整，子会话下一轮模型调用即见），要求：停止一切新的探索与扩展性工作、不再派生子会话、不再启动长耗时操作，在宽限秒数内**直接输出最终回复**（不再调工具），内容按已有信息组织为「已完成的结论与产物（文件:行号/命令/链接）+ 尚未来得及做的部分 + 若要继续的下一步」，并如实说明不确定性
- **宽限与兜底**：触发即启动宽限计时（`SUBSESSION_FINISH_GRACE_MS` 缺省 120 秒，传值夹取到 1s~600s）——宽限内模型自己收尾则终态 `done`（报告照常交付/合入）；逾期仍在运行则**强制终止**（与 `bg_task stop` 同口径 `cancelled`，过程保留在存档）。已在收尾/已结束的重复触发为幂等（不重复注入）
- **三个触发入口**：① **主会话主动**——`bg_task action=finish`（`reason` 写明结束原因并随指令下发；`timeout` 作宽限秒数；状态行显示「收尾中」）；② **运行时限**——`subsession_run` 的 `timeout`（秒），到时触发快速结束（不是硬杀）；③ **会话任务超时收尾**——任务执行超时（提示词型，`TASK_PROMPT_TIMEOUT_MS`）不直接 `cancel`，而是 `engine.windDown(sessionId, {reason})`：批量快速结束本会话运行中的子会话并等宽限（子会话全部结束或宽限到期即止），再取消会话任务本身——取消必然发生（兜底计时器防个别运行不 settle 拖住），等待期间新派生的子会话不再开第二轮宽限（由父任务停止传播一并终止）
- **不改变主动停止语义**：用户停止（停止按钮 / REST `POST /api/v1/sessions/:id/cancel` / WS `session.cancel`）与 `bg_task stop` 仍是即时硬终止（显式终止要立刻生效）；快速结束是「因超时才收尾」的路径，不与主动停止混用
- **门禁**：单次调用子会话数 ≤ 8（`SUBSESSION_MAX_PER_CALL`）；单会话并发子会话运行 ≤ 8（`SUBSESSION_MAX_CONCURRENT`，**全树合计**——子会话里再派生同样计入本会话并发，超限拒绝并引导清理：`bg_task stop` 终止或等待）；继承深度 ≤ 3（`SUBAGENT_DEPTH`，启动前同步校验）；名字缺省 `s1..sN`、批内唯一、≤32 字符不含空白（中文名合法）；终态记录保留最近 20 条（`SUBSESSION_KEEP`，超出淘汰最旧，运行中不淘汰）；预加载清单**同步校验**（去重/`self_optimize` 连带 `code`/数量与深度上限/未知名检查，`engine.normalizeRunAgents`，同步/异步启动共用——未知名等错误立即回给模型而非留下幽灵运行）；参数与环境问题（形态二选一/数量超限/并发超限/未知名）作为工具结果文本回传，模型可修正；轮次上限与主循环同规则（不设上限）；`bg_task` 管理动作免审批（查询/等待/终止本会话后台任务）

#### 子会话运行安全限制

- **递归深度上限**：子会话内再派生子会话的嵌套深度受限（默认 3 层，进程树深度，主任务为 0），防止无限递归；异步后台运行同规则（启动前同步校验）
- **超时与快速结束**：子会话**缺省不设整体超时**（执行过程实时推送到前端、进度可见，无「无反馈空转」问题）；显式设 `timeout` 时到时走**快速结束**（注入收敛指令给宽限拿结论，逾期才强制终止，见「子会话快速结束」）。中止路径：父任务取消传播（用户停止，异步运行连带终止）、`bg_task action=stop`（即时硬终止）与 `action=finish`（先礼后兵）；挂起工具仍受工具级超时兜底（`TOOL_TIMEOUT_MS`）保护
- **轮次上限**：子会话内部工具调用轮次与主循环同一规则（不设上限，见常量参考；失控防护靠重复检测终止与父任务取消传播）
- **结果大小限制**：合入父上下文的报告按 `SUBSESSION_MERGE_MAX_CHARS` 头尾截断；隔离形态结果作为工具结果超过截断阈值时按上下文保护规则处理
- **并发上限**：单会话并发子会话运行 ≤ 8（`SUBSESSION_MAX_CONCURRENT`，全树合计，超限拒绝新运行并引导清理）；终态记录保留最近 20 条（`SUBSESSION_KEEP`）

#### 工具与命名空间

子Agent 工具注册进总Agent 工具集时采用 **`{agent_name}_{tool_name}`**（单下划线）命名空间，路由按「全局精确匹配 → 子Agent 前缀匹配」两步解析：

**为什么选单下划线（对弱模型最友好）**

- 下划线是工具名中最常见的分隔符，弱模型回显 `agent_tool` 的准确率最高；`--`（双连字符）易被缩写成单 `-`，`.`/`:` 易被改写或直接报错，均不适合低性能模型
- 命名空间分隔符不依赖模型理解，模型只需把 schema 里的名字原样回显即可

**命名约束（消除解析歧义）**

| 名称 | 规则 | 说明 |
|------|------|------|
| 子Agent 名 | `[a-z0-9_]+` | 小写字母/数字/下划线 |
| 子Agent 工具名 | `[a-zA-Z0-9_]+` | 不含 `.`/`-`/`:` |
| 全局工具名 | `[a-z][a-z0-9_]*` | 小写开头，不含 `.`/`-`/`:` |
| 总长度 | ≤ 40 字符 | 弱模型易截断长名，`{agent}_{tool}` 总长严格受控 |

**参数命名契约（蛇形）与模型误差容错**

- **全部工具输入参数名一律蛇形（`[a-z][a-z0-9_]*`）**：模型按 schema 原样回显参数键，驼峰/连字符键（`oldString`/`full-page`）是弱模型的高频出错形态，参数面统一蛇形与工具名同一约定（描述文本中引用参数名同样用蛇形，提示词不再教驼峰名）
- **容错层（`core/base/tool-args.ts`，三派发点共用）**：引擎主循环、子会话循环与 js 脚本 RPC 桥在解析/执行前对模型生成的调用做两层归一——①**工具名**：`.`/`-`/`:` 分隔符与驼峰拆点归一蛇形小写（`agent.run`/`agentRun` → `subsession_run`，已合规名恒等）；②**参数键**：按工具 schema 的属性键做**指纹匹配**（去除非字母数字 + 小写，`oldString` ≡ `old_string`），嵌套对象与数组元素按子 schema 递归，schema 未声明的键原样透传（任意透传参数不受影响）、指纹歧义（两 schema 键同指纹）只认精确匹配
- **归一时机在落盘/入上下文之前**（两循环的 assistant(toolCalls) 持久化前）：历史记录、事件推送与实际执行同用规范名——模型的风格误差一次自愈，不以「未知工具/缺少必填参数」失败浪费往返
- **不在归一范围**：工具输出 `data` 字段（消费侧为代码读取，驼峰无害且为既有契约）、跨端协议键（WS 载荷如捕获 `fullPage`、playwright 桥/driver 协议键）、内部 TS 接口与存量持久化数据（任务的 `timeoutMs` 等接口字段，入参蛇形读取后在工具内映射）；B 级嵌套约定字段（wps style/theme 等仅 description 声明的字段）读取端做「蛇形优先 + 旧驼峰兜底」双读

**两步解析（服务端确定无歧义）**

1. **全局精确匹配**：先查全局工具表，命中即全局工具
2. **子Agent 前缀匹配**：遍历已注册子Agent，取「最长匹配的 `{agent}_` 前缀」，剩余部分为工具名——注册表固定，解析结果确定，任何 `a_b_c` 形态都有唯一解

**注册期碰撞检查**（注册时校验——构建期 bundle 注册走同一路径）

- 全局工具名不得以任何 `{agent}_` 开头（与子Agent 命名空间互斥）
- 子Agent 名不得互为前缀（如 `web` 与 `web_search` 同时存在会引入前缀竞争，构建时报错）

**弱模型容错兜底**

- 精确查找失败时做**分隔符归一化重试**：将工具名中的 `-`、`.`、`:` 全部替换为 `_` 后重查，覆盖弱模型的分隔符误写
- 仍失败则返回明确错误（`unknownToolMsg` 附**该子Agent 可用工具全名清单**），引导模型下一轮修正，不静默吞错

**透明转发**

- 子Agent 内部只见自身工具名 `tool_name`，schema 注入时由总Agent 加 `{agent}_` 前缀，调用时按上述两步解析后透明转发，对子Agent 完全无感
- 同一子Agent 可被多个会话同时加载使用，无共享可变状态（工具函数为纯函数或注入依赖）

**工具卡片展示元数据（`Tool.card`，注册时声明）**

- 工具定义可声明 `card: { titleParams?, args?, codeField?, codeLang?, file?, taskIdParam? }`，经 `session.tool.get` 随 ToolInfo 下发，前端按声明渲染工具调用卡片，**不在前端硬编码工具名**：
  - **卡片头部为结构化布局**（图标：运行中**信号灯闪烁**（accent 圆点，与标题栏信号灯同款 `conn-thinking` 不规则闪烁，低性能模式同样保留）/ 完成 `✓` / 历史调用 `🛠` + 工具名 + 标题参数后缀三个 span）：标题参数**始终入头部**——短值直接入标题，超长单值（>48 字符）**智能截断**（路径型保留尾部、URL 保留头部、其余保留首尾，卡片头单行省略号），截断时**悬浮 title 见全文**；实时调用、完成态更新（`appendToolResult` 重建头部）与历史重载三态共用同一渲染入口（`toolHead`）
  - `titleParams`：参数名列表，其值直接拼入卡片标题——**实际存在单参数仅显示值**（`read · src/main.ts`，省略 `key=` 前缀；按实际传入数而非声明数判定——声明多个但本次只传一个时同样裸显），多参数 `key=value`（`·` 连接）；简单工具的关键参数（如 `open` 的 url、`write` 的 path）一目了然（**路径始终可见**——项目绝对路径再长也截断入头部而非整体丢弃，edit/read/write/patch 卡片头始终能看到目标文件）；**`project` 参数自动入卡片头**：`projectAware` 包装的工具（全局文件工具与 code/explore 独有工具）在 titleParams 前插 `project`——模型传了 project 时卡片头显示 `project=todo-app · path=src/main.ts`（相对路径不再缺项目上下文），未传时前端跳过缺失参数、卡片头形态不变（project 同时从参数区移入头部，不重复展示）
  - `args`：参数区模式——**缺省自适应**（扁平标量参数渲染为**键值行**：参数名 + 值 pre-wrap 展示，比 JSON 块更可读；嵌套结构回退 JSON 语法高亮）/ `"json"`（强制完整 JSON 高亮，标题参数不省略）/ `"kv"`（强制键值行，嵌套值紧凑 JSON 单行展示）/ `"none"`（不展示参数区，如 `env_detect`）/ `"code"`（`codeField` 参数渲染为语法高亮代码块，其余参数键值行/JSON 附注，如 `sh` 的 command；**超长内容同样先渲染后自动折叠**，不整块直显）/ `"edits"`（`codeField` 数组参数的 `{old_string,new_string}` 项渲染为**旧（红）/新（绿）对比块**；前端只读蛇形键，风格兼容统一收敛在后端归一层——多处修改编号「修改 i/n」，空串侧省略（纯新增/纯删除），形态不符回退自适应渲染；`edit` 工具因此不再显示 JSON；**edit 工具 card 声明不可用时（工具清单拉取失败/旧服务端未声明）前端按参数形态内建兜底同样渲染对比块**，长短修改一律先渲染后按阈值折叠，不回退直显 JSON）/ `"block"`（**结果直出内容块**：调用不显示通用工具卡片，结果直接渲染 blocks 内容块，如 `show`/`diff`）
  - `file`（文件卡声明，「文件展示方式」设置的**弹窗查看**模式据此渲染）：路径参数名（如 read/write/edit/patch 的 `path`，code 子Agent 同款包装自动继承）——声明本工具的**产物 file 块**（下方文件内容卡）在弹窗查看模式下收敛为**文件链接 chip**（图标 + 文件名 + 路径 + 下载，点击弹窗查看文件）；**参数区与输出不受影响**（write 的 content 代码块、edit 的旧/新对比块、patch 的 diff 块、read 的输出照常渲染），图片/图表等视觉产物块照常内联（仅 file 块切换形态）；块路径为服务端解析后的真实路径（会话 `tmp/` 逻辑或项目绝对路径，见「文件预览」）。脚本桥（js/py）透传的 file 块没有可归因的外层工具名，按**块上的来源工具名**（`via`）走同一判定——桥内调用文件工具与直接调用一致，桥内的 `show` 产物照常内联
  - `taskIdParam`（后台任务工具声明，如 `bg_task` 的 `id`）：该参数值为**后台任务 id**——前端在标题参数之后**补上任务身份**（`bg_task · action=wait · id=s9c2e1b0 · 子会话「调研A」`；命令任务为 `· 命令 <命令首行>`），超长身份同标题参数规则智能截断（悬浮 title 见全文），id 未登记时只显示原参数（不凭空补）。身份由**工具结果文本**登记（`task-labels.ts`：后台启动结果与任务状态行的 `taskId t… / runId s…` 行；实时结果到达即登记，历史加载时先扫一遍会话消息含子会话存档——消息窗口化按需渲染，不能依赖「启动卡先渲染」的顺序兜底）。**任务 id 本身无信息量（`t`/`s` + 8 位随机）、等待中的卡片尚无输出，补身份后等待中与完成态都能一眼看出在等什么**
  - **标题参数不在参数区重复展示**（titleParams 已入标题的键从参数区省略，如 `read` 参数区只显示 offset/limit；全部参数入标题时无参数区）；**超长参数区折叠只发生在完成态**（>800 字符收敛为「查看参数（N 字符）」折叠块，默认收起，点击展开，与输出折叠同款交互）——**执行/审批等待期完整直显不折叠**（实时调用卡（运行中信号灯闪烁）带全量参数，执行与审批时可查看全貌），结果到达（`✓` 完成态，appendToolResult）时收敛为折叠块，与历史回放同构；subsession_run/subsession_run 专用参数块不参与折叠
  - 已声明示例：全局 `sh`/`py`/`write`（code 模式 + 文件卡）、`edit`（edits 模式 + 文件卡）、`read`（文件卡）、`fetch_url`/`agent_load`/`subsession_run`/`bg_task`/`ls`/`grep`/`glob`/`file`（标题参数，`bg_task` 另声明任务 id）、`patch`（code 模式 + 标题参数 path + 文件卡）、`show`（block 模式）；code `search_symbols`/`analyze`（标题参数）、`env_detect`/`system_info`（无参数区）；desktop `screenshot`/`window_focus`/`key_press`/`window_list`/`clipboard_read`/`screen_info`/`ocr`/`locate`/`detect`；playwright `open`/`new_page`/`serve_dir`/`switch_page`/`close_page`/`press`/`pages`/`close`/`ocr`/`locate`/`locate_image`
  - **`subsession_run` 专用卡片**：卡片头部直接列出**全部预加载子Agent 名**（`subsession_run · code + playwright`，以 `+` 连接、不截断、省略 `key=` 前缀——后缀 span 带 `wrap` 标记允许多行完整展示）；参数区**只显示输入提示词**（任务指令全文、pre-wrap 展示，不参与自动折叠）；实时卡片完成态（`✓`）保留标题后缀，参数区不因执行完成而消失（执行中与完成后均可见）
  - **`subsession_run` 专用卡片**：卡片头部直接列出**各子会话名**（`subsession_run · 左路 + 右路（fast）`，`+` 连接、带模型路由后缀、wrap 多行；缺省名按服务端规则 `s1..sN` 补，与合并消息命名一致）；参数区**按子会话渲染小节**（每节「🌿 名（模型路由）」头 + 任务指令块，指令块复用子会话输入样式），并附运行形态提示行（继承/隔离上下文、async 后台执行、merge 摘要合入）；**不声明 card 元数据**——`subsessions` 为对象数组，通用 titleParams 路径会渲染 `[object Object]`，专用卡片由前端全权接管（单任务形态 input+agents 同一路径渲染，工具卡片测试覆盖实时/历史/空清单三态）
  - 元数据拉取失败或未声明时按默认渲染（标题仅工具名、参数区按缺省自适应规则），不影响功能

### 内置子Agent 示例：通用源码分析/修改（`code`）

作为子Agent 扩展机制的落地示例，内置「通用源码分析与修改」子Agent，定义于 `packages/agents/src/agents/code/index.ts`（@gebai/agents 包），面向**其他项目**（本地使用场景下不限制目录，可访问本机任意路径）：

```ts
export const name = "code"
export const description = "涉及代码编写与源码分析时装载本子Agent（不处理歌白自身代码，自我优化用 self_optimize）：新建/修改项目与功能实现、代码分析、问题定位修复；装载后按 探索→方案→修改→验证 流程执行，改动较多时优先 patch。输入：需求/问题描述；输出：代码修改方案与验证结果。"
export const systemPrompt = "你是源码分析与修改专家（工作流参考 opencode 编码助手）。文件读写查询（read/write/edit/patch/ls/grep/glob/file/sh/py）与交互编排（ask/todo/subsession_run/fetch_url）为全局工具，直接用全局名调用；本子Agent 补充编码专属工具（search_symbols/analyze/git/preview_server/env_detect/system_info，以 code_ 前缀调用）。工作流程：\n0) 环境确认：开工前先读本提示词开头注入的项目环境注记（项目根/工作目录、预置项目清单、受限模式说明）——目标代码所在项目与 project 参数取值由此确定；……清单确无对应项目时用文件工具的 project 参数直接传项目根路径（自由项目），此后相对路径即相对该根解析；……（完整提示词见 code.ts）"
export const tools: ToolSet = { search_symbols, analyze, git, preview_server, env_detect, system_info } // 只声明全局集没有的独有工具——文件读写查询复用全局工具（带 project 参数）
export const preload = false
```

要点：

- **工具集定位（只声明独有工具）**：code 的工具集只含全局集没有的编码专属工具（search_symbols/analyze/git/preview_server/env_detect/system_info）；文件读写查询（read/write/edit/patch/ls/grep/glob/file/sh/py）与交互编排（fetch_url/todo/ask/subsession_run/bg_task）复用**全局工具**——装载到主会话与 subsession_run 子会话运行（全局工具默认继承）均直接用全局名调用，不再重复注册 `{agent}_` 同款（重复工具已彻底删除，见「装载工具会话可见性」）
- **工作流（参考 opencode 编码助手）**：环境确认（先读提示词开头注入的项目环境注记——预置项目清单/项目根/受限模式，确定目标项目与 project 参数，警惕与目标同名的 API 封装/适配层）→ 规划（todo 待办跟踪）→ 探索（grep/glob/search_symbols/ls/analyze/git ls-files 先定位、并行调用；grep 宽泛摸底用 files 形态、锁定后 content+context 看语境、exclude 排噪、literal 字面搜代码片段；大范围摸底可 subsession_run 委托 explore 只读探索，再精确读取——read 默认带行号、分段读附位置注记）→ 定位 → 方案（改动点清单 + `ask` 方向确认）→ 修改（遵循项目既有约定、与周围代码风格一致；改动多时 `patch` 补丁应用优先（可跨多文件，带 ---/+++ 头原子应用）、`edit` 定点替换（唯一性校验）次之、`write` 仅新建/整体覆盖（防盲写守卫统一覆盖 write/edit/patch；新建大文件分段——首段 write、后续 append:true 续写），不添加无关注释/密钥，成功后无需重读验证）→ 验证（测试 + typecheck/lint，失败续修）→ 收尾（`git` 工具只读核对变更后只提交预期文件，不擅自 commit；总结先结论后细节、引用 文件:行号、失败如实报告）
- **分析工具（全局 + 独有分层）**：`read`/`ls`/`grep`/`glob`/`fetch_url` 为全局工具（描述见功能列表；grep files/count 摸底 + content/context 精读、include/exclude 过滤、literal 字面匹配、head_limit 压低上限）；独有：`search_symbols`（tree-sitter 符号搜索双模式：**定义**定位——解析函数/类/方法/类型定义，返回 文件:行号: 类型 名称，精确匹配优先；`mode=references` **引用/调用点**——叶子节点精确匹配 + 排除定义名与注释/字符串（比 grep 文本匹配少误报），梳理调用链/影响面用）、`analyze`（tree-sitter 语法结构概览）、`git`（只读 Git 检查，见「git 版本控制工具」）
- **修改工具（全部为全局工具）**：`patch`（**unified diff 补丁应用**：一次多 hunk、可跨多文件（---/+++ 头分组）、行号模糊容错、原子落盘、dry_run 预演，改动多/行号易偏移时优先）/`edit`（定点替换，小范围改动）/`write`（新建/整体覆盖）三者同受防盲写守卫约束、默认无需审批（与总Agent 既有姿态一致；旧版由 code `requiresApproval` 收紧审批的双胞胎覆写已随工具删重移除）；`sh`/`py`（Python 项目执行测试与脚本）走工具自身动态审批（默认需用户审批，复用统一审批流；`approval:false` 按次免审，见「工具审批」；`sh` 的 `workdir` 参数或 `project` 参数指定命令工作目录）；`bg_task`（后台异步任务统一管理——命令任务/子Agent 运行/子会话运行，见「sh 异步后台任务」「子会话运行的异步运行」「子会话运行」）；`file` 文件管理（copy/rename/move/mkdir/delete/info 多动作，delete 动态需审批）
- **协作工具（全局）**：`ask`（需求澄清、方案取舍确认，阻塞等待用户回应）、`todo`（待办增删改查统一入口，entries 列表一次批量操作、空列表即查询，与会话待办联动）、`subsession_run`（派生子会话委托其他子Agent——如 `playwright` 做 Web 项目浏览器端验证；**不得委托 `self_optimize`**，见「职责边界」）
- **项目机制（`project` 参数，无状态逐次指定）**：操作项目必须**显式**指定——全局文件工具与 code 独有工具的 `project` 参数传**预置项目名**或**项目根路径**（自由项目），路径即相对所选根解析（沙箱模式限定该根内）；未传 `project` 时相对路径以会话工作目录为基准（默认语义不变）。无会话粘性默认根（旧版 `project` 工具的 use/clear 已移除——「每次调用重复传根路径」由预置项目名/相对路径本身消化，机制无状态更简洁）；`grep`/`glob`/`search_symbols` 在项目模式下递归扫描项目根（跳过 `.git`/`node_modules`/`dist` 等大型目录，限深 10 层）；**绑定项目根的子会话（`subsession_run` + `{AGENT}_PROJECT`）内文件清单基准随根切换**——`ctx.listFiles` 与路径解析基准（resolveBase）一致取项目根文件树，`search_symbols` 等以 `listFiles` 为扫描清单的工具默认扫项目根，无需显式 `project` 参数（此前清单恒为会话 tmp 子树，绑定根会话里只能搜到 tmp）；`git`/`sh`/`py`/`preview_server` 以项目根为工作目录
- **保留项目名 `tmp`（会话工作区）**：`project` 参数接受保留名 `tmp`，解析到**会话工作区**（引擎恒定注入 `ctx.sessionWorkdir`——`workdir` 在绑定项目根的会话里是项目根，`tmp` 不随之变化；未注入时回退 `workdir`）。访问会话文件（附件、中间产物、临时脚本）的显式通道——「世界切换」统一到 project 参数语义，不再依赖工具名分叉。**预置项目名不得占用 `tmp`**：启动校验拒绝（`index.ts` 扫描子Agent `envVars` 声明的 `{AGENT}_PROJECTS` 进程环境变量），前端注入的任务级 env 由 `presetProjectsFor` 同规则兜底抛错——防呆在配置期暴露，避免静默遮蔽保留名后无法访问会话文件
- **验证/环境工具（自全局工具集下沉，全局集最小化）**：`preview_server`（临时新端口启动/停止一份独立进程的 GEBAI 服务，`action=start`/`stop`，供用户验证服务端类改动——不中断当前会话，启动返回 URL/PID/日志路径；经 `projectAware` 包装以 `{workdir:true}` 暴露——带 `project` 参数时以项目根为启动目录）、`env_detect`（环境探测：平台/架构、PATH 去重、关键工具链版本 node/bun/python/git/cargo 等（缺失标记不可用）、Windows MSVC 与 WebView2 状态，指导安装缺失组件）、`system_info`（系统基础信息）——三者不注册为全局工具，以 `code_preview_server`/`code_env_detect`/`code_system_info` 命名空间暴露（`self_optimize` 经连带装载 code 一并获得），全部免审批
- **验证对象与前端热重建（`preview_server`）**：预览服务即**歌白自身**（不是任意项目的通用预览器；验证外部项目用项目自己的启动命令 + `sh` 后台任务）；`hot` 参数（缺省开，仅脚本模式）给预览进程注入 `GEBAI_DEV_RELOAD=1`，复用服务端既有开发热刷新（`DevReloadManager` → `vite build --watch`）——**改动 `packages/web` 源码后 dist 自动重建、页面自动刷新，不需要再停止/重启预览服务**（旧形态只服务既有静态产物，对旧 CSS 误判「改动没生效」即此因）；二进制形态无仓库源码，自动不开（`hotDefault = !binary`），`hot:false` 可显式关闭。启动信息与状态文件（`gebai-preview.json` 的 `hot` 字段）如实标注热重建是否开启。**副作用**：热重建写的是仓库共享 `dist`（每次构建先 `clean-dist` 清空再重建，构建窗口内页面产物引用可能短暂为空），因此开启热重建期间主实例与预览实例共用同一份产物；仅服务端改动用 `hot:false` 更稳
- **tree-sitter 语法分析（`analyze`）**：基于 `web-tree-sitter`（wasm）按语言加载语法（JS/TS/TSX/Python/Go/Rust/Java/C/C++/JSON/HTML/CSS/Bash 等 30+ 语言，语法文件来自 `tree-sitter-wasms`），输出结构化概览（导入/导出、函数/类/方法/类型定义及行号、嵌套关系），替代逐行阅读快速定位；首次使用懒加载、按语言缓存 parser；**wasm 加载双通道**——dev 模式 `require.resolve` 读 node_modules，二进制/打包模式回退构建期内嵌注册表（`analyzer-wasm.embedded.generated.json`，`packages/server/scripts/build-analyzer-wasm.ts` 生成，gzip 压缩约 3.6MB）；**报错区分两类**：语言不在映射表报「不支持的语言」（附支持列表），wasm 资源加载失败报「语法分析不可用」（提示依赖缺失/未内嵌并引导改用 read）——不再把资源缺失误报为语言不支持
- **两种项目形态（统一 project 参数，二选一）**：
  - **预置项目**：会话环境变量 `CODE_PROJECTS`（JSON 数组 `[{name, path, description?}]`）声明命名项目注册表——**工具以 `project` 参数（项目名）选择目标项目**，路径参数相对所选项目根解析；预置项目**可携带项目说明**（`description`），清单（名称/说明/路径）注入**子Agent 系统提示词**（engine 组装子Agent 提示词时注入——`subsession_run` 派生子会话与装载写会话记录（`ensureSessionAgents`）均注入，**不注入总Agent 系统提示词**；注记置于职责分隔头之后、静态提示词之前（环境信息前置，模型开工先读清单按名选项目））；**描述动态体现**：engine 的 `agentDescription` 把预置项目摘要（名称: 说明（路径））与装载后工具摘要追加进 code 描述——未装载清单（总Agent 系统提示词）与 `agent_list` 输出均展示，总Agent 装载前即可按项目名/工具能力关联任务与代码位置，完整注记（工具 project 参数用法说明）仍只在子Agent 提示词；非法 JSON 静默忽略（回退自由项目模式），同名项目去重（首个生效）
  - **自由路径项目**：`project` 参数直接传**项目根路径**（绝对/相对；`looksLikePath` 判定——绝对路径、含路径分隔或 `.`/`~` 开头；本地模式绝对直用/相对按进程 cwd 解析，沙箱模式限定用户数据目录内与预置项目同规则）——无需预配置即获得与预置项目一致的「路径相对项目根解析 + 项目根工作目录 + 项目根递归搜索」三重路由
  - 实现：项目机制统一实现在 **`core/tools/projects.ts`**（`projectAware`/`resolveProjectRoot`——引擎级基础设施）；全局文件工具在 `createGlobalTools` 注册处统一经 `projectAware` 包装（code/explore 的独有工具同规则），传入 `project`（预置名/路径形态/保留名 `tmp`，`resolveProjectRoot` 统一解析）时把 `resolvePath`/`workdir`/`listFiles` 基准切换到该根（沙箱模式限定项目内，本地模式放开）
- **项目内置（特定项目绑定）**：会话环境变量 `CODE_PROJECT`（子Agent 大写前缀 `CODE_AGENT_*` 约定）声明默认项目根——子Agent 的工作目录与路径解析以项目根为基准，系统提示词注入项目路径；沙箱模式限定项目内，本地模式放开；未设置时按用户给定路径处理。**沙箱降级**：沙箱模式下越界/绝对路径的项目配置（`{AGENT}_PROJECT` 绑定与 `{AGENT}_PROJECTS` 预置项目）解析失败时**静默跳过/回退工作目录**（不使任务失败，与非法 JSON 忽略一致），避免本机环境变量注入的越界路径拖垮服务端部署。通用机制：任意子Agent 均可通过 `{AGENT_NAME_UPPER}_PROJECT`（项目根绑定）与 `{AGENT_NAME_UPPER}_PROJECTS`（预置项目注册表，engine 解析并注入 ctx/提示词，工具自行决定是否暴露 `project` 参数）使用。**默认项目根兜底（`SubAgentDef.projectRoot`）**：`{AGENT}_PROJECT` 未配置时 def 可声明兜底解析（返回绝对路径即视为绑定）——与环境变量绑定**同语义**（子Agent 提示词「项目根」注记、`subsession_run` 子会话以其为工作目录、项目 AGENTS.md 自动注入、总Agent 侧绑定声明均生效），沙箱模式同规则拒绝（默认根通常在用户目录外，回退工作目录）；如 `self_optimize` 在脚本调试（dev）模式下按模块路径自动推导歌白仓库根（二进制模式无兜底，须显式配置 `SELF_OPTIMIZE_PROJECT`）。**总Agent 侧注入项目绑定（不含预置项目清单）**：`buildSystemPrompt` 汇总所有已注册子Agent 的 `{AGENT}_PROJECT`（「xx 子Agent 项目绑定：<根路径>（subsession_run 子会话运行该子Agent 时以其为项目根；装载模式下路径基准为会话目录，访问项目用 project 参数或绝对路径）」——仅声明绑定，不宣称装载模式下工作目录已切换）注入总Agent 系统提示词；`{AGENT}_PROJECTS` 预置项目说明（含清单）**只注入子Agent 系统提示词**（`subsession_run` 派生子会话形态，见上），总Agent 侧仅经**描述摘要**（`agentDescription`：未装载清单/`agent_list` 中描述附「预置项目：名称: 说明（路径）」）轻量体现，不注入完整清单注记；主循环 `buildContext` 合并全部预置项目供 `project` 参数路由（装载模式直接用其工具时 project 参数照常可用；清单提示词装载模式同样注入——`ensureSessionAgents` 写会话记录提示词消息时动态拼接 presetNote，与 `subsession_run` 子会话一致）
- **受限模式（CODE_RESTRICT_PROJECTS=true）**：全局文件工具与 code 独有工具（read/write/edit/patch/ls/grep/glob/file/sh/py/search_symbols/analyze/git）**仅允许操作预配置项目**——必须携带 `project` 参数（项目名须在 `CODE_PROJECTS` 清单中），自由路径（`path`）被拒绝并提示改用 project 参数；处于 `{AGENT}_PROJECT` 绑定根内（子会话运行模式）时未传 project 放行。**默认关闭（不限制）**，可经会话/用户/进程环境变量开启；受限说明属 code 子Agent 行为约束，**只注入子Agent 系统提示词**（`subsession_run` 子会话运行时，code 静态提示词含开关含义、动态 restrictNote 注入当前开启状态），**不注入总Agent 系统提示词**——装载模式下模型走自由路径时由工具错误提示自愈引导（DESIGN「环境变量配置」）
- **环境变量一览注入提示词**：系统提示词内置「项目与环境变量配置」段落，逐条列出项目相关 env 的名称与作用（`CODE_PROJECTS` 预置项目注册表 / `CODE_PROJECT` 项目根绑定 / `CODE_RESTRICT_PROJECTS` 受限模式），模型无需猜测配置来源即可直接按名使用项目；预置项目实际清单（名称/说明/路径）仍由 engine 动态注入（见上），注记前置（职责分隔头之后、静态提示词之前）保证模型开工先读环境
- **项目约定自动注入（AGENTS.md）**：子Agent 项目根存在 `AGENTS.md`（兼容 `AGENT.md` 命名）时，engine 在 `runSubSession` 组装系统提示词时**自动读取并注入**（`\n\n项目约定（AGENTS.md，编码/维护必须遵守）:\n<内容>`，≤8KB 截断，不存在/不可读静默跳过）——code 绑定项目（`CODE_PROJECT`）与 self_optimize 绑定 GEBAI 仓库（`SELF_OPTIMIZE_PROJECT`，或 dev 模式 `projectRoot` 兜底自动推导的仓库根）均生效，模型无需先 `read` 即可遵守项目约定；预置项目（`project` 参数形态）运行期才确定目标，不自动注入（模型可用 `read` 按需读取）
- **工作区限制（按运行形态）**：
  - **桌面/本地浏览器模式**：**不限制目录**，可直接分析/修改本机任意路径的项目（用户本地工作目录、克隆的仓库等），无需先复制进 `tmp/`
  - **服务端部署**：受路径沙箱约束（见「多用户隔离与安全」），目标项目须位于会话 `tmp/` 或用户数据目录内（`CODE_PROJECT` 同样受限）
  - 任一形态下**不得修改 GEBAI 服务端自身源码**（提示词与权限双重约束，自我优化走 `self_optimize`）
- **验证闭环**：系统提示词要求修改后运行项目测试（按语言选 `bun test`/`npm test`/`pytest` 等，Python 项目用 `py`）与类型检查/lint（`bun run typecheck`/`bun run lint` 等），失败则继续修复；Web 项目需要浏览器端验证时经 `subsession_run` 委托 `playwright`
- **子会话运行模式**：总Agent 以 `subsession_run` 派生子会话（预加载 `code`）时，`code` 自主完成「规划 → 分析 → 方案 → 修改 → 验证」全流程后交回
- **预加载**：`preload` 声明该子Agent 是否默认注入——**内置子Agent 一律 `preload = false`**（按需 `agent_load` / 路由自愈），部署方可用 `GEBAI_PRELOAD_SUB_AGENTS` 指定预载集合
- 新增子Agent 只需照此模式放置定义（目录形态 `{name}/{name}.ts` + `{name}.md`，或纯 `.md` 零 TS 定义），构建期按 `GEBAI_BUILD_SUBAGENTS` 选择性打包（见「选择性打包」）

> **职责边界**：`code` 只处理外部项目。**自我优化（改 GEBAI 自身代码）由独立的 `self_optimize` 子Agent 承担**（见下节），两者工作区、审批策略、提示词均分离，避免普通源码分析流程获得修改服务端代码的权限。`code` 的 `subsession_run` 委托仅用于只读/验证类委托（如 `playwright`），不委托 `self_optimize`（防经子Agent 链间接获得服务端修改能力，边界由总Agent 路由引导「GEBAI 自身代码 → self_optimize」维护）。

### 更多内置子Agent

按同一模式补充以下实用子Agent，构建时选择性打包：



#### `vision`（视觉能力，图片分析型）

实现于 `packages/agents/src/agents/vision/`（@gebai/agents 包；`vision.ts` 定义入口 + `vision.md` 系统提示词 + `vision.test.ts`），视觉能力的**通用层收拢**（图片输入型）与其他子代理的复用入口——全局 `vision` 工具已移除，本子代理是视觉语义分析的**唯一入口**（主会话 agent_load 装载/路由自愈，子Agent 依赖声明连带装载）（分层与两种复用方式的边界判定见「小模型识别」末条「视觉能力分层与子代理复用边界」）。**跨语言合并形态**（边车协议 v2 请求级 ctx 的首个内置样例）：

- **客卿侧（`keqing/python/vision/`，onnxruntime 原生推理）贡献**：`ocr`/`locate`/`locate_image`/`detect` 四个本地识别工具——重计算迁边车（原生推理比 wasm 快、模型进程级加载一次全会话复用），manifest `requiresApproval:false`（只读免审批）；算法与 TS `core/cv` 纯函数同源移植（det/rec 前后处理、DB 后处理、CTC 解码、零均值 NCC 模板匹配、letterbox+v8/v5 双形态后处理、ultralytics ONNX 元数据自适应），模型资产复用同一套（GEBAI_CV_MODELS_DIR / assets/cv-models / {GEBAI_HOME}/models/detect）；图片路径经请求级 `ctx.cwd` 解析；依赖（onnxruntime/numpy/pillow）缺失时工具返回安装提示不拋栈，探测延迟到调用时（模块加载阶段缺依赖不阻断工具集上报）
- **TS 侧贡献**：仅 `analyze`（多模态语义分析）——provider 抽象属宿主 LLM 层（三协议流式适配、GEBAI_VISION_* 与任务级 env 覆盖、多模态回落链经 `getVisionProvider`），凭证不下传边车；description 留空不贡献，识别工具描述与决策序提示词由客卿侧 PROMPT.md 提供，合并层拼接两侧：

- **工具集**（跨语言合并后共 8 个：客卿侧贡献 `ocr`/`locate`/`locate_image`/`detect` 四项，与语言基础工具 `run`/`pip`/`status` 合并后随 `analyze` 一并装载；全部只读免审批；**受客卿门控**——客卿仅本地形态启用、沙箱启用即整体禁用，故服务端部署下只有 TS 侧 `analyze` 可用）：`analyze`（多模态模型语义分析——理解图像内容/布局含义/非文字元素，支持 png/jpg/jpeg/gif/webp；复用 `makeVisionTool` 工厂注入 `getVisionProvider`（同一 provider 解析：GEBAI_VISION_* 外挂 → 多模态主模型回落））、`ocr`（本地 OCR 读文字带图片像素坐标）、`locate`（定位文字坐标）、`locate_image`（模板匹配定位图标/图形）、`detect`（自备 YOLO 目标检测，默认配对 OCR 文本）——后四者复用 `createCvAnalysisTools`/`createDetectTool` 共享工厂，注入「无缺省源（image 必填）+ 无闸门」形态
- **边界（不截图）**：输入一律为图片文件路径（PNG；analyze 另支持 jpg/jpeg/gif/webp）——本子Agent 不提供缺省图像源，需要现截屏幕/页面由 desktop/playwright 子代理完成；坐标为图片像素系，消费方自行映射到目标环境（屏幕加窗口原点、页面用 elementFromPoint）
- **决策序（硬性，按性能从高到低）**：①读文字/找文字/判断可 OCR 状态 → `ocr`/`locate`（本地 OCR，毫秒~秒级、精确坐标、离线不耗配额）——禁止用 analyze 回答 ocr 能答的问题；②找图标/图形/UI 元素 → `locate_image`（本地模板匹配）；③找对象/框目标 → `detect`（本地 YOLO）；④`analyze`（外部多模态模型，秒级延迟、耗配额、可能超时）仅语义理解（图像内容/布局含义/非文字元素）或 1~3 无结果时兜底——超时不原样重试，先回到 1~3
- **依赖复用（方式一消费方）**：`self_optimize` 声明 `dependencies: ["code", "vision"]`——subsession_run 隔离子会话即使不继承全局工具也有视觉能力（page_capture 截图后 `vision_analyze` 分析、`vision_ocr` 读图文字）；desktop/playwright 的本地识别推理也消费 vision 边车（见下条「推理委托」）；语义分析兜底按需 agent_load 装载；全局 `vision` 工具已移除（降级指引文案统一改指 vision_analyze）
- **推理委托（desktop/playwright 复用边车推理，sidecar-first）**：desktop_ocr/desktop_locate/desktop_detect、playwright_ocr/playwright_locate/playwright_detect 与 desktop_wait_for 的 text 模式，推理优先经 `ToolContext.registry` 调用 `vision_ocr`/`vision_detect`（即 vision Python 边车——onnxruntime 原生推理、模型进程级缓存），未装载/报错回落进程内 wasm（`@gebai/agents` core/shared/cv-analysis 内 ocrInfer/detectInfer 统一封装）。跨进程图像传递：RgbaImage 经 `encodePng`（`@gebai/agents` core/cv/image，与 decodePng 对称）落会话工作区**每次调用唯一**的临时文件（`tmp/cv_sidecar_in_<序号>_<随机>.png`，用后即删）后以相对路径调用，边车经请求级 ctx.cwd 解析——并发 CV 调用（js 编排 Promise.all 并行 ocr/locate/detect）不共用路径，不会互相覆盖或读到瞬时半写文件；坐标系完全同构（边车返回坐标相对传入 PNG=裁剪后图像像素系，与 wasm 一致，消费层 offX/offY 回加不变）——纯 drop-in。经注册表而非直连边车进程：复用装载门控/审批策略/ctx 组装，不绕过任何治理层；边车未装载时零开销回落（registry.resolve 不到即 wasm）
- **缺省源截图**：desktop/playwright 的缺省图像源（现截屏幕/当前页视口）同样用每次调用唯一的临时文件名（`cv_capture.png` / `pw_cv_capture_<序号>_<随机>.png`）并读后即删——同一会话内并发取图不互相覆盖
- **大图与小字（OCR 分块）**：整图长边超过 `OCR_TILE_SIDE` 时按块检测（相邻块重叠固定像素、重叠区同行按框 IoU 去重取高分），每块以原生分辨率送模型——整图等比缩到长边上限会把小字号压到不可辨识；检测输入尺寸对耗时影响很小（FHD 960→1920 仅 +20% 耗时，小字高分命中 2/7→7/7），故默认原生尺度送检
- **平坦窗口（模板匹配）**：粗扫与精化两阶段的 NCC 分母均设下限（窗口方差≈0 时 NCC 无定义，记 0 分）——否则浮点残差会被放大成伪高分，挤占精化候选名额而使真匹配拿不到
- **降采样语义一致**：两侧（TS `core/cv/template.ts` 与 客卿 `vision/tools.py`）均为 box 块均值降采样（边缘块按实际像素数平均），同一模板在两侧得到一致分数
- **定位失败回落（`locate`）**：目标文字精确匹配落空时，按相似度给出最相近的若干识别行（含文字、坐标与相似度）——OCR 对小字/艺术字的误读会让精确匹配落空，直接报「未找到」把可继续的失败变成死路（模型只能撞词重试）；确无相近行时才给出换通道建议，两条失败路径都指向可行动的下一步
- **检测类别缺标签（`detect`）**：模型无内嵌 `names` 元数据时类别以 `class_N` 占位并显式提示（可用 `GEBAI_CV_DETECT_LABELS` 指向每行一个类名的文本文件补齐），占位类别名即 `class_N`（`data.objects` 里直接可辨）——占位类别无法据此判断检测到的是什么，静默输出会让模型误读

#### `desktop`（桌面控制）

```ts
export const name = "desktop"
export const description = "涉及宿主机桌面操作时装载本子Agent（仅本地模式，服务端部署不可用）：截图（全屏=虚拟屏幕覆盖多显示器）、本地 OCR 识别与文字/模板定位（小模型离线推理）、UIA 语义树枚举（uia_inspect，控件角色/状态/值，Chromium/原生应用可选语义通道）、窗口控制（激活/移动/最小化/最大化/关闭/置顶，HWND 精确定位多窗口）、键盘鼠标输入（输入/点击/滚动/拖拽，修饰键组合/按住分离/中键三击）与剪贴板读写（文本/图片）、界面条件等待（文字/模板/画面变化）；窗口优先 PID/HWND 定位，输入/点击类操作需审批。输入：操作目标；输出：操作结果、屏幕文字坐标与屏幕图片。"
export const systemPrompt = "你是桌面控制助手，直接操作宿主机桌面（仅本地模式；服务端部署下所有工具拒绝执行）。工作流程：1) 明确目标：先 window_list 确认目标窗口（PID/进程名/前台标记*/窗口位置/标题；窗口优先用 PID 定位——标题可能重复，前台标记可确认当前焦点窗口），截图前先说明截图用途；2) 准备：截图/坐标操作前先 screen_info 确认显示器分辨率与主屏原点（主屏左上角为坐标原点，副屏坐标可为负；全屏截图=虚拟屏幕覆盖所有显示器，OCR/locate 返回的坐标已映射回主屏原点像素系，可直接用于点击），避免 region/坐标错位；3) 定位：点击屏幕元素优先取精确坐标再 mouse_click——文字元素用 desktop_locate，图标/图形元素用 desktop_locate_image（模板匹配，template 为既有截图裁剪的模板 PNG 或 template_region 在当前截图内取区域；同尺寸匹配，模板需与目标显示尺寸一致）；读取屏幕文字用 desktop_ocr（本地小模型，快且带精确坐标，离线不耗配额），仅语义理解/非文字内容才装载 vision 子代理用 vision_analyze 分析截图；desktop_detect 检测 UI 组件/图标（自备 ultralytics YOLO ONNX 即插即用——输入尺寸与类别自动读模型元数据，GPU 可用时自动走 sidecar 加速），输出组件框并默认配对 OCR 文本，适合结构感知与无文字元素定位，找特定文字按钮仍以 desktop_locate 为主；4) 执行：输入/点击/窗口控制类操作（window_focus/window_move/window_state/type_text/key_press/mouse_move/mouse_click/mouse_scroll/mouse_drag/clipboard_write）需审批，操作前先说明操作意图与目标窗口；滚动页面/列表用 mouse_scroll，拖放/滑块用 mouse_drag，窗口最小化/最大化/关闭用 window_state（close 走优雅关闭）；执行前必须先用 window_focus 激活目标窗口，仅当输出「已激活」才继续输入/点击——输出「激活失败」时不得继续（会把内容输入到错误窗口），请用户手动点击目标窗口后重试；type_text 剪贴板模式输出「输入失败」时不要原样重试（改 mode=\"keys\" 输 ASCII 或提示用户检查剪贴板管理软件）；只读类（screenshot/window_list/screen_info/clipboard_read/desktop_ocr/desktop_locate/desktop_locate_image/desktop_detect/desktop_wait_for）免审批可直接执行；5) 反馈与等待：执行后反馈结果——截图返回图片，mouse_click 坐标基于截图实际尺寸判断（desktop_locate/desktop_locate_image 返回的坐标已相对屏幕原点，可直接使用），说明截图位置与关键结论；操作后等待界面就绪用 desktop_wait_for（等文字出现/消失或画面变化，默认 20s 超时），不要自己反复截图轮询；6) 约束：只执行用户明确要求的操作，不做额外破坏性动作（不改系统设置、不删除文件、不触发危险快捷键）。验证多通道：不依赖单一验证通道——截图黑屏/纯色（工具会主动提示）时立即切换通道，不要反复重试截图：用 window_list 确认窗口是否在前台、用 desktop_ocr/clipboard_read 验证界面文本与剪贴板状态，或经 subsession_run 委托 code 子Agent 读取应用数据文件断言结果；任何通道失效即降级并明确告知用户当前采用的验证方式。"
export const tools: ToolSet = { screenshot, window_list, window_focus, window_move, window_state, type_text, key_press, mouse_move, mouse_click, mouse_scroll, mouse_drag, clipboard_read, clipboard_write, screen_info, ocr, locate, locate_image, detect, wait_for, uia_inspect }
export const requiresApproval = { window_focus: true, window_move: true, window_state: true, type_text: true, key_press: true, mouse_move: true, mouse_click: true, mouse_scroll: true, mouse_drag: true, clipboard_write: true }
export const preload = false
```

- 实现于 `packages/agents/src/agents/desktop/` 目录（@gebai/agents 包）（`desktop.ts` 定义 + `desktop_tools.ts` 工具集 + `desktop_cv_tools.ts` 本地识别工具集 + `desktop_uia_tools.ts` UIA 语义工具 + 各自 `*.test.ts` 测试），工具不注册为全局工具，仅经子Agent 命名空间暴露（只声明桌面操控独有工具；验证多通道经全局 `subsession_run` 委托 code 子Agent 读取应用数据文件断言结果——编排用全局名，def 不复刻全局工具）
- **提示词纪律（js 编排优先）**：多步操作（窗口定位→聚焦→识别→点击/输入→验证，含分支/重试）默认经全局 `js` 编排为一段脚本执行——脚本内各 desktop 工具像函数一样 await（按注册名），按中间结果分支与重试，不逐步往返；js 保持默认审批（一次审批覆盖脚本内全部工具调用，含输入/点击类——免审运行时内部需审批工具被拒）；先 window_focus 复核「已激活」再输入/点击；OCR/locate 限定目标窗口 region 小区域识别（屏幕坐标=窗口原点+区域内坐标）；等待一律 desktop_wait_for，不反复截图轮询；vision 仅语义理解兜底，定位一律本地 OCR/locate；**vision 硬性兑底序**：读文字/找元素/判断界面状态（按钮文字是否变化、处于哪一页、是否弹提示）一律 desktop_ocr（find 过滤）/desktop_locate 先行，禁止用 vision 回答 OCR 能答的问题；仅当 OCR/locate 无结果或需理解非文字内容（图像内容/布局含义/报错图标）才 vision；**uia_inspect 决策序**：需要控件语义/状态（禁用/勾选/值/焦点控件——OCR 判不了）或控件树交叉校验时用 desktop_uia_inspect（仅 Windows；空树/浅树=Flutter/游戏/自绘立即回落像素通道），定位坐标仍以 desktop_locate/locate_image 为主通道；**等待纪律**：等非文字元素（加载动画/图标）用 wait_for 的 image/image_gone 模板模式，非 change（画面变化误报率高）
- **权限拦截降级（UIPI）**：模拟输入对管理员权限（elevated）目标会被 Windows UIPI 静默拦截——症状为「点击/输入报告成功但界面毫无变化」（window_focus 已确认前台）。提示词约定：连续 2 次无效即停止重试，立即降级换通道（目标应用自带的本地 API/CLI（游戏客户端多带本地控制接口，经全局 sh/js 调用）、键盘/剪贴板间接操作，或建议用户以普通权限重启目标/提权运行歌白）；佐证判据：Get-Process/Win32_Process 查询目标进程路径/命令行为空 = 完整性级别高于当前会话（whoami /groups 查自身，Medium 即不可读写 elevated 进程信息），模拟输入与进程信息查询均注定失败
- 跨平台：Windows 走内置 PowerShell（截图/窗口/输入，无外部依赖）；macOS 走 `screencapture` + `osascript`（鼠标需额外 `cliclick`）；Linux 依赖 `xdotool`/`wmctrl`/`scrot`（缺失时明确报错）。**Windows 坐标一致性**：全部涉及像素坐标的 PowerShell 脚本（截图/screen_info/window_list/window_move/mouse_*）开头声明 `SetProcessDPIAware`——坐标统一物理像素，防高 DPI 缩放（150% 等）下截图/窗口/鼠标坐标落入逻辑像素空间错位；**多显示器**：全屏截图与 cv 现截均取**虚拟屏幕**（`SystemInformation.VirtualScreen`，覆盖所有显示器，副屏可为负坐标），`region` 参数 x/y 支持负值（副屏在主屏左侧/上方），OCR/locate/locate_image 返回坐标一律映射回主屏原点像素系（虚拟屏幕原点/区域原点偏移），可直接用于 mouse_click；`window_list` 输出含**前台标记**（`GetForegroundWindow` 比对，`*` 行 + 「当前前台」汇总行）与**窗口 bounds**（`GetWindowRect` 物理像素 `x,y,w,h`）
- **Windows PowerShell 执行通道（临时 .ps1 文件 + -File）**：所有 PowerShell 脚本（desktop_tools/desktop_cv_tools/desktop_uia_tools 三文件共用 `psCmd()` 助手）不再用 `-EncodedCommand` base64 内联——旧通道把整个脚本内联进命令行，受 cmd 单条命令行 8191 字符上限约束（截图/UIA 类长脚本必超——实测 ≥4KB 脚本即「command line is too long」静默 exit 1 且无任何输出），且企业管控软件会静默拦截长内联命令；改为脚本写入会话 `tmp/ps-scripts/` 下临时 .ps1，**UTF-8 带 BOM**（PS 5.1 对无 BOM 文件按 ANSI 解码，中文必乱码），经 `powershell -NoProfile -ExecutionPolicy Bypass -NonInteractive -File "<绝对路径>"` 执行（路径双引号包裹——Windows 参数解析不认单引号，单引号混入路径致 -File 打开失败、PS 回退交互模式污染 stdout；内嵌双引号反引号转义），执行后延迟 60s 删除临时文件（执行中的 .ps1 被 Windows 锁定；unref 不阻塞进程退出；删除失败留在会话目录无害）；`ctx.writeFile` 不可用/失败时兑底回 EncodedCommand 内联（短脚本行为同旧通道）；工具 run() 失败输出 stderr+stdout 合并展示（原来只显示其一，排障信息丢失）
- 截图返回 `image` 内容块实时展示，并**自动做黑帧/纯色检测**（平均亮度极低提示显示器休眠/锁屏，暗色单色提示非真实画面）与**尺寸/原点元数据**（`STAT` 行 / sips / ImageMagick——原点供图片坐标 → 屏幕坐标换算）；`screen_info` 列出全部显示器（分辨率/位置/主屏）供 region 与坐标参考；文本输入默认剪贴板粘贴法（中文/符号可靠，**写入回验重试**——剪贴板管理软件拦截写入时明确报错不粘贴；粘贴后**延时恢复**原剪贴板——目标应用异步消费剪贴板，立即恢复会粘出旧内容），`mode="keys"` 纯按键模式（绕剪贴板，仅 ASCII；中文 IME 激活时部分标点可能丢失）；`window_focus` **激活后复核前台窗口**（`GetForegroundWindow` 比对），前台锁定拦截时经模拟 Alt 击键缓解重试，仍失败明确报错（不虚报成功）；`window_state`（最小化/最大化/还原/关闭，close 经 `WM_CLOSE` 优雅关闭可弹保存确认）、`mouse_scroll`（滚轮四方向，每格 120 单位）、`mouse_drag`（左键按住 + 12 步插值移动 + 抬起，适配依赖真实鼠标轨迹的目标）与 `clipboard_write`（写入回验 + 敏感值**告警不中止**——复制密钥供本人粘贴是常见需求）为 Windows 实现，macOS/Linux 部分明确输出「暂未支持」；`type_text`/`clipboard_read` 输入或读取前自动做**敏感值扫描**（`sk-` 前缀、KEY/SECRET/TOKEN 赋值、Bearer 令牌，长令牌启发式 40+ 字母数字混合且非纯十六进制——纯英文长串/git commit sha 不误判），命中即中止（type_text）/告警（clipboard_read）；**服务端部署（沙箱启用）下所有工具拒绝执行**；**Windows 输入增强（2024 批次）**：`key_press` 支持 `action: press/down/up`（按住/抬起分离——按住 Shift 点选、按住 W 前进）与 `hold_ms`（press 按住时长），系统级虚拟键经 vk 名直发 keybd_event 扫描码（`win+r`/`volume_up`/`media_next`/`vk_left`/`vk_XX` 任意十六进制码——SendKeys 覆盖不了的媒体/Win/箭头键；`win` 在组合中是修饰键，`vk_shift` 纯修饰键组合仅 down/up 有意义，裸 `shift` 仍走 SendKeys `+` 语义）；`mouse_click` 支持 `modifiers`（ctrl/shift/alt 以 + 组合，按住期间点击后逆序抬起）与 `button: middle/triple`（中键/三击段落选择，双击/三击 50ms 间隔时序）；`mouse_drag` 同样支持 `modifiers`（Ctrl+拖拽复制文件）；**窗口控制增强**：`window_list` 经 EnumWindows 枚举全部顶层可见窗口（替代 Get-Process MainWindowHandle——后者每进程只见一个主窗口，浏览器多窗口/多开应用漏窗口），输出末列 HWND；`window_focus/move/state` 新增 `hwnd` 参数精确指向多窗口进程中的具体窗口（优先于 pid/title，winLocate 助手统一注入——注意 PS 条件须用 `$true` 字面量，裸 `true` 在 PS5.1 被当命令调用恒走 else）；`window_state` 新增 `topmost/notopmost`（SetWindowPos HWND_TOPMOST/-2 + SWP_NOMOVE|NOSIZE）；**剪贴板图片**：`clipboard_write` 的 `image` 参数（PNG 路径 → 系统剪贴板位图，FromFile+Dispose——FromStream+`::new` 内联在 PS5.1 有类型解析陷阱；写入后回验尺寸一致，截图交给用户 Ctrl+V 粘贴的高频路径），与 `text` 二选一
- **本地识别五工具（只读免审批，`desktop_cv_tools.ts`，基建见「小模型识别」；其中 ocr/locate/locate_image/detect 四工具均经共享工厂 `@gebai/agents` core/shared/cv-analysis（playwright 与 vision 子代理复用同工厂，见「视觉能力分层与子代理复用边界」），wait_for 为 desktop 独有实现）**：`desktop_ocr`（读屏幕/图片文字：image 省略现截全屏、region 限定区域（负坐标覆盖副屏）、find 关键词过滤；返回行文本+框+中心坐标+置信度，超 200 行截断提示收窄；data 携带结构化 lines）、`desktop_locate`（按目标文字定位：匹配分级 完全相等 > 行包含目标 > 目标包含行，返回最佳匹配中心坐标（直接可 `mouse_click`）+ 候选清单；未找到时给出多通道建议）、`desktop_locate_image`（**模板匹配定位图标/图形**：`template` 模板 PNG 路径或 `template_region` 在搜索图坐标系内取模板区域；NCC 同尺寸匹配（模板需与目标同一显示环境同尺寸），threshold 默认 0.8；返回最佳匹配中心坐标 + 候选；未找到建议降阈值/改 desktop_locate/vision）、`desktop_detect`（自备 YOLO 检测：`GEBAI_CV_DETECT_MODEL` 配置**或放入 `{GEBAI_HOME}/models/detect/` 唯一 .onnx 自动发现**（`GEBAI_CV_DETECT_LABELS` 可选——ultralytics 导出 ONNX 自动读取 names/imgsz 元数据）；**默认与 OCR 配对输出「组件类型+文本」**（`pair_text` 可关——检测只给类别不含语义，找特定文字按钮仍以 desktop_locate 为主）；`conf`/`iou`（NMS 阈值，密集小控件建议 0.1）可调；**推理分层**——GPU sidecar（GEBAI_CV_DETECT_BACKEND/EP，Windows DirectML 等任意 DX12 显卡）可用即用，输出如实标注实际后端，不可用/失败自动回落 wasm；未配置返回指引）、`desktop_wait_for`（**界面条件等待**：mode=text/text_gone 等待文字出现/消失（本地 OCR 轮询）、image/image_gone 等待模板图像出现/消失（NCC 模板匹配，`template` PNG 路径 + `threshold` 默认 0.8——加载动画/图标等非文字元素等待）、change 等待画面变化（灰度采样均差 > 2/255 对比基线）；timeout_s 默认 20 上限 120、interval_s 默认 2；超时不视为错误，返回最后观察状态）；坐标一律**映射回主屏原点像素系**（虚拟屏幕原点/区域偏移 + 缩放还原——识别在降采样图像上进行，返回坐标对准原屏幕）；现截复用固定文件 `cv_capture.png`（覆盖写，不随调用累积）；输入图仅支持 PNG（desktop/playwright 截图生态一致，其他格式明确报错）；模型/运行时缺失时错误输出为中文配置指引（不中断会话）
- **UIA 语义树（`desktop_uia_inspect`，`desktop_uia_tools.ts`，仅 Windows、只读免审批）**：枚举目标窗口的 UI Automation 控件树——控件角色/名称/AutomationId/类名/bounds/状态（disabled/focused）/值模式文本（输入框内容），中心坐标可直接 mouse_click；定位窗口按 `hwnd`（window_list 结果最后一列，同进程多窗口精确指向）/pid/title（可组合）——只给 pid 时同进程多顶层窗口（explorer 的桌面与任务栏、浏览器多窗口）会命中多个，取面积最大者为主窗口并在输出中报告候选数（按枚举顺序取首个会命中任务栏等辅助窗口）；`find` 关键词只输出名称/ID 匹配控件（含路径）、`depth`（默认 6 上限 12）/`max`（默认 200 上限 400）控制规模，超限标记 truncated。**Chromium/Electron/WebView2 无障碍树惰性构建已内置双查询**（首查触发构建 → 400ms → 正式收集——实测 Edge 首查 50 节点、复查 598），**未显式指定 depth 时浅树（<50 节点）自动加深到 12 重查一次**（这类应用的容器节点与真实语义常跨多层：VSCode depth=6 仅 12 节点、depth=12 有 75 节点真实语义）；BoundingRectangle 可为 Infinity/NaN（屏幕外/未布局元素）已归零防 Int32 转换异常；**空树/浅树（<5 节点）= Flutter/游戏/自绘框架不暴露语义**——输出明确提示回落像素通道（desktop_ocr/locate/locate_image），提示词同源纪律：控件状态（禁用/勾选/值）OCR 判不了才用 uia_inspect，与像素通道互补而非替代；服务端部署拒绝（同域闸门）

#### `feishu_docs`（飞书云文档）

实现于 `packages/agents/src/agents/feishu_docs/`（`styles/` 排版 SKILL + `styles/index.ts` 聚合 + `docx_xml.ts` XML 转换 + `feishu_api.ts` 工具集 + `feishu_docs.md` 系统提示词 + `feishu_docs.ts` 定义入口），全面对接飞书开放平台云文档能力（docx v1 / drive v1 / sheets v2·v3 / bitable v1 / wiki v2）：

- **文档 docx**：创建/元信息/纯文本/块结构读取、**大纲模式（`get_doc_blocks outline=true`：标题层级/文本/block_id/每节顶层块数，大文档先看目录）**、**按文本反查块 id**（`find_blocks`：子串匹配忽略大小写，支持块类型过滤，返回 block_id/类型标注 `type_name`/文本/所在路径）、**小节读取**（`get_doc_text` 传 `block_id` 只读该子树文本，含标题层级与表格行，长文档按小节读取）、**自动翻页**（`get_doc_blocks`/`list_blocks` 支持 `page_all=true`，上限 2000 块，**达到上限时明确提示改用小节读取/翻页续读**；块列表输出附 `type_name` 类型标注——**实测修正：图片块=27，原 43 误标 image（43 为 mindnote 思维笔记），quote=15**）、**图形块读取 `get_board`**（块类型 43 = mindnote 思维导图/画板，`get_doc_blocks` 只返回 `board.token` 占位；`get_board` 收 `board_token` 或 `document_id`+`block_id`（自动从 mindnote 块提取画板 token），调 `/open-apis/board/v1/whiteboards/{token}/nodes`（board v1）分页读取并**结构化提取后再输出**——**优先返回 PlantUML 源码（`syntax.code`）**（UML 图语义完整），否则**重建「形状文本 + 连接线关系」为流程描述**（`<A> ->(label) <B>`），避免原始大 JSON 爆 token/截断；权限 scope `board:whiteboard`）、添加块（children 通道自动分批 ≤50；**含 todo/表格/嵌套结构（children 引用/自带 block_id）时自动改走「创建嵌套块」descendant 通道**——一次请求创建完整表格/嵌套结构，**表格支持 `table.rows` 二维数组简化写法**自动展开为 table+table_cell+text（避免逐格填充 N 次调用与限频 429），**列宽按内容自适应**（汉字计双宽按权重分配文档正文宽度 730px——实测官方 convert 通道各列数表格总宽恒为 730、单列下限 100px，而接口 `column_width` 缺省为每列 100px、会把宽内容挤成长条；可 `table.column_width` 显式指定每列 px、`table.total_width` 改自适应目标宽），Markdown 表格默认置 `header_row` 标题行、单元格内 `\|` 按字面竖线处理（不错切列）且 `<br>` 转单元格内软换行、**反引号内 `|` 不切列**（官方 convert 会错切）、**连续两个 `<br>` 拆为多段落（cell 可多子块）**，嵌套子块递归补全局唯一 block_id（忽略调用方自带 id 防引用冲突）；**块字段自动映射**：按 `block_type` 把统一传的 `text` 字段改写为对应驼峰字段 `heading1~9`/`bullet`/`ordered`/`quote`/`todo`/`code`（16 equation 不可创建，不做映射），code 块 `language` 语言名按飞书官方枚举表转数字（如 ts=63、js=30、py=49，未知回退 PlainText）、默认 `wrap=true`，分割线块自动补 `divider:{}` 字段（缺字段 invalid param），**缺 `block_type` 默认 text、字符串字段/顶层 elements 自动包装**（修复简化写法 99992402），**非文本容器类型（table 等）不再被强制转成 text 字段**（修复 invalid param），**描述附全部可创建块类型提示表**（文本类 2 text/3~11 heading1~9/12 bullet/13 ordered/14 code/15 quote/17 todo/22 divider（**16 equation 不可经 API 创建**——官方创建接口枚举不含 16，实测 99992402，提示改用普通文本块/手动插入）；容器类 31 table（`table.rows` 简化写法）、24 grid（`column_size` 2~5 必填且与 grid_column 子块数一致，校验不一致报 1770041）+25 grid_column（**每列必须带整数 `width_ratio` 且至少一个子块**——实测缺 `width_ratio` 或空列报 1770041 open schema mismatch、`width_ratio` 传小数报 9499（只接受整数）；`prepareGridColumns` 自动补默认权重（小数按比例换算为整数）与空子块，列内内容随分栏一次创建）、19 callout（**正文在 children 子块**（`callout.elements` 被服务端忽略）、**颜色/emoji 为 callout 顶层字段**——background_color/border_color/text_color 数字枚举、emoji_id 表情名（`callout.style` 包裹会被忽略并回落默认配色）、**必须至少一个子块**（官方约束：GridColumn/TableCell/Callout 至少含一个子块，缺则 1770041 open schema mismatch；text 快捷写法自动生成子块））；引用型 35 embed（url）、37 file（token）、39 sheet（token）、43 mindnote（token）、44 bitable（token）、46 diagram（diagram_type）；限制提示：27 image 走 `insert_image` 三步流程、32 table_cell 不可单独创建、1 page 不可创建）——模型按提示可直接构造各类型块 JSON，**块类型提示表内置于 `add_blocks` 工具描述**（描述是调用时第一信息来源；系统提示词只留「见 add_blocks 工具描述」指针——**单源不重复**，块表两份拷贝曾多占约 1.6k 字符上下文））、更新块（PATCH 文本元素/插入/表格属性 `table_property`→官方 `update_table_property`；**`insert_text` 失败（飞书参数校验严格报 invalid param）时自动降级**：读块原文 → 在 index 处拼接 → 整体替换，功能兜底可用）、**`set_table_width` 重设已有表格列宽**（缺省按单元格内容自适应重算，或 `columns` 显式指定每列 px；官方接口一次仅改一列，工具逐列串行提交并留间隔避 429；兼可设 `header_row`/`header_column`）、**插入图片 `insert_image`**（官方三步流程：创建空 image 块（block_type=27 `image:{}`，创建时传 token 报 1770001）→ `medias/upload_all` multipart 上传素材（`parent_type=docx_image` 关联 image 块；**云空间 file_token 不能直接用于文档 image 块**）→ PATCH `replace_image` 设置素材 token，width/height 自动识别；图源支持本地路径与 http(s) 地址，与 Markdown 导入共用同一上传实现）、批量删除（**半开区间语义 `[start, end)`**，end_index 缺省/相同自动按单块删除）、**Markdown 导入**（`import_markdown`：本地转换引擎——**多级标题（#~#########，1~9 级）/段落/有序无序列表（缩进嵌套：每 2 空格或 1 tab 一级，跳级缩进归一为 +1 级、最深 9 级，子项作为父块 children 经 descendant 通道一次创建；**有序列表逐项下发 `ordered.style.sequence`**——按 CommonMark 语义段首项决定起始编号、其后自增，全写 `1.` 时为 1/2/3…）/任务列表/代码块/引用/表格/分割线/**图片（独立成行的 `![说明](本地路径或URL)` → image 占位块 + 素材上传回填：经 descendant 的 `block_id_relations` 把临时 id 映射为真实块 id 后上传素材并 PATCH `replace_image`，单张 ≤20MB，单张失败只提示不中断整篇导入）**/**图表围栏（```mermaid / ```plantuml / ```d2 / ```echarts）→ 调 `ctx.renderDiagram`（engine 注入，与 `show` 的 render=backend 同一渲染器：PlantUML TeaVM / Mermaid + happy-dom / D2 WASM / ECharts）渲染 PNG，同样经占位块+素材回填插入为图片；渲染器缺失（受限环境）或渲染失败时就地把占位块降级为同语言代码块（源码不丢、导入不中断），失败原因折成单行回传；`diagram_source=keep` 在图下再留一份源码代码块（独立 `diagsrc*` id 块组，避免与占位块数字 id 冲突）**/行内样式（加粗/斜体/粗斜体/删除线/行内代码/链接）**；段落行首两个全角空格（或 `&emsp;&emsp;`）→ `text.style.indentation_level=OneLevelIndent` 首行缩进；引用内代码围栏转行内代码元素（**实测 quote 块不支持子块**，无法内嵌列表/代码块）；新建文档时首行 H1 与 title 相同自动去重，GitHub 告示语法 `> [!NOTE]`/`[!TIP]`/`[!IMPORTANT]`/`[!WARNING]`/`[!CAUTION]` 自动转 **callout 高亮块**（浅色系背景+同色系边框枚举+emoji，标题粗体置首）；经 descendant 通道写入，自动分批 ≤1000；**代码块 language 按飞书官方枚举表（75 项）映射**（语言标识归一化查表：ts→TypeScript 63、js→JavaScript 30、py→Python 49、cpp→C++ 9，未知回退 PlainText），默认 `wrap=true` 自动换行（长行不溢出）；`engine="official"` 走官方 `blocks/convert` 转换通道，**复杂组合（代码块+表格）转换失败时自动回退本地转换**并提示）、导出（`export_tasks` 创建+轮询——**轮询查询只携带 `token` query 参数**（实测多余 file_extension/type 报 field validation failed），任务终态失败（job_status 3/107~123）提前返回 job_error_msg；**导出 token 必须是文档级 token**——bitable 传 app_token，传数据表 table_id 报 1069914 file token invalid；**bitable/sheet 导出 csv 必须指定 `sub_id` 子表 ID**（官方接口要求，xlsx 不需要）→ `download_file`）
- **块层级能力矩阵（实测）**：**quote 块不支持子块**（带 children 报 1770041 open schema mismatch）——引用内列表/代码块只能文本化；**普通文本块（text/heading）也不支持子块**（块层级只由列表嵌套与容器表达）；列表块可嵌套列表块（bullet+bullet/ordered+ordered/bullet+todo）、callout 可带 text 子块、**table_cell 可带多个 text 子块与列表子块**；`folded` 折叠需子块，实际只对列表块有意义
- **图形化制图能力边界（实测，防止重复探测）**：飞书**不支持经 API 把 mermaid/PlantUML/UML 导入为可编辑图形**——① diagram 块（46，文档「绘图」块）不可创建（`1770029 block not support to create`）；② 官方 `blocks/convert` 对 ` ```mermaid `/` ```plantuml ` 围栏只产出 code 块（14），不转图；③ 画板（mindnote 块 43 + `board/v1`）半可行：mindnote 块可创建且平台自动分配画板 token，**`POST /open-apis/board/v1/whiteboards/{token}/nodes` 可写入图形节点**（composite_shape 的 rect/round_rect/ellipse/diamond + text + style，已验证渲染正确；style 为字符串枚举——`border_width:"narrow"`/`font_weight:"regular"`/`horizontal_align:"center"`，数字会报 99992402；写入为最终一致，立即 GET 可能为空），**但连线无法锚定节点**——connector 节点可创建（`connector:{start,end}` 正确写法，`connector:{start_object..}` 报 4005072 connector info empty），然而 start/end 内的节点引用字段（id/object_id/attached_object_id 等 10 种均试）被服务端静默忽略，连线不落地（下载画板图片可见图形无连线）；此外无节点更新/删除接口（DELETE/PATCH/PUT nodes 均 404），写接口未见于官方公开文档（公开的仅 `GET nodes` 与 `download_as_image`）。**可行替代（已实现）**：本地渲染→插入为图片——`import_markdown` 与 `import_xml`（`<whiteboard type="mermaid|plantuml|d2|echarts">`）统一经 `insertWithMedia` 共享管线：调 `ctx.renderDiagram`（与 show render=backend 同一渲染器）渲染 PNG 后经 image 占位块+素材上传回填插入（已验证：图可下载为非空 PNG）；渲染失败/渲染器缺失降级为代码块，`diagram_source=keep` 可保留源码；`download_as_image` 可用于画板内容取图
- **导入/导出**：`import_markdown`（Markdown → 块，见下）、**`import_xml`（XML 排版语法 → 块，整篇创作首选）**、`export_doc`（`export_tasks` 创建+轮询——**轮询查询只携带 `token` query 参数**（实测多余 file_extension/type 报 field validation failed），任务终态失败（job_status 3/107~123）提前返回 job_error_msg；**导出 token 必须是文档级 token**——bitable 传 app_token，传数据表 table_id 报 1069914 file token invalid；**bitable/sheet 导出 csv 必须指定 `sub_id` 子表 ID**（官方接口要求，xlsx 不需要）→ `download_file`）
- **文档创作与排版（排版 SKILL + 两阶段工作流）**：根因是 Markdown 只能表达飞书块模型子集（自动编号/分栏/高亮块配色/表格列宽/题注均无语法），故排版能力分三层落地——
  - **排版 SKILL 资源**（`styles/`：`style.md` 排版总纲 + `xml.md` XML 语法 + `genres/*.md` 15 类体裁契约（备忘简报/周报/方案/执行计划/PRD/技术文档/SOP/复盘/会议纪要/研究报告/数据报告/商业分析/白皮书/正式文档/法定公文），`style.md` 内置体裁选择表（读者任务与排除信号优先，关键词仅用于召回）；**入口 `styles/index.ts` 以 `with { type: "text" }` 显式 text loader 导入**（Bun 默认对 .md 走 html loader，会包 `<p>` 标签，显式属性使技能文档拿到原始 Markdown、不依赖 bunfig loader 生效范围），dev 与 dist/二进制均由打包器内联）；经 `style_guide` 工具**按需读取**（不占用常驻上下文）：无 `name` 列出全部文档，`name` 返回全文，未知名回可读清单；`styles.test.ts` 断言 genres 目录与登记表一一对应，防漏登记/死登记
  - **整篇创作（`import_xml`）**：类 HTML 标签 → 块描述（`docx_xml.ts` 纯转换层，零 HTTP 依赖、可独立单测）→ 与 `add_blocks` 同一套块归一流程（callout 配色/子块、code 语言枚举、todo done、表格列宽）+ `prepareGridColumns` 列补全 → descendant 通道写入。语法覆盖：`<title>`（首个同名 H1 自动去重）、`<h1>~<h9 seq="auto">`（**自动编号 1 / 1.1 / 1.1.1**，跳级补 1 并提示）、`<p align>`、`<ul>/<ol seq>/<li>`（嵌套写在 `<li>` 内，有序起始序号落 `ordered.style.sequence`）、`<checkbox done>`、`<blockquote>`、`<hr/>`、`<br/>`、`<pre lang caption>`（**代码内容按字面文本处理**：`<pre>` 内标签先转义再解析，代码示例里的 `<h1>`/`<div>` 不会被当块消费）、`<table><colgroup><col width>`（→ `table.column_width`，数量不符回落内容自适应并提示）+`<thead>/<th>`（→ `header_row`）、`<callout emoji background-color border-color text-color>`（→ callout 顶层字段；`light-*` 用基础色相、`medium-*` 用 +7 偏移枚举，`medium-*` 边框回落基础色并提示；非段落/列表子块移出并提示）、`<grid><column width-ratio>`（2~5 列）、`<img path href caption>`（题注落斜体段落）、`<whiteboard type="mermaid|plantuml|d2|echarts">`（内联源码或 `path="@./a.mmd"` 引用本地文件：`xmlToBlocks` 接受注入的 `readFile`（工具侧用 `readFileSync`+`ctx.resolvePath`），读取失败只记说明不阻断整篇 → `ctx.renderDiagram` 渲染 PNG 插入）、`<whiteboard type="svg">`（本地文件直传图片、内联转字节后走同一媒体回填管线；飞书画板无法从 SVG 创建故不可编辑）、`<cite type="user" user-id="ou_…"/>`（→ `mention_user` 元素，**@人可用**；经实测只用真实 open_id）+ 行内 `<b>/<em>/<u>/<del>/<code>/<a>/<span text-color background-color>/<latex>`；不支持项（`<bookmark>`/`<button>`/`<time>`/`<task>`/`<sheet>`/`<cite type="doc">`（**@文档实测 1770038**）/`<source>`/`<html5-block>`/`<okr>`、单元格底色与合并、图片宽高（**image.size 只读，实测 1770001**）、公式块）**逐条给出降级说明而非静默丢弃**；XML 语法/标签未闭合就地报可读错误（含标签名）供模型局部修正
  - **写入前预检（`import_xml` 的 `dry_run`）**：只解析不写入（**零请求——既不建文档也不写块**，避免历史上「语法错也先建出空文档」）；输出块画像（`xmlProfile`：顶层块/总块含子树/字数/类型分布）+ `precheckMedia` 问题清单（本地图片存在与否与 20MB 上限、URL 图片可达性与 content-length、内联 SVG 提示、图表试渲染结果）——把错在产生半成品之前暴露
  - **回查精修（`lint_doc`）**：读取块结构后按文档流（page 根块深度优先，子块 id 解析）逐条体检并**输出 block_id + 修复动作**（可 `limit` 截断）：标题层级跳级/连续标题/空标题/层级过深、空段落与超长段落（>300 字或 >6 行）、表格列宽过窄（<100px）/缺表头/列数过多、代码块缺语言、列表嵌套过深（>3 层）、分割线过密、高亮块滥用、缺标题结构、纯文字墙；无问题时给出明确结论。修法：表格 `set_table_width`、文本 `update_block`、长段拆分 `add_blocks`+`delete_blocks`，每轮改后重新体检（**不沿用旧 block_id**）。`lintBlocks` 为纯函数（`feishu_api.ts` 导出，可单测）
  - **定位与批量改写**：`get_doc_blocks outline=true` → `docOutline` 纯函数抽标题（层级/文本/block_id/**每节顶层块数**）成大纲，大文档先看目录再按节读取（对应官方 `--scope outline` → `--scope section` 两步）；`replace_text` → `replaceInBlocks` 纯函数**逐 text_run 替换以保留行内样式与链接**，命中跨 run（跨样式边界）的块无法逐段替换、单列进 `crossRun` 提示改用 `update_block`；正式提交走公开接口 `docx.v1.documentBlock.batchUpdate`（`PATCH blocks/batch_update`，单请求多块，实测可用）每批 20 块，整批失败时逐块重试并如实报出失败块
  - **实测平台限制（对照官方 skill 逐项验证，不再尝试）**：**文档历史版本/回滚不可用**——`docx/v1/documents/{id}/versions`、`drive/v1/files/{id}/versions`、`docx/v1/documents/{id}/history` 均 404（公开 OpenAPI 清单中 docx 域无版本接口，仅 `drive.v1.fileVersion` 手动打版本无回滚；官方 CLI 仓库无 history 实现文件）；**无块移动接口**（`children/move`、`children/batch_update` 均 404）；**图片显示尺寸只读**（`image.size`/`image.width` 两写法均 1770001）；**@文档（mention_doc）不可用**（obj_type 1/3/8 与带 url 组合均 1770038），只能改用普通链接
  - **实测修正（grid 分栏原实现必然失败，已重写）**：descendant 接口要求**每个 grid_column 带整数 `width_ratio` 且至少一个子块**——缺 `width_ratio` 或空列报 1770041 open schema mismatch，`width_ratio` 传小数（0.5）报 **9499 Invalid parameter value**（只接受整数）；`widthRatioWeights` 把 0~1 小数权重整体 ×100 取整后按最大公约数约分（0.5/0.5→1/1、0.6/0.4→3/2、0.25/0.75→1/3），`prepareGridColumns` 补默认权重与空子块并**保留列内内容随分栏一次创建**（旧实现剥离列内容与列宽、引导逐列 `update_block` 回填，必被接口拒——已删）
  - **实测能力校准（字段枚举）**：callout `background_color` 1~14（1~7 基础色相 + 8~14 medium 浅色）、`border_color` 1~7、emoji_id 接受 51 种内置表情名（bulb/warning/pushpin/heart/thumbsup/check/rocket/…，任意名不报错但仅内置名有图）；文本/标题 `style.align` 1/2/3（左/中/右）；表格 `column_width` 指定列宽与 `header_row` 均原值生效；`todo.style.done` 可写
- **错误可诊断性**：API 错误统一携带 `method path` 与常见错误码提示（token 失效/过期、scope 未开通、参数不合法）；**权限类错误码（9999166x/9999167x，含实测 99991668/99991672 等未逐一收录的码）自动附「按接口路径特征推断的所需 scope + 开发者后台授权链接」（`https://open.feishu.cn/app/{app_id}/auth?q={scope}`，app_id 非敏感）**，减少 AI 反复试错、引导用户快速授权；docx 块操作失败时本地探测诊断，区分 `DOC_NOT_FOUND` / `BLOCK_NOT_FOUND` / 叶子块不支持子块，避免「invalid param」式无信息错误码的盲猜重试
- **用户授权（user_access_token，创建用户所有权资源）**：默认应用身份（tenant_access_token，资源归应用所有）；`auth_user_authorize`（生成 OAuth 授权链接：`accounts.feishu.cn/open-apis/authen/v1/authorize`，参数 scopes/redirect_uri/state/prompt=consent，缺省 scope `docx:document offline_access auth:user.id:read`）→ 用户授权 → **默认自动回调**（回调地址 = `GEBAI_PUBLIC_URL`（缺省 `http://localhost:{GEBAI_PORT|3000}`）+ `/api/v1/oauth/feishu/callback`，首次使用需在开发者后台「安全设置 → 重定向 URL」登记）→ **REST 回调端点自动兑换并写回发起授权的会话**（无需粘贴 code）；回调失败时保留手动路径：粘贴带 `code` 的回调地址 → `auth_user_token`（**OAuth v3 token 端点 `accounts.feishu.cn/oauth/v3/token` 兑换**（grant_type=authorization_code，携带 client_id/client_secret）；`code` 从完整回调地址自动提取，state 与授权时比对防混淆；尽力绑定用户信息）→ `auth_user_status` 查看/`auth_user_clear` 清除。**令牌按会话存储**（会话目录 `feishu_user_token.json`，`UserTokenStore` 依赖注入可测试，绝不输出明文）；**配置后资源类接口（docx/drive/sheets/bitable/wiki/board/搜索）自动以用户身份调用**（创建资源归用户所有、读写用户文档无需再添加应用协作）；access 过期**自动用 refresh_token 刷新**（单次有效、轮换落盘），刷新失败（授权超 365 天等 OAuth 错误 20037/20064/20073 附提示）回退应用身份并提示重新授权；**99991679（用户令牌缺权限）自动附 `permission_violations` 缺失 scope 清单 + 重新授权引导，且失效缓存重读一次自动重试**（重新授权后新令牌自动生效）；OAuth 错误码（20003/20004/20065 授权码失效、20071 回调不匹配、20074 未开启刷新开关等）附可读提示；**REST 回调端点 `GET /api/v1/oauth/feishu/callback`**（公开免鉴权，state 即会话关联凭证：随机不可猜、一次性消费防重放；兑换成功发布 `event oauth.completed` 并回显绑定用户；失败页附可读原因）
- **云空间 drive**：文件清单/文件夹创建/**元信息（走 `metas/batch_query`，`files/{token}` 对 docx 返回 404；缺省 type 自动识别失败时自动按 `doc_type=file` 补查）**/上传（multipart，支持 base64）/下载（落会话 tmp/，文件名经 batch_query 获取；**下载请求携带 `Range: bytes=0-` 头**——导出文件缺 Range 返回 403；**导出产物是 media 类型 token，`files` 接口 403/404 时自动回退 `medias` 下载接口**）/删除
- **电子表格**：创建（v3）、工作表信息（**合并返回完整工作表列表 `sheets/query`**）、读写（v2 values，`range` 前缀**支持工作表名称自动解析为 sheet_id**（用名称读写本须 sheet_id，直接用报 90215）；**前缀与任何工作表名/sheet_id 均不匹配时本地直接报错并列出可用工作表清单**——常见误因：误用表格标题作前缀，替代接口侧 90215 盲错；sheet_id 按 `sheets/query` 返回字段匹配透传，不依赖 oVs 前缀形态）、追加行（**单格 range 自动扩展为覆盖全部数据行的区域**，只传起始格追加多行不再报 wrong range）
- **多维表格**：创建（**支持 `fields` 字段定义数组创建后自动建自定义字段**——默认表只有基础字段，写入自定义字段名报 FieldNameNotFound；type 枚举：1 多行文本/2 数字/3 单选/4 多选/5 日期/7 复选框/11 人员/13 电话/15 超链接，单选多选需 `property.options`，逐个 POST `/tables/{id}/fields`；**创建后平台默认自动生成 10 条空占位记录（平台行为）**，写入时直接更新/追加即可）、数据表列表、记录增删改查（批量新增/更新/**删除走 `records/batch_delete`（POST，body 为字符串数组 `records: [\"rec1\",...]`，对象数组实测报错；DELETE 的 body 不生效）**、查询支持 filter——**filter 走 `records/search`（POST body），GET `?filter=` 报 InvalidFilter**）
- **知识库**：空间列表、创建节点（创建后可用 Markdown 写入正文）、按 token 查询节点
- **搜索**：云文档搜索（`suite/docs-api/search/object`，需开通「云文档搜索」权限）
- **权限**：添加协作者（`drive/v1/permissions`）、**链接分享设置（`set_link_share`：**PATCH** `permissions/{token}/public`（实测修正：方法为 PATCH 非 PUT，PUT 404），type 必填缺失 404，link_share_entity 枚举 tenant_readable（缺省组织内可读）/tenant_editable/anyone_readable/anyone_editable/closed）**
- **目标文件夹（让用户直接拥有资源权限）**：`FEISHU_DOCS_FOLDER_URL`（或全局 `GEBAI_FEISHU_FOLDER_URL`）配置用户的飞书文件夹（URL 或 folder token；路径含 /folder/{token} 自动解析）——创建类接口（create_doc/import_markdown 新建/create_sheet/create_bitable/create_folder/upload_file）未显式传 `folder_token` 时缺省落在该文件夹下（显式传参优先），用户在飞书里对该文件夹有权限即自动拥有其中资源（无需逐个分享/转移）；未配置则落应用云空间根目录（归应用所有，创建结果附落位说明）；配置值无法识别（既非 URL 也非 token）时报错提示修正而非静默忽略；权限类操作受限（`add_permission` 9999166x/403 等）自动附「文件夹方案」引导
- **兜底**：`api_call` 直接调用任意 `/open-apis/` 接口（自动携带 tenant_access_token，路径白名单校验），保证新接口零等待可用

凭证从环境变量读取（子Agent 前缀规范）：`FEISHU_DOCS_APP_ID` / `FEISHU_DOCS_APP_SECRET`（兼容全局 `GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`，见「常量与环境变量」表）；**目标文件夹 `FEISHU_DOCS_FOLDER_URL`（兼容全局 `GEBAI_FEISHU_FOLDER_URL`）配置用户自己的文件夹后，创建类接口缺省落在该文件夹下**——用户在飞书里对该文件夹有权限即自动拥有其中资源，未配置时落应用云空间根目录（归应用所有，创建结果附落位说明）；统一使用 `tenant_access_token`（应用身份），仅能访问**应用自有资源**（应用云空间），访问用户文档需文档所有者授权；token 模块级缓存（有效期提前 200s 刷新）；写操作（创建/修改/删除/上传/授权）全部 `requiresApproval`；工具经 `createFeishuTools(deps)` 依赖注入工厂（fetch/token 缓存可注入），`markdownToBlocks`/`textElements`/`stripTableMergeInfo` 为纯函数，可独立单测（行覆盖率 95%）。**提示词纪律**：先确认落位（创建类操作前明确资源落在配置的目标文件夹还是应用云空间，未配置且用户本人要用时给出配置方式或先 `add_permission` 分享）；先读后写（修改/插入前先 `get_doc_text`/`get_doc_blocks` 确认当前内容、`find_blocks` 定位 block_id，防基于过期内容修改）；写操作前说明改动点与影响范围（审批预期）；批量写入自动分批（块 ≤50/记录 ≤100），限频 3 次/秒失败等待重试，不并发轰炸同一接口；**文档创作与排版**（`feishu_docs.md` 独立章节）：整篇创作走两阶段——① 读规范（`style_guide` 读 `style` 总纲与对应体裁契约，用 XML 时补读 `xml`）② 一次成型（整篇 `import_xml`，Markdown 草稿/简单追加用 `import_markdown`）③ 回查（`get_doc_text`/`get_doc_blocks`/`find_blocks`）④ 精修（`lint_doc` 体检报告逐条最小范围修复，每轮重查、不沿用旧 block_id）；排版原则（读者本位/结构先行/视觉服从语义/克制连贯/编号体系唯一/颜色表达语义）与 Markdown→飞书块对照表保留在提示词内（摘要），完整规范与自检清单在 `style_guide` 技能文档内（按需读取，不占常驻上下文）。

#### `playwright`（浏览器自动化）

实现于 `packages/agents/src/agents/playwright/`（目录形式：`playwright.ts` 定义入口 + `playwright.md` 系统提示词 + `playwright_tools.ts` 工具集 + `playwright_session_tools.ts` 会话类工具集 + `playwright_cv_tools.ts` 本地识别工具集；桥接基建与 node 驱动在 `core/browser/`，见「桥接架构」），基于无头 Chromium（playwright）提供浏览器自动化：

- **工具集**（30 个）＝ 导航与内容（`playwright_tools.ts`）+ 会话类（`playwright_session_tools.ts`）+ 本地识别（`playwright_cv_tools.ts`）：
  - 导航/内容/交互（`playwright_tools.ts`，20 个）：`open`（导航，支持 http/https 与 **file:// 本地文件**，可配 wait_until；失败输出「打开失败: …」+ 排查建议）、`content`（text/html/both 读取，超长自动截断落盘）、`screenshot`（页面/元素/整页，保存会话 `tmp/` 并返回图片块与绝对路径）、`click`/`fill`/`press`/`select`/`check`（交互与表单；`check` 的 checked 非布尔值直接报错，不当 true 静默处理）、`hover`（悬停展开菜单）/`dblclick`（双击）/`drag`（元素拖拽，locator.dragTo）/`upload`（文件上传 setInputFiles，files 经 `resolvePath` 沙箱解析）、`wait_for`（元素/URL/网络空闲等待）、`evaluate`（页面内 JS 执行，JSON 序列化结果；**表达式与函数字面量 `() => …`均支持**（后者自动调用，否则求值得函数对象而序列化为 undefined）；失败提示改用 `content` 观察 DOM；**求值失败时输出保留页面内错误原文并附求值形态与表达式首行**——页面内 `fetch` 取 JSON 前先验响应（`r.ok`/`content-type`），对非 JSON 响应（如 404 的 `Not Found` 文本）调 `r.json()` 会抛 JSON 解析错误，那是响应体问题而不是本工具异常）、`pages`/`new_page`/`switch_page`/`close_page`（多标签页管理；**浏览器内部页**（edge://downloads-hub 等 WebUI）不进标签页索引，`close_page` 有界等待 5s，超时转「忽略」而不拖死桥接请求）、`close`（关闭会话浏览器上下文）、`serve_dir`（**内置静态文件服务器**：服务本地目录返回 http 地址，解决本地 HTML 需手工起服务的场景；服务端部署仅可服务预置项目目录）
  - 会话类（`playwright_session_tools.ts`，7 个）：`pdf`（页面导出 PDF 到会话 `tmp/`）、`downloads`（**下载捕获**：页面触发的下载自动存到系统临时目录，list 查看 / save 复制进会话目录）、`dialogs`（**对话框管理**：alert/confirm/prompt 记录 list + 自动应答 auto（accept/dismiss，prompt 可配文本）+ clear；无配置时默认 dismiss 防页面冻结）、`emulate`（**环境仿真**：视口/UA/语言/时区/移动端模式（isMobile+hasTouch），action=reset 恢复默认）、`cookies`（cookie list/set/clear，输出真实凭证）、`local_storage`（当前页 origin 的 localStorage 读写，driver 定式 evaluate 非任意脚本）、`storage_state`（**登录态持久化**：save/restore playwright storageState 格式（cookie+各 origin localStorage）文件，跨会话/跨任务免重复登录；restore 逐 origin 开临时页写 localStorage，不可达 origin 告警不中断）
  - 本地识别（`playwright_cv_tools.ts`，3 个，只读免审批）：`ocr`/`locate`/`locate_image`——**复用 desktop 同款 core/cv 小模型基建**（共享工厂 `@gebai/agents` core/shared/cv-analysis，见「小模型识别」），缺省图像源注入为「共享桥接截当前页视口」（与 playwright 工具同一桥接单例与会话锁；临时文件**每次调用唯一命名**（序号+随机，并发调用不互相覆盖）且用后即删；image 参数传 PNG 时同 desktop 语义）：`playwright_ocr`（读当前页面/图片文字：region 限定区域、find 过滤，返回行文本+框+中心坐标+置信度）、`playwright_locate`（按目标文字定位视口像素坐标）、`playwright_locate_image`（模板匹配定位图标/图形——验证图标/logo 真实渲染）；坐标一律**视口像素系**（`evaluate` 的 `document.elementFromPoint(x, y)` 定位元素后直接操作）；浏览器为隔离环境，**无 desktop 的本地模式闸门，服务端部署（沙箱）可用**；定位 canvas/WebGL 渲染文字、图片化文字/验证码等 DOM 读不到内容的兜底通道（普通文本/元素仍优先 content/选择器），语义理解按需装载 vision 子代理（vision_analyze）；**环境耦合的缺省源便利留在域内（复用方式二），不依赖 vision 子代理**（边界判定见「视觉能力分层与子代理复用边界」）
- **选择器穿透 iframe**：selector 支持 `iframe选择器 >> 子iframe选择器 >> 目标选择器`（`>>` 分段，前段逐级 `Locator.contentFrame()`），click/fill/content/screenshot 等全部带 selector 的工具生效；CSS 引擎天然穿透开放 shadow DOM
- **桥接架构（core/browser/bridge.ts，平台级基建）**：Bun 运行时与 playwright driver 的 pipe 通信存在兼容问题（chromium 启动超时，node 环境正常），因此 **Bun 进程内不直接 import playwright**——桥接 spawn 常驻 `node driver.mjs` 子进程（driver 在 `core/browser/driver.mjs`），经 stdin/stdout 行分隔 JSON-RPC 通信（请求 id 匹配、180s 超时杀进程重启、stderr 环形缓冲诊断；**kill 按 pid 整树终止**——Windows 无进程组，`proc.kill()` 只终止 driver，其拉起的浏览器孙进程会成孤儿驻留（占内存与 profile 临时目录），故 Windows 下先 `taskkill /pid <pid> /t /f`；**driver 回传的错误信息剥除 ANSI 转义码**（playwright 的 Call log 自带颜色码，回传模型是噪音））；driver 内 Browser 单例（断开自动重建），BrowserContext 按 sessionId 隔离（多用户/多会话互不串扰，`acceptDownloads: true` 创建），空闲 10 分钟惰性回收；页面级监听（dialog/download/websocket 帧）经 `context.on("page")` 统一挂接；Bun 侧按会话串行化工具调用（同会话防并发操作同一页面，`withSessionLock`）。桥接进程与浏览器会话**全进程共享单例**（`createLazyBridge()` 惰性构造——首次工具调用才解析 playwright 模块并启动桥接进程，playwright 与 reverse_site 两命名空间及透明浏览器代理操作同一浏览器，页面状态一致；解析失败降级为工具级报错，不影响服务启动）。基建属 core 域（`core/browser/`），**不依赖任何子Agent 定义的存在**——playwright 被裁剪/停用时子Agent 工具面消失，但桥接与透明浏览器代理照常可用
- **网络录制（driver 扩展能力，reverse_site 使用）**：driver 额外提供 `network_*` 操作——按会话录制浏览器上下文内的全部请求（方法/URL/类型/请求头/请求体/响应状态/响应头/响应体预览，单会话上限 500 条，响应体预览上限 20KB，entry 保留 Response 对象供完整响应体提取），**录制时自动脱敏**（`authorization`/`cookie`/`x-api-key`/`set-cookie` 等敏感头与 postData 中 `token`/`password`/`secret` 等键值一律 `***`）；`network_list` 支持按 method/url（正则或子串）/status 过滤、detail 详情开关与 limit 条数上限（HAR 全量导出用）
- **透明浏览器代理（`GEBAI_BROWSER_PROXY=1`，服务启动时 `boot/compose.ts` 安装、重启生效）**：内网「仅限浏览器访问」（非浏览器进程直连被拒）场景的进程级 fetch 垫片——`core/browser/fetch-proxy.ts` 替换 `globalThis.fetch`，**工具执行作用域内**（引擎 `runToolInterruptible` 统一进入 `runInToolFetchScope(sessionId)`，原语在 `core/support/fetch-scope.ts` 的 AsyncLocalStorage）的 http(s) 请求自动改经共享浏览器会话的 `context.request` 发出（携带浏览器 UA 与会话 cookie/登录态），由 driver `http_fetch` 操作承载（响应体经系统临时文件中转——二进制安全、绕开桥接 JSON 结果上限，读毕即删；返回**未脱敏**响应头，垫片构造回标准 `Response`，模型可见输出的脱敏仍由各工具层自行负责；单次请求超时 110s）。**平台级能力：不依赖任何子Agent**——安装点在组合根（boot/compose），桥接基建在 core/browser，playwright 被裁剪/停用不影响代理可用；工具代码与模型零感知（工具 schema/系统提示词均不变），任何子Agent（含 self_optimize 生成的新Agent）用普通 `fetch` 即可访问受限内网站点。边界：作用域外（LLM 请求/webhook/调度/启动逻辑）不受影响；嵌套引擎（`subsession_run` 在工具内跑完整引擎）的 LLM 流式请求经 `runWithoutFetchProxy` 豁免（`core/llm/llm.ts` fetchRetry 单点包裹）恒直连；sh/py/js **子进程**内网络不经此垫片；`Request` 对象输入与 FormData/Blob 等非字符串请求体回退直连（multipart 上传等复杂形态）；`redirect:"manual"` 映射 `maxRedirects=0`（`fetchWithRedirectGuard` 逐跳 SSRF 校验语义保留——沙箱公网限制不因代理放宽）；AbortSignal 语义保留（触发即 AbortError）。未开启（缺省）时安装为 no-op 零开销
- **仿真档案语义**：UA/视口/locale/timezone 为上下文级选项，只在 newContext 时生效——`emulate` 存档后**立即重建上下文应用**（已打开页面/cookie/录制状态清空，拦截规则自动重挂；工具描述与提示词均注明「需保留登录态先 storage_state save」）
- **依赖与部署**：运行时需宿主机可执行 `node`。playwright 模块解析顺序：**单二进制形态**优先物化内嵌 playwright-core（`packages/server/scripts/build-pwcore-embed.ts` 整树 gzip base64 内嵌 → 运行时物化 `{GEBAI_HOME}/vendor/playwright-core/`，版本不一致自动重建）；回退 `playwrightModuleUrl()` 经 `Bun.resolveSync` 解析 node_modules 的 playwright 包（源码/服务端部署形态）。浏览器本体不内嵌：Windows 默认 `channel=msedge` 驱动系统自带 Edge（Win10/11 必装，免下载浏览器），`GEBAI_PLAYWRIGHT_CHANNEL` 覆写（如 `chromium`/`chrome`，留空 = 不指定 channel、用默认 chromium——需 `bunx playwright install chromium`，Linux 服务端部署即此路径）。模块缺失/解析失败降级为**工具级报错**，不影响服务启动与其他子Agent。构建时 `driver.mjs` 由 `build-subagents.ts` 复制到 `dist/` 与产物同目录（不能被 bundle 内联）
- **审批**：导航/交互/脚本/服务/凭证/仿真类（`open`/`click`/`fill`/`press`/`select`/`check`/`hover`/`dblclick`/`drag`/`upload`/`evaluate`/`new_page`/`serve_dir`/`emulate`/`cookies`/`local_storage`/`storage_state`）默认需审批——服务端部署下可被诱导访问内网/提交表单，防 SSRF 与任意脚本滥用；**凭证类（cookies/local_storage/storage_state）输出真实值，审批门控与 evaluate 同级**；只读类（`content`/`screenshot`/`pdf`/`downloads`/`dialogs`/`pages`/`wait_for`/`switch_page`/`ocr`/`locate`/`locate_image`）与清理类（`close_page`/`close`）免审批
- **提示词纪律**：等待一律 `wait_for` 目标条件（不用固定 sleep——异步渲染时序不可靠）；被 `code`/`self_optimize` 委托做浏览器端验证时明确验证目标→操作→content/screenshot 取证→给出结论与证据位置；交互类操作前先说明意图（审批预期）；凭证输出只用于用户明确要求的登录态分析/注入，不外传不入文档

#### `reverse_site`（网站/接口逆向）

实现于 `packages/agents/src/agents/reverse_site/`（目录形式：`reverse_site.ts` 定义入口 + `reverse_site.md` 系统提示词 + `reverse_site_tools.ts` 工具集 + `reverse_site_net_tools.ts` 网络增强工具集 + 各自 `*.test.ts` 测试），面向「逆向网站与接口」场景——摸清站点结构、还原接口契约、产出分析文档，并可与 `self_optimize` 联动把逆向结果转化为新的子Agent：

- **依赖 playwright（`dependencies: ["playwright"]`，装载/预加载自动连带，见「子Agent 依赖与自动装载」）**：浏览器自动化全套由 playwright def 提供（`playwright_` 前缀：导航/内容/交互/截图/多标签 + 会话类 pdf/downloads/dialogs/emulate/cookies/local_storage/storage_state，审批映射亦由 playwright def 单源维护）——本 def 不复刻任何浏览器工具；capture_*/route 经共享桥接单例（`createLazyBridge` 全进程共享）与 playwright 操作**同一浏览器会话**，页面操作与网络录制天然一致；playwright 被启停名单移除/构建裁剪时跳过（本 Agent 仅剩接口直连探测）
- **工具集**（11 个，全部 `reverse_site_` 前缀）＝ 接口捕获与重放（`reverse_site_tools.ts`：`capture_start`/`capture_stop`/`capture_clear`/`capture_list`/`capture_body`/`capture_replay`/`capture_curl`）+ 网络增强（`reverse_site_net_tools.ts`：`capture_har`/`capture_ws`/`route`）+ 接口探测（`http_request`）；文件读写与编排（write/read/fetch_url/subsession_run）为全局工具直接用全局名（提示词开头列明三段命名空间映射）
- **接口捕获**（`capture_*`）：录制浏览器网络请求还原接口——浏览前 `capture_start`，浏览/操作页面让 XHR/fetch 自然发生，`capture_list` 分析（默认摘要，`detail=true` 含请求头/体与响应头/体预览，可按 method/url/status 过滤，`file` 参数把完整记录导出会话 `tmp/` JSON）；`capture_body` 取指定请求的**完整响应体**（文本预览 / `file` 落盘支持二进制与大文件，绕过桥接结果上限直接写盘）；`capture_har` 把录制导出 **HAR 1.2**（Chrome DevTools 可打开，脱敏口径与录制一致）；`capture_ws` 查看录制的 **WebSocket 帧**（行情/聊天/通知类实时接口，随录制开关联动，单会话 300 帧/单帧 4KB 预览）；录制在 driver 侧完成（见 playwright 网络录制），**敏感字段（cookie/token/密码等）自动脱敏**
- **带登录态重放**（`capture_replay`）：以 `context.request.fetch` 重放录制的请求——**自动携带浏览器 cookie/存储状态，可重放需登录接口**（`http_request` 做不到）；`method`/`url`/`headers`（覆盖合并原始请求头）/`body`/`params`（合并进 URL 查询串）为覆盖项，改参试边界一步到位；沙箱模式目标 URL 过公网校验且**不自动跟随重定向**（3xx 返回 Location 由模型逐跳改 url 重放，防跳板 SSRF）；`capture_curl` 从**原始未脱敏请求**生成可直接运行的重放命令（curl/fetch/python，`buildReplayCommand` 纯函数）——与 `http_request` 免密直连互补，含真实凭证故审批门控
- **请求拦截**（`route`）：`context.route` 拦截浏览器内全部请求（持续到 clear/会话结束，单会话上限 32 条）——mode=block 中止匹配请求（屏蔽广告/埋点/干扰资源）、mode=mock 伪造响应（status/content_type/body——前端联调、绕过客户端 gating 验证）、mode=modify 改写请求头后放行；上下文重建（emulate）后自动重挂；**作用于浏览器上下文内流量，无新增网络出口**，list/clear 免审批、add 需审批（工具级函数按 action 判定）
- **接口探测**（`http_request`）：直接发送 HTTP 请求验证逆向出的接口（任意方法/请求头/请求体/查询参数），返回状态码、响应头（敏感字段脱敏）与响应体（50KB 内截断展示，超长走截断保护）；服务端部署限公网地址（复用 `assertPublicHttpUrl` 防 SSRF，默认路径另带重定向逐跳校验）；fetch 可注入（测试替身）；默认需审批。`GEBAI_BROWSER_PROXY=1` 时本工具与一切进程内工具 fetch 透明经浏览器代理（见 playwright「透明浏览器代理」）——受限内网站点照常探测，沙箱公网限制不放宽
- **编排闭环**：分析文档写会话 `tmp/`（`site_map.md` + `api_docs.md`/`api_docs.json`，示例请求可用 capture_curl 产物）后经全局 `subsession_run` 把文档交给 `self_optimize`，由 `self_optimize` 生成/修改子Agent 定义文件（`packages/agents/src/agents/{name}.ts` + 可选 `{name}.md`）并通过测试验证——「网站 → 逆向文档 → 新子Agent」全链路
- **审批**：`http_request`/`capture_replay`（直连探测）/`capture_curl`（真实凭证）默认需审批；浏览器交互类与凭证类（cookies/local_storage/storage_state）随 playwright def（连带装载生效）；`capture_start/stop/clear/list/body/har/ws` 与 `route list/clear` 只读免审批
- **提示词约束**：只逆向用户授权/自有网站，敏感信息不扩散，不爆破/不拖库/不高频恶意请求；`capture_list` 先摘要定位候选再 `detail=true` 细看（超长记录 `file` 导出后 `read` 分块分析）；验证优先级 replay（带登录态最快）> curl 命令交付 > http_request 直连；route 用完即清（防干扰后续捕获）；验证多通道——截图/读取失效切 evaluate/capture_list/replay/http_request，失效即切换并告知用户

#### `explore`（只读代码探索）

实现于 `packages/agents/src/agents/explore/explore.ts`（目录形式），参照 ZCode Explore 子代理设计的**只读探索专家**——把「大范围扫读」从主上下文中剥离出去，主会话只拿结论：

- **用途**：跨大量文件的代码摸底/架构梳理/多点位定位——`code`（或总Agent）经 `subsession_run` 委托执行，广度优先搜索后返回**结论 + 文件:行号 引用清单**，中间搜索/读取过程留在子会话存档里，不占父上下文（防上下文膨胀的标准委托形态）
- **硬约束（代码级）**：工具集只声明只读独有工具（search_symbols/analyze/git，全部免审批），文件读取与检索（read/ls/grep/glob/fetch_url/todo）复用**全局只读工具**（subsession_run 委托默认继承全局工具）——没有 write/edit/patch/sh/py/file（含 delete/move 动作），探索不可能产生任何修改或命令执行；需要修改时装载/委托 `code`
- **工作流（提示词内置）**：圈定范围（project 参数/任务给定根；ls/glob（支持花括号与 exclude）看结构、Git 项目用 git ls-files 拿已跟踪文件清单（尊重 .gitignore）、git grep 已跟踪文件内容搜索、grep 宽泛定位先 output=files（exclude 排噪、literal 字面搜代码片段））→ 抽查精读（关键文件 read 分段读（默认带行号，直接引用 文件:行号）、search_symbols 定位定义与**调用点**（mode=references——注释/字符串不误报，梳理调用链优先于 grep）、grep 定位文本（output=count 先估命中面）、analyze 结构概览代替通读）→ 汇总结论（**先结论后细节**，关键位置给 文件:行号，未确认点明确标注不猜测）→ 规模纪律（命中面大先 output=count 估面再挑重点；只保留与目标相关的结论）
- **项目路由**：独有工具经 `projectAware` 包装（实现位于 `core/tools/projects.ts`，与全局文件工具共享同一基础设施）——支持 `project` 参数按预置项目名/路径/保留名 tmp 路由，路径相对所选根解析；受限模式（CODE_RESTRICT_PROJECTS）同规则生效
- 与 `code` 的分工：`code` 是「探索→方案→修改→验证」全流程执行者；`explore` 只做其中的「探索」段且产出为结论——大范围摸底先委托 explore 拿地图，再由 code 精确改动，两段各司其职



#### `feishu_group`（飞书群基础能力）

实现于 `packages/agents/src/agents/feishu_group/feishu_group.ts`，以应用身份（tenant_access_token）操作飞书群基础能力——群查询、群维护与任务通知的取材（@ 人 open_id / 群 chat_id）：

- **工具集**（十工具，`feishu_group_` 前缀）：
  - 查询类（免审批）：`chats_list`（机器人所在群分页列表——chat_id/名称/描述）；`chat_info`（群详情：名称/描述/群主/成员数）；`members_list`（群成员分页列表：open_id+姓名+成员类型——**@特定人的 open_id 来源**）；`user_info`（按 open_id 查用户姓名等信息——核对 at 名单）
  - 写操作（需审批）：`message_send`（向群发文本，正文支持 `<at user_id="open_id">` @特定人与 `<at user_id="all">` @所有人——验证通知效果/直接推送）；`chat_create`（建群：名称/描述/初始成员）；`chat_update`（改群名/描述）；`chat_members_add`（拉人，open_id 列表，失败项逐条给原因）；`chat_members_remove`（移出成员）；`chat_disband`（解散群，不可恢复）
- **凭证**：`FEISHU_GROUP_APP_ID`/`FEISHU_GROUP_APP_SECRET`（子Agent 前缀），缺省回落全局 `GEBAI_FEISHU_APP_ID/SECRET`（与机器人桥接/云文档共用）；机器人须已入群；token 缓存复用 feishu_docs 同款机制（提前 200s 刷新）
- **与任务通知联动**（高频场景）：`chats_list` 查到的群 chat_id 直接填任务通知 `feishu` 通道 `target`（指定群以应用身份推送）；`members_list` 查到的 open_id 直接填 `at` 名单（@特定人）
- **权限提示**：查询需 `im:chat:readonly`（或 `im:chat`），写操作需 `im:chat`，发消息需 `im:message:send_as_bot`，用户信息需 `contact:user.base:readonly`——权限不足的错误附开发者后台开通引导
- **预加载**：`preload = false`，按需装载


#### `task`（统一任务管理）

实现于 `packages/agents/src/agents/task/task.ts`（工具名经命名空间为 `task_*`），管理与执行**用户级**任务——定时（`scheduled`）/普通（`manual`）/闲时（`idle`）三类共用一份存储与一条队列（能力实现见「统一任务管理」），支持脚本运行与提示词运行 agent、执行目标（新会话/专用会话/绑定会话）、时区、一次性 `@at`、错过补跑、执行记录（按文件落盘，见「统一任务管理 → 执行记录」）、连续失败自动停用、飞书群与 Webhook 通知（含模型主动推送 `task_notify`）、任务资源文件：

- **工具集**（八工具，命名空间内单字 `add`/`list`/`update`/`run`/`cancel`/`remove`/`files`/`notify`）：
  - `add`（`runner` 必填，`kind` 缺省按是否给 `schedule` 推断）：创建任务；可选 `name`/`script`/`prompt`/`schedule`/`timezone`/`misfire`/`target`/`session_id`/`agents`/`timeout_ms`/`notify`/`notify_on`/`max_consecutive_errors`/`enabled`/`run_now`/`front`，返回任务行与资源目录提示
  - `list`：查看**当前用户全部**任务（ID/名称/类别/执行体/启用状态/运行态/周期/下次执行/次数/最近错误）+ 队列概览（并发额度、排队顺序与等待原因、运行中条目）
  - `update`（`id`）：按 id 修改（全部可变字段），定时任务改后重算下次执行时间；`notify` 的 `secret` 传 `***` 表示保持原值；重新启用重置连续失败计数
  - `run`（`id`，可选 `front`）：手动执行一次（入队；不改动既定调度节奏），返回队列位置
  - `cancel`（`id`，`mode=dequeue|stop`）：出队（排队中）或终止运行中的那次执行
  - `remove`（`id`）：按 id 删除（不可恢复；任务资源目录文件保留）
  - `files`（`id`，`op=list|read|write|delete`）：任务资源目录（脚本/文档）读写，越界路径拒绝
  - `notify`（`text` 必填，可选 `title`/`id`/`at`）：主动推送一条通知——自撰 markdown 正文投递到任务配置的通道（未配则全局默认通道）；`id` 缺省时按当前会话反查正在运行的任务（任务执行中调用无需传 id）；投递目标限定为用户已配置的通道（不接受任意 URL），无可用通道时返回配置指引
- **审批**：`add`/`update`/`run`/`cancel`/`remove`/`files` **默认需审批**（任务 = 无人值守的任意命令/会话执行，创建/修改/删除/执行/资源文件写入均须用户确认，服务模式下防普通用户绕过审批边界创建后门任务）；`list` 与 `notify` 免审批——无人值守执行等不到人工审批，且通知的投递目标被限定为用户已配置的通道（任务创建时已经过审批，不新增任意外发面）
- **能力开关**：`GEBAI_TASKS_ENABLED` 默认 `true`；显式 `false` 时 `task` 子Agent 不注册（定义从子Agent 清单移除——`agent_list`/`agent_load`/`subsession_run` 均不可见，与调度器、REST 管理面一致完全隐藏）；`ctx.tasks` 未注入（引擎未挂调度器）时工具返回「能力未启用」提示
- **用户级绑定**：任务经 ToolContext 绑定**当前用户**（与会话解耦——任何会话创建后该用户全局可见可管，`TaskManager` 校验用户归属，跨用户不可见不可操作；`originSessionId` 仅记录创建来源会话供结果消息写回）；执行记录同样按用户归属校验（读非本人任务记录报「任务不存在」）
- **预加载**：`preload = false`，按需装载（与其余子Agent 一致）


#### `wps`（Office 文档处理）

实现于 `packages/agents/src/agents/wps/`（目录形式：`wps.ts` 定义入口 + `wps.md` 系统提示词 + 按格式拆分的工具 `word.ts`/`excel.ts`/`ppt.ts`/`pdf.ts` + 共享基础 `ooxml.ts`/`markdown.ts`/`shared.ts`），Word/Excel/PowerPoint（Office Open XML：.docx/.xlsx/.pptx）与 PDF 的创建/读取/追加/编辑与富排版：

- **文档库**（非 AI 依赖）：`docx`（Word 生成——标题样式/编号列表/表格/图片/页眉页脚/目录域）、`exceljs`（Excel 读写——公式/样式/合并/冻结/筛选）、`pptxgenjs`（PPT 生成——版式/图表/表格/形状/备注；**Bun 严格模式下文本数组必须归一为 `{text, options}` 对象形态**，裸字符串数组触发只读属性赋值错误）、`fflate`（ZIP 解包——docx/pptx 是 ZIP 容器，读取与 word_append 重打包共用）、`pdf-lib`+`@pdf-lib/fontkit`（PDF 生成与页面编辑）、`unpdf`（PDF 文本提取——内嵌 pdf.js）；XML 部件解析复用 `happy-dom` 的 `DOMParser`（`text/xml` 模式）
- **正文输入双形态**（word_create/word_append/pdf_create 共用，`markdown.ts` 迷你解析器与 blocks JSON 归一化（`toRuns`/`normalizeBlocks`/`bodyInput`）自实现——文档生成只需受控子集，不引入 markdown 库）：markdown 文本（`#` 标题/行内样式（`**粗** *斜* ~~删~~ \`码\`）/`[链接](url)`/有序无序列表（缩进分级）/表格（对齐）/引用/围栏代码块/`![图](路径)` 嵌入/`<!--pagebreak-->` 分页/`<!--toc-->` 目录）或 blocks JSON（结构化块数组，逐 run 样式与图片定宽）
- **工具集**（十三工具，均 `projectAware` 包装——project 参数路由项目内文件）：
  - `word_create`：富排版建 .docx；`style` 调页面（A4/letter、横竖向、页边距 cm）与正文（字体/字号，默认微软雅黑 10.5pt）、页眉页脚（`{page}`/`{pages}` 页码占位）、文档属性（title）
  - `word_read`：解析回 markdown（标题→`#`、编号/列表、表格、内嵌图片 `[图片]` 占位；首行块数/图片数/页面尺寸摘要；`offset`/`limit` 按块分页读长文档）
  - `word_append`：**原 XML 拼接**——原文档格式/样式/图片原样保留，新内容以直接格式化 XML 插入 body 级 `sectPr` 之前（fflate 解包 → 生成片段 → 图片/超链接补 `document.xml.rels` 关系、媒体入包、`[Content_Types].xml` 补扩展声明 → 重打包；**写前回读校验**，失败不落盘防损坏用户文档）
  - `excel_write` / `excel_read` / `excel_edit`：全量建表（多工作表；单元格标量或 `{value, bold, italic, color, fill, font_size, align, wrap, number_format, border, hyperlink}`——hyperlink 为值形态 `{text, hyperlink}` 统一写入；`=` 开头字符串自动按公式，**写入不含缓存计算值**（Office 打开重算，读取默认模式无缓存值时回显公式原文并标注「（未计算）」——写入侧不计算，错误缓存值比无缓存值更误导）；列宽/合并/冻结 `A2`/自动筛选；**行容错**：单格行以对象/标量直传（未包数组）按单格行收敛并回报提示，不静默丢弃）；读取（不传 sheet 返回概览；markdown（默认）/json（rows 入 data 供 js 编排）/csv 三种输出；`range`（A1:D20 或 A:D）/`max_rows`/`formulas`；.csv/.tsv 文本表格兼容）；ops 批量编辑（`set` 设值设样式 / 行列增删 / `add_sheet`·`rename_sheet`·`delete_sheet` / `merge`·`unmerge` / `col_width`·`row_height` / `freeze`·`autofilter`，各项可带 sheet 指定目标表；改完回读校验）
  - `ppt_create` / `ppt_read`：简式页（title/subtitle/bullets/notes/background）与自由元素版式（`elements`：text 文本框/image 嵌图（会话/项目内路径，自动探测像素尺寸）/table 表格/chart 图表（bar/hbar/line/area/pie/doughnut/scatter，data `[{name, labels, values}]`）/shape 形状（shape 缺省 rect），坐标英寸）；**元素容错**：type 缺失/无法识别时按字段签名推断（path→image、chart_type/data→chart、rows→table、形状名/fill→shape、text→text）并在输出提示；页级误传元素对象（带 type）、或携带元素级字段（chart_type/data/rows/path）未包 elements 数组时自动按附加元素处理并提示；layout wide（16:9 13.33×7.5 默认）/4x3/自定义尺寸，theme 全局字体字号色；读取回每页文本（首个文本框按标题标记）/表格/备注（剔除页码占位符）/图表与图片计数
  - `pdf_create`：markdown/blocks → 排版 PDF（语法同 word_create——标题/行内样式/列表/表格（对齐/列宽/跨页重绘表头）/引用/代码块（灰底）/图片（仅 png/jpg，居中等比缩放）/`<!--pagebreak-->`/`<!--toc-->`）；**CJK 字体自动发现**（按平台扫描系统字体目录：win 微软雅黑/等线/黑体/宋体、mac 苹方/冬青黑体、linux Noto/文泉驿/Droid，`style.base_font` 可传字体族别名或 .ttf/.ttc 文件路径（会话/项目内或系统绝对路径；**仅支持 TrueType 轮廓**——pdf-lib 将自定义字体一律按 TrueType 位置嵌入（FontFile2/CIDFontType2），CFF 轮廓（OTTO 头 OTF，Noto/思源/PingFang 官方发行版）在该结构下不规范、渲染器拒绝光栅化（文本层完好），sfnt 魔数检测拒绝并跳过该候选）），TTC 先按 name 表挑 Regular/Bold 子字体再**二进制重建独立 TTF**（embedFont 只收字节），**子集化嵌入**（仅用到字形，产物小）；纯西文走标准 14 字体（Helvetica 族，探测到非 WinAnsi 字符自动回退自定义字体），粗体优先伴随字体（msyhbd.ttc/Dengb.ttf 或单 TTC 多字重子字体）无则回退常规；字体目录扫描为目录 + 一层子目录（`readdirSync recursive:true` 对 Windows Fonts 触发 EPERM）；`style.subset_font`（默认 true）子集化逃生舱——pyftsubset 等工具产出的字体再经 pdf-lib 二次子集化会损坏 glyf（渲染空白、文本层完好），字形异常时关闭改整字嵌入；**排版引擎自实现**（绘制指令两遍：CJK 逐字/拉丁按词折行、超宽原子硬切、keepNext 标题防孤行、表格逐行分页）；**目录两遍布局**（pass1 虚拟布局记标题页码（目录占 0 页）→ 按条目折行估目录页数 k → pass2 渲染目录（标题+页码+虚线引导）独占页、其后标题页码=虚拟+k）；页眉页脚 `{page}`/`{pages}` 逐页替换居中；style 同 word（page_size/orientation/margins cm/base_font/base_size/header/footer/title）
  - `pdf_read`：pdf.js（unpdf `getDocumentProxy` 低层 API）逐页提取文本层（`## 第 N 页` 分节）+ 首行元数据摘要（页数/标题/作者/生成器）；`pages` 区间选页（"1-3,5,8-"）、`max_pages` 上限（默认 30）、加密 PDF 传 `password`；文本层为空的页标注（扫描件/图片型引导）；内容经截断保护
  - `pdf_merge`：多 PDF 合并（inputs 路径字符串或 `{path, pages}` 选页抽取），copyPages 按序拼接
  - `pdf_split`：ranges（每区间一文件）/every（每 N 页）/single（逐页）三模式，输出 `{prefix}-p{起}-{止}.pdf` 至 outdir（默认会话工作目录）
  - `pdf_edit`：ops 按序原地编辑（页码指当前状态）——`delete` 删页/`rotate` 旋转（±90/±180/±270，相对现有角度累加）/`move` 页序移动（from/to）/`metadata` 元数据（title/author/subject/keywords/creator）/`watermark` 水印（中文自动嵌系统字体，fontSize/color/opacity/angle/pages 可调）；**写前产物自校验**（重新 load，失败不落盘）；内置 pdf-lib `removePage` 不失效 pageCache 的补丁（删除后失效页缓存，防后续 rotate/move 取幽灵页）
- **阅读视图预览（前端内联渲染）**：pdf 产物由前端文件卡/弹窗以 iframe 直接加载 `files/preview` 原文件（浏览器原生 PDF 渲染，无需服务端阅读视图）；docx/xlsx/xlsm/pptx 的文件卡与「原文件」弹窗不再是二进制占位——前端按 office 类型取数 `files/preview?path=…&render=office`（服务端 `wps/preview.ts` 复用读取模型渲染结构化 HTML：Word 标题/列表/表格/图片按出现顺序 data URI 内嵌（单图 ≤2MB、总量 ≤8MB 超额降级为标记）、Excel 按工作表出带样式表格（字体/底色/对齐、合并单元格 rowspan/colspan，500 行 × 64 列截断）、PPT 逐页大纲（标题/表格/图表标记/备注）），沙箱 iframe 承载（同 html 分支）；内容全量 HTML 转义，**不还原精确分页与版式**（阅读视图口径，头部 meta 行明示）；渲染失败（损坏文件/非 office 扩展名 → 422）回退二进制占位与下载引导；渲染器在 app 层**惰性引入**（解析较重不拖启动），解析单一真相源仍在 wps
- **审批与安全**：全部免审批（本地文件产物无外部副作用，与全局文件工具姿态一致）；**防盲覆盖守卫在工具体内**（目标已存在且本会话未读取过 → 拒绝；读取类工具与全局 read 共享会话已读追踪 `fileGuard`，exceljs 类修改工具天然先读后写）；写类工具（word_create/word_append/excel_write/excel_edit/ppt_create/pdf_create/pdf_merge/pdf_split/pdf_edit）显式 `safeMode: false`（安全模式不提供——文档生成必然落盘），读类工具默认注册（实现只读）；路径经 `resolvePath` 沙箱约束、写前经 `writeGuard`（self_optimize 等装载期写范围政策同规则生效）
- **工作流（提示词内置）**：明确需求（素材先浏览，不编造）→ 大纲先行（正式/大型文档 ask 确认）→ 分段生成（超长 Word 先 create 后 append，避免单次输出截断）→ 读回校验 → 交付路径与摘要；排版规范内置（Word 标题不超三级、Excel 首行表头+冻结+筛选、PPT 一页一主题 3~5 要点、图表选型指引）；数据类需求先 py/js 加工再写入
- **限制**：旧版二进制格式 .doc/.xls/.ppt 不支持（提示在 Office/WPS 另存为 OOXML）；OOXML→PDF 转换（保真导出）仍经 sh 检测并调用 LibreOffice `soffice --headless --convert-to pdf`（宿主机存在时；无则 word_read 读回内容后 pdf_create 重建，版式不保真）；pdf_create 图片仅 png/jpg、自定义字体仅 TrueType 轮廓（OTF/CFF 不支持，见 pdf_create 条目）、加密 PDF 仅 pdf_read 可凭密码读（merge/split/edit 拒绝）；exceljs 不计算公式——读取时无缓存值回显公式原文
- **预加载**：`preload = false`，按需装载


#### `reel`（产品视频制作）

实现于 `packages/agents/src/agents/reel/`（`reel.ts` 定义入口 + `reel.md` 系统提示词 + `paths.ts` 路径与环境、`detect.ts` 主机与工程探测、`external.ts` 渲染外部件（浏览器可执行文件 / 原生二进制目录）解析与浏览器就绪判定、`profile.ts` 渲染档决策（纯函数）、`library.ts` 共享运行时、`runtime.ts` 原生库运行时、`jobs.ts` 作业与实测调优、`template-signature.ts` + `template.generated.ts` 内置模板签名与内联副本、`setup.ts`/`project.ts`/`render.ts`/`voice.ts` 四工具）：把产品（前端项目/网页/桌面应用）做成电影感视频（宣传片 / demo reel / 单镜头动效复刻）。

创作能力走**内化**路线：设计 token、18 个镜头原语、2.5D 相机、时间线骨架、配音与字幕接线与可直接渲染的示例片以真实工程文件内置，**零外部载荷**（不依赖任何上游技能库与下载源，配音也不依赖任何云服务——走本机系统语音栈）；渲染经「原生渲染库直连 + 共享 Remotion 运行时」地基。

- **内置资产与内联机制**：维护源为 `packages/agents/assets/reel-template/`（真实工程文件：`theme.ts` 设计 token、`ui.tsx` 18 个镜头原语（Backplate/Glow/Grain/Kicker/Rule/Headline/Wordmark/Caption/Subtitle/Mono/Panel/Crosshair/Grid/NodeFlow/DigitRoll/FlashCut/Roster/Show（时间窗容器））、`PageCam.tsx` 2.5D 相机 + `PageBox` 页面坐标系注记、`timeline.ts` 时间线（镜头窗口/文案/解说/音效钉帧唯一真相源）、`Film.tsx` 装配与 4 个示例镜头、`voice.generated.ts` 配音与字幕表、`sfx.generated.ts` 合成音效表（两份空表即无声无字幕，分别由 `reel_voice` 的 build / sfx 写入））。**落位时要写出真实文件，而 bundle 形态磁盘上没有 `assets/`**，故由 `packages/agents/scripts/embed-reel-template.ts` 把模板内联为 `template.generated.ts`（`TEMPLATE_FILES` 表 + 内容签名）；`template.test.ts` 比对磁盘维护源与内联副本的签名，**改模板后忘重跑生成脚本即测试失败**（消除双份真相漂移），并守住三条底线：工程骨架齐备、无绝对路径、模板源码无 `Math.random`/`Date.now`
- **共享运行时与解耦**：本子Agent 对内**不依赖任何其他子Agent 或外部库**（源码零交叉引用，创作能力全在包内）。运行时依赖按四步取用：① 本库根 runtime 已就绪且模板签名匹配 → 直接复用；② 依赖仍在但模板改版 → 重落位模板、沿用本机依赖；③ **通用复用**：扫描同实例 `{GEBAI_HOME}/vendor/<其他库根>/runtime`（或 `REEL_SHARED_RUNTIME` 显式指定）里**同版本**的 Remotion，落位模板后把其 `node_modules` 以目录联接接入（POSIX symlink / Windows junction，均免管理员）——按目录扫描而非硬编码某个子Agent 名字，有则省数百 MB，无则静默跳过；④ 自主安装：`npm ci`（有 lock）→ `npm install` → `bun install` 回退，无包管理器则明确报错；失败登记 `failed` + 错误尾部且不清理文件便于排查。即**移除任何其他子Agent 后本子Agent 仍可完整工作**（只是多花一次安装）
- **渲染档与实测**：硬件编码（NVENC 需 Remotion ≥ 4.0.484 / VideoToolbox，用 `videoBitrate`，硬件档不支持 crf）、Linux+NVIDIA 才用 `chrome-for-testing`、并发自动档取有效核数的一半（与官方 `min(8, cores/2)` 同口径；全片实测 c=2 与 c=4 无差异——取半档是为少占内存，**不是为提速**：单浏览器内加并发不突破截帧串行瓶颈），被拒时按报错上限**自愈重试一次**。**三个旋钮都是实测决定、不写死结论**：`bench` 用 `hardwareAcceleration=required` 强制探针、逐光栅化后端（gl）实测、逐并发候选实测，三组结论按 `profileKey(项目, 合成)` 写入本机调优缓存（按库根隔离）供后续渲染自动采用；gl 候选由 `defaultGlCandidates` 按主机形态给出（有 GPU 则测 `angle`/`vulkan`，无 GPU 测 `swangle`），**默认档即「交给 Chrome 自选」也作为一个候选参与实测**（后端收益与内容/驱动强相关：本机 1080p 合成上自选后端 26.9 fps、`angle` 38.1 fps、`swangle` 7.4 fps）。**浏览器按档位启动**（gl/chromeMode 均为启动参数，渲染时再传 `chromiumOptions` 对已启动的浏览器无效）——故先定档再准备浏览器：合成 ID 未知时先准备一次去列合成，定档后若 gl/chromeMode 变了则**再准备一次换对应档位的浏览器**（bundle 按签名缓存，第二次只多开一台浏览器）。**准备阶段拆成两个可分别限时的子阶段**（`runtime.ts` 的 `bundleProject` / `openSharedBrowser`，`prepareBundle` 为二者组合）：打包默认 300s、浏览器（含联网下载）默认 180s，超时即报出哪一步、浏览器现状与修复动作（`GEBAI_REEL_BUNDLE_TIMEOUT_MS` / `GEBAI_REEL_BROWSER_TIMEOUT_MS` 可放宽）；准备进度与阶段耗时写进 `state/prepare.log` 并由 `render action=status` 报出，渲染返回附「准备：打包 … · 浏览器 …ms（来源）」一行——准备发生在作业登记之前，没有这段轨迹时外部只看到「工具没反应」
- **分片并行渲染（`shards.ts` + `jobs.ts` 的 `runShardedRender`）**：成片/preview 可自动分片——帧段切 K 片、**每片一个独立 Chrome** 并行渲染无声视频段，再用 ffmpeg（concat 分离器 + `-c copy`）无损拼接、合回整段音轨。**分片不是万能提速：它只在单浏览器留有空闲 CPU 时才成立，而这必须实测、不能按核数推断**（容器往往只拿到宿主的一部分配额，`nproc` 与真实配额可差一倍）。故 `planShards` 以**上次整片实测的在用核数**为判据：`cpu-sampler.ts` 采本容器 cgroup `cpu.stat`（宿主全局 `/proc/stat` 读数只作参考、不参与判定），每次成片/preview 渲染后写回 `state/` 调优缓存的 `throughput` 记账，渲染输出附「性能：上次整片实测 … fps · CPU 在用 X/Y 核（配额已吃满/有余量）」一行；接近配额（≥`CPU_BOUND_RATIO` 0.7×核数）→ 单浏览器并如实说明「分片只会加剧争抢」；明显富余（≤`CPU_HEADROOM_RATIO` 0.5×核数）→ 按可用核数切；无实测记录时回落核数的保守推算；显式传 `shards` 仍优先，但与实测冲突时在理由里告警。实测两种真实形态（详见 `docs/reel-render-performance.md`）：20 核 + 独显机器上 6 片 ×2.14（1 片 37.2 → 6 片 72.0 fps）；**4 核配额容器（无 GPU）上 1/2/4/6 片 = 7.2/5.0/4.5/4.4 fps（加片反而慢）**——后者同时证明「CPU 只用三成」不能跨机器外推（该容器实测单浏览器就占 3.69/4 核且全程被节流）。`PAGES_PER_SHARD = 4`（片内页数实测 4 已饱和）、`MIN_FRAMES_PER_SHARD = 60`、`MAX_SHARDS = 6`、总帧数低于 180 不自动分片。**音轨不进分片**：每段 AAC 都带自己的编码器延迟，逐段拼接会在接缝留下数十毫秒静音或重叠——分片只出无声视频（`muted`），音轨整段单独渲一次后合轨，按构造正确。**失败一律回退整段渲染**（取消不触发回退），合轨失败则交出无声视频并如实说明；分片数与拼接/合轨结果都写进作业日志与工具输出。逐帧 PSNR 验证：同帧段分片 vs 整段 **mean 56 dB / worst 42.8 dB**（容器时长、帧数、音视频流完全一致）
- **单帧成本构成（实测，决定优化方向）**：1080p 下每帧 ≈ **95ms 管道开销 + 4ms 内容**——空场景（只有底色）95ms/帧 vs 真实场景 99ms/帧，即 React/滤镜/光斑等**内容渲染只占 4%**；主体是 CDP 截帧（1080p 66ms / 540p 33ms，含 ~33ms 固定项），canvas 直绘同画面仅 0.03–1ms。故**「优化画面素材/滤镜」不是提速手段**（已实测否证）；可用旋钮只有：降分辨率（540p 快 1.47×）、并发档（2 最优，实测 1/2/4 = 6.0/7.2/6.9 fps）、更多 CPU 配额/核数、GPU、或换掉截帧架构
- **多渲染通道（`backend`）**：`reel_render` 的 preview/video 可按 `backend` 选路（取值：调用参数 > `GEBAI_REEL_BACKEND` > 默认 `remotion`）。`remotion`（默认，DOM + CDP 截帧）行为不变、保真度最高；`dom-canvas` 与 `record` 由 `browser-render.ts` 自驱（见下）；`canvas` 留接口但需 canvas 版原语，暂未实现。**三个通道都能出声音**（音轨走 Remotion 音频通道再合轨，与通道无关）。
- **浏览器通道（`browser-render.ts`）**：自己启浏览器（`Bun.spawn`）并直连 CDP（Bun 内置 `WebSocket`），**零第三方依赖**——两条现成通道都走不通：Remotion 的浏览器参数是固定列表（拿不到 `--enable-blink-features=CanvasDrawElement`），而 playwright/puppeteer 无法进 server 包（`chromium-bidi` 解析不到，本仓库本就把 playwright-core 当内嵌资产）。帧驱动**复用 Remotion 自己的页面契约**（`remotion_setBundleMode({type:'composition'})` → `renderReady` → `remotion_setFrame(frame, 合成ID, 0)` → 再等 ready → `fonts.ready`），实测该握手的产物与官方渲染器**逐像素一致（diff=0）**。`dom-canvas`：`drawElementImage` 把舞台抓进 canvas（实测 3.2ms/帧）→ WebCodecs H.264（`avc1.640028`，实测 22.8ms/帧）→ 码流边产边回流 → ffmpeg 封 mp4；同帧段实测 **10.3 fps vs remotion 5.9 fps（1.75×）**（该场景含 blur 光晕，抓帧升到 80ms/帧）。`record`：`captureStream` + `MediaRecorder` 实时录制，直出 mp4（用于交互/实时内容；**不可能快于实时**、时序跟墙钟且不可复现）。**均已实测须知的坑**：① 不先切渲染模式，`setFrame` 会**静默失效**；② `serializedResolvedPropsWithSchema` 必须是 JSON 字符串；③ `drawElementImage` 要求目标元素是 canvas **直接子节点**，且须**先移入元素、最后才把 canvas 插入文档**（反过来报 `No cached paint record`）；④ bundle 页面**没有 `#root`**，舞台在 `#video-container`（抓 `<script>` 必报同样的错）；⑤ 内置 compositor 的 ffmpeg 是精简构建（无 psnr/ssim 滤镜、无 rawvideo 封装）；⑥ **从未绘制过的 canvas 不产帧**——record 也必须先 `drawElementImage` 把画面画进 canvas，否则 MediaRecorder 拿到空产物。`record` 的实测边界：抓帧本身要 60–80ms/帧，故 30 帧录出 **1.905s / 60fps 元数据**（应为 1.0s）——有效帧率低于合成 fps 时产物会拉长（慢放），工具会在日志里如实报出并建议改用其他后端。**dom-canvas 的已知偏差（已归因）**：与 remotion 同帧产物 meanDiff ≈ 7.7/255，**结构完全一致（亮度相关系数 0.9948）、但暗部系统性偏暗**（暗部区平均差 -10.3、亮部区 -1.7，99.8% 的像素都是 remotion 更亮；例：CSS 背景 `rgb(11,13,18)` 经 `drawElementImage` 后变成 `(8,10,14)`）。已排除：编码损失（remotion 自身 H.264 vs 官方 PNG 仅 0.638）、alpha 合成（画布输出 alpha=255）、canvas 预铺底色、`--force-color-profile=srgb`。**归属 layout-subtree 合成路径本身**，故 dom-canvas 定位为草稿/预览通道，交付前必须抽帧目视复核。**音轨**：浏览器通道**默认就带声音**——声音走 Remotion 自己的音频通道（`renderMedia({codec:'aac'})` 能**单独出 AAC**，不跑视频截帧），与画面同帧段，再由 ffmpeg `-c copy` 合入（`-map 0:v:0 -map 1:a:0`）。两个实测要点：① **合轨不能用 `-shortest`**——在「裸流 H.264 输入（`-f h264`）+ `-c copy`」组合下它会把音轨整条丢掉（exit 0 但产物只剩视频流），改用按帧数算出的精确时长 `-t <帧数/fps>`；② **无音频标签的合成会得到一条高码率静音轨**（实测 0.68s 就 29KB，按字节大小完全看不出无声），故合轨前用 `audioPeak`（降为 PCM 取峰值）实测一次：峰值恒为 0 即报「合成内没有音频内容」并保持无声，**不把静音当有声报出去**。另外，自驱帧页面必须**在加载前注入渲染环境**（`remotion_puppeteerTimeout` + `process.env.NODE_ENV='production'`，与 Remotion 自己的 `setPropsAndEnv` 一致）——`<Audio>` 只在 `environment.isRendering` 为真时才注册音轨，缺了它组件会静默走预览分支（不报错、不注册、也不请求音频文件），且浏览器需先 `about:blank` 再导航才能在文档创建前注入。
- **外部件可配置（离线/内网关键路径）**：浏览器可执行文件与原生二进制目录各一条配置通道，取值优先级统一为**调用参数 > 环境变量 > `.reel.json` 同名字段**（相对路径按工程目录解析；配置了但不存在/不完整即报错，**不静默回落下载**）——`browserExecutable`（`render` 的 `chrome_executable` 参数 / `GEBAI_REEL_CHROME_EXECUTABLE`）完全绕开 Remotion 的缓存与联网下载；`binariesDirectory`（`render` 的 `binaries_directory` 参数 / `GEBAI_REEL_BINARIES_DIR`）整体替换 compositor 与 ffmpeg，**目录内须含 `remotion`/`ffmpeg`/`ffprobe` 三件套**（缺一即报错——只给一个 ffmpeg 会让 compositor 查找失败，这是默认不传该选项的原因），用于换用带硬件编码器的 ffmpeg 构建。浏览器就绪判定（`external.ts`）复刻 Remotion 的目录与 `VERSION` 版本规则，按**将要使用的 Chrome 形态**在**多候选缓存根**（进程 cwd 的 Remotion 规则根 > 工程目录 > 共享运行时）里判定，得出「配置的可执行文件 / 本地缓存可用 / 将在渲染时联网下载」三种结论，由 `setup` 与 `project status` 在开工前报出、`render` 每次发起时附一行。**只要找到可执行文件（含 VERSION 与当前 Remotion 期望不一致）就显式指定给 Remotion**——交给它自行判定时，版本不一致的缓存会被**先删掉再联网下载**（内网等于自毁一份可用浏览器），故缓存继承优先于下载、版本差异如实报出而非静默失败；只有本机真的没有可执行文件时才落到「渲染时联网下载」
- **工程入口点（`.reel.json` 的 `entryPoint`）**：`init` 写**相对路径**（可随工程移动）；读取时**凡指向工程目录之外的绝对入口一律不信**，改走本工程入口并由 `render` 打出一行告警（`declaredEntryOutsideProject`）。缘由：工程被复制/移动后，清单里留下的绝对路径仍指向原目录的**另一个工程的源码**，捆出的 bundle 与该工程内容不符——产物看着正常却是别的片，且渲染**不报任何错**（实测踩过：复制工程做实验时连续多轮都在渲另一个工程的画面，还一度把它归因成“音频失效”）。（进程 cwd 向上最近 `package.json` 的 `node_modules/.remotion`），查找为**多候选根**：Remotion 规则根优先，其次工程目录与共享运行时（Remotion 自身不查这两处，但本机已有即可直接继承——渲染总是把命中的可执行文件显式交给原生库），由 setup/status 如实报告目录与体积
- **输出尺寸与画质档（`output.ts`）**：视频尺寸用 `height=<目标高>`（按合成长宽比换算）或 `scale=<比例>`，二选一；换算结果必须**整数且偶数**（h264 要求）——不合法时在工具入口即报错并**按当前合成列出可用档位**（`feasibleSizes`），不把问题留到编码阶段（自由浮点 scale 写 0.667 会得到 1281×720 而停在 ffmpeg 报错里，与「尺寸没算对」离得很远）。`quality=draft` 为确认动效与节奏的快速档：更低分辨率 + `ultrafast` 编码 + 帧图质量 70；**静帧不降**（抽帧是判读细节用的）。**分辨率那一项按通道判定**（`resolveOutputScale`）——实测同帧段 120 帧：remotion 通道 540p 比 1080p 快 **1.47×**（CDP 截帧成本随像素线性），而浏览器通道只有 **1.04×**（抓帧仅 3.2ms/帧，属噪声内），故草稿档在浏览器通道保持全分辨率、不做白丢画质的降采样。`x264_preset` 可调编码速度档（默认 Remotion 内置 medium）。依据（4 核 · 无 GPU · 软件编码 · 热态实测）：全片 1080p final 42s、1080p+ultrafast 38s、540p draft 35s、360p draft 32s；节级草稿 120/230 帧分别 5s / 10s；单张抽帧约 0.4s（这批数字出自 20 核机器上的首轮实测，其中“540p 只快 17%”的结论在 4 核机上不成立，已按上面的通道实测更正）。草稿档的价值在**把返工限制在一节内**，故首选**做小**（节级 / 抽帧）而不是整片低清。另：**开头几十帧的耗时不可外推到整片**（元素密集的开场镜头与首个作业的固定开销都会高估——以 60 帧样本外推约高估一倍）
- **提示词纪律（内化自上游方法论）**：⓪ **默认「确认式创作」——三处确认点（概要设计 / 每节主帧 / 成片关键帧）把控制权交回用户**，目标是**一次出片**：返工消灭在纸面与单镜，而不是全片渲完再改（全自主模式需用户明确表态；送审纪律含先自检后送审、一节一送过审再进下一节、拒绝/沉默时不擅自做方向类变更并标注未确认、裁决逐条记入 SPEC）① 复刻既有页面必须真实截图（2–4 倍纹理 + 数据脱敏冻结）② 视觉从产品自身生长（改 `theme.ts` 换整片皮肤，禁另造“宣传片皮肤”）③ 电影感来自运镜/光影/节奏/声音而非炫技 ④ 每镜一个动效 + 落定后呼吸（字标 hold ≥1s、批量收尾 0.5s、手法全片只当一次主角）⑤ 强节奏 BGM 必须卡拍且**卡拍管时机不管幅度**（整画面冲击全片 ≤3 处）⑥ 用原语先读源码、命门参数不得降档 ⑦ 方向判断前置（简报→决策表→视觉方向→镜头映射→分镜）⑧ 验收贯穿全程 + 交付前独立终检（带帧号证据）⑨ 确定性渲染（禁 `Math.random`/`Date.now`，用 `theme.ts` 的固定种子伪随机）⑩ **开工前必检（离线/内网）**：`setup` 的浏览器行显示「将联网下载」就先配 `chrome_executable` 再渲；渲染卡住/超时按「`status` 看准备阶段 → `log` 看作业 → 显式指定浏览器重试 → CLI 兜底」处置，**禁杀 Chrome/headless-shell 进程**（破坏进程内热浏览器复用，且不解决准备阶段的问题）⑪ **「全面」类需求先穷举来源文档的能力章节（≥8 条）再映射取舍**，砍项写明理由 ⑫ **解说与镜头同源**：分镜前用 `reel_voice estimate` 定镜头窗口，配音定稿后按**实测时长**重排 `SHOTS`/`TOTAL`（画面等解说的长度，不是解说追画面）；有配音的句子不再写进 `CAPTIONS`（叠字）
- **配音与字幕（`voice.ts`，全本地）**：`reel_voice` 把解说词变成成片里的声音与字幕；**复用 `core/tts` 基建不重写引擎**（脚本、SSML 构造、结果解析、失败分类、`concatWav`、`escapeXml` 与 `TtsDeps` 注入形态与 tts 子Agent/REST 朗读同一份实现）——仅新增视频侧那一层（帧号排布、WAV 头实测、SRT、生成模块）。零联网、零云服务、零安装
  - **实测时长是确定量**：逐句合成 WAV 到工程 `public/audio/voice/<name>-NN.wav`，时长**读 WAV 头得出**（`readWavLayout`/`wavDurationSec`；流式 WAV 的长度字段不可信（0/越界）时按实际字节数算）——引擎自报值只作兜底，两者都没有则报错而非当成 0 秒
  - **帧号是唯一坐标**（默认 30fps，与 `timeline.FPS` 同坐标系）：一份帧号三条出口——① `src/film/voice.generated.ts`（`VOICEOVER` 音频钉帧表 + `SUBTITLES` 字幕表，`Film.tsx` 已接线：`<Audio>` 与新增 `Subtitle` 原语逐条挂 `<Sequence>`；模板内空表，示例片照旧可直接渲染）；② 交付字幕 `out/subtitles/<name>.srt`（由帧换算的时间码，与成片同帧）；③ 分段 JSON（改文案后 `action=srt` 免重合成重出字幕）
  - **字幕不依赖配音**：某条给 `durationMs` 即纯字幕段（不合成音频、不进 `VOICEOVER`），一条语音也没有时字幕照烧入；非 Windows 平台（无内置离线引擎）因此仍可用字幕，**有语音段则明确拒绝并给出「改用 durationMs」的补救**，不静默降级、不回落联网服务。逐条可覆盖 `voice`（多角色）/`at`（绝对帧钉镜头）/`gapMs`（句间隙）；`gain` 写进 `VOICEOVER.volume`
  - **动作**：`estimate`（只估时长不落盘——中文按字数、西文按词、标点计停顿，语速参数按比例缩放，标注 ±20%，供分镜定窗口）/`build`（默认：合成 + 出全套产物）/`srt`/`voices`/`sfx`。整段旁白预览轨（`out/audio/<name>-voiceover.wav`）按排好的帧号补静音拼接，**格式不一致（多引擎/多音色混放）或顺序被打乱时放弃拼接**，而不是硬拼一条失真音频
  - **音效合成（`action=sfx`，与配音互不依赖）**：直接用 `core/tts/audio.ts` 的波形合成内核（零素材、纯本地、不依赖系统语音引擎——非 Windows 平台同样可用）；`sfx` 数组逐条给预设名（`riser`/`impact`/`sparkle`/`whoosh`/`explosion`/`end` 等，也可给中文关键词；不传 `sfx` 即列出全部预设）或自定义单音（`wave`/`freq`/`freqTo` 滑频/`duration`/`decay`/`attack`/`release`/`gain`），放置字段 `at`（**绝对帧钉画面动作**）/`gapMs`/`volume`（成片播放音量 0~2，写进 `SFX_TRACKS.volume`）/`windowFrames`/`repeat`+`repeatGapMs`/`note`。产物：`public/audio/sfx/<name>-NN.wav` + `src/film/sfx.generated.ts`（`SFX_TRACKS`，模板内空表、`Film.tsx` 已接线，与手工登记的 `timeline.SFX` **并行生效**），结果里直接附回音频可听。两个设计点：① 播放窗默认按音频时长**向上取整**（窗口短于音频会被 `Sequence` 截断声音，故不向下取），用户显式给 `windowFrames` 且短于音频时**如实告警**而非静默交付半声；② 排布缺省空隙为 0（音效本就该贴在动作上，与配音的 300ms 句隙是两个语义）
  - **音效预设库补 `impact`（低频冲击）与 `sparkle`（高频碎点）**：视频收尾的固定句式是 `riser→impact→sparkle`，原库只有 `riser`——补齐后该句式三条一次可出（三件套齐备已由 `audio.test.ts` 钉住）
  - **覆盖语义与失败边界**：WAV（成片按固定路径取音，只能覆盖）与数据模块覆盖写；字幕文件与预览轨同名自动追加 `-v2`（历史版本可回看）；逐条合成中途失败即**报出条号与已完成进度**，不把半个工程当成功；产物名白名单校验（它进文件路径）
- **工具集（四工具 17 动作）**：`setup`（环境与渲染档探测 + 库根/运行时状态 + 按将用的 Chrome 形态报浏览器就绪；可选立即准备依赖）；`project`（`init`（落位模板 + 依赖联接 + 写 `.reel.json` 记录入口点/模板签名；重复 init 保留外部件配置）/`install`（准备或强制重装依赖并重建联接）/`status`（含浏览器就绪与外部件配置））；`render`（`still`/`preview`/`video`/`bench`/`status`/`log`/`stop`；入参含 `height`/`scale`/`quality`/`x264_preset`/`chrome_executable`/`binaries_directory`，`out` 相对路径以工程目录为基准）。**送审通道**：`wait=true` 同步等作业完成并把产物直接附进工具结果——`still` 附帧图（内容块→UI 用户可见 + 多模态图片→模型自查），`preview`/`video` 附视频文件（用户可在对话里查看）；默认不传 `wait` 仍是后台作业（立即返回作业 ID，适合批量 QA，批量帧可由 `js` 编排、产物块随 js 结果一并展示）；等待超时（静帧/片段 180s、成片 600s）返回当前作业状态与日志入口，不静默失败；`status` 无作业时同时报出**准备阶段**轨迹（阶段/进度/最近日志）；`voice`（`build`/`sfx`/`estimate`/`srt`/`voices`：本地离线配音、字幕与音效——逐句合成 WAV 到工程 `public/audio/voice/`、写 `src/film/voice.generated.ts`（配音音轨表 + 字幕表）、交付 `out/subtitles/*.srt` 与分段 JSON、额外出整段旁白预览轨；`sfx` 用波形合成内核出音效 WAV 到 `public/audio/sfx/` 并写 `src/film/sfx.generated.ts`（不依赖语音引擎）；`estimate` 不落盘只估时）
- **审批**：`setup`、`project init`/`install`、`render` 全部动作需审批（`voice` 免审：本机离线合成、只写自己的产物，与 `tts` 子Agent 同姿态）
- **环境变量**：`REEL_LIBRARY_DIR`（库根，默认 `{GEBAI_HOME}/vendor/reel`：`runtime/` 共享运行时、`state/` 调优与作业）、`REEL_SHARED_RUNTIME`（显式指定可复用的运行时目录；缺省扫描 `{GEBAI_HOME}/vendor/<其他库根>/runtime` 找同版本）、`REEL_PROJECT`（默认工程根，经 def 的 `projectRoot` 绑定）、`REEL_GPU`（`auto`/`off` 强制软件档）、`GEBAI_REEL_CHROME_EXECUTABLE`（浏览器可执行文件，绕开缓存与下载）、`GEBAI_REEL_BINARIES_DIR`（含 remotion/ffmpeg/ffprobe 的目录，替换内置 compositor 与 ffmpeg）、`GEBAI_REEL_BROWSER_TIMEOUT_MS` / `GEBAI_REEL_BUNDLE_TIMEOUT_MS`（准备阶段两个子阶段的时限，毫秒，默认 180000 / 300000）、`TTS_VOICE`（配音默认音色，`reel_voice` 未传 `voice` 时使用）、`TTS_ENGINE`（配音语音引擎：auto/winrt/sapi——与 tts 子Agent 同名同义）——经 def 的 `envVars` 汇总进前端环境变量面板白名单
- **预加载**：`preload = false`，按需装载

#### `tts`（语音合成）

实现于 `packages/agents/src/agents/tts/`（@gebai/agents 包；`tts.ts` 定义与语音工具 + `sfx.ts` 音效与音频处理工具 + `tts.md` 系统提示词 + `tts.test.ts`/`sfx.test.ts` 用例）与 `packages/agents/src/core/tts/`（基建域：`speech.ts` 系统语音引擎、`audio.ts` 音效合成与音频处理、`vocoder.ts` 相位声码器），**纯本机离线**（不联网、零第三方依赖、零安装——语音走系统语音栈，音效与后处理是纯 PCM 计算）。

- **引擎链**：Windows 两个系统语音栈——WinRT OneCore（`Windows.Media.SpeechSynthesis`，Windows 10+ 标准，质量优于 SAPI5）优先，SAPI5（`System.Speech`）回退；`engine` 参数可显式指定（auto/winrt/sapi）。非 Windows 平台当前没有等价的内置离线引擎，工具如实报错并指明本能力**不做联网合成**（不回落在线服务），系统提示词同步约束（不要去在线 API / 下载语音模型）
- **实现要点（三处实机踩坑）**：① 脚本经 PowerShell 5.1 的 `-EncodedCommand`（UTF-16LE base64）调起——PowerShell 按系统 ANSI 代码页解码 `.ps1` 文件，无 BOM 的中文脚本会解析失败，编码进命令行彻底规避且不落脚本文件；② 待合成文本与运行结果都走 UTF-8 文件（命令行与单个环境变量都有长度上限与转义负担，PowerShell 的 stderr 是 CLIXML、stdout 编码随宿主漂移——文件是唯一稳定接口）；③ SSML **必须带 `xml:lang`**（缺失时 WinRT 直接报错），故语言随所选音色在脚本内确定；文本的 `& < >` 与换行在 TS 侧完成 XML 转义（脚本只做拼接）。时长经脚本解析 WAV 头得出（不把整段音频读回内存）
- **工具集（五工具）**：`speak`（text 必填 + voice/rate/pitch/volume/out/engine/play；产物 WAV 落会话 `tmp/tts/voice-<时间戳>.wav`（`out` 可指定），结果附 file 块——聊天内原生播放器直接播放、可直接交付；单次文本上限 4000 字符，超限提示拆分而非截断；参数越界钳制，语速极端时提示试听确认）；`voices`（find/limit：列出本机音色（名称/语言/性别）供 `speak` 的 voice 取值）；`sfx`（音效合成：`preset` 预设名或中文关键词（`list=true` 列出 27 个预设）/ 自定义 `wave`+`freq`（可 `freqTo` 滑频）+`duration`+`decay` 等；`repeat`+`repeatGapMs` 连响；`volume`/`sampleRate`/`out`）；`effect`（音频效果：`input` 必填 + `tempo`/`pitch`/`speed`/`gainDb`/`normalize`/`fadeIn`/`fadeOut`/`reverse`/`trimStart`/`trimEnd`/`echoDelayMs`(+`echoFeedback`/`echoMix`)/`reverbMix`(+`reverbSize`)/`lowpass`/`highpass`/`robot`）；`mix`（`tracks` 数组（`path` + `delayMs`/`gain`/`loop`）+ `mode` mix|sequence + `gapMs`/`durationSec`/`sampleRate`）
- **引擎为基建、两条链路共用**：合成引擎在 `packages/agents/src/core/tts/speech.ts`（基建域）——脚本、SSML 构造、结果解析、失败分类、分片（`splitText`）与 WAV 拼接（`concatWav`）同一份实现；执行通道（runCommand/文件读写）经 `TtsDeps` 注入，故子Agent 工具（会话沙箱内、产物落会话）与 REST 朗读接口（进程级子进程、产物不落盘）共用而不复制
- **助手回复朗读（Web）**：`POST /api/v1/tts` + 前端消息操作按钮组内的朗读按钮——助手回复一点即听，不必先合成再播放；合成服务不落会话产物（朗读是「听一下」，不该在会话文件里堆积音频），并补子Agent 不需要的两件事：**长文本分片拼接**（助手回复常超过单次合成上限，按句边界分片后拼 WAV，格式不一致则如实报错而非交付残缺音频）与**进程内缓存**（同文同参重复点击不再调系统引擎，按字节上限淘汰最旧）；平台无内置离线引擎时返回 503 并说明不做联网合成，前端据此不渲染死按钮
- **本机扬声器播报（`play`）**：`play=true` 时除落盘外把产物送到**运行 GEBAI 这台机器**的默认音频设备——经 `Start-Process` 派生独立的隐藏播放进程（`SoundPlayer.PlaySync`）后立即返回，播报不阻塞工具返回（长文本可播十几分钟），也不会因父进程退出而中断；**仅本地模式提供**（服务端部署下播报的是服务器机器的音频设备，会干扰同机其他用户，故拒绝并写明原因——产物照常落盘，可下载后自行播放）；播报失败不影响合成结果交付，原因进注意项
- **音色匹配与回落**：引擎按「精确名 → 名称包含（如「Kangkang」→「Microsoft Kangkang」）→ 中文优先默认音色」三级选择；第三级回落时在结果里写明实际音色（**不静默换声**），关键词命中不算回落
- **失败分类**：无引擎（指定 winrt 时不回退 SAPI vs 两者皆无）/ 无音色 / 超时（120 秒，与用户取消区分）/ 非零退出（CLIXML 噪音清洗后透出根因）/ 未产出音频——各自给出可行动的下一步指引
- **音效与音频处理（`sfx`/`effect`/`mix`，内核 `core/tts/audio.ts` + `vocoder.ts`）**：三条能力都是**纯 TS 计算**（不依赖系统语音引擎，非 Windows 平台同样可用）——① 合成：波形（正弦/方波/三角/锯齿/噪声）× 频率（可滑频）× 包络（起振/收尾/指数衰减）组合，内置 27 个预设（提示/交互/氛围/系统音四类，含视频收尾句式所需的 `riser`/`impact`/`sparkle`），也支持自定义单音与连响；噪声层用固定种子伪随机（同参数输出逐样本可复现），其 `freq` 为该层低通截止；② 效果：**变速不变调**（`tempo`，相位声码器——`vocoder.ts` 的 STFT + 相位推进 + 相位锁定，时长随之变化而音高保持；实测约实时 160 倍（16kHz 语音 217 秒用时 1.3 秒），真正约束是产物缓冲内存，故按**产物时长**限 600 秒、放慢倍率相应收紧输入上限，超限提示先裁剪或分段）、变调（OLA 时间伸缩 + 重采样，时长不变）、变速（`speed` 重采样，音高随时长一起变）、回声、混响（四路梳状简化模型）、低通/高通（RBJ biquad）、环形调制、淡入淡出、反转、增益/归一化/裁剪，按**固定顺序**施加（裁剪→反转→变调→变速不变调→变速→滤波→调制→回声→混响→淡入淡出→增益→归一化）使同参数结果可复现；③ 组合：多轨混音（轨级延迟/增益/循环铺底——总长以非循环轨为基准，`durationSec` 只用于把铺底垫得更长、不截断内容）与顺序拼接（段间静音），采样率自动统一到最高轨；输入只接受未压缩 WAV（PCM 8/16/24/32 与 float 32/64，其余格式如实说明需先转 WAV），输出统一 16bit 单声道 WAV；叠加超峰自动等比限制而非削波（不静默交付失直音频），输入时长上限 30 分钟、混音轨数上限 16
- **相位声码器为独立数值模块**：`packages/agents/src/core/tts/vocoder.ts` 只用内置类型（不依赖项目内其它模块）——radix-2 FFT（旋转因子按帧长缓存复用）、帧长按采样率取约 46 毫秒的 2 的幂（钳制 512~4096）、分析 hop = 帧长/4、合成 hop 按速度倍率缩放；**相位锁定**（Laroche & Dolson：只有谱峰累积相位、其余频点跟随所在区域的峰的相对相位）抑制纯相位声码器典型的「相位散乱」噪声；瞬态（爆破音、鼓点）仍有涂抹——这是相位声码器的固有限制，非实现缺陷
- **审批**：无（与 `wps` 同级——产物落会话内，属于生成本机文件）；安全模式不提供（`safeMode: false`：写文件并发起脚本子进程）
- **环境变量**：`TTS_VOICE`（默认音色，可写完整名或关键词）、`TTS_ENGINE`（默认引擎 auto/winrt/sapi）——经 def 的 `envVars` 汇总进前端环境变量面板白名单
- **预加载**：`preload = false`，按需装载

#### `nsight`（NVIDIA Nsight 报告分析与 GPU 性能问题定位）

实现于 `packages/agents/src/agents/nsight/`（`nsight.ts` 定义入口 + `nsight.md` 系统提示词 + 分析/解析/定位分层模块 + 五个测试文件：诊断规则、多卡与开销口径、原生等价性、报告与环境解析、合成事件库聚合），面向「报告 → 问题 → 代码」的完整链路：解析 Nsight Systems（`.nsys-rep`/`.qdstrm`，时间线：GPU 活动、内核、显存传输、CUDA API/同步、NVTX）与 Nsight Compute（`.ncu-rep`，单内核硬件计数器）报告，产出量化证据 + 根因 + 修复方向的问题清单，并把报告符号映射到工程源码 `文件:行`；无现成报告时可对目标程序做 nsys/ncu 采集。

- **工具集（12 个，含原生聚合后端）**：`doctor`（工具链/GPU/计数器权限/缓存自检）、`reports`（报告索引 list·info·import）、`overview`（利用率·紧凑时间线·**每卡分解**·**采集开销**·**CUDA Graph**·热点·传输/API/同步/流）、`kernels`（排行与单内核下钻）、`timeline`（空闲缝含前后邻接活动·启动间隔·流并行度）、`query`（事件库只读 SQL：schema/run——标准报告未覆盖的维度精确取数）、`findings`（诊断清单，可选自动定位、**可选导出 Markdown**）、`compare`（**两份报告改前改后对比**：度量与问题清单差异）、`kernel_detail`（ncu 单内核深查；传 `.nsys-rep` 时降级为调用模式分析并说明差异）、`locate`（符号→源码）、`capture`（采集）、`aggregate`（客卿 Rust 边车贡献的原生聚合后端，分析类工具自动选用；一般无需直接调用）。`overview`/`timeline`/`findings` 另支持 **`time_from_ms`/`time_to_ms` 时间窗**（相对报告首个活动，与活动区间相交即命中，下推到 SQL）。**审批：仅 `capture`**（执行被分析程序）需审批，其余只读免审批；报告路径类工具带 `project` 参数（预置项目/路径/保留名 `tmp`），`locate` 的搜索范围即该项目根。
- **报告解析与缓存**：nsys 经 `nsys export --type sqlite` 落事件库（Bun 内置 `bun:sqlite` 只读打开）；ncu 经 `ncu --import --page <页> --csv` 落指标页（details/raw/source，流式解析）。缓存根 `{GEBAI_HOME}/cache/nsight/{报告名}-{大小}-{mtime}/`（内容指纹寻址——报告重新采集自动换目录，不误用旧数据）；**导出的事件库默认无索引**，故导入后对超过 20 万行的事件表按查询模式建**覆盖索引**（热扫描用到的列全部纳入：普通索引每行仍需回表随机读）——实测 1000 万行事件库下同样的逐行扫描由 18.9s 降至 11.2s。**扫描的活动表**：内核、显存传输、**显存 memset**（三者按首时间戳归并，缺表即该维度为空而不中断分析）、**`deviceId` 列**（多卡分流；旧版导出缺该列时按单卡处理）。
- **超大报告（GB 级、千万级事件）的实时分析**，三条纪律（内存与报告规模解耦）：① **不物化全量事件**——游标流式消费，内核与显存传输两路各自走索引归并（避免 `UNION` 的全局排序与临时落盘）；② **结果有界**——排行用 Top-K、分位数用**固定种子的受控采样**（超出容量转蓄水池抽样并标注估计；固定种子使同一报告重复分析得到同一组分位数，与原生边车的固定种子同语义）、时间线用自适应分辨率分桶（跨度增长只倍粗分辨率，桶数恒定，故无需第二遍扫描）、空闲缝上限截断；③ **一次扫描多次复用**——聚合事实按「报告指纹 + 阈值」进程级缓存，概览/热点/诊断/定位共享同一份扫描结果（首次调用付扫描成本、后续命中秒回，工具如实回报耗时、聚合来源与缓存命中情况）；API/同步/NVTX 等维度用 `GROUP BY … ORDER BY … LIMIT` 下推 SQLite，只把有界结果带回 JS。
- **原生聚合后端（客卿 Rust 边车，可选加速路径）**：`keqing/rust/nsight/`（cargo bin crate，依赖语言目录共享框架 + rusqlite（bundled 特性内嵌 SQLite —— 不依赖系统 sqlite3））以同名子Agent 跨语言合并贡献一个 `nsight_aggregate` 工具，把**同一套单趟聚合**在原生执行（游标流式、固定容量聚合器、分组下标记忆化、逐行只写 Copy 类型、JSON 只在输出阶段构造一次）。TS 侧 `resolveTimelineFacts` **原生优先、自动回退**：边车不可用（未构建/服务端部署下客卿整体禁用）或调用出错/返回值不合约即回退 JS 流式实现，回退原因随工具输出如实回报（不静默降级）；`NSIGHT_NATIVE=off` 可显式关闭原生通道。两条实现的**输出同构由测试锁定**（同一合成事件库逐字段比对规模/耗时/并集/空闲缝/并发/传输/分组）。
- **实测（千万级事件，本机 RTX 4080 SUPER 宿主）**：事件库 1000 万内核 + 20 万传输（含覆盖索引，1038 MB）下，JS 流式聚合 16.2s、原生边车 11.5s（**1.4×**），峰值内存 RSS 143 MB → 83 MB（1.7×）；结果核心指标逐字段一致。**与语言无关的真实瓶颈**：扫描循环内采样计时显示耗时几乎全在「行读取」（≈ 1 μs/行 × 15 列，两种实现都在付 SQLite 行解码这笔成本），聚合逻辑本身只占零头；且实测 SQL 侧聚合并不更快（`GROUP BY` 多聚合 9.3s、窗口函数求并集 12.4s/最大并发 19.4s，均于 1000 万行），故**进一步提速的杠杆是减少需解码的列/行（窄列扫描：15 列 8.7s → 2 列 2.0s，4.3×）而非再换语言**。
- **导入预算与后台衔接**：单次工具调用受引擎时间上限约束，导出超出预算时返回「导入进行中」+ 可直接后台执行的完整命令（nsys 不支持续传，不重复启动导出）；下次调用以「SQLite 可打开且含事件表」判定产物是否可用——用户/后台在同路径自行导出的产物同样被复用。
- **诊断规则**：时间线侧——GPU 空闲占比、同步阻塞（含阻塞型 API 归因）、热点内核集中、小内核启动开销、网格规模不足、传输包偏小、单流无并发、寄存器/共享内存占用压力；阈值集中定义在 `findings.ts`（`FINDING_THRESHOLDS`）便于复核。ncu 侧——NVIDIA 官方规则（含预估收益，按收益分级）、SOL 瓶颈单元（带宽/计算/缓存谁饱和）、占用率上限与**限制因素**（寄存器/共享内存/块数）、停顿主因分解（源采样汇总）、访存效率（越界扇区=非合并访问、共享内存 bank 冲突）、分支发散。
- **采集开销与 CUDA Graph 维度**：读 `PROFILER_OVERHEAD`（采集插桩给被测程序引入的开销）与 `CUPTI_ACTIVITY_KIND_GRAPH`/`GRAPH_NODE`。开销**按活动窗口求交**：只有落在窗口内的部分才构成对被测区间的扰动，启动阶段（进程初始化，实测最长可达窗口前 3.1s）与退出 flush 单列并注明不影响窗口内结论——实测某报告 39 个开销点全部在窗口之外，若按总数除以窗口会得出误导性的 82.3%（窗口内实为 0.4%）。Graph 维度未采集时**不判定为「没有用图」**，而是如实列入未分析维度并给出开启参数（`--cuda-graph-trace=node`）；图占比高时提示「图内部依赖不在逐内核视图里」。
- **多卡正确性**：GPU 活动按 `deviceId` 分流——`TimelineFacts.devices[]` 给出每张卡的忙碌/利用率/卡内空闲缝/最大并发/占用序列；顶层字段保持「任一卡在忙」的合并口径（单卡行为不变）。**合并口径只回答「机器有活干吗」，回答「哪张卡被困住」必须看每卡分解**——实测构造场景：卡 0 忙 900µs、卡 1 只忙 100µs 时合并利用率 100%，而卡 1 实际闲置 89%。诊断据此新增 `device-imbalance` 规则（卡间利用率差超 `FINDING_THRESHOLDS.deviceUtilSpread` 即单独指出并给出每卡证据）。JS 与原生边车两侧同实现，等价性测试逐字段比对 `devices`。
- **报告间对比**（`compare` 工具 + `core/perf/compare.ts`）：两份报告（改前改后/两次采集）的度量按同名对齐、问题清单按 `id` 对齐，输出改善/退化/新增/消失/代价变化。三条防误导规则：**单位不同不比数值**（`3.9 MB` 与 `679.0 KB` 按裸数字比大小会得出相反结论）、**相对变化低于显著性阈值记「持平（阈值内）」**（`DEFAULT_SIGNIFICANCE_PCT`=1%，避免噪声级差异淹没真实变化）、**标记只说好坏、百分比只说变化量**（早期用 ↑/↓ 表示数值升降与好坏混排，读起来自相矛盾如「↑ 退化（-75.9%）」）。净变化按两侧各自全量求和——只算共有问题会看不见「修好的问题省了多少」。
- **结果导出**（`core/perf/export.ts`）：`findings` 支持 `save`/`save_dir` 落 Markdown（含证据、根因、建议与源码定位），路径按 `project` 根解析、文件名带时间戳（重跑不覆盖上一版）；导出失败不影响分析结果（以提示形式附在输出末尾）。
- **证据纪律**（系统提示词硬约束）：时间线证据（何时忙/闲、谁等谁、调用模式）与硬件计数器证据（SOL/占用率/stall）严格区分，不用其一断言另一维度；抽样与截断如实标注；报告未采集的维度明确列出并给出补采参数（NVTX、源码关联 `--import-source` 等）。
- **符号→源码定位**：报告符号以 **demangled 名**入清单（mangled 名不可作源码搜索词，命中该形态时给出改用 demangled 的提示）；归一为函数基名（去模板参数/参数列表/命名空间），搜索内核定义（`__global__`/`__device__`）、启动点（`<<<>>>`）、NVTX 打点与名称引用，按「定义 → 启动点 → 打点 → 一般引用」排序输出 `文件:行` 与上下文。**词边界匹配 + 通用标识符停用表**（避免 `at` 命中 `path` 一类噪声）；NVTX 名支持 `StringIds.textId` 与 `NVTX_EVENTS.text` 两种存储形态（torch 等运行时用后者）——框架工作负载据此落到用户代码行。扫描设文件数/单文件大小/总字节上限，超限与超大文件跳过均标记为「扫描不完整」；符号来自预编译库（cuBLAS/cuDNN/PyTorch 内置算子）时明确提示优化点在上层调用方式而非内核源码。
- **采集与权限**：`capture` 构造 nsys（`--trace` 项前置校验，Windows 默认关闭 cuda-event-trace 以减小开销）/ncu（`--set`/`--launch-count`/内核筛选/源码关联）命令并执行，失败按错误模式分类给修复动作（计数器权限、注入失败、trace 项非法、路径不存在）；ncu 采集前先探性能计数器权限——**受限时立即返回开启方法**（管理员权限或 NVIDIA 控制面板开发者设置），不消耗采集时间。nsys 采集与全部报告分析**不需要**该权限。
- **与 `torch` 子Agent 的分工（两个面互不依赖、可同时装载）**：本子Agent 负责 Nsight 报告（Systems 时间线 + Compute 单内核）；PyTorch Profiler trace（`.pt.trace.json(.gz)`）由 `torch` 子Agent 负责（算子级/Python 级/显存级解释）——两者共用 `src/core/perf/` 基建但**零互相引用**（守护测试 `core/perf/coexist.test.ts` 固定三条：工具命名空间无交集、任一面不得引用另一面、core 不得反向依赖分析面）。Windows 上 PyTorch 的 CUPTI 采集不可用，故 PyTorch 场景的 GPU 内核级时间线靠本子Agent 的 nsys 采集补齐。

#### `torch`（PyTorch Profiler trace 分析与代码问题定位）

实现于 `packages/agents/src/agents/torch/`（`torch.ts` 定义入口 + `torch.md` 系统提示词 + 解析/聚合/诊断/工具分层 + `torch.test.ts`）：
解析 `torch.profiler.profile(...).export_chrome_trace()` 导出的 Chrome Trace（Kineto），面向「哪个算子、哪行 Python、显存怎么用」，
产出量化问题清单并把热点定位到工程源码 `文件:行`。与 `nsight` 相互独立、可同时装载。

- **PyTorch Profiler trace（Chrome Trace / Kineto）**：本子Agent 的第二个报告面（工具 `torch_overview`/`torch_ops`/`torch_memory`/`torch_findings`），面向「哪个算子/哪行 Python/显存怎么用」。格式依**真实 trace 核实**：顶层 `{"traceEvents":[…], "profile_memory":1, "with_stack":1, …}`，`ts`/`dur` 单位为**微秒**且 `ts` 为绝对纪元时间；事件类别 `cpu_op`/`python_function`（名称内嵌 `文件(行): 函数`）/`user_annotation`（`ProfilerStep#N`）/`kernel`/`cuda_runtime`/`gpu_memcpy`/`gpu_memset`/`cpu_instant_event`（含 `[memory]` 分配器事件，`Bytes` 负数为释放、`Total Allocated`/`Total Reserved` 为累计快照）。
- **超大 trace 的流式解析**：`jsonstream.ts` **不整文件 `JSON.parse`**（内存会与文件同阶），而是增量扫描 `traceEvents` 数组——状态机定位后逐个元素切片、交由调用方单独解析与聚合，内存与文件规模解耦；原生支持 gzip（`*.pt.trace.json.gz`，TensorBoard trace handler 的默认产物），解码与解析流水化、不落中间文件。**单位边界**：µs 在入参处换算为 ns 再交给共享时间线组件（并集/并发/自适应分桶按 ns 语义设计，含 50 µs 空闲缝阈值），出参再换算回 µs——否则空隙阈值被当成 0.05 µs、所有空闲缝漏检。
- **单趟聚合产出**：类别统计、算子/内核/CUDA API/用户标注排行（**自身耗时**与总耗时分开——同类嵌套做减法、跨类不做，避免无定义的归因）、内核几何与「内核 → 发起算子」归属（`correlation`）、传输按方向聚合、显存（峰值已分配/已保留、碎片率、最大分配、按设备分布、地址追踪超限降级标记）、时间线（CPU/GPU 双路并集与占用序列、GPU 空闲缝及**缝内 CPU 是否在忙**、CPU/GPU 重叠）、步级统计与 python 位置热点；上限集中在 `TORCH_LIMITS`。
- **诊断规则**（`torch-findings.ts`，阈值集中在 `TORCH_THRESHOLDS`、测试锁定）：同步阻塞（`.item()`/主机往返）、CPU 受限（GPU 利用率低且空闲缝内 CPU 在忙）、Python 开销、算子碎片化、autograd 引擎开销、小内核启动受限、单内核主导、占用率压力、显存碎片与分配 churn、步时抖动、float64 混入、布局/拷贝转换、用户代码热点。**缺维度的规则不做判定，只如实说明缺什么、怎么补**（例：无 GPU 事件时不给内核级结论，而是指向 `nsight_capture kind=nsys`）——这是证据纪律在 torch 侧的延续。
- **平台事实（实测）**：**Windows 上 PyTorch 的 CUPTI GPU 采集不可用**——显式启用 `ProfilerActivity.CUDA` 仍无任何 `kernel`/`gpu_memcpy` 事件。因此 torch trace 侧专注 CPU/算子/内存维度，GPU 内核级时间线由 nsys 提供（Windows 可用）；两者用同一套「量化证据 → 源码位置」口径互补。
- **工具集（7 个）**：`torch_reports`（trace 索引：list/info，含是否已有事实缓存）、`torch_overview`（总览，含前向/反向拆分）、`torch_ops`（算子/内核下钻，含筛选与内核归属）、`torch_memory`（显存分析）、`torch_findings`（诊断，可选 `locate=true` 落到源码 `文件:行`、可选 `save=true` 导出 Markdown）、`torch_compare`（两份 trace 改前改后对比）、`torch_capture`（生成/执行采集脚本）。**审批**：仅 `torch_capture mode=run`（执行被分析脚本）需审批，其余只读免审批；报告路径类参数带 `project` 包装。
- **时间预算与落盘事实缓存**（对齐 nsight 的 pending 分支）：扫描带时间预算（`DEFAULT_SCAN_BUDGET_MS` 5 分钟，依据引擎单次工具调用上限留渲染/定位余量），超预算**中止扫描并返回未完成事实**（不抛超时错误），工具层给出「本次未完成 + 已扫多少 + 可复制的后台命令 + 跑完再调即命中落盘缓存」；事实缓存为**内存→落盘→扫描**三级（`{GEBAI_HOME}/cache/torch`，`TORCH_CACHE_DIR` 可覆盖，键=路径+大小+mtime），落盘只在完整扫描且耗时 ≥2s 时发生（未完成的结果不缓存，避免把半份事实当结论）。实测：285.7 MB / 200 万事件触发 pending → 后台 18s 落盘 → 再调用 **5 ms** 命中。
- **前向/反向拆分（fwdbwd）**：**格式依真实 trace 核实**——`cat:"fwdbwd"` 的 `ph:"s"` 挂在**前向算子**上（`ts` 等于该算子起点）、同 `id` 的 `ph:"f"` 挂在**其反向算子**上（并非「前向段/反向段」的区间标记，`name` 就是 `fwdbwd`）。故口径是「前向算子耗时合计 vs 其反向算子耗时合计」与逐步反向占比，而不是整段时间的划分；无 fwdbwd 事件时如实说明未采集。
- **内核归属**：`correlation` 链（内核 → 最内层 `cpu_op` → python 帧）为主，`ac2g` 流事件（CUDA API ↔ 内核）为第二通道补全无 correlation 的内核；无流事件时如实说明归属仅依赖 correlation。Windows 无内核事件时退化为「CUDA API → 发起算子/发起位置」视图。
- **文件变更防护**：扫描前后校验存在性与大小/mtime，不一致与 `ENOENT` 类错误都重写为可操作提示（「分析过程中被修改/删除，请重试」），不把原始系统错误抛给模型。
- **实测性能（本机 RTX 4080 SUPER 宿主）**：**按事件数是可跨格式比较的口径**（事件平均大小随字段密度变化，按 MB 口径不能直接对比）。JS 路径在**真实 torch 输出格式**（平均 ~350 字节/事件，事件带缩进与嵌套 args）下约 **30 MB/s**（≈12 万事件/s）；原生后端（见下）**62 MB/s**（扫描阶段独占 **300 MB/s**）。历史记录（1158 MB / 605 万事件 38.5s 等）出自**简化字段的合成 trace**（89 字节/事件），按事件口径约 15.7 万/秒——不代表真实格式耗时，真实格式的字段密度高约 4 倍，同一实现会慢一个量级。JS 路径的优化手段全部由对照实验选定：`charCodeAt` 状态机（384 MB/s）而非 `buf[i]`（26 MB/s）或「5 定界符 indexOf 跳转」（36 MB/s）、元素按批产出、两级分组表（免每事件字符串拼接）、字段按偏移量提取（不构造中间对象）。剩余成本在「逐元素文本物化」与流式缓冲的逐块拼接（实测：同一段循环跑在扁平整串上 389 ms vs 跑在流式缓冲上 1327 ms）。
- **原生后端（客卿 Rust 边车）**：`keqing/rust/torch/` 以同一套聚合在原生执行——**整文件读入内存**（GB 级内存可接受，这是明确取舍）、**字节级扫描**（不经过 JS 层，也没有「逐块拼接缓冲」的开销）。实测 259.7 MB / 105 万事件：原生 scanMs 1.85s vs JS 8.70s（**4.7×**；真实 0.4 MB trace 上 **13.0×**、gz 形态 **5.6×**），**逐字段精确一致**（9212 项零差异），峰值 RSS 268 MB 对 JS 侧约 1.5 GB（原生连内存都更省：约 1× 文件大小 vs 约 5.8×）。分阶段实测（同一文件，按事件记）：仅扫描 710 ms（**366 MB/s**）、叠加字段提取 1182 ms（220 MB/s）、叠加聚合 1847 ms（**140.6 MB/s**，优化前 62 MB/s）。
- **原生的热点与优化原则**：扫描阶段已到 366 MB/s（与 Go 重写同量级），剩余耗时在**逐事件的字符串分配**与 **HashMap 重查**——优化手段是把字段提取改为**借用切片**（`Cow` / `&[u8]`，仅在新分组插入时才 `to_owned`）、查表用借用键、复用键拼接缓冲。实现内保留基准探针 `TORCH_BENCH_PROBE=noop|parse`（只扫描 / 叠加字段提取）与 `TORCH_BENCH_PHASES=1`（打印并行分块的阶段耗时：块起点摘要/块内采集/归并/输出组装）——性能回归靠它定位到阶段，不靠盲猜。
- **原生回退语义**：`resolveTorchFacts` 原生优先、失败**自动回退 JS**（边车未构建/未装载——服务端部署下客卿整体禁用/调用报错），回退原因如实回报；原生返回值经 `coerceNativeFacts` **严格校验**（字段缺失/类型不符即抛错回退，绝不让半份事实进入诊断——诊断据此给出「没问题」比慢更危险）；`TORCH_NATIVE=off` 可显式固定走 JS。工具输出在原生路径标注「｜原生聚合」、回退且曾尝试原生时标注「｜JS 聚合（原生不可用）」；JS 常态输出保持原样。等价性由 `native.test.ts` 锁定（真实字段结构合成 trace 上原生 vs JS 逐字段比对，含步级/算子/内核/fwdbwd/flows/归属/显存/时间线；未构建边车时原生侧自动跳过）。**时间预算只作用于 JS 回退路径**——原生不设流式中止点，可用时不会出现「未完成」。
- **原生并行（rayon 分块 + 有序归并）**：分块**不能**是「切段各自聚合再相加」——聚合状态里有一半带**顺序依赖**（浅栈自身耗时、采样器蓄水池的逐次替换、时间线并集的 running-max 语义、显存活跃集/峰值/TopK、flow 配对与全局 `last_activity`、correlation 启发表、分组首现序与 `MAX_GROUPS`/`SAMPLER_GROUPS` 分配序）。做法：① **精确块起点**——先并行算「块间结构摘要」（字符串态 + 括号深度增量），再按块序顺序合成（K 步、与字节数无关）得到每块起点的**精确**扫描器状态；猜起点是不行的：猜错会把嵌套对象当顶层元素、静默污染结果。② 块内沿用同一套状态机（元素按「起点落在块内」归属，跨块元素由本块扫完、下一块跳过起点前已开始的片段），只算**可交换**的部分（计数/求和/min/max、分组统计、类别统计、python 位置、传输聚合），顺序依赖部分只记**紧凑日志**（栈事件流 24 B/条、区间 16 B/条、采样时长 8 B/条、显存/flow/correlation 记录）。③ 归并阶段按全局序回放日志，并与单趟路径**共用同一批 helper**（`close_frame_at`/`pair_flow`/`pair_fwd_bwd`/`upsert_attr`/`Union::add`/`Bins::add`/`Sampler::add`/`apply_mem_instant`）——语义只有一份实现，两条路径不会漂移；浅栈按**（进程,线程）并行回放**（自身耗时 + cuda_runtime 发起帧解析，与顺序路径同一套懒关闭/包含/清栈规则）。两处配套优化：**结构摘要用 SSE2 掩码跳纯文本段**（16 字节一组，`"`/`\`/`[`/`{`/`]`/`}` 的掩码等价类——只允许多报不漏报，非 x86_64 回退 SWAR）；**非 gz 大文件分片并行读**（每片独立句柄 + seek，I/O 并行度与核数解耦）。开关 `TORCH_NATIVE_THREADS`（未设 = 自动：< 4 MB 走单趟，否则 = 可用核数；`=1` 强制单趟路径，即改造前行为）。
- **原生并行的实测（本机容器 cgroup 限 4 核 CPU，宿主 64 逻辑核；多轮取 min）**：224 MB / 94 万事件 **743ms → 408ms（1.82×）**；672 MB / 279 万事件 **2175ms → 1201ms（1.81×）**（2 线程 1.36×、4 线程 1.78×、8 线程 1.82×——**4 线程即饱和**，因为 CPU 配额就是 4）。CPU 时间 2.09s → 3.25s：多出来的是「精确块起点的那一遍结构摘要」（≈ 一次额外全文件扫描，用 SSE2 掩码跳纯文本段）与块内日志/归并回放。**加速比受 Amdahl 限制**，以 672 MB 为例（scanMs 分解）：读文件（已分片并行）＋块起点摘要（并行）320ms、块内扫描与聚合（并行）420ms、归并回放 400ms（全序串行：并集/分桶 120ms、correlation 启发表 94ms、flow 44ms、显存 22ms、采样器 6ms、可交换统计与分组表合并 4ms）、输出组装 100ms为串行段——真正可并行的扫描+解析在 4 核上只摊到 420ms。峰值 RSS 826 MB → 1096 MB（≈ 1.6× 文件大小；日志约 24–48 B/事件）。测量噪声：共享宿主下同一配置的 min/median 可差 20%+（实测区间回放 121ms 与 278ms 都出现过），故上述数字均为**多轮取 min**。
- **原生并行的等价性防线**：`native-parallel.test.ts` 用**同一二进制改块数**（1 vs 2/8/64 块）逐字段比对（数值按相对 1e-9、其余精确）——真实结构夹具、对抗夹具（跨块嵌套帧、乱序清栈、flow/fwdbwd、显存、多线程/进程）、gz 形态都覆盖；块内小计 vs 逐事件累加的求和顺序差异实测最大相对偏差 ~5e-13（容差内）。未回放的状态、漏携带的跨块帧/活动都会在这里变成字段差异，而不是靠「看起来对」。
- **原生并行的已知顺序剩余（后续可继续压）**：归并回放里仍有全序串行段——并集/分桶回放（约 120ms/672MB，Union 的 running-max 与 gap 记录语义就取决于事件序，Bins 虽可证明与到达序无关但共用同一遍历）、correlation 启发表回放（约 94ms，launch→kernel 配对跨块，且有 `MAX_FLOWS` 上限这个全局序条件）、flow 回放（约 44ms）、输出组装（约 100ms，`steps` 等逐项构 JSON 的分配成本）。另外块起点的结构摘要预扫是额外一遍全文件读（无它就必须串行扫描才能得到块起点状态，而猜起点会把嵌套对象当顶层元素）。当前选择是「先保证逐字段一致，再谈吞吐」；继续压需要把 Union 改成并行前缀/分段摘要（`1/(0.4+0.6/4)` 量级的上限已接近，收益递减）。
- **跨平台**：nsys/ncu 解析顺序为 环境变量 → 安装目录扫描（Windows `Program Files\NVIDIA Corporation\Nsight *`（2025.x 的 `target-windows-x64`/旧版 `bin`）、Linux `/opt/nvidia`·`/usr/local/cuda`·`/usr`、macOS `/opt/nvidia`·应用包）→ `PATH`；命令构造按**宿主 shell 语义**（Windows 经 PowerShell 时带调用运算符 `&` 且路径含空格必须引号、`GEBAI_SH_SHELL=cmd` 回落双引号、POSIX 单引号）——nsys/ncu 的安装路径恒含空格，裸拼必失败。
- **环境变量**：`NSIGHT_SYSTEMS_BIN`/`NSIGHT_COMPUTE_BIN`（显式指定可执行文件）、`NSIGHT_CACHE_DIR`（缓存根）、`NSIGHT_PROJECT`（默认工程根，用于报告路径与源码定位基准）、`NSIGHT_NATIVE`（`off` 时显式关闭原生聚合通道、固定走 JS 流式实现）——经 def 的 `envVars` 汇总进前端环境变量面板。
- **预加载**：`preload = false`，按需装载（无 GPU 的机器上报告分析同样可用，只是采集不可用）。

- **解耦与共装载**：共用基建置于 `src/core/perf/`（流式聚合原语 `agg.ts`、格式化与工具构造 `format.ts`、符号定位 `locate.ts`、计时 `timing.ts`、输入指纹 `input.ts`、测试桩 `test-ctx.ts`），
  任何一面都不引用另一面的模块；宿主注册表按 `{agent}_{tool}` 命名，两面工具命名空间天然无交集（`nsight_*` / `torch_*`）。
  守护测试 `core/perf/coexist.test.ts` 把「可同时装载」与「零互相引用」固定为断言——工具面演进若破坏约束会直接测试失败。
- **环境变量**：`TORCH_TRACE_PROJECT`（默认工程根，用于 trace 路径与源码定位基准）。
- **预加载**：`preload = false`，按需装载；与 `nsight` 同时装载时各自工具面完整可用。

#### 命名与预加载总览

| 子Agent | 工具 | 审批 | 预加载 | 适用 |
|---------|------|------|--------|------|
| `code` | 独有工具 search_symbols/analyze/git/preview_server/env_detect/system_info（文件读写查询与交互编排复用**全局工具**——read/write/edit/patch/sh/py/ls/grep/glob/file/fetch_url/ask/todo/subsession_run，带 project 参数路由项目，不重复注册） | 无（全局工具维持自身姿态） | ✗ | 代码编写与源码分析/修改（非 GEBAI 自身代码：tree-sitter 语法分析与符号双模式搜索（定义 + 引用/调用点）、git 只读核对（status/diff/log/show/branch/ls-files/grep）、浏览器端验证委托、项目内置、验证服务（自全局下沉 preview_server）与环境/工具链探测（env_detect/system_info）；文件读写/补丁应用/待办规划/方案确认经全局工具完成） |
| `self_optimize` | 独有工具 read_feedback/run_tests/rollback/journal/backlog/page_capture；**通用工具与工作流直接复用**（装载/`subsession_run` 预加载均连带 code——文件读写用全局工具、分析/验证类操作用 `code_*` 独有工具（含 code_preview_server），code 工作流提示词随连带装载注入，不重复注册；视觉分析经 vision 子代理 `vision_analyze`/`vision_ocr`——依赖连带装载获得，def 不复刻）；声明 `writeGuard` 写范围守卫（核心引擎源码默认只读的代码级强制；可写面 = 子Agent 扩展面三域 + 仓库级文档/配置） | run_tests+rollback | ✗ | 优化歌白自身（tree-sitter/补丁应用/验证服务等通用能力经全局工具与 code；特有：反馈读取、测试准入（test/typecheck/lint 三件套）+回滚（含新建文件清理）、优化日志跨会话沉淀、待优化项暂存与集中全面优化（backlog 离线优化——知识/工具不足导致重复试错时先暂存后集中处理）、项目内置+AGENTS.md 自动注入、**三类子Agent 开发场景与目录（内置/二开/客卿）**、前端页面捕获读取实际 html/截图 + 视觉分析（vision 子代理）、写范围守卫；**装载即连带装载 code 与 vision**） |
| `explore` | 独有工具 search_symbols/analyze/git（全部只读，支持 project 参数路由；文件读取检索复用全局只读工具） | 无（全免审批） | ✗ | 只读代码探索（大范围摸底/架构梳理/多点位定位，subsession_run 委托执行（默认继承全局工具），返回结论与 文件:行号 清单，中间过程不占主上下文；修改用 code） |
| `vision` | analyze + run/pip/status + ocr/locate/locate_image/detect（装载后共 8 个：客卿侧四项本地识别与语言基础工具合并，全部只读免审批；image 必填无缺省源——不截图，实现复用 makeVisionTool/cv-analysis 共享工厂） | 无（全免审批） | ✗ | 视觉能力通用层（图片分析型，全局 vision 工具已移除——视觉语义分析唯一入口：主会话 agent_load/路由自愈，子Agent 依赖声明连带装载）：多模态语义分析（analyze）+ 本地 OCR/文字定位/模板匹配/YOLO 检测（**受客卿门控——客卿仅本地形态启用，沙箱启用即整体禁用，故服务端部署下只剩 TS 侧 analyze 可用**；被依赖方——self_optimize 声明 dependencies 复用；desktop/playwright 域内走共享工厂不依赖）；现截屏幕/页面用 desktop/playwright |
| `desktop` | screenshot/window_*/type_text/key_press/mouse_*/clipboard_*/screen_info/ocr/locate/locate_image/detect/wait_for | window_*+type/key/mouse+clipboard_write | ✗ | 桌面控制（截图（虚拟屏幕多显示器）/本地 OCR 识别与文字·模板定位/窗口（激活/移动/状态）/输入（输入/点击/滚动/拖拽）/剪贴板读写/界面条件等待，仅本地模式） |
| `feishu_docs` | auth_status/auth_user_authorize/auth_user_token/auth_user_status/auth_user_clear/create_doc/get_doc_meta/get_doc_text/get_doc_blocks/list_blocks/find_blocks/add_blocks/update_block/replace_text/set_table_width/delete_blocks/import_markdown/import_xml/lint_doc/style_guide/export_doc/list_files/create_folder/get_file_meta/upload_file/download_file/delete_file/search/create_sheet/get_sheet_meta/read_sheet/write_sheet/append_sheet/create_bitable/list_bitable_tables/list_bitable_records/add_bitable_records/update_bitable_record/delete_bitable_records/list_wiki_spaces/create_wiki_node/get_wiki_node/get_board/add_permission/set_link_share/api_call | 写操作全部（创建/修改/删除/上传/授权）；**只读类不审批**（lint_doc 体检、style_guide 读排版规范） | ✗ | 飞书云文档（文档/表格/多维表格/知识库/云空间/搜索/权限/思维导图画板；**可配置 user_access_token 以用户身份操作、创建用户所有权资源**；`FEISHU_DOCS_FOLDER_URL` 配置用户目标文件夹后创建的资源自动落该文件夹（用户自动有全部权限）；**内置排版 SKILL（排版总纲 + 体裁选择表 + XML 排版语法 + 15 类体裁契约）与排版体检、写入前预检、大纲定位与跨块替换**；需配置 `FEISHU_DOCS_*` 或全局 `GEBAI_FEISHU_*` 凭证） |
| `playwright` | open/content/screenshot/click/fill/press/select/check/hover/dblclick/drag/upload/wait_for/evaluate/pages/new_page/switch_page/close_page/close/serve_dir + pdf/downloads/dialogs/emulate/cookies/local_storage/storage_state + ocr/locate/locate_image | open+click+fill+press+select+check+hover+dblclick+drag+upload+evaluate+new_page+serve_dir+emulate+cookies+local_storage+storage_state（凭证类与 evaluate 同级） | ✗ | 浏览器自动化（无头 Chromium，node 桥接；选择器 `>>` 穿透 iframe；下载/对话框/仿真/登录态管理；页面截图本地 OCR/文字·模板定位（canvas 等无 DOM 内容兜底，沙箱可用）；需宿主机 node + playwright 包 + 浏览器） |
| `reverse_site` | 独有工具 capture_start/capture_stop/capture_clear/capture_list/capture_body/capture_replay/capture_curl/capture_har/capture_ws/route/http_request；**依赖 playwright（`dependencies` 自动连带装载——浏览器自动化全套与审批映射复用 `playwright_` 命名空间，共享同一浏览器会话）**，文件读写与编排走全局工具 | http_request+capture_replay+capture_curl（route add 工具级函数按 action；浏览器交互类随 playwright def） | ✗ | 网站/接口逆向（网络录制+WebSocket 帧还原接口、带登录态改参重放、重放命令生成、HAR 导出、请求拦截 block/mock/modify、直连探测验证、产出 API 文档；可联动 self_optimize 转新子Agent；需宿主机 node + playwright 包 + 浏览器） |
| `feishu_group` | chats_list/chat_info/members_list/user_info/message_send/chat_create/chat_update/chat_members_add/chat_members_remove/chat_disband | 写操作全部（发消息/建群/改群/拉人/移人/解散） | ✗ | 飞书群基础能力（群列表/详情/成员查询（open_id+姓名——@特定人与 cron 通知 at 名单取材）/用户信息/群内发消息（at 标签）/建群改群/成员增删/解散；需 FEISHU_GROUP_APP_ID/SECRET 或全局 GEBAI_FEISHU_* 凭证） |
| `task` | add/list/update/run/cancel/remove/files/notify（→ `task_add`/`task_list`/`task_update`/`task_run`/`task_cancel`/`task_remove`/`task_files`/`task_notify`） | add+update+remove+run+cancel+files | ✗ | 统一任务管理（定时/普通/闲时三类共用一条队列：创建脚本运行/提示词运行 agent 的用户级无人值守任务、查看/修改/手动执行/取消/删除、任务资源文件与模型主动通知 `task_notify`；支持执行目标（独立新会话/专用会话/绑定会话）、时区、@at 一次性、错过补跑、飞书群/webhook 通知、连续失败自动停用） |
| `wps` | word_create/word_read/word_append、excel_read/excel_write/excel_edit、ppt_create/ppt_read、pdf_create/pdf_read/pdf_merge/pdf_split/pdf_edit（projectAware 项目路由；文件浏览与交互编排复用全局工具） | 无（防盲覆盖守卫在工具体内，与全局 write 同语义） | ✗ | Office/PDF 文档处理（.docx/.xlsx/.pptx 读写与富排版：markdown/块结构生成 Word、原 XML 追加保留原文档格式、Excel 多表公式样式与 ops 批量编辑、PPT 版式/图表/图片/备注，csv/tsv 读取；PDF 生成（中文字体自动嵌入子集化）/逐页文本提取/合并/拆分/页面编辑与水印；旧版二进制格式 .doc/.xls/.ppt 不支持） |
| `reel` | setup（库根与共享运行时状态 + 主机/GPU 探测与渲染档 + 浏览器就绪）、project（init/install/status——落位内置模板并以目录联接复用共享依赖）、render（still/preview/video/bench/status/log/stop，进程内直连原生渲染库；成片默认分片并行（多浏览器，失败回退整段）并可传 `shards` 显式指定；浏览器与原生二进制目录均可配置；`still wait=true` 送审主帧）、voice（build/sfx/estimate/srt/voices——本地离线配音、字幕与音效：合成 WAV 落工程 public/audio/voice、写 src/film/voice.generated.ts（配音表 + 字幕表）、出 out/subtitles/*.srt 与分段 JSON；sfx 波形合成音效落 public/audio/sfx 并写 src/film/sfx.generated.ts） | setup + project init/install + render 全部动作（voice 免审） | ✗ | 产品视频制作（电影感宣传片 / demo reel / 单镜头动效复刻）：**创作能力内化**——设计 token、18 个镜头原语（字标/大标题/网格/节点流程/数字/字幕/口播字幕/等宽块/面板/准星/闪切/合影/时间窗…）、2.5D 真实页面相机（放大走 CSS `zoom` 布局级缩放，保文字锐利）、时间线唯一真相源与可直接渲染的示例片，`project init` 展开成可编辑工程；**默认确认式创作**（概要设计 → 每节主帧 → 成片关键帧三处送审，主帧直接停在对话里给用户看）；零外部载荷（模板内联于包内，dev 与 bundle 形态均可用），共享运行时优先复用既有安装（目录联接，省 753MB）；渲染提速两把旋钮均实测落地（分片并行 + 实测光栅化后端，20 核 + 独显机器上真片 23.4 → 60.6 fps；4 核配额容器上分片为负收益，已由实测吞吐自动改判），无 GPU 如实报软件档；**配音、字幕与音效全程本地**（`reel_voice`：本机系统语音合成 + 帧号同源的字幕烧入与 SRT 与分段 JSON 交付，不联网、不耗配额；`sfx` 用波形合成内核出 riser/impact/sparkle 等惯用音效，零素材；`estimate` 可在分镜前定镜头窗口；字幕与音效都不依赖配音） |
| `tts` | speak/voices/sfx/effect/mix（→ `tts_speak`/`tts_voices`/`tts_sfx`/`tts_effect`/`tts_mix`） | 无（会话内产物落盘；安全模式不提供） | ✗ | 语音合成与音效：把文本合成为可播放音频，**本机离线完成**（Windows 系统语音 WinRT OneCore 优先、SAPI5 回退——不联网、不耗配额、无需安装；非 Windows 如实报错不回落在线服务），支持音色选择与语速/音调/音量；另含音效合成（27 个预设 + 自定义波形与滑频）、音频效果处理（变速不变调（相位声码器）/变调/变速/回声/混响/滤波/机器人音/淡入淡出等）、多轨混音与拼接（提示音+语音+结束音一次成段，循环铺底）——音效与处理为纯计算，任何平台可用；产物 WAV 落会话 tmp/tts/、聊天内直接播放；`play=true` 同时在运行 GEBAI 这台机器的扬声器上播报（后台播放，仅本地模式）；其引擎基建（`core/tts/speech.ts` 的语音与 `core/tts/audio.ts` 的音效）同被 reel 子Agent 的 `reel_voice` 复用（同一份实现，不复制） |
| `imgproc`（客卿） | info/grayscale/resize/stats（→ `imgproc_info`/`imgproc_grayscale`/`imgproc_resize`/`imgproc_stats`） | 全部 | ✗ | 图像处理（C++ 典型场景：stb 单头库 vendor（`cpp/stb/`，解码/编码/重采样），尺寸与亮度分布探测、Rec.601 灰度化、sRGB 高质量缩放（等比/倍率）、RGB 通道统计与 Otsu 阈值；构建引导自动编译） |
| `disk`（客卿） | tree/du/top/depth/volumes/scan/clean/trash（→ `disk_tree`/`disk_du`/`disk_top`/`disk_depth`/`disk_volumes`/`disk_scan`/`disk_clean`/`disk_trash`） | 全部 | ✗ | 磁盘使用分析与清理（Go 典型场景：goroutine 并发遍历 + 原子在途计数（无死锁收尾），目录树概览/指定深度占用排行/大文件排行（可按扩展名过滤）/结构统计（总量、最大深度、空目录），du 语义子树大小；盘容量总览（linux/darwin/windows 分平台探测）；清理候选扫描（temp/log/backup/dump/cache/empty_dir 类别与 big_file/old_file 阈值）；执行清理 dry-run 预览 / quarantine 隔离（可 trash restore 还原）/ delete，带范围护栏（目标限 dir 内、拒绝卷根·用户主目录·系统目录、符号链接跳过、条目上限）与审计日志；隔离区批次列出/还原/彻底清除；go module 管理） |
| `nsight` | doctor/reports/overview/kernels/timeline/query/findings/compare/kernel_detail/locate/capture + aggregate（客卿 Rust 边车贡献的原生聚合后端；报告路径类工具带 project 参数；全部分析只读） | capture（执行被分析程序） | ✗ | NVIDIA Nsight 报告分析与 GPU 性能问题定位（Nsight Systems 时间线 + Nsight Compute 单内核）：报告 → 问题清单（量化证据 + 根因 + 修复方向）→ 源码 `文件:行`；**多卡按 deviceId 分流**（每卡利用率/空闲/并发 + 不均衡诊断，合并口径会掩盖单卡停滞）；memset 计入 GPU 活动；采集开销按窗口内外区分；CUDA Graph 维度；参数化时间窗；**报告间对比**（改前改后）；结果可导出 Markdown；超大报告流式聚合 + 事实缓存（内存与规模解耦）；聚合有原生（Rust 边车）/ JS 两条同构实现，原生优先、不可用即自动回退；ncu 采集需 GPU 性能计数器权限（工具前置探测并给出开启方法），nsys 采集与报告分析不需要 |
| `torch` | reports/overview/ops/memory/findings/compare/capture（→ `torch_reports`/`torch_overview`/`torch_ops`/`torch_memory`/`torch_findings`/`torch_compare`/`torch_capture`）**+ aggregate（客卿 Rust 边车贡献的原生聚合后端）**；trace 路径类工具带 project 参数；除 `capture mode=run` 外只读 | `capture mode=run`（执行被分析脚本） | ✗ | PyTorch Profiler trace（Chrome Trace / Kineto）分析与代码定位：trace → 算子/内核热点（含自身耗时与张量形状）· 步级耗时与抖动 · **前向/反向拆分（fwdbwd 配对）** · **内核→发起算子归属（correlation + ac2g 流）** · 显存峰值与碎片率 · 问题清单（同步/CPU 受限/Python/碎片化/autograd/小内核/显存/精度与布局）→ 源码 `文件:行`；**原生聚合后端**（整文件读入 + 字节级扫描，实测 4.7–13× 于 JS、峰值内存约为 1× 文件大小，且逐字段一致；不可用时自动回退 JS 并如实回报原因）；**分块并行**（rayon 分块采集 + 有序归并，实测 224 MB/94 万事件 1.5×、672 MB/279 万事件 1.59×，`TORCH_NATIVE_THREADS` 可调/可关，逐字段一致由同二进制改块数的 A/B 锁定）；**时间预算 + 落盘事实缓存**（超预算返回未完成 + 后台命令，跑完再调毫秒级命中）；**trace 间对比**（改前改后）与结果导出 Markdown；超大 trace 流式扫描 + 缓存；与 `nsight` 零互相引用、可同时装载（Windows 上 PyTorch CUPTI 采集不可用，GPU 内核级时间线由 `nsight` 的 nsys 采集补齐） |
| `local_infer` | engines/engine_fetch/model_fetch + **sources/source_build（内网源码编译）** + status/models/start/stop/restart/logs/bench/inspect + targets/generate/batch/jobs（→ `local_infer_engines`/`local_infer_engine_fetch`/`local_infer_model_fetch` + `local_infer_sources`/`local_infer_source_build` + `local_infer_status`/`local_infer_models`/`local_infer_start`/`local_infer_stop`/`local_infer_restart`/`local_infer_logs`/`local_infer_bench`/`local_infer_inspect` + `local_infer_targets`/`local_infer_generate`/`local_infer_batch`/`local_infer_jobs`） | start+stop+restart+bench+engine_fetch+model_fetch+source_build | ✗ | 推理引擎的**全生命周期管理 + 统一调用入口**（子项目 `infer/`）——**推理不绑定操作系统、不绑定 GPU、不强制外网**（引擎矩阵覆盖 Windows/Linux/macOS × CPU/CUDA/Vulkan/Metal/ROCm/SYCL；无 GPU 用 CPU 档 + 小模型；**无外网用源码就地编译**）：**引擎供给**（`engines` 列矩阵×本机平台×已安装状态并给设备探测依据，`engine_fetch` 按资产直下（断点续传 + 校验 + 解压 + **递归定位可执行文件** + 写 `.engine.json` 标记）、`model_fetch` 下载模型（含预置小模型，幂等））；**内网/离线：源码发现与自动编译**（`sources` 发现 `{GEBAI_HOME}/resources/src/` 等根下的源码目录/归档并给离线可行性依据（归档预览：CMakeLists、**是否自带 vendor/ 依赖**——采样会假阴性故全量过滤）、`action=toolchain` 给工具链与各设备就绪 + 内网补齐办法（含各工具绝对路径，便携工具链不必改 PATH）；`source_build` 解压 → 工具链与设备门禁 → cmake 配置 → 编译 → 定位 llama-server → 安装到 `vendor/<engine-id>/bin/`（解引用符号链接）→ 写同格式标记（额外记 `build.{from_source,device,offline,log}`）；`background=true` 后台编译，`status/log/list/cancel` 管理；预编译包需外网，内网走这条）；**进程管理**（跨平台 launcher 为缺省启动路径——detached 拉起 + 日志重定向 + `/health` 就绪轮询，**不依赖 PowerShell**；状态文件 `infer/run/server-<port>.json` 记 PID/档位/模型/引擎/启动方式/日志/argv，status 逐个校验 PID 存活，stop 可按 PID/端口/档位精确终止进程树，restart 换档位/换模型/换引擎，logs 尾读日志并给错误特征摘要；Windows 无引擎时回退 `run-server.ps1`）；**统一推理目标**（`targets` 列本机/命名远端目标并可探活（n_ctx/slot/模型名），`generate`/`batch` 的 `target` 可选本机、直连 URL（可带 api_key）或 `LOCAL_INFER_TARGETS` 声明的命名目标——同一套调用面可派到本机 CPU 引擎、局域网机器或云端兼容端点，密钥一律掩码）；**批量提交**（条目数组或 JSONL、worker pool 限流（与档位 `parallel`/服务端 slot 比对）、逐条 append 落盘、按 id 断点续跑、`max_items` 分片，`background=true` 后台跑并立即返回 job_id，`jobs` 从 results.jsonl 实时统计并按 `owner_pid` 探活）；**结构化输出**（`response_format.json_schema` 交服务端转 GBNF 约束解码，工具侧再抽取 + 轻量 Schema 校验，失败回灌重试一次；`grammar` 直传兜底、`json_object` 最弱，服务端不认约束时自动降级并给预警）、模型与档位清单、llama-bench 基准、GGUF 结构与张量布局解析；工具复用子项目配置与脚本不复制逻辑；环境变量 `LOCAL_INFER_HOME`/`LOCAL_INFER_PROFILE`/`LOCAL_INFER_PORT`/`LOCAL_INFER_MODELS_DIR`/`LOCAL_INFER_TIMEOUT_MS`/`LOCAL_INFER_BATCH_MAX_ITEMS`/`LOCAL_INFER_TARGETS`/`LOCAL_INFER_REMOTE_API_KEY`/`LOCAL_INFER_LAUNCH`/`LOCAL_INFER_SOURCE_DIRS` |

#### 客卿（多语言子代理：边车协议 + 自动发现启动注册）

任意语言（Python/C++/Go/…）实现的子代理：**放置即自动发现 → 启动边车进程 → 握手拉取工具清单 → 注册为标准子代理**（`agent_list` 可见、`agent_load` 装载、`subsession_run` 委派——与 TS 子代理完全同构）。设计原则：**实现语言对模型透明**——子代理 = 工具 + 提示词（能力导向命名与描述），语言仅是工程组织维度。**self_optimize 可写**（客卿域 `keqing/` 属子Agent 扩展面，与内置域/二开域并列；改动需重新构建时由 manifest `build` 自动引导）。

- **按语言组织**（仓库根 `keqing/{lang}/`）：语言目录下共享基础框架驱动（协议实现 + 语言生态工具）与运行时资产（venv/构建脚本），每个二级目录一个子代理项目（manifest + 提示词 + 专属工具）——一种语言派生任意多个子代理；发现根即各语言目录（每个语言目录一个扫描根，二级目录即子代理项目），用户自建 `{GEBAI_HOME}/agents/{name}/` 同构（同名覆盖内置）
- **manifest 发现**（`core/agents/keqing.ts`）：扫描内置源 `keqing/`（dist 构建时由 build-subagents 整树复制——过滤 venv/__pycache__/编译产物；二进制形态物化 `{GEBAI_HOME}/vendor/keqing/`）；manifest 字段/占位符（`{python}`/`{driver}`/`{agent_dir}`/`{lang_dir}`/`{agent_name}`/`{exe}`/`{GEBAI_HOME}`）见 `keqing/README.md` 协议规范
- **构建引导**（编译型语言开箱即用）：manifest `build` 字段（`command`/`windows`/`unix` 平台分支）声明编译命令，command 首元素指向的可执行文件不存在时自动执行（占位符同 command，cwd 为 manifest 目录）——C++ 经 `build.bat`（vswhere 定位 MSVC）/Rust 经 `rustc` 直编；缺编译器/构建失败记 loadErrors（模型可见根因），不阻断其他子代理；可执行体已存在则跳过（增量）
- **边车协议 v2**（语言无关，NDJSON over stdio）：`init`（上报 name/protocol，须与 manifest 与宿主一致——锁步升级不做历史兼容，不匹配即注册失败记 loadErrors）→ `tools.list`（工具清单：裸名 + JSON Schema 原样透传）→ `tool.call`（驱动侧执行返回 `{output, data?}`，请求体顶级字段 `tool`/`args`/`ctx`）；**请求级 ctx**（v2 核心）：边车为进程单例跨会话共享，会话上下文随每次 tool.call 传递——`ctx.cwd`（会话工作区绝对路径，相对路径解析基准）/`ctx.env`（任务级 env 覆盖，随消息变随调用传）/`ctx.sessionId`（会话态隔离键，如 REPL 命名空间分桶）/`ctx.user`/`ctx.sandboxed`；四语言框架分发循环串行执行内设「当前请求 ctx」并提供 `ctx_env(key)`（请求 env → 进程 env 回落，**禁止写 os.environ**——并发请求不同会话互踩）与 `ctx_resolve(path)`（相对基准=ctx.cwd）助手；stdout 只写协议行、stderr 自由排障、stdin EOF 即退出防孤儿；**行尾容忍 CRLF**（Windows 驱动 text-mode stdout 默认翻译 `\n` 为 `\r\n`，宿主行解析剥尾部 `\r`——跨语言驱动不因平台换行约定挂起）
- **边车宿主**（`core/agents/sidecar.ts`，进程管理对齐 CV sidecar）：惰性启动/启动串行化/请求 id 配对并发复用；**工具执行时按子Agent 名从活跃注册表取当前边车实例**（装载时的工具对象跨重扫存活——热加载重扫会 dispose 旧实例并登记新实例，闭包直用旧实例会让已装载会话在重扫后全部报「边车已销毁」且不再拉起；取活跃实例后源码改动即「重扫 → 新进程加载新代码 → 已装载会话立即生效」，等效热重载）；**请求级 ctx 组装**（`sidecarTool.execute` 从 ToolContext 取 sessionId/user/sessionWorkdir/env/sandboxed 随每次 tool.call 下发；崩溃重发即原样重写请求行，ctx 不丢）；请求超时杀进程重启；**崩溃自愈**（意外退出自动重启一次 + 在途请求重发一次；连续快速退出 3 次放弃自动重启防抖动风暴，下次调用再拉起）；exit hook + 驱动 EOF 双保险；stderr 环形缓冲
- **生命周期集成**：`SubAgentManager.discover()` 尾部并行启动（boot 显式接线后生效——`setKeqingOpts`，测试不注入零影响；`roots` 选项可覆盖发现根供测试隔离）；**启动不阻塞**：boot 以 `discover({ deferNative: true })` 调用——TS 域扫描（本地文件遍历 + 定义 import）同步完成，客卿发现转后台任务（进程内同时在途只保留一份），就绪后自动重建合并视图；需要完整清单的操作（`load`，即 agent_load/路由自愈/预加载）经 `whenNativeReady()` 等它。为何必须如此：侧车握手与**编译型客卿的构建引导**耗时不可控（实测：受限网络下 go 下载 toolchain 的 TCP 超时把服务启动拉长 30s；`ensureBuilt` 自身的超时上限为 300s），启动不该为可选能力付这个代价；代价是启动瞬间（后台发现未回）的客卿子代理尚未进入清单——TS 侧同名贡献仍可见，下一次操作自动补齐；manifest/驱动/提示词文件变化纳入热加载签名（重扫重注册，**目录删除对账回收**——本次发现名单中消失的从贡献集移除，进程级边车注册表同步回收旧进程）；**TS 签名与 客卿 签名各自判定**——TS 目录未变而仅 客卿 变化时 `refreshIfChanged` 只重拉 客卿（幂等跳过 TS 扫描），REST `GET /api/v1/sub-agents` 响应前惰性调用 `refreshIfChanged`（放置新目录即出现在列表，无需重启）；pip 安装成功后驱动主动退出 → 宿主自愈重启 → 命令工厂重新解析占位符（venv 创建后自动切换 venv 解释器，无需重启服务）
- **跨语言同名合并**（`core/agents/merge.ts` `mergeSubAgentDefs` 纯函数）：TS 侧（`packages/agents/src/agents/{name}.ts`）与 客卿 侧（manifest 目录）同名定义共存为「贡献集」，SubAgentManager 分别维护 tsDefs/nativeDefs、任一变化后重算合并视图（TS 贡献在前）——description/systemPrompt 非空项依次拼接（全空兜底生成）、工具集并集（同名冲突保留 TS 侧并告警）、dependencies/envVars 并集、preload 取或、projectRoot/writeGuard 取首个非空；装载后两侧工具同一 `{name}_` 命名空间，卸载一并注销。配套约定「只在一处定义、其他地方留空」：manifest `description` 可省略、`PROMPT.md` 可缺失（即本侧不贡献，不生成占位文本）；分工样例 `vision`——重计算（onnxruntime 推理）与描述/提示词由 Python 侧写（`keqing/python/vision/`），LLM 耦合（多模态 provider）的 `analyze` 由 TS 侧写（见「视觉能力分层与子代理复用边界」）
- **边车环境**：基于宿主进程 env 继承基础变量（PATH/SYSTEMROOT 等——Windows 下 python 编码/subprocess 初始化依赖 SYSTEMROOT，极小 env 会启动即卡死无报错）+ `GEBAI_HOME` + `GEBAI_AGENT_DIR`（驱动定位子代理项目专属资产，如 Python 驱动加载 `{agent_dir}/tools.py`）+ manifest env 覆盖同名项；manifest `cwd` 语义收敛为驱动自身资产定位（进程工作目录），会话级路径一律经请求 ctx 传递——与进程单例跨会话共享的模型自洽
- **项目工具热重载（Python 驱动）**：`{agent_dir}/tools.py` 不再只在进程启动时加载一次——每次 `tools.list`/`tool.call` 前比对 mtime（一次 `os.stat`，开销可忽略），变更即重新加载（stderr 记 `[driver] 项目工具已热重载`）。**重载从基础集重建再叠加项目工具**（否则追加式合并会让上一版改名/删掉的工具残留）；`tools.py` 被删除 → 回退基础工具集；源码语法错误 → 回退基础集且不影响基础工具（失败安全，进程不退出）。为何需要：旧行为下改完源码旧代码持续生效（除非重启边车），调试本地能力（vision）时极易误判「修改无效」。重载会重置该模块的模块级状态（如惰性加载的模型），故只在源码真的变了才做
- **门控与边界**：仅本地形态（沙箱启用即禁用；`GEBAI_KEQING=off` 显式关闭）；边车工具缺省恒需审批（任意代码执行面），manifest `requiresApproval:false` 可声明只读免审批（如 vision 识别四工具——对齐 TS 侧同能力工具体验）；单项失败（manifest 损坏/启动/握手失败/协议版本不匹配）记 loadErrors 模型可见根因，不阻断其他子代理；工具名驱动侧为裸名（注册表自动加 `{agent}_` 前缀）
- **内置客卿子代理**（`keqing/{python,cpp,rust,go}/`）：`vision`（Python + TS 跨语言合并，见下：本地识别四工具 ocr/locate/locate_image/detect + TS 侧 analyze）；`imgproc`（C++：stb 单头库 vendor 于 `cpp/stb/`，info/grayscale/resize/stats 图像处理）；`disk`（Go：磁盘使用分析与清理——tree/du/top/depth 并发目录分析 + volumes 容量总览 + scan/clean/trash 清理与隔离区，go module 管理）；**Python 语言目录项目同时携带语言框架基础工具** `{agent}_run`（常驻命名空间 REPL：末行独立表达式求值 repr 回显，**缺省按 ctx.sessionId 隔离命名空间**——同会话共享状态跨会话互不可见，显式 session 参数仍可细粒度隔离，stdout/stderr 捕获，timeout 秒默认 300）/`{agent}_pip`（install：packages 或 `-r requirements.txt`，venv 不存在自动创建，装完边车自动重启加载，freeze 快照写回）/`{agent}_status`——现为 vision_run/vision_pip/vision_status，经 `tools.py` 合并模式与项目工具共存；解释器解析 `GEBAI_PYTHON_DIR` → 语言目录 venv（`keqing/python/venv`）→ 系统 PATH；四语言基础框架（Python 共享驱动 + C++ 头文件 + Rust cargo workspace + Go module，均含请求级 ctx 助手）见 `keqing/README.md`
- **协议方向性约束（依「核心不变式：能力外置、权力内守」）**：客卿协议为**单向下行**（宿主 → 驱动 init/tools.list/tool.call，驱动只回响应）——驱动侧不得自取工具、不得自证授权；未来若开反向调用（协议 v3+）必须为**委托式**：驱动只发意图，宿主按该次 `tool.call` 绑定的会话 ctx 与调用者审批姿态重新裁决，并需来源可证不可提权（宿主签发 token，不得转交）+ 回调白名单（默认只读）；自取式回调（驱动自主取用工具/自带授权声明）不予准入

> 全部按需装载（懒加载）；`GEBAI_PRELOAD_SUB_AGENTS` 可指定启动预加载名单，符合「预加载少而精」原则。

### 自我优化（代码级自改进）

Agent 通过修改**自身代码**来持续改进自己，不使用记忆（memory）、技能文件（skill）等运行时注入的不稳定能力：

#### 为什么不用 memory / skill

- **不稳定**：记忆注入、技能文件依赖运行时状态与外部文件，内容漂移、不可复现、难以审计
- **不可审查**：行为改进无法 diff、无法测试、无法回滚
- **冲突**：与「多用户隔离」「单一真相源」原则相悖（记忆按谁存？技能按谁加载？）
- **代码即一切**：子Agent 文件、工具实现、系统提示词本身就是能力载体，改代码 = 改能力，天然可审查/可测试/可回滚

#### 优化对象（均为代码）

| 对象 | 位置 | 说明 |
|------|------|------|
| 子Agent 定义 | `packages/agents/src/agents/`（单文件 `*.ts` 或目录 `{name}/{name}.ts` + `{name}.md`） | 新增/修改子Agent（名称、描述、提示词、工具） |
| 全局工具实现 | 服务端源码 | 工具行为优化、新工具开发 |
| 系统提示词模板 | 服务端源码 | 总Agent/子Agent 提示词迭代 |
| 默认配置 | 环境变量默认值/常量 | 阈值、超时、默认行为的调优 |

#### 优化闭环

```
发现改进点（用户反馈/失败案例/日志分析/自身复盘；任务执行中重复试错低效而不便立即优化的，
    先 self_optimize_backlog add 暂存问题+方向（自动记会话ID），随即 ask 用户确认时机（当场修复/后续）；
    后续 list 取清单集中全面优化；
    开工先 self_optimize_journal 查历史避坑）
    → 生成修改方案（改动点清单 + 预期效果）
    → 审批（需用户确认，高风险改动用/approval）
    → 由 self_optimize 子Agent 修改代码（专用子Agent，见下；新建/修改子Agent 后立即验证注册——
      注册失败错误附带载失败原因，据因修复）
    → 运行验证（self_optimize_run_tests：先相关测试文件，确认后 test/typecheck/lint 三件套，
      失败则修复或 rollback 回滚——恢复修改并删除新建文件）
    → 设计变更回写 DESIGN.md（修改行为/接口/协议/存储布局/常量/命名规则等设计层面时同步对应章节，文档与代码保持一致）
    → 用户验证（ask 确认方式）：UI/前端类修改 → page_capture 捕获当前页面
      （模型 read 实际渲染 html + vision_analyze 分析截图（vision 子代理连带装载），dev 模式自动热更新，先请用户刷新页面）；
      服务端功能类修改 → preview_server 临时新端口验证服务（独立进程不中断当前会话，验证后停止）
    → 优化记录（self_optimize_journal append：改动/验证/结论/教训，跨会话沉淀）
    → 变更落盘：代码变更即持久化，无需额外记忆
    → 构建/重启生效（开发模式直接生效）
```

#### `self_optimize` 专用子Agent

自我优化由独立子Agent `self_optimize` 承担（与 `code` 拆分，见「职责边界」）。**工具与提示词直接复用 `code` 与 `vision`**——def 声明 `dependencies: ["code", "vision"]`（依赖自动装载，见「子Agent 依赖与自动装载」），只声明独有能力（反馈读取、测试准入、回滚、优化日志、待优化暂存、页面捕获），通用编码能力（文件/分析/修改/验证工具与「规划→探索→定位→方案→修改→验证→收尾」工作流）由连带装载/预加载的 `code` 提供（验证服务 `preview_server` 亦经 code 的 `code_preview_server` 获得），视觉能力（截图语义分析/读图）由 `vision` 子代理提供（`vision_analyze`/`vision_ocr` 等 `vision_` 前缀），**不重复注册工具、不复刻提示词**：

```ts
export const name = "self_optimize"
export const description = "优化歌白自身（自身代码/子Agent/提示词/配置；外部项目用 code）……知识/工具不足或错误重复试错、低效时也装载：不便立即优化先 self_optimize_backlog add 暂存，后续集中全面优化……修改须过测试（run_tests）并同步 DESIGN.md，失败可 rollback 回滚，优化历史经 self_optimize_journal 沉淀。"
export const tools: ToolSet = { read_feedback, run_tests, rollback, journal, backlog, page_capture }
export const requiresApproval = { run_tests: true, rollback: true }
export const preload = false          // 按需装载，非默认注入
export const dependencies = ["code", "vision"]  // 依赖自动装载：连带装载 code（编码工作流复用）与 vision（视觉能力 vision_* 复用）
export const writeGuard = (env, absPaths) => string | null   // 写范围守卫声明（见下「写范围守卫」）
export const projectRoot = (env) => string | undefined        // 默认项目根兜底（见下「项目名称与项目根」）
```

- **复用 code 与全局工具（两种路径同规则，`dependencies` 声明驱动级联）**：
  - **装载模式**：装载 `self_optimize` 时 `SubAgentManager.load` **连带装载 `code`**（幂等，WS `sub_agent.load`/`agent_load`/预加载所有装载路径均生效）——code 完整提示词写入会话记录；文件读写查询用**全局工具**（read/write/edit/patch/grep/sh 等，带 project 参数可按名操作预置项目），分析/验证类操作用 code 独有工具（`code_search_symbols`/`code_analyze`/`code_git`/`code_preview_server` 等 `code_*` 前缀）
  - **子会话运行（`subsession_run`）**：`normalizeRunAgents` 依赖展开**自动连带预加载 `code`**（code 前置，系统提示词含两段职责分隔头——code 的通用工作流在前，self_optimize 的特有流程与约束在后），子会话工具集为**继承的全局工具** + `code_*` 独有工具 + `self_optimize_*` 独有工具（全局工具默认继承，见「子会话运行的上下文隔离」）
  - 提示词分层：self_optimize 静态提示词**只承载自我优化特有内容**（反馈输入、写范围、**三类子Agent 开发场景与目录**、设计同步铁律+产物纯净、测试准入/回滚、用户验证、git 收尾），通用工作流以「直接遵循 code 子Agent 提示词」引用（两种路径下 code 提示词均在上下文内）——符合「子Agent 静态提示词不复刻其他子Agent 内容」的分层原则
- **项目名称与项目根（内置）**：静态提示词**内置项目名称「歌白（GEBAI Agent）」**并指明「项目根以系统提示词动态注记『项目根:』为准」——具体目录由引擎按绑定动态注入，**装载与子会话两种形态均注入**（装载段落 `loadAgentsForSession` 与 `subsession_run` 的 `buildAgentSection` workNote 同款语义；装载形态附相对路径限定语）；项目根解析三态：`SELF_OPTIMIZE_PROJECT` 环境变量 > dev 模式 `projectRoot` 兜底（按模块路径自动推导源码仓库根，与写范围守卫/`run_tests`/`rollback` 同源）> 二进制模式无兜底（按用户给定路径处理）——**dev 模式无需任何配置提示词即携带仓库根目录**，绑定同时使 subsession_run 子会话以仓库根为工作目录、自动注入仓库 AGENTS.md（沙箱模式同规则拒绝，回退工作目录）
- **反馈读取（`self_optimize_read_feedback`）**：自全局工具集下沉（全局不再注册 `read_feedback`，自我优化为唯一消费方）——按用户反馈按时间倒序读取，声明进 def 同时覆盖装载模式（`self_optimize_read_feedback` 命名空间）与子会话运行环境（全局工具不在注册表，def 声明保证可用）；反馈是自我优化的核心输入通道
- **页面捕获（`page_capture`）**：仿 show 图表分支的前端配合链路——引擎发布 `event.capture.request`（含 captureId + fullPage + delay）→ 前端捕获**响应页面的整个文档**（`documentElement`，不指定元素/区域；同一浏览器打开多个页面或用户切到其它视图（如文件工作台）时，即该页面当时的渲染——验证前先确认页面处于目标视图；渲染后 DOM html 截断 300KB + modern-screenshot 截图，png/jpeg，体积压缩 ≤2MB；fullPage=true 截整页，高度上限 12000px，缺省视口；**delay 为捕获前等待毫秒数**（UI 操作/动画/异步渲染完成后截图，上限 10 秒，前端 sleep 后统一捕获 html 与截图））经 WS `capture.result` 回传 → 服务端落盘会话 `tmp/capture/`（`page-<ts>.html` + `page-<ts>.png|jpg`）并返回文件/图片内容块；模型用 `read`（code_*）读取实际渲染 html，截图主模型多模态时用 `read` 直接查看（图片内联进上下文）、否则用 `vision_analyze`（vision 子代理，agent_load 装载或依赖连带装载）分析（结果文案按能力分流引导）——**UI 修改后模型直接看到真实渲染效果**（dev 模式修改后自动热更新，捕获前提示用户刷新页面；30 秒超时返回失败提示）
- **截图的前端成本控制（长会话不卡死的关键）**：modern-screenshot 逐节点算样式并克隆，成本 ≈ 0.5ms/节点——自页面根（`documentElement`）无过滤截图时，长会话（数千至上万节点）会把**主线程阻塞数秒**（实测 1500 条消息 / 7508 节点阻塞 3.9s，用户表现为「一用就卡死」）。三层防护：① **可见区域过滤**（`makeCaptureFilter`：矩形完全在捕获区域外——上方已滚出或下方未进入——的子树直接排除，长历史里绝大多数消息因此不入渲染）；② **节点预算**（`CAPTURE_MAX_NODES = 3000`，用尽即不再纳入，为整页模式 + 超长历史的硬兜底）；③ **超时兜底**（`CAPTURE_SCREENSHOT_TIMEOUT_MS = 15s`，截图内部不可取消，超时即按截图失败返回 html，不让工具侧无限等待）。实测整页/视口两种模式在 1.5 万节点下均 < 200ms 阻塞（修复前 7508 节点即 3.9s）；html 不参与截图节点预算（自身按 300KB 截断首部，超长页面只有前部）；截图前 `requestAnimationFrame` 让出一帧，避免在掉帧时刻叠加长任务
- **视觉分析（依赖 `vision` 子代理，`dependencies` 声明复用——方式一）**：def 声明依赖后 `vision_analyze`（语义分析截图）/`vision_ocr`（读图文字）随装载/预加载自动可用——`subsession_run` 隔离子会话即使不继承全局工具也有视觉能力；analyze 的 provider 解析与全局基建同一源（组装层 `boot/compose.ts` 注册 `setVisionProviderGetter`，`GEBAI_VISION_*` 外挂视觉模型 → 多模态主模型回落——`makeVisionTool` 工厂单源）；全局 vision 工具已移除，视觉分析统一经此通道
- **写范围守卫（`SubAgentDef.writeGuard`，代码级强制而非仅提示词）**：def 声明 `writeGuard(env, absPaths)`，引擎注入 `ToolContext.writeGuard`——文件写类工具（`write`/`edit`/`patch`/`file`（rename/move/delete 动作，move/rename 校验源与目标两路径））写入前以**解析后的绝对路径**调用，返回非空字符串即拒绝（作为工具结果返回引导模型调整，不抛错不落盘）。**装载模式按会话装载名单动态收集**（`sessionWriteGuard`：调用时点读会话 `loadedSubAgents`，任务中途 `agent_load` 装载后立即生效）、**子会话形态按预加载名单静态组合**（`defsWriteGuard`）——两个路径一致生效（旧实现仅子会话路径有守卫，装载路径因工具去重丢失守卫副本，现已修复）。政策内容：**默认只读模式仅允许写入 子Agent 扩展面（内置域 `packages/agents/src/`、二开域 `custom/`、客卿域 `keqing/`——三域即「沉淀为子Agent」的全部落点）与仓库级文档/配置（`DESIGN.md`/`AGENTS.md`/`AGENT.md`/`.env.example`/`README.md`）**，核心引擎源码（`core/`/`app`/`ws` 等）拒绝写入（返回拒绝说明引导改用子Agent 扩展或开启开关）；`GEBAI_SELF_MODIFY=true`（启动级环境变量）放开到仓库内任意路径；仓库根解析：`SELF_OPTIMIZE_PROJECT` 优先，dev 模式按模块路径推导，二进制模式必须显式配置；**守卫只保护歌白仓库**——仓库根之外的常规写入（会话 `tmp/` 产物等）不受限（守卫目的是保护服务端源码，不约束无关产物）。**边界（脚本通道不拦）**：守卫拦截的是文件写类工具，`sh`/`py` 脚本内的重定向/写文件不经守卫——防线为「sh 写类命令默认需审批（免审白名单仅只读/测试类，见「工具审批」）+ 提示词明令禁止经脚本写仓库文件」；`GEBAI_APPROVAL_SKIP=true` 跳过审批时仅剩提示词约束（操作者自担）
- **测试准入 + 回滚工具**：`run_tests`（在仓库根执行验证，需审批）——**`checks` 参数选择检查项**（`["test"]` 缺省：`bun test` 指定文件（files 相对仓库根；**同属一个包时自动切到该包目录执行**——`bunfig.toml` 的 md loader 与测试环境净化只在包目录生效，否则 md 被按 HTML 导入、仓库根 `.env` 注入测试进程）或 `bun run test` 全量；`["test","typecheck","lint"]` 三件套与 AGENTS.md 提交准入一致，**一次审批跑全**，按序执行首项失败即停，超时 10 分钟）；输出**始终合并 stdout+stderr**（bun test 在 Windows 把用例明细/汇总写 stderr（exit 0 亦然），只取 stdout 会丢失「跑了哪些用例、几个 pass」——准入判定看 exit code，明细供人核验）；`rollback`（恢复被修改的 tracked 文件 + **删除新建的 untracked 文件**，需审批）——新文件是自我修改的主要产物（如新建子Agent），`git checkout` 只恢复 tracked、残留会被热加载注册为破损 Agent，故先 `git clean -nd` dry-run 列出将删除的新建文件（输出如实展示）再 `git clean -fd` 清理；checkout 对新建文件本就无可恢复（pathspec 不匹配属预期，不报错）；两者 files/paths 路径参数**校验前置**（含引号/shell 元字符/百分号的条目直接拒绝——审批界面展示的是参数而非拼好的命令，不设防的拼接会把注入带过审批门），合法条目双引号包裹拼入命令（空格路径保持单参数）；文件写入经全局 write/edit/patch（默认免审批，受防盲写守卫约束），sh/py 走工具自身动态审批（默认需审批、`approval` 参数按次免审，见「工具审批」）
- **优化日志（`self_optimize_journal`，跨会话优化记忆）**：`append` 记录一次优化（title 必填 + changes 改动清单 + verification 验证方式与结果 + outcome applied/reverted/failed + lessons 经验教训）、`list` 读最近记录（limit 默认 10，新→旧）；**action 必传**——漏传（或带 append 专属参数却传 list）时显式报错且不写盘，不静默按查询处理（缺省当查询会让「想记录」的调用拿到一份正常列表而记录丢失）——**变更管理的补丁记录落地**：git 历史只记代码变更，journal 补「为什么改 + 验证结果 + 教训」，后续优化任务开工先查历史不重复踩坑（提示词固定引导：开工 list、收尾 append）；存储 `users/{user}/self-optimize-journal.json`（与 ws-journal 同位的 gitignored 运行时数据），环形保留最近 100 条，损坏/首次从空开始
- **待优化暂存清单（`self_optimize_backlog`，离线优化）**：任务执行中因自身知识/工具不足或错误导致重复试错、低效，而不便中断当前任务深入优化时 `add` 暂存（problem 问题现象必填 + direction 优化方向；session_id 缺省自动记当前会话，供后续回溯完整上下文）；**action 必传**，漏传或与 add 专属参数冲突时显式报错且不做变更——**优化时机后移、证据先落盘，不打断当前任务；暂存后 ask 向用户确认处理时机（当场修复 / 留待后续集中全面优化）**；后续执行全面优化时 `list` 取待优化项清单（旧→新）按主题归并逐项处理（需更多上下文可读来源会话记录 `users/{user}/sessions/{ID前2位}/{第3-4位}/{会话ID}/chat.json`——本地模式可读，沙箱部署模式会话文件不可读时以暂存文本为准），完成后 `resolve ids=[…]` 移除（优化记录本身仍由 journal append 承载）；存储 `users/{user}/self-optimize-backlog.json`（与 journal 同位的 gitignored 运行时数据；**add/resolve 与 journal append 的读-改-写经进程内互斥串行化**——两者的写入均为「读全量 → 改 → 整文件覆盖」，并发调用（同一回复的多个工具调用、js 编排并行）各自读到同一旧值再写回，后写会静默覆盖前写而丢条目），解决即移除、**不设环形上限**（暂存项是待办不是历史，静默淘汰会丢待办）；触发引导内置于 def description——总Agent 任务执行中重复试错低效时装载 self_optimize 暂存或直接优化
- **验证服务（`preview_server`，经 code 命名空间 `code_preview_server` 使用）**：临时新端口独立进程（不中断当前会话），用于服务端功能类修改的验证；UI/前端类修改优先走 `page_capture` 当前页面验证
- **工作流**：通用段（规划→探索→定位→方案→修改→验证→收尾）直接遵循 code 提示词（探索段 grep/analyze/search_symbols 分工、修改段 patch 优先等见「code」章节）；特有段：反馈输入（`self_optimize_read_feedback`）+ 开工查优化日志（`self_optimize_journal list` 相关历史与教训）与待优化清单（`self_optimize_backlog list`——有积压且本次目标就是优化时以此为工作清单）+ 离线优化（任务执行中重复试错低效不便立即优化的先 `backlog add` 暂存，暂存后 ask 用户确认当场修复或留待后续，后续集中全面优化）+ **开发场景与目录**（三类子Agent 扩展的落点与选型：内置域 `packages/agents/src/agents/{name}/`、二开域 `custom/agents/{name}/`（同名覆盖内置、可整体迁移）、客卿域 `keqing/{lang}/{name}/`（manifest + PROMPT.md + 驱动，跨语言同名合并，能力外置·权力内守）——放置即发现、改完立即验证注册）→ **设计同步铁律**（任何行为/接口/协议/存储布局/常量/命名规则等设计层面变更，必须同步更新 `DESIGN.md` 对应章节；**产物纯净**——写出的代码/子Agent 提示词/文档只描述当前完整的能力与限制，不留「何时发现/修复了什么问题」等历史注记（变更缘由归 git 提交说明与 journal，遇既有注记顺手清除））→ 修改（范围由写范围守卫代码级强制；**仓库写入一律用 write/edit/patch 文件工具，禁止经 sh/py 脚本绕过守卫**；新建/修改子Agent 后立即验证注册——失败错误附带载原因）→ 验证（`run_tests` 测试准入，确认后 checks 三件套，失败 `rollback` 回滚（恢复修改 + 删除新建文件））→ 用户验证（UI 类 `page_capture`、服务端类 `preview_server`）→ 收尾（`git` 只读核对、不擅自 commit、`self_optimize_journal append` 记录、解决了待优化项的 `self_optimize_backlog resolve` 移除、总结先结论后细节）
- **与 `code` 的差异**：
  - 工作区：服务端源码（`packages/agents/src/agents/` 等，`GEBAI_SELF_MODIFY` 开启时更宽） vs 会话 `tmp/` 外部项目
  - 审批：自我优化改动影响所有用户，默认更严格（改动子Agent 定义即影响全局能力）
  - 预加载：`preload = false`，避免普通对话中总Agent 误用自我修改能力
- **协作边界**：`self_optimize` 可经 `subsession_run` 委托 `playwright` 做外部 URL（如 `preview_server` 页面）的浏览器验证；`code` 不得反向委托 `self_optimize`（防经子Agent 链间接获得服务端修改能力），**写权限仅限服务端允许范围**

- **改进点来源**：用户反馈（见「用户反馈」）、审批拒绝原因、工具执行失败、任务超时/中断日志、用户显式指令（如「你下次不要再…」）、任务执行中自身重复试错（知识/工具不足或错误导致的低效——description 引导总Agent 装载 self_optimize 直接优化，不便立即优化的先 `self_optimize_backlog add` 暂存并 ask 用户确认当场修复或后续集中处理）
- **变更管理**：每次自我修改经 `self_optimize_journal` 生成补丁记录（title/changes/verification/outcome/lessons，`users/{user}/self-optimize-journal.json` 环形 100 条，可 list 查看）；回滚走 `rollback` 工具（恢复修改 + 删除新建文件）；代码版本控制（git）即代码层优化历史，journal 补「为什么改 + 验证结果 + 教训」的决策层历史
- **生效方式**：
  - 脚本调试模式：**子Agent 定义热加载**（见「子Agent 热加载」）——新增/修改/删除子Agent 文件在下一次装载/新任务前即时生效（self_optimize 改完子 Agent 后当会话内即可 `subsession_run` 验证成果）；核心引擎源码修改仍需重启进程生效
  - 二进制模式：无源码目录，热加载不适用（bundle 注册表不可变）；修改后的代码进入下次构建；运行期经会话级环境变量可覆盖**可配置项**（模型/路由/CV 路径/`{AGENT}_*` 等）实现即时调优——**子Agent 静态提示词不在可覆盖范围内**（随 bundle 固化，改动须重新构建）
- **安全约束**：自我修改走统一审批流（写操作逐次审批）；修改范围由**写保护闸门代码级强制**——默认只读模式仅限子Agent 扩展面（内置域 `packages/agents/src/`、二开域 `custom/`、客卿域 `keqing/`）与仓库级文档/配置，核心引擎源码默认只读（`GEBAI_SELF_MODIFY=true` 显式开启，见「写保护闸门」）
- **测试门槛**：任何自我修改必须通过相关测试（`run_tests`）才能视为完成，防止退化；测试失败用 `rollback` 回滚本次改动（测试策略与覆盖率门槛见「测试策略」）
- **用户验证**：修改通过测试后，`self_optimize` 用 `ask` 询问用户是否启动验证服务（`preview_server`，临时新端口独立进程，不中断当前会话），确认后启动并告知访问 URL；验证结束（或用户拒绝）即停止，避免残留进程

### 会话管理
- 会话按用户持久化到 `{GEBAI_HOME}/users/{user}/sessions/{s0}/{s1}/{session_id}/chat.json`（`{s0}`/`{s1}` 为会话 ID 自身 hex 前缀分片——前 2 位/第 3-4 位，肉眼可从 ID 推路径，见目录结构）
- **会话记录保存子Agent 装载状态**：`loadedSubAgents` 字段（已装载名单）+ `loadedAgent` 标记的 system 消息（完整提示词，UI 渲染为简短装载提示）；**会话级装载痕迹以本会话记录为准（与进程级工具注册解耦，见「装载（模块）」）**；恢复历史会话时引擎按 `loadedSubAgents ∪ 装载提示词痕迹 ∪（仅 `undefined` 的新会话/旧格式才落到）启动预载名单` 重新注册工具并补齐提示词与名单（`ensureSessionAgents`，幂等）——**进程重启后装载状态由此自动恢复，无需模型重新 `agent_load`**；卸载到空保留 `[]`（区别于「从未初始化」的 `undefined`），卸载不会因重启被预载名单复活；新会话首次运行按启动预载名单（`GEBAI_PRELOAD_SUB_AGENTS`）初始化，未配置默认不预载任何子Agent
- 会话归属校验：仅会话所有者可访问（服务模式）
- 列表查询不为元信息解析正文：走同目录 `meta.json` 指纹缓存（见下条），`chat.json` 仅在缓存缺失/指纹不符时解析
- **列表元信息缓存（`meta.json`）**：会话列表只消费标题/时间/置顶/上下文用量，若每查询都解析 `chat.json` 全文，开销随历史体量正相关（数十个会话可达数十 MB）。与 `chat.json` 同目录维护 `meta.json`（`id`/`name`/`userId`/`createdAt`/`updatedAt`/`pinned`/`ctxCachedTokens`/`ctxTokensFallback`/`messageCount` + 正文指纹 `source: {size, mtimeMs}`），`save()` 同步刷新；`listSessionInfos()` 命中缓存则直接返回，**文件缺失/损坏/指纹不符（陈旧）时回退读正文并就地重建**——**可重建的缓存、不是真相源**（`chat.json` 恒为准，外部编辑/旧版本写入/中途崩溃均自动纠正），存量会话首次列表自动建立；`listSessions()`（返回完整 `SessionData`）保留给需要正文的调用方（GC/归档等），列表类消费方（WS/REST 列表、快照、文件工作台目录选择、飞书会话命令）走 `listSessionInfos()`
- 支持会话创建、切换、重命名、置顶、删除
- **会话置顶**：`pinned` 字段（chat.json 持久化，未定义 = 未置顶，旧格式天然兼容）标记重要会话；列表查询置顶优先、组内按更新时间倒序；置顶/取消为元数据操作，**不刷新 `updatedAt`**（不动排序基线，旧会话置顶不会跳入时间分组）；前端「置顶」独立分组脱离时间分组展示
- 支持会话级的审批跳过（`/approval-skip`）

### 用户反馈

用户可对任意 Agent 回复（总Agent/子Agent 消息）提交反馈，用于质量追踪与改进：

- **反馈类型**：
  - **点赞/点踩**：对单条回复快速评价，前端弹层附**可选原因标签**（👍 优秀/有用/完整，👎 错误/不完整/不符合预期）与**补充说明文字**
  - **文字反馈**：附带详细说明，如指出错误、期望的修正、补充背景（类型保留，经 API 提交）
  - **建议改进**：对功能/产品层面的建议（新工具、子Agent 需求等）（类型保留，经 API 提交）
- **关联上下文**：反馈自动关联所属会话与消息 ID；落盘时服务端按会话记录**自动补 model**（assistant 消息落盘携带的模型名，见「消息模型与数据结构」）与 **subAgent**（会话当前装载的子Agent 名单，逗号连接）——尽力关联不阻断提交（会话不存在/读取异常时反馈照常写入，仅缺关联字段）
- **存储**：持久化到 `{GEBAI_HOME}/users/{user}/feedback/YYYY-MM-DD/{h0}/{h1}/{feedback_id}.json`（按日期 + ID 自身前缀分片，见目录结构），仅本人可见（服务模式）
- **前端**：助手消息操作组内 👍/👎 按钮（与复制/撤回/重新生成同排）点击打开弹层（标签单选可反选 + 补充说明，提交成功禁用防重复、失败恢复可重试）；管理员可在设置面板「反馈」页查看（含模型/子Agent 关联信息）并**导出 JSON**
- **用途**：反馈数据供改进系统提示词、评估模型表现、排查工具问题（`self_optimize_read_feedback` 读取进入自我优化闭环，见「自我优化」）；管理员可导出分析

### 环境变量配置

所有配置统一采用**环境变量**形式（`GEBAI_*` / `OPENAI_*` / 子Agent `{AGENT_NAME_UPPER}_*` 等），无独立配置文件（`.env` 仅是环境变量的文件来源：脚本调试=仓库根、二进制=`{GEBAI_HOME}/.env`，真实环境变量恒优先）。**用户/会话环境变量服务端零留存**（不落任何 env 文件），层级与存储：

| 层级 | 来源 | 存储 | 说明 |
|------|------|------|------|
| 全局 | 进程环境变量（启动时注入，`.env`/系统环境） | 服务端启动配置 | 服务端默认配置，所有用户/会话共享 |
| 浏览器本地 | 前端设置面板 | **浏览器 localStorage（`gebai.ui.env`）——用户环境变量唯一持久化位置** | 随每条 prompt 临时注入为任务级覆盖，清除站点数据即清除 |
| 会话内存态 | env 接口 / 飞书命令运行中设置 | **服务端内存（不落盘，进程重启即空）** | 仅供运行中即时生效类开关（如自动审批）；由前端每次加载会话自行重新同步所需键 |

**admin 密码引导**：服务模式不设注册表引导（**不落明文密码**）——启动参数 `GEBAI_ADMIN_PASSWORD_HASH="salt:hash"`（scrypt 加盐哈希，`bun run --cwd packages/server hash-password` 生成）**设置则启用 admin（覆盖其哈希），不设置则 admin 被禁用**——admin 唯一入口即此参数（不可注册创建）；admin 禁用不影响普通用户（登录页可自助注册，注册用户恒为普通角色）。

#### 前端环境变量（浏览器本地，不保存到服务端）

- **前端设置面板配置的环境变量保存在浏览器本地（localStorage，键 `gebai.ui.env`）**，对本浏览器所有会话生效，不是每个会话单独配置，**不写入服务端**（用户环境变量服务端零留存——不落任何 env 文件）——敏感配置（密钥等）只存在用户自己的浏览器，防服务端侧泄露
- 发送消息时由前端随 `POST /api/v1/sessions/:id/prompt` 请求（body `env`）/ WS `session.prompt`（payload `env`）**临时注入**服务端，仅本次任务生效（引擎合并进本次运行的 env），请求结束即失效，服务端不持久化
- **前端配置模型相关变量即可使用模型**（服务端可不配置任何模型变量）：`GEBAI_LLM_MODEL`（主模型）/`GEBAI_VISION_MODEL`（视觉模型）等 `GEBAI_LLM_*`/`GEBAI_VISION_*` 注入后按任务生效——任务启动时按合并后 env 重建 Provider 覆盖服务端启动配置（见「覆盖规则」），未配置的项沿用服务端启动配置
- 名称与权限宽容过滤：prompt 的 `env`（浏览器本地注入通道）对不支持/非法的变量**直接跳过、不拒绝任务**（`filterEnvInjection`：丢弃非法标识符名/`__proto__`/非 string 值）——前端 localStorage 可能残留旧版目录外键，拒绝整个任务会阻断正常使用；显式管理通道（REST `PUT /env`、WS `session.env.set`）仍严格校验（`validateEnvVars` 非法返回 400/错误应答）
- **目录驱动、不可自定义**：前端设置面板从服务端 `GET /api/v1/env/catalog` 拉取环境变量目录（白名单，含变量作用说明），**按「全局 / 各子Agent」分组展示**（全局：模型相关 `GEBAI_LLM_*`/`GEBAI_VISION_*`、审批跳过、代理/时区等；子Agent 组：`{AGENT_NAME_UPPER}_*` 前缀变量如 `CODE_PROJECTS`/`SELF_OPTIMIZE_PROJECT`/`FEISHU_DOCS_APP_ID`）；**子Agent 组由各子Agent 导出 `envVars` 声明汇总**（`core/env-catalog.ts` 不做子Agent 硬编码，新增子Agent 环境变量只需在子Agent 定义中声明，见「子Agent文件格式」）；**未配置的项显示为空、请求不携带**；鼠标悬停（tip）显示变量作用
- **不支持自定义变量名**：只能配置目录内的项（保存时过滤目录外旧值）；启动级与安全敏感变量（`GEBAI_MODE`、`GEBAI_ADMIN_PASSWORD_HASH`、`GEBAI_SAFE_MODE`、`GEBAI_SANDBOX`、`GEBAI_HOST/PORT` 等）不在目录中，天然不可配置
- 清除浏览器站点数据（localStorage）即清除该配置

#### ask 填值分支（模型驱动的前端填值，原 ask_env）

- **工具缺少必需环境变量时报错给模型**（错误信息含缺失变量名、用途说明与可选的 `ask` 调用引导，如 feishu_docs 缺 `FEISHU_DOCS_APP_ID` 时提示「可调用 ask 工具（name=FEISHU_DOCS_APP_ID，secret=true）请求用户直接填写」）
- **`ask` 填值分支**：模型调用（`name` 变量名 + `description` 用途说明 + `secret` 敏感值掩码）→ 引擎发布 `event.env.request`（含 envId/name/description/secret）并**阻塞等待** → 前端渲染**填值卡片**（变量名与说明展示，secret 时密码框）→ 用户提交后：
  - **提交成功填值卡片随即关闭**（取消/拒绝同样关闭）；提交失败卡片保留并提示重试
  - 值**注入本次任务 env**（任务级 env 引用原地更新，`sh`/`py` 子进程与后续工具读取**立即生效**，不进入模型上下文——密钥不外泄给模型）
  - **变量名校验与敏感键限制**：与其余 env 写入通道同规则（`validateEnvVars`：仅 `[A-Za-z_][A-Za-z0-9_]*` 标识符名、拒绝 `__proto__` 原型污染），**服务模式下一律拒绝 `GEBAI_APPROVAL_SKIP`**（ask 填值分支是模型驱动的第四通道，不得自设审批跳过——用户本人经前端开关/env 接口/飞书命令设置）
  - 同时**保存到浏览器本地**（localStorage，后续任务自动生效）
  - 用户拒绝/超时（审批超时同值）返回失败，工具结果引导模型说明所需配置或改用其他方式
- 填值分支仅实时前端通道（合并型工具分支门控：非 realtime 明确报错并引导用户在设置面板配置）；值与其余 env 写入同规则校验（`validateEnvVars`）

#### 会话内存态环境变量（内部机制 + API）

- 会话 env 为**纯内存态**（服务端零留存）：不落盘、进程重启即空，前端每次加载会话**与每次任务启动前**自行重新同步所需键（如自动审批开关——草稿页首条消息创建的会话不经过消息加载路径，同步点覆盖两处，防子会话首个任务的开关失效）
- 会话级修改**不通过前端设置面板**（面板存浏览器本地），仅经 API（REST `PUT /api/v1/sessions/:id/env` / WS `session.env.set`，内存写入）或内部机制（如 `approval-skip` 自动审批开关按会话同步 `GEBAI_APPROVAL_SKIP`）；**写入响应与读取一致脱敏**（敏感键值以掩码返回，防明文密钥回读）
- 作用范围：会话内 LLM 调用（模型/Provider 配置，任务级生效）、工具执行（`sh`/`py` 子进程环境）、子Agent 环境变量读取，均与浏览器本地注入合并后生效
- 用途：按会话定制（`CODE_PROJECT`/`SELF_OPTIMIZE_PROJECT` 项目绑定、`CODE_PROJECTS` 预置项目注册表、预加载集合覆盖等；服务重启后此类配置由用户浏览器本地随 prompt 重新注入恢复）
- **历史数据清理**：启动时清理遗留的用户级 `users/{user}/env.json`（`cleanupLegacyUserEnv`）；会话目录遗留的 `env.json` 在该会话首次触达 env 读取时惰性删除——迁移后服务端不留存任何 env 文件
- **环境变量目录接口**：`GET /api/v1/env/catalog` 返回可配置变量白名单（按「全局 / 各子Agent」分组 + 变量作用说明），供前端设置面板渲染（不可自定义变量名）；启动级与安全敏感变量不在目录中

#### 覆盖规则

- 生效顺序：**浏览器本地注入（本次任务） > 会话内存态 > 全局**，同名字段取最高优先级的非空值（前端 localStorage 注入仅覆盖当前运行的任务，不修改任何持久化层级）
- **模型相关配置（`GEBAI_LLM_*` 全套与 `GEBAI_VISION_*`）任务级生效**：浏览器本地/会话内存态注入在任务启动时按合并后 env 重建 Provider（`applyModelEnvOverrides`/`resolveVisionProvider`），覆盖 Provider 级（进程环境变量）配置——主循环与 `subsession_run` 子会话运行、上下文压缩阈值/摘要、附件图片内联判定、视觉分析均生效；无覆盖键时沿用启动 Provider 实例；非法值（API_KIND 非三类/MAX_CONTEXT 非正数）忽略回退
- 会话内存态删除某变量 = 恢复为全局的值
- 修改环境变量（任一来源）后，当前正在运行的任务不受影响，新任务使用新值（运行中的会话内存态开关除外——自动审批等实时判定类按次读取）
- 敏感变量（含密钥的 `*_KEY` / `*_TOKEN` 等）在服务端 API/UI 中脱敏显示，仅可覆盖不可回读明文；浏览器本地存储的值存于用户自己的浏览器，按明文编辑

### 会话临时文件查看与下载

会话 `tmp/` 中的文件（Agent 产出、脚本输出、截断内容引用等）支持在 UI 中查看与下载：

- **文件列表**：**经文件工作台浏览**（聊天页无独立会话文件面板）——工作台暴露 `sess:<sessionId>` 根指向该会话 `tmp/` 子树（WS `session.files.list` / REST `/api/v1/sessions/:id/files` 提供数据，SDK 有对应方法）
- **查看**：文本文件（文本/JSON/代码）内嵌预览——消息流文件卡文本上限 **40,000 字符**（超出截断提示），工作台文本读取上限为 `GEBAI_FS_MAX_READ`（默认 10MB）；二进制文件（图片等）可预览，其余提示下载
- **下载**：单文件下载、多选打包下载（zip）；通过 REST 下载端点返回原文件（`Content-Disposition` 指定文件名）
- **安全边界**：文件操作严格限定在会话 `tmp/` 内，路径解析复用路径沙箱（拒绝 `../`、绝对路径、符号链接），仅会话所有者可访问；**列表仅暴露 `tmp/` 子树**（`chat.json` 等会话数据文件不列出），REST/WS 文件接口的路径解析统一以 `tmp/` 为根并兼容 `tmp/` 前缀（旧附件/截断引用路径）
- **文件预览（`files/preview`，文件卡/文件链接取数入口）**：`read`/`write` 产物 file 块的路径为**服务端解析后的真实路径**（工具执行时按会话 `tmp/` 真实绝对路径（`sessionPath` 拼接——项目绑定工具的 workdir 是项目根不能作判定依据）归属判定：会话内 → `tmp/` 逻辑路径，code 项目文件（project 参数/预置项目解析后）→ 绝对路径），前端「文件展示方式=弹窗查看」时产物 file 块收敛为**文件链接 chip**（点击弹窗查看）、嵌入模式下文件内容卡取数同样可用（原始参数路径在项目工具下无法由 files 接口解析的 404 缺陷由此修复）；取数统一走 `GET /sessions/:id/files/preview?path=`：**相对路径以会话 `tmp/` 为根**（与 content 同规则），**绝对路径按用户隔离边界放行**——沙箱用户仅允许本用户数据目录（`users/{user}/`，与文件工具 project 参数经 `resolveInSandbox(root=users/{user})` 的可达范围一致，含符号链接逃逸检查），非沙箱（本地模式操作者本人，与文件工具能力对齐）放开；`?download=1` 以附件形式返回（文件卡/chip 的下载入口），前端不猜测路径解析
- **与截断内容联动**：上下文保护落盘的截断文件也可在 UI 中直接查看/下载
- **Office 阅读视图（`?render=office`）**：docx/xlsx/xlsm/pptx 的文件卡/弹窗内联渲染——前端按 office 类型在 preview URL 上追加 `render=office`，服务端经 wps 子Agent 的读取模型输出结构化 HTML（沙箱 iframe 承载，详见「`wps`（Office 文档处理）」章节「阅读视图预览」条目）；非 office 扩展名或损坏文件返回 422，前端回退二进制占位与下载引导
- **用途**：用户随时检视 Agent 工作产物（生成的报告、脚本、数据文件），无需进入文件系统

#### 接口

- WS：`session.files.list` / `session.files.get`
- REST：`GET /api/v1/sessions/:id/files`、`/files/content`（?path=）、`/files/download`（?path= 单文件 / POST body {paths} 多选 zip 打包）、`/files/preview`（?path= &download= &render=office 文件预览：会话相对/项目绝对路径统一入口，点击弹窗查看用；render=office 返回 docx/xlsx/xlsm/pptx 阅读视图 HTML）
- SDK：`listSessionFiles(sessionId)` / `readSessionFile(sessionId, path)` / `downloadSessionFile(sessionId, path)` / `downloadFilesZip(sessionId, paths)`

### 文件工作台（`/files`）

面向「彻底摆脱 VSCode」的一套自足文件工作台：**独立页面 + 独立路径**（`/files`，Vite 多入口 `files.html`），入口在聊天页标题栏**会话列表按钮右边**（主按钮＝并列，悬浮翻出的副按钮＝**全屏**（右箭头）；文件工作台那一侧活动栏最下方一对：**关闭文件工作台**（左箭头）/ **进入分屏**——文件工作台与会话工作台是同一个窗口里的两个工作台）。面向用户本人直操（不走工具审批，但写操作落审计）；让 Agent 去改仍走工具审批链路。设计稿与实现说明：`docs/file-workbench-design.md`、`docs/file-workbench-implementation.md`。

- **键盘快捷键**：与主界面共用同一套机制（`../keymap.ts` 分发器 + `files/keymap-wb.ts` 建表）——`Ctrl+S`（保存）、`Ctrl+P`（快速打开，VSCode 式模糊搜文件名 + 最近打开）、`Ctrl+Shift+O`（转到符号）、`Alt+W`（关标签）、`Ctrl+E`（查看↔编辑）、`Ctrl+F`（目录过滤）、`Ctrl+B`（左栏）、`Ctrl+Shift+E/F/G`（资源管理器/搜索/变更）、`Ctrl+K`（更多菜单）、`Ctrl+Shift+D`（比较）、``Ctrl+` ``（终端）、`F5`（刷新）、`Alt+G`（Git 工具窗）、`F7`/`Shift+F7`（差异块）、`F8`/`Shift+F8`（变更文件 / 合并冲突）、`Alt+M`（标记为解决）、`F2`（重命名）、`Ctrl+C`/`Ctrl+V`（资源管理器：复制选中项 / 粘贴到选中目录）、`Alt+Z`、`Esc`；终端面板内 `Ctrl+Shift+C/V`（复制/粘贴）、`Ctrl+F`（搜索）、`Ctrl+=/-/0`（字号）、`Ctrl+C`（中断）、`Ctrl+L`（清屏，降级终端）。接管浏览器默认的键位一律走**捕获阶段**（抢在 Monaco / xterm 之前，也避开 Monaco 把 `Ctrl+K` 当多键前缀、`F7` 当 diffReview）；焦点在终端面板内时全局键一律让位，`Ctrl+W/P/E/K/B` 交还 shell 的 readline。`Ctrl+W` 是 Chromium 保留命令（页面收不到），只在桌面/app 形态生效，浏览器窗口里的误关由工作台 `beforeunload`（有未保存改动时确认）兜底。
- **编辑器字符宽度测量（字体就绪 / 像素比变化 / 自检校准）**：Monaco 只在 `editor.create` 时量一次字符宽度并长期缓存，光标位置、选区矩形与鼠标点击落点都按「列号 × 该宽度」算。编辑器字体是 `@font-face` + `font-display: swap` 的懒加载 woff2，测量若落在字体落地之前，量到的是**回退字体的前进宽度**（Windows 回退的 Consolas 0.55em 对 JetBrains Mono 0.6em，几十列就偏出一个字符），字体换上前后误差随列号累积且**不自愈**；分数像素比下逐字形取整是另一条同表现路径。`files/editor-metrics.ts` 三件事收口：① 建编辑器前 `ensureEditorFont()`——`fonts.check` 命中即零等待，未就绪则显式 `fonts.load` 并最多等 800ms，超时照常建编辑器（不阻塞打开文件）；② 度量同步 `createMetricsSync()` 在 `document.fonts` 的 `loadingdone`、`matchMedia('(resolution: Xdppx)')` 变化（窗口跨缩放显示器/改系统缩放）与首次布局后复检：先 `remeasureFonts()` 清陈旧缓存，像素比变化另 `layout()`；③ 复检口径是**缓存宽度 vs 实绘前进宽度**（量编辑器自己渲染的行，取不到用同款字体的探针 span）——重测后仍不一致才关掉等宽快速路径（`disableMonospaceOptimizations`，该路径正是「列号 × 缓存宽度」假定的来源）并留一条控制台诊断。编辑器与差异视图共用同一套（字体/字号常量同源），句柄销毁时同步器一并 `dispose()`。
- **快速打开（Ctrl+P，VSCode Quick Open 同款）**：`files/quick-open.ts`（面板与交互）+ `files/quick-open-core.ts`（模糊匹配与排序，纯函数、有单测）+ `files/recents.ts`（最近打开，localStorage 按根隔离）。索引一次取回（`GET /api/v1/fs/files` → `rg --files`，原生尊重 .gitignore；不可用回退内置遍历），之后逐键筛选全在前端；按根缓存 60s，收到 fs 变更事件即失效（新建/删除的文件立刻可搜到）。匹配强调三点：子序列（`smain` 命中 `src/main.ts`）、**文件名命中优先于目录名**、连续/词首/驼峰边界加分而跨段空隙扣分；同分按路径长度再按字典序，**顺序稳定**（键盘操作不能每次换位置）。空查询显示最近打开；`Enter` 开预览标签、`Ctrl+Enter` 固定常驻（与 VSCode 一致）；命中字符高亮（目录段与文件名共用一套下标）。

- **文件内符号跳转**：`F12` / `Ctrl+Click` 跳到光标处标识符**在当前文件内**的定义，`Ctrl+Shift+O` 列出文件符号。符号来源四级仲裁（**有语言服务器时用它的 `textDocument/documentSymbol`**——语义级且与跳转同源；其余按语言分工）：**TypeScript / JavaScript / JSON / CSS 系 / HTML 由 Monaco 内置语言服务**（本地 worker，非 LSP）提供——工作台的 model 就在它的脚本清单里（`getScriptFileNames` / `getNavigationTree` / `getDefinitionAtPosition` 实测正常），这类语言走编辑器自带能力；**有 tree-sitter 语法文件的 15 种语言（python/go/rust/c/cpp/java/kotlin/scala/swift/dart/ruby/php/lua/shell/elixir）走真语法树**；**其余 27 种（csharp/objc/groovy/perl/r/powershell/bat/vb/pascal/haskell/fsharp/clojure/erlang/sol/wgsl/asm/sql/yaml/ini/markdown/latex/graphql/protobuf/hcl/dockerfile/makefile/cmake）词法规则兜底**（也是语法树不可用时的回退）。Ctrl+Shift+O 在两种焦点环境下都可用：编辑器内直接走 Monaco 的大纲动作（`editor.action.quickOutline`），编辑器之外先判定语言归属——内置服务负责的语言把焦点交给编辑器再触发同一个动作，其余语言弹出工作台自己的符号面板（复用「快速打开」的 `.fw-qo-*` 外观与 ↑↓/Enter 语义，状态栏标出结果来自「语言服务器」「语法树」还是「词法规则」）。自研部分三件：`files/symbols-core.ts`（**词法级、纯函数、有单测**——逐行规则 + 作用域栈，注释/字符串/正则字面量先掩码成**等长空格**再匹配，行号列号因此与原文严格对齐）、`files/symbols-ts-rules.ts` + `files/symbols-ts.ts`（**tree-sitter 路径**——声明式节点映射表 + 懒加载运行时，输出同一种 `Sym` 结构）、`files/symbols-extract.ts`（调度：语法树优先、失败回退词法）、`files/symbols.ts`（Monaco 的 `DocumentSymbolProvider` / `DefinitionProvider` 桥，按 **model 版本号**缓存提取结果、FIFO 只留最近 8 个 model；provider 可异步）、`files/symbol-panel.ts`（工作台面板，先开先填）。词法分层三种口径（`LangSpec.scope`）：`brace`（花括号语言按行首 `{}` 深度）、`indent`（Python / YAML / Ruby 按缩进）、`level`（Markdown 标题 / INI 节 / LaTeX section 自带层级）；符号种类再做一次修正（类内函数 → `method`、与容器同名或 `__init__`/`__construct`/`initialize` → `constructor`）。两条路径的语言 id 均取自 `@gebai/sdk` 的 `file-language.ts`（**路径→语言的唯一真相**：服务端 `core/fs/mime.ts` 与前端差异/比较/合并视图都转发到它——早先前端另有一份手写表，导致 `x.mts`/`Cargo.toml`/`Dockerfile` 这类路径在编辑器里有语言、在差异视图里却是 `plaintext`）。并集（升上本机有语言服务器的语言，**再剔除 Monaco 内置语言服务覆盖的语言**）当文档符号 provider 的选择器；**定义跳转** provider 仍用「剔除了有 LSP 的语言」的集合（避免同一个跳转两份候选）。**有意不做的**：类型推断与重载解析（同名多定义按作用域就近 + 出现顺序给候选）、同一行的多个定义只取先命中的、跨文件跳转。
  - **资产与体积**：tree-sitter 运行时（`tree-sitter.js` 147KB + 核心 wasm 201KB）由 `build-vendor.ts` 拷入 `public/vendor/tree-sitter/`（与 monaco/xterm/d2 同一惯例：稳定文件名、静态伺服、离线可用）；**语言语法 wasm 不另存一份**——15 种语言原始体积约 25MB，放进 web 产物会被内嵌进二进制，改为新增静态路径 `/vendor/tree-sitter/lang/<grammar>.wasm` 从服务端**已内嵌的**语法集（分析器那份）按 `Accept-Encoding` 回源（gzip 后单语言 11KB～415KB），并 `Cache-Control: max-age` 允许浏览器长期缓存。语言表 `TREE_SITTER_GRAMMAR` 放在 `@gebai/sdk`（前后端共用一份真相，服务端据此做白名单、web 据以加载）。语言列表、映射细节与取舍见 `files/symbols-ts-rules.ts` 与 `docs/file-workbench-implementation.md` 的 5.34。
- **语言服务器（LSP）：有则用、没有不影响**：编辑器按文件语言在**本机 PATH** 上探测常驻语言服务器（`core/lsp/registry.ts` 内置表：`gopls`→go、`rust-analyzer`→rust、`clangd`→c/cpp/objective-c、`pyright-langserver`/`pylsp`→python，另有 lua / yaml / shell / kotlin / ruby / php / csharp / dart），探到即提供**补全 / 悬停 / 定义跳转 / 引用 / 重命名 / 格式化 / 签名帮助 / 诊断**；**探不到（未安装、`GEBAI_LSP=false`、沙箱非豁免用户）时清单为空，前端不注册任何 provider、不建连接**——Monaco 内置语言服务（TS/JS/JSON/CSS/HTML）与符号提取链路照旧。服务端 `core/lsp/`：`protocol.ts`（`Content-Length` 帧编解码，长度按**字节**计）、`session.ts`（单「根 × 服务器」进程：initialize 握手、文档同步、请求关联、**服务器反向请求兜底应答**——`workspace/configuration` 等不应答会卡住其初始化）、`service.ts`（会话池 + 文档归属 + 事件分发 + 空闲回收）、`ws-handlers/lsp.ts`（`lsp.open/change/save/request/close` + 推送 `lsp.notify/exit/log`）、`routes/lsp.ts`（清单端点）；前端 `files/lsp.ts` + `files/lsp-convert.ts` + `files/lsp-capabilities.ts`（能力协商判定，纯函数 + 单测）+ 通用 WS 请求客户端 `files/ws-client.ts`（终端与 LSP 共用）。三个设计取舍：① 前端**只认 `docId`**（不认识绝对路径）——`didOpen` 的 file uri 由服务端用 `rootAbs + 相对路径` 生成、请求参数里的 `uri === docId` 转发前替换、诊断回来反向换回 docId，路径边界仍由 Root 抽象单点把守；② 变更**一律发全文**（对 LSP 的 Full/Incremental 两种同步模式都合法，省掉前端增量 diff），键入 220ms 节流；③ 会话按 **(用户 × 工程根 × 服务器)** 复用（工程根由 `core/lsp/project-root.ts` 向上探测，见下文；同一工程的多个文件——哪怕来自不同工作台根——共用一个 gopls/rust-analyzer），并发上限 4、无文档会话空闲 10 分钟回收（有打开文档时按 6 倍阈值，避免「文件开着不动就丢能力」）。跨文件跳转由 `registerEditorOpener` 把 `file://` uri 折算回工作台的「根 + 相对路径」再开标签——否则 Monaco 在未打开的 model 上会静默失败。**跳转目标分两态**（`LspJumpTarget`）：命中已挂载根 → 根内相对路径（行为如上）；**未命中任何根**（库文件：`/usr/include/c++/13/string`、GOROOT 标准库、rust-src、site-packages/typeshed）→ 绝对路径交给工作台（`files/repo-paths.ts:resolveAbsPath` 纯函数：已有根优先 → **库锚点**建 `abs:` 临时根 → 父目录兜底；锚点分强/弱两级），并**复用跳转来源那份文档的语言服务器会话**（`lsp.open` 的 `attachTo`，同用户 + 同一服务器二进制才复用）——库文件由原来那个服务器回答（它已有索引与编译参数），也不新建进程、不吃并发槽位；临时库根**不抢左栏**（跳库文件是“看一眼定义”，不把资源管理器搬到 `/usr/include`）；非 `file:` 协议（如 Java `jdt://`）给明确提示而不是静默。与符号提取的分工：某语言有可用服务器时 `files/symbols.ts` 的**定义跳转**选择器剔除它（语义比词法/语法树准，也避免同一跳转出两份候选），而**文档符号/大纲**选择器把它并回来（符号来源在 `symbolsOf` 里按 LSP 优先仲裁，同样不会出两份）。诊断以 `setModelMarkers` 整体替换（owner 按服务器分），进程退出时清诊断并按（每个服务器）30s 限频**重挂全部受影响的文档**；拉起服务器记 `lsp.start` 审计（detail 带工程根与命中的标记）。三条实测约束备查：**客户端能力声明不声明 `workspace.workspaceFolders`**（pyright 声明后会挂起分析准备，补全/悬停/定义均不应答；initialize 参数仍带 workspaceFolders）；**诊断的 uri 按归一化键匹配**（服务器会改写盘符大小写与百分号编码，精确字符串匹配会丢诊断）；**清单拉取失败不固化**（网络波动后下次打开文件重试，deep link 打开的文件会等清单就绪）；另：只读态下 Monaco 不弹补全候选（编辑器行为），切到编辑态即可用，悬停与诊断在只读态照常。另（本轮打磨）：**工程根探测**（`core/lsp/project-root.ts`）——会话的 cwd/`rootUri` 与**复用键**都取「从文件目录向上找到的语言标记所在目录」（`go.mod`/`Cargo.toml`/`compile_commands.json`/`pyproject.toml`… 24 层上限、语言标记优先、`.git` 兜底、找不到回退工作台根；命中永久缓存、未命中 10s TTL），再做一步**工作区根细化**（Cargo workspace 成员 crate 各自有 `Cargo.toml`，取最近的那个会为每个成员各起一份 rust-analyzer；因此再向上找**声明了工作区的**标记——`Cargo.toml` 含 `[workspace]`/`go.work`/`package.json` 含 `workspaces`——命中就用它；这一步要读小的标记文件，仅此三类且命中后永久缓存），因此工作台根落在模块子目录时 gopls/clangd/rust-analyzer/pyright 仍能正确解析（`projectRoot`/`projectMarker` 随 `lsp.open` 回给前端，状态栏 title 可见），同一工程跨工作台根也只起一个进程；**能力协商**——服务器能力表未声明的请求不发、回过 `-32601`（method not found）的方法在本页记住不再问（实测 gopls 不实现 `rangeFormatting` 与 `completionItem/resolve`）；**零宽区间分两处处置**——诊断/悬停撑开一格（否则波浪线不可见），补全/格式化/重命名的零宽编辑**不**扩张（否则接受补全会吃掉光标后的字符；gopls 的自动导入正是两条零宽 `additionalTextEdits`）；**跨文件结果不按本文夹取**、`file://` uri 折算**跨平台正确**（POSIX 保留根斜杠、Windows 去盘符前斜杠）、跳转**带列号**（落在标识符上而非行首）；**进程退出按「每服务器一次」限频并重挂该服务器的全部文档**（此前限频与挂载写在同一循环里，只有第一个文档被重挂）；诊断额外映射 `tags`（未使用/已弃用）、`relatedInformation`（在此处声明）与 `codeDescription`（诊断码文档链接）；服务端转发前兜底补 `textDocument.uri`（缺了 gopls 报 `no package metadata for file `）；补全接 `completionItem/resolve` 与 `additionalTextEdits`，符号侧新增 `textDocument/documentSymbol` 与 `textDocument/rangeFormatting` 两个 provider。配置见下表 `GEBAI_LSP*`。

- **路径 → 语言（单份真相）**：`@gebai/sdk` 的 `file-language.ts` 是 `(路径) → Monaco 语言 id` 的唯一实现（`languageOfPath` / `extOfPath` / `baseNameOfPath`），服务端 `core/fs/mime.ts:languageForPath` 与前端 `files/main.ts:languageOf`、`files/ui.ts:extOf` 全部转发到它。判定顺序是**先扩展名、再特殊名**：`makefile-helper.ts` 这种「特殊名前缀 + 真扩展名」必须以扩展名为准；只有扩展名认不出语言时才看特殊名与变体前缀（`Dockerfile.dev` / `.env.example` / `Makefile.am`）。路径形态的容错也在这一层：正反斜杠、UNC、尾部空格/点（Windows 复制路径常见脏名）都不影响扩展名解析。相对路径与绝对路径一律可用（只取 basename）。测试见 `packages/sdk/src/file-language.test.ts`（51 例，含仓库真实路径抽查）。跨文件跳转的**生效语言**另有一个纯函数 `effectiveLanguageOf(detected, hint)`：路径判不出语言（`plaintext`）时用**跳转来源文档**的语言（无扩展名的库文件：`/usr/include/c++/13/string`），**C 家族歧义**（`.h` 从 C++ 跳过来）也按来源语言（libstdc++ 的 `bits/basic_string.h` 是 C++）；其余一律以路径为准（跳转可能跨语言，无条件覆盖会把目标文件的高亮改错）。
- **形态与入口**：`packages/web/files.html` + `packages/web/src/files/{main,explorer,editor,viewers,git,compare,ui,api}.ts`；入口按钮 `packages/web/src/files-entry.ts` 注入标题栏，URL 透传 `session`（`sess:` 根指向本会话工作区）/`root`/`project`/`path`；**主题不进 URL**——工作台以 `localStorage` 与主界面共享同一份用户级偏好（跨标签页 `storage` 同步 + 嵌入态 `postMessage`，`initTheme({ urlPrefs: false })` 忽略 URL 上的主题参数，见「主题」章节）。为什么新标签而非同页路由：Monaco + Git 面板资源重，「一边让 Agent 改、一边自己核差异」是常态，独立页同时带来故障隔离。
- **根抽象（权限边界）**：所有 fs/git 接口只接受 `(root, 相对路径)`——`sess:<id>`（会话 `tmp/`）/`proj:<name>`（预置项目）/`bind:<agent>`（会话绑定项目）/`user:`（当前用户目录）/`abs:<path>`（绝对路径，**服务模式沙箱下拒绝**）；根清单 `rootCatalog()` 去重供前端根选择器；`GEBAI_FS_ROOTS` 白名单根（JSON 数组）供服务模式授予指定目录。路径防护三层：词法（拒 `..`/绝对路径）→ realpath（拒软链逃逸）→ 前缀（兄弟目录边界）。
- **两套路径坐标（仓库相对 ↔ 根相对）**：**Git 侧**（status/diff/log/paths、提交里的文件清单）说**仓库相对**路径；**文件侧**（fs 端点、编辑器标签、资源管理器）说**根相对**路径。两者在「根 == 仓库根」时重合，**子目录根**（会话工作区常落在项目仓库的子目录里）时才分岔；而「整仓库」范围下还会出现**根之外**的改动——那条路径在根内根本无法表达。换算集中在一处（`files/repo-paths.ts`，纯函数 + 单测）：
  - **根前缀**由 `repoPrefixOfAbs(根绝对路径, 仓库根绝对路径)` 给出（`""` = 根就是仓库根，`null` = 不在此仓库——两者语义不同，不可混用）；仓库根取 `/git/status` 的 `repoRoot`（**子目录根往往不在根清单里，只有它带得出仓库根**），清单的 `repoRoot` 兜底。根绝对路径优先查清单，其次从 `abs:` 根 id 解析（本页可能被以清单之外的 `abs:` 根打开）。
  - **打开仓库内任意文件**（`openRepoFile` / 冲突合并的落点 / 提交清单里的「打开文件」）走 `resolveRepoPath`：① 在当前根内 → 用当前根；② 否则用清单里**覆盖它**的根（最长前缀优先，同长按项目类 > 会话 > 用户 > 任意目录）；③ 都没有 → 以**仓库根**建一个 `abs:` 临时根并登记进清单。切换根只影响标签自己（打开文件不动左栏），而「在资源管理器中定位」会**换根并在树里选中**——菜单项说的就是「带我去看它」。
  - **Git 路径过滤/文件历史统一用仓库坐标**（服务端跑 git 时 cwd 就是仓库根，paths 因此天然是仓库相对）：资源管理器右键给的是根相对路径，交给日志栏前必须补前缀（`toRepoRel`），否则子目录根下会**静默过滤成空**。
- **根清单探测（`GET /api/v1/roots` 的成本控制）**：根清单要给每个根标出「是不是 Git 仓库 / 当前分支」，早期实现**逐根串行** `repoRoot()` + 全量 `status()`——本地实测 25 个根（其中 20 个会话工作区都落在同一个仓库内）首次请求 **~5.0s**：同一仓库被反复探测，而每个 git 进程在 Windows 上光启动就要 ~65ms。现为四层省成本：① **浅路径优先 + 父仓库短路**（先探浅目录，摸清仓库根后落在其中的其他根直接复用，**逐级查 `.git` 防嵌套仓库被误短路**）；② **按仓库根去重**（同仓库只探一次）；③ **限并发 ≤8**（避免一次 spawn 几十个 git 进程）；④ **轻量 `probeRepo()` 替代 `status()`**（`rev-parse --show-toplevel --git-dir` + 读 `.git/HEAD` 判定分支——**不能用 `--abbrev-ref HEAD`**：未出生分支仓库上它报 `ambiguous argument 'HEAD'` 并非 0 退出，只看退出码会把空仓库误判成非仓库，而不看退出码又无法与 detached（同样输出字面量 `HEAD`）区分），结果缓存 TTL 60s（该缓存只喂左栏「仓库/分支」标签，实时状态另有 `/git/status` 等端点）。实测：git 进程数 71 → 8、冷启动 **~5.0s → 211ms**（真实 HTTP，另起实例）、热请求 1–2ms，25 个根的结果逐根一致（含嵌套仓库边界 6/6）。
- **启动时序（首屏与数据解耦）**：`boot()` 分两阶段——阶段一**同步**搭外壳（不依赖任何网络往返），**双 rAF 首帧后立即抹掉启动遮罩**；阶段二才装配数据（根清单 / URL 恢复 / Git 状态 / 各面板渲染）。为何如此：外壳渲染与 `roots` 之类的往返毫无依赖，早期把遮罩挂到「roots + 目录树 + Git 状态全部返回」之后，等于把服务端的往返整段暴露成白屏等待（实测遮罩 6.2s vs 首帧 0.05s）。空窗期由中央占位（「正在准备工作区…」）与 Git 工具窗的「正在加载…」承担；`nextFrame()` 带 200ms 兜底（后台标签页 rAF 会被暂停）。启动期对同一根重复的 `git/status` 请求做去重（进行中复用 + 500ms 窗口，2 次 → 1 次），Git 面板延到根与状态就绪才建（空根会被判成「不是仓库」并摆出「初始化仓库」按钮）。**Monaco 空闲预热**：**数据装配（根清单/Git 状态）完成之后**才起 `requestIdleCallback`（超时 2.5s 兜底）预取编辑器内核——预热不阻塞首屏，但排在首屏数据请求前面会与之抢带宽；后台标签页（Ctrl+点击打开）先不预热，切到前台再起。首次打开文件不再空等 ~0.5s——`loadMonaco` 的失败/超时**必须复位单例**（早期把 `resolve(null)` 的 promise 永久缓存，一次失败就让本页永远降级为轻量编辑器；并发调用仍共享同一进行中 promise）。启动遮罩退场（色球收拢 + 淡出）与外壳入场均走纯 `opacity/transform`（不引发布局抖动），并尊重 `prefers-reduced-motion`。
- **目录树**：懒加载单层列举（上层 5000 条 + truncated 标记）、目录优先自然序（`file2` 在 `file10` 前）、四种排序、隐藏文件默认列出（可在「更多」菜单里切换）、Git 状态色装饰、新建/改名/复制/移动/删除（物理删除，不可恢复；二次确认把关）/上传（拖拽）/下载。**复制走剪贴板**：右键「复制」（或 `Ctrl+C`）记下条目，到目标目录右键「粘贴」（或 `Ctrl+V`）落盘（目录行与空白处都有「粘贴」）；粘贴**从不覆盖**——同名时落成「xxx - 副本」「xxx - 副本 (2)」，因为粘贴是最高频的写操作而覆盖不可逆（「复制到…」一次性走完同一套落点规则），命名与落点判定在 `files/clipboard-core.ts`（纯函数 + 单测）。三条守卫：目录不能粘进自己的子树（否则递归复制）、跨根不粘贴（`/fs/copy` 的源与目标在同一根内解析，条目在另一根时菜单项直接禁用）、只读模式不粘贴。
- **目录树自动刷新（预取 + 事件驱动的增量同步）**：① **展开即预取下一层**——展开一个目录时顺手列举它的直接子目录（`MAX_PREFETCH = 24`、并发 ≤4、后台标签页不预取、换根即丢弃结果），用户接着展开子目录时命中缓存、不再等一次往返（实测展开 35ms 出内容，未经预取时要一次 HTTP 往返）；根目录总是展开的，它的下一层在 `requestIdleCallback` 里预取（不与首屏请求抢带宽）。**只预取一层**：更深层在用户展开时自然成为新的「下一层」。② **变更事件驱动的增量同步**（`explorer.syncDirs`，由「变更监听」章节的长轮询唤醒）——把变化路径换算成「父目录 + 自己」（`dirsToRefresh`），**只**重新列举已缓存且处于展开链上的目录，与缓存逐项比对（名字/类型），确实变了才换缓存：根列表变了走整树重建，其余目录只换**那一块子树**（删旧行 → 按新缓存重画）。内容修改（保存文件）不会重建任何行——行上显示的是名字，改动本身由 Git 装饰与变更面板表达。「未知变化」（路径太多或监听被关）时退化为重列根 + 展开目录（夹到 24 个，见兜底轮询）。
- **目录树渲染增量化**：① **装饰查表**——把「有变更的路径」一次做成 `Map`（精确命中）、「有变更的祖先目录」做成 `Set`（子项变更），行内只做 O(1) 查（早期每行两轮线性扫 `status.changes`，2000 行× 500 变更 = 每次刷新百万级比较，而且**渲染路径上就会调它**）；变更清单指纹未变则直接复用上一份表；行上记录已应用的装饰，未变的行完全不碰 DOM。② **展开/收起只动局部**——展开时把子树插到该目录行之后、收起时删掉它之后的子树行（扁平渲染下「深度大于本行」的连续行恰好就是它的子树），不再重建整树（大目录里展开一个子目录不再连已展开的全部行一起重建）。③ 整树重建走一次 `replaceChildren(片段)`；选中/活动态由 `refreshSelection()` 定点改「上一个/当前」两行（不再每次点击遍历全行）；`path → 行/条目` 索引表同时供装饰刷新与 reveal 定位复用。④ 名称过滤输入防抖 140ms。⑤ 行上的交互一律用 `on*` **属性赋值**（不是 `addEventListener`）——它们是元素上的普通属性、不产生监听器注册开销，因此**不做事件委托**（审计里「每行 7 个监听器」的结论只对 `h()` 的 `on*` 入参成立）。⑥ **鼠标语义按 IDE 口径分工：单击 = 预览、双击 = 固定**——单击文件行用**预览标签**打开（斜体标题、会被下一个预览就地替换，适合“顺着树往下扫文件”；预览的落地刻意延后 220ms，时序判定在 `files/preview-click.ts`），双击把它**钉成常驻**（目录行的双击仍是展开/折叠，见 `explorer.ts` 的 `onclick`/`ondblclick`；双击时待定的那一发预览被作废，只发一次常驻打开，并顺手收掉预览槽里那个无关的文件）。与标签栏双击（固定 ⇄ 取消预览）、快速打开 `Enter`/`Ctrl+Enter` 同一套状态，细节见「预览标签 ⇄ 常驻标签」条。
- **编辑路径开销**：① 脏标记不再逐击键 `getValue()` 全文比对（大文件 = 每键物化 MB 级字符串），改为击键时先乐观置脏 + 300ms 防抖复核（撤销回原样仍能变回干净）；② 状态栏整条重建前先比对状态快照（光标/标签/编码/EOL/Git/权限/内核等全部输入），未变直接返回；光标与拖选的刷新按帧合并；③ 选区字符数按**行长度累加**算出（同一范围复用结果），不再 `getValueInRange()` 物化选区文本——拖选/全选大文件时那是每次 mousemove 一次 MB 级分配。
- **查看矩阵**：文本/代码 → Monaco（VSCode 同款内核，vendor 静态伺服）；图片/视频/音频（HTTP Range 拖动进度）/PDF/Office（服务端转换阅读视图）/压缩包（零依赖中央目录解析 + 解压）/二进制（hex）/图表源文件。条目 `kind`/`language`/`editable` 由服务端裁决（前端据此选查看器与是否显示编辑按钮）。
- **编辑器配色跟随主题（`files/editor.ts` · `defineTheme`）**：用 `editor.defineTheme` 把当前界面的 CSS 令牌映射成 Monaco 主题（`base: vs/vs-dark` 按合成背景的明暗自适应，换肤时重新 define + setTheme，不硬编码配色）。三个必须守住的点：① **半透明令牌先合成**——主题令牌多为半透明（默认主题 acrylic 的 `--bg-elev` 是 `rgba(18,18,23,.82)`、`--bg-inset` 亮色下是 `rgba(0,0,0,.05)`），直接丢 alpha 只取 rgb 分量会得到与页面无关的假色：亮色主题下编辑器被判成暗色（`vs-dark`）并铺成黑底，再叠上亮色主题的深色前景 `--text` 就是「黑底黑字」（用户可见：切了亮色主题，文本编辑器没跟着换配色）。现按「页面底（body 计算背景）→ 编辑器底」逐层合成出不透明实色（复用 `css-color.ts` 的 `cssVarToHex`；其合成底同时认 `rgb()/rgba()` 与 `#rrggbb`，好让上一层合成结果能当下一层的底）。② **编辑器底取视图面板底色（`--bg-elev`）、不用内凹色**——编辑器坐在工作台视图里，与左栏/工具窗/标签栏/状态栏（均 `--bg-elev`）同层同亮度才是一整块；内凹色 `--bg-inset`（输入框/代码内嵌的语义）比面板暗 **4~19 级亮度**（亮色亚克力：面板 250 里嵌一块 236 的灰块；浪潪主题 31 vs 12），用户可见为「视图背景与编辑器背景亮度差得有点大」。浮层/建议框再用 `--bg-elev-2` 抬一档（缺失则同底、靠 border 分辨）。③ **等过渡停稳再取色**——换肤会带动 body 背景的 CSS 过渡（base.css 的全局过渡名单含 body），过渡途中取色会把中间值固化成编辑器底色（亮色切回后整块偏暗）且此后无事件重定义、不会自愈；现用 `getAnimations()` 等 body 的 `background-color` 过渡结束再定义（第一帧只作让位——过渡要等样式变更后的下一帧才启动，头两帧读到的还是旧值；无过渡时等一帧即定义；`getAnimations` 不可用或超 1s 时兜底定义）。
- **默认只读**：编辑器状态机 `查看态 readOnly ⇄ 编辑态`（工具栏「编辑」/Ctrl+E 解锁，脏标记 ●，Ctrl+S 保存）；默认只读既是习惯也是安全默认（与 Agent 共用文件，误触不得改文件）。
- **自动换行（`Alt+Z` / 动作轮盘）**：换行是**用户级偏好**（`gebai.ui.wordWrap`，默认关闭——代码的横向滚动是默认阅读方式），主编辑器、差异视图（两侧一起）、合并/暂存窗格与降级编辑器（textarea 走 CSS 类）都由 `files/editor.ts` 的**模块级开关 + 活动实例注册**统一施加：切换点有轮盘项与 `Alt+Z` 两处，而实例可能同时有十几个（每个标签一个，合并视图一次三个），把「记住开关」与「应用到全部」收在一处才保证一致（实例 dispose 自行注销）。`Alt+Z` 与 `F7`/`Shift+F7` 同一处置：**捕获阶段**拦下——Monaco 自己绑了这个键（`editor.action.toggleWordWrap`），但它只改实例选项、不动偏好（刷新即回退）也不更新按钮态，而且编辑器获焦时事件到不了冒泡阶段；**表单输入框里不拦**（Mac 上 Option+Z 是输入 Ω 的手势，在提交信息框/搜索框里打字时被快捷键抢走才是真的碍事），但 Monaco 与降级编辑器内部的隐藏输入区也是 textarea，靠「是否在编辑器容器内」区分。
- **编辑器右键：复制路径（含行号）+ 发送会话**（`files/editor.ts` 把两项注册进 Monaco 的 `EditorContext` 菜单；拼字符串的规则全在 `files/editor-ref.ts`，纯函数 + 20 例单测）：
  - **引用格式**（与 `grep` 输出的 `文件:行号` 同口径）：单行 `/abs/path/a.ts:12`、选中块 `/abs/path/a.ts:12-20`。两条容易错、又都不报错的规则：① **拖到下一行行首不算选中那一行**（Monaco 的 selection 语义，拖选到行首时 `endLineNumber` 是下一行）——`normalizeLineRange` 归一，写出去才不会多一行；② 单行不写成 `12-12`（行号后缀分两档）。**绝对路径取时现算**（`absOfRepo(rootAbsOf(tab.root), tab.path)`）：根清单 / 临时 `abs:` 根都可能后到，写死一个字符串会在那些情况下静默写成错的路径。
  - **位置与回收**：分组名 `gebai` 让它落在「剪切 / 复制 / 粘贴」之后、「命令面板 / 转到定义」之前（Monaco 按组名字典序排，`navigation` 被硬编码在最前）。**`addAction` 的句柄必须随编辑器 dispose 手回收**：它往**全局**菜单注册表加一条（带 `editorId` 前提，只在本编辑器弹），而编辑器自己的 `dispose()` 只清内部 action 表、不动那份注册——本页每个文件的查看/编辑态切换与「重新加载」都会重建编辑器，不回收就是“重建几次、右键菜单里多几组重项”（实测：不改之前每次重建都多一条）。降级编辑器（Monaco vendor 缺失）没有 Monaco 菜单、原生右键又被全站屏蔽，两项在那里是**自绘**的（挂容器而非 `pre`/`textarea`：两态互切会换可见元素）。
  - **发送会话（仅分屏）**：独立 `/files` 标签页里没有对话可发，所以这一项**只在嵌入（分屏）时注册**（`EMBEDDED`，见宿主入口）；点击后 `postMessage` → 宿主 `files-split.ts` → `composer.insertIntoComposer`。宿主侧三条约定：**追加不覆盖**（输入框里常已有半句话，替掉就是静默丢草稿；空行分隔）、**聚焦并送光标到末尾**（下一步就是接着写“把这里改成…”）、**按 `input` 那一套收尾**（自动高度 + 发送按钮形态 + 清输入历史导航——不清的话接着按 `↑` 会把刚发进来的内容当成“上一条”换掉）。跨窗口消息再卡一道长度上限（64KB；工作台侧已按段截断）。
  - **发送的片段** = 引用行 + 围栏代码块（语言标注只用语言 id）：参考行**不能塞进围栏信息串**（把 `ts src/a.ts:12` 写在围栏上会被 markdown-it 整串当语言名交给 highlight.js，`getLanguage` 必然失败——代码块**静默丢高亮**）；截断提示（200 行 / 20k 字符，防“整文件塞进输入框”）放在代码块**之外**，否则复制走就带上了不属于原文的一行。
  - **无选中时给光标所在整行**（右键“发送”却不选任何东西是很常见的手势，空代码块等于白点一下）。降级编辑器的**只读态没有光标**（textarea 被 `display:none`，且 JS 赋值后 `selectionStart` 停在**文档末尾**——照搬会报出一个与用户所见无关的末行），那里改从 **DOM 选区**算：highlight.js 只套标签、**不增删字符**，按文本节点累加即源码偏移；无选区则**不给行号**（复制得到纯路径），也就没有可发的内容（不摆那一项）。——① **侧边列**（`gutter`）：编辑器左侧独立一列（`.fw-blame-gutter`，在 Monaco 容器**之外**：flex 兄弟，右侧 1px 弱线分隔），逐行显示「作者 · 时间」；行位置用 `ed.getScrolledVisiblePosition()` 随滚动/折行按帧重算（只渲染可见行，行节点池复用），未提交行转警告色；② **光标行行尾**（`inline`）：`after` 注入到光标所在行行尾，样式更弱（淡色 + 斜体 + 半透明），光标移动只更新这一个装饰集合。两者共用一份数据（`/git/blame`），可以只开一个或都开。**只有行尾态的开关记在浏览器本地**（`files/blame-prefs.ts` → `gebai.ui.blameInline`）：它是一条跟随光标的淡色注释，常开不妨碍阅读；侧边列占一行宽度，每次打开文件按关闭处理。两者都是装饰/注入文本，**不进模型**：复制、保存、撤销拿到的仍是原文，文字也**不可选中**（注入文本 `cursorStops: None` 让方向键不在注释里停靠，`user-select: none` 堵住鼠标框选）。**行尾态在查看与编辑态都给**（跟随光标的一行批注，编辑时同样有用），侧边列仅查看态（整列作者会随编辑漂移）。三个必须踩准的点：① 装饰集合用**编辑器实例**的 `editor.createDecorationsCollection()`——`monaco.editor.createDecorationsCollection` 不是静态 API，取值得到 `undefined` 后静默降级为「什么都不画」（按钮照常高亮、不报错）；② 注入文本挂在**空 range** 上时必须显式 `showIfCollapsed: true`（Monaco 取注入文本会按 `showIfCollapsed || !range.isEmpty()` 过滤，否则整条装饰被静默丢弃）；③ 时间取**相对值**（`ui.ts` 的 `timeAgo`）：blame 关心的是「多久前有人动过这里」，绝对时间戳要占 16 字符；作者名截 14 字符。
- **编码/换行保真**：读取探测 UTF-8(BOM)/UTF-16/GBK 回退/换行（lf crlf cr mixed），保存按原编码与目标换行回写；**写乐观锁**：保存携带 `etag`，磁盘已变则 409 返回当前内容 + 「覆盖/另存/放弃」。
- **状态跨刷新保留（本标签页）**：打开的标签（root / path / 查看态 / 光标行）、活动标签、当前根、左栏视图与显隐写进 **`sessionStorage`**（`files/session-state.ts`，键 `gebai.ui.fwSession`）——它是**本标签页的会话状态**：独立 `/files` 标签页与分屏 iframe 里的工作台各记各的（同源 iframe 与宿主共享同一份 sessionStorage，用 localStorage 会把两处记忆互相覆盖），刷新（含 dev-reload）后回到原处，关掉标签页即随会话消失；**用户级偏好**（自动换行 / 行尾 blame / 主题 / 面板宽度）仍走 localStorage。只记**普通文件标签**——差异 / 合并 / 暂存 / 比较标签各自需要打开时的上下文（端点对、冲突文件、比较两端），一个路径恢复不出来；有未保存修改的标签按查看态打开并**如实提示一次**（内容不跨刷新，别让人以为改动还在）。恢复与 URL **取并集**（`boot()` 阶段二）：先按记忆把上次的标签恢复出来，再让 URL 落位（深链接 / 前进后退是明确的「看这里」，它决定的是**活动标签**）。不能写成「URL 带 `path` 就整段跳过记忆」——普通刷新后的地址栏里总带着当前文件（`activate` 会同步地址栏），跳过记忆就变成「一次刷新只剩那一个文件」，且随后的写回会把记忆也改成缩水状态（另一个标签从此再也回不来）。该文件已在记忆里时 `openFile` 命中已有标签、只把它激活并跳行，不会重复打开；新建标签页的深链接不受影响（新标签页的 sessionStorage 本就是空的）。**根不在清单里的标签跳过**（项目被移除 / 换了会话），否则刷新的结果是一串打不开的错误页。解析对脏值免疫（形状不对的条目丢弃、标签数上限 24）、无内容时清键不留残留——纯函数带单测（`session-state.test.ts`）。
- **标签的光标与滚动位置记忆**：`activate()` 会把上一个标签的**光标行/列与滚动位置**记在标签对象上（`cursorLine`/`cursorColumn`/`scrollTop`），回到该标签时先落光标、再回滚动（**顺序要紧**：反过来的话 Monaco 会因为“光标仍在原处”而在落光标那一步把视口扭回去，刚回的滚动位置白记）；编辑器（Monaco / 降级）两侧都实现 `getCursorPos()`/`setCursorPos()`（降级侧由 textarea 的 `selectionStart` 反推行/列）。为什么真正起效的地方是 **`loadTab` 重建**而不是切标签：编辑器实例是**常驻**的（切标签只是 `display:none`，位置本来就不会丢），会丢的只有重建——「重新加载当前文件」、「以文本打开」、图表源码↔渲染预览互切、大文件在 Monaco/降级间切换都走 `loadTab` 拆了重装（带 `opts.line` 时不还原：深链接/定位给出的目标优先）。同时每次激活都把状态栏的光标从**这个**编辑器同步过来（`state.cursor` 是“上一次光标事件”留下的值，而 `onCursor` 只认活动标签、`setCursorPos` 又只在位置真变时才触发事件——不校正的话切到一个光标恰好在原位的文件，状态栏会继续显示**上一个文件**的行号；這个陈旧值还会经 `session-state` 写进刷新记忆，下次刷新就把那个行号带到另一个文件上）。
- **下载/上传**：单文件流式下载（`Content-Disposition`）；多选/目录打包 ZIP（`zipPaths`，UTF-8 文件名，超限 413 引导分批）；上传支持拖拽到指定目录、同名冲突处理。
- **Git 图形化（任意两端对比为一等公民）**：差异统一抽象为**端点**——`WORKTREE` / `INDEX` / 空串（与 WORKTREE 搭配 = 未暂存）/ 任意 rev（提交/分支/标签/`HEAD~n`/SHA）；`compare(from,to)` 支持提交↔提交、提交↔工作区、提交↔暂存区、暂存区↔工作区、工作区↔历史提交、分支↔分支（`mergeBase` 三点语义）；`contentAt(ref,path)` 供 Monaco DiffEditor 取 **两侧真实文本**（非解析 patch，CRLF/编码差异下表现正确）；前端「比较」标签页两端各有端点选择器（工作副本/引用/本地分支/远程分支/最近提交/手输 rev）+ 交换 + `...` 开关 + path 过滤。界面为**无标题栏的三区**（IDEA 新 UI 式，见下文「界面结构」）：**左栏工具窗**（变更／资源管理器／搜索，三视图互斥）+ **底部工具窗**（分支／日志／提交内容三栏并排，分界可拖）。变更面板负责「我改了什么、要不要提交」（分组/逐块暂存/放弃更改 + 提交框 sticky 底部），底部三栏负责「历史上发生过什么」（点分支 → 看它的日志 → 点提交 → 右栏看内容）——「分支」栏内再切标签、**储存**（stash）、远程（fetch/pull/push force-with-lease）；破坏性写操作默认先备份并返回可恢复引用；`git blame` 追溯、冲突四方内容（base/ours/theirs/current）查看。**术语严格分开**：**暂存**（index，`git add`）与**储存**（stash，存放工作现场）不是一件事，界面文案与入口都不混用。
- **冲突合并（三窗格）**：对齐 IDEA 的 Merge 工具——冲突文件（Git 面板冲突组的 `merge` 按钮 / 双击 / 右键「解决冲突」）打开合并标签：左「我方（当前分支）」/ 中「合并结果（**可编辑** Monaco）」/ 右「对方（合入分支）」，可选展开「共同祖先（diff3 的 `base` 段，无则提示）」。工具条：冲突导航（上一个/下一个，显示「冲突 n/m + 各段行数」）、采纳我方/采纳对方/两者都留（作用于当前块）、全部我方/全部对方（整文件，二次确认）、保存（`Ctrl+S`，走 fs 写 + **etag 乐观锁**，外部改动 → 409 而非静默覆盖）、标记为解决（`Alt+M`，`git add` 后冲突态结束）。快捷键 `F8`/`Shift+F8` 跳冲突/上一个变更文件。
  - 解析与替换是**纯函数层**（`files/merge.ts`）：`parseConflictBlocks` / `applyResolution` / `hasConflictMarkers`——单独成模块是因为「合并会改写用户文件」，一旦解析漏块或替换错行就是静默损坏，必须能脱离 DOM/Monaco 单测（16 例：两段式/diff3 三段式/多块/未闭合与畸形标记/CRLF/空侧/`both` 顺序/批量解决行号不偏移）。容错取向：**只把完整闭合的块算作冲突**，畸形片段原样保留不参与解决（宁可不识别，也不乱改内容）。
  - 渲染与交互在 `files/merge-view.ts`；三窗格窄屏（<1180px）纵向堆叠。工具条另有**冲突计数**（「N 处冲突」）与「全部两者」（每块都保留两侧）。
- **部分暂存（按块 / 按行）**：差异不必整文件进暂存区——未暂存侧可「暂存此块 / 丢弃此块」，已暂存侧可「取消暂存此块」，逐行则由每行的复选框控制（上下文行不可勾）。
  - 底层是一条**只含选中改动的补丁**：`buildPartialPatch`（纯函数，`core/git/service.ts` 末尾）从单文件 unified diff 里切出选中的 hunk/行——未选中的 `+` 行整行丢弃、未选中的 `-` 行**降级为上下文**，hunk 头按新计数重写（与 `git add -p` 同一套语义）；随后按方向施加：暂存 `git apply --cached`、取消暂存 `--cached --reverse`、放弃 `--reverse`（**不建 stash 备份**：`backup:false`，与整文件放弃同一口径——要留存改动请在「储存」栏显式储存）。
  - **为何构造补丁而不是直接写文件**：写工作区再 `git add` 会改动用户文件（哪怕只是中间态），而 `git apply` 靠上下文匹配、失效时整体失败且不落盘——「行号已经变了」会报错而不是改错地方。构造环节单独成纯函数是因为「拼错了就是静默改错内容」，必须能脱离仓库单测（12 例：整块 / 仅删除行 / 仅新增行 / 单行 / no-newline 标记 / 空选择 / 真实仓库回环）。
  - 入口三处（对齐 IDEA）：差异视图的「逐块操作」开关（`files/partial.ts`；Monaco 是只读阅读器、放不进逐块控件，故整块换成可勾选清单）、变更面板的行内 hover 按钮与右键「逐块暂存…」、「三向暂存编辑器」（`files/staging.ts`：左 HEAD / 中**暂存区内容可编辑** / 右工作区；写入走 `stage-content` 的 hash-object + update-index，不碰工作区与 HEAD）。
- **编辑历史（交互式变基）**：日志条目右键「编辑历史（从这条之后改写）…」——列表**新→旧**展示（与日志一致），每条可选保留 / 改信息 / 压合 / 修补 / 丢弃 / 停下编辑，↑↓ 调顺序，压合与改信息就地编辑提交信息；提交给服务端时翻成**应用顺序（旧→新）**。
  - 服务端**自己重放提交**（切到基准 → 逐条 cherry-pick → 回写分支）而非 `git rebase -i`：`-i` 要交互式序列编辑器（`GIT_SEQUENCE_EDITOR`），跨平台得写临时脚本，而每步的语义（压合时如何合并信息、停在哪条）依旧要自己掌握；重放则把「计划」变成我们自己的数据，冲突/暂停时可**落盘到仓库的 git 目录**（`gebai-history-edit.json`），继续与中止都可控。压合的写法是「先正常 cherry-pick 出提交，再 `reset --soft HEAD~1` 拆回暂存区，最后 `commit --amend` 让上一条吸收」——这样中途出冲突仍走 git 自己的 `CHERRY_PICK_HEAD` 状态。
  - 安全网：动分支前先建 `gebai/backup-<ts>` 备份分支；未完成的计划在变更面板顶部出「继续 / 中止」横幅（中止 = 分支指回原 HEAD，备份分支保留供比对）。限制：要求工作区干净、在分支上（非游离 HEAD）、不重放合并提交（需指定主线父，不在本能力语义内）。
- **工具窗与目录树的 IDE 细节**：
  - **底部停靠**：Git 工具窗在编辑区**下方**（`--git-dock-h` 高度可拖，默认 300px，双击拖条复位，高度记忆在 localStorage；窄屏 <1180px 时三栏纵向堆叠），rail 上的 git 按钮切换展开/收起、展开态高亮；关闭按钮在**面板自己的标题栏**内（与 IDEA 工具窗一致，不在标签栏上）。
- **左栏三视图常驻、只切显隐**（`mountLeftView`）：变更 / 资源管理器 / 搜索挂在同一栏里（隐藏类 `fw-view-hidden`），不再 `clear + appendChild` 搬动子树——早期做法每次切换都要重排整栏，且被摘下的子树**丢失滚动位置**（目录树看到一半去看「变更」再回来，树回到了顶部）。
- **尺寸变化与编辑器重排**：拖动分界条（左栏宽度、工具窗高度）的尺寸写入按帧合并（`requestAnimationFrame`，每个 mousemove 直接写 style 会连带强制布局；工具窗底边在拖动期间恒定，起手量一次），且**只重排活动标签**的编辑器，松手时再一次性补齐全部（`layoutAllEditors`）——Monaco 虽开 `automaticLayout`，隐藏标签在 `display:none` 下量不到尺寸，仍需显式 layout，但拖动过程中每帧给看不见的标签重排纯属浪费。分屏宿主（主界面侧）的 `resize` 回调同样按帧合并。
- **Git 面板取数成本**：① 分支的 ahead/behind 用 `for-each-ref` 的 `%(upstream:track)` 一次带出（早期对每个跟踪分支各跑一次 `rev-list`，上限 50 个 = 最多 50 次进程启动，Windows 上单次 ~65ms；不认识 `:track` 的老 git 回退到旧的逐分支计数路径）；② `compare` 不再额外跑一次 `git diff --stat`（那又是一次完整内容 diff，只为拿一行摘要），`stats` 由已解析出的文件清单**本地汇总**（形状对齐 git：每文件一行 + 末尾汇总）；③ 面板**收起时不刷新**内部三栏（保存/`F5` 链只在展开态刷，展开时 `toggleGitPanel` 补刷），刷新入口做同刻合并（切根 + 保存 + `F5` 常在同一拍里触发）。
- **降级 diff 渲染的行数上限**：Monaco 不可用 / 两侧超体量时走结构化 hunks 渲染，单文件上限 **3000 行**（服务端的字符上限能装下数万行，每行 4 个 span = 十万级节点会让主线程停机数秒），超出部分明确提示「仅渲染前 N 行，另有 M 行未显示」。
- **日志与菜单的细节**：日志「滚动到底自动加载」监听**真正的滚动容器** `.fw-git-col-body`（早期挂在无 overflow 的 `.fw-log-list` 上，事件永不触发，是死代码）；右键/下拉菜单的全局监听**同步注册**并在 `closeMenu()` 里显式卸载（早期延后到下一宏任务注册、只靠 MutationObserver 清理，快速连开菜单会漏掉一对永久 document 监听器）；对话框遮罩去掉 `backdrop-filter`（全屏 blur 每开一次弹窗都让合成器把整个视口快照重模糊），菜单自身的毛玻璃保留（那是主题观感的一部分）。
  - **vendor 引擎脚本与 Monaco 的 AMD loader 冲突**（`files/viewers.ts` 的 `loadScript`）：工作台会加载 Monaco（`vendor/monaco/vs/loader.js` 定义全局 `define`），而 vendor 里的 esbuild/UMD 包（mermaid / echarts / viz）见到 `define` 就走 AMD 分支——与已存在的匿名 define 冲突（`Can only have one anonymous define call per script file`），脚本抱错、全局变量永不挂载（表现为图表预览“渲染失败：Cannot read properties of undefined”）。解法：**加载期间临时摘掉 `window.define`**（结束后成败都立即恢复），迫使它们回落全局挂载。
  - **子菜单坐标用父菜单局部系**：`.fw-menu-pop` 带 `backdrop-filter`，而它会（同 `filter`）把自身变成 fixed 子元素的**包含块**——子菜单写视口坐标会被当成 host 内偏移，于是弹到很远的地方（鼠标根本移不到）。所以子菜单 `left/top` 按 `btn.right - host.left + 2` / `btn.top - host.top` 算（并钓制在父菜单高度内，否则鼠标一出父菜单就丢）；开合用父项/子菜单双向 `mouseenter/mouseleave` + 150ms 延迟收起，且**已展开不重建**（重建会把鼠标正下方的子菜单换掉）。
- **查看器与负载释放**：图片查看器的拖拽 `mousemove/mouseup` 只在拖动期间挂、松手即摘（早期一次注册就永久留在 window 上）；标签加载用**令牌 + 存活校验**（`loadGen`），`await` 回来后若标签已关闭或被新一轮加载取代就当场回收刚建好的编辑器；重建视图前统一 `dispose` 旧编辑器/查看器，比较标签补上 `viewDispose`（早期它从不设，`compareTabs` 里的视图与状态一直留着）；图表「源码 ⇄ 渲染预览」互切先把上一态彻底卸掉（只 append 不清 host 会两态同屏叠着），且标签有未保存修改时拒绝切换。
  - **目录树 VCS 装饰（IDEA 风格）**：文件名下方**彩色下划线**表示状态，同时保留右侧**字母徽标**（精确状态：M/A/D/R/S/U/!）与 hover 提示。颜色与 Git 面板的变更字母**同一套语言**（修改=警告色琥珀 / 新增·未跟踪·已暂存=成功色绿 / 冲突=危险色红 / 删除=红色删除线 / 仅「子项有变更」的目录=中性灰）——不用 `--tool`（那是聊天里工具消息的颜色，部分主题下是灰的，语义不符）。装饰刷新走 `explorer.refreshGitDecorations()`——**只换装饰元素、不重建树**（树的展开态/滚动位置/选中项都在 DOM 里，重建会「折叠回去 + 滚动跳顶」）。
    - 修过的坑①：git 状态是异步到达的，而树在状态到达**之前**就渲染完了；早期只在渲染时取一次 `gitStatus()` → 徽标永远为空（只有手动刷新才出现）。现在 `refreshGit()` 拿到状态后回填装饰。
    - 修过的坑②（**`line-height: 1` 把字母下半部裁掉**）：树容器原为 `line-height: 1`，行盒 = 12.5px 而字体 ascent+descent = 15px——半行距为**负**，基线以下只剩 ~1.75px；而 `g/j/p/q/y` 的尾巴需要 ~3px，文件名 span 的 `overflow: hidden`（横向省略号所必需）会把**纵向溢出一起裁掉**。后果：`keqing` 的 `g` 尾巴被切平，看起来像 `q`（更糟的字体度量下切得更多），且新加的状态下划线（baseline 下 2px）**整条不可见**——computed style 说 `underline`，实际一个像素都没画。改为 `line-height: 1.55`（19.4px，基线以下 ~5.2px）后尾巴与下划线都完整，行高 22px 不变。**教训：`overflow: hidden` 的省略号容器会把 `line-height < 字体度量` 的墨迹（含 descender 与 text-decoration）静默裁掉，且 computed style 不反映——必须看渲染结果或量基线以下空间。**
- **子系统边界（会话工作区 vs 仓库根）**：root 可为仓库子目录（典型：会话 `tmp/` 位于项目仓库内）——服务端返回 `rootPath` + `prefix`，Git 面板与比较视图**默认限定该子目录**（chip 一键切整仓库；限定范围内无差异且整仓库有差异时给引导）。路径语义：**git 侧为仓库相对**（`contentAt`/git 命令）、**fs 侧为根相对**（树/编辑器），前端 `toRootPath()` 负责在打开文件时换算；`routes/git.ts` 仓库定位用独立 `dir` 参数（不复用 `path`——`path` 在这些端点是 pathspec，混用会把文件当目录）。
- **安全与审计**：`GEBAI_FS_ENABLED`（总开关，false → 页面与 fs/git 端点全 404）/`GEBAI_FS_WRITE`（工作台写开关）/`GEBAI_GIT_WRITE`/`GEBAI_GIT_REMOTE`；写操作统一审计（`GEBAI_FS_AUDIT` → `{GEBAI_HOME}/audit-fs.jsonl`）；**删除即物理删除**（不设回收站，破坏性操作由前端二次确认把关）；上限 `GEBAI_FS_MAX_READ|WRITE|UPLOAD|ZIP`，以及差异侧的 `GEBAI_FS_MAX_DIFF` / `GEBAI_GIT_MAX_FILE`（见下文「体量与失败」）。
- **与主界面的集成**：除标题栏轮盘左侧的「文件」按钮外，**消息流里的文件产物也能一键跳工作台**——文件链接 chip、文件卡工具栏、原文件弹窗标题栏三处各有「在文件工作台中打开」（`file-wb-icon`）；产物在哪个会话产生就打开哪个会话的工作区（chip 显式携带渲染时的会话 id，不随当前会话漂移）。
- **深层链接（`files/deeplink.ts`）**：主界面只透传**原始路径**（`/files?session=&path=[&line=]`），**根的定位在工作台侧完成**——解析逻辑只此一份，主界面无需复制根清单与匹配规则，根的增删/改名都不会让入口失配。三档优先级：① 显式 `?root=`/`?project=`；② `?path=` 为绝对路径 → 根清单中**最长前缀匹配**的根（平局时按类型：proj/bind > sess > user > abs——本地模式的 `abs:` 白名单根常与注册项目指向同一目录，不应抢占项目根）；②b 绝对路径不属任何已知根（项目外的 Agent 产物/临时目录，如 `/tmp/x/f.txt`）→ 以**所在目录**为 `abs:` 根直达（服务端在沙箱模式下 403 拒绝，前端不是安全边界）；③ 其余（相对路径或无 path）→ `?session=` 的会话根（无参数时默认项目根 → 绑定根 → 第一个根）。**显式 `?root=abs:<绝对路径>` 即使不在根清单内也接受**（「打开任意文件夹」链接直达）；会话根只接相对路径——绝对路径交给会话根必然越界，故走 ②b。`?line=` 打开后跳行（产物行号直达）；**路径归一按目标根类型**（`normalizeArtifactPath`）——会话根下剥掉 `tmp/` 前缀（会话根本身指向 tmp，再带一层即 `tmp/tmp/…`），而项目/其它根下的 `tmp/` 是正当目录名**不剥**（仓库里真有 tmp 目录）；同一份规则被地址栏恢复（`restoreFromUrlState`）复用——它此前直接用原始 query 路径，与深层链接解析分叉，会出现「同一链接刷新后多一层 tmp 而 404」。纯函数、参数可注入（`search`/`isWin`）、无 DOM 依赖——28 例单测覆盖（显式根/清单外 abs 根/最长前缀与嵌套根/Windows 盘符大小写/会话相对路径与 tmp 前缀按根类型/显式会话根/项目根 tmp 不剥/中文空格路径/无匹配回退）。
- **界面结构（无标题栏三区）**：整体对齐 IDEA **新 UI**——去掉传统标题栏/菜单栏，入口交给**左侧活动栏**与各面板自身：
  - 活动栏分两组（中间以细分隔）：**上组 = 视图切换**（资源管理器 / 搜索 / 变更，三视图**互斥**——同一时刻只看一件事：要么在找文件、要么在看改动）；**下组 = 工具窗与全局入口**（Git 工具窗开关 / **更多**）；**嵌入（嵌入 iframe）时最下方多一对同窗切换按钮**：主按钮 = **关闭文件工作台**（左箭头，与「更多」里那一项同一个图标、同一个动作，走既有的 `requestCloseSplit()`），悬浮时从它右侧弹出的辅助按钮 = **进入分屏**（两栏图标，`gebai:files-enter-split`；只有全屏态才渲染）——面板内最直接的那两个出口，独立标签页不渲染。
  - 「更多」承接原菜单栏的杂项（新建文件·文件夹 / 上传 / 比较 / 刷新根清单 / 快捷键一览 / 服务端开关 / 浏览器全屏 / 关闭文件工作台（独立标签页下是「返回歌白主界面」）），`Ctrl+K` 直开；文件类动作同时在树右键菜单里有（两处入口，不是唯一路径）。**没有面包屑与单独的工具条行**：路径信息由标签标题 + 资源管理器表达。
  - **预览标签 ⇄ 常驻标签（VSCode 同款，双击切换）**：标签有**预览态**（VSCode 说法 preview，本页表达为**标题斜体**，`.fw-tab.preview .fw-tab-title { font-style: italic }`）——预览标签**只有一个**，下一个预览会把它**就地换成新文件**（同一个标签、不堆标签），适合“扫一眼几个文件再决定看哪个”。三处交互对齐 VSCode 的口径：① **资源管理器单击文件 = 预览**（`hooks.openFile(root, path, { preview: true })`）、**双击 = 固定**（`{ preview: false, only: true }`）；② **标签上双击 = 固定 ⇄ 取消预览**（与标签右键菜单「固定/取消预览」同一动作、同一份状态）；③ 快速打开里 `Enter` = 预览、`Ctrl+Enter` = 固定。四条状态机约定：**(a) 单击的预览延后一拍落地**（时序逻辑在 `files/preview-click.ts`，纯函数 + 单测；资源管理器只负责把行元素交给它）。为什么不立即开：双击在浏览器里是“两次 click + 一次 dblclick”，而单击与**双击的第一发**在事件层一模一样（实测里第二发 click 还可能被浏览器合并掉）；而“开预览”在标签层是**就位替换**（旧预览会被顶掉）。立即打开的话，双击 B 就变成“第一发把上一个预览 A 顶掉 → dblclick 把 B 钉住”，A 已经默默没了；延后到双击窗口之后，双击只剩“开 B + 钉住”一个结果（待定的那一发被作废，连请求都不发）。代价是单击的预览晚 220ms 出现（选中/高亮是即时的），换来双击不再产生中间态。**(b) 双击是「只要这一个」**：`openFile` 的 `only`（浏览器在双击时只给一次 click + 一次 dblclick，第二发 click 被吞，于是走的会是“标签已存在”分支、只把它钉住）——双击路径先把预览槽里那个无关的文件收掉，再把目标标签建/钉成常驻。**(c) 离开即落定**：切到别的标签时，被留下的预览标签自动转常驻（`activate` 里做）。不这样做的话，它永远占着唯一的预览槽，下次单击别的文件会把它顶掉，而它早已不在眼前。**(d) 就位替换的旧预览不是“离开”**：替换前先把 `prev.preview` 置假，它被顶掉就是被顶掉（少了这一行，旧预览会被 (c) 误当成“用户切走了”而转成常驻标签永久留下来——实测症状：双击 B 之后，先前的预览 A 变成第二个标签，B 自己反而还是预览）。同理，**命中已有标签时不把它的预览态改回去**（`opts.preview === true` 不清不设）：否则“单击一个已固定的文件”会把它悄悄降回预览，接着双击的第一发又把第二发钉住——两次交互互相抵消，用户看到的是“双击没反应”。预览态**不持久化**（刷新后恢复的标签一律常驻，见 `session-state.ts`：恢复本来就是为了接着用）。双击面板里的文件行（目录行是展开/折叠）、右键「打开」、搜索结果、变更面板等**明确动作**仍走常驻打开（不传 `preview`）。
- **标签栏右侧只留高频动作，其余进轮盘**（与标题栏轮盘同一套交互，共用 `wheel-core.ts` + `css/wheel.css`；入口 hover 展开、键盘 Enter/空格/↓ 同样展开，Esc/外点/resize 收起；**弧位**：半径**固定不变**（内/外弧各由调用方给定，不随项数自动扩大——弧位拥挤的正解是减项或改分组，拿半径让路会把“两圈”变成一大一小两个不清不楚的圈），只调**角度**：从起始角向左侧长（对称外扩会把首个按钮顶出屏幕右缘），上限 = min(maxSpan, 不越过入口那一行)，上限内尽量拉开到「边长 + 间隙」，拉不开就停在上限），常驻「编辑/查看」，以及**可渲染文件多一个「渲染/原文」切换**（**markdown（md/markdown/mdx）渲染为文档**、图表源码（mermaid / plantuml / d2 / echarts）画成图；它决定看到的是文档/图还是文本，比其它动作都要紧；图标随之在“切换到渲染预览 / 切换到源码”间切换并标 active）；判定见 `files/preview-kind.ts`（纯函数：markdown 的服务端 kind 是 `text`，不能靠 kind 分派，只能按扩展名；该模块也是图表扩展名清单的唯一真相），markdown 渲染**懒加载**与聊天页共用的 `md-core.ts`（工作台首屏不背 markdown-it + highlight.js 体积）；两类预览容器都带 `.fw-preview`（标签栏按钮与 toggleRendered 靠它判定“当前是预览态”）；轮盘**内弧 = 历史相关三个**（文件历史 / 行尾 blame / 侧边 blame 列——都回答“这行、这文件是什么时候、谁改的”，是一类动作），**外弧 = 文件本身的动作**（保存 / 重载 / 下载 / 复制路径；保存带脏标记高亮与禁用态）**+ 显示开关「自动换行」**（全局偏好，按钮高亮即开关态，与 `Alt+Z` 同效，见「自动换行」）。按钮区**不画分界线**（它紧贴标签栏右端，多一条线只是把两个相邻区域切成两栏），与标签栏底色一致。两弧分别是“偶尔用一次”的动作，铺成一排会把编辑区右上角变成按钮墙、也把高频动作淹没。两个常驻按钮与轮盘入口均仅图标、随活动标签类型变化；Git 动作在各自面板里。
  - **轮盘的保持区不吃指针事件**（`wheel-core.ts` + `css/wheel.css`，两个入口共用）：容器是一块覆盖扇形边界盒的矩形，盒下面往往就是真实控件（入口在界面右上角、扇形向下左展开，标签栏与编辑器上缘正在盒内）——容器一旦可命中，展开期间那些控件就都点不到（点击落在容器上：既不触发下方按钮，也不算“点了外面”，页面不报错、只是「点了没反应」）。因此容器一律 `pointer-events: none`（展开态也不恢复），只有扇形按钮自己 `pointer-events: auto`；「指针还在扇形附近就不收起」改由 document 上的 pointermove 按坐标与容器矩形比对（**展开期间才挂**，收起态不跑每帧回调），指针离开整个文档（切窗口）另由文档根的 pointerleave 兜底（文档根不冒泡，捕获监听才收得到；只在事件目标就是文档根时响应——元素级 pointerleave 是“指针从入口滑到扇形按钮上”这类正常移动）。点扇形按钮后收起仍靠容器上的 click 冒泡，与容器是否可命中无关。
  - **标签条溢出时在条内横向滚动，右侧动作永远在位**：标签栏分三段——`.fw-tabstrip`（标签条，`flex: 0 1 auto` + `min-width: 0` + `overflow-x: auto`）、`.fw-tabbar-spacer`（空白区，双击快速打开）、`.fw-tabbar-actions`（动作区，`flex: none`）。**标签 `flex: none`**：宽度只由标题与 220px 上限决定，不随标签数量变窄（压缩标签只会得到一排只剩省略号的窄条，等于没有标签栏）；`min-width: 0` 是标签条能被压缩的前提（flex 项默认的 min-content 底线会顶住不缩，压不动也就滚不起来）。于是宽度不够时唯一的变化是标签条自己滚，被牺牲的也只有它（空白区与动作区各留自己的最小宽度，动作按钮不会被顶出 `overflow: hidden` 之外）。**横向滚动条不画**（34px 的栏里 11px 的条会把标签压矮一档，而它只在溢出时出现、那正是最需要这 34px 的一屏）：滚法由三处承担——滚轮（`main.ts` 显式把 deltaY 交给标签条，Chromium 不把纵向滚轮映射到横向滚动容器上；横向滚动与 Ctrl+滚轮缩放不拦）、触控板与 Shift+滚轮的横向滚动、以及 `scrollActiveTabIntoView`（每次重渲染后按需调 `scrollLeft` 把活动标签带进视野——新标签总长在最右，Ctrl+Tab 也可能切到视野外的那个）。
  - **面板之间的分隔比控件边框更弱一档**（`--fw-divider` = `--border` 降饱和到 55%，见 files.css 的 `.fw-page`）：`--border` 是「内容与背景的分界」（表单/卡片/输入框/图标按钮，需清楚），而外壳各面板只是同一面墙的不同房间，同强度的线会把界面切成方格纸——所以另立一档更弱的分隔色，只给外壳级分界用（控件与列表行分隔仍用 `--border`）。**活动栏右边界（1px `--fw-divider`）属于这一档**：活动栏与左栏虽同为 `--bg-elev`，但职责完全不同（面板开关 vs 内容），没线时两块贴在一起像一块宽面板；左栏收起时它同时充当活动栏与编辑区的分界。另外**左栏首行与标签栏同高（34px）**：两处本是同一条横线，差几像素就会看成两道错位的分界。**但这一档只在“真需要分界”处用**：资源管理器头部与目录树之间**不画线**——它们同属一个面板（一棵树从头部下面长出来是自然的），头部的控件已足够表达“这里是头部”；线反而把两个像素级的孪生区域切开。
  - **底色已不同处不画线（线只在“两侧同底色”时才承担分界）**：相邻区域各自有背景差时，线是重复信息，白添噪声与疲劳；反之两侧同为一种底色时，没有线就分不清是两块区域。按此判据，工作台里保留的线只剩：活动栏右界与页签之间（同为 `--bg-elev`）、可拖的三栏分界（`.fw-col-resizer`）、以及控件/弹层自身的轮廓（按钮、输入框、芯片、菜单、对话框——它们是浮在背景上的实体，没轮廓就糊在一起）。而栏头、工具条、列表分组标题、查看器条、横幅、比较面板头部、对话框标题/底部动作区、提示条、状态栏上界、页签栏下界都属于“两侧底色已不同”→ **一律不画**；**底部工具窗的上界例外**（`.fw-git-dock` 的 border-top 必须保留——它是编辑器与工具窗唯一的分界，两侧同为 `--bg-elev`）。明细见 `css/files.css` 与 `css/terminal.css` 中各选择器的注释。
  - **Git 工具窗不设常驻标题栏**：那一行里每一件都有别的载体——面板身份＝活动栏高亮（「源代码管理」四字本身不是信息）、当前分支＝状态栏的分支项（以及分支列表的 ✓）、「比较」＝活动栏「更多」与 `Ctrl+Shift+D`、刷新＝各栏自己的刷新与 `F5`。仅有两件事**别处说不了**：多步操作进行中（merge/rebase 与冲突数）与在途写操作（fetch/pull/push 耗时以秒计）。因此这一行**空闲时整行隐藏、零高度**，只在这两件事发生时亮成一条状态带（上下两条线此时才出现，把这条带框出来）。
  - **就地关闭入口在面板右上角**（`panelClose`，放在第三栏头部末端——最后一栏的头部右缘就是面板的右上角）：标题栏降为条件状态带后，“在这里关掉它”没有可就近点的东西了，这一枚按钮补上这个位置（活动栏按钮与 `Alt+G` 仍在）。它与提交动作按钮同排，故栏头部的图标按钮一律 `flex: none`，栏被拖窄时不会被挤成一条线。
  - **终端面板的标题栏是常驻的**（与 Git 面板不同）：会话页签 + 新建/清屏/中断/查找/设置/关闭都是主入口、别处没地方放，因此它保留上下两条线。
  - **「拖动改大小」的分割区（`.fw-resizer` / `.fw-dock-resizer`）：拖条只管交互，面板自己画上下边界**——拖条铺**面板色**，指针进入/拖动中整条转 `--accent-soft`（这就是“可拖”的提示），同时在拖动期间让面板那条上边线让位（否则「1px 线 + 4px 高亮带」又是两道）。拖条**不能透明**：它本质是两块面板之间的 4px 缝，`transparent` 透出的是**页面底**（比面板暗一档：面板 `rgb(16,16,21)` → 缝 `rgb(8,8,8)`），中间会裂开一道比分界线还宽还黑的缝，那才是「分割区很割裂」的主因。也不能改成给父容器 `.fw-body`/`.fw-main-right` 铺色：面板自己的 `--bg-elev` 是半透明的，父级再铺一层会让左栏叠两层、比编辑器亮 2 级。**左栏与编辑器之间不画线**：两侧底色本就不同（左栏 `--bg-elev` / 编辑器 `--bg-inset`），色差已构成边界。
  - **滚动条静止透明**（与主界面 `#session-list`/`#messages` 同构）：树/列表/日志都是常驻长列表，滚动条常显就是一根赖在面板边缘的亮竖条，紧贴分隔线看着像“两条平行分界”（实测 slider 亮度 68 vs 分隔线 33）。WebKit 用 `*:hover::-webkit-scrollbar-thumb`（父元素 hover 对子元素的滚动条生效），Firefox 的 `scrollbar-color` 无“非 hover 透明”写法，降为“静止弱色、hover 强色”。
  - **外壳三区 + 活动栏贯穿全高**：`左栏 | 编辑区` 构成上部（`.fw-body`），底部工具窗在它下方、从**活动栏右侧**一直延伸到窗口右缘；活动栏是唯一贯穿全高的一列。理由：① 底部三栏（分支/日志/提交内容）看的是全局历史信息，与左边在看哪个目录无关，需要完整横宽才摆得下；② 视图切换/工具窗开关这类入口要在任何面板开合时都待在原位——被工具窗截断半截，下方那几个按钮会看起来像工具窗的一部分。左栏随之上收到上半部分（同 IDEA：打开底部工具窗时左侧 Project 工具窗被压矮，而非并排）。
  - **条目行内动作按 VSCode 的改动列表**（从左到右：**打开文件 → 放弃更改 → 暂存更改**；已暂存组是「取消暂存」，冲突组另有「冲突解决」，已删除条目不给自己没文件可开的「打开文件」）：**第一个按钮是「打开文件」（打开文件本身）而不是「打开差异」**——看改动是**点整行**的事，两者混在一个入口上会让人以为按钮只是「另一种看差异的方式」；右键菜单里两项也拆开（未跟踪无 HEAD 侧、已删除无工作区侧，各自按可行性隐藏）。**逐块暂存与三向暂存编辑器不进条目**（前者要勾选、后者是独立编辑器，都不是「一行一个动作」的粒度），统一收在右键菜单里——条目上的按钮越少越不容易点错，而那两件事本来也不是随手点的。
  - **树视图的目录行也有整支动作**（放弃 / 暂存 / 取消暂存）：树一收拢，一个目录就代表它下面那一批文件——「先把 `packages/web` 这一支暂存了」「这一支先都放弃」时逐行点文件名既慢又容易漏，目录行上的一键对着**整棵子树**执行（作用清单由 `collectDirPaths` 给出，含深层子目录里的改动；建树前算一次 `Map<目录, 路径[]>`，避免每个目录行各扫一遍分组）。**按钮集合与文件行逐条对齐**，只少一个「打开文件」（目录没有可打开的文件，它的行内动作就是点行折叠/展开）；顺序也照文件行（放弃在前、暂存在后），跨行读起来一致：
    - **未暂存**：`放弃该目录下的更改（N 个文件）` + `暂存该目录（N 个文件）`；
    - **未跟踪**：`删除该目录下 N 个未跟踪文件` + `暂存该目录（N 个文件）`（未跟踪的「放弃」就是删除，与文件行同一措辞）；
    - **已暂存**：只有 `取消暂存该目录（N 个文件）`（与文件行一致：先取消暂存再谈放弃）；
    - **冲突**：两个都不给（与文件行、组头一致：冲突要先解决再谈暂存/放弃——`git checkout --` 对 unmerged 文件本就报错）。
    - 文案带实际文件数（严格等于该支下的改动数）；**放弃是不可逆动作**，确认框写清范围与条数（`放弃「pkg/」下的 3 个文件的更改？…此操作不可恢复`），未跟踪则明说「不在 Git 版本控制里，删除后无法恢复」。撤销备份走服务端 `backup:false`（不自动 stash，见「放弃更改不带自动 stash 备份」那条），要留存改动请在「储存」栏显式储存。
    - 点按钮**不等于点行**（否则暂存完顺带把目录折上，还得再展开看结果）——`onclick` 里用 `closest("button")` 过滤，与文件行同一手法。目录行同样适用「按钮不可压缩、名字先让位」那条布局契约（目录名 `text-overflow: ellipsis`；实测最窄 180px、两个按钮同在也不越界）。
  - **行内布局：按钮永不越出容器，宽度不够缩略文件名**。左栏可以拖得很窄（下限由提交框动作行实测决定），而条目行里「标记 + 路径 + 按钮」是全宽排布——若不显式分工，路径区作为 flex item 的 `min-width` 默认是 auto（= 内容宽），整行总宽由「路径全宽 + 按钮」决定，栏一拖窄就只能溢出：按钮被挤出容器、或被父级裁掉半个（点得到看不到）。现在的分工：**按钮组是不可压缩的整体**（`.fw-change-actions` `flex: none`，按钮自身 24px 定宽，组内间距 2px 比行内 gap 省一半），**路径区是这一行里唯一可收缩的部分**（`.fw-change-path` 显式 `min-width: 0` + `overflow: hidden`，目录与文件名各自 `text-overflow: ellipsis`），收缩时先让出**目录**（shrink 权重远大于文件名，窄栏下目录整段消失）、再让**文件名**出省略号；行上再叠一道 `overflow: hidden` 兜底。实测（1500px 窗口、长路径文件）：480px 时目录与文件名都完整，360px 时目录让位、文件名仍完整，≤300px 时文件名开始省略；各宽度下按钮右缘始终在行内（`scrollWidth == clientWidth`）。
  - **「放弃更改」不带自动 stash 备份**（`discard` 传 `backup:false`）：丢弃就是丢弃（确认框明说不可恢复）。原先的「丢弃前自动 stash」会把每次放弃都变成「储存」栏里的一条记录，把**储存**（stash，用户主动保存工作现场）与**暂存**（index，准备提交的内容）两件事混在一起——现在两者的入口与文案严格分开：条目行上是「暂存更改 / 取消暂存」（index），「分支」栏内的小切换才是「储存」（stash：储存当前更改 / 恢复储存 / 删除储存）。服务端仍保留 `backup` 参数供 API 调用方显式要求备份。
  - **自动刷新**：变更面板与状态栏同源（`main.ts` 的 `refreshGit` 落地后即调 `changesPanel.refresh()`），外部写入（Agent 改文件、终端里跑 git）经「变更监听」唤醒后按 600ms（最长 3s）合并刷新一次 Git 状态，工具窗只在**可见**时连带刷新三栏（分支/日志查询比一次 status 贵得多）。已打开且**没有未保存改动**的文件被外部改写时就地重载（保留滚动位置、不重建编辑器；刚由本页保存的文件在 3s 内忽略回声，见「变更监听」）。
- **变更面板在左栏**（不是底部）：回答「我现在改了什么、要不要提交」——编码时随时要看，放左侧常驻；底部留给「分支 / 日志 / 提交内容」三栏，那是回顾历史时才看的，两者节奏不同。提交框 sticky 在面板底部，改动行内的 stage/unstage/放弃按钮 **hover 才显形**（列表保持干净；触屏媒体查询下常显，否则点不到）。面板**头部一行 34px**（与资源管理器头部、编辑器标签栏同高）：范围芯片（仅当前目录 ⇄ 整仓库，无子目录前缀时整块不显示）+ **视图切换**（**一个**图标按钮：点一下在列表 / 树之间切，图标 = 当前视图、title 说明点下去会变成什么——不用一对互斥按钮，那种写法里总有一个是“当前状态”而不可点）+ 刷新（重取状态并重渲染，与 `F5` 同一条路——“改完想立刻看结果”的预期落在面板自己的按钮上，不依赖宿主下一次刷新时机）；芯片原先挂在列表第一行（会被滚动带走、也不像个控件），且**前缀晚于首次渲染就绪时无人再渲染**——所以前缀改为优先取根清单（`repoPrefixOf`，状态接口不带 `rootPath` 时也能得到），且 `onRootChanged` 先算前缀再刷新。**改动列表两种视图**（`files/changes-tree.ts` 管数据、`files/changes.ts` 只管画）：**列表**（缺省）= 现有按状态的四个分组内平铺，目录写在文件名前；**树** = 各分组内按目录收拢（目录行带后代改动数、可点击折叠，缩进走 `--fw-depth` 行内变量）——两个视图看的是同一批改动，视图选择记在 `gebai.ui.changesView`（未存过 = 列表），切换时带**滚动位置**（滚动容器每次渲染重建，不复位等于把“我看到哪了”丢掉），折叠状态不持久化（刷新即展开）。提交框的动作**全在一行、右对齐**（`修补` + 历史信息在左，`推送` / `提交` 在右；按钮组必须在**最窄左栏**（180px，内容宽 164）里完整排下，“提交并推送”六字会把这一组撑到 185px、栏一拖窄主按钮就被顶出可视区，故按钮文字取“推送”、完整语义进 title）：`.fw-commit-opts` 与 `.fw-commit-btns` 各自不可折、不可压缩，栏宽不够时只在这两组**之间**折行（按钮永远同行同右缘，不会被拆开或顶出可视区）；修正了原先 `flex-wrap: wrap` 把两个按钮拆到两行且“提交”跑到左边的问题。状态文本（几个文件已暂存 / 未暂存将包含全部改动）改由按钮 title 承担：它本来就不该占这一行，而“N 个文件已暂存”在分组表头里本就有。**不再提供「署名」（`--signoff`）开关**：它是仓库/团队的固定习惯而非逐次决策，服务端仍支持该参数（API 未变），只是界面不再摆一个几乎不改的勾选框。**左栏宽度下限 = 提交框动作行的实测宽度**（`--fw-left-min`，面板每次渲染后回调上报；算术在 `files/panel-width.ts`，纯函数带单测）：拖动夹取与 CSS `min-width` 共用同一个值，两个提交按钮永远同行、不会被裁掉——写死的数字在改动文案或主题字体一变就静默失效，而症状只是“按钮被裁半个/挤成两行”。量的时候必须等节点进文档（面板渲染是先建树后 append，建树时量出来是 0），也不计入可省略项（它们本就能压到 0）。
  - 底部三栏从左到右是**一条动线**：点分支 → 看它的日志 → 点某条提交 → 右栏看这条提交改了什么。分支栏与标签栏的**单击即把日志切到这个引用**（当前范围显示在日志栏头部的范围选择器上，并在对应行上高亮；检出仍在右键菜单里），日志栏自己也能换范围。**「已检出」与「正在看它的日志」是两件事，视觉上也必须分开**：前者是仓库状态、靠行首 ✓ 图标（图标转强调色）表达，**不给行底色**；后者是视图状态、是列表里**唯一**带底色的行（浅底 + 左侧色块）。否则同一行既是“当前分支”又是“正在看”，两种含义糊在一起分不清；两个状态的文案也都写进 `aria-label`（视觉靠图标/底色，读屏只能靠文字）。日志栏**只放日志**（提交详情不再就地展开——否则「看列表」与「看内容」互相打断，返回后滚动位置也丢）；当前选中的提交在列表里高亮，栏标题右侧提示当前哈希。
  - **三栏头部各只有一行、且都不画底线**：分支栏头部 = 标题行小切换（分支/标签/储存/远程，**栏头不放「分支」标题**：切换组自己就说明了栏内是什么，标题只占宽度）；日志栏头部 = 标题 + 范围选择器 + 过滤输入 + 过滤芯片 + 刷新（旧实现搜索框独占一条过滤条）；提交内容栏头部 = 标题 + 选中提交的短哈希 + 「与工作区比较 / 整提交差异」+ 就地关闭按钮（后两者选中提交后才出现，详情内不再摆操作条；短哈希也只在选中时出现——**空态不摆「点击日志查看」这类引导文案**，空态不需要教用户怎么用）。头部行内控件统一 20px 高。头部底线全部去掉的依据是上面那条判据：头部底色 `--bg-elev` 与下面列表的 `--bg` 已经不同，再画线就是重复信息。**日志范围选择器**（IDEA 的 `Log: <branch>` 口径；分组清单由 `files/git-refs.ts` 给出）常驻日志栏头部：默认显示「全部分支」（= `git log --all`），点开是带搜索的引用浮层（范围 / 本地分支 / 远程分支 / 标签，行与图标复用 `.fw-ref-row` 口径），↑/↓ 移动高亮、Enter 选中、Esc 或点外关闭；已限定在某个引用上时按钮旁多一个 ×（一键回到全部分支）且边框转强调色。范围**不做成过滤芯片**——选择器已经显示它并自带清除，同一行里再重复一份只占宽度。
  - **过滤条件芯片只给「别处看不见的状态」**：文件路径 / 作者 / 时间范围三者的当前值分别来自资源管理器右键、日志行右键、日期菜单，头部没有它们的载体，所以摆成芯片并自带清除；**提交信息过滤不做芯片**——载体就是那个输入框（写着什么就是在过滤什么），回车即生效、输入框内自带 × 一键清除（清空即取消），再摆一个标签只占宽度（与范围选择器同理）。输入框在过滤生效时加 `filtering` 类亮边框，作为「条件已生效」的指示；清除按钮有内容才显形，并统一把「过滤条件」与「输入框内容」一起置空（两者不同步会残留一个看不见的过滤条件，日志少了却不知道为什么）。
  - **分支栏工具条只放动作图标，不放状态文本**（新建 + 检出 + 删除 + 抓取 + 拉取 + 同步 + 推送 + 刷新；「当前 master → origin/master」这类信息不显示——当前分支在状态栏的分支项、行内 ✓ 勾与高亮已有，重复展示只占宽度）。除刷新外全部左对齐，刷新贴右缘（与三栏头部的刷新同一视觉锚点）。工具条**不画下边线**（它下面就是本栏自己的列表，且两侧底色不同）。同样因为它 sticky，**背景必须垫实**：单层 `--bg-elev` 在 acrylic（0.82）下会让滚过去的行从底下透出来、与图标叠在一起——按本仓已有手法叠两层背景达 ~95%（`style-contract.test.ts` 有一条守卫：凡 `position: sticky` 的规则必须垫实或显式豁免）。**栏宽下限 = 工具条（按钮组 + 计数文本）的实测宽度**（不是写死的数字）：工具条每次重建后量一遍（只排除 `.fw-grow` 占位空白那一个弹性项），写入 `--git-col-a-min` 供 CSS `min-width` 与拖动夹取两处共用（算术在 `files/git-cols.ts`，纯函数带单测）——按钮随 tab 变、还会增减，写死的值在加按钮后静默失效（栏拖窄时按钮被裁掉半个，看不出是设计如此还是坏了）。**计数文本（「N 个标签 / N 条储存」）也计入**：它虽是唯一的可省略项，但只算按钮时栏能拖到「计数被省略成省略号、与按钮挤在一起」的宽度——现在的口径是**下限 ≥ 内容宽度**（+4px：2px 取整余量 + 2px 呼吸位），代价是这几个 tab 的栏不能比工具条更窄；计数文本的字号另降一号（10.5px），它不应与按钮抢视觉重心。检出 / 删除各弹一份本地分支清单菜单（检出勾选当前分支并置灰；删除排除当前分支——git 不允许删自己）；抓取 = `fetch --all --prune`（抓全部远程，与当前分支的上游无关，图标用云+箭头 fetch；单个远程的抓取在远程栏双击/右键）；同步 = 先拉取后推送（拉取失败即停不推，图标用 sync 双向箭头——拉取/推送各用 download/upload 单向箭头）；推送走 `pushBranch`（有上游按上游推、无上游择远程发布），领先数 ↑N 写进 tooltip；拉取/同步需有上游（无则置灰并在 tooltip 说明）；四个远程按钮包在 `.fw-remote-actions` 里，在途禁用与远程栏同一口径。**分支行只在当前分支前留 ✓**：其余行不再逐行摆一个相同的分支图标（列表本来就在「分支」栏里，重复的图标只是噪声），「我站在哪个分支上」才需要一行一个记号。
  - **远程栏的按钮同理只留高频**：抓取 / 拉取 / 推送 三个图标 + 「更多」菜单（仅快进的拉取 / 变基拉取 / 强制推送（`--force-with-lease`）/ 添加远程），共一行、**与分支栏一样左对齐**（只有刷新贴右缘；右对齐时按钮跟在一段空白后面，眼睛要在整栏宽里找它）；变体与低频管理动作本就不是每屏都要看到的东西，平铺七个带文字的按钮会在 200px 宽的栏里折成 169px 高的一堆，把远程列表挤到看不见。状态文本只留「别处说不了」的那一种：**未设置上游**（拉取/同步不可用、推送是首次发布），跟在按钮组后面；已设上游时上游名写进拉取按钮的 tooltip（与分支栏同一口径）——栏宽下限是工具条的实际内容宽，固定占一句“跟踪 origin/…”会把栏白白顶宽。**远程行名称与地址分两行**（`.fw-remote-main` 列容器）：两者都长（地址尤其），挤在一行时地址必被截成看不出是哪个平台的半截；地址**允许折行**（`overflow-wrap: anywhere`，不用省略号）——既然特意给它单起一行就是为了看得完整，截断就白分了，无空格长串不写任何断行规则也断不开。
  - **分支清单的「远程」靠 ref 命名空间判定，不看名字里有没有斜杠**（`%(refname)` 以 `refs/remotes/` 开头才算远程）：本地分支完全可以叫 `feature/alpha`，按斜杠判定会把它们整批归进远程分组（分组、排序、图标全错）。同理，日志行引用标签的颜色分类也要拿**已知的远程名首段**去比，不能用「有斜杠」当远程特征。
  - **日志刷新的「历史未变」判定必须连 refs 一起比**（`loadLog` 的 unchanged 分支）：打标签 / 建分支 / 切 HEAD 不产生新提交（hash 全同），但提交行上的分支/标签芯片已经变了——只比 hash 会把这些变化吞掉（刷新后标签不更新）。refs 变了就走重建路径，芯片随之刷新；hash+refs 全同时保留现有列表（不把翻了几页的位置拽回第一页）。
  - **分支右键菜单里的「推送」不能拼成 `git push <branch>`**：git 把无仓库参数时的第一个参数当成**仓库**（远程名或 URL），不是 refspec——`git push feat-x` 报 `'feat-x' does not appear to be a git repository`。必须显式给远程 + `本地:远程` refspec：有上游时按**上游名**推（上游可以叫别的名字，同名推会新建错分支）、无上游时择远程发布同名分支并 `--set-upstream`（多远程且无 origin 时弹菜单选，不替用户猜）。
  - 三栏与左栏宽度都可拖（分界条 hover 变强调色、双击复位；宽度记 localStorage）；左栏与底部工具窗可整体收起（`Ctrl+B` / `Alt+G`），编辑器随之 `layout()` 重排。
- **根提交的 A 侧 = 空树对象**（`EMPTY_TREE`，`4b825dc6…`）：端点里的 `<rev>^`，若 `<rev>` 是**根提交**（无父提交），`normalizeRev` 把它换成 git 空树——根提交相对什么变化不是「错误」而是「相对空树的全量新增」。不归一时 `git diff <root>^ <root>` 直接报 `ambiguous argument`（422），而提交详情把 A 侧与**文件清单**都挂在 `${hash}^` 上 → 跨文件导航的 `gitCompare` 默默 422 → 按钮凭空消失（用户以为「没有更多文件」）。归一只发生在「`X^` 存在且 X 无父」这一种情况：X 本身不合法时仍报 422（若也当空树，就是拿错数据当差分）。空树参与的比较不再走三点（`A...B` 需两个 commit，空树会报 `is a tree, not a commit`）——语义上空树就是共同祖先。
- **体量与失败：把「撑不住」变成看得见的事**（`GEBAI_FS_MAX_DIFF` 默认 8MB / `GEBAI_GIT_MAX_FILE` 默认 1M 字符）：
  - `trimDiffText` 在**文件边界**截断（不在 hunk 中间剪一刀，免得解析出半截 hunk 当真内容）；被截掉的文件用 `--name-status` + `--numstat` **补齐清单与真实 +N/-N**——「这次动了哪些文件」是提交详情的骨架信息，不该随逐行内容一起丢。
  - `contentAt` 先问 `git cat-file -s`（只读元数据）再决定要不要读 blob，超限直接返回 `tooLarge + size`，**不拉正文**；前端 `mountDiffView` 见到 `tooLarge` 就**不建 Monaco model**，改走服务端结构化 hunks + 一行说明（这正是「点开大文件卡死/空白」的源头）。
  - 降级一律带**原因提示条**（`.fw-hint-bar`）：降级本身不可怕，不说一声的降级才可怕（用户会把「没显示」理解成「没改动」）。两侧端点都不存在该文件时也不再给两片空白，而是说明「可能路径不对 / 被删除后未重建」。
  - `truncated` 只在「逐行内容确实被上限截掉」时标；纯重命名/纯改权限（本来就无内容差异）不标——否则「换了个名字」会显示成「大到看不了」。
- **失败不再静默**：`prepareReview`（取跨文件导航清单）原先 `catch {}`，失败与「只有一个文件」在界面上长得一模一样；现在失败给 warn toast（修正前的实测症状：根提交点文件时 422 被吞，导航按钮直接消失）。`spec.fallback` 也不再依赖调用点是否记得传：`fetchFallback` 拿不到就现取一份单文件差异（文件历史对话框那条路径此前就没传）。
- **变更文件遍历（上一个 / 下一个变更文件）**：标签栏上两组导航，语义分两层——**文件内换差异 = `F7`/`Shift+F7`**（与 Monaco 的 diffReview 同义），**跨文件换变更文件 = `F8`/`Shift+F8`**（与合并视图的冲突导航同键，按当前视图分派，见 `files/main.ts` 的 `stepIssue()`）。文件清单按**与当前差异相同的端点对**取（工作区改动 / 某次提交 / 任意两端比较通吃），语义是**同一标签内换文件**（不给每个文件堆一个标签），换文件时给标签改 key、标题保留来源后缀，到头双向回卷。注意两个端点约定**不同**且不能混用：取内容用 `gitContent`（`WORKTREE`/`INDEX`/rev），取文件清单用 `gitCompare`（「未暂存」必须写成 from 留空 + `to=WORKTREE`，写成 `from=INDEX` 会被解释成 `--cached` 而静默返回另一批文件）。另：`F7` 被 Monaco 的 diffReview 占用、`Ctrl+Alt+↓/↑` 是它的多光标组合，故这些键一律**捕获阶段**监听，先于编辑器接管。
- **两种同窗形态（会话工作台 ⇄ 文件工作台）**：**会话侧**（标题栏**左侧**、会话列表按钮右边）入口主按钮 = **并列**（iframe 嵌工作台，缺省**停靠在左**、会话区在右，默认五五开、分界可拖双击复位；图标为**外框+中间竖线**——一眼就是"一窗分两栏"），它 **hover 时右方**弹出的副按钮 = **全屏**（文件工作台独占整个窗口、会话工作台收起但 DOM 与状态全留着；图标为**右箭头**——从会话这一侧往文件工作台去）。**文件侧**（工作台活动栏最下方，仅嵌入态）是同一对按钮的反向：主按钮 = **关闭文件工作台**（**左箭头**，恒回会话工作台），它 **hover 时右方**弹出的辅助按钮 = **进入分屏**（图标为分屏两栏；只有全屏态才给——并列态它就是"再进一次并列"，没有去处；窗口容不下并列时宿主经 `gebai:files-mode` 的 `canSplit` 告知，也不给）。消息流里的文件产物（文件卡 / 链接 / 原文件弹窗）走的是另一条路（新标签打开整个工作台页面，自动带上所属会话）。
  - **同窗三形态**（`SplitMode`：`off` / `split` / `solo`，归一在 `files-split-core.ts`）：会话侧主按钮管 `off ⇄ split`（窗口容不下并列时退化为 `off ⇄ solo`）、副按钮管 `off/split → solo`；文件侧主按钮恒为 `→ off`（关闭文件工作台）、辅助按钮管 `solo → split`。`Ctrl+\` 同一个键管三种（连按两次总能回到会话桌面），**这个键只标在会话侧主按钮上**：副按钮标它会变成“承诺一件它做不到的事”（`Ctrl+\` 在 off 态开的是并列，全屏没有快捷键）。**全屏态的关闭一律回会话工作台**（不再退回进入前的形态——想并列就点那颗“进入分屏”，一个动作一个出口），切形态**从不重载 iframe**（标签、滚动位置、Git 状态照旧）。全屏态下 `header` / `aside` / `main` 整块收起、面板吃满整个窗口（`body.files-split-solo`）、分界拖条隐藏。窄窗口（< 1100px）下会话侧主按钮就是全屏开关、副按钮收起：**分屏按钮不承诺做不到的事**（判据 `files-split-core.splitFitsWindow`，入口与动作同源）。
  - **分屏布局（停靠侧 + 宽度，列序随侧互换）**：`#app` 分屏时列变成 `1fr auto`（面板在右）/ `auto 1fr`（面板在左），面板**宽度由它自己的 `width: var(--files-split-w, 50vw)` 承担**、`auto` 列跟着它走；面板占第**整个窗口高度**（网格 `grid-row: 1 / -1`）——工作台自己是 IDE 式界面（自带顶栏与状态栏），头顶再压一条宿主标题栏只是白占编辑区的垂直高度；标题栏收在**会话区那一列上方**（面板在右 → `grid-column: 1`；面板在左 → `2`），不再横跨整宽。宽度放面板上而不是网格列里：这一个变量同时喂给 iframe（`.files-split-frame` 的 `width`）与主题太阳的定位，只留一处真相；`grid-template-columns` 里写死 var 也不合适（列宽要的是「跟着面板走」，插值在旧 WebView 上会静默退化成瞬变）。折叠/展开**不动宽度**（面板整体平移，见下一条），拖分界时 `auto` 列跟着面板即时跟上、不经过任何插值。
  - **停靠侧可换（会话与文件左右互换）**：`gebai.ui.filesSplitSide`（`left` 缺省 / `right`）持久化，工作台「更多」菜单里「分屏停靠改到左(右)侧」一项即刻换侧——宿主只改 `body[data-files-split-side]`，列序 / 标题栏列 / 分界线朝向 / 拖条贴哪条边 / 主题太阳以哪块为中心全由 CSS 按它分支，**iframe 不重载**（换侧不丢工作台的标签 / 滚动 / Git 状态）；分界线不再由面板边框画（否则 iframe 会被推离面板边缘 1px），只由分界拖条的 1px 线画。宽度换算与停靠侧归一化抽在 `files-split-core.ts`（纯函数 + `files-split-core.test.ts`）：左停靠取「指针 x − 面板左缘」、右停靠取「窗口宽 − 指针 x」。
  - **过渡是 200ms 的合成层动作（并列滑入 / 滑出，涉及全屏的切换淡入 / 淡出）**：`#files-split.anim` 只在过渡期间打开（`transition: transform 0.2s cubic-bezier(.22,.61,.36,1), opacity 0.2s ease` + `will-change: transform, opacity`），栅格宽度**一步到位**——整场过渡只重排一次会话，且那一次落在面板已经盖住（展开）/ 已经让出视线（收起）的时刻。**为什么不做宽度过渡**：面板宽度一变，`1fr` 那一列的会话区每帧都要重排**整段会话**（实测：289 条消息 ≈ 50ms/次、794 条 ≈ 141ms/次），220ms 的过渡因此只渲染得出两三帧——看着一跳一跳，消息越多越明显。**为什么并列滑、全屏淡**：一整屏的东西从窗口边缘横滑进来只是拖时间，而面板尺寸在 `split ↔ solo` 之间必然要跳变一次（宽度没法用位移表达）——跳变藏在**起步 / 收尾的透明**里（进全屏 `opacity 0 → 1` + 自停靠侧轻移 24px，出全屏反向；`solo → split` 借并列那套滑入：面板被推到窗外时尺寸已落到停靠位，跳变不在画面上）。**过渡期面板被提出栅格**（`body.files-mode-anim` + `data-files-anim-geo`，见 `holdPane`）：改成固定定位、几何与目标形态一致，于是在它下面切布局（会话区收或放）一点也带不动它——中途不闪底色也不错位；但面板一旦不在栅格里，分屏那列 `auto` 会塌成 0（会话区先铺满整窗、收尾再缩回，跳一下），故过渡期把那一列**显式按面板宽度留出来**（`.files-mode-anim.files-split #app` 的 `grid-template-columns`，与面板自己的 `width` 同值），**并把会话正文 `main` 显式钉在会话那一列**（面板在左 → 第 2 列，在右 → 第 1 列；与标题栏同列）——否则面板离栅格后自动排布会把正文翻到第 1 列去（实测 x 从 640 跳到 0、收尾再跳回），半透明面板下看得见。**布局何时落地**：并列两条路在起止两侧各自落（面板还盖着 / 已经让开），涉及全屏的那几条留到收尾 `commitLayout()` 才落——那时的会话区收放发生在面板已不透光（或已藏起）时，否则会在半透明面板下被看见。iframe 绝对定位、宽度与高度**都显式给**（`width: var(--files-split-w, 50vw)`、`height: 100%`）——宽度恒取变量、平移期间尺寸自始至终不变，工作台**内部不重排**（Monaco 不会被挤成 0 宽反复 relayout）。**高度为什么不能靠 `top: 0; bottom: 0` 夹**：iframe 是**替换元素**，绝对定位下上下夹 + `height: auto` 不会拉满，而是退回固有尺寸 300×**150**——实测症状：面板 720 高、工作台只剩顶上 150px 一条（状态栏被挤到 y≈140 与树重叠），下面全是面板底色。推开时先把起步态（内联 `transform`：并列推到窗外 `translateX(±100%)`、全屏只轻移 24px，全屏另带 `opacity: 0`）写上并强制一帧再撤掉——否则浏览器只看到「hidden → 显示 + 归位」一次状态变化，没有插值起点，过渡根本不发生。**写起步态时必须先把 `.anim` 摘掉**（写→强制一帧→加 `.anim`→撤值）：面板已可见时（如 `split ↔ solo`），带着过渡写起步态会**自己起一条随即被取消的过渡**，浏览器用 `elapsedTime ≈ 0` 的 transitionend 给它收尾——“过渡已结束”因此被当场判成事实，整段过渡一跳到位（实测症状就是硬切）。收尾由 `transitionend`（只认面板自身的位移：`propertyName === "transform"` 且 `e.target === 面板`——iframe 内部的过渡会冒泡上来；另加**时长守卫**）与兜底定时器共同保证（动画被换侧 / 连点打断时 `.anim` 不能永远留着，否则面板一直挂在合成层上）；拖动分界 / 双击复位 / 窗口缩放都没有过渡（`.anim` 不在场，宽度要跟手）；`prefers-reduced-motion: reduce` 下直接到位；窗口窄到 1100px 以下的自动退出不播动画。
  - **为什么把并列放主按钮**：主按钮是常驻可见的那个，该承担**最常做的动作**——一边让 Agent 改文件、一边自己核对，正是并列的存在理由；而"看一整屏文件"是少数情况，交给悬浮翻出的副按钮（IDEA 工具窗也是常驻优先、窗口化次之）。并列开着时主按钮带 `.icon-btn.active`（全站通用的"已开启"语义，轮盘按钮同款）——标题栏没有 ✕，开关态由按钮自己表达。
  - 关闭同窗形态**不是标题栏上的 ✕**，而是面板内与全局几条路径：工作台活动栏最下方那颗按钮（**关闭文件工作台**，左箭头，并列 / 全屏都是它）、它边上悬浮弹出的「进入分屏」、工作台「更多」菜单同名项、`Ctrl+\`（面板内），以及点标题栏的「会话列表」（并列时它本就被折叠，点它＝把列表拿回来，顺带关并列）；全屏下另有一条 `Esc` 兜底（走键位表的**浮层作用域**，见下）——面板万一没加载出来时手上得有一条退路。关闭图标是**固定的左箭头**（不随停靠侧翻转：它表达的是“回会话工作台”，与宿主那颗右箭头互为反向），与 `split`（进入分屏）、`swap`（左右互换）同住在 `files/ui.ts`；形态与 `canSplit` 由宿主经 `gebai:files-mode` 推给工作台。
  - 同窗用 **iframe 而非组件化**：工作台本就是独立入口（Monaco + Git + 完整状态模型），iframe 保住单实例、故障隔离与状态独立；代价是主题同步、**停靠侧同步**、**形态同步**与「关闭」这几个跨界动作要走 `postMessage` 桥接（`gebai:theme` / `gebai:files-split-side` / `gebai:files-mode`（含 `canSplit`）宿主→子页，`gebai:files-close-split` / `gebai:files-enter-split` / `gebai:files-split-swap` 子页→宿主；嵌入态下工作台的「返回歌白主界面」自动变为「关闭文件工作台」）。关闭同窗**不销毁 iframe**（IDE 里工具窗关掉再开也是原样）。宽度与停靠侧持久化，**形态同样持久化**（`gebai.ui.filesSplitOpen` 存 `split`/`solo`；旧版存的 `"1"` 在 `normalizeSplitMode` 里平移到 `split`）：刷新页面后自动把并列 / 全屏重新拉起来（延到空闲期，不让一个重工作台与主界面首屏数据抢带宽），用户主动关闭时清除记忆，而**因窗口过窄被自动退出时保留**（那是“窗口放不下”而非“不要分屏”）；记忆是 `split` 而窗口窄于 1100px 下限时**不恢复**（用户要的是并列，悄悄开出一整屏文件工作台是另一种意外）。
  - **面板是纯容器，没有标题栏**：只有分界拖条 + iframe。那条 30px 横条要花掉整个编辑区的垂直空间，而它的按钮各有去处——**关闭＝也不在标题栏**（走面板内与全局路径：活动栏最下方那颗、工作台「更多」菜单、`Ctrl+\`、全屏态的 `Esc`、点「会话列表」）；打开＝两侧入口的按钮（会话侧两颗 + 工作台活动栏那对）；重新加载／分屏停靠改到左(右)侧／关闭文件工作台＝工作台自己的「更多」菜单（页面级动作归页面自己）。副按钮只表达一件事：**全屏打开文件工作台**（再点一下就回到会话工作台）。全屏态的 `Esc` 兜底走**键位表的浮层作用域**（`pushEscScope` 而不是自挂 document 监听）：分发器按“后推的优先”匹配，所以全屏态下再打开文件预览 / 设置面板时，`Esc` 仍先归那一层——不会把「关预览」抢成「退出全屏」。
  - 悬浮副按钮有一条 6px 空隙，指针穿过时 `:hover` 会丢失——必须用透明「桥」+ 宽限延迟（`::after` 盖住空隙），否则按钮在真实指针下点不到；自动化里 `click()` 是瞬移，测不出来。宿主入口那对（`#files-solo-btn`）与工作台活动栏那对（`.fw-rail-split`）同一套做法。标题栏贴顶，提示气泡默认落在下方正好压住副按钮，故加了 `data-tip-pos="left"` 让该入口的提示贴左侧。
- **差异视图导航（定位差异是一等操作）**：工具条上一组「上一处 / n / m / 下一处」+ 快捷键 `F7`/`Shift+F7`（同效），并把**概览尺**在差异视图打开——右侧竖条标出全部差异位置，配按钮一眼看清「还剩几处、都在哪」。跳转时选中整块并居中；计数以**视口**判定（滚到哪就显示哪一块）。注意 Monaco 的 diff editor **自带** `F7` 绑定且会 `stopPropagation`，故快捷键以**捕获阶段**挂在 document 上先于 Monaco 接管（否则编辑器一获焦，`F7` 就变成 Monaco 自己的行为且与计数不同步）——语义一致，只是计数与按钮态由歌白统管。
- **主题与主界面共用（`theme-core.ts`）**：主题是**用户级偏好**，主界面（`/`）与工作台（`/files`）两个入口必须完全一致，因此把引擎（定义/解析/按需加载/持久化/生效）抽成**不依赖任何界面模块**的 `theme-core.ts`，`theme.ts` 只在其上叠加「🎨 主题面板」并 `export *` 转出引擎（既有调用点零改动）；两侧入口都调同一个 `initTheme()`。工作台**不再有自己的主题设置入口**（rail 上的主题按钮已删）——它有过的坑正是「只读了 localStorage 的 id，没应用人民币面额配色与默认主题黑白变体」，于是主界面选 50 元配色进工作台会跑成 100 元红。
- **顶层级内容区必须显式加入「内容层」（z-index ≥ 1）**：主题的背景装饰是 **fixed 伪元素**（浪潮的太阳 = `#app::after`、`z-index: 0`、视口居中），它们与面板同处一个堆叠上下文；两者 z-index 都是 0/auto 时，层序只由树的先后决定，而伪元素是 `#app` 的**最后**子节点 → 装饰会画到面板上面（"不透明背景"只决定像素怎么盖，不决定层序）。主题自己就是这套约定（`synthwave main { z-index: 1 }`）+ 面板 `#files-split { z-index: 1 }`。**以后再加顶层级内容区（新面板/弹层/分屏）都要跟这一步。**
  - 顺带：浪潮主题在分屏时把太阳改以**会话区中心**为心——面板在右 `left: calc((100vw - var(--files-split-w, 50vw)) / 2)`，面板在左 `calc((100vw + var(--files-split-w, 50vw)) / 2)`，并给它带上 `left` 过渡（`transition: left 0.22s ease`）——栅格一步到位后太阳自己滑到新中心、不会瞬移（收起时栅格在面板出画后才换，太阳随之稍滞后）；否则视口居中的太阳会被不透明面板切掉一半。
- **窄面板优先（分屏常在"半宽多一点"用）**：工作台要能在 ~600px 的面板里正常工作，为此有三条约定——① **活动栏右侧的包裹类必须叫 `.fw-main-right`**（`.fw-right` 是已废弃的"右侧 Git 面板"用过的名字，CSS 里残留过针对它的 ≤1080px 绝对定位规则，撞名会让整块包裹变成一条窄抽屉、编辑区被压成几十像素；那条死规则已删）；② **Git 三栏的堆叠断点取 700px 而非 1180px**——860px 面板下工具窗还有 ~815px，三栏（200＋弹性＋240）摆得下且好用，按 1180 堆叠反而把三栏拆成一列高塔（原规则还写错了属性：给 flex 容器设 `grid-template-columns`，从未生效）；③ **状态栏按优先级退让**——条目逐条打 `data-pri`（1 最要、3 最先隐），≤900px 隐编码/EOL/大小/时间/内核，≤700px 再隐语言/行列；状态栏自身 `overflow:hidden` 兜底（它是窄面板下真正的横向溢出源：条目全 `nowrap` 且不肯收缩，能独自把文档撑出 159px 滚动）。状态栏**不列 stash 计数**：储存（stash）与暂存（index）是两件事，常驻一个数字会把两者混在一起；要看储存时在 Git 工具窗的「储存」栏（那里才是它的家）。
 - **资源管理器头部（单行 + 按需过滤）**：头部只有一行——根选择按钮占主宽（点击弹根清单），右侧只留**刷新 + 更多**两个图标。**不再有常驻的过滤输入框与面包屑**：目录过滤改为按需展开的输入行（`Ctrl+F` 或「更多」菜单入口，`Esc` 收起并清空），排序 / 显示隐藏文件 / 上传 / 折叠全部收进「更多」菜单（当前态用 `✓ ` 前缀标记）——高频动作留图标、低频动作进菜单，省下的三行纵向空间全给树。**新建文件/文件夹不入头部也不入「更多」**：它们是低频动作，而树右键菜单里本就有（选中在哪就在哪建，比头部按钮的动作目标更准），常驻两个「+」只是把头部挤窄。面包屑移除的理由：标签页与树的选中行已分别表达「在看什么文件」与「在哪个目录」，再摆一行面包屑是重复信息。**显示隐藏文件默认开启**（`GEBAI_FS_HIDDEN`，默认 `true`）：根清单（`/api/v1/roots` 的 `showHidden`）到达时宿主把它喂进树（`applyHiddenDefault`），用户一旦在菜单里手动切换过就不再被默认值覆盖——「默认」只管首次，否则「刷新根清单」会把用户刚做的选择弹回去；`/fs/list`、`/fs/tree` 的 `showHidden` 参数**只在显式给出时**覆盖服务端默认（`pickBoolMaybe` 的三态读取），「参数 || 配置」的旧写法会让配置为 `true` 时前端**关不掉**隐藏文件、菜单的勾选态也与实际相反。前端一侧同样得守三态（`files/api.ts` 的 `flag()`）：`true` → `1`、`false` → `0`、不给 → 不带参数——`req()` 会把 `false` 参数**整条丢掉**，直接传布尔值会让「关闭」在请求里退化成「没给」，又回到被默认值接管的原地。
 - **根清单菜单的分组与折叠**（`files/root-menu.ts`，纯函数可单测）：分组顺序 **项目（`proj`，标题就叫「项目」）→ 绑定项目 → 会话工作区 → 其它**，空分组不显示（未配置预置项目时不会留一个空标题）。**会话组默认只列 5 条**（会话名往往很长，全铺出来会把菜单拉成一长条），其余收进「更多会话（N）」子菜单（hover 展开）；**当前所在的会话永远进首屏**（否则「我在哪个会话」要靠猜），它挤掉首屏最后一个而不是额外插一条。
 - **根清单的项目注册表与模型同源**（`routes/fs-shared.ts` 的 `buildRootContext`）：预置项目（`{AGENT}_PROJECTS`）与绑定项目（`{AGENT}_PROJECT`）经 `engine.workbenchProjects` 解析——**env 三层合并：进程全局（.env / 系统注入）∪ 会话内存态 ∪ 请求携带（`?env=` / `body.env`，浏览器本地 env）**，优先级从左到右递增。必须含进程层：项目注册表通常配在 `服务端 .env`，而模型侧（`EnvManager.resolve`）看得到——漏掉进程层就会出现「提示词里有项目、工作台根选择菜单里没有」。
- **地址栏即界面状态（`files/url-state.ts`）**：进目录/开文件同步到 `?root=&path=[&line=]`，于是**刷新回到原处**、**浏览器后退在目录间穿行**、链接可分享。写入策略：目录切换 `pushState`（后退能回上一个目录），同目录内开文件 `replaceState`（否则连开几个文件要按多次后退）；同期变更节流合并成一条历史。与 `deeplink.ts` 分工：那边**解析并落位**（含根推断规则），这边只负责**写回**与从既有参数恢复，共用同一套参数名，匹配逻辑不重复实现。
- **Git 公共构件（`files/git-shared.ts`）**：变更面板（左栏）与 Git 工具窗（底部）是**两个独立挂载点**，但写操作流程（toast 文案统一、成功后刷新状态 + 各自视图）与非仓库占位只该有一份——各写一份很容易出现「一边刷新了另一边没刷新」。`createOpRunner(hooks, after)` 由调用方给 `after`（各自渲染入口），保证写完成两边都刷新。
 - **窄屏与触摸（≤700px / coarse pointer）**：文件工作台在手机上从「四区并列」改为**单列 + 浮层**——① 左栏变抽屉（`.fw-left` `position: fixed` + `translateX(-105%)`，宽度 `min(300px, 82vw)`，配 `.fw-drawer-scrim` 遮罩，点遮罩/Esc/选中文件自动收起；分屏拖条隐藏）；② 底部 Git 工具窗变**整屏面板**（折叠时 `display:none`）；③ 拖宽/拖高改 **Pointer Events + `setPointerCapture`**（`mousedown` 路径保留，所在元素 `touch-action: none`——否则浏览器接管手势会发 `pointercancel`）；④ 目录树**长按=条目菜单**（`LONG_PRESS_MS` 520ms、位移 >10px 作废，按住期间临时关 `draggable`），菜单里的「移动到…」是触屏的移动入口（HTML5 DnD 在触屏不产生手势）；⑤ 命中目标按 `pointer: coarse` 放大（rail 44、树行 44、菜单项 40、标签关闭常显）。**抽屉开关与桌面「左栏收起」记忆解耦**（`leftPanelShown()`）：窄屏开合抽屉不写回桌面态。终端两版式的手机档写在各自己的 `terminal.css` / `terminal-pty.css`（构建后 CSS 顺序不保证，同特异性规则会互相覆盖）。
- **验证**：`core/fs/service.test.ts`（17 例：列举/自然序/越界与软链防护/编码回环/Range/ZIP 含系统 unzip 交叉验证/搜索/根解析）与 `core/git/service.test.ts`（14 例：任意两端对比含 mergeBase、`contentAt` 三端点、`fileDiff`、refs/status/log）；另经真实浏览器（Playwright）冒烟：树展开 / Monaco 打开与语法 token / 只读→编辑切换 / Git 六视图 / 比较视图 / 并列差异 / 端点选择器 / 提交↔提交 对比，控制台零错误。 终端（PTY）另经真实浏览器（Playwright）逐项验证：多标签新建/关闭（激活态唯一）、shell 原生提示符与回显、**真彩色**（`Write-Host -ForegroundColor` 的绿色）、中文输入与输出、`cd` 后状态持续、Backspace 行内编辑、`Ctrl+F` 搜索栏、`Ctrl+C` 中断 `ping -t` 并重建 shell 后继续可用、拖拽工具窗高度后 xterm 自动重排；单测：`core/exec/pty-session.test.ts`（19 例：协议与尺寸、UTF-8 跨块解码、订阅回放与 detach、缓冲上限、中断与世代号、收尾与空闲回收）。
- **端到端回归脚本（`scripts/e2e/`）**：三套 Playwright 脚本共 84 项断言——`files-e2e.mjs`（面板基础能力：初始尺寸与折行、配色两档、搜索高亮/开关/计数、键位、未读与 OSC 标题、粘贴与复制、设置菜单、关闭确认、dead 标签重启、刷新接管）、`files-tui.mjs`（独占模式：vi / less / watch / htop 的备用屏进出与主屏恢复、TUI 中改尺寸、鼠标上报归属、备用屏搜索高亮）、`files-edit.mjs`（行内编辑：光标移动与前删/后删、中段插入、宽字符与代理对、折行接缝、输入法整段提交、vim）。跑法 `bun run e2e:term[:tui|:edit|:all]`（需一份在跑的服务端；截图落 `/tmp/gebai-e2e`）；只作改完手动回归用，不进 `bun test`（需要真终端会话）。
- **客户端**：工作台页面自用薄客端 `files/api.ts`（**同源 fetch**，复用浏览器登录态；`session`/`env` 经查询参数传递，不引入 WS 通道），**不经 `@gebai/sdk`**——该页是独立入口；REST 契约与服务端同源，SDK 侧如需可后续薄封一层。
- **底部工具窗：Git 与终端同槽互斥**：底部工具窗（`.fw-git-dock`）承载两个视图——Git（分支 | 日志 | 提交内容）与终端，共用同一停靠位、同一高度变量（`--git-dock-h`）与同一拖拽条；两者**实例都保留**，切换只切显隐类（`.fw-dock-hidden`），因此 Git 的滚动位置与终端的滚动缓冲/会话都不会丢。可见性与当前视图持久化（`gebai.ui.dockVisible2` / `gebai.ui.dockView`，无记忆时默认收起）；入口＝活动栏终端/Git 按钮、``Ctrl+` ``（终端）/`Alt+G`（Git）、状态栏分支项。
- **终端面板（真 PTY + xterm）**：分两层——**PTY 通道**（Windows ConPTY / POSIX openpty，双平台）与**降级通道**（管道式持久 shell）。
  - **PTY 通道（首选）**：`core/exec/pty-driver.ts` + `core/exec/pty-session.ts` + `ws-handlers/terminal.ts`。伪控制台/伪终端只能经原生 API 创建，而运行时（Bun）无原生绑定（node-pty 系 N-API addon 在 Bun 下实测不可用：fd 静默无输出、resize EBADF；bun:ffi 直调 libc 拿不下控制终端且段错误风险高）：驱动是**一段自包含原生源码**，由**系统自带**编译器编译成临时小可执行（`$TMPDIR/gebai-pty/driver-<hash>[.exe]`，按平台+源码内容哈希命名、改内容自动换文件；启动后后台预热），**不引入任何第三方原生依赖、仓库不存编译产物**。双平台同协议（行分隔 JSON，stdin/stdout 双向，输出走 base64 避开协议混淆）：`open`（shell 命令行 / cwd / 尺寸）· `in` · `resize` · `close` ⇒ `ready` / `out` / `exit` / `error`。
    - **Windows**：C# + `csc.exe`（.NET Framework）→ ConPTY。三个**必须做对**的点（任一错都会让子进程绕过伪控制台、直接写宿主管道）：`lpValue` 直接传 HPCON 句柄本身（不是指向句柄的指针）、`STARTF_USESTDHANDLES` + 三个标准句柄置空、`bInheritHandles=false`。
    - **POSIX（Linux/macOS）**：C + `cc`/`gcc`/`clang` → `openpty`+`setsid`（哈希含平台前缀，多平台共享 /tmp 不串台；musl 下 openpty 内置于 libc，`-lutil` 重试无副作用）。四个**必须做对**的点：① `openpty` 的 `aslave` 必须传真实指针（glibc 无条件写入，NULL 直接段错误）；② 子进程 `setsid()` 后**重开** slave 路径（新会话 leader 首个 tty open 自动成为控制终端，Linux/BSD/macOS 通用，不用 TIOCSCTTY）；③ 子进程 tty 配足**交互式终端语义**（逐项置位，不依赖 openpty 默认值）——输出侧 `OPOST|ONLCR` 是硬要求（内核不把换行翻成回车+换行时，`ls`/日志/报错这类多行输出会逐行右移成阶梯、列全对不齐），输入侧 `ICRNL|IXON` + 行规程 `ISIG|ICANON|ECHO|IEXTEN`（缺则 `\x03` 不再转 SIGINT、行编辑与回显全失）；④ 启动期竞态：openpty 返回的父侧 slave fd 不能立即 close（bash 重开 slave 前的空窗内 poll master 会报伪 HUP/EIO，会话出生即死），须持有到首笔 master 数据或子进程退出再释放。
  - **PTY 通道（首选）**：`core/exec/pty-driver.ts` + `core/exec/pty-session.ts` + `ws-handlers/terminal.ts`。Windows 的伪控制台只能经原生 API（`CreatePseudoConsole` + `CreateProcess` 的 `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`）创建，而运行时（Bun）无原生绑定：驱动是**一段自包含 C#**，由**系统自带**的 .NET Framework 编译器（`csc.exe`）编译成临时 exe（`%TEMP%\gebai-pty\driver-<hash>.exe`，按源码内容哈希命名、改内容自动换文件；启动后后台预热），**不引入任何第三方原生依赖、仓库存不编译产物**。驱动与宿主用行分隔 JSON 协议（stdin/stdout 双向，输出走 base64 避开协议混淆）：`open`（shell 命令行 / cwd / 尺寸）· `in` · `resize` · `close` ⇒ `ready` / `out` / `exit` / `error`。三个**必须做对**的点（任一错都会让子进程绕过伪控制台、直接写宿主管道）：`lpValue` 直接传 HPCON 句柄本身（不是指向句柄的指针）、`STARTF_USESTDHANDLES` + 三个标准句柄置空、`bInheritHandles=false`。会话模型：服务端持有会话表 + 回放缓冲（订阅即回放，重连 / 刷新不丢屏），驱动世代号（`gen`）作废被杀进程的晚到回调（否则中断后会立刻被旧驱动的退出回调标成「已结束」）。
  - **通信通道**：WS（`/ws` 的 `term.open` / `term.attach` / `term.input` / `term.resize` / `term.interrupt` / `term.close` / `term.list`，推送 `term.out` / `term.exit` / `term.ready` / `term.error`）——终端是**字节级交互**（每次按键 / Tab 补全 / 方向键都要即时进 shell），250ms 轮询做不到。门禁与 REST 终端端点一致（文件工作台开关 / 终端开关 / 沙箱 / 只读）；输入按回车近似切分记入 `audit-fs.jsonl`（`term.exec`，`detail.mode="pty"`）。
  - **中断语义（`term.interrupt`）——分平台**：POSIX 写 `\x03` 进 PTY（termios 行规程转 SIGINT 给前台进程组），**shell 本体存活，cd/变量/历史全保留**，与真实终端 Ctrl+C 完全一致；Windows 只能终止进程树并以原 cwd / 尺寸重建 shell（会话 id、回放缓冲、订阅保留）——ConPTY 下向输入管道写 ETX 不会生成 CTRL_C_EVENT（实测 `ping -t` / `timeout /t` 均不停），代价是 shell 内部状态丢失。平台语义集中在 `PtyPlatformSemantics`（`detached`/`softInterrupt`，测试可注入固定）。
  - **前端渲染**：`files/terminal-pty.ts` 用 **xterm.js**（VSCode 同款内核）+ addon（fit / search / web-links）——真彩色、光标定位、清屏、行内编辑与 TUI 全屏程序都由它处理；键盘输入经 `onData` 原样回传（Tab 补全与方向键历史交给 shell），尺寸经 `FitAddon` 量出后回传。资源走 **vendor 静态伺服**（`/vendor/xterm/*.mjs`，稳定文件名、运行时 ESM 动态 `import()`，与 Monaco / mermaid 同一套约定）；为何不用 UMD 构建：xterm 6 的 UMD 包靠 `for (var s in exports)` 挂全局，而这批导出是不可枚举属性，全局拿不到 `Terminal`。
  - **建会话前先量尺寸（首屏尺寸）**：xterm 建出来是默认 80×24，面板尺寸要等它进入可见布局后才量得到（FitAddon）；先发 `term.open` 会让 PTY 以 80×24 起步、屏幕却按真实列数渲染——`COLUMNS/LINES` 错、长行折行错位、TUI 按 80×24 排版，而且只有窗口 resize 才偶然被纠正（建会话期的 resize 带的是前端占位 id，服务端不认识）。现在：先挂上并选中 → 等一帧 → `fit()` → 带真实 cols/rows 发 `term.open`；拿到真会话 id 后再补发一次尺寸，`term.ready` 亦补一次（对齐 open 往返期间拖面板 / 改窗口造成的偏差）。
  - **ConPTY 语义只在 Windows 开**：`windowsPty:{backend:"conpty"}` 影响 xterm 对包装行与光标重绘的处理，POSIX 上启用会与真实 tty 行为不一致；平台按**服务端给的 shell 路径**判定（终端跑在服务端，不在浏览器那台机器上），UA 兜底。
  - **配色**：背景 / 前景 / 光标 / 选区来自界面主题；**16 色 ANSI 调色板不来自主题**——内置 VSCode 默认的暗 / 亮两套（`files/term-theme.ts`，按背景亮度择一），亮色与常规色必须是两档**不同**取值（旧实现 `brightRed` 与 `red` 同值，`\e[91m` 与 `\e[31m` 的语义区别直接消失）；启用 VSCode 默认的**最小对比度 4.5**（对比不足时 xterm 向可读方向调整该色，实测暗底上的 fg-1 被调整到合规色）。`gebai:theme-change` 到达时重算并下发。
  - **查找**：`Ctrl+F` 开右上角查找框，三开关（`Aa` 区分大小写 / `ab` 全词 / `.*` 正则）与「第 n/m 项」计数与 VSCode 同序同义，开关状态持久化（`gebai.ui.termSearchOpts`）。两个坑都在实测里踩过：① **命中底色得我们自己补**——xterm 6 的 decoration 渲染器不读 addon 传的 `backgroundColor`（只按调用方的 `onRender` 设样式，addon 只给当前命中加了一道 1px outline），故底色走 CSS 变量落到 `.xterm-find-result-decoration`（当前命中按内联 outline 判定再加一档）；② **换开关不会自动重算**——addon 按关键词缓存判定，同一关键词下改选项不重建高亮与计数，所以切开关时先 `clearDecorations()` 再查。正则输入过程中必然出现半截式（`[`、`(`），查找调用一律包 catch → 状态显示「正则无效」，不再抛未捕获异常。
  - **键位**：单一来源 `files/term-keys.ts`（纯数据，单测直接跑 `keymap.validateKeymap`；焦点限定终端、捕获阶段）：`Ctrl+Shift+C` / `Ctrl+Insert` 复制，`Ctrl+Shift+V` / `Shift+Insert` 粘贴（中键粘贴同效）、`Ctrl+F` 查找、`Ctrl+=/-/0` 字号、`Ctrl+Shift+K` 清屏、`Ctrl+Shift+A` 全选、`Ctrl+Shift+`` 新建、`Alt+Shift+W` 关闭、`Ctrl+Shift+↑/↓` 切标签、`Ctrl+Shift+Home/End` 滚到顶 / 底、`Ctrl+C` 无选区则中断（有选区即复制）。三条**不能踩**的线：① **shell 的读行键一个不占**——`Ctrl+K`（readline 删至行尾）/`Ctrl+A`/`Ctrl+E`/`Ctrl+W`/`Ctrl+L`（readline 清屏）全部透传给 shell，xterm 会把它们编成 `^K`/`^A`… 发给程序（见 xterm `Keyboard.ts`），占了就是把用户的 shell 习惯改掉；② `Ctrl+Shift+<字母>` 在 xterm 里**不产生字节**（只有 `Ctrl+Shift+-/2/6` 有映射），是干净的空位——清屏/全选都用这一族；③ VSCode 的切标签键 `Ctrl+PageUp/PageDown` **不能抄**：那是 Chromium 的保留组合，页面收不到事件。另外 `Ctrl+滚轮` 接管为字号缩放（浏览器默认是整页缩放）。
  - **全屏程序（vi / less / watch / htop）**：备用屏由 xterm 处理，终端面板额外做四件事——① `windowsPty` 只在 Windows 开（ConPTY 的包装行/重绘语义对 POSIX 真实 tty 不成立）；② TUI 运行时拖面板/改窗口发 `resize` → 驱动 `TIOCSWINSZ` → 程序收 SIGWINCH 重排（实测 `watch 'stty size'` 逐次跟随）；③ **鼠标上报开启时（`term.modes.mouseTrackingMode !== "none"`）中键与右键都归程序**（vim `set mouse=a`、htop 之类），Shift+右键强制调出面板菜单、Shift+拖动仍可绕开上报选文本（xterm 自带规则）；④ 备用屏里的搜索命中高亮要**显式恢复显示**（xterm 6 的装饰渲染器对备用屏一律 `display:none`；实测位置与文字完全对齐，放开是对的）。
  - **浮层在终端上的 Esc**：菜单（右键 / 标签 / 设置）不搬焦点，所以它的 Esc 必须**含终端焦点且走捕获阶段**（`pushEscScope(..., { includeTerminal: true })`）——xterm 在自己的 textarea 上就 `preventDefault + stopPropagation` 把按键吃掉并向 shell 发 ESC，document 冒泡阶段的绑定根本收不到。修之前的表现有两层：菜单关不掉；同时那枚孤立 ESC 穿透给了 shell——readline 进入 ESC 前缀态，**后续括号粘贴被它吃掉**（回显成 `[200~`），vim 则直接退出插入模式。
  - **偏好**：字号与「跟随当前根」沿用降级实现的两个键（`gebai.ui.termFontSize` / `gebai.ui.termFollowRoot`，两版式共用一份字号），行高 / 光标样式 / 光标闪烁 / 选中即复制 / Ctrl+滚轮缩放收在 `gebai.ui.termPrefs`（`files/term-prefs.ts`，逐项容错解析：缺键回落默认值而不是最小字号）。设置入口是标题栏的轮盘 / 「设置」菜单。
  - **标签**：名字按「用户重命名 > shell 的 OSC 标题 > shell 名」取（`files/term-tabs.ts`），双击或右键重命名，tooltip 带出 shell / 工作目录 / 退出码；非活动标签有新输出点亮未读点，切回即消；进程结束的标签标 dead，按 Enter 就地重启（不用先关再建）。
  - **关闭确认**：有输出在跑的标签关闭前先问一次（与 VSCode `confirmOnKill` 同语义）；判据是**近似**的——最近 1.5s 内有输出（精确判定要看 PTY 里的前台进程组，需驱动侧上报，见已知边界）。
  - **剪贴板**：写剪贴板失败（非安全上下文 / 权限受限）回退 `execCommand("copy")`（隐藏 textarea）；读失败时给出可行路径（原生粘贴事件不经过权限接口，故提示 Ctrl+V / Shift+Insert / 右键）。
  - **降级通道**：驱动不可用（无编译器：Windows 缺 .NET Framework / POSIX 缺 cc·gcc·clang / 编译失败 / 其他平台）时 `/api/v1/terminal/info` 回 `pty:false` + 中文原因（含安装指引：Linux 装 `build-essential`、macOS 执行 `xcode-select --install`），前端门面（`files/terminal.ts`）落到**管道式持久 shell**（`core/exec/term-session.ts` + `routes/terminal.ts` 的 `/api/v1/terminal/{info,create,input,read,interrupt,close,list}`，REST + 250ms 增量轮询）：常驻 shell 子进程（Windows 默认 `cmd.exe`，POSIX 默认 `bash`），stdin 保持打开 → `cd`/`set` 在会话内生效；命令边界用**哨兵行**（`echo {TOKEN}%errorlevel%^|%CD%` / `echo "{TOKEN}$?|$PWD"` / PowerShell 等价物），服务端剥离后产出 `{退出码, cwd}`；命令回显与提示符由前端自绘（`files/terminal-legacy.ts` + `terminal-core.ts` 的 ANSI / `\r` / `\b` 解析与缓冲）。POSIX shell 清单：bash → zsh → fish → sh → dash（PATH 探测覆盖 Homebrew 等非系统路径），`$SHELL` 按 id 去重后补充（usrmerge 下 `/bin/bash` 与 `/usr/bin/bash` 同体，按路径比对会重复列出）；shell 以裸路径启动（isatty → 自动进交互模式）。
  - **会话跨刷新接管**：服务端会话与连接解耦（刷新/断线不销毁 shell，订阅即回放缓冲），前端把会话 id 记在 `gebai.ui.termSessions`（`files/term-sessions.ts`，含活动项与实现标识 `pty`/`legacy`）——开/关/切标签时写回，面板激活时先 `term.list` 对账再逐个 `term.attach`，已回收/服务重启造成的失效 id 丢弃，一个都没接管到才新建。不做这件事的后果：并发上限 8，而每刷新一次就新建一条，几轮刷新后新建就全是「已达上限」。接管入口与新建同构做了互斥（`activate()` 会被并发触发：启动阶段恢复工具窗状态 + 用户点活动栏/切视图），否则同一批会话会被 attach 两遍；写回时对 id 去重。降级实现（管道式）同一套规则（`/terminal/list` 对账 + `/terminal/read?since=0` 拉回缓冲）。
  - **安全与资源**：`GEBAI_TERMINAL` 总开关（默认 true）/ `GEBAI_TERMINAL_SHELL`；沙箱启用且非豁免用户 403（终端等同于任意命令执行）；`GEBAI_FS_WRITE=false` 拒绝执行；并发上限 8、空闲 30 分钟回收。
- **Git 面板细节（实现约定）**：① **日志刷新**——`doRefresh` 每次重置到第一页（否则提交后 / `F5` 之后日志停在旧历史），但首页与现有列表一致时不重建 DOM，翻页位置不丢；滚动到底自动续页（监听真正的滚动容器 `colLog`）。② **竞态**——日志加载用代际号 + 根比对判活，切根时旧根的响应被丢弃（早期 `logLoading` 早退会让新根首页整轮不加载）。③ **状态失败 ≠ 不是仓库**——`/git/status` 读失败进 `state.gitStatusError`，面板与状态栏显示可重试的故障态（不再摆出「初始化仓库」误导入口）。④ **在途反馈**——`createOpRunner` 通过 `onBusy` 通知标题栏显示「抓取远程…」等进度，并禁用远程动作按钮。⑤ **栏内状态**——分支/标签/暂存/远程四栏都有加载中 / 读取失败（含重试）与空态，重建内容时保留滚动位置。⑥ **范围与过滤**——日志范围 = 引用（`git log` 的 `ref`，空即 `--all`），由头部范围选择器或分支栏 / 标签栏单击设置，分组与搜索过滤在 `files/git-refs.ts`（纯逻辑、单测覆盖）；过滤支持按文件（资源管理器 / 变更面板 / 提交内容右键「在 Git 日志中筛选」）、按作者（日志行右键）与提交信息关键字，选择器与过滤条常驻不重建（保住输入与焦点）。按文件过滤时范围归零——看单文件历史时再被引用范围裁一刀，多数时候只剩空列表。⑦ **可达性**——列表行 `tabindex` + `role=button` + Enter/Space，分界条可聚焦并用 ←/→ 调宽（Pointer Capture + rAF 合并，窗口 resize 后重新夹宽度）。⑧ **附带补全**——blame 接线（文件标签工具条开关，服务端端点与编辑器行装饰早已就位）、远程分支删除（`git push <remote> --delete`）、`push --tags`、覆盖已有标签显式传 `force`。⑨ **提交图（日志栏左侧泳道）**——布局在 `files/git-graph.ts`（纯逻辑、单测覆盖：车道分配 / 连线锚点 / 颜色槽位 / 路径几何），`files/git.ts` 只做像素换算与 SVG 落地。一条车道 = 「还欠着某个提交一条线」（记期待的提交 hash 与颜色槽位）：提交出现时它所在车道成为节点，**首个「后面还会出现」的父**接管该车道（继承颜色）、其余父另开车道，多分支汇入同一提交时在节点处合并；被消费的车道槽位可复用、尾部空槽裁掉——宽度不随历史长度增长。布局**逐行推导、前缀稳定**：日志是顺序追加分页的，「加载更多」不会让已显示的行重新排布。`complete`（= 已加载到末尾）决定「后面不会再出现的父」要不要留线——按路径过滤（`git log -- <path>`）时父链常断裂，否则线会挂满整个列表。渲染：行高**固定 44px**（`.fw-log-row.graph` 的 `height` 与 `ROW_H` 严格同值——它**不能再叠那 1px 行边框**：行边框会让行高成 45px，而 SVG 仍按 44 画，连起来每行漂 1px；行分隔线因而也一并去掉）、一行一个 SVG、图列宽度按全列表最大车道数统一（各行节点左右对齐）、车道多时按图列总宽上限 132px 等比收窄车道宽；节点色取固定 10 色板（**不随主题令牌**——绘图要靠多色区分并行分支，主题里没有这么多稳定语义色），合并提交画空心节点。
- **变更监听（自动刷新：前端长轮询 + 后端 `fs.watch`）**：工作台的目录树、变更面板、Git 装饰与已打开的干净文件都要「自己变」——用户在终端里跑 git、Agent 在另一个会话里改文件、别的编辑器保存，页面此前只能靠手动 `F5`。形态是**前端长轮询 + 后端事件**：前端循环请求 `GET /api/v1/fs/watch?root=&dirs=&rev=N&wait=20&git=1`，把「当前关心的根内目录」（根 + 已展开目录 + 打开文件的父目录，`WATCH_DIR_CAP = 64`）带上去；服务端只给这些目录挂 `fs.watch`，有变化立刻把在途请求唤醒（对前端等价于推送），没变化就挂到 `wait` 秒再回一个心跳。`rev` 不传 = 只取基线（立即返回），`wait=0` = 即时检查（兜底轮询用），`git=0` = 不监听 git 元数据。
  - **成本控制（本机制存在的主要理由）**：① **只监听请求点名的目录**，不做递归全仓监听（大仓的 inotify watch 数量会直接顶到上限）；② 同名目录在多个连接/多轮请求之间**共享同一个 watcher**（引用计数 + 45s 宽限期，前端长轮询循环之间不断流；宽限期到期由定时器回收，不依赖「下一次有人订阅」）；③ **事件合并**（120ms debounce）——一次保存/一次 git 操作往往触发一串事件，合并成一拍通知；④ 目录挂不上（ENOSPC/EMFILE/被删）**记失败并退避** 30s，不每轮重试；⑤ **无变化时零工作**：长轮询挂在 Promise 上，不占 CPU、不 spawn 任何 git 进程、不做任何列举；⑥ 变更路径只在 TTL（15s）内保留、超过 200 条降级为 `paths: null`（前端做一次「可见部分全刷」）；⑦ 在途长轮询超过 16 个时 server 端降级为即时检查（背压，不让连接/fd 被吃光）。前端一侧：后台标签页断开在途请求并停轮询（切回前台续），连续失败 3 次后退化为 20s 间隔的纯轮询（接口不可用/中间层掐长连接时页面仍会自己变）。实测空闲 45s：`watch` 3 次（每次挂满 20s）、`git/status` 1 次（仅启动）、`fs/list` 2 次（根 + 预取）——即「空闲时后端无事可做」。
  - **git 元数据单独监听**：仓库外部的 git 动作（内置终端里 `git commit`、切分支）**不改工作区文件**，工作区目录的 watch 完全看不到「历史变了但文件没变」。故另挂 `.git`（index / HEAD / ORIG_HEAD / MERGE_HEAD / packed-refs 等）与 `.git/refs`（**递归**：分支/远程引用各是一层子目录里的文件）——`.git` 也可能是**文件**（worktree/子模块的 `gitdir: …`），此时按它指到真目录。仓库根探测有 30s 缓存（`gitWatchDirs` 不必每轮 spawn 一次 git）。事件带 `git: true` 标记，前端据此只刷 Git 状态与工具窗、不刷目录树。
  - **前端落地**：`createFsWatcher`（`files/watch.ts`）持一条长轮询，目录集变化（展开/折叠）时 `poke()` 立刻断开重连把新目录带上；唤醒后按 400ms 窗口合并，然后 ① `explorer.syncDirs(paths)` 做目录树的增量同步（见「目录树自动刷新」）、② 命中已打开文件时重载该标签（保留滚动位置、不重建编辑器；刚由本页保存的文件 3s 内忽略回声）、③ 600ms（最长 3s）合并刷新一次 Git 状态与变更面板。目录清单归一、指纹、退避与「变化路径 → 目录」换算在 `files/watch-core.ts`（纯函数 + 单测，含**根目录的线上记号 `.`**：空串在逗号分隔里会被丢掉，不换记号的话根目录永远不会被监听——实测踩过）。服务端中枢在 `core/fs/watch.ts`（`FsWatchHub` + `gitWatchDirs`，带单测）；`GEBAI_FS_WATCH=false` 时端点返回 `enabled:false`，前端退化为纯轮询。
  - **自动刷新是静默的（无变化不重绘）**：刷新机制（变更监听、兜底轮询、Git 状态到达）只保证「数据是新鲜的」，**要不要重绘由数据指纹决定**——各面板渲染前先算一次「本面板渲染所依赖的全部输入」的指纹（`files/refresh-guard.ts`：通用 `fingerprint` + 变更清单 / 日志 / 通用清单三个域构造器，纯函数带单测），与上次渲染相同就直接返回。没有这层判据时，一次心跳也会把活动栏、变更列表、提交框、Git 工具窗整列重建一遍——滚动位置回跳、hover 消失、分组折叠态复位、正在输入的提交信息丢焦点与光标（输入法组合中的字直接断掉）。落地各处：变更面板（`panelKey`：状态/变更清单/视图/范围/分组与目录折叠态/编辑历史计划/写权限）与活动栏（视图/显隐/改动数/面板开关/嵌入态与停靠侧）走指纹短路；Git 工具窗的日志、分支、标签、储存、远程、标题栏、引用范围选择器与过滤芯片各自短路（**日志指纹必须含 refs**——打标签 / 建分支 / 切 HEAD 不产生新提交但芯片会变；**相对时间不进指纹**，否则永远判定为「变了」；远程名也进指纹，引用芯片的配色靠它判定）；目录树与 Git 装饰本就逐项比对（`sameEntries` / `dataset.deco`），状态栏与编辑器重载本就有签名与 etag 守卫。四处配套：① **提交框与变更列表容器改为常驻节点**（提交框从「每次渲染重建」改为只定点更新已暂存数 / 推送可用性 / 按钮文案），自动刷新永不触碰用户正在输入的节点；② 引用栏的「加载中…」只在**还没有清单时**出现（首屏 / 失败重试），有数据时的后台刷新是 stale-while-revalidate，不再每拍闪一次加载条；③ 磁盘上的文件被改而自动重载时，**只有当前活动标签**提示一句（后台标签静默重载——没人看见却弹提示，等于刷新在刷存在感）；④ 分组折叠与列表滚动位置跨重绘保留（刷新一下把用户折上的分组展开回去，与“无变化抖动”是同一类毛病）。
  - ⑩ **引用标签的语义色**（对齐 IDEA）：黄=当前分支头（加粗）/ 绿=本地分支 / 紫=远程 / 青=标签，第十个之后折叠为中性色「+N」（tooltip 列出被折叠的清单）——一个提交上挂十几条分支时全铺出来会把标题行挤成半行；判断远程靠**与已知的远程名首段同名**（本地分支也可以带 `/`，纯粹按「有没有斜杠」分会把 `feat/x` 误判成远程）；当前分支的提交行另加浅底。标签自身 `nowrap + flex:none`、标题行 `flex:none`：两者都不允许被压缩，否则标签多时标题会被挤成半行（字被裁一半）。⑪ **日志过滤三件套**——搜索框旁的 `.*`（开=扩展正则 `--extended-regexp`，**关=字面文本** `--fixed-strings`；不能只加前者，git 默认是 BRE、仍是正则，不加 `--fixed-strings` 时「关掉正则」对用户就是尞设）与 `Cc`（`-i`）开关、`日期` 菜单（今天 / 7 天 / 30 天 / 一年 / 自定义，直接把日期串交给 `--since`）以及原有的按文件 / 按作者 / 关键字；三者都进 `logQueryKey`，否则过滤变更会被「同一查询」短路当作普通刷新而不重建。⑫ **提交框的提交信息历史**（时钟按钮，取最近 30 条 subject 去重后点选填入）、**stash 条目的「查看内容差异」**（与 `stash@{n}^` 对比，直接复用任意两端比较视图）。
- **已知边界**：Office 预览非像素级一致（兜底下载打开）；超大文件只读片段并提示下载；无内置 hex 编辑；冲突合并为**文本级三窗格**（不做语义/结构化合并，二进制冲突只能选一侧或外部处理）；未做 Git 子模块详情、LFS；终端 `Ctrl+C` 语义分平台——POSIX 为真 SIGINT（shell 存活、状态保留），Windows 为「终止当前命令并以原 cwd 重建 shell」（ConPTY 不接受 ETX 字节，见上），shell 内部状态（`set` 变量、`cd` 后的位置）中断后回到会话创建时的 cwd；PTY 驱动依赖系统自带编译器（Windows：.NET Framework 的 csc.exe；POSIX：cc/gcc/clang），缺失时终端自动降级为管道式实现（无 TTY 语义，全屏交互式程序不可用；POSIX 装机指引：Linux `build-essential` / macOS `xcode-select --install`）；终端面板的「跟随当前根」只在切换根时补一条 `cd`，不跟踪 shell 内的 `cd`（无法从无提示符的字节流里可靠地读出 cwd）；不做 **shell integration**（命令装饰 / 滚动条命令标记 / cwd 自动跟踪 / 命令间跳转，需 shell 侧注入与 OSC 序列支持）、**拆分终端**、**标签拖拽重排**、**WebGL（GPU）渲染**与 **iTerm2 / Sixel 图片协议**；关闭确认的忙闲判据是近似的（按最近有无输出，而非 PTY 前台进程组——后者需驱动侧上报），所以 `sleep 30` 这类静默命令关标签不会拦。

### 日志系统
- 日志级别：`debug`、`info`、`warn`、`error`
- **最小日志器**（`@gebai/sdk/node` 的 `logger.ts`）：`log.debug/info/warn/error` 按 `GEBAI_LOG_LEVEL` 过滤（`debug` < `info` 默认 < `warn` < `error`；`setLogLevel` 由组合根在 `loadConfig` 后立即调用，早于任何装配日志）；另导出 `logEnabled(level)` 供调用点短路昂贵的参数构造。**只做级别过滤**，不做结构化日志、不引依赖——保持既有排障习惯（stdout/stderr 分流、宿主重定向到 `server.log.*`）。
- 日志走**标准输出/错误**（`log.*` 内部仍是 `console.log/warn/error`），无文件 sink、无轮转；二进制形态由宿主收集（如桌面侧车重定向到 `server.log.*`）
- **协议性输出不走日志器**：宿主/工具据以解析的行直接 `console.*`——启动就绪行 `[gebai] listening on http://…`（桌面启动器靠它取端口）、拉起器状态文件与 `server.log.*`、`dev-reload` 子进程 stderr 转发、启动期致命错误：它们不是「可调级别」的东西，被级别静默会让外部依赖方失效
- 日志脱敏：不记录密码、令牌、密钥明文，敏感字段以 `***` 替代（env 回显经 `maskEnv`）；会话内容默认不落日志

### 数据生命周期

清理任务（`core/session/gc.ts`）随服务端启动执行一次，之后每日周期运行，失败不影响在线任务；可用 `GEBAI_GC_DISABLED=1` 关闭：

- **垃圾回收**：`trash/` 中删除/过期的会话超过保留期（默认 7 天）自动物理清理（按归档日期目录 `trash/YYYY-MM-DD/` 判定）
- **截断文件**：随会话 `tmp/` 整体清理——会话过期归档到 `trash/` 时一并归档，超保留期随 `trash/` 删除（不再单独设置截断文件保留期）
- **反馈数据**：`feedback/` 按日期分片，超过保留期（默认 180 天）自动清理，管理导出不受影响
- **临时文件**：会话 `tmp/` 在会话删除或会话过期（默认 90 天无活跃）时整体清理（含截断文件、附件、产物）
- **会话过期策略**：本地/服务模式均可配置会话闲置过期时间（默认 90 天，按 `chat.json` 最后修改时间判定），过期会话归档到 `trash/` 后可恢复，超保留期删除；**归档会话自助恢复通道**：`POST /api/v1/sessions/:id/restore`（WS `session.restore` 同权）——归属用户或 admin 可把 `trash/{date}/{id}` 目录整体移回分片存储位置（数据/tmp 附件/env 一并恢复），会话已存在返回 409，未归档/无权限统一 404（不泄露他人会话存在性）
- 历史遗留的用户级 `truncated/` 目录（截断文件并入会话 `tmp/` 前的旧数据）由 GC 在**30 天宽限期后直接删除**（不迁移内容，宽限期防与旧版本进程写入冲突）

### 工具审批
- 全局工具：`sh`、`py` 默认需要审批；`read`/`write`/`edit` 默认无需审批（`file` 的 `delete` 动作**动态需审批**——递归且不可恢复，能力上甚于一次 `sh rm`）
- **免审白名单强制（`approval:false` 不再无条件生效）**：`sh` 的免审标记经服务端强制校验（`shApprovalFreeAllowed`）——只读命令（`validateShCommandSafeMode` 全量解析通过）或测试/静态检查/包管理器 test 子命令（`bun test`/`npm run <脚本名>`/`pytest`/`tsc`/`eslint`/`go test` 等，`run` 仅脚本名形态、禁文件直跑）放行免审，其余（含 `curl`/`wget`、命令替换、输出重定向、无法识别结构）一律仍走审批——防提示词注入诱导模型自行声明免审执行任意命令；**安全模式例外**：sh 在工具内按只读白名单降级（风险已由白名单约束），免审标记直接生效；**`py`/`js` 的 code 为任意代码**：`py` 免审标记不生效（恒需审批）；`js` 按词元扫描放行——纯数据加工/工具编排代码（无 `fetch`/`WebSocket`/`Bun.*`（除 `Bun.file`）/`process.env`/动态 import 等网络外发、进程、敏感读取通道）免审生效，含上述通道仍需审批（fail-closed，字符串误报只是多一次审批）
- 子Agent工具：通过 `requiresApproval` 声明（仅对**独有工具**生效——编码类子Agent 不再重复定义文件工具，全局 write/edit/patch 维持默认免审批姿态；静态 `true` 会覆盖工具自身的函数形态声明，须保留动态判定的工具不要静态声明）；`task` 子Agent 的 `task_add`/`task_update`/`task_remove`/`task_run`/`task_cancel`/`task_files` 默认需要审批（任务 = 无人值守执行，见「统一任务管理」）
^- `js`：**默认审批一次覆盖整个脚本含内部工具调用**（代码已经用户审阅；`approval:false` 免审运行时内部需审批工具在 RPC 分发层被拒——脚本体未经审阅不得免审执行审批工具）；**动态审批机制**：`Tool.requiresApproval` 支持函数形态 `(args, ctx) => boolean`，引擎在审批点解析（函数异常按需审批 fail-safe））；嵌套调用的 params 中自带 `approval: false` 不改变外层审批姿态（分发层按剥离免审标记后的姿态解析，防脚本内自我免审）
- 会话级跳过：`/approval-skip` 命令；**会话运行中开启即时生效**——引擎审批点实时判定（任务 env 快照或会话内存态 env 任一为 `true` 即跳过，前端开启时自动通过当前等待中的审批卡片，后续审批直接跳过；关闭需下次任务生效）；**用户本人可设置自己的会话**（`GEBAI_APPROVAL_SKIP` 写入会话内存态 env，不落盘——非管理员仍受路径/脚本/网络沙箱完整约束；ask 填值分支模型驱动通道服务模式下一律拒绝）
- 请求级跳过/收紧（REST `prompt`/`chat` body 的 `autoApprove` 布尔，任务级生效不持久化）：`true` 映射任务级 `approvalPolicy=auto`（需审批工具自动通过，含服务模式——调用方即用户本人，与会话级跳过同一授权面）；`false` 映射 `deny`（无交互通道下需审批工具直接拒绝，本地模式同样生效，不空等超时）；见「交互模式 → 审批策略按模式分级」
- 审批被拒/超时**只计数展示**（`event.approval.request` 的 `retries`），**不设次数上限**；模型连续重复调用由重复检测终止（`MAX_REPEAT_HITS=3`/`MAX_REPEAT_STALLS=2`）

### 工具选择

工具集支持**全局开关控制**，可按需启用/禁用任意工具（含子Agent 工具），多级配置：

| 层级 | 配置 | 说明 |
|------|------|------|
| 全局 | `GEBAI_TOOL_ENABLE` / `GEBAI_TOOL_DISABLE`（逗号分隔） | 进程启动时生效，所有用户/会话共享 |

- **白名单优先**：`GEBAI_TOOL_ENABLE` 声明后仅启用列表内工具；`GEBAI_TOOL_DISABLE` 排除指定工具；两者同时配置时先白名单后黑名单
- **粒度**：全局工具按名称（`sh`/`read`/`edit`…）；子Agent 工具按 `{agent_name}_{tool_name}` 精确控制，也可按 `{agent_name}_*` 整包禁用
- **禁用效果**：禁用后工具从总Agent schema 中移除，模型不可见、不可调用；已装载子Agent 中被禁用的工具同样不注入
- **管理入口**：经 REST `GET/PATCH /api/v1/tools`（SDK `listTools`/`setToolEnabled`）——**前端暂无图形化开关面板**（设置面板注明「工具启停经 API 使用」）；保存后新任务生效
- **用途**：安全收紧（如生产环境禁用 `sh`/`py`）、裁剪上下文、按业务场景定制能力面
- **权限**：REST `PATCH /api/v1/tools` 与 WS 工具启停为**服务端全局状态**（所有用户/会话共享），服务模式下仅管理员可操作（普通用户返回 403）；环境变量级（`GEBAI_TOOL_ENABLE`/`GEBAI_TOOL_DISABLE`）由部署方配置不受此限

### 交互模式

引擎按接入通道区分三种**交互模式**（`engine.run` 的 `interactionMode` 参数，任务级生效），工具通过 `Tool.interaction` 声明**最低可用模式**（缺省 `none` 即全模式可用）；声明高于当前模式时工具被自动禁用（schema 过滤 + 执行阻止，模型收到「当前通道不可用」说明并改用其他方式），替代旧的「等超时」体验：

| 模式 | 语义 | 通道 | 可用工具 |
|------|------|------|----------|
| `none`（**无交互**） | 单次请求：一次调用执行完返回结果，无前端、无往返；**本地模式需审批工具自动通过**（无人可询问）；**服务模式需审批工具直接拒绝**（防普通用户经 REST 免审批执行敏感工具） | REST `POST /sessions/:id/prompt` | 仅 `none` 声明（文件/网络/脚本等） |
| `multi_turn`（**多轮交互**） | 多轮请求-响应往返（非流式），有往返但无前端页面；**仅关键操作（requiresApproval）询问用户**（如飞书审批回调卡片），非关键操作自动 | 飞书机器人 | `none` + `multi_turn` 声明（ask 选择/计划分支走飞书选择卡片；show 图表分支走飞书后端渲染） |
| `realtime`（**实时交互**） | 实时流式交互：完整前端，关键操作询问用户 | WebSocket（Web UI，默认） | 全部 |

- **工具声明**（`interaction: "realtime"` 仅实时前端）：`page_capture`（依赖前端页面配合）；`interaction: "multi_turn"`（至少多轮交互）：无（原 ask_user/plan 的飞书适配由 ask 分支承接）；其余工具缺省 `none`；**合并型工具不做工具级声明**——`show`（图表/HTML/文件三分支）与 `ask`（选择/填值/计划三分支）全模式可见，按 `ctx.interactionMode` 在分支内校验通道能力（show：html 分支仅 realtime、图表分支 none 下引导 `render=backend`；ask：填值分支仅 realtime、选择/计划分支 none 下报「无交互能力」），见「内容展示」/「用户询问」
- **审批策略按模式分级**：无交互模式 `isApprovalSkipped`——**本地模式恒真**（`sh`/`py`/`write`/`edit` 等需审批工具**自动通过**，任务不会卡在审批等待）；**服务模式返回拒绝**（需审批工具在审批点直接拒绝执行，返回「需审批但当前通道无交互」说明，不进入等待——REST 无人可审批，普通用户不得借此免审批执行 shell/任务；管理员可经正式通道设置 `GEBAI_APPROVAL_SKIP` 后执行）；**请求级 `autoApprove` 显式覆盖**（REST `prompt`/`chat` body 布尔字段，映射引擎任务级 `approvalPolicy`）：`true` = 需审批工具自动通过（**含服务模式**——调用方即用户本人，等价其自设 `GEBAI_APPROVAL_SKIP` 会话 env，模型驱动的 ask 填值通道仍拒绝该键，防提示词注入）；`false` = 无交互通道下需审批工具直接拒绝（**本地模式同样生效**——单次调用无人可审批，不空等 5 分钟超时）；缺省 = 通道默认姿态（本地自动/服务拒绝）；`sh`/`py` 的 `approval:false` 按次免审**只作用于交互审批**，服务模式无交互通道按剥离免审标记后的默认审批姿态照常拒绝（引擎 `stripApprovalFlags` 递归删键解析，防模型自行声明免审绕过硬门槛）；多轮交互模式关键操作（requiresApproval）经审批卡片询问用户（飞书审批交互卡片，见「飞书机器人集成 → 审批交互卡片」），非关键操作不打扰；实时交互模式维持询问用户（前端审批卡片）
- **飞书通道** = `interactionMode: "multi_turn"`：realtime 声明的工具（`page_capture`）自动禁用（原 `FEISHU_DISABLED_TOOLS` 名单已移除，由声明统一驱动）；`ask`/`show` 不做工具级禁用（ask 选择/计划分支经飞书选择卡片作答、填值分支明确报错；show html 分支明确报错、图表分支经飞书后端渲染出图），关键操作经审批卡片询问
- **REST 通道** = `interactionMode: "none"`：实时前端工具自动禁用，不再等待至超时；`ask`/`show` 不做工具级禁用（ask 选择/计划分支报「无交互能力」、填值分支引导设置面板；show html 分支明确报错、图表分支直接引导 `render=backend`，均不空等超时）；本地模式需审批工具自动通过，**服务模式需审批工具直接拒绝**（防免审批执行）；审批姿态可经请求级 `autoApprove` 显式覆盖（见上方「审批策略按模式分级」）；需要完整交互能力请走 WS 通道
- **定时/普通/闲时任务执行**（调度器触发的 prompt 型）按**无人值守与否**分流（见「统一任务管理」）：`ephemeral`/`sticky` = `interactionMode: "none"`——无人盯着执行会话，故按无交互语义运行（本地模式需审批工具**自动通过**、不空等 5 分钟审批超时后跳过；服务模式需审批工具在审批点**直接拒绝**并落因；`page_capture` 自动禁用、`show` 图表分支引导后端渲染、`ask` 直接报「无交互能力」），同时触发消息注入一行「无人值守执行」上下文告知模型；`target=session`（绑定用户会话，可能有人在场当场审批）= `realtime` 维持现状
- 禁用判定同时匹配子Agent 命名空间工具（`{agent}_page_capture` 等同名工具同样禁用）；与 `disabledTools` 名单（部署方可另行指定）叠加生效

#### 输出方式（与交互模式正交，同样请求层配置）

**交互模式与输出方式均为请求层配置，服务端引擎内部全部支持**，接入方按自身形式自由组合适配（如业务系统 REST 集成、IM 机器人、Web 前端互不影响）：

| 输出方式 | 语义 | 说明 |
|----------|------|------|
| `final_only`（仅最终响应） | 不推送文本增量（`event.message.delta`）与推理流（`event.message.reasoning`） | 结构化事件（工具调用/审批/`event.message.done` 最终响应/子会话运行过程）仍推送；REST 同步响应即最终文本（content 纯正文，推理在消息独立字段 `Message.reasoning`） |
| `streaming`（流式输出，默认） | 推送文本增量与推理流（含子会话运行过程文本） | Web 前端打字机效果 |

- 引擎 `engine.run` 的 `outputMode` 参数（默认 `streaming`，保持现有 Web/飞书行为）；`final_only` 下文本仍完整落盘会话消息（接入方经存储/最终响应获取完整内容）
- **请求层暴露**（默认值保持各通道现状，传入即覆盖）：
  - REST `POST /api/v1/sessions/:id/prompt`：`interactionMode`（默认 `none`）+ `stream`（默认 `false`=仅最终响应；`true`=流式输出，接入方经 WS 事件订阅消费流）
  - WS `session.prompt`：`interactionMode`（默认 `realtime`）+ `stream`（默认 `true`=流式输出；`false`=仅最终响应）
  - 飞书通道经**接口层**（`BotPromptAdapter`/`EngineBotAdapter`）固定 `multi_turn` + `final_only`（多轮交互 + 仅最终回复）：bot 不直接接触引擎/事件总线，任务内过程事件（工具调用/推理/文本增量）不推送，仅最终回复经回调发送；关键操作（审批/选择）与画图经回调询问/渲染
- 非法 `interactionMode` 值返回 400（REST）/ reply error（WS）

### 待办跟踪

复杂任务支持**待办清单（Todo）跟踪**，Agent 拆解任务、逐项推进，用户全程可见：

- **工具**：`todo`（待办增删改查统一入口——`entries` 为操作列表，每项 `op=add/update/delete`：add 需 `title`（可带 `priority`/`note`/`eta`），update/delete 按 `id` 或 `title` 定位（id 优先，无 id 时 title 精确匹配、唯一包含匹配兜底，多个同名提示改用 id；update 改标题需用 id），省略或空数组 = 查询；返回操作摘要与**当前全部待办状态**快照（含 id），模型一次掌握最新清单，无需再查 id。工具描述引导：新增待办开启新任务时，及时 delete 清理与当前任务无关的历史残留待办——待办只跟踪当前任务，陈旧条目徒增干扰与 token 浪费）
- **状态机**：`pending → in_progress → completed`，异常终止为 `failed`/`cancelled`
- **字段**：标题、状态、优先级、进度（0-100%）、预计耗时（分钟）、依赖项（`TodoItem.dependencies` 类型保留，但 `todo` 工具 schema 与 UI 均未开放）、备注
- **持久化**：随 `chat.json` 的 `todos` 字段持久化（会话级，随会话隔离与恢复；无独立 `todo.json`）
- **Agent 引导**：系统提示词引导模型在复杂多步任务开始时先 `todo` 拆解计划（entries 批量建清单），每完成一步 `todo` 更新，任务结束 `todo`（空 entries）汇报
- **UI 展示**：会话消息流侧边栏实时呈现待办面板（状态/进度/依赖），事件推送 `event.todo.update` 驱动增量更新；`todo` 工具在消息流中渲染为**待办清单卡片**（状态图标 + 标题 + 元信息；状态为内联 SVG 描边图标——待处理空圆/进行中环箭头/已完成圈勾/已失败圈叉/已取消禁止符，随主题着色：进行中沿用主题强调色、已完成/已失败用语义色 `--success`/`--danger`，悬浮提示中文状态标签，元信息行只留优先级（中文高/中/低，纯文本不做视觉区分）/进度/备注），替代通用工具卡片，实时与历史会话一致；清单过长（>8 项）时自动**折叠较早的已完成项**（保留最近 3 项完成作上下文，未完成项始终可见），连续隐藏段收敛为一行「已折叠 N 项已完成」按钮，点击展开、展开后可收起
- **失败恢复**：任务中断后基于待办清单继续执行，跳过已 `completed` 项，从剩余项恢复
- **待办续做**：每轮会话完成（模型给出最终回复）后，引擎自动检查待办清单——仍有 `pending`/`in_progress` 项时，追加一条「【智体·待办提醒】（智体运行时自动注入，非用户输入）当前会话仍有未完成的待办：…请自行决策：继续执行未完成的待办，或确认其已无需处理后收尾」消息并再次进入工具调用循环，直至待办全部完成、达到续做轮次上限（见常量参考，默认 **1 轮**——待办提醒一事一议、不反复打扰，模型未续做即视为已决策收尾）或模型决策收尾；提示为 **user 角色的软性提醒**（仅陈述未完成事实，继续还是直接收尾由模型自行决策）——消息**落盘即 `role: "user"` + `Message.engineNote: "todo"` 标记**：与用户输入同角色使其随用户消息受上下文保护，并避开思考类模型（DeepSeek thinking 等）**不接受以 assistant 结尾的请求**的约束（视为前缀续写、要求回传 `reasoning_content`，尾部 assistant 提醒会让后续每次调用 400、任务静默中断——实测）；标记用于**展示上与用户自己发的消息区分**（前端渲染为弱化的「引擎提示」虚线通知条 `.msg.engine-note`，非用户气泡、不提供撤回，称谓行显示「引擎提示」；存量数据（标记上线前的 assistant 形态提醒）按内容前缀兜底识别——前缀兜底限定 assistant 角色，用户手打同前缀文本不误判）；持久化进会话历史并推送 `event.todo.continue` 事件（含 round/remaining/messageId/text，前端实时渲染）；**模型对提示的回应为纯文本（未执行任何工具）视为已决定收尾，不再注入**；`completed`/`cancelled`/`failed` 项视为已了结不再续做；回复与上上轮完全相同时附防复述提示（需 ≥2 轮才可能触发，当前 1 轮上限下为休眠路径）；压缩护栏的「本次任务输入」（最新一条用户消息，永不裁剪）**不受引擎提示影响**（isEngineNote 命中则跳过）
- **收尾验证提醒**：与待办续做同机制的兜底纪律——任务结束（无未完成待办）时若**本任务修改过代码文件（write/edit/patch 命中代码扩展名且成功落盘）但全程未运行任何测试/检查类命令**（sh/py 的 command 命中测试/lint/typecheck 关键词；**验证类工具**——名称后缀 `run_tests`，含命名空间形态 `self_optimize_run_tests`；**js 编排**——脚本同时出现命令调用点（`sh(`/`py(`/`bg_task(`/`command:`）与验证关键词；三条通道任一命中即算已验证，见「实现与坑」），追加一条「【智体·收尾验证】（智体运行时自动注入，非用户输入）…请先运行相关测试或检查确认无回归，再给出最终回复；确不适用请说明」消息再续跑一轮（模型跑验证后正常收尾，或说明原因），上限 1 轮防反复打扰；提醒同为 **user 角色软性提醒**（与待办续做同形态：落盘 `role: "user"` + `engineNote: "verify"`，展示为「引擎提示」通知条、回放保持 user），持久化进会话历史并推送 `event.verify.nudge` 事件（含 messageId/text，前端实时渲染）；拒绝/安全模式拦截与 dry_run 不计入修改，md 等非代码文件不触发。**验证识别的实现与坑**（`trackTaskMods`）：① 写类工具取命名空间短名（`code_write` 与全局 `write` 同权），但**验证工具必须用名称后缀正则**——短名取的是「最后一段下划线之后」，`self_optimize_run_tests` 会变成 `tests`，按短名相等判定永远不命中（实测：全部会话 8 次提醒中 5 次是该 bug 引起的误报，模型明明跑过 `self_optimize_run_tests`）；② js 编排是主推的复杂流程通道，脚本内跑验证命令同样计入，但判定要**「命令调用点 + 验证关键词」同时出现**（仅关键词不算——否则 grep/read 搜到 lint/typecheck 字样的任务会被误记为已验证，反向漏掉真该提醒的场景）；③ **会话工作区（`tmp/`）内的文件不计入代码改动**——那是模型的临时产物与测试夹具（为验证某能力随手写的脚本、导出的中间结果），不是需要回归验证的代码改动；计入会让「写夹具测工具」这类任务收尾误报（实测：测试 `show` 工具时写的 `tmp/demo.py` 被当成「1 个代码文件改动」）。判定按 `ctx.resolvePath` 解析后的绝对路径与会话 `tmp/` 真实路径比对（项目绑定子Agent 的相对路径按项目根解析，故项目代码与绝对路径用户代码不受影响）；解析异常按「非会话文件」保守计入，不因解析异常放过真实改动
- **引擎注入消息的角色约定（统一 user + engineNote 标记）**：思考类模型（DeepSeek thinking 等）**不接受以 assistant 结尾的请求**（视为前缀续写、要求回传 `reasoning_content` → 400，实测），而引擎/系统写入的合成消息**注入位置往往就是模型下一次调用的前一条**（待办续做、收尾验证、子会话合入、任务结果写回、全量压缩摘要等）。因此凡引擎而非模型产出的消息一律：**落盘与进模型上下文均为 `user` 角色**（落盘即 user，回放同形，不再区分两套形态），并带 `Message.engineNote` 标记供前端渲染为弱化通知条（与用户自己发的消息区分，不提供撤回）。**内容头一律带智体身份标记 `【智体·类别】`**（`core/support/agent-note.ts` 的 `agentNoteHead`：待办提醒 / 收尾验证 / 子会话「名」已合并 / 定时任务「名」执行结果 / 任务中断）——消息与用户输入同角色，模型只能凭内容判来源，身份头让它一眼看出是运行时自动注入；**命令式提醒**（待办续做、收尾验证要模型去做事）随头附自述行「（智体运行时自动注入，非用户输入）」，避免被当成用户新要求执行。已收口点（全部落 user + 标记）：待办续做提醒（`todo`）/收尾验证提醒（`verify`）/定时任务结果写回（`cron`）/子会话报告合入（`subsession`）——“引擎提示”/“定时任务”/“子会话合入”通知条；**上下文压缩摘要**（loadHistory 按 user 注入，全量压缩 `scope:"all"` 时摘要会落尾）；**子Agent 装载提示词**（role=system，loadHistory 前置到历史最前，不会落尾）。两个配套细节：①**“本次任务输入”定位与裁剪**——压缩硬护栏的“最新一条用户消息”（永不裁剪）与前端会话自动命名取首条输入，均跳过 `isEngineNote` 命中的消息（否则任务末尾的提醒/写回会顶替真输入）；但**护栏裁剪本身不排除引擎提示**（子会话报告可达数千字符且可再生——全文在过程存档/bg_task，不该永占窗口）。②**总兜底**：`llm.ts` 三个 provider 序列化前统一执行**尾部纯文本 assistant 降级为 user**（`demoteTailAssistant`，带 warn 日志）——未预见的来源（存量数据/撤回截断残留/未来新机制）也会在发送前被归一化，不再让整个会话因尾部形态被 400 卡死（若将来确需前缀续写语义，需带回该 assistant 的 `reasoning_content` 并关闭此兜底）。

#### 用户询问（`ask`）

全局工具 `ask` 是**向用户询问并阻塞等待回应的统一入口**（原 `ask_user`/`ask_env`/`plan` 三工具合并），分支按专属参数三选一：**选项询问**（`prompt`+`options`，原 ask_user——方案确认、方向决策）/ **环境变量填值**（`name`，原 ask_env——模型驱动的凭证索取，见「环境变量」章）/ **计划审批**（`title`+`steps`/`content`，原 plan——复杂任务先获批再执行）；协议层不变（`event.choice.request`/`event.env.request` 与 `choice.decide`/`env.decide` 回传通道共用）；结果文案前缀（「用户选择：」「计划已批准」等）与「请审核计划」prompt 前缀是前端卡片识别契约。

**① 选项询问分支**——向用户提出一组选项并**阻塞等待用户回应**，用户的回应作为工具结果返回给模型，据此继续执行：

- **参数**：`prompt`（问题描述）、`options`（选项数组，至少 1 项）、可选 `multi`（布尔，默认 false，true 为多选）
- **选项形式**：每项可为纯文本字符串，或**复杂选项** `{ title, description? }`（UI 按标题 + 说明展示，提交值取 title）
- **回应方式**（三种，均可）：① 点选选项（`multi=true` 时可勾选多项，提交值为选项集合）；② **输入自定义文本**（直接输入自己的答案，不限于给定选项，多选时追加到已勾选项一并提交）；③ **拒绝回答**（不再追问）
- **执行**：引擎发布 `event.choice.request`（携带 `choiceId`/prompt/options/multi；**计划审批分支另携带 `plan` 载荷**——`{title, content, path}` 计划标题/Markdown 全文/文档逻辑路径）并**阻塞等待**；用户经 UI 选择卡片、WS `choice.decide` 或 REST `POST /sessions/:id/choice` 提交后，ask 选项询问分支返回结果注入模型，任务继续：
  - 选项或自定义文本 → 「用户选择：X」
  - 多选 → 「用户选择：X、Y、Z」（以「、」连接）
  - 拒绝回答（WS/REST `refuse: true` 或 option/options 均缺失，引擎侧传 null）→ 「用户拒绝了本次询问…」，模型停止继续询问、基于现有信息自行决策
- **超时**：等待超时（与审批同值，5 分钟）自动取消，返回降级提示让模型自行决策
- **卡片高度与收缩**：审批容器 `#approvals` 整体限高并内部滚动（卡片再多也不会把会话区压没）；单卡另提供两种收缩操作（`web/src/card-fold.ts`，纯高度模型 + DOM 绑定）：
  - 两种控件都位于**卡片外侧上方**的一条操作带（`--ic-handle-h`，18px）——不占卡片内容区、不压住卡片头与正文；内容确实超高时该带才出现（卡片随带高补 `margin-top`）
  - **折叠按钮**（操作带右端）：箭头指向「点下去往哪走」——展开态朝下（点它收起）、折叠态朝上（点它展开），`aria-expanded`/`aria-label` 同步；折叠态把内容裁到预览高度（`--ic-preview-h`，默认 54px）并底部渐隐——保留卡片头与首行内容（问的是什么仍看得见），其余收起
  - **内容区上限统一走一个变量 `--ic-max`**（由 `card-fold.ts` 写卡片上）：折叠态 = 预览高度；**展开态 = 拖出值，未拖过则 = 本尺寸配额**——展开态同样受配额约束，内容超出部分在内容区内部滚动。不这么收口就会出现「未拖过的卡片按内容自然高度无限长」（实测长卡片 1322px 吃满屏，用户还得在卡片区容器里再滚一次才能看到选项）
  - **拖拽把手**（操作带中段）：向下拖到吸附线以下即折叠、向上拉回即展开，中间为任意高度；键盘可及（把手聚焦后 ↑/↓ 每次 40px）
  - **高度配额分档**：容器宽屏取 60dvh；**窄屏（≤860px）另取一道「给会话区留白」的硬上限**——`min(42dvh, 100dvh − 标题栏 − 288px)`。单卡上限同口径分档（宽屏视口 60% / 窄屏 42%）。手机一屏本就只有几百像素，沿用宽屏配额会把对话内容直接挤得看不见
  - **初始态：全展开**——卡片一出现就是完整可读、可直接作答的内容（问题与计划全文正是用户作答的依据），不要求用户先点一次「展开」；空间约束由展开态配额承担（内容超出部分在内容区内部滚动），收缩是用户的**显式动作**（折叠按钮/拖拽）。折叠态只在两种情形出现：用户手动折叠，或同 reqId 重建时继承上一次的折叠态
  - 内容本来就不超过预览高度 + 24px 时不提供折叠（操作带整条隐藏，不引入多余控件）；折叠态与自定义高度写在卡片元素上（`data-folded`/`data-card-h`），**同 choiceId/envId 重建（断线重连事件重放）时继承**，用户刚调好的高度不被重置
- **UI 渲染**：审批容器中渲染**选择卡片**（问题 + 选项按钮/复杂选项行 + 自定义文本输入框 + 拒绝按钮；**自定义答案输入是多行 `textarea`**——用户作答常带 Markdown 列表/代码块，单行输入会把换行吃掉（粘贴同理）；`Enter` 提交、`Shift+Enter` 换行（输入法组合态的回车是选词确认，不当提交），高度随内容自增（上限与 CSS `max-height` 同口径，超出后输入框内部滚动，`border-box` 下自增高度补上边框高免得多出一条滚动条）；多选时选项可勾选、多一个「确认选择」按钮；**携带 `plan` 载荷时卡内顶部内嵌计划全文**——复用计划卡渲染（标题 + Markdown），限高滚动防长计划撑爆审批容器，审批时直接可见不依赖消息流位置，刷新/切回经 attach 快照恢复；**计划区固定 `flex: none`**——卡片内容区是列向 flex 且被 `--ic-max` 限高，计划区若参与收缩（默认 `shrink: 1`，且 `overflow: auto` 使其自动最小尺寸为 0）会被下方选项区挤成一条线、正文被裁掉只剩标题；不收缩则按自然高度铺开（上限 40vh），超出部分由内容区整体滚动承载；**问题文本按 Markdown 渲染**（与计划卡同口径，模型常带列表/行内代码/粗体；选项按钮文本仍是纯文本）,提交即决策（实时，绑定 choiceId），**提交成功卡片随即关闭**（任务结束清理兜底）；**同一 choiceId 重复推送（断线重连事件 seq 重放）替换旧卡不堆叠**；历史裸调用（异常中断、无结果）回退可交互选择卡，提交降级为作为用户消息（「我选择：X」/「我选择：A、B」/「我拒绝回答」）发送
- **选中态视觉约定**：选项选中态为 **accent 实底 + ✓ 标记 + 加粗**（未选中悬停为浅底描边），两者必须一眼可辨——多选卡片的勾选反馈是用户唯一的“已选”确认。两条 CSS 约束：①规则写在各 hover 规则**之后**（同特异性下后出现者生效，避免悬停时把选中态盖成 hover 样式）；②选择器带 `[data-theme]` 前缀提权到 0,4,0——主题样式普遍以 `[data-theme="x"] .choice-opt`（0,2,0，且在本表之后加载）定义选项基础外观，会同特异性盖掉选中态（`matrix` 主题的 `::before` 勾选框同理会盖掉 ✓）。即**主题可改外观、不可改交互态**，与「窄屏抽屉是布局契约、特异性须压过主题」同一约定
- **消息流问答记录卡（等待期不预览）**：ask 选项询问分支调用时**中断当前文本段**（封段），等待作答期间消息流**不渲染问题预览卡**——交互作答由审批容器选择卡片承载（与消息流展示卡上下堆叠会被视为重复卡片）；**结果到达时**在消息流落**问答记录卡**（问题 + 选项展示态（**用户选中的选项高亮**，与交互卡同款选中态；自定义文本/拒绝不命中选项则无高亮） + 头部按结果文案呈现「✓ 用户回答 / ✕ 用户拒绝 / ⏱ 选择超时」+ 回答文本），**问题与回答文本都按 Markdown 渲染**（同计划卡口径；回答块内字号跟随 `.choice-answer` 的 12.5px，不被工具卡正文的 13.5px 规则提走），模型后续回复另起新气泡——问答交换在会话流中完整可见；**历史重载**按持久化参数 + 结果同构渲染记录卡（带结果不再呈现为可交互的未答状态）；子会话运行过程（subsession_run）内的 ask 选项询问分支同样封段、记录卡渲染进折叠容器
- **等待期不误判挂起**：选择/填值/画图/捕获等交互等待事件刷新前端流活跃时间——空闲超时兜底（150s 无数据，高于服务端 LLM 读空闲超时 120s：模型调用假死先由服务端超时上报明确错误，前端看门狗只兜底服务端检测不到的挂起）只按「无任何数据」判定，等待用户回应的挂起不计入（防误杀最长 5 分钟的选择/填值等待）；看门狗中止且无任何输出时渲染「生成超时」说明气泡，不再静默结束
- **长工具执行心跳**：工具执行期间引擎按 `TOOL_HEARTBEAT_MS`（默认 25s）周期发布 `event.tool.alive`（payload 含 `name`/`toolCallId`，主循环与子会话循环统一收口在 `runToolInterruptible`；快工具在首个周期前结束不产生事件，前端不渲染仅刷新活跃时间）——阻塞类工具（sh/py 跑构建/长测试等）执行期间无其他事件，无心跳会被前端空闲看门狗误判挂起取消（工具自身 timeout 尚未到点），心跳让看门狗与工具超时各司其职：看门狗抓真正的流挂起/断连，工具超时管进程执行上限

**② 填值分支**：模型调用（`name` 变量名 + `description` 用途说明 + `secret` 敏感值掩码）→ 前端填值卡片，值注入本次任务 env 并保存浏览器本地（完整机制见「环境变量」章「ask 填值分支」节）；仅实时前端通道。

**③ 计划审批分支**：为复杂/多步骤任务制定执行计划——`buildPlanMarkdown`（title+steps 勾选清单或 content 全文）落盘会话工作目录 `plans/{标题清洗}.md`（物理位置 `{session}/tmp/plans/`；writeGuard 预检）→ `waitForChoice` 阻塞等待批准/拒绝（**`plan` 载荷随 `event.choice.request` 到达前端——选择卡内嵌计划全文，审批时直接可见**，不再只给标题 + 文件路径；**等待期消息流不重复渲染计划卡**（与选项询问分支同款延迟落卡，上下两张同款计划卡会被视为重复），决策结果到达时消息流才落计划卡并呈现审批结果态）；批准 → 模型严格按计划逐步执行（todo 跟踪）、拒绝（可附自定义修改意见作为 feedback）→ 修订后重新提交、拒绝审核/超时 → 停止计划相关操作；`data` 返回 `{status, title, path, feedback}`（`tool_schemas` 可查）。

**分支门控（合并型工具的通道能力校验，与 show 同模式）**——`ask` 不声明工具级 `interaction`（全模式可见），按 `ctx.interactionMode` 在分支内校验：选项询问/计划分支 `none` 下明确报错「当前通道无交互能力」（不空等 5 分钟超时）、`multi_turn`（飞书）经选择卡片作答；填值分支仅 `realtime`（飞书/REST 下明确报错并引导用户在设置面板配置）。

### 统一任务管理

**用户级**统一任务：定时（`scheduled`）/ 普通（`manual`）/ 闲时（`idle`）三类任务共用一份用户级存储（`users/{user}/tasks.json`）与一条调度队列，任何会话内经 `task` 子Agent 创建或经 REST 创建。任务属于**用户**而非会话——会话删除/过期后任务仍在，同一用户在任何会话可见可管（跨用户隔离不变）。能力由环境变量 `GEBAI_TASKS_ENABLED`（**默认 `true`**）统一开关：显式 `false` 关闭时 `task` 子Agent 不注册（`task_*` 工具在工具表/schema、`agent_list`/`agent_load`/`subsession_run` 中完全不可见）、调度器不启动、REST 管理面返回 503；开启后按需装载、REST `/api/v1/tasks` 可管（见「task 子Agent」）。

- **类别（`kind`）只决定何时入队**，执行体与其余参数完全通用：
  - `scheduled` **定时**：按 `schedule` 表达式到期**自动插入队首**（优先级最高；不抢占正在运行的任务，无空槽就在队首等待）
  - `manual` **普通**：创建即入队（`runNow`，缺省 true）按序执行，也可随时手动 `run` 再次入队；入队默认排普通任务队尾，`front=true` 置顶
  - `idle` **闲时**：仅当队列中无 scheduled/manual 条目、且没有运行中的任务、且该用户没有运行中的会话时启动，**同时只跑 1 个**（串行推进）；用户级待办开启 ⚡ 闲时自动执行时即绑定此类任务（见「用户级待办」）。闲时条目**一次入队即一次执行**：成功执行后自动停用（防空闲时无限重复跑；下一次由待办重开开关或手动执行恢复启用），失败/超时按 tick 周期节流后才重试（`nextRunAt = 结束时刻 + 30s`）；排队顺序由外部清单序提供（待办场景按其清单顺序，其他场景按创建时间）
- **执行体（`runner`）**：
  - `script` **脚本运行**：执行 shell 命令（在任务资源目录 `users/{user}/tasks/{task_id}/` 以用户环境运行——目录跨次运行保留产物；环境为进程环境 + 尽力解析的关联会话环境，不依赖会话存活），执行结果（成功/失败 + 输出）写入任务运行历史，并在**来源会话仍存在时**作为消息写回其消息流（`【智体·{类别}「名称」执行结果（成功/失败）】`，如 `【智体·定时任务「日报」执行结果（成功）】`；历史可见、模型可感知；来源会话已删除则静默跳过）。**写回消息为 `role: "user"` + `engineNote: "task"`**（前端渲染为「任务」通知条）——与引擎提醒同规则：思考类模型不接受以 assistant 结尾的请求（写回后它往往成为尾消息，会话下次带工具面的请求会被 400 拒绝，实测）
  - `prompt` **提示词运行 agent**：以指定提示词触发一次完整 Agent 会话（复用主循环），过程与结果在该执行会话的消息流呈现，末条 assistant 消息作为结果摘要进任务记录/通知；**交互模式按执行目标分流**——`ephemeral`/`sticky` 无人值守形态跑 `interactionMode: "none"`（见「交互模式」：本地模式需审批工具自动通过，不空等 5 分钟审批超时后跳过；服务模式直接拒绝并落因；`page_capture`/前端渲染/ask 询问不可用），触发消息附一行「无人值守执行」上下文告知模型；`target=session` 可能有人在场当场审批，保持 `realtime`
- **执行目标（`target`，prompt 型，解耦会话的核心设计）**：
  - `ephemeral`（缺省）：每次执行**新建独立会话**（会话名 `{类别}「名称」`，进入用户会话列表，上下文每次全新不累积、随正常数据生命周期清理）——例行检查/报告类首选；可选 `agents` 预载子Agent 名单（写入新会话 `loadedSubAgents`，装载保障按此注册工具与提示词）
  - `sticky`：**专用会话跨次复用**（首次触发惰性创建并记 `stickySessionId`，后续在同一会话续跑——上下文延续，适合需要记住上次状态的任务）
  - `session`：**绑定既有会话**执行（缺省为创建来源会话；该会话正有任务运行时条目留在队列等待，不打断用户会话）；绑定会话被删除后**自愈降级为 ephemeral**（任务保留，一次性记因），不再随会话消失而丢失
- **定时表达式**：5 段 cron（`分 时 日 月 周`；支持 `*`/`*/n`/`a-b`/`a,b,c`，日与周均受限时任一命中）或 `@every <n>s|m|h|d`、`@daily`/`@hourly`/`@weekly`/`@monthly`、`@at <时间>`（**一次性**：如 `@at 2026-09-01T09:00`，触发后自动停用；创建/修改时拒绝已过去的时间）；可选 `timezone`（IANA 名如 `Asia/Shanghai`，缺省服务器本地时区）——指定时区时按目标时区墙上时钟扫描（UTC 域日历推进 + 双次偏移换算，跨 DST 正确），服务器 UTC 部署也能按用户时区触发；非法表达式/时区创建/修改时即拒绝，永不触发的表达式（如 `0 0 30 2 *`）同样拒绝
- **可靠性参数**：
  - `misfire` 停机错过补跑策略：`skip`（缺省，错过即跳过、下次从当前时间重算）/ `run`（服务启动后发现触发点已过期则立即补跑**一次**——加载时保留过期 `nextRunAt`，首个 tick 执行后按当前时间重算）
  - `timeoutMs` 单次执行超时（缺省脚本 5 分钟（与 `sh`/`py` 同级）/ 提示词 30 分钟；合法区间 1s~24h）——提示词型到时走 `engine.windDown`：先让运行中的子会话**快速结束**拿结论（注入收敛指令 + 宽限，见「子会话快速结束」），再取消会话任务，并记 `status=timeout`（注意超时定时器不可 `unref`：await 挂起的 Promise 不保活事件循环）
  - `maxConsecutiveErrors` 连续失败自动停用阈值（缺省 0 不停用；连续 error/timeout 达阈值即 `enabled=false` 并在 `lastError` 记因——防错误任务无限重试刷屏/刷通知；成功清零，重新启用也清零）
- **执行记录**（`users/{user}/task-runs/{task_id}/{时间}.json`，按文件落盘、不内联在任务定义里）：每次运行（含定时到期未启动的 `skipped`）各写一个记录文件，内容为完整 `TaskRunRecord`——触发/结束时间/状态（success/error/skipped/timeout）/耗时/输出摘要/执行会话 id/手动标记/未启动即跳过的原因 `reason`；**文件名为记录时间**（UTC ISO，`:` → `-` 以适配 Windows 文件名；同毫秒多条追加 `_N`），字典序即时序——列表无需读内容排序、清理按名删最旧；单任务保留最近 `TASK_RUNS_KEEP`（200）条，超出按时间删除最旧；**旧数据自愈**：启动加载时把定义文件里遗留的 `runs` 数组一次性导入记录目录（幂等：目录已有记录则跳过），随后经 `forceWrite` 沉降把该字段从定义文件清掉。读取入口：`ctx.tasks.runs`（工具侧）、`GET /api/v1/tasks/:id/runs?limit=`（REST）与前端任务详情「运行历史」（异步拉取，运行次数变化即失效重取）
- **队列与额度**（每个用户一条队列，三类任务与待办手动执行同队列统一调度）：
  - **额度**：每用户 `GEBAI_TASK_MAX_CONCURRENT`（缺省 5）个同时运行的任务会话；额度只约束任务，用户对话会话不占额度；**运行中的任务不因额度不足被中断**（新条目排队等待）
  - **排序**：定时（权重 0）< 普通置顶（999）< 普通（1000）< 闲时（2000），同级按入队顺序 FIFO（同毫秒入队也保序：排序只比优先级与入队时刻，不用 id 兜底）；定时任务到期自动排到队首
  - **闲时自动进场仅由跑调度的实例发起**（调度器主实例锁门控）：从实例只服务请求、不主动领闲时活——从实例自己没有会话、`busyUser` 恒假，不加门控则每次手动执行/新建任务都会顺手把闲时任务拉起来跑
  - **目标会话忙**（`target=session` 指向的会话正在对话或跑任务）：条目留在队列等待，不启动、不占额度，下一轮再评估——不打断用户会话
  - **同一任务不并发**：上次未结束又到点（或排队中又到点）→ 记 `skipped` 运行记录（`lastError` 记因，不计入连续失败计数），不重复入队叠加
  - **一次性定时任务（`@at`）**：到期入队时清 `nextRunAt`（退出到期检查）、**执行结束**才停用——入队前停用会被入队路径以「任务已停用」拒绝，导致一次性任务永不执行；加载时已过期且无未来触发点的直接停用（防每次启动重算为过去值的热循环）
  - **重启恢复**：`queued` 条目重新入队（保持相对顺序）、`running` 条目标记为中断（不自动重跑，`misfire=run` 的定时任务除外）
  - **执行链健壮性（落盘/异常不阻断调度）**：任务是「出队即异步跑」——启动与收尾的落盘失败（目录被移除/磁盘满/权限）只降级为告警（`log.warn`），收尾链照常走完（状态回 idle、`event.task.result` 发布、队列继续推进）；执行链抛出的异常走统一兜底（清运行标记 + 记 `status=error` + 发结果事件 + 推进队列）——不会成为进程级未捕获 rejection，也不会让任务永久停在「运行中」（落盘仅影响重启后的状态恢复）
  - tick 周期 30 秒（到期检查 + 队列推进）；入队/置顶/运行结束均立即触发一次推进
- **通知通道（`notify`，任务内嵌数组可配多条）**——无人值守任务的送达手段（`core/schedule/notify.ts`）：
  - `webhook`：任意 http(s) 回调，POST JSON（载荷即 `task.result` 事件形态 + 通道 `at` 名单：任务信息/状态/输出摘要/耗时/执行会话/停用标记/@ 人名单，接收方系统可据此渲染提及）；URL 直配时经事件 Webhook 同规则 SSRF 校验（回环/链路本地/元数据地址默认拒绝，注册与投递逐跳共用），可选 `secret` 附 `X-Gebai-Signature: sha256=HMAC` 签名头（与事件 Webhook 投递同款，接收方一套校验通吃）；亦可 `webhook_id` **引用 REST `/api/v1/webhooks` 注册的事件 Webhook**（与 `target` 二选一，创建时校验存在性与归属——具名注册仅本人任务可引用、全局注册（admin，`userId` 未记录=部署方集成通道）人人可引用；投递时经注入的解析器（`WebhookManager.of`）解析其 URL 与签名密钥——注册侧改 URL/密钥即时生效，引用消失记 `lastNotifyError` 跳过不影响任务）
  - `feishu`：飞书通知通道，`target` **双形态**——群自定义机器人 webhook（`https://open.feishu.cn/open-apis/bot/v2/hook/…`，域名与路径强校验）或**群 chat_id**（`oc_` 前缀——以应用身份向指定群推送，走 `feishu_chat` 同款应用消息，需服务端配置飞书应用凭证，未配置时创建即拒；chat_id 可经 `feishu_group` 子Agent `chats_list` 查询）；webhook 形态可选 `secret` 加签（`sign = base64(HMAC-SHA256(key=timestamp+\n+secret, msg=""))`）；**消息形态随通道与 `at` 名单自动选择**——webhook（URL）形态默认发 **1.0 卡片**（`msg_type=interactive`：`config.wide_screen_mode` + 头部按状态着色 green/red/orange/grey + 任务名，正文 `div`/`lark_md` 组件（`**粗体**` 字段行：状态/周期/时间/耗时/错误/输出摘要/执行会话/停用标记）+ note 脚注；**自定义机器人 webhook 不支持 2.0 卡片**——schema V2 + note 组件实测被拒 `code=11246`，2.0 卡片仅应用消息接口支持），chat_id（应用消息）形态默认发 **2.0 卡片**（`schema:"2.0"`，与对话桥接同款新版本接口，正文 `markdown` 组件，整体上限 12000 字符）；`at` 含 `"all"` 时降级 **text 消息**（见 `at` 名单条目）；**投递校验飞书业务码**——webhook 业务失败时 HTTP 仍返回 200，解析响应 JSON `code !== 0` 即视为投递失败（记 `lastNotifyError`，防「任务 success 但群内无通知」静默失败）
  - `feishu_chat`：飞书**应用消息**（`target` 为群 chat_id），复用全局 `GEBAI_FEISHU_APP_ID/SECRET` 凭证（与机器人桥接/云文档共用）经 tenant api 发送，消息形态与 `at` 规则同 `feishu`（默认 2.0 markdown 卡片、含 `"all"` 降级 text）；未配置凭证时创建即拒绝
  - `at` **@ 人名单**（可选，全通道）：条目为 open_id（`ou_`/`un_`/`on_` 前缀）或 `"all"`（@所有人），字符串或 `{id,name}` 形态（name 为展示名，缺省由客户端解析真实姓名），存储归一为 `{id,name?}` 并去重（**@特定人**：open_id 不知道时可装载 `feishu_group` 子Agent 用 `members_list` 按姓名查询）；webhook 通道随 JSON 载荷 `at` 字段携带（供接收方解析提及）；飞书通道按名单自动选择消息形态——**仅 @ 具体 open_id 时 markdown 卡片**（webhook 形态为 1.0 卡片 `div`/`lark_md`、应用消息形态为 2.0 卡片 `markdown` 组件，`<at id=…>` 标签均置于正文首行——与 text 消息的 `<at user_id=…>` 属性语法不同，被 @ 用户收到提及通知），**含 `"all"` 时自动降级 text 消息**（@所有人 提及通知以 text 正文标签为可靠路径：1.0 卡片时代卡片内 @所有人 被静默忽略（实测），2.0 markdown 组件虽支持 `<at id=all>` 但提及权限因应用配置而异，通知场景求稳不冒险；降级后格式化为纯文本多行正文，具体 open_id 的 at 标签在 text 中同样生效，群机器人 webhook 与应用消息同规则）；飞书正文输出/错误的尖括号全角化净化（卡片与 text 两形态同规则），防任务输出注入 `<at>`/`<a>` 标签（@ 与链接仅由通道配置产生）
  - `notifyOn` 通知时机（**两种**）：`auto`（缺省，执行结束自动把结果摘要发出——prompt 型即会话最后一条回复、脚本型为输出）/ `model`（**不自动发**——通知完全由执行会话的模型经 `task_notify` 决定与撑写：例行正常保持静默、异常或需要用户知晓时主动推送；脚本型任务无模型参与，配 `model` 则不自动通知）；投递**尽力而为**——失败记 `lastNotifyError` 不影响执行结果与调度，成功清除；通知密钥在 `task_list`/REST 回显中脱敏（`***`），修改时传 `***` 保持原值；安全模式下通知投递（外发网络）跳过
  - **主动通知（`task_notify`）**：任何模式下模型都可主动推送自撑正文——`TaskManager.notify(user, id?, {text,title?,at?}, {sessionId})` 解析投递通道 = 任务 `notify` ?? 全局默认通道（webhookId 引用即时解析、secret 用真值、`at` 可由调用方覆盖通道配置），逐通道在**用户已配置的通道**上投递（不接受调用方传入任意 URL），尽力而为返回 `{taskId, delivered, errors}` 并记 `lastNotifyError`；`id` 缺省时按**执行会话**反查正在运行的任务（引擎适配层注入 sessionId，模型无需回显任务 ID）；安全模式拒绝、无可用通道报配置指引。**执行会话可用性**：prompt 型任务存在可用通道时，会话解析自动把 `task` 追加进执行会话的装载名单（不改任务自身 `agents` 配置），触发消息附任务 ID 与 `task_notify` 用法提示（并限定：执行任务期间不要用 task 的其它工具管理任务）——否则执行中的模型拿不到该工具
  - **全局默认通道**（环境变量 `GEBAI_TASK_NOTIFY_WEBHOOK` / `GEBAI_TASK_NOTIFY_FEISHU`）：任务未配置自己的 `notify` 时自动经全局通道推送（运营兜底——无人值守任务忘配通知不至失联）；**任务自配 `notify` 则只走任务自己的通道、不与全局叠加**（防重复推送）；全局通道在**投递时**解析（不写入任务数据，环境变量改动重启后即时生效），`notifyOn=model` 时不自动发（其余与 at/卡片形态规则同款）；启动构建期逐条校验（SSRF/域名/chat_id 形态、chat_id 形态需飞书应用凭证），非法配置告警忽略不阻断启动
- **工具**（`task` 子Agent 命名空间暴露 `task_*`）：
  - `task_add`：创建任务（`runner` 必填，`kind` 缺省按是否给 `schedule` 推断；可选 `name`/`script`/`prompt`/`schedule`/`timezone`/`misfire`/`target`/`session_id`/`agents`/`timeout_ms`/`notify`（webhook 支持直配 URL/`webhook_id` 引用注册通道，含全通道 `at` @ 人名单）/`notify_on`/`max_consecutive_errors`/`enabled`/`run_now`/`front`）
  - `task_list`：查看当前用户全部任务（含类别/执行体/运行态/周期/下次执行/次数/最近错误）+ 队列概览（并发额度、排队顺序与等待原因、运行中条目）
  - `task_update`：按 id 修改（全部可变字段）
  - `task_run`：手动执行一次（入队，可置顶；不改动既定调度节奏——`nextRunAt` 不变）
  - `task_cancel`：出队（排队中）或终止运行中的那次执行（`mode=dequeue|stop`）
  - `task_remove`：按 id 删除（不可删资源目录文件）
  - `task_files`：任务资源目录读写（`op=list|read|write|delete`，路径限定目录内）
  - `task_notify`：主动推送通知（`text` 必填，可选 `title`/`id`/`at`；**免审批**——无人值守执行等不到人工审批，投递目标限定为用户已配置的通道）
- **资源文件**：每任务一个资源目录 `users/{user}/tasks/{task_id}/`（脚本型任务的工作目录即它；prompt 型作为资料目录）——`task_files` 与 REST `/api/v1/tasks/:id/files*` 可列/读/写/删（单文件上限 4 MB，越界路径拒绝），任务删除后文件保留
- **REST 管理面**（与工具同源同权，前端任务视图与第三方集成用）：

| 端点 | 语义 |
|---|---|
| `GET /api/v1/tasks?kind=&state=` | 任务清单（可按类别/运行态过滤） |
| `POST /api/v1/tasks` | 创建（body 同 `task_add`；可选 `originSessionId`），201 |
| `GET /api/v1/tasks/:id` | 单任务 |
| `PATCH /api/v1/tasks/:id` | 修改 |
| `DELETE /api/v1/tasks/:id` | 删除 |
| `POST /api/v1/tasks/:id/run` | 手动执行（入队；body `{front?}`）→ `{task, queued, position?, reason?}` |
| `POST /api/v1/tasks/:id/front` | 排队中置顶 |
| `DELETE /api/v1/tasks/:id/queue` | 出队（取消排队中的执行） |
| `POST /api/v1/tasks/:id/stop` | 终止运行中的执行 |
| `GET /api/v1/tasks/queue` | 队列视图（额度/排队顺序/运行中；保留路径段，与 `:id` 通配不冲突） |
| `GET /api/v1/tasks/:id/runs?limit=` | 执行记录（新→旧；`limit` 非法返回 400。记录本身存于 `task-runs/{task_id}/{时间}.json`） |
| `GET /api/v1/tasks/:id/files` | 资源目录文件清单 |
| `GET /api/v1/tasks/:id/files/content?path=` | 读文件 → `{path, content}` |
| `PUT /api/v1/tasks/:id/files/content` | 写文件（body `{path, content}`） |
| `DELETE /api/v1/tasks/:id/files?path=` | 删除文件/目录 |

  写操作已有身份认证边界、不再叠加审批；按认证用户过滤（用户级归属校验），任务 id 走 32 位 hex 格式白名单，能力关闭时 503
- **用户级归属**：任务经 ToolContext/REST 绑定**当前用户**，`task_list`/`task_update`/`task_remove`/`task_run`/`task_cancel` 均校验用户归属，跨用户不可见、不可操作；`originSessionId` 记录创建来源会话（脚本结果写回目标）
- **执行规则**：
  - 调度器每 30 秒 tick 检查到期定时任务 + 推进队列；**入队即推进 `nextRunAt`**（执行排队不阻塞后续调度），入队与运行结束也立即触发一次推进
  - **启动加载校验 schedule/timezone 合法性**——tasks.json 被外部编辑改坏且任务 enabled 时直接禁用该任务并落 `lastError`（否则时间解析失败回退 +30s 会形成每 30 秒触发一次的热循环）；`queued` 条目重建进队列、`running` 条目标记中断
  - **入队失败（存储异常等）同样从当前时间重算下次执行时间**（记录 `lastError`/`lastStatus=error`），防 `nextRunAt` 停留在过去导致每个 tick 无重试热循环；一次性任务（@at）无法重算未来时间，失败直接停用防热循环
  - 手动执行（`task_run`/REST run/待办立即执行）不推进 `nextRunAt`（不打乱既定节奏），执行记录带 `manual` 标记
  - 任务记录保留上次执行状态/输出（输出限 4000 字符）；脚本输出写回会话消息限 8000 字符；`skipped` 不计入连续失败计数；**执行记录落盘失败只告警**（同持久化降级策略：结果与调度不得因记录写入失败而降级）
- **执行结果回写**：执行结束由调度器回调通知待办侧（`recordTaskResult`，仅在任务携带 `todoId` 时）——待办据此自动勾选完成、停用绑定任务或累计失败计次；回写以**落盘后的真值**判定（避免用陈旧计数把失败待办误判为正常）。
- **存储**：用户级 `users/{user}/tasks.json`（任务定义，随用户目录生命周期，不随会话分片清理；**不含执行记录**——记录在 `users/{user}/task-runs/{task_id}/` 下按文件落盘），服务端重启时扫描加载；落盘走 RMW + 跨进程写锁（单条 upsert 合并进磁盘真值，见下「写路径」）；**旧用户级 `cron.json` 启动时一次性迁移**（任务转 `kind=scheduled`、`runner` 取原 `type`，旧文件改名 `cron.json.migrated.bak` 保留；脚本工作目录 `cron-workspace/{id}` 并入 `tasks/{id}`），迁移仅在该用户尚无 `tasks.json` 时执行；旧会话级布局（`sessions/{s0}/{s1}/{id}/cron.json`）不再支持——启动遇之忽略（任务删除后资源目录与执行记录保留，不主动清理）
- **安全**：`task_add`/`task_update`/`task_run`/`task_cancel`/`task_remove`/`task_files` **默认需审批**（任务 = 无人值守的任意命令/会话执行，创建/修改/删除/立即执行/资源文件写入均须用户确认，服务模式下防普通用户绕过审批边界创建后门任务；`task_list` 与 `task_notify` 免审批（`task_list` 安全模式下仍提供；`task_notify` 无人值守执行等不到人工审批，投递目标限定为用户已配置的通道——任务创建时已经过审批、不新增任意外发面，安全模式下不注册）；REST 管理面已有身份认证边界、写操作不再叠加审批）；脚本以任务所属用户身份、任务资源目录与用户环境运行（与 `sh` 工具同隔离级别，沙箱模式下脚本环境同样剔除敏感变量，见「脚本执行环境」）；安全模式下脚本执行与通知投递均跳过（记 skipped），任务调度类工具（`task_add`/`task_update`/`task_remove`/`task_run`/`task_cancel`）硬阻断；通知 URL 经 SSRF 校验防内网探测；能力整体由 `GEBAI_TASKS_ENABLED` 开关管控（默认开启，显式 false 完全不可见）
- **事件**：入队推 `event.task.queued`（含来源与队列位置）、启动推 `event.task.start`、结束推 `event.task.result`（成功/失败/跳过/超时与输出摘要、prompt 型含执行会话 id、自动停用标记）、队列变化推 `event.task.queue`（额度/排队/运行中计数）——前端任务视图与队列面板据此实时刷新（prompt 型详细过程在该会话消息流）
- **注入链路**：构造顺序为 `AgentEngine` 先建、`TaskManager` 后建（两者互相需要——调度器要 engine 执行 prompt 型任务、engine 要调度器绑定 `task_*` 工具，避免循环构造依赖）——`tasks.attach(engine)` 为**双向绑定**：调度器持有 engine，同时引擎侧 `opts.tasks` 经 `setTasks()` 回填（`task_*` 工具的 ToolContext 绑定源；单向注入不回填会使能力开启下工具仍恒报「能力未启用」）；通知依赖（fetch/飞书应用消息发送器）、子Agent 名校验器（`agentExists`）、每用户额度（`maxConcurrent`）与任务结束回调（待办联动）随构造注入
- **多实例**：定时任务到期判定与队列推进全在**进程内**（内存队列 + 任务状态持久化），同一 `GEBAI_HOME` 下多实例并存会重复调度——由「调度器主实例锁」收敛（见下）：只有主实例跑 tick 与队列，其余实例只加载任务（REST/工具读写正常）不跑调度；闲时任务的**自动进场**另受 `schedulerActive` 门控（从实例不领闲时活，用户显式手动执行仍会在接到请求的实例上执行）

#### 调度器主实例锁（同一 GEBAI_HOME 单实例调度）

闲时任务与定时任务的调度判定都是**进程内**状态（`TaskManager` 的内存队列与 `engine.busyUser()` 会话空闲判定），`tasks.json`/`todos.json` 也只在各自 `start()` 时读入内存镜像、各写各的——多实例并存时每个实例都会跑一份调度：闲时任务被重复领走（**从实例/残留实例自己没有会话 → 永远认为服务端空闲**）、定时任务重复触发、通知重复投递。故同一 `GEBAI_HOME` 下必须只允许一个实例跑调度。

- **主实例锁**（`core/schedule/primary.ts`）：锁文件 `{GEBAI_HOME}/.gebai-primary.json`（内容 `{ pid, port, at }`，`at` = 最近一次续租时刻；写入一律临时文件 + rename 原子落盘）——文件不存在/损坏/持有者 PID 已死/租约过期 → 抢占成为主实例；持有者 PID 是自己 → 续租；他人持有且 PID 存活、租约未过期 → 从实例。租约（`PRIMARY_LEASE_MS` = 5 分钟）把「PID 复用」这一不可判定项收敛为可判定：即使 PID 被无关进程复用，停止续租后租约到期即被接管
- **看门狗**（周期 `PRIMARY_WATCHDOG_INTERVAL_MS` = 30 秒，远小于租约）：主实例每周期续租；从实例在主实例死亡/租约过期后**自动接管**并启动调度（日志 `[scheduler] 已接管主实例调度`）；主实例自身错过续租被他人接管时停调度退让（日志 `[scheduler] 主实例锁已被 PID x 接管`），避免双跑
- **模式**（`GEBAI_SCHEDULER`）：`auto`（默认）=按锁判定（仅此模式启用看门狗）；`on`=强制本实例跑调度（忽略锁、不落锁——多实例同时 `on` 会重复调度，仅用于单实例部署/调试）；`off`=完全不跑调度（只提供 HTTP/WS）
- **退化为从实例时的行为**：数据照常加载（REST `/api/v1/tasks`、`/api/v1/todos` 与工具读写都可用），只是不跑 tick 与队列推进；主实例退出/被接管后由看门狗补上
- **写路径**（`core/support/json-store.ts`，`tasks.json`/`todos.json` 共用）：上述锁只收敛「谁跑调度」，**不收敛「谁写文件」**——落盘因此独立做成 **RMW（读磁盘真值 → 合并本次变更 → 原子写）**：
  - 合并基准是**磁盘真值**而非本进程镜像（待办/任务均按单条 upsert 或变更函数合并进磁盘清单）——镜像陈旧或为空不再具有破坏性（旧实现对镜像整体覆盖：多个实例各写各的，后写者抹掉前者条目；且空镜像一次写回就能把磁盘既有条目连同执行记录清空）；
  - **跨进程写锁**：`<file>.lock`（`O_EXCL` 独占创建 + 内容 `{pid,at}`；持有者 PID 已死/租约（15s）过期→抢占；等待上限 5s 超时**抛错**而不降级为无锁写）；创建与写内容之间的微小窗口用 2s 宽限期避开误抢占；
  - **无实质变更不写**（避免无谓覆盖/备份/mtime 抖动）；`forceWrite` 选项用于**结构沉降**——归一化会剥离旧版遗留字段（如任务定义里内联的 `runs`），但变更函数看到的已是剥离后的真值，恒等变更会被本条跳过、旧字段就永远留在磁盘上，故迁移路径显式要求写回一次；
  - **写前复核**：写前重取 `mtime`/`size`，期间被非协作写者（旧版本进程/手工编辑）改动则重读重试（至多 3 次，仍冲突则抛错）；
  - **原子写 + 滚动备份**：临时文件 + rename（读方永不看到半截 JSON），覆盖前把磁盘现值留存 `<file>.bak`（原内容为空/`[]` 时不覆盖可用备份）；**临时名带进程内序号**（`{file}.{pid}.{seq}.tmp`：同进程并发写者共用一个 tmp 名会互相抢文件——先 rename 的一把抽走、另一个报 ENOENT 半程失败），rename 对 Windows 短暂持有（EPERM/EBUSY/EACCES）退避重试至多 3 次；
  - 锁是**建议性**的：只有经由本模块写入的进程之间才互斥，外部编辑由写前复核兜底；同一进程内对同一文件**不可重入**（变更函数内不得再写同一文件）
- **已知边界**：锁是**建议性**（协作式）的——绕过歌白直接操作锁文件、或两个实例同时抢占一个陈旧锁，都存在短暂双主窗口（抢占后回读校验 + 看门狗每周期收敛，极端情况下最长约一个看门狗周期）；主实例长时间阻塞事件循环（超过租约）也会被接管

### 用户级待办

**用户级**待办清单（标题栏轮盘「待办」按钮打开的可拖动弹窗）：与引擎**会话级**待办（`todo` 工具，agent 自己维护的任务清单，随会话走）语义不同——用户待办属于**用户**（`users/{user}/todos.json`），跨会话/重启保留，供用户自己记事项，可**一键填入输入框**或**入队执行一次**。待办清单与任务清单是**两份独立资源**（待办不等同于任务）：任何待办都随时可手动执行（入队按序跑一次，不要求开启任何开关），只有开启 ⚡ **闲时自动执行**的条目才**绑定一个闲时任务**、由任务调度器在队列空闲且没有运行中的会话时按清单顺序串行执行（见「统一任务管理」→ 闲时类别）。能力由 `GEBAI_IDLE_TODO_ENABLED`（**默认 `true`**）开关：显式 `false` 时 REST 返回 503。

- **存储与归属**：用户级 `users/{user}/todos.json`（**数组顺序即清单顺序**），启动 `walkDir` 扫描加载 + Map 驻留；**落盘走 RMW**（读磁盘真值 → 在真值上应用本次变更函数 → 跨进程写锁内原子写 + 滚动备份，见「统一任务管理」→「多实例」下的写路径；落盘后用真值刷新本地镜像，本进程仅贡献变更）；条目字段 `id`（32 位 hex）/`user`（归属用户）/`text`/`done`/`idle`/`createdAt`/`updatedAt` + `idleTaskId`（绑定的闲时任务 id）+ 执行记录（`idleState` pending|running|done|failed / `idleAttempts` / `idleError` / `idleRunAt` / `idleSessionId` / `idleResult`）；单用户上限 500 条、单条文本上限 2000 字符（也是执行时的提示词）；开启闲时标记时重置失败计数重新排队，取消勾选完成视作重新排队；启动加载时执行中的状态复位为 pending，并为开启 ⚡ 的未完成待办补齐绑定任务
- **REST 管理面**（前端弹窗与第三方集成共用，写操作不经审批——REST 已有身份认证边界，与任务域同姿态）：`GET /api/v1/todos`（清单）、`POST /api/v1/todos`（新增 `{text, idle?}`，201）、`PATCH /api/v1/todos`（**清单级批量重排** `{ids: [...]}`，拖动排序落库；未列出的条目按原序追加在后防丢失）、`PATCH /api/v1/todos/:id`（`{text?, done?, idle?}`）、`DELETE /api/v1/todos/:id`、`POST /api/v1/todos/:id/run`（**立即执行：入队跑一次**——body `{front?}`，返回 `{todo, queued, position?, reason?, taskId, ephemeral}`；任务能力未启用 503）；条目 id 走 32 位 hex 格式白名单（`:id` 与其子路径 `/run` 同规则），按认证用户过滤（跨用户不可见不可操作），能力关闭时 503
- **执行链路（统一任务队列）**：手动执行时——待办已有关联任务则把该任务入队（必要时恢复启用并同步文本），否则**建一条一次性普通任务**（`kind=manual`、`runner=prompt`、`prompt=待办文本`、`ephemeral=true`、绑定 `todoId`）入队，执行完自动删除（任务清单不被一次性执行塞满）；闲时自动执行 = 绑定闲时任务的调度行为。两种路径的执行结果都由任务调度器回调 `recordTaskResult` 回写：**成功自动勾选完成**（`done=true`、`idleState=done`、记 `idleResult`/`idleSessionId`，并停用绑定任务）；失败累计 `idleAttempts`，达上限（3 次）置 `idleState=failed` 并停用绑定任务（防死循环重试）。执行会话标题为 `{类别}「任务名」`（如 `普通任务「待办：xxx」`），完整过程与产物在该会话回看
- **与闲时任务的绑定联动**：开启 ⚡ → 创建（或校正）绑定闲时任务（`kind=idle`、`todoId=待办 id`、`prompt=待办文本`、`target=ephemeral`）；待办文本变更 → 同步任务提示词；关闭 ⚡ → 删除绑定任务；待办勾选完成 → 停用绑定任务（重新手动执行时自动恢复启用）；待办删除 → 连带删除绑定任务
- **弹窗交互（`packages/web/src/todo-pop.ts`）**：
- **弹窗交互（`packages/web/src/todo-pop.ts`）**：
  - **尺寸**：`min(680px, 94vw) × min(78vh, 860px)`（宽/高各有下限保底）——待办正文即模型提示词（可多行长文），窗口要同时容得下“读全文”与“写长文”；长文本在列表内**折叠展示**（超阈值高度截断 + 「展开全文」/「收起」），避免单条长提示词把列表打爆
  - **显隐**：**只有点击弹窗右上角 ✕ 才隐藏**——点轮盘按钮只负责打开/前置（不切换关闭）、点弹窗外不关、Esc 不关（编辑态 Esc 仅取消编辑），避免写长提示词时误触丢失；**打开状态与位置一并持久化**（`gebai.ui.todo.open` / `gebai.ui.todo.pos`），页面刷新（含 dev-reload 自动刷新）后恢复原状
  - **拖动与位置**：**标题栏拖动移动窗口**（pointer capture + 视口钳制——顶部与左右保留最小可见像素），localStorage 记忆位置，脏数据回退默认位
  - **新增/编辑**：都用**多行文本域**，行内编辑区最矮 132px（可拖拽调高）、Ctrl/Cmd+Enter 保存 · Esc 取消 · 「保存/取消」按钮可点，**点击别处不自动保存**（防误触丢失/误存）；删除走确认框
  - **新增区（底部）**：输入框**高度随内容自适应**（`autosizeAdd`：按实际内容高度实时长高，上限视口 30%，到上限后转内部滚动）——多行长提示词不被埋在固定高度里；右侧只有一个**绘制的加号图标按钮**（圆形，非文字）——空内容时置灰禁用、有内容即可点且 hover/active 给激活反馈，点击即提交；**Shift+点击加号 = 直接开启闲时自动执行**，Ctrl/Cmd+Enter 同提交（Ctrl/Cmd+Shift+Enter 同）
  - **列表项操作**：勾选完成、⚡ 闲时自动执行开关（行内显示执行中/完成摘要/失败原因）、**「▶ 执行」加入任务队列按序跑一次**（提示队列位置，进度与结果可在「任务」视图与执行会话查看）、**点击条目文本或「填入」按钮把内容写进对话输入框**（`input.value` + `autosize()` + `syncSendButton()` + `focusInput()`，沿用快捷胶囊的既有做法，不自动发送）、**拖动排序**（HTML5 drag，落点上下半区高亮，drop 后调 `PATCH /api/v1/todos` 落库，失败回滚为服务端真值）；打开期间每 15 秒静默同步状态（不阻塞执行），数据无变化不重绘（不打断滚动与编辑）
  - 纯逻辑（拖动重排/落点换算/位置钳制）拆在 `todo-core.ts` 单测（同 `press-gesture.ts` 的拆法，无 jsdom 依赖）

### 工具执行与渲染
- 工具调用时在 UI 中打印工具名和参数，参数以 JSON 格式展示（脚本类工具渲染为语法高亮代码块）
- `todo` 与 `ask` 渲染为**特别卡片**（待办清单卡 / 问答记录卡 / 计划卡；ask 按参数形态分流——options → 问答记录卡、title → 计划卡、其余（填值）走通用工具卡），不显示通用工具卡片
- 脚本执行类工具（`sh`、`py`）的输出以 Markdown 代码块独立渲染
- **执行超时**：`sh`/`py` 子进程均设超时上限（默认 5 分钟，可经 `timeout` 参数按次调整，上限 540 秒不晚于引擎 9 分钟兜底），超时强制终止
- **用户中断的结果形态**：被中断的工具调用结果统一带 `[interrupted by user]` 标记（脚本类工具另带自身中断标记与中断前已产生的输出；未执行/未完成的调用写明「本次调用未执行」），执行中被停的调用另报**已执行时间**（「（已执行 12.3 秒）」，按取消信号到达时刻计时；未及执行的调用不报）——模型下一轮据此区分「用户停止」与执行超时、引擎错误（见「核心Agent流程 · 中断与取消」）
- **编码**：`py` 强制 UTF-8 模式（`python -X utf8` + 环境变量 `PYTHONUTF8=1`），避免 Windows 下默认 GBK 输出造成乱码/解析问题
- **解释器自适应**：`py` 执行前按 `python3` → `python` → `py` 顺序探测可用命令（`--version` 退出码判定，结果缓存），适配 Linux（多为 `python3`）与 Windows（多为 `python`/`py`），无需配置

#### 脚本执行环境

脚本工具的可用性取决于宿主机环境，分三档：

| 脚本 | 运行方式 | 宿主机要求 |
|------|---------|-----------|
| `sh` | 子进程执行 shell（POSIX 经 `bash -c`——PATH 中的 `bash`，极简环境缺 bash 时回落 `/bin/sh`；**Windows 经 PowerShell**——优先 PATH 中的 `pwsh.exe`（PowerShell 7+，支持 `&&`/`||`），缺失回落系统内置 `powershell.exe`（Windows PowerShell 5.1），`GEBAI_SH_SHELL` 可指定解释器（POSIX 同用此变量指定 bash 或其他 POSIX shell）、值 `cmd` 回落 cmd.exe 分支；PowerShell 形态以 `-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command` 承载命令，命令行前置段切 UTF-8 输出编码（`[Console]::OutputEncoding`/`$OutputEncoding`）并关进度输出，后置段 `exit $LASTEXITCODE` 带出原生命令退出码（`-Command` 默认把退出码归一成 0/1）；cmd 回落分支保持 `chcp 65001 >nul && …` 切代码页；输出自适应解码：UTF-8 优先、含替换字符回退 GBK、ANSI 控制序列剥离；Windows 子进程不设 detached——实测会导致外部程序管道输出丢失；命令成功但无输出时明确提示「无输出」，区分捕获失败；**`timeout` 参数（秒，默认 300、上限 540）调整个别执行超时**） | 有 shell（各平台自带） |
| `py` | 子进程执行 python（**`timeout` 参数同 `sh`**）；**本地模式带工具桥**（工具名即函数 / `tools.call` / `ctx`·`input` 注入，协议走回环 socket，见「py 工具桥（仅本地模式）」） | 安装 Python |
| `js` | 子进程执行 JS/TS（Bun 运行时：脚本调试模式 `bun <script>` 直跑；二进制模式 `[execPath, 入口, "exec", script]` 复用**编译进二进制/打包产物的 Bun 运行时**自执行，见「js 脚本工具」；**`timeout` 参数同 `sh`**） | **无需安装**（运行时已内嵌） |
| JS/TS（经 `sh`） | 宿主机已有 `node`/`bun` 时 `sh` 直接调用亦可（`bun run x.ts` / `node x.js`），与 `js` 工具两条路径并存 | 安装 bun/node |

- 二进制编译时已内嵌 Bun 运行时，`gebai exec` 隐藏子命令（`import.meta.main` 入口拦截 `process.argv`，**exec 段双位置路由**：argv[2] 或 argv[3]）让二进制无需宿主机安装 bun/node 即可执行 JS/TS 脚本，同时保持子进程隔离（不回到进程内执行）。子进程命令二进制模式统一**带入口段** `[execPath, 入口, "exec", script]`：编译单文件形态（`gebai.exe`）子进程 argv[1] 自动为内嵌虚拟入口，入口段仅占位（exec 落在 argv[3]）；**容器形态**（execPath=bun 跑 `dist/` 打包产物，包内各模块 `import.meta.path` 一致指向打包产物）入口段是真实 dist 入口（exec 落在 argv[2]）——缺失入口段时 bun 会把 `exec` 当子命令、script 当命令名直接报错（js 工具全挂）
- 解释器自适应：`py` 调用时惰性探测可用命令（`python3` → `python` → `py`，结果缓存），schema 描述**不做启动期可用性标注**；环境/工具链探测能力下沉 code 子Agent 的 `env_detect`（平台/架构、工具链版本、MSVC/WebView2 状态）
- **工作目录标注**：`sh`/`py` 在**非会话默认目录**执行（`workdir` 参数、`project` 参数路由或项目绑定会话）时输出末尾标注「（工作目录: …）」——按 cwd 发现目标的工具（`bun test` 等）目录不对时一眼可辨（「命令在哪个目录跑的」不再靠猜）；`sh` 的退出码直接读返回结果的 `exitCode` 字段（无需 `echo $?` / `%errorlevel%` / `$LASTEXITCODE`——Windows 分支已由命令行包装段带出）
- **输出大小**：工具输出超过截断阈值走上下文保护（截断落盘），防止内存膨胀

#### sh 异步后台任务（`async:true`，管理经 `bg_task`）

长耗时命令（全量构建/测试/依赖安装）同步等待会占死一轮工具调用，`sh` 传 `async: true` 转**后台执行**——立即返回 `taskId`（结构化 `data.taskId`/`pid`），模型先处理其他任务，之后经 `bg_task` 回头处理（与 subsession_run 异步运行统一管理面，按 id 前缀分发）：

- **启动**（`sh async:true`）：经 `Sandbox.spawnBackground` 起进程——与同步 `exec` 同规则的 shell（Windows PowerShell；`cmd` 回落分支 `chcp 65001`）/环境脱敏（沙箱用户剔除敏感变量）/Unix 进程组语义，但 stdout+stderr **合并持续写入日志文件**（WriteStream 落盘，不占内存）且不等待完成；`timeout` 参数在此语义为**任务生命周期上限**（默认 1800 秒、上限 3600 秒，防僵尸进程常驻）
- **状态落盘**（会话 `tmp/sh-tasks/`，跨工具调用与服务重启可见）：`tasks.json` 记录（命令/cwd/pid/起止时间/退出码，原子写 tmp+rename）+ 每任务 `{id}.log` 合并输出日志；引擎按 `user:sessionId` 复用服务实例（`ToolContext.shTasks`），会话删除时释放（`forgetSession`）
- **生命周期**：记录**先落盘、后注册** `exited` 回调——命令可能瞬时退出（`echo` 类），回调先于记录落盘会读不到记录而丢弃退出码，任务永久停在 running；回调落盘回写退出码（同进程内准确）；服务重启后 pid 失活而记录无终态 → 判定 `lost`（已结束、退出码未知，日志尾部仍可读）；**本进程持有该 pid 的句柄时不做 pid 探测**（退出由回调回写，中间态探测与 close 竞态会误判 lost），句柄缺失或 pid 不符才走 pid 兜底；生命周期超限在 status/wait/list/kill 时**惰性检查**——终止进程树（本进程内经句柄精确 kill，重启后按 pid 兜底：Windows `taskkill /T`/Unix 进程组）并标记 `timed_out`
- **查询/等待/终止**（`bg_task`，id 前缀 `t` 分发到本服务）：`status` 立即返回当前状态（running/done/failed/killed/timed_out/lost）与输出尾部；`wait` 阻塞至完成或等待超时（默认/上限 60 秒——上限压到 1 分钟强制按进度轮询，超时返回当前状态可再次 wait）；`stop` 终止进程树并标记；`list` 与子Agent 运行、子会话运行合并列出本会话全部后台任务
- **上限**：单会话并发运行任务 ≤ 8（`SH_TASK_MAX_CONCURRENT`，超限拒绝新任务并引导清理）；输出尾部默认 4000、上限 20000 字符（`tail` 参数）
- **审批与安全模式**：与同步 `sh` 完全同规则——命令本身照常过动态审批（`approval:false` 免审白名单强制）与安全模式只读白名单（`validateShCommandSafeMode`，降级语义不分同步/异步）；`bg_task` 管理动作（查询/等待/终止本会话后台任务）免审批

#### 工具双输出（output / data）与输出 Schema

工具结果（`ToolResult`）携带两条相互独立的输出通道（面向「工具即函数、模型动态编程」的编排能力）：

| 通道 | 字段 | 消费方 | 说明 |
|------|------|--------|------|
| 文本输出 | `output` | 模型（进 LLM 上下文） | 面向模型分析的文本，截断保护照常生效 |
| 结构化输出 | `data` | **js 编排**（不进 LLM 上下文） | 供 js 脚本内工具函数返回值引用（`r.data.字段`）、分支判定；运行期在脚本内传递，不落盘、不占上下文词元 |

- **`Tool.outputSchema`**：声明 `data` 的 JSON Schema，经 `tool_schemas` 工具批量暴露给模型——js 编排前先查输出结构，避免逐个试调浪费往返
- **引擎兜底截断保留 `data`**（含 `subSessionArchive` 扩展字段）：截断只作用于模型可见文本
- 已提供结构化输出的全局工具：`ls`（entries）、`glob`（files/total）、`file`（info：path/type/size/isDir/modifiedAt/entries/encoding/text/extMismatch）、`grep`（**三键齐备**：matches/files/counts——不论 output 选哪种模式三键同时给出，主键为本次形态，避免调用方按 `data.matches` 读取时在 files/count 模式静默得到空数组）、`sh`/`py`（stdout/stderr/exitCode，stdout/stderr 在 data 中截断至 100k 字符）、`js`（logs/result/exitCode/calls，logs/result 截断至 100k 字符）、`fetch_url`（ok/status/contentType/error）、`todo`（todos）、`show`、`patch`、`agent_load`/`subsession_run`/`subsession_run`/`bg_task`（后者均带 `Tool.outputSchema`）；**`agent_list` 未注册进全局工具表**（仅在已装载子Agent 的上下文可见，故不计入本清单）；子Agent 工具可按同一模式声明（`ToolResult.data` + `Tool.outputSchema`，如 `code_git`：status/log、`playwright_*`：读类工具给结构化结果——`content` 的 `{mode, selector?, text?, html?}`、`pages` 的 `{pages}`、`evaluate` 的 `{value}`（页面返回值的 JSON 解析结果，driver 已截断或非 JSON 时省略））
- **`tool_schemas` 工具**（批量查询）：`tools` 传工具名列表返回各工具 `{name, description, parameters, outputSchema}`（未知/未启用标记错误）；省略时返回全部已启用工具的输出结构概要（紧凑一行一个，不含输入参数）

#### 富内容块渲染

工具返回不再限于纯文本，可通过 `ToolResult.blocks` 携带**结构化的内容块**（`ContentBlock`），前端按类型逐块渲染，并**随消息持久化**——历史会话同样可查看：

| 块类型 | 渲染 |
|--------|------|
| `text` | 纯文本段落 |
| `code` | **文件内容卡**（`markdown` 语言渲染 md、其余语法高亮；`path` 附带时工具栏提供复制/原文件查看/下载，见「文件内容卡」） |
| `image` | 内嵌图片（经 `files/content` 加载会话 `tmp/` 内文件；**点击进入全屏查看器**——缩放/平移/复制/下载，与图表查看器同骨架，`show` 图表分支 `render=backend` 产出的 PNG 同样可全屏查看） |
| `file` | **文件内容卡**（统一文件展示形态，按 mime/扩展分派——图片内联点击全屏、音视频内联播放（原生控件，取数支持 Range 可拖进度）、PDF 卡内 iframe、`text/html` 卡内沙箱 iframe、md 渲染、文本/代码语法高亮、二进制占位提示下载；内容**进入视口才经 `files/preview` 按需加载**（会话相对与项目绝对路径统一入口，卡内渲染上限 4 万字符，超出截断引导原文件查看）；工具栏复制/原文件查看（弹窗）/下载。`read`/`write` 等常规工具产物为此形态（默认「弹窗查看」模式下收敛为文件链接 chip，见「文件展示方式」），`show` 的 path 分支对无法内联的类型使用，见「文件内容卡」与「内容展示（`show`）」） |
| `diagram` | **交互式图表**（Mermaid/PlantUML/D2/ECharts 四语言本地渲染，缩略图卡片 + 全屏查看器，源码查看/下载、高清 PNG 下载） |
| `diff` | **并排文本对比**（旧/新两栏行对齐，增删行红/绿着色，按文本类型语法高亮；**长行自动换行不横向溢出**——与工具卡代码块同款断行策略，降级视图同样）——`diff` 工具已移除，此块仅用于历史会话回放 |
| `html` | **HTML 页面沙箱渲染**（iframe 直接预览，全屏查看器 + 源码查看/复制/下载；域隔离 sandbox 内脚本可执行、无法访问宿主页面，见「内容展示（`show`）」） |

- 工具按产出自动生成块：`read`/`write`/`edit`/`patch`（修改类以**修改后内容**产出，与读写同款；dry_run 预演不落盘无产物块）依据扩展名识别图片（PNG/JPG/GIF/WebP/SVG/BMP）与图表（`.puml`/`.plantuml`→plantuml、`.mmd`/`.mermaid`→mermaid、`.d2`→d2、`.echarts`→echarts，`diagram` 块 `format` 字段携带图表语言）；`show` 工具按内容源分支产出 `diagram`/`html`/`image`/`code`/`file` 块（见「内容展示」）；`diff` 块（并排对比）仅存在于历史会话（`diff` 工具已移除）
- **markdown 内图表围栏兜底渲染**（`web/markdown.ts`）：聊天页 markdown（助手正文、推理块、计划卡、文件内容卡的 md 预览、子会话报告）中的 ` ```mermaid `/` ```mmd `/` ```plantuml `/` ```puml `/` ```d2 `/` ```echarts ` 代码块**不经 `show` 工具也渲染为图表**——渲染后把该代码块替换为图表卡片（`diagram-embed` 包裹，与 `diagram` 内容块共用同一套本地引擎/卡片/全屏查看器，源码经查看器查看）；**源码空白的块与文本末尾未闭合的围栏（流式半成品）跳过**（闭合后的下一次渲染出图，避免增长中的源码反复渲染）；非图表语言（含 `json`）按普通代码块保留
- **文件内容卡**（`code` 带文件信息与 `file` 块的统一渲染，`web/file-card.ts`）：头部（文件名 + 语言/类型徽标 + hover 渐显工具栏：**复制/原文件查看** + **常驻下载图标**（不随 hover 显隐；描边图标，文件卡头部/文件链接 chip/弹窗共用 `downloadAnchor`，经 `files/preview` 附件形式，桌面 WebView 提示下载位置））+ 内容区按类型渲染——`markdown` 语言**渲染 md**（复制复制源码、弹窗看原文）、其余**语法高亮**（块内 `language` 优先；**缺省时按 `name`、再按 `path` 扩展名回退推断**——历史卡片与展示名不含扩展名的块同样按真实类型渲染而非落 `highlightAuto`；hljs 语言集与前后端 `EXT_LANG` 三表同步，扩充 go/rust/java/kotlin/ruby/c/cpp/csharp/php/sql/lua/swift/dart/scss/less/**yaml**（旧版 `yml`/`yaml` 误映射为 `markdown`，YAML 被当 markdown 文档渲染））、图片内联点击全屏、PDF 卡内 iframe、`text/html` 卡内**沙箱 iframe**（复用 html 块的域隔离管线）、二进制类型占位提示；`file` 块内容**进入视口才 fetch**（`files/preview` 按需加载——会话相对与项目绝对路径统一入口，read/write 产物块路径为解析后真实路径，**code 项目文件的文件卡/下载同样可用**（旧版按原始参数路径经 `files/content` 解析在项目文件下 404）；历史长会话不产生全量请求），卡内文本渲染上限 4 万字符（超出截断引导「原文件」查看）；「原文件查看」弹窗（**尺寸按视口比例**：标准档 `width: min(90vw, 100%)` + `max-height: 88vh`——消息列 `--msg-max-w` = 800px 下旧值 920px 几乎与正文同宽、代码长行/网页预览横向滚动明显；窄屏（≤ 860px）遮罩 padding 收到 12px；标题栏**全宽切换**按钮（`.preview-full`，图标按钮）在「标准比例 / 铺满视口」（`.preview-card.is-full` = width 100% + max-height 94vh）间切换，偏好记忆于 localStorage `gebai.ui.previewFull`；标题栏含**常驻下载按钮**（同 `downloadAnchor`，与关闭同款图标交互）+ 图片直显（宽卡内居中，`.preview-body > img` 不误伤查看器缩放图）/PDF 内嵌/md 渲染/沙箱 html/文本高亮，与文件链接 chip 共用弹窗骨架）承载全文；超长文本截断由 show 服务端产出**单个带 `path` 的 `code` 块**（不再附独立 file 卡片，全文经工具栏获取）
- **文件展示方式（「嵌入展示/弹窗查看」，设置面板「外观」，`web/file-display.ts`）**：`read`/`write` 等文件工具（含 code 子Agent 同款包装，`card.file` 声明）的**产物文件卡**（工具卡下方的 file 内容块）在两种形态间切换——**弹窗查看**（默认，现状：产物 file 块收敛为**文件链接 chip**：📄 图标 + 文件名 + 路径（弱化单行省略）+ 下载图标，点击弹窗查看文件全文；chip 即块位置渲染于 `.msg-body`，路径取块自带的解析后真实路径，经 `files/preview` 取数）或**嵌入展示**（产物 file 块渲染为文件内容卡，进入视口按需加载内联展示）；**参数区与工具输出不受影响**（write 的 content、edit 的旧/新对比、read 的输出照常渲染——展示方式只作用于下方产物文件卡），图片/图表等视觉产物块照常内联（仅 file 块切换形态），show 等非文件卡工具的块不受影响；实时（appendToolResult）与历史（appendMsg）同构分流；localStorage `gebai.ui.fileDisplay` 仅存 `"inline"`，跨标签 storage 事件同步，切换后重载当前会话消息即时生效（渲染是结构性的）。**脚本桥（js/py）编排的产物同样分流**：桥汇集内层块时给 file 块写入来源工具名（`via`），前端按来源工具查同一份 `card.file` 声明（`fileBlockAsLink`）——桥内 read/write 与直接调用一致收敛为 chip，`show` 等主动展示工具的产物照常内联；旧记录（无 `via`）按内联渲染，不误收敛
- 图表渲染走**通道化渲染链路**：`show` 图表分支执行中引擎发布 `event.draw.render`（含 renderId + 源码 + **图表语言 `format`**）→ **Web 通道**前端**本地渲染**（按 format 分派本地引擎、**零网络请求**：mermaid → 官方 `dist/mermaid.min.js` 自包含 UMD、plantuml → 官方 `@plantuml/core` 引擎（TeaVM 编译，含内置 Graphviz 布局）、d2 → 官方 `@terrastruct/d2` WASM 引擎（浏览器构建，Web Worker 内编译渲染）、echarts → 官方 `dist/echarts.min.js` 自包含 UMD（**SSR 模式直接输出 SVG 字符串**——JSON option 纯计算渲染为静态 SVG，无 DOM 挂载、无动画，与其余三语言同一「源码 → SVG」契约）；**引擎经 `public/vendor/` 稳定文件名静态伺服**——无内容 hash，重建后 URL 不变，开发模式旧页面的动态分块 404 从根上消除，见「构建性能」）经 WS `draw.result` 或 REST `POST /sessions/:id/draw` 回传结果（**前端按 payload.format 原样分发**——`onDrawRender` 不做语言归一化，未知语言由 `renderDiagramSvg` 显式报错；echarts 曾被这里的归一化吞掉喂给 PlantUML 引擎，最新前端也报「PlantUML 渲染错误」）；**飞书通道与 `render=backend`** 由服务端**组合后端渲染器四语言全支持**（见「后端图表渲染」）——`show` 图表分支 **`render` 参数选择渲染通道**：`frontend`（**默认首选**，浏览器本地渲染 SVG 可交互缩放、零服务端开销，降低后端性能压力）/ `backend`（**服务端直接渲染成 PNG 图片**：引擎经 ToolContext `renderDiagram` 调用组合渲染器，落盘会话 `tmp/{name}.png` 并返回 `image` 内容块（**前端图片块点击进入全屏查看器**：缩放/平移/复制/下载），**仅导出/分享图片等确需 PNG 文件时使用**，四语言均支持；**前端渲染不可用（收到「画图能力受限」）时提示模型改用 backend 重试**）；**`code` 与 `path` 二选一（文件渲染）**——`path` 指定会话内已存在的图表文件（`.mmd`/`.mermaid`/`.puml`/`.plantuml`/`.d2`/`.echarts`）直接读取渲染（图表名默认取文件主名，format 未传时按扩展名推断，规范化源码落盘 `tmp/{base}.{ext}`，已生成图表文件再渲染/换通道时不重发源码），`code` 缺省仅需 `path`；**渲染成功工具才返回成功**（前端：图表块 + 落盘源码文件；后端：图片块 + 落盘源码/`.png`），渲染报错（语法错误等）把错误文本返回给模型（据此修正源码，错误信息指明失败的语言），**5 秒超时返回「画图能力受限」**（前端/飞书通道渲染端离线或引擎加载失败时降级，不阻塞任务；`render=backend` 不经前端回传，走服务端渲染器自身超时兜底）；**echarts 通道加固（实测复盘，前端渲染为默认正确通道、不自动换通道旁路）**：① 服务端**预校验 JSON**（`parseEchartsInput` 纯解析零渲染开销）——无效 JSON 立即精确报错，不白跑前端一轮；② **前端版本错位明确诊断**：前端报错引擎与请求语言不符（如请求 echarts 却由 PlantUML 引擎报错——旧版前端把未知语言静默当 PlantUML 渲染）时，返回「前端渲染器版本过旧，请刷新页面」的诊断而非误导性的源码错误；新版前端对未知语言自身也显式报错引导（见「前端本地渲染引擎」）
- **后端图表渲染（四语言）**：组合渲染器 `core/support/diagram-render.ts`（show 图表分支 `render=backend` 与飞书通道共用，浅色主题白底图）——**plantuml**：复用 `feishu-bot/plantuml.ts`（TeaVM 引擎 + 极简 DOM 垫层，串行队列）；**mermaid**：`mermaid` npm 包 + **happy-dom DOM 垫层**（固定浅色主题 `htmlLabels: false` 纯 SVG 输出；happy-dom 无布局引擎，`getBBox` 以几何属性估算覆盖——rect/path/line 坐标、文本按**全角 1.0em / 半角 0.6em 逐字符估算**（全角统一按半角 0.6em 会低估约 40%，中文标签经 getBBox→viewBox 传导导致整图偏窄、文字被裁剪），否则 mermaid 布局坍缩为 16px）；**d2**：`@terrastruct/d2` WASM（node-esm 构建，**文件路径 Worker**——bun build 无法内联：dev 直接 import 包；**二进制模式**从内嵌产物（`packages/server/scripts/build-d2js.ts` 生成 `d2js.embedded.generated.json`，gzip base64，随产物打进二进制）物化到 `{GEBAI_HOME}/vendor/d2js/{version}/` 后动态 import，版本目录幂等物化 + 旧版本清理）；**echarts**：`echarts` npm 包 SSR 渲染（`init(null, null, {renderer:"svg", ssr:true, width, height})` + `renderToSVGString()`，零 DOM）——`parseEchartsInput` 解析 JSON option（宽松 JSON：容忍注释/尾逗号）或 `{"option":…,"width":…,"height":…}` 信封（尺寸钳制 200-4000，默认 960×600），注入 `animation:false`（SSR 静态输出必须）；**标题/图例防重叠**（echarts 6 图例默认在底部，模型按 v5 习惯显式 `legend.top: 0/'top'/小数值` 时置顶图例与顶部标题必然压字——纵向冲突时图例自动下移到标题底边之下，越界时联动下调未显式设置的 `grid.top`，规则见常量表「ECharts 标题/图例避让」，前端 `parseEchartsOption` 同规则）；**echarts 求值环境隔离**（zrender 环境探测在模块求值期完成，误判浏览器环境后（浏览器路径文本测量需真 canvas，垫层不支持）一旦垫层卸载全局 window 即抛 `window is not defined`；而 PlantUML/happy-dom 垫层在模块作用域安装全局 window/document 且不还原，谁先加载会决定判定结果——模块顶部对全局作**临时清理后求值（TLA）并立即还原**，使环境判定与加载顺序解耦；diagram-render.ts 经引擎与飞书桥接惰性 import，不增加启动开销）；**SVG → PNG** 共享栅格化（`@resvg/resvg-js`，2x 超采样、1600×2400 上限）：根元素规范化（百分比尺寸显式化、**负原点 viewBox 平移归一**——resvg 对负原点 viewBox panic，以 `translate` 包裹内容平移进正象限且不包裹 `<style>`/`<defs>`/`<title>`）；**全局环境切换**（共享进程全局 DOM/Worker，统一走单一串行队列）：happy-dom 垫层与 PlantUML 垫层相互覆盖全局 document——mermaid 渲染前强制重放 happy-dom 垫层、plantuml 渲染前重放其垫层；**`globalThis.window` 仅临时存在**（mermaid 导入/渲染期临时注入，plantuml 渲染后即删）——实测 Bun 的 `node:worker_threads` 在全局 `window` 存在时 Worker 启动挂起（D2 渲染器依赖）；错误按语言包装回传（`Mermaid 渲染错误：…`/`D2 渲染错误：…`/`ECharts 渲染错误：…`），各引擎按需加载（echarts 在模块求值期以隔离环境加载，不拖慢启动）
- 图表配色**跟随 UI 主题**：三种语言按各自机制适配——① **PlantUML 双层机制**：注入引擎确认支持的 `skinparam`（backgroundColor/defaultFontColor/class/object/state/note/activity/时序图分组/生命线/泳道等，追加在源码末尾覆盖用户 `!theme`/自定义 skinparam）+ **渲染后颜色兜底修正**（TeaVM 版仅支持少量 skinparam，其余元素为引擎硬编码默认色）：将引擎默认亮色（节点/激活条/分组背景）、默认暗色（文字/描边）等替换为主题 CSS 变量色；② **Mermaid**：按当前 UI 明暗以 `theme: dark/default` 重新 `initialize`（主题为全局状态，明暗切换时重建；**固定 `htmlLabels: false` 纯 SVG 文本标签**——默认 true 时标签渲染为 HTML 元素包于 `<foreignObject>`，会被注入 DOM 前的 DOMPurify SVG 净化剥离导致「有框无文字」，前端与后端渲染器一致；**`suppressErrorRendering: true` 抑制内置错误渲染**——默认渲染失败（语法错误等）会在 `document.body` 遗留「Syntax error in text」错误图标容器且无法关闭，抑制后渲染期临时元素一并清理，错误统一经 Promise 拒绝由调用方处理：缩略图占位/查看器回退/回传模型修正源码）；③ **D2**：按当前 UI 明暗选主题 ID（亮色 `0`=Neutral Default、暗色 `200`=Dark Mauve，官方默认暗色主题）作为渲染参数；④ **ECharts**：按当前 UI 明暗注入 `darkMode: true/false`（echarts 内置暗色适配——轴线/标签/默认调色板自动切换，用户 option 显式设置的颜色不受影响）；**标题/图例防重叠**（`fixEchartsLegendOverlap`，前后端解析入口统一应用：置顶图例与顶部标题纵向冲突时自动下移避让，见常量表「ECharts 标题/图例避让」）；**JSON option 无法表达函数**（formatter 等用字符串模板 `"{b}: {c}"`，echarts 原生支持；前后端解析一致：宽松 JSON 容忍注释/尾逗号，信封指定画布尺寸）；**明暗判定统一按正文文字色感知亮度**（`hexLuminance(--text) ≥ 128` 视为暗色）；**主题变量为 rgba 时与 body 背景合成不透明等效色**（`css-color.ts` 纯函数，带单测——否则亮色主题下图表退化回暗色默认值、深字深底不可读）；用户显式指定的颜色不受影响；切换主题后已渲染图表自动重绘；SVG 按主题化源码缓存，同图重复渲染零开销；引擎空闲预热（PlantUML + mermaid + echarts；D2 的 8MB WASM 开销大且架构图频率低不预热）避免首次调用超时；渲染 SVG 注入 DOM 前经 DOMPurify 净化（PlantUML 支持 `<html>`/`<img>` 嵌入，防脚本注入）
- 消息 `blocks` 字段持久化于 `chat.json`，历史会话加载时以同一渲染管线呈现，保证「历史可看」
- 工具执行事件（`event.tool.result`）携带 `blocks`，UI 实时展示图片/图表等产物

#### 内容展示（`show`）

全局工具 `show` 是**向用户展示内容的统一入口**（原 `draw`/`render_html`/`show_file` 三工具合并），参数面统一为 `name`（展示名）/ `format`（内容格式，`content` 必选、`path` 可选）/ `content`（内容）/ `path`（已有文件），**内容与路径二选一**（`content` 直给内容、`path` 指向已有文件；同时传或都缺均立即报错引导）；`format` 值域 = 四种图表语言 + `html`（页面预览）+ 三种文本型内容（`markdown`/`code`/`text`），`content` 的解释方式完全由它决定，`path` 未传时按文件真实类型/扩展名推断；另有**可选参数** `language`（`code` 分支的高亮语言）与**可选微调参数** `render`（图表渲染通道）/ `width`/`height`（HTML 预览尺寸），不参与内容源选择；产物均落盘会话 `tmp/` 并返回对应内容块；`card.args="block"` 声明（调用不显示通用工具卡片，内容块直接渲染）；各分支均默认无需审批。

**① 图表交互式创作（`content` + 图表语言 `format`，或 `path` 指向图表源文件）**——专门支持**结构化图表的交互式创作**，**四种图表语言由工具描述/参数说明内置选择指南指导模型按需选择**（四语言对比如下）：

| 图表语言 | 首选场景 | 典型触发词 |
|----|------|------|
| **Mermaid** | 通用场景首选：流程图/时序图/状态图/甘特图/用户旅程、Markdown 文档嵌入（README/Wiki/博客）、简单架构，语法最简洁 | 流程图、时序图、状态图、甘特图、文档 |
| **PlantUML** | UML 与严谨建模首选：类图/组件图/部署图/用例图/活动图/ER 图等标准 UML（支持全部 14 种），表达复杂继承/依赖/关联，语义严谨 | 类图、UML、组件图、部署图、用例图、ER图、继承关系、软件设计 |
| **D2** | 美观架构图与对外展示首选：系统架构/云架构/网络拓扑/微服务，PPT/汇报/技术分享，默认布局最现代化 | 架构图、系统架构、云架构、微服务、汇报、美观 |
| **ECharts** | 数据可视化与统计图表首选：柱状/折线/饼图/散点/雷达/仪表盘/热力图/地图等，动画级视觉效果（SSR 静态输出），适合数据报告 | 柱状图、折线图、饼图、数据图表、统计、报表、Dashboard |

- 组合场景指引（内置在 `format` 参数说明中）：系统设计文档 = PlantUML 类图/组件图 + Mermaid 流程图；架构汇报 = D2 全景架构图 + PlantUML 详细组件图；数据分析 = ECharts 统计图表
- 工具参数：**`format` 必选**（content + 图表语言模式）：`mermaid`/`plantuml`/`d2`/`echarts`——**缺失/非法立即报错列出可选值，不静默回退 plantuml**（实测复盘：模型漏传 format 时 ECharts JSON 被当 PlantUML 渲染，「PlantUML 渲染错误」的报错误导模型连续多轮失败；`path` 模式扩展名可推断时免传）；Mermaid 直接给图定义、PlantUML 源码无需 `@startuml`/`@enduml` 包裹自动补全、D2 直接给声明式文本、**ECharts 给 option 的严格 JSON**（键名与字符串一律双引号，不支持单引号/裸键名/`…`省略号缩写，容错 `//` 注释与尾逗号；值禁止函数，格式化用字符串模板如 `"{b}: {c}"`；可选信封 `{"option": {...}, "width": 960, "height": 600}` 指定画布尺寸，默认 960×600；图例默认在画布底部，与标题同顶冲突时渲染器自动下移避让，无需手动设置 `legend.top`）；`path` 指向已有 `.mmd`/`.puml`/`.plantuml`/`.d2`/`.echarts` 文件直接渲染（图表名默认取文件主名、format 按扩展名推断，避免重发源码；显式传 `format` 可按指定语言渲染任意文本文件）；可选微调 `render` 参数：`frontend` **默认首选**（浏览器本地渲染、零服务端开销）、`backend` 服务端渲染成 PNG 图片（仅导出/分享图片场景，四语言均支持）；配套工具 `read`/`write`（参考素材/落盘说明）、`todo`（拆解步骤）
- 流程：澄清需求 → 选语言 → `show` 生成初始图表（实时渲染——Web 前端本地渲染/飞书桥接后端渲染成图片/`render=backend` 服务端渲染成 PNG 图片，渲染成功才返回成功，报错会收到错误信息）→ 展示并说明图意 → 依用户反馈反复迭代（增删节点、改连线、调整布局）
- **PlantUML 布局规范（防图表杂乱）**：① show 工具描述/参数说明显式要求模型控制布局——流程/时序类图表声明方向（横向流程用 `left to right direction`，分层架构保持默认纵向），**不得靠逐条连线上写 `-down->`/`-right->` 硬控全局布局**；② 密集图表间距由 **⑤布局兜底注入**承担——工具描述**不再**建议手动设置 `skinparam ranksep 80`/`nodesep 40`（自动注入已覆盖，手动建议与之矛盾且冗余）；③ 关系紧密的节点用 `together { … }` 保持相邻；④ 控制规模：单图节点 ≤20 个，架构图按层拆包（package），跨层连线过多时拆成多个图表分别展示；⑤ **布局兜底注入**：源码未显式设置 `skinparam ranksep`/`nodesep` 时，服务端（`injectPlantUmlLayout`）与前端（`plantuml-layout.ts` 同规则）在渲染/落盘前自动注入默认间距参数（仅 `@startuml` 类图；mindmap/wbs/gantt/salt/json/yaml 等布局由结构决定不注入，已显式设置者尊重用户布局）；Mermaid/D2 的布局由各自语言的结构决定，不做注入
- - UI 交互：`diagram` 块在消息流中渲染为**缩略图卡片**（**铺满卡片宽度**的较大尺寸预览 + 文件名——svg 按 `width: 100%` 放大至卡片宽（mermaid 内联 `max-width` 限布局宽、d2 无宽高属性默认 300px 导致预览过小，`max-width: none !important` 压过内联样式），高图限 60vh（横排 48vh）防撑爆消息流；**始终默认渲染**（低性能模式同样自动渲染）；**渲染失败不在主页面暴露错误细节**——仅显示「图表渲染失败，点击查看详情」占位（技术信息进查看器/控制台）；点击进入**全屏查看器**；**同一消息内连续多个图表横排**展示——均分宽度自动换行，节省纵向空间）；**块级工具结果封段**：show/diff 等 `card.args="block"` 工具结果到达时封存当前文本段——图表卡片独立展示，**画图后的输出另起新卡片显示在图下方**（防输出继续追加到画图前的那张卡片、在图上方滚动）；查看器**默认自动适应视口**（初始放大至 125% 显示更清晰、图不超屏）并**居中显示**（缩放基准取 SVG 逻辑尺寸——优先 viewBox，mermaid/d2 渲染时已显式化根 `width/height`（`width="100%"`/无宽高属性的 SVG 在 flex 容器内 `getBoundingClientRect` 解析为 0×0 导致查看器空白）），单栏标题栏（文件名 + 图标工具栏：还原视图、−/＋ 锚点缩放、拖拽平移（zoom 盒 `flex:none` 必需——flex 行容器子项默认可收缩，被压回容器宽后水平滚动区消失、放大溢出被 `overflow:hidden` 裁掉，横向永远拖不动）、复制图片、下载图片、查看源码；**缩放锚点**：滚轮**严格以光标为锚点**（溢出态连续缩放指针下的内容点不动）、按钮以视口中心为锚点——锚点坐标经**量测 zoom 盒实际屏幕位置**（`getBoundingClientRect`）推导，不能按 `scrollLeft+视口偏移` 计算：flex 居中 margin/padding 下滚动几何原点与视口原点不重合，「居中适应 → 溢出可滚动」过渡时锚点会跳（指针下的内容点漂移）；无滚动余量的轴自然钳制保持居中）；**下载图片为 3x 超采样高清 PNG**（SVG 矢量无损放大，本地绘制，无网络）；源码弹窗内支持**一键复制**与**按图表语言的扩展名下载**（`.mmd`/`.puml`/`.d2`/`.echarts`）
- 审批：`write` 需审批；图表分支默认无需审批，降低创作摩擦

**② HTML 页面渲染（`content` + `format: "html"`）**——生成 HTML 页面**直接在聊天界面内渲染展示**（类似图表卡片，但面向网页原型/数据报表/卡片徽章/可视化组件等页面型产物）：

- **参数**：`content`（HTML 源码，完整文档或片段均可，自动补全为完整页面）+ `format: "html"`（必传——内容类型由 format 判定）；`name`（可选，页面标题/文件名，默认 `page`，自动剥离 `.html` 后缀）、`width`/`height`（可选微调，预览尺寸 px——由模型按内容设计显式指定，如移动端页面窄高、宽表格页面宽高；非法值（非正数/超上限）忽略回退默认；已有 `.html` 文件经 `path` 分支直显（html 块，显式尺寸同样生效），不重发源码；`path` + `format: "html"` 亦可把任意扩展名文件按页面渲染）
- - **产物**：HTML 落盘会话 `tmp/{name}.html`（UI 文件面板可见、模型可经 `read` 读取），同时返回 `html` 内容块（`{ type: "html"; html; name; width?; height? }`）随消息持久化，历史会话以同一渲染管线重放
- - **UI 渲染**：消息流中渲染为**预览卡片**（标题栏 + 工具栏：全屏查看/查看源码/复制源码/下载 `.html` + iframe 预览，点击预览或标题进入**全屏查看器**，查看器内可查看源码/下载）；与图表分支同属 `card.args="block"` 声明——不渲染通用工具卡片，调用与结果直接呈现 html 块（无内容块时以输出文本兜底）；**预览宽度机制**：iframe 默认**固定 100% 铺满消息流宽度**，**不参与任何内容宽度反馈**（无尺寸上报、无 resize 回路，杜绝越缩越窄）；**预览高度机制**：无显式 `height` 时 iframe 高度 = **会话区域高度的 2/3**（`.chat-wrap` 声明 `container-type: size` 作为尺寸容器，iframe 用容器查询单位 `height: 66.67cqh` 随窗口尺寸自动重算；不支持 cqh 的旧浏览器回退 480px）；含 HTML 卡片的消息脱离 `.msg` 的 `fit-content` 宽度计算（iframe 为 replaced element，其 `width:100%` 在 max-content 计算中会退化回内置 300px 宽把消息压窄）——消息占满消息流宽度且 `.msg-body` 以 `flex:1` 撑满，实测锁定 300px 的塌缩被根治；模型显式传 `width`/`height` 时按指定值渲染（宽于消息流的显式宽度由卡片横向滚动承载）
- - **安全（域隔离）**：模型生成的 HTML 视为不可信输入，渲染于 **sandbox iframe（`allow-scripts`，不含 `allow-same-origin`）**——脚本可执行但运行在**隔离 opaque origin**：无法访问宿主页面 DOM/Cookie/存储（跨域访问抛 SecurityError）、无法顶层导航（无 `allow-top-navigation`）、弹窗（无 `allow-popups`）、表单提交（无 `allow-forms`）、下载（无 `allow-downloads`）；嵌套 iframe 继承沙箱限制；`referrerpolicy="no-referrer"` 防 Referer 泄漏；**CSP meta 注入** srcdoc 文档 head（`default-src * data: blob:; script-src 'unsafe-inline' 'unsafe-eval'; style-src 'unsafe-inline'`）放行内联/外部脚本、样式与网络，页面自带 CSP 与注入策略取交集只会更严
- - **脚本说明**：脚本在沙箱内正常执行（交互/动态效果/外部脚本均可）；跨域网络请求受浏览器 CORS 与第三方 Cookie 限制（opaque origin 请求带 `Origin: null`，仅 `Access-Control-Allow-Origin: *` 响应可读，SameSite 默认 Lax 不携带跨站 Cookie）；需访问宿主页面数据/同源 API 的诉求无法满足
- - **主题跟随**：iframe 预览区边框与背景走主题变量（`--border`/`--bg-inset`），内容页无自带背景时与主题一致（避免亮色突兀块）；srcdoc 注入 `style#gebai-theme` 同步**主题变量集**（`--bg`/`--text`/`--accent`/`--border`/`--radius-*`/`--font-mono` 等 20 个核心变量写入 `:root`，工具/预览页面可用 `var(--x)` 引用主题色）与背景/滚动条色值，并设置 `data-theme` 属性（支持 `[data-theme]` 分支样式）；初始快照注入，宿主主题切换经 postMessage `gebai-host/theme` 广播动态更新——**`gebai:theme-change` 事件驱动立即广播**（免 500ms 轮询延迟，轮询兜底异步加载）；内容页自带背景/变量时其自身规则（文档中靠后）覆盖注入值，不破坏内容设计
- **审批**：默认无需审批（仅展示 + 写入会话 `tmp/`，与图表分支同级）

**③ 文本型内容（`content` + `format: "markdown"|"code"|"text"`，或 `path` + 显式 format）**——把文档/源码/纯文本**直接呈现给用户**（长文档、源码交付不必堆进回复正文）：

- **三种格式**：`markdown` 按 markdown 渲染为排版文档（标题/列表/表格/引用/代码块）；`code` 为源码/配置文件，可选 `language` 指定高亮语言（`typescript`/`python`/`bash`/`json`/`yaml`/`sql` 等，入参归一化为小写，缺省按 `path` 的真实文件扩展名推断、推断不出时由前端自动识别）；`text` 为纯文本（日志/命令输出/配置片段），**不做语法高亮**；`name` 缺省时产物主名取 `doc`（markdown）/ `code` / `text`
- **产物**：落盘会话 `tmp/{主名}-{内容哈希8}.{扩展名}`（`markdown` → `.md`、`text` → `.txt`、`code` → 按语言映射的扩展名（`core/base/diff.ts` 的 `extForLang`，未命中回落 `.txt`））——UI 文件面板可见、模型可经 `read` 读回、可下载；命名带内容哈希，同内容幂等、异内容各存（历史消息里的产物引用不被后续同名产出覆盖）
- **内容块**：统一 `code` 块（`{ type: "code"; text; language?; path; name }`）——`markdown` 由前端文件内容卡**渲染为文档**（而非源码高亮）、`code` 按语言语法高亮并标语言徽标、`text` 走**纯转义呈现**（不自动高亮、不标语言徽标）；超 4 万字符（`SHOW_TEXT_MAX_CHARS`）块内截断，全文留在产物文件（与 `path` 直显同口径）
- **`path` + 显式 format**：可把任意文本文件按指定格式解释（不限扩展名，如 `.txt` 按 `markdown` 渲染/任意文件按 `code` 高亮）；未显式传 format 时仍按扩展名推断（既有行为）
- **通道与审批**：文本型分支不依赖前端渲染能力，实时 / 飞书 / REST 全通道可用（产物路径随输出文本给出）；默认无需审批（仅展示 + 写入会话 `tmp/`，与图表/HTML 分支同级）

**④ 文件主动展示（`path` 内容源）**——把文件**直接展示给用户**（交付产物 / 需要用户过目的文件）——**按文件类型产出直显内容块，内容在消息流内联呈现**：

- **直显矩阵**：图片（PNG/JPG/GIF/WebP/SVG/BMP）→ `image` 块（内联 `<img>` + 点击全屏查看器）；图表源文件（`.puml`/`.plantuml`/`.mmd`/`.mermaid`/`.d2`/`.echarts`）→ 走①的渲染验证管线（format 按扩展名推断，与 `content` 模式同一闭环，重新渲染/换通道不重发源码）产出 `diagram` 块；`.html` → `html` 块（沙箱 iframe 页面预览，显式 `width`/`height` 生效）；文本/代码（txt/md/csv/json/yaml/log/常见代码扩展名，≤512KB）→ `code` 块（附带 `path`/`name`，文件内容卡渲染：md 渲染、源码语法高亮、工具栏复制/原文件/下载；超 4 万字符截断展示，全文经工具栏下载或「原文件」弹窗获取）；**无扩展名/dotfile**（`LICENSE`/`Makefile`/`Dockerfile`/`.gitignore` 等）→ **按内容探测判定**（无 NUL 字节、替换字符与控制字符占比 ≤1% 视为文本）后同样走 `code` 块内联，否则回落 `file` 块——这类文件没有扩展名可信、展开白名单永远追不全，只能按内容判定；探测**刻意只覆盖无扩展名/dotfile**（空扩展名没有「类型承诺」，探测零风险）：`tiny.pdf` 这类有扩展名的格式即使内容恰好是纯 ASCII 也仍按扩展名给卡片（探测它们会把二进制格式误判成文本，已有用例固化）；**语言/类型一律按真实文件路径（`abs`）推断，不用展示名 `name`**——`name` 惯例不含扩展名（参数描述即写「不含扩展名」，实测历史调用 `{path:"xxx.md", name:"调研报告"}`），据它推断 language 为空 → 前端只能 `highlightAuto`，markdown 被当源码高亮而不是渲染成文档（yml/json/ts 等同样丢高亮）；**无法内联的类型**（PDF/压缩包/Office 等）→ `file` 块（文件内容卡按 mime 分派渲染，内容进入视口才按需加载）；**音视频**（`video/*`/`audio/*` 或扩展名 .mp4/.webm/.mov/.m4v/.mkv/.mp3/.wav/.m4a/.aac/.ogg/.flac）→ 同走 `file` 块，卡内渲染浏览器原生 `<video controls>` / `<audio controls>`（取数经 `files/preview`，**服务端 `Response(Bun.file(…))` 自动处理 Range** → 206 + `content-range`，进度条可拖动；不自动播放，`preload="metadata"` 只拉元数据）
- **与普通工具的语义区分**：`show` path 分支是「给用户看」的主动通道（内容直接呈现）；`read` 等常规工具按既有行为返回产物块（文件类产物为查看/下载卡片，不自动展开内容）
- **参数**：`path`（与 `content` 二选一，会话 `tmp/` 相对路径（前缀可省略）；本地模式可为工作区相对/绝对路径）、`name`（可选，展示文件名，默认取文件主名；**仅影响展示名与产物文件名，不参与类型/语言判断**——类型一律按真实文件推断）、`format`（可选，显式指定时按该格式解释文件内容：图表语言 → 渲染该文件、`html` → 页面预览、`markdown`/`code`/`text` → 文本型解释，不再按扩展名推断）、`language`（可选，与 `format:"code"` 搭配的高亮语言；未传按真实文件路径扩展名推断）
- - **路径处理**：会话 `tmp/` 内文件**直接引用**（零复制）；会话外文件（本地模式工作区/绝对路径）**复制一份**到会话 `tmp/shown/{主名}-{内容哈希8}.{扩展名}`（内容哈希命名，重复展示复用同一副本；上限 100MB，超出引导改为告知路径）后引用——前端文件接口只服务会话 `tmp/`，复制保证可见性与文件面板留存。**归属判定用会话 `tmp/` 真实绝对路径**（`sessionPath` 拼接；项目绑定子Agent 的 `resolvePath` 基准是项目根，不能作判定依据），复制目标同样直接写会话 `tmp/` 绝对路径（绕开项目根基准，保证落在真实会话文件区）
- - **mime 推断**：按扩展名映射（图片 + PDF/文本/CSV/JSON/HTML/Office/音视频等常见类型），驱动前端预览入口形态（图片内嵌/PDF 内嵌查看/文本高亮/下载提示）
- **审批**：默认无需审批（只读 + 复制进本会话 `tmp/`，与图表分支同级）；`card.args="block"` 声明（调用不显示通用工具卡片，内容块直接渲染）

**分支门控（合并型工具的通道能力校验）**——`show` 不声明工具级 `interaction`（全模式可见），引擎把当前任务的 `interactionMode` 注入 ToolContext（`ctx.interactionMode`），工具在分支内校验通道能力：`html` 分支（`content` + `format: "html"`，或 `path` 指向 `.html`/显式 `format: "html"`）仅 `realtime`（飞书/REST 下明确报错「当前通道不支持 HTML 页面预览」，引导改用文字描述或产出 `.html` 文件后经 `path` 交付）；图表分支 `realtime` → 前端渲染、`multi_turn` → 飞书后端渲染（同一 `event.draw.render` 通道）、`none` → **不空等 5 秒超时**直接引导 `render=backend`（backend 为服务端能力，无交互模式可用）；`path` 文件分支全模式可用（REST 消费者仍可拿到内容块）；文本型分支（`markdown`/`code`/`text`）不依赖前端渲染，全模式可用。未注入 `interactionMode`（测试桩/无引擎环境）不做分支门控。

### 上下文保护

对话上下文（消息历史 + 工具返回）超出 LLM 上下文窗口时，自动执行压缩；也支持用户主动压缩。

#### 自动压缩

- 触发条件：**窗口剩余不足以支撑一次回复**时自动触发，无需人工干预（不以窗口百分比判定——真正约束是「留给输出的空间」，输出上限由模型与配置（`GEBAI_LLM_MAX_OUTPUT_TOKENS`）决定，并非窗口的固定比例）
- **压缩四原则**（用户口径，逐条落到 `planCompactRange` + `store.compactMessages`）：
  ① **近消息是滑动窗口**：最近 `COMPACT_WINDOW_MESSAGES`（12 条，任意角色；上限为历史一半，避免短会话压不动；`GEBAI_COMPACT_WINDOW` 可调）**永不进**压缩区间（原样保留），随新消息自然向前滑动；
  ② **远消息用模型压缩、原消息完全抛弃**：区间取最早的连续一段（远的先压），区间内**除系统提示词以外**的消息（含用户输入、assistant、tool 结果）全部由摘要替换并**从上下文移除**——不残留原文（会话记录 chat.json 同样只留摘要，原文不另存）；摘要恒为一条且置于消息数组**最前**（模型先读历史摘要再读近期原文，与近期消息不交错：越旧的信息越靠前）；
  ③ **压到目标水位即停**：上水位（触发）= 剩余 < 输出预留（窗口 - 输入 < reserve）；下水位（目标）= 窗口 × `COMPACT_TARGET_RATIO`（缺省 40%，`GEBAI_COMPACT_TARGET_RATIO` 可调，合法区间 0.1~0.9；且**不高于触发线**——比例定得比触发线还高时永远压不到，夹到触发线即停）——需腾出量 = 真实基线 − 目标输入，压到目标即停；可压消息不够（窗口外全压完仍高于目标）时压满窗口外，不足部分交给迭代压缩与溢出恢复；
  ④ **系统提示词不要压缩**：压缩区间只含可压缩消息（`isCompressibleMessage`）——**角色 system 的消息一律不进区间**，夹带时由 `compactMessages` 原位保留（主 system 段消息、子Agent 装载提示词均不被摘要替换）；缓存友好前缀请求会把它们作为主循环同前缀一并发给模型（前缀缓存匹配所需，模型只是重看一眼已看过的内容），但会话记录里的这些消息一字不改、位置不变
- **上下文占用口径：只认模型服务返回的真实大小**（`input_tokens`，含 system 提示词与工具 schema，即「真上下文」）——压缩判定不依赖任何估算，估算容易误判（chars/4 对中文低估 2~4 倍、工具 schema 与图片块又难以折算），一律以接口真值为准：
  - **跨 run**：上次任务最后一次调用返回的真实 `input_tokens` 持久化为基线（`SessionData.ctxInputTokens`）；下次 run 基线本身已致剩余不足一次回复即先压缩（本次调用只会更大），不做增量估算
  - **任务中途**：每轮调用返回的真实 `input_tokens` 使窗口剩余 < 输出预留时压缩最早历史（`makeContextRoom`），长任务不再等接口拒绝
  - **溢出恢复**：接口以上下文长度错误拒绝（4xx = 真实大小的权威信号）时，压缩最早历史后重试（至多 3 次）——无基线会话首次调用即超窗也能收敛，不再任务失败
  - 接口不返回 usage（或压缩替换消息导致索引锚点失效）时不估算预判，由本次调用的真值 / 溢出恢复接管；估算仅用于**压缩量规划**（`estimateMessageTokens`：需腾出多少空间→压缩多少条；真值口径的需腾出量按 `estTotal/基线` 比率**折算为估算口径**再逐条累计——估算实测低于真值 20%~30%，不折算会压过头或压不到目标）与会话列表 ctxTokens 展示的增量补足（**估算口径含工具 schema 段**：schema 不在 messages 里却真实计入 input_tokens，由 `estimateSchemasTokens` 补齐——压缩重建后推送与持久化的估算值因此不再系统性偏低、下一轮真值回来时不跳变）
  - **展示值随真值同步落盘（运行中）**：每轮拿到真值后除推送 `event.session.ctx`，还调 `store.updateCtxStats`（只重写 meta.json，不重写 chat.json）——会话列表 / 状态快照 / 页面刷新读到的与实时推送一致；否则这些读取面只能拿到「上次任务结束时写入的值」，运行中反复刷新会在陈旧值与当前真值之间来回跳（同一会话两个百分比轮流显示）
  - **展示值随上下文改写失效**：压缩（`compactMessages`）与溢出护栏降级（`degradeProtectedMessages`）改写了历史，真值基线与展示值同时失效——清基线并把 `ctxTokens` 重算为当前消息估算（与 `truncateMessages` 同口径）；压缩成功后引擎还**立即补发一次** `event.session.ctx`（压缩后估算 + schema 段），圆环当场回落——不补发则 UI 停在「压缩前」的百分比上直到下一轮调用（观感：压缩了但百分比没掉）
  - **输出预留取值**（`outputReserveTokens`）：模型单次响应输出上限（能力声明 `maxOutputTokens`），未声明时缺省 16384，并夹在 `[1024, 窗口一半]` 内——预留不能超过窗口一半，否则小窗口模型会永远处于「剩余不足」而反复压缩
  - **提示词缓存命中度量（展示口径）**：usage 中的缓存命中字段统一提取为 `LLMUsage.cachedTokens`（OpenAI chat/responses 的 `prompt_tokens_details`/`input_tokens_details.cached_tokens` 已含在 input 内，Anthropic 的 `cache_read_input_tokens` 在 `input_tokens` 之外——pickUsage 折算并入 inputTokens 统一「cached ⊆ input」口径）；随真实 usage 基线同点位流转：每轮经 `event.session.ctx` 携带 `ctxCachedTokens` 推送、任务结束持久化为 `SessionData.ctxCachedTokens`（接口不返回缓存字段时 undefined，撤回/压缩清基线时一并清除），前端上下文圆环悬浮展示「缓存命中 tokens（占比）」。仅度量不改变请求构造——三家接口均未发送缓存控制标记（Anthropic `cache_control` / OpenAI 自动前缀缓存），命中率由服务端自动前缀缓存自然产生
- 压缩策略（按序使用）：
  0. **超长用户输入落盘（预防）**：发送时超过阈值的用户输入自动全文写入会话工作目录 `user_inputs/{内容哈希}.txt`（原文不丢——文件面板可见、模型可经 `read` 工具读取全文；按内容哈希命名，相同输入映射同一文件），消息正文保留头尾预览 + 文件引用，避免大段粘贴撑爆上下文；未超阈值原样不变，落盘失败降级为原样保留（不改变优先）
  1. **工具大输出截断**：工具返回超过截断阈值自动截取头尾摘要（**按行保留完整行**，避免切断半行/半条目；单行巨长如 minified 时该行按字符兜底），完整内容写入文件，截断消息中附带文件路径供大模型后续读取。**引擎兜底（不依赖工具自觉）**：工具未自行截断的超长输出，由引擎在主循环统一截断落盘——凡 `output` 超过截断阈值且未带 `truncated` 标记的结果，自动复用同一截断逻辑（含内容块保留），保证任何第三方/新工具都不会撑爆上下文；已自行截断的工具结果不重复处理
  2. **旧消息摘要（压缩后置于消息数组最前，原消息完全抛弃）**：将最早一段历史消息（**除系统提示词以外全部**：用户输入/assistant/tool）由 LLM 生成摘要，摘要替换这些消息并从上下文中移除（**摘要恒为一条且位于消息数组最前**，不与近期原文交错；原文不另存——摘要即其在会话中的唯一留存形态，模型与 UI 都只看得到摘要）。摘要保留关键信息。**摘要输入优先用「缓存友好前缀请求」**：直接把与主循环**逐字节同前缀**的历史原文发过去（同一 system 提示词 + 同一段历史 + 同一批工具 schema，经 `loadHistory(upToIndex)` 渲染）+ 尾部压缩指令——服务端前缀缓存命中，已处理过的 token 按缓存价计（比把历史重写成骨架再发全价更便宜，且保留完整原文与工具调用史）；不可用时退回「骨架行 + 分块」：每条消息压成一行（assistant 工具调用轮 content 常为空——补上工具名与参数摘要，否则「调用过什么工具」在摘要里彻底消失；tool 结果带工具名与内容前段），单块输入上限 20000 字符、至多 6 块（总覆盖约 12 万字符），超出时**逐块摘要后合并**（map-reduce；单块失败跳过，不影响其余块）；总量超总预算时**头尾保留**（最早的任务背景与最新进度都进摘要）+ 中部省略说明行——不再按 20000 字符一刀切静默丢弃（52 万 token 窗口下一次压缩常覆盖 15 万字符以上，旧实现只有约 1/8 进摘要且无任何省略提示）。**滚动摘要合并**：压缩时既有摘要摘内容作为「此前摘要」并入新摘要并随新摘要替换（摘要恒为一条，不再随压缩次数累积）；**区间端点对齐 `assistant(toolCalls)/tool` 配对边界**（边界处的工具结果一并纳入，避免被当孤儿丢弃）；**摘要失败降级为骨架行**（角色 + 工具名/参数 + 内容首段，而非一句空占位），原文仍在会话记录中可查看。**系统提示词不可压缩**（`isCompressibleMessage`：仅 role=system 被排除；用户输入/assistant/tool/引擎注入消息均可压缩）——不选进压缩区间、区间夹带时原位保留；压缩条数只计实际移除的消息
  3. **摘要超限截断**：摘要本身超长时按 `SUMMARY_OUTPUT_LIMIT`（2000 字符）截断——更早历史的移除由压缩区间（②）与溢出护栏承担，不另设「滚动裁剪」独立步骤
  4. **溢出硬护栏（压缩无法收敛时的最后防线）**：压缩无内容可压时（历史几乎全是系统提示词，或可压缩消息都在滑动窗口内）受保护消息让路——最旧用户消息的图片附件降级为文本说明（可用 vision/read 按需查看）、仍不够将最旧用户消息替换为裁剪占位（占位文本明示「原 N 字符，**原文不再保留**」——压缩与护栏都会改写 chat.json，原文不另存）；**最新一条用户消息（本次任务输入）永不裁剪**；压缩为**迭代执行**（run 前按基线迭代压缩直至收敛，任务中途/溢出恢复经 `makeContextRoom` 压缩 → 护栏降级两步腾挪）
  5. **历史图片内联窗口**：仅最近 3 组含图片的消息（用户图片附件与工具结果图片——read 读取的图片引用）内联进上下文，更早的图片降级为路径说明（图片永久占窗口且不参与压缩，长会话会被历史图片占死窗口）
  6. **LLM 流式读空闲超时**：SSE 建立后连续 120 秒无任何 chunk 判定接口假死，中止本次调用（无产出走重试、有产出上抛为任务错误）——此前网关/上游挂起会无限挂起任务
- 压缩过程对用户透明，UI 显示压缩通知（压缩范围、摘要内容）——**被压缩区间的原文不另存**（chat.json 只留摘要，见原则 ②）
- 压缩后继续原任务流程，不影响进行中的工具调用循环

> **实现**：已落地。`engine.compactSession()` 支持主动（`session.compact` / REST `POST /compact`，scope 指定区间）与自动触发（**窗口剩余 < 输出预留**时压缩，触发与目标口径见「上下文占用口径」与「常量参考」）；摘要由 LLM 生成，**首选「缓存友好前缀请求」**：直接把与主循环**逐字节同前缀**的历史原文（同一 system 提示词、同一段历史、同一批工具 schema，经 `loadHistory(upToIndex)` 渲染）加上尾部压缩指令发给模型（`CACHE_PREFIX_INSTRUCTION`）——服务端前缀缓存（OpenAI 自动前缀缓存 / DeepSeek 上下文缓存 / 智谱・千问等同机制）因此**命中**，已处理过的 token 按缓存价计（多数服务商 10%~50%），且无需把消息重写成骨架（保留完整原文与工具调用史，摘要信息量更高）；不可用时退回「骨架行 + 分块」路径：前缀装不下（`estimateMessageLikeTokens` 超 `窗口 - 8192`）、前缀内有既有摘要无法吸收、上次同模型调用未命中缓存（`usage.cachedTokens=0`，按 provider+model 记忆，不白付全价）、空文本/接口异常。`GEBAI_COMPACT_CACHE_PREFIX` 可强制开关（0/off 强制骨架、1/on 强制原文前缀、缺省自适应）。摘要调用同样带读空闲超时与取消信号（同 `chatWithIdleTimeout` 防假死）——压缩在任务流程内同步等待，无超时会把整个运行中任务永久挂死；失败降级为骨架行占位（保留被裁剪内容的工具/文件脉络，而非一句空话）。摘要消息持久化（`compacted`/`summary` 标记，UI 渲染为压缩通知，历史重载时作为 **user 角色**注入且置于历史最前）；已压缩摘要消息不重复保留（压缩时既有摘要内容作为「此前摘要」并入新摘要输入、随新摘要替换——长会话摘要恒为一条且始终在数组最前）。**手动压缩在会话有任务运行时被拒**（summarize 是秒级 LLM 调用，期间任务持续追加消息，陈旧压缩区间会套删未参与摘要的新落盘内容；自动压缩经 `internal` 标记在任务流程内自身协调，不受此限）。**超长用户输入落盘**（`spillLongUserInput`，run 发送时执行）：超阈值输入全文写入会话工作目录 `user_inputs/`，消息正文保留头尾 + 文件引用。**压缩只不碰系统提示词**（`isCompressibleMessage`：仅 role=system 被排除；用户输入/assistant/tool/引擎注入提醒均可压缩）——系统提示词消息（含装载提示词）不选进压缩区间、区间夹带时由 `store.compactMessages` 原位保留不移动；缓存友好前缀请求会把系统提示词作为**主循环同前缀**一并发给模型（前缀缓存匹配所需），但**会话记录里它一字不改、位置不变**；区间内无可压缩消息时不做任何改动（不创建摘要、不动 usage 基线）；`compactMessages` 的压缩条数只计实际移除消息数。超限截断（`trimToCacheLimit`，2000 条上限，纯存储安全网）**按批裁剪**：超限时一次裁到低水位（`TRIM_LOW_WATER_MESSAGES`=上限×0.9=1800 条，单批腾出 `TRIM_BATCH_MESSAGES`=200 条余量）而非逐条挤出——裁剪会改写送模型的历史前缀（服务端前缀缓存失效），逐条挤出等于每条新消息都改一次前缀，按批裁剪把前缀变化摊薄到每积累 200 条消息一次；同样保护：受保护消息原位保留、从最早的其他消息开始丢弃，受保护消息本身使长度仍高于低水位时按原样保留（软上限，不改变优先），丢弃按 tool_call 配对原子执行——assistant(toolCalls) 被丢弃时连带其后紧邻的 tool 结果（拆散配对会产生孤儿 tool 消息，严格校验的 LLM 接口会拒绝整个请求），实际保留条数可略低于低水位；截断保护同上（`compacted`/`loadedAgent`/用户输入消息在超限截断中原位保留、不重排）。**配对完整性修复（`repairToolPairing`）**：任务取消中断、压缩/截断边界或旧版本缺陷仍可能产生「孤儿 tool 结果」（发起 assistant 已删）或「未应答 toolCalls」（结果缺失）——严格校验的接口（OpenAI tool_calls/tool、Anthropic tool_use/tool_result 配对）会拒绝整个请求，会话自此每次运行 400 卡死。修复分层落地：① 引擎工具循环取消/异常路径为本轮全部 toolCalls 补写占位结果（assistant 先落盘后执行的中断不再缺结果）；② 存储层 `compactMessages` 压缩后、`readFileByPath` 磁盘装载时即时修复（孤儿普通 tool 丢弃、孤儿受保护 tool（subsession_run 存档）补最小 assistant 桩、中途未应答补占位结果；尾部未应答**不在存储层 flush**——正常执行流 assistant 先落盘结果随后到达，提前 flush 会让真实结果反被判孤儿丢弃）；③ `llm.ts` 三家序列化入口兜底（含尾部 flush + Anthropic 相邻同角色 user 消息合并），旧版本已损坏的会话在下一次模型调用自愈。压缩替换消息后真实 usage 基线的索引锚点失效：`store.compactMessages` 自动清除 `ctxInputTokens`/`ctxAtMessage`，并把展示值 `ctxTokens` 重算为当前消息估算（旧展示值不得残留，否则 UI 停畵在压缩前）；引擎包装层在 `compacted > 0` 时立即补发一次 `event.session.ctx`（压缩后估算 + schema 段）并落盘，圆环当场回落。**压缩迭代 + 溢出护栏 + 中途压缩 + 溢出恢复 + 读空闲超时均已落地**（`engine.ts`：`makeContextRoom`/`degradeProtectedMessages`/`callModelWithOverflowRecovery`/`isContextOverflowError`/`chatWithIdleTimeout`；图片内联窗口 `INLINE_IMAGE_RECENT=3` 作用于 `loadHistory`）。**摘要输入保真与滚动合并**（`compressor.ts`：`summarizeMessageLine` 工具调用骨架、`buildSummaryChunks` 分块与头尾保留、`summarizeFallback` 骨架降级、`summarize` 的 map-reduce 与合并、`compactSession` 的既有摘要吸收与区间配对对齐）；**输出预留驱动的压缩规划**（`outputReserveTokens` + `lacksOutputRoom` + `planCompactRange`：触发看「窗口剩余是否还够一次回复（maxOutputTokens）」，目标为「窗口 × `COMPACT_TARGET_RATIO`（缺省 40%，`GEBAI_COMPACT_TARGET_RATIO` 可调），且不高于触发线」——**压到目标即停**，压缩区间不越**近消息滑动窗口**（最近 12 条消息，`COMPACT_WINDOW_MESSAGES`/`GEBAI_COMPACT_WINDOW` 可调，上限为历史一半；**有真实占用基线时窗口仅为保底下限**——压多少由水位算出的需腾出量决定），需腾出量为真值口径、按 `estTotal/基线` 比率折算为估算口径后逐条累计（`estTotal` 含工具 schema 段估算）——确保「最近的保留、远的先压、压到目标即止」）；**截断记录与提示**（`SessionData.trimmed` + `loadHistory` 注入「[历史裁剪]」提示，模型知道更早内容已不在上下文中，而不会把历史从中间开始当作完整历史）；**溢出护栏降级可见**（`degradeProtectedMessages` 经 `event.message.compact` 发布 `degraded` 事件，UI 显示上下文为何变化）；**超限裁剪用户可见**（`engine.notifyTrim`：`SessionData.trimmed` 计数增长时经同通道发布 `degraded: "trim"`，前端标题「历史消息条数超限裁剪」——此前截断只有模型侧的 `[历史裁剪]` 提示、UI 无声，长会话里易被误认为「压缩没生效」）。

#### 主动压缩

- 主动压缩经接口触发（REST `POST /api/v1/sessions/:id/compact`；**无 `/compact` 斜杠命令，UI 不提供压缩按钮**——日常上下文由引擎按水位自动压缩，见上）
- 支持自定义压缩范围：`scope` 取 `"all"`（全部历史）或 `{from,to}`（指定消息区间）——**无「仅工具输出」**（仅 REST/WS 入口可指定 scope）
- 主动压缩同样走上述策略，压缩结果立即生效并持久化

#### 截断保护存储

- 存储路径：`{session}/tmp/truncated/{tool_name}_{content_hash}.txt`（`{session}` 为会话根目录 `{GEBAI_HOME}/users/{user}/sessions/{s0}/{s1}/{session_id}`）
- 消息中返回**会话工作目录内相对路径**（如 `truncated/read_xxx.txt`）：模型可经 `read` 工具直接读取，且同一路径在 `sh`/`py`/`js`（cwd 即会话工作目录）里可直接使用——带 `tmp/` 前缀会被脚本当子目录多套一层（`tmp/tmp/…`）；前端文件面板同步可见/可下载（面板/REST 契约路径仍带 `tmp/` 前缀，文件工具对两种写法等价接受）
- 文件名含工具名可溯源；文件名按内容 SHA256 哈希命名（同内容同路径，重复截断幂等覆盖，写入无存在性短路）
- 生命周期随会话：会话删除/过期时随 `tmp/` 整体清理（见「数据生命周期」）

### 消息模型与数据结构

核心数据结构（SDK 与 UI 共用，作为类型契约）：

```ts
// 内容块（消息内容的结构化呈现，UI 逐块渲染；image/file 的 path 为会话 tmp/ 内逻辑路径）
type ContentBlock =
  | { type: "text"; text: string }
  | { type: "code"; text: string; language?: string; path?: string; name?: string }
  | { type: "image"; path: string; name?: string; mime?: string }
  | { type: "file"; path: string; name: string; mime?: string }
  | { type: "diagram"; format: "plantuml" | "mermaid" | "d2" | "echarts"; code: string; name?: string; version?: number }
  | { type: "diff"; oldText: string; newText: string; language?: string; name?: string; oldName?: string; newName?: string; lines: DiffLine[] }
  | { type: "html"; html: string; name?: string; width?: number; height?: number }

// 行级 diff 结果（diff 内容块；diff 工具已移除，仅历史会话回放）：按顺序排列，每行标注差异类型
type DiffLine = { kind: "equal" | "add" | "del"; text: string }

// 消息（持久化于 chat.json）
interface Message {
  id: string
  role: "user" | "assistant" | "tool" | "system"
  name?: string                        // 工具消息的工具名 / 助手消息名
  images?: Array<{ path: string; mime: string }>
                                       // 工具结果图片引用（轻量、不含 base64）：loadHistory 按内联窗口重读内联
  arguments?: Record<string, unknown>  // assistant 工具调用的参数（与 toolCalls 配对）
  loadedAgent?: string                 // 子Agent 装载提示词的 system 消息标记（loadHistory 透传进模型上下文）
  content: string
  reasoning?: string                   // 推理内容（reasoning_content/thinking）独立字段：assistant 消息持久化时写入，content 保持纯正文；
                                       // 回放给 LLM 时（loadHistory）不携带——推理绝不进模型上下文；UI 历史渲染为折叠推理卡；旧版数据推理内嵌 content 的 <think> 块（兼容展示/剥离）
  model?: string                       // 生成该回复的模型名（assistant 消息落盘时由 provider capabilities 携带；仅存储/UI 展示用，loadHistory 不回放给模型；用户反馈 FeedbackInfo.model 据此自动关联，见「用户反馈」）
  blocks?: ContentBlock[]              // 富内容块（文本/代码/图片/文件/图表），随消息持久化
  attachments?: AttachmentRef[]        // 多模态引用
  toolCalls?: ToolCall[]               // assistant 的工具调用请求
  toolCallId?: string                  // tool 结果关联
  createdAt: number
  compacted?: boolean                  // 上下文压缩摘要消息标记（role=system），UI 渲染为压缩通知
  summary?: string                     // 压缩摘要消息：被压缩的原始区间描述（条数/时间范围）
  session?: boolean                    // 子会话运行过程消息（旧版逐条存档，历史兼容）：完整存档但【不进入主 LLM 上下文】（loadHistory 跳过），前端按 runId 分组折叠渲染
  subSessionId?: string               // 子会话运行 run 标识：同一次 subsession_run 执行过程的消息共享（前端回放按此分组）
  subSessionMeta?: { agents: string[]; input: string }  // 子会话 run 元信息（仅该 run 首条消息携带）：折叠容器标题用（预加载子Agent 名与输入）
  subSessionArchive?: SubSessionArchive    // 子会话 run 完整存档（subsession_run 工具调用记录 / 合并消息 / bg_task 结果扩展字段）：执行过程全部内容，历史回放渲染折叠容器；
                                      // 子会话运行（subsession_run）合并消息同样携带（subsession 字段标识，渲染「🌿 子会话」容器）
  subSessionMerged?: { runId: string; name: string; model?: string }
                                      // 子会话合并消息标记（role=user + engineNote: "subsession"）：subsession_run 继承形态子会话最终报告/阶段性合入的消息携带；
                                      // 用于子会话互相感知（父会话增量标注来源）、本子会话自身合入跳过判定；前端按 engineNote 渲染为「子会话合入」通知条
                                      // （drainBranchMerges 注入点即下一条模型调用前，assistant 形态会被思考类模型 400 拒绝）
  engineNote?: "todo" | "verify" | "task" | "branch"
// 引擎提示标记（role=user）：待办续做/收尾验证提醒/任务结果写回/子会话报告合入——
                                      // 与用户输入同角色（避开思考类模型的尾 assistant 约束、并随用户消息受上下文保护），标记供 UI 渲染为
                                      // 弱化通知条（「引擎提示」/「任务」/「子会话合入」），与用户自己发的消息区分；详见「引擎注入消息的角色约定」
  // 旧版（agent_call 时代）字段：subAgent/subAgentRunId/subAgentMeta/subAgentRun 兼容历史会话回放，新数据不再写入
}
interface SubSessionArchive {
  runId: string                        // run 标识（与实时事件 subSessionId 同源）
  agents: string[]                     // 预加载进子会话的子Agent 列表
  input: string                        // 任务输入（容器标题展示）
  output: string                       // 最终返回文本（折叠后摘要展示；异常/取消为空串）
  messages: SubSessionEntry[]          // 执行过程全部消息（输入/每轮回复/推理/工具调用与结果）
  branch?: { name: string; model?: string }
                                       // 子会话运行（subsession_run）标识：存档携带（runId/名/模型），前端容器标题按子会话名渲染
}
interface SubSessionEntry {
  role: "user" | "assistant" | "tool"
  name?: string
  content: string
  reasoning?: string                   // 推理内容独立字段（同 Message.reasoning 语义）：assistant 条目持久化时写入，回放展示不回流模型
  toolCalls?: ToolCall[]
  toolCallId?: string
  arguments?: Record<string, unknown>
  blocks?: ContentBlock[]
  subSessionArchive?: SubSessionArchive    // 嵌套子会话（子会话内再派生）存档递归携带
}

interface ToolCall {
  id: string
  name: string                          // 全局名（含命名空间前缀）
  arguments: Record<string, unknown>
}

interface ChatChunk {                   // 流式输出单元
  kind: "text" | "reasoning" | "tool_call" | "tool_result" | "approval" | "done" | "error" | "reset" | "session_start" | "session_done" | "resume" | "model_error"
  messageId?: string                    // text delta 携带本条 assistant 消息 id（前端反馈/操作绑定）
  subSession?: boolean                  // 事件来自子会话运行过程（subsession_run 派生的子会话；父会话主回复不带此标记）
  subSessionId?: string                // 子会话运行 run 标识：同一次 subsession_run/subsession_run 的执行过程事件共享（前端按此分组渲染）
  subSessionMeta?: { agents: string[]; input?: string; output?: string; subsession?: string; model?: string }  // 子会话 run 元信息：start 携带 agents/input，done 携带 agents/output（折叠容器标题用）；子会话运行（subsession_run）携带 branch（分支名）/model（模型路由名）——容器标题渲染「🌿 子会话 · 名（模型）」
  text?: string
  toolCall?: ToolCall
  approval?: { toolCallId: string; retries: number; tool: string }
  error?: string
  output?: string                       // done 携带的最终输出（含 session 形态）
  blocks?: ContentBlock[]               // tool_result 携带的富内容块（图片/图表/文件）
  retry?: number; maxRetry?: number     // model_error 携带的重试进度（非终态；前端渲染为消息流内常驻异常记录）
}

// 补充语义：
// - `reasoning`：推理内容增量（reasoning_content / thinking），前端渲染为折叠推理块：思考中默认展开实时展示（推理内容可见），推理段结束（正文开始/工具调用封段/流结束）自动折回收起态，用户可点 summary 重新展开；**内容 markdown 完整渲染**（与正文同路径节流——**120ms 尾沿节流是全模式统一路径**，低性能模式不再单独降频）；推理内容超出可视高度（`.reasoning-body` 限高 min(60vh, 720px)）时内部滚动条自动跟随最新内容，用户上翻翻阅历史不打扰（`reasoning-scroll.ts`）；
//   推理**持久化为独立字段**（`Message.reasoning`，content 保持纯正文；历史会话/切回可见，前端默认折叠可展开，内容同样 markdown 渲染），**回放给 LLM 时不携带**（`loadHistory` 仅映射 content——推理绝不进模型上下文）；旧版数据推理内嵌 content 的 `<think>` 块：前端回退解析展示、回放时 `stripThinkTags` 剥离（兼容，不做数据迁移）
// - `text`：文本增量；**携带 `subSession: true` + `subSessionId` 表示文本来自子会话运行过程**（subsession_run 派生会话流式回复），前端渲染进该 run 的折叠容器（见下）
// - `session_start`：子会话 run 开始（携带 runId + agents/input），前端创建折叠容器——执行中**展开并滚动到可见**；服务端**每轮重推**（同 runId 幂等，前端容器已存在则忽略），前端容器随消息重载丢失（切走会话/断线重连）后新一轮 delta 前可据此重建；子会话运行的 start 携带 `subsession`/`model`（容器标题「🌿 子会话 · 名（模型）」）
// - `session_done`：子会话 run 结束（携带 runId + agents/output），前端封存流式文本段、写入最终返回摘要并**自动折叠容器**（只显示输入与最终返回，点 summary 展开看完整过程）
// - 子会话运行过程（text/reasoning/tool_call/tool_result/approval）事件**全部携带 `subSession: true` + `subSessionId`**（与主循环同构渲染：推理折叠块、工具卡片、流式文本），并**完整落盘**（见「子会话运行存档」）——仅存档与前端回放，`loadHistory` 跳过（不进入主 LLM 上下文）；会话列表 ctxTokens 估算同样排除
// - `error`：任务失败（LLM 接口错误等），前端应渲染错误提示（与流异常中断同等对待）
// - `tool_call`/`tool_result`/`approval`：WS `event.*` 推送，SDK `sendPrompt` 订阅事件流转换为 ChatChunk 迭代（单通道，无 SSE 兑底）

interface AgentEvent {                  // WS event.* / Webhook 统一载荷
  type: string                          // event.message.delta 等
  sessionId: string
  payload: Record<string, unknown>
  timestamp: number
}
```

- 消息持久化上限（1000 条/会话，纯存储安全网，超出截断最早的非保护消息）+ LRU 会话缓存（10 个）+ 会话 env 解析 LRU 缓存（256 个，`MAX_ENV_CACHE_SESSIONS`）
- 附件引用 `AttachmentRef`：`{ path, mime, name, size }`，指向会话 `tmp/` 内文件

## 功能列表

### 总Agent全局工具

| 工具 | 功能 | 默认审批 |
|------|------|---------|
| `read` | 读取文件内容（相对路径以会话 `tmp/` 为基准，`tmp/` 前缀可省略；服务端部署受沙箱限制，桌面/本地浏览器不限制，见路径基准）；可选 `offset`（起始行号，1 起始）与 `limit`（行数，正数取 offset 起 N 行、负数取末尾 N 行），按行切片便于大文件分段阅读；**默认每行前缀真实行号**（`line_numbers` 默认 true——cat -n 风格右对齐+制表符，切片后仍对应文件行号，按 文件:行号 引用/构造补丁的定位基准；不需要可传 false；复制原文给 `edit` 的 old_string 时须去掉行号前缀——edit 检测到行号前缀误拷贝会给明确提示）；`offset`/`limit` 切片读取附尾部位置注记（`（第 X–Y 行，共 N 行）`），模型据此判断剩余内容与下一段 offset；**编码自动识别**（BOM/UTF-16 LE·BE/UTF-8/GBK 按字节探测解码，非 UTF-8 文件不再按 UTF-8 读成乱码，附编码注记；不可识别回落文本通道）；**`encoding` 指定编码解码读取**（如 `gbk`——file info 探测为非 UTF-8 时用，fatal 解码失败给明确报错；仅解码读取，转码改写用 `py`）；**UTF-8 BOM 不进输出**（底层 readFile 用 node utf8 保留 BOM、`Bun.file().text()` 会剥离，工具层统一 `stripBom` 去除——`edit`/`patch` 匹配用干净正文（BOM 会让首行 oldString 匹配失败）、写回时按原文件有无 BOM 补回，`write` 覆盖写同样保留——BOM 文件（Windows 工具生成常见）往返编辑不丢头）；读目录给出可读引导（用 `ls`/`glob`，不再抛原始 EISDIR）；**图片文件（png/jpg/jpeg/gif/webp）不以文本读取**——主模型多模态时二进制读入并内联进工具结果消息（`ToolResult.images` → 统一 image 块，模型直接可见，无需 vision），非多模态返回说明 + vision 指引；轻量引用随工具消息落盘（`Message.images`），历史重建按引用重读内联（同附件图片的最近窗口与降级规则，见「多模态支持」）；svg 为文本正常读取、bmp 不在白名单给转换引导；读取成功登记「本会话已读」（防盲写守卫依据，见「防盲写守卫」） | 否 |
| `write` | 写入文件（默认整体覆盖；`append:true` 追加模式——内容接在文件末尾，不存在则新建）。相对路径以会话 `tmp/` 为基准（`tmp/` 前缀可省略，受沙箱限制）；目标文件**已存在且本会话未 read 过**时拒绝（防盲覆盖，先 read 再覆盖，覆盖/追加同规则，见「防盲写守卫」）；**已存在但内容自上次读取/写入后被修改过**（并行分支/脚本命令/外部编辑）同样拒绝（防陈旧覆盖，重新 read 后再写）；**非 UTF-8 目标文件（GBK/UTF-16）拒绝整体写入**（write 恒按 UTF-8 落盘会破坏原编码——引导转码或改用 `edit` 按原编码写回）；**大文件（约 300 行以上）分段写入**——首段普通 write、后续段 `append:true` 续写（每段 200~300 行），防单次模型输出过长被输出上限截断或接口超时；**同一路径的写入按队列串行**（读旧值 → 守卫判定 → 计算 → 落盘 → 登记指纹整段串行）——脚本内 `Promise.all` 并行 append/写同一文件不再互相覆盖丢行（并发 append 按调用顺序全部保留；仅给写盘加锁不够：并发调用各自读到同一份旧值仍会覆盖） | 否 |
| `ls` | 列出目录内容（文件/子目录、大小） | 否 |
| `grep` | 按正则表达式在会话工作目录（`tmp/`）中递归搜索文本内容（返回 文件:行号: 匹配行——路径带 `tmp/` 前缀可直接用于文件工具，限文件大小与匹配数）；`output` 三种结果形态（`content` 逐行内容 / `files` 仅命中文件清单——宽泛摸底定位优先 / `count` 每文件命中行数）、`context` 附匹配行前后上下文（格式同 `grep -n -C`：匹配行 `文件:行号:` 前缀、上下文行 `文件-行号-` 前缀、组间 `--` 分隔；`contextBefore`/`contextAfter` 可指定**非对称**上下文（同 `-B`/`-A`——看定义后的实现体常用），指定时覆盖 `context` 对应侧）、`literal:true` 按字面匹配（正则元字符自动转义——搜索 `foo.bar(` 类代码片段免转义）、`include`/`exclude` 按路径 glob 过滤/排除（逗号分隔多模式、`{a,b}` 花括号交替；无 `/` 的模式按目录/文件名匹配任意层级）、`head_limit` 压低匹配上限先看一部分；node_modules/.git/dist 等大型目录默认跳过（include 原文显式点名除外）（见「grep 内容检索工具」） | 否 |
| `glob` | 按文件名模式（glob：`*`/`**` 跨目录、`?` 单字符、`{a,b}` 花括号交替）在会话工作目录（`tmp/`）递归查找文件（path 可限定子目录，`tmp/` 前缀可省略，与 `read`/`write` 同一路径解析规则；返回路径带 `tmp/` 前缀，可直接用于文件工具）；`exclude` 排除路径模式（与 grep 同语法）；**`**/` 可匹配零层目录**（`**/*` 同时命中根级与子目录文件——`**/test/*.js` 命中根级 `test/`，与 `core/fs/service.ts` 的 glob 语义一致；旧实现把 `**` 后的 `/` 当字面量，根级文件全漏）；node_modules/.git/dist 等大型目录默认跳过（模式显式点名除外）；**本地模式下 `path` 可传 `tmp/` 外路径并实际遍历搜索**（`walkDirFiles`：跳过大型/生成目录、深度上限 10；按给定前缀输出路径可直接用于文件工具；模式与 `exclude` 统一按**相对给定 path 的路径**判定——否则 `exclude: "src/**"` 这类含分隔符的模式对绝对路径永不命中；`path` 指向单个文件时按文件名匹配；路径不存在或无可列文件时**明确报错**，不再与「目录内无匹配」同义）；沙箱模式拒绝越界 | 否 |
| `file` | **文件管理（单工具多动作）**：`copy` 复制文件（`to` 含目标文件名，二进制通道支持任意类型、父目录自动创建，≤100MB）/ `rename` 重命名（同目录改名，`new_name` 仅名字不含路径——含分隔符拒绝防越界，跨目录用 move）/ `move` 移动或跨目录改名（`to` 含目标文件名，父目录不存在自动创建，与 `write` 一致）/ `mkdir` 新建目录（递归，已存在幂等不报错）/ `delete` 删除文件或目录（递归，不可恢复）/ `info` 查看文件信息——**按内容探测**（类似 `file` 命令，读头部 1KB）：魔数识别实际类型（图片/压缩包/Office/PDF/可执行/SQLite/Java class 等）、文本 vs 二进制判定（二进制勿盲 read）、编码检测（UTF-8/BOM/UTF-16/疑似 GBK——GBK 用 `read` 的 `encoding=gbk` 读取）、shebang 解释器；**扩展名与实际内容不符时显式提示**（`data.extMismatch`）；附人类可读大小与修改时间，目录附直接子条目数。写动作（copy/rename/move/mkdir/delete）走写范围守卫；与 `ls`（目录列表）分工——`ls` 单独保留；写动作走写范围守卫 | **否**（`delete` 动作动态需审批） |
| `edit` | **精确修改文件**：`old_string` → `new_string` 定点替换（可多处），或 `pattern`（正则）→ `new_string`——二者二选一，正则项配 `regex_flags`（`g/i/m/s/u/y`，`g` 自动补齐）与 `$&`/`$1..$9`/`$$` 捕获引用，大段原文只改少量字符时用正则省去整段重发；替换前校验匹配与**唯一性**（多处命中报错列出行号，或该项 `replace_all: true` 全部替换），失败即报错不落盘；空 `edits`、项非对象、缺 `new_string`、`old_string` 与 `pattern` 同给、`old_string` 为空或等于 `new_string` 均拒绝；目标文件已存在但本会话未 read 过时拒绝（防盲改守卫，与 write 同规则）；**编码感知**（BOM/UTF-8/UTF-16 LE·BE/GBK 自动探测，按原编码写回——GBK 仅支持命中区域与替换文本均纯 ASCII，否则明确拒绝并引导转码，不再静默写坏）；**行尾健壮**：CRLF/裸 CR 文件 × LF 原文片段在 LF 归一空间匹配（双向自适应），写回按源字符区间拼接——未修改区域字节级保留（混合行尾不被整文件改写），仅替换文本按文件主导行尾；`old_string` 误携 read 行号前缀时自动剥离；匹配失败时检测空白/缩进近似原文并给提示；成功回报各处应用行号（见「edit 修改工具」） | 否 |
| `patch` | **应用 unified diff 补丁**：一次多 hunk、行号模糊容错（上下文裁剪重试），全部 hunk 校验通过才整体落盘（原子），`dry_run` 可预演不落盘；**多文件补丁**按 `---`/`+++` 文件头分组逐文件应用（`a/`/`b/` 前缀自动剥离，跨文件原子——任一文件任一 hunk 失败整体不落盘）；目标文件已存在但未 read 过时拒绝（防盲改守卫）；**行尾感知**（与 edit 同构）：CRLF 文件 × LF 补丁在 LF 归一空间匹配、写回按原文件行尾还原（见「patch 补丁应用工具」） | 否 |
| `restart_server` | 重启本服务进程（仅本地模式注入，见「restart_server」；支持 `action=status` 只读查询与重启后续跑 `prompt`；**重启动作执行后本轮任务即结束**——结果不回灌模型，续跑经 `prompt` 接续） | **是** |
| `tool_schemas` | **批量获取工具 schema**：按工具名列表返回输入参数与结构化输出（`data`）的 JSON Schema；省略时返回全部已启用工具的输出结构概要——js 编排前理解输出结构，避免逐个试调 | 否 |
| `agent_list` | 列出可用子Agent（名称/描述/是否已装载；**不列工具名**，工具名以注册的工具集为准）。**不注册进总Agent 全局工具集**——未装载清单已由 `systemPromptInjection` 注入提示词（模型上下文已有，工具冗余且干扰工具选择）；仅在子会话运行环境注入（纯 md 组合子Agent 自动注入编排工具时，见「子Agent文件格式」） | 否 |
| `agent_load` | **装载**子Agent 能力模块（类比 import 子模块：工具并入当前工具集、**完整系统提示词作为 system 消息写入会话记录**（持久化，恢复会话自动还原），**不创建独立上下文**；默认使用方式：装载后直接用其工具，仅在需要干净上下文或防膨胀时才改用 `subsession_run` 子会话运行；装载反馈**不枚举工具清单**——`{agent}_*` 工具 schema 已注册进工具集（下一轮请求即全量下发），再列一遍是冗余） | 否 |
| `subsession_run` | **子会话运行**（无需装载，一套入口覆盖两种形态）：`inherit_context:false`（缺省）= 派生子会话执行独立子任务，`agents` 可省略/为空（不加载任何子Agent），中间过程/推理/内部工具不进父上下文、全程存档可回放，最终结果作为工具结果返回（异步经 `bg_task` 取回）；`inherit_context:true` = 从父会话当前上下文 fork，报告自动合入父上下文。**默认与父会话同构**——`inherit_global_tools` 与 `inherit_global_prompt` 默认均为 true（全局工具同名同参注册 + 父会话全局系统提示词前缀注入，子Agent 只提供独有能力；false 分别裁剪）；**`async` 参数**：true 后台异步执行——立即返回 runId（`s` 前缀）不阻塞，子会话内可 `subsession_merge` 主动合入阶段性成果（长任务先做别的，见「子会话运行」）；默认优先 `agent_load` 装载后直接用其工具 | 否 |
| `subsession_run` | **子会话运行（统一父子会话模型）**：一套入口覆盖两种形态——`inherit_context:false`（缺省）= 派生子会话执行独立子任务（预加载子Agent 可省略；最终结果作为工具结果返回或经 `bg_task` 取回）；`inherit_context:true` = 从**父会话当前上下文** fork 子会话（父消息历史 + 系统提示词 + 工具面快照），最终报告**自动合入父上下文**（合并消息 + 过程存档，父会话下轮即见）——同一任务的并行多路探索/执行，像 git 一样不停 fork/合并摆脱单轮串行的模型服务速度限制。单任务用 `input`（+可选 `agents`/`model`）、多任务并发用 `subsessions`（1-8 项，每项 `{ name?（缺省 s1..sN，批内唯一，≤32 字符不含空白、中文名合法）, input, agents?, model? }`，两形态二选一）；`model` 走**模型路由**（`GEBAI_LLM_ROUTES` 命名路由走独立端点，多路接口并行）；子会话内用 `subsession_merge`（**仅异步运行注入**）双向同步父会话（传 content 交出阶段性成果并继续运行/不传拉取父会话完整增量），父会话与兄弟子会话进展以通知注入**互相感知**（见「子会话运行」）；`merge` 可选合入粒度（缺省 `full` 全文；`summary` 摘要合入——长报告压成结论要点进父会话、全文留过程存档）；默认阻塞等全部完成（结果为概要，隔离形态附最终结果、继承形态全文在随后的合并消息），`async:true` 后台执行——立即返回 runId（`s` + 8 位 hex），`bg_task`（s 前缀）管理 | 否 |
| `bg_task` | **后台异步任务统一管理**（三类同构管理面合并，**按 id 前缀自动识别**，无需指定类型——旧 `sh_task`/`agent_task` 已合并为本工具）：**命令任务**（`sh async:true` 启动，taskId 形如 `tXXXXXXXX`，见「sh 异步后台任务」）——`status` 返回状态与 stdout+stderr 合并日志尾部（`tail` 参数默认 4000 上限 20000 字符，完整日志 `sh-tasks/{id}.log`，相对会话工作目录）；**子会话运行**（`subsession_run async:true` 启动，runId 形如 `sXXXXXXXX`，见「子会话运行」）——`status` 返回进度（已执行轮次/工具调用/最近活动，已结束含最终结果），`wait` 完成时取回最终结果与完整存档（挂执行记录扩展字段供历史回放；继承形态报告完成即自动合入父上下文，无需取回动作），`stop` 终止且该子会话不合入（已执行过程保留在存档）。公共动作：`wait` 阻塞等待完成（`timeout` 秒内未完成返回当前状态，默认/上限 60 秒——「先做别的再回头等结果」，上限 1 分钟强制按进度轮询）；`stop` 终止（命令任务杀进程树、子会话运行协作中止且已执行过程保留在存档）；`list` 三类合并列出本会话全部后台任务（按启动顺序） | 否 |
| `sh` | 执行Shell命令（**Windows 经 PowerShell、POSIX 经 `bash -c`，命令按所在平台的 shell 语法书写；退出码读返回结果的 `exitCode` 字段，无需在命令里输出**；**`workdir` 参数**：命令工作目录（相对路径基于会话工作目录/项目根解析——替代在命令里串联 `cd`；async 后台任务同以该目录为 cwd；非默认工作目录执行时输出末尾标注「（工作目录: …）」）；**`input` 参数**：stdin 输入，对象/数组自动序列化为 JSON 文本（双引号，脚本 `json.loads` 可解析）；**`timeout` 参数：执行超时秒数，默认 300、上限 540，超时按进程树终止并返回超时结果（`async:true` 时为任务生命周期上限：默认 1800、上限 3600）**；**`strict` 参数**：true 时非 0 退出码抛工具级错误（js 编排「非 0 即中断」，默认 false 非 0 退出作为正常结果返回，exitCode 在结构化输出）；**`async` 参数**：true 后台异步执行——立即返回 taskId 不阻塞（长耗时构建/测试先做其他事再回头查询，见「sh 异步后台任务」）；**`approval` 参数**：本次调用是否需审批，默认 true，明确安全的只读/幂等命令可传 false 按次免审（见「工具审批」）；可运行 `bun run`/`node`；JS/TS 亦可通过内置运行时 `gebai exec` 自执行，见「脚本执行环境」） | **是**（默认；`approval:false` 按次免审） |
| `py` | 执行Python代码（**`input` 参数同 `sh`**——本地模式工具桥下对象/数组原样注入 `input` 变量（与 js 的 `input` 语义对齐，不再经 schema 强制序列化为 JSON 文本）；**`timeout` 参数同 `sh`**；**`strict` 参数同 `sh`**；`approval` 为**兼容参数**：`code` 为任意代码、无法静态判定安全性，**免审标记不生效——恒需审批**） | **是**（恒需审批，不接受 `approval:false` 免审） |
| `js` | **执行 JS/TS 脚本（工具动态编程）**：Bun 子进程运行，脚本内工具**像内置函数一样直接调用**——`await read(params)`（已启用工具名即顶层函数，动态名字 `tools.call`）+ `ctx` **注入会话上下文**（user/sessionId/workdir/home/sandboxed/env/projects/messages 最近消息快照）+ `input`（编排传入）；console 输出即工具输出，`return` 值进 `data.result`；`timeout`/`strict`/`approval` 参数同 `sh`（见「js 脚本工具」） | **是**（默认；`approval:false` 按次免审） |
| `show` | **向用户展示内容的统一入口**（原 `draw`/`render_html`/`show_file` 三工具合并，参数面统一为 `name`/`format`/`content`/`path`，**内容与路径二选一**）：①**图表**——`content` + 图表语言 `format`（必选：`mermaid`/`plantuml`/`d2`/`echarts`，工具描述与参数说明内置选择指南——Mermaid 通用图表首选、PlantUML 标准 UML 严谨建模、D2 美观架构图/对外展示、ECharts 数据可视化/统计图表（option 的严格 JSON，值禁止函数）），渲染成功才返回成功、报错回传模型、5 秒超时判定画图能力受限；Web 前端本地渲染（配色跟随 UI 主题）/飞书桥接后端渲染 PNG/`render=backend` 服务端渲染 PNG（返回 `image` 内容块）；产物保存会话 `tmp/` 并返回 `diagram` 内容块（`format` 字段携带图表语言）；`path` 指向已有图表文件直接渲染（图表名取文件主名、format 按扩展名推断，不重发源码）；**PlantUML 布局规范内置于工具描述**（未设置间距时自动注入 `skinparam ranksep 80`/`nodesep 40` 兜底，见「内容展示」）；②**HTML 页面**——`content`（HTML 源码）+ `format: "html"` 直接在聊天界面渲染展示（沙箱 iframe 域隔离预览：脚本可执行、隔离于宿主页面；适合网页原型/数据报表/可视化组件/带交互脚本的小页面），落盘 `tmp/` 并返回 `html` 内容块，可选微调 `width`/`height` 预览尺寸；③**文件直显**——`path`（与 `content` 二选一）按**文件真实类型**产出直显内容块（显式 `format` 时按该格式解释：图表语言 → 渲染该文件、`html` → 页面预览；与 `name` 无关：图片内联、图表源文件渲染图表、`.html` 页面预览、markdown（`.md`/`.markdown`）**渲染为文档而非源码高亮**、音频/视频在文件卡内联播放（原生控件可拖进度）、其余文本/代码语法高亮内联（无扩展名文件如 `LICENSE`/`Makefile`/`Dockerfile`/`.gitignore` 按内容探测后同样内联；超 4 万字符截断 + 文件卡片取全文）、其余类型（PDF/压缩包/Office）查看/下载卡片）；会话 `tmp/` 内直接引用，会话外文件复制 `tmp/shown/{主名}-{内容哈希}.{扩展名}`（≤100MB）后引用（见「内容展示」）；`card.args="block"` 声明（调用不显示通用卡片，结果直出内容块）；**分支门控**（html 分支仅实时通道、图表分支无交互模式直接引导 `render=backend`、path 分支全模式可用，见「内容展示」） | 否 |
| `fetch_url` | 抓取 URL 内容（网页/API/文档；服务端部署模式限制公网地址防 SSRF 并逐跳校验重定向，响应超阈值截断） | 否 |
| `vision` | **视觉分析**：把图片文件交给多模态（视觉）模型分析（参数 `target` 目标 + `image` 图片文件路径；模型选择 `GEBAI_VISION_*` 额外视觉模型，未配置回落到声明多模态能力的主模型；base64 内联传输，单图 8MB 上限），返回分析文本并携带 `image` 内容块 | 否 |
| `todo` | 待办管理（统一入口）：`entries` 操作列表，每项 `op=add/update/delete`——add 新建（需 title，可带 priority/note/eta），update 按 id 改 status/progress/title，delete 按 id 删除；省略/空数组 = 查询；返回操作摘要与当前全部待办状态 | 否 |
| `ask` | **向用户询问并阻塞等待回应的统一入口**（原 ask_user/ask_env/plan 三工具合并，分支按专属参数三选一）：①选项询问——`prompt`+`options`（multi 多选、复杂选项 title+description、自定义文本/拒绝均可），用户点选后结果返回模型据此继续，见「用户询问」；②环境变量填值——`name`+`description`+`secret`，前端弹窗填值后注入本次任务 env 并保存浏览器本地（用户拒绝/超时返回失败），见「环境变量」；③计划审批——`title`+`steps`/`content`，计划落盘 `plans/{标题}.md`（相对会话工作目录），`plan` 载荷随事件内嵌进选择卡（审批时直接可见；等待期消息流不重复预览计划卡，决策后落计划卡呈现结果态），阻塞等待批准/拒绝（拒绝可附修改意见），`data` 返回 `{status,title,path,feedback}`；**分支门控**：填值分支仅实时通道、选择/计划分支无交互模式报「无交互能力」（见「用户询问」）；`card.args="none"`（问答记录卡/计划卡/填值卡各有专属 UI） | 否 |

> **`current_time` 已移除**：时间获取经 `sh`/`py`/`js` 脚本按需完成（如 `sh` 执行 `date`），不占全局工具位；不注入时间相关提示词引导——模型自行选用现有工具。

> **全局工具集最小化**：编码工作流专属工具不注册为全局工具——`git`（只读变更核对，`code_git`/`explore_git` 命名空间暴露）与 `preview_server`/`env_detect`/`system_info`（验证服务与环境/工具链探测，`code_preview_server`/`code_env_detect`/`code_system_info`）由 `code`/`explore` 子Agent 按需暴露（`self_optimize` 经连带装载 code 一并获得）；反馈读取（`read_feedback`）下沉 `self_optimize`（`self_optimize_read_feedback`，自我优化专属输入通道）；定时任务（原全局 `cron_add`/`cron_list`/`cron_update`/`cron_remove`）下沉 `cron` 子Agent（命名空间不变并补 `cron_trigger` 手动触发，见「cron 子Agent」）；`delete_file`/`move_file` 合并为全局 `file` 多动作工具——全局集只保留高频基线与编排/交互入口，低频域工具下沉子Agent。

> **路径基准（统一）**：文件工具（`read`/`write`/`edit`/`patch`/`ls`/`grep`/`glob`/`file`/`show`/`vision` 等）的相对路径统一以**会话 `tmp/` 子目录**为基准——与 `sh`/`py`/`js` 子进程工作目录（cwd）、`glob`/`grep` 搜索范围、UI 文件面板范围完全一致，跨工具传递路径无需考虑前缀（`write("a.txt")` 产物 `sh` 直接可见、`glob`/`grep` 直接命中）；**js 脚本内裸 fs/`Bun.write` 的相对路径同样落在会话 `tmp/`——勿再带 `tmp/` 前缀**（会多套一层写入 `tmp/tmp/…`），`tmp/` 前缀别名仅工具参数层剥离（js 工具描述内置此说明）。带 `tmp/` 前缀的逻辑路径（列表/附件/截断产物契约，如 `tmp/a.txt`）剥离前缀后解析，两种写法等价（`stripTmpPrefix`）；`glob`/`grep` 的 `path`/`include` 过滤同样兼容两种写法（列表坐标双候选匹配）。沙箱模式限定会话 `tmp/` 内（桌面/本地浏览器放开，绝对路径直用）。

> **项目路由（`project` 参数）**：全局文件工具（read/write/edit/patch/ls/grep/glob/file/sh/py）与编码类子Agent 独有工具统一携带可选 `project` 参数——传**预置项目名**（`{AGENT}_PROJECTS` 清单项）或**项目根路径**（自由项目，绝对/相对均可）时，路径参数相对所选项目根解析（沙箱模式限定该根内），`sh`/`py`/`git`/`preview_server` 同时以该根为工作目录；保留名 `tmp` = 会话工作区。未传时相对路径以会话工作目录为基准（上文路径基准语义）。实现：`core/tools/projects.ts` 的 `projectAware` 在全局注册处统一包装（见「项目机制」与 code 章节）。

### `edit` 修改工具

在 `write`（整体写入）之外提供**精确修改**能力，适合代码/配置文件的小步修改：

- **参数**：`path`（目标文件）+ `edits: { old_string | pattern, new_string, regex_flags?, replace_all? }[]`（可一次多处替换；`replace_all` 缺省 false）；每项 `old_string` 与 `pattern` **二选一**，`new_string` 必给（删除内容传空串）
- **正则匹配（`pattern`）**：`old_string` 的替代通道——大段原文只改少量字符时用正则定位，省去整段重发；`regex_flags` 支持 `g/i/m/s/u/y`（`g` 自动补齐），`new_string` 支持 `$&` 整段匹配、`$1..$9` 捕获组（未参与匹配为空串）、`$$` 字面 `$`；匹配在 **LF 归一空间**进行（CRLF 文件同样可用）；**匹配在独立子进程执行**（灾难性回溯防护，20 秒超时强杀，与 `grep` 同机制）；单次匹配上限 1000 处（超限拒绝——无法安全判定替换范围）；零长度匹配（`^`/`a*` 类）明确拒绝
- **唯一性校验（防误改）**：默认要求匹配**唯一命中**——多处命中时整体失败不落盘，错误信息列出各命中行号（至多 8 处）并给出两条出路：扩大匹配范围使其唯一，或确认全部替换时该项传 `replace_all: true`；避免「相同片段出现在多处时改错第一处」的静默误改；**区间重叠**（同一项内匹配交错或与前项替换区域交叠）整体失败——无法确定替换结果
- **安全校验**：替换前校验匹配成立，不匹配则整体失败并报错，**不落盘**，避免模型基于过期内容误改；参数前置校验：空 `edits`、项非对象、`old_string` 与 `pattern` 同给或同缺、缺 `new_string`、`replace_all` 非布尔、`old_string` 等于 `new_string`（无改动）均作为工具结果返回拒绝（不抛错）
- **编码感知（字节级）**：目标文件按**字节探测编码**（BOM / UTF-16 LE·BE / UTF-8 / GBK）后解码匹配、按**原编码写回**（BOM 与字节序保持）——非 UTF-8 文件不再被按 UTF-8 误读误写（`core/base/file-text.ts`）；**GBK 无编码表**（`TextEncoder` 只支持 UTF 系）：仅支持「命中区域与替换文本均纯 ASCII」的字节级替换，非 ASCII 场景明确拒绝并引导先转码为 UTF-8（含 GB18030 四字节字符的文件直接拒绝）；不可识别为文本（二进制/未知编码）明确拒绝；`write` 对非 UTF-8 目标文件同样拒绝整体写入（引导转码或改用 `edit`）
- **行尾健壮（区间映射）**：匹配在 **LF 归一空间**进行（CRLF 与裸 CR 都归一，模型给的 LF/CRLF 片段双向自适应），写回按**源字符区间拼接**——未修改区域取自原始字节（混合行尾文件不被整文件改写），仅替换文本按文件**主导行尾**落地；与旧版「整体还原行尾」相比，混合行尾与裸 CR 文件不再被静默改写
- **防盲改守卫（已读前置 + 陈旧拦截）**：目标文件已存在但本会话未 read 过时拒绝（与 `write` 防误覆盖同规则，见「防盲写守卫」）——防模型凭记忆/假设内容构造 `old_string` 盲改（匹配失败白跑一轮往返）；内容自上次读取/写入后被修改过同样拒绝（防陈旧改——patch 行号模糊容错可对漂移内容误命中错位应用）；`read`/`edit`/`patch`/`write` 成功过的文件视为已读；指纹按解码文本登记（与 `read` 口径一致，非 UTF-8 文件同样可比）
- **空白容错匹配（自动对齐）与失配诊断**：精确匹配失败后追加**字符级空白容错匹配**——去掉全部空白后的字符序列转正则（字符间 \s*，每字符转义）在 LF 归一空间查找，**唯一命中即采用**（输出注记「空白容错命中」；多打/漏打空格、缩进不一致、行尾差异、CJK 文本内空白差异均双向容错；多处命中不盲替换仍报错）；仍失配时报**首个分歧点**（长行中非空白字符差异的定位上下文，如「分歧起点约在『…也消费 ◀ vision 边车…』」）；行号前缀误拷贝（read 默认行号输出）**自动剥离**后重新匹配（`old_string` 携带 read 默认行号输出的「行号→制表符」前缀）：**自动剥离**后重新匹配，能命中即直接按修正后的原文替换（少一次往返），仍不命中才报错——read 默认带行号后的配套防呆
- **应用行号回报**：成功后输出每处修改的应用行号（如 `已对 a.ts 应用 2 处修改：1) 行 12；2) 行 40`），便于汇报与后续引用；内容无变化时不写盘并注记
- **与 `write` 的关系**：`edit` 用于既有文件的定点修改（保留无关内容）；`write` 用于新建/整体覆盖（受防误覆盖守卫约束）；模型按需选择（js 编排中可混用）；改动较多或行号容易偏移时优先用 `patch` 应用 unified diff（见「patch 补丁应用工具」）
- **审批**：默认无需审批（与 `write` 同级）
- **路径限制**：与 `read`/`write` 同一路径基准与沙箱（相对路径基于会话 `tmp/`；服务端部署限其内，桌面/本地浏览器不限制）

### `grep` 内容检索工具

按正则表达式递归搜索文本内容（`core/tools/fs.ts`），是编码探索的主定位工具。**双引擎**：内置 ripgrep（随包分发、无需系统安装，见「内置 ripgrep」）优先，rg 不可用或其正则语法不支持该 pattern 时自动回退内置遍历引擎——两引擎在非截断场景下结果逐字段一致（有专门的一致性测试守约），`data.engine` 如实反映本次实际所用引擎：

- **参数**：`pattern`（正则，或 `literal:true` 字面匹配）+ 可选 `path`（搜索起点：目录（递归）或单个文件（直接内搜该文件，grep 传文件语义）；默认 `.`，显式传 `.` 与省略等价（经 `resolvePath` 归一化，与 `glob` 同一惯例），相对会话工作目录、`tmp/` 前缀可省略）/`ignore_case`/`output`/`context`/`include`/`exclude`/`head_limit`；返回路径带 `tmp/` 前缀（可直接用于 `read` 等文件工具）
- **literal 固定字符串模式**：`literal:true` 时 `pattern` 按字面字符串匹配（正则元字符自动转义）——搜索含 `.`/`(`/`[` 等字符的代码片段（如 `foo.bar(`）免转义、防「点号通配造成的意外命中」；默认 false 正则模式
- **文件过滤（`include`）与排除（`exclude`）**：按路径 glob 过滤/排除（`*.ts`、`src/**`、`tests/**,*.{json,md}`）——逗号分隔多模式 + `{a,b}` 花括号交替（花括号内的逗号是交替项不分割）；无 `/` 的模式（如 `dist`、`*.log`）按目录/文件名匹配**任意层级**路径段（同 `rg --glob` 语义），含 `/` 的模式按整条相对路径匹配；`exclude` 命中即剔除（优先于 include）
- **默认跳过大型/生成目录**：node_modules/.git/dist/build/.next/.cache/__pycache__/.venv/venv/target/.idea/.vscode/coverage/.turbo（`WALK_SKIP_DIRS` 同源清单）默认不进结果——会话 `tmp/` 列表场景的防噪兜底（项目根遍历 `walkDirFiles` 本就跳过）；include 原文显式点名该目录（如 `node_modules/**`）时不排除（确需搜依赖时的显式通道）；单文件内搜（path 精确命中）不受限
- **搜索范围**：默认会话 `tmp/` 子树（与文件面板一致）；`path` 指向 `tmp/`/项目根**外**时——本地模式（桌面/本地浏览器）与 `read` 同款自由度，按给定 path 前缀**实际遍历**目标搜索（`walkDirFiles`：跳过 node_modules/.git/dist 等大型/生成目录、深度上限 10，**并发 stat 且必须同时取出 size 与 mtime**——`FileEntry.modifiedAt` 恒为 0 会让「按修改时间排序/显示」类工具静默失效（曾出现：两份实现各自都只取了 size，列表全显 1970），故该函数在 `@gebai/sdk/node` 的 `walk.ts` 为**单一来源**、server 侧仅重导出，与 code/explore 项目遍历同一实现），结果路径带给定前缀可直接用于 `read`；沙箱部署模式仍拒绝越界（无匹配）；路径不存在**明确报错**（不再静默「无匹配文件」）
- **二进制内容跳过**：文件内容含 NUL 字节视为二进制跳过（同 grep 二进制检测语义），防乱码匹配行刷进上下文；**不读 `.gitignore`**——忽略规则仅内置目录黑名单（node_modules/dist/.git/target 等），被 git 忽略的文件（如 `*.log`）仍会命中
- **三种结果形态（`output`）**：
  - `content`（默认）：逐行匹配内容，`文件:行号: 匹配行` 格式
  - `files`：仅命中文件清单（不刷内容）——**宽泛摸底定位优先用**（「哪些文件涉及 X」先看文件面，再对重点文件精读，避免大量匹配行灌进上下文）
  - `count`：每文件命中行数（按命中数降序）——快速评估命中面大小、决定深入策略
- **上下文行（`context`，0-10，仅 content 模式）**：匹配行前后各附 N 行，格式同 `grep -n -C`——匹配行前缀 `文件:行号:`、上下文行前缀 `文件-行号-`、不相邻组之间 `--` 分隔；重叠区间自动合并，一次调用即可看清命中语境（免二次 `read`）；**非对称上下文**：`context_before`/`context_after`（0-10，同 `-B`/`-A`）独立指定前后行数（指定时覆盖 `context` 对应侧）——「只看定义后的实现体」场景不必为看后文付出前文噪音
- **上限**：单文件 1MB、全局匹配默认 200 处（三种形态同一口径，达上限附「结果可能不完整」提示；`head_limit` 参数可压低先看一部分——`data.truncated` 标记截断）；输出超长走统一截断保护；**截断时保留哪一条取决于扫描顺序**（两引擎均提前收工，属早停的固有代价，详见「内置 ripgrep」）
- **灾难性回溯防护（内置引擎侧，双通道）**：分界不是「快慢」而是**是否可能存在回溯组合爆炸**——正则路径在**独立子进程**执行，literal（字面量）路径走**进程内匹配**。
  - **为何正则必须隔离**：正则 `test` 是**同步 CPU 操作且不可中断**——一旦进入，连 `setTimeout` 超时回调都要排在后面执行，期间该进程**全部会话**的 WS/HTTP 冻结（「全部会话冻结」的实质）。而真实工作量是「每文件 × 每行」各调一次（数千次），**单次调用的引擎级缓解约束不了整体**：本项目仅部署 Bun（JSC），其回溯预算实测随形态而异——`(a+)+b` ~0.78s、`(a*)*b` ~2.1s、`(a|a)+b` 在长输入达 3.5s（亦有 14ms 级形态，差两个数量级）；**即使单次，2.1s 的同步阻塞对一个多会话服务已不可接受**。累积效应更决定性：实测 **20 次调用即 >10s**（强杀），按 ~0.78s/次外推 1000 行 ≈ 13 分钟、5000 行 ≈ 66 分钟。且该预算属**引擎内部实现**（非文档化契约），把安全性建立在它上面很脆（引擎升级可能改变）。——故隔离到子进程，把「无界挂死」变成「20s 内有界失败 + 引导」。行数据按 4MB 批量经 stdin 送子进程、命中行号回父进程渲染，20 秒超时强杀（Unix 杀进程组/Windows `taskkill /T`）；超时返回「简化 pattern/缩小范围」引导而非回退进程内匹配（回退即重新暴露挂死面）。
  - **为何 literal 可进程内**：回溯的本质是**多条可能路径的指数组合**；转义后的字面量不含量词/交替，**只有一条匹配路径**，不存在组合爆炸——故可安全进程内执行，省掉「内容序列化 + 跨进程搬运 + 反序列化」（实测 108MB 量级省约 1.6s）。
  - 两条通道**读取均按块并发**（限额 16、文件顺序不变）。rg 引擎的 Rust 正则**线性时间**（有限自动机模拟，设计上不存在指数回溯；实测同 pattern 恒为 18–20ms），天然不需要该防护（仍设 20 秒上限防病态 I/O）。
- **结构化输出**：`data = { mode, matches, files, counts, truncated?, engine }`——**三键齐备**（不论 output 选哪种模式，matches（行级）/files（文件清单）/counts（每文件命中数，按命中数降序）同时给出，`mode` 标明本次主形态；无匹配时三键均为空数组），供 js 编排引用——**不要按 mode 猜键名**（曾在校对中因按 `data.matches` 读 files 模式而误判「代码中不存在」）；`engine` 为本次实际使用的引擎（`rg`/`builtin`），用于可观测与回退排查
- **审批**：默认无需审批（纯读取）

### 内置 ripgrep（`rg`）与双引擎选择

**问题**：grep 旧实现把过滤后的**每个候选文件全文读进内存**再整批送子进程正则匹配——gebai 仓库根目录搜索实测读入 2033 文件 / 107.7MB、**读全文 16 秒**（加匹配与序列化共 20 秒+）；同一查询 `rg` 仅需 79ms。慢的不是过滤（遍历 161ms）也不是匹配，而是**全量内容 I/O**。

**解析链**（`core/support/ripgrep.ts`，首个可用者胜出；结果缓存）：

1. `GEBAI_RG_PATH` —— 显式覆盖（运维/内网自备/调试；指向不存在即视为不可用，不静默改用其他来源）
2. `{GEBAI_HOME}/vendor/ripgrep/<platform>-<arch>/rg[.exe]` —— **二进制（`bun --compile`）形态**：运行时物化点，内容来自构建期内嵌产物（`rg.embedded.generated.json`，gzip base64）+ 物化（与 d2js / playwright driver / CV 模型同一套「内嵌 + 物化」闭环，POSIX 补 0755）；单二进制形态在用户机器上既无 node_modules 也不保证装了 rg，故随产物内嵌。**源码/dev 形态不走此路**（无需物化，直接用下面两条）
3. node_modules 的 `@vscode/ripgrep`（`optionalDependencies`，**拉不到不阻断 `bun install`**）—— 实现上问主包要它自己导出的 `rgPath`（不自拼路径：1.18+ 二进制在平台子包 `@vscode/ripgrep-<platform>-<arch>/bin/rg[.exe]`、≤1.15.x 在主包 `bin/` 下，跨包管理器布局由包自身保证——自拼平台包路径在 bun workspace 下解析不到）。现版此包**经 npm registry 分发**（平台子包 + optionalDependencies，无 postinstall 外网下载），内网有 npm 镜像即可；历史版本（≤1.15.x）才是 postinstall 联网下载那套
4. 系统 `PATH` 上的 `rg`
5. 全部缺失 → 内置遍历引擎（**功能不降级、只降速**，`data.engine=builtin`）

**不依赖 `resources/` 资源子仓库**：刻意**不存第二份二进制副本**——来源收敛为「npm 包」与「系统」两条，npm 包已覆盖跨平台分发（12 平台子包）与版本管理，自存副本徒增仓库体积与失同步风险。

**内嵌产物供给**（`packages/server/scripts/build-rg-embed.ts`，已并入 `build`/`typecheck` 链路）：取 rg 本体的顺序与运行时解析链一致——`GEBAI_RG_PATH` → node_modules 的 `@vscode/ripgrep` → 系统 `PATH`；**不落盘任何资源文件、不从网络下载**（只把取到的二进制 gzip 内嵌进生成产物）；取不到时只生成空清单、**不阻断构建**（运行时给配置指引：`bun install` 拉 optional 依赖，或构建机装 rg）。

**双引擎对齐（结果一致性）**——否则两引擎会静默漂移，以下几处是刻意取舍：

- `--hidden --no-ignore --no-ignore-vcs/-parent/-dot/-global`：内置遍历只跳过 `WALK_SKIP_DIRS`，既不读 `.gitignore` 也不跳过隐藏文件；rg 默认两者都做，须显式关闭才能对齐（`GEBAI_GREP_RESPECT_GITIGNORE` 为预留的可选开关）
- `--max-count <head_limit+1>`：多读一条才判定「结果不完整」，与内置匹配器同口径（**超限的那一条不进入结果**）
- `--max-filesize`：与内置的候选文件大小上限（1MB）一致；二进制文件由 rg 与内置引擎各自按 NUL 字节跳过，行为等价
- 行切分对齐：以换行结尾的文件**不产出末尾幻影空行**（内置引擎原先按 `split("\n")` 会多出一行，导致 `-A` 上下文多渲染一行空行、`^$` 类 pattern 多一处命中——本次一并修正）

**回填与过滤分层**：rg 直接扫真实文件系统，结果按「绝对路径 → 显示路径」allowlist 精确回填——过滤语义（`ctx.listFiles()` 坐标 + include/exclude + 默认跳过目录 + 单文件内搜）**仍由过滤层决定**，rg 视野 ⊇ 过滤结果，回填只做收敛，不会漏命中。候选非空而 rg 一个文件都没扫到（`summary.stats.searches === 0`）时判定「列表与磁盘不一致」（虚拟/陈旧清单），回退内置引擎而**非静默报无匹配**。

**回退与可观测**：`GEBAI_GREP_ENGINE=auto`（默认）/`rg`（强制，不可用则**明确报错**，便于部署期发现内置缺失）/`builtin`（逃生口与性能回归排查）；运行期失败（Rust 正则不支持后向引用/环视、超时、被中断）静默回退内置引擎，`data.engine` 给出实际所用引擎。

**顺带修掉的三处（同源性能/健壮性问题）**：① `core/fs/service.ts` 的 `/api/v1/fs/search` 原用 `Bun.spawnSync`（**同步阻塞整个服务事件循环**——搜索期间全部会话一起卡），改异步 spawn 并共用同一解析器（并去掉重复的 `--max-count 50 -m 50`）；该路径**保留** rg 默认的 `.gitignore` 规则（面向仓库浏览，与 grep 工具的对齐取舍不同，属有意差异）。② `walkDirFiles` 逐文件串行 `stat` → 并发（上限 32），遍历顺序不变。③ grep 匹配子进程 runner 的落盘判定原仅凭内存缓存——runner 位于系统临时目录，被外部清理（tmp 清理任务等）后会反复启动子进程失败，**且失败现象是「超时」误导**；改为带存在性复核、缺失即重写。

**内置引擎（回退路径）自身的两项优化**（2026-09 补充：rg 之外的兜底不应停在秒级）：读取由逐文件串行 await 改为**按块并发**（限额 16、顺序不变）、literal 改**进程内匹配**。实测仓库根 2107 文件 / 108MB（热缓存，含遍历+读+匹配+渲染）：

| 路径 | 改造前 | 改造后 | rg |
| --- | --- | --- | --- |
| 内置 literal（进程内） | 4535ms | **253ms** | 146–184ms |
| 内置正则（子进程隔离） | 4535ms | **3078ms** | 146–184ms |

**为何不把内置正则也改进程内**：正是「灾难性回溯防护」要解决的问题——进程内匹配遇病态正则会长时间**同步阻塞**事件循环（JSC 的单次回溯预算只约束单次调用，不约束「文件×行」整体）且不可中断。若将来要再提速，可行方向是「子进程只传路径、自读自匹配」（实测 1293ms）——代价是放弃 `ctx.readFile` 抽象（编码探测/虚拟文件/测试桩），**不划算而未采用**。

### 防盲写守卫（已读追踪 + 内容指纹）

防止模型对未见过的文件内容盲写（参照 ZCode 的「未读不许覆盖/修改」设计），覆盖 `write`（整体覆盖/追加）、`edit`（定点替换）与 `patch`（补丁应用）三个写通道，规则一致：

- **守卫规则（两档）**：①**防盲写**——目标文件**已存在且本会话未读取过**时拒绝写入，返回引导信息（先 `read` 掌握现有内容再改——`write` 覆盖确认整体内容、`edit`/`patch` 基于当前原文构造修改）；②**防陈旧覆盖**——文件**读过但内容自本会话上次读取/写入登记后已被修改**（并行子会话、父会话任务、`sh`/`py` 脚本命令、外部编辑均会造成漂移）时同样拒绝，引导重新 `read` 最新内容后再写——防基于旧认知静默覆盖他人改动；**新建文件不受限**；`append:true` 追加模式同规则（追加不破坏已有内容，但同样要求先读且不陈旧——防对未见/已变内容盲目拼接；截断抢救落盘亦受此守卫约束）
- **edit/patch 纳入同一守卫（防盲改/防陈旧改）**：两者虽天然带原文校验（幻觉内容匹配不上即失败），但「凭记忆构造 oldString/补丁 → 匹配失败白跑一轮往返」本身就是高频失败模式——未读前置把失败提前到写入口（一次往返引导 read，与匹配失败等价成本但引导更明确），且杜绝「碰巧匹配成功但模型并未见过全文」的盲改；陈旧拦截对 `patch` 尤其重要——**行号模糊容错可对漂移内容误命中错位应用**；`edit`/`patch`/`write` 成功即刷新登记，迭代修改自身产物不受阻
- **已读登记（fileGuard）**：引擎维护会话级已读表（`AgentEngine.readFiles`，sessionId → 已读绝对路径 → 读取/写入时内容指纹），经 `ToolContext.fileGuard`（`markRead(path, content?)`/`hasRead`/`staleSinceRead(path, current)`）注入工具上下文；`read`/`edit`/`patch`/`write` 成功后自动登记（含指纹）——修改过的文件视为已掌握内容，后续写入放行；指纹为**解码文本**（去 BOM，按探测编码解码——GBK/UTF-16 文件同样可比）sha256 前 16 位（BOM 无关——read 登记去 BOM 正文、write 比对含 BOM 原文，边界归一后一致；edit 写回登记按落盘字节的解码文本，行尾归一在守卫之后，不会误判陈旧）
- **子会话 fork 快照隔离**：子会话 fork 时同步拷贝会话级已读表（`runSubSession` 组装 fork 时快照）——子会话的已读基线 = fork 时父会话可见内容；此后父会话/兄弟子会话的读写互不串扰：**fork 后他人读过的文件不视为本子会话已读**（防跨执行流盲写代理），**他人写入造成的指纹漂移在本子会话写时被拦截**（防陈旧覆盖）——与 fork 上下文快照哲学一致（见「子会话运行」）。主循环与隔离子会话沿用会话级共享表（既有语义：隔离子会话虽不继承历史，但其读写在同一会话工作区，与父会话互通登记）
- **生命周期**：登记按会话隔离；REST/WS 删除会话时经 `engine.forgetSession` 释放；单会话登记上限 2000 条（`READ_TRACK_CAP`，超出整表重置——守卫降级为「需重读」，保护语义不破坏）；进程重启后登记为空（重新 read 即可，文件可能已变化，重读本身即正确行为）
- **兼容性**：`fileGuard` 为可选注入——测试桩/无引擎环境不注入时守卫自动放行（行为与旧版一致）；js 编排内的工具调用走同一 ctx，守卫同样生效
- **设计动机**：`write` 整体覆盖**没有任何内容校验**——未读守卫补齐「从未见过」盲区，指纹守卫补齐「见过但已过期」盲区（并发写的最小版本仲裁：写入时点比对，早于合入时点）；拒绝结果作为正常工具输出返回（模型下一轮自行 `read` 后重写，一次往返自愈，不中断任务）

### `diff` 内容块（仅历史会话回放）

`diff` 文本对比工具已移除——对比两段文本改用 `sh` 的 `diff` 命令（需审批）或两次 `read`；`core/base/diff.ts` 仅保留 `splitLines`（patch 解析用）与 `inferLang`（read/show 语言推断用）两个纯函数。`diff` 内容块的类型定义与前端并排渲染管线**保留**，仅用于回放历史会话中已持久化的块（见「内容块」）。

### `patch` 补丁应用工具

在 `edit`（定点替换）之外提供**整体补丁应用**能力，适合改动较多、行号容易偏移的代码修改：

- **参数**：`patch`（unified diff 文本）+ `path`（目标文件——单文件补丁定位用，多文件补丁按文件头定位时可省略）+ 可选 `dry_run`（仅预演校验，不写入）
- **多文件补丁**：含多组 `---`/`+++` 文件头的补丁按各文件头分组**逐文件应用**（`a/`/`b/` git 前缀自动剥离，`/dev/null` 旧侧即新建）；单文件补丁文件头可省略、以 `path` 参数定位（传了 `path` 时优先 `path`，兼容既有形态）；无头段落回退 `path` 参数，两者皆缺报错；同一文件多段按序累积应用；**跨文件原子**——全部文件全部 hunk 校验通过才整体落盘，任一失败整体不修改
- **解析（`parsePatch`，纯函数实现于 `core/patch.ts`）**：支持 `---`/`+++` 文件头（可省略）、`@@ -l,c +l,c @@` hunk 头（容错省略 count 的形式）、上下文/新增/删除行、`\ No newline` 标记与 CRLF；git 风格元数据行（`diff --git`/`index`/`mode` 等）容忍跳过；hunk 内的**空行按空上下文行处理**（unified diff 规范写作单个空格，构造补丁时常写成真空行）、**hunk 尾部空上下文行修剪**（补丁排版空行不代表文件内容）
- **匹配（`applyPatch`）**：hunk 的「上下文 + 删除」块在文件中**分档匹配**——精确 → 忽略行尾空白 → 忽略首尾空白 → 头尾上下文裁剪（各至多 `PATCH_FUZZ_LINES`，含删除行时删除行必留在块内、无删除行时至少留 1 行上下文）→ **仅删除行锚定**（上下文抄错而删除行准确的救场档）；顺序为「先少裁剪、先严比较」（多一行上下文参与校验即少一分误配）；每档收集**全部候选位置**（上限 `PATCH_MAX_CANDIDATES`），再按 `@@` 声明行号（叠加先前 hunk 行数偏移）就近选优——多候选且行号缺失/并列/最近者也超出 `PATCH_ANCHOR_TOLERANCE`（行号不可信）时按**歧义报错**（列出候选行号），绝不任选一处（防静默改错位置）
- **应用**：统一走「块定位 + 块内按补丁顺序重放」——上下文行沿用文件原有行（宽松匹配下不把补丁里的写法差异写回文件）、删除行跳过、新增行原位插入；**纯新增 hunk 同一条路径**（新增行夹在上下文中间也正确落位），无任何上下文/删除行时才按 `@@` 行号插入（声明行号超出文件行数时报错，不静默改写位置）；用到宽松档（空白差异/上下文裁剪/仅删除行定位）时在结果里点名，提醒复核落位
- **失败诊断**：未匹配时不只报「未匹配」，而是给出失败 hunk 序号、`@@` 声明行号、块内关键行，以及文件中**最相近的 3 处位置**（行号 + 真实内容 + 差异说明：内容一致仅空白差异 / 最相近）与建议 `read` 区间——据此修正补丁比反复重发有效
- **原子性**：全部 hunk 校验通过才一次性写盘（与 `edit` 同语义——任一 hunk 不匹配整体失败不修改，返回失败 hunk 序号、原因与相近位置诊断）
- **防盲改守卫**：目标文件已存在但本会话未 read 过时拒绝（与 `write`/`edit` 同规则，见「防盲写守卫」）——多文件补丁对**每个**已存在目标逐一校验
- **新建文件**：`--- /dev/null` 头或目标文件不存在时按新建处理（仅允许新增行）；删除类补丁（全部删除行）自然清空文件
- **与 `edit` 的关系**：改动多/跨多处/行号易偏移时 `patch` 一次提交全部改动（可跨多个相关文件；`dry_run=true` 预演）；小范围定点改动用 `edit`；`write` 仅用于新建/整体覆盖
- **上限**：单文件 hunk 数 ≤ 100（`PATCH_MAX_HUNKS`）、目标文件 ≤ 5MB（`PATCH_MAX_FILE_BYTES`，超出提示改用 `edit` 分段）
- **审批**：默认无需审批（与 `edit` 同级，受防盲写守卫约束）
- **路径限制**：与 `read`/`write` 同一路径沙箱

### `git` 版本控制工具（只读）

将编码工作流收尾环节的 git 只读操作从 `sh`（每次需审批）独立出来，免审批直接查看变更。**不注册为全局工具**（全局集最小化，见功能列表注记）——经 `code`/`explore` 子Agent 以 `code_git`/`explore_git` 命名空间暴露（`self_optimize` 经连带装载 code 获得；写操作 add/commit 等在任何命名空间下仍走 `sh` 需审批）：

- **参数**：`action`（`status` 工作区状态 / `diff` 变更内容 / `log` 最近提交 / `show` 查看某提交或文件的完整内容（`ref` 默认 HEAD）/ `branch` 本地与远程分支列表（附各子会话最新提交）/ `ls-files` 已跟踪文件清单（**自动尊重 `.gitignore`**，项目结构摸底快于 glob；`path` 参数可按前缀/glob 过滤）/ `grep` 已跟踪文件内容搜索）+ 可选 `dir`（仓库目录，默认会话工作目录，经路径沙箱约束）+ `staged`（diff 是否查看暂存区）+ `max_entries`（log 条数，默认 10、上限 50）+ `ref`（**diff/log/show 通用**：提交哈希/分支/tag/HEAD~n/范围（`main..dev`）等，拼为 `git diff <ref>`/`git log <ref>`/`git show <ref>`）+ `path`（**pathspec 通用**：diff/log/show 限定该路径的变更（`-- <path>`）、ls-files 前缀/glob 过滤、grep 搜索范围限定）——`ref`+`path` 可组合（如 `diff ref=HEAD~2 path=src/` 只看某次提交中某目录的变更、`log ref=main..dev path=packages/` 看分支间某包的提交历史、`show ref=abc123 path=src/a.ts` 看某提交中某文件的变更）+ `pattern`（grep 搜索模式，扩展正则 ERE——`|` 为交替）
- **执行**：命令参数由 action 枚举构造（status/branch 无用户字符串拼接）；携带用户字符串的参数（`ref`/`path`/`pattern`）——`ref`/`path` 经元字符黑名单校验（拒绝 `"`/`&`/`|`/`<`/`>`/`^`/`%`/反引号等 cmd/shell 注入形态）后引号包裹拼入，pathspec 以 `--` 分隔防被解读为选项；`grep` 的 `pattern` 用**放宽黑名单**（仅拒 `"`/`%`/`$`/反引号——引号内活动元字符；`&`/`|`/`<`/`>` 在双引号内为字面量**放行**，正则交替 `a|b` 语法需要）并以 `-E`（扩展正则，`|` 交替生效）+ `-e` 定界模式；以 `dir` 为 cwd 执行对应 git 命令；非零退出码附 stderr（非仓库目录等可诊断信息）；输出走统一截断保护，`ls-files` 附结构化 `data.files`
- **只读安全边界**：不提供任何写操作（`add`/`commit`/`checkout` 等仍走 `sh`，需审批且受沙箱约束）——免审批仅限读操作，写操作权限不变
- **审批**：默认无需审批；经 `code`/`explore` 子Agent 的 `projectAware` 包装暴露（`code_git`/`explore_git`，以项目根为工作目录，预置项目形态按项目根执行；`self_optimize` 经连带装载 code 获得）

### `js` 脚本工具（工具动态编程）

**数据流编排的唯一内建方式**（原 `flow` 声明式管道已移除——上下文占用大、使用门槛高，js 完整语言能力可覆盖其全部场景且更直观）：可预判的固定流程与高阶编排逻辑（动态构造参数、按中间结果分支重试、复杂聚合变换、正则/字符串处理、错误分类处理）统一用 `js` 一次编程执行——脚本运行于 Bun 子进程（隔离、可超时终止），进程内注入**工具调用桥**与**会话上下文**，把工具当作函数做动态编程。实现为纯模块 `core/js-tool.ts`（前导生成 + 子进程桥接，可独立单测）；**分发层守卫抽到 `core/exec/tool-bridge.ts` 与 py 桥共用**（两条桥一份实现，漏一条即多一条绕过通道），py 侧见「py 工具桥（仅本地模式）」。

#### 为什么编排层是 `js`（而非 `py`）

**分工结论：编排首选 `js`，计算归 `py`**——这条边界由两边能力边界决定，非口味偏好（`py` 与客卿承载重计算：pandas/numpy/BM25/哈希/图像；`js` 承载编排），也是**默认编排入口落在 `js`** 的依据。`py` 侧的编排能力**仅在本地模式**以「py 工具桥」补齐（见下节）——结论不是「py 不能编排」，而是下表三项成本在沙箱/多用户部署不划算；本地单用户部署下它们可接受，故桥只在该形态启用：

| 编排层的硬要求 | `js` | `py` |
|----------------|------|------|
| **零装配可用** | Bun 运行时已 `--compile` 内嵌进二进制，`gebai exec` 复用自身（边际成本 0，交付即用） | 解释器不可内嵌——需宿主机安装，版本/环境不可控（DESIGN「脚本执行环境」标明宿主机要求为「安装 Python」） |
| **协议通道可独占** | `console.*` 是单一汇聚点，补丁即接管几乎全部输出，stdout 保持纯协议 | 输出路径分散（`print`/`logging`/`warnings`/C 扩展直写 fd/`os.write(1,…)`/子进程继承 fd），**stdout 不可独占**——故 py 桥**不占 stdio，另开通道**：回环 socket + 一次性 token 认证（Windows 无 fd 继承，stdio 之外只有命名管道/loopback socket 两条路，已按后者落地，见下节） |
| **可静态审查（安全模式的只读承诺）** | `scanJsReadOnly` 词元级拒绝 + 子进程 shim（写/进程/网络 API 屏蔽、`Function.prototype.constructor` 中性化）可**做实**「降级为只读运行时」 | 等价承诺不可做：`__import__`/`getattr`/`eval`/`exec`/`ctypes`/`pickle` 遍地逃逸面，字符串可拼出任意调用；进程内 `sys.addaudithook` 也拦不住 `os.system` 起的新进程（新进程无钩子）——**故安全模式不注入 py 桥**（只读运行时形态保持原样） |
| **固化闭环** | 函数一等公民，`fn.toString()` 直接序列化、跨进程重新求值语义干净（defineTool 与编排在同一语言里闭合） | `inspect.getsource` 在闭包/装饰器/模块依赖边界上脆弱，「源码序列化→新进程求值」不干净 |

编排的三个刚需 **`js` 原生即得**（**工具即函数**——模块作用域函数声明 + `defineTool` 注册后同脚本内立即可调；**真并发**——`Promise.all` + 行级分发按 id 配对；**数据模型同构**——工具 schema/工具实现/编排同为 JS，一个语言内闭合），`py` 侧则由「py 工具桥」在**本地模式**现搭（工具名即函数、线程池并行调用、`json.loads` 注入与 `None`/`True` 映射的心智成本）。加上内嵌运行时的毫秒级启动，契合临时脚本这一高频用法。

`py` 不可替代之处（故只作计算/长驻状态，不作编排）：重计算生态、已有 Python 资产与团队技能（写客卿驱动同样进工具面）、**长驻状态**（`vision_run` 的会话级 REPL 命名空间——`js` 是一次性脚本不留状态）。

#### py 工具桥（仅本地模式）

`py` 在本地模式（非沙箱、非安全模式、非经 js 桥调用）下**也具备「工具即函数」编排能力**，调用面与 `js` 对称——代价按上表三项在小规模部署可接受，且不与「编排首选 js」冲突：`js` 仍是默认编排入口（零装配 + `defineTool` 闭环），py 桥服务于已有 Python 资产/技能与「计算与编排同脚本」的本地场景。

- **通信：协议不借用 stdio**（本实现的核心取舍）。Python 输出路径分散，stdout 无法像 `console.*` 那样被单点接管，故桥**另开通道**：父进程 `net.createServer` 监听 `127.0.0.1:0` 随机端口 + 一次性 token（`randomUUID`），**只接受首个连接**（首行 `{"t":"auth",…}` 校验，失败即断开）且连接建立后**立即关闭监听**；`print`/fd 直写/C 扩展/子进程继承的所有输出一律原样进 stdout（父进程按纯文本捕获，与旧 `runCommand` 路径语义一致），stdin 亦不被占用（`input` 以变量注入）——**不存在用户输出污染协议解析的问题，也无需 fd 重定向/特殊标记**
- **调用面**：已启用工具名即函数——`r = read({"path": "a.txt"})`（Python 侧按 `keyword`/`builtins` 过滤非法标识符与内建同名）；动态名用 `tools.call(name, params)` 或 `tools.<工具名>(params)`；返回值为 dict 且支持属性访问（`r["output"]` ≡ `r.output`，含 `data`/`blocks`/`truncated`；**属性访问仅作用于顶层**——嵌套字段是普通 dict，按下标访问，如 `r["data"]["exitCode"]`）；工具失败抛 `_G_ToolError`（异常类**已注入用户命名空间**，可 `except _G_ToolError` 按名捕获——未注入则 NameError，与工具描述承诺不符；`except Exception` 同样可行）。注入 `ctx`（`env` 运行时取 `os.environ`——脚本文件不落盘密钥，与 js 同规则）与 `input`（对象/数组原样注入为 `dict`/`list`，与 js 的 `input` 语义一致；遮蔽内建 `input()`）；脚本设顶层变量 `result = …` 即作为结构化返回值（进 `data.result`），`data` 为 `{stdout, stderr, exitCode, result?, calls?}`
- **真并发**：应答由后台读线程按 id 派发到各调用方队列，`tools.call` 为**同步阻塞** API——单线程写法自然，`ThreadPoolExecutor` 可并行调用（与 js 的 `Promise.all` 同语义）
- **门控（fail-closed）**：仅 `!ctx.sandboxed && !ctx.safeMode && !bridgeLangs.includes("py")` 才注入桥——沙箱模式（服务端多用户部署）与安全模式走既有 `runCommand` 纯脚本路径（行为不变）；安全模式**刻意不给桥**（只读承诺不因新通道被削弱，需要编排时用 js）；py 桥重入（链中已含 py，链断在该层）同样不注桥。另一种语言首次进入时照常注桥（`js→py` 的 py 带桥，链重入由链封死）
- **降级**：socket/进程不可用（连接未在 8 秒内建立、子进程未能启动）时**不退回有污染风险的 stdio 桥**，改纯脚本执行并在输出首行说明「脚本桥不可用」
- **不支持 `defineTool`**（Python「源码序列化 → 新进程求值」不干净）；**审批恒需**（`code` 为任意代码、无法静态判定，`approval:false` 不生效；默认审批一次覆盖脚本内全部工具调用）
- **守卫与 js 共用**（`core/exec/tool-bridge.ts`）：名称容错、同类桥重入、运行时定义工具 depth 限制、安全模式硬阻断集（cron 类）、调用总数上限、必填参数校验、免审拦截、结果封顶与 `blocks`/子会话存档透传
- **嵌套面：链式重入（同类桥不可重入）**——ctx 携带**脚本桥语言链**（`bridgeLangs`，分发层逐层追加），判定用「已进入链 + 本桥语言」：
  - `py→js`、`js→py`（**另一种语言首次进入**）：**放行**——一层混合编排合法（py 里可借 JS 能力，js 里可借 Python 计算/生态）；链只增不减，任一语言第二次出现即拒，链长上限 2（最多再由 2 层脚本子进程），自然终止
  - `js→…→js` / `py→…→py`（同类重入，含 `js→py→js`、`py→js→py` 与动态工具中转）：**拒绝**——分发层硬拒（脚本侧表现为工具级异常，`try/except` 可捕获）；js 的 execute 另有一道链检查作纵深防御（拦住「工具内部直调桥」的旁路；此处为**返回文本的软拒绝**：主循环下模型读文本自愈，脚本内则由分发层先拦）
  - **为何重入必须硬拒绝**：脚本桥内是**程序化调用**——软拒绝会被脚本当正常返回值继续处理（`try/except` 抓不到），脚本无从判定
- **实现期踩坑（均已回归覆盖）**：① JSON 字面量含 `true/false/null` 不是合法 Python 字面量——注入数据一律经 `json.loads` 解析（`_G_CTX`/`_G_INPUT`/`_G_TOOL_NAMES`）；② 子进程退出（`child close`）与 socket `data` 事件派发顺序不保证——收尾须**排空**（等 in-flight 工具调用结束 + 数据安静 60ms，上限 500ms），否则 `done` 消息丢失（表现为「脚本返回值丢失」）；③ Windows 上 `close()` 时若有 pending `recv` 会触发 RST、对端丢弃尚未派发的 `done`——Python 侧先 `shutdown(SHUT_RDWR)` 再 `close`；④ Python 管道下块缓冲会打乱输出顺序——以 `-u` + `PYTHONUTF8=1` 启动

#### 执行模型

- 生成脚本文件（会话 `tmp/.gebai_js_{uuid}.ts`）= **运行时桥前导**（工具调用/日志重定向/引导执行）+ **工具内置函数声明**（模块作用域，按当前已启用工具名生成）+ 用户代码（包进 async 函数：顶层 await 可用、`return` 返回值），子进程执行后即删
- **子进程命令**：脚本调试模式 `bun <script>`；二进制模式 `[execPath, 入口, "exec", script]`（隐藏子命令复用内嵌 Bun 运行时，exec 段双位置路由，见「脚本执行环境」）；无 shell 参与（参数数组直传，无注入面）
- 子进程环境/工作目录与 `sh` 同规则：cwd=会话 `tmp/`、任务 env 注入；沙箱模式剔除敏感变量（豁免/本地不剔除）；超时/取消按进程树终止（Unix 进程组 / Windows taskkill，`timeout` 默认 300s 上限 540s，超时/中断退出码 124）
- **stdio JSON 行协议桥**：脚本调用工具 → stdout 写 `{"t":"call",id,name,params}`；运行时工具定义（defineTool）→ `{"t":"def",id,name,description,parameters,source,overwrite?}`；运行时工具注销（undefineTool）→ `{"t":"undef",id,name}`；服务端分发执行 → stdin 回 `{"t":"res",id,ok,result|error}`（行级并发：脚本可 `Promise.all` 并行调用，应答按 id 配对）；`console.*` 经补丁转 `{"t":"log",level,text}` 回传（stdout 保持纯协议通道）；结束以 `{"t":"done",value}`（返回值）或 `{"t":"fail",error}`（未捕获异常）收口，非 JSON 行按日志透传兜底

#### 注入脚本的调用面（内置函数风格）

- **工具函数（内置函数风格）**：执行时按当前已启用工具名生成**模块顶层函数声明**——`const r = await read({ path: "a.txt" })`，每个工具名都是一个可直接 await 的函数（含子Agent 命名空间工具）；动态名字用 `await tools.call(name, params)` 或 `await tools.xxx(params)`。返回 `{ output, data, blocks, truncated, filePath }`（`data` 即工具双输出的结构化通道，结构可先用 `tool_schemas` 查询）；工具级异常 = Promise reject（脚本可 try/catch 容错继续）；单字段 RPC 截断 100k 字符。**遮蔽无冲突**：函数声明在模块作用域，用户代码在 `__G_main` 内层——同名 let/const/function 正常遮蔽；非法标识符/保留字/与注入全局（tools/ctx/input/console）冲突的工具名跳过生成（仍可 `tools.call` 动态调用）
- **`ctx`（会话上下文）**：`{ user, sessionId, workdir, home, sandboxed, env, projects, messages }`——`env` 与子进程环境同规则脱敏；`messages` 为最近会话消息快照（默认最近 50 条、单条 2000 字符，文本抽取、跳过 system/空消息）；另有 `input`（编排传入的任意输入，JSON 文本自行 `JSON.parse`）
- **`defineTool(def)`（运行时工具定义）**：与子Agent 工具同写法定义新工具（见「运行时工具定义（defineTool）」），注册成功即注入脚本全局、后续可像内置函数一样调用；**`undefineTool(name)`** 注销已注册的动态工具（注册错版本/用完即弃的一次性工具）

#### 运行时工具定义（defineTool）

脚本可以像在子Agent 文件里写工具一样，把一段能力**固化为会话级新工具**：`await defineTool({ name, description, parameters, execute })`——`execute` 与子Agent 工具同签名 `async (args, ctx) => ({ output, data? })`（方法简写 `async execute(args) {...}` / 箭头 / 具名函数均可，`fn.toString()` 序列化保存）。

- **注册**：定义经 RPC `{t:"def"}` 桥到服务端，引擎校验（命名 `[a-z][a-z0-9_]{0,39}`、重名/子Agent 命名空间碰撞、描述与 execute 源码合法、安全模式拒绝）后并入**会话覆盖层**；注册成功即注入脚本全局——**同脚本内可立即像内置函数一样调用**；后续轮次模型直接按名调用（schema 随下一轮下发，与内置工具一致可被其他 js 脚本调用）。**`overwrite: true`**：覆盖本会话已注册的**同名动态工具**（原地替换、不占新名额，返回 `{registered, overwritten}`）——全局工具与子Agent 命名空间占用名**仍拒绝**（rename 不出的名字只能换名）
- **执行模型**：后续每次调用在子进程求值 execute 源码（复用 js 运行时桥：体内可调任意工具/ctx/内置函数，`runtimeDefined` 标记 + RPC 分发层 depth 守卫——**动态工具内不能再调用动态工具/js**（防递归嵌套子进程））；返回 `{output, data?}`（子Agent 语义，字符串自动映射为 output），失败抛工具级错误（编排「失败即中断」语义）
- **作用域与生命周期**：会话级，定义清单随会话 `chat.json` 落盘（`SessionData.dynamicTools`，execute 源码序列化保存）、`run()` 水合恢复（重启不丢）、`forgetSession` 随会话删除释放（会话删除即文件删除）；子会话运行（subsession_run）内定义的工具进本次运行注册表（运行结束释放，不落盘、不外泄主会话）；单会话上限 50（水合同限）；**`undefineTool(name)`** 注销（从覆盖层与 `chat.json` 同时摘除，重启不再水合；名字不在覆盖层即报错——绝不回落删除全局工具/子Agent 工具）；子会话运行未注入注销通道时 `undefineTool` 返回环境不支持（运行结束自然释放）
- **审批**：`requiresApproval` 可选（**默认 true 需审批**——固化后的每次调用与 sh 同姿态，仅明确安全的只读/幂等工具定义时显式传 false；定义时脚本本身已经过一次审批，代码展示在审批卡）；沙箱模式经 `isRiskyToolName` 拦截（js 整体受限，无定义入口）
- **校验反馈**：RPC 分发层对工具调用做必填参数校验（缺参即时拒绝并列出参数名，近似 TS 类型检查的即时反馈）；execute 源码须自包含（不闭包脚本局部变量——每次调用在全新子进程求值，体内可用 `read(...)` 等工具函数/`ctx`/`input`）

```
// 示例：把多轮要复用的加工流程固化为工具（后续直接 hello_tool({who})，不必重贴脚本）
await defineTool({
  name: "hello_tool",
  description: "向指定对象问好",
  parameters: { type: "object", properties: { who: { type: "string" } }, required: ["who"] },
  async execute(args, ctx) {
    const r = await sh({ command: "date" })
    return { output: `hello, ${args.who}（${r.data.stdout.trim()}）` }
  },
})
```

#### 守卫与限制

- **审批**：与 `sh` 同姿态——默认需审批（脚本=任意代码执行），`approval:false` 按次免审；**默认审批一次覆盖整个脚本含内部工具调用**（一次审批、依次执行）；**免审运行（`approval:false`/免审动态工具）时内部调用需审批的工具在 RPC 分发层被拒**——免审脚本体未经用户审阅，按剥离免审标记后的审批姿态解析（`stripApprovalFlags`，防脚本内再传 `approval:false` 自我免审，与引擎无交互硬门槛同规则；`requiresApproval` 函数异常按需审批处理）
- **安全模式**：`js` 本身**降级为只读运行时**而非禁用——静态扫描（`scanJsReadOnly`：动态 `import()`/`require()`/`eval()`/`Function()`/`import.meta.require`/`process.getBuiltinModule`/`process.binding`/`Bun.fetch`/`Bun.sqlite` 前置拒绝）+ 子进程 shim（Bun 写/进程/网络 API 屏蔽、`Bun.file` 拦写留读、eval/Function/fetch/Worker 全局删除、`Function.prototype.constructor` 中性化）；内部工具调用在 RPC 分发层按硬阻断集（`isToolBlockedInSafeMode`，cron 调度类）拦截（无绕过通道），`sh`/`py`/`write` 等照常可调、由各工具降级规则执行；`defineTool` 与磁盘持久化动态工具同规则降级（execute 源码扫描 + 只读 shim），注册与水合均允许——只读动态工具安全模式下可用
- **内部调用走同一 `ToolContext`**：`writeGuard`（子Agent 写范围）/`fileGuard`（防盲覆盖）等会话级守卫对脚本内工具调用同样生效
- **防嵌套（链式重入）**：脚本桥不可重入——ctx 携带**语言链**（`bridgeLangs`，分发层逐层追加，判定用「已进入链 + 本桥语言」），同类桥再次进入即拒（`js→…→js`、`py→…→py`，含经直执行工具/动态工具的中转）；**另一种语言首次进入放行**（`js→py` / `py→js` 一层混合编排，链长上限 2 自然终止）。**动态工具内不能调用动态工具/js**（`runtimeDefined` + depth 守卫），**动态工具运行器（depth 1）内不能再 `defineTool`**（防递归注册）；`js` 的 execute 另有链检查作纵深防御（拦「工具内部直调桥」旁路，为返回文本的软拒绝）；`sh`/`py` 不受限（脚本本就可 `Bun.spawn`）
- **规模上限**：单次脚本工具调用总数 ≤ 100；RPC 协议行子进程侧 ~2MB / 服务端 2.5MB 截断兜底（防巨对象撑爆内存）；非 JSON 输出行 ≤100k 按日志透传、超长丢弃留注（多为 2MB 截断产生的非法 JSON，防垃圾文本刷屏）
- **密钥不落盘**：`ctx.env` 不嵌入临时脚本文件（历来明文写入合并后的全部环境变量，崩溃残留即密钥泄漏）——子进程内改为运行时引用 `process.env`（spawn 已传同源环境、沙箱脱敏同规则，语义完全一致）；messages 等会话数据仍嵌入（ctx 注入契约，属任务数据）
- **内置函数声明过滤 JS 全局名**：子Agent 工具名为 JS 全局（`fetch`/`JSON`/`Promise`/`Bun` 等）时不生成模块作用域函数声明（会遮蔽全局，`JSON.parse` 莫名变工具调用），仍可经 `tools.call` 动态调用
- **失败语义**：未捕获异常/非 0 退出/超时/中断 → 失败结果（`[脚本失败]` + 错误/退出码，不中断任务）；`strict:true` 时失败抛工具级错误（编排「失败即中断」，与 sh/py 一致）；成功无输出明确提示「（脚本执行成功，无输出）」

#### 结果（双输出）

- 模型可见 `output`：console 输出汇总（warn/error 加前缀）+ `[返回值]` 预览（2000 字符，完整值在 data；返回值须为纯 JSON 结构——Map/Set/Date 经 JSON 序列化会变形或丢失）
- 结构化 `data`：`{ exitCode, logs, result, calls: [{name, ok, error?}], timedOut?, interrupted? }`（logs/result 截断至 100k 字符）
- **blocks 透传**：内部工具产生的图片/图表/文件块去重（`type:path`）限量（10 个）透传到 js 结果——编排 read 图片/产物类工具时图像块直达 UI/模型，不再丢失；file 块另带**来源工具名 `via`**（`collectBridgeBlocks` 写入）——脚本桥外层没有 `card.file` 声明，前端「文件展示方式」据此判定产物卡形态，桥内调用文件工具与直接调用表现一致
- **`subsession_run` 存档透传**：脚本内调用 `subsession_run` 时子会话完整存档（`subSessionArchive`）透传到 js 工具调用记录（历史回放不丢；脚本侧返回值同样携带）；多个调用取最后一个

```
// 动态编程示例：读文件 → 按内容分支重试 → 聚合返回（工具像内置函数一样直接调用）
// js({ code }) — 已启用工具名即顶层函数：read/sh/glob/...，直接 await 调用
const r = await read({ path: "config.json" })
let cfg
for (let i = 0; ; i++) {
  try { cfg = JSON.parse(r.output); break } catch (e) {
    if (i >= 2) throw new Error("配置解析失败: " + e.message)
    await sh({ command: "echo retry", approval: false })
  }
}
const items = await Promise.all(cfg.files.map(f => tools.call("read", { path: f })))
return { total: items.length, sizes: items.map(x => x.output.length) }
```

### 通信协议

**前后端统一在一个端口暴露**：静态 Web UI、WebSocket、REST API 均由同一服务（Hono）在同一监听端口（`GEBAI_PORT`）上提供，按路径路由分发：

| 路径 | 通道 |
|------|------|
| `/`、`/assets/*`、`/vendor/*`、`/fonts/*` | 内置 Web UI 静态资源（浏览器/WebView 直接访问；`/vendor` 为图表引擎等静态伺服的第三方产物、`/fonts` 为内置字体） |
| `/ws` | WebSocket（实时通道） |
| `/api/*` | REST HTTP（同步通道） |
| `/api/docs` | OpenAPI 文档 |

服务端对外提供双通道接口，方便业务系统集成：

- **WebSocket（`/ws`，实时通道）**：双向。客户端发起请求-应答 RPC（会话管理、审批决策等）；同时服务端**主动推送引擎事件**（`event.*` 前缀消息，如 `event.tool.call` / `event.approval.request` / `event.tool.result` / `event.message.delta` / `event.task.done`），每个连接订阅事件总线并按「会话归属」过滤——仅推送该连接用户可访问会话的事件，多用户隔离。SDK 侧 `onEvent(cb)` 接收推送事件。
- **REST HTTP（`/api/*`，同步通道）**：会话管理/文件/审批决策等；`POST /api/v1/sessions/:id/prompt` 为**非流式 JSON**（同步等待任务完成，返回 `{ message: 最终 assistant 消息, error?: 任务错误 }`），供业务系统与无 WS 环境集成；**流式输出统一走 WebSocket**（`/ws`）

消息发送与流式输出**统一走 WebSocket 单通道**：`sendPrompt`（SDK）经 WS `session.prompt` 发起任务，引擎事件（`event.message.delta`/`tool_call`/`tool_result`/`approval`/`task.done` 等）由连接级订阅推送，SDK 将事件流转换为 `ChatChunk` 迭代返回（`wsEventToChunk`，字段与原 SSE 契约一致）。

#### 反向代理支持

前后端同端口暴露，**整体可被反向代理**（Nginx/Caddy/网关等）统一代理到同一域名/路径下：

- **单一 upstream**：只需代理一个后端地址（`{host}:{port}`），无需为前端/API/WebSocket 配置多个 upstream 或端口
- **路径前缀挂载（免配置）**：前端一切请求路径（静态资源、`/api/*`、`/ws`）均以**当前页面 URL 为基准**自动解析——页面在 `/gebai/`、`/gebai` 或 `/gebai/files` 下时请求即自动带 `/gebai` 前缀（`@gebai/sdk` 的 `appBase`/`appPath`/`appWsUrl`，构建产物亦为相对引用），代理只需**剥离前缀**转发到服务端根路径，无需任何环境变量或构建参数；访问形式 `/gebai`（无尾斜杠）也能正确推出基准
- **WebSocket 代理**：代理需开启 Upgrade/Connection 透传（服务端 WS 路径为根路径 `/ws`，浏览器侧由页面基准带前缀），服务端依据标准 WebSocket 握手，可与 HTTP 同一 location 规则转发
- **代理头透传**：**仅消费 `X-Forwarded-For`**（来源 IP 判定，用于登录/注册限流分桶；`GEBAI_TRUST_PROXY` 控制是否信任）与 `X-Forwarded-Host`（仅在 `GEBAI_TRUST_PROXY=true` 时参与同源判定，见下条）；`X-Forwarded-Proto` **无消费点**，对外回调地址由 `GEBAI_PUBLIC_URL` 决定
- **同源校验与代理改写 Host**：本地/桌面免登录形态下带 `Origin` 的请求要求 `Origin.host` 与请求 `Host` 同源（模块脚本与 WS 握手必带 Origin）——代理须**保留原始 Host**（nginx：`proxy_set_header Host $http_host;`，HTTP 与 WS 升级两处都要），否则请求被 403（`cross-origin ws rejected`）；网关无法保留 Host 时设 `GEBAI_TRUST_PROXY=true` 并转发 `X-Forwarded-Host`（判定改按该转发主机认主；跨源伪造转发头需自定义请求头，浏览器先发预检、预检自带 Origin 且不带该头，仍被拦下）
- **HTTPS 终结**：代理侧终结 TLS 后转发明文即可，WebSocket 使用 `wss://`，无需服务端额外证书配置
- **示例（Nginx）**：
  ```
  location = /gebai { return 301 /gebai/; }   # 无尾斜杠访问先补尾斜杠（前端基准由此推出，与路由惯例一致）
  location /gebai/ {
      proxy_pass http://127.0.0.1:3000/;      # 尾斜杠 = 剥离 /gebai 前缀，服务端按根路径受理
      proxy_http_version 1.1;
      proxy_set_header Host $http_host;       # 保留原始 Host（同源校验按它比对；改写成 $proxy_host 会全量 403）
      proxy_set_header Upgrade $http_upgrade;        # WS 升级（同一 location 规则即覆盖 /gebai/ws）
      proxy_set_header Connection $connection_upgrade;
      proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
      proxy_set_header X-Forwarded-Proto $scheme;
  }
  ```

#### WebSocket（实时通道）

面向实时双向交互（内置 Web UI、桌面端、SDK），单连接承载会话操作与流式推送，连接地址为 `ws(s)://{host}:{port}/ws`。

**请求-应答机制（RPC）**：客户端每个请求带自增 `id`，服务端应答回带同一 `id` + `ok`/`error`；SDK 按 `id` 匹配，把应答自动分派到发送时注册的成功/失败回调（`send()` 的 `onOk`/`onError`，应答错误/超时/断线均走失败回调），也提供 Promise 版 `request()`。适用于**一次性操作**（会话管理、审批决策、心跳 ping 等，应答即终态）。**对持续增长的内容不适用**——流式输出（`event.message.delta`/`reasoning` 等）由服务端以无 `id` 的 `event.*` 推送承载（带 seq 可重放、断线补偿），`session.prompt` 的应答仅确认任务接收，内容全部经事件通道回流；**应答语义如实**：启动期失败（会话不存在/任务已在运行/限流）在应答中返回 `ok:false` + 协议错误码（`code: session_not_found | already_running | rate_limited | prompt_required`），客户端按错误码判定（补发幂等识别 `already_running`，不再依赖错误文案正则——跨包隐式契约），任务运行期错误仍经 `event.task.error` 推送。

**心跳保活**：客户端连接建立后周期性发送 `ping`（默认 5s 间隔，构造参数可调）——**心跳复用请求-应答机制**（`send()` 注册回调，`timeoutMs` 取心跳超时），服务端应答 `pong`（payload `{pong:true}`），防止 Nginx 等代理按闲置时间静默断连；pong 应答超时（默认 10s）或应答错误判定连接已死（代理掐断后的半开 TCP），客户端主动断开并触发指数退避自动重连。

**消息按序处理（每连接串行）**：服务端对同一连接的请求**按到达顺序串行处理**（前一条处理完成后再处理下一条）。Bun 对 async `websocket.message` 处理器不保证串行（前一条 await 磁盘/网络 I/O 时下一条即并发进入），因此服务端在 `/ws` 入口以每连接 promise 链强制串行——保证 `auth.login` 的 `conn.set(user)` 先于其后任何请求生效（否则紧跟在认证后的请求会以未登录态并发抢先执行，被拒 `unauthorized: login required`），也保证 `session.switch` 等有状态操作对后续请求可见（SDK 依赖该契约：建连后先发认证、再冲刷离线队列）。

**状态一致性模型（MVC 桌面模式）**：服务端为**权威状态**（Model），连接层为控制器（Controller），SDK/UI 为视图镜像（View）。核心机制：

- **每用户事件日志（seq + 有界环形缓冲，容量 1000）**：服务端级单点订阅事件总线，按会话归属路由到所属用户的日志，分配全局递增 `seq`；在线推送即日志条目的实时投递。**断线期间事件照常入日志**（与连接数无关），重连后客户端按 `seq` 增量重放（`sync.request` → `sync.replay`），离线事件不丢失；缓冲溢出（缺口）返回 `overrun`，客户端走全量重同步（`session.get` 等）收敛。**日志尾部持久化**（`users/{user}/ws-journal.jsonl`，JSONL 追加、定期重写裁剪回尾部 1000 条）：服务重启后 seq 从持久化尾部连续编号——断线×重启重叠窗口内的事件仍可重放（此前重启后 seq 重编号、重叠窗口事件永久丢失且不触发 overrun）。**文本增量合并**（`event.message.delta` 按消息 50ms 窗口合并为单条日志事件，非增量事件先冲刷保持顺序）：快速流式输出不再逐 chunk 高频冲掉日志缓冲（稍长断线即 overrun 降级全量重同步）
- **每用户连接状态持久化**：`session.switch` 的当前会话写入 `{GEBAI_HOME}/conn-state.json`（防抖落盘），新连接/服务端重启后自动恢复（连接级显式覆盖优先）；会话删除时若为当前会话则清空
- **状态快照（`state.snapshot`）**：连接建立（本地模式）/登录成功（服务模式）后服务端自动推送，含当前会话/会话列表/运行中会话/日志基线 `lastSeq`/**模型上下文窗口 `maxContextTokens`**（engine `contextWindow()` 取自 provider capabilities，标题栏上下文占比显示用，0=未知）；客户端作为模型基线，也可主动请求（幂等）；**SDK 快照模型保留 `maxContextTokens` 字段**（重建快照不丢弃，标题栏占比显示的数据源）；**运行态明细 `runtime`**（会话 id → `{ startedAt, pending, toolCalls, bgTasks, subRuns }`，键集即 `running`，引擎 `runtimeOf()` 同一遍历产出 running 与明细）——待决交互明细 + 在途工具调用数 + 后台命令任务（sh async 记录，按会话过滤）+ 子会话运行概要；前端据此把「哪个会话在跑/在等谁」标进会话列表（行内角标：运行中圆点、等待输入沙漏，悬浮说明含后台任务与子会话数），不必逐个附加就能看出后台会话的处境（含非当前会话），任务开始/结束事件另行增量收敛角标（不等下一次快照）
- **重连自动重新认证**：WS 连接无法携带 Header，SDK 持有令牌/API Key 时在每次建连后自动发送 `auth.login { token }` / `auth.login { apiKey }` 恢复用户上下文（服务模式重连不掉回未登录态）
- **流式任务断线恢复**：`sendPrompt` 断线时**挂起**（不抛错），等待自动重连后按 `seq` 重放离线事件无缝续流；任务未确认接收时以快照判定（运行中→恢复；已跑完→从存储合成收尾；未开始→补发一次，遇协议错误码 `already_running` 视为已接受**（仅限补发路径：原请求已被服务端受理、确认帧丢失）**；**首次发送**遇 `already_running` 如实上抛——另一处的任务在跑、本次输入未被受理，前端撤回幻影消息（已上屏但从未进存储/模型）并把内容放回输入框，同时附加该会话运行态；**已受理的任务重连后核对服务端真相**（快照 `running` 不含该会话 → 从存储合成收尾：有助手回复则回填该回复，无回复但有引擎提示（服务中断说明）则展示说明，两者皆无则合成「已结束但无产出」说明——否则要等前端 150s 空闲看门狗才收场，服务模式（无 bootId 自动刷新）尤其明显；受理后引擎登记运行态前的起跑竞态由宽限窗口（`acceptSettleMs`，默认 15s）排除，测试可调小）；`overrun` 走全量重同步（`resume` 重置 + **标记 `reloadHistory`**：前端重读消息列表（缺口期间的工具卡/子会话容器事件已丢失且不可重放）+ 重取待决交互清单重建卡片，重载期间正文只累积、完成后按累积内容重建在途气泡；种子只取**本轮**（当前用户消息之后）的已落盘内容——不过滤会把上一轮回答当本轮在途文本回填）。**重放事件经全局 `onEvent` 分发**（与在线推送同通道）：离线期间的审批/选择/工具事件全局订阅者（前端卡片渲染）也能收到——重连后审批卡/工具卡可恢复，而非只进 `sendPrompt` 的 chunk 通道（此前离线窗口内的审批请求永远不渲染、任务卡死至超时）
- **运行中会话恢复（页面刷新/切换，`session.attach`）**：前端恢复逻辑拆分在 `web/src/attach.ts`（纯副作用模块——模块尾部 `setRunningAttach(attachRunningSession)` 注册钩子，无具名导出），**入口 `main.ts` 须保留 `import "./attach"`**——丢失导入不触发 typecheck/lint 报错、钩子恒为 null 静默跳过（`attach.test.ts` 源扫描守住该导入）；断线恢复覆盖「连接断而页面在」，**页面刷新/切换进入运行中会话**是另一类恢复——页面内存态（运行态 runs、sendPrompt 流、待决交互卡片）全部丢失，事件早已推送过、重放基线（快照 lastSeq）也已越过。恢复链路：引擎在任务态上维护**附加快照**（`TaskState`：`startedAt` 任务开始时刻、`stream` 在途 assistant 回合的累积文本/推理——delta/reasoning 发布点同步累积、消息持久化点清空、含 session 标记路由到子会话容器；审批/选择/填值/画图/捕获的等待项保留**展示载荷**——工具名、prompt/options、env 名与说明、图表源码、捕获参数）→ `session.attach` 返回 `{ running, startedAt, stream, pending, tools, lastSeq }`（归属校验同其余会话操作；lastSeq 为该用户日志基线；**`tools` 为在途工具调用**——已发出尚未产出结果的调用（名称/参数/子会话归属），引擎在事件发布单点拦截维护（tool.call 记入、tool.result 删除），前端据此重建「**等待中的工具卡**」：走实时事件同一渲染入口 `onToolCall`，结果到达时按 toolCallId 配对填充，与在线路径完全同构）→ 前端 `loadMessages` 尾部触发附加：**待决交互卡片重建**（审批/选择/填值/画图/捕获走既有渲染入口，替换式幂等——不重建则任务干等到超时；审批锁输入随之恢复）→ `consumeTaskStream` 接管 SDK `attachStream`（与 `sendPrompt` 同构的 ChatChunk 迭代但不发起任务：订阅就位后请求快照，**在途文本/推理作为种子 chunk 先行**（session 标记路由到惰性重建的子会话折叠容器），按 seq 重放快照之后的事件（与断线恢复同一套机制，缺口 overrun 放弃附加由存储恢复兜底），此后实时续流到任务结束）——信号灯思考闪烁、停止按钮、单轮计时（起点用快照 startedAt）、空闲看门狗、收尾清理（审批卡/配对/自动标题）与发起页完全同构。附加期间再次断线：挂起重连后**重新附加**（`resume` 重置渲染态 + 新快照重播种，防内容重复）；附加竞态（任务已在附加前结束，running=false）立即结束迭代，视图由存储恢复兜底。防重：附加请求在途标记 + `runs` 已接管即跳过；**附加失败退避重试**（1s/3s/9s 共三次）——静默失败会让界面谎报空闲（无信号灯/停止按钮/计时），用户随后的发送还会被 `already_running` 拒结
- **任务开始事件与快照清单（运行态同步的两条腿）**：任务并非总由本页发起——重启续跑/飞书桥接/定时任务/其他标签页都会在页面空闲时开始运行，而「进入会话时探测一次」会漏掉此后的开始（典型：重启后页面先自动刷新、续跑才启动），页面便一直没有信号灯/停止按钮/单轮计时。两条腿分别覆盖两种时序：引擎在 `run` 的用户消息落盘后发布 `event.task.start`（含 `startedAt`）——**刷新早于任务开始**时页面经全局 `onEvent` 实时收到并附加恢复；快照 `running` 清单（连接建立/重连到达）——**刷新晚于任务开始**时事件已投递过且不随快照重放，页面据此发现当前会话在运行而未接管。两处均调 `sessions.attachRunningIfNeeded`（与进入会话时同一附加实现，`runs`/`attaching` 去重，未运行或本页已接管时 no-op）。
- **交互等待心跳与看门狗边界（前端断开不停任务）**：等待用户作答（审批/选择/填值/画图/捕获）期间无任何输出，工具执行心跳也不覆盖——等待注册时启动 25s 周期心跳 `event.interaction.alive`（payload `{kind, id}`，等待结束/超时/取消即停，与工具心跳同款，见「等待期不误判挂起」）。前端空闲看门狗只在「**已连接** + **无待决交互卡** + 长时间无数据」时才中止并 `cancelTask`：断线期间无从判定服务端死活（且取消请求会随重连送达而杀掉仍在跑的任务），等待用户作答则本就不是挂起（服务端等待超时会推来结果事件自行刷新活跃）——此前两者都命中看门狗，断开或思考超 150s 即把后台任务杀掉。
- **服务中断留痕（跨进程重启的中断可见）**：任务运行期间在会话目录写 `run.json`（`startedAt`/归属用户/本轮输入摘要），正常收尾（成功/失败/取消）删除；**残留标记 = 上一进程死在任务中途**——启动时扫描（`reportInterruptedRuns`）对这类会话补一条可见说明（user 角色 + `engineNote: "interrupted"`，与其余引擎注入同口径（内容头带 `【智体·任务中断】` 身份标记）；UI 渲染为「任务中断」通知条），并清标记防重复补记（写失败也清）。否则这类会话只剩「用户问了、助手一个字都没有」，既看不出发生过什么也判断不了要不要重发；说明同时进模型上下文（下一轮模型知道上一轮被打断了）。
- **发送背压**：服务端所有 WS 发送经统一 sink（`makeWsSink`）——连接发送缓冲超 16MB 判定慢客户端，主动断开让其走自动重连 + seq 重放收敛（此前 `ws.send()` 返回值被忽略，慢客户端 + 高频流式下发送缓冲无界增长）

WebSocket 消息格式（JSON）：
```ts
// 客户端 → 服务端（请求）
{ type: string; payload?: object; id?: string }
// 服务端 → 客户端（请求响应 + 主动事件）
{ type: string; payload?: object; id?: string; ok?: boolean; error?: string }
// 服务端主动推送的事件（带 seq，幂等序号）
{ type: "event.*"; seq: number; sessionId: string; payload: object; timestamp: number }
```

客户端请求类型：

| type | 说明 |
|------|------|
| `ping` | 心跳（客户端周期发送，复用请求-应答机制，服务端回 `pong`，保持连接穿越代理；pong 超时判定死连自动重连） |
| `auth.login` | 用户登录（服务模式），成功后建立用户上下文 |
| `auth.logout` | 登出，令牌失效 |
| `session.list` | 获取会话列表 |
| `session.create` | 创建新会话 |
| `session.get` | 获取会话详情 |
| `session.delete` | 删除会话 |
| `session.rename` | 重命名会话 |
| `session.pin` | 置顶/取消置顶会话（`pinned` 布尔；元数据操作不刷新 `updatedAt`） |
| `session.switch` | 切换当前会话 |
| `session.env.get` | 获取会话环境变量（内存态，含来源层级） |
| `session.env.set` | 设置/覆盖/删除会话环境变量（内存写入，不落盘） |
| `session.todo.get` | 获取会话待办清单 |
| `session.tool.get` | 获取当前生效工具集（含启用状态） |
| `sub_agent.list` | 列出可用子Agent |
| `sub_agent.load` | 装载子Agent 模块（按需装载；可选 `sessionId`——传入时装载到该会话：注册工具 + 提示词消息写入会话记录，缺省仅全局注册工具） |
| `session.files.list` | 列出会话临时文件 |
| `session.files.get` | 读取会话临时文件内容 |
| `session.current` | 获取当前会话 |
| `sub_agent.unload` | 卸载子Agent（带 `sessionId` 为会话级；全局形态仅 admin） |
| `session.restore` | 从 `trash/` 恢复已归档会话（归属用户或 admin） |
| `session.cancel` | 取消当前任务 |
| `session.attach` | **运行中会话附加快照**（页面刷新/切换恢复用）：返回 `{ running, startedAt, stream, pending, lastSeq }`——在途流式累积（未持久化部分文本/推理 + messageId/session 标记）、待决交互清单（审批/选择/填值/画图/捕获，含重渲染所需全部载荷）、任务开始时刻与该用户事件日志基线 seq；`running=false` 表示未运行（前端放弃附加）；归属校验同其余会话操作 |
| `session.compact` | 主动压缩会话上下文（支持范围参数，返回压缩条数与摘要） |
| `session.prompt` | 发送对话消息（WS 通道发起任务，流式事件经 `event.*` 推送回流，reply 仅确认；payload 支持 `messageId` 可选字段，语义同 REST prompt） |
| `session.attachment.upload` | 上传附件（WS 通道，base64 整体传输，返回会话内引用路径） |
| `choice.decide` | 提交用户选择（ask 选项询问分支阻塞等待的回应；`option` 单选 / `options` 数组多选 / `refuse: true` 或 option、options 均缺失表示拒绝回答） |
| `env.decide` | 提交用户填写的环境变量值（ask 填值分支阻塞等待的回应，envId + value；value 缺失表示拒绝提供） |
| `draw.result` | 提交前端渲染结果（show 图表分支阻塞等待的渲染回传，renderId + ok + error） |
| `capture.result` | 提交前端页面捕获结果（page_capture 工具阻塞等待的捕获回传，captureId + html + imageBase64[可选，png/jpeg data URL 或裸 base64] + error[可选]） |
| `feedback.list` | 查询反馈（管理员可查全部用户，普通用户仅本人；支持 messageId/sessionId/type 过滤） |
| `user.list` / `user.create` / `user.update` / `user.delete` | 用户管理（服务模式，仅管理员） |
| `approval.decide` | 审批决策（通过/拒绝，对应待审批的工具调用）；**决策已失效时返回错误码 `expired`**（等待超时/已在其他地方处理/任务已结束；同规则适用于 `choice.decide`/`env.decide`/`draw.result`/`capture.result`，前端据此提示并撕掉失效卡片，不再假报成功） |
| `feedback.submit` | 提交反馈（点赞/点踩/文字，关联会话与消息；服务端自动补 model/subAgent 关联，见「用户反馈」） |
| `state.snapshot` | 获取状态快照（主动请求，幂等；与自动推送 payload 一致） |
| `sync.request` | 断线补偿：按 `lastSeq` 请求重放离线期间错过的日志事件；`sync.replay`（`{events, lastSeq}`，seq 严格递增、payload 含 sessionId）或 `overrun`（`{events:null, overrun:true, lastSeq}`，缺口→客户端全量重同步） |
| `auth.login` | 登录支持两种形式：`{username, password}`（密码）与 `{token}`（已有令牌，SDK 重连自动重新认证用）；成功后连接级事件订阅自动重绑到新用户并推送状态快照 |

服务端主动推送事件（`id` 为空，`type` 以 `event.` 为前缀，**均携带 `seq`**——每用户日志序号，重连后按 seq 重放补偿；旧服务端不携带 seq）：

| type | 说明 |
|------|------|
| `event.message.delta` | LLM 文本增量（流式）；子会话运行过程的文本增量携带 `subSession: true` + `subSessionId`（前端渲染进子会话折叠容器）；`messageId` 每轮刷新（引擎每轮生成新 id），前端据此检测轮界——主循环 text 路径与子会话容器均按 id 变化封段（后台会话工具事件不渲染卡片从而不在工具调用处封段，轮界检测是唯一分段保障） |
| `event.message.done` | 一条完整消息生成完成（子会话轮的 done 同样携带 `subSession: true` + `subSessionId`） |
| `event.message.reasoning` | 推理内容增量（reasoning_content/thinking，前端思考中展开实时展示、推理段结束自动折叠）；子会话运行过程的推理增量携带 `subSession: true` + `subSessionId` |
| `event.subsession.start` | 子会话 run 开始（含 runId + agents 列表 + input + depth；**每轮重推、同 runId 幂等**——前端容器已存在则忽略，容器随消息重载丢失后据此重建）；携带 `subsession`（子会话名）+ `model`（模型路由名，未指定缺省），前端容器标题渲染「🌿 子会话 · 名（模型）」 |
| `event.subsession.done` | 子会话 run 结束（含 runId + agents + output[最终返回]；异常时 output 为空并携带 error），前端折叠容器并写入返回摘要；`subsession` 标识子会话名。**每轮模型回复结束与循环上限退出都会推送**（与 start 同为每轮重推、同 runId 幂等），前端靠封段而非等 run 结束 |
| `event.subsession.merged` | 子会话报告合入父上下文（subsession_run 继承形态**最终合入**与运行中 `subsession_merge` 交出 content 的**阶段性合入**均推送，含 messageId/runId/name/model/text[合并消息全文]）：前端实时渲染「子会话合入」通知条（消息落盘 `role: "user"` + `engineNote: "subsession"`；历史回放由存储中的合并消息承担——最终合并含过程存档折叠容器，阶段性合入仅文本） |
| `event.tool.call` | 工具开始执行（含名称与参数）；子会话运行过程中的工具调用携带 `subSession: true` + `subSessionId`；**门控说明性结果（缺参/未知工具/通道禁用/安全拦截/无交互拒绝/重复中断/重复终止）同样推送**（与结果事件成对，前端实时建卡，不依赖刷新回看落盘历史） |
| `event.tool.result` | 工具执行结果（含截断标记与文件路径）；子会话运行过程中的结果携带 `subSession: true` + `subSessionId`；**审批拒绝/超时与取消/中断占位补写同样推送**（实时卡片落终态，不停留「执行中」） |
| `event.todo.update` | 待办清单变更（新增/状态/进度） |
| `event.todo.continue` | 待办续做提示（会话完成时仍有未完成待办，已追加 user 提示消息（`engineNote: "todo"`）继续会话；含 round/remaining/messageId/text，前端据此实时渲染「引擎提示」通知条） |
| `event.verify.nudge` | 收尾验证提醒（改代码未跑测试的任务结束注入一次，与待办续做同形态的 user 软性提醒（`engineNote: "verify"`）；含 messageId/text，前端据此实时渲染「引擎提示」通知条） |
| `event.draw.render` | 画图渲染请求（show 图表分支执行中推送，含 renderId + 图表源码 + **`format` 图表语言**（mermaid/plantuml/d2/echarts），前端按语言本地渲染后经 `draw.result` 回传） |
| `event.env.request` | 环境变量填值请求（ask 填值分支执行中推送，含 envId + name + description + secret[是否敏感值]，前端弹窗填值后经 `env.decide` 回传；值注入本次任务 env 并保存到浏览器本地） |
| `event.capture.request` | 页面捕获请求（page_capture 工具执行中推送，含 captureId + fullPage[是否整页截图]，前端捕获当前页面渲染后 DOM html + 截图后经 `capture.result` 回传） |
| `event.approval.request` | 审批请求（含工具信息、重试次数与**调用参数 `arguments`**——飞书审批卡片据此展示参数摘要） |
| `event.task.queued` | 任务入队（含任务 ID/类别/来源与队列位置） |
| `event.task.start` | 任务开始执行（含类别/执行体/来源/执行会话） |
| `event.task.result` | 任务执行结果（成功/失败/跳过/超时与输出摘要、自动停用标记；prompt 型过程经消息流呈现） |
| `event.task.queue` | 任务队列变化（额度/排队数/运行中数） |
| `event.task.done` | 本轮任务完成 |
| `event.task.start` | 本轮任务开始（前端据此切换运行态/信号灯） |
| `event.choice.request` | ask 选项询问请求（前端弹选择卡片，经 `choice.decide` 回传） |
| `event.tool.result.start` | 工具结果输出开始（长结果分段落屏前的信号） |
| `event.tool.alive` | 长工具执行心跳（默认 25s，刷新前端空闲看门狗，见「等待期不误判挂起」） |
| `event.interaction.alive` | 交互等待心跳（默认 25s，payload `{kind, id}`；等待用户作答期间无输出，看门狗不得据此判定挂起，见「交互等待心跳与看门狗边界」） |
| `event.message.intermediate` | 助手中间轮文本（飞书 `notifyIntermediate` 通道预览用） |
| `event.message.compact` | 上下文压缩 / 护栏降级 / 超限裁剪通知（含 `degraded` 标记——`tool-images`/`user-images`/`user-message` = 溢出护栏降级，`trim` = 消息条数上限裁剪；**前端按此区分标题**：无标记 =「已压缩 N 条历史消息」，`trim` =「历史消息条数超限裁剪」，其余标记 =「上下文溢出护栏」，具体降级类型与后果由 `summary` 承载） |
| `event.task.error` | 本轮任务出错（含错误信息） |
| `event.model.error` | **模型服务异常（非终态，引擎将自动重试）**：接口异常/空响应重试前推送 `{error, retry, maxRetry}`——重试退避期间任务无输出，前端据此在消息流尾部挂**常驻**异常记录块（`model-error.ts`）：每次重试一行「模型服务异常（第 N/M 次重试）：原因」，行尾状态随进展更新（重试中 → 已恢复 / 重试未成功），**不随恢复输出或任务结束消失**（报错信息是排查依据，恢复与否都留痕）；同一条重试的重放原地更新不堆叠；重试耗尽的最终失败仍走 `event.task.error` |
| `event.session.ctx` | 运行中上下文大小更新（每轮模型调用后推送，含 ctxTokens token 计数：真实 usage 基准 + 未发送增量估算，无真值时全量估算 + 工具 schema 段估算兜底；会话列表 k 显示用。**推送同时把展示值与真实 usage 基线落盘**——`store.updateCtxStats` 只重写 meta.json：列表/状态快照/页面刷新与推送同口径，否则运行中反复刷新会在「上次任务结束时的旧值」与当前真值之间来回跳；基线随轮次即时写入，任务中断/重启后压缩判定与展示仍有真值（见「上下文占用口径」）。接口返回缓存字段时携带 `ctxCachedTokens`：同一次调用的提示词缓存命中 tokens，前端上下文圆环悬浮展示命中率；压缩/护栏降级改变上下文时立即补发一次，圆环当场回落） |
| `event.session.tps` | **输出速率帧**（标题栏速率仪表的数据源，`{tps, outTokens, genMs, est, active}`）：`tps = 输出 tokens ÷ 生成窗口`（窗口 = 首个输出 chunk 到末个输出 chunk，**不含首 token 等待**，量的是解码速度而非接口响应延迟）。**一次模型调用内按 1s 节流持续推送生成中帧**（`est: true, active: true`，服务端按 CJK 感知字符折算估算，窗口 ≥300ms 才报）——长回答全长期间数字持续刷新；**调用结束推收尾帧**（`active: false`，接口返回 usage 时 `est: false` 为实测口径，不返回时仍为估算收尾）。产出瞬时完成（收尾窗口 <50ms，非流式）时不推帧。前端只展示不自算，两帧之间保持上一帧（见「输出速率」） |

另有一个**非事件推送**：`state.snapshot`（`id` 为空，建连/登录后自动推送状态快照，客户端更新 MVC 模型并触发 `onSnapshot` 订阅）。

#### REST HTTP（同步通道）

面向业务系统同步集成，同一套核心能力以 HTTP 形式暴露，返回 JSON：

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/health` | GET | 健康检查（**公开端点**，服务模式下也无需鉴权——探活/负载均衡探测） |
| `/api/v1/users` | GET/POST | 用户列表/创建（服务模式，管理员） |
| `/api/v1/users/:id` | PATCH/DELETE | 用户启用/禁用/删除（管理员） |
| `/api/v1/sessions` | GET/POST | 会话列表/创建 |
| `/api/v1/sessions/:id` | GET/DELETE/PATCH | 会话详情/删除/重命名与置顶（PATCH body：`name` / `pinned`） |
| `/api/v1/sessions/:id/prompt` | POST | 发送消息，**非流式 JSON 返回**（同步等待任务完成，`{ message: 最终 assistant 消息, error?: 任务错误 }`）；body 支持附件引用、`env`（浏览器本地环境变量，临时注入仅本次任务生效，不持久化）、`messageId`（可选：客户端生成的用户消息 id，撤回/反馈定位用；非法格式服务端回退自动生成）、`interactionMode`（默认 `none`）与 `stream`（默认 `false` 仅最终响应）、`autoApprove`（可选布尔：`true` 需审批工具自动通过/`false` 无交互通道下直接拒绝，缺省通道默认姿态——本地自动通过、服务模式拒绝，见「交互模式」）；缺 `prompt` 返回 400；**流式输出请走 WebSocket（`/ws`）** |
| `/api/v1/chat` | POST | **单 HTTP 一站式调用**：一次请求完成「建会话（`sessionId` 缺省自动创建，`name` 可选命名）→ 执行任务 → 返回最终回复」，返回 `{ sessionId, message: 最终 assistant 消息, error? }`（`sessionId` 供后续请求续聊多轮）；带 `sessionId` 在既有会话续聊（id 白名单校验 400 / 不存在或非本人 404）；`interactionMode` 固定 `none`，body 其余字段（`prompt` 必填 400、附件/env/messageId/stream）与 `prompt` 端点同规则，`autoApprove` 支持自动审批（与 `prompt` 端点同语义）；与 `prompt` 共用每用户速率限制桶 |
| `/api/v1/sessions/:id/attachments` | POST | 上传附件（multipart，多模态内容），返回会话内引用路径 |
| `/api/v1/auth/me` | GET | 当前登录用户信息（服务模式；本地模式为 admin 用户） |
| `/api/v1/auth/exchange` | POST | **外部身份兑换**（服务模式 + 已配置验证器；body `{username, credential}`）：网站本地登录态 → GEBAI 令牌；400 缺参 / 401 验证失败 / 404 未启用 |
| `/api/v1/auth/external-config` | GET | **外部身份扩展点探测**（公开端点）：`{enabled, storageKey?, autocreate}`，供 Web UI 启动时读取（不泄露密钥） |
| `/api/v1/sessions/:id/truncate` | POST | 撤回消息：删除 before（消息 id，**用户/助手消息均可**）及其后的所有消息（body: `{ before: string }`）；消息 id 为持久化 id——前端发送时携带 `messageId` 后撤回可对齐当前会话刚发的消息，助手最终回复按流式 messageId 落盘同样即时可撤回；**会话有任务运行时 409 拒绝**（先停止或等任务完成） |
| `/api/v1/sessions/:id/cancel` | POST | 取消当前任务 |
| `/api/v1/sessions/:id/approval` | POST | 审批决策（通过/拒绝） |
| `/api/v1/sessions/:id/choice` | POST | 选择决策（ask 选项询问分支等待的用户回应，body: choiceId + option 单选 / options 数组多选 / refuse=true 拒绝，option、options、refuse 至少其一，options 不得为空） |
| `/api/v1/sessions/:id/draw` | POST | 画图渲染结果回传（show 图表分支等待的前端渲染结果，body: renderId + ok + error） |
| `/api/v1/feedback` | POST/GET | 提交/查询反馈（提交自动补 model/subAgent 关联；管理员 GET 可查全部用户并导出分析） |
| `/api/v1/sessions/:id/env` | GET/PUT | 获取/设置会话环境变量（内存态，不落盘） |
| `/api/v1/sessions/:id/compact` | POST | 主动压缩会话上下文（body 可指定范围） |
| `/api/v1/sessions/:id/todos` | GET | 获取会话待办清单 |
| `/api/v1/sessions/:id/files` | GET | 列出会话临时文件（含子目录，返回路径/大小/修改时间） |
| `/api/v1/sessions/:id/files/content` | GET | 读取会话临时文件内容（`?path=`，文本截断预览或原始内容） |
| `/api/v1/sessions/:id/files/download` | GET | 下载会话临时文件（`?path=`，二进制/文本原样下载，`Content-Disposition`） |
| `/api/v1/sessions/:id/files/download` | POST | 多选打包下载（body 指定 paths 列表，返回 zip） |
| `GET /files` | GET | **文件工作台页面**（独立 Vite 入口 `files.html`；`GEBAI_FS_ENABLED=false` 时 404） |
| `/api/v1/roots` | GET | 文件工作台根清单（`sess:` 会话工作区 / `proj:` 预置项目 / `bind:` 绑定项目 / `user:` 用户目录 / `abs:` 白名单根）+ 能力开关（fsEnabled/fsWrite/gitEnabled/gitWrite/gitRemote/writable/sandboxed/上限）；每根附 Git 标记（`isRepo`/`branch`/`repoRoot`）——探测语义（去重 + 浅路径优先父仓库短路 + ≤8 并发 + 60s 缓存）见「文件工作台 → 根清单探测」 |
| `/api/v1/roots/resolve` | GET | 单个根解析（?id=）——根可用性与真实路径校验 |
| `/api/v1/fs/list` \| `/fs/tree` | GET | 目录列举（懒加载，?root&path&sort&order&showHidden）/ 树快照（?root&depth） |
| `/api/v1/fs/stat` \| `/fs/read` | GET | 元信息 + `etag`（乐观锁）/ 文本读取（编码·换行·binary·truncated） |
| `/api/v1/fs/raw` | GET | 原始字节（支持 HTTP Range：图片/视频/PDF/大文件） |
| `/api/v1/fs/office` \| `/fs/archive` | GET | Office 阅读视图转换（docx/xlsx/pptx）/ 压缩包条目列表 |
| `/api/v1/fs/search` | GET | 名称/内容搜索（ripgrep 优先（异步 spawn，不阻塞事件循环），回退内置遍历；mode/glob/regex/maxResults） |
| `/api/v1/fs/watch` | GET | **变更监听（长轮询）**：`?root&dirs&rev&wait&git`——为 `dirs`（`.` = 根本身）挂 `fs.watch`，有变化立即返回 `{changed,rev,paths,git}`，无变化挂到 `wait` 秒回心跳；`rev` 缺省 = 只取基线；服务端中枢 `core/fs/watch.ts`（`GEBAI_FS_WATCH=false` 时返回 `enabled:false`） |
| `/api/v1/fs/download` | GET/POST | 单文件流式下载（?root&path）/ 多选或目录打包 ZIP（body paths，UTF-8 文件名） |
| `/api/v1/fs/write` | PUT | 保存文件（`etag` 乐观锁，冲突返回 409 + 当前磁盘内容；编码/换行保真） |
| `/api/v1/fs/{mkdir,rename,move,copy,delete,upload,archive/extract}` | POST | 新建目录 / 重命名 / 移动 / 复制 / 删除（物理删除，不可恢复） / 上传（multipart） / 解压到工作区 |
| `/api/v1/lsp/servers` | GET | 工作台**语言服务器清单**：本机探测到的 `servers`（语言 / 服务器标识 / 命令 / 参数）+ 已配置但未安装的 `missing` + 覆盖表解析问题 `errors`；`GEBAI_LSP=false` 或服务未注入时回 `enabled:false`（前端静默降级，不注册任何 provider） |
| `/api/v1/git/status` | GET | 仓库状态：`root` = 请求时给的**根 id**（回显）、`repoRoot` = **仓库根绝对路径**（子目录根只能靠它往上定位仓库）、分支、变更分组与计数。两个「根」曾经被写成同一个字段（`{ root: rootId, ...status }` 里 status 自带的 root 是仓库路径，展开后把 rootId 盖掉了）——客户端拿到的 `root` 与类型说明不一致且不报错，子目录根算不出前缀就是从这里开始的，现拆为两个明确字段 |
| `/api/v1/git/diff` \| `/git/compare` | GET | **任意两端差异/对比**（?root&from&to&path&mergeBase；端点：`WORKTREE`/`INDEX`/空串（配合 WORKTREE 表未暂存）/任意 rev） |
| `/api/v1/git/file-diff` \| `/git/content` \| `/git/show` | GET | 单文件差异 / 端点内容（Monaco 并列视图两侧文本）/ 指定提交文件内容 |
| `/api/v1/git/log` \| `/git/commit` \| `/git/file-history` \| `/git/blame` | GET | 提交日志（可路径/作者/关键字/时间范围过滤、正则与大小写开关、分页）/ 提交详情 / 单文件历史 / 逐行追溯 |
| `/api/v1/git/refs` \| `/branches` \| `/tags` \| `/remotes` \| `/stash` \| `/conflicts` | GET | 引用（分支/标签/最近提交/HEAD）/ 分支 / 标签 / 远程 / 储存（stash）列表 / 冲突四方内容 |
| `/api/v1/git/stage-hunks` \| `/unstage-hunks` \| `/discard-hunks` | POST | **按块/按行**暂存 / 取消暂存 / 放弃（`selections: [{hunk, lines?}]`，行序与 `GitFileDiff.hunks[].lines` 一致；`backup` 默认可关，前端传 `false`） |
| `/api/v1/git/stage-content` | POST | 把一份**任意内容**写入暂存区（三向暂存编辑器的保存动作；hash-object + update-index，不碰工作区与 HEAD） |
| `/api/v1/git/history-edit` | GET/POST | 查询未完成的编辑历史计划 / 按计划重放提交改写历史（`steps` 按**应用顺序**；冲突或 `edit` 停顿时计划落盘在仓库的 git 目录） |
| `/api/v1/git/history-edit/{continue,abort}` | POST | 继续未完成的编辑历史 / 中止并把分支恢复到改写前的 HEAD |
| `/api/v1/git/{stage,unstage,discard,commit,branch,tag,checkout,merge,rebase,cherry-pick,revert,reset,stash,remote,fetch,pull,push,init}` | POST | Git 写操作（统一「开关守卫 → 执行 → 审计」；破坏性操作默认先备份并返回可恢复引用；`GEBAI_GIT_WRITE`/`GEBAI_GIT_REMOTE` 可关） |
| `/api/v1/tools` | GET/PATCH | 工具集查询/启停配置 |
| `/api/v1/sub-agents` | GET | 子Agent 能力列表（名称、描述、工具、打包状态） |
| `/api/v1/webhooks` | GET/POST/DELETE | Webhook 注册/管理 |
| `/api/v1/tts` | POST | 语音朗读：`{ text, voice?, rate?, pitch?, volume? }` → `audio/wav` 字节流（不落盘；长文本按句分片合成后拼接，同文同参进程内缓存；平台无内置离线引擎时 503 并说明不做联网合成）；`/api/v1/tts/status` 探测可用性与缓存现状 |

- 认证方式：`Authorization: Bearer <token>`（用户令牌，登录获取）或 `Authorization: Basic base64(username:password)`（HTTP Basic 单次请求直验，等价隐式登录——复用密码校验与登录限流，不签发令牌，适合简单单次调用；**base64 非加密，须 HTTPS**）；无独立服务密钥
- 全端点支持 CORS，可通过环境变量配置允许的来源
- 会话操作与 WebSocket 共用同一套归属校验与隔离逻辑
- 消息发送与流式输出统一走 WebSocket：`sendPrompt` 经 WS `session.prompt` 发起任务，引擎事件经连接级订阅推送，SDK 转换为 `ChatChunk` 迭代返回（`wsEventToChunk`，与原 SSE 契约字段一致：文本增量/工具调用/审批请求/任务完成/错误）

#### 事件推送（Webhook）

业务系统可注册 Webhook（`/api/v1/webhooks`，GET 列表 / POST 注册 / DELETE 删除），服务端在关键事件（任务完成、审批请求、工具执行失败，默认三类，可自定义事件白名单）时向指定 URL 推送 JSON 事件，实现异步集成：

- **签名校验**：配置 `secret` 时推送头携带 `X-Gebai-Signature: sha256=<HMAC-SHA256(secret, body)>`，接收方验签防伪造；列表/注册返回的 secret 已脱敏（`***`）
- **重试策略**：投递失败（HTTP 非 2xx 或网络错误）自动重试，最多 3 次、指数退避（0.5s / 1s / 2s）
- **事件过滤**：按注册时的 `events` 白名单过滤；服务模式下事件按「会话归属」过滤——仅推送该 Webhook 注册者可访问会话的事件
- **存储**：配置持久化于 `{GEBAI_HOME}/webhooks.json`，重启保留；注册需校验 URL 为 http(s)
- 与 WebSocket 事件同源（均订阅服务端 EventBus），负载格式与 `AgentEvent` 一致：`{ type, sessionId, payload, timestamp }`

### 业务系统集成

面向其他前端与业务系统对接，提供多层次的集成方式：

- **官方 SDK（`@gebai/sdk`）**：WebSocket 连接管理 + REST 调用封装，开箱即用（`login`、`sendPrompt` 流式消费等）
- **OpenAPI 规范**：`/api/docs` 的**端点表由路由注册自动生成**（请求时遍历 `app.routes`，故注册顺序无关；只取 `/api/` 域、跳过 Hono 派生的 `HEAD` 与中间件 `ALL`，路径参数 `:id` → `{id}`），**摘要取自补充表**（未登记的端点仍列出并标注「未登记摘要」，不隐藏）——手写清单会与实现漂移（新增忘登记、删掉的仍列着），生成器保证「有哪些端点」永远与代码一致；响应含 `x-endpoints-total`/`x-summary-covered` 便于判断摘要覆盖度。业务系统可据此生成任意语言客户端（Java/Go/Python 等）
- **任意前端接入**：任何支持 WebSocket/HTTP 的前端（React/Vue/小程序/App 等）均可直接对接双通道 API，不绑定 UI
- **Web UI 嵌入**：内置 Web UI 支持 iframe 嵌入业务系统页面，可通过 URL 参数指定 UI 风格/自定义主题变量（`gb_style`/`gb_vars`/`gb_cny`）；**无「URL 参数携带令牌免登录」**——登录态只存浏览器本地（`localStorage`），跨系统免登录走外部身份兑换（`gb_ext_username`/`gb_ext_credential`）
- **接口认证**：REST 支持 `Authorization: Bearer <token>`（先登录获取令牌）与 HTTP Basic（单次请求直验账号密码，复用登录限流、不签发令牌）两种方式，WS 统一 `auth.login`；不提供独立服务令牌（原 `X-API-Key` 服务身份机制已移除）
- **外部身份扩展点（同源集成）**：服务模式下网站可复用自身登录态作为 GEBAI 用户——配置 `GEBAI_EXTERNAL_AUTH_*` 后，前端把本地登录态经 URL 参数（`?gb_ext_username=&gb_ext_credential=`）或 localStorage（`GEBAI_EXTERNAL_AUTH_STORAGE_KEY`，同源直读）交给 Web UI，Web UI 启动时自动调 `POST /api/v1/auth/exchange` 兑换令牌（HMAC 验签或 HTTP 回调验证，见「认证与鉴权」）；业务系统也可用 SDK `exchangeExternalUser` 自行对接（React/Vue 等任意前端），无需依赖内置 UI
- **身份对接**：服务模式下支持**外部身份兑换扩展点**（`GEBAI_EXTERNAL_AUTH_SECRET` HMAC / `GEBAI_EXTERNAL_AUTH_URL` 回调，见「多用户隔离与安全」），复用业务系统已有账号体系；**标准 SSO/OIDC 对接未实现**（列于「待实现」）
- **URL 携带提示词自动运行（`gb_prompt`）**：业务系统跳转链接可直接带任务进来——`?gb_prompt=<文本>`（URL 编码）在页面首屏就绪后自动**新建会话并发送该提示词**，随后把地址栏 `history.replaceState` 为会话地址（`?session=<会话 id>`，其余参数保留、`gb_prompt`/`gb_new` 移除）；刷新因此只打开该会话，**不会重复创建会话、重复执行任务**。`gb_new=1` 强制新建（同链接带 `session` 时也新建）；带 `session=<id>` 且会话存在时改为在该会话续接发送，该会话运行中则不抢占（重定向后把提示词回落输入框并提示）；建会话失败时提示词回落输入框且**不重定向**（刷新重试仍会执行）。关闭入口用独立配置文件的 `allowUrlPrompt: false`（默认开）；解析/重定向/编排见 `packages/web/src/url-prompt.ts`
- **前端独立配置文件（`gebai.config.js`）**：产物根的可选文件（`packages/web/public/gebai.config.js` 是带注释的模板），`index.html` 与 `files.html` 在模块脚本之前自动引入（相对路径，反代子路径下成立；文件缺失时静默跳过）——二开把「环境变量预置」与「宿主 localStorage → 歌白设置」写成配置即可接入已有系统，**无需改动上游源码**（升级时整文件原样保留）。通过 `window.__GEBAI_WEB_CONFIG__` 暴露：`env`（预置浏览器环境变量，随消息请求注入，与设置面板同一通道）、`envFromStorage`（环境变量 ← 宿主 localStorage 键，运行时读取）、`storage`（歌白设置键 ← 宿主键/字面值，仅在歌白键**未设置**时写入，`force: true` 才覆盖）、`allowUrlPrompt`。应用时机在页面初始化最早期（先于主题/低功耗/文件展示等读取 localStorage 的模块），优先级为 URL 参数 > 用户本次手动选择 > 本地存储既有值 > 配置文件 > 服务端全局配置 > 内置默认；读取与容错归一化见 `packages/web/src/boot-config.ts`（配置写错不使页面失效，按空配置处理）
- **审批集成**：审批请求可通过 REST/Webhook 转发到业务系统审批流，而非局限于内置 UI

### 飞书机器人集成

支持将 GEBAI 接入**飞书机器人**，用户可在飞书聊天中直接使用 Agent 能力（`GEBAI_FEISHU_BOT_ENABLED=true` 启用，需配置 `GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`）：

- **接入方式**：**长连接模式**（已实现）——服务端主动出站连接飞书，无需公网回调地址，本地桌面/服务端均可使用。协议为飞书现行 protobuf 帧协议（参照官方 SDK `lark_oapi`）：`POST /callback/ws/endpoint` 端点发现（`{AppID, AppSecret}`）→ WebSocket 连接（URL 携带 `device_id`/`service_id`）→ 心跳 ping（服务端下发 `PingInterval`，pong 可动态更新配置）→ 事件 DATA 帧（schema 2.0 JSON，`sum>1` 分片按 `message_id` 合包）→ 处理完回发 ACK 帧（`{"code":200}` + `biz_rt` 耗时）；**卡片回传交互（card.action.trigger，新版卡片回调）经事件帧（`type="event"`，schema 2.0，`header.event_type="card.action.trigger"`）下发**，`onEvent` 回调按事件类型分流：卡片回调同步快速处理并**返回卡片响应体**（toast/card）——ACK 封官方信封 `{"code":200,"data":"<base64(响应JSON)>"}`（对齐 lark_oapi ws/client.py：`resp.data = base64(JSON)`）；普通消息事件 fire-and-forget 立即回 `{"code":200}`；**更新卡片的 `card` 字段必须为官方包装 `{"type":"raw","data":{卡片JSON}}`**——裸卡片 JSON 判错误码 200672「响应体格式错误」，客户端按钮转圈不消、回调失败重推；3 秒内必须响应；旧版 `type="card"` 帧路径保留兼容（官方 SDK 新版对该帧直接丢弃）；`operator` 兼容新版扁平 `{open_id}` 与旧版嵌套 `{operator_id:{open_id}}` 两种形态；**断连韧性**——断连日志携带 close code/reason 与连接存活时长（区分服务端踢线/代理掐断/客户端判死）；判活时钟以「最近任意服务端帧」计（不只 pong，防 pong 丢失误判掐线造成周期性断连），静默超 `3x PingInterval` 主动重连（不依赖 onclose 回调，半开 socket close 可能无回调）；握手 15s 超时（防火墙黑洞下 open/error/close 均不触发，无超时会永久挂起）；重连节奏为断开后立即重连（`ReconnectNonce` 抖动）、失败后前 3 次快速重试（≤5s，网络闪断秒级恢复）再按服务端 `ReconnectInterval`、达 `ReconnectCount` 上限后降频 60s 续试不永久停摆。帧编解码为自研极简 protobuf 实现（`feishu-bot/pb.ts`），不依赖第三方 SDK；**启动时机**：长连接在服务监听建立之后异步发起（`boot/serve.ts`），握手不阻塞服务可用，失败只记 error 日志、服务照常运行
- **身份映射**：本地模式全部映射到 admin 用户（`AuthService.defaultUser`；**旧版默认用户为 `default`——升级后启动时自动迁移遗留归属与旧会话：`feishu/chat-owners.json` 内 `default` 归属改写为当前默认用户、`users/default/sessions/` 下飞书会话目录搬迁至 `users/admin/`、chat.json 的 `userId` 同步改写，幂等可重试、仅本地模式触发**）；服务模式按飞书用户 `open_id` 自动创建 GEBAI 映射用户（用户名 `feishu_{sha256(open_id)前24位}`——open_id 可含大写/超长，直接拼接过不了用户名白名单，哈希派生确定性防碰撞；随机密码不可密码登录，角色普通用户，管理员可在用户管理禁用），10 分钟内存缓存；**映射用户创建失败即中止任务**（绝不以默认 admin 兕底运行——那会让飞书侧任务以沙箱豁免身份执行）
- **会话映射**：每个飞书单聊/群聊自动关联独立 GEBAI 会话（**会话 id = `sha256("feishu:"+chat_id)` 前 32 位 hex**——满足存储层会话 id 白名单 `[0-9a-f]{32}`，`feishu_{chat_id}` 形态会被 `sessionPath` 拒绝；确定性派生，重启不变；会话名取飞书会话名称），消息上下文与 Web UI 完全互通；群聊成员共享同一会话（引擎身份为会话创建者，不因他人发言重建/覆盖）
- **消息互通**：文本消息（群聊自动剥离 `@_user_N` 提及占位）→ `session.prompt`；图片消息 → 下载为附件（魔数探测 mime）进入会话；任务完成发最终 **interactive 卡片**（JSON 2.0 markdown 渲染，超长截断并提示 Web UI 查看，**以「回复」引用原用户消息**）；开启 `GEBAI_FEISHU_BOT_NOTIFY_ASSISTANT` 时中间轮文本另发同构 markdown 卡片（见配置节）；最终回复与错误回复同为终态（`finalSent` 互斥标记），任务结束时尚未收到任何终态才补「✅ 任务完成」兜底（结果全在子 Agent/画图输出中的任务在飞书侧也有完成信号；出错/取消后不另发完成提示）；工具调用发「🔧 正在执行」状态消息（同任务至多一条）。**消息保留策略：机器人发出的所有消息一律不撤回**（预览/状态/滚动提示/审批与选择卡片均留痕），仅最终回复引用原用户消息；唯一例外是「Typing」表情反应（非消息本体，输出完成后移除）
- **画图（show 图表分支）后端渲染**：飞书通道不再依赖前端渲染，由桥接**后端直接渲染成图片**——`event.draw.render` 到达后，桥接调用**四语言组合渲染器**（`core/support/diagram-render.ts`：plantuml = `@plantuml/core` TeaVM 引擎本地渲染 SVG（零网络，浏览器 DOM API 以极简 shim 垫层运行）、mermaid = mermaid + happy-dom 垫层（getBBox 几何估算覆盖）、d2 = `@terrastruct/d2` WASM（二进制模式内嵌产物物化到 `{GEBAI_HOME}/vendor/d2js/{version}/`）、echarts = npm 包 SSR 渲染（`ssr:true` + SVGRenderer 零 DOM）→ `@resvg/resvg-js` 栅格化 PNG，浅色主题白底图，单一串行队列防引擎/全局环境冲突，超长按上限等比缩放）→ PNG 落盘会话 `tmp/{name}.png`（与源码文件并列，Web UI 文件面板可见）→ 上传飞书图片（`im/v1/images`，multipart）→ 发送 `image` 消息 → 经 `decideDrawResult` 回传引擎（成功才返回成功；失败把渲染错误回传模型供修正源码；`EngineBotAdapter.onDraw` 透传 format 字段）；**该渲染器同样供 Web 通道 `show` 图表分支 `render=backend` 复用**（引擎经 ToolContext `renderDiagram` 惰性加载，落盘 `tmp/{name}.png` 并返回 `image` 内容块）
- **选择（ask 选项询问分支）交互卡片**：`event.choice.request` 到达后发送**交互式按钮卡片**（选项按钮每行至多 5 个，`value` 携带 `choiceId`+`act`+选项值）替代前端选择卡；按钮点击经卡片交互帧回传——单选立即 `decideChoice`，多选切换勾选（点击回包 `card` 字段更新卡片按钮态与「已选」提示），「✅ 完成选择」提交勾选集合、「❌ 拒绝回答/放弃」提交拒绝；**已决策的卡片经 ACK 响应更新为终态**（「✅ 已选择：X」/「❌ 已放弃回答」，按钮不可再点；`card` 字段为 `{"type":"raw","data":…}` 官方包装）；**仅任务发起者可作答**（与审批授权一致，防群聊成员越权，他人点击回 toast 拒绝）；任务结束（完成/错误）清除待作答状态（卡片保留留痕，状态清除后按钮回调不再决策）
- **接口层桥接（多轮交互 + 仅最终回复）**：飞书 bot 不直接接触 AgentEngine/EventBus（不侵入引擎层），经 `BotPromptAdapter` 接口运行——固定 `interactionMode: "multi_turn"` + `outputMode: "final_only"` + **通道环境注记 `channelNote`**（`FEISHU_CHANNEL_NOTE`：引擎 `run` 选项通道无关地注入系统提示词[主提示词]——模型据此感知「当前对话经飞书机器人通道进行」：回复以 Markdown 卡片渲染且超长会截断（大段产物建议落盘并引导 Web UI 查看）、审批/选择经卡片按钮作答且用户可能回复 /approve 等命令、依赖前端页面的工具本通道不可用、图片自动转附件而其余富媒体不支持）；引擎事件流由 `EngineBotAdapter` 映射为语义回调（onApproval/onChoice/onDraw/onDone/onError/onEnd），**过程事件（工具调用状态、文本增量、推理、子会话运行过程）不推送**，回复仅最终消息（无打字机预览）；**收到消息先给用户消息添加「Typing」表情反应模拟「正在输入」（飞书开放平台无 typing 接口——实测 `POST /im/v1/messages/{id}/typing` 与 `POST /im/v1/chats/{id}/input_status` 均 404；改用 Message Reaction API `POST /im/v1/messages/{message_id}/reactions`，body `{"reaction_type":{"emoji_type":"Typing"}}`——`emoji_type` 必须为[官方表情文案说明](https://open.feishu.cn/document/server-docs/im-v1/message-reaction/emojis-introduce)中的标准 ID（`Typing` 为敲键盘表情，传 emoji 字符会报 231001），响应 `data.reaction_id` 为唯一标识；权限 `im:message` 或 `im:message.reactions:write_only` 任一即可），输出完成（最终回复/出错/兜底）后移除该反应（`DELETE /im/v1/messages/{message_id}/reactions/{reaction_id}`，表情反应非消息本体，不受消息保留策略约束），不发送额外表情/状态消息；**最终回复（interactive 卡片）、错误回复与任务完成兜底提示均以「回复」形式引用原消息发送（`POST /im/v1/messages/{message_id}/reply`，客户端显示引用气泡；message_id 不合法时回落普通发送）**；依赖实时前端的工具（`page_capture`，声明 `interaction: "realtime"`）由引擎按交互模式自动从模型 schema 中移除（含子Agent 命名空间同名工具 `{agent}_{tool}`），被调用时阻止执行并返回「当前通道不可用」说明（见「交互模式」）；`ask`/`show` 全模式可见（ask 选择/计划分支经飞书选择卡片作答、填值分支明确报错；show html 分支明确报错、图表分支经飞书后端渲染出图）；关键操作（requiresApproval）经审批交互卡片询问用户（见「审批交互卡片」）
- **交互命令**：`/help`、`/new`（清空当前对话上下文重建会话，**仅会话创建者可操作**）、`/sessions`、`/cancel`（**仅任务发起者可操作**）、`/approve`、`/reject`（批准/拒绝最近一个待审批工具调用，**仅审批发起者可操作**；审批卡片按钮的主路径兜底——卡片不可用/按钮失效时以命令完成决策（卡片保留留痕））、`/approval-skip`（**仅会话创建者可开启**——群聊成员不得为共享会话开启免审批；写入会话内存态 env，不落盘、服务重启后需重新开启）
- **审批交互卡片**：Agent 审批请求（`event.approval.request`，载荷含**调用参数 `arguments`**——飞书通道无消息流工具卡片，参数须随审批展示供用户判断）推送**交互式审批卡片**：工具名 + **参数摘要**（`formatApprovalArgs`：每参数一行 `key: value`，长值截断[单值 200 字符]+总长兜底 1200 字符防撑爆卡片，提示完整参数可在 Web UI 查看）+ 重复请求提示（`retries>0` 时标注「已被拒绝/超时 N 次」）+「✅ 批准 / ❌ 拒绝」按钮（`value` 携带 `approvalId`+`act`，与选择卡片的 `choiceId` 区分路由）；按钮点击经卡片交互帧回传——**仅任务发起者可操作**（他人点击回 toast 拒绝，与选择卡片/命令授权一致），决策经 `decideApproval` 回传引擎，**ACK 响应更新卡片为终态**（「✅ 已批准，任务继续」绿头 /「❌ 已拒绝，任务已取消」灰头，按钮不可再点；`card` 字段同样为 `{"type":"raw","data":…}` 官方包装）；已决策后再点忽略（幂等）；卡片发送失败回落纯文本命令指引（工具名 + /approve /reject）；新审批覆盖旧待审批（超时残留）时仅替换状态映射（旧卡片保留留痕——按钮回调经映射校验，失效 toolCallId 不会误决策）；任务结束（完成/错误/`/new`）清除待审批状态（卡片保留）
- **富文本（卡片 JSON 2.0）**：全部飞书卡片（最终回复/选择/审批及终态更新卡）统一 **JSON 2.0 结构**（`schema:"2.0"` 显式声明 + `config.update_multi`（卡片多次更新，多选卡逐次回包依赖）+ `header` + `body:{elements}` 包裹，`cardV2` 骨架统一构建；回调更新卡必须与原卡同结构——1.0/2.0 混用报 200830）；**2.0 的 markdown 组件支持完整语法**（1–6 级标题、GFM 管道表格（仅 2.0 支持，单卡最多 4 个、正文最多展示 5 行超出分页）、代码块（60+ 语言高亮）、引用、分割线、删除线等），回复 Markdown **原样透传无需降级转换**——旧 JSON 1.0 `lark_md` 不支持标题与表格（`#`/`|` 原样显示），2.0 升级后消除；**交互按钮同规则升级**：1.0 `action` 组件在 2.0 已废弃（报 200861 unsupported tag action），改为 `column_set`（`flex_mode:"flow"` 流式布局、列宽 auto 按内容收缩）横排承载 + 按钮 `behaviors:[{type:"callback", value}]` 声明回传（`card.action.trigger` 的 `action.value` 原样回传 value，与 1.0 顶层 `value` 同语义；按钮 `type` 枚举 `primary`/`danger` 在 2.0 仍有效）；低版本客户端（<7.20）对 2.0 卡片显示兜底提示；长内容截断附 Web UI 会话提示（会话文件下载依赖 Web UI 访问）
- **配置**：`GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`（与 `feishu_docs` 子Agent 共用全局凭证）+ `GEBAI_FEISHU_BOT_ENABLED`；行为开关三变量（均默认 false 保持现状）：
  - `GEBAI_FEISHU_BOT_NOTIFY_TOOLS`——**工具调用过程推送**：每个工具调用一条消息——开始发「🔧 名字(参数摘要)…」，结束时原地更新为「✔ 名字(参数摘要)（耗时）」（PATCH，消息保留不撤回）；并行工具按 `toolCallId` 各自独立精确配对；适配层 `EngineBotAdapter` 构造注入 `notifyTools`，`event.tool.call`（含 `arguments`）/`event.tool.result`（不受 outputMode 限制、始终发布）转发为 `onToolCall`/`onToolResult` 回调，bot 侧 `ChatOutbox.toolStart`/`toolFinish` 按调用管理消息（参数摘要 `formatToolArgs`：`key="value"` 空格连接，单值 80/总长 200 字符截断）；原地更新经 `PATCH /im/v1/messages/{id}`（`api.patchMessage`，仅同类型可更新），无权限/失败回落结束时发新消息（均保留）
  - `GEBAI_FEISHU_BOT_NOTIFY_ASSISTANT`——**助手中间消息推送**：带工具调用的中间轮过程陈述/阶段结论发 markdown 卡片消息（与最终回复同构——同一 `buildReplyCard`（JSON 2.0 markdown 完整渲染/超长截断），普通发送不引用原消息，保留不撤回；开启后不再发「✍️ 预览」文本）；引擎新增 `run` 选项 `notifyIntermediate`（与 outputMode 正交——final_only 通道也可感知过程），中间轮文本在持久化后随 `event.message.intermediate` 发布（载荷含 `toolCalls` 本轮工具调用数，仅非空文本发布），适配层转发为 `onIntermediate` 回调，bot 侧 `ChatOutbox.assistantNote` 每轮直接发卡（无节流链路——中间轮为轮级事件非字符流）
  - `GEBAI_FEISHU_BOT_AUTO_APPROVE`——**自动审批工具**：需审批工具自动通过（审批卡片不再弹出）；适配层透传引擎任务级 `autoApprove: true`（`approvalPolicy=auto`，含服务模式——部署方为飞书通道整体担责，与映射用户自设审批跳过等价）；仅建议内网/低风险部署开启，公网部署保持审批卡片交互
- **飞书 TLS 策略**：`GEBAI_FEISHU_INSECURE_TLS=true` 时**所有飞书出站请求禁用 TLS 证书校验**（内网代理/中间人证书场景）——机器人桥接 REST 请求（token/消息/图片，`feishu-bot/api.ts`）、长连接 endpoint 发现与 WebSocket（`feishu-bot/conn.ts`，Bun WebSocket `tls.rejectUnauthorized=false`）、`feishu_docs` 子Agent 全部接口与 OAuth 兑换/用户信息（`feishu-bot/tls.ts` 共享 `feishuFetch`/`feishuWsOptions` 助手，注入 `fetch`/`WebSocket` 的 `tls.rejectUnauthorized=false`；仅在可信内网开启）
- **安全**：长连接为出站连接（不暴露回调端口）；凭证仅存环境变量（本地 `.env` 不入版本库）；服务模式按飞书用户隔离数据与权限（映射用户为普通角色）；消息/会话 id 白名单校验（`[A-Za-z0-9_-]{1,64}`）防路径注入；卡片按钮回调的 chatId/openId 同样白名单校验 + 发起者授权；**事件处理异常全程捕获**（async 回调 rejection 不得成为 unhandled rejection）；会话归属/映射用户缺失时中止而非兜底
- **实现**：`packages/server/src/feishu-bot/`（`pb.ts` protobuf 帧编解码、`protocol.ts` 帧协议/合包/ACK、`conn.ts` 长连接客户端（事件 + 卡片交互帧；判活/握手超时/快速重试断连韧性）、`api.ts` 开放平台 API（token 缓存/消息收发/图片上传下载/会话信息）；TLS 策略助手已迁 `@gebai/agents` core/shared/tls.ts（`GEBAI_FEISHU_INSECURE_TLS` 禁用证书校验，机器人/feishu_docs/OAuth 共用，server 侧经包导入）、`plantuml.ts` 后端渲染器（DOM shim + TeaVM 引擎 + resvg）、`bot.ts` 桥接编排（依赖全部注入））；单测 129 用例（真实凭证握手验证通过）；Webhook 回调模式（公网 HTTPS + 签名验签）为后续迭代

### 子Agent文件格式

**单文件形式**（`packages/agents/src/agents/{name}.ts`）：

```ts
export const def: SubAgentDef             // ★ 契约主体：加载器只认 mod.def（未导出即报「须 export const def: SubAgentDef」）
// SubAgentDef 字段：name / description / systemPrompt / tools / requiresApproval? / preload? /
//   dependencies? / envVars? / writeGuard?（工具结构化输出契约写在各 Tool 上：Tool.outputSchema，JSON Schema）
export const name: string                 // 可选：便捷导出
export const description: string
export const systemPrompt: string
export const tools: ToolSet               // 子Agent 自有工具（注册为 {agent}_{tool}）
export const requiresApproval?: Record<string, boolean>
export const preload?: boolean            // 是否预加载（默认 false，按需装载）
export const dependencies?: string[]     // 依赖的其他子Agent 名单（装载/预加载/subsession_run 自动连带装载，
                                         // 工具与提示词按依赖方自身命名空间复用，见「子Agent 依赖与自动装载」）
// 工具级安全模式自主声明（Tool.safeMode，写在各工具定义上）：
//   true  = 作者判定安全模式下可提供（即使短名风险如 xxx_sh——须自行保证实现只读或体内按 ctx.safeMode 校验）
//   false = 作者判定安全模式下不提供（即使名字无风险命中，如内部写文件/外发请求的工具）
//   未声明 = 按短名风险规则默认（{agent}_sh / {agent}_write / {agent}_file / {agent}_delete / {agent}_task_add 等 _risk 后缀命中则不注册）
// 注册期过滤（ToolRegistry({safeMode})）：不注册即 schema 不可见、调用报未知工具，见「安全模型 → 安全模式」
export const envVars?: EnvCatalogVar[]    // 可配置环境变量声明（{AGENT_NAME_UPPER}_ 前缀），汇总进环境变量目录（前端面板白名单，见「环境变量配置」）
export const writeGuard?: (env: Record<string, string>, absPaths: string[]) => string | null
// 写范围守卫声明：会话装载本子Agent（或子会话预加载）后注入 ToolContext.writeGuard——
// 文件写类工具（write/edit/patch/file（rename/move/delete））写入前以解析后的绝对路径调用，
// 返回非空字符串 = 拒绝写入（self_optimize 用它实现「核心引擎源码默认只读」的代码级强制）
```

**目录形式**（`packages/agents/src/agents/{name}/{name}.ts` + 可选 `{name}.md`）：

定义入口按优先级探测：`{name}/{name}.ts` → `{name}/index.ts` → 纯 md。双入口共存时 `{name}.ts` 优先，`index.ts` 静默忽略（不报错）——优先级规则由测试锁定（subagents.test.ts）。

```ts
import systemPromptBase from "./{name}.md"   // Bun 原生 .md 文本导入（bunfig.toml 统一为 text loader）

export const systemPrompt = systemPromptBase
  .replaceAll("{{PLACEHOLDER}}", "替换值")   // 修饰一：占位替换
  .concat("\n补充内容……")                     // 修饰二：内容补充（md 之外追加动态说明）
```

- 目录形式下定义入口固定为 `{dir}/{dir}.ts`，系统提示词可拆为同目录 `{name}.md` 独立维护；ts 文件 import 后自由修饰（占位替换 `{{...}}`、内容补充、条件拼接），修饰完全由 ts 控制，加载器不干预
- 目录内其他辅助文件（如 `tools.ts` 工具集）由入口文件自行 import，仅入口文件被扫描收录
- `description`：总Agent 根据描述判断何时调用该子Agent，是子Agent 能力被发现的关键
- `preload`：声明该子Agent 默认预加载（模块语义，属「装载」的启动期形态：工具与能力描述立即注入总Agent）；未声明者通过 `agent_load` 按需装载，也可用 `GEBAI_PRELOAD_SUB_AGENTS` 统一声明
- 工具的结构化输出契约写在各 `Tool` 上（`Tool.outputSchema?: ToolSchema`，JSON Schema），经 `tool_schemas` 工具批量暴露——**无 `toolSchemas()` 导出、无 `returns` 字段**
- `subsession_run` 派生子会话：输入为 `agents`（子Agent 列表）+ `input`（任务文本），返回为 `output`（文本），隔离子会话（预加载子Agent）自行规划执行（默认阻塞执行返回；`async:true` 后台异步执行，`bg_task` 管理，见「子会话运行」）；继承形态则 fork 父上下文并把报告自动合入父会话
- 子Agent 内无全局可变量，工具函数体为纯函数或依赖注入工厂，天然支持并发加载
- **复用其他子Agent 的能力用 `dependencies` 声明**（装载/预加载自动连带，工具以依赖方 `{dep}_` 命名空间注册——如 reverse_site 依赖 playwright、self_optimize 依赖 code），不在 `tools` 中展开依赖方的工具实现（旧形态会产生重复注册与审批映射复刻）；机制上 `tools` 仍可直接引用全局工具实现（如 `read`/`write`/`sh`，以 `{agent}_` 前缀暴露），但按「子Agent 只声明独有工具」约定应直接用全局名，不重复声明
- 示例：内置 `desktop` 即目录形式（`packages/agents/src/agents/desktop/desktop.ts` + `desktop_tools.ts`）

**纯提示词简化定义**（`packages/agents/src/agents/{name}/{name}.md` 单独存在，零 TS）：用于简单子Agent 与组合式子Agent

```md
---
description: 一句话能力描述（可选；缺省取正文首行）
dependencies: playwright, code（可选；依赖的子Agent 名，逗号分隔——装载/预加载自动连带装载）
preload: true（可选；启动即装载）
env_vars:（可选；可配置环境变量声明，变量名须以 {NAME 大写}_ 前缀，汇总进前端环境变量面板）
  - name: MY_AGENT_TOKEN
    description: 访问令牌
---
系统提示词正文（说明编排策略/角色职责等）
```

- 目录内无同名 `{name}.ts` 时，加载器直接由 md 构成定义（`name`=目录名）；frontmatter 识别 `description`/`dependencies`/`preload`/`env_vars`（条目须符合命名规则 `[a-z0-9_]+`，非法条目过滤；`env_vars` 的变量名须以 `{NAME 大写}_` 前缀，非前缀条目忽略）——组合式子Agent 声明依赖后编排提示词可直接引用依赖方 `{dep}_` 工具，无需运行时先 `agent_load`；`preload`/`env_vars` 使零 TS 定义具备与 TS 目录形式同等的启动期/面板能力
- 无工具的简化定义在子会话运行环境**自动注入编排工具**（`agent_list`/`agent_load`/`subsession_run`/`bg_task`，原名暴露、无 `{agent}_` 前缀），支持组合式子Agent 编排装载/派生子会话其他子Agent（受递归深度 3 层限制）
- 有工具的子Agent 需要编排能力时，可显式引用导出的 `agentListTool`/`agentLoadTool`/`subSessionRunTool`/`subSessionMergeTool`/`bgTaskTool`（`core/tools/` barrel），注册为 `{agent}_subsession_run` 等带前缀形态
- 示例：纯 md 组合子Agent 即 `packages/agents/src/agents/{name}/{name}.md`（零 TS），编排其他子Agent 产出完整链路的组合能力可由此模式定义

## 桌面端架构

基于 **tao + wry（系统原生 WebView：Windows WebView2 / macOS WKWebView / Linux WebKitGTK）** 实现桌面应用，复用服务端内置 Web UI，无需额外前端框架、无 Tauri 框架依赖。产物为两个独立可分发的 exe：

- **`dist/gebai.exe`（纯 Bun 单文件，浏览器形态）**：`bun build --compile` 产物（入口 `src/index.ts`），同进程启动服务端并自动打开系统默认浏览器（`GEBAI_NO_OPEN=1` 时不打开）；Web UI、子Agent、D2.js、playwright driver 与 playwright-core 等全部内嵌，单文件即可运行，也可作为服务端直接部署（`--server`）
- **`dist/gebai-desktop.exe`（原生 WebView 启动器）**：Rust 单文件（`launcher/src/main.rs`，约 230 行），构建时经 `include_bytes!` **内嵌整个服务端二进制**；启动时物化到用户数据目录（Windows `%LOCALAPPDATA%\{app}\gebai-server.exe`——**长度先行短路，长度相同再比 SipHash**，一致即跳过重写）并 spawn（`GEBAI_NO_OPEN=1` + `CREATE_NO_WINDOW`），从侧车 stdout 解析监听端口（`[gebai] listening on http://HOST:PORT`），WebView 导航到 `http://127.0.0.1:{port}`；窗口关闭（`CloseRequested`/`LoopDestroyed`）时一并 kill 侧车；WebView 用户数据目录固定在 `%LOCALAPPDATA%\{app}\webview\`（不在 exe 旁落地）；WebView 经初始化脚本注入 `window.__GEBAI_DESKTOP__` 桌面形态标记，Web UI（`state.isDesktopApp`）据此在下载（文件卡/图表图片/HTML 源码/会话导出）时以 toast 提示进度位置与保存位置——WebView 内下载指示不明显，浏览器形态有自身下载指示故不提示。**场景变体参数化**（`launcher/build.rs` 构建期环境变量注入，`rustc-env` 烘焙进 `main.rs`；缺省值 = 完整桌面端，行为不变）：`GEBAI_LAUNCHER_SERVER_EXE`（内嵌的服务端二进制路径，缺省 `dist/gebai.exe`）、`GEBAI_LAUNCHER_APP_NAME`（应用标识——物化数据目录名/非 Windows 同目录服务端文件名/WebView 配置目录，缺省 `gebai`）、`GEBAI_LAUNCHER_TITLE`（窗口标题与 exe 资源 ProductName，缺省「歌白」）、`GEBAI_LAUNCHER_PORT`（可选固定端口：spawn 时设 `GEBAI_PORT`，变体与完整桌面端各占独立端口互不冲突且 localStorage origin 各自稳定，缺省不指定）
- **单文件自带 UI**：构建时 `packages/server/scripts/build-web-bundle.ts` 把 `packages/web/dist` 全量资源 base64 内嵌为 `web.bundle.generated.ts`（gitignore），随服务端编译进二进制；二进制模式（`webDist` 不存在）静态路由改从内嵌资源提供，`/` 与 `/assets/*` 均可访问——单文件二进制完整可用（含 Web UI + 全部子Agent）；脚本调试模式该文件缺失时服务端自动回退（不内嵌 UI），无需预先构建
- 桌面端与浏览器访问共用同一套 Web UI、同一套服务端核心，仅宿主不同
- **端口固定（桌面形态）**：桌面形态（`gebai-desktop.exe` 侧车与 `gebai.exe` 浏览器形态）默认固定端口 **47896**（`DESKTOP_PORT`，不易冲突——避开常见开发端口与 Windows 临时端口段 49152+）。核心目的是保持 origin（协议+主机+端口）稳定：浏览器/WebView 的 localStorage 按源隔离，端口每次随机变化会使环境变量（`gebai.ui.env`）、主题、快捷键等全部浏览器本地数据跨重启「凭空丢失」（用户环境变量仅存浏览器本地，服务端零留存，无兜底可恢复，见「环境变量隔离」）。端口被占（如重复启动）**直接报错退出，不回退随机端口**——随机 origin 正是数据丢失的根因；显式 `GEBAI_PORT` 或调用方 overrides 优先。服务端部署形态仍默认 3000
- 数据目录 `GEBAI_HOME` 默认 `~/.gebai/`；侧车物化采用**内容哈希比对**（长度相同再比 SipHash，防同尺寸不同内容的陈旧残留）；两种桌面形态（浏览器形态 exe 与启动器侧车）同为二进制模式，启动时加载 `{GEBAI_HOME}/.env` 作为启动配置（真实环境变量优先，见「启动参数与环境变量」）——模型密钥等配置放 `~/.gebai/.env` 即可生效，无需设置系统环境变量

### 构建与产物

- 构建链：`bun run --cwd packages/desktop build` → `server:build`（图标生成 → web 构建 → web bundle → 子Agent bundle → D2.js 内嵌 → driver 内嵌 → playwright-core 内嵌 → CV 运行时与模型内嵌 → CV sidecar 驱动内嵌 → `--compile` 产出 `dist/gebai.exe`）→ `launcher:build`（cargo release 编译 `launcher/` 并复制为 `dist/gebai-desktop.exe`）
- **品牌图标管线**：canonical 源为 `packages/web/public/favicon.svg`（大脑造型）；`packages/desktop/scripts/gen-icon.ts` 用 resvg 渲染多尺寸打包为 `icons/icon.ico`（gitignore，16/24/32/48/64/128 无压缩 BMP + 256 PNG），并回写 `packages/web/index.html` 的内联 favicon data URI。exe 嵌入**不走 bun `--windows-icon`**（其会改动像素/alpha，与 rcedit 同样不可用）：`gebai.exe` 由 `packages/desktop/scripts/embed-icon.ts` 经 kernel32 `UpdateResource`（bun:ffi）原样写入 RT_GROUP_ICON+RT_ICON 资源（32px 条目排首位，兼容简化提取器）；`gebai-desktop.exe` 经 `winresource`（`launcher/build.rs`，`rerun-if-changed` 聟动重建）——网页 favicon 与两个 exe 图标同源。**运行时窗口图标**（任务栏/标题栏）不属于 exe 资源，需窗口类显式设置：gen-icon 同时产出 `icons/icon32.rgba`（32px 原始 RGBA），launcher `include_bytes` 内嵌后经 tao `with_window_icon` 挂到窗口
- 产物：`packages/desktop/dist/gebai.exe` 与 `packages/desktop/dist/gebai-desktop.exe`（`launcher:build` 依赖 `dist/gebai.exe` 已存在，故构建顺序固定）
- 要求：Rust 工具链（仅启动器需要）、WebView2 运行时（Windows 自带）；`gebai.exe` 单独分发时无需任何 Rust 依赖
- 浏览器桥接驱动 `@gebai/agents` core/browser/driver.mjs（playwright/reverse_site 子Agent 与透明浏览器代理共用）：服务端 dist（非编译）形态由 `packages/server/scripts/build-subagents.ts` 复制到 dist/ 与入口同目录；二进制形态由 `packages/server/scripts/build-driver-embed.ts` 生成内嵌产物 `driver.embedded.generated.json`（gzip base64），运行时物化到 `{GEBAI_HOME}/vendor/playwright/driver.mjs`（桥接仍需运行机器具备 node；playwright 模块源码/部署形态解析 node_modules，二进制形态用内嵌 pwcore 产物）
- 本地 CV 运行时与模型 `@gebai/agents` core/cv/cv.embedded.generated.json（desktop 子Agent 小模型识别）：`packages/server/scripts/build-cv-embed.ts` 把 onnxruntime-web dist 两文件（入口 mjs + wasm 本体）与 PP-OCR 模型三件套（det/rec ONNX + 字典，构建时从 `GEBAI_CV_MODEL_BASE` 下载或取 `{GEBAI_HOME}/resources/models/cv/ocr` 已有自备）整包 gzip base64 内嵌，运行时释放到资源目录——模型 → `{GEBAI_HOME}/resources/models/cv/ocr/`、ort 运行时 → `{GEBAI_HOME}/resources/vendor/cv/`（单二进制形态下 ort 与模型均不可依赖 node_modules，必须内嵌；源码/部署形态 ort 解析 node_modules、模型走 `{GEBAI_HOME}/resources/models/cv/ocr` 或 `GEBAI_CV_MODELS_DIR`；下载失败生成空清单不阻断构建，运行时给配置指引）；**模型资产一律不入 packages 源码树**（集中放资源子仓库 `resources/`，获取方式：`bun run resources:download` 按清单 `scripts/resources.manifest.json` 拉取，或克隆资源子仓库）
- CV GPU sidecar 驱动 `@gebai/agents` core/cv/cv-driver.mjs（检测分层后端，见「小模型识别」）：`packages/server/scripts/build-cvdriver-embed.ts` 生成内嵌产物 `cvdriver.embedded.generated.json`，运行时释放到 `{GEBAI_HOME}/resources/vendor/cv/cv-driver.mjs`；dist 形态由 build-subagents 复制到产物目录。驱动脚本本身仅数 KB 随构建内嵌；**onnxruntime-node 依赖不内嵌**（体积/许可）——运行机安装该依赖或设 `GEBAI_CV_ORT_NODE_DIR` 指向其目录即启用 GPU 推理，缺省回落 wasm（保留现有方案兜底）；sidecar 需运行机具备 node（与 playwright 桥接同前提）

### 启动方式

- 浏览器形态：直接运行 `gebai.exe`，自动打开系统浏览器访问本地服务
- 桌面形态：运行 `gebai-desktop.exe`，启动器物化并拉起内嵌服务端，原生 WebView 窗口加载内置 UI；核心逻辑与浏览器/服务端部署完全一致，仅宿主不同

- 关闭窗口时进程退出，内嵌侧车服务端一并退出（除非以服务端模式单独运行）

## 构建与发布

### 最终构建目标

单二进制可执行文件，通过 `bun build --compile` 产出，支持 Windows / Linux / macOS 三平台。同一二进制承载全部运行形态，分发物仅一个文件，**服务本体零运行时依赖**（Bun 运行时已内嵌，宿主机无需安装 bun/node/python 即可运行服务本身；脚本工具的解释器要求见「脚本执行环境」，playwright 桥接与 CV 边车需宿主 node）：

- 本地桌面应用（WebView 宿主）
- 本地浏览器访问（服务端 + 浏览器）
- 服务模式（多用户公用）

Vite 构建产物（Web UI）、桌面端 WebView 宿主、子Agent 代码一并编译进二进制，运行时按启动参数/环境变量切换形态，无需区分构建，也无需在运行时读取子Agent 文件。

构建时支持**子Agent 选择性打包**（白名单/黑名单，见「子Agent」章节），不同发行规格（桌面全量版、服务端精简版）由同一源码产出：

```
GEBAI_BUILD_SUBAGENTS=a,b bun run --cwd packages/server build   # 仅打包指定子Agent（依赖闭包自动展开）
GEBAI_BUILD_PRELOAD=a bun run --cwd packages/server build      # 烘焙 def.preload=true
bun run --cwd packages/server build                            # 全量（缺省）
# 注：选择性打包是**构建期环境变量**，无 --sub-agents/--exclude-sub-agents CLI；
#     运行时黑名单用 GEBAI_SUB_AGENTS_ENABLE/DISABLE（见「子Agent 启停名单」）
```

### 运行模式区分

代码区分运行模式以 **`package.json` 是否存在**判定二进制形态（`isBinaryMode()`），并据此定 `GEBAI_HOME`（二进制 → `~/.gebai/`，源码 → 仓库根）：

| 模式 | 判定方式 | GEBAI_HOME | 子Agent 来源 |
|------|---------|------------|-------------|
| 脚本调试 (`bun run dev:*`) | `Bun.argv` 含源码路径 | 项目根目录 | `packages/agents/src/agents/` 源码直接引用 |
| 二进制运行 | 无源码路径，编译产物 | `~/.gebai/` | 编译时打包进二进制 |

二进制运行形态（本地模式 / 服务模式）由启动参数与 `GEBAI_MODE` 等环境变量决定，不影响二进制构建。

### 容器镜像（服务模式）

以 Ubuntu 24.04 为基础镜像的多阶段构建（`Dockerfile` + `docker/build.sh`/`build.ps1`，用法与边界见 `docker/README.md`）：构建阶段装依赖并跑完整构建链（Web UI 产物、子Agent/工具注册表、D2.js、tree-sitter wasm、playwright 驱动与 pwcore、内置 ripgrep、可选本地 CV），再 `bun build --compile` 产出单文件 Linux 可执行；运行阶段只有系统库加该二进制，**不含 node_modules、不含 bun**。镜像默认 `GEBAI_MODE=server` + `GEBAI_HOME=/data`（挂卷），因此路径沙箱与会话目录脚本隔离均强制开启。系统依赖只为明确用途而装：tini（PID 1 收尸）/ git / python3（py 工具、vision_pip）/ bubblewrap（隔离的文件系统层）/ fonts-noto-cjk（PDF 与图表中文）/ curl（健康探针）/ tzdata（定时任务）。

- **架构限制**：二进制内嵌的 `@resvg/resvg-js` 是平台原生模块，跨架构编译会嵌错平台——镜像仅支持「构建机架构 = 目标架构」。
- **脚本文件系统隔离（bubblewrap）的容器前提已实测**：默认 seccomp 下容器内无法创建 user namespace（`unshare: Operation not permitted`），bwrap 不可用——此时自动降级为环境收敛（HOME/TEMP/XDG 仍在会话目录内）；`--cap-add SYS_ADMIN` **不够**（unshare 可用但 bwrap 卡在 `pivot_root: Operation not permitted`）；`--security-opt seccomp=unconfined` 实测可用（系统只读、仅会话目录可写、宿主家目录不可见）。
- **能力边界**：`desktop`（宿主桌面操控）在服务模式一律不可用；`tts_speak` 仅 Windows；客卿子Agent 在服务模式整体禁用；`reel` 需 Node/ffmpeg/Chrome（未预装）；浏览器类子Agent 需 `--with-browser` 构建；容器内重启用 `docker restart`（而非 `restart_server` 工具）。

架构与体积：`linux/amd64` 实测构建产出的镜像 867MB（二进制 244MB），构建耗时主要在前端构建与依赖安装（首次约数分钟，缓存后秒级）。

### 升级与兼容

- **原地升级**：新版本二进制直接替换旧文件重启即可，数据（`GEBAI_HOME`）与配置（环境变量）无需迁移
- **数据兼容**：**存储格式未设版本字段**——兼容靠一次性迁移（`users/default/` → `users/admin/` 目录搬迁、飞书 `default` 归属改写）与「新字段可选、旧数据缺省」的读侧容错
- **API 兼容**：`/api/v1` 语义化版本，破坏性变更升级主版本号；WebSocket 消息类型保持向后兼容（新增类型不影响旧客户端）
- **多实例共处**：单机可并排部署多实例（不同 `GEBAI_PORT` / `GEBAI_HOME`），便于灰度与多租户；容器形态同理，但同一数据卷只应有一个实例（调度主实例锁在同一数据根上互斥）

## 实施路线

按依赖顺序分阶段交付，每阶段可独立验证：

**阶段一：核心闭环（MVP）**
- 服务端骨架：Hono + WS/REST 单端口 + `SessionStore`（分片）+ `EnvManager`（全局 + 会话内存态两层合并）
- `LLMProvider`（OpenAI 兼容接口 + 流式解析 + 能力声明）与 `AgentEngine` 主循环
- 全局工具：`read`/`write`/`sh`/`py`/`flow`/`current_time`/`system_info`
- 审批流 + 工具执行/渲染 + 上下文截断保护
- 会话 CRUD + `session.prompt` 流式 + SDK 基础 + 最小 Web UI（聊天/会话列表）

**阶段二：子Agent 与自我优化**
- 子Agent 装载器（定义扫描/命名空间与前缀解析/碰撞校验/选择性预加载）
- `code` / `self_optimize` + 待办跟踪 + 反馈

**阶段三：多用户与安全**
- 认证/令牌/用户管理 + 路径沙箱 + 限流 + 多模态附件

**阶段四：集成与分发**
- 单二进制构建（Web UI + 子Agent 打包）+ 桌面端 WebView + 反向代理子路径挂载（前端按页面 URL 相对解析，免配置）
- 多套 UI 风格 + 上下文自动压缩 + 飞书机器人 + Webhook

**阶段五：完善**
- 数据生命周期/GC、升级兼容、自我优化产物管理、多实例

### 待实现（已知项，后续迭代）

| 待实现项 | 现状与影响 | 计划方案 |
|---------|-----------|----------|
| 服务端消息分页 | 会话消息**全量加载**（`session.get` 返回全部正文），前端已做 **DOM 窗口化**：DOM 只保留视口附近的块（见「消息区窗口化」），布局/渲染开销不随历史条数增长——未做的是**传输侧**分页，长会话打开仍一次性传输完整正文（网络与解析成本随历史增长；已渲染块容器的 JS 引用也常驻，用于重挂零重建） | ① `session.get` 支持窗口/游标参数 + 前端上滚按需拉取更早页，替代一次性返回全部正文（窗口化已具备按需挂载路径，接上「未加载区间」即可）；② 极端长会话的已渲染节点 LRU 释放（需重建渲染 + 运行态引用迁移） |

## 测试策略

统一使用 `bun test`，测试文件与被测代码同目录（`*.test.ts`）。

```bash
bun run --cwd packages/server test
bun run --cwd packages/sdk test
```

### 测试执行速度

全套测试是 AI 编码迭代与 `self_optimize` 优化闭环的准入凭证，执行速度直接影响迭代效率，三项结构化保障：

- **server 分片并行**：`packages/server` 的 `"test"` 脚本走 `packages/server/scripts/test-parallel.ts`——全部 `*.test.ts` 按**文件字节数降序的最长处理时间优先（LPT）装箱**分成 N 个分片并行启动 `bun test` 子进程（bun test 单进程内测试文件串行执行，是全套件耗时主因；测试耗时与文件规模强相关，轮转分配会把大文件堆在少数分片形成长尾）。默认 `min(8, max(2, CPU 核数))` 分片，`--shards=N` 参数或 `GEBAI_TEST_SHARDS` 环境变量覆盖；带文件路径/`-t`/`--coverage` 等参数时自动退回单进程透传（定向运行分片无收益）。输出逐行加 `[i/N]` 前缀流式透传。`test:serial` 保留串行入口
- **失败复验（区分并行抖动与真失败）**：任一分片失败时，该分片文件列表**单进程串行重跑**——重跑通过判定为并行抖动（跨分片共享资源竞态，机器满载下真实 spawn 类用例也会触达用例超时），以 `⚠` 警告列出并给出精确复现命令、整体视为通过；重跑同样失败才是真失败（非零退出）。避免把环境/竞态抖动误当代码回归反复排查
- **测试进程环境净化**：`bunfig.toml` 的 `[test] preload` 挂载 `packages/server/scripts/test-preload.ts`，启动时清除全部 `GEBAI_`/`CODE_` 前缀变量——Bun 在 `bun test` 时按 cwd 自动加载 `.env`（仓库根 `.env` 的 `GEBAI_APPROVAL_SKIP`/`GEBAI_SELF_MODIFY`/`GEBAI_LLM_*` 会直接改变引擎审批、写守卫、Provider 解析等断言结果），宿主 shell 亦可能残留调试变量；配套 `loadConfig` 的 `loadDotEnv` 在 `NODE_ENV === "test"` 时跳过读仓库 `.env`（防用例内首次 loadConfig 重新注入）。测试结果不依赖开发者本地配置
- **跨平台用例约定**：涉及平台分支的用例显式注入平台参数（如 `restart_server` 的 `platform: "win32"`），不随宿主平台漂移；安全判定对 Windows 形态绝对路径（盘符/UNC）在非 win32 平台一律按越界拒绝（fail-closed）
- **turbo `test` 不依赖 `^build`**：各包 `main` 均指向 `src/*.ts`（bun 直接执行 TS 源码），测试无需先构建依赖包——冷缓存/改动后跑测试不再先付 vite 构建 + wasm 内嵌 + `bun build --compile` 的构建成本
- **构建产物幂等（内容不变不写盘 + 跨平台确定性）**：`packages/server/scripts/build-*.ts` 统一经 `write-if-changed.ts` 落盘——内容与磁盘一致则跳过写入，`typecheck`/`build` 反复重跑不再刷新已提交生成产物的 mtime 与字节（此前跑一次 typecheck 工作区就变脏，AI 每轮都要先确认「是否既有改动」）；内嵌产物 gzip 经 `gzip-deterministic.ts` 固定头部 XFL/OS 字节（zlib 默认随平台变化，同一源码在 Windows/Linux 生成不同字节）；`src/types/generated-json.d.ts` 通配声明 `*.generated.json`（`@gebai/server` 与 `@gebai/agents` 各一份），构建产物缺失时 tsc 不再报 TS2307（运行时仍走各模块「缺失→回退/引导」分支）
- **子Agent 发现进程级缓存（目录签名校验）**：`SubAgentManager.discover()` 首次扫描 `packages/agents/src/agents/` 后缓存定义列表与**目录签名**（递归 `路径:mtime` 拼接，~30 次 stat），测试中每个用例新建 manager 重复 discover 时签名未变直接水合缓存、跳过目录扫描/动态 import；签名变化（新增/修改/删除子Agent 文件）即失效重扫（热加载，见「子Agent 热加载」）

### 测试分层

| 层级 | 覆盖范围 | 要求 |
|------|---------|------|
| 单元测试 | 工具函数、命名空间解析、环境变量合并、路径沙箱、分片路径、截断/压缩算法 | 核心模块**必须**单测，零外部依赖（mock 注入） |
| 集成测试 | AgentEngine 主循环、审批/重试状态机、子Agent 调用、会话持久化 | 用 mock LLM Provider 跑完整流程，不触真实网络 |
| 契约测试 | WS/REST 消息格式、SDK 与协议一致性 | 协议变更必须有契约测试兜底 |
| E2E | 端到端（mock LLM + 内存存储）：prompt → 工具调用 → 审批 → 完成 | 阶段一闭环后建立，回归主路径 |

### 自测工具：fake-llm（假模型端到端自测）

`packages/server/scripts/fake-llm.ts`——无真实模型 Key 的**手工端到端自测**工具：按场景脚本回放 OpenAI `chat/completions` 流式响应（文本分片 / tool_calls 分片 / usage），服务端 provider 无感接入，用于浏览器实测前端行为（流式渲染、subsession_run 执行过程、计划审批滚动等单测覆盖不到的 UI 链路）。

```bash
bun run --cwd packages/server fake-llm [场景]   # 场景缺省 = text；端口 9801（FAKE_LLM_PORT 覆盖）
# 服务端指向假模型联调：
GEBAI_LLM_API_BASE=http://127.0.0.1:9801/v1 GEBAI_LLM_API_KEY=test \
  GEBAI_LLM_MODEL=fake GEBAI_PORT=3900 bun run --cwd packages/server dev
```

| 场景 | 脚本 | 验证点 |
|------|------|--------|
| `text` | 单轮纯文本回复 | 链路冒烟（消息上屏/流式渲染） |
| `subsession_run` | 主会话调 `subsession_run` → 子会话两轮（文本 + `code_system_info` 工具 / 多行结论）→ 主会话收尾 | 子会话运行过程实时渲染、结果 markdown 换行无双倍空行 |
| `ask` | 延迟 4s 调 `ask` 计划分支（阻塞等审批）→ 批准后收尾 | 计划卡片全文、选择卡片弹出时消息流落底、审批后继续 |
| `error` | 每次调用 HTTP 500 | 错误气泡与重试耗尽呈现 |

### 自测工具：e2e-subsession（子会话运行服务级端到端，假模型 + 真实服务）

`packages/server/scripts/e2e-subsession.ts`——服务级全链路验证：**真实歌白服务进程**（独立 `GEBAI_HOME` + 独立端口 + 内嵌脚本化假模型，不污染真实数据）+ SDK（REST/WS），逐场景断言事件流 / 会话落盘 / 子Agent 发现 / 门禁回传。覆盖 7 组场景共 54 项断言：①同步隔离子会话（`subSession` 事件标记、过程存档挂调用记录、父会话不逐条落盘过程、不注入合并工具）②同步继承子会话（fork 上下文、两份报告自动合入、合并消息 `role=user + engineNote=subsession`、下一轮上下文可见）③异步运行（`bg_task` s 前缀 wait 取回结果与存档、`subsession_merge` 阶段性合入 + 兄弟感知 + 事件广播）④待办运行内隔离（不继承/不回流/落盘只含父任务、事件带 `subSession` 标记）⑤门禁与参数校验回传（超限/两形态互斥/未知子Agent 附因）⑥空 `agents` 通用子会话（只给全局工具 + 编排）⑦递归深度（进程树三层，第四层被拒、嵌套存档递归挂载）。

```bash
bun run --cwd packages/server e2e:subsession        # 假模型全场景（约 30s，54 项断言）
bun run --cwd packages/server e2e:subsession:real   # 真实模型（GEBAI_LLM_* 配置）实测模型是否按新语义调用，3 任务 24 项断言
bun run --cwd packages/server e2e:subsession:ui     # 常驻假模型 + 服务（页面手输 S1/S2 触发两形态），供 playwright/浏览器实测 UI 渲染
```

**对已启动实例实测（不自起服务）**：`e2e:subsession:real` 支持外部模式——把 `E2E_REAL_EXTERNAL=1 E2E_REAL_PORT=<端口>` 指向一个正在运行的实例（如验证实例），断言会话消息改经协议读取（不依赖实例的 `GEBAI_HOME` 布局），并额外校验前端主产物含 `subsession-run`（防「后端新版 + 前端旧版」错位验证）：

```bash
E2E_REAL_EXTERNAL=1 E2E_REAL_PORT=3999 bun run --cwd packages/server e2e:subsession:real
```

**验证实例启动**（不动现有实例的数据与外部通道，独立 HOME + 关任务/待办/飞书）：

```bash
set "GEBAI_HOME=%TEMP%\gebai-live-3999" && set "GEBAI_PORT=3999" && set "GEBAI_TASKS_ENABLED=false" && set "GEBAI_IDLE_TODO_ENABLED=false" && set "GEBAI_FEISHU_BOT_ENABLED=false" && bun --preload ./scripts/build-env-embed.ts ./src/index.ts
```

UI 层验证（需 playwright）：打开 `http://127.0.0.1:3991/` 输入 `S1`（隔离形态 → 折叠容器「🌿 子会话 · s1 · ⚙ code」渲染 + 内部工具卡 + 返回摘要）、`S2`（继承形态 → 两个容器「🌿 子会话 · 甲/乙」+ 两条「子会话合入」通知条）；对真实模型实例可直接用自然语言（如「用 subsession_run 的 inherit_context:true 并行两个子会话算题并汇总」）实测端到端。

### 自测工具：compact-e2e（压缩接续质量端到端，真实模型）

`packages/server/scripts/compact-e2e.ts`——用**真实模型**量「上下文压缩后能不能接着干活」：压缩的单测只覆盖「摘要输入构造正确」，而接续质量（任务目标 / 文件路径 / 未完成事项是否被摘要保住）只能拿真模型跑。对照设计：A 基线会话（同构长历史不压缩直接问，确认测试本身有效）+ B 压缩会话（先压缩掉含「任务规格」的早期区间再问），判据为 B 的回复能复述出五要素（弱正则匹配）。同时打印生成的摘要——用于评估 `SUMMARY_ITEM_LIMIT` 与块预算是否需要调。

```bash
bun run --cwd packages/server scripts/compact-e2e.ts              # 缺省 320 段 ≈ 7 万字符（约 8 分钟）
COMPACT_E2E_LINES=60 bun run --cwd packages/server scripts/compact-e2e.ts   # 冒烟（约 1 分钟）
```

数据隔离：脚本把 `GEBAI_HOME` 指向临时目录（不碰真实用户数据），LLM 配置仍来自仓库 `.env`；退出码 0 = 压缩后可复述全部五要素。实测（320 段 / 73071 字符 / 323 条）：压缩 303 条耗时 4.7s，摘要 5294 字符**完整保住目标、两个文件路径与两条未完成事项**（并把重复填充段标注为「无新增信息」）；压缩后复述 **5/5**，基线同题也 5/5——即当前参数下接续质量无损失，无需调参。

场景脚本耗尽后回复固定收尾文本（防场景外调用死循环）；`/probe` 页面（`http://127.0.0.1:9801/probe`）连服务端 `/ws` 验证浏览器连通性（标题四态：`WS_OK`/`WS_ERR`/`WS_TIMEOUT`/`WS_CLOSED`）。

### 覆盖率门槛

- 核心引擎（AgentEngine、ToolRegistry、EnvManager、Sandbox、命名空间解析）行覆盖率 ≥ 90%
- 工具函数 ≥ 80%；整体仓库 ≥ 70%（`bun run --cwd packages/server test:coverage` 产出报告）

> 上述为**人工约定的目标值**：无 `coverageThreshold` 配置、仓库亦无 CI 流水线（`.github/workflows` 不存在），门槛不构成强制门禁；提交前跑全量测试靠约定（`run_tests` 工具与 AGENTS.md 纪律）。

#### 实现状态（初始落地快照——部分计数与描述为当时形态，最新口径以正文各节与「常量参考」为准）


- 已实现：`@gebai/server`（AgentEngine 主循环、LLMProvider（OpenAI 兼容 chat/completions + OpenAI Responses + Anthropic 三接口 SSE 解析，**usage 真值解析**（OpenAI `stream_options.include_usage` 末 chunk / Responses `response.usage` / Anthropic `message_start.input_tokens`+`message_delta.output_tokens`，统一挂 done chunk 的 `usage` 字段；服务端不返回时为 undefined → 引擎估算兜底，见「上下文占用口径」），**统一多模态内容块转换**（图片块 base64 内联 → OpenAI `image_url` / Anthropic `image`，`imageMessageBlocks` 助手），**额外模型接口参数 `GEBAI_LLM_EXTRA_PARAMS`（JSON，如推理强度 `reasoning_effort`）Provider 级 + 任务级（浏览器本地注入）两级覆盖，顶层合并进请求体**）、ToolRegistry 命名空间解析（含注册期前缀互斥校验）、SessionStore 分片持久化、EnvManager（全局 + 会话内存态，用户环境变量零留存）、Sandbox 路径沙箱/子进程、AuthService 多用户令牌、**外部身份扩展点**（同源部署集成网站：`POST /api/v1/auth/exchange` 外部身份 → GEBAI 令牌，验证器可插拔——`GEBAI_EXTERNAL_AUTH_SECRET` HMAC 验签（±10 分钟防重放）或 `GEBAI_EXTERNAL_AUTH_URL` HTTP 回调验证，互斥同设报错；`AUTOCREATE` 自动创建/白名单两种映射；Web UI 启动时 URL 参数或 localStorage 同源直读自动兑换，SDK `exchangeExternalUser`/`getExternalAuthConfig`；`external-auth.ts` 单测 13 用例 + 端点集成测试 6 用例）、EventBus、富内容块（text/code/image/file/diagram/diff/html）渲染、**视觉工具 `vision`（后已移除——视觉统一走 vision 子代理）**（额外多模态模型 `GEBAI_VISION_*` 配置，目标 `target` + 图片文件 `image` 参数，base64 内联调用视觉模型，未配置时回落到显式声明多模态能力的主模型，单图 8MB 上限，输出截断保护 + `image` 内容块；`makeVisionTool` 依赖注入可单测）、**图片附件链路**（本地模式附件源路径按会话根解析（修复 CWD 误解析）、`AttachmentRef` 存逻辑路径、多模态主模型 base64 内联/其余降级文本说明 + vision 指引、接口 HTTP 4xx 拒绝图片块时自动降级重试、Web 端 canvas 图片压缩 1280px/2MB）、**内容展示工具 `show` 图表分支（原 `draw`）**（**四种图表语言**——`format` 必选参数 `mermaid`/`plantuml`/`d2`/`echarts`，工具描述与参数说明内置选择指南指导模型按需选择；前端实时渲染确认：成功才返回、报错回传模型、5 秒超时降级；Web 前端本地渲染四语言各自引擎（mermaid npm 包 / `@plantuml/core` / `@terrastruct/d2` WASM / echarts UMD SSR 渲染 SVG，均零网络）；**后端组合渲染器四语言全支持**（飞书与 `render=backend` 通道：plantuml TeaVM 引擎 / mermaid + happy-dom 垫层（getBBox 几何估算覆盖防布局坍缩）/ d2 WASM（二进制模式内嵌产物物化 `{GEBAI_HOME}/vendor/d2js/{version}/`，版本变更自动换目录）/ echarts npm 包 SSR（`ssr:true` 零 DOM，顶层急切导入防 zrender 环境误判）+ 共享 resvg 栅格化（负原点 viewBox 平移归一兼容），全局环境切换 + 单一串行队列，`globalThis.window` 仅临时存在））、**html 分支（原 `render_html`）**（沙箱 iframe 域隔离渲染：`allow-scripts` 不含 `allow-same-origin`，脚本可执行但隔离于 opaque origin，无法访问宿主页面 DOM/存储/顶层导航；落盘会话 `tmp/` 并返回 `html` 块，支持模型显式指定预览尺寸 `width`/`height`，未指定时 iframe 固定铺满消息流宽度、无任何内容宽度反馈）、**path 分支（原 `show_file`）**（模型把文件**直接展示给用户**：按类型产出直显内容块——图片内联 `image`、图表源文件 `diagram` 渲染、`.html` 沙箱预览、文本/代码内联 `code`（≤512KB 读取/4 万字符截断）；无法内联类型回退 `file` 查看卡片（点击按需加载，PDF 内嵌渲染）；会话 `tmp/` 内直接引用，会话外文件复制到 `tmp/shown/{主名}-{内容哈希}.{扩展名}`（≤100MB）后引用）、**全局 diff 工具（后已移除）**（LCS 行级对比原为纯函数 `core/diff.ts`，工具本身已下线——`diff` 内容块仅用于历史会话回放）、5 个内置子Agent（当时；**现为 12 个**，见「更多内置子Agent」）。以下为当时形态（含 `desktop`、**飞书云文档 `feishu_docs`**：42 个工具覆盖文档 docx 创建/读取/块编辑/按文本反查块 id（`find_blocks`）/Markdown 导入导出/**插入图片（`insert_image` 三步流程）**/**思维导图画板读取（`get_board`：mindnote 块自动提取画板 token，结构化提取 PlantUML 源码或重建连接线流程）**/**用户授权（`auth_user_authorize`/`auth_user_token`/`auth_user_status`/`auth_user_clear`：OAuth code 流程配置 user_access_token，会话级存储+自动刷新，配置后资源操作以用户身份执行、创建用户所有权文档；**默认自动回调**——授权后浏览器跳回内置端点 `GET /api/v1/oauth/feishu/callback` 自动兑换写回会话（`GEBAI_PUBLIC_URL` 可配，见「飞书用户授权」））**、云空间、电子表格、多维表格、知识库、搜索、权限与 `api_call` 兜底；块列表附 `type_name` 标注、`page_all` 自动翻页（达上限提示）、小节读取、块操作失败本地诊断、**权限类错误码自动附所需 scope 与授权链接**；`FEISHU_DOCS_*` 凭证 + tenant_access_token 缓存；写操作审批；`createFeishuTools` 依赖注入 + `markdownToBlocks`/`blockText`/`extractBoardContent`/`extractOAuthCode` 纯函数 + 共享 OAuth 模块（`oauth.ts`：兑换/刷新/会话令牌存取/授权状态注册，工具与 REST 回调共用），单测覆盖率 95%）、**浏览器自动化 `playwright`**（无头 Chromium，**当时 15 个工具，现为 30 个**（20 基础 + 7 会话 + 3 CV），覆盖导航/读取/截图/交互/表单/JS 执行/多标签页；**node 桥接架构**——Bun 与 playwright driver pipe 兼容问题用常驻 node 子进程 JSON-RPC 规避，BrowserContext 按会话隔离 + 空闲回收，导航/交互/脚本类默认审批；`createPlaywrightTools` 依赖注入 + Bridge 协议层单测 17 用例，真实 chromium E2E 验证通过）、REST/WS 双通道 API（WS 消息处理独立 `ws.ts`、反馈存取独立 `feedback.ts`；REST 全端点：feedback 查询/导出、多选 zip 打包下载、Webhook CRUD、OpenAPI 文档、auth/me、**飞书 OAuth 回调 `/api/v1/oauth/feishu/callback`**）、**上下文压缩**（当时口径：主动 + 自动 80% 窗口阈值触发、LLM 摘要 + 滚动裁剪降级；**现为「窗口剩余 < 输出预留」水位触发**，见「上下文保护」）、**Webhook 推送**（HMAC 签名、3 次指数退避重试、事件白名单、多用户会话归属过滤）、命名空间注册期碰撞校验、服务模式 admin 密码哈希引导（`GEBAI_ADMIN_PASSWORD_HASH`：设置启用/未设置禁用，启动参数权威配置）、会话操作归属校验）、`@gebai/sdk`（GebaiClient：含 getCurrentSession、webhook/工具启停/打包下载方法）、`@gebai/web`（Vite 聊天 UI，按功能域模块化拆分：state/messages/tool-cards/sessions/composer/attachments/approvals/settings/auth/markdown/diagram/diff/html-view/jump-bottom 等，样式按 base/chat/composer/overlays 分片；**Inter / JetBrains Mono 字体内置**（@font-face 随产物分发，不依赖目标机器字体）；富内容块渲染、历史加载、**交互式图表编辑（Mermaid/PlantUML/D2/ECharts 四语言本地渲染 + 主题适配——PlantUML skinparam 注入与渲染后颜色兜底修正、Mermaid 按 UI 明暗重新 initialize、D2 按 UI 明暗选主题 ID 0/200、ECharts 按 UI 明暗注入 darkMode、标题/图例同顶冲突自动下移避让）**、**diff 并排对比视图（按语言语法高亮，跨行着色平衡）**、**HTML 页面沙箱渲染（预览卡片 + 全屏查看器 + 源码/复制/下载，`render_html` 工具产物）**、**多套 UI 风格（当时 9 套，现 10 套含 `qinhan`；除默认主题外各配画布环境高级特效）**、**低性能模式（纯手动开关，降级动画/毛玻璃特效与图表导出采样，不做硬件自动检测）**、**单轮计时器（任务运行期间消息上实时显示本轮耗时，外观 tab 可关）**、**人民币招财猫（`cny` 主题专属：悬浮可拖动玩偶 + 拖动金币轨迹 + 点击连击爆金币 + 单轮完成大爆发（运行越久越多）；全屏 canvas 粒子引擎（金币 3D 翻转 + 六面额纸币各半，预渲染精灵），低性能模式不发射）**、多用户登录页、会话重命名、压缩入口与压缩通知、设置面板（**浏览器本地环境变量增删改（localStorage，对本浏览器所有会话生效，随消息临时注入服务端、不落盘防泄露；服务端不配模型变量时仅前端配置即可使用；含 `GEBAI_LLM_EXTRA_PARAMS` 建议项，可按任务覆盖模型接口参数；一句话说明与保存按钮固定面板底部）**/外观性能模式/用户管理/反馈列表；工具启停、子Agent 装载与 Webhook 管理不设 UI，保留 SDK/API 方法）、**图表预览卡片随内容自适应**、**发送消息/切换会话自动锁定滚动到底（粘底跟随核心 `sticky-follow.ts` **意图驱动**——主消息列（`sticky-scroll.ts` 工厂 + 「跳到最新」按钮）/ 推理体（`reasoning-scroll.ts`）/ 子会话容器（`scrollSessionSticky`）三处共用同一工厂，可独立单测；「跳到最新」按钮显隐 = 跟随状态完全一致：按钮隐藏 = 跟随中（新内容持续滚动到底），按钮显示 = 用户在阅读历史不打扰。跟随状态只由三类信号翻转，**不做滚动事件位置取证**（目标位置比对 / 程序落位差值）——浏览器 scroll 事件异步合并送达，内容增长/收缩引发的钳制与布局调整同样产生滚动事件，取证式分类必被迟到事件误判为用户滚动、被过期落位误判为用户上翻（「自动滚动滚一段后失灵」的根因）：① **用户输入意图**（同步、必然先于其滚动效果到达，无竞态）：滚轮上滚 / 触摸上滑 / 向上滚动键（PageUp/ArrowUp/Home；输入框内方向键滚动的是文本光标不触发）/ 滚动条拖动（pointerdown 命中滚动条槽区：`offsetWidth-clientWidth` 宽度带）/ 消息导航跳转（`stopFollowing` 显式解除）→ 立即解除；② **几何贴底**：任何滚动事件落在阈值内（主列 64px / 容器 8px / 推理体 4px）→ 恢复跟随（滚回底部 / 收缩钳制到底收敛；小幅上滚在阈值内视为仍贴底继续跟随）；③ **静默窗口兜底**：未贴底且距最近程序滚动/DOM 变化超 80ms → 无法归因为内部动作，视为未知输入（中键自动滚动 / 查找定位 / 覆盖式滚动条拖动）→ 解除；窗口内的未贴底事件视为程序滚动/钳制的迟到事件：保持跟随并回正到底（续滚无需登记目标落位——旧实现须读回 clamp 后实际落位精确比对，且续滚自身不登记导致容器内跟随必现失效）。**窗口时间戳仅在跟随中刷新**——未跟随时内容变更（流式每 120ms 重解析）不再续窗：否则窗口对流式会话永不关闭，非滚轮类滚动输入每滚一下都被回正拽底（「滚动卡死」形态）。**粘底对齐保持（帧预算循环 240 帧）**沿用：内容高度存在不触发 MutationObserver 的异步修正（字体/图片加载等异步布局修正）——跟随期间按帧续查对齐（每帧仅属性读取、贴底即空转）、预算耗尽自停（几何不可用/NaN 时停转防死循环）；新内容（DOM 变化 MutationObserver / 图片加载含 markdown 内嵌 `<img>` 由 `msgEl` 委托捕获阶段 load 监听 / ResizeObserver）触发跟随，rAF 节流每帧至多一次保证流式高频更新性能；会话切回滚动位置恢复走 `restoreScroll`：落位后按几何同步跟随状态，未决跟随回调（排期中的 rAF/对齐保持循环）按执行时状态自然失效——loadMessages 尾部先排期 scrollIfSticky rAF 再 lockToBottom 后恢复历史位置，未决回调不把恢复位置拽到底部）**、**消息质量反馈（助手消息 👍/👎 提交反馈，设置面板反馈页可见）**、**会话导出（Markdown 下载）**、DOM 引用统一集中于 `state.ts` 与代码高亮复用（`highlightedCode`））、`@gebai/desktop`（服务端同进程 + 浏览器兜底宿主）。

- 测试：`bun run --cwd packages/server test`（分片并行）全量用例通过（核心引擎、命名空间、沙箱、压缩、Webhook、ZIP、协议契约、服务模式鉴权、飞书机器人全覆盖）。
- 已实现补充：**原生 WebView 桌面宿主**（`packages/desktop/launcher/`：tao 窗口 + wry WebView，无 Tauri 依赖；`include_bytes!` 内嵌 `gebai.exe`，物化到 `%LOCALAPPDATA%\gebai\` 后 spawn，stdout 解析端口导航 WebView，关窗回收侧车）、**playwright driver 打包闭环**（`packages/server/scripts/build-driver-embed.ts` 生成 `driver.embedded.generated.json` 内嵌产物，二进制运行时物化 `{GEBAI_HOME}/vendor/playwright/driver.mjs`）、**playwright-core 打包闭环与惰性桥接**（`packages/server/scripts/build-pwcore-embed.ts` 整树 gzip base64 内嵌、运行时物化 `{GEBAI_HOME}/vendor/playwright-core/`；桥接经 `createLazyBridge()` 全进程惰性单例，bundle 图内子Agent 模块禁止模块作用域第三方包解析；Windows 默认 `channel=msedge` 驱动系统 Edge，`GEBAI_PLAYWRIGHT_CHANNEL` 覆写）、**数据生命周期 GC**（`core/session/gc.ts`：会话 90 天闲置归档 `trash/`、`trash/` 7 天物理删除、`feedback/` 180 天清理、遗留用户级 `truncated/`（30 天宽限期后删除，非迁移）；启动即跑 + 每日周期，`GEBAI_GC_DISABLED=1` 关闭）、**子Agent 目录化**（`packages/agents/src/agents/{name}/{name}.ts` + `{name}.md` 提示词拆分与 ts 导入修饰）、**子Agent 打包闭环**（`packages/server/scripts/build-subagents.ts` 构建时生成 bundle 注册表，dist/二进制模式 `discover()` 回退加载，md 提示词随静态 import 内联进产物；playwright 的 node 桥接驱动 `driver.mjs` 复制到 dist/ 与产物同目录；**D2.js 打包闭环**——`packages/server/scripts/build-d2js.ts` 生成内嵌产物 `d2js.embedded.generated.json`（node-esm 构建 7 文件 gzip base64，静态 import 随产物打进二进制，运行时物化到 `{GEBAI_HOME}/vendor/d2js/{version}/` 供文件路径 Worker 运行；构建命令 `--external @terrastruct/d2`））、**定时任务**（`core/schedule/cron.ts` + `packages/agents/src/agents/cron/cron.ts` + `core/schedule/notify.ts`：`GEBAI_CRON_ENABLED` 开关（**默认开启**，显式 false 时 `cron` 子Agent 不注册、`cron_*` 工具完全不注册、REST 503）；`cron` 子Agent 命名空间暴露 `cron_add`/`cron_list`/`cron_update`/`cron_trigger`/`cron_remove` 工具（创建/修改/删除/手动触发默认需审批，自原全局工具下沉并补手动触发）；**用户级存储** `users/{user}/cron.json`（与会话生命周期解耦，会话内保存的旧版布局已弃用、启动遇之忽略）+ 任务专属工作目录；脚本运行 + 提示词运行 agent 两种类型，prompt 型执行目标 ephemeral/sticky/session 三态（独立新会话/专用会话复用/绑定会话，绑定会话删除自愈降级）；5 段 cron（可配 IANA 时区，日周 OR 语义）/`@every n{s,m,h,d}`/`@daily` 等/`@at` 一次性表达式；`misfire` 错过补跑、`timeoutMs` 执行超时（prompt 型 engine.cancel 终止）、`maxConsecutiveErrors` 连续失败自动停用、每任务执行记录按文件保留（上限 200 条）；通知通道（webhook/飞书群自定义机器人加签/飞书应用消息，`notifyOn` 时机，尽力而为不阻塞执行，密钥回显脱敏）；REST `/api/v1/cron` 管理面；30 秒 tick 调度；`event.cron.run`/`event.cron.result` 事件）、**多用户安全加固**（REST 用户管理端点管理员校验（与 WS 同权限，防提权）、`GEBAI_APPROVAL_SKIP` 用户本人可设置自己的会话（会话内存态 env 不落盘，写入只影响本人会话，非管理员仍受沙箱约束；ask 填值分支服务模式拒绝）、沙箱模式脚本子进程环境剔除敏感变量（`*_KEY`/`*_TOKEN`/`*_SECRET`/`PASSWORD`，防服务端密钥经 sh/py/cron 外泄）、`PATCH /api/v1/tools` 服务模式限管理员、**默认用户沙箱豁免**（**admin 用户为特权用户**，`Sandbox.isExempt`/`enforcedFor(user)` 按用户判定——豁免**仅本地模式生效**（`auth === "local"`），服务端部署下 admin 同受沙箱约束；普通用户恒受约束）、`store.load` 按用户索引 + 会话归属记录（修复旧版无 userId 会话跨用户命中））、**前端页面捕获 `page_capture`**（仿 show 图表分支前端配合链路：引擎发布 `event.capture.request` → 前端捕获当前页面渲染后 DOM html（截断 300KB）+ modern-screenshot 截图（png/jpeg，体积压缩 ≤2MB）→ WS `capture.result` 回传 → 落盘会话 `tmp/capture/` 并返回文件/图片块；`self_optimize` 重构为 **code 超集**（继承其全部工具（含并入 code 的 preview_server）+ page_capture/vision），vision provider 经 `setVisionProviderGetter` 注册点与主 Agent 共用解析逻辑；SDK `submitCaptureResult`；页面捕获超时 30 秒、整页截图高度上限 12000px）、**飞书机器人对话桥接**（`feishu-bot/`：自研极简 protobuf 帧编解码（pbbp2.Frame）+ 长连接协议层（endpoint 发现/心跳/分片合包/ACK/自动重连）+ 开放平台 API（tenant_access_token 缓存、消息发送/撤回、图片资源下载、会话信息）+ 桥接编排（会话映射当时为 `feishu_{chat_id}`，**现为 `sha256("feishu:"+chat_id)` 前 32 位 hex**；身份映射（多用户按 open_id 自动建户）、文本/图片消息处理、@提及剥离、流式增量预览（节流合并发新撤旧）+ 最终卡片（lark_md）、斜杠命令、文本命令式审批；`GEBAI_FEISHU_BOT_ENABLED` 开关，依赖全部注入；单测 **134 用例** + 真实凭证长连接握手验证通过）。
- 未完成（后续迭代）：子Agent 选择性打包的黑名单形态（构建期只有白名单 `GEBAI_BUILD_SUBAGENTS`，黑名单为**运行时** `GEBAI_SUB_AGENTS_DISABLE`）、飞书机器人 Webhook 回调模式、OIDC 身份对接。

### 稳定性保障：防「模型误改」安全网

自我优化（`self_optimize`）修改代码后，**测试是唯一准入凭证**：

- **修改前基线**：改动前先跑全量测试确认绿色基线，记录结果
- **修改后门槛**：任何代码变更必须通过相关测试（新增工具 → 工具测试；改提示词 → 契约/E2E 测试；改引擎 → 全量回归）才能落盘
- **回归守护**：全量测试含关键路径断言（审批流转、上下文压缩、命名空间解析、沙箱边界），大模型误改行为（破坏协议、绕过沙箱、错误状态机）会被测试直接捕获
- **自动回滚**：测试失败自动回滚本次改动，返回失败原因给模型引导修正（与「自我优化」章节的测试门槛一致）
- **防呆测试**：对脆弱逻辑（正则解析、路径拼接、哈希分片）编写**边界与对抗用例**（畸形输入、路径穿越、超长名、Unicode），确保模型改代码时不易踩碎隐性契约

### 代码设计原则

- **高度模块化**：核心模块无副作用、依赖显式注入，可独立实例化测试
- **接口优先**：Agent、Session、SubAgent 等核心概念通过接口定义，便于 mock
- **工具可测试**：全局工具和子Agent 工具的函数体为纯函数或可注入依赖的函数工厂
- **Code Agent 自主测试**：每个代码生成任务完成后，Agent 应能自主运行相关测试验证，测试命令明确、零配置（`bun test` 即可）
- **可伪造性**：LLM Provider、时间、文件系统均有测试替身（fake），测试不依赖真实网络/时钟/磁盘状态

## 常量参考

| 常量 | 值 | 说明 |
|------|-----|------|
| 模型调用重试次数 | 2 次 | 引擎层空响应/无产出异常重试上限（`LLM_RETRY_COUNT`）；provider fetch 层网络错误/429/5xx 重试 2 次 |
| 模型调用重试退避 | 800ms 基数 | 引擎层指数退避（800/1600/3200ms，`LLM_RETRY_BACKOFF_MS`，测试可注入 `retryBackoffMs` 加速重试用例）；provider 层 500ms 基数 |
| 工具返回截断阈值 | ~12000 字符 | 超出后按行截头尾（保留完整行，单行巨长按字符兜底），完整内容写入会话工作目录 `truncated/{tool}_{hash}.txt`（物理位置 `{session}/tmp/truncated/`，沙箱内可读），返回的相对路径在文件工具与脚本里都能直接用；**引擎在主循环兜底**：工具未自行截断的超长输出统一走该逻辑（`TRUNCATE_THRESHOLD`）；阈值适配现代大上下文窗口，膨胀防护由上层压缩器兜底 |
| 截断保留首/尾 | 各 4000 字符 | 截断消息保留的 head/tail 长度（`TRUNCATE_HEAD_CHARS`/`TRUNCATE_TAIL_CHARS`） |
| 计划文档目录 | `plans/` | ask 计划分支计划落盘目录（相对会话工作目录，物理位置 `{session}/tmp/plans/`，随会话文件面板可见；文件名按标题清洗（Unicode 字母/数字/下划线/连字符——**中文保留**，空回退 `plan`，长标题截断 60 字符），同标题重提计划覆盖更新） |
| 用户输入落盘阈值 | 12000 字符 | 超长用户输入发送时全文落盘会话工作目录 `user_inputs/{hash}.txt`（原文不丢，read 可读），消息正文保留头尾各 4000 字符 + 文件引用（`USER_INPUT_SPILL_THRESHOLD`/`USER_INPUT_SPILL_HEAD`/`USER_INPUT_SPILL_TAIL`，见「上下文保护」） |
| grep 单文件读取上限 | 1MB | 超出跳过该文件（防大文件/二进制拖慢搜索） |
| 测试分片默认数 | `min(8, max(2, CPU 核数))` | `packages/server/scripts/test-parallel.ts` 分片数（`--shards=N`/`GEBAI_TEST_SHARDS` 覆盖）；LPT 按文件字节数装箱均衡负载 |
| grep 匹配子进程超时 | 20 秒 | 正则匹配在独立子进程执行（灾难性回溯防护），超时强杀并返回引导（`GREP_MATCHER_TIMEOUT_MS`）；行数据按 4MB 批量送子进程 |
| edit 正则匹配超时 | 20 秒 | `edit` 的 `pattern` 项在独立子进程执行（灾难性回溯防护），超时强杀并返回引导（`REGEX_MATCHER_TIMEOUT_MS`）；单次匹配上限 1000 处（`REGEX_MAX_MATCHES`，超限拒绝） |
| read/edit 文件大小上限 | 8MB / 5MB | 全量读入内存前 stat 预检（GB 级文件直接 OOM；edit 与 patch 5MB 同口径），超限引导 offset/limit 分段或 grep/patch 定位（`READ_MAX_FILE_BYTES`/`EDIT_MAX_FILE_BYTES`） |
| 文本编码探测 | BOM / UTF-16 LE·BE / UTF-8 / GBK | `core/base/file-text.ts`：BOM 优先→无 BOM UTF-16 启发式（NUL 占比 ≥30% 且解码无控制字符）→UTF-8 严格解码（控制字符占比 ≤30%）→GBK（严格解码成功且含 CJK）；均不命中按二进制拒绝；GBK 无编码表，仅支持纯 ASCII 的字节级替换 |
| fetch_url 流式读取上限 | 10MB | 响应体流式限量读取（下载后截断防不住内存：超大/无限流在截断前已全量入内存，`FETCH_URL_STREAM_MAX_BYTES`） |
| js 子进程日志总量上限 | 2MB 字符 / 4MB 单行缓冲 | 父进程侧日志总量封顶（超时只杀子进程不回收已累积内容，`JS_LOG_TOTAL_CAP`）；无换行巨流的 stdoutBuf 硬上限（`JS_STDOUT_BUF_CAP`，行长检查仅在遇换行时生效） |
| 会话 env 内存缓存 | 256 会话 | `envCache` LRU 上限（命中刷新位置，防长生命周期进程无界增长） |
| 先到决策/选择/环境值排队上限 | 64 条/任务 | `pendingDecisions`/`pendingChoices`/`pendingEnvRequests` 容量（随机 id 无界堆积的内存放大防护，`PENDING_QUEUE_LIMIT`） |
| OAuth pending 授权有效期/上限 | 10 分钟 / 32 条 | 飞书 user_access_token 授权 state 的 TTL 与容量（未完成授权的条目含 appSecret 明文，按 TTL 惰性清理+超容量淘汰） |
| grep 最大匹配行 | 200 行 | 达到即停止，返回结果附「已达匹配上限」提示（content/files/count 三形态同一口径）；`head_limit` 参数可压低先看一部分（`data.truncated` 标记截断）；`context` 参数 0-10 行；`literal:true` 字面匹配（正则元字符自动转义）；`include`/`exclude` 支持逗号多模式与 `{a,b}` 花括号 |
| grep/glob 默认排除目录 | `WALK_SKIP_DIRS` 14 项 | node_modules/.git/dist/build/.next/.cache/__pycache__/.venv/venv/target/.idea/.vscode/coverage/.turbo——grep/glob 结果默认跳过（与 `walkDirFiles` 目录遍历同一清单（**单一来源**：`@gebai/sdk/node` 的 `walk.ts`，server 侧仅重导出）：会话 `tmp/` 列表场景的防噪兜底）；include/pattern 原文显式点名该目录时不排除 |
| 已读登记上限 | 2000 条/会话 | 防盲写守卫（write/edit/patch）的会话级已读集合上限（`READ_TRACK_CAP`，超出整表重置——守卫降级为「需重读」）；会话删除时释放（`engine.forgetSession`） |
| patch 匹配容错 | 裁剪 3 行 / 消歧 30 行 / 候选 200 处 / 诊断 3 条 | 上方 patch 章节的三层参数：头尾上下文裁剪上限（`PATCH_FUZZ_LINES`）、多候选时声明行号的可信距离上限（`PATCH_ANCHOR_TOLERANCE`，超出按歧义报错）、单处 hunk 候选位置收集上限（`PATCH_MAX_CANDIDATES`）、失败诊断中列出的相近位置条数（`PATCH_DIAG_CANDIDATES`） |
| patch hunk 上限 | 100 处 | 单次补丁 hunk 数上限（超出提示拆分补丁） |
| patch 文件上限 | 5MB | 目标文件大小上限（超出提示改用 edit 分段修改） |
| search_symbols 单文件上限 | 1MB | 超出跳过该文件（与 grep 同级） |
| search_symbols 扫描上限 | 500 个文件 | 最多扫描文件数（内容预筛后 tree-sitter 解析）；references 模式同限（叶子节点精确匹配 + 定义名排除，单树遍历节点数上限 20 万防巨文件拖慢） |
| search_symbols 匹配上限 | 50 条 | 达到即停止，精确匹配优先排序 |
| git log 条数 | 10 条 / 50 条 | 默认条数 / 上限（`max_entries` 参数可调） |
| file copy 复制上限 | 100MB | `file` copy 动作的二进制整读整写上限（`FILE_COPY_MAX_BYTES`，超限引导用 sh 复制）；mkdir 递归创建、已存在幂等 |
| URL 抓取响应上限 | 200KB | `fetch_url` 超出截断（含截断落盘），防内存膨胀 |
| URL 抓取超时 | 15 秒 | `fetch_url` 单次请求上限 |
| 重定向跳数上限 | 5 跳 | `fetch_url`/`http_request` 重定向逐跳校验的最大跟随次数（防重定向循环与跳板链）；`capture_replay` 沙箱模式不自动跟随重定向（3xx 返回 Location 逐跳重放） |
| 浏览器网络录制条数 | 500 条/会话 | playwright driver 网络录制上限（超出丢弃最旧；响应体预览 20KB/条，超 200KB 不捕获；`network_list` 默认返回最近 200 条、limit 参数最高 500） |
| 浏览器 WS 帧录制 | 300 帧/会话，单帧预览 4KB | WebSocket 帧录制上限（随 capture 录制开关联动；`capture_ws` last 参数默认 100、上限 300） |
| 浏览器请求拦截规则 | 32 条/会话 | `reverse_site_route` 拦截规则上限（超出提示先 clear；上下文重建后自动重挂） |
| 浏览器响应体提取上限 | 20MB | `capture_body` 完整响应体落盘上限（文本预览 200KB 截断，超长/二进制走 file 参数直接写盘）；透明浏览器代理（`GEBAI_BROWSER_PROXY`）的单响应体临时文件中转同限，单次代理请求超时 110s |
| 浏览器对话框/下载记录 | 100 条/会话 | dialog 记录（list 默认返回最近 50）与下载记录上限（下载文件保留在系统临时目录，仅记录淘汰） |
| 上下文压缩触发 | 窗口剩余 < 一次回复的输出预留 | **不以窗口百分比判定**——真正约束是「留给输出的空间」：窗口剩余（`maxContextTokens - 最近一次真实 input tokens`）不足以支撑一次回复（输出预留 = 模型单次响应输出上限 `maxOutputTokens`，即 `GEBAI_LLM_MAX_OUTPUT_TOKENS`；未声明时缺省 16384，并夹在 `[1024, 窗口一半]` 内）时触发压缩（`outputReserveTokens`/`lacksOutputRoom`）（见「上下文保护」） |
| 上下文压缩目标 | **窗口 × 40%**（下水位，压到即停） | 上水位（触发）= 剩余 < 输出预留（见上行）；下水位（目标）= `窗口 × COMPACT_TARGET_RATIO`（缺省 0.4，`GEBAI_COMPACT_TARGET_RATIO` 可调，合法区间 0.1~0.9），且不高于触发线（配置矛盾时以触发线为准）——需腾出量 = 真实基线 − 目标输入，按 `estTotal/基线` 比率折算为估算口径后逐条累计（比率越界退回 1:1，`COMPACT_EST_SCALE_MIN/MAX`）；可压消息不够（窗口外全压完仍高于目标）时压满窗口外，不足部分交给迭代压缩与溢出恢复；基线已低于目标（如接口已报溢出）时至少腾挪窗口 5%（`COMPACT_MIN_ROOM_RATIO`） |
| 上下文压缩可压缩口径 | 除系统提示词外全部 | `isCompressibleMessage`：**仅 role=system 不可压缩**（系统提示词/装载提示词区间夹带时原位保留）；用户输入、assistant、tool 结果、引擎注入提醒进区间后被摘要替换并**完全移除**（滑动窗口内的近消息不入区间）；子会话运行存档（session/subAgent 标记）不进主上下文也不动。与超限截断口径（`isProtectedMessage`：user/system/存档不做无摘要丢弃）分开 |
| 上下文占用口径 | 模型服务返回的 usage 真值 | 压缩判定只认接口返回的 `input_tokens`（含 system 提示词与工具 schema）：run 前按上次调用持久化基线、任务中途按每轮真实 usage（窗口剩余 < 输出预留时压缩，见上行）、接口上下文长度 4xx 拒绝时压缩重试（溢出恢复）。**基线每轮即时落盘**——每轮真值到达时 `store.updateCtxStats` 一并写入真实 `input_tokens` 与索引锚点（随后任意一次正文落盘即进 chat.json，不只任务结束）：任务中断/进程重启后，下一次 run 的压缩判定与列表展示仍有真值可用，不退回估算。**展示口径与真值同点位同步**：每轮真值到达时推送 `event.session.ctx` 并调 `store.updateCtxStats` 落盘（只写 meta.json，不重写正文）——列表/快照/刷新与推送一致；压缩与护栏降级改写历史时把展示值重算为当前消息估算（旧数不得残留）。估算（`estimateCharsTokens`，CJK 约 1 token/字、ASCII 约 4 字符/token；**多模态图片块按张常量**——`estimateContentTokens`/`IMAGE_TOKEN_ESTIMATE`，内联块携 base64，按序列化长度折算会把一张 1MB 图片算成 30 万 token）用于压缩量规划（`estimateMessageTokens`，真值口径的需腾出量按 `estTotal/基线` 比率折算）与会话列表 ctxTokens 展示的增量补足（`estimateCtxTokens`/`estimateTokens` 均已计入工具 schema 段估算 `estimateSchemasTokens`）（见「上下文保护」）。缓存命中（`cachedTokens`/`ctxCachedTokens`）为纯展示口径，随 usage 基线同点位建立/清除（见「自动压缩」的缓存命中度量） |
| 压缩保留最近下限 | **滑动窗口 12 条消息**（任意角色） | 近消息窗口（`COMPACT_WINDOW_MESSAGES`，`GEBAI_COMPACT_WINDOW` 可调）：最近这么多条消息永不进压缩区间（原样保留），随新消息自然向前滑动；上限为历史一半（`floor(len/2)`）——否则短会话永远压不动。有真实占用基线时窗口仅为**保底下限**（压多少由水位算出的需腾出量决定，实际会话一般不会触及窗口）；无基线（老会话/接口不返回 usage/窗口未知）时退保守口径：只压掉窗口外可压缩消息的一半 |
| 摘要输入/输出上限 | 单块 20000 / 总覆盖 12 万 / 输出 2000 字符；请求预留 8192 tokens | **首选缓存友好前缀请求**（与主循环逐字节同前缀的原文 + 尾部压缩指令，不分块；装不下时才算超预算）；骨架路径：单块输入预算（`SUMMARY_INPUT_LIMIT`）、块数上限 6（`SUMMARY_MAX_CHUNKS`，超出时头尾保留 + 中部省略说明）、输出上限（`SUMMARY_OUTPUT_LIMIT`）、单条骨架 600 字符（`SUMMARY_ITEM_LIMIT`）、降级骨架 15 行 × 120 字符（`SUMMARY_FALLBACK_LINES`/`SUMMARY_FALLBACK_ITEM_LIMIT`）；前缀请求可行性预判用 `SUMMARY_OUTPUT_RESERVE_TOKENS`（8192：摘要输出 + 工具 schema 段开销）
| 工具调用轮次上限 | 不限制 | 单次任务内模型工具调用轮次无上限（超长任务不截停）；失控防护由重复检测终止/用户取消/上下文压缩承担，`rounds` 仅计数回传 |
| 重复检测窗口 | 最近 8 次调用 | 工具调用签名（工具名+参数 JSON）滚动窗口（`MAX_REPEAT_WINDOW`） |
| 重复检测命中阈值 | 连续 3 次 | 窗口**尾部连续**出现相同签名第 3 次（其间无任何其他调用）才判定为无效重复，中断该次执行并注入引导提示（`MAX_REPEAT_HITS`）；间隔其他调用后重发同签名不累积（「改动后复查」合法）；**同批重复签名只记录一次**（同批相同调用是有意扇出，跨轮连续重发才累积） |
| 重复中断上限 | 2 次 | 重复中断超过该值终止工具循环（`MAX_REPEAT_STALLS`），仍返回最后产出文本 |
| 同批工具并行上限 | 8 个 | 单次模型响应返回的多个工具调用并行执行的并发护栏（`MAX_PARALLEL_TOOLS`，超出按调用顺序排队）；需严格串行的操作由模型用 js 脚本编排或拆分多轮（见「同批工具并行执行」） |
| 审批超时 | 5 分钟 | 审批请求等待上限，超时自动拒绝并提示模型调整 |
| 脚本执行超时 | 5 分钟 | `sh`/`py` 单次执行上限（超时杀进程并返回 `[timed out after ...]` 结果给模型，不结束任务）；`timeout` 参数可按次调整（秒，默认 300、上限 540，不晚于工具执行超时兜底） |
| sh 异步任务生命周期 | 默认 30 分钟 / 上限 60 分钟 | `sh async:true` 后台任务生命周期上限（status/wait/list/kill 时惰性检查，超限终止并标记 timed_out；`timeout` 参数在此语义下调整，`shTaskLifetimeMs`） |
| sh 异步任务并发上限 | 8 个/会话 | 同时运行的后台任务数上限（`SH_TASK_MAX_CONCURRENT`，超限拒绝新任务并引导清理） |
| sh 异步任务存储 | `tmp/sh-tasks/` | 记录 `tasks.json`（原子写）+ 每任务 `{id}.log` 合并输出（stdout+stderr）；任务 id 形如 `t` + 8 位 hex |
| bg_task 命令任务输出尾部 | 默认 4000 / 上限 20000 字符 | status/wait 返回的日志尾部字符数（`tail` 参数可调，完整日志 `sh-tasks/{id}.log`，相对会话工作目录） |
| bg_task 等待默认/上限 | 60 / 60 秒 | `action=wait` 阻塞等待上限（三类后台任务同口径，轮询 300ms）——**上限压到 1 分钟强制按进度轮询**：等满上限而不查过程是无收益的空耗，超时返回当前状态与进度、需要继续等再次 wait（子会话运行的完成报告落存档、继承形态自动合入父上下文，都不会因等待超时丢失） |
| 子会话并发上限 | 8 个/会话 | 同时运行的子会话数上限（`SUBSESSION_MAX_CONCURRENT`，**全树合计**——子会话内再派生同样计入，超限拒绝新运行并引导清理；另：单次调用子会话数上限 8（`SUBSESSION_MAX_PER_CALL`）） |
| 子会话终态保留 | 最近 20 条/会话 | 已结束子会话的记录保留上限（`SUBSESSION_KEEP`，超出淘汰最旧；运行中不淘汰；进程内不落盘，重启即中断） |
| 子会话命名与 runId | `s1..sN` / `s` + 8 位 hex | 子会话名缺省按批内序号 `s1..sN`（调用方可传 `name`，批内唯一、≤32 字符不含空白——中文名合法）；runId 形如 `s` + 8 位 hex（`bg_task` 按前缀分发） |
| 子会话报告合入上限 | 16000 字符 | 合入父上下文的子会话报告长度上限（超出保留头尾 + 省略说明，`SUBSESSION_MERGE_MAX_CHARS`——纯上下文保护，无落盘兜底）；**摘要合入阈值** 1500 字符（`SUBSESSION_MERGE_SUMMARY_SKIP_CHARS`，`merge=summary` 时报告超过该值才触发模型摘要，短报告原文合入）；父会话/子会话通知（互相感知注入）上限 2000 字符（`SUBSESSION_NOTICE_MAX_CHARS`，保留头部） |
| 子会话快速结束宽限 | 120 秒（夹取 1s~600s） | 收敛指令注入后留给模型输出结论的时间（`SUBSESSION_FINISH_GRACE_MS` 缺省，传值夹取到 `SUBSESSION_FINISH_GRACE_MIN_MS`~`SUBSESSION_FINISH_GRACE_MAX_MS`）；逾期强制终止；触发入口见「子会话快速结束」 |
| 工具执行超时兜底 | 9 分钟 | 引擎层兜底（`TOOL_TIMEOUT_MS`，可注入）：覆盖不响应超时的工具（如网络请求挂起）；超时不结束任务，结果作为「执行超时」返回模型自行调整（子Agent 内挂起工具同样受此保护） |
| 用户中断宽限期 | 300 毫秒 | 取消后等待工具自身交回中断返回的上限（`TOOL_ABORT_GRACE_MS`，引擎选项 `cancelGraceMs` 可注入）：宽限内交回则其真实返回并入工具结果，未交回则以统一中断标记即时收口（不拖慢取消；见「中断与取消」） |
| 长工具执行心跳 | 25 秒 | 工具执行期间周期发布 `event.tool.alive` 刷新前端空闲看门狗（`TOOL_HEARTBEAT_MS`，测试可注入 `heartbeatMs`；见「等待期不误判挂起」） |
| 交互等待心跳 | 25 秒 | 等待用户作答（审批/选择/填值/画图/捕获）期间周期发布 `event.interaction.alive`（`INTERACTION_ALIVE_MS`，等待结束即停）；前端看门狗另按「已连接 + 无待决交互」两重豁免，断线或长时间思考不得杀后台任务 |
| 任务受理宽限窗口 | 15 秒 | SDK 任务受理后不据快照判定任务已结束（排除引擎登记运行态前的起跑竞态）；构造函数选项 `acceptSettleMs` 可调（测试） |
| 子Agent 调用超时 | 不设 | 子Agent 调用不设整体超时（执行进度实时可见，中止仅由父任务取消传播；`SUBAGENT_TIMEOUT` 已移除） |
| 子Agent 递归深度 | 3 层 | 子Agent 嵌套调用最大深度 |
| 待办续做轮次上限 | 1 轮 | 会话完成时仍有 `pending`/`in_progress` 待办，追加 **user 软性提醒**（`engineNote: "todo"`）继续会话的轮次上限（`MAX_TODO_CONTINUE`，达到即停止；模型对提醒的纯文本回应视为决策收尾，不再注入） |
| 收尾验证提醒轮次上限 | 1 轮 | 任务修改了代码文件但未运行测试/检查时，结束注入验证提醒的轮次上限（`MAX_VERIFY_NUDGE`，防反复打扰）；代码文件按扩展名判定，**会话工作区（`tmp/`）内的文件不计入**（临时产物/测试夹具，非代码改动，见「收尾验证提醒」）；验证识别三通道（sh/py 命令关键词、验证类工具名后缀 `run_tests` 含命名空间形态、js 编排的「命令调用点 + 关键词」组合）均按关键词宽匹配（宁漏勿紧） |
| 预览服务就绪超时 | 15 秒 | `preview_server` 启动后 TCP 就绪探测总时限（超时即终止并回显日志尾部） |
| 预览服务轮询间隔 | 300ms | 就绪探测轮询间隔 |
| 预览服务日志/状态 | `os.tmpdir()/gebai-preview-{port}.log`、`gebai-preview.json` | 独立进程 stdout/stderr 日志与运行状态（port/pid/url），状态文件按 PID 存活清理 |
| 垃圾回收保留期 | 7 天 | `trash/` 中删除/过期数据保留时长（已实现：GC 每日执行） |
| 反馈数据保留期 | 180 天 | `feedback/` 反馈保留时长（已实现） |
| 会话闲置过期 | 90 天 | 无活跃会话归档到 `trash/` 的时间（按 `chat.json` mtime 判定，已实现） |
| GC 周期 | 24 小时 | 数据生命周期清理任务执行周期（启动时立即执行一次） |
| WS 事件日志容量 | 1000 条 | 每用户事件日志上限（`JOURNAL_CAP`，超出丢最旧；重放出现缺口时客户端触发全量重同步） |
| 注册端点限流 | 30 突发/0.5每秒 + 10 突发/0.1每秒 | 注册独立令牌桶（全局桶 + 来源桶，`routes/auth.ts`） |
| 消息缓存上限 | 2000 条（超限单次裁到低水位 1800 条） | 会话消息**持久化**上限——**纯存储安全网，不参与压缩判定**（上下文保护只认 token 水位口径）：超限**按批裁剪**（`TRIM_LOW_WATER_MESSAGES`=上限×0.9，一次裁到低水位、单批腾出 `TRIM_BATCH_MESSAGES`=200 条余量）丢最早的非保护消息，按 tool_call 配对原子丢弃（连带丢弃的 tool 结果计入移除数）；系统提示词/用户输入/存档消息原位保留（见「上下文保护」）。按批裁剪是为**少改上下文前缀**（服务端前缀缓存）：逐条挤出等于每条新消息都改一次前缀，按批裁剪下前缀每 200 条消息才变一次。取值高于水位口径可达的条数（128k 窗口、输出预留 16384、system 提示词与工具 schema 约 13k 时，撞水位需平均约 50 token/条以上），正常会话的提前裁剪由压缩承担。**代价**：`store.save` 每条消息全量重写 `chat.json`——实测中位约 2.9KB/条，本上限下单个会话文件可达数 MB（写放大随消息数线性增长） |
| 桌面固定端口 | 47896 | 桌面形态默认监听端口（`DESKTOP_PORT`，见「端口固定」） |
| Session 缓存 LRU | 10 个 | 会话列表 LRU 驱逐上限 |
| 截断内容哈希 | SHA256 | 基于完整返回内容计算，用于去重和文件命名 |
| 图片压缩上限 | 1280px / 2MB | 发送给大模型前的服务端压缩阈值（`@gebai/agents` core/shared/image-resize.ts，长边/体积超限等比缩放后同格式重编码，JPEG/WebP 质量 0.85；原图保存不压缩，见「图片压缩时序」） |
| analyze 超时 | 30 秒（钳制 1~300） | vision 子代理 `analyze` 的 `timeout` 参数默认值；超时返回提示引导改用本地视觉工具 |
| 视觉工具图片上限 | 8MB | 视觉分析（vision_analyze）单张图片大小上限（超出提示压缩后重试） |
| 页面捕获等待超时 | 30 秒 | `page_capture` 工具等待前端捕获回传的最长时间（超时返回「页面捕获失败」提示） |
| 页面捕获 html 上限 | 300KB | 前端捕获 DOM html 的传输/落盘截断长度（超出截取首部，完整结构可用 `read` 分文件读取） |
| 页面捕获截图上限 | 1600px / 2MB | 前端截图输出长边与体积上限（超限 JPEG 重编码/等比缩放降质；png/jpeg） |
| 整页截图高度上限 | 12000px | fullPage 截图最大高度（canvas 尺寸上限保护，超出截取顶部） |
| 附件图片内联上限 | 8MB | 主模型多模态内联附件图片的 base64 大小上限（超出降级为文本说明） |
| 历史图片内联窗口 | 最近 3 组 | 仅最近 3 组含图片的消息（用户图片附件与工具结果图片——read 读取的图片引用）内联进上下文，更早的降级为路径说明（图片不参与压缩，长会话防图片占死窗口；`INLINE_IMAGE_RECENT`） |
| CV 输入图像上限 | 8MB | 本地识别工具（desktop_ocr/locate/detect）输入 PNG 大小上限（同附件内联上限，超出报错引导压缩） |
| CV 输入降采样上限 | 长边 1280px | `GEBAI_CV_MAX_SIDE` 可调；超过等比缩小后识别，坐标仍映射回原始像素系 |
| OCR det 输入上限 | 长边 960（32 倍数） | det 前处理等比缩放 + 零填充（`DET_MAX_SIDE`，wasm CPU 亚秒级） |
| OCR rec 输入尺寸 | 48 高 × 320 宽 | rec 前处理等比缩放到 48 高、右侧零填充至 320 宽（`REC_HEIGHT`/`REC_WIDTH`，CTC 忽略尾部填充时间步）；**形状固定 → 可拼批**（见「OCR rec 批处理」） |
| OCR 输出行数上限 | 200 行 | desktop_ocr 输出超过截断，提示 find 参数过滤收窄 |
| YOLO letterbox / NMS | 640 边、IoU 0.45、conf 0.25 | desktop_detect 前处理边长 / NMS 阈值 / 置信度下限（conf 参数可调） |
| 模板匹配置信度阈值 | 0.8 | desktop_locate_image 默认 NCC 相似度阈值（threshold 参数可调；内部粗扫门限 0.4 仅作精化引导）；返回候选至多 5 个 |
| wait_for 轮询默认 | 超时 20s（上限 120）/ 间隔 2s / 变化判定差 2/255 | desktop_wait_for 默认参数（timeout_s/interval_s 可调）；change 模式灰度采样均差超阈值判定画面变化，超时不视为错误并返回最后观察状态 |
| LLM 流式读空闲超时 | 120 秒 | SSE 建立后连续无 chunk 判定接口假死中止本次调用（`LLM_IDLE_TIMEOUT_MS`，测试可注入 `llmIdleTimeoutMs`） |
| 模型单次响应输出上限 | anthropic 缺省 8192 / 其余接口缺省 | 单次响应输出 token 上限（`GEBAI_LLM_MAX_OUTPUT_TOKENS` 启动/任务级可配）：输出超限截断由引擎检测（`length`/`max_tokens`/Responses `incomplete`）并抢救落盘 + 引导 `write append` 分段续写（见「核心Agent流程」大文件分段写入与截断抢救）；Anthropic 接口强制要求 `max_tokens` 故有内置缺省 |
| 溢出护栏裁剪下限 | 500 字符 | 用户消息超过该长度才可被护栏裁剪为占位（短消息裁剪无收益；最新一条用户消息永不裁剪）；**每次降级经 `event.message.compact` 发布 `degraded` 事件**（UI 可见上下文为何变化，不再只写 console.warn） |
| subsession_run 子Agent 上限 | 5 个 | 单次子会话运行可预加载的子Agent 数量上限（去重后判定，`MAX_AGENTS_PER_RUN`） |
| WS 事件日志持久化 | `users/{user}/ws-journal.jsonl` | 日志尾部 JSONL 追加持久化（每 2000 条重写裁剪），重启后 seq 连续 |
| delta 合并窗口 | 50ms / 200KB 上限 | 同一消息的连续文本增量合并为单条日志事件（`DELTA_MERGE_MS`/`DELTA_MERGE_MAX_CHARS`） |
| WS 发送缓冲上限 | 16MB | 单连接发送缓冲超限判定慢客户端并断开（走自动重连 + seq 重放收敛，`WS_MAX_BUFFERED`） |
| 登录 IP 限流 | 60 突发/2每秒 + 10 突发/0.2每秒 | 登录/兑换端点全局桶与来源桶（`GEBAI_TRUST_PROXY=true` 按 X-Forwarded-For 区分来源） |
| 附件大小上限 | 无强制上限 | 附件上传端点**未做尺寸判定**（原文「20MB」无对应实现；代码中仅飞书图片 20MB 与浏览器桥响应体 20MB 两处，属不同场景） |
| 任务 tick 周期 | 30 秒 | 调度器检查周期（`TASK_TICK_INTERVAL_MS`：到期入队 + 队列推进） |
| 任务并发额度 | 每用户 5 | `GEBAI_TASK_MAX_CONCURRENT` 可调（`TASK_MAX_CONCURRENT_DEFAULT`）；定时/普通任务并行上限，闲时任务另受「队列空闲 + 每用户仅 1 个」约束 |
| 任务执行超时 | 脚本 5 分钟 / 提示词 30 分钟 | 单次执行缺省上限（`TASK_SCRIPT_TIMEOUT_MS`/`TASK_PROMPT_TIMEOUT_MS`，任务 `timeoutMs` 可覆盖，范围 1s~24h；提示词型到时 `engine.windDown`——先快速结束运行中的子会话拿结论，再取消会话任务） |
| 任务超时上下限 | 1 秒 / 24 小时 | `timeoutMs` 合法区间（`TASK_TIMEOUT_MIN_MS`/`TASK_TIMEOUT_MAX_MS`） |
| 任务输出保留 | 4000 / 8000 字符 | 任务记录保留输出长度 / 写入会话消息的脚本输出上限 |
| 任务执行记录 | 每任务 200 条 | 执行记录按文件落盘，保留上限 `TASK_RUNS_KEEP`（超出按时间删最旧）；`TASK_RUNS_HISTORY`（10）仅为展示默认条数；单记录文件读取上限 1 MB（`TASK_RUN_FILE_MAX_BYTES`） |
| 任务名长度上限 | 100 字符 | `TASK_NAME_MAX`（单用户条数上限 500，`TASK_MAX_ITEMS`） |
| 任务资源文件上限 | 4 MB | 单文件写入/读取上限（`TASK_FILE_MAX_BYTES`），递归列目录深度上限 6 |
| 用户待办失败上限 | 3 次 | 待办执行连续失败上限（`TODO_MAX_ATTEMPTS`，达上限停用绑定的闲时任务，`idleError` 记因待人工处理） |
| 任务通知正文/投递 | 2000 字符 / 10 秒 | 通知卡片单字段（执行结果摘要的输出/错误、`task_notify` 主动通知正文）的保留长度（`NOTIFY_TEXT_MAX`；卡片整体限 12000——`NOTIFY_CARD_MAX`，1.0 lark_md / 2.0 markdown 组件上限，与对话桥接 `truncateForFeishu` 同额）/ 通知 HTTP 投递超时（`NOTIFY_TIMEOUT_MS`） |
| show html 预览尺寸上限 | 4000 × 2000 px | `width`/`height` 显式预览尺寸上限，超限忽略回退默认 |
| 脚本桥调用总数上限 | 100 | 单次脚本（js/py 桥）内工具调用总数（`BRIDGE_TOOL_MAX_CALLS`；js 侧别名 `JS_TOOL_MAX_CALLS`） |
| 脚本桥字段截断 | 100k 字符 | 脚本桥单字段（output/data）截断（`BRIDGE_FIELD_CAP`，js 侧别名 `JS_RPC_FIELD_CAP`）；内层 blocks 透传上限 10（`BRIDGE_BLOCKS_CAP`） |
| py 桥常量 | 连接 8s / stdout 2MB / 协议行 2.5MB | 桥连接建立超时（`PY_BRIDGE_CONNECT_TIMEOUT_MS`，超时即降级纯脚本执行）/ stdout 捕获上限（`PY_STDOUT_CAP`，超出丢弃留注）与 stderr 尾部 8k（`PY_STDERR_TAIL`）/ 单条协议行上限（`PY_PROTOCOL_LINE_CAP`）；data 字段截断 100k（`PY_DATA_TEXT_CAP`）、返回值预览 2000（`PY_RESULT_PREVIEW_CHARS`） |
| js data 截断 | 100k 字符 | js 结构化 data 中 logs/result 截断（`JS_DATA_TEXT_CAP`，与 sh/py 对齐）；返回值预览 2000 字符（`JS_RESULT_PREVIEW_CHARS`） |
| js RPC 协议行上限 | 子进程 2MB / 服务端 2.5MB | 协议行截断兜底（防巨对象撑爆内存，超出注明截断） |
| js 会话上下文注入 | 最近 50 条 / 单条 2000 字符 | ctx.messages 快照条数与单条内容上限（`JS_CONTEXT_MESSAGES_MAX`/`JS_CONTEXT_MESSAGE_CHARS`） |
| js 动态工具上限 | 50 / 会话 | 会话内运行时定义工具数量上限（防注册风暴，`DYNAMIC_TOOLS_CAP`）；定义清单随会话 chat.json 落盘、重启恢复 |
| js 动态工具源码上限 | 100k 字符 | execute 源码长度上限（`JS_DYNAMIC_SOURCE_CAP`，源码随会话持久化，防撑爆 chat.json） |
| js 动态工具名 | `[a-z][a-z0-9_]{0,39}` | 运行时定义工具命名约束（与全局工具命名一致，`DYNAMIC_TOOL_NAME_RE`）；execute 源码 ≤ 2000 字符描述 |
| sh/py 结构化 data 文本上限 | 100k 字符 | `data.stdout`/`data.stderr` 超长截断（完整文本以 output 截断文件为准，`SCRIPT_DATA_TEXT_CAP`） |
| 后端图表渲染超时 | 20 秒（仅 plantuml） | `feishu-bot/plantuml.ts` 的 `PLANTUML_TIMEOUT_MS`（可注入）——**mermaid 与 d2 的后端渲染无超时**（`core/support/diagram-render.ts` 直接 await；20 秒超时只在前端本地渲染侧，见下行） |
| 后端图表输出尺寸上限 | 1600 × 2400 px | 后端渲染默认 2x 超采样，超出按比例缩放到该上限（防超大 PNG 超飞书图片限制；`DEFAULT_MAX_WIDTH`/`DEFAULT_MAX_HEIGHT`） |
| 图表语言 | `mermaid` / `plantuml` / `d2` / `echarts` | show 图表分支 `format` 参数四取值（SDK `DiagramFormat`，缺失/非法立即报错）；产物扩展名 `.mmd`/`.puml`/`.d2`/`.echarts`；前端本地渲染与后端组合渲染器（飞书/`render=backend`）均四语言全支持；echarts 源码为 option 的严格 JSON（双引号，容错注释/尾逗号）；服务端合法值域单点真相在 `artifacts.ts`（`DIAGRAM_FORMAT_VALUES` 派生自 `DIAGRAM_EXT_FOR` 键集，show 参数校验/schema enum/飞书桥接透传共用，SDK 新增语言漏项即编译报错） |
| ECharts 画布尺寸 | 默认 960×600，钳制 200-4000 px | echarts SSR 渲染必须显式宽高（无 DOM 测量）；信封 `width`/`height` 超范围钳制、非法值回退默认（前后端一致） |
| ECharts 源码形态 | 严格 JSON（键/字符串双引号），容错注释/尾逗号 | 不支持单引号/裸键名/`…`缩写（对象字面量不可用）；信封 `{"option":…,"width":…,"height":…}` 指定画布尺寸；解析失败错误信息点名常见病因（前后端一致） |
| ECharts 标题/图例避让 | 冲突图例下移至标题底边 +6px；图例底边越界时联动下调未显式设置的 `grid.top` | echarts 6 图例默认在画布底部，但模型常按 v5 习惯显式 `legend.top: 0/'top'/小数值` 置顶——与顶部标题（默认占带 15–46.6）必然压字；高度按默认值折算（标题行高 = 字号×1.2 + padding×2 + 副标题 itemGap+行高；图例单行 30、折行每行 +28，条目总宽按全角 1em/半角 0.6em 估算，条目名缺省取 series 名）；不干预：图例 top 未设置（默认底部）/非顶部区域、标题不在顶部、二者水平分居左右两侧、图例隐藏、显式 `grid.top`/`grid.height`（`fixEchartsLegendOverlap`，前后端解析入口统一应用） |
| show format 校验 | 缺失/非法立即报错 | 不静默回退 plantuml（防漏传 format 时源码被错误语言渲染、报错误导模型）；`path` 模式扩展名可推断时免传 |
| show echarts 通道加固 | 预校验 + 版本错位诊断 | 服务端 `parseEchartsInput` 预校验 JSON（无效立即报错不跑前端）；前端报错引擎与请求语言不符（旧版前端把 echarts 当 PlantUML 渲染）返回「前端渲染器版本过旧，请刷新页面」诊断，**不自动换通道**（前端渲染为默认正确通道） |
| D2 主题 ID | 0（亮）/ 200（暗） | 前端按 UI 明暗选择、后端固定浅色（0=Neutral Default、200=Dark Mauve，`diagram.ts`/`diagram-render.ts` 常量） |
| 前端本地渲染引擎 | mermaid（懒加载）/ @plantuml/core（懒加载）/ @terrastruct/d2（懒加载）/ echarts（懒加载） | 四语言零网络本地渲染；加载超时 30 秒（echarts 15 秒——约 1MB 体积小）、渲染/编译超时 20 秒（`diagram.ts`，引擎体积大慢机器加载可超 15 秒故放宽）；echarts SSR 模式（`init(null,…,{renderer:"svg",ssr:true})` + `renderToSVGString`）纯计算输出 SVG、无 DOM 挂载，缓存 key 含主题明暗（darkMode 注入）；**未知图表语言显式报错引导改用 `render=backend`（不静默回退 PlantUml——服务端新增语言而前端为旧版本时，回退会把源码当 PlantUML 渲染出误导性错误）**；**D2 前端单一串行队列**（浏览器构建单 Worker 共享 currentResolve，并发调用互相覆盖导致超时，`enqueueD2` 一次一个）；D2 编译错误 JSON 数组转可读文本（`formatD2Error` 提取 errmsg，前后端一致）；**动态分块加载失败自动整页刷新一次**（开发模式重建后旧页面引用旧 hash 分块 404，浏览器报 "Failed to fetch dynamically imported module"）；空闲预热仅限**本机实际用过的引擎**（痕迹记于 localStorage `gebai.diagram.engines`，`renderDiagramSvg` 分派时记录），无图表使用史的会话首屏不下载任何引擎（单引擎数 MB）；D2（8MB WASM）不预热 |
| 后端渲染引擎 | plantuml（TeaVM + DOM shim）/ mermaid + happy-dom / d2（`@terrastruct/d2` WASM）/ echarts（npm 包 SSR） | 四语言后端渲染（`core/support/diagram-render.ts`，飞书与 `render=backend` 共用）；懒加载（echarts 顶层急切导入——zrender 环境探测须先于 DOM 垫层污染）；单一串行队列；`globalThis.window` 仅临时存在（Bun worker_threads 冲突规避）；d2 二进制模式从内嵌产物（`packages/server/scripts/build-d2js.ts` 生成 JSON，gzip base64）物化 `{GEBAI_HOME}/vendor/d2js/{version}/` |
| show 复制上限 | 100MB | 会话 `tmp/` 外文件复制进会话文件区的尺寸上限（`SHOW_MAX_BYTES`），超出引导改为告知路径 |
| show 文本直显上限 | 读取 512KB / 内联 4 万字符 | 文本/代码内联 `code` 块的读取上限（`SHOW_TEXT_DIRECT_BYTES`，超出仅给查看/下载卡片）与截断阈值（`SHOW_TEXT_MAX_CHARS`，超出截断展示 + 附 file 卡片取全文） |
| D2.js 内嵌产物 | `packages/server/src/core/d2js.embedded.generated.json`（gitignore） | 构建时 `packages/server/scripts/build-d2js.ts` 生成；node-esm 构建 7 文件 gzip base64（22MB wasm → 9.9MB）；构建命令 `--external @terrastruct/d2` 保持运行时文件路径 Worker 可用 |
| 粘底阈值 | 64px | 距底 ≤ 该值视为在底部（微调容忍区：`scroll-intent.ts` 的 `threshold`；在此范围内上翻不解除跟随） |
| 恢复跟随的距离 | 4px | `scroll-intent.ts` 的 `resumePx`：需**真正到底**才恢复跟随——触摸板/触屏能精确停在容忍区内，在那里恢复会把「往回调一点继续读」误锁为「想追最新」 |
| 上翻确认时长 | 60ms | 位置离开底部且有向上位移后，确认期满仍离开才解除（回弹/过冲豁免；停手无后续事件时靠此定时器复查） |
| 上翻宽限 | 120ms | 收到上翻输入（滚轮上滑/触摸上滑）后，期间不做程序回正（滚动是合成器异步派发的，内容增长的 rAF 可能先行） |
| 特效画布分辨率上限 | 1x | `theme-fx.ts` 的 `FX_MAX_DPR`（相对 CSS 像素）：全屏读写代价随其平方增长，高 dpr 屏按 2x 铺满时矩阵逐帧全屏渐隐需读写 24MB/帧 |
| 特效运行中帧率 | 24fps | `FX_BUSY_FPS`：当前会话有任务在跑（`data-fx-busy`）时降频，与输入档取更慢者 |
| 矩阵全屏渐隐帧率 | ≤30fps | `FX_FADE_FPS`：渐隐量按累计 dt 算，降频视觉等效；全屏半透明读改写是该特效最贵一步 |
