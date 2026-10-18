# 总纲与工程结构

> 本册是 `DESIGN.md` 的分册，**章节号与主文件保持一致**（源码注释按 `DESIGN.md §x.y` 引用章节号，不重编号）。
> 索引与全貌见 [`../DESIGN.md`](../DESIGN.md)。

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
| `@gebai/web` | `packages/web/` | Web UI：Vite 构建，打包进二进制作为内置前端。**多入口**：`index.html`（聊天页）+ `files.html`（**文件工作台** `/files`，源码 `src/files/{main,explorer,editor,viewers,git,git-graph,compare,ui,api,changes,merge,merge-view,git-shared,url-state,deeplink}.ts` + `src/files-entry.ts` 标题栏入口按钮）；**按钮轮盘共用原语** `src/wheel-core.ts`（**按空间自动分圈 + 角窗口内均布**的扇形几何 + hover 保持区 + 开合时序，样式 `css/wheel.css`）——聊天页标题栏入口（`src/wheel.ts`）与工作台编辑器动作区（`files/main.ts` 的 `tabActions`）共用；Monaco 经 `public/vendor/monaco` 静态伺服（同 diagram 引擎惯例，不进 vite 打包）；xterm.js 同径（`public/vendor/xterm`，终端面板内核，运行时 ESM 动态 import） |
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
| `tools/` | **开发期复核工具**（不入任何包的构建/发布链路，但**入库**）：`tools/mutation/status-core/`——变异台（`mutants.ts` 清单 + `bench.ts` 执行器 + `stamp.ts` 证据指纹），`bun run mutate:status` 一条命令跑；`tools/tsconfig.json` 接进根 `typecheck`（`bun run typecheck:tools`）。与 `scripts/` 的分工：`scripts/` 服务于构建/发布/E2E 编排（会被 `build`/`typecheck` 依赖），`tools/` 是**按需人工复核**的分析器（不接进 `build`）。 **与防忘闸门的关系**：变异台本身不进任何门禁（理由见「测试策略 → 变异验证」），但它跑成功时写下的 `stamp.json` **会被**常规测试 `packages/web/src/files/status-mutation-gate.test.ts` 对账——闸门在那个包里、走常规 `bun test`；故「tools/ 不入构建链路」与「它的证据要被人看见」两件事并不冲突 |

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

模型权重落 `{GEBAI_HOME}/resources/models/infer/`（资源子仓库约定）。运维与调用入口为 `llm_infer` 子Agent（见「更多内置子Agent」）；服务本身是标准 OpenAI 兼容端点，可直接作为 `GEBAI_LLM_ROUTES` 中的一路本地算力，也可被其他客户端直接调用。

**统一接口层（四块，职责不重叠）**：

