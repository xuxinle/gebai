/**
 * 拟人模式面板（设置 → 拟人）：**「它到底在不在干活」一眼可见**。
 *
 * ## 为什么放在设置面板而不是另开一个页面
 *
 * 拟人模式是一个**部署特性**（要不要开、开哪些源、凭据通不通），不是日常操作面。
 * 它的用户是部署方/运维，打开频率低但每次都关心细节——正是设置面板的定位。
 * 另开页面会让它被当成「一个新功能模块」，反而更难找。
 *
 * ## 这个面板必须回答的四个问题（顺序刻意如此）
 *
 * 1. **凭据通不通**：不通时人格「不回消息」，而所有计数都正常——最容易误判的一类
 * 2. **事件进不进得来**：事件源是否启动 + 渠道入站计数（含 unrouted：无人订阅）
 * 3. **谁在干活**：每个分身的会话、队列深度、失败计数
 * 4. **有没有配错**：源加载失败清单 + 订阅面
 *
 * 前三项都是「现状是否健康」，第四项是「配置对不对」——健康与正确是两件事，
 * 混在一起说会让人以为「跑起来了就是配对了」。
 */

import { el } from "./state"

/** 与服务端 `PersonaStateSnapshot` 对齐（前端不 import 后端类型）。 */
export interface PersonaStateView {
  enabled: boolean
  generatedAt: number
  personas: Array<{
    id: string
    name: string
    role: string
    description?: string
    drivers: Array<{
      id: string
      focus: string
      subscriptions: string[]
      origin: "static" | "dynamic"
      reason?: string
      maxConcurrent?: number
      /** 该分身声明的定时任务（`DriverDef.schedules`）。 */
      schedules?: Array<{ id: string; cron: string; description?: string; timezone?: string; misfire?: "skip" | "run" }>
      active: boolean
      running: number
      queued: number
      accepted: number
      deduped: number
      overflowed: number
      failed: number
      sessionId: string | null
      sessionRunning: boolean
    }>
  }>
  sources: Array<{
    name: string
    channel: string
    description: string
    enabled: boolean
    started: boolean
    error?: string
    stats: { emitted: number; accepted: number; unattended: number; duplicate: number; overflow: number; unavailable: number }
  }>
  sourceErrors: Record<string, string>
  channelInput: Record<string, { emitted: number; accepted: number; unrouted: number; duplicate: number; overflow: number; unavailable: number }>
  /**
   * 渠道启停状态（含**启动失败原因**）。`undefined` = 旧服务端未提供该字段。
   *
   * 为何单列一段：渠道因“与已启用的会话机器人同 AppID”而拒启时，那条错误只在
   * `channels_list` 工具输出与启动日志里——模型看得见、**人看不见**。而“人格不回话”的
   * 首要嫌疑就是“渠道没起来”，面板必须自己能答。
   */
  channelState?: Array<{
    name: string
    description: string
    enabled: boolean
    started: boolean
    error?: string
    listening?: boolean
    /** 方向：in 只收；out 只发/读（不建连接）。旧后端不返回时当 out 处理。 */
    dir?: "in" | "out"
    /** 这个实例能否启停监听（out 实例或渠道类型未实现 startListen 时为 false）。 */
    canListen?: boolean
  }>
  /** 派出去的活（最近的在前；快照有、面板渲染它） */
  workers: Array<{
    runId: string
    label?: string
    personaId: string
    driverId: string
    status: "running" | "done" | "failed" | "cancelled"
    startedAt: number
    elapsedMs?: number
  }>
  credentials: { ready: boolean; state: string; message: string; remainingMs?: number; needsAuth: boolean } | null
  /** 拟人装配失败原因（服务仍正常运行；有值 = 开关开了但坏了）。 */
  setupError?: string
  totals: { drivers: number; activeDrivers: number; running: number; queued: number; failed: number; workersRunning: number }
}

/**
 * 拉取渠道启停状态（**不依赖拟人是否启用**）。
 *
 * 为何单独一个请求：拟人未启用时 `/persona/state` 是 404，但“渠道起没起来”
 * 恰恰是用户最需要看的——它跟人格无关，却是“它不说话”的常见成因（撞车/缺凭据）。
 * 拿不到就返回空数组（不抛：渠道面板不是关键路径，不该因它拖垮整个浮层）。
 */
export async function fetchChannelState(): Promise<NonNullable<PersonaStateView["channelState"]>> {
  try {
    const res = await fetch("/api/v1/channels/state")
    if (!res.ok) return []
    const body = (await res.json()) as { channels?: NonNullable<PersonaStateView["channelState"]> }
    return body.channels ?? []
  } catch {
    return []
  }
}

