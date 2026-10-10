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
import { EMPTY_TREE, GitService, buildPartialPatch } from "./service"

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

describe("git 日志按提交 ID 定位（rev）", () => {
  test("完整哈希 → 只返回那一条，hasMore=false（不翻页）", async () => {
    const r = await svc.log(dir, { rev: c1 })
    expect(r.commits.map((c) => c.hash)).toEqual([c1])
    expect(r.commits[0]!.parents).toEqual([])
    expect(r.hasMore).toBe(false)
  })

  test("短哈希前缀同样命中", async () => {
    const r = await svc.log(dir, { rev: c1.slice(0, 8) })
    expect(r.commits.map((c) => c.hash)).toEqual([c1])
  })

  test("rev 优先于 ref / all / grep（其余过滤不再叠加）", async () => {
    const r = await svc.log(dir, { rev: c1, all: true, ref: "main", grep: "不存在的词" })
    expect(r.commits.map((c) => c.hash)).toEqual([c1])
  })

  test("解析不出的 rev（打错前缀 / 非法串）→ 空列表不报错（过滤语义，不是命令失败）", async () => {
    expect((await svc.log(dir, { rev: "deadbeef" })).commits).toEqual([])
    expect((await svc.log(dir, { rev: "..zz.." })).commits).toEqual([])
    expect((await svc.log(dir, { rev: "HEAD~99" })).commits).toEqual([])
  })

  test("与路径过滤叠加：该提交确实改过这个文件才命中", async () => {
    const hit = await svc.log(dir, { rev: c2, path: "src/new.ts" })
    expect(hit.commits.map((c) => c.hash)).toEqual([c2])
    const miss = await svc.log(dir, { rev: c1, path: "src/new.ts" })
    expect(miss.commits).toEqual([])
  })
})

