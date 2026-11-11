/**
 * 内置渠道 `feishu`：真实外部渠道，验证抽象在**网络 + 凭据 + 白名单**场景下成立。
 *
 * ## 为什么这个渠道是抽象的关键验证
 *
 * `local` 渠道证明「自己家的事」能收进同一套动作面，但它太简单（无网络、无凭据、
 * 无失败重试）。真正的考验是：一个**与会话身份完全无关的外部系统**，能否也只靠
 * `ChannelDef` 表达出来，而不需要给 `act` 加任何特例字段。
 *
 * 结论：能。需要的东西正好是 `ChannelDef` 已有的三样——凭据（`envVars` + `start`）、
 * 能力声明（`verbs`/`observes`）、目标白名单（`targets`）。
 *
 * ## 与既有 `feishu_group` 子 Agent 的分工
 *
 * | | `feishu_group` 子Agent | 本渠道 |
 * |---|---|---|
 * | 形态 | 装载后才有的工具（`message_send` 等） | 常驻动作面（`act`） |
 * | 何时可见 | 装载时才进上下文（省 token） | 始终可调（`channels_list` 可见） |
 * | 适用 | 一次性运维操作（建群/拉人/解散） | 持续性的收发与观测 |
 *
 * 两者不冲突：本渠道只覆盖「收发消息 + 读历史 + 看成员」这四类高频动作；
 * 群生命周期管理（建群/解散/改配置）仍归子 Agent——那不是「对外动作」的高频形态。
 *
 * ## 凭据：**独立机器人**，不与会话机器人共用
 *
 * 本渠道用 `GEBAI_CHANNELS_FEISHU_APP_ID` / `_APP_SECRET`（**不回落**到 `GEBAI_FEISHU_*`）：
 * 会话机器人（`FeishuBot`，`GEBAI_FEISHU_BOT_ENABLED`）负责「人和歌白对话」，
 * 本渠道负责「人格对外的收发」。两者是**不同机器人**（不同 AppID），即使同时启用也不互踢 ——
 * 飞书对**同一应用**只允许一条长连接，而这是两个应用。
 *
 * 若你只有一个应用可用：**要么只开其中一个**，要么接受它们抢连接（后连的踢掉先连的，
 * 表现为其中一边「偶发收不到消息」——反而更难排查）。所以在 `start()` 里对这一点显式检查。
 *
 * ## 输入面：自建长连接，不依赖宿主
 *
 * `startListen` 通过 `deps.feishuLongConn` 注入的长连接实现（默认用 `@gebai/server` 的
 * `FeishuConn`，但本包不 import 它 —— 依赖注入保持 channels 零依赖、可独立单测）。
 * 拿不到注入时如实报错（不静默降级成「收不到消息也不说」）。
 */

import type { ChannelContext, ChannelDef } from "../channel"

const FEISHU_BASE = "https://open.feishu.cn/open-apis"

/** 令牌缓存（进程内；渠道实例可能被多次 start，故放模块级但按 appId 分键）。 */
const tokenCache = new Map<string, { token: string; expiresAt: number }>()

async function tenantToken(appId: string, appSecret: string): Promise<string> {
  const key = `${appId}:${appSecret.slice(0, 8)}`
  const hit = tokenCache.get(key)
  if (hit && hit.expiresAt > Date.now() + 60_000) return hit.token
  const res = await fetch(`${FEISHU_BASE}/auth/v3/tenant_access_token/internal`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ app_id: appId, app_secret: appSecret }),
  })
  const json = (await res.json()) as { code?: number; msg?: string; tenant_access_token?: string; expire?: number }
  if (!res.ok || json.code !== 0 || !json.tenant_access_token) {
    throw new Error(`获取 tenant_access_token 失败：code=${json.code} msg=${json.msg ?? res.status}`)
  }
  const ttl = Math.max(60, Number(json.expire ?? 7200)) * 1000
  tokenCache.set(key, { token: json.tenant_access_token, expiresAt: Date.now() + ttl })
  return json.tenant_access_token
}

/**
 * 本实例的凭据（来自它自己那份 `GEBAI_CHANNEL_IN/OUT_FEISHU[]` 项，**不回落** `GEBAI_FEISHU_*`）。
 *
 * 为何取 `ctx.config` 而不是 `ctx.env(...)`：后者是全局环境变量，同类型的第二个实例
 * （`feishu_ops`）拿到的是同一份值——那样“同类配两份”根本不成立。
 */
function credsOf(ctx: ChannelContext): { appId: string | undefined; appSecret: string | undefined } {
  const c = ctx.config
  return {
    appId: typeof c.appId === "string" ? c.appId : undefined,
    appSecret: typeof c.appSecret === "string" ? c.appSecret : undefined,
  }
}

