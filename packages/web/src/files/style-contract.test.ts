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

  test("头部里的范围芯片可省略、且 hidden 时真的不显示", () => {
    expect(ruleBody(".fw-changes-head .fw-scope-chip")).toMatch(/min-width:\s*0/)
    // .fw-chip 自带 display:inline-flex，会压过 hidden 属性——必须有显式规则兜底
    expect(ruleBody(".fw-changes-head .fw-scope-chip[hidden]")).toMatch(/display:\s*none/)
  })

  test("左栏下限读 CSS 变量（写死的数字在按钮文案/字体变化后会静默失效）", () => {
    expect(ruleBody(".fw-left")).toMatch(/min-width:\s*var\(--fw-left-min/)
  })

  test("树视图行：缩进读 --fw-depth（行背景仍铺满整行，缩进用 padding 而非 margin）", () => {
    const body = ruleBody(".fw-changes-panel .fw-change-row")
    expect(body).toMatch(/padding-left:\s*calc\([^)]*var\(--fw-depth/)
    expect(ruleBody(".fw-change-dirrow")).toMatch(/padding-left:\s*calc\([^)]*var\(--fw-depth/)
  })

  test("目录行的折叠箭头靠类名旋转（改了看不到折叠）", () => {
    expect(ruleBody(".fw-change-dirrow.collapsed .fw-icon")).toMatch(/transform:\s*rotate\(-90deg\)/)
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
