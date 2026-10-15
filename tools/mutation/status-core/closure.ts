/**
 * **判据面的依赖闭包**：把「哪些文件算在判据面里」从「四份根」扩展到「四份根 + 它们**递归引用**到的本仓源码」。
 *
 * ## 为什么需要它（缺了这半边就是一次**假绿**）
 * 指纹原先只哈希四份根文件**本身**。可判定链并不止这几份：矩阵判 `status-core.ts`，一旦
 * `status-core.ts` 开始引用别的模块（例如把取词表抽成 `status-labels.ts`、把类型抽成 `status-types.ts`），
 * **那段被引用的代码就成了判据的一部分**——它被改坏时，`status-core.ts` 的字节没变、指纹没变、
 * 闸门不红，而矩阵的「全绿」已经不再代表守住了。这是「过窄 = 假绿」的典型形态，也是本模块存在的唯一理由。
 *
 * 自动收录 import 图后，这类改动**当场把指纹弄脏**：闸门会把新成员报成「新纳入指纹面，从未被那次跑覆盖过」，
 * 于是「改了被 import 的模块同样会变行为」不再靠人记得，而是逼出一次重跑（或一次显式的豁免决定）。
 *
 * ## 边界（不划边界的话，第一个 `import` 就会把 node_modules 全拖进来）
 * 只收**本仓源码**：
 *   - **路径必须落在 `packages/` 或 `tools/` 下**（判据面四份根就在这里：两份在 `packages/web/src/files/`、
 *     两份在 `tools/mutation/status-core/`）。收 `tools/` 不是顺手——`bench.ts` / `mutants.ts` 就在那里，
 *     而它们是**真判据**（执行器与变异体清单）。
 *   - **且路径里不得出现 `node_modules` / `vendor` / `dist` / `build` / `public`**：前两个是第三方与随包资源，
 *     本仓实测 `node_modules` 下躺着 **6908** 份 `.ts`/`.tsx`、`vendor` 下 **3762** 份（`@babel/parser`、
 *     `monaco` 这类**打包进来的第三方**都在里面）——收进来只会让「改了无关的第三方」也变红，而它们
 *     从不由本仓改动的文件触发：那是纯噪声红。`dist`/`build` 是构建产物，`public` 是静态资源。
 *   - **`node:` / `bun:` 内建与裸包名（`bun:test` / `react`）一律不算**：它们不是本仓文件，
 *     改了不会让本仓某份源码变样。
 *
 * 实测：本仓 `packages/` + `tools/` 下 777 份源文件里，依赖解析落在 `vendor/` 或 `node_modules/` 内的**一份也没有**
 * ——即这条边界在当下不改变任何事实，它挡的是**将来**某份源码引了打包进来的第三方（否则那会是一次 30 秒的噪声红）。
 *
 * ## 解析口径（只认可解析的字面量，不做 AST）
 *   - `from "…"` / `import("…")` / `import "…"`：ESM 三态；
 *   - 源码里的 `"….ts"` 字面量：`readFileSync(join(import.meta.dir, "status-core.ts"))` 这种读法
 *     （矩阵正是这么读被审源码的——它是一条**真依赖**，不比 `import` 弱）；
 *   - **相对说明符**（`"./x"` / `"../x"`）按「本文件所在目录」解析；
 *   - **裸文件名 / 仓根相对路径**（`"status-core.ts"`、`"packages/web/src/files/status-core.ts"`）
 *     先按「本文件所在目录」试，**再按仓根**试——两种写法本仓都在用，而**后者是判据面里真出现的那种**
 *     （`mutants.ts` 用 `TARGET_FILE` / `MATRIX_TEST` 两个**仓根相对**常量登记被审对象与判官）。
 *     只试仓根**不**试任意目录：否则任何一个 `"….ts"` 字面量都会去扫全仓，闭包随时可能被一句注释里的
 *     示例路径撑大（那是「过宽 = 噪声红」那一侧）。
 *   - 扩展名补全 `.ts` / `.tsx` / `/index.ts` / `/index.tsx`（与 TS 的 bundler 解析习惯一致）。
 *   - **认不出的不算**：路径一旦装进变量就看不见。这与既有闸门的取舍一致（不上 AST、按「与蓄意绕过同类」处置）。
 *   - **但「认不出」不许退化成「更小的心照不宣的范围」**：解析口径在真仓里落空时（例如四份根本下
 *     零依赖），闭包会是**空集**，而空集让「只哈希根文件」与「根 + 闭包」**行为等价**——机制看着在、
 *     实际没拦。闸门侧因此有一条**只盯真仓**的断言：四份根的直接依赖**一条都不许解析不到**（见
 *     `status-mutation-gate.test.ts` 的「真仓解析零漏」一例）。
 *
 * **去注释走一个引号感知的状态机**，而不用「块注释配对」那条非贪婪正则：后者会把字符串里的注释样式文本、
 * 以及**同一行行内块注释之后**的真代码一起吞掉（实测：`const A = 1` 后跟一段行内块注释再跟 `const B = 2`，
 * 只剩 `const A = 1`）——于是一份真依赖会被看漏，而那正是「假绿」那一侧的错，不可接受。
 *
 * ## 与 stamp.ts 的关系（谁是指纹、谁只是实现）
 * 本模块**不进指纹面**（同 `stamp.ts`、`stale.ts`）：改一句文案不该要人重跑那台 30 秒的变异台。
 * 但「算法行为」由**闸门侧**的独立判据钉住（`status-mutation-gate.test.ts` 的「依赖闭包」一组）：
 * 那组测试在**临时目录里造一棵真依赖树**，验递归、验豁免、验边界（`node:` / 裸包名 /
 * `node_modules` / `vendor` / `dist` / `public` / 出仓）、验环安全——所以把本模块掏空（例如恒返回 `[]`）
 * 会当场红，而不是静默丢掉覆盖。这是本仓一贯的分工：**实现一处、判据另一处，两处不得互推**。
 */
