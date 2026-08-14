import { createHash, createHmac } from "node:crypto"
import { lstatSync, realpathSync } from "node:fs"
import { readdir } from "node:fs/promises"
import { join, relative, isAbsolute, resolve, sep, dirname } from "node:path"

/**
 * Sharding helpers per DESIGN.md.
 * Base 256 per layer (16x16). Paths are built from hex hash prefixes.
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

/** 会话 ID 格式白名单：32 位小写 hex（randomUUID 去连字符）。任何外部输入的 id 必须先过此校验，
 * 防止 `a/../../../` 等路径穿越串经 sessionPath 拼出 GEBAI_HOME 外/他人用户目录（多用户隔离防线）。 */
export function isValidSessionId(id: string): boolean {
  return /^[0-9a-f]{32}$/.test(id)
}

export function sessionPath(home: string, user: string, sessionId: string): string {
  if (!isValidSessionId(sessionId)) throw new Error(`invalid session id: ${sessionId}`)
  const [s0, s1] = shardPath(sessionId, 2)
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

/** 截断文件逻辑路径（相对会话根，模型/前端感知的逻辑路径，如 `tmp/truncated/read_xxx.txt`；固定正斜杠跨平台）。 */
export function truncatedLogicalPath(toolName: string, content: string): string {
  const hash = sha256Hex(content)
  return `tmp/truncated/${toolName}_${hash}.txt`
}

export function truncatedPath(home: string, user: string, sessionId: string, toolName: string, content: string): string {
  return join(sessionPath(home, user, sessionId), truncatedLogicalPath(toolName, content))
}

export function feedbackPath(home: string, user: string, feedbackId: string): string {
  const hash = sha256Hex(feedbackId)
  const date = new Date().toISOString().slice(0, 10)
  const [h0, h1] = shardPath(hash, 2)
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

/** 符号链接逃逸检查：abs 及其祖先中任何指向沙箱外的符号链接都拒绝（DESIGN 路径沙箱）。 */
function assertNoSymlinkEscape(root: string, abs: string, input: string): void {
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
