/**
 * 真机端到端验证脚本（bun 运行，非测试）：客卿全链路——
 * 真实驱动（python/cpp/rust/go）、真实 spawn、真实 SubAgentManager/ToolRegistry。
 * 验证：发现注册 → 工具名带前缀 → 常驻状态保持 → 崩溃自愈 → pip status →
 * 构建引导（cpp/rust/go 可执行体缺失时自动编译）→ 四语言典型场景工具真机调用
 * （docqa 文档问答 / imgproc 图像处理 / hsh 哈希校验 / dirs 目录分析）→
 * hsh 跨语言合并（TS 侧 crc32 与 Rust 侧工具同子代理）→
 * vision 跨语言合并（TS 侧 analyze + Python 侧识别四工具，依赖就绪时）→
 * 请求级 ctx（协议 v2：docqa_run 无 session 参数时 REPL 命名空间按 ctx.sessionId 隔离）。
 */
import { SubAgentManager } from "../src/core/agents/subagents"
import { ToolRegistry } from "../src/core/base/registry"
import { disposeAllKeqing } from "../src/core/agents/keqing"
import { mkdirSync, writeFileSync, rmSync, existsSync } from "node:fs"
import { join } from "node:path"

const registry = new ToolRegistry()
const m = new SubAgentManager({ registry, preloadOverride: [] })
m.setKeqingOpts({}) // 本地形态默认启用
await m.discover()

// 收尾时回收边车进程（防孤儿残留）
process.on("exit", () => disposeAllKeqing())

const expectAgent = (name: string) => {
  const def = m.def(name)
  if (!def) {
    console.error(`FAIL: ${name} 子代理未注册`)
    console.error("loadErrors:", m.loadError(name))
    process.exit(1)
  }
  console.log(`PASS: ${name} 子代理已注册，工具:`, Object.keys(def.tools ?? {}))
  return def
}

const fakeCtx = {
  user: "admin",
  sessionId: "e2e-session",
  workdir: process.cwd(),
  sessionWorkdir: process.cwd(),
  home: process.cwd(),
  env: {},
  resolvePath: (p: string) => p,
  readFile: async () => "",
  readBinaryFile: async (p: string) => new Uint8Array(await Bun.file(p).arrayBuffer()),
  writeFile: async () => {},
  writeBinaryFile: async (p: string, data: Uint8Array) => {
    await Bun.write(p, data)
  },
  listFiles: async () => [],
  listDir: async () => [],
  deleteFile: async () => {},
  moveFile: async () => {},
  runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
  uploadAttachment: async (r: { name: string }) => r.name,
  publish: () => {},
  projects: [],
  resolveProjectPath: (n: string) => n,
  getTodos: async () => [],
  setTodos: async () => {},
  registry: { schemas: () => [], resolve: (n: string) => ({ name: n, tool: null }), getAgentNames: () => [] },
  listSubAgentDefs: () => [],
  loadSubAgent: async () => {},
} as never

// ---------------- docqa（Python）：文档问答全链路 ----------------
const docqa = expectAgent("docqa")
console.log("提示词前 80 字:", docqa.systemPrompt.slice(0, 80).replace(/\n/g, " "))
await m.load("docqa")

const indexTool = registry.resolve("docqa_index")
const queryTool = registry.resolve("docqa_query")
if (!indexTool || !queryTool) {
  console.error("FAIL: docqa_index/docqa_query 未在注册表")
  process.exit(1)
}

// 构造临时语料（验证索引/检索/高亮全链路）
const corpus = join(process.cwd(), "tmp-e2e-docqa")
rmSync(corpus, { recursive: true, force: true })
mkdirSync(corpus, { recursive: true })
writeFileSync(join(corpus, "a.md"), "# 边车协议\n\n宿主与驱动通过 stdin/stdout 交换 NDJSON。超时由宿主控制，默认 120 秒。\n")
writeFileSync(join(corpus, "b.md"), "# 索引设计\n\nBM25 是经典词法检索排序函数，k1 控制词频饱和，b 控制长度归一化。\n")
writeFileSync(join(corpus, "c.txt"), "无关内容：部署清单与沙箱开关说明。\n")

