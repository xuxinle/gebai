import { homedir } from "node:os"
import { dirname, join } from "node:path"
import { existsSync, readFileSync } from "node:fs"
import { combinePromptExtra, loadDomainProfile, readExtraFile, type LoadedDomainProfile } from "./profile"

export type AuthMode = "local" | "server"
export type SandboxMode = "auto" | "on" | "off"

/** `.env` 文件路径（二进制模式=`{GEBAI_HOME}/.env`，源码模式=仓库根 `.env`；与 loadDotEnv 同一口径）。 */
export function dotenvFilePath(): string {
  const base = isBinaryMode() ? resolveGebaiHome() : join(import.meta.dirname, "..", "..", "..", "..", "..")
  return join(base, ".env")
}

/** 本进程启动时 `.env` 里声明的变量名（loadDotEnv 读文件时登记，**与是否真的注入了无关**）。 */
const dotenvDeclared = new Set<string>()

/**
 * `.env` 文件里声明的变量名（重启拉起器据此把继承来的旧值剥掉）。
 *
 * 咬住的是一个真实踩过的坑：`loadDotEnv` 把 `.env` 的值写进 `process.env`（“真 env 优先”），
 * 拉起器 `{ ...process.env }` 原样继承给新进程，新进程看到 `process.env[K] !== undefined`
 * 就**跳过 `.env`** —— `.env` 改动从第一次重启起永久不生效（典型症状：改了
 * `GEBAI_ADMIN_PASSWORD_HASH`，重启后新口令登不进、旧口令仍能用；模型 Key 同理）。
 *
 * **为何按“文件声明了哪些键”而不是“本进程实际注入了哪些”**（这个区别是修好与否的分界线）：
 * 用“实际注入”会**永不自愈**——被污染的父进程把值继承下来后，新进程 `loadDotEnv` 一个都不注入，
 * 名单恒为空，它生成的拉起器依旧不剔除，脏环境代代相传。按文件声明则是**幂等且自愈**的：
 * 无论当前进程的环境有多脏，只要 `.env` 里写着这个键，就交给新进程重新读。
 *
 * **系统注入的真 env 不受影响**：剔除只发生在“重启拉起器向新进程传 env”这一步；新进程启动后
 * `loadDotEnv` 仍是“真 env 优先”，系统级注入的键会在剔除后重新由系统环境补上。
 */
export function dotenvDeclaredKeys(): string[] {
  return [...dotenvDeclared]
}

