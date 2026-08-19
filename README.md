# 歌白智能体（GEBAI Agent）

> 极致动态扩展能力的人工智能体：**一个 TS 文件就能定义一个子 Agent**，Agent 还能**修改自己的代码来改进自己**。支持桌面应用、本地浏览器、服务端部署三端合一，一个二进制文件走天下。

GEBAI 是围绕一条稳定的「**对话 → 工具调用 → 审批 → 执行**」主循环构建的 Agent 引擎。它不引入 memory / skill 等运行时注入的"不稳定能力"，一切能力的扩展与改进都沉淀为**可审查、可测试、可回滚的代码变更**。

---

## 为什么值得关注

### 🧩 极致的子 Agent 扩展制（单文件定义、零注册）

在 `packages/server/src/sub-agents/` 下放一个 `.ts` 文件（或一个目录 + `.md` 提示词），就完成了一个子 Agent 的定义——构建时自动扫描收集，**无需任何注册表、配置或代码登记**：

```ts
export const name = "my_agent"
export const description = "何时该装载本子 Agent 的一句话描述"
export const systemPrompt = "你是……（完整系统提示词）"
export const tools: ToolSet = { read, write, sh, ... }  // 复用或新建工具
export const requiresApproval = { write: true }
```

关键设计是「**装载 vs 新会话执行**」两种语义的精确区分：

| 能力 | 装载（`agent_load`，模块语义） | 新会话执行（`agent_run`，会话语义） |
|------|------|------|
| 类比 | `import` 子模块 | 派生临时新会话 |
| 上下文 | 并入主上下文，全程可见 | 独立上下文，完全隔离、只回传结果 |
| 效果 | 工具注册进当前工具集 + 完整提示词写入会话 | 预加载一个或多个子 Agent 后阻塞执行并完整存档 |
| 适用 | 默认方式：装完直接用其工具 | 需要干净上下文、防止上下文膨胀 |

工具以 `{agent}_{tool}` 单下划线命名空间透明路由，对子 Agent 完全无感；命名冲突构建期校验，弱模型容错兜底。

### 🔁 代码级自我优化（self_optimize）

Agent 通过**修改自身代码**来持续改进自己——子 Agent 定义、工具实现、系统提示词、默认配置，全是可 diff、可测试、可回滚的代码：

- 反馈读取 → 方案 → 审批 → 改代码  **测试准入**（`bun test` 是唯一准入凭证，失败自动 `rollback`）
- 写范围守卫**代码级强制**：默认只读模式只允许修改子 Agent 目录与仓库级文档，核心引擎源码默认只读
- UI 类修改用 `page_capture` 直接捕获真实渲染页面 + `vision` 视觉分析验证；服务端类修改用 `preview_server` 临时端口验证
- 设计变更自动回写 `DESIGN.md`（文档与代码保持一致）

### 💻 一套代码，三种形态，一个二进制

`bun build --compile` 产出单个可执行文件，**零运行时依赖**（Bun已内嵌）：

- **桌面应用**：tao/wry 原生 WebView 启动器（Windows WebView2 / macOS WKWebView / Linux WebKitGTK）
- **本地浏览器**：`gebai.exe` 开箱即用，自动打开浏览器
- **服务端部署**：`--server` 服务模式，多用户账号密码登录

Web UI、全部子 Agent、tree-sitter 语法、图表引擎、playwright 驱动全部内嵌；支持 `--sub-agents a,b` 按需裁剪二进制体积。

### 🛡️ 多用户安全隔离

- **用户 → 会话**级数据隔离，所有会话操作校验归属
- 路径沙箱：拒绝 `../`、绝对路径、符号链接（含 IPv4-mapped IPv6、整数/八进制 IP、尾点 FQDN 等绕过形式）
- 脚本子进程环境**剔除敏感变量**（`*_KEY`/`*_TOKEN`/`*_SECRET`…）防止密钥外泄
- SSRF 防护：`fetch_url`/`http_request`/Webhook 拒绝内网地址 + 重定向逐跳校验
- `GEBAI_SAFE_MODE`：风险能力**降级而非禁用**（sh 只读白名单、py/js 只读运行时审计钩子）
- 登录限流（口令锁定 + IP 令牌桶）、6 位加密令牌机制、scrypt 加盐哈希密码

