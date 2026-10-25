/**
 * 神经语音合成（neural TTS）用例：**纯函数与降级语义**，不下载模型、不起 Python 进程。
 *
 * 这里钉住的是「扫描/挑选/失败说明」这三件最容易在重构中悄悄退化的事：
 * 它们坏了不会报错，只会表现为「模型明明装了却说没有」或「报错原文被换成一句无关的安装提示」
 * （后者真的发生过：把任何失败都映射成「缺 sherpa-onnx」，把真因埋掉了）。
 */
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { afterAll, describe, expect, test } from "bun:test"
import {
  NEURAL_REQUIRED_FILES,
  matchNeuralModel,
  neuralFailureNote,
  neuralTtsScript,
  pickNeuralModel,
  pythonCandidates,
  scanNeuralModels,
  writeTempScript,
} from "../../core/tts/neural"

const roots: string[] = []
/** 造一个「GEBAI_HOME 根」；模型实际落在其下的 resources/models/tts（与生产布局一致）。 */
function makeRoot(): string {
  const home = mkdtempSync(join(tmpdir(), "gebai-neural-"))
  roots.push(home)
  const modelRoot = join(home, "resources", "models", "tts")
  mkdirSync(modelRoot, { recursive: true })
  return home
}
afterAll(() => {
  for (const dir of roots) rmSync(dir, { recursive: true, force: true })
})

/**
 * 造一个「文件齐全」的模型目录（相对 home 的 resources/models/tts）。
 *
 * 齐全的语义：一个 onnx + `tokens.txt` + （`lexicon.txt` 或 `espeak-ng-data/` 二选一）。
 * `phonemes` 可切换音素资源形态，用来盖住两条音素化路径。
 */
function makeModel(
  home: string,
  id: string,
  opts: { onnx?: string; skip?: string; phonemes?: "lexicon" | "espeak" } = {},
): string {
  const dir = join(home, "resources", "models", "tts", id)
  mkdirSync(dir, { recursive: true })
  writeFileSync(join(dir, opts.onnx ?? "model.onnx"), "fake-onnx")
  for (const f of NEURAL_REQUIRED_FILES) {
    if (f === opts.skip) continue
    writeFileSync(join(dir, f), "x")
  }
  if (opts.phonemes === "espeak") {
    mkdirSync(join(dir, "espeak-ng-data"), { recursive: true })
  } else if (opts.skip !== "lexicon.txt") {
    writeFileSync(join(dir, "lexicon.txt"), "x")
  }
  return dir
}

describe("模型扫描：按文件齐全判定，而不是硬编码清单", () => {
  test("齐全的目录被发现；缺 onnx / 缺 tokens / 缺音素资源的一律忽略", () => {
    const root = makeRoot()
    makeModel(root, "full")
    makeModel(root, "no-tokens", { skip: "tokens.txt" })
    makeModel(root, "no-lexicon", { skip: "lexicon.txt" })
    // 另一条音素化路径：只有 espeak-ng-data、没有 lexicon.txt 也算齐全
    makeModel(root, "espeak-only", { phonemes: "espeak" })
    mkdirSync(join(root, "resources", "models", "tts", "empty"), { recursive: true })
    const found = scanNeuralModelsFrom(root)
    expect(found.map((m) => m.id).sort()).toEqual(["espeak-only", "full"])
    expect(found.find((m) => m.id === "full")!.onnx.endsWith("model.onnx")).toBe(true)
    expect(found.every((m) => m.bytes > 0)).toBe(true)
  })

  test("一个目录里有两个 onnx：取非 int8 的最大那个作主模型（不再一律排除）", () => {
    const root = makeRoot()
    const dir = makeModel(root, "two", { onnx: "model.onnx" })
    writeFileSync(join(dir, "model.int8.onnx"), "int8")
    const found = scanNeuralModelsFrom(root)
    expect(found.map((m) => m.id)).toEqual(["two"])
    expect(found[0]!.onnx.endsWith("model.onnx")).toBe(true)
  })
})

describe("模型挑选：默认与关键词匹配", () => {
  const models = [
    { id: "vits-zh-theresa", onnx: "/m/theresa.onnx", bytes: 1 },
    { id: "vits-zh-fanchen-C", onnx: "/m/fanchen.onnx", bytes: 1 },
  ]

  test("默认优先 theresa（22kHz 高保真），其次 fanchen", () => {
    expect(pickNeuralModel(models)?.id).toBe("vits-zh-theresa")
    expect(pickNeuralModel([models[1]!])?.id).toBe("vits-zh-fanchen-C")
    expect(pickNeuralModel([])).toBeNull()
  })
  test("关键词匹配：全名 / 子串 / 未命中 / 空查询回落默认", () => {
    expect(matchNeuralModel(models, "vits-zh-theresa")?.id).toBe("vits-zh-theresa")
    expect(matchNeuralModel(models, "FANCHEN")?.id).toBe("vits-zh-fanchen-C")
    expect(matchNeuralModel(models, "theresa")?.id).toBe("vits-zh-theresa")
    expect(matchNeuralModel(models, "nope")).toBeNull()
    expect(matchNeuralModel(models, "")?.id).toBe("vits-zh-theresa")
  })
})

