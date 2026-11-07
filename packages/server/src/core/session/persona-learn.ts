/**
 * 人格的「自动学习」：每轮跑完后，把**进来的人话**里值得记的东西沉进册府。
 *
 * ## 为什么独立成文件（而不是塞进 runner 或 compose）
 *
 * 这段逻辑有三个互相牵制的约束，混在别处必然顾此失彼：
 *
 * 1. **只认人话**：模型自己的输出是推理，不是事实。它说“其实应该是……”是假设，
 *    人说的“不对，应该……”才是知识。
 * 2. **宁可少写**：写错一条知识，人格下次会**更自信地**再错一遍（因为“册府里有”）。
 *    所以门槛在 `memory-learn.ts`（纯函数、可穷举单测），这里只做「落库」。
 * 3. **scope 必须对**：把某个群的规矩写成 global，会在别的群也变成“理应如此”，
 *    而没人知道那是从别处传染来的。
 *
 * ## 学习是附加能力，不能拖垮干活
 *
 * 这一轮回复已经发出去了（`engine.run` 之后才调），所以这里任何失败都只记 warn。
 * 反过来说：如果哪天发现“回了消息但知识没进库”，先查这里的日志，而不是怀疑人格。
 */

import type { PersonaEvent } from "@gebai/persona"
import type { LearnedCandidate } from "@gebai/persona"

/** 册府写入面（注入，避免本模块直接依赖子Agent 的加载状态）。 */
export interface MemoryWriter {
  put(input: {
    title: string
    category: string
    source: string
    symptom?: string
    steps?: string[]
    facts?: Array<{ k: string; v: string }>
    scope: string
    confidence: string
  }): Promise<{ ok: boolean; id?: string; reason?: string }>
}

export interface LearnFromOptions {
  personaId: string
  chatId?: string | undefined
  sessionId?: string | undefined
  writer: MemoryWriter
  log?: { info(msg: string): void; warn(msg: string): void }
}

/**
 * 把一条候选落库。
 *
 * scope 的判定刻意**不用模型猜**：
 * - 纠正 → 该群（用户当场在那个群纠正的，换个群未必适用）
 * - 共识 → 该群（“找张三”是本群的分工）
 * - 全局知识得靠用户显式要求，或事后在册府里改 scope
 *
 * 为什么默认偏紧：scope 写宽了会把局部规矩扩散成全局规矩，
 * 而写窄了最多只是“这个群里有、别的群看不到”——后者的代价小得多。
 */
export async function learnCandidate(
  candidate: LearnedCandidate,
  opts: LearnFromOptions,
): Promise<{ learned: boolean; id?: string; why: string }> {
  const scope = opts.chatId
    ? JSON.stringify({ kind: "chat", id: opts.chatId })
    : JSON.stringify({ kind: "persona", id: opts.personaId })

  const res: { ok: boolean; id?: string; reason?: string } = await opts.writer
    .put({
      title: candidate.title,
      category: candidate.category,
      source: candidate.source,
      ...(candidate.symptom ? { symptom: candidate.symptom } : {}),
      ...(candidate.steps.length ? { steps: candidate.steps } : {}),
      ...(candidate.facts?.length ? { facts: candidate.facts } : {}),
      scope,
      // 置信度由识别层决定：只有「人当场纠正」能是 verified
      confidence: candidate.confidence,
    })
    .catch((err: unknown) => ({ ok: false, reason: String((err as Error)?.message || err) }))

  if (!res.ok) {
    opts.log?.warn(`[persona:learn] 写入册府失败（不影响回答）：${res.reason ?? "未知"}`)
    return { learned: false, why: res.reason ?? "写入失败" }
  }
  opts.log?.info(
    `[persona:learn] 已沉淀 [${res.id ?? "?"}] ${candidate.title}（${candidate.confidence}，scope=${scope}）`,
  )
  return { learned: true, ...(res.id ? { id: res.id } : {}), why: "" }
}

/**
 * 从本轮事件里挑出「人话」文本。
 *
 * 判据是事件源打的 `refs.extra.by === "human"`——能走到这里的确实是用户亲手发的
 *（人格自己的回灌在入站时已被发送记录拦掉）。取不到标记的事件不参与学习：
 * 定时任务/webhook 的“正文”是系统话术，不是人的纠正。
 */
export function humanTextsOf(events: PersonaEvent[]): string[] {
  const out: string[] = []
  for (const e of events) {
    if (e.context.refs?.extra?.by !== "human") continue
    const text = (e.body ?? "").trim()
    if (text) out.push(text)
  }
  return out
}
