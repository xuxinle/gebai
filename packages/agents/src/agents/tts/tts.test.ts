/**
 * tts 子Agent 用例：纯函数与工具契约全覆盖，零网络、零外部依赖——脚本执行一律走假 runCommand
 * （模拟 PowerShell 写结果 JSON / 产物 WAV），不真起进程、不依赖本机语音引擎。
 * 平台判定经 setTtsPlatform 注入，不随宿主平台漂移。
 */
import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { def, speakTool, voicesTool } from "./tts"
import {
  TTS_MAX_TEXT,
  TTS_PLAY_SCRIPT,
  TTS_SCRIPT,
  clampPercent,
  defaultOutPath,
  encodePowerShellCommand,
  escapeXml,
  filterVoices,
  formatBytes,
  formatDuration,
  formatPercent,
  formatVoiceList,
  isSupportedPlatform,
  normalizeEngine,
  parseScriptResult,
  playbackBlockedReason,
  scriptFailureNote,
  setTtsPlatform,
  validateText,
  voiceMismatchNote,
  type TtsVoice,
} from "../../core/tts/speech"
import { makeCtx, scriptStub } from "./test-ctx"

beforeAll(() => setTtsPlatform("win32"))
afterAll(() => setTtsPlatform(undefined))

const roots: string[] = []
function tempHome(): string {
  const dir = mkdtempSync(join(tmpdir(), "gebai-tts-"))
  roots.push(dir)
  return dir
}
afterAll(() => {
  for (const dir of roots) rmSync(dir, { recursive: true, force: true })
})

const VOICES: TtsVoice[] = [
  { name: "Microsoft Huihui", lang: "zh-CN", gender: "Female", engine: "winrt" },
  { name: "Microsoft Kangkang", lang: "zh-CN", gender: "Male", engine: "winrt" },
  { name: "Microsoft Zira", lang: "en-US", gender: "Female", engine: "winrt" },
]

const okPayload = {
  ok: true,
  engine: "winrt",
  voice: "Microsoft Huihui",
  lang: "zh-CN",
  bytes: 2048,
  sampleRate: 16000,
  durationSec: 12.5,
}

describe("参数归一", () => {
  test("clampPercent：非数字回落、超范围钳制、取整", () => {
    expect(clampPercent(80, { min: -100, max: 200 })).toBe(80)
    expect(clampPercent("80", { min: -100, max: 200 })).toBe(80)
    expect(clampPercent(999, { min: -100, max: 200 })).toBe(200)
    expect(clampPercent(-999, { min: -100, max: 200 })).toBe(-100)
    expect(clampPercent(12.6, { min: -100, max: 200 })).toBe(13)
    expect(clampPercent(undefined, { min: -100, max: 200 })).toBe(0)
    expect(clampPercent("abc", { min: -100, max: 200 })).toBe(0)
    expect(clampPercent("abc", { min: -100, max: 200 }, 7)).toBe(7)
  })

  test("formatPercent：带符号百分比", () => {
    expect(formatPercent(0)).toBe("+0%")
    expect(formatPercent(80)).toBe("+80%")
    expect(formatPercent(-50)).toBe("-50%")
  })

  test("normalizeEngine：合法值直取、非法/缺省回落 auto", () => {
    expect(normalizeEngine("winrt")).toBe("winrt")
    expect(normalizeEngine("SAPI")).toBe("sapi")
    expect(normalizeEngine("auto")).toBe("auto")
    expect(normalizeEngine("nope")).toBe("auto")
    expect(normalizeEngine(undefined, "sapi")).toBe("sapi")
  })

  test("平台判定按 win32 收敛", () => {
    expect(isSupportedPlatform("win32")).toBe(true)
    expect(isSupportedPlatform("linux")).toBe(false)
    expect(isSupportedPlatform("darwin")).toBe(false)
    expect(isSupportedPlatform()).toBe(true) // 注入 win32 后
  })
})

