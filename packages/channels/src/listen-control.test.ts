/**
 * 渠道监听启停：**只停收消息**，发送/读取不受影响；只影响本次运行。
 *
 * 为什么这两个语义必须钉住：
 *
 * 1. **只停监听** —— 真实诉求是「先别收消息了，但还能发」（群太吵、要重新授权、
 *    排查事件风暴）。若实现成「整个实例下线」，那用户失去的比想停的多。
 * 2. **出站实例返回 400** —— 出站实例本来就不建连接（能力面里没有 startListen）。
 *    静默返回成功会让用户以为“点了开关它就会收”，而实际什么都没发生。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { ChannelRegistry } from "./registry"
import type { ChannelDef } from "./channel"

/** 造一个能收也能发的假渠道（不碰网络）。 */
function makeChannel(state: { listened: number; started: number }): ChannelDef {
  return {
    name: "fake",
    description: "测试渠道",
    dirs: ["in", "out"],
    config: {},
    verbs: { send: { summary: "发", args: {} } },
    observes: { messages: { summary: "读", args: {} } },
    listens: { messages: { summary: "收" } },
    act: async () => ({ summary: "ok" }),
    observe: async () => ({ summary: "ok" }),
    async start() {
      state.started++
    },
    async startListen() {
      state.listened++
    },
    async stopListen() {
      state.listened--
    },
  }
}

function makeRegistry(env: Record<string, string>, state: { listened: number; started: number }) {
  const home = mkdtempSync(join(tmpdir(), "ch-listen-"))
  const reg = new ChannelRegistry({
    scanDirs: { builtin: join(home, "__n1__"), custom: join(home, "__n2__") },
    home,
    envReader: (n) => env[n],
    log: { info: () => {}, warn: () => {}, error: () => {} },
  })
  // 监听需要输入桥接器（`startDef` 里 `if (def.startListen && this.bridge)` 才订阅）。
  // 不接的话 `listening` 永远是 false——测试会误以为“停监听没生效”。
  reg.attachDispatcher({ deliver: async () => ({ status: "accepted" }) } as never)
  reg.register(makeChannel(state))
  return { reg, home }
}

const ENV = {
  GEBAI_CHANNEL_IN_FAKE: JSON.stringify([{ name: "fake_in" }]),
  GEBAI_CHANNEL_OUT_FAKE: JSON.stringify([{ name: "fake_out" }]),
}

describe("state.canListen：前端据此决定开关是否可点", () => {
  test("入站实例可启停监听；出站实例不可", async () => {
    const state = { listened: 0, started: 0 }
    const { reg, home } = makeRegistry(ENV, state)
    try {
      await reg.init()
      const inSt = reg.state().find((s) => s.name === "fake_in")!
      const outSt = reg.state().find((s) => s.name === "fake_out")!
      console.log(`    实测: fake_in.canListen=${inSt.canListen} fake_out.canListen=${outSt.canListen}`)
      expect(inSt.canListen).toBe(true)
      expect(outSt.canListen).toBe(false)
      // 出站实例连 listens 清单都没有（能力面裁剪过）
      expect(outSt.listens).toEqual([])
    } finally {
      await reg.stop()
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("stop 监听：只影响收，不影响发/读", () => {
  test("停监听后 listening=false，但 act/observe 仍可用", async () => {
    const state = { listened: 0, started: 0 }
    const { reg, home } = makeRegistry(ENV, state)
    try {
      await reg.init()
      expect(reg.state().find((s) => s.name === "fake_in")!.listening).toBe(true)

      const r = await reg.setListening("fake_in", false)
      console.log(`    实测: stop → ok=${r.ok} listened=${state.listened}`)
      expect(r.ok).toBe(true)
      expect(reg.state().find((s) => s.name === "fake_in")!.listening).toBe(false)

      // 关键：实例本身仍在运行，写/读面照常（stop 的是监听，不是实例）
      // 关键：停的是**监听**，不是实例。实例仍在运行（`started` 不变），
      // 而它的“发送/读取能力”属于**出站实例**（`fake_out`）——那边不受影响。
      expect(reg.state().find((s) => s.name === "fake_in")!.started).toBe(true)
      const outDef = reg.usable("fake_out")
      expect(outDef).not.toBeNull()
      expect(typeof outDef!.act).toBe("function")
      expect(typeof outDef!.observe).toBe("function")
      // 入站实例本来就没有 act（能力面按方向裁剪过）：这不是缺陷，是隔离
      expect(reg.usable("fake_in")!.act).toBeUndefined()
    } finally {
      await reg.stop()
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("恢复监听 → listening=true（可以反复拨）", async () => {
    const state = { listened: 0, started: 0 }
    const { reg, home } = makeRegistry(ENV, state)
    try {
      await reg.init()
      await reg.setListening("fake_in", false)
      expect(reg.state().find((s) => s.name === "fake_in")!.listening).toBe(false)

      const r = await reg.setListening("fake_in", true)
      expect(r.ok).toBe(true)
      expect(reg.state().find((s) => s.name === "fake_in")!.listening).toBe(true)
    } finally {
      await reg.stop()
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("幂等：已是目标状态时再拨一次不重复建/拆", async () => {
    const state = { listened: 0, started: 0 }
    const { reg, home } = makeRegistry(ENV, state)
    try {
      await reg.init()
      const before = state.listened
      const r = await reg.setListening("fake_in", true) // 已经开着
      expect(r.ok).toBe(true)
      expect(state.listened).toBe(before) // 没有重复 startListen
    } finally {
      await reg.stop()
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("对出站实例启停监听：明确拒绝而不是静默成功", () => {
  test("出站实例 → ok=false 且说明原因", async () => {
    const state = { listened: 0, started: 0 }
    const { reg, home } = makeRegistry(ENV, state)
    try {
      await reg.init()
      const r = await reg.setListening("fake_out", true)
      console.log(`    实测: ${JSON.stringify(r)}`)
      expect(r.ok).toBe(false)
      expect(r.reason).toContain("出站实例")
      expect(r.reason).toContain("没有可启停的监听")
    } finally {
      await reg.stop()
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("未知实例 → ok=false（不抛错，便于前端直接展示）", async () => {
    const state = { listened: 0, started: 0 }
    const { reg, home } = makeRegistry(ENV, state)
    try {
      await reg.init()
      const r = await reg.setListening("nope", true)
      expect(r.ok).toBe(false)
      expect(r.reason).toContain("未知渠道实例")
    } finally {
      await reg.stop()
      rmSync(home, { recursive: true, force: true })
    }
  })
})
