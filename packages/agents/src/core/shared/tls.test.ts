import { describe, expect, test, afterEach } from "bun:test"
import { feishuFetch, feishuTlsInsecure, feishuWsOptions } from "./tls"

const KEY = "GEBAI_FEISHU_INSECURE_TLS"

afterEach(() => {
  delete process.env[KEY]
})

describe("feishu-bot/tls", () => {
  test("feishuTlsInsecure：默认关闭，true/1 开启，其余关闭", () => {
    delete process.env[KEY]
    expect(feishuTlsInsecure()).toBe(false)
    process.env[KEY] = "true"
    expect(feishuTlsInsecure()).toBe(true)
    process.env[KEY] = "1"
    expect(feishuTlsInsecure()).toBe(true)
    process.env[KEY] = "0"
    expect(feishuTlsInsecure()).toBe(false)
    process.env[KEY] = "false"
    expect(feishuTlsInsecure()).toBe(false)
  })

  test("feishuFetch：关闭时原样转发（不附加 tls 选项）", async () => {
    const orig = globalThis.fetch
    let capturedInit: unknown = "unset"
    globalThis.fetch = (async (_input: unknown, init?: unknown) => {
      capturedInit = init
      return new Response("ok")
    }) as typeof fetch
    try {
      await feishuFetch("https://open.feishu.cn/x")
      expect(capturedInit).toBeUndefined()
    } finally {
      globalThis.fetch = orig
    }
  })

  test("feishuFetch：开启时附加 tls.rejectUnauthorized=false（保留原 init）", async () => {
    process.env[KEY] = "true"
    const orig = globalThis.fetch
    let capturedInit: unknown
    globalThis.fetch = (async (_input: unknown, init?: unknown) => {
      capturedInit = init
      return new Response("ok")
    }) as typeof fetch
    try {
      await feishuFetch("https://open.feishu.cn/x", { method: "POST" })
      expect(capturedInit).toEqual({ method: "POST", tls: { rejectUnauthorized: false } })
    } finally {
      globalThis.fetch = orig
    }
  })

  test("feishuWsOptions：跟随开关", () => {
    delete process.env[KEY]
    expect(feishuWsOptions()).toBeUndefined()
    process.env[KEY] = "true"
    expect(feishuWsOptions()).toEqual({ tls: { rejectUnauthorized: false } })
  })
})
