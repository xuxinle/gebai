/**
 * 部署配置面：**人格共性写代码、使用者差异写环境变量**。
 *
 * ## 为什么这些解析必须在内核里，而不是装配层
 *
 * 「哪些群归这个人格」「它能发到哪」「这个角色谁来扮演」都是**人格语义**，
 * 不是某一种宿主（server / 桌面 / 测试）的装配细节。写在 compose 里会有三个后果：
 *
 * 1. 无法用纯函数单测（要起服务端才能验一条解析规则）
 * 2. 入站收窄在 compose、出站白名单在 core/tools、身份渲染在第三个地方——三处各写一遍
 *    必然分叉（同一个「只看这些群」的语义被两处按不同口径解释过，是真实踩过的坑）
 * 3. 换宿主（桌面形态、CLI）就得重写一遍
 *
 * 所以这里收敛成**纯函数**：输入 `Map<id, PersonaDef>` + env 视图，输出解析后的定义。
 * 装配层只负责"把 process.env 传进来"。
 *
 * ## 变量表
 *
 * | 变量 | 语义 |
 * |---|---|
 * | `GEBAI_PERSONA_ENABLE` | 人格启用白名单（逗号分隔 id；非空仅启用名单内） |
 * | `GEBAI_PERSONA_<ID>` | **该人格的全部分身配置（JSON 数组，每项一个分身）**——见 `persona-config.ts` |
 * | `GEBAI_PERSONA_SESSION_ROUTING` | 兜底规则：`driver`（缺省，一个上下文）/ `chat`（群×话题） |
 * | `GEBAI_PERSONA_APPROVAL` | 审批姿态：`ask`（缺省，相期弹窗问人）/ `auto` / `deny` |
 *
 * ## 为什么不再拼 `<ID>_<DRIVER>_XXX`（已删除的旧形态）
 *
 * 人格 id 与 driver id 都可能含下划线，`GEBAI_PERSONA_FIN_OPS_ALERT_CHATS` 到底是
 * 「人格 fin_ops 的分身 alert」还是「人格 fin 的分身 ops_alert」在字符串层面**无解**。
 * 旧实现靠“拿已知定义逐条比对”绕过这个问题，但那是给一个设计缺陷打补丁：
 * 新增/改名一个分身就要重新推一遍变量名，且拼错只是静默不生效。
 * 现在这些属性都写在 `GEBAI_PERSONA_<ID>` 数组的对应项里，歧义**不存在**。
 */

import type { PersonaDef } from "./persona"
import type { DriverDef, OutputTarget } from "./ports"
import { parseSessionMap } from "./session-map"
import { matchesSubscription } from "./subscription"
import {
  dirRefsOf,
  mergeDriverEnv,
  outputsFromChannels,
  parsePersonaEnv,
  type DriverEnvItem,
} from "./persona-config"

/** env 视图（缺省读 process.env；测试注入）。 */
export type EnvView = Record<string, string | undefined>

const listOf = (raw: string | undefined): string[] =>
  (raw ?? "")
    .split(/[,\s]+/)
    .map((s) => s.trim())
    .filter(Boolean)

/** 变量名片段：id 大写、非字母数字压成下划线（与既有口径一致）。 */
const upper = (s: string): string => s.toUpperCase()

/**
 * 解析 `channel:address` 形式的输出目标（**保留**，不属于已删除的拼名路径）。
 *
 * 两种调用场景：
 * 1. 代码里的人格的 `drivers[].outputs` 仍用这个类型（不是环境变量）
 * 2. 二开代码可以直接调它把字串转成目标
 *
 * 环境变量侧不再产生这种字串（改用 `channels:["实例名"]`，见 `persona-config.ts`）。
 */
