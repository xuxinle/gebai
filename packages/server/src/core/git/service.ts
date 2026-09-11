/** 文件工作台 · Git 服务（DESIGN「文件工作台·Git 图形化」）。
 *
 *  为什么用宿主 git CLI 而非纯 JS 实现：rebase/cherry-pick/worktree/LFS/submodule/credential helper
 *  等功能纯 JS 库覆盖不全，写操作极易损坏仓库。CLI 即 git 本身，能力 100% 对齐。
 *  风险处置：
 *  - **注入**：一律数组参数 spawn，不经 shell（`git` 与参数分列，任何路径/分支名都是纯数据）；
 *  - **编码**：`-c core.quotepath=false -c i18n.logOutputEncoding=UTF-8` 保证中文路径/提交信息可读；
 *  - **并发**：写操作按仓库串行队列（否则并发触发 `.git/index.lock` 冲突，连点即坏仓库）；
 *  - **交互**：`GIT_TERMINAL_PROMPT=0` 杜绝等输入卡死；`--no-pager` 防分页器挂起；
 *  - **输出**：优先 porcelain=v2 / `-z`（NUL 分隔，免转义歧义），日志用 \x1f/\x1e 自定义分隔符自解析。
 */
import { existsSync, readFileSync, statSync } from "node:fs"
import { isAbsolute, join } from "node:path"

export interface GitResult {
  stdout: string
  stderr: string
  code: number
}

export class GitError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message)
    this.name = "GitError"
  }
}

/** 变更条目（porcelain v2 规范化后）。 */
export interface GitChange {
  path: string
  origPath?: string
  /** 暂存区状态码（X：'.' 表示无） */
  index: string
  /** 工作区状态码（Y：'.' 表示无） */
  worktree: string
  /** 未跟踪（??） */
  untracked: boolean
  /** 冲突（u） */
  conflicted: boolean
  /** 重命名/复制（2） */
  renamed: boolean
  staged: boolean
  unstaged: boolean
  kind: "added" | "modified" | "deleted" | "renamed" | "copied" | "typechange" | "untracked" | "conflicted" | "unknown"
}

export interface GitStatus {
  isRepo: boolean
  root?: string
  branch?: string
  oid?: string
  upstream?: string
  ahead: number
  behind: number
  detached: boolean
  unborn: boolean
  changes: GitChange[]
  counts: { staged: number; unstaged: number; untracked: number; conflicted: number }
  stashCount: number
  /** 进行中的多步操作（merge/rebase/cherry-pick/revert/bisect） */
  operation?: "merge" | "rebase" | "cherry-pick" | "revert" | "bisect" | "am"
}

export interface GitDiffLine {
  type: "context" | "add" | "del"
  text: string
  oldLine: number | null
  newLine: number | null
}

export interface GitDiffHunk {
  header: string
  oldStart: number
  oldLines: number
  newStart: number
  newLines: number
  lines: GitDiffLine[]
}

export interface GitFileDiff {
  path: string
  oldPath?: string
  binary: boolean
  hunks: GitDiffHunk[]
  additions: number
  deletions: number
  /** 新增/删除/重命名等语义 */
  status?: GitChange["kind"]
  /**
   * 逐行内容被省略（文件或整次 diff 超出体量上限）。
   *
   * 为什么需要这个标记：上限一旦生效，**不给逐行内容**与**这个文件没改动**
   * 在数据长得很像（都是 `hunks: []`）——UI 必须能区分，前者要提示
   * 「过大，已省略」，后者该老老实实显示空。统计（+N/-N）与状态不受影响，仍然准确。
   */
  truncated?: boolean
}

export interface GitCommit {
  hash: string
  short: string
  parents: string[]
  author: string
  authorEmail: string
  authorTime: number
  committer: string
  committerEmail: string
  commitTime: number
  refs: string[]
  subject: string
  body?: string
}

export interface GitBranch {
  name: string
  /** 本地 / 远程 */
  remote: boolean
  current: boolean
  hash: string
  upstream?: string
  ahead?: number
  behind?: number
  subject?: string
  time?: number
}

export interface GitTag {
  name: string
  hash: string
  time?: number
  subject?: string
}

export interface GitRemote {
  name: string
  fetchUrl: string
  pushUrl: string
}

export interface GitBlameLine {
  line: number
  hash: string
  author: string
  authorEmail: string
  time: number
  summary: string
  /** 该行是否为未提交改动（0000…） */
  uncommitted: boolean
}

const CMD_TIMEOUT_MS = 60_000
const NET_TIMEOUT_MS = 300_000

/** 自定义字段分隔（\x1f）与记录分隔（\x1e）：提交信息即含换行，不能用行分隔。 */
const F = "\x1f"
const R = "\x1e"

/**
 * git 的**空树对象**（所有仓库共有、内容恒定的那个 tree）。
 *
 * 用来表达「根提交的 A 侧」：根提交没有父提交，`<hash>^` 在 git 里是
 * `fatal: ambiguous argument '<hash>^': unknown revision`（422），但它相对什么变化
 * 并非「错误」而是**空树**——任何文件对它都是新增。前端的提交详情与文件清单
 * 都按 `${hash}^` ↔ `${hash}` 这一对端点取数，不归一的话根提交的跨文件导航
 * 会因为 422 被静默吞掉（`prepareReview` 的 catch），用户以为「没有更多文件」。
 */
export const EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"

export class GitService {
  /** 每仓库写操作串行队列（防 index.lock 竞争）。 */
  private locks = new Map<string, Promise<unknown>>()
  /** 仓库探测缓存（rev-parse --show-toplevel），TTL 内复用。 */
  private repoCache = new Map<string, { root: string | null; ts: number }>()
  private repoCacheTtl = 5000

  constructor(
    private opts: {
      /** 写操作总开关（GEBAI_GIT_WRITE=false → 只读） */
      writeEnabled: boolean
      /** 远程操作开关（GEBAI_GIT_REMOTE=false → fetch/pull/push 拒绝） */
      remoteEnabled: boolean
      /** 凭据注入（P4：临时 GIT_ASKPASS）；返回 env 增量 */
      credentialEnv?: (repo: string) => Record<string, string>
      /** 单次 diff 文本总量上限（GEBAI_FS_MAX_DIFF 字节，默认 8MB）：超限则在**文件边界**截断 */
      maxDiffBytes?: number
      /** 单个文本文件（内容与逐行差异）上限（GEBAI_GIT_MAX_FILE 字符，默认 1M）：超限只给统计 */
      maxFileChars?: number
    },
  ) {}

  /* 体量上限（构造参数可覆盖，见 opts）。
   * 目的不是省资源，而是**把「撑不住」变成一件看得见的事**：
   * 没上限时，超大文件/超大提交会把整份内容 JSON 化、给 Monaco 建巨型 model，
   * 表现是卡死或半途而废且没有任何提示；有上限后最坏情况是「统计 + 一行说明」——
   * 信息少一点，但用户知道发生了什么（truncated / tooLarge 标记 + UI 提示）。 */
  private get maxDiffBytes(): number {
    return this.opts.maxDiffBytes ?? 8 * 1024 * 1024
  }

  private get maxFileChars(): number {
    return this.opts.maxFileChars ?? 1_000_000
  }

  /** 底层执行：数组参数、不经 shell、统一禁交互与分页。 */
  async run(args: string[], cwd: string, opts: { timeout?: number; input?: string; allowFail?: boolean; env?: Record<string, string> } = {}): Promise<GitResult> {
    const env: Record<string, string> = {
      ...process.env as Record<string, string>,
      GIT_TERMINAL_PROMPT: "0",
      GIT_PAGER: "cat",
      GIT_OPTIONAL_LOCKS: "0",
      ...(this.opts.credentialEnv?.(cwd) ?? {}),
      ...(opts.env ?? {}),
    }
    const proc = Bun.spawn(
      [
        "git",
        "-c",
        "core.quotepath=false",
        "-c",
        "i18n.logOutputEncoding=UTF-8",
        "-c",
        "i18n.commitEncoding=UTF-8",
        "-c",
        "color.ui=false",
        "-c",
        "advice.detachedHead=false",
        "--no-pager",
        ...args,
      ],
      { cwd, env, stdin: opts.input === undefined ? "ignore" : "pipe", stdout: "pipe", stderr: "pipe" },
    )
    let timer: ReturnType<typeof setTimeout> | undefined
    const timeout = opts.timeout ?? CMD_TIMEOUT_MS
    const killed = new Promise<null>((resolve) => {
      timer = setTimeout(() => {
        try {
          proc.kill()
        } catch {
          /* 已退出 */
        }
        resolve(null)
      }, timeout)
    })
    if (opts.input !== undefined) {
      try {
        proc.stdin?.write(opts.input)
        proc.stdin?.end()
      } catch {
        /* 进程已退出 */
      }
    }
    const [stdout, stderr] = await Promise.all([new Response(proc.stdout).arrayBuffer(), new Response(proc.stderr).arrayBuffer()])
    await Promise.race([proc.exited, killed])
    if (timer) clearTimeout(timer)
    const code = proc.exitCode ?? (await proc.exited)
    const result: GitResult = {
      stdout: new TextDecoder().decode(stdout),
      stderr: new TextDecoder().decode(stderr),
      code,
    }
    if (code !== 0 && !opts.allowFail) {
      const msg = (result.stderr || result.stdout).trim() || `git ${args[0]} 失败（exit ${code}）`
      throw new GitError(422, msg)
    }
    return result
  }

