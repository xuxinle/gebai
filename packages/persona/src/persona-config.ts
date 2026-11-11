/**
 * 人格的声明式配置：`GEBAI_PERSONA_<ID>` = JSON 数组，**每项一个分身**。
 *
 * ## 为什么从「拼变量名」改成「数组里写着」
 *
 * 旧形态把「人格 + 分身 + 属性」三段拼进变量名：
 *
 * ```dotenv
 * GEBAI_PERSONA_XIAOSHUANG_SUPPORT_CHATS=oc_zzz
 * GEBAI_PERSONA_XIAOSHUANG_SUPPORT_OUTPUT=feishu:oc_zzz=支持群
 * GEBAI_PERSONA_XIAOSHUANG_SUPPORT_SESSION_MAP=per_thread
 * ```
 *
 * 三个后果，都在实际部署里踩过：
 *
 * 1. **下划线歧义**：人格 id 与分身 id 都可能含 `_`，`GEBAI_PERSONA_FIN_OPS_ALERT_CHATS`
 *    到底是「人格 fin_ops 的分身 alert」还是「人格 fin_ops_alert」？只能靠约定猜，
 *    且猜错不报错——这条注释在旧实现里就写着「是真实踩过的坑」。
 * 2. **一个分身散成 4 个变量**：看着不觉得它们是同一件事，改一处漏一处。
 * 3. **表达不了「引用通道实例」**：想指定「用 feishu_main 这个通道实例发」，
 *    旧形态只能写 `channel:address`，实例名无处安放。
 *
 * 新形态：
 *
 * ```dotenv
 * GEBAI_PERSONA_XIAOSHUANG=[
 *   {"name":"support","channels":["feishu_main"],"chats":["oc_zzz"],
 *    "sessionMap":"per_thread","outbound":"shape","signature":"——小霜"}
 * ]
 * ```
 *
 * ## 与代码定义的关系
 *
 * 代码里的人格定义（`custom/persona/xiaoshuang.ts`）仍然是**权威**：它写着
 * id / 身份 / 角色 / 分身的 focus 与订阅。环境变量只表达**部署差异**——
 * 哪个群、发到哪、怎么分割上下文。这与改造前的分工完全一致，只是搬到了数组里。
 */

import type { DriverDef, OutputTarget } from "./ports"
import type { SessionMapRule } from "./session-map"

/**
 * 一个方向（入站或出站）的引用：引哪些渠道实例、落在哪些群。
 *
 * 为何拆成两个子对象：同一人格经常是「收这个群、发那个群」，
 * 甚至「用 A 机器人收、用 B 机器人发」。把它们塞进同一组 `channels`/`chats`
 * 就只能“同进同出”——要表达分离必须再起一套键，不如一开始就分开。
 */
export interface DirRef {
  /** 引用哪些渠道实例（`GEBAI_CHANNEL_<DIR>_<TYPE>` 里配置的 `name`）。 */
  channels?: string[]
  /** 哪些群（in：收哪些群；out：只能发到哪些群）。 */
  chats?: string[]
}

