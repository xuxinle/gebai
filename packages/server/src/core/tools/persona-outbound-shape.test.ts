/**
 * 人格出站加工的反向验证。
 *
 * 核心取舍（每一条都对应一个真实代价）：
 *
 * | 变异 | 后果 |
 * |---|---|
 * | 不删 `@所有人` | 人格以用户本人名义点名全群；一次「排版用」的 @all 打扰几百人，用户不知情 |
 * | 署名无幂等 | 重试叠出两行署名 |
 * | 截断不提示 | 收消息的人以为内容就这么多 |
 * | 润色失败拦消息 | 一句本来能发的话发不出去（fail-open 才是对的，与审批相反） |
 * | 润色接受“扩写三倍” | 模型开始加原文没有的事实，而发言人是用户本人 |
 * | 空输出当成功 | 群里收到一条空消息 |
 */

import { describe, expect, test } from "bun:test"
import {
  CHANNEL_TEXT_LIMIT,
  outboundModeOf,
  polishOutbound,
  polishPrompt,
  shapeOutbound,
  signatureOf,
  type PolishModel,
} from "./persona-outbound-shape"

/** 可编排的润色模型替身。 */
function makeModel(reply: { text?: string; throw?: string; chunks?: string[] }): PolishModel {
  return {
    async *chat() {
      if (reply.throw) throw new Error(reply.throw)
      if (reply.chunks) {
        for (const c of reply.chunks) yield { text: c }
        return
      }
      yield { text: reply.text ?? "" }
    },
  }
}

describe("整形：@全体必须剥掉", () => {
  test("中英文各种写法都剥（模型不该拥有点名全群的能力）", () => {
    for (const ats of ["@all", "@ALL", "@everyone", "@所有人", "@全体成员", "@全体"]) {
      const r = shapeOutbound({ text: `大家好 ${ats} 今晚停机`, channel: "feishu", mode: "shape" })
      expect(r.text).not.toMatch(/@(all|everyone|所有人|全体成员|全体)/i)
      expect(r.text).toContain("今晚停机")
      expect(r.changes.join()).toContain("@全体")
    }
  })

  test("剥掉后不留双空格与行尾空白", () => {
    const r = shapeOutbound({ text: "大家好  @all  今晚停机  \n", channel: "feishu", mode: "shape" })
    expect(r.text).toBe("大家好 今晚停机")
  })

  test("连续调用不漏判（带 g 的正则有状态，lastIndex 不清零会漏掉下一条）", () => {
    const a = shapeOutbound({ text: "a @all b", channel: "feishu", mode: "shape" })
    const b = shapeOutbound({ text: "c @all d", channel: "feishu", mode: "shape" })
    expect(a.text).toBe("a b")
    expect(b.text).toBe("c d")
  })

  test("普通 @某人 不动（那是正常的业务提及）", () => {
    const r = shapeOutbound({ text: "@张三 看下这个", channel: "feishu", mode: "shape" })
    expect(r.text).toBe("@张三 看下这个")
    expect(r.changes).toEqual([])
  })
})

describe("整形：署名与截断", () => {
  test("署名只加一次（重试不叠两行）", () => {
    const once = shapeOutbound({ text: "结论如下", channel: "feishu", mode: "shape", signature: "——小霜" })
    expect(once.text).toBe("结论如下\n\n——小霜")
    const twice = shapeOutbound({ text: once.text, channel: "feishu", mode: "shape", signature: "——小霜" })
    expect(twice.text).toBe("结论如下\n\n——小霜")
    expect(twice.changes).toEqual([])
  })

  test("超长按渠道上限截断，且**明确提示**有内容被截", () => {
    const long = "字".repeat(CHANNEL_TEXT_LIMIT.feishu! + 500)
    const r = shapeOutbound({ text: long, channel: "feishu", mode: "shape" })
    expect(r.truncated).toBe(true)
    expect(r.text.length).toBeLessThanOrEqual(CHANNEL_TEXT_LIMIT.feishu!)
    expect(r.text).toContain("已截断")
    expect(r.text).toContain(`共 ${long.length} 字`)
  })

  test("未配置上限的渠道不截断（不知道就不要猜）", () => {
    const long = "x".repeat(9999)
    const r = shapeOutbound({ text: long, channel: "jira", mode: "shape" })
    expect(r.truncated).toBe(false)
    expect(r.text).toHaveLength(9999)
  })
})

