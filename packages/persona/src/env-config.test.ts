/**
 * 身份模板 + 部署配置面的反向验证。
 *
 * 这一组都是「配错了不报错、现象像玄学」的形态：
 *
 * | 变异 | 现象 |
 * |---|---|
 * | 模板变量未声明就能过 | 身份渲染成 `${FOO}` 或空串，模型看见“你是，工号”——日志全绿 |
 * | 未配变量渲染成空串 | 提示词悄悄缺一段，比原样保留更难发现 |
 * | 输出目标解析失败静默丢弃 | 人格以为能发，发出去被拒，而部署方不知道为什么 |
 * | 默认目标不标记 | 模型每轮自己挑一个，行为漂移 |
 * | 收窄用字符串枚举而不是匹配函数 | 订阅语法演进后漏一类，群收窄静默失效 |
 */

import { describe, expect, test } from "bun:test"
import { ACTOR_FIELDS_JSON_KEY, actorFieldsFromEnv, renderTemplate, resolveActorFields, templateVars, validateTemplate } from "./identity"
import {
  applyPersonaEnv,
  chatSessionKeyOf,
  narrowSubscriptions,
  parseOutputTargets,
  personaApprovalMode,
  sessionRoutingMode,
} from "./env-config"
import { validatePersonaDef } from "./persona"
import type { PersonaDef } from "./persona"

describe("身份模板：解析", () => {
  test("取变量名（去重、保序）", () => {
    expect(templateVars("我是 ${A}，工号 ${B}，也叫 ${A}")).toEqual(["A", "B"])
    expect(templateVars("没有变量")).toEqual([])
  })

  test("替换已提供的变量，未提供的**原样保留**（不静默变空串）", () => {
    const env = (n: string) => ({ NAME: "张三" })[n]
    expect(renderTemplate("我是${NAME}，工号${STAFF}", env)).toBe("我是张三，工号${STAFF}")
  })

  test("空值不替换（空串与未配置同义，都保留占位符）", () => {
    expect(renderTemplate("${NAME}", () => "")).toBe("${NAME}")
  })
})

describe("身份模板：加载期校验（漏声明必须报错）", () => {
  test("引用了未声明的变量 → 报错并指名道姓", () => {
    const err = validateTemplate("我是 ${ACTOR_NAME}", new Set(["OTHER"]), "name")
    expect(err).toContain("${ACTOR_NAME}")
    expect(err).toContain("envVars")
  })

  test("声明的变量 → 通过", () => {
    expect(validateTemplate("我是 ${ACTOR_NAME}", new Set(["ACTOR_NAME"]), "name")).toBeNull()
  })

  test("人格定义里：identity 用了未声明的变量 → 加载失败（不静默）", () => {
    const bad = {
      id: "p1",
      name: "测试",
      identity: "你是 ${ACTOR_NAME}",
      role: "r",
    }
    expect(validatePersonaDef(bad, "p1")).toContain("${ACTOR_NAME}")
  })

  test("声明了就能过（自由键值：键由部署方定）", () => {
    const ok = {
      id: "p1",
      name: "测试",
      identity: "你是 ${ACTOR_NAME}",
      role: "r",
      envVars: [{ name: "ACTOR_NAME", description: "扮演者姓名" }],
      actor: { fields: { 姓名: "${ACTOR_NAME}", 工号: "${ACTOR_STAFF}" } },
    }
    expect(validatePersonaDef(ok, "p1")).toContain("${ACTOR_STAFF}") // 工号引用未声明
    ok.envVars.push({ name: "ACTOR_STAFF", description: "工号" })
    expect(validatePersonaDef(ok, "p1")).toBeNull()
  })

  test("任意身份键都合法（部门/邮箱/花名/工位…）——不再是 name/staffId/account 三件套", () => {
    const ok = {
      id: "p1",
      name: "n",
      identity: "i",
      role: "r",
      envVars: [{ name: "ACTOR", description: "整组身份" }],
      actor: { fields: { 姓名: "张三", 花名: "小张", 部门: "基础架构", 邮箱: "z@corp", 工位: "B3-12" } },
    }
    expect(validatePersonaDef(ok, "p1")).toBeNull()
  })

  test("身份字段的模板逐项校验（含整组注入的 `${VAR}`）", () => {
    const bad = {
      id: "p1",
      name: "n",
      identity: "i",
      role: "r",
      actor: { fields: { 部门: "${DEPT}" } },
    }
    expect(validatePersonaDef(bad, "p1")).toContain("${DEPT}")
  })

  test("actor 里除 fields 外的旧字段被拒（说清该怎么写）", () => {
    const bad = { id: "p1", name: "n", identity: "i", role: "r", actor: { name: "张三" } }
    const err = validatePersonaDef(bad, "p1")!
    expect(err).toContain("fields")
    expect(err).toContain("自由键值")
  })

  test("actor.fields 非对象/值为非字符串 → 报错", () => {
    expect(validatePersonaDef({ id: "p1", name: "n", identity: "i", role: "r", actor: { fields: "x" } }, "p1")).toContain("必须是对象")
    expect(validatePersonaDef({ id: "p1", name: "n", identity: "i", role: "r", actor: { fields: { a: 1 } } }, "p1")).toContain(
      "必须是字符串",
    )
  })

  test("user 字段支持模板并校验", () => {
    const bad = { id: "p1", name: "n", identity: "i", role: "r", user: "${OWNER}" }
    expect(validatePersonaDef(bad, "p1")).toContain("${OWNER}")
  })
})

