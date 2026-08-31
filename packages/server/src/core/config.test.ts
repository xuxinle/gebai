import { describe, expect, test } from "bun:test"
import { loadConfig } from "./config"

describe("loadConfig 模式与密钥解析", () => {
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
