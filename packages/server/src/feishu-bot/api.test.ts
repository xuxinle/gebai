import { describe, expect, test } from "bun:test"
import { createHmac } from "node:crypto"
import { createFeishuApi, verifyFeishuSignature } from "./api"

interface FakeResponse {
  ok: boolean
  status: number
  body: unknown
  json(): Promise<unknown>
  arrayBuffer(): Promise<ArrayBuffer>
}
function jsonResponse(body: unknown, status = 200): FakeResponse {
  return { ok: status >= 200 && status < 300, status, body, json: async () => body, arrayBuffer: async () => new ArrayBuffer(0) }
}

/** 记录调用的 fake fetch。 */
function makeFetch() {
  const calls: Array<{ url: string; init: RequestInit }> = []
  const fetchImpl = async (url: string, init: RequestInit) => {
    calls.push({ url, init })
    if (url.includes("/auth/v3/tenant_access_token/internal")) {
      return jsonResponse({ code: 0, tenant_access_token: "t-token", expire: 7200 })
    }
    if (url.includes("/im/v1/messages?") && init.method === "POST") {
      return jsonResponse({ code: 0, data: { message_id: "om_sent" } })
    }
    if (url.includes("/reply") && init.method === "POST") {
      return jsonResponse({ code: 0, data: { message_id: "om_reply" } })
    }
    if (url.includes("/im/v1/messages/") && init.method === "DELETE") {
      return jsonResponse({ code: 0 })
    }
    if (url.includes("/reactions") && init.method === "POST") {
      return jsonResponse({ code: 0, data: { reaction_id: "re_1" } })
    }
    if (url.includes("/reactions/") && init.method === "DELETE") {
      return jsonResponse({ code: 0 })
    }
    if (url.includes("/resources/")) {
      return jsonResponse({ code: 0 }) // 资源下载为二进制，下面单独覆盖
    }
    if (url.includes("/im/v1/chats/")) {
      return jsonResponse({ code: 0, data: { name: "测试群" } })
    }
    if (url.includes("/im/v1/images")) {
      return jsonResponse({ code: 0, data: { image_key: "img_v2_uploaded" } })
    }
    return jsonResponse({ code: 0 })
  }
  return { calls, fetchImpl }
}

