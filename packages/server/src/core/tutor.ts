import { access, mkdir, readFile, rm, writeFile } from "node:fs/promises"
import { dirname, join } from "node:path"
import { sha256Hex, shardPath, walkDir } from "./paths"

/* ---------- 中小学学习辅导（tutor_secondary / tutor_primary 子Agent 共享存储）：学习档案 + 错题本 + 知识点掌握度（用户级持久化） ---------- */

/** 档案字段长度上限（字符）。 */
export const PROFILE_MAX_FIELD = 2000
/** 错题长文本字段（题目/答案/解析）长度上限（字符）。 */
export const MISTAKE_MAX_FIELD = 4000
/** 错题短字段（学科/知识点/来源）长度上限（字符）。 */
export const MISTAKE_MAX_SHORT = 120
/** 错题 id 白名单：小写字母/数字 6-32 位（newMistakeId 生成；防路径穿越）。 */
export const MISTAKE_ID_RE = /^[a-z0-9]{6,32}$/

/** 间隔复习间隔（天）：登记后次日起复习，通过后按连续通过次数递进；连续通过 5 次视为掌握。 */
export const REVIEW_INTERVAL_DAYS = [1, 3, 7, 14, 30] as const
const DAY_MS = 24 * 60 * 60 * 1000

/** 用户身份白名单（档案 role 字段）：决定辅导模式，缺省 student。 */
export const TUTOR_ROLES = ["student", "teacher", "parent"] as const
export type TutorRole = (typeof TUTOR_ROLES)[number]
export const TUTOR_ROLE_LABELS: Record<TutorRole, string> = { student: "学生", teacher: "教师", parent: "家长" }

export interface TutorProfile {
  /** 用户身份：student 学生本人（缺省）/ teacher 教师 / parent 家长——决定辅导模式。 */
  role?: TutorRole
  /** 年级（初一~高三）。 */
  grade?: string
  /** 教材版本（如 人教版数学/北师大版物理）。 */
  textbook?: string
  /** 学习目标（如 中考冲刺、期末数学上 110）。 */
  goal?: string
  /** 薄弱学科与知识点。 */
  weaknesses?: string
  /** 考试日程（如 6月17-19 中考）。 */
  exams?: string
  /** 教学偏好与其他备注（如 只要思路不要答案）。 */
  notes?: string
  updatedAt: number
}

export type MistakeStatus = "active" | "mastered"
export type ReviewResult = "pass" | "fail" | "master"

export interface MistakeRecord {
  id: string
  /** 学科（语文/数学/英语/物理/化学/生物/政治/历史/地理）。 */
  subject: string
  /** 知识点（如 一元二次方程）。 */
  topic: string
  /** 题目原文。 */
  question: string
  studentAnswer?: string
  correctAnswer?: string
  /** 错因分析与正确思路。 */
  analysis?: string
  /** 题目来源（作业/月考卷等）。 */
  source?: string
  createdAt: number
  updatedAt: number
  status: MistakeStatus
  /** 连续通过次数（fail 归零）。 */
  reviewCount: number
  /** 累计复习次数。 */
  reviewTotal: number
  /** 下次应复习时间（mastered 后不再排期）。 */
  nextReviewAt: number
  lastReviewedAt?: number
}

export interface MistakeFilter {
  /** 学科精确匹配。 */
  subject?: string
  /** 知识点子串匹配。 */
  topic?: string
  /** 状态过滤，缺省 active（复习中）。 */
  status?: MistakeStatus
  /** 只列到期（nextReviewAt <= dueBefore）的 active 错题。 */
  dueBefore?: number
}

const PROFILE_FIELDS = ["role", "grade", "textbook", "goal", "weaknesses", "exams", "notes"] as const
type ProfileField = (typeof PROFILE_FIELDS)[number]

export function tutorDir(home: string, user: string): string {
  return join(home, "users", user, "tutor")
}

export function profilePath(home: string, user: string): string {
  return join(tutorDir(home, user), "profile.json")
}