/**
 * 订阅清理器集合（键 = `home + 渠道定义对象`，两个维度缺一不可）。
 *
 * - 按 def 分键：热加载后新旧 def 对象共存，各自持自己的清理器
 * - 按 home 分键：同一份 def 被多个注册表实例复用（并行测试、多租户）时互不覆盖
 */
const cleanups = new Map<string, () => void>()
const defIds = new WeakMap<object, number>()
let defIdSeq = 0
function cleanupKey(home: string, def: object): string {
  let id = defIds.get(def)
  if (id === undefined) {
    id = ++defIdSeq
    defIds.set(def, id)
  }
  return `${home}\u0000${id}`
}

/** 从飞书 schema 2.0 事件里抽出一条消息（非消息事件返回 null）。 */
function extractMessage(ev: Record<string, unknown>): {
  messageId: string
  chatId: string
  chatName?: string
  /** 会话类型（飞书 `chat_type`：`p2p` 单聊 / `group` 群）：群消息才走整群成员表查名 */
  chatType?: string
  /** 话题根消息 id（话题群内消息非空；顶层消息为空）——会话分流与回复到话题都靠它。 */
  rootId?: string
  senderId?: string
  senderName?: string
  senderType?: string
  text: string
  createTime?: number
} | null {
  const header = (ev.header ?? {}) as { event_type?: string; create_time?: string }
  if (header.event_type !== "im.message.receive_v1") return null
  const event = (ev.event ?? {}) as Record<string, unknown>
  const message = (event.message ?? {}) as Record<string, unknown>
  const messageId = String(message.message_id ?? "")
  const chatId = String(message.chat_id ?? "")
  // 缺 message_id/chat_id 就不能投（幂等键与出站目标都靠它）——宁可丢也不乱投
  if (!messageId || !chatId) return null

  const sender = (event.sender ?? {}) as { sender_id?: { open_id?: string }; sender_type?: string }
  const chatType = String(message.chat_type ?? "")

  // 正文：只有 text 类型才取 content.text；其余类型降级为可读说明
  // （不能不看 message_type 直接取 text：图片/文件消息的 content 里可能残留 text 字段，
  //  按 text 处理会让模型以为看到的是完整内容）
  const messageType = String(message.message_type ?? "text")
  let text = ""
  if (messageType === "text") {
    try {
      const content = JSON.parse(String(message.content ?? "{}")) as { text?: string }
      text = String(content.text ?? "")
    } catch {
      text = String(message.content ?? "")
    }
  }
  if (!text.trim()) {
    text = `[${messageType} 消息]（message_id=${messageId}；如需内容请用 observe 的 messages 读取）`
  }

  const ct = Number(header.create_time ?? 0)
  // 话题归属：话题群每条消息都带 thread_id（omt_ 前缀，指向所属话题；非话题群恒空）。
  // 注意不是 root_id——那是普通群「引用回复链」的字段，话题群里恒为空（实测两种群形态验证）。
  // 出站回话题 = reply 本条消息（飞书自动继承 thread_id 落回原话题，实测 code=0 验证）。
  const rootId = String(message.thread_id ?? "").trim() || undefined
  return {
    messageId,
    chatId,
    ...(chatType === "p2p" ? { chatName: "与我的单聊" } : {}),
    // chatType 必须原样带出：下游要据此决定「是否走整群成员表查发送者姓名」——
    // 单聊没有群成员接口，带上 p2p 才不会再白打一次必然失败的请求。
    // 早期这里只把 p2p 转成 chatName 就丢了 chatType，下游那句 `msg.chatType !== "p2p"`
    // 于是恒为真（undefined !== "p2p"），单聊也去查群成员表。（类型缺失被 typecheck 抓出）
    ...(chatType ? { chatType } : {}),
    ...(rootId ? { rootId } : {}),
    ...(sender.sender_id?.open_id ? { senderId: sender.sender_id.open_id } : {}),
    ...(sender.sender_type ? { senderType: sender.sender_type } : {}),
    text,
    ...(Number.isFinite(ct) && ct > 0 ? { createTime: ct } : {}),
  }
}

/** open_id → 姓名缓存（进程内、模块级：渠道实例可能多次起，按 openId 分键即可共享）。 */
const userNameCache = new Map<string, string>()

/** 群成员表缓存（chat_id → 成员表 + 拉取时刻）：一次请求拉整群，TTL 内全群消息直接命中。
 *  空群/接口失败也缓存（空表 + 短 TTL）——否则权限不足时每条消息都白打一次接口。 */
const memberCache = new Map<string, { members: Map<string, string>; fetchedAt: number }>()
const MEMBER_CACHE_TTL_MS = 5 * 60_000
/** 空表/失败的冷却（更短）：避免消息风暴时每条都重试无效请求。 */
const MEMBER_CACHE_EMPTY_TTL_MS = 60_000

