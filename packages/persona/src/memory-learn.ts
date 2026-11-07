/**
 * 从一轮对话里识别「值得进知识库的东西」——**纯函数，零依赖，可穷举单测**。
 *
 * ## 为什么这是整个册府接入里最危险的一步
 *
 * 自动写入做错，代价不是“多几条垃圾”而是**知识库变成不可信**：
 * 一旦人格答错过一次并且把错误写进去，下次它会**更自信地**再答错一遍
 * （因为「知识库里有」）。所以这里的取向是**宁可少写，也不写可疑的**。
 *
 * ## 三种信号，置信度天差地别
 *
 * | 信号 | 依据 | confidence | 为什么 |
 * |---|---|---|---|
 * | **用户纠正** | 「不对 / 应该是 / 不是这样」 | `verified` | 人当场否定了它，并给了正确答案——最贵最准 |
 * | **群里共识** | 「找张三 / 他负责这个」 | `inferred` | 可能是玩笑/过时信息，要人确认 |
 * | **解决过程** | 查了资料/试错后答成 | `inferred` | 结论可能只对这个场景成立 |
 *
 * **只有第一种**能进 `verified`。把人随口一说标成“已验证”，等于给未来的错误盖章。
 *
 * ## 一个刻意的设计：不自动写「事实性答案」
 *
 * 比如用户问「今天几号」「多少天」，人格答对了——这类**不该进库**：
 * 它是可计算的、会过期的，写进去只会让下次答一个旧值。
 * 所以这里只认「怎么做事」（步骤/命令/找谁），不认「是什么」（具体数值/日期）。
 */

/** 识别出的候选知识（还没落库）。 */
export interface LearnedCandidate {
  /** 来源类型（决定 confidence 与入库与否的优先级）。 */
  kind: "correction" | "consensus" | "procedure"
  /** 一句话标题（直接可作为 title）。 */
  title: string
  /** 分类建议。 */
  category: string
  /** 现象/触发条件。 */
  symptom: string
  /** 有序步骤。 */
  steps: string[]
  /** 硬事实（k-v）：IP/端口/阈值/责任人。可空。 */
  facts?: Array<{ k: string; v: string }>
  /** 出处（务必带：没有出处的知识事后无法判断要不要信）。 */
  source: string
  /** 置信度。 */
  confidence: "verified" | "inferred"
  /** 命中的原始片段（诊断与人工复核用）。 */
  evidence: string
}

/** 纠正类信号：人明确否定 + 给出替代。 */
const CORRECTION_PATTERNS = [
  /不对[，,。；;]?\s*(?:应该|是|要|得)/,
  /不是这样/,
  /不是这个意思/,
  /错了[，,。；;]?\s*(?:应该|是|要|得)/,
  /其实(?:应该|是|要|得)/,
  /(?:应该|要)用\s*\S+/,
  /更正一下/,
  /纠正一下/,
]

/** 共识类信号：谁负责什么。
 *
 * ## 为何这些正则写得这么“紧”（实测踩到的坑）
 *
 * 第一版用贪婪量词，结果：
 * - `找\s*([一-龥]{2,4})\s*(?:处理)` 对「找张三处理」捕获到 **「张三处理」**（把动词也吞了）
 * - `([一-龥]{2,4})\s*负责` 对「配额这块李四负责」捕获到 **「这块李四」**（把定语也吞了）
 *
 * 而「找我看一眼」会被当成“责任人是我”——人称代词必须先排除。
 * 现在的做法：不依赖正则一次到位，先粗抽再在代码里剥后缀（见 `normalizeName`）。
 */
const CONSENSUS_PATTERNS = [
  /找\s*([\u4e00-\u9fa5A-Za-z]{2,6})/,
  /([\u4e00-\u9fa5]{2,4})\s*负责/,
  /这个(?:问|找)\s*([\u4e00-\u9fa5A-Za-z]{2,6})/,
  /归\s*([\u4e00-\u9fa5A-Za-z]{2,6})\s*管/,
]

/** 不该进库的问句（可计算的、时效性的、闲聊的）。 */
const NOISE_PATTERNS = [
  /今天(?:几号|星期几|天气)/,
  /(?:多少|几)天/,
  /现在几点/,
  /你是谁/,
  /^你好/,
  /谢谢/,
]

/**
 * 识别纠正类候选。
 *
 * 只认**人说的**纠正（`by: "human"`），不认模型自己的话——
 * 模型说“其实应该是……”是它在推理，不是事实。
 */