/** 一个分身的部署配置（JSON 数组的一项）。 */
export interface DriverEnvItem {
  /** 分身 id：必须与代码定义里的 `drivers[].id` 对应。 */
  name: string
  /**
   * 入站：收哪些渠道实例的哪些群（驱动 → 会话）。
   *
   * 不写 = 听代码定义里的订阅面（通常是“该渠道全部群”）。
   */
  listen?: DirRef
  /**
   * 出站：能发到哪些实例的哪些群。
   *
   * 不写 = 没有显式输出目标（只能回事件来源）。
   */
  send?: DirRef
  /**
   * 旧形态：同时作用于入站与出站（“同进同出”的简写）。
   *
   * @deprecated 改用 `listen` / `send`。保留原因：已有部署不必为了新能力改配置。
   * 优先级：listen/send 写了就用自己的，没写才回落到这里。
   */
  channels?: string[]
  /** @deprecated 同 `channels`，回落到 listen/send 的 chats。 */
  chats?: string[]
  /** 会话映射（预设名 / DSL / JSON）。 */
  sessionMap?: string
  /** 出站加工档位：raw / shape / polish。 */
  outbound?: string
  /** 出站署名。 */
  signature?: string
  /**
   * 对外输出是否要人审核（缺省 true）。
   *
   * 管的是**发出去之前要不要弹卡批准**，与 `outbound`（发出去之前怎么加工）是两件事。
   * 为何放进这个数组而不是单开一个环境变量：它是“这个人格发消息时怎么做事”，
   * 与 chats/outbound 同属一个分身的行为参数。放全局会导致：想给 A 人格关审核、
   * B 人格留审核，只能建两个部署。缺省仍是 true（安全的那一侧）。
   */
  reviewOutbound?: boolean
  /** 并发额度。 */
  maxConcurrent?: number
  /**
   * 这个分身**要弹窗问人的工具**（逗号分隔字符串或数组；支持尾部通配 `code_*` 与 `*`）。
   *
   * 不写 = 该分身工具全部不审批（缺省）；写空串/空数组 = 显式表示“什么都不问”
   * （与不写的效果相同，但能覆盖人格级默认）。
   *
   * 为何放在分身配置里而不是一个全局变量：不同分身干的事不一样——
   * 盯告警的分身只需要读日志（全不必问），而执行运维动作的分身应该把 `sh` 拦住。
   * 全局名单只能取最保守的交集，结果是所有人一起被卡。
   */
  askTools?: string | string[]
  /**
   * 定时任务的**运行期覆盖**（按声明 id）。
   *
   * 为何需要它：cron 表达式是“部署方想几点跑”的差异（早报 8 点还是 9 点），
   * 而且不同环境（开发/生产）往往不同；写死在代码里就只能改代码。
   * 不写 = 用代码定义里的 `schedules[].cron`（缺省行为完全不变）。
   *
   * 只覆盖 **cron**（必要时带 prompt/timezone）：声明本身（id、是否存在）
   * 仍由代码定义决定——否则“配错了 id”会变成一个凭空多出来的定时任务，
   * 而不是一条报错。
   */
  schedules?: Record<string, ScheduleOverride>
  /**
   * 是否回复**用户本人**发的消息（缺省 true）。
   *
   * false 时：用户自己说的话只入上下文、不触发回复（防“自言自语”）；
   * 别人说的话照常回。
   */
  replyToOwner?: boolean
  /**
   * 让位窗口秒数（缺省 180）：用户刚说过话后这么久内，分身**不主动插话**。
   *
   * 0 = 关闭让位（它随时可以插）；与“回不回复”是两件事——回用户的话不受它限制。
   */
  yieldSeconds?: number
  /**
   * 用户说话时是否**硬打断**正在跑的一轮（缺省 false）。
   *
   * 仅适合坐席类场景（必须马上响应新指令）；开着会让长任务被一句话打断。
   */
  humanInterrupt?: boolean
  /**
   * 会话路由：`driver`（缺省，一个分身一个上下文）/ `chat`（按群×话题分会话）。
   *
   * 为何缺省 `driver`：把群当会话身份会把“一个人”拆成几份互不知情的助手。
   * 真正的分割应来自**上下文**，而不是消息从哪里来。
   */
  sessionRouting?: "driver" | "chat"
}

/** 一条定时任务的覆盖值（只认这几个键，拼错即报错）。 */
export interface ScheduleOverride {
  /** 执行表达式（5 段 cron 或 `@daily` / `@every 30m` / `@at ...`）。 */
  cron?: string
  /** 到点后的指令（不写就用代码里的 prompt）。 */
  prompt?: string
  /** IANA 时区（如 `Asia/Shanghai`）。 */
  timezone?: string
  /** 是否启用这条声明（false = 不触发，但保留定义）。 */
  enabled?: boolean
}

export interface ParsedPersonaEnv {
  /** 人格级默认（数组里没有 name、或 name 为空的那一项）。 */
  personaDefaults: Omit<DriverEnvItem, "name"> | null
  /** 分身级配置（按 name）。 */
  drivers: Map<string, DriverEnvItem>
  errors: string[]
}

const NAME_RE = /^[a-z0-9_]+$/
/** 定时声明 id 规则（与 `DriverSchedule.id` 一致）。 */
const SCHEDULE_ID_RE = /^[a-z0-9_]+$/

/**
 * 解析 `GEBAI_PERSONA_<ID>` 的 JSON。
 *
 * 两种项形态（同一数组里可以只有其中一种）：
 * - **带 name**：该名字的分身配置
 * - **不带 name**：人格级默认（分身的缺省值），最多一项
 *
 * 为什么不强制每项都有 name：单分身的人格不必为此多写一层嵌套；
 * 而且「人格级默认 + 个别分身覆盖」这个两级结构在旧形态里就是存在的
 * （`<ID>_CHATS` 与 `<ID>_<DRIVER>_CHATS`），强行抹平会丢掉那层语义。
 */
