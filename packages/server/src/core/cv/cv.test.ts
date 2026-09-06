import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { getCvRunner, setCvDetectDirForTests, setCvDevAssetsDirForTests, setCvOrtLoader, setCvRunnerFactory } from "./cv"
import { resetCvSidecarForTests, setCvSidecarFactoryForTests, type CvSidecar } from "./sidecar"
import type { OrtModule, OrtSession, OrtTensorLike } from "./ort-loader"
import type { RgbaImage } from "./image"

/* ---------- 假 ort 层：按模型文件首字节分发（1=det / 2=rec / 其他=detect） ---------- */

function solid(w: number, h: number): RgbaImage {
  const data = new Uint8ClampedArray(w * h * 4)
  for (let i = 0; i < w * h; i++) data.set([200, 200, 200, 255], i * 4)
  return { width: w, height: h, data }
}

class FakeTensor {
  constructor(
    public type: string,
    public data: Float32Array,
    public dims: readonly number[],
  ) {}
}

/** det 输出工厂（用例可临时替换后恢复）。缺省：全图 0.9 → 一个整图文本框。 */
let detProbFactory: (h: number, w: number) => OrtTensorLike = (h, w) => ({
  data: new Float32Array(h * w).fill(0.9),
  dims: [1, 1, h, w],
})
/** detect 输出（设置后按 v8 形态返回）。 */
let detectOut: OrtTensorLike | null = null

const ort: OrtModule = {
  Tensor: FakeTensor as unknown as OrtModule["Tensor"],
  InferenceSession: {
    create: async (bytes: Uint8Array): Promise<OrtSession> => {
      if (bytes[0] === 1) {
        return {
          inputNames: ["x"],
          run: async (feeds) => {
            const t = feeds["x"] as FakeTensor
            return { out: detProbFactory(t.dims[2], t.dims[3]) }
          },
        }
      }
      if (bytes[0] === 2) {
        return {
          inputNames: ["x"],
          run: async () => ({ out: { data: new Float32Array([0.1, 0.9, 0, 0, 0.1, 0.9, 0, 0]), dims: [1, 2, 4] } }),
        }
      }
      if (detectOut) {
        return { inputNames: ["images"], run: async () => ({ out: detectOut! }) }
      }
      throw new Error("假 ort：未设置 detect 输出")
    },
  },
  env: { wasm: { numThreads: 0 } },
}

function modelDir(): string {
  const dir = mkdtempSync(join(tmpdir(), "gebai-cv-"))
  writeFileSync(join(dir, "det.onnx"), Buffer.from([1]))
  writeFileSync(join(dir, "rec.onnx"), Buffer.from([2]))
  writeFileSync(join(dir, "dict.txt"), "存\n保\n")
  return dir
}

beforeAll(() => {
  setCvOrtLoader(() => Promise.resolve({ ort, assetsDir: null }))
  // 屏蔽 dev 资产目录回退（本机可能已下载真模型），保证「未配置→指引」用例确定性
  setCvDevAssetsDirForTests(false)
  // 屏蔽检测模型约定目录发现（本机 models/detect 可能有真模型），自动发现用例内按需指向临时目录
  setCvDetectDirForTests(false)
  // sidecar 恒不可用（分层后端用例内按需注入替身）——保证缺省走 wasm 路径的确定性
  setCvSidecarFactoryForTests(() => null)
})

afterAll(() => {
  setCvOrtLoader(null)
  setCvDevAssetsDirForTests(undefined)
  setCvDetectDirForTests(undefined)
  setCvRunnerFactory(null)
  resetCvSidecarForTests()
})

describe("cv runner 注入", () => {
  test("setCvRunnerFactory 替身优先", () => {
    const fake = { ocr: async () => ({ lines: [], backend: "wasm-cpu" }), detect: async () => ({ objects: [], backend: "wasm-cpu" }) }
    setCvRunnerFactory(() => fake)
    expect(getCvRunner()).toBe(fake)
    setCvRunnerFactory(null)
    expect(getCvRunner()).not.toBe(fake)
  })
})

