import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "../../core/types"
import { createFeishuGroupTools, def, envVars, name, requiresApproval, tools as defTools } from "./feishu_group"

type Req = { url: string; init?: RequestInit }

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } })
}

function ctx(home: string, env: Record<string, string> = {}): ToolContext {
  return {
    user: "default",
    sessionId: "s1",
    workdir: home,
    home,
    env,
    sandboxed: false,
    resolvePath: (p) => p,
    readFile: async () => "x",
    readBinaryFile: async () => new Uint8Array(),
    writeFile: async () => {},
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (ref) => ref.name,
    publish: () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    runNewSession: async () => ({ output: "", archive: {} as never }),
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => null,
    waitForCapture: async () => null,
    projects: [],
    resolveProjectPath: () => home,
    getTodos: async () => [],
    setTodos: async () => {},
  }
}

/** 注入 mock fetch 的工具集：自动应答 tenant_access_token，其余请求交给 handler。 */
function makeTools(handler: (req: Req) => Response | Promise<Response>, creds = { FEISHU_GROUP_APP_ID: "cli_x", FEISHU_GROUP_APP_SECRET: "sec" }) {
  const records: Req[] = []
  const tools = createFeishuGroupTools({
    tokenCache: new Map(),
    fetchFn: (async (url: string | URL | Request, init?: RequestInit) => {
      const req: Req = { url: String(url), init }
      records.push(req)
      if (req.url.endsWith("/open-apis/auth/v3/tenant_access_token/internal")) {
        return jsonResponse({ code: 0, tenant_access_token: "tt-1", expire: 7200 })
      }
      return await handler(req)
    }) as unknown as typeof fetch,
  })
  return { tools, records, home: mkdtempSync(join(tmpdir(), "gebai-fg-")), creds }
}

