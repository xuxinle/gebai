import { describe, expect, test } from "bun:test"
import { detectConsensus, detectCorrection, detectProcedure, shouldLearn } from "./memory-learn"

const ctx = { personaId: "xiaoshuang", chatId: "oc_test", sessionId: "sess_1" }

describe("识别：用户纠正（唯一能拿 verified 的自动来源）", () => {
  test("「不对，应该是…」→ 识别为纠正且 verified", () => {
    const c = detectCorrection({ ...ctx, humanText: "不对，应该用 GEBAI_PERSONA_REPLY_TO_OWNER=true" })
    expect(c).not.toBeNull()
    expect(c!.kind).toBe("correction")
    expect(c!.confidence).toBe("verified")
    expect(c!.source).toContain("用户纠正")
    // 出处必须带会话与群，否则事后无法追溯
    expect(c!.source).toContain("sess_1")
    expect(c!.source).toContain("oc_test")
  })

  test("「不是这样」「其实应该是」等变体都能认", () => {
    for (const t of ["不是这样，要走 OA 审批", "其实应该是先查日志再重启", "更正一下：阈值是 5x 不是 3x"]) {
      expect(detectCorrection({ ...ctx, humanText: t })).not.toBeNull()
    }
  })

  test("普通提问不误判为纠正", () => {
    for (const t of ["这个告警怎么处理？", "能帮我看下日志吗", "好的谢谢"]) {
      expect(detectCorrection({ ...ctx, humanText: t })).toBeNull()
    }
  })

  test("太短的文本不算（多半是语气词）", () => {
    expect(detectCorrection({ ...ctx, humanText: "不对" })).toBeNull()
  })
})

describe("识别：群内共识（只能是 inferred）", () => {
  test("「找张三处理」→ 责任人 + inferred", () => {
    const c = detectConsensus({ humanText: "这个找张三处理就行", ...ctx })
    expect(c).not.toBeNull()
    expect(c!.kind).toBe("consensus")
    // 关键：群里说的话可能是玩笑/过时信息，绝不能标 verified
    expect(c!.confidence).toBe("inferred")
    expect(c!.facts?.[0]?.v).toBe("张三")
  })

  test("「XX 负责」也能认", () => {
    const c = detectConsensus({ humanText: "配额这块李四负责", ...ctx })
    expect(c).not.toBeNull()
    expect(c!.facts?.[0]?.v).toBe("李四")
  })

  test("人称代词不算责任人（「找我」不是职责）", () => {
    for (const t of ["这个找我处理", "找你看看", "让他负责"]) {
      const c = detectConsensus({ humanText: t, ...ctx })
      expect(c).toBeNull()
    }
  })
})

describe("「该不该写」的门槛（防污染：宁可少写）", () => {
  test("时效性内容不写（会过期，下次答旧值）", () => {
    const c = detectCorrection({ ...ctx, humanText: "不对，今天几号应该是查日历" })
    // 先确认识别本身是否放过；无论识别结果如何，shouldLearn 必须拦住时效性
    if (c) {
      const verdict = shouldLearn({ ...c, title: "今天几号是多少" })
      expect(verdict.ok).toBe(false)
      expect(verdict.why).toContain("时效")
    }
    // 直接构造一条时效性的也应被拦
    const v = shouldLearn({
      kind: "correction",
      title: "今天几号",
      category: "纠正",
      symptom: "",
      steps: [],
      source: "x",
      confidence: "verified",
      evidence: "",
    })
    expect(v.ok).toBe(false)
  })

  test("缺出处不写（没有出处的知识事后无法判断要不要信）", () => {
    const v = shouldLearn({
      kind: "correction",
      title: "这是一个足够长的标题",
      category: "纠正",
      symptom: "",
      steps: [],
      source: "  ",
      confidence: "verified",
      evidence: "",
    })
    expect(v.ok).toBe(false)
    expect(v.why).toContain("出处")
  })

  test("标题太短不写（检索不到等于没写）", () => {
    const v = shouldLearn({
      kind: "correction",
      title: "短",
      category: "纠正",
      symptom: "",
      steps: [],
      source: "ok",
      confidence: "verified",
      evidence: "",
    })
    expect(v.ok).toBe(false)
  })

  test("正常纠正通过", () => {
    const c = detectCorrection({ ...ctx, humanText: "不对，应该先看 GEBAI_PERSONA_REPLY_TO_OWNER 再判断" })
    expect(shouldLearn(c).ok).toBe(true)
  })

  test("null 候选不写", () => {
    expect(shouldLearn(null).ok).toBe(false)
  })
})

describe("识别：解决过程（门槛最严——顺序≠因果）", () => {
  const base = { chatId: "oc_x", sessionId: "s1" }

  test("真执行过命令 + 答案带具体值 → 收，但只能是 inferred", () => {
    const c = detectProcedure({
      ...base,
      question: "TSDB 写入延迟高怎么办",
      executedCommands: ["curl -s localhost:8428/metrics | grep write"],
      answerText: "写放大到了 7.2x，超过阈值 5x，需要扩容",
    })
    expect(c).not.toBeNull()
    expect(c!.kind).toBe("procedure")
    // 关键：顺序不等于因果，绝不能标 verified
    expect(c!.confidence).toBe("inferred")
    expect(c!.steps.some((s) => s.includes("curl"))).toBe(true)
  })

  test("只是“建议执行”而没有真执行 → 不收", () => {
    const c = detectProcedure({
      ...base,
      question: "这个怎么查",
      executedCommands: [],
      answerText: "你可以执行 curl 看一下，大概 5x 左右",
    })
    expect(c).toBeNull()
  })

  test("答案没有具体值（纯形容）→ 不收（那只是猜测，不是知识）", () => {
    const c = detectProcedure({
      ...base,
      question: "服务慢了怎么办",
      executedCommands: ["systemctl status foo"],
      answerText: "应该是网络问题，重启一下试试",
    })
    expect(c).toBeNull()
  })

  test("时效性问题不学（今天几号）", () => {
    const c = detectProcedure({
      ...base,
      question: "今天几号",
      executedCommands: ["date"],
      answerText: "2026年11月7日，星期六",
    })
    expect(c).toBeNull()
  })
})
