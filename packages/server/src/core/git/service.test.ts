/**
 * Git 服务测试（文件工作台·Git 图形化）——重点是「**任意两个提交之间、提交与工作树之间**的文件对比」
 * 这一一等公民能力：端点（WORKTREE / INDEX / 任意 rev）语义、重命名识别、共同祖先（mergeBase）、
 * 两侧内容取数（contentAt）与单文件差异。
 *
 * 用真实 git 仓库（临时目录 + 真实 git 命令）验证，避免 mock 掩盖命令语义差异。
 */
import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, writeFileSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { GitService } from "./service"

let dir = ""
let c1 = ""
let c2 = ""

const svc = new GitService({ writeEnabled: true, remoteEnabled: false, credentialEnv: () => ({}) } as never)

function git(cmd: string): string {
  const p = Bun.spawnSync(["bash", "-lc", `cd '${dir}' && ${cmd}`], { stdout: "pipe", stderr: "pipe" })
  if (p.exitCode !== 0) throw new Error(`${cmd}\n${p.stderr.toString()}`)
  return p.stdout.toString().trim()
}

const lines = (s: string, n: number): string => Array.from({ length: n }, (_, i) => `${s} ${i}`).join("\n")

beforeAll(() => {
  dir = mkdtempSync(join(tmpdir(), "gebai-git-"))
  git("git init -q -b main && git config user.email t@t && git config user.name T")
  mkdirSync(join(dir, "src"), { recursive: true })
  writeFileSync(join(dir, "src/a.ts"), lines("A", 5))
  writeFileSync(join(dir, "readme.md"), "# 一\n")
  git("git add -A && git commit -q -m 'feat: 初始'")
  c1 = git("git rev-parse HEAD")
  writeFileSync(join(dir, "src/a.ts"), `${lines("A", 5)}\n${lines("B", 3)}`)
  writeFileSync(join(dir, "src/new.ts"), "export const n = 1\n")
  git("git add -A && git commit -q -m 'feat: 追加'")
  c2 = git("git rev-parse HEAD")
  // 工作区：未暂存改动 + 已暂存新增 + 重命名
  writeFileSync(join(dir, "src/a.ts"), `${lines("A", 5)}\n${lines("B", 3)}\n// 工作区又加了一行\n`)
  writeFileSync(join(dir, "src/staged.ts"), "export const s = 2\n")
  git("git add src/staged.ts")
  git("git mv readme.md README.md")
})

afterAll(() => {
  rmSync(dir, { recursive: true, force: true })
})

describe("git 任意两端对比：提交 ↔ 提交", () => {
  test("两个提交之间的文件清单与增删统计", async () => {
    const r = await svc.compare(dir, { from: c1, to: c2 })
    expect(r.files.map((f) => f.path).sort()).toEqual(["src/a.ts", "src/new.ts"])
    expect(r.additions).toBe(5)
    expect(r.deletions).toBe(1)
    expect(r.files.find((f) => f.path === "src/new.ts")?.status).toBe("added")
  })

  test("提交 ↔ 提交：反向对比（B → A）语义对称", async () => {
    const r = await svc.compare(dir, { from: c2, to: c1 })
    expect(r.files.find((f) => f.path === "src/new.ts")?.status).toBe("deleted")
    expect(r.deletions).toBeGreaterThan(0)
  })

  test("相邻提交用 HEAD~/HEAD 亦可（rev 透传）", async () => {
    const r = await svc.compare(dir, { from: "HEAD~1", to: "HEAD" })
    expect(r.files.map((f) => f.path)).toContain("src/a.ts")
  })
})

describe("git 任意两端对比：提交 ↔ 工作树 / 暂存区", () => {
  test("提交 ↔ 工作区（WORKTREE）含未暂存与已暂存、并识别重命名", async () => {
    const r = await svc.compare(dir, { from: c2, to: "WORKTREE" })
    const paths = r.files.map((f) => f.path)
    expect(paths).toContain("src/a.ts")
    expect(paths.some((p) => p.endsWith("staged.ts"))).toBe(true)
    const renamed = r.files.find((f) => f.status === "renamed")
    expect(renamed?.oldPath).toBe("readme.md")
    expect(renamed?.path).toBe("README.md")
  })

  test("提交 ↔ 暂存区（INDEX）只看已暂存内容", async () => {
    const r = await svc.compare(dir, { from: c2, to: "INDEX" })
    const paths = r.files.map((f) => f.path)
    expect(paths.some((p) => p.endsWith("staged.ts"))).toBe(true)
    expect(paths).not.toContain("src/a.ts") // a.ts 的改动尚未暂存
  })

  test("暂存区 ↔ 工作区（未暂存改动）", async () => {
    const r = await svc.compare(dir, { from: "WORKTREE", to: "" })
    expect(r.files.map((f) => f.path)).toContain("src/a.ts")
  })

  test("工作区 ↔ 任意历史提交（to 为 rev、from 为 WORKTREE）", async () => {
    const r = await svc.compare(dir, { from: "WORKTREE", to: c1 })
    expect(r.files.map((f) => f.path)).toContain("src/new.ts")
  })
})

