/**
 * 人格定义（角色）与发现：**与子 Agent / 事件源 / 渠道同款「丢文件即注册」**。
 *
 * ## 为什么人格也用「代码式定义文件」
 *
 * 人格是**长期稳定的部署事实**（「小霜是运维助手，她盯告警和答疑」），不是运行时
 * 注入的临时状态。它需要：
 *
 * - **可 diff**：谁改了人格的身份、加了哪些 driver，git 里看得见
 * - **可测试**：人格配置错了应该在测试里炸，而不是上线后「AI 说话怪怪的」
 * - **可迁移**：`custom/persona/` 整个拷到新仓库即完成迁移
 *
 * 这三条正是「代码即一切」不变式的理由。后续若发现「非工程师也要改人格」的强需求，
 * 再加配置式（读取侧统一走 `PersonaLookup`，换实现即可，调用方零改动）。
 *
 * ## 文件形态
 *
 * ```ts
 * // custom/persona/xiaoshuang.ts
 * export const persona: PersonaDef = {
 *   id: "xiaoshuang",
 *   name: "小霜",
 *   identity: "你是小霜，训练平台的值班工程师……",
 *   role: "运维",
 *   drivers: [
 *     { id: "support", focus: "答支持群的问题", subscriptions: ["feishu:oc_support", "message"] },
 *     { id: "alerts",  focus: "盯告警",         subscriptions: ["webhook:alert", "schedule"] },
 *   ],
 * }
 * ```
 *
 * 扫描域：内置 `packages/persona/src/personas/` + 二开 `custom/persona/`（同名二开胜出）。
 *
 * ## 与 driver 的关系
 *
 * `drivers` 是**静态定义**（权威）；`DriverRegistry` 负责运行期**动态创建**的 driver
 * （模型自己想出来的分身）。两者在 `PersonaLookup.drivers()` 处合流：
 * 静态在前、动态在后。手写配置永远优先于模型自建。
 */

import { readdir, readFile, stat } from "node:fs/promises"
import { join } from "node:path"
import type { DriverDef, PersonaIdentity } from "./ports"

/**
 * 人格定义：`PersonaIdentity` + 它下面的静态 driver 清单。
 *
 * 为什么不是 `PersonaIdentity & { drivers }` 的交叉类型：交叉类型会让
 * `PersonaLookup.get()`（只要身份）与这里的字段纠缠——分开写更清楚，
 * 且 `identity` / `role` 是**原样进系统提示词**的文本，不该有隐式来源。
 */
export interface PersonaDef extends PersonaIdentity {
  /** 该人格下的静态 driver（缺省空：只有身份，还没有分工）。 */
  drivers?: DriverDef[]
  /** 一句话说明（面板与诊断可见；不进提示词）。 */
  description?: string
  /** 本定义需要的环境变量（声明式白名单，与渠道/事件源同口径）。 */
  envVars?: Array<{ name: string; description: string; secret?: boolean }>
  /**
   * 这个人格的会话默认装载哪些**子代理工具包**（如 `["cefu"]`）。
   *
   * ## 为何是声明式而不是默认全装
   *
   * 装载一份工具包 = 多一层工具面（每个工具的名称+描述+schema 都要进上下文）。
   * 默认全装等于“不告诉部署方就改了他的人格行为与 token 开销”，那是悄悄改契约。
   * 而写在这里的好处是：**人格需要什么能力是人格定义的一部分**，与 `drivers`、
   * `envVars` 同一层语义，看得见、可审计。
   *
   * ## 写进提示词的那一半
   *
   * 光有工具不够——模型面对一排工具常常“不知道什么时候该用”（工具在表里但不会被想起来）。
   * 所以声明了 `agents` 的人格，提示词里会多一段「记忆纪律」，告诉它何时查、何时写。
   * 两半合起来才叫“有了记忆”；只装工具不告诉它，等于给了本子但不说要记。
   *
   * 名字必须是已注册的子代理名（与 `custom/agents/` 目录名、内置子代理名一致）；
   * 写错不会静默失效，会在装载时如实报错（一个装不上的工具包等于不存在，最难查）。
   */
  agents?: string[]
}

