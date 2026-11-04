import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { SessionStore } from "./store"
import { EnvManager } from "./env"

describe("会话模型配置（chat.json 持久化 + 任务 env 生效）", () => {
  test("setModelConfig：落盘持久化、切换不刷新 updatedAt、空值清除字段", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-modelcfg-"))
    try {
      const store = new SessionStore({ home })
      const s = await store.createSession("alice", "会话")
      const before = (await store.load(s.id, "alice"))!.updatedAt
      await store.setModelConfig(s.id, "glm", "alice")
      const loaded = (await store.load(s.id, "alice"))!
      expect(loaded.modelConfig).toBe("glm")
      expect(loaded.updatedAt).toBe(before) // 配置操作不动排序基线
      // 重开 store（进程重启）：仍是持久化的值
      expect((await new SessionStore({ home }).load(s.id, "alice"))!.modelConfig).toBe("glm")
      // 列表路径（meta 缓存）同样带上（前端切换会话据此对齐下拉）
      expect((await store.listSessionInfos("alice")).find((x) => x.id === s.id)?.modelConfig).toBe("glm")
      // 空/空白 = 清除（回落清单默认配置），落盘干净
      await store.setModelConfig(s.id, "  ", "alice")
      expect((await store.load(s.id, "alice"))!.modelConfig).toBeUndefined()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("env.resolve：会话选择以 GEBAI_LLM_CONFIG 并入任务 env（每次运行都按它解析 Provider）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-modelcfg-env-"))
    try {
      const store = new SessionStore({ home })
      const env = new EnvManager(store)
      const a = await store.createSession("alice", "A")
      const b = await store.createSession("alice", "B")
      await store.setModelConfig(a.id, "glm", "alice")
      expect((await env.resolve(a.id, "alice")).GEBAI_LLM_CONFIG).toBe("glm")
      // 未选择配置的会话不带该键（用清单默认，即第一条）
      expect((await env.resolve(b.id, "alice")).GEBAI_LLM_CONFIG).toBeUndefined()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("env.resolve：会话/前端脚本显式设置的同名键优先（配置按引用覆盖，不被会话默认盖掉）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-modelcfg-over-"))
    try {
      const store = new SessionStore({ home })
      const env = new EnvManager(store)
      const s = await store.createSession("alice", "S")
      await store.setModelConfig(s.id, "glm", "alice")
      await store.setEnv(s.id, "alice", { GEBAI_LLM_CONFIG: "big" })
      expect((await env.resolve(s.id, "alice")).GEBAI_LLM_CONFIG).toBe("big")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