export function parsePersonaEnv(personaId: string, raw: string | undefined): ParsedPersonaEnv {
  const errors: string[] = []
  const varName = `GEBAI_PERSONA_${personaId.toUpperCase()}`
  const out: ParsedPersonaEnv = { personaDefaults: null, drivers: new Map(), errors }
  const text = (raw ?? "").trim()
  if (!text) return out

  let parsed: unknown
  try {
    parsed = JSON.parse(text)
  } catch (err) {
    errors.push(`${varName} 不是合法 JSON：${(err as Error).message}`)
    return out
  }
  if (!Array.isArray(parsed)) {
    errors.push(`${varName} 必须是 JSON 数组（每项一个分身，如 [{"name":"support","channels":["feishu_main"]}]）`)
    return out
  }

  parsed.forEach((item, i) => {
    const at = `${varName}[${i}]`
    if (!item || typeof item !== "object" || Array.isArray(item)) {
      errors.push(`${at} 必须是对象`)
      return
    }
    const o = item as Record<string, unknown>

    // 未知键 → 报错（拼错的 `chat` 若不报错，就是“配了但没生效”的典型）
    const KNOWN = new Set([
      "name",
      "listen",
      "send",
      "channels",
      "chats",
      "sessionMap",
      "outbound",
      "signature",
      "reviewOutbound",
      "maxConcurrent",
      "askTools",
      "schedules",
      "replyToOwner",
      "yieldSeconds",
      "humanInterrupt",
      "sessionRouting",
    ])
    for (const k of Object.keys(o)) {
      if (!KNOWN.has(k)) {
        errors.push(`${at} 含未知键「${k}」（可用：${[...KNOWN].join(" / ")}）`)
      }
    }

    const name = typeof o.name === "string" ? o.name.trim() : ""
    if (name && !NAME_RE.test(name)) {
      errors.push(`${at} 的 name「${name}」非法（分身 id 须 [a-z0-9_]+，与代码定义一致）`)
      return
    }

    const item2: DriverEnvItem = { name }
    if (o.channels !== undefined) {
      const v = stringArray(o.channels)
      if (typeof v === "string") errors.push(`${at} 的 channels ${v}`)
      else item2.channels = v
    }
    if (o.chats !== undefined) {
      const v = stringArray(o.chats)
      if (typeof v === "string") errors.push(`${at} 的 chats ${v}`)
      else item2.chats = v
    }
    // listen / send：两个方向的引用（各自可含 channels 与 chats）
    for (const dir of ["listen", "send"] as const) {
      const raw = o[dir]
      if (raw === undefined) continue
      if (!raw || typeof raw !== "object" || Array.isArray(raw)) {
        errors.push(`${at} 的 ${dir} 必须是对象（如 {"channels":["feishu_in"],"chats":["oc_x"]}）`)
        continue
      }
      const r = raw as Record<string, unknown>
      const DIR_KEYS = new Set(["channels", "chats"])
      for (const k of Object.keys(r)) {
        if (!DIR_KEYS.has(k)) errors.push(`${at}.${dir} 含未知键「${k}」（可用：channels / chats）`)
      }
      const out: DirRef = {}
      for (const k of ["channels", "chats"] as const) {
        if (r[k] === undefined) continue
        const v = stringArray(r[k])
        if (typeof v === "string") errors.push(`${at}.${dir} 的 ${k} ${v}`)
        else out[k] = v
      }
      item2[dir] = out
    }
    for (const k of ["sessionMap", "outbound", "signature"] as const) {
      if (o[k] === undefined) continue
      if (typeof o[k] !== "string") {
        errors.push(`${at} 的 ${k} 必须是字符串`)
        continue
      }
      item2[k] = o[k] as string
    }
    if (o.reviewOutbound !== undefined) {
      if (typeof o.reviewOutbound !== "boolean") {
        errors.push(`${at} 的 reviewOutbound 必须是布尔值（true/false）`)
      } else {
        item2.reviewOutbound = o.reviewOutbound
      }
    }
    if (o.maxConcurrent !== undefined) {
      if (typeof o.maxConcurrent !== "number" || !Number.isFinite(o.maxConcurrent) || o.maxConcurrent < 1) {
        errors.push(`${at} 的 maxConcurrent 必须是 ≥1 的数字`)
      } else {
        item2.maxConcurrent = o.maxConcurrent
      }
    }
    // askTools：字符串或数组都收（写字符串更接近环境变量的手感）
    if (o.askTools !== undefined) {
      if (typeof o.askTools === "string") {
        item2.askTools = o.askTools
      } else if (Array.isArray(o.askTools)) {
        if (o.askTools.some((x) => typeof x !== "string")) {
          errors.push(`${at} 的 askTools 数组里每项必须是字符串（工具名）`)
        } else {
          item2.askTools = o.askTools as string[]
        }
      } else {
        errors.push(`${at} 的 askTools 必须是字符串（如 "sh,py"）或字符串数组（如 ["sh","py"]）`)
      }
    }
    // 行为参数（与 askTools/reviewOutbound 同属“这个分身怎么干活”，所以写在同一个数组项里）
    const BOOLS = ["replyToOwner", "humanInterrupt"] as const
    for (const k of BOOLS) {
      if (o[k] === undefined) continue
      if (typeof o[k] !== "boolean") {
        errors.push(`${at} 的 ${k} 必须是布尔值（true/false）`)
      } else {
        item2[k] = o[k] as boolean
      }
    }
    if (o.yieldSeconds !== undefined) {
      if (typeof o.yieldSeconds !== "number" || !Number.isFinite(o.yieldSeconds) || o.yieldSeconds < 0) {
        errors.push(`${at} 的 yieldSeconds 必须是 ≥0 的数字（0 = 关闭让位）`)
      } else {
        item2.yieldSeconds = o.yieldSeconds
      }
    }
    if (o.sessionRouting !== undefined) {
      const v = typeof o.sessionRouting === "string" ? o.sessionRouting.trim().toLowerCase() : ""
      if (v !== "driver" && v !== "chat") {
        errors.push(`${at} 的 sessionRouting 只能是 "driver"（缺省，一个分身一个上下文）或 "chat"（按群×话题分会话）`)
      } else {
        item2.sessionRouting = v
      }
    }
    // schedules：定时任务覆盖（按声明 id）。形如 {"morning":{"cron":"0 8 * * 1-5"}}
    if (o.schedules !== undefined) {
      const rawSched = o.schedules
      if (!rawSched || typeof rawSched !== "object" || Array.isArray(rawSched)) {
        errors.push(`${at} 的 schedules 必须是对象（声明 id → 覆盖值，如 {"morning":{"cron":"0 8 * * *"}}）`)
      } else {
        const outSched: Record<string, ScheduleOverride> = {}
        for (const [sid, v] of Object.entries(rawSched as Record<string, unknown>)) {
          const where = `${at}.schedules.${sid}`
          if (!SCHEDULE_ID_RE.test(sid)) {
            errors.push(`${where} 的声明 id 非法（须 [a-z0-9_]+）`)
            continue
          }
          if (!v || typeof v !== "object" || Array.isArray(v)) {
            errors.push(`${where} 必须是对象（如 {"cron":"0 8 * * *"}）`)
            continue
          }
          const r = v as Record<string, unknown>
          const KEYS = new Set(["cron", "prompt", "timezone", "enabled"])
          for (const k of Object.keys(r)) {
            if (!KEYS.has(k)) errors.push(`${where} 含未知键「${k}」（可用：cron / prompt / timezone / enabled）`)
          }
          const one: ScheduleOverride = {}
          for (const k of ["cron", "prompt", "timezone"] as const) {
            if (r[k] === undefined) continue
            if (typeof r[k] !== "string" || !(r[k] as string).trim()) {
              errors.push(`${where} 的 ${k} 必须是非空字符串`)
              continue
            }
            one[k] = (r[k] as string).trim()
          }
          if (r.enabled !== undefined) {
            if (typeof r.enabled !== "boolean") errors.push(`${where} 的 enabled 必须是布尔值`)
            else one.enabled = r.enabled
          }
          if (!Object.keys(one).length) {
            errors.push(`${where} 是空对象（至少写 cron / prompt / timezone / enabled 之一）`)
            continue
          }
          outSched[sid] = one
        }
        if (Object.keys(outSched).length) item2.schedules = outSched
      }
    }

    if (!name) {
      if (out.personaDefaults) {
        errors.push(`${at} 是第二个人格级默认项（不带 name 的项最多一个）`)
        return
      }
      out.personaDefaults = {
        ...(item2.listen ? { listen: item2.listen } : {}),
        ...(item2.send ? { send: item2.send } : {}),
        ...(item2.channels ? { channels: item2.channels } : {}),
        ...(item2.chats ? { chats: item2.chats } : {}),
        ...(item2.sessionMap ? { sessionMap: item2.sessionMap } : {}),
        ...(item2.outbound ? { outbound: item2.outbound } : {}),
        ...(item2.signature ? { signature: item2.signature } : {}),
        // 布尔值需用 !== undefined 判：false 是合法值（关审核）
        ...(item2.reviewOutbound !== undefined ? { reviewOutbound: item2.reviewOutbound } : {}),
        ...(item2.maxConcurrent ? { maxConcurrent: item2.maxConcurrent } : {}),
        ...(item2.askTools !== undefined ? { askTools: item2.askTools } : {}),
        ...(item2.schedules ? { schedules: item2.schedules } : {}),
        // 布尔值用 !== undefined 判：false 是合法值
        ...(item2.replyToOwner !== undefined ? { replyToOwner: item2.replyToOwner } : {}),
        ...(item2.humanInterrupt !== undefined ? { humanInterrupt: item2.humanInterrupt } : {}),
        ...(item2.yieldSeconds !== undefined ? { yieldSeconds: item2.yieldSeconds } : {}),
        ...(item2.sessionRouting !== undefined ? { sessionRouting: item2.sessionRouting } : {}),
      }
      return
    }
    if (out.drivers.has(name)) {
      errors.push(`${at} 的分身 name「${name}」重复`)
      return
    }
    out.drivers.set(name, item2)
  })

  return out
}