describe("文本校验与转义", () => {
  test("validateText：空/空白/超长/非字符串/正常", () => {
    expect(validateText("")).toContain("为空")
    expect(validateText("   \n ")).toContain("为空")
    expect(validateText(undefined)).toContain("为空")
    expect(validateText(123)).toContain("为空")
    expect(validateText("a".repeat(TTS_MAX_TEXT + 1))).toContain("过长")
    expect(validateText("a".repeat(TTS_MAX_TEXT))).toBeNull()
    expect(validateText("你好")).toBeNull()
  })

  test("escapeXml：& < > 与换行转义（SSML 内容位）", () => {
    expect(escapeXml("A & B")).toBe("A &amp; B")
    expect(escapeXml("<test>")).toBe("&lt;test&gt;")
    expect(escapeXml("a\r\nb\nc")).toBe("a&#10;b&#10;c")
    expect(escapeXml("正常文本，无特殊字符")).toBe("正常文本，无特殊字符")
    // & 先转义：已是实体的文本不会二次转义成非法形式
    expect(escapeXml("&amp;")).toBe("&amp;amp;")
  })

  test("脚本常量不含宿主模板串插值序列（PowerShell 变量不会被 TS 插值）", () => {
    expect(TTS_SCRIPT.includes("${")).toBe(false)
    expect(TTS_SCRIPT.includes("`")).toBe(false)
    expect(TTS_SCRIPT.startsWith("$ErrorActionPreference = 'Stop'")).toBe(true)
    // 引擎与模式分支的关键标记在位
    for (const marker of ["Windows.Media.SpeechSynthesis.SpeechSynthesizer", "System.Speech", "SynthesizeSsmlToStreamAsync", "SpeakSsml", "xml:lang", "GEBAI_TTS_RESULT"]) {
      expect(TTS_SCRIPT).toContain(marker)
    }
  })
})

describe("产物路径与展示", () => {
  test("defaultOutPath：tmp/tts 下带时间戳的 wav", () => {
    expect(defaultOutPath(1700000000000)).toBe("tmp/tts/voice-1700000000000.wav")
  })

  test("formatDuration：秒/分转换", () => {
    expect(formatDuration(12.4)).toBe("12 秒")
    expect(formatDuration(83)).toBe("1 分 23 秒")
    expect(formatDuration(120)).toBe("2 分")
    expect(formatDuration(0)).toBe("0 秒")
  })

  test("formatBytes：B/KB/MB", () => {
    expect(formatBytes(512)).toBe("512 B")
    expect(formatBytes(2048)).toBe("2 KB")
    expect(formatBytes(3 * 1024 * 1024)).toBe("3.0 MB")
  })

  test("voiceMismatchNote：精确命中/关键词命中/缺省/真正回落", () => {
    expect(voiceMismatchNote("Microsoft Huihui", "Microsoft Huihui")).toBeNull()
    expect(voiceMismatchNote("microsoft huihui", "Microsoft Huihui")).toBeNull()
    // 包含命中（引擎按关键词选音色）不是回落，不提示
    expect(voiceMismatchNote("Kangkang", "Microsoft Kangkang")).toBeNull()
    expect(voiceMismatchNote("  ", "Microsoft Huihui")).toBeNull()
    expect(voiceMismatchNote(undefined, "Microsoft Huihui")).toBeNull()
    expect(voiceMismatchNote("晓晓", "Microsoft Huihui")).toContain("回落")
  })
})

describe("结果解析", () => {
  test("完整结果与缺省字段", () => {
    const r = parseScriptResult(JSON.stringify({ ok: true, engine: "winrt", voice: "Microsoft Huihui", bytes: 10, durationSec: 1.5 }))
    expect(r).toEqual({ ok: true, error: undefined, engine: "winrt", voice: "Microsoft Huihui", lang: undefined, requested: undefined, bytes: 10, sampleRate: undefined, durationSec: 1.5, pid: undefined, voices: undefined })
    const bad = parseScriptResult(JSON.stringify({ ok: false, error: "boom", bytes: "x", durationSec: null }))
    expect(bad?.ok).toBe(false)
    expect(bad?.error).toBe("boom")
    expect(bad?.bytes).toBeUndefined()
    expect(bad?.durationSec).toBeUndefined()
  })

  test("坏 JSON 与非对象按解析失败", () => {
    expect(parseScriptResult("{bad json")).toBeNull()
    expect(parseScriptResult("null")).toBeNull()
    expect(parseScriptResult('"str"')).toBeNull()
  })

  test("音色清单：无名条目过滤、非数组忽略", () => {
    const r = parseScriptResult(JSON.stringify({ ok: true, engine: "sapi", voices: [{ name: "A", lang: "zh-CN", gender: "Female", engine: "sapi" }, { lang: "en-US" }] }))
    expect(r?.voices).toEqual([{ name: "A", lang: "zh-CN", gender: "Female", engine: "sapi" }])
    expect(parseScriptResult(JSON.stringify({ ok: true, voices: "x" }))?.voices).toBeUndefined()
  })
})

