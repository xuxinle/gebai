/**
 * 对比标签页「切回即重验」的接线护栏（静态检查）。
 *
 * 行为测试（compare-view.test.ts）证明了 `revalidate()` 本身会去拉后端；
 * 但如果没人**调用**它，一切都白搭——这正是这次缺陷的形态：方法不存在、调用点也不存在，
 * 单测全绿而用户看到的是过期数据。
 *
 * 这里守两件事：
 * ① `activate()` 里真的有对比较视图的重验调用（切回标签 = 天然刷新时机）；
 * ② 首次打开时**不会重复拉**：`activate` 早于 `compareTabs.set`，此时取不到视图，
 *    只有随后的 `view.refresh()` 拉一次。若顺序被改动（先 set 再 activate），
 *    打开一个对比标签就会请求两次（白跑一次 git diff，大仓库很明显）。
 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const SRC = readFileSync(join(import.meta.dir, "main.ts"), "utf8")

/** 取某个函数体（从 `function name(` 开始到下一个顶层函数声明前）。 */
function bodyOf(name: string): string {
  const start = SRC.indexOf(`function ${name}(`)
  if (start < 0) throw new Error(`找不到函数 ${name}`)
  const rest = SRC.slice(start + 1)
  const nextFn = rest.search(/\nfunction [a-zA-Z]/)
  return nextFn < 0 ? rest : rest.slice(0, nextFn)
}

describe("activate() 里的对比视图重验接线", () => {
  test("activate 会调用比较视图的 revalidate（切回标签即向后端重验）", () => {
    const body = bodyOf("activate")
    expect(body).toContain("compareTabs.get(id)?.view.revalidate()")
  })

  test("重验调用**不阻断**激活（失败不能让标签切不过去）", () => {
    const body = bodyOf("activate")
    const line = body.split("\n").find((l) => l.includes("revalidate()")) ?? ""
    expect(line).toContain(".catch(")
  })

  test("首次打开不重复拉：compareTabs.set 之后不得再有 activate（否则打开就请求两次）", () => {
    /*
     * 要守的性质：新建对比标签时，activate 必须在 compareTabs 登记**之前**发生——
     * 因为 activate 里的重验是靠 compareTabs.get(id) 拿到视图的；登记之前取不到就自然跳过，
     * 随后的 view.refresh() 是唯一一次拉取。若顺序反过来（先 set 再 activate），
     * 打开一个对比标签就会拉两次 git diff（大仓库看得见）。
     *
     * 注意：不能简单拿 `indexOf("activate(id)")` 比位置——openCompare 里还有一条
     * 「标签已存在」分支也有 activate，它本来就在最前面。所以只能检查 set 之后到函数尾之间。
     */
    const body = bodyOf("openCompare")
    const idxSet = body.indexOf("compareTabs.set(id")
    const idxRefresh = body.indexOf("await view.refresh()")
    expect(idxSet).toBeGreaterThan(0)
    expect(idxRefresh).toBeGreaterThan(idxSet)
    const afterSet = body.slice(idxSet, idxRefresh)
    expect(afterSet).not.toContain("activate(id)")
  })

  test("比较视图接口包含 revalidate（防止有人只删实现忘了删接口导致运行时炸）", () => {
    const compareSrc = readFileSync(join(import.meta.dir, "compare.ts"), "utf8")
    expect(compareSrc).toContain("revalidate: () => Promise<void>")
    expect(compareSrc).toContain("revalidate: async () => {")
  })
})