/**
 * 尽力而为地按 open_id 查发送者姓名（contact API，应用身份）。
 *
 * 飞书事件里没有姓名，只有 open_id；要显示「谁说的」只能查通讯录接口
 * （需应用开通 `contact:user.base:readonly`）。查不到/无权限/失败一律返回 undefined——
 * 入站带名字是锦上添花，绝不能让查名失败阻断消息投递。
 */
async function resolveUserNameContact(ctx: ChannelContext, openId: string): Promise<string | undefined> {
  if (!openId) return undefined
  const hit = userNameCache.get(openId)
  if (hit !== undefined) return hit
  const c0 = credsOf(ctx)
  if (!c0.appId || !c0.appSecret) return undefined
  try {
    const token = await tenantToken(c0.appId, c0.appSecret)
    const res = await fetch(`${FEISHU_BASE}/contact/v3/users/${encodeURIComponent(openId)}?user_id_type=open_id`, {
      method: "GET",
      headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
    })
    const json = (await res.json()) as { code?: number; msg?: string; data?: { user?: { name?: string } } }
    if (!res.ok || json.code !== 0) return undefined
    const name = json.data?.user?.name?.trim()
    if (name) userNameCache.set(openId, name)
    return name || undefined
  } catch {
    return undefined
  }
}

/**
 * 拉取群成员表（应用身份）：`GET /im/v1/chats/{chat_id}/members` 一次拿整群成员
 * （页大小 100，按需翻页）。比逐条 contact 查询少很多请求、且权限点不同——
 * 群成员接口不可用时静默返回空表，调用方回退 contact。
 */
async function resolveMemberTable(ctx: ChannelContext, chatId: string): Promise<Map<string, string>> {
  const c0 = credsOf(ctx)
  if (!c0.appId || !c0.appSecret) return new Map()
  // 命中缓存（含空表冷却）：直接返回，不重复拉
  const cached = memberCache.get(chatId)
  if (cached) {
    const ttl = cached.members.size ? MEMBER_CACHE_TTL_MS : MEMBER_CACHE_EMPTY_TTL_MS
    if (Date.now() - cached.fetchedAt <= ttl) return cached.members
  }
  try {
    const token = await tenantToken(c0.appId, c0.appSecret)
    const members = new Map<string, string>()
    let pageToken: string | undefined
    for (;;) {
      const q = new URLSearchParams({ page_size: "100", member_id_type: "open_id", ...(pageToken ? { page_token: pageToken } : {}) })
      const res = await fetch(`${FEISHU_BASE}/im/v1/chats/${encodeURIComponent(chatId)}/members?${q.toString()}`, {
        method: "GET",
        headers: { "content-type": "application/json", authorization: `Bearer ${token}` },
      })
      const json = (await res.json()) as {
        code?: number
        msg?: string
        data?: { items?: Array<{ member_id?: string; name?: string }>; page_token?: string; has_more?: boolean }
      }
      if (!res.ok || json.code !== 0) {
        // 无权限/失败：回退 contact，且冷却一段（不每条消息都重试）
        memberCache.set(chatId, { members: new Map(), fetchedAt: Date.now() })
        return new Map()
      }
      for (const it of json.data?.items ?? []) {
        if (it.member_id && it.name) members.set(it.member_id, it.name.trim())
      }
      if (!json.data?.has_more || !json.data?.page_token) break
      pageToken = json.data.page_token
    }
    memberCache.set(chatId, { members, fetchedAt: Date.now() })
    // 群成员表入库后也回填 openId 缓存（后续消息直接命中单条路径）
    for (const [oid, name] of members) userNameCache.set(oid, name)
    return members
  } catch {
    memberCache.set(chatId, { members: new Map(), fetchedAt: Date.now() })
    return new Map()
  }
}

interface FeishuCreds {
  appId: string
  appSecret: string
  /** 目标白名单（`GEBAI_CHANNELS_FEISHU_CHATS`，逗号分隔的 chat_id）。 */
  chats: string[]
}

async function api<T>(creds: FeishuCreds, path: string, init?: RequestInit, overrideToken?: string): Promise<T> {
  const token = overrideToken ?? (await tenantToken(creds.appId, creds.appSecret))
  const res = await fetch(`${FEISHU_BASE}${path}`, {
    ...init,
    headers: { "content-type": "application/json", authorization: `Bearer ${token}`, ...(init?.headers ?? {}) },
  })
  const json = (await res.json()) as { code?: number; msg?: string; data?: T }
  if (!res.ok || json.code !== 0) throw new Error(`飞书接口失败：${path} code=${json.code} msg=${json.msg ?? res.status}`)
  return (json.data ?? {}) as T
}

