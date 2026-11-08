/**
 * 人格的**定时工作面**：把 `DriverDef.schedules` 声明变成按点到达的事件。
 *
 * ## 为什么定时任务长在 driver 上，而不是「人格的 cron 表」
 *
 * driver 的粒度是**专注的任务类别**（"我盯告警"、"我写日报"），它的入口本来就是
 * `subscriptions`。定时任务与订阅是同一件事的两种触发方式——放人格级会迫使一个角色的
 * 多个分身共用一张时刻表，而实际上「早报在报告分身跑」「巡检在运维分身跑」。
 *
 * ## 为什么走事件，而不是直接调引擎 / 复用统一任务管理
 *
 * | 路径 | 后果 |
 * |---|---|
 * | 本模块 → `ctx.emit` → `PersonaRuntime` | 身份、出站纪律、审批姿态、同会话串行**全部沿用** |
 * | 直接 `engine.run` | 不注入人格注记（模型不知道自己是谁）、绕开调度器的同会话串行约束 |
 * | 借 `TaskManager`（`target: "session"`） | 同上，且人格调度器看不到会话被占（「以为闲着」） |
 *
 * 所以本文件只做两件纯函数的事：**算档期**（`collectDue`）与**造事件**（`scheduleEvent`）。
 * 到点之后的一切归内核裁决，与渠道消息完全同一条路。
 *
 * ## 三条语义（都是「不静默做错事」取向）
 *
 * 1. **首次见到某条声明不回溯**（`cursor = now`）：加一条 `0 9 * * *` 不该在 14:00
 *    立刻触发一次。要「启动即补跑」必须显式 `misfire: "run"`。
 * 2. **落后窗口有上限**（`DEFAULT_CATCHUP_MS`）：进程被卡住/休眠后醒来，只补最近
 *    窗口内的档期，更早的整段跳过——否则 `@every 1m` 停一天会一次喷出 1440 条事件，
 *    把该分身的队列瞬间打满（而队列满的表现是后续真实消息全被 overflow 丢弃）。
 * 3. **幂等键按「人格/分身/声明/档期时刻」推导**：同一档期重投必被丢弃（`eventIdFor`
 *    的 schedule 规则），不同分身跑各自的同一时刻互不干扰。
 */

import type { PersonaEventRefs } from "./event"
import type { DriverSchedule } from "./ports"
import { isOneShotSchedule, parseSchedule } from "./cron"

/** 落后窗口：只补这段时间内的档期（更早的整段跳过，见文件头第 2 条）。 */
export const DEFAULT_CATCHUP_MS = 5 * 60_000

/** 单次 tick 最多补发多少档期（防一条 `@every 1s` 把队列打满）。 */
export const MAX_SLOTS_PER_TICK = 64

/** 一条声明 + 它的归属（源按这个视图算档期）。 */
export interface ScheduleDeclaration {
  personaId: string
  driverId: string
  /** 分身显示名（进事件的「来源」，模型据此知道是哪个人格被唤醒了）。 */
  personaName?: string
  schedule: DriverSchedule
}

/** 声明发现所需的最小人格视图（结构兼容 `PersonaDef`；不 import 它以免与校验模块循环）。 */
export interface ScheduleOwnerView {
  id: string
  name?: string
  drivers?: Array<{ id: string; schedules?: DriverSchedule[] }>
}

/**
 * 从一组人格定义收出全部定时声明（宿主每次 tick 现取，见源里的 `personaSchedules`）。
 *
 * 为什么不在源里扫目录：人格定义的发现是宿主的职责（双域扫描 + 二开覆盖 + 热加载），
 * 源再扫一份就是第二份发现逻辑，两份必然分叉（“源看到的声明”与“内核认的分身”不一致）。
 */
export function collectScheduleDeclarations(personas: ReadonlyArray<ScheduleOwnerView>): ScheduleDeclaration[] {
  const out: ScheduleDeclaration[] = []
  for (const p of personas) {
    for (const d of p.drivers ?? []) {
      for (const schedule of d.schedules ?? []) {
        out.push({
          personaId: p.id,
          driverId: d.id,
          ...(p.name ? { personaName: p.name } : {}),
          schedule,
        })
      }
    }
  }
  return out
}

