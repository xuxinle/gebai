import { describe, expect, test } from "bun:test"
import { mkdtempSync, readFileSync, rmSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import { HumanSpeechWindow, yieldWindowFromEnv, DEFAULT_YIELD_WINDOW_MS } from "./human-speech-window"

describe("HumanSpeechWindow：用户亲手发言的让位窗口", () => {
  test("用户发言后进入窗口，窗口过后自动失效（不需要人工解除）", () => {
    let clock = 1_000_000
    const w = new HumanSpeechWindow({ windowMs: 60_000, now: () => clock })
    expect(w.inWindow("oc_a")).toBe(false) // 没说过话
    w.note("oc_a")
    expect(w.inWindow("oc_a")).toBe(true)
    clock += 59_000
    expect(w.inWindow("oc_a")).toBe(true) // 窗口内
    clock += 2_000
    // 关键：过期**自动**失效。静音式设计在这里会让群永久哑掉，没人会去解除它
    expect(w.inWindow("oc_a")).toBe(false)
  })

  test("按群隔离：A 群说话不影响 B 群", () => {
    let clock = 1_000_000
    const w = new HumanSpeechWindow({ windowMs: 60_000, now: () => clock })
    w.note("oc_a")
    expect(w.inWindow("oc_a")).toBe(true)
    expect(w.inWindow("oc_b")).toBe(false) // 用户在 A 说话，不该影响人格在 B 答别人
    w.note("oc_b")
    expect(w.inWindow("oc_b")).toBe(true)
  })

  test("重复发言重置窗口（后来的发言重新起算）", () => {
    let clock = 1_000_000
    const w = new HumanSpeechWindow({ windowMs: 60_000, now: () => clock })
    w.note("oc_a")
    clock += 50_000
    w.note("oc_a") // 又说了一句
    clock += 30_000 // 距第一次 80s（本该过期），距第二次 30s
    expect(w.inWindow("oc_a")).toBe(true)
  })

  test("remainingMs 给出剩余时间（给模型的提示措辞要用）", () => {
    let clock = 1_000_000
    const w = new HumanSpeechWindow({ windowMs: 60_000, now: () => clock })
    w.note("oc_a")
    clock += 20_000
    expect(w.remainingMs("oc_a")).toBe(40_000)
    clock += 100_000
    expect(w.remainingMs("oc_a")).toBe(0)
  })

  test("chatId 为空时忽略（拿不到群就没法按群让位）", () => {
    const w = new HumanSpeechWindow({ windowMs: 60_000 })
    w.note(undefined)
    w.note("")
    expect(w.size()).toBe(0)
  })

  test("长期运行不无界增长（陈旧群被清理）", () => {
    let clock = 1_000_000
    const w = new HumanSpeechWindow({ windowMs: 60_000, now: () => clock })
    for (let i = 0; i < 600; i += 1) {
      w.note(`oc_${i}`)
      clock += 1_000
    }
    clock += 60 * 60_000 // 过一小时，全部陈旧
    w.note("oc_new") // 触发清理
    expect(w.size()).toBeLessThan(600)
  })
})

describe("让位窗口的配置", () => {
  test("缺省 3 分钟", () => {
    expect(yieldWindowFromEnv({})).toBe(DEFAULT_YIELD_WINDOW_MS)
    expect(DEFAULT_YIELD_WINDOW_MS).toBe(3 * 60_000)
  })

  test("GEBAI_PERSONA_YIELD_SECONDS=0 → 关闭让位（窗口 0 = 永不阻拦）", () => {
    expect(yieldWindowFromEnv({ GEBAI_PERSONA_YIELD_SECONDS: "0" })).toBe(0)
    const w = new HumanSpeechWindow({ windowMs: 0, now: () => 1000 })
    w.note("oc_a")
    // 0 窗口 = 立即过期：等于不启用让位
    expect(w.inWindow("oc_a", 1000)).toBe(false)
  })

  test("非法值回落缺省（不静默变成 0/NaN）", () => {
    expect(yieldWindowFromEnv({ GEBAI_PERSONA_YIELD_SECONDS: "abc" })).toBe(DEFAULT_YIELD_WINDOW_MS)
    expect(yieldWindowFromEnv({ GEBAI_PERSONA_YIELD_SECONDS: "-5" })).toBe(DEFAULT_YIELD_WINDOW_MS)
  })
})

describe("SentMessageRegistry 落盘：跨重启仍能认出自己发的消息", () => {
  test("进程重启后仍命中（这是「人格把自己话当新指令」的根治点）", async () => {
    const { SentMessageRegistry } = await import("@gebai/channels")
    const dir = mkdtempSync(join(tmpdir(), "sentmsg-"))
    const path = join(dir, "sent-messages.jsonl")

    // 第一个进程：人格发了三条
    const a = new SentMessageRegistry({ storePath: path })
    a.record("om_1")
    a.record("om_2")

    // 重启：全新实例从盘上恢复
    const b = new SentMessageRegistry({ storePath: path })
    expect(b.hit("om_1")).toBe(true)
    expect(b.hit("om_2")).toBe(true)
    // 用户亲手发的（没被记录过）仍然是 false —— 不能因为落盘就把用户发言也吞掉
    expect(b.hit("om_user_typed")).toBe(false)

    rmSync(dir, { recursive: true, force: true })
  })

  test("TTL 是 7 天（旧值 10 分钟会让重投窗口外的自发言失去保护）", async () => {
    const { SentMessageRegistry, DEFAULT_TTL_MS } = await import("@gebai/channels")
    expect(DEFAULT_TTL_MS).toBe(7 * 24 * 60 * 60_000)
    const dir = mkdtempSync(join(tmpdir(), "sentmsg-ttl-"))
    const path = join(dir, "m.jsonl")
    const a = new SentMessageRegistry({ storePath: path })
    a.record("om_1")
    // 11 分钟：旧实现到这里就失效了
    const b = new SentMessageRegistry({ storePath: path })
    expect(b.hit("om_1")).toBe(true)
    rmSync(dir, { recursive: true, force: true })
  })

  test("加载时跳过已过期条目（不让陈旧记录占容量）", async () => {
    const { SentMessageRegistry } = await import("@gebai/channels")
    const dir = mkdtempSync(join(tmpdir(), "sentmsg-exp-"))
    const path = join(dir, "m.jsonl")
    const t0 = 1_000_000
    const a = new SentMessageRegistry({ storePath: path, now: () => t0 })
    a.record("om_old")
    // 8 天后重启
    const b = new SentMessageRegistry({ storePath: path, now: () => t0 + 8 * 24 * 60 * 60_000 })
    expect(b.hit("om_old")).toBe(false)
    rmSync(dir, { recursive: true, force: true })
  })

  test("文件损坏/半写不阻断启动（尽力而为的身份记忆）", async () => {
    const { SentMessageRegistry } = await import("@gebai/channels")
    const dir = mkdtempSync(join(tmpdir(), "sentmsg-bad-"))
    const path = join(dir, "m.jsonl")
    const ok = new SentMessageRegistry({ storePath: path })
    ok.record("om_good")
    // 追加一行垃圾（**必须 await**：Bun.write 是异步的，不等它就构造新实例会读到旧内容，
    // 于是这条测试会“碰巧通过”而实际什么都没验证到）
    await Bun.write(path, readFileSync(path, "utf-8") + "这不是合法行\n\u0000乱码")
    const next = new SentMessageRegistry({ storePath: path })
    expect(next.hit("om_good")).toBe(true) // 好的那条仍在
    rmSync(dir, { recursive: true, force: true })
  })
})
