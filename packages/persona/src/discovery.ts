/**
 * 事件源发现与注册表：**与子 Agent 同款扩展机制**（丢文件即注册）。
 *
 * 四套机制逐条对齐 `SubAgentManager`（`core/agents/subagents.ts`）：
 *
 * | 机制 | 子 Agent | 本文件 |
 * |---|---|---|
 * | 目录扫描 | `src/agents/` + `custom/agents/` 双域 | `sources/` + `custom/persona/` 双域 |
 * | 热加载 | 目录签名（`路径:mtime`）比对 | `discoverySignature` 同算法 |
 * | 失败隔离 | 单文件抛错只记 `loadErrors` | 同 |
 * | 二开覆盖 | custom 后扫、同名胜出 | 同 |
 * | 命名约束 | `[a-z0-9_]+` | 同 |
 *
 * 为何必须对齐而不是「另立一套简便机制」：仓库里已经有两套「丢文件即注册」
 * （子 Agent、全局工具），再来一套不同规则的会让维护者反复踩「为什么这个不生效」。
 * 规则的**一致性本身**是可用性。
 *
 * 与子 Agent 的关键差异：事件源**不需要预载/装载概念**（不存在「装载一个源」——
 * 源是常驻订阅，要么启用要么不启用），故无 `preload`/`loadedSubAgents` 对应物。
 */

import { readdir, readFile, stat } from "node:fs/promises"
import { join } from "node:path"
import type { EventSourceDef, EventSourceLoadError } from "./source"

/** 扫描域（内置 + 二开）。 */
export interface EventSourceScanDirs {
  builtin: string
  custom: string
}

/** 默认扫描域：与子 Agent 的物理分域同构（基建与定义分离，无需排除清单）。 */
export function defaultEventSourceScanDirs(opts: { packageRoot: string; repoRoot?: string }): EventSourceScanDirs {
  // 仓库根：packageRoot 为 `<repo>/packages/persona`，上两级即仓库根
  // （二进制/产物形态下 custom/ 不存在 → 签名 null → 跳过，与 custom/agents 同）
  const repoRoot = opts.repoRoot ?? join(opts.packageRoot, "..", "..")
  return {
    // 内置源：本包 src/sources/（包内自带，随包分发）
    builtin: join(opts.packageRoot, "src", "sources"),
    // 二开源：仓库根 custom/persona/（**代码域，入库随代码迁移**，与 custom/agents 同级同语义）
    custom: join(repoRoot, "custom", "persona"),
  }
}

/**
 * 目录签名：递归收集定义文件（`.ts`/`.js`，排除 `.test.`）的 `相对路径:mtimeMs` 排序拼接。
 *
 * 与 `subagentsDirSignature` 同算法。**目录不存在返回 `null`**（区分「域未使用」与
 * 「域为空」：前者不算错误，后者是配了空目录）；存在但无定义文件返回空串。
 */
