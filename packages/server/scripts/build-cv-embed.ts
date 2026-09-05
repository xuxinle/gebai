/**
 * 构建时生成本地 CV（onnxruntime-web 运行时 + PP-OCR 模型）内嵌产物（`src/core/cv.embedded.generated.json`）。
 *
 * 背景：bun --compile 单二进制形态在用户机器上不可依赖 node_modules，本地 CV 推理的 ort
 * 运行时（dist 入口 mjs + wasm 本体，见 core/cv/ort-loader.ts）与 PP-OCR 模型三件套
 * （det/rec ONNX + 字典）须随产物内嵌，运行时物化到 `{GEBAI_HOME}/vendor/cv/`。
 *
 * 模型来源：`assets/cv-models/` 已有文件优先（离线/内网自备三件套）；缺失时从
 * GEBAI_CV_MODEL_BASE 下载（缺省 hf-mirror 的 RapidOCR 托管 PP-OCRv4 mobile，内网可覆写
 * 镜像；文件名固定 det.onnx/rec.onnx 对应两个 URL）。字典从 rec 模型内嵌的 character
 * 元数据提取（RapidOCR 约定，免去单独的字典下载源）。下载失败时生成空清单——构建不失败，
 * 运行时 desktop_ocr/desktop_locate 给出 GEBAI_CV_MODELS_DIR 配置指引。
 *
 * 该文件为生成产物，已 gitignore，勿手改。
 */
import { gzipSync } from "node:zlib"
import { existsSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs"
import { dirname, join } from "node:path"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/server
const outFile = join(root, "src", "core", "cv.embedded.generated.json")
const assetsDir = join(root, "assets", "cv-models")

const MODEL_BASE = process.env.GEBAI_CV_MODEL_BASE || "https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv4"
const MODEL_SOURCES = [
  { name: "det.onnx", url: `${MODEL_BASE}/ch_PP-OCRv4_det_infer.onnx` },
  { name: "rec.onnx", url: `${MODEL_BASE}/ch_PP-OCRv4_rec_infer.onnx` },
]

/** ort dist 两文件（与 core/cv/ort-loader.ts 的 ORT_ENTRY/ORT_WASM 一致）。 */
const ORT_FILES = ["ort.wasm.bundle.min.mjs", "ort-wasm-simd-threaded.wasm"]

const files: Array<{ path: string; data: string }> = []
function addFile(path: string, bytes: Uint8Array): void {
  files.push({ path, data: gzipSync(Buffer.from(bytes)).toString("base64") })
}

async function downloadModels(): Promise<boolean> {
  mkdirSync(assetsDir, { recursive: true })
  for (const { name, url } of MODEL_SOURCES) {
    const target = join(assetsDir, name)
    if (existsSync(target) && statBytes(target) > 1_000_000) continue
    try {
      console.log(`[build-cv-embed] downloading ${name} <- ${url}`)
      const res = await fetch(url, { signal: AbortSignal.timeout(600_000) })
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      const bytes = new Uint8Array(await res.arrayBuffer())
      if (bytes.length < 1_000_000) throw new Error(`文件过小（${bytes.length}B，疑似错误页）`)
      writeFileSync(target, bytes)
    } catch (e) {
      console.warn(`[build-cv-embed] 下载失败（跳过内嵌，运行时走配置指引）: ${name}: ${e instanceof Error ? e.message : e}`)
      rmSync(target, { force: true })
      return false
    }
  }
  return true
}

function statBytes(path: string): number {
  try {
    return statSync(path).size
  } catch {
    return -1
  }
}

/** 从 rec 模型内嵌的 character 元数据提取字典（ModelProto.metadata_props: key="character"，value=逐行字符）。 */
function extractDictFromRec(recBytes: Uint8Array): string | null {
  const key = Buffer.from("character", "utf8")
  const idx = Buffer.from(recBytes).indexOf(key)
  if (idx < 0) return null
  // StringStringEntryProto 布局：field1(key)=0x0A len "character" field2(value)=0x12 varint-len bytes
  let p = idx + key.length
  if (recBytes[p] !== 0x12) return null
  p++
  let len = 0
  let shift = 0
  for (;;) {
    const b = recBytes[p++]
    len |= (b & 0x7f) << shift
    if ((b & 0x80) === 0) break
    shift += 7
    if (shift > 28) return null
  }
  if (len <= 0 || p + len > recBytes.length) return null
  return Buffer.from(recBytes.subarray(p, p + len)).toString("utf8")
}

async function main(): Promise<void> {
  // 模型：本地已有优先，缺失则下载
  const modelsReady =
    (existsSync(join(assetsDir, "det.onnx")) && existsSync(join(assetsDir, "rec.onnx"))) || (await downloadModels())
  // ort 运行时：解析 node_modules 的 onnxruntime-web dist
  let ortDir: string | null = null
  try {
    const resolved = Bun.resolveSync("onnxruntime-" + "web", root)
    const candidates = [dirname(resolved), dirname(dirname(resolved))]
    const pkgDir = candidates.find((c) => existsSync(join(c, "dist", ORT_FILES[0])))
    ortDir = pkgDir ? join(pkgDir, "dist") : null
  } catch {
    ortDir = null
  }
  if (!ortDir) console.warn("[build-cv-embed] onnxruntime-web 解析失败（跳过内嵌；源码形态运行时按需从 node_modules 加载）")

  if (!modelsReady || !ortDir) {
    writeFileSync(outFile, JSON.stringify({ version: "", files: [] }))
    console.log(`[build-cv-embed] 空清单（models=${modelsReady} ort=${!!ortDir}）-> ${outFile}`)
    return
  }

  for (const f of ORT_FILES) addFile(f, new Uint8Array(readFileSync(join(ortDir, f))))
  addFile("det.onnx", new Uint8Array(readFileSync(join(assetsDir, "det.onnx"))))
  addFile("rec.onnx", new Uint8Array(readFileSync(join(assetsDir, "rec.onnx"))))

  // 字典：rec 元数据提取（缺省已随模型内嵌）；已有 dict.txt 则沿用（允许手工替换）
  const dictPath = join(assetsDir, "dict.txt")
  let dict = existsSync(dictPath) ? readFileSync(dictPath, "utf8") : null
  if (!dict) {
    dict = extractDictFromRec(new Uint8Array(readFileSync(join(assetsDir, "rec.onnx"))))
    if (!dict) throw new Error("[build-cv-embed] rec 模型无内嵌 character 字典，且 assets/cv-models/dict.txt 缺失——请自备字典文件")
    writeFileSync(dictPath, dict)
  }
  addFile("dict.txt", Buffer.from(dict, "utf8"))

  const version = String(Bun.hash(files.map((f) => `${f.path}:${f.data}`).join("\n")))
  writeFileSync(outFile, JSON.stringify({ version, files }))
  console.log(
    `[build-cv-embed] embedded ort + PP-OCR models (${files.length} files, base64 ${Math.round(files.reduce((n, f) => n + f.data.length, 0) / 1024)} KB) -> ${outFile}`,
  )
}

await main()