export function parseOutputTargets(raw: string | undefined): { targets: OutputTarget[]; errors: string[] } {
  const targets: OutputTarget[] = []
  const errors: string[] = []
  for (const item of listOf(raw)) {
    let text = item
    let isDefault = false
    if (text.startsWith("*")) {
      isDefault = true
      text = text.slice(1)
    }
    // 标签：末段 `=` 之后（地址里不会出现 `=`；`#` 留给话题）
    let label: string | undefined
    const eq = text.lastIndexOf("=")
    if (eq > 0) {
      label = text.slice(eq + 1).trim() || undefined
      text = text.slice(0, eq)
    }
    let thread: string | undefined
    const hash = text.indexOf("#")
    if (hash > 0) {
      thread = text.slice(hash + 1).trim() || undefined
      text = text.slice(0, hash)
    }
    const idx = text.indexOf(":")
    if (idx <= 0 || idx === text.length - 1) {
      errors.push(`输出目标「${item}」格式非法（应为 channel:address，如 feishu:oc_xxx）`)
      continue
    }
    const channel = text.slice(0, idx).trim().toLowerCase()
    const address = text.slice(idx + 1).trim()
    if (!channel || !address) {
      errors.push(`输出目标「${item}」渠道名或地址为空`)
      continue
    }
    targets.push({
      channel,
      address,
      ...(thread ? { thread } : {}),
      ...(label ? { label } : {}),
      ...(isDefault ? { default: true } : {}),
    })
  }
  // 没有显式 `*` 时把首条当默认（模型不必每次自己挑）
  if (targets.length && !targets.some((t) => t.default)) targets[0] = { ...targets[0]!, default: true }
  return { targets, errors }
}

/** 收窄结果（诊断用：部署方要能看见「哪些群真的生效了」）。 */
export interface PersonaEnvReport {
  /** 被启用白名单排除的人格。 */
  disabled: string[]
  /** 收窄了入站群的人格/分身（`personaId` 或 `personaId/driverId` → 群列表）。 */
  narrowed: Record<string, string[]>
  /** 输出目标（`personaId` 或 `personaId/driverId` → 条数）。 */
  outputs: Record<string, number>
  /** 会话映射已配置的分身（`personaId/driverId` → 规则条数）。 */
  sessionMaps: Record<string, number>
  /** 数组里配了其他分身、但没配这个的分身（不是错误，但面板要说清）。 */
  unconfigured: string[]
  /** 显式关掉出站审核的人格/分身（`personaId` 或 `personaId/driverId`）——缺省开，关必须显式写。 */
  reviewDisabled: string[]
  /** 被环境变量覆盖了 cron 的定时声明：`人格/分身` → 条数（启动时可审计）。 */
  scheduleOverrides: Record<string, number>
  /** 解析错误（配错了但不该阻断启动——如实上报）。 */
  errors: string[]
}

/**
 * 能匹配「某渠道实例的消息事件」的订阅（收窄时被替换掉的那几类）。
 *
 * 用真实订阅匹配函数判定而不是字符串枚举：枚举法会在订阅语法演进（三段式、
 * 大小写、顺序无关）后**静默漏掉**一类，表现为“设了 chats 但某些人格还是收到别的群”。
 *
 * `channel` 必须传**实际渠道实例名**（如 `feishu_main`），不能写死 `feishu`：
 * 渠道名就是 target 前缀与事件的 channel 字段，实例化之后它可能不是类型名。
 */
function coversChannelMessages(sub: string, channel: string): boolean {
  return matchesSubscription(sub, { kind: "message", channel, chatId: "__probe__" })
}

/**
 * 收窄某组订阅到指定群（替换覆盖**该渠道**消息的那几条，其余原样保留）。
 *
 * `channel` 是该群所属的**渠道实例名**：生成的逐群订阅必须是 `feishu_main:oc_x:message`
 * 而不是 `feishu:oc_x:message`——事件里的 channel 字段就是实例名，
 * 前缀写错的话收窄后的订阅一条也命不中，表现是“配了 chats 之后它彻底不说话了”。
 */
export function narrowSubscriptions(
  subs: readonly string[],
  chats: readonly string[],
  channel = "feishu",
): string[] {
  const kept = subs.filter((s) => !coversChannelMessages(s, channel))
  const chatSubs = chats.map((c) => `${channel}:${c}:message`)
  return [...kept, ...chatSubs.filter((s) => !kept.includes(s))]
}