### 🎨 顶级的多模态与富内容能力

- **三个 LLM 接口原生支持**（不依赖第三方 AI SDK，自行实现 SSE 流解析）：OpenAI 兼容 / OpenAI Responses / Anthropic，全部支持文本 + 图片/音视频
- **上下文保护**：以模型真实 `input_tokens` 为口径自动压缩（截断落盘 → 旧消息摘要 → 滚动裁剪 → 溢出硬护栏）
- **富内容块**：代码、图片、文件、并排 diff、沙箱 HTML 渲染，以及 **Mermaid / PlantUML / D2 三种图表的交互式创作*（本地渲染，零网络请求，配色跟随主题）
- **12 套 UI 主题**：亚克力 / 以太 / 经典 / 暗夜 / 现代 / 极简 / 矩阵 / 东京夜 / 赛博 / 浪潮 / 极光 / 人民币，运行时热切换

### ⚙️ 面向工程的能力

- **审批流**：命令行式人机协作——工具级审批、`plan` 计划审批、`ask_user` 选择、`ask_env` 填值，键盘 Y/N 快捷键
- **flow 数据流编排**：一次调用编排多步工具链（引用映射 / 条件分支 / foreach/while 循环），把多轮往返压成一次
- **js 动态编程**：脚本内把工具当函数直接调用（`await read(...)`），`defineTool` 可把能力固化为会话级新工具
- **待办跟踪**：Todo 拆解 → 执行 → 失败恢复续做
- **定时任务**：`cron_add` 创建无人值守的周期脚本/ Agent 任务
- **飞书机器人**：长连接接入（无需公网回调），群聊/单聊直接用 Agent，交互卡片审批与画图
- **业务集成**：官方 TS SDK、OpenAPI 文档、WS/REST 双通道、Webhook（HMAC 签名 + 重试）、外部身份兑换（同源登录态复用）、iframe 嵌入免登录
- **数据生命周期**：GC 自动清理、会话归档与恢复

---

## 快速开始

### 环境要求

