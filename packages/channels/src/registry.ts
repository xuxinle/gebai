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
import { materializeInstance, parseChannelInstances } from "./config"

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
  const allowed = new Set(["name", "description", "config", "requiresConfig", "dirs", "verbs", "observes", "listens", "defaultKind", "targets", "act", "observe", "startListen", "stopListen", "start", "stop"])
  for (const k of Object.keys(o)) {
    if (!allowed.has(k)) return `含未知字段「${k}」（可用：${[...allowed].join(" / ")}）`
  }
  // dirs：类型能力声明，写错会让该方向永远展不开（静默失效）
  if (o.dirs !== undefined) {
    if (!Array.isArray(o.dirs) || o.dirs.length === 0) return "dirs 必须是非空数组（如 [\"in\",\"out\"]）"
    for (const d of o.dirs) {
      if (d !== "in" && d !== "out") return `dirs 只能含 "in" / "out"（当前：${String(d)}）`
    }
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
  if (o.config !== undefined) {
    if (!o.config || typeof o.config !== "object" || Array.isArray(o.config)) {
      return "config 必须是对象（键 → 声明，如 { appId: { type: \"string\", description: \"…\", required: true } }）"
    }
    const TYPES = new Set(["string", "number", "boolean", "string[]"])
    for (const [k, v] of Object.entries(o.config as Record<string, unknown>)) {
      if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(k)) return `config 键名非法：${k}（须 [A-Za-z_][A-Za-z0-9_]*）`
      if (!v || typeof v !== "object") return `config「${k}」必须是对象`
      const spec = v as Record<string, unknown>
      if (typeof spec.type !== "string" || !TYPES.has(spec.type)) {
        return `config「${k}」的 type 必须是 ${[...TYPES].join(" / ")}（当前：${String(spec.type)}）`
      }
      if (typeof spec.description !== "string" || !spec.description.trim()) {
        return `config「${k}」缺少 description`
      }
    }
  }
  if (o.requiresConfig !== undefined && typeof o.requiresConfig !== "boolean") {
    return "requiresConfig 必须是布尔值"
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
  /**
   * 方向：`in` 只收（不建写/读面）、`out` 只发/读（不建连接）。
   *
   * 面板必须区分，否则出站实例的 `listening=false` 会被误判成“只能发不能收”的故障态——
   * 而它本来就不该建连接。
   */
  dir: "in" | "out"
  /**
   * 这个实例能否启停监听（前端据此决定开关是否可点）。
   *
   * `dir=out` 时为 false（本来就不建连接）；渠道类型没实现 `startListen` 时也为 false。
   */
  canListen: boolean
}

export interface ChannelRegistryOptions {
  scanDirs: ChannelScanDirs
  /** 数据根（`ctx.home`）。 */
  home: string
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
  /** 类型名 → 定义（来自目录扫描 / 显式注册）。实例展开前只有这一层。 */
  private types = new Map<string, ChannelDef>()
  /** 实例名 → 定义（已按 `GEBAI_CHANNEL_<TYPE>` 展开）。对外一律看这一层。 */
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
  /** 实例名 → 该实例的配置（解析自 `GEBAI_CHANNEL_<DIR>_<TYPE>`）；与 `defs` 分开存。 */
  private instanceSettings = new Map<string, Readonly<Record<string, unknown>>>()
  /** 实例名 → 方向（`in` 只收、`out` 只发/读）。 */
  private instanceDirs = new Map<string, "in" | "out">()
  /**
   * 实例名 → **渠道类型名**（`feishu_out` → `feishu`）。
   *
   * 为何必须单独存：实例名是部署方起的（`feishu_main` / `ops_bot`…），代码里
   * 任何「这是不是飞书渠道」的判断都不能靠名字猜——猜错的形态是**按实例名配置的
   * 治理整段静默失效**（人格出站审核、群白名单、@全体整形全部跳过），而日志里
   * 一个字都没有。类型名来自定义文件本身，是稳定事实。
   */
  private instanceType = new Map<string, string>()

  constructor(private readonly opts: ChannelRegistryOptions) {}

  /** 发现并启动全部启用的渠道（失败隔离：单渠道失败不阻断其它）。 */
  async init(): Promise<void> {
    await this.discover()
    await this.startAll()
  }

