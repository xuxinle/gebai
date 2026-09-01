import { describe, expect, test } from "bun:test"
import { loadConfig } from "./config"

describe("loadConfig 模式与密钥解析", () => {
  test("定时任务全局默认通知通道环境变量解析", () => {
    const prevW = process.env.GEBAI_CRON_NOTIFY_WEBHOOK
    const prevF = process.env.GEBAI_CRON_NOTIFY_FEISHU
    try {
      delete process.env.GEBAI_CRON_NOTIFY_WEBHOOK
      delete process.env.GEBAI_CRON_NOTIFY_FEISHU
      const off = loadConfig()
      expect(off.cronNotifyWebhook).toBeUndefined()
      expect(off.cronNotifyFeishu).toBeUndefined()
      process.env.GEBAI_CRON_NOTIFY_WEBHOOK = "https://hooks.example.com/cron"
      process.env.GEBAI_CRON_NOTIFY_FEISHU = "oc_0123456789abcdef"
      const on = loadConfig()
      expect(on.cronNotifyWebhook).toBe("https://hooks.example.com/cron")
      expect(on.cronNotifyFeishu).toBe("oc_0123456789abcdef")
    } finally {
      for (const [k, v] of [["GEBAI_CRON_NOTIFY_WEBHOOK", prevW], ["GEBAI_CRON_NOTIFY_FEISHU", prevF]] as const) {
        if (v === undefined) delete process.env[k]
        else process.env[k] = v
      }
    }
  })

  test("GEBAI_CRON_ENABLED 默认开启，显式 false 关闭", () => {
    const prev = process.env.GEBAI_CRON_ENABLED
    try {
      delete process.env.GEBAI_CRON_ENABLED
      expect(loadConfig().cronEnabled).toBe(true)
      process.env.GEBAI_CRON_ENABLED = "false"
      expect(loadConfig().cronEnabled).toBe(false)
    } finally {
      if (prev === undefined) delete process.env.GEBAI_CRON_ENABLED
      else process.env.GEBAI_CRON_ENABLED = prev
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
      process.env.GEBAI_SUB_AGENTS_DISABLE = "cron, feishu_group"
      const on = loadConfig()
      expect(on.subAgentsEnable).toEqual(["code", "self_optimize"])
      expect(on.subAgentsDisable).toEqual(["cron", "feishu_group"])
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
})
