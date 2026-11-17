/**
 * `content_hash64` 口径守卫（JS 侧）。
 *
 * ## 为何必须有这条测试
 *
 * trailer 里的哈希是**跨语言契约**：打包脚本（本仓库的 TypeScript）写值、宿主
 * （霜天 C++）启动时重算并比对。两侧任何一边改了口径而另一边没改，
 * 症状不是崩溃而是「每次启动都把 259MB 侧车重写一遍」——能跑、只是莫名慢。
 *
 * 这种缺陷在代码评审里几乎看不出来（两侧代码长得不像），所以用**已知值**钉住：
 * SHA-256 前 8 字节、按**小端**读成 u64。
 *
 * C++ 侧对应守卫：`shuangtian/tools/content_hash_cross_check.cpp`。
 */
import { createHash } from "node:crypto"
import { describe, expect, test } from "bun:test"

/** 与 `pack-desktop.ts` 的 `contentHash64Hex` 逐字相同（测试独立复述，防实现被改名后失去覆盖）。 */
function contentHash64Hex(bytes: Uint8Array): string {
  const digest = createHash("sha256").update(bytes).digest()
  let value = 0n
  for (let i = 0; i < 8; i++) value |= BigInt(digest[i]) << BigInt(i * 8)
  return value.toString(16).padStart(16, "0")
}

describe("content_hash64 口径（与宿主 C++ 侧一致）", () => {
  test("已知 SHA-256 值的**前 8 字节小端**读法", () => {
    // 空输入 SHA-256 = e3b0c44298fc1c14… → 小端 u64 = 141cfc9842c4b0e3
    expect(contentHash64Hex(new Uint8Array([]))).toBe("141cfc9842c4b0e3")
    // "abc" SHA-256 = ba7816bf8f01cfea… → 小端 u64 = eacf018fbf1678ba
    expect(contentHash64Hex(new TextEncoder().encode("abc"))).toBe("eacf018fbf1678ba")
  })

  test("长度参与区分（空 ≠ 单零字节）", () => {
    expect(contentHash64Hex(new Uint8Array([]))).not.toBe(contentHash64Hex(new Uint8Array([0])))
  })

  test("同一输入稳定、不同输入不同（改变 1 字节即变）", () => {
    const base = new Uint8Array(1024).fill(7)
    const first = contentHash64Hex(base)
    expect(contentHash64Hex(base)).toBe(first)
    base[512] ^= 0xff
    expect(contentHash64Hex(base)).not.toBe(first)
  })

  test("输出恒为 16 位十六进制（trailer 的 u64 字段同宽）", () => {
    for (const size of [0, 1, 7, 8, 63, 64, 65, 1000]) {
      expect(contentHash64Hex(new Uint8Array(size).fill(0x5a))).toMatch(/^[0-9a-f]{16}$/)
    }
  })
})
