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
import { EMPTY_TREE, GitService } from "./service"

let dir = ""
let c1 = ""
let c2 = ""

const svc = new GitService({ writeEnabled: true, remoteEnabled: false, credentialEnv: () => ({}) } as never)

/** 真实 git 执行（数组传参 + cwd 定位：不经 shell，跨平台一致且免去引号转义）。 */
function runGit(cwd: string, args: string[]): string {
  const p = Bun.spawnSync(["git", ...args], { cwd, stdout: "pipe", stderr: "pipe" })
  if (p.exitCode !== 0) throw new Error(`git ${args.join(" ")}\n${p.stderr.toString()}`)
  return p.stdout.toString().trim()
}

function git(...args: string[]): string {
  return runGit(dir, args)
}

const lines = (s: string, n: number): string => Array.from({ length: n }, (_, i) => `${s} ${i}`).join("\n")

beforeAll(() => {
  dir = mkdtempSync(join(tmpdir(), "gebai-git-"))
  git("init", "-q", "-b", "main")
  git("config", "user.email", "t@t")
  git("config", "user.name", "T")
  mkdirSync(join(dir, "src"), { recursive: true })
  writeFileSync(join(dir, "src/a.ts"), lines("A", 5))
  writeFileSync(join(dir, "readme.md"), "# 一\n")
  git("add", "-A")
  git("commit", "-q", "-m", "feat: 初始")
  c1 = git("rev-parse", "HEAD")
  writeFileSync(join(dir, "src/a.ts"), `${lines("A", 5)}\n${lines("B", 3)}`)
  writeFileSync(join(dir, "src/new.ts"), "export const n = 1\n")
  git("add", "-A")
  git("commit", "-q", "-m", "feat: 追加")
  c2 = git("rev-parse", "HEAD")
  // 工作区：未暂存改动 + 已暂存新增 + 重命名
  writeFileSync(join(dir, "src/a.ts"), `${lines("A", 5)}\n${lines("B", 3)}\n// 工作区又加了一行\n`)
  writeFileSync(join(dir, "src/staged.ts"), "export const s = 2\n")
  git("add", "src/staged.ts")
  git("mv", "readme.md", "README.md")
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
    git("checkout", "-q", "-b", "other")
    writeFileSync(join(dir, "src/other.ts"), "export const o = 3\n")
    // 只提交本分支的新文件：保留工作区其余脏状态，不影响其它用例
    // 只提交该路径（pathspec commit）：暂存区里其它内容保持暂存状态，供后续用例断言
    git("add", "src/other.ts")
    git("commit", "-q", "-m", "feat: other", "--", "src/other.ts")
    const r = await svc.compare(dir, { from: "main", to: "other", mergeBase: true })
    expect(r.mergeBaseOf).toBeTruthy()
    expect(r.files.map((f) => f.path)).toContain("src/other.ts")
    const two = await svc.compare(dir, { from: "main", to: "other" })
    expect(two.files.length).toBeGreaterThanOrEqual(r.files.length)
    git("checkout", "-q", "main")
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

describe("git 根提交（无父提交）：A 侧归一到空树", () => {
  /**
   * 背景（实测复现）：前端的提交详情把 A 侧与文件清单都挂在 `${hash}^` 上。
   * 对根提交，git 对 `c1^` 直接报 `ambiguous argument`（422）——而它相对什么变化
   * 不是错误，是**空树**（所有文件对根提交都是新增）。不归一的话跨文件导航
   * 会因为 422 被 prepareReview 静默吞掉、按钮凭空消失。
   */
  test("compare：`<根提交>^` ⇄ `<根提交>` 不报错，且清单等于该提交的全部文件（= git show 口径）", async () => {
    const r = await svc.compare(dir, { from: `${c1}^`, to: c1 })
    const byShow = git("show", "--name-only", "--format=", c1).split("\n").filter(Boolean).sort()
    expect(r.files.map((f) => f.path).sort().filter((p, i, a) => a.indexOf(p) === i)).toEqual(byShow)
    expect(r.files.length).toBeGreaterThanOrEqual(2)
    // 根提交里所有文件都是「新增」
    expect(r.files.every((f) => f.status === "added" || f.additions > 0)).toBe(true)
  })

  test("diff / fileDiff：根提交的 A 侧同样可用（不再 422）", async () => {
    const d = await svc.diff(dir, { from: `${c1}^`, to: c1 })
    expect(d.files.length).toBeGreaterThanOrEqual(2)
    const one = await svc.fileDiff(dir, "readme.md", { from: `${c1}^`, to: c1 })
    expect(one.additions).toBeGreaterThan(0)
  })

  test("contentAt：根提交的 `^` 侧取不到内容 → missing（前端渲染为空文档）", async () => {
    const a = await svc.contentAt(dir, `${c1}^`, "readme.md")
    expect(a.missing).toBe(true)
    expect(a.content).toBe("")
    const b = await svc.contentAt(dir, c1, "readme.md")
    expect(b.missing).toBe(false)
    expect(b.content).toContain("#")
  })

  test("mergeBase：根提交参与比较时基准即空树（不发 merge-base 子进程）", async () => {
    const r = await svc.compare(dir, { from: `${c1}^`, to: c1, mergeBase: true })
    expect(r.mergeBaseOf).toBe(EMPTY_TREE)
  })

  test("回归：非根提交端点行为不变；非法 rev 仍报 422（不被静默改成空树）", async () => {
    const normal = await svc.compare(dir, { from: `${c2}^`, to: c2 })
    expect(normal.files.map((f) => f.path)).toContain("src/new.ts")
    // 不存在的 rev：必须仍然报错（若被当成空树，就是拿错数据当差分了）
    await expect(svc.compare(dir, { from: "deadbeefdeadbeef^", to: c2 })).rejects.toThrow()
  })
})

describe("git 体量上限：把「撑不住」变成看得见的事", () => {
  /**
   * 背景（实测）：一个 262 文件 / 3.1MB diff 的提交，服务端要拼 6.5MB JSON、
   * 前端要等 8s；单文件内容则会被整份拉进 Monaco model。
   * 上限不是“省资源”，而是保证最坏情况退化为**统计 + 标记**，而不是卡死或默默少东西。
   */
  let d2 = ""
  let big = ""
  beforeAll(() => {
    d2 = mkdtempSync(join(tmpdir(), "gebai-git-cap-"))
    const g = (...args: string[]): string => runGit(d2, args)
    g("init", "-q", "-b", "main")
    g("config", "user.email", "t@t")
    g("config", "user.name", "T")
    for (let i = 0; i < 12; i += 1) writeFileSync(join(d2, `f${i}.txt`), lines(`L${i}`, 20))
    g("add", "-A")
    g("commit", "-q", "-m", "c1")
    for (let i = 0; i < 12; i += 1) writeFileSync(join(d2, `f${i}.txt`), `${lines(`L${i}`, 20)}\n${lines("ADD", 15)}`)
    g("add", "-A")
    g("commit", "-q", "-m", "c2")
    big = g("rev-parse", "HEAD")
  })
  afterAll(() => rmSync(d2, { recursive: true, force: true }))

  /** 阈值缩到极小的探针实例（同一仓库、同一套代码路径）。 */
  const tiny = new GitService({ writeEnabled: false, remoteEnabled: false, maxDiffBytes: 300, maxFileChars: 120 } as never)

  test("contentAt：超限的文件不拉正文，只给 tooLarge + size", async () => {
    const r = await tiny.contentAt(d2, big, "f0.txt")
    expect(r.tooLarge).toBe(true)
    expect(r.content).toBe("")
    expect(r.size).toBeGreaterThan(120)
    // 阈值以内的文件正常拿到内容（不能一刀切全封）
    writeFileSync(join(d2, "small.txt"), "hi\n")
    const ok = await tiny.contentAt(d2, "WORKTREE", "small.txt")
    expect(ok.tooLarge).toBeUndefined()
    expect(ok.content).toBe("hi\n")
  })

  test("compare：diff 文本超限 → 标 truncated，但**文件清单仍然完整**（靠 --name-status 补齐）", async () => {
    const r = await tiny.compare(d2, { from: `${big}^`, to: big })
    expect(r.truncated).toBe(true)
    expect(r.files.length).toBe(12)
    // 被截掉的那些文件没有逐行内容，但状态/统计仍在
    const noHunks = r.files.filter((f) => !f.hunks.length)
    expect(noHunks.length).toBeGreaterThan(0)
    expect(r.files.every((f) => f.additions + f.deletions > 0)).toBe(true)
  })

  test("fileDiff：单文件超限 → hunks 清空 + truncated（而不是半个 hunk 或报错）", async () => {
    const f = await tiny.fileDiff(d2, "f0.txt", { from: `${big}^`, to: big })
    expect(f.truncated).toBe(true)
    expect(f.hunks).toHaveLength(0)
    expect(f.additions).toBeGreaterThan(0)
  })

  test("commitDetail：超大提交同样退化（truncated + 清单完整）", async () => {
    const r = await tiny.commitDetail(d2, big)
    expect(r.truncated).toBe(true)
    expect(r.files.length).toBe(12)
    expect(r.stats).toContain("f0.txt")
  })

  test("默认阈值下的正常仓库：不误伤（不标 truncated / tooLarge）", async () => {
    const normal = await svc.compare(dir, { from: c1, to: c2 })
    expect(normal.truncated).toBe(false)
    const content = await svc.contentAt(dir, c1, "readme.md")
    expect(content.tooLarge).toBeUndefined()
    expect(content.missing).toBe(false)
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
