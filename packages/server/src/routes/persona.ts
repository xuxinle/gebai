/**
 * 拟人模式的凭据路由：状态查询 / 发起授权 / 回调兑换 / 撤销。
 *
 * ## 为什么需要独立的回调端点
 *
 * 既有的 `/api/v1/oauth/feishu/callback` 是**会话级**的（把 token 写回发起授权的那个
 * 对话目录）。拟人模式的凭据是**进程级**的（人格跨会话常驻），两处的归属不同，
 * 共用一个端点会让「令牌到底写给了谁」变得模糊。
 *
 * ## 为什么状态查询要能自动生成授权链接
 *
 * 用户的心智是「我点一下就能修好」。若状态接口只回 `needsAuth: true` 而不给链接，
 * 前端就得再发一次请求——多一次往返、还多一处状态（链接什么时候生成）。
 * 让状态接口惰性生成链接，前端拿到即可直接渲染按钮。
 *
 * ## 安全
 *
 * - 状态接口**不返回任何令牌明文**（只给「授权人是谁、何时过期」）
 * - 回调是公开端点（免鉴权）：`state` 即一次性凭证（随机不可猜，兑换后作废）
 * - 回调返回 HTML 页面（浏览器跳转场景），不做 JSON——用户看到的是网页
 */

import type { Context, Hono } from "hono"
import type { AppEnv } from "../app"
import type { PersonaCredentials } from "@gebai/persona"
import { isOfflineAccessRejection } from "@gebai/persona"
import type { PersonaStateSnapshot } from "../core/session/persona-status"

/**
 * 人格定时任务控制面（路由依赖形状）。
 *
 * 为何走控制面而不是停整个 source：一个源管全部人格的全部定时（包括 `*`/`3` 这种高频档期），
 * 停源等于把别人的定时一起停掉。以「声明」为粒度才是对的口径。
 *
 * `run` 不要求先启用：手动跑一次是「验证配置对不对」的手段，
 * 如果必须先开，那“想看一眼效果又不想让它以后一直跑”就没有办法表达。
 */
export interface ScheduleControlApi {
  /** 列当前全部声明（含已停用的——否则界面无法把它们列出来重新打开）。 */
  list(): Promise<
    Array<{
      key: string
      personaId: string
      driverId: string
      scheduleId: string
      enabled: boolean
      cron: string
      description?: string
      timezone?: string
      lastRunAt?: number
    }>
  >
  /** 改开关（持久）。 */
  setEnabled(key: string, enabled: boolean): Promise<{ ok: boolean; error?: string }>
  /** 手动触发一次（不改变启用状态）。 */
  run(key: string): Promise<{ ok: boolean; error?: string; eventId?: string }>
}

