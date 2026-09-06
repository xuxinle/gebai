import { describe, expect, test } from "bun:test"
import { join } from "node:path"
import { CvSidecar, type SidecarProc } from "./sidecar"

/* ---------- 可控字节流（模拟子进程 stdout/stderr） ---------- */

class FakeStream {
  private ctrl!: ReadableStreamDefaultController<Uint8Array>
  readonly stream = new ReadableStream<Uint8Array>({ start: (c) => (this.ctrl = c) })
  emit(data: string | Uint8Array): void {
    this.ctrl.enqueue(typeof data === "string" ? new TextEncoder().encode(data) : data)
  }
  close(): void {
    try {
      this.ctrl.close()
    } catch { /* 已关闭 */ }
  }
}

/* ---------- 协议替身：按 cv-driver.mjs 协议解析请求并按脚本应答 ---------- */

interface FakeBehavior {
  initOk: boolean
  createResult: { sessionId: number; ep: string; tried: string[] }
  failRun: string | null
  hangRun: boolean
  /** 输出张量工厂（dims + float 数据）。 */
  runOutput: () => { dims: number[]; data: Float32Array }
  /** 响应字节帧按多小的分片下发（协议解析须容忍任意分片）。 */
  chunkSize: number
}

class FakeSidecar {
  procs: FakeProc[] = []
  requests: Array<{ op: string; args: Record<string, unknown>; tensorBytes: number }> = []
  behavior: FakeBehavior = {
    initOk: true,
    createResult: { sessionId: 1, ep: "dml", tried: [] },
    failRun: null,
    hangRun: false,
    runOutput: () => ({ dims: [1, 6, 8], data: new Float32Array(6 * 8) }),
    chunkSize: 1 << 20,
  }

  spawn = (cmd: string[]): SidecarProc => {
    const p = new FakeProc(this, cmd)
    this.procs.push(p)
    return p.proc
  }
}

class FakeProc {
  readonly out = new FakeStream()
  readonly err = new FakeStream()
  private buf = Buffer.alloc(0)
  private killedFlag = false
  readonly proc: SidecarProc

  constructor(private readonly owner: FakeSidecar, readonly cmd: string[]) {
    const self = this
    this.proc = {
      stdin: { write: (d) => self.onWrite(d), flush: () => {} },
      stdout: this.out.stream,
      stderr: this.err.stream,
      kill: () => {
        self.killedFlag = true
        self.out.close()
        self.err.close()
      },
      get killed() {
        return self.killedFlag
      },
    }
  }

  private onWrite(data: string | Uint8Array): void {
    this.buf = Buffer.concat([this.buf, typeof data === "string" ? Buffer.from(data) : Buffer.from(data)])
    this.pump()
  }

  private pump(): void {
    const nl = this.buf.indexOf(0x0a)
    if (nl < 0) return
    const header = JSON.parse(this.buf.subarray(0, nl).toString("utf8")) as {
      id: number
      op: string
      args: Record<string, unknown>
      tensorBytes?: number
    }
    let tensor: Buffer | null = null
    if (header.tensorBytes && header.tensorBytes > 0) {
      if (this.buf.length < nl + 1 + header.tensorBytes) return // 字节帧未到齐
      tensor = this.buf.subarray(nl + 1, nl + 1 + header.tensorBytes)
      this.buf = this.buf.subarray(nl + 1 + header.tensorBytes)
    } else {
      this.buf = this.buf.subarray(nl + 1)
    }
    this.handle(header, tensor)
    this.pump() // 可能已缓冲后续请求
  }

  private respondRaw(payload: string | Uint8Array): void {
    const bytes = typeof payload === "string" ? Buffer.from(payload) : Buffer.from(payload)
    for (let off = 0; off < bytes.length; off += this.owner.behavior.chunkSize) {
      this.out.emit(bytes.subarray(off, off + this.owner.behavior.chunkSize))
    }
  }

  private handle(header: { id: number; op: string; args: Record<string, unknown> }, tensor: Buffer | null): void {
    const b = this.owner.behavior
    this.owner.requests.push({ op: header.op, args: header.args, tensorBytes: tensor?.length ?? 0 })
    if (header.op === "init") {
      if (b.initOk) this.respondRaw(JSON.stringify({ id: header.id, ok: true, result: { version: "fake" } }) + "\n")
      else this.respondRaw(JSON.stringify({ id: header.id, ok: false, error: "onnxruntime-node 加载失败" }) + "\n")
      return
    }
    if (header.op === "session.create") {
      this.respondRaw(JSON.stringify({ id: header.id, ok: true, result: { ...b.createResult } }) + "\n")
      return
    }
    if (header.op === "session.run") {
      if (b.hangRun) return // 模拟无响应（供超时用例）
      if (b.failRun) {
        this.respondRaw(JSON.stringify({ id: header.id, ok: false, error: b.failRun }) + "\n")
        return
      }
      const { dims, data } = b.runOutput()
      const msg = { id: header.id, ok: true, result: { dims }, outBytes: data.byteLength }
      this.respondRaw(JSON.stringify(msg) + "\n")
      this.respondRaw(new Uint8Array(data.buffer, data.byteOffset, data.byteLength))
      return
    }
    this.respondRaw(JSON.stringify({ id: header.id, ok: true, result: {} }) + "\n")
  }
}

