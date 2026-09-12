/**
 * ort 运行时与 CV 资产解析（core/cv）：onnxruntime-web 不进 bundle 图——运行时动态
 * import dist 入口（playwright 模块同款拼接规避 + file URL 动态加载，bundle 注册表启动安全）。
 * 二进制形态从内嵌产物（core/cv.embedded.generated.json，构建脚本
 * packages/server/scripts/build-cv-embed.ts 生成，gzip base64）释放到资源目录 `{GEBAI_HOME}/resources/`，
 * 相对结构与资源子仓库一致（模型 → `models/cv/ocr/`，ort 运行时 → `vendor/cv/`，见 ./resources.ts）；
 * 源码/部署形态解析 node_modules 的 dist 目录。
 */
import { pathToFileURL } from "node:url"
import { dirname, join, normalize, sep } from "node:path"
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs"
import { isBinaryMode } from "../shared/config"
import { resourceRoot } from "./resources"

/** ort 入口文件（wasm 内嵌胶水变体，wasm 本体在同目录外部加载）。 */
const ORT_ENTRY = "ort.wasm.bundle.min.mjs"
const ORT_WASM = "ort-wasm-simd-threaded.wasm"

/** 内嵌 CV 资产清单（gzip base64）：path 为相对资源根的路径。files 至少含 ort 入口与 wasm。 */
export interface EmbeddedCvAssets {
  version: string
  files: Array<{ path: string; data: string }>
}

export interface OrtTensorLike {
  data: Float32Array
  dims: readonly number[]
}

export interface OrtSession {
  inputNames: readonly string[]
  run(feeds: Record<string, unknown>): Promise<Record<string, OrtTensorLike>>
}

export interface OrtModule {
  Tensor: new (type: "float32", data: Float32Array, dims: readonly number[]) => unknown
  InferenceSession: { create(bytes: Uint8Array): Promise<OrtSession> }
  env: { wasm: { numThreads: number } }
}

/** CV 运行时目录：ort 入口所在目录 + 二进制形态释放的内嵌模型目录。 */
export interface CvRuntimeDirs {
  /** ort wasm 运行时目录（动态 import 入口所在）。 */
  ortDir: string
  /** 二进制形态释放的内嵌 PP-OCR 模型目录（与资源子仓库同构）；源码/部署形态为 null（模型走资源目录候选链）。 */
  modelsDir: string | null
}

let cvMaterializing: Promise<CvRuntimeDirs> | null = null

/** 清单项是否模型资产（模型文件已存在时保留不覆盖——用户自备/替换的模型优先）。 */
const isModelAsset = (path: string) => path.startsWith("models/")

/**
 * 释放内嵌 CV 资产到 `{GEBAI_HOME}/resources/`（按清单 path 铺开：`models/cv/ocr/*` → 模型目录，
 * `vendor/cv/*` → ort 运行时目录）。版本一致且 ort 入口在位时跳过；并发共享一次执行。导出供测试。
 */
export async function materializeCvAssets(embedded: EmbeddedCvAssets): Promise<CvRuntimeDirs> {
  const root = resourceRoot()
  if (!root) throw new Error("无法释放内嵌 CV 资产：{GEBAI_HOME} 不可解析")
  const dirs: CvRuntimeDirs = { ortDir: join(root, "vendor", "cv"), modelsDir: join(root, "models", "cv", "ocr") }
  const marker = `${dirs.ortDir}.version`
  if (existsSync(marker) && readFileSync(marker, "utf8").trim() === embedded.version && existsSync(join(dirs.ortDir, ORT_ENTRY))) {
    return dirs
  }
  if (cvMaterializing) return cvMaterializing
  cvMaterializing = (async () => {
    for (const f of embedded.files) {
      const target = join(root, f.path)
      if (!normalize(target).startsWith(normalize(root) + sep)) continue // 防穿越（产物自生成，纵深防御）
      if (isModelAsset(f.path) && existsSync(target)) continue
      mkdirSync(dirname(target), { recursive: true })
      writeFileSync(target, Bun.gunzipSync(Buffer.from(f.data, "base64")))
    }
    mkdirSync(dirname(marker), { recursive: true })
    writeFileSync(marker, embedded.version)
    return dirs
  })()
  try {
    return await cvMaterializing
  } finally {
    cvMaterializing = null
  }
}

/** 内嵌产物（二进制形态；缺失/为空返回 null——构建时未内嵌，运行时给配置指引）。 */
export async function loadEmbeddedCvAssets(): Promise<EmbeddedCvAssets | null> {
  const embedded = await import("./cv.embedded.generated.json")
    .then((m) => m.default as EmbeddedCvAssets)
    .catch(() => null)
  if (!embedded || !embedded.version || !embedded.files?.length) return null
  return embedded
}

/** CV 运行时解析（不加载 ort 模块——sidecar 推理路径解析模型路径时用，避免强制加载
 *  onnxruntime-web）：二进制形态释放内嵌产物到资源目录；源码/部署形态解析 node_modules 的 dist；
 *  不可得返回 null（模型目录解析随后回落 GEBAI_CV_MODELS_DIR → 资源目录候选链）。 */
export async function resolveCvRuntime(): Promise<CvRuntimeDirs | null> {
  if (isBinaryMode()) {
    const embedded = await loadEmbeddedCvAssets()
    if (!embedded) return null
    return await materializeCvAssets(embedded)
  }
  try {
    // 拼接规避 bundler 对字面量的静态解析（onnxruntime-web 不打包进产物，运行时按需加载）
    const name = "onnxruntime-" + "web"
    const resolved = Bun.resolveSync(name, import.meta.dir)
    const candidates = [dirname(resolved), dirname(dirname(resolved))]
    const dir = candidates.find((c) => existsSync(join(c, "dist", ORT_ENTRY)))
    return dir ? { ortDir: join(dir, "dist"), modelsDir: null } : null
  } catch {
    return null
  }
}

/**
 * 加载 ort 模块（惰性、全进程共享）：二进制形态释放内嵌产物后从资源目录动态 import；
 * 源码/部署形态解析 node_modules 的 onnxruntime-web/dist。返回 ort 命名空间与运行时目录。
 */
export async function loadOrtModule(): Promise<{ ort: OrtModule } & CvRuntimeDirs> {
  const runtime = await resolveCvRuntime()
  if (!runtime) {
    throw new Error(
      isBinaryMode()
        ? "本地识别运行时缺失：单二进制形态未内嵌 CV 资产（构建时运行 packages/server/scripts/build-cv-embed.ts），" +
            "或设置 GEBAI_CV_MODELS_DIR 指向含 det.onnx/rec.onnx/dict.txt 的目录"
        : `onnxruntime-web 解析失败（源码/部署形态需安装依赖）。若为裁剪部署，请设置 GEBAI_CV_MODELS_DIR 并安装依赖后重试`,
    )
  }
  const ort = (await import(pathToFileURL(join(runtime.ortDir, ORT_ENTRY)).href)) as OrtModule
  ort.env.wasm.numThreads = 1 // 单线程（无 SharedArrayBuffer/worker 依赖，Bun 进程内稳定）
  return { ort, ...runtime }
}

/** ort wasm 本体文件名（构建脚本内嵌清单引用）。 */
export const ORT_ASSET_FILES = [ORT_ENTRY, ORT_WASM]