/** 读仓库根（或 GEBAI_HOME）`.env`，**只在真 env 未设置时**填值（真 env 优先）；同时登记文件声明的键。 */
function loadDotEnv() {
  // 测试进程不读仓库 .env（开发者本地配置会改变断言结果，见 scripts/test-preload.ts）
  if (process.env.NODE_ENV === "test") return
  const p = dotenvFilePath()
  try {
    const raw = readFileSync(p, "utf8")
    for (const line of raw.split("\n")) {
      const m = line.match(/^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$/)
      if (!m) continue
      // 文件声明就登记（区别于“是否注入”：见 dotenvDeclaredKeys 的注释）
      dotenvDeclared.add(m[1])
      if (process.env[m[1]] === undefined) process.env[m[1]] = m[2].replace(/^["']|["']$/g, "")
    }
  } catch {
    /* no .env; fine */
  }
}

export interface ServerConfig {
  host: string
  port: number
  /** 运行形态：`local`（本地模式，默认，直接使用 admin 用户免登录）/ `server`（服务模式，账号密码登录）。
   *  注意：本地模式设置了 `adminPasswordHash` 时**仍需登录**——运行形态语义不变（沙箱豁免、
   *  restart_server、admin 特权全部照旧），只是多了一道访问门。判定统一用 `loginRequired`。 */
  auth: AuthMode
  corsOrigins: string[]
  trustProxy: boolean
  sandbox: SandboxMode
  /** 脚本隔离模式（GEBAI_SCRIPT_ISOLATION，默认 auto）：**服务模式默认且强制开启会话目录隔离**
   *  （`off` 与服务模式互斥，启动即拒绝）；auto = 环境收敛（HOME/TEMP/XDG → 会话目录）
   *  + bubblewrap 可用时升为文件系统隔离；本地模式恒不收敛。 */
  scriptIsolation: "auto" | "off" | "env" | "bwrap"
  preloadSubAgents: string[]
  /** 子Agent 白名单（GEBAI_SUB_AGENTS_ENABLE，逗号分隔）：非空时仅保留名单内的子Agent（其余全部
   *  unregister——agent_list/agent_load/subsession_run/系统提示词注入均不可见）；与黑名单同时配置时先白后黑
   *  （黑名单最终生效）。 */
  subAgentsEnable?: string[]
  /** 子Agent 黑名单（GEBAI_SUB_AGENTS_DISABLE，逗号分隔）：名单内的子Agent unregister（运行时开关，
   *  与构建期选择性打包 GEBAI_BUILD_SUBAGENTS 互补——不改产物按部署裁剪能力面）。 */
  subAgentsDisable?: string[]
  /** 全局提示词**段落白名单**（`GEBAI_PROMPT_ENABLE`，逗号分隔的段落键）：非空时仅注入名单内段落
   *  （段落键与含义见 `core/engine/prompt.ts` 的 `PROMPT_SECTION_KEYS`）。 */
  promptEnable?: string[]
  /** 全局提示词**段落黑名单**（`GEBAI_PROMPT_DISABLE`，逗号分隔的段落键）：名单内段落不注入；
   *  与白名单同时配置**先白后黑**（黑名单最终生效）。 */
  promptDisable?: string[]
  /** 领域补充提示词（`GEBAI_PROMPT_EXTRA` 内联 / `GEBAI_PROMPT_EXTRA_FILE` 文件，或 `GEBAI_PROFILE` 的
   *  `prompt.extra`/`prompt.extra_file`）：追加在裁剪后的全局提示词末尾。 */
  promptExtra?: string
  /** 领域专用模式档案（`GEBAI_PROFILE`）：档案名（`{GEBAI_HOME}/profiles/{名}.json`）或档案文件路径。 */
  profile?: string
  uiStyle: string
  /** 浏览器页面标题（GEBAI_TITLE）：设置则注入前端（window.__GEBAI_TITLE__ + 替换入口 HTML 的 <title>，
   *  见 routes/static.ts）；未设置走前端内置默认「歌白」。运行时 updateTitle 同源读取，两者一致。 */
  title?: string
  logLevel: "debug" | "info" | "warn" | "error"
  toolEnable?: string[]
  toolDisable?: string[]
  selfModify: boolean
  gebaiHome: string
  binaryMode: boolean
  webDist: string
  /** 访问口令哈希（GEBAI_ADMIN_PASSWORD_HASH，格式 `salt:hash` 均为 hex，与注册表 scrypt 哈希一致）。
   *  两种形态下的含义：
   *  - 服务模式：启用 admin 用户（覆盖其注册表哈希），不设置则禁用 admin；
   *  - 本地模式：设为该实例的**访问口令**（只存进程内存，不落注册表）——本地模式本免登录，
   *    在公网/局域网暴露时必须给一道门；口令比对与限流复用服务模式同一套（scrypt + 恒时比较 + 失败锁定）。 */
  adminPasswordHash?: string
  /** 注册审批模式（GEBAI_SIGNUP_MODE）：open=注册即用（默认）/ approval=注册待 admin 审批（disabled+pending，批准后启用）。 */
  signupMode: "open" | "approval"
  /** 注册来源限制（GEBAI_SIGNUP_SOURCE）：any=默认（内置登录页与二开前端均可注册）/ custom=仅二开前端代码可注册
 * （注册请求须携带 X-GEBAI-Signup-Source: custom 头；内置登录页注册入口据探测端点自动隐藏，不隐藏则服务端 403 兜底）。 */
  signupSource: "any" | "custom"
  /** 关闭数据生命周期 GC 清理任务（默认开启）。 */
  gcDisabled: boolean
  /** 是否启用统一任务能力（GEBAI_TASKS_ENABLED，默认 true：注册 task 子Agent（task_* 工具）并启动
   *  任务调度器；显式 false 时完全不可见）。 */
  tasksEnabled: boolean
  /** 每用户同时运行的任务会话数（GEBAI_TASK_MAX_CONCURRENT，默认 5）：定时/普通任务并行上限，
   *  闲时任务另受「队列空闲 + 每用户仅 1 个」约束；运行中的任务不因额度不足被中断。 */
  taskMaxConcurrent: number
  /** 是否启用用户级待办（GEBAI_IDLE_TODO_ENABLED，默认 true：REST /api/v1/todos 可用；
   *  显式 false 时路由返回 503 且待办不参与闲时调度）。 */
  idleTodoEnabled: boolean
  /** 调度器门控模式（GEBAI_SCHEDULER，默认 auto）——同一 GEBAI_HOME 下的调度互斥：
   *  auto=按主实例锁判定（只一个实例跑定时任务与闲时待办，余者退化为从实例并挂看门狗等接管）；
   *  on=强制本实例跑调度（忽略锁，多实例同时 on 会重复调度）；off=完全不跑调度。 */
  scheduler: "auto" | "on" | "off"
  /** 任务全局默认通知 webhook（GEBAI_TASK_NOTIFY_WEBHOOK）：任务未配 notify 时自动追加该通道。 */
  taskNotifyWebhook?: string
  /** 任务全局默认飞书通知（GEBAI_TASK_NOTIFY_FEISHU）：群 chat_id（oc_ 前缀，应用身份推送）或群机器人
   *  webhook URL；任务未配 notify 时自动追加该通道。 */
  taskNotifyFeishu?: string
  /** 外部身份扩展点：HMAC 共享密钥（GEBAI_EXTERNAL_AUTH_SECRET，与 GEBAI_EXTERNAL_AUTH_URL 互斥）。 */
  externalAuthSecret?: string
  /** 外部身份扩展点：HTTP 回调验证 URL（GEBAI_EXTERNAL_AUTH_URL，与 GEBAI_EXTERNAL_AUTH_SECRET 互斥）。 */
  externalAuthUrl?: string
  /** 外部身份扩展点：外部用户名不存在时自动创建 GEBAI 用户（默认 true；false 时仅允许已存在用户）。 */
  externalAuthAutocreate: boolean
  /** 外部身份扩展点：Web UI 同源直读宿主 localStorage 的凭证 key（可选，不设则仅支持 URL 参数注入）。 */
  externalAuthStorageKey?: string
  /**
   * 凭证来源链（GEBAI_CREDENTIAL_SOURCES）：逗号分隔，如 `bearer,basic,cookie:gebai.auth.token`。
   * 缺省（未设置）= bearer → basic → cookie `gebai.auth.token`；设置为空值即清空链路、并按代码级
   * 来源（custom/auth/）重建。名称语法 bearer / basic / cookie:<名> / header:<名>，见 credential-sources.ts。
   */
  credentialSources?: string
  /** 开发模式热刷新（bun run dev --reload 或 GEBAI_DEV_RELOAD=1）：Web 源码变更自动重建并广播页面刷新。 */
  devReload: boolean
  /** 飞书机器人对话桥接（GEBAI_FEISHU_BOT_ENABLED=true 时启用长连接事件订阅）。 */
  feishuBotEnabled: boolean
  /**
   * 拟人运行时是否装配（建组件 / 开 REST 面），**不**等于“有人格在干活”。
   *
   * 判定依据（任一非空）：
   * - `GEBAI_PERSONA_ENABLE`（人格启用名单）
   * - 任意 `GEBAI_PERSONA_<ID>`（某个人格的分身配置）
   *
   * 而“哪几个人格真的干活”只看 `GEBAI_PERSONA_ENABLE`：名单为空 = 一个都不启用。
   * 为何必须显式：人格的订阅面与定时工作面写在**代码定义**里，所以**不配任何
   * `GEBAI_PERSONA_<ID>` 也能跑**——只要它在启用名单里，就会收全部 `message` 事件、
   * 按 cron 自己跑。若“发现即启用”，放个定义文件进目录（或升级代码带进来一个内置人格）
   * 就等于授权它刷群。
   */
  personaEnabled: boolean
  /** 飞书机器人行为配置（GEBAI_FEISHU_BOT_*）：工具调用过程推送（滚动状态消息）。 */
  feishuBotNotifyTools: boolean
  /** 助手中间轮文本推送（预览消息；引擎 notifyIntermediate 事件）。 */
  feishuBotNotifyAssistant: boolean
  /** 自动审批（需审批工具自动通过——部署方为飞书通道整体担责；审批卡片不再弹出）。 */
  feishuBotAutoApprove: boolean
  /** 飞书应用 App ID（GEBAI_FEISHU_APP_ID；feishu_docs 子 Agent 与机器人桥接共用）。 */
  feishuAppId?: string
  /** 飞书应用 App Secret（GEBAI_FEISHU_APP_SECRET）。 */
  feishuAppSecret?: string
  /** 安全模式（GEBAI_SAFE_MODE=true 启动时加载，不可在会话/任务级修改）：有风险的工具（命令执行/写删文件等）
   *  被阻止执行，直接返回限制信息给模型。 */
  safeMode: boolean
  /** 文件工作台总开关（GEBAI_FS_ENABLED，默认 true）：关闭时 `/files` 页面 404 且 fs/git 端点全部 404。 */
  fsEnabled: boolean
  /** 文件工作台写开关（GEBAI_FS_WRITE，默认 true）：false 时工作台为纯只读检视（含新建/改名/删除/上传/保存）。 */
  fsWrite: boolean
  /** 额外白名单根（GEBAI_FS_ROOTS，JSON 数组：字符串或 {name,path,description,writable}）：服务模式授予
   *  指定目录访问；本地模式在其上追加盘符/主目录等。 */
  fsRoots?: string
  /** 单次读取上限（GEBAI_FS_MAX_READ 字节，默认 10MB）。 */
  fsMaxRead: number
  /** 单次写入上限（GEBAI_FS_MAX_WRITE 字节，默认 10MB）。 */
  fsMaxWrite: number
  /** 上传单文件上限（GEBAI_FS_MAX_UPLOAD 字节，默认 100MB）。 */
  fsMaxUpload: number
  /** 打包下载上限（GEBAI_FS_MAX_ZIP 字节，默认 500MB，超出引导分批）。 */
  fsMaxZip: number
  /** 默认显示隐藏文件（GEBAI_FS_HIDDEN，默认 true；前端在「更多」菜单里可随时切换）。 */
  fsHidden: boolean
  /** 工作台变更监听（GEBAI_FS_WATCH，默认 true）：`/api/v1/fs/watch` 的 fs.watch 推送；
   *  关掉后前端退化为纯轮询（每轮都真的比对一次，成本高些，但无 inotify/fd 占用）。 */
  fsWatch: boolean
  /** 写操作审计（GEBAI_FS_AUDIT，默认 true → `{GEBAI_HOME}/audit-fs.jsonl`）。 */
  fsAudit: boolean
  /** Git 写操作开关（GEBAI_GIT_WRITE，默认 true）。 */
  gitWrite: boolean
  /** Git 远程操作开关（GEBAI_GIT_REMOTE，默认 true）：fetch/pull/push 需要网络与凭据。 */
  gitRemote: boolean
  /** 单次 diff 文本总量上限（GEBAI_FS_MAX_DIFF 字节，默认 8MB）：对比/提交详情超限时
   *  在**文件边界**截断并标 truncated，文件清单仍靠 `--name-status` 补齐。 */
  fsMaxDiff: number
  /** 单个文本文件（内容与逐行差异）上限（GEBAI_GIT_MAX_FILE 字符，默认 1M）：
   *  超限时内容侧返回 tooLarge（不拉正文）、差异侧只给统计，避免巨型 Monaco model 锁死主线程。 */
  gitMaxFile: number
  /** 终端面板总开关（GEBAI_TERMINAL，默认 true）：false 时终端端点 404（info 回 enabled:false + 原因）。 */
  terminalEnabled: boolean
  /** 终端默认 Shell（GEBAI_TERMINAL_SHELL：id 或可执行路径，如 cmd / bash / /bin/zsh）；空 = 按平台自动探测。 */
  terminalShell?: string
  /** 语言服务器总开关（GEBAI_LSP，默认 true）：false 时工作台不提供 LSP 补全/悬停/跳转/诊断，
   *  其余能力（Monaco 内置语言服务、符号提取）照旧。 */
  lspEnabled: boolean
  /** 语言服务器覆盖表（GEBAI_LSP_SERVERS，JSON：语言 id → 命令 / {command,args} / 候选数组 / null 关闭）。 */
  lspServers?: string
  /** LSP 空闲回收毫秒（GEBAI_LSP_IDLE_MS，默认 600000）：无打开文档的服务器进程超时即退出。 */
  lspIdleMs: number
  /** LSP 并发语言服务器进程上限（GEBAI_LSP_MAX，默认 4）。 */
  lspMaxSessions: number
}

function env(name: string, fallback = ""): string {
  return process.env[name] ?? fallback
}

function bool(name: string, fallback = false): boolean {
  const v = process.env[name]
  if (v === undefined) return fallback
  return v === "true" || v === "1"
}

/**
 * 拟人运行时是否装配（**决定要不要建这一套组件**，不决定谁在干活）。
 *
 * 判据（任一非空）：`GEBAI_PERSONA_ENABLE`（启用名单）或任意 `GEBAI_PERSONA_<ID>`（分身配置）。
 *
 * 两个问题必须分开：“要不要装出这套运行时” vs “哪几个人格真的干活”。
 * 后者由 `GEBAI_PERSONA_ENABLE` **显式**决定（`applyPersonaEnv` 里空名单 = 一个都不启用）：
 * 人格的订阅面与定时工作面写在**代码定义**里，所以**零个 `GEBAI_PERSONA_<ID>` 也能跑**——
 * 在名单里就会收全部 `message` 事件、按 cron 自己跑。“有定义”绝不能等于“它在干活”。
 *
 * 为何不把装配本身也压成“必须写 ENABLE”：装配好了才能在面板上把
 * “有人格定义、但一个都没启用”这件事显示出来（否则用户只看到 404 与“未启用”，
 * 分不清“没定义”“没启用”“装配坏了”三种情形）。
 *
 * 为何要把“公共变量”排除干净：`GEBAI_PERSONA_APPROVAL_ASK_TOOLS`、`GEBAI_PERSONA_YIELD_SECONDS`
 * 这类是**全局兑底的行为参数**，不是“我要跑人格”。若把它们也算作配置，部署方改一个审批名单
 * 就会凭空启用整个人格运行时——包括开始刷群。所以公共项单独列一份并显式排除；
 * 新增公共项忘了加进这个表时，后果是“改参数就开了人格”（严重），所以该表要跟着新增项一起改。
 */
export function hasPersonaConfig(env: Record<string, string | undefined>): boolean {
  if ((env.GEBAI_PERSONA_ENABLE ?? "").trim()) return true
  /** 公共行为参数（不是“某个人格”的配置）。 */
  const SHARED = new Set([
    "GEBAI_PERSONA_ENABLE",
    "GEBAI_PERSONA_APPROVAL_ASK_TOOLS",
    "GEBAI_PERSONA_REPLY_TO_OWNER",
    "GEBAI_PERSONA_HUMAN_INTERRUPT",
    "GEBAI_PERSONA_YIELD_SECONDS",
    "GEBAI_PERSONA_SESSION_ROUTING",
  ])
  for (const k of Object.keys(env)) {
    if (!k.startsWith("GEBAI_PERSONA_")) continue
    if (SHARED.has(k)) continue
    if ((env[k] ?? "").trim()) return true
  }
  return false
}

/** 正数环境变量（非法/非正回退默认值）。 */
function num(name: string, fallback: number): number {
  const v = process.env[name]
  if (v === undefined || v.trim() === "") return fallback
  const n = Number(v)
  return Number.isFinite(n) && n > 0 ? n : fallback
}

export function isBinaryMode(): boolean {
  return !existsSync(join(import.meta.dirname, "..", "..", "..", "package.json"))
}

export function resolveGebaiHome(): string {
  if (process.env.GEBAI_HOME) return process.env.GEBAI_HOME
  if (isBinaryMode()) return join(homedir(), ".gebai")
  return join(import.meta.dirname, "..", "..", "..", "..", "..")
}

function splitList(v: string | undefined): string[] {
  if (!v) return []
  return v
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean)
}

/**
 * 领域补充提示词解析：显式环境变量（`GEBAI_PROMPT_EXTRA_FILE` + `GEBAI_PROMPT_EXTRA`）优先于档案；
 * 同一来源内文件内容在前、内联文本在后。环境变量的相对路径相对进程 cwd，档案的相对路径相对档案目录。
 */
function resolvePromptExtra(loaded: LoadedDomainProfile | undefined): string | undefined {
  const envFile = env("GEBAI_PROMPT_EXTRA_FILE").trim()
  const envInline = env("GEBAI_PROMPT_EXTRA").trim()
  if (envFile || envInline) {
    return combinePromptExtra(envFile ? readExtraFile(envFile, process.cwd()) : undefined, envInline)
  }
  const p = loaded?.profile.prompt
  if (!loaded || !p || (!p.extra_file && !p.extra)) return undefined
  return combinePromptExtra(p.extra_file ? readExtraFile(p.extra_file, dirname(loaded.path)) : undefined, p.extra)
}

/**
 * 启动裁剪配置解析（DESIGN「启动裁剪与领域专用模式」）：`GEBAI_PROFILE` 档案为**默认值**，
 * 显式环境变量按字段覆盖（清单为空 = 未配置，用档案值）；四组清单口径一致——白名单非空仅保留名单内，
 * 黑名单移除名单内（先白后黑）。档案/提示词文件缺失或非法在启动期抛错（本函数即启动配置入口）。
 */
function resolveStartupTrim(gebaiHome: string): {
  profile?: string
  prompt: { enable: string[]; disable: string[]; extra?: string }
  tools: { enable: string[]; disable: string[] }
  subAgents: { enable: string[]; disable: string[]; preload: string[] }
} {
  const profile = env("GEBAI_PROFILE").trim() || undefined
  const loaded = profile ? loadDomainProfile(profile, gebaiHome) : undefined
  const def = loaded?.profile
  const pick = (fromEnv: string[], fromProfile?: string[]): string[] => (fromEnv.length ? fromEnv : (fromProfile ?? []))
  return {
    profile,
    prompt: {
      enable: pick(splitList(env("GEBAI_PROMPT_ENABLE")), def?.prompt?.enable),
      disable: pick(splitList(env("GEBAI_PROMPT_DISABLE")), def?.prompt?.disable),
      extra: resolvePromptExtra(loaded),
    },
    tools: {
      enable: pick(splitList(env("GEBAI_TOOL_ENABLE")), def?.tools?.enable),
      disable: pick(splitList(env("GEBAI_TOOL_DISABLE")), def?.tools?.disable),
    },
    subAgents: {
      enable: pick(splitList(env("GEBAI_SUB_AGENTS_ENABLE")), def?.sub_agents?.enable),
      disable: pick(splitList(env("GEBAI_SUB_AGENTS_DISABLE")), def?.sub_agents?.disable),
      preload: pick(splitList(env("GEBAI_PRELOAD_SUB_AGENTS")), def?.sub_agents?.preload),
    },
  }
}

/**
 * 运行形态解析（默认本地模式）：
 * 1. CLI 参数 `--server` 开启服务模式（最高优先，便于二进制直接传参）
 * 2. `GEBAI_MODE=server|local` 环境变量
 * 3. 兼容旧 `GEBAI_AUTH`：`none` → local、`multi` → server
 */
export function resolveAuthMode(): AuthMode {
  if (process.argv.includes("--server")) return "server"
  const mode = process.env.GEBAI_MODE || process.env.GEBAI_AUTH || "local"
  if (mode === "server" || mode === "multi") return "server"
  return "local"
}

/**
 * 启动期配置互斥校验（返回错误文案；合规返回 null）——「服务模式下隔离不可关闭」的**代码级**保证，
 * 与 `Sandbox` 的生效判定同一口径（服务模式 = 多用户/多会话边界成立的前提）：
 * - `GEBAI_SANDBOX=off`：路径沙箱关闭后任意用户可越界读写宿主任意路径（files 接口回退 resolve）；
 * - `GEBAI_SCRIPT_ISOLATION=off`：脚本的 HOME/TEMP/XDG 落回宿主共享位置（跨用户、跨会话相互可见可覆盖），
 *   且不再有 bubblewrap 文件系统隔离；需降级强度用 `env`（环境收敛仍生效）。
 * 两类都是安全配置错误，在启动期暴露而非运行期出事后追溯。
 */
export function isolationConfigConflict(config: Pick<ServerConfig, "auth" | "sandbox" | "scriptIsolation">): string | null {
  if (config.auth !== "server") return null
  if (config.sandbox === "off") {
    return "GEBAI_SANDBOX=off 与服务模式（GEBAI_MODE=server）互斥：多用户隔离要求路径沙箱，请使用 auto/on"
  }
  if (config.scriptIsolation === "off") {
    return "GEBAI_SCRIPT_ISOLATION=off 与服务模式（GEBAI_MODE=server）互斥：服务模式默认且强制开启会话目录隔离，需降级请用 env（仅环境收敛）/ auto"
  }
  return null
}

/**
 * 是否需要登录（唯一的鉴权闸门判据）：服务模式恒需登录；本地模式仅在设置了访问口令时需要。
 *
 * 为何单独成函数而不复用 `auth === "server"`：本地模式的「要不要登录」与「运行形态语义」是两件事——
 * 设了口令的本地实例仍是本地模式（admin 豁免路径沙箱、注入 restart_server、数据目录不变），
 * 只是把匿名可达变成需凭证可达。所有鉴权分支（REST 401、WS 未登录拒绝、快照推送时机、
 * 前端登录页）都读这个函数，避免「REST 拦住了、WS 还能直连」的口径错位。
 */
export function loginRequiredOf(config: Pick<ServerConfig, "auth" | "adminPasswordHash">): boolean {
  return config.auth === "server" || !!config.adminPasswordHash
}

export function loadConfig(overrides: Partial<ServerConfig> = {}): ServerConfig {
  loadDotEnv()
  const defaultWebDist = join(import.meta.dirname, "..", "..", "..", "..", "web", "dist")
  const gebaiHome = resolveGebaiHome()
  const trim = resolveStartupTrim(gebaiHome)
  const config: ServerConfig = {
    host: env("GEBAI_HOST", "127.0.0.1"),
    port: Number(env("GEBAI_PORT", "3000")),
    auth: resolveAuthMode(),
    corsOrigins: splitList(env("GEBAI_CORS_ORIGINS", "*")),
    trustProxy: bool("GEBAI_TRUST_PROXY", false),
    sandbox: env("GEBAI_SANDBOX", "auto") as SandboxMode,
    // 服务模式默认且强制开启会话目录隔离：off 与服务模式互斥（启动即拒，见 boot/compose.ts 的同款防呆）
    scriptIsolation: (() => {
      const v = env("GEBAI_SCRIPT_ISOLATION", "auto").trim().toLowerCase()
      return v === "off" || v === "env" || v === "bwrap" ? (v as "off" | "env" | "bwrap") : "auto"
    })(),
    preloadSubAgents: trim.subAgents.preload,
    subAgentsEnable: trim.subAgents.enable,
    subAgentsDisable: trim.subAgents.disable,
    uiStyle: env("GEBAI_UI_STYLE", "acrylic"),
    title: env("GEBAI_TITLE").trim() || undefined,
    logLevel: env("GEBAI_LOG_LEVEL", "info") as ServerConfig["logLevel"],
    toolEnable: trim.tools.enable,
    toolDisable: trim.tools.disable,
    promptEnable: trim.prompt.enable,
    promptDisable: trim.prompt.disable,
    promptExtra: trim.prompt.extra,
    profile: trim.profile,
    selfModify: bool("GEBAI_SELF_MODIFY", false),
    gebaiHome,
    binaryMode: isBinaryMode(),
    webDist: defaultWebDist,
    adminPasswordHash: env("GEBAI_ADMIN_PASSWORD_HASH") || undefined,
    signupMode: env("GEBAI_SIGNUP_MODE") === "approval" ? "approval" : "open",
    signupSource: env("GEBAI_SIGNUP_SOURCE") === "custom" ? "custom" : "any",
    gcDisabled: bool("GEBAI_GC_DISABLED", false),
    tasksEnabled: bool("GEBAI_TASKS_ENABLED", true),
    taskMaxConcurrent: (() => {
      const n = Number(env("GEBAI_TASK_MAX_CONCURRENT").trim())
      return Number.isFinite(n) && n >= 1 ? Math.floor(n) : 5
    })(),
    idleTodoEnabled: bool("GEBAI_IDLE_TODO_ENABLED", true),
    // 调度器门控模式（GEBAI_SCHEDULER，默认 auto）：auto=按主实例锁判定（同库单主实例调度）；
    // on/off 为显式部署决策——解析规则与 core/schedule/primary.ts 的 resolveSchedulerMode 一致
    scheduler: (() => {
      const v = env("GEBAI_SCHEDULER").trim().toLowerCase()
      return v === "on" || v === "off" ? v : "auto"
    })(),
    taskNotifyWebhook: env("GEBAI_TASK_NOTIFY_WEBHOOK").trim() || undefined,
    taskNotifyFeishu: env("GEBAI_TASK_NOTIFY_FEISHU").trim() || undefined,
    externalAuthSecret: env("GEBAI_EXTERNAL_AUTH_SECRET") || undefined,
    externalAuthUrl: env("GEBAI_EXTERNAL_AUTH_URL") || undefined,
    externalAuthAutocreate: bool("GEBAI_EXTERNAL_AUTH_AUTOCREATE", true),
    externalAuthStorageKey: env("GEBAI_EXTERNAL_AUTH_STORAGE_KEY") || undefined,
    credentialSources: env("GEBAI_CREDENTIAL_SOURCES").trim() || undefined,
    // 开发模式热刷新：bun run dev --reload 或 GEBAI_DEV_RELOAD=1
    devReload: process.argv.includes("--reload") || env("GEBAI_DEV_RELOAD") === "1",
    // 飞书机器人对话桥接（长连接模式）：需同时配置 GEBAI_FEISHU_APP_ID / GEBAI_FEISHU_APP_SECRET
    feishuBotEnabled: bool("GEBAI_FEISHU_BOT_ENABLED", false),
    // 拟人模式（事件驱动：渠道收到的事件 → 人格分身跑一轮）：**有配置就启用**。
    // 判定：GEBAI_PERSONA_ENABLE 非空，或存在任意 GEBAI_PERSONA_<ID>（人格 id 的配置）。
    // 注意排除统一配置项：只能看「除已知公共变量以外还有没有 GEBAI_PERSONA_*」。
    personaEnabled: hasPersonaConfig(process.env as Record<string, string | undefined>),
    // 飞书机器人行为配置（均默认 false 保持现状）：过程推送两类 + 自动审批
    feishuBotNotifyTools: bool("GEBAI_FEISHU_BOT_NOTIFY_TOOLS", false),
    feishuBotNotifyAssistant: bool("GEBAI_FEISHU_BOT_NOTIFY_ASSISTANT", false),
    feishuBotAutoApprove: bool("GEBAI_FEISHU_BOT_AUTO_APPROVE", false),
    feishuAppId: env("GEBAI_FEISHU_APP_ID") || undefined,
    feishuAppSecret: env("GEBAI_FEISHU_APP_SECRET") || undefined,
    // 安全模式：仅启动时从环境变量加载，不进入会话 env（不可被 ask 填值分支/前端 envOverride 修改）
    safeMode: bool("GEBAI_SAFE_MODE", false),
    // 文件工作台（DESIGN「文件工作台」）：默认开启且可写；服务模式可置 GEBAI_FS_WRITE=false 做纯只读检视环境
    fsEnabled: bool("GEBAI_FS_ENABLED", true),
    fsWrite: bool("GEBAI_FS_WRITE", true),
    fsRoots: env("GEBAI_FS_ROOTS") || undefined,
    fsMaxRead: num("GEBAI_FS_MAX_READ", 10 * 1024 * 1024),
    fsMaxWrite: num("GEBAI_FS_MAX_WRITE", 10 * 1024 * 1024),
    fsMaxUpload: num("GEBAI_FS_MAX_UPLOAD", 100 * 1024 * 1024),
    fsMaxZip: num("GEBAI_FS_MAX_ZIP", 500 * 1024 * 1024),
    fsHidden: bool("GEBAI_FS_HIDDEN", true),
    fsWatch: bool("GEBAI_FS_WATCH", true),
    fsAudit: bool("GEBAI_FS_AUDIT", true),
    gitWrite: bool("GEBAI_GIT_WRITE", true),
    gitRemote: bool("GEBAI_GIT_REMOTE", true),
    // diff/文件体量上限：把「撑不住」变成看得见的事（truncated/tooLarge + UI 提示）
    fsMaxDiff: num("GEBAI_FS_MAX_DIFF", 8 * 1024 * 1024),
    gitMaxFile: num("GEBAI_GIT_MAX_FILE", 1_000_000),
    // 终端（DESIGN「文件工作台·终端」）：持久 shell 会话；shell 留空按平台探测
    // （Windows 依次 pwsh.exe → powershell.exe → cmd.exe，POSIX /bin/bash、/bin/sh、$SHELL）
    terminalEnabled: bool("GEBAI_TERMINAL", true),
    terminalShell: env("GEBAI_TERMINAL_SHELL") || undefined,
    // 语言服务器（DESIGN「文件工作台·语言服务器」）：有则用、没有不影响——探测 PATH 上的
    // gopls / rust-analyzer / clangd 等常驻进程；覆盖表见 core/lsp/registry.ts 的格式说明
    lspEnabled: bool("GEBAI_LSP", true),
    lspServers: env("GEBAI_LSP_SERVERS") || undefined,
    lspIdleMs: num("GEBAI_LSP_IDLE_MS", 10 * 60 * 1000),
    lspMaxSessions: num("GEBAI_LSP_MAX", 4),
  }
  return { ...config, ...overrides }
}
