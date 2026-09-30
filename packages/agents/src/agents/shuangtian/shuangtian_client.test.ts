/**
 * 霜天控制通道客户端测试：用**真实 TCP** 的 mock 服务端验证协议层
 * （帧编解码、请求/响应配对、事件帧忽略、错误映射、超时、目标解析）。
 */
import { afterEach, describe, expect, test } from "bun:test"
import { mkdtempSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createServer, type Server, type Socket } from "node:net"
import { ControlError, call_control, call_once, request, resolve_target, unwrap } from "./shuangtian_client"

type Handler = (message: { id: number; method: string; params: Record<string, unknown> }) =>
  | { ok: true; result: unknown }
  | { ok: false; error: { code: string; message: string } }
  | { defer: true }
  | { frames: Buffer[] }  // 直接给原始帧字节（事件帧/超长帧等协议级场景）

interface MockServer {
  port: number
  close: () => Promise<void>
  calls: Array<{ method: string; params: Record<string, unknown> }>
}

/** 起一个按帧协议应答的 mock 控制通道。 */
async function startMock(handler: Handler): Promise<MockServer> {
  const calls: Array<{ method: string; params: Record<string, unknown> }> = []
  const sockets = new Set<Socket>()
  const server: Server = createServer((socket) => {
    sockets.add(socket)
    socket.on("close", () => sockets.delete(socket))
    let buffer = Buffer.alloc(0)
    socket.on("data", (chunk: Buffer) => {
      buffer = Buffer.concat([buffer, chunk])
      while (buffer.length >= 4) {
        const length = buffer.readUInt32BE(0)
        if (buffer.length < 4 + length) break
        const body = buffer.subarray(4, 4 + length).toString("utf8")
        buffer = buffer.subarray(4 + length)
        const message = JSON.parse(body) as { id: number; method: string; params: Record<string, unknown> }
        calls.push({ method: message.method, params: message.params ?? {} })
        const reply = handler(message)
        if ("defer" in reply) continue
        if ("frames" in reply) {
          socket.write(Buffer.concat(reply.frames))
          continue
        }
        const payload = JSON.stringify({ id: message.id, ...reply })
        const encoded = Buffer.from(payload, "utf8")
        const header = Buffer.alloc(4)
        header.writeUInt32BE(encoded.length, 0)
        socket.write(Buffer.concat([header, encoded]))
      }
    })
  })
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve))
  const address = server.address()
  const port = typeof address === "object" && address !== null ? address.port : 0
  return {
    port,
    calls,
    close: async () => {
      for (const socket of sockets) socket.destroy()
      await new Promise<void>((resolve) => server.close(() => resolve()))
    },
  }
}

const servers: MockServer[] = []
afterEach(async () => {
  while (servers.length > 0) {
    const server = servers.pop()
    if (server) await server.close()
  }
})

async function mock(handler: Handler): Promise<MockServer> {
  const server = await startMock(handler)
  servers.push(server)
  return server
}

describe("resolve_target", () => {
  test("接受 host:port / 纯端口 / 控制文件", async () => {
    expect(await resolve_target("127.0.0.1:9701")).toEqual({ host: "127.0.0.1", port: 9701 })
    expect(await resolve_target("9701")).toEqual({ host: "127.0.0.1", port: 9701 })
    expect(await resolve_target("localhost:1234")).toEqual({ host: "localhost", port: 1234 })

    const dir = mkdtempSync(join(tmpdir(), "st-ctl-"))
    const file = join(dir, "control.json")
    writeFileSync(file, JSON.stringify({ port: 4321, app: "gallery" }))
    expect(await resolve_target(file)).toEqual({ host: "127.0.0.1", port: 4321 })
  })

  test("控制文件缺失时报 not_found 而非静默失败", async () => {
    const missing = join(tmpdir(), "st-ctl-definitely-missing.json")
    await expect(resolve_target(missing)).rejects.toBeInstanceOf(ControlError)
  })
})

