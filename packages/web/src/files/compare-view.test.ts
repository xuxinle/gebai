/**
 * 对比视图「切回标签即重验」的行为护栏（真调 view 实例，不只是纯函数）。
 *
 * diffSignature 的用例只能证明“判定对不对”，证明不了“到底有没有去拉后端”。
 * 而这次的缺陷正是**没去拉**——所以必须有一个用例盯着 hooks.api 的调用次数：
 * ① `refresh()` 拉一次；② `revalidate()` 再拉一次（切回标签时走的就是它）；
 * ③ 数据没变时 `revalidate()` **不重绘**（列表节点保持同一批，滚动位置不丢）；
 * ④ 数据变了时重绘出新内容（不然等于没刷新）。
 *
 * 环境：复用 fw 的最小真实 DOM（属性/子节点/事件语义齐全），用完还原全局。
 */
import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import { installMiniDom } from "../fw/mini-dom.test-helper"

let restore: () => void = () => {}
beforeAll(() => {
  restore = installMiniDom().restore
})
afterAll(() => {
  restore()
})

const { createCompareView } = await import("./compare")

/** 假的 FsApi：只实现对比视图用到的两个方法，并记录调用次数。 */
function makeApi(scripts: { compare: Array<{ files: any[]; additions: number; deletions: number; mergeBaseOf?: string }> }) {
  const calls = { compare: 0, refs: 0 }
  let compareIdx = 0
  const api = {
    gitCompare: async () => {
      calls.compare++
      const r = scripts.compare[Math.min(compareIdx++, scripts.compare.length - 1)]
      return r
    },
    gitRefs: async () => {
      calls.refs++
      return { branches: [], tags: [], recent: [], head: { branch: "main", hash: "abc" } }
    },
  }
  return { api, calls }
}

const hooks = (api: unknown) => ({
  api: api as never,
  root: () => "/repo",
  openDiff: () => {},
  openFile: () => {},
  onFsChanged: () => {},
})

const file = (path: string, status = "modified", additions = 1, deletions = 0) => ({ path, status, additions, deletions })

describe("对比视图重验：真的会去拉后端", () => {
  test("refresh() 首次拉取；revalidate() 会**再拉一次**（切回标签走的就是它）", async () => {
    const { api, calls } = makeApi({ compare: [{ files: [file("a.ts")], additions: 1, deletions: 0 }] })
    const view = createCompareView(hooks(api))
    await view.refresh()
    expect(calls.compare).toBe(1)
    await view.revalidate()
    expect(calls.compare).toBe(2)
    await view.revalidate()
    expect(calls.compare).toBe(3)
  })

  test("revalidate 同时刷新引用（分支/HEAD 也可能变了）", async () => {
    const { api, calls } = makeApi({ compare: [{ files: [], additions: 0, deletions: 0 }] })
    const view = createCompareView(hooks(api))
    await view.refresh()
    const refsBefore = calls.refs
    await view.revalidate()
    expect(calls.refs).toBeGreaterThan(refsBefore)
  })

  test("数据没变时 revalidate 不重绘（列表节点是同一批，滚动位置不丢）", async () => {
    const same = { files: [file("a.ts")], additions: 1, deletions: 0 }
    const { api } = makeApi({ compare: [same, { ...same, files: [file("a.ts")] }] })
    const view = createCompareView(hooks(api))
    await view.refresh()
    const listBefore = view.el.querySelector(".fw-compare-list")!
    const childrenBefore = [...listBefore.childNodes]
    await view.revalidate()
    const childrenAfter = [...listBefore.childNodes]
    // 同一批节点（未被替换）——重绘会 replaceChildren 出新节点
    expect(childrenAfter.length).toBe(childrenBefore.length)
    expect(childrenAfter[0]).toBe(childrenBefore[0])
  })

  test("**数据变了时 revalidate 必须重绘**（否则等于没刷新，正是本次要修的问题）", async () => {
    const { api } = makeApi({
      compare: [
        { files: [file("a.ts", "modified", 1, 0)], additions: 1, deletions: 0 },
        { files: [file("a.ts", "modified", 9, 4), file("b.ts", "added", 3, 0)], additions: 12, deletions: 4 },
      ],
    })
    const view = createCompareView(hooks(api))
    await view.refresh()
    const list = view.el.querySelector(".fw-compare-list")!
    const rowsBefore = list.querySelectorAll(".fw-compare-file").length
    await view.revalidate()
    const rowsAfter = list.querySelectorAll(".fw-compare-file").length
    expect(rowsBefore).toBe(1)
    expect(rowsAfter).toBe(2) // 新文件出现了
  })

  test("只有行数变化（文件数不变）也会重绘——这正是“只看数量”会漏掉的情形", async () => {
    const { api } = makeApi({
      compare: [
        { files: [file("a.ts", "modified", 1, 0)], additions: 1, deletions: 0 },
        { files: [file("a.ts", "modified", 7, 3)], additions: 7, deletions: 3 },
      ],
    })
    const view = createCompareView(hooks(api))
    await view.refresh()
    // 汇总条（summary 自身是 span，diff-stat 是它的**兄弟**而非子节点——所以读 summary 容器的文本）
    const readStats = (): string => {
      const host = view.el.querySelector(".fw-compare-head")!
      return [...host.querySelectorAll(".fw-diff-stat")].map((n) => n.textContent).join("|")
    }
    const before = readStats()
    await view.revalidate()
    const after = readStats()
    expect(before).toBe("+1|-0")
    expect(after).toBe("+7|-3")
  })

  test("revalidate 遇到后端报错：保留现有列表，不把用户眼前的内容换成错误页", async () => {    let fail = false
    const calls = { n: 0 }
    const api = {
      gitCompare: async () => {
        calls.n++
        if (fail) throw new Error("network down")
        return { files: [file("a.ts")], additions: 1, deletions: 0 }
      },
      gitRefs: async () => {
        if (fail) throw new Error("network down")
        return { branches: [], tags: [], recent: [], head: {} }
      },
    }
    const view = createCompareView(hooks(api))
    await view.refresh()
    const list = view.el.querySelector(".fw-compare-list")!
    const rows = list.querySelectorAll(".fw-compare-file").length
    fail = true
    await view.revalidate() // 不应抛错
    expect(list.querySelectorAll(".fw-compare-file").length).toBe(rows)
    expect(list.querySelector(".fw-error")).toBeNull()
    expect(calls.n).toBe(2)
  })
})
