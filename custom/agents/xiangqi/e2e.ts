/**
 * 相期子Agent 的**端到端**验证：真启动相期窗口，走真 WebSocket。
 *
 * 单元测试里“相期”是我用 WS 客户端冒充的；这里换成**真的相期进程**，
 * 验证的是与 `examples/xiangqi/main.cpp` 的协议真能对上——桩能测语义，
 * 测不了「对端到底认不认这个报文」。
 *
 * 关键设计：**用户答复走真实路径**（经相期的控制通道点按钮），
 * 而不是从服务端伪造一条 answer 蒙混过关。
 *
 * 用法：
 *   bun run custom/agents/xiangqi/e2e.ts <xiangqi.exe 路径>
 */

import { spawn } from "node:child_process"
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

import { createController } from "./controller"

const exe = process.argv[2]
if (exe === undefined) {
  console.error("用法：bun run custom/agents/xiangqi/e2e.ts <xiangqi.exe 路径>")
  process.exit(2)
}

const home = mkdtempSync(join(tmpdir(), "xiangqi-e2e-"))
let failures = 0

function check(ok: boolean, label: string, detail = ""): void {
  if (ok) {
    console.log(`  PASS ${label}`)
  } else {
    failures += 1
    console.log(`  FAIL ${label}${detail === "" ? "" : ` — ${detail}`}`)
  }
}

/** 等直到条件成立（轮询），超时抛。 */
async function until(condition: () => boolean, ms = 8000, label = "条件"): Promise<void> {
  const deadline = Date.now() + ms
  while (Date.now() < deadline) {
    if (condition()) return
    await new Promise((resolve) => setTimeout(resolve, 50))
  }
  throw new Error(`等待超时：${label}`)
}

/**
 * 相期控制通道的最小客户端（长度前缀 JSON，与 `tools/xiangqi_e2e.py` 同协议）。
 *
 * 注：Bun 的 `Socket` 是**回调式**（`socket.data` 里拿到块），没有 `.read()`。
 * 所以这里用“缓冲区 + 挂起等待者”的模式自己拼帧。
 */
class ControlClient {
  private buffer = new Uint8Array(0)
  private pending: ((payload: Record<string, unknown>) => void) | null = null
  private broken = false

  private constructor(private socket: import("bun").Socket) {}

  static async connect(port: number, token: string): Promise<ControlClient> {
    let client: ControlClient | null = null
    const socket = await Bun.connect({
      hostname: "127.0.0.1",
      port,
      socket: {
        data(_socket, chunk) {
          client?.feed(new Uint8Array(chunk))
        },
        close() {
          client?.markBroken()
        },
      },
    })
    client = new ControlClient(socket)
    await client.call("hello", { token })
    return client
  }

  private markBroken(): void {
    this.broken = true
    this.pending?.({ ok: false, error: { message: "控制通道断开" } })
    this.pending = null
  }

  private feed(chunk: Uint8Array): void {
    const merged = new Uint8Array(this.buffer.length + chunk.length)
    merged.set(this.buffer)
    merged.set(chunk, this.buffer.length)
    this.buffer = merged
    this.tryComplete()
  }

  private tryComplete(): void {
    if (this.pending === null) return
    if (this.buffer.length < 4) return
    const length = new DataView(this.buffer.buffer, this.buffer.byteOffset, 4).getUint32(0, false)
    if (this.buffer.length < 4 + length) return
    const body = this.buffer.subarray(4, 4 + length)
    this.buffer = this.buffer.subarray(4 + length)
    const resolve = this.pending
    this.pending = null
    try {
      resolve(JSON.parse(new TextDecoder().decode(body)) as Record<string, unknown>)
    } catch {
      resolve({ ok: false, error: { message: "控制通道返回了非法 JSON" } })
    }
  }

