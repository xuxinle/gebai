# AGENTS.md — 歌白智能体 (GEBAI Agent)

本文件是编码/维护本仓库时必须遵循的约定。**权威设计文档为 `DESIGN.md`**，任何设计层面的变更都必须在 `DESIGN.md` 中同步回写，二者保持一致。

## 项目是什么

GEBAI 是一个极致动态扩展能力的智能体：单 TS 文件即可定义子 Agent 扩展能力。核心是稳定的「对话 → 工具调用 → 审批 → 执行」主循环，支持多用户安全隔离、多端部署（桌面 WebView / 浏览器 / 服务端）、以及通过修改自身代码实现的代码级自我优化。

设计目标、架构、模块接口、协议、安全模型、常量等全部以 `DESIGN.md` 为准。动手前先读 `DESIGN.md` 相关章节。

## 设计同步铁律

- `DESIGN.md` 是唯一权威设计来源。
- 当你修改了代码所体现的行为、接口、协议、存储布局、常量、命名规则等，**必须同步更新 `DESIGN.md` 对应章节**。
- 新增能力 / 工具 / 子 Agent 时，需在 `DESIGN.md` 中补充（功能列表、子 Agent 定义、常量表等）。
- 改动设计文档时，保持章节结构与既有措辞风格一致（中文、表格、代码块）。

## 技术栈与仓库结构

Bun workspaces + Turborepo 的 Monorepo：

| 包 | 路径 | 职责 |
|----|------|------|
| `@gebai/server` | `packages/server/` | 服务端核心：Hono、Agent 引擎、会话管理、子 Agent 装载/新会话执行、REST/WS/Webhook；**代码分层**：核心引擎与全局工具（`AgentEngine`/`ToolRegistry`/`Sandbox`/`SessionStore`/`LLMProvider`/全局工具）在 `src/core/`，应用层（HTTP/WS/Webhook/鉴权/配置）在 `src/` 根 |
| `@gebai/agents` | `packages/agents/` | TS 子代理包，**双域分居**：`src/agents/`（纯子代理定义——扫描域，丢文件即注册，无需排除清单）+ `src/core/`（基建组件：`analyzer/`、`browser/`、`cv/`、`code-tools.ts`、`shared/`）。**零 import `@gebai/server`**（编译期强制）；契约类型一律来自 `@gebai/sdk`，node 工具值导入走 `@gebai/sdk/node` |
| `@gebai/sdk` | `packages/sdk/` | 客户端 SDK：WS/REST 连接管理、类型定义、API 契约。**双入口**：主入口 `.` 为浏览器安全集（types/cron-types/agent-contract + GebaiClient，**零 node 内建**）；node 内建模块（agent-utils/artifacts/projects/walk/paths）走 `@gebai/sdk/node`——**主入口混入 node 内建会致 web 构建崩溃**（vite treeshake:false 解析 `__vite-browser-external` 具名导出失败） |
| `@gebai/web` | `packages/web/` | Web UI：Vite 构建，打包进二进制 |
| `@gebai/desktop` | `packages/desktop/` | 桌面端宿主：`dist/gebai.exe`（纯 Bun `--compile` 单文件，浏览器形态）+ `dist/gebai-desktop.exe`（`launcher/`：tao/wry 原生 WebView 启动器，`include_bytes!` 内嵌服务端二进制；构建期可参数化产出场景变体） |

- **二次开发域**：仓库根 `custom/`（`custom/agents/` 子代理定义 + `custom/core/` 依赖组件，与 `packages/` 平级）——放置即注册、同名覆盖内置；上游更新时整个目录拷到新仓库根即完成迁移。
- 语言：TypeScript，运行时 Bun。
- Web 框架：Hono（服务端）、Vite（前端构建）。
- LLM 接入**不依赖第三方 AI SDK**，自行实现 OpenAI 兼容 `chat/completions`、OpenAI `responses` 与 Anthropic `messages` 三类接口的请求与 SSE 流解析，统一抽象为 `provider.chat()`。