export async function sourceDirSignature(dir: string): Promise<string | null> {
  let entries
  try {
    entries = await readdir(dir, { withFileTypes: true })
  } catch {
    return null
  }
  const parts: string[] = []
  for (const e of entries) {
    if (e.isDirectory()) {
      // 目录形式：{dir}/{name}/{name}.ts 或 index.ts（与子 Agent 目录形态一致）
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

/** 双域签名（热加载判定单一来源，`discover` 与 `refreshIfChanged` 同源防漂移）。 */
export async function discoverySignature(dirs: EventSourceScanDirs): Promise<string> {
  const builtin = (await sourceDirSignature(dirs.builtin)) ?? ""
  const custom = (await sourceDirSignature(dirs.custom)) ?? ""
  return `builtin:${builtin}|custom:${custom}`
}

/** 是否为定义文件（排除测试与类型声明）。 */
function isDefFile(name: string): boolean {
  if (!/\.(ts|js)$/.test(name)) return false
  if (name.endsWith(".test.ts") || name.endsWith(".test.js")) return false
  if (name.endsWith(".d.ts")) return false
  if (name.startsWith("_")) return false // 下划线前缀 = 内部辅助，不参与注册
  return true
}

/** 发现结果。 */
export interface DiscoveryResult {
  /** 已注册的源（name → def）。 */
  sources: Map<string, EventSourceDef>
  /** 加载失败（name → 原因）。 */
  errors: Map<string, EventSourceLoadError>
  /** 本次扫描的签名（下次热加载比对）。 */
  signature: string
}

/**
 * 发现全部事件源（双域扫描，custom 后扫同名覆盖）。
 *
 * 失败隔离：单个源 import 抛错/未导出 `source`/字段非法 → 只记 `errors`，
 * 其余源照常注册。**绝不因一个源坏掉而让事件驱动整体不可用**——那会让
 * 「飞书源配错」连带「定时任务也不跑了」，是最难排查的耦合。
 */
export async function discoverEventSources(dirs: EventSourceScanDirs): Promise<DiscoveryResult> {
  const sources = new Map<string, EventSourceDef>()
  const errors = new Map<string, EventSourceLoadError>()
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
    // 稳定顺序：目录优先按名排序，保证 custom 覆盖语义可复现（不依赖文件系统返回序）
    entries.sort((a, b) => a.name.localeCompare(b.name))
    for (const e of entries) {
      const loaded = await loadOne(dir, e, domain)
      if (!loaded) continue
      if ("error" in loaded) {
        // 同名源已注册（custom 试图覆盖内置，但新版本坏了）→ **保留旧的可用版本**，
        // 并把失败记进 errors。不能丢掉旧版：那会让「改二开版改坏了」连带生产能力消失。
        if (!sources.has(loaded.name)) {
          errors.set(loaded.name, loaded.error)
        } else {
          const prev = errors.get(loaded.name)
          const note = `[${loaded.error.domain}] 覆盖版加载失败，已保留先前版本：${loaded.error.error}`
          errors.set(loaded.name, { ...loaded.error, error: prev ? `${prev.error}\n${note}` : note })
        }
        continue
      }
      if (domain === "custom" && sources.has(loaded.def.name)) {
        // 二开覆盖：后写胜出（与子 Agent 同语义）
        errors.delete(loaded.def.name)
      }
      sources.set(loaded.def.name, loaded.def)
    }
  }
  return { sources, errors, signature }
}

type OneLoaded =
  | { def: EventSourceDef }
  | { error: EventSourceLoadError; name: string }

/**
 * 这个 default 导出看起来像人格定义吗（而不是事件源）。
 *
 * 用途：`custom/persona/` 两类文件混放，靠导出名区分。若一个文件 `export default` 的是
 * 人格定义（有 identity/role 但没有 channel/start），事件源扫描要**安静跳过**它——
 * 报「未导出 source」只会制造噪声，并把真正的加载错误淹没。
 */
function isPersonaExport(v: unknown): boolean {
  if (!v || typeof v !== "object") return false
  const o = v as Record<string, unknown>
  const looksPersona = typeof o.identity === "string" || typeof o.role === "string"
  const looksSource = typeof o.channel === "string" || typeof o.start === "function"
  return looksPersona && !looksSource
}

/** 加载单个定义（文件或目录形态）。 */
async function loadOne(dir: string, entry: { name: string; isDirectory(): boolean; isFile(): boolean }, domain: "builtin" | "custom"): Promise<OneLoaded | null> {
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
    if (!filePath) return null // 纯资源目录，跳过
  } else {
    return null
  }

  try {
    // mtime 查询参数绕过模块缓存（Bun 对 file:// URL 的查询参数不生效，故用绝对路径 + query）
    const mtime = (await stat(filePath).catch(() => null))?.mtimeMs ?? 0
    const mod = (await import(`${filePath}?t=${mtime}`)) as { source?: unknown; persona?: unknown; default?: unknown }
    // **同名目录共用的靠写**：`custom/persona/` 同时放人格定义与事件源（同一个二开域，
    // 靠导出名区分）。发现人格时会把事件源文件当成“无 persona 导出”报错，反之亦然——
    // 两边都报就是持续噪声，且会把真实错误淹掉。所以这里先看**另一个导出**在不在：
    // 在就说明这个文件属于另一类，安静跳过（不是本类的源）。
    if (mod.source === undefined && (mod.persona !== undefined || isPersonaExport(mod.default))) {
      return null
    }
    const raw = mod.source ?? (isPersonaExport(mod.default) ? undefined : mod.default)
    if (!raw) {
      return {
        name: base,
        error: {
          name: base,
          domain,
          error: `${filePath} 未导出 source（须 \`export const source: EventSourceDef = {…}\`，或 default 导出）`,
        },
      }
    }
    const invalid = validateSourceDef(raw, base)
    if (invalid) return { name: base, error: { name: base, domain, error: invalid } }
    const def = raw as EventSourceDef
    if (def.name !== base) {
      return {
        name: base,
        error: { name: base, domain, error: `source.name「${def.name}」与文件名「${base}」不一致（防止改名后配置失配）` },
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
const CHANNEL_RE = /^[a-z0-9_]+$/

/**
 * 定义校验（与 `persona/profile` 同姿态：**字段错了就报错，不静默降级**）。
 *
 * 静默降级的具体危害：`channel` 拼错 → 路由永远匹配不上 → 该源收到的事件全部
 * `unattended` 被丢弃，而日志里没有任何异常，表现为「配了但没反应」。
 */
export function validateSourceDef(raw: unknown, expectedName: string): string | null {
  if (!raw || typeof raw !== "object") return "source 必须是对象"
  const o = raw as Record<string, unknown>
  const allowed = new Set(["name", "description", "channel", "envVars", "start", "stop"])
  for (const k of Object.keys(o)) {
    if (!allowed.has(k)) return `含未知字段「${k}」（可用：${[...allowed].join(" / ")}）`
  }
  if (typeof o.name !== "string" || !NAME_RE.test(o.name)) return `name 必须是 [a-z0-9_]+（当前：${String(o.name)}）`
  if (o.name !== expectedName) return `name「${o.name}」与文件名「${expectedName}」不一致`
  if (typeof o.description !== "string" || !o.description.trim()) return "description 必须是非空字符串"
  if (typeof o.channel !== "string" || !CHANNEL_RE.test(o.channel)) return `channel 必须是 [a-z0-9_]+（当前：${String(o.channel)}）`
  if (typeof o.start !== "function") return "start 必须是函数（async start(ctx)）"
  if (o.stop !== undefined && typeof o.stop !== "function") return "stop 若提供必须是函数"
  if (o.envVars !== undefined) {
    if (!Array.isArray(o.envVars)) return "envVars 必须是数组"
    for (const v of o.envVars) {
      if (!v || typeof v !== "object") return "envVars 每项必须是对象"
      const item = v as Record<string, unknown>
      if (typeof item.name !== "string" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(item.name)) return `envVars.name 非法：${String(item.name)}`
      if (typeof item.description !== "string") return `envVars「${item.name}」缺少 description`
    }
  }
  return null
}

/** 读取定义文件原文（诊断/面板展示用；失败返回 null 而不是抛错）。 */
export async function readSourceFile(dir: string, name: string): Promise<string | null> {
  for (const candidate of [join(dir, `${name}.ts`), join(dir, name, `${name}.ts`), join(dir, name, "index.ts")]) {
    const text = await readFile(candidate, "utf8").catch(() => null)
    if (text !== null) return text
  }
  return null
}
