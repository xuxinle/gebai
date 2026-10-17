/**
 * 提交范围：**追加式暂存的纪律**，以及「这次提交里有没有别人的东西」的判据。
 *
 * ## 为什么需要它（一起真实事故，本仓 2026-10-17）
 *
 * worker `shared-lease` 提交 `856ba9bb` 时用了宽范围暂存（`git add -A`），把同一个仓里**另一个仍在跑的
 * worker**（`retire-tree`，当时 384 消息、在途未收）的两份未完成成果一起提交了：
 *
 * ```
 * A  packages/agents/src/agents/shengsheng/proc-tree.ts                  (+596)
 * A  packages/agents/src/agents/shengsheng/shengsheng-retire-tree.test.ts (+573)
 * ```
 *
 * 它的提交说明只写了自己那一单，根本没提这两份。内容没丢，但真损失有三样：① 另一个 worker 若按
 * 「我还没提交」的认知继续干，会重复劳动或在旧基线上改；② 台账归属记错；③ 若那份代码有缺陷，
 * 责任人定错。
 *
 * **根因不是粗心，是守卫的缺口**：`edit`/`write` 的「防陈旧覆盖守卫」只管**已跟踪文件**的修改；
 * 未跟踪的新文件（`git status` 的 `??` 行）**不受任何守卫保护**——**谁 add 谁带走**。而新增文件
 * 恰恰是 worker 最常产出的东西（新模块、新用例）。
 *
 * ## 三条纪律（本模块把它们变成可执行的东西，不只是文档里的一句话）
 *
 * 1. **只暂存自己列出的路径**：`git add -- <逐个路径>`（{@link stagingCommands}）。
 *    **禁止** `git add -A` / `git add .` / `git add -u` / `git commit -a`（{@link commitProtocolLine}）。
 * 2. **提交前逐行核对**：`git diff --cached --name-status`，出现不在工单清单里的文件就停下。
 * 3. **工作树有 `??` 时先问「这是谁的新文件」**——新增文件最容易被带走，因为它们不触发 edit 守卫。
 *
 * ## 工具层判据：review 时核「最近一次提交 vs 工单提到的路径」
 *
 * {@link scopeAudit} 把提交里改动的文件与工单里提到的路径做比对，产出**不在工单里**的那些
 * ——那正是「疑似宽范围暂存带走了别人的改动」的形状。它是**提示**而不是判决：
 * 新文件（回归测试、拆出的模块）本来就常没被工单逐字点名，所以报告里同时给出每个文件的
 * `+N/−M`，让人/模型自己核一眼「这行是不是别人的活」。
 *
 * ## 边界（诚实声明）
 *
 * · 判据是**路径文本**：工单只写了「补回归用例」而没写路径时，新用例文件会被报成「不在工单里」
 *   （提示而非拦截，代价是一行噪音）。
 * · 只看**最近一次提交**：一条线上连续几次提交里夹带的，只有最后一次被核到。
 * · 只看**文件名与增删行数**，不看内容归属：两个 worker 改同一文件的不同段落，本模块无从分辨。
 */
import type { ToolContext } from "@gebai/sdk"

/** 宽范围暂存的**禁用形态**（每一条都是「会把工作树里别人的东西一起带走」的写法）。 */
export const WIDE_STAGING_FORMS: readonly { form: string; why: string }[] = [
  { form: "git add -A", why: "整棵树，含别人未提交的修改与未跟踪的新文件" },
  { form: "git add .", why: "整个目录（子目录里含别人的活）" },
  { form: "git add -u", why: "所有已跟踪文件的修改（含别人改的那几行）" },
  { form: "git commit -a", why: "提交时自动暂存全部已跟踪修改（等于把 add -u 藏在 commit 里）" },
  { form: "git add --all", why: "`-A` 的长写" },
]

