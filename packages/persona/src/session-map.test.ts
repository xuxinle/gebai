/**
 * 会话映射规则表的反向验证。
 *
 * 这一组的核心是「配置写错时会发生什么」。会话映射的失败形态极其隐蔽——
 * 它不会报错，只会让**上下文悄悄分裂或多路串味**：
 *
 * | 变异 | 现象 |
 * |---|---|
 * | 占位符名写错（`{thred}`）静默忽略 | 规则永不命中，人以为“配了但没生效” |
 * | `onMissing` 缺省用 drop | `oc_x__t_` 这类残键看起来像正常会话，上下文分裂没人察觉 |
 * | 话题群/普通群混在同一规则 | 普通群消息全落进一个空键，全部串味 |
 * | 规则顺序被忽略（后面的规则先命中） | 兜底 `*=` 吃掉一切，切分彻底失效 |
 * | 空键与“没命中”混为一谈 | 写了兜底却仍走部署默认，行为对不上配置 |
 */

import { describe, expect, test } from "bun:test"
import {
  KNOWN_PLACEHOLDERS,
  SESSION_MAP_PRESETS,
  isKnownPlaceholder,
  matchSessionMap,
  parseSessionMap,
  placeholdersOf,
  renderSessionKey,
  validateSessionMap,
  type SessionMapContext,
} from "./session-map"
import { applyPersonaEnv } from "./env-config"
import { validatePersonaDef } from "./persona"
import type { PersonaDef } from "./persona"

const ctx = (over: Partial<SessionMapContext> = {}): SessionMapContext => ({
  channel: "feishu",
  kind: "message",
  refs: { chatId: "oc_a", messageId: "om_1", senderId: "ou_zhang", senderName: "张三" },
  ...over,
})

describe("占位符：取值与合法性", () => {
  test("基本占位符", () => {
    expect(renderSessionKey({ key: "{channel}" }, ctx()).key).toBe("feishu")
    expect(renderSessionKey({ key: "{kind}" }, ctx()).key).toBe("message")
    expect(renderSessionKey({ key: "{chat}" }, ctx()).key).toBe("oc_a")
    expect(renderSessionKey({ key: "{senderName}" }, ctx()).key).toBe("张三")
  })

  test("extra.* 取源自定义字段（工单号/仓库名由源自己塞）", () => {
    const c = ctx({ refs: { chatId: "oc_a", extra: { ticket: "T-1", repo: "gebai" } } })
    expect(renderSessionKey({ key: "ticket:{extra.ticket}" }, c).key).toBe("ticket:T-1")
    expect(renderSessionKey({ key: "{extra.repo}" }, c).key).toBe("gebai")
  })

  test("未知占位符判定：基本集 + extra.* 才算已知（extra 键名不限 ASCII）", () => {
    for (const p of KNOWN_PLACEHOLDERS) expect(isKnownPlaceholder(p)).toBe(true)
    expect(isKnownPlaceholder("extra.任意-键")).toBe(true)
    expect(isKnownPlaceholder("extra.工单")).toBe(true) // 中文键是真实写法，不能逼人改拼音
    expect(isKnownPlaceholder("thred")).toBe(false) // 典型拼错
    expect(isKnownPlaceholder("extra")).toBe(false) // 少了 .键
    expect(isKnownPlaceholder("extra.")).toBe(false) // 空键
    expect(isKnownPlaceholder("extra.a b")).toBe(false) // 带空白会破坏模板
  })

  test("占位符提取（去重保序）", () => {
    expect(placeholdersOf("{chat}__t_{thread}_{chat}")).toEqual(["chat", "thread"])
  })
})