const r1 = await indexTool.tool.execute({ dir: corpus }, fakeCtx)
console.log("docqa_index:", r1.output.split("\n")[0])
if (!/索引完成: 3 个文档/.test(r1.output)) {
  console.error("FAIL: docqa 索引应含 3 个文档:", r1.output)
  process.exit(1)
}
// 检索命中（中文二元分词 + 高亮）
const r2 = await queryTool.tool.execute({ query: "边车 超时", dir: corpus, top_k: 2 }, fakeCtx)
console.log("docqa_query 首行:", r2.output.split("\n")[0])
if (!r2.output.includes("a.md") || !r2.output.includes("【")) {
  console.error("FAIL: docqa 检索未命中 a.md 或未高亮:", r2.output)
  process.exit(1)
}
// 增量索引复用（mtime/size 未变 → 复用旧词条）
const r3 = await indexTool.tool.execute({ dir: corpus }, fakeCtx)
if (!/索引完成: 3 个文档/.test(r3.output)) {
  console.error("FAIL: docqa 增量索引:", r3.output)
  process.exit(1)
}
console.log("PASS: docqa（Python）索引 + BM25 检索 + 高亮 + 增量复用")

// Python 基础工具（driver.py 框架能力仍可用：run/pip/status 合并上报）
const runTool = registry.resolve("docqa_run")
if (!runTool) {
  console.error("FAIL: docqa 未合并基础 run 工具")
  process.exit(1)
}
// 常驻命名空间状态保持
const p1 = await runTool.tool.execute({ code: "import math\nval = math.pi\nval", session: "e2e" }, fakeCtx)
if (!p1.output.includes("3.14")) {
  console.error("FAIL: 首次执行应回显 math.pi:", p1.output)
  process.exit(1)
}
const p2 = await runTool.tool.execute({ code: "round(val * 2, 4)", session: "e2e" }, fakeCtx)
if (p2.output.trim() !== "6.2832") {
  console.error("FAIL: 常驻状态丢失:", p2.output)
  process.exit(1)
}
console.log("PASS: docqa 常驻命名空间状态保持（tools.py 合并基础工具）")

// 请求级 ctx（协议 v2）：无 session 参数时 REPL 命名空间缺省按 ctx.sessionId 隔离——
// 不同会话（sessionId 不同）互不可见，同会话共享
const c1 = await runTool.tool.execute({ code: "ctx_val = 42\nctx_val" }, fakeCtx)
if (!c1.output.includes("42")) {
  console.error("FAIL: ctx 缺省命名空间执行:", c1.output)
  process.exit(1)
}
const otherCtx = { ...(fakeCtx as Record<string, unknown>), sessionId: "e2e-other-session" } as never
const c2 = await runTool.tool.execute({ code: "'ctx_val' in dir()" }, otherCtx)
if (c2.output.includes("True")) {
  console.error("FAIL: 跨会话命名空间应隔离（ctx.sessionId 分桶）:", c2.output)
  process.exit(1)
}
const c3 = await runTool.tool.execute({ code: "ctx_val" }, fakeCtx)
if (!c3.output.includes("42")) {
  console.error("FAIL: 同会话命名空间应共享:", c3.output)
  process.exit(1)
}
console.log("PASS: 请求级 ctx（协议 v2）REPL 命名空间按 sessionId 隔离（跨会话互不可见）")

// pip status
const pipTool = registry.resolve("docqa_pip")!
const p3 = await pipTool.tool.execute({ action: "status" }, fakeCtx)
if (!p3.output.includes("venv:")) {
  console.error("FAIL: docqa_pip status 无 venv 报告:", p3.output)
  process.exit(1)
}
console.log("PASS: docqa_pip status 报告")

// 崩溃自愈：驱动内 os._exit(1) → 宿主重启 → 下次调用新进程成功
await runTool.tool.execute({ code: "import os\nos._exit(1)", session: "crash" }, fakeCtx).catch(() => "")
const p4 = await runTool.tool.execute({ code: "'alive-after-crash'", session: "crash2" }, fakeCtx)
if (!p4.output.includes("alive-after-crash")) {
  console.error("FAIL: 崩溃后新进程未恢复:", p4.output)
  process.exit(1)
}
console.log("PASS: 崩溃自愈（真实 os._exit → 重启 → 新进程可用）")

rmSync(corpus, { recursive: true, force: true })
console.log("\n=== docqa（Python）段全部通过 ===")

// ---------------- imgproc（C++）：图像处理 ----------------
expectAgent("imgproc")
await m.load("imgproc")
// 1x1 红色 PNG（手写最小合法 PNG 字节）——不依赖外部图片资产
const pngPath = join(process.cwd(), "tmp-e2e-imgproc.png")
const pngB64 =
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=="
writeFileSync(pngPath, Buffer.from(pngB64, "base64"))

