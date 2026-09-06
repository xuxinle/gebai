/**
 * 本地 CV 推理入口（core/cv）：惰性共享单例——ort 模块加载、模型目录解析（环境变量
 * GEBAI_CV_MODELS_DIR → 二进制物化目录 → 源码形态 assets/cv-models）、session 缓存
 * （模型文件路径+大小键控）与全进程推理串行（wasm CPU 推理互斥，防同批扇出并发争抢）。
 * 检测（detect）另走分层后端：GPU sidecar（node + onnxruntime-node，见 sidecar.ts）→
 * wasm 进程内兜底（GEBAI_CV_DETECT_BACKEND 控制；标签/输入尺寸支持 ultralytics ONNX
 * 元数据自适应，见 onnx-meta.ts）。测试注入点：setCvRunnerFactory 整体替身
 * （desktop 工具测试）/ setCvOrtLoader ort 层替身。
 */
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs"
import { basename, join } from "node:path"
import { isBinaryMode, resolveGebaiHome } from "../base/config"
import { cropImage, type RgbaImage } from "./image"
import { ctcDecode, dbPostprocess, detPreprocess, recPreprocess, type OcrLine } from "./ocr"
import { letterbox, yoloPostprocess, type DetectObject } from "./detect"
import { parseOnnxMetadata, ultralyticsMeta } from "./onnx-meta"
import { cvSidecarClient, poisonCvSidecar, type CvSidecar } from "./sidecar"
import { loadOrtModule, resolveCvAssetsDir, type OrtModule, type OrtSession } from "./ort-loader"

/** OCR 模型三件套（模型目录内固定文件名）。 */
const DET_MODEL = "det.onnx"
const REC_MODEL = "rec.onnx"
const DICT_FILE = "dict.txt"

const MODEL_DIR_GUIDE =
  "本地识别模型未配置：请设置 GEBAI_CV_MODELS_DIR 指向包含 det.onnx / rec.onnx / dict.txt" +
  "（PP-OCR 中英文 det/rec ONNX 与字典）的目录；源码形态可运行 scripts/build-cv-embed.ts 下载到" +
  " packages/server/assets/cv-models/；单二进制形态需构建时内嵌（scripts/build-cv-embed.ts）"

const DETECT_MODEL_GUIDE =
  "目标检测未配置：设置 GEBAI_CV_DETECT_MODEL（YOLO ONNX 模型路径），或把模型放入 " +
  "{GEBAI_HOME}/models/detect/（唯一 .onnx 自动生效——资源仓库整体放入 models/ 即零配置可用）。" +
  "模型不随构建内嵌，请自备（ultralytics YOLO 导出的 ONNX 自动读取内嵌 imgsz/names 元数据——" +
  "免标签文件与尺寸配置；其他来源需设 GEBAI_CV_DETECT_LABELS，每行一个类别）"

export interface DetectOutcome {
  objects: DetectObject[]
  /** 实际推理后端：sidecar:dml/cuda/coreml/cpu（node 原生，GPU 优先）或 wasm-cpu（含回落注记）。 */
  backend: string
}

export interface OcrOutcome {
  lines: OcrLine[]
  /** 实际推理后端（同 DetectOutcome.backend 语义）。 */
  backend: string
}

export interface CvRunner {
  /** OCR：返回文本行（box 坐标相对传入图像的像素系）与实际推理后端。 */
  ocr(img: RgbaImage, opts?: { maxSide?: number; env?: Record<string, string> }): Promise<OcrOutcome>
  /** YOLO 检测：modelPath/labels 来自环境变量（GEBAI_CV_DETECT_MODEL；标签/尺寸可从 ONNX 元数据自适应）；
   *  iou 为 NMS 阈值（缺省 0.45，密集 UI 控件可调低）。 */
  detect(img: RgbaImage, opts: { env: Record<string, string>; conf: number; iou?: number }): Promise<DetectOutcome>
}

/* ---------------- 测试注入 ---------------- */

let runnerFactory: (() => CvRunner) | null = null
export function setCvRunnerFactory(factory: (() => CvRunner) | null): void {
  runnerFactory = factory
}

let ortLoader: () => Promise<{ ort: OrtModule; assetsDir: string | null }> = loadOrtModule
export function setCvOrtLoader(loader: (() => Promise<{ ort: OrtModule; assetsDir: string | null }>) | null): void {
  ortLoader = loader ?? loadOrtModule
}