/**
 * 找出某个群属于哪个渠道实例。
 *
 * 判定顺序：
 * 1. 该分身显式引用的 `channels` 里，哪个实例的白名单包含它
 * 2. 否则在全部已知实例里找第一个包含它的
 * 3. 都找不到 → 返回 null（**不猜**，由调用方报错）
 *
 * 为何不回落成 `"feishu"`：实例化之后事件的 channel 是实例名（`feishu_main`），
 * 猜测出来的类型名前缀会生成一条**永远命不中**的订阅——
 * 表现是“配了 chats 之后它彻底不说话了”，而日志里一个字都没有。
 * 宁可在装配期报一条明确的错。
 */
function channelOfChat(
  chat: string,
  merged: { channels?: string[] },
  channelChats: ReadonlyMap<string, readonly string[]>,
): string | null {
  for (const inst of merged.channels ?? []) {
    if (channelChats.get(inst)?.includes(chat)) return inst
  }
  for (const [inst, chats] of channelChats) {
    if (chats.includes(chat)) return inst
  }
  return null
}

/**
 * 把环境变量应用到人格定义上（纯函数，返回新 Map）。
 *
 * 三件事，各自独立，失败互不影响：
 * 1. 启用白名单过滤（`GEBAI_PERSONA_ENABLE`）
 * 2. 入站收窄 + 输出目标（`GEBAI_PERSONA_<ID>` 数组里的 `chats` / `channels`）
 * 3. 会话映射（同一个数组里的 `sessionMap`）
 *
 * `channelChats` 是通道实例名 → 该实例允许的群（由宿主从已装配的渠道注册表取）。
 * 宽窄引用必须拿它才能解析——否则“引用了不存在的实例”只能等到发消息那一刻才报错。
 */
