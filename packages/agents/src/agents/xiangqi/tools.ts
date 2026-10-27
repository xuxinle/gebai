/**
 * 相期工具面：把「打断用户」这件事做成几个语义明确的工具。
 *
 * ## 为什么不是 notice / choice / todo 各一个工具
 *
 * 三个 `kind` × (投递 / 等待 / 查询 / 撤回) 会膨胀成十几个工具，而它们的
 * 参数形状几乎一样。工具数量本身就是上下文税，所以按**阻塞语义**分，不按 kind 分：
 *
 * | 工具 | 阻塞 | 用途 |
 * |---|---|---|
 * | `card_notice` | 否 | 纯告知，推完就走 |
 * | `card_ask` | 是（可超时） | 需要用户定夺，模型也无事可做，就该等 |
 * | `card_list` | 否 | 看谁还在等 |
 * | `card_dismiss` | 否 | 撤回全部 |
 *
 * `card_ask` 的阻塞语义与引擎既有的 `ask` 工具**一致**——两者都是“在等用户输入”，
 * 不该有两套等待行为。
 */

import type { Tool, ToolResult, ToolSet } from "@gebai/sdk"

import type { CardAnswer, CardKind, CardSpec } from "./controller"
import { controller } from "./controller"

/** 缺省等待时限：5 分钟。可用 `XIANGQI_WAIT_MS` 调（非法值回落此值）。 */
export const DEFAULT_WAIT_MS = 5 * 60 * 1000

/** `card_ask` 的支持超时上限：用户去开会了，模型不该永远挂着。 */
export const MAX_WAIT_MS = 30 * 60 * 1000

/** 从环境读缺省等待时限（`XIANGQI_WAIT_MS`），夹到 `[1000, MAX_WAIT_MS]`。 */
function defaultWaitMs(): number {
  const raw = process.env.XIANGQI_WAIT_MS
  if (raw === undefined || raw.trim() === "") return DEFAULT_WAIT_MS
  const parsed = Number.parseInt(raw, 10)
  if (!Number.isFinite(parsed) || parsed <= 0) return DEFAULT_WAIT_MS
  return Math.max(1000, Math.min(parsed, MAX_WAIT_MS))
}

function textResult(output: string, data?: unknown): ToolResult {
  return data === undefined ? { output } : { output, data }
}

/** 把用户答复渲染成给模型看的自然语言（按 kind 分支，不糊成一坨 JSON）。 */
export function describeAnswer(card: CardSpec, answer: CardAnswer): string {
  const body = answer.answer
  if (card.kind === "choice") {
    const picked = typeof body.choice === "string" ? body.choice : JSON.stringify(body.choice)
    // 把 value 反查成 label：模型给用户看的是 label，回来的是 value，
    // 不翻译的话模型会对着 `oc_support` 这类内部值说话。
    const label = card.options?.find((option) => option.value === picked)?.label ?? picked
    return `用户选了「${label}」（value=${picked}）`
  }
  if (card.kind === "todo") {
    if (body.done === true) return "用户标记「完成了」"
    if (body.snooze === true) return "用户标记「先放着」（还记着，晚点做）"
    return `用户给了答复：${JSON.stringify(body)}`
  }
  if (body.acknowledged === true) return "用户点了「知道了」"
  return `用户给了答复：${JSON.stringify(body)}`
}

const cardKindSchema = {
  type: "string",
  enum: ["notice", "choice", "todo"],
  description: "notice=纯告知；choice=必须选一个；todo=必须给结果（完成/先放着）",
} as const

/**
 * 造工具集。
 *
 * ## 为何 `ctl` 是“每次取”而不是构造时固定
 *
 * 早期写的是 `const ctl = deps.controller ?? controller()`——那会在**模块加载**
 * 时就抓好单例（`xiangqi.ts` 顶层就调 `createTools()`），后果：
 *
 * 1. 单例一出生就绑定了当时的 cwd（缺省数据根 `<cwd>/xiangqi`）——
 *    测试里就算后来 `resetControllerForTest()` 也救不回来，还会把卡片文件
 *    写进仓库目录（实测踩到）。
 * 2. 子Agent 的“进程级单例”变成“模块级单例”，多实例/重建场景全错。
 *
 * 改成函数后，每次工具调用才解析控制器：要么用注入的，要么取当前单例。
 */