/** 测试覆盖 dev 资产目录回退（false = 视为不存在，保证「未配置→指引」用例确定性）。 */
let devAssetsDirOverride: string | false | undefined
export function setCvDevAssetsDirForTests(dir: string | false | undefined): void {
  devAssetsDirOverride = dir
}

export function getCvRunner(): CvRunner {
  return runnerFactory?.() ?? realRunner
}

/* ---------------- 推理串行（全进程互斥） ---------------- */

let chain: Promise<unknown> = Promise.resolve()
function serialize<T>(fn: () => Promise<T>): Promise<T> {
  const next = chain.then(fn, fn)
  chain = next.catch(() => {})
  return next
}

/* ---------------- 模型目录与 session 缓存 ---------------- */

/** 解析 OCR 模型目录：GEBAI_CV_MODELS_DIR（绝对/相对路径均可）→ 二进制物化目录 → 源码 assets。 */
function resolveModelDir(env: Record<string, string>, assetsDir: string | null): string | null {
  const custom = String(env.GEBAI_CV_MODELS_DIR ?? "").trim()
  if (custom) return custom
  if (assetsDir && [DET_MODEL, REC_MODEL, DICT_FILE].every((f) => existsSync(join(assetsDir, f)))) return assetsDir
  if (!isBinaryMode()) {
    const dev = devAssetsDirOverride === false ? null : (devAssetsDirOverride ?? join(import.meta.dir, "..", "..", "..", "assets", "cv-models"))
    if (dev && [DET_MODEL, REC_MODEL, DICT_FILE].every((f) => existsSync(join(dev, f)))) return dev
  }
  return null
}

interface OcrState {
  det: OrtSession
  rec: OrtSession
  /** CTC 字符表：index 0 = blank 占位，末位 = 空格。 */
  chars: string[]
}

let ortModule: Promise<{ ort: OrtModule; assetsDir: string | null }> | null = null
const ocrStates = new Map<string, Promise<OcrState>>()
const sessions = new Map<string, Promise<OrtSession>>()

function loadOrt(): Promise<{ ort: OrtModule; assetsDir: string | null }> {
  return (ortModule ??= ortLoader())
}

/** 模型文件 session（按路径+大小缓存，文件变更自动重建）。 */
function sessionFor(ort: OrtModule, path: string): Promise<OrtSession> {
  const size = existsSync(path) ? statSync(path).size : -1
  const key = `${path}:${size}`
  const cached = sessions.get(key)
  if (cached) return cached
  if (size < 0) {
    return Promise.reject(new Error(`模型文件不存在: ${path}`))
  }
  const created = ort.InferenceSession.create(new Uint8Array(readFileSync(path)))
  sessions.set(key, created)
  created.catch(() => sessions.delete(key))
  return created
}

function ocrStateFor(env: Record<string, string>): Promise<OcrState> {
  return loadOrt().then(({ ort, assetsDir }) => {
    const dir = resolveModelDir(env, assetsDir)
    if (!dir) throw new Error(MODEL_DIR_GUIDE)
    const detPath = join(dir, DET_MODEL)
    const recPath = join(dir, REC_MODEL)
    const dictPath = join(dir, DICT_FILE)
    const missing = [detPath, recPath, dictPath].filter((p) => !existsSync(p))
    if (missing.length) {
      throw new Error(`本地识别模型目录 ${dir} 缺少文件: ${missing.map((p) => p.split(/[\\/]/).pop()).join("、")}`)
    }
    const key = `${detPath}:${statSync(detPath).size}|${recPath}:${statSync(recPath).size}|${dictPath}:${statSync(dictPath).size}`
    const cached = ocrStates.get(key)
    if (cached) return cached
    const state = (async () => {
      const [det, rec] = await Promise.all([sessionFor(ort, detPath), sessionFor(ort, recPath)])
      const dict = readFileSync(dictPath, "utf8").split(/\r?\n/).filter((l) => l.length > 0)
      return { det, rec, chars: ["\uFFFD", ...dict, " "] } satisfies OcrState
    })()
    ocrStates.set(key, state)
    state.catch(() => ocrStates.delete(key))
    return state
  })
}

/* ---------------- 分层后端（GPU sidecar ↔ wasm，全部 CV 推理共用） ---------------- */

type CvBackendMode = "auto" | "sidecar" | "wasm"

