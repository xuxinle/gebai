/**
 * 渠道发现与注册表：**与子 Agent / 事件源同款「丢文件即注册」**。
 *
 * 四套机制逐条对齐（规则的**一致性本身**是可用性——仓库里已有多套「丢文件即注册」，
 * 再来一套不同规则的会让维护者反复踩「为什么这个不生效」）：
 *
 * | 机制 | 行为 |
 * |---|---|
 * | 双域扫描 | 内置 `packages/channels/src/channels/` + 二开 `custom/channels/`，同名**二开胜出** |
 * | 热加载 | 目录签名（`路径:mtime`）变化即重扫重启，无需重启服务 |
 * | 失败隔离 | 单渠道 import/启动抛错只记 `loadErrors`，其它渠道照常运行 |
 * | 严格校验 | 未知字段 / 类型不符 / 命名非法 / verb 无 schema → 拒绝并报错（不静默降级） |
 */

import { readdir, readFile, stat } from "node:fs/promises"
import { join } from "node:path"
import type { ChannelContext, ChannelDef, ChannelLoadError } from "./channel"

/** 扫描域（内置 + 二开）。 */
export interface ChannelScanDirs {
  builtin: string
  custom: string
}

/**
 * 默认扫描域：与子 Agent 的物理分域同构。
 *
 * 二开域是 `custom/channels/`（**代码域，随代码迁移**；二进制形态下不存在则签名 null → 跳过）。
 */
export function defaultChannelScanDirs(opts: { packageRoot: string; repoRoot?: string }): ChannelScanDirs {
  const repoRoot = opts.repoRoot ?? join(opts.packageRoot, "..", "..")
  return {
    builtin: join(opts.packageRoot, "src", "channels"),
    custom: join(repoRoot, "custom", "channels"),
  }
}

/** 是否为定义文件（排除测试与类型声明；`_` 前缀 = 内部辅助，不参与注册）。 */
function isDefFile(name: string): boolean {
  if (!/\.(ts|js)$/.test(name)) return false
  if (name.endsWith(".test.ts") || name.endsWith(".test.js")) return false
  if (name.endsWith(".d.ts")) return false
  if (name.startsWith("_")) return false
  return true
}

/**
 * 目录签名：递归收集定义文件的 `相对路径:mtimeMs` 排序拼接。
 *
 * **目录不存在返回 `null`**（区分「域未使用」与「域为空」：前者不算错误，后者是配了空目录）；
 * 存在但无定义文件返回空串。签名含 mtime 是热加载的唯一判据——不含 mtime 会导致
 * 「改文件内容不触发热加载」。
 */
export async function channelDirSignature(dir: string): Promise<string | null> {
  let entries
  try {
    entries = await readdir(dir, { withFileTypes: true })
  } catch {
    return null
  }
  const parts: string[] = []
  for (const e of entries) {
    if (e.isDirectory()) {
      const base = e.name
      if (!/^[a-z0-9_]+$/.test(base)) continue
      for (const candidate of [`${base}.ts`, "index.ts"]) {
        const p = join(dir, base, candidate)
        const st = await stat(p).catch(() => null)
        if (st?.isFile()) parts.push(`${base}/${candidate}:${st.mtimeMs}`)
      }
    } else if (e.isFile() && isDefFile(e.name)) {
      const base = e.name.replace(/\.(ts|js)$/, "")
      if (!/^[a-z0-9_]+$/.test(base)) continue
      const st = await stat(join(dir, e.name)).catch(() => null)
      if (st) parts.push(`${e.name}:${st.mtimeMs}`)
    }
  }
  parts.sort()
  return parts.join("|")
}

/** 双域签名（热加载判定单一来源，discover 与 refreshIfChanged 同源防漂移）。 */
export async function discoverySignature(dirs: ChannelScanDirs): Promise<string> {
  const builtin = (await channelDirSignature(dirs.builtin)) ?? ""
  const custom = (await channelDirSignature(dirs.custom)) ?? ""
  return `builtin:${builtin}|custom:${custom}`
}