  /**
   * 只做发现与实例展开，**不启动**（也不建连接）。
   *
   * 为何需要这一步：人格在启动早期就要解析 `channels:["实例名"]`（输出目标、
   * 入站收窄），而它必须知道“这个实例允许哪些群”——那是渠道配置的事。
   * 但此时渠道还不能启动（依赖尚未就绪）。`init()` 之后再调一次 `discover()`
   * 是幂等的（签名未变不重扫），所以两步可以安全拼起来。
   */
  async discoverOnly(): Promise<void> {
    await this.discover()
  }

  /** 实例名 → 允许的群（供人格引用解析；无 chats 的实例为空数组）。 */
  instanceChats(): Map<string, string[]> {
    const out = new Map<string, string[]>()
    for (const [name, settings] of this.instanceSettings) {
      const c = settings.chats
      out.set(name, Array.isArray(c) ? (c as string[]).filter((x): x is string => typeof x === "string") : [])
    }
    return out
  }

  /** 实例名 → 方向（`in`/`out`）。人格据此报“拿输出实例配 listen”这类错。 */
  instanceDirections(): Map<string, "in" | "out"> {
    return new Map(this.instanceDirs)
  }

  /**
   * 实例名 → 渠道类型名（`feishu_out` → `feishu`）。
   *
   * 供「按渠道类型做治理」的调用方使用（如人格出站治理只针对飞书，而飞书可能是
   * `feishu` / `feishu_out` / `ops_bot` 任一个实例名）。
   */
  instanceTypes(): Map<string, string> {
    return new Map(this.instanceType)
  }

  /** 实例名 → 该实例的原始配置项（只读快照）。 */
  instanceConfig(name: string): Readonly<Record<string, unknown>> | undefined {
    return this.instanceSettings.get(name)
  }

