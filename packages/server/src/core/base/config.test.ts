import { describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { loadConfig, isolationConfigConflict } from "./config"

describe("loadConfig 模式与密钥解析", () => {
  test("任务全局默认通知通道环境变量解析", () => {
    const prevW = process.env.GEBAI_TASK_NOTIFY_WEBHOOK
    const prevF = process.env.GEBAI_TASK_NOTIFY_FEISHU
    try {
      delete process.env.GEBAI_TASK_NOTIFY_WEBHOOK
      delete process.env.GEBAI_TASK_NOTIFY_FEISHU
      const off = loadConfig()
      expect(off.taskNotifyWebhook).toBeUndefined()
      expect(off.taskNotifyFeishu).toBeUndefined()
      process.env.GEBAI_TASK_NOTIFY_WEBHOOK = "https://hooks.example.com/task"
      process.env.GEBAI_TASK_NOTIFY_FEISHU = "oc_0123456789abcdef"
      const on = loadConfig()
      expect(on.taskNotifyWebhook).toBe("https://hooks.example.com/task")
      expect(on.taskNotifyFeishu).toBe("oc_0123456789abcdef")
    } finally {
      for (const [k, v] of [["GEBAI_TASK_NOTIFY_WEBHOOK", prevW], ["GEBAI_TASK_NOTIFY_FEISHU", prevF]] as const) {
        if (v === undefined) delete process.env[k]
        else process.env[k] = v
      }
    }
  })

  test("GEBAI_TASKS_ENABLED 默认开启，显式 false 关闭", () => {
    const prev = process.env.GEBAI_TASKS_ENABLED
    try {
      delete process.env.GEBAI_TASKS_ENABLED
      expect(loadConfig().tasksEnabled).toBe(true)
      process.env.GEBAI_TASKS_ENABLED = "false"
      expect(loadConfig().tasksEnabled).toBe(false)
    } finally {
      if (prev === undefined) delete process.env.GEBAI_TASKS_ENABLED
      else process.env.GEBAI_TASKS_ENABLED = prev
    }
  })

  test("GEBAI_TASK_MAX_CONCURRENT：缺省 5，正数生效，非法值回落", () => {
    const prev = process.env.GEBAI_TASK_MAX_CONCURRENT
    try {
      delete process.env.GEBAI_TASK_MAX_CONCURRENT
      expect(loadConfig().taskMaxConcurrent).toBe(5)
      process.env.GEBAI_TASK_MAX_CONCURRENT = "2"
      expect(loadConfig().taskMaxConcurrent).toBe(2)
      // 非法（非正整数/非数）：回落缺省
      for (const bad of ["0", "-1", "abc"]) {
        process.env.GEBAI_TASK_MAX_CONCURRENT = bad
        expect(loadConfig().taskMaxConcurrent).toBe(5)
      }
    } finally {
      if (prev === undefined) delete process.env.GEBAI_TASK_MAX_CONCURRENT
      else process.env.GEBAI_TASK_MAX_CONCURRENT = prev
    }
  })

  test("子Agent 启停名单环境变量解析（GEBAI_SUB_AGENTS_ENABLE / GEBAI_SUB_AGENTS_DISABLE）", () => {
    const prevE = process.env.GEBAI_SUB_AGENTS_ENABLE
    const prevD = process.env.GEBAI_SUB_AGENTS_DISABLE
    try {
      delete process.env.GEBAI_SUB_AGENTS_ENABLE
      delete process.env.GEBAI_SUB_AGENTS_DISABLE
      const off = loadConfig()
      expect(off.subAgentsEnable).toEqual([])
      expect(off.subAgentsDisable).toEqual([])
      process.env.GEBAI_SUB_AGENTS_ENABLE = "code, self_optimize"
      process.env.GEBAI_SUB_AGENTS_DISABLE = "task, feishu_group"
      const on = loadConfig()
      expect(on.subAgentsEnable).toEqual(["code", "self_optimize"])
      expect(on.subAgentsDisable).toEqual(["task", "feishu_group"])
    } finally {
      for (const [k, v] of [
        ["GEBAI_SUB_AGENTS_ENABLE", prevE],
        ["GEBAI_SUB_AGENTS_DISABLE", prevD],
      ] as const) {
        if (v === undefined) delete process.env[k]
        else process.env[k] = v
      }
    }
  })

  test("飞书机器人行为开关环境变量解析（GEBAI_FEISHU_BOT_*，默认全关）", () => {
    const keys = ["GEBAI_FEISHU_BOT_NOTIFY_TOOLS", "GEBAI_FEISHU_BOT_NOTIFY_ASSISTANT", "GEBAI_FEISHU_BOT_AUTO_APPROVE"] as const
    const prev = keys.map((k) => [k, process.env[k]] as const)
    try {
      // 置空串而非删除：loadConfig 内 loadDotEnv 会在变量未定义时从仓库根 .env 回填
      //（本机飞书通道真实配置存在这些变量），删除后测试结果依赖宿主 .env 内容；空串已定义不
      // 触发回填且 bool 解析为 false——对宿主环境封闭
      for (const k of keys) process.env[k] = ""
      const off = loadConfig()
      expect(off.feishuBotNotifyTools).toBe(false)
      expect(off.feishuBotNotifyAssistant).toBe(false)
      expect(off.feishuBotAutoApprove).toBe(false)
      process.env.GEBAI_FEISHU_BOT_NOTIFY_TOOLS = "true"
      process.env.GEBAI_FEISHU_BOT_NOTIFY_ASSISTANT = "1"
      process.env.GEBAI_FEISHU_BOT_AUTO_APPROVE = "true"
      const on = loadConfig()
      expect(on.feishuBotNotifyTools).toBe(true)
      expect(on.feishuBotNotifyAssistant).toBe(true)
      expect(on.feishuBotAutoApprove).toBe(true)
    } finally {
      for (const [k, v] of prev) {
        if (v === undefined) delete process.env[k]
        else process.env[k] = v
      }
    }
  })

  test("运行形态：默认 local，GEBAI_MODE=server 开启服务模式，兼容旧 GEBAI_AUTH", () => {
    const saved = { ...process.env }
    delete process.env.GEBAI_MODE
    delete process.env.GEBAI_AUTH
    expect(loadConfig().auth).toBe("local")
    process.env.GEBAI_MODE = "server"
    expect(loadConfig().auth).toBe("server")
    delete process.env.GEBAI_MODE
    process.env.GEBAI_AUTH = "multi"
    expect(loadConfig().auth).toBe("server")
    process.env.GEBAI_AUTH = "none"
    expect(loadConfig().auth).toBe("local")
    process.env = saved
  })

  test("服务密钥机制已移除：config 不再读取 GEBAI_SERVICE_API_KEY（接口统一账号密码认证）", () => {
    const saved = { ...process.env }
    delete process.env.GEBAI_SERVICE_API_KEY
    process.env.GEBAI_SERVICE_API_KEY = "svc-key"
    const cfg = loadConfig()
    expect("apiKey" in cfg).toBe(false)
    expect((cfg as unknown as Record<string, unknown>).apiKey).toBeUndefined()
    process.env = saved
  })

  test("脚本隔离模式（GEBAI_SCRIPT_ISOLATION）解析：默认 auto，白名单外/大小写容错，非法值回落 auto", () => {
    const saved = { ...process.env }
    try {
      delete process.env.GEBAI_SCRIPT_ISOLATION
      expect(loadConfig().scriptIsolation).toBe("auto")
      for (const [raw, want] of [["off", "off"], ["env", "env"], ["bwrap", "bwrap"], [" BWRAP ", "bwrap"], ["yes", "auto"], ["", "auto"]] as const) {
        process.env.GEBAI_SCRIPT_ISOLATION = raw
        expect(loadConfig().scriptIsolation).toBe(want)
      }
    } finally {
      process.env = saved
    }
  })

  test("服务模式默认且强制开启会话目录隔离：off 与服务模式互斥（启动拒绝），本地模式不受限", () => {
    const base = { auth: "server", sandbox: "auto" } as const
    // 服务模式：off 报冲突（与 GEBAI_SANDBOX=off 同款防呆）；auto/env/bwrap 均合规
    expect(isolationConfigConflict({ ...base, scriptIsolation: "off" })).toContain("GEBAI_SCRIPT_ISOLATION=off")
    expect(isolationConfigConflict({ ...base, scriptIsolation: "auto" })).toBeNull()
    expect(isolationConfigConflict({ ...base, scriptIsolation: "env" })).toBeNull()
    expect(isolationConfigConflict({ ...base, scriptIsolation: "bwrap" })).toBeNull()
    // 路径沙箱互斥（既有防呆同样收敛到本函数）：服务模式 + GEBAI_SANDBOX=off
    expect(isolationConfigConflict({ auth: "server", sandbox: "off", scriptIsolation: "auto" })).toContain("GEBAI_SANDBOX=off")
    // 本地模式：两者都可显式关闭（操作者本人机器）
    expect(isolationConfigConflict({ auth: "local", sandbox: "off", scriptIsolation: "off" })).toBeNull()
  })

  test("提示词裁剪与领域补充（GEBAI_PROMPT_ENABLE/DISABLE/EXTRA）解析", () => {
    const saved = { ...process.env }
    try {
      // 置空串而非删除（与飞书开关用例同口径）：已定义不触发 .env 回填，对宿主环境封闭
      for (const k of ["GEBAI_PROMPT_ENABLE", "GEBAI_PROMPT_DISABLE", "GEBAI_PROMPT_EXTRA", "GEBAI_PROMPT_EXTRA_FILE", "GEBAI_PROFILE"]) process.env[k] = ""
      const off = loadConfig()
      expect(off.promptEnable).toEqual([])
      expect(off.promptDisable).toEqual([])
      expect(off.promptExtra).toBeUndefined()
      expect(off.profile).toBeUndefined()
      process.env.GEBAI_PROMPT_ENABLE = "persona, workspace"
      process.env.GEBAI_PROMPT_DISABLE = "subagent_catalog"
      process.env.GEBAI_PROMPT_EXTRA = "领域补充：只谈订单。"
      const on = loadConfig()
      expect(on.promptEnable).toEqual(["persona", "workspace"])
      expect(on.promptDisable).toEqual(["subagent_catalog"])
      expect(on.promptExtra).toBe("领域补充：只谈订单。")
    } finally {
      process.env = saved
    }
  })

  test("领域专用模式档案（GEBAI_PROFILE）：档案为默认值，显式环境变量按字段覆盖", () => {
    const saved = { ...process.env }
    const home = mkdtempSync(join(tmpdir(), "gebai-profile-cfg-"))
    try {
      mkdirSync(join(home, "profiles"), { recursive: true })
      writeFileSync(
        join(home, "profiles", "coding.json"),
        JSON.stringify({
          name: "coding",
          prompt: { disable: ["artifact_naming"], extra: "只写代码。", extra_file: "coding-prompt.md" },
          tools: { disable: ["sh"] },
          sub_agents: { enable: ["code", "explore"], preload: ["code"] },
        }),
      )
      writeFileSync(join(home, "profiles", "coding-prompt.md"), "领域提示词文件内容")
      for (const k of [
        "GEBAI_PROMPT_ENABLE",
        "GEBAI_PROMPT_DISABLE",
        "GEBAI_PROMPT_EXTRA",
        "GEBAI_PROMPT_EXTRA_FILE",
        "GEBAI_TOOL_ENABLE",
        "GEBAI_TOOL_DISABLE",
        "GEBAI_SUB_AGENTS_ENABLE",
        "GEBAI_SUB_AGENTS_DISABLE",
        "GEBAI_PRELOAD_SUB_AGENTS",
      ]) process.env[k] = ""
      process.env.GEBAI_HOME = home
      process.env.GEBAI_PROFILE = "coding"
      const byName = loadConfig()
      expect(byName.profile).toBe("coding")
      expect(byName.promptDisable).toEqual(["artifact_naming"])
      expect(byName.promptExtra).toBe("领域提示词文件内容\n只写代码。") // 文件在前、内联在后
      expect(byName.toolDisable).toEqual(["sh"])
      expect(byName.subAgentsEnable).toEqual(["code", "explore"])
      expect(byName.preloadSubAgents).toEqual(["code"])
      // 显式环境变量按字段覆盖：同名清单用环境变量，未配置的字段仍用档案值
      process.env.GEBAI_PRELOAD_SUB_AGENTS = "wps"
      process.env.GEBAI_PROMPT_EXTRA = "只谈订单。"
      const overridden = loadConfig()
      expect(overridden.preloadSubAgents).toEqual(["wps"])
      expect(overridden.promptExtra).toBe("只谈订单。") // 环境变量源整体覆盖档案源（含档案的 extra_file）
      expect(overridden.toolDisable).toEqual(["sh"])
      expect(overridden.subAgentsEnable).toEqual(["code", "explore"])
      // 档案缺失：启动期报错（不静默按默认能力面运行）
      process.env.GEBAI_PROFILE = "ghost"
      expect(() => loadConfig()).toThrow(/档案不存在/)
    } finally {
      process.env = saved
      rmSync(home, { recursive: true, force: true })
    }
  })
})
