import { describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { SessionStore, toSessionInfo } from "./store"
import { sessionPath } from "../base/paths"

/** 会话目录（listSessionInfos 读 meta.json 的位置）。 */
function dirOf(home: string, user: string, id: string): string {
  return sessionPath(home, user, id)
}

async function seed(store: SessionStore, user: string, msgs = 3) {
  const s = await store.createSession(user, "会话")
  for (let i = 0; i < msgs; i++) {
    await store.appendMessage(s.id, { id: `m${i}`, role: "user", content: `消息 ${i}`, createdAt: i + 1 } as never, user)
  }
  return s.id
}

describe("会话列表元信息缓存（meta.json）", () => {
  test("save 同步写 meta；缓存命中时直接返回元信息，不再回退解析正文", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-meta-"))
    try {
      const store = new SessionStore({ home })
      const id = await seed(store, "alice")
      const dir = dirOf(home, "alice", id)
      const chat = join(dir, "chat.json")
      const meta = join(dir, "meta.json")
      expect(existsSync(meta)).toBe(true)
      expect(JSON.parse(readFileSync(meta, "utf8")).source.size).toBe(statSync(chat).size)

      // 列表命中缓存：不重写 meta（回退解析正丈的分支必然重写 meta，mtime 变化即证据）
      const before = statSync(meta).mtimeMs
      const infos = await store.listSessionInfos("alice")
      expect(infos.map((s) => s.id)).toEqual([id])
      expect(infos[0].name).toBe("会话")
      expect(statSync(meta).mtimeMs).toBe(before)

      // 对照：缓存缺失时会回退读正丈并重建缓存（mtime 变化）
      rmSync(meta)
      expect(await store.listSessionInfos("alice")).toHaveLength(1)
      expect(existsSync(meta)).toBe(true)
      expect(statSync(meta).mtimeMs).not.toBe(before)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("缓存陈旧（正文被外部修改/旧版本写入）时回退读正文并刷新缓存", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-meta-stale-"))
    try {
      const store = new SessionStore({ home })
      const id = await seed(store, "alice")
      const dir = dirOf(home, "alice", id)
      const chat = join(dir, "chat.json")
      // 外部改写正文（模拟旧版本写入/手工编辑/备份恢复）：meta 指纹不再匹配
      const data = JSON.parse(readFileSync(chat, "utf8"))
      data.name = "外部改名"
      writeFileSync(chat, JSON.stringify(data, null, 2))
      const infos = await store.listSessionInfos("alice")
      expect(infos[0].name).toBe("外部改名") // 读到的是正文新值，而非陈旧缓存
      // 缓存已就地刷新：下次仍返回新值
      expect(JSON.parse(readFileSync(join(dir, "meta.json"), "utf8")).name).toBe("外部改名")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("存量会话（无 meta）首次列表自动建立缓存", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-meta-legacy-"))
    try {
      const store = new SessionStore({ home })
      const id = await seed(store, "alice")
      const dir = dirOf(home, "alice", id)
      rmSync(join(dir, "meta.json"))
      const infos = await store.listSessionInfos("alice")
      expect(infos.map((s) => s.id)).toEqual([id])
      expect(existsSync(join(dir, "meta.json"))).toBe(true)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("改名/置顶经缓存路径立即可见（meta 随 save 刷新）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-meta-ops-"))
    try {
      const store = new SessionStore({ home })
      const a = await seed(store, "alice")
      const b = await seed(store, "alice")
      await store.rename(a, "改名后", "alice")
      await store.setPinned(b, true, "alice")
      const infos = await store.listSessionInfos("alice")
      expect(infos[0].id).toBe(b) // 置顶优先
      expect(infos[0].pinned).toBe(true)
      expect(infos.find((s) => s.id === a)?.name).toBe("改名后")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("删除会话后 meta 随之移除（不残留幽灵条目）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-meta-del-"))
    try {
      const store = new SessionStore({ home })
      const id = await seed(store, "alice")
      await store.delete(id, "alice")
      expect(existsSync(dirOf(home, "alice", id))).toBe(false)
      expect(await store.listSessionInfos("alice")).toHaveLength(0)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("列表与 load 的上下文用量口径一致（真值优先，估算值不污染 SessionData.ctxTokens）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-meta-ctx-"))
    try {
      const store = new SessionStore({ home })
      const id = await seed(store, "alice", 5)
      const infos = await store.listSessionInfos("alice")
      const loaded = await store.load(id, "alice")
      expect(infos[0].ctxTokens).toBeDefined() // 列表有展示值（估算兜底）
      expect(infos[0].ctxTokens).toBe(toSessionInfo(loaded!).ctxTokens) // 与详情口径一致
      expect(loaded!.ctxTokens).toBeUndefined() // 磁盘上仍无 usage 真值，字段语义未被污染
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("跨用户隔离：meta 缓存不越权返回他人会话", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-meta-iso-"))
    try {
      const store = new SessionStore({ home })
      await seed(store, "alice")
      expect(await store.listSessionInfos("bob")).toHaveLength(0)
      expect(await store.listSessionInfos("alice")).toHaveLength(1)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