  private async call(method: string, params: Record<string, unknown>): Promise<Record<string, unknown>> {
    if (this.broken) throw new Error("控制通道已断开")
    const body = new TextEncoder().encode(JSON.stringify({ method, params }))
    const frame = new Uint8Array(4 + body.length)
    new DataView(frame.buffer).setUint32(0, body.length, false)
    frame.set(body, 4)
    const reply = new Promise<Record<string, unknown>>((resolve, reject) => {
      this.pending = resolve
      this.socket.write(frame)
      setTimeout(() => {
        if (this.pending === resolve) {
          this.pending = null
          reject(new Error(`${method} 超时（控制通道无响应）`))
        }
      }, 10_000)
    })
    this.tryComplete()
    return await reply
  }

  /** 控制通道的回包是 `{id, ok, result}` 包了一层的。 */
  async ok(method: string, params: Record<string, unknown>): Promise<Record<string, unknown>> {
    const reply = await this.call(method, params)
    if (reply.ok !== true) throw new Error(`${method} 失败：${JSON.stringify(reply.error ?? reply)}`)
    return (reply.result ?? {}) as Record<string, unknown>
  }

  /** 便利：某选择器命中几个。 */
  async count(selector: string): Promise<number> {
    const result = await this.ok("find", { selector })
    return Number(result.count ?? 0)
  }

  close(): void {
    this.socket.end()
  }
}

