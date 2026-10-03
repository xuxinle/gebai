/**
 * 文件工作台 · Git 修改标记（dirty-diff gutter）纯逻辑：行级差异 → gutter 标记描述。
 *
 * 为什么单独成模块：差异换算错了**不报错**——只表现为「标错行 / 标错颜色 / 纯删除不显示」，
 * 而行号惯例（空区间的两种历史表达）与夹取规则正是单测能钉住、DOM 侧钉不住的部分。
 * 请求/DOM 侧（HEAD 基线获取、隐藏 diff 引擎、装饰渲染）在 editor.ts 与 main.ts。
 */

/**
 * 行级差异（结构对齐 Monaco `ILineChange` 的字段子集；不 import monaco 类型，纯函数可单测）。
 *
 * 空区间惯例**两种都兼容**（`start > end` 一并覆盖）：
 * - 现行（0.4x+，DetailedLineRangeMapping 适配层）：纯插入 `originalStart = originalEnd + 1`、
 *   纯删除 `modifiedStart = modifiedEnd + 1`（空区间表达）；
 * - 旧惯例（0.2x 直接暴露 DiffComputer 输出）：插入/删除时对侧 `end = 0`。
 */
export interface GitLineChange {
  originalStartLineNumber: number
  originalEndLineNumber: number
  modifiedStartLineNumber: number
  modifiedEndLineNumber: number
}

/**
 * 一段 gutter 标记的语义描述（editor.ts 据此构造 Monaco 装饰：gutter 条 + 概览标尺 + 小地图）。
 * `line` 为装饰挂靠行（1 起始）：added/modified 取块内每一行（逐行一条，删除锚才能与相邻块同行共存）；
 * deleted 只有一条，挂在**删除点后的第一行**（删到文件末尾时锚在最后一行）。
 */
export interface GitMark {
  kind: "added" | "modified" | "deleted"
  line: number
}

/** 行级差异 → gutter 标记序列（按差异块顺序、块内按行序）。totalLines 用于夹取越界行——diff 结果与 model 行数存在短暂不一致的窗口（打字后 diff 异步重算期间）。 */
export function marksFromLineChanges(changes: readonly GitLineChange[], totalLines: number): GitMark[] {
  const max = Math.max(1, totalLines)
  const clamp = (n: number): number => Math.min(Math.max(1, n), max)
  const marks: GitMark[] = []
  for (const c of changes) {
    // 数值防御：NaN/越界输入按空区间丢弃（一个 diff 块至少一侧有行，双空不存在）
    const oS = Number(c.originalStartLineNumber)
    const oE = Number(c.originalEndLineNumber)
    const mS = Number(c.modifiedStartLineNumber)
    const mE = Number(c.modifiedEndLineNumber)
    if (![oS, oE, mS, mE].every(Number.isFinite)) continue
    const oEmpty = oS > oE
    const mEmpty = mS > mE
    if (oEmpty && !mEmpty) {
      // 纯插入：新行整块标绿
      for (let l = clamp(mS); l <= clamp(mE); l++) marks.push({ kind: "added", line: l })
    } else if (!oEmpty && mEmpty) {
      // 纯删除：删除点锚行挂红三角（modifiedStart 是删除内容之后的第一行）
      marks.push({ kind: "deleted", line: clamp(mS) })
    } else if (!oEmpty && !mEmpty) {
      // 修改（原行被改写，含块内增删混合）：新行整块标蓝
      for (let l = clamp(mS); l <= clamp(mE); l++) marks.push({ kind: "modified", line: l })
    }
  }
  return marks
}

/**
 * gutter 是否适用于该文件（接线处据此短路：不发基线请求、不挂 diff 引擎）。
 * 口径与 VSCode 的 dirty diff 一致：非仓库 / HEAD 取不到（未跟踪、二进制、超体量）/
 * 工作区读取被截断（内容不完整，行级差异会全错）/ 降级编辑器（画不出装饰）都不标。
 */
export function gutterEligible(o: { isRepo: boolean; headAvailable: boolean; truncated: boolean; editorKind: "monaco" | "fallback" }): boolean {
  return o.isRepo && o.headAvailable && !o.truncated && o.editorKind === "monaco"
}
