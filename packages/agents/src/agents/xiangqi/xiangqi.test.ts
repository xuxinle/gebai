/**
 * 相期控制器的测试。
 *
 * 控制器是 **WebSocket 服务端**（相期是客户端连过来）。
 * 所以这里用**真的 WS 客户端**冒充相期，而不是给服务端塞桩——
 * 桩测不了「协议方向对不对」，而方向搞反正是最容易犯且最难发现的错
 * （控制器连自己形成自环：`isConnected` 报 true、`send` 不报错，卡片永远到不了）。
 *
 * 重点验收四条语义：
 *
 * 1. **连接是宿主唯一的**——多个会话拿同一个控制器，不抢端口
 * 2. **送不到要如实说**——不假装成功
 * 3. **超时不丢卡片**——卡片仍在窗口上，答复仍会回来
 * 4. **不重试**——送不到时 `card_ask` 直接返回，不白等
 */

import { afterEach, beforeEach, describe, expect, it } from "bun:test"
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

import type { CardAnswer } from "./controller"
import { controller, createController, resetControllerForTest } from "./controller"
import { createTools, DEFAULT_WAIT_MS, describeAnswer, MAX_WAIT_MS } from "./tools"

/** 冒充相期的 WS 客户端：连上去并记录收到的报文。 */
class FakeXiangqi {
  received: Array<{ type?: string; cards?: Array<{ id: string; kind: string; title: string }> }> = []
  private socket: WebSocket

  private constructor(socket: WebSocket) {
    this.socket = socket
  }

  static async connect(url: string): Promise<FakeXiangqi> {
    const socket = new WebSocket(url)
    const peer = new FakeXiangqi(socket)
    socket.onmessage = (event) => {
      try {
        peer.received.push(JSON.parse(String(event.data)))
      } catch {
        // 脏数据忽略
      }
    }
    await new Promise<void>((resolve, reject) => {
      socket.onopen = () => resolve()
      socket.onerror = () => reject(new Error("连不上控制器"))
      setTimeout(() => reject(new Error("连接超时")), 3000)
    })
    return peer
  }

  /** 最近一次推下来的 cards。 */
  lastCards(): Array<{ id: string; kind: string; title: string }> {
    for (let index = this.received.length - 1; index >= 0; index -= 1) {
      const message = this.received[index]!
      if (message.type === "cards") return message.cards ?? []
    }
    return []
  }

  /** 模拟用户点了某个选项。 */
  answer(id: string, kind: string, answer: Record<string, unknown>): void {
    this.socket.send(JSON.stringify({ type: "answer", id, kind, answer }))
  }

  close(): void {
    this.socket.close()
  }
}

/**
 * 造一个监听随机端口、还没人连的控制器。
 *
 * ⚠ 必须显式给 `homeDir`（临时目录）：不给的话会落到缺省的 `<cwd>/相期`，
 * 测试会把卡片文件写到仓库目录里、还互相干扰。
 */
function fresh(homeDir?: string) {
  return createController({
    port: 0,
    homeDir: homeDir ?? mkdtempSync(join(tmpdir(), "xq-test-")),
  })
}

/** 造一个带指定数据根（用于文件通路测试）的控制器。 */
function freshWithHome(homeDir: string) {
  return createController({ port: 0, homeDir, outboxPollMs: 20 })
}

/** 等直到条件成立（轮询），超时抛。 */
async function until(condition: () => boolean, ms = 2000, label = "条件"): Promise<void> {
  const deadline = Date.now() + ms
  while (Date.now() < deadline) {
    if (condition()) return
    await new Promise((resolve) => setTimeout(resolve, 10))
  }
  throw new Error(`等待超时：${label}`)
}

beforeEach(() => {
  resetControllerForTest()
})

afterEach(() => {
  resetControllerForTest()
})

describe("控制器：方向", () => {
  it("是服务端——相期连过来才算 connected", async () => {
    const ctl = fresh()
    ctl.listen()
    // 没人连时如实说未连接
    expect(ctl.isConnected()).toBe(false)

    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => ctl.isConnected(), 2000, "相期连上")
    expect(ctl.isConnected()).toBe(true)

    peer.close()
    await until(() => !ctl.isConnected(), 2000, "相期断开后复位")
    ctl.close()
  })

  it("url() 在 port=0 时给出真实端口", () => {
    const ctl = fresh()
    ctl.listen()
    expect(ctl.url()).toMatch(/^ws:\/\/127\.0\.0\.1:\d+\/xiangqi$/)
    // 端口不可能是 0（那是“让系统挑”的常量，不是实际端口）
    expect(ctl.url()).not.toContain(":0/")
    ctl.close()
  })
})