  /**
   * 某个实例的凭据与授权补充（供人格 OAuth 用）。
   *
   * 为何从实例配置取而不是读环境变量：
   * 凭据与 `offline_access` 都是**应用级**属性，跟着实例走才是对的——
   * 同类配两份时（`feishu_in` / `feishu_ops`）环境变量只能表达一份。
   * 旧形态把它们焊在 `GEBAI_CHANNELS_FEISHU_APP_ID` / `GEBAI_PERSONA_SCOPES` 上，
   * 实例化之后这两个变量已经不存在，继续读只会静默拿到 `undefined`（真实踩过的坑）。
   */
  instanceCredentials(name: string): { appId?: string; appSecret?: string; scopes: string[] } | undefined {
    const c = this.instanceSettings.get(name)
    if (!c) return undefined
    const str = (k: string): string | undefined => (typeof c[k] === "string" && c[k] ? (c[k] as string) : undefined)
    const scopes = Array.isArray(c.scopes)
      ? (c.scopes as unknown[]).filter((x): x is string => typeof x === "string")
      : []
    return { appId: str("appId"), appSecret: str("appSecret"), scopes }
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

  /**
   * 启动/停止单个实例的**监听**（不动它的 act/observe 能力）。
   *
   * 为何单独一个方法而不复用 `startOne/stopOne`：那两者启停的是“整个实例”
   * （含写/读面与连接）。而“停监听”的真实诉求是「先别收消息了，但还能发」——
   * 比如群太吵了、或要重新授权前先把入站掐断。两者混在一起就只能二选一。
   *
   * 返回 `{ ok:false }` 而不是抛错：这是诊断/运营动作，失败原因要能直接展示给用户。
   */
  async setListening(name: string, want: boolean): Promise<{ ok: boolean; reason?: string }> {
    const def = this.defs.get(name)
    if (!def) return { ok: false, reason: `未知渠道实例「${name}」` }
    if (this.instanceDirs.get(name) === "out") {
      // 出站实例本来就不建连接（能力面里没有 startListen），开关对它没有意义。
      // 不静默成功：前端要能告诉用户“这个开关为何点不了”。
      return { ok: false, reason: `实例「${name}」是出站实例（只发/读，不建连接），没有可启停的监听` }
    }
    if (!def.startListen) return { ok: false, reason: `实例「${name}」不支持监听（渠道类型未实现 startListen）` }
    if (want === this.listening.has(name)) {
      // 幂等：已经是目标状态，直接认为成功（避免重复建连接）
      return { ok: true }
    }
    try {
      if (want) {
        await this.startListeningFor(name, def)
      } else {
        await def.stopListen?.(this.context(def))
        this.listening.delete(name)
        this.log("info", `实例「${name}」的监听已手动停止（发送/读取能力不受影响）`)
      }
      return { ok: true }
    } catch (err) {
      return { ok: false, reason: `操作失败：${String((err as Error)?.message || err)}` }
    }
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
    return this.started.has(name) ? def : null
  }

  /** 已展开的全部渠道实例（名字 = 配置里的 name；没有启停名单，存在即启用）。 */
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
        // 没有启停名单了：能展开成实例就是启用（存在即启用）
        enabled: true,
        started: this.started.has(name),
        ...(this.startErrors.has(name) ? { error: this.startErrors.get(name) } : {}),
        verbs: Object.keys(def.verbs ?? {}),
        observes: Object.keys(def.observes ?? {}),
        listens: Object.keys(def.listens ?? {}),
        listening: this.listening.has(name),
        restricted: typeof def.targets === "function",
        dir: this.instanceDirs.get(name) ?? "out",
        // 能启停监听 = 入站实例 + 渠道类型实现了 startListen
        canListen: (this.instanceDirs.get(name) ?? "out") === "in" && typeof def.startListen === "function",
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
        dir: "out",
        canListen: false,
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
   *
   * 注册的是**类型**，随即按配置展开成实例（与目录发现走同一条展开路径）——
   * 否则程序化注册的渠道会绕过实例化，`ctx.config` 永远是空的。
   */
  register(def: ChannelDef): void {
    const invalid = validateChannelDef(def, def.name)
    if (invalid) {
      this.startErrors.set(def.name, `注册被拒绝：${invalid}`)
      this.log("warn", `渠道 ${def.name} 注册被拒绝：${invalid}`)
      return
    }
    this.types.set(def.name, def)
    this.explicit.add(def.name)
    this.expandTypes([def])
  }

  /** 手动启动单个渠道。 */
  async startOne(name: string): Promise<boolean> {
    const def = this.defs.get(name)
    if (!def || this.started.has(name)) return false
    return this.startDef(def)
  }

  /**
   * 构造渠道上下文：`ctx` 是全流程唯一出口。
   *
   * 实例隔离靠 `name`：同一份类型定义（`feishu.ts`）的多个实例（`feishu_main` /
   * `feishu_ops`）各自拿到**自己的 settings**，所以凭据/白名单不会串。
   */
  context(def: ChannelDef): ChannelContext {
    const key = def.name
    const settings = this.instanceSettings.get(key) ?? {}
    return {
      name: def.name,
      home: this.opts.home,
      deps: this.opts.deps ?? {},
      log: (level, msg) => this.log(level, `[${def.name}] ${msg}`),
      config: settings,
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
        this.types.set(name, def)
      }
      for (const [name, err] of result.errors) {
        if (this.explicit.has(name)) continue
        this.errors.set(name, err)
      }
      this.signature = result.signature
      // 已消失的目录渠道从 types 里摘除（显式注册的保留）
      for (const name of [...this.types.keys()]) {
        if (this.explicit.has(name)) continue
        if (!result.channels.has(name)) this.types.delete(name)
      }
      await this.expandInstances()
      for (const [name, err] of result.errors) {
        this.log("warn", `渠道 ${name} 加载失败 [${err.domain}]: ${err.error}`)
      }
    } finally {
      this.starting = false
    }
  }

  /**
   * 把「类型」展开成「实例」：一份 `GEBAI_CHANNEL_<TYPE>` 配置 → 一个可运行实例。
   *
   * 两个关键决定：
   * - **实例名与类型名解耦**：`feishu.ts` 能配出 `feishu_main` / `feishu_ops`；
   *   同类的第二份配置从此有地方可写（旧形态因变量名把类型焊死，做不到）。
   * - **未配置但可选**：`requiresConfig=false` 的类型（如 local）自动得到一个
   *   缺省实例（名字 = 类型名），不必让部署方先写一行 JSON；
   *   而 `requiresConfig=true`（如 feishu）未配置时**不凭空造实例**——
   *   一个没有凭据的 feishu 实例只会把“配了但没反应”变成更难查的形态。
   */
  private async expandInstances(): Promise<void> {
    // 先清掉旧的展开结果（热加载/重扫时要能反映配置变化）
    for (const name of [...this.defs.keys()]) {
      if (!this.types.has(name)) {
        this.defs.delete(name)
        this.instanceSettings.delete(name)
        this.instanceDirs.delete(name)
        this.instanceType.delete(name)
      }
    }
    this.expandTypes([...this.types.values()])
  }