describe("onMissing：缺省 skip（不产生残键）", () => {
  test("缺省策略是 skip：话题缺失 → 本规则不命中（而不是产出 oc_a__t_）", () => {
    const r = renderSessionKey({ key: "{chat}__t_{thread}" }, ctx())
    expect(r.matched).toBe(false)
    expect(r.missing).toEqual(["thread"])
  })

  test("drop：删占位符并折叠**连续**分隔符（不猜标记段）", () => {
    const r = renderSessionKey({ key: "{chat}--{thread}", onMissing: "drop" }, ctx())
    expect(r.matched).toBe(true)
    expect(r.key).toBe("oc_a")
  })

  test("drop 保留 chat id 本身（不能把 oc_a 的 _a 当标记删掉）", () => {
    // 这条是反向验证出来的真缺陷：“分隔符+短字母=标记就删”的启发式会得到 `oc`
    const r = renderSessionKey({ key: "{chat}__t_{thread}", onMissing: "drop" }, ctx())
    expect(r.key).not.toBe("oc")
    expect(r.key!.startsWith("oc_a")).toBe(true)
  })

  test("带形态标记的键：用缺省 skip 才拿得到干净键（per_thread 预设就是这么写的）", () => {
    const skip = renderSessionKey({ key: "{chat}__t_{thread}" }, ctx())
    expect(skip.matched).toBe(false) // 不命中 → 交给下一条 `{chat}`
    expect(matchSessionMap(SESSION_MAP_PRESETS.per_thread!, ctx())).toBe("oc_a")
  })

  test("drop 把整条键都删光时 → 不命中（不冒充“常驻上下文”）", () => {
    const r = renderSessionKey({ key: "{thread}", onMissing: "drop" }, ctx())
    expect(r.matched).toBe(false)
  })

  test("literal：保留占位符原样（排查用）", () => {
    const r = renderSessionKey({ key: "{chat}__t_{thread}", onMissing: "literal" }, ctx())
    expect(r.matched).toBe(true)
    expect(r.key).toBe("oc_a__t_{thread}")
    expect(r.missing).toEqual(["thread"])
  })

  test("空串与缺失同义（空白值不算有值）", () => {
    const c = ctx({ refs: { chatId: "oc_a", threadId: "  " } })
    expect(renderSessionKey({ key: "{chat}__t_{thread}" }, c).matched).toBe(false)
  })
})

describe("规则表：顺序、命中与兜底", () => {
  test("按顺序第一条命中即用（兜底 `*` 不会抢在前面的规则之前）", () => {
    const rules = SESSION_MAP_PRESETS.per_thread!
    // 话题消息 → 第一条（per-thread）
    expect(matchSessionMap(rules, ctx({ refs: { chatId: "oc_a", threadId: "omt_9" } }))).toBe("oc_a__t_omt_9")
    // 普通群消息（无话题）→ 第一条不命中，落到第二条（per-chat）
    expect(matchSessionMap(rules, ctx())).toBe("oc_a")
  })

  test("`key: \"\"` = 明确要求常驻上下文，与“没命中”区分开", () => {
    expect(matchSessionMap([{ when: "*", key: "" }], ctx())).toBe("")
    expect(matchSessionMap([{ when: "webhook", key: "" }], ctx())).toBeUndefined() // 未命中 → undefined
  })

  test("when 用订阅语法：channel / kind / 群 id 都能限", () => {
    const rules = [{ when: "feishu", key: "feishu:{chat}" }, { when: "*", key: "other" }]
    expect(matchSessionMap(rules, ctx())).toBe("feishu:oc_a")
    expect(matchSessionMap(rules, ctx({ channel: "schedule", kind: "schedule" }))).toBe("other")
    // 三段式：只对某个群生效
    const perChat = [{ when: "feishu:oc_special:message", key: "special" }, { when: "*", key: "normal" }]
    expect(matchSessionMap(perChat, ctx({ refs: { chatId: "oc_special" } }))).toBe("special")
    expect(matchSessionMap(perChat, ctx({ refs: { chatId: "oc_other" } }))).toBe("normal")
  })

  test("规则全不命中 → undefined（调用方回落默认）", () => {
    expect(matchSessionMap([{ when: "webhook", key: "w" }], ctx())).toBeUndefined()
  })

  test("空/未声明规则表 → undefined（零配置不改行为）", () => {
    expect(matchSessionMap(undefined, ctx())).toBeUndefined()
    expect(matchSessionMap([], ctx())).toBeUndefined()
  })
})

