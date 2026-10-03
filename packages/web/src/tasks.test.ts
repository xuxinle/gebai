import { describe, expect, test } from "bun:test"
import type { Task, TaskQueueView } from "@gebai/sdk"
import {
  canDequeue,
  canFront,
  canRun,
  canStop,
  emptyForm,
  envLines,
  filterTasks,
  formFromTask,
  formToCreateInput,
  formToUpdateInput,
  formatDuration,
  fileLine,
  idleBlockedText,
  lastResultLine,
  limitText,
  metaLine,
  notifyLines,
  notifySummary,
  parseEnvLines,
  parseNotifyLines,
  runLine,
  scheduleSummary,
  stateLabel,
  taskTitle,
  validateForm,
} from "./tasks-core"

/** 任务管理视图纯逻辑单测（无 DOM 依赖：渲染层只把这里的结果贴到元素上，见 tasks.ts 文件头）。 */

function mk(patch: Partial<Task> = {}): Task {
  return {
    id: "0123456789abcdef0123456789abcdef",
    user: "default",
    kind: "scheduled",
    runner: "prompt",
    name: "晨报",
    prompt: "写一份晨报",
    schedule: "0 9 * * *",
    enabled: true,
    createdAt: 1_700_000_000_000,
    updatedAt: 1_700_000_000_000,
    state: "idle",
    runCount: 3,
    ...patch,
  }
}

