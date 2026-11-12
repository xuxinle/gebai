/**
 * 长期授权探测：读应用权限清单看有没有 `offline_access`。
 *
 * 这层为什么值得单测：它是“点去授权不会掉进 20027 死页”的唯一防线，
 * 而它的三种结局（支持 / 不支持 / 读不到）在真实环境里各自对应一种完全不同的
 * 用户可见后果——把 `unknown` 误判成 `supported`，用户就直接看到死页。
 */
import { describe, expect, test } from "bun:test"
import { probeOfflineAccess } from "./offline-access"

const APP_ID = "cli_aa51bb6b25789cbc"

/** 造一个假的飞书：tenant token 正常，应用信息接口按 `scopes` 返回。 */
function fakeFeishu(opts: {
  scopes?: string[]
  appInfoCode?: number
  appInfoMsg?: string
  tenantFail?: boolean
}): { fetchImpl: typeof fetch; calls: string[] } {
  const calls: string[] = []
  const fetchImpl = (async (input: string | URL | Request) => {
    const url = String(input)
    calls.push(url)
    if (url.includes("tenant_access_token")) {
      if (opts.tenantFail) {
        return new Response(JSON.stringify({ code: 10003, msg: "invalid param" }), {
          status: 200,
          headers: { "content-type": "application/json" },
        })
      }
      return new Response(JSON.stringify({ code: 0, tenant_access_token: "t-abc", expire: 7200 }), {
        status: 200,
        headers: { "content-type": "application/json" },
      })
    }
    if (url.includes("/application/v6/applications/")) {
      if (opts.appInfoCode !== undefined && opts.appInfoCode !== 0) {
        return new Response(JSON.stringify({ code: opts.appInfoCode, msg: opts.appInfoMsg ?? "denied" }), {
          status: 200,
          headers: { "content-type": "application/json" },
        })
      }
      const scopes = (opts.scopes ?? []).map((s) => ({ scope: s, token_types: ["user"] }))
      return new Response(JSON.stringify({ code: 0, data: { app: { app_id: APP_ID, scopes } } }), {
        status: 200,
        headers: { "content-type": "application/json" },
      })
    }
    return new Response("{}", { status: 404 })
  }) as unknown as typeof fetch
  return { fetchImpl, calls }
}

describe("probeOfflineAccess：三态判定", () => {
  test("清单里有 offline_access → supported", async () => {
    const { fetchImpl, calls } = fakeFeishu({ scopes: ["im:message", "offline_access"] })
    const r = await probeOfflineAccess({ appId: APP_ID, appSecret: "s", fetchImpl })
    console.log(`    实测: ${r.support} scopeCount=${r.scopeCount} 调用=${calls.length} 次`)
    expect(r.support).toBe("supported")
    expect(r.scopeCount).toBe(2)
    // 先换应用身份令牌，再读应用信息：两步
    expect(calls.length).toBe(2)
    expect(calls[0]).toContain("tenant_access_token")
    expect(calls[1]).toContain(`/applications/${APP_ID}`)
  })

  test("清单读到了但**没有** offline_access → unsupported（极典型的真实故障）", async () => {
    // 实测形态：许工二号（cli_aa51bb…）437 项权限里没有 offline_access，
    // allow_refresh_token 却是 true——只凭后者会误判为「能长期」，故只认清单。
    const { fetchImpl } = fakeFeishu({ scopes: ["im:message", "im:chat:readonly"] })
    const r = await probeOfflineAccess({ appId: APP_ID, appSecret: "s", fetchImpl })
    console.log(`    实测: ${r.support}，原因=${r.reason?.slice(0, 60)}`)
    expect(r.support).toBe("unsupported")
    // 原因要能指导人去后台开：只说「不支持」等于没说
    expect(r.reason).toContain("持续访问已授权的数据")
    // 带上条数才能让“清单是不是真的读到了”可自证
    expect(r.reason).toContain("2 项")
  })

  test("读不到清单（应用没开读取应用信息权限）→ unknown，且原因点明缺哪个权限", async () => {
    const { fetchImpl } = fakeFeishu({
      appInfoCode: 99991672,
      appInfoMsg: "Access denied. One of the following scopes is required: [admin:app.info:readonly, application:application:self_manage]",
    })
    const r = await probeOfflineAccess({ appId: APP_ID, appSecret: "s", fetchImpl })
    console.log(`    实测: ${r.support}，原因=${r.reason?.slice(0, 80)}`)
    expect(r.support).toBe("unknown")
    expect(r.reason).toContain("99991672")
    expect(r.reason).toContain("application:application:self_manage")
  })

  test("应用凭据错（取不到 tenant token）→ unknown（不抛错，授权流程还得继续）", async () => {
    const { fetchImpl } = fakeFeishu({ tenantFail: true })
    const r = await probeOfflineAccess({ appId: APP_ID, appSecret: "bad", fetchImpl })
    console.log(`    实测: ${r.support}，原因=${r.reason?.slice(0, 60)}`)
    expect(r.support).toBe("unknown")
    expect(r.reason).toContain("10003")
  })

  test("网络直接抛错 → unknown（不把异常往上扔：探测失败不该打断授权）", async () => {
    const fetchImpl = (async () => {
      throw new Error("unable to verify the first certificate")
    }) as unknown as typeof fetch
    const r = await probeOfflineAccess({ appId: APP_ID, appSecret: "s", fetchImpl })
    console.log(`    实测: ${r.support}，原因=${r.reason?.slice(0, 60)}`)
    expect(r.support).toBe("unknown")
    expect(r.reason).toContain("unable to verify the first certificate")
  })

  test("应用信息响应缺 data.app（如 404 形态）→ unknown，不当成“没开通”", async () => {
    const fetchImpl = (async (input: string | URL | Request) => {
      const url = String(input)
      if (url.includes("tenant_access_token")) {
        return new Response(JSON.stringify({ code: 0, tenant_access_token: "t-abc" }), {
          status: 200,
          headers: { "content-type": "application/json" },
        })
      }
      return new Response(JSON.stringify({ code: 0 }), { status: 200, headers: { "content-type": "application/json" } })
    }) as unknown as typeof fetch
    const r = await probeOfflineAccess({ appId: APP_ID, appSecret: "s", fetchImpl })
    console.log(`    实测: ${r.support}，原因=${r.reason?.slice(0, 60)}`)
    expect(r.support).toBe("unknown")
  })
})
