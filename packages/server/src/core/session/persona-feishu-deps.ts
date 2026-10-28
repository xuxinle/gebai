/**
 * 拟人事件源的宿主依赖注入：**长连接 + 读历史**。
 *
 * 事件源插件是零依赖的（`packages/persona` 不认识 `@gebai/server`），所以这两件宿主能力
 * 必须从外面注入。本文件同时集中了两处**最容易配错、且错得很难查**的地方：
 *
 * ## 一、长连接撞车（同一飞书应用只能一条）
 *
 * 飞书对同一 AppID 只允许一条长连接。若拟人源用的应用与会话机器人（`FeishuBot`）相同，
 * 两条连接会**互踢**——现象是「两边都偶发收不到消息」，日志里只有零星重连，看不出根因。
 *
 * 所以这里不是「照注入就好」，而是**先在启动期判定并如实报错**：
 *
 * ```
 * persona AppID == FeishuBot AppID 且 FeishuBot 已启用  →  拒绝注入 + 明确说明怎么改
 * ```
 *
 * 这与 `@gebai/channels` 里 feishu 渠道的做法一致（那里也做同一检查）——同一个坑不值得
 * 让两处各自发现一次。
 *
 * ## 二、读历史必须用户身份（而且拿不到要如实说）
 *
 * 应用身份只能读机器人所在群；用户身份能读自己参与的所有群。所以：
 * - 有用户令牌 → 用它读（能力最大）
 * - 没有 → 退回应用身份**并带上原因**（`fallbackReason`），由源写进日志
 *
 * 静默降级会让「这个群为什么读不到历史」永远查不出来，而它看起来完全像「这个群就是没历史」。
 */

/** 长连接工厂（与 `@gebai/channels` 的 feishu 渠道同形，宿主统一注入）。 */
export type FeishuLongConnFactory = (opts: {
  appId: string
  appSecret: string
  onEvent: (ev: Record<string, unknown>) => void
}) => { start(): Promise<void>; stop(): void }

export interface FeishuHistoryMessage {
  senderName?: string
  text: string
  createTime?: number
}

export interface FeishuHistoryResult {
  messages: FeishuHistoryMessage[]
  readIdentity: "user" | "app"
  fallbackReason?: string
}

export interface FeishuSourceDepsOptions {
  /** 拟人源要用的应用凭据（`FEISHU_BOT_*`，缺省回落 `GEBAI_FEISHU_*`）。 */
  appId: () => string | undefined
  appSecret: () => string | undefined
  /** 会话机器人（FeishuBot）当前是否启用 + 它用的 AppID：用于撞车判定。 */
  sessionBot: () => { enabled: boolean; appId?: string }
  /** 造一个长连接（宿主传 `FeishuConn` 的工厂）。 */
  makeConn: (opts: { appId: string; appSecret: string; onEvent: (ev: Record<string, unknown>) => void }) => {
    start(): Promise<void>
    stop(): void
  }
  /** 用户令牌（拟人凭据）：拿不到时退回应用身份。 */
  userToken?: () => Promise<{ token: { accessToken: string } } | { reason: string }>
  /** 应用令牌（应用身份读历史）。 */
  appToken?: () => Promise<string>
  /** 自发送消息记账（渠道 act 成功登记 message_id；入站侧据此过滤自己发的消息）。可选——未注入时不做防回灌过滤。 */
  sentMessages?: { record(id: string): void; hit(id: string): boolean }
  /** HTTP 垫片（测试注入；缺省 fetch）。 */
  fetchImpl?: (url: string, init: RequestInit) => Promise<{ ok: boolean; status: number; json(): Promise<unknown> }>
  log?: { info(msg: string): void; warn(msg: string): void }
}

const FEISHU_BASE = "https://open.feishu.cn/open-apis"

/**
 * 构造注入给 `EventSourceManager` 的 `deps`。
 *
 * 返回的 `deps.feishuLongConn` 可能是 `undefined`（撞车时）：源启动会如实报错并保持未启动，
 * 不会静默什么都不做——「配了但收不到」是最难排查的一类。
 */
