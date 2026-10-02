import { describe, expect, test } from "bun:test"
import { parseExternalCredential } from "./external-auth"
import { clearTokenCookie, syncTokenCookie } from "./auth"

function storage(entries: Record<string, string> = {}) {
  return { getItem: (k: string) => entries[k] ?? null }
}

describe("parseExternalCredential", () => {
  test("URL params take priority", () => {
    const params = new URLSearchParams("?gb_ext_username=alice&gb_ext_credential=123.sig")
    const got = parseExternalCredential(params, storage({ "myapp.auth": '{"username":"bob","credential":"zzz"}' }), "myapp.auth")
    expect(got).toEqual({ username: "alice", credential: "123.sig" })
  })

  test("URL params trim whitespace; missing either side ignored", () => {
    expect(parseExternalCredential(new URLSearchParams("?gb_ext_username=%20alice%20&gb_ext_credential=%20sig%20"), storage(), null)).toEqual({ username: "alice", credential: "sig" })
    expect(parseExternalCredential(new URLSearchParams("?gb_ext_username=alice"), storage(), null)).toBeNull()
  })

  test("localStorage JSON object form", () => {
    const got = parseExternalCredential(new URLSearchParams(), storage({ app: '{"username":"alice","credential":"tok-1"}' }), "app")
    expect(got).toEqual({ username: "alice", credential: "tok-1" })
  })

  test("localStorage string form username:credential", () => {
    const got = parseExternalCredential(new URLSearchParams(), storage({ app: "alice:tok-1" }), "app")
    expect(got).toEqual({ username: "alice", credential: "tok-1" })
  })

  test("no storageKey and no params -> null; storage read failure -> null", () => {
    expect(parseExternalCredential(new URLSearchParams(), storage(), null)).toBeNull()
    const throwing = { getItem: () => { throw new Error("denied") } }
    expect(parseExternalCredential(new URLSearchParams(), throwing, "app")).toBeNull()
  })

  test("malformed storage values -> null", () => {
    expect(parseExternalCredential(new URLSearchParams(), storage({ app: '{"username":42}' }), "app")).toBeNull()
    expect(parseExternalCredential(new URLSearchParams(), storage({ app: ":only-cred" }), "app")).toBeNull()
    expect(parseExternalCredential(new URLSearchParams(), storage({ app: "only-name:" }), "app")).toBeNull()
    expect(parseExternalCredential(new URLSearchParams(), storage({ app: "" }), "app")).toBeNull()
  })
})

/**
 * 登录态 cookie 同步（`syncTokenCookie`/`clearTokenCookie`）：服务模式下图片/视频/下载是浏览器
 * **原生请求**，前端无法给它们插请求头——令牌必须同时落在同源 cookie 上（服务端 credential-sources
 * 的 cookie 来源据此鉴权，且只对 GET/HEAD 生效）。
 *
 * 另一条同样要锁住的语义：**未登录时不得写空值**。cookie 按 host 共享、不隔离端口，同一个 IP 上
 * 并排多套歌白时，B 实例的页面加载不得清掉 A 实例的 cookie（清 cookie 只发生在显式登出）。
 */
describe("登录态 cookie 同步", () => {
  /** document 桩（bunfig preload 的最小 DOM）——cookie 赋值直接落到该属性上。 */
  const doc = document as unknown as { cookie: string }
  const reset = (): void => {
    doc.cookie = "gebai.auth.token=; max-age=0"
  }

  test("写入带令牌的 cookie：URL 编码 + 有效期 + path + SameSite", () => {
    reset()
    syncTokenCookie("tok.a/b+c")
    expect(doc.cookie).toContain("gebai.auth.token=tok.a%2Fb%2Bc")
    expect(doc.cookie).toContain("max-age=604800") // 7 天，与服务端令牌 TTL 一致
    expect(doc.cookie).toMatch(/path=\/?/)
    expect(doc.cookie).toContain("SameSite=Lax")
  })

  test("空令牌不写 cookie（未登录页面不得清掉同 host 其他实例的登录态）", () => {
    doc.cookie = "gebai.auth.token=live-token"
    const before = doc.cookie
    syncTokenCookie("")
    expect(doc.cookie).toBe(before) // 原样保留，未被覆盖为空
  })

  test("登出显式清 cookie（max-age=0）", () => {
    doc.cookie = "gebai.auth.token=live-token"
    clearTokenCookie()
    expect(doc.cookie).toContain("max-age=0")
    expect(doc.cookie).not.toContain("live-token")
  })

  test("部署方自定义非 Bearer 载体时不写 cookie（载体选择交给部署方）", () => {
    const g = globalThis as Record<string, unknown>
    const prev = g.__GEBAI_AUTH__
    g.__GEBAI_AUTH__ = { requestHeaders: (t: string | null) => (t ? { "X-Gebai-Token": t } : {}) }
    try {
      doc.cookie = "gebai.auth.token=untouched"
      syncTokenCookie("tok-x")
      expect(doc.cookie).toContain("untouched") // 未被 cookie 载体逻辑改写
    } finally {
      if (prev === undefined) delete g.__GEBAI_AUTH__
      else g.__GEBAI_AUTH__ = prev
    }
  })
})