describe("控制器：投递", () => {
  it("没人连时 push 仍收下，但如实说 delivered=false", async () => {
    const ctl = fresh()
    const result = ctl.push({ kind: "notice", title: "t" })
    // 关键：**不抛错**。抛错会让调用方重试 → 同一件事推两张卡
    expect(result.delivered).toBe(false)
    expect(result.id).not.toBe("")
    ctl.close()
  })

  it("相期连上后把攒下的卡重推", async () => {
    const ctl = fresh()
    ctl.push({ kind: "notice", title: "断线时攒的" })
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => peer.lastCards().length === 1, 2000, "连上即重推")
    expect(peer.lastCards()[0]!.title).toBe("断线时攒的")
    peer.close()
    ctl.close()
  })

  it("相期断开重连后仍会整批重推", async () => {
    const ctl = fresh()
    const first = await FakeXiangqi.connect(ctl.url())
    ctl.push({ kind: "notice", title: "一" })
    await until(() => first.lastCards().length === 1, 2000, "首推")
    first.close()
    await until(() => !ctl.isConnected(), 2000, "断开")

    // 窗口重启（旧连接没了、那边是空的）
    const second = await FakeXiangqi.connect(ctl.url())
    await until(() => second.lastCards().length === 1, 2000, "重连后重推")
    second.close()
    ctl.close()
  })

  it("新窗口取代旧窗口（只认一个）", async () => {
    const ctl = fresh()
    const first = await FakeXiangqi.connect(ctl.url())
    ctl.push({ kind: "notice", title: "一" })
    await until(() => first.lastCards().length === 1, 2000, "首推")

    // 用户重启了相期：新连接应接管
    const second = await FakeXiangqi.connect(ctl.url())
    await until(() => second.lastCards().length === 1, 2000, "新窗口接管并拿到全部卡")
    expect(ctl.isConnected()).toBe(true)
    second.close()
    ctl.close()
  })
})

describe("控制器：答复", () => {
  it("答复经 WS 回来时唤醒等待者", async () => {
    const ctl = fresh()
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => ctl.isConnected(), 2000, "连上")

    const { id } = ctl.push({
      kind: "choice",
      title: "选",
      options: [{ label: "A", value: "a" }],
    })
    const waiting = ctl.waitForAnswer(id, 5000)
    peer.answer(id, "choice", { choice: "a" })
    const answer = await waiting
    expect(answer?.answer).toEqual({ choice: "a" })
    peer.close()
    ctl.close()
  })

  it("超时返回 null，但卡片**不从窗口消失**", async () => {
    const ctl = fresh()
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => ctl.isConnected(), 2000, "连上")

    const { id } = ctl.push({
      kind: "choice",
      title: "选",
      options: [{ label: "A", value: "a" }],
    })
    const answer = await ctl.waitForAnswer(id, 30)
    expect(answer).toBeNull()
    // 关键：仍在窗口上 → 用户回来还能答，答复仍会作为事件回来
    expect(peer.lastCards().some((card) => card.id === id)).toBe(true)
    peer.close()
    ctl.close()
  })
})

describe("工具：card_notice", () => {
  it("连上时推完立即返回", async () => {
    const ctl = fresh()
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => ctl.isConnected(), 2000, "连上")
    const tools = createTools({ controller: ctl })
    const result = await tools.card_notice!.execute({ title: "办完了" }, {} as never)
    expect(result.output).toContain("已推送")
    await until(() => peer.lastCards().length === 1, 2000, "推送到窗口")
    peer.close()
    ctl.close()
  })

  it("未连接时如实说**用户看不到**", async () => {
    const ctl = fresh()
    const tools = createTools({ controller: ctl })
    const result = await tools.card_notice!.execute({ title: "重要通知" }, {} as never)
    expect(result.output).toContain("用户看不到")
    expect(result.output).toContain("其他方式告知")
    ctl.close()
  })
})

