/**
 * 样式契约：`.active` 这类「点亮」类必须有对应样式规则。
 *
 * 存在的理由（不是假想的）：`classList.toggle("active", …)` 在 JS 侧永远「成功」——
 * 类名加上了、断言也能过，但 CSS 里没有对应规则时**用户什么也看不见**：
 * 开关看不出开没开、选中行看不出选没选中，比没有这个控件更糟（用户会当成坏了）。
 * 这类问题光测 DOM 也抓不全（要读 computed style、还得先有真实布局）。
 *
 * 这里守两道，各自都不依赖人工维护：
 * ① **模板条件追加**（`` class: `fw-x${cond ? " active" : ""}` ``）——前缀即宿主类，
 *    静态可判定且无误报，新增这种写法会被自动覆盖；
 * ② **已知开关清单**——防「误删样式」（有人在清理 CSS 时删掉 .fw-chip.active，这里会红）。
 * 变量宿主（`setToggle(btn, …)` 之类）静态判定不了，不在这里硬扫：那种写法靠浏览器实测覆盖，
 * 与其写一条只会空转的检查（测试全绿但什么也没守着），不如明确不写。
 */
import { describe, expect, test } from "bun:test"
import { readdirSync, readFileSync } from "node:fs"
import { join } from "node:path"

const SRC = join(import.meta.dir, "..")
const CSS_DIR = join(SRC, "css")

/** 全部样式表里出现过 `.X.active` 的类名集合（跨文件：终端页签的样式在 terminal.css）。 */
function activeClassSet(css: string): Set<string> {
  const set = new Set<string>()
  for (const m of css.matchAll(/\.([a-z][a-z0-9-]*)\.active(?![\w-])/g)) set.add(m[1]!)
  return set
}

/** 递归列出源码文件（跳过测试与外部产物）。 */
function sourceFiles(dir: string): string[] {
  const out: string[] = []
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name)
    if (e.isDirectory()) {
      if (e.name === "vendor" || e.name === "node_modules") continue
      out.push(...sourceFiles(p))
    } else if (e.name.endsWith(".ts") && !e.name.endsWith(".test.ts")) {
      out.push(p)
    }
  }
  return out
}

/** 全部样式表的合并文本（**剥掉注释**：注释里提到 `.fw-x.active` 不算规则，否则断言永远为真）。 */
function allCss(): string {
  return readdirSync(CSS_DIR)
    .filter((f) => f.endsWith(".css"))
    .map((f) => readFileSync(join(CSS_DIR, f), "utf8"))
    .join("\n")
    .replace(/\/\*[\s\S]*?\*\//g, "")
}

describe("样式契约：点亮的 active 类必须有样式规则", () => {
  const known = activeClassSet(allCss())

  test("模板里条件追加的 active（前缀即宿主类）都有样式", () => {
    const violations: string[] = []
    for (const file of sourceFiles(SRC)) {
      const lines = readFileSync(file, "utf8").split("\n")
      lines.forEach((line, i) => {
        // `` class: `fw-chip${… ? " active" : ""}` `` → 捕获组 1 = 前缀（可含前导类名）
        for (const m of line.matchAll(/class:\s*`([^`]*)\$\{[^}]*\?\s*" active"\s*:\s*""/g)) {
          const host = m[1]!.trim().split(/\s+/).pop() ?? ""
          if (!host || host.includes("${")) continue
          if (!known.has(host)) violations.push(`${file.replace(SRC, "src")}:${i + 1}  ${host}`)
        }
      })
    }
    expect(violations).toEqual([])
  })

  test("已知开关类的样式不能被删掉", () => {
    // 文件工作台里会用 `.active` 表达「按下 / 选中」的控件（新增控件时把类名加进来）
    const switches = ["fw-chip", "fw-btn", "fw-log-row", "fw-ref-row", "fw-tab-view", "fw-term-tab", "fw-rail-btn", "fw-tree-row"]
    const missing = switches.filter((c) => !known.has(c))
    expect(missing).toEqual([])
  })

  test("sticky 元素必须有垫实的背景（否则滚动内容会透出来）", () => {
    /* 为什么单列一条：半透明主题（acrylic 下 --bg-elev 约 0.82 不透明）里，
     * sticky 元素只用单层背景时，滚过去的行会**从它底下透出来**，看着像画错了。
     * 约定是叠两层（“双背景垫实”，与 wheel.css 的扇形按钮同一手法）。
     * 若某元素确实是不透明背景（写死的实色），把它加进 allowlist —— 不猜主题令牌的透明度。 */
    const allowlist: string[] = []
    const css = readFileSync(join(CSS_DIR, "files.css"), "utf8")
    const violations: string[] = []
    for (const [, sel, body] of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
      if (!/position:\s*sticky/.test(body!)) continue
      const selector = sel!.trim().split("\n").pop()!.trim()
      if (allowlist.includes(selector)) continue
      const layers = (body!.match(/linear-gradient\(/g) ?? []).length
      if (layers < 2) violations.push(`${selector}（背景层数 ${layers}，需叠两层或用不透明实色并加 allowlist）`)
    }
    expect(violations).toEqual([])
  })
})

/**
 * 样式契约 helper：取某个选择器（逗号分隔中的一员）声明的**全部**声明体，拼接返回；没命中返回空串。
 * 全部样式表合并、**剥掉注释**后按选择器查（注释里提到某规则不算数）。
 * 同一选择器可出现在多条规则里（后面那条常是增补，如 `.fw-commit-btns` 的 `margin-left`）——
 * 只看第一条会漏掉这些增补，把它们当成不存在。
 */
const CONTRACT_CSS = allCss()
function ruleBody(selector: string): string {
  const out: string[] = []
  for (const [, sel, body] of CONTRACT_CSS.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    if (sel!.split(",").map((s) => s.trim()).includes(selector)) out.push(body!)
  }
  return out.join("\n")
}

