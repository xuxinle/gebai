/**
 * 行为参数：**人格共性写代码、使用者差异写 env**，且逐分身可不同。
 *
 * 这四个参数都是「这个分身怎么干活」的一部分，与 `askTools` / `reviewOutbound`
 * 同一性质，所以放在同一个地方（`GEBAI_PERSONA_<ID>` 数组项），而不是各占一个
 * 全局环境变量。全局变量只能表达“所有分身都一样”，而真实诉求往往是
 * “值班分身要让位 60 秒，日报分身不必”。
 *
 * ## 取值顺序
 *
 * 分身显式值 > 人格级默认（数组里不带 name 的那项）> 代码缺省
 *
 * 刻意**保留全局变量作最后兜底**（`GEBAI_PERSONA_*`）：已有部署不必为了升级改配置。
 * 但逐分身声明优先——否则“全局开让位、某个分身不要”就无法表达。
 */

import type { DriverEnvItem } from "./persona-config"

/** 四个行为参数（全部有安全缺省，不配即用缺省）。 */
export interface BehaviorParams {
  /** 是否回复用户本人发的消息（缺省 true）。 */
  replyToOwner: boolean
  /** 让位窗口秒数（缺省 180；0 = 关闭让位）。 */
  yieldSeconds: number
  /** 用户说话时的硬打断（缺省 false）。 */
  humanInterrupt: boolean
  /** 会话路由（缺省 driver）。 */
  sessionRouting: "driver" | "chat"
}

/** 代码缺省（与历史全局变量的缺省完全一致，升级不改变现有行为）。 */
export const BEHAVIOR_DEFAULTS: BehaviorParams = {
  replyToOwner: true,
  yieldSeconds: 180,
  humanInterrupt: false,
  sessionRouting: "driver",
}

type Env = Record<string, string | undefined>

const asBool = (raw: string | undefined): boolean | undefined => {
  const v = (raw ?? "").trim().toLowerCase()
  if (!v) return undefined
  return !/^(0|false|off|no)$/i.test(v)
}

const asSeconds = (raw: string | undefined): number | undefined => {
  const v = (raw ?? "").trim()
  if (!v) return undefined
  const n = Number(v)
  // 非法值当作“没配”：宁可回到缺省，也不要让一个笔误把让位窗口变成 NaN
  return Number.isFinite(n) && n >= 0 ? n : undefined
}

/**
 * 解析行为参数：逐分身 > 人格级默认 > 全局变量 > 代码缺省。
 *
 * `personaId` 为空表示“只读全局与代码缺省”（启动期打印总览时用）。
 */
export function behaviorParamsFor(
  env: Env,
  personaId: string,
  driverId?: string,
  /** 已解析出来的分身配置（有就用，省一次 JSON 解析）。 */
  items?: { defaults: Omit<DriverEnvItem, "name"> | null; own: DriverEnvItem | undefined },
): BehaviorParams {
  // ① 逐分身配置（数组项）里声明的值优先
  const fromItem = items ? mergeItems(items.defaults, items.own) : undefined

  // ② 全局变量兜底（兼容既有部署）
  const fromEnv: Partial<BehaviorParams> = {}
  const r = asBool(env.GEBAI_PERSONA_REPLY_TO_OWNER)
  if (r !== undefined) fromEnv.replyToOwner = r
  const y = asSeconds(env.GEBAI_PERSONA_YIELD_SECONDS)
  if (y !== undefined) fromEnv.yieldSeconds = y
  const h = asBool(env.GEBAI_PERSONA_HUMAN_INTERRUPT)
  if (h !== undefined) fromEnv.humanInterrupt = h
  const sr = (env.GEBAI_PERSONA_SESSION_ROUTING ?? "").trim().toLowerCase()
  if (sr === "chat" || sr === "driver") fromEnv.sessionRouting = sr

  void personaId
  void driverId

  return {
    replyToOwner: fromItem?.replyToOwner ?? fromEnv.replyToOwner ?? BEHAVIOR_DEFAULTS.replyToOwner,
    yieldSeconds: fromItem?.yieldSeconds ?? fromEnv.yieldSeconds ?? BEHAVIOR_DEFAULTS.yieldSeconds,
    humanInterrupt: fromItem?.humanInterrupt ?? fromEnv.humanInterrupt ?? BEHAVIOR_DEFAULTS.humanInterrupt,
    sessionRouting: fromItem?.sessionRouting ?? fromEnv.sessionRouting ?? BEHAVIOR_DEFAULTS.sessionRouting,
  }
}

/** 人格级默认 + 分身覆盖（只取行为参数，逐字段判 undefined）。 */
function mergeItems(
  defaults: Omit<DriverEnvItem, "name"> | null,
  own: DriverEnvItem | undefined,
): Partial<BehaviorParams> | undefined {
  const d = defaults ?? {}
  const o = own ?? ({} as DriverEnvItem)
  const out: Partial<BehaviorParams> = {}
  const pick = <K extends keyof BehaviorParams>(k: K): BehaviorParams[K] | undefined =>
    (o[k] as BehaviorParams[K] | undefined) ?? (d[k] as BehaviorParams[K] | undefined)
  const r = pick("replyToOwner")
  if (r !== undefined) out.replyToOwner = r
  const y = pick("yieldSeconds")
  if (y !== undefined) out.yieldSeconds = y
  const h = pick("humanInterrupt")
  if (h !== undefined) out.humanInterrupt = h
  const s = pick("sessionRouting")
  if (s !== undefined) out.sessionRouting = s
  return Object.keys(out).length ? out : undefined
}