describe("合成脚本：参数正确嵌入，文本走 base64", () => {
  test("脚本含模型目录、说话人、语速、输出路径；中文文本不以明文出现（避免引号/换行拆串）", () => {
    const text = "把旧世界的技术，收编成躯体。"
    const script = neuralTtsScript({
      modelDir: "/models/tts/vits-zh-theresa",
      textBase64: Buffer.from(text, "utf8").toString("base64"),
      speaker: 7,
      speed: 1.2,
      outPath: "/out/a.wav",
    })
    expect(script).toContain("vits-zh-theresa")
    expect(script).toContain("sid=7")
    expect(script).toContain("speed=1.2")
    expect(script).toContain("/out/a.wav")
    expect(script).not.toContain(text)
    expect(script).toContain("sherpa_onnx")
    // 报告行必须是最后一个 print，调用方靠它取结果
    expect(script.trimEnd().endsWith("print(json.dumps(report))")).toBe(true)
  })
})

describe("解释器候选链", () => {
  test("优先客卿共享 venv（复用 onnxruntime），再回落系统 python", () => {
    const list = pythonCandidates("/repo")
    expect(list[0]).toContain("keqing")
    expect(list.some((p) => p.includes("python.exe") || p.includes("/bin/python"))).toBe(true)
    expect(list).toContain("python")
    // 不重复
    expect(new Set(list).size).toBe(list.length)
  })
})

describe("失败说明：区分真因，不把所有错误都说成缺依赖", () => {
  test("只有真正缺依赖才提示安装", () => {
    expect(neuralFailureNote("missing-dependency: No module named 'sherpa_onnx'")).toContain("tts_install")
  })

  test("解释器/脚本没跑起来 → 说启动失败，而不是让人去装包", () => {
    const note = neuralFailureNote("no-report")
    expect(note).toContain("启动")
    expect(note).not.toContain("不会加载模型")
  })

  test("推理阶段出错 → 指向模型/版本，并保留原文", () => {
    const note = neuralFailureNote("runtime-error: InvalidProtobuf")
    expect(note).toContain("推理")
    expect(note).toContain("InvalidProtobuf")
  })

  test("模型不完整 → 指出缺哪些文件", () => {
    expect(neuralFailureNote("no-model-file")).toContain("tokens.txt")
    expect(neuralFailureNote("invalid-config")).toContain("lexicon.txt")
  })

  // ## 为何要钉对象形态（实测代价：一整轮查不下去）
  //
  // `runNeural` 的 no-report 分支把真因放在 `detail: { stdout, stderr }` 里，
  // 而旧代码只 `String(raw)` → `[object Object]`，**连 `no-report` 都匹配不上**，
  // 落到兜底文案「神经语音合成失败。原文：[object Object]」。
  // 后果：本机真的报错时，stderr 里的 `ImportError: DLL load failed` 一行都看不到。
  test("detail 是对象时，必须展开 stderr/stdout，而不是变成 [object Object]", () => {
    const note = neuralFailureNote({
      error: "no-report",
      note: "python 未返回报告",
      detail: {
        stdout: "",
        stderr: "Traceback (most recent call last):\nImportError: DLL load failed while importing sherpa_onnx",
      },
    })
    expect(note).not.toContain("[object Object]")
    // 分支必须命中「启动」那一档（有 error 码）
    expect(note).toContain("启动")
    // 真因必须原样保留
    expect(note).toContain("ImportError: DLL load failed")
  })

  test("字符串形态仍然可用（不破坏既有调用）", () => {
    expect(neuralFailureNote("runtime-error: InvalidProtobuf")).toContain("InvalidProtobuf")
    expect(neuralFailureNote("")).toContain("失败")
  })
})

describe("临时脚本：必须落成真文件（不用 -c，避免换行被 shell 拆开）", () => {
  test("写入后文件内容与脚本一致，cleanup 删干净", async () => {
    const dir = makeRoot()
    const body = 'import sys\nprint("hi")\n'
    const file = writeTempScript(dir, body, "synth")
    const { readFileSync } = await import("node:fs")
    expect(readFileSync(file.path, "utf8")).toBe(body)
    expect(file.path.endsWith(".py")).toBe(true)
    file.cleanup()
    expect(scanNeuralModelsFrom(dir)).toEqual([])
    const { existsSync } = await import("node:fs")
    expect(existsSync(file.path)).toBe(false)
  })
})

/**
 * 用指定根目录跑一次扫描。
 *
 * `scanNeuralModels()` 的根来自 `{GEBAI_HOME}`（进程级），用例不能改宿主环境；
 * 这里用 `GEBAI_HOME` 临时指向本用例的临时根——顺带把「根目录解析」这条链也钉住。
 */
function scanNeuralModelsFrom(root: string) {
  const prev = process.env.GEBAI_HOME
  process.env.GEBAI_HOME = root
  try {
    return scanNeuralModels()
  } finally {
    if (prev === undefined) delete process.env.GEBAI_HOME
    else process.env.GEBAI_HOME = prev
  }
}