export function applyPersonaEnv(
  personas: ReadonlyMap<string, PersonaDef>,
  env: EnvView,
  channelChats: ReadonlyMap<string, readonly string[]> = new Map(),
  channelDirs: ReadonlyMap<string, "in" | "out"> = new Map(),
): { personas: Map<string, PersonaDef>; report: PersonaEnvReport } {
  const report: PersonaEnvReport = {
    disabled: [],
    narrowed: {},
    outputs: {},
    sessionMaps: {},
    unconfigured: [],
    reviewDisabled: [],
    scheduleOverrides: {},
    errors: [],
  }
  const enable = new Set(listOf(env.GEBAI_PERSONA_ENABLE))
  const out = new Map<string, PersonaDef>()

  for (const [id, def] of personas) {
    if (enable.size && !enable.has(id)) {
      report.disabled.push(id)
      continue
    }
    // 一个变量装下该人格的全部分身配置（不再拼 `<ID>_<DRIVER>_XXX`）
    const parsedEnv = parsePersonaEnv(id, env[`GEBAI_PERSONA_${upper(id)}`])
    report.errors.push(...parsedEnv.errors.map((e) => `人格 ${id}：${e}`))

    // 人格级默认：先算自己的输出目标（分身未单独配时继承）
    const defaultsMerged = parsedEnv.personaDefaults
    const defaultsOutputs = defaultsMerged
      ? outputsFromChannels(defaultsMerged as DriverEnvItem, channelChats, channelDirs)
      : { targets: [], errors: [] }
    report.errors.push(...defaultsOutputs.errors.map((e) => `人格 ${id}：${e}`))
    const defaultsListen = defaultsMerged ? dirRefsOf(defaultsMerged as DriverEnvItem).listen : { channels: [], chats: [] }
    if (defaultsListen.chats.length) report.narrowed[id] = defaultsListen.chats
    if (defaultsOutputs.targets.length) report.outputs[id] = defaultsOutputs.targets.length

    const drivers = (def.drivers ?? []).map((d) => {
      const own = parsedEnv.drivers.get(d.id)
      if (own === undefined && parsedEnv.drivers.size > 0) {
        // 数组里配了其他分身但没配这个：不是错误（其余用默认），但面板要知道
        // 它是“没被单独配”而不是“配了但没生效”。
        report.unconfigured.push(`${id}/${d.id}`)
      }
      const merged = mergeDriverEnv(parsedEnv.personaDefaults, own)
      const driverOutputs = outputsFromChannels(merged, channelChats, channelDirs)
      report.errors.push(...driverOutputs.errors.map((e) => `人格 ${id} 分身 ${d.id}：${e}`))

      const listenRef = dirRefsOf(merged).listen
      const chats = listenRef.chats
      // 方向校验：listen 引用出站实例 = 收不到（它没有 startListen）
      for (const ch of listenRef.channels) {
        if (channelDirs.get(ch) === "out") {
          report.errors.push(
            `人格 ${id} 分身 ${d.id}：listen 引用了出站实例「${ch}」（它只能发、不能收）——` +
              `入站要用 GEBAI_CHANNEL_IN_<类型> 里配的实例`,
          )
        }
      }
      // 输出目标：分身级有就用自己的（覆盖人格级），都空则不注入
      const outputs = driverOutputs.targets.length ? driverOutputs.targets : defaultsOutputs.targets
      if (chats.length) report.narrowed[`${id}/${d.id}`] = chats
      if (driverOutputs.targets.length) report.outputs[`${id}/${d.id}`] = driverOutputs.targets.length

      // 会话映射：分身级优先，其次人格级（已在 mergeDriverEnv 里合过）
      const parsedMap = parseSessionMap(merged.sessionMap)
      report.errors.push(...parsedMap.errors.map((e) => `人格 ${id} 分身 ${d.id}：${e}`))
      const sessionMap = parsedMap.rules.length ? parsedMap.rules : d.sessionMap
      if (parsedMap.rules.length) report.sessionMaps[`${id}/${d.id}`] = parsedMap.rules.length

      // 出站审核开关：**缺省开**，只有显式写 false 才关（安全的那一侧）。
      // 记进 report 的原因：关审核是“人格会以你名义不打招呼就发言”，启动时该看得见。
      if (merged.reviewOutbound === false) report.reviewDisabled.push(`${id}/${d.id}`)

      // 收窄订阅：**按该群所属的渠道实例名**生成逐群订阅。
      //
      // 为何不能统一用 `feishu`：事件里的 channel 字段是**实例名**（`feishu_main`），
      // 实例化之后它不一定等于类型名；前缀写错会让收窄后的订阅一条也命不中——
      // 表现是“配了 chats 之后它彻底不说话了”。
      // 一个分身可能监多个实例的群，所以要按实例分组，而不是一次 narrow 完事。
      const narrowedSubs = ((): string[] => {
        if (!chats.length) return [...d.subscriptions]
        const byChannel = new Map<string, string[]>()
        for (const c of chats) {
          const inst = channelOfChat(c, merged, channelChats)
          if (!inst) {
            // 群不属于任何已知实例：报错而不猜。否则会生成一条永远命不中的订阅。
            report.errors.push(
              `人格 ${id} 分身 ${d.id}：群 ${c} 不属于任何渠道实例的白名单` +
                `（请在某个 GEBAI_CHANNEL_<类型> 实例的 chats 里加上它，或用 channels 指定实例）`,
            )
            continue
          }
          const arr = byChannel.get(inst) ?? []
          arr.push(c)
          byChannel.set(inst, arr)
        }
        let subs = [...d.subscriptions]
        for (const [inst, group] of byChannel) subs = narrowSubscriptions(subs, group, inst)
        return subs
      })()

      // 定时表达式覆盖：环境变量里的 `schedules` 按声明 id 覆盖代码定义里的 cron/prompt/timezone。
      //
      // 为何只覆盖已有声明的字段、不新增声明：声明（id、是否存在）是**代码事实**，
      // 而“几点跑”是**部署差异**。允许环境变量新增声明会让“id 拼错”
      // 变成一个凭空多出的定时任务（静默错），而不是一条可读的报错。
      const schedOverrides = (() => {
        const ov = merged.schedules
        if (!ov || !Object.keys(ov).length) return undefined
        const base = d.schedules ?? []
        if (!base.length) {
          report.errors.push(
            `人格 ${id} 分身 ${d.id}：schedules 配了覆盖（${Object.keys(ov).join("、")}），` +
              `但代码定义里没有任何 schedules——覆盖无处可施（检查声明 id）`,
          )
          return undefined
        }
        const known = new Set(base.map((s) => s.id))
        for (const sid of Object.keys(ov)) {
          if (!known.has(sid)) {
            report.errors.push(
              `人格 ${id} 分身 ${d.id}：schedules 覆盖引用了不存在的声明「${sid}」` +
                `（代码定义里有：${[...known].join("、")}）`,
            )
          }
        }
        const applied = base
          .filter((s) => ov[s.id]?.enabled !== false)
          .map((s) => {
            const o = ov[s.id]
            if (!o) return s
            return {
              ...s,
              ...(o.cron ? { cron: o.cron } : {}),
              ...(o.prompt ? { prompt: o.prompt } : {}),
              ...(o.timezone ? { timezone: o.timezone } : {}),
            }
          })
        report.scheduleOverrides[`${id}/${d.id}`] = Object.entries(ov).filter(
          ([sid, o]) => o.cron && known.has(sid),
        ).length
        return applied
      })()

      const next: DriverDef = {
        ...d,
        subscriptions: narrowedSubs,
        ...(outputs.length ? { outputs } : {}),
        ...(sessionMap?.length ? { sessionMap } : {}),
        ...(merged.maxConcurrent ? { maxConcurrent: merged.maxConcurrent } : {}),
        ...(schedOverrides ? { schedules: schedOverrides } : {}),
      }
      return next
    })

    out.set(id, { ...def, drivers })
  }
  return { personas: out, report }
}