const infoTool = registry.resolve("imgproc_info")!
const grayTool = registry.resolve("imgproc_grayscale")!
const resizeTool = registry.resolve("imgproc_resize")!
const statsTool = registry.resolve("imgproc_stats")!
const i1 = await infoTool.tool.execute({ path: pngPath }, fakeCtx)
console.log("imgproc_info:", i1.output.replace(/\n/g, " | "))
if (!i1.output.includes("1x1")) {
  console.error("FAIL: imgproc_info 应为 1x1:", i1.output)
  process.exit(1)
}
const i2 = await resizeTool.tool.execute({ path: pngPath, width: 8, output: join(process.cwd(), "tmp-e2e-imgproc-8.png") }, fakeCtx)
if (!i2.output.includes("1x1 -> 8x8")) {
  console.error("FAIL: imgproc_resize 等比缩放:", i2.output)
  process.exit(1)
}
const i3 = await grayTool.tool.execute({ path: pngPath }, fakeCtx)
if (!existsSync(join(process.cwd(), "tmp-e2e-imgproc.png.gray.png"))) {
  console.error("FAIL: imgproc_grayscale 未产出文件:", i3.output)
  process.exit(1)
}
const i4 = await statsTool.tool.execute({ path: pngPath }, fakeCtx)
if (!i4.output.includes("Otsu")) {
  console.error("FAIL: imgproc_stats 无 Otsu:", i4.output)
  process.exit(1)
}
// 清理测试产物
for (const f of [pngPath, join(process.cwd(), "tmp-e2e-imgproc-8.png"), join(process.cwd(), "tmp-e2e-imgproc.png.gray.png")]) {
  rmSync(f, { force: true })
}
console.log("PASS: imgproc（C++ + stb）info/grayscale/resize/stats")

// ---------------- hsh（Rust + TS 跨语言合并）：哈希校验（RFC 官方向量）----------------
const hshDef = expectAgent("hsh")
// 跨语言合并：TS 侧贡献 crc32（描述/提示词留空），Rust 侧贡献 sha256/... 与描述提示词——同一子代理
if (!("crc32" in (hshDef.tools ?? {})) || !("sha256" in (hshDef.tools ?? {}))) {
  console.error("FAIL: hsh 应同时含 TS 贡献 crc32 与 客卿 贡献 sha256:", Object.keys(hshDef.tools ?? {}))
  process.exit(1)
}
if (hshDef.description !== "哈希校验子代理（Rust 边车常驻进程）：hsh_sha256/hsh_sha1/hsh_md5 摘要（文本/字节/文件）、hsh_hmac_sha256 HMAC 签名、hsh_verify 完整性校验（多算法一键比对）") {
  console.error("FAIL: hsh 描述应为 客卿 单侧贡献（TS 留空不拼接空串）:", hshDef.description)
  process.exit(1)
}
await m.load("hsh")
const crc32Tool = registry.resolve("hsh_crc32")!
const h0 = await crc32Tool.tool.execute({ text: "123456789" }, fakeCtx)
if ((h0.data as { digest?: string })?.digest !== "cbf43926") {
  console.error("FAIL: hsh_crc32('123456789') 不符标准向量 cbf43926:", h0.output)
  process.exit(1)
}
console.log("PASS: hsh 跨语言合并（TS 贡献 crc32 与 Rust 工具同命名空间，标准向量通过）")
const sha256Tool = registry.resolve("hsh_sha256")!
const h1 = await sha256Tool.tool.execute({ text: "abc" }, fakeCtx)
if ((h1.data as { digest?: string })?.digest !== "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
  console.error("FAIL: hsh_sha256('abc') 不符 RFC 向量:", h1.output)
  process.exit(1)
}
const hmacTool = registry.resolve("hsh_hmac_sha256")!
const h2 = await hmacTool.tool.execute({ key: "Jefe", text: "what do ya want for nothing?" }, fakeCtx)
if (!h2.output.includes("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")) {
  console.error("FAIL: hsh_hmac_sha256 不符 RFC 4231 向量:", h2.output)
  process.exit(1)
}
const verifyTool = registry.resolve("hsh_verify")!
const h3 = await verifyTool.tool.execute({ text: "abc", sha256: "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD" }, fakeCtx)
if (!h3.output.includes("校验通过")) {
  console.error("FAIL: hsh_verify 大写 hex 应通过:", h3.output)
  process.exit(1)
}
const h4 = await verifyTool.tool.execute({ text: "abc", md5: "deadbeef" }, fakeCtx)
if (!h4.output.includes("校验失败")) {
  console.error("FAIL: hsh_verify 错误值应失败:", h4.output)
  process.exit(1)
}
console.log("PASS: hsh（Rust）SHA-256/HMAC RFC 向量 + verify 双向判定 + TS crc32 合并")

