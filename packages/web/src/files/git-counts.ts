/**
 * 状态栏「分支格」里变更计数的**呈现规则**（纯逻辑，可单测）。
 *
 * 为什么用**中文短词**而不给符号：初版用 `1! 2+ 1± 1?` 这类符号，两个硬伤——
 * ① `±` 字形就是 "+−" 叠在一起，与旁边的 `+`（已暂存）并排时靠字形分辨太费劲，而且它语义上也不
 *    贴切（“未暂存”不是“加减”）；② 符号得先去记一张表才知道什么意思（使用者连问两次“怎么读”）。
 * 现在直接写词：`1冲突 2暂存 1改动 1新增`——多占约一倍宽度，但不用学、不会看错
 *（“未暂存”写“改动”而不写“未暂存”，是为了每项都是**两字词**、宽度可预期；“新增”
 * 特指“还没纳入版本控制的新文件”——一旦 `git add` 它就计入“暂存”了）。
 *
 * 旧符号与词一一对应（历史沿用的符号全在，方便对照旧文档/旧截图）：`!` 冲突 / `+` 已暂存 /
 * `±` 未暂存 / `?` 未跟踪。
 */
import type { GitStatusInfo } from "./api"

export type ChangeKind = "conflicted" | "staged" | "unstaged" | "untracked"

/** 一个计数片段：种类 + 数字 + 显示文本（词在数字后面）。 */
export interface ChangePart {
  kind: ChangeKind
  n: number
  text: string
}

/** 计数与短词的对应（顺序即显示顺序：严重度从高到低）。 */
const PARTS: { kind: ChangeKind; word: string }[] = [
  { kind: "conflicted", word: "冲突" },
  { kind: "staged", word: "暂存" },
  { kind: "unstaged", word: "改动" },
  { kind: "untracked", word: "新增" },
]

/** 变更计数片段（**只给非零项**，按严重度排序）。全零 = 空数组（调用方据此不渲染那一段）。 */
export function changeParts(c: GitStatusInfo["counts"]): ChangePart[] {
  return PARTS.filter((p) => c[p.kind] > 0).map((p) => ({ kind: p.kind, n: c[p.kind], text: `${c[p.kind]}${p.word}` }))
}

/** 悬浮说明里每项的完整说法（短词的展开；短词是它的缩写，两者词根一致）。 */
const NAMES: Record<ChangeKind, string> = {
  conflicted: "冲突待解决",
  staged: "已暂存",
  unstaged: "未暂存改动",
  untracked: "未跟踪新文件",
}

/**
 * 分支格的悬浮说明：把每个计数写成完整说法，并说明领先/落后远程几个提交。
 *
 * 与格内短词是**词根一致的展开**（`1改动` ↔ 「未暂存改动 1 个」），悬浮因此不是另一套说法、
 * 而是同一件事说全（另附分支/游离态与“工作区干净”，并写明这一格可点）。
 */
export function branchTitle(s: Pick<GitStatusInfo, "branch" | "detached" | "ahead" | "behind" | "counts">): string {
  const segs: string[] = []
  const head = s.detached ? "HEAD 处于游离状态（detached）" : `当前分支 ${s.branch || "-"}`
  segs.push(head)
  if (s.ahead) segs.push(`领先远程 ${s.ahead} 个提交`)
  if (s.behind) segs.push(`落后远程 ${s.behind} 个提交`)
  const parts = changeParts(s.counts)
  if (parts.length) segs.push(parts.map((p) => `${NAMES[p.kind]} ${p.n} 个`).join("，"))
  else segs.push("工作区干净")
  return `${segs.join("；")}（点击打开源代码管理工具窗 Alt+G）`
}
