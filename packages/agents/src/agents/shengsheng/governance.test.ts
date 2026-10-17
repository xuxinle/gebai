/**
 * 治理三件套（授权面 / 收单门禁 / 定期简报）的断言面。
 *
 * 三条重心都在这一个文件里，因为它们的口径必须同源：
 *  ① **简报只报异常**——全是成功案例的简报等于噪音，人会停止看（那比不报更糟）。
 *  ② **三态且「没核」不冒充任何一侧**——授权面的 `unverified`、门禁的 `unknown`
 *     都不阻断，但必须留痕；把它们当阻断会让系统变成不干活（实测两次踩到）。
 *  ③ **未声明 ≠ 声明为空**——读不到配置时凭空造空配置，会让所有存量对象瞬间越界。
 *
 * 文件与实现同粒度：三者同属 `governance.ts`。
 */
import { describe, expect, test } from "bun:test"
import {
  applyForce,
  buildBriefing,
  decideScope,
  dueForBriefing,
  gateCommitScope,
  gateReverseVerify,
  judgeScope,
  pathsInDirection,
  renderBriefing,
  splitLedgerSections,
  summarize,
  underPrefix,
  type Authority,
} from "./governance"

const NOW = Date.parse("2026-10-17T12:00:00.000Z")
const H = 3_600_000

// ── 简报用的最小状态（只取 buildBriefing 真正读的字段）──────────────
function state(patch: { workers?: unknown[]; inflight?: unknown[]; seeds?: unknown[] } = {}) {
  return {
    workers: (patch.workers ?? []) as { alias: string }[],
    inflight: (patch.inflight ?? []) as { alias: string; dispatchedAt?: number }[],
    seeds: (patch.seeds ?? []) as { direction: string; outOfScope?: boolean }[],
  }
}

const ledgerWith = (times: number[]) =>
  times.map((t, i) => `## ${new Date(t).toISOString()}｜w${i}｜d\n- 方向：x\n- 会话：s${i}｜轮次 1\n### 交付：做了点事`).join("\n")

// ===========================================================================
// 定期简报
// ===========================================================================
describe("简报：只报异常", () => {
  test("一切正常（有收单、无停滞）⇒ **零待决项**，且 headline 说清事实", () => {
    const b = buildBriefing({
      now: NOW,
      state: state({ workers: [{ alias: "w1" }], seeds: [{ direction: "x" }] }),
      ledger: ledgerWith([NOW - 2 * H, NOW - 5 * H]),
      notifyReachable: true,
    })
    expect(b.decisions).toHaveLength(0)
    expect(b.counts.ledgerSections).toBe(2)
    expect(b.headline).toContain("收 2 单")
    expect(b.headline).toContain("待你决定 0 项")
  })

  test("有动作零进展 ⇒ 报「反复」（「有动作≠有进展」的机器化）", () => {
    const b = buildBriefing({
      now: NOW,
      state: state({ workers: [{ alias: "w1" }], inflight: [{ alias: "w1", dispatchedAt: NOW - H }] }),
      ledger: ledgerWith([NOW - 30 * H]), // 窗口内没有新节
    })
    const d = b.decisions.find((x) => x.kind === "repeated")!
    expect(d).toBeTruthy()
    expect(d.fact).toContain("没有任何一单收进台账")
    expect(d.options?.length).toBeGreaterThan(0)
  })

  test("在途挂太久 ⇒ 报「停滞」，且带小时数", () => {
    const b = buildBriefing({
      now: NOW,
      state: state({ inflight: [{ alias: "w9", dispatchedAt: NOW - 60 * H }], seeds: [{ direction: "x" }] }),
      ledger: ledgerWith([NOW - H]),
    })
    const d = b.decisions.find((x) => x.kind === "stalled")!
    expect(d.fact).toContain("60h")
  })

  test("种子与在途都空 ⇒ 报「方向枯」（要人给方向，不是自己编）", () => {
    const b = buildBriefing({ now: NOW, state: state(), ledger: ledgerWith([NOW - H]) })
    expect(b.decisions.map((d) => d.kind)).toContain("no-direction")
  })

  test("越权请求 ⇒ 报到人面前，且说明「AI 无权扩授权」", () => {
    const b = buildBriefing({
      now: NOW,
      state: state({ seeds: [{ direction: "改 packages/server 的鉴权", outOfScope: true }] }),
      ledger: ledgerWith([NOW - H]),
    })
    const d = b.decisions.find((x) => x.kind === "out-of-scope")!
    expect(d.why).toContain("无权扩大")
    expect(d.options).toContain("批准并入授权")
  })

  test("通知不可达 ⇒ 报出来（否则演进照跑、人收不到任何消息）", () => {
    const b = buildBriefing({ now: NOW, state: state({ seeds: [{ direction: "x" }] }), ledger: ledgerWith([NOW - H]), notifyReachable: false })
    expect(b.decisions.map((d) => d.kind)).toContain("unreachable")
  })
})