describe("工具：card_ask", () => {
  it("choice 没 options 时抛错（那等于把用户逼进死路）", () => {
    const ctl = fresh()
    expect(() => ctl.push({ kind: "choice", title: "选" })).toThrow(/option/)
    ctl.close()
  })

  it("把 value 翻译成 label 再告诉模型", () => {
    const card = {
      kind: "choice" as const,
      title: "发哪个群",
      options: [
        { label: "支持群", value: "oc_support" },
        { label: "内部群", value: "oc_internal" },
      ],
    }
    const answer: CardAnswer = {
      id: "x",
      kind: "choice",
      answer: { choice: "oc_support" },
      answeredAtMs: 0,
    }
    const text = describeAnswer(card, answer)
    expect(text).toContain("支持群")
    expect(text).toContain("oc_support")
  })

  it("todo 的「完成了」与「先放着」分得清", () => {
    const card = { kind: "todo" as const, title: "加监控" }
    const done = describeAnswer(card, {
      id: "x", kind: "todo", answer: { done: true }, answeredAtMs: 0,
    })
    const snooze = describeAnswer(card, {
      id: "x", kind: "todo", answer: { snooze: true }, answeredAtMs: 0,
    })
    expect(done).toContain("完成了")
    expect(snooze).toContain("先放着")
    // 「先放着」不是忽略——这句必须让模型看出来
    expect(snooze).toContain("晚点做")
  })

  it("超时的话必须说明卡片还在窗口上", async () => {
    const ctl = fresh()
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => ctl.isConnected(), 2000, "连上")
    const tools = createTools({ controller: ctl })
    const result = await tools.card_ask!.execute(
      { kind: "choice", title: "选", options: [{ label: "A", value: "a" }], wait_ms: 30 },
      {} as never,
    )
    expect(result.output).toContain("仍留在用户窗口")
    expect(result.output).toContain("随时可能处理")
    const data = result.data as { answered: boolean; timedOut: boolean }
    expect(data.answered).toBe(false)
    expect(data.timedOut).toBe(true)
    peer.close()
    ctl.close()
  })

  it("送不到时**直接返回、不白等**", async () => {
    const ctl = fresh()
    const tools = createTools({ controller: ctl })
    const started = Date.now()
    const result = await tools.card_ask!.execute(
      // 即使要等 30 秒，未连接时也该立刻返回
      { kind: "choice", title: "选", options: [{ label: "A", value: "a" }], wait_ms: 30_000 },
      {} as never,
    )
    expect(Date.now() - started).toBeLessThan(1000)   // 没有真的等
    expect(result.output).toContain("用户看不到")
    expect(result.output).toContain("等下去没意义")
    ctl.close()
  })

  it("时限常量有界（用户去开会了，模型不该永远挂着）", () => {
    expect(MAX_WAIT_MS).toBeGreaterThan(0)
    expect(MAX_WAIT_MS).toBeLessThanOrEqual(60 * 60 * 1000)
    expect(DEFAULT_WAIT_MS).toBeLessThanOrEqual(MAX_WAIT_MS)
  })
})

describe("工具：card_list / card_dismiss", () => {
  it("未连接时如实说无法确认，而不是用空列表冒充没有待办", async () => {
    const ctl = fresh()
    const tools = createTools({ controller: ctl })
    const result = await tools.card_list!.execute({}, {} as never)
    expect(result.output).toContain("未连接")
    expect(result.output).toContain("无法确认")
    ctl.close()
  })

  it("有卡在等时列得出来", async () => {
    const ctl = fresh()
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => ctl.isConnected(), 2000, "连上")
    const tools = createTools({ controller: ctl })
    const { id } = ctl.push({
      kind: "choice",
      title: "发哪个群",
      options: [{ label: "A", value: "a" }],
    })
    // 时限要短：留一个长 timer 会把 `bun test` 整轮拖住（已踩过）
    const waiting = ctl.waitForAnswer(id, 50)
    const result = await tools.card_list!.execute({}, {} as never)
    expect(result.output).toContain("发哪个群")
    expect(result.output).toContain("已连接")
    await waiting
    peer.close()
    ctl.close()
  })

  it("dismiss 后窗口上是空列表", async () => {
    const ctl = fresh()
    const peer = await FakeXiangqi.connect(ctl.url())
    ctl.push({ kind: "notice", title: "一" })
    await until(() => peer.lastCards().length === 1, 2000, "首推")
    ctl.dismissAll()
    await until(() => peer.lastCards().length === 0, 2000, "清空")
    peer.close()
    ctl.close()
  })
})

