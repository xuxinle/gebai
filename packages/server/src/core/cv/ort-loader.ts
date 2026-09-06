/**
 * ort 运行时与 CV 资产解析（core/cv）：onnxruntime-web 不进 bundle 图——运行时动态
 * import dist 入口（playwright 模块同款拼接规避 + file URL 动态加载，bundle 注册表启动安全）。
 * 二进制形态从内嵌产物（core/cv.embedded.generated.json，构建脚本 scripts/build-cv-embed.ts
 * 生成，gzip base64）物化到 {GEBAI_HOME}/vendor/cv/（版本 marker 整目录重建 + 防穿越 +
 * 并发共享，仿 materializePwCore）；源码/部署形态解析 node_modules 的 dist 目录。
 */
import { pathToFileURL } from "node:url"
import { dirname, join, normalize, sep } from "node:path"
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { isBinaryMode, resolveGebaiHome } from "../base/config"

/** ort 入口文件（wasm 内嵌胶水变体，wasm 本体在同目录外部加载）。 */
const ORT_ENTRY = "ort.wasm.bundle.min.mjs"
const ORT_WASM = "ort-wasm-simd-threaded.wasm"

/** 内嵌 ort + CV 模型资产清单（gzip base64）。files 至少含 ort 入口与 wasm；模型三件套可选。 */
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

let cvMaterializing: Promise<string> | null = null

/** 物化内嵌 CV 资产到 {GEBAI_HOME}/vendor/cv/（版本一致跳过，并发共享一次执行）。导出供测试。 */
export async function materializeCvAssets(embedded: EmbeddedCvAssets): Promise<string> {
  const vendorRoot = join(resolveGebaiHome(), "vendor")
  const dir = join(vendorRoot, "cv")
  const marker = join(vendorRoot, "cv.version")
  if (existsSync(marker) && readFileSync(marker, "utf8") === embedded.version) return dir
  if (cvMaterializing) return cvMaterializing
  cvMaterializing = (async () => {
    rmSync(dir, { recursive: true, force: true })
    mkdirSync(vendorRoot, { recursive: true })
    for (const f of embedded.files) {
      const target = join(dir, f.path)
      if (!normalize(target).startsWith(normalize(dir) + sep)) continue // 防穿越（产物自生成，纵深防御）
      mkdirSync(dirname(target), { recursive: true })
      writeFileSync(target, Bun.gunzipSync(Buffer.from(f.data, "base64")))
    }
    writeFileSync(marker, embedded.version)
    return dir
  })()
  try {
    return await cvMaterializing
  } finally {
    cvMaterializing = null
  }
}

/** 内嵌产物（二进制形态；缺失/为空返回 null——构建时未内嵌，运行时给配置指引）。 */
export async function loadEmbeddedCvAssets(): Promise<EmbeddedCvAssets | null> {
  const embedded = await import("../cv.embedded.generated.json")
    .then((m) => m.default as EmbeddedCvAssets)
    .catch(() => null)
  if (!embedded || !embedded.version || !embedded.files?.length) return null
  return embedded
}

/** CV 资产目录解析（不加载 ort 模块——sidecar 推理路径解析模型路径时用，避免强制加载
 *  onnxruntime-web）：二进制形态物化内嵌产物；源码/部署形态解析 node_modules 的 dist；
 *  不可得返回 null（模型目录解析随后回落 环境变量 → 源码 assets）。 */
export async function resolveCvAssetsDir(): Promise<string | null> {
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
    return dir ? join(dir, "dist") : null
  } catch {
    return null
  }
}

/**
 * 加载 ort 模块（惰性、全进程共享）：二进制形态物化内嵌产物后从 vendor 目录动态 import；
 * 源码/部署形态解析 node_modules 的 onnxruntime-web/dist。返回 ort 命名空间与资产目录
 * （二进制形态 = vendor/cv；源码形态 = null，模型走 assets 目录或环境变量指定）。
 */
export async function loadOrtModule(): Promise<{ ort: OrtModule; assetsDir: string | null }> {
  const dir = await resolveCvAssetsDir()
  if (!dir) {
    throw new Error(
      isBinaryMode()
        ? "本地识别运行时缺失：单二进制形态未内嵌 CV 资产（构建时运行 scripts/build-cv-embed.ts），" +
            "或设置 GEBAI_CV_MODELS_DIR 指向含 det.onnx/rec.onnx/dict.txt 的目录"
        : `onnxruntime-web 解析失败（源码/部署形态需安装依赖）。若为裁剪部署，请设置 GEBAI_CV_MODELS_DIR 并安装依赖后重试`,
    )
  }
  const ort = (await import(pathToFileURL(join(dir, ORT_ENTRY)).href)) as OrtModule
  ort.env.wasm.numThreads = 1 // 单线程（无 SharedArrayBuffer/worker 依赖，Bun 进程内稳定）
  return { ort, assetsDir: dir }
}

/** ort wasm 本体文件名（构建脚本内嵌清单引用）。 */
export const ORT_ASSET_FILES = [ORT_ENTRY, ORT_WASM]