describe("git 日志过滤：字面 / 正则 / 大小写", () => {
  const subjects = async (opts: Parameters<typeof svc.log>[1]): Promise<string[]> => (await svc.log(dir, { limit: 10, ...opts })).commits.map((c) => c.subject)

  test("默认按字面文本搜（元字符不当正则用）", async () => {
    expect(await subjects({ grep: "feat: 初始" })).toEqual(["feat: 初始"])
    // `.` 在正则下是任意字符、在字面下只是个点：不加 --fixed-strings 时这条会命中，用户眼里就是「关不掉正则」
    expect(await subjects({ grep: "feat. 初始" })).toEqual([])
  })

  test("开启正则后元字符生效", async () => {
    expect(await subjects({ grep: "feat. 初始", grepRegex: true })).toEqual(["feat: 初始"])
    expect(await subjects({ grep: "feat: (初始|追加)", grepRegex: true })).toHaveLength(2)
  })

  test("时间范围：since 排除更早的提交（自带仓库：一条 2020 年的提交 + 一条刚才的）", async () => {
    const d = mkdtempSync(join(tmpdir(), "gebai-git-since-"))
    try {
      const g = (args: string[], env?: Record<string, string>): string => {
        const p = Bun.spawnSync(["git", ...args], { cwd: d, stdout: "pipe", stderr: "pipe", env: { ...process.env, ...env } as Record<string, string> })
        if (p.exitCode !== 0) throw new Error(p.stderr.toString())
        return p.stdout.toString().trim()
      }
      g(["init", "-q", "-b", "main"])
      g(["config", "user.email", "t@t"])
      g(["config", "user.name", "T"])
      const old = { GIT_AUTHOR_DATE: "2020-01-01T00:00:00", GIT_COMMITTER_DATE: "2020-01-01T00:00:00" }
      g(["commit", "-q", "--allow-empty", "-m", "old-2020"], old)
      g(["commit", "-q", "--allow-empty", "-m", "new-today"])
      const subjects = async (opts: Parameters<typeof svc.log>[1]): Promise<string[]> => (await svc.log(d, { limit: 10, ...opts })).commits.map((c) => c.subject)
      expect(await subjects({})).toEqual(["new-today", "old-2020"])
      // 预设就这么用：相对日期直接下传，git 自己按提交日期截断
      expect(await subjects({ since: "1 year ago" })).toEqual(["new-today"])
      expect(await subjects({ since: "midnight" })).toEqual(["new-today"])
    } finally {
      rmSync(d, { recursive: true, force: true })
    }
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

  test("branches：本地带斜杠的分支不得被当成远程（看 ref 命名空间，不看名字里有没有斜杠）", async () => {
    const d = mkdtempSync(join(tmpdir(), "gebai-git-branch-"))
    try {
      runGit(d, ["init", "-q", "-b", "main"])
      runGit(d, ["config", "user.email", "t@t"])
      runGit(d, ["config", "user.name", "T"])
      runGit(d, ["commit", "-q", "--allow-empty", "-m", "c1"])
      runGit(d, ["branch", "feature/alpha"])
      runGit(d, ["branch", "topic"])
      // 造一个真的远程引用：把仓库自己当远程抓一份回来（refs/remotes/origin/*）
      runGit(d, ["remote", "add", "origin", d])
      runGit(d, ["fetch", "-q", "origin"])

      const list = await svc.branches(d)
      const by = (n: string) => list.find((b) => b.name === n)
      expect(by("feature/alpha")?.remote).toBe(false)
      expect(by("topic")?.remote).toBe(false)
      expect(by("main")?.remote).toBe(false)
      expect(by("main")?.current).toBe(true)
      expect(by("origin/main")?.remote).toBe(true)
      // 排序：当前分支在最前，本地分支全部排在远程分支之前
      expect(list[0]?.name).toBe("main")
      const firstRemote = list.findIndex((b) => b.remote)
      expect(list.slice(0, firstRemote).every((b) => !b.remote)).toBe(true)
    } finally {
      rmSync(d, { recursive: true, force: true })
    }
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

  test("network fetch：all=true 生成 --all --prune 且不带 remote（抓全部远程）；无 all 时仍按 remote 抓单个", async () => {
    const calls: string[][] = []
    const spy = new GitService({ writeEnabled: true, remoteEnabled: true, credentialEnv: () => ({}) } as never)
    const realRun = (spy as unknown as { run: (...a: unknown[]) => Promise<{ code: number; stdout: string; stderr: string }> }).run.bind(spy)
    // 只拦截 fetch 本身（requireRepo/repoRoot 等仍走真实子进程）；记录拼好的参数钉住拼装逻辑
    ;(spy as unknown as { run: unknown }).run = (args: string[], ...rest: unknown[]) => {
      if (args[0] === "fetch") {
        calls.push(args)
        return Promise.resolve({ code: 0, stdout: "", stderr: "" })
      }
      return realRun(args, ...rest)
    }
    await spy.network(dir, "fetch", { all: true, prune: true })
    await spy.network(dir, "fetch", { remote: "origin", prune: true })
    expect(calls[0]!.join(" ")).toBe("fetch --all --prune")
    expect(calls[1]!.join(" ")).toBe("fetch --prune origin")
  })
})

/** 手写单文件补丁（含两个增两个删）：计数重写的断言用它，不依赖 git 生成。 */
const RAW_ONE_HUNK = [
  "diff --git a/f.txt b/f.txt",
  "index 1111111..2222222 100644",
  "--- a/f.txt",
  "+++ b/f.txt",
  "@@ -1,5 +1,5 @@",
  " A 0",
  "-A 1",
  "-A 2",
  "+A 1 mod",
  "+A 2 mod",
  " A 3",
  " A 4",
  "",
].join("\n")

describe("部分暂存：补丁构造（纯函数）", () => {
  const body = (p: string): string[] => p.split("\n").slice(5).filter((l) => l !== "")

  test("整块选中：原样输出，计数不变", () => {
    const r = buildPartialPatch(RAW_ONE_HUNK, [{ hunk: 0 }])
    expect(r.hunks).toEqual([0])
    expect(r.changed).toBe(4)
    expect(r.patch).toContain("@@ -1,5 +1,5 @@")
    expect(body(r.patch)).toEqual([" A 0", "-A 1", "-A 2", "+A 1 mod", "+A 2 mod", " A 3", " A 4"])
  })

  test("只选删除行：未选中的新增行**整行丢弃**，新侧计数减少", () => {
    const r = buildPartialPatch(RAW_ONE_HUNK, [{ hunk: 0, lines: [1, 2] }])
    expect(r.changed).toBe(2)
    expect(r.patch).toContain("@@ -1,5 +1,3 @@")
    expect(body(r.patch)).toEqual([" A 0", "-A 1", "-A 2", " A 3", " A 4"])
  })

  test("只选新增行：未选中的删除行**降级为上下文**（两不误），旧侧不变、新侧变多", () => {
    const r = buildPartialPatch(RAW_ONE_HUNK, [{ hunk: 0, lines: [3, 4] }])
    expect(r.changed).toBe(2)
    expect(r.patch).toContain("@@ -1,5 +1,7 @@")
    expect(body(r.patch)).toEqual([" A 0", " A 1", " A 2", "+A 1 mod", "+A 2 mod", " A 3", " A 4"])
  })

  test("单行选中：只拿一条改动，另一条保持原样", () => {
    const r = buildPartialPatch(RAW_ONE_HUNK, [{ hunk: 0, lines: [1, 3] }])
    expect(r.changed).toBe(2)
    expect(body(r.patch)).toEqual([" A 0", "-A 1", " A 2", "+A 1 mod", " A 3", " A 4"])
  })

  test("未选中任何改动（空选择 / 越界 hunk）→ 空补丁（调用方据此报「改动已变化」）", () => {
    expect(buildPartialPatch(RAW_ONE_HUNK, []).patch).toBe("")
    expect(buildPartialPatch(RAW_ONE_HUNK, [{ hunk: 9 }]).patch).toBe("")
    expect(buildPartialPatch(RAW_ONE_HUNK, [{ hunk: 0, lines: [] }]).patch).toBe("")
  })

  test("`\\ No newline at end of file` 只跟随被发出的行（丢弃的新增行不该带出它）", () => {
    const raw = [
      "diff --git a/n.txt b/n.txt",
      "--- a/n.txt",
      "+++ b/n.txt",
      "@@ -1,2 +1,2 @@",
      " x",
      "-y",
      "\\ No newline at end of file",
      "+z",
      "\\ No newline at end of file",
      "",
    ].join("\n")
    const whole = buildPartialPatch(raw, [{ hunk: 0 }])
    expect(whole.patch.split("\\ No newline").length - 1).toBe(2)
    const onlyDel = buildPartialPatch(raw, [{ hunk: 0, lines: [1] }])
    expect(onlyDel.patch).toContain("-y")
    expect(onlyDel.patch).not.toContain("+z")
    expect(onlyDel.patch.split("\\ No newline").length - 1).toBe(1)
  })
})

describe("部分暂存：写入工作区 / 暂存区（真实仓库）", () => {
  /** 专用仓库：与上面共享 fixture 隔离（那些用例依赖特定的脏状态）。 */
  let d3 = ""
  const file = (): string => join(d3, "src/f.txt")
  /** 两个相距足够远的改动 = 两个独立 hunk（第一个 hunk 内两条相邻改动，供行级测试用）。 */
  const dirty = (): void => {
    const base = Array.from({ length: 20 }, (_, i) => `A ${i}`)
    base[1] = "A 1 mod"
    base[2] = "A 2 mod"
    base[17] = "A 17 mod"
    writeFileSync(file(), `${base.join("\n")}\n`)
  }
  const clean = (): void => {
    const base = Array.from({ length: 20 }, (_, i) => `A ${i}`)
    writeFileSync(file(), `${base.join("\n")}\n`)
  }

  beforeAll(() => {
    d3 = mkdtempSync(join(tmpdir(), "gebai-git-partial-"))
    const g = (...args: string[]): string => runGit(d3, args)
    g("init", "-q", "-b", "main")
    g("config", "user.email", "t@t")
    g("config", "user.name", "T")
    mkdirSync(join(d3, "src"), { recursive: true })
    clean()
    g("add", "-A")
    g("commit", "-q", "-m", "c1")
  })

  afterAll(() => rmSync(d3, { recursive: true, force: true }))

  /** 每个用例前回到「干净 HEAD + 两个 hunk 的脏工作区」。 */
  const reset = (): void => {
    runGit(d3, ["reset", "-q", "--hard", "HEAD"])
    dirty()
  }

  test("按块暂存：选中第一个 hunk，暂存区只多这一块，工作区不动", async () => {
    reset()
    const r = await svc.stageHunks(d3, "src/f.txt", [{ hunk: 0 }])
    expect(r.hunks).toEqual([0])
    const idx = await svc.contentAt(d3, "INDEX", "src/f.txt")
    expect(idx.content).toContain("A 1 mod")
    expect(idx.content).not.toContain("A 17 mod")
    // 工作区仍然带着两处改动
    const work = await svc.contentAt(d3, "WORKTREE", "src/f.txt")
    expect(work.content).toContain("A 1 mod")
    expect(work.content).toContain("A 17 mod")
  })

  test("按行暂存：同一 hunk 里只暂存一条改动（其余降级为上下文，内容不丢）", async () => {
    reset()
    // hunk0 行序：0=ctx, 1=del A1, 2=del A2, 3=add A1, 4=add A2, 5=ctx, 6=ctx
    await svc.stageHunks(d3, "src/f.txt", [{ hunk: 0, lines: [1, 3] }])
    const idx = await svc.contentAt(d3, "INDEX", "src/f.txt")
    expect(idx.content).toContain("A 1 mod")
    expect(idx.content).toContain("A 2\n") // 第二条改动仍在工作区，暂存区里保持原值
    expect(idx.content).not.toContain("A 2 mod")
  })

  test("按块取消暂存：已暂存的块退回工作区（`git reset -p` 语义）", async () => {
    reset()
    await svc.stageHunks(d3, "src/f.txt", [{ hunk: 0 }])
    expect((await svc.contentAt(d3, "INDEX", "src/f.txt")).content).toContain("A 1 mod")
    // 取消暂存时补丁取 HEAD→暂存区，此时只剩已暂存的那一块
    await svc.unstageHunks(d3, "src/f.txt", [{ hunk: 0 }])
    const idx = await svc.contentAt(d3, "INDEX", "src/f.txt")
    expect(idx.content).not.toContain("A 1 mod")
    // 退回工作区：内容还在
    expect((await svc.contentAt(d3, "WORKTREE", "src/f.txt")).content).toContain("A 1 mod")
  })

  test("按块丢弃：只撤掉选中块（带 stash 备份），另一块保留在工作区", async () => {
    reset()
    const before = (await svc.stashList(d3)).length
    const r = await svc.discardHunks(d3, "src/f.txt", [{ hunk: 1 }], { backup: true })
    expect(r.backupRef).toBe("stash@{0}")
    expect((await svc.stashList(d3)).length).toBe(before + 1)
    const work = await svc.contentAt(d3, "WORKTREE", "src/f.txt")
    expect(work.content).toContain("A 1 mod")
    expect(work.content).not.toContain("A 17 mod")
    // 备份是「先存后还原」的：备份条目在，工作区仍是操作前的内容
    expect(runGit(d3, ["stash", "show", "-p", "stash@{0}"])).toContain("A 17 mod")
    await svc.stashOp(d3, "drop", { index: 0 })
  })

  test("选择已不成立的改动：报 422（提示刷新），不静默改错内容", async () => {
    reset()
    const raw = Array.from({ length: 20 }, (_, i) => `A ${i}`)
    writeFileSync(file(), `${raw.join("\n")}\n`) // 还原干净：已无可暂存的 hunk
    await expect(svc.stageHunks(d3, "src/f.txt", [{ hunk: 0 }])).rejects.toThrow()
  })

  test("新增文件：未跟踪时没有 diff（报「没有可比对的改动」），已暂存时也不支持部分取消暂存", async () => {
    reset()
    writeFileSync(join(d3, "src/brand.txt"), "new\n")
    // 未跟踪：git diff 里根本不出现这个文件
    await expect(svc.stageHunks(d3, "src/brand.txt", [{ hunk: 0 }])).rejects.toThrow(/没有可比对的改动/)
    // 整文件暂存后：补丁是 new file mode，没有「部分」可言
    await svc.stage(d3, ["src/brand.txt"])
    await expect(svc.unstageHunks(d3, "src/brand.txt", [{ hunk: 0 }])).rejects.toThrow(/新增／删除/)
    await svc.unstage(d3, ["src/brand.txt"])
    rmSync(join(d3, "src/brand.txt"), { force: true })
  })
})

describe("编辑历史（交互式变基）：重放 / 压合 / 丢弃 / 冲突与中止", () => {
  /** 专用仓库：三条可自由处置的提交 + 一个可造冲突的分支。 */
  let d4 = mkdtempSync(join(tmpdir(), "gebai-git-history-"))
  const g = (...args: string[]): string => runGit(d4, args)
  const write = (name: string, text: string): void => writeFileSync(join(d4, name), text)
  const hashes: string[] = []

  /** 每条用例都从同一初始状态开始：base 之前的提交 + 三条待编辑提交。 */
  const setup = (): void => {
    rmSync(d4, { recursive: true, force: true })
    mkdirSync(d4, { recursive: true })
    g("init", "-q", "-b", "main")
    g("config", "user.email", "t@t")
    g("config", "user.name", "T")
    write("base.txt", "base\n")
    g("add", "-A")
    g("commit", "-q", "-m", "base")
    hashes.length = 0
    for (const [i, name] of ["a", "b", "c"].entries()) {
      write(`${name}.txt`, `${name}${i}\n`)
      g("add", "-A")
      g("commit", "-q", "-m", `提交 ${name}`)
      hashes.push(g("rev-parse", "HEAD"))
    }
  }

  beforeAll(() => setup())
  afterAll(() => rmSync(d4, { recursive: true, force: true }))

  test("reword：只改提交信息（内容不变）", async () => {
    setup()
    const r = await svc.editHistory(d4, {
      base: "HEAD~3",
      steps: [
        { commit: hashes[0]!, action: "reword", message: "改写后的第一条" },
        { commit: hashes[1]!, action: "pick" },
        { commit: hashes[2]!, action: "pick" },
      ],
    })
    expect(r.ok).toBe(true)
    expect(r.applied).toBe(3)
    // 日志新→旧：最后应用的提交在最上面
    const subjects = g("log", "--pretty=%s").split("\n")
    expect(subjects).toEqual(["提交 c", "提交 b", "改写后的第一条", "base"])
    // 内容没变：重放保留了每条提交自己的改动
    expect(g("show", "--name-only", "--format=", "HEAD").trim()).toBe("c.txt")
    expect(g("show", "--name-only", "--format=", "HEAD~2").trim()).toBe("a.txt")
  })

  test("squash / fixup：三条合成两条（fixup 丢弃被压合提交的信息）", async () => {
    setup()
    const r = await svc.editHistory(d4, {
      base: "HEAD~3",
      steps: [
        { commit: hashes[0]!, action: "pick" },
        { commit: hashes[1]!, action: "squash" },
        { commit: hashes[2]!, action: "fixup" },
      ],
    })
    expect(r.ok).toBe(true)
    const subjects = g("log", "--pretty=%s").split("\n")
    expect(subjects).toContain("base")
    expect(subjects.some((s) => s.includes("提交 a"))).toBe(true)
    // squash 把两条信息合进上一条；fixup 丢掉信息，所以「提交 c」不应当单独成条
    const top = g("log", "-1", "--pretty=%B")
    expect(top).toContain("提交 a")
    expect(top).toContain("提交 b")
    expect(top).not.toContain("提交 c")
    expect(subjects.length).toBe(2) // base + 合成后的一条（fixup 也并进了同一条）
    // 三条提交的改动都在那一条里
    expect(g("show", "--name-only", "--format=", "HEAD").trim().split("\n").sort()).toEqual(["a.txt", "b.txt", "c.txt"])
  })

  test("drop + 排序：丢掉中间的提交并把后一条提到前面", async () => {
    setup()
    const r = await svc.editHistory(d4, {
      base: "HEAD~3",
      steps: [
        { commit: hashes[2]!, action: "pick" },
        { commit: hashes[0]!, action: "pick" },
        { commit: hashes[1]!, action: "drop" },
      ],
    })
    expect(r.ok).toBe(true)
    // steps 是**应用顺序**（旧→新）：c 先落地、a 后落地，所以日志里 a 在上
    const subjects = g("log", "--pretty=%s").split("\n")
    expect(subjects).toEqual(["提交 a", "提交 c", "base"])
    expect(g("show", "--name-only", "--format=", "HEAD").trim()).toBe("a.txt")
    expect(g("show", "--name-only", "--format=", "HEAD~1").trim()).toBe("c.txt")
  })

  test("备份与恢复：动历史前建备份分支，中止后回到原 HEAD", async () => {
    setup()
    const before = g("rev-parse", "HEAD")
    const r = await svc.editHistory(d4, { base: "HEAD~3", steps: [{ commit: hashes[0]!, action: "pick" }] })
    expect(r.backupBranch).toMatch(/^gebai\/backup-/)
    expect(g("rev-parse", r.backupBranch!)).toBe(before)
    expect(g("log", "--pretty=%s").split("\n").length).toBe(2) // base + a
  })

  test("工作区脏 / 游离 HEAD / 非祖先基准：都明确报错而不动历史", async () => {
    setup()
    write("dirty.txt", "dirty\n")
    await expect(svc.editHistory(d4, { base: "HEAD~3", steps: [{ commit: hashes[0]!, action: "pick" }] })).rejects.toThrow(/工作区有未提交/)
    rmSync(join(d4, "dirty.txt"), { force: true })
    await expect(svc.editHistory(d4, { base: "不存在的引用", steps: [{ commit: hashes[0]!, action: "pick" }] })).rejects.toThrow(/不是当前分支的祖先/)
    g("checkout", "-q", "--detach")
    await expect(svc.editHistory(d4, { base: "HEAD~3", steps: [{ commit: hashes[0]!, action: "pick" }] })).rejects.toThrow(/游离 HEAD/)
    g("checkout", "-q", "main")
    expect(g("rev-parse", "HEAD")).toBe(hashes[2])
    expect(await svc.editPlan(d4)).toBeNull()
  })

  test("edit：停在该提交（计划落盘），继续后接着重放完", async () => {
    setup()
    const r = await svc.editHistory(d4, {
      base: "HEAD~3",
      steps: [
        { commit: hashes[0]!, action: "edit" },
        { commit: hashes[1]!, action: "pick" },
      ],
    })
    expect(r.ok).toBe(false)
    expect(r.halted).toBe("edit")
    const plan = await svc.editPlan(d4)
    expect(plan?.branch).toBe("main")
    expect(plan?.index).toBe(1)
    // 停在第一条：此时只有 base + a
    expect(g("log", "--pretty=%s").split("\n")).toEqual(["提交 a", "base"])
    const done = await svc.continueHistoryEdit(d4)
    expect(done.ok).toBe(true)
    expect(g("branch", "--show-current")).toBe("main")
    expect(g("log", "--pretty=%s").split("\n")).toEqual(["提交 b", "提交 a", "base"])
    expect(await svc.editPlan(d4)).toBeNull()
  })

  test("冲突：停下来并给出冲突文件，中止后完全回到原状", async () => {
    setup()
    // 造一个必然冲突：把 base.txt 在两条提交里改成不同内容，再倒序重放
    const conflictHashes: string[] = []
    for (const text of ["一", "二"]) {
      write("base.txt", `${text}\n`)
      g("add", "-A")
      g("commit", "-q", "-m", `改 ${text}`)
      conflictHashes.push(g("rev-parse", "HEAD"))
    }
    const before = g("rev-parse", "HEAD")
    const r = await svc.editHistory(d4, {
      base: "HEAD~2",
      steps: [
        { commit: conflictHashes[1]!, action: "pick" },
        { commit: conflictHashes[0]!, action: "pick" },
      ],
    })
    expect(r.ok).toBe(false)
    expect(r.halted).toBe("conflict")
    expect(r.conflicts).toContain("base.txt")
    const aborted = await svc.abortHistoryEdit(d4)
    expect(aborted.branch).toBe("main")
    expect(g("rev-parse", "HEAD")).toBe(before)
    expect(g("branch", "--show-current")).toBe("main")
    expect(await svc.editPlan(d4)).toBeNull()
  })

  test("默认落库：重放出的提交是全新 hash（历史真的被改写了）", async () => {
    setup()
    await svc.editHistory(d4, { base: "HEAD~3", steps: [{ commit: hashes[0]!, action: "reword", message: "新的" }, { commit: hashes[1]!, action: "pick" }, { commit: hashes[2]!, action: "pick" }] })
    expect(g("rev-parse", "HEAD")).not.toBe(hashes[2])
    expect(await svc.editPlan(d4)).toBeNull()
  })
})