describe("文件通路：inbox 降级推卡", () => {
  it("未连接时把卡写进 inbox（相期下次启动能看到）", () => {
    const home = mkdtempSync(join(tmpdir(), "xq-inbox-"))
    const ctl = freshWithHome(home)
    const result = ctl.push({ kind: "notice", title: "断线时也要送到" })
    // 仍如实说“此刻看不到”
    expect(result.delivered).toBe(false)
    // 但文件已经落地了
    const file = join(home, "inbox", `${result.id}.json`)
    expect(existsSync(file)).toBe(true)
    const written = JSON.parse(readFileSync(file, "utf-8")) as { title?: string; kind?: string }
    expect(written.title).toBe("断线时也要送到")
    expect(written.kind).toBe("notice")
    ctl.close()
    rmSync(home, { recursive: true, force: true })
  })

  it("WS 通了就不写 inbox（避免同一张卡出现两次）", async () => {
    const home = mkdtempSync(join(tmpdir(), "xq-inbox2-"))
    const ctl = freshWithHome(home)
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => ctl.isConnected(), 2000, "连上")
    ctl.push({ kind: "notice", title: "走 WS" })
    await until(() => peer.lastCards().length === 1, 2000, "推送")
    // inbox 里不该有文件（WS 是主路，文件是降级路）
    const inbox = join(home, "inbox")
    expect(existsSync(inbox) ? readdirSync(inbox).length : 0).toBe(0)
    peer.close()
    ctl.close()
    rmSync(home, { recursive: true, force: true })
  })
})

describe("文件通路：outbox 兼底取答复", () => {
  it("WS 断了但答复已落盘时，轮询能把它捡回来", async () => {
    const home = mkdtempSync(join(tmpdir(), "xq-outbox-"))
    mkdirSync(join(home, "outbox"), { recursive: true })
    const ctl = freshWithHome(home)

    // 未连接也得先推一张（否则控制器不知道这个 id）
    const asked = ctl.push({
      kind: "choice",
      title: "选",
      options: [{ label: "A", value: "a" }],
    })

    // 相期点了按钮，但 WS 恰好在那一刻断了 → 只落了文件
    writeFileSync(
      join(home, "outbox", `${asked.id}.json`),
      JSON.stringify({ type: "answer", id: asked.id, kind: "choice", answer: { choice: "a" } }),
      "utf-8",
    )

    const answer = await ctl.waitForAnswer(asked.id, 2000)
    expect(answer).not.toBeNull()
    expect(answer!.answer).toEqual({ choice: "a" })
    ctl.close()
    rmSync(home, { recursive: true, force: true })
  })

  it("答复先到、后来才等 → 不白等整个超时", async () => {
    const home = mkdtempSync(join(tmpdir(), "xq-early-"))
    mkdirSync(join(home, "outbox"), { recursive: true })
    const ctl = freshWithHome(home)
    const asked = ctl.push({
      kind: "choice",
      title: "选",
      options: [{ label: "A", value: "a" }],
    })
    writeFileSync(
      join(home, "outbox", `${asked.id}.json`),
      JSON.stringify({ type: "answer", id: asked.id, kind: "choice", answer: { choice: "a" } }),
      "utf-8",
    )

    const started = Date.now()
    const answer = await ctl.waitForAnswer(asked.id, 5000)
    const elapsed = Date.now() - started
    // 关键：命中“已先到”的缓存，而不是等满 5 秒
    expect(elapsed).toBeLessThan(1000)
    expect(answer!.answer).toEqual({ choice: "a" })
    ctl.close()
    rmSync(home, { recursive: true, force: true })
  })

  it("陈旧 outbox 文件不会乱投（上个进程留下的）", async () => {
    const home = mkdtempSync(join(tmpdir(), "xq-stale-"))
    mkdirSync(join(home, "outbox"), { recursive: true })
    // 造一个我们根本不认识的 id
    writeFileSync(
      join(home, "outbox", "card-unknown.json"),
      JSON.stringify({ type: "answer", id: "card-unknown", kind: "notice", answer: {} }),
      "utf-8",
    )
    const ctl = freshWithHome(home)
    const asked = ctl.push({
      kind: "choice",
      title: "选",
      options: [{ label: "A", value: "a" }],
    })
    // 不认识的那条不该把我们这张卡弄成“已答复”
    const answer = await ctl.waitForAnswer(asked.id, 200)
    expect(answer).toBeNull()
    ctl.close()
    rmSync(home, { recursive: true, force: true })
  })

  it("坏文件不被标记已消费（半写状态下不永久丢件）", async () => {
    const home = mkdtempSync(join(tmpdir(), "xq-broken-"))
    mkdirSync(join(home, "outbox"), { recursive: true })
    const ctl = freshWithHome(home)
    const asked = ctl.push({
      kind: "choice",
      title: "选",
      options: [{ label: "A", value: "a" }],
    })
    const file = join(home, "outbox", `${asked.id}.json`)
    // 先写个半截的（模拟正在写）
    writeFileSync(file, '{"type":"answer","id":"' + asked.id, "utf-8")
    // 等两轮轮询：坏文件应当被跳过
    await new Promise((resolve) => setTimeout(resolve, 80))
    // 补全它 → 下一轮应当能捡到
    writeFileSync(
      file,
      JSON.stringify({ type: "answer", id: asked.id, kind: "choice", answer: { choice: "a" } }),
      "utf-8",
    )
    const answer = await ctl.waitForAnswer(asked.id, 2000)
    expect(answer).not.toBeNull()
    ctl.close()
    rmSync(home, { recursive: true, force: true })
  })
})

