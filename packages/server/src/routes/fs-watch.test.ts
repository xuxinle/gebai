/**
 * 变更监听端点测试（`GET /api/v1/fs/watch`）：闸门、基线/长轮询语义、根目录的线上记号、git 元数据开关。
 *
 * 为什么值得单独测：这条路是「页面自己会变」的唯一通道，它的契约细节（`.` 代表根、`rev` 缺省只取基线、
 * `wait` 上限、`GEBAI_FS_WATCH=false` 的退化信号）错了都不会报错——只会表现成「有时不刷新」，很难查。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createApp, SERVICE_USER, type AppDeps } from "../app"
import type { ServerConfig } from "../core/base/config"

function makeDeps(opts: { home?: string; fsWatch?: boolean; sandboxed?: boolean; git?: unknown } = {}): AppDeps {
  const config = {
    auth: "local",
    gebaiHome: opts.home ?? join(tmpdir(), "gebai-watch-home"),
    fsEnabled: true,
    fsWrite: true,
    fsWatch: opts.fsWatch !== false,
    fsHidden: false,
  } as unknown as ServerConfig
  return {
    config,
    auth: { defaultUser: () => SERVICE_USER },
    sandbox: { enforcedFor: () => opts.sandboxed === true, isExempt: () => false },
    engine: { workbenchProjects: () => [] },
    store: { getEnv: async () => ({}) },
    git: opts.git,
  } as unknown as AppDeps
}

interface WatchBody {
  enabled: boolean
  changed: boolean
  rev: number
  paths: string[] | null
  git: boolean
}

const dirs: string[] = []
function tmpRepo(prefix = "watch-route-"): string {
  const d = mkdtempSync(join(tmpdir(), prefix))
  dirs.push(d)
  return d
}
const app = (deps: AppDeps) => createApp(deps)
const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms))

describe("/api/v1/fs/watch", () => {
  test("闸门：fs 关闭时 404；GEBAI_FS_WATCH=false 时返回 enabled:false（前端据此退化轮询）", async () => {
    const off = app(makeDeps({ fsWatch: false }))
    const res1 = await off.request("/api/v1/fs/watch?root=abs:/tmp&dirs=.", { method: "GET" })
    expect(res1.status).toBe(200)
    expect(((await res1.json()) as WatchBody).enabled).toBe(false)

    const deps = makeDeps()
    ;(deps.config as unknown as { fsEnabled: boolean }).fsEnabled = false
    const res2 = await app(deps).request("/api/v1/fs/watch?root=abs:/tmp&dirs=.", { method: "GET" })
    expect(res2.status).toBe(404)
  })

  test("rev 缺省 = 只取基线（立即返回，changed:false）", async () => {
    const root = tmpRepo()
    const a = app(makeDeps())
    const res = await a.request(`/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.&git=0`, { method: "GET" })
    const body = (await res.json()) as WatchBody
    expect(res.status).toBe(200)
    expect(body.enabled).toBe(true)
    expect(body.changed).toBe(false)
    expect(typeof body.rev).toBe("number")
  })

  test("`.` 代表根本身：根下新增文件会唤醒长轮询并回报根内相对路径", async () => {
    const root = tmpRepo()
    const a = app(makeDeps())
    const base = (await (await a.request(`/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.&git=0`, { method: "GET" })).json()) as WatchBody
    const waiting = a.request(
      `/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.&git=0&rev=${base.rev}&wait=10`,
      { method: "GET" },
    )
    await sleep(150)
    writeFileSync(join(root, "hello.txt"), "x")
    const body = (await (await waiting).json()) as WatchBody
    expect(body.changed).toBe(true)
    expect(body.rev).toBeGreaterThan(base.rev)
    expect(body.paths).toContain("hello.txt")
  })

  test("子目录监视：嵌套路径回报为 `sub/deep.txt`（根内相对）", async () => {
    const root = tmpRepo()
    mkdirSync(join(root, "sub"), { recursive: true })
    const a = app(makeDeps())
    const base = (await (await a.request(`/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.,sub&git=0`, { method: "GET" })).json()) as WatchBody
    const waiting = a.request(
      `/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.,sub&git=0&rev=${base.rev}&wait=10`,
      { method: "GET" },
    )
    await sleep(150)
    writeFileSync(join(root, "sub", "deep.txt"), "x")
    const body = (await (await waiting).json()) as WatchBody
    expect(body.changed).toBe(true)
    expect(body.paths).toContain("sub/deep.txt")
  })

  test("越界与不存在的目录项被静默丢弃（不因一项出错整轮失败）", async () => {
    const root = tmpRepo()
    const a = app(makeDeps())
    const res = await a.request(
      `/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.,,../etc,nope/deep&git=0&rev=0&wait=0`,
      { method: "GET" },
    )
    expect(res.status).toBe(200)
    expect(((await res.json()) as WatchBody).enabled).toBe(true)
  })

  test("git=1 时监听 git 元数据：仓库根由 GitService 给出，改动带 git 标记", async () => {
    const root = tmpRepo()
    mkdirSync(join(root, ".git", "refs", "heads"), { recursive: true })
    const a = app(makeDeps({ git: { repoRoot: async () => root } }))
    const base = (await (await a.request(`/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.`, { method: "GET" })).json()) as WatchBody
    const waiting = a.request(
      `/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.&rev=${base.rev}&wait=10`,
      { method: "GET" },
    )
    await sleep(150)
    // 仓库外部的 git 动作（切分支/提交）只改 .git：工作区目录的非递归 watch 看不到，靠单独挂的 .git 兜住
    writeFileSync(join(root, ".git", "index"), "x")
    const body = (await (await waiting).json()) as WatchBody
    expect(body.changed).toBe(true)
    expect(body.git).toBe(true)
    expect(body.paths).toContain(".git/index")
  })

  test("git=0（或没有 git 服务）时照常工作，工作区改动不带 git 标记", async () => {
    const root = tmpRepo()
    mkdirSync(join(root, ".git"), { recursive: true })
    const a = app(makeDeps())
    const base = (await (await a.request(`/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.&git=0`, { method: "GET" })).json()) as WatchBody
    const waiting = a.request(
      `/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.&git=0&rev=${base.rev}&wait=10`,
      { method: "GET" },
    )
    await sleep(150)
    writeFileSync(join(root, "plain.txt"), "x")
    const body = (await (await waiting).json()) as WatchBody
    expect(body.changed).toBe(true)
    expect(body.git).toBe(false)
    expect(body.paths).toContain("plain.txt")
  })

  test("wait 超时（无变化）也返回 200 与空变化标记", async () => {
    const root = tmpRepo()
    const a = app(makeDeps())
    const base = (await (await a.request(`/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.&git=0`, { method: "GET" })).json()) as WatchBody
    const t0 = Date.now()
    const res = await a.request(
      `/api/v1/fs/watch?root=${encodeURIComponent(`abs:${root}`)}&dirs=.&git=0&rev=${base.rev}&wait=1`,
      { method: "GET" },
    )
    const body = (await res.json()) as WatchBody
    expect(res.status).toBe(200)
    expect(body.changed).toBe(false)
    expect(body.rev).toBe(base.rev)
    expect(Date.now() - t0).toBeGreaterThanOrEqual(900)
  })
})

// 收尾：清掉本文件建的临时目录
process.on("exit", () => {
  for (const d of dirs) rmSync(d, { recursive: true, force: true })
})