## 常用命令

```bash
# 安装依赖（Bun workspaces）
bun install

# 开发（脚本调试模式，GEBAI_HOME 为项目根目录）
bun run dev          # 或 turbo run dev

# 构建
bun run build

# 测试（bun test，测试文件与被测代码同目录 *.test.ts）
bun run test
bun run --cwd packages/server test
bun run --cwd packages/sdk test

# 覆盖率
bun run --cwd packages/server test:coverage

# 类型检查 / Lint
bun run typecheck
bun run typecheck:custom   # 二开域（custom/）单独类型检查
bun run lint
```

## 编码约定

- **接口优先**：`LLMProvider`、`AgentEngine`、`ToolRegistry`、`SessionStore`、`EnvManager`、`AuthService`、`Sandbox`、`EventBus`、`ContextCompressor` 等核心概念通过接口定义，便于 mock。
- **依赖注入**：核心模块无副作用、依赖显式注入，可独立实例化测试；不隐式访问全局单例。
- **工具可测试**：全局工具与子 Agent 工具函数体为纯函数或可注入依赖的函数工厂，天然支持并发加载与测试。
- **不引入第三方 AI SDK**：LLM 请求与流解析自行实现。
- **不要添加无关注释**：除非必要，代码注释从简；遵循文件内既有风格。
- **环境变量示例同步**：任何新增/变更的环境变量都必须同步写入根目录 `.env.example`（含注释说明与示例值）；`.env` 不入版本库，示例文件与 `DESIGN.md`「环境变量配置」表、`core/agents/env-catalog.ts` 的面板白名单三处保持一致。

### 模块分层与自动扩展

- **服务端目录按领域分层**（详见 `DESIGN.md`「服务端目录结构」）：`routes/`（REST 按域）、`ws-handlers/`（WS 消息按域）、`boot/`（compose/serve/cli）、`core/{base,llm,engine,tools,support,session,schedule,exec,fs,git,browser,security,agents}`——core 根目录只放构建生成物，禁止往根平铺新源码文件。**注意**：`cv/`、`analyzer/`、`browser/` 桥接等基建已迁 `@gebai/agents`，server 侧 `core/browser/` 仅剩 `fetch-proxy`（新增基建前先确认归属包）。
- **依赖单向**：`base` ← `support`/`security` ← 各领域 ← `engine` ← 传输层 ← `boot`；`core/` 内部模块**禁止 import `core/tools` 聚合 barrel**（其目录扫描顶层 await 会因反向依赖成环产生未初始化绑定）——共用能力直引 `core/support/*`、`core/security/*` 叶子模块。
- **全局工具零注册**：新增全局工具 = 在 `packages/server/src/core/tools/` 新建导出 `export const globalTools: GlobalToolEntry[]` 的文件（契约见 `tools/shared.ts`），不改任何中央注册表——与子 Agent 同款「丢文件即注册」扩展模型。
- **REST 路由 / WS 处理器按域新增**：新路由域在 `routes/` 新建 `register{Domain}Routes`（经 `routes/context.ts` 的 RouteCtx）并在 `app.ts` 装配；新 WS 消息类型在 `ws-handlers/` 对应域文件加 handler（入口守卫在 `ws.ts` 统一）。

### 命名与命名空间

