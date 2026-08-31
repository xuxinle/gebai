import type { ToolSchema } from "@gebai/sdk"
import type { SubAgentDef, Tool, ToolContext } from "../../core/types"
import { feishuFetch } from "../../feishu-bot/tls"

/**
 * 飞书群基础能力（feishu_group 子Agent）：机器人所在群列表/群详情/群成员查询/用户信息/群内发消息，
 * 以及群维护写操作（建群/改群信息/拉人/移人/解散）。
 * 凭证从环境变量读取（子Agent 前缀规范，兼容全局 GEBAI_FEISHU_*）：
 *   FEISHU_GROUP_APP_ID / FEISHU_GROUP_APP_SECRET
 * 定时任务通知联动：members_list 查到的 open_id 可直接填 cron_add 的 at 名单（@特定人），
 * 群 chat_id 可直接填 feishu 通知通道 target（指定群以应用身份推送）。
 */

const BASE_URL = "https://open.feishu.cn"
const API_TIMEOUT_MS = 30_000
/** tenant_access_token 有效期 7200s，提前 200s 刷新。 */
const TOKEN_REFRESH_EARLY_MS = 200_000

const moduleTokenCache = new Map<string, { token: string; expireAt: number }>()

export interface FeishuGroupDeps {
  fetchFn: typeof fetch
  tokenCache: Map<string, { token: string; expireAt: number }>
}

export const name = "feishu_group"
export const description =
  "飞书群基础能力：查询机器人所在的群列表/群详情/群成员（open_id+姓名，@特定人与定时任务通知的取材来源）、按 open_id 查用户信息、向群发文本消息，以及群维护（建群/改群名描述/拉人/移人/解散）。需要群成员名单、群管理或为定时任务通知配 @ 人/指定群时装载本子Agent。需配置 FEISHU_GROUP_APP_ID/SECRET 或全局 GEBAI_FEISHU_APP_ID/SECRET。"
export const systemPrompt =
  "你是飞书群管理助手，以应用身份（tenant_access_token）操作飞书群基础能力。工具经本子Agent 命名空间暴露（feishu_group_ 前缀）。工作要点：\n" +
  "1) 查询类（免审批）：chats_list 列出机器人所在的群（chat_id/名称/描述，分页）；chat_info 群详情（名称/描述/群主/成员数）；members_list 群成员分页列表（open_id + 姓名——@ 特定人与定时任务通知 at 名单的 open_id 来源）；user_info 按 open_id 查用户姓名等信息；\n" +
  "2) 写操作（需审批）：message_send 向群发文本（支持 <at user_id=\"open_id\">名字</at> 与 <at user_id=\"all\">所有人</at> 标签）；chat_create 建群；chat_update 改群名/描述；chat_members_add 拉人进群（open_id 列表）；chat_members_remove 移出群成员；chat_disband 解散群（不可恢复，删前必须向用户确认）；\n" +
  "3) 与定时任务通知联动（高频场景）：为 cron_add 配置通知时——feishu 通道 target 可直接填群 chat_id（以应用身份推送该群）；at 名单 @特定人需要 open_id，用 members_list 查（姓名 → open_id），@所有人 用 \"all\"；\n" +
  "4) 凭证：FEISHU_GROUP_APP_ID/FEISHU_GROUP_APP_SECRET，缺省回落全局 GEBAI_FEISHU_APP_ID/GEBAI_FEISHU_APP_SECRET（机器人需已入群）；\n" +
  "5) 常见问题：拉人失败多为被拉人未开通飞书或无互加权限；members_list 需要应用具备 im:chat:readonly（或 im:chat）权限，写操作需要 im:chat，发消息需要 im:message:send_as_bot——权限不足时提示用户到开发者后台开通对应 scope 并重新发布版本。"

export const envVars = [
  { name: "FEISHU_GROUP_APP_ID", description: "飞书应用 App ID（feishu_group 群基础能力凭证；缺省回落 GEBAI_FEISHU_APP_ID）" },
  { name: "FEISHU_GROUP_APP_SECRET", description: "飞书应用 App Secret（敏感，仅本次任务临时注入，不落盘；缺省回落 GEBAI_FEISHU_APP_SECRET）" },
]

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

/** 列表输出裁剪（超长防撑爆上下文）。 */
function clip(text: string, max = 12000): string {
  return text.length > max ? `${text.slice(0, max)}\n…（已截断）` : text
}