export function mistakePath(home: string, user: string, id: string): string {
  if (!MISTAKE_ID_RE.test(id)) throw new Error(`非法错题 id: ${id}`)
  const [h0, h1] = shardPath(id, 2)
  return join(tutorDir(home, user), "mistakes", h0, h1, `${id}.json`)
}

/** 生成错题 id：时间戳 base36 + 随机后缀（可注入 now 供测试）。 */
export function newMistakeId(now: number = Date.now()): string {
  return now.toString(36) + Math.random().toString(36).slice(2, 8)
}

function capField(v: unknown, max: number, label: string): string {
  const s = String(v ?? "")
  if (s.length > max) throw new Error(`${label}超限（${s.length} 字符，上限 ${max}）`)
  return s
}

function requiredField(v: unknown, max: number, label: string): string {
  const s = capField(v, max, label)
  if (!s.trim()) throw new Error(`${label}不能为空`)
  return s
}

/* ---------- 学习档案 ---------- */

export async function loadProfile(home: string, user: string): Promise<TutorProfile | null> {
  try {
    const raw = JSON.parse(await readFile(profilePath(home, user), "utf8")) as TutorProfile
    return raw && typeof raw === "object" ? raw : null
  } catch {
    return null
  }
}

/** 合并更新档案：仅覆盖传入字段（传空串清除该字段），返回更新后完整档案。 */
export async function saveProfile(
  home: string,
  user: string,
  patch: Partial<Record<ProfileField, string>>,
): Promise<TutorProfile> {
  const next: TutorProfile = (await loadProfile(home, user)) ?? { updatedAt: 0 }
  for (const key of PROFILE_FIELDS) {
    const v = patch[key]
    if (v == null) continue
    const s = capField(v, PROFILE_MAX_FIELD, `档案字段 ${key}`)
    if (s === "") {
      delete next[key]
      continue
    }
    if (key === "role") {
      if (!TUTOR_ROLES.includes(s as TutorRole)) {
        throw new Error(`身份 role 须为 ${TUTOR_ROLES.join("/")}（学生/教师/家长）`)
      }
      next.role = s as TutorRole
    } else {
      next[key] = s
    }
  }
  next.updatedAt = Date.now()
  const p = profilePath(home, user)
  await mkdir(dirname(p), { recursive: true })
  await writeFile(p, JSON.stringify(next))
  return next
}

/* ---------- 错题本 ---------- */

export async function addMistake(
  home: string,
  user: string,
  input: {
    subject: string
    topic: string
    question: string
    studentAnswer?: string
    correctAnswer?: string
    analysis?: string
    source?: string
    /** 创建时间（可注入供测试），同时用于 id 生成与首次复习排期。 */
    now?: number
  },
): Promise<MistakeRecord> {
  const now = input.now ?? Date.now()
  const id = newMistakeId(now)
  const m: MistakeRecord = {
    id,
    subject: requiredField(input.subject, MISTAKE_MAX_SHORT, "学科"),
    topic: requiredField(input.topic, MISTAKE_MAX_SHORT, "知识点"),
    question: requiredField(input.question, MISTAKE_MAX_FIELD, "题目"),
    studentAnswer: input.studentAnswer != null ? capField(input.studentAnswer, MISTAKE_MAX_FIELD, "学生答案") || undefined : undefined,
    correctAnswer: input.correctAnswer != null ? capField(input.correctAnswer, MISTAKE_MAX_FIELD, "正确答案") || undefined : undefined,
    analysis: input.analysis != null ? capField(input.analysis, MISTAKE_MAX_FIELD, "解析") || undefined : undefined,
    source: input.source != null ? capField(input.source, MISTAKE_MAX_SHORT, "来源") || undefined : undefined,
    createdAt: now,
    updatedAt: now,
    status: "active",
    reviewCount: 0,
    reviewTotal: 0,
    nextReviewAt: now + REVIEW_INTERVAL_DAYS[0] * DAY_MS,
  }
  const p = mistakePath(home, user, id)
  await mkdir(dirname(p), { recursive: true })
  await writeFile(p, JSON.stringify(m))
  return m
}

