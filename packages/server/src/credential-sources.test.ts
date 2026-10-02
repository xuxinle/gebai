/**
 * 凭证来源扩展点（`credential-sources.ts` + `custom-auth.ts`）。
 *
 * 为什么单独测：这一层决定「服务端从哪里认出用户」——是部署方改变鉴权行为的入口。
 * 必须锁住四件事：
 * ① 内置三种载体（Bearer / Basic / cookie）行为正确，且 cookie 只对安全方法生效（防 CSRF）；
 * ② 取值与校验分离——来源只取凭证，身份一律经 authorize/verifyCredentials/userByName，
 *    伪造/过期/禁用一律 401（扩展点不放宽任何校验）；
 * ③ 数据级配置解析：非法项启动报错（不静默降级）、来源顺序即尝试顺序；
 * ④ 代码级 `custom/auth/` 加载：单个文件报错只记 errors 不影响其余（失败隔离）。
 */
import { afterAll, describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import {
  bearerSource,
  basicSource,
  cookieTokenSource,
  credentialContext,
  defaultCredentialSources,
  headerTokenSource,
  isSafeMethod,
  parseCookieHeader,
  parseCredentialSources,
  resolveCredential,
  type CredentialContext,
  type CredentialSource,
} from "./credential-sources"
import { AuthService } from "./auth"
import { loadCustomCredentialSources } from "./custom-auth"

const home = mkdtempSync(join(tmpdir(), "gebai-credsrc-"))
const auth = new AuthService(home, "server")
let token = ""
let alice: { username: string; id: string }

/** 请求上下文构造：headers 用小写键（与 Hono c.req.header 同语义）。 */
function ctx(method: string, headers: Record<string, string> = {}): CredentialContext {
  const lower: Record<string, string> = {}
  for (const [k, v] of Object.entries(headers)) lower[k.toLowerCase()] = v
  return credentialContext({ method, header: (n) => lower[n.toLowerCase()], auth })
}

async function setup() {
  const r = await auth.register("alice", "pw123")
  token = r.token!
  alice = { username: r.user.username, id: r.user.id }
}
await setup()

afterAll(() => rmSync(home, { recursive: true, force: true }))

describe("内置载体", () => {
  test("Bearer：有效令牌通过，伪造拒绝", async () => {
    const s = bearerSource()
    expect(await s.resolve(ctx("GET", { authorization: `Bearer ${token}` }))).not.toBeNull()
    expect(await s.resolve(ctx("GET", { authorization: "Bearer forged" }))).toBeNull()
    expect(await s.resolve(ctx("GET", {}))).toBeNull() // 无头 = 本来源不适用
  })

  test("Basic：账号密码校验生效（含错误密码拒绝）", async () => {
    const s = basicSource()
    const ok = Buffer.from("alice:pw123").toString("base64")
    const bad = Buffer.from("alice:wrong").toString("base64")
    expect(await s.resolve(ctx("GET", { authorization: `Basic ${ok}` }))).not.toBeNull()
    expect(await s.resolve(ctx("GET", { authorization: `Basic ${bad}` }))).toBeNull()
    expect(await s.resolve(ctx("GET", { authorization: "Basic !!!notbase64" }))).toBeNull()
  })

  test("cookie：解析出令牌并校验；多 cookie 混排/URL 编码均正确", async () => {
    const s = cookieTokenSource("gebai.auth.token")
    expect(await s.resolve(ctx("GET", { cookie: `theme=dark; gebai.auth.token=${token}` }))).not.toBeNull()
    expect(await s.resolve(ctx("GET", { cookie: `gebai.auth.token=${encodeURIComponent(token)}` }))).not.toBeNull()
    expect(await s.resolve(ctx("GET", { cookie: "gebai.auth.token=forged" }))).toBeNull()
  })

  test("cookie 默认仅安全方法：写方法跳过该来源（防 CSRF）", async () => {
    const sources = defaultCredentialSources()
    const cookie = { cookie: `gebai.auth.token=${token}` }
    expect(await resolveCredential(sources, ctx("GET", cookie))).not.toBeNull()
    expect(await resolveCredential(sources, ctx("HEAD", cookie))).not.toBeNull()
    expect(await resolveCredential(sources, ctx("POST", cookie))).toBeNull()
    expect(await resolveCredential(sources, ctx("DELETE", cookie))).toBeNull()
    // Bearer 不受方法限制（写端点照常）
    expect(await resolveCredential(sources, ctx("POST", { authorization: `Bearer ${token}` }))).not.toBeNull()
  })

  test("声明 safeMethodsOnly=false 的来源可覆盖默认（部署方显式选择）", async () => {
    const all = cookieTokenSource("gebai.auth.token", false)
    expect(await resolveCredential([all], ctx("POST", { cookie: `gebai.auth.token=${token}` }))).not.toBeNull()
  })

  test("header 载体：自定义头取令牌并校验", async () => {
    const s = headerTokenSource("X-Gebai-Token")
    expect(await s.resolve(ctx("GET", { "x-gebai-token": token }))).not.toBeNull()
    expect(await s.resolve(ctx("GET", { "x-gebai-token": "forged" }))).toBeNull()
  })

  test("isSafeMethod / parseCookieHeader 边界", () => {
    expect(isSafeMethod("get")).toBe(true)
    expect(isSafeMethod("HEAD")).toBe(true)
    expect(isSafeMethod("post")).toBe(false)
    expect(parseCookieHeader(undefined, "a")).toBeUndefined()
    expect(parseCookieHeader("a=1", "b")).toBeUndefined()
    expect(parseCookieHeader(" a = 1 ; b = 2 ", "b")).toBe("2")
    expect(parseCookieHeader("a=%E4%B8%AD", "a")).toBe("中")
    expect(parseCookieHeader("a=%ZZ", "a")).toBe("%ZZ") // 非法转义保原值
  })
})

describe("来源链语义", () => {
  test("顺序即优先级：前一个命中即返回，不再尝试后续", async () => {
    const order: string[] = []
    const mk = (name: string, hit: boolean): CredentialSource => ({
      name,
      resolve: () => {
        order.push(name)
        return hit ? { id: name, username: name, role: "user", disabled: false, createdAt: 0, salt: "", hash: "" } : null
      },
    })
    const user = await resolveCredential([mk("first", false), mk("second", true), mk("third", true)], ctx("GET"))
    expect(user?.id).toBe("second")
    expect(order).toEqual(["first", "second"]) // third 未被尝试
  })

  test("来源抛错按未命中处理并回调 onError（单个扩展点故障不使鉴权瘫痪）", async () => {
    const errs: string[] = []
    const broken: CredentialSource = {
      name: "broken",
      resolve: () => {
        throw new Error("boom")
      },
    }
    const user = await resolveCredential([broken, bearerSource()], ctx("GET", { authorization: `Bearer ${token}` }), (n) => errs.push(n))
    expect(user).not.toBeNull() // 后续来源照常生效
    expect(errs).toEqual(["broken"])
  })

  test("全部未命中 → null（调用方据此 401）", async () => {
    expect(await resolveCredential(defaultCredentialSources(), ctx("GET", { cookie: "gebai.auth.token=bad" }))).toBeNull()
  })

  test("用户名型来源经 userByName：只认已启用用户；禁用后失效", async () => {
    const byHeader: CredentialSource = { name: "gateway", safeMethodsOnly: false, resolve: (c) => c.userByName(c.header("x-user") ?? "") }
    expect(await resolveCredential([byHeader], ctx("GET", { "x-user": "alice" }))).not.toBeNull()
    expect(await resolveCredential([byHeader], ctx("GET", { "x-user": "ghost" }))).toBeNull() // 不存在
    expect(await resolveCredential([byHeader], ctx("GET", { "x-user": "Alice" }))).not.toBeNull() // 规范化命中
    await auth.updateUser(alice.id, { disabled: true })
    expect(await resolveCredential([byHeader], ctx("GET", { "x-user": "alice" }))).toBeNull() // 禁用后失效
    await auth.updateUser(alice.id, { disabled: false })
  })

  test("userByName 不返回待审批用户（不绕过 pending）", async () => {
    await auth.register("penduser", "pw1", "approval")
    expect(await auth.userByName("penduser")).toBeNull()
  })
})

describe("数据级配置解析（GEBAI_CREDENTIAL_SOURCES）", () => {
  test("解析各类名称", () => {
    const s = parseCredentialSources("bearer, basic ,cookie:gebai.auth.token,header:X-Tok")
    expect(s.map((x) => x.name)).toEqual(["bearer", "basic", "cookie:gebai.auth.token", "header:X-Tok"])
  })

  test("空/未设置 → 空链（调用方决定缺省）", () => {
    expect(parseCredentialSources(undefined)).toEqual([])
    expect(parseCredentialSources("  ,  ")).toEqual([])
  })

  test("非法项抛错而非静默忽略（配置错误在启动期暴露）", () => {
    expect(() => parseCredentialSources("bearer,weird")).toThrow(/无法识别/)
    expect(() => parseCredentialSources("cookie:")).toThrow(/缺名称/)
    expect(() => parseCredentialSources("header:")).toThrow(/缺名称/)
  })

  test("配置的 cookie 来源同样只对安全方法生效", async () => {
    const s = parseCredentialSources("cookie:gebai.auth.token")
    expect(await resolveCredential(s, ctx("GET", { cookie: `gebai.auth.token=${token}` }))).not.toBeNull()
    expect(await resolveCredential(s, ctx("POST", { cookie: `gebai.auth.token=${token}` }))).toBeNull()
  })
})

describe("代码级扩展域（custom/auth/）", () => {
  const dirs: string[] = []
  const mkDir = (): string => {
    const d = mkdtempSync(join(tmpdir(), "gebai-customauth-"))
    dirs.push(d)
    return d
  }
  afterAll(() => {
    for (const d of dirs) rmSync(d, { recursive: true, force: true })
  })

  test("目录不存在 → 空结果（未二开是常态，不报错）", async () => {
    const r = await loadCustomCredentialSources(join(tmpdir(), "gebai-nonexistent-auth-dir"))
    expect(r.sources).toEqual([])
    expect(r.errors).toEqual([])
  })

  test("默认导出单个来源即注册；数组导出注册多个", async () => {
    const dir = mkDir()
    writeFileSync(
      join(dir, "a_gateway.ts"),
      `export default { name: "gateway", safeMethodsOnly: false, resolve: (c) => c.userByName(c.header("x-user") ?? "") }\n`,
    )
    writeFileSync(
      join(dir, "b_pair.js"),
      `const s1 = { name: "p1", resolve: () => null }\nconst s2 = { name: "p2", resolve: () => null }\nexport default [s1, s2]\n`,
    )
    const r = await loadCustomCredentialSources(dir)
    expect(r.errors).toEqual([])
    expect(r.sources.map((s) => s.name)).toEqual(["gateway", "p1", "p2"]) // 文件名排序
  })

  test("失败隔离：坏文件只记 errors，其余来源照常加载", async () => {
    const dir = mkDir()
    writeFileSync(join(dir, "a_ok.ts"), `export default { name: "ok", resolve: () => null }\n`)
    writeFileSync(join(dir, "b_throw.ts"), `throw new Error("bad module")\n`)
    writeFileSync(join(dir, "c_badshape.ts"), `export default { nope: true }\n`)
    const r = await loadCustomCredentialSources(dir)
    expect(r.sources.map((s) => s.name)).toEqual(["ok"])
    expect(r.errors.length).toBe(2)
    expect(r.errors.some((e) => e.includes("b_throw.ts"))).toBe(true)
    expect(r.errors.some((e) => e.includes("c_badshape.ts") && e.includes("默认导出"))).toBe(true)
  })

  test("忽略测试文件与下划线前缀文件（不参与加载）", async () => {
    const dir = mkDir()
    writeFileSync(join(dir, "_helper.ts"), `export default { name: "helper", resolve: () => null }\n`)
    writeFileSync(join(dir, "real.test.ts"), `export default { name: "test-only", resolve: () => null }\n`)
    writeFileSync(join(dir, "real.ts"), `export default { name: "real", resolve: () => null }\n`)
    const r = await loadCustomCredentialSources(dir)
    expect(r.sources.map((s) => s.name)).toEqual(["real"])
  })

  test("自定义来源可参与真实鉴权（与内置链同一语义）", async () => {
    const dir = mkDir()
    writeFileSync(
      join(dir, "gw.ts"),
      `export default { name: "gw", safeMethodsOnly: false, resolve: (c) => c.userByName(c.header("x-user") ?? "") }\n`,
    )
    const { sources } = await loadCustomCredentialSources(dir)
    expect(await resolveCredential(sources, ctx("POST", { "x-user": "alice" }))).not.toBeNull()
    expect(await resolveCredential(sources, ctx("POST", { "x-user": "nobody" }))).toBeNull()
  })
})