- **子 Agent 概念术语（消除模型误解的关键）**：`agent_load` = **装载**（模块语义，类比 import 子模块：工具并入当前工具集，无独立上下文）；`agent_run` = **新会话执行**（会话语义：派生临时新会话，预加载一个或多个子 Agent（完整提示词+工具）后阻塞执行，只返回最终结果）。代码/注释/系统提示词/文档一律用「装载/新会话执行」，不用「加载/调用子Agent」。详见 `DESIGN.md`「装载 vs 新会话执行（概念模型）」。
- 子 Agent 名：`[a-z0-9_]+`（小写字母/数字/下划线）。
- 子 Agent 工具名：`[a-zA-Z0-9_]+`（不含 `.`/`-`/`:`）。
- 全局工具名：`[a-z][a-z0-9_]*`（小写开头，不含 `.`/`-`/`:`）。
- **工具输入参数名一律蛇形**：`[a-z][a-z0-9_]*`（`old_string`/`full_page` 风格，不用驼峰——驼峰/连字符键是弱模型回显 schema 的高频出错形态）；描述文本引用参数名同样用蛇形。输出 `data` 字段与跨端协议键（WS 载荷、playwright 桥）不在此列。引擎派发点（`core/base/tool-args.ts`）对模型生成的名/键做容错归一，工具自身仍以蛇形契约为准。
- 工具总名（`{agent}_{tool}`）长度 ≤ 40 字符。
- 命名空间采用**单下划线**，路由按「全局精确匹配 → 子 Agent 最长前缀匹配」两步解析；全局工具名不得以任何 `{agent}_` 开头，子 Agent 名不得互为前缀（**注册期校验**，违反即抛错；无构建期独立校验）。

### 存储与分片

- 所有数据存于 `GEBAI_HOME`（脚本调试=项目根目录，二进制=`~/.gebai/`）。
- 按文件数量增长的目录（`sessions/`、`feedback/`）做多层分片，禁止单目录堆积过多文件：hex ID（会话/反馈）分片段**直接取 ID 自身前缀**（前 2 位/第 3-4 位——肉眼可从 ID 推目录）；非 hex 键按哈希分片。分片仅影响存储布局，上层感知逻辑路径。（`tmp/truncated/` 在会话目录内，随会话分片，无需额外分片。）

### 安全

- **绝不提交密钥**：`.env`、API Key、令牌、密码不得进入版本库（已 gitignore）。不要复制或打印 `.env` 中的明文密钥。
- 多用户模式下按「用户 → 会话」两级隔离；所有会话操作校验归属。
- 路径沙箱：服务端部署模式限制文件工具访问范围，拒绝 `../`、绝对路径、符号链接（路径穿越）。
- 日志脱敏：日志走**标准输出/错误**（无文件 sink、无轮转），不记录密码/令牌/密钥明文，敏感字段以 `***` 替代（env 回显经 `maskEnv`），会话内容默认不落日志。`GEBAI_LOG_LEVEL` 目前只读入配置、级别过滤尚未实现。
- 环境变量作用域：`{AGENT_NAME_UPPER}_*` 前缀为命名约定与前端目录白名单口径；运行时硬边界是脚本子进程/`js` ctx 的敏感变量剔除（按完整结尾单词匹配 `_KEY`/`_TOKEN`/`_SECRET`/`_HASH` 等形态），多用户沙箱模式下生效。

## 测试策略

- 统一 `bun test`，测试文件与被测代码同目录（`*.test.ts`）。
- **环境封闭**：测试进程不读仓库 `.env`（`packages/server/bunfig.toml` 的 `[test] preload` 清 `GEBAI_`/`CODE_` 变量 + `loadConfig` 的 `loadDotEnv` 在 test 期跳过）；**该 preload 只在包目录下生效**——从仓库根直接跑 `bun test packages/server/...` 不会加载它（工具/脚本跑测试请用 `--cwd packages/server`，否则仓库根 `.env` 会污染断言）。断言不得依赖开发者本地配置或宿主环境变量。
- **跨平台**：涉及平台分支的用例显式注入平台参数（如 `platform: "win32"`），不随宿主平台漂移。
- **并行安全**：新增测试不得在仓库目录内写文件/改 mtime（用 `mkdtempSync`）、不得依赖固定端口/固定临时路径；真实 spawn 类用例给足用例超时（并行分片满载时 5s 默认不够）。
- 分层：单元测试（核心模块必须，零外部依赖）→ 集成测试（mock LLM Provider 跑 AgentEngine 主循环）→ 契约测试（WS/REST/SSE 消息格式、SDK 一致性）→ E2E（mock LLM + 内存存储跑主路径）。
- 覆盖率门槛（**目标值**，无 `coverageThreshold` 配置与 CI 强制，靠约定）：核心引擎（`AgentEngine`/`ToolRegistry`/`EnvManager`/`Sandbox`/命名空间解析）行覆盖率 ≥ 90%；工具函数 ≥ 80%；整体 ≥ 70%。
- 可伪造性：`LLMProvider`、时间、文件系统均有测试替身（fake），测试不依赖真实网络/时钟/磁盘。
- 自我优化（`self_optimize`）修改代码后，测试是唯一准入凭证；测试失败自动回滚。