describe("音色过滤与清单", () => {
  test("filterVoices：名称/语言/大小写/无匹配/空查询", () => {
    expect(filterVoices(VOICES, "huihui").length).toBe(1)
    expect(filterVoices(VOICES, "ZH").length).toBe(2)
    expect(filterVoices(VOICES, "english").length).toBe(0)
    expect(filterVoices(VOICES, "en-").length).toBe(1)
    expect(filterVoices(VOICES, "").length).toBe(3)
    expect(filterVoices(VOICES, undefined).length).toBe(3)
  })

  test("formatVoiceList：名称/语言/性别", () => {
    expect(formatVoiceList([VOICES[0]])).toBe("- Microsoft Huihui（zh-CN，Female）")
    expect(formatVoiceList([{ name: "X", lang: "zh", gender: "", engine: "sapi" }])).toBe("- X（zh）")
  })
})

describe("PowerShell 命令编码", () => {
  test("UTF-16LE base64 往返（含中文与非 ASCII 不被破坏）", () => {
    const script = "$s = '你好，歌白'; Write-Output $s"
    const b64 = encodePowerShellCommand(script)
    expect(Buffer.from(b64, "base64").toString("utf16le")).toBe(script)
  })

  test("实际下发的脚本编码可解回原文（脚本内中文不致被 ANSI 误读）", () => {
    const b64 = encodePowerShellCommand(TTS_SCRIPT)
    expect(Buffer.from(b64, "base64").toString("utf16le")).toBe(TTS_SCRIPT)
  })
})

describe("失败文案", () => {
  const run = (code: number, result: ReturnType<typeof parseScriptResult>, stderr = "") => ({ result, stderr, code })

  test("无引擎：指定 winrt 与 auto 的指引不同", () => {
    const noEngine = parseScriptResult(JSON.stringify({ ok: false, error: "no-engine" }))
    expect(scriptFailureNote(run(0, noEngine), "winrt")).toContain("engine=auto")
    expect(scriptFailureNote(run(0, noEngine), "auto")).toContain("未检测到本机离线语音引擎")
  })

  test("无音色与引擎报错原样透出", () => {
    expect(scriptFailureNote(run(0, parseScriptResult(JSON.stringify({ ok: false, error: "no-voice" }))), "auto")).toContain("没有可用音色")
    expect(scriptFailureNote(run(0, parseScriptResult(JSON.stringify({ ok: false, error: "System.Blah" }))), "auto")).toContain("System.Blah")
  })

  test("超时/取消区分（退出码 124）", () => {
    expect(scriptFailureNote(run(124, null, "[timed out after 120000ms]"), "auto")).toContain("超时")
    expect(scriptFailureNote(run(124, null, "[interrupted by user]"), "auto")).toContain("已取消")
  })

  test("非零退出与无结果（CLIXML 噪音清洗）", () => {
    const note = scriptFailureNote(run(1, null, '<Objs Version="1.1"><S S="Error">找不到引擎_x000D_</S></Objs>'), "auto")
    expect(note).toContain("退出码 1")
    expect(note).not.toContain("Objs")
    expect(scriptFailureNote(run(0, null), "auto")).toContain("未产出音频")
  })
})