/** 后端选择：细分覆盖（GEBAI_CV_DETECT_BACKEND / GEBAI_CV_OCR_BACKEND）> 全局 GEBAI_CV_BACKEND > auto。 */
function resolveBackendMode(env: Record<string, string>, specificKey: string): CvBackendMode {
  for (const key of [specificKey, "GEBAI_CV_BACKEND"]) {
    const v = String(env[key] ?? "").trim().toLowerCase()
    if (v === "sidecar" || v === "wasm") return v
  }
  return "auto"
}

/** EP 选择（GEBAI_CV_EP；GEBAI_CV_DETECT_EP 为早期别名兼容）。 */
function resolveEp(env: Record<string, string>): string {
  return String(env.GEBAI_CV_EP ?? env.GEBAI_CV_DETECT_EP ?? "").trim() || "auto"
}

/**
 * 分层执行（检测与 OCR 共用）：GPU sidecar（node 原生）可用即用；auto 失败回落 wasm 并毒化
 * （进程生命周期内不再重试，避免每次推理都等一遍超时）；显式 sidecar 不回落、错误如实上抛；
 * 显式 wasm 直接进程内。sidecarFn 返回实际后端名（sidecar:ep）。
 */
async function runTiered<T>(
  env: Record<string, string>,
  specificKey: string,
  sidecarFn: (sidecar: CvSidecar) => Promise<{ value: T; backend: string }>,
  wasmFn: () => Promise<T>,
): Promise<{ value: T; backend: string }> {
  const mode = resolveBackendMode(env, specificKey)
  if (mode !== "wasm") {
    const sidecar = cvSidecarClient()
    if (sidecar) {
      try {
        return await sidecarFn(sidecar)
      } catch (e) {
        const msg = e instanceof Error ? e.message : String(e)
        if (mode === "sidecar") {
          throw new Error(`GPU sidecar 推理失败（${specificKey}/GEBAI_CV_BACKEND=sidecar 不回落；改 auto/wasm 可切换）: ${msg}`)
        }
        poisonCvSidecar(msg)
      }
    } else if (mode === "sidecar") {
      throw new Error(
        "CV sidecar 不可用：onnxruntime-node 未解析到（放入 {GEBAI_HOME}/models/vendor/node_modules、设 GEBAI_CV_ORT_NODE_DIR 或安装依赖）；" +
          "显式 sidecar 不回落，改 auto/wasm 可切换",
      )
    }
  }
  return { value: await wasmFn(), backend: "wasm-cpu" }
}

/* ---------------- OCR 模型路径（后端无关——sidecar 路径不加载 ort-web） ---------------- */

interface OcrModelPaths {
  detPath: string
  recPath: string
  dictPath: string
  detSize: number
  recSize: number
}

const ocrPathsCache = new Map<string, Promise<OcrModelPaths>>()
const ocrCharsCache = new Map<string, string[]>()

function ocrModelPathsFor(env: Record<string, string>): Promise<OcrModelPaths> {
  return resolveCvAssetsDir().then((assetsDir) => {
    const dir = resolveModelDir(env, assetsDir)
    if (!dir) throw new Error(MODEL_DIR_GUIDE)
    const cached = ocrPathsCache.get(dir)
    if (cached) return cached
    const detPath = join(dir, DET_MODEL)
    const recPath = join(dir, REC_MODEL)
    const dictPath = join(dir, DICT_FILE)
    const missing = [detPath, recPath, dictPath].filter((p) => !existsSync(p))
    if (missing.length) {
      throw new Error(`本地识别模型目录 ${dir} 缺少文件: ${missing.map((p) => p.split(/[\\/]/).pop()).join("、")}`)
    }
    const paths = { detPath, recPath, dictPath, detSize: statSync(detPath).size, recSize: statSync(recPath).size }
    ocrPathsCache.set(dir, Promise.resolve(paths))
    return paths
  })
}

/** CTC 字符表（按字典路径缓存；index 0 = blank 占位，末位 = 空格）。 */
function ocrCharsFor(dictPath: string): string[] {
  let chars = ocrCharsCache.get(dictPath)
  if (!chars) {
    const dict = readFileSync(dictPath, "utf8").split(/\r?\n/).filter((l) => l.length > 0)
    chars = ["\uFFFD", ...dict, " "]
    ocrCharsCache.set(dictPath, chars)
  }
  return chars
}

/* ---------------- 检测配置（模型路径 / 标签 / 输入尺寸） ---------------- */

/** 检测模型约定发现目录（drop-in 即用）：{GEBAI_HOME}/models/detect/。 */
let detectDirOverride: string | false | undefined
/** 测试注入：false = 视为不存在（保证「未配置→指引」用例确定性，防本机真模型干扰）。 */
export function setCvDetectDirForTests(dir: string | false | undefined): void {
  detectDirOverride = dir
}