function stringArray(v: unknown): string[] | string {
  if (!Array.isArray(v)) return "必须是字符串数组，如 [\"oc_xxx\"]"
  for (const x of v) {
    if (typeof x !== "string" || !x.trim()) return "每项必须是非空字符串"
  }
  return (v as string[]).map((x) => x.trim())
}

/**
 * 把「引用的通道实例 + 群」翻译成输出目标。
 *
 * 为何需要通道实例这一层：旧形态写 `feishu:oc_xxx`，`feishu` 既是渠道类型也是
 * 唯一实例名。现在同类可有多实例（`feishu_main` / `feishu_ops`），所以
 * `channels:["feishu_ops"]` 必须解析成 `feishu_ops:<该实例的群>`。
 *
 * `channels` 与 `chats` 的组合语义：
 * - 只有 `channels`：取这些实例的**全部**群
 * - 只有 `chats`：不限定通道实例（发到默认实例 / 或由调用方决定）
 * - 两者都有：**笛卡尔积**受限，但只保留「该实例确实允许的群」（越界不发）
 */
/**
 * 把「旧形态的扁平 channels/chats」与「新形态的 listen/send」统一成两个方向的引用。
 *
 * 优先级（每个方向各自决定）：该方向的显式声明 > 扁平旧字段 > 空。
 * 所以 `listen` 与 `send` 可以只写一个，另一个自然回落到旧字段（通常为空）。
 */
