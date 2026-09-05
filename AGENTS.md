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
| `@gebai/server` | `packages/server/` | 服务端核心：Hono、Agent 引擎、会话管理、子 Agent 加载、REST/WS/Webhook |
| `@gebai/sdk` | `packages/sdk/` | 客户端 SDK：WS/REST 连接管理、类型定义、API 契约 |
| `@gebai/web` | `packages/web/` | Web UI：Vite 构建，打包进二进制 |
| `@gebai/desktop` | `packages/desktop/` | 桌面端宿主：`dist/gebai.exe`（纯 Bun `--compile` 单文件，浏览器形态）+ `launcher/`（tao/wry 原生 WebView 启动器，内嵌服务端二进制） |

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
bun run lint
```

## 编码约定

- **接口优先**：`LLMProvider`、`AgentEngine`、`ToolRegistry`、`SessionStore`、`EnvManager`、`AuthService`、`Sandbox`、`EventBus`、`Compressor` 等核心概念通过接口定义，便于 mock。
- **依赖注入**：核心模块无副作用、依赖显式注入，可独立实例化测试；不隐式访问全局单例。
- **工具可测试**：全局工具与子 Agent 工具函数体为纯函数或可注入依赖的函数工厂，天然支持并发加载与测试。
- **不引入第三方 AI SDK**：LLM 请求与流解析自行实现。
- **不要添加无关注释**：除非必要，代码注释从简；遵循文件内既有风格。
- **环境变量示例同步**：任何新增/变更的环境变量都必须同步写入根目录 `.env.example`（含注释说明与示例值）；`.env` 不入版本库，示例文件是唯一文档来源。

### 模块分层与自动扩展

- **服务端目录按领域分层**（详见 `DESIGN.md`「服务端目录结构」）：`routes/`（REST 按域）、`ws-handlers/`（WS 消息按域）、`boot/`（compose/serve/cli）、`core/{base,llm,engine,tools,support,session,schedule,exec,browser,cv,security,agents,widgets}`——core 根目录只放构建生成物，禁止往根平铺新源码文件。
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
- 命名空间采用**单下划线**，路由按「全局精确匹配 → 子 Agent 最长前缀匹配」两步解析；全局工具名不得以任何 `{agent}_` 开头，子 Agent 名不得互为前缀（构建时校验）。

### 存储与分片

- 所有数据存于 `GEBAI_HOME`（脚本调试=项目根目录，二进制=`~/.gebai/`）。
- 按文件数量增长的目录（`sessions/`、`feedback/`）做多层分片，禁止单目录堆积过多文件：hex ID（会话/反馈）分片段**直接取 ID 自身前缀**（前 2 位/第 3-4 位——肉眼可从 ID 推目录）；非 hex 键（小工具名等）按哈希分片。分片仅影响存储布局，上层感知逻辑路径。（`tmp/truncated/` 在会话目录内，随会话分片，无需额外分片。）

### 安全

- **绝不提交密钥**：`.env`、API Key、令牌、密码不得进入版本库（已 gitignore）。不要复制或打印 `.env` 中的明文密钥。
- 多用户模式下按「用户 → 会话」两级隔离；所有会话操作校验归属。
- 路径沙箱：服务端部署模式限制文件工具访问范围，拒绝 `../`、绝对路径、符号链接（路径穿越）。
- 日志脱敏：不记录密码/令牌/密钥明文，敏感字段以 `***` 替代，会话内容默认不落日志。
- 环境变量作用域：`{AGENT_NAME_UPPER}_*` 前缀为命名约定与前端目录白名单口径；运行时硬边界是脚本子进程/`js` ctx 的敏感变量剔除（按完整结尾单词匹配 `_KEY`/`_TOKEN`/`_SECRET`/`_HASH` 等形态），多用户沙箱模式下生效。

## 测试策略

- 统一 `bun test`，测试文件与被测代码同目录（`*.test.ts`）。
- 分层：单元测试（核心模块必须，零外部依赖）→ 集成测试（mock LLM Provider 跑 AgentEngine 主循环）→ 契约测试（WS/REST/SSE 消息格式、SDK 一致性）→ E2E（mock LLM + 内存存储跑主路径）。
- 覆盖率门槛：核心引擎（`AgentEngine`/`ToolRegistry`/`EnvManager`/`Sandbox`/命名空间解析）行覆盖率 ≥ 90%；工具函数 ≥ 80%；整体 ≥ 70%。
- 可伪造性：`LLMProvider`、时间、文件系统均有测试替身（fake），测试不依赖真实网络/时钟/磁盘。
- 自我优化（`self_optimize`）修改代码后，测试是唯一准入凭证；测试失败自动回滚。

## 如何新增子 Agent

1. 在 `packages/server/src/sub-agents/` 下新增**单文件** `{name}.ts`（导出 `name`、`description`、`systemPrompt`、`tools`、可选 `requiresApproval`/`preload`/`dependencies`/`envVars`——`dependencies` 为依赖的其他子Agent 名单（装载/预加载/`agent_run` 自动连带装载，工具与提示词按依赖方自身命名空间复用，不在本 def 重复声明）；`envVars` 为该子Agent 可配置环境变量声明（`{AGENT_NAME_UPPER}_` 前缀），自动汇总进前端环境变量面板白名单）或**目录** `{name}/{name}.ts`（系统提示词可拆 `{name}.md` 由 ts 导入并修饰），模板见 `DESIGN.md`「子Agent文件格式」。
2. **纯提示词简化定义**：仅需系统提示词的简单/组合式子Agent 可直接放 `{name}/{name}.md`（零 TS，可选 frontmatter `description`/`dependencies`）；无工具定义会自动获得编排工具（`agent_run`/`agent_list`/`agent_load`），组合式子Agent 在提示词中说明编排策略即可。
3. 命名符合规则；工具名无需关注前缀，总 Agent 自动加 `{agent}_` 命名空间。
4. 构建时自动扫描收集（零注册）；可按需选择性打包。
5. 同步在 `DESIGN.md` 中补充该子 Agent 的说明与总览表。

## 如何新增全局工具

1. 在 `packages/server/src/core/tools/` 下新建文件（或并入既有域文件），导出 `export const globalTools: GlobalToolEntry[] = [{ name, tool, project? }]`——`project` 声明 `projectAware` 包装（`true` 默认 / `"workdir"` 附工作目录参数）。
2. 工具名符合全局工具命名规则（`[a-z][a-z0-9_]*`）；重名在聚合时抛错。
3. dev 形态重启进程即生效；二进制形态由 `scripts/build-tools.ts` 构建时自动收进 bundle 注册表。
4. 命名空间专属工具（不进全局表）放 `tools/extras.ts`，由子 Agent def 以 `{agent}_{tool}` 引用。
5. 同步在 `DESIGN.md`「总Agent全局工具」补条目。

## 提交 / 验证

- 提交前先看 `git status` / `git diff`，只暂存预期文件，不提交密钥。
- 完成后运行 `bun run test`、`bun run typecheck`、`bun run lint` 确认通过。
- 设计变更记得回写 `DESIGN.md`。
