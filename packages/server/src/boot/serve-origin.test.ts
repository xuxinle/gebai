import { describe, expect, test } from "bun:test"
import { wsOriginAllowed } from "./serve"

describe("WS 跨源升级判定（豁免面与 REST 的 CORS 中间件对齐）", () => {
  const local = { auth: "local" }
  const server = { auth: "server" }

  test("无 Origin：非浏览器客户端放行（原生/CLI/服务端调用不受同源策略约束）", () => {
    expect(wsOriginAllowed({ origin: null, host: "127.0.0.1:3000", ...local }).ok).toBe(true)
  })

  test("本地模式 + 缺省 *：要求 Origin 与 Host 同源", () => {
    expect(wsOriginAllowed({ origin: "http://127.0.0.1:3000", host: "127.0.0.1:3000", ...local }).ok).toBe(true)
    const bad = wsOriginAllowed({ origin: "http://evil.example.com", host: "127.0.0.1:3000", ...local })
    expect(bad.ok).toBe(false)
    expect(bad.reason).toBe("cross-origin")
  })

  test("畸形 Origin：拒绝并标记 invalid-origin", () => {
    const bad = wsOriginAllowed({ origin: "not a url", host: "127.0.0.1:3000", ...local })
    expect(bad.ok).toBe(false)
    expect(bad.reason).toBe("invalid-origin")
  })

  test("显式配 GEBAI_CORS_ORIGINS（不含 *）＝有意开放白名单：跨源放行（与 REST 同口径）", () => {
    const r = wsOriginAllowed({ origin: "http://app.example.com", host: "127.0.0.1:3000", ...local, corsOrigins: ["http://app.example.com"] })
    expect(r.ok).toBe(true)
  })

  test("服务模式：有令牌鉴权，跨源放行（异域前端可连 WS）", () => {
    const r = wsOriginAllowed({ origin: "http://app.example.com", host: "10.0.0.5:3000", ...server })
    expect(r.ok).toBe(true)
  })

  test("白名单含 * 时仍按同源拦截（* 不构成有意开放）", () => {
    const bad = wsOriginAllowed({ origin: "http://evil.example.com", host: "127.0.0.1:3000", ...local, corsOrigins: ["*"] })
    expect(bad.ok).toBe(false)
  })

  test("与 REST 侧规则的一致性：同一组输入在两侧应给出同一放行结论", () => {
    // REST（app.ts）：auth !== "server" && corsOrigins.includes("*") && origin 时才校验同源
    const restBlocks = (o: { origin: string; host: string; auth: string; corsOrigins: string[] }) =>
      o.auth !== "server" && o.corsOrigins.includes("*") && !!o.origin && new URL(o.origin).host !== o.host
    const cases = [
      { origin: "http://evil.example.com", host: "127.0.0.1:3000", auth: "local", corsOrigins: ["*"] },
      { origin: "http://127.0.0.1:3000", host: "127.0.0.1:3000", auth: "local", corsOrigins: ["*"] },
      { origin: "http://app.example.com", host: "10.0.0.5:3000", auth: "server", corsOrigins: ["*"] },
      { origin: "http://app.example.com", host: "10.0.0.5:3000", auth: "local", corsOrigins: ["http://app.example.com"] },
    ]
    for (const c of cases) {
      const ws = wsOriginAllowed({ origin: c.origin, host: c.host, auth: c.auth, corsOrigins: c.corsOrigins }).ok
      expect(ws).toBe(!restBlocks(c))
    }
  })
})