/**
 * 源码断言 helper：**先剥注释再扫**。本仓惯于把「历史缺陷」的原文写进注释当反面教材，
 * 不剥的话 `not.toContain("旧文案")` 会被自己的注释碰红——那时这条断言就只剩「删注释」的作用。
 */
function stripComments(source: string): string {
  return source.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^[ \t]*\/\/.*$/gm, "")
}

/** 取出 `call(` 的**每一处**实参文本（按括号配平截取，跨行也认）；一处都没有返回空数组。 */
function callArgs(source: string, call: string): string[] {
  const out: string[] = []
  for (let i = source.indexOf(call); i >= 0; i = source.indexOf(call, i + call.length)) {
    const open = i + call.length - 1
    let depth = 0
    let j = open
    for (; j < source.length; j++) {
      if (source[j] === "(") depth++
      else if (source[j] === ")") {
        depth--
        if (depth === 0) break
      }
    }
    out.push(source.slice(open + 1, j))
  }
  return out
}

/**
 * 源码扫描的**归一化**：剥注释 + 抹平行尾分号与空白。
 *
 * 为什么要归一而不直接看原文：本仓代码常被 prettier 折成多行、且**不写行尾分号**（ASI），
 * 而「某段代码在不在」与这两件事无关——把分号或换行写死进断言，就等于给好代码埋一个
 * 与判据无关的碰红点（实测踩过：写死分号的那一版，把调用折成四行的良性重构就被误杀）。
 */
function normalized(source: string): string {
  return stripComments(source)
    .replace(/^[ \t]*import\b.*$/gm, "")
    .replace(/[ \t]*;[ \t]*$/gm, "")
    .replace(/\s+/g, " ")
}

/**
 * 「由唯一取数口 `writabilityOf` 派生」的标识符集合——**逐语句的数据流**，不是字面包含。
 *
 * 为什么要到语句粒度：`saveItem({ dirty: t.dirty, refusal })` 里同时出现两个标识符——
 * `dirty` 本来就是标签自己的字段（该自取），`refusal` 才是必须来自判据的那个。整段匹配会把
 * 好代码判红（实测：把 `writabilityOf(t)` 提成局部变量、或多行排版，旧断言就报“输入未收口”，
 * 而判据来源一个字没改）。这里对每个显式写出的实参**逐个**判定，并容许把取数口提出去。
 *
 * 闭包上限 4 轮：派生链在本仓最多两跳，够用且必然收敛。
 */
