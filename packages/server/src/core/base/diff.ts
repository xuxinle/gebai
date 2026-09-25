/** 文本行拆分与语法语言推断（read/show/patch 等共用的小工具）。 */

/** 按扩展名推断语法高亮语言（与前端 EXT_LANG 保持一致）。 */
const EXT_LANG: Record<string, string> = {
  ts: "typescript", tsx: "typescript", js: "javascript", jsx: "javascript", mjs: "javascript", cjs: "javascript",
  json: "json", py: "python", pyw: "python", sh: "bash", bash: "bash", zsh: "bash",
  css: "css", scss: "scss", less: "less", html: "xml", htm: "xml", xml: "xml", svg: "xml", vue: "xml", svelte: "xml",
  md: "markdown", markdown: "markdown", yml: "yaml", yaml: "yaml",
  go: "go", rs: "rust", java: "java", kt: "kotlin", kts: "kotlin", rb: "ruby",
  c: "c", h: "c", cpp: "cpp", hpp: "cpp", cc: "cpp", cs: "csharp", php: "php",
  sql: "sql", lua: "lua", swift: "swift", dart: "dart",
}

export function inferLang(name: string): string {
  const ext = name.split(".").pop()?.toLowerCase() ?? ""
  return EXT_LANG[ext] ?? ""
}

/** 语法高亮语言 → 规范化产物扩展名（show `code` 分支落盘命名用；未命中回落 `txt`）。 */
const LANG_EXT: Record<string, string> = {
  typescript: "ts", javascript: "js", json: "json", python: "py", bash: "sh",
  html: "html", xml: "xml", css: "css", scss: "scss", less: "less", yaml: "yml",
  markdown: "md", go: "go", rust: "rs", java: "java", kotlin: "kt", ruby: "rb",
  c: "c", cpp: "cpp", csharp: "cs", php: "php", sql: "sql", lua: "lua", swift: "swift", dart: "dart",
}

export function extForLang(lang: string): string {
  return LANG_EXT[lang.toLowerCase()] ?? "txt"
}

/** 拆分文本为行（忽略末尾换行产生的空行，`"a\n"` 与 `"a"` 等价）。 */
export function splitLines(text: string): string[] {
  if (text === "") return []
  const lines = text.split("\n")
  if (lines.length > 1 && lines[lines.length - 1] === "") lines.pop()
  return lines
}
