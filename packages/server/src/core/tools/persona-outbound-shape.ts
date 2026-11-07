/**
 * 出站加工：人格发出去的消息，在**真正的渠道动作之前**过一道程序化整形 / 可选润色。
 *
 * ## 为什么这一层必须存在（而不是写在提示词里）
 *
 * 设计要求：输出信息可以由驱动进一步程序化处理，或者用大模型润色。纯靠提示词要求
 * 「不要 @所有人、不超过 800 字、结尾署名」会得到**概率性服从**——同一个人格在
 * 第八轮上下文里就开始忘。而这里的事情是确定性的、可审计的、可单测的。
 *
 * ## 三档（`GEBAI_PERSONA_<ID>_OUTBOUND`）
 *
 * | 值 | 行为 |
 * |---|---|
 * | `raw`（缺省） | 不加工，模型写什么就发什么（保持零意外） |
 * | `shape` | 只做程序化整形：去 `@all`、按渠道长度截断、附署名 |
 * | `polish` | `shape` + 主模型润色（只改措辞，不许增删事实；失败回落原文） |
 *
 * ## 为什么润色必须 fail-open（回落原文），而审批必须 fail-closed
 *
 * 两者失败的方向相反，因为代价不同：
 * - 审批失败放行 = 一条没人批准过的 `rm` 执行了（不可逆）
 * - 润色失败拦截 = 一句本来能发的话发不出去（可重试，且用户至少看到“它想说什么”）
 *
 * 所以润色只能失败回落，绝不能因为它挂了就把整条回复扣住。
 *
 * ## 为什么 @all 要无条件删掉
 *
 * 人格以**用户本人**名义发消息。一条 `@所有人` 是这个人对全群发的通知——
 * 模型把「@all」当成排版手段用一次，代价是几百人被打扰，而用户完全不知情。
 * 这不是“措辞偏好”，是权限问题：**模型不该拥有点名整个群的能力**。
 */

/** 渠道长度上限（超出会被飞书截断或报错，按渠道给保守值）。 */
export const CHANNEL_TEXT_LIMIT: Record<string, number> = {
  // 飞书 text 消息上限约 150KB，但 markdown 卡片实际可读长度远小于此；
  // 1500 是「一条消息能读完」的经验值（超出走截断 + 提示看产物）。
  feishu: 1500,
}

/** 加工档位。 */
export type OutboundMode = "raw" | "shape" | "polish"

/** 从环境变量读加工档位（`GEBAI_PERSONA_<ID>_OUTBOUND`）。 */
export function outboundModeOf(personaId: string, env: Record<string, string | undefined> = process.env as Record<string, string | undefined>): OutboundMode {
  const raw = (env[`GEBAI_PERSONA_${personaId.toUpperCase()}_OUTBOUND`] ?? "").trim().toLowerCase()
  if (raw === "shape" || raw === "polish") return raw
  return "raw"
}

/** 从环境变量读署名（`GEBAI_PERSONA_<ID>_SIGNATURE`；空 = 不署名）。 */
export function signatureOf(personaId: string, env: Record<string, string | undefined> = process.env as Record<string, string | undefined>): string {
  return (env[`GEBAI_PERSONA_${personaId.toUpperCase()}_SIGNATURE`] ?? "").trim()
}

/**
 * 消息正文里对「全群广播」的表达（一律剥掉）。
 *
 * 为何不用 `\b` 收尾：JS 正则的 `\b` 只在「单词字符 ↔ 非单词字符」之间成立，
 * 而汉字属于**非单词字符**——`@全体成员 今晚停机` 里 `员` 与空格之间没有词边界，
 * 加了 `\b` 就永远匹配不到中文写法（实测踩过）。所以 ASCII 写法用「后面不是
 * 单词字符」的前瞻，中文写法不加边界（中文后面接什么都该判为 @all）。
 */
const AT_ALL = /@(?:all|everyone)(?![A-Za-z0-9_])|@(?:所有人|全体成员|全体)/gi

/**
 * 程序化整形（确定性、可单测）。
 *
 * @returns 整形后的文本与动过手的说明（说明进日志，让人知道系统改了什么）
 */