/** 拉取快照（404 = 拟人未启用或装配失败；装配失败时 body 带 setupError，透出给调用方；其它错误抛出）。 */
export async function fetchPersonaState(): Promise<PersonaStateView | null> {
  const res = await fetch("/api/v1/persona/state")
  if (res.status === 404) {
    // 404 body 可能携带装配失败原因（setupError）：不解析就丢，前端只能显示「未启用」，
    // 与「把真实原因接到状态接口」的设计意图相反（排障还得翻启动日志）。
    // 返回形态：带 setupError 的空快照（调用方据 setupError 渲染）；无则为 null（真未启用）。
    const body = (await res.json().catch(() => ({}))) as { setupError?: string }
    if (body.setupError) {
      return {
        enabled: false,
        generatedAt: Date.now(),
        personas: [],
        sources: [],
        sourceErrors: {},
        channelInput: {},
        // 装配失败也要看得到渠道（可能是渠道配置错导致装配失败）
        channelState: await fetchChannelState(),
        workers: [],
        credentials: null,
        setupError: body.setupError,
        totals: { drivers: 0, activeDrivers: 0, running: 0, queued: 0, failed: 0, workersRunning: 0 },
      }
    }
    return null
  }
  if (!res.ok) {
    const body = (await res.json().catch(() => ({}))) as { error?: string }
    throw new Error(body.error ?? `HTTP ${res.status}`)
  }
  return (await res.json()) as PersonaStateView
}

/**
 * 从失败响应里取出**能看的原因**。
 *
 * 为何不能只 `res.json().catch(() => ({}))` 然后 `body.error ?? "未知错误"`：
 * 服务端并非所有失败都回 JSON——Hono 自带 404/405 回 `text/plain`，代理层回 HTML，
 * 网络层失败连响应都没有。旧写法这三种情况全部归零成 `undefined`，界面只好说“未知错误”，
 * 而真正的原因（比如路由根本没注册 → 404）就完全看不出来了。
 * 所以：JSON 里取 error，非 JSON 时给出**状态码 + 响应片段**。
 */
async function readFailure(res: Response): Promise<string> {
  const text = await res.text().catch(() => "")
  try {
    const body = JSON.parse(text) as { error?: unknown }
    if (typeof body?.error === "string" && body.error.trim()) return body.error
  } catch {
    /* 非 JSON：继续走下方文本分支 */
  }
  const snippet = text.trim().replace(/\s+/g, " ").slice(0, 120)
  return snippet ? `HTTP ${res.status}：${snippet}` : `HTTP ${res.status} ${res.statusText}`.trim()
}

/** 手动启停一个事件源（诊断用）。 */
export async function controlPersonaSource(name: string, action: "start" | "stop"): Promise<{ ok: boolean; error?: string }> {
  const res = await fetch(`/api/v1/persona/sources/${encodeURIComponent(name)}/${action}`, { method: "POST" })
  if (!res.ok) return { ok: false, error: await readFailure(res) }
  const body = (await res.json().catch(() => ({}))) as { ok?: boolean; error?: string }
  return { ok: body.ok !== false, ...(body.error ? { error: body.error } : {}) }
}

/** 一条定时任务的控制面形状（含**已停用**的：界面要能把它们重新打开）。 */
export interface ScheduleControlRow {
  key: string
  personaId: string
  driverId: string
  scheduleId: string
  enabled: boolean
  cron: string
  description?: string
  timezone?: string
  lastRunAt?: number
}

/** 列全部定时任务（含已停用）。未装配/不可用时 `ok:false` + 原因（不抛错，面板照常渲染其余部分）。 */
export async function fetchPersonaSchedules(): Promise<{ ok: boolean; rows: ScheduleControlRow[]; error?: string }> {
  const res = await fetch("/api/v1/persona/schedules")
  if (!res.ok) {
    // 404 的两种成因必须区分开：一是控制面真的未装配（服务端 JSON 会说明），
    // 二是这条端点根本不存在（路由/代理层回的 text/plain 404）——后者是部署或路由缺陷，
    // 当成“未装配”会把一个真 bug 包装成正常状态。readFailure 两者都能说出真相。
    return { ok: false, rows: [], error: await readFailure(res) }
  }
  const body = (await res.json().catch(() => ({}))) as { schedules?: ScheduleControlRow[]; error?: string }
  return { ok: true, rows: Array.isArray(body.schedules) ? body.schedules : [] }
}

/** 开/关一个渠道实例的**监听**（只停收消息；只影响本次运行）。 */
export async function controlChannelListen(
  name: string,
  listen: boolean,
): Promise<{ ok: boolean; error?: string }> {
  const res = await fetch(`/api/v1/channels/${encodeURIComponent(name)}/listen/${listen ? "start" : "stop"}`, {
    method: "POST",
  })
  if (!res.ok) return { ok: false, error: await readFailure(res) }
  const body = (await res.json().catch(() => ({}))) as { ok?: boolean; error?: string }
  return { ok: body.ok !== false, ...(body.error ? { error: body.error } : {}) }
}