describe("列表展示逻辑", () => {
  test("taskTitle 名称优先，缺省用短 id", () => {
    expect(taskTitle(mk())).toBe("晨报")
    expect(taskTitle(mk({ name: "   " }))).toBe("任务 01234567")
    expect(taskTitle(mk({ name: undefined }))).toBe("任务 01234567")
  })

  test("按类别筛选（all 原样返回）", () => {
    const list = [mk({ id: "a".repeat(32) }), mk({ id: "b".repeat(32), kind: "manual" }), mk({ id: "c".repeat(32), kind: "idle" })]
    expect(filterTasks(list, "all")).toHaveLength(3)
    expect(filterTasks(list, "manual").map((t) => t.kind)).toEqual(["manual"])
    expect(filterTasks(list, "idle").map((t) => t.kind)).toEqual(["idle"])
  })

  test("状态文案：排队中附队列位置，位置未知不带括号", () => {
    const queue: TaskQueueView = {
      user: "default",
      limit: 5,
      running: [],
      entries: [{ taskId: "a".repeat(32), user: "default", kind: "manual", runner: "prompt", source: "manual", priority: 1000, enqueuedAt: 1, position: 2 }],
      busy: false,
    }
    expect(stateLabel(mk({ state: "idle" }), queue)).toBe("空闲")
    expect(stateLabel(mk({ state: "running" }), queue)).toBe("运行中")
    expect(stateLabel(mk({ id: "a".repeat(32), state: "queued" }), queue)).toBe("排队中（第 2 位）")
    expect(stateLabel(mk({ id: "b".repeat(32), state: "queued" }), queue)).toBe("排队中")
    expect(stateLabel(mk({ id: "b".repeat(32), state: "queued" }), null)).toBe("排队中")
  })

  test("行内操作可用性：停用不可执行、定时任务不可置顶、仅运行中可终止", () => {
    expect(canRun(mk())).toBe(true)
    expect(canRun(mk({ enabled: false }))).toBe(false)
    expect(canRun(mk({ state: "running" }))).toBe(false)
    expect(canFront(mk({ state: "queued", kind: "manual" }))).toBe(true)
    expect(canFront(mk({ state: "queued", kind: "scheduled" }))).toBe(false)
    expect(canFront(mk({ state: "running" }))).toBe(false)
    expect(canDequeue(mk({ state: "queued" }))).toBe(true)
    expect(canDequeue(mk({ state: "idle" }))).toBe(false)
    expect(canStop(mk({ state: "running" }))).toBe(true)
    expect(canStop(mk({ state: "queued" }))).toBe(false)
  })

  test("周期与通知摘要", () => {
    expect(scheduleSummary(mk({ timezone: "Asia/Shanghai" }))).toBe("0 9 * * *（Asia/Shanghai）")
    expect(scheduleSummary(mk({ kind: "manual", schedule: undefined }))).toBe("")
    expect(notifySummary(mk())).toBe("")
    expect(notifySummary(mk({ notify: [{ type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/x" }] }))).toBe("飞书")
    expect(notifySummary(mk({ notify: [{ type: "feishu_chat", target: "oc_abc", at: [{ id: "all" }] }, { type: "webhook", target: "https://e.com/h" }] }))).toBe("飞书群 / Webhook · 含 @ 提醒")
  })

  test("元信息与结果行", () => {
    const line = metaLine(mk({ nextRunAt: 1_700_000_060_000 }))
    expect(line).toContain("定时 · 提示词")
    expect(line).toContain("已运行 3 次")
    expect(metaLine(mk({ kind: "manual" }))).not.toContain("周期")
    expect(lastResultLine(mk())).toBe("尚未执行")
    expect(lastResultLine(mk({ lastStatus: "error", lastError: "exit 1" }))).toBe("失败：exit 1")
    expect(lastResultLine(mk({ lastStatus: "success", lastOutput: "done" }))).toBe("成功：done")
  })

  test("耗时与文件行", () => {
    expect(formatDuration(500)).toBe("500ms")
    expect(formatDuration(12_340)).toBe("12.3s")
    expect(formatDuration(95_000)).toBe("1m35s")
    expect(formatDuration(undefined)).toBe("-")
    expect(fileLine({ path: "run.sh", size: 12, mtimeMs: 0, dir: false })).toBe("run.sh · 12 字节")
    expect(fileLine({ path: "notes", size: 0, mtimeMs: 0, dir: true })).toBe("notes/")
  })

  test("队列视图文案（额度与闲时让路）", () => {
    const q: TaskQueueView = { user: "default", limit: 5, running: [{ taskId: "a", kind: "manual", runner: "prompt", startedAt: 1 }], entries: [], busy: true }
    expect(limitText(q)).toBe("1/5")
    expect(idleBlockedText(q)).toContain("让路")
    expect(idleBlockedText({ ...q, busy: false })).toContain("队列空闲")
  })

  test("运行历史行（手动标记 / 跳过原因）", () => {
    const line = runLine({ id: "r1", at: 1_700_000_000_000, endedAt: 1_700_000_010_000, status: "success", durationMs: 10_000, manual: true, output: "ok" })
    expect(line).toContain("成功")
    expect(line).toContain("手动")
    expect(line).toContain("ok")
    expect(runLine({ id: "r2", at: 1_700_000_000_000, endedAt: 0, status: "skipped", durationMs: 0, reason: "上次执行尚未结束，本轮跳过" })).toContain("上次执行尚未结束")
  })
})

describe("表单 → 请求体", () => {
  test("定时脚本任务：表达式/时区/补跑 + 数字字段解析", () => {
    const v = { ...emptyForm(), kind: "scheduled" as const, runner: "script" as const, name: " 备份 ", schedule: "@every 30m", timezone: " Asia/Shanghai ", misfire: "run" as const, script: " bash run.sh ", timeoutMs: "60000", maxConsecutiveErrors: "3", enabled: false }
    expect(validateForm(v)).toBeNull()
    expect(formToCreateInput(v)).toEqual({
      kind: "scheduled",
      runner: "script",
      enabled: false,
      name: "备份",
      schedule: "@every 30m",
      timezone: "Asia/Shanghai",
      misfire: "run",
      script: "bash run.sh",
      timeoutMs: 60_000,
      maxConsecutiveErrors: 3,
      notifyOn: "auto",
    })
  })

  test("普通提示词任务：预载子Agent 切分、置顶与立即入队", () => {
    const v = { ...emptyForm(), kind: "manual" as const, runner: "prompt" as const, prompt: "整理日报", agents: "code, task  feishu_group", front: true }
    const input = formToCreateInput(v)
    expect(input.agents).toEqual(["code", "task", "feishu_group"])
    expect(input.runNow).toBe(true)
    expect(input.front).toBe(true)
    expect(input.timeoutMs).toBeUndefined()
    expect(input.schedule).toBeUndefined()
  })

  test("闲时任务不携带 run_now/front（由闲时调度决定执行时机）", () => {
    const v = { ...emptyForm(), kind: "idle" as const, runner: "prompt" as const, prompt: "闲时清理", runNow: true, front: true }
    const input = formToCreateInput(v)
    expect(input.runNow).toBeUndefined()
    expect(input.front).toBeUndefined()
  })

  test("绑定会话：仅 target=session 且填了 id 时才带上", () => {
    const base = { ...emptyForm(), runner: "prompt" as const, prompt: "x" }
    expect(formToCreateInput({ ...base, target: "ephemeral" as const, sessionId: "s1" }).sessionId).toBeUndefined()
    expect(formToCreateInput({ ...base, target: "session" as const, sessionId: " s1 " }).sessionId).toBe("s1")
    expect(formToCreateInput({ ...base, target: "session" as const, sessionId: "" }).sessionId).toBeUndefined()
  })

  test("更新补丁：改名/启停/内容/阈值；通知留空=不改动（undefined 从 JSON 中消失）", () => {
    const v = { ...emptyForm(), kind: "scheduled" as const, runner: "prompt" as const, name: "新名", schedule: "0 8 * * *", prompt: "新提示词", enabled: true, timeoutMs: "" }
    const patch = formToUpdateInput(v)
    expect(patch.name).toBe("新名")
    expect(patch.schedule).toBe("0 8 * * *")
    expect(patch.timeoutMs).toBeUndefined()
    expect(patch.notify).toBeUndefined()
    expect(patch.notifyOn).toBe("auto") // 通知时机两值恒有（缺省 auto）
    expect(JSON.parse(JSON.stringify({ ...patch }))).not.toHaveProperty("notify")
  })

  test("编辑态反填：任务 → 表单 → 更新补丁 保持字段一致", () => {
    const t = mk({
      runner: "script",
      script: "bash run.sh",
      prompt: undefined,
      timezone: "Asia/Shanghai",
      misfire: "skip",
      timeoutMs: 5000,
      maxConsecutiveErrors: 2,
      notify: [{ type: "feishu_chat", target: "oc_abc", secret: "sec" }],
      notifyOn: "model",
    })
    const form = formFromTask(t)
    expect(form.schedule).toBe("0 9 * * *")
    expect(form.notifyText).toBe("feishu_chat oc_abc sec")
    const patch = formToUpdateInput(form)
    expect(patch.script).toBe("bash run.sh")
    expect(patch.misfire).toBe("skip")
    expect(patch.timeoutMs).toBe(5000)
    expect(patch.maxConsecutiveErrors).toBe(2)
    expect(patch.notifyOn).toBe("model")
    expect(patch.notify).toEqual([{ type: "feishu_chat", target: "oc_abc", secret: "sec" }])
  })

  test("校验：脚本型缺脚本 / 提示词型缺提示词 / 定时缺表达式 / 数字非法 / 通知行非法", () => {
    expect(validateForm({ ...emptyForm(), runner: "script", script: " " })).toContain("脚本")
    expect(validateForm({ ...emptyForm(), runner: "prompt", prompt: "" })).toContain("提示词")
    expect(validateForm({ ...emptyForm(), runner: "prompt", prompt: "x" })).toContain("执行表达式")
    expect(validateForm({ ...emptyForm(), runner: "prompt", prompt: "x", schedule: "0 9 * * *", timeoutMs: "abc" })).toContain("数字")
    expect(validateForm({ ...emptyForm(), runner: "prompt", prompt: "x", schedule: "0 9 * * *", notifyText: "sms 123" })).toContain("类型无效")
    expect(validateForm({ ...emptyForm(), runner: "prompt", prompt: "x", schedule: "0 9 * * *", notifyText: "webhook" })).toContain("缺少目标")
    expect(validateForm({ ...emptyForm(), kind: "manual", runner: "prompt", prompt: "x" })).toBeNull()
  })
})

describe("通知通道行文本", () => {
  test("行文本 → 通道（type target [secret]；webhook 32 位 hex 视为引用）", () => {
    expect(parseNotifyLines("")).toBeUndefined()
    expect(parseNotifyLines("\n  \n")).toBeUndefined()
    expect(parseNotifyLines("feishu https://open.feishu.cn/open-apis/bot/v2/hook/x sec123")).toEqual([
      { type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/x", secret: "sec123" },
    ])
    expect(parseNotifyLines("webhook https://e.com/hook")).toEqual([{ type: "webhook", target: "https://e.com/hook" }])
    expect(parseNotifyLines(`webhook ${"a".repeat(32)}`)).toEqual([{ type: "webhook", webhookId: "a".repeat(32) }])
  })

  test("通道 → 行文本（webhookId 引用保持引用形态）", () => {
    expect(notifyLines(mk({ notify: [{ type: "webhook", webhookId: "b".repeat(32) }, { type: "feishu_chat", target: "oc_x", secret: "s" }] }))).toBe(
      `webhook ${"b".repeat(32)}\nfeishu_chat oc_x s`,
    )
    expect(notifyLines(mk())).toBe("")
  })
})

describe("任务环境变量行文本", () => {
  test("环境变量 → 行文本（每行 NAME=值）", () => {
    expect(envLines(mk())).toBe("")
    expect(envLines(mk({ env: { API_BASE: "https://e.com", REPORT_DIR: "data" } }))).toBe("API_BASE=https://e.com\nREPORT_DIR=data")
    expect(envLines(mk({ env: { EMPTY: "" } }))).toBe("EMPTY=")
  })

  test("行文本 → 环境变量（空文本 undefined；值含 = 保留；非法行拒绝）", () => {
    expect(parseEnvLines("")).toBeUndefined()
    expect(parseEnvLines("\n   \n")).toBeUndefined()
    expect(parseEnvLines("A=1\nB=two words")).toEqual({ A: "1", B: "two words" })
    expect(parseEnvLines("TOKEN=abc=def")).toEqual({ TOKEN: "abc=def" })
    expect(parseEnvLines("EMPTY=")).toEqual({ EMPTY: "" })
    expect(() => parseEnvLines("NO_EQ")).toThrow(/缺少/)
    expect(() => parseEnvLines("1BAD=1")).toThrow(/变量名非法/)
  })

  test("创建请求体携带 env；编辑态反填后更新为全量视图（清空即 {}）", () => {
    const created = formToCreateInput({ ...emptyForm(), kind: "manual", runner: "prompt", prompt: "x", envText: "A=1" })
    expect(created.env).toEqual({ A: "1" })
    expect(formToCreateInput({ ...emptyForm(), kind: "manual", runner: "prompt", prompt: "x" }).env).toBeUndefined()

    const t = mk({ env: { A: "1", B: "2" } })
    const form = formFromTask(t)
    expect(form.envText).toBe("A=1\nB=2")
    expect(formToUpdateInput(form).env).toEqual({ A: "1", B: "2" })
    // 表单是环境变量的全量视图：清空后更新补丁为 {}（清除），而非「未提供不改动」
    expect(formToUpdateInput({ ...form, envText: "" }).env).toEqual({})
  })

  test("校验：环境变量行非法拦截", () => {
    expect(validateForm({ ...emptyForm(), runner: "prompt", prompt: "x", schedule: "0 9 * * *", envText: "NO_EQ" })).toContain("缺少")
    expect(validateForm({ ...emptyForm(), runner: "prompt", prompt: "x", schedule: "0 9 * * *", envText: "1BAD=1" })).toContain("变量名非法")
  })
})
