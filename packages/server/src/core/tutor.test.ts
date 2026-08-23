import { describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import {
  MISTAKE_ID_RE,
  PROFILE_MAX_FIELD,
  REVIEW_INTERVAL_DAYS,
  addMistake,
  applyReview,
  getMistake,
  knowledgePath,
  listKnowledge,
  listMistakes,
  loadProfile,
  mistakePath,
  newMistakeId,
  profilePath,
  removeMistake,
  reviewMistake,
  saveProfile,
  upsertKnowledge,
} from "./tutor"
import type { MistakeRecord } from "./tutor"

const DAY = 24 * 60 * 60 * 1000

function cleanup(home: string): void {
  rmSync(home, { recursive: true, force: true })
}

describe("applyReview（间隔复习纯函数）", () => {
  const t0 = 1_700_000_000_000
  test("pass 按连续通过次数递进 1/3/7/14 天，第 5 次掌握", () => {
    type S = Pick<MistakeRecord, "status" | "reviewCount" | "reviewTotal" | "nextReviewAt" | "lastReviewedAt">
    let m: S = { status: "active", reviewCount: 0, reviewTotal: 0, nextReviewAt: 0 }
    m = { ...m, ...applyReview(m, "pass", t0) }
    expect(m.status).toBe("active")
    expect(m.reviewCount).toBe(1)
    expect(m.nextReviewAt).toBe(t0 + 1 * DAY)
    m = { ...m, ...applyReview(m, "pass", t0) }
    expect(m.reviewCount).toBe(2)
    expect(m.nextReviewAt).toBe(t0 + 3 * DAY)
    m = { ...m, ...applyReview(m, "pass", t0) }
    expect(m.nextReviewAt).toBe(t0 + 7 * DAY)
    m = { ...m, ...applyReview(m, "pass", t0) }
    expect(m.nextReviewAt).toBe(t0 + 14 * DAY)
    expect(m.status).toBe("active")
    m = { ...m, ...applyReview(m, "pass", t0) }
    expect(m.status).toBe("mastered")
    expect(m.reviewCount).toBe(REVIEW_INTERVAL_DAYS.length)
  })
  test("fail 重置连续次数、1 天后再复习", () => {
    const m = applyReview({ status: "active", reviewCount: 3, reviewTotal: 5 }, "fail", t0)
    expect(m.status).toBe("active")
    expect(m.reviewCount).toBe(0)
    expect(m.nextReviewAt).toBe(t0 + 1 * DAY)
    expect(m.reviewTotal).toBe(6)
  })
  test("master 直接掌握；已掌握记录 pass 保持掌握、fail 回到复习", () => {
    const mastered = applyReview({ status: "active", reviewCount: 1, reviewTotal: 1 }, "master", t0)
    expect(mastered.status).toBe("mastered")
    const stay = applyReview({ status: "mastered", reviewCount: 5, reviewTotal: 5 }, "pass", t0)
    expect(stay.status).toBe("mastered")
    const back = applyReview({ status: "mastered", reviewCount: 5, reviewTotal: 5 }, "fail", t0)
    expect(back.status).toBe("active")
    expect(back.reviewCount).toBe(0)
  })
})

describe("学习档案", () => {
  test("初始为空，saveProfile 合并更新、空串清除字段", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      expect(await loadProfile(home, "u1")).toBeNull()
      const a = await saveProfile(home, "u1", { grade: "初三", goal: "中考 650" })
      expect(a.grade).toBe("初三")
      expect(a.goal).toBe("中考 650")
      expect(existsSync(profilePath(home, "u1"))).toBe(true)
      // 合并：未传字段保留，传入字段覆盖
      const b = await saveProfile(home, "u1", { goal: "中考 680", weaknesses: "二次函数、电磁感应" })
      expect(b.grade).toBe("初三")
      expect(b.goal).toBe("中考 680")
      expect(b.weaknesses).toBe("二次函数、电磁感应")
      // 空串清除
      const c = await saveProfile(home, "u1", { goal: "" })
      expect(c.goal).toBeUndefined()
      expect(c.grade).toBe("初三")
      // 用户隔离
      expect(await loadProfile(home, "u2")).toBeNull()
    } finally {
      cleanup(home)
    }
  })
  test("role 身份：白名单校验、空串清除、不影响其他字段", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      const a = await saveProfile(home, "u1", { role: "parent", grade: "三年级" })
      expect(a.role).toBe("parent")
      expect((await loadProfile(home, "u1"))?.role).toBe("parent")
      // 换身份与非法身份
      await saveProfile(home, "u1", { role: "teacher" })
      await expect(saveProfile(home, "u1", { role: "admin" })).rejects.toThrow("role")
      // 空串清除（回退缺省学生），未传字段保留
      const c = await saveProfile(home, "u1", { role: "" })
      expect(c.role).toBeUndefined()
      expect(c.grade).toBe("三年级")
    } finally {
      cleanup(home)
    }
  })
  test("字段超限拒绝", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      await expect(saveProfile(home, "u1", { goal: "x".repeat(PROFILE_MAX_FIELD + 1) })).rejects.toThrow("超限")
    } finally {
      cleanup(home)
    }
  })
})