export function dirRefsOf(item: DriverEnvItem): {
  listen: { channels: string[]; chats: string[] }
  send: { channels: string[]; chats: string[] }
} {
  const pick = (d?: DirRef) => ({
    // 该方向的显式声明优先；否则回落到扁平旧字段。
    // 注意不能用 `d?.channels ?? legacy`—— `d.channels` 为空数组时也会“胜出”，
    // 而 `{}`（写了 listen 但没写 channels）应当被视为“没声明”而不是“声明为空”。
    channels: d?.channels?.length ? d.channels : (item.channels ?? []),
    chats: d?.chats?.length ? d.chats : (item.chats ?? []),
  })
  return { listen: pick(item.listen), send: pick(item.send) }
}

export function outputsFromChannels(
  item: DriverEnvItem,
  instanceChats: ReadonlyMap<string, readonly string[]>,
  instanceDirs?: ReadonlyMap<string, "in" | "out">,
): { targets: OutputTarget[]; errors: string[] } {
  const errors: string[] = []
  const targets: OutputTarget[] = []
  const { send } = dirRefsOf(item)
  const channels = send.channels
  const chats = send.chats

  if (!channels.length) return { targets, errors }

  for (const ch of channels) {
    const allowed = instanceChats.get(ch)
    if (allowed === undefined) {
      errors.push(
        `send 引用了未配置的通道实例「${ch}」——先在 GEBAI_CHANNEL_OUT_<类型> 里配一个 name 为「${ch}」的实例`,
      )
      continue
    }
    // 方向校验：拿入站实例当输出目标 = 发不出去（它根本没有 act 能力面）
    if (instanceDirs?.get(ch) === "in") {
      errors.push(
        `send 引用了入站实例「${ch}」（它只能收、不能发）——` +
          `输出目标要用 GEBAI_CHANNEL_OUT_<类型> 里配的实例`,
      )
      continue
    }
    // 该实例允许的群：chats 给定时取交集（越界不发，不静默发到别的群）
    const picked = chats.length ? chats.filter((c) => allowed.includes(c)) : [...allowed]
    if (chats.length) {
      for (const c of chats) {
        if (!allowed.includes(c)) {
          errors.push(`群 ${c} 不在通道实例「${ch}」的白名单里（该实例允许：${allowed.join("、") || "无"}）`)
        }
      }
    }
    for (const c of picked) targets.push({ channel: ch, address: c, label: ch })
  }
  // 没有显式 `*` 时首条当默认（与旧解析一致）
  if (targets.length) targets[0] = { ...targets[0]!, default: true }
  return { targets, errors }
}