async function readMistakeFile(p: string): Promise<MistakeRecord | null> {
  try {
    const m = JSON.parse(await readFile(p, "utf8")) as MistakeRecord
    return m && typeof m.id === "string" ? m : null
  } catch {
    return null
  }
}

export async function getMistake(home: string, user: string, id: string): Promise<MistakeRecord | null> {
  if (!MISTAKE_ID_RE.test(id)) return null
  return readMistakeFile(mistakePath(home, user, id))
}

/** 列出错题（过滤后排序：active 按下次复习时间升序在前，mastered 按更新时间降序在后）。 */
export async function listMistakes(home: string, user: string, filter: MistakeFilter = {}): Promise<MistakeRecord[]> {
  const out: MistakeRecord[] = []
  await walkDir(join(tutorDir(home, user), "mistakes"), 2, async (p) => {
    if (!p.endsWith(".json")) return
    const m = await readMistakeFile(p)
    if (!m) return
    if (filter.subject && m.subject !== filter.subject) return
    if (filter.topic && !m.topic.includes(filter.topic)) return
    if (m.status !== (filter.status ?? "active")) return
    if (filter.dueBefore != null && m.nextReviewAt > filter.dueBefore) return
    out.push(m)
  })
  out.sort((a, b) => {
    if (a.status !== b.status) return a.status === "active" ? -1 : 1
    if (a.status === "active") return a.nextReviewAt - b.nextReviewAt
    return b.updatedAt - a.updatedAt
  })
  return out
}

/** 复习结果推进（纯函数）：fail=重置为 1 天后再复习；pass=按连续通过次数递进间隔（1/3/7/14/30 天），
 *  连续通过 5 次掌握；master=直接掌握；对已掌握记录 pass 保持掌握、fail 重新回到复习。 */
export function applyReview(
  m: Pick<MistakeRecord, "status" | "reviewCount" | "reviewTotal">,
  result: ReviewResult,
  now: number,
): Pick<MistakeRecord, "status" | "reviewCount" | "reviewTotal" | "nextReviewAt" | "lastReviewedAt"> {
  const base = { reviewTotal: m.reviewTotal + 1, lastReviewedAt: now }
  if (result === "fail") {
    return { ...base, status: "active", reviewCount: 0, nextReviewAt: now + REVIEW_INTERVAL_DAYS[0] * DAY_MS }
  }
  if (result === "master" || m.status === "mastered") {
    return { ...base, status: "mastered", reviewCount: Math.max(m.reviewCount, REVIEW_INTERVAL_DAYS.length), nextReviewAt: now }
  }
  const count = m.reviewCount + 1
  if (count >= REVIEW_INTERVAL_DAYS.length) {
    return { ...base, status: "mastered", reviewCount: count, nextReviewAt: now }
  }
  return { ...base, status: "active", reviewCount: count, nextReviewAt: now + REVIEW_INTERVAL_DAYS[count - 1] * DAY_MS }
}

/** 汇报复习结果并落盘：按 applyReview 更新排期，返回更新后记录；不存在返回 null。 */
export async function reviewMistake(
  home: string,
  user: string,
  id: string,
  result: ReviewResult,
  now: number = Date.now(),
): Promise<MistakeRecord | null> {
  const m = await getMistake(home, user, id)
  if (!m) return null
  const next: MistakeRecord = { ...m, ...applyReview(m, result, now), updatedAt: now }
  await writeFile(mistakePath(home, user, id), JSON.stringify(next))
  return next
}

export async function removeMistake(home: string, user: string, id: string): Promise<boolean> {
  if (!MISTAKE_ID_RE.test(id)) return false
  const p = mistakePath(home, user, id)
  try {
    await access(p)
  } catch {
    return false
  }
  await rm(p, { force: true })
  return true
}

/* ---------- 知识点掌握度（引导式教学的诊断依据） ---------- */

/** 掌握度分级：0 未学 / 1 薄弱 / 2 一般 / 3 良好 / 4 掌握。 */
export const MASTERY_MIN = 0
export const MASTERY_MAX = 4
export const MASTERY_LABELS = ["未学", "薄弱", "一般", "良好", "掌握"] as const
/** 掌握度变化轨迹保留条数（最近在前）。 */
export const KNOWLEDGE_HISTORY_MAX = 5

