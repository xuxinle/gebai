/**
 * 脚本桥共用的工具 RPC 分发层：`js`（stdio 行协议，子进程内 `tools.call`）与 `py`
 * （回环 socket 行协议，仅本地模式）两条桥共用同一套守卫——两处各写一份必然漂移，
 * 漏掉任一条即等于多出一条绕过通道。
 *
 * 守卫顺序（与引擎派发同口径，逐条对应用户可见承诺）：
 * 1. 名称容错归一（分隔符/驼峰偏差，`core/base/tool-args`）——不因风格误差报未知工具；
 * 2. 同类桥重入守卫（`bridgeReentryGuard`）：目标是脚本桥工具且该语言已在链中即拒（另一种语言
 *    首次进入放行——py→js / js→py 一层混合编排合法）；
 * 3. 未知工具 / 参数键容错归一（schema 驱动）；
 * 4. 运行时定义工具仅 depth 0 可调（防递归嵌套子进程）；
 * 5. 安全模式硬阻断集（cron 调度类）在分发层同规则拦截（无绕过通道）——`sh`/`py`/`write`
 *    等风险工具不在此拦截，各自在 execute 内降级（白名单/审计钩子/写范围）；
 * 6. 调用总数上限（防脚本放大）；
 * 7. 必填参数校验（缺参即拒绝并列出参数名，近似类型检查的即时反馈）；
 * 8. 免审运行拦截：免审（`approval:false`）运行或免审动态工具的脚本体未经用户审阅，
 *    内部需审批的工具按**剥离免审标记后**的审批姿态拒绝（`stripApprovalFlags`，
 *    防脚本内再传 `approval:false` 自我免审）；默认审批运行的一次审批覆盖内部调用；
 * 9. 执行（ctx 携带**语言链**（`bridgeLangs`）与结果封顶（output/data 单字段截断、
 *    blocks 与子会话存档透传给调用方组装）——链逐层追加，js/py 各自的 execute 据此判定重入。
 */
import type { ContentBlock, SubSessionArchive } from "@gebai/sdk"
import { normalizeToolArgs, tolerantToolName } from "../base/tool-args"
import type { ToolContext } from "../base/types"
import { isToolBlockedInSafeMode, safeModeRestrictionMsg, stripApprovalFlags } from "../security/safety"

/** 单次脚本执行内工具调用总数上限（js/py 同口径）。 */
export const BRIDGE_TOOL_MAX_CALLS = 100
/** 单条 RPC 结果字段字符上限（防巨对象撑爆协议行与内存）。 */
export const BRIDGE_FIELD_CAP = 100_000
/** 内层工具 blocks 透传上限（去重后；图片/图表等重内容限量，防巨量 blocks 撑爆结果）。 */
export const BRIDGE_BLOCKS_CAP = 10

/** 调用计数（跨一次脚本运行共享；调用方负责新建与上限，`max` 缺省 BRIDGE_TOOL_MAX_CALLS）。 */
export interface BridgeCallCounter {
  n: number
  max?: number
}

export interface BridgeDispatchOptions {
  /** 脚本语言标签（"js"/"py"）：用于错误文案与嵌套守卫。 */
  script: "js" | "py"
  /** 调用计数（可选；不传则不设上限——测试/无计数场景）。 */
  counter?: BridgeCallCounter
  /** 子进程层级：0（缺省）=脚本本体；>0=动态工具运行器（仅 js，运行时定义工具不可再调用）。 */
  depth?: number
  /** 本次脚本运行是否免审（内部需审批工具拦截）。 */
  approvalFree?: boolean
}

/** 工具调用成功结果（字段已封顶；blocks 由调用方去重限量汇集）。 */
export interface BridgeToolSuccess {
  output: string
  data: unknown
  blocks: ContentBlock[]
  truncated: boolean
  filePath: string | null
  subSessionArchive: SubSessionArchive | null
}

export type BridgeToolReply =
  | { name: string; ok: true; result: BridgeToolSuccess }
  | { name: string; ok: false; error: string }

/** 脚本桥语言（链式重入判定的对象）。 */
const BRIDGE_LANGS = ["js", "py"] as const

/** 同类脚本桥重入守卫：目标是脚本桥工具（js/py，含 `{agent}_{lang}` 命名空间形态）且该语言已在链中
 *  → 拒绝。**另一种语言首次进入放行**——py→js / js→py 一层混合编排合法且自然终止（链只增不减，
 *  任一语言第二次出现即拒；链长上限 2，最多再由 2 层脚本子进程）。
 *  **为何必须在分发层硬拒绝**（而非只靠各桥 execute 自检）：js 的 execute 对重入是「返回拒绝文本」
 *  （对主循环友好，模型读文本自愈），但脚本桥内是**程序化调用**——软拒绝会被脚本当正常返回值继续
 *  处理（`try/except` 也抓不到），故分发层硬拒绝（脚本侧表现为工具级异常），与两侧桥形态一致。 */
export function bridgeReentryGuard(chain: string[], name: string): string | null {
  for (const lang of BRIDGE_LANGS) {
    if (chain.includes(lang) && (name === lang || name.endsWith(`_${lang}`))) {
      return `不能再调用 ${lang}：本脚本桥调用链中已含 ${lang}（同类脚本桥不可重入，防嵌套子进程）；需要 shell 用 tools.sh`
    }
  }
  return null
}

