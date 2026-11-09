/**
 * 发送者判定专项测试：**每条消息都进上下文，但不是每条都触发执行**。
 *
 * 这里钉的是两条决策的分工。最要紧的一条：
 * **`record` 恒为 true** —— 本模块不再有否决任何消息录入的权力。
 * 旧实现把"不该回"实现成"直接丢弃"，导致上下文出现缺口（人格看不到用户
 * 说过什么、也看不到自己刚代发过什么），而它之后所有判断都建立在残缺上下文上。
 */
import { describe, expect, test } from "bun:test"
import { classifySpeaker, replyToOwnerFromEnv, speakerNote } from "./speaker-filter"

const OWNER = "ou_85e5456855ccb5cb39454eafddea829c"
const STRANGER = "ou_other_person"

describe("record 恒为 true（本模块无权否决录入）", () => {
  test("三种发送者、两种 replyToOwner 配置，全都必须录入", () => {
    const cases = [
      { senderId: OWNER, ownerOpenId: OWNER, isSelf: true },
      { senderId: OWNER, ownerOpenId: OWNER, isSelf: false },
      { senderId: STRANGER, ownerOpenId: OWNER, isSelf: false },
      // 判定不了的边界情形同样必须录入
      { senderId: OWNER, ownerOpenId: undefined, isSelf: false },
      { senderId: undefined, ownerOpenId: OWNER, isSelf: false },
      { senderId: "", ownerOpenId: "", isSelf: false },
    ]
    for (const c of cases) {
      for (const replyToOwner of [true, false]) {
        const v = classifySpeaker({ ...c, replyToOwner })
        expect(v.record).toBe(true)
      }
    }
  })
})

describe("人格自己代发的回灌：记入但不执行", () => {
  test("isSelf → record=true, deliver=false，且不可配置", () => {
    // 把自己刚说的话当新指令重答一遍，是确定错误而不只是噪音——所以两个开关组合都不执行
    for (const replyToOwner of [true, false]) {
      const v = classifySpeaker({ senderId: OWNER, ownerOpenId: OWNER, isSelf: true, replyToOwner })
      expect(v.kind).toBe("self")
      expect(v.record).toBe(true)
      expect(v.deliver).toBe(false)
      expect(v.reason).toContain("回灌")
    }
  })
})

describe("用户本人", () => {
  test("缺省配置（replyToOwner=true）→ 记入 + 执行", () => {
    // 这是主要使用方式：用户在群里说「帮我查一下」，人格就该动
    const v = classifySpeaker({ senderId: OWNER, ownerOpenId: OWNER, isSelf: false, replyToOwner: true })
    expect(v.kind).toBe("owner")
    expect(v.record).toBe(true)
    expect(v.deliver).toBe(true)
  })

  test("显式关掉 → 只记入不执行（上下文不能缺）", () => {
    const v = classifySpeaker({ senderId: OWNER, ownerOpenId: OWNER, isSelf: false, replyToOwner: false })
    expect(v.kind).toBe("owner")
    expect(v.record).toBe(true) // 关键：仍然录入
    expect(v.deliver).toBe(false)
    expect(v.reason).toContain("GEBAI_PERSONA_REPLY_TO_OWNER")
  })
})

describe("其他群成员", () => {
  test("永远记入 + 执行（与 replyToOwner 无关）", () => {
    for (const replyToOwner of [true, false]) {
      const v = classifySpeaker({ senderId: STRANGER, ownerOpenId: OWNER, isSelf: false, replyToOwner })
      expect(v.kind).toBe("other")
      expect(v.record).toBe(true)
      expect(v.deliver).toBe(true)
    }
  })
})

describe("判定边界（这些错了会让消息走错分支）", () => {
  test("拿不到 ownerOpenId（未授权）→ 不能把所有人当 owner", () => {
    const v = classifySpeaker({ senderId: OWNER, isSelf: false, replyToOwner: false })
    expect(v.kind).toBe("other") // 无法判定 → other
    expect(v.deliver).toBe(true)
  })

  test("消息缺 senderId → 不能匹配成 owner", () => {
    const v = classifySpeaker({ ownerOpenId: OWNER, isSelf: false, replyToOwner: false })
    expect(v.kind).toBe("other")
    expect(v.deliver).toBe(true)
  })

  test("owner 与 sender 都为空 → 不能因为“两边都空”判成匹配", () => {
    // 最阴的一种：两个空值相等。判它匹配会让所有缺字段的消息走错分支
    const v = classifySpeaker({ ownerOpenId: "", senderId: "", isSelf: false, replyToOwner: true })
    expect(v.kind).toBe("other")
  })

  test("前后空白不影响匹配（飞书字段偶尔带空白）", () => {
    const v = classifySpeaker({ senderId: ` ${OWNER} `, ownerOpenId: OWNER, isSelf: false, replyToOwner: true })
    expect(v.kind).toBe("owner")
  })

  test("isSelf 优先于 owner 判定（自己代发的即使 openId 与用户相同也算 self）", () => {
    // 这正是 as_user 发言的形态：sender 是用户本人，但消息 id 命中发送记录
    const v = classifySpeaker({ senderId: OWNER, ownerOpenId: OWNER, isSelf: true, replyToOwner: true })
    expect(v.kind).toBe("self")
    expect(v.deliver).toBe(false)
  })
})

describe("配置解析（缺省从 false 改为 true）", () => {
  test("未配置 = 开（用户说话就该它干活）", () => {
    expect(replyToOwnerFromEnv({})).toBe(true)
  })

  test("显式关掉的常见写法都认", () => {
    for (const v of ["0", "false", "off", "no", "FALSE", " Off "]) {
      expect(replyToOwnerFromEnv({ GEBAI_PERSONA_REPLY_TO_OWNER: v })).toBe(false)
    }
  })

  test("真值与垃圾值都回 true", () => {
    for (const v of ["1", "true", "on", "yes", "TRUE", " True ", "也许吧"]) {
      expect(replyToOwnerFromEnv({ GEBAI_PERSONA_REPLY_TO_OWNER: v })).toBe(true)
    }
  })
})

describe("身份注记（写进正文，模型据此分辨来历）", () => {
  test("self：明确说这是自己的输出，不是新指令", () => {
    const n = speakerNote("self", "张三")
    expect(n).toContain("你自己")
    expect(n).toContain("张三")
    expect(n).toContain("不要重复作答")
  })

  test("owner：明确说这是人亲手打的", () => {
    const n = speakerNote("owner", "张三")
    expect(n).toContain("亲手打的")
    expect(n).toContain("张三")
  })

  test("other：返回空串（姓名前缀已经够，不叠加噪音）", () => {
    expect(speakerNote("other")).toBe("")
  })

  test("缺姓名时用兜底称呼，不留空洞", () => {
    expect(speakerNote("self")).toContain("用户本人")
    expect(speakerNote("owner", "  ")).toContain("用户本人")
  })
})
