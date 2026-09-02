import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "../../core/base/types"
import { getMiniTool } from "../../core/widgets/mini-tools"
import { def } from "./widgets"

function ctx(home: string): ToolContext {
  return {
    user: "default",
    sessionId: "s1",
    workdir: home,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(home, p),
  } as ToolContext
}

function cleanup(home: string) {
  rmSync(home, { recursive: true, force: true })
}

describe("widgets sub-agent", () => {
  test("def 暴露增删改查四工具（save/list/get/delete），preload=false", () => {
    const names = Object.keys(def.tools!)
    expect(names.sort()).toEqual(["delete", "get", "list", "save"])
    expect(def.name).toBe("widgets")
    expect(def.preload).toBe(false)
  })

  test("delete 需审批，四工具均限实时前端（interaction=realtime）", () => {
    expect(def.tools!.delete.requiresApproval).toBe(true)
    for (const t of Object.values(def.tools!)) {
      expect(t.interaction).toBe("realtime")
    }
  })

  test("save 默认私有、scope=public 公用，同名覆盖", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-widgets-save-"))
    const c = ctx(home)
    const priv = await def.tools!.save.execute({ name: "calc", html: "<p>1</p>" }, c)
    expect(priv.output).toContain("calc")
    expect(priv.output).toContain("用户私有")
    const pub = await def.tools!.save.execute({ name: "clock", html: "<p>2</p>", scope: "public" }, c)
    expect(pub.output).toContain("公用")
    expect(await getMiniTool(home, "default", "calc")).toMatchObject({ name: "calc", scope: "private" })
    expect(await getMiniTool(home, "default", "clock")).toMatchObject({ name: "clock", scope: "public" })
    // 同名覆盖
    await def.tools!.save.execute({ name: "calc", html: "<p>v2</p>" }, c)
    expect((await getMiniTool(home, "default", "calc"))!.html).toBe("<p>v2</p>")
    cleanup(home)
  })

  test("list 列出清单（含范围），get 读取源码（私有优先），delete 删除", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-widgets-crud-"))
    const c = ctx(home)
    await def.tools!.save.execute({ name: "temp", html: "<p>x</p>", scope: "public" }, c)
    await def.tools!.save.execute({ name: "temp", html: "<p>私有版</p>" }, c)
    const list = await def.tools!.list.execute({}, c)
    expect(list.output).toContain("共 1 个小工具")
    expect(list.output).toContain("temp")
    // get 解析顺序：用户私有 → 公用
    const got = await def.tools!.get.execute({ name: "temp" }, c)
    expect(got.output).toContain("私有版")
    expect(got.output).toContain("用户私有")
    const missing = await def.tools!.get.execute({ name: "nope" }, c)
    expect(missing.output).toContain("不存在")
    const del = await def.tools!.delete.execute({ name: "temp", scope: "public" }, c)
    expect(del.output).toContain("已删除")
    expect(await getMiniTool(home, "default", "temp")).toMatchObject({ scope: "private" })
    const delMissing = await def.tools!.delete.execute({ name: "gone", scope: "private" }, c)
    expect(delMissing.output).toContain("不存在")
    cleanup(home)
  })
})
