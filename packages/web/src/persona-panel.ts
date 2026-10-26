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
  credentials: { ready: boolean; state: string; message: string; remainingMs?: number; needsAuth: boolean } | null
  /** 拟人装配失败原因（服务仍正常运行；有值 = 开关开了但坏了）。 */
  setupError?: string
  totals: { drivers: number; activeDrivers: number; running: number; queued: number; failed: number }
}

/** 拉取快照（404 = 拟人模式未启用，返回 null；其它错误抛出）。 */
export async function fetchPersonaState(): Promise<PersonaStateView | null> {
  const res = await fetch("/api/v1/persona/state")
  if (res.status === 404) return null
  if (!res.ok) {
    const body = (await res.json().catch(() => ({}))) as { error?: string }
    throw new Error(body.error ?? `HTTP ${res.status}`)
  }
  return (await res.json()) as PersonaStateView
}

/** 手动启停一个事件源（诊断用）。 */
export async function controlPersonaSource(name: string, action: "start" | "stop"): Promise<{ ok: boolean; error?: string }> {
  const res = await fetch(`/api/v1/persona/sources/${encodeURIComponent(name)}/${action}`, { method: "POST" })
  const body = (await res.json().catch(() => ({}))) as { ok?: boolean; error?: string }
  return { ok: res.ok && body.ok !== false, ...(body.error ? { error: body.error } : {}) }
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
 */
export async function completePersonaAuthorize(raw: string): Promise<{ ok: boolean; error?: string }> {
  try {
    const res = await fetch("/api/v1/persona/authorize/complete", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ code: raw }),
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
 * 渲染整个面板（append 到给定容器）。
 *
 * `onChanged` 在手动启停源之后调用：让调用方决定重渲染方式（面板自己不做全量刷新，
 * 免得把用户正看着的滚动位置与展开状态抹掉）。
 */
export function renderPersonaPanel(host: HTMLElement, snapshot: PersonaStateView, onChanged?: () => void): void {
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
  host.appendChild(cred)

  // ── 2. 事件源 ──
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
  host.appendChild(srcSec)

  // ── 3. 渠道入站 ──
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
    host.appendChild(chSec)
  }

  // ── 4. 人格与分身 ──
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
  host.appendChild(pSec)
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