/** 发现结果。 */
export interface ChannelDiscoveryResult {
  channels: Map<string, ChannelDef>
  errors: Map<string, ChannelLoadError>
  signature: string
}

/**
 * 发现全部渠道（双域扫描，custom 后扫同名覆盖）。
 *
 * 失败隔离：单渠道 import 抛错/未导出 `channel`/字段非法 → 只记 `errors`，其余照常注册。
 * 二开覆盖版写坏时**保留旧可用版本**（否则「改二开版改坏了」会连带生产能力消失）。
 */
export async function discoverChannels(dirs: ChannelScanDirs): Promise<ChannelDiscoveryResult> {
  const channels = new Map<string, ChannelDef>()
  const errors = new Map<string, ChannelLoadError>()
  const signature = await discoverySignature(dirs)

  for (const [domain, dir] of [
    ["builtin", dirs.builtin],
    ["custom", dirs.custom],
  ] as const) {
    let entries
    try {
      entries = await readdir(dir, { withFileTypes: true })
    } catch {
      continue // 域不存在：正常（未二开/裁剪形态）
    }
    entries.sort((a, b) => a.name.localeCompare(b.name))
    for (const e of entries) {
      const loaded = await loadOne(dir, e, domain)
      if (!loaded) continue
      if ("error" in loaded) {
        if (!channels.has(loaded.name)) {
          errors.set(loaded.name, loaded.error)
        } else {
          const prev = errors.get(loaded.name)
          const note = `[${loaded.error.domain}] 覆盖版加载失败，已保留先前版本：${loaded.error.error}`
          errors.set(loaded.name, { ...loaded.error, error: prev ? `${prev.error}\n${note}` : note })
        }
        continue
      }
      if (domain === "custom" && channels.has(loaded.def.name)) errors.delete(loaded.def.name)
      channels.set(loaded.def.name, loaded.def)
    }
  }
  return { channels, errors, signature }
}

type OneLoaded = { def: ChannelDef } | { error: ChannelLoadError; name: string }

/** 加载单个定义（文件或目录形态）。 */
async function loadOne(
  dir: string,
  entry: { name: string; isDirectory(): boolean; isFile(): boolean },
  domain: "builtin" | "custom",
): Promise<OneLoaded | null> {
  let base: string
  let filePath: string | null = null
  if (entry.isFile()) {
    if (!isDefFile(entry.name)) return null
    base = entry.name.replace(/\.(ts|js)$/, "")
    if (!/^[a-z0-9_]+$/.test(base)) return null
    filePath = join(dir, entry.name)
  } else if (entry.isDirectory()) {
    base = entry.name
    if (!/^[a-z0-9_]+$/.test(base)) return null
    for (const candidate of [`${base}.ts`, "index.ts", `${base}.js`, "index.js"]) {
      const p = join(dir, base, candidate)
      const st = await stat(p).catch(() => null)
      if (st?.isFile()) {
        filePath = p
        break
      }
    }
    if (!filePath) return null // 纯资源目录
  } else {
    return null
  }

  try {
    // mtime 查询参数绕过模块缓存（Bun 对 file:// URL 的查询参数不生效，故用绝对路径 + query）
    const mtime = (await stat(filePath).catch(() => null))?.mtimeMs ?? 0
    const mod = (await import(`${filePath}?t=${mtime}`)) as { channel?: unknown; default?: unknown }
    const raw = mod.channel ?? mod.default
    if (!raw) {
      return {
        name: base,
        error: {
          name: base,
          domain,
          error: `${filePath} 未导出 channel（须 \`export const channel: ChannelDef = {…}\`，或 default 导出）`,
        },
      }
    }
    const invalid = validateChannelDef(raw, base)
    if (invalid) return { name: base, error: { name: base, domain, error: invalid } }
    const def = raw as ChannelDef
    if (def.name !== base) {
      return {
        name: base,
        error: { name: base, domain, error: `channel.name「${def.name}」与文件名「${base}」不一致（防止改名后配置失配）` },
      }
    }
    return { def }
  } catch (err) {
    return {
      name: base,
      error: { name: base, domain, error: `加载 ${filePath} 失败: ${String((err as Error)?.message || err)}` },
    }
  }
}

