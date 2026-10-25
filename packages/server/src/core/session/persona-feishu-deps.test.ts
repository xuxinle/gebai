/**
 * 拟人飞书事件源的宿主依赖注入：**两处最容易配错、错了又最难查的地方**。
 *
 * 1. **长连接撞车**：同一飞书应用只能一条长连接。撞了会互踢——现象是「两边都偶发收不到
 *    消息」，日志只有零星重连，看不出根因。所以必须**启动期拒绝并说清楚怎么改**。
 * 2. **读历史降级**：应用身份只能读机器人所在群。退回应用身份却不说明，会让
 *    「这个群为什么没历史」永远查不出来——它看起来完全像「这个群就是没历史」。
 */
import { describe, expect, test } from "bun:test"
import { createFeishuSourceDeps, parseBody, type FeishuHistoryResult } from "./persona-feishu-deps"

const base = {
  appId: () => "cli_persona",
  appSecret: () => "secret",
  sessionBot: () => ({ enabled: false }),
  makeConn: () => ({ start: async () => {}, stop: () => {} }),
}

/** fetch 替身：按调用次序给出响应（记录请求头里的 token 以断言用了哪种身份）。 */
function fakeFetch(opts: { items?: unknown[]; code?: number; failOnUser?: boolean } = {}) {
  const calls: Array<{ auth: string }> = []
  const impl = async (_url: string, init: RequestInit) => {
    const auth = String((init.headers as Record<string, string>)?.authorization ?? "")
    calls.push({ auth })
    if (opts.failOnUser && auth.includes("user-")) {
      return { ok: true, status: 200, json: async () => ({ code: 99991663, msg: "user token invalid" }) }
    }
    return { ok: true, status: 200, json: async () => ({ code: opts.code ?? 0, data: { items: opts.items ?? [] } }) }
  }
  return { impl, calls }
}

describe("createFeishuSourceDeps：长连接注入与撞车", () => {
  test("正常：注入 feishuLongConn 与 feishuHistory", () => {
    const deps = createFeishuSourceDeps(base)
    expect(typeof deps.feishuLongConn).toBe("function")
    expect(typeof deps.feishuHistory).toBe("function")
  })

  test("未配凭据 → 不注入（源启动时报「缺凭据」，那条信息更贴近使用者）", () => {
    const deps = createFeishuSourceDeps({ ...base, appId: () => undefined })
    expect(deps.feishuLongConn).toBeUndefined()
    expect(deps.feishuHistory).toBeUndefined()
  })

  test("与会话机器人同应用 → **拒绝注入并说明后果**（否则两条长连接互踢）", () => {
    const warnings: string[] = []
    const deps = createFeishuSourceDeps({
      ...base,
      appId: () => "cli_same",
      sessionBot: () => ({ enabled: true, appId: "cli_same" }),
      log: { info: () => {}, warn: (m) => void warnings.push(m) },
    })
    expect(deps.feishuLongConn).toBeUndefined()
    expect(warnings.join("\n")).toContain("互踢")
    expect(warnings.join("\n")).toContain("单独建一个应用")
  })

  test("机器人未启用 → 同 AppID 也允许（不会互踢，因为只有一条连接）", () => {
    const deps = createFeishuSourceDeps({
      ...base,
      appId: () => "cli_same",
      sessionBot: () => ({ enabled: false, appId: "cli_same" }),
    })
    expect(typeof deps.feishuLongConn).toBe("function")
  })
})