export function detectCorrection(input: {
  humanText: string
  /** 该群/该人格（用于 scope 与 source）。 */
  chatId?: string | undefined
  personaId: string
  sessionId?: string | undefined
}): LearnedCandidate | null {
  const text = input.humanText.trim()
  if (!text || text.length < 6) return null // 太短的多半是语气词
  if (NOISE_PATTERNS.some((p) => p.test(text))) return null
  if (!CORRECTION_PATTERNS.some((p) => p.test(text))) return null

  // 标题取纠正句的前 40 字（去掉换行）：它本身就是“原来如此”的一句话
  const title = text.replace(/\s+/g, " ").slice(0, 40)
  return {
    kind: "correction",
    title,
    category: "纠正",
    symptom: "用户当场纠正过的做法（照此执行，不要再按旧方式）",
    steps: [text],
    source: `用户纠正@${input.sessionId ?? "会话"}${input.chatId ? `#${input.chatId}` : ""}`,
    // 人当场说的 = 已验证。这是唯一能拿 verified 的自动来源
    confidence: "verified",
    evidence: text,
  }
}

/**
 * 识别「解决过程」——人格这一轮**实际做了什么才把问题解决**。
 *
 * ## 与另两类的根本区别
 *
 * 纠正与共识的依据是**人说了什么**；这一类的依据是**人格实际做了什么**：
 * 它调了哪些工具、看了哪些地方、最终给了什么结论。
 *
 * ## 为何门槛最严（宁可漏）
 *
 * “怎么解决”是个**因果断言**，而对话里只有“顺序”没有“因果”：
 * 它先查了 A 又查了 B，不代表 A 是原因。把顺序当成步骤写进库，
 * 下次别人照着做会走弯路——而知识库里的话天然带权威感。
 *
 * 所以只收**有硬证迹**的那种：
 * - 实际执行过命令（不是“建议执行”），且
 * - 结论里带具体值（阈值/IP/版本/错误码）——这类是可验证的，不是形容
 *
 * 而且一律 `inferred`：等技术上有人照做成功、或用户确认后才算数。
 */
export function detectProcedure(input: {
  /** 这一轮人格实际执行过的命令（由调用方从工具调用记录里取）。 */
  executedCommands: string[]
  /** 人格给出的回答正文（取具体值用）。 */
  answerText: string
  /** 用户最初问的那句（当标题素材）。 */
  question: string
  chatId?: string | undefined
  sessionId?: string | undefined
}): LearnedCandidate | null {
  const answer = input.answerText.trim()
  const question = input.question.trim()
  if (!answer || !question) return null
  if (NOISE_PATTERNS.some((p) => p.test(question))) return null // 时效性问题不学

  // 硬证迹一：真的执行过命令（不是“你可以试试”）
  const cmds = input.executedCommands.map((c) => c.trim()).filter(Boolean)
  if (!cmds.length) return null

  // 硬证迹二：答案里带可验证的具体值。
  //
  // 为何要这一条：只凭“执行过命令 + 给了答案”就写库，会把
  // “我查了一下，应该是网络问题”这种猜测也当成知识。
  // 具体值（数字/版本/错误码/IP/路径）至少是可被下一个人验证的。
  const concrete = /\d+(\.\d+)*\s*(?:x|%|ms|s|MB|GB|次|条|个)?|\bv?\d+\.\d+|\b\d{2,5}\b|[a-z]:\\\\[^\s，。]+|\/api\/[\w\/.-]+/.test(answer)
  if (!concrete) return null

  // 标题取问题本身（“XX 怎么办”），它比一个抽象概括更好检索
  const title = question.replace(/\s+/g, " ").slice(0, 40)
  return {
    kind: "procedure",
    title,
    category: "处置过程",
    symptom: question.slice(0, 80),
    steps: [...cmds.map((c) => `执行：${c}`), `结论：${answer.slice(0, 200)}`],
    source: `实测@${input.sessionId ?? "会话"}${input.chatId ? `#${input.chatId}` : ""}`,
    // 一律 inferred：顺序≠因果，得等人验证
    confidence: "inferred",
    evidence: `${question} → ${cmds.join(" | ")}`,
  }
}

/**
 * 识别共识类候选（谁负责什么）。
 *
 * 标 `inferred` 而不是 verified：群里说“找张三”可能是玩笑、可能是旧信息。
 * 让人在册府里确认一次才算数——这正是 `cefu_confirm` 的用途。
 */