describe("单例：连接必须宿主唯一", () => {
  it("两次 controller() 拿到同一个实例", () => {
    resetControllerForTest()
    const first = controller({ port: 0, homeDir: mkdtempSync(join(tmpdir(), "xq-single-")) })
    const second = controller()
    // 同一个对象 → 不会有两个会话各占一个端口抢窗口
    expect(first).toBe(second)

    // 顺带钉住一个踩过的坑：`createTools()` **不能在构造时就抓好单例**。
    // 以前它是 `const ctl = deps.controller ?? controller()`，而 `xiangqi.ts`
    // 顶层就调 `createTools()`——于是单例一出生就绑定了当时的 cwd，
    // 把卡片文件写进仓库目录（实测：跑完测试仓库里多了 相期/inbox/）。
    resetControllerForTest()
    const tools = createTools()   // 自己不传控制器 → 走单例
    const fresh = controller({ port: 0, homeDir: mkdtempSync(join(tmpdir(), "xq-lazy-")) })
    expect(fresh.isConnected()).toBe(false)
    // 上面这一行能跑就说明：`createTools()` 没把“旧的那个”锁死
    expect(tools.card_notice).toBeDefined()
    resetControllerForTest()
  })
})


describe("单卡撤回：授权完成后让那张卡自动消失", () => {
  it("只撤指定的一张，不影响其他还在等的卡", async () => {
    const ctl = fresh()
    await ctl.listen()
    // 先推再连：连上的瞬间会整批重推 `live` 里已有的卡
    const auth = ctl.push({ kind: "notice", title: "去授权" })
    const other = ctl.push({ kind: "todo", title: "跟进磁盘告警" })
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => peer.lastCards().length === 2, 2000, "两张都在")

    const ok = ctl.dismiss(auth.id)
    expect(ok).toBe(true)
    await until(() => peer.lastCards().length === 1, 2000, "撤掉一张")
    // 关键：另一张仍在（不能因为撤一张把别人的也清了）
    console.log("    实测剩余:", peer.lastCards().map((c) => (c as { title?: string }).title).join(","))
    expect(peer.lastCards()[0]!.id).toBe(other.id)
    peer.close()
  })

  it("撤不存在的 id 返回 false（不抛）", () => {
    const ctl = fresh()
    expect(ctl.dismiss("card-nope")).toBe(false)
  })

  it("撤回仍在等待的卡 → 唤醒等待者（返回 null，不白等到超时）", async () => {
    const ctl = fresh()
    await ctl.listen()
    const { id } = ctl.push({ kind: "choice", title: "等答复", options: [{ label: "A", value: "a" }] })
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => peer.lastCards().length === 1, 2000, "卡片到达")

    const startedAt = Date.now()
    const waiting = ctl.waitForAnswer(id, 30_000)
    await new Promise((r) => setTimeout(r, 30)) // 让 waitForAnswer 真正进入等待
    ctl.dismiss(id)
    const answer = await waiting
    const elapsed = Date.now() - startedAt
    console.log(`    实测: 撤回后 ${elapsed}ms 返回 ${answer === null ? "null" : "答复"}`)
    expect(answer).toBeNull()
    expect(elapsed).toBeLessThan(5000) // 被唤醒而不是等满 30 秒
    peer.close()
  })
})