describe("real runner（假 ort 层）", () => {
  test("模型目录未配置 → 中文指引", async () => {
    await expect(getCvRunner().ocr(solid(64, 32), { env: {} })).rejects.toThrow(/GEBAI_CV_MODELS_DIR/)
  })

  test("模型目录缺文件 → 列出缺失文件名", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-empty-"))
    await expect(getCvRunner().ocr(solid(64, 32), { env: { GEBAI_CV_MODELS_DIR: dir } })).rejects.toThrow(/det\.onnx/)
  })

  test("OCR 全链路：det 整图框 → rec 解码出字典字符，box 覆盖图像", async () => {
    const r = await getCvRunner().ocr(solid(64, 32), { env: { GEBAI_CV_MODELS_DIR: modelDir() } })
    expect(r.backend).toBe("wasm-cpu")
    expect(r.lines.length).toBe(1)
    expect(r.lines[0].text).toBe("存")
    expect(r.lines[0].score).toBeGreaterThan(0.8)
    expect(r.lines[0].box.w).toBeGreaterThan(50)
    expect(r.lines[0].box.h).toBeGreaterThan(20)
  })

  test("det 无命中（全低概率）→ 空行列表", async () => {
    const original = detProbFactory
    detProbFactory = (h, w) => ({ data: new Float32Array(h * w).fill(0.1), dims: [1, 1, h, w] })
    try {
      const r = await getCvRunner().ocr(solid(64, 32), { env: { GEBAI_CV_MODELS_DIR: modelDir() } })
      expect(r.lines.length).toBe(0)
    } finally {
      detProbFactory = original
    }
  })

  test("并发两次 OCR 不死锁（串行互斥链）", async () => {
    const env = { GEBAI_CV_MODELS_DIR: modelDir() }
    const [a, b] = await Promise.all([getCvRunner().ocr(solid(64, 32), { env }), getCvRunner().ocr(solid(64, 32), { env })])
    expect(a.lines.length).toBe(1)
    expect(b.lines.length).toBe(1)
  })

  test("detect 未配置 → 指引错误", async () => {
    await expect(getCvRunner().detect(solid(64, 64), { env: {}, conf: 0.25 })).rejects.toThrow(/GEBAI_CV_DETECT_MODEL/)
  })

  test("detect 类别未配置（无标签文件且模型无元数据）→ 指引", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-nolabel-"))
    writeFileSync(join(dir, "model.onnx"), Buffer.from([9]))
    await expect(
      getCvRunner().detect(solid(64, 64), { env: { GEBAI_CV_DETECT_MODEL: join(dir, "model.onnx") }, conf: 0.25 }),
    ).rejects.toThrow(/GEBAI_CV_DETECT_LABELS/)
  })

  test("detect 全链路：v8 形态输出 → 标签与坐标还原", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-det-"))
    writeFileSync(join(dir, "model.onnx"), Buffer.from([9]))
    writeFileSync(join(dir, "labels.txt"), "btn\nicon\n")
    const n = 8
    const out = new Float32Array(6 * n)
    // letterbox：64x64 图 → scale 10 全幅无 padding；cx=320,cy=200,w=100,h=50 → 原图 (27,17.5,10,5)
    out[0 * n] = 320
    out[1 * n] = 200
    out[2 * n] = 100
    out[3 * n] = 50
    out[(4 + 1) * n] = 0.9
    detectOut = { data: out, dims: [1, 6, n] }
    try {
      const r = await getCvRunner().detect(solid(64, 64), {
        env: { GEBAI_CV_DETECT_MODEL: join(dir, "model.onnx"), GEBAI_CV_DETECT_LABELS: join(dir, "labels.txt") },
        conf: 0.25,
      })
      expect(r.backend).toBe("wasm-cpu")
      expect(r.objects.length).toBe(1)
      expect(r.objects[0].label).toBe("icon")
      expect(r.objects[0].score).toBeCloseTo(0.9, 5)
      expect(r.objects[0].x).toBeCloseTo(27, 4)
      expect(r.objects[0].y).toBeCloseTo(17.5, 4)
      expect(r.objects[0].w).toBeCloseTo(10, 4)
      expect(r.objects[0].h).toBeCloseTo(5, 4)
    } finally {
      detectOut = null
    }
  })

  test("detect iou 透传：调低后中等重叠同类框被 NMS 抑制", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-det-iou-"))
    writeFileSync(join(dir, "model.onnx"), Buffer.from([9]))
    writeFileSync(join(dir, "labels.txt"), "btn\n")
    const n = 8
    const out = new Float32Array(5 * n)
    const env = { GEBAI_CV_DETECT_MODEL: join(dir, "model.onnx"), GEBAI_CV_DETECT_LABELS: join(dir, "labels.txt") }
    // scale=1 无 padding：框 [0,0,64,64] 与 [0,32,64,64]，IoU ≈ 0.333
    out[0 * n] = 32
    out[1 * n] = 32
    out[2 * n] = 64
    out[3 * n] = 64
    out[4 * n] = 0.9
    out[0 * n + 1] = 32
    out[1 * n + 1] = 64
    out[2 * n + 1] = 64
    out[3 * n + 1] = 64
    out[4 * n + 1] = 0.8
    detectOut = { data: out, dims: [1, 5, n] }
    try {
      const keep = await getCvRunner().detect(solid(64, 64), { env, conf: 0.25 })
      const suppress = await getCvRunner().detect(solid(64, 64), { env, conf: 0.25, iou: 0.1 })
      expect(keep.objects.length).toBe(2)
      expect(suppress.objects.length).toBe(1)
    } finally {
      detectOut = null
    }
  })
})