## 如何新增子 Agent

1. 在 `packages/agents/src/agents/` 下新增定义：**单文件** `{name}.ts`、**目录** `{name}/{name}.ts`（`{name}/index.ts` 为回退入口；系统提示词可拆 `{name}.md` 由 ts 导入并修饰），或 **纯提示词** `{name}/{name}.md`（零 TS）。
   **TS 定义形态必须导出 `export const def: SubAgentDef`**（纯提示词 `.md` 形态无需此项）——加载器只读 `mod.def`，未导出即报「未导出 def（须 export const def: SubAgentDef）」加载失败。字段：`name`/`description`/`systemPrompt`/`tools`（省略即纯提示词子 Agent）/`requiresApproval`/`preload`/`dependencies`（依赖名单：装载/预加载/`agent_run` 自动连带装载，工具与提示词按依赖方命名空间复用，不在本 def 重复声明）/`envVars`（可配置环境变量声明，`{AGENT_NAME_UPPER}_` 前缀，自动汇总进前端面板白名单）/`projectRoot`（默认项目根兜底）/`writeGuard`（写范围守卫）；契约定义见 `packages/sdk/src/agent-contract.ts`，模板见 `DESIGN.md`「子Agent文件格式」（顶层可先导出同名常量再组装，如 `export const preload = false` + `export const def = { ... }`）。
2. **纯提示词简化定义**：仅需系统提示词的简单/组合式子Agent 可直接放 `{name}/{name}.md`（零 TS，可选 frontmatter `description`/`dependencies`/`preload`/`env_vars`）；`tools` 省略时自动注入编排工具（`agent_list`/`agent_load`/`agent_run` + `bg_task`），组合式子Agent 在提示词中说明编排策略即可。
3. 命名符合规则；工具名无需关注前缀，总 Agent 自动加 `{agent}_` 命名空间。
4. 构建时自动扫描收集（零注册）；可按需选择性打包（构建期环境变量 `GEBAI_BUILD_SUBAGENTS`/`GEBAI_BUILD_PRELOAD`，无 CLI 参数）。
5. 同步在 `DESIGN.md` 中补充该子 Agent 的说明与总览表。

## 如何新增全局工具

1. 在 `packages/server/src/core/tools/` 下新建文件（或并入既有域文件），导出 `export const globalTools: GlobalToolEntry[] = [{ name, tool, project? }]`——`project` 声明 `projectAware` 包装（`true` 默认 / `"workdir"` 附工作目录参数）。
2. 工具名符合全局工具命名规则（`[a-z][a-z0-9_]*`）；重名在聚合时抛错。
3. dev 形态重启进程即生效；二进制形态由 `packages/server/scripts/build-tools.ts` 构建时自动收进 bundle 注册表。
4. 命名空间专属工具（不进全局表）落 `@gebai/agents` 的 `src/core/shared/`（或域内文件，如 `code-tools.ts`），由子 Agent def 以 `{agent}_{tool}` 引用——server 侧原 `core/tools/extras.ts` 已整体迁出，**不要再往那里放**。
5. 同步在 `DESIGN.md`「总Agent全局工具」补条目。

## 提交 / 验证

- 提交前先看 `git status` / `git diff`，只暂存预期文件，不提交密钥。
- 完成后运行 `bun run test`、`bun run typecheck`、`bun run lint` 确认通过。
- 设计变更记得回写 `DESIGN.md`。