export function shapeOutbound(input: {
  text: string
  /** 渠道名（决定长度上限）。 */
  channel: string
  mode: OutboundMode
  /** 署名（空 = 不署）。 */
  signature?: string
}): { text: string; changes: string[]; truncated: boolean } {
  if (input.mode === "raw") return { text: input.text, changes: [], truncated: false }
  const changes: string[] = []
  let text = input.text

  // ① 去 @all：模型不该拥有点名整个群的能力（见文件头）
  if (AT_ALL.test(text)) {
    const before = text
    text = text.replace(AT_ALL, "")
    // 清理被 @all 留下的多余空白（避免发出「大家好，  今天…」/
    // 以及行尾挂着两个空格这种一眼看出是机器拼过的痕迹）
    text = text
      .split("\n")
      .map((line) => line.replace(/[ \t]{2,}/g, " ").trimEnd())
      .join("\n")
      .replace(/\n{3,}/g, "\n\n")
      .trimEnd()
    if (text !== before) changes.push("已移除 @全体")
  }
  // 复位 lastIndex（带 g 的正则有状态；复用同一个常量时不清零会让下一条消息漏判）
  AT_ALL.lastIndex = 0

  // ② 署名（只在不以同样文本结尾时加——否则重试会叠出两行署名）
  const sig = input.signature?.trim()
  if (sig && !text.trimEnd().endsWith(sig)) {
    text = `${text.trimEnd()}\n\n${sig}`
    changes.push("已附署名")
  }

  // ③ 渠道长度截断（保留首部 + 明确提示，模型与人都知道有内容被截）
  const limit = CHANNEL_TEXT_LIMIT[input.channel]
  let truncated = false
  if (limit && text.length > limit) {
    const note = `\n\n…（超出 ${limit} 字已截断，共 ${text.length} 字；完整内容请落产物或分条发送）`
    text = text.slice(0, Math.max(0, limit - note.length)) + note
    truncated = true
    changes.push(`按 ${input.channel} 长度上限截断`)
  }
  return { text, changes, truncated }
}

/** 润色所需的模型面（与引擎 provider 同形的最小接口，便于单测与解耦）。 */
export interface PolishModel {
  chat(
    messages: Array<{ role: string; content: string }>,
    opts?: { maxTokens?: number; temperature?: number; signal?: AbortSignal },
  ): AsyncIterable<{ text?: string } | { delta?: string }>
}

/** 把引擎 provider 适配成润色模型（只用到 `chat`，窄接口避免把 llm 依赖带进工具层单测）。 */
export function polishModelOf(provider: {
  chat(
    messages: never,
    opts?: { maxTokens?: number; temperature?: number; signal?: AbortSignal },
  ): AsyncIterable<unknown>
}): PolishModel {
  return {
    chat: (messages, opts) =>
      provider.chat(messages as never, opts as never) as AsyncIterable<{ text?: string } | { delta?: string }>,
  }
}

/** 润色提示词：只改措辞，不许增删事实。 */
export function polishPrompt(text: string, ctx: { personaName?: string; chatLabel?: string }): Array<{ role: string; content: string }> {
  const where = ctx.chatLabel ? `在「${ctx.chatLabel}」` : "在群里"
  return [
    {
      role: "system",
      content:
        `你是一个发言润色器，替「${ctx.personaName ?? "这个人"}」把要发${where}的话改得更通顺、更得体。\n\n` +
        "硬约束（违反任何一条就是失败）：\n" +
        "1. **只改措辞**，不许增加原文没有的事实、数字、结论、承诺。\n" +
        "2. **不许删除**关键信息（人名、时间、结论、待办）。\n" +
        "3. 保持原文的语言与语气（原来是中文就中文，原来是生硬的技术陈述就别改成客套话）。\n" +
        "4. 保持长度量级（可小幅精简，不许扩写成三倍长）。\n" +
        "5. 不要加开头问候语或结尾套话（如「希望以上对您有帮助」）。\n" +
        "6. 直接输出润色后的正文，不要任何解释、前后缀、代码块标记。\n\n" +
        "如果原文已经通顺得体，就原样返回。",
    },
    { role: "user", content: text },
  ]
}

/**
 * 用模型润色（失败**回落原文**——见文件头「为什么润色必须 fail-open」）。
 *
 * @returns 润色结果；任何失败（抛错、空输出、超长）都返回原文并记 reason
 */
export async function polishOutbound(input: {
  text: string
  model: PolishModel
  personaName?: string
  chatLabel?: string
  /** 润色超时（毫秒，缺省 20 秒）：润色是锦上添花，不能把一条消息拖成 2 分钟。 */
  timeoutMs?: number
}): Promise<{ text: string; ok: boolean; reason?: string }> {
  const timeoutMs = input.timeoutMs ?? 20_000
  const ctl = new AbortController()
  const timer = setTimeout(() => ctl.abort(), timeoutMs)
  try {
    let out = ""
    for await (const chunk of input.model.chat(polishPrompt(input.text, { ...(input.personaName ? { personaName: input.personaName } : {}), ...(input.chatLabel ? { chatLabel: input.chatLabel } : {}) }), {
      maxTokens: Math.max(256, Math.ceil(input.text.length * 1.5)),
      temperature: 0.3,
      signal: ctl.signal,
    })) {
      const piece = (chunk as { text?: string; delta?: string }).text ?? (chunk as { delta?: string }).delta ?? ""
      if (piece) out += piece
    }
    out = out.trim()
    // 空输出/明显跑偏（模型只回了一句解释）都回落原文：宁可发原话，也不发一句莫名其妙的话
    if (!out) return { text: input.text, ok: false, reason: "模型返回空内容" }
    if (out.length > input.text.length * 3) return { text: input.text, ok: false, reason: "模型输出长度异常（疑似扩写）" }
    return { text: out, ok: true }
  } catch (err) {
    return { text: input.text, ok: false, reason: String((err as Error)?.message || err) }
  } finally {
    clearTimeout(timer)
  }
}