const NAME_RE = /^[a-z0-9_]+$/
const VERB_RE = /^[a-z0-9_]+$/
const ARG_TYPES = new Set(["string", "number", "boolean", "array", "object"])

/**
 * 定义校验（**字段错了就报错，不静默降级**）。
 *
 * 静默降级的典型危害：`verb` 拼错 → 模型调用永远失败而错误信息指不出是配置问题；
 * `targets` 写成字符串 → 白名单退化失效（那是最危险的一项）。
 */
export function validateChannelDef(raw: unknown, expectedName?: string): string | null {
  if (!raw || typeof raw !== "object") return "channel 必须是对象"
  const o = raw as Record<string, unknown>
  const allowed = new Set(["name", "description", "envVars", "verbs", "observes", "listens", "defaultKind", "targets", "act", "observe", "startListen", "stopListen", "start", "stop"])
  for (const k of Object.keys(o)) {
    if (!allowed.has(k)) return `含未知字段「${k}」（可用：${[...allowed].join(" / ")}）`
  }
  if (typeof o.name !== "string" || !NAME_RE.test(o.name)) return `name 必须是 [a-z0-9_]+（当前：${String(o.name)}）`
  if (expectedName !== undefined && o.name !== expectedName) {
    return `name「${o.name}」与文件名「${expectedName}」不一致`
  }
  if (typeof o.description !== "string" || !o.description.trim()) return "description 必须是非空字符串"

  // 声明面（verbs/observes/listens）与实现面（act/observe/startListen）必须成对出现：
  // 只有声明无实现 → 每次调用都抛，且错误位置离配置很远；只有实现无声明 → 模型不知道能调什么。
  for (const [declKey, implKey, implSig] of [
    ["verbs", "act", "async act(input, ctx)"],
    ["observes", "observe", "async observe(input, ctx)"],
    ["listens", "startListen", "async startListen(sink, ctx)"],
  ] as const) {
    const decl = o[declKey]
    const impl = o[implKey]
    if (decl !== undefined) {
      if (!decl || typeof decl !== "object" || Array.isArray(decl)) return `${declKey} 必须是对象（名称 → 声明）`
      const keys = Object.keys(decl as Record<string, unknown>)
      for (const key of keys) {
        if (!VERB_RE.test(key)) return `${declKey} 键「${key}」非法（须 [a-z0-9_]+）`
        const bad = validateCapabilitySpec((decl as Record<string, unknown>)[key], `${declKey}.${key}`)
        if (bad) return bad
      }
      if (keys.length > 0 && typeof impl !== "function") {
        return `${declKey} 有 ${keys.length} 项声明但缺少实现（须提供 \`${implSig}\`）`
      }
    }
    if (impl !== undefined && typeof impl !== "function") return `${implKey} 必须是函数`
    if (typeof impl === "function" && (decl === undefined || Object.keys(decl as object).length === 0)) {
      return `${implKey} 有实现但 ${declKey} 无声明：模型不知道能收/调什么（必须声明每项能力）`
    }
  }

  if (o.stopListen !== undefined && typeof o.stopListen !== "function") return "stopListen 若是提供必须是函数"

  if (typeof o.targets !== "undefined" && typeof o.targets !== "function") {
    return "targets 必须是函数（返回合法 address 数组，或 null 表示不做地址级限制）"
  }
  if (o.start !== undefined && typeof o.start !== "function") return "start 必须是非空函数"
  if (o.stop !== undefined && typeof o.stop !== "function") return "stop 若提供必须是函数"
  if (o.envVars !== undefined) {
    if (!Array.isArray(o.envVars)) return "envVars 必须是数组"
    for (const v of o.envVars) {
      if (!v || typeof v !== "object") return "envVars 每项必须是对象"
      const item = v as Record<string, unknown>
      if (typeof item.name !== "string" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(item.name)) {
        return `envVars.name 非法：${String(item.name)}`
      }
      if (typeof item.description !== "string") return `envVars「${item.name}」缺少 description`
    }
  }
  return null
}