  /** 写操作串行化（按仓库根排队；队列内异常不阻断后续）。 */
  private serialize<T>(repo: string, fn: () => Promise<T>): Promise<T> {
    const prev = this.locks.get(repo) ?? Promise.resolve()
    const next = prev.then(fn, fn)
    this.locks.set(
      repo,
      next.catch(() => {
        /* 吞掉，避免污染后续排队 */
      }),
    )
    return next
  }

  /** 仓库根探测（缓存 TTL 内复用；非仓库返回 null）。 */
  async repoRoot(dir: string): Promise<string | null> {
    let st
    try {
      st = statSync(dir)
    } catch {
      return null
    }
    const base = st.isDirectory() ? dir : join(dir, "..")
    const cached = this.repoCache.get(base)
    if (cached && Date.now() - cached.ts < this.repoCacheTtl) return cached.root
    const res = await this.run(["rev-parse", "--show-toplevel"], base, { allowFail: true })
    const root = res.code === 0 && res.stdout.trim() ? res.stdout.trim().replace(/\r?\n$/, "") : null
    this.repoCache.set(base, { root, ts: Date.now() })
    return root
  }

  /** 探测是否仓库（供根清单标记）。 */
  async isRepo(dir: string): Promise<boolean> {
    return (await this.repoRoot(dir)) !== null
  }

  /**
   * 轻量仓库探测：**一次 spawn 拿到仓库根与分支名**（供根清单标记）。
   *
   * 为什么不用 `status()`：`status()` 跑 `--porcelain=v2 --branch -z --untracked-files=all`
   * 并额外一次 `stash list`，大仓库上是百毫秒级开销；而根清单只要「是不是仓库、分支叫什么」。
   * 根清单一次要给几十个根打标记，其中大量根落在同一仓库内（会话工作区就在仓库里），
   * 逐个跑全量 status 会把首屏拖到秒级。
   *
   * 为什么分支名不交给 git 而读 `.git/HEAD`：`--abbrev-ref HEAD` 在**未出生的分支**
   * （刚 `git init`、还没首次提交）上会报 `ambiguous argument 'HEAD'` 并以非 0 退出，
   * 而 `--show-toplevel` 本身是成功的——只看退出码会把「空仓库」错判成「非仓库」，
   * 不看退出码又不行（未出生分支与分离头指针都输出字面量 `HEAD`，无法区分）。
   * HEAD 永远是散文件、内容只有两种形态（`ref: refs/heads/<名>` 或提交 hash），
   * 读它既准确又零额外进程；配 `--git-dir` 一并拿到后，子模块/worktree（`.git` 是文件）
   * 也不用自己解析 `.git` 指向。
   *
   * 语义与 `status()` 对齐：非仓库返回 null；分离头指针时 `branch` 为 undefined 且
   * `detached` 为 true（调用方据此显示 `(detached)`）；未出生分支能拿到分支名。
   */
  async probeRepo(dir: string): Promise<{ root: string; branch?: string; detached: boolean } | null> {
    let st
    try {
      st = statSync(dir)
    } catch {
      return null
    }
    const base = st.isDirectory() ? dir : join(dir, "..")
    const res = await this.run(["rev-parse", "--show-toplevel", "--git-dir"], base, { allowFail: true })
    if (res.code !== 0) {
      this.repoCache.set(base, { root: null, ts: Date.now() })
      return null
    }
    const lines = res.stdout.split("\n").map((s) => s.trim())
    const root = lines[0] ?? ""
    if (!root) {
      this.repoCache.set(base, { root: null, ts: Date.now() })
      return null
    }
    // 顺手回填仓库探测缓存（与 repoRoot 同一份信息）：后续 status()/requireRepo() 对该目录
    // 免再 spawn 一次 rev-parse——根清单是首屏第一请求，之前正是它顺带把这份缓存焐热的。
    this.repoCache.set(base, { root, ts: Date.now() })
    // --git-dir 可能是相对路径（仓库就在 cwd 时输出 ".git"），按 base 归一
    const gitDirRaw = lines[1] ?? ".git"
    const gitDir = isAbsolute(gitDirRaw) ? gitDirRaw : join(base, gitDirRaw || ".git")
    let head = ""
    try {
      head = readFileSync(join(gitDir, "HEAD"), "utf8").trim()
    } catch {
      /* 读不到 HEAD（权限等异常形态）：已确认是仓库，分支留空 */
      return { root, detached: false }
    }
    if (!head.startsWith("ref: ")) return { root, detached: true }
    const ref = head.slice(5).trim()
    const branch = ref.startsWith("refs/heads/") ? ref.slice("refs/heads/".length) : ref
    return branch ? { root, branch, detached: false } : { root, detached: false }
  }

  /** 缓存失效（任何写操作后调用，保证后续读取看到新状态）。 */
  invalidate(dir: string): void {
    this.repoCache.delete(dir)
  }

  /** 仓库快捷断言：返回仓库根，非仓库抛 422（消息面向用户可读）。 */
  async requireRepo(dir: string): Promise<string> {
    const root = await this.repoRoot(dir)
    if (!root) throw new GitError(422, `不是 Git 仓库：${dir}`)
    return root
  }

  /* ------------------------- 只读：状态 / 差异 / 日志 ------------------------- */

  /** 状态（porcelain=v2 -z --branch + untracked=all）：分支信息、变更分组、多步操作态。 */
  async status(dir: string): Promise<GitStatus> {
    const empty: GitStatus = {
      isRepo: false,
      ahead: 0,
      behind: 0,
      detached: false,
      unborn: false,
      changes: [],
      counts: { staged: 0, unstaged: 0, untracked: 0, conflicted: 0 },
      stashCount: 0,
    }
    const root = await this.repoRoot(dir)
    if (!root) return empty
    const res = await this.run(["status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all"], root)
    const tokens = res.stdout.split("\0").filter((t) => t.length > 0)
    const status: GitStatus = { ...empty, isRepo: true, root }
    let branch = ""
    for (let i = 0; i < tokens.length; i++) {
      const t = tokens[i]
      if (t.startsWith("# ")) {
        const [key, ...rest] = t.slice(2).split(" ")
        const val = rest.join(" ")
        if (key === "branch.oid") {
          status.oid = val
          if (val === "(initial)") status.unborn = true
        } else if (key === "branch.head") {
          if (val === "(detached)") status.detached = true
          else branch = val
        } else if (key === "branch.upstream") status.upstream = val
        else if (key === "branch.ab") {
          const m = /\+(-?\d+)\s+-(-?\d+)/.exec(val)
          if (m) {
            status.ahead = Number(m[1])
            status.behind = Number(m[2])
          }
        }
        continue
      }
      if (t.startsWith("1 ")) {
        // 1 XY sub mH mI mW hH hI path
        const parts = t.split(" ")
        const xy = parts[1] ?? ".."
        const path = parts.slice(8).join(" ")
        status.changes.push(makeChange(path, xy))
      } else if (t.startsWith("2 ")) {
        // 2 XY sub mH mI mW hH hI Xscore path<NUL>origPath
        const parts = t.split(" ")
        const xy = parts[1] ?? ".."
        const path = parts.slice(9).join(" ")
        const orig = tokens[i + 1]
        if (orig !== undefined) i++
        status.changes.push(makeChange(path, xy, { origPath: orig, renamed: true }))
      } else if (t.startsWith("u ")) {
        const parts = t.split(" ")
        const xy = parts[1] ?? "UU"
        const path = parts.slice(10).join(" ")
        status.changes.push(makeChange(path, xy, { conflicted: true }))
      } else if (t.startsWith("? ")) {
        status.changes.push(makeChange(t.slice(2), "??", { untracked: true }))
      }
    }
    if (branch) status.branch = branch
    status.changes.sort((a, b) => a.path.localeCompare(b.path))
    status.counts = {
      staged: status.changes.filter((c) => c.staged).length,
      unstaged: status.changes.filter((c) => c.unstaged).length,
      untracked: status.changes.filter((c) => c.untracked).length,
      conflicted: status.changes.filter((c) => c.conflicted).length,
    }
    status.stashCount = (await this.stashList(root)).length
    status.operation = detectOperation(root)
    return status
  }

  /** 单文件/整树差异（结构化 hunks，前端交 Monaco diff 渲染）。端点语义见 diffArgs。 */
  /**
   * 差异端点解析（DESIGN「文件工作台·Git 图形化」：**任意两个提交之间、提交与工作树之间**）。
   * 端点语法：
   *  - `WORKTREE`（缺省右侧）→ 工作区文件；`INDEX` → 暂存区；
   *  - 其余字符串按 git rev 解析（分支名/标签/短 hash/`HEAD~2`/`main@{1}`/`A^`…）。
   * 组合语义：
   *  | from | to | 实际命令 | 含义 |
   *  |---|---|---|---|
   *  | 空/WORKTREE | 空/WORKTREE | `git diff` | 暂存区 vs 工作区（未暂存改动） |
   *  | 空 | INDEX | `git diff --cached` | HEAD vs 暂存区（已暂存改动） |
   *  | REF | 空/WORKTREE | `git diff REF` | 提交 vs 工作树（**常查：这个提交之后我又改了什么**） |
   *  | REF | INDEX | `git diff --cached REF` | 提交 vs 暂存区 |
   *  | REF1 | REF2 | `git diff REF1 REF2` | 任意两提交/引用之间 |
   *  | REF1 | REF2 + mergeBase | `git diff REF1...REF2` | 两分支从共同祖先起的变化（分支对比的默认正确语义） |
   *
   * 端点里可能出现的 `<rev>^`（提交详情的 A 侧）先经 normalizeRev 归一（根提交 → 空树）。
   */

