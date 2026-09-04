import { describe, expect, test } from "bun:test"
import { existsSync, writeFileSync } from "node:fs"
import type { BridgeLike } from "./bridge"
import { browserProxyEnabled, proxyFetch, installBrowserFetchProxy, BROWSER_PROXY_ENV } from "./fetch-proxy"
import { runInToolFetchScope, runWithoutFetchProxy } from "../support/fetch-scope"

/** fake bridge：记录调用；http_fetch 按 outPath 写响应体文件并返回落盘形态结果。 */
function recordingBridge(body = "hello") {
  const calls: Array<{ op: string; args: Record<string, unknown> }> = []
  const bridge: BridgeLike = {
    request: async (op, args) => {
      calls.push({ op, args })
      if (op !== "http_fetch") throw new Error(`unexpected op: ${op}`)
      const outPath = String(args.outPath ?? "")
      writeFileSync(outPath, body)
      return { status: 200, headers: { "content-type": "text/plain", "x-echo": "1" }, path: outPath, size: body.length }
    },
  }
  return { bridge, calls }
}

/** fake 直连 fetch（未被代理时应被调用）。 */
function directFetch() {
  const seen: Array<{ input: unknown; init?: RequestInit }> = []
  const fetcher = (async (input: unknown, init?: RequestInit) => {
    seen.push({ input, init })
    return new Response("direct", { status: 200 })
  }) as typeof fetch
  return { fetcher, seen }
}

describe("browserProxyEnabled", () => {
  test("truthy values on, everything else off", () => {
    for (const v of ["1", "true", "TRUE", "on", "Yes", " yes "]) {
      expect(browserProxyEnabled({ [BROWSER_PROXY_ENV]: v })).toBe(true)
    }
    for (const v of ["", "0", "false", "off", "no", "garbage", undefined]) {
      expect(browserProxyEnabled({ [BROWSER_PROXY_ENV]: v })).toBe(false)
    }
    expect(browserProxyEnabled({})).toBe(false)
  })
})

describe("proxyFetch", () => {
  test("routes in-scope http fetch through bridge http_fetch and builds Response", async () => {
    const { bridge, calls } = recordingBridge("proxied-body")
    const { fetcher, seen } = directFetch()
    const res = await runInToolFetchScope("sess-1", () =>
      proxyFetch(bridge, fetcher, "https://intra.example.com/api/list", {
        method: "POST",
        headers: new Headers({ "content-type": "application/json", "x-k": "v" }),
        body: '{"page":1}',
      }))
    expect(seen).toHaveLength(0)
    expect(calls).toHaveLength(1)
    expect(calls[0].op).toBe("http_fetch")
    expect(calls[0].args).toMatchObject({
      sessionId: "sess-1",
      url: "https://intra.example.com/api/list",
      method: "POST",
      headers: { "content-type": "application/json", "x-k": "v" },
      body: '{"page":1}',
    })
    expect(calls[0].args.followRedirects).toBeUndefined()
    expect(typeof calls[0].args.outPath).toBe("string")
    expect(res.status).toBe(200)
    expect(res.headers.get("x-echo")).toBe("1")
    expect(await res.text()).toBe("proxied-body")
    // 中转临时文件已清理
    expect(existsSync(String(calls[0].args.outPath))).toBe(false)
  })

  test("outside tool scope falls back to direct fetch", async () => {
    const { bridge, calls } = recordingBridge()
    const { fetcher, seen } = directFetch()
    const res = await proxyFetch(bridge, fetcher, "https://api.example.com/x")
    expect(calls).toHaveLength(0)
    expect(seen).toHaveLength(1)
    expect(await res.text()).toBe("direct")
  })

  test("runWithoutFetchProxy exempts in-scope calls (LLM 豁免语义)", async () => {
    const { bridge, calls } = recordingBridge()
    const { fetcher, seen } = directFetch()
    await runInToolFetchScope("s", () => runWithoutFetchProxy(() => proxyFetch(bridge, fetcher, "https://llm.example.com/v1/chat", { method: "POST", body: "{}" })))
    expect(calls).toHaveLength(0)
    expect(seen).toHaveLength(1)
  })

  test("non-string body / Request input / non-http(s) fall back to direct", async () => {
    const { bridge, calls } = recordingBridge()
    const { fetcher, seen } = directFetch()
    await runInToolFetchScope("s", () => proxyFetch(bridge, fetcher, "https://a.example.com/u", { method: "POST", body: new FormData() }))
    await runInToolFetchScope("s", () => proxyFetch(bridge, fetcher, new Request("https://a.example.com/r")))
    await runInToolFetchScope("s", () => proxyFetch(bridge, fetcher, "data:text/plain,hi"))
    expect(calls).toHaveLength(0)
    expect(seen).toHaveLength(3)
  })

  test("redirect manual maps to followRedirects=false; default GET omits body", async () => {
    const { bridge, calls } = recordingBridge()
    const { fetcher } = directFetch()
    await runInToolFetchScope("s", () => proxyFetch(bridge, fetcher, "https://a.example.com/hop", { redirect: "manual" }))
    expect(calls[0].args.followRedirects).toBe(false)
    expect(calls[0].args.body).toBeUndefined()
    expect(calls[0].args.method).toBe("GET")
    expect(fetcher).toBeDefined()
  })

  test("pre-aborted signal rejects with AbortError and cleans temp file", async () => {
    const { bridge, calls } = recordingBridge()
    const { fetcher } = directFetch()
    const outPath = () => String(calls[0]?.args.outPath ?? "")
    await expect(
      runInToolFetchScope("s", () => proxyFetch(bridge, fetcher, "https://a.example.com/slow", { signal: AbortSignal.abort() })),
    ).rejects.toMatchObject({ name: "AbortError" })
    expect(calls).toHaveLength(1)
    expect(existsSync(outPath())).toBe(false)
  })

  test("204/304 responses construct null-body Response", async () => {
    const calls: Array<Record<string, unknown>> = []
    const bridge: BridgeLike = {
      request: async (_op, args) => {
        calls.push(args)
        writeFileSync(String(args.outPath), "")
        return { status: 304, headers: { location: "https://a.example.com/next" }, path: String(args.outPath), size: 0 }
      },
    }
    const { fetcher } = directFetch()
    const res = await runInToolFetchScope("s", () => proxyFetch(bridge, fetcher, "https://a.example.com/maybe"))
    expect(res.status).toBe(304)
    expect(res.headers.get("location")).toBe("https://a.example.com/next")
    expect(res.body).toBeNull()
    expect(calls).toHaveLength(1)
  })
})

describe("installBrowserFetchProxy", () => {
  test("disabled env: no-op; forced: patches global fetch with in-scope routing", async () => {
    const original = globalThis.fetch
    try {
      // 环境未开启：安装为 no-op（global fetch 引用不变）
      expect(browserProxyEnabled({})).toBe(false)
      installBrowserFetchProxy({ env: {} })
      expect(globalThis.fetch).toBe(original)

      // force 安装：global fetch 被替换，且作用域内经桥接代理
      const { bridge, calls } = recordingBridge("via-global")
      installBrowserFetchProxy({ bridge, force: true })
      expect(globalThis.fetch).not.toBe(original)
      const res = await runInToolFetchScope("s-g", () => fetch("https://intra.example.com/api"))
      expect(await res.text()).toBe("via-global")
      expect(calls[0].args).toMatchObject({ sessionId: "s-g", url: "https://intra.example.com/api" })
      // 作用域外回退直连（原 fetch）的行为已由 proxyFetch 注入测试覆盖，这里不发起真实网络请求
    } finally {
      globalThis.fetch = original
    }
  })
})