/** 开/关一条定时任务（持久）。 */
export async function setPersonaScheduleEnabled(
  key: string,
  enabled: boolean,
): Promise<{ ok: boolean; error?: string }> {
  const res = await fetch(`/api/v1/persona/schedules/${encodeURIComponent(key)}/${enabled ? "enable" : "disable"}`, {
    method: "POST",
  })
  if (!res.ok) return { ok: false, error: await readFailure(res) }
  const body = (await res.json().catch(() => ({}))) as { ok?: boolean; error?: string }
  return { ok: body.ok !== false, ...(body.error ? { error: body.error } : {}) }
}

/** 立即跑一次（不改变开关状态；停用状态下也可跑，用于验证配置）。 */
export async function runPersonaSchedule(key: string): Promise<{ ok: boolean; error?: string; eventId?: string }> {
  const res = await fetch(`/api/v1/persona/schedules/${encodeURIComponent(key)}/run`, { method: "POST" })
  if (!res.ok) return { ok: false, error: await readFailure(res) }
  const body = (await res.json().catch(() => ({}))) as { ok?: boolean; error?: string; eventId?: string }
  return {
    ok: body.ok !== false,
    ...(body.error ? { error: body.error } : {}),
    ...(body.eventId ? { eventId: body.eventId } : {}),
  }
}

/**
 * 发起飞书授权。
 *
 * **令牌请求走相期弹窗**（用户要求的主动通路）：服务端生成链接后顺手推一张
 * 带链接的授权卡到桌面常驻窗口。卡比网页提示难被忽略，而且授权本来就是
 * 「离开浏览器去飞书点同意」的动作——在你平时就在的窗口里告诉你，比在网页角落里写一行字合理。
 *
 * 返回的 `card.delivered=false` = 相期没连（卡已缓存，它启动后会重推），
 * 此时前端必须降级给一个可点的链接（否则用户被卡在一个“已发出但看不到”的状态）。
 *
 * 权限链路：这是**用户本人**的授权（user_access_token），与机器人应用凭据无关。
 */
export async function startPersonaAuthorize(): Promise<{
  ok: boolean
  url?: string
  error?: string
  card?: { delivered: boolean; error?: string }
}> {
  try {
    const res = await fetch("/api/v1/persona/authorize", { method: "POST" })
    const body = (await res.json().catch(() => ({}))) as {
      ok?: boolean
      url?: string
      error?: string
      card?: { delivered?: boolean; error?: string }
    }
    return {
      ok: res.ok && body.ok !== false && !!body.url,
      ...(body.url ? { url: body.url } : {}),
      ...(body.error ? { error: body.error } : {}),
      ...(body.card ? { card: { delivered: body.card.delivered === true, ...(body.card.error ? { error: body.card.error } : {}) } } : {}),
    }
  } catch (err) {
    return { ok: false, error: String((err as Error)?.message || err) }
  }
}

/**
 * 手动提交授权码（回调打不到歌白时的兜底）。
 *
 * 真实场景：redirect_uri 指向别处（如端口不对、被别的服务占用），
 * 浏览器跳转回不到歌白——此时用户把地址栏里带 code 的 URL 粘进来即可。
 *
 * 粘贴的通常是完整回调 URL：里面的 `state` 会一并提取提交（服务端按 state 对号
 * 兑换；多开授权页时靠 state 区分，不提取就会「兑错会话/被拒」）。
 */
export async function completePersonaAuthorize(raw: string): Promise<{ ok: boolean; error?: string }> {
  try {
    // 从粘贴的 URL 里提取 state（与 code 同源同段；只粘 code 时提取不到、不传）
    const stateMatch = /[?&]state=([^&\s]+)/.exec(raw)
    const state = stateMatch?.[1]
    const res = await fetch("/api/v1/persona/authorize/complete", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ code: raw, ...(state ? { state } : {}) }),
    })
    const body = (await res.json().catch(() => ({}))) as { ok?: boolean; error?: string }
    return { ok: res.ok && body.ok !== false, ...(body.error ? { error: body.error } : {}) }
  } catch (err) {
    return { ok: false, error: String((err as Error)?.message || err) }
  }
}

/** 一行状态徽标（`ok` / `warn` / `bad`）。 */
function badge(text: string, kind: "ok" | "warn" | "bad"): HTMLElement {
  return el("span", `persona-badge persona-badge-${kind}`, text)
}

function section(title: string): HTMLElement {
  const sec = el("div", "settings-section")
  sec.appendChild(el("div", "settings-section-title", title))
  return sec
}

/** 计数行（紧凑的 `k=v` 串：一眼扫完，不用读句子）。 */
function counters(...pairs: Array<[string, number]>): HTMLElement {
  return el("div", "persona-counters", pairs.map(([k, v]) => `${k} ${v}`).join(" · "))
}