async function main(): Promise<void> {
  // 控制器是**服务端**（相期连过来）。
  // `homeDir` 必须与下发给相期的 `--home` 一致——否则文件兼底两边各写各的目录。
  const appHome = join(home, "app")
  const ctl = createController({ port: 0, homeDir: appHome, outboxPollMs: 100 })
  const url = ctl.url()
  console.log(`相期 e2e：控制器监听 ${url}`)

  const ctlFile = join(home, "ctl.json")
  const appLog: string[] = []
  const app = spawn(exe, [
    "--headless",
    "--control-port", "0",
    "--control-file", ctlFile,
    "--shots", home,
    "--home", appHome,
    "--ui", join(import.meta.dir, "..", "..", "..", "shuangtian", "examples", "xiangqi", "ui"),
    "--agent", url,
  ])
  app.stdout?.on("data", (chunk: Uint8Array) => appLog.push(String(chunk)))
  app.stderr?.on("data", (chunk: Uint8Array) => appLog.push(String(chunk)))

  let control: ControlClient | null = null

  try {
    // ① 相期连上来
    await until(() => ctl.isConnected(), 15_000, "相期连上控制器")
    check(true, "相期连上控制器")

    // ② 控制通道就绪
    await until(() => existsSync(ctlFile), 15_000, "控制通道文件生成")
    for (let attempt = 0; attempt < 100 && control === null; attempt += 1) {
      try {
        const info = JSON.parse(readFileSync(ctlFile, "utf-8")) as { port?: number; token?: string }
        if (info.port !== undefined && info.port > 0) {
          control = await ControlClient.connect(info.port, info.token ?? "")
        }
      } catch {
        await new Promise((resolve) => setTimeout(resolve, 100))
      }
    }
    check(control !== null, "控制通道就绪")
    if (control === null) throw new Error("控制通道连不上，后续无法验证")
    const ui = control

    // ③ notice：协议通不通
    //
    // ⚠ 断言不能只查 `#card-0`：**只有一张 notice 时相期进入轻提示形态**
    // （`#compact` 显示、主卡片区隐藏）——这是「不必每次弹出主界面」的
    // 核心体验，不是缺陷。两种形态任一命中即可。
    const notice = ctl.push({
      kind: "notice",
      title: "端到端验证",
      body: "**markdown** 正文\n\n- 列表项\n- `代码`",
      from: "e2e",
    })
    check(notice.delivered, "notice 推送送达（控制器→相期 WS）")
    let noticeRendered = false
    for (let attempt = 0; attempt < 80 && !noticeRendered; attempt += 1) {
      if ((await ui.count("#card-0")) > 0) {
        noticeRendered = true
      } else {
        const compact = await ui.ok("get", { id: "compact-title" })
        const text = String(compact.value ?? compact.text ?? "")
        noticeRendered = text.includes("端到端验证")
      }
      if (!noticeRendered) await new Promise((resolve) => setTimeout(resolve, 100))
    }
    check(noticeRendered, "notice 已渲染（轻提示或主界面）")

    // ④ choice：走真实路径等答复
    const asked = ctl.push({
      kind: "choice",
      title: "选一个",
      body: "端到端验证：请在桌面上点一下",
      options: [
        { label: "选项甲", value: "a" },
        { label: "选项乙", value: "b" },
      ],
    })
    check(asked.delivered, "choice 推送送达")
    // 选择卡是第二张 → index 1
    let buttonReady = false
    for (let attempt = 0; attempt < 80 && !buttonReady; attempt += 1) {
      buttonReady = (await ui.count("#card-1-act-opt-0")) > 0
      if (!buttonReady) await new Promise((resolve) => setTimeout(resolve, 100))
    }
    check(buttonReady, "选择卡的两个选项按钮已出现")

    // 用户真的点一下（经控制通道 invoke，与相期自己的 e2e 同一手法）
    const waiting = ctl.waitForAnswer(asked.id, 8000)
    await ui.ok("invoke", { id: "card-1-act-opt-0", action: "click" })

    const answer = await waiting
    check(answer !== null, "用户点击后收到答复（相期→WS→控制器）")
    check(
      JSON.stringify(answer?.answer ?? {}).includes("a"),
      "答复内容正确",
      JSON.stringify(answer?.answer),
    )

    // ⑤ 撤回
    ctl.dismissAll()
    let cleared = false
    for (let attempt = 0; attempt < 80 && !cleared; attempt += 1) {
      cleared = (await ui.count("#card-0")) === 0
      if (!cleared) await new Promise((resolve) => setTimeout(resolve, 100))
    }
    check(cleared, "dismissAll 清空了窗口上的卡片")

    // ── 场景 B：WS 断在点击那一刻——outbox 兼底接力 ──────────────────
    //
    // 这才是「文件通路」真正救命的场景：用户点了按钮，但实时链路不给力。
    // 相期的设计是“答复总是落 outbox”，于是控制器扫文件就能捡回来。
    // 不验这一条的话，“超时不丢答复”只是一句口号。
    const offline = ctl.push({
      kind: "choice",
      title: "断线也要能回答",
      options: [
        { label: "选它", value: "offline-a" },
        { label: "不选", value: "offline-b" },
      ],
    })
    // 先确保相期渲染出来了（此时 WS 还通）
    let offlineReady = false
    for (let attempt = 0; attempt < 80 && !offlineReady; attempt += 1) {
      offlineReady = (await ui.count("#card-0-act-opt-0")) > 0
      if (!offlineReady) await new Promise((resolve) => setTimeout(resolve, 100))
    }
    check(offlineReady, "断线前的卡片已渲染")

    // 现在把 WS 断掉（相期仍活着、卡片仍在它窗口上）
    ctl.dropPeerForTest()
    await new Promise((resolve) => setTimeout(resolve, 200))
    check(!ctl.isConnected(), "WS 已断开（而相期仍活着）")

    // 用户在这时点了按钮：实时路径已断，只剩文件
    const offlineWaiting = ctl.waitForAnswer(offline.id, 8000)
    await ui.ok("invoke", { id: "card-0-act-opt-0", action: "click" })

    const offlineAnswer = await offlineWaiting
    check(offlineAnswer !== null, "WS 断开时仍拿到了答复（靠 outbox 兼底）")
    check(
      JSON.stringify(offlineAnswer?.answer ?? {}).includes("offline-a"),
      "兼底答复内容正确",
      JSON.stringify(offlineAnswer?.answer),
    )

    // ⑦ 相期没被搞崩
    check(app.exitCode === null, "相期进程存活")
  } finally {
    if (failures > 0 && appLog.length > 0) {
      console.log("\n---- 相期侧日志（尾部）----")
      console.log(appLog.join("").split("\n").slice(-30).join("\n"))
    }
    control?.close()
    ctl.close()
    app.kill()
    await new Promise((resolve) => setTimeout(resolve, 300))
    rmSync(home, { recursive: true, force: true })
  }

  console.log(failures === 0 ? "\n全部通过" : `\n失败 ${failures} 项`)
  process.exit(failures === 0 ? 0 : 1)
}

await main()
