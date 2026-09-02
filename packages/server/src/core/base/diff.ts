import type { DiffLine } from "@gebai/sdk"

/** diff 工具单侧行数上限（LCS DP 内存与耗时保护）。 */
export const DIFF_MAX_LINES = 2000
/** unified diff 上下文行数。 */
const DIFF_CONTEXT = 3

/** 按扩展名推断语法高亮语言（与前端 EXT_LANG 保持一致）。 */
const EXT_LANG: Record<string, string> = {
  ts: "typescript", tsx: "typescript", js: "javascript", jsx: "javascript", mjs: "javascript", cjs: "javascript",
  json: "json", py: "python", pyw: "python", sh: "bash", bash: "bash", zsh: "bash",
  css: "css", scss: "scss", less: "less", html: "xml", htm: "xml", xml: "xml", svg: "xml", vue: "xml", svelte: "xml",
  md: "markdown", markdown: "markdown", yml: "markdown", yaml: "markdown",
  go: "go", rs: "rust", java: "java", kt: "kotlin", kts: "kotlin", rb: "ruby",
  c: "c", h: "c", cpp: "cpp", hpp: "cpp", cc: "cpp", cs: "csharp", php: "php",
  sql: "sql", lua: "lua", swift: "swift", dart: "dart",
}

export function inferLang(name: string): string {
  const ext = name.split(".").pop()?.toLowerCase() ?? ""
  return EXT_LANG[ext] ?? ""
}

/** 拆分文本为行（忽略末尾换行产生的空行，`"a\n"` 与 `"a"` 等价）。 */
export function splitLines(text: string): string[] {
  if (text === "") return []
  const lines = text.split("\n")
  if (lines.length > 1 && lines[lines.length - 1] === "") lines.pop()
  return lines
}

/**
 * 行级 LCS diff：返回按顺序排列的行序列，每行标注 equal/add/del。
 * 行数乘积超限时退化为「全删 + 全增」，保证大输入不爆内存。
 */
export function diffLines(oldText: string, newText: string): DiffLine[] {
  const a = splitLines(oldText)
  const b = splitLines(newText)
  const n = a.length
  const m = b.length
  if (n * m > DIFF_MAX_LINES * DIFF_MAX_LINES) {
    const lines: DiffLine[] = []
    for (let i = 0; i < n; i++) lines.push({ kind: "del", text: a[i] })
    for (let j = 0; j < m; j++) lines.push({ kind: "add", text: b[j] })
    return lines
  }
  const w = m + 1
  const dp = new Uint32Array((n + 1) * w)
  for (let i = 1; i <= n; i++) {
    const ai = a[i - 1]
    for (let j = 1; j <= m; j++) {
      dp[i * w + j] = ai === b[j - 1]
        ? dp[(i - 1) * w + j - 1] + 1
        : Math.max(dp[(i - 1) * w + j], dp[i * w + j - 1])
    }
  }
  const lines: DiffLine[] = []
  // 回溯时 del/add 暂存双栈，遇 equal 时先弹出 add 再弹出 del（reverse 后得到 del 在前、add 在后的正确顺序）
  const delStack: string[] = []
  const addStack: string[] = []
  const flush = () => {
    // 栈内为逆序收集（相对正序文本），正序遍历 push；整体 reverse 后得到 del 按旧序、add 按新序
    for (const t of addStack) lines.push({ kind: "add", text: t })
    for (const t of delStack) lines.push({ kind: "del", text: t })
    addStack.length = 0
    delStack.length = 0
  }
  let i = n
  let j = m
  while (i > 0 && j > 0) {
    if (a[i - 1] === b[j - 1]) {
      flush()
      lines.push({ kind: "equal", text: a[i - 1] })
      i--
      j--
    } else if (dp[(i - 1) * w + j] >= dp[i * w + j - 1]) {
      delStack.push(a[i - 1])
      i--
    } else {
      addStack.push(b[j - 1])
      j--
    }
  }
  while (i > 0) delStack.push(a[--i])
  while (j > 0) addStack.push(b[--j])
  flush()
  lines.reverse()
  return lines
}

