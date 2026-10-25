/**
 * 神经语音合成（neural TTS）：VITS 声学模型的 onnxruntime 原生推理。
 *
 * ## 与系统语音引擎的分工
 *
 * 系统语音（WinRT OneCore / SAPI5，见 `speech.ts`）**零安装、任意 Windows 可用**，是本能力的兜底；
 * 但它的音色是十年前的拼接式韵律，中文宣传片/口播这类对听感有要求的场合明显不够。
 * 本模块提供第二档：**本地神经网络 TTS**——音质接近云端合成的水平，仍然离线、不联网、不耗配额。
 *
 * ## 为什么是 sherpa-onnx + VITS
 *
 * 选型约束（按优先级）：① 离线；② **不依赖 PyTorch**（装 torch 动辄 2GB+，且与用户机器上的
 * CUDA 版本互相绑架）；③ 中文可用；④ 音色可选。满足这四条的最小实现是
 * **VITS 导出 ONNX + sherpa-onnx 运行时**：
 * - 模型是单个 `model.onnx` + `tokens.txt` + `lexicon.txt` + 若干 `*.fst`（中文用词典式
 *   音素化 + fst 后处理，不需要 espeak-ng），全套约 120MB；
 * - 推理只走 onnxruntime CPU，22.05kHz 合成比实时快（实测 4.3s 音频约 1.4s，4 线程）。
 *
 * ## 依赖与降级
 *
 * Python 侧只需 `sherpa-onnx`（wheel 内含 onnxruntime，无 torch）。
 * 缺依赖/缺模型时**如实说明并指向兜底路径**，不静默回落到系统语音——
 * 静默回落会让「音质没变」看起来像模型没生效。
 */