describe("tts_speak 工具契约", () => {
  test("缺 text：直接报错，不起进程", async () => {
    const { ctx, runs } = makeCtx(tempHome(), scriptStub(okPayload))
    expect((await speakTool.execute({}, ctx)).output).toContain("待合成文本为空")
    expect((await speakTool.execute({ text: "   " }, ctx)).output).toContain("待合成文本为空")
    expect(runs.length).toBe(0)
  })

  test("超长文本：提示拆分，不起进程", async () => {
    const { ctx, runs } = makeCtx(tempHome(), scriptStub(okPayload))
    expect((await speakTool.execute({ text: "啊".repeat(TTS_MAX_TEXT + 1) }, ctx)).output).toContain("文本过长")
    expect(runs.length).toBe(0)
  })

  test("非 Windows 平台：如实说明不做联网合成", async () => {
    setTtsPlatform("linux")
    try {
      const { ctx, runs } = makeCtx(tempHome(), scriptStub(okPayload))
      const r = await speakTool.execute({ text: "你好" }, ctx)
      expect(r.output).toContain("仅 Windows 提供")
      expect(r.output).toContain("不做联网合成")
      expect(runs.length).toBe(0)
    } finally {
      setTtsPlatform("win32")
    }
  })

  test("正常合成：产物 file 块 + data + 可播报说明", async () => {
    const { ctx, runs } = makeCtx(tempHome(), scriptStub({ ...okPayload, requested: "" }))
    const r = await speakTool.execute({ text: "你好，世界" }, ctx)
    expect(r.output).toContain("已合成语音")
    expect(r.output).toContain("Microsoft Huihui")
    expect(r.output).toContain("13 秒")
    expect(r.output).toContain("2 KB")
    expect(r.blocks?.[0]).toMatchObject({ type: "file", mime: "audio/wav" })
    expect(String((r.blocks?.[0] as { path: string }).path)).toStartWith("tmp/tts/voice-")
    expect(String((r.blocks?.[0] as { name: string }).name)).toEndWith(".wav")
    expect(r.data).toMatchObject({ engine: "winrt", voice: "Microsoft Huihui", durationSec: 12.5 })
    // 下发给脚本的环境变量：模式、引擎、已转义文本文件、百分比格式
    expect(runs[0].env.GEBAI_TTS_MODE).toBe("synth")
    expect(runs[0].env.GEBAI_TTS_ENGINE).toBe("auto")
    expect(runs[0].env.GEBAI_TTS_RATE).toBe("+0%")
    expect(runs[0].env.GEBAI_TTS_PITCH).toBe("+0%")
    expect(runs[0].env.GEBAI_TTS_VOLUME).toBe("+0%")
    expect(runs[0].env.GEBAI_TTS_TEXT).toContain("tmp")
    expect(runs[0].cmd).toContain("-EncodedCommand")
  })

  test("参数下传：语速/音调/音量钳制并格式化、out 指定路径、engine 指定", async () => {
    const { ctx, runs } = makeCtx(tempHome(), scriptStub(okPayload))
    const r = await speakTool.execute({ text: "测试", rate: 80, pitch: -20, volume: 999, out: "tmp/out.mp3", engine: "sapi" }, ctx)
    expect(runs[0].env.GEBAI_TTS_RATE).toBe("+80%")
    expect(runs[0].env.GEBAI_TTS_PITCH).toBe("-20%")
    expect(runs[0].env.GEBAI_TTS_VOLUME).toBe("+100%")
    expect(runs[0].env.GEBAI_TTS_ENGINE).toBe("sapi")
    expect(runs[0].env.GEBAI_TTS_OUT).toEndWith("out.mp3")
    expect(r.blocks?.[0]).toMatchObject({ path: "tmp/out.mp3", name: "out.mp3" })
  })

  test("环境变量兜底：TTS_ENGINE / TTS_VOICE 在参数缺省时生效", async () => {
    const { ctx, runs } = makeCtx(tempHome(), scriptStub(okPayload), { TTS_ENGINE: "sapi", TTS_VOICE: "Kangkang" })
    await speakTool.execute({ text: "测试" }, ctx)
    expect(runs[0].env.GEBAI_TTS_ENGINE).toBe("sapi")
    expect(runs[0].env.GEBAI_TTS_VOICE).toBe("Kangkang")
  })

  test("音色回落：结果写明实际音色并给出提示", async () => {
    const { ctx } = makeCtx(tempHome(), scriptStub({ ...okPayload, requested: "晓晓" }))
    const r = await speakTool.execute({ text: "你好", voice: "晓晓" }, ctx)
    expect(r.output).toContain("回落")
    expect(r.output).toContain("tts_voices")
  })

  test("关键词命中音色：不误报为回落", async () => {
    const { ctx } = makeCtx(tempHome(), scriptStub({ ...okPayload, voice: "Microsoft Kangkang", requested: "Kangkang" }))
    const r = await speakTool.execute({ text: "你好", voice: "Kangkang" }, ctx)
    expect(r.output).toContain("Microsoft Kangkang")
    expect(r.output).not.toContain("回落")
  })

  test("out 已存在：提示本次已覆盖", async () => {
    const home = tempHome()
    const { ctx } = makeCtx(home, scriptStub(okPayload))
    await ctx.writeFile(ctx.resolvePath("tmp/exist.wav"), "old")
    const r = await speakTool.execute({ text: "你好", out: "tmp/exist.wav" }, ctx)
    expect(r.output).toContain("已覆盖")
  })

  test("极限语速：提示试听确认可懂度", async () => {
    const { ctx } = makeCtx(tempHome(), scriptStub(okPayload))
    const r = await speakTool.execute({ text: "你好", rate: -80 }, ctx)
    expect(r.output).toContain("自然语速")
  })

  test("脚本失败：原样报错且不产出块", async () => {
    const { ctx } = makeCtx(tempHome(), scriptStub({ ok: false, error: "no-engine" }))
    const r = await speakTool.execute({ text: "你好" }, ctx)
    expect(r.output).toContain("未检测到本机离线语音引擎")
    expect(r.blocks).toBeUndefined()
  })

  test("临时文件清理：只留产物", async () => {
    const home = tempHome()
    const { ctx } = makeCtx(home, scriptStub(okPayload))
    await speakTool.execute({ text: "你好" }, ctx)
    const { readdirSync } = await import("node:fs")
    const files = readdirSync(join(home, "users", "default", "sessions", "s1", "tmp", "tts"))
    expect(files.some((f) => f.startsWith(".text-") || f.startsWith(".result-"))).toBe(false)
    expect(files.some((f) => f.endsWith(".wav"))).toBe(true)
  })
})