describe("身份字段：自由键值解析", () => {
  const envOf = (vars: Record<string, string>) => (n: string) => vars[n]

  test("逐字段模板：键与值都由部署方定", () => {
    const { fields, problems } = resolveActorFields(
      { 姓名: "${NAME}", 部门: "${DEPT}", 邮箱: "${NAME}@corp.com" },
      envOf({ NAME: "张三", DEPT: "基础架构" }),
    )
    expect(problems).toEqual([])
    expect(fields).toEqual({ 姓名: "张三", 部门: "基础架构", 邮箱: "张三@corp.com" })
  })

  test("整组注入：一个 JSON 变量装下所有身份字段", () => {
    const { fields, problems } = resolveActorFields(
      { [ACTOR_FIELDS_JSON_KEY]: "${GEBAI_ACTOR}" },
      envOf({ GEBAI_ACTOR: JSON.stringify({ 姓名: "李四", 工号: "20022", 花名: "小李" }) }),
    )
    expect(problems).toEqual([])
    expect(fields).toEqual({ 姓名: "李四", 工号: "20022", 花名: "小李" })
  })

  test("整组变量未配置 → 安静跳过（部署方还没填，不是错误）", () => {
    const { fields, problems } = resolveActorFields({ [ACTOR_FIELDS_JSON_KEY]: "${GEBAI_ACTOR}" }, envOf({}))
    expect(fields).toEqual({})
    expect(problems).toEqual([])
  })

  test("整组变量不是 JSON / 不是对象 → 如实上报问题（不静默）", () => {
    const bad = resolveActorFields({ [ACTOR_FIELDS_JSON_KEY]: "${GEBAI_ACTOR}" }, envOf({ GEBAI_ACTOR: "{坏的" }))
    expect(bad.problems[0]).toContain("不是合法 JSON")
    const arr = resolveActorFields({ [ACTOR_FIELDS_JSON_KEY]: "${GEBAI_ACTOR}" }, envOf({ GEBAI_ACTOR: "[1,2]" }))
    expect(arr.problems[0]).toContain("必须是 JSON 对象")
  })

  test("整组注入的嵌套对象被拒（保持扁平，方便模型读）", () => {
    const { problems } = resolveActorFields(
      { [ACTOR_FIELDS_JSON_KEY]: "${GEBAI_ACTOR}" },
      envOf({ GEBAI_ACTOR: JSON.stringify({ 部门: { 一级: "基础架构" } }) }),
    )
    expect(problems[0]).toContain("不支持嵌套对象")
  })

  test("未配置的变量：**跳过该字段**并记问题（不能把 ${VAR} 发到群里）", () => {
    const { fields, problems } = resolveActorFields({ 工号: "${STAFF}", 姓名: "张三" }, envOf({}))
    expect(fields).toEqual({ 姓名: "张三" })
    expect(fields).not.toHaveProperty("工号")
    expect(problems[0]).toContain("变量未配置")
  })

  test("部分渲染成功的字段保留（如邮箱拼接）", () => {
    const { fields } = resolveActorFields({ 邮箱: "${NAME}@corp.com" }, envOf({ NAME: "张三" }))
    expect(fields.邮箱).toBe("张三@corp.com")
  })

  test("空值/空白值不输出（不产生“部门：”这种半截行）", () => {
    const { fields } = resolveActorFields({ 部门: "  " }, envOf({}))
    expect(fields).toEqual({})
  })

  test("整组注入与逐字段可混用（逐字段优先铺在后面）", () => {
    const { fields, problems } = resolveActorFields(
      { [ACTOR_FIELDS_JSON_KEY]: "${GEBAI_ACTOR}", 邮箱: "override@corp.com" },
      envOf({ GEBAI_ACTOR: JSON.stringify({ 姓名: "王五", 邮箱: "old@corp.com" }) }),
    )
    expect(problems).toEqual([])
    expect(fields).toEqual({ 姓名: "王五", 邮箱: "override@corp.com" })
  })

  test("undefined 入参 → 空结果（无人格身份字段时）", () => {
    expect(resolveActorFields(undefined, envOf({}))).toEqual({ fields: {}, problems: [] })
  })

  test("actorFieldsFromEnv：产出整组注入的哨兵键（空变量名 → 空对象）", () => {
    expect(actorFieldsFromEnv("GEBAI_ACTOR")).toEqual({ [ACTOR_FIELDS_JSON_KEY]: "${GEBAI_ACTOR}" })
    expect(actorFieldsFromEnv("   ")).toEqual({})
    // 端到端：定义里写 actorFieldsFromEnv，加载期能看到 ${VAR}（白名单校验能拦住漏声明）
    const err = validatePersonaDef(
      { id: "p1", name: "n", identity: "i", role: "r", actor: { fields: actorFieldsFromEnv("GEBAI_ACTOR") } },
      "p1",
    )
    expect(err).toContain("${GEBAI_ACTOR}")
  })
})

