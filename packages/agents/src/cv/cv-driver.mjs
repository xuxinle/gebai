/**
 * CV GPU sidecar 驱动（node 子进程，供 core/cv/sidecar.ts spawn）——onnxruntime-node 原生推理，
 * 承载检测类重模型绕开 wasm 单线程 CPU 的算力天花板。本文件保持独立 JS（不进 bun bundle，
 * 与 core/browser/driver.mjs 同款约束）；构建时复制到 dist/ / 二进制形态内嵌物化。
 *
 * 协议（stdin/stdout，行分隔 JSON 头 + 长度前缀原始字节帧，避免大张量 JSON 化）：
 *   请求 = 一行 JSON 头 {id, op, args, tensorBytes?: N}，若带 tensorBytes 紧跟 N 字节 float32
 *          张量（小端原生序，little-endian，与 Float32Array 内存布局一致）
 *   响应 = 一行 JSON 头 {id, ok: true, result, outBytes?: M}（紧跟 M 字节输出张量）
 *          或 {id, ok: false, error}
 *   请求串行处理（读一条→处理→回写→下一条），天然避免 stdout 交错。
 *
 * ops：
 *   init           {ortDir?: string}            → {version}（加载 onnxruntime-node，失败即错误）
 *   session.create {modelPath, ep}              → {sessionId, ep, tried}（ep 逐级探测：显式指定
 *                                                  只试该项；auto 按平台顺序 + cpu 兜底；上报实际
 *                                                  落地的 EP——不静默降级）
 *   session.run    {sessionId, dims} + 张量字节  → {dims} + 输出张量字节（第一个输出）
 *
 * EP 说明：Windows 首选 dml（DirectML，任意 DX12 GPU 免装驱动级依赖）次选 cuda；
 * macOS coreml；其余 cuda。onnxruntime-node 不随 GEBAI 构建内嵌，由 sidecar.ts 按
 * 环境变量/node_modules 解析后经 init.ortDir 注入；创建失败逐级回落 native cpu（多线程）。
 */
import { pathToFileURL } from "node:url"
import { createRequire } from "node:module"
import { existsSync, readFileSync } from "node:fs"
import { join, dirname } from "node:path"

/* ---------------- stdin 读取器：行 + 定长字节帧 ---------------- */

class FrameReader {
  constructor(stream) {
    this.chunks = []
    this.waiters = []
    stream.on("data", (c) => {
      this.chunks.push(c)
      this.pump()
    })
    stream.on("end", () => {
      this.ended = true
      this.pump()
    })
    this.ended = false
  }
  pump() {
    for (;;) {
      const w = this.waiters[0]
      if (!w) return
      if (w.line !== undefined) {
        const nl = this.peekIndexOf("\n".charCodeAt(0))
        if (nl < 0) {
          if (this.ended) return w.reject(new Error("sidecar stdin 已结束（无完整行）"))
          return
        }
        this.waiters.shift()
        w.resolve(this.takeString(nl))
      } else {
        if (this.buffered() < w.bytes) {
          if (this.ended) return w.reject(new Error("sidecar stdin 已结束（字节不足）"))
          return
        }
        this.waiters.shift()
        w.resolve(this.takeBytes(w.bytes))
      }
    }
  }
  buffered() {
    return this.chunks.reduce((n, c) => n + c.length, 0)
  }
  peekIndexOf(byte) {
    let seen = 0
    for (const c of this.chunks) {
      const i = c.indexOf(byte)
      if (i >= 0) return seen + i
      seen += c.length
    }
    return -1
  }
  takeString(until) {
    let out = Buffer.alloc(0)
    let need = until + 1
    for (let i = 0; i < this.chunks.length && need > 0; i++) {
      const take = Math.min(need, this.chunks[i].length)
      out = Buffer.concat([out, this.chunks[i].subarray(0, take)])
      this.chunks[i] = this.chunks[i].subarray(take)
      need -= take
    }
    this.chunks = this.chunks.filter((c) => c.length > 0)
    return out.subarray(0, until).toString("utf8")
  }
  takeBytes(n) {
    const out = Buffer.alloc(n)
    let filled = 0
    for (let i = 0; i < this.chunks.length && filled < n; i++) {
      const take = Math.min(n - filled, this.chunks[i].length)
      this.chunks[i].copy(out, filled, 0, take)
      this.chunks[i] = this.chunks[i].subarray(take)
      filled += take
    }
    this.chunks = this.chunks.filter((c) => c.length > 0)
    return out
  }
  readLine() {
    return new Promise((resolve, reject) => {
      this.waiters.push({ line: true, resolve, reject })
      this.pump()
    })
  }
  readBytes(n) {
    return new Promise((resolve, reject) => {
      this.waiters.push({ bytes: n, resolve, reject })
      this.pump()
    })
  }
}

/* ---------------- ort 加载与会话管理 ---------------- */

let ort = null

/**
 * 解析 onnxruntime-node 包入口文件（ESM 不支持目录导入，须落到 package.json main/exports
 * 指向的真实文件）。ortDir 兼容两种形态：包根目录 / 包所在的 node_modules 目录（或其父目录）。
 */