describe("feishuHistory：用户身份优先，降级如实上报", () => {
  const history = (deps: Record<string, unknown>) => deps.feishuHistory as (i: { chatId: string; limit: number }) => Promise<FeishuHistoryResult>

  test("有用户令牌 → 用用户身份读，readIdentity=user", async () => {
    const { impl, calls } = fakeFetch({ items: [{ body: { content: '{"text":"早"}' }, msg_type: "text", sender: { name: "张三" } }] })
    const deps = createFeishuSourceDeps({
      ...base,
      fetchImpl: impl,
      userToken: async () => ({ token: { accessToken: "user-token" } }),
      appToken: async () => "app-token",
    })
    const r = await history(deps)({ chatId: "oc_1", limit: 5 })
    expect(r.readIdentity).toBe("user")
    expect(calls[0]!.auth).toContain("user-token")
    expect(r.messages[0]!.senderName).toBe("张三")
  })

  test("没有用户令牌 → 退回应用身份，**带上原因**（静默降级就查不出“为什么没历史”）", async () => {
    const { impl, calls } = fakeFetch()
    const deps = createFeishuSourceDeps({
      ...base,
      fetchImpl: impl,
      userToken: async () => ({ reason: "尚未完成飞书用户授权" }),
      appToken: async () => "app-token",
    })
    const r = await history(deps)({ chatId: "oc_1", limit: 5 })
    expect(r.readIdentity).toBe("app")
    expect(r.fallbackReason).toContain("尚未完成飞书用户授权")
    expect(calls[0]!.auth).toContain("app-token")
  })

  test("用户令牌失效（接口拒绝）→ 再试应用身份，仍把根因带出", async () => {
    const { impl } = fakeFetch({ failOnUser: true })
    const deps = createFeishuSourceDeps({
      ...base,
      fetchImpl: impl,
      userToken: async () => ({ token: { accessToken: "user-bad" } }),
      appToken: async () => "app-token",
    })
    const r = await history(deps)({ chatId: "oc_1", limit: 5 })
    expect(r.readIdentity).toBe("app")
    expect(r.fallbackReason).toContain("99991663")
  })

  test("两种身份都不可用 → 抛错（不返回空历史假装成功）", async () => {
    const deps = createFeishuSourceDeps({
      ...base,
      userToken: async () => ({ reason: "未授权" }),
    })
    await expect(history(deps)({ chatId: "oc_1", limit: 5 })).rejects.toThrow(/未配置任何可用身份|用户授权不可用/)
  })

  test("飞书返回倒序 → 翻转成时间正序（模型按时间读「之前发生了什么」）", async () => {
    const { impl } = fakeFetch({
      items: [
        { body: { content: '{"text":"第三条"}' }, msg_type: "text", create_time: "3000" },
        { body: { content: '{"text":"第二条"}' }, msg_type: "text", create_time: "2000" },
        { body: { content: '{"text":"第一条"}' }, msg_type: "text", create_time: "1000" },
      ],
    })
    const deps = createFeishuSourceDeps({ ...base, fetchImpl: impl, appToken: async () => "app" })
    const r = await history(deps)({ chatId: "oc_1", limit: 5 })
    expect(r.messages.map((m) => m.text)).toEqual(["第一条", "第二条", "第三条"])
  })

  test("limit 夹取到 1..50（越界值不该打穿接口）", async () => {
    const { impl } = fakeFetch()
    const deps = createFeishuSourceDeps({ ...base, fetchImpl: impl, appToken: async () => "app" })
    // 只验不抛错；上限由查询参数内的 page_size 决定（夹取逻辑在实现里）
    await expect(history(deps)({ chatId: "oc_1", limit: 999 })).resolves.toBeDefined()
  })
})

describe("parseBody：正文解析", () => {
  test("text 类型取 content.text", () => {
    expect(parseBody("text", '{"text":"你好"}')).toBe("你好")
  })

  test("富文本（post）拼出可读纯文本（比「[post 消息]」有用得多）", () => {
    const post = JSON.stringify({ title: "标题", content: [[{ tag: "text", text: "第一段" }], [{ tag: "text", text: "第二段" }]] })
    expect(parseBody("post", post)).toBe("标题 第一段 第二段")
  })

  test("图片/文件 → 可读占位（不猜内容）", () => {
    expect(parseBody("image", "{}")).toContain("image")
    expect(parseBody("file", "{}")).toContain("file")
  })

  test("空 content → 空串", () => {
    expect(parseBody("text", "")).toBe("")
  })

  test("content 非 JSON 时原样返回（不丢正文）", () => {
    expect(parseBody("text", "不是 JSON")).toBe("不是 JSON")
  })
})
