/**
 * 前端 vendor 引擎组（构建期裁剪单位）与 `GEBAI_WEB_VENDOR` 清单解析。
 *
 * 供 `build-vendor.ts`（拷贝与清理）与测试共用。清单一律是**包含**语义：未设置或空串 = 全量；
 * 含未知组名直接抛错——静默忽略会让「裁剪清单」与产物实际能力不符，比构建失败更难排查。
 */
export const VENDOR_GROUPS = ["monaco", "plantuml", "mermaid", "echarts", "d2js", "xterm", "tree_sitter"] as const
export type VendorGroup = (typeof VENDOR_GROUPS)[number]

export function isVendorGroup(v: string): v is VendorGroup {
  return (VENDOR_GROUPS as readonly string[]).includes(v)
}

/** 解析清单：空 = 全量；含未知组抛错（调用方转构建失败）。 */
export function resolveVendorGroups(value: string | undefined): Set<VendorGroup> {
  const requested = (value ?? "")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean)
  const unknown = requested.filter((g) => !isVendorGroup(g))
  if (unknown.length) {
    throw new Error(`GEBAI_WEB_VENDOR 含未知组: ${unknown.join(", ")}（可用: ${VENDOR_GROUPS.join(", ")}）`)
  }
  return new Set<VendorGroup>(requested.length ? (requested as VendorGroup[]) : [...VENDOR_GROUPS])
}
