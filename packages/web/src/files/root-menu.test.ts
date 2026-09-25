/**
 * 根选择菜单的分组与折叠（`files/root-menu.ts`）。
 *
 * 三条口径要钉住：项目组排最上且标题就叫「项目」；会话组超过阈值时折叠（其余进「更多」子菜单）；
 * 当前所在的会话永远可见（否则「我在哪个会话」要靠猜）。
 */
import { describe, expect, test } from "bun:test"
import { buildRootSections, SESSION_VISIBLE, type RootMenuEntry } from "./root-menu"

const root = (id: string, name: string, kind: string, extra: Partial<RootMenuEntry> = {}): RootMenuEntry => ({ id, name, kind, ...extra })

const tree: RootMenuEntry[] = [
  root("sess:1", "会话一", "sess"),
  root("sess:2", "会话二", "sess"),
  root("sess:3", "会话三", "sess"),
  root("proj:gebai", "gebai", "proj", { isRepo: true, branch: "master" }),
  root("user:", "用户目录", "user"),
  root("abs:C:\\tmp", "tmp", "abs"),
]

describe("根选择菜单分组", () => {
  test("项目组在最上，标题是「项目」（不是「预置项目」）", () => {
    const s = buildRootSections(tree, "sess:1")
    expect(s[0]!.title).toBe("项目")
    expect(s[0]!.entries.map((e) => e.id)).toEqual(["proj:gebai"])
    expect(s.map((x) => x.title)).toEqual(["项目", "会话工作区", "其它"])
  })

  test("空分组不出现（没有预置项目时不留空标题）", () => {
    const s = buildRootSections(tree.filter((x) => x.kind !== "proj"), "sess:1")
    expect(s.map((x) => x.title)).toEqual(["会话工作区", "其它"])
  })

  test("其它组收揽 sess/proj 之外的根（user / abs）", () => {
    const s = buildRootSections(tree, "sess:1")
    const other = s.find((x) => x.title === "其它")!
    expect(other.entries.map((e) => e.kind)).toEqual(["user", "abs"])
  })
})

describe("会话组折叠", () => {
  test("不超过阈值：全显示，没有「更多」", () => {
    const few = tree.filter((x) => !["sess:3"].includes(x.id))
    const s = buildRootSections(few, "")
    const sess = s.find((x) => x.title === "会话工作区")!
    expect(sess.entries.map((e) => e.id)).toEqual(["sess:1", "sess:2"])
    expect(sess.more).toBeUndefined()
  })

  test("超过阈值：只留前若干个 + 「更多会话（N）」，两组不重不漏", () => {
    const many = Array.from({ length: 12 }, (_, i) => root(`sess:${i + 1}`, `会话${i + 1}`, "sess"))
    const s = buildRootSections(many, "")
    const sess = s.find((x) => x.title === "会话工作区")!
    expect(sess.entries.length).toBe(SESSION_VISIBLE)
    expect(sess.more?.label).toBe(`更多会话（${12 - SESSION_VISIBLE}）`)
    const all = [...sess.entries, ...(sess.more?.entries ?? [])].map((e) => e.id)
    expect(all.length).toBe(12)
    expect(new Set(all).size).toBe(12)
  })

  test("当前会话在折叠区里时仍进首屏（且不重复出现在「更多」）", () => {
    const many = Array.from({ length: 12 }, (_, i) => root(`sess:${i + 1}`, `会话${i + 1}`, "sess"))
    const s = buildRootSections(many, "sess:12")
    const sess = s.find((x) => x.title === "会话工作区")!
    expect(sess.entries.map((e) => e.id)).toContain("sess:12")
    expect(sess.more?.entries.map((e) => e.id)).not.toContain("sess:12")
    expect(sess.entries.length).toBe(SESSION_VISIBLE)
  })

  test("折叠顺序：其余项保持传入顺序（会话按最近使用排），只有“当前会话顶上来”会挤掉一个", () => {
    const many = Array.from({ length: 8 }, (_, i) => root(`sess:${i + 1}`, `会话${i + 1}`, "sess"))
    const s = buildRootSections(many, "sess:7")
    const sess = s.find((x) => x.title === "会话工作区")!
    expect(sess.entries.map((e) => e.id)).toEqual(["sess:1", "sess:2", "sess:3", "sess:4", "sess:7"])
    expect(sess.more?.entries.map((e) => e.id)).toEqual(["sess:5", "sess:6", "sess:8"])
  })
})