/** 扫描域（内置 + 二开）。 */
export interface PersonaScanDirs {
  builtin: string
  custom: string
}

/**
 * 默认扫描域。
 *
 * 二开域刻意与事件源共用 `custom/persona/`：两者都是「拟人模式的二开物」，
 * 放同一个目录更符合使用者心智（不必记「人格放 A、事件源放 B」）。
 * 文件名不冲突即可（人格导出 `persona`，事件源导出 `source`）。
 */
export function defaultPersonaScanDirs(opts: { packageRoot: string; repoRoot?: string }): PersonaScanDirs {
  const repoRoot = opts.repoRoot ?? join(opts.packageRoot, "..", "..")
  return {
    builtin: join(opts.packageRoot, "src", "personas"),
    custom: join(repoRoot, "custom", "persona"),
  }
}

function isDefFile(name: string): boolean {
  if (!/\.(ts|js)$/.test(name)) return false
  if (name.endsWith(".test.ts") || name.endsWith(".test.js")) return false
  if (name.endsWith(".d.ts")) return false
  if (name.startsWith("_")) return false
  return true
}

const ID_RE = /^[a-z0-9_]+$/

/**
 * 人格定义校验。
 *
 * **严格**（未知字段即拒绝）的理由：人格定义决定「AI 是谁、盯什么」，
 * 一个拼错的 `driver` 字段（如 `subcription`）会让订阅静默失效——
 * 表现是「配了但从来不响应」，而日志里没有任何异常。宁可在加载期报出来。
 */
export function validatePersonaDef(raw: unknown, expectedId: string): string | null {
  if (!raw || typeof raw !== "object") return "persona 必须是对象"
  const o = raw as Record<string, unknown>
  const allowed = new Set(["id", "name", "identity", "role", "drivers", "description", "envVars", "agents"])
  for (const k of Object.keys(o)) {
    if (!allowed.has(k)) return `含未知字段「${k}」（可用：${[...allowed].join(" / ")}）`
  }
  if (typeof o.id !== "string" || !ID_RE.test(o.id)) return `id 必须是 [a-z0-9_]+（当前：${String(o.id)}）`
  if (o.id !== expectedId) return `id「${o.id}」与文件名「${expectedId}」不一致（防止改名后订阅/会话映射失配）`
  for (const k of ["name", "identity", "role"] as const) {
    if (typeof o[k] !== "string" || !(o[k] as string).trim()) return `${k} 必须是非空字符串`
  }
  if (o.description !== undefined && typeof o.description !== "string") return "description 若是提供必须是字符串"
  if (o.drivers !== undefined) {
    if (!Array.isArray(o.drivers)) return "drivers 必须是数组"
    const seen = new Set<string>()
    for (const d of o.drivers) {
      if (!d || typeof d !== "object") return "drivers 每项必须是对象"
      const dr = d as Record<string, unknown>
      const dAllowed = new Set(["id", "focus", "subscriptions", "maxConcurrent"])
      for (const k of Object.keys(dr)) {
        if (!dAllowed.has(k)) return `drivers 项含未知字段「${k}」（可用：${[...dAllowed].join(" / ")}）`
      }
      if (typeof dr.id !== "string" || !ID_RE.test(dr.id)) return `drivers.id 必须是 [a-z0-9_]+（当前：${String(dr.id)}）`
      if (seen.has(dr.id)) return `drivers.id「${dr.id}」重复（同一人格内须唯一）`
      seen.add(dr.id)
      if (typeof dr.focus !== "string" || !dr.focus.trim()) return `drivers「${dr.id}」缺少 focus（这个分身在关注什么）`
      if (!Array.isArray(dr.subscriptions) || dr.subscriptions.some((s) => typeof s !== "string" || !s.trim())) {
        return `drivers「${dr.id}」的 subscriptions 必须是非空字符串数组`
      }
      if (dr.maxConcurrent !== undefined && (typeof dr.maxConcurrent !== "number" || dr.maxConcurrent < 1)) {
        return `drivers「${dr.id}」的 maxConcurrent 必须是 >=1 的数字`
      }
    }
  }
  if (o.envVars !== undefined) {
    if (!Array.isArray(o.envVars)) return "envVars 必须是数组"
    for (const v of o.envVars) {
      if (!v || typeof v !== "object") return "envVars 每项必须是对象"
      const item = v as Record<string, unknown>
      if (typeof item.name !== "string" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(item.name)) return `envVars.name 非法：${String(item.name)}`
      if (typeof item.description !== "string") return `envVars「${item.name}」缺少 description`
    }
  }
  if (o.agents !== undefined) {
    if (!Array.isArray(o.agents)) return "agents 必须是数组"
    const seenAgents = new Set<string>()
    for (const a of o.agents) {
      if (typeof a !== "string" || !a.trim()) return "agents 每项必须是非空字符串"
      // 子代理名与事件源的 name 同口径（[a-z0-9_]+）：写中文/大写/短横线会在装载时失败，
      // 而“装不上的工具包”表现为“模型没这个能力”，最难查——所以在这里就拦住。
      if (!ID_RE.test(a)) return `agents「${a}」必须是 [a-z0-9_]+（与子代理目录名一致）`
      if (seenAgents.has(a)) return `agents「${a}」重复`
      seenAgents.add(a)
    }
  }
  return null
}

