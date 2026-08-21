# 歌白智能体(GEBAI Agent)

## 定位
极致动态扩展能力的智能体，能使用单个TS文件定义子Agent扩展Agent能力。核心目标：

1. **核心Agent流程完善**：稳定的「对话 → 工具调用 → 审批 → 执行」主循环，可观测、可中断、可恢复
2. **极强的子Agent扩展机制**：单文件定义、零注册、装载即并入工具与完整系统提示词（获得完整能力）、透明工具路由，动态组合 Agent 能力
3. **多端部署使用方式**：单二进制分发，一套代码同时支持桌面应用、本地浏览器、服务端部署
4. **多用户安全隔离**：用户 → 会话两级数据隔离，路径沙箱与执行隔离，构建多用户安全环境
5. **代码级自我优化**：Agent 能修改自身代码（子Agent 定义、工具实现、系统提示词）来改进自己；不依赖记忆（memory）、技能（skill）等运行时注入的不稳定能力，一切改进沉淀为可审查、可测试、可回滚的代码变更

## GEBAI_HOME
所有数据统一存储在 `GEBAI_HOME` 目录下（日志、会话、截断内容等；**用户环境变量零留存**——只存浏览器本地，服务端不落任何 env 文件）：

| 运行模式 | GEBAI_HOME 路径 |
|---------|----------------|
| 脚本调试 (`bun run dev:*`) | 项目根目录 |
| 二进制发行 | `~/.gebai/` |

目录结构（文件可能非常多，所有按文件数量增长的目录均做**多层分片**，避免单目录文件过多）：
```
GEBAI_HOME/
├── gebai.log              # 日志文件（全局）
├── conn-state.json        # WS 连接状态（每用户当前会话，防抖落盘；重连/重启后恢复，见「WebSocket」）
├── tools/                 # 公用 HTML 小工具（widgets 子Agent 保存，全局共享，按名称哈希分片）
│   └── {h0}/{h1}/{name}.json   # { name, html, scope:"public", owner, createdAt, updatedAt }
└── users/                 # 用户数据目录（多用户安全隔离）
    ├── registry.json      # 用户注册表（服务模式）：用户名 → 加盐哈希/角色/状态
    └── {user}/            # 每个用户独立的数据目录
        ├── tools/         # 用户私有 HTML 小工具（按名称哈希分片，仅本人可见）
        │   └── {h0}/{h1}/{name}.json   # { name, html, scope:"private", createdAt, updatedAt }
        ├── sessions/      # 会话持久化（按会话隔离，多层分片）
        │   └── {s0}/{s1}/{session_id}/
        │       ├── chat.json        # 会话消息
        │       ├── cron.json        # 会话级定时任务（GEBAI_CRON_ENABLED 启用时使用）
        │       └── tmp/             # 该会话的临时文件工作区（附件、产物、截断文件等）
        │           └── truncated/   # 工具超长输出截断落盘（{tool_name}_{content_hash}.txt）
        ├── feedback/      # 用户反馈（按日期+哈希分片）
        │   └── YYYY-MM-DD/{h0}/{h1}/
        │       └── {feedback_id}.json
        └── trash/         # 删除/过期的会话（按日期分片），定期清理
            └── YYYY-MM-DD/{session_id}/
```

分片规则（基于内容/ID 哈希的前缀分层，单目录容量可控）：

| 目录 | 分片键 | 分片层级 |
|------|--------|---------|
| `sessions/` | 会话 ID（哈希） | 前 2 位 `{s0}` / 前 4 位 `{s1}` |
| `feedback/` | 反馈 ID（哈希） | 日期 + 前 2 位 `{h0}` / 前 4 位 `{h1}` |
| `trash/` | 归档日期 | `YYYY-MM-DD/` |
| `tools/`（公用）/ `users/{user}/tools/`（私有） | 工具名（哈希） | 前 2 位 `{h0}` / 前 4 位 `{h1}` |

- 截断文件（`tmp/truncated/`）在会话目录内，随会话分片；内容 SHA256 哈希命名去重（同内容不重复写入），会话内数量有限无需额外分片

- 每层分片基数 256（16×16），单目录满约 16K 子目录/文件时自动感知；层级可按需加深（`{s2}`、`{h2}` 等）
- 会话 ID、反馈 ID 哈希均用于分片，天然支持 O(1) 定位（无需扫描）
- 分片仅影响存储布局，路径由服务端内部解析，上层（模型/前端/API）感知的仍是逻辑路径

> 本地模式使用固定默认用户 **admin**（用户名/用户 id 均为 `admin`），目录结构完全一致，核心代码单一路径。**admin 为特权用户（超级权限）**：`role: "admin"` 管理员角色，且**豁免路径沙箱**——沙箱启用（服务端部署/`GEBAI_SANDBOX=on`）时 admin 仍按本地模式放开（绝对路径/`../` 越界放行、脚本环境不剔除敏感变量、桌面控制/私网访问不限制），不受用户沙箱控制。旧版本地模式数据目录 `users/default/` 在启动时自动迁移为 `users/admin/`。

## 技术栈
- **语言**: TypeScript (bun 运行时)
- **架构**: C/S 模式（参考 opencode）
- **Web 框架**: Hono
- **LLM 接入**: 支持三类 LLM 服务接口，不依赖第三方 AI SDK，自行实现请求/流解析，便于定制与扩展：
  - **OpenAI 兼容 Chat Completions**（`/chat/completions`，SSE 流式）：OpenAI、DeepSeek、通义、Kimi、GLM、Ollama 等主流服务
  - **OpenAI Responses API**（`/responses`，SSE 流式）：OpenAI 新一代接口（`gpt-5` 等新模型）
  - **Anthropic Messages**（`/v1/messages`，SSE 流式）：Anthropic Claude 官方与兼容服务
  - 三类接口均支持**多模态**（文本 + 图片/音视频），统一抽象为 `provider.chat()`，按模型能力自动组装对应格式
- **前端**: Web (Vite 构建)，桌面端由原生 WebView 启动器（tao/wry）或系统浏览器加载同一套 Web UI
- **语法分析**: tree-sitter（wasm，`web-tree-sitter` + `tree-sitter-wasms`），供 code 的 `analyze` 工具做代码结构概览；非 AI 依赖，不影响「不引入第三方 AI SDK」原则；**语法 wasm 构建期内嵌**（`scripts/build-analyzer-wasm.ts` 生成 gzip+base64 注册表，二进制打包模式回退内嵌产物，dev 模式读 node_modules）

## 软件包结构

Monorepo 采用 Bun workspaces + Turborepo：

| 包 | 路径 | 职责 |
|---|------|------|
| `@gebai/server` | `packages/server/` | 服务端核心：Hono 服务、Agent 引擎、会话管理、子Agent 装载/新会话执行、REST/WebSocket/Webhook 对外接口；**代码分两层**——核心引擎与全局工具（`AgentEngine`/`ToolRegistry`/`Sandbox`/`SessionStore`/`LLMProvider`/全局工具等）位于 `src/core/`，应用层（HTTP/WS/Webhook/鉴权/配置）位于 `src/` 根，子Agent 位于 `src/sub-agents/` |
| `@gebai/sdk` | `packages/sdk/` | 客户端 SDK：WebSocket/REST 连接管理、类型定义、API 契约 |
| `@gebai/web` | `packages/web/` | Web UI：Vite 构建，打包进二进制作为内置前端 |
| `@gebai/desktop` | `packages/desktop/` | 桌面端宿主：`dist/gebai.exe`（纯 Bun `--compile` 单文件，浏览器形态）+ `launcher/`（tao/wry 原生 WebView 启动器，内嵌服务端二进制一并打包） |

#### 仓库根目录

Monorepo 根目录包含以下脚手架文件，非运行时依赖，仅服务于开发与构建编排：

| 文件 | 用途 |
|------|------|
| `package.json` | 根包：Bun workspaces（`packages/*`）+ Turborepo 任务脚本（`dev`/`build`/`test`/`lint`/`typecheck`） |
| `turbo.json` | Turborepo 任务编排（缓存、依赖顺序、持久任务） |
| `tsconfig.base.json` | 各包共享的 TypeScript 基础配置（Bun/strict） |
| `kilo.json` | 开发工具（Kilo）项目配置 |
| `AGENTS.md` | 编码约定（对编码 Agent 的指令）：以 `DESIGN.md` 为权威设计来源，设计变更必须回写 `DESIGN.md` |
| `DESIGN.md` | **权威设计文档**，本文件 |
| `.env` | 开发本地配置（`GEBAI_*`/`OPENAI_*` 环境变量），**仅本地、不入版本库**；运行时配置一律以环境变量为准，无独立配置文件 |
| `.gitignore` | 忽略 `.env`、`node_modules`、构建产物等 |

> 编码约定由 `AGENTS.md` 承载（项目根，全局指令）；任何代码/设计变更须同时保持 `DESIGN.md` 与实现一致，新增能力时在 `DESIGN.md` 同步补充。

### SDK (`@gebai/sdk`)

客户端通过 SDK 与服务端 WebSocket/REST 连接，提供以下能力（TypeScript 官方 SDK，其他语言可基于 OpenAPI 规范生成）：

```ts
class GebaiClient {
  connect(): Promise<void> // WS 建连带 8s 超时，服务不可达/代理挂起时快速失败，避免初始化永久等待；连接地址解析：显式 baseUrl 优先（http→ws / https→wss），否则浏览器 DOM 下按 location.origin 解析为绝对地址（WebView 内嵌 about:blank/srcdoc、file: 等基址文档下相对路径会抛 "The URL '/ws' is invalid"），非 DOM 环境回退相对路径 `/ws`；连接建立后自动心跳保活（默认 5s 周期 ping，pong 应答超时 10s 判定死连主动断开触发自动重连，防代理按闲置时间断连；间隔/超时可在构造参数覆盖）
  // 认证（服务模式）
  login(username: string, password: string): Promise<void>
  logout(): Promise<void>
  // 未登录占位（公开端点/未认证上下文兜底；不作为认证结果）
  setApiKey(key: string): void
  // 会话
  listSessions(): Promise<SessionInfo[]>
  createSession(name?: string): Promise<SessionInfo>
  getSession(id: string): Promise<SessionInfo>
  deleteSession(id: string): Promise<void>
  renameSession(id: string, name: string): Promise<void>
  switchSession(id: string): Promise<void>
  getCurrentSession(): Promise<SessionInfo | null>
  // 环境变量（会话内存态，不落盘；用户环境变量只存浏览器本地）
  getSessionEnv(sessionId: string): Promise<Record<string, string>>
  setSessionEnv(sessionId: string, vars: Record<string, string | null>): Promise<void>
  // 上下文压缩
  compactSession(sessionId: string, scope?: "all" | { from: number; to: number }): Promise<void>
  // 任务控制
  cancelTask(sessionId: string): Promise<void>
  decideApproval(sessionId: string, toolCallId: string, approve: boolean): Promise<void>
  // 反馈
  submitFeedback(feedback: FeedbackInput): Promise<void>
  listFeedback(filter?: FeedbackFilter): Promise<FeedbackInfo[]>
  // 用户管理（服务模式，管理员）
  listUsers(): Promise<UserInfo[]>
  createUser(username: string, password: string): Promise<UserInfo>
  updateUser(id: string, patch: Partial<UserPatch>): Promise<UserInfo>
  deleteUser(id: string): Promise<void>
  // 子Agent
  listSubAgents(): Promise<SubAgentInfo[]>
  loadSubAgent(name: string): Promise<void>
  // HTML 小工具（REST）
  listMiniTools(): Promise<MiniToolMeta[]>
  getMiniTool(name: string): Promise<MiniToolInfo>
  deleteMiniTool(name: string, scope?: "public" | "private"): Promise<void>
  // 待办
  listTodos(sessionId: string): Promise<TodoItem[]>
  // 会话临时文件
  listSessionFiles(sessionId: string): Promise<FileEntry[]>
  readSessionFile(sessionId: string, path: string): Promise<string>
  downloadSessionFile(sessionId: string, path: string): Promise<Blob | Uint8Array>
  downloadFilesZip(sessionId: string, paths: string[]): Promise<Blob | Uint8Array>
  // 工具选择
  listTools(): Promise<ToolInfo[]>
  setToolEnabled(name: string, enabled: boolean): Promise<void>
  // 当前用户 / Webhook（REST）
  getCurrentUser(): Promise<UserInfo>
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
  // 附件（多模态）
  uploadAttachment(sessionId: string, file: Blob | Uint8Array, name: string): Promise<AttachmentInfo>
}
```

### Web UI

服务端内置 Web UI，由 Vite 构建打包并嵌入二进制。同一套 UI 同时服务于两种宿主：本地模式（WebView/浏览器）与服务模式（部署浏览器）。

> **开发**：脚本调试模式（`bun run dev`）下服务端托管 `packages/web/dist` 构建产物。启动时若检测到 `packages/web` 源码比 `dist` 产物新（或 `dist` 缺失），会**自动执行 web 构建**后再监听端口，避免「改了前端代码但页面仍是旧产物」；二进制模式不触发（产物随二进制分发）。**开发热刷新**：`bun run dev --reload`（或 `GEBAI_DEV_RELOAD=1`）额外启动 `bun run build:watch`（先经 `scripts/clean-dist.ts` 带重试安全清空 dist——Windows 上 vite 内置 emptyDir 无重试、删除瞬时占用文件会抛 `ENOTEMPTY` 崩溃，故 vite 配置 `emptyOutDir: false`；再 `vite build --watch`）——Web 源码变更自动增量重建 dist，构建完成后经专用 WebSocket 通道（`/__gebai_hot`）广播，页面自动刷新；页面注入的监听脚本在连接断开（服务端重启）后也会自动刷新页面。**首轮构建窗口期兜底**：`--reload` 启动后 dist 会被 clean-dist 清空、vite 尚需数秒重建，此窗口期 `GET /` 读取不到 `index.html`——服务端不再抛 ENOENT 崩溃，而是返回 503 占位页（「前端构建中」，复用 `/__gebai_hot` 监听构建完成广播自动刷新，另以 3s 定时刷新兜底），构建完成后下次请求即返回真实页面；dev-reload 模式下即使 dist 目录整体暂时缺失，Web UI 路由也保持注册。**HTML 不缓存**：dev-reload 模式下 `GET /` 每次请求重读 `dist/index.html`（vite 每次重建产出新 hash 资源，若缓存启动时的旧 HTML，页面刷新后仍会加载旧资源、改动永不生效）；非 dev-reload（生产/二进制）模式维持启动后首次读取并缓存。

> **构建性能**：图表渲染引擎全部**不参与 vite/rollup 打包**——构建/开发前由 `packages/web/scripts/build-vendor.ts` 原样拷贝到 `public/vendor/`（gitignore），运行时 `diagram.ts` 以**稳定文件名**按需加载：PlantUML 引擎 `@plantuml/core`（上游 TeaVM 编译单文件 `plantuml.js`，约 6.9MB，若走打包链路需 ~11s 占 web 构建 90%+，此方式将构建降至 ~1s）、`viz-global.js`（Graphviz 布局，classic script 注入）、Mermaid 官方 `dist/mermaid.min.js`（约 3.5MB 自包含 UMD，含全部图型）、ECharts 官方 `dist/echarts.min.js`（约 1MB 自包含 UMD，含 SVG 渲染器，SSR 模式直接输出 SVG 字符串）、D2 官方浏览器构建目录（`d2js/`：index.js + worker + wasm，内部相对路径引用）。**稳定文件名（无内容 hash）+ 静态伺服**：开发模式重建后 URL 不变，旧页面引用旧 hash 动态分块导致的 404（「Failed to fetch dynamically imported module」）**从根上消除**（`diagram.ts` 仍保留整页刷新一次兜底，覆盖极端缓存竞态）。产物随 `dist/` 一并分发。

页面清单：

| 页面 | 功能 |
|------|------|
| 登录页 | 用户名/密码（服务模式）、令牌登录态持久化（localStorage） |
| 聊天页 | 会话消息流、流式渲染（**markdown 链接一律新标签页打开**：`target="_blank"` + `rel="noopener noreferrer"`，防止原地跳转打断会话使用；**流式文本 120ms 尾沿节流渲染**（所有模式统一，markdown 全量重解析是流式期间最重的 CPU 开销，逐 chunk 同步渲染在长回答下 O(n²)））、附件上传、工具调用/审批卡片（**审批卡片为紧凑居中卡片**：限宽 ≤460px 居中显示，**仅展示工具名**（参数已在消息流工具卡片中展示，不重复），工具名单行省略号截断、悬浮 title 可见全名，避免长工具名撑成长条；**键盘快捷键 Y = 通过、N = 拒绝**，作用于最早等待的卡片，带修饰键或长按不触发，按钮内以弱化小字提示快捷键（矩阵主题沿用其 [Y]/[N] 前缀样式）；**工具调用即建卡**——参数先展示、结果到达后再追加到同一卡片（无参数调用仅头部，同样调用即建卡，不在执行完成后一并出现），输出完整不折叠）、压缩通知；首个回答完成后自动以首个用户输入生成会话标题（默认标题「新会话」时）；**输入历史**：输入框 ↑/↓ 键浏览**用户级全局输入历史**（跨会话通用、localStorage `gebai.ui.inputHistory` 持久化，按最后使用去重排序，同一输入只保留一条、限 50 条；空输入时 ↑ 进入、↓ 可恢复原草稿）；**多会话后台运行**：切换会话不中断进行中的回答，流式文本按会话累积，切回时恢复渲染（**推理累积同样恢复**）；**输入草稿与附件按会话保存/恢复**（切换保存、切回恢复，新会话/删除会话清理——A 的草稿不会误发到 B，附件上传绑定发起会话 id 不受切换影响）；**选择/环境变量填值卡片与审批卡片同构**（渲染到审批容器，切走隐藏、切回恢复，不随消息重载丢失；任务结束随审批一并清理；**同一请求 id 重复推送（断线重连事件 seq 重放）替换旧卡不堆叠**；draw 渲染与 page_capture 为页面级操作，后台会话同样执行不回传失败）；**ask_user 问答记录卡**（调用时中断当前文本段（封段）；等待作答期间消息流不渲染问题预览——交互作答由审批容器选择卡片承载，避免同款选择卡上下重复，结果到达时在消息流落问答记录卡：问题 + 选项展示态 + 「✓ 用户回答 / ✕ 用户拒绝 / ⏱ 选择超时」结果头部与回答文本，模型后续回复另起新气泡；历史重载按参数 + 结果同构渲染记录卡；新会话执行容器内同构渲染）；**plan 计划卡片**（同构于 ask_user：调用时开卡展示计划标题 + 计划 Markdown 全文（`# 标题` + `- [ ] 步骤` 勾选清单或模型直传 content，双端同构拼装），审批由审批容器选择卡片承载（批准执行/拒绝执行/自定义修改意见；**选择卡片出现时消息流滚动到底展示计划全文**——计划卡片在消息流底部，用户可能正上翻阅读历史，缺的是计划内容而非选择卡片），结果到达后卡片头部更新为「✓ 计划已批准 / ✕ 计划已拒绝 / ✕ 计划已取消 / ⏱ 计划审批超时」并追加结果文本；历史重载按持久化参数同构渲染计划全文、按结果文本呈现审批结果态）；**工具调用配对按 `会话:toolCallId` 隔离**（toolCallId 跨会话不保证唯一；切回会话时重建运行中卡片的 DOM 引用，后续结果仍追加同一卡片，任务结束清理残留配对）；**滚动位置跨会话记忆**（离开时未粘底则切回恢复原阅读位置，粘底/新会话落底）；**后台流结束不抢焦点**（焦点守卫：仅发起会话仍为当前会话时恢复输入焦点）；**撤回确认期间切换会话不打断当前浏览**（确认后校验会话归属再重载视图/回填输入框）；会话切换失败（网络抖动）回滚当前会话标记并提示，不留下状态不一致；审批卡片绑定来源会话，**仅显示当前会话的待审批卡片**（切走隐藏、切回恢复；后台会话的审批不打断当前会话）；**当前会话有待审批时锁定页面输入**（输入框/发送/附件禁用，焦点落到审批卡片，全部处理完恢复输入焦点），键盘 Y/N 处理；**任务结束（完成/取消/拒绝）时该会话残留审批卡片随任务终止清理**（审批已失效，避免卡片残留锁死输入）；**运行中输入排队**（见「会话输入队列」）：运行中 Enter/发送 = 输入**入本地会话输入队列**（不进消息流，呈现于输入框上方**排队条**——每条含序号/内容预览/附件计数与「⚡立即执行（中断当前任务）/✎编辑（内联）/✕撤回」操作，前端本地状态维护、当前回答结束后自动按序发送下一条），**Ctrl+Enter = 中断插入**（插队首并取消当前循环后立即执行本条），运行中发送按钮随草稿有无切换「排队发送（箭头）/停止（方块）」（空输入点击 = 停止，停止只取消当前任务、队列继续）；**消息撤回**：用户消息撤回按钮按**消息 id** 删除该消息及其后续（发送时前端携带 `messageId`、服务端采用同一 id 持久化，撤回对当前会话刚发的消息同样生效，失败 Toast 提示）；**会话内消息导航**：消息流右侧窄导航列（**消息流滚动条自动显隐**——与会话列表同构：沟槽恒定占位（`scrollbar-gutter: stable`，消息宽度不随滚动条显隐伸缩）、静止透明，hover 或滚动中（`.scrolling` 由共用助手 `ui.ts autoHideScrollbar` 维护：scroll 事件加类、停止 400ms 后移除）才显示滑块；导航列右移避让不变；曾整体隐藏滚动条、也曾长期常显），**只为用户输入**建一条短横线（等间距紧凑聚于导航列中部；**静止态 2px 细线**（1px 过细已加粗；125%/150% 缩放下 2/3px 交替肉眼可忽略），hover/focus 加粗至 3px 拉长变亮并浮出消息预览气泡——用户消息省略"我"的称谓标签），点击 / 键盘 / 拖动跳转；**滚动时高亮当前位置**（视口中心所在消息的短横线加长至 20px（普通 16px 与 hover 30px 之间）且加亮，同色无橙色区分；激活区域按「输入开始 → 下一条输入开始」划分，整段输出归属其输入，不按输出中点平分）；**连接韧性**：WS 断开自动指数退避重连（1s→30s，主动登出不重连），连接状态为**纯圆点信号灯**（位于标题栏右端、上下文占比圆环的圆心；**圆点绘制于圆环同一 SVG 内**（`.conn-dot` 第三圆，状态经 `#header-ctx:has(#conn.*)` 桥接、`#conn` 仅作状态载体——原 DOM `::before` 圆点在分数布局原点（44px 头部 - 1px 边框）上光栅化，100% 缩放下相对圆环恒偏半像素，同坐标系绘制后任何 DPI/主题下严格同心））——已连接且无生成任务时**隐藏**（无文字无胶囊），**任意会话运行（思考/流式生成）时显示并快速闪烁**（主题色圆点；流 150s 无数据视为挂起：前端主动中断并清理运行态，防信号灯残留常闪——阈值高于服务端 LLM 读空闲超时 120s，模型调用假死先由服务端超时上报明确错误，前端看门狗只兜底服务端检测不到的挂起；**看门狗中止且无任何输出时渲染「生成超时」说明气泡，不再静默结束**；**交互等待（选择/填值/画图/捕获）刷新活跃时间，等待用户回应的挂起不误判挂起**），断开/异常显示红色圆点常亮（圆心放不下原文字徽章，具体信息悬浮圆点经 data-tip 展示「已断开，自动重连中…」等）；**无输出收尾兜底**：任务结束/失败但没有任何可见输出（无文本/推理/工具卡/错误气泡）时补渲染说明气泡（「任务已结束，但没有收到任何回复内容」/错误原因），杜绝「无声无息就结束」；**消息发送走 WS 单通道**（`sendPrompt` 经 WS 发起任务、事件流渲染：文本/推理增量与审批/工具调用/结果统一由 `event.*` 推送驱动；**WS 断开时进行中的流挂起等待重连，重连后按事件 seq 重放离线内容无缝续流**——`resume` chunk 重置当前消息元素防重复渲染；长断线（>150s 无数据）由空闲超时兜底取消任务）；**按钮轮盘**：小工具 ◫ / 导出 ⤓ / 压缩 ⤤（会话操作组）与自动审批 ⚡ / 极简模式（三横递减图标，会话仅启用 sh 与 edit 工具，任务中模型可经 `full_mode` 工具（需审批）切换完整模式，切换事件 `event.session.minimal` 随之关闭本地开关）/ 设置 ⚙ / 主题 / 登出（应用操作组）8 个低频操作按钮收敛为**标题栏最右按钮轮盘**（九宫格网格入口，平时仅显示此一个按钮，悬浮展开为扇形）：**双弧扇形**——内弧 r=85 会话操作组（accent 边框组标识、背景与扇形按钮一致——不用 accent 调底，避免与开关开启态 accent-soft 底混淆）、外弧 r=145 应用操作组（按可见按钮数在角度区间内均布，登出隐藏时自动重排），两弧之间 r=115 细弧线分区（标题栏高 44px，扇形只能朝左下方展开，全部按钮不被标题栏遮挡）；纯 hover 交互（不支持点击切换）：入口 hover **立即展开（无延迟）**、**指针在扇形区域内保持展开（保持区=入口按钮∪扇形按钮的边界盒外扩 8px，按可见按钮动态收紧），离开区域 250ms 延迟收起**，外点/Esc/窗口 resize 关闭；展开时扇形按钮自圆心错落弹出（translate+scale+opacity 过渡）；扇形按钮**双背景垫实**（本体与 ::before 各铺一层 `--bg-elev-2`，半透明主题双层叠加 ~95% 不透明、不透明主题本就实心，全主题表面接近实色）+ 文字色边框阴影；**扇形按钮图标统一 `--text`**（accent 图标叠 accent 底相对亮度差被抹平——亮色主题（acrylic 亮/classic/modern/cny）仅 2:1~4:1 不可读，accent 仅作底色/边框组标识，图标全主题 ≥7:1）；**轮盘内开关类按钮（自动审批/极简模式）开启态同款处理**（accent-soft 底 + accent 边框作"开"标识、图标保持 `--text`——主题 `[data-theme] .icon-btn.active` 的 accent 染色同样会让亮色主题看不清）；**入口按钮静止态无底色、无边框**（仅 `--text` 亮图标，标题栏内靠图标本身可辨——`.icon-btn` 基线静止态无底色（背景 none/边框 transparent/图标 muted）、各主题仅处理 hover/active，故入口按钮以 `#wheel-btn.icon-btn`（1,1,0）兜底压过所有主题只补图标亮色；hover 底色 `--bg-hover`、展开态 `.active` accent 底，图标恒用 `--text`）；8 按钮由隐藏源容器移入（事件绑定在元素上不失效），`logout` 显隐仍由 auth 控制；标题栏保留折叠按钮/会话标题/**上下文圆环 + 轮盘入口构成右侧固定组**（`.header-right` 容器单一 `margin-left:auto` 整体靠右——ctx 与入口各自带 auto margin 会被 flex 平分剩余空间、把 ctx 拉向中线与居中标题重叠；ctx 隐藏时组容器仍占据右端、入口不左移）/**上下文占比圆环**（当前会话 ctxTokens / 模型窗口：24px SVG 圆环弧线（`pathLength=100` 归一化周长，`stroke-dashoffset=100-占比` 免换算）+ **圆心连接信号灯**（见「连接韧性」），悬浮圆环 data-tip 显示精确值与百分比；**切换会话即切换**，运行中随 `event.session.ctx` 每轮实时更新；弧线比例分级着色 <50% 主题色 / 50-80% 警告色 / ≥80% 危险色；窗口大小来自快照 `maxContextTokens`，未知时隐藏圆环仅剩圆心信号灯）；**小工具弹窗与悬浮窗口**：点「小工具」弹出已保存 HTML 小工具列表（公用/私有徽标、可刷新），点击加载到独立悬浮窗口（沙箱 iframe、可拖拽/缩放/刷新/关闭），重复点击同一工具恢复之前的窗口不重新加载；**会话搜索框带图标与占位提示**（放大镜 SVG 图标 +「搜索会话」文字提示，聚焦时图标随边框变 accent 色）；**发送按钮为圆角上箭头图标**（线性描边，与停止方块同控显隐）；**新会话懒创建（草稿页）**：点击「＋ 新会话」/登录后无会话/删除全部会话的兜底均只进入**空白草稿页**（不落盘创建会话——避免每次点击产生一个空会话堆积），首条消息发送时才真正创建会话（创建失败保留输入并提示）；草稿态由显式标志维护，迟到的服务端快照不把草稿页覆盖回服务端记忆的当前会话；**空白页快捷按钮**：唯一管理入口在空白页——内置「查看子代理和工具」胶囊（与用户快捷同级样式、不可删，点击填入提示词由模型编排工具作答）居首；用户胶囊为**单按钮样式**（无嵌套边框），点击把内容填入输入框、hover 浮层预览完整内容、右上角显现 ✕ 徽章删除（确认弹窗）；末尾「＋ 添加」虚线胶囊弹窗新增（标题+内容，内容支持多行——Enter 换行、Ctrl+Enter 提交，Esc/遮罩取消）；localStorage `gebai.ui.shortcuts` 仅存本浏览器 |
| 会话列表 | 新建/切换/删除会话，双击会话名重命名，上下文压缩入口（标题栏轮盘 🗜️）；**行内仅保留选中按钮**：每个会话行时间区有一个**勾选框（选中按钮）**——**默认隐藏**，右键菜单「选中」进入多选（批量）模式后常驻显示，点击勾选/取消（多选统一走该按钮）；✎/✕ 操作按钮已移除，**重命名/删除/复制会话 ID 收敛到右键菜单**（屏蔽浏览器默认菜单，随光标定位 + 视口边缘翻转，Esc/点击/滚动/新右键关闭；「复制会话 ID」任意条目可用、Toast 反馈——定位问题/反馈用；批量模式下菜单项变为「选择/取消选择」）；**滚动条占位恒定**：滚动条沟槽恒定保留（`scrollbar-gutter: stable`，静止透明、hover/滑动才显色），会话条目宽度不随 hover 伸缩（覆盖式滚动条环境此前 hover 时条目无端变长 8px、离开缩回）；**批量删除**：右键「选中」进入多选（批量）模式（批量模式下点击行也切换选中），批量模式下操作条显示已选计数，删除所选走确认弹窗逐个删除；全部取消自动退出批量模式；当前会话被删自动切换到剩余第一个或新建；**删除成功后重建列表**；**按日期分组**：列表按更新时间分组为 今天/昨天/近7天/更早（组内按更新时间倒序），**组头点击折叠/展开**（折叠组不渲染成员，箭头旋转指示，折叠状态 localStorage `gebai.ui.sessionsCollapsed` 记忆；批量模式下强制全展开——折叠分组全部可见可勾选，**组头点击切换全组选中/取消**；搜索时平铺不分组）；**整栏折叠**：桌面端（>860px）标题栏最左按钮折叠/展开整个会话列表（折叠隐藏侧栏、主区占满，状态 `gebai.ui.sidebarCollapsed` 持久化；**Ctrl+B 快捷键等效切换**），窄屏维持滑动抽屉行为 |
| 环境变量页 | 浏览器本地（localStorage）增删改，对本浏览器所有会话生效，随消息临时注入服务端（不落盘防泄露；服务端不配模型变量时仅前端配置即可使用）——并入设置面板 |
| 设置页 | UI 风格（外观性能模式）、`/approval-skip`（标题栏轮盘 ⚡）——设置面板；工具启停、子Agent 装载、Webhook 管理无 UI（设置面板不设对应 tab，经 SDK/API 使用） |
| 用户管理页（管理员） | 用户创建/禁用/删除（服务模式）——并入设置面板 |

- **启动动画画面**：页面加载（外部 CSS/JS 就绪前）即显示全屏启动动画——深黑底 + 极光呼吸光球 + 旋转光环 + 歌白品牌辉光文字（样式内联于 `index.html`，不依赖外部样式，避免「经典蓝色主题空白窗口」闪现）；`init()` 完成（含服务模式未登录转登录层）或失败后淡出移除，另以 12s 内联脚本兜底防残留
| 反馈页（管理员） | 反馈查询/导出——并入设置面板 |

> **实现**：登录页与设置面板已落地；环境变量/用户/反馈三页统一收敛为设置抽屉（标题栏轮盘 ⚙️），管理员身份自动显示用户/反馈 tab。工具启停、子Agent 装载与 Webhook 管理原设置面板 tab 已移除（对使用者无实际意义：启停/装载由模型经 `agent_load` 等自主完成、Webhook 属服务端集成配置），对应能力保留在服务端 API 与 SDK 方法中。

#### 自研交互组件（零浏览器原生交互）

前端**不依赖浏览器原生交互控件**，全部交互组件基于主题 CSS 变量自绘（`packages/web/src/ui.ts` + `css/overlays.css`）：

- **对话框**：`alert`/`confirm` 全部替换——错误提示走 **Toast**（底部居中浮层，`toast(text, kind)`，自动消失，`error`/`ok` 两种色点）；确认走**自绘模态框** `confirmDialog({ title, text, okLabel, danger, list })`（返回 Promise<boolean>，Esc/遮罩/取消关闭，复用 `.preview-overlay` + `.confirm-card` 样式，支持待删条目列表展示）
- **Tooltip**：`title` 属性全部替换为 `data-tip` 属性（JS 侧 `tip(el, text)` 助手），`bindTooltips()` 全局委托（pointerover/pointerout/focusin/focusout）渲染单个固定定位浮层——不受容器 overflow 裁剪、视口边缘翻转、长文本省略
- **表单控件**：`<select>` 替换为自绘下拉 `customSelect({ options, value, onChange })`（按钮 + 固定定位浮层，Esc/外部点击关闭，暴露 `root`/`value`/`setValue`；**浮层懒创建且挂载到 `document.body`**——经典主题等给行加 hover `transform`（`.settings-row` / `#session-list li` 的 `translateX`），transform 祖先会成为 fixed 子元素的 containing block，浮层 `left/top` 会被解释为行内局部坐标而飞出屏外（实测偏移约 820px）；挂 body 后 fixed 始终相对视口，按钮 `getBoundingClientRect()` 视口坐标直接可用；同时**每次打开重建、关闭即移除**，设置面板重渲染不会在 body 堆积隐藏的孤儿浮层）；`datalist` 替换为输入联想浮层 `bindSuggestions(input, items)`（focus/input 过滤展示，点击回填，浮层同样挂 body）；复选框自绘（`appearance: none`，`.ck` 类，勾选 SVG 对勾）；`required` 原生校验气泡关闭（表单 `novalidate` + 提交时自绘校验，失败 Toast 提示）；文件选择沿用隐藏 input + 自定义触发按钮

#### HTML 小工具库（Mini Tools）

Agent 可将**调试好的 HTML 小工具**保存到服务端（标题栏轮盘「小工具」按钮随时加载使用），与消息流内的 `render_html` 预览卡片互补——前者一次性展示，后者持久可复用：

- **保存流程**：Agent 先用 `render_html` 在聊天中调试预览，满意后装载 `widgets` 子Agent 保存（`widgets_save`，参数 `name`/`html`/`scope`；`scope=public` 公用 / `private` 用户私有，默认私有）；小工具增删改查经 `widgets_save`/`widgets_list`/`widgets_get`/`widgets_delete` 四工具（`delete` 需审批），详见「widgets 子Agent」
- **存储**：公用存 `{GEBAI_HOME}/tools/{h0}/{h1}/{name}.json`（全局共享、记录 `owner`），私有存 `{GEBAI_HOME}/users/{user}/tools/{h0}/{h1}/{name}.json`（按名称哈希分片，仅本人可见）；同名覆盖更新（保留 `createdAt`）；私有与公用同名时**私有优先**（列表与读取均只命中用户侧）
- **名称约束**：`[a-zA-Z0-9_\u4e00-\u9fff]{1,40}`（不含 `.`/`/` 等路径分隔符，直接作为文件名），HTML 上限 200KB
- **权限**：私有工具天然按用户目录隔离（他人不可读/删）；公用工具任何用户可读，**服务模式下仅管理员可创建/覆盖/删除**（共享资源防投毒——普通用户 `widgets_save`/`widgets_delete`/REST DELETE 指定 public 一律拒绝并提示改用 private；本地模式不限制）；删除公用工具需审批（本地模式）
- **REST**：`GET /api/v1/mini-tools`（列表，不含 html）、`GET /api/v1/mini-tools/:name`（单条含 html）、`DELETE /api/v1/mini-tools/:name?scope=`（删除）；SDK：`listMiniTools`/`getMiniTool`/`deleteMiniTool`
- **UI**：轮盘内弧「小工具」按钮（会话操作组最左）→ 弹窗列表（名称 + 公用/私有徽标，打开时实时拉取、可手动刷新；行 hover 显示 ✕ 删除按钮，触屏常驻——**删除走确认弹窗**：私有「删除后不可恢复」、公用额外强调「所有用户不可用」，成功后刷新列表并**一并关闭该工具已打开的悬浮窗口**）→ 点击**加载到独立悬浮窗口**（沙箱 iframe 渲染，域隔离同 `render_html`：`sandbox="allow-scripts"` 无 `allow-same-origin`，主题跟随广播）；**重复点击同一工具 = 恢复之前的窗口**（置顶聚焦，不重新创建/重载）；**关闭（✕）仅隐藏窗口**（微信小程序式：保留 iframe 运行状态，再次点击同工具即恢复，不重载），**刷新按钮才重新加载内容**（工具更新后可见新版本）；窗口可拖拽（指针捕获，松手即停）/缩放，多个工具可同时打开（层叠定位，点击置顶）
- **安全**：工具 HTML 视为不可信输入，渲染于沙箱 iframe（与 `render_html` 同一隔离与 CSP 注入管线）

#### 多套风格 UI

前端支持**多套 UI 风格**，同一套前端代码、同一份业务逻辑，运行时按需加载不同风格：
- **内置风格**：内置多套风格，随二进制分发，切换无需重新部署：

  | 风格 | 特征 |
  |------|------|
  | `acrylic` | 亚克力（默认）：黑色半透明面板 + 毛玻璃（backdrop blur）、近黑底极弱光晕，**会话元素（列表条目/搜索框等）零边框**，纯靠背景层次与圆角区分；支持**黑白（暗/亮）切换** |
  | `aether` | 以太（旗舰）：光之玻璃、青紫粉渐变辉光 |
  | `classic` | 经典：深蓝调 + 背景光晕、渐变按钮与辉光细节（`:root` 令牌兜底） |
  | `dark` | 暗夜：近黑高对比、青蓝冷光晕、荧光 accent |
  | `modern` | 现代：多层彩色光斑、强玻璃拟态、紫粉渐变与发光按钮 |
  | `minimal` | 极简：黑白高级质感、锐利焦点态、选中指示条 |
  | `matrix` | 矩阵：纯黑底荧光绿、等宽字体、终端质感 |
  | `tokyo-night` | 东京夜：程序员配色、紫/青/粉点缀、柔和霓虹 |
  | `cyberpunk` | 赛博：霓虹粉/青光晕、彩色辉光、赛博朋克 |
  | `synthwave` | 浪潮：复古网格线背景、橙粉紫霓虹日落 |
  | `aurora` | 极光：深空底 + 青绿紫三色渐变、大圆角玻璃面板 |
  | `cny` | 人民币：中国红丝绒底 + 鎏金点缀、面额色渐变气泡、纸钞防伪纹细节；**招财猫玩偶**（悬浮可拖动 + 爆金币特效，见「人民币招财猫」） |

- **主题面板排序与交互**：默认主题（`acrylic`）**不归属任何分组**，独立显示于列表顶部（无分组标题、无说明）；其余按风格族分组展示，组内保持固定顺序——基础（`classic`/`dark`/`modern`/`minimal`）→ 科技风（`matrix`/`tokyo-night`/`cyberpunk`/`synthwave`）→ 氛围风（`aether`/`aurora`）→ 特色（`cny`）；主题按钮悬浮 120ms 自动展开，指针离开入口/面板 250ms 延迟收起，外点/Esc/窗口 resize 关闭；选中主题后立即切换并关闭面板

- **风格解析优先级**（高 → 低）：
  1. URL 参数 `?gb_style=<id>`（宿主 / iframe 注入，业务系统嵌入指定风格）
  2. 用户级：界面主题面板切换，持久化 `localStorage["gebai.ui.style"]`
  3. 全局：环境变量 `GEBAI_UI_STYLE`，由服务端渲染 `/` 时注入 `window.__GEBAI_UI_STYLE__`
  4. 默认 `acrylic`
- **按宿主默认**：桌面端/浏览器/服务端部署可各自通过 `GEBAI_UI_STYLE` 配置全局默认风格
- **主题变量化**：UI 样式基于 CSS 变量/设计令牌（颜色、圆角、阴影、字体、布局令牌）实现，风格间差异仅体现在令牌与组件装饰层，核心组件与交互逻辑完全复用
- **轻量热切换**：非默认风格为独立 CSS 文件（`src/themes/*.css`），运行时动态 `<link>` 按需加载，切换无需刷新；未选中风格不加载其资源；加载失败静默回退默认令牌
- **自定义风格**：支持外部注入 CSS 变量覆盖，URL 参数 `?gb_vars=--accent:%236366f1,--radius-md:8px`（逗号分隔 `--变量:值`）写入根元素内联样式，满足品牌化嵌入需求
- **人民币面额配色**：人民币主题（`cny`）内置 6 种面额配色（100 红 / 50 绿 / 20 棕 / 10 蓝黑 / 5 紫 / 1 橄榄绿），通过根元素 `data-cny-scheme` 属性切换 `cny.css` 中的配色变量块；解析优先级：会话手动选择 → URL 参数 `?gb_cny=<id>`（宿主注入） → 用户级持久化 `localStorage["gebai.ui.cnyScheme"]` → 默认 100 元红；主题面板在人民币主题激活时显示「人民币配色」分组（其余主题隐藏）。**面额配色即强调色**：每个面额完整定义 `--accent` 三件套
- **默认主题黑白切换**：默认主题（`acrylic`）支持黑白（暗/亮）两档切换，根元素 `data-acrylic-lt` 切换 `acrylic.css` 中的白色亚克力变量块（白色半透明面板 + 浅色光晕 + 深色文字全套变量重定义）；解析优先级：会话手动选择 → URL 参数 `?gb_acrylic_lt=<id>`（宿主注入） → 用户级持久化 `localStorage["gebai.ui.acrylicLt"]` → 默认暗色；主题面板中为**独立一行色块、不归属任何分组**（仅默认主题激活时显示）
- **人民币招财猫**：人民币主题（`cny`）专属**悬浮玩偶 + 爆金币特效**（`cny-cat.ts` 玩偶交互 + `coin-fx.ts` 全屏 canvas 粒子引擎），仅 `data-theme=cny` 时挂载（监听 `gebai:theme-change` 同步增删）：
  - **玩偶**：内联 SVG 招财猫（挥手手臂独立分组做 CSS 摆动动画），悬浮可拖动（pointer capture，视口钳制），位置持久化 `localStorage["gebai.ui.cnyCat"]`；核心定位样式内联（主题 CSS 卸载时序不影响），装饰动效（挥手/悬浮/弹跳）在 `cny.css`
  - **拖动金币/纸币轨迹**：拖动期间沿指针位置混撒金币与纸币（30ms 节流：每拍一枚小金币 + 半数概率一张小面额纸币，`popTrailMoney` 低初速小尺寸快速消隐）；点击与拖动按累计位移阈值（6px）判定
  - **点击爆发 + 连击递增**：点击爆少量金币，900ms 内连续点击连击 +1、每次爆得更多（10 起步、每击 +7、封顶 58，初速随连击小幅提升）；错误/取消不触发
  - **单轮完成大爆发**：任务正常完成（`done` 分片）时从猫位置大量爆发，**运行越久爆得越多**（36 + 每秒 1.4，封顶 230，分 2-4 波间隔 110ms 发射）；与单轮计时器共用同一起算点（`RunState.startedAt`）
  - **粒子渲染**：金币与各面额纸币约各半——金币 3D 翻转（椭圆面压缩 + 近侧视边缘厚度、鎏金径向渐变、内环 + ¥ 浮雕、落地反弹两次后静止消隐）；纸币取第五套人民币六面额配色（与面额配色同源 `CNY_SCHEMES`），飘落摇摆（终端速度 + 正弦横摆 + 绕横轴翻面的纵向压缩），版面细节（内框双线/水印圆/人像剪影/面额与角标数字）；**预渲染精灵**（金币 32 相位帧 × 各半径、纸币各面额版面，离屏 canvas 2x 超采样）逐帧只做 transform + drawImage；画布懒创建（定位样式内联）、粒子耗尽自动移除、上限 420 粒子、rAF 仅在有粒子时运行
  - **降级**：低性能模式 / `prefers-reduced-motion` 不发射粒子（玩偶保留、挥手动画由全局低功耗规则停用）
- **低性能模式**：无 GPU / 低配机器自动开启，可手动强制开启（设置面板「外观」tab 单行按钮：开启=强制低性能，关闭=恢复自动检测，说明一行显示当前生效状态），不影响配色与布局、仅降级特效与渲染开销：
  - **自动检测（默认）**：WebGL 不可用（GPU 缺失/驱动禁用）、CPU ≤4 核、内存 ≤4GB（`navigator.deviceMemory`，Chrome/Edge），任一命中即开启；`localStorage["gebai.ui.lowPower"]` 仅存 `"on"`（强制开启），不存/其它值 = 自动检测（旧三态 `auto`/`off` 兼容映射），跨标签页 storage 事件同步
  - **生效标记**：根元素 `data-low-power="on"`，CSS 全局关闭动画/过渡（主题呼吸、脉冲、入场动画等）、View Transitions 与扫描光切换动画、`backdrop-filter` 毛玻璃（无 GPU 时持续重绘最卡）；**状态指示动画豁免**：流式输出光标（`caret-blink`）与连接信号灯闪烁（`conn-thinking`）是任务进行中的必要指示且开销极小，低性能模式下保留
  - **消息渲染降级**：视口外消息跳过布局/绘制（`.msg` `content-visibility: auto`；**高度估算按内容长度内联** `contain-intrinsic-size: auto {估算}px`（`appendMsg` 依字符数/块卡片数估算，样式表 320px 仅为兜底）——统一 320px 估算时长回答（真实高度可达数千 px）滚动经过首渲染高度突变顶开视口「跳一大段」，按内容估算把突变降到误差级；`auto` 关键字记忆上次渲染高度，回看稳定；含 iframe 卡片移入视口时浏览器再渲染）；主消息区 `#messages`、新会话容器 `.session-body` 与推理体 `.reasoning-body` 关闭浏览器滚动锚定（`overflow-anchor: none`，全模式生效）——content-visibility 与 scroll anchoring 组合缺陷：生成中底部内容持续增长、用户上翻后锚定节点被跳过渲染/高度异步修正，锚定算法选中失效锚点把视口猛拉到列表顶部（跳回第一输入）；且锚定调整自行产生滚动事件、粘底跟随核心（sticky-follow，见「聊天页」）无法将其归因为内部动作，内容收缩（推理折叠/容器折叠）时会把贴底视口拽离底部。粘底跟随由应用自管，底部追加不影响已上翻的阅读位置
  - **流式输出降频**：文本 delta 渲染按 120ms 尾沿节流合并（`scheduleStreamRender`，计时器挂 `RunState.renderTimer`，封段/重置/结束时随 run 清理；**封段前冲刷最后一帧**——定时器待触发即存在未上屏增量，只清零不冲刷会把最后一个节流窗口（120ms）内的文本永久丢失，快速模型整轮回复可全部落在一个窗口内、气泡残留空白（agent_run 执行过程「只见结果不见过程」的主因））——markdown 全量重解析是流式期间最重 CPU 开销；标准模式保持逐 chunk 同步渲染
  - **代码高亮降级**：无语言标注的代码块跳过 `highlightAuto` 自动检测（穷举全部语言最贵），仅转义；显式语言标注仍正常高亮
  - **图表始终默认渲染**：缩略图自动渲染（不随低性能模式改为按需，避免「图不默认渲染」困惑；渲染失败只显示占位提示、错误细节不暴露到主页面，进查看器/控制台）；本地渲染引擎不做空闲预热（低性能模式，首次渲染由消息流触发）；PNG 导出超采样 3x → 1.5x、复制图片 2x → 1x（`isLowPower()` 实时读取），渲染与 draw 工具链路不变

- **单轮计时器**：任务（单轮对话）耗时显示于**标题栏右侧、上下文占比左侧**（`.turn-timer`，细线沙漏图标 + 时长文本，常驻单一位置）：运行中实时走（250ms 刷新、图标呼吸动效），**运行结束不清除**（定格保留显示），**下次运行重启归零**；多会话并发时由最近一次开始的运行接管显示（其余 run 的 tick 空转）。**六级时长渐进变色**（`data-dur` 属性，冷→暖，阈值 10s/30s/1m/3m/5m）：<10s 极淡（`--text-faint`）、10-30s 中性（`--text-muted`）、30s-1m 工具青绿（`--tool`）、1-3m 主题色（`--accent`）、3-5m 警告（`--warning`）、≥5m 危险（`--danger`），与上下文圆环的分级着色语言一致。**信号灯思考闪烁周期同步六级放缓**：运行中把同一 `data-dur` 同步到 `#header-ctx`，`--conn-blink` 变量按级取 0.07s → 0.11s → 0.16s → 0.25s → 0.4s → 0.6s（**越久越慢——闪得慢=执行得慢**：刚开始急促闪烁表活跃，拖长后逐渐放缓表执行缓慢；SVG 圆点 `.conn-dot` 经继承解析；运行结束移除属性恢复默认 0.25s；低性能模式豁免保留闪烁且保留分级周期；`no-ring` 无数据模式仅隐藏轨道/弧线、圆点仍可显示）。**hover 显示总运行时**（`data-tip`）：每轮净耗时累加（含进行中这轮的已耗时，不含轮间空闲），而非距第一条消息的墙钟时间；累计为页面生命周期内统计，刷新后归零。默认开启；设置面板「外观」tab 单行按钮可关（`localStorage["gebai.ui.turnTimer"]` 仅存 `"off"`，根元素 `data-turn-timer="off"` 时 CSS 隐藏，跨标签页 storage 事件同步，运行中被关闭即时停表隐藏）。纯展示层能力：不持久化到会话记录（`Message` 无耗时字段）

## 架构

### 运行形态

一套核心代码、同一个二进制可执行文件，支持两种运行形态，由启动参数/环境变量切换：

| 形态 | 启动方式 | 说明 |
|------|---------|------|
| **本地模式**（默认） | 直接运行二进制 / 访问 `http://127.0.0.1:{port}` | 本机使用，直接以 **admin 用户**身份工作，**免登录、不受任何权限限制**（超级权限 + 路径沙箱豁免）；桌面 WebView / 浏览器均可 |
| **服务模式** | 后台运行二进制 + `GEBAI_MODE=server`（或 `--server` 参数） | 部署在服务器上，多用户公用，**账号密码登录**（密码仅存加盐哈希）；**开放注册**（注册用户为普通角色）；admin 为特权用户，**唯一入口是 `GEBAI_ADMIN_PASSWORD_HASH`**（未设置时 admin 禁用） |

核心逻辑完全一致，仅宿主（WebView / 浏览器 / 远程）与用户机制不同。

#### 启动参数与环境变量

| 参数/变量 | 说明 | 默认 |
|----------|------|------|
| `GEBAI_HOST` | 监听地址（`127.0.0.1` 本地 / `0.0.0.0` 对外） | `127.0.0.1` |
| `GEBAI_PORT` | 监听端口（桌面形态默认固定 47896，见「端口固定」） | `3000` |
| `GEBAI_PLAYWRIGHT_CHANNEL` | playwright 子Agent 浏览器启动 channel（`chromium.launch` 的 channel 参数，留空 = 不指定用默认 chromium） | 平台默认：Windows=`msedge`（系统 Edge，免下载浏览器），其余空 |
| `GEBAI_MODE` | 运行形态：`local`（本地模式，默认）/ `server`（服务模式，需登录鉴权）；兼容旧 `GEBAI_AUTH`（`none`→local、`multi`→server） | `local` |
| `GEBAI_ADMIN_PASSWORD_HASH` | 服务模式 admin 用户密码哈希（格式 `salt:hash` 均为 hex，salt 16 字节/hash 64 字节，与注册表 scrypt 加盐哈希一致）；**设置则启用 admin（每次启动覆盖其哈希），不设置则禁用 admin 用户**；非法格式启动即报错。生成命令：`bun run --cwd packages/server hash-password -- '密码'`（或管道/交互输入） | 空（admin 禁用） |
| `GEBAI_SERVICE_API_KEY` | ~~已移除~~：原业务系统服务密钥机制已废止——**接口统一账号密码认证**（登录签发令牌），不再有独立服务令牌（避免任何服务端密钥进入 Agent 可达环境） | - |
| `GEBAI_CORS_ORIGINS` | 允许跨域来源（逗号分隔，`*` 表示全部） | `*` |
| `GEBAI_BASE_PATH` | 反向代理挂载前缀（如 `/gebai`），静态资源/API/WS 均以该前缀为基准 | `/` |
| `GEBAI_TRUST_PROXY` | 是否信任 `X-Forwarded-*` 代理头（`true`/`false`） | `false` |
| `GEBAI_SANDBOX` | 路径沙箱：`auto`（**只看运行形态**——服务模式强制启用，本地模式不限制；不按监听地址/IP 判定）/ `on`（强制限制）/ `off`（不限制）；**admin 用户豁免**（特权用户，不受用户沙箱控制，见「多用户隔离与安全」） | `auto` |
| `GEBAI_PRELOAD_SUB_AGENTS` | 启动预载子Agent 名单（逗号分隔）：启动时注册其工具，**每个新会话创建时自动装载**（提示词 system 消息写入会话记录 + 工具注册）；为空 = 默认不预载任何子Agent | 空 |
| `GEBAI_UI_STYLE` | 默认 UI 风格（`acrylic`/`aether`/`classic`/`dark`/`modern`/`minimal`/`matrix`/`tokyo-night`/`cyberpunk`/`synthwave`/`aurora`/`cny`），可被 URL/用户级覆盖 | `acrylic` |
| `GEBAI_LOG_LEVEL` | 日志级别：`debug`/`info`/`warn`/`error` | `info` |
| `GEBAI_TOOL_ENABLE` | 工具白名单（逗号分隔，配置后仅启用列表内工具） | 空（全部启用） |
| `GEBAI_TOOL_DISABLE` | 工具黑名单（逗号分隔，排除指定工具） | 空 |
| `GEBAI_FEISHU_*` | 飞书集成配置：全局应用凭证 `GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`（`feishu_docs` 子Agent 的全局兜底 + 机器人桥接凭证）、机器人桥接开关 `GEBAI_FEISHU_BOT_ENABLED`（`true` 启用长连接事件订阅，见「飞书机器人集成」）、**TLS 策略 `GEBAI_FEISHU_INSECURE_TLS`（`true`/`1` 时所有飞书出站请求禁用证书校验——内网代理场景：机器人桥接 REST/长连接 WebSocket、`feishu_docs` 子Agent 接口与 OAuth 回调兑换，见「飞书 TLS 策略」）** | 不启用 |
| `GEBAI_PUBLIC_URL` | 对外可访问地址（如 `http://localhost:3000` 或公网域名）：飞书用户授权（user_access_token）自动回调默认取 `{GEBAI_PUBLIC_URL}/api/v1/oauth/feishu/callback`（缺省回落 `http://localhost:{GEBAI_PORT|3000}`），需在开发者后台「安全设置 → 重定向 URL」登记 | 空（回落 localhost） |
| `GEBAI_SELF_MODIFY` | 是否允许 `self_optimize` 修改服务端源码（`true`/`false`） | `false` |
| `GEBAI_SAFE_MODE` | 安全模式（**仅启动时从 .env/环境变量加载，不可在会话/任务级修改**）：风险能力**降级而非禁用**——`sh` 只读命令白名单 + 重定向限用户目录、`py`/`js` 只读运行时（写/进程/网络屏蔽，仅保留文件读取）、`write`/`edit`/`patch`/`file` 限定用户目录内、`cron_*` 调度类硬阻断、子Agent 工具按 `Tool.safeMode` 自主声明过滤注册（详见「安全模型 → 安全模式」） | `false` |
| `GEBAI_LLM_MODEL` / `OPENAI_*` | LLM Provider 与模型配置 | - |
| `GEBAI_LLM_API_BASE` / `GEBAI_LLM_API_KEY` | LLM 服务地址与密钥（等价 `OPENAI_*`） | - |
| `GEBAI_LLM_API_KIND` | LLM 接口类型：`openai`（兼容 chat/completions）/ `responses`（OpenAI Responses API）/ `anthropic` | `openai` |
| `GEBAI_LLM_EXTRA_PARAMS` | 额外模型接口参数（JSON 对象，如 `{"reasoning_effort":"high"}`），每次请求顶层合并进请求体；非法 JSON 启动即报错 | 空 |
| `GEBAI_LLM_MULTIMODAL` | 主模型多模态能力声明：`true` 时图片附件 base64 内联进消息，`vision` 工具可回落到主模型；默认 `false`（纯文本模型须配 `GEBAI_VISION_*` 外挂视觉模型；声明了但接口拒绝图片时引擎自动降级为文本说明） | `false` |
| `GEBAI_VISION_MODEL` | **额外多模态（视觉）模型**：配置后启用独立视觉 Provider（`vision` 工具使用）；接口地址/密钥/类型缺省时继承主模型配置；不配置则 `vision` 工具回落到主模型（须声明多模态能力） | 空 |
| `GEBAI_VISION_API_BASE` / `GEBAI_VISION_API_KEY` | 视觉模型接口地址与密钥（缺省继承主模型） | - |
| `GEBAI_VISION_API_KIND` | 视觉模型接口类型：`openai`（兼容）/ `responses` / `anthropic` | 同主模型 |
| `GEBAI_VISION_MAX_CONTEXT` | 视觉模型上下文 token 预算 | `128000` |
| `GEBAI_VISION_EXTRA_PARAMS` | 视觉模型额外请求体参数（JSON 对象，同 `GEBAI_LLM_EXTRA_PARAMS` 语义） | 空 |
| `GEBAI_SIGNUP_MODE` | 注册审批模式：`open`（默认，注册即用）/ `approval`（注册待 admin 审批——用户置 `disabled+pending` 待审、不可登录，admin 在用户管理页批准/拒绝） | `open` |
| `GEBAI_APPROVAL_SKIP` | 会话级审批跳过（等价 `/approval-skip`，`true` 跳过） | 空 |
| `GEBAI_MINIMAL_MODE` | 会话级极简模式（`true` 仅启用 `sh` 与 `edit` 工具（外加 `full_mode` 切换入口），其余工具从 schema 移除且调用被阻止，系统提示词同步极简化；前端「极简模式」开关同步写入，见「工具选择」） | 空 |
| `GEBAI_CRON_ENABLED` | 是否启用定时任务能力（`true` 注册 `cron` 子Agent（`cron_add`/`cron_list`/`cron_update`/`cron_remove` 工具）并启动调度器，装载后会话内可直接创建定时任务；`false` 时子Agent 不注册、调度器不启动，能力完全不可见） | `false` |
| `GEBAI_EXTERNAL_AUTH_SECRET` | 外部身份扩展点：HMAC 共享密钥（与 `GEBAI_EXTERNAL_AUTH_URL` 互斥，同设启动报错）；网站用密钥对「用户名.过期时间戳」签名（HMAC-SHA256，hex），凭证格式 `{exp}.{sig}`，exp 为毫秒时间戳，±10 分钟有效窗口防重放 | 空（不启用） |
| `GEBAI_EXTERNAL_AUTH_URL` | 外部身份扩展点：HTTP 回调验证 URL（与 `GEBAI_EXTERNAL_AUTH_SECRET` 互斥，同设启动报错；**必须 HTTPS**，localhost/127.0.0.1 例外防中间人伪造）；GEBAI 把 `{username, credential}` POST 给回调（5s 超时），业务系统自行校验（如查自己 localStorage 对应的服务端态），**必须核验 username 与凭证归属一致**，响应 2xx 且 `{"ok":true}` 即通过，可用 `username` 字段覆盖映射（仅应在明确校验后使用） | 空（不启用） |
| `GEBAI_EXTERNAL_AUTH_AUTOCREATE` | 外部用户名不存在时自动创建 GEBAI 用户（普通角色、随机密码不可密码登录）；`false` 时仅允许管理员预建的同名用户 | `true` |
| `GEBAI_EXTERNAL_AUTH_STORAGE_KEY` | Web UI 同源直读宿主 localStorage 的凭证 key（值支持 JSON `{"username","credential"}` 或 `"username:credential"` 字符串）；不设则仅支持 URL 参数注入 | 空 |
| `--server` | 开启服务模式（等价 `GEBAI_MODE=server`，参数优先） | - |
| `--no-webview` | 强制不启动桌面 WebView | - |

> 以上为**全局层**环境变量（进程注入），会话层可覆盖其中可运行时变更的项（**模型/Provider 配置全量可覆盖**：`GEBAI_LLM_MODEL`/`API_BASE`/`API_KEY`/`API_KIND`/`MAX_CONTEXT`/`MULTIMODAL` 与 `GEBAI_VISION_*` 任务级生效，按任务重建 Provider，见「环境变量配置」）。

### 服务端
- 服务端通过 WebSocket 与客户端通信，Agent 能力（Chat、工具调用、审批等）均在服务端内部实现
- **单端口暴露**：静态 Web UI、WebSocket（`/ws`）、REST API（`/api/*`）全部由同一端口承载，无多端口部署需求
- 对外提供双通道 API：**WebSocket**（实时双向流式）与 **REST HTTP**（同步请求，业务系统集成）
- 服务端内置 Web UI，通过浏览器可直接访问使用
- LLM 交互、子Agent管理、工具执行等逻辑全部封装在服务端内部
- 模型配置通过环境变量管理（`GEBAI_*` / `OPENAI_*`）；**服务端可不配置任何模型变量**——前端「设置 → 环境变量」配置（浏览器本地、不落盘），随每次发送消息临时注入，任务启动按合并后 env 重建 Provider（见「环境变量配置」）；未配置接口地址时 Provider 抛配置错误（`LLMConfigError`，不重试直接失败），错误信息给出前端配置入口指引
- 支持多种 LLM Provider：支持 OpenAI 兼容 `chat/completions`、OpenAI Responses（`/responses`）与 Anthropic `messages` 三类接口（均流式），通过统一的 `provider.chat()` 抽象封装，自行解析 SSE 流，不依赖第三方 AI SDK
- **额外模型接口参数**：支持自定义非标准请求体参数（如 `reasoning_effort` 推理强度、`temperature`、`thinking` 等），来源两级——Provider 级（`GEBAI_LLM_EXTRA_PARAMS` 环境变量，启动解析失败即报错）+ 任务级（浏览器本地注入同名环境变量，非法 JSON 静默忽略），后者优先，均顶层合并进请求体
- **任务级模型覆盖**：主模型与视觉模型配置（`GEBAI_LLM_*` 全套与 `GEBAI_VISION_*`）支持会话/任务级覆盖——启动时以进程环境变量固化基准配置（`core/llm.ts` 的 `applyModelEnvOverrides`/`resolveVisionProvider`，index.ts 组装 `AgentEngineOptions.resolveProvider` 与 env 感知的视觉 getter），每次任务启动按合并后 env 解析 Provider（无覆盖键时沿用启动实例零开销）；覆盖项含模型名、接口地址、密钥、接口类型、上下文预算、多模态声明，未覆盖项继承启动配置；作用域覆盖主循环（含上下文压缩阈值/摘要与附件内联判定——**主动压缩（UI/REST 入口）未显式指定 Provider 时同样按合并后 env 解析**）与 `agent_run` 新会话执行
- 多模态：`provider.chat()` 统一承载文本 + 图片/音视频消息，按 Provider/模型能力自动组装各自消息格式（统一内部图片块 `{type:"image", mime, data}` → OpenAI 系（chat/completions 与 Responses）`image_url` data URL / Anthropic base64 `image` 块）
- **额外多模态（视觉）模型**：支持独立配置视觉模型（`GEBAI_VISION_*`，缺省继承主模型接口），供全局工具 `vision` 将图片文件交给视觉模型分析（目标 + 图片文件参数）；未配置时 `vision` 回落到主模型（须声明多模态能力）
- **能力声明**：`LLMProvider.capabilities()` 返回 `{ streaming, toolCalling, multimodal, maxContextTokens }`，Agent 引擎据此决定：是否启用工具循环、附件降级策略、上下文占用判定（压缩阈值触发）
- 通过 `GEBAI_MODE`（默认 `local`；兼容旧 `GEBAI_AUTH`，或 CLI `--server`）环境变量切换运行形态：
  - **本地模式**（默认）：无需登录，直接以 **admin 用户**身份工作（**管理员超级权限 + 路径沙箱豁免**，不受任何权限限制），数据仍按用户目录存储
  - **服务模式**：启用登录鉴权（用户名/密码），密码仅存**加盐哈希**（scrypt，不落明文），会话按用户隔离，多用户公用同一服务端；**开放注册**（注册用户恒为普通角色）；**admin 为特权用户**（不受用户权限限制），**唯一入口是 `GEBAI_ADMIN_PASSWORD_HASH`**（未配置时 admin 禁用，但普通用户可注册使用）
- 业务系统集成统一走**账号密码认证**：`POST /api/v1/auth/login` 获取令牌后以 `Authorization: Bearer <token>` 调用 REST；或**单次请求直接带 HTTP Basic**（`Authorization: Basic base64(username:password)`，等价隐式登录，复用密码校验与登录限流、不签发令牌）；WS 用 `auth.login { token }` 建立用户上下文——**无独立服务令牌**（原 `GEBAI_SERVICE_API_KEY` 服务身份机制已移除，避免任何服务端密钥进入 Agent 可达环境）

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
| `AgentEngine` | 主循环状态机：工具循环/审批/重试/压缩/取消；**模型调用健壮性**：空响应（无文本且无工具调用，含只思考未输出）与无产出异常经 `callModel` 指数退避重试（2 次，800ms 基数，注入提示引导），已有产出后断流不重试（避免重复输出），耗尽抛中文错误；**重试过程前端可见**——每次将重试的模型服务异常推送 `event.model.error`（非终态瞬时提示，见「事件清单」）；**重复检测**：最近 8 次工具调用签名（工具名+参数 JSON）滚动窗口，相同签名第 3 次起中断执行并注入引导提示，中断超 2 次终止工具循环（避免模型无效重复）；待办续做回复与上轮完全相同（纯文本）时追加防复述提示；**会话级已读文件追踪**（fileGuard，write 防误覆盖守卫，见「write 防误覆盖守卫」） | `run(session, prompt, opts)`、`cancel(sessionId)` |
| `ToolRegistry` | 工具注册/命名空间解析/启停/审批声明 | `register(tool)`、`resolve(name)`、`list()` |
| `SessionStore` | 会话/消息/待办/附件持久化（分片路径） | `load(id)`、`save(session)`、`appendMessage()` |
| `EnvManager` | 环境变量合并：全局（进程 env）+ 会话内存态（不落盘）；用户环境变量只存浏览器本地、随 prompt 任务级注入 | `resolve(sessionId): Record<string,string>` |
| `AuthService` | 用户认证、令牌、归属校验、API Key | `login()`、`authorize(ws/api)` |
| `Sandbox` | 路径沙箱、子进程执行、超时控制（支持取消信号：abort 按进程树终止——Unix 进程组 SIGKILL、Windows `taskkill /T`） | `resolvePath(user, path)`、`exec(cmd, opts)` |
| `EventBus` | 服务端事件分发（WS/Webhook/飞书） | `publish(event)`、`subscribe()` |
| `Compressor` | 上下文压缩与截断落盘 | `compact(session, scope)` |

### 核心Agent流程

单次消息处理的主循环（Agent Loop），状态机驱动、全程可观测：

```
session.prompt → 组装上下文（历史+系统提示词+临时文件提示）
    → LLM 流式生成 → 输出文本增量实时推送
    → 模型请求工具调用?
        ├─ 否 → 生成完成，回复落库
        └─ 是 → 解析工具调用
            → 审批检查（需审批则暂停等待用户审批；拒绝→停止当前会话生成；超时自动拒绝并提示模型调整，最多 10 次）
            → 执行工具（可并行/管道）→ 结果注入上下文
            → 循环回 LLM 生成（直至无工具调用或达轮次上限）
```

关键机制：

- **工具调用循环**：一次 `session.prompt` 内模型可多轮调用工具（轮次上限见常量参考），每轮结果作为消息追加回 LLM，直至生成最终回复
- **流式体验**：LLM 文本增量、工具调用事件、审批请求、执行结果均以服务端事件实时推送客户端（见「事件推送」），UI 呈现完整过程
- **中断与取消**：客户端可随时取消当前任务，服务端停止本轮循环并保留已生成内容；**停止即时打断进行中的工具执行**（工具执行包装统一收口：任务取消信号传递到 `Sandbox.exec`，脚本类子进程按进程树终止——Unix 杀进程组、Windows `taskkill /T`，结果标记 `[interrupted by user]`；非脚本工具立即返回「已取消」结果落盘）；**取消统一解开全部挂起等待**（审批/选择/画图/捕获的等待 promise 同步 resolve——仅 abort 信号不会中断 `await`，不 resolve 会让 runLoop 永久挂起、任务收尾不完成、`isRunning` 残留，下一次 prompt 被「task already running」拒绝，表现为**中断后要发两次才能继续**；四个等待函数同时监听取消/超时信号，子Agent 超时同样立即解开）；**审批拒绝同样停止会话生成**（不再让模型调整方案继续执行，前端静默收尾并清理该会话残留审批卡片，取消/拒绝不渲染错误气泡）
- **重复检测**：模型连续/交替重复调用相同工具（工具名+参数相同，最近 8 次内第 3 次）时中断该次执行并注入引导提示（「不要重复相同操作，改用其他方法或直接给出最终回答」）；中断超过 2 次判定模型陷入重复，终止工具循环提前返回（仍返回最后产出文本，保持会话消息序列完整）；待办续做中模型回复与上上轮完全相同（纯文本）时，续做提醒追加「请勿复述」提示
- **错误恢复**：LLM 请求失败/超时自动退避重试；工具执行失败将错误注入上下文，引导模型自行修正；**工具执行超时不结束任务**——脚本类工具先由脚本执行超时（`sh`/`py` 可经 `timeout` 参数（秒，默认 300、上限 540）调整个别执行超时，缺省 5 分钟）杀进程并返回超时结果，引擎层 9 分钟兜底（`TOOL_TIMEOUT_MS`）覆盖不响应超时的工具（如网络请求挂起），超时均作为工具结果返回给模型自行调整方案
- **防盲覆盖守卫**：`write` 覆盖「已存在但本会话未读过」的文件被拒绝（会话级 fileGuard 已读追踪，`read`/`edit`/`patch`/`write` 成功即登记），引导模型先 `read` 再覆盖——拒绝作为工具结果返回，模型一轮自愈，不中断任务（见「write 防误覆盖守卫」）
- **上下文管理**：历史超出窗口阈值时自动压缩（工具输出截断 → 旧消息摘要 → 滚动裁剪，见「上下文保护」），也支持用户主动压缩
- **审批超时**：审批请求超时（见常量参考）自动拒绝该次调用并提示模型调整，避免任务悬挂

### 会话输入队列（前端实现）

会话任务运行期间到达的用户输入**排队等待，当前任务结束（完成/取消/出错）后自动按序发送执行**，无需等待或放弃输入。队列为**前端本地状态**（`packages/web/src/queue.ts`，按会话隔离、纯内存——服务端无感知、无协议扩展，刷新页面/关闭浏览器后队列丢失）：

- **入队**：运行中提交（Enter/发送；运行中发送按钮带草稿时为箭头形态）不打断当前任务，输入进本会话排队条（不进消息流、不发 WS）；附件在入队时即上传会话 `tmp/`（队列条目保存引用，执行时直用）
- **自动执行（drain）**：任务流收尾（`consumeTaskStream` finally，覆盖完成/取消/出错）后由前端按序取队首发送下一条——与直接发送走同一 `sendPrompt` 任务流（用户消息在执行时渲染，`messageId` 入队时生成供撤回/反馈定位）；入队后也触发一次 drain（覆盖「运行恰在入队前结束」的竞态窗口），drain 幂等（会话运行中空转）
- **中断插入（Ctrl+Enter / 排队条 ⚡）**：新输入或已排队输入插队首并取消当前任务（`session.cancel`），当前任务收尾后立即执行队首；空闲时等同直接发送
- **编辑/撤回**：未执行的排队输入可改内容、可撤回（本地队列操作）；已执行的排队输入即普通用户消息（走消息撤回 `truncate`）
- **停止语义**：停止按钮只取消当前任务，队列继续（排队输入是独立意图，撤回由用户逐条控制）；要全部停止先撤回排队项
- **多会话**：队列按会话隔离（切走不丢、切回恢复，随会话删除清理）；后台会话的排队输入同样在其任务结束后自动执行（发送与流式渲染不依赖当前视图）

### 多模态支持

用户可在会话中发送或引用多模态内容（图片、音频、视频、文档），模型视能力理解处理：

- **消息附件**：`session.prompt` 支持携带附件（图片/音频/视频/PDF 等），附件先上传到会话 `tmp/`，消息中携带引用路径与 MIME 类型（`AttachmentRef.path` 为**会话根相对逻辑路径**，如 `tmp/foo.png`，模型/工具/前端统一按此解析；历史兼容绝对路径附件——沙箱拒绝解析时自动降级为文本说明）
- **附件 → 模型内容**（`AgentEngine.loadHistory` 重建）：图片附件（png/jpeg/gif/webp）且主模型声明多模态能力（`GEBAI_LLM_MULTIMODAL=true`）且 ≤8MB 时 base64 内联为统一 `image` 块（携带 `path/name/size` 元数据）；其余（非图片/超限/文件缺失/模型无多模态）降级为文本说明（路径 + MIME + 大小 + vision 工具指引），由模型决定用 `vision`/`read` 等工具处理
- **接口拒绝自动降级**：主模型声明多模态但接口实际拒绝图片块（HTTP 4xx）时，引擎将图片块一次性降级为文本说明后重试（模型可改走 `vision` 工具），实现「无多模态能力自动降级」兜底
- **图片自动压缩**：Web 端上传前 canvas 重采样（长边 ≤1280px、体积 ≤2MB，JPEG 质量 0.85）；PNG 优先保留透明通道（仅当缩放后体积仍超限才转 JPEG，白底兜底），转 JPEG 时同步修正扩展名（`vision` 工具按扩展名判 MIME）；GIF 跳过
- **多模态消息格式**：统一内部消息模型（文本 + 内容块数组），`provider.chat()` 按接口规范转换：
  - OpenAI 兼容：`content` 为 `[{ type: "text" | "image_url", ... }]` 结构
  - Anthropic：`content` 为 `[{ type: "text" | "image" | "document", ... }]` 结构
- **图片**：支持本地上传/粘贴/URL 引用，传输时自动压缩重采样（尺寸与大小上限，见常量参考），按 Provider 要求编码（base64 data URL 或外链）
- **音频/视频**：上传到会话 `tmp/` 并转为可引用文件；Provider 不支持直接理解时降级为「附文件路径 + 文件名」文本提示（由工具/脚本代处理，如转写）
- **文档（PDF/Office）**：上传后可经工具（如 `read`/脚本）抽取文本喂给模型，或按 Provider 原生 `document` 块传递（能力自动探测）
- **能力探测**：模型声明 `multimodal` 能力（`GEBAI_LLM_MULTIMODAL=true` 显式声明，默认 false），无多模态能力的模型收到附件时提示用户或自动降级（文本说明 + `vision` 工具指引）
- **UI**：消息输入支持拖拽/粘贴/选择附件，预览缩略图，附件随消息一并持久化（会话目录内）
- **工具产物**：Agent 生成的图片/文件同样可回传到会话供查看与下载（见「会话临时文件查看与下载」）

#### 视觉工具 `vision`

全局工具（主 Agent），把会话内图片文件交给多模态（视觉）模型分析（适合主模型无多模态能力或需要更强视觉模型时）；子Agent 定义（如 `self_optimize`）可经 `makeVisionTool` + 组装层 provider 注册点（`setVisionProviderGetter`）复用同一解析逻辑：

- **参数**：`target`（目标：要查看/识别/描述的内容，必填）+ `image`（图片文件路径，相对会话工作目录——`tmp/` 前缀可省略——或绝对路径，受路径沙箱约束，支持 png/jpg/jpeg/gif/webp）
- **模型选择**：配置 `GEBAI_VISION_MODEL` 后使用独立视觉 Provider（`GEBAI_VISION_*`，接口地址/密钥/类型缺省继承主模型）；未配置时回落到主模型（须显式声明多模态能力，`GEBAI_LLM_MULTIMODAL=true`，默认 false），两者皆不可用则返回配置提示
- **传输**：图片以 base64 内联（统一内部图片块 → OpenAI `image_url` data URL / Anthropic base64 `image` 块）随文本目标一起调用视觉模型，单图上限见常量参考；返回分析文本（超长走截断保护），并携带 `image` 内容块供 UI 展示图片
- **审批**：默认无需审批（纯读取/外部模型调用）

### 多用户隔离与安全

服务模式下，所有数据与执行环境按 **用户 → 会话** 两级隔离：

#### 认证与鉴权
- **登录鉴权**：服务模式启用登录（用户名/密码），密码使用加盐哈希存储（scrypt，不落明文）
- **开放注册（可审批）**：服务模式登录页提供「注册账号」入口（`POST /api/v1/auth/register`，公开端点）；**注册用户恒为普通角色**（不可注册 admin，admin 唯一入口是启动参数 `GEBAI_ADMIN_PASSWORD_HASH`）；**用户名经 `normalizeUsername` 规范化**：小写折叠（Windows 文件系统不区分大小写，`Alice` 与 `alice` 不得形成两个注册表键共享同一物理目录）+ 格式白名单 `[a-z0-9][a-z0-9_-]*`（1-32 字符，拒绝 `..`/`.`/路径分隔符/控制字符防用户目录穿越）+ 拒绝 Windows 保留设备名（con/nul/com1 等）；重名/非法返回 400；本地模式不开放（404）。**审批模式由 `GEBAI_SIGNUP_MODE` 控制**：`open`（默认）=注册即登录（签发令牌）；`approval`=注册用户置 `disabled+pending` 待审（不签发令牌、不可登录），登录页提示「等待管理员审批」，admin 在用户管理页**批准**（`PATCH {disabled:false, pending:false}` 启用）或**拒绝**（删除用户）
- **外部身份扩展点**：服务模式下支持「外部身份 → GEBAI 令牌」兑换（`POST /api/v1/auth/exchange`），同源部署网站可直接复用其本地登录态（localStorage）作为 GEBAI 用户，免二次登录。验证器可插拔（`external-auth.ts`）：配置 `GEBAI_EXTERNAL_AUTH_SECRET` 走 HMAC 验签（凭证 `{exp}.{sig}`，±10 分钟窗口 + **一次性消费**——签名无 nonce，已消费凭证摘要缓存防窗口内重放），配置 `GEBAI_EXTERNAL_AUTH_URL` 走 HTTP 回调（业务系统自行校验凭证真实性；**回调返回的用户名必须与请求一致**——允许规范化去空白，拒绝不一致，防宽松回调被利用接管任意用户）；两者互斥，验证失败统一 401 不泄露原因。**外部身份禁止命中本地 admin 账号**（外部系统同名 `admin` 用户兑换不得继承本地 admin 角色——admin 唯一入口是启动参数口令）。用户映射：`GEBAI_EXTERNAL_AUTH_AUTOCREATE=true`（默认）自动创建普通角色用户（随机密码不可密码登录），`false` 仅允许已存在用户；外部用户名经 `normalizeUsername` 规范化（同上：小写折叠 + 白名单 + 保留名拒绝）。前端注入：Web UI 启动时若本地无令牌，先取 URL 参数 `?gb_ext_username=&gb_ext_credential=`，再按 `GEBAI_EXTERNAL_AUTH_STORAGE_KEY` 直读宿主 localStorage（JSON 或 `username:credential` 字符串）；`GET /api/v1/auth/external-config` 供前端探测启用状态（不泄露密钥）
- **登录限流**：连续失败 5 次锁定该用户名 60 秒（内存计数），防在线爆破；**登录/兑换/注册端点另加令牌桶**（REST：登录/兑换全局桶 60 突发/2 每秒 + 来源桶 10 突发/0.2 每秒，注册独立桶 30/0.5 + 10/0.1；WS `auth.login` 密码路径同款桶 30/1——scrypt 即使异步化仍耗 CPU，轮换用户名即可绕过按用户名锁定，防 CPU DoS 放大器；`GEBAI_TRUST_PROXY=true` 时 REST 按 `X-Forwarded-For` 首段区分来源）
- **令牌机制**：登录后签发会话令牌（HMAC 签名，7 天 TTL），后续 WebSocket 连接携带令牌建立用户上下文；**令牌表持久化到 `{GEBAI_HOME}/auth-tokens.json`**（签发/撤销/过期清理时落盘，进程重启后已签发令牌仍有效——单机部署下重启不掉线；过期令牌在 authorize/保存时顺带清理，不无界增长）
- **WS 未登录拦截**：服务模式下未登录（无令牌）的 WS 连接仅允许 `auth.login`，其余消息一律拒绝
- **跨站来源防护（本地/桌面免登录形态）**：WebSocket 不受同源策略约束且本地模式免登录——恶意网页可直连 `ws://127.0.0.1:*` 以 admin 身份建会话执行命令（REST 通道因 CORS `*` 同样暴露）。防护：WS upgrade 与 REST `/api/*` 均校验 **Origin 与 Host 同源**（浏览器发起的跨站请求必带 Origin，不同源即 403；非浏览器客户端无 Origin 不受限；显式配置 `GEBAI_CORS_ORIGINS` 视为有意开放，仅缺省 `*` 且本地模式时拦截）；服务模式有令牌鉴权豁免
- **WS 全局子Agent 装载/卸载管理员门槛**：`sub_agent.load`/`sub_agent.unload` 不带 `sessionId` 的**全局形态**（变更所有用户的工具注册面）服务模式下仅 admin（与 REST 工具启停同门槛）；带 `sessionId` 的会话级装载/卸载不受限（只影响本人会话）；模型侧 `agent_load` 装载进当前会话（会话级引用，见「子Agent」引用计数）
- **用户管理**：支持管理员创建/禁用用户；每个用户独立命名空间（`users/{user}/`）；服务模式 admin 用户通过启动参数 `GEBAI_ADMIN_PASSWORD_HASH` 引导（**设置则启用并覆盖其密码哈希，不设置则禁用**，启动参数为权威配置每次启动重置；admin 被禁用时**普通用户可经注册页自助注册使用**（普通角色），管理员能力须部署方设置哈希启用）；**REST 与 WS 双通道的用户管理端点均校验管理员角色**（非管理员一律 403，防普通用户提权/越权管理）
- **请求校验**：所有会话操作先解析令牌确定用户，再校验会话归属（含取消任务、审批决策等控制类操作）；**会话 ID 格式白名单**（32 位小写 hex，`randomUUID` 去连字符）在存储层 `sessionPath`/`store` 与 REST 中间件、WS 消息入口四层强制——畸形/穿越形态（`../`、路径分隔符）一律 400/错误应答，从根上杜绝会话 ID 拼路径形成的目录穿越
- **附件安全**：上传/引用的附件文件名仅取 basename 并拒绝路径穿越；沙箱约束用户附件来源路径限定会话目录内（防任意文件读取与越界写入），豁免用户（admin，特权用户）不限制来源

#### 数据与执行隔离
- **数据隔离**：会话、临时文件、截断内容均存储于 `{GEBAI_HOME}/users/{user}/` 下，用户之间互不可见
- **会话归属校验**：任何会话操作（读取、切换、发送、删除）均校验当前登录用户与会话所有者一致，禁止跨用户访问
- **路径沙箱**：服务端部署模式下，文件类工具（`read`/`write` 等）的相对路径统一以当前会话 `tmp/` 为基准（见「路径基准」），仅允许访问该目录内路径，对目录外路径一律拒绝，防止路径穿越（`../`、绝对路径、符号链接）；**桌面/本地浏览器模式默认不启用目录限制**，可访问本机任意路径（由 `GEBAI_SANDBOX` 控制，默认按运行形态自动选择）；**仅本地模式默认用户（id=admin）豁免路径沙箱**（`Sandbox.isExempt`/`enforcedFor(user)` 按用户判定：本地模式是操作者本人机器、豁免用户即使沙箱启用也按本地模式放开；**服务模式一律沙箱**——admin 也不豁免，多租户边界一致）；**`GEBAI_SANDBOX=off` 与服务模式互斥**（启动即拒绝：多租户 + 关沙箱会让 files 接口回退裸 `resolve` 可 `../` 越界读任意路径，安全配置错误在启动期暴露）
- **脚本隔离**：`sh`/`py`/`js` 脚本以独立子进程运行，工作目录限定为当前会话 `tmp/`，环境变量注入当前用户上下文；**沙箱（服务端部署）模式与安全模式下脚本子进程环境剔除敏感变量**（按完整结尾单词匹配 `_KEY`/`_TOKEN`/`_SECRET`/`_HASH`/`_KEY_ID`/`_CREDENTIAL`/`_AUTH`/`DATABASE_URL` 及裸名 `TOKEN`/`SECRET`/`PASSWORD` 等形态，防止任意用户经脚本 `env`/读取外泄服务端全局密钥，也防安全模式下 js 读 `process.env` 拿到密钥；**豁免用户（admin，特权用户）不剔除**，本地模式不剔除（安全模式仍剔除）；`js` 的会话上下文注入 `ctx.env` 同规则脱敏）；JS/TS 脚本优先通过**内置运行时自执行**（见「脚本执行环境」），不依赖宿主机安装 bun/node
- **桌面控制隔离**：`desktop`（截图/窗口控制/键盘鼠标输入）是对宿主机桌面的真实操作，**仅本地/桌面模式或沙箱豁免用户可用**——服务端部署（沙箱约束用户）下全部工具一律拒绝执行；输入/点击/窗口控制类工具默认需审批，防远程滥用与误操作。部署警示：**服务模式（`GEBAI_SANDBOX=auto` 时）自动强制启用沙箱**，阻断远程桌面操控（admin 豁免除外）
- **浏览器隔离**：`playwright`（无头浏览器自动化）运行在**隔离的浏览器环境**（独立 Chromium 进程，不触宿主机桌面/文件系统），服务端部署可用；但导航/交互/脚本类工具（`open`/`click`/`fill`/`press`/`select`/`check`/`evaluate`/`new_page`）默认需审批——防远程用户借服务端浏览器探测内网（SSRF）、提交表单或执行任意页面脚本；浏览器上下文按会话隔离，`evaluate` 可读取页面内数据（含表单值/cookie），仅限审批后执行
- **Webhook SSRF 防护**：Webhook 注册默认拒绝回环/链路本地/云元数据地址（`localhost`、`127.*`、`169.254.*`、`::1`、`fe80:*` 及其 IPv4-mapped/尾点 FQDN 等绕过形式），需内网回调时以 `GEBAI_WEBHOOK_ALLOW_PRIVATE=true` 显式放开；**投递同样带逐跳重定向校验**（复用 `fetchWithRedirectGuard`，每跳 Location 重新过 `checkWebhookUrl`，防「注册公网 URL → 302 内网/元数据」跳板绕过注册期校验，与 `fetch_url`/`http_request` 同口径）
- **公网访问守卫（fetch_url/http_request）**：沙箱约束用户（服务端部署模式）仅允许公网地址——拒绝回环/链路本地/私网（RFC1918）与 **ULA**（`fc00::/7`）；主机名判定统一走 `core/ip.ts`，覆盖常见绕过形式：**IPv4-mapped IPv6**（`[::ffff:127.0.0.1]` 及完整形式）、IPv4-compatible IPv6、**整数/十六进制/八进制 IPv4**（`2130706433`/`0x7f000001`/`0177.0.0.1`，WHATWG URL 已规范化为点分十进制）、**尾点 FQDN**（`localhost.`）；`fetch_url` 与 `http_request`（默认路径）另带**重定向逐跳校验**（`redirect: "manual"` 手动跟随，每跳 Location 重新执行公网校验，跳数上限 5），防「初始公网 → 302 内网」跳板绕过；**域名做 DNS 解析复查**（`assertPublicHttpUrl` 异步解析主机名，任一解析地址命中私网/回环即拒绝——覆盖内网 DNS 名（`kubernetes.default.svc` 等）与「公网域名 A 记录指向内网」的重绑定形态；解析失败/3s 超时放行由 fetch 层超时兜底）；**豁免用户（admin）不限制私网访问**
- **环境变量隔离**：**用户环境变量服务端零留存**（只存浏览器本地 localStorage，服务端不落任何 env 文件；会话内存态 env 不落盘、重启即空），用户间互不可见；`{AGENT_NAME_UPPER}_*` 前缀为**命名约定与前端目录白名单口径**（envVars 声明汇总进环境变量面板），运行时硬边界为脚本子进程/`js` ctx 的**敏感变量剔除**（见「脚本隔离」——敏感判定按完整结尾单词匹配：`*_KEY`/`*_TOKEN`/`*_SECRET`/`*_HASH`/`*_KEY_ID`/`*_CREDENTIAL`/`DATABASE_URL`/`CONNECTION_STRING` 及裸名 `TOKEN`/`SECRET`/`PASSWORD` 等前后缀/裸名形态，防 `GEBAI_ADMIN_PASSWORD_HASH`/`AWS_ACCESS_KEY_ID` 类漏判），敏感变量脱敏显示
- **子Agent共享**：子Agent 为服务端内置代码，构建时编译进二进制，全局共享、只读、无用户差异
- **审批隔离**：`/approval-skip` 为会话级设置，仅作用于当前会话（会话内存态 env，不落盘），不影响其他用户；**用户本人可设置自己的会话**（前端开关、REST `PUT /env`、WS `session.env.set`、飞书 `/approval-skip` 命令——写入只影响本人会话，非管理员仍受路径/脚本/网络沙箱完整约束）；**ask_env 填值通道服务模式下一律拒绝 `GEBAI_APPROVAL_SKIP`**——模型驱动的写入不得自设审批跳过（防提示词注入诱导），本地/单用户模式不受限
- **安全模式（`GEBAI_SAFE_MODE`）**：部署方可在启动时开启降级运行形态（**仅启动时从 .env/环境变量加载，不进会话/任务级 env，`ask_env` 与前端本地 env 注入均无法修改**）——原则是**风险能力降级而非一刀切禁用**，各风险工具在自身 execute 内降级：
  | 能力 | 安全模式降级形态 |
  |------|------------------|
  | `sh` | 只读命令白名单（`cat`/`head`/`tail`/`grep`/`find`/`ls`/`git` 读子命令/`sort`/`diff` 等查看与文本处理类；`sed`/`awk` 有脚本内写/执行通道、`less`/`more` 有 `!` shell 逃逸，均不入列）；命令解析 fail-closed（反引号/后台执行 `&`/进程替换 `<(cmd)`/未闭合引号等无法识别的结构一律拒绝）；输出重定向 `>`/`>>`/`2>`/`&>` 目标须在**安全写范围**内（`/dev/null`/`NUL` 放行），fd 复制 `2>&1` 放行；`$(...)` 命令替换与双引号内替换按替换语义**递归校验**（POSIX 双引号内 `$()` 与反引号会执行，单引号为字面量）；`find` 禁 `-exec`/`-delete` 等执行/写动作、`sort` 禁 `-o`/`--output`、`git` 仅读子命令（`status`/`log`/`diff`/`show`/`blame`/`rev-parse`/`ls-files`/`describe` 等）、`env` 仅单独执行、`date` 禁 `-s`/`--set`、`hostname` 仅 flag 参数 |
  | `py` | 子进程内 `sys.addaudithook` 审计钩子（`core/safety.ts` `PY_SAFE_BOOTSTRAP`）：写模式 `open`（mode 含 `w`/`a`/`x`/`+` 或 flags 含 `O_WRONLY`/`O_RDWR`/`O_CREAT`/`O_TRUNC`/`O_APPEND`）、进程（`os.system`/`subprocess.*`/`os.posix_spawn`/`os.fork`）、网络（`socket.connect`/`bind`）、文件变更（`os.remove`/`rename`/`mkdir`/`chmod` 等、`shutil.*`）、`ctypes.dlopen`（防绕过钩子的裸系统调用）、`sqlite3.connect` 全部拒绝；钩子堆叠不可移除，**仅保留文件读取** |
  | `js` | 双层降级：静态扫描（`scanJsReadOnly`）**按词元级拒绝**（而非调用形态）——`import`/`require` 任意出现即拒（静态 import 语句提升先于 shim 执行、`const rq = require; rq(...)` 别名形态调用正则拦不住）、`Bun` 仅放行 `Bun.file`（`Bun["fetch"]` 括号访问可躲点号正则，且 fetch/sqlite 为不可覆写 getter、Bun 全局 non-configurable 无法运行时代理，扫描是唯一防线）、`getBuiltinModule` 词元拒绝；运行时 shim（注入子进程）屏蔽其余——`Bun` 对象属性覆写为抛错桩（`write`/`spawn`/`serve`/`$`/`sql` 等）、`Bun.file` 包装拦截 `write`/`writer`/`sink`/`truncate`（读方法照常）、删除 `eval`/`Function`/`fetch`/`WebSocket`/`Worker` 全局、`Function.prototype.constructor` 中性化防 `(fn).constructor` 回收、`process` 仅拦 `binding`/`dlopen`/`getBuiltinModule`/`kill`（桥协议依赖 stdin/stdout/exit）、字符串定时器拒绝、`Reflect.get` 作用于 Bun 对象时拒绝（防反射读取绕过属性覆写）、**模块级 `var require` 中和**（CJS 参数遮蔽/ESM 定义绑定，别名引用拿到拒绝桩）——**仅保留文件读取**；子进程环境同样剔除敏感变量（安全模式下 `process.env` 不暴露密钥）；写文件用 `write` 工具、网络用 `fetch_url` 工具（RPC 调用各工具按其降级规则执行） |
  | `write`/`edit`/`patch`/`file` | 限定**安全写范围**内（`safeModeWriteCheck`）：沙箱模式=用户数据根（`users/{user}`）；本地模式=OS 用户主目录 + `GEBAI_HOME` + 会话工作目录（Windows 大小写不敏感比较）。越界拒绝并提示，范围内照常 |
  | 动态工具（`js` defineTool） | 与 `js` 同规则降级（execute 源码静态扫描 + 子进程只读 shim），注册与水合**不再跳过**——只读动态工具（数据处理/查询类）安全模式下保持可用 |
  | `cron_add`/`cron_update`/`cron_remove` | **维持硬阻断**（定时任务延迟触发任意执行，无法降级）：引擎主/子循环、`flow` step 层、`js` RPC 分发层按 `isToolBlockedInSafeMode` 同规则拦截，模型调用时直接返回限制信息（不执行、不弹审批） |
  | 子Agent 工具 | **自主声明 `Tool.safeMode`**：`true`=作者判定安全模式下可提供（即使短名风险如 `{agent}_sh`，须自行保证实现只读或体内按 `ctx.safeMode` 校验）；`false`=判定不提供（即使名字无风险）；未声明=按短名风险规则默认（`isRiskyToolName`：`{agent}_sh`/`{agent}_write`/`{agent}_file`/`{agent}_delete`/`{agent}_cron_add` 等 `_risk` 后缀命中则不注册）。注册期过滤（`ToolRegistry({safeMode})`，主注册表与 agent_run 新会话注册表同规则）——不注册即 schema 不可见、调用报未知工具 |
  **安全写范围**与**降级说明注入**：系统提示词（主循环与 agent_run 新会话）在安全模式下追加降级能力说明（模型知晓能力边界）；已创建的 script 型定时任务触发时跳过（落盘提示、不执行 shell，`nextRunAt` 正常推进）；审批姿态不变（各工具原 `requiresApproval` 规则照常生效）
- **限流保护**：按用户限制并发任务数与消息速率，防止资源滥用；单用户单会话同时仅一个任务运行；**每用户 prompt 令牌桶限流**（REST `POST /sessions/:id/prompt` 与 WS `session.prompt` 同规则：容量 60 突发、30/秒补充，超限返回 429 / error reply）

#### 安全与稳定性强化批次（2026-08，全量审查修复）

除上述各节就地更新的语义外，本批次还包含以下工程级强化（均已落地并带回归测试）：

- **持久化原子性**：`chat.json` 写入改为 tmp+rename 原子替换（崩溃/断电/磁盘满不再留下截断 JSON 致会话静默丢失；Windows 上目标被并发读取时 rename EPERM 回退直接覆写——原子性降级为最佳努力，不因竞态中断任务）
- **任务并发 TOCTOU 防护**：`engine.run()` 检查与注册之间的 await 间隙可让并发请求（WS 重复帧/REST 与 WS 双通道）双双通过检查造成同会话双任务——改为**同步占位再异步校验**（校验失败同步回滚），入口层 `isRunning` 检查不再有竞态窗口
- **会话缓存 LRU 化**：`store.load()` 命中刷新位置（运行中会话不因其他会话活跃被挤出 10 条缓存）；任务流程内的持久化调用（`appendMessage`/`getTodos`/`setTodos`）带归属用户——挤出后仍可从磁盘确定性装载（此前挤出后无 user 的装载失败会中断运行中任务）
- **审批授权竞态（飞书群聊）**：`runOwners` 绑定条件写入（不覆盖既有绑定）+ 解绑校验「自己仍是绑定者」——并发消息竞态下后来者覆盖/误删发起者绑定会使审批 `openId` 变空、跳过「仅发起者可操作」校验（任意群成员可批准高危操作）
- **飞书长连接韧性**：pong 超时检测（连续 3 个 ping 周期无 pong 主动断开走重连——NAT 超时/半开 TCP 下 onclose 不触发、bot 永久失联）；重连达服务端上限后**降频续试**（60s 封顶，不再永久停摆）；事件按 `event_id` 去重（飞书重推防任务重复执行）；`finalSent` 每任务复位（空最终回复的任务在飞书侧得不到完成信号的缺陷）
- **feishu_docs token 管理**：user refresh_token 刷新**会话级单飞**（单次有效的 refresh_token 并发双发必有一个失败静默降级 tenant 身份）；tenant token 远端失效（99991661/99991663）逐出缓存重试一次（此前最长 2 小时持续失败）
- **子Agent 装载引用计数**：`SubAgentManager.load/unload` 按 owner（会话 id/agent_run 共享标记/全局）解引用——一个会话卸载子Agent 不再注销全局注册表砍掉其他会话正在使用的工具（装载状态按会话建模、注册表全局共享的矛盾以引用计数弥合）
- **SDK 连接韧性**：心跳判死阈值回归 `heartbeatTimeoutMs` 语义（此前下一拍间隔即杀，高延迟链路「连上→误杀→重连」循环）；断线批量 reject 在途请求时清超时 timer（不再二次 onError「WS 请求超时」误导）；建连期无 onerror 的 close 路径 settle 共享 promise（调用方不再永久挂起）；断线恢复后 seq 基线收敛（缺口后方无新事件窗口内不再重复重放旧区间——旧 `task.done` 截断新任务流）；`dispatchEvent` 按 seq 去重（多会话并发恢复同一批重放事件不再双份分发）；WS 附件 `Uint8Array` 序列化为数组（此前 JSON 化为类数组对象、服务端构造抛错）
- **Web 交互修复**：审批 Y/N 快捷键排除文本输入焦点（搜索/重命名框打字误批）；审批等待刷新前端看门狗活跃时间（服务端审批等 5 分钟、前端 150s 看门狗不再误杀）；审批卡按 toolCallId 去重（事件重放不堆叠）；切回会话时运行中工具卡重建过滤历史已完成项；断线全量重同步后子Agent 容器惰性重建（run 输出不再静默丢弃）
- **其他**：`file delete` 递归删除需审批（见「工具审批」）；附件重名自动加序号（同批两个 data.csv 不再静默覆盖）；analyzer 的 tree-sitter Tree 用毕显式释放（wasm 原生内存）；mini-tools 拒绝 Windows 保留设备名；`serve_dir` 静态服务 8 个上限（最旧淘汰）；desktop `screenshot` 的 `name` 参数落地（此前声明未实现）；`draw`/`render_html` 的 name 参数补 writeGuard/安全模式写检查（此前可越出子Agent 写范围）；SSE 解析兼容 CRLF/CR 行结尾与多行 data 按规范 join（部分网关 CRLF 行结尾整轮静默无输出）；三家 provider 的 done chunk 语义统一（恰好一次、断流补发）；SSE 消费方提前退出时 cancel 上游响应流（连接不悬挂）；vision 调用透传取消信号

### 总Agent
- 系统提示词中注入：**未装载**子Agent 的轻量引导列表（名称 + 描述），引导模型通过 `agent_load` 装载后使用（装载 = 工具注册 + 完整提示词写入会话记录）或直接 `agent_run` 执行新会话（无需装载）。**已装载子Agent 的完整系统提示词不注入总层提示词**——装载时已作为 system 消息写入会话记录（`loadedAgent` 标记，`loadHistory` 透传进模型上下文），此处再注入会双份占用上下文。**不展开工具 schema**——工具名已注册进工具集；但未装载清单的描述**附工具摘要**（`agentDescription` 追加「装载后工具：a、b、c…（以 {agent}_ 前缀调用）」，超出 10 个截断计数）——未装载的工具名不在 schema 里，摘要是装载前的路由匹配面（`agent_list` 同样不列工具 schema，描述同样携带摘要）
- 系统提示词内置**任务类型路由引导**：总层只给「装载 vs 新会话语义」机制说明 + 「按任务类型从下方可选子Agent 清单选用（描述即触发场景）」引导；**具体任务→子Agent 映射不硬编码**——由 `systemPromptInjection` 注入的未装载子Agent description（触发场景 + 职责边界）承载（code/self_optimize 互指边界：不处理歌白自身代码/不处理外部项目），新增子Agent 零改动自动进入路由信息（纯文本问答不装载）。**选用原则：默认 `agent_load` 装载（模块语义，工具与完整系统提示词写入当前会话记录、不创建独立执行）后直接用其工具**，仅在需要干净上下文（子任务结果隔离、不污染主上下文）或防止上下文膨胀（子任务中间过程多、输出大）时才用 `agent_run` 执行新会话（会话语义，**无需装载**：派生临时新会话，预加载一个或多个子Agent（完整系统提示词与工具）后阻塞执行，只返回最终结果）；执行过程子Agent 的模型回复、推理与工具调用全程实时推送前端
- 桌面/浏览器子Agent 系统提示词内置**验证多通道降级策略**：截图黑屏/失败时切换 DOM/content、窗口状态、数据文件等通道，任一失效立即降级并告知用户，不盲目重试单一通道
- **系统提示词中声明会话工作目录**（会话 `tmp/`，如 `{GEBAI_HOME}/users/{user}/sessions/{s0}/{s1}/{session_id}/tmp/`）并说明**所有文件工具的相对路径以此为基准（`tmp/` 前缀可省略）**；服务端部署模式下大模型读写限定在该目录，桌面/本地浏览器模式不限制目录（同路径沙箱规则）
- 系统提示词中引导模型：复杂操作应编写脚本（`sh`/`py`）一次执行，避免大量单步工具调用
- 系统提示词中引导模型：**重大任务（多步骤/有风险/不可逆/用户需要把关）先制定计划**——调用 `plan` 工具把计划文档写入会话文件并在界面展示，阻塞等待用户批准后再执行（被拒绝则按修改意见修订后重新提交 `plan`）；简单任务无需 `plan`，`todo` 跟踪即可
- 子Agent 装载后，系统提示词实时更新；**`self_optimize` 装载即连带装载 `code`**（`SubAgentManager.load` 幂等，WS `sub_agent.load`/`agent_load`/预加载所有装载路径均生效）；**`agent_run` 预加载 `self_optimize` 时同样连带预加载 `code`**（`runNewSession` 同规则——self_optimize 的 def 只声明独有工具，通用工具与工作流提示词由 code 提供，不重复注册，见「self_optimize」章节）
- **提示词分层职责（严格划分，防止职责越界与重复）**：
   - **总Agent 系统提示词**（`buildSystemPrompt`）只承载：身份、环境边界（工作区/沙箱）、执行策略、任务类型→子Agent 路由（紧凑映射 + 装载/新会话语义）、子Agent 能力注册表（`systemPromptInjection`：仅未装载轻量列表——已装载的完整提示词在会话记录的 `loadedAgent` 消息里）、`{AGENT}_PROJECT` 项目绑定声明（路由信息）。**不注入**子Agent 动态项目注记（预置项目清单、受限模式说明、项目根——属运行期上下文，随子Agent 提示词注入：`agent_run` 新会话执行与装载写会话记录均注入）；唯一例外：**子Agent 描述动态体现预置项目摘要**（未装载清单/`agent_list` 中，`{AGENT}_PROJECTS` 配置时描述附「预置项目：名称: 说明（路径）」摘要，方便总Agent 按项目名关联任务与代码位置，完整清单注记仍只在子Agent 提示词）
   - **子Agent 新会话系统提示词**（`runNewSession`）只承载：新会话隔离声明、各预加载子Agent 的完整系统提示词（自包含）+ 职责分隔头（`### {name}（{description}）`，多 Agent 预加载时明确各段提示词对应的工具命名空间与职责域）+ 动态项目上下文（项目根/工作目录、预置项目清单、受限状态、AGENTS.md）；**动态环境注记置于职责分隔头之后、静态提示词之前**（配置信息前置——模型开工先读环境注记确定目标项目与 project 参数，再读工作流）
  - **子Agent 静态系统提示词**只承载自身职责域：本 Agent 的工作流、行为约束、工具协作说明；**不复刻其他子Agent 的内容**（如 self_optimize 不复刻 code 的通用工作流——直接引用连带装载的 code 提示词，自身只写自我优化特有流程与约束），不写实现机制说明（如工具复用/连带装载——机制由代码承担，提示词只写模型行为指令）
  - **工具 schema 描述**自包含（装载/新会话形态通用，工具描述独立成立——模型选择工具的第一信息来源，如 `project` 参数直接说明 CODE_PROJECTS 清单；子Agent 完整提示词随装载写入会话记录，但工具描述仍须自成体系，不依赖提示词解释）
  - **单源去重（上下文经济性铁律）**：schema 每轮随请求全量重发、装载后提示词与工具描述同处一个上下文，同一条信息只允许一处权威副本，其余位置单向指针——参数级语义只写在参数 description（工具描述正文不罗列参数）；领域知识表内置于对应工具描述（如飞书块类型表在 `add_blocks` 描述，系统提示词只留指针）；安全模式降级说明单源于系统提示词（主循环与 `agent_run` 新会话同规则注入，工具描述不重复）；「装载 vs 新会话执行」选用原则单源于总Agent 路由段（`agent_load`/`agent_run` 描述只写机制，未装载清单头不解释用法）；子Agent 提示词写工作流取舍，不重述工具描述已有的参数级细节；「无需审批」类姿态说明对模型决策零价值，不写入描述（`sh`/`py` 的 `approval:false` 传参引导除外——模型需主动传参）

#### 子Agent 提示词编写规范（8 维强化检查清单）

新增/修改子Agent 系统提示词时按以下 8 个维度逐项检查（自我优化审查提示词同样适用）；内置子Agent 均已按此清单强化：

| 维度 | 检查项 |
|------|--------|
| 1. 身份与职责边界 | 开头一句话身份自述（「你是 xx 专家/助手」）+ 职责范围与边界（做什么/不做什么/运行形态限制如服务端拒绝） |
| 2. 工作流骨架 | 编号步骤：目标确认 → 探索/读取 → 方案 → 执行 → 验证 → 收尾反馈；多步骤任务用 `todo` 跟踪、重大/有风险任务先 `plan` 提交计划经用户批准后再执行、方向取舍用 `ask_user` 确认、前后对比用 `diff` 展示 |
| 3. 工具使用纪律 | 每个工具的触发场景与优先级；相似工具分工（如 `find_blocks` 定位 vs `get_doc_text` 读取）；常见失败规避——**先读后写**（修改前确认当前内容，防基于过期内容操作）、**等待优先**（wait_for 目标条件，不用固定 sleep）、失败分类定位不盲目重试 |
| 4. 安全与审批 | 审批意识（写/交互/导航类操作需审批，操作前先说明方案与影响范围）；敏感数据（密钥/凭据/token 不输出明文、自动脱敏）；权限边界（只操作授权范围） |
| 5. 验证多通道 | 不依赖单一验证通道（截图/内容读取/DOM/请求记录/直连探测）；任一通道失效立即切换并**明确告知用户当前采用的验证方式**，不盲目重试同一通道 |
| 6. 协作与编排 | `agent_run` 委托形态（委托谁、输入什么、期望产出——如 desktop 委托 code 读数据文件验证、playwright 被 code 委托做浏览器验证）；与 `self_optimize` 联动边界（仅 reverse_site 转交文档） |
| 7. 验证闭环 | 修改/操作后验证（测试/检查/断言）；失败先定位（读错误信息分类）再修，不盲目重复执行 |
| 8. 降级与诊断 | 错误分类 → 针对性处理（元素不存在/导航失败/鉴权失败/限频等）；回退/占位路径说明（如摘要失败降级滚动裁剪、OAuth 刷新失败回退应用身份） |

> 提示词只写**模型行为指令**，不写实现机制（工具内部如何实现由代码承担）；不复刻其他子Agent 的内容；保持精简——每个维度点到即止，防止提示词膨胀稀释注意力。

### 子Agent（扩展机制）

- 定义形式二选一：**单文件** `sub-agents/{name}.ts`，或**目录** `sub-agents/{name}/{name}.ts`（入口文件）+ 可选 `{name}.md`（系统提示词拆分维护）；作为服务端代码放在 `packages/server/src/sub-agents/` 下
- 目录形式下，系统提示词 md 由入口 ts 文件**导入并修饰**（见「子Agent文件格式」）：`import systemPrompt from "./{name}.md"`（Bun 原生文本导入，构建时随 ts 一起内联进产物）
- **零注册**：文件/目录即声明，运行时自动扫描收集（跳过 `*.test.ts` 与辅助文件，目录形式只认 `{dir}/{dir}.ts`），无需任何配置文件或代码注册
- **打包闭环**：`bun build` 前由 `scripts/build-subagents.ts` 扫描 `src/sub-agents/` 生成 bundle 注册表（`src/core/subagents.bundle.generated.ts`，gitignore），全部子Agent 定义（含 md 提示词）以静态 import 内联进产物；dist/二进制模式下源码目录不可用，`discover()` 自动回退到 bundle 注册表——子Agent 真正「打包进二进制」，运行时无需读取任何子Agent 文件。**bundle 注册表缺失或加载失败必抛错（fail-fast），绝不静默降级为空子Agent 列表**——启动「成功」但没有任何子Agent 比启动失败更难排查。**例外与配套**：playwright 子Agent 的 `driver.mjs`（node 桥接进程，须保持独立文件）由构建脚本复制到 `dist/` 与产物同目录，运行时按 `import.meta.dir` 定位（`--compile` 形态另经 `scripts/build-driver-embed.ts` 内嵌、物化到 `{GEBAI_HOME}/vendor/playwright/`）；playwright-core 包树经 `scripts/build-pwcore-embed.ts` 整树 gzip base64 内嵌（`pwcore.embedded.generated.json`，gitignore），运行时物化到 `{GEBAI_HOME}/vendor/playwright-core/`（见 playwright 子Agent「依赖与部署」）。**铁律：bundle 图内的子Agent 模块禁止模块作用域的第三方包解析**（`Bun.resolveSync`、裸 `import`/`require` 等）——编译产物中这类解析锚定真实 CWD 的 node_modules 可达性，启动期解析失败会炸掉整个 bundle 注册表（服务启动即退出）；此类依赖须延迟到首次工具调用（playwright 经 `createLazyBridge()` 惰性单例，解析失败降级为工具级运行时错误，不影响服务启动）
- **构建期裁剪与预加载指定**（环境变量，二进制形态无法改源码、构建时定死）：`GEBAI_BUILD_SUBAGENTS`（逗号分隔包含清单，缺省 = 全部打包）按需产出精简二进制；`GEBAI_BUILD_PRELOAD`（逗号分隔预加载清单）烘焙为 `def.preload=true`（启动即装载，运行时 `GEBAI_PRELOAD_SUB_AGENTS` 覆盖仍优先）；`GEBAI_BUILD_EXCLUDE_TOOLS`（逗号分隔**全局工具排除清单**，`scripts/build-tools.ts` 生成 `tools-excluded.generated.ts` 烘焙）——被排除的全局工具不注册不暴露（schema 不可见、调用报未知工具），agent_run 新会话内建编排工具（flow/tool_schemas/js）与 index.ts 的 vision 注册同规则过滤（`isGlobalToolExcluded`）；语义注意：全局工具排除是**能力裁剪**——工具实现与工具表同模块仍会打包（无法摇树），体积裁剪主要来自子Agent 包含清单与内嵌产物跳过。三清单中的未知名字构建直接失败并列出可用名单（防产物静默缺失）。`tools-excluded.generated.ts` 与 subagents.bundle 不同——**提交默认空名单入库**（消费方 `core/tools.ts` 静态导入：运行时读文件在 `--compile` 单文件形态不可行），裁剪构建后为脏属预期，勿提交裁剪态
- **裁剪构建样例**（根 `scripts/` 目录，`bun run build:code`）：`build-code-agent.ts` 产出 code 场景精简服务端单文件二进制（`packages/server/dist/gebai-code[.exe]`，浏览器形态、内嵌 Web UI）——三层裁剪组合示范：子Agent 包含清单（code+explore，体积收益主来源：未选子Agent 模块整体摇出产物）+ 预加载清单（code 开箱即用）+ 全局工具排除清单（draw/render_html/fetch_url）。作为其他场景裁剪构建的模板：复制脚本改三处清单即可（如 reverse_site 站点逆向、feishu 文档、只读分析）
- 命名规则：仅限小写字母、数字、下划线
- 子Agent 定义的工具名无需关注前缀，总Agent 负责在其 schema 中添加 `{agent_name}_` 前缀（命名空间规则见「工具与命名空间」），以及工具调用时的路由和转发，对子Agent 完全透明
- 环境变量作用域：子Agent 只能访问 `{AGENT_NAME_UPPER}_*` 前缀的变量
- 全局共享：所有用户使用同一份内置子Agent，无用户差异

#### 装载 vs 新会话执行（概念模型）

「子Agent」一词承载**两种语义迥异的操作**，代码、文档与系统提示词必须严格区分——这是消除模型误解的关键：

| 维度 | **装载**（`agent_load`，模块语义） | **新会话执行**（`agent_run`，会话语义） |
|------|------|------|
| 类比 | import 子模块 / 静态链接 | 派生新会话 / 独立执行体 |
| 前置条件 | 无 | 无（**无需先装载**，两种操作互相独立） |
| 上下文 | **不创建新上下文**：并入主上下文 | **新建独立上下文**：独立消息历史 + 独立系统提示词 + 独立工具循环 |
| 效果 | 工具注册进当前工具集（`{agent}_` schema 全名）、**完整系统提示词作为 system 消息写入会话记录**（`loadedAgent` 标记持久化，`loadHistory` 时按 system 角色**前置**进模型上下文——统一置于历史最前，保持 `assistant(tool_calls)` 与其 `tool` 结果相邻，避免装载消息夹在中间被模型接口校验拒绝（实测 DeepSeek 400「tool_calls 后必须紧跟 tool 响应」）） | 派生临时新会话，**预加载一个或多个子Agent**（完整系统提示词拼接+工具并入）后阻塞执行，只返回最终结果文本 |
| 隔离/存档 | 无隔离、无存档（全程在主上下文，模型可见每一步） | 完全隔离（中间过程/推理/内部工具不进主上下文）+ `SessionRunArchive` 完整存档 |
| 幂等 | 幂等（重复装载跳过） | 每次执行 = 一次新会话 |
| 生命周期 | 装载/卸载（`sub_agent.load`/`agent_load`/预加载，会话级持久） | 一次执行即结束（run 生命周期） |

- **装载（模块）**：类比 import 子模块——子Agent 的工具注册进当前工具集（`{agent}_` 前缀）、**完整系统提示词作为 system 消息写入会话记录**（`loadedAgent` 标记，chat.json 持久化；装载后当次会话后续轮次立即进入上下文，恢复历史会话时从会话记录透传），装载后直接调用其 `{agent}_` 工具，全程在主循环/主上下文内完成，无独立执行过程。**会话记录 `loadedSubAgents` 保存已装载名单**：恢复历史会话时引擎自动按名单重新注册工具（`engine.ensureSessionAgents`，幂等），实现「会话按保存的文件完全恢复状态」。预加载（`preload`/`GEBAI_PRELOAD_SUB_AGENTS`）即装载的启动期形态——启动预载的子Agent 在每个**新会话创建时自动写入**提示词消息与工具
- **新会话执行（会话）**：类比派生新会话——`agent_run` 派生一个临时新会话（独立上下文），把指定的一个或多个子Agent **预加载**进该会话（各自完整系统提示词拼接为系统提示词、工具以 `{agent}_` 命名空间并入工具集），阻塞执行任务到结束，只把最终结果带回主会话；上下文隔离、全程存档，详见「新会话执行的上下文隔离」
- **多 Agent 预加载**：`agents` 参数支持列表（如 `["code", "playwright"]`）——多个子Agent 的能力同时进入新会话（提示词拼接、工具集叠加）；每个子Agent 提示词前加**职责分隔头**（`### {name}（{description}）`）明确各自职责域与工具命名空间；各自的项目绑定/预置项目注记分别注入，工作目录取首个含项目绑定的 Agent，预置项目全量合并（同名去重）
- **代码对应**：装载 = `SubAgentManager.load`（返回本次实际装载集合，含 `self_optimize` 连带 `code`）/ `ToolContext.loadSubAgent` / `engine.loadAgentToSession`（装载并写入会话记录）；新会话执行 = `engine.runNewSession` / `ToolContext.runNewSession`（原名 `dispatchSubAgent`/`subAgentRunner` 已更名）
- **模型侧引导**：`agent_load`/`agent_run` 工具描述、系统提示词注入均按上述语义措辞，默认引导「装载后用其工具」，仅在需要干净上下文或防上下文膨胀时用 `agent_run` 执行新会话

#### 路由自愈（未装载命名空间工具自动装载）

模型偶尔跳过 `agent_load` 直接调用未装载子Agent 的 `{agent}_*` 工具（长会话遗忘/弱模型跳步），旧版该调用以「未知工具」失败浪费一轮。现引擎在两个循环的解析失败点做**自动装载自愈**（`engine.subAgentForToolName` 最长 `{agent}_` 前缀匹配）：

- **主循环**：按 `agent_load` 同路径自动装载（`ctx.loadSubAgent`——工具注册进全局注册表 + 提示词写入会话记录 + 拼接进当前上下文系统前置段），随后重解析执行该调用；工具结果前置说明「引擎已自动装载子Agent X」，模型知晓能力已可用
- **新会话循环**：走 `loadAgentIntoRun`——工具注册进**本次运行注册表** + 提示词段落插入临时 messages 系统前置段 + 预置项目并入 `ctx.projects`，**不写全局注册表、不落盘父会话记录**（新会话执行隔离语义：子会话内的装载不外溢）；显式 `agent_load`（纯 md 组合子Agent 注入的编排工具）在新会话内同样双注册（`buildContext.loadIntoRegistry`），装载后工具立即可解析——修复了旧版「新会话内 agent_load 只注册进全局注册表、本次运行不可见」的断层
- **不可恢复时给出恢复面**：自动装载后仍不可解析（拼错工具名等），错误信息附该子Agent 的可用工具全名清单（`unknownToolMsg`），一步引导修正
- **既有防线不放宽**：自动装载后的调用照常走通道禁用/极简白名单/安全模式/审批全部门禁；主动 `agent_load` 仍是首选路径（系统提示词引导不变），自愈仅为兜底

#### 选择性打包

构建时按需选择打进二进制的子Agent 集合，支持三种方式（可组合）：

| 方式 | 配置 | 说明 |
|------|------|------|
| 全部打包 | 默认 | 打包 `src/sub-agents/` 下所有子Agent |
| 白名单 | 构建脚本传 `--sub-agents a,b,c` | 仅打包指定子Agent，控制二进制体积 |
| 黑名单 | 构建脚本传 `--exclude-sub-agents x,y` | 排除指定子Agent，其余全部打包 |

- 未打包的子Agent 在运行时不可见（`agent_list` 不列出、无法装载/执行新会话），相当于彻底裁剪该能力
- 打包清单支持按发行形态差异化（桌面版全量、服务端精简版等），同一套源码产出不同规格二进制
- 实现状态：当前 bundle 脚本默认**全量打包**所有子Agent；白名单/黑名单裁剪为后续迭代（`SubAgentManager` 的 `bundledNames` 选项已预留，构建脚本参数未接通）

#### 选择性预加载

- **预加载**：启动时（或会话创建时）自动**装载**的子Agent 模块（模块语义，见「装载 vs 新会话执行」），其工具进入总Agent 工具集、完整系统提示词作为 system 消息写入新会话记录（`loadedSubAgents` 名单 + `loadedAgent` 提示词消息持久化）
- **按需装载**：默认未预加载的子Agent 仅注册在目录中，总Agent 通过 `agent_load`（或 WS `sub_agent.load` 带 sessionId）按需装载——装载即写入当前会话记录（工具注册 + 提示词消息），后续恢复会话时自动还原
- 通过 `preload` 字段或环境变量 `GEBAI_PRELOAD_SUB_AGENTS`（逗号分隔）声明预加载集合；未声明者**默认不预载任何子Agent**（按需装载）
- 预加载少而精：控制系统提示词与工具集规模，降低模型选择噪音；高频/核心子Agent 预加载，低频/重型子Agent 按需装载
- **内置子Agent 默认全部不预加载**（`preload = false`），完全按需装载；部署方可用 `GEBAI_PRELOAD_SUB_AGENTS` 声明预加载集合
- 会话级可通过环境变量（会话内存态或浏览器本地注入）覆盖预加载集合，按会话定制

#### 选用原则：默认「装载」而非「新会话执行」

**选用原则：默认「装载」而非「新会话执行」。** 默认用 `agent_load` 把子Agent 装载进总Agent（工具注册进工具集、完整系统提示词注入当前上下文，模块语义、无独立执行），总Agent 直接使用其 `{agent}_` 工具，过程全程可见、无结果截断损失；`agent_run`（无需装载，仅阻塞执行返回一种方式，会话语义）仅在两类场景使用：

| 场景 | 原因 |
|------|------|
| **需要干净上下文** | 子任务结果隔离：独立执行、只返回最终结果，中间过程与工具调用不进入主上下文，避免污染主 Agent 的推理链 |
| **防止上下文膨胀** | 子任务中间过程多、输出大：独立上下文执行并自行截断（结果超阈值按上下文保护规则落盘），主上下文只保留最终结果摘要 |

`agent_run` 只支持一种执行方式——**阻塞执行返回**（无 `mode` 参数，`delegate` 委托模式已移除）：执行时**无需先 `agent_load` 装载**，新会话（预加载子Agent 列表）在独立上下文执行任务，执行完毕返回最终结果给总Agent，总Agent 继续主导。

#### 新会话执行的上下文隔离

- 新会话拥有独立的对话上下文（独立消息历史与系统提示词），不污染总Agent 上下文
- 执行时传入的参数（`input`）作为新会话的初始消息；返回的结果作为一条消息注入总Agent
- 新会话内部工具调用不暴露给总Agent 上下文，仅返回最终结果（及可选的完整过程摘要）
- **执行过程完整可见（含推理/工具）**：`agent_run` 执行过程中，新会话的推理、每轮模型回复文本、工具调用与结果**全部实时推送到前端**（与主循环同构渲染——推理折叠块、工具卡片、流式文本），推送事件（`delta`/`reasoning`/`tool.call`/`tool.result`/`approval`）携带 `session: true` + `sessionRunId` 标记；另推送 `event.session.start`（run 开始，含 agents/input；**每轮重推、同 runId 幂等**，前端容器重建兜底）与 `event.session.done`（run 结束，含最终输出）。渠道层可据此区分「新会话执行过程」事件（如飞书渠道对新会话的 `done` 不触发最终卡片——任务结束以 `event.task.done` 为准）
- **前端折叠容器**：新会话执行过程渲染进 `details.session-run` 折叠容器——**执行中展开并滚动到可见**（完整过程实时展示，容器内限高 45vh 独立滚动 + 粘底自动跟随（sticky-follow 意图驱动核心，同主聊天区），用户上翻停止跟随），**执行结束后自动折叠，只显示输入与最终返回**（点 summary 可展开查看完整过程）；历史会话回放渲染同样的折叠容器（默认折叠）；嵌套执行（新会话内再 agent_run）在回放时递归渲染进外层容器 body
- **新会话执行存档（执行记录扩展字段）**：执行过程的**全部内容（输入/每轮回复/推理/工具调用与结果，含嵌套执行存档）**收集为 `SessionRunArchive`（`runId`/`agents`/`input`/`output`/`messages`），由 `agent_run` 工具作为**执行记录的扩展字段**（`Message.sessionRun`）随工具结果一起落盘——不再逐条写独立消息：会话文件只有一条 `agent_run` 工具消息即携带完整执行过程，历史回放据此渲染折叠容器（agent_run 结果卡片输出按 **markdown 渲染**；工具卡片气泡对 markdown 容器恢复 `white-space: normal`——气泡的 pre-wrap 面向纯文本参数/输出，markdown 软换行已是 `<br>`，markdown-it 在 `<br>` 后保留的源换行若再按 pre-wrap 折行，每行之间会多出一个空行）；仅存档与前端回放：`loadHistory` 跳过（**不进入主 LLM 上下文**，上下文隔离不变）、上下文压缩与 ctxTokens 估算同样排除（压缩时带存档的工具消息原位保留，存档不随压缩丢失）；主循环的 `agent_run` 执行卡片与最终结果仍属主上下文（模型可见执行与最终返回）；旧版逐条 `subAgent` 存档消息与 `agent_call` 时代存档（`SubAgentRunArchive` 单 Agent 形态）仍兼容回放（历史会话不受影响）

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

**两步解析（服务端确定无歧义）**

1. **全局精确匹配**：先查全局工具表，命中即全局工具
2. **子Agent 前缀匹配**：遍历已注册子Agent，取「最长匹配的 `{agent}_` 前缀」，剩余部分为工具名——注册表固定，解析结果确定，任何 `a_b_c` 形态都有唯一解

**注册期碰撞检查**（构建时自动校验）

- 全局工具名不得以任何 `{agent}_` 开头（与子Agent 命名空间互斥）
- 子Agent 名不得互为前缀（如 `web` 与 `web_search` 同时存在会引入前缀竞争，构建时报错）

**弱模型容错兜底**

- 精确查找失败时做**分隔符归一化重试**：将工具名中的 `-`、`.`、`:` 全部替换为 `_` 后重查，覆盖弱模型的分隔符误写
- 仍失败则返回明确错误（提示最接近的候选工具名），引导模型下一轮修正，不静默吞错

**透明转发**

- 子Agent 内部只见自身工具名 `tool_name`，schema 注入时由总Agent 加 `{agent}_` 前缀，调用时按上述两步解析后透明转发，对子Agent 完全无感
- 同一子Agent 可被多个会话同时加载使用，无共享可变状态（工具函数为纯函数或注入依赖）

**工具卡片展示元数据（`Tool.card`，注册时声明）**

- 工具定义可声明 `card: { titleParams?, args?, codeField?, codeLang? }`，经 `session.tool.get` 随 ToolInfo 下发，前端按声明渲染工具调用卡片，**不在前端硬编码工具名**：
  - **卡片头部为结构化布局**（图标 `🛠`/`✓` + 工具名 + 标题参数后缀三个 span）：后缀**全文展示不缩略**——短值直接入标题，超长值（后缀全文 >48 字符，头部一行放不下）不入标题、降级为参数气泡在参数区展示；实时调用、完成态更新（`appendToolResult` 重建头部）与历史重载三态共用同一渲染入口（`toolHead`）
  - `titleParams`：参数名列表，其值直接拼入卡片标题——**单参数仅显示值**（`🛠 read · src/main.ts`，省略 `key=` 前缀），多参数 `key=value`（`·` 连接）；简单工具的关键参数（如 `open` 的 url、`write` 的 path）一目了然
  - `args`：参数区模式——**缺省自适应**（扁平标量参数渲染为**键值行**：参数名 + 值 pre-wrap 展示，比 JSON 块更可读；嵌套结构回退 JSON 语法高亮）/ `"json"`（强制完整 JSON 高亮，标题参数不省略）/ `"kv"`（强制键值行，嵌套值紧凑 JSON 单行展示）/ `"none"`（不展示参数区，如 `env_detect`）/ `"code"`（`codeField` 参数渲染为语法高亮代码块，其余参数键值行/JSON 附注，如 `sh` 的 command；**超长内容同样先渲染后自动折叠**，不整块直显）/ `"edits"`（`codeField` 数组参数的 `{oldString,newString}` 项渲染为**旧（红）/新（绿）对比块**——多处修改编号「修改 i/n」，空串侧省略（纯新增/纯删除），形态不符回退自适应渲染；`edit` 工具因此不再显示 JSON；**edit 工具 card 声明不可用时（工具清单拉取失败/旧服务端未声明）前端按参数形态内建兜底同样渲染对比块**，长短修改一律先渲染后按阈值折叠，不回退直显 JSON）/ `"block"`（**结果直出内容块**：调用不显示通用工具卡片，结果直接渲染 blocks 内容块，如 `draw`/`diff`/`render_html`/`show_file`）
  - **标题参数不在参数区重复展示**（titleParams 已入标题的键从参数区省略，如 `read` 参数区只显示 offset/limit；全部参数入标题时无参数区）；**超长未入标题的标题参数降级为参数气泡**（与其余参数一同以键值行全文展示，如超长路径 `path: <全文>`，不截断）；**超长参数区自动折叠**（>800 字符默认收起为「查看参数（N 字符）」，点击展开，与输出折叠同款交互）
  - 已声明示例：全局 `sh`/`py`/`write`（code 模式）、`edit`（edits 模式，标题参数 path）、`read`/`fetch_url`/`agent_load`/`agent_run`/`ls`/`grep`/`glob`/`file`（标题参数）、`patch`（code 模式 + 标题参数 path）、`draw`/`diff`/`render_html`/`show_file`（block 模式）；code `search_symbols`/`analyze`（标题参数）、`env_detect`/`system_info`（无参数区）；widgets `save`/`get`/`delete`（标题参数）；desktop `screenshot`/`window_focus`/`key_press`/`window_list`/`clipboard_read`/`screen_info`；playwright `open`/`new_page`/`serve_dir`/`switch_page`/`close_page`/`press`/`pages`/`close`
  - **`agent_run` 专用卡片**：卡片头部直接列出**全部预加载子Agent 名**（`🛠 agent_run · code + playwright`，以 `+` 连接、不截断、省略 `key=` 前缀——后缀 span 带 `wrap` 标记允许多行完整展示）；参数区**只显示输入提示词**（任务指令全文、pre-wrap 展示，不参与自动折叠）；实时卡片完成态（`✓`）保留标题后缀，参数区不因执行完成而消失（执行中与完成后均可见）
  - 元数据拉取失败或未声明时按默认渲染（标题仅工具名、参数区按缺省自适应规则），不影响功能

#### 新会话执行安全限制

- **递归深度上限**：新会话内再执行新会话的嵌套深度受限（默认 3 层），防止无限递归
- **不设执行超时**：`agent_run` 执行不设置整体超时——执行过程新会话回复实时推送到前端（进度可见，无「无反馈空转」问题），中止仅由父任务取消（用户停止）传播；挂起工具仍受工具级超时兜底（`TOOL_TIMEOUT_MS`）保护
- **轮次上限**：新会话内部工具调用轮次受限（与主循环同一常量），防止失控循环
- **结果大小限制**：新会话返回结果超过截断阈值时按上下文保护规则处理

### 内置子Agent 示例：通用源码分析/修改（`code`）

作为子Agent 扩展机制的落地示例，内置「通用源码分析与修改」子Agent，定义于 `packages/server/src/sub-agents/code.ts`，面向**其他项目**（本地使用场景下不限制目录，可访问本机任意路径）：

```ts
export const name = "code"
export const description = "涉及代码编写与源码分析时装载本子Agent（不处理歌白自身代码，自我优化用 self_optimize）：新建/修改项目与功能实现、代码分析、问题定位修复；装载后按 探索→方案→修改→验证 流程执行，改动较多时优先 patch；写操作需审批。输入：需求/问题描述；输出：代码修改方案与验证结果。"
export const systemPrompt = "你是源码分析与修改专家（工作流参考 opencode 编码助手）。工作流程：\n0) 环境确认：开工前先读本提示词开头注入的项目环境注记（项目根/工作目录、预置项目清单、受限模式说明）——目标代码所在项目与 project 参数取值由此确定；分析某系统/服务源码时，先在预置项目清单（名称/说明/路径）中定位系统本体，警惕与目标同名的 API 封装/适配层（网关封装 ≠ 被管理系统源码），不要在其上浪费时间；清单确无对应项目时用 project 参数直接传项目根路径（自由项目），或先用 project 工具（action=use）设定会话默认项目根——此后未传 project 的文件工具都以该根为基准，不必每次重复传长路径（预置清单/当前设定随时用 project action=list 查看）；\n1) 规划：多步骤任务先用 todo 建立待办清单（entries 一次可含 add/update/delete 多条，探索→定位→方案→修改→验证），eta 参数给出每步预计耗时（分钟）让用户有耗时预期；每完成一步用 todo 更新状态，返回的清单即最新全部待办，无需再查；\n2) 探索：grep（内容搜索）/glob（按文件名查找）/search_symbols（按符号名定位**定义**位置，跨文件；找**引用/调用点**用 grep）/ls（目录结构）/analyze（tree-sitter 结构概览）快速定位，再精确读取相关文件（大文件分段读，避免整读超长输出），不大范围逐行通读；grep 宽泛摸底先 output=files，锁定文件后再 content 模式；对独立的目标可一次发起多个并行工具调用；查阅第三方库/框架文档用 fetch_url；跨大量文件的摸底/架构梳理（只要结论不要过程）可 agent_run 委托 explore 子Agent（只读探索，返回结论与 文件:行号 清单，中间过程不占本会话上下文）；\n3) 定位：梳理问题/需求涉及的代码位置、调用链与依赖关系；\n4) 方案：输出改动点清单（文件、改动内容、预期效果与影响面）；方向有取舍时用 ask_user 提供选项向用户确认（如实现方案、测试框架、改动范围）；可用 diff 展示「修改前/后」对比供审查；\n5) 修改：先 read 目标区域确认当前内容，再动手；遵循项目既有约定——先看 README/package.json/AGENTS.md 与相邻文件，了解技术栈、风格与依赖，模仿现有写法（新代码的命名/注释密度/习惯与周围代码保持一致，不引入无关改动）；改动较多或行号容易偏移时优先 patch（上下文行给 2~4 行即可——过多易不匹配、过少定位不稳；一次补丁聚焦一个改动点，不相关的改动分批提交），小范围定点改动用 edit，write 仅用于新建/整体覆盖；edit/patch 成功即已按原文校验落盘，无需重读验证；补丁不匹配时先 read 当前文件内容核对再重试；不添加无关注释；不引入/提交密钥凭据；写操作（edit/write/patch/sh/py）需审批，修改前必须先给出方案；重复性/批量操作（批量替换、批量跑测试等）优先用 sh/py 脚本一次执行，避免大量单步工具调用；明确安全的只读命令（如 git status）与测试/静态检查类（bun test、pytest、tsc、eslint 等）可给 sh 传 approval:false 跳过本次审批（服务端按白名单强制校验，不满足仍弹审批），其余命令勿免审；py 的免审标记不生效（恒需审批），纯数据加工类 js 脚本可传 approval:false（含网络/进程/环境读取通道的代码除外）；长耗时命令（全量构建/测试/安装）可给 sh 传 async:true 后台执行（立即返回 taskId），期间继续其他工作，回头用 sh_task（action=wait/status）取结果——不必干等；\n6) 验证：先跑与改动相关的测试文件（如 bun test 指定文件），通过后再跑全量与类型检查/lint（bun run typecheck/bun run lint 等）确认无回归；失败先看错误信息定位（grep 错误关键字找断言/堆栈位置）再修复重测，不盲目重复执行；Python 项目用 py；Web 项目需要浏览器端验证时可 agent_run 委托 playwright 子Agent；服务端功能类改动可让用户用 preview_server 在临时新端口启动独立验证服务确认（不中断当前会话，验证完 action=stop 停止）；环境/依赖异常（工具链缺失、PATH 问题）用 env_detect 探测（平台/PATH/关键工具链版本/缺失组件），系统基础信息用 system_info；\n7) 收尾：用 git 工具只读查看变更（status/diff/log，无需审批）确认改动范围，只提交预期文件，不擅自 commit（add/commit 等写操作用 sh 且需审批；与本次任务无关的既有改动不要误动）；用 todo（空 entries 查询）核对全部待办后给出总结——**先结论后细节**（第一句话回答做了什么/结果如何），关键改动位置引用 文件:行号，改动理由与影响面随后展开；验证/测试未通过时如实说明并附关键错误输出，不粉饰、不略过失败项；\n项目与环境变量配置（项目相关配置经进程环境变量或前端本地注入进任务 env）：\n- CODE_PROJECTS：预置项目注册表（JSON 数组 [{name,path,description}]），文件工具用 project 参数传**项目名**（清单见本提示词开头「预置项目」注记；project 参数也接受项目根路径——自由项目，或先用 project 工具设定会话默认根）；\n- CODE_PROJECT：默认项目根绑定——未指定 project 时以该项目根为基准；\n- CODE_RESTRICT_PROJECTS：受限模式（true 开启）——仅允许操作预配置项目，自由路径（不带 project）被拒绝；\n未设置预置项目时直接用 path 参数传项目/文件路径（自由项目），或 project 工具设定会话默认根。"
export const tools: ToolSet = { read, write, edit, patch, sh, sh_task, py, ls, grep, glob, search_symbols, file, diff, analyze, git, project, fetch_url, ask_user, agent_run, todo, preview_server, env_detect, system_info }
export const requiresApproval = { edit: true, write: true, patch: true } // sh/py 走工具自身动态审批（默认需审批、approval 参数按次免审），不静态覆盖
export const preload = false
```

要点：

- **工作流（参考 opencode 编码助手）**：环境确认（先读提示词开头注入的项目环境注记——预置项目清单/项目根/受限模式，确定目标项目与 project 参数，警惕与目标同名的 API 封装/适配层）→ 规划（todo 待办跟踪）→ 探索（grep/glob/search_symbols/ls/analyze 先定位、并行调用；grep 宽泛摸底用 files 形态、锁定后 content+context 看语境；大范围摸底可 agent_run 委托 explore 只读探索，再精确读取）→ 定位 → 方案（改动点清单 + `ask_user` 方向确认 + `diff` 前后对比展示）→ 修改（遵循项目既有约定、与周围代码风格一致；改动多时 `patch` 补丁应用优先、`edit` 定点替换（唯一性校验）次之、`write` 仅新建/整体覆盖（未读守卫），不添加无关注释/密钥，成功后无需重读验证）→ 验证（测试 + typecheck/lint，失败续修）→ 收尾（`git` 工具只读核对变更后只提交预期文件，不擅自 commit；总结先结论后细节、引用 文件:行号、失败如实报告）
- **分析工具**：`read`（读源码，`lineNumbers=true` 行号定位）、`ls`（目录结构）、`grep`（内容搜索：files/count 形态摸底 + content/context 精读、include 过滤文件类型）、`glob`（文件名 glob 查找）、`search_symbols`（**跨文件符号定义定位**：tree-sitter 解析函数/类/方法/类型定义，返回 文件:行号: 类型 名称，精确匹配优先，比 grep 更精准）、`analyze`（tree-sitter 语法结构概览）、`diff`（修改前后/两文件对比，UI 并排高亮）、`fetch_url`（查阅第三方库/框架文档）、`sh`（运行测试）
- **修改工具**：`patch`（**unified diff 补丁应用**：一次多 hunk、行号模糊容错、原子落盘、dryRun 预演，改动多/行号易偏移时优先）/`edit`（定点替换，小范围改动）/`write`（新建/整体覆盖）由 code 的 `requiresApproval` 声明审批；`sh`/`py`（Python 项目执行测试与脚本）走工具自身动态审批（默认需用户审批，复用统一审批流；`approval:false` 按次免审，见「工具审批」）；`sh_task`（sh 异步后台任务管理，`sh async:true` 启动的任务查询/等待/终止，见「sh 异步后台任务」）；`file` 文件管理（rename/move/delete/info 多动作）；`git`（**只读**：status/diff/log 免审批，写操作走 `sh`）
- **协作工具**：`ask_user`（需求澄清、方案取舍确认，阻塞等待用户选择）、`todo`（待办增删改查统一入口，entries 列表一次批量操作、空列表即查询，与会话待办联动）、`agent_run`（执行新会话委托其他子Agent——如 `playwright` 做 Web 项目浏览器端验证；**不得委托 `self_optimize`**，见「职责边界」）
- **项目设定工具（`project`）**：预置项目与自由路径项目的统一入口——`action=use` 设定**会话粘性项目根**（project 参数形态：预置项目名或项目根路径），此后未传 `project` 的文件工具都以该根为基准（自由路径项目一次设定、免重复传长路径）；`list` 查看预置清单与当前设定；`clear` 清除。粘性根按 `user:sessionId` 记忆（含该会话派生的新会话执行——agent_run explore 等继承同一根）；受限模式下 `use` 仅接受预置项目名（与文件工具 project 参数同规则）
- **验证/环境工具（自全局工具集下沉，全局集最小化）**：`preview_server`（临时新端口启动/停止一份独立进程的 GEBAI 服务，`action=start`/`stop`，供用户验证服务端类改动——不中断当前会话，启动返回 URL/PID/日志路径；经 `projectAware` 包装以 `{workdir:true}` 暴露——带 `project` 参数/粘性根时以项目根为启动目录）、`env_detect`（环境探测：平台/架构、PATH 去重、关键工具链版本 node/bun/python/git/cargo 等（缺失标记不可用）、Windows MSVC 与 WebView2 状态，指导安装缺失组件）、`system_info`（系统基础信息）——三者不注册为全局工具，以 `code_preview_server`/`code_env_detect`/`code_system_info` 命名空间暴露（`self_optimize` 经连带装载 code 一并获得），全部免审批
- **tree-sitter 语法分析（`analyze`）**：基于 `web-tree-sitter`（wasm）按语言加载语法（JS/TS/TSX/Python/Go/Rust/Java/C/C++/JSON/HTML/CSS/Bash 等 30+ 语言，语法文件来自 `tree-sitter-wasms`），输出结构化概览（导入/导出、函数/类/方法/类型定义及行号、嵌套关系），替代逐行阅读快速定位；首次使用懒加载、按语言缓存 parser；**wasm 加载双通道**——dev 模式 `require.resolve` 读 node_modules，二进制/打包模式回退构建期内嵌注册表（`analyzer-wasm.embedded.generated.json`，`scripts/build-analyzer-wasm.ts` 生成，gzip 压缩约 3.6MB）；**报错区分两类**：语言不在映射表报「不支持的语言」（附支持列表），wasm 资源加载失败报「语法分析不可用」（提示依赖缺失/未内嵌并引导改用 read）——不再把资源缺失误报为语言不支持
- **两种项目形态（统一 project 参数，二选一 + 会话粘性默认）**：
  - **预置项目**：会话环境变量 `CODE_PROJECTS`（JSON 数组 `[{name, path, description?}]`）声明命名项目注册表——**工具以 `project` 参数（项目名）选择目标项目**，路径参数相对所选项目根解析；预置项目**可携带项目说明**（`description`），清单（名称/说明/路径）注入**子Agent 系统提示词**（engine 组装子Agent 提示词时注入——`agent_run` 执行新会话与装载写会话记录（`ensureSessionAgents`）均注入，**不注入总Agent 系统提示词**；注记置于职责分隔头之后、静态提示词之前（环境信息前置，模型开工先读清单按名选项目））；**描述动态体现**：engine 的 `agentDescription` 把预置项目摘要（名称: 说明（路径））与装载后工具摘要追加进 code 描述——未装载清单（总Agent 系统提示词）与 `agent_list` 输出均展示，总Agent 装载前即可按项目名/工具能力关联任务与代码位置，完整注记（工具 project 参数用法说明）仍只在子Agent 提示词；非法 JSON 静默忽略（回退自由项目模式），同名项目去重（首个生效）
  - **自由路径项目**：`project` 参数直接传**项目根路径**（绝对/相对；`looksLikePath` 判定——绝对路径、含路径分隔或 `.`/`~` 开头；本地模式绝对直用/相对按进程 cwd 解析，沙箱模式限定用户数据目录内与预置项目同规则）——无需预配置即获得与预置项目一致的「路径相对项目根解析 + 项目根工作目录 + 项目根递归搜索」三重路由；或保持旧行为不传 `project` 用 `path` 参数逐次传完整路径
  - **会话粘性项目根（`project` 工具）**：自由路径项目一次 `project action=use`（预置名或路径）设定默认根，此后未传 `project` 的文件工具一律以该根为基准（`sessionProjectRoots`，key=`user:sessionId`，`projectAware` 无 project 参数时优先取粘性根）——消除「每次调用重复传根路径」；`list`/`clear` 管理；本会话派生的新会话执行（agent_run explore 等）继承同一根
  - 实现：`code` 的每个文件工具经 `projectAware` 包装（`sub-agents/code.ts`）——传入 `project`（预置名或路径形态，`resolveProjectRoot` 统一解析）或存在粘性根时，把 `resolvePath`/`workdir`/`listFiles` 基准切换到该根（沙箱模式限定项目内，本地模式放开）；`grep`/`glob`/`search_symbols` 在项目模式下递归扫描项目根（跳过 `.git`/`node_modules`/`dist` 等大型目录，限深 10 层）；`git`/`sh`/`py`/`preview_server` 以项目根为工作目录
- **项目内置（特定项目绑定）**：会话环境变量 `CODE_PROJECT`（子Agent 大写前缀 `CODE_AGENT_*` 约定）声明默认项目根——子Agent 的工作目录与路径解析以项目根为基准，系统提示词注入项目路径；沙箱模式限定项目内，本地模式放开；未设置时按用户给定路径处理。**沙箱降级**：沙箱模式下越界/绝对路径的项目配置（`{AGENT}_PROJECT` 绑定与 `{AGENT}_PROJECTS` 预置项目）解析失败时**静默跳过/回退工作目录**（不使任务失败，与非法 JSON 忽略一致），避免本机环境变量注入的越界路径拖垮服务端部署。通用机制：任意子Agent 均可通过 `{AGENT_NAME_UPPER}_PROJECT`（项目根绑定）与 `{AGENT_NAME_UPPER}_PROJECTS`（预置项目注册表，engine 解析并注入 ctx/提示词，工具自行决定是否暴露 `project` 参数）使用。**总Agent 侧注入项目绑定（不含预置项目清单）**：`buildSystemPrompt` 汇总所有已注册子Agent 的 `{AGENT}_PROJECT`（「xx 子Agent 项目绑定：<根路径>（agent_run 新会话执行该子Agent 时以其为项目根；装载模式下路径基准为会话目录，访问项目用 project 参数或绝对路径）」——仅声明绑定，不宣称装载模式下工作目录已切换）注入总Agent 系统提示词；`{AGENT}_PROJECTS` 预置项目说明（含清单）**只注入子Agent 系统提示词**（`agent_run` 执行新会话形态，见上），总Agent 侧仅经**描述摘要**（`agentDescription`：未装载清单/`agent_list` 中描述附「预置项目：名称: 说明（路径）」）轻量体现，不注入完整清单注记；主循环 `buildContext` 合并全部预置项目供 `project` 参数路由（装载模式直接用其工具时 project 参数照常可用；清单提示词装载模式同样注入——`ensureSessionAgents` 写会话记录提示词消息时动态拼接 presetNote，与 `agent_run` 新会话一致）
- **受限模式（CODE_RESTRICT_PROJECTS=true）**：code 子Agent 文件工具（read/write/edit/patch/ls/grep/glob/search_symbols/file/diff/analyze/sh/py/git）**仅允许操作预配置项目**——必须携带 `project` 参数（项目名须在 `CODE_PROJECTS` 清单中），自由路径（`path`）被拒绝并提示改用 project 参数；处于 `{AGENT}_PROJECT` 绑定根内（新会话执行模式）或已设定粘性项目根（`project` 工具——受限模式下 `use` 仅接受预置项目名，设定时已校验）时未传 project 放行。**默认关闭（不限制）**，可经会话/用户/进程环境变量开启；受限说明属 code 子Agent 行为约束，**只注入子Agent 系统提示词**（`agent_run` 新会话执行时，code 静态提示词含开关含义、动态 restrictNote 注入当前开启状态），**不注入总Agent 系统提示词**——装载模式下模型走自由路径时由工具错误提示自愈引导（DESIGN「环境变量配置」）
- **环境变量一览注入提示词**：系统提示词内置「项目与环境变量配置」段落，逐条列出项目相关 env 的名称与作用（`CODE_PROJECTS` 预置项目注册表 / `CODE_PROJECT` 项目根绑定 / `CODE_RESTRICT_PROJECTS` 受限模式），模型无需猜测配置来源即可直接按名使用项目；预置项目实际清单（名称/说明/路径）仍由 engine 动态注入（见上），注记前置（职责分隔头之后、静态提示词之前）保证模型开工先读环境
- **项目约定自动注入（AGENTS.md）**：子Agent 项目根存在 `AGENTS.md`（兼容 `AGENT.md` 命名）时，engine 在 `runNewSession` 组装系统提示词时**自动读取并注入**（`\n\n项目约定（AGENTS.md，编码/维护必须遵守）:\n<内容>`，≤8KB 截断，不存在/不可读静默跳过）——code 绑定项目（`CODE_PROJECT`）与 self_optimize 绑定 GEBAI 仓库（`SELF_OPTIMIZE_PROJECT`）均生效，模型无需先 `read` 即可遵守项目约定；预置项目（`project` 参数形态）运行期才确定目标，不自动注入（模型可用 `read` 按需读取）
- **工作区限制（按运行形态）**：
  - **桌面/本地浏览器模式**：**不限制目录**，可直接分析/修改本机任意路径的项目（用户本地工作目录、克隆的仓库等），无需先复制进 `tmp/`
  - **服务端部署**：受路径沙箱约束（见「多用户隔离与安全」），目标项目须位于会话 `tmp/` 或用户数据目录内（`CODE_PROJECT` 同样受限）
  - 任一形态下**不得修改 GEBAI 服务端自身源码**（提示词与权限双重约束，自我优化走 `self_optimize`）
- **验证闭环**：系统提示词要求修改后运行项目测试（按语言选 `bun test`/`npm test`/`pytest` 等，Python 项目用 `py`）与类型检查/lint（`bun run typecheck`/`bun run lint` 等），失败则继续修复；Web 项目需要浏览器端验证时经 `agent_run` 委托 `playwright`
- **新会话执行模式**：总Agent 以 `agent_run` 执行新会话（预加载 `code`）时，`code` 自主完成「规划 → 分析 → 方案 → 修改 → 验证」全流程后交回
- **预加载**：`preload = true` 使该子Agent 默认注入，随时可被总Agent 选用
- 新增子Agent 只需照此模式放置 TS 文件，构建时选择性打包（`--sub-agents code`）

> **职责边界**：`code` 只处理外部项目。**自我优化（改 GEBAI 自身代码）由独立的 `self_optimize` 子Agent 承担**（见下节），两者工作区、审批策略、提示词均分离，避免普通源码分析流程获得修改服务端代码的权限。`code` 的 `agent_run` 委托仅用于只读/验证类委托（如 `playwright`），不委托 `self_optimize`（防经子Agent 链间接获得服务端修改能力，边界由总Agent 路由引导「GEBAI 自身代码 → self_optimize」维护）。

### 更多内置子Agent

按同一模式补充以下实用子Agent，构建时选择性打包：



#### `desktop`（桌面控制）

```ts
export const name = "desktop"
export const description = "涉及宿主机桌面操作时装载本子Agent（仅本地模式，服务端部署不可用）：截图、窗口控制、键盘鼠标输入与剪贴板；窗口优先 PID 定位，输入/点击类操作需审批。输入：操作目标；输出：操作结果与屏幕图片。"
export const systemPrompt = "你是桌面控制助手，直接操作宿主机桌面（仅本地模式；服务端部署下所有工具拒绝执行）。工作流程：1) 明确目标：先 window_list 确认目标窗口（PID/进程名/标题；窗口优先用 PID 定位——标题可能重复），截图前先说明截图用途；2) 准备：截图/坐标操作前先 screen_info 确认显示器分辨率与主屏原点（主屏左上角为坐标原点），避免 region/坐标错位；3) 执行：输入/点击/窗口控制类操作（window_focus/window_move/type_text/key_press/mouse_move/mouse_click）需审批，操作前先说明操作意图与目标窗口；执行前必须先用 window_focus 确认目标窗口已激活，避免输入到错误窗口；只读类（screenshot/window_list/screen_info/clipboard_read）免审批可直接执行；4) 反馈：执行后反馈结果——截图返回图片，mouse_click 坐标基于截图实际尺寸判断，说明截图位置与关键结论；5) 约束：只执行用户明确要求的操作，不做额外破坏性动作（不改系统设置、不删除文件、不触发危险快捷键）。验证多通道：不依赖单一验证通道——截图黑屏/纯色（工具会主动提示）时立即切换通道，不要反复重试截图：用 window_list 确认窗口是否在前台、用 clipboard_read 验证剪贴板状态，或经 agent_run 委托 code 子Agent 读取应用数据文件断言结果；任何通道失效即降级并明确告知用户当前采用的验证方式。"
export const tools: ToolSet = { screenshot, window_list, window_focus, window_move, type_text, key_press, mouse_move, mouse_click, clipboard_read, screen_info }
export const requiresApproval = { window_focus: true, window_move: true, type_text: true, key_press: true, mouse_move: true, mouse_click: true }
export const preload = false
```

- 实现于 `sub-agents/desktop/` 目录（`desktop.ts` 定义 + `desktop_tools.ts` 工具集 + `desktop_tools.test.ts` 测试），工具不注册为全局工具，仅经子Agent 命名空间暴露（另含 `agent_run` 编排工具：验证多通道委托 code 子Agent 读取应用数据文件断言结果）
- 跨平台：Windows 走内置 PowerShell（截图/窗口/输入，无外部依赖）；macOS 走 `screencapture` + `osascript`（鼠标需额外 `cliclick`）；Linux 依赖 `xdotool`/`wmctrl`/`scrot`（缺失时明确报错）
- 截图返回 `image` 内容块实时展示，并**自动做黑帧/纯色检测**（平均亮度极低提示显示器休眠/锁屏，暗色单色提示非真实画面）与**尺寸元数据**（`STAT` 行 / sips / ImageMagick）；`screen_info` 列出全部显示器（分辨率/位置/主屏）供 region 与坐标参考；文本输入默认剪贴板粘贴法（中文/符号可靠，**输入前备份、输入后恢复**，Windows try/finally、macOS 容错恢复），`mode="keys"` 纯按键模式（绕剪贴板，仅 ASCII）；`type_text`/`clipboard_read` 输入或读取前自动做**敏感值扫描**（`sk-`/超长串/KEY=值/Bearer 令牌），命中即中止/告警防密钥泄漏；**服务端部署（沙箱启用）下所有工具拒绝执行**

#### `feishu_docs`（飞书云文档）

实现于 `sub-agents/feishu_docs/`（目录形式：`feishu_api.ts` 工具集 + `feishu_docs.md` 系统提示词 + `feishu_docs.ts` 定义入口），全面对接飞书开放平台云文档能力（docx v1 / drive v1 / sheets v2·v3 / bitable v1 / wiki v2）：

- **文档 docx**：创建/元信息/纯文本/块结构读取、**按文本反查块 id**（`find_blocks`：子串匹配忽略大小写，支持块类型过滤，返回 block_id/类型标注 `type_name`/文本/所在路径）、**小节读取**（`get_doc_text` 传 `block_id` 只读该子树文本，含标题层级与表格行，长文档按小节读取）、**自动翻页**（`get_doc_blocks`/`list_blocks` 支持 `page_all=true`，上限 2000 块，**达到上限时明确提示改用小节读取/翻页续读**；块列表输出附 `type_name` 类型标注——**实测修正：图片块=27，原 43 误标 image（43 为 mindnote 思维笔记），quote=15**）、**图形块读取 `get_board`**（块类型 43 = mindnote 思维导图/画板，`get_doc_blocks` 只返回 `board.token` 占位；`get_board` 收 `board_token` 或 `document_id`+`block_id`（自动从 mindnote 块提取画板 token），调 `/open-apis/board/v1/whiteboards/{token}/nodes`（board v1）分页读取并**结构化提取后再输出**——**优先返回 PlantUML 源码（`syntax.code`）**（UML 图语义完整），否则**重建「形状文本 + 连接线关系」为流程描述**（`<A> ->(label) <B>`），避免原始大 JSON 爆 token/截断；权限 scope `board:whiteboard`）、添加块（children 通道自动分批 ≤50；**含 todo/表格/嵌套结构（children 引用/自带 block_id）时自动改走「创建嵌套块」descendant 通道**——一次请求创建完整表格/嵌套结构，**表格支持 `table.rows` 二维数组简化写法**自动展开为 table+table_cell+text（避免逐格填充 N 次调用与限频 429），嵌套子块递归补全局唯一 block_id（忽略调用方自带 id 防引用冲突）；**块字段自动映射**：按 `block_type` 把统一传的 `text` 字段改写为对应驼峰字段 `heading1~9`/`bullet`/`ordered`/`quote`/`todo`/`code`（16 equation 不可创建，不做映射），code 块 `language` 语言名自动转数字枚举（**实测 26=JavaScript**，未知回退 PlainText），分割线块自动补 `divider:{}` 字段（缺字段 invalid param），**缺 `block_type` 默认 text、字符串字段/顶层 elements 自动包装**（修复简化写法 99992402），**非文本容器类型（table 等）不再被强制转成 text 字段**（修复 invalid param），**描述附全部可创建块类型提示表**（文本类 2 text/3~11 heading1~9/12 bullet/13 ordered/14 code/15 quote/17 todo/22 divider（**16 equation 不可经 API 创建**——官方创建接口枚举不含 16，实测 99992402，提示改用普通文本块/手动插入）；容器类 31 table（`table.rows` 简化写法）、24 grid（`column_size` 2~5 必填且与 grid_column 子块数一致，校验不一致报 1770041）+25 grid_column（**不带 `width_ratio`**——实测 9499 invalid parameter，列宽默认均分；列内内容创建后经 `update_block` 填充，带 children 报 field validation failed）、19 callout（正文在 `callout.elements`（Text 结构）非 children，**颜色/emoji 字段统一归一进 `callout.style`**——background_color/border_color/text_color 数字枚举、emoji_id 表情名，实测放 callout 顶层报 schema mismatch，text 快捷写法自动映射）；引用型 35 embed（url）、37 file（token）、39 sheet（token）、43 mindnote（token）、44 bitable（token）、46 diagram（diagram_type）；限制提示：27 image 走 `insert_image` 三步流程、32 table_cell 不可单独创建、1 page 不可创建）——模型按提示可直接构造各类型块 JSON，**块类型提示表内置于 `add_blocks` 工具描述**（描述是调用时第一信息来源；系统提示词只留「见 add_blocks 工具描述」指针——**单源不重复**，块表两份拷贝曾多占约 1.6k 字符上下文））、更新块（PATCH 文本元素/插入；**`insert_text` 失败（飞书参数校验严格报 invalid param）时自动降级**：读块原文 → 在 index 处拼接 → 整体替换，功能兜底可用）、**插入图片 `insert_image`**（官方三步流程：创建空 image 块（block_type=27 `image:{}`，创建时传 token 报 1770001）→ `medias/upload_all` multipart 上传素材（`parent_type=docx_image` 关联 image 块；**云空间 file_token 不能直接用于文档 image 块**）→ PATCH `replace_image` 设置素材 token，width/height 自动识别）、批量删除（**半开区间语义 `[start, end)`**，end_index 缺省/相同自动按单块删除）、**Markdown 导入**（`import_markdown`：本地转换引擎——**多级标题（#~#########，1~9 级）/段落/有序无序列表（缩进嵌套：每 2 空格或 1 tab 一级，跳级缩进归一为 +1 级、最深 9 级，子项作为父块 children 经 descendant 通道一次创建）/任务列表/代码块/引用/表格/分割线/行内样式（加粗/斜体/粗斜体/删除线/行内代码/链接）**，GitHub 告示语法 `> [!NOTE]`/`[!TIP]`/`[!IMPORTANT]`/`[!WARNING]`/`[!CAUTION]` 自动转 **callout 高亮块**（浅色系背景+同色系边框枚举+emoji，标题粗体置首）；经 descendant 通道写入，自动分批 ≤1000；**代码块 language 为数字枚举**（语言名自动映射，未知回退 PlainText）；`engine="official"` 走官方 `blocks/convert` 转换通道，**复杂组合（代码块+表格）转换失败时自动回退本地转换**并提示）、导出（`export_tasks` 创建+轮询——**轮询查询只携带 `token` query 参数**（实测多余 file_extension/type 报 field validation failed），任务终态失败（job_status 3/107~123）提前返回 job_error_msg；**导出 token 必须是文档级 token**——bitable 传 app_token，传数据表 table_id 报 1069914 file token invalid；**bitable/sheet 导出 csv 必须指定 `sub_id` 子表 ID**（官方接口要求，xlsx 不需要）→ `download_file`）
- **错误可诊断性**：API 错误统一携带 `method path` 与常见错误码提示（token 失效/过期、scope 未开通、参数不合法）；**权限类错误码（9999166x/9999167x，含实测 99991668/99991672 等未逐一收录的码）自动附「按接口路径特征推断的所需 scope + 开发者后台授权链接」（`https://open.feishu.cn/app/{app_id}/auth?q={scope}`，app_id 非敏感）**，减少 AI 反复试错、引导用户快速授权；docx 块操作失败时本地探测诊断，区分 `DOC_NOT_FOUND` / `BLOCK_NOT_FOUND` / 叶子块不支持子块，避免「invalid param」式无信息错误码的盲猜重试
- **用户授权（user_access_token，创建用户所有权资源）**：默认应用身份（tenant_access_token，资源归应用所有）；`auth_user_authorize`（生成 OAuth 授权链接：`accounts.feishu.cn/open-apis/authen/v1/authorize`，参数 scopes/redirect_uri/state/prompt=consent，缺省 scope `docx:document offline_access auth:user.id:read`）→ 用户授权 → **默认自动回调**（回调地址 = `GEBAI_PUBLIC_URL`（缺省 `http://localhost:{GEBAI_PORT|3000}`）+ `/api/v1/oauth/feishu/callback`，首次使用需在开发者后台「安全设置 → 重定向 URL」登记）→ **REST 回调端点自动兑换并写回发起授权的会话**（无需粘贴 code）；回调失败时保留手动路径：粘贴带 `code` 的回调地址 → `auth_user_token`（**OAuth v3 token 端点 `accounts.feishu.cn/oauth/v3/token` 兑换**（grant_type=authorization_code，携带 client_id/client_secret）；`code` 从完整回调地址自动提取，state 与授权时比对防混淆；尽力绑定用户信息）→ `auth_user_status` 查看/`auth_user_clear` 清除。**令牌按会话存储**（会话目录 `feishu_user_token.json`，`UserTokenStore` 依赖注入可测试，绝不输出明文）；**配置后资源类接口（docx/drive/sheets/bitable/wiki/board/搜索）自动以用户身份调用**（创建资源归用户所有、读写用户文档无需再添加应用协作）；access 过期**自动用 refresh_token 刷新**（单次有效、轮换落盘），刷新失败（授权超 365 天等 OAuth 错误 20037/20064/20073 附提示）回退应用身份并提示重新授权；**99991679（用户令牌缺权限）自动附 `permission_violations` 缺失 scope 清单 + 重新授权引导，且失效缓存重读一次自动重试**（重新授权后新令牌自动生效）；OAuth 错误码（20003/20004/20065 授权码失效、20071 回调不匹配、20074 未开启刷新开关等）附可读提示；**REST 回调端点 `GET /api/v1/oauth/feishu/callback`**（公开免鉴权，state 即会话关联凭证：随机不可猜、一次性消费防重放；兑换成功发布 `event oauth.completed` 并回显绑定用户；失败页附可读原因）
- **云空间 drive**：文件清单/文件夹创建/**元信息（走 `metas/batch_query`，`files/{token}` 对 docx 返回 404；缺省 type 自动识别失败时自动按 `doc_type=file` 补查）**/上传（multipart，支持 base64）/下载（落会话 tmp/，文件名经 batch_query 获取；**下载请求携带 `Range: bytes=0-` 头**——导出文件缺 Range 返回 403；**导出产物是 media 类型 token，`files` 接口 403/404 时自动回退 `medias` 下载接口**）/删除
- **电子表格**：创建（v3）、工作表信息（**合并返回完整工作表列表 `sheets/query`**）、读写（v2 values，`range` 前缀**支持工作表名称自动解析为 sheet_id**（用名称读写本须 sheet_id，直接用报 90215）；**前缀与任何工作表名/sheet_id 均不匹配时本地直接报错并列出可用工作表清单**——常见误因：误用表格标题作前缀，替代接口侧 90215 盲错；sheet_id 按 `sheets/query` 返回字段匹配透传，不依赖 oVs 前缀形态）、追加行（**单格 range 自动扩展为覆盖全部数据行的区域**，只传起始格追加多行不再报 wrong range）
- **多维表格**：创建（**支持 `fields` 字段定义数组创建后自动建自定义字段**——默认表只有基础字段，写入自定义字段名报 FieldNameNotFound；type 枚举：1 多行文本/2 数字/3 单选/4 多选/5 日期/7 复选框/11 人员/13 电话/15 超链接，单选多选需 `property.options`，逐个 POST `/tables/{id}/fields`；**创建后平台默认自动生成 10 条空占位记录（平台行为）**，写入时直接更新/追加即可）、数据表列表、记录增删改查（批量新增/更新/**删除走 `records/batch_delete`（POST，body 为字符串数组 `records: [\"rec1\",...]`，对象数组实测报错；DELETE 的 body 不生效）**、查询支持 filter——**filter 走 `records/search`（POST body），GET `?filter=` 报 InvalidFilter**）
- **知识库**：空间列表、创建节点（创建后可用 Markdown 写入正文）、按 token 查询节点
- **搜索**：云文档搜索（`suite/docs-api/search/object`，需开通「云文档搜索」权限）
- **权限**：添加协作者（`drive/v1/permissions`）、**链接分享设置（`set_link_share`：**PATCH** `permissions/{token}/public`（实测修正：方法为 PATCH 非 PUT，PUT 404），type 必填缺失 404，link_share_entity 枚举 tenant_readable（缺省组织内可读）/tenant_editable/anyone_readable/anyone_editable/closed）**
- **兜底**：`api_call` 直接调用任意 `/open-apis/` 接口（自动携带 tenant_access_token，路径白名单校验），保证新接口零等待可用

凭证从环境变量读取（子Agent 前缀规范）：`FEISHU_DOCS_APP_ID` / `FEISHU_DOCS_APP_SECRET`（兼容全局 `GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`，见「常量与环境变量」表）；统一使用 `tenant_access_token`（应用身份），仅能访问**应用自有资源**（应用云空间），访问用户文档需文档所有者授权；token 模块级缓存（有效期提前 200s 刷新）；写操作（创建/修改/删除/上传/授权）全部 `requiresApproval`；工具经 `createFeishuTools(deps)` 依赖注入工厂（fetch/token 缓存可注入），`markdownToBlocks`/`textElements`/`stripTableMergeInfo` 为纯函数，可独立单测（行覆盖率 95%）。**提示词纪律**：先读后写（修改/插入前先 `get_doc_text`/`get_doc_blocks` 确认当前内容、`find_blocks` 定位 block_id，防基于过期内容修改）；写操作前说明改动点与影响范围（审批预期）；批量写入自动分批（块 ≤50/记录 ≤100），限频 3 次/秒失败等待重试，不并发轰炸同一接口；**文档排版指南**（`feishu_docs.md` 独立章节）：生成/重写文档必须主动排版——Markdown → 飞书块对照表（标题/列表/待办/代码块/引用/告示/分割线/表格/行内样式）+ 排版原则（标题逐级不跳级、并列用列表、命令代码块化、二维信息表格化、提示用告示块）+ 工具选择（整篇优先 `import_markdown`、中间插块 `add_blocks`、改文本 `update_block`），**生成内容一律结构化富文本而非普通段落堆砌**。

#### `playwright`（浏览器自动化）

实现于 `sub-agents/playwright/`（目录形式：`playwright.ts` 定义入口 + `playwright.md` 系统提示词 + `playwright_tools.ts` 工具集 + `driver.mjs` node 桥接驱动），基于无头 Chromium（playwright）提供浏览器自动化：

- **工具集**（16 个）：`open`（导航，支持 http/https 与 **file:// 本地文件**，可配 waitUntil）、`content`（text/html/both 读取，超长自动截断落盘）、`screenshot`（页面/元素/整页，保存会话 `tmp/` 并返回图片块与绝对路径）、`click`/`fill`/`press`/`select`/`check`（交互与表单）、`wait_for`（元素/URL/网络空闲等待）、`evaluate`（页面内 JS 执行，JSON 序列化结果；失败提示改用 `content` 观察 DOM）、`pages`/`new_page`/`switch_page`/`close_page`（多标签页管理）、`close`（关闭会话浏览器上下文）、`serve_dir`（**内置静态文件服务器**：服务本地目录返回 http 地址，解决本地 HTML 需手工起服务的场景；服务端部署仅可服务预置项目目录）
- **桥接架构**：Bun 运行时与 playwright driver 的 pipe 通信存在兼容问题（chromium 启动超时，node 环境正常），因此 **Bun 进程内不直接 import playwright**——`playwright_tools.ts` spawn 常驻 `node driver.mjs` 子进程，经 stdin/stdout 行分隔 JSON-RPC 通信（请求 id 匹配、180s 超时杀进程重启、stderr 环形缓冲诊断）；driver 内 Browser 单例（断开自动重建），BrowserContext 按 sessionId 隔离（多用户/多会话互不串扰），空闲 10 分钟惰性回收；Bun 侧按会话串行化工具调用（同会话防并发操作同一页面）。桥接进程与浏览器会话**全进程共享单例**（`createLazyBridge()` 惰性构造——首次工具调用才解析 playwright 模块并启动桥接进程，playwright 与 reverse_site 两命名空间操作同一浏览器，页面状态一致；解析失败降级为工具级报错，不影响服务启动）
- **网络录制（driver 扩展能力，reverse_site 使用）**：driver 额外提供 `network_*` 操作（start/stop/clear/list）——按会话录制浏览器上下文内的全部请求（方法/URL/类型/请求头/请求体/响应状态/响应头/响应体预览，单会话上限 500 条，响应体预览上限 20KB），**录制时自动脱敏**（`authorization`/`cookie`/`x-api-key`/`set-cookie` 等敏感头与 postData 中 `token`/`password`/`secret` 等键值一律 `***`）；`network_list` 支持按 method/url（正则或子串）/status 过滤与 detail 详情开关
- **依赖与部署**：运行时需宿主机可执行 `node`。playwright 模块解析顺序：**单二进制形态**优先物化内嵌 playwright-core（`scripts/build-pwcore-embed.ts` 整树 gzip base64 内嵌 → 运行时物化 `{GEBAI_HOME}/vendor/playwright-core/`，版本不一致自动重建）；回退 `playwrightModuleUrl()` 经 `Bun.resolveSync` 解析 node_modules 的 playwright 包（源码/服务端部署形态）。浏览器本体不内嵌：Windows 默认 `channel=msedge` 驱动系统自带 Edge（Win10/11 必装，免下载浏览器），`GEBAI_PLAYWRIGHT_CHANNEL` 覆写（如 `chromium`/`chrome`，留空 = 不指定 channel、用默认 chromium——需 `bunx playwright install chromium`，Linux 服务端部署即此路径）。模块缺失/解析失败降级为**工具级报错**，不影响服务启动与其他子Agent。构建时 `driver.mjs` 由 `build-subagents.ts` 复制到 `dist/` 与产物同目录（不能被 bundle 内联）
- **审批**：导航/交互/脚本/服务类（`open`/`click`/`fill`/`press`/`select`/`check`/`evaluate`/`new_page`/`serve_dir`）默认需审批——服务端部署下可被诱导访问内网/提交表单，防 SSRF 与任意脚本滥用；只读类（`content`/`screenshot`/`pages`/`wait_for`/`switch_page`）与清理类（`close_page`/`close`）免审批
- **提示词纪律**：等待一律 `wait_for` 目标条件（不用固定 sleep——异步渲染时序不可靠）；被 `code`/`self_optimize` 委托做浏览器端验证时明确验证目标→操作→content/screenshot 取证→给出结论与证据位置；交互类操作前先说明意图（审批预期）

#### `reverse_site`（网站/接口逆向）

实现于 `sub-agents/reverse_site/`（目录形式：`reverse_site.ts` 定义入口 + `reverse_site.md` 系统提示词 + `reverse_site_tools.ts` 工具集 + `reverse_site_tools.test.ts` 测试），面向「逆向网站与接口」场景——摸清站点结构、还原接口契约、产出分析文档，并可与 `self_optimize` 联动把逆向结果转化为新的子Agent：

- **工具集**（22 个）：浏览器自动化全套（复用 playwright 的 `open`/`content`/`screenshot`/`click`/`fill`/`press`/`select`/`check`/`wait_for`/`evaluate`/`pages`/`new_page`/`switch_page`/`close_page`/`close`，与 capture_* 共享同一桥接进程与浏览器会话）+ 接口逆向专属 + 编排工具
- **接口捕获**（`capture_start`/`capture_stop`/`capture_clear`/`capture_list`）：录制浏览器网络请求还原接口——浏览前 `capture_start`，浏览/操作页面让 XHR/fetch 自然发生，`capture_list` 分析（默认摘要，`detail=true` 含请求头/体与响应头/体预览，可按 method/url/status 过滤，`file` 参数把完整记录导出会话 `tmp/` JSON）；录制在 driver 侧完成（见 playwright 网络录制），**敏感字段（cookie/token/密码等）自动脱敏**
- **接口探测**（`http_request`）：直接发送 HTTP 请求验证逆向出的接口（任意方法/请求头/请求体/查询参数），返回状态码、响应头（敏感字段脱敏）与响应体（50KB 内截断展示，超长走截断保护）；服务端部署限公网地址（复用 `assertPublicHttpUrl` 防 SSRF，默认路径另带重定向逐跳校验）；fetch 可注入（测试替身）；默认需审批
- **编排闭环**：内置 `agent_run`/`agent_list`/`agent_load`（子Agent 有工具时不自动注入，此处显式声明），分析文档写会话 `tmp/`（`site_map.md` + `api_docs.md`/`api_docs.json`）后经 `agent_run` 把文档交给 `self_optimize`，由 `self_optimize` 生成/修改子Agent 定义文件（`sub-agents/{name}.ts` + 可选 `{name}.md`）并通过测试验证——「网站 → 逆向文档 → 新子Agent」全链路
- **审批**：浏览器交互类（同 playwright）与 `http_request`/`write` 默认需审批，防 SSRF、防越权探测、防写操作；只读类（`capture_*`/`fetch_url`/`read`）免审批
- **提示词约束**：只逆向用户授权/自有网站，敏感信息不扩散，不爆破/不拖库/不高频恶意请求；`capture_list` 先摘要定位候选再 `detail=true` 细看（超长记录 `file` 导出后 `read` 分块分析）；验证多通道——截图/读取失效切 evaluate/capture_list/http_request，失效即切换并告知用户

#### `explore`（只读代码探索）

实现于 `sub-agents/explore/explore.ts`（目录形式），参照 ZCode Explore 子代理设计的**只读探索专家**——把「大范围扫读」从主上下文中剥离出去，主会话只拿结论：

- **用途**：跨大量文件的代码摸底/架构梳理/多点位定位——`code`（或总Agent）经 `agent_run` 委托执行，广度优先搜索后返回**结论 + 文件:行号 引用清单**，中间搜索/读取过程留在新会话存档里，不占主上下文（防上下文膨胀的标准委托形态）
- **硬约束（代码级）**：工具集**只有只读工具**（read/ls/grep/glob/search_symbols/analyze/git/fetch_url/todo，全部免审批）——没有 write/edit/patch/sh/py/file（含 delete/move 动作），探索不可能产生任何修改或命令执行；需要修改时装载/委托 `code`
- **工作流（提示词内置）**：圈定范围（project 参数/任务给定根；ls/glob 看结构、grep output=files 宽泛定位）→ 抽查精读（关键文件 read 分段读、search_symbols 定位定义、grep 定位调用点、analyze 结构概览代替通读）→ 汇总结论（**先结论后细节**，关键位置给 文件:行号，未确认点明确标注不猜测）→ 规模纪律（命中面大先 output=count 估面再挑重点；只保留与目标相关的结论）
- **项目路由**：文件工具经 `projectAware` 包装（复用 code 的实现，导出共享）——支持 `project` 参数按预置项目名路由，路径相对项目根解析；受限模式（CODE_RESTRICT_PROJECTS）同规则生效
- 与 `code` 的分工：`code` 是「探索→方案→修改→验证」全流程执行者；`explore` 只做其中的「探索」段且产出为结论——大范围摸底先委托 explore 拿地图，再由 code 精确改动，两段各司其职


#### `widgets`（HTML 小工具库管理）

实现于 `sub-agents/widgets/widgets.ts`，承接原全局工具 `save_tool`/`delete_tool` 并**补齐增删改查**（原缺 list/get，模型无法查看已保存的小工具清单与源码）；工具名从 `save_tool`/`delete_tool` 简化为命名空间内单字（`widgets_save` 等），**语义上与模型工具（tool call）区分**——「小工具」指 Web 标题栏「小工具」面板中加载的 HTML 页面组件，提示词中明确这一区分：

- **工具集**（四工具均 `interaction: "realtime"` 仅实时前端，多轮交互/无交互通道自动移除）：
  - `save`（`name`/`html`/`scope`）：保存/覆盖（增+改）；`scope=public` 公用 / `private` 用户私有（默认）
  - `list`：列出全部小工具（名称/范围/更新时间；私有同名覆盖公用不重复展示）——查清单
  - `get`（`name`）：读取完整源码与元信息（解析顺序：用户私有 → 公用）——查明细，修改前先 get
  - `delete`（`name`/`scope`）：删除（不可恢复），**需审批**（公用删除影响所有用户）
- **工作流（提示词内置）**：创建（render_html 调试满意 → save）→ 查看（list/get）→ 修改（get 读源码 → 改 → save 同名覆盖 → render_html 复核）→ 删除（delete，公用删前向用户确认）
- **存储/权限**：复用 `core/mini-tools.ts`（公用 `tools/` + 私有 `users/{user}/tools/` 哈希分片，同名私有覆盖公用；服务模式公用写仅管理员），见「HTML 小工具库」
- **预加载**：`preload = false`，按需装载


#### `cron`（定时任务管理）

实现于 `sub-agents/cron/cron.ts`，由原全局工具 `cron_add`/`cron_list`/`cron_update`/`cron_remove` **下沉为子Agent**（工具名经命名空间保持 `cron_*` 不变，`cron_list` 对应命名空间内 `list` 等单字工具）：创建/查看/修改/删除会话级定时任务——无人值守的周期性脚本与 Agent 任务（能力实现见「定时任务」）：

- **工具集**（四工具，命名空间内单字 `add`/`list`/`update`/`remove` → `cron_add`/`cron_list`/`cron_update`/`cron_remove`）：
  - `add`（`type`+`schedule` 必填，`script`/`prompt` 按类型必填，可选 `name`/`enabled`）：创建任务，返回任务 ID 与下次执行时间
  - `list`：查看本会话任务列表（ID/名称/类型/周期/启用状态/上次与下次执行/次数/最近错误）
  - `update`（`id`）：按 id 修改（启用状态/周期/类型/内容），改后自动重算下次执行时间
  - `remove`（`id`）：按 id 删除（不可恢复）
- **审批**：`add`/`update`/`remove` **默认需审批**（定时任务 = 无人值守的任意命令/会话执行，创建/修改/删除均须用户确认，服务模式下防普通用户绕过审批边界创建后门任务）；`list` 免审批
- **能力开关**：仅 `GEBAI_CRON_ENABLED=true` 时 `cron` 子Agent 注册（关闭时定义从子Agent 清单移除——`agent_list`/`agent_load`/`agent_run` 均不可见，与调度器一致完全隐藏）；`ctx.cron` 未注入（引擎未挂调度器）时工具返回「能力未启用」提示
- **会话绑定**：任务经 ToolContext 绑定创建它的会话，跨会话不可见、不可操作（`CronManager` 校验归属）
- **预加载**：`preload = false`，按需装载（与其余子Agent 一致）


#### 命名与预加载总览

| 子Agent | 工具 | 审批 | 预加载 | 适用 |
|---------|------|------|--------|------|
| `code` | read/write/edit/patch/sh/py/ls/grep/glob/search_symbols/file/diff/analyze/git/fetch_url/ask_user/agent_run/todo/preview_server/env_detect/system_info | edit+write+patch+sh+py | ✗ | 代码编写与源码分析/修改（非 GEBAI 自身代码：tree-sitter 语法分析、补丁应用、符号定位、git 只读核对、修改前后对比、文档查阅、待办规划、方案确认、Python 执行、浏览器端验证委托、项目内置、验证服务（自全局下沉 preview_server）与环境/工具链探测（env_detect/system_info）、文件管理（file：重命名/移动/删除/信息）） |
| `self_optimize` | 独有工具 read_feedback/run_tests/rollback/page_capture/vision；**通用工具与工作流直接复用 code**（装载/`agent_run` 预加载均连带 code——文件/分析/验证类操作用 `code_*` 工具（含 code_preview_server/code_file），code 工作流提示词随连带装载注入，不重复注册）；声明 `writeGuard` 写范围守卫（核心引擎源码默认只读的代码级强制） | run_tests+rollback（code_* 写类审批由 code 声明承接） | ✗ | 优化歌白自身（tree-sitter/补丁应用/验证服务等通用能力经 code；特有：反馈读取、测试准入+回滚、项目内置+AGENTS.md 自动注入、前端页面捕获读取实际 html/截图 + 视觉分析、写范围守卫；**装载即连带装载 code**） |
| `widgets` | save/list/get/delete | delete（公用/私有删除均需审批） | ✗ | HTML 小工具库增删改查（自全局 save_tool/delete_tool 下沉并补齐：保存/清单/读取源码/删除；四工具限实时前端 interaction=realtime；与模型工具语义区分——小工具是「小工具」面板加载的页面组件） |
| `explore` | read/ls/grep/glob/search_symbols/analyze/git/fetch_url/todo（全部只读，支持 project 参数路由） | 无（全免审批） | ✗ | 只读代码探索（大范围摸底/架构梳理/多点位定位，agent_run 委托执行，返回结论与 文件:行号 清单，中间过程不占主上下文；修改用 code） |
| `desktop` | screenshot/window_*/type_text/key_press/mouse_*/clipboard_read/screen_info | window_*+type/key/mouse | ✗ | 桌面控制（截图/窗口/输入/剪贴板/屏幕信息，仅本地模式） |
| `feishu_docs` | auth_status/auth_user_authorize/auth_user_token/auth_user_status/auth_user_clear/create_doc/get_doc_meta/get_doc_text/get_doc_blocks/list_blocks/find_blocks/add_blocks/update_block/delete_blocks/import_markdown/export_doc/list_files/create_folder/get_file_meta/upload_file/download_file/delete_file/search/create_sheet/get_sheet_meta/read_sheet/write_sheet/append_sheet/create_bitable/list_bitable_tables/list_bitable_records/add_bitable_records/update_bitable_record/delete_bitable_records/list_wiki_spaces/create_wiki_node/get_wiki_node/get_board/add_permission/api_call | 写操作全部（创建/修改/删除/上传/授权） | ✗ | 飞书云文档（文档/表格/多维表格/知识库/云空间/搜索/权限/思维导图画板；**可配置 user_access_token 以用户身份操作、创建用户所有权资源**；需配置 `FEISHU_DOCS_*` 或全局 `GEBAI_FEISHU_*` 凭证） |
| `playwright` | open/content/screenshot/click/fill/press/select/check/wait_for/evaluate/pages/new_page/switch_page/close_page/close/serve_dir | open+click+fill+press+select+check+evaluate+new_page+serve_dir | ✗ | 浏览器自动化（无头 Chromium，node 桥接；需宿主机 node + playwright 包 + 浏览器） |
| `reverse_site` | 浏览器自动化全套（同 playwright）+ http_request/fetch_url/capture_start/capture_stop/capture_clear/capture_list/read/write/agent_list/agent_load/agent_run | 浏览器交互类+http_request+write | ✗ | 网站/接口逆向（浏览器网络录制还原接口、直连探测验证、产出 API 文档；可联动 self_optimize 转新子Agent；需宿主机 node + playwright 包 + 浏览器） |
| `cron` | add/list/update/remove（→ `cron_add`/`cron_list`/`cron_update`/`cron_remove`） | add+update+remove | ✗ | 定时任务管理（自全局 cron_* 下沉：创建脚本运行/提示词运行 agent 的无人值守任务、查看/修改/删除；仅 `GEBAI_CRON_ENABLED=true` 时注册，关闭时完全不可见） |

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
| 子Agent 定义 | `packages/server/src/sub-agents/`（单文件 `*.ts` 或目录 `{name}/{name}.ts` + `{name}.md`） | 新增/修改子Agent（名称、描述、提示词、工具） |
| 全局工具实现 | 服务端源码 | 工具行为优化、新工具开发 |
| 系统提示词模板 | 服务端源码 | 总Agent/子Agent 提示词迭代 |
| 默认配置 | 环境变量默认值/常量 | 阈值、超时、默认行为的调优 |

#### 优化闭环

```
发现改进点（用户反馈/失败案例/日志分析/自身复盘）
    → 生成修改方案（改动点清单 + 预期效果）
    → 审批（需用户确认，高风险改动用/approval）
    → 由 self_optimize 子Agent 修改代码（专用子Agent，见下）
    → 运行测试验证（bun test，失败则继续修复）
    → 设计变更回写 DESIGN.md（修改行为/接口/协议/存储布局/常量/命名规则等设计层面时同步对应章节，文档与代码保持一致）
    → 用户验证（ask_user 确认方式）：UI/前端类修改 → page_capture 捕获当前页面
      （模型 read 实际渲染 html + vision 分析截图，dev 模式自动热更新，先请用户刷新页面）；
      服务端功能类修改 → preview_server 临时新端口验证服务（独立进程不中断当前会话，验证后停止）
    → 变更落盘：代码变更即持久化，无需额外记忆
    → 构建/重启生效（开发模式直接生效）
```

#### `self_optimize` 专用子Agent

自我优化由独立子Agent `self_optimize` 承担（与 `code` 拆分，见「职责边界」）。**工具与提示词直接复用 `code`**——def 只声明 `code` 没有的独有能力（反馈读取、测试准入、回滚、页面捕获、视觉分析），通用编码能力（文件/分析/修改/验证工具与「规划→探索→定位→方案→修改→验证→收尾」工作流）由连带装载/预加载的 `code` 提供（验证服务 `preview_server` 亦经 code 的 `code_preview_server` 获得），**不重复注册工具、不复刻提示词**：

```ts
export const name = "self_optimize"
export const description = "优化歌白自身（涉及本 Agent 自身代码/子Agent/提示词/配置时加载）：改进定义、修复缺陷、验证修改。输入：改进点/失败案例/反馈（可经 self_optimize_read_feedback 工具读取用户反馈）；输出：代码修改方案与验证结果；修改必须通过相关测试（测试是准入凭证，run_tests 工具）并同步 DESIGN.md，测试失败可 rollback 回滚。不处理外部项目（外部代码用 code）。"
export const tools: ToolSet = { read_feedback, run_tests, rollback, page_capture, vision }
export const requiresApproval = { run_tests: true, rollback: true }
export const preload = false          // 按需装载，非默认注入
export const writeGuard = (env, absPaths) => string | null   // 写范围守卫声明（见下「写范围守卫」）
```

- **复用 code（两种路径同规则）**：
  - **装载模式**：装载 `self_optimize` 时 `SubAgentManager.load` **连带装载 `code`**（幂等，WS `sub_agent.load`/`agent_load`/预加载所有装载路径均生效）——code 完整提示词写入会话记录，文件/分析类操作直接用 `code_*` 工具（带 project 参数，可按名操作预置项目）
  - **新会话执行（`agent_run`）**：`runNewSession` 预加载 `self_optimize` 时**自动连带预加载 `code`**（code 前置，系统提示词含两段职责分隔头——code 的通用工作流在前，self_optimize 的特有流程与约束在后），新会话工具集为 `code_*` 通用工具 + `self_optimize_*` 独有工具
  - 提示词分层：self_optimize 静态提示词**只承载自我优化特有内容**（反馈输入、写范围、设计同步铁律、测试准入/回滚、用户验证、git 收尾），通用工作流以「直接遵循 code 子Agent 提示词」引用（两种路径下 code 提示词均在上下文内）——符合「子Agent 静态提示词不复刻其他子Agent 内容」的分层原则
- **反馈读取（`self_optimize_read_feedback`）**：自全局工具集下沉（全局不再注册 `read_feedback`，自我优化为唯一消费方）——按用户反馈按时间倒序读取，声明进 def 同时覆盖装载模式（`self_optimize_read_feedback` 命名空间）与新会话执行环境（全局工具不在注册表，def 声明保证可用）；反馈是自我优化的核心输入通道
- **页面捕获（`page_capture`）**：仿 draw 的前端配合链路——引擎发布 `event.capture.request`（含 captureId + fullPage + delay）→ 前端捕获**当前浏览器页面**（渲染后 DOM html 截断 300KB + modern-screenshot 截图，png/jpeg，体积压缩 ≤2MB；fullPage=true 截整页，高度上限 12000px，缺省视口；**delay 为捕获前等待毫秒数**（UI 操作/动画/异步渲染完成后截图，上限 10 秒，前端 sleep 后统一捕获 html 与截图））经 WS `capture.result` 回传 → 服务端落盘会话 `tmp/capture/`（`page-<ts>.html` + `page-<ts>.png|jpg`）并返回文件/图片内容块；模型用 `read`（code_*）读取实际渲染 html、用 `vision` 分析截图——**UI 修改后模型直接看到真实渲染效果**（dev 模式修改后自动热更新，捕获前提示用户刷新页面；30 秒超时返回失败提示）
- **视觉分析（`vision`）**：与主 Agent 同一 provider 解析逻辑——组装层（`index.ts`）注册 `setVisionProviderGetter`，`GEBAI_VISION_*` 外挂视觉模型 → 多模态主模型回落；子Agent 定义经 `getVisionProvider` 构造工具
- **写范围守卫（`SubAgentDef.writeGuard`，代码级强制而非仅提示词）**：def 声明 `writeGuard(env, absPaths)`，引擎注入 `ToolContext.writeGuard`——文件写类工具（`write`/`edit`/`patch`/`file`（rename/move/delete 动作，move/rename 校验源与目标两路径））写入前以**解析后的绝对路径**调用，返回非空字符串即拒绝（作为工具结果返回引导模型调整，不抛错不落盘）。**装载模式按会话装载名单动态收集**（`sessionWriteGuard`：调用时点读会话 `loadedSubAgents`，任务中途 `agent_load` 装载后立即生效）、**新会话模式按预加载名单静态组合**（`defsWriteGuard`）——两个路径一致生效（旧实现仅新会话路径有守卫，装载路径因工具去重丢失守卫副本，现已修复）。政策内容：**默认只读模式仅允许写入 子Agent 目录（`packages/server/src/sub-agents/`）与仓库级文档/配置（`DESIGN.md`/`AGENTS.md`/`.env.example`/`README.md`/`kilo.json`）**，核心引擎源码（`core/`/`app`/`ws` 等）拒绝写入（返回拒绝说明引导改用子Agent 扩展或开启开关）；`GEBAI_SELF_MODIFY=true`（启动级环境变量）放开到仓库内任意路径；仓库根解析：`SELF_OPTIMIZE_PROJECT` 优先，dev 模式按模块路径推导，二进制模式必须显式配置；**守卫只保护歌白仓库**——仓库根之外的常规写入（会话 `tmp/` 产物等）不受限（守卫目的是保护服务端源码，不约束无关产物）
- **测试准入 + 回滚工具**：`run_tests`（在仓库根执行 `bun test` 指定文件或 `bun run test` 全量，需审批）——测试是自我修改的唯一准入凭证；`rollback`（`git checkout --` 指定路径或全部，需审批）——测试失败后的恢复路径；code_* 写类工具的审批由 code 的 `requiresApproval` 声明承接（edit/write/patch），sh/py 走工具自身动态审批（默认需审批、`approval` 参数按次免审，见「工具审批」）
- **验证服务（`preview_server`，经 code 命名空间 `code_preview_server` 使用）**：临时新端口独立进程（不中断当前会话），用于服务端功能类修改的验证；UI/前端类修改优先走 `page_capture` 当前页面验证
- **工作流**：通用段（规划→探索→定位→方案→修改→验证→收尾）直接遵循 code 提示词（探索段 grep/analyze/search_symbols 分工、修改段 patch 优先等见「code」章节）；特有段：反馈输入（`self_optimize_read_feedback`）→ **设计同步铁律**（任何行为/接口/协议/存储布局/常量/命名规则等设计层面变更，必须同步更新 `DESIGN.md` 对应章节）→ 修改（范围由写范围守卫代码级强制）→ 验证（`run_tests` 测试准入，失败 `rollback` 回滚，再 typecheck/lint）→ 用户验证（UI 类 `page_capture`、服务端类 `preview_server`）→ 收尾（`git` 只读核对、不擅自 commit、总结先结论后细节）
- **与 `code` 的差异**：
  - 工作区：服务端源码（`packages/server/src/sub-agents/` 等，`GEBAI_SELF_MODIFY` 开启时更宽） vs 会话 `tmp/` 外部项目
  - 审批：自我优化改动影响所有用户，默认更严格（改动子Agent 定义即影响全局能力）
  - 预加载：`preload = false`，避免普通对话中总Agent 误用自我修改能力
- **协作边界**：`self_optimize` 可经 `agent_run` 委托 `playwright` 做外部 URL（如 `preview_server` 页面）的浏览器验证；`code` 不得反向委托 `self_optimize`（防经子Agent 链间接获得服务端修改能力），**写权限仅限服务端允许范围**

- **改进点来源**：用户反馈（见「用户反馈」）、审批拒绝原因、工具执行失败、任务超时/中断日志、用户显式指令（如「你下次不要再…」）
- **变更管理**：每次自我修改生成补丁记录（改动前/后、原因、验证结果），可查看、可回滚；代码版本控制（git）即优化历史
- **生效方式**：
  - 脚本调试模式：修改源码后**重启进程生效**（子Agent 启动时扫描发现，无运行期热加载）
  - 二进制模式：修改后的代码进入下次构建；运行期通过环境变量覆盖提示词/配置（会话级）实现即时调优
- **安全约束**：自我修改走统一审批流（写操作逐次审批）；修改范围由**写保护闸门代码级强制**——默认只读模式仅限子Agent 目录与仓库级文档/配置，核心引擎源码默认只读（`GEBAI_SELF_MODIFY=true` 显式开启，见「写保护闸门」）
- **测试门槛**：任何自我修改必须通过相关测试（`run_tests`）才能视为完成，防止退化；测试失败用 `rollback` 回滚本次改动（测试策略与覆盖率门槛见「测试策略」）
- **用户验证**：修改通过测试后，`self_optimize` 用 `ask_user` 询问用户是否启动验证服务（`preview_server`，临时新端口独立进程，不中断当前会话），确认后启动并告知访问 URL；验证结束（或用户拒绝）即停止，避免残留进程

### 会话管理
- 会话按用户持久化到 `{GEBAI_HOME}/users/{user}/sessions/{s0}/{s1}/{session_id}/chat.json`（`{s0}`/`{s1}` 为会话 ID 哈希前缀分片，见目录结构）
- **会话记录保存子Agent 装载状态**：`loadedSubAgents` 字段（已装载名单）+ `loadedAgent` 标记的 system 消息（完整提示词，UI 渲染为简短装载提示）；恢复历史会话时引擎自动按名单重新注册工具（`ensureSessionAgents`，幂等）——会话按保存的文件完全恢复状态；新会话首次运行按启动预载名单（`GEBAI_PRELOAD_SUB_AGENTS`）初始化，未配置默认不预载任何子Agent
- 会话归属校验：仅会话所有者可访问（服务模式）
- 列表查询时基于消息内容的哈希去重
- 支持会话创建、切换、重命名、删除
- 支持会话级的审批跳过（`/approval-skip`）

### 用户反馈

用户可对任意 Agent 回复（总Agent/子Agent 消息）提交反馈，用于质量追踪与改进：

- **反馈类型**：
  - **点赞/点踩**：对单条回复快速评价，附可选原因标签（错误/不完整/不符合预期/优秀等）
  - **文字反馈**：附带详细说明，如指出错误、期望的修正、补充背景
  - **建议改进**：对功能/产品层面的建议（新工具、子Agent 需求等）
- **关联上下文**：反馈自动关联所属会话、消息 ID、使用的模型与子Agent，便于定位问题
- **存储**：持久化到 `{GEBAI_HOME}/users/{user}/feedback/YYYY-MM-DD/{h0}/{h1}/{feedback_id}.json`（按日期+哈希分片），仅本人可见（服务模式）
- **前端**：消息内反馈按钮（👍/👎）已移除；反馈数据经后端 API 写入，管理员可在设置面板查看/导出
- **用途**：反馈数据供改进系统提示词、评估模型表现、排查工具问题；可导出分析（管理员）
- **匿名选项**：服务模式下提交时可选择匿名（仅保留反馈内容与时间，不关联用户）

### 环境变量配置

所有配置统一采用**环境变量**形式（`GEBAI_*` / `OPENAI_*` / 子Agent `{AGENT_NAME_UPPER}_*` 等），无独立配置文件。**用户/会话环境变量服务端零留存**（不落任何 env 文件），层级与存储：

| 层级 | 来源 | 存储 | 说明 |
|------|------|------|------|
| 全局 | 进程环境变量（启动时注入，`.env`/系统环境） | 服务端启动配置 | 服务端默认配置，所有用户/会话共享 |
| 浏览器本地 | 前端设置面板 | **浏览器 localStorage（`gebai.ui.env`）——用户环境变量唯一持久化位置** | 随每条 prompt 临时注入为任务级覆盖，清除站点数据即清除 |
| 会话内存态 | env 接口 / 飞书命令运行中设置 | **服务端内存（不落盘，进程重启即空）** | 仅供运行中即时生效类开关（如自动审批、极简模式）；由前端每次加载会话自行重新同步所需键 |

**admin 密码引导**：服务模式不设注册表引导（**不落明文密码**）——启动参数 `GEBAI_ADMIN_PASSWORD_HASH="salt:hash"`（scrypt 加盐哈希，`bun run --cwd packages/server hash-password` 生成）**设置则启用 admin（覆盖其哈希），不设置则 admin 被禁用**——admin 唯一入口即此参数（不可注册创建）；admin 禁用不影响普通用户（登录页可自助注册，注册用户恒为普通角色）。

#### 前端环境变量（浏览器本地，不保存到服务端）

- **前端设置面板配置的环境变量保存在浏览器本地（localStorage，键 `gebai.ui.env`）**，对本浏览器所有会话生效，不是每个会话单独配置，**不写入服务端**（用户环境变量服务端零留存——不落任何 env 文件）——敏感配置（密钥等）只存在用户自己的浏览器，防服务端侧泄露
- 发送消息时由前端随 `POST /api/v1/sessions/:id/prompt` 请求（body `env`）/ WS `session.prompt`（payload `env`）**临时注入**服务端，仅本次任务生效（引擎合并进本次运行的 env），请求结束即失效，服务端不持久化
- **前端配置模型相关变量即可使用模型**（服务端可不配置任何模型变量）：`GEBAI_LLM_MODEL`（主模型）/`GEBAI_VISION_MODEL`（视觉模型）等 `GEBAI_LLM_*`/`GEBAI_VISION_*` 注入后按任务生效——任务启动时按合并后 env 重建 Provider 覆盖服务端启动配置（见「覆盖规则」），未配置的项沿用服务端启动配置
- 名称与权限宽容过滤：prompt 的 `env`（浏览器本地注入通道）对不支持/非法的变量**直接跳过、不拒绝任务**（`filterEnvInjection`：丢弃非法标识符名/`__proto__`/非 string 值）——前端 localStorage 可能残留旧版目录外键，拒绝整个任务会阻断正常使用；显式管理通道（REST `PUT /env`、WS `session.env.set`）仍严格校验（`validateEnvVars` 非法返回 400/错误应答）
- **目录驱动、不可自定义**：前端设置面板从服务端 `GET /api/v1/env/catalog` 拉取环境变量目录（白名单，含变量作用说明），**按「全局 / 各子Agent」分组展示**（全局：模型相关 `GEBAI_LLM_*`/`GEBAI_VISION_*`、审批跳过、代理/时区等；子Agent 组：`{AGENT_NAME_UPPER}_*` 前缀变量如 `CODE_PROJECTS`/`SELF_OPTIMIZE_PROJECT`/`FEISHU_DOCS_APP_ID`）；**子Agent 组由各子Agent 导出 `envVars` 声明汇总**（`core/env-catalog.ts` 不做子Agent 硬编码，新增子Agent 环境变量只需在子Agent 定义中声明，见「子Agent文件格式」）；**未配置的项显示为空、请求不携带**；鼠标悬停（tip）显示变量作用
- **不支持自定义变量名**：只能配置目录内的项（保存时过滤目录外旧值）；启动级与安全敏感变量（`GEBAI_MODE`、`GEBAI_ADMIN_PASSWORD_HASH`、`GEBAI_SAFE_MODE`、`GEBAI_SANDBOX`、`GEBAI_HOST/PORT` 等）不在目录中，天然不可配置
- 清除浏览器站点数据（localStorage）即清除该配置

#### ask_env 工具（模型驱动的前端填值）

- **工具缺少必需环境变量时报错给模型**（错误信息含缺失变量名、用途说明与可选的 `ask_env` 调用引导，如 feishu_docs 缺 `FEISHU_DOCS_APP_ID` 时提示「可调用 ask_env 工具（name=FEISHU_DOCS_APP_ID，secret=true）请求用户直接填写」）
- **`ask_env` 工具**：模型调用（`name` 变量名 + `description` 用途说明 + `secret` 敏感值掩码）→ 引擎发布 `event.env.request`（含 envId/name/description/secret）并**阻塞等待** → 前端渲染**填值卡片**（变量名与说明展示，secret 时密码框）→ 用户提交后：
  - **提交成功填值卡片随即关闭**（取消/拒绝同样关闭）；提交失败卡片保留并提示重试
  - 值**注入本次任务 env**（任务级 env 引用原地更新，`sh`/`py` 子进程与后续工具读取**立即生效**，不进入模型上下文——密钥不外泄给模型）
  - **变量名校验与敏感键限制**：与其余 env 写入通道同规则（`validateEnvVars`：仅 `[A-Za-z_][A-Za-z0-9_]*` 标识符名、拒绝 `__proto__` 原型污染），**服务模式下一律拒绝 `GEBAI_APPROVAL_SKIP`**（ask_env 是模型驱动的第四通道，不得自设审批跳过——用户本人经前端开关/env 接口/飞书命令设置）
  - 同时**保存到浏览器本地**（localStorage，后续任务自动生效）
  - 用户拒绝/超时（审批超时同值）返回失败，工具结果引导模型说明所需配置或改用其他方式
- 声明 `interaction: "realtime"`（需前端填值弹窗），无交互/多轮交互模式自动禁用；值与其余 env 写入同规则校验（`validateEnvVars`）

#### 会话内存态环境变量（内部机制 + API）

- 会话 env 为**纯内存态**（服务端零留存）：不落盘、进程重启即空，前端每次加载会话**与每次任务启动前**自行重新同步所需键（如自动审批开关、极简模式开关——草稿页首条消息创建的会话不经过消息加载路径，同步点覆盖两处，防新会话首个任务的开关失效）
- 会话级修改**不通过前端设置面板**（面板存浏览器本地），仅经 API（REST `PUT /api/v1/sessions/:id/env` / WS `session.env.set`，内存写入）或内部机制（如 `approval-skip` 自动审批开关按会话同步 `GEBAI_APPROVAL_SKIP`）；**写入响应与读取一致脱敏**（敏感键值以掩码返回，防明文密钥回读）
- 作用范围：会话内 LLM 调用（模型/Provider 配置，任务级生效）、工具执行（`sh`/`py` 子进程环境）、子Agent 环境变量读取，均与浏览器本地注入合并后生效
- 用途：按会话定制（`CODE_PROJECT`/`SELF_OPTIMIZE_PROJECT` 项目绑定、`CODE_PROJECTS` 预置项目注册表、预加载集合覆盖等；服务重启后此类配置由用户浏览器本地随 prompt 重新注入恢复）
- **历史数据清理**：启动时清理遗留的用户级 `users/{user}/env.json`（`cleanupLegacyUserEnv`）；会话目录遗留的 `env.json` 在该会话首次触达 env 读取时惰性删除——迁移后服务端不留存任何 env 文件
- **环境变量目录接口**：`GET /api/v1/env/catalog` 返回可配置变量白名单（按「全局 / 各子Agent」分组 + 变量作用说明），供前端设置面板渲染（不可自定义变量名）；启动级与安全敏感变量不在目录中

#### 覆盖规则

- 生效顺序：**浏览器本地注入（本次任务） > 会话内存态 > 全局**，同名字段取最高优先级的非空值（前端 localStorage 注入仅覆盖当前运行的任务，不修改任何持久化层级）
- **模型相关配置（`GEBAI_LLM_*` 全套与 `GEBAI_VISION_*`）任务级生效**：浏览器本地/会话内存态注入在任务启动时按合并后 env 重建 Provider（`applyModelEnvOverrides`/`resolveVisionProvider`），覆盖 Provider 级（进程环境变量）配置——主循环与 `agent_run` 新会话执行、上下文压缩阈值/摘要、附件图片内联判定、vision 工具均生效；无覆盖键时沿用启动 Provider 实例；非法值（API_KIND 非三类/MAX_CONTEXT 非正数）忽略回退
- 会话内存态删除某变量 = 恢复为全局的值
- 修改环境变量（任一来源）后，当前正在运行的任务不受影响，新任务使用新值（运行中的会话内存态开关除外——自动审批等实时判定类按次读取）
- 敏感变量（含密钥的 `*_KEY` / `*_TOKEN` 等）在服务端 API/UI 中脱敏显示，仅可覆盖不可回读明文；浏览器本地存储的值存于用户自己的浏览器，按明文编辑

### 会话临时文件查看与下载

会话 `tmp/` 中的文件（Agent 产出、脚本输出、截断内容引用等）支持在 UI 中查看与下载：

- **文件列表**：UI 提供会话文件面板，按目录树展示 `tmp/` 下全部文件（路径、大小、修改时间），可刷新
- **查看**：文本文件（文本/JSON/代码）内嵌预览，超过预览阈值（默认 100KB）截断提示；二进制文件（图片等）可预览，其余提示下载
- **下载**：单文件下载、多选打包下载（zip）；通过 REST 下载端点返回原文件（`Content-Disposition` 指定文件名）
- **安全边界**：文件操作严格限定在会话 `tmp/` 内，路径解析复用路径沙箱（拒绝 `../`、绝对路径、符号链接），仅会话所有者可访问；**列表仅暴露 `tmp/` 子树**（`chat.json`/`cron.json`/`todo.json` 等会话数据文件不列出），REST/WS 文件接口的路径解析统一以 `tmp/` 为根并兼容 `tmp/` 前缀（旧附件/截断引用路径）
- **与截断内容联动**：上下文保护落盘的截断文件也可在 UI 中直接查看/下载
- **用途**：用户随时检视 Agent 工作产物（生成的报告、脚本、数据文件），无需进入文件系统

#### 接口

- WS：`session.files.list` / `session.files.get`
- REST：`GET /api/v1/sessions/:id/files`、`/files/content`（?path=）、`/files/download`（?path= 单文件 / POST body {paths} 多选 zip 打包）
- SDK：`listSessionFiles(sessionId)` / `readSessionFile(sessionId, path)` / `downloadSessionFile(sessionId, path)` / `downloadFilesZip(sessionId, paths)`

### 日志系统
- 日志级别：`debug`、`info`、`warn`、`error`
- 通过环境变量 `GEBAI_LOG_LEVEL` 配置（默认 `info`）
- 日志写入 `{GEBAI_HOME}/gebai.log`
- 日志脱敏：不记录密码、令牌、密钥明文，敏感字段以 `***` 替代；会话内容默认不落日志

### 数据生命周期

清理任务（`core/gc.ts`）随服务端启动执行一次，之后每日周期运行，失败不影响在线任务；可用 `GEBAI_GC_DISABLED=1` 关闭：

- **垃圾回收**：`trash/` 中删除/过期的会话超过保留期（默认 7 天）自动物理清理（按归档日期目录 `trash/YYYY-MM-DD/` 判定）
- **截断文件**：随会话 `tmp/` 整体清理——会话过期归档到 `trash/` 时一并归档，超保留期随 `trash/` 删除（不再单独设置截断文件保留期）
- **反馈数据**：`feedback/` 按日期分片，超过保留期（默认 180 天）自动清理，管理导出不受影响
- **临时文件**：会话 `tmp/` 在会话删除或会话过期（默认 90 天无活跃）时整体清理（含截断文件、附件、产物）
- **会话过期策略**：本地/服务模式均可配置会话闲置过期时间（默认 90 天，按 `chat.json` 最后修改时间判定），过期会话归档到 `trash/` 后可恢复，超保留期删除；**归档会话自助恢复通道**：`POST /api/v1/sessions/:id/restore`（WS `session.restore` 同权）——归属用户或 admin 可把 `trash/{date}/{id}` 目录整体移回分片存储位置（数据/tmp 附件/env 一并恢复），会话已存在返回 409，未归档/无权限统一 404（不泄露他人会话存在性）
- 历史遗留的用户级 `truncated/` 目录（截断文件并入会话 `tmp/` 前的旧数据）由 GC 一次性迁移清理

### 工具审批
- 全局工具：`sh`、`py` 默认需要审批；`read`/`write`/`edit` 默认无需审批（`file` 的 `delete` 动作**动态需审批**——递归且不可恢复，能力上甚于一次 `sh rm`）
- **免审白名单强制（`approval:false` 不再无条件生效）**：`sh` 的免审标记经服务端强制校验（`shApprovalFreeAllowed`）——只读命令（`validateShCommandSafeMode` 全量解析通过）或测试/静态检查/包管理器 test 子命令（`bun test`/`npm run <脚本名>`/`pytest`/`tsc`/`eslint`/`go test` 等，`run` 仅脚本名形态、禁文件直跑）放行免审，其余（含 `curl`/`wget`、命令替换、输出重定向、无法识别结构）一律仍走审批——防提示词注入诱导模型自行声明免审执行任意命令；**安全模式例外**：sh 在工具内按只读白名单降级（风险已由白名单约束），免审标记直接生效；**`py`/`js` 的 code 为任意代码**：`py` 免审标记不生效（恒需审批）；`js` 按词元扫描放行——纯数据加工/工具编排代码（无 `fetch`/`WebSocket`/`Bun.*`（除 `Bun.file`）/`process.env`/动态 import 等网络外发、进程、敏感读取通道）免审生效，含上述通道仍需审批（fail-closed，字符串误报只是多一次审批）
- 子Agent工具：通过 `requiresApproval` 声明（含对 `edit`/`write` 等全局工具的按需收紧；静态 `true` 会覆盖工具自身的函数形态声明，`code` 因此不在映射中静态声明 `sh`/`py`——保留工具自身动态判定以支持按次免审）；`cron` 子Agent 的 `cron_add`/`cron_update`/`cron_remove` 默认需要审批（定时任务 = 无人值守执行，见「定时任务」）
^- `flow`：是否需要审批取决于编排内调用的工具（**动态审批机制**：`Tool.requiresApproval` 支持函数形态 `(args, ctx) => boolean`，引擎在审批点解析（函数异常按需审批 fail-safe）；flow 的函数递归扫描全部步骤，任一工具需审批则整个 flow 提交一次审批，见「flow 数据流编排工具」；步骤 params 中的 `approval: false` 同样生效——全部步骤免审时 flow 整体免审）
- 会话级跳过：`/approval-skip` 命令；**会话运行中开启即时生效**——引擎审批点实时判定（任务 env 快照或会话内存态 env 任一为 `true` 即跳过，前端开启时自动通过当前等待中的审批卡片，后续审批直接跳过；关闭需下次任务生效）；**用户本人可设置自己的会话**（`GEBAI_APPROVAL_SKIP` 写入会话内存态 env，不落盘——非管理员仍受路径/脚本/网络沙箱完整约束；ask_env 模型驱动通道服务模式下一律拒绝）
- 同一消息最多重试 10 次

### 工具选择

工具集支持**全局开关控制**，可按需启用/禁用任意工具（含子Agent 工具），多级配置：

| 层级 | 配置 | 说明 |
|------|------|------|
| 全局 | `GEBAI_TOOL_ENABLE` / `GEBAI_TOOL_DISABLE`（逗号分隔） | 进程启动时生效，所有用户/会话共享 |
| 会话 | 前端「极简模式」开关（`GEBAI_MINIMAL_MODE` 会话 env） | 仅当前会话生效，任务启动时按 env 快照裁剪 |

- **白名单优先**：`GEBAI_TOOL_ENABLE` 声明后仅启用列表内工具；`GEBAI_TOOL_DISABLE` 排除指定工具；两者同时配置时先白名单后黑名单
- **粒度**：全局工具按名称（`sh`/`read`/`edit`…）；子Agent 工具按 `{agent_name}_{tool_name}` 精确控制，也可按 `{agent_name}_*` 整包禁用
- **禁用效果**：禁用后工具从总Agent schema 中移除，模型不可见、不可调用；已装载子Agent 中被禁用的工具同样不注入
- **前端支持**：工具管理界面提供开关列表（分组：全局/子Agent）、搜索与即时生效，保存后新任务生效
- **用途**：安全收紧（如生产环境禁用 `sh`/`py`）、裁剪上下文、按业务场景定制能力面
- **权限**：REST `PATCH /api/v1/tools` 与 WS 工具启停为**服务端全局状态**（所有用户/会话共享），服务模式下仅管理员可操作（普通用户返回 403）；环境变量级（`GEBAI_TOOL_ENABLE`/`GEBAI_TOOL_DISABLE`）由部署方配置不受此限
- **极简模式（会话级）**：前端会话列表底栏「极简模式」开关（localStorage 持久化，与自动审批开关同机制）——开启/关闭均幂等同步当前会话内存态 env `GEBAI_MINIMAL_MODE`（会话加载时与任务启动前同步，服务端零留存、重启后由前端重新同步）；引擎**任务启动按 env 快照**裁剪工具白名单：仅 `sh`/`edit`（外加 `full_mode` 切换入口）保留（schema 过滤 + 执行阻止，主循环与 agent_run 新会话循环统一生效），其余工具模型不可见，被调用时返回「会话处于极简模式」说明（含 `full_mode` 切换指引）；**系统提示词同步极简化**——仅保留身份、路径基准与极简工具说明，裁剪编排/任务路由/子Agent 清单等段落（对应工具均不可用，注入纯属浪费上下文）；**下次任务起生效**（任务级快照，运行中切换不打断当前任务）；与全局白/黑名单、通道禁用叠加生效（取并集更严）；用途：极限裁剪上下文与工具面（只用 shell + 文件编辑的轻量会话、弱模型稳定工具选择）
- **`full_mode` 切换完整模式（极简会话专属工具）**：极简模式下任务确需其他工具能力时，模型调用 `full_mode`（**需用户审批**，可选 `reason` 参数说明原因供审批参考；仅极简会话可见可用，完整模式会话从 schema 移除）——批准后：① 当前任务工具白名单清除（下一轮 schema 立即全量下发，本任务内即时生效）；② 主循环系统提示词**原地升级为完整版**（首条 system 与极简参照全等才替换，agent_run 新会话提示词不受影响）；③ 会话内存 env 极简标记删除（后续任务不再极简）；④ 发布 `event.session.minimal {enabled:false}` 通知前端关闭本地开关（防下次任务前幂等同步把极简标记写回，开关为用户级单份、不区分来源会话）。被拒绝则继续用 sh/edit 完成。REST 无交互通道（服务模式）下需审批工具自动拒绝，无人批准即无法切换

### 交互模式

引擎按接入通道区分三种**交互模式**（`engine.run` 的 `interactionMode` 参数，任务级生效），工具通过 `Tool.interaction` 声明**最低可用模式**（缺省 `none` 即全模式可用）；声明高于当前模式时工具被自动禁用（schema 过滤 + 执行阻止，模型收到「当前通道不可用」说明并改用其他方式），替代旧的「等超时」体验：

| 模式 | 语义 | 通道 | 可用工具 |
|------|------|------|----------|
| `none`（**无交互**） | 单次请求：一次调用执行完返回结果，无前端、无往返；**本地模式需审批工具自动通过**（无人可询问）；**服务模式需审批工具直接拒绝**（防普通用户经 REST 免审批执行敏感工具） | REST `POST /sessions/:id/prompt` | 仅 `none` 声明（文件/网络/脚本等） |
| `multi_turn`（**多轮交互**） | 多轮请求-响应往返（非流式），有往返但无前端页面；**仅关键操作（requiresApproval）询问用户**（如飞书审批回调卡片），非关键操作自动 | 飞书机器人 | `none` + `multi_turn` 声明（ask_user/draw 已有按钮/后端渲染适配） |
| `realtime`（**实时交互**） | 实时流式交互：完整前端，关键操作询问用户 | WebSocket（Web UI，默认） | 全部 |

- **工具声明**（`interaction: "realtime"` 仅实时前端）：`page_capture`/`render_html`/`ask_env` 与 widgets 四工具（`save`/`list`/`get`/`delete`，依赖前端页面配合，ask_env 需填值弹窗）；`interaction: "multi_turn"`（至少多轮交互）：`ask_user`/`plan`/`draw`（飞书通道已有选择卡片/后端渲染适配）；其余工具缺省 `none`
- **审批策略按模式分级**：无交互模式 `isApprovalSkipped`——**本地模式恒真**（`sh`/`py`/`write`/`edit` 等需审批工具**自动通过**，任务不会卡在审批等待）；**服务模式返回拒绝**（需审批工具在审批点直接拒绝执行，返回「需审批但当前通道无交互」说明，不进入等待——REST 无人可审批，普通用户不得借此免审批执行 shell/定时任务；管理员可经正式通道设置 `GEBAI_APPROVAL_SKIP` 后执行）；`sh`/`py` 的 `approval:false` 按次免审**只作用于交互审批**，服务模式无交互通道按剥离免审标记后的默认审批姿态照常拒绝（引擎 `stripApprovalFlags` 递归删键解析，防模型自行声明免审绕过硬门槛）；多轮交互模式关键操作（requiresApproval）经审批卡片询问用户（飞书回调卡片），非关键操作不打扰；实时交互模式维持询问用户（前端审批卡片）
- **飞书通道** = `interactionMode: "multi_turn"`：realtime 声明的 5 个工具自动禁用（原 `FEISHU_DISABLED_TOOLS` 名单已移除，由声明统一驱动），`ask_user`/`draw` 可用（走飞书交互卡片/后端渲染），关键操作经审批卡片询问
- **REST 通道** = `interactionMode: "none"`：`ask_user`/`draw` 及实时前端工具自动禁用，不再等待至超时；本地模式需审批工具自动通过，**服务模式需审批工具直接拒绝**（防免审批执行）；需要完整交互能力请走 WS 通道
- 禁用判定同时匹配子Agent 命名空间工具（`{agent}_page_capture` 等同名工具同样禁用）；与 `disabledTools` 名单（部署方可另行指定）叠加生效

#### 输出方式（与交互模式正交，同样请求层配置）

**交互模式与输出方式均为请求层配置，服务端引擎内部全部支持**，接入方按自身形式自由组合适配（如业务系统 REST 集成、IM 机器人、Web 前端互不影响）：

| 输出方式 | 语义 | 说明 |
|----------|------|------|
| `final_only`（仅最终响应） | 不推送文本增量（`event.message.delta`）与推理流（`event.message.reasoning`） | 结构化事件（工具调用/审批/`event.message.done` 最终响应/新会话执行过程）仍推送；REST 同步响应即最终文本（content 纯正文，推理在消息独立字段 `Message.reasoning`） |
| `streaming`（流式输出，默认） | 推送文本增量与推理流（含新会话执行过程文本） | Web 前端打字机效果 |

- 引擎 `engine.run` 的 `outputMode` 参数（默认 `streaming`，保持现有 Web/飞书行为）；`final_only` 下文本仍完整落盘会话消息（接入方经存储/最终响应获取完整内容）
- **请求层暴露**（默认值保持各通道现状，传入即覆盖）：
  - REST `POST /api/v1/sessions/:id/prompt`：`interactionMode`（默认 `none`）+ `stream`（默认 `false`=仅最终响应；`true`=流式输出，接入方经 WS 事件订阅消费流）
  - WS `session.prompt`：`interactionMode`（默认 `realtime`）+ `stream`（默认 `true`=流式输出；`false`=仅最终响应）
  - 飞书通道经**接口层**（`BotPromptAdapter`/`EngineBotAdapter`）固定 `multi_turn` + `final_only`（多轮交互 + 仅最终回复）：bot 不直接接触引擎/事件总线，任务内过程事件（工具调用/推理/文本增量）不推送，仅最终回复经回调发送；关键操作（审批/选择）与画图经回调询问/渲染
- 非法 `interactionMode` 值返回 400（REST）/ reply error（WS）

### 待办跟踪

复杂任务支持**待办清单（Todo）跟踪**，Agent 拆解任务、逐项推进，用户全程可见：

- **工具**：`todo`（待办增删改查统一入口——`entries` 为操作列表，每项 `op=add/update/delete`：add 需 `title`（可带 `priority`/`note`/`eta`），update/delete 按 `id` 或 `title` 定位（id 优先，无 id 时 title 精确匹配、唯一包含匹配兜底，多个同名提示改用 id；update 改标题需用 id），省略或空数组 = 查询；返回操作摘要与**当前全部待办状态**快照（含 id），模型一次掌握最新清单，无需再查 id）
- **状态机**：`pending → in_progress → completed`，异常终止为 `failed`/`cancelled`
- **字段**：标题、状态、优先级、进度（0-100%）、预计耗时（分钟）、依赖项、备注
- **持久化**：会话级 `todo.json`（`sessions/{s0}/{s1}/{session_id}/todo.json`），随会话隔离与恢复
- **Agent 引导**：系统提示词引导模型在复杂多步任务开始时先 `todo` 拆解计划（entries 批量建清单），每完成一步 `todo` 更新，任务结束 `todo`（空 entries）汇报
- **UI 展示**：会话消息流侧边栏实时呈现待办面板（状态/进度/依赖），事件推送 `event.todo.update` 驱动增量更新；`todo` 工具在消息流中渲染为**待办清单卡片**（状态图标 + 标题 + 元信息），替代通用工具卡片，实时与历史会话一致；清单过长（>8 项）时自动**折叠较早的已完成项**（保留最近 3 项完成作上下文，未完成项始终可见），连续隐藏段收敛为一行「已折叠 N 项已完成」按钮，点击展开、展开后可收起
- **与审批联动**：审批请求可关联待办项，拒绝/通过后对应待办状态联动更新
- **失败恢复**：任务中断后基于待办清单继续执行，跳过已 `completed` 项，从剩余项恢复
- **待办续做**：每轮会话完成（模型给出最终回复）后，引擎自动检查待办清单——仍有 `pending`/`in_progress` 项时，追加一条「【待办续做】…请继续执行，直至全部完成」消息并再次进入工具调用循环继续会话，直至待办全部完成或达到续做轮次上限（见常量参考，默认 3 轮）；`completed`/`cancelled`/`failed` 项视为已了结不再续做；续做消息持久化进会话历史（用户可见），并推送 `event.todo.continue` 事件

#### 选择工具（`ask_user`）

向用户提出一组选项并**阻塞等待用户回应**（方案确认、方向决策等场景），用户的回应作为工具结果返回给模型，据此继续执行：

- **参数**：`prompt`（问题描述）、`options`（选项数组，至少 1 项）、可选 `multi`（布尔，默认 false，true 为多选）
- **选项形式**：每项可为纯文本字符串，或**复杂选项** `{ title, description? }`（UI 按标题 + 说明展示，提交值取 title）
- **回应方式**（三种，均可）：① 点选选项（`multi=true` 时可勾选多项，提交值为选项集合）；② **输入自定义文本**（直接输入自己的答案，不限于给定选项，多选时追加到已勾选项一并提交）；③ **拒绝回答**（不再追问）
- **执行**：引擎发布 `event.choice.request`（携带 `choiceId`/prompt/options/multi）并**阻塞等待**；用户经 UI 选择卡片、WS `choice.decide` 或 REST `POST /sessions/:id/choice` 提交后，ask_user 工具返回结果注入模型，任务继续：
  - 选项或自定义文本 → 「用户选择：X」
  - 多选 → 「用户选择：X、Y、Z」（以「、」连接）
  - 拒绝回答（WS/REST `refuse: true` 或 option/options 均缺失，引擎侧传 null）→ 「用户拒绝了本次询问…」，模型停止继续询问、基于现有信息自行决策
- **超时**：等待超时（与审批同值，5 分钟）自动取消，返回降级提示让模型自行决策
- **UI 渲染**：审批容器中渲染**选择卡片**（问题 + 选项按钮/复杂选项行 + 自定义文本输入框 + 拒绝按钮；多选时选项可勾选、多一个「确认选择」按钮），提交即决策（实时，绑定 choiceId），**提交成功卡片随即关闭**（任务结束清理兜底）；**同一 choiceId 重复推送（断线重连事件 seq 重放）替换旧卡不堆叠**；历史裸调用（异常中断、无结果）回退可交互选择卡，提交降级为作为用户消息（「我选择：X」/「我选择：A、B」/「我拒绝回答」）发送
- **消息流问答记录卡（等待期不预览）**：ask_user 调用时**中断当前文本段**（封段），等待作答期间消息流**不渲染问题预览卡**——交互作答由审批容器选择卡片承载（与消息流展示卡上下堆叠会被视为重复卡片）；**结果到达时**在消息流落**问答记录卡**（问题 + 选项展示态 + 头部按结果文案呈现「✓ 用户回答 / ✕ 用户拒绝 / ⏱ 选择超时」+ 回答文本），模型后续回复另起新气泡——问答交换在会话流中完整可见；**历史重载**按持久化参数 + 结果同构渲染记录卡（带结果不再呈现为可交互的未答状态）；新会话执行过程（agent_run）内的 ask_user 同样封段、记录卡渲染进折叠容器
- **等待期不误判挂起**：选择/填值/画图/捕获等交互等待事件刷新前端流活跃时间——空闲超时兜底（150s 无数据，高于服务端 LLM 读空闲超时 120s：模型调用假死先由服务端超时上报明确错误，前端看门狗只兜底服务端检测不到的挂起）只按「无任何数据」判定，等待用户回应的挂起不计入（防误杀最长 5 分钟的选择/填值等待）；看门狗中止且无任何输出时渲染「生成超时」说明气泡，不再静默结束
- **长工具执行心跳**：工具执行期间引擎按 `TOOL_HEARTBEAT_MS`（默认 25s）周期发布 `event.tool.alive`（payload 含 `name`/`toolCallId`，主循环与新会话循环统一收口在 `runToolInterruptible`；快工具在首个周期前结束不产生事件，前端不渲染仅刷新活跃时间）——阻塞类工具（sh/py 跑构建/长测试等）执行期间无其他事件，无心跳会被前端空闲看门狗误判挂起取消（工具自身 timeout 尚未到点），心跳让看门狗与工具超时各司其职：看门狗抓真正的流挂起/断连，工具超时管进程执行上限

### 定时任务

会话内可直接创建**定时任务**，到点自动执行，适合周期性脚本与无人值守的 Agent 任务。能力由环境变量 `GEBAI_CRON_ENABLED`（默认 `false`）统一开关：**关闭时 `cron` 子Agent 不注册（`cron_*` 工具在工具表/schema、`agent_list`/`agent_load`/`agent_run` 中完全不可见）、调度器不启动**；开启后 `cron` 子Agent 可见、按需装载后所有会话可用（见「cron 子Agent」）。

- **两种任务类型**：
  - `script` **脚本运行**：执行 shell 命令（在会话 `tmp/` 目录以会话环境运行，超时同 `sh`/`py` 5 分钟），执行结果（成功/失败 + 输出）作为消息写入会话消息流（`[定时任务「名称」执行结果]`），历史可见、模型可感知
  - `prompt` **提示词运行 agent**：以指定提示词触发一次完整 Agent 会话（复用主循环，携带会话上下文与工具能力），过程与结果经正常消息流呈现
- **定时表达式**：5 段 cron（`分 时 日 月 周`，本地时区；支持 `*`/`*/n`/`a-b`/`a,b,c`，日与周均受限时任一命中）或 `@every <n>s|m|h|d`、`@daily`/`@hourly`/`@weekly`/`@monthly`；非法表达式创建/修改时即拒绝，永不触发的表达式（如 `0 0 30 2 *`）同样拒绝
- **工具**（`cron` 子Agent 命名空间暴露 `cron_add`/`cron_list`/`cron_update`/`cron_remove`，仅 `GEBAI_CRON_ENABLED=true` 时注册；装载 cron 子Agent 后可用）：
  - `cron_add`：创建任务（`type`+`schedule` 必填，`script`/`prompt` 按类型必填，可选 `name`/`enabled`），返回任务 ID 与下次执行时间
  - `cron_list`：查看本会话任务列表（ID/名称/类型/周期/启用状态/上次与下次执行/执行次数/最近错误）
  - `cron_update`：按 id 修改（启用状态/周期/类型/内容），修改后重算下次执行时间
  - `cron_remove`：按 id 删除
- **会话绑定**：任务经 ToolContext 绑定**创建它的会话**，`cron_list`/`cron_update`/`cron_remove` 均校验会话归属，跨会话不可见、不可操作
- **执行规则**：
  - 调度器每 30 秒 tick 检查；会话正有任务运行时 prompt 型跳过本轮（下次从当前时间重算，不并发写会话），脚本型可立即执行；**script 型重入防护**——单次执行可长达 5 分钟（超 tick 间隔）而 `nextRunAt` 在执行结束后才推进，执行中的任务在 `firing` 集合内标记，tick 重复触发同一任务直接跳过（无防护时长脚本被并发叠加执行、副作用重复）
  - 会话已删除的任务自动清理（含内存条目）；停机期间错过的触发不补跑（下次从当前时间重算）；**启动加载校验 schedule 合法性**——cron.json 被外部编辑改坏且任务 enabled 时直接禁用该任务并落 `lastError`（否则 nextTime 解析失败回退 +30s 会形成每 30 秒触发一次的热循环）
  - **执行失败（fire 抛错）同样从当前时间重算下次执行时间**（记录 `lastError`/`lastStatus=error`），防 nextRunAt 停留在过去导致每个 tick 无限重试
  - 任务记录保留上次执行状态/输出（输出限 4000 字符）；脚本输出写入会话消息限 8000 字符
- **存储**：会话级 `cron.json`（`sessions/{s0}/{s1}/{session_id}/cron.json`），随会话分片与清理（会话删除/过期即消失），服务端重启时扫描加载
- **安全**：`cron_add`/`cron_update`/`cron_remove` **默认需审批**（定时任务 = 无人值守的任意命令/会话执行，创建/修改/删除均须用户确认，服务模式下防普通用户绕过审批边界创建后门任务；`cron_list` 免审批）；脚本触发时以会话所属用户身份、会话 `tmp/` 工作目录与会话环境运行（与 `sh` 工具同隔离级别，沙箱模式下脚本环境同样剔除敏感变量，见「脚本执行环境」）；能力整体由 `GEBAI_CRON_ENABLED` 开关管控（关闭时完全不可见）
- **事件**：触发时推送 `event.cron.run`（任务 ID/类型/名称）；脚本型执行结束推送 `event.cron.result`（成功/失败与输出；prompt 型结果经消息流呈现）

### 工具执行与渲染
- 工具调用时在 UI 中打印工具名和参数，参数以 JSON 格式展示（脚本类工具渲染为语法高亮代码块）
- `todo` 与 `ask_user` 渲染为**特别卡片**（待办清单卡 / 选择卡），不显示通用工具卡片
- 脚本执行类工具（`sh`、`py`）的输出以 Markdown 代码块独立渲染
- **执行超时**：`sh`/`py` 子进程均设超时上限（默认 5 分钟，可经 `timeout` 参数按次调整，上限 540 秒不晚于引擎 9 分钟兜底），超时强制终止
- **编码**：`py` 强制 UTF-8 模式（`python -X utf8` + 环境变量 `PYTHONUTF8=1`），避免 Windows 下默认 GBK 输出造成乱码/解析问题
- **解释器自适应**：`py` 执行前按 `python3` → `python` → `py` 顺序探测可用命令（`--version` 退出码判定，结果缓存），适配 Linux（多为 `python3`）与 Windows（多为 `python`/`py`），无需配置

#### 脚本执行环境

脚本工具的可用性取决于宿主机环境，分三档：

| 脚本 | 运行方式 | 宿主机要求 |
|------|---------|-----------|
| `sh` | 子进程执行 shell（Windows 自动 `chcp 65001` 切 UTF-8 代码页 + 输出自适应解码：UTF-8 优先、含替换字符回退 GBK；Windows 子进程不设 detached——实测会导致外部程序管道输出丢失；命令成功但无输出时明确提示「无输出」，区分捕获失败；**`timeout` 参数（秒，默认 300、上限 540）调整个别执行超时**） | 有 shell（各平台自带） |
| `py` | 子进程执行 python（**`timeout` 参数同 `sh`**） | 安装 Python |
| `js` | 子进程执行 JS/TS（Bun 运行时：脚本调试模式 `bun <script>` 直跑；二进制模式 `[execPath, 入口, "exec", script]` 复用**编译进二进制/打包产物的 Bun 运行时**自执行，见「js 脚本工具」；**`timeout` 参数同 `sh`**） | **无需安装**（运行时已内嵌） |
| JS/TS（经 `sh`） | 宿主机已有 `node`/`bun` 时 `sh` 直接调用亦可（`bun run x.ts` / `node x.js`），与 `js` 工具两条路径并存 | 安装 bun/node |

- 二进制编译时已内嵌 Bun 运行时，`gebai exec` 隐藏子命令（`import.meta.main` 入口拦截 `process.argv`，**exec 段双位置路由**：argv[2] 或 argv[3]）让二进制无需宿主机安装 bun/node 即可执行 JS/TS 脚本，同时保持子进程隔离（不回到进程内执行）。子进程命令二进制模式统一**带入口段** `[execPath, 入口, "exec", script]`：编译单文件形态（`gebai.exe`）子进程 argv[1] 自动为内嵌虚拟入口，入口段仅占位（exec 落在 argv[3]）；**容器形态**（execPath=bun 跑 `dist/` 打包产物，包内各模块 `import.meta.path` 一致指向打包产物）入口段是真实 dist 入口（exec 落在 argv[2]）——缺失入口段时 bun 会把 `exec` 当子命令、script 当命令名直接报错（js 工具全挂）
- 能力探测：启动时检测宿主可用解释器（`python`/`node`/`bun`），在工具 schema 描述与 UI 中标注当前环境可用性，模型据实选择
- **输出大小**：工具输出超过截断阈值走上下文保护（截断落盘），防止内存膨胀

#### sh 异步后台任务（`async:true` + `sh_task`）

长耗时命令（全量构建/测试/依赖安装）同步等待会占死一轮工具调用，`sh` 传 `async: true` 转**后台执行**——立即返回 `taskId`（结构化 `data.taskId`/`pid`），模型先处理其他任务，之后经 `sh_task` 回头处理：

- **启动**（`sh async:true`）：经 `Sandbox.spawnBackground` 起进程——与同步 `exec` 同规则的 shell/环境脱敏（沙箱用户剔除敏感变量）/Windows `chcp 65001`/Unix 进程组语义，但 stdout+stderr **合并持续写入日志文件**（WriteStream 落盘，不占内存）且不等待完成；`timeout` 参数在此语义为**任务生命周期上限**（默认 1800 秒、上限 3600 秒，防僵尸进程常驻）
- **状态落盘**（会话 `tmp/sh-tasks/`，跨工具调用与服务重启可见）：`tasks.json` 记录（命令/cwd/pid/起止时间/退出码，原子写 tmp+rename）+ 每任务 `{id}.log` 合并输出日志；引擎按 `user:sessionId` 复用服务实例（`ToolContext.shTasks`），会话删除时释放（`forgetSession`）
- **生命周期**：进程退出码由启动时注册的 `exited` 回调落盘回写（同进程内准确）；服务重启后 pid 失活而记录无终态 → 判定 `lost`（已结束、退出码未知，日志尾部仍可读）；生命周期超限在 status/wait/list/kill 时**惰性检查**——终止进程树（本进程内经句柄精确 kill，重启后按 pid 兜底：Windows `taskkill /T`/Unix 进程组）并标记 `timed_out`
- **查询/等待/终止**（`sh_task`）：`status` 立即返回当前状态（running/done/failed/killed/timed_out/lost）与输出尾部；`wait` 阻塞至完成或等待超时（默认 60 秒、上限 540——不晚于引擎 9 分钟工具兜底，超时返回当前状态可再次 wait）；`kill` 终止进程树并标记；`list` 列出本会话全部任务
- **上限**：单会话并发运行任务 ≤ 8（`SH_TASK_MAX_CONCURRENT`，超限拒绝新任务并引导清理）；输出尾部默认 4000、上限 20000 字符（`tail` 参数）
- **审批与安全模式**：与同步 `sh` 完全同规则——命令本身照常过动态审批（`approval:false` 免审白名单强制）与安全模式只读白名单（`validateShCommandSafeMode`，降级语义不分同步/异步）；`sh_task` 管理动作（查询/等待/终止本会话任务）免审批

#### 工具双输出（output / data）与输出 Schema

工具结果（`ToolResult`）携带两条相互独立的输出通道（面向「工具即函数、模型动态编程」的编排能力）：

| 通道 | 字段 | 消费方 | 说明 |
|------|------|--------|------|
| 文本输出 | `output` | 模型（进 LLM 上下文） | 面向模型分析的文本，截断保护照常生效 |
| 结构化输出 | `data` | **flow 数据流编排**（不进 LLM 上下文） | 供编排步骤引用/映射（`{{步骤id.data.字段}}`）、分支判定；运行期在编排引擎内传递，不落盘、不占上下文词元 |

- **`Tool.outputSchema`**：声明 `data` 的 JSON Schema，经 `tool_schemas` 工具批量暴露给模型——编排前先查输出结构，避免逐个试调浪费往返
- **引擎兜底截断保留 `data`**（含 `sessionRun` 扩展字段）：截断只作用于模型可见文本
- 已提供结构化输出的全局工具：`ls`（entries）、`glob`（files/total）、`file`（info：path/type/size/isDir/modifiedAt/entries/encoding/text/extMismatch）、`grep`（matches）、`sh`/`py`（stdout/stderr/exitCode，stdout/stderr 在 data 中截断至 100k 字符）、`js`（logs/result/exitCode/calls，logs/result 截断至 100k 字符）、`fetch_url`（ok/status/contentType/error）、`todo`（todos）、`agent_list`（agents）；子Agent 工具可按同一模式声明（`ToolResult.data` + `Tool.outputSchema`，如 `code_git`：status/log）
- **`tool_schemas` 工具**（批量查询）：`tools` 传工具名列表返回各工具 `{name, description, parameters, outputSchema}`（未知/未启用标记错误）；省略时返回全部已启用工具的输出结构概要（紧凑一行一个，不含输入参数）

#### 富内容块渲染

工具返回不再限于纯文本，可通过 `ToolResult.blocks` 携带**结构化的内容块**（`ContentBlock`），前端按类型逐块渲染，并**随消息持久化**——历史会话同样可查看：

| 块类型 | 渲染 |
|--------|------|
| `text` | 纯文本段落 |
| `code` | 代码块（标注语言） |
| `image` | 内嵌图片（经 `files/content` 加载会话 `tmp/` 内文件；**点击进入全屏查看器**——缩放/平移/复制/下载，与图表查看器同骨架，`draw` 工具 `render=backend` 产出的 PNG 同样可全屏查看） |
| `file` | 文件卡片（`files/download` 下载链接；**点击按需预览**——图片直接展示、PDF 浏览器内嵌渲染、文本/代码语法高亮、其余提示下载；常规工具产物默认此形态，`show_file` 仅对无法内联的类型回退使用，见「文件主动展示（`show_file`）」） |
| `diagram` | **交互式图表**（Mermaid/PlantUML/D2/ECharts 四语言本地渲染，缩略图卡片 + 全屏查看器，源码查看/下载、高清 PNG 下载） |
| `diff` | **并排文本对比**（旧/新两栏行对齐，增删行红/绿着色，按文本类型语法高亮，见「diff 文本对比工具」） |
| `html` | **HTML 页面沙箱渲染**（iframe 直接预览，全屏查看器 + 源码查看/复制/下载；域隔离 sandbox 内脚本可执行、无法访问宿主页面，见「HTML 页面渲染（`render_html`）」） |

- 工具按产出自动生成块：`read`/`write` 依据扩展名识别图片（PNG/JPG/GIF/WebP/SVG/BMP）与图表（`.puml`/`.plantuml`→plantuml、`.mmd`/`.mermaid`→mermaid、`.d2`→d2、`.echarts`→echarts，`diagram` 块 `format` 字段携带图表语言）；`draw` 工具专门产出 `diagram` 块；`diff` 工具产出 `diff` 块（并排对比）；`render_html` 工具产出 `html` 块（沙箱预览）；`show_file` 工具产出 `file` 块（文件卡片，见「文件主动展示」）
- 图表渲染走**通道化渲染链路**：`draw` 工具执行中引擎发布 `event.draw.render`（含 renderId + 源码 + **图表语言 `format`**）→ **Web 通道**前端**本地渲染**（按 format 分派本地引擎、**零网络请求**：mermaid → 官方 `dist/mermaid.min.js` 自包含 UMD、plantuml → 官方 `@plantuml/core` 引擎（TeaVM 编译，含内置 Graphviz 布局）、d2 → 官方 `@terrastruct/d2` WASM 引擎（浏览器构建，Web Worker 内编译渲染）、echarts → 官方 `dist/echarts.min.js` 自包含 UMD（**SSR 模式直接输出 SVG 字符串**——JSON option 纯计算渲染为静态 SVG，无 DOM 挂载、无动画，与其余三语言同一「源码 → SVG」契约）；**引擎经 `public/vendor/` 稳定文件名静态伺服**——无内容 hash，重建后 URL 不变，开发模式旧页面的动态分块 404 从根上消除，见「构建性能」）经 WS `draw.result` 或 REST `POST /sessions/:id/draw` 回传结果（**前端按 payload.format 原样分发**——`onDrawRender` 不做语言归一化，未知语言由 `renderDiagramSvg` 显式报错；echarts 曾被这里的归一化吞掉喂给 PlantUML 引擎，最新前端也报「PlantUML 渲染错误」）；**飞书通道与 `render=backend`** 由服务端**组合后端渲染器四语言全支持**（见「后端图表渲染」）——`draw` 工具 **`render` 参数选择渲染通道**：`frontend`（**默认首选**，浏览器本地渲染 SVG 可交互缩放、零服务端开销，降低后端性能压力）/ `backend`（**服务端直接渲染成 PNG 图片**：引擎经 ToolContext `renderDiagram` 调用组合渲染器，落盘会话 `tmp/{name}.png` 并返回 `image` 内容块（**前端图片块点击进入全屏查看器**：缩放/平移/复制/下载），**仅导出/分享图片等确需 PNG 文件时使用**，四语言均支持；**前端渲染不可用（收到「画图能力受限」）时提示模型改用 backend 重试**）；**`code` 与 `path` 二选一（文件渲染）**——`path` 指定会话内已存在的图表文件（`.mmd`/`.mermaid`/`.puml`/`.plantuml`/`.d2`/`.echarts`）直接读取渲染（图表名默认取文件主名，format 未传时按扩展名推断，规范化源码落盘 `tmp/{base}.{ext}`，已生成图表文件再渲染/换通道时不重发源码），`code` 缺省仅需 `path`；**渲染成功工具才返回成功**（前端：图表块 + 落盘源码文件；后端：图片块 + 落盘源码/`.png`），渲染报错（语法错误等）把错误文本返回给模型（据此修正源码，错误信息指明失败的语言），**5 秒超时返回「画图能力受限」**（前端/飞书通道渲染端离线或引擎加载失败时降级，不阻塞任务；`render=backend` 不经前端回传，走服务端渲染器自身超时兜底）；**echarts 通道加固（实测复盘，前端渲染为默认正确通道、不自动换通道旁路）**：① 服务端**预校验 JSON**（`parseEchartsInput` 纯解析零渲染开销）——无效 JSON 立即精确报错，不白跑前端一轮；② **前端版本错位明确诊断**：前端报错引擎与请求语言不符（如请求 echarts 却由 PlantUML 引擎报错——旧版前端把未知语言静默当 PlantUML 渲染）时，返回「前端渲染器版本过旧，请刷新页面」的诊断而非误导性的源码错误；新版前端对未知语言自身也显式报错引导（见「前端本地渲染引擎」）
- **后端图表渲染（四语言）**：组合渲染器 `core/diagram-render.ts`（draw 工具 `render=backend` 与飞书通道共用，浅色主题白底图）——**plantuml**：复用 `feishu-bot/plantuml.ts`（TeaVM 引擎 + 极简 DOM 垫层，串行队列）；**mermaid**：`mermaid` npm 包 + **happy-dom DOM 垫层**（固定浅色主题 `htmlLabels: false` 纯 SVG 输出；happy-dom 无布局引擎，`getBBox` 以几何属性估算覆盖——rect/path/line 坐标、文本按**全角 1.0em / 半角 0.6em 逐字符估算**（全角统一按半角 0.6em 会低估约 40%，中文标签经 getBBox→viewBox 传导导致整图偏窄、文字被裁剪），否则 mermaid 布局坍缩为 16px）；**d2**：`@terrastruct/d2` WASM（node-esm 构建，**文件路径 Worker**——bun build 无法内联：dev 直接 import 包；**二进制模式**从内嵌产物（`scripts/build-d2js.ts` 生成 `d2js.embedded.generated.json`，gzip base64，随产物打进二进制）物化到 `{GEBAI_HOME}/vendor/d2js/{version}/` 后动态 import，版本目录幂等物化 + 旧版本清理）；**echarts**：`echarts` npm 包 SSR 渲染（`init(null, null, {renderer:"svg", ssr:true, width, height})` + `renderToSVGString()`，零 DOM）——`parseEchartsInput` 解析 JSON option（宽松 JSON：容忍注释/尾逗号）或 `{"option":…,"width":…,"height":…}` 信封（尺寸钳制 200-4000，默认 960×600），注入 `animation:false`（SSR 静态输出必须）；**echarts 顶层急切导入**（zrender 环境探测在模块求值期完成，若在 happy-dom/PlantUML 垫层污染全局 window/document 后才加载会误判浏览器环境——浏览器路径文本测量需真 canvas，垫层不支持；diagram-render.ts 本身经引擎惰性 import，急切导入不增加启动开销）；**SVG → PNG** 共享栅格化（`@resvg/resvg-js`，2x 超采样、1600×2400 上限）：根元素规范化（百分比尺寸显式化、**负原点 viewBox 平移归一**——resvg 对负原点 viewBox panic，以 `translate` 包裹内容平移进正象限且不包裹 `<style>`/`<defs>`/`<title>`）；**全局环境切换**（共享进程全局 DOM/Worker，统一走单一串行队列）：happy-dom 垫层与 PlantUML 垫层相互覆盖全局 document——mermaid 渲染前强制重放 happy-dom 垫层、plantuml 渲染前重放其垫层；**`globalThis.window` 仅临时存在**（mermaid 导入/渲染期临时注入，plantuml 渲染后即删）——实测 Bun 的 `node:worker_threads` 在全局 `window` 存在时 Worker 启动挂起（D2 渲染器依赖）；错误按语言包装回传（`Mermaid 渲染错误：…`/`D2 渲染错误：…`/`ECharts 渲染错误：…`），各引擎懒加载（echarts 急切，不拖慢启动/测试）
- 图表配色**跟随 UI 主题**：三种语言按各自机制适配——① **PlantUML 双层机制**：注入引擎确认支持的 `skinparam`（backgroundColor/defaultFontColor/class/object/state/note/activity/时序图分组/生命线/泳道等，追加在源码末尾覆盖用户 `!theme`/自定义 skinparam）+ **渲染后颜色兜底修正**（TeaVM 版仅支持少量 skinparam，其余元素为引擎硬编码默认色）：将引擎默认亮色（节点/激活条/分组背景）、默认暗色（文字/描边）等替换为主题 CSS 变量色；② **Mermaid**：按当前 UI 明暗以 `theme: dark/default` 重新 `initialize`（主题为全局状态，明暗切换时重建；**固定 `htmlLabels: false` 纯 SVG 文本标签**——默认 true 时标签渲染为 HTML 元素包于 `<foreignObject>`，会被注入 DOM 前的 DOMPurify SVG 净化剥离导致「有框无文字」，前端与后端渲染器一致；**`suppressErrorRendering: true` 抑制内置错误渲染**——默认渲染失败（语法错误等）会在 `document.body` 遗留「Syntax error in text」错误图标容器且无法关闭，抑制后渲染期临时元素一并清理，错误统一经 Promise 拒绝由调用方处理：缩略图占位/查看器回退/回传模型修正源码）；③ **D2**：按当前 UI 明暗选主题 ID（亮色 `0`=Neutral Default、暗色 `200`=Dark Mauve，官方默认暗色主题）作为渲染参数；④ **ECharts**：按当前 UI 明暗注入 `darkMode: true/false`（echarts 内置暗色适配——轴线/标签/默认调色板自动切换，用户 option 显式设置的颜色不受影响）；**JSON option 无法表达函数**（formatter 等用字符串模板 `"{b}: {c}"`，echarts 原生支持；前后端解析一致：宽松 JSON 容忍注释/尾逗号，信封指定画布尺寸）；**明暗判定统一按正文文字色感知亮度**（`hexLuminance(--text) ≥ 128` 视为暗色）；**主题变量为 rgba 时与 body 背景合成不透明等效色**（`css-color.ts` 纯函数，带单测——否则亮色主题下图表退化回暗色默认值、深字深底不可读）；用户显式指定的颜色不受影响；切换主题后已渲染图表自动重绘；SVG 按主题化源码缓存，同图重复渲染零开销；引擎空闲预热（PlantUML + mermaid + echarts；D2 的 8MB WASM 开销大且架构图频率低不预热）避免首次调用超时；渲染 SVG 注入 DOM 前经 DOMPurify 净化（PlantUML 支持 `<html>`/`<img>` 嵌入，防脚本注入）
- 消息 `blocks` 字段持久化于 `chat.json`，历史会话加载时以同一渲染管线呈现，保证「历史可看」
- 工具执行事件（`event.tool.result`）携带 `blocks`，UI 实时展示图片/图表等产物

#### 图表交互式创作（`draw`）

全局 `draw` 工具专门支持**结构化图表的交互式创作**，**四种图表语言由工具描述/参数说明内置选择指南指导模型按需选择**（四语言对比如下）：

| 图表语言 | 首选场景 | 典型触发词 |
|----|------|------|
| **Mermaid** | 通用场景首选：流程图/时序图/状态图/甘特图/用户旅程、Markdown 文档嵌入（README/Wiki/博客）、简单架构，语法最简洁 | 流程图、时序图、状态图、甘特图、文档 |
| **PlantUML** | UML 与严谨建模首选：类图/组件图/部署图/用例图/活动图/ER 图等标准 UML（支持全部 14 种），表达复杂继承/依赖/关联，语义严谨 | 类图、UML、组件图、部署图、用例图、ER图、继承关系、软件设计 |
| **D2** | 美观架构图与对外展示首选：系统架构/云架构/网络拓扑/微服务，PPT/汇报/技术分享，默认布局最现代化 | 架构图、系统架构、云架构、微服务、汇报、美观 |
| **ECharts** | 数据可视化与统计图表首选：柱状/折线/饼图/散点/雷达/仪表盘/热力图/地图等，动画级视觉效果（SSR 静态输出），适合数据报告 | 柱状图、折线图、饼图、数据图表、统计、报表、Dashboard |

- 组合场景指引（内置在 `format` 参数说明中）：系统设计文档 = PlantUML 类图/组件图 + Mermaid 流程图；架构汇报 = D2 全景架构图 + PlantUML 详细组件图；数据分析 = ECharts 统计图表
- 工具：`draw`（**`format` 必选参数**：`mermaid`/`plantuml`/`d2`/`echarts`——**缺失/非法立即报错列出可选值，不静默回退 plantuml**（实测复盘：模型漏传 format 时 ECharts JSON 被当 PlantUML 渲染，「PlantUML 渲染错误」的报错误导模型连续多轮失败；`path` 模式扩展名可推断时免传）；Mermaid 直接给图定义、PlantUML 源码无需 `@startuml`/`@enduml` 包裹自动补全、D2 直接给声明式文本、**ECharts 给 option 的严格 JSON**（键名与字符串一律双引号，不支持单引号/裸键名/`…`省略号缩写，容错 `//` 注释与尾逗号；值禁止函数，格式化用字符串模板如 `"{b}: {c}"`；可选信封 `{"option": {...}, "width": 960, "height": 600}` 指定画布尺寸，默认 960×600）；`code` 与 `path` 二选一——已有 `.mmd`/`.puml`/`.plantuml`/`.d2`/`.echarts` 文件可经 `path` 直接渲染，避免重发源码，format 未传时按扩展名推断；可选 `render` 参数：`frontend` **默认首选**（浏览器本地渲染、零服务端开销）、`backend` 服务端渲染成 PNG 图片（仅导出/分享图片场景，四语言均支持））、`read`/`write`（参考素材/落盘说明）、`todo`（拆解步骤）
- 流程：澄清需求 → 选语言 → `draw` 生成初始图表（实时渲染——Web 前端本地渲染/飞书桥接后端渲染成图片/`render=backend` 服务端渲染成 PNG 图片，渲染成功才返回成功，报错会收到错误信息）→ 展示并说明图意 → 依用户反馈反复用 `draw` 迭代（增删节点、改连线、调整布局）
- **PlantUML 布局规范（防图表杂乱）**：① draw 工具描述/参数说明显式要求模型控制布局——流程/时序类图表声明方向（横向流程用 `left to right direction`，分层架构保持默认纵向），**不得靠逐条连线上写 `-down->`/`-right->` 硬控全局布局**；② 密集图表间距由 **⑤布局兜底注入**承担——工具描述**不再**建议手动设置 `skinparam ranksep 80`/`nodesep 40`（自动注入已覆盖，手动建议与之矛盾且冗余）；③ 关系紧密的节点用 `together { … }` 保持相邻；④ 控制规模：单图节点 ≤20 个，架构图按层拆包（package），跨层连线过多时拆成多个图表分别展示；⑤ **布局兜底注入**：源码未显式设置 `skinparam ranksep`/`nodesep` 时，服务端（`injectPlantUmlLayout`）与前端（`plantuml-layout.ts` 同规则）在渲染/落盘前自动注入默认间距参数（仅 `@startuml` 类图；mindmap/wbs/gantt/salt/json/yaml 等布局由结构决定不注入，已显式设置者尊重用户布局）；Mermaid/D2 的布局由各自语言的结构决定，不做注入
- UI 交互：`diagram` 块在消息流中渲染为**缩略图卡片**（**铺满卡片宽度**的较大尺寸预览 + 文件名——svg 按 `width: 100%` 放大至卡片宽（mermaid 内联 `max-width` 限布局宽、d2 无宽高属性默认 300px 导致预览过小，`max-width: none !important` 压过内联样式），高图限 60vh（横排 48vh）防撑爆消息流；**始终默认渲染**（低性能模式同样自动渲染）；**渲染失败不在主页面暴露错误细节**——仅显示「图表渲染失败，点击查看详情」占位（技术信息进查看器/控制台）；点击进入**全屏查看器**；**同一消息内连续多个图表横排**展示——均分宽度自动换行，节省纵向空间）；**块级工具结果封段**：draw/diff/render_html 等 `card.args="block"` 工具结果到达时封存当前文本段——图表卡片独立展示，**画图后的输出另起新卡片显示在图下方**（防输出继续追加到画图前的那张卡片、在图上方滚动）；查看器**默认自动适应视口**（初始放大至 125% 显示更清晰、图不超屏）并**居中显示**（缩放基准取 SVG 逻辑尺寸——优先 viewBox，mermaid/d2 渲染时已显式化根 `width/height`（`width="100%"`/无宽高属性的 SVG 在 flex 容器内 `getBoundingClientRect` 解析为 0×0 导致查看器空白）），单栏标题栏（文件名 + 图标工具栏：还原视图、−/＋ 锚点缩放、拖拽平移、复制图片、下载图片、查看源码）；**下载图片为 3x 超采样高清 PNG**（SVG 矢量无损放大，本地绘制，无网络）；源码弹窗内支持**一键复制**与**按图表语言的扩展名下载**（`.mmd`/`.puml`/`.d2`/`.echarts`）
- 审批：`write` 需审批；`draw` 默认无需审批，降低创作摩擦

#### HTML 页面渲染（`render_html`）

全局工具 `render_html` 让模型生成 HTML 页面**直接在聊天界面内渲染展示**（类似 `draw` 的图表卡片，但面向网页原型/数据报表/卡片徽章/可视化组件等页面型产物）：

- **参数**：`html` 与 `path` 二选一——`html`（HTML 源码，完整文档或片段均可）、`path`（会话内已存在的 `.html` 文件路径，直接读取渲染，页面名默认取文件主名——已生成页面文件仅需重新展示/调预览尺寸时不重发源码）；`name`（可选，页面标题/文件名，默认 `page`，自动剥离 `.html` 后缀）、`width`/`height`（可选，预览尺寸 px——由模型按内容设计显式指定，如移动端页面窄高、宽表格页面宽高；非法值（非正数/超上限）忽略回退默认）
- **产物**：HTML 落盘会话 `tmp/{name}.html`（UI 文件面板可见、模型可经 `read` 读取），同时返回 `html` 内容块（`{ type: "html"; html; name; width?; height? }`）随消息持久化，历史会话以同一渲染管线重放
- **UI 渲染**：消息流中渲染为**预览卡片**（标题栏 + 工具栏：全屏查看/查看源码/复制源码/下载 `.html` + iframe 预览，点击预览或标题进入**全屏查看器**，查看器内可查看源码/下载）；与 `draw`/`diff` 同属 `card.args="block"` 声明——不渲染通用工具卡片，调用与结果直接呈现 html 块（无内容块时以输出文本兜底）；**预览宽度机制**：iframe 默认**固定 100% 铺满消息流宽度**，**不参与任何内容宽度反馈**（无尺寸上报、无 resize 回路，杜绝越缩越窄）；**预览高度机制**：无显式 `height` 时 iframe 高度 = **会话区域高度的 2/3**（`.chat-wrap` 声明 `container-type: size` 作为尺寸容器，iframe 用容器查询单位 `height: 66.67cqh` 随窗口尺寸自动重算；不支持 cqh 的旧浏览器回退 480px）；含 HTML 卡片的消息脱离 `.msg` 的 `fit-content` 宽度计算（iframe 为 replaced element，其 `width:100%` 在 max-content 计算中会退化回内置 300px 宽把消息压窄）——消息占满消息流宽度且 `.msg-body` 以 `flex:1` 撑满，实测锁定 300px 的塌缩被根治；模型显式传 `width`/`height` 时按指定值渲染（宽于消息流的显式宽度由卡片横向滚动承载）
- **安全（域隔离）**：模型生成的 HTML 视为不可信输入，渲染于 **sandbox iframe（`allow-scripts`，不含 `allow-same-origin`）**——脚本可执行但运行在**隔离 opaque origin**：无法访问宿主页面 DOM/Cookie/存储（跨域访问抛 SecurityError）、无法顶层导航（无 `allow-top-navigation`）、弹窗（无 `allow-popups`）、表单提交（无 `allow-forms`）、下载（无 `allow-downloads`）；嵌套 iframe 继承沙箱限制；`referrerpolicy="no-referrer"` 防 Referer 泄漏；**CSP meta 注入** srcdoc 文档 head（`default-src * data: blob:; script-src 'unsafe-inline' 'unsafe-eval'; style-src 'unsafe-inline'`）放行内联/外部脚本、样式与网络，页面自带 CSP 与注入策略取交集只会更严
- **脚本说明**：脚本在沙箱内正常执行（交互/动态效果/外部脚本均可）；跨域网络请求受浏览器 CORS 与第三方 Cookie 限制（opaque origin 请求带 `Origin: null`，仅 `Access-Control-Allow-Origin: *` 响应可读，SameSite 默认 Lax 不携带跨站 Cookie）；需访问宿主页面数据/同源 API 的诉求无法满足
- **主题跟随**：iframe 预览区边框与背景走主题变量（`--border`/`--bg-inset`），内容页无自带背景时与主题一致（避免亮色突兀块）；srcdoc 注入 `style#gebai-theme` 同步**主题变量集**（`--bg`/`--text`/`--accent`/`--border`/`--radius-*`/`--font-mono` 等 20 个核心变量写入 `:root`，工具/预览页面可用 `var(--x)` 引用主题色）与背景/滚动条色值，并设置 `data-theme` 属性（支持 `[data-theme]` 分支样式）；初始快照注入，宿主主题切换经 postMessage `gebai-host/theme` 广播动态更新——**`gebai:theme-change` 事件驱动立即广播**（免 500ms 轮询延迟，轮询兜底异步加载）；内容页自带背景/变量时其自身规则（文档中靠后）覆盖注入值，不破坏内容设计
- **审批**：默认无需审批（仅展示 + 写入会话 `tmp/`，与 `draw` 同级）

#### 文件主动展示（`show_file`）

全局工具 `show_file` 让模型**把文件直接展示给用户**（交付产物 / 需要用户过目的文件）——**按文件类型产出直显内容块，内容在消息流内联呈现**：

- **直显矩阵**：图片（PNG/JPG/GIF/WebP/SVG/BMP）→ `image` 块（内联 `<img>` + 点击全屏查看器）；图表源文件（`.puml`/`.plantuml`/`.mmd`/`.mermaid`/`.d2`/`.echarts`）→ `diagram` 块（前端本地渲染图表卡片，format 按扩展名推断）；`.html` → `html` 块（沙箱 iframe 页面预览）；文本/代码（txt/md/csv/json/yaml/log/常见代码扩展名，≤512KB）→ `code` 块内联展示内容（超 4 万字符截断展示 + 附 `file` 卡片取全文）；**无法内联的类型**（PDF/压缩包/Office/音视频等）→ `file` 块（查看/下载卡片，点击时才经 `files/content` 按需加载——PDF 点击经浏览器原生查看器内嵌渲染）
- **与普通工具的语义区分**：`show_file` 是「给用户看」的主动通道（内容直接呈现）；`read` 等常规工具按既有行为返回产物块（文件类产物为查看/下载卡片，不自动展开内容）
- **参数**：`path`（必选，会话 `tmp/` 相对路径（前缀可省略）；本地模式可为工作区相对/绝对路径）、`name`（可选，展示文件名，默认取文件主名）
- **路径处理**：会话 `tmp/` 内文件**直接引用**（零复制）；会话外文件（本地模式工作区/绝对路径）**复制一份**到会话 `tmp/shown/{主名}-{内容哈希8}.{扩展名}`（内容哈希命名，重复展示复用同一副本；上限 100MB，超出引导改为告知路径）后引用——前端文件接口只服务会话 `tmp/`，复制保证可见性与文件面板留存。**归属判定用会话 `tmp/` 真实绝对路径**（`sessionPath` 拼接；项目绑定子Agent 的 `resolvePath` 基准是项目根，不能作判定依据），复制目标同样直接写会话 `tmp/` 绝对路径（绕开项目根基准，保证落在真实会话文件区）
- **mime 推断**：按扩展名映射（图片 + PDF/文本/CSV/JSON/HTML/Office/音视频等常见类型），驱动前端预览入口形态（图片内嵌/PDF 内嵌查看/文本高亮/下载提示）
- **审批**：默认无需审批（只读 + 复制进本会话 `tmp/`，与 `draw` 同级）；`card.args="block"` 声明（调用不显示通用工具卡片，内容块直接渲染）

### 上下文保护

对话上下文（消息历史 + 工具返回）超出 LLM 上下文窗口时，自动执行压缩；也支持用户主动压缩。

#### 自动压缩

- 触发条件：上下文接近窗口阈值（如 80%）时自动触发，无需人工干预
- **上下文占用口径：只认模型服务返回的真实大小**（`input_tokens`，含 system 提示词与工具 schema，即「真上下文」）——压缩判定不依赖任何估算，估算容易误判（chars/4 对中文低估 2~4 倍、工具 schema 与图片块又难以折算），一律以接口真值为准：
  - **跨 run**：上次任务最后一次调用返回的真实 `input_tokens` 持久化为基线（`SessionData.ctxInputTokens`）；下次 run 基线本身超阈值即先压缩（本次调用只会更大），不做增量估算
  - **任务中途**：每轮调用返回的真实 `input_tokens` 超阈值（80%）时压缩最早历史（`makeContextRoom`），长任务不再等接口拒绝
  - **溢出恢复**：接口以上下文长度错误拒绝（4xx = 真实大小的权威信号）时，压缩最早历史后重试（至多 3 次）——无基线会话首次调用即超窗也能收敛，不再任务失败
  - 接口不返回 usage（或压缩替换消息导致索引锚点失效）时不估算预判，由本次调用的真值 / 溢出恢复接管；估算仅用于**展示**（会话列表 ctxTokens 增量补足，见常量表）
- 压缩策略（按序使用）：
  0. **超长用户输入落盘（预防）**：发送时超过阈值的用户输入自动全文写入会话 `tmp/user_inputs/{内容哈希}.txt`（原文不丢——文件面板可见、模型可经 `read` 工具读取全文；内容哈希去重，相同输入复用同一文件），消息正文保留头尾预览 + 文件引用，避免大段粘贴撑爆上下文；未超阈值原样不变，落盘失败降级为原样保留（不改变优先）
  1. **工具大输出截断**：工具返回超过截断阈值自动截取头尾摘要（**按行保留完整行**，避免切断半行/半条目；单行巨长如 minified 时该行按字符兜底），完整内容写入文件，截断消息中附带文件路径供大模型后续读取。**引擎兜底（不依赖工具自觉）**：工具未自行截断的超长输出，由引擎在主循环统一截断落盘——凡 `output` 超过截断阈值且未带 `truncated` 标记的结果，自动复用同一截断逻辑（含内容块保留），保证任何第三方/新工具都不会撑爆上下文；已自行截断的工具结果不重复处理
  2. **旧消息摘要**：将最早一段历史消息由 LLM 生成摘要，替换原始消息，摘要保留关键信息。**系统提示词与用户输入不压缩不改变**（`isProtectedMessage`：user/system 角色消息与新会话执行存档）——不选进压缩区间、不进摘要输入、区间夹带时原位保留，压缩只作用于 assistant/tool 消息，压缩条数只计实际移除的消息
  3. **滚动裁剪**：摘要仍超限时丢弃最早消息，保证最新上下文完整
  4. **溢出硬护栏（压缩无法收敛时的最后防线）**：历史几乎全是用户输入/系统提示词（无可压缩内容）时，受保护消息让路——最旧用户消息的图片附件降级为文本说明（可用 vision/read 按需查看）、仍不够将最旧用户消息替换为裁剪占位（原文仍在 chat.json，UI 可查、不丢数据）；**最新一条用户消息（本次任务输入）永不裁剪**；压缩为**迭代执行**（run 前按基线迭代压缩直至收敛，任务中途/溢出恢复经 `makeContextRoom` 压缩 → 护栏降级两步腾挪）
  5. **历史图片内联窗口**：仅最近 3 组含图片的用户消息内联进上下文，更早的图片降级为路径说明（图片永久占窗口且不参与压缩，长会话会被历史图片占死窗口）
  6. **LLM 流式读空闲超时**：SSE 建立后连续 120 秒无任何 chunk 判定接口假死，中止本次调用（无产出走重试、有产出上抛为任务错误）——此前网关/上游挂起会无限挂起任务
- 压缩过程对用户透明，UI 显示压缩通知（压缩范围、摘要内容），原始消息可从会话文件回溯
- 压缩后继续原任务流程，不影响进行中的工具调用循环

> **实现**：已落地。`engine.compactSession()` 支持主动（`session.compact` / REST `POST /compact`，scope 指定区间）与自动触发（真实 usage 超窗口 80% 时压缩最早历史，触发口径见「上下文占用口径」）；摘要由 LLM 生成（**同样带读空闲超时与取消信号**（同 `chatWithIdleTimeout` 防假死）——压缩在任务流程内同步等待，无超时会把整个运行中任务永久挂死；失败降级为滚动裁剪占位），摘要消息持久化（`compacted`/`summary` 标记，UI 渲染为压缩通知，历史重载时作为 assistant 角色注入）；已压缩摘要消息不参与后续压缩。**手动压缩在会话有任务运行时被拒**（summarize 是秒级 LLM 调用，期间任务持续追加消息，陈旧压缩区间会套删未参与摘要的新落盘内容；自动压缩经 `internal` 标记在任务流程内自身协调，不受此限）。**超长用户输入落盘**（`spillLongUserInput`，run 发送时执行）：超阈值输入全文写入会话 `tmp/user_inputs/`，消息正文保留头尾 + 文件引用。**压缩不碰系统提示词与用户输入**（`isProtectedMessage`：user/system 角色 + 新会话执行存档）——不选进压缩区间、不进摘要输入（摘要只覆盖将被移除的 assistant/tool 消息）、区间夹带时由 `store.compactMessages` 原位保留不移动（装载提示词是会话恢复的关键记录，用户输入是原始上下文基准，压缩过程中完全不受影响）；区间内无可压缩消息时不做任何改动（不创建摘要、不动 usage 基线）；`compactMessages` 的压缩条数只计实际移除消息数。超限截断（`trimToCacheLimit`，300 条上限）同样保护：受保护消息原位保留、从最早的其他消息开始丢弃，受保护消息本身超上限时按原样保留（软上限，不改变优先），丢弃按 tool_call 配对原子执行——assistant(toolCalls) 被丢弃时连带其后紧邻的 tool 结果（拆散配对会产生孤儿 tool 消息，严格校验的 LLM 接口会拒绝整个请求），实际保留条数可略低于上限；截断保护同上（`compacted`/`loadedAgent`/用户输入消息在超限截断中原位保留、不重排）。**配对完整性修复（`repairToolPairing`）**：任务取消中断、压缩/截断边界或旧版本缺陷仍可能产生「孤儿 tool 结果」（发起 assistant 已删）或「未应答 toolCalls」（结果缺失）——严格校验的接口（OpenAI tool_calls/tool、Anthropic tool_use/tool_result 配对）会拒绝整个请求，会话自此每次运行 400 卡死。修复分层落地：① 引擎工具循环取消/异常路径为本轮全部 toolCalls 补写占位结果（assistant 先落盘后执行的中断不再缺结果）；② 存储层 `compactMessages` 压缩后、`readFileByPath` 磁盘装载时即时修复（孤儿普通 tool 丢弃、孤儿受保护 tool（agent_run 存档）补最小 assistant 桩、中途未应答补占位结果；尾部未应答**不在存储层 flush**——正常执行流 assistant 先落盘结果随后到达，提前 flush 会让真实结果反被判孤儿丢弃）；③ `llm.ts` 三家序列化入口兜底（含尾部 flush + Anthropic 相邻同角色 user 消息合并），旧版本已损坏的会话在下一次模型调用自愈。压缩替换消息后真实 usage 基线的索引锚点失效：`store.compactMessages` 自动清除 `ctxInputTokens`/`ctxAtMessage`，压缩判定改由下一次真实调用重建真值。**压缩迭代 + 溢出护栏 + 中途压缩 + 溢出恢复 + 读空闲超时均已落地**（`engine.ts`：`makeContextRoom`/`degradeProtectedMessages`/`callModelWithOverflowRecovery`/`isContextOverflowError`/`chatWithIdleTimeout`；图片内联窗口 `INLINE_IMAGE_RECENT=3` 作用于 `loadHistory`）。

#### 主动压缩

- 用户可随时在 UI 中手动触发压缩当前会话：一键「压缩上下文」或 `/compact` 命令
- 支持自定义压缩范围：全部历史 / 指定消息区间 / 仅工具输出
- 主动压缩同样走上述策略，压缩结果立即生效并持久化

#### 截断保护存储

- 存储路径：`{session}/tmp/truncated/{tool_name}_{content_hash}.txt`（`{session}` 为会话根目录 `{GEBAI_HOME}/users/{user}/sessions/{s0}/{s1}/{session_id}`）
- 消息中返回**会话根内逻辑路径**（如 `tmp/truncated/read_xxx.txt`）：沙箱模式下模型可经 `read` 工具直接读取（修复早期「沙箱读不到截断文件」的矛盾），前端文件面板同步可见/可下载
- 文件名含工具名可溯源；基于内容 SHA256 哈希去重，相同输出不重复写入
- 生命周期随会话：会话删除/过期时随 `tmp/` 整体清理（见「数据生命周期」）

### 消息模型与数据结构

核心数据结构（SDK 与 UI 共用，作为类型契约）：

```ts
// 内容块（消息内容的结构化呈现，UI 逐块渲染；image/file 的 path 为会话 tmp/ 内逻辑路径）
type ContentBlock =
  | { type: "text"; text: string }
  | { type: "code"; text: string; language?: string }
  | { type: "image"; path: string; name?: string; mime?: string }
  | { type: "file"; path: string; name: string; mime?: string }
  | { type: "diagram"; format: "plantuml"; code: string; name?: string; version?: number }
  | { type: "diff"; oldText: string; newText: string; language?: string; name?: string; oldName?: string; newName?: string; lines: DiffLine[] }
  | { type: "html"; html: string; name?: string }

// 行级 diff 结果（diff 内容块）：按顺序排列，每行标注差异类型
type DiffLine = { kind: "equal" | "add" | "del"; text: string }

// 消息（持久化于 chat.json）
interface Message {
  id: string
  role: "user" | "assistant" | "tool" | "system"
  content: string
  reasoning?: string                   // 推理内容（reasoning_content/thinking）独立字段：assistant 消息持久化时写入，content 保持纯正文；
                                       // 回放给 LLM 时（loadHistory）不携带——推理绝不进模型上下文；UI 历史渲染为折叠推理卡；旧版数据推理内嵌 content 的 <think> 块（兼容展示/剥离）
  blocks?: ContentBlock[]              // 富内容块（文本/代码/图片/文件/图表），随消息持久化
  attachments?: AttachmentRef[]        // 多模态引用
  toolCalls?: ToolCall[]               // assistant 的工具调用请求
  toolCallId?: string                  // tool 结果关联
  createdAt: number
  compacted?: boolean                  // 上下文压缩摘要消息标记（role=system），UI 渲染为压缩通知
  summary?: string                     // 压缩摘要消息：被压缩的原始区间描述（条数/时间范围）
  session?: boolean                    // 新会话执行过程消息（旧版逐条存档，历史兼容）：完整存档但【不进入主 LLM 上下文】（loadHistory 跳过），前端按 runId 分组折叠渲染
  sessionRunId?: string               // 新会话执行 run 标识：同一次 agent_run 执行过程的消息共享（前端回放按此分组）
  sessionMeta?: { agents: string[]; input: string }  // 新会话 run 元信息（仅该 run 首条消息携带）：折叠容器标题用（预加载子Agent 名与输入）
  sessionRun?: SessionRunArchive     // 新会话 run 完整存档（agent_run 工具调用记录扩展字段）：执行过程全部内容，历史回放渲染折叠容器
  // 旧版（agent_call 时代）字段：subAgent/subAgentRunId/subAgentMeta/subAgentRun 兼容历史会话回放，新数据不再写入
}
interface SessionRunArchive {
  runId: string                        // run 标识（与实时事件 sessionRunId 同源）
  agents: string[]                     // 预加载进新会话的子Agent 列表
  input: string                        // 任务输入（容器标题展示）
  output: string                       // 最终返回文本（折叠后摘要展示；异常/取消为空串）
  messages: SessionRunEntry[]         // 执行过程全部消息（输入/每轮回复/推理/工具调用与结果）
}
interface SessionRunEntry {
  role: "user" | "assistant" | "tool"
  name?: string
  content: string
  reasoning?: string                   // 推理内容独立字段（同 Message.reasoning 语义）：assistant 条目持久化时写入，回放展示不回流模型
  toolCalls?: ToolCall[]
  toolCallId?: string
  arguments?: Record<string, unknown>
  blocks?: ContentBlock[]
  sessionRun?: SessionRunArchive     // 嵌套执行（新会话内再 agent_run）存档递归携带
}

interface ToolCall {
  id: string
  name: string                          // 全局名（含命名空间前缀）
  arguments: Record<string, unknown>
}

interface ChatChunk {                   // 流式输出单元
  kind: "text" | "reasoning" | "tool_call" | "tool_result" | "approval" | "done" | "error" | "reset" | "session_start" | "session_done"
  messageId?: string                    // text delta 携带本条 assistant 消息 id（前端反馈/操作绑定）
  session?: boolean                    // 事件来自新会话执行过程（agent_run 派生会话；主回复不带此标记）
  sessionRunId?: string                // 新会话执行 run 标识：同一次 agent_run 的执行过程事件共享（前端按此分组渲染）
  sessionMeta?: { agents: string[]; input?: string; output?: string }  // 新会话 run 元信息：start 携带 agents/input，done 携带 agents/output（折叠容器标题用）
  text?: string
  toolCall?: ToolCall
  approval?: { toolCallId: string; retries: number; tool: string }
  error?: string
}

// 补充语义：
// - `reasoning`：推理内容增量（reasoning_content / thinking），前端流式推理中展开实时展示、推理完成（正文开始/流结束）自动折叠，可手动展开；**内容 markdown 完整渲染**（与正文同路径节流渲染，低性能模式合并 120ms）；推理内容超出可视高度（`.reasoning-body` 限高 200px）时内部滚动条自动跟随最新内容，用户上翻翻阅历史不打扰（`reasoning-scroll.ts`）；
//   推理**持久化为独立字段**（`Message.reasoning`，content 保持纯正文；历史会话/切回可见，前端默认折叠可展开，内容同样 markdown 渲染），**回放给 LLM 时不携带**（`loadHistory` 仅映射 content——推理绝不进模型上下文）；旧版数据推理内嵌 content 的 `<think>` 块：前端回退解析展示、回放时 `stripThinkTags` 剥离（兼容，不做数据迁移）
// - `text`：文本增量；**携带 `session: true` + `sessionRunId` 表示文本来自新会话执行过程**（agent_run 派生会话流式回复），前端渲染进该 run 的折叠容器（见下）
// - `session_start`：新会话 run 开始（携带 runId + agents/input），前端创建折叠容器——执行中**展开并滚动到可见**；服务端**每轮重推**（同 runId 幂等，前端容器已存在则忽略），前端容器随消息重载丢失（切走会话/断线重连）后新一轮 delta 前可据此重建
// - `session_done`：新会话 run 结束（携带 runId + agents/output），前端封存流式文本段、写入最终返回摘要并**自动折叠容器**（只显示输入与最终返回，点 summary 展开看完整过程）
// - 新会话执行过程（text/reasoning/tool_call/tool_result/approval）事件**全部携带 `session: true` + `sessionRunId`**（与主循环同构渲染：推理折叠块、工具卡片、流式文本），并**完整落盘**（见「新会话执行存档」）——仅存档与前端回放，`loadHistory` 跳过（不进入主 LLM 上下文）；会话列表 ctxTokens 估算同样排除
// - `error`：任务失败（LLM 接口错误等），前端应渲染错误提示（与流异常中断同等对待）
// - `tool_call`/`tool_result`/`approval`：WS `event.*` 推送，SDK `sendPrompt` 订阅事件流转换为 ChatChunk 迭代（单通道，无 SSE 兑底）

interface AgentEvent {                  // WS event.* / Webhook 统一载荷
  type: string                          // event.message.delta 等
  sessionId: string
  payload: Record<string, unknown>
  timestamp: number
}
```

- 消息内存缓存（300 条/会话）+ LRU 会话缓存（10 个），超出走持久化
- 附件引用 `AttachmentRef`：`{ path, mime, name, size }`，指向会话 `tmp/` 内文件

## 功能列表

### 总Agent全局工具

| 工具 | 功能 | 默认审批 |
|------|------|---------|
| `read` | 读取文件内容（相对路径以会话 `tmp/` 为基准，`tmp/` 前缀可省略；服务端部署受沙箱限制，桌面/本地浏览器不限制，见路径基准）；可选 `offset`（起始行号，1 起始）与 `limit`（行数，正数取 offset 起 N 行、负数取末尾 N 行），按行切片便于大文件分段阅读；`lineNumbers=true` 每行前缀真实行号（右对齐+制表符，切片后仍对应文件行号，规划修改/按 文件:行号 引用时推荐）；读取成功登记「本会话已读」（write 防误覆盖守卫依据，见「write 防误覆盖守卫」） | 否 |
| `write` | 写入文件（整体覆盖）。相对路径以会话 `tmp/` 为基准（`tmp/` 前缀可省略，受沙箱限制）；目标文件**已存在且本会话未 read 过**时拒绝（防盲覆盖，先 read 再覆盖，见「write 防误覆盖守卫」） | 否 |
| `ls` | 列出目录内容（文件/子目录、大小） | 否 |
| `grep` | 按正则表达式在会话工作目录（`tmp/`）中递归搜索文本内容（返回 文件:行号: 匹配行——路径带 `tmp/` 前缀可直接用于文件工具，限文件大小与匹配数）；`output` 三种结果形态（`content` 逐行内容 / `files` 仅命中文件清单——宽泛摸底定位优先 / `count` 每文件命中行数）、`context` 附匹配行前后上下文（格式同 `grep -n -C`：匹配行 `文件:行号:` 前缀、上下文行 `文件-行号-` 前缀、组间 `--` 分隔）、`include` 按文件路径 glob 过滤（如 `*.ts`）（见「grep 内容检索工具」） | 否 |
| `glob` | 按文件名模式（glob：`*`/`**` 跨目录、`?` 单字符）在会话工作目录（`tmp/`）递归查找文件（path 可限定子目录，`tmp/` 前缀可省略，与 `read`/`write` 同一路径解析规则；返回路径带 `tmp/` 前缀，可直接用于文件工具） | 否 |
| `file` | **文件管理（单工具多动作）**：`rename` 重命名（同目录改名，`newName` 仅名字不含路径——含分隔符拒绝防越界，跨目录用 move）/ `move` 移动或跨目录改名（`to` 含目标文件名，父目录不存在自动创建，与 `write` 一致）/ `delete` 删除文件或目录（递归，不可恢复）/ `info` 查看文件信息——**按内容探测**（类似 `file` 命令，读头部 1KB）：魔数识别实际类型（图片/压缩包/Office/PDF/可执行/SQLite/Java class 等）、文本 vs 二进制判定（二进制勿盲 read）、编码检测（UTF-8/BOM/UTF-16/疑似 GBK——GBK 直接 read 会乱码）、shebang 解释器；**扩展名与实际内容不符时显式提示**（`data.extMismatch`）；附人类可读大小与修改时间，目录附直接子条目数。写动作（rename/move/delete）走写范围守卫；与 `ls`（目录列表）分工——`ls` 单独保留 | 否 |
| `edit` | **精确修改文件**：基于 `old_string` → `new_string` 替换（可多处），替换前校验原文匹配与**唯一性**（多处命中报错列出行号，或该项 `replaceAll: true` 全部替换），失败即报错不落盘；空 `oldString` 拒绝；成功回报各处应用行号（见「edit 修改工具」） | 否 |
| `patch` | **应用 unified diff 补丁**：一次多 hunk、行号模糊容错（上下文裁剪重试），全部 hunk 校验通过才整体落盘（原子），`dryRun` 可预演不落盘；单次单文件（见「patch 补丁应用工具」） | 否 |
| `diff` | **文本/文件对比**：对比两段文本或两个文件（旧 → 新），输出 unified diff 文本并返回 `diff` 内容块，UI 并排对比渲染、按文本类型语法高亮 | 否 |
| `flow` | **数据流编排**（工具即函数、动态编程）：一次调用执行多步工具链——引用映射（`{{id.data.字段}}`，一对一/一对多/多对一）、`when` 条件分支、`foreach`/`while` 循环、`optional` 容错；旧版线性串联格式兼容（见「flow 数据流编排工具」） | **取决于内部工具**（动态审批） |
| `tool_schemas` | **批量获取工具 schema**：按工具名列表返回输入参数与结构化输出（`data`）的 JSON Schema；省略时返回全部已启用工具的输出结构概要——编排（flow）前理解输出结构，避免逐个试调 | 否 |
| `agent_list` | 列出可用子Agent（名称/描述/是否已装载；**不列工具名**，工具名以注册的工具集为准）。**不注册进总Agent 全局工具集**——未装载清单已由 `systemPromptInjection` 注入提示词（模型上下文已有，工具冗余且干扰工具选择）；仅在新会话执行环境注入（纯 md 组合子Agent 自动注入编排工具时，见「子Agent文件格式」） | 否 |
| `agent_load` | **装载**子Agent 能力模块（类比 import 子模块：工具并入当前工具集、**完整系统提示词作为 system 消息写入会话记录**（持久化，恢复会话自动还原），**不创建独立上下文**；默认使用方式：装载后直接用其工具，仅在需要干净上下文或防膨胀时才改用 `agent_run` 新会话执行；装载反馈**枚举实际并入的工具全名**（`{agent}_{tool}` 清单），模型无需猜测直接调用） | 否 |
| `agent_run` | **执行新会话**（派生临时新会话，**无需装载**：预加载一个或多个子Agent（完整系统提示词拼接+工具并入）后阻塞执行一次并返回最终结果，中间过程/推理/内部工具不进主上下文，全程存档可回放；默认优先 `agent_load` 装载后直接用其工具，仅在需要干净上下文或防上下文膨胀时使用） | 否 |
| `full_mode` | **切换完整模式**（**仅极简模式会话可见可用**，完整模式会话从 schema 移除）：极简下任务确需其他工具能力时调用，用户批准后解锁全部工具（本任务后续轮次 schema 立即全量下发）、系统提示词原地升级为完整版、会话极简标记清除并通知前端关闭开关；可选 `reason` 参数说明原因供审批参考（见「工具选择」极简模式） | **是** |
| `sh` | 执行Shell命令（**Windows 下经 cmd.exe 执行：命令串联用 `&&`/`||`/换行，`;` 非分隔符会被并入参数，引号语义以 cmd 为准**；**`input` 参数**：stdin 输入，对象/数组自动序列化为 JSON 文本（双引号，脚本 `json.loads` 可解析）；**`timeout` 参数：执行超时秒数，默认 300、上限 540，超时按进程树终止并返回超时结果（`async:true` 时为任务生命周期上限：默认 1800、上限 3600）**；**`strict` 参数**：true 时非 0 退出码抛工具级错误（flow 编排「非 0 即中断」，默认 false 非 0 退出作为正常结果返回，exitCode 在结构化输出）；**`async` 参数**：true 后台异步执行——立即返回 taskId 不阻塞（长耗时构建/测试先做其他事再回头查询，见「sh 异步后台任务」）；**`approval` 参数**：本次调用是否需审批，默认 true，明确安全的只读/幂等命令可传 false 按次免审（见「工具审批」）；可运行 `bun run`/`node`；JS/TS 亦可通过内置运行时 `gebai exec` 自执行，见「脚本执行环境」） | **是**（默认；`approval:false` 按次免审） |
| `sh_task` | **管理 sh 异步后台任务**（`sh async:true` 启动并返回 taskId，见「sh 异步后台任务」）：`action=status` 立即返回任务状态与输出尾部 / `wait` 阻塞等待完成（`timeout` 秒内未完成返回当前状态，默认 60 上限 540——「先做别的再回头等结果」）/ `kill` 终止任务进程树 / `list` 列出本会话全部后台任务；输出为 stdout+stderr 合并日志尾部（`tail` 参数默认 4000 上限 20000 字符，完整日志 `tmp/sh-tasks/{id}.log`） | 否 |
| `py` | 执行Python代码（**`input` 参数同 `sh`**；**`timeout` 参数同 `sh`**；**`strict` 参数同 `sh`**；**`approval` 参数同 `sh`**） | **是**（默认；`approval:false` 按次免审） |
| `js` | **执行 JS/TS 脚本（工具动态编程）**：Bun 子进程运行，脚本内工具**像内置函数一样直接调用**——`await read(params)`（已启用工具名即顶层函数，动态名字 `tools.call`）+ `ctx` **注入会话上下文**（user/sessionId/workdir/home/sandboxed/env/projects/messages 最近消息快照）+ `input`（flow/编排传入）；console 输出即工具输出，`return` 值进 `data.result`；`timeout`/`strict`/`approval` 参数同 `sh`（见「js 脚本工具」） | **是**（默认；`approval:false` 按次免审） |
| `draw` | **创建/更新结构化图表**（**四种图表语言**：`format` 必选参数 `mermaid`/`plantuml`/`d2`/`echarts`，工具描述与参数说明内置选择指南指导模型按需选择——Mermaid 通用流程图/时序图/状态图/甘特图首选、PlantUML 标准 UML 严谨建模首选（类图/组件图/部署图/用例图等）、D2 美观架构图/云架构/对外展示首选、ECharts 数据可视化/统计图表首选（option 的 JSON，值禁止函数），见「图表交互式创作」），渲染成功才返回成功、报错回传模型、5 秒超时判定画图能力受限；渲染按通道实现——Web 前端本地渲染（四种语言各自本地引擎、配色跟随 UI 主题）、飞书桥接后端渲染成 PNG 图片（四语言组合渲染器）、`render=backend` 参数服务端直接渲染成 PNG 图片（四语言组合渲染器，返回 `image` 内容块）；**`code` 与 `path` 二选一（文件渲染）**——`path` 指定会话内已有 `.mmd`/`.puml`/`.plantuml`/`.d2`/`.echarts` 文件直接读取渲染（图表名默认取文件主名、format 按扩展名推断，不重发源码）；默认 `frontend` 通道，保存到会话 `tmp/` 并返回 `diagram` 内容块（`format` 字段携带图表语言）供 UI 交互式渲染；**PlantUML 布局规范内置于工具描述**（方向声明/间距 skinparam/together/节点规模控制，未设置间距时自动注入 `skinparam ranksep 80`/`nodesep 40` 兜底，见「图表交互式创作」） | 否 |
| `render_html` | **生成 HTML 页面并直接在聊天界面内渲染展示**（沙箱 iframe 域隔离预览：脚本可执行、隔离于宿主页面；适合网页原型/数据报表/可视化组件/带交互脚本的小页面），保存到会话 `tmp/` 并返回 `html` 内容块；**`html` 与 `path` 二选一（文件渲染）**——`path` 指定会话内已有 `.html` 文件直接读取渲染（页面名默认取文件主名，不重发源码）；与 `draw`/`diff` 同属 `card.args="block"` 声明（调用不显示通用卡片，结果直出内容块） | 否 |
| `show_file` | **把文件直接展示给用户**（交付产物/需用户过目的文件）：按类型产出直显内容块——图片内联、图表源文件渲染图表、`.html` 页面预览、文本/代码内联展示（超 4 万字符截断 + 文件卡片取全文）；无法内联的类型（PDF/压缩包/Office/音视频）给查看/下载卡片（点击时才按需加载，PDF 内嵌渲染）。会话 `tmp/` 内直接引用，会话外文件复制到 `tmp/shown/{主名}-{内容哈希}.{扩展名}`（≤100MB）后引用，见「文件主动展示」；`card.args="block"` 声明（内容块直接渲染） | 否 |
| `fetch_url` | 抓取 URL 内容（网页/API/文档；服务端部署模式限制公网地址防 SSRF 并逐跳校验重定向，响应超阈值截断） | 否 |
| `vision` | **视觉分析**：把图片文件交给多模态（视觉）模型分析（参数 `target` 目标 + `image` 图片文件路径；模型选择 `GEBAI_VISION_*` 额外视觉模型，未配置回落到声明多模态能力的主模型；base64 内联传输，单图 8MB 上限），返回分析文本并携带 `image` 内容块 | 否 |
| `todo` | 待办管理（统一入口）：`entries` 操作列表，每项 `op=add/update/delete`——add 新建（需 title，可带 priority/note/eta），update 按 id 改 status/progress/title，delete 按 id 删除；省略/空数组 = 查询；返回操作摘要与当前全部待办状态 | 否 |
| `ask_user` | 向用户提出选项并**阻塞等待回应**（选项可多选/复杂选项 title+description/自定义文本/拒绝均可，见「选择工具」） | 否 |
| `ask_env` | **请求用户设置环境变量并阻塞等待**（name 变量名 + description 用途说明 + secret 敏感值掩码；前端弹窗填值，提交后值注入本次任务 env——后续工具读取立即生效，并保存到浏览器本地后续任务自动生效；用户拒绝/超时返回失败，见「环境变量」） | 否 |
| `plan` | **计划审批工具**：为复杂/多步骤任务制定执行计划——计划文档落盘会话 `tmp/plans/{标题}.md`（文件名清洗，前端消息流直接展示计划全文）后**阻塞等待用户审核**：批准 → 模型严格按计划逐步执行；拒绝（可附修改意见）→ 按意见修订后重新调用本工具提交新版本；审批超时/用户拒绝审核 → 不再提交，基于现有信息直接回答。适用于多步骤、有风险、不可逆或用户需要把关的任务；简单任务无需使用（`todo` 跟踪即可）。审批内置于本工具（`waitForChoice`，不占工具审批通道） | 否 |

> **`current_time` 已移除**：时间获取经 `sh`/`py`/`js` 脚本按需完成（如 `sh` 执行 `date`），不占全局工具位；不注入时间相关提示词引导——模型自行选用现有工具。

> **全局工具集最小化**：编码工作流专属工具不注册为全局工具——`git`（只读变更核对，`code_git`/`explore_git` 命名空间暴露）与 `preview_server`/`env_detect`/`system_info`（验证服务与环境/工具链探测，`code_preview_server`/`code_env_detect`/`code_system_info`）由 `code`/`explore` 子Agent 按需暴露（`self_optimize` 经连带装载 code 一并获得）；小工具库管理（原全局 `save_tool`/`delete_tool`）下沉 `widgets` 子Agent 并补齐增删改查（`widgets_save`/`widgets_list`/`widgets_get`/`widgets_delete`，与模型工具语义区分）；反馈读取（`read_feedback`）下沉 `self_optimize`（`self_optimize_read_feedback`，自我优化专属输入通道）；定时任务（原全局 `cron_add`/`cron_list`/`cron_update`/`cron_remove`）下沉 `cron` 子Agent（`cron_add`/`cron_list`/`cron_update`/`cron_remove` 命名空间不变，见「cron 子Agent」）；`delete_file`/`move_file` 合并为全局 `file` 多动作工具——全局集只保留高频基线与编排/交互入口，低频域工具下沉子Agent。

> **路径基准（统一）**：文件工具（`read`/`write`/`edit`/`patch`/`ls`/`grep`/`glob`/`file`/`draw`/`vision` 等）的相对路径统一以**会话 `tmp/` 子目录**为基准——与 `sh`/`py` 子进程工作目录（cwd）、`glob`/`grep` 搜索范围、UI 文件面板范围完全一致，跨工具传递路径无需考虑前缀（`write("a.txt")` 产物 `sh` 直接可见、`glob`/`grep` 直接命中）。带 `tmp/` 前缀的逻辑路径（列表/附件/截断产物契约，如 `tmp/a.txt`）剥离前缀后解析，两种写法等价（`stripTmpPrefix`）；`glob`/`grep` 的 `path`/`include` 过滤同样兼容两种写法（列表坐标双候选匹配）。沙箱模式限定会话 `tmp/` 内（桌面/本地浏览器放开，绝对路径直用）。

### `edit` 修改工具

在 `write`（整体写入）之外提供**精确修改**能力，适合代码/配置文件的小步修改：

- **参数**：`path`（目标文件）+ `edits: { oldString, newString, replaceAll? }[]`（可一次多处替换；`replaceAll` 缺省 false）
- **唯一性校验（防误改）**：默认要求 `oldString` 在文件中**唯一命中**——多处命中时整体失败不落盘，错误信息列出各命中行号（至多 8 处）并给出两条出路：扩大 `oldString` 上下文使其唯一，或确认全部替换时该项传 `replaceAll: true`；避免「相同片段出现在多处时改错第一处」的静默误改
- **安全校验**：替换前校验 `oldString` 在文件中**精确匹配**，不匹配则整体失败并报错，**不落盘**，避免模型基于过期内容误改；`oldString` 为空直接拒绝（空串匹配会静默插入文件头，显式报错引导新建文件用 `write`）
- **近似匹配提示**：精确匹配失败但空白归一化后可命中时，错误信息提示「空白/缩进不一致的近似原文」——引导模型 `read` 后从原文逐字符复制（含缩进），常见于模型凭记忆写 `oldString` 忘记缩进的场景
- **应用行号回报**：成功后输出每处修改的应用行号（如 `已对 a.ts 应用 2 处修改：1) 行 12；2) 行 40`），便于汇报与后续引用
- **与 `write` 的关系**：`edit` 用于既有文件的定点修改（保留无关内容）；`write` 用于新建/整体覆盖（受防误覆盖守卫约束）；模型按需选择，`flow` 中可混用；改动较多或行号容易偏移时优先用 `patch` 应用 unified diff（见「patch 补丁应用工具」）
- **审批**：默认无需审批（与 `write` 同级）；子Agent 可通过 `requiresApproval` 声明 `edit` 需审批
- **路径限制**：与 `read`/`write` 同一路径基准与沙箱（相对路径基于会话 `tmp/`；服务端部署限其内，桌面/本地浏览器不限制）

### `grep` 内容检索工具

按正则表达式递归搜索文本内容（`core/tools.ts` 纯实现，行级匹配），是编码探索的主定位工具：

- **参数**：`pattern`（正则）+ 可选 `path`（搜索起点：目录（递归）或单个文件（直接内搜该文件，grep 传文件语义）；默认 `.`，显式传 `.` 与省略等价（经 `resolvePath` 归一化，与 `glob` 同一惯例），相对会话工作目录、`tmp/` 前缀可省略）/`ignoreCase`/`output`/`context`/`include`；返回路径带 `tmp/` 前缀（可直接用于 `read` 等文件工具）
- **搜索范围**：默认会话 `tmp/` 子树（与文件面板一致）；`path` 指向 `tmp/`/项目根**外**时——本地模式（桌面/本地浏览器）与 `read` 同款自由度，按给定 path 前缀**实际遍历**目标搜索（`walkDirFiles`：跳过 node_modules/.git/dist 等大型/生成目录、深度上限 10，与 code/explore 项目遍历同一实现），结果路径带给定前缀可直接用于 `read`；沙箱部署模式仍拒绝越界（无匹配）；路径不存在**明确报错**（不再静默「无匹配文件」）
- **二进制内容跳过**：文件内容含 NUL 字节视为二进制跳过（同 grep 二进制检测语义），防乱码匹配行刷进上下文；**不读 `.gitignore`**——忽略规则仅内置目录黑名单（node_modules/dist/.git/target 等），被 git 忽略的文件（如 `*.log`）仍会命中
- **三种结果形态（`output`）**：
  - `content`（默认）：逐行匹配内容，`文件:行号: 匹配行` 格式
  - `files`：仅命中文件清单（不刷内容）——**宽泛摸底定位优先用**（「哪些文件涉及 X」先看文件面，再对重点文件精读，避免大量匹配行灌进上下文）
  - `count`：每文件命中行数（按命中数降序）——快速评估命中面大小、决定深入策略
- **上下文行（`context`，0-10，仅 content 模式）**：匹配行前后各附 N 行，格式同 `grep -n -C`——匹配行前缀 `文件:行号:`、上下文行前缀 `文件-行号-`、不相邻组之间 `--` 分隔；重叠区间自动合并，一次调用即可看清命中语境（免二次 `read`）
- **文件过滤（`include`）**：按文件路径 glob 过滤（如 `*.ts`、`**/*.test.ts`；`*`/`**` 跨目录、`?` 单字符，与 `glob` 工具同一套语义）
- **上限**：单文件 1MB、全局匹配 200 处（三种形态同一口径，达上限附「结果可能不完整」提示）；输出超长走统一截断保护
- **正则匹配在独立子进程执行（灾难性回溯防护）**：模型提供的正则存在灾难性回溯形态（嵌套量词如 `(a+)+b` 配超长单行——同步匹配挂死 JS 事件循环且无中断手段，服务端全部会话冻结），匹配（行数据按 4MB 批量经 stdin 送子进程、命中行号回父进程渲染）在独立子进程执行，20 秒超时强杀（Unix 杀进程组/Windows `taskkill /T`）；超时返回「简化 pattern/缩小范围」引导而非回退进程内匹配（回退即重新暴露挂死面）
- **结构化输出**：`data = { mode, matches?, files?, counts?, truncated? }`（按形态携带，`mode` 标明本次形态），供 flow 编排引用
- **审批**：默认无需审批（纯读取）

### `write` 防误覆盖守卫（已读追踪）

防止模型**盲覆盖**未见过的文件内容（参照 ZCode Write 的「未读不许覆盖」设计）：

- **守卫规则**：`write` 目标文件**已存在且本会话未读取过**时拒绝写入，返回引导信息（先 `read` 掌握现有内容，确认整体覆盖再 `write`；只改局部用 `edit`/`patch`）；**新建文件不受限**
- **已读登记（fileGuard）**：引擎维护会话级已读文件集合（`AgentEngine.readFiles`，sessionId → 已读绝对路径），经 `ToolContext.fileGuard`（`markRead`/`hasRead`）注入工具上下文；`read`/`edit`/`patch`/`write` 成功后自动登记——修改过的文件视为已掌握内容，后续 `write` 覆盖放行（迭代改写自身产物不受阻）
- **生命周期**：登记按会话隔离（`agent_run` 新会话有独立追踪）；REST/WS 删除会话时经 `engine.forgetSession` 释放；单会话登记上限 2000 条（`READ_TRACK_CAP`，超出整表重置——守卫降级为「需重读」，保护语义不破坏）；进程重启后登记为空（重新 read 即可，文件可能已变化，重读本身即正确行为）
- **兼容性**：`fileGuard` 为可选注入——测试桩/无引擎环境不注入时守卫自动放行（行为与旧版一致）；`flow` 编排内的工具调用走同一 ctx，守卫同样生效
- **设计动机**：`edit`/`patch` 天然带原文校验（幻觉内容匹配不上），而 `write` 整体覆盖**没有任何内容校验**——未读守卫补齐这一盲区；拒绝结果作为正常工具输出返回（模型下一轮自行 `read` 后重写，一次往返自愈，不中断任务）

### `diff` 文本对比工具

对比两段文本或两个文件（旧 → 新），用于展示代码/配置/文档的变更，UI 提供**并排对比视图**：

- **参数**：`oldText`/`newText`（文本直传）或 `oldPath`/`newPath`（文件对比，受路径沙箱限制）二选一；`language`（语法高亮语言，默认按文件名扩展名推断：`.ts`→`typescript`、`.py`→`python`、`.json`→`json` 等）；`name`（对比标题，工具参数传入，推荐给有意义的标题如「重构前后对比」；不传默认取文件名）；`oldName`/`newName`（旧/新两侧面板标题，如「重构前」「v1」，不传默认「旧」/「新」）
- **输出**：`output` 为 unified diff 文本（含 `---`/`+++` 头与 `@@` hunk 块，供 LLM 阅读，超长走截断保护）；同时返回 `diff` 内容块（`oldText`/`newText`/`language`/`name`/`oldName`/`newName`/`lines`），前端按行级结果渲染并排对比
- **行级对比算法**：LCS 动态规划（纯函数实现于 `core/diff.ts`，可单测），按行输出 `equal`/`add`/`del` 序列，回溯保证 `del` 在前、`add` 在后；单侧行数上限 2000 行（防内存/耗时爆炸，超出报错提示分段）
- **前端渲染**：左右两栏（旧/新）并排、行号对齐，增删行分别红/绿着色；按 `language` 对**连续同类段整段高亮后切行**，跨行注释/字符串的着色不被截断；历史消息持久化后以同一渲染管线重放；与 `draw` 同属 `card.args="block"` 声明——**不渲染通用工具卡片**，调用与结果直接呈现 diff 内容块（无内容块时以输出文本兜底）
- **审批**：默认无需审批（纯读取/纯文本计算，不落盘）

### `patch` 补丁应用工具

在 `edit`（定点替换）之外提供**整体补丁应用**能力，适合改动较多、行号容易偏移的代码修改：

- **参数**：`path`（目标文件）+ `patch`（unified diff 文本）+ 可选 `dryRun`（仅预演校验，不写入）
- **解析（`parsePatch`，纯函数实现于 `core/patch.ts`）**：支持 `---`/`+++` 文件头（可省略）、`@@ -l,c +l,c @@` hunk 头（容错省略 count 的形式）、上下文/新增/删除行、`\ No newline` 标记与 CRLF；git 风格元数据行（`diff --git`/`index`/`mode` 等）容忍跳过；多文件补丁报错提示分文件调用（单次仅一个文件）
- **匹配与容错（`applyPatch`）**：含删除行的 hunk 以删除行作锚点在文件中整体匹配，上下文不符时**裁剪头/尾上下文各至多 3 行重试**（`PATCH_FUZZ_LINES`，只裁上下文、删除行必在匹配块内，防「应删未删」残留）；纯新增 hunk 按 `@@` 头行号（叠加先前 hunk 的行数偏移）定位插入，带上下文则同样校验
- **原子性**：全部 hunk 校验通过才一次性写盘（与 `edit` 同语义——任一 hunk 不匹配整体失败不修改，返回失败 hunk 序号与原因，提示先 `read` 当前文件核对或改用 `edit`）
- **新建文件**：`--- /dev/null` 头或目标文件不存在时按新建处理（仅允许新增行）；删除类补丁（全部删除行）自然清空文件
- **与 `edit` 的关系**：改动多/跨多处/行号易偏移时 `patch` 一次提交全部改动（可基于 `diff` 工具输出构造补丁，`dryRun=true` 预演）；小范围定点改动用 `edit`；`write` 仅用于新建/整体覆盖
- **上限**：hunk 数 ≤ 100（`PATCH_MAX_HUNKS`）、目标文件 ≤ 5MB（`PATCH_MAX_FILE_BYTES`，超出提示改用 `edit` 分段）
- **审批**：默认无需审批（与 `edit` 同级）；`code`/`self_optimize` 子Agent 声明 `patch` 需审批（与 `edit`/`write` 同级，见下）
- **路径限制**：与 `read`/`write` 同一路径沙箱

### `git` 版本控制工具（只读）

将编码工作流收尾环节的 git 只读操作从 `sh`（每次需审批）独立出来，免审批直接查看变更。**不注册为全局工具**（全局集最小化，见功能列表注记）——经 `code`/`explore` 子Agent 以 `code_git`/`explore_git` 命名空间暴露（`self_optimize` 经连带装载 code 获得；写操作 add/commit 等在任何命名空间下仍走 `sh` 需审批）：

- **参数**：`action`（`status` 工作区状态 / `diff` 变更内容 / `log` 最近提交）+ 可选 `dir`（仓库目录，默认会话工作目录，经路径沙箱约束）+ `staged`（diff 是否查看暂存区）+ `maxEntries`（log 条数，默认 10、上限 50）
- **执行**：命令参数由 action 枚举构造（无用户字符串拼接注入），以 `dir` 为 cwd 执行 `git -C <dir> status --short --branch` / `git diff [--staged] --no-color` / `git log --oneline -n N`；非零退出码附 stderr（非仓库目录等可诊断信息）；输出走统一截断保护
- **只读安全边界**：不提供任何写操作（`add`/`commit`/`checkout` 等仍走 `sh`，需审批且受沙箱约束）——免审批仅限读操作，写操作权限不变
- **审批**：默认无需审批；经 `code`/`explore` 子Agent 的 `projectAware` 包装暴露（`code_git`/`explore_git`，以项目根为工作目录，预置项目形态按项目根执行；`self_optimize` 经连带装载 code 获得）

### `flow` 数据流编排工具

将工具视为函数、flow 调用视为动态编程：一次调用编排多步工具链（引用映射 / 条件分支 / 循环），把「多轮工具调用往返」压缩为「一次编排 + 一次结果」，显著降低时延与词元消耗。编排引擎为纯函数模块 `core/flow.ts`（表达式解析/求值 + 步骤执行器，可独立单测）。

#### 步骤模型

- **工具步骤** `{ id?, tool, params?, input?, when?, optional? }`：执行单个工具；**分组步骤** `{ id?, foreach | while, maxLoops?, when?, steps: [...] }`：循环体（子步骤数组，必须声明 `foreach` 或 `while` 之一）
- 步骤 `id` 缺省按声明顺序自动编号 `s1`/`s2`…（自动编号跳过与显式 id 冲突的序号）；显式 id 须为标识符，不得重复、不得占用保留根名（`prev`/`item`/`index`/`iteration`/`input`）
- 分组嵌套深度 ≤ 4

#### 引用与映射（一对一 / 一对多 / 多对一）

- **引用表达式**：`{{s1.data.xxx}}` 引用步骤**结构化输出**（`ToolResult.data`，各工具结构经 `tool_schemas` 批量查询）、`{{s1.output}}` 引用文本输出、`{{s1.status}}`/`{{s1.runs}}` 引用执行状态。根名：步骤 id / `prev`（上一实际执行步骤）/ `item`+`index`（foreach 当前项与序号）/ `iteration`（while 轮次）/ `input`（flow 的 `input` 参数）。路径访问 `.字段`、`[下标]`、`.length`
- **模板插值**：params 值恰为一个 `{{表达式}}` 时**保留原始类型**（数字/数组/对象原样传递）；混排字符串（如 `report_{{item.id}}.md`）按文本拼接；对象与数组递归插值
- **`input` 显式映射**：`{ 目标参数名: "{{源}}" }`，解析结果覆盖 params 同名字段并**抑制自动注入**——字段改名（`path: "{{s1.data.file}}"`）、**多对一汇聚**（多个步骤输出映射进同一工具的不同参数）均由此表达；**非对象形式**（如 `input: "{{item}}"`）等价于 `{ input: "{{item}}" }`——直接给工具的 `input` 参数传值（脚本 stdin），**模板求值结果即使为对象/数组也作为 input 值传入**（按 `step.input` 原始形态判定，不会误当映射对象展开进 params）。**对象/数组映射给 sh/py 的 `input` 参数时由工具侧序列化为 JSON 文本（双引号）传入 stdin**，脚本 `json.loads` 可直接解析（不出现 Python repr 单引号形态）
- **`foreach` 一对多扇出**：值为数组（逐项——**可直接写裸 JSON 数组文本如 `"[1,2,3]"`**（先按 JSON 解析），或表达式/`{{引用}}` 求值为数组，**求值结果为 JSON 数组文本时同样自动解析**——脚本 stdout 常见形态）或正整数（按次数，字符串与数字形式均可），体内经 `{{item}}`/`{{index}}` 引用；**快照语义**——迭代次数固定为求值时的长度（循环体修改/缩短源数组不影响遍历，删除类流程安全）；**嵌套时内层 `{{item}}`/`{{index}}` 遮蔽外层同名引用**（外层循环值需提前映射进中间步骤/字段再引用）；分组结果 `data` = 每轮末步 `data` 的数组，供后续步骤按下标消费

#### 分支与循环

- **`when` 条件分支**（工具步骤与分组均可）：条件表达式为假时跳过（`status=skipped`，不更新 `prev`、不中断后续）。空数组视为假（`when: "s1.data.items"` 即「有内容才执行」）
- **`while` 条件循环**（do-while 语义）：先执行一轮再判断，条件为真继续下一轮——条件可引用本组最新结果（如 `while: "{{g.data.exitCode}} != 0"` 的重试直到成功模式）；需前置判断配 `when`。`maxLoops` 默认 10、硬上限 50，达上限停止并在报告注明。**组 `data` = 最后一轮末步的 `data`（单轮结果对象，非数组、无 `.length`）**——与 foreach 组的「每轮结果数组」不同，`{{g.data.xxx}}` 始终取最新轮结果，轮次用 `iteration` 引用
- 表达式语言（**两种写法等价**：裸表达式 `g.data.exitCode != 0` 或 `{{表达式}}` 包裹/混排 `{{g.data.exitCode}} != 0`——包裹块归一化为括号子表达式）：比较 `==`/`!=`/`>`/`>=`/`<`/`<=`、逻辑 `&&`/`||`/`!`、括号、字面量（数字/单双引号字符串/true/false/null）、函数 `len()`/`contains()`/`exists()`；宽松相等（null/undefined 等价、数字与数字字符串按数值、对象按 JSON 文本）；求值器为手写递归下降解析（无 eval/Function）；**引用不存在的路径解析为 undefined（整值模板原样传递）/空串（混排字符串拼接），不报错**——需判定存在性用 `exists()`（如 `when: "exists(s2.data.maybe)"`）

#### 失败语义与守卫

- **失败 = 工具级异常**（调用不存在的工具、strict 脚本非 0 退出、工具内部抛错等）——中断整个 flow（错误信息带步骤 id、原因与**已执行步骤清单**（最近 20 步的 id·工具·状态，模型据此判断已发生的副作用、安全续接重试））；`optional: true` 的步骤**任何步骤级失败**（执行失败、参数模板错误、when 表达式错误、未知工具）都不中断（`status=error`、`data={error}`，继续后续）
- **错误定位上下文**：when/while/foreach 表达式错误与参数模板错误均在错误信息中携带所在步骤/分组 id 与工具名（裸表达式错误不再无定位抛出）
- **结构校验**：分组步骤的 `steps` 不能为空（构建期明确报错，防执行期静默跳过）
- **sh/py 非 0 退出码默认是正常结果**（`data.exitCode` 携带，文本含 `[exit N]`）——不构成中断；需要「非 0 即中断」时给该步传 `strict: true`（sh/py 参数，非 0 退出转工具级异常，配合 optional 可容错），或自行用 `when` 判定 `exitCode`
- 规模上限：单次工具调用总数 ≤ 100（含循环迭代展开）、foreach ≤ 50 项、分组嵌套 ≤ 4 层；报告单步输出截断 2000 字符、单轮 500 字符（完整内容在 `data` 与工具自身截断文件中）
- **`timeout` 整体超时**（秒，缺省不限制）：步骤间累计执行时间超限即在步骤边界/循环轮首中止（循环失控保护），**优雅返回已执行部分**（`data.timedOut=true` + `executed` 步数，报告含超时说明——超时是预期内事件，不按错误中断任务，模型可据此调整：减小循环规模/为慢步骤加单步 timeout/拆分）；单步执行中无法中止——单步耗时用各工具自身 `timeout` 参数（sh/py 默认 300s 上限 540s）
- **混排引用进脚本源码注意**：字符串值须自带引号（写 `'item:{{item}}'`——裸拼 `'item:' + {{item}}` 会把值当标识符、执行报错）；`{{s1.data.stdout}}` 等多行引用会带入原始换行，拼进源码语法错误——多行文本建议走 stdin（`input` 参数）或三引号包裹，引用单值字段（`exitCode`/`length`）不受影响

#### 自动注入（旧版线性格式兼容）

未显式 `input` 映射时保留旧版数据传递（`{steps: [{tool, params}]}` 线性写法行为不变）：

- **非脚本工具** → 上一步的 JSON 输出自动匹配下一步的输入参数 schema，按字段名映射注入（解析失败/无匹配字段兜底注入 `input` 参数）
- **脚本工具（`sh`/`py`）** → 上一步的文本输出通过 stdin（`input` 参数）传入
- 注入物恒为上一步**文本输出**（`output`，不区分上一步工具类型）——上一步为 `js` 时即日志 + `[返回值]` 前缀的混排文本（非干净 JSON），下游要干净结果请显式映射 `{{id.data.result}}`

#### 审批与安全模式

- **审批取决于内部工具（动态审批）**：`Tool.requiresApproval` 支持函数形态 `(args, ctx) => boolean`，引擎在审批点解析（主循环与新会话循环一致，函数异常按需审批 fail-safe）；flow 声明的函数递归扫描全部步骤（含循环体），任一工具需审批则**整个 flow 提交一次审批**（审批卡片参数区含完整步骤定义），通过后依次执行；全部工具无需审批时直接执行；步骤 params 中的 `approval: false` 同样生效（如 sh 步骤声明免审则该步不触发整体审批——全部步骤免审时 flow 整体免审；服务模式无交互通道的硬拒绝仍按剥离免审标记后的姿态解析，见「工具审批」）
- **安全模式**：step 层按硬阻断集（`isToolBlockedInSafeMode`，cron 调度类）拦截（返回限制信息、不执行），拦截步骤不计入审批判定（不弹审批卡）；`sh`/`py`/`js`/`write` 等风险工具 step 不再一刀切拦截——直接执行、由各工具 execute 内降级（白名单/审计钩子/只读 shim/写范围），内部审批姿态（如 `sh` 的 `approval` 参数）照常参与整体审批判定

#### 结果

- 模型可见 `output`：逐步报告（`### id · 工具（✓/跳过/受限/失败）` + 摘要，循环分组逐轮摘要）
- 结构化 `data`：`{ steps: [{id, tool, status, runs, data}] }`（编排结果自身也走双输出）

#### 模型引导（系统提示词）

编排能力内嵌进两处系统提示词，引导模型在复杂任务中主动少轮次（每轮工具调用都产生往返时延与上下文词元）：

- **总Agent 系统提示词**（`buildSystemPrompt`）：「复杂/多步操作优先编排一次执行，避免大量单步工具调用浪费往返与词元：固定流程用 flow（引用映射/分支/循环，语法见其工具描述，编排前可用 tool_schemas 查询输出结构；flow 是声明式管道，保持步骤简单）；表达式写不出的高阶逻辑（复杂变换/动态参数计算/错误捕获分支/条件重试/跨步骤聚合）一律用 js 脚本动态编程，不要在 flow 里硬凑复杂表达式；纯系统操作用 sh/py 脚本」（flow/js 的完整能力语义单源于各自工具描述，系统提示词只做一句选型指引——原文复述工具描述曾双倍占上下文；**flow 工具描述内亦声明适用边界**：声明式管道只做稳定固定流程编排，高阶逻辑引导到 js）
- **新会话执行环境**（`agent_run`）：同样注册 `flow`/`tool_schemas`/`js`（工具以会话注册表解析——子Agent 工具可在 flow 内编排、js 内调用），系统消息内嵌同样的编排优先引导（子Agent 内部任务同样受益）

```
// 多对一 + 分支 + 一对多扇出示例
flow({
  input: { repo: "gebai" },
  steps: [
    { id: "files", tool: "glob", params: { pattern: "*.ts" } },
    { tool: "write", when: "len(files.data.files) > 0", input: { path: "report.md", content: "共 {{files.data.total}} 个文件" } },
    { id: "batch", foreach: "{{files.data.files}}", steps: [
      { tool: "read", input: { path: "{{item}}" } },
      { tool: "grep", params: { pattern: "TODO" }, when: "len(prev.output) > 0" },
    ] },
  ],
})
```

### `js` 脚本工具（工具动态编程）

flow 的声明式步骤 + 表达式语言覆盖「可预判的固定流程」；当编排逻辑本身需要**真正的编程**（动态构造参数、按中间结果分支重试、复杂聚合变换、正则/字符串处理、错误分类处理）时，用 `js` 一次编程执行——脚本运行于 Bun 子进程（隔离、可超时终止），进程内注入**工具调用桥**与**会话上下文**，把工具当作函数做动态编程。实现为纯模块 `core/js-tool.ts`（前导生成 + 子进程桥接，可独立单测）。

#### 执行模型

- 生成脚本文件（会话 `tmp/.gebai_js_{uuid}.ts`）= **运行时桥前导**（工具调用/日志重定向/引导执行）+ **工具内置函数声明**（模块作用域，按当前已启用工具名生成）+ 用户代码（包进 async 函数：顶层 await 可用、`return` 返回值），子进程执行后即删
- **子进程命令**：脚本调试模式 `bun <script>`；二进制模式 `[execPath, 入口, "exec", script]`（隐藏子命令复用内嵌 Bun 运行时，exec 段双位置路由，见「脚本执行环境」）；无 shell 参与（参数数组直传，无注入面）
- 子进程环境/工作目录与 `sh` 同规则：cwd=会话 `tmp/`、任务 env 注入；沙箱模式剔除敏感变量（豁免/本地不剔除）；超时/取消按进程树终止（Unix 进程组 / Windows taskkill，`timeout` 默认 300s 上限 540s，超时/中断退出码 124）
- **stdio JSON 行协议桥**：脚本调用工具 → stdout 写 `{"t":"call",id,name,params}`；运行时工具定义（defineTool）→ `{"t":"def",id,name,description,parameters,source}`；服务端分发执行 → stdin 回 `{"t":"res",id,ok,result|error}`（行级并发：脚本可 `Promise.all` 并行调用，应答按 id 配对）；`console.*` 经补丁转 `{"t":"log",level,text}` 回传（stdout 保持纯协议通道）；结束以 `{"t":"done",value}`（返回值）或 `{"t":"fail",error}`（未捕获异常）收口，非 JSON 行按日志透传兜底

#### 注入脚本的调用面（内置函数风格）

- **工具函数（内置函数风格）**：执行时按当前已启用工具名生成**模块顶层函数声明**——`const r = await read({ path: "a.txt" })`，每个工具名都是一个可直接 await 的函数（含子Agent 命名空间工具）；动态名字用 `await tools.call(name, params)` 或 `await tools.xxx(params)`。返回 `{ output, data, blocks, truncated, filePath }`（`data` 即工具双输出的结构化通道，结构可先用 `tool_schemas` 查询）；工具级异常 = Promise reject（脚本可 try/catch 容错继续）；单字段 RPC 截断 100k 字符。**遮蔽无冲突**：函数声明在模块作用域，用户代码在 `__G_main` 内层——同名 let/const/function 正常遮蔽；非法标识符/保留字/与注入全局（tools/ctx/input/console）冲突的工具名跳过生成（仍可 `tools.call` 动态调用）
- **`ctx`（会话上下文）**：`{ user, sessionId, workdir, home, sandboxed, env, projects, messages }`——`env` 与子进程环境同规则脱敏；`messages` 为最近会话消息快照（默认最近 50 条、单条 2000 字符，文本抽取、跳过 system/空消息）；另有 `input`（flow 步骤/编排传入的任意输入，JSON 文本自行 `JSON.parse`）
- **`defineTool(def)`（运行时工具定义）**：与子Agent 工具同写法定义新工具（见「运行时工具定义（defineTool）」），注册成功即注入脚本全局、后续可像内置函数一样调用

#### 运行时工具定义（defineTool）

脚本可以像在子Agent 文件里写工具一样，把一段能力**固化为会话级新工具**：`await defineTool({ name, description, parameters, execute })`——`execute` 与子Agent 工具同签名 `async (args, ctx) => ({ output, data? })`（方法简写 `async execute(args) {...}` / 箭头 / 具名函数均可，`fn.toString()` 序列化保存）。

- **注册**：定义经 RPC `{t:"def"}` 桥到服务端，引擎校验（命名 `[a-z][a-z0-9_]{0,39}`、重名/子Agent 命名空间碰撞、描述与 execute 源码合法、安全模式拒绝）后并入**会话覆盖层**；注册成功即注入脚本全局——**同脚本内可立即像内置函数一样调用**；后续轮次模型直接按名调用（schema 随下一轮下发，与内置工具一致可进 flow/被其他 js 脚本调用）
- **执行模型**：后续每次调用在子进程求值 execute 源码（复用 js 运行时桥：体内可调任意工具/ctx/内置函数，`runtimeDefined` 标记 + RPC 分发层 depth 守卫——**动态工具内不能再调用动态工具/js**（防递归嵌套子进程））；返回 `{output, data?}`（子Agent 语义，字符串自动映射为 output），失败抛工具级错误（flow 编排「失败即中断」语义）
- **作用域与生命周期**：会话级，定义清单随会话 `chat.json` 落盘（`SessionData.dynamicTools`，execute 源码序列化保存）、`run()` 水合恢复（重启不丢）、`forgetSession` 随会话删除释放（会话删除即文件删除）；新会话执行（agent_run）内定义的工具进本次运行注册表（运行结束释放，不落盘、不外泄主会话）；单会话上限 50（水合同限）
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

- **审批**：与 `sh` 同姿态——默认需审批（脚本=任意代码执行），`approval:false` 按次免审；**默认审批一次覆盖整个脚本含内部工具调用**（flow 同语义：一次审批、依次执行）；**免审运行（`approval:false`/免审动态工具）时内部调用需审批的工具在 RPC 分发层被拒**——免审脚本体未经用户审阅，按剥离免审标记后的审批姿态解析（`stripApprovalFlags`，防脚本内再传 `approval:false` 自我免审，与引擎无交互硬门槛同规则；`requiresApproval` 函数异常按需审批处理）
- **安全模式**：`js` 本身**降级为只读运行时**而非禁用——静态扫描（`scanJsReadOnly`：动态 `import()`/`require()`/`eval()`/`Function()`/`import.meta.require`/`process.getBuiltinModule`/`process.binding`/`Bun.fetch`/`Bun.sqlite` 前置拒绝）+ 子进程 shim（Bun 写/进程/网络 API 屏蔽、`Bun.file` 拦写留读、eval/Function/fetch/Worker 全局删除、`Function.prototype.constructor` 中性化）；内部工具调用在 RPC 分发层按硬阻断集（`isToolBlockedInSafeMode`，cron 调度类）拦截（无绕过通道），`sh`/`py`/`write` 等照常可调、由各工具降级规则执行；`defineTool` 与磁盘持久化动态工具同规则降级（execute 源码扫描 + 只读 shim），注册与水合均允许——只读动态工具安全模式下可用
- **内部调用走同一 `ToolContext`**：`writeGuard`（子Agent 写范围）/`fileGuard`（防盲覆盖）等会话级守卫对脚本内工具调用同样生效
- **防嵌套**：脚本内不能再调用 `js`（含 `{agent}_js`）——防嵌套 RPC 桥子进程失控；**动态工具内不能调用动态工具/js**（`runtimeDefined` + depth 守卫），**动态工具运行器（depth 1）内不能再 `defineTool`**（防递归注册）；RPC 分发层执行工具统一携带 **`fromJsBridge` 标记**，`js` 的 execute 见标记即拒——封死 js→flow→js→… 交替递归与所有经直执行工具回到 js 的路径（动态工具不经标记拒绝：depth 0 同脚本调用是合法路径，js→flow→dyn 至多一层动态子进程、无递归通道）；`sh`/`py` 不受限（脚本本就可 `Bun.spawn`）
- **规模上限**：单次脚本工具调用总数 ≤ 100（与 flow 对齐）；RPC 协议行子进程侧 ~2MB / 服务端 2.5MB 截断兜底（防巨对象撑爆内存）；非 JSON 输出行 ≤100k 按日志透传、超长丢弃留注（多为 2MB 截断产生的非法 JSON，防垃圾文本刷屏）
- **密钥不落盘**：`ctx.env` 不嵌入临时脚本文件（历来明文写入合并后的全部环境变量，崩溃残留即密钥泄漏）——子进程内改为运行时引用 `process.env`（spawn 已传同源环境、沙箱脱敏同规则，语义完全一致）；messages 等会话数据仍嵌入（ctx 注入契约，属任务数据）
- **内置函数声明过滤 JS 全局名**：子Agent 工具名为 JS 全局（`fetch`/`JSON`/`Promise`/`Bun` 等）时不生成模块作用域函数声明（会遮蔽全局，`JSON.parse` 莫名变工具调用），仍可经 `tools.call` 动态调用
- **失败语义**：未捕获异常/非 0 退出/超时/中断 → 失败结果（`[脚本失败]` + 错误/退出码，不中断任务）；`strict:true` 时失败抛工具级错误（flow 编排「失败即中断」，与 sh/py 一致）；成功无输出明确提示「（脚本执行成功，无输出）」

#### 结果（双输出）

- 模型可见 `output`：console 输出汇总（warn/error 加前缀）+ `[返回值]` 预览（2000 字符，完整值在 data；返回值须为纯 JSON 结构——Map/Set/Date 经 JSON 序列化会变形或丢失）
- 结构化 `data`：`{ exitCode, logs, result, calls: [{name, ok, error?}], timedOut?, interrupted? }`（logs/result 截断至 100k 字符）
- **blocks 透传**：内部工具产生的图片/图表/文件块去重（`type:path`）限量（10 个）透传到 js 结果——编排 read 图片/产物类工具时图像块直达 UI/模型，不再丢失

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
| `/`、`/assets/*` | 内置 Web UI 静态资源（浏览器/WebView 直接访问） |
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
- **路径前缀挂载**：支持 `GEBAI_BASE_PATH`（如 `/gebai`）将整体挂载到业务域名子路径下，静态资源、`/api/*`、`/ws` 均以该前缀为基准解析，前端资源内引用的路径自动带上前缀
- **WebSocket 代理**：代理需开启 Upgrade/Connection 透传（`ws://` 路径同为 `{base}/ws`），服务端依据标准 WebSocket 握手，可与 HTTP 同一 location 规则转发
- **代理头透传**：支持 `X-Forwarded-For` / `X-Forwarded-Proto` / `X-Forwarded-Host`，用于生成正确的回调地址与日志记录；`GEBAI_TRUST_PROXY`（或等价）控制是否信任代理头
- **HTTPS 终结**：代理侧终结 TLS 后转发明文即可，WebSocket 使用 `wss://`，无需服务端额外证书配置
- **示例（Nginx）**：
  ```
  location /gebai/ {
      proxy_pass http://127.0.0.1:3000/;
      proxy_http_version 1.1;
      proxy_set_header Upgrade $http_upgrade;
      proxy_set_header Connection "upgrade";
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
- **状态快照（`state.snapshot`）**：连接建立（本地模式）/登录成功（服务模式）后服务端自动推送，含当前会话/会话列表/运行中会话/日志基线 `lastSeq`/**模型上下文窗口 `maxContextTokens`**（engine `contextWindow()` 取自 provider capabilities，标题栏上下文占比显示用，0=未知）；客户端作为模型基线，也可主动请求（幂等）；**SDK 快照模型保留 `maxContextTokens` 字段**（重建快照不丢弃，标题栏占比显示的数据源）
- **重连自动重新认证**：WS 连接无法携带 Header，SDK 持有令牌/API Key 时在每次建连后自动发送 `auth.login { token }` / `auth.login { apiKey }` 恢复用户上下文（服务模式重连不掉回未登录态）
- **流式任务断线恢复**：`sendPrompt` 断线时**挂起**（不抛错），等待自动重连后按 `seq` 重放离线事件无缝续流；任务未确认接收时以快照判定（运行中→恢复；已跑完→从存储合成收尾；未开始→补发一次，遇协议错误码 `already_running` 视为已接受）；`overrun` 走全量重同步（`resume` 重置 + 存储内容重建）。**重放事件经全局 `onEvent` 分发**（与在线推送同通道）：离线期间的审批/选择/工具事件全局订阅者（前端卡片渲染）也能收到——重连后审批卡/工具卡可恢复，而非只进 `sendPrompt` 的 chunk 通道（此前离线窗口内的审批请求永远不渲染、任务卡死至超时）
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
| `session.switch` | 切换当前会话 |
| `session.env.get` | 获取会话环境变量（内存态，含来源层级） |
| `session.env.set` | 设置/覆盖/删除会话环境变量（内存写入，不落盘） |
| `session.compact` | 主动压缩会话上下文（支持范围参数） |
| `session.todo.get` | 获取会话待办清单 |
| `session.tool.get` | 获取当前生效工具集（含启用状态） |
| `sub_agent.list` | 列出可用子Agent |
| `sub_agent.load` | 装载子Agent 模块（按需装载；可选 `sessionId`——传入时装载到该会话：注册工具 + 提示词消息写入会话记录，缺省仅全局注册工具） |
| `session.files.list` | 列出会话临时文件 |
| `session.files.get` | 读取会话临时文件内容 |
| `session.prompt` | 发送对话消息（流式返回 Chat 内容，支持附件引用） |
| `session.attachment.upload` | 上传附件（多模态，二进制分段或整体传输） |
| `session.current` | 获取当前会话 |
| `session.cancel` | 取消当前任务 |
| `session.compact` | 主动压缩会话上下文（支持范围参数，返回压缩条数与摘要） |
| `session.prompt` | 发送对话消息（WS 通道发起任务，流式事件经 `event.*` 推送回流，reply 仅确认；payload 支持 `messageId` 可选字段，语义同 REST prompt） |
| `session.attachment.upload` | 上传附件（WS 通道，base64 整体传输，返回会话内引用路径） |
| `choice.decide` | 提交用户选择（ask_user 工具阻塞等待的回应；`option` 单选 / `options` 数组多选 / `refuse: true` 或 option、options 均缺失表示拒绝回答） |
| `env.decide` | 提交用户填写的环境变量值（ask_env 工具阻塞等待的回应，envId + value；value 缺失表示拒绝提供） |
| `draw.result` | 提交前端渲染结果（draw 工具阻塞等待的渲染回传，renderId + ok + error） |
| `capture.result` | 提交前端页面捕获结果（page_capture 工具阻塞等待的捕获回传，captureId + html + imageBase64[可选，png/jpeg data URL 或裸 base64] + error[可选]） |
| `feedback.list` | 查询反馈（管理员可查全部用户，普通用户仅本人；支持 messageId/sessionId/type 过滤） |
| `user.list` / `user.create` / `user.update` / `user.delete` | 用户管理（服务模式，仅管理员） |
| `approval.decide` | 审批决策（通过/拒绝，对应待审批的工具调用） |
| `feedback.submit` | 提交反馈（点赞/点踩/文字，关联会话与消息） |
| `state.snapshot` | 获取状态快照（主动请求，幂等；与自动推送 payload 一致） |
| `sync.request` | 断线补偿：按 `lastSeq` 请求重放离线期间错过的日志事件；`sync.replay`（`{events, lastSeq}`，seq 严格递增、payload 含 sessionId）或 `overrun`（`{events:null, overrun:true, lastSeq}`，缺口→客户端全量重同步） |
| `auth.login` | 登录支持两种形式：`{username, password}`（密码）与 `{token}`（已有令牌，SDK 重连自动重新认证用）；成功后连接级事件订阅自动重绑到新用户并推送状态快照 |

服务端主动推送事件（`id` 为空，`type` 以 `event.` 为前缀，**均携带 `seq`**——每用户日志序号，重连后按 seq 重放补偿；旧服务端不携带 seq）：

| type | 说明 |
|------|------|
| `event.message.delta` | LLM 文本增量（流式）；子Agent 执行过程的文本增量携带 `session: true` + `sessionRunId`（前端渲染进子Agent 折叠容器） |
| `event.message.done` | 一条完整消息生成完成（子Agent 轮的 done 同样携带 `session: true` + `sessionRunId`） |
| `event.message.reasoning` | 推理内容增量（reasoning_content/thinking，前端流式推理中展开实时展示、推理完成自动折叠）；子Agent 执行过程的推理增量携带 `session: true` + `sessionRunId` |
| `event.session.start` | 新会话 run 开始（含 runId + agents 列表 + input + depth；**每轮重推、同 runId 幂等**——前端容器已存在则忽略，容器随消息重载丢失后据此重建） |
| `event.session.done` | 新会话 run 结束（含 runId + agents + output[最终返回]；异常时 output 为空并携带 error），前端折叠容器并写入返回摘要 |
| `event.tool.call` | 工具开始执行（含名称与参数）；子Agent 执行过程中的工具调用携带 `session: true` + `sessionRunId` |
| `event.tool.result` | 工具执行结果（含截断标记与文件路径）；子Agent 执行过程中的结果携带 `session: true` + `sessionRunId` |
| `event.todo.update` | 待办清单变更（新增/状态/进度） |
| `event.todo.continue` | 待办续做轮次（会话完成时仍有未完成待办，已追加提醒消息继续会话；含 round/remaining） |
| `event.draw.render` | 画图渲染请求（draw 工具执行中推送，含 renderId + 图表源码 + **`format` 图表语言**（mermaid/plantuml/d2），前端按语言本地渲染后经 `draw.result` 回传） |
| `event.env.request` | 环境变量填值请求（ask_env 工具执行中推送，含 envId + name + description + secret[是否敏感值]，前端弹窗填值后经 `env.decide` 回传；值注入本次任务 env 并保存到浏览器本地） |
| `event.capture.request` | 页面捕获请求（page_capture 工具执行中推送，含 captureId + fullPage[是否整页截图]，前端捕获当前页面渲染后 DOM html + 截图后经 `capture.result` 回传） |
| `event.approval.request` | 审批请求（含工具信息与重试次数） |
| `event.cron.run` | 定时任务触发（含任务 ID/类型/名称/周期，见「定时任务」） |
| `event.cron.result` | 定时任务脚本执行结果（成功/失败与输出；prompt 型结果经消息流呈现） |
| `event.task.done` | 本轮任务完成 |
| `event.task.error` | 本轮任务出错（含错误信息） |
| `event.model.error` | **模型服务异常（非终态，引擎将自动重试）**：接口异常/空响应重试前推送 `{error, retry, maxRetry}`——重试退避期间任务无输出，前端据此显示「模型服务异常，正在自动重试」瞬时提示（文本恢复/任务结束时移除）；重试耗尽的最终失败仍走 `event.task.error` |
| `event.session.ctx` | 运行中上下文大小更新（每轮模型调用后推送，含 ctxTokens token 计数：真实 usage 基准 + 未发送增量估算，无真值时全量估算兜底；会话列表 k 显示用） |

另有一个**非事件推送**：`state.snapshot`（`id` 为空，建连/登录后自动推送状态快照，客户端更新 MVC 模型并触发 `onSnapshot` 订阅）。

#### REST HTTP（同步通道）

面向业务系统同步集成，同一套核心能力以 HTTP 形式暴露，返回 JSON：

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/health` | GET | 健康检查（**公开端点**，服务模式下也无需鉴权——探活/负载均衡探测） |
| `/api/v1/users` | GET/POST | 用户列表/创建（服务模式，管理员） |
| `/api/v1/users/:id` | PATCH/DELETE | 用户启用/禁用/删除（管理员） |
| `/api/v1/sessions` | GET/POST | 会话列表/创建 |
| `/api/v1/sessions/:id` | GET/DELETE/PATCH | 会话详情/删除/重命名 |
| `/api/v1/sessions/:id/prompt` | POST | 发送消息，**非流式 JSON 返回**（同步等待任务完成，`{ message: 最终 assistant 消息, error?: 任务错误 }`）；body 支持附件引用、`env`（浏览器本地环境变量，临时注入仅本次任务生效，不持久化）与 `messageId`（可选：客户端生成的用户消息 id，撤回/反馈定位用；非法格式服务端回退自动生成）；**流式输出请走 WebSocket（`/ws`）** |
| `/api/v1/sessions/:id/attachments` | POST | 上传附件（multipart，多模态内容），返回会话内引用路径 |
| `/api/v1/auth/me` | GET | 当前登录用户信息（服务模式；本地模式为 admin 用户） |
| `/api/v1/auth/exchange` | POST | **外部身份兑换**（服务模式 + 已配置验证器；body `{username, credential}`）：网站本地登录态 → GEBAI 令牌；400 缺参 / 401 验证失败 / 404 未启用 |
| `/api/v1/auth/external-config` | GET | **外部身份扩展点探测**（公开端点）：`{enabled, storageKey?, autocreate}`，供 Web UI 启动时读取（不泄露密钥） |
| `/api/v1/sessions/:id/truncate` | POST | 撤回消息：删除 before（消息 id）及其后的所有消息（body: `{ before: string }`）；消息 id 为持久化 id——前端发送时携带 `messageId` 后撤回可对齐当前会话刚发的消息 |
| `/api/v1/sessions/:id/cancel` | POST | 取消当前任务 |
| `/api/v1/sessions/:id/approval` | POST | 审批决策（通过/拒绝） |
| `/api/v1/sessions/:id/choice` | POST | 选择决策（ask_user 工具等待的用户回应，body: choiceId + option 单选 / options 数组多选 / refuse=true 拒绝，option、options、refuse 至少其一，options 不得为空） |
| `/api/v1/sessions/:id/draw` | POST | 画图渲染结果回传（draw 工具等待的前端渲染结果，body: renderId + ok + error） |
| `/api/v1/feedback` | POST/GET | 提交/查询反馈（管理员可导出） |
| `/api/v1/sessions/:id/env` | GET/PUT | 获取/设置会话环境变量（内存态，不落盘） |
| `/api/v1/sessions/:id/compact` | POST | 主动压缩会话上下文（body 可指定范围） |
| `/api/v1/sessions/:id/todos` | GET | 获取会话待办清单 |
| `/api/v1/sessions/:id/files` | GET | 列出会话临时文件（含子目录，返回路径/大小/修改时间） |
| `/api/v1/sessions/:id/files/content` | GET | 读取会话临时文件内容（`?path=`，文本截断预览或原始内容） |
| `/api/v1/sessions/:id/files/download` | GET | 下载会话临时文件（`?path=`，二进制/文本原样下载，`Content-Disposition`） |
| `/api/v1/sessions/:id/files/download` | POST | 多选打包下载（body 指定 paths 列表，返回 zip） |
| `/api/v1/tools` | GET/PATCH | 工具集查询/启停配置 |
| `/api/v1/sub-agents` | GET | 子Agent 能力列表（名称、描述、工具、打包状态） |
| `/api/v1/webhooks` | GET/POST/DELETE | Webhook 注册/管理 |
| `/api/v1/mini-tools` | GET | HTML 小工具列表（公用全部 + 本人私有；同名私有覆盖公用，不含 html 源码） |
| `/api/v1/mini-tools/:name` | GET/DELETE | 读取单个 HTML 小工具（含源码；私有优先）/ 删除（`?scope=private\|public`，私有仅本人） |

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
- **OpenAPI 规范**：REST 端点自动生成 OpenAPI 文档（`/api/docs`），业务系统可据此生成任意语言客户端（Java/Go/Python 等）
- **任意前端接入**：任何支持 WebSocket/HTTP 的前端（React/Vue/小程序/App 等）均可直接对接双通道 API，不绑定 UI
- **Web UI 嵌入**：内置 Web UI 支持 iframe 嵌入业务系统页面，通过 URL 参数携带令牌免登录，并可通过参数指定 UI 风格/自定义主题变量
- **接口认证**：REST 支持 `Authorization: Bearer <token>`（先登录获取令牌）与 HTTP Basic（单次请求直验账号密码，复用登录限流、不签发令牌）两种方式，WS 统一 `auth.login`；不提供独立服务令牌（原 `X-API-Key` 服务身份机制已移除）
- **外部身份扩展点（同源集成）**：服务模式下网站可复用自身登录态作为 GEBAI 用户——配置 `GEBAI_EXTERNAL_AUTH_*` 后，前端把本地登录态经 URL 参数（`?gb_ext_username=&gb_ext_credential=`）或 localStorage（`GEBAI_EXTERNAL_AUTH_STORAGE_KEY`，同源直读）交给 Web UI，Web UI 启动时自动调 `POST /api/v1/auth/exchange` 兑换令牌（HMAC 验签或 HTTP 回调验证，见「认证与鉴权」）；业务系统也可用 SDK `exchangeExternalUser` 自行对接（React/Vue 等任意前端），无需依赖内置 UI
- **身份对接**：服务模式下支持对接外部 SSO/OIDC（可选），复用业务系统已有账号体系
- **审批集成**：审批请求可通过 REST/Webhook 转发到业务系统审批流，而非局限于内置 UI

### 飞书机器人集成

支持将 GEBAI 接入**飞书机器人**，用户可在飞书聊天中直接使用 Agent 能力（`GEBAI_FEISHU_BOT_ENABLED=true` 启用，需配置 `GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`）：

- **接入方式**：**长连接模式**（已实现）——服务端主动出站连接飞书，无需公网回调地址，本地桌面/服务端均可使用。协议为飞书现行 protobuf 帧协议（参照官方 SDK `lark_oapi`）：`POST /callback/ws/endpoint` 端点发现（`{AppID, AppSecret}`）→ WebSocket 连接（URL 携带 `device_id`/`service_id`）→ 心跳 ping（服务端下发 `PingInterval`，pong 可动态更新配置）→ 事件 DATA 帧（schema 2.0 JSON，`sum>1` 分片按 `message_id` 合包）→ 处理完回发 ACK 帧（`{"code":200}` + `biz_rt` 耗时）；**卡片交互帧（`type="card"`，card.action.trigger）**同样路由到 `onCardAction` 回调，ACK 帧 payload 回填回调响应 JSON（`card`/`toast`，缺省 `{}`）；断线按 `ReconnectInterval` 自动重连（首次抖动 `ReconnectNonce`）。帧编解码为自研极简 protobuf 实现（`feishu-bot/pb.ts`），不依赖第三方 SDK
- **身份映射**：本地模式全部映射到 admin 用户；服务模式按飞书用户 `open_id` 自动创建 GEBAI 映射用户（用户名 `feishu_{sha256(open_id)前24位}`——open_id 可含大写/超长，直接拼接过不了用户名白名单，哈希派生确定性防碰撞；随机密码不可密码登录，角色普通用户，管理员可在用户管理禁用），10 分钟内存缓存；**映射用户创建失败即中止任务**（绝不以默认 admin 兜底运行——那会让飞书侧任务以沙箱豁免身份执行）
- **会话映射**：每个飞书单聊/群聊自动关联独立 GEBAI 会话（**会话 id = `sha256("feishu:"+chat_id)` 前 32 位 hex**——满足存储层会话 id 白名单 `[0-9a-f]{32}`，`feishu_{chat_id}` 形态会被 `sessionPath` 拒绝；确定性派生，重启不变；会话名取飞书会话名称），消息上下文与 Web UI 完全互通；群聊成员共享同一会话（引擎身份为会话创建者，不因他人发言重建/覆盖）
- **消息互通**：文本消息（群聊自动剥离 `@_user_N` 提及占位）→ `session.prompt`；图片消息 → 下载为附件（魔数探测 mime）进入会话；Agent 流式回复实时推送——增量文本节流合并（60 字符/1.5s）为「✍️ 预览」消息（发新撤旧，撤回尽力而为），任务完成发最终 **interactive 卡片**（`lark_md` 渲染 Markdown，超长截断并提示 Web UI 查看），工具调用发「🔧 正在执行」状态消息（同任务至多一条）
- **画图（`draw`）后端渲染**：飞书通道不再依赖前端渲染，由桥接**后端直接渲染成图片**——`event.draw.render` 到达后，桥接调用**四语言组合渲染器**（`core/diagram-render.ts`：plantuml = `@plantuml/core` TeaVM 引擎本地渲染 SVG（零网络，浏览器 DOM API 以极简 shim 垫层运行）、mermaid = mermaid + happy-dom 垫层（getBBox 几何估算覆盖）、d2 = `@terrastruct/d2` WASM（二进制模式内嵌产物物化到 `{GEBAI_HOME}/vendor/d2js/`）、echarts = npm 包 SSR 渲染（`ssr:true` + SVGRenderer 零 DOM）→ `@resvg/resvg-js` 栅格化 PNG，浅色主题白底图，单一串行队列防引擎/全局环境冲突，超长按上限等比缩放）→ PNG 落盘会话 `tmp/{name}.png`（与源码文件并列，Web UI 文件面板可见）→ 上传飞书图片（`im/v1/images`，multipart）→ 发送 `image` 消息 → 经 `decideDrawResult` 回传引擎（成功才返回成功；失败把渲染错误回传模型供修正源码；`EngineBotAdapter.onDraw` 透传 format 字段）；**该渲染器同样供 Web 通道 `draw` 工具 `render=backend` 复用**（引擎经 ToolContext `renderDiagram` 惰性加载，落盘 `tmp/{name}.png` 并返回 `image` 内容块）
- **选择（`ask_user`）交互卡片**：`event.choice.request` 到达后发送**交互式按钮卡片**（选项按钮每行至多 5 个，`value` 携带 `choiceId`+`act`+选项值）替代前端选择卡；按钮点击经卡片交互帧回传——单选立即 `decideChoice`，多选切换勾选（点击回包 `card` 字段更新卡片按钮态与「已选」提示），「✅ 完成选择」提交勾选集合、「❌ 拒绝回答/放弃」提交拒绝；**已决策的卡片经 ACK 响应更新为终态**（「✅ 已选择：X」/「❌ 已放弃回答」，按钮不可再点）；**仅任务发起者可作答**（与审批授权一致，防群聊成员越权，他人点击回 toast 拒绝）；任务结束（完成/错误）撤回待作答卡片并清理状态
- **接口层桥接（多轮交互 + 仅最终回复）**：飞书 bot 不直接接触 AgentEngine/EventBus（不侵入引擎层），经 `BotPromptAdapter` 接口运行——固定 `interactionMode: "multi_turn"` + `outputMode: "final_only"`；引擎事件流由 `EngineBotAdapter` 映射为语义回调（onApproval/onChoice/onDraw/onDone/onError/onEnd），**过程事件（工具调用状态、文本增量、推理、新会话执行过程）不推送**，回复仅最终消息（无打字机预览）；**收到消息先给用户消息添加「Typing」表情反应模拟「正在输入」（飞书开放平台无 typing 接口——实测 `POST /im/v1/messages/{id}/typing` 与 `POST /im/v1/chats/{id}/input_status` 均 404；改用 Message Reaction API `POST /im/v1/messages/{message_id}/reactions`，body `{"reaction_type":{"emoji_type":"Typing"}}`——`emoji_type` 必须为[官方表情文案说明](https://open.feishu.cn/document/server-docs/im-v1/message-reaction/emojis-introduce)中的标准 ID（`Typing` 为敲键盘表情，传 emoji 字符会报 231001），响应 `data.reaction_id` 为唯一标识；权限 `im:message` 或 `im:message.reactions:write_only` 任一即可），输出完成（最终回复/出错/兜底）后撤回该反应（`DELETE /im/v1/messages/{message_id}/reactions/{reaction_id}`），不发送额外表情/状态消息；**最终回复（interactive 卡片）、错误回复与任务完成兜底提示均以「回复」形式引用原消息发送（`POST /im/v1/messages/{message_id}/reply`，客户端显示引用气泡；message_id 不合法时回落普通发送）**；依赖实时前端的工具（`page_capture`/`render_html`/`ask_env` 及 widgets 四工具（save/list/get/delete），声明 `interaction: "realtime"`）由引擎按交互模式自动从模型 schema 中移除（含子Agent 命名空间同名工具 `{agent}_{tool}`），被调用时阻止执行并返回「当前通道不可用」说明（见「交互模式」）；`ask_user`/`draw` 声明 `multi_turn` 可用，走飞书交互卡片/后端渲染；关键操作（requiresApproval）经审批回调卡片询问用户
- **交互命令**：`/help`、`/new`（清空当前对话上下文重建会话，**仅会话创建者可操作**）、`/sessions`、`/cancel`（**仅任务发起者可操作**）、`/approve`、`/reject`（批准/拒绝最近一个待审批工具调用，**仅审批发起者可操作**）、`/approval-skip`（**仅会话创建者可开启**——群聊成员不得为共享会话开启免审批；写入会话内存态 env，不落盘、服务重启后需重新开启）
- **审批**：Agent 审批请求推送提示消息（工具名 + /approve /reject 指引），文本命令式完成审批
- **富文本**：最终回复卡片渲染 Markdown；长内容截断附 Web UI 会话提示（会话文件下载依赖 Web UI 访问）
- **配置**：`GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`（与 `feishu_docs` 子Agent 共用全局凭证）+ `GEBAI_FEISHU_BOT_ENABLED`
- **飞书 TLS 策略**：`GEBAI_FEISHU_INSECURE_TLS=true` 时**所有飞书出站请求禁用 TLS 证书校验**（内网代理/中间人证书场景）——机器人桥接 REST 请求（token/消息/图片，`feishu-bot/api.ts`）、长连接 endpoint 发现与 WebSocket（`feishu-bot/conn.ts`，Bun WebSocket `tls.rejectUnauthorized=false`）、`feishu_docs` 子Agent 全部接口与 OAuth 兑换/用户信息（`feishu-bot/tls.ts` 共享 `feishuFetch`/`feishuWsOptions` 助手，注入 `fetch`/`WebSocket` 的 `tls.rejectUnauthorized=false`；仅在可信内网开启）
- **安全**：长连接为出站连接（不暴露回调端口）；凭证仅存环境变量（本地 `.env` 不入版本库）；服务模式按飞书用户隔离数据与权限（映射用户为普通角色）；消息/会话 id 白名单校验（`[A-Za-z0-9_-]{1,64}`）防路径注入；卡片按钮回调的 chatId/openId 同样白名单校验 + 发起者授权；**事件处理异常全程捕获**（async 回调 rejection 不得成为 unhandled rejection）；会话归属/映射用户缺失时中止而非兜底
- **实现**：`packages/server/src/feishu-bot/`（`pb.ts` protobuf 帧编解码、`protocol.ts` 帧协议/合包/ACK、`conn.ts` 长连接客户端（事件 + 卡片交互帧）、`api.ts` 开放平台 API（token 缓存/消息收发/图片上传下载/会话信息）、`tls.ts` TLS 策略助手（`GEBAI_FEISHU_INSECURE_TLS` 禁用证书校验，机器人/feishu_docs/OAuth 共用）、`plantuml.ts` 后端渲染器（DOM shim + TeaVM 引擎 + resvg）、`bot.ts` 桥接编排（依赖全部注入））；单测 98 用例（真实凭证握手验证通过）；Webhook 回调模式（公网 HTTPS + 签名验签）为后续迭代

### 子Agent文件格式

**单文件形式**（`sub-agents/{name}.ts`）：

```ts
export const name: string
export const description: string          // 能力描述，注入系统提示词供总Agent 决策
export const systemPrompt: string
export const tools: ToolSet               // 子Agent 自有工具（注册为 {agent}_{tool}）
export function toolSchemas(): ToolSet
export const requiresApproval?: Record<string, boolean>
export const preload?: boolean            // 是否预加载（默认 false，按需装载）
// 工具级安全模式自主声明（Tool.safeMode，写在各工具定义上）：
//   true  = 作者判定安全模式下可提供（即使短名风险如 xxx_sh——须自行保证实现只读或体内按 ctx.safeMode 校验）
//   false = 作者判定安全模式下不提供（即使名字无风险命中，如内部写文件/外发请求的工具）
//   未声明 = 按短名风险规则默认（{agent}_sh / {agent}_write / {agent}_file / {agent}_delete / {agent}_cron_add 等 _risk 后缀命中则不注册）
// 注册期过滤（ToolRegistry({safeMode})）：不注册即 schema 不可见、调用报未知工具，见「安全模型 → 安全模式」
export const envVars?: EnvCatalogVar[]    // 可配置环境变量声明（{AGENT_NAME_UPPER}_ 前缀），汇总进环境变量目录（前端面板白名单，见「环境变量配置」）
export const writeGuard?: (env: Record<string, string>, absPaths: string[]) => string | null
// 写范围守卫声明：会话装载本子Agent（或新会话预加载）后注入 ToolContext.writeGuard——
// 文件写类工具（write/edit/patch/file（rename/move/delete））写入前以解析后的绝对路径调用，
// 返回非空字符串 = 拒绝写入（self_optimize 用它实现「核心引擎源码默认只读」的代码级强制）
```

**目录形式**（`sub-agents/{name}/{name}.ts` + 可选 `{name}.md`）：

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
- 工具的返回类型（text/json）在 `toolSchemas()` 中声明，通过 schema 的 `returns` 字段体现，不单独导出 `returnFormats`
- `agent_run` 执行新会话：输入为 `agents`（子Agent 列表）+ `input`（任务文本），返回为 `output`（文本），新会话（预加载子Agent）自行规划执行（仅阻塞执行返回一种方式）
- 子Agent 内无全局可变量，工具函数体为纯函数或依赖注入工厂，天然支持并发加载
- 子Agent 可复用全局工具能力：`tools` 中可直接引用全局工具实现（如 `read`/`write`/`sh`），引用时以自身命名空间暴露（`{agent}_read` 等），路由由总Agent 转发
- 示例：内置 `desktop` 即目录形式（`sub-agents/desktop/desktop.ts` + `desktop_tools.ts`）

**纯提示词简化定义**（`sub-agents/{name}/{name}.md` 单独存在，零 TS）：用于简单子Agent 与组合式子Agent

```md
---
description: 一句话能力描述（可选；缺省取正文首行）
---
系统提示词正文（说明编排策略/角色职责等）
```

- 目录内无同名 `{name}.ts` 时，加载器直接由 md 构成定义（`name`=目录名）；可选 frontmatter 仅识别 `description`
- 无工具的简化定义在新会话运行环境**自动注入编排工具**（`agent_list`/`agent_load`/`agent_run`，原名暴露、无 `{agent}_` 前缀），支持组合式子Agent 编排装载/执行新会话其他子Agent（受递归深度 3 层限制）
- 有工具的子Agent 需要编排能力时，可显式引用导出的 `agentListTool`/`agentLoadTool`/`agentCallTool`（`core/tools.ts`），注册为 `{agent}_agent_run` 等带前缀形态
- 示例：纯 md 组合子Agent 即 `sub-agents/{name}/{name}.md`（零 TS），编排其他子Agent 产出完整链路的组合能力可由此模式定义

## 桌面端架构

基于 **tao + wry（系统原生 WebView：Windows WebView2 / macOS WKWebView / Linux WebKitGTK）** 实现桌面应用，复用服务端内置 Web UI，无需额外前端框架、无 Tauri 框架依赖。产物为两个独立可分发的 exe：

- **`dist/gebai.exe`（纯 Bun 单文件，浏览器形态）**：`bun build --compile` 产物（入口 `src/index.ts`），同进程启动服务端并自动打开系统默认浏览器（`GEBAI_NO_OPEN=1` 时不打开）；Web UI、子Agent、D2.js、playwright driver 与 playwright-core 等全部内嵌，单文件即可运行，也可作为服务端直接部署（`--server`）
- **`dist/gebai-desktop.exe`（原生 WebView 启动器）**：Rust 单文件（`launcher/`，约 180 行），构建时经 `include_bytes!` **内嵌整个 `gebai.exe`**；启动时物化到用户数据目录（Windows `%LOCALAPPDATA%\gebai\gebai-server.exe`，长度不一致即覆盖）并 spawn（`GEBAI_NO_OPEN=1` + `CREATE_NO_WINDOW`），从侧车 stdout 解析监听端口（`[gebai] listening on http://HOST:PORT`），WebView 导航到 `http://127.0.0.1:{port}`；窗口关闭（`CloseRequested`/`LoopDestroyed`）时一并 kill 侧车；WebView 用户数据目录固定在 `%LOCALAPPDATA%\gebai\webview\`（不在 exe 旁落地）
- **单文件自带 UI**：构建时 `scripts/build-web-bundle.ts` 把 `packages/web/dist` 全量资源 base64 内嵌为 `web.bundle.generated.ts`（gitignore），随服务端编译进二进制；二进制模式（`webDist` 不存在）静态路由改从内嵌资源提供，`/` 与 `/assets/*` 均可访问——单文件二进制完整可用（含 Web UI + 全部子Agent）；脚本调试模式该文件缺失时服务端自动回退（不内嵌 UI），无需预先构建
- 桌面端与浏览器访问共用同一套 Web UI、同一套服务端核心，仅宿主不同
- **端口固定（桌面形态）**：桌面形态（`gebai-desktop.exe` 侧车与 `gebai.exe` 浏览器形态）默认固定端口 **47896**（`DESKTOP_PORT`，不易冲突——避开常见开发端口与 Windows 临时端口段 49152+）。核心目的是保持 origin（协议+主机+端口）稳定：浏览器/WebView 的 localStorage 按源隔离，端口每次随机变化会使环境变量（`gebai.ui.env`）、主题、快捷键等全部浏览器本地数据跨重启「凭空丢失」（用户环境变量仅存浏览器本地，服务端零留存，无兜底可恢复，见「环境变量隔离」）。端口被占（如重复启动）**直接报错退出，不回退随机端口**——随机 origin 正是数据丢失的根因；显式 `GEBAI_PORT` 或调用方 overrides 优先。服务端部署形态仍默认 3000
- 数据目录 `GEBAI_HOME` 默认 `~/.gebai/`；侧车物化采用**内容哈希比对**（长度相同再比 SipHash，防同尺寸不同内容的陈旧残留）

### 构建与产物

- 构建链：`bun run --cwd packages/desktop build` → `server:build`（图标生成 → web 构建 → web bundle → 子Agent bundle → D2.js 内嵌 → driver 内嵌 → playwright-core 内嵌 → `--compile` 产出 `dist/gebai.exe`）→ `launcher:build`（cargo release 编译 `launcher/` 并复制为 `dist/gebai-desktop.exe`）
- **品牌图标管线**：canonical 源为 `packages/web/public/favicon.svg`（大脑造型）；`scripts/gen-icon.ts` 用 resvg 渲染多尺寸打包为 `icons/icon.ico`（gitignore，16–128 无压缩 BMP + 256 PNG），并回写 `packages/web/index.html` 的内联 favicon data URI。exe 嵌入**不走 bun `--windows-icon`**（其会改动像素/alpha，与 rcedit 同样不可用）：`gebai.exe` 由 `scripts/embed-icon.ts` 经 kernel32 `UpdateResource`（bun:ffi）原样写入 RT_GROUP_ICON+RT_ICON 资源（32px 条目排首位，兼容简化提取器）；`gebai-desktop.exe` 经 `winresource`（`launcher/build.rs`，`rerun-if-changed` 聟动重建）——网页 favicon 与两个 exe 图标同源。**运行时窗口图标**（任务栏/标题栏）不属于 exe 资源，需窗口类显式设置：gen-icon 同时产出 `icons/icon32.rgba`（32px 原始 RGBA），launcher `include_bytes` 内嵌后经 tao `with_window_icon` 挂到窗口
- 产物：`packages/desktop/dist/gebai.exe` 与 `packages/desktop/dist/gebai-desktop.exe`（`launcher:build` 依赖 `dist/gebai.exe` 已存在，故构建顺序固定）
- 要求：Rust 工具链（仅启动器需要）、WebView2 运行时（Windows 自带）；`gebai.exe` 单独分发时无需任何 Rust 依赖
- playwright 子Agent 的 node 桥接驱动 `driver.mjs`：服务端 dist（非编译）形态由 `scripts/build-subagents.ts` 复制到 dist/ 与入口同目录；二进制形态由 `scripts/build-driver-embed.ts` 生成内嵌产物 `driver.embedded.generated.json`（gzip base64），运行时物化到 `{GEBAI_HOME}/vendor/playwright/driver.mjs`（playwright 子Agent 仍需运行机器具备 node 与 playwright 包）

### 启动方式

- 浏览器形态：直接运行 `gebai.exe`，自动打开系统浏览器访问本地服务
- 桌面形态：运行 `gebai-desktop.exe`，启动器物化并拉起内嵌服务端，原生 WebView 窗口加载内置 UI；核心逻辑与浏览器/服务端部署完全一致，仅宿主不同

- 关闭窗口时进程退出，内嵌侧车服务端一并退出（除非以服务端模式单独运行）

## 构建与发布

### 最终构建目标

单二进制可执行文件，通过 `bun build --compile` 产出，支持 Windows / Linux / macOS 三平台。同一二进制承载全部运行形态，分发物仅一个文件，**零运行时依赖**（Bun 运行时已内嵌，宿主机无需安装 bun/node/python 即可运行服务本身；脚本工具的解释器要求见「脚本执行环境」）：

- 本地桌面应用（WebView 宿主）
- 本地浏览器访问（服务端 + 浏览器）
- 服务模式（多用户公用）

Vite 构建产物（Web UI）、桌面端 WebView 宿主、子Agent 代码一并编译进二进制，运行时按启动参数/环境变量切换形态，无需区分构建，也无需在运行时读取子Agent 文件。

构建时支持**子Agent 选择性打包**（白名单/黑名单，见「子Agent」章节），不同发行规格（桌面全量版、服务端精简版）由同一源码产出：

```
bun run --cwd packages/server build:win                  # 全量子Agent 打包
bun run --cwd packages/server build:win --sub-agents a,b # 仅打包指定子Agent
bun run --cwd packages/server build:win --exclude-sub-agents x
```

### 运行模式区分

代码必须区分运行模式，通过 `Bun.main === import.meta.path` 或环境变量判定：

| 模式 | 判定方式 | GEBAI_HOME | 子Agent 来源 |
|------|---------|------------|-------------|
| 脚本调试 (`bun run dev:*`) | `Bun.argv` 含源码路径 | 项目根目录 | `packages/server/src/sub-agents/` 源码直接引用 |
| 二进制运行 | 无源码路径，编译产物 | `~/.gebai/` | 编译时打包进二进制 |

二进制运行形态（本地模式 / 服务模式）由启动参数与 `GEBAI_MODE` 等环境变量决定，不影响二进制构建。

### 升级与兼容

- **原地升级**：新版本二进制直接替换旧文件重启即可，数据（`GEBAI_HOME`）与配置（环境变量）无需迁移
- **数据兼容**：会话/环境变量存储格式带版本字段，升级时兼容旧版本数据，必要时自动迁移
- **API 兼容**：`/api/v1` 语义化版本，破坏性变更升级主版本号；WebSocket 消息类型保持向后兼容（新增类型不影响旧客户端）
- **多实例共处**：单机可并排部署多实例（不同 `GEBAI_PORT` / `GEBAI_HOME`），便于灰度与多租户

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
- 单二进制构建（Web UI + 子Agent 打包）+ 桌面端 WebView + 反向代理/`GEBAI_BASE_PATH`
- 多套 UI 风格 + 上下文自动压缩 + 飞书机器人 + Webhook

**阶段五：完善**
- 数据生命周期/GC、升级兼容、自我优化产物管理、多实例

### 待实现（已知项，后续迭代）

| 待实现项 | 现状与影响 | 计划方案 |
|---------|-----------|----------|
| 前端消息虚拟化/分页 | 会话消息全量加载 + 全量渲染（`session.get` 返回全部消息，`loadMessages` 重建全部 DOM，逐条重跑 markdown 解析与代码高亮）；数千条消息的会话切回时开销显著。已缓解项：流式渲染 120ms 尾沿节流全模式统一（消除流式期间 O(n²) 重解析，历史全量重建开销仍在） | 分两步：① 服务端按游标分页 + 前端只渲染最近 N 条、上滚加载更早（半虚拟化，改动集中在 `sessions.ts`/`messages.ts`）；② 视口窗口虚拟化（与粘底滚动/消息导航/跨会话滚动位置记忆协同，需回归验证） |
| 子 Agent 运行期热加载 | `discover()` 仅启动时扫描一次，新增/修改子 Agent 必须重启进程生效（DESIGN 已如实标注「重启生效」）；self_optimize 改完子 Agent 后当前进程内无法验证成果 | dev 模式 fs watch → 绕过模块缓存重载 → registry 增量 diff + 会话内已装载提示词的迁移策略 + 失败保留旧版本回退；二进制模式无源码目录不适用 |

## 测试策略

统一使用 `bun test`，测试文件与被测代码同目录（`*.test.ts`）。

```bash
bun run --cwd packages/server test
bun run --cwd packages/sdk test
```

### 测试执行速度

全套测试是 AI 编码迭代与 `self_optimize` 优化闭环的准入凭证，执行速度直接影响迭代效率，三项结构化保障：

- **server 分片并行**：`packages/server` 的 `"test"` 脚本走 `scripts/test-parallel.ts`——全部 `*.test.ts` 排序后按轮转分成 N 个分片并行启动 `bun test` 子进程（bun test 单进程内测试文件串行执行，是全套件耗时主因；各文件相互独立、端口/临时目录均动态分配，并行安全）。默认 `min(8, max(2, CPU 核数))` 分片，`--shards=N` 参数或 `GEBAI_TEST_SHARDS` 环境变量覆盖；带文件路径/`-t`/`--coverage` 等参数时自动退回单进程透传（定向运行分片无收益）。输出逐行加 `[i/N]` 前缀流式透传，任一分片失败即非零退出。`test:serial` 保留串行入口
- **turbo `test` 不依赖 `^build`**：各包 `main` 均指向 `src/*.ts`（bun 直接执行 TS 源码），测试无需先构建依赖包——冷缓存/改动后跑测试不再先付 vite 构建 + wasm 内嵌 + `bun build --compile` 的构建成本
- **子Agent 发现进程级缓存**：`SubAgentManager.discover()` 首次扫描 `sub-agents/` 后缓存定义列表（目录内容进程内不变），测试中每个用例新建 manager 重复 discover 时跳过目录扫描/动态 import；运行期改动子Agent 源码经重启生效，行为不变

### 测试分层

| 层级 | 覆盖范围 | 要求 |
|------|---------|------|
| 单元测试 | 工具函数、命名空间解析、环境变量合并、路径沙箱、分片路径、截断/压缩算法 | 核心模块**必须**单测，零外部依赖（mock 注入） |
| 集成测试 | AgentEngine 主循环、审批/重试状态机、子Agent 调用、会话持久化 | 用 mock LLM Provider 跑完整流程，不触真实网络 |
| 契约测试 | WS/REST 消息格式、SDK 与协议一致性 | 协议变更必须有契约测试兜底 |
| E2E | 端到端（mock LLM + 内存存储）：prompt → 工具调用 → 审批 → 完成 | 阶段一闭环后建立，回归主路径 |

### 自测工具：fake-llm（假模型端到端自测）

`packages/server/scripts/fake-llm.ts`——无真实模型 Key 的**手工端到端自测**工具：按场景脚本回放 OpenAI `chat/completions` 流式响应（文本分片 / tool_calls 分片 / usage），服务端 provider 无感接入，用于浏览器实测前端行为（流式渲染、agent_run 执行过程、计划审批滚动等单测覆盖不到的 UI 链路）。

```bash
bun run --cwd packages/server fake-llm [场景]   # 场景缺省 = text；端口 9801（FAKE_LLM_PORT 覆盖）
# 服务端指向假模型联调：
GEBAI_LLM_API_BASE=http://127.0.0.1:9801/v1 GEBAI_LLM_API_KEY=test \
  GEBAI_LLM_MODEL=fake GEBAI_PORT=3900 bun run --cwd packages/server dev
```

| 场景 | 脚本 | 验证点 |
|------|------|--------|
| `text` | 单轮纯文本回复 | 链路冒烟（消息上屏/流式渲染） |
| `agent_run` | 主会话调 `agent_run` → 子会话两轮（文本 + `code_ls` 工具 / 多行结论）→ 主会话收尾 | 新会话执行过程实时渲染、结果 markdown 换行无双倍空行 |
| `plan` | 延迟 4s 调 `plan`（阻塞等审批）→ 批准后收尾 | 计划卡片全文、选择卡片弹出时消息流落底、审批后继续 |
| `error` | 每次调用 HTTP 500 | 错误气泡与重试耗尽呈现 |

场景脚本耗尽后回复固定收尾文本（防场景外调用死循环）；`/probe` 页面（`http://127.0.0.1:9801/probe`）连服务端 `/ws` 验证浏览器连通性（标题 `WS_OK`/`WS_ERR`/`WS_TIMEOUT`）。

### 覆盖率门槛

- 核心引擎（AgentEngine、ToolRegistry、EnvManager、Sandbox、命名空间解析）行覆盖率 ≥ 90%
- 工具函数 ≥ 80%；整体仓库 ≥ 70%（`bun run --cwd packages/server test:coverage` 产出报告）
- CI/本地提交前强制跑全量测试，失败即阻塞

#### 实现状态（初始落地）


- 已实现：`@gebai/server`（AgentEngine 主循环、LLMProvider（OpenAI 兼容 chat/completions + OpenAI Responses + Anthropic 三接口 SSE 解析，**usage 真值解析**（OpenAI `stream_options.include_usage` 末 chunk / Responses `response.usage` / Anthropic `message_start.input_tokens`+`message_delta.output_tokens`，统一挂 done chunk 的 `usage` 字段；服务端不返回时为 undefined → 引擎估算兜底，见「上下文占用口径」），**统一多模态内容块转换**（图片块 base64 内联 → OpenAI `image_url` / Anthropic `image`，`imageMessageBlocks` 助手），**额外模型接口参数 `GEBAI_LLM_EXTRA_PARAMS`（JSON，如推理强度 `reasoning_effort`）Provider 级 + 任务级（浏览器本地注入）两级覆盖，顶层合并进请求体**）、ToolRegistry 命名空间解析（含注册期前缀互斥校验）、SessionStore 分片持久化、EnvManager（全局 + 会话内存态，用户环境变量零留存）、Sandbox 路径沙箱/子进程、AuthService 多用户令牌、**外部身份扩展点**（同源部署集成网站：`POST /api/v1/auth/exchange` 外部身份 → GEBAI 令牌，验证器可插拔——`GEBAI_EXTERNAL_AUTH_SECRET` HMAC 验签（±10 分钟防重放）或 `GEBAI_EXTERNAL_AUTH_URL` HTTP 回调验证，互斥同设报错；`AUTOCREATE` 自动创建/白名单两种映射；Web UI 启动时 URL 参数或 localStorage 同源直读自动兑换，SDK `exchangeExternalUser`/`getExternalAuthConfig`；`external-auth.ts` 单测 13 用例 + 端点集成测试 6 用例）、EventBus、富内容块（text/code/image/file/diagram/diff/html）渲染、**视觉工具 `vision`**（额外多模态模型 `GEBAI_VISION_*` 配置，目标 `target` + 图片文件 `image` 参数，base64 内联调用视觉模型，未配置时回落到显式声明多模态能力的主模型，单图 8MB 上限，输出截断保护 + `image` 内容块；`makeVisionTool` 依赖注入可单测）、**图片附件链路**（本地模式附件源路径按会话根解析（修复 CWD 误解析）、`AttachmentRef` 存逻辑路径、多模态主模型 base64 内联/其余降级文本说明 + vision 指引、接口 HTTP 4xx 拒绝图片块时自动降级重试、Web 端 canvas 图片压缩 1280px/2MB）、**画图工具 `draw`**（**四种图表语言**——`format` 必选参数 `mermaid`/`plantuml`/`d2`/`echarts`，工具描述与参数说明内置选择指南指导模型按需选择；前端实时渲染确认：成功才返回、报错回传模型、5 秒超时降级；Web 前端本地渲染四语言各自引擎（mermaid npm 包 / `@plantuml/core` / `@terrastruct/d2` WASM / echarts UMD SSR 渲染 SVG，均零网络）；**后端组合渲染器四语言全支持**（飞书与 `render=backend` 通道：plantuml TeaVM 引擎 / mermaid + happy-dom 垫层（getBBox 几何估算覆盖防布局坍缩）/ d2 WASM（二进制模式内嵌产物物化 `{GEBAI_HOME}/vendor/d2js/`）/ echarts npm 包 SSR（`ssr:true` 零 DOM，顶层急切导入防 zrender 环境误判）+ 共享 resvg 栅格化（负原点 viewBox 平移归一兼容），全局环境切换 + 单一串行队列，`globalThis.window` 仅临时存在））、**HTML 页面工具 `render_html`**（沙箱 iframe 域隔离渲染：`allow-scripts` 不含 `allow-same-origin`，脚本可执行但隔离于 opaque origin，无法访问宿主页面 DOM/存储/顶层导航；落盘会话 `tmp/` 并返回 `html` 块，支持模型显式指定预览尺寸 `width`/`height`，未指定时 iframe 固定铺满消息流宽度、无任何内容宽度反馈）、**文件展示工具 `show_file`**（模型把文件**直接展示给用户**：按类型产出直显内容块——图片内联 `image`、图表源文件 `diagram` 渲染、`.html` 沙箱预览、文本/代码内联 `code`（≤512KB 读取/4 万字符截断）；无法内联类型回退 `file` 查看卡片（点击按需加载，PDF 内嵌渲染）；会话 `tmp/` 内直接引用，会话外文件复制到 `tmp/shown/{主名}-{内容哈希}.{扩展名}`（≤100MB）后引用）、**全局 diff 工具**（LCS 行级对比 + unified diff 文本，纯函数 `core/diff.ts`）、5 个内置子Agent（含 `desktop`、**飞书云文档 `feishu_docs`**：42 个工具覆盖文档 docx 创建/读取/块编辑/按文本反查块 id（`find_blocks`）/Markdown 导入导出/**插入图片（`insert_image` 三步流程）**/**思维导图画板读取（`get_board`：mindnote 块自动提取画板 token，结构化提取 PlantUML 源码或重建连接线流程）**/**用户授权（`auth_user_authorize`/`auth_user_token`/`auth_user_status`/`auth_user_clear`：OAuth code 流程配置 user_access_token，会话级存储+自动刷新，配置后资源操作以用户身份执行、创建用户所有权文档；**默认自动回调**——授权后浏览器跳回内置端点 `GET /api/v1/oauth/feishu/callback` 自动兑换写回会话（`GEBAI_PUBLIC_URL` 可配，见「飞书用户授权」））**、云空间、电子表格、多维表格、知识库、搜索、权限与 `api_call` 兜底；块列表附 `type_name` 标注、`page_all` 自动翻页（达上限提示）、小节读取、块操作失败本地诊断、**权限类错误码自动附所需 scope 与授权链接**；`FEISHU_DOCS_*` 凭证 + tenant_access_token 缓存；写操作审批；`createFeishuTools` 依赖注入 + `markdownToBlocks`/`blockText`/`extractBoardContent`/`extractOAuthCode` 纯函数 + 共享 OAuth 模块（`oauth.ts`：兑换/刷新/会话令牌存取/授权状态注册，工具与 REST 回调共用），单测覆盖率 95%）、**浏览器自动化 `playwright`**（无头 Chromium，15 个工具覆盖导航/读取/截图/交互/表单/JS 执行/多标签页；**node 桥接架构**——Bun 与 playwright driver pipe 兼容问题用常驻 node 子进程 JSON-RPC 规避，BrowserContext 按会话隔离 + 空闲回收，导航/交互/脚本类默认审批；`createPlaywrightTools` 依赖注入 + Bridge 协议层单测 17 用例，真实 chromium E2E 验证通过）、REST/WS 双通道 API（WS 消息处理独立 `ws.ts`、反馈存取独立 `feedback.ts`；REST 全端点：feedback 查询/导出、多选 zip 打包下载、Webhook CRUD、OpenAPI 文档、auth/me、**飞书 OAuth 回调 `/api/v1/oauth/feishu/callback`**）、**上下文压缩**（主动 + 自动 80% 阈值，LLM 摘要 + 滚动裁剪降级）、**Webhook 推送**（HMAC 签名、3 次指数退避重试、事件白名单、多用户会话归属过滤）、命名空间注册期碰撞校验、服务模式 admin 密码哈希引导（`GEBAI_ADMIN_PASSWORD_HASH`：设置启用/未设置禁用，启动参数权威配置）、会话操作归属校验）、`@gebai/sdk`（GebaiClient：含 getCurrentSession、webhook/工具启停/打包下载方法）、`@gebai/web`（Vite 聊天 UI，按功能域模块化拆分：state/messages/tool-cards/sessions/composer/attachments/approvals/settings/auth/markdown/diagram/diff/html-view/jump-bottom 等，样式按 base/chat/composer/overlays 分片；**Inter / JetBrains Mono 字体内置**（@font-face 随产物分发，不依赖目标机器字体）；富内容块渲染、历史加载、**交互式图表编辑（Mermaid/PlantUML/D2/ECharts 四语言本地渲染 + 主题适配——PlantUML skinparam 注入与渲染后颜色兜底修正、Mermaid 按 UI 明暗重新 initialize、D2 按 UI 明暗选主题 ID 0/200、ECharts 按 UI 明暗注入 darkMode）**、**diff 并排对比视图（按语言语法高亮，跨行着色平衡）**、**HTML 页面沙箱渲染（预览卡片 + 全屏查看器 + 源码/复制/下载，`render_html` 工具产物）**、**多套 UI 风格（12 套）**、**低性能模式（无 GPU 自动检测，降级动画/毛玻璃特效与图表导出采样，设置面板可手动覆盖）**、**单轮计时器（任务运行期间消息上实时显示本轮耗时，外观 tab 可关）**、**人民币招财猫（`cny` 主题专属：悬浮可拖动玩偶 + 拖动金币轨迹 + 点击连击爆金币 + 单轮完成大爆发（运行越久越多）；全屏 canvas 粒子引擎（金币 3D 翻转 + 六面额纸币各半，预渲染精灵），低性能/减少动画不发射）**、多用户登录页、会话重命名、压缩入口与压缩通知、设置面板（**浏览器本地环境变量增删改（localStorage，对本浏览器所有会话生效，随消息临时注入服务端、不落盘防泄露；服务端不配模型变量时仅前端配置即可使用；含 `GEBAI_LLM_EXTRA_PARAMS` 建议项，可按任务覆盖模型接口参数；一句话说明与保存按钮固定面板底部）**/外观性能模式/用户管理/反馈列表；工具启停、子Agent 装载与 Webhook 管理不设 UI，保留 SDK/API 方法）、**图表预览卡片随内容自适应**、**发送消息/切换会话自动锁定滚动到底（粘底跟随核心 `sticky-follow.ts` **意图驱动**——主消息列（`sticky-scroll.ts` 工厂 + 「跳到最新」按钮）/ 推理体（`reasoning-scroll.ts`）/ 新会话容器（`scrollSessionSticky`）三处共用同一工厂，可独立单测；「跳到最新」按钮显隐 = 跟随状态完全一致：按钮隐藏 = 跟随中（新内容持续滚动到底），按钮显示 = 用户在阅读历史不打扰。跟随状态只由三类信号翻转，**不做滚动事件位置取证**（目标位置比对 / 程序落位差值）——浏览器 scroll 事件异步合并送达，内容增长/收缩引发的钳制与布局调整同样产生滚动事件，取证式分类必被迟到事件误判为用户滚动、被过期落位误判为用户上翻（「自动滚动滚一段后失灵」的根因）：① **用户输入意图**（同步、必然先于其滚动效果到达，无竞态）：滚轮上滚 / 触摸上滑 / 向上滚动键（PageUp/ArrowUp/Home；输入框内方向键滚动的是文本光标不触发）/ 滚动条拖动（pointerdown 命中滚动条槽区：`offsetWidth-clientWidth` 宽度带）/ 消息导航跳转（`stopFollowing` 显式解除）→ 立即解除；② **几何贴底**：任何滚动事件落在阈值内（主列 64px / 容器 8px / 推理体 4px）→ 恢复跟随（滚回底部 / 收缩钳制到底收敛；小幅上滚在阈值内视为仍贴底继续跟随）；③ **静默窗口兜底**：未贴底且距最近程序滚动/DOM 变化超 80ms → 无法归因为内部动作，视为未知输入（中键自动滚动 / 查找定位 / 覆盖式滚动条拖动）→ 解除；窗口内的未贴底事件视为程序滚动/钳制的迟到事件：保持跟随并回正到底（续滚无需登记目标落位——旧实现须读回 clamp 后实际落位精确比对，且续滚自身不登记导致容器内跟随必现失效）。**粘底对齐保持（帧预算循环 240 帧）**沿用：内容高度存在不触发 MutationObserver 的异步修正（低性能模式 `content-visibility` 懒布局、字体/图片加载等）——跟随期间按帧续查对齐（每帧仅属性读取、贴底即空转）、预算耗尽自停（几何不可用/NaN 时停转防死循环）；新内容（DOM 变化 MutationObserver / 图片加载含 markdown 内嵌 `<img>` 由 `msgEl` 委托捕获阶段 load 监听 / ResizeObserver）触发跟随，rAF 节流每帧至多一次保证流式高频更新性能；会话切回滚动位置恢复走 `restoreScroll`：落位后按几何同步跟随状态，未决跟随回调（排期中的 rAF/对齐保持循环）按执行时状态自然失效——loadMessages 尾部先排期 scrollIfSticky rAF 再 lockToBottom 后恢复历史位置，未决回调不把恢复位置拽到底部）**、**消息质量反馈（助手消息 👍/👎 提交反馈，设置面板反馈页可见）**、**会话导出（Markdown 下载）**、DOM 引用统一集中于 `state.ts` 与代码高亮复用（`highlightedCode`））、`@gebai/desktop`（服务端同进程 + 浏览器兜底宿主）。

- 测试：`bun run --cwd packages/server test`（分片并行）全量用例通过（核心引擎、命名空间、沙箱、压缩、Webhook、ZIP、协议契约、服务模式鉴权、飞书机器人全覆盖）。
- 已实现补充：**原生 WebView 桌面宿主**（`packages/desktop/launcher/`：tao 窗口 + wry WebView，无 Tauri 依赖；`include_bytes!` 内嵌 `gebai.exe`，物化到 `%LOCALAPPDATA%\gebai\` 后 spawn，stdout 解析端口导航 WebView，关窗回收侧车）、**playwright driver 打包闭环**（`scripts/build-driver-embed.ts` 生成 `driver.embedded.generated.json` 内嵌产物，二进制运行时物化 `{GEBAI_HOME}/vendor/playwright/driver.mjs`）、**playwright-core 打包闭环与惰性桥接**（`scripts/build-pwcore-embed.ts` 整树 gzip base64 内嵌、运行时物化 `{GEBAI_HOME}/vendor/playwright-core/`；桥接经 `createLazyBridge()` 全进程惰性单例，bundle 图内子Agent 模块禁止模块作用域第三方包解析；Windows 默认 `channel=msedge` 驱动系统 Edge，`GEBAI_PLAYWRIGHT_CHANNEL` 覆写）、**数据生命周期 GC**（`core/gc.ts`：会话 90 天闲置归档 `trash/`、`trash/` 7 天物理删除、`feedback/` 180 天清理、遗留用户级 `truncated/` 迁移清理；启动即跑 + 每日周期，`GEBAI_GC_DISABLED=1` 关闭）、**子Agent 目录化**（`sub-agents/{name}/{name}.ts` + `{name}.md` 提示词拆分与 ts 导入修饰）、**子Agent 打包闭环**（`scripts/build-subagents.ts` 构建时生成 bundle 注册表，dist/二进制模式 `discover()` 回退加载，md 提示词随静态 import 内联进产物；playwright 的 node 桥接驱动 `driver.mjs` 复制到 dist/ 与产物同目录；**D2.js 打包闭环**——`scripts/build-d2js.ts` 生成内嵌产物 `d2js.embedded.generated.json`（node-esm 构建 7 文件 gzip base64，静态 import 随产物打进二进制，运行时物化到 `{GEBAI_HOME}/vendor/d2js/` 供文件路径 Worker 运行；构建命令 `--external @terrastruct/d2`））、**定时任务**（`core/cron.ts` + `sub-agents/cron/cron.ts`：`GEBAI_CRON_ENABLED` 开关（默认关闭，关闭时 `cron` 子Agent 不注册、`cron_*` 工具完全不注册）；`cron` 子Agent 命名空间暴露 `cron_add`/`cron_list`/`cron_update`/`cron_remove` 工具（创建/修改/删除默认需审批，自原全局工具下沉）；脚本运行 + 提示词运行 agent 两种类型；5 段 cron（本地时区，日周 OR 语义）/`@every n{s,m,h,d}`/`@daily` 等表达式；会话级 `cron.json` 持久化 + 重启扫描加载；30 秒 tick 调度；会话删除自动清理；`event.cron.run`/`event.cron.result` 事件）、**多用户安全加固**（REST 用户管理端点管理员校验（与 WS 同权限，防提权）、`GEBAI_APPROVAL_SKIP` 用户本人可设置自己的会话（会话内存态 env 不落盘，写入只影响本人会话，非管理员仍受沙箱约束；ask_env 模型驱动通道服务模式拒绝）、沙箱模式脚本子进程环境剔除敏感变量（`*_KEY`/`*_TOKEN`/`*_SECRET`/`PASSWORD`，防服务端密钥经 sh/py/cron 外泄）、`PATCH /api/v1/tools` 服务模式限管理员、**默认用户沙箱豁免**（**admin 用户为特权用户**，`Sandbox.isExempt`/`enforcedFor(user)` 按用户判定豁免——沙箱启用时仍按本地模式放开：绝对路径/越界放行、脚本环境不脱敏、桌面控制/私网访问可用；普通用户受约束）、`store.load` 按用户索引 + 会话归属记录（修复旧版无 userId 会话跨用户命中））、**HTML 小工具库**（`core/mini-tools.ts`：公用 `tools/` + 私有 `users/{user}/tools/` 按名称哈希分片存储，同名私有覆盖公用；widgets 子Agent 四工具 `widgets_save`/`widgets_list`/`widgets_get`/`widgets_delete`（后者需审批）；REST `GET/DELETE /api/v1/mini-tools` + SDK `listMiniTools`/`getMiniTool`/`deleteMiniTool`；Web 标题栏「小工具」按钮弹窗列表 + 独立悬浮窗口加载（沙箱 iframe、可拖拽/刷新/关闭，重复点击恢复原窗口不重载））、**前端页面捕获 `page_capture`**（仿 draw 前端配合链路：引擎发布 `event.capture.request` → 前端捕获当前页面渲染后 DOM html（截断 300KB）+ modern-screenshot 截图（png/jpeg，体积压缩 ≤2MB）→ WS `capture.result` 回传 → 落盘会话 `tmp/capture/` 并返回文件/图片块；`self_optimize` 重构为 **code 超集**（继承其全部工具（含并入 code 的 preview_server）+ page_capture/vision），vision provider 经 `setVisionProviderGetter` 注册点与主 Agent 共用解析逻辑；SDK `submitCaptureResult`；页面捕获超时 30 秒、整页截图高度上限 12000px）、**飞书机器人对话桥接**（`feishu-bot/`：自研极简 protobuf 帧编解码（pbbp2.Frame）+ 长连接协议层（endpoint 发现/心跳/分片合包/ACK/自动重连）+ 开放平台 API（tenant_access_token 缓存、消息发送/撤回、图片资源下载、会话信息）+ 桥接编排（会话映射 `feishu_{chat_id}`、身份映射（多用户按 open_id 自动建户）、文本/图片消息处理、@提及剥离、流式增量预览（节流合并发新撤旧）+ 最终卡片（lark_md）、斜杠命令、文本命令式审批；`GEBAI_FEISHU_BOT_ENABLED` 开关，依赖全部注入；单测 79 用例 + 真实凭证长连接握手验证通过）。
- 未完成（后续迭代）：子Agent 选择性打包（白名单/黑名单裁剪）、飞书机器人 Webhook 回调模式与交互卡片审批、OIDC 身份对接。

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
| 工具返回截断阈值 | ~12000 字符 | 超出后按行截头尾（保留完整行，单行巨长按字符兜底），完整内容写入会话 `tmp/truncated/{tool}_{hash}.txt`（会话根内逻辑路径，沙箱内可读），返回路径给模型；**引擎在主循环兜底**：工具未自行截断的超长输出统一走该逻辑（`TRUNCATE_THRESHOLD`）；阈值适配现代大上下文窗口，膨胀防护由上层压缩器兜底 |
| 截断保留首/尾 | 各 4000 字符 | 截断消息保留的 head/tail 长度（`TRUNCATE_HEAD_CHARS`/`TRUNCATE_TAIL_CHARS`） |
| 计划文档目录 | `tmp/plans/` | `plan` 工具计划落盘目录（会话 `tmp/` 内，随会话文件面板可见；文件名按标题清洗（仅字母/数字/下划线/连字符，空回退 `plan`，长标题截断 60 字符），同标题重提计划覆盖更新） |
| 用户输入落盘阈值 | 12000 字符 | 超长用户输入发送时全文落盘会话 `tmp/user_inputs/{hash}.txt`（原文不丢，read 可读），消息正文保留头尾各 4000 字符 + 文件引用（`USER_INPUT_SPILL_THRESHOLD`/`USER_INPUT_SPILL_HEAD`/`USER_INPUT_SPILL_TAIL`，见「上下文保护」） |
| grep 单文件读取上限 | 1MB | 超出跳过该文件（防大文件/二进制拖慢搜索） |
| grep 匹配子进程超时 | 20 秒 | 正则匹配在独立子进程执行（灾难性回溯防护），超时强杀并返回引导（`GREP_MATCHER_TIMEOUT_MS`）；行数据按 4MB 批量送子进程 |
| read/edit 文件大小上限 | 8MB / 5MB | 全量读入内存前 stat 预检（GB 级文件直接 OOM；edit 与 patch 5MB 同口径），超限引导 offset/limit 分段或 grep/patch 定位（`READ_MAX_FILE_BYTES`/`EDIT_MAX_FILE_BYTES`） |
| fetch_url 流式读取上限 | 10MB | 响应体流式限量读取（下载后截断防不住内存：超大/无限流在截断前已全量入内存，`FETCH_URL_STREAM_MAX_BYTES`） |
| js 子进程日志总量上限 | 2MB 字符 / 4MB 单行缓冲 | 父进程侧日志总量封顶（超时只杀子进程不回收已累积内容，`JS_LOG_TOTAL_CAP`）；无换行巨流的 stdoutBuf 硬上限（`JS_STDOUT_BUF_CAP`，行长检查仅在遇换行时生效） |
| 会话 env 内存缓存 | 256 会话 | `envCache` LRU 上限（命中刷新位置，防长生命周期进程无界增长） |
| 先到决策/选择/环境值排队上限 | 64 条/任务 | `pendingDecisions`/`pendingChoices`/`pendingEnvRequests` 容量（随机 id 无界堆积的内存放大防护，`PENDING_QUEUE_LIMIT`） |
| OAuth pending 授权有效期/上限 | 10 分钟 / 32 条 | 飞书 user_access_token 授权 state 的 TTL 与容量（未完成授权的条目含 appSecret 明文，按 TTL 惰性清理+超容量淘汰） |
| grep 最大匹配行 | 200 行 | 达到即停止，返回结果附「已达匹配上限」提示（content/files/count 三形态同一口径）；`context` 参数 0-10 行 |
| 已读登记上限 | 2000 条/会话 | write 防误覆盖守卫的会话级已读集合上限（`READ_TRACK_CAP`，超出整表重置——守卫降级为「需重读」）；会话删除时释放（`engine.forgetSession`） |
| patch 行号容错 | 3 行 | 上下文裁剪重试上限（`PATCH_FUZZ_LINES`，只裁上下文、删除行必在匹配块内） |
| patch hunk 上限 | 100 处 | 单次补丁 hunk 数上限（超出提示拆分补丁） |
| patch 文件上限 | 5MB | 目标文件大小上限（超出提示改用 edit 分段修改） |
| search_symbols 单文件上限 | 1MB | 超出跳过该文件（与 grep 同级） |
| search_symbols 扫描上限 | 500 个文件 | 最多扫描文件数（内容预筛后 tree-sitter 解析） |
| search_symbols 匹配上限 | 50 条 | 达到即停止，精确匹配优先排序 |
| git log 条数 | 10 条 / 50 条 | 默认条数 / 上限（`maxEntries` 参数可调） |
| URL 抓取响应上限 | 200KB | `fetch_url` 超出截断（含截断落盘），防内存膨胀 |
| URL 抓取超时 | 15 秒 | `fetch_url` 单次请求上限 |
| 重定向跳数上限 | 5 跳 | `fetch_url`/`http_request` 重定向逐跳校验的最大跟随次数（防重定向循环与跳板链） |
| 上下文压缩阈值 | 窗口的 80% | 达到后自动触发上下文压缩（见「上下文保护」） |
| 上下文占用口径 | 模型服务返回的 usage 真值 | 压缩判定只认接口返回的 `input_tokens`（含 system 提示词与工具 schema）：run 前按上次调用持久化基线、任务中途按每轮真实 usage（80% 阈值触发压缩）、接口上下文长度 4xx 拒绝时压缩重试（溢出恢复）；接口不返回 usage 时不估算预判（由真值/溢出恢复接管）。估算（`estimateCharsTokens`，CJK 约 1 token/字、ASCII 约 4 字符/token）仅用于会话列表 ctxTokens 展示的增量补足（见「上下文保护」） |
| 压缩保留最近比例 | 50% | 主动/自动压缩默认保留最近一半可压缩消息，保证最新上下文完整 |
| 摘要输入/输出上限 | 20000 / 2000 字符 | LLM 摘要请求的输入裁剪长度与输出上限 |
| 工具调用轮次上限 | 200 次/任务 | 单次任务内模型工具调用最大轮次，防止失控循环 |
| 重复检测窗口 | 最近 8 次调用 | 工具调用签名（工具名+参数 JSON）滚动窗口（`MAX_REPEAT_WINDOW`） |
| 重复检测命中阈值 | 3 次 | 窗口内相同签名第 3 次起判定为无效重复，中断该次执行并注入引导提示（`MAX_REPEAT_HITS`） |
| 重复中断上限 | 2 次 | 重复中断超过该值终止工具循环（`MAX_REPEAT_STALLS`），仍返回最后产出文本 |
| 消息重试上限 | 10 次 | 审批**超时自动拒绝**后提示模型调整的最大重试次数（显式拒绝即停止会话，不重试；`approval.request` 的 `retries` 字段为该工具调用累计拒绝次数） |
| 审批超时 | 5 分钟 | 审批请求等待上限，超时自动拒绝并提示模型调整 |
| 脚本执行超时 | 5 分钟 | `sh`/`py` 单次执行上限（超时杀进程并返回 `[timed out after ...]` 结果给模型，不结束任务）；`timeout` 参数可按次调整（秒，默认 300、上限 540，不晚于工具执行超时兜底） |
| sh 异步任务生命周期 | 默认 30 分钟 / 上限 60 分钟 | `sh async:true` 后台任务生命周期上限（status/wait/list/kill 时惰性检查，超限终止并标记 timed_out；`timeout` 参数在此语义下调整，`shTaskLifetimeMs`） |
| sh 异步任务并发上限 | 8 个/会话 | 同时运行的后台任务数上限（`SH_TASK_MAX_CONCURRENT`，超限拒绝新任务并引导清理） |
| sh 异步任务存储 | `tmp/sh-tasks/` | 记录 `tasks.json`（原子写）+ 每任务 `{id}.log` 合并输出（stdout+stderr）；任务 id 形如 `t` + 8 位 hex |
| sh_task 输出尾部 | 默认 4000 / 上限 20000 字符 | status/wait 返回的日志尾部字符数（`tail` 参数可调，完整日志 `tmp/sh-tasks/{id}.log`） |
| sh_task 等待默认/上限 | 60 / 540 秒 | `action=wait` 阻塞等待上限（轮询 300ms，不晚于引擎工具超时兜底） |
| 工具执行超时兜底 | 9 分钟 | 引擎层兜底（`TOOL_TIMEOUT_MS`，可注入）：覆盖不响应超时的工具（如网络请求挂起）；超时不结束任务，结果作为「执行超时」返回模型自行调整（子Agent 内挂起工具同样受此保护） |
| 长工具执行心跳 | 25 秒 | 工具执行期间周期发布 `event.tool.alive` 刷新前端空闲看门狗（`TOOL_HEARTBEAT_MS`，测试可注入 `heartbeatMs`；见「等待期不误判挂起」） |
| 子Agent 调用超时 | 不设 | 子Agent 调用不设整体超时（执行进度实时可见，中止仅由父任务取消传播；`SUBAGENT_TIMEOUT` 已移除） |
| 子Agent 递归深度 | 3 层 | 子Agent 嵌套调用最大深度 |
| 待办续做轮次上限 | 3 轮 | 会话完成时仍有 `pending`/`in_progress` 待办，追加提醒继续会话的轮次上限（`MAX_TODO_CONTINUE`，达到即停止本轮任务） |
| 预览服务就绪超时 | 15 秒 | `preview_server` 启动后 TCP 就绪探测总时限（超时即终止并回显日志尾部） |
| 预览服务轮询间隔 | 300ms | 就绪探测轮询间隔 |
| 预览服务日志/状态 | `os.tmpdir()/gebai-preview-{port}.log`、`gebai-preview.json` | 独立进程 stdout/stderr 日志与运行状态（port/pid/url），状态文件按 PID 存活清理 |
| 垃圾回收保留期 | 7 天 | `trash/` 中删除/过期数据保留时长（已实现：GC 每日执行） |
| 反馈数据保留期 | 180 天 | `feedback/` 反馈保留时长（已实现） |
| 会话闲置过期 | 90 天 | 无活跃会话归档到 `trash/` 的时间（按 `chat.json` mtime 判定，已实现） |
| GC 周期 | 24 小时 | 数据生命周期清理任务执行周期（启动时立即执行一次） |
| 消息缓存上限 | 300 条 | 会话消息持久化上限（超限截断丢最早的非保护消息，按 tool_call 配对原子丢弃；系统提示词/用户输入/存档消息原位保留，见「上下文保护」） |
| 桌面固定端口 | 47896 | 桌面形态默认监听端口（`DESKTOP_PORT`，见「端口固定」） |
| Session 缓存 LRU | 10 个 | 会话列表 LRU 驱逐上限 |
| 截断内容哈希 | SHA256 | 基于完整返回内容计算，用于去重和文件命名 |
| 图片压缩上限 | 1280px / 2MB | 多模态图片自动压缩重采样的尺寸与大小上限（Web 端 canvas 重采样，JPEG 质量 0.85） |
| 视觉工具图片上限 | 8MB | `vision` 工具单张图片大小上限（超出提示压缩后重试） |
| 页面捕获等待超时 | 30 秒 | `page_capture` 工具等待前端捕获回传的最长时间（超时返回「页面捕获失败」提示） |
| 页面捕获 html 上限 | 300KB | 前端捕获 DOM html 的传输/落盘截断长度（超出截取首部，完整结构可用 `read` 分文件读取） |
| 页面捕获截图上限 | 1600px / 2MB | 前端截图输出长边与体积上限（超限 JPEG 重编码/等比缩放降质；png/jpeg） |
| 整页截图高度上限 | 12000px | fullPage 截图最大高度（canvas 尺寸上限保护，超出截取顶部） |
| 附件图片内联上限 | 8MB | 主模型多模态内联附件图片的 base64 大小上限（超出降级为文本说明） |
| 历史图片内联窗口 | 最近 3 组 | 仅最近 3 组含图片的用户消息内联进上下文，更早的降级为路径说明（图片不参与压缩，长会话防图片占死窗口；`INLINE_IMAGE_RECENT`） |
| LLM 流式读空闲超时 | 120 秒 | SSE 建立后连续无 chunk 判定接口假死中止本次调用（`LLM_IDLE_TIMEOUT_MS`，测试可注入 `llmIdleTimeoutMs`） |
| 溢出护栏裁剪下限 | 500 字符 | 用户消息超过该长度才可被护栏裁剪为占位（短消息裁剪无收益；最新一条用户消息永不裁剪） |
| agent_run 子Agent 上限 | 5 个 | 单次新会话执行可预加载的子Agent 数量上限（去重后判定，`MAX_AGENTS_PER_RUN`） |
| WS 事件日志持久化 | `users/{user}/ws-journal.jsonl` | 日志尾部 JSONL 追加持久化（每 2000 条重写裁剪），重启后 seq 连续 |
| delta 合并窗口 | 50ms / 200KB 上限 | 同一消息的连续文本增量合并为单条日志事件（`DELTA_MERGE_MS`/`DELTA_MERGE_MAX_CHARS`） |
| WS 发送缓冲上限 | 16MB | 单连接发送缓冲超限判定慢客户端并断开（走自动重连 + seq 重放收敛，`WS_MAX_BUFFERED`） |
| 登录 IP 限流 | 60 突发/2每秒 + 10 突发/0.2每秒 | 登录/兑换端点全局桶与来源桶（`GEBAI_TRUST_PROXY=true` 按 X-Forwarded-For 区分来源） |
| 附件大小上限 | 20MB | 单附件上传大小上限（音频/视频/文档） |
| 定时任务 tick 周期 | 30 秒 | 调度器检查周期（`CRON_TICK_INTERVAL_MS`） |
| 定时脚本执行超时 | 5 分钟 | 脚本型定时任务单次执行上限（与 `sh`/`py` 同级） |
| 定时任务输出保留 | 4000 / 8000 字符 | 任务记录保留输出长度 / 写入会话消息的脚本输出上限 |
| 小工具 HTML 上限 | 200KB | `widgets_save` 单次保存的 HTML 源码大小上限（超限报错提示精简） |
| 小工具名称规则 | `[a-zA-Z0-9_\u4e00-\u9fff]{1,40}` | 工具名即文件名（分片键），不含 `.`/`/` 等路径分隔符 |
| render_html 预览尺寸上限 | 4000 × 2000 px | `width`/`height` 显式预览尺寸上限，超限忽略回退默认 |
| flow 工具调用总数上限 | 100 | 单次 flow 执行的工具调用总数（含循环迭代展开，`FLOW_MAX_STEPS`） |
| flow foreach 展开上限 | 50 项 | foreach 数组长度/计数上限，超限报错提示分批（`FLOW_FOREACH_MAX`） |
| flow while 轮数上限 | 默认 10 / 硬上限 50 | `maxLoops` 参数可调，超硬上限钳制（`FLOW_WHILE_DEFAULT_MAX`/`FLOW_WHILE_HARD_MAX`） |
| flow 分组嵌套深度上限 | 4 层 | 循环分组嵌套上限（`FLOW_MAX_DEPTH`） |
| flow 报告截断 | 单步 2000 / 单轮 500 字符 | flow 报告中步骤/循环轮次输出保留长度（完整内容在 data 与截断文件中，`FLOW_REPORT_STEP_CHARS`/`FLOW_REPORT_ROUND_CHARS`） |
| js 工具调用总数上限 | 100 | 单次 js 脚本内工具调用总数（`JS_TOOL_MAX_CALLS`，与 flow 对齐） |
| js RPC 字段截断 | 100k 字符 | js 工具调用桥单字段（output/data）截断（`JS_RPC_FIELD_CAP`） |
| js data 截断 | 100k 字符 | js 结构化 data 中 logs/result 截断（`JS_DATA_TEXT_CAP`，与 sh/py 对齐）；返回值预览 2000 字符（`JS_RESULT_PREVIEW_CHARS`） |
| js RPC 协议行上限 | 子进程 2MB / 服务端 2.5MB | 协议行截断兜底（防巨对象撑爆内存，超出注明截断） |
| js 会话上下文注入 | 最近 50 条 / 单条 2000 字符 | ctx.messages 快照条数与单条内容上限（`JS_CONTEXT_MESSAGES_MAX`/`JS_CONTEXT_MESSAGE_CHARS`） |
| js 动态工具上限 | 50 / 会话 | 会话内运行时定义工具数量上限（防注册风暴，`DYNAMIC_TOOLS_CAP`）；定义清单随会话 chat.json 落盘、重启恢复 |
| js 动态工具源码上限 | 100k 字符 | execute 源码长度上限（`JS_DYNAMIC_SOURCE_CAP`，源码随会话持久化，防撑爆 chat.json） |
| js 动态工具名 | `[a-z][a-z0-9_]{0,39}` | 运行时定义工具命名约束（与全局工具命名一致，`DYNAMIC_TOOL_NAME_RE`）；execute 源码 ≤ 2000 字符描述 |
| sh/py 结构化 data 文本上限 | 100k 字符 | `data.stdout`/`data.stderr` 超长截断（完整文本以 output 截断文件为准，`SCRIPT_DATA_TEXT_CAP`） |
| 后端图表渲染超时 | 20 秒 | 后端组合渲染器（`core/diagram-render.ts`）单次超时上限：plantuml 走 `plantuml.ts` `PLANTUML_TIMEOUT_MS`（可注入），mermaid 渲染与 d2 编译/渲染各 20 秒 Promise 超时；引擎渲染本身秒级，超时防大图/挂起 |
| 后端图表输出尺寸上限 | 1600 × 2400 px | 后端渲染默认 2x 超采样，超出按比例缩放到该上限（防超大 PNG 超飞书图片限制；`DEFAULT_MAX_WIDTH`/`DEFAULT_MAX_HEIGHT`） |
| 图表语言 | `mermaid` / `plantuml` / `d2` / `echarts` | draw 工具 `format` 参数四取值（SDK `DiagramFormat`，缺失/非法立即报错）；产物扩展名 `.mmd`/`.puml`/`.d2`/`.echarts`；前端本地渲染与后端组合渲染器（飞书/`render=backend`）均四语言全支持；echarts 源码为 option 的严格 JSON（双引号，容错注释/尾逗号） |
| ECharts 画布尺寸 | 默认 960×600，钳制 200-4000 px | echarts SSR 渲染必须显式宽高（无 DOM 测量）；信封 `width`/`height` 超范围钳制、非法值回退默认（前后端一致） |
| ECharts 源码形态 | 严格 JSON（键/字符串双引号），容错注释/尾逗号 | 不支持单引号/裸键名/`…`缩写（对象字面量不可用）；信封 `{"option":…,"width":…,"height":…}` 指定画布尺寸；解析失败错误信息点名常见病因（前后端一致） |
| draw format 校验 | 缺失/非法立即报错 | 不静默回退 plantuml（防漏传 format 时源码被错误语言渲染、报错误导模型）；`path` 模式扩展名可推断时免传 |
| draw echarts 通道加固 | 预校验 + 版本错位诊断 | 服务端 `parseEchartsInput` 预校验 JSON（无效立即报错不跑前端）；前端报错引擎与请求语言不符（旧版前端把 echarts 当 PlantUML 渲染）返回「前端渲染器版本过旧，请刷新页面」诊断，**不自动换通道**（前端渲染为默认正确通道） |
| D2 主题 ID | 0（亮）/ 200（暗） | 前端按 UI 明暗选择、后端固定浅色（0=Neutral Default、200=Dark Mauve，`diagram.ts`/`diagram-render.ts` 常量） |
| 前端本地渲染引擎 | mermaid（懒加载）/ @plantuml/core（懒加载）/ @terrastruct/d2（懒加载）/ echarts（懒加载） | 四语言零网络本地渲染；加载超时 30 秒（echarts 15 秒——约 1MB 体积小）、渲染/编译超时 20 秒（`diagram.ts`，引擎体积大慢机器加载可超 15 秒故放宽）；echarts SSR 模式（`init(null,…,{renderer:"svg",ssr:true})` + `renderToSVGString`）纯计算输出 SVG、无 DOM 挂载，缓存 key 含主题明暗（darkMode 注入）；**未知图表语言显式报错引导改用 `render=backend`（不静默回退 PlantUml——服务端新增语言而前端为旧版本时，回退会把源码当 PlantUML 渲染出误导性错误）**；**D2 前端单一串行队列**（浏览器构建单 Worker 共享 currentResolve，并发调用互相覆盖导致超时，`enqueueD2` 一次一个）；D2 编译错误 JSON 数组转可读文本（`formatD2Error` 提取 errmsg，前后端一致）；**动态分块加载失败自动整页刷新一次**（开发模式重建后旧页面引用旧 hash 分块 404，浏览器报 "Failed to fetch dynamically imported module"）；空闲预热 PlantUML + mermaid + echarts，D2（8MB WASM）不预热 |
| 后端渲染引擎 | plantuml（TeaVM + DOM shim）/ mermaid + happy-dom / d2（`@terrastruct/d2` WASM）/ echarts（npm 包 SSR） | 四语言后端渲染（`core/diagram-render.ts`，飞书与 `render=backend` 共用）；懒加载（echarts 顶层急切导入——zrender 环境探测须先于 DOM 垫层污染）；单一串行队列；`globalThis.window` 仅临时存在（Bun worker_threads 冲突规避）；d2 二进制模式从内嵌产物（`scripts/build-d2js.ts` 生成 JSON，gzip base64）物化 `{GEBAI_HOME}/vendor/d2js/{version}/` |
| show_file 复制上限 | 100MB | 会话 `tmp/` 外文件复制进会话文件区的尺寸上限（`SHOW_FILE_MAX_BYTES`），超出引导改为告知路径 |
| show_file 文本直显上限 | 读取 512KB / 内联 4 万字符 | 文本/代码内联 `code` 块的读取上限（`SHOW_FILE_TEXT_DIRECT_BYTES`，超出仅给查看/下载卡片）与截断阈值（`SHOW_FILE_TEXT_MAX_CHARS`，超出截断展示 + 附 file 卡片取全文） |
| D2.js 内嵌产物 | `src/core/d2js.embedded.generated.json`（gitignore） | 构建时 `scripts/build-d2js.ts` 生成；node-esm 构建 7 文件 gzip base64（22MB wasm → 9.9MB）；构建命令 `--external @terrastruct/d2` 保持运行时文件路径 Worker 可用 |