/** sidecar 替身（仅 runModel 语义；impl 可按 modelKey 分流 det/rec/检测模型）。 */
const fakeSidecar = (impl: (m: { modelKey: string; dims: readonly number[] }) => Promise<{ ep: string; dims: number[]; data: Float32Array }>) =>
  ({ runModel: impl }) as unknown as CvSidecar

describe("OCR 分层后端（sidecar ↔ wasm）", () => {
  test("sidecar 可用即优先：det/rec 两模型经 runModel 推理，backend 如实上报", async () => {
    const dir = modelDir()
    setCvSidecarFactoryForTests(() =>
      fakeSidecar(async (m) => {
        if (m.modelKey.includes("det.onnx")) {
          const h = m.dims[2] ?? 1
          const w = m.dims[3] ?? 1
          return { ep: "dml", dims: [1, 1, h, w], data: new Float32Array(h * w).fill(0.9) } // 整图文本框
        }
        return { ep: "dml", dims: [1, 2, 4], data: new Float32Array([0.1, 0.9, 0, 0, 0.1, 0.9, 0, 0]) } // rec → 字典字符
      }),
    )
    try {
      const r = await getCvRunner().ocr(solid(64, 32), { env: { GEBAI_CV_MODELS_DIR: dir } })
      expect(r.backend).toBe("sidecar:dml")
      expect(r.lines.length).toBe(1)
      expect(r.lines[0].text).toBe("存")
    } finally {
      resetCvSidecarForTests()
      setCvSidecarFactoryForTests(() => null)
    }
  })

  test("auto：sidecar 失败回落 wasm 且毒化（后续调用不再走 sidecar）", async () => {
    const dir = modelDir()
    let sidecarCalls = 0
    setCvSidecarFactoryForTests(() =>
      fakeSidecar(async () => {
        sidecarCalls++
        throw new Error("驱动崩溃")
      }),
    )
    try {
      const r = await getCvRunner().ocr(solid(64, 32), { env: { GEBAI_CV_MODELS_DIR: dir } })
      expect(r.backend).toBe("wasm-cpu")
      expect(r.lines.length).toBe(1) // wasm 全链路照常
      const again = await getCvRunner().ocr(solid(64, 32), { env: { GEBAI_CV_MODELS_DIR: dir } })
      expect(again.backend).toBe("wasm-cpu")
      expect(sidecarCalls).toBeGreaterThanOrEqual(1) // 毒化后不再重试（第二次调用不再触发）
      expect(sidecarCalls).toBeLessThanOrEqual(2)
    } finally {
      resetCvSidecarForTests()
      setCvSidecarFactoryForTests(() => null)
    }
  })

  test("GEBAI_CV_OCR_BACKEND=wasm：忽略可用 sidecar，直接进程内推理", async () => {
    const dir = modelDir()
    let sidecarCalls = 0
    setCvSidecarFactoryForTests(() =>
      fakeSidecar(async () => {
        sidecarCalls++
        return { ep: "dml", dims: [1, 1, 1, 1], data: new Float32Array(1) }
      }),
    )
    try {
      const r = await getCvRunner().ocr(solid(64, 32), { env: { GEBAI_CV_MODELS_DIR: dir, GEBAI_CV_OCR_BACKEND: "wasm" } })
      expect(r.backend).toBe("wasm-cpu")
      expect(sidecarCalls).toBe(0)
    } finally {
      resetCvSidecarForTests()
      setCvSidecarFactoryForTests(() => null)
    }
  })
})