/** 加载错误。 */
export interface PersonaLoadError {
  id: string
  domain: "builtin" | "custom"
  error: string
}

/** 发现结果。 */
export interface PersonaDiscoveryResult {
  personas: Map<string, PersonaDef>
  errors: Map<string, PersonaLoadError>
  signature: string
}

/** 目录签名（热加载判定；与子 Agent / 事件源 / 渠道同算法）。 */
async function dirSignature(dir: string): Promise<string | null> {
  let entries
  try {
    entries = await readdir(dir, { withFileTypes: true })
  } catch {
    return null
  }
  const parts: string[] = []
  for (const e of entries) {
    if (e.isDirectory()) {
      if (!ID_RE.test(e.name)) continue
      for (const c of [`${e.name}.ts`, "index.ts"]) {
        const st = await stat(join(dir, e.name, c)).catch(() => null)
        if (st?.isFile()) parts.push(`${e.name}/${c}:${st.mtimeMs}`)
      }
    } else if (e.isFile() && isDefFile(e.name)) {
      const base = e.name.replace(/\.(ts|js)$/, "")
      if (!ID_RE.test(base)) continue
      const st = await stat(join(dir, e.name)).catch(() => null)
      if (st) parts.push(`${e.name}:${st.mtimeMs}`)
    }
  }
  parts.sort()
  return parts.join("|")
}

export async function discoverySignature(dirs: PersonaScanDirs): Promise<string> {
  const builtin = (await dirSignature(dirs.builtin)) ?? ""
  const custom = (await dirSignature(dirs.custom)) ?? ""
  return `builtin:${builtin}|custom:${custom}`
}

type OneLoaded = { def: PersonaDef } | { error: PersonaLoadError; id: string }

/**
 * 这个 default 导出看起来像事件源吗（而不是人格定义）。
 *
 * 与 `discovery.ts` 的 `isPersonaExport` 对称：`custom/persona/` 两类文件混放，
 * 靠导出名区分；先认清“另一个类”，才能安静跳过而不互相误报。
 */
function isSourceExportForPersona(v: unknown): boolean {
  if (!v || typeof v !== "object") return false
  const o = v as Record<string, unknown>
  const looksSource = typeof o.channel === "string" || typeof o.start === "function"
  const looksPersona = typeof o.identity === "string" || typeof o.role === "string"
  return looksSource && !looksPersona
}