export interface PersonaRouteDeps {
  /** 凭据编排（拟人模式未启用时为 null → 全部端点返回「未启用」）。 */
  credentials: PersonaCredentials | null
  /** 事件总线（授权完成/失败通知前端刷新状态）。 */
  publish: (type: string, payload: Record<string, unknown>) => void
  /** 用于拼默认回调地址（`GEBAI_PUBLIC_URL` → `http://localhost:{GEBAI_PORT}`）。 */
  publicBase: () => string
  /**
   * 运行态快照（人格/分身/事件源/入站计数/凭据汇总）。
   *
   * 未启用拟人模式时为缺省（端点 404）——与 `credentials` 同一开关语义：
   * 关闭时**不实例化任何 persona 组件**，所以这里也不该给定一个假快照。
   */
  state?: () => Promise<PersonaStateSnapshot>
  /** 单个事件源的启停（诊断用：临时打开一个源看它能不能收到东西，不必改配置重启）。 */
  sourceControl?: {
    start(name: string): Promise<boolean>
    stop(name: string): Promise<boolean>
  }
  /**
   * 人格定时任务的启停与手动运行。
   *
   * 与 `sourceControl` 是两个粒度：那个是「整个事件源通不通电」（诊断用），
   * 这个是「某一条定时现在允不允许跑」（日常操作）。
   */
  scheduleControl?: ScheduleControlApi
  /**
   * 拟人装配失败原因（未失败时 undefined）。
   *
   * 装配失败**不阻断启动**（见 compose）：代价是失败被“藏”起来了——用户只看到“人格没反应”。
   * 把真实原因回给前端，才不用去翻启动日志猜。
   */
  setupError?: string
  /**
   * 把授权卡推到相期桌面窗口（令牌请求的主通路）。
   *
   * 为何需要这个端点：相期推送原先只在启动时和 5 分钟定时器里做，
   * 用户“现在就想让它弹出来”时没有任何入口。授权是个需要人动手的动作，
   * 而相期是**常驻桌面窗口**——它比任何网页面板都难被忽略。
   *
   * 返回 delivered=false 时说明相期没连（卡已缓存，它启动后会重推），
   * 同时**把链接一并给前端**，让面板能降级提供一个可点的链接。
   */
  pushAuthCard?: () => Promise<{ ok: boolean; delivered: boolean; url?: string; error?: string }>
  /**
   * 写面管理员校验（服务模式）：非 admin 返回 403。
   *
   * 拟人凭据与事件源是**部署级**资源（进程级、跨用户共享）——普通登录用户不应能
   * 撤销全局授权（令所有人格失声）或停掉全局驱动源。未注入时不拦截（本地模式/单测）。
   */
  requireAdmin?: (c: Context) => Response | null
  /**
   * 渠道启停与失败原因（**不依赖拟人是否启用**）。
   *
   * 为何单独一个口子：渠道状态与人格运行时是两件独立的事——渠道在先，人格在其上。
   * 把渠道状态只放在 `/persona/state` 里，会造成“没配人格 → 什么都看不到”（包括
   * “渠道因为撞车没起来”这种与人格无关的故障）。未注入时为 undefined（客户端自行降级）。
   */
  channelState?: () => Array<{
    name: string
    description: string
    started: boolean
    error?: string
    listening?: boolean
    dir?: "in" | "out"
    canListen?: boolean
  }>
  /**
   * 启停单个渠道实例的**监听**（不动发送/读取能力）。
   *
   * 返回 `{ ok:false, reason }` 而不是抛错：失败原因（如“这是出站实例，没有监听可启停”）
   * 要能直接展示给用户，而不是变成一个 500。
   */
  channelListenControl?: (name: string, want: boolean) => Promise<{ ok: boolean; reason?: string }>
}