describe("detect 分层后端（sidecar ↔ wasm）", () => {
  const modelEnv = (dir: string) => {
    writeFileSync(join(dir, "model.onnx"), Buffer.from([9]))
    writeFileSync(join(dir, "labels.txt"), "btn\nicon\n")
    return { GEBAI_CV_DETECT_MODEL: join(dir, "model.onnx"), GEBAI_CV_DETECT_LABELS: join(dir, "labels.txt") }
  }

  test("sidecar 可用即优先：backend 如实上报实际 EP", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-sc-"))
    const env = modelEnv(dir)
    const n = 8
    const out = new Float32Array(6 * n)
    out[0 * n] = 320
    out[1 * n] = 200
    out[2 * n] = 100
    out[3 * n] = 50
    out[(4 + 1) * n] = 0.9
    setCvSidecarFactoryForTests(() =>
      fakeSidecar(async () => ({ ep: "dml", dims: [1, 6, n], data: out })),
    )
    try {
      const r = await getCvRunner().detect(solid(64, 64), { env, conf: 0.25 })
      expect(r.backend).toBe("sidecar:dml")
      expect(r.objects.length).toBe(1)
      expect(r.objects[0].label).toBe("icon") // 后处理与 wasm 同一条路径（输出张量同一解码）
    } finally {
      setCvSidecarFactoryForTests(() => null)
    }
  })

  test("auto：sidecar 失败回落 wasm 且毒化（后续调用不再走 sidecar）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-sc2-"))
    const env = modelEnv(dir)
    const n = 8
    const out = new Float32Array(6 * n)
    // 整图框：cx=320,cy=320,w=640,h=640 → 原图 (0,0,64,64)
    out[0 * n] = 320
    out[1 * n] = 320
    out[2 * n] = 640
    out[3 * n] = 640
    out[4 * n] = 0.9
    detectOut = { data: out, dims: [1, 6, n] }
    let sidecarCalls = 0
    setCvSidecarFactoryForTests(() =>
      fakeSidecar(async () => {
        sidecarCalls++
        throw new Error("驱动崩溃")
      }),
    )
    try {
      const r = await getCvRunner().detect(solid(64, 64), { env, conf: 0.25 })
      expect(r.backend).toBe("wasm-cpu")
      expect(r.objects.length).toBe(1) // box [0,0,64,64]（letterbox 全幅）
      const again = await getCvRunner().detect(solid(64, 64), { env, conf: 0.25 })
      expect(again.backend).toBe("wasm-cpu")
      expect(sidecarCalls).toBe(1) // 毒化后不再重试 sidecar
    } finally {
      detectOut = null
      resetCvSidecarForTests()
      setCvSidecarFactoryForTests(() => null)
    }
  })

  test("显式 sidecar：失败不回落，错误如实上抛", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-sc3-"))
    const env = { ...modelEnv(dir), GEBAI_CV_DETECT_BACKEND: "sidecar" }
    setCvSidecarFactoryForTests(() =>
      fakeSidecar(async () => {
        throw new Error("GPU 内存不足")
      }),
    )
    try {
      await expect(getCvRunner().detect(solid(64, 64), { env, conf: 0.25 })).rejects.toThrow(/不回落/)
    } finally {
      resetCvSidecarForTests()
      setCvSidecarFactoryForTests(() => null)
    }
  })

  test("显式 sidecar 但 sidecar 不可用 → 引导错误（不回落）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-sc4-"))
    const env = { ...modelEnv(dir), GEBAI_CV_DETECT_BACKEND: "sidecar" }
    await expect(getCvRunner().detect(solid(64, 64), { env, conf: 0.25 })).rejects.toThrow(/GEBAI_CV_ORT_NODE_DIR/)
  })

  test("显式 wasm：忽略可用 sidecar，直接进程内推理", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-sc5-"))
    const env = { ...modelEnv(dir), GEBAI_CV_DETECT_BACKEND: "wasm" }
    const n = 8
    detectOut = { data: new Float32Array(6 * n), dims: [1, 6, n] }
    let sidecarCalls = 0
    setCvSidecarFactoryForTests(() =>
      fakeSidecar(async () => {
        sidecarCalls++
        return { ep: "dml", dims: [1, 6, n], data: new Float32Array(6 * n) }
      }),
    )
    try {
      const r = await getCvRunner().detect(solid(64, 64), { env, conf: 0.25 })
      expect(r.backend).toBe("wasm-cpu")
      expect(sidecarCalls).toBe(0)
    } finally {
      detectOut = null
      resetCvSidecarForTests()
      setCvSidecarFactoryForTests(() => null)
    }
  })
})

