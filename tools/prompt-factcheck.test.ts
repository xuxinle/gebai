/**
 * `prompt-factcheck` 的断言面。
 *
 * 它守的是「精简提示词不丢硬事实」，所以测试重心是**判别力**：
 * 阈值被删必须报，而无关改写不得误报。
 */
import { describe, expect, test } from "bun:test"
import { diff, digits, extract } from "./prompt-factcheck"

const FENCE = "```"
const BASE = [
  "上限 `150_000`，env `SHENGSHENG_REUSE_ADVISORY_CTX` 可改。",
  "跑 `bun test x.test.ts` 验；路径 `packages/agents/src/x.ts`。",
  FENCE,
  "python3 tools/shengsheng_sentinel.py --anchors",
  FENCE,
].join("\n")

describe("prompt-factcheck", () => {
  test("四类硬事实都能抽到", () => {
    const f = extract(BASE)
    expect(f.code).toContain("150_000")
    expect(f.env).toContain("SHENGSHENG_REUSE_ADVISORY_CTX")
    expect(f.path).toContain("packages/agents/src/x.ts")
    expect(f.cmd.some((c) => c.includes("--anchors"))).toBe(true)
  })

  test("删掉阈值 ⇒ 必报（这是它存在的全部理由）", () => {
    const next = BASE.replace("150_000", "某个数")
    const r = diff(BASE, next)
    expect(r.missing.some((m) => m.fact === "150_000")).toBe(true)
    expect(r.missingNums).toContain("150_000")
  })

  test("删掉代码块里的整行命令 ⇒ 必报", () => {
    const r = diff(BASE, BASE.replace("python3 tools/shengsheng_sentinel.py --anchors", ""))
    expect(r.missing.some((m) => m.kind === "命令")).toBe(true)
  })

  test("只做改写（字面保留）⇒ 不误报", () => {
    const next = BASE.replace("可改。", "也可以改。")
    expect(diff(BASE, next).missing).toHaveLength(0)
  })

  test("digits 认下划线阈值写法", () => {
    expect(digits("512_000 与 1500 与 3")).toEqual(["1500", "512_000"])
  })
})