describe("预设：覆盖真实部署形态", () => {
  test("per_thread：话题内共享、话题间隔离、普通群退回按群", () => {
    const rules = SESSION_MAP_PRESETS.per_thread!
    const topic1 = matchSessionMap(rules, ctx({ refs: { chatId: "oc_a", threadId: "omt_1" } }))
    const topic1b = matchSessionMap(rules, ctx({ refs: { chatId: "oc_a", threadId: "omt_1", messageId: "om_2" } }))
    const topic2 = matchSessionMap(rules, ctx({ refs: { chatId: "oc_a", threadId: "omt_2" } }))
    expect(topic1).toBe(topic1b) // 同话题：同一上下文（追问接得上）
    expect(topic1).not.toBe(topic2) // 不同话题：隔离
    expect(matchSessionMap(rules, ctx())).toBe("oc_a") // 普通群
  })

  test("single / per_chat / per_sender", () => {
    expect(matchSessionMap(SESSION_MAP_PRESETS.single!, ctx())).toBe("")
    expect(matchSessionMap(SESSION_MAP_PRESETS.per_chat!, ctx())).toBe("oc_a")
    expect(matchSessionMap(SESSION_MAP_PRESETS.per_sender!, ctx())).toBe("sender:ou_zhang")
  })
})

describe("parseSessionMap：三种写法", () => {
  test("预设名（大小写不敏感）", () => {
    expect(parseSessionMap("per_thread").rules).toEqual(SESSION_MAP_PRESETS.per_thread!)
    expect(parseSessionMap("PER_THREAD").rules).toEqual(SESSION_MAP_PRESETS.per_thread!)
  })

  test("紧凑 DSL：when=key;…，省略 when 即 *", () => {
    const { rules, errors } = parseSessionMap("feishu:message={chat}__t_{thread};message={chat};*=")
    expect(errors).toEqual([])
    expect(rules).toEqual([
      { when: "feishu:message", key: "{chat}__t_{thread}" },
      { when: "message", key: "{chat}" },
      { when: "*", key: "" },
    ])
  })

  test("JSON 写法（带 onMissing）", () => {
    const { rules, errors } = parseSessionMap('[{"when":"message","key":"{chat}-{thread}","onMissing":"drop"}]')
    expect(errors).toEqual([])
    expect(rules[0]).toEqual({ when: "message", key: "{chat}-{thread}", onMissing: "drop" })
  })

  test("空值 → 无规则（不报错）", () => {
    expect(parseSessionMap(undefined)).toEqual({ rules: [], errors: [] })
    expect(parseSessionMap("  ")).toEqual({ rules: [], errors: [] })
  })

  test("写法错 → 如实报错（不静默变成“无映射”）", () => {
    expect(parseSessionMap("no-equal-sign").errors[0]).toContain("缺少 =")
    expect(parseSessionMap("[坏的").errors[0]).toContain("不是合法 JSON")
    expect(parseSessionMap("message={thred}").errors[0]).toContain("未知占位符")
  })

  test("占位符写错时错误文案列出可用占位符（可行动）", () => {
    const err = parseSessionMap("message={chat}__t_{thred}").errors[0]!
    expect(err).toContain("{thread}")
    expect(err).toContain("{extra.名字}")
  })
})