/** 微型 ONNX 编码：field 14 metadata_props（imgsz/names）——元数据与自动发现用例共用。 */
function metaModel(entries: string[]): Buffer {
  const varint = (v: number): number[] => {
    const out: number[] = []
    let n = v
    for (;;) {
      const b = n & 0x7f
      n = Math.floor(n / 128)
      out.push(n > 0 ? b | 0x80 : b)
      if (n === 0) return out
    }
  }
  const ld = (field: number, payload: number[]): number[] => [...varint((field << 3) | 2), ...varint(payload.length), ...payload]
  const str = (s: string): number[] => Array.from(new TextEncoder().encode(s))
  const entry = (k: string, v: string): number[] => ld(14, [...ld(1, str(k)), ...ld(2, str(v))])
  const parts: number[][] = [ld(7, new Array(64).fill(1))]
  for (let i = 0; i + 1 < entries.length; i += 2) parts.push(entry(entries[i], entries[i + 1]))
  const total = parts.reduce((n, p) => n + p.length, 0)
  const out = new Uint8Array(total)
  let off = 0
  for (const p of parts) {
    out.set(p, off)
    off += p.length
  }
  return Buffer.from(out)
}

describe("detect 模型约定目录自动发现（models/detect drop-in 即用）", () => {
  test("目录内唯一 .onnx 自动生效（免 GEBAI_CV_DETECT_MODEL）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-disc-"))
    writeFileSync(join(dir, "a.onnx"), metaModel(["imgsz", "[640, 640]", "names", '{"0":"btn"}']))
    setCvDetectDirForTests(dir)
    const n = 8
    const out = new Float32Array(5 * n)
    out[0 * n] = 320
    out[1 * n] = 320
    out[2 * n] = 640
    out[3 * n] = 640
    out[4 * n] = 0.9
    detectOut = { data: out, dims: [1, 5, n] }
    try {
      const r = await getCvRunner().detect(solid(64, 64), { env: {}, conf: 0.25 }) // env 完全为空
      expect(r.objects.length).toBe(1)
      expect(r.objects[0].label).toBe("btn") // 标签亦来自元数据——全链路零配置
    } finally {
      detectOut = null
      setCvDetectDirForTests(false)
    }
  })

  test("多个 .onnx → 列出候选要求显式指定", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-disc2-"))
    writeFileSync(join(dir, "a.onnx"), metaModel(["names", '{"0":"a"}']))
    writeFileSync(join(dir, "b.onnx"), metaModel(["names", '{"0":"b"}']))
    setCvDetectDirForTests(dir)
    try {
      await expect(getCvRunner().detect(solid(64, 64), { env: {}, conf: 0.25 })).rejects.toThrow(/a\.onnx、b\.onnx/)
    } finally {
      setCvDetectDirForTests(false)
    }
  })

  test("GEBAI_CV_DETECT_MODEL 显式指定优先于约定目录", async () => {
    const discDir = mkdtempSync(join(tmpdir(), "gebai-cv-disc3-"))
    writeFileSync(join(discDir, "auto.onnx"), metaModel(["names", '{"0":"auto"}']))
    const explicitDir = mkdtempSync(join(tmpdir(), "gebai-cv-disc4-"))
    writeFileSync(join(explicitDir, "explicit.onnx"), metaModel(["names", '{"0":"explicit"}']))
    setCvDetectDirForTests(discDir)
    const n = 8
    const out = new Float32Array(5 * n)
    out[4 * n] = 0.9
    out[2 * n] = 640
    out[3 * n] = 640
    detectOut = { data: out, dims: [1, 5, n] }
    try {
      const r = await getCvRunner().detect(solid(64, 64), {
        env: { GEBAI_CV_DETECT_MODEL: join(explicitDir, "explicit.onnx") },
        conf: 0.25,
      })
      expect(r.objects[0].label).toBe("explicit")
    } finally {
      detectOut = null
      setCvDetectDirForTests(false)
    }
  })

  test("目录不存在 / 为空 → 仍走未配置指引", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-disc5-"))
    setCvDetectDirForTests(dir) // 空目录
    try {
      await expect(getCvRunner().detect(solid(64, 64), { env: {}, conf: 0.25 })).rejects.toThrow(/GEBAI_CV_DETECT_MODEL/)
    } finally {
      setCvDetectDirForTests(false)
    }
  })
})