/**
 * 取用户令牌（以用户本人名义发消息时用）。
 *
 * 由字主经 `deps.personaCredentials` 注入——渠道包不依赖 `@gebai/persona`
 * （保持零依赖可独立单测）。拿不到时**如实报错而不是静默回落到机器人**：
 * 那会让「人格以为用你的名义发了，实际是机器人」成为极难发现的行为偏差。
 */
async function userTokenOf(ctx: ChannelContext): Promise<string> {
  const creds = ctx.deps.personaCredentials as
    | { token(): Promise<{ token: { accessToken: string } } | { reason: string }> }
    | undefined
  if (!creds) {
    throw new Error(
      "as_user=true 需要拟人模式已启用（GEBAI_PERSONA=true）且已完成飞书授权——" +
        "当前未注入 personaCredentials，无法以用户名义发送。可用 as_user=false 改以机器人名义发。",
    )
  }
  const got = await creds.token()
  if ("reason" in got) {
    throw new Error(`as_user=true 但飞书用户授权不可用：${got.reason}——请在界面完成授权后重试`)
  }
  return got.token.accessToken
}

/**
 * 观测（读历史/成员）的取身份策略：**用户优先、机器人回落**。
 *
 * 机器人（tenant token）读群历史需要 `im:message.group_msg`，多数应用未开通；
 * 而用户身份只需授权时勾选 `im:message.group_msg:get_as_user`（默认可申请）。
 * 所以有用户令牌就**优先用它**（能力最大，能读用户参与的所有群）；拿不到/调用失败时
 * 回落机器人身份，并把原因带出来（`note`）——静默降级会让「这个群为什么读不到历史」
 * 永远查不出来，而它看起来完全像「这个群就是没历史」。
 */
async function observeIdentity(ctx: ChannelContext): Promise<{ token?: string; note?: string }> {
  const creds = ctx.deps.personaCredentials as
    | { token(): Promise<{ token: { accessToken: string } } | { reason: string }> }
    | undefined
  if (!creds) return { note: "以机器人身份读取（未启用拟人授权）" }
  try {
    const got = await creds.token()
    if ("token" in got) return { token: got.token.accessToken }
    return { note: `以机器人身份读取（用户授权不可用：${got.reason}）` }
  } catch (err) {
    return { note: `以机器人身份读取（用户令牌失效：${String((err as Error)?.message ?? err)}）` }
  }
}

/**
 * 本实例的群白名单（来自它自己那份配置的 `chats`）。
 *
 * 两个面都用它：出站 `targets()`（fail-closed）与入站群过滤。
 */
function chatsOf(ctx: ChannelContext): string[] {
  const c = ctx.config.chats
  if (!Array.isArray(c)) return []
  return c.filter((x): x is string => typeof x === "string" && !!x.trim()).map((x) => x.trim())
}