/**
 * 会话关联规则（`GEBAI_PERSONA_SESSION_ROUTING`）。
 *
 * ## 会话面向的是**上下文**，不是群
 *
 * 这是本内核最容易理解错的一条，写在这里当作契约：
 *
 * > 会话是**一段连续工作的上下文**，不是“一个群”。渠道（群）只是事件的一个属性，
 * > 不参与会话身份。因此同一个会话里可以同时有来自多个群的消息——它们都归这个
 * > 分身当前的关注面（“我盯告警”就是一个上下文）。
 *
 * 反过来说也对：同一个群里的不同话题/工单可以是不同上下文（那要用 `sessionKeyOf`
 * 声明“按什么分”，例如按工单号、按仓库、按话题根）。
 *
 * ## 两档
 *
 * | 值 | 含义 | 适用 |
 * |---|---|---|
 * | `driver`（缺省） | 一个分身一个常驻会话；**所有群的消息都注入这个上下文** | 绝大多数场景 |
 * | `chat` | 按「群 × 话题」分会话 | 部署方**显式要求**群间上下文隔离时 |
 *
 * 为何缺省是 `driver`：把群当会话身份会把“一个人”拆成几份互不知情的助手——
 * A 群里排查了一小时，B 群里问同一件事时它又从头问起；而人不是这样工作的。
 * 真正的分割应来自**上下文**（不同事务），而不是来自**渠道**（消息从哪里来）。
 */
export function sessionRoutingMode(env: EnvView): "driver" | "chat" {
  const raw = (env.GEBAI_PERSONA_SESSION_ROUTING ?? "").trim().toLowerCase()
  return raw === "chat" ? "chat" : "driver"
}

/**
 * `chat` 模式下的会话键推导（**只在部署显式要求群间隔离时使用**）。
 *
 * 强调：这不是默认行为。会话面向上下文，不面向群——把这个函数当默认会把
 * “一个人”拆成几份互不知情的助手（见 `sessionRoutingMode` 的注释）。
 *
 * 规则：message → `{chatId}` 或 `{chatId}__t_{threadId}`；定时/自省 → `anchor`。
 */
export function chatSessionKeyOf(event: { kind: string; channel: string; context: { refs?: { chatId?: string; threadId?: string } } }): string {
  if (event.kind === "schedule" || event.kind === "idle") return "anchor"
  const chat = event.context.refs?.chatId ?? ""
  if (!chat) return ""
  const thread = event.context.refs?.threadId ?? ""
  return thread ? `${chat}__t_${thread}` : chat
}

