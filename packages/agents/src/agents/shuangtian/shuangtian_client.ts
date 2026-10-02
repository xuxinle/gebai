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
  /** 鉴权 token（控制文件携出；`hello` 必须携带同值）。
   * 服务端自 2026-09-30 起默认自动生成 token 并要求握手——不携带会被拒并断连。 */
  token?: string
}

export interface ControlResult {
  ok: boolean
  result?: unknown
  error?: { code: string; message: string }
  events?: Array<{ event: string; seq: number; data: unknown }>
}

const MAX_FRAME = 64 * 1024 * 1024  // 与服务端 ServerOptions::max_frame 对齐（避免两端不一致）

/** 解析目标：`host:port` 字符串、`{host,port}`、或控制文件路径（JSON `{port,host,token}`）。 */
export async function resolve_target(
  spec: string | { host?: string; port: number; token?: string },
): Promise<ControlTarget> {
  if (typeof spec !== "string") return { host: spec.host ?? "127.0.0.1", port: spec.port, token: spec.token }
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
  const payload = (await file.json()) as { port?: number; host?: string; token?: string }
  if (typeof payload.port !== "number") {
    throw new ControlError("parse", `控制文件缺少 port 字段: ${text}`)
  }
  return { host: payload.host ?? "127.0.0.1", port: payload.port, token: payload.token }
}

function encode_frame(payload: unknown): Buffer {
  const body = Buffer.from(JSON.stringify(payload), "utf8")
  const header = Buffer.alloc(4)
  header.writeUInt32BE(body.length, 0)
  return Buffer.concat([header, body])
}

/**
 * 建立连接、顺序发送请求并收集响应（可在一条连接上多次往返；`wait` 类请求会长时间挂起）。
 *
 * **自动握手**：服务端要求每连接首帧必须是 `hello`（或 ping；未握手连接调用其他方法会被拒并断连），
 * 且默认要求 `hello.params.token` 与控制文件一致。本函数在需要时**把 hello 与用户请求同批写出**——
 * 不额外等一个往返（同一条 TCP 流内服务端按序处理）；hello 的响应被过滤，不进入调用方结果。
 */