describe("链接按钮：授权卡要能把用户送到浏览器", () => {
  it("带 url 的选项原样下发（相期渲染为可点击链接）", async () => {
    const ctl = fresh()
    await ctl.listen()
    const url = "https://accounts.feishu.cn/open-apis/authen/v1/authorize?client_id=x"
    ctl.push({
      kind: "choice",
      title: "需要飞书授权",
      options: [{ label: "去授权", value: "open", url }],
    })
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => peer.lastCards().length === 1, 2000, "授权卡到达")

    const card = peer.lastCards()[0] as unknown as { options?: Array<{ label: string; value: string; url?: string }> }
    console.log("    实测下发选项:", JSON.stringify(card?.options))
    expect(card?.options?.[0]?.url).toBe(url)
    expect(card?.options?.[0]?.label).toBe("去授权")
    peer.close()
  })
})

describe("TTL：过期卡片自动消失（链接已失效就别留着误导用户）", () => {
  it("expiresAtMs 到期后下次下发不再包含该卡", async () => {
    const ctl = fresh()
    await ctl.listen()
    const shortLived = ctl.push({ kind: "notice", title: "10 分钟有效的链接", expiresAtMs: Date.now() + 60 })
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => peer.lastCards().some((c) => c.id === shortLived.id), 2000, "卡片到达")

    await new Promise((r) => setTimeout(r, 120)) // 等它过期
    ctl.push({ kind: "notice", title: "新卡" }) // 触发一次下发（顺带做过期剔除）
    await until(() => !peer.lastCards().some((c) => c.id === shortLived.id), 2000, "过期卡被剔除")
    console.log("    实测过期后剩余:", peer.lastCards().map((c) => (c as { title?: string }).title).join(","))
    expect(peer.lastCards().some((c) => c.id === shortLived.id)).toBe(false)
    peer.close()
  })

  it("不设 expiresAtMs 的卡永不过期（普通待办不该自己消失）", async () => {
    const ctl = fresh()
    await ctl.listen()
    const forever = ctl.push({ kind: "todo", title: "一直要记着的事" })
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => peer.lastCards().length === 1, 2000, "卡片到达")

    await new Promise((r) => setTimeout(r, 120))
    ctl.push({ kind: "notice", title: "新卡" })
    await until(() => peer.lastCards().length >= 2, 2000, "两张都在")
    expect(peer.lastCards().some((c) => c.id === forever.id)).toBe(true)
    peer.close()
  })
})

describe("card_authorize 工具：授权卡不阻塞", () => {
  it("推完立即返回（不等授权结果），并给出 card id 供完成后撤卡", async () => {
    const ctl = fresh()
    const tools = createTools({ controller: ctl })
    await ctl.listen()
    const peer = await FakeXiangqi.connect(ctl.url())

    const started = Date.now()
    const res = await tools.card_authorize.execute!(
      { title: "需要飞书授权", url: "https://accounts.feishu.cn/x", body: "人格要回消息就得先授权" },
      {} as never,
    )
    const elapsed = Date.now() - started
    console.log(`    实测: 耗时=${elapsed}ms`)
    expect(elapsed).toBeLessThan(1000) // 不阻塞
    expect(String(res.output)).toContain("card_dismiss") // 告诉模型完成后要撤卡
    const data = res.data as { id?: string; url?: string }
    expect(data.url).toBe("https://accounts.feishu.cn/x")
    expect(typeof data.id).toBe("string")
    peer.close()
  })

  it("相期未连接 → 如实告知送不到，并把链接直接给出来", async () => {
    const ctl = fresh() // 不 listen、不连 peer
    const tools = createTools({ controller: ctl })
    const res = await tools.card_authorize.execute!({ title: "需要授权", url: "https://x.test/a" }, {} as never)
    const out = String(res.output)
    console.log(`    实测: ${out.slice(0, 70)}`)
    expect(out).toContain("看不到")
    expect(out).toContain("https://x.test/a") // 链接必须给出来，否则用户没法自救
  })

  it("拒绝非法 url（http(s) 之外一律拒）", async () => {
    const ctl = fresh()
    const tools = createTools({ controller: ctl })
    await expect(tools.card_authorize.execute!({ title: "t", url: "javascript:alert(1)" }, {} as never)).rejects.toThrow(/http/)
  })
})