describe("detect 模型元数据自适应（ultralytics ONNX）", () => {

  test("无标签文件时 names/imgsz 元数据驱动类别与输入尺寸（960 letterbox）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-meta-"))
    writeFileSync(
      join(dir, "model.onnx"),
      metaModel(["imgsz", "[960, 960]", "names", '{"0":"button","1":"icon"}']),
    )
    const n = 8
    const out = new Float32Array(6 * n)
    // letterbox 960：64x64 图 → scale 15 全幅无 padding；cx=480,cy=300,w=150,h=75 → 原图 (27,17.5,10,5)
    out[0 * n] = 480
    out[1 * n] = 300
    out[2 * n] = 150
    out[3 * n] = 75
    out[(4 + 1) * n] = 0.9
    detectOut = { data: out, dims: [1, 6, n] }
    try {
      const r = await getCvRunner().detect(solid(64, 64), {
        env: { GEBAI_CV_DETECT_MODEL: join(dir, "model.onnx") }, // 不设 LABELS——走 names 元数据
        conf: 0.25,
      })
      expect(r.objects.length).toBe(1)
      expect(r.objects[0].label).toBe("icon")
      expect(r.objects[0].x).toBeCloseTo(27, 4)
      expect(r.objects[0].y).toBeCloseTo(17.5, 4)
      expect(r.objects[0].w).toBeCloseTo(10, 4)
      expect(r.objects[0].h).toBeCloseTo(5, 4)
    } finally {
      detectOut = null
    }
  })

  test("GEBAI_CV_DETECT_SIZE 覆盖元数据尺寸；GEBAI_CV_DETECT_LABELS 覆盖元数据类别", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-cv-meta2-"))
    writeFileSync(join(dir, "model.onnx"), metaModel(["imgsz", "[960, 960]", "names", '{"0":"meta_a","1":"meta_b"}']))
    writeFileSync(join(dir, "labels.txt"), "file_a\nfile_b\n")
    const n = 8
    const out = new Float32Array(6 * n)
    out[0 * n] = 320
    out[1 * n] = 200
    out[2 * n] = 100
    out[3 * n] = 50
    out[(4 + 1) * n] = 0.9
    detectOut = { data: out, dims: [1, 6, n] }
    try {
      // SIZE=640：scale 10 → cx=320,cy=200,w=100,h=50 → 原图 (27,17.5,10,5)（960 元数据被覆盖）
      const r = await getCvRunner().detect(solid(64, 64), {
        env: {
          GEBAI_CV_DETECT_MODEL: join(dir, "model.onnx"),
          GEBAI_CV_DETECT_LABELS: join(dir, "labels.txt"),
          GEBAI_CV_DETECT_SIZE: "640",
        },
        conf: 0.25,
      })
      expect(r.objects[0].label).toBe("file_b")
      expect(r.objects[0].x).toBeCloseTo(27, 4)
    } finally {
      detectOut = null
    }
  })
})
