/**
 * 按工具名强制审批（人格会话的 `askTools` 名单）与请求级审批策略的**顺序**。
 *
 * 这条链上曾有两个相反的坏形态，都必须钉住：
 *
 * | 顺序 | 人格会话（WS，能问人） | 接口请求（interactionMode: none） |
 * |---|---|---|
 * | `approvalPolicy` 先判（曾如此） | ❌ 名单整体失效，名单里的 sh 被直接执行 | （无感） |
 * | 强制审批提到最前、不管通道 | ✅ 名单生效 | ❌ 拦下来白等 5 分钟再拒 → 工具在接口上不可用 |
 * | **现在**：deny → 强制审批(仅能问人) → auto | ✅ | ✅ 退回普通免审流程 |
 *
 * 关键判据是 `interactionMode`：审批的前提是**有人能点头**。无人可批的通道上，
 * “必须问人”这个约束没有落地处，硬拦只会把工具打成不可用。
 */
import { describe, expect, test } from "bun:test"
import { AgentEngine } from "./engine"

/** 引擎真实判定入口（private，按最小控制变量实验直接取用——不经 run 免得起全套依赖）。 */
type Probe = {
  isApprovalSkipped: (sid: string, user: string, env: Record<string, string>, tool?: string) => Promise<boolean>
}

/** 造一个只装 `isApprovalSkipped` 所需依赖的引擎实例。 */
function probe(opts: {
  askTools: string[]
  /** 人格会话 = true（runner 传 autoApprove:true）；接口显式收紧 = false；不传 = 普通会话。 */
  autoApprove?: boolean
  /** 通道能力：缺省 realtime（能被问）；"none" = 接口请求（无人可批）。 */
  interactionMode?: "realtime" | "multi_turn" | "none"
  env?: Record<string, string>
  sessionEnv?: Record<string, string>
}): Probe {
  const engine = Object.create(AgentEngine.prototype) as Record<string, unknown>
  const task: Record<string, unknown> = { user: "admin", env: {}, interactionMode: opts.interactionMode ?? "realtime" }
  if (opts.autoApprove !== undefined) task.approvalPolicy = opts.autoApprove ? "auto" : "deny"
  ;(engine as { tasks: Map<string, unknown> }).tasks = new Map([["s1", task]])
  ;(engine as { opts: unknown }).opts = {
    authMode: "local",
    store: { getEnv: async () => opts.sessionEnv ?? {} },
    forceApprovalFor: (_sid: string, toolName: string) => opts.askTools.includes(toolName),
  }
  return engine as unknown as Probe
}

const ASK = ["sh", "file_delete"]
/** 免审批（直接执行）。 */
const SKIP = true
/** 需要审批（弹卡 / 或无人可批时走硬门槛）。 */
const ASK_HUMAN = false

describe("人格会话（能问人）：名单压过会话级全免", () => {
  test("**autoApprove:true（人格 runner 的真实形态）+ 名单内工具 → 要审批**", async () => {
    const r = await probe({ askTools: ASK, autoApprove: true }).isApprovalSkipped("s1", "admin", {}, "sh")
    console.log(`    实测: 人格会话 sh 免审=${r}`)
    expect(r).toBe(ASK_HUMAN)
  })

  test("名单外工具 → 仍免审（整体不弹卡的语义保留）", async () => {
    const r = await probe({ askTools: ASK, autoApprove: true }).isApprovalSkipped("s1", "admin", {}, "read")
    expect(r).toBe(SKIP)
  })

  test("multi_turn 通道（人格 runner 实际传的交互模式）同样拦名单内工具", async () => {
    const r = await probe({ askTools: ASK, autoApprove: true, interactionMode: "multi_turn" }).isApprovalSkipped("s1", "admin", {}, "sh")
    expect(r).toBe(ASK_HUMAN)
  })

  test("会话 env 开了 GEBAI_APPROVAL_SKIP → 名单依然压过它", async () => {
    const r = await probe({ askTools: ASK, env: { GEBAI_APPROVAL_SKIP: "true" } }).isApprovalSkipped(
      "s1",
      "admin",
      { GEBAI_APPROVAL_SKIP: "true" },
      "sh",
    )
    console.log(`    实测: 会话 env 全免 + 名单内 = 免审 ${r}`)
    expect(r).toBe(ASK_HUMAN)
  })

  test("会话内存态 env 开了跳过 → 名单依然压过它", async () => {
    const r = await probe({ askTools: ASK, sessionEnv: { GEBAI_APPROVAL_SKIP: "true" } }).isApprovalSkipped("s1", "admin", {}, "sh")
    expect(r).toBe(ASK_HUMAN)
  })
})

describe("接口通道（无人可批）：名单不得把工具打成不可用", () => {
  test("**interactionMode:none + 名单内工具 → 免审直接执行**（拦下来只会白等超时）", async () => {
    const r = await probe({ askTools: ASK, autoApprove: true, interactionMode: "none" }).isApprovalSkipped("s1", "admin", {}, "sh")
    console.log(`    实测: 接口请求 sh 免审=${r}`)
    expect(r).toBe(SKIP)
  })

  test("接口请求未传 autoApprove（本地模式）→ 仍是无交互自动通过", async () => {
    const r = await probe({ askTools: ASK, interactionMode: "none" }).isApprovalSkipped("s1", "admin", {}, "sh")
    expect(r).toBe(SKIP)
  })

  test("接口请求显式 autoApprove:false（deny）→ 不执行（与通道无关的最高优先）", async () => {
    const r = await probe({ askTools: ASK, autoApprove: false, interactionMode: "none" }).isApprovalSkipped("s1", "admin", {}, "sh")
    expect(r).toBe(ASK_HUMAN)
  })
})

describe("deny 是唯一的最高优先：名单也不例外", () => {
  test("请求级 deny + 名单外工具 → 同样不执行", async () => {
    const r = await probe({ askTools: ASK, autoApprove: false }).isApprovalSkipped("s1", "admin", {}, "read")
    expect(r).toBe(ASK_HUMAN)
  })

  test("请求级 deny 即使通道能问人也不放行名单内工具（拒绝，而不是弹卡）", async () => {
    const r = await probe({ askTools: ASK, autoApprove: false, interactionMode: "realtime" }).isApprovalSkipped("s1", "admin", {}, "sh")
    expect(r).toBe(ASK_HUMAN)
  })
})

describe("不装钩子时行为不变（普通会话不受影响）", () => {
  test("名单为空 → 人格会话整体免审", async () => {
    const r = await probe({ askTools: [], autoApprove: true }).isApprovalSkipped("s1", "admin", {}, "sh")
    expect(r).toBe(SKIP)
  })

  test("普通会话（无 approvalPolicy、无 env）→ 不跳过（走常规审批）", async () => {
    const r = await probe({ askTools: ASK }).isApprovalSkipped("s1", "admin", {}, "sh")
    expect(r).toBe(ASK_HUMAN)
  })

  test("普通会话 + env 打开跳过 → 跳过（既有语义不变）", async () => {
    const r = await probe({ askTools: ASK, env: { GEBAI_APPROVAL_SKIP: "true" } }).isApprovalSkipped(
      "s1",
      "admin",
      { GEBAI_APPROVAL_SKIP: "true" },
      "read",
    )
    expect(r).toBe(SKIP)
  })
})