  /**
   * 端点归一：`<rev>^` 里的 `<rev>` 若为**根提交**（无父提交），换成 git 空树对象。
   *
   * 为什么需要：根提交的 `^` 在 git 里直接报错（`ambiguous argument '<rev>^'`，422），
   * 而前端的提交详情把 A 侧与文件清单都挂在 `${hash}^` 上——不归一的话
   * ① 跨文件导航静默消失（compare 422 被 `prepareReview` 的 catch 吃掉）；
   * ② 比较视图选「某提交 ↔ 另一提交」时根提交一侧也会报错。
   * 归一是**语义等价**的：根提交没有父提交，它相对空树的变化就是它的全部内容。
   *
   * 其他情况原样返回（含 `<rev>` 本身不合法——那该由 git 报错，不能被这里静默改成空树）。
   */
  private async normalizeRev(root: string, rev: string | undefined): Promise<string | undefined> {
    const s = String(rev ?? "").trim()
    const m = /^(.+)\^$/.exec(s)
    if (!m) return rev
    const hasParent = await this.run(["rev-parse", "--verify", "--quiet", s], root, { allowFail: true })
    if (hasParent.code === 0 && hasParent.stdout.trim()) return rev
    const base = m[1] ?? ""
    const isCommit = await this.run(["rev-parse", "--verify", "--quiet", `${base}^{commit}`], root, { allowFail: true })
    if (isCommit.code !== 0 || !isCommit.stdout.trim()) return rev
    // `rev-list --parents -n 1 <rev>` 对根提交只输出自身一个 token，非根提交带父 hash
    const parents = await this.run(["rev-list", "--parents", "-n", "1", base], root, { allowFail: true })
    return parents.code === 0 && parents.stdout.trim().split(/\s+/).length <= 1 ? EMPTY_TREE : rev
  }

  /** 成对归一（空值原样透传）。 */
  private async normalizePair(root: string, opts: { from?: string; to?: string }): Promise<{ from?: string; to?: string }> {
    const from = await this.normalizeRev(root, opts.from)
    const to = await this.normalizeRev(root, opts.to)
    return { ...opts, from, to }
  }

  /**
   * diff 文本裁剪：超上限时**在文件边界处**截断（不在 hunk 中间剪一刀，
   * 以免解析出半截 hunk 被错当真实内容），并告知调用方已截断。
   */
  private trimDiffText(text: string): { text: string; truncated: boolean } {
    if (text.length <= this.maxDiffBytes) return { text, truncated: false }
    const cut = text.lastIndexOf("\ndiff --git ", this.maxDiffBytes)
    return { text: cut > 0 ? text.slice(0, cut) : text.slice(0, this.maxDiffBytes), truncated: true }
  }

  /**
   * 逐行内容限制：单文件 hunks 总字符超上限时丢掉 hunks、只留统计，并标 truncated。
   * 文件列表与 +N/-N 不受影响（那才是「有哪些文件改了」的骨架信息）。
   */
  private capHunks(files: GitFileDiff[]): GitFileDiff[] {
    return files.map((f) => {
      if (f.truncated) return f
      let chars = 0
      for (const h of f.hunks) {
        chars += h.header.length
        for (const l of h.lines) chars += l.text.length + 8
        if (chars > this.maxFileChars) break
      }
      return chars > this.maxFileChars ? { ...f, hunks: [], truncated: true } : f
    })
  }

  /**
   * 用 `--name-status` 补齐「没有逐行内容」的文件条目（纯重命名/纯模式变更，
   * 以及被 trimDiffText 截掉的那部分）——保证**文件清单完整**，只是部分条目没 hunks。
   *
   * 被截掉的文件光有路径不够：UI 每行都显示 +N/-N，不给计数就会显示成「+0/-0」——
   * 那看起来像「这个文件没改」，比没这行还糟。所以再问一次 `--numstat` 把真实计数补上
   *（只在确实需要补齐时オ发这一趟，正常提交不付这个代价）。
   */
  private async mergeNameStatus(root: string, args: string[], files: GitFileDiff[], opts: { truncated?: boolean } = {}): Promise<void> {
    const base = args.filter((a) => a !== "--stat" && a !== "--numstat")
    const nameRes = await this.run([...base, "--name-status", "-z"], root, { allowFail: true })
    if (nameRes.code !== 0 || !nameRes.stdout.includes("\0")) return
    const tokens = nameRes.stdout.split("\0").filter(Boolean)
    const pending: Array<{ path: string; oldPath?: string; status: GitFileDiff["status"] }> = []
    for (let i = 0; i < tokens.length; ) {
      const statusCode = tokens[i++]
      const p1 = tokens[i++]
      if (!statusCode || p1 === undefined) break
      const code = statusCode[0]
      let path = p1
      let oldPath: string | undefined
      if (code === "R" || code === "C") {
        oldPath = p1
        path = tokens[i++] ?? p1
      }
      const exist = files.find((f) => f.path === path)
      if (exist) {
        if (!exist.status) exist.status = code === "A" ? "added" : code === "D" ? "deleted" : code === "R" ? "renamed" : code === "M" ? "modified" : undefined
        if (oldPath && !exist.oldPath) exist.oldPath = oldPath
      } else {
        pending.push({ path, oldPath, status: code === "A" ? "added" : code === "D" ? "deleted" : code === "R" ? "renamed" : code === "C" ? "copied" : code === "T" ? "typechange" : "modified" })
      }
    }
    if (!pending.length) return
    const stats = await this.numstat(root, base)
    for (const p of pending) {
      const st = stats.get(p.path)
      files.push({
        path: p.path,
        oldPath: p.oldPath,
        binary: false,
        hunks: [],
        additions: st?.add ?? 0,
        deletions: st?.del ?? 0,
        status: p.status,
        /*
         * 只有「整次 diff 被截断」时才标 truncated：那时没有 hunks 的原因是**逐行内容被上限截掉了**；
         * 而正常目录下的 name-status 补齐（纯重命名/纯改权限）是真的没有内容差异，
         * 标成 truncated 会显示成「改动过大」——把「换了个名字」误报成「大到看不了」。
         */
        ...(opts.truncated ? { truncated: true } : {}),
      })
    }
  }

  /** `--numstat -z` → 路径 → 增/删行数（二进制文件的两列是 `-`，按 0 计）。 */
  private async numstat(root: string, base: string[]): Promise<Map<string, { add: number; del: number }>> {
    const out = new Map<string, { add: number; del: number }>()
    const res = await this.run([...base, "--numstat", "-z"], root, { allowFail: true })
    if (res.code !== 0) return out
    const toks = res.stdout.split("\0").filter((t) => t !== "")
    for (let i = 0; i < toks.length; ) {
      const m = /^(\d+|-)\t(\d+|-)\t(.*)$/.exec(toks[i++] ?? "")
      if (!m) break // 遇到 diff 正文即停
      let path = m[3] ?? ""
      if (path === "") {
        i++ // 重命名：紧跟 old、new 两个 token
        path = toks[i++] ?? ""
      }
      out.set(path, { add: m[1] === "-" ? 0 : Number(m[1]), del: m[2] === "-" ? 0 : Number(m[2]) })
    }
    return out
  }
  private diffArgs(opts: { from?: string; to?: string; staged?: boolean; mergeBase?: boolean; path?: string; context?: number; ignoreWhitespace?: boolean; stat?: boolean; nameOnly?: boolean }): string[] {
    const norm = (v: string | undefined): string => {
      const s = String(v ?? "").trim()
      if (!s) return ""
      const u = s.toUpperCase()
      if (u === "WORKTREE" || u === ".") return "WORKTREE"
      if (u === "INDEX" || u === "STAGED" || u === "--cached") return "INDEX"
      return s
    }
    const from = norm(opts.from)
    const to = norm(opts.to)
    const args = ["diff", "--no-color", "--no-ext-diff", "--find-renames", `-U${Math.max(0, Math.min(opts.context ?? 3, 20))}`]
    if (opts.ignoreWhitespace) args.push("-w")
    if (opts.stat) args.push("--stat")
    if (opts.nameOnly) args.push("--name-status", "-z")
    const isWork = (v: string) => v === "" || v === "WORKTREE"
    const isIndex = (v: string) => v === "INDEX"
    if (isWork(from) && isWork(to) && !opts.staged) {
      // 空 → git diff（索引 vs 工作区）
    } else if ((isWork(from) && isIndex(to)) || (isIndex(from) && isWork(to)) || (isWork(from) && isWork(to) && opts.staged)) {
      args.push("--cached")
    } else if (isIndex(from) && !to) {
      args.push("--cached")
    } else if (isWork(from) && !isIndex(to) && to) {
      // 空 → REF：REF 与工作区
      args.push(to)
    } else if (isIndex(from) && !isWork(to) && to) {
      args.push("--cached", to)
    } else if (!isWork(from) && !isIndex(from) && isWork(to)) {
      // REF → 空：REF 与工作区
      args.push(from)
    } else if (!isWork(from) && !isIndex(from) && isIndex(to)) {
      args.push("--cached", from)
    } else if (!isWork(from) && !isWork(to) && !isIndex(from) && !isIndex(to)) {
      args.push(opts.mergeBase ? `${from}...${to}` : from, ...(opts.mergeBase ? [] : [to]))
    } else {
      throw new GitError(400, `不支持的差异端点组合：${from || "工作区"} → ${to || "工作区"}`)
    }
    if (opts.path) args.push("--", opts.path)
    return args
  }

