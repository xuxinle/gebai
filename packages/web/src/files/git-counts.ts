/**
 * 状态栏「分支格」里变更计数的**呈现规则**（纯逻辑，可单测）。
 *
 * 为什么单独拎出来：这一段原先是一句内联模板串（`${staged}± ${unstaged}± ${untracked}?`），
 * 有两个实测出来的毛病：
 *
 * ① **冲突数被漏掉**——服务端 `makeChange` 里 `staged/unstaged` 对冲突行恒为 false
 *    （`!conflicted && …`），冲突只进 `conflicted`；而模板串只印了前三个。于是**一个只有冲突的仓库
 *    会显示 `0± 0± 0?`**（实测：counts = {0,0,0,1} → 状态栏 `main 0± 0± 0?`）——不但没说有冲突，
 *    看着还像“工作区干净”。
 * ② **零值照印**——`0± 1± 0?` 这种“两个零夹一个 1”，既占宽又要靠位置去猜哪个是哪个。
 *
 * 现规则：**非零才显示**，符号各自表意（`!` 冲突 / `+` 已暂存 / `±` 未暂存 / `?` 未跟踪），
 * 顺序按**严重度**：冲突最先（最该先处理），其次已暂存（准备提交的）、未暂存、未跟踪。
 */
import type { GitStatusInfo } from "./api"

export type ChangeKind = "conflicted" | "staged" | "unstaged" | "untracked"

/** 一个计数片段：种类 + 数字 + 显示文本（符号在文本里）。 */
export interface ChangePart {
  kind: ChangeKind
  n: number
  text: string
}

/** 计数与符号的对应（顺序即显示顺序：严重度从高到低）。 */
const PARTS: { kind: ChangeKind; symbol: string }[] = [
  { kind: "conflicted", symbol: "!" },
  { kind: "staged", symbol: "+" },
  { kind: "unstaged", symbol: "±" },
  { kind: "untracked", symbol: "?" },
]

/** 变更计数片段（**只给非零项**，按严重度排序）。全零 = 空数组（调用方据此不渲染那一段）。 */
export function changeParts(c: GitStatusInfo["counts"]): ChangePart[] {
  return PARTS.filter((p) => c[p.kind] > 0).map((p) => ({ kind: p.kind, n: c[p.kind], text: `${c[p.kind]}${p.symbol}` }))
}

/** 悬浮说明里每项的中文名（与符号表一一对应）。 */
const NAMES: Record<ChangeKind, string> = {
  conflicted: "冲突待解决",
  staged: "已暂存",
  unstaged: "未暂存修改",
  untracked: "未跟踪文件",
}

/**
 * 分支格的悬浮说明：把每个符号写成中文，并说明领先/落后远程几个提交。
 *
 * 这是“这一格到底怎么读”的**唯一答案所在**——状态栏位置有限，符号只能极简（`2! 3+ 4± 5?`），
 * 含义写进悬浮（沿用状态栏其他格子的做法：能点/能看的格子都用 title 自解释）。
 * 没有变更时给「工作区干净」而不是省略（否则悬浮只剩分支名，看不出“为什么没有计数”）。
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
