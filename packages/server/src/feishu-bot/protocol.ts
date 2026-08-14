/**
 * 飞书长连接协议层：帧构造/解析、事件合包、客户端配置。
 * 协议细节参照 lark_oapi 官方 SDK（endpoint 发现 → protobuf Frame → 心跳/事件/ACK）。
 */
import { decodeFrame, encodeFrame, type PbFrame } from "./pb"

/** 帧方法：CONTROL=0（心跳控制），DATA=1（事件/卡片数据）。 */
export const FRAME_CONTROL = 0
export const FRAME_DATA = 1

/** header key 常量（lark_oapi ws/const.py）。 */
export const H_TYPE = "type"
export const H_MESSAGE_ID = "message_id"
export const H_SUM = "sum"
export const H_SEQ = "seq"
export const H_TRACE_ID = "trace_id"
export const H_BIZ_RT = "biz_rt"

/** 消息类型（lark_oapi ws/enum.py）。 */
export const MSG_EVENT = "event"
export const MSG_CARD = "card"
export const MSG_PING = "ping"
export const MSG_PONG = "pong"

/** 服务端下发的客户端配置（endpoint 响应 / pong 帧 payload）。 */
export interface FeishuClientConfig {
  ReconnectCount?: number
  ReconnectInterval?: number
  ReconnectNonce?: number
  PingInterval?: number
}

/** endpoint 发现响应。 */
export interface EndpointResp {
  code: number
  msg?: string
  data?: { URL?: string; ClientConfig?: FeishuClientConfig }
}

function headerOf(frame: PbFrame, key: string): string | undefined {
  return frame.headers.find((h) => h.key === key)?.value
}

/** 构造心跳 ping 帧（CONTROL，type=ping，service 取自 WS URL 的 service_id）。 */
export function buildPingFrame(service: number): Uint8Array {
  return encodeFrame({
    SeqID: 0,
    LogID: 0,
    service,
    method: FRAME_CONTROL,
    headers: [{ key: H_TYPE, value: MSG_PING }],
  })
}

/**
 * 构造事件 ACK 帧：以收到的事件帧为基础，payload 替换为响应 JSON，并追加 biz_rt 处理耗时 header（与官方 SDK 行为一致）。
 * - 事件帧默认响应 `{"code":200}`；卡片交互帧（type="card"）响应携带卡片回调返回体（card/toast，可为 `{}`）。
 */
export function buildAckFrame(original: Uint8Array, bizMs: number, responseJson = '{"code":200}'): Uint8Array {
  const frame = decodeFrame(original)
  frame.headers = [...frame.headers.filter((h) => h.key !== H_BIZ_RT), { key: H_BIZ_RT, value: String(bizMs) }]
  frame.payload = new TextEncoder().encode(responseJson)
  return encodeFrame(frame)
}

/** 解析 pong 帧 payload（服务端可能推送新的客户端配置）。 */
export function parseClientConfig(payload: Uint8Array): FeishuClientConfig | null {
  try {
    const obj = JSON.parse(new TextDecoder().decode(payload)) as FeishuClientConfig
    if (typeof obj !== "object" || obj === null || Array.isArray(obj)) return null
    return {
      ReconnectCount: typeof obj.ReconnectCount === "number" ? obj.ReconnectCount : undefined,
      ReconnectInterval: typeof obj.ReconnectInterval === "number" ? obj.ReconnectInterval : undefined,
      ReconnectNonce: typeof obj.ReconnectNonce === "number" ? obj.ReconnectNonce : undefined,
      PingInterval: typeof obj.PingInterval === "number" ? obj.PingInterval : undefined,
    }
  } catch {
    return null
  }
}

/** 解析事件帧 payload（schema 2.0 JSON）。 */
export function parseEventPayload(payload: Uint8Array): Record<string, unknown> | null {
  try {
    const obj = JSON.parse(new TextDecoder().decode(payload)) as Record<string, unknown>
    return obj && typeof obj === "object" && !Array.isArray(obj) ? obj : null
  } catch {
    return null
  }
}

/** 已解析的 DATA 帧元信息（headers 中的 message_id/sum/seq 等）。 */
export interface DataFrameInfo {
  frame: PbFrame
  messageId: string
  sum: number
  seq: number
  traceId?: string
  type: string
  payload: Uint8Array
}

/** 解析 DATA 帧：提取 headers 元信息（缺省时 sum=1/seq=0/messageId=空）。 */
export function parseDataFrame(buf: Uint8Array): DataFrameInfo {
  const frame = decodeFrame(buf)
  return {
    frame,
    messageId: headerOf(frame, H_MESSAGE_ID) ?? "",
    sum: Number(headerOf(frame, H_SUM) ?? "1"),
    seq: Number(headerOf(frame, H_SEQ) ?? "0"),
    traceId: headerOf(frame, H_TRACE_ID),
    type: headerOf(frame, H_TYPE) ?? "",
    payload: frame.payload ?? new Uint8Array(0),
  }
}

/** 分片合包缓存：按 message_id 收集，sum 片到齐后拼接（5 秒过期，参照官方 SDK）。 */
export class FrameAssembler {
  private parts = new Map<string, { chunks: (Uint8Array | null)[]; expiresAt: number }>()

  constructor(private clock: () => number = Date.now, private ttlMs = 5000) {}

  /**
   * 投递一个分片；返回完整 payload（到齐时），否则返回 null（继续等待）。
   */
  push(info: Pick<DataFrameInfo, "messageId" | "sum" | "seq" | "payload">): Uint8Array | null {
    if (info.sum <= 1) return info.payload
    const now = this.clock()
    for (const [id, entry] of this.parts) {
      if (entry.expiresAt < now) this.parts.delete(id)
    }
    let entry = this.parts.get(info.messageId)
    if (!entry) {
      entry = { chunks: new Array(info.sum).fill(null), expiresAt: now + this.ttlMs }
      this.parts.set(info.messageId, entry)
    }
    if (info.seq >= 0 && info.seq < info.sum) entry.chunks[info.seq] = info.payload
    if (entry.chunks.some((c) => c === null)) return null
    this.parts.delete(info.messageId)
    return Buffer.concat(entry.chunks as Uint8Array[])
  }
}
