/**
 * 人格「自组织」工具的服务端适配：把 `@gebai/persona` 的 `PersonaTool`（结构兼容 SDK Tool）
 * 接到引擎的 `Tool`（增强 ToolContext）上，并解决**身份从哪来**。
 *
 * ## 三个必须由宿主回答的问题
 *
 * | 问题 | persona 内核的答案 | 宿主怎么给 |
 * |---|---|---|
 * | 我是哪个人格？ | 不给——内核说「身份必须由宿主给定」 | 按 `ctx.sessionId` 反查 `SessionData.persona` |
 * | 只让我在自己的会话里用 | 不给——内核只校验 `personaId` | 非人格会话（普通对话）→ 工具报「这里不是人格会话」 |
 * | 注册表变了运行时怎么知道 | `onChange` 回调 | 回调里 `runtime.syncDriver`，否则新建的分身收不到事件 |
 *
 * **为什么不给模型传 `personaId`**：那等于让模型自己声明「我代表谁」，一次错误就能让 A 角色
 * 创建/删除 B 角色的分工。身份只能从**会话归属**读出来——那是部署方在创建会话时确定的。
 *
 * ## 为什么工具是「全局注册 + 运行时自检」而不是「会话级注册」
 *
 * 工具的可见面（`Tool.requiresApproval` / `execute`）由引擎在**调用时**解析，而注册是进程级的。
 * 若按会话注册/注销，会话删除、重建、人格热加载三件事都要同步维护注册表——多一处状态就多
 * 一处不一致。这里改成：**总是注册，执行时用会话归属判定**。普通会话里调用会得到一句
 * 「这里不是人格会话」而不是一个看不懂的失败，而注册表始终只有一份。
 */

import type { Tool, ToolContext } from "../base/types"
import type { SessionStore, SessionData } from "./store"
import type { DriverRegistry, PersonaTool } from "@gebai/persona"
import type { PersonaRuntime } from "@gebai/persona"

/** 会话归属（从 `SessionData.persona` 读出；普通会话为 null）。 */
export interface PersonaOwner {
  personaId: string
  driverId: string
}

export interface PersonaDriverToolDeps {
  registry: DriverRegistry
  /** 会话存储：`sessionId → persona 归属` 的唯一真相源（不额外维护映射表）。 */
  store: Pick<SessionStore, "load">
  /** 运行时（`onChange` 时同步调度器：新建的分身必须立刻能被事件投到）。 */
  runtime: Pick<PersonaRuntime, "syncDriver">
  log?: { info(msg: string): void; warn(msg: string): void }
}

/** 从会话归属解析出人格身份（普通会话/会话不存在 → null）。 */
export async function personaOwnerOf(
  store: Pick<SessionStore, "load">,
  sessionId: string,
  user: string,
): Promise<PersonaOwner | null> {
  if (!sessionId) return null
  let session: SessionData | null = null
  try {
    session = await store.load(sessionId, user)
  } catch {
    return null // 读不到就当没有：宁可少给能力，也不给错身份
  }
  const p = session?.persona
  if (!p?.personaId || !p?.driverId) return null
  return { personaId: p.personaId, driverId: p.driverId }
}

/**
 * 给内核工具的 `resolveContext`：从**适配层注入的 ctx** 读身份。
 *
 * 内核约定「身份必须由宿主给定，不能让模型传」——宿主给的方式就是这个：适配层在调用前
 * 从会话归属算出 `{personaId, sessionId}`，内核工具通过本函数取它（取不到 = 普通会话）。
 *
 * 为何不直接写 `() => null`：那样内核工具永远认为「当前通道不支持」，
 * 表现为**所有人格会话里 driver_create 都不可用**（工具存在但永远拒绝）。
 */
export function personaContextResolver(raw: unknown): { personaId: string; sessionId?: string } | null {
  const c = raw as { personaId?: unknown; sessionId?: unknown } | null | undefined
  const personaId = typeof c?.personaId === "string" ? c.personaId.trim() : ""
  if (!personaId) return null
  return { personaId, ...(typeof c?.sessionId === "string" ? { sessionId: c.sessionId } : {}) }
}

/**
 * 把内核给的 `PersonaTool[]` 转成引擎可注册的 `Tool[]`。
 *
 * `requiresApproval` 的默认值：**不弹审批**（与内核取向一致——创建/下线 driver 都写盘留痕且可回滚，
 * 靠「可见 + 可回滚」承担风险；逐次审批会让「自我组织」退化成「每步问人」）。
 * 部署方若要收紧，可用 `GEBAI_PERSONA_DRIVER_TOOLS_APPROVAL=true`（见 compose）。
 */
export function toEngineTools(
  personaTools: PersonaTool[],
  deps: PersonaDriverToolDeps,
  opts: { requireApproval?: boolean } = {},
): Tool[] {
  return personaTools.map((pt): Tool => {
    const tool: Tool = {
      name: pt.name,
      // 前缀把「什么时候能用」放在最前面：全局工具对普通会话也可见，
      // 不写清楚的话模型会在普通对话里试探性调用（白花一次往返）。
      description: `【仅人格会话可用】${pt.description}`,
      parameters: pt.parameters as Tool["parameters"],
      ...(opts.requireApproval ? { requiresApproval: true } : {}),
      async execute(args: Record<string, unknown>, ctx: ToolContext) {
        const owner = await personaOwnerOf(deps.store, ctx.sessionId, ctx.user)
        if (!owner) {
          // 普通会话：明确说明「为什么这里不能用」而不是抛错或被静默忽略。
          // 这条文本会回到模型，它据此知道该去 persona 会话里做这件事（或让用户去配置）。
          return {
            output:
              "当前会话不属于任何人格，无法管理人格分工（driver）。\n" +
              "driver 是**人格**的工作结构：只有人格的常驻会话（会话名形如「小霜·support」）里才能增删。\n" +
              "如果你要为人格调整分工，请在那个会话里操作；若是部署方的静态配置，请改人格定义文件。",
          }
        }
        const r = await pt.execute(args, { personaId: owner.personaId, sessionId: ctx.sessionId })
        return { output: r.output, ...(r.data !== undefined ? { data: r.data } : {}) }
      },
    }
    return tool
  })
}

/**
 * `onChange` 的处理器：注册表变了 → 运行时要同步，否则新 driver 永远收不到事件。
 *
 * 这一步漏掉时的现象很骗人：工具返回「已创建分身」，日志也有，但那个分身**从不响应**——
 * 因为调度器里没有它的条目（`schedulerFor` 只在事件投递时才惰性创建，而事件永远投不进来
 * 之后就没有之后了）。
 */
export function onDriverChange(deps: Pick<PersonaDriverToolDeps, "runtime" | "log">) {
  return (change: { action: "created" | "removed"; personaId: string; driverId: string }): void => {
    try {
      deps.runtime.syncDriver(change.personaId, change.driverId, change.action === "created" ? "online" : "offline")
      deps.log?.info(`[persona:drivers] 运行时已同步 ${change.personaId}/${change.driverId}（${change.action}）`)
    } catch (err) {
      deps.log?.warn(
        `[persona:drivers] 同步运行时失败 ${change.personaId}/${change.driverId}: ${String((err as Error)?.message || err)}` +
          "——磁盘已写入，重启后仍会生效",
      )
    }
  }
}