/** 档期游标键：`{personaId}/{driverId}/{scheduleId}`。 */
export function scheduleKey(d: Pick<ScheduleDeclaration, "personaId" | "driverId" | "schedule">): string {
  return `${d.personaId}/${d.driverId}/${d.schedule.id}`
}

/** 幂等键：同一档期（同一时刻）只该触发一次。 */
export function scheduleEventId(personaId: string, driverId: string, scheduleId: string, slot: number): string {
  return `task:persona:${personaId}/${driverId}/${scheduleId}@${slot}`
}

/** 一条到期档期的投递内容（源直接交给 `ctx.emit`）。 */
export interface DueEvent {
  id: string
  personaId: string
  driverId: string
  /** 计划触发时刻（不是发现时刻——补跑时两者不同，幂等键用计划时刻）。 */
  at: number
  source: string
  summary: string
  body: string
  refs: PersonaEventRefs
}

/** 档期计算结果。 */
export interface DueResult {
  events: DueEvent[]
  /** 被跳过积压的分身声明（诊断：落后太久时如实记一笔，而不是装作没发生）。 */
  skipped: Array<{ key: string; droppedUntil: number }>
  /** 表达式解析失败（声明是加载期校验过的，这里只剩「永不触发」一类）。 */
  errors: Array<{ key: string; error: string }>
}

/** 档期的下一触发时刻（解析失败向上抛，由调用方按声明隔离）。 */
export function nextSlot(schedule: DriverSchedule, fromMs: number): number {
  return parseSchedule(schedule.cron, schedule.timezone).next(fromMs)
}

/**
 * 算出一批声明在 `now` 时刻之前到期的档期，并推进游标。
 *
 * `cursors` 会被**原地修改**（源持有它，跨 tick 保留）。语义见文件头三条。
 */
export function collectDue(input: {
  declarations: ReadonlyArray<ScheduleDeclaration>
  cursors: Map<string, number>
  now: number
  catchupMs?: number
  maxSlots?: number
}): DueResult {
  const catchupMs = input.catchupMs ?? DEFAULT_CATCHUP_MS
  const maxSlots = input.maxSlots ?? MAX_SLOTS_PER_TICK
  const out: DueResult = { events: [], skipped: [], errors: [] }
  // 已消失的声明要从游标表里摘掉：否则同名 id 日后被重新加回时，
  // 旧游标会把它当成「一直在跑」——表现是「重新加回去的定时任务第一次不触发」。
  const alive = new Set(input.declarations.map((d) => scheduleKey(d)))
  for (const key of [...input.cursors.keys()]) if (!alive.has(key)) input.cursors.delete(key)

  for (const decl of input.declarations) {
    const key = scheduleKey(decl)
    const { schedule } = decl
    try {
      let cursor = input.cursors.get(key)
      if (cursor === undefined) {
        // 首次见到该声明：**不回溯**。要「启动即补跑」由 misfire:"run" 显式表达。
        if (schedule.misfire === "run") {
          out.events.push(makeEvent(decl, input.now))
        }
        input.cursors.set(key, nextSlot(schedule, input.now))
        continue
      }
      // 游标语义：**下一条待触发的档期**（不是“上次触发时刻”）。
      // 所以先看游标本身是否已到期，再推向下一条——而不是先算下一条再比（那样
      // 游标指向的那一档会被永久跳过：首算得 T，到 T 时又算得 T+1，T 从未发出）。
      if (input.now - cursor > catchupMs) {
        // 落后太久（进程阻塞/休眠/机器关机）：只补最近窗口内的档期，更早的整段跳——
        // 否则 @every 1m 停一天会一次喷出上千条事件，把该分身的队列打满（后果是
        // 后续**真实消息**全被 overflow 丢弃，比少跑几档严重得多）。
        const resume = nextSlot(schedule, input.now - catchupMs)
        out.skipped.push({ key, droppedUntil: resume })
        cursor = resume
      }
      let emitted = 0
      let slot = cursor
      for (;;) {
        if (slot > input.now) break
        if (emitted >= maxSlots) {
          // 上限兜底（`@every 1s` 之类）：游标推到当前时刻，剩余积压按「跳过」处理
          out.skipped.push({ key, droppedUntil: input.now })
          slot = nextSlot(schedule, input.now)
          break
        }
        out.events.push(makeEvent(decl, slot))
        emitted++
        slot = nextSlot(schedule, slot)
      }
      input.cursors.set(key, slot)
    } catch (err) {
      out.errors.push({ key, error: String((err as Error)?.message || err) })
    }
  }
  return out
}

