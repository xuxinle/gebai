/**
 * 隐藏文件的默认可见性（`GET /api/v1/fs/list`）：请求没带 `showHidden` 时取服务端配置
 * （`GEBAI_FS_HIDDEN`，默认列出），**显式给了以参数为准**。
 *
 * 为什么值得单独测：`参数 || 配置` 的旧写法会把显式的 `false` 吞掉，且不报错——只表现成
 * 「菜单里点了关不掉隐藏文件、勾选态还和实际相反」，配置为 true 时尤其难查。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createApp, SERVICE_USER, type AppDeps } from "../app"
import type { ServerConfig } from "../core/base/config"

function makeDeps(fsHidden: boolean, home: string): AppDeps {
  const config = {
    auth: "local",
    gebaiHome: home,
    fsEnabled: true,
    fsWrite: true,
    fsHidden,
  } as unknown as ServerConfig
  return {
    config,
    auth: { defaultUser: () => SERVICE_USER },
    sandbox: { enforcedFor: () => false, isExempt: () => true },
    engine: { workbenchProjects: () => [] },
    store: { getEnv: async () => ({}) },
  } as unknown as AppDeps
}

const dirs: string[] = []
function tmpDir(): string {
  const d = mkdtempSync(join(tmpdir(), "fs-hidden-"))
  writeFileSync(join(d, ".env"), "A=1")
  writeFileSync(join(d, "a.txt"), "x")
  dirs.push(d)
  return d
}

/** 列举该根的条目名（`query` 传额外的查询参数，如 `&showHidden=0`）。 */
async function names(dir: string, fsHidden: boolean, query = ""): Promise<string[]> {
  const rootId = encodeURIComponent(`abs:${dir}`)
  const res = await createApp(makeDeps(fsHidden, dir)).request(`/api/v1/fs/list?root=${rootId}${query}`)
  expect(res.status).toBe(200)
  const body = (await res.json()) as { entries: Array<{ name: string }> }
  return body.entries.map((e) => e.name)
}

describe("/api/v1/fs/list 的隐藏文件默认值", () => {
  test("配置默认列出（true）：不传 showHidden 时隐藏文件可见", async () => {
    const dir = tmpDir()
    expect(await names(dir, true)).toContain(".env")
  })

  test("显式 showHidden=0 覆盖配置：true 时也能关掉（回归：旧写法让前端关不掉）", async () => {
    const dir = tmpDir()
    const list = await names(dir, true, "&showHidden=0")
    expect(list).toContain("a.txt")
    expect(list).not.toContain(".env")
  })

  test("配置为 false：默认不列出，显式 showHidden=1 可打开", async () => {
    const dir = tmpDir()
    expect(await names(dir, false)).not.toContain(".env")
    expect(await names(dir, false, "&showHidden=1")).toContain(".env")
  })
})

process.on("exit", () => {
  for (const d of dirs) rmSync(d, { recursive: true, force: true })
})