describe("输出目标解析", () => {
  test("基本形态 + 首条默认", () => {
    const { targets, errors } = parseOutputTargets("feishu:oc_a,feishu:oc_b")
    expect(errors).toEqual([])
    expect(targets).toEqual([
      { channel: "feishu", address: "oc_a", default: true },
      { channel: "feishu", address: "oc_b" },
    ])
  })

  test("话题与标签", () => {
    const { targets } = parseOutputTargets("feishu:oc_a#om_root=支持群")
    expect(targets[0]).toEqual({ channel: "feishu", address: "oc_a", thread: "om_root", label: "支持群", default: true })
  })

  test("显式 * 指定默认目标（不是第一条）", () => {
    const { targets } = parseOutputTargets("feishu:oc_a,*feishu:oc_b")
    expect(targets.find((t) => t.default)?.address).toBe("oc_b")
  })

  test("格式非法的条目进 errors（不静默丢弃）", () => {
    const { targets, errors } = parseOutputTargets("no-colon,feishu:")
    expect(targets).toEqual([])
    expect(errors).toHaveLength(2)
  })

  test("空配置 → 无目标、无错误", () => {
    expect(parseOutputTargets(undefined)).toEqual({ targets: [], errors: [] })
    expect(parseOutputTargets("  ")).toEqual({ targets: [], errors: [] })
  })
})

describe("入站收窄：用匹配函数而不是字符串枚举", () => {
  test("能匹配 feishu 消息的四类订阅都被替换为逐群三段式", () => {
    expect(narrowSubscriptions(["*", "message", "feishu", "feishu:message"], ["oc_x"])).toEqual(["feishu:oc_x:message"])
  })

  test("不碰 feishu 消息的订阅原样保留（定时/webhook 照常）", () => {
    expect(narrowSubscriptions(["schedule", "webhook:alert"], ["oc_x"])).toEqual([
      "schedule",
      "webhook:alert",
      "feishu:oc_x:message",
    ])
  })

  test("已经是该群的订阅不重复添加", () => {
    expect(narrowSubscriptions(["feishu:oc_x:message"], ["oc_x"])).toEqual(["feishu:oc_x:message"])
  })
})