function resolveOrtEntry(ortDir) {
  // 兼容三种形态：包根目录 / 包所在的 node_modules 目录 / 含该 node_modules 的前缀目录
  const tryDirs = [ortDir, join(ortDir, "node_modules"), dirname(ortDir)]
  for (const dir of tryDirs) {
    const pkg = join(dir, "onnxruntime-node", "package.json")
    if (!existsSync(pkg)) continue
    try {
      const main = JSON.parse(readFileSync(pkg, "utf8")).main
      if (main) {
        const entry = join(dir, "onnxruntime-node", main)
        if (existsSync(entry)) return entry
      }
    } catch { /* 损坏 package.json——下一候选 */ }
  }
  return null
}

async function loadOrt(ortDir) {
  if (ort) return
  // 候选入口文件（绝对路径）：显式目录解析 → 驱动自身位置向上解析 node_modules
  const candidates = []
  if (ortDir) {
    const entry = resolveOrtEntry(String(ortDir))
    if (entry) candidates.push(entry)
  }
  const req = createRequire(import.meta.url)
  try {
    candidates.push(req.resolve("onnxruntime-node"))
  } catch { /* 未安装——仅剩显式候选 */ }
  let lastErr = null
  for (const entry of candidates) {
    try {
      const mod = await import(pathToFileURL(entry).href)
      // CJS 包经 ESM import 得到 Module 命名空间：直接导出或挂在 default 上两种形态
      ort = mod.InferenceSession ? mod : (mod.default ?? mod)
      if (!ort?.InferenceSession) throw new Error("模块缺少 InferenceSession 导出")
      return
    } catch (e) {
      lastErr = e
    }
  }
  throw new Error(
    `onnxruntime-node 加载失败（GEBAI_CV_ORT_NODE_DIR 指向其目录，或安装该依赖；无 GPU sidecar 时自动回落 wasm CPU）: ${lastErr?.message ?? lastErr}`,
  )
}

/** auto 模式 EP 探测顺序（平台相关），末尾由调用方补 cpu 兜底。 */
function epOrder() {
  if (process.platform === "win32") return ["dml", "cuda"]
  if (process.platform === "darwin") return ["coreml", "cuda"]
  return ["cuda"]
}

const sessions = new Map()
let nextSessionId = 1

async function createSession(args) {
  await loadOrt(args.ortDir)
  const wanted = args.ep && args.ep !== "auto" ? String(args.ep) : null
  const order = wanted ? [wanted] : [...epOrder(), "cpu"]
  const tried = []
  let lastErr = null
  for (const ep of order) {
    try {
      const session = await ort.InferenceSession.create(String(args.modelPath), { executionProviders: [ep] })
      const id = nextSessionId++
      sessions.set(id, session)
      return { sessionId: id, ep, tried }
    } catch (e) {
      tried.push(`${ep}: ${e?.message ?? e}`)
      lastErr = e
    }
  }
  throw new Error(`模型会话创建失败（逐级探测 ${order.join(" → ")} 均失败）: ${lastErr?.message ?? lastErr}`)
}

async function runSession(args, tensorBytes) {
  const session = sessions.get(args.sessionId)
  if (!session) throw new Error(`会话 ${args.sessionId} 不存在（sidecar 已重启？）`)
  const u8 = new Uint8Array(tensorBytes.length)
  u8.set(tensorBytes)
  const tensor = new ort.Tensor("float32", new Float32Array(u8.buffer), args.dims)
  const out = await session.run({ [session.inputNames[0]]: tensor })
  const first = Object.values(out)[0]
  if (!first) throw new Error("模型无输出")
  const data = first.data ?? first
  const dims = first.dims ?? args.dims
  return { dims: Array.from(dims), bytes: Buffer.from(data.buffer, data.byteOffset, data.byteLength) }
}

/* ---------------- 主循环 ---------------- */

const reader = new FrameReader(process.stdin)

function writeResponse(msg) {
  process.stdout.write(JSON.stringify(msg) + "\n")
}

for (;;) {
  let header
  try {
    const line = await reader.readLine()
    if (!line.trim()) continue
    header = JSON.parse(line)
  } catch {
    process.exit(0) // stdin 结束（父进程已退出）——干净退出，不空转
  }
  let tensor = null
  if (header.tensorBytes > 0) {
    try {
      tensor = await reader.readBytes(header.tensorBytes)
    } catch (e) {
      writeResponse({ id: header.id, ok: false, error: `张量字节读取失败: ${e?.message ?? e}` })
      continue
    }
  }
  try {
    let result
    let bytes = null
    if (header.op === "init") {
      await loadOrt(header.args?.ortDir)
      result = { version: ort.version ?? "" }
    } else if (header.op === "session.create") {
      result = await createSession(header.args ?? {})
    } else if (header.op === "session.run") {
      if (!tensor) throw new Error("session.run 需要 tensorBytes 张量帧")
      const r = await runSession(header.args ?? {}, tensor)
      result = { dims: r.dims }
      bytes = r.bytes
    } else if (header.op === "ping") {
      result = { ok: 1 }
    } else {
      throw new Error(`未知 op: ${header.op}`)
    }
    const msg = { id: header.id, ok: true, result }
    if (bytes) {
      msg.outBytes = bytes.length
      process.stdout.write(JSON.stringify(msg) + "\n")
      process.stdout.write(bytes)
    } else {
      writeResponse(msg)
    }
  } catch (e) {
    writeResponse({ id: header.id, ok: false, error: e?.message ?? String(e) })
  }
}