export async function call_control(
  target: ControlTarget,
  requests: Array<{ method: string; params?: Record<string, unknown> }>,
  options: { timeout_ms?: number; keep_events?: boolean; subscribe?: string[] } = {},
): Promise<ControlResult[]> {
  const timeout_ms = options.timeout_ms ?? 10_000
  if (requests.length === 0) return []
  const socket = connect({ host: target.host, port: target.port })
  const results: ControlResult[] = []
  let buffer = Buffer.alloc(0)
  let settled = false

  // 组装帧：必要时前置合成 hello（id=0，响应会被过滤）。
  const prepend_hello =
    requests.length > 0 && requests[0].method !== "hello" && requests[0].method !== "ping"
  const frames: Array<{ id: number; method: string; params: Record<string, unknown> }> = []
  if (prepend_hello) {
    const hello_params: Record<string, unknown> = { client: "shuangtian-agent" }
    if (target.token) hello_params.token = target.token
    if (options.subscribe) hello_params.subscribe = true
    frames.push({ id: 0, method: "hello", params: hello_params })
  }
  requests.forEach((request, index) => {
    const params = { ...(request.params ?? {}) }
    if (!prepend_hello && index === 0 && options.subscribe) params.subscribe = true
    // 调用方自己的 hello：补上 token（控制文件读出）——缺 token 会被服务端拒。
    if (request.method === "hello" && target.token !== undefined && params.token === undefined) {
      params.token = target.token
    }
    frames.push({ id: index + 1, method: request.method, params })
  })
  // 只计**调用方**的请求：合成的 hello（id=0）单独跟踪，它失败时必须**报错**而不是被丢掉
  // （见下方 `id === 0` 分支）。
  const expected = requests.length
  /** 合成的 hello 是否已被服务端接受（未前置 hello 时为 true——没有握手可等）。 */
  let handshake_ok = !prepend_hello

  const done = new Promise<void>((resolve, reject) => {
    /**
     * 构造"连得上但答不出来"的诊断信息。
     *
     * 实测踩到的场景：控制文件陈旧 → 连到了**上一个实例**的端口 → token 不匹配 →
     * 服务端拒绝握手并断连。旧实现把 hello 的响应静默丢掉，于是调用方只能看到
     * 「未收到 capture 的响应」——一条完全指不出方向的错误。
     */
    const transport_note = (): string => {
      if (handshake_ok) {
        return `控制通道在收齐响应前被关闭（已收到 ${results.length}/${expected} 个响应；应用可能已退出或崩溃——用 run(action=status) 确认，必要时重新 start）`
      }
      return `控制通道在握手阶段被关闭：token 可能不匹配（控制文件陈旧 → 连到了别的实例？用 run(action=status) 确认）`
    }
    const timer = setTimeout(() => {
      if (!settled) {
        settled = true
        socket.destroy()
        reject(new ControlError("timeout", `控制通道超时（${timeout_ms}ms，已收到 ${results.length}/${expected} 个响应）`))
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
      for (const frame of frames) socket.write(encode_frame(frame))
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
          if (typeof message.id !== "number") continue  // 事件帧：不参与请求/响应配对
          // id=0 是**合成的** hello（调用方请求从 1 开始）：握手被拒必须立刻带着服务端
          // 原文报错——否则整类"连错实例/token 不匹配"都会退化成"未收到 X 的响应"。
          if (message.id === 0) {
            if (message.ok === false) {
              const error = message.error as { code?: string; message?: string } | undefined
              settled = true
              clearTimeout(timer)
              socket.destroy()
              reject(
                new ControlError(
                  (error?.code ?? "permission") as ControlErrorCode,
                  `握手被拒：${error?.message ?? "服务端未说明原因"}（控制文件可能已陈旧——连到了另一个实例；` +
                    `用 run(action=status) 确认，或重新 start 取新 token）`,
                ),
              )
              return
            }
            handshake_ok = true
            continue
          }
          results.push(message as unknown as ControlResult)
          if (results.length >= expected) finish()
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
      // 收齐才算成功：只收到一部分（例如握手过了、业务请求没回来）也是失败，
      // 且必须**说清收到几个**——这正是"应用在命令执行中悄悄退出"的特征信号。
      if (results.length >= expected) resolve()
      else reject(new ControlError("transport", transport_note()))
    })
  })

  await done
  // 合成的 hello 响应已在上面单独处理（不进 `results`），这里直接返回调用方的响应。
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
  // 走到这里说明连接正常关闭却没拿到响应：如实说明，别退回一个无指向的"未收到"。
  if (!first) throw new ControlError("internal", `未收到 ${method} 的响应（控制通道正常关闭但无回包）`)
  return first
}

/**
 * 订阅并等待事件（常驻连接、收满或超时即归）。
 *
 * 用途：`ui.changed`（含变更 id 清单）这类**推送**语义——`wait` 方法只能等
 * 服务端可判定的条件（元素/文本/帧），等不了事件流本身（如"等界面变更通知"）。
 *
 * 实现选择（有意的）：不维护跨调用的常驻连接（那需要模块级状态/生命周期管理，
 * 在多工具实例/重启场景下容易变味），而是在单次调用内完成
 * 「连上 → hello(subscribe) → 收集事件 → 达标或超时 → 断开」。
 * 对"等一个事件"的典型用法足够，且无隐藏状态。
 */
export async function wait_for_event(
  target: ControlTarget,
  options: { kinds?: string[]; count?: number; timeout_ms?: number } = {},
): Promise<Array<{ event: string; seq: number; data: unknown }>> {
  const timeout_ms = options.timeout_ms ?? 10_000
  const wanted = options.count ?? 1
  const kinds = options.kinds ?? []
  const socket = connect({ host: target.host, port: target.port })
  const collected: Array<{ event: string; seq: number; data: unknown }> = []
  let buffer = Buffer.alloc(0)
  let settled = false
  const done = new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => {
      if (settled) return
      settled = true
      socket.destroy()
      // 超时不算失败：返回已收集（可能为空）的事件，由调用方判 satisfied
      resolve()
    }, timeout_ms)
    const finish = () => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      socket.destroy()
      resolve()
    }
    socket.on("connect", () => {
      const hello: Record<string, unknown> = { client: "shuangtian-agent", subscribe: true }
      if (target.token) hello.token = target.token
      if (kinds.length > 0) hello.kinds = kinds
      socket.write(encode_frame({ id: 0, method: "hello", params: hello }))
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
          // 事件帧（无 id）：收集；响应帧（带 id，如 hello 确认）忽略
          if (typeof message.id !== "number" && typeof message.event === "string") {
            collected.push({
              event: message.event,
              seq: typeof message.seq === "number" ? message.seq : 0,
              data: message.data,
            })
            if (collected.length >= wanted) {
              finish()
              return
            }
          }
        } catch (error) {
          settled = true
          clearTimeout(timer)
          socket.destroy()
          reject(new ControlError("parse", `事件帧 JSON 解析失败: ${(error as Error).message}`))
          return
        }
      }
    })
    socket.on("error", (error: Error) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      socket.destroy()
      reject(new ControlError("transport", `事件连接失败: ${error.message}`))
    })
    socket.on("close", () => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      resolve()
    })
  })
  await done
  return collected
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