import { readFileSync, statSync } from "node:fs"
import { join, posix } from "node:path"

/** 仓根（由本文件位置推出：`tools/mutation/status-core/` → 上溯三层），**不依赖 cwd**。 */
export const REPO_ROOT = join(import.meta.dir, "..", "..", "..")

/** 闭包只收这两个顶层目录下的源码（判据面四份根就落在这里：两份 `packages/`、两份 `tools/`）。 */
export const SCOPE_DIRS = ["packages/", "tools/"] as const

/** 边界内一律排除的目录段（第三方 / 随包资源 / 构建产物 / 静态资源——理由见文件头「边界」）。 */
export const SCOPE_EXCLUDED_DIRS = ["node_modules", "vendor", "dist", "build", "public"] as const

/** 统一路径形态：反斜杠归一到 `/`、去掉 `./` 与重复分隔符。 */
function norm(rel: string): string {
  return posix.normalize(rel.replace(/\\/g, "/"))
}

/**
 * **闭包边界判据**：一条仓根相对路径算不算「本仓源码」。
 * 纯函数（不碰文件系统），故闸门侧能拿它去量任意构造的树——边界本身因此是可测的。
 */
export function inFingerprintScope(rel: string): boolean {
  const p = norm(rel)
  if (p === "." || p.startsWith("..")) return false
  if (!SCOPE_DIRS.some((d) => p.startsWith(d))) return false
  return !p.split("/").some((seg) => (SCOPE_EXCLUDED_DIRS as readonly string[]).includes(seg))
}

/**
 * **去注释（引号感知）**：只丢注释，**字符串原样保留**——因为依赖的字面量就长在字符串里。
 *
 * 三种字面量（双引号 / 单引号 / 反引号）都按字符串走完（含 `\` 转义），故：
 *   - 字符串里的 `//` 或块注释起始标记不会被当成注释（旧的正则式会）；
 *   - 行内块注释**之后**的真代码不会被吞（旧的正则式会——那是看漏真依赖，属「假绿」侧）。
 */