- [Bun](https://bun.sh) ≥ 1.2

### 开发模式

```bash
# 1. 安装依赖
bun install

# 2. 配置模型（复制 .env.example 为 .env 并填入 LLM 配置）
cp .env.example .env

# 3. 启动（脚本调试模式，GEBAI_HOME 为项目根目录）
bun run dev
```

然后访问 `http://127.0.0.1:3000` 即可开始对话。Web 前端改动自动热刷新：`bun run dev --reload`。

### 构建与分发

```bash
bun run build                 # 全量构建（Web UI + 子 Agent 打包 + 桌面端）
bun run build:code            # 裁剪构建示例：code 场景精简单文件二进制
bun run --cwd packages/server build:win --sub-agents a,b   # 仅打包指定子 Agent
```

- 桌面端：`packages/desktop/dist/gebai-desktop.exe`（原生 WebView 启动器，内嵌服务端）
- 纯服务端：`packages/desktop/dist/gebai.exe`（单文件，`--server` 切服务模式）
- Docker：见 `docker/Dockerfile`（`docker/build.sh` 构建）

### 服务模式（多用户）

```bash
# 生成 admin 密码哈希并启动
bun run --cwd packages/server hash-password -- '你的密码'
GEBAI_MODE=server GEBAI_ADMIN_PASSWORD_HASH=salt:hash ./gebai.exe
```

**模型配置可以完全由前端完成**：服务端不配任何模型变量，用户在浏览器「设置 → 环境变量」面板填入密钥（仅存 localStorage，服务端零留存），随消息临时注入。

### 测试 / 检查

```bash
bun run test                  # 全量测试（718+ 用例）
bun run typecheck             # 类型检查
bun run lint                  # Lint
```

核心引擎（AgentEngine / ToolRegistry / EnvManager / Sandbox / 命名空间解析）行覆盖率 ≥ 90%。

---

## 内置子 Agent

| 子 Agent | 能力 | 说明 |
|----------|------|------|
| `code` | 源码分析与修改 | 探索→方案→修改→验证完整工作流，tree-sitter 语法分析、patch 补丁应用、符号定位、项目内置（`CODE_PROJECTS`） |
| `explore` | 只读代码探索 | 大范围摸底/架构梳理，只读工具集，返回结论 + `文件:行号` 清单，不占主上下文 |
| `self_optimize` | 优化 GEBAI 自身 | 反馈读取、测试准入、回滚、页面捕获、视觉分析、写范围守卫 |
| `desktop` | 桌面控制 | 截图、窗口控制、键盘鼠标、剪贴板（Windows/macOS/Linux） |
| `playwright` | 浏览器自动化 | 无头 Chromium，16 个工具，node 桥接，网络录制 |
| `reverse_site` | 网站/接口逆向 | 网络录制还原接口契约、直连探测、产出 API 文档、联动 self_optimize 生成新子 Agent |
| `feishu_docs` | 飞书云文档 | 文档/表格/多维表格/知识库/云空间/画板，支持用户授权（user_access_token） |
| `widgets` | HTML 小工具库 | 保存/复用调试好的 HTML 页面组件 |
| `cron` | 定时任务 | 无人值守的周期脚本与 Agent 任务（`GEBAI_CRON_ENABLED=true` 启用） |

## 架构一览

```
┌──────────────┐        ┌──────────────┐
│   API 层     │        │  AgentEngine │        ┌────────────┐
│ Hono 路由    │───────►│  核心主循环   │◄──────►│ ToolRegistry│
│ WS/REST/     │        │ 工具/审批/压缩 │        └────────────┘
│ Webhook/飞书 │        └──────┬───────┘
└──────────────┘   ┌──────────┼───────────┐
            ┌──────▼───┐ ┌────▼────┐ ┌────▼──────┐
            │SessionStore│  │EnvManager│  │ EventBus  │
            └──────────┘  └─────────┘  └───────────┘
```

Monorepo（Bun workspaces + Turborepo）：`@gebai/server`（服务端核心）/ `@gebai/sdk`（官方客户端 SDK）/ `@gebai/web`（Web UI）/ `@gebai/desktop`（桌面宿主）。核心模块全部接口化 + 依赖注入，可独立实例化测试。

---

## 设计文档

- **`DESIGN.md`**：权威设计文档——架构、模块接口、协议、安全模型、常量、子 Agent 规范等全部细节
- **`AGENTS.md`**：编码约定（子 Agent 新增指南、提交规范）

## 路线图

- [x] 核心主循环（对话 → 工具 → 审批 → 执行）
- [x] 单文件子 Agent 扩展 + 装载/新会话执行
- [x] 代码级自我优化（测试准入 + 自动回滚）
- [x] 多用户隔离 + 路径沙箱 + 外部身份集成
- [x] 单二进制三形态分发（桌面 / 浏览器 / 服务端）
- [x] 多套 UI、富内容、图表创作、飞书机器人、Webhook
- [ ] 前端消息虚拟化/分页（超长会话优化）
- [ ] 子 Agent 运行期热加载（dev 模式）

## 参与贡献

1. 阅读 `DESIGN.md` 与 `AGENTS.md`
2. 新增子 Agent？就是一个文件的事——`packages/server/src/sub-agents/` 下照模板写即可，记得同步 `DESIGN.md` 总览表
3. 提交前跑 `bun run test` / `bun run typecheck` / `bun run lint`
```