describe("简报：没核 ≠ 没问题", () => {
  test("台账读不到 ⇒ 进 gaps，**不报「无进展」**", () => {
    const b = buildBriefing({ now: NOW, state: state({ seeds: [{ direction: "x" }] }), ledger: null })
    expect(b.gaps.join()).toContain("台账读不到")
    expect(b.gaps.join()).toContain("没核")
    // 关键：不能因为读不到就说「零收单」
    expect(b.counts.ledgerSections).toBeNull()
    expect(b.headline).toContain("台账未核")
    expect(b.decisions.map((d) => d.kind)).not.toContain("repeated")
  })

  test("通知可达性未核 ⇒ 进 gaps（不是「可达」）", () => {
    const b = buildBriefing({ now: NOW, state: state({ seeds: [{ direction: "x" }] }), ledger: ledgerWith([NOW - H]) })
    expect(b.gaps.join()).toContain("未核")
  })

  test("renderBriefing 要短，且把「没有待决项」说清（避免被读成「一切良好」）", () => {
    const b = buildBriefing({ now: NOW, state: state({ seeds: [{ direction: "x" }] }), ledger: ledgerWith([NOW - H]), notifyReachable: true })
    const text = renderBriefing(b)
    expect(text).toContain("没有需要你决定的事")
    expect(text).toContain("不等于方向正确")
    expect(text.split("\n").length).toBeLessThan(30) // 超过一屏人不看
  })
})

describe("简报：窗口与节奏", () => {
  test("只数窗口内的台账节", () => {
    expect(splitLedgerSections(ledgerWith([NOW - H, NOW - 40 * H]))).toHaveLength(2)
    const b = buildBriefing({ now: NOW, state: state({ seeds: [{ direction: "x" }] }), ledger: ledgerWith([NOW - H, NOW - 40 * H]) })
    expect(b.counts.ledgerSections).toBe(1)
  })

  test("dueForBriefing：从没出过 ⇒ 该出；窗口未到 ⇒ 不该出", () => {
    expect(dueForBriefing(null, NOW)).toBe(true)
    expect(dueForBriefing(NOW - H, NOW)).toBe(false)
    expect(dueForBriefing(NOW - 25 * H, NOW)).toBe(true)
  })
})