/**
 * 标签页容器：返回「往哪个面板里放内容」。
 *
 * 为何分页而不是继续往下接：面板已经到 6 段——「找渠道开关」和「看人格状态」是
 * 两个不相关的事，却排在同一个长页面里。分页的判据是**它们服务哪个对象**：
 * - 人格页：这个人格怎么说话/怎么干活（身份、订阅、定时、派活、凭据）
 * - 通道页：消息从哪个口进来/从哪个口出去（实例、长连接、监听开关）
 *
 * 两页各自保留滚动位置（切换不重渲染），且**默认停在人格页**（那是主要查看对象）。
 */
function tabbedPanel(
  host: HTMLElement,
  tabs: Array<{ id: string; label: string; build: () => void }>,
): void {
  const bar = el("div", "persona-tabs")
  const body = el("div", "persona-tabs-body")
  const panes = new Map<string, HTMLElement>()
  const buttons = new Map<string, HTMLElement>()
  for (const t of tabs) {
    const btn = el("button", "persona-tab", t.label)
    btn.type = "button"
    btn.dataset.tab = t.id
    const pane = el("div", "persona-tab-pane")
    panes.set(t.id, pane)
    buttons.set(t.id, btn)
    btn.addEventListener("click", () => activate(t.id))
    bar.appendChild(btn)
    body.appendChild(pane)
  }
  const activate = (id: string): void => {
    for (const [tid, btn] of buttons) {
      const on = tid === id
      btn.classList.toggle("persona-tab-active", on)
      panes.get(tid)!.style.display = on ? "" : "none"
    }
  }
  host.append(bar, body)
  // 构建内容（每页只在首次激活时构建：避免一进面板就把所有请求都发出去）
  const built = new Set<string>()
  const ensureBuilt = (id: string): void => {
    if (built.has(id)) return
    built.add(id)
    tabs.find((t) => t.id === id)?.build()
  }
  // 先构建全量：内容都在本地快照里，构建成本是 DOM 拼装（无请求），预先建好让切页瞬时完成
  for (const t of tabs) ensureBuilt(t.id)
  activate(tabs[0]!.id)
}

/**
 * 渲染整个面板（append 到给定容器）。
 *
 * `onChanged` 在手动启停源之后调用：让调用方决定重渲染方式（面板自己不做全量刷新，
 * 免得把用户正看着的滚动位置与展开状态抹掉）。
 */