describe("validateSessionMap：人格定义加载期校验", () => {
  test("合法规则表", () => {
    expect(validateSessionMap([{ when: "message", key: "{chat}" }])).toBeNull()
    expect(validateSessionMap([{ key: "" }])).toBeNull()
  })

  test("非数组 / 空数组 / 项非对象 → 报错", () => {
    expect(validateSessionMap("x")).toContain("必须是数组")
    expect(validateSessionMap([])).toContain("不能为空数组")
    expect(validateSessionMap([1])).toContain("必须是对象")
  })

  test("未知字段 / key 非字符串 / 未知 onMissing → 报错", () => {
    expect(validateSessionMap([{ key: "{chat}", subcription: "x" }])).toContain("subcription")
    expect(validateSessionMap([{ key: 1 }])).toContain("key 必须是字符串")
    expect(validateSessionMap([{ key: "{chat}", onMissing: "maybe" }])).toContain("skip / drop / literal")
  })

  test("人格定义里 sessionMap 写错 → 加载失败（分身名指得出来）", () => {
    const bad = {
      id: "p1",
      name: "n",
      identity: "i",
      role: "r",
      drivers: [{ id: "support", focus: "f", subscriptions: ["message"], sessionMap: [{ key: "{thred}" }] }],
    }
    const err = validatePersonaDef(bad, "p1")!
    expect(err).toContain("support")
    expect(err).toContain("{thred}")
  })

  test("人格定义里 sessionMap 合法 → 通过", () => {
    const ok = {
      id: "p1",
      name: "n",
      identity: "i",
      role: "r",
      drivers: [
        { id: "support", focus: "f", subscriptions: ["message"], sessionMap: SESSION_MAP_PRESETS.per_thread },
      ],
    }
    expect(validatePersonaDef(ok, "p1")).toBeNull()
  })
})

describe("部署配置：GEBAI_PERSONA_<ID>[_<DRIVER>]_SESSION_MAP", () => {
  const DEF: PersonaDef = {
    id: "xiaoshuang",
    name: "小霜",
    identity: "你是小霜",
    role: "运维",
    drivers: [
      { id: "support", focus: "答疑", subscriptions: ["message"] },
      { id: "alerts", focus: "盯告警", subscriptions: ["webhook"] },
    ],
  }
  const map = new Map([[DEF.id, DEF]])

  test("人格级生效到所有分身", () => {
    const { personas, report } = applyPersonaEnv(map, { GEBAI_PERSONA_XIAOSHUANG_SESSION_MAP: "per_thread" })
    const d = personas.get("xiaoshuang")!
    expect(d.drivers![0]!.sessionMap).toEqual(SESSION_MAP_PRESETS.per_thread!)
    expect(d.drivers![1]!.sessionMap).toEqual(SESSION_MAP_PRESETS.per_thread!)
    expect(report.sessionMaps["xiaoshuang/support"]).toBe(2)
  })

  test("分身级覆盖人格级（更具体的声明胜出）", () => {
    const { personas } = applyPersonaEnv(map, {
      GEBAI_PERSONA_XIAOSHUANG_SESSION_MAP: "per_chat",
      GEBAI_PERSONA_XIAOSHUANG_SUPPORT_SESSION_MAP: "per_thread",
    })
    const d = personas.get("xiaoshuang")!
    expect(d.drivers![0]!.sessionMap).toEqual(SESSION_MAP_PRESETS.per_thread!)
    expect(d.drivers![1]!.sessionMap).toEqual(SESSION_MAP_PRESETS.per_chat!)
  })

  test("DSL 写法可用", () => {
    const { personas } = applyPersonaEnv(map, {
      GEBAI_PERSONA_XIAOSHUANG_SESSION_MAP: "feishu:message={chat}__t_{thread};message={chat}",
    })
    expect(personas.get("xiaoshuang")!.drivers![0]!.sessionMap).toHaveLength(2)
  })

  test("配置写错 → 进 report.errors（不静默），且不覆盖定义里的规则", () => {
    const withDef = new Map([
      ["xiaoshuang", { ...DEF, drivers: [{ ...DEF.drivers![0]!, sessionMap: SESSION_MAP_PRESETS.single }] }],
    ])
    const { personas, report } = applyPersonaEnv(withDef, { GEBAI_PERSONA_XIAOSHUANG_SESSION_MAP: "message={thred}" })
    expect(report.errors.some((e) => e.includes("未知占位符"))).toBe(true)
    expect(personas.get("xiaoshuang")!.drivers![0]!.sessionMap).toEqual(SESSION_MAP_PRESETS.single!)
  })

  test("未配置 → 不动定义（零配置不改行为）", () => {
    const { personas, report } = applyPersonaEnv(map, {})
    expect(personas.get("xiaoshuang")!.drivers![0]!.sessionMap).toBeUndefined()
    expect(report.sessionMaps).toEqual({})
  })
})
