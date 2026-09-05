import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { getCvRunner, setCvDevAssetsDirForTests, setCvOrtLoader, setCvRunnerFactory } from "./cv"
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
})

afterAll(() => {
  setCvOrtLoader(null)
  setCvDevAssetsDirForTests(undefined)
  setCvRunnerFactory(null)
})

describe("cv runner 注入", () => {
  test("setCvRunnerFactory 替身优先", () => {
    const fake = { ocr: async () => [], detect: async () => [] }
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
    expect(r.length).toBe(1)
    expect(r[0].text).toBe("存")
    expect(r[0].score).toBeGreaterThan(0.8)
    expect(r[0].box.w).toBeGreaterThan(50)
    expect(r[0].box.h).toBeGreaterThan(20)
  })

  test("det 无命中（全低概率）→ 空行列表", async () => {
    const original = detProbFactory
    detProbFactory = (h, w) => ({ data: new Float32Array(h * w).fill(0.1), dims: [1, 1, h, w] })
    try {
      const r = await getCvRunner().ocr(solid(64, 32), { env: { GEBAI_CV_MODELS_DIR: modelDir() } })
      expect(r.length).toBe(0)
    } finally {
      detProbFactory = original
    }
  })

  test("并发两次 OCR 不死锁（串行互斥链）", async () => {
    const env = { GEBAI_CV_MODELS_DIR: modelDir() }
    const [a, b] = await Promise.all([getCvRunner().ocr(solid(64, 32), { env }), getCvRunner().ocr(solid(64, 32), { env })])
    expect(a.length).toBe(1)
    expect(b.length).toBe(1)
  })

  test("detect 未配置 → 指引错误", async () => {
    await expect(getCvRunner().detect(solid(64, 64), { env: {}, conf: 0.25 })).rejects.toThrow(/GEBAI_CV_DETECT_MODEL/)
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
      const objs = await getCvRunner().detect(solid(64, 64), {
        env: { GEBAI_CV_DETECT_MODEL: join(dir, "model.onnx"), GEBAI_CV_DETECT_LABELS: join(dir, "labels.txt") },
        conf: 0.25,
      })
      expect(objs.length).toBe(1)
      expect(objs[0].label).toBe("icon")
      expect(objs[0].score).toBeCloseTo(0.9, 5)
      expect(objs[0].x).toBeCloseTo(27, 4)
      expect(objs[0].y).toBeCloseTo(17.5, 4)
      expect(objs[0].w).toBeCloseTo(10, 4)
      expect(objs[0].h).toBeCloseTo(5, 4)
    } finally {
      detectOut = null
    }
  })
})