/** 造一条到期事件（人类可读来源与摘要都带人格名，模型据此知道是谁被唤醒）。 */
function makeEvent(decl: ScheduleDeclaration, slot: number): DueEvent {
  const who = decl.personaName ? `${decl.personaName}（${decl.personaId}）` : decl.personaId
  const label = decl.schedule.description?.trim() || decl.schedule.id
  return {
    id: scheduleEventId(decl.personaId, decl.driverId, decl.schedule.id, slot),
    personaId: decl.personaId,
    driverId: decl.driverId,
    at: slot,
    source: `定时任务「${label}」· ${who}`,
    summary: `${label}到点了（${decl.schedule.cron}，计划 ${formatTime(slot)}）`,
    body: decl.schedule.prompt,
    refs: {
      taskId: decl.schedule.id,
      extra: { scheduleId: decl.schedule.id, cron: decl.schedule.cron, slot: String(slot) },
    },
  }
}

function formatTime(ms: number): string {
  const d = new Date(ms)
  const p2 = (n: number) => String(n).padStart(2, "0")
  return `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())} ${p2(d.getHours())}:${p2(d.getMinutes())}`
}

/** 单条声明校验（加载期用；返回错误文案或 null）。 */
export function validateDriverSchedule(raw: unknown, ctx: { driverId: string; seen: Set<string> }): string | null {
  if (!raw || typeof raw !== "object") return `drivers「${ctx.driverId}」的 schedules 每项必须是对象`
  const o = raw as Record<string, unknown>
  const allowed = new Set(["id", "cron", "prompt", "description", "timezone", "misfire"])
  for (const k of Object.keys(o)) {
    if (!allowed.has(k)) return `drivers「${ctx.driverId}」的 schedules 项含未知字段「${k}」（可用：${[...allowed].join(" / ")}）`
  }
  if (typeof o.id !== "string" || !/^[a-z0-9_]+$/.test(o.id)) {
    return `drivers「${ctx.driverId}」的 schedules.id 必须是 [a-z0-9_]+（当前：${String(o.id)}）`
  }
  if (ctx.seen.has(o.id)) return `drivers「${ctx.driverId}」的 schedules.id「${o.id}」重复（同一分身内须唯一）`
  ctx.seen.add(o.id)
  if (typeof o.cron !== "string" || !o.cron.trim()) return `drivers「${ctx.driverId}」的定时「${o.id}」缺少 cron 表达式`
  if (typeof o.prompt !== "string" || !o.prompt.trim()) {
    return `drivers「${ctx.driverId}」的定时「${o.id}」缺少 prompt（到点后要它做什么）`
  }
  if (o.description !== undefined && typeof o.description !== "string") {
    return `drivers「${ctx.driverId}」的定时「${o.id}」的 description 必须是字符串`
  }
  if (o.timezone !== undefined && typeof o.timezone !== "string") {
    return `drivers「${ctx.driverId}」的定时「${o.id}」的 timezone 必须是 IANA 时区名（如 Asia/Shanghai）`
  }
  if (o.misfire !== undefined && o.misfire !== "skip" && o.misfire !== "run") {
    return `drivers「${ctx.driverId}」的定时「${o.id}」的 misfire 只能是 "skip"（缺省）或 "run"`
  }
  // 表达式必须**可解析且能算出下一次**：写错的 cron 在运行期只会落进 errors 并永不触发，
  // 那正是「配了但没反应」最难查的形态——所以在加载期就拦住。
  try {
    const now = Date.now()
    const next = parseSchedule(o.cron, typeof o.timezone === "string" ? o.timezone : undefined).next(now)
    // @at 已过期：放行等于「加进去立刻触发一次」，与部署方的意图（那一刻做那件事）不符。
    if (isOneShotSchedule(o.cron) && next <= now) {
      return `drivers「${ctx.driverId}」的定时「${o.id}」的一次性时间已过去（${o.cron}）——改成未来时刻，或用 cron/@every`
    }
  } catch (err) {
    return `drivers「${ctx.driverId}」的定时「${o.id}」表达式无效: ${String((err as Error)?.message || err)}`
  }
  return null
}