export interface KnowledgeRecord {
  subject: string
  topic: string
  mastery: number
  /** 评估轨迹（最近在前，上限 5 条）：变化「2→1 依据」/ 同级「1 依据」/ 新建「设为 2 依据」。 */
  history: string[]
  createdAt: number
  updatedAt: number
}

/** 学科+知识点 → 存储键（sha256 前 16 位 hex，upsert 天然去重）。 */
export function knowledgeKey(subject: string, topic: string): string {
  return sha256Hex(`${subject}\n${topic}`).slice(0, 16)
}

export function knowledgePath(home: string, user: string, subject: string, topic: string): string {
  const key = knowledgeKey(subject, topic)
  const [h0, h1] = shardPath(key, 2)
  return join(tutorDir(home, user), "knowledge", h0, h1, `${key}.json`)
}

/** 设置/更新掌握度：同 subject+topic 覆盖更新（记录变化轨迹），返回更新后记录。 */
export async function upsertKnowledge(
  home: string,
  user: string,
  input: { subject: string; topic: string; mastery: number; evidence?: string; now?: number },
): Promise<KnowledgeRecord> {
  const subject = requiredField(input.subject, MISTAKE_MAX_SHORT, "学科")
  const topic = requiredField(input.topic, MISTAKE_MAX_SHORT, "知识点")
  const mastery = input.mastery
  if (!Number.isInteger(mastery) || mastery < MASTERY_MIN || mastery > MASTERY_MAX) {
    throw new Error(`掌握度须为 ${MASTERY_MIN}~${MASTERY_MAX} 整数（${MASTERY_LABELS.join("/")}）`)
  }
  const evidence = input.evidence != null ? capField(input.evidence, MISTAKE_MAX_SHORT * 2, "评估依据") : undefined
  const now = input.now ?? Date.now()
  const p = knowledgePath(home, user, subject, topic)
  let existing: KnowledgeRecord | null = null
  try {
    existing = JSON.parse(await readFile(p, "utf8")) as KnowledgeRecord
    if (!existing || typeof existing.subject !== "string") existing = null
  } catch {
    existing = null
  }
  const entry = existing
    ? existing.mastery === mastery
      ? `${mastery}${evidence ? ` ${evidence}` : ""}`
      : `${existing.mastery}→${mastery}${evidence ? ` ${evidence}` : ""}`
    : `设为 ${mastery}${evidence ? ` ${evidence}` : ""}`
  const next: KnowledgeRecord = {
    subject,
    topic,
    mastery,
    history: [entry, ...(existing?.history ?? [])].slice(0, KNOWLEDGE_HISTORY_MAX),
    createdAt: existing?.createdAt ?? now,
    updatedAt: now,
  }
  await mkdir(dirname(p), { recursive: true })
  await writeFile(p, JSON.stringify(next))
  return next
}

/** 列出掌握度记录（排序：掌握度升序（薄弱在前）→ 同级按更新时间降序）。 */
export async function listKnowledge(
  home: string,
  user: string,
  filter: { subject?: string; /** 只列掌握度 ≤ 该值的项（找薄弱点）。 */ maxMastery?: number } = {},
): Promise<KnowledgeRecord[]> {
  const out: KnowledgeRecord[] = []
  await walkDir(join(tutorDir(home, user), "knowledge"), 2, async (p) => {
    if (!p.endsWith(".json")) return
    let k: KnowledgeRecord | null = null
    try {
      k = JSON.parse(await readFile(p, "utf8")) as KnowledgeRecord
    } catch {
      return
    }
    if (!k || typeof k.subject !== "string" || typeof k.mastery !== "number") return
    if (filter.subject && k.subject !== filter.subject) return
    if (filter.maxMastery != null && k.mastery > filter.maxMastery) return
    out.push(k)
  })
  out.sort((a, b) => a.mastery - b.mastery || b.updatedAt - a.updatedAt)
  return out
}