/** 单条能力声明校验。 */
function validateCapabilitySpec(raw: unknown, where: string): string | null {
  if (!raw || typeof raw !== "object") return `${where} 必须是对象`
  const o = raw as Record<string, unknown>
  const allowed = new Set(["summary", "args", "writes"])
  for (const k of Object.keys(o)) {
    if (!allowed.has(k)) return `${where} 含未知字段「${k}」（可用：${[...allowed].join(" / ")}）`
  }
  if (typeof o.summary !== "string" || !o.summary.trim()) return `${where} 缺少 summary（模型靠它判断何时用）`
  if (o.writes !== undefined && typeof o.writes !== "boolean") return `${where}.writes 必须是布尔`
  if (o.args !== undefined) {
    if (!o.args || typeof o.args !== "object" || Array.isArray(o.args)) return `${where}.args 必须是对象`
    for (const [k, spec] of Object.entries(o.args as Record<string, unknown>)) {
      if (!/^[a-z0-9_]+$/.test(k)) return `${where}.args 参数名「${k}」非法（须 [a-z0-9_]+）`
      const bad = validateArgSpec(spec, `${where}.args.${k}`)
      if (bad) return bad
    }
  }
  return null
}

function validateArgSpec(raw: unknown, where: string): string | null {
  if (!raw || typeof raw !== "object") return `${where} 必须是对象`
  const o = raw as Record<string, unknown>
  const allowed = new Set(["type", "description", "items", "enum", "required"])
  for (const k of Object.keys(o)) {
    if (!allowed.has(k)) return `${where} 含未知字段「${k}」`
  }
  if (typeof o.type !== "string" || !ARG_TYPES.has(o.type)) {
    return `${where}.type 非法（须 ${[...ARG_TYPES].join("/")}；当前 ${String(o.type)}）`
  }
  if (typeof o.description !== "string" || !o.description.trim()) return `${where} 缺少 description`
  if (o.required !== undefined && typeof o.required !== "boolean") return `${where}.required 必须是布尔`
  if (o.enum !== undefined) {
    if (!Array.isArray(o.enum) || o.enum.some((x) => typeof x !== "string")) return `${where}.enum 必须是字符串数组`
  }
  if (o.items !== undefined) {
    if (o.type !== "array") return `${where}.items 只用于 array 类型`
    const bad = validateArgSpec(o.items, `${where}.items`)
    if (bad) return bad
  }
  return null
}

/** 读取定义文件原文（诊断/面板展示用；失败返回 null 而不是抛错）。 */
export async function readChannelFile(dir: string, name: string): Promise<string | null> {
  for (const candidate of [join(dir, `${name}.ts`), join(dir, name, `${name}.ts`), join(dir, name, "index.ts")]) {
    const text = await readFile(candidate, "utf8").catch(() => null)
    if (text !== null) return text
  }
  return null
}

/** 渠道运行状态（面板/诊断可见）。 */
export interface ChannelState {
  name: string
  description: string
  /** 是否通过启停名单。 */
  enabled: boolean
  /** 是否已启动。 */
  started: boolean
  /** 启动失败原因（若有）。 */
  error?: string
  /** 写面动词清单。 */
  verbs: string[]
  /** 读面观测项清单。 */
  observes: string[]
  /** 输入面监听项清单。 */
  listens: string[]
  /** 是否已建立订阅。 */
  listening: boolean
  /** 是否有限定目标白名单。 */
  restricted: boolean
}