describe("整形：raw 模式一字不改", () => {
  test("raw 不删 @all、不署名、不截断（部署方显式选了“不要动我的话”）", () => {
    const text = `@所有人 ${"字".repeat(CHANNEL_TEXT_LIMIT.feishu! + 10)}`
    const r = shapeOutbound({ text, channel: "feishu", mode: "raw", signature: "——小霜" })
    expect(r.text).toBe(text)
    expect(r.changes).toEqual([])
    expect(r.truncated).toBe(false)
  })
})

describe("润色：失败一律回落原文（fail-open）", () => {
  const LONG = "这条回复的原文。"

  test("正常润色 → 用模型输出", async () => {
    const r = await polishOutbound({ text: LONG, model: makeModel({ text: "润色后的回复。" }) })
    expect(r.ok).toBe(true)
    expect(r.text).toBe("润色后的回复。")
  })

  test("模型抛错 → 回落原文并给出原因", async () => {
    const r = await polishOutbound({ text: LONG, model: makeModel({ throw: "模型 500" }) })
    expect(r.ok).toBe(false)
    expect(r.text).toBe(LONG)
    expect(r.reason).toContain("500")
  })

  test("空输出 → 回落原文（群里不能收到一条空消息）", async () => {
    const r = await polishOutbound({ text: LONG, model: makeModel({ text: "   " }) })
    expect(r.ok).toBe(false)
    expect(r.text).toBe(LONG)
  })

  test("疑似扩写（>3 倍）→ 回落原文（不许加原文没有的事实）", async () => {
    const r = await polishOutbound({ text: LONG, model: makeModel({ text: "很长".repeat(100) }) })
    expect(r.ok).toBe(false)
    expect(r.text).toBe(LONG)
    expect(r.reason).toContain("扩写")
  })

  test("流式分片拼接", async () => {
    const r = await polishOutbound({ text: LONG, model: makeModel({ chunks: ["润色", "后的", "回复。"] }) })
    expect(r.text).toBe("润色后的回复。")
  })
})

describe("润色提示词：约束必须写死（不许增删事实）", () => {
  test("含六条硬约束与“原样返回”兜底", () => {
    const msgs = polishPrompt("原文", { personaName: "小霜", chatLabel: "支持群" })
    const sys = msgs[0]!.content
    expect(sys).toContain("只改措辞")
    expect(sys).toContain("不许增加原文没有的事实")
    expect(sys).toContain("不许删除")
    expect(sys).toContain("保持长度量级")
    expect(sys).toContain("不要加开头问候语")
    expect(sys).toContain("原样返回")
    expect(sys).toContain("小霜")
    expect(sys).toContain("支持群")
    expect(msgs[1]!.content).toBe("原文")
  })
})

describe("配置面", () => {
  test("档位：缺省 raw（零意外），只认 shape/polish", () => {
    expect(outboundModeOf("xiaoshuang", {})).toBe("raw")
    expect(outboundModeOf("xiaoshuang", { GEBAI_PERSONA_XIAOSHUANG_OUTBOUND: "shape" })).toBe("shape")
    expect(outboundModeOf("xiaoshuang", { GEBAI_PERSONA_XIAOSHUANG_OUTBOUND: "POLISH" })).toBe("polish")
    expect(outboundModeOf("xiaoshuang", { GEBAI_PERSONA_XIAOSHUANG_OUTBOUND: "胡说" })).toBe("raw")
  })

  test("署名：缺省空（不署名）", () => {
    expect(signatureOf("p", {})).toBe("")
    expect(signatureOf("p", { GEBAI_PERSONA_P_SIGNATURE: " ——小霜 " })).toBe("——小霜")
  })
})