export function createTools(deps: { controller?: ReturnType<typeof controller> } = {}): ToolSet {
  /** 解析本次调用要用的控制器。 */
  const resolve = (): ReturnType<typeof controller> => deps.controller ?? controller()

  const notice: Tool = {
    name: "card_notice",
    description:
      "在用户桌面上的相期窗口推一条**纯告知**卡片，推完立即返回（不等用户看）。\n" +
      "适用于「事情办完了说一声」「有个变化值得知道但不需要他动手」。\n" +
      "正文支持 markdown（列表/代码块/强调），大模型产出的内容直接贴。\n" +
      "⚠ 不要用它代替该问的事：需要用户定夺用 card_ask——notice 可以被批量忽略。",
    parameters: {
      type: "object",
      properties: {
        title: { type: "string", description: "卡片标题（一句话说清是什么事）" },
        body: { type: "string", description: "正文，markdown" },
        from: { type: "string", description: "来源标签（谁发的，如「小霜」）" },
      },
      required: ["title"],
    },
    async execute(args) {
      const spec: CardSpec = {
        kind: "notice",
        title: String(args.title ?? ""),
        body: args.body === undefined ? undefined : String(args.body),
        from: args.from === undefined ? undefined : String(args.from),
      }
      const { id, delivered } = resolve().push(spec)
      if (!delivered) {
        // **如实告知送不到**：静默成功会让模型以为用户看到了，
        // 于是它继续跑，而那个提醒根本没人收到。
        return textResult(
          `⚠ 相期未连接——这条推送**用户看不到**（id=${id}，已缓存，相期启动后会重推）。\n` +
            `如果这条很重要，请用其他方式告知用户（或提示他启动相期）。`,
          { id, kind: "notice", delivered: false },
        )
      }
      return textResult(`已推送到相期（id=${id}）。用户会在桌面窗口里看到，可以稍后处理。`, {
        id,
        kind: "notice",
        delivered: true,
      })
    },
  }

  const ask: Tool = {
    name: "card_ask",
    description:
      "在相期窗口推一张**需要用户定夺**的卡，并等待他的答复。\n" +
      "- `kind=choice`：必须选一个（options 必填）——适用于「发到哪个群」「用哪个方案」\n" +
      "- `kind=todo`：必须给结果（完成了 / 先放着）——适用于「请你去做这件事」\n" +
      "返回值要么是用户的选择，要么是超时。\n" +
      "⚠ 超时**不代表失败**：卡片仍留在用户窗口上，他随时可能处理。\n" +
      "超时后你应当：先做不依赖这个决定的部分，或明确告知用户你在等什么。",
    parameters: {
      type: "object",
      properties: {
        kind: { ...cardKindSchema, enum: ["choice", "todo"] },
        title: { type: "string", description: "要用户定夺的是什么（一句话）" },
        body: { type: "string", description: "补充说明，markdown" },
        from: { type: "string", description: "来源标签" },
        options: {
          type: "array",
          description: "kind=choice 必填：选项（label 给人看，value 回来时给你）",
          items: {
            type: "object",
            properties: {
              label: { type: "string" },
              value: { type: "string" },
              hint: { type: "string" },
            },
            required: ["label", "value"],
          },
        },
        due: { type: "string", description: "kind=todo 可选：截止说明（如「今天」）" },
        wait_ms: {
          type: "number",
          description: `最多等多久（毫秒），缺省 ${DEFAULT_WAIT_MS}，上限 ${MAX_WAIT_MS}`,
        },
      },
      required: ["kind", "title"],
    },
    async execute(args) {
      const kind = (String(args.kind ?? "choice") as CardKind) ?? "choice"
      const rawOptions = Array.isArray(args.options) ? args.options : undefined
      const options = rawOptions?.map((option) => {
        const item = option as Record<string, unknown>
        return {
          label: String(item.label ?? ""),
          value: String(item.value ?? ""),
          hint: item.hint === undefined ? undefined : String(item.hint),
        }
      })
      const spec: CardSpec = {
        kind,
        title: String(args.title ?? ""),
        body: args.body === undefined ? undefined : String(args.body),
        from: args.from === undefined ? undefined : String(args.from),
        options,
        due: args.due === undefined ? undefined : String(args.due),
      }
      const requested = typeof args.wait_ms === "number" ? args.wait_ms : defaultWaitMs()
      const waitMs = Math.max(1000, Math.min(requested, MAX_WAIT_MS))

      const { id, delivered } = resolve().push(spec)
      if (!delivered) {
        // 送不到就别等了：白等 5 分钟对模型和用户都是浪费
        return textResult(
          `⚠ 相期未连接——**用户看不到这张卡**，等下去没意义。\n` +
            `请先把其他能做的做完，并告知用户「有个决定需要他做，但相期没在运行」。`,
          { id, kind, delivered: false, answered: false },
        )
      }
      const answer = await resolve().waitForAnswer(id, waitMs)

      if (answer === null) {
        // 超时：**必须说清卡片还在**，否则模型会以为用户没看到
        return textResult(
          `等 ${Math.round(waitMs / 1000)} 秒没有收到答复。\n` +
            `⚠ 卡片**仍留在用户窗口上**（id=${id}），他随时可能处理；答复会作为事件回来。\n` +
            `你可以先做不依赖这个决定的部分，或告诉用户你在等什么。`,
          { id, kind, answered: false, timedOut: true },
        )
      }
      return textResult(`用户已答复：${describeAnswer(spec, answer)}`, {
        id,
        kind,
        answered: true,
        answer: answer.answer,
      })
    },
  }

  const list: Tool = {
    name: "card_list",
    description: "查看相期窗口上当前有哪些卡还在等人处理（含你自己正在等的）。连不上时如实说明。",
    parameters: { type: "object", properties: {} },
    async execute() {
      const connected = resolve().isConnected()
      const pendingCards = resolve().pending()
      if (pendingCards.length === 0) {
        return textResult(
          connected ? "相期上当前没有正在等待答复的卡。" : "相期未连接——无法确认窗口状态。",
          { connected, pending: [] },
        )
      }
      const lines = pendingCards.map(
        (card) => `- [${card.kind}] ${card.title}（id=${card.id}）`,
      )
      return textResult(
        `${connected ? "已连接" : "⚠ 未连接"}。正在等答复的卡：\n${lines.join("\n")}`,
        { connected, pending: pendingCards },
      )
    },
  }

  const dismiss: Tool = {
    name: "card_dismiss",
    description:
      "把相期窗口上的卡片清掉。\n" +
      "传 `id` = 只撤那一张（已处理完的事）；不传 = 清全部。\n" +
      "⚠ 不要用清全部去抹掉还在等用户定夺的 choice/todo（那等于替用户做了「不处理」的决定）。",
    parameters: {
      type: "object",
      properties: {
        id: { type: "string", description: "要撤的卡片 id（省略 = 清全部）" },
      },
    },
    async execute(args) {
      const id = args.id === undefined ? "" : String(args.id).trim()
      if (!id) {
        resolve().dismissAll()
        return textResult("已清空相期上的全部卡片。")
      }
      const ok = resolve().dismiss(id)
      return textResult(ok ? `已撤回卡片 ${id}。` : `相期上没有 id=${id} 的卡片（可能已处理或已过期）。`, {
        id,
        dismissed: ok,
      })
    },
  }

  /**
   * 授权卡：把一个**外部链接**送到用户桌面上，并跟踪他什么时候完成。
   *
   * 为何不复用 `card_ask`：
   *
   * | | `card_ask` | `card_authorize` |
   * |---|---|---|
   * | 用户要做的 | 在卡片上选一个 | **离开去浏览器完成** |
   * | 答案从哪来 | 卡片上的点击 | 外部系统回调（不是卡片） |
   * | 阻塞姿态 | 等答复（可超时） | **不阻塞**（授权可能很久，且可能是别的设备上完成） |
   * | 完成后 | 用户点选 → 返回 | 外部状态变了 → **主动撤卡** |
   *
   * 硬塞进 `card_ask` 会让「阻塞等待」这个语义失真——授权完成不会有 `answer` 回来。
   */
  const authorize: Tool = {
    name: "card_authorize",
    description:
      "在相期窗口推一张「去授权」卡：带一个可点击的外部链接，并告知授权完成前什么功能不可用。\n" +
      "**不阻塞等待**（授权可能很久，也可能在手机等其他设备完成）。\n" +
      "授权完成后用 `card_dismiss(id=…)` 把卡撤掉——不要留着让用户以为还没好。",
    parameters: {
      type: "object",
      properties: {
        title: { type: "string", description: "一句话说清要授权什么" },
        url: { type: "string", description: "授权链接（点击打开浏览器）" },
        body: { type: "string", description: "补充说明，markdown（如：为什么需要、完成后会怎样）" },
        from: { type: "string", description: "来源标签" },
        ttl_ms: { type: "number", description: "卡片有效期（毫秒）；链接过期后卡片自动消失。缺省 10 分钟" },
      },
      required: ["title", "url"],
    },
    async execute(args) {
      const url = String(args.url ?? "").trim()
      if (!/^https?:\/\//i.test(url)) throw new Error("url 必须是 http(s) 链接")
      const ttl = typeof args.ttl_ms === "number" && args.ttl_ms > 0 ? args.ttl_ms : 10 * 60 * 1000
      const spec: CardSpec = {
        // `choice` + 带 url 的选项 = 链接按钮（相期渲染为可点击）
        kind: "choice",
        title: String(args.title ?? ""),
        body: args.body === undefined ? undefined : String(args.body),
        from: args.from === undefined ? undefined : String(args.from),
        options: [{ label: "去授权", value: "open", url }],
        expiresAtMs: Date.now() + ttl,
      }
      const { id, delivered } = resolve().push(spec)
      if (!delivered) {
        return textResult(
          `⚠ 相期未连接——**用户看不到这张授权卡**（id=${id}，已缓存，相期启动后会重推）。\n` +
            `授权链接：${url}\n` +
            `如果相期没在运行，请通过其他方式把链接发给用户。`,
          { id, delivered: false, url },
        )
      }
      return textResult(`已在相期推送授权卡（id=${id}），用户点击链接即可完成授权。授权完成后请用 card_dismiss 撤卡。`, {
        id,
        delivered: true,
        url,
        expiresAtMs: spec.expiresAtMs,
      })
    },
  }

  return {
    card_notice: notice,
    card_ask: ask,
    card_authorize: authorize,
    card_list: list,
    card_dismiss: dismiss,
  }
}