/**
 * 生成 unified diff 文本（含 hunk 头与上下文），供 LLM 阅读。
 * 空文本侧用 `/dev/null` 标注；`aName`/`bName` 为对比名（默认 old/new）。
 */
export function unifiedDiff(oldText: string, newText: string, aName = "old", bName = "new"): string {
  const lines = diffLines(oldText, newText)
  const out: string[] = []
  out.push(`--- ${oldText === "" ? "/dev/null" : aName}`)
  out.push(`+++ ${newText === "" ? "/dev/null" : bName}`)
  // 预处理：pos[i] = 从 i 起是否还存在变化行，用于 hunk 截断判断
  const hasChangeAfter = new Array<boolean>(lines.length + 1).fill(false)
  for (let k = lines.length - 1; k >= 0; k--) {
    hasChangeAfter[k] = hasChangeAfter[k + 1] || lines[k].kind !== "equal"
  }
  let i = 0 // lines 游标
  let aNo = 0 // 已消费的旧侧行数
  let bNo = 0
  while (i < lines.length) {
    // 跳过 equal 连续区（上下文）
    while (i < lines.length && lines[i].kind === "equal") {
      aNo++
      bNo++
      i++
    }
    if (i >= lines.length) break
    // 变化前回看 DIFF_CONTEXT 行上下文
    const ctx: string[] = []
    for (let back = i - 1, k = 0; back >= 0 && k < DIFF_CONTEXT && lines[back].kind === "equal"; back--, k++) {
      ctx.unshift(lines[back].text)
    }
    const body: string[] = []
    for (const c of ctx) body.push(` ${c}`)
    let cntA = ctx.length
    let cntB = ctx.length
    while (i < lines.length) {
      const l = lines[i]
      if (l.kind === "equal") {
        // 统计连续 equal 行数
        let run = 0
        while (i + run < lines.length && lines[i + run].kind === "equal") run++
        if (hasChangeAfter[i]) {
          if (run > DIFF_CONTEXT * 2) {
            // 距下一变化较远：取 DIFF_CONTEXT 行尾部上下文后结束 hunk
            for (let k = 0; k < DIFF_CONTEXT; k++) {
              body.push(` ${lines[i + k].text}`)
              cntA++
              cntB++
              aNo++
              bNo++
            }
            i += DIFF_CONTEXT
            break
          }
          // 间隔 ≤ 2×DIFF_CONTEXT：全部并入本 hunk（GNU 合并语义）
          for (let k = 0; k < run; k++) {
            body.push(` ${lines[i + k].text}`)
            cntA++
            cntB++
            aNo++
            bNo++
          }
          i += run
          if (i >= lines.length) break
          continue // 间隔内的变化并入同一 hunk
        }
        // 无后续变化：取 DIFF_CONTEXT 行尾部上下文后结束（GNU 尾部上下文最多 3 行）
        const take = Math.min(DIFF_CONTEXT, run)
        for (let k = 0; k < take; k++) {
          body.push(` ${lines[i + k].text}`)
          cntA++
          cntB++
          aNo++
          bNo++
        }
        i += take
        break
      } else if (l.kind === "del") {
        body.push(`-${l.text}`)
        cntA++
        aNo++
        i++
      } else {
        body.push(`+${l.text}`)
        cntB++
        bNo++
        i++
      }
    }
    const startA = cntA === 0 ? 0 : aNo - cntA + 1
    const startB = cntB === 0 ? 0 : bNo - cntB + 1
    out.push(`@@ -${startA}${cntA === 1 ? "" : `,${cntA}`} +${startB}${cntB === 1 ? "" : `,${cntB}`} @@`)
    for (const line of body) out.push(line)
  }
  if (out.length === 2) out.push("（无差异）")
  return out.join("\n")
}
