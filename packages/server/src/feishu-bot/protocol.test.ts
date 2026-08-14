import { describe, expect, test } from "bun:test"
import { decodeFrame, encodeFrame } from "./pb"
import { buildAckFrame, buildPingFrame, FrameAssembler, parseClientConfig, parseDataFrame, parseEventPayload } from "./protocol"

function eventFrame(payload: object, headers: Record<string, string> = {}): Uint8Array {
  return encodeFrame({
    SeqID: 0,
    LogID: 0,
    service: 1,
    method: 1,
    headers: [{ key: "type", value: "event" }, ...Object.entries(headers).map(([key, value]) => ({ key, value }))],
    payload: new TextEncoder().encode(JSON.stringify(payload)),
  })
}

describe("buildPingFrame", () => {
  test("CONTROL 帧 + type=ping + service", () => {
    const dec = decodeFrame(buildPingFrame(42))
    expect(dec.method).toBe(0)
    expect(dec.service).toBe(42)
    expect(dec.headers).toContainEqual({ key: "type", value: "ping" })
  })
})

describe("buildAckFrame", () => {
  test("保留原帧头并追加 biz_rt，payload 为 {\"code\":200}", () => {
    const original = eventFrame({ schema: "2.0" }, { message_id: "om_1", sum: "1", seq: "0", trace_id: "tr_1" })
    const ack = decodeFrame(buildAckFrame(original, 12))
    expect(ack.method).toBe(1)
    expect(ack.headers).toContainEqual({ key: "message_id", value: "om_1" })
    expect(ack.headers).toContainEqual({ key: "biz_rt", value: "12" })
    expect(ack.headers.filter((h) => h.key === "biz_rt")).toHaveLength(1)
    expect(JSON.parse(new TextDecoder().decode(ack.payload))).toEqual({ code: 200 })
  })
})

describe("parseClientConfig", () => {
  test("解析 pong 配置", () => {
    const cfg = parseClientConfig(new TextEncoder().encode('{"ReconnectCount":-1,"ReconnectInterval":120,"ReconnectNonce":30,"PingInterval":120}'))
    expect(cfg).toEqual({ ReconnectCount: -1, ReconnectInterval: 120, ReconnectNonce: 30, PingInterval: 120 })
  })
  test("非法 JSON 返回 null", () => {
    expect(parseClientConfig(new TextEncoder().encode("not-json"))).toBeNull()
    expect(parseClientConfig(new TextEncoder().encode("[1]"))).toBeNull()
  })
})

describe("parseDataFrame", () => {
  test("提取 headers 元信息与 payload", () => {
    const info = parseDataFrame(eventFrame({ a: 1 }, { message_id: "om_9", sum: "3", seq: "1", trace_id: "tr" }))
    expect(info.type).toBe("event")
    expect(info.messageId).toBe("om_9")
    expect(info.sum).toBe(3)
    expect(info.seq).toBe(1)
    expect(info.traceId).toBe("tr")
    expect(parseEventPayload(info.payload)).toEqual({ a: 1 })
  })
  test("缺省 headers 兜底", () => {
    const info = parseDataFrame(eventFrame({}))
    expect(info.sum).toBe(1)
    expect(info.seq).toBe(0)
    expect(info.messageId).toBe("")
  })
  test("CONTROL pong 帧", () => {
    const buf = encodeFrame({ SeqID: 0, LogID: 0, service: 1, method: 0, headers: [{ key: "type", value: "pong" }], payload: new TextEncoder().encode('{"PingInterval":60}') })
    const info = parseDataFrame(buf)
    expect(info.frame.method).toBe(0)
    expect(info.type).toBe("pong")
  })
})

describe("FrameAssembler 合包", () => {
  test("单片直接返回", () => {
    const a = new FrameAssembler(() => 0)
    expect(a.push({ messageId: "", sum: 1, seq: 0, payload: new TextEncoder().encode("x") })).toEqual(new TextEncoder().encode("x"))
  })

  test("多片乱序到齐拼接，未到齐返回 null", () => {
    const a = new FrameAssembler(() => 0)
    const p0 = new TextEncoder().encode("part0|")
    const p1 = new TextEncoder().encode("part1")
    expect(a.push({ messageId: "m", sum: 2, seq: 1, payload: p1 })).toBeNull()
    expect(a.push({ messageId: "m", sum: 2, seq: 0, payload: p0 })).toEqual(new TextEncoder().encode("part0|part1"))
  })

  test("超时过期后释放", () => {
    let now = 0
    const a = new FrameAssembler(() => now, 5000)
    a.push({ messageId: "m", sum: 2, seq: 1, payload: new TextEncoder().encode("p1") })
    now = 6000
    // 新 messageId 触发清理后，旧片不再影响新合包
    a.push({ messageId: "m2", sum: 1, seq: 0, payload: new TextEncoder().encode("x") })
    // 旧 id 重新开始
    expect(a.push({ messageId: "m", sum: 2, seq: 0, payload: new TextEncoder().encode("p0") })).toBeNull()
  })

  test("seq 越界忽略", () => {
    const a = new FrameAssembler(() => 0)
    expect(a.push({ messageId: "m", sum: 2, seq: 5, payload: new TextEncoder().encode("x") })).toBeNull()
  })
})

describe("parseEventPayload", () => {
  test("schema 2.0 事件解析", () => {
    const ev = parseEventPayload(new TextEncoder().encode('{"schema":"2.0","header":{"event_type":"im.message.receive_v1"},"event":{}}'))
    expect(ev?.["schema"]).toBe("2.0")
  })
  test("非法 JSON / 非对象返回 null", () => {
    expect(parseEventPayload(new TextEncoder().encode("boom"))).toBeNull()
    expect(parseEventPayload(new TextEncoder().encode('"str"'))).toBeNull()
  })
})