export interface ChannelRegistryOptions {
  scanDirs: ChannelScanDirs
  /** 数据根（`ctx.home`）。 */
  home: string
  /** 启停名单（`GEBAI_CHANNELS_ENABLE` / `DISABLE`）：白名单非空仅启用名单内，先白后黑。 */
  enable?: string[]
  disable?: string[]
  /** 环境变量读取器（缺省 `process.env`；测试注入）。 */
  envReader?: (name: string) => string | undefined
  /** 宿主注入的共享依赖（经 `ctx.deps` 给渠道只读访问）。 */
  deps?: Readonly<Record<string, unknown>>
  log?: { info(msg: string): void; warn(msg: string): void; error(msg: string): void }
}

/**
 * 渠道注册表：发现 / 启停 / 热加载 / 诊断。
 *
 * 生命周期：`init()`（发现 + 启动全部启用渠道）→ 运行 → `stop()`（逆序停止）。
 * 热加载：`refreshIfChanged()` 比对目录签名（成本约一次目录遍历）。
 */
export class ChannelRegistry {
  private defs = new Map<string, ChannelDef>()
  private errors = new Map<string, ChannelLoadError>()
  private started = new Map<string, ChannelDef>()
  /** 已建立订阅的渠道（`startListen` 已成）。 */
  private listening = new Set<string>()
  /** 输入桥接器（宿主经 `attachDispatcher` 注入；未注入时不订阅）。 */
  private bridge: { deliver(ch: string, defKind: import("./channel").IngressKind | undefined, ev: import("./channel").IngressEvent): Promise<import("./channel").EmitResult> } | null = null
  private startErrors = new Map<string, string>()
  private signature: string | null = null
  private starting = false
  private explicit = new Set<string>()

  constructor(private readonly opts: ChannelRegistryOptions) {}

  /** 发现并启动全部启用的渠道（失败隔离：单渠道失败不阻断其它）。 */
  async init(): Promise<void> {
    await this.discover()
    await this.startAll()
  }

  /**
   * 注入输入桥接器（必须在 `init()` 之前调用）。
   *
   * 为何由宿主注入而不内置：渠道只回答「收到了什么」，「这归谁、进哪个会话」
   * 是调度内核（persona 或任何宿主）的事。注册表不依赖内核，保持可独立单测。
   *
   * 参数取结构形态而非导入 `ChannelInputBridge` 类型：避免 core 与 bridge 相互引用。
   */
  attachDispatcher(bridge: {
    deliver(ch: string, defKind: import("./channel").IngressKind | undefined, ev: import("./channel").IngressEvent): Promise<import("./channel").EmitResult>
  }): void {
    this.bridge = bridge
  }

  /** 该渠道是否已建立订阅（诊断）。 */
  isListening(name: string): boolean {
    return this.listening.has(name)
  }

  /** 停止全部渠道（逆序；单渠道异常只记日志）。
   *
   * 先停监听再停连接：监听可能持有连接的读者，反过来会拿到半死的连接。
   */
  async stop(): Promise<void> {
    for (const name of [...this.listening].reverse()) {
      const def = this.defs.get(name)
      try {
        await def?.stopListen?.(def ? this.context(def) : ({} as never))
      } catch (err) {
        this.log("warn", `停止渠道 ${name} 的订阅失败: ${String((err as Error)?.message || err)}`)
      }
    }
    this.listening.clear()
    for (const [name, def] of [...this.started.entries()].reverse()) {
      try {
        await def.stop?.()
      } catch (err) {
        this.log("warn", `停止渠道 ${name} 失败: ${String((err as Error)?.message || err)}`)
      }
    }
    this.started.clear()
  }

  /** 热加载检查（目录签名变化即重扫重启）。返回是否发生了重扫。 */
  async refreshIfChanged(): Promise<boolean> {
    const sig = await discoverySignature(this.opts.scanDirs)
    if (sig === this.signature) return false
    this.log("info", "渠道目录变化，重新扫描")
    await this.stop()
    await this.discover()
    await this.startAll()
    return true
  }

