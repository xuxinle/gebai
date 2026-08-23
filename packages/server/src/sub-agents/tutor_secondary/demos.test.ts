import { describe, expect, test } from "bun:test"
import { DEMO_TEMPLATES, DEMO_TEMPLATE_IDS, columnSteps } from "./demos"
import { REF_ENTRIES } from "./reference-data"

const render = (id: string, params: Record<string, unknown>): string => {
  const tpl = DEMO_TEMPLATES.find((t) => t.id === id)
  if (!tpl) throw new Error(`unknown template: ${id}`)
  return tpl.render(params)
}

describe("教学演示模板库（demos）", () => {
  test("注册表完整：8 个模板且 id 唯一", () => {
    expect(DEMO_TEMPLATE_IDS.sort()).toEqual(["column", "flashcards", "fraction", "function_graph", "geometry", "mental_math", "quiz", "reference"])
    expect(new Set(DEMO_TEMPLATE_IDS).size).toBe(DEMO_TEMPLATE_IDS.length)
    for (const t of DEMO_TEMPLATES) expect(t.usage.length).toBeGreaterThan(10)
  })

  test("全部交互页面脚本语法合法（字符串拼接 JS 的回归护栏：new Function 编译不执行）", () => {
    const cases: Array<[string, Record<string, unknown>]> = [
      ["quiz", { questions: [{ q: "1+1", options: ["1", "2"], answer: "2" }] }],
      ["mental_math", {}],
      ["column", { a: 37, b: 25, op: "add" }],
      ["column", { a: 23, b: 12, op: "mul" }],
      ["column", { a: 52, b: 8, op: "sub" }],
      ["function_graph", { fn: "quadratic" }],
      ["function_graph", { fn: "inverse", a: -2 }],
      ["geometry", { preset: "pythagorean" }],
      ["geometry", { preset: "triangle_height" }],
      ["geometry", { preset: "circle" }],
      ["geometry", { preset: "parallel" }],
      ["fraction", { numerator: 1, denominator: 2, numerator2: 2, denominator2: 3 }],
      ["fraction", { numerator: 3, denominator: 4, shape: "bar" }],
      ["flashcards", { items: [{ front: "a", back: "b" }] }],
    ]
    for (const [template, params] of cases) {
      const html = render(template, params)
      const m = html.match(/<script>([\s\S]*?)<\/script>/)
      if (!m || !m[1].trim()) continue
      expect(() => new Function(m[1]), `${template} ${JSON.stringify(params)}`).not.toThrow()
    }
  })

  test("quiz：题目/选项/批改脚本渲染；校验空题与 answer 不在 options", () => {
    const html = render("quiz", {
      title: "小测验",
      studentName: "小明",
      questions: [
        { q: "1+1=?", options: ["1", "2", "3"], answer: "2", explain: "加法事实" },
        { q: "中国的首都是？", answer: "北京" },
      ],
    })
    expect(html).toContain("<title>小测验</title>")
    expect(html).toContain("小明")
    expect(html).toContain("加法事实")
    expect(html).toContain("提交批改")
    expect(() => render("quiz", {})).toThrow("questions")
    expect(() =>
      render("quiz", { questions: [{ q: "x", options: ["a", "b"], answer: "c" }] }),
    ).toThrow("options 之一")
  })

  test("mental_math：配置渲染；非法运算拒绝", () => {
    const html = render("mental_math", { ops: ["add", "div"], digits: 2, count: 10, seconds: 30, studentName: "小红" })
    expect(html).toContain("小红的口算闯关")
    expect(html).toContain("加法 + 除法")
    expect(html).toContain("10 题 · 30 秒")
    expect(() => render("mental_math", { ops: ["pow"] })).toThrow("add/sub/mul/div")
  })

  test("column：分步动画竖式；columnSteps 数学正确；校验拒绝", () => {
    const add = render("column", { a: 37, b: 25, op: "add" })
    expect(add).toContain("下一步")
    expect(add).toContain("自动播放")
    expect(add).toContain("进位")
    // 步骤数学：37+25 个位 7+5=12 进 1 写 2；十位 3+2+1=6
    const addSteps = columnSteps(37, 25, "add")
    expect(addSteps[0].text).toContain("7 + 5 = 12")
    expect(addSteps[0].resultDigit).toBe("2")
    expect(addSteps[0].carry).toBe(1)
    expect(addSteps[1].text).toContain("3 + 2 + 进 1 = 6")
    const sub = render("column", { a: 52, b: 8, op: "sub" })
    expect(sub).toContain("借位")
    // 52−8：个位 2 不够减 8，借 1 当 12−8=4；十位 5（被借走 1 剩 4）
    const subSteps = columnSteps(52, 8, "sub")
    expect(subSteps[0].text).toContain("借 1 当 10：12 − 8 = 4")
    expect(subSteps[1].text).toContain("剩 4")
    const mul = render("column", { a: 23, b: 12, op: "mul" })
    expect(mul).toContain("23 × 12 = 276")
    expect(mul).toContain("部分积")
    const mulSteps = columnSteps(23, 12, "mul")
    expect(mulSteps[0].partial).toBe("46")
    expect(mulSteps[1].partial).toBe("230")
    expect(mulSteps[mulSteps.length - 1].revealAll).toBe(true)
    expect(() => render("column", { a: 3, b: 8, op: "sub" })).toThrow("a ≥ b")
    expect(() => render("column", { a: 3, b: 8, op: "mod" })).toThrow("add/sub/mul")
  })

  test("function_graph：解析式与标注渲染 + 交互滑块；非法函数拒绝", () => {
    const lin = render("function_graph", { fn: "linear", a: 2, b: -1 })
    expect(lin).toContain("y = 2x − 1")
    expect(lin).toContain("x轴交点")
    expect(lin).toContain('input type="range" id="sA"')
    const quad = render("function_graph", { fn: "quadratic" })
    expect(quad).toContain("x²")
    expect(quad).toContain("顶点")
    expect(quad).toContain('id="sC"')
    expect(() => render("function_graph", { fn: "cubic" })).toThrow("linear/quadratic/inverse/absolute")
    expect(() => render("function_graph", { fn: "quadratic", a: 0 })).toThrow("不能为 0")
  })

  test("geometry：四个预设交互化渲染与非法预设拒绝", () => {
    for (const preset of ["pythagorean", "triangle_height", "circle", "parallel"]) {
      const html = render("geometry", { preset })
      expect(html).toContain("<svg")
    }
    const pyth = render("geometry", { preset: "pythagorean" })
    expect(pyth).toContain("重排拼图")
    expect(pyth).toContain('class="tri"')
    expect(render("geometry", { preset: "triangle_height" })).toContain("pointerdown")
    expect(render("geometry", { preset: "circle" })).toContain('data-g="g-chord"')
    const par = render("geometry", { preset: "parallel" })
    expect(par).toContain('id="ppslider"')
    expect(par).toContain("∠1=∠5")
    expect(() => render("geometry", { preset: "cube" })).toThrow("pythagorean")
  })

  test("fraction：饼图/条形/双分数对比 + 交互按钮；非法分子分母拒绝", () => {
    const pie = render("fraction", { numerator: 3, denominator: 4 })
    expect(pie).toContain("3/4（75%）")
    expect(pie).toContain("饼图分格")
    expect(pie).toContain("分母")
    expect(pie).toContain("data-t=\"d1\"")
    const bar = render("fraction", { numerator: 1, denominator: 3, shape: "bar" })
    expect(bar).toContain("条形分格")
    const cmp = render("fraction", { numerator: 1, denominator: 2, numerator2: 2, denominator2: 3 })
    expect(cmp).toContain("1/2 ＜ 2/3")
    expect(cmp).toContain("通分比较")
    expect(cmp).toContain('id="cmp"')
    expect(() => render("fraction", { numerator: 5, denominator: 4 })).toThrow("不能大于")
    expect(() => render("fraction", {})).toThrow("必填")
  })

  test("flashcards：卡片渲染；空清单拒绝", () => {
    const html = render("flashcards", { title: "乘法口诀", items: [{ front: "3×4", back: "12" }, { front: "6×7", back: "42" }] })
    expect(html).toContain("乘法口诀")
    expect(html).toContain("3×4")
    expect(html).toContain("需复习")
    expect(() => render("flashcards", { items: [] })).toThrow("items")
  })

  test("reference：索引页/学科主题过滤/单条大卡；无匹配引导", () => {
    // 索引页：不传参数 → 分组目录（含学科标题与 id）
    const index = render("reference", {})
    expect(index).toContain("公式与定理速查")
    expect(index).toContain("数学")
    expect(index).toContain("id: sm-quadratic")
    // 学科过滤 → 速查卡（公式大字块）
    const math = render("reference", { subject: "数学", topic: "一元二次方程" })
    expect(math).toContain("求根公式与判别式")
    expect(math).toContain("x = (−b ± √(b²−4ac)) ÷ 2a")
    expect(math).toContain("韦达定理")
    // 单条大卡：带图示的定理
    const single = render("reference", { id: "sm-circle-angle" })
    expect(single).toContain("圆周角定理")
    expect(single).toContain("证明思路")
    expect(single).toContain("<svg")
    // 小学适用条目
    const primary = render("reference", { subject: "数学", topic: "平面图形" })
    expect(primary).toContain("长方形与正方形")
    expect(primary).toContain("梯形面积")
    // 无匹配：列出可用学科引导
    expect(() => render("reference", { subject: "体育" })).toThrow("没有匹配的条目")
    expect(() => render("reference", { id: "no-such-id" })).toThrow("未找到 id")
  })

  test("reference：空白页知识快捷入口全覆盖（各学段知识点的主题过滤均有内置条目）", () => {
    // 与 packages/web/src/shortcuts.ts 的 39 个内置知识快捷对应——模型按快捷提示词中的
    // 学科/主题关键词调用 reference，此表保证每个关键词都有内置演示兜底（无匹配会 throw）
    const cases: Array<[subject: string, topic: string]> = [
      // 小学
      ["数学", "平面图形"], ["数学", "立体图形"], ["数学", "运算律"], ["数学", "分数与小数"],
      ["数学", "竖式计算"], ["数学", "单位换算"], ["数学", "行程"], ["数学", "统计"],
      // 初中
      ["数学", "乘法公式"], ["数学", "因式分解"], ["数学", "二次根式"], ["数学", "分式"],
      ["数学", "一元二次方程"], ["数学", "锐角三角函数"], ["数学", "三角形"], ["数学", "函数"], ["数学", "圆"],
      ["物理", "力与运动"], ["物理", "光与声"], ["物理", "热学"], ["物理", "压强"],
      ["物理", "功和能"], ["物理", "电学"],
      ["化学", "金属"], ["化学", "方程式"], ["化学", "酸碱盐"], ["化学", "溶液"],
      // 高中
      ["数学", "三角函数"], ["数学", "解三角形"], ["数学", "数列"], ["数学", "不等式"],
      ["数学", "导数"], ["数学", "解析几何"], ["数学", "立体几何"], ["数学", "概率"],
      ["物理", "力学综合"], ["物理", "电磁学"],
      ["化学", "物质的量"], ["化学", "有机"], ["化学", "守恒"],
    ]
    for (const [subject, topic] of cases) {
      const html = render("reference", { subject, topic })
      expect(html.length).toBeGreaterThan(600)
    }
    expect(REF_ENTRIES.length).toBeGreaterThanOrEqual(80)
  })
})
