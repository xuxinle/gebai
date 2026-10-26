import { existsSync, mkdirSync, statSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import type { ContentBlock, SubAgentDef, Tool, ToolContext } from "@gebai/sdk"
import { schema } from "@gebai/sdk/node"
import {
  TTS_ENGINES,
  TTS_MAX_TEXT,
  TTS_OUT_DIR,
  TTS_PITCH,
  TTS_RATE,
  TTS_VOLUME,
  UNSUPPORTED_PLATFORM_NOTE,
  clampPercent,
  defaultOutPath,
  escapeXml,
  filterVoices,
  formatBytes,
  formatDuration,
  formatVoiceList,
  isSupportedPlatform,
  normalizeEngine,
  playbackBlockedReason,
  runTtsScript,
  scriptFailureNote,
  validateText,
  voiceMismatchNote,
  type TtsDeps,
  type TtsEngine,
} from "../../core/tts/speech"
import {
  matchNeuralModel,
  neuralFailureNote,
  neuralTtsScript,
  neuralTtsRoots,
  pickNeuralModel,
  pythonCandidates,
  scanNeuralModels,
  writeTempScript,
} from "../../core/tts/neural"
import { resolveGebaiHome } from "../../core/shared/config"
// 系统提示词独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./tts.md" with { type: "text" }
// 音效工具（合成/效果/混音）：纯本地计算，不依赖系统语音引擎。
import { effectTool, mixTool, sfxTool } from "./sfx"

/** 会话上下文 → 引擎执行通道（合成内核为基建，执行依赖经此适配注入）。 */
function ttsDeps(ctx: ToolContext): TtsDeps {
  return {
    runCommand: (cmd, opts) => ctx.runCommand(cmd, opts),
    readFile: ctx.readFile,
    writeFile: ctx.writeFile,
    deleteFile: ctx.deleteFile,
    tmpDir: ctx.resolvePath(TTS_OUT_DIR),
    signal: ctx.signal,
  }
}

/** 会话/任务级环境变量兜底默认值（前端环境变量面板可配置；工具参数优先）。 */
function engineFromEnv(env: Record<string, string>): TtsEngine {
  return normalizeEngine(env.TTS_ENGINE)
}

/** 是否默认走神经引擎（TTS_ENGINE=neural）；与系统引擎取值空间分开，避免把它当下游 TtsEngine 传。 */
function neuralPreferred(env: Record<string, string>): boolean {
  return String(env.TTS_ENGINE ?? "").trim().toLowerCase() === "neural"
}

function modelFromEnv(env: Record<string, string>): string {
  return String(env.TTS_MODEL ?? "").trim()
}

function voiceFromEnv(env: Record<string, string>): string | undefined {
  const v = String(env.TTS_VOICE ?? "").trim()
  return v || undefined
}

/** 产物文件名（逻辑路径或绝对路径均可：取末段，Windows 反斜杠一并规整）。 */
function baseName(p: string): string {
  return p.replace(/\\/g, "/").split("/").pop() || "voice.wav"
}

export const speakTool: Tool = {
  name: "speak",
  safeMode: false,
  description:
    `把文本合成为音频文件。**两档离线引擎**：engine=neural 本地神经网络 TTS（sherpa-onnx + VITS 声学模型）——` +
    `中文音质接近云端合成、单个模型可含几十到上百个音色、22.05kHz，推荐用于宣传片/口播/有声内容；` +
    `engine=auto/winrt/sapi 是 Windows 系统语音（零安装、任意 Windows 可用，音色机械）作为兜底。` +
    `两者都离线、不联网、不耗配额。参数：text 待合成文本（必填，上限 ${TTS_MAX_TEXT} 字符）；` +
    `engine 引擎（neural / auto / winrt / sapi）；voice 系统语音的音色名（仅非 neural 生效）；` +
    `model neural 用的模型 id（缺省取最优；先用 tts_models 查看可用模型）；` +
    `speaker neural 的音色序号（0 起，越大不等于越好，试听确认；缺省 0）；` +
    `rate 语速百分比（${TTS_RATE.min}~${TTS_RATE.max}）；pitch/volume 仅系统语音生效；` +
    `out 产物路径（缺省 tmp/tts/voice-<时间戳>.wav）。产物为 WAV，可直接在聊天内播放；` +
    `play=true 时同时在运行 GEBAI 这台机器的扬声器上播报（仅本地模式）。`,
  parameters: schema(
    {
      text: { type: "string", description: `要合成的文本（中英混排均可；上限 ${TTS_MAX_TEXT} 字符，超长请拆成多段分别合成，不要截断内容）` },
      voice: { type: "string", description: "音色名：支持完整名（Microsoft Huihui）或关键词（慧慧 / Kangkang）；缺省本机中文女声" },
      rate: { type: "number", description: `语速百分比 ${TTS_RATE.min}~${TTS_RATE.max}（负慢正快，默认 0）` },
      pitch: { type: "number", description: `音调百分比 ${TTS_PITCH.min}~${TTS_PITCH.max}（负低正高，默认 0）` },
      volume: { type: "number", description: `音量百分比 ${TTS_VOLUME.min}~${TTS_VOLUME.max}（默认 0）` },
      out: { type: "string", description: "产物音频路径（相对会话工作目录；缺省 tmp/tts/voice-<时间戳>.wav）" },
      engine: {
        type: "string",
        enum: ["neural", ...TTS_ENGINES],
        description: "语音引擎：neural（本地神经 TTS，音质最好，需已装模型）/ auto（默认，WinRT 优先 SAPI 回退）/ winrt / sapi",
      },
      model: { type: "string", description: "neural 引擎的模型 id（如 vits-zh-theresa）；缺省自动选最优，先用 tts_models 查看" },
      speaker: { type: "number", description: "neural 引擎的音色序号（0 起）；模型含几十~上百个音色，越陌生越要试听确认" },
      play: { type: "boolean", description: "合成后在运行 GEBAI 这台机器的扬声器上播报（后台播放，不阻塞返回；仅本地模式，服务端部署拒绝）" },
    },
    ["text"],
  ),
  outputSchema: schema(
    {
      path: { type: "string", description: "产物路径（相对会话工作目录）" },
      engine: { type: "string", description: "实际使用的引擎：neural / winrt / sapi" },
      voice: { type: "string", description: "实际使用的音色名" },
      bytes: { type: "number", description: "产物字节数" },
      durationSec: { type: "number", description: "音频时长（秒）" },
      played: { type: "boolean", description: "是否已在本机扬声器开始播报（play=true 且未被拦截时）" },
    },
    ["path", "engine", "voice"],
  ),
  async execute(args, ctx) {
    if (!isSupportedPlatform()) return { output: UNSUPPORTED_PLATFORM_NOTE }
    const problem = validateText(args.text)
    if (problem) return { output: problem }
    const text = String(args.text)
    const rate = clampPercent(args.rate, TTS_RATE)
    const outRel = String(args.out ?? "").trim() || defaultOutPath(Date.now())
    const outAbs = ctx.resolvePath(outRel)
    // 覆盖提示按「写入前是否已存在」判定（合成后才查会永远为真）
    const existed = args.out ? existsSync(outAbs) : false

    // —— 神经引擎（engine=neural）：本地 VITS + onnxruntime ——
    const engineRaw = String(args.engine ?? "").trim().toLowerCase()
    if (engineRaw === "neural" || (engineRaw === "" && neuralPreferred(ctx.env))) {
      const models = scanNeuralModels()
      if (models.length === 0) {
        return {
          output:
            "神经语音暂不可用：未找到已安装的模型。\n" +
            "用 tts_install（会从镜像拉取中文 VITS 模型约 120MB）或手动把 sherpa-onnx 兼容的 " +
            "VITS 中文模型目录（*.onnx + tokens.txt + lexicon.txt）放到 " +
            "`{GEBAI_HOME}/resources/models/tts/<模型名>/`。\n" +
            "暂时可改用 engine=auto（Windows 系统语音，零安装，但音色机械）。",
        }
      }
      const model = matchNeuralModel(models, String(args.model ?? "").trim() || modelFromEnv(ctx.env)) ?? pickNeuralModel(models)!
      const requestedSpeaker = Math.max(0, Math.round(Number(args.speaker ?? 0) || 0))
      const speed = Math.max(0.5, Math.min(2.0, 1 + rate / 100))
      const modelDir = model.onnx.replace(/[\\/][^\\/]+$/, "")
      let speaker = requestedSpeaker
      let run = await runNeural(ctx, { modelDir, text, speaker, speed, outAbs })
      // 音色序号越界：sherpa-onnx 只打日志、返回空样本（不抛错），于是报“空音频”。
      // 音色总数只有模型加载后才知道，所以不能预先夹取——改为拿到报告后**回落 0 号重跑一次**，
      // 并告知用户真实范围。绝不因越界而交付一个 0 秒的 WAV。
      let clamped = false
      if (!run.ok && String(run.report.error ?? "") === "empty-audio" && requestedSpeaker > 0) {
        const guess = Number(run.report.numSpeakers ?? 0)
        if (guess === 0 || requestedSpeaker >= guess) {
          speaker = 0
          clamped = true
          run = await runNeural(ctx, { modelDir, text, speaker, speed, outAbs })
        }
      }
      void clamped
        if (!run.ok) {
    // ## 失败时把 `report` 一起交给说明函数（不只是 note）
    //
    // 旧写法是 `run.note || JSON.stringify(run.report)`——只要 note 有值，
    // **report 就被丢掉**，而 report 里才是结构化的 `{error, detail:{stdout,stderr}}`。
    // 后果：可见文案永远是「python 未返回报告」，而真的 stderr 一行都看不到。
    // 现在把两者合并后一起传：`error` 码保证分支命中，`detail` 提供原文。
    return {
      output: neuralFailureNote({
        error: String((run.report as Record<string, unknown>)?.error ?? "unknown"),
        note: run.note,
        detail: (run.report as Record<string, unknown>)?.detail,
      }),
    }
  }
      const report = run.report
      const bytes = existsSync(outAbs) ? statSync(outAbs).size : 0
      const durationSec = Number(report.durationSec ?? 0)
      const numSpeakers = Number(report.numSpeakers ?? 0)
      const notes: string[] = []
      if (existed) notes.push("目标路径已有同名文件，本次已覆盖。")
      if (numSpeakers > 0 && requestedSpeaker >= numSpeakers) {
        // sherpa-onnx 对越界 sid 不报错，而是落到 0 号并继续合成。
        // 产物是好的，但**用错音色必须说出来**——否则用户以为 #9999 生效了。
        notes.push(
          `speaker=${requestedSpeaker} 超出该模型音色范围（0~${numSpeakers - 1}），` +
            "实际用的是 0 号音色。音色序号与“好听”无关，建议试听后固定下来。",
        )
      }
      const blocks: ContentBlock[] = [
        { type: "file", path: outRel, name: baseName(outRel), mime: "audio/wav" },
      ]
      let played = false
      if (args.play === true) {
        const blocked = playbackBlockedReason(ctx)
        if (blocked) notes.push(blocked)
        else {
          // 播报复用系统语音的播放通道（它只负责把 WAV 送到扬声器，与合成引擎无关）
          const playRun = await runTtsScript(ttsDeps(ctx), { mode: "play", engine: "auto", wav: outAbs })
          if (playRun.result?.ok) played = true
          else notes.push(scriptFailureNote(playRun, "auto"))
        }
      }
      return {
        output: [
          `已合成语音（神经引擎）：${outRel}`,
          `模型 ${model.id} · 音色 #${speaker}${numSpeakers ? ` / 共 ${numSpeakers} 个` : ""} · ` +
            `${Number(report.sampleRate ?? 0)}Hz · 时长 ${formatDuration(durationSec)} · 大小 ${formatBytes(bytes)}`,
          rate !== 0 ? `语速 ${rate > 0 ? "+" : ""}${rate}%` : "",
          played ? "已在运行 GEBAI 的这台机器的扬声器上开始播报。" : "",
          notes.length ? `注意：${notes.join(" ")}` : "",
        ]
          .filter(Boolean)
          .join("\n"),
        data: {
          path: outRel,
          engine: "neural",
          voice: `${model.id}#${speaker}`,
          bytes,
          durationSec,
          played,
        },
        blocks,
      }
    }

    const engine = normalizeEngine(args.engine, engineFromEnv(ctx.env))
    const voice = String(args.voice ?? "").trim() || voiceFromEnv(ctx.env)
    const pitch = clampPercent(args.pitch, TTS_PITCH)
    const volume = clampPercent(args.volume, TTS_VOLUME)
    const run = await runTtsScript(ttsDeps(ctx), {
      mode: "synth",
      engine,
      text: escapeXml(text),
      voice,
      rate,
      pitch,
      volume,
      out: outAbs,
    })
    if (!run.result?.ok) return { output: scriptFailureNote(run, engine) }

    const result = run.result
    const bytes = result.bytes ?? 0
    const duration = result.durationSec ?? 0
    const blocks: ContentBlock[] = [{ type: "file", path: outRel, name: baseName(outRel), mime: "audio/wav" }]
    const notes: string[] = []
    const mismatch = voiceMismatchNote(result.requested, result.voice)
    if (mismatch) notes.push(mismatch)
    if (existed) notes.push("目标路径已有同名文件，本次已覆盖。")
    if (rate <= -60 || rate >= 120) notes.push("语速偏离自然语速较多，试听确认可懂度。")

    // 本机扬声器播报（play）：合成照常完成，播报被拦时只加说明——产物仍归用户
    let played = false
    if (args.play === true) {
      const blocked = playbackBlockedReason(ctx)
      if (blocked) {
        notes.push(blocked)
      } else {
        const playRun = await runTtsScript(ttsDeps(ctx), { mode: "play", engine, wav: outAbs })
        if (playRun.result?.ok) played = true
        else notes.push(scriptFailureNote(playRun, engine))
      }
    }

    const output = [
      `已合成语音：${outRel}`,
      `音色 ${result.voice}（${result.lang ?? "未知语言"}），引擎 ${result.engine}，时长 ${formatDuration(duration)}，大小 ${formatBytes(bytes)}`,
      played ? "已在运行 GEBAI 的这台机器的扬声器上开始播报（后台播放，可继续其他操作）。" : "",
      notes.length ? `注意：${notes.join(" ")}` : "",
    ]
      .filter(Boolean)
      .join("\n")
    return {
      output,
      data: { path: outRel, engine: result.engine, voice: result.voice, bytes, durationSec: duration, played },
      blocks,
    }
  },
}

export const voicesTool: Tool = {
  name: "voices",
  safeMode: false,
  description:
    "列出本机可用的离线语音音色（Windows 系统语音；可选 find 按音色名或语言关键词过滤，如「慧慧」「zh」「Kangkang」）。合成（tts_speak）的 voice 参数取这里的名称或其一词即可。",
  parameters: schema({
    find: { type: "string", description: "关键词过滤：匹配音色名或语言（不区分大小写），如 慧慧 / zh / english" },
    limit: { type: "number", description: "最多返回条数（默认 50）" },
  }),
  outputSchema: schema({
    engine: { type: "string", description: "实际使用的引擎：winrt / sapi" },
    voices: {
      type: "array",
      items: schema(
        { name: { type: "string" }, lang: { type: "string" }, gender: { type: "string" } },
        ["name", "lang"],
      ),
    },
  }),
  async execute(args, ctx) {
    if (!isSupportedPlatform()) return { output: UNSUPPORTED_PLATFORM_NOTE }
    const engine = engineFromEnv(ctx.env)
    const run = await runTtsScript(ttsDeps(ctx), { mode: "voices", engine })
    if (!run.result?.ok) return { output: scriptFailureNote(run, engine) }
    const all = run.result.voices ?? []
    const limit = Math.max(1, Math.min(200, Math.round(Number(args.limit) || 50)))
    const matched = filterVoices(all, args.find ? String(args.find) : undefined)
    if (!matched.length) {
      return {
        output: `没有匹配「${String(args.find ?? "")}」的音色。本机共 ${all.length} 个可用音色：\n${formatVoiceList(all.slice(0, limit))}`,
      }
    }
    const shown = matched.slice(0, limit)
    const tail = matched.length > shown.length ? `\n…（共 ${matched.length} 个匹配，已列出前 ${shown.length} 个）` : ""
    return {
      output: `本机可用音色（引擎 ${run.result.engine}，共 ${all.length} 个${args.find ? `，匹配 ${matched.length} 个` : ""}）：\n${formatVoiceList(shown)}${tail}`,
      data: { engine: run.result.engine, voices: shown },
    }
  },
}

/**
 * 跑一次神经合成：逐个解释器候选尝试，返回首个成功的运行结果。
 *
 * 为何逐个试而不探测一次：失败原因多样（venv 不存在 / 该 python 没装 sherpa-onnx /
 * 沙箱下不可达），逐候选实测比“猜哪个能用”可靠；代价是一次失败启动，可忽略。
 * 失败时不报错而是返回最后一次报告 + 已试列表，由调用方统一转成可读说明。
 */
async function runNeural(
  ctx: ToolContext,
  opts: { modelDir: string; text: string; speaker: number; speed: number; outAbs: string },
): Promise<{ ok: boolean; report: Record<string, unknown>; note: string; usedPython: string }> {
  const script = neuralTtsScript({
    modelDir: opts.modelDir,
    textBase64: Buffer.from(opts.text, "utf8").toString("base64"),
    speaker: opts.speaker,
    speed: opts.speed,
    outPath: opts.outAbs,
  })
  const candidates = pythonCandidates(resolveGebaiHome())
  const tmpDir = ctx.resolvePath(TTS_OUT_DIR)
  const file = writeTempScript(tmpDir, script, "synth")
  let last: { report: Record<string, unknown>; note: string; python: string } | null = null
  try {
    for (const python of candidates) {
      let stdout = ""
      let stderr = ""
      let spawnError = ""
      try {
        const run = await ctx.runCommand(pythonCommand(python, file.path), {
          timeoutMs: 240_000,
          signal: ctx.signal,
        })
        stdout = String(run.stdout ?? "")
        // ## stderr 必须收集（实测代价：一轮无法定位的失败）
        //
        // 原先只看 stdout。而 python 的 **ImportError / 运行时异常全在 stderr**——
        // 脚本在打印报告行之前就死了（比如 venv 里 import sherpa_onnx 失败、
        // 或依赖 DLL 加载不到），stdout 就是空的，于是工具只能报
        // 「python 未返回报告」——而真正的 traceback 已丢。
        // 实测：同一模型手搓脚本正常，但工具恒报 no-report，无法查因。
        stderr = String(run.stderr ?? "")
      } catch (err) {
        spawnError = String((err as Error)?.message ?? err)
      }
      if (spawnError) {
        last = { report: { error: "spawn-failed", detail: spawnError }, note: spawnError, python }
        continue
      }
      // 脚本最后一行是 JSON 报告；前面可能有 numpy/ort 的无关输出，从后往前找第一行可解析的 JSON
      let parsed: Record<string, unknown> | null = null
      for (const line of stdout.trim().split("\n").reverse()) {
        const trimmed = line.trim()
        if (!trimmed.startsWith("{")) continue
        try {
          parsed = JSON.parse(trimmed) as Record<string, unknown>
          break
        } catch {
          /* 不是报告行，继续往上找 */
        }
      }
      if (parsed?.ok === true) {
        return { ok: true, report: parsed, note: "", usedPython: python }
      }
      if (!parsed) {
        // 没有报告行：python 没跑起来，或跑到一半就抛了。
        // **把 stderr 尾部一起带上**——那才是真因（Traceback / ImportError）。
        // 只报“python 未返回报告”等于把唯一的线索丢掉。
        const tail = stderr.trim().split("\n").slice(-6).join("\n")
        last = {
          report: {
            error: "no-report",
            detail: { stdout: stdout.slice(-400), stderr: tail },
          },
          note: tail || stdout.slice(-400) || "python 未返回报告",
          python,
        }
        continue
      }
      last = { report: parsed, note: String(parsed.detail ?? parsed.error ?? ""), python }
      // 缺依赖 → 换下一个解释器；其它错误（模型坏、文本错）再换也没用，直接停
      const code = String(parsed.error ?? "")
      if (code === "missing-dependency") continue
      break
    }
  } finally {
    file.cleanup()
  }
  return {
    ok: false,
    report: last?.report ?? { error: "no-python" },
    note: last?.note ?? "未找到可用的 Python 解释器（需要 python3 + sherpa-onnx）",
    usedPython: last?.python ?? "",
  }
}

export const modelsTool: Tool = {
  name: "models",
  safeMode: false,
  description:
    "列出已安装的**神经语音模型**（本地 VITS + onnxruntime，中文音质接近云端合成）及其音色数量，" +
    "并提示依赖（sherpa-onnx）是否就绪。合成时 `tts_speak` 传 engine=neural，`model` 取这里的 id、`speaker` 取音色序号。",
  parameters: schema({}),
  outputSchema: schema({
    models: { type: "array", items: schema({ id: { type: "string" }, bytes: { type: "number" } }, ["id"]) },
    root: { type: "string", description: "模型根目录" },
  }),
  async execute(_args, ctx) {
    const models = scanNeuralModels()
    const root = neuralTtsRoots()[0] ?? ""
    const picked = pickNeuralModel(models)
    const python = pythonCandidates(resolveGebaiHome())[0] ?? "python"
    // 探一下依赖：只 import，不加载模型（毫秒级）
    let dep = "未知"
    try {
      const r = await ctx.runCommand(`"${python}" -c "import sherpa_onnx; print(getattr(sherpa_onnx,'__version__','ok'))"`, {
        timeoutMs: 30_000,
        signal: ctx.signal,
      })
      dep = String(r.stdout ?? "").trim() || "未知"
      if (!dep) dep = "未知"
    } catch (err) {
      dep = `不可用（${String((err as Error)?.message ?? err).slice(0, 120)}）`
    }
    const lines: string[] = []
    if (models.length === 0) {
      lines.push(
        `未找到已安装的神经语音模型（模型根：${root || "未解析"}）。`,
        "用 tts_install 下载（中文 VITS，约 120MB / 个），或手动把 sherpa-onnx 兼容的模型目录放进去：",
        "  {GEBAI_HOME}/resources/models/tts/<模型名>/  ← 需含 *.onnx + tokens.txt + lexicon.txt",
      )
    } else {
      lines.push(`已安装神经语音模型（${models.length} 个），模型根：${root}`)
      for (const m of models) {
        const mark = picked?.id === m.id ? "  ← 默认" : ""
        lines.push(`· ${m.id}（${(m.bytes / 1048576).toFixed(0)}MB）${mark}`)
      }
      lines.push("", "用法：tts_speak(engine=\"neural\", model=\"<id>\", speaker=<序号>, text=\"...\"）")
    }
    lines.push("", `sherpa-onnx 依赖：${dep}（解释器 ${python}）`)
    return {
      output: lines.join("\n"),
      data: { models: models.map((m) => ({ id: m.id, bytes: m.bytes })), root },
    }
  },
}

/**
 * 模型下载源。
 *
 * 缺省 `hf-mirror.com`（公网可用的 HuggingFace 镜像）；合规/内网环境用
 * `HF_ENDPOINT` 指向自建镜像（如 `https://<内网域名>/hf`）——否则下载必然超时，
 * 而错误信息只会是「连接超时」，看不出是地址问题而不是模型问题。
 *
 * 末尾斜杠先去掉：两种写法（带/不带）都应能拼出正确 URL。
 */
export function hfEndpoint(): string {
  const raw = process.env.HF_ENDPOINT?.trim()
  const base = raw && raw.length > 0 ? raw : "https://hf-mirror.com"
  return base.replace(/\/+$/, "")
}

/**
 * 拼一条「以解释器路径开头」的命令行。
 *
 * ## 为何 Windows 下必须加 `&` 调用运算符
 *
 * `ctx.runCommand` 在 Windows 上是 **`shell: true`**（走 PowerShell 解析），
 * 而 PowerShell 里**以引号开头的行是字符串表达式、不是命令**：
 *
 * ```
 * "C:\path with space\python.exe" -X utf8 a.py
 * # → Unexpected token '-X' in expression or statement.
 * ```
 *
 * 于是整条命令根本没启动，stdout 为空、stderr 只有一段 ParserError。
 * 用户侧看到的是「找不到可用的 Python 解释器」——而解释器明明就在。
 *
 * 加 `&`（调用运算符）后 PowerShell 才把它当命令执行：
 * `& "C:\path\python.exe" -X utf8 a.py`。
 *
 * POSIX shell 里 `&` 是后台运算符，不能前缀，所以只在 Windows 加。
 */
export function pythonCommand(python: string, args: string): string {
  const base = `"${python}" ${args}`
  return process.platform === "win32" ? `& ${base}` : base
}

/** 可下载的官方中文模型（sherpa-onnx 发布，镜像可用、不需要 torch）。 */
const DOWNLOADABLE_MODELS: Record<string, { repo: string; onnx: string; note: string }> = {
  "vits-melo-zh": {
    repo: "csukuangfj/vits-melo-tts-zh_en",
    onnx: "model.onnx",
    note: "MeloTTS 中文 · 44.1kHz · 单音色 —— 采样率最高，旁白首选",
  },
  "vits-zh-eula": {
    repo: "csukuangfj/vits-zh-hf-eula",
    onnx: "eula.onnx",
    note: "女声 · 22.05kHz · 804 音色",
  },
  "vits-zh-theresa": {
    repo: "csukuangfj/vits-zh-hf-theresa",
    onnx: "theresa.onnx",
    note: "女声 · 22.05kHz · 804 音色",
  },
  "vits-zh-fanchen-C": {
    repo: "csukuangfj/vits-zh-hf-fanchen-C",
    onnx: "vits-zh-hf-fanchen-C.onnx",
    note: "女声 · 16kHz · 187 音色",
  },
}

export const installTool: Tool = {
  name: "install",
  safeMode: false,
  description:
    "安装神经语音合成所需的东西：① pip 装 sherpa-onnx（Python 侧车 venv）；② 从镜像下载中文 VITS 模型到 " +
    "{GEBAI_HOME}/resources/models/tts/。两者都可单独装（参数控制）。下载量约 120MB/模型，装完用 tts_models 核实。",
  parameters: schema({
    model: {
      type: "string",
      description: "要下载的模型 id（vits-zh-theresa / vits-zh-fanchen-C）；缺省只装依赖不下载模型",
    },
    skip_dependency: { type: "boolean", description: "true = 只下载模型，不装 pip 依赖" },
  }),
  async execute(args, ctx) {
    const out: string[] = []
    const python = pythonCandidates(resolveGebaiHome())[0] ?? "python"
    if (args.skip_dependency !== true) {
      try {
        const r = await ctx.runCommand(pythonCommand(python, "-m pip install -q sherpa-onnx"), {
          timeoutMs: 600_000,
          signal: ctx.signal,
        })
        out.push(`sherpa-onnx 安装：${String(r.stdout ?? "").trim() ? "有输出" : "完成"}`)
      } catch (err) {
        out.push(`sherpa-onnx 安装失败：${String((err as Error)?.message ?? err).slice(0, 300)}`)
      }
    }
    const wanted = String(args.model ?? "").trim()
    if (wanted) {
      const spec = DOWNLOADABLE_MODELS[wanted]
      if (!spec) {
        return {
          output: `不认识的模型「${wanted}」。可下载：${Object.keys(DOWNLOADABLE_MODELS).join(" / ")}`,
        }
      }
      const root = neuralTtsRoots()[0]!
      const dir = join(root, wanted)
      mkdirSync(dir, { recursive: true })
      // 必需件（缺了模型就不完整）与可选件（部分仓库不带，缺了不影响合成）分开：
      // 必需件下载失败要说清楚，可选件 404 属正常，不能拿它当失败。
      const required = [spec.onnx, "tokens.txt", "lexicon.txt"]
      // 可选件：部分仓库不带（404 正常）；Kokoro 系还需 data_dir/dict_dir，走手工安装
      const optional = [
        "date.fst",
        "number.fst",
        "phone.fst",
        "new_heteronym.fst",
        "date-zh.fst",
        "number-zh.fst",
        "phone-zh.fst",
      ]
      const files = [...required.map((f) => ({ f, must: true })), ...optional.map((f) => ({ f, must: false }))]
      const mirror = hfEndpoint()
      out.push(`下载源：${mirror}`)
      for (const { f: name, must } of files) {
        // 不能只靠文件名就断定仓库里叫这个：`tts_models` 扫描只认**已落盘**的名字，
        // 而模型的真实文件名由上游仓库决定（MeloTTS 的 `model.onnx`、theresa 的
        // `theresa.onnx`、fanchen 的 `vits-zh-hf-fanchen-C.onnx` 各不相同）。
        // 上游改名/写错时，旧写法只会报一个 HTTP 404，看不出是“源不通”还是“名字不对”。
        const url = `${mirror}/${spec.repo}/resolve/main/${name}`
        try {
          const resp = await fetch(url, { signal: AbortSignal.timeout(600_000) })
          if (!resp.ok) {
            out.push(`  ${name}: HTTP ${resp.status}${must ? "（必需件，安装不完整）" : "（可选件，跳过）"}`)
            continue
          }
          writeFileSync(join(dir, name), Buffer.from(await resp.arrayBuffer()))
        } catch (err) {
          out.push(`  ${name}: ${String((err as Error)?.message ?? err).slice(0, 160)}`)
        }
      }
      const got = scanNeuralModels().some((m) => m.id === wanted)
      out.push(got ? `模型 ${wanted} 已就绪（${spec.note}）→ ${dir}` : `模型 ${wanted} 下载不完整，再试一次 tts_install`)
    }
    out.push("完成后用 tts_models 核实；合成：tts_speak(engine=\"neural\", ...)。")
    return { output: out.join("\n") }
  },
}

export const name = "tts"
export const description =
  "语音合成（文本转语音）与音效。**两档离线语音**：神经 TTS（sherpa-onnx + VITS 中文模型，音质接近云端合成、可选上百音色、22.05kHz，推荐用于宣传片/口播/有声内容）与 Windows 系统语音（零安装、任意 Windows 可用，作为兜底）；均不联网、不耗配额。另含音效合成、音频效果处理与多轨混音。产物 WAV 可直接在聊天内播放。输入：待合成文本 / 音效需求 / 音频处理需求。"
export const systemPrompt = systemPromptBase

export const tools = {
  speak: speakTool,
  voices: voicesTool,
  models: modelsTool,
  install: installTool,
  sfx: sfxTool,
  effect: effectTool,
  mix: mixTool,
}
export const preload = false

export const envVars = [
  { name: "TTS_VOICE", description: "默认音色名（tts_speak 未传 voice 时使用；可写完整名或关键词，如 慧慧）" },
  {
    name: "TTS_ENGINE",
    description: "默认语音引擎：neural（神经 TTS，音质最好）/ auto（WinRT 优先 SAPI 回退）/ winrt / sapi",
  },
  {
    name: "TTS_MODEL",
    description: "默认神经模型 id（engine=neural 且未传 model 时使用；缺省自动选最优，见 tts_models）",
  },
  {
    name: "HF_ENDPOINT",
    description:
      "模型下载源（tts_install 用）：缺省 https://hf-mirror.com；"
      + "合规/内网环境指向自建镜像（如 https://<内网域名>/hf），否则下载必然超时",
  },
]

export const def: SubAgentDef = { name, description, systemPrompt, tools, preload, envVars }