/** HTML 转义（反射型 XSS 防护：错误消息可能含外部可控内容）。 */
function esc(s: string): string {
  return s.replace(/[&<>"']/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[ch] ?? ch)
}

/** 结果页（浏览器跳转场景：用户看到的是网页，不是 JSON）。 */
function page(title: string, body: string, ok: boolean): Response {
  const html = `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><title>${esc(title)}</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;background:#f5f5f7;color:#333}
.card{text-align:center;background:#fff;padding:36px 48px;border-radius:12px;box-shadow:0 4px 20px rgba(0,0,0,.08);max-width:560px}
.icon{font-size:44px}.title{font-size:20px;font-weight:600;margin:12px 0 8px}
.detail{color:#666;font-size:14px;line-height:1.7;word-break:break-all}
.err{color:#c0392b}
</style></head><body><div class="card"><div class="icon">${ok ? "✅" : "❌"}</div>
<div class="title">${esc(title)}</div><div class="detail ${ok ? "" : "err"}">${body}</div>
<script>setTimeout(function(){location.href="/"},1800)</script></div></body></html>`
  return new Response(html, { status: 200, headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-cache" } })
}

export function registerPersonaRoutes(app: Hono<AppEnv>, deps: PersonaRouteDeps): void {
  // 未装配/装配失败：把**真实原因**一并返回。
  //
  // 装配失败已被隔离成「服务照常启动」，但这意味着用户看到的只是「人格不回消息」——
  // 如果没有这一行，他就只能去翻启动日志。两种情形语义不同，不能混为一谈：
  //   · 未配置——没写任何 GEBAI_PERSONA_<ID>，没打算跑人格
  //   · 装配失败——配了但坏了，**这就是用户最需要看到的那句话**
  const notEnabled = (c: Context) =>
    c.json(
      {
        error: deps.setupError
          ? `拟人模式装配失败：${deps.setupError}`
          : "拟人模式未启用（没有任何 GEBAI_PERSONA_<ID> 配置）",
        ...(deps.setupError ? { setupError: deps.setupError } : {}),
      },
      404,
    )

  /**
   * 当前凭据状态（前端横幅据此决定是否提示授权）。
   *
   * 返回里的 `authorizeUrl` 是**惰性生成**的：只有在需要授权时才有值。
   */
  app.get("/api/v1/persona/status", async (c) => {
    if (!deps.credentials) return notEnabled(c)
    const status = await deps.credentials.status()
    return c.json(status)
  })

  /** 重新发起授权（生成新链接；旧链接随之失效）。写面：仅管理员。 */
  app.post("/api/v1/persona/authorize", async (c) => {
    if (deps.requireAdmin) {
      const denied = deps.requireAdmin(c)
      if (denied) return denied
    }
    if (!deps.credentials) return notEnabled(c)
    try {
      const ticket = deps.credentials.startAuthorize()
      // 生成新链接后**顺手推一张相期卡**：用户意图很明确（“我要授权”），
      // 不该让他再去桌面上找。推送失败不影响返回链接（前端仍有兵底入口）。
      let card: { delivered: boolean; error?: string } | null = null
      if (deps.pushAuthCard) {
        try {
          const r = await deps.pushAuthCard()
          card = { delivered: r.delivered, ...(r.error ? { error: r.error } : {}) }
        } catch (err) {
          card = { delivered: false, error: String((err as Error)?.message || err) }
        }
      }
      return c.json({
        ok: true,
        url: ticket.url,
        scopes: ticket.scopes,
        expiresInMs: ticket.expiresInMs,
        ...(card ? { card } : {}),
      })
    } catch (err) {
      return c.json({ ok: false, error: String((err as Error)?.message || err) }, 400)
    }
  })

  /**
   * 手动提交授权码（回调打不到歌白时的兜底）。
   *
   * 真实场景：用户配的 redirect_uri 指向别处（如 `http://localhost` 无端口），
   * 浏览器的跳转落到别的服务上——此时用户可以把地址栏里带 code 的 URL 粘进来。
   */
  app.post("/api/v1/persona/authorize/complete", async (c) => {
    if (deps.requireAdmin) {
      const denied = deps.requireAdmin(c)
      if (denied) return denied
    }
    if (!deps.credentials) return notEnabled(c)
    const body = await c.req.json<{ code?: string; state?: string }>().catch(() => ({}) as { code?: string; state?: string })
    const code = String(body.code ?? "").trim()
    if (!code) return c.json({ ok: false, error: "缺少 code（粘贴授权后的完整回调地址或 code）" }, 400)
    try {
      const status = await deps.credentials.complete(code, body.state)
      deps.publish("event.persona.credentials", { ready: status.ready, state: status.state })
      return c.json({ ok: true, status })
    } catch (err) {
      const message = String((err as Error)?.message || err)
      deps.publish("event.persona.credentials", { ready: false, state: "error", message })
      return c.json({ ok: false, error: message }, 400)
    }
  })

  /** 撤销授权（清除落盘令牌，人格将停止对外动作并重新提示授权）。写面：仅管理员。 */
  app.delete("/api/v1/persona/authorize", async (c) => {
    if (deps.requireAdmin) {
      const denied = deps.requireAdmin(c)
      if (denied) return denied
    }
    if (!deps.credentials) return notEnabled(c)
    await deps.credentials.revoke()
    deps.publish("event.persona.credentials", { ready: false, state: "missing" })
    return c.json({ ok: true })
  })

  /**
   * 飞书授权回调（**公开端点**，免鉴权）。
   *
   * `state` 即会话关联凭证（随机、一次性、10 分钟 TTL）：兑换成功/失败后都作废，
   * 防重放。没有 state 也要能兑换（用户可能手动开了只带 code 的链接），
   * 此时用最近一次 pending 的 redirect_uri——否则飞书会因 redirect_uri 不匹配拒绝。
   */
  app.get("/api/v1/persona/feishu/callback", async (c) => {
    if (!deps.credentials) return page("拟人模式未启用", "请先配置 GEBAI_PERSONA_<人格ID> 与渠道实例，然后重启歌白。", false)
    const { code = "", state = "", error = "", error_description = "" } = c.req.query()
    if (error) {
      // 飞书在**跳转授权页前**就把应用没开通 offline_access 挡回来了（20027）。
      // 部署方不该为了这个去查后台权限，所以这里自己退一步：
      // 换一张不带 offline_access 的短期授权链接，直接把人送过去（用户几乎无感）。
      if (isOfflineAccessRejection(error) || isOfflineAccessRejection(error_description)) {
        const ticket = deps.credentials.downgradeToShortLived?.()
        if (ticket) {
          deps.publish("event.persona.credentials", {
            ready: false,
            state: "short_lived",
            message: "应用未开通长期授权，已改为短期授权",
          })
          // 302 自动跳转：用户不需再回面板点一次
          return c.redirect(ticket.url, 302)
        }
      }
      return page("飞书授权失败", `飞书返回错误：${esc(String(error_description || error))}`, false)
    }
    if (!code) {
      return page("授权参数缺失", "回调地址里没有 code。请回到歌白重新发起授权，或把完整回调地址粘贴到设置页。", false)
    }
    try {
      const status = await deps.credentials.complete(String(code), state ? String(state) : undefined)
      deps.publish("event.persona.credentials", { ready: status.ready, state: status.state, user: status.authorizedUser ?? "" })
      return page(
        "飞书授权成功",
        `人格已获得飞书访问权限${status.authorizedUser ? `（${esc(status.authorizedUser)}）` : ""}，可以开始处理消息了。`,
        true,
      )
    } catch (err) {
      const message = String((err as Error)?.message || err)
      deps.publish("event.persona.credentials", { ready: false, state: "error", message })
      return page("飞书授权失败", esc(message), false)
    }
  })

  /** 默认回调地址（供前端展示「需要在飞书后台登记哪个地址」）。 */
  app.get("/api/v1/persona/callback-url", (c) => {
    if (!deps.credentials) return notEnabled(c)
    const url = `${deps.publicBase()}/api/v1/persona/feishu/callback`
    return c.json({ url })
  })

  /**
   * 运行态快照：人格 / 分身 / 事件源 / 入站计数 / 凭据。
   *
   * 为什么合成一个端点而不是拆四个：这四项要**一起看**才成立——
   * 「人格已配置但事件源没启动」和「事件源在收但没人订阅（unrouted）」是两种完全不同的故障，
   * 分四次请求拿数据后在前端拼，必然出现判对一处漏一处。
   */
  app.get("/api/v1/persona/state", async (c) => {
    if (!deps.credentials || !deps.state) return notEnabled(c)
    try {
      return c.json(await deps.state())
    } catch (err) {
      return c.json({ error: `运行态快照生成失败：${String((err as Error)?.message || err)}` }, 500)
    }
  })

  /**
   * 渠道启停状态（**不受拟人开关影响**）。
   *
   * 为何需要这个独立端点：渠道与人格是两层东西（渠道先起、人格在其上），而
   * “渠道因为撞车没起来”与“人格没配”是两回事。若只把渠道状态塞在
   * `/persona/state` 里，没配人格时那个端点 404，面板就整块黑屏——
   * 而这恰恰是用户最想查“它为什么不说话”的时候。
   *
   * 只读、无副作用；未注入渠道注册表时如实回 404（客户端自行降级）。
   */
  app.get("/api/v1/channels/state", (c) => {
    if (!deps.channelState) return c.json({ error: "渠道状态不可用（此宿主未注入渠道注册表）" }, 404)
    try {
      return c.json({ channels: deps.channelState() })
    } catch (err) {
      return c.json({ error: `渠道状态读取失败：${String((err as Error)?.message || err)}` }, 500)
    }
  })

  /**
   * 手动启停**单个渠道实例的监听**（收消息）。
   *
   * 语义（刻意窄）：
   * - **只停监听**：发送/读取能力不受影响——否则“先别收消息”就无法表达，
   *   而那是真实需求（群太吵、要先重新授权、排查事件风暴）
   * - **只影响本次运行**：不写配置、不落盘；重启后回到配置里的状态
   * - 只对**入站实例**有意义：出站实例本来就不建连接，返回 400 并说明原因
   *   （静默成功会让用户以为“点了开关它就会收”，而实际什么都没发生）
   *
   * 需管理员：这是部署级资源（进程级、跨用户共享）。
   */
  app.post("/api/v1/channels/:name/listen/:action", async (c) => {
    if (deps.requireAdmin) {
      const denied = deps.requireAdmin(c)
      if (denied) return denied
    }
    if (!deps.channelListenControl) {
      return c.json({ ok: false, error: "渠道监听控制不可用（此宿主未注入渠道注册表）" }, 404)
    }
    const action = String(c.req.param("action") ?? "").toLowerCase()
    if (action !== "start" && action !== "stop") {
      return c.json({ ok: false, error: `未知动作「${action}」（可用：start / stop）` }, 400)
    }
    const name = decodeURIComponent(String(c.req.param("name") ?? ""))
    try {
      const r = await deps.channelListenControl(name, action === "start")
      if (!r.ok) return c.json({ ok: false, name, action, error: r.reason }, 400)
      deps.publish("event.channel.listen", { name, listening: action === "start" })
      return c.json({ ok: true, name, action, listening: action === "start" })
    } catch (err) {
      return c.json({ ok: false, name, action, error: String((err as Error)?.message || err) }, 500)
    }
  })

  /**
   * 手动启停单个事件源（诊断）。
   *
   * 典型用途：新写了一个源，想确认它能不能连上、事件能不能进——
   * 不必改环境变量重启整个服务（那会中断正在跑的人格任务）。
   * **不改变启停名单**：下次热加载/重启仍按 `GEBAI_EVENT_SOURCES_*` 决定。
   */
  app.post("/api/v1/persona/sources/:name/:action", async (c) => {
    if (deps.requireAdmin) {
      const denied = deps.requireAdmin(c)
      if (denied) return denied
    }
    if (!deps.sourceControl) {
      return c.json(
        {
          error: deps.setupError
            ? `拟人模式装配失败：${deps.setupError}`
            : "拟人模式未启用（没有任何 GEBAI_PERSONA_<ID> 配置）",
        },
        404,
      )
    }
    const name = c.req.param("name")
    const action = c.req.param("action")
    if (action !== "start" && action !== "stop") {
      return c.json({ error: `不支持的动作 ${action}（可用：start / stop）` }, 400)
    }
    // 源名来自 URL：只做精确匹配（不拼路径、不做通配），未知名字由管理器返回 false
    if (!/^[a-z0-9_]+$/.test(name)) return c.json({ error: `非法的源名 ${name}` }, 400)
    const ok = action === "start" ? await deps.sourceControl.start(name) : await deps.sourceControl.stop(name)
    if (!ok) {
      return c.json(
        {
          ok: false,
          error:
            action === "start"
              ? `源 ${name} 未启动（不存在 / 已在运行 / 启动失败——具体原因看 /api/v1/persona/state 的 sourceErrors）`
              : `源 ${name} 未停止（不存在 / 本来就没在运行）`,
        },
        404,
      )
    }
    return c.json({ ok: true, name, action })
  })

  /**
   * 定时开关不可用时的统一回包（与 `notEnabled` 同一取向：把真实原因说出来）。
   *
   * 单独一个函数而不是复用 notEnabled：两者的成因不同——
   * notEnabled 是「拟人整个没开/装失败」，这里是「拟人开着但这个装配没接上」，
   * 混在一起会让人往错方向查。
   */
  const notSchedulesEnabled = (c: Context) =>
    c.json(
      {
        error: deps.setupError
          ? `拟人模式装配失败：${deps.setupError}`
          : "人格定时任务控制面未装配（拟人模式未启用，或该构建未接 scheduleControl）",
        ...(deps.setupError ? { setupError: deps.setupError } : {}),
      },
      404,
    )

  /**
   * 人格定时任务：列表（含已停用的）。
   *
   * 为何不能只返回启用的：界面要能把停用的列出来并提供「重新打开」；
   * 用 `persona/state` 里的 schedules 拿不到已停用项（那里只列声明）。
   */
  app.get("/api/v1/persona/schedules", async (c) => {
    if (!deps.scheduleControl) return notSchedulesEnabled(c)
    try {
      return c.json({ schedules: await deps.scheduleControl.list() })
    } catch (err) {
      return c.json({ error: `定时任务列表生成失败：${String((err as Error)?.message || err)}` }, 500)
    }
  })

  /**
   * 人格定时任务：启用 / 停用 / 立即运行一次。
   *
   * key 形态 `{personaId}/{driverId}/{scheduleId}`（与 `scheduleKey()` 逐字一致）。
   * 含斜杠 → 客户端必须 `encodeURIComponent` 成单段（前端就是这么发的）。
   *
   * ## 为什么不用 `:key{.+}` 匹配整段（踩过的坑）
   *
   * `:key{.+}` 是**跨斜杠正则参数**，只有 `RegExpRouter` 支持；而本服务的路由表里存在
   * 「同方法静态段与参数段同层」（如 `GET /tasks/queue` 与 `GET /tasks/:id`），
   * `RegExpRouter` 对这类组合直接抛 `UnsupportedPathError`，`Hono` 的 `SmartRouter` 于是
   * **静默降级到 `TrieRouter`**——而 `TrieRouter` 既不支持 `{.+}`，`getPattern` 也认不出
   * 带花括号的段，于是这条路由**永远匹配不上**：请求直接 404（`text/plain`，不是我们的
   * JSON 错误体），前端 `res.json()` 解析失败 → 用户只看见「未知错误」。
   *
   * 所以这里用**普通参数段**：两种路由器都支持，语义由「客户端把 key 编码成单段」保证。
   * 未编码的多段形态（`/a/b/c/disable`）也有对应路由兜住（见下），二者都落到同一个处理器。
   *
   * ## 为什么 param 不再 decodeURIComponent
   *
   * Hono 的 `c.req.param()` **已经解码过一次**（内部 `tryDecodeURIComponent`）。再解一次是
   * 二次解码：key 里原本就有 `%` 时（`a%25b` → 一次解码 `a%b` → 二次解码抛 URIError）
   * 会变成 500。原实现多写的那一次是多余的。
   */
  const personaScheduleAction = async (c: Context<AppEnv>) => {
    if (deps.requireAdmin) {
      const denied = deps.requireAdmin(c)
      if (denied) return denied
    }
    if (!deps.scheduleControl) return notSchedulesEnabled(c)
    // 单段形态取 `:key`；三段形态拼回同一个 key（`scheduleKey()` 的 `${personaId}/${driverId}/${scheduleId}`）。
    // 两者都不会为空——但空 key 仍单独报 400（不把“没取到参数”当成“查无此定时”）。
    const single = c.req.param("key")
    const key = (
      single ??
      [c.req.param("personaId"), c.req.param("driverId"), c.req.param("scheduleId")].filter((s) => s !== undefined).join("/")
    ).trim()
    const action = c.req.param("action")
    if (!key) return c.json({ error: "缺少定时任务 key" }, 400)
    if (action !== "enable" && action !== "disable" && action !== "run") {
      return c.json({ error: `不支持的动作 ${action}（可用：enable / disable / run）` }, 400)
    }
    // 精确匹配已有声明：不存在的 key 直接 404。
    // 为何不做「先存起来，以后出现了再生效」：key 写错（手抄、拼错分身名）时
    // 静默接受会让人以为已经设好了——而实际那条定时根本没有——这类静默失败最难查。
    const all = await deps.scheduleControl.list()
    const found = all.find((s) => s.key === key)
    if (!found) {
      return c.json(
        {
          ok: false,
          error: `未找到定时任务 ${key}`,
          available: all.map((s) => s.key),
        },
        404,
      )
    }
    if (action === "run") {
      const r = await deps.scheduleControl.run(key)
      if (!r.ok) return c.json({ ok: false, key, action, error: r.error }, 400)
      deps.publish("event.persona.schedule", { key, action, eventId: r.eventId })
      return c.json({ ok: true, key, action, eventId: r.eventId })
    }
    const wantEnabled = action === "enable"
    const r = await deps.scheduleControl.setEnabled(key, wantEnabled)
    if (!r.ok) return c.json({ ok: false, key, action, error: r.error }, 400)
    deps.publish("event.persona.schedule", { key, action, enabled: wantEnabled })
    return c.json({ ok: true, key, action, enabled: wantEnabled })
  }

  // 两条路由写到同一个处理器，覆盖两种客户端写法：
  // - `:key`：key 已 URL 编码成单段（前端 `encodeURIComponent`，推荐形态）
  // - `:personaId/:driverId/:scheduleId`：key 原样拼三段（终端/curl 手敲时最自然，不必先编码）
  // 都不支持跨斜杠正则（见上方注释：那会把路由降级成永不匹配）。
  app.post("/api/v1/persona/schedules/:key/:action", personaScheduleAction)
  app.post("/api/v1/persona/schedules/:personaId/:driverId/:scheduleId/:action", personaScheduleAction)
}
