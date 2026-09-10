import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import {
  deleteMiniTool,
  getMiniTool,
  listMiniTools,
  miniToolPath,
  saveMiniTool,
  validateToolName,
  MINI_TOOL_MAX_HTML,
} from "./mini-tools"

function cleanup(home: string): void {
  rmSync(home, { recursive: true, force: true })
}

describe("validateToolName", () => {
  test("accepts valid names", () => {
    expect(validateToolName("unit_converter")).toBe("")
    expect(validateToolName("qr生成器")).toBe("")
    expect(validateToolName("a")).toBe("")
  })
  test("rejects invalid names", () => {
    expect(validateToolName("")).not.toBe("")
    expect(validateToolName("a.b")).not.toBe("")
    expect(validateToolName("a-b")).not.toBe("")
    expect(validateToolName("a/b")).not.toBe("")
    expect(validateToolName("a b")).not.toBe("")
    expect(validateToolName("a".repeat(41))).not.toBe("")
  })
})

describe("saveMiniTool", () => {
  test("saves public tool under GEBAI_HOME/tools with name-hash sharding", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    const info = await saveMiniTool(home, "default", { name: "clock", html: "<p>hi</p>", scope: "public" })
    expect(info.name).toBe("clock")
    expect(info.scope).toBe("public")
    expect(info.owner).toBe("default")
    expect(info.html).toBe("<p>hi</p>")
    const p = miniToolPath(home, "default", "clock", "public")
    expect(p).toContain(join("tools"))
    expect(existsSync(p)).toBe(true)
    cleanup(home)
  })

  test("saves private tool under users/{user}/tools", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    await saveMiniTool(home, "u1", { name: "calc", html: "<p>1</p>", scope: "private" })
    const p = miniToolPath(home, "u1", "calc", "private")
    expect(p).toContain(join("users", "u1", "tools"))
    expect(existsSync(p)).toBe(true)
    // 其他用户目录下不存在
    expect(existsSync(miniToolPath(home, "u2", "calc", "private"))).toBe(false)
    cleanup(home)
  })

  test("overwrites same-name tool and refreshes updatedAt", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    const a = await saveMiniTool(home, "default", { name: "x", html: "v1", scope: "public" })
    const b = await saveMiniTool(home, "default", { name: "x", html: "v2", scope: "public" })
    expect(b.html).toBe("v2")
    expect(b.createdAt).toBe(a.createdAt)
    expect(b.updatedAt).toBeGreaterThanOrEqual(a.updatedAt)
    cleanup(home)
  })

  test("rejects invalid name and oversized html", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    await expect(saveMiniTool(home, "default", { name: "bad name", html: "x", scope: "public" })).rejects.toThrow("工具名仅限")
    await expect(saveMiniTool(home, "default", { name: "ok", html: "x".repeat(MINI_TOOL_MAX_HTML + 1), scope: "public" })).rejects.toThrow("HTML 内容超限")
    cleanup(home)
  })
})

describe("listMiniTools", () => {
  test("lists public tools for all users, private tools only for owner", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    await saveMiniTool(home, "u1", { name: "pub1", html: "a", scope: "public" })
    await saveMiniTool(home, "u2", { name: "pub2", html: "b", scope: "public" })
    await saveMiniTool(home, "u1", { name: "secret", html: "s", scope: "private" })
    await saveMiniTool(home, "u2", { name: "secret", html: "s2", scope: "private" })

    const forU1 = await listMiniTools(home, "u1")
    const names1 = forU1.map((t) => t.name)
    expect(names1).toContain("pub1")
    expect(names1).toContain("pub2")
    expect(names1).toContain("secret")
    // 元数据不含 html
    expect("html" in forU1[0]).toBe(false)

    const forU2 = await listMiniTools(home, "u2")
    const names2 = forU2.map((t) => t.name)
    expect(names2).toContain("pub1")
    expect(names2).toContain("pub2")
    expect(names2).toContain("secret")
    cleanup(home)
  })

  test("private tool shadows public tool with the same name in the list", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    await saveMiniTool(home, "u1", { name: "dup", html: "public", scope: "public" })
    await saveMiniTool(home, "u1", { name: "dup", html: "private", scope: "private" })
    const list = await listMiniTools(home, "u1")
    expect(list.filter((t) => t.name === "dup").length).toBe(1)
    expect(list.find((t) => t.name === "dup")!.scope).toBe("private")
    cleanup(home)
  })

  test("empty home returns empty list", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    expect(await listMiniTools(home, "default")).toEqual([])
    cleanup(home)
  })

  test("ignores malformed json and foreign files", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    await saveMiniTool(home, "u1", { name: "good", html: "g", scope: "public" })
    // 手工放置损坏文件与无关文件（任意分片目录下）
    const bad = join(home, "tools", "zz", "zz")
    mkdirSync(bad, { recursive: true })
    writeFileSync(join(bad, "broken.json"), "{not json")
    const list = await listMiniTools(home, "u1")
    expect(list.map((t) => t.name)).toEqual(["good"])
    cleanup(home)
  })
})