/** 提交纪律的**操作形态**（派工模板逐单注入；worker 只看得见这条消息，看不见生生的提示词）。
 *
 *  为什么由代码生成而不在模板里写死：纪律文本与 {@link stagingCommands} 必须同源——
 *  各写一份就会漂成「消息让 worker 逐个列路径，示例却是 `git add -A`」。
 *  `own` 给出工单里的交付路径时，示例直接用它（worker 照抄即合规）。 */
export function commitProtocolLine(own: readonly string[] = []): string {
  const sample = own.length ? own.slice(0, 3).join(" ") : "<你这一单要提交的每个文件路径>"
  return [
    "提交纪律（宽范围暂存会带走别人的东西——本条来自一起真实事故）：",
    `✅ 只暂存自己列出的路径：git add -- ${sample}`,
    `❌ 禁止 ${WIDE_STAGING_FORMS.map((w) => `\`${w.form}\``).join(" / ")}`,
    "提交前必须 `git diff --cached --name-status` 逐行核对：出现不在自己工单清单里的文件就停下（那是别人的活）。",
    "工作树里有 `??` 未跟踪文件时先问「这是谁的新文件」——新增文件不触发 edit/write 的防陈旧覆盖守卫，最容易被一起带走。",
  ].join(" ")
}

/** 追加式暂存的**命令序列**（{@link commitProtocolLine} 的可执行形态）。
 *
 *  `git add -- <paths>` 里的 `--` 不是装饰：路径以 `-` 开头时没有它会被当成选项。
 *  显式路径 + `--` 之后，**这台机器上不存在**「顺手把别的文件带进去」的语法空间。 */
export function stagingCommands(own: readonly string[]): string[] {
  const paths = own.map((p) => p.trim()).filter(Boolean)
  return [
    paths.length ? `git add -- ${paths.join(" ")}` : "git add -- <逐个列出你这一单要提交的文件>",
    "git diff --cached --name-status",
  ]
}

/** 一条提交里的文件改动。 */
export interface CommitFile {
  /** 仓内相对路径（重命名取**新**路径）。 */
  path: string
  /** 增行数（二进制文件为 `null`）。 */
  added: number | null
  /** 删行数（二进制文件为 `null`）。 */
  deleted: number | null
  /** `git --name-status` 的首字母：A/M/D/R/C/T。 */
  status: string
}

/** 最近一次提交。 */
export interface LastCommit {
  sha: string
  /** 提交时间（git 的 `%ci` 原文）。 */
  when: string
  subject: string
  files: CommitFile[]
}

/** 解析 `git log -1 --pretty=format:%H%x1f%ci%x1f%s --raw --numstat` 的输出。
 *
 *  两种行交织出现（先 raw 块、再 numstat 块），故按**形状**分流再按路径合并：
 *    · `:100644 100644 <sha> <sha> A\tpath`（raw 格式）或 `A\tpath`（`--name-status` 格式）—— 状态
 *    · `12\t3\tpath` —— 增删行数（二进制是 `-\t-\tpath`）
 *  合并不上的行直接忽略（解析应当只比 git 输出宽松一点，不该因为一行怪数据就丢掉整条提交）。
 *
 *  为什么用 `--raw --numstat` 而不是 `--name-status --numstat`：两者同时指定时 **name-status 胜出、
 *  numstat 行根本不出现**，于是「+596」这个本事故最重要的特征（见模块头注释）会被静默丢掉。
 *  这一条是实测出来的：首版用的就是 `--name-status`，用例里所有文件都成了“（二进制）”。 */
export function parseLastCommit(stdout: string): LastCommit | null {
  const lines = stdout.split(/\r?\n/)
  const head = lines[0] ?? ""
  const [sha, when, ...subjectParts] = head.split("\x1f")
  if (!sha || !/^[0-9a-f]{7,40}$/i.test(sha.trim())) return null
  const byPath = new Map<string, CommitFile>()
  const order: string[] = []
  const touch = (path: string): CommitFile => {
    let f = byPath.get(path)
    if (!f) {
      f = { path, added: null, deleted: null, status: "?" }
      byPath.set(path, f)
      order.push(path)
    }
    return f
  }
  for (const line of lines.slice(1)) {
    if (!line.trim()) continue
    // raw 格式：`:100644 100644 <sha> <sha> A\tpath`（重命名是 `R100\told\tnew`）
    const raw = /^:\d{6} \d{6} [0-9a-f]+ [0-9a-f]+ ([AMDRCT])\d*\t(.+)$/.exec(line)
    if (raw) {
      const rest = raw[2].split("\t")
      const path = (rest[rest.length - 1] ?? "").trim()
      if (path) touch(path).status = raw[1]
      continue
    }
    // name-status 格式：`A\tpath` / `R100\told\tnew`
    const status = /^([AMDRCT])(?:\d+)?\t(.+)$/.exec(line)
    if (status) {
      const code = status[1][0]
      const rest = status[2].split("\t")
      // 重命名/复制：`old\tnew`，归属与路径都取新路径
      const path = (rest[rest.length - 1] ?? "").trim()
      if (!path) continue
      const f = touch(path)
      f.status = code
      continue
    }
    const num = /^(-|\d+)\t(-|\d+)\t(.+)$/.exec(line)
    if (num) {
      const f = touch(num[3].trim())
      f.added = num[1] === "-" ? null : Number(num[1])
      f.deleted = num[2] === "-" ? null : Number(num[2])
    }
  }
  const files = order.map((p) => byPath.get(p)!).filter((f) => f.status !== "?")
  return { sha: sha.trim(), when: (when ?? "").trim(), subject: subjectParts.join("\x1f").trim(), files }
}

/** 从自由文本里抽**路径候选**（工单的方向、预期产出、任务摘要都是自由文本）。
 *
 *  两种形状：① 含 `/` 的路径（`packages/agents/src/x.ts`、`docs/PITFALLS.md`、`tools/a/b/`）；
 *  ② 裸文件名带常见扩展名（`proc-tree.ts`、`PITFALLS.md`、`*.revbak`）。
 *  不做存在性校验——工单里写的路径可能还没创建（那正是「这一单要新建的文件」）。 */
export function mentionPaths(text: string): string[] {
  const out = new Set<string>()
  const withSlash = /[\w@.+-]+(?:\/[\w@.+-]+)+\/?/g
  for (const m of text.matchAll(withSlash)) out.add(m[0].replace(/[.,;:、）)]+$/, ""))
  const bare = /(?:^|[\s（(、「",：:；;])([\w@.+-]+\.(?:ts|tsx|js|mjs|py|md|cpp|hpp|h|json|txt|sh|yml|yaml|pkg))(?=$|[\s）)、,，。：:；;"'])/g
  for (const m of text.matchAll(bare)) out.add(m[1])
  return [...out].map(normalizePath).filter(Boolean)
}

/** 路径归一：去引号/反引号、去 `./` 前缀、反斜杠转正斜杠、压掉尾部 `/`（目录标记另判）。 */
function normalizePath(p: string): string {
  return p.replace(/[`'"]/g, "").replace(/\\/g, "/").replace(/^\.\//, "").replace(/\/+$/, "").trim()
}