function stripComments(src: string): string {
  const s = src.replace(/\r\n/g, "\n")
  let out = ""
  let i = 0
  while (i < s.length) {
    const c = s[i]!
    const n = s[i + 1]
    if (c === "/" && n === "/") {
      while (i < s.length && s[i] !== "\n") i++
      continue
    }
    if (c === "/" && n === "*") {
      i += 2
      while (i < s.length && !(s[i] === "*" && s[i + 1] === "/")) i++
      i += 2
      out += " "
      continue
    }
    if (c === '"' || c === "'" || c === "`") {
      out += c
      i++
      while (i < s.length && s[i] !== c) {
        if (s[i] === "\\") {
          out += s[i]! + (s[i + 1] ?? "")
          i += 2
          continue
        }
        out += s[i]!
        i++
      }
      out += c
      i++
      continue
    }
    out += c
    i++
  }
  return out
}

/** 从源码里取出「像依赖」的字面量（三种形态 + `.ts`/`.tsx` 文件名字面量）。 */
function scanSpecifiers(src: string): string[] {
  const body = stripComments(src)
  const out = new Set<string>()
  for (const m of body.matchAll(/\bfrom\s*["']([^"']+)["']/g)) out.add(m[1]!)
  for (const m of body.matchAll(/\bimport\s*\(\s*["']([^"']+)["']/g)) out.add(m[1]!)
  for (const m of body.matchAll(/\bimport\s+["']([^"']+)["']/g)) out.add(m[1]!)
  for (const m of body.matchAll(/["']([^"']*\.tsx?)["']/g)) out.add(m[1]!)
  return [...out]
}

/**
 * 把**一个**说明符解析成仓根相对路径；解析不到（内建 / 裸包名 / 出仓 / 越界 / 文件不存在）返回 `null`。
 *
 * **两个基准目录，按序试**（顺序本身是判据，别调换）：
 *   ① 本文件所在目录——相对说明符（`"./x"` / `"../x"`）与**裸文件名**（`"status-core.ts"`）都走这一支，
 *      这正是矩阵 `readFileSync(join(import.meta.dir, "status-core.ts"))` 的写法；
 *   ② **仓根**——只在 ①落空时试，且只对**仓根相对路径字面量**（`"packages/web/src/files/status-core.ts"`）。
 *
 * ② 为什么必须有（它修的是一个**真实的空集**，不是理论缺口）：判据面里登记被审对象与判官的方式就是
 * 两个**仓根相对**常量（`mutants.ts` 的 `TARGET_FILE` / `MATRIX_TEST`）。少了这一支，那两条依赖一律
 * 解析不到——执行器那一支整条断掉、`closure.ts` 也进不来，**真仓闭包因此恒为空集**，而空集让
 * 「只哈希根」与「根 + 闭包」行为等价（机制在、但没拦）。
 *
 * ② 为什么**不**试「任意祖先目录」：那会让闭包随「某份文件恰好放在哪一层」而变，也会让一句注释里的
 * 示例路径把无关文件拖进面里——而**过宽 = 噪声红**（改一个无关文件也要等 30 秒），本仓明确不要那一侧。
 */
function resolveSpecifier(fromRel: string, spec: string, base: string): string | null {
  if (spec.startsWith("node:") || spec.startsWith("bun:")) return null
  const isRelative = spec.startsWith("./") || spec.startsWith("../")
  // 裸说明符只有长得像文件名（带 .ts/.tsx）才可能是本仓文件；`react` / `zod/v4` 这类一律不算
  if (!isRelative && !/\.tsx?$/.test(spec)) return null
  const bases = isRelative ? [posix.dirname(norm(fromRel))] : [posix.dirname(norm(fromRel)), "."]
  for (const dir of bases) {
    const head = norm(posix.join(dir, spec))
    // 出仓（`..` 开头）与「原地打转」（`.`）都不是本仓文件
    if (head === "." || head.startsWith("..")) continue
    const candidates = [head, `${head}.ts`, `${head}.tsx`, posix.join(head, "index.ts"), posix.join(head, "index.tsx")]
    for (const c of candidates) {
      const rel = norm(c)
      if (!inFingerprintScope(rel)) continue
      try {
        if (statSync(join(base, rel)).isFile()) return rel
      } catch {
        // 不存在 → 试下一个候选
      }
    }
  }
  return null
}

/**
 * 一份文件**真正引用的本仓源码**（直接依赖，去重且已排序）。
 *
 * `base` 默认仓根；闸门侧的边界测试传自己的临时目录，于是这套解析口径连同边界一起可测。
 * 文件读不到（不存在 / 二进制）时返回 `[]`——闸门另有「成员必须真实存在」的检查，不靠这里报错。
 */
export function staticDeps(rel: string, base: string = REPO_ROOT): string[] {
  let src: string
  try {
    src = readFileSync(join(base, norm(rel)), "utf8")
  } catch {
    return []
  }
  const out: string[] = []
  for (const spec of scanSpecifiers(src)) {
    const hit = resolveSpecifier(rel, spec, base)
    if (hit) out.push(hit)
  }
  return [...new Set(out)].sort()
}

/**
 * **依赖闭包**：从若干「根」出发，递归收录它们引用到的本仓源码（不含根自身）。
 *
 * - **豁免只剪「被点名的那一份」**（默认；`pruneSubtree` 可改成整棵子树一起剪）：
 *   被豁免者自己不进闭包、也**不再往下走**（省一层噪声）；但它只要被面内**另一条**路径引用到，
 *   就仍然照常进面、并照常往下递归。
 *
 *   为什么默认必须是「只剪点名的那份」（这条修的是一个**真实的空集**，不是洁癖）：上一版是「被豁免者
 *   连子树一起剪」，于是 `stamp.ts` 被豁免时，它引用的 `closure.ts`（闭包算法本体）**和无条件递归
 *   完全等价**——而 `closure.ts` 又是执行器 `bench.ts` 的依赖。结果是四份根里**唯一**能收到的依赖
 *   （`bench.ts → closure.ts`）被这张豁免表的**间接效果**剪掉了，真仓闭包恒为空集：机制在，但一次都
 *   没生效过。「豁免」的语义本该是「这份不参与判定」（一个**逐文件**的判断），不该顺手把依赖图也剪短
 *   ——依赖图短了，判据面就小了，而判据面小了是**假绿**那一侧（本仓明说不可接受的那一侧）。
 *   保留整树剪枝开关是为了让「连树剪」这个语义仍可被合成树测到（见闸门「豁免是连子树一起剪的」一组），
 *   也为了哪天真需要它时不必重写一遍遍历。
 * - **环安全**：`seen` 集合既挡重复也断环（本仓当下无环，但依赖图里出现环是正常的，不能因此挂死）。
 * - **确定性**：返回排序后的数组——指纹的键序要逐字节稳定（同 `stamp.ts` 的「内容确定」纪律）。
 */
export function depClosure(
  roots: readonly string[],
  exempt: Iterable<string> = [],
  base: string = REPO_ROOT,
  opts: { pruneSubtree?: boolean } = {},
): string[] {
  const pruneSubtree = opts.pruneSubtree ?? false
  const skip = new Set([...exempt].map(norm))
  const seen = new Set(roots.map(norm))
  const queue = [...seen]
  const out: string[] = []
  while (queue.length > 0) {
    const rel = queue.shift()!
    for (const dep of staticDeps(rel, base)) {
      if (seen.has(dep)) continue
      seen.add(dep)
      if (skip.has(dep)) {
        // 只剪点名的那一份时，仍要往下走一步：被豁免者引用的东西**不是**被豁免者，
        // 它们该不该进面取决于**谁引用它们**——上面那个 `seen` 已经保证只走一遍。
        if (!pruneSubtree) queue.push(dep)
        continue
      }
      out.push(dep)
      queue.push(dep)
    }
  }
  return out.sort()
}

/** 在 `among` 这些文件里，谁引用了 `rel`（给报告用：让「它为什么在指纹里」当场可读）。 */
export function importersOf(rel: string, among: readonly string[], base: string = REPO_ROOT): string[] {
  const target = norm(rel)
  return among.filter((p) => staticDeps(p, base).includes(target)).sort()
}
