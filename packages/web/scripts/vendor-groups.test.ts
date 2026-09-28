import { describe, expect, test } from "bun:test"
import { resolveVendorGroups, VENDOR_GROUPS } from "./vendor-groups"

describe("GEBAI_WEB_VENDOR 清单解析", () => {
  test("未设置 / 空串 / 全空白 = 全量（缺省不裁剪）", () => {
    expect([...resolveVendorGroups(undefined)]).toEqual([...VENDOR_GROUPS])
    expect([...resolveVendorGroups("")]).toEqual([...VENDOR_GROUPS])
    expect([...resolveVendorGroups(" , ")]).toEqual([...VENDOR_GROUPS])
  })

  test("子集：去首尾空白、保持清单顺序", () => {
    expect([...resolveVendorGroups("monaco, xterm")]).toEqual(["monaco", "xterm"])
    expect([...resolveVendorGroups("tree_sitter")]).toEqual(["tree_sitter"])
  })

  test("未知组报错并列出可用组（不静默忽略）", () => {
    expect(() => resolveVendorGroups("monaco,chalk")).toThrow(/未知组.*chalk/)
    expect(() => resolveVendorGroups("Monaco")).toThrow(/未知组/)
  })
})
