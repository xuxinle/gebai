import { describe, expect, test } from "bun:test"
import { classifySpeaker, replyToOwnerFromEnv } from "./speaker-filter"

const OWNER = "ou_85e5456855ccb5cb39454eafddea829c"
const STRANGER = "ou_other_person"

describe("发送者判定：三种人的处置", () => {
  test("人格自己发的（发送记录命中）→ 永远丢弃，且**不可配置**", () => {
    // 把自己的话当新指令是确定错误，不是噪音——所以 replyToOwner 管不到它
    for (const replyToOwner of [true, false]) {
      const v = classifySpeaker({ senderId: OWNER, ownerOpenId: OWNER, isSelf: true, replyToOwner })
      expect(v.deliver).toBe(false)
      expect(v.kind).toBe("self")
      expect(v.reason).toContain("防回灌")
    }
  })

  test("用户本人 + 默认配置 → 不回复（调试口径）", () => {
    const v = classifySpeaker({ senderId: OWNER, ownerOpenId: OWNER, isSelf: false, replyToOwner: false })
    expect(v.deliver).toBe(false)
    expect(v.kind).toBe("owner")
    // 日志要能让人一眼看出为什么没回，并指出开关名
    expect(v.reason).toContain("GEBAI_PERSONA_REPLY_TO_OWNER")
  })

  test("用户本人 + 开启配置 → 回复", () => {
    const v = classifySpeaker({ senderId: OWNER, ownerOpenId: OWNER, isSelf: false, replyToOwner: true })
    expect(v.deliver).toBe(true)
    expect(v.kind).toBe("owner")
  })

  test("其他人 → 永远回复（与 replyToOwner 无关）", () => {
    for (const replyToOwner of [true, false]) {
      const v = classifySpeaker({ senderId: STRANGER, ownerOpenId: OWNER, isSelf: false, replyToOwner })
      expect(v.deliver).toBe(true)
      expect(v.kind).toBe("other")
    }
  })
})

describe("发送者判定的边界（这些错了会静默吞消息）", () => {
  test("拿不到 ownerOpenId（未授权）→ 不能把所有消息当成 owner 吞掉", () => {
    const v = classifySpeaker({ senderId: OWNER, isSelf: false, replyToOwner: false })
    expect(v.deliver).toBe(true) // 无法判定 → 按 other 处理，宁可回也不要静默丢
    expect(v.kind).toBe("other")
  })

  test("消息缺 senderId → 不能匹配成 owner", () => {
    const v = classifySpeaker({ ownerOpenId: OWNER, isSelf: false, replyToOwner: false })
    expect(v.deliver).toBe(true)
    expect(v.kind).toBe("other")
  })

  test("ownerOpenId 为空串 → 同“拿不到”", () => {
    const v = classifySpeaker({ senderId: OWNER, ownerOpenId: "", isSelf: false, replyToOwner: false })
    expect(v.deliver).toBe(true)
  })

  test("owner 与 sender 都为空 → 不能因为“两边都空”判成匹配", () => {
    // 这是最阴的一种：两个空值相等。判它匹配会让所有缺字段的消息静默消失
    const v = classifySpeaker({ ownerOpenId: "", senderId: "", isSelf: false, replyToOwner: false })
    expect(v.deliver).toBe(true)
    expect(v.kind).toBe("other")
  })

  test("前后空白不影响匹配（飞书字段偶尔带空白）", () => {
    const v = classifySpeaker({ senderId: ` ${OWNER} `, ownerOpenId: OWNER, isSelf: false, replyToOwner: false })
    expect(v.kind).toBe("owner")
  })
})

describe("配置解析", () => {
  test("缺省 false（调试时不吵自己）", () => {
    expect(replyToOwnerFromEnv({})).toBe(false)
  })

  test("常见真值写法都认", () => {
    for (const v of ["1", "true", "on", "yes", "TRUE", " True "]) {
      expect(replyToOwnerFromEnv({ GEBAI_PERSONA_REPLY_TO_OWNER: v })).toBe(true)
    }
  })

  test("常见假值与垃圾值都回 false", () => {
    for (const v of ["0", "false", "off", "no", "", "也许吧"]) {
      expect(replyToOwnerFromEnv({ GEBAI_PERSONA_REPLY_TO_OWNER: v })).toBe(false)
    }
  })
})