describe("错题本", () => {
  test("addMistake 落盘到 id 哈希分片路径，次日首复", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      const now = 1_700_000_000_000
      const m = await addMistake(home, "u1", {
        subject: "数学",
        topic: "一元二次方程",
        question: "解方程 x²-3x+2=0",
        studentAnswer: "x=1",
        correctAnswer: "x=1 或 x=2",
        analysis: "因式分解未取全解",
        source: "作业",
        now,
      })
      expect(MISTAKE_ID_RE.test(m.id)).toBe(true)
      expect(newMistakeId(now).length).toBeGreaterThan(5)
      const p = mistakePath(home, "u1", m.id)
      expect(p).toContain(join("users", "u1", "tutor", "mistakes"))
      expect(existsSync(p)).toBe(true)
      expect(m.nextReviewAt).toBe(now + 1 * DAY)
      expect(await getMistake(home, "u1", m.id)).not.toBeNull()
      // 其他用户不可见
      expect(await getMistake(home, "u2", m.id)).toBeNull()
    } finally {
      cleanup(home)
    }
  })
  test("必填校验与长度上限", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      await expect(addMistake(home, "u1", { subject: "", topic: "t", question: "q" })).rejects.toThrow("学科")
      await expect(addMistake(home, "u1", { subject: "数学", topic: "  ", question: "q" })).rejects.toThrow("知识点")
      await expect(addMistake(home, "u1", { subject: "数学", topic: "t", question: "x".repeat(4001) })).rejects.toThrow("超限")
      // 非法 id（路径穿越）被白名单拒绝
      expect(await getMistake(home, "u1", "../../etc")).toBeNull()
      expect(await removeMistake(home, "u1", "../../etc")).toBe(false)
    } finally {
      cleanup(home)
    }
  })
  test("listMistakes 过滤（学科/知识点子串/状态/到期）与排序", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      const now = 1_700_000_000_000
      const a = await addMistake(home, "u1", { subject: "数学", topic: "一元二次方程", question: "q1", now: now })
      const b = await addMistake(home, "u1", { subject: "数学", topic: "全等三角形", question: "q2", now: now + 1000 })
      const c = await addMistake(home, "u1", { subject: "英语", topic: "定语从句", question: "q3", now: now + 2000 })
      // 默认只列 active，按 nextReviewAt 升序
      const all = await listMistakes(home, "u1")
      expect(all.map((m) => m.id)).toEqual([a.id, b.id, c.id])
      // 学科过滤
      expect((await listMistakes(home, "u1", { subject: "数学" })).map((m) => m.id)).toEqual([a.id, b.id])
      // 知识点子串
      expect((await listMistakes(home, "u1", { topic: "方程" })).map((m) => m.id)).toEqual([a.id])
      // 到期过滤：只有 a 到期
      expect((await listMistakes(home, "u1", { dueBefore: a.nextReviewAt })).map((m) => m.id)).toEqual([a.id])
      // 掌握后默认不列，status=mastered 可查
      await reviewMistake(home, "u1", c.id, "master", now)
      expect((await listMistakes(home, "u1")).map((m) => m.id)).toEqual([a.id, b.id])
      expect((await listMistakes(home, "u1", { status: "mastered" })).map((m) => m.id)).toEqual([c.id])
    } finally {
      cleanup(home)
    }
  })
  test("reviewMistake 推进排期；不存在返回 null", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      const now = 1_700_000_000_000
      const m = await addMistake(home, "u1", { subject: "物理", topic: "浮力", question: "q", now })
      const pass = await reviewMistake(home, "u1", m.id, "pass", now + 2 * DAY)
      expect(pass?.reviewCount).toBe(1)
      expect(pass?.nextReviewAt).toBe(now + 2 * DAY + 1 * DAY)
      expect(pass?.reviewTotal).toBe(1)
      const fail = await reviewMistake(home, "u1", m.id, "fail", now + 5 * DAY)
      expect(fail?.reviewCount).toBe(0)
      expect(fail?.nextReviewAt).toBe(now + 5 * DAY + 1 * DAY)
      expect(await reviewMistake(home, "u1", "noexist123", "pass", now)).toBeNull()
    } finally {
      cleanup(home)
    }
  })
  test("removeMistake 删除后不可查", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      const m = await addMistake(home, "u1", { subject: "化学", topic: "化学方程式配平", question: "q" })
      expect(await removeMistake(home, "u1", m.id)).toBe(true)
      expect(await getMistake(home, "u1", m.id)).toBeNull()
      expect(await removeMistake(home, "u1", m.id)).toBe(false)
    } finally {
      cleanup(home)
    }
  })
})