  /**
   * 差异（结构化 hunks + 原始文本）。覆盖工作区/暂存区/任意两个 rev 的全部组合（见 diffArgs 语义表）。
   */
  async diff(
    dir: string,
    opts: { path?: string; staged?: boolean; from?: string; to?: string; mergeBase?: boolean; context?: number; ignoreWhitespace?: boolean } = {},
  ): Promise<{ files: GitFileDiff[]; raw: string; truncated: boolean }> {
    const root = await this.requireRepo(dir)
    // 端点归一（根提交的 `<hash>^` → 空树）：见 normalizeRev
    const ep = await this.normalizePair(root, opts)
    const args = this.diffArgs({ ...opts, ...ep })
    const res = await this.run(args, root, { allowFail: true })
    if (res.code !== 0) throw new GitError(422, (res.stderr || "git diff 失败").trim())
    const t = this.trimDiffText(res.stdout)
    const files = this.capHunks(parseUnifiedDiff(t.text))
    // 重命名/仅内容不变（纯 mode 变更）时 parseUnifiedDiff 可能产出空 hunks 的文件条目：保留
    return { files, raw: t.text, truncated: t.truncated }
  }

  /**
   * 差异统计摘要（对齐 `git diff --stat` 的形状：每文件一行 + 末尾汇总）。
   *
   * 为什么自己拼：`--stat` 是**又一次完整内容 diff**（含重命名探测），而我们只要一行摘要
   * 加每文件的增删数——后者已经在 files 里算好了（调用方为 hunk 解析付过代价了）。
   * 文件多时只列前 200 行（与 `--stat` 截断显示同一个意思），避免响应体白胖。
   */
  private summarize(files: GitFileDiff[]): string {
    if (!files.length) return ""
    const lines = files.slice(0, 200).map((f) => {
      const total = f.additions + f.deletions
      const bar = `${"+".repeat(Math.min(f.additions, 40))}${"-".repeat(Math.min(f.deletions, 40))}`
      return ` ${f.path} | ${total} ${bar}`.trimEnd()
    })
    if (files.length > 200) lines.push(` …（另有 ${files.length - 200} 个文件）`)
    const ins = files.reduce((n, f) => n + f.additions, 0)
    const del = files.reduce((n, f) => n + f.deletions, 0)
    const parts = [`${files.length} file${files.length === 1 ? "" : "s"} changed`]
    if (ins) parts.push(`${ins} insertion${ins === 1 ? "" : "s"}(+)`)
    if (del) parts.push(`${del} deletion${del === 1 ? "" : "s"}(-)`)
    lines.push(` ${parts.join(", ")}`)
    return lines.join("\n")
  }

  /**
   * 任意两端对比总览：文件清单（含状态/增删行数/重命名前后路径）+ 统计。
   * 前端「比较」视图的数据源（A 端 / B 端可分别选提交、分支、标签、工作区、暂存区）。
   */
  async compare(
    dir: string,
    opts: { from?: string; to?: string; mergeBase?: boolean; path?: string; context?: number; ignoreWhitespace?: boolean } = {},
  ): Promise<{ files: GitFileDiff[]; additions: number; deletions: number; from: string; to: string; stats: string; mergeBaseOf?: string; truncated?: boolean }> {
    const root = await this.requireRepo(dir)
    // 端点归一（根提交的 `<hash>^` → 空树）：见 normalizeRev
    const ep = await this.normalizePair(root, opts)
    // 三点（分支对比）：显式解析共同祖先，UI 上显示「基准 = merge-base(A,B)」，避免用户误以为在比 A..B
    let mergeBaseOf: string | undefined
    if (opts.mergeBase && ep.from && ep.to) {
      if (ep.from === EMPTY_TREE || ep.to === EMPTY_TREE) {
        // 根提交参与的比较：它没有祖先，「共同祖先」就是空树（全量新增），不必也无法交给 merge-base
        mergeBaseOf = EMPTY_TREE
      } else {
        const mb = await this.run(["merge-base", ep.from, ep.to], root, { allowFail: true })
        if (mb.code === 0 && mb.stdout.trim()) {
          mergeBaseOf = mb.stdout.trim()
        } else {
          throw new GitError(422, `无法计算共同祖先（${ep.from} / ${ep.to}）：${(mb.stderr || "无共同祖先").trim()}`)
        }
      }
    }
    /*
     * 根提交参与比较时**不能再走三点（`A...B`）**：git 的对称差需要两个 commit，
     * 而 A 侧已经是空树（tree），会报 `object <hash> is a tree, not a commit`。
     * 语义上空树**就是**共同祖先，直接用普通两点即可（拿到的正是「全量新增」）。
     */
    const usesEmptyTree = ep.from === EMPTY_TREE || ep.to === EMPTY_TREE
    const args = this.diffArgs({ ...opts, ...ep, mergeBase: opts.mergeBase && !usesEmptyTree })
    const res = await this.run(args, root, { allowFail: true })
    if (res.code !== 0) throw new GitError(422, (res.stderr || "git diff 失败").trim())
    const t = this.trimDiffText(res.stdout)
    const files = this.capHunks(parseUnifiedDiff(t.text))
    // 补全「无内容差异但状态变化」（纯重命名/纯模式变更），以及被体量上限截掉的那部分文件：
    // --name-status 的代价很小，却能把**文件清单的完整性**与逐行内容分开——
    // 用户至少知道「这次提交一共动了哪些文件」。
    await this.mergeNameStatus(root, args, files)
    const additions = files.reduce((n, f) => n + f.additions, 0)
    const deletions = files.reduce((n, f) => n + f.deletions, 0)
    // stats 由已算好的 files 汇总（不再跑 `git diff --stat`：那又是一次完整内容 diff，
    // 只为拿一行摘要——见 summarize）
    return { files, additions, deletions, from: opts.from ?? "WORKTREE", to: opts.to ?? "WORKTREE", stats: this.summarize(files), mergeBaseOf, truncated: t.truncated }
  }

  /** 单文件差异（任意两端组合：工作区/暂存区/两 rev）。 */
  async fileDiff(
    dir: string,
    path: string,
    opts: { staged?: boolean; from?: string; to?: string; mergeBase?: boolean; context?: number; ignoreWhitespace?: boolean } = {},
  ): Promise<GitFileDiff> {
    const root = await this.requireRepo(dir)
    // 端点归一（根提交的 `<hash>^` → 空树）：见 normalizeRev
    const ep = await this.normalizePair(root, opts)
    const args = this.diffArgs({ ...opts, ...ep, path })
    const res = await this.run(args, root, { allowFail: true })
    if (res.code !== 0) throw new GitError(422, (res.stderr || "git diff 失败").trim())
    const t = this.trimDiffText(res.stdout)
    const files = this.capHunks(parseUnifiedDiff(t.text))
    if (files.length) return files[0]
    // 无内容差异时给出状态（重命名/删除/新增）
    const nameRes = await this.run([...args.filter((a) => a !== "--name-status" && a !== "-z"), "--name-status"], root, { allowFail: true })
    const line = nameRes.stdout.trim().split("\n").pop() ?? ""
    const m = /^([A-Z])\d*\t(.*)$/.exec(line)
    const statusMap: Record<string, GitFileDiff["status"]> = { A: "added", D: "deleted", R: "renamed", C: "copied", M: "modified", T: "typechange" }
    return { path, binary: false, hunks: [], additions: 0, deletions: 0, status: m ? statusMap[m[1]] : undefined, truncated: t.truncated || undefined }
  }

  /**
   * 引用清单（比较视图的端点选择器）：本地/远程分支、标签、HEAD 与最近提交，附相对时间与主题。
   * 单次调用拿齐，避免前端为选择器打多个请求。
   */
  async refs(dir: string, opts: { recent?: number } = {}): Promise<{
    head: { branch?: string; hash?: string; detached: boolean; unborn: boolean }
    branches: GitBranch[]
    tags: GitTag[]
    recent: Array<{ hash: string; short: string; subject: string; author: string; time: number; refs: string[] }>
  }> {
    const root = await this.requireRepo(dir)
    const [status, branches, tags, log] = await Promise.all([this.status(root), this.branches(root), this.tags(root), this.log(root, { limit: Math.max(1, Math.min(opts.recent ?? 40, 200)), all: true })])
    return {
      head: { branch: status.branch, hash: status.oid, detached: status.detached, unborn: status.unborn },
      branches,
      tags,
      recent: log.commits.map((c) => ({ hash: c.hash, short: c.short, subject: c.subject, author: c.author, time: c.commitTime, refs: c.refs })),
    }
  }