  /** 把指定类型集合展开成实例（`register` 与 `discover` 共用同一条路径）。 */
  private expandTypes(list: ChannelDef[]): void {
    const readEnv = this.opts.envReader ?? ((n: string) => process.env[n])

    for (const typeDef of list) {
      const typeName = typeDef.name
      // 类型声明了哪些方向就展开哪些组；未声明 = 两者都支持（兼容旧写法与自带缺省值的类型）
      const dirs: Array<"in" | "out"> = typeDef.dirs ?? ["in", "out"]
      let made = 0

      for (const dir of dirs) {
        const varName = `GEBAI_CHANNEL_${dir.toUpperCase()}_${typeName.toUpperCase()}`
        const parsed = parseChannelInstances(typeName, readEnv(varName), typeDef.config, dir)
        for (const e of parsed.errors) this.log("warn", `[配置] ${e}`)
        if (parsed.errors.length) {
          // 配错时不启用（也不回退默认值）：带着半份配置跑起来比不跑更难查
          this.startErrors.set(`${dir}:${typeName}`, `${varName} 配置有误：${parsed.errors[0]}`)
          continue
        }

        for (const inst of parsed.instances) {
          // 两个方向的实例名必须全局唯一：它们共享 target 前缀与事件 channel 字段，
          // 同名会让“发到 feishu_x”指向不可预期的一侧。
          if (this.defs.has(inst.name)) {
            this.startErrors.set(inst.name, `实例名「${inst.name}」与已有实例重名（in/out 两组之间也必须唯一）`)
            this.log("warn", `渠道实例名「${inst.name}」重名，已忽略 ${varName} 中的这一项`)
            continue
          }
          this.defs.set(inst.name, materializeInstance(typeDef, inst))
          this.instanceSettings.set(inst.name, Object.freeze({ ...inst.settings }))
          this.instanceDirs.set(inst.name, dir)
          this.instanceType.set(inst.name, typeName)
          made++
        }
      }

      // 两端都未配置：必需配置的类型要一条可查询的提示；可选类型给一个缺省**出站**实例
      if (made === 0 && typeDef.requiresConfig) {
        const hint =
          `未配置 ${dirs.map((d) => `GEBAI_CHANNEL_${d.toUpperCase()}_${typeName.toUpperCase()}`).join(" 或 ")}` +
          `（JSON 数组，每项一个实例，如 [{"name":"${typeName}_main",${Object.keys(typeDef.config ?? {})
            .slice(0, 2)
            .map((k) => `"${k}":"…"`)
            .join(",")}}]）`
        this.log("warn", `渠道类型「${typeName}」${hint}`)
        this.startErrors.set(typeName, hint)
      } else if (made === 0 && dirs.includes("out")) {
        // 可选类型且未配置：给一个缺省**出站**实例（名字 = 类型名）。
        //
        // 为何缺省只造 out、不造 in：入站往往意味着常驻资源（轮询定时器/长连接），
        // 为“没写任何配置的部署”无形中多开一个轮询器是不对的；
        // 而缺省出站实例是「本地记忆/落盘」这类开箱即用的能力，代价为零。
        const def = materializeInstance(typeDef, { name: typeName, dir: "out", settings: {} })
        if (!this.defs.has(typeName)) {
          this.defs.set(typeName, def)
          this.instanceSettings.set(typeName, Object.freeze({}))
          this.instanceDirs.set(typeName, "out")
          this.instanceType.set(typeName, typeName)
        }
      }
    }
  }

  private async startAll(): Promise<void> {
    for (const def of this.defs.values()) {
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
        try {
          await this.startListeningFor(def.name, def)
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

  /**
   * 真正把监听建起来（`startDef` 与手动启停共用）。
   *
   * 每个渠道拿到**绑了自己名字**的 sink：渠道无从伪造路由键。
   * 抛错由调用方处理（启动期记 loadError 降级，手动启停则回给用户）。
   */
  private async startListeningFor(name: string, def: ChannelDef): Promise<void> {
    if (!this.bridge) throw new Error("未注入输入桥接器（先 attachDispatcher），无法订阅")
    const chName = name
    const dk = def.defaultKind
    const sink = {
      emit: (ev: import("./channel").IngressEvent) => this.bridge!.deliver(chName, dk, ev),
    }
    await def.startListen!(sink, this.context(def))
    this.listening.add(name)
    this.startErrors.delete(name)
  }

  private log(level: "info" | "warn" | "error", msg: string): void {
    this.opts.log?.[level](`[channels] ${msg}`)
  }
}