describe("播报（play：本机扬声器）", () => {
  test("playbackBlockedReason：本地放行、服务端与沙箱拒绝（并说明产物仍在）", () => {
    expect(playbackBlockedReason({ authMode: "local" })).toBeNull()
    expect(playbackBlockedReason({})).toBeNull()
    expect(playbackBlockedReason({ authMode: "server" })).toContain("仅本地模式")
    expect(playbackBlockedReason({ sandboxed: true })).toContain("仅本地模式")
    expect(playbackBlockedReason({ authMode: "server" })).toContain("产物已落盘")
  })

  test("播报脚本：Start-Process 派生独立播放进程（不阻塞）且不带模板串插值序列", () => {
    expect(TTS_PLAY_SCRIPT).toContain("Start-Process")
    expect(TTS_PLAY_SCRIPT).toContain("PlaySync")
    expect(TTS_PLAY_SCRIPT).toContain("GEBAI_TTS_WAV")
    expect(TTS_PLAY_SCRIPT.includes("${")).toBe(false)
    expect(TTS_PLAY_SCRIPT.includes("`")).toBe(false)
  })

  test("play=true：合成后另起一次脚本调用播报，WAV 路径经环境变量下传", async () => {
    const { ctx, runs } = makeCtx(tempHome(), scriptStub({ ...okPayload, requested: "" }))
    const r = await speakTool.execute({ text: "念给我听", play: true }, ctx)
    expect(runs.length).toBe(2)
    expect(runs[0].env.GEBAI_TTS_MODE).toBe("synth")
    expect(runs[1].env.GEBAI_TTS_MODE).toBe("play")
    expect(runs[1].env.GEBAI_TTS_WAV).toEndWith(".wav")
    expect(r.output).toContain("已在运行 GEBAI 的这台机器的扬声器上开始播报")
    expect((r.data as { played: boolean }).played).toBe(true)
    // 播报不阻碍产物交付
    expect(r.blocks?.[0]).toMatchObject({ type: "file", mime: "audio/wav" })
  })

  test("play 缺省（false）：不发起播报调用", async () => {
    const { ctx, runs } = makeCtx(tempHome(), scriptStub({ ...okPayload, requested: "" }))
    const r = await speakTool.execute({ text: "导出音频" }, ctx)
    expect(runs.length).toBe(1)
    expect(r.output).not.toContain("扬声器")
    expect((r.data as { played: boolean }).played).toBe(false)
  })

  test("服务端模式：不播报但产物照常交付，并写明原因", async () => {
    const { ctx, runs } = makeCtx(tempHome(), scriptStub({ ...okPayload, requested: "" }))
    ctx.authMode = "server"
    const r = await speakTool.execute({ text: "念给我听", play: true }, ctx)
    expect(runs.length).toBe(1)
    expect(r.output).toContain("仅本地模式")
    expect(r.blocks?.[0]).toMatchObject({ type: "file" })
    expect((r.data as { played: boolean }).played).toBe(false)
  })

  test("播报脚本失败：合成结果照常交付，失败原因入注意项", async () => {
    let call = 0
    const { ctx } = makeCtx(tempHome(), async (env) => {
      call++
      const { mkdir, writeFile } = await import("node:fs/promises")
      if (env.GEBAI_TTS_RESULT) {
        await mkdir(join(env.GEBAI_TTS_RESULT, ".."), { recursive: true })
        const payload = call === 1 ? okPayload : { ok: false, error: "no-audio" }
        await writeFile(env.GEBAI_TTS_RESULT, JSON.stringify(payload), "utf8")
      }
      if (call === 1 && env.GEBAI_TTS_OUT) {
        await mkdir(join(env.GEBAI_TTS_OUT, ".."), { recursive: true })
        await writeFile(env.GEBAI_TTS_OUT, Buffer.alloc(64, 1))
      }
      return { code: 0, stderr: "" }
    })
    const r = await speakTool.execute({ text: "念给我听", play: true }, ctx)
    expect(r.output).toContain("已合成语音")
    expect(r.output).toContain("产物音频不存在")
    expect((r.data as { played: boolean }).played).toBe(false)
  })
})