// ===========================================================================
// 授权面
// ===========================================================================
describe("授权围栏：默认拒绝，但区分未核与越界", () => {
  const auth: Authority = {
    allowPaths: ["packages/agents/src/agents/shengsheng", "tools"],
    denyPaths: [".env", "packages/server/src/core"],
    declared: true,
  }

  test("在授权前缀内 ⇒ in-scope", () => {
    expect(judgeScope("改 packages/agents/src/agents/shengsheng 的 collect 门禁", auth, NOW).kind).toBe("in-scope")
  })

  test("按段比较：`toolshed` 不算在 `tools` 之下（否则「禁 tools」会误禁一片）", () => {
    expect(underPrefix("toolshed/x.py", "tools")).toBe(false)
    expect(underPrefix("tools/x.py", "tools")).toBe(true)
    expect(underPrefix("tools", "tools")).toBe(true)
  })

  test("文件族用 `*` 通配：`.env*` 禁住 `.env` / `.env.local`（否则顶层配置禁区形同虚设）", () => {
    const a2: Authority = { allowPaths: ["tools"], denyPaths: [".env*"], declared: true }
    expect(judgeScope("改 .env", a2, NOW).kind).toBe("out-of-scope")
    expect(judgeScope("改 .env.local", a2, NOW).kind).toBe("out-of-scope")
    // 反向：不写通配时 `.env` 只精确命中它自己（.envrc 不受影响）——两种语义不混
    expect(underPrefix(".envrc", ".env")).toBe(false)
    expect(underPrefix(".env", ".env")).toBe(true)
  })

  test("落在禁区 ⇒ out-of-scope，且禁区优先于允许区", () => {
    const v = judgeScope("改 tools/a.py，顺带调 packages/server/src/core/engine.ts", auth, NOW)
    expect(v.kind).toBe("out-of-scope")
    expect((v as { reason: string }).reason).toContain("禁区")
  })

  test("授权到期 ⇒ 视为无授权（不用「曾经授权过」充数）", () => {
    const expired: Authority = { ...auth, expiresAt: NOW - H }
    const v = judgeScope("改 tools/a.py", expired, NOW)
    expect(v.kind).toBe("out-of-scope")
    expect((v as { reason: string }).reason).toContain("到期")
  })

  test("allowPaths 为空（**显式声明为空**）⇒ 默认拒绝（不是默认允许）", () => {
    const v = judgeScope("改 tools/a.py", { allowPaths: [], declared: true }, NOW)
    expect(v.kind).toBe("out-of-scope")
    expect((v as { reason: string }).reason).toContain("尚未授与")
  })

  test("**未声明授权面（无文件）⇒ unverified，不是越界**（否则新项目/存量演进线一接即停）", () => {
    const v = judgeScope("改 tools/a.py", { allowPaths: [] }, NOW)
    expect(v.kind).toBe("unverified")
    // 且必须能照派——这是「不得从幕后把演进停掉」的底线
    expect(decideScope("改 tools/a.py", { allowPaths: [] }, NOW).action).toBe("accept")
  })

  test("方向里没有路径 ⇒ **unverified**（未核），不当作通过也不当作越界", () => {
    expect(judgeScope("把性能优化一下", auth, NOW).kind).toBe("unverified")
  })

  test("decideScope：未核（自然语言方向）⇒ **照派**但留痕（拦不住「说不清」，只能拦「明确越界」）", () => {
    const d = decideScope("把性能优化一下", auth, NOW)
    expect(d.action).toBe("accept")
    expect(d.note).toContain("未核")
    expect(d.note).toContain("照派")
  })

  test("反向：大量自然语言方向不得被围栏拦下（否则围栏从「限制越权」变成「停止演进」）", () => {
    for (const dir of ["修一下并发丢写", "把提示词精简但不丢信息", "补一条反向验证", "看下台账为什么没长"]) {
      expect(decideScope(dir, auth, NOW).action).toBe("accept")
    }
  })

  test("decideScope：落在禁区 ⇒ reject；只是没在授权内 ⇒ hold（可能是该扩授权）", () => {
    expect(decideScope("改 .env", auth, NOW).action).toBe("reject")
    const outside = decideScope("改 packages/web/src/main.ts", auth, NOW)
    expect(outside.action).toBe("hold")
    expect(outside.note).toContain("越界")
  })

  test("pathsInDirection：抽路径、去尾斜杠、忽略自然语言", () => {
    expect(pathsInDirection("改 tools/a.py 与 packages/x/y.ts")).toEqual(["tools/a.py", "packages/x/y.ts"])
    expect(pathsInDirection("优化性能")).toEqual([])
  })
})