import { existsSync, mkdirSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { resolveGebaiHome } from "../shared/config"

/** 模型根目录名（`{GEBAI_HOME}/resources/models/tts`）。 */
export const NEURAL_TTS_DIRNAME = ["resources", "models", "tts"] as const
/**
 * 模型目录内的必需文件（相对模型目录）。
 *
 * 中文模型的音素化有两条路：词典式（`lexicon.txt`）与 espeak 式（`espeak-ng-data/`），
 * 满足**任一**即可——早期只认 lexicon，会把需要 espeak 的模型误判为不可用。
 */
export const NEURAL_REQUIRED_FILES = ["tokens.txt"] as const

/** 音素化资源：二者有其一即算齐全。 */
const PHONEME_SOURCES = ["lexicon.txt", "espeak-ng-data"] as const

/** 一个可用的神经语音模型（目录名即 `model` 参数取值）。 */
export interface NeuralVoiceModel {
  /** 目录名（选择模型时用它）。 */
  id: string
  /** 模型文件绝对路径。 */
  onnx: string
  /** 采样率由模型决定，此处仅描述（读取时以运行时为准，此处给已知值便于展示）。 */
  sampleRate?: number
  /** 说话人数量（0 = 未知/单说话人）。 */
  speakers?: number
  /** 目录体积（字节）。 */
  bytes: number
}

/** 模型根候选目录（`{GEBAI_HOME}/resources/models/tts` → 历史 `models/tts`）。 */
export function neuralTtsRoots(): string[] {
  const home = resolveGebaiHome()
  if (!home) return []
  return [join(home, ...NEURAL_TTS_DIRNAME), join(home, "models", "tts")]
}

/**
 * 扫描已安装的神经语音模型：`{root}/{id}/*.onnx` + tokens/lexicon 齐全即视为可用。
 *
 * 为什么按「文件齐全」判定而不是维护一份清单：模型是**外部下载物**（可由用户自行换成
 * 任意 sherpa-onnx 兼容的 VITS 中文模型），硬编码清单会让「我放进去的新模型不生效」成为谜。
 */
export function scanNeuralModels(): NeuralVoiceModel[] {
  const found: NeuralVoiceModel[] = []
  for (const root of neuralTtsRoots()) {
    if (!existsSync(root)) continue
    let entries: string[]
    try {
      entries = readdirSync(root)
    } catch {
      continue
    }
    for (const id of entries) {
      const dir = join(root, id)
      try {
        if (!statSync(dir).isDirectory()) continue
      } catch {
        continue
      }
      const onnx = readdirSync(dir).filter((f) => f.endsWith(".onnx"))
      // 允许多个 onnx：Vocoder/声码器与主模型分开放（如 melo 的 model.onnx 与 model.int8.onnx，
      // 或未来主模型 + 独立声码器）——取最大体积那个作为主模型，其余视为附属件。
      if (onnx.length === 0) continue
      if (!NEURAL_REQUIRED_FILES.every((f) => existsSync(join(dir, f)))) continue
      if (!PHONEME_SOURCES.some((f) => existsSync(join(dir, f)))) continue
      const sized = onnx
        .map((f) => ({ f, size: statSync(join(dir, f)).size }))
        .sort((a, b) => b.size - a.size)
      const main = sized.find((s) => !s.f.includes("int8")) ?? sized[0]!
      let bytes = 0
      for (const f of readdirSync(dir)) {
        try {
          bytes += statSync(join(dir, f)).size
        } catch {
          /* 单文件读不到不影响整体可用性 */
        }
      }
      found.push({ id, onnx: join(dir, main.f), bytes })
    }
    if (found.length > 0) break
  }
  // 21.05kHz 系列（theresa 等）按体积与命名规律无法可靠推断采样率，交给运行时回报。
  return found.sort((a, b) => a.id.localeCompare(b.id))
}

/** 默认模型 id 的偏好顺序（先优质，再小体积）。 */
const PREFERRED_MODEL_ORDER = ["vits-zh-theresa", "vits-zh-fanchen-C"]

/** 挑默认模型：优先偏好列表里存在的，否则取第一个可用模型。 */
export function pickNeuralModel(models: readonly NeuralVoiceModel[]): NeuralVoiceModel | null {
  if (models.length === 0) return null
  for (const id of PREFERRED_MODEL_ORDER) {
    const hit = models.find((m) => m.id === id)
    if (hit) return hit
  }
  return models[0]!
}

/** 按 id/关键词挑模型（支持「theresa」「fanchen」这类子串；未命中返回 null）。 */
export function matchNeuralModel(
  models: readonly NeuralVoiceModel[],
  query: string,
): NeuralVoiceModel | null {
  const q = query.trim().toLowerCase()
  if (!q) return pickNeuralModel(models)
  return (
    models.find((m) => m.id.toLowerCase() === q) ??
    models.find((m) => m.id.toLowerCase().includes(q)) ??
    null
  )
}

/**
 * 生成合成脚本（在 Python 侧车 venv 里执行）。
 *
 * 为什么脚本由 TS 拼接而不是内置 .py 文件：参数量少（模型目录/文本/说话人/语速/输出），
 * 拼接一段自包含脚本可以避免「二进制形态下 .py 不在包里」的发布问题。
 * 文本一律经 base64 传递——命令行参数里的中文/引号/换行在 Windows 上会被 shell 拆开。
 */
export function neuralTtsScript(input: {
  modelDir: string
  textBase64: string
  speaker: number
  speed: number
  outPath: string
}): string {
  const dir = JSON.stringify(input.modelDir)
  const out = JSON.stringify(input.outPath)
  return `
import base64, json, sys, wave
import numpy as np
import sherpa_onnx

DIR = ${dir}
OUT = ${out}
TEXT = base64.b64decode(${JSON.stringify(input.textBase64)}).decode("utf-8")

report = {"ok": False}
try:
    onnx = None
    import os
    for name in os.listdir(DIR):
        if name.endswith(".onnx"):
            onnx = os.path.join(DIR, name)
            break
    if onnx is None:
        report["error"] = "no-model-file"
        print(json.dumps(report)); sys.exit(0)

    fsts = []
    for name in ("date.fst", "number.fst", "phone.fst", "new_heteronym.fst"):
        p = os.path.join(DIR, name)
        if os.path.exists(p):
            fsts.append(p)

    cfg = sherpa_onnx.OfflineTtsConfig(
        model=sherpa_onnx.OfflineTtsModelConfig(
            vits=sherpa_onnx.OfflineTtsVitsModelConfig(
                model=onnx,
                tokens=os.path.join(DIR, "tokens.txt"),
                lexicon=os.path.join(DIR, "lexicon.txt"),
            ),
            num_threads=${input.speaker >= 0 ? 4 : 4},
        ),
        rule_fsts=",".join(fsts),
    )
    if not cfg.validate():
        report["error"] = "invalid-config"
        print(json.dumps(report)); sys.exit(0)

    tts = sherpa_onnx.OfflineTts(cfg)
    audio = tts.generate(TEXT, sid=${input.speaker}, speed=${input.speed})
    # 空音频必须当失败（不是“成功但 0 秒”）：空文本等情形会返回空样本，
    # 若按成功上报，调用方会交付一个 0 秒的 WAV 并以为合成完了（实测踩到）。
    if audio is None or len(audio.samples) == 0:
        report["error"] = "empty-audio"
        report["detail"] = "empty samples (speaker out of range or empty text?)"
        report["numSpeakers"] = int(tts.num_speakers)
        print(json.dumps(report)); sys.exit(0)

    pcm = (np.clip(audio.samples, -1.0, 1.0) * 32767.0).astype(np.int16)
    with wave.open(OUT, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(audio.sample_rate)
        f.writeframes(pcm.tobytes())

    report.update({
        "ok": True,
        "sampleRate": audio.sample_rate,
        "samples": len(audio.samples),
        "durationSec": len(audio.samples) / float(audio.sample_rate),
        "numSpeakers": tts.num_speakers,
        "requestedSpeaker": ${input.speaker},
        "model": os.path.basename(onnx),
    })
except ImportError as exc:
    report["error"] = "missing-dependency"
    report["detail"] = str(exc)
except Exception as exc:  # 成因多（模型损坏 / 音素化失败 / 显存不足），原文回传便于定位
    report["error"] = "runtime-error"
    report["detail"] = f"{type(exc).__name__}: {exc}"

print(json.dumps(report))
`
}

/**
 * 把脚本写成临时文件再把路径交给 python（**不用 `-c`**）。
 *
 * 为何不能用 `-c`：Windows shell 会把参数里的换行当续行符，而把脚本转成命令行参数时
 * 换行往往变成 `\n` 字面量——python 收到的是一个满屏转义符的单行串，直接 SyntaxError（实测）。
 * 临时文件还能同时避开命令行长度上限（模型路径 + 长文本很容易超）。
 *
 * 返回 cleanup，调用方务必在 finally 里调用（无论成败）。
 */
export function writeTempScript(
  dir: string,
  script: string,
  tag: string,
): { path: string; cleanup: () => void } {
  mkdirSync(dir, { recursive: true })
  const file = join(dir, `neural-${tag}-${Date.now()}-${Math.random().toString(36).slice(2, 8)}.py`)
  writeFileSync(file, script, "utf8")
  return { path: file, cleanup: () => rmSync(file, { force: true }) }
}

/**
 * Python 解释器候选链（神经推理跑在这里）。
 *
 * 优先用**客卿共享 venv**（`keqing/python/venv`）：那里面已经装了 onnxruntime，
 * 且与 vision 等其它客卿共用一份依赖，避免每个能力各建一个 venv（每份 onnxruntime 几十 MB）。
 * 而后才是系统 python。返回的候选由调用方逐个试（前一个跑不通就换下一个）。
 */
export function pythonCandidates(home: string | null): string[] {
  const out: string[] = []
  const push = (p: string) => {
    if (p && !out.includes(p)) out.push(p)
  }
  if (home) {
    // {GEBAI_HOME} 为仓库根（脚本调试模式）时的客卿语言目录
    push(join(home, "keqing", "python", "venv", "Scripts", "python.exe"))
    push(join(home, "keqing", "python", "venv", "bin", "python"))
    // 二进制/安装形态：客卿源物化在 vendor/keqing 下
    push(join(home, "vendor", "keqing", "python", "venv", "Scripts", "python.exe"))
    push(join(home, "vendor", "keqing", "python", "venv", "bin", "python"))
  }
  push("python")
  push("python3")
  return out
}

/** 脚本回报的可读失败说明（面向模型/用户，含下一步动作）。 */
export function neuralFailureNote(raw: unknown): string {
  // ## `detail` 可能是对象，必须展开（实测代价：一整轮无法定位）
  //
  // `runNeural` 的 no-report 分支把 `detail` 写成 `{ stdout, stderr }` —— 那正是
  // 最需要看的两段线索。而旧代码只 `String(raw)`，对象就变成 `[object Object]`：
  // 正则匹配不到任何分支（连 `no-report` 都匹配不上），最后落到兵底文案
  // 「神经语音合成失败。原文：[object Object]」——**真因彻底消失**。
  const normalized = (() => {
    if (raw == null) return ""
    if (typeof raw === "string") return raw
    try {
      return JSON.stringify(raw)
    } catch {
      return String(raw)
    }
  })().trim()
  // 可读尾段：对象里的 stdout/stderr 分别标出来（不要塞一坨 JSON 给模型）
  const readable = (() => {
    if (raw != null && typeof raw === "object") {
      const o = raw as Record<string, unknown>
      const parts: string[] = []
      if (typeof o.error === "string") parts.push(o.error)
      // note 是“人能读的那一句”（如 python 的 traceback 尾部），优先级高于 detail
      if (typeof o.note === "string" && o.note.trim()) parts.push(o.note.trim())
      const d = o.detail
      if (typeof d === "string") parts.push(d)
      else if (d && typeof d === "object") {
        const dd = d as Record<string, unknown>
        if (dd.stderr) parts.push(`stderr:\n${String(dd.stderr)}`)
        if (dd.stdout) parts.push(`stdout:\n${String(dd.stdout)}`)
      }
      if (parts.length > 0) return parts.join("\n")
    }
    return normalized
  })()
  const text = normalized
  const tail = readable ? `\n原文：${readable.slice(-600)}` : ""
  if (/missing-dependency|No module named 'sherpa_onnx'/.test(text)) {
    return (
      "神经语音合成不可用：Python 侧车缺少 `sherpa-onnx`。安装：`tts_install` 或 " +
      "`keqing/python/venv/Scripts/python.exe -m pip install sherpa-onnx`。\n" +
      "装上之后即可用 `tts_speak`（engine=neural）合成；系统语音（engine=auto）不受影响。" +
      tail
    )
  }
  if (/no-model-file|invalid-config|missing tokens|lexicon/.test(text)) {
    return (
      "神经语音模型不完整：模型目录内需要恰好一个 `.onnx` 且同时有 `tokens.txt`、`lexicon.txt`。" +
      "用 `tts_models` 查看当前可用的模型目录。" +
      tail
    )
  }
  if (/empty-audio|num_speakers|speaker/.test(text)) {
    return `神经语音合成失败（可能是 speaker 超出该模型的音色范围）：先用 tts_models 查音色数量。${tail}`
  }
  if (/no-report|spawn-failed/.test(text)) {
    return (
      "神经语音合成没能启动：找不到可用的 Python 解释器（或它无法执行脚本）。" +
      "用 tts_install 装依赖与模型，或检查沙箱是否允许子进程。" +
      tail
    )
  }
  if (/runtime-error/.test(text)) {
    return `神经语音合成出错（推理阶段）：模型文件可能不完整或与该 sherpa-onnx 版本不兼容。${tail}`
  }
  return `神经语音合成失败。${tail}`
}