  /**
   * 某端点下的文件内容（并列 diff 的取数口）：端点语法同 diffArgs（WORKTREE / INDEX / rev）。
   * - WORKTREE：读磁盘（相对仓库根）；
   * - INDEX：`git show :0:path`（冲突时为 `:2:` 我方）；
   * - rev：`git show rev:path`。
   * 返回 missing 便于前端把「新增/删除」侧渲染成空文档而非报错；
   * 超体量上限时返回 `tooLarge: true` + `size`，**不拉正文**——
   * 两侧内容是要进 Monaco model 的，几 MB 的字符串建 model + 算差异会把主线程锁死。
   */
  async contentAt(dir: string, ref: string, path: string): Promise<{ content: string; binary: boolean; missing: boolean; size: number; ref: string; tooLarge?: boolean }> {
    const root = await this.requireRepo(dir)
    const norm = String(ref ?? "").trim()
    const up = norm.toUpperCase()
    const rel = path.replace(/\\/g, "/").replace(/^\/+/, "")
    if (!rel) throw new GitError(400, "缺少 path")
    if (!norm || up === "WORKTREE" || up === ".") {
      const abs = join(root, ...rel.split("/"))
      if (!existsSync(abs)) return { content: "", binary: false, missing: true, size: 0, ref: "WORKTREE" }
      const st = statSync(abs)
      if (st.isDirectory()) return { content: "", binary: false, missing: true, size: 0, ref: "WORKTREE" }
      // 先看大小再读盘：上限之外的文件连读都不读（读一个几十 MB 的文件只为了发现它太大，很亏）
      if (st.size > this.maxFileChars) return { content: "", binary: false, missing: false, size: st.size, ref: "WORKTREE", tooLarge: true }
      const buf = new Uint8Array(await Bun.file(abs).arrayBuffer())
      const binary = buf.subarray(0, 8192).includes(0)
      if (binary) return { content: "", binary: true, missing: false, size: buf.length, ref: "WORKTREE" }
      const { decodeBuffer } = await import("../fs/service")
      return { content: decodeBuffer(buf).text, binary: false, missing: false, size: buf.length, ref: "WORKTREE" }
    }
    if (up === "INDEX" || up === "STAGED") {
      const big = await this.oversizedBlob(root, `:0:${rel}`)
      if (big) return { content: "", binary: false, missing: false, size: big, ref: "INDEX", tooLarge: true }
      const res = await this.run(["show", `:0:${rel}`], root, { allowFail: true })
      if (res.code !== 0) {
        const ours = await this.run(["show", `:2:${rel}`], root, { allowFail: true })
        if (ours.code !== 0) return { content: "", binary: false, missing: true, size: 0, ref: "INDEX" }
        return { content: ours.stdout, binary: false, missing: false, size: ours.stdout.length, ref: "INDEX" }
      }
      return { content: res.stdout, binary: false, missing: false, size: res.stdout.length, ref: "INDEX" }
    }
    const big = await this.oversizedBlob(root, `${norm}:${rel}`)
    if (big) return { content: "", binary: false, missing: false, size: big, ref: norm, tooLarge: true }
    const res = await this.run(["show", `${norm}:${rel}`], root, { allowFail: true })
    if (res.code !== 0) return { content: "", binary: false, missing: true, size: 0, ref: norm }
    return { content: res.stdout, binary: false, missing: false, size: res.stdout.length, ref: norm }
  }

  /**
   * 先问 git「这个对象多大」而不是直接 `show`：`cat-file -s` 只读元数据，
   * 几十 MB 的 blob 不会被拉进进程。返回 > 0 表示超限（即大小），否则 0。
   * 对象不存在时也返回 0——让后面的 `show` 正常走 missing 分支，保持原有语义。
   */
  private async oversizedBlob(root: string, spec: string): Promise<number> {
    const r = await this.run(["cat-file", "-s", spec], root, { allowFail: true })
    if (r.code !== 0) return 0
    const size = Number(r.stdout.trim())
    return Number.isFinite(size) && size > this.maxFileChars ? size : 0
  }

  /** 提交日志（分页 + 路径/作者/关键字过滤；parents 供前端画泳道图）。 */
  async log(
    dir: string,
    opts: { limit?: number; skip?: number; path?: string; ref?: string; all?: boolean; since?: string; until?: string; author?: string; grep?: string; firstParent?: boolean } = {},
  ): Promise<{ commits: GitCommit[]; hasMore: boolean }> {
    const root = await this.requireRepo(dir)
    const limit = Math.max(1, Math.min(opts.limit ?? 50, 500))
    const args = [
      "log",
      "--date-order",
      `--max-count=${limit}`,
      "--pretty=format:" + ["%H", "%P", "%an", "%ae", "%at", "%cn", "%ce", "%ct", "%D", "%s", "%b"].join(F) + R,
    ]
    if (opts.skip) args.push(`--skip=${opts.skip}`)
    if (opts.all) args.push("--all")
    if (opts.firstParent) args.push("--first-parent")
    if (opts.since) args.push(`--since=${opts.since}`)
    if (opts.until) args.push(`--until=${opts.until}`)
    if (opts.author) args.push(`--author=${opts.author}`)
    if (opts.grep) args.push(`--grep=${opts.grep}`)
    if (opts.ref) args.push(opts.ref)
    if (opts.path) args.push("--", opts.path)
    const res = await this.run(args, root, { allowFail: true })
    if (res.code !== 0) throw new GitError(422, (res.stderr || "git log 失败").trim())
    const commits = res.stdout
      .split(R)
      .map((rec) => rec.replace(/^\n/, ""))
      .filter((rec) => rec.includes(F))
      .map((rec) => {
        const f = rec.split(F)
        return {
          hash: f[0] ?? "",
          short: (f[0] ?? "").slice(0, 8),
          parents: (f[1] ?? "").split(" ").filter(Boolean),
          author: f[2] ?? "",
          authorEmail: f[3] ?? "",
          authorTime: Number(f[4] ?? 0) * 1000,
          committer: f[5] ?? "",
          committerEmail: f[6] ?? "",
          commitTime: Number(f[7] ?? 0) * 1000,
          refs: (f[8] ?? "")
            .split(",")
            .map((s) => s.trim())
            .filter(Boolean),
          subject: f[9] ?? "",
          body: (f[10] ?? "").trim() || undefined,
        } satisfies GitCommit
      })
    return { commits, hasMore: commits.length >= limit }
  }

  /** 单提交详情（元信息 + 变更文件 + 统计）。 */
  async commitDetail(dir: string, hash: string): Promise<{ commit: GitCommit; files: GitFileDiff[]; stats: string; truncated?: boolean }> {
    const root = await this.requireRepo(dir)
    const meta = await this.log(root, { limit: 1, ref: hash })
    const commit = meta.commits[0]
    if (!commit) throw new GitError(404, `提交不存在: ${hash}`)
    const args = ["show", "--no-color", "--no-ext-diff", "-U0", "--format=", hash]
    const res = await this.run(args, root, { allowFail: true })
    const t = this.trimDiffText(res.stdout)
    const files = this.capHunks(parseUnifiedDiff(t.text))
    // 体量上限截掉了后面的文件时，用 --name-status 把清单补齐（提交详情的重心是「动了哪些文件」）
    if (t.truncated) await this.mergeNameStatus(root, args, files, { truncated: true })
    const stat = await this.run(["show", "--no-color", "--stat", "--format=", hash], root, { allowFail: true })
    return { commit, files, stats: stat.stdout.trim(), truncated: t.truncated }
  }

  /** 单提交对某文件的差异。 */
  async commitFileDiff(dir: string, hash: string, path: string, context = 3): Promise<GitFileDiff> {
    const root = await this.requireRepo(dir)
    const res = await this.run(["show", "--no-color", "--no-ext-diff", `-U${context}`, "--format=", hash, "--", path], root, { allowFail: true })
    const t = this.trimDiffText(res.stdout)
    const files = this.capHunks(parseUnifiedDiff(t.text))
    return files[0] ?? { path, binary: false, hunks: [], additions: 0, deletions: 0, truncated: t.truncated || undefined }
  }

  /** 某文件的完整历史（跨重命名）。 */
  async fileHistory(dir: string, path: string, limit = 50): Promise<GitCommit[]> {
    const root = await this.requireRepo(dir)
    const res = await this.run(
      ["log", "--follow", `--max-count=${limit}`, "--pretty=format:" + ["%H", "%P", "%an", "%ae", "%at", "%cn", "%ce", "%ct", "%D", "%s", "%b"].join(F) + R, "--", path],
      root,
      { allowFail: true },
    )
    if (res.code !== 0) return []
    return res.stdout
      .split(R)
      .map((rec) => rec.replace(/^\n/, ""))
      .filter((rec) => rec.includes(F))
      .map((rec) => {
        const f = rec.split(F)
        return {
          hash: f[0] ?? "",
          short: (f[0] ?? "").slice(0, 8),
          parents: (f[1] ?? "").split(" ").filter(Boolean),
          author: f[2] ?? "",
          authorEmail: f[3] ?? "",
          authorTime: Number(f[4] ?? 0) * 1000,
          committer: f[5] ?? "",
          committerEmail: f[6] ?? "",
          commitTime: Number(f[7] ?? 0) * 1000,
          refs: (f[8] ?? "").split(",").map((s) => s.trim()).filter(Boolean),
          subject: f[9] ?? "",
          body: (f[10] ?? "").trim() || undefined,
        } satisfies GitCommit
      })
  }