describe("git 任意两端对比：分支/标签（共同祖先语义）", () => {
  test("mergeBase=true 时只显示各自分支引入的改动（三点 ... 语义）", async () => {
    git("git checkout -q -b other")
    writeFileSync(join(dir, "src/other.ts"), "export const o = 3\n")
    // 只提交本分支的新文件：保留工作区其余脏状态，不影响其它用例
    // 只提交该路径（pathspec commit）：暂存区里其它内容保持暂存状态，供后续用例断言
    git("git add src/other.ts && git commit -q -m 'feat: other' -- src/other.ts")
    const r = await svc.compare(dir, { from: "main", to: "other", mergeBase: true })
    expect(r.mergeBaseOf).toBeTruthy()
    expect(r.files.map((f) => f.path)).toContain("src/other.ts")
    const two = await svc.compare(dir, { from: "main", to: "other" })
    expect(two.files.length).toBeGreaterThanOrEqual(r.files.length)
    git("git checkout -q main")
  })
})

describe("git 两侧内容取数（差异视图的数据源）", () => {
  test("contentAt：WORKTREE / INDEX / 历史提交 / 不存在的端点", async () => {
    const work = await svc.contentAt(dir, "WORKTREE", "src/a.ts")
    const idx = await svc.contentAt(dir, "INDEX", "src/a.ts")
    const old = await svc.contentAt(dir, c1, "src/a.ts")
    const missing = await svc.contentAt(dir, c1, "src/new.ts")
    expect(work.content).toContain("工作区又加了一行")
    expect(idx.content).not.toContain("工作区又加了一行")
    expect(old.content.split("\n")).toHaveLength(5)
    expect(missing.missing).toBe(true)
    expect(work.ref).toBe("WORKTREE")
  })

  test("fileDiff：任意两端单文件差异（并列视图左/右文本来源）", async () => {
    const byCommit = await svc.fileDiff(dir, "src/a.ts", { from: c1, to: c2 })
    const byWorktree = await svc.fileDiff(dir, "src/a.ts", { from: c2, to: "WORKTREE" })
    expect(byCommit.additions).toBeGreaterThan(0)
    expect(byWorktree.additions).toBeGreaterThan(0)
  })
})

describe("git 引用与状态（图形化界面的骨架数据）", () => {
  test("refs：分支/标签/最近提交/HEAD 与当前分支", async () => {
    const refs = await svc.refs(dir, { recent: 5 })
    expect(refs.branches.map((b) => b.name)).toContain("main")
    expect(refs.branches.map((b) => b.name)).toContain("other")
    expect(refs.recent.length).toBeGreaterThanOrEqual(2)
    expect(refs.head.branch).toBeTruthy()
    expect(refs.recent[0].hash).toMatch(/^[0-9a-f]{7,40}$/)
  })

  test("status：变更分组与计数（含重命名与未跟踪）", async () => {
    const s = await svc.status(dir)
    expect(s.isRepo).toBe(true)
    const staged = s.changes.filter((c) => c.staged)
    expect(staged.some((c) => c.path.endsWith("staged.ts"))).toBe(true)
    const renames = s.changes.filter((c) => c.path === "README.md" || c.origPath === "readme.md")
    expect(renames.length).toBeGreaterThan(0)
    expect(s.counts.staged + s.counts.unstaged + s.counts.untracked).toBeGreaterThan(0)
  })

  test("log：提交历史含作者/时间/主题（日志视图数据源）", async () => {
    const page = await svc.log(dir, { limit: 10 })
    expect(page.commits.length).toBeGreaterThanOrEqual(2)
    const c = page.commits[0]
    expect(c.hash).toBeTruthy()
    expect(c.subject).toContain("feat:")
    expect(c.author).toBe("T")
    expect(c.authorTime).toBeGreaterThan(0)
  })

  test("非仓库路径：requireRepo 抛可读错误（前端显示「不是 Git 仓库」）", async () => {
    const plain = mkdtempSync(join(tmpdir(), "gebai-norepo-"))
    try {
      await expect(svc.requireRepo(plain)).rejects.toThrow()
      expect(await svc.isRepo(plain)).toBe(false)
    } finally {
      rmSync(plain, { recursive: true, force: true })
    }
  })
})