export function createFeishuSourceDeps(opts: FeishuSourceDepsOptions): Record<string, unknown> {
  const deps: Record<string, unknown> = {}

  const appId = opts.appId()
  const appSecret = opts.appSecret()
  if (!appId || !appSecret) {
    // 没配置拟人专用应用：不注入（源启动时会报「缺凭据」，那条信息比这里更贴近使用者）
    return deps
  }

  // ① 撞车判定（同一应用只能一条长连接）
  const bot = opts.sessionBot()
  if (bot.enabled && bot.appId && bot.appId === appId) {
    opts.log?.warn(
      "[gebai] 拟人飞书事件源未注入长连接：FEISHU_BOT_APP_ID / GEBAI_FEISHU_APP_ID 与已启用的会话机器人相同。" +
        "飞书对同一应用只允许一条长连接，两条会互踢（表现为两边都偶发收不到消息）。" +
        "给拟人模式单独建一个应用（配 FEISHU_BOT_APP_ID / _SECRET），或关闭其中一个。",
    )
    return deps
  }

  deps.feishuLongConn = ((o: { appId: string; appSecret: string; onEvent: (ev: Record<string, unknown>) => void }) =>
    opts.makeConn(o)) satisfies FeishuLongConnFactory

  // ② 读历史（用户身份优先，失败退回应用身份并带原因）
  deps.feishuHistory = async (input: { chatId: string; limit: number }): Promise<FeishuHistoryResult> => {
    const fetchImpl = opts.fetchImpl ?? ((url, init) => fetch(url, init) as never)
    const call = async (token: string, identity: "user" | "app"): Promise<FeishuHistoryResult> => {
      const q = new URLSearchParams({
        container_id_type: "chat",
        container_id: input.chatId,
        sort_type: "ByCreateTimeDesc",
        page_size: String(Math.min(50, Math.max(1, input.limit))),
      })
      const res = await fetchImpl(`${FEISHU_BASE}/im/v1/messages?${q.toString()}`, {
        method: "GET",
        headers: { authorization: `Bearer ${token}`, "content-type": "application/json; charset=utf-8" },
      })
      const json = (await res.json()) as {
        code?: number
        msg?: string
        data?: { items?: Array<{ body?: { content?: string }; msg_type?: string; sender?: { name?: string; id?: string }; create_time?: string }> }
      }
      if (!res.ok || json.code !== 0) {
        throw new Error(`读群历史失败：code=${json.code} msg=${json.msg ?? res.status}`)
      }
      const items = json.data?.items ?? []
      return {
        // 飞书返回倒序：翻转成时间正序（模型读「之前发生了什么」就该按时间读）
        messages: items
          .map((m) => ({
            text: parseBody(m.msg_type, m.body?.content),
            ...(m.sender?.name ? { senderName: m.sender.name } : {}),
            ...(m.create_time ? { createTime: Number(m.create_time) || undefined } : {}),
          }))
          .filter((m) => m.text.trim().length > 0)
          .reverse(),
        readIdentity: identity,
      }
    }

    if (opts.userToken) {
      try {
        const t = await opts.userToken()
        if ("token" in t) return await call(t.token.accessToken, "user")
        opts.log?.info(`[gebai] 拟人飞书源读历史回退应用身份：${t.reason}`)
        if (opts.appToken) return { ...(await call(await opts.appToken(), "app")), fallbackReason: t.reason }
        throw new Error(`用户授权不可用（${t.reason}）且未配置应用身份读取`)
      } catch (err) {
        // 用户身份读失败（令牌过期/无权限）：再试一次应用身份——比整条历史都拿不到强
        const reason = String((err as Error)?.message || err)
        opts.log?.warn(`[gebai] 拟人飞书源以用户身份读历史失败（改试应用身份）：${reason}`)
        if (opts.appToken) return { ...(await call(await opts.appToken(), "app")), fallbackReason: reason }
        throw err
      }
    }
    if (!opts.appToken) throw new Error("未配置任何可用身份读取群历史（既无用户令牌也无应用凭据）")
    return call(await opts.appToken(), "app")
  }

  // ③ 入站消息带发送者姓名：**群成员表优先**（一次拉整群，批量覆盖全群消息），
  //    contact API 兜底（单聊没群成员表）。查不到返回 undefined，不阻断入站。
  // 与 feishuHistory 共用 appToken（应用身份）；进程内按 openId + chatId 双缓存。
  const userNameCache = new Map<string, string>()
  const memberCache = new Map<string, { members: Map<string, string>; fetchedAt: number }>()
  const MEMBER_CACHE_TTL_MS = 5 * 60_000
  const MEMBER_CACHE_EMPTY_TTL_MS = 60_000

  /** 拉取群成员表（应用身份）：一次拿整群（页大小 100，按需翻页）。失败/无权限 → 空表。 */
  const memberTable = async (chatId: string): Promise<Map<string, string>> => {
    const cached = memberCache.get(chatId)
    if (cached) {
      const ttl = cached.members.size ? MEMBER_CACHE_TTL_MS : MEMBER_CACHE_EMPTY_TTL_MS
      if (Date.now() - cached.fetchedAt <= ttl) return cached.members
    }
    const fetchImpl = opts.fetchImpl ?? ((url, init) => fetch(url, init) as never)
    try {
      const token = await opts.appToken!()
      const members = new Map<string, string>()
      let pageToken: string | undefined
      for (;;) {
        const q = new URLSearchParams({ page_size: "100", member_id_type: "open_id", ...(pageToken ? { page_token: pageToken } : {}) })
        const res = await fetchImpl(`${FEISHU_BASE}/im/v1/chats/${encodeURIComponent(chatId)}/members?${q.toString()}`, {
          method: "GET",
          headers: { authorization: `Bearer ${token}`, "content-type": "application/json; charset=utf-8" },
        })
        const json = (await res.json()) as {
          code?: number
          msg?: string
          data?: { items?: Array<{ member_id?: string; name?: string }>; page_token?: string; has_more?: boolean }
        }
        if (!res.ok || json.code !== 0) {
          memberCache.set(chatId, { members: new Map(), fetchedAt: Date.now() }) // 冷却
          return new Map() // 无权限/失败 → 回退 contact
        }
        for (const it of json.data?.items ?? []) {
          if (it.member_id && it.name) members.set(it.member_id, it.name.trim())
        }
        if (!json.data?.has_more || !json.data?.page_token) break
        pageToken = json.data.page_token
      }
      memberCache.set(chatId, { members, fetchedAt: Date.now() })
      if (members.size) {
        for (const [oid, name] of members) userNameCache.set(oid, name)
      }
      return members
    } catch {
      memberCache.set(chatId, { members: new Map(), fetchedAt: Date.now() }) // 冷却
      return new Map()
    }
  }

  /** open_id → 姓名（带 chatId 群上下文）：群成员表优先，contact 兜底。 */
  deps.feishuUserName = async (openId: string, chatId?: string): Promise<string | undefined> => {
    if (!openId) return undefined
    const hit = userNameCache.get(openId)
    if (hit !== undefined) return hit
    if (chatId) {
      const name = (await memberTable(chatId)).get(openId)
      if (name) {
        userNameCache.set(openId, name)
        return name
      }
    }
    if (!opts.appToken) return undefined
    const fetchImpl = opts.fetchImpl ?? ((url, init) => fetch(url, init) as never)
    try {
      const token = await opts.appToken()
      const res = await fetchImpl(`${FEISHU_BASE}/contact/v3/users/${encodeURIComponent(openId)}?user_id_type=open_id`, {
        method: "GET",
        headers: { authorization: `Bearer ${token}`, "content-type": "application/json; charset=utf-8" },
      })
      const json = (await res.json()) as { code?: number; msg?: string; data?: { user?: { name?: string } } }
      if (!res.ok || json.code !== 0) {
        opts.log?.info(`[gebai] 查飞书用户名失败（不影响入站）：code=${json.code} ${json.msg ?? res.status}`)
        return undefined
      }
      const name = json.data?.user?.name?.trim()
      if (name) userNameCache.set(openId, name)
      return name || undefined
    } catch (err) {
      opts.log?.info(`[gebai] 查飞书用户名异常（不影响入站）：${String((err as Error)?.message ?? err)}`)
      return undefined
    }
  }

  // ④ 自发送消息记账（防回灌）：渠道 act 成功后登记 message_id，本源入站据此过滤——
  // 自己以用户名义发的消息（sender_type=user）无法从事件字段区分，只能精确对账。
  if (opts.sentMessages) deps.sentMessages = opts.sentMessages

  return deps
}

/** 解析消息正文（只取文本类；其它类型给可读占位，不猜内容）。 */
export function parseBody(msgType: string | undefined, content: string | undefined): string {
  const raw = (content ?? "").trim()
  if (!raw) return ""
  if (msgType && msgType !== "text") {
    if (msgType === "post") {
      // 富文本：拼出纯文本（比「[post 消息]」有用得多，且不引入结构解析）
      try {
        const parsed = JSON.parse(raw) as { content?: Array<Array<{ tag?: string; text?: string }>>; title?: string }
        const parts: string[] = []
        if (parsed.title) parts.push(parsed.title)
        for (const line of parsed.content ?? []) {
          for (const node of line) if (node?.text) parts.push(node.text)
        }
        const text = parts.join(" ").trim()
        if (text) return text
      } catch {
        /* 落到占位 */
      }
    }
    return `（${msgType} 消息，正文未展开）`
  }
  try {
    const parsed = JSON.parse(raw) as { text?: string }
    return (parsed.text ?? "").trim()
  } catch {
    return raw
  }
}