export function renderPersonaPanel(
  host: HTMLElement,
  snapshot: PersonaStateView,
  onChanged?: () => void,
  schedules?: ScheduleControlRow[],
  /** 拉取定时控制面失败的原因（有值时面板顶部如实说出来，而不是默默少一排开关）。 */
  scheduleError?: string,
): void {
  // 定时任务的启停状态（含**已停用项**）：不在 ssr 快照里（那里只列声明），
  // 由调用方一并拉好传进来。
  //
  // 为何不在本函数里 fetch：渲染是同步的，异步补数据只能靠「回来后重画」，
  // 而重画会再触发一次 fetch——一个自反馈的刷新环。取数交给调用方一次取全。
  // 两个标签页的容器：人格（怎么说话/怎么干活）与通道（消息从哪进/从哪出）。
  // 各段内容仍由下面原有代码构建，只是挂到对应页里而不是全部堆在 host 上。
  const personaPane = el("div", "persona-pane")
  const channelPane = el("div", "persona-pane")

  const scheduleState = new Map<string, ScheduleControlRow>()
  for (const row of schedules ?? []) scheduleState.set(row.key, row)

  // ── 1. 凭据 ──
  const cred = section("出站凭据")
  if (!snapshot.credentials) {
    cred.appendChild(blockLine("未配置飞书应用凭据：人格可以收事件、但**无法以你的名义发言**。"))
  } else if (snapshot.credentials.ready) {
    const row = el("div", "settings-row")
    const info = el("div", "settings-row-info")
    info.append(
      el("div", "settings-row-name", "飞书用户授权"),
      el("div", "settings-row-desc", snapshot.credentials.message),
    )
    row.append(info, badge("可用", "ok"))
    cred.appendChild(row)
  } else {
    const row = el("div", "settings-row")
    const info = el("div", "settings-row-info")
    info.append(
      el("div", "settings-row-name", "飞书用户授权不可用"),
      el(
        "div",
        "settings-row-desc",
        `${snapshot.credentials.message}\n（人格仍会处理事件，但对外发言会失败）`,
      ),
    )
    row.append(info, badge("不可用", "bad"))
    cred.appendChild(row)
    // 授权动作的主通路是**相期弹窗**（桌面常驻窗口推一张带链接的授权卡）：
    // 授权是个「离开浏览器去飞书点同意」的动作，而相期能把卡常驻在你眼前、
    // 完成后自动撤掉（见 compose.ts 的 syncAuthCard）。
    //
    // 这里保留的是两个**兼底入口**：相期没开/推送失败时，用户仍有路可走。
    cred.appendChild(authorizeActions(onChanged))
  }
  personaPane.appendChild(cred)

  // ── 2. 渠道（飞书应用与长连接）──
  //
  // 为何排在事件源之前、且必须是独立一段：人格“不回话”的首要嫌疑不是人格本身，而是
  // **渠道没起来**。而渠道最常见的坏法是一个协议硬约束：同一飞书应用只允许一条长连接，
  // 两个角色配了同一个 AppID 就会互踢——渠道侧直接拒启，错误只说在工具输出与日志里。
  // 所以这一段要把“哪个渠道、起没起来、为什么没起来、有没有建连”直接摆在脸上。
  const chState = snapshot.channelState
  if (chState && chState.length) {
    const achSec = section("飞书应用与本机渠道（输入 / 输出）")
    const list = el("div", "settings-list")
    for (const c of chState) {
      const row = el("div", "settings-row")
      const info = el("div", "settings-row-info")
      // 方向决定“什么算健康”：
      //   入站（in） 建长连接才算健康 → 看 listening
      //   出站（out）**本来就不建连接** → listening=false 正常，只看 started
      // 不区分就会把每个出站实例都标成“只能发不能收”，那是假告警。
      const isIn = c.dir === "in"
      const faces = [
        isIn ? "输入（收）" : "输出（发/读）",
        c.started ? "已启动" : "未启动",
        isIn
          ? c.listening
            ? "长连接已建"
            : "未建长连接"
          : "不建连接（纯请求-响应）",
      ].join(" · ")
      info.append(
        el("div", "settings-row-name", c.name),
        el("div", "settings-row-desc", `${faces}${c.description ? ` — ${c.description}` : ""}`),
      )
      row.appendChild(info)
      row.appendChild(
        c.error
          ? badge("不可用", "bad")
          : !c.enabled
            ? badge("已停用", "warn")
            : !c.started
              ? badge("未启动", "warn")
              : isIn && !c.listening
                ? badge("只能发不能收", "warn")
                : badge("正常", "ok"),
      )
      list.appendChild(row)
      // 监听开关：**只停收消息**，发/读照常（后端的语义就是只动 startListen/stopListen）。
      //
      // 为何需要它：“群太吵先别收”“重新授权前先掐断入站”“排查事件风暴”都是常见动作，
      // 而它们都不该逼人改 .env 重启（重启会中断正在跑的人格任务）。
      // 只影响**本次运行**：不写配置、不落盘，重启后回到配置里的状态。
      if (c.canListen) {
        const bar = el("div", "settings-row-actions")
        const btn = el("button", "mini-btn", c.listening ? "停监听" : "开监听")
        btn.addEventListener("click", async () => {
          btn.disabled = true
          const r = await controlChannelListen(c.name, !c.listening)
          btn.disabled = false
          if (!r.ok) {
            // 失败要看得见：静默不动会让人以为已经改好了
            btn.textContent = "失败"
            bar.appendChild(el("div", "settings-row-desc persona-error", r.error ?? "未知错误"))
            return
          }
          onChanged?.()
        })
        bar.appendChild(btn)
        bar.appendChild(
          el("div", "settings-row-desc", "只停收消息（发送/读历史不受影响）；仅本次运行有效"),
        )
        list.appendChild(bar)
      } else if (isIn) {
        // 入站实例却没有 canListen（渠道类型没实现 startListen）：不默默少一个按钮
        list.appendChild(el("div", "settings-row-desc", "该渠道类型不支持手动启停监听"))
      }
      // 失败原因原文照排：长（含修复指引），但它是唯一能直接照着改的东西，不放折叠。
      if (c.error) list.appendChild(blockLine(`原因：${c.error}`))
    }
    achSec.appendChild(list)
    achSec.appendChild(
      el(
        "div",
        "settings-row-desc",
        "同一飞书应用只允许一条长连接：所以两个**输入**实例（以及会话机器人）必须各用独立应用。" +
          "**输出**实例不建连接，因此可以与输入实例/会话机器人共用同一应用。" +
          "角色对照与配置形态见 docs/feishu-apps.md。",
      ),
    )
    channelPane.appendChild(achSec)
  }

  // ── 3. 事件源 ──
  const srcSec = section("事件源（驱动面）")
  if (snapshot.sources.length === 0) {
    srcSec.appendChild(
      blockLine(
        "没有任何事件源。人群里来的消息走渠道入站，但定时 / webhook / 文件投递这些驱动源需要源插件——\n" +
          "放一个文件到 custom/persona/ 即注册（见 packages/persona/docs/event-sources.md）。",
      ),
    )
  } else {
    const list = el("div", "settings-list")
    for (const s of snapshot.sources) {
      const row = el("div", "settings-row")
      const info = el("div", "settings-row-info")
      const healthy = s.started && !s.error
      info.append(
        el("div", "settings-row-name", `${s.name} · ${s.channel || "（无 channel）"}`),
        el("div", "settings-row-desc", s.description || "（无说明）"),
        counters(
          ["收", s.stats.emitted],
          ["接管", s.stats.accepted],
          ["无人订阅", s.stats.unattended],
          ["重复", s.stats.duplicate],
          ["溢出", s.stats.overflow],
          ["不可用", s.stats.unavailable],
        ),
      )
      if (s.error) info.appendChild(el("div", "settings-row-desc persona-error", s.error))
      row.appendChild(info)
      // 无人订阅是**订阅配置错**的信号，不是源坏了——徽标文案要说清楚区别
      if (s.stats.unattended > 0 && healthy) row.appendChild(badge("有人收但无人订阅", "warn"))
      else row.appendChild(badge(healthy ? "运行中" : s.started ? "启动后有错" : "未运行", healthy ? "ok" : "bad"))
      // 手动启停（诊断）：不必改环境变量重启服务
      const btn = el("button", "mini-btn", s.started ? "停" : "启动")
      btn.onclick = async () => {
        btn.disabled = true
        const r = await controlPersonaSource(s.name, s.started ? "stop" : "start")
        btn.disabled = false
        if (!r.ok) {
          btn.textContent = "失败"
          if (r.error) info.appendChild(el("div", "settings-row-desc persona-error", r.error))
          return
        }
        onChanged?.()
      }
      row.appendChild(btn)
      list.appendChild(row)
    }
    srcSec.appendChild(list)
  }
  const srcErrNames = Object.keys(snapshot.sourceErrors)
  if (srcErrNames.length) {
    srcSec.appendChild(el("div", "settings-row-desc persona-error", `加载/启动失败：${srcErrNames.join("、")}`))
    for (const [name, err] of Object.entries(snapshot.sourceErrors)) {
      srcSec.appendChild(el("div", "settings-row-desc persona-error", `· ${name}：${err}`))
    }
  } else if (snapshot.sources.length > 0) {
    srcSec.appendChild(el("div", "settings-row-desc", "全部源加载正常（没有加载失败记录）"))
  }
  personaPane.appendChild(srcSec)

  // ── 4. 渠道入站（计数）──
  const chNames = Object.keys(snapshot.channelInput)
  if (chNames.length) {
    const chSec = section("渠道入站（人群来的消息）")
    const list = el("div", "settings-list")
    for (const name of chNames) {
      const c = snapshot.channelInput[name]!
      const row = el("div", "settings-row")
      const info = el("div", "settings-row-info")
      info.append(
        el("div", "settings-row-name", name),
        counters(
          ["收", c.emitted],
          ["接管", c.accepted],
          ["无人订阅", c.unrouted],
          ["重复", c.duplicate],
          ["溢出", c.overflow],
          ["不可用", c.unavailable],
        ),
      )
      row.appendChild(info)
      row.appendChild(badge(c.unrouted > 0 ? "有事件没人认领" : "正常", c.unrouted > 0 ? "warn" : "ok"))
      list.appendChild(row)
    }
    chSec.appendChild(list)
    chSec.appendChild(
      el(
        "div",
        "settings-row-desc",
        "「无人订阅」持续增长 = 该渠道没被任何人格的订阅覆盖（改人格定义的 subscriptions，不是渠道坏了）。",
      ),
    )
    channelPane.appendChild(chSec)
  }

  // ── 5. 人格与分身 ──
  const pSec = section(`人格与分身（${snapshot.totals.drivers} 个分身，${snapshot.totals.activeDrivers} 个已通电）`)
  const pList = el("div", "settings-list")
  for (const p of snapshot.personas) {
    const head = el("div", "settings-row")
    const hinfo = el("div", "settings-row-info")
    hinfo.append(
      el("div", "settings-row-name", `${p.name}（${p.id}）· ${p.role}`),
      el("div", "settings-row-desc", p.description ?? ""),
    )
    head.appendChild(hinfo)
    pList.appendChild(head)

    for (const d of p.drivers) {
      const row = el("div", "settings-row persona-driver")
      const info = el("div", "settings-row-info")
      const bits: string[] = [`订阅 ${d.subscriptions.join("/")}`, d.origin === "dynamic" ? "自建" : "静态"]
      if (d.maxConcurrent !== undefined) bits.push(`并发 ${d.maxConcurrent}`)
      if (d.sessionId) bits.push(`会话 ${d.sessionId.slice(0, 8)}`)
      else bits.push("尚无会话（还没接过事件）")
      if (d.schedules?.length) bits.push(`定时 ${d.schedules.length} 条`)
      info.append(
        el("div", "settings-row-name", `${d.id} — ${d.focus}`),
        el("div", "settings-row-desc", bits.join(" · ")),
        counters(
          ["接管", d.accepted],
          ["排队", d.queued],
          ["在跑", d.running],
          ["去重", d.deduped],
          ["溢出", d.overflowed],
          ["失败", d.failed],
        ),
      )
      // 定时任务逐条列出 + 开关 + 「立即跑一次」。
      //
      // 为何放在面板里而不是只留着改代码：定时是**静默的**（不触发时不报错），
      // 而「今天先别跑这条」「跑一次看看配置对不对」是两个每天都会出现的念头，
      // 不该为了它们改代码重启。
      //
      // 启停状态与声明是两份数据：声明在 snapshot（只含启用的），开关在 /schedules
      // （含停用的）。所以在这里对一次 key，单位对不上的就不渲染开关（宁可不给按钮，
      // 也不给一个点了没用的按钮）。
      //
      // 拉不到控制面时把原因写在人群标题下：少一排开关本身很容易被当成「本来就没有」，
      // 而那通常是端点在 404（见 route-guard.ts 的「静默失效」）——不说出来就没人会查。
      if (scheduleError && (d.schedules?.length ?? 0) > 0) {
        info.appendChild(el("div", "persona-pop-error", `定时开关不可用：${scheduleError}`))
      }
      for (const s of d.schedules ?? []) {
        const tz = s.timezone ? `（${s.timezone}）` : ""
        const key = `${p.id}/${d.id}/${s.id}`
        const state = scheduleState.get(key)
        const line = el("div", "settings-row-desc schedule-row")
        // 停用的也照样列（否则就没有地方把它重新打开了）
        const on = state ? state.enabled : true
        line.appendChild(el("span", on ? "schedule-dot on" : "schedule-dot off", on ? "●" : "○"))
        line.appendChild(el("span", "", ` ${s.description ?? s.id}：${s.cron}${tz}`))
        if (state?.lastRunAt) {
          line.appendChild(el("span", "schedule-meta", ` · 上次 ${new Date(state.lastRunAt).toLocaleString()}`))
        }
        if (!on) line.appendChild(el("span", "schedule-meta", " · 已停用"))

        // 开关：只在控制面确实认得这个 key 时给（见上）
        if (state) {
          const toggle = el("button", "mini-btn", on ? "停用" : "启用")
          toggle.addEventListener("click", async () => {
            toggle.disabled = true
            const r = await setPersonaScheduleEnabled(key, !on)
            if (!r.ok) {
              toggle.disabled = false
              // 失败得说出来：静默不动会让人以为已经改好了。
              // r.ok=false 时 r.error 恒有值（setPersonaScheduleEnabled 已保证：
              // 非 2xx 一律经 readFailure 取出状态码/服务端原因），``未知错误`` 只是兵底。
              alert(`定时任务${on ? "停用" : "启用"}失败：${r.error ?? "未知错误"}`)
              return
            }
            onChanged?.()
          })
          line.appendChild(toggle)

          // 立即跑一次：**停用状态下也给**——「先看看效果，再决定以后要不要一直跑」
          // 是最自然的使用顺序，强制先启用会把这条路堵死。
          const runBtn = el("button", "mini-btn", "立即跑一次")
          runBtn.addEventListener("click", async () => {
            runBtn.disabled = true
            runBtn.textContent = "已投递…"
            const r = await runPersonaSchedule(key)
            if (!r.ok) {
              runBtn.disabled = false
              runBtn.textContent = "立即跑一次"
              alert(`触发失败：${r.error ?? "未知错误"}`)
              return
            }
            // 投递成功不等于跑完（引擎异步）：题面写「已投递」，刷新交给 onChanged
            runBtn.textContent = "已投递"
            setTimeout(() => onChanged?.(), 1200)
          })
          line.appendChild(runBtn)
        }
        info.appendChild(line)
      }
      if (d.reason) info.appendChild(el("div", "settings-row-desc", `创建原因：${d.reason}`))
      row.appendChild(info)
      // 会话在跑 = 最可信的「正在干活」信号（调度计数会滞后）
      row.appendChild(
        d.sessionRunning ? badge("正在跑", "ok") : d.active ? badge("待命中", "ok") : badge("未通电", "warn"),
      )
      pList.appendChild(row)
    }
  }
  if (snapshot.totals.failed > 0) {
    pSec.appendChild(el("div", "settings-row-desc persona-error", `累计失败 ${snapshot.totals.failed} 次（看上面的分身分解）`))
  }
  pSec.appendChild(pList)
  pSec.appendChild(
    el(
      "div",
      "settings-row-desc",
      "「未通电」= 还没发生过投递（调度器惰性创建）。若长期如此，先看事件源与渠道入站两项。",
    ),
  )
  personaPane.appendChild(pSec)

  // ── 6. 派活（driver 派出去的独立工作会话）──
  // 五面里唯一原先无 UI 的一环：后端快照一直有 workers，不渲染就成了盲区——
  // 「人格说查了但没结果」时，这里能直接看到活是 running/failed/cancelled。
  const running = snapshot.workers.filter((w) => w.status === "running").length
  const wSec = section(`派活（在跑 ${running}）`)
  if (!snapshot.workers.length) {
    wSec.appendChild(blockLine("还没有派过活。driver 週到耗时任务时会用 worker_dispatch 派出去，结论会作为事件回到它。"))
  } else {
    const wList = el("div", "settings-list")
    for (const w of snapshot.workers.slice(0, 20)) {
      const row = el("div", "settings-row")
      const info = el("div", "settings-row-info")
      const seconds = w.elapsedMs !== undefined ? `${Math.round(w.elapsedMs / 1000)}s` : `${Math.max(0, Math.round((Date.now() - w.startedAt) / 1000))}s`
      info.append(
        el("div", "settings-row-name", `${w.label ?? w.runId} · ${w.personaId}/${w.driverId}`),
        el("div", "settings-row-desc", `${w.status} · ${seconds} · ${new Date(w.startedAt).toLocaleTimeString("zh-CN")}`),
      )
      row.appendChild(info)
      row.appendChild(
        w.status === "running" ? badge("在跑", "ok") : w.status === "done" ? badge("完成", "ok") : w.status === "cancelled" ? badge("已叫停", "warn") : badge("失败", "bad"),
      )
      wList.appendChild(row)
    }
    wSec.appendChild(wList)
  }
  personaPane.appendChild(wSec)

  // 把两页挂上（页面顺序：人格在前——那是主要查看对象）
  tabbedPanel(host, [
    { id: "persona", label: "人格", build: () => host.appendChild(personaPane) },
    { id: "channel", label: "通道", build: () => host.appendChild(channelPane) },
  ])
}