// ===========================================================================
// 收单门禁
// ===========================================================================
describe("收单门禁：三态与总判", () => {
  test("全 pass 才 ok（「没有 fail」不等于 ok）", () => {
    expect(summarize([
      { gate: "anchors", state: "pass", detail: "", forceable: false },
      { gate: "commit-scope", state: "pass", detail: "", forceable: false },
    ]).ok).toBe(true)

    const oneUnknown = summarize([
      { gate: "anchors", state: "pass", detail: "", forceable: false },
      { gate: "commit-scope", state: "unknown", detail: "读不到 git", forceable: false },
    ])
    // 关键：unknown 也挡住入账——「没核」与「核过没问题」是两回事
    expect(oneUnknown.ok).toBe(false)
    expect(oneUnknown.blockers.join()).toContain("未核成")
  })

  test("unknown 与 fail 在 blockers 里措辞可区分", () => {
    const r = summarize([
      { gate: "anchors", state: "fail", detail: "缺 2 条", forceable: false },
      { gate: "commit-scope", state: "unknown", detail: "读不到", forceable: false },
    ])
    expect(r.blockers).toHaveLength(2)
    expect(r.blockers[0]).not.toContain("未核成")
    expect(r.blockers[1]).toContain("未核成")
  })
})

describe("收单门禁：强制跳过必须留痕", () => {
  const base = summarize([{ gate: "reverse-verify", state: "fail" as const, detail: "vacuous", forceable: true }])

  test("带理由的跳过：过门，但**另记一条**进 skipped", () => {
    const r = applyForce(base, [{ gate: "reverse-verify", reason: "非机制类：纯文档改动" }])
    expect(r.ok).toBe(true)
    expect(r.skipped).toHaveLength(1)
    expect(r.skipped[0]).toContain("已强制跳过")
    expect(r.skipped[0]).toContain("原判 fail") // 原来的判断必须留在痕里
  })

  test("理由为空 ⇒ 跳过**不成立**（静默跳过等于没有门禁）", () => {
    const r = applyForce(base, [{ gate: "reverse-verify", reason: "   " }])
    expect(r.ok).toBe(false)
    expect(r.skipped).toHaveLength(0)
  })

  test("只跳指定的那一门，别的不受影响", () => {
    const two = summarize([
      { gate: "reverse-verify", state: "fail" as const, detail: "vacuous", forceable: true },
      { gate: "anchors", state: "fail" as const, detail: "缺 1 条", forceable: false },
    ])
    const r = applyForce(two, [{ gate: "reverse-verify", reason: "非机制类" }])
    expect(r.ok).toBe(false)
    expect(r.blockers.join()).toContain("anchors")
  })
})

describe("门②提交范围：读不到 ≠ 没问题", () => {
  test("核过且全在范围内 ⇒ pass", () => {
    expect(gateCommitScope({ outOfScope: [], inScope: 3 }).state).toBe("pass")
  })

  test("有不在工单里的文件 ⇒ fail，且逐条列出（+行/−行）", () => {
    const o = gateCommitScope({ outOfScope: [{ path: "src/other-worker.ts", added: 596, deleted: 0 }], inScope: 1 })
    expect(o.state).toBe("fail")
    expect(o.detail).toContain("src/other-worker.ts")
    expect(o.detail).toContain("+596")
  })

  test("读不到 git ⇒ **unknown**，绝不当 pass（最需要警报的环境恰好最安静）", () => {
    const o = gateCommitScope(null, "不在 git 仓内")
    expect(o.state).toBe("unknown")
    expect(o.detail).toContain("不等于")
  })
})

describe("门③反向验证：没声明 ≠ 通过", () => {
  test("未声明 spec ⇒ unknown 且标记可强制跳过", () => {
    const o = gateReverseVerify(false, null)
    expect(o.state).toBe("unknown")
    expect(o.forceable).toBe(true)
  })

  test("genuine ⇒ pass", () => {
    expect(gateReverseVerify(true, { verdict: "genuine", reason: "四档全过" }).state).toBe("pass")
  })

  test("空壳类判决（vacuous/empty/broken/wrong-cause）⇒ fail", () => {
    for (const v of ["vacuous", "empty", "broken", "wrong-cause"]) {
      expect(gateReverseVerify(true, { verdict: v, reason: `${v} 的理由` }).state).toBe("fail")
    }
  })

  test("没跑起来 / 抖动 ⇒ unknown（不可判定 ≠ 失败）", () => {
    for (const v of ["not-run", "unstable"]) {
      expect(gateReverseVerify(true, { verdict: v, reason: `${v}` }).state).toBe("unknown")
    }
  })
})
