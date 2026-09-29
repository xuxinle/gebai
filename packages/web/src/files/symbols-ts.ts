/**
 * 文件工作台 · **tree-sitter（wasm）符号提取**：真语法树解析，输出与 `symbols-core.ts` 同形的 `Sym` 树。
 *
 * 分工（详见 `symbols-ts-rules.ts`）：有语法文件的语言走这里，其余语言走词法规则；这里拿不到结果
 * （运行时缺失、语法文件缺失、解析异常）一律返回 null，由调用方回退词法——**任何一条失败路径都不
 * 让符号功能整体消失**。
 *
 * 资产：运行时 JS 与核心 wasm 由 `build-vendor.ts` 静态伺服（`/vendor/tree-sitter/`，稳定文件名，
 * 与 monaco/xterm/d2 同一惯例）；**语言语法 wasm 由服务端按需回源**（`/vendor/tree-sitter/lang/…`，
 * 字节取自服务端已内嵌的语法集，不往 web 产物里再存一份 25MB）。
 *
 * 加载是**懒且按语言缓存**的：打开某个语言的文件时才拉那份语法（首次一次性成本，之后常驻），
 * 解析结果由上层按 model 版本号缓存（见 `symbols.ts`），不在每次按键时重解析。
 */
import { wbUrl, wbAbsUrl } from "./url-base"
import type { Sym, SymKind } from "./symbols-core"
import { CLASS_LIKE_KINDS, CONTAINER_KINDS, CTOR_NAMES, TS_LANGUAGES, type TsNode, type TsNodeRule } from "./symbols-ts-rules"

/** web-tree-sitter 的导出面（只用到这几个）。 */
export interface TsRuntime {
  Parser: { init(opts?: unknown): Promise<void>; new (): TsParserLike }
  Language: { load(input: Uint8Array | string): Promise<unknown> }
}

interface TsParserLike {
  setLanguage(language: unknown): void
  parse(text: string): { rootNode: TsNode }
}

/** 语法字节加载器（浏览器=HTTP；测试=磁盘）。返回 null 表示该语法不可用。 */
export type GrammarLoader = (file: string) => Promise<Uint8Array | null>

let runtimePromise: Promise<TsRuntime | null> | null = null
let grammarLoaderOverride: GrammarLoader | null = null
/** 测试注入的运行时（见 `setTsRuntimeForTest`）；为 null 时走 vendor 加载。 */
let runtimeOverride: TsRuntime | null = null
const parsers = new Map<string, Promise<TsParserLike | null>>()

/**
 * 浏览器侧默认语法加载：从服务端静态路径取（`Cache-Control` 允许长期缓存，重复打开只付一次）。
 *
 * 两类失败分开处置（与 LSP 清单拉取同一口径）：**404/403 = 该语法确定拿不到**（回 null，调用方记住
 * 别再反复请求）；**其余 HTTP 错误与网络异常**则抛出去（调用方不缓存结果，下次打开文件可重试）。
 */
async function fetchGrammar(file: string): Promise<Uint8Array | null> {
  const res = await fetch(wbUrl(`/vendor/tree-sitter/lang/${file}`))
  if (res.status === 404 || res.status === 403) return null
  if (!res.ok) throw new Error(`语法文件请求失败（HTTP ${res.status}）`)
  return new Uint8Array(await res.arrayBuffer())
}

/** 注入语法加载器（测试用；传 null 恢复默认）。 */
export function setGrammarLoader(loader: GrammarLoader | null): void {
  grammarLoaderOverride = loader
  parsers.clear() // 换了来源，已建的 parser 不再可信
}

/** 该语言是否走语法树（有映射表即算；实际能否解析取决于语法文件是否取到）。 */
export function hasTsSupport(language: string): boolean {
  return Object.prototype.hasOwnProperty.call(TS_LANGUAGES, language)
}

/**
 * 注入运行时（测试用；传 null 恢复默认的 vendor 加载）。
 *
 * 浏览器里运行时是从 vendor 静态目录加载的模块（惰性、失败可重试）；单测里不能走
 * 网络/静态资源，改为注入 `web-tree-sitter` 包本体，**与浏览器同一条代码路径**（同样的 Parser/Language
 * 接口、同样的语法加载器）。
 */
export function setTsRuntimeForTest(runtime: TsRuntime | null): void {
  runtimeOverride = runtime
  runtimePromise = null
  parsers.clear()
}

/** 加载运行时（浏览器：vendor 目录里的 ESM；失败返回 null → 调用方回退词法）。 */
export function loadTsRuntime(): Promise<TsRuntime | null> {
  if (runtimeOverride) return Promise.resolve(runtimeOverride)
  if (runtimePromise) return runtimePromise
  const url = wbAbsUrl("/vendor/tree-sitter/tree-sitter.js")
  runtimePromise = import(/* @vite-ignore */ url)
    .then(async (mod) => {
      const rt = mod as unknown as TsRuntime
      await rt.Parser.init({ locateFile: () => wbAbsUrl("/vendor/tree-sitter/tree-sitter.wasm") })
      return rt
    })
    .catch(() => {
      runtimePromise = null // 失败不固化（下次可重试，与 Monaco 加载同一处置）
      return null
    })
  return runtimePromise
}

