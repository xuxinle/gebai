import { createHash, createHmac } from "node:crypto"
import { lstatSync, realpathSync, readdirSync } from "node:fs"
import { readdir } from "node:fs/promises"
import { join, relative, isAbsolute, resolve, sep, dirname } from "node:path"

/**
 * Sharding helpers per DESIGN.md.
 * Base 256 per layer (16x16). Paths are built from hex hash prefixes.
 * 会话/反馈等 hex ID 的分片段直接取 ID 自身前缀（肉眼可从 ID 推目录）；
 * 本函数供非 hex 的存储键做哈希分片。
 */
export function sha256Hex(input: string): string {
  return createHash("sha256").update(input).digest("hex")
}

export function hmacHex(key: string, input: string): string {
  return createHmac("sha256", key).update(input).digest("hex")
}

export function shardPath(key: string, layers = 2): string[] {
  const hex = sha256Hex(key)
  const parts: string[] = []
  for (let i = 0; i < layers; i++) parts.push(hex.slice(i * 2, i * 2 + 2))
  return parts
}

/** ID 自身 hex 前缀分片段（每层 2 位 = 256 路）：ID 已过格式白名单（32 位小写 hex，随机均匀分布），
 *  分片段与 ID 前缀一致——从会话/反馈 ID 可直接目视定位目录，无需计算哈希。 */
function idPrefixShards(id: string): string[] {
  return [id.slice(0, 2), id.slice(2, 4)]
}

/** 会话 ID 格式白名单：32 位小写 hex（randomUUID 去连字符）。任何外部输入的 id 必须先过此校验，
 * 防止 `a/../../../` 等路径穿越串经 sessionPath 拼出 GEBAI_HOME 外/他人用户目录（多用户隔离防线）。 */
export function isValidSessionId(id: string): boolean {
  return /^[0-9a-f]{32}$/.test(id)
}

export function sessionPath(home: string, user: string, sessionId: string): string {
  if (!isValidSessionId(sessionId)) throw new Error(`invalid session id: ${sessionId}`)
  const [s0, s1] = idPrefixShards(sessionId)
  return join(home, "users", user, "sessions", s0, s1, sessionId)
}

/** 递归遍历目录（按深度限制），对每个文件调用 onFile。目录遍历统一入口（store/feedback 共用）。 */
export async function walkDir(dir: string, depth: number, onFile: (p: string) => Promise<void>): Promise<void> {
  if (depth < 0) return
  let entries
  try {
    entries = await readdir(dir, { withFileTypes: true })
  } catch {
    return
  }
  for (const e of entries) {
    const full = join(dir, e.name)
    if (e.isDirectory()) await walkDir(full, depth - 1, onFile)
    else await onFile(full)
  }
}

/** 剥离逻辑路径首段 `tmp/` 前缀（统一以会话 tmp/ 为路径基准后，兼容带前缀引用——列表/UI/附件契约路径
 *  如 `tmp/a.txt`；仅相对路径首段生效，绝对路径原样返回）。 */
export function stripTmpPrefix(p: string): string {
  if (p.startsWith("tmp/")) return p.slice(4)
  if (p.startsWith("tmp\\")) return p.slice(4).replace(/\\/g, "/")
  return p
}

/** 截断文件逻辑路径（相对**会话工作目录**，模型/前端感知的逻辑路径，如 `truncated/read_xxx.txt`；固定正斜杠跨平台）。
 *  与 sh/py/js 的 cwd 同一基准：同一字符串在文件工具与脚本里都能直接用（带 `tmp/` 前缀会被脚本当子目录多套一层）。 */
export function truncatedLogicalPath(toolName: string, content: string): string {
  const hash = sha256Hex(content)
  return `truncated/${toolName}_${hash}.txt`
}

export function truncatedPath(home: string, user: string, sessionId: string, toolName: string, content: string): string {
  return join(sessionPath(home, user, sessionId), "tmp", truncatedLogicalPath(toolName, content))
}

