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

let initPromise: Promise<void> | null = null
const parsers = new Map<string, Parser>()

/** 初始化 tree-sitter 运行时（幂等单例）。 */
function ensureInit(): Promise<void> {
  if (!initPromise) initPromise = Parser.init().then(() => undefined)
  return initPromise
}

/** 按语言加载（并缓存）parser。不支持的语言返回 null。 */
async function parserFor(lang: string): Promise<Parser | null> {
  const wasmName = LANG_WASM[lang]
  if (!wasmName) return null
  const cached = parsers.get(lang)
  if (cached) return cached
  await ensureInit()
  try {
    const wasmPath = require.resolve(`tree-sitter-wasms/out/${wasmName}`)
    // Uint8Array 加载：兼容二进制打包与各运行时（Bun/Node 均可）
    const buf = new Uint8Array(await Bun.file(wasmPath).arrayBuffer())
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
  const parser = await parserFor(ext)
  if (!parser) return `analyze: 不支持的语言（.${ext}）。支持: ${Object.keys(LANG_WASM).slice(0, 20).join(", ")}`
  const tree = parser.parse(code)!
  const root = tree.rootNode
  const entries = extractOutline(root)
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
    for (const e of extractOutline(tree.rootNode)) {
      if (!e.name || hits.length >= SYMBOL_MAX_MATCHES) break
      if (kind && !e.type.includes(kind)) continue
      if (e.name !== symbol && !e.name.includes(symbol)) continue
      hits.push({ path: f.path, line: e.start, type: e.type, name: e.name })
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
    "在目录中按符号名搜索代码定义位置（tree-sitter 解析：函数/类/方法/接口/类型定义），返回 文件:行号: 类型 名称，精确匹配优先。定位函数/类等定义比 grep 更精准（grep 仅内容匹配）；纯内容搜索请用 grep。",
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
  parameters: { type: "object", properties: { path: { type: "string", description: "代码文件路径" } }, required: ["path"] },
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    const content = await ctx.readFile(path)
    const ext = String(args.path).split(".").pop()?.toLowerCase() ?? ""
    const result = await analyzeCode(content, ext, String(args.path))
    return truncate(result, "analyze", ctx)
  },
}
