#!/usr/bin/env bun
/**
 * 提示词**硬事实核对器**：把「精简提示词时不丢掉关键信息」变成可复算的判据。
 *
 * ## 它解决什么
 *
 * 精简提示词时，最危险的不是删掉整段（那看得见），而是删掉**藏在句子里的一个具体值**——
 * `150_000`、`3 分钟宽限`、`--naive`、五种禁用形态里的一条。这些东西删掉后句子照样通顺、
 * 照样读得懂，只有真正用到它的那一息才会发现「规则还在，可是不知道阈值是多少」。
 *
 * ## 用法
 *
 * ```
 * # 1. 先把当前版本存档为基线
 * bun tools/prompt-factcheck.ts --baseline old.md
 *
 * # 2. 改完之后核对
 * bun tools/prompt-factcheck.ts --baseline old.md --check new.md
 *
 * # 3. 只看基线里有哪些多位数字（人工判「哪个值不能再丢」）
 * bun tools/prompt-factcheck.ts --baseline old.md --numbers
 * ```
 *
 * 退出码：0 = 全部硬事实仍在；1 = 有缺失（逐条列出）；2 = 参数错。
 *
 * ## 判据口径（四类，都取**字面**）
 *
 * ① 行内代码片段（`` `...` ``）——提示词里操作性事实的载体
 * ② env 变量名（`SHENGSHENG_*` 一类的全大写下划线名）
 * ③ 文件路径（`packages/…`、`tools/…`、`.evolve/…`）
 * ④ 围栏代码块里的整行命令
 *
 * **数字单独成表**：核对器只报「基线有、新文件没有」的多位数字，供人工判——
 * 有些数字（事故现场的一次性测量值）本来就该丢，有些（阈值）丢了就是缺陷。
 * 这个判断不能自动化，所以本工具**只报不改**。
 *
 * ## 它不做什么
 *
 * 不判「该不该缺」。精简时删掉事故编号是对的，删掉阈值是错的，两者在字面上没有区别——
 * 所以缺失清单必须由人过一遍（参考做法：把每条缺失落成一张豁免表，写明为什么可以缺）。
 */

import { readFileSync } from "node:fs"

interface Facts {
  code: string[]
  env: string[]
  path: string[]
  cmd: string[]
}

/** 从提示词文本里抽四类硬事实。 */
export function extract(text: string): Facts {
  const uniq = (xs: string[]) => [...new Set(xs)]
  const code = uniq([...text.matchAll(/`([^`\n]{1,80})`/g)].map((m) => m[1].trim()).filter(Boolean))
  const env = uniq([...text.matchAll(/\b[A-Z][A-Z0-9]*(?:_[A-Z0-9]+){2,}\b/g)].map((m) => m[0]))
  const path = uniq(
    [...text.matchAll(/(?:\.evolve[\w./-]*|(?:packages|tools|shuangtian|users)\/[\w./-]+)/g)].map((m) => m[0]),
  )
  const blocks = [...text.matchAll(/```[a-z]*\n([\s\S]*?)```/g)].map((m) => m[1])
  const cmd = uniq(
    blocks
      .flatMap((b) => b.split("\n"))
      .map((l) => l.replace(/\s+#.*$/, "").trim())
      .filter((l) => l.length > 3 && /\s/.test(l)),
  )
  return { code, env, path, cmd }
}

/** 找出全部多位数字（含下划线分隔的阈值写法，如 `150_000`）。 */
export function digits(text: string): string[] {
  return [...new Set([...text.matchAll(/\b\d[\d_]{2,}\b/g)].map((m) => m[0]))].sort()
}

/** 核对：基线里的每条硬事实，新文本是否仍包含。 */
export function diff(baseline: string, next: string): { missing: { kind: string; fact: string }[]; missingNums: string[] } {
  const base = extract(baseline)
  const missing: { kind: string; fact: string }[] = []
  const check = (kind: string, list: string[]) => {
    for (const fact of list) if (!next.includes(fact)) missing.push({ kind, fact })
  }
  check("行内代码", base.code)
  check("env", base.env)
  check("路径", base.path)
  check("命令", base.cmd)
  return { missing, missingNums: digits(baseline).filter((d) => !next.includes(d)) }
}

// ── CLI（只在直接执行时跑；被测试 import 时不跑）────────────────────
if (import.meta.main) {
  const args = process.argv.slice(2)
  const arg = (name: string) => {
    const i = args.indexOf(name)
    return i >= 0 ? args[i + 1] : undefined
  }
  const baselinePath = arg("--baseline")
  const checkPath = arg("--check")
  if (!baselinePath) {
    console.error("用法: bun tools/prompt-factcheck.ts --baseline <文件> [--check <新文件>] [--numbers]")
    process.exit(2)
  }
  const baseText = readFileSync(baselinePath, "utf8")
  const base = extract(baseText)
  const total = base.code.length + base.env.length + base.path.length + base.cmd.length

  if (args.includes("--numbers")) {
    console.log(`=== 基线里的多位数字（${digits(baseText).length} 个）===`)
    console.log(digits(baseText).join(" "))
  }
  if (!checkPath) {
    console.log(`=== 基线硬事实：${total} 条 ===`)
    console.log(`  行内代码 ${base.code.length}｜env ${base.env.length}｜路径 ${base.path.length}｜命令 ${base.cmd.length}`)
    process.exit(0)
  }

  const nextText = readFileSync(checkPath, "utf8")
  const r = diff(baseText, nextText)
  console.log(`=== 核对结果 ===`)
  console.log(`基线 ${baseText.length} 字 → 新 ${nextText.length} 字（缩减 ${((1 - nextText.length / baseText.length) * 100).toFixed(1)}%）`)
  console.log(`硬事实 ${total} 条，缺失 ${r.missing.length} 条`)
  if (r.missing.length) for (const m of r.missing) console.log(`  ✗ [${m.kind}] ${m.fact}`)
  else console.log("  ✓ 全部硬事实仍在")
  console.log(`多位数字缺失 ${r.missingNums.length} 个${r.missingNums.length ? "：" + r.missingNums.join(" ") : ""}`)
  // 只报不改：缺失必须由人判「该不该缺」（事故编号该丢、阈值不该丢，字面上没区别）
  process.exit(r.missing.length ? 1 : 0)
}
