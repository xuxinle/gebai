/**
 * 文件索引接口（`GET /api/v1/fs/files`）——「快速打开」（Ctrl+P）的名单来源。
 *
 * 为什么值得单独测：这条接口的产物直接决定用户「搜不到文件」还是「搜到一堆不该出现的文件」。
 * 三件事必须锁住：① 重目录（node_modules/.git/dist 等）不进名单；② `.gitignore` 生效（rg 路径下
 * 由 rg 保证、内置回退靠 WALK_SKIP_DIRS + 隐藏名过滤）；③ `limit` 截断时**如实标注 truncated**，
 * 而不是默默少列（前端据此提示「索引已截断」）。
 */
import { afterAll, describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createApp, SERVICE_USER, type AppDeps } from "../app"
import { listFilesInRoot } from "../core/fs/service"
import type { ServerConfig } from "../core/base/config"

function makeDeps(home: string, fsHidden = false): AppDeps {
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
/** 造一棵小仓库：源码 + 一个被 .gitignore 忽略的目录 + 重目录 + 隐藏文件。 */
function makeTree(): string {
  const root = mkdtempSync(join(tmpdir(), "fs-files-"))
  dirs.push(root)
  mkdirSync(join(root, "src", "files"), { recursive: true })
  mkdirSync(join(root, "node_modules", "pkg"), { recursive: true })
  mkdirSync(join(root, ".git", "objects"), { recursive: true })
  mkdirSync(join(root, "build"), { recursive: true })
  writeFileSync(join(root, "src", "main.ts"), "export {}\n")
  writeFileSync(join(root, "src", "files", "quick-open.ts"), "export {}\n")
  writeFileSync(join(root, "README.md"), "# x\n")
  writeFileSync(join(root, ".env"), "A=1\n")
  writeFileSync(join(root, "node_modules", "pkg", "index.js"), "module.exports = {}\n")
  writeFileSync(join(root, ".git", "objects", "abc"), "bin\n")
  writeFileSync(join(root, "build", "out.js"), "// built\n")
  writeFileSync(join(root, ".gitignore"), "build/\n")
  return root
}

afterAll(() => {
  for (const d of dirs) rmSync(d, { recursive: true, force: true })
})

describe("listFilesInRoot（文件索引）", () => {
  test("列出源码文件，重目录与 .gitignore 忽略的目录不进名单", async () => {
    const root = makeTree()
    const res = await listFilesInRoot(root)
    const has = (p: string): boolean => res.files.includes(p)
    expect(has("src/main.ts")).toBe(true)
    expect(has("src/files/quick-open.ts")).toBe(true)
    expect(has("README.md")).toBe(true)
    expect(has("node_modules/pkg/index.js")).toBe(false)
    expect(has(".git/objects/abc")).toBe(false)
    expect(has("build/out.js")).toBe(false)
    // 引擎如实回报（有 rg 就是 ripgrep，没有就 builtin——两条路都必须给出同一份可用名单）
    expect(["ripgrep", "builtin"]).toContain(res.engine)
    expect(res.truncated).toBe(false)
  })

  test("隐藏文件默认不列，显式要求时列出", async () => {
    const root = makeTree()
    const plain = await listFilesInRoot(root)
    expect(plain.files).not.toContain(".env")
    const hidden = await listFilesInRoot(root, { showHidden: true })
    expect(hidden.files).toContain(".env")
  })

  test("limit 截断时如实标注（不默默少列）", async () => {
    const root = makeTree()
    const res = await listFilesInRoot(root, { limit: 1 })
    expect(res.files).toHaveLength(1)
    expect(res.truncated).toBe(true)
  })

  test("不存在的根返回空名单而不是抛错（前端面板显示「无匹配」即可）", async () => {
    const res = await listFilesInRoot(join(tmpdir(), "fs-files-not-exist-xyz"))
    expect(res.files).toEqual([])
  })
})

describe("GET /api/v1/fs/files", () => {
  test("返回该根的文件名单与引擎；隐藏文件按服务端配置", async () => {
    const root = makeTree()
    const rootId = encodeURIComponent(`abs:${root}`)
    const app = createApp(makeDeps(root, false))
    const res = await app.request(`/api/v1/fs/files?root=${rootId}`)
    expect(res.status).toBe(200)
    const body = (await res.json()) as { files: string[]; engine: string; truncated: boolean }
    expect(body.files).toContain("src/main.ts")
    expect(body.files).not.toContain(".env")
    expect(body.truncated).toBe(false)

    // 显式 showHidden=1 优先于服务端配置（与 fs/list 同口径）
    const shown = (await (await app.request(`/api/v1/fs/files?root=${rootId}&showHidden=1`)).json()) as { files: string[] }
    expect(shown.files).toContain(".env")

    // limit 透传
    const limited = (await (await app.request(`/api/v1/fs/files?root=${rootId}&limit=1`)).json()) as { files: string[]; truncated: boolean }
    expect(limited.files).toHaveLength(1)
    expect(limited.truncated).toBe(true)
  })

  test("未启用文件工作台时返回 404（与其他 fs 路由同口径，不泄露能力存在性）", async () => {
    const root = makeTree()
    const app = createApp(makeDeps(root)) // 默认启用：先确认路径存在
    const ok = await app.request(`/api/v1/fs/files?root=${encodeURIComponent(`abs:${root}`)}`)
    expect(ok.status).toBe(200)

    const deps = makeDeps(root)
    ;(deps as unknown as { config: { fsEnabled: boolean } }).config.fsEnabled = false
    const off = await createApp(deps).request(`/api/v1/fs/files?root=${encodeURIComponent(`abs:${root}`)}`)
    expect(off.status).toBe(404)
    expect(((await off.json()) as { error: string }).error).toContain("未启用")
  })
})
