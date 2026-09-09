/**
 * 真机端到端验证脚本（bun 运行，非测试）：多语言子代理全链路——
 * 真实驱动（python/cpp/rust/go）、真实 spawn、真实 SubAgentManager/ToolRegistry。
 * 验证：发现注册 → 工具名带前缀 → 常驻状态保持 → 崩溃自愈 → pip status →
 * 构建引导（cpp/rust/go 可执行体缺失时自动编译）→ 四语言典型场景工具真机调用
 * （docqa 文档问答 / imgproc 图像处理 / hsh 哈希校验 / dirs 目录分析）→
 * hsh 跨语言合并（TS 侧 crc32 与 Rust 侧工具同子代理）。
 */
import { SubAgentManager } from "../src/core/agents/subagents"
import { ToolRegistry } from "../src/core/base/registry"
import { disposeAllNativeAgents } from "../src/core/agents/native-agents"
import { mkdirSync, writeFileSync, rmSync, existsSync } from "node:fs"
import { join } from "node:path"

const registry = new ToolRegistry()
const m = new SubAgentManager({ registry, preloadOverride: [] })
m.setNativeAgentsOpts({}) // 本地形态默认启用
await m.discover()

// 收尾时回收边车进程（防孤儿残留）
process.on("exit", () => disposeAllNativeAgents())

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
  workdir: process.cwd(),
  sessionWorkdir: process.cwd(),
  home: process.cwd(),
  env: {},
  resolvePath: (p: string) => p,
  readFile: async () => "",
  writeFile: async () => {},
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
  console.error("FAIL: hsh 应同时含 TS 贡献 crc32 与 native 贡献 sha256:", Object.keys(hshDef.tools ?? {}))
  process.exit(1)
}
if (hshDef.description !== "哈希校验子代理（Rust 边车常驻进程）：hsh_sha256/hsh_sha1/hsh_md5 摘要（文本/字节/文件）、hsh_hmac_sha256 HMAC 签名、hsh_verify 完整性校验（多算法一键比对）") {
  console.error("FAIL: hsh 描述应为 native 单侧贡献（TS 留空不拼接空串）:", hshDef.description)
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
const goDir = join(process.cwd(), "..", "..", "native-agents", "go")
const d1 = await duTool.tool.execute({ dir: goDir, depth: 1, top_k: 5 }, fakeCtx)
console.log("dirs_du:", d1.output.split("\n").slice(0, 3).join(" | "))
if (!d1.output.includes("占用排行")) {
  console.error("FAIL: dirs_du:", d1.output)
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

console.log("\n=== 真机端到端全部通过（python + cpp + rust + go 四语言）===")
disposeAllNativeAgents() // 显式回收后再退出（exit hook 兄弟保险，防孤儿进程）
process.exit(0)
