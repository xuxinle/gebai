import { Language, Parser } from "web-tree-sitter"
import type { Tool } from "./types"
import { truncate } from "./tools"

/**
 * tree-sitter（wasm）语法分析。
 * - wasm 语法文件来自 `tree-sitter-wasms`（node_modules/tree-sitter-wasms/out/）
 * - 首次使用初始化 Parser（懒加载），按语言缓存 parser 实例
 * - 输出「结构概览」（导入/导出、函数/类/方法/类型定义及位置），而非完整 AST，控制给 LLM 的信息量
 */

const LANG_WASM: Record<string, string> = {
  js: "tree-sitter-javascript.wasm",
  jsx: "tree-sitter-javascript.wasm",
  mjs: "tree-sitter-javascript.wasm",
  cjs: "tree-sitter-javascript.wasm",
  ts: "tree-sitter-typescript.wasm",
  tsx: "tree-sitter-tsx.wasm",
  py: "tree-sitter-python.wasm",
  go: "tree-sitter-go.wasm",
  rs: "tree-sitter-rust.wasm",
  java: "tree-sitter-java.wasm",
  c: "tree-sitter-c.wasm",
  h: "tree-sitter-c.wasm",
  cpp: "tree-sitter-cpp.wasm",
  cc: "tree-sitter-cpp.wasm",
  hpp: "tree-sitter-cpp.wasm",
  json: "tree-sitter-json.wasm",
  html: "tree-sitter-html.wasm",
  htm: "tree-sitter-html.wasm",
  css: "tree-sitter-css.wasm",
  sh: "tree-sitter-bash.wasm",
  bash: "tree-sitter-bash.wasm",
  rb: "tree-sitter-ruby.wasm",
  php: "tree-sitter-php.wasm",
  swift: "tree-sitter-swift.wasm",
  kt: "tree-sitter-kotlin.wasm",
  scala: "tree-sitter-scala.wasm",
  lua: "tree-sitter-lua.wasm",
  dart: "tree-sitter-dart.wasm",
  elixir: "tree-sitter-elixir.wasm",
  ex: "tree-sitter-elixir.wasm",
}

/** 导出供构建脚本（scripts/build-analyzer-wasm.ts）收集需内嵌的语法文件（单一真相源，勿与脚本重复维护清单）。 */
export { LANG_WASM }

let initPromise: Promise<void> | null = null
const parsers = new Map<string, Parser>()

/** 测试用：覆盖 wasm 字节加载源（模拟二进制模式资源缺失/内嵌损坏），null 恢复默认。 */
let wasmLoadOverride: ((name: string) => Promise<Uint8Array | null>) | null = null
export function _setWasmLoaderForTest(fn: ((name: string) => Promise<Uint8Array | null>) | null): void {
  wasmLoadOverride = fn
}

/** 初始化 tree-sitter 运行时（幂等单例）。 */
function ensureInit(): Promise<void> {
  if (!initPromise) initPromise = Parser.init().then(() => undefined)
  return initPromise
}

/** 内嵌产物注册表（构建脚本生成的 gzip+base64 wasm）：二进制模式 node_modules 不存在时的回退来源。 */
let embeddedPromise: Promise<Record<string, string> | null> | null = null
function loadEmbeddedRegistry(): Promise<Record<string, string> | null> {
  if (!embeddedPromise) {
    embeddedPromise = import("./analyzer-wasm.embedded.generated.json")
      .then((m) => ((m as { default?: { gzip?: boolean; files?: Record<string, string> } }).default?.gzip ? ((m as { default: { files: Record<string, string> } }).default.files) : null))
      .catch(() => null)
  }
  return embeddedPromise
}

/** 加载语法 wasm 字节：dev 模式读 node_modules（tree-sitter-wasms），失败（二进制/打包模式）回退内嵌产物。 */
async function loadWasmBytes(name: string): Promise<Uint8Array | null> {
  try {
    const wasmPath = require.resolve(`tree-sitter-wasms/out/${name}`)
    return new Uint8Array(await Bun.file(wasmPath).arrayBuffer())
  } catch {
    const files = await loadEmbeddedRegistry()
    const b64 = files?.[name]
    if (!b64) return null
    try {
      return Bun.gunzipSync(Buffer.from(b64, "base64"))
    } catch {
      return null // 内嵌产物损坏
    }
  }
}

/** 按语言加载（并缓存）parser。语言不支持返回 null；资源加载失败同样返回 null（调用方按「不可用」报错）。 */
async function parserFor(lang: string): Promise<Parser | null> {
  const wasmName = LANG_WASM[lang]
  if (!wasmName) return null
  const cached = parsers.get(lang)
  if (cached && !wasmLoadOverride) return cached
  await ensureInit()
  const buf = wasmLoadOverride ? await wasmLoadOverride(wasmName) : await loadWasmBytes(wasmName)
  if (!buf) return null
  try {
    const language = await Language.load(buf)
    const parser = new Parser()
    parser.setLanguage(language)
    parsers.set(lang, parser)
    return parser
  } catch {
    return null
  }
}