  /** 取渠道定义（未注册返回 null）。 */
  get(name: string): ChannelDef | null {
    return this.defs.get(name) ?? null
  }

  /** 已启用且已启动的渠道（调用方据此判断「能不能用」）。 */
  usable(name: string): ChannelDef | null {
    const def = this.defs.get(name)
    if (!def) return null
    if (!this.isEnabled(name)) return null
    return this.started.has(name) ? def : null
  }

  /** 已注册的全部渠道（含未启用）。 */
  list(): ChannelDef[] {
    return [...this.defs.values()]
  }

  /** 状态快照（面板/诊断）。 */
  state(): ChannelState[] {
    const out: ChannelState[] = []
    for (const [name, def] of this.defs) {
      out.push({
        name,
        description: def.description,
        enabled: this.isEnabled(name),
        started: this.started.has(name),
        ...(this.startErrors.has(name) ? { error: this.startErrors.get(name) } : {}),
        verbs: Object.keys(def.verbs ?? {}),
        observes: Object.keys(def.observes ?? {}),
        listens: Object.keys(def.listens ?? {}),
        listening: this.listening.has(name),
        restricted: typeof def.targets === "function",
      })
    }
    for (const [name, err] of this.errors) {
      if (out.some((x) => x.name === name)) continue
      out.push({
        name,
        description: "",
        enabled: false,
        started: false,
        error: `[${err.domain}] ${err.error}`,
        verbs: [],
        observes: [],
        listens: [],
        listening: false,
        restricted: false,
      })
    }
    return out.sort((a, b) => a.name.localeCompare(b.name))
  }

  /** 加载错误清单。 */
  loadErrors(): Record<string, string> {
    const out: Record<string, string> = {}
    for (const [n, e] of this.errors) out[n] = `[${e.domain}] ${e.error}`
    for (const [n, e] of this.startErrors) out[n] = e
    return out
  }

  /**
   * 程序化注册（不经目录扫描）。
   *
   * 语义与扫描发现一致：同名覆盖，且不受启停名单约束（显式注册即视为部署方意图）。
   * 仍走字段校验（不能绕过）。
   */
  register(def: ChannelDef): void {
    const invalid = validateChannelDef(def, def.name)
    if (invalid) {
      this.startErrors.set(def.name, `注册被拒绝：${invalid}`)
      this.log("warn", `渠道 ${def.name} 注册被拒绝：${invalid}`)
      return
    }
    this.defs.set(def.name, def)
    this.explicit.add(def.name)
  }

  /** 手动启动单个渠道。 */
  async startOne(name: string): Promise<boolean> {
    const def = this.defs.get(name)
    if (!def || this.started.has(name)) return false
    return this.startDef(def)
  }

  /** 构造渠道上下文：`ctx` 是全流程唯一出口。 */
  context(def: ChannelDef): ChannelContext {
    const declared = new Set((def.envVars ?? []).map((v) => v.name))
    const readEnv = this.opts.envReader ?? ((n: string) => process.env[n])
    return {
      name: def.name,
      home: this.opts.home,
      deps: this.opts.deps ?? {},
      log: (level, msg) => this.log(level, `[${def.name}] ${msg}`),
      env: (name) => {
        // 白名单过滤：未声明即不可读（防「读告警的渠道」顺手拿到模型密钥）
        if (!declared.has(name)) {
          this.log("warn", `渠道 ${def.name} 读取未声明的环境变量 ${name}（已拒绝；请在 envVars 中声明）`)
          return undefined
        }
        return readEnv(name)
      },
    }
  }

  // ── 内部 ──