/** 取（并按语言缓存）指定语言的 parser；不可用返回 null。 */
export function parserFor(runtime: TsRuntime, language: string): Promise<TsParserLike | null> {
  const spec = TS_LANGUAGES[language]
  if (!spec) return Promise.resolve(null)
  const hit = parsers.get(language)
  if (hit) return hit
  const loader = grammarLoaderOverride ?? fetchGrammar
  // `.then(() => loader(...))` 而非直接调用：把 loader 推到微任务，`job` 在此前已赋值（catch 里要按它清缓存）
  const job = Promise.resolve()
    .then(() => loader(spec.grammar))
    .then(async (bytes): Promise<TsParserLike | null> => {
      if (!bytes) return null // 确定拿不到（404 / 测试注入的 null）：缓存空结果，不再反复请求
      try {
        const lang = await runtime.Language.load(bytes)
        const parser = new runtime.Parser()
        parser.setLanguage(lang)
        return parser
      } catch {
        return null // 语法字节与运行时版本不匹配等：同样记住，不反复重试
      }
    })
    .catch((): TsParserLike | null => {
      // 网络波动 / 超时：本次当无语法树（回退词法），但**不固化**——下次打开文件重试
      if (parsers.get(language) === job) parsers.delete(language)
      return null
    })
  parsers.set(language, job)
  return job
}

/* --------------------------- 语法树 → 符号 --------------------------- */

interface Ctx {
  containers: { name: string; kind: string }[]
  /** 是否处在函数体内（`notInFunction` 规则据此生效与否） */
  inFunction: boolean
}

/** 名字取法：规则自带 → 否则取 `name` 字段 → 再取 `type` 字段（Rust impl 这类）。 */
function nodeName(node: TsNode, rule: TsNodeRule): string | undefined {
  const raw = rule.name ? rule.name(node) : (node.childForFieldName("name")?.text ?? node.childForFieldName("type")?.text)
  return raw?.trim() || undefined
}

/**
 * 名字在首行里的列号：语法节点从声明起点（修饰符/关键字）开始，而跳转应落在**名字**上
 * （光标定位与词法路径同口径）。名字不在首行（换行签名等）时退回节点起始列。
 */
function nameColumn(node: TsNode, name: string): number {
  const firstLine = node.text.split("\n")[0] ?? ""
  const at = firstLine.indexOf(name)
  return at < 0 ? node.startPosition.column : node.startPosition.column + at
}

/** 种类修正：类性质容器内的函数 → 方法；与容器同名（或惯用构造器名）→ 构造器。 */
function refineKind(kind: SymKind, name: string, ctx: Ctx): SymKind {
  if (kind !== "function" && kind !== "method") return kind
  const enclosing = [...ctx.containers].reverse().find((c) => CLASS_LIKE_KINDS.has(c.kind))
  if (!enclosing) return kind
  return enclosing.name === name || CTOR_NAMES.has(name.toLowerCase()) ? "constructor" : "method"
}

/**
 * 用语法树提取符号（`runtime` 由调用方给出，便于测试注入）。
 * 语言无映射 / parser 不可用 / 解析抛错时返回 null（调用方回退词法）。
 */
export async function extractWithRuntime(runtime: TsRuntime, text: string, language: string): Promise<Sym[] | null> {
  const spec = TS_LANGUAGES[language]
  if (!spec) return null
  const parser = await parserFor(runtime, language)
  if (!parser) return null
  let root: TsNode
  try {
    root = parser.parse(text).rootNode
  } catch {
    return null
  }
  const out: Sym[] = []
  const visit = (node: TsNode, ctx: Ctx, sink: Sym[]): void => {
    const rule = spec.nodes[node.type]
    if (rule && !(rule.notInFunction && ctx.inFunction)) {
      const kind = (typeof rule.kind === "function" ? rule.kind(node) : rule.kind) as SymKind | ""
      const name = nodeName(node, rule)
      if (kind && name) {
        const finalKind = refineKind(kind, name, ctx)
        const sym: Sym = {
          name,
          kind: finalKind,
          line: node.startPosition.row,
          column: nameColumn(node, name),
          endLine: node.endPosition.row,
          children: [],
        }
        sink.push(sym)
        // 容器：子级归到它名下；进入函数体后 `notInFunction` 规则不再生效
        const nextKinds = new Set(["function", "method", "constructor"])
        const childCtx: Ctx = {
          containers: CONTAINER_KINDS.has(kind) ? [...ctx.containers, { name, kind }] : ctx.containers,
          inFunction: ctx.inFunction || nextKinds.has(finalKind),
        }
        for (const child of node.namedChildren) visit(child, childCtx, sym.children)
        return
      }
    }
    for (const child of node.namedChildren) visit(child, ctx, sink)
  }
  for (const child of root.namedChildren) visit(child, { containers: [], inFunction: false }, out)
  return out
}

/** 高层入口：懒加载运行时并按语言提取；拿不到结果返回 null。 */
export async function extractWithTreeSitter(text: string, language: string): Promise<Sym[] | null> {
  if (!hasTsSupport(language)) return null
  const runtime = await loadTsRuntime()
  if (!runtime) return null
  return extractWithRuntime(runtime, text, language)
}
