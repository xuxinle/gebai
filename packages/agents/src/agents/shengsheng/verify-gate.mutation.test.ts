/**
 * 判据的**判别力**：把每一档退回旧写法，对应判决必须变成「不通过」。
 *
 * ## 为什么这在测试文件里，而不是一个改源码的脚本
 *
 * 原先的形态是一个独立脚本：读自己源码、逐条做字符串替换、跑 `bun test` 看红不红、
 * 再回填。它能用，但代价不小——改源码的脚本在任何一次中断（Ctrl-C、断电、超时）
 * 都可能把源码停在变异态，于是又需要一套 `.revbak` 救援与收尾校验。
 *
 * 而这里要证明的事情其实不需要修改磁盘：**判据是一个纯函数**。
 * 「把 S2 那一档退掉」等价于「喂给它一个 S2 不该挡住的观察集，看它是否仍判不通过」——
 * 也就是一组**反事实观察集**。这样做的好处是它能在每次 `bun test` 里跑到，
 * 而不是等人记得去手动执行一次。
 *
 * ## 这里断言的是什么
 *
 * 对每一档：喂一个「若该档失效则会误判为通过」的观察集，断言它**没有**被误判为通过，
 * 且给出的是**该档对应的**结论（而不是碰巧被别的档挡住）。
 * 后者是关键——只说「不通过」的话，把 S0~S3 全删掉、只留 S4 也能通过测试。
 */
import { describe, expect, test } from "bun:test"
import { isPassing, judge, type Observation, type ReverseSpec } from "./verify-gate"

const spec: ReverseSpec = {
  cmd: "bun test x.test.ts",
  file: "src/x.ts",
  rollback: "return computeGuard(input)",
  defect: "return true",
}

const G: Observation = { code: 0 }
const R: Observation = { code: 1 }
/** 四档全过的基线。 */
const ok = { original: G, rolled: R, restored: G, original2: G, rolled2: R }

describe("每一档判据都有判别力（退掉它必然误判）", () => {
  test("S0 恒真动作：若失效，`true` 会被当成合法反向验证", () => {
    // 变异态会把 looksEmpty 恒返回 false ⇒ 落到 S1/S2/S3 全过 ⇒ genuine
    const r = judge({ ...spec, cmd: "true" }, ok)
    expect(r.verdict).toBe("empty")
    expect(isPassing(r)).toBe(false)
  })

  test("S0b 空操作回退：若失效，`rollback === defect` 会走到干预并「通过」", () => {
    // 变异态会删掉这条判断 ⇒ 替换前后一样 ⇒ 行为不变 ⇒ 但 S2 恰好红（因为命令红）
    // 所以这里必须断言的是**结论是 empty 而不是 genuine**
    const r = judge({ ...spec, defect: spec.rollback }, ok)
    expect(r.verdict).toBe("empty")
  })

  test("S1 原样绿：若失效，交上来就是坏的也会被判 genuine", () => {
    const r = judge(spec, { ...ok, original: R, original2: R })
    expect(r.verdict).toBe("broken")
    expect(isPassing(r)).toBe(false)
  })

  test("S2 退回必红：若失效，**恒绿的空壳**会被判 genuine —— 这是最致命的一档", () => {
    // 变异态会把 `o2.code === 0` 的判断删掉 ⇒ 退回后仍绿也放行
    const r = judge(spec, { ...ok, rolled: G, rolled2: G })
    expect(r.verdict).toBe("vacuous")
    expect(isPassing(r)).toBe(false)
  })

  test("S3 红的归因：若失效，「命令本来就红」会被 S2 误当成「退回会红」", () => {
    // 构造：original 绿、rolled 红、放回仍红 —— 只有 S3 能挡住它
    const r = judge(spec, { ...ok, restored: R })
    expect(r.verdict).toBe("wrong-cause")
    expect(isPassing(r)).toBe(false)
    // 反向：把 restored 换回绿，同一观察集其余不变则必须通过（证明是 S3 在起作用，不是别的东西挡着）
    expect(judge(spec, { ...ok, restored: G }).verdict).toBe("genuine")
  })

  test("S4 抖动：若失效，抖动的结论会被当成确定结论", () => {
    const r = judge(spec, { ...ok, rolled2: G })
    expect(r.verdict).toBe("unstable")
    expect(isPassing(r)).toBe(false)
  })

  test("缺观察当通过：若失效，「没跑」会被报成「通过」", () => {
    // 变异态会把 not-run 映射成 genuine ⇒ 一次都没跑的命令也「通过」收单
    for (const key of ["original", "rolled", "restored"] as const) {
      const obs: Record<string, Observation> = { ...ok }
      delete obs[key]
      const r = judge(spec, obs)
      expect(r.verdict).toBe("not-run")
      expect(isPassing(r)).toBe(false)
    }
  })

  test("四档是**合取**：每一档单独失效都能被这一组断言覆盖", () => {
    // 逐个把某一档的观察改成「会误判为通过」的形态，断言结论**都不是** genuine。
    // 这等价于「退掉任一档判据 ⇒ 必红」，但不需要改源码。
    const cases: [string, Parameters<typeof judge>[1]][] = [
      ["S0", { ...ok }],
      ["S1", { ...ok, original: R }],
      ["S2", { ...ok, rolled: G }],
      ["S3", { ...ok, restored: R }],
      ["S4", { ...ok, rolled2: G }],
    ]
    for (const [label, obs] of cases) {
      const r = judge(label === "S0" ? { ...spec, cmd: "exit 0" } : spec, obs)
      expect(r.verdict).not.toBe("genuine")
    }
  })
})