export function createFeishuGroupTools(deps: FeishuGroupDeps = { fetchFn: feishuFetch, tokenCache: moduleTokenCache }): Record<string, Tool> {
  function readConfig(ctx: ToolContext): { appId: string; appSecret: string } {
    const appId = ctx.env.FEISHU_GROUP_APP_ID || ctx.env.GEBAI_FEISHU_APP_ID
    const appSecret = ctx.env.FEISHU_GROUP_APP_SECRET || ctx.env.GEBAI_FEISHU_APP_SECRET
    if (!appId || !appSecret) {
      throw new Error("缺少飞书应用凭证：请配置 FEISHU_GROUP_APP_ID/FEISHU_GROUP_APP_SECRET（或全局 GEBAI_FEISHU_APP_ID/GEBAI_FEISHU_APP_SECRET）")
    }
    return { appId, appSecret }
  }

  async function getTenantToken(ctx: ToolContext): Promise<string> {
    const { appId, appSecret } = readConfig(ctx)
    const cached = deps.tokenCache.get(appId)
    if (cached && cached.expireAt > Date.now() + TOKEN_REFRESH_EARLY_MS) return cached.token
    const res = await deps.fetchFn(`${BASE_URL}/open-apis/auth/v3/tenant_access_token/internal`, {
      method: "POST",
      headers: { "Content-Type": "application/json; charset=utf-8" },
      body: JSON.stringify({ app_id: appId, app_secret: appSecret }),
      signal: AbortSignal.timeout(API_TIMEOUT_MS),
    })
    const json = (await res.json()) as { code?: number; msg?: string; tenant_access_token?: string; expire?: number }
    if (json.code !== 0 || !json.tenant_access_token) throw new Error(`飞书凭证获取失败: code=${json.code} ${json.msg ?? ""}`)
    deps.tokenCache.set(appId, { token: json.tenant_access_token, expireAt: Date.now() + Math.max(60, json.expire ?? 7200) * 1000 })
    return json.tenant_access_token
  }

  /** 统一请求：携带 tenant token，解析飞书业务码（非 0 抛错带可读提示）。 */
  async function request<T = Record<string, unknown>>(ctx: ToolContext, path: string, init: RequestInit = {}): Promise<T> {
    const token = await getTenantToken(ctx)
    const res = await deps.fetchFn(`${BASE_URL}${path}`, {
      ...init,
      headers: { "Content-Type": "application/json; charset=utf-8", Authorization: `Bearer ${token}`, ...(init.headers ?? {}) },
      signal: AbortSignal.timeout(API_TIMEOUT_MS),
    })
    let json: Record<string, unknown>
    try {
      json = (await res.json()) as Record<string, unknown>
    } catch {
      throw new Error(`飞书 API ${init.method ?? "GET"} ${path} 响应非 JSON (HTTP ${res.status})`)
    }
    if (json.code !== 0) throw new Error(`飞书 API ${init.method ?? "GET"} ${path}: code=${json.code} ${json.msg ?? ""}（权限不足时到开发者后台开通对应 scope 并发布版本）`)
    return json as T
  }

  const chatsList: Tool = {
    name: "chats_list",
    description: "列出机器人所在的全部群（chat_id/名称/描述，分页）。定时任务通知指定群推送时用本工具取群 chat_id。",
    parameters: schema({ page_size: { type: "number", description: "每页数量（默认 20，上限 100）" }, page_token: { type: "string", description: "分页标记（首次不传）" } }),
    async execute(args, ctx) {
      const qs = new URLSearchParams({ page_size: String(Math.min(100, Math.max(1, Number(args.page_size) || 20))) })
      if (args.page_token) qs.set("page_token", String(args.page_token))
      const json = await request<{ data?: { items?: Array<Record<string, unknown>>; has_more?: boolean; page_token?: string } }>(ctx, `/open-apis/im/v1/chats?${qs}`)
      const items = json.data?.items ?? []
      const lines = items.map((c) => `- ${String(c.chat_id ?? "")} ${String(c.name ?? "")}${c.description ? `（${String(c.description)}）` : ""}`)
      const out = lines.length ? lines.join("\n") : "机器人尚未加入任何群。"
      return { output: clip(out + (json.data?.has_more ? `\n（还有更多，传 page_token="${json.data?.page_token ?? ""}" 继续翻页）` : "")) }
    },
  }

  const chatInfo: Tool = {
    name: "chat_info",
    description: "查询群详情（名称/描述/群主 open_id/成员数/群形态）。",
    parameters: schema({ chat_id: { type: "string", description: "群 chat_id（chats_list 查看）" } }, ["chat_id"]),
    async execute(args, ctx) {
      const json = await request<{ data?: Record<string, unknown> }>(ctx, `/open-apis/im/v1/chats/${encodeURIComponent(String(args.chat_id))}`)
      const d = json.data ?? {}
      return {
        output: clip(
          `chat_id: ${String(d.chat_id ?? "")}\n名称: ${String(d.name ?? "")}\n描述: ${String(d.description ?? "")}\n群主: ${String(d.owner_id ?? "")}\n成员数: ${String(d.user_count ?? d.member_count ?? "")}\n外部群: ${d.external ? "是" : "否"}`,
          12000,
        ),
      }
    },
  }

  const membersList: Tool = {
    name: "members_list",
    description: "查询群成员分页列表（open_id + 姓名 + 成员类型）。@ 特定人（cron_add 的 at 名单 / message_send 的 @ 标签）需要的 open_id 用本工具查。",
    parameters: schema(
      {
        chat_id: { type: "string", description: "群 chat_id（chats_list 查看）" },
        page_size: { type: "number", description: "每页数量（默认 50，上限 100）" },
        page_token: { type: "string", description: "分页标记（首次不传）" },
      },
      ["chat_id"],
    ),
    async execute(args, ctx) {
      const qs = new URLSearchParams({ member_id_type: "open_id", page_size: String(Math.min(100, Math.max(1, Number(args.page_size) || 50))) })
      if (args.page_token) qs.set("page_token", String(args.page_token))
      const json = await request<{ data?: { items?: Array<Record<string, unknown>>; has_more?: boolean; page_token?: string } }>(
        ctx,
        `/open-apis/im/v1/chats/${encodeURIComponent(String(args.chat_id))}/members?${qs}`,
      )
      const items = json.data?.items ?? []
      const lines = items.map((m) => `- ${String(m.member_id ?? "")} ${String(m.name ?? "")}${m.type ? `（${String(m.type)}）` : ""}`)
      const out = lines.length ? lines.join("\n") : "群内无成员。"
      return { output: clip(out + (json.data?.has_more ? `\n（还有更多，传 page_token="${json.data?.page_token ?? ""}" 继续翻页）` : "")) }
    },
  }

  const userInfo: Tool = {
    name: "user_info",
    description: "按 open_id 查询用户信息（姓名/英文姓名/账号等）——核对 at 名单里的 open_id 是否为目标人。",
    parameters: schema({ open_id: { type: "string", description: "用户 open_id（ou_ 前缀，members_list 查看）" } }, ["open_id"]),
    async execute(args, ctx) {
      const json = await request<{ data?: { user?: Record<string, unknown> } }>(
        ctx,
        `/open-apis/contact/v3/users/${encodeURIComponent(String(args.open_id))}?user_id_type=open_id`,
      )
      const u = json.data?.user ?? {}
      return { output: clip(`open_id: ${String(u.open_id ?? args.open_id)}\n姓名: ${String(u.name ?? "")}\n英文姓名: ${String(u.en_name ?? "")}\n账号: ${String(u.enterprise_email ?? u.email ?? "")}`) }
    },
  }

  const messageSend: Tool = {
    name: "message_send",
    description: "向指定群发送文本消息（应用身份）。正文支持 <at user_id=\"open_id\">名字</at> @特定人 与 <at user_id=\"all\">所有人</at>；可用于验证通知效果或直接推送内容。",
    requiresApproval: true,
    parameters: schema(
      {
        chat_id: { type: "string", description: "群 chat_id（chats_list 查看）" },
        text: { type: "string", description: "消息正文（支持 at 标签）" },
      },
      ["chat_id", "text"],
    ),
    async execute(args, ctx) {
      const json = await request<{ data?: { message_id?: string } }>(ctx, `/open-apis/im/v1/messages?receive_id_type=chat_id`, {
        method: "POST",
        body: JSON.stringify({ receive_id: String(args.chat_id), msg_type: "text", content: JSON.stringify({ text: String(args.text) }) }),
      })
      return { output: `消息已发送: ${json.data?.message_id ?? ""}` }
    },
  }

  const chatCreate: Tool = {
    name: "chat_create",
    description: "创建群（指定群名，可选描述与初始成员 open_id 列表；机器人自动入群并成为群主）。返回新群 chat_id。",
    requiresApproval: true,
    parameters: schema({ name: { type: "string", description: "群名称" }, description: { type: "string", description: "群描述（可选）" }, user_id_list: { type: "array", items: { type: "string" }, description: "初始成员 open_id 列表（可选）" } }, ["name"]),
    async execute(args, ctx) {
      const body: Record<string, unknown> = { name: String(args.name) }
      if (args.description) body.description = String(args.description)
      const users = Array.isArray(args.user_id_list) ? args.user_id_list.map(String).filter(Boolean) : []
      if (users.length) body.user_id_list = users
      const json = await request<{ data?: { chat_id?: string } }>(ctx, `/open-apis/im/v1/chats`, { method: "POST", body: JSON.stringify(body) })
      return { output: `群已创建: ${json.data?.chat_id ?? ""}（${String(args.name)}）` }
    },
  }

  const chatUpdate: Tool = {
    name: "chat_update",
    description: "更新群信息（群名/群描述，至少传一项）。",
    requiresApproval: true,
    parameters: schema({ chat_id: { type: "string", description: "群 chat_id" }, name: { type: "string", description: "新群名（可选）" }, description: { type: "string", description: "新群描述（可选）" } }, ["chat_id"]),
    async execute(args, ctx) {
      const body: Record<string, unknown> = {}
      if (args.name != null) body.name = String(args.name)
      if (args.description != null) body.description = String(args.description)
      if (!Object.keys(body).length) return { output: "未指定修改项（name/description 至少传一项）" }
      await request(ctx, `/open-apis/im/v1/chats/${encodeURIComponent(String(args.chat_id))}`, { method: "PUT", body: JSON.stringify(body) })
      return { output: `群信息已更新: ${args.chat_id}` }
    },
  }

  const chatMembersAdd: Tool = {
    name: "chat_members_add",
    description: "拉用户进群（open_id 列表）。失败项会逐条给出原因（未开通/无权限等）。",
    requiresApproval: true,
    parameters: schema(
      {
        chat_id: { type: "string", description: "群 chat_id" },
        open_ids: { type: "array", items: { type: "string" }, description: "要拉入的成员 open_id 列表（members_list/user_info 取）" },
      },
      ["chat_id", "open_ids"],
    ),
    async execute(args, ctx) {
      const ids = (Array.isArray(args.open_ids) ? args.open_ids : []).map(String).filter(Boolean)
      if (!ids.length) return { output: "open_ids 为空" }
      const json = await request<{ data?: { failed_items?: Array<Record<string, unknown>>; invalid_id_list?: string[] } }>(
        ctx,
        `/open-apis/im/v1/chats/${encodeURIComponent(String(args.chat_id))}/members`,
        { method: "POST", body: JSON.stringify({ id_list: ids, member_id_type: "open_id" }) },
      )
      const failed = json.data?.failed_items ?? json.data?.invalid_id_list ?? []
      const failLines = failed.map((f) => (typeof f === "string" ? `- ${f}` : `- ${String(f.member_id ?? "")}（${String(f.reason ?? f.msg ?? "")}）`)).join("\n")
      return { output: failed.length ? `部分成员拉入失败:\n${failLines}` : `已拉入 ${ids.length} 名成员` }
    },
  }

  const chatMembersRemove: Tool = {
    name: "chat_members_remove",
    description: "将成员移出群（open_id 列表；群主不可被移出）。",
    requiresApproval: true,
    parameters: schema(
      {
        chat_id: { type: "string", description: "群 chat_id" },
        open_ids: { type: "array", items: { type: "string" }, description: "要移出的成员 open_id 列表" },
      },
      ["chat_id", "open_ids"],
    ),
    async execute(args, ctx) {
      const ids = (Array.isArray(args.open_ids) ? args.open_ids : []).map(String).filter(Boolean)
      if (!ids.length) return { output: "open_ids 为空" }
      const qs = new URLSearchParams({ member_id_type: "open_id" })
      await request(ctx, `/open-apis/im/v1/chats/${encodeURIComponent(String(args.chat_id))}/members?${qs}`, { method: "DELETE", body: JSON.stringify({ id_list: ids }) })
      return { output: `已移出 ${ids.length} 名成员` }
    },
  }

  const chatDisband: Tool = {
    name: "chat_disband",
    description: "解散群（不可恢复，执行前必须向用户确认）。",
    requiresApproval: true,
    parameters: schema({ chat_id: { type: "string", description: "群 chat_id" } }, ["chat_id"]),
    async execute(args, ctx) {
      await request(ctx, `/open-apis/im/v1/chats/${encodeURIComponent(String(args.chat_id))}`, { method: "DELETE" })
      return { output: `群已解散: ${args.chat_id}` }
    },
  }

  return { chats_list: chatsList, chat_info: chatInfo, members_list: membersList, user_info: userInfo, message_send: messageSend, chat_create: chatCreate, chat_update: chatUpdate, chat_members_add: chatMembersAdd, chat_members_remove: chatMembersRemove, chat_disband: chatDisband }
}

export const tools = createFeishuGroupTools()
export const requiresApproval = {
  chats_list: false,
  chat_info: false,
  members_list: false,
  user_info: false,
  message_send: true,
  chat_create: true,
  chat_update: true,
  chat_members_add: true,
  chat_members_remove: true,
  chat_disband: true,
}
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars }
