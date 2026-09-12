/**
 * 资源目录约定（core/cv）：资源子仓库 `{GEBAI_HOME}/resources/` 与单二进制形态的释放目录共用同一结构——
 *
 *   resources/models/cv/ocr/            OCR 三件套（GEBAI_CV_MODELS_DIR 缺省位置；二进制形态内嵌释放于此）
 *   resources/models/cv/detect/         检测模型 drop-in 目录（唯一 .onnx 自动生效）
 *   resources/vendor/cv/                二进制形态释放的 CV 运行时（ort wasm 入口与本体、cv-driver.mjs、版本 marker）
 *   resources/vendor/node_modules/      GPU sidecar 原生依赖闭包（npm 布局）
 *
 * 历史目录名 `models/`（旧资源仓库）保留在候选链末端作为解析回退。
 */
import { existsSync } from "node:fs"
import { join } from "node:path"
import { resolveGebaiHome } from "../shared/config"

/** 资源根目录名与其历史名（后者仅作解析回退）。 */
export const RESOURCE_DIRNAME = "resources"
const LEGACY_RESOURCE_DIRNAME = "models"

/** 资源根内的相对路径（构建脚本与运行时共用同一约定）。 */
export const RESOURCE_PATHS = {
  ocrModels: [RESOURCE_DIRNAME, "models", "cv", "ocr"],
  detectModels: [RESOURCE_DIRNAME, "models", "cv", "detect"],
  cvRuntime: [RESOURCE_DIRNAME, "vendor", "cv"],
  ortNodeModules: [RESOURCE_DIRNAME, "vendor"],
} as const

/** {GEBAI_HOME} 不可解析（测试环境等）时返回 null。 */
function gebaiHomeOrNull(): string | null {
  try {
    return resolveGebaiHome()
  } catch {
    return null
  }
}

/** 资源根目录（{GEBAI_HOME}/resources）；{GEBAI_HOME} 不可解析时返回 null。 */
export function resourceRoot(): string | null {
  const home = gebaiHomeOrNull()
  return home ? join(home, RESOURCE_DIRNAME) : null
}

/** 资源根候选：新资源目录 → 历史 `models/` 目录（不存在时为单个候选）。 */
export function resourceRoots(): string[] {
  const home = gebaiHomeOrNull()
  return home ? [join(home, RESOURCE_DIRNAME), join(home, LEGACY_RESOURCE_DIRNAME)] : []
}

/** OCR 三件套候选目录（新布局 `resources/models/cv/ocr` → 旧布局 `models/ocr`）。 */
export function ocrModelDirCandidates(): string[] {
  const [cur, legacy] = resourceRoots()
  return cur ? [join(cur, "models", "cv", "ocr"), join(legacy!, "ocr")] : []
}

/** 检测模型候选目录（新布局 `resources/models/cv/detect` → 旧布局 `models/detect`）。 */
export function detectModelDirCandidates(): string[] {
  const [cur, legacy] = resourceRoots()
  return cur ? [join(cur, "models", "cv", "detect"), join(legacy!, "detect")] : []
}

/** onnxruntime-node 解析前缀候选（资源仓库 `vendor/` → 旧资源仓库 → {GEBAI_HOME}/vendor）。 */
export function ortNodePrefixCandidates(): string[] {
  const home = gebaiHomeOrNull()
  if (!home) return []
  return [
    join(home, RESOURCE_DIRNAME, "vendor"),
    join(home, LEGACY_RESOURCE_DIRNAME, "vendor"),
    join(home, "vendor"),
  ]
}

/** 二进制形态释放 CV 运行时的目录（resources/vendor/cv）。 */
export function cvRuntimeDir(): string | null {
  const home = gebaiHomeOrNull()
  return home ? join(home, ...RESOURCE_PATHS.cvRuntime) : null
}

/** 释放 CV 运行时的版本 marker（resources/vendor/cv.version；目录被清时 marker 一并作废）。 */
export function cvRuntimeVersionMarker(): string | null {
  const dir = cvRuntimeDir()
  return dir ? `${dir}.version` : null
}

/** 候选链中第一个存在的目录（新路径优先）。 */
export function firstExistingDir(dirs: readonly string[]): string | null {
  for (const d of dirs) if (d && existsSync(d)) return d
  return null
}