describe("card_dismiss 支持单卡撤回", () => {
  it("传 id 只撤那张", async () => {
    const ctl = fresh()
    const tools = createTools({ controller: ctl })
    await ctl.listen()
    const a = ctl.push({ kind: "notice", title: "A" })
    const b = ctl.push({ kind: "notice", title: "B" })
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => peer.lastCards().length === 2, 2000, "两张都在")

    const res = await tools.card_dismiss.execute!({ id: a.id }, {} as never)
    expect(String(res.output)).toContain(a.id)
    await until(() => peer.lastCards().length === 1, 2000, "撤掉 A")
    expect(peer.lastCards()[0]!.id).toBe(b.id)
    peer.close()
  })

  it("不传 id 仍清全部（向后兼容）", async () => {
    const ctl = fresh()
    const tools = createTools({ controller: ctl })
    await ctl.listen()
    ctl.push({ kind: "notice", title: "A" })
    ctl.push({ kind: "notice", title: "B" })
    const peer = await FakeXiangqi.connect(ctl.url())
    await until(() => peer.lastCards().length === 2, 2000, "两张都在")
    await tools.card_dismiss.execute!({}, {} as never)
    await until(() => peer.lastCards().length === 0, 2000, "全清")
    expect(peer.lastCards().length).toBe(0)
    peer.close()
  })
})

describe("数据根：缺省不写到 cwd（防污染仓库）", () => {
  it("设了 GEBAI_HOME 时，箱件落在 {GEBAI_HOME}/相期", async () => {
    const home = mkdtempSync(join(tmpdir(), "xq-gh-"))
    const prev = process.env.GEBAI_HOME
    process.env.GEBAI_HOME = home
    try {
      // 不给 homeDir：走环境的缺省解析
      const ctl = createController({ port: 0 })
      await ctl.listen()
      const peer = await FakeXiangqi.connect(ctl.url())
      // 先断开再推，道破 WS 通路使卡走文件兼底
      ctl.dropPeerForTest()
      ctl.push({ kind: "notice", title: "需要写盘的卡" })
      void peer
      await new Promise((r) => setTimeout(r, 150))
      const wroteToGebaiHome = existsSync(join(home, "相期", "inbox"))
      const wroteToCwd = existsSync(join(process.cwd(), "相期"))
      console.log(`    实测: GEBAI_HOME下=${wroteToGebaiHome} cwd下=${wroteToCwd}`)
      expect(wroteToGebaiHome).toBe(true)
      expect(wroteToCwd).toBe(false)
      ctl.close()
    } finally {
      if (prev === undefined) delete process.env.GEBAI_HOME
      else process.env.GEBAI_HOME = prev
      rmSync(home, { recursive: true, force: true })
    }
  })

  it("XIANGQI_HOME 优先于 GEBAI_HOME（显式指定的最优先）", async () => {
    const explicit = mkdtempSync(join(tmpdir(), "xq-exp-"))
    const home = mkdtempSync(join(tmpdir(), "xq-gh2-"))
    const prevHome = process.env.GEBAI_HOME
    const prevX = process.env.XIANGQI_HOME
    process.env.GEBAI_HOME = home
    process.env.XIANGQI_HOME = explicit
    try {
      const ctl = createController({ port: 0 })
      await ctl.listen()
      ctl.dropPeerForTest()
      ctl.push({ kind: "notice", title: "落显式目录" })
      await new Promise((r) => setTimeout(r, 150))
      console.log(`    实测: 显式下=${existsSync(join(explicit, "inbox"))} GEBAI_HOME下=${existsSync(join(home, "相期", "inbox"))}`)
      expect(existsSync(join(explicit, "inbox"))).toBe(true)
      expect(existsSync(join(home, "相期", "inbox"))).toBe(false)
      ctl.close()
    } finally {
      if (prevHome === undefined) delete process.env.GEBAI_HOME
      else process.env.GEBAI_HOME = prevHome
      if (prevX === undefined) delete process.env.XIANGQI_HOME
      else process.env.XIANGQI_HOME = prevX
      rmSync(explicit, { recursive: true, force: true })
      rmSync(home, { recursive: true, force: true })
    }
  })
})
