/**
 * 朗读合成服务（REST `/api/v1/tts` 的内核）：把文本合成为可播放的 WAV 字节流，供 Web 助手回复的
 * 「朗读」按钮调用。
 *
 * 与 tts 子Agent 共用 `@gebai/agents` 的引擎（core/tts）：脚本、SSML 构造、结果解析、失败分类同一份实现，
 * 执行通道（runCommand/文件读写）经 TtsDeps 注入——本模块只补两件子Agent 不需要的事：
 * - **长文本分片**：助手回复常超过单次合成上限，按句边界分片合成后拼接 WAV（不合成长度残缺的音频）；
 * - **进程内缓存**：同一段文本重复点击（改主题/重播/多人同问）不再重复调用系统语音引擎。
 *
 * 产物不落盘（除分片合成期间的临时文件，用后即删）——朗读是「听一下」，不该在会话产物里堆积音频。
 */
import { spawn } from "node:child_process"
import { createHash } from "node:crypto"
import { mkdir, readFile, rm, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { dirname, join } from "node:path"
import {
  TTS_CHUNK_CHARS,
  TTS_PITCH,
  TTS_RATE,
  TTS_REQUEST_MAX_TEXT,
  TTS_VOLUME,
  UNSUPPORTED_PLATFORM_NOTE,
  clampPercent,
  concatWav,
  escapeXml,
  isSupportedPlatform,
  normalizeEngine,
  plainTextForSpeech,
  runTtsScript,
  scriptFailureNote,
  splitText,
  validateText,
  type TtsDeps,
} from "@gebai/agents"

/** 缓存总字节上限（默认 24MB，约 12 分钟 16kHz 单声道音频）：超出按插入顺序淘汰最旧的。 */
export const TTS_CACHE_MAX_BYTES = 24 * 1024 * 1024
/** 同时进行的合成数上限（系统语音引擎是进程内独占资源，串行化避免相互拖慢）。 */
export const TTS_MAX_CONCURRENT = 2

export interface TtsRequest {
  text: string
  voice?: string
  rate?: number
  pitch?: number
  volume?: number
}

export type TtsResult =
  | { ok: true; wav: Uint8Array; durationSec: number; engine: string; voice: string; cached: boolean }
  | { ok: false; error: string; status: 400 | 503 | 500 }

interface CacheEntry {
  wav: Uint8Array
  durationSec: number
  engine: string
  voice: string
}

/** 子进程输出解码：Windows 中文系统的 PowerShell 报错走 GBK，按 UTF-8 解出替换字符时重解。 */
function decodeOutput(buf: Buffer): string {
  const utf8 = buf.toString("utf8")
  if (!utf8.includes("\uFFFD")) return utf8
  try {
    return new TextDecoder("gbk").decode(buf)
  } catch {
    return utf8
  }
}

/** 进程级执行通道：服务端不经会话沙箱（无会话上下文），用 node 子进程直接调 PowerShell。 */
const commandDeps = (tmpDir: string): TtsDeps => ({
  runCommand: (cmd, opts) =>
    new Promise((resolve) => {
      // 不经 shell：`-EncodedCommand` 的内嵌脚本使整条命令远超 cmd.exe 的 8191 字符上限，
      // 直接以可执行 + 参数数组 spawn（CreateProcess 上限 32767）——命令字符串为自造，仅含空格分隔。
      const [file, ...args] = cmd.split(" ")
      const child = spawn(file as string, args, { windowsHide: true, env: { ...process.env, ...(opts?.env ?? {}) } })
      const out: Buffer[] = []
      const err: Buffer[] = []
      let settled = false
      const finish = (code: number) => {
        if (settled) return
        settled = true
        clearTimeout(timer)
        resolve({ stdout: decodeOutput(Buffer.concat(out)), stderr: decodeOutput(Buffer.concat(err)), code })
      }
      const timer = setTimeout(() => {
        try {
          child.kill()
        } catch {
          /* 已退出 */
        }
        // 与 Sandbox.exec 同口径：124 表示超时（调用方据此区分超时与脚本报错）
        err.push(Buffer.from(`\n[timed out after ${opts?.timeoutMs ?? 0}ms]`))
        finish(124)
      }, opts?.timeoutMs ?? 120_000)
      child.stdout?.on("data", (d) => out.push(Buffer.from(d)))
      child.stderr?.on("data", (d) => err.push(Buffer.from(d)))
      // stdin 的错误（EPIPE：子进程提前退出）必须有监听者，否则流上的 error 事件会抛到进程级
      child.stdin?.on("error", () => {})
      child.on("error", (e) => {
        err.push(Buffer.from(String(e)))
        finish(1)
      })
      child.on("close", (code) => finish(code ?? 1))
    }),
  readFile: (p) => readFile(p, "utf8"),
  writeFile: async (p, content) => {
    await mkdir(dirname(p), { recursive: true })
    await writeFile(p, content, "utf8")
  },
  deleteFile: (p) => rm(p, { force: true }),
  tmpDir,
})

export interface TtsService {
  synthesize(req: TtsRequest): Promise<TtsResult>
  /** 缓存现状（诊断/测试用）。 */
  cacheStatus(): { entries: number; bytes: number }
}

/** 创建合成服务（进程级单例语义：缓存与并发闸门都在闭包内）。
 *  deps 可注入（用例用假执行通道驱动，不真起 PowerShell、不依赖本机语音引擎）。 */
export function createTtsService(
  opts: { cacheMaxBytes?: number; tmpDir?: string; deps?: (tmpDir: string) => TtsDeps; env?: Record<string, string> } = {},
): TtsService {
  const cacheMax = opts.cacheMaxBytes ?? TTS_CACHE_MAX_BYTES
  const tmpDir = opts.tmpDir ?? join(tmpdir(), "gebai-tts")
  const readEnv = (key: string): string | undefined => opts.env?.[key] ?? process.env[key]
  const makeDeps = opts.deps ?? commandDeps
  const cache = new Map<string, CacheEntry>()
  let cacheBytes = 0
  let running = 0
  const waiters: Array<() => void> = []

  const acquire = async (): Promise<void> => {
    if (running < TTS_MAX_CONCURRENT) {
      running++
      return
    }
    await new Promise<void>((resolve) => waiters.push(resolve))
    running++
  }
  const release = (): void => {
    running--
    waiters.shift()?.()
  }

  const put = (key: string, entry: CacheEntry): void => {
    cache.set(key, entry)
    cacheBytes += entry.wav.byteLength
    while (cacheBytes > cacheMax && cache.size > 1) {
      const oldest = cache.keys().next().value as string | undefined
      if (oldest === undefined) break
      const evicted = cache.get(oldest)
      cache.delete(oldest)
      cacheBytes -= evicted?.wav.byteLength ?? 0
    }
  }

  const synthesizeOne = async (text: string, params: { voice?: string; rate: number; pitch: number; volume: number }) => {
    const outPath = join(tmpDir, `synth-${Date.now()}-${Math.random().toString(36).slice(2, 8)}.wav`)
    await mkdir(tmpDir, { recursive: true })
    try {
      const run = await runTtsScript(makeDeps(tmpDir), {
        mode: "synth",
        engine: normalizeEngine(readEnv("TTS_ENGINE")),
        text: escapeXml(text),
        voice: params.voice,
        rate: params.rate,
        pitch: params.pitch,
        volume: params.volume,
        out: outPath,
      })
      if (!run.result?.ok) return { ok: false as const, error: scriptFailureNote(run, normalizeEngine(readEnv("TTS_ENGINE"))) }
      return { ok: true as const, wav: new Uint8Array(await readFile(outPath)), durationSec: run.result.durationSec ?? 0, engine: run.result.engine ?? "", voice: run.result.voice ?? "" }
    } finally {
      await rm(outPath, { force: true })
    }
  }

  return {
    async synthesize(req) {
      if (!isSupportedPlatform()) return { ok: false, error: UNSUPPORTED_PLATFORM_NOTE, status: 503 }
      // 非字符串/缺参按空处理——String(undefined) 会得到字面量 "undefined" 被当成正文合成
      const raw = typeof req?.text === "string" ? req.text : ""
      const text = plainTextForSpeech(raw)
      const problem = validateText(text, TTS_REQUEST_MAX_TEXT)
      if (problem) return { ok: false, error: problem, status: 400 }
      const voice = String(req.voice ?? "").trim() || String(readEnv("TTS_VOICE") ?? "").trim() || undefined
      const rate = clampPercent(req.rate, TTS_RATE)
      const pitch = clampPercent(req.pitch, TTS_PITCH)
      const volume = clampPercent(req.volume, TTS_VOLUME)

      const key = createHash("sha256").update(JSON.stringify({ text, voice, rate, pitch, volume })).digest("hex")
      const hit = cache.get(key)
      if (hit) return { ok: true, ...hit, cached: true }

      await acquire()
      try {
        // 分片合成：单次脚本调用有文本上限，长回复按句边界分片后拼接（格式不一致则如实报错，不交付残缺音频）
        const chunks = splitText(text, TTS_CHUNK_CHARS)
        const parts: Uint8Array[] = []
        let duration = 0
        let engine = ""
        let usedVoice = ""
        for (const chunk of chunks) {
          const one = await synthesizeOne(chunk, { voice, rate, pitch, volume })
          if (!one.ok) return { ok: false, error: one.error, status: 503 }
          parts.push(one.wav)
          duration += one.durationSec
          engine = one.engine
          usedVoice = one.voice
        }
        const wav = parts.length > 1 ? concatWav(parts) : parts[0]
        if (!wav) return { ok: false, error: "语音合成失败：长文本分片音频格式不一致，无法拼接为完整音频。", status: 500 }
        put(key, { wav, durationSec: duration, engine, voice: usedVoice })
        return { ok: true, wav, durationSec: duration, engine, voice: usedVoice, cached: false }
      } finally {
        release()
      }
    },
    cacheStatus: () => ({ entries: cache.size, bytes: cacheBytes }),
  }
}