describe("getMiniTool", () => {
  test("returns private tool first, then public", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    await saveMiniTool(home, "u1", { name: "dup", html: "public", scope: "public" })
    const pub = await getMiniTool(home, "u1", "dup")
    expect(pub!.html).toBe("public")
    await saveMiniTool(home, "u1", { name: "dup", html: "private", scope: "private" })
    const priv = await getMiniTool(home, "u1", "dup")
    expect(priv!.html).toBe("private")
    expect(priv!.scope).toBe("private")
    cleanup(home)
  })

  test("user cannot read another user's private tool", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    await saveMiniTool(home, "u1", { name: "secret", html: "s", scope: "private" })
    expect(await getMiniTool(home, "u2", "secret")).toBeNull()
    cleanup(home)
  })

  test("returns null for missing tool", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    expect(await getMiniTool(home, "u1", "nope")).toBeNull()
    cleanup(home)
  })
})

describe("deleteMiniTool", () => {
  test("deletes private and public tools", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    await saveMiniTool(home, "u1", { name: "a", html: "x", scope: "private" })
    await saveMiniTool(home, "u1", { name: "b", html: "y", scope: "public" })
    expect(await deleteMiniTool(home, "u1", "a", "private")).toBe(true)
    expect(existsSync(miniToolPath(home, "u1", "a", "private"))).toBe(false)
    expect(await deleteMiniTool(home, "u1", "b", "public")).toBe(true)
    expect(existsSync(miniToolPath(home, "u1", "b", "public"))).toBe(false)
    // 重复删除返回 false（文件已不存在）
    expect(await deleteMiniTool(home, "u1", "a", "private")).toBe(false)
    cleanup(home)
  })

  test("private delete never touches another user's tool", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-"))
    await saveMiniTool(home, "u1", { name: "secret", html: "s", scope: "private" })
    await deleteMiniTool(home, "u2", "secret", "private")
    expect(existsSync(miniToolPath(home, "u1", "secret", "private"))).toBe(true)
    cleanup(home)
  })
})

describe("public tool permissions (multi-user shared resource)", () => {
  test("multi-user: public create/overwrite denied for non-admin, allowed for admin", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-auth-"))
    // 普通用户创建公共工具被拒（共享资源防投毒）；私有不受限
    await expect(
      saveMiniTool(home, "u1", { name: "pub", html: "x", scope: "public" }, { mode: "server", role: "user" }),
    ).rejects.toThrow("仅管理员")
    await saveMiniTool(home, "u1", { name: "priv", html: "x", scope: "private" }, { mode: "server", role: "user" })
    // 管理员可创建/覆盖公共工具
    await saveMiniTool(home, "admin", { name: "pub", html: "v1", scope: "public" }, { mode: "server", role: "admin" })
    // 普通用户不能覆盖他人已存在的公共工具
    await expect(
      saveMiniTool(home, "u2", { name: "pub", html: "evil", scope: "public" }, { mode: "server", role: "user" }),
    ).rejects.toThrow("仅管理员")
    expect(getMiniTool(home, "u2", "pub")).resolves.toMatchObject({ html: "v1" })
    // 单用户模式（auth=none）不限制
    await saveMiniTool(home, "default", { name: "pub2", html: "y", scope: "public" }, { mode: "local", role: "admin" })
    cleanup(home)
  })

  test("multi-user: public delete denied for non-admin, allowed for admin", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-minitools-del-"))
    await saveMiniTool(home, "admin", { name: "shared", html: "s", scope: "public" }, { mode: "server", role: "admin" })
    // 普通用户删除公共工具被拒（返回 false，不删除）
    expect(await deleteMiniTool(home, "u1", "shared", "public", { mode: "server", role: "user" })).toBe(false)
    expect(existsSync(miniToolPath(home, "admin", "shared", "public"))).toBe(true)
    // 私有删除不受限
    await saveMiniTool(home, "u1", { name: "mine", html: "m", scope: "private" }, { mode: "server", role: "user" })
    expect(await deleteMiniTool(home, "u1", "mine", "private", { mode: "server", role: "user" })).toBe(true)
    // 管理员可删
    expect(await deleteMiniTool(home, "u1", "shared", "public", { mode: "server", role: "admin" })).toBe(true)
    cleanup(home)
  })
})
