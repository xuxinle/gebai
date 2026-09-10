import { createHash, createHmac } from "node:crypto"

/** sha256 十六进制摘要（分片键等）。 */
export function sha256Hex(input: string): string {
  return createHash("sha256").update(input).digest("hex")
}
/** HMAC-SHA256 十六进制签名。 */
export function hmacHex(key: string, input: string): string {
  return createHmac("sha256", key).update(input).digest("hex")
}
/** 非 hex 键的哈希分片段（每层 2 位 = 256 路）。 */
export function shardPath(key: string, layers = 2): string[] {
  const hex = sha256Hex(key)
  const parts: string[] = []
  for (let i = 0; i < layers; i++) parts.push(hex.slice(i * 2, i * 2 + 2))
  return parts
}
/**
 * 沙箱路径解析（自 server core/base/paths.ts 下沉的子集，sdk 单一来源）：resolveInSandbox 与
 * 符号链接逃逸检查——项目机制（projectAware 沙箱分支）与引擎路径沙箱共用同规则。
 */
import { lstatSync, realpathSync } from "node:fs"
import { isAbsolute, relative, resolve, sep, dirname } from "node:path"

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

import { readdir } from "node:fs/promises"
import { join } from "node:path"

/** 递归遍历目录（按深度限制），对每个文件调用 onFile（自 server base/paths 下沉；反馈扫描等共用）。 */
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
