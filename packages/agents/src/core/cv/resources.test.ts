/**
 * 资源目录约定（core/cv/resources.ts）与二进制形态内嵌资产释放：
 * 资源子仓库 `{GEBAI_HOME}/resources/` 的候选链（旧 `models/` 布局回退）与释放布局（与子仓库同构）。
 */
import { afterEach, beforeEach, describe, expect, test } from "bun:test"
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, relative } from "node:path"
import { gzipSync } from "node:zlib"
import {
  cvRuntimeDir,
  cvRuntimeVersionMarker,
  detectModelDirCandidates,
  firstExistingDir,
  ocrModelDirCandidates,
  ortNodePrefixCandidates,
  resourceRoot,
  resourceRoots,
} from "./resources"
import { materializeCvAssets } from "./ort-loader"

const savedHome = process.env.GEBAI_HOME
let home = ""

beforeEach(() => {
  home = mkdtempSync(join(tmpdir(), "gebai-resources-"))
  process.env.GEBAI_HOME = home
})

afterEach(() => {
  if (savedHome === undefined) delete process.env.GEBAI_HOME
  else process.env.GEBAI_HOME = savedHome
  rmSync(home, { recursive: true, force: true })
})

// 内嵌清单 data 的生成口径与构建脚本一致（node:zlib gzip + base64）
const gz = (s: string) => gzipSync(Buffer.from(s)).toString("base64")

describe("资源目录候选链（core/cv/resources）", () => {
  test("资源根与各候选顺序：新资源目录优先、旧 models 布局回退", () => {
    const resources = join(home, "resources")
    const legacy = join(home, "models")
    expect(resourceRoot()).toBe(resources)
    expect(resourceRoots()).toEqual([resources, legacy])
    expect(ocrModelDirCandidates()).toEqual([join(resources, "models", "cv", "ocr"), join(legacy, "ocr")])
    expect(detectModelDirCandidates()).toEqual([
      join(resources, "models", "cv", "detect"),
      join(legacy, "detect"),
    ])
    expect(ortNodePrefixCandidates()).toEqual([join(resources, "vendor"), join(legacy, "vendor"), join(home, "vendor")])
    expect(cvRuntimeDir()).toBe(join(resources, "vendor", "cv"))
    expect(cvRuntimeVersionMarker()).toBe(join(resources, "vendor", "cv.version"))
  })

  test("firstExistingDir 取候选链中第一个存在的目录", () => {
    const dirs = [join(home, "a"), join(home, "b")]
    expect(firstExistingDir(dirs)).toBeNull()
    mkdirSync(dirs[1]!, { recursive: true })
    expect(firstExistingDir(dirs)).toBe(dirs[1]!)
    mkdirSync(dirs[0]!, { recursive: true })
    expect(firstExistingDir(dirs)).toBe(dirs[0]!)
  })
})

describe("内嵌 CV 资产释放（二进制形态，与资源子仓库同构）", () => {
  test("模型释放到 models/cv/ocr、ort 运行时释放到 vendor/cv；已存在的模型保留不覆盖", async () => {
    const ocrDir = join(home, "resources", "models", "cv", "ocr")
    mkdirSync(ocrDir, { recursive: true })
    writeFileSync(join(ocrDir, "det.onnx"), "user-det")
    const dirs = await materializeCvAssets({
      version: "v-test-1",
      files: [
        { path: "vendor/cv/ort.wasm.bundle.min.mjs", data: gz("ort-entry") },
        { path: "vendor/cv/ort-wasm-simd-threaded.wasm", data: gz("ort-wasm") },
        { path: "models/cv/ocr/det.onnx", data: gz("embedded-det") },
        { path: "models/cv/ocr/rec.onnx", data: gz("embedded-rec") },
        { path: "models/cv/ocr/dict.txt", data: gz("embedded-dict") },
      ],
    })
    expect(dirs.ortDir).toBe(join(home, "resources", "vendor", "cv"))
    expect(dirs.modelsDir).toBe(ocrDir)
    expect(readFileSync(join(dirs.ortDir, "ort.wasm.bundle.min.mjs"), "utf8")).toBe("ort-entry")
    expect(readFileSync(join(dirs.ortDir, "ort-wasm-simd-threaded.wasm"), "utf8")).toBe("ort-wasm")
    expect(readFileSync(join(ocrDir, "det.onnx"), "utf8")).toBe("user-det")
    expect(readFileSync(join(ocrDir, "rec.onnx"), "utf8")).toBe("embedded-rec")
    expect(readFileSync(join(ocrDir, "dict.txt"), "utf8")).toBe("embedded-dict")
    expect(readFileSync(join(home, "resources", "vendor", "cv.version"), "utf8")).toBe("v-test-1")
  })

  test("清单外路径不落盘（防穿越）；ort 入口缺失时按 marker 版本重建", async () => {
    const embedded = {
      version: "v-test-2",
      files: [
        { path: "../escape.txt", data: gz("escape") },
        { path: "vendor/cv/ort.wasm.bundle.min.mjs", data: gz("ort") },
      ],
    }
    const entry = join(home, "resources", "vendor", "cv", "ort.wasm.bundle.min.mjs")
    await materializeCvAssets(embedded)
    expect(existsSync(join(home, "escape.txt"))).toBe(false)
    expect(existsSync(entry)).toBe(true)
    rmSync(entry)
    await materializeCvAssets(embedded)
    expect(existsSync(entry)).toBe(true)
  })
})

/** 主仓库下载清单（scripts/resources.manifest.json，`bun run resources:download` 消费）。 */
const manifestPath = join(import.meta.dirname, "..", "..", "..", "..", "..", "scripts", "resources.manifest.json")

describe("下载清单与资源目录约定一致", () => {
  const manifest = JSON.parse(readFileSync(manifestPath, "utf8")) as {
    entries: Array<{ path: string; required?: boolean; sources?: Array<{ kind: string; url: string }>; derive?: { method?: string } }>
  }

  test("条目路径为资源根下的相对路径（无前导斜杠/上跳）", () => {
    for (const e of manifest.entries) {
      expect(e.path.includes("\\"), `${e.path} 应用正斜杠`).toBe(false)
      expect(e.path.startsWith("/"), `${e.path} 不应用绝对路径`).toBe(false)
      expect(e.path.split("/").includes(".."), `${e.path} 不应上跳`).toBe(false)
    }
  })

  test("必选条目有下载来源或派生途径；来源为 http(s) 且带 kind", () => {
    for (const e of manifest.entries) {
      if (e.required !== false) expect((e.sources?.length ?? 0) > 0 || !!e.derive, `${e.path} 缺来源`).toBe(true)
      for (const s of e.sources ?? []) {
        expect(s.kind, `${e.path} 来源缺 kind`).toBeTruthy()
        expect(s.url.startsWith("http"), `${e.path} 来源 URL 非法: ${s.url}`).toBe(true)
      }
    }
  })

  test("OCR 三件套与检测目录路径与 resources.ts 约定同源（改名不会漏改清单）", () => {
    const root = resourceRoot()!
    const ocrRel = relative(root, ocrModelDirCandidates()[0]!).replaceAll("\\", "/")
    const detectRel = relative(root, detectModelDirCandidates()[0]!).replaceAll("\\", "/")
    const paths = manifest.entries.map((e) => e.path)
    for (const f of ["det.onnx", "rec.onnx", "dict.txt"]) expect(paths).toContain(`${ocrRel}/${f}`)
    expect(paths.some((p) => p.startsWith(`${detectRel}/`))).toBe(true)
  })
})
