/**
 * 待办弹窗窄屏排版契约（手机端「待办内容变成一条竖线」的回归护栏）。
 *
 * 事故形态（2026-11-06 实测，390×844 粗指针）：待办行 `.todo-item` 不换行，
 * 触屏下 7 个 36px 操作按钮横排占 264px，把 `.todo-body`（正文）挤到 **27px** 宽——
 * 每个汉字单独一行，整条待办看起来像一根竖线。DOM 与数据都完全正常，
 * 只有真实布局能暴露，所以**必须**在 CSS 层用静态断言守住，而不是靠人眼。
 *
 * 守三件事：
 * ① 窄屏/粗指针下操作区整行下沉（`flex-basis: 100%`）——否则正文拿不到整行宽；
 * ② 同一断点下正文有**宽度下限**（min-width）——父项 flex-wrap 才会先让操作区换行；
 * ③ 上面两条都带 `.todo-pop` 作用域——`.todo-item`/`.todo-body` 在 chat.css 里
 *    也被会话内待办卡片使用（同名类），无作用域写法会把弹窗规则泄漏到卡片上。
 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const CSS = readFileSync(join(import.meta.dir, "css", "overlays.css"), "utf8")

/** 剥掉注释：注释里写了规则名不算数（否则断言永远为真）。 */
const stripped = CSS.replace(/\/\*[\s\S]*?\*\//g, "")

/** 从 `from` 处的 `{` 开始按大括号配对取出块正文（支持嵌套）；找不到返回 null。 */
function blockBody(text: string, openBrace: number): string | null {
  let depth = 0
  for (let k = openBrace; k < text.length; k++) {
    if (text[k] === "{") depth++
    else if (text[k] === "}") {
      depth--
      if (depth === 0) return text.slice(openBrace + 1, k)
    }
  }
  return null
}

/** 取某个 `@media` 块的正文（找不到返回空串）。 */
function mediaBlock(head: string): string {
  const i = stripped.indexOf(head)
  if (i < 0) return ""
  const open = stripped.indexOf("{", i)
  return open < 0 ? "" : (blockBody(stripped, open) ?? "")
}

/** 删除全部 `@media` 块，只留顶层规则（按 `@media` 出现处逐个配对删除）。 */
function topLevel(): string {
  let out = ""
  let i = 0
  while (i < stripped.length) {
    const at = stripped.indexOf("@media", i)
    if (at < 0) {
      out += stripped.slice(i)
      break
    }
    out += stripped.slice(i, at)
    const open = stripped.indexOf("{", at)
    if (open < 0) break
    const body = blockBody(stripped, open)
    if (body === null) break
    // body 是内部文本，跳过它后面那个 `}`
    i = open + 1 + body.length + 1
  }
  return out
}

/**
 * 取块内**选择器精确等于** selector 的声明。
 * 必须精确：`ruleOf(块, ".todo-actions")` 不能把 `.todo-pop .todo-actions` 算进来
 * （那正是「作用域是否写了」这条断言要区分的东西）。
 */
function ruleOf(block: string, selector: string): string {
  for (const m of block.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    const sels = m[1]!.split(",").map((s) => s.trim().replace(/\s+/g, " "))
    if (sels.includes(selector)) return m[2] ?? ""
  }
  return ""
}

const NARROW = mediaBlock("@media (max-width: 640px), (pointer: coarse)")

describe("待办弹窗窄屏排版（竖线事故护栏）", () => {
  test("窄屏/粗指针断点存在（max-width: 640px 或粗指针）", () => {
    expect(NARROW.length).toBeGreaterThan(0)
  })

  test("① 操作区整行下沉：flex-basis 100%（正文才拿得到整行宽）", () => {
    expect(ruleOf(NARROW, ".todo-pop .todo-actions")).toContain("flex: 1 1 100%")
  })

  test("① 操作区允许折行（320px 视口下 7 个 36px 按钮共 264px，一行放不下）", () => {
    expect(ruleOf(NARROW, ".todo-pop .todo-actions")).toContain("flex-wrap: wrap")
  })

  test("② 正文有宽度下限：被操作按钮挤压时先让操作区换行，而不是把正文压成竖线", () => {
    const m = ruleOf(NARROW, ".todo-pop .todo-body").match(/min-width:\s*(\d+(?:\.\d+)?)em/)
    expect(m).not.toBeNull()
    // 12em × 13.5px ≈ 162px ≈ 12 个汉字/行（事故现场 27px ≈ 2 字符/行）
    expect(Number(m![1])).toBeGreaterThanOrEqual(10)
  })

  test("③ 两条规则都带 .todo-pop 作用域（同名类在 chat.css 里也用于会话内待办卡片）", () => {
    expect(ruleOf(NARROW, ".todo-actions")).toBe("")
    expect(ruleOf(NARROW, ".todo-body")).toBe("")
    expect(topLevel()).toContain(".todo-pop .todo-item { flex-wrap: wrap; }")
  })

  test("④ 桌面宽度不受影响：正文 min-width 只在窄屏断点，顶层仍为 0", () => {
    const decl = ruleOf(topLevel(), ".todo-body")
    expect(decl).toContain("min-width: 0")
    expect(decl).not.toContain("em")
  })
})