const basenameOf = (p: string): string => p.slice(p.lastIndexOf("/") + 1)

/** 这一条提到是否覆盖该文件（三档判据，任一成立即算覆盖）：
 *  ① 精确同路径；② 提到是**目录**且文件在它之下（`.../shengsheng/` 或 `.../shengsheng`）；
 *  ③ 文件名相同（工单写 `proc-tree.ts`、提交里是 `packages/…/proc-tree.ts`）。 */
export function mentionCovers(mention: string, file: string): boolean {
  const m = normalizePath(mention)
  const f = normalizePath(file)
  if (!m || !f) return false
  if (m === f) return true
  if (f.startsWith(`${m}/`)) return true
  // 提到末段无扩展名（且不是通配）⇒ 当目录看待：`packages/agents/src/agents/shengsheng`
  const last = basenameOf(m)
  if (!last.includes(".") && !last.includes("*") && f.startsWith(`${m}/`)) return true
  return basenameOf(m) === basenameOf(f)
}

/** 一次范围核对的结论。 */
export interface ScopeAudit {
  /** 在工单范围内的文件。 */
  inScope: CommitFile[]
  /** **不在**范围内的文件（疑似宽范围暂存带走的东西）。 */
  outOfScope: CommitFile[]
  /** 工单里抽到的路径候选（人读：为什么这么判）。 */
  mentions: string[]
}