describe("createFeishuApi", () => {
  test("tenant_access_token 获取并缓存", async () => {
    const { calls, fetchImpl } = makeFetch()
    const api = createFeishuApi({ appId: "cli_a", appSecret: "sec", fetchImpl })
    const t1 = await api.getTenantToken()
    const t2 = await api.getTenantToken()
    expect(t1).toBe("t-token")
    expect(t2).toBe("t-token")
    expect(calls.filter((c) => c.url.includes("tenant_access_token"))).toHaveLength(1)
    const body = JSON.parse(String(calls[0].init.body))
    expect(body).toEqual({ app_id: "cli_a", app_secret: "sec" })
  })

  test("token 过期后重新获取（提前 60s 阈值）", async () => {
    let now = 0
    let tokenCalls = 0
    const { fetchImpl } = makeFetch()
    const counting = async (url: string, init: RequestInit) => {
      if (url.includes("tenant_access_token")) tokenCalls++
      return fetchImpl(url, init)
    }
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl: counting, clock: () => now })
    await api.getTenantToken()
    await api.getTenantToken()
    expect(tokenCalls).toBe(1) // 缓存命中
    now = 7150 * 1000 // expire 7200 - 60 阈值之后
    await api.getTenantToken()
    expect(tokenCalls).toBe(2) // 过期重取
  })

  test("sendMessage 组装参数并返回 message_id", async () => {
    const { calls, fetchImpl } = makeFetch()
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl })
    const id = await api.sendMessage({ receiveId: "oc_1", receiveIdType: "chat_id", msgType: "text", content: { text: "你好" } })
    expect(id).toBe("om_sent")
    const call = calls.find((c) => c.url.includes("/im/v1/messages?"))!
    expect(call.url).toContain("receive_id_type=chat_id")
    const body = JSON.parse(String(call.init.body)) as Record<string, unknown>
    expect(body.receive_id).toBe("oc_1")
    expect(body.msg_type).toBe("text")
    expect(body.content).toBe('{"text":"你好"}')
    expect(call.init.headers).toMatchObject({ Authorization: "Bearer t-token" })
  })

  test("deleteMessage 成功/失败容错", async () => {
    let fail = false
    const { fetchImpl } = makeFetch()
    const wrapped = async (url: string, init: RequestInit) => {
      if (fail && init.method === "DELETE") return jsonResponse({ code: 99999, msg: "err" }, 400)
      return fetchImpl(url, init)
    }
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl: wrapped })
    expect(await api.deleteMessage("om_x")).toBe(true)
    fail = true
    expect(await api.deleteMessage("om_x")).toBe(false)
  })

  test("replyMessage 回复指定消息（引用气泡）", async () => {
    const { calls, fetchImpl } = makeFetch()
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl })
    const id = await api.replyMessage("om_x", "text", { text: "你好" })
    expect(id).toBe("om_reply")
    const call = calls.find((c) => c.url.includes("/reply"))!
    expect(call.url).toContain("/im/v1/messages/om_x/reply")
    expect(call.init.method).toBe("POST")
    const body = JSON.parse(String(call.init.body)) as Record<string, unknown>
    expect(body.msg_type).toBe("text")
    expect(body.content).toBe('{"text":"你好"}')
    expect(call.init.headers).toMatchObject({ Authorization: "Bearer t-token" })
  })

  test("addMessageReaction 添加表情反应（返回 reaction_id）", async () => {
    const { calls, fetchImpl } = makeFetch()
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl })
    expect(await api.addMessageReaction("om_x", "Typing")).toBe("re_1")
    const call = calls.find((c) => c.url.includes("/reactions") && c.init.method === "POST")!
    expect(call.url).toContain("/im/v1/messages/om_x/reactions")
    expect(JSON.parse(String(call.init.body))).toEqual({ reaction_type: { emoji_type: "Typing" } })
  })

  test("addMessageReaction 失败返回 null", async () => {
    let fail = false
    const { fetchImpl } = makeFetch()
    const wrapped = async (url: string, init: RequestInit) => {
      if (fail && url.includes("/reactions") && init.method === "POST") return jsonResponse({ code: 99999, msg: "err" }, 400)
      return fetchImpl(url, init)
    }
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl: wrapped })
    expect(await api.addMessageReaction("om_x", "Typing")).toBe("re_1")
    fail = true
    expect(await api.addMessageReaction("om_x", "Typing")).toBeNull()
  })

  test("deleteMessageReaction 删除表情反应（成功/失败容错）", async () => {
    let fail = false
    const { calls, fetchImpl } = makeFetch()
    const wrapped = async (url: string, init: RequestInit) => {
      if (fail && url.includes("/reactions/") && init.method === "DELETE") return jsonResponse({ code: 99999, msg: "err" }, 400)
      return fetchImpl(url, init)
    }
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl: wrapped })
    expect(await api.deleteMessageReaction("om_x", "re_1")).toBe(true)
    const call = calls.find((c) => c.url.includes("/reactions/re_1") && c.init.method === "DELETE")!
    expect(call.url).toContain("/im/v1/messages/om_x/reactions/re_1")
    fail = true
    expect(await api.deleteMessageReaction("om_x", "re_1")).toBe(false)
  })

  test("downloadResource 返回字节", async () => {
    const bytes = new TextEncoder().encode("imgdata").buffer
    const fetchImpl = async (url: string, _init: RequestInit) => {
      if (url.includes("/resources/")) {
        return { ok: true, status: 200, json: async () => ({}), arrayBuffer: async () => bytes }
      }
      return jsonResponse({ code: 0, tenant_access_token: "t", expire: 7200 })
    }
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl })
    const data = await api.downloadResource("om_1", "img_v2_k", "image")
    expect(new TextDecoder().decode(data)).toBe("imgdata")
  })

  test("downloadResource 业务错误码（HTTP 200 + JSON）抛错", async () => {
    const fetchImpl = async (url: string, _init: RequestInit) => {
      if (url.includes("/resources/")) {
        const body = JSON.stringify({ code: 99991661, msg: "invalid param" })
        return { ok: true, status: 200, json: async () => ({}), arrayBuffer: async () => new TextEncoder().encode(body).buffer }
      }
      return jsonResponse({ code: 0, tenant_access_token: "t", expire: 7200 })
    }
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl })
    await expect(api.downloadResource("om_1", "k", "image")).rejects.toThrow(/99991661/)
  })

  test("getChatName 成功与失败容错", async () => {
    const { fetchImpl } = makeFetch()
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl })
    expect(await api.getChatName("oc_1")).toBe("测试群")
    const failing = async () => jsonResponse({ code: 999, msg: "not found" }, 404)
    const api2 = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl: failing })
    expect(await api2.getChatName("oc_1")).toBeNull()
  })

  test("uploadImage 组装 multipart 表单并返回 image_key", async () => {
    const { calls, fetchImpl } = makeFetch()
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl })
    const png = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 1, 2, 3])
    const key = await api.uploadImage(png, "image/png", "diagram.png")
    expect(key).toBe("img_v2_uploaded")
    const call = calls.find((c) => c.url.includes("/im/v1/images"))!
    expect(call.url).toBe("https://open.feishu.cn/open-apis/im/v1/images")
    expect(call.init.method).toBe("POST")
    expect(call.init.headers).toMatchObject({ Authorization: "Bearer t-token" })
    // multipart 表单内容（fetch 生成边界；解析字段校验）
    const body = call.init.body as FormData
    expect(body).toBeInstanceOf(FormData)
    const parts: Record<string, unknown> = {}
    for (const [k, v] of body as unknown as Iterable<[string, Blob | string]>) parts[k] = v
    expect(parts.image_type).toBe("message")
    const file = parts.image as Blob
    expect(file.type).toBe("image/png")
    expect(new Uint8Array(await file.arrayBuffer())).toEqual(png)
  })

  test("uploadImage 响应缺 image_key 抛错", async () => {
    const fetchImpl = async (url: string, _init: RequestInit) => {
      if (url.includes("/auth/")) return jsonResponse({ code: 0, tenant_access_token: "t", expire: 7200 })
      return jsonResponse({ code: 0, data: {} })
    }
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl })
    await expect(api.uploadImage(new Uint8Array([1]), "image/png")).rejects.toThrow(/image_key/)
  })

  test("业务码非 0 抛错", async () => {
    const fetchImpl = async () => jsonResponse({ code: 99991661, msg: "invalid param" }, 200)
    const api = createFeishuApi({ appId: "a", appSecret: "s", fetchImpl })
    await expect(api.sendMessage({ receiveId: "x", receiveIdType: "chat_id", msgType: "text", content: { text: "x" } })).rejects.toThrow(/99991661/)
  })
})

describe("verifyFeishuSignature", () => {
  test("HMAC 签名校验（timestamp+nonce+body）", () => {
    const secret = "encrypt-key"
    const expected = createHmac("sha256", secret).update("1700000000abc-123{\"a\":1}").digest("hex")
    expect(verifyFeishuSignature(secret, "1700000000", "abc-123", '{"a":1}', expected)).toBe(true)
    expect(verifyFeishuSignature(secret, "1700000000", "abc-123", '{"a":2}', expected)).toBe(false)
  })
})