  private async discover(): Promise<void> {
    if (this.starting) return
    this.starting = true
    try {
      const result = await discoverChannels(this.opts.scanDirs)
      // 合并而非覆盖：显式 register() 的渠道不经目录扫描，重扫不能把它们丢掉
      for (const [name, def] of result.channels) {
        if (this.explicit.has(name)) continue
        this.defs.set(name, def)
      }
      for (const [name, err] of result.errors) {
        if (this.explicit.has(name)) continue
        this.errors.set(name, err)
      }
      this.signature = result.signature
      // 已消失的目录渠道从 defs 里摘除（显式注册的保留）
      for (const name of [...this.defs.keys()]) {
        if (this.explicit.has(name)) continue
        if (!result.channels.has(name)) this.defs.delete(name)
      }
      for (const [name, err] of result.errors) {
        this.log("warn", `渠道 ${name} 加载失败 [${err.domain}]: ${err.error}`)
      }
      this.warnUnknownAllowlistNames()
    } finally {
      this.starting = false
    }
  }

  private async startAll(): Promise<void> {
    for (const def of this.defs.values()) {
      if (!this.isEnabled(def.name)) continue
      await this.startDef(def)
    }
  }

  /** 启动单个渠道（失败隔离：记 startErrors，不影响其它渠道）。 */
  private async startDef(def: ChannelDef): Promise<boolean> {
    try {
      await def.start?.(this.context(def))
      this.started.set(def.name, def)
      this.startErrors.delete(def.name)
      const faces = [
        Object.keys(def.verbs ?? {}).length ? `写 ${Object.keys(def.verbs!).join("/")}` : "",
        Object.keys(def.observes ?? {}).length ? `读 ${Object.keys(def.observes!).join("/")}` : "",
        Object.keys(def.listens ?? {}).length ? `收 ${Object.keys(def.listens!).join("/")}` : "",
      ].filter(Boolean)
      this.log("info", `渠道已启动: ${def.name}（${faces.join("；") || "无能力"}）`)
      // 输入面：start 成功后才订阅（避免停不下来的半启动态）。
      // 监听失败只记日志、不回滚 start —— 该渠道的写/读面仍然可用（降级而非全损）。
      if (def.startListen && this.bridge) {
        // 每个渠道拿到**绑了自己名字**的 sink：渠道无从伪造路由键
        const chName = def.name
        const dk = def.defaultKind
        const sink = {
          emit: (ev: import("./channel").IngressEvent) => this.bridge!.deliver(chName, dk, ev),
        }
        try {
          await def.startListen(sink, this.context(def))
          this.listening.add(def.name)
        } catch (err) {
          const msg = `订阅失败: ${String((err as Error)?.message || err)}`
          this.startErrors.set(def.name, msg)
          this.log("warn", `渠道 ${def.name} ${msg}（写/读面仍可用）`)
        }
      }
      return true
    } catch (err) {
      const msg = `启动失败: ${String((err as Error)?.message || err)}`
      this.startErrors.set(def.name, msg)
      this.log("warn", `渠道 ${def.name} ${msg}`)
      return false
    }
  }

  /** 启停名单判定（先白后黑）。 */
  private isEnabled(name: string): boolean {
    if (this.explicit.has(name)) return true
    const enable = this.opts.enable ?? []
    const disable = this.opts.disable ?? []
    if (enable.length && !enable.includes(name)) return false
    return !disable.includes(name)
  }

  /** 名单里的未知名告警（拼错名单会让渠道静默不启用，属最难排查的一类）。 */
  private warnUnknownAllowlistNames(): void {
    for (const name of [...(this.opts.enable ?? []), ...(this.opts.disable ?? [])]) {
      if (!this.defs.has(name) && !this.errors.has(name)) {
        this.log("warn", `启停名单中的渠道「${name}」不存在（拼写错误？已注册：${[...this.defs.keys()].join(", ") || "无"}）`)
      }
    }
  }

  private log(level: "info" | "warn" | "error", msg: string): void {
    this.opts.log?.[level](`[channels] ${msg}`)
  }
}
