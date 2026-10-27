/**
 * `/xiangqi` 走主服务端口（方案 B）的**真实握手**用例。
 *
 * 相期是原生 C++ 客户端（自带 WS 实现，`shuangtian/examples/xiangqi`），握手请求里
 * **没有 Origin**。而主服务在 fetch 里对所有请求先过一道 `originAllowed`（跨站防护：
 * 恶意网页能直接连 `ws://127.0.0.1:*` 以本地身份执行命令）。这两件事放在一起理解错，
 * 就会以为"相期连不上是被 origin 挡了"——实测不是（无 Origin 按非浏览器客户端放行），
 * 但**这个结论必须有测试钉住**：将来谁把放行规则收紧，相期会静默变回"连不上"，
 * 而那时现象只会是"窗口显示未连接"，离真因很远。
 *
 * 同时钉住跨站那条线没被放宽：带外站 Origin 的 `/xiangqi` 仍必须 403。
 */
import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import net from "node:net"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { resetXiangqiControllerForTest, xiangqiController } from "@gebai/agents"
import { startServer, type ServerHandle } from "../index"

const home = mkdtempSync(join(tmpdir(), "gebai-xq-serve-"))
let handle: ServerHandle
let port = 0

beforeAll(async () => {
  process.env.GEBAI_HOME = home
  process.env.GEBAI_MODE = "local"
  process.env.GEBAI_SANDBOX = "off"
  // 相期 exe 不在这台机器上也能测：本条只验证「升级 + 桥到控制器」，拉起与否由另一条线覆盖
  process.env.XIANGQI_AUTO = "off"
  handle = await startServer({
    gebaiHome: home,
    auth: "local",
    sandbox: "off",
    binaryMode: false,
    preloadSubAgents: [],
    personaEnabled: false,
    port: 0,
  })
  port = handle.server.port ?? 0
})

afterAll(() => {
  resetXiangqiControllerForTest()
  handle.gc?.stop()
  handle.server.stop(true)
  delete process.env.GEBAI_HOME
  delete process.env.GEBAI_AUTO
  delete process.env.XIANGQI_AUTO
  rmSync(home, { recursive: true, force: true })
})

/** 发一个 WS 握手请求，返回状态行 + socket（保活以便观察控制器 peer 状态）。 */
function wsHandshake(path: string, origin?: string): Promise<{ status: number; socket: net.Socket | null }> {
  return new Promise((resolve) => {
    const socket = net.connect(port, "127.0.0.1", () => {
      const lines = [
        `GET ${path} HTTP/1.1`,
        `Host: 127.0.0.1:${port}`,
        "Upgrade: websocket",
        "Connection: Upgrade",
        // 与相期 C++ 客户端一致：不带 Origin / Sec-WebSocket-Protocol
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==",
        "Sec-WebSocket-Version: 13",
      ]
      if (origin !== undefined) lines.push(`Origin: ${origin}`)
      socket.write(lines.join("\r\n") + "\r\n\r\n")
    })
    let buf = ""
    let settled = false
    const finish = (status: number): void => {
      if (settled) return
      settled = true
      // 101 保活（否则控制器立刻收到 close peer），其余立刻断开
      if (status !== 101) socket.destroy()
      resolve({ status, socket: status === 101 ? socket : null })
    }
    socket.on("data", (chunk) => {
      buf += chunk.toString("latin1")
      const m = /^HTTP\/1\.1 (\d{3})/.exec(buf)
      if (m) finish(Number(m[1]))
    })
    socket.on("error", () => finish(0))
    socket.on("close", () => finish(0))
  })
}

describe("/xiangqi 走主服务端口（方案 B）", () => {
  test("原生客户端（无 Origin）握手成功 101，且连接被交给控制器（peer 已连）", async () => {
    const ctl = xiangqiController()
    expect(ctl.isConnected()).toBe(false)
    const { status, socket } = await wsHandshake("/xiangqi")
    expect(status).toBe(101)
    // open 生命周期在服务端异步触发：等一小会再看控制器状态
    for (let i = 0; i < 50 && !ctl.isConnected(); i++) await Bun.sleep(10)
    expect(ctl.isConnected()).toBe(true)
    socket?.destroy()
    for (let i = 0; i < 50 && ctl.isConnected(); i++) await Bun.sleep(10)
    // 关窗后控制器要把 peer 清掉（否则下一次窗口连上来前 isConnected 会一直骗人）
    expect(ctl.isConnected()).toBe(false)
  })

  test("跨站 Origin 仍被挡（403）：给相期放行不等于给跨站网页放行", async () => {
    const { status } = await wsHandshake("/xiangqi", "http://evil.example")
    expect(status).toBe(403)
    expect(xiangqiController().isConnected()).toBe(false)
  })

  test("同源 Origin 也放行（浏览器里手点联调时不该被拦）", async () => {
    const { status, socket } = await wsHandshake("/xiangqi", `http://127.0.0.1:${port}`)
    expect(status).toBe(101)
    socket?.destroy()
  })
})