export function feedbackPath(home: string, user: string, feedbackId: string): string {
  // 分片段直接取反馈 ID 自身前缀（ID 为服务端生成的 32 位小写 hex）；格式白名单防路径穿越
  // （哈希分片时代由哈希保证安全，前缀分片必须显式校验）
  if (!isValidSessionId(feedbackId)) throw new Error(`invalid feedback id: ${feedbackId}`)
  const date = new Date().toISOString().slice(0, 10)
  const [h0, h1] = idPrefixShards(feedbackId)
  return join(home, "users", user, "feedback", date, h0, h1, `${feedbackId}.json`)
}

/**
 * 附件/上传文件名消毒：仅保留 basename，拒绝 `..`、路径分隔符、控制字符与空名。
 * 用于防止上传路径穿越（../ 逃逸会话目录）。
 */
export function basenameName(name: string): string {
  const base = String(name ?? "")
    .replace(/\\/g, "/")
    .split("/")
    .pop() ?? ""
  if (!base || base === "." || base === ".." || /[\x00-\x1f]/.test(base)) return ""
  return base
}

/**
 * 上传附件落盘不覆盖：目标名已被同批占用或磁盘已存在同名文件时追加序号（a.png → a-2.png）
 * 直到可用。多张同名图（如复制粘贴的 image.png）各自落独立文件，不再互相覆盖只剩最后一张。
 * dir 为落盘目录（不存在视为空）；taken 为同批已用名（上传/落盘时目标文件尚未写入，批内去重 + 磁盘去重双保险）。
 */
export function uniqueUploadName(dir: string, name: string, taken: ReadonlySet<string> = new Set()): string {
  const used = new Set(taken)
  try {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      if (e.isFile()) used.add(e.name)
    }
  } catch {
    /* 目录不存在：首批上传，无磁盘占用 */
  }
  if (!used.has(name)) return name
  const dot = name.lastIndexOf(".")
  const stem = dot > 0 ? name.slice(0, dot) : name
  const ext = dot > 0 ? name.slice(dot) : ""
  let i = 2
  while (used.has(`${stem}-${i}${ext}`)) i++
  return `${stem}-${i}${ext}`
}

/**
 * Resolve a user-supplied path against a sandbox root.
 * Rejects `..`, absolute paths, and symlinks per DESIGN.md path sandbox.
 * Returns the safe absolute path.
 */
export function resolveInSandbox(root: string, input: string): string {
  const normalized = input.replace(/\\/g, "/")
  const segments = normalized.split("/").filter((s) => s && s !== ".")
  if (segments.some((s) => s === "..")) {
    throw new Error(`path traversal not allowed: ${input}`)
  }
  if (isAbsolute(normalized)) {
    throw new Error(`absolute path not allowed: ${input}`)
  }
  const abs = resolve(root, ...segments)
  const rel = relative(resolve(root), abs)
  if (rel.startsWith("..") || isAbsolute(rel)) {
    throw new Error(`path outside sandbox: ${input}`)
  }
  if (rel.split(sep).includes("..")) {
    throw new Error(`path traversal not allowed: ${input}`)
  }
  assertNoSymlinkEscape(resolve(root), abs, input)
  return abs
}

/** 符号链接逃逸检查：abs 及其祖先中任何指向沙箱外的符号链接都拒绝（DESIGN 路径沙箱）。
 *  导出复用：files/preview 接口的绝对路径边界检查（用户数据目录内）与 resolveInSandbox 同规则。 */
export function assertNoSymlinkEscape(root: string, abs: string, input: string): void {
  let cur = abs
  const seen = new Set<string>()
  while (cur !== root && !seen.has(cur)) {
    seen.add(cur)
    let st
    try {
      st = lstatSync(cur)
    } catch {
      // 当前路径不存在（新建目标）：继续向上检查祖先（可能存在指向外部的符号链接目录）
      const parent = dirname(cur)
      if (parent === cur || parent === root) break
      cur = parent
      continue
    }
    if (st.isSymbolicLink()) {
      const real = realpathSync(cur)
      const rel = relative(root, real)
      if (rel.startsWith("..") || isAbsolute(rel)) {
        throw new Error(`symlink outside sandbox: ${input}`)
      }
    }
    const parent = dirname(cur)
    if (parent === cur) break
    cur = parent
  }
}