  /**
   * 分支清单（本地 + 远程；跟踪分支附 ahead/behind）。
   *
   * ahead/behind 用 `%(upstream:track)` **一次带出来**：早期是筛出跟踪分支后逐个
   * `git rev-list --left-right --count`（上限 50 个 = 最多 50 次进程启动；Windows 上单次
   * 约 65ms，大型仓库点开 Git 面板就是秒级等待）。现在零额外进程。
   * 极老版本 git（< 2.13）不认识 `:track` 字段时会整条 for-each-ref 失败，退回旧的逐分支计数路径。
   */
  async branches(dir: string): Promise<GitBranch[]> {
    const root = await this.requireRepo(dir)
    const fields = ["%(refname:short)", "%(objectname)", "%(upstream:short)"]
    const fmt = [...fields, "%(upstream:track)", "%(HEAD)", "%(committerdate:unix)", "%(contents:subject)"].join(F)
    const res = await this.run(["for-each-ref", `--format=${fmt}`, "refs/heads", "refs/remotes"], root, { allowFail: true })
    if (res.code !== 0) return this.branchesByRevList(root)
    const out: GitBranch[] = []
    for (const line of res.stdout.split("\n")) {
      if (!line.includes(F)) continue
      const [name, hash, upstream, track, head, time, subject] = line.split(F)
      if (!name || name.endsWith("/HEAD")) continue
      const b: GitBranch = {
        name,
        remote: name.includes("/") && !name.startsWith("refs/"),
        current: head === "*",
        hash: hash ?? "",
        upstream: upstream || undefined,
        time: time ? Number(time) * 1000 : undefined,
        subject: subject || undefined,
      }
      // track 形如 `[ahead 2, behind 1]` / `[ahead 2]` / `[behind 1]` / `[gone]`；同步时为 None
      const ahead = /ahead (\d+)/.exec(track ?? "")
      const behind = /behind (\d+)/.exec(track ?? "")
      if (ahead) b.ahead = Number(ahead[1])
      if (behind) b.behind = Number(behind[1])
      out.push(b)
    }
    out.sort((a, b) => (a.current ? -1 : b.current ? 1 : a.remote === b.remote ? a.name.localeCompare(b.name) : a.remote ? 1 : -1))
    return out
  }

  /** 分支清单的兼容路径（不支持 `%(upstream:track)` 的老 git）：查询后逐分支 rev-list 数 ahead/behind。 */
  private async branchesByRevList(root: string): Promise<GitBranch[]> {
    const fmt = ["%(refname:short)", "%(objectname)", "%(upstream:short)", "%(HEAD)", "%(committerdate:unix)", "%(contents:subject)"].join(F)
    const res = await this.run(["for-each-ref", `--format=${fmt}`, "refs/heads", "refs/remotes"], root, { allowFail: true })
    const out: GitBranch[] = []
    for (const line of res.stdout.split("\n")) {
      if (!line.includes(F)) continue
      const [name, hash, upstream, head, time, subject] = line.split(F)
      if (!name || name.endsWith("/HEAD")) continue
      out.push({
        name,
        remote: name.includes("/") && !name.startsWith("refs/"),
        current: head === "*",
        hash: hash ?? "",
        upstream: upstream || undefined,
        time: time ? Number(time) * 1000 : undefined,
        subject: subject || undefined,
      })
    }
    // ahead/behind：仅对跟踪分支计算（上限 50 个，避开大仓库慢）
    for (const b of out.filter((x) => x.upstream).slice(0, 50)) {
      const r = await this.run(["rev-list", "--left-right", "--count", `${b.name}...${b.upstream}`], root, { allowFail: true })
      const m = /^(\d+)\s+(\d+)/.exec(r.stdout.trim())
      if (m) {
        b.ahead = Number(m[1])
        b.behind = Number(m[2])
      }
    }
    out.sort((a, b) => (a.current ? -1 : b.current ? 1 : a.remote === b.remote ? a.name.localeCompare(b.name) : a.remote ? 1 : -1))
    return out
  }

  async tags(dir: string): Promise<GitTag[]> {
    const root = await this.requireRepo(dir)
    const fmt = ["%(refname:short)", "%(objectname)", "%(creatordate:unix)", "%(contents:subject)"].join(F)
    const res = await this.run(["for-each-ref", `--format=${fmt}`, "refs/tags"], root, { allowFail: true })
    return res.stdout
      .split("\n")
      .filter((l) => l.includes(F))
      .map((l) => {
        const [name, hash, time, subject] = l.split(F)
        return { name: name ?? "", hash: hash ?? "", time: time ? Number(time) * 1000 : undefined, subject: subject || undefined }
      })
  }

  async remotes(dir: string): Promise<GitRemote[]> {
    const root = await this.requireRepo(dir)
    const res = await this.run(["remote", "-v"], root, { allowFail: true })
    const map = new Map<string, GitRemote>()
    for (const line of res.stdout.split("\n")) {
      const m = /^(\S+)\s+(\S+)\s+\((fetch|push)\)$/.exec(line.trim())
      if (!m) continue
      const cur = map.get(m[1]) ?? { name: m[1], fetchUrl: "", pushUrl: "" }
      if (m[3] === "fetch") cur.fetchUrl = m[2]
      else cur.pushUrl = m[2]
      map.set(m[1], cur)
    }
    return [...map.values()].map((r) => ({ ...r, pushUrl: r.pushUrl || r.fetchUrl }))
  }

  /** 逐行 blame（前端左侧 blame 栏 + 悬浮提交信息）。 */
  async blame(dir: string, path: string, ref?: string): Promise<GitBlameLine[]> {
    const root = await this.requireRepo(dir)
    const args = ["blame", "--line-porcelain"]
    if (ref) args.push(ref)
    args.push("--", path)
    const res = await this.run(args, root, { allowFail: true })
    if (res.code !== 0) throw new GitError(422, (res.stderr || "git blame 失败").trim())
    const out: GitBlameLine[] = []
    let cur: Partial<GitBlameLine> | null = null
    let lineNo = 0
    for (const raw of res.stdout.split("\n")) {
      const m = /^([0-9a-f]{7,40})\s+\d+\s+(\d+)(?:\s+(\d+))?$/.exec(raw)
      if (m) {
        if (cur && cur.hash) out.push(cur as GitBlameLine)
        lineNo = Number(m[2])
        cur = { line: lineNo, hash: m[1], author: "", authorEmail: "", time: 0, summary: "", uncommitted: /^0{40}$/.test(m[1]) }
        continue
      }
      if (!cur) continue
      if (raw.startsWith("author ")) cur.author = raw.slice(7)
      else if (raw.startsWith("author-mail ")) cur.authorEmail = raw.slice(12).replace(/[<>]/g, "")
      else if (raw.startsWith("author-time ")) cur.time = Number(raw.slice(12)) * 1000
      else if (raw.startsWith("summary ")) cur.summary = raw.slice(8)
    }
    if (cur && cur.hash) out.push(cur as GitBlameLine)
    return out
  }

  /** 读取仓库内某路径在指定 ref 下的内容（`:0:path` 索引版、`HEAD:path`、`<hash>^:path` 等）。
   *  用途：前端拿「旧版/新版」两侧真实文本，交给 Monaco 差异编辑器做带语法高亮的并列 diff。 */
  async showFile(dir: string, ref: string, path: string): Promise<{ content: string; binary: boolean; missing: boolean; size: number }> {
    const root = await this.requireRepo(dir)
    const res = await this.run(["show", `${ref}:${path}`], root, { allowFail: true })
    if (res.code !== 0) return { content: "", binary: false, missing: true, size: 0 }
    const buf = new TextEncoder().encode(res.stdout)
    const binary = buf.subarray(0, 8192).includes(0)
    return { content: res.stdout, binary, missing: false, size: buf.length }
  }

  async stashList(dir: string): Promise<Array<{ index: number; ref: string; message: string; time?: number }>> {
    const root = await this.requireRepo(dir)
    const res = await this.run(["stash", "list", `--format=%gd${F}%s${F}%ct`], root, { allowFail: true })
    return res.stdout
      .split("\n")
      .filter((l) => l.includes(F))
      .map((l) => {
        const [ref, message, time] = l.split(F)
        const idx = Number(/stash@\{(\d+)\}/.exec(ref ?? "")?.[1] ?? 0)
        return { index: idx, ref: ref ?? "", message: message ?? "", time: time ? Number(time) * 1000 : undefined }
      })
  }

  /** 冲突三方内容（前端冲突解决器数据源）：base/ours/theirs + 当前工作区（含冲突标记）。 */
  async conflicts(dir: string, paths?: string[]): Promise<Array<{ path: string; base: string | null; ours: string | null; theirs: string | null; current: string | null }>> {
    const root = await this.requireRepo(dir)
    const status = await this.status(root)
    const targets = (paths && paths.length ? status.changes.filter((c) => paths.includes(c.path)) : status.changes.filter((c) => c.conflicted)).map((c) => c.path)
    const out: Array<{ path: string; base: string | null; ours: string | null; theirs: string | null; current: string | null }> = []
    for (const p of targets) {
      out.push({
        path: p,
        base: await this.stageBlob(root, 1, p),
        ours: await this.stageBlob(root, 2, p),
        theirs: await this.stageBlob(root, 3, p),
        current: existsSync(join(root, p)) ? await Bun.file(join(root, p)).text().catch(() => null) : null,
      })
    }
    return out
  }