function discoverDetectModel(): { path: string } | { multiple: string[] } | null {
  let dir: string | null
  if (detectDirOverride === false) return null
  if (detectDirOverride !== undefined) dir = detectDirOverride
  else {
    try {
      dir = join(resolveGebaiHome(), "models", "detect")
    } catch {
      return null
    }
  }
  let onnx: string[]
  try {
    onnx = readdirSync(dir)
      .filter((f) => f.toLowerCase().endsWith(".onnx"))
      .sort()
      .map((f) => join(dir!, f))
  } catch {
    return null
  }
  if (onnx.length === 1) return { path: onnx[0] }
  if (onnx.length > 1) return { multiple: onnx }
  return null
}

interface DetectConfig {
  modelPath: string
  /** 模型文件大小（sidecar 会话缓存键——文件变更自动重建会话）。 */
  modelSize: number
  labels: string[]
  /** letterbox 目标边长（环境变量 GEBAI_CV_DETECT_SIZE > ONNX 元数据 imgsz > 640）。 */
  size: number
  labelsFromMeta: boolean
}

const detectConfigs = new Map<string, Promise<DetectConfig>>()

function detectConfigFor(env: Record<string, string>): Promise<DetectConfig> {
  let modelPath = String(env.GEBAI_CV_DETECT_MODEL ?? "").trim()
  if (!modelPath) {
    // 约定目录自动发现（env 显式指定优先）：models/detect/ 唯一 .onnx 即生效，多个列出候选
    const found = discoverDetectModel()
    if (found && "multiple" in found) {
      return Promise.reject(
        new Error(
          `models/detect/ 下有 ${found.multiple.length} 个 ONNX 检测模型，无法自动选择：` +
            `${found.multiple.map((p) => basename(p)).join("、")}。设置 GEBAI_CV_DETECT_MODEL 指定其一，或目录内只保留一个 .onnx`,
        ),
      )
    }
    if (found) modelPath = found.path
  }
  if (!modelPath) return Promise.reject(new Error(DETECT_MODEL_GUIDE))
  if (!existsSync(modelPath)) return Promise.reject(new Error(`目标检测模型文件不存在: ${modelPath}`))
  const labelsPath = String(env.GEBAI_CV_DETECT_LABELS ?? "").trim()
  const envSize = Number(env.GEBAI_CV_DETECT_SIZE)
  const sizeOverride = Number.isFinite(envSize) && envSize >= 320 && envSize <= 4096 ? Math.round(envSize) : 0
  const key = `${modelPath}:${statSync(modelPath).size}:${labelsPath}:${sizeOverride}`
  const cached = detectConfigs.get(key)
  if (cached) return cached
  const cfg = (async (): Promise<DetectConfig> => {
    // 元数据从模型字节直接解析（与推理后端无关，wasm/sidecar 同一口径）
    const meta = ultralyticsMeta(parseOnnxMetadata(new Uint8Array(readFileSync(modelPath))))
    let labels: string[] | null = null
    let labelsFromMeta = false
    if (labelsPath) {
      const list = readFileSync(labelsPath, "utf8").split(/\r?\n/).map((l) => l.trim()).filter(Boolean)
      if (list.length) labels = list
    }
    if (!labels && meta.names) {
      labels = meta.names
      labelsFromMeta = true
    }
    if (!labels) {
      throw new Error(
        `目标检测类别未配置：设置 GEBAI_CV_DETECT_LABELS（标签文件，每行一个类别），` +
          `或改用 ultralytics 导出的 ONNX（内嵌 names 元数据自动读取）: ${modelPath}`,
      )
    }
    return { modelPath, modelSize: statSync(modelPath).size, labels, size: sizeOverride || meta.imgsz || 640, labelsFromMeta }
  })()
  detectConfigs.set(key, cfg)
  cfg.catch(() => detectConfigs.delete(key))
  return cfg
}

/* ---------------- 真实 runner ---------------- */