/** 提交里的文件 vs 工单提到的路径。 */
export function scopeAudit(files: readonly CommitFile[], mentions: readonly string[]): ScopeAudit {
  const inScope: CommitFile[] = []
  const outOfScope: CommitFile[] = []
  for (const f of files) {
    if (mentions.some((m) => mentionCovers(m, f.path))) inScope.push(f)
    else outOfScope.push(f)
  }
  return { inScope, outOfScope, mentions: [...mentions] }
}

/** 一条工作树里的未跟踪/已修改条目（`git status --porcelain` 的一行）。 */
export interface TreeEntry {
  /** 两字母状态码，如 `??` / ` M` / `M `。 */
  code: string
  path: string
}

/** 解析 `git status --porcelain`（只取路径；重命名行取新路径）。 */
export function parseStatusPorcelain(stdout: string): TreeEntry[] {
  const out: TreeEntry[] = []
  for (const line of stdout.split(/\r?\n/)) {
    if (line.trim().length < 4) continue
    const code = line.slice(0, 2)
    const rest = line.slice(3).trim()
    const path = rest.includes(" -> ") ? rest.slice(rest.indexOf(" -> ") + 4) : rest
    if (path) out.push({ code, path })
  }
  return out
}

/** 一条文件改动的可读形：`A proc-tree.ts（+596 −0）`。 */
export function describeFile(f: CommitFile): string {
  const delta = f.added == null && f.deleted == null ? "（二进制）" : `（+${f.added ?? 0} −${f.deleted ?? 0}）`
  return `${f.status} ${f.path}${delta}`
}

// ---------------------------------------------------------------------------
// 读盘：最近提交 + 工作树（经 `ctx.runCommand`，拿不到就安静退化）
// ---------------------------------------------------------------------------

/** `git` 探测结论：拿不到就 `null`（不在 git 仓内 / 没装 git / 宿主不提供 runCommand）。 */
export async function readGit(ctx: ToolContext, repoTop: string, args: string, timeoutMs = 5000): Promise<string | null> {
  const run = (ctx as { runCommand?: ToolContext["runCommand"] }).runCommand
  if (typeof run !== "function") return null
  if (repoTop.includes('"')) return null
  try {
    const r = await run(`git -C "${repoTop}" ${args}`, { timeoutMs })
    if (r.code !== 0) return null
    return r.stdout ?? ""
  } catch {
    return null
  }
}

/** 最近一次提交（读不到 ⇒ `null`，调用方不报「没有宽范围暂存」而只是不加这条建议——
 *  「读不到」与「核过没问题」必须是两个结论，见 PITFALLS「0 命中与读不到必须是两个数」）。 */
export async function readLastCommit(ctx: ToolContext, repoTop: string): Promise<LastCommit | null> {
  const out = await readGit(ctx, repoTop, "log -1 --pretty=format:%H%x1f%ci%x1f%s --raw --numstat")
  return out == null ? null : parseLastCommit(out)
}

/** 工作树现状（未跟踪的新文件在 `??` 行）。 */
export async function readWorkTree(ctx: ToolContext, repoTop: string): Promise<TreeEntry[] | null> {
  const out = await readGit(ctx, repoTop, "status --porcelain")
  return out == null ? null : parseStatusPorcelain(out)
}

/** 工作树里「属于别人」的**未跟踪新文件**（`??`）——本条纪律最想让人看见的那一类。 */
export function untrackedOf(entries: readonly TreeEntry[]): TreeEntry[] {
  return entries.filter((e) => e.code === "??")
}