  private async stageBlob(root: string, stage: 1 | 2 | 3, path: string): Promise<string | null> {
    const res = await this.run(["show", `:${stage}:${path}`], root, { allowFail: true })
    return res.code === 0 ? res.stdout : null
  }

  /* ------------------------- 写操作（串行化 + 开关校验） ------------------------- */

  private assertWrite(): void {
    if (!this.opts.writeEnabled) throw new GitError(403, "Git 写操作已禁用（GEBAI_GIT_WRITE=false）")
  }

  private assertRemote(): void {
    this.assertWrite()
    if (!this.opts.remoteEnabled) throw new GitError(403, "Git 远程操作已禁用（GEBAI_GIT_REMOTE=false）")
  }

  async stage(dir: string, paths: string[]): Promise<void> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    if (!paths.length) throw new GitError(400, "未指定文件")
    await this.serialize(root, () => this.run(["add", "--", ...paths], root))
    this.invalidate(root)
  }

  async unstage(dir: string, paths: string[]): Promise<void> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    if (!paths.length) throw new GitError(400, "未指定文件")
    await this.serialize(root, async () => {
      const res = await this.run(["reset", "-q", "HEAD", "--", ...paths], root, { allowFail: true })
      // 无 HEAD（unborn 分支）：reset HEAD 失败，改用 rm --cached
      if (res.code !== 0) await this.run(["rm", "--cached", "-r", "--quiet", "--", ...paths], root, { allowFail: true })
    })
    this.invalidate(root)
  }

  /** 丢弃工作区改动（可选先自动 stash 备份——破坏性操作兜底）。 */
  async discard(dir: string, paths: string[], opts: { backup?: boolean } = {}): Promise<{ backupRef?: string }> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    return this.serialize(root, async () => {
      let backupRef: string | undefined
      if (opts.backup !== false) {
        const msg = `gebai-backup-${new Date().toISOString().replace(/[:.]/g, "-")}`
        const res = await this.run(["stash", "push", "-u", "-m", msg, "--", ...paths], root, { allowFail: true })
        if (res.code === 0 && !/No local changes/i.test(res.stdout + res.stderr)) backupRef = "stash@{0}"
      }
      // 已跟踪文件恢复；未跟踪文件删除
      const st = await this.run(["status", "--porcelain=v2", "-z", "--", ...paths], root, { allowFail: true })
      const untracked: string[] = []
      for (const t of st.stdout.split("\0")) if (t.startsWith("? ")) untracked.push(t.slice(2))
      const tracked = paths.filter((p) => !untracked.includes(p))
      if (tracked.length) await this.run(["checkout", "--", ...tracked], root, { allowFail: true })
      for (const u of untracked) await this.run(["clean", "-f", "--", u], root, { allowFail: true })
      this.invalidate(root)
      return { backupRef }
    })
  }

  async commit(
    dir: string,
    opts: { message: string; paths?: string[]; amend?: boolean; signoff?: boolean; author?: string; allowEmpty?: boolean },
  ): Promise<{ hash: string; subject: string }> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    const message = String(opts.message ?? "").trim()
    if (!message && !opts.amend) throw new GitError(422, "提交信息不能为空")
    return this.serialize(root, async () => {
      if (opts.paths && opts.paths.length) await this.run(["add", "--", ...opts.paths], root)
      const args = ["commit", "-F", "-"]
      if (opts.amend) args.push("--amend")
      if (opts.signoff) args.push("--signoff")
      if (opts.allowEmpty) args.push("--allow-empty")
      if (opts.author) args.push(`--author=${opts.author}`)
      const res = await this.run(args, root, { input: message ? `${message}\n` : undefined, allowFail: true })
      if (res.code !== 0) throw new GitError(422, (res.stderr || res.stdout || "提交失败").trim())
      this.invalidate(root)
      const hashRes = await this.run(["rev-parse", "HEAD"], root, { allowFail: true })
      const hash = hashRes.stdout.trim()
      const subj = await this.run(["log", "-1", "--pretty=%s"], root, { allowFail: true })
      return { hash, subject: subj.stdout.trim() }
    })
  }

  async branchOp(
    dir: string,
    op: { action: "create" | "checkout" | "delete" | "rename" | "track" | "upstream"; name: string; startPoint?: string; newName?: string; force?: boolean; remote?: boolean },
  ): Promise<void> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    await this.serialize(root, async () => {
      if (op.action === "create") await this.run(["branch", ...(op.force ? ["-f"] : []), op.name, ...(op.startPoint ? [op.startPoint] : [])], root)
      else if (op.action === "checkout") await this.run(["checkout", op.name], root)
      else if (op.action === "rename") await this.run(["branch", "-m", op.name, op.newName ?? ""], root)
      else if (op.action === "track") await this.run(["branch", "--set-upstream-to", op.startPoint ?? "", op.name], root)
      else if (op.action === "upstream") await this.run(["branch", `--set-upstream-to=${op.startPoint ?? ""}`, op.name], root)
      else await this.run(["branch", op.force ? "-D" : "-d", op.name], root)
      this.invalidate(root)
    })
  }

  async checkoutRef(dir: string, ref: string, opts: { detach?: boolean } = {}): Promise<void> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    await this.serialize(root, () => this.run(["checkout", ...(opts.detach ? ["--detach"] : []), ref], root))
    this.invalidate(root)
  }

  async merge(dir: string, ref: string, opts: { noFf?: boolean; squash?: boolean; message?: string } = {}): Promise<{ ok: boolean; conflicts: string[]; output: string }> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    return this.serialize(root, async () => {
      const args = ["merge", "--no-edit"]
      if (opts.noFf) args.push("--no-ff")
      if (opts.squash) args.push("--squash")
      if (opts.message) args.push("-m", opts.message)
      args.push(ref)
      const res = await this.run(args, root, { allowFail: true })
      this.invalidate(root)
      const st = await this.status(root)
      return { ok: res.code === 0, conflicts: st.changes.filter((c) => c.conflicted).map((c) => c.path), output: (res.stdout + res.stderr).trim() }
    })
  }

  async rebase(dir: string, ref: string, opts: { abort?: boolean; continue?: boolean; skip?: boolean } = {}): Promise<{ ok: boolean; conflicts: string[]; output: string }> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    return this.serialize(root, async () => {
      const args = ["rebase"]
      if (opts.abort) args.push("--abort")
      else if (opts.continue) args.push("--continue")
      else if (opts.skip) args.push("--skip")
      else args.push(ref)
      const res = await this.run(args, root, { allowFail: true, env: { GIT_EDITOR: "true" } })
      this.invalidate(root)
      const st = await this.status(root)
      return { ok: res.code === 0, conflicts: st.changes.filter((c) => c.conflicted).map((c) => c.path), output: (res.stdout + res.stderr).trim() }
    })
  }

  async cherryPick(dir: string, ref: string, opts: { abort?: boolean; continue?: boolean } = {}): Promise<{ ok: boolean; conflicts: string[]; output: string }> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    return this.serialize(root, async () => {
      const args = ["cherry-pick"]
      if (opts.abort) args.push("--abort")
      else if (opts.continue) args.push("--continue")
      else args.push(ref)
      const res = await this.run(args, root, { allowFail: true, env: { GIT_EDITOR: "true" } })
      this.invalidate(root)
      const st = await this.status(root)
      return { ok: res.code === 0, conflicts: st.changes.filter((c) => c.conflicted).map((c) => c.path), output: (res.stdout + res.stderr).trim() }
    })
  }

  async revert(dir: string, ref: string, opts: { abort?: boolean; continue?: boolean; noCommit?: boolean } = {}): Promise<{ ok: boolean; conflicts: string[]; output: string }> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    return this.serialize(root, async () => {
      const args = ["revert", "--no-edit"]
      if (opts.abort) args.push("--abort")
      else if (opts.continue) args.push("--continue")
      else if (opts.noCommit) args.push("--no-commit")
      if (!opts.abort && !opts.continue) args.push(ref)
      const res = await this.run(args, root, { allowFail: true, env: { GIT_EDITOR: "true" } })
      this.invalidate(root)
      const st = await this.status(root)
      return { ok: res.code === 0, conflicts: st.changes.filter((c) => c.conflicted).map((c) => c.path), output: (res.stdout + res.stderr).trim() }
    })
  }

  /** reset（hard 时可选自动建备份分支）。 */
  async reset(dir: string, ref: string, mode: "soft" | "mixed" | "hard", opts: { backup?: boolean } = {}): Promise<{ backupBranch?: string }> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    return this.serialize(root, async () => {
      let backupBranch: string | undefined
      if (mode === "hard" && opts.backup) {
        backupBranch = `gebai/backup-${Date.now()}`
        await this.run(["branch", backupBranch], root, { allowFail: true })
      }
      await this.run(["reset", `--${mode}`, ref], root)
      this.invalidate(root)
      return { backupBranch }
    })
  }

  async stashOp(dir: string, action: "push" | "pop" | "apply" | "drop" | "show" | "clear", opts: { message?: string; index?: number; staged?: boolean; path?: string } = {}): Promise<{ output: string }> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    return this.serialize(root, async () => {
      const args = ["stash", action]
      if (action === "push") {
        if (opts.staged) args.push("--staged")
        if (opts.message) args.push("-m", opts.message)
        if (opts.path) args.push("--", opts.path)
      } else if (action === "show") args.push(`stash@{${opts.index ?? 0}}`, "--stat")
      else if (opts.index !== undefined) args.push(`stash@{${opts.index}}`)
      const res = await this.run(args, root, { allowFail: true })
      if (res.code !== 0) throw new GitError(422, (res.stderr || res.stdout || `stash ${action} 失败`).trim())
      this.invalidate(root)
      return { output: (res.stdout + res.stderr).trim() }
    })
  }

  async tagOp(dir: string, action: "create" | "delete", name: string, opts: { ref?: string; message?: string; force?: boolean } = {}): Promise<void> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    await this.serialize(root, async () => {
      if (action === "create") {
        const args = ["tag"]
        if (opts.force) args.push("-f")
        if (opts.message) args.push("-a", name, "-m", opts.message)
        else args.push(name)
        if (opts.ref) args.push(opts.ref)
        await this.run(args, root)
      } else await this.run(["tag", "-d", name], root)
      this.invalidate(root)
    })
  }

  async remoteOp(dir: string, action: "add" | "remove" | "set-url", name: string, url?: string): Promise<void> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    await this.serialize(root, async () => {
      if (action === "add") await this.run(["remote", "add", name, url ?? ""], root)
      else if (action === "remove") await this.run(["remote", "remove", name], root)
      else await this.run(["remote", "set-url", name, url ?? ""], root)
      this.invalidate(root)
    })
  }

  /** fetch / pull / push（网络操作，长超时；失败原样回传可读消息）。 */
  async network(
    dir: string,
    action: "fetch" | "pull" | "push",
    opts: { remote?: string; branch?: string; refspec?: string; forceWithLease?: boolean; rebase?: boolean; ffOnly?: boolean; tags?: boolean; prune?: boolean; setUpstream?: boolean } = {},
  ): Promise<{ ok: boolean; output: string }> {
    this.assertRemote()
    const root = await this.requireRepo(dir)
    return this.serialize(root, async () => {
      const args: string[] = [action]
      if (opts.prune && action === "fetch") args.push("--prune")
      if (opts.tags) args.push("--tags")
      if (action === "pull") {
        if (opts.rebase) args.push("--rebase")
        if (opts.ffOnly) args.push("--ff-only")
      }
      if (action === "push") {
        if (opts.forceWithLease) args.push("--force-with-lease")
        if (opts.setUpstream) args.push("--set-upstream")
      }
      if (opts.remote) args.push(opts.remote)
      if (opts.refspec) args.push(opts.refspec)
      else if (opts.branch) args.push(opts.branch)
      const res = await this.run(args, root, { allowFail: true, timeout: NET_TIMEOUT_MS })
      this.invalidate(root)
      return { ok: res.code === 0, output: (res.stdout + res.stderr).trim() }
    })
  }

  async init(dir: string, opts: { bare?: boolean; initialBranch?: string } = {}): Promise<void> {
    this.assertWrite()
    const args = ["init"]
    if (opts.bare) args.push("--bare")
    if (opts.initialBranch) args.push(`--initial-branch=${opts.initialBranch}`)
    await this.run(args, dir)
    this.invalidate(dir)
  }

  /** 追加 .gitignore 条目（文件树右键「忽略」）。 */
  async addIgnore(dir: string, rootAbs: string, entries: string[]): Promise<void> {
    this.assertWrite()
    const root = await this.requireRepo(dir)
    const file = join(root, ".gitignore")
    let existing = ""
    try {
      existing = await Bun.file(file).text()
    } catch {
      existing = ""
    }
    const lines = existing.split(/\r?\n/)
    const add = entries.filter((e) => e.trim() && !lines.includes(e))
    if (!add.length) return
    const sep = existing && !existing.endsWith("\n") ? "\n" : ""
    await Bun.write(file, `${existing}${sep}${add.join("\n")}\n`)
    void rootAbs
  }
}

