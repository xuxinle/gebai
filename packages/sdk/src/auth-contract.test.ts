/**
 * 凭证契约（`window.__GEBAI_AUTH__` 覆盖点 + 默认实现）。
 *
 * 为什么单独测：这一层决定「令牌存哪、请求怎么带」——是部署方在框架之外改变鉴权行为的
 * 唯一入口。三件事必须锁住：① 未覆盖时行为与既有实现完全一致（localStorage + Bearer）；
 * ② 覆盖点逐项生效、且只实现一部分也能工作（其余回落默认）；③ 覆盖点抛错按「未提供」处理，
 * 部署方脚本的 bug 不得让整个界面失效。
 */
import { afterEach, beforeEach, describe, expect, test } from "bun:test"
import { AUTH_TOKEN_KEY, clearTokenState, readToken, requestHeaders, writeToken } from "./auth-contract"

const g = globalThis as Record<string, unknown>
let store: Map<string, string>
let prevLocalStorage: unknown
let prevOverride: unknown

beforeEach(() => {
  store = new Map()
  prevLocalStorage = g.localStorage
  prevOverride = g.__GEBAI_AUTH__
  g.localStorage = {
    getItem: (k: string) => store.get(k) ?? null,
    setItem: (k: string, v: string) => void store.set(k, v),
    removeItem: (k: string) => void store.delete(k),
  }
  delete g.__GEBAI_AUTH__
})

afterEach(() => {
  g.localStorage = prevLocalStorage
  if (prevOverride === undefined) delete g.__GEBAI_AUTH__
  else g.__GEBAI_AUTH__ = prevOverride
})

describe("默认实现（无覆盖点）", () => {
  test("读写清令牌走 localStorage 的 gebai.auth.token 键", () => {
    expect(readToken()).toBeNull()
    writeToken("tok-1")
    expect(store.get(AUTH_TOKEN_KEY)).toBe("tok-1")
    expect(readToken()).toBe("tok-1")
    clearTokenState()
    expect(readToken()).toBeNull()
  })

  test("请求头为 Authorization: Bearer；无令牌时不附加任何头", () => {
    expect(requestHeaders("tok-1")).toEqual({ Authorization: "Bearer tok-1" })
    expect(requestHeaders(null)).toEqual({})
  })

  test("存储不可用（隐私模式）时读返回 null 而非抛错", () => {
    g.localStorage = {
      getItem: () => {
        throw new Error("denied")
      },
      setItem: () => {
        throw new Error("denied")
      },
      removeItem: () => {
        throw new Error("denied")
      },
    }
    expect(readToken()).toBeNull()
    expect(() => writeToken("x")).not.toThrow()
    expect(() => clearTokenState()).not.toThrow()
  })
})

describe("覆盖点（window.__GEBAI_AUTH__）", () => {
  test("整体替换：令牌改由自定义载体承载，默认 localStorage 不再被使用", () => {
    const custom = new Map<string, string>()
    g.__GEBAI_AUTH__ = {
      readToken: () => custom.get("t") ?? null,
      writeToken: (t: string) => void custom.set("t", t),
      clearToken: () => void custom.delete("t"),
      requestHeaders: (t: string | null) => (t ? { "X-Gebai-Token": t } : {}),
    }
    writeToken("tok-2")
    expect(custom.get("t")).toBe("tok-2")
    expect(store.has(AUTH_TOKEN_KEY)).toBe(false) // 默认载体未被写入（部署方完全接管）
    expect(readToken()).toBe("tok-2")
    expect(requestHeaders("tok-2")).toEqual({ "X-Gebai-Token": "tok-2" })
    clearTokenState()
    expect(readToken()).toBeNull()
  })

  test("逐项回落：只实现 readToken，其余动作仍走默认实现", () => {
    g.__GEBAI_AUTH__ = { readToken: () => "from-custom" }
    expect(readToken()).toBe("from-custom")
    writeToken("tok-3") // 未覆盖 → 默认写 localStorage
    expect(store.get(AUTH_TOKEN_KEY)).toBe("tok-3")
    expect(requestHeaders("tok-3")).toEqual({ Authorization: "Bearer tok-3" })
  })

  test("覆盖点抛错按未提供处理：不影响读令牌与发请求", () => {
    g.__GEBAI_AUTH__ = {
      readToken: () => {
        throw new Error("boom")
      },
      requestHeaders: () => {
        throw new Error("boom")
      },
    }
    store.set(AUTH_TOKEN_KEY, "fallback-tok")
    expect(readToken()).toBe("fallback-tok")
    expect(requestHeaders("fallback-tok")).toEqual({ Authorization: "Bearer fallback-tok" })
  })

  test("非对象覆盖点被忽略（脚本写错类型不使界面失效）", () => {
    g.__GEBAI_AUTH__ = "oops"
    store.set(AUTH_TOKEN_KEY, "tok-4")
    expect(readToken()).toBe("tok-4")
    expect(requestHeaders("tok-4")).toEqual({ Authorization: "Bearer tok-4" })
  })

  test("requestHeaders 可挂非令牌头（无令牌时仍被调用）", () => {
    g.__GEBAI_AUTH__ = { requestHeaders: (t: string | null) => ({ "X-Site": "aidp", ...(t ? { Authorization: `Bearer ${t}` } : {}) }) }
    expect(requestHeaders(null)).toEqual({ "X-Site": "aidp" })
    expect(requestHeaders("t")).toEqual({ "X-Site": "aidp", Authorization: "Bearer t" })
  })
})
