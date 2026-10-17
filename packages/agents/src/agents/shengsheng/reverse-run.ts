/**
 * 反向验证的**执行面**：按 spec 采出观察集，交 `verify-gate.judge` 裁决。
 *
 * ## 为何在 TS 里做，而不是一个独立 Python CLI
 *
 * 本仓已有的回退型门禁（`reverse_verify.py`）是为**人跑的单次门禁**设计的：
 * 它有 CLI 参数、信号陷阱、`--force` 逃生口、stdout 报告。而这里需要的是
 * **收单路径上的一个函数**：输入 spec、输出观察集、失败就抛。把它做成脚本意味着
 * 再造一套参数解析、退出码语义与输出格式，且两条路径的判定会漂成两份。
 *
 * 所以执行也在这里：复用宿主已注入的 `ctx.runCommand`（与 `repoTopOf` 同一通道）、
 * 复用宿主的文件读写（与 `mutateControl` 同一套原子写），不新增命令行入口。
 *
 * ## 安全约束（每条都对应一个已知失效形态）
 *
 * ① **改前先备份、改后逐字节校验**：源码停在半成品上比「验证没过」严重得多。
 * ② **空操作拦截**：`rollback === defect` 或 `rollback` 不在文件里 ⇒ 不跑命令，
 *    直接判 `unsupported`（本仓真案例：空操作回退让「0 条红」看起来像通过）。
 * ③ **跑不起来记为 `null`**，不冒充绿也不冒充红——由判据判 `not-run`。
 * ④ **收尾必回填**（`finally`）：中断路径也要走，回填失败即抛（不许静默继续）。
 */

import type { ToolContext } from "@gebai/sdk"
import { judge, type GateResult, type Observation, type ReverseSpec } from "./verify-gate"

/** 单个状态下的观察键（与 `judge` 的观察集键一致）。 */
type ObsKey = "original" | "rolled" | "restored" | "original2" | "rolled2"

/** 采集结果：观察集 + 判定。 */
export interface ReverseRun {
  gate: GateResult
  observations: Partial<Record<ObsKey, Observation>>
  /** 回填是否成功（`false` 时源码状态不可信，调用方必须当成失败）。 */
  restored: boolean
}

const runOf = (ctx: ToolContext) => (ctx as { runCommand?: NonNullable<ToolContext["runCommand"]> }).runCommand

/** 跑一次命令 → 观察。**跑不起来记 `code: null`**（超时/命令不存在），不冒充任何一侧。 */
async function observe(ctx: ToolContext, cmd: string, workdir: string, timeoutMs: number): Promise<Observation> {
  const run = runOf(ctx)
  if (typeof run !== "function") return { code: null, tail: "宿主不提供 runCommand" }
  try {
    const r = await run(cmd, { workdir, timeoutMs })
    return { code: r.code, tail: `${r.stdout ?? ""}${r.stderr ?? ""}`.slice(-800) }
  } catch (e) {
    return { code: null, tail: `跑不起来：${String(e).slice(0, 200)}` }
  }
}

/**
 * 按 spec 采集观察集并判定。
 *
 * 三态各观测一次 + `original`/`rolled` 各复跑一次（抖动判据）。
 * 调用方 `await` 它时会**真的改一次源码再改回来**——这是反向验证的本性，
 * 无法避免；能避免的是「改坏了不留痕」（靠 finally 回填 + 失败即抛）。
 */
export async function runReverseVerify(
  ctx: ToolContext,
  spec: ReverseSpec,
  opts: { repoRoot: string; timeoutMs?: number } ,
): Promise<ReverseRun> {
  const timeoutMs = opts.timeoutMs ?? 600_000
  const empty: Partial<Record<ObsKey, Observation>> = {}

  // 前置：spec 自洽（不跑命令就能判的，不浪费一次源码改动）
  const pre = judge(spec, {})
  if (pre.verdict === "empty") return { gate: pre, observations: empty, restored: true }

  const target = spec.file.startsWith("/") ? spec.file : `${opts.repoRoot.replace(/[\\/]+$/, "")}/${spec.file}`
  let before = ""
  try {
    before = await ctx.readFile(target)
  } catch (e) {
    return {
      gate: { verdict: "unsupported", reason: `读不到回退目标（${spec.file}）：${String(e).slice(0, 120)}`, hints: [] },
      observations: empty,
      restored: true,
    }
  }
  if (!before.includes(spec.rollback)) {
    // 锚点未命中 = 无法干预。既不是通过也不是失败。
    return {
      gate: { verdict: "unsupported", reason: `rollback 锚点不在 ${spec.file} 里（代码已变或 spec 过期）——无法干预`, hints: [] },
      observations: empty,
      restored: true,
    }
  }

  const observations: Partial<Record<ObsKey, Observation>> = {}
  let restored = true
  try {
    // ① original（+ 复跑）
    observations.original = await observe(ctx, spec.cmd, opts.repoRoot, timeoutMs)
    observations.original2 = await observe(ctx, spec.cmd, opts.repoRoot, timeoutMs)

    // ② rolled：把 rollback 换成 defect（只换一处）
    await ctx.writeFile(target, before.replace(spec.rollback, spec.defect))
    observations.rolled = await observe(ctx, spec.cmd, opts.repoRoot, timeoutMs)
    observations.rolled2 = await observe(ctx, spec.cmd, opts.repoRoot, timeoutMs)
  } finally {
    // ③ 无论怎么退出都要回填；回填后校验（逐字节）
    try {
      await ctx.writeFile(target, before)
      const now = await ctx.readFile(target)
      restored = now === before
    } catch {
      restored = false
    }
  }

  // ④ restored：回填之后**再观测一次**——用来把「命令本身就红」筛掉
  if (restored) observations.restored = await observe(ctx, spec.cmd, opts.repoRoot, timeoutMs)

  if (!restored) {
    return {
      gate: {
        verdict: "unsupported",
        reason: `回填失败：${spec.file} 没能回到起跑前（源码状态不可信，本次结论作废，请先 git checkout -- 该文件）`,
        hints: [],
      },
      observations,
      restored: false,
    }
  }
  return { gate: judge(spec, observations), observations, restored: true }
}