// ---------------- dirs（Go）：目录空间分析 ----------------
expectAgent("dirs")
await m.load("dirs")
const duTool = registry.resolve("dirs_du")!
const depthTool = registry.resolve("dirs_depth")!
const topTool = registry.resolve("dirs_top")!
const treeTool = registry.resolve("dirs_tree")!
const goDir = join(process.cwd(), "..", "..", "keqing", "go")
const d1 = await duTool.tool.execute({ dir: goDir, depth: 1, top_k: 5 }, fakeCtx)
console.log("dirs_du:", d1.output.split("\n").slice(0, 3).join(" | "))
if (!d1.output.includes("占用排行")) {
  console.error("FAIL: dirs_du:", d1.output)
  process.exit(1)
}
// 参数到达验证：data.root 必须等于传参目录（tool.call 平级 args 被丢弃时工具会回退缺省目录，输出仍含「占用排行」标题）
if (String((d1.data as Record<string, unknown>)?.root ?? "").replaceAll("\\", "/") !== goDir.replaceAll("\\", "/")) {
  console.error(`FAIL: dirs_du 参数未到达工具（data.root=${String((d1.data as Record<string, unknown>)?.root)}，期望 ${goDir}）——tool.call 请求的平级 args 被丢弃`)
  process.exit(1)
}
const d2 = await depthTool.tool.execute({ dir: goDir }, fakeCtx)
if (!/文件: \d+/.test(d2.output) || !/最大深度/.test(d2.output)) {
  console.error("FAIL: dirs_depth:", d2.output)
  process.exit(1)
}
const d3 = await topTool.tool.execute({ dir: goDir, top_k: 3 }, fakeCtx)
if (!d3.output.includes("最大文件排行")) {
  console.error("FAIL: dirs_top:", d3.output)
  process.exit(1)
}
const d4 = await treeTool.tool.execute({ dir: goDir, max_depth: 2 }, fakeCtx)
if (!d4.output.includes("项）")) {
  console.error("FAIL: dirs_tree:", d4.output)
  process.exit(1)
}
console.log("PASS: dirs（Go）tree/du/top/depth（并发遍历）")

// ---------------- vision（Python + TS 跨语言合并）：识别四工具 + analyze ----------------
const visionDef = expectAgent("vision")
// 跨语言合并：TS 侧贡献 analyze（描述留空），Python 侧贡献 ocr/locate/locate_image/detect
if (!("analyze" in (visionDef.tools ?? {})) || !("ocr" in (visionDef.tools ?? {}))) {
  console.error("FAIL: vision 应同时含 TS 贡献 analyze 与 客卿 贡献 ocr:", Object.keys(visionDef.tools ?? {}))
  process.exit(1)
}
if (!visionDef.description || !visionDef.description.includes("本地")) {
  console.error("FAIL: vision 描述应由 客卿 侧贡献（TS 留空不拼接空串）:", visionDef.description)
  process.exit(1)
}
await m.load("vision")
const ocrTool = registry.resolve("vision_ocr")!
// 依赖/模型就绪时真机 OCR（生成含文字 PNG 不现实，用空图验证链路与错误引导）；
// 依赖缺失时验证安装提示而非栈追踪
const vDir = join(process.cwd(), "tmp-e2e-vision")
rmSync(vDir, { recursive: true, force: true })
mkdirSync(vDir, { recursive: true })
const vPng = join(vDir, "blank.png")
// 纯白 64x32 PNG（用系统 python + PIL 生成真实可解码文件；PIL 严格校验 CRC，手写最小编码会被拒）
Bun.spawnSync(["python", "-X", "utf8", "-c", "from PIL import Image; Image.new('RGB', (64, 32), (255,255,255)).save(r'" + vPng.replace(/\\/g, "/") + "')"], { stdout: "ignore", stderr: "pipe" })
if (!existsSync(vPng)) {
  console.log("SKIP: vision 真机 OCR（无系统 python/PIL 生成测试图）")
} else {
  const v1 = await ocrTool.tool.execute({ image: vPng }, fakeCtx)
  if (/Traceback|ImportError|ModuleNotFoundError/.test(v1.output)) {
    console.error("FAIL: vision_ocr 依赖缺失应给安装提示而非栈:", v1.output)
    process.exit(1)
  }
  const missingDeps = /依赖缺失/.test(v1.output)
  const missingModels = /模型未配置/.test(v1.output)
  if (missingDeps || missingModels) {
    console.log(`SKIP: vision 真机 OCR（${missingDeps ? "依赖未装" : "模型未配置"}）——链路与引导文案验证通过:`)
    console.log("  ", v1.output.split("\n")[0])
  } else {
    // 依赖与模型就绪：纯白空图应正常返回「未识别到文字」引导
    if (!v1.output.includes("未识别到文字")) {
      console.error("FAIL: vision_ocr 空图应返回未识别引导:", v1.output)
      process.exit(1)
    }
    console.log("PASS: vision 真机 OCR（onnxruntime 原生推理）空图引导")
  }
}
rmSync(vDir, { recursive: true, force: true })
console.log("PASS: vision 跨语言合并（TS 贡献 analyze 与 Python 识别四工具同命名空间）")