/** 内层工具 blocks 的去重限量汇集（调用方持有 seen/acc，跨多次调用去重）。 */
export function collectBridgeBlocks(seen: Set<string>, acc: ContentBlock[], blocks: ContentBlock[]): void {
  for (const b of blocks) {
    if (seen.size >= BRIDGE_BLOCKS_CAP) return
    const key = `${b.type}:${(b as { path?: string; name?: string }).path ?? (b as { name?: string }).name ?? ""}`
    if (seen.has(key)) continue
    seen.add(key)
    acc.push(b)
  }
}

/** 分发一次脚本内的工具调用（守卫顺序见模块注释）。返回结构化结果，由调用方格式化回脚本。 */
export async function dispatchBridgeTool(
  call: { name: unknown; params?: unknown },
  ctx: ToolContext,
  opts: BridgeDispatchOptions,
): Promise<BridgeToolReply> {
  const name = tolerantToolName(String(call.name ?? ""))
  const rawParams = (call.params && typeof call.params === "object" ? call.params : {}) as Record<string, unknown>
  const chain = ctx.bridgeLangs ?? []
  // 重入判定用「已进入链 + 本桥语言」：顶层 js 调 js、py→js→js 等均命中；
  // chain 本身只是已进入部分，不含本次调用的发起方。
  const denied = bridgeReentryGuard([...chain, opts.script], name)
  if (denied) return { name, ok: false, error: denied }
  const rt = ctx.registry.resolve(name)
  if (!rt) return { name, ok: false, error: `未知工具: ${name}` }
  const params = normalizeToolArgs(rt.tool, rawParams)
  if (rt.tool.runtimeDefined && (opts.depth ?? 0) > 0) {
    return { name: rt.name, ok: false, error: `动态工具 ${rt.name} 不能在 ${opts.script}/动态工具内调用（防递归嵌套子进程）` }
  }
  if (ctx.safeMode && isToolBlockedInSafeMode(rt.name)) {
    return { name: rt.name, ok: false, error: safeModeRestrictionMsg(rt.name) }
  }
  if (opts.counter) {
    opts.counter.n += 1
    const max = opts.counter.max ?? BRIDGE_TOOL_MAX_CALLS
    if (opts.counter.n > max) return { name: rt.name, ok: false, error: `工具调用总数超上限（>${max}），请精简脚本或拆分执行` }
  }
  const required = Array.isArray((rt.tool.parameters as { required?: unknown } | undefined)?.required)
    ? (rt.tool.parameters as { required: string[] }).required
    : []
  const missing = required.filter((k) => params[k] === undefined)
  if (missing.length) {
    const props = Object.keys((rt.tool.parameters as { properties?: Record<string, unknown> }).properties ?? {})
    return {
      name: rt.name,
      ok: false,
      error: `工具 ${rt.name} 缺少必填参数: ${missing.join(", ")}${props.length ? `（参数: ${props.join(", ")}）` : ""}，请修正后重试`,
    }
  }
  if (opts.approvalFree) {
    const ra = rt.tool.requiresApproval
    let needs: boolean
    if (typeof ra === "function") {
      try {
        needs = !!(await ra(stripApprovalFlags(params) as Record<string, unknown>, ctx))
      } catch {
        needs = true
      }
    } else {
      needs = !!ra
    }
    if (needs) {
      return {
        name: rt.name,
        ok: false,
        error: `本次 ${opts.script} 以免审模式（approval:false）运行，内部调用需审批的工具 ${rt.name} 被拒绝。请去掉 approval:false（整体审批覆盖内部调用），或改用只读/免审工具`,
      }
    }
  }
  try {
    // 语言链（脚本桥标记）：把本次桥语言追加后传给被调工具——本桥的 execute 据此判定重入，
    // 并继续向更深一层传递（封死「脚本桥 → 直执行工具 → 脚本桥」的递归）
    const r = await rt.tool.execute(params, { ...ctx, bridgeLangs: [...chain, opts.script] })
    const cap = (s: unknown): string => {
      const t = s == null ? "" : String(s)
      return t.length > BRIDGE_FIELD_CAP ? `${t.slice(0, BRIDGE_FIELD_CAP)}…` : t
    }
    let data: unknown = null
    try {
      data = JSON.parse(
        JSON.stringify(r.data ?? null, (_k, v) => (typeof v === "string" && v.length > BRIDGE_FIELD_CAP ? `${v.slice(0, BRIDGE_FIELD_CAP)}…` : v)),
      )
    } catch {
      data = null // 结构化 data 不可序列化（BigInt/循环等）：丢弃，output 仍完整
    }
    return {
      name: rt.name,
      ok: true,
      result: {
        output: cap(r.output),
        data,
        blocks: r.blocks ?? [],
        truncated: !!r.truncated,
        filePath: r.filePath ?? null,
        subSessionArchive: r.subSessionArchive ?? null,
      },
    }
  } catch (err) {
    const e = (err as Error).message ?? String(err)
    return { name: rt.name, ok: false, error: `工具 ${rt.name} 执行失败: ${e}` }
  }
}