describe("feishu_group sub-agent", () => {
  test("def 结构：命名/工具面/审批姿态/envVars 声明", () => {
    expect(name).toBe("feishu_group")
    expect(Object.keys(defTools).sort()).toEqual([
      "chat_create", "chat_disband", "chat_info", "chat_members_add", "chat_members_remove", "chat_update", "chats_list", "members_list", "message_send", "user_info",
    ])
    // 查询类免审批；写操作（发消息/建群/改群/拉人/移人/解散）全部需审批
    expect(requiresApproval).toEqual({
      chats_list: false, chat_info: false, members_list: false, user_info: false,
      message_send: true, chat_create: true, chat_update: true, chat_members_add: true, chat_members_remove: true, chat_disband: true,
    })
    expect(defTools.members_list.requiresApproval).toBeFalsy()
    expect(defTools.chat_disband.requiresApproval).toBe(true)
    expect(envVars.map((v) => v.name)).toEqual(["FEISHU_GROUP_APP_ID", "FEISHU_GROUP_APP_SECRET"])
    expect(def.preload).toBe(false)
    expect(def.name).toBe("feishu_group")
  })

  test("chats_list / members_list：分页拉取群与成员（open_id+姓名）", async () => {
    const { tools, records, home, creds } = makeTools((req) => {
      if (req.url.includes("/open-apis/im/v1/chats?")) {
        const token = new URL(req.url).searchParams.get("page_token")
        return token
          ? jsonResponse({ code: 0, data: { items: [{ chat_id: "oc_2", name: "运维二群" }], has_more: false } })
          : jsonResponse({ code: 0, data: { items: [{ chat_id: "oc_1", name: "告警群", description: "值班" }], has_more: true, page_token: "pg2" } })
      }
      if (req.url.includes("/members")) {
        return jsonResponse({ code: 0, data: { items: [{ member_id: "ou_aaa", name: "张三" }, { member_id: "ou_bbb", name: "李四" }], has_more: false } })
      }
      return jsonResponse({ code: 0 })
    })
    try {
      const r1 = await tools.chats_list.execute({}, ctx(home, creds))
      expect(r1.output).toContain("oc_1")
      expect(r1.output).toContain("告警群")
      expect(r1.output).toContain("pg2")
      const r2 = await tools.chats_list.execute({ page_token: "pg2" }, ctx(home, creds))
      expect(r2.output).toContain("oc_2")
      const m = await tools.members_list.execute({ chat_id: "oc_1" }, ctx(home, creds))
      expect(m.output).toContain("ou_aaa 张三")
      expect(m.output).toContain("ou_bbb 李四")
      // 成员查询携带 open_id 形态与分页参数
      const memberReq = records.find((r) => r.url.includes("/members"))!
      expect(memberReq.url).toContain("member_id_type=open_id")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("message_send：文本正文原样携带 at 标签（@特定人/@所有人）", async () => {
    const { tools, records, home, creds } = makeTools(() => jsonResponse({ code: 0, data: { message_id: "om_1" } }))
    try {
      const r = await tools.message_send.execute({ chat_id: "oc_1", text: '<at user_id="ou_aaa">张三</at> <at user_id="all">所有人</at> 巡检报告已生成' }, ctx(home, creds))
      expect(r.output).toContain("om_1")
      const sent = JSON.parse(String(records.find((r2) => r2.url.includes("/im/v1/messages"))!.init!.body))
      expect(sent.receive_id).toBe("oc_1")
      expect(sent.msg_type).toBe("text")
      expect(JSON.parse(sent.content).text).toContain('<at user_id="ou_aaa">张三</at>')
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("写操作端点与参数（建群/拉人/移人/改群/解散）+ 业务错误码可读化", async () => {
    const { tools, records, home, creds } = makeTools(() => jsonResponse({ code: 0, data: { chat_id: "oc_new" } }))
    try {
      await tools.chat_create.execute({ name: "值班群", user_id_list: ["ou_aaa"] }, ctx(home, creds))
      const createReq = records.find((r) => r.url.endsWith("/open-apis/im/v1/chats") && r.init?.method === "POST")!
      expect(JSON.parse(String(createReq.init!.body))).toEqual({ name: "值班群", user_id_list: ["ou_aaa"] })

      const add = await tools.chat_members_add.execute({ chat_id: "oc_new", open_ids: ["ou_aaa", "ou_bbb"] }, ctx(home, creds))
      expect(add.output).toContain("已拉入 2 名成员")
      const addReq = records.filter((r) => r.url.includes("/members") && r.init?.method === "POST").pop()!
      expect(JSON.parse(String(addReq.init!.body))).toEqual({ id_list: ["ou_aaa", "ou_bbb"], member_id_type: "open_id" })

      await tools.chat_members_remove.execute({ chat_id: "oc_new", open_ids: ["ou_bbb"] }, ctx(home, creds))
      const rmReq = records.filter((r) => r.url.includes("/members") && r.init?.method === "DELETE").pop()!
      expect(rmReq.url).toContain("member_id_type=open_id")
      expect(JSON.parse(String(rmReq.init!.body))).toEqual({ id_list: ["ou_bbb"] })

      await tools.chat_update.execute({ chat_id: "oc_new", name: "值班群2" }, ctx(home, creds))
      expect(records.some((r) => r.url.endsWith("/chats/oc_new") && r.init?.method === "PUT")).toBe(true)
      const disband = await tools.chat_disband.execute({ chat_id: "oc_new" }, ctx(home, creds))
      expect(disband.output).toContain("已解散")
      expect(records.some((r) => r.url.endsWith("/chats/oc_new") && r.init?.method === "DELETE")).toBe(true)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("缺少凭证与飞书业务错误均抛可读错误", async () => {
    const { tools, home } = makeTools(() => jsonResponse({ code: 0 }))
    try {
      await expect(tools.chats_list.execute({}, ctx(home))).rejects.toThrow(/缺少飞书应用凭证/)
      await expect(tools.chats_list.execute({}, ctx(home, { GEBAI_FEISHU_APP_ID: "g", GEBAI_FEISHU_APP_SECRET: "s" }))).resolves.toBeTruthy() // 全局凭证回落
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
    const fail = makeTools(() => jsonResponse({ code: 230002, msg: "not in chat" }))
    try {
      await expect(fail.tools.members_list.execute({ chat_id: "oc_x" }, ctx(fail.home, fail.creds))).rejects.toThrow(/230002/)
    } finally {
      rmSync(fail.home, { recursive: true, force: true })
    }
  })
})