/** 声明类节点类型判定（不同语言的函数/类/导入/类型定义）。 */
const DECL_RE = /(function|class|method|definition|declaration|import|export|interface|type_declaration|assignment|statement)$/i

function nodeName(node: import("web-tree-sitter").Node): string {
  const field = node.childForFieldName("name")
  if (field) return field.text
  const first = node.namedChildren[0]
  if (first && first.type === "identifier") return first.text
  return ""
}

interface OutlineEntry {
  type: string
  name: string
  start: number
  end: number
  depth: number
}

/** 提取结构概览：顶层声明 + 类内方法（限制深度与总量，避免嵌套噪音）。 */
function extractOutline(root: import("web-tree-sitter").Node): OutlineEntry[] {
  const out: OutlineEntry[] = []
  const walk = (node: import("web-tree-sitter").Node, depth: number) => {
    if (depth > 4 || out.length > 120) return
    const type = node.type
    const isDecl = DECL_RE.test(type) || type === "import_statement" || type === "export_statement"
    if (isDecl && depth >= 1 && depth <= 3 && node.namedChildren.length > 0) {
      // export/import 包裹的声明：提取内部声明名，避免与内层重复
      let name = nodeName(node)
      let label = type
      if (!name && (type.startsWith("export") || type.startsWith("import"))) {
        const inner = node.namedChildren.find((c) => c && DECL_RE.test(c.type))
        if (inner) {
          name = nodeName(inner)
          label = `${type} ${inner.type}`
        }
      }
      out.push({ type: label, name, start: node.startPosition.row + 1, end: node.endPosition.row + 1, depth })
      // export/import 不递归内层（避免重复）；其余（含 class）继续下钻收集方法
      if (!type.startsWith("export") && !type.startsWith("import")) {
        for (const c of node.namedChildren) {
          if (c) walk(c, depth + 1)
        }
      }
      return
    }
    for (const c of node.namedChildren) {
      if (c) walk(c, depth + 1)
    }
  }
  for (const c of root.namedChildren) {
    if (c) walk(c, 1)
  }
  return out
}

/** 用 tree-sitter 解析代码，返回结构概览文本。 */
export async function analyzeCode(code: string, ext: string, displayPath: string): Promise<string> {
  if (!LANG_WASM[ext]) return `analyze: 不支持的语言（.${ext}）。支持: ${Object.keys(LANG_WASM).slice(0, 20).join(", ")}`
  const parser = await parserFor(ext)
  if (!parser) {
    // 与「不支持的语言」区分：语法在支持列表内但 wasm 资源加载失败（二进制打包未内嵌/依赖缺失/内嵌损坏），
    // 误导性报错会令模型反复尝试或放弃正确路径
    return `analyze: 语法分析不可用（tree-sitter wasm 资源加载失败：${ext}）——请确认依赖已安装（dev 模式 bun install）或构建时已内嵌（二进制打包应运行 scripts/build-analyzer-wasm.ts）。可改用 read 分段阅读该文件。`
  }
  const tree = parser.parse(code)!
  const root = tree.rootNode
  const entries = extractOutline(root)
  // Tree/Node 为 wasm 原生内存对象（不随 GC 自动回收）：用毕显式释放，防频繁分析的堆累积
  tree.delete?.()
  if (!entries.length) return `[${ext}] ${displayPath} — 未发现可提取的顶层结构（${root.type} 根节点）。`
  const lines = [`[${ext}] ${displayPath} — ${entries.length} 个结构定义:`]
  for (const e of entries) {
    const indent = e.depth > 1 ? "  " : ""
    const name = e.name ? ` ${e.name}` : ""
    lines.push(`${indent}- ${e.type}${name} (${e.start}-${e.end})`)
  }
  return lines.join("\n")
}

/** search_symbols：单文件大小上限、最多扫描文件数、匹配条目上限。 */
export const SYMBOL_MAX_FILE_BYTES = 1024 * 1024
export const SYMBOL_MAX_FILES = 500
export const SYMBOL_MAX_MATCHES = 50

export interface SymbolHit {
  path: string
  line: number
  type: string
  name: string
}

/**
 * 跨文件符号定义搜索：内容预筛（includes 快路径，避免全量解析）→ tree-sitter 解析 →
 * 收集名称含 symbol 的结构定义条目（精确匹配优先）。files 为候选文件列表（size 用于预筛）。
 */