const DRIVER = join(import.meta.dir, "cv-driver.mjs")

function client(fake: FakeSidecar, opts: { ortNodeDir?: string | null; timeoutMs?: number } = {}): CvSidecar {
  return new CvSidecar({
    spawn: fake.spawn,
    driverPath: DRIVER,
    ortNodeDir: opts.ortNodeDir ?? "C:/fake/ort-node",
    requestTimeoutMs: opts.timeoutMs ?? 5000,
  })
}

describe("CvSidecar 协议", () => {
  test("往返：init 注入 ortDir → 建会话 → 张量帧推理（float 原样往返）", async () => {
    const fake = new FakeSidecar()
    const out = new Float32Array([1.5, -2.25, 3.75, 4.5])
    fake.behavior.runOutput = () => ({ dims: [1, 2, 2], data: out })
    const sc = client(fake)
    const r = await sc.detectRun({ modelKey: "m:960", modelPath: "m.onnx", ep: "auto", dims: [1, 3, 4, 4], data: new Float32Array(48) })
    expect(r.ep).toBe("dml")
    expect(Array.from(r.data)).toEqual(Array.from(out))
    expect(r.dims).toEqual([1, 2, 2])
    // 请求序列：init（ortDir 注入）→ session.create → session.run（张量字节帧 = 48 floats）
    expect(fake.procs[0].cmd).toEqual(["node", DRIVER])
    expect(fake.requests[0].op).toBe("init")
    expect(fake.requests[0].args.ortDir).toBe("C:/fake/ort-node")
    expect(fake.requests[1].op).toBe("session.create")
    expect(fake.requests[1].args).toEqual({ modelPath: "m.onnx", ep: "auto" })
    expect(fake.requests[2].op).toBe("session.run")
    expect(fake.requests[2].tensorBytes).toBe(48 * 4)
  })

  test("会话按 modelKey 缓存：第二次推理不再建会话", async () => {
    const fake = new FakeSidecar()
    const sc = client(fake)
    const dims = { modelKey: "m:640", modelPath: "m.onnx", ep: "auto", dims: [1, 3, 2, 2] as const, data: new Float32Array(12) }
    await sc.detectRun(dims)
    await sc.detectRun(dims)
    expect(fake.requests.filter((r) => r.op === "session.create").length).toBe(1)
    expect(fake.requests.filter((r) => r.op === "session.run").length).toBe(2)
  })

  test("错误响应 → reject（错误消息透传）", async () => {
    const fake = new FakeSidecar()
    fake.behavior.failRun = "驱动报错：EP 不可用"
    const sc = client(fake)
    await expect(sc.detectRun({ modelKey: "m", modelPath: "m.onnx", ep: "auto", dims: [1, 3, 2, 2], data: new Float32Array(12) })).rejects.toThrow(/EP 不可用/)
  })

  test("响应字节帧按 3 字节分片下发仍正确解析", async () => {
    const fake = new FakeSidecar()
    fake.behavior.chunkSize = 3
    const out = new Float32Array([0.25, 0.5, 0.75, 1, 1.25, 1.5])
    fake.behavior.runOutput = () => ({ dims: [1, 2, 3], data: out })
    const sc = client(fake)
    const r = await sc.detectRun({ modelKey: "m", modelPath: "m.onnx", ep: "auto", dims: [1, 3, 2, 2], data: new Float32Array(12) })
    expect(Array.from(r.data)).toEqual(Array.from(out))
  })

  test("请求超时 → 杀进程、pending 拒绝、下次调用重启", async () => {
    const fake = new FakeSidecar()
    fake.behavior.hangRun = true
    const sc = client(fake, { timeoutMs: 150 })
    await expect(sc.detectRun({ modelKey: "m", modelPath: "m.onnx", ep: "auto", dims: [1, 3, 2, 2], data: new Float32Array(12) })).rejects.toThrow(/超时/)
    expect(fake.procs[0].proc.killed).toBe(true)
    // 重启：下一次调用重新 spawn（会话缓存已随进程退出清空）
    fake.behavior.hangRun = false
    const r = await sc.detectRun({ modelKey: "m", modelPath: "m.onnx", ep: "auto", dims: [1, 3, 2, 2], data: new Float32Array(12) })
    expect(r.ep).toBe("dml")
    expect(fake.procs.length).toBe(2)
  })

  test("init 失败（ort 不可加载）→ 启动即报错", async () => {
    const fake = new FakeSidecar()
    fake.behavior.initOk = false
    const sc = client(fake)
    await expect(sc.detectRun({ modelKey: "m", modelPath: "m.onnx", ep: "auto", dims: [1, 3, 2, 2], data: new Float32Array(12) })).rejects.toThrow(/onnxruntime-node 加载失败/)
  })
})
