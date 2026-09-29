/**
 * 霜天控制通道客户端（TCP，协议 `st-control/1`）。
 *
 * 帧格式：`uint32 大端长度` + UTF-8 JSON 体；请求 `{id, method, params}`，响应 `{id, ok, result|error}`，
 * 事件 `{event, seq, data}`（无 `id`）。**协议内坐标一律为逻辑像素**（DPI 无关）；
 * 物理像素 = 逻辑 × `hello.screen.scale`。
 */
import { connect } from "node:net"

/** 控制通道错误码（与服务端 `error.code` 一致）。 */
export type ControlErrorCode =
  | "invalid"
  | "not_found"
  | "io"
  | "parse"
  | "unsupported"
  | "timeout"
  | "internal"
  | "busy"
  | "cancelled"
  | "ambiguous"
  | "exists"
  | "permission"
  | "overflow"
  | "transport"

export class ControlError extends Error {
  code: ControlErrorCode
  constructor(code: ControlErrorCode, message: string) {
    super(message)
    this.name = "ControlError"
    this.code = code
  }
}

export interface ControlTarget {
  host: string
  port: number
}

export interface ControlResult {
  ok: boolean
  result?: unknown
  error?: { code: string; message: string }
  events?: Array<{ event: string; seq: number; data: unknown }>
}

const MAX_FRAME = 256 * 1024 * 1024

/** 解析目标：`host:port` 字符串、`{host,port}`、或控制文件路径（JSON `{port,host}`）。 */
export async function resolve_target(spec: string | { host?: string; port: number }): Promise<ControlTarget> {
  if (typeof spec !== "string") return { host: spec.host ?? "127.0.0.1", port: spec.port }
  const text = spec.trim()
  if (/^\d+$/.test(text)) return { host: "127.0.0.1", port: Number(text) }
  const match = text.match(/^(?<host>[^:]+):(?<port>\d+)$/)
  if (match?.groups) {
    return { host: match.groups.host, port: Number(match.groups.port) }
  }
  // 视作控制文件路径
  const file = Bun.file(text)
  if (!(await file.exists())) {
    throw new ControlError("not_found", `控制目标不可解析（既非 host:port，也不是存在的控制文件）: ${text}`)
  }
  const payload = (await file.json()) as { port?: number; host?: string }
  if (typeof payload.port !== "number") {
    throw new ControlError("parse", `控制文件缺少 port 字段: ${text}`)
  }
  return { host: payload.host ?? "127.0.0.1", port: payload.port }
}

function encode_frame(payload: unknown): Buffer {
  const body = Buffer.from(JSON.stringify(payload), "utf8")
  const header = Buffer.alloc(4)
  header.writeUInt32BE(body.length, 0)
  return Buffer.concat([header, body])
}

/**
 * 建立连接、顺序发送请求并收集响应（可在一条连接上多次往返；`wait` 类请求会长时间挂起）。
 */
export async function call_control(
  target: ControlTarget,
  requests: Array<{ method: string; params?: Record<string, unknown> }>,
  options: { timeout_ms?: number; keep_events?: boolean; subscribe?: string[] } = {},
): Promise<ControlResult[]> {
  const timeout_ms = options.timeout_ms ?? 10_000
  const socket = connect({ host: target.host, port: target.port })
  const results: ControlResult[] = []
  let buffer = Buffer.alloc(0)
  let expected = 0
  let settled = false

  const done = new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => {
      if (!settled) {
        settled = true
        socket.destroy()
        reject(new ControlError("timeout", `控制通道超时（${timeout_ms}ms，已收到 ${results.length}/${requests.length} 个响应）`))
      }
    }, timeout_ms)

    const finish = () => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      socket.destroy()
      resolve()
    }

    socket.on("connect", () => {
      requests.forEach((request, index) => {
        const params = { ...(request.params ?? {}) }
        if (index === 0 && options.subscribe) params.subscribe = true
        socket.write(encode_frame({ id: index + 1, method: request.method, params }))
      })
    })

    socket.on("data", (chunk: Buffer) => {
      buffer = Buffer.concat([buffer, chunk])
      while (buffer.length >= 4) {
        const length = buffer.readUInt32BE(0)
        if (length > MAX_FRAME) {
          settled = true
          clearTimeout(timer)
          socket.destroy()
          reject(new ControlError("overflow", `帧长度超限: ${length}`))
          return
        }
        if (buffer.length < 4 + length) break
        const body = buffer.subarray(4, 4 + length).toString("utf8")
        buffer = buffer.subarray(4 + length)
        try {
          const message = JSON.parse(body) as Record<string, unknown>
          if (typeof message.id === "number") {
            results.push(message as unknown as ControlResult)
            if (results.length >= requests.length) finish()
          }
        } catch (error) {
          settled = true
          clearTimeout(timer)
          socket.destroy()
          reject(new ControlError("parse", `响应 JSON 解析失败: ${(error as Error).message}`))
          return
        }
      }
    })

    socket.on("timeout", () => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      socket.destroy()
      reject(new ControlError("timeout", `控制通道读超时（${timeout_ms}ms）`))
    })

    socket.on("error", (error: Error) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      socket.destroy()
      reject(new ControlError("transport", `控制通道连接失败: ${error.message}`))
    })

    socket.on("close", () => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      if (results.length > 0) resolve()
      else reject(new ControlError("transport", "控制通道在收到响应前被关闭"))
    })

    void expected
  })

  await done
  return results
}

/** 单次调用（最常用形态）。 */
export async function call_once(
  target: ControlTarget,
  method: string,
  params: Record<string, unknown> = {},
  timeout_ms = 10_000,
): Promise<ControlResult> {
  const [first] = await call_control(target, [{ method, params }], { timeout_ms })
  if (!first) throw new ControlError("internal", `未收到 ${method} 的响应`)
  return first
}

/** 取出成功结果；失败抛 `ControlError`（错误信息含服务端原文，便于诊断）。 */
export function unwrap<T = unknown>(response: ControlResult, method: string): T {
  if (response.ok) return (response.result ?? {}) as T
  const code = (response.error?.code ?? "internal") as ControlErrorCode
  throw new ControlError(code, `${method} 失败: ${response.error?.message ?? "未知错误"}`)
}

/** 便捷：单次调用并解包。 */
export async function request<T = unknown>(
  target: ControlTarget,
  method: string,
  params: Record<string, unknown> = {},
  timeout_ms = 10_000,
): Promise<T> {
  return unwrap<T>(await call_once(target, method, params, timeout_ms), method)
}