/** 变更条目构造（porcelain v2 状态码 → 语义化）。 */
function makeChange(path: string, xy: string, extra: { origPath?: string; renamed?: boolean; untracked?: boolean; conflicted?: boolean } = {}): GitChange {
  const index = xy[0] ?? "."
  const worktree = xy[1] ?? "."
  const untracked = extra.untracked ?? xy === "??"
  const conflicted = extra.conflicted ?? (index === "U" || worktree === "U" || xy === "AA" || xy === "DD")
  const renamed = extra.renamed ?? (index === "R" || worktree === "R")
  const staged = !untracked && !conflicted && index !== "." && index !== "?"
  const unstaged = !untracked && !conflicted && worktree !== "." && worktree !== "?"
  const code = index !== "." && index !== "?" ? index : worktree
  const kind: GitChange["kind"] = conflicted
    ? "conflicted"
    : untracked
      ? "untracked"
      : renamed
        ? "renamed"
        : code === "A"
          ? "added"
          : code === "D"
            ? "deleted"
            : code === "M"
              ? "modified"
              : code === "T"
                ? "typechange"
                : code === "C"
                  ? "copied"
                  : "unknown"
  return { path, origPath: extra.origPath, index, worktree, untracked, conflicted, renamed, staged, unstaged, kind }
}

/** 进行中的多步操作探测（.git 目录标记文件）。 */
function detectOperation(root: string): GitStatus["operation"] {
  const gitDir = join(root, ".git")
  if (existsSync(join(gitDir, "MERGE_HEAD"))) return "merge"
  if (existsSync(join(gitDir, "rebase-merge")) || existsSync(join(gitDir, "rebase-apply"))) {
    return existsSync(join(gitDir, "rebase-apply", "applying")) ? "am" : "rebase"
  }
  if (existsSync(join(gitDir, "CHERRY_PICK_HEAD"))) return "cherry-pick"
  if (existsSync(join(gitDir, "REVERT_HEAD"))) return "revert"
  if (existsSync(join(gitDir, "BISECT_LOG"))) return "bisect"
  return undefined
}

/**
 * 解析 unified diff 文本为结构化 hunks（多文件）。
 * 支持 `diff --git` 段、`new file`/`deleted file`/`rename` 元信息、二进制段与 `\ No newline` 标记。
 */
export function parseUnifiedDiff(text: string): GitFileDiff[] {
  const files: GitFileDiff[] = []
  let cur: GitFileDiff | null = null
  let hunk: GitDiffHunk | null = null
  let oldLine = 0
  let newLine = 0
  const push = () => {
    if (cur) files.push(cur)
    cur = null
    hunk = null
  }
  for (const line of text.split("\n")) {
    if (line.startsWith("diff --git ")) {
      push()
      const m = /^diff --git a\/(.*) b\/(.*)$/.exec(line)
      cur = { path: m?.[2] ?? "", oldPath: m?.[1], binary: false, hunks: [], additions: 0, deletions: 0 }
      continue
    }
    if (!cur) continue
    if (line.startsWith("new file mode")) {
      cur.status = "added"
      continue
    }
    if (line.startsWith("deleted file mode")) {
      cur.status = "deleted"
      continue
    }
    if (line.startsWith("rename from ")) {
      cur.oldPath = line.slice(12)
      cur.status = "renamed"
      continue
    }
    if (line.startsWith("rename to ")) {
      cur.path = line.slice(10)
      cur.status = "renamed"
      continue
    }
    if (line.startsWith("copy from ")) {
      cur.oldPath = line.slice(10)
      cur.status = "copied"
      continue
    }
    if (line.startsWith("copy to ")) {
      cur.path = line.slice(8)
      cur.status = "copied"
      continue
    }
    if (line.startsWith("Binary files ") || line.startsWith("GIT binary patch")) {
      cur.binary = true
      continue
    }
    if (line.startsWith("--- ")) continue
    if (line.startsWith("+++ ")) continue
    if (line.startsWith("index ") || line.startsWith("old mode") || line.startsWith("new mode") || line.startsWith("similarity index") || line.startsWith("dissimilarity index")) continue
    const hm = /^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)$/.exec(line)
    if (hm) {
      oldLine = Number(hm[1])
      newLine = Number(hm[3])
      hunk = {
        header: line,
        oldStart: oldLine,
        oldLines: hm[2] === undefined ? 1 : Number(hm[2]),
        newStart: newLine,
        newLines: hm[4] === undefined ? 1 : Number(hm[4]),
        lines: [],
      }
      cur.hunks.push(hunk)
      continue
    }
    if (!hunk) continue
    if (line.startsWith("\\")) continue // \ No newline at end of file
    const marker = line[0]
    const body = line.slice(1)
    if (marker === "+") {
      hunk.lines.push({ type: "add", text: body, oldLine: null, newLine })
      newLine++
      cur.additions++
    } else if (marker === "-") {
      hunk.lines.push({ type: "del", text: body, oldLine, newLine: null })
      oldLine++
      cur.deletions++
    } else {
      hunk.lines.push({ type: "context", text: body, oldLine, newLine })
      oldLine++
      newLine++
    }
  }
  push()
  return files.filter((f) => f.hunks.length > 0 || f.binary || f.status)
}