// ---------------- desktop_ocr → vision 边车委托（sidecar-first 真机链路） ----------------
// desktop 的 ocr/locate/detect 推理经注册表调用 vision 边车（onnxruntime 原生推理），
// 坐标语义与 wasm 同构；此处验证真边车进程 + 真推理 + 真坐标映射（含文字图片）
if (process.platform === "win32" && existsSync(join(process.cwd(), "..", "..", "keqing", "python", "venv"))) {
  await m.load("desktop") // 懒装载：desktop 工具入注册表（desktop_ocr 等）
  const desktopOcr = registry.resolve("desktop_ocr")
  if (!desktopOcr) {
    console.error("FAIL: desktop_ocr 未在注册表（desktop 子代理未注册）")
    process.exit(1)
  }
  // 生成含文字 PNG（真机 OCR：PIL 画字，与本会话验证到的边车 OCR 能力对接）
  const e2eDir = join(process.cwd(), "tmp-e2e-desktop")
  rmSync(e2eDir, { recursive: true, force: true })
  mkdirSync(e2eDir, { recursive: true })
  const shotPng = join(e2eDir, "shot.png")
  Bun.spawnSync(["python", "-X", "utf8", "-c", "from PIL import Image, ImageDraw, ImageFont; f = ImageFont.truetype(r'C:/Windows/Fonts/arial.ttf', 20); im = Image.new('RGB', (160, 44), (255,255,255)); d = ImageDraw.Draw(im); d.text((8, 10), 'Hello OCR 123', fill=(0,0,0), font=f); im.save(r'" + shotPng.replace(/\\/g, "/") + "')"], { stdout: "ignore", stderr: "pipe" })
  if (existsSync(shotPng)) {
    const r5 = await desktopOcr.tool.execute({ image: shotPng }, { ...fakeCtx, registry: { schemas: () => [], resolve: (n: string) => (n === "vision_ocr" ? registry.resolve("vision_ocr") : undefined), getAgentNames: () => ["vision"] } } as never)
    const lines = (r5.data as { lines?: Array<{ text: string }> })?.lines ?? []
    if (r5.output.includes("HelloOCR123") || lines.some((l) => l.text.replace(/\s/g, "").includes("HelloOCR123"))) {
      console.log("PASS: desktop_ocr → vision 边车委托（真边车真推理，坐标回加同构）")
    } else if (/本地识别失败|未配置/.test(r5.output)) {
      console.log("SKIP: desktop_ocr 委托真机推理（模型未配置）:", r5.output.split("\n")[0])
    } else {
      console.error("FAIL: desktop_ocr 委托未得到预期文字:", r5.output)
      process.exit(1)
    }
    rmSync(e2eDir, { recursive: true, force: true })
  } else {
    console.log("SKIP: desktop_ocr 委托真机（无系统 python/PIL 生成测试图）")
  }
}

console.log("\n=== 真机端到端全部通过（python + cpp + rust + go 四语言 + vision 跨语言合并 + 请求级 ctx + desktop→vision 委托）===")
disposeAllKeqing() // 显式回收后再退出（exit hook 兄弟保险，防孤儿进程）
process.exit(0)