async function loadOne(dir: string, entry: { name: string; isDirectory(): boolean; isFile(): boolean }, domain: "builtin" | "custom"): Promise<OneLoaded | null> {
  let base: string
  let filePath: string | null = null
  if (entry.isFile()) {
    if (!isDefFile(entry.name)) return null
    base = entry.name.replace(/\.(ts|js)$/, "")
    if (!ID_RE.test(base)) return null
    filePath = join(dir, entry.name)
  } else if (entry.isDirectory()) {
    base = entry.name
    if (!ID_RE.test(base)) return null
    for (const c of [`${base}.ts`, "index.ts", `${base}.js`, "index.js"]) {
      const p = join(dir, base, c)
      const st = await stat(p).catch(() => null)
      if (st?.isFile()) {
        filePath = p
        break
      }
    }
    if (!filePath) return null
  } else {
    return null
  }

  try {
    const mtime = (await stat(filePath).catch(() => null))?.mtimeMs ?? 0
    const mod = (await import(`${filePath}?t=${mtime}`)) as { persona?: unknown; source?: unknown; default?: unknown }
    // **同名目录共用的靠写**（与 discovery.ts 的 loadOne 对称）：`custom/persona/` 同时放
    // 人格定义与事件源。先看 source 在不在：在就说明这是事件源文件，不是人格——
    // 安静跳过，避免“两边互相报对方格式不对”的持续噪声。
    if (mod.persona === undefined && (mod.source !== undefined || isSourceExportForPersona(mod.default))) {
      return null
    }
    const raw = mod.persona ?? (isSourceExportForPersona(mod.default) ? undefined : mod.default)
    if (!raw) {
      return { id: base, error: { id: base, domain, error: `${filePath} 未导出 persona（须 \`export const persona: PersonaDef = {…}\`，或 default 导出）` } }
    }
    const invalid = validatePersonaDef(raw, base)
    if (invalid) return { id: base, error: { id: base, domain, error: invalid } }
    return { def: raw as PersonaDef }
  } catch (err) {
    return { id: base, error: { id: base, domain, error: `加载 ${filePath} 失败: ${String((err as Error)?.message || err)}` } }
  }
}

/**
 * 发现全部人格（双域扫描，custom 后扫同名覆盖）。
 *
 * 失败隔离：单个人格定义坏掉只记 `errors`，其余照常注册——
 * 「小霜的人格文件写错了」不该让「老张也不工作了」。
 * 二开覆盖版写坏时**保留旧可用版本**（与渠道/事件源同语义）。
 */
export async function discoverPersonas(dirs: PersonaScanDirs): Promise<PersonaDiscoveryResult> {
  const personas = new Map<string, PersonaDef>()
  const errors = new Map<string, PersonaLoadError>()
  const signature = await discoverySignature(dirs)

  for (const [domain, dir] of [
    ["builtin", dirs.builtin],
    ["custom", dirs.custom],
  ] as const) {
    let entries
    try {
      entries = await readdir(dir, { withFileTypes: true })
    } catch {
      continue // 域不存在：正常
    }
    entries.sort((a, b) => a.name.localeCompare(b.name))
    for (const e of entries) {
      const loaded = await loadOne(dir, e, domain)
      if (!loaded) continue
      if ("error" in loaded) {
        if (!personas.has(loaded.id)) {
          errors.set(loaded.id, loaded.error)
        } else {
          const prev = errors.get(loaded.id)
          const note = `[${loaded.error.domain}] 覆盖版加载失败，已保留先前版本：${loaded.error.error}`
          errors.set(loaded.id, { ...loaded.error, error: prev ? `${prev.error}\n${note}` : note })
        }
        continue
      }
      if (domain === "custom" && personas.has(loaded.def.id)) errors.delete(loaded.def.id)
      personas.set(loaded.def.id, loaded.def)
    }
  }
  return { personas, errors, signature }
}

/** 读取定义文件原文（诊断用；失败返回 null 而不抛错）。 */
export async function readPersonaFile(dirs: PersonaScanDirs, id: string): Promise<string | null> {
  for (const dir of [dirs.custom, dirs.builtin]) {
    for (const c of [join(dir, `${id}.ts`), join(dir, id, `${id}.ts`), join(dir, id, "index.ts")]) {
      const text = await readFile(c, "utf8").catch(() => null)
      if (text !== null) return text
    }
  }
  return null
}
