import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { AuthService } from "./auth"
import { EnvManager } from "./core/env"
import { SessionStore } from "./core/store"

describe("AuthService", () => {
  test("create + login + authorize roundtrip", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth-"))
    const auth = new AuthService(home, "server")
    await auth.createUser("alice", "s3cret", "admin")
    const token = await auth.login("alice", "s3cret")
    expect(token).toBeTruthy()
    const user = await auth.authorize(token!)
    expect(user!.username).toBe("alice")
    expect(user!.role).toBe("admin")
    cleanup(home)
  })

  test("verifyCredentials: 凭据直验通过/失败/禁用（不签发令牌）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth-vc-"))
    const auth = new AuthService(home, "server")
    const u = await auth.createUser("carol", "pw1")
    const ok = await auth.verifyCredentials("carol", "pw1")
    expect(ok?.username).toBe("carol")
    expect(await auth.verifyCredentials("carol", "wrong")).toBeNull()
    await auth.updateUser(u.id, { disabled: true })
    expect(await auth.verifyCredentials("carol", "pw1")).toBeNull()
    cleanup(home)
  })

  test("verifyCredentials 与 login 共享登录限流", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth-vc2-"))
    const auth = new AuthService(home, "server")
    await auth.createUser("dave", "right")
    // 连续失败 5 次（verifyCredentials）后，login 也被锁定；反之亦然
    for (let i = 0; i < 5; i++) {
      expect(await auth.verifyCredentials("dave", "wrong")).toBeNull()
    }
    expect(await auth.login("dave", "right")).toBeNull() // 锁定期内
    cleanup(home)
  })

  test("不存在用户名同样计入失败锁定（恒时路径，防用户名枚举）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth-vc3-"))
    const auth = new AuthService(home, "server")
    // 连续 5 次对不存在用户名的失败 → 锁定期内即使「注册后」也无法登录（同一实例内）
    for (let i = 0; i < 5; i++) {
      expect(await auth.verifyCredentials("ghost", "x")).toBeNull()
    }
    await auth.createUser("ghost", "realpw")
    expect(await auth.login("ghost", "realpw")).toBeNull() // 锁定期内
    cleanup(home)
  })

  test("wrong password rejected", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth2-"))
    const auth = new AuthService(home, "server")
    await auth.createUser("bob", "pw1")
    expect(await auth.login("bob", "wrong")).toBeNull()
    cleanup(home)
  })

  test("disabled user cannot login", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth3-"))
    const auth = new AuthService(home, "server")
    const u = await auth.createUser("carol", "pw1")
    await auth.updateUser(u.id, { disabled: true })
    expect(await auth.login("carol", "pw1")).toBeNull()
    cleanup(home)
  })

  test("repeated login failures lock the account briefly (brute-force guard)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth-lock-"))
    const auth = new AuthService(home, "server")
    await auth.createUser("dave", "right")
    // 连续失败 AUTH_MAX_FAILS 次（5）后，正确密码也被拒绝（锁定期内）
    for (let i = 0; i < 5; i++) {
      expect(await auth.login("dave", "wrong")).toBeNull()
    }
    expect(await auth.login("dave", "right")).toBeNull() // 锁定期内
    // 锁定期（60s）不可等待；验证其他用户名不受影响
    await auth.createUser("erin", "pw")
    expect(await auth.login("erin", "pw")).toBeTruthy()
    cleanup(home)
  })

  test("register: 注册用户恒为普通角色，注册即登录；重名/非法拒绝", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth-reg-"))
    const auth = new AuthService(home, "server")
    const { user, token } = await auth.register("newbie", "pw123")
    expect(user.role).toBe("user") // 不可注册 admin
    expect(token).toBeTruthy()
    expect((await auth.authorize(token!))!.username).toBe("newbie")
    // 重名拒绝
    expect(auth.register("newbie", "x")).rejects.toThrow(/exists/)
    // 非法用户名（路径分隔符/超长/空）拒绝
    expect(auth.register("a/b", "x")).rejects.toThrow(/用户名非法/)
    expect(auth.register("", "x")).rejects.toThrow(/用户名非法/)
    expect(auth.register("ok_name", "")).rejects.toThrow(/密码不能为空/)
    // 注册用户仍可正常登录（非禁用）
    expect(await auth.login("newbie", "pw123")).toBeTruthy()
    cleanup(home)
  })

  test("register approval 模式：待审批（disabled+pending、不签发令牌、不可登录）；批准后可登录", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth-reg-appr-"))
    const auth = new AuthService(home, "server")
    const { user, token, pending } = await auth.register("applicant", "pw1", "approval")
    expect(pending).toBe(true)
    expect(token).toBeNull()
    expect(user.disabled).toBe(true)
    expect(user.pending).toBe(true)
    expect(await auth.login("applicant", "pw1")).toBeNull() // 待审不可登录
    // admin 批准：启用 + 清除待审
    await auth.updateUser(user.id, { disabled: false, pending: false })
    expect(await auth.login("applicant", "pw1")).toBeTruthy()
    cleanup(home)
  })

  test("default user for local mode is admin", () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth5-"))
    const auth = new AuthService(home, "local")
    expect(auth.defaultUser().username).toBe("admin")
    expect(auth.defaultUser().role).toBe("admin")
    expect(auth.defaultUser().id).toBe("admin")
    cleanup(home)
  })

  test("applyAdminHash enables admin with given hash", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth6-"))
    const auth = new AuthService(home, "server")
    // 用 hashPassword 同款算法预计算 salt:hash
    const { scryptSync, randomBytes } = await import("node:crypto")
    const salt = randomBytes(16).toString("hex")
    const hash = scryptSync("admin-secret", salt, 64).toString("hex")
    const cred = `${salt}:${hash}`
    await auth.applyAdminHash(cred)
    expect(await auth.login("admin", "admin-secret")).toBeTruthy()
    const user = await auth.authorize((await auth.login("admin", "admin-secret"))!)
    expect(user!.role).toBe("admin")
    expect(user!.disabled).toBe(false)
    cleanup(home)
  })

  test("applyAdminHash with bad format throws", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth7-"))
    const auth = new AuthService(home, "server")
    expect(auth.applyAdminHash("not-a-hash")).rejects.toThrow(/GEBAI_ADMIN_PASSWORD_HASH/)
    expect(auth.applyAdminHash("abcd:1234")).rejects.toThrow(/GEBAI_ADMIN_PASSWORD_HASH/)
    cleanup(home)
  })

  test("applyAdminHash without hash disables existing admin", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth8-"))
    const auth = new AuthService(home, "server")
    const { scryptSync, randomBytes } = await import("node:crypto")
    const salt = randomBytes(16).toString("hex")
    const cred = `${salt}:${scryptSync("pw", salt, 64).toString("hex")}`
    await auth.applyAdminHash(cred)
    expect(await auth.login("admin", "pw")).toBeTruthy()
    // 未设置哈希（undefined）：admin 被禁用，无法登录
    await auth.applyAdminHash(undefined)
    expect(await auth.login("admin", "pw")).toBeNull()
    const users = await auth.listUsers()
    const admin = users.find((u) => u.username === "admin")
    expect(admin!.disabled).toBe(true)
    cleanup(home)
  })

  test("applyAdminHash re-enables and overwrites hash", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-auth9-"))
    const auth = new AuthService(home, "server")
    const { scryptSync, randomBytes } = await import("node:crypto")
    const salt = randomBytes(16).toString("hex")
    const oldCred = `${salt}:${scryptSync("old", salt, 64).toString("hex")}`
    await auth.applyAdminHash(oldCred)
    await auth.applyAdminHash(undefined)
    const salt2 = randomBytes(16).toString("hex")
    const newCred = `${salt2}:${scryptSync("new", salt2, 64).toString("hex")}`
    await auth.applyAdminHash(newCred)
    expect(await auth.login("admin", "new")).toBeTruthy()
    expect(await auth.login("admin", "old")).toBeNull()
    cleanup(home)
  })
})

describe("EnvManager describe", () => {
  test("describes env with source and masking", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-env2-"))
    const store = new SessionStore({ home })
    const env = new EnvManager(home, store)
    const session = await store.createSession("default")
    writeFileSync(join(home, "users", "default", "env.json"), JSON.stringify({ MY_KEY: "usersecret" }))
    process.env.GLOBAL_VAR = "g"
    const listed = await env.describe(session.id, "default")
    expect(listed.some((e) => e.name === "GLOBAL_VAR" && e.source === "global")).toBe(true)
    const masked = listed.find((e) => e.name === "MY_KEY")
    expect(masked!.source).toBe("user")
    expect(masked!.sensitive).toBe(true)
    expect(masked!.value).not.toContain("usersecret")
    delete process.env.GLOBAL_VAR
    cleanup(home)
  })
})

function cleanup(home: string) {
  rmSync(home, { recursive: true, force: true })
}