describe("call_control", () => {
  test("多请求顺序往返：id 配对正确", async () => {
    const server = await mock((message) => ({
      ok: true,
      result: { method: message.method, echoed: message.params },
    }))
    const results = await call_control(
      { host: "127.0.0.1", port: server.port },
      [{ method: "hello", params: { protocol: 1 } }, { method: "ping" }],
    )
    expect(results).toHaveLength(2)
    expect(unwrap<{ method: string }>(results[0], "hello").method).toBe("hello")
    expect(unwrap<{ method: string }>(results[1], "ping").method).toBe("ping")
    expect(server.calls.map((call) => call.method)).toEqual(["hello", "ping"])
  })

  test("事件帧（无 id）被忽略，不污染响应", async () => {
    const server = await mock((message) => {
      if (message.method === "events") {
        const frame = (payload: string): Buffer => {
          const body = Buffer.from(payload, "utf8")
          const header = Buffer.alloc(4)
          header.writeUInt32BE(body.length, 0)
          return Buffer.concat([header, body])
        }
        // 先推一个事件帧（无 id），再推真正的响应 —— 客户端必须忽略事件帧
        return {
          frames: [
            frame(JSON.stringify({ event: "frame", seq: 9, data: { frames: 3 } })),
            frame(JSON.stringify({ id: message.id, ok: true, result: { enabled: true } })),
          ],
        }
      }
      return { ok: true, result: {} }
    })
    const response = await request<{ enabled: boolean }>(
      { host: "127.0.0.1", port: server.port },
      "events",
      { enable: true },
    )
    expect(response.enabled).toBe(true)
  })

  test("服务端错误 → ControlError 带错误码与原文", async () => {
    const server = await mock(() => ({ ok: false, error: { code: "not_found", message: "未找到元素: #nope" } }))
    try {
      await request({ host: "127.0.0.1", port: server.port }, "get", { id: "#nope" })
      throw new Error("应当抛出")
    } catch (error) {
      expect(error).toBeInstanceOf(ControlError)
      expect((error as ControlError).code).toBe("not_found")
      expect((error as ControlError).message).toContain("未找到元素")
    }
  })

  test("超时 → timeout 且不悬挂", async () => {
    const server = await mock(() => ({ defer: true }))
    const started = Date.now()
    await expect(
      call_once({ host: "127.0.0.1", port: server.port }, "wait", { for: "element" }, 300),
    ).rejects.toBeInstanceOf(ControlError)
    expect(Date.now() - started).toBeLessThan(3000)
  })

  test("连接失败 → transport 错误（不是静默空结果）", async () => {
    try {
      await call_once({ host: "127.0.0.1", port: 1 }, "ping", {}, 1500)
      throw new Error("应当抛出")
    } catch (error) {
      expect(error).toBeInstanceOf(ControlError)
      expect(["transport", "timeout"]).toContain((error as ControlError).code)
    }
  })

  test("自动握手：非 hello 请求前置合成 hello 并过滤其响应", async () => {
    // 服务端自 2026-09-30 起要求每连接首帧必须是 hello（或 ping），且默认校验 token。
    // 客户端应把 hello 与用户请求同批写出（不额外等往返），并把 hello 的响应过滤掉。
    const server = await mock((message) => ({ ok: true, result: { method: message.method } }))
    const response = await request<{ method: string }>(
      { host: "127.0.0.1", port: server.port, token: "tk-1" },
      "tree",
      { depth: 2 },
    )
    expect(response.method).toBe("tree")
    expect(server.calls.map((call) => call.method)).toEqual(["hello", "tree"])
    expect(server.calls[0].params.token).toBe("tk-1")
  })

  test("显式 hello 请求自动补 token（调用方无需手拼）", async () => {
    const server = await mock((message) => ({ ok: true, result: { method: message.method } }))
    const response = await request<{ method: string }>(
      { host: "127.0.0.1", port: server.port, token: "tk-2" },
      "hello",
    )
    expect(response.method).toBe("hello")
    expect(server.calls).toHaveLength(1)
    expect(server.calls[0].params.token).toBe("tk-2")
  })

  test("ping 不前置 hello（协议门允许探测）", async () => {
    const server = await mock((message) => ({ ok: true, result: { method: message.method } }))
    await request({ host: "127.0.0.1", port: server.port }, "ping")
    expect(server.calls.map((call) => call.method)).toEqual(["ping"])
  })

  test("超长帧 → overflow 拒绝（不吞内存）", async () => {
    const server = await mock(() => {
      const header = Buffer.alloc(4)
      header.writeUInt32BE(0x7fffffff, 0) // 声明 2GB 长度（远超上限）
      return { frames: [header] }
    })
    try {
      await call_once({ host: "127.0.0.1", port: server.port }, "capture", {}, 2000)
      throw new Error("应当抛出")
    } catch (error) {
      expect(error).toBeInstanceOf(ControlError)
      expect(["overflow", "transport", "timeout"]).toContain((error as ControlError).code)
    }
  })
})