/** 分身配置合并结果（人格级默认 → 分身级覆盖）。 */
export function mergeDriverEnv(
  defaults: Omit<DriverEnvItem, "name"> | null,
  own: DriverEnvItem | undefined,
): DriverEnvItem {
  const d = defaults ?? {}
  const o = own ?? { name: "" }
  return {
    name: o.name,
    ...(o.listen ?? d.listen ? { listen: o.listen ?? d.listen } : {}),
    ...(o.send ?? d.send ? { send: o.send ?? d.send } : {}),
    ...(o.channels ?? d.channels ? { channels: o.channels ?? d.channels } : {}),
    ...(o.chats ?? d.chats ? { chats: o.chats ?? d.chats } : {}),
    ...(o.sessionMap ?? d.sessionMap ? { sessionMap: o.sessionMap ?? d.sessionMap } : {}),
    ...(o.outbound ?? d.outbound ? { outbound: o.outbound ?? d.outbound } : {}),
    ...(o.signature ?? d.signature ? { signature: o.signature ?? d.signature } : {}),
    // 布尔值用 `!== undefined` 判：`false` 是合法且有意义的覆盖值（关审核），
    // 不能像字符串那样用真值判断——那会让分身写了 false 却被当成“没写”。
    ...(o.reviewOutbound !== undefined || d.reviewOutbound !== undefined
      ? { reviewOutbound: o.reviewOutbound ?? d.reviewOutbound }
      : {}),
    ...(o.maxConcurrent ?? d.maxConcurrent ? { maxConcurrent: o.maxConcurrent ?? d.maxConcurrent } : {}),
    // 用 !== undefined 判：空串/空数组是**有意的值**（= 什么都不问，覆盖掉人格级默认）
    ...(o.askTools !== undefined || d.askTools !== undefined
      ? { askTools: o.askTools !== undefined ? o.askTools : d.askTools }
      : {}),
    ...(o.schedules ?? d.schedules ? { schedules: o.schedules ?? d.schedules } : {}),
    // 行为参数：同样用 !== undefined（false 与 0 都是合法值）
    ...(o.replyToOwner !== undefined || d.replyToOwner !== undefined
      ? { replyToOwner: o.replyToOwner !== undefined ? o.replyToOwner : d.replyToOwner }
      : {}),
    ...(o.humanInterrupt !== undefined || d.humanInterrupt !== undefined
      ? { humanInterrupt: o.humanInterrupt !== undefined ? o.humanInterrupt : d.humanInterrupt }
      : {}),
    ...(o.yieldSeconds !== undefined || d.yieldSeconds !== undefined
      ? { yieldSeconds: o.yieldSeconds !== undefined ? o.yieldSeconds : d.yieldSeconds }
      : {}),
    ...(o.sessionRouting !== undefined || d.sessionRouting !== undefined
      ? { sessionRouting: o.sessionRouting !== undefined ? o.sessionRouting : d.sessionRouting }
      : {}),
  }
}

/** 便捷：把 sessionMap 字符串解析成规则（复用既有解析器，避免两套语法）。 */
export type { SessionMapRule, DriverDef }