export const channel: ChannelDef = {
  name: "feishu",
  description: "飞书群：发消息 / 引用回复，读历史消息与群成员",
  // 凭据是必需的：没有 AppID/Secret 的 feishu 实例只会把“配了但没反应”
  // 变成更难查的形态，所以未配置时干脆不造实例。
  requiresConfig: true,
  // 能收也能发。入站实例建长连接；出站实例**不建连接**（只调 HTTP），
  // 所以两者可以用同一个飞书应用。
  dirs: ["in", "out"],
  config: {
    appId: {
      type: "string",
      description: "本实例专用机器人 App ID（每个实例一个应用——同一应用只能一条长连接）",
      required: true,
    },
    appSecret: { type: "string", description: "本实例专用机器人 App Secret", required: true, secret: true },
    chats: {
      type: "string[]",
      description: "允许操作的群 chat_id 列表——**白名单，越界直接拒绝**；留空则实例不可用（fail-closed）",
    },
    ignoreSelf: {
      type: "boolean",
      description: "忽略本机器人自己发的消息（缺省 true；关掉会因自己发的消息再次触发而无限循环）",
    },
    scopes: {
      type: "string[]",
      description:
        "以**用户本人**名义授权时要申请的额外权限（并集，基础集自动包含）。" +
        "填 [\"offline_access\"] 才能拿到 refresh_token——否则 user_access_token 2 小时就过期、需重新授权。" +
        "⚠ 该权限需先在飞书后台开通，未开通时授权页会报 20027。只对人恪生效；不填 = 不要长期授权",
    },
  },

  verbs: {
    send: {
      summary: "向群发文本消息（支持 <at user_id=\"open_id\">名字</at> @人；话题群传 root 发进指定话题）",
      writes: true,
      args: {
        text: { type: "string", description: "消息正文", required: true },
        root: {
          type: "string",
          description: "要回复的消息 id（om_ 前缀）：传入则 reply 该消息。话题群内**必须**传本轮事件的 refs.messageId（飞书自动落回原话题）；不传会另开新话题", 
        },
        as_user: {
          type: "boolean",
          description: "以**用户本人**名义发（默认以机器人名义）。人格回复应该用 true——群里看到的是“你”在说话，而不是机器人",
        },
      },
    },
    reply: {
      summary: "引用回复某条消息（带上原文，上下文清晰）",
      writes: true,
      args: {
        text: { type: "string", description: "回复正文", required: true },
        reply_to: { type: "string", description: "被回复的消息 id（om_ 前缀）", required: true },
        as_user: { type: "boolean", description: "以用户本人名义回复（默认以机器人名义）" },
      },
    },
  },

  observes: {
    messages: {
      summary: "读群历史消息（倒序，最近的在前）",
      args: { limit: { type: "number", description: "条数（默认 20，上限 50）" } },
    },
    members: {
      summary: "列出群成员（姓名 + open_id，@ 人时用 open_id）",
      args: { limit: { type: "number", description: "条数（默认 50，上限 100）" } },
    },
  },

  /** 输入面：本渠道自建长连接收到的消息都归 `message` 类。 */
  listens: {
    messages: {
      summary: "订阅群消息（机器人被 @ 或单聊时；由本渠道自己的长连接推送）",
    },
  },
  defaultKind: "message",

  /**
   * 白名单：**只**允许本实例 `chats` 里列出的群。
   *
   * 留空返回 `[]`（而不是 `null`）：未配置白名单时该实例**不能**发任何消息。
   * 这是 fail-closed——「没配就是不许发」比「没配就是随便发」安全得多，
   * 因为后者的失败形态是「把内部信息发错群」，而前者只是「还没配好」。
   */
  targets(ctx: ChannelContext) {
    return chatsOf(ctx)
  },

  async start(ctx: ChannelContext) {
    const appId = credsOf(ctx).appId
    const appSecret = credsOf(ctx).appSecret
    if (!appId || !appSecret) {
      throw new Error(
        "缺少 GEBAI_CHANNELS_FEISHU_APP_ID / GEBAI_CHANNELS_FEISHU_APP_SECRET（该渠道不可用，其它渠道不受影响）。" +
          "注意：本渠道**不回落** GEBAI_FEISHU_* —— 会话机器人（FeishuBot）与对外人格是两个不同的飞书应用。",
      )
    }
    // 显式检查与 session bot 撞应用的配置错误：飞书对**同一应用**只允许一条长连接，
    // 两者同时用同一个 AppID 会互踢（表现为其中一边偶发收不到消息，极难排查）。
    // 只在会话机器人**已启用**时才判撞：bot 停用时它根本不建长连接，同 AppID 不冲突
    // （把残留的 GEBAI_FEISHU_APP_ID 当冲突源会误拒——bot 关了渠道也起不来，两边都哑）。
    const sessionAppId = ctx.deps.feishuSessionAppId
    const sessionBotEnabled = ctx.deps.feishuSessionBotEnabled !== false // 缺省视为启用（兼容旧宿主只注入 appId 的形态）
    if (sessionBotEnabled && typeof sessionAppId === "string" && sessionAppId && sessionAppId === appId) {
      throw new Error(
        `通道实例「${ctx.name}」的 appId 与已启用的会话机器人（GEBAI_FEISHU_APP_ID）相同：` +
          "同一飞书应用不能建两条长连接（会互踢）。" +
          "给本实例换一个独立应用，或关闭其中一个（GEBAI_FEISHU_BOT_ENABLED=false）。",
      )
    }
    const chats = chatsOf(ctx)
    if (!chats.length) {
      // 不抛错：凭据是好的，只是没配白名单 → 实例起来但什么也发不出去（targets 返回 []）
      ctx.log("warn", `实例「${ctx.name}」未配 chats —— 该实例不允许任何目标（fail-closed），act 会被直接拒绝`)
    } else {
      ctx.log("info", `实例「${ctx.name}」就绪，允许 ${chats.length} 个群：${chats.join("、")}`)
    }
    // 验一次令牌（失败即该渠道不可用：与其等到第一次发消息才炸，不如启动期如实失败）
    await tenantToken(appId, appSecret)
    ctx.log("info", "飞书 tenant_access_token 获取成功")
  },

  /**
   * 订阅群消息（自建长连接，不依赖会话机器人）。
   *
   * 长连接实现经 `ctx.deps.feishuLongConn` 注入（工厂形态，参数为 appId/appSecret/onEvent）：
   * channels 包零依赖，不 import `@gebai/server` 的 `FeishuConn`。
   *
   * 拿不到注入时**如实失败**而不是静默降级——「配了但收不到消息」是最难排查的一类。
   */
  async startListen(sink, ctx) {
    const { appId, appSecret } = credsOf(ctx)
    if (!appId || !appSecret) throw new Error("缺少 GEBAI_CHANNELS_FEISHU_APP_ID / _APP_SECRET，无法订阅消息")

    const factory = ctx.deps.feishuLongConn as
      | ((opts: { appId: string; appSecret: string; onEvent: (ev: Record<string, unknown>) => void }) => {
          start(): Promise<void>
          stop(): void
        })
      | undefined
    if (typeof factory !== "function") {
      throw new Error(
        "未注入长连接实现（deps.feishuLongConn）——无法订阅飞书消息。" +
          "宿主应把 FeishuConn 以工厂形态注入（channels 包不直接依赖 @gebai/server）。",
      )
    }

    // 缺省 true（忽略自己发的）；显式 false 才关
    const ignoreSelfCfg = ctx.config.ignoreSelf
    const ignoreSelf = ignoreSelfCfg === undefined ? true : ignoreSelfCfg !== false
    // 自发送消息记账（宿主注入）：自己发出/回复的消息按 message_id 精确过滤——
    // 以用户名义（as_user=true）发的消息 sender_type=user，能绕过 IGNORE_SELF 的 app 过滤，
    // 不查这笔账就会把自己说的话又当新消息投回去（人格多处理一轮）。
    const allowed = new Set(chatsOf(ctx))
    // 未配白名单 = 不限定入站群（入站不做写操作，风险低于出站）；但日志里明确说清楚
    if (!allowed.size) ctx.log("info", `实例「${ctx.name}」未配 chats：入站不限群（出站仍 fail-closed）`)

    const conn = factory({
      appId,
      appSecret,
      onEvent: (ev) => {
        const msg = extractMessage(ev)
        if (!msg) return // 非消息事件（卡片回调等）不归本渠道
        if (ignoreSelf && msg.senderType === "app") return // 防自触发循环
        if (allowed.size && !allowed.has(msg.chatId)) {
          ctx.log("info", `忽略非白名单群消息：${msg.chatId}`)
          return
        }
        // 发送者判定：决定**触不触发执行**，但**不再决定要不要记录**（与事件源同口径）。
        //
        // 旧实现对判定为“不该回”的消息直接 return——消息整条消失。现在两条入站路径
        // 汇总到同一条纪律：每条消息都进上下文，只是有些不起轮次。
        //
        // 为何渠道也要判：渠道入站是**投给人格运行时**的（ChannelInputBridge），
        // 而人格事件源现在也在收同一批消息。两处口径不一致 → 「有时回有时不回」。
        // 判定实现由宿主注入（channels 不依赖 persona 包，保持依赖方向）。
        const sentMessages = ctx.deps.sentMessages as { hit(id: string): boolean } | undefined
        const isSelf = sentMessages?.hit(msg.messageId) === true
        void (async () => {
          const decide = ctx.deps.speakerFilter as
            | ((input: { senderId?: string; isSelf: boolean }) => {
                deliver: boolean
                record?: boolean
                kind?: "self" | "owner" | "other"
                reason?: string
                note?: string
              })
            | undefined
          let verdict: { deliver: boolean; record?: boolean; kind?: "self" | "owner" | "other"; reason?: string; note?: string }
          if (decide) {
            verdict = await Promise.resolve(decide({ ...(msg.senderId ? { senderId: msg.senderId } : {}), isSelf })).catch(
              // 判定器抛错：当作“要处理”——宁可多回一条，也不能静默吞掉（旧代码同取向）
              () => ({ deliver: true, record: true }) as { deliver: boolean; record?: boolean },
            )
          } else {
            // 未注入判定器（旧宿主）：至少保留防回灌
            verdict = isSelf ? { deliver: false, record: true, kind: "self" as const, reason: "旧宿主未注入判定器（防回灌）" } : { deliver: true, record: true }
          }
          if (!verdict.deliver) {
            ctx.log("info", `消息 ${msg.messageId} 只记入上下文、不触发执行：${verdict.reason ?? "被 speakerFilter 拦下"}`)
          }
        // 发送者姓名：优先群成员表（批量，一次覆盖全群），未命中再逐条查 contact API。
        // 查不到（权限不足/接口失败）时 body 兜底标「用户」——模型至少知道这条是别人发的，
        // 而不是把无前缀消息当成来自自己的上下文。        void (async () => {
          let senderName: string | undefined
          // 群消息：先尝试整群成员表；单聊（p2p）没有群成员接口，直接走 contact。
          // 判据取**协议字段** `chatType`，不拿展示文案 `chatName === "与我的单聊"` 当前置条件——
          // 后者是给人看的字符串（随时可能改措辞/本地化），把它当判据等于让「改一句文案」变成改变行为。
          if (msg.chatId && msg.chatType !== "p2p") {
            const table = await resolveMemberTable(ctx, msg.chatId)
            senderName = table.get(msg.senderId ?? "")
          }
          if (!senderName && msg.senderId) senderName = await resolveUserNameContact(ctx, msg.senderId)
          const who = senderName?.trim() ? `【${senderName.trim()}】` : "【用户】"
          void sink
            .emit({
              summary: `${senderName ?? msg.senderId ?? "用户"}：${msg.text.slice(0, 60)}`,
              body: who + msg.text,
              kind: "message",
              // 幂等键：飞书消息 id（它会重投同一消息）
              id: `feishu:${msg.messageId}`,
              at: msg.createTime,
              refs: {
                chatId: msg.chatId,
                messageId: msg.messageId,
                // 话题归属：话题群内消息带 root_id（话题根消息 id）——人格侧据此按话题分会话，
                // 出站回复也靠它回到原话题（reply 的 root 语义）。非话题消息无此字段。
                ...(msg.rootId ? { threadId: msg.rootId } : {}),
                ...(msg.senderId ? { senderId: msg.senderId } : {}),
                ...(senderName ? { senderName } : {}),
                ...(!senderName ? { senderName: "用户" } : {}),
                // 身份/处置标记（与事件源同口径）：让 runtime 知道这条要不要触发执行、
                // 让模型知道这段话是什么来历。渠道不自己造词，一律用判定器给的。
                ...(verdict.kind ? { extra: { speakerKind: verdict.kind } } : {}),
                ...(verdict.deliver
                  ? {}
                  : {
                      extra: {
                        ...(verdict.kind ? { speakerKind: verdict.kind } : {}),
                        contextOnly: "1",
                        ...(verdict.reason ? { contextOnlyReason: verdict.reason } : {}),
                        ...(verdict.note ? { speakerNote: verdict.note } : {}),
                      },
                    }),
              },
              // 话题群内消息的来源标注带话题（模型可感知「这是某个话题内的一句话」）
              source: msg.rootId
                ? msg.chatName
                  ? `飞书群「${msg.chatName}」话题 ${msg.rootId}`
                  : `飞书群 ${msg.chatId} 话题 ${msg.rootId}`
                : msg.chatName === "与我的单聊"
                  ? "飞书单聊"
                  : msg.chatName
                    ? `飞书群「${msg.chatName}」`
                    : `飞书群 ${msg.chatId}`,
            })
            .then((r) => {
              if (r.status !== "accepted") ctx.log("warn", `消息 ${msg.messageId} 未被接管（${r.status}）`)
            })
            .catch((err) => ctx.log("warn", `投递 ${msg.messageId} 失败: ${String((err as Error)?.message || err)}`))
        })()
      },
    })

    await conn.start()
    cleanups.set(cleanupKey(ctx.home, this as object), () => conn.stop())
    ctx.log("info", "飞书消息订阅已建立（本渠道自己的长连接）")
  },

  async stopListen(ctx) {
    const key = cleanupKey(ctx.home, this as object)
    const fn = cleanups.get(key)
    if (fn) {
      cleanups.delete(key)
      fn()
    }
  },

  async act(input, ctx) {
    const c0 = credsOf(ctx)
    const creds: FeishuCreds = { appId: c0.appId!, appSecret: c0.appSecret!, chats: [] }
    // `as_user=true` 时以用户本人名义发（人格回复应如此：群里看到的是「你」在说话）
    const asUser = input.args.as_user === true

    if (input.verb === "send") {
      const text = String(input.args.text ?? "")
      // 话题内发送：root 非空时 reply 该消息（飞书自动继承 thread_id 落回原话题——实测验证；
      // 话题群没有「群顶层」：不带 root 直发会**另开新话题**，打断不到正在讨论的人）。
      // root 取值应为**消息 id**（om_ 前缀），不是话题 id（omt_）。
      const root = String(input.args.root ?? "").trim()
      const data = await api<{ message_id?: string }>(
        creds,
        root
          ? `/im/v1/messages/${encodeURIComponent(root)}/reply`
          : "/im/v1/messages?receive_id_type=chat_id",
        {
          method: "POST",
          body: root
            ? JSON.stringify({ msg_type: "text", content: JSON.stringify({ text }) })
            : JSON.stringify({ receive_id: input.address, msg_type: "text", content: JSON.stringify({ text }) }),
        },
        asUser ? await userTokenOf(ctx) : undefined,
      )
      const who = asUser ? "（以你的名义）" : ""
      // 自发送记账：入站长连接收到自己发的消息时能认出并丢弃（防回灌）。
      // 飞书事件里只有 message_id 能精确对上（sender_type=user 与用户本人无法区分）；
      // 拿不到 message_id（接口异常等）也不阻断发送——只是少了防回灌能力。
      const sent = ctx.deps.sentMessages as { record(id: string): void } | undefined
      if (data.message_id) sent?.record(data.message_id)
      return {
        summary: `已发到飞书群 ${input.address}${root ? ` 话题 ${root}` : ""}${who}（${text.length} 字符）`,
        ...(data.message_id ? { ref: data.message_id } : {}),
        data: { chatId: input.address, ...(root ? { root } : {}), messageId: data.message_id ?? null, asUser },
      }
    }

    if (input.verb === "reply") {
      const text = String(input.args.text ?? "")
      const replyTo = String(input.args.reply_to ?? "")
      const data = await api<{ message_id?: string }>(
        creds,
        `/im/v1/messages/${encodeURIComponent(replyTo)}/reply`,
        { method: "POST", body: JSON.stringify({ msg_type: "text", content: JSON.stringify({ text }) }) },
        asUser ? await userTokenOf(ctx) : undefined,
      )
      const who = asUser ? "（以你的名义）" : ""
      const sent = ctx.deps.sentMessages as { record(id: string): void } | undefined
      if (data.message_id) sent?.record(data.message_id)
      return {
        summary: `已引用回复 ${replyTo}${who}（新消息 ${data.message_id ?? "?"}）`,
        ...(data.message_id ? { ref: data.message_id } : {}),
        data: { chatId: input.address, replyTo, messageId: data.message_id ?? null, asUser },
      }
    }

    throw new Error(`feishu 渠道不支持 verb「${input.verb}」`)
  },

  async observe(input, ctx) {
    const c0 = credsOf(ctx)
    const creds: FeishuCreds = { appId: c0.appId!, appSecret: c0.appSecret!, chats: [] }
    const identity = await observeIdentity(ctx)

    // 机器人身份读不到（230027 等权限码）且拟人凭据存在：几乎必然是**用户 token 过期后回落**——
    // 真实原因是「需重新授权」而非「接口失败」。报错必须带上授权语义，否则上层 gate
    // 识别不出、不弹授权卡，用户只看到无休止的 code=230027（实测：算力审核员会话
    // 静默等待数小时，没人知道要去授权）。
    const hasPersona = (ctx.deps.personaCredentials ?? null) !== null
    const readAsBot = async <T>(path: string): Promise<T> => {
      try {
        return await api<T>(creds, path, undefined, identity?.token)
      } catch (err) {
        if (hasPersona && !identity?.token && /code=230027|code=99991672/.test(String((err as Error)?.message ?? err))) {
          throw new Error(
            `用户授权不可用：token 已过期或失效，回落机器人身份后无权读取（${String((err as Error)?.message ?? err)}）——请在相期授权卡重新完成飞书授权`,
          )
        }
        throw err
      }
    }

    if (input.what === "messages") {
      const limit = Math.max(1, Math.min(50, Number(input.args.limit ?? 20) || 20))
      const data = await readAsBot<{ items?: Array<Record<string, unknown>>; has_more?: boolean }>(
        `/im/v1/messages?container_id_type=chat&container_id=${encodeURIComponent(input.address)}&sort_type=ByCreateTimeDesc&page_size=${limit}`,
      )
      const items = data.items ?? []
      const rows = items.map((m) => {
        const body = (m.body as { content?: string } | undefined)?.content
        let text = ""
        try {
          text = String((JSON.parse(body ?? "{}") as { text?: string }).text ?? "")
        } catch {
          text = body ?? ""
        }
        const sender = (m.sender as { id?: string; sender_type?: string } | undefined)?.id ?? "?"
        return `- [${String(m.create_time ?? "")}] ${sender}: ${text.slice(0, 200)}${text.length > 200 ? "…" : ""}`
      })
      return {
        summary: rows.length
          ? `飞书群 ${input.address} 最近 ${rows.length} 条消息${identity.note ? `（${identity.note}）` : ""}：\n${rows.join("\n")}`
          : "（这个群还没有消息，或当前身份不在群里）",
        data: { chatId: input.address, count: rows.length, hasMore: !!data.has_more, items, identity: identity.token ? "user" : "bot" },
      }
    }

    if (input.what === "members") {
      const limit = Math.max(1, Math.min(100, Number(input.args.limit ?? 50) || 50))
      const data = await readAsBot<{ items?: Array<Record<string, unknown>> }>(
        `/im/v1/chats/${encodeURIComponent(input.address)}/members?member_id_type=open_id&page_size=${limit}`,
      )
      const items = data.items ?? []
      const rows = items.map((m) => `- ${String(m.name ?? "?")}（${String(m.member_id ?? m.open_id ?? "?")}）`)
      return {
        summary: rows.length
          ? `飞书群 ${input.address} 共 ${rows.length} 位成员${identity.note ? `（${identity.note}）` : ""}：\n${rows.join("\n")}`
          : "（读不到成员：当前身份可能不在这个群）",
        data: { chatId: input.address, count: rows.length, members: items, identity: identity.token ? "user" : "bot" },
      }
    }

    throw new Error(`feishu 渠道不支持 observe「${input.what}」`)
  },
}