describe("applyPersonaEnv：三个配置面 + 分身级覆盖", () => {
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

  test("启用白名单：名单外的人格不进结果（且记录）", () => {
    const { personas, report } = applyPersonaEnv(map, { GEBAI_PERSONA_ENABLE: "other" })
    expect(personas.size).toBe(0)
    expect(report.disabled).toEqual(["xiaoshuang"])
  })

  test("人格级 CHATS 收窄所有分身的 feishu 消息订阅", () => {
    const { personas, report } = applyPersonaEnv(map, { GEBAI_PERSONA_XIAOSHUANG_CHATS: "oc_a,oc_b" })
    const d = personas.get("xiaoshuang")!
    expect(d.drivers![0]!.subscriptions).toEqual(["feishu:oc_a:message", "feishu:oc_b:message"])
    // alerts 只订 webhook：收窄后原有订阅保留，又多了逐群消息订阅（CHATS 的语义就是“只看这些群”）
    expect(d.drivers![1]!.subscriptions).toContain("webhook")
    expect(report.narrowed.xiaoshuang).toEqual(["oc_a", "oc_b"])
  })

  test("分身级 CHATS 优先于人格级（更具体的声明胜出）", () => {
    const { personas, report } = applyPersonaEnv(map, {
      GEBAI_PERSONA_XIAOSHUANG_CHATS: "oc_persona",
      GEBAI_PERSONA_XIAOSHUANG_SUPPORT_CHATS: "oc_driver",
    })
    const d = personas.get("xiaoshuang")!
    expect(d.drivers![0]!.subscriptions).toEqual(["feishu:oc_driver:message"])
    expect(d.drivers![1]!.subscriptions).toContain("feishu:oc_persona:message")
    expect(report.narrowed["xiaoshuang/support"]).toEqual(["oc_driver"])
  })

  test("输出目标注入到 driver（人格级作为默认值，分身级覆盖）", () => {
    const { personas } = applyPersonaEnv(map, {
      GEBAI_PERSONA_XIAOSHUANG_OUTPUT: "feishu:oc_persona",
      GEBAI_PERSONA_XIAOSHUANG_ALERTS_OUTPUT: "feishu:oc_ops=运维群",
    })
    const d = personas.get("xiaoshuang")!
    expect(d.drivers![0]!.outputs?.[0]?.address).toBe("oc_persona")
    expect(d.drivers![1]!.outputs?.[0]?.address).toBe("oc_ops")
    expect(d.drivers![1]!.outputs?.[0]?.label).toBe("运维群")
  })

  test("输出目标格式错 → 进 report.errors（不静默丢）", () => {
    const { report } = applyPersonaEnv(map, { GEBAI_PERSONA_XIAOSHUANG_OUTPUT: "bad" })
    expect(report.errors.some((e) => e.includes("格式非法"))).toBe(true)
  })

  test("未配置任何变量 → 定义原样返回（零配置不改行为）", () => {
    const { personas, report } = applyPersonaEnv(map, {})
    expect(personas.get("xiaoshuang")).toEqual(DEF)
    expect(report.disabled).toEqual([])
    expect(report.errors).toEqual([])
  })
})

describe("会话路由：缺省面向上下文，不面向群", () => {
  test("chatSessionKeyOf（仅群间隔离模式用）：按群分；话题群再加话题维", () => {
    expect(chatSessionKeyOf({ kind: "message", channel: "feishu", context: { refs: { chatId: "oc_a" } } })).toBe("oc_a")
    expect(
      chatSessionKeyOf({ kind: "message", channel: "feishu", context: { refs: { chatId: "oc_a", threadId: "omt_1" } } }),
    ).toBe("oc_a__t_omt_1")
  })

  test("chatSessionKeyOf：定时/自省落 anchor；无 chatId 返回空串", () => {
    expect(chatSessionKeyOf({ kind: "schedule", channel: "schedule", context: {} })).toBe("anchor")
    expect(chatSessionKeyOf({ kind: "webhook", channel: "hook", context: {} })).toBe("")
  })

  test("模式解析：**缺省 driver**（一个分身一个上下文），显式 chat 才按群分", () => {
    expect(sessionRoutingMode({})).toBe("driver")
    expect(sessionRoutingMode({ GEBAI_PERSONA_SESSION_ROUTING: "chat" })).toBe("chat")
    expect(sessionRoutingMode({ GEBAI_PERSONA_SESSION_ROUTING: "CHAT" })).toBe("chat")
    // 无法识别的值回到缺省（不同群共享上下文），而不是悄悄改成分群
    expect(sessionRoutingMode({ GEBAI_PERSONA_SESSION_ROUTING: "胡说" })).toBe("driver")
  })
})

describe("审批姿态解析", () => {
  test("缺省 ask（不能决策的事要人介入）", () => {
    expect(personaApprovalMode({})).toBe("ask")
  })

  test("auto / deny 及其同义词", () => {
    expect(personaApprovalMode({ GEBAI_PERSONA_APPROVAL: "auto" })).toBe("auto")
    expect(personaApprovalMode({ GEBAI_PERSONA_APPROVAL: "off" })).toBe("auto")
    expect(personaApprovalMode({ GEBAI_PERSONA_APPROVAL: "deny" })).toBe("deny")
    expect(personaApprovalMode({ GEBAI_PERSONA_APPROVAL: "false" })).toBe("deny")
  })

  test("无法识别的值 → 回到最安全的 ask（不是 auto）", () => {
    expect(personaApprovalMode({ GEBAI_PERSONA_APPROVAL: "也许吧" })).toBe("ask")
  })
})