export async function searchSymbols(
  files: Array<{ path: string; size: number }>,
  readFn: (p: string) => Promise<string>,
  symbol: string,
  kind?: string,
): Promise<{ hits: SymbolHit[]; scanned: number; parsed: number }> {
  const hits: SymbolHit[] = []
  let scanned = 0
  let parsed = 0
  for (const f of files) {
    if (hits.length >= SYMBOL_MAX_MATCHES || scanned >= SYMBOL_MAX_FILES) break
    if (f.size > SYMBOL_MAX_FILE_BYTES) continue
    const ext = f.path.split(".").pop()?.toLowerCase() ?? ""
    if (!LANG_WASM[ext]) continue
    scanned++
    let content: string
    try {
      content = await readFn(f.path)
    } catch {
      continue
    }
    if (!content.includes(symbol)) continue // 预筛：不含符号名则跳过解析（快路径）
    const parser = await parserFor(ext)
    if (!parser) continue
    parsed++
    const tree = parser.parse(content)
    if (!tree) continue
    try {
      for (const e of extractOutline(tree.rootNode)) {
        if (!e.name || hits.length >= SYMBOL_MAX_MATCHES) break
        if (kind && !e.type.includes(kind)) continue
        if (e.name !== symbol && !e.name.includes(symbol)) continue
        hits.push({ path: f.path, line: e.start, type: e.type, name: e.name })
      }
    } finally {
      tree.delete?.() // wasm 原生内存显式释放（同 analyze）
    }
  }
  // 精确匹配优先，其余按路径/行号稳定排序
  hits.sort((a, b) => Number(a.name !== symbol) - Number(b.name !== symbol) || a.path.localeCompare(b.path) || a.line - b.line)
  return { hits, scanned, parsed }
}

/** code 符号搜索工具（注册为 code_search_symbols）。 */
export const searchSymbolsTool: Tool = {
  name: "search_symbols",
  description:
    "按符号名搜索代码定义位置（tree-sitter 解析：函数/类/方法/接口/类型定义），返回 文件:行号: 类型 名称，精确匹配优先。找**定义**比 grep 精准；找**引用/调用点**用 grep。",
  card: { titleParams: ["symbol"] },
  parameters: {
    type: "object",
    properties: {
      symbol: { type: "string", description: "符号名（如函数名 handleRequest、类名 Engine；精确或名称包含匹配）" },
      path: { type: "string", description: "搜索起点（默认 .）" },
      kind: { type: "string", description: "可选：定义类型过滤（function/class/method/interface/type 等，按类型名包含匹配）" },
    },
    required: ["symbol"],
  },
  async execute(args, ctx) {
    const symbol = String(args.symbol)
    const path = args.path ? String(args.path) : ""
    const prefix = path ? `${path.replace(/\/+$/, "")}/` : ""
    const files = (await ctx.listFiles()).filter((f) => !f.isDir && (prefix ? f.path.startsWith(prefix) : true))
    const { hits, scanned, parsed } = await searchSymbols(files, (p) => ctx.readFile(ctx.resolvePath(p)), symbol, args.kind ? String(args.kind) : undefined)
    if (!hits.length) {
      const scannedNote = files.length === 0 ? "（无可扫描文件）" : `（扫描 ${Math.min(scanned, files.length)}/${files.length} 个文件）`
      return { output: `search_symbols: 未找到符号定义: ${symbol}${scannedNote}；内容搜索请用 grep` }
    }
    const lines = hits.map((h) => `${h.path}:${h.line}: ${h.type} ${h.name}`)
    const result = `找到 ${hits.length} 处定义（扫描 ${Math.min(scanned, files.length)} 个文件，解析 ${parsed} 个）:\n${lines.join("\n")}`
    return truncate(result, "search_symbols", ctx)
  },
}

/** code 语法分析工具（注册为 code_analyze）。 */
export const analyzeTool: Tool = {
  name: "analyze",
  description: "使用 tree-sitter 语法分析器解析代码文件，返回结构化概览（导入/导出、函数/类/方法/类型定义、位置行号与嵌套关系）。支持 JS/TS/TSX/Python/Go/Rust/Java/C/C++/JSON/HTML/CSS/Bash/Ruby/PHP/Swift/Kotlin 等。用于快速理解文件结构与定位代码。",
  card: { titleParams: ["path"] },
  parameters: { type: "object", properties: { path: { type: "string", description: "代码文件路径" } }, required: ["path"] },
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    const content = await ctx.readFile(path)
    const ext = String(args.path).split(".").pop()?.toLowerCase() ?? ""
    const result = await analyzeCode(content, ext, String(args.path))
    return truncate(result, "analyze", ctx)
  },
}