describe("知识点掌握度", () => {
  test("upsert：同 subject+topic 去重合并，记录变化轨迹（新建/变化/同级）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      const now = 1_700_000_000_000
      const a = await upsertKnowledge(home, "u1", { subject: "数学", topic: "一元二次方程", mastery: 2, evidence: "作业 4/5 对", now })
      expect(a.mastery).toBe(2)
      expect(a.history).toEqual(["设为 2 作业 4/5 对"])
      expect(existsSync(knowledgePath(home, "u1", "数学", "一元二次方程"))).toBe(true)

      const b = await upsertKnowledge(home, "u1", { subject: "数学", topic: "一元二次方程", mastery: 1, evidence: "练习 3/5 对", now: now + 1000 })
      expect(b.createdAt).toBe(a.createdAt)
      expect(b.history[0]).toBe("2→1 练习 3/5 对")
      expect(b.history).toHaveLength(2)

      const c = await upsertKnowledge(home, "u1", { subject: "数学", topic: "一元二次方程", mastery: 1, evidence: "复习仍错", now: now + 2000 })
      expect(c.history[0]).toBe("1 复习仍错")

      // 大小写/空白不同的同学科同知识点分别记录；其他用户隔离
      await upsertKnowledge(home, "u1", { subject: "英语", topic: "定语从句", mastery: 0, now })
      expect(await listKnowledge(home, "u1")).toHaveLength(2)
      expect(await listKnowledge(home, "u2")).toHaveLength(0)
    } finally {
      cleanup(home)
    }
  })
  test("mastery 越界与必填校验", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      await expect(upsertKnowledge(home, "u1", { subject: "数学", topic: "t", mastery: 5 })).rejects.toThrow("掌握度")
      await expect(upsertKnowledge(home, "u1", { subject: "数学", topic: "t", mastery: 1.5 })).rejects.toThrow("掌握度")
      await expect(upsertKnowledge(home, "u1", { subject: "", topic: "t", mastery: 1 })).rejects.toThrow("学科")
    } finally {
      cleanup(home)
    }
  })
  test("listKnowledge 过滤（学科/薄弱 ≤N）与排序（掌握度升序、同级近更新在前）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-"))
    try {
      const now = 1_700_000_000_000
      await upsertKnowledge(home, "u1", { subject: "数学", topic: "函数图像", mastery: 3, now })
      await upsertKnowledge(home, "u1", { subject: "数学", topic: "一元二次方程", mastery: 1, now: now + 1000 })
      await upsertKnowledge(home, "u1", { subject: "物理", topic: "浮力", mastery: 1, now: now + 2000 })
      await upsertKnowledge(home, "u1", { subject: "英语", topic: "定语从句", mastery: 4, now })
      // 全部：薄弱在前；两级同为 1 时近更新在前（浮力 > 一元二次方程）
      const all = await listKnowledge(home, "u1")
      expect(all.map((k) => k.topic)).toEqual(["浮力", "一元二次方程", "函数图像", "定语从句"])
      // 学科过滤
      expect((await listKnowledge(home, "u1", { subject: "数学" })).map((k) => k.topic)).toEqual(["一元二次方程", "函数图像"])
      // 薄弱过滤（≤2，规划找薄弱点用）
      expect((await listKnowledge(home, "u1", { maxMastery: 2 })).map((k) => k.topic)).toEqual(["浮力", "一元二次方程"])
    } finally {
      cleanup(home)
    }
  })
})