describe("tts_voices 工具契约", () => {
  test("列出全部音色", async () => {
    const { ctx, runs } = makeCtx(tempHome(), scriptStub({ ok: true, engine: "winrt", voices: VOICES }))
    const r = await voicesTool.execute({}, ctx)
    expect(runs[0].env.GEBAI_TTS_MODE).toBe("voices")
    expect(r.output).toContain("共 3 个")
    expect(r.output).toContain("Microsoft Huihui（zh-CN，Female）")
    expect((r.data as { voices: TtsVoice[] }).voices.length).toBe(3)
  })

  test("关键词过滤与 limit", async () => {
    const { ctx } = makeCtx(tempHome(), scriptStub({ ok: true, engine: "winrt", voices: VOICES }))
    const r = await voicesTool.execute({ find: "zh", limit: 1 }, ctx)
    expect(r.output).toContain("匹配 2 个")
    expect(r.output).toContain("已列出前 1 个")
  })

  test("无匹配：列出全部可用音色", async () => {
    const { ctx } = makeCtx(tempHome(), scriptStub({ ok: true, engine: "winrt", voices: VOICES }))
    const r = await voicesTool.execute({ find: "不存在的音色" }, ctx)
    expect(r.output).toContain("没有匹配")
    expect(r.output).toContain("Microsoft Huihui")
  })

  test("引擎不可用与平台不支持", async () => {
    const { ctx } = makeCtx(tempHome(), scriptStub({ ok: false, error: "no-engine" }))
    expect((await voicesTool.execute({}, ctx)).output).toContain("未检测到本机离线语音引擎")
    setTtsPlatform("darwin")
    try {
      const unsupported = makeCtx(tempHome(), scriptStub({ ok: true, voices: [] }))
      expect((await voicesTool.execute({}, unsupported.ctx)).output).toContain("仅 Windows 提供")
    } finally {
      setTtsPlatform("win32")
    }
  })
})

describe("def 契约", () => {
  test("定义完整：名称/描述/提示词/工具/预加载/环境变量", () => {
    expect(def.name).toBe("tts")
    expect(def.description).toContain("语音合成")
    expect(def.description).toContain("离线")
    expect(Object.keys(def.tools ?? {}).sort()).toEqual([
      "effect",
      "install",
      "mix",
      "models",
      "sfx",
      "speak",
      "voices",
    ])
    expect(def.preload).toBe(false)
    expect(def.systemPrompt).toContain("tts_speak")
    expect(def.envVars?.map((v) => v.name)).toEqual(["TTS_VOICE", "TTS_ENGINE", "TTS_MODEL", "HF_ENDPOINT"])
    // 变量分两类：TTS_ 前缀是本代理自有；HF_ENDPOINT 是**全局通用名**
    // （HuggingFace 官方约定，直接读它才能复用用户已有配置），因此不能一律要求 TTS_ 前缀。
    for (const v of def.envVars ?? []) {
      expect(v.name === "HF_ENDPOINT" || v.name.startsWith("TTS_")).toBe(true)
      expect((v.description ?? "").length).toBeGreaterThan(8)
    }
  })

  test("安全模式姿态：两工具都会写产物，显式不提供（safeMode:false）", () => {
    expect(speakTool.safeMode).toBe(false)
    expect(voicesTool.safeMode).toBe(false)
  })

  test("工具名为短名（命名空间由引擎加前缀）", () => {
    expect(speakTool.name).toBe("speak")
    expect(voicesTool.name).toBe("voices")
    expect(speakTool.parameters.required).toEqual(["text"])
  })
})