- **引擎层（`engines.ts` + `config/engines.json`）**：回答「用哪个 llama.cpp、跑在什么设备上、从哪来」——按本机 `platform+arch` 过滤矩阵、按设备探测（`nvidia-smi` / 设备节点 / `vulkaninfo` / darwin 判 Metal）给可用后端与依据，缺引擎时直接从发行版资产**下载 + 校验 + 解压 + 定位可执行文件 + 写安装标记**（zip 用 fflate，tar.gz 交系统 tar；下载支持断点续传与 sha256/字节数校验，先写 `.part` 再 rename，不留半成品）。归档内部布局各平台不同（如 Linux 是 `<tag>/llama-server`、Windows 在 `build/bin`），因此可执行文件**递归定位**而非写死路径。
- **源码编译层（`sourcefind.ts` + `sourcebuild.ts`，内网/离线主力）**：回答「拿不到预编译包时怎么办」——**发现源码 → 就地编译 → 装成引擎**，产物与下载安装的引擎同一落点（`vendor/<engine-id>/`）与同一标记格式，因此启动/状态/档位对其无感知。发现根优先级：`GEBAI_LLM_INFER_SOURCE_DIRS` → `{GEBAI_HOME}/resources/src/` → `{GEBAI_HOME}/resources/engines/` → `<infer>/engine/`；目录与归档都支持，归档**只预览不解压**（条目数/顶层/是否含 `CMakeLists.txt`/**是否自带 `vendor/` 依赖**）——后者是离线可行性的关键信号（llama.cpp 的 CMake 缺 vendor 时会尝试联网 FetchContent，内网表现为长时间挂起），故采用**全量过滤**而非采样判定（该归档 3999 条里 vendor 在最后几十条，采样会假阴性）；解压延迟到编译时（落 `vendor/.cache/src/<id>/`，幂等复用）。编译用 llama.cpp 专用 CMake 预设（按后端只开目标开关 + `-DLLAMA_BUILD_SERVER=ON -DLLAMA_CURL=OFF`，`Ninja` 优先），安装时复制可执行文件所在目录并**解引用符号链接**（`build/bin/` 里是 `libggml-base.so → .so.0 → .so.0.25.3` 链，漏拷即起不来），标记里额外记 `build.{from_source,source_dir,build_dir,device,offline,log,commit}`；工具链探测同时给出各工具的**绝对路径**（便携工具链放 `resources/toolchain/bin` 无需改 PATH 即可被编译层调用），且**探测命令与编译命令同基准带上会话 PATH**——否则会出现「探测说缺工具、构建时又用得上」的自相矛盾（把便携工具链挂到会话 PATH 正是内网常见做法）；PATH 传输键名按平台惯例（Windows `Path` / POSIX `PATH`，同名不同大小写的键并存时子进程取到哪个取决于实现，PATH 收敛会静默失效）。
- **进程管理层（`launcher.ts`）**：回答「怎么起、怎么停」——**默认路径不再依赖 PowerShell**：`detached` 拉起 + stdout/stderr 重定向到日志文件（不继承调用方 stdio，否则工具会挂在长驻子进程上）+ `/health` 就绪轮询 + 按 PID/端口精确终止；平台命令构造全部收敛为「显式平台参数的纯函数」（win32/POSIX 双轨可测，断言不随宿主漂移）。Windows 上无已装引擎且未显式要求 launcher 时回退 `infer/scripts/run-server.ps1`（GPU 档位老路径，`GEBAI_LLM_INFER_LAUNCH=script` 可强制）。
- **推理目标层（`providers.ts`，调用面）**：回答「往哪发任务」——统一为 OpenAI 兼容调用面，目标三类：`local`（本机受管服务，端口取状态文件 > 环境变量 > 8080）、直连 URL（`target="http://host:port"`，可带 api_key）、**命名目标**（`GEBAI_LLM_INFER_TARGETS` JSON 声明，含密钥与模型名）。因此同一套 `generate/batch/jobs` 可以把任务派到本机 CPU 引擎、局域网另一台机器或云端兼容端点；密钥在输出与产物中一律掩码。

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
| 聊天页 | 会话消息流、流式渲染（**markdown 链接一律新标签页打开**：`target="_blank"` + `rel="noopener noreferrer"`，防止原地跳转打断会话使用；**GFM 任务列表渲染为勾选框**——markdown-it 无内置 task list 支持，`applyTaskLists` 渲染后把列表项行首 `- [ ]`/`- [x]` 的 `[ ]` 占位转为勾选框 span（`li.task-item` 去圆点、`task-box(.done)` 打勾，紧凑/松散列表与有序任务项均覆盖），计划勾选清单不再泄漏字面 `[]`；**markdown 内图表围栏兜底渲染**——正文里的 ` ```mermaid `/` ```plantuml `/` ```d2 `/` ```echarts ` 代码块不经 `show` 工具也渲染为图表卡片（与 `diagram` 内容块共用本地引擎与全屏查看器，源码可在查看器内查看；源码为空或围栏未闭合（流式半成品）的块跳过，闭合后出图）；**流式文本 120ms 尾沿节流渲染**（所有模式统一，markdown 全量重解析是流式期间最重的 CPU 开销，逐 chunk 同步渲染在长回答下 O(n²)））、附件上传、工具调用/审批卡片（**审批卡片为紧凑居中卡片**：限宽 ≤460px 居中显示，**仅展示工具名**（参数已在消息流工具卡片中展示，不重复），工具名单行省略号截断、悬浮 title 可见全名，避免长工具名撑成长条；**键盘快捷键 Y = 通过、N = 拒绝**，作用于最早等待的卡片，带修饰键或长按不触发，按钮内以弱化小字提示快捷键（矩阵主题沿用其 [Y]/[N] 前缀样式）；**工具调用即建卡**——参数先展示、结果到达后再追加到同一卡片（无参数调用仅头部，同样调用即建卡，不在执行完成后一并出现），输出完整不折叠）、压缩通知；**自动标题**：首条消息**发送时点**即以首个用户输入生成会话标题（压缩空白后截 50 字符、超出省略号——侧栏/标题栏按容器宽度自行省略，落盘名只防超长输入整段入库；仅默认标题「新会话」时命名，不覆盖自定义名；发送时点内存首条输入缺失——如页面刷新——回退历史首条用户消息（子会话执行存档不算）；改名失败不标记，任务结束兜底重试）；**输入历史**：输入框 ↑/↓ 键浏览**用户级全局输入历史**（跨会话通用、localStorage `gebai.ui.inputHistory` 持久化，按最后使用去重排序，同一输入只保留一条、限 50 条；空输入时 ↑ 进入、↓ 可恢复原草稿）；**多会话后台运行**：切换会话不中断进行中的回答，流式文本按会话累积，切回时恢复渲染（**推理累积同样恢复**）；**主循环轮界封段**（`stream.ts` text 分支 messageId 变化即封存上一段并清空累积）——后台会话工具事件不渲染卡片（`onToolCall` 开头即早退）从而工具调用处不封段，若无轮界检测，`run.acc` 跨轮只增不减，切回时整任务多轮文本渲染进同一张流式卡片（新回复追加在前面的卡片内，修复前缺陷）；前台另有工具调用封段双保险，重连 `resume` 重置轮界基准（重放不误判）；**输入草稿与附件按会话保存/恢复**（切换保存、切回恢复，新会话/删除会话清理——A 的草稿不会误发到 B，附件上传绑定发起会话 id 不受切换影响）；**选择/环境变量填值卡片与审批卡片同构**（渲染到审批容器，切走隐藏、切回恢复，不随消息重载丢失；任务结束随审批一并清理；**同一请求 id 重复推送（断线重连事件 seq 重放）替换旧卡不堆叠**；show 图表渲染与 page_capture 为页面级操作，后台会话同样执行不回传失败）；**ask 问答记录卡**（调用时中断当前文本段（封段）；等待作答期间消息流不渲染问题预览——交互作答由审批容器选择卡片承载，避免同款选择卡上下重复，结果到达时在消息流落问答记录卡：问题 + 选项展示态 + 「✓ 用户回答 / ✕ 用户拒绝 / ⏱ 选择超时」结果头部与回答文本，模型后续回复另起新气泡；历史重载按参数 + 结果同构渲染记录卡；子会话运行容器内同构渲染）；**ask 计划卡片**（同构于选项询问的延迟落卡：**等待期消息流不渲染计划卡**——计划全文由审批容器选择卡内嵌承载（`event.choice.request` 携带 `plan` 载荷：标题/Markdown 全文/路径，卡内顶部限高滚动展示，审批时直接可见；**带计划的选择卡加 `has-plan` 类跟随内容区宽度**——`.msg` 的 `width:fit-content` 会按最短计划行把卡收缩成窄条，`msg-body` 以 `flex:1` 撑满（同 html-card 先例）；上下两张同款计划卡会被视为重复），批准执行/拒绝执行/自定义修改意见均在选择卡上作答；**结果到达时**消息流落计划卡（标题 + 计划 Markdown 全文，`# 标题` + `- [ ] 步骤` 勾选清单或模型直传 content，双端同构拼装），头部更新为「✓ 计划已批准 / ✕ 计划已拒绝 / ✕ 计划已取消 / ⏱ 计划审批超时」并追加结果文本；历史重载按持久化参数同构渲染计划全文、按结果文本呈现审批结果态）；**工具调用配对按 `会话:toolCallId` 隔离**（toolCallId 跨会话不保证唯一；切回会话时重建运行中卡片的 DOM 引用，后续结果仍追加同一卡片，任务结束清理残留配对）；**滚动位置跨会话记忆**（离开时未粘底则切回恢复原阅读位置，粘底/新会话落底）；**后台流结束不抢焦点**（焦点守卫：仅发起会话仍为当前会话时恢复输入焦点）；**撤回确认期间切换会话不打断当前浏览**（确认后校验会话归属再重载视图/回填输入框）；会话切换失败（网络抖动）回滚当前会话标记并提示，不留下状态不一致；审批卡片绑定来源会话，**仅显示当前会话的待审批卡片**（切走隐藏、切回恢复；后台会话的审批不打断当前会话）；**当前会话有待审批时锁定页面输入**（输入框/发送/附件禁用，焦点落到审批卡片，全部处理完恢复输入焦点），键盘 Y/N 处理；**任务结束（完成/取消/拒绝）时该会话残留审批卡片随任务终止清理**（审批已失效，避免卡片残留锁死输入）；**运行中输入排队**（见「会话输入队列」）：运行中 Enter/发送 = 输入**入本地会话输入队列**（不进消息流，呈现于输入框上方**排队条**——每条含序号/内容预览/附件计数与「⚡立即执行（中断当前任务）/✎编辑（内联）/✕撤回」操作，前端本地状态维护、当前回答结束后自动按序发送下一条），**Ctrl+Enter = 中断插入**（插队首并取消当前循环后立即执行本条），运行中发送按钮随草稿有无切换「排队发送（箭头）/停止（方块）」（空输入点击 = 停止，停止只取消当前任务、队列继续）；**消息撤回**：用户与**助手消息**均有撤回按钮，按**消息 id** 删除该消息及其后续（发送时前端携带 `messageId`、服务端采用同一 id 持久化，撤回对当前会话刚发的消息同样生效；助手最终回复以流式 messageId 落盘（引擎最终轮消息 id 与增量推送一致）——刚完成的回复即可撤回，撤回中途 assistant(toolCalls) 消息连带其工具结果与后续回复一并删除；用户消息撤回后内容回填输入框，助手消息撤回后聚焦输入框便于直接输入指导修正；**任务运行中不可撤回**——前端拦截提示 + 服务端 409 拒绝（运行中任务持有自己的上下文快照并继续追加消息，中途截断会产生交错历史，先停止或等任务完成）；新会话容器内回放消息与本地收尾说明气泡不提供撤回（id 为本地生成、无服务端落点）；撤回同时清理 usage 基线锚点（索引随删除错位，回退估算由下次真实调用重建）；失败 Toast 提示）；**会话内消息导航**：消息流右侧窄导航列（**消息流滚动条自动显隐**——与会话列表同构：沟槽恒定占位（`scrollbar-gutter: stable`，消息宽度不随滚动条显隐伸缩）、静止透明，hover 或滚动中（`.scrolling` 由共用助手 `ui.ts autoHideScrollbar` 维护：scroll 事件加类、停止 400ms 后移除）才显示滑块；导航列右移避让不变；曾整体隐藏滚动条、也曾长期常显），**只为用户输入**建一条短横线（等间距紧凑聚于导航列中部；**静止态 2px 细线**（1px 过细已加粗；125%/150% 缩放下 2/3px 交替肉眼可忽略），hover/focus 加粗至 3px 拉长变亮并浮出消息预览气泡——用户消息省略"我"的称谓标签），点击 / 键盘 / 拖动跳转（**点击与拖动经按下-阈值手势判定（`press-gesture.ts` 纯逻辑工厂）**——按下只记起点，位移超 6px 才进入拖动搓动并接管指针 capture，未拖动松开 = 明确点击，系统取消（pointercancel）只复位不触发点击；短横线 `::before` 扩展命中区收窄至上下 3px/左右 4px——只补足 2px 细线可点性，与相邻步长 18px 不连成竖带。历史缺陷：命中区曾扩至上下 8px/左右 12px，相邻短横线命中区垂直无缝连成右侧**连绵命中带**，而导航列是 `#messages` 的**兄弟元素、不在其滚动链上**——悬停带内滚轮无任何滚动目标（「滚轮失灵」），用户转而点击/拖动立即触发 pointerdown 跳转（偏上位置 = 跳顶）+ pointer capture 劫持后续拖动，形成「锁在底部→一滚就跳顶→滚不动→只能点跳到最新按钮」的偶现锁死链。现三层防护：① 命中区收窄打散连绵带（间隙命中 `#messages` 滚轮原生有效）；② 导航列上的 wheel 转发给消息列（`deltaMode` 行/页单位换算，上滚同步解除粘底跟随——与 `#messages` 自身 wheel 监听同语义，防流式静默窗口把手动落位归因为内部动作拽回底部）；③ 阈值手势消除轻触即跳与 capture 劫持）；**滚动时高亮当前位置**（视口中心所在消息的短横线加长至 20px（普通 16px 与 hover 30px 之间）且加亮，同色无橙色区分；激活区域按「输入开始 → 下一条输入开始」划分，整段输出归属其输入，不按输出中点平分）；**连接韧性**：WS 断开自动指数退避重连（1s→30s，主动登出不重连），连接状态为**纯圆点信号灯**（位于标题栏右端、上下文占比圆环的圆心；**圆点绘制于圆环同一 SVG 内**（`.conn-dot` 第三圆，状态经 `#header-ctx:has(#conn.*)` 桥接、`#conn` 仅作状态载体——原 DOM `::before` 圆点在分数布局原点（44px 头部 - 1px 边框）上光栅化，100% 缩放下相对圆环恒偏半像素，同坐标系绘制后任何 DPI/主题下严格同心））——已连接且无生成任务时**隐藏**（无文字无胶囊），**当前会话运行（思考/流式生成）时显示并快速闪烁**（仅跟随当前会话——多会话后台运行的信号灯不打扰当前视图，切换会话经 `setCurrentSession` 联动刷新）（主题色圆点；流 150s 无数据视为挂起：前端主动中断并清理运行态，防信号灯残留常闪——阈值高于服务端 LLM 读空闲超时 120s，模型调用假死先由服务端超时上报明确错误，前端看门狗只兜底服务端检测不到的挂起；**看门狗中止且无任何输出时渲染「生成超时」说明气泡，不再静默结束**；**交互等待（选择/填值/画图/捕获）刷新活跃时间，等待用户回应的挂起不误判挂起**），断开/异常显示红色圆点常亮（圆心放不下原文字徽章，具体信息并入圆环悬浮——**整个圆环区域均为悬浮区**，断开原因置 tip 首行、上下文数值随其后；`#conn` 铺满圆环仅作状态载体不挂 data-tip，防其自有 tip 遮蔽整环的占比数值悬浮）；**无输出收尾兜底**：任务结束/失败但没有任何可见输出（无文本/推理/工具卡/错误气泡）时补渲染说明气泡（「任务已结束，但没有收到任何回复内容」/错误原因），杜绝「无声无息就结束」；**消息发送走 WS 单通道**（`sendPrompt` 经 WS 发起任务、事件流渲染：文本/推理增量与审批/工具调用/结果统一由 `event.*` 推送驱动；**WS 断开时进行中的流挂起等待重连，重连后按事件 seq 重放离线内容无缝续流**——`resume` chunk 重置当前消息元素防重复渲染；长断线（>150s 无数据）由空闲超时兜底取消任务）；**按钮轮盘**：导出 ⤓（**导出当前会话为 JSON 往返文件**，与轮盘「导入」同一格式——导出的文件能再导回）/ 导入会话 ⤒ / 待办 / 任务（会话操作组）与自动审批 ⚡ / 设置 ⚙ / 主题 / 登出（应用操作组）低频操作按钮收敛为**标题栏最右按钮轮盘**（九宫格网格入口，平时仅显示此一个按钮，悬浮展开为扇形）：**按空间自动分圈的扇形**——首圈 r=85（会话操作组：分组边界必换圈，所以会话操作组与应用操作组不会混在同一圈；组内 accent 边框作组标识、背景与扇形按钮一致——不用 accent 调底，避免与开关开启态 accent-soft 底混淆）；之后每圈步进 = ceil((边长·√2 + 间隙)/2)·2（**按最坏情况 45° 对角取值**，同角度也不叠），**组内均衡分圈**（先算最少圈数再把项数均分，避免孤项圈），每圈在**自己的角窗口**内均布（下界 = 右缘不越屏、上界 = 不压入口行；两约束不对称故围绕窗口中心而非全局轴心）、跨圈整圈滑动避让，每圈容量按该圈角窗口算、放不下的自动落到下一圈；**每圈画一条引导弧线**（半径 = 该圈半径、穿过按钮圆心、画在按钮之下，看得见的是按钮之间那几段短弧），圈数 = 弧线条数；**可用空间约束**：以入口为圆心按视口左/下边界（margin 8px）算出**最大半径**，半径再往外就超空间的圈不再开，剩余项并进最后一圈（宁可挤一点也不出屏、不压入口行；入口贴近屏幕左缘时 6 项全落在首圈）；（标题栏高 40px，扇形只能朝左下方展开，全部按钮不被标题栏遮挡）；纯 hover 交互（不支持点击切换）：入口 hover **立即展开（无延迟）**、**指针在扇形区域内保持展开（保持区=入口按钮∪扇形按钮的边界盒外扩 8px，按可见按钮动态收紧），离开区域 250ms 延迟收起**，外点/Esc/窗口 resize 关闭；展开时扇形按钮自圆心错落弹出（translate+scale+opacity 过渡）；扇形按钮**双背景垫实**（本体与 ::before 各铺一层 `--bg-elev-2`，半透明主题双层叠加 ~95% 不透明、不透明主题本就实心，全主题表面接近实色）+ 文字色边框阴影；**扇形按钮图标统一 `--text`**（accent 图标叠 accent 底相对亮度差被抹平——亮色主题（acrylic 亮/ink/cny）仅 2:1~4:1 不可读，accent 仅作底色/边框组标识，图标全主题 ≥7:1）；**轮盘内开关类按钮（自动审批）开启态同款处理**（accent-soft 底 + accent 边框作"开"标识、图标保持 `--text`——主题 `[data-theme] .icon-btn.active` 的 accent 染色同样会让亮色主题看不清）；**入口按钮静止态无底色、无边框**（仅 `--text` 亮图标，标题栏内靠图标本身可辨——`.icon-btn` 基线静止态无底色（背景 none/边框 transparent/图标 muted）、各主题仅处理 hover/active，故入口按钮以 `#wheel-btn.icon-btn`（1,1,0）兜底压过所有主题只补图标亮色；hover 底色 `--bg-hover`、展开态 `.active` accent 底，图标恒用 `--text`）；6 按钮由隐藏源容器移入（事件绑定在元素上不失效），`logout` 显隐仍由 auth 控制；标题栏保留折叠按钮/会话标题/**上下文圆环 + 轮盘入口构成右侧固定组**（`.header-right` 容器单一 `margin-left:auto` 整体靠右——ctx 与入口各自带 auto margin 会被 flex 平分剩余空间、把 ctx 拉向中线与居中标题重叠；ctx 隐藏时组容器仍占据右端、入口不左移）/**上下文占比圆环**（当前会话 ctxTokens / 模型窗口：24px SVG 圆环弧线（`pathLength=100` 归一化周长，`stroke-dashoffset=100-占比` 免换算）+ **圆心连接信号灯**（见「连接韧性」），悬浮圆环 data-tip 显示精确值与百分比（**整个圆环区域均为悬浮区**——`#header-ctx` 统一挂 tip，圆心信号灯 `#conn` 铺满圆环但不拦截：思考闪烁/断开态悬浮不打折，断开原因置首行），**接口返回缓存字段时追加第二行「缓存命中 tokens（占比）」**（`ctxCachedTokens`：同一次调用的提示词缓存命中，多行 tip 走 `.tip.multiline` 换行展示；接口不返回缓存字段时无此行）；**切换会话即切换**，运行中随 `event.session.ctx` 每轮实时更新；弧线比例分级着色 <50% 主题色 / 50-80% 警告色 / ≥80% 危险色；窗口大小来自快照 `maxContextTokens`，未知时隐藏圆环仅剩圆心信号灯）；**会话搜索框带图标与占位提示**（放大镜 SVG 图标 +「搜索会话」文字提示，聚焦时图标随边框变 accent 色）；**发送按钮为圆角上箭头图标**（线性描边，与停止方块同控显隐）；**新会话懒创建（草稿页）**：点击「＋ 新会话」/登录后无会话/删除全部会话的兜底均只进入**空白草稿页**（不落盘创建会话——避免每次点击产生一个空会话堆积），首条消息发送时才真正创建会话（创建失败保留输入并提示）；草稿态由显式标志维护，迟到的服务端快照不把草稿页覆盖回服务端记忆的当前会话；**空白页快捷按钮**：唯一管理入口在空白页——内置「查看子代理和工具」胶囊（与用户快捷同级样式、不可删，点击填入提示词由模型编排工具作答）居首；用户胶囊为**单按钮样式**（无嵌套边框），点击把内容填入输入框、hover 浮层预览完整内容、右上角显现 ✕ 徽章删除（确认弹窗）；末尾「＋ 添加」虚线胶囊弹窗新增（标题+内容，内容支持多行——Enter 换行、Ctrl+Enter 提交，Esc/遮罩取消）；localStorage `gebai.ui.shortcuts` 仅存本浏览器 |
| 会话列表 | 新建/切换/删除会话（**草稿页跨刷新保持**——进入草稿页即清除 localStorage 记忆的上次会话（`gebai.ui.session`），刷新后保持空白草稿页而非跳回旧会话，首条消息发送时才创建会话的懒创建语义与刷新恢复解耦；**新会话快捷键 Alt+N**——与「＋」按钮等效进入草稿页，已处于草稿页时无操作防误触清草稿，批量模式随按钮禁用；`Alt+N` 而非 `Ctrl+N`：后者是 Chromium 保留命令（浏览器开新窗口，页面收不到按键）），双击会话名重命名，上下文压缩入口（标题栏轮盘 🗜️）；**行内仅保留选中按钮**：每个会话行时间区有一个**勾选框（选中按钮）**——**默认隐藏**，右键菜单「选中」进入多选（批量）模式后常驻显示，点击勾选/取消（多选统一走该按钮）；✎/✕ 操作按钮已移除，**重命名/删除/复制会话 ID/导出（Markdown 与 JSON 会话文件双格式）收敛到右键菜单**（屏蔽浏览器默认菜单，随光标定位 + 视口边缘翻转，Esc/点击/会话列表自身滚动/新右键关闭（**「随滚动关闭」按锚点容器判定**——`scroll-dismiss.ts` 的 `dismissOnScroll`（纯函数带单测）只在会话列表自身（或其内元素）或页面级滚动时收菜单：生成中的消息流为咬住底部每帧写 `scrollTop`，那是**另一个容器**、fixed 菜单并不会跟着漂移，按「任意滚动一律关闭」会让菜单在运行中会话里刚打开就被冲掉（表现为右键闪退），与 tooltip 的滚动隐藏同口径；见「自研交互组件」）；「复制会话 ID」任意条目可用、Toast 反馈——定位问题/反馈用；「导出会话」下载单会话 JSON（文件名 = `gebai-session-{会话 ID}.json`——**以 ID 而非标题命名**：标题可能出现超长/特殊字符/重名/Windows 保留名，ID 为 32 位小写 hex、天然无非法字符且长度恒定；批量包格式、可经轮盘导入回歌白），「导出 Markdown」为人读版（仍按标题命名，只供阅读不进导入通道）；批量模式下菜单项变为「选择/取消选择」）；**滚动条占位恒定**：滚动条沟槽恒定保留（`scrollbar-gutter: stable`，静止透明、hover/滑动才显色），会话条目宽度不随 hover 伸缩（覆盖式滚动条环境此前 hover 时条目无端变长 8px、离开缩回）；**批量删除**：右键「选中」进入多选（批量）模式（批量模式下点击行也切换选中），批量模式下操作条显示已选计数与「导出所选」（选中会话打包单 JSON 文件，与右键导出/轮盘导入同格式联动），删除所选走确认弹窗逐个删除；全部取消自动退出批量模式；当前会话被删自动切换到剩余第一个或新建；**删除成功后重建列表**；**列表渲染两条约定**：① 行内交互（点击切换 / 双击重命名 / 右键菜单 / 勾选）统一由 `#session-list` **容器级事件委托**分发（`click`/`dblclick`/`contextmenu` 各一个监听器），监听器数量不随会话行数线性增长；② `refreshSessions` 先比对**列表结构签名**（搜索词 + 批量态 + 折叠组 + 各行的 id/名称/置顶/更新时间），签名未变时**跳过整列重建**、仅同步当前会话的 active 高亮——切换会话不再空重建整列 DOM；**刷新一律拉新数据，不用 lastSessions 陈旧数组重流**：任务开始/结束的列表刷新（`markSessionRunning`）不带 preloaded 发起——preloaded 路径不发请求且 seq 更大，与自动命名刷新（`maybeAutoTitle` rename 后的 `refreshSessions`，携新名但请求在途）并发时会把它用 seq 守卫丢弃，列表停留旧名（如「新会话」）直到手动刷新才恢复；**按日期分组**：列表按更新时间分组为 今天/昨天/近7天/更早（组内按更新时间倒序），**组头点击折叠/展开**（折叠组不渲染成员，箭头旋转指示，折叠状态 localStorage `gebai.ui.sessionsCollapsed` 记忆；批量模式下强制全展开——折叠分组全部可见可勾选，**组头点击切换全组选中/取消**；搜索时平铺不分组）；**整栏折叠**：桌面端（>860px）标题栏最左按钮折叠/展开整个会话列表（折叠隐藏侧栏、主区占满，状态 `gebai.ui.sidebarCollapsed` 持久化；**Ctrl+B 快捷键等效切换**），窄屏维持滑动抽屉行为 |
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
- **自绘浮层的「随滚动关闭」口径（`packages/web/src/scroll-dismiss.ts`）**：浮层是 fixed 定位，只会随**宿主所在的滚动容器**漂移——判定为「该容器自身（或其内元素）滚动、或页面级滚动」才关闭，与浮层无关的**其他容器**的**程序滚动**不收浮层。会话右键菜单、tooltip 的滚动隐藏均按此口径（后者在 `ui.ts` 的 `bindTooltips` 内联同义判定）。反例（实际踩过）：生成中的消息流为咬住底部每帧写 `msgEl.scrollTop`，事件冒泡到 document，若按「任意滚动一律关」，运行中会话里右键菜单刚打开就被冲掉（表现为闪退）。
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
  | `acrylic` | 亚克力（默认）：黑色半透明面板 + 毛玻璃（backdrop blur）、近黑底极弱光晕，**会话元素（列表条目/搜索框等）零边框**，纯靠背景层次与圆角区分；**低功耗模式（全站关闭毛玻璃）下浮层（主题面板、设置抽屉、预览、待办、下拉、确认框、右键菜单）改为实底**——该模式下无模糊兜底、半透明浮层会透出背后正文，实底同时也更省；**浮层名单必须含文件工作台的浮层**（`.fw-dialog` / `.fw-qo` / `.fw-toast`）——它们的底也是半透明令牌（`--bg-elev`/`--bg-elev-2`，暗 0.82 / 亮 0.8），漏在名单外的后果是**没有毛玻璃**（亚克力是唯一 `--bg-elev` 半透明的主题，故只有它现身）：背后的正文清晰透上来与浮层文字叠着（实测白色亚克力下文件历史弹窗几乎没法读），故 `style-contract.test.ts` 用一条契约钉住这三类；支持**黑白（暗/亮）切换**；不配环境特效（保持毛玻璃原味） |
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
- **页面标题定制（`GEBAI_TITLE`）**：浏览器 tab 标题的品牌化入口，与 `GEBAI_UI_STYLE` 同一注入链路——服务端 `routes/static.ts` 渲染入口 HTML（`/` 与 `/files`）时双点替换：① 静态 `<title>`（`files` 页保留「文件工作台 · 」前缀、只替换名字，其余页面整条替换；HTML 实体转义）② 运行时 `window.__GEBAI_TITLE__`（JSON 序列化 + `<` 转义为 `\u003c`，防标题含 `</script>` 截断页面），前端 `updateTitle()`（会话切换/运行时反复执行）模块加载期固化该值、空白视为未设置回落内置默认「歌白」——两路一致，运行时不会把注入标题改回去。PWA 安装名（manifest）与站内品牌文字不跟随
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
  - **消息区窗口化（全模式生效）**：`#messages` 的直接子节点是 `[.vz-spacer.top] [挂载中的块 `.vz-block` …] [.vz-spacer.bottom] [尾部活动区节点…]`——只有视口附近的块留在 DOM，远离视口的块整体卸载（块容器引用常驻，重挂零重建），高度由 spacer 按「实测记忆 + 定稿估高」承担；折叠 k 个块为 1 个 spacer 时补回少掉的 k-1 份间距，故折叠态与全渲染态总高度一致（滚动条长度不漂移）。**DOM 规模恒定**（约视口 ×3），布局/样式/绘制/流式 MutationObserver 开销不再随会话长度增长。滚动位置由「贴底保持 + **元素级滚动锚定**」保证（贴底时不因内容增减离开底部，阅读中保持视口内容不动）：自管一套代替被关闭的 `overflow-anchor` 的锚定——维护一条基线（**视口顶部所在元素**的屏幕位置 + 当时的 scrollTop，元素从容器直接子节点**逐层下钻到跨过视口顶部的后代**，故图片/图表/懒渲染导致的**块内长高**同样可察觉；块级锚点察觉不到），任何布局变更后用「实际位移 − 用户滚动量」得出纯布局位移补回 scrollTop——**只补增量、不写绝对位置**（绝对写回会把「渲染超大卡片期间用户继续滚动」的那段滚动抹掉，表现为跳）。**挂载/卸载/落 spacer 的顺序**：先挂载 → 再落 spacer → 最后卸载（先摘 DOM 会让布局瞬时变短，浏览器把 scrollTop 钳到临时上限、每次回退一个滚轮步长，并让挂载区间来回翻转、同一块反复挂载/卸载）。**挂载位置按槽位顺序插入**（插到下一个已挂载的更大序号块之前）——向上扩窗时新块序号更小，只往尾 spacer 前插会落到已挂载块之后，块自身空间与上方 spacer 重复计算。**窗口收缩滞回**（`RANGE_HYSTERESIS`）：丢块要超出更紧的边距才生效，否则视口恰好停在边界上时 spacer 的间隙差就会让区间反复翻转。**贴底跟随由三态意图识别决定**（`scroll-intent.ts`：follow 追最新 / hold 停在此处 / reading 阅读历史；`sticky-follow.ts` 只做 DOM 绑定）——只看用户**真实造成的位移量**，不看事件来源：判定全在 scroll 位移上进行（滚轮/触屏/键盘/中键自动滚动/查找定位一律经位移体现，无来源漏判；位置没动就没有位移，橡皮筋回弹与惯性过冲自然豁免）。**位置离开底部阈值且本手势有向上位移**（用户确实滚开）→ 确认 60ms 后解除（期间回底则取消；用户停手无后续事件时靠定时器到点复查）；**向下滚到底（距底 ≤ `resumePx` 4px）且本手势有向下位移** → 恢复跟随——单纯「位置落在阈值内」（内容收缩、程序落位）不恢复，避免把视口从阅读位置拽回底部；**恢复不沿用容忍区阈值**：触摸板/触屏能精确停在阈值内，若在那里就恢复，用户只想往回调一点继续读却被锁定、后续内容增长把视口钉到底部（所以「到底」才是「想追最新」的自然凭据，滚轮会 clamp 到底、对此无感）；hold 与 reading 按距底距离归属（1.5 屏内 / 更远）。输入事件不改状态，只用于**短期抑制程序回正**（上翻输入 120ms 宽限内、或用户本手势刚上翻且仍在手势窗内时不 pin）——滚动是合成器异步派发的，输入与位置变化之间内容增长的 rAF 可能先行；该抑制**必须随手势窗结束而失效**（否则一次微调会让后续内容增长永久不再贴底，状态是「追最新」却名不副实）。**程序回正只保证「追到最新」，不篡改用户的距底位移**（`followOffset`）：跟随态内的位置就是用户的意图，容忍区内的小步上翻同样如此——回正落点 = 底部减该位移（`pinTarget`，未上翻时为 0 即绝对底部），而非无条件写 `scrollHeight`。窗口化与跟随核心共用这一个落点（`setFollowSource` 注入 `pinTarget`，返回 null = 不回正）：各自写绝对底部会抹掉用户的微调，小步上翻每次被弹回、位移永远累计不到容忍区之外（实测：图表异步长高期间连滚 3 次滚轮，距底始终为 0；体感即「要来回卡几下才滚得出去」）。窗口化据此取落点（未注入时退回几何判定 + 绝对底部）——不看几何位置：运行中会话内容持续增长会让「距底 ≤ 阈值」反复不成立，据此判定会在用户追最新时走锚点分支、把人留在旧位置（永远差一截），而在阈值区内又把人拽到最底（停不住）；贴底赋值后额外通知跟随核心接手后续对齐。**尾部活动区不入坐标表**（新消息/在途流/工具卡追加在块表之外、高度是真实 DOM），故视口顶部越过末块末尾时改按「距底距离」保持——块坐标 `locate` 会把越界坐标钳到末块末尾，照旧回写会把视口反复拉回该处（表现：接近底部滚不动、到不了最底部，差距恰为尾部内容高度）。滚动记忆同口径：跟随时记「下次落底」而非阅读位置。**滚动诊断探针**（`scroll-probe.ts`，按需开启默认关闭）：滚动体感类缺陷只能在实际出问题的机器上复现，探针把现场取证做成产品能力——开启后记录消息列每次 `scrollTop` 写入（含调用栈）、周期性快照（位置/高度/距底/spacer 高度/挂载块数/跟随状态/视口中心命中）与滚动事件，控制台 `__gbScrollProbe()` 打印并复制报告；开启方式 URL 参数 `?gb_scroll_probe=1`（临时）或 localStorage `gebai.ui.scrollProbe="on"`（持久），记录上限 1500 条环形丢弃。不再依赖浏览器估高缓存——CSS 的 `content-visibility: auto` + `contain-intrinsic-size` 布局隔离已随窗口化移除（它是旧内核丢弃 `auto` 关键字声明时视口外消息高度塔为 0、`scrollHeight` 严重低估、滚动被钳制跳动的根源；窗口化后 DOM 恒定，隔离亦无收益）。**尾部活动区**（新消息 / 在途流 / 工具卡 / 模型异常提示）恒定挂载在末尾、高度由 DOM 真实承担，在途运行态引用（`run.el` / `pendingTools` / 子会话容器）不掉线。低性能模式与标准模式共用同一布局行为。主消息区 `#messages`、子会话容器 `.subsession-body` 与推理体 `.reasoning-body` 关闭浏览器滚动锚定（`overflow-anchor: none`，全模式生效）——锚定算法在生成中底部内容持续增长/内容收缩场景会选中失效锚点把视口猛拉到列表顶部（跳回第一输入）；且锚定调整自行产生滚动事件、粘底跟随核心（sticky-follow，见「聊天页」）无法将其归因为内部动作，内容收缩（推理折叠/容器折叠）时会把贴底视口拽离底部。粘底跟随由应用自管，底部追加不影响已上翻的阅读位置。**消息子项禁止 flex 收缩**（`#messages > * { flex-shrink: 0 }`）：`#messages` 是 flex 列滚动容器，flex 子项的自动最小高度（`min-height: auto`）仅在自身 overflow 可见时等于内容高度，`overflow ≠ visible` 的子项（subsession_run 执行容器 `.session-run` 圆角裁剪需 `overflow: hidden`）自动最小高度为 **0**——会话内容超过一屏后 flex 负空间全部压到这类子项上，容器被压缩到近 0 高（实测 1px）、正文整段被裁剪不可见且 `scrollHeight` 塌缩至视口（滚动条近乎满比例、长内容滚不到，即「生成过程中滚动卡死、页面内容很长但滚动条显示很大比例」；历史回放渲染为折叠态不触发，故刷新后现象消失——执行容器只在执行中展开）；`flex-shrink: 0` 令所有子项保持内容高度，溢出全部进入滚动区。附注：会话列表条目正文（`#session-list .session-body`）与子会话容器正文（`.subsession-run .subsession-body`）曾共用未限定作用域的同名类 `.session-body`，后者内部消息对齐（`.msg.user align-self: flex-end`）实际依赖前者的全局 flex 规则意外供给——已将列表侧规则收窄到 `#session-list`、容器侧在 `chat.css` 显式声明 `display: flex; flex-direction: column`（同名类职责分离，消除跨模块样式耦合）
  - **流式输出降频**：文本 delta 渲染按 120ms 尾沿节流合并（`scheduleStreamRender`，计时器挂 `RunState.renderTimer`，封段/重置/结束时随 run 清理；**封段前冲刷最后一帧**——定时器待触发即存在未上屏增量，只清零不冲刷会把最后一个节流窗口（120ms）内的文本永久丢失，快速模型整轮回复可全部落在一个窗口内、气泡残留空白（subsession_run 执行过程「只见结果不见过程」的主因））
  - **流式渲梁增量渲染**（`stream-render.ts`）：markdown 解析是流式期间最重的 CPU 开销，逐帧重解析累积全文在长回答下是 O(n²)（实测：全量模式下 3.5KB 正文已出现 12 个 longtask / 最大 679ms、帧率降到 30fps）——按**块边界**把文本切成「已稳定的前缀」与「仍在增长的尾部」：前缀渲染一次即常驻复用，每次只重渲染尾部（尾部长度与「最后一块」同量级、不随回答变长）。结构保持与全量渲染一致（容器内单层 `.markdown`，前缀块与尾部块都是它的直接子节点，尾部块每帧整体替换），片段挂进最终位置后边距由相邻兄弟规则决定。**切点安全**：围栏代码块未闭合、`<think>` 未闭合期间不切（片段会解析不出代码块 / 抽不出推理卡片）；空行两侧同类且可跨空行延续（松散列表 / 多段引用 / 缩进块）时不切（否则裂成两块，编号列表会重新计数）；`html: false` 故无 HTML 块跨空行的歧义。含 `<think>` 的文本与文本回退（重连重放）走全量渲染（推理卡片需整体抽取）；参考式链接定义跨切点会失效（流式半成品可接受，封段后以全量文本渲染为准）。渲染器按容器（气泡内 `.msg-text`）分配（WeakMap）：共享实例会在容器间反复重建宿主而退化为全量。标准模式不再逐 chunk 同步渲染
  - **代码高亮降级**：无语言标注的代码块跳过 `highlightAuto` 自动检测（穷举全部语言最贵），仅转义；显式语言标注仍正常高亮
  - **图表始终默认渲染**：缩略图自动渲染（不随低性能模式改为按需，避免「图不默认渲染」困惑；渲染失败只显示占位提示、错误细节不暴露到主页面，进查看器/控制台）；本地渲染引擎**空闲预热**（`load` 后经 `requestIdleCallback`（缺省回退 800ms 定时）调度，避开首屏；**仅预热本机实际用过的引擎**——痕迹由 `markEngineUsed` 记录，无图表使用史的会话首屏不下载任何引擎；D2 的 WASM 开销大且架构图频率低不预热）；**低性能模式跳过预热**，首次渲染由消息流触发；PNG 导出超采样 3x → 1.5x、复制图片 2x → 1x（`isLowPower()` 实时读取），渲染与 show 图表分支链路不变

- **单轮计时器**：任务（单轮对话）耗时显示于**标题栏右侧、上下文占比左侧**（`.turn-timer`，细线沙漏图标 + 时长文本，常驻单一位置；时长文本为**冒号分隔、不带单位后缀**——`分:秒`（如 `0:07`），≥1h 为 `时:分:秒`（如 `1:05:03`））：运行中实时走（250ms 刷新、图标呼吸动效），**运行结束不清除**（定格保留显示），**下次运行重启归零**；**随会话视图切换**——计时显示归属当前会话：运行中切走不打扰（tick 空转）、切回续走（`gebai:session-view` 事件联动刷新），非运行会话显示该会话最后定格时长、无记录隐藏；后台会话的启动/结束不抢占当前视图。**六级时长渐进变色**（`data-dur` 属性，冷→暖，阈值 10s/30s/1m/3m/5m）：<10s 极淡（`--text-faint`）、10-30s 中性（`--text-muted`）、30s-1m 工具青绿（`--tool`）、1-3m 主题色（`--accent`）、3-5m 警告（`--warning`）、≥5m 危险（`--danger`），与上下文圆环的分级着色语言一致。**信号灯思考闪烁周期同步六级放缓**：运行中把同一 `data-dur` 同步到 `#header-ctx`，`--conn-blink` 变量按级取 0.07s → 0.11s → 0.16s → 0.25s → 0.4s → 0.6s（**越久越慢——闪得慢=执行得慢**：刚开始急促闪烁表活跃，拖长后逐渐放缓表执行缓慢；SVG 圆点 `.conn-dot` 经继承解析；运行结束移除属性恢复默认 0.25s；低性能模式豁免保留闪烁且保留分级周期；`no-ring` 无数据模式仅隐藏轨道/弧线、圆点仍可显示）。**hover 显示总运行时**（`data-tip`）：每轮净耗时累加（含进行中这轮的已耗时，不含轮间空闲），而非距第一条消息的墙钟时间；累计**按会话记账**（页面生命周期内，刷新后归零）。默认开启；设置面板「外观」tab 单行按钮可关（`localStorage["gebai.ui.turnTimer"]` 仅存 `"off"`，根元素 `data-turn-timer="off"` 时 CSS 隐藏，跨标签页 storage 事件同步，运行中被关闭即时停表隐藏）。纯展示层能力：不持久化到会话记录（`Message` 无耗时字段）

- **输出速率（tok/s）**：模型输出速度显示于**标题栏单轮计时器右侧**（`.token-rate`，仪表盘图标 + 纯数值如 `42.3`，**单位与口径标记都不上屏**——避免标题栏噪声，单位与口径在 hover 提示里给出），便于观测模型服务性能。**数据源唯一在服务端**（`event.session.tps`）：服务端在一次模型调用内**按 1s 节流周期持续上报**（`est: true, active: true`，服务端按 CJK 感知字符折算估算——与 usage 真值同量级），调用结束时推**收尾帧**（`active: false`；接口返回 usage 时 `est: false` 即实测口径，不返回时仍为估算收尾）——长回答全长期间数字都在刷新，不是只有在调用结束时才有一个数；前端**只展示不自算**（两套口径并存会让数字来回跳，也说不清看到的是谁的量）。速率 = 输出 tokens ÷ **生成窗口**（首个输出 chunk 到末个输出 chunk，不含首 token 等待——量的是解码速度而非接口响应延迟）；生成中帧要求窗口 ≥300ms 且距上帧 ≥1s，收尾帧要求窗口 ≥50ms（整段一次性返回的非流式产出测不出解码速度，不报）。**仪表常驻**：两帧之间（工具执行 / 审批等待 / 思考停顿）**保持上一帧的值与配色**，不闪没；速率按会话记账，随会话视图切换显示对应会话的帧，无该会话帧时隐藏。**按导航交通拥挤配色分级着速**（`data-rate` 六级，越快越绿）：≥80 畅通（`--success`）、50-80 顺畅、30-50 良好（前三级与后三级间的过渡色由主题 `--success`/`--warning` 经 `color-mix` 派生，随主题自适应）、18-30 缓行（`--warning`）、8-18 拥堵、<8 严重拥堵（`--danger`）；估算帧整体降不透明度（真值帧满不透明度）。hover 提示为**两行固定结构**（信息与噪声分离）：速率行 `42.3 tok/s · 良好（实测）`（数值 + 单位 + 路况级名 + 口径，估算帧作「估算」）+ 依据行 `输出 423 tokens / 10.0s`（该帧输出 tokens 与生成窗口）。**随会话视图切换**（与计时器同一联动点）：非当前会话不抢占视图；任务结束定格保留（下一帧到达才更新）。默认开启；设置面板「外观」tab 单行按钮可关（`localStorage["gebai.ui.tokenRate"]` 仅存 `"off"`，根元素 `data-token-rate="off"` 时 CSS 隐藏，跨标签页 storage 事件同步，关闭即停计量）。纯展示层能力：不持久化到会话记录

- **文件展示方式**：设置面板「外观」tab 单行按钮（嵌入展示 ⇄ 弹窗查看）——read/write 等文件工具（含 code 子Agent 同款包装）的**产物文件卡**形态切换：**弹窗查看**（默认，现状：收敛为文件链接 chip，点击弹窗查看全文；适配会话相对与 code 项目绝对路径，经 `files/preview` 取数）/ **嵌入展示**（file 内容卡内联展示；参数区与输出不受影响，详见「会话临时文件查看与下载」与「文件展示方式」渲染说明）。`localStorage["gebai.ui.fileDisplay"]` 仅存 `"inline"`，跨标签 storage 事件同步；渲染是结构性的，切换后重载当前会话消息即时生效。纯展示层能力：不持久化到会话记录。**脚本桥（js/py）编排的产物同样受控**——桥自身没有文件卡声明（它的参数是脚本，路径参数不固定），透传的 file 块带上**来源工具名**（`via`），判定按来源工具走同一份 `card.file`：桥内调用 `read`/`write`/`edit` 与直接调用表现一致，而 `show` 这类主动展示工具的产物照常内联