const realRunner: CvRunner = {
  async ocr(img, opts = {}) {
    const env = opts.env ?? {}
    const maxSide = Math.max(320, Math.min(4096, Math.round(Number(env.GEBAI_CV_MAX_SIDE) || 1280)))
    // det：前处理 → 推理 → DB 后处理（坐标已还原到传入图像像素系）；rec：逐框裁剪 → 推理 → CTC 解码
    const pre = detPreprocess(img, Math.min(maxSide, 960))
    const decode = (b: { x: number; y: number; w: number; h: number }, data: Float32Array, dims: readonly number[], chars: string[]): OcrLine | null => {
      // rec 输出 [1, T, C]（batch/时间步/类别）
      const steps = dims[1] ?? Math.floor(Math.sqrt(data.length))
      const classes = dims[2] ?? Math.floor(data.length / steps)
      const { text, score } = ctcDecode(data, steps, classes, chars)
      return text.trim() ? { text, score, box: b } : null
    }
    return runTiered<OcrLine[]>(env, "GEBAI_CV_OCR_BACKEND",
      async (sc) => {
        const paths = await ocrModelPathsFor(env)
        const chars = ocrCharsFor(paths.dictPath)
        const ep = resolveEp(env)
        const det = await sc.runModel({
          modelKey: `${paths.detPath}:${paths.detSize}`,
          modelPath: paths.detPath,
          ep,
          dims: [1, 3, pre.height, pre.width],
          data: pre.data,
        })
        const boxes = dbPostprocess(det.data, det.dims[3] ?? pre.width, det.dims[2] ?? pre.height, img.width, img.height, pre.scale)
        const lines: OcrLine[] = []
        for (const b of boxes) {
          const rec = recPreprocess(cropImage(img, b))
          const out = await sc.runModel({
            modelKey: `${paths.recPath}:${paths.recSize}`,
            modelPath: paths.recPath,
            ep,
            dims: [1, 3, 48, rec.width],
            data: rec.data,
          })
          const line = decode(b, out.data, out.dims, chars)
          if (line) lines.push(line)
        }
        return { value: lines, backend: `sidecar:${det.ep}` }
      },
      () =>
        serialize(async () => {
          const state = await ocrStateFor(env)
          const { ort } = await loadOrt()
          const detOut = await state.det.run({
            [state.det.inputNames[0]]: new ort.Tensor("float32", pre.data, [1, 3, pre.height, pre.width]),
          })
          const prob = firstOutput(detOut)
          const boxes = dbPostprocess(
            prob.data,
            prob.dims[3] ?? pre.width,
            prob.dims[2] ?? pre.height,
            img.width,
            img.height,
            pre.scale,
          )
          const lines: OcrLine[] = []
          for (const b of boxes) {
            const rec = recPreprocess(cropImage(img, b))
            const recOut = await state.rec.run({
              [state.rec.inputNames[0]]: new ort.Tensor("float32", rec.data, [1, 3, 48, rec.width]),
            })
            const out = firstOutput(recOut)
            const line = decode(b, out.data, out.dims, state.chars)
            if (line) lines.push(line)
          }
          return lines
        }),
    ).then(({ value, backend }) => ({ lines: value, backend }))
  },

  async detect(img, opts) {
    const cfg = await detectConfigFor(opts.env)
    const pre = letterbox(img, cfg.size)
    const ep = resolveEp(opts.env)
    const postprocess = (data: Float32Array, dims: readonly number[]) =>
      yoloPostprocess(data, dims, cfg.labels, {
        srcW: img.width,
        srcH: img.height,
        scale: pre.scale,
        padX: pre.padX,
        padY: pre.padY,
        conf: opts.conf,
        iou: opts.iou,
      })
    return runTiered<DetectObject[]>(opts.env, "GEBAI_CV_DETECT_BACKEND",
      async (sc) => {
        const run = await sc.runModel({
          modelKey: `${cfg.modelPath}:${cfg.modelSize}`,
          modelPath: cfg.modelPath,
          ep,
          dims: [1, 3, pre.size, pre.size],
          data: pre.data,
        })
        return { value: postprocess(run.data, run.dims), backend: `sidecar:${run.ep}` }
      },
      () =>
        serialize(async () => {
          const { ort } = await loadOrt()
          const session = await sessionFor(ort, cfg.modelPath)
          const out = await session.run({
            [session.inputNames[0]]: new ort.Tensor("float32", pre.data, [1, 3, pre.size, pre.size]),
          })
          const res = firstOutput(out)
          return postprocess(res.data, res.dims)
        }),
    ).then(({ value, backend }) => ({ objects: value, backend }))
  },
}

function firstOutput(out: Record<string, { data: Float32Array; dims: readonly number[] }>): {
  data: Float32Array
  dims: readonly number[]
} {
  const first = Object.values(out)[0]
  if (!first) throw new Error("模型无输出")
  return first
}