function writabilitySourced(source: string): Set<string> {
  const assigned = [...source.matchAll(/(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*([^;\n]*)/g)]
  const sourced = new Set<string>()
  for (let pass = 0; pass < 4; pass++) {
    for (const m of assigned) {
      const name = m[1]!
      if (sourced.has(name)) continue
      const rhs = m[2]!
      if (rhs.includes("writabilityOf") || [...sourced].some((v) => new RegExp(`\\b${v}\\b`).test(rhs))) sourced.add(name)
    }
  }
  return sourced
}

/** 实参去掉尾逗号后的形态。 */
const trimmedArg = (arg: string) => arg.trim().replace(/,\s*$/, "").trim()

/** 实参是否**就是一次** `writabilityOf(…)` 调用（配平的右括号就在末尾才算）。 */
function isWritabilityCall(arg: string): boolean {
  const t = trimmedArg(arg)
  if (!t.startsWith("writabilityOf(")) return false
  let depth = 0
  for (let i = t.indexOf("("); i < t.length; i++) {
    if (t[i] === "(") depth++
    else if (t[i] === ")") {
      depth--
      if (depth === 0) return i === t.length - 1
    }
  }
  return false
}

/**
 * **对象实参的值**是否由取数口派生：引用了 `writabilityOf`，或引用了闭包判为派生的变量。
 * 用于 `{ readOnly: … }` / `{ refusal: … }` 这类值的判定。
 */
function valueFromPort(value: string, sourced: Set<string>): boolean {
  if (value.includes("writabilityOf")) return true
  return [...sourced].some((v) => new RegExp(`\\b${v}\\b`).test(value))
}

/**
 * **位置实参**是否就是取数口（或它的具名变量）：只收「整个实参就是一次取数口调用」与「整个实参就是一个
 * 派生的具名变量」两种形态。
 *
 * 为什么这里不能像值那样用「包含」：`saveRefusal(writabilityOf(tab).rootWritable === false ? … : writabilityOf(tab))`
 * 也含这个词，但那是在取数口结果上**又加了一道自己的判**——已经不是「一组输入、一个函数」了。
 * 实测：用「包含」判定时这个退化漏检。
 */
function argFromPort(arg: string, sourced: Set<string>): boolean {
  if (isWritabilityCall(arg)) return true
  const t = trimmedArg(arg)
  return /^[A-Za-z_$][\w$]*$/.test(t) && sourced.has(t)
}

/**
 * 从一个调用实参里取出**顶层**条目（`{ dirty: t.dirty, refusal }` → `[{key:"dirty",value:"t.dirty"},{key:"refusal",value:"refusal"}]`）。
 *
 * 取**值**而不是整段实参——这是关键：`{ mode: t.mode, readOnly: null }` 与
 * `{ mode: t.mode, readOnly: readOnlyReason(writabilityOf(t)) }` 整段看起来都“含 readOnly”，
 * 只按整段是否出现某个变量名判定，会把前者（真的另拼了输入）放过去（实测漏检）。
 */
function argEntries(arg: string): Array<{ key: string; value: string }> {
  const t0 = trimmedArg(arg)
  // **必须整个实参就是一个对象字面量**才算对象形（否则像 `a ? {…} : b` 这种里层的 `{` 会被误当成对象）
  if (!t0.startsWith("{")) return []
  let depth = 0
  let close = -1
  for (let i = 0; i < t0.length; i++) {
    if ("([{".includes(t0[i]!)) depth++
    else if (")]}".includes(t0[i]!)) {
      depth--
      if (depth === 0) {
        close = i
        break
      }
    }
  }
  if (close !== t0.length - 1) return []
  const inner = t0.slice(1, close)
  const out: Array<{ key: string; value: string }> = []
  let d = 0
  let cur = ""
  const push = () => {
    const t = cur.trim()
    cur = ""
    if (!t) return
    // 先试显式键（可带引号）；再试简写。顺序不能反——否则 `readOnly: null` 会把值 `null` 误当键名
    const m = t.match(/^["']?([A-Za-z_$][\w$]*)["']?\s*:\s*([\s\S]*)$/)
    if (m) out.push({ key: m[1]!, value: m[2]!.trim() })
    else if (/^[A-Za-z_$][\w$]*$/.test(t)) out.push({ key: t, value: t })
  }
  for (const ch of inner) {
    if ("([{<".includes(ch)) d++
    else if (")\]}>`".includes(ch)) d--
    if (ch === "," && d === 0) push()
    else cur += ch
  }
  push()
  return out
}

/** 取数口的三个原始输入字段名：消费点若**自己现拼**它们，就已经不是“一组输入”了。 */
const PORT_FIELDS = ["statEditable", "truncated", "rootWritable"]

/**
 * 样式契约：状态栏「查看 / 编辑」模式格的 **pri1 永不隐**，且只读原因的措辞只在一处。
 *
 * 存在的理由（两件都不报错、只在窄面板里静默发生）：
 * ① 状态栏是 flex + `overflow: hidden`、条目右对齐且 nowrap——**宽度不够时被裁的是靠后的格**。
 *   模式格回答“我现在能不能改这个文件”，在窄面板（分屏常态）里被裁掉就只剩用户自己猜；
 *   而“它被裁了”在任何单测与页面报错里都看不出来，只有缩到 700px 以下才看得到。
 * ② 不可写的**原因文案**（`status-core` 的 REASON）被三处消费：状态栏徽标的 title、标签栏编辑按钮的
 *   title、`saveTab` 的拒绝 toast。任一处另写一份，同一屏幕上就会出现两种解释——用户只能猜。
 *   这里用**存在性 + 结构**检查（`ruleBody` / 源码扫描）钉住，不依赖真实排版。
 */
describe("样式契约：模式格 pri1 恒在 + 不可写原因单一来源", () => {
  /** 把 CSS 里所有规则按 `@media` 块归位（顶层媒体查询不嵌套，足够本仓使用）。 */
  function mediaBlocks(css: string): Array<{ cond: string; body: string }> {
    const out: Array<{ cond: string; body: string }> = []
    const re = /@media([^{]+)\{([\s\S]*?)\n\}/g
    for (const m of css.matchAll(re)) out.push({ cond: m[1]!.trim(), body: m[2]! })
    return out
  }

  test("退让规则只点名 pri3 / pri2，没有任何「pri1 在窄屏被隐」的写法", () => {
    const media = mediaBlocks(allCss()).filter((b) => /max-width/.test(b.cond))
    expect(media.length, "一个宽度退让块都没有？退让规则被误删了").toBeGreaterThan(0)
    for (const b of media) {
      const hides = [...b.body.matchAll(/\[data-pri="(\d)"\]\s*\{[^}]*display:\s*none/g)].map((m) => m[1]!)
      expect(hides, `@media ${b.cond} 隐掉了 pri1（模式格/根名格必须恒在）`).not.toContain("1")
    }
  })

  test("模式格带自查标记（否则 DOM 层面无从断言这一格是 pri1）", () => {
    // 只降格式断言：归一空白后找 `slotEl.dataset.fwModeSlot = slot.text` 这条赋值，
    // 不锁多行 / 分号 / 换行位置——排版变化不该碰红契约测试。
    const main = normalized(readFileSync(join(SRC, "files", "main.ts"), "utf8"))
    expect(main).toContain("dataset.fwModeSlot = slot.text")  })

  test("不可写原因的文案只有 status-core 一处（按钮/徽标/保存拒绝共用 REASON）", () => {
    // **先剔掉注释**再扫：本仓惯于把“历史缺陷”原文写进注释当反面教材（如 renderTabActions 上方那段的
    // 「该文件类型不支持编辑」），不剔的话这条断言会被自己的注释碰红——那时它就只剩“删注释”的作用。
    const main = stripComments(readFileSync(join(SRC, "files", "main.ts"), "utf8"))
    expect(main).toContain("readOnlyReason(")
    expect(main).toContain("editButton(")
    // 历史文案：不可写时一律说“类型不支持编辑”——三个原因（根只读/截断/类型）三种下一步，
    // 一律归到“类型”会让人去扩展名上找原因。它只允许出现在 status-core 的 REASON 里。
    expect(main, "main.ts 里不得再自行拼不可写文案（口径会与状态栏漂移）").not.toContain("该文件类型不支持编辑")
    expect(main, "saveTab 的拒绝原因应取 readOnlyWhy 而非自写一份").not.toContain("当前为只读模式（GEBAI_FS_WRITE=false），无法保存")
  })

  /**
   * 三口径的**接线契约**（行为契约在 status-core.test.ts 的「三口径一致性」块）。
   *
   * 为什么要有：行为测试只能证明**函数**自洽，证明不了 main.ts 真的调了它们——有人把某处改回
   * 「自己取 `t.stat.editable`」时，status-core 的单测依然全绿（函数没变），而界面又回到了三处各说各话
   * 的旧样子。本仓不跑 DOM 集成测试（main.ts 一 import 就绑真 DOM 与网络），所以只能扫源码钉住接线。
   *
   * **本用例是重构友好版**（复核时实测重写）：只断言两件事，两件都与「代码长什么样」无关——
   *   ① 取数口 `writabilityOf` 仍从**同一组三个来源**取（`stat.editable` / `truncated` / 根清单）；
   *   ② `modeSlot` / `editButton` / `saveItem` / `readOnlyReason` / `saveRefusal` 的**每一处调用**，
   *      其语义上该来自取数口的实参（`readOnly` / `refusal` / `o`）确实来自取数口（直接或经派生变量）。
   *
   * **旧的写法与它的两个毛病**（本单实测，供后人别再走回去）：原用字面片断
   * （`toContain("const refusal = saveRefusal(writabilityOf(")` + 数 `writabilityOf` 出现次数 ≥ 4）——
   *   ①「名字出现过 ≠ 真的接了」：把编辑按钮改回只判 `t.stat.editable` 时，`writabilityOf` 仍在保存那行
   *      出现、计数仍 ≥ 4，**全绿放行**，而界面已经退回旧口径；
   *   ②「好代码误杀」：把取数口提成局部变量（`const w2 = writabilityOf(t); saveRefusal(w2)`）是正常重构
   *      却会碰红（实测：换行、硬括号、键名加引号、键顺序对调都会误杀）。
   *
   * 为什么不做「位置敏感性」断言（“modeSlot 的第一个实参必须是 writabilityOf(t)”）：实测那类正则
   * 会把**局部变量提取**这类重构全判红（改前 vs 改后：良性重构误报 6 例）。而「拆开取数口」这种
   * 真退化，`WritabilityInput` 是个**具名结构类型**——拆开时 TS 会报“参数类型不匹配”，编译期就拦住了，
   * 不必再拿脆弱正则去重。
   */
  test("三处消费都接在 status-core 的同一判据上（徽标 / 按钮 / 保存拒绝）", () => {
    // 注释必须先剥：本块的反面教材文案（旧口径原文）就写在 main.ts 的注释里
    const main = stripComments(readFileSync(join(SRC, "files", "main.ts"), "utf8"))
    const SP = String.raw`\s*`
    // ① 单一取数口的定义仍在，且确实从三个来源取——缺一项就不是同一组输入了
    expect(main, "取数口 writabilityOf 不再从三个来源取（三处又会各拼一份输入）")
      .toMatch(new RegExp(String.raw`function writabilityOf\(\w+: Tab\): WritabilityInput` + SP + String.raw`\{[^}]*statEditable: \w+\.stat\?\.editable[^}]*truncated: \w+\.truncated[^}]*rootWritable: state\.rootsResp\?\.writable`))
    // ② 逐调用点判「输入从哪来」——不看长相，只看数据流（详见上方说明）
    const sourced = writabilitySourced(main)
    const loose: string[] = []
    /** 只对「语义上必须是取数口派生」的显式键做判定；键不在名单里（如 `dirty` 由标签自取）就不管。 */
    const check = (label: string, call: string, mustBeSourced: string[]) => {
      const args = callArgs(main, call)
      // 空集会让下面的循环空转成「静默通过」——改名或搬走时必须显式来改这份清单
      expect(args.length, `${call}… 在 main.ts 里一处调用点都找不到（改名或搬走了？请同步本清单）`).toBeGreaterThan(0)
      for (const arg of args) {
        const entries = argEntries(arg)
        // 位置参数形态（`readOnlyReason(o)` / `saveRefusal(o)` 就是这样被调的）：整个实参必须是取数口
        if (entries.length === 0) {
          if (!argFromPort(arg, sourced)) loose.push(`${label}：实参未从 writabilityOf 取（「${trimmedArg(arg).replace(/\s+/g, " ").slice(0, 60)}」）`)
          continue
        }
        // 消费点**自己现拼取数口的三个字段**＝又在自己拼输入了（实测过的退化形态）
        const handRolled = entries.filter((e) => PORT_FIELDS.includes(e.key))
        if (handRolled.length > 0) loose.push(`${label}：消费点自己现拼了输入字段（${handRolled.map((e) => e.key).join("/")}）——应整体用 writabilityOf 的结果`)
        // 对象参数形态：逐个看**值**（不看键——键名里也有 readOnly/refusal，会把自己的值盖住）
        for (const { key: k, value } of entries) {
          if (!mustBeSourced.includes(k)) continue
          if (!valueFromPort(value, sourced)) loose.push(`${label}：${k} 未从 writabilityOf 取（值「${value.replace(/\s+/g, " ").slice(0, 60)}」）`)
        }
      }
    }
    check("模式格徽标", "modeSlot(", ["readOnly"])
    check("标签栏编辑按钮", "editButton(", ["readOnly"])
    check("轮盘保存项", "saveItem(", ["refusal"])
    check("展示侧判据", "readOnlyReason(", ["o"])
    check("保存侧判据", "saveRefusal(", ["o"])
    expect(loose, "同一组事实被各拼了一份输入——三处又会算出两个结论").toEqual([])
    // ③ 旧口径与旧文案的**读取**一旦回来就报错（断言「读」的形态；注释已剥，不会误伤注释里引用的旧代码）
    expect(main, "saveTab 不得再直接看 rootsResp.writable（截断/类型不可编辑会被放行）").not.toMatch(/rootsResp\??\.writable\s*!==\s*true/)
    expect(main, "轮盘保存项不得再自行拼「存不回」文案").not.toContain("`保存（Ctrl+S）· 存不回：${readOnlyWhy")
  })
})

/**
 * 样式契约：标签栏溢出时**标签条自己滚**、动作区留在右端。
 *
 * 存在的理由：这件事由三条规则共同成立——标签条可被压缩且可横向滚动（`flex: 0 1 auto` +
 * `min-width: 0` + `overflow-x: auto`）、标签本身不被压缩（`flex: none`）、动作区不被压缩（`flex: none`）。
 * 删掉任何一条都会**静默**退回旧症状：标签全被压成一排只剩省略号的窄条，或者右侧那两个按钮被顶出
 * `.fw-tabbar` 的 `overflow: hidden` 之外——看不见也点不到，而页面本身并不报错。
 * 布局类问题单测测不到（要真实排版），但「规则被删」这件事测得到。
 */
describe("样式契约：标签栏溢出滚动", () => {
  test("标签条可压缩、可横向滚动", () => {
    const body = ruleBody(".fw-tabstrip")
    expect(body).toMatch(/overflow-x:\s*auto/)
    expect(body).toMatch(/min-width:\s*0/)
    expect(body).toMatch(/flex:\s*0 1 auto/)
  })

  test("标签与动作区都不许被压缩（宽度不够时只能是标签条滚）", () => {
    expect(ruleBody(".fw-tab")).toMatch(/flex:\s*none/)
    expect(ruleBody(".fw-tabbar-actions")).toMatch(/flex:\s*none/)
  })

  test("标签条不画横向滚动条（34px 的标签栏容不下 11px 的条，滚法由滚轮与自动滚入视野承担）", () => {
    expect(ruleBody(".fw-page .fw-tabstrip")).toMatch(/scrollbar-width:\s*none/)
    expect(ruleBody(".fw-page .fw-tabstrip::-webkit-scrollbar")).toMatch(/display:\s*none/)
  })

  test("标签只挂在标签条上（重新挂回 tabbar 就会退回“按钮被挤出可视区”）", () => {
    const src = readFileSync(join(SRC, "files", "main.ts"), "utf8")
    expect(src).toContain('class: "fw-tabstrip"')
    expect(src).toContain("tabstrip.appendChild(el)")
    expect(src).not.toMatch(/tabbar\.appendChild/)
  })
})

/**
 * 样式契约：轮盘容器不吃指针事件（展开时不挡下方控件）。
 *
 * 容器是一块覆盖扇形边界盒的实心矩形，盒下面往往就是标签栏/消息区里的真实控件（入口在界面右上角、
 * 扇形向下左展开，盒子自然压住它们）。容器一旦可命中，展开期间那些控件就都点不到——点击落在容器上，
 * 既不触发下方按钮、也不算“点了外面”，页面不报错、只是「点了没反应」。保持区的判定因此改由
 * wheel-core 的 pointermove 按坐标做，扇形按钮自己恢复可命中。
 */
describe("样式契约：轮盘容器不吃指针事件", () => {
  test("容器 pointer-events: none，展开态也不恢复可命中", () => {
    expect(ruleBody(".wheel")).toMatch(/pointer-events:\s*none/)
    expect(ruleBody(".wheel.open")).not.toMatch(/pointer-events:\s*auto/)
  })

  test("扇形按钮自己恢复可命中（pointer-events 可继承，不恢复则轮盘自己的按钮都点不到）", () => {
    expect(ruleBody(".wheel button.wheel-item")).toMatch(/pointer-events:\s*auto/)
  })
})

/**
 * 样式契约：分支栏的宽度下限来自工具条按钮组的**实测**宽度。
 *
 * 三件事合起来才成立：CSS 的下限读变量（JS 每次渲染重建工具条时写入）、工具条里的固定项
 * 不被压缩（否则量到的是「已经被压扁的宽度」，下限跟着缩、按钮实际还是会被裁）、
 * 状态文本可省略（它是唯一该让路的东西）。任一条被改掉都会静默退回旧症状：
 * 栏拖窄后按钮被裁掉半个或折行，而页面本身不报错。
 */
describe("样式契约：分支栏下限取工具条宽度", () => {
  test("下限读 CSS 变量（写死的数字在增删按钮后会静默失效）", () => {
    expect(ruleBody('.fw-git-col[data-col="refs"]')).toMatch(/min-width:\s*var\(--git-col-a-min/)
  })

  test("工具条里的按钮不许被压缩", () => {
    expect(ruleBody(".fw-git-subbar > .fw-btn")).toMatch(/flex:\s*none/)
    expect(ruleBody(".fw-git-subbar > .fw-icon-btn")).toMatch(/flex:\s*none/)
    expect(ruleBody(".fw-remote-actions")).toMatch(/flex:\s*none/)
  })

  test("状态文本是让路的那一个（省略号收尾，而不是把按钮挤出去）", () => {
    const body = ruleBody(".fw-git-subbar .fw-info")
    expect(body).toMatch(/overflow:\s*hidden/)
    expect(body).toMatch(/text-overflow:\s*ellipsis/)
  })

  test("远程行：名称与地址分行（列容器可压缩，地址可断行而不被截断）", () => {
    const main = ruleBody(".fw-remote-main")
    expect(main).toMatch(/flex-direction:\s*column/)
    expect(main).toMatch(/min-width:\s*0/)
    // 地址是无空格长串：不写 overflow-wrap 就断不开（要么溢出、要么退回省略号截断，白分这一行）
    expect(ruleBody(".fw-remote-url")).toMatch(/overflow-wrap:\s*anywhere/)
  })
})

/**
 * 样式契约：变更面板（左栏）的头部与提交框。
 *
 * 三件事各自都是“删一行就静默退化”的：头部行高（与标签栏错开几像素就成了两道错位的分界）、
 * 提交框两组的不可压与右对齐（一旦可压，按钮就会被挤成省略号或被顶出可视区——而页面不报错）。
 */
describe("样式契约：变更面板", () => {
  test("头部与资源管理器头部、编辑器标签栏同行高（34px）", () => {
    expect(ruleBody(".fw-changes-head")).toMatch(/height:\s*34px/)
    expect(ruleBody(".fw-explorer-head")).toMatch(/height:\s*34px/)
  })

  test("头部有标题与改动数（只有两个图标按钮时左上角是一大片空白）", () => {
    /* 为何钉在源码层：标题与计数都是“删了不会报错、界面上只是少东西”的那类元素——
     * 而左栏首行没有标题时，用户看不出这是哪一块面板（与资源管理器头部有根名按钮不对称）。 */
    const src = stripComments(readFileSync(join(SRC, "files", "changes.ts"), "utf8"))
    expect(src).toContain('class: "fw-changes-title"')
    expect(src).toContain('class: "fw-changes-count"')
    // 标题必须在头部里，且**排在前面**（放右侧就变成“靠右的小字”，不再是标题）
    const head = src.slice(src.indexOf('class: "fw-changes-head"'), src.indexOf('class: "fw-changes-head"') + 200)
    expect(head.indexOf("headTitle")).toBeGreaterThan(0)
    // 计数随状态更新（且干净时隐去）
    expect(src).toContain("headCount.textContent")
    expect(src).toContain("headCount.hidden")
    // 样式存在（缺失时标题会变成一个裸 span，字号字重都不对）
    expect(ruleBody(".fw-changes-title")).toMatch(/font-size:/)
    expect(ruleBody(".fw-changes-count")).toMatch(/border-radius:/)
  })

  test("分组头的计数与标题计数是同一种胶囊（两处各写一份样式必然漂移）", () => {
    /* 为何钉住：分组头原先把数字写在组名里（「未暂存（2）」），与标题的胶囊
     * 是两种视觉语言；统一后若某天有人只改一边（或把样式拆成两份），
     * 两处会看着像两个不相干的面板。这里要求两选择器**共用同一条声明块**。 */
    const src = stripComments(readFileSync(join(SRC, "files", "changes.ts"), "utf8"))
    expect(src).toContain('class: "fw-change-group-count"')
    // 组名与计数是两个节点（再写回「名（N）」就没有胶囊了）
    expect(src).not.toContain("（${g.items.length}）")
    // 同一条规则里同时含两个选择器：取两边的声明体逐字比（相等 = 共用一份样式）
    const title = ruleBody(".fw-changes-count")
    const group = ruleBody(".fw-change-group-count")
    expect(title.length).toBeGreaterThan(0)
    expect(group).toBe(title)
    // 目录行的计数走的是另一套（灰字、无底）——不能被胶囊规则误伤
    expect(ruleBody(".fw-change-count")).not.toMatch(/border-radius:/)
  })

  test("分组头的整组动作：放弃在前、暂存在后，且已暂存组不给放弃", () => {
    /* 为何钉在源码层：分组头的两个按钮是“顺序反了就随时多一个误点”的东西——
     * 行内与目录行的次序都是「放弃 → 暂存」，组头若反过来，用户在同一个面板里
     * 就要记住两套手势方向；而「已暂存组不给放弃」是一条**语义**约束（`discard` 对已跟踪路径
     * 走 `git checkout -- <path>`，源是 index，全部暂存的文件点下去什么也不会发生），
     * 删掉这道守卫不会报错，只会多一个静默的空按钮。 */
    const src = stripComments(readFileSync(join(SRC, "files", "changes.ts"), "utf8"))
    // ① 两类可放弃的组都有整组放弃，且用的是同一个 discard（确认框/backup:false 只有一份）
    expect(src).toContain('g.key === "unstaged" || g.key === "untracked"')
    // ② 组头里的次序：discardAll 在 stageAll 之前
    const head = src.slice(src.indexOf('class: `fw-change-group-head'), src.indexOf('class: `fw-change-group-head') + 900)
    expect(head.indexOf("discardAll")).toBeGreaterThan(-1)
    expect(head.indexOf("discardAll")).toBeLessThan(head.indexOf("stageAll"))
    // ③ 放弃是破坏性动作：按钮必须走 danger 档（图标按钮的另一条 hover 色，见 .fw-icon-btn.danger）
    const discardBlock = src.slice(src.indexOf("const discardAll"), src.indexOf("const stageAll"))
    expect(discardBlock).toContain('"danger"')
    expect(ruleBody(".fw-icon-btn.danger:hover")).toMatch(/--danger/)
  })

  test("改动列表一律整仓库（不按仓库内前缀过滤）", () => {
    /* 为何钉在源码层：本面板回答「我改了什么、要不要提交」，而提交动的是整个仓库——
     * 若按当前根在仓库内的子目录前缀过滤，别处的改动会从列表里消失（且不留任何提示），
     * 用户以为工作区只改了这几个文件。回归不会报错，只会静默少列。 */
    const src = stripComments(readFileSync(join(SRC, "files", "changes.ts"), "utf8"))
    expect(src).not.toContain("repoPrefix") // hooks 不再接收前缀
    expect(src).not.toMatch(/\.startsWith\(`\$\{prefix\}/) // 不做前缀过滤
    // 头部也不该再留范围开关（删功能就一并删样式，否则是“看不见的死代码”）
    expect(allCss()).not.toContain("fw-scope-chip")
  })

  test("左栏下限读 CSS 变量（写死的数字在按钮文案/字体变化后会静默失效）", () => {
    expect(ruleBody(".fw-left")).toMatch(/min-width:\s*var\(--fw-left-min/)
  })

  test("树视图行：缩进读 --fw-depth（行背景仍铺满整行，缩进用 padding 而非 margin）", () => {
    const body = ruleBody(".fw-changes-panel .fw-change-row")
    expect(body).toMatch(/padding-left:\s*calc\([^)]*var\(--fw-depth/)
    expect(ruleBody(".fw-change-dirrow")).toMatch(/padding-left:\s*calc\([^)]*var\(--fw-depth/)
  })

  test("目录行与分组头的折叠箭头都靠折叠类旋转（改了看不到折叠）", () => {
    // 选的是**直接子图标**（`> .fw-icon`）：行内按钮的图标也是 .fw-icon，后代选择器会把它们一起转
    expect(ruleBody(".fw-change-dirrow.collapsed > .fw-icon")).toMatch(/transform:\s*rotate\(-90deg\)/)
    expect(ruleBody(".fw-change-group-head.collapsed > .fw-icon")).toMatch(/transform:\s*rotate\(-90deg\)/)
  })

  test("改动行：行内动作常不占宽度（只藏 opacity 的话按钮仍吃掉 60~70px）", () => {
    /* 为何钉在样式层：左栏 286px，三个 20px 按钮 + 间距就是一行宽度的四分之一。
     * 用 `opacity: 0` 藏按钮时 DOM/截图里都“看不到它”，但它仍在 flex 行里占位——
     * 于是路径被持续挤压（实测同批 10 行里 8 行目录被截），而肉眼只能归因于“栏太窄”。 */
    const idle = ruleBody(".fw-changes-panel .fw-change-row .fw-change-actions")
    expect(idle).toMatch(/width:\s*0/)
    expect(idle).toMatch(/overflow:\s*hidden/)
    // 悬停/聚焦时必须把宽度让回来，否则按钮点了没反应（看得见、点不到）
    const shown =
      ruleBody(".fw-changes-panel .fw-change-row:hover .fw-change-actions") +
      ruleBody(".fw-changes-panel .fw-change-row:focus-within .fw-change-actions")
    expect(shown).toMatch(/width:\s*auto/)
  })

  test("改动行：目录写在文件名之后（顺序反了窄栏下目录先占据宽度、文件名被挤）", () => {
    const src = stripComments(readFileSync(join(SRC, "files", "changes.ts"), "utf8"))
    const name = src.indexOf('class: "fw-change-name"')
    const dir = src.indexOf('class: "fw-change-dir"')
    expect(name).toBeGreaterThan(0)
    expect(dir).toBeGreaterThan(name)
    // 目录不再带尾斜杠（它是后缀，不是前缀）
    expect(src).not.toContain('text: `${dir}/`')
  })

  test("改动行：容器不再包一层 .fw-change-path（那会把目录与文件名捆成同一个不可分的 flex item）", () => {
    const src = stripComments(readFileSync(join(SRC, "files", "changes.ts"), "utf8"))
    expect(src).not.toContain("fw-change-path")
    expect(allCss()).not.toContain(".fw-change-path")
  })

  test("提交框：选项组与动作组都不可压，动作组右对齐", () => {
    expect(ruleBody(".fw-commit-actions .fw-commit-opts")).toMatch(/flex:\s*none/)
    expect(ruleBody(".fw-commit-actions .fw-commit-btns")).toMatch(/flex:\s*none/)
    expect(ruleBody(".fw-commit-actions .fw-commit-btns")).toMatch(/margin-left:\s*auto/)
    // 允许折行，但只应在两组之间（两个按钮不许被拆到两行）
    expect(ruleBody(".fw-commit-actions")).toMatch(/flex-wrap:\s*wrap/)
  })
})

/**
 * 样式契约：Git 工具窗**没有标题栏**，写操作在途与多步操作的提示落在底部状态栏。
 *
 * 守的理由：旧实现有一条按需亮出的标题带（`.fw-git-titlebar`），每次写操作把三栏上下推挤一下。
 * 只要有人把这一行加回去（或把状态栏那格删了），**页面不会报错**——只是面板又开始跳、
 * 或者写操作变成“点了没任何反馈”。两边各钉一条。
 */
describe("样式契约：Git 工具窗无标题栏（提示在状态栏）", () => {
  test("面板与样式中都不再存在标题栏/状态带", () => {
    const git = normalized(readFileSync(join(SRC, "files", "git.ts"), "utf8"))
    expect(git).not.toContain("fw-git-titlebar")
    expect(git).not.toContain("renderTitleBar")
    expect(ruleBody(".fw-git-titlebar")).toBe("")
    expect(ruleBody(".fw-git-opbar")).toBe("")
  })

  test("在途提示由状态栏那格承担（有样式、有动画、宿主确实渲染它）", () => {
    expect(ruleBody(".fw-status-item.git-busy")).toMatch(/color:\s*var\(--accent\)/)
    expect(ruleBody(".fw-status-item.git-busy .fw-icon")).toMatch(/animation:/)
    const main = normalized(readFileSync(join(SRC, "files", "main.ts"), "utf8"))
    expect(main).toContain("state.gitBusy")
    expect(main).toContain("fw-status-item git-busy")
  })
})

/**
 * 样式契约：亚克力主题必须接管工作台的**半透明浮层**（给毛玻璃 + 更实的 --pop-bg）。
 *
 * 存在的理由（实测过的缺陷）：工作台浮层的底色是 `--bg-elev`（亚克力下是半透明的 0.8 白 /
 * 0.82 黑），而它们没进亚克力主题的浮层白名单——于是**没有毛玻璃**，背后的编辑器代码**清晰可读地**
 * 透出来，与弹窗文字叠在一起（白色亚克力下文件历史弹窗几乎没法读）。
 * 这件事不报错、页面也不出错，只有一眼看才知道；而“新加一个工作台浮层忘了加白名单”
 * 与“改白名单时删了这几行”都会**静默**退回原症状，所以用测试钉住。
 *
 * 只查“有没有被接管”与“是否真给了 backdrop-filter / --pop-bg”，不锁具体色值（那是主题的审美域）。
 */
describe("样式契约：亚克力主题接管工作台浮层（否则透出背后正文）", () => {
  const acrylic = readFileSync(join(SRC, "themes", "acrylic.css"), "utf8")
  /** 工作台里“压在正文上、且底色是半透明令牌”的浮层类（以后新增同类浮层就补到这里）。
   *  为何不含菜单类（.fw-menu-pop / .fw-log-ref-pop）：它们本就自己写死了 blur(12px)；
   *  也不含扇形按钮：那是双背景垫实的实底。 */
  const WORKBENCH_FLOATS = [".fw-dialog", ".fw-qo", ".fw-toast"]

  test("每个浮层都在亚克力的浮层名单里，且跟随 --pop-bg / --glass", () => {
    for (const sel of WORKBENCH_FLOATS) {
      /* 选择器必须**边界匹配**：不能用 indexOf(前缀)——那样 `.fw-dialog` 会命中 `.fw-dialog-XX`、
         `.fw-toast` 会命中真实存在的 `.fw-toast-close`、`.fw-qo` 会命中 `.fw-qo-overlay`，
         于是「把这一行从名单里删掉 / 改错」变成静默通过（反向验证实测就掉进去过）。
         名单是一条逗号分隔的选择器列表，所以选择器之后只能是 `,` 或 ` {`。 */
      const re = new RegExp(`\\[data-theme="acrylic"\\]\\s*${sel.replace(/\./g, "\\.")}\\s*(?=[,{])`)
      const m = re.exec(acrylic)
      expect(m, `${sel} 不在亚克力浮层白名单里（或与别的选择器写在一起了）——会静默退回“背景清晰透出”`).not.toBeNull()
      // 该选择器所在规则块必须真给了毛玻璃与浮层底色（从命中处截到该规则块的收尾 } 为止）
      const block = acrylic.slice(m!.index, acrylic.indexOf("}", m!.index))
      /* 正则必须**行首锚定**：否则 `backdrop-filter: var(--glass)` 会命中
         `-webkit-backdrop-filter: var(--glass)` 这个子串（两条本来就是一正一前缀，
         前一条被删掉 / 改值时仍能静默通过——反向验证实测掉进去过）。 */
      expect(block, `${sel} 所在规则块未设 backdrop-filter`).toMatch(/^[ \t]*backdrop-filter:\s*var\(--glass\)/m)
      expect(block, `${sel} 所在规则块未设 --pop-bg 底色`).toMatch(/^[ \t]*background:\s*var\(--pop-bg\)/m)
    }
  })
})

/**
 * 样式契约：差异视图导航按钮的位置——在差异视图自己的头部（`fw-viewer-bar` 内的 `.fw-diff-nav`），
 * 不在标签栏动作区。
 *
 * 存在的理由：按钮描述的是「这块差异视图」的内容（第几个变更文件、当前/总差异块数），
 * 放标签栏上会随激活标签变化而意义漂移（预览标签被顶掉时按钮消失、切回来又重建，
 * 计数订阅跟着标签栏重建退订重订）；丟失订阅/按钮退回标签栏都不会报错——只是「按钮又跑到
 * 标签栏上去了」或「计数不更新了」，只有契约能拦住。头部栏里的固定项不可压缩
 * （窄窗口下被牺牲的只能是 A/B 标签文本，不是导航按钮）。
 */
describe("样式契约：差异视图导航在头部不在标签栏", () => {
  test("导航容器与按钮组样式存在（缺失时按钮裸排成一行、看不出是一组导航）", () => {
    expect(ruleBody(".fw-diff-nav")).toMatch(/display:\s*flex/)
    expect(ruleBody(".fw-viewer-bar .fw-nav-group")).toMatch(/display:\s*flex/)
    expect(ruleBody(".fw-viewer-bar .fw-nav-group")).toMatch(/flex:\s*none/)
  })

  test("导航按钮挂在差异视图头部（navHost），不在标签栏动作区", () => {
    const main = readFileSync(join(SRC, "files", "main.ts"), "utf8")
    // 两组按钮由 mountDiffHeaderNav 装进视图头部容器（挂载点：openDiff / loadDiffInto 两处都在）
    expect(main).toContain("mountDiffHeaderNav(view.navHost, tab)")
    expect((main.match(/mountDiffHeaderNav\(view\.navHost, tab\)/g) ?? []).length).toBe(2)
    // 标签栏渲染里不得再有差异导航（回迁会双重渲染：两套按钮各自计数、各自订阅）
    const fnStart = main.indexOf("function renderTabActions(")
    const fnEnd = main.indexOf("\nfunction ", fnStart + 1)
    const fnBody = main.slice(fnStart, fnEnd)
    for (const token of ["chevronUp", "chevronDown", "fw-nav-count", "diffNav", "navigateReview"]) {
      expect(fnBody, `renderTabActions 不应再含差异导航（${token}）`).not.toContain(token)
    }
    // 订阅随视图生命周期（dispose），不再挂标签栏重建节奏的全局退订变量
    expect(main).not.toContain("diffNavUnsub")
  })

  test("差异视图（含降级渲染）都提供头部导航容器 navHost", () => {
    const git = readFileSync(join(SRC, "files", "git.ts"), "utf8")
    // Monaco 主路径与降级 hunks 路径都暴露 navHost（漏一路 = 那个形态下跨文件导航凭空消失）
    expect(git).toContain("nav: null, navHost")
    expect(git).toContain("nav: handle.nav ?? null, navHost")
    // 头部容器进的是 viewer-bar（不是任意位置：布局由该栏承担）
    expect((git.match(/class: "fw-diff-nav"/g) ?? []).length).toBe(2)
  })

  test("打开差异视图后自动定位到第一处差异（不要求用户再点一下）", () => {
    const editor = readFileSync(join(SRC, "files", "editor.ts"), "utf8")
    const at = editor.indexOf("export async function createDiffEditor")
    expect(at).toBeGreaterThan(-1)
    const end = editor.indexOf("\n/* -", at + 1)
    const body = editor.slice(at, end > at ? end : undefined)
    // 首跳标志 + 跳一次即清（否则每次差异重算都把视口拽回第一块）
    expect(body).toContain("autoJumpPending")
    expect(body).toMatch(/autoJumpPending\s*=\s*false/)
    // 自动跳不抢焦点（用户刚点开标签，焦点不应被拽进编辑器）
    expect(body).toContain("reveal(0, false)")
    // 用户已自己滚过则不抢（大文件差异算得慢，算完时他可能已在看别处）
    expect(body).toContain("userScrolled")
    expect(body).toContain("!userScrolled")
    // 「用户已滚过」只能用真实 DOM 输入判定：不能拿 onDidScrollChange 当用户滚动——
    // Monaco 实例化/换模型自己会写 scrollTop（实测会误杀首跳：差异视图停在第一行，
    // 导航显示 1/1 却还在开头）。
    expect(body).toContain('host.addEventListener("wheel", markUser')
    expect(body).toContain('host.addEventListener("pointerdown", markUser')
    expect(body).toContain('host.addEventListener("touchmove", markUser')
    // onDidScrollChange 的回调体内不得出现赋值（靠 markUser 手动置位）
    const cbStart = body.indexOf("onDidScrollChange(() => {")
    expect(cbStart).toBeGreaterThan(-1)
    const cbBody = body.slice(cbStart, body.indexOf("})", body.indexOf("emit()", cbStart)))
    expect(cbBody).not.toContain("userScrolled = true")
    // 无差异不跳
    expect(body).toMatch(/list\(\)\.length\s*\)\s*reveal\(0, false\)/)
  })
})