function blockLine(text: string): HTMLElement {
  const d = el("div", "settings-row-desc persona-block")
  d.textContent = text
  return d
}

/**
 * 授权操作区：去授权 / 粘贴回调地址。
 *
 * **主通路是相期弹窗**：点「去授权」服务端会生成链接并**同时推一张授权卡到桌面窗口**，
 * 用户直接在卡上点就能走。相期没连时（`delivered=false`）才降级为这里开新页 + 文案。
 *
 * 三个入口各管一段：
 * - 相期卡：主通路（常驻窗口，难被忽略，完成后自动撤卡）
 * - 新页打开：相期没连/用户就在电脑前直接点
 * - 粘贴回调地址：redirect_uri 未在飞书后台登记时，浏览器跳转回不到歌白（否则用户卡在“授权完什么都没发生”）
 */
function authorizeActions(onChanged?: () => void): HTMLElement {
  const wrap = el("div", "persona-auth-actions-inline")
  const msg = el("div", "settings-row-desc")
  const go = el("button", "mini-btn", "去授权")
  const paste = el("button", "mini-btn", "粘贴回调地址")

  go.onclick = async () => {
    go.disabled = true
    msg.textContent = "正在生成授权链接并推送到相期…"
    const r = await startPersonaAuthorize()
    go.disabled = false
    if (!r.ok || !r.url) {
      msg.textContent = `生成授权链接失败：${r.error ?? "未知原因"}`
      return
    }
    // 相期卡已送达：**不新开标签页**——那样会同时弹出两个授权入口，
    // 用户可能在两处都点一遍（第二次的 code 已经用过了，会看到一个莫名其妙的失败）。
    if (r.card?.delivered) {
      msg.textContent =
        "已把授权卡推到桌面上的相期窗口，去那里点「去授权」即可。完成后卡会自动消失。\n" +
        "若没看到相期窗口，可重新点本按钮——相期没开时会把链接直接开在新窗口。"
      return
    }
    // 相期不可用：降级为直接开新页，并如实告知链路
    window.open(r.url, "_blank", "noopener,noreferrer")
    msg.textContent =
      `相期未被送达${r.card?.error ? `（${r.card.error}）` : "（未连接）"}，已在新窗口打开授权页。\n` +
      "完成后重新打开本面板即可看到新状态；若飞书报「重定向 URL 不合法」，" +
      "说明后台未登记当前回调地址，可改用下方「粘贴回调地址」。"
  }

  paste.onclick = async () => {
    const raw = prompt("把浏览器地址栏里带 code= 的完整回调地址粘贴到这里（或只粘贴 code）：")
    if (!raw?.trim()) return
    msg.textContent = "正在兑换…"
    const r = await completePersonaAuthorize(raw.trim())
    if (!r.ok) {
      msg.textContent = `兑换失败：${r.error ?? "未知原因"}`
      return
    }
    msg.textContent = "授权完成。"
    onChanged?.()
  }

  wrap.append(go, paste, msg)
  return wrap
}