export function detectConsensus(input: { humanText: string; chatId?: string | undefined; sessionId?: string | undefined }): LearnedCandidate | null {
  const text = input.humanText.trim()
  if (!text || text.length < 5) return null
  if (NOISE_PATTERNS.some((p) => p.test(text))) return null

  for (const pattern of CONSENSUS_PATTERNS) {
    const m = pattern.exec(text)
    if (!m) continue
    const who = normalizeName(m[1] ?? "")
    if (!who) continue
    // 排除人称代词与指示词（它们不是名字）
    if (/^(我|你|他|她|它|咱|俺)$/.test(who)) continue
    if (/^(这块|这个|那个|这里|那里|这边|那边)/.test(who)) continue
    if (/^(让他|让她|找你|找我|让他)/.test(who)) continue
    return {
      kind: "consensus",
      title: `${who} 负责相关事项`,
      category: "人/职责",
      symptom: text.slice(0, 60),
      steps: [],
      facts: [{ k: "负责人", v: who }],
      source: `群内共识@${input.sessionId ?? "会话"}${input.chatId ? `#${input.chatId}` : ""}`,
      confidence: "inferred",
      evidence: text,
    }
  }
  return null
}

/** 把粗抽片段归一成名字。
 *
 * ## 为何是“取开头”而不是“剥后缀”（这里返工过两次）
 *
 * 初版用“剥动词后缀再取末尾 2~3 字”，对长尾巴会错位：
 * 「张三处理就行」→ **「理就行」**；「这块李四」→ **「块李四」**。
 * 中文名字总在短语**开头**（后面才是动词/语气），所以取前部更稳。
 */
function normalizeName(raw: string): string {
  let s = raw.trim()
  if (!s) return ""
  // 人称代词开头 → **直接拒**（“我处理”、“你看看”、“让他负责”都不是职责）。
  // 必须先判：若先截 2 字会变成“我处/你看/让他”，代词规则就漏过去了（实测踩到）。
  if (/^[我你他她它咱俺]/.test(s)) return ""
  if (/^(让他|让她|让您)/.test(s)) return ""
  // 英文名：取开头连续字母
  const ascii = /^[A-Za-z][A-Za-z.\-]{1,15}/.exec(s)
  if (ascii) return ascii[0]
  // 指示词开头说明粗抽偏移了（“这块李四”）：跳过后再取
  if (/^(这块|这个|那个|这里|那里|这边|那边)/.test(s)) s = s.slice(2)
  return takeNamePrefix(s) ?? ""
}

/** 从粗抽片段的前部取名字（中文名带 2~3 字，**先试 2 字**）。 *
 * 为何先取 2 字：实测「张三处理就行」取 3 字会得到 **「张三处」**（多吃一个动词首字）。
 * 中文人名 2 字与 3 字都很常见，但**词尾边界不可知**——而“张三位”这样的错名
 * 比“漏掉三字名”危害大得多（前者会造出一个不存在的人）。
 * 所以：默认取 2 字；只有后一个字不是常见动词首字时才扩到 3 字。
 */
const VERB_HEADS = "处负看跟搞弄来就吧呢了的管问找"

function takeNamePrefix(s: string): string | null {
  const zh = /^[\u4e00-\u9fa5]{2,}/.exec(s)
  if (!zh) return null
  const run = zh[0]
  if (run.length >= 3 && !VERB_HEADS.includes(run[2]!)) return run.slice(0, 3)
  return run.slice(0, 2)
}

/**
 * 过滤：这条候选值不值得写。
 *
 * 独立于识别——识别只管“像不像”，这里管“该不该”。
 * 分开的好处：识别规则可以放宽（少漏），写入门槛单独把关（防污染）。
 */
export function shouldLearn(candidate: LearnedCandidate | null): { ok: boolean; why: string } {
  if (!candidate) return { ok: false, why: "没识别到可学的东西" }
  // 标题太短 = 信息量不足，写进去也检索不到
  if (candidate.title.trim().length < 6) return { ok: false, why: "标题太短，信息量不足" }
  // 没有出处的知识事后无法判断该不该信
  if (!candidate.source.trim()) return { ok: false, why: "缺出处" }
  // 纯数值/日期类答案不该进库（会过期，下次答旧值）
  if (NOISE_PATTERNS.some((p) => p.test(candidate.title))) return { ok: false, why: "时效性内容（会过期）" }
  return { ok: true, why: "" }
}
