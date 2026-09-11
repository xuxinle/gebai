/** 文件工作台 Root 抽象（DESIGN「文件工作台」）：所有 fs/git 接口以 `(root, path)` 寻址——
 *  `root` 为命名空间化标识，`path` 恒为相对 root 的 POSIX 风格相对路径。收益：
 *  1) 多用户隔离单点收敛（会话 tmp / 预置项目 / 用户目录 / 绝对路径四类来源同一处校验）；
 *  2) 前端目录树天然多根，同一套 UI 承载会话产物与项目源码；
 *  3) 越界、符号链接逃逸、Windows 保留名 / 盘符 / UNC 等平台细节集中处置，路由层不重复实现。
 *
 *  root 形态：
 *  | `sess:<sessionId>` | 会话工作区（users/{u}/sessions/.../tmp）——仅本人会话（id 白名单 + 路径自证归属） |
 *  | `proj:<name>`      | 预置项目（{AGENT}_PROJECTS 注册表，与模型 project 参数同一份真相） |
 *  | `bind:<agent>`     | 会话绑定项目根（{AGENT}_PROJECT） |
 *  | `user:`            | 当前用户数据目录（users/{u}/） |
 *  | `abs:<绝对路径>`   | 任意绝对路径根（仅本地模式；服务模式一律 403） |
 */
import { existsSync, statSync } from "node:fs"
import { homedir } from "node:os"
import { isAbsolute, join, relative, resolve } from "node:path"
import { assertNoSymlinkEscape, isValidSessionId, sessionPath } from "../base/paths"

export type RootKind = "sess" | "proj" | "bind" | "user" | "abs"

/** 统一错误类型（路由层映射 HTTP 状态码；消息面向用户可读）。 */
export class FsError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message)
    this.name = "FsError"
  }
}

export const fsBadRequest = (m: string) => new FsError(400, m)
export const fsForbidden = (m: string) => new FsError(403, m)
export const fsNotFound = (m: string) => new FsError(404, m)
export const fsConflict = (m: string) => new FsError(409, m)
export const fsTooLarge = (m: string) => new FsError(413, m)
export const fsUnprocessable = (m: string) => new FsError(422, m)

/** 根条目（根清单返回给前端；`path` 为绝对路径——本地模式本无秘密，服务模式限本人数据目录内）。 */
export interface FileRoot {
  id: string
  kind: RootKind
  name: string
  path: string
  description?: string
  writable: boolean
  sessionId?: string
}

export interface ResolvedRoot {
  id: string
  kind: RootKind
  /** root 绝对路径（已规范化） */
  abs: string
  writable: boolean
}

/** 根解析上下文（路由层按请求装配：身份 / 沙箱 / 项目注册表 / 开关）。 */
export interface RootContext {
  /** GEBAI_HOME */
  home: string
  user: string
  /** 沙箱约束（服务模式非豁免用户）：绝对路径根拒绝、项目根须落在用户数据目录内 */
  sandboxed: boolean
  /** 全局写开关（GEBAI_FS_WRITE=false → 所有根只读） */
  writable: boolean
  /** 预置项目（与模型 project 参数同源；由引擎提供） */
  projects: Array<{ name: string; path: string; description?: string }>
  /** 会话绑定项目根（{AGENT}_PROJECT） */
  binds: Array<{ agent: string; root: string }>
  /** 额外白名单根（GEBAI_FS_ROOTS / 本地模式盘符与常用目录） */
  extraRoots: FileRoot[]
  /** 会话清单（构建 `sess:` 根） */
  sessions?: Array<{ id: string; name: string }>
  /** 目录存在性判定（测试注入用；默认查真实文件系统） */
  isDir?: (p: string) => boolean
}

const SEP = "/"

/** 是否为 Windows 平台（路径大小写不敏感 + 保留名约束；测试可通过 GEBAI_FS_FORCE_WIN 强制）。 */
export function isWindowsFs(): boolean {
  if (process.env.GEBAI_FS_FORCE_WIN === "1") return true
  if (process.env.GEBAI_FS_FORCE_POSIX === "1") return false
  return process.platform === "win32"
}

/** Windows 设备保留名（含带扩展名形态：`CON.txt` 同样非法）。 */
const WIN_RESERVED = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$/i

/** 路径段安全校验：拒绝空段/`.`/`..`/控制字符/Windows 保留名与非法尾字符。 */
export function assertSafeSegment(seg: string): void {
  if (!seg || seg === "." || seg === "..") throw fsBadRequest(`非法路径段: ${JSON.stringify(seg)}`)
  if (/[\x00-\x1f]/.test(seg)) throw fsBadRequest("路径含控制字符")
  if (seg.length > 255) throw fsBadRequest("路径段过长")
  if (isWindowsFs()) {
    if (/[<>:"|?*]/.test(seg)) throw fsBadRequest(`Windows 路径段含非法字符: ${seg}`)
    if (/[. ]$/.test(seg)) throw fsBadRequest(`Windows 路径段不可以点或空格结尾: ${seg}`)
    if (WIN_RESERVED.test(seg)) throw fsBadRequest(`Windows 保留名不可用: ${seg}`)
  }
}

/** 归一化相对路径：反斜杠转正斜杠、压掉重复分隔与 `.` 段；`..` 原样保留（由边界检查拒绝）。 */
export function normalizeRel(input: string): string {
  const raw = String(input ?? "").replace(/\\/g, SEP)
  const out: string[] = []
  for (const seg of raw.split(SEP)) {
    if (!seg || seg === ".") continue
    out.push(seg)
  }
  return out.join(SEP)
}

/** 相对路径的段清单（供逐段校验）。 */
export function relSegments(input: string): string[] {
  return normalizeRel(input)
    .split(SEP)
    .filter(Boolean)
}

/** abs 是否落在 root 之内（Windows 大小写不敏感）。 */
export function isInside(root: string, abs: string): boolean {
  const r = isWindowsFs() ? resolve(root).toLowerCase() : resolve(root)
  const a = isWindowsFs() ? resolve(abs).toLowerCase() : resolve(abs)
  if (a === r) return true
  const rel = relative(r, a)
  return !!rel && !rel.startsWith("..") && !isAbsolute(rel)
}

function dirExists(p: string): boolean {
  try {
    return statSync(p).isDirectory()
  } catch {
    return false
  }
}

/**
 * 把 root 内的相对路径解析为安全绝对路径（**所有 fs 操作必经**）：
 * 归一化 → 逐段校验（穿越/保留名）→ 拼接 → 边界判定 → 符号链接逃逸检查。
 * `allowAbsolute`：`abs:` 根允许直接给绝对路径（仍须落在该根内）。
 */
export function resolveInRoot(rootAbs: string, rel: string, opts: { allowAbsolute?: boolean } = {}): string {
  const input = String(rel ?? "")
  if (isAbsolute(input.replace(/\\/g, SEP))) {
    if (!opts.allowAbsolute) throw fsBadRequest(`不允许绝对路径: ${input}`)
    const abs = resolve(input)
    if (!isInside(rootAbs, abs)) throw fsForbidden(`路径越界: ${input}`)
    assertNoSymlinkEscape(resolve(rootAbs), abs, input)
    return abs
  }
  const segs = relSegments(input)
  if (segs.some((s) => s === "..")) throw fsBadRequest(`路径穿越不允许: ${input}`)
  for (const s of segs) assertSafeSegment(s)
  const abs = resolve(rootAbs, ...segs)
  // 防御：极端形态（如 Windows 上 `C:` 段）经 resolve 后可能跳出 root
  if (!isInside(rootAbs, abs)) throw fsForbidden(`路径越界: ${input}`)
  assertNoSymlinkEscape(resolve(rootAbs), abs, input)
  return abs
}

/** 解析 root 标识（返回 kind 与 id；格式非法抛 400）。 */
export function parseRootId(rootId: string): { kind: RootKind; id: string } {
  const raw = String(rootId ?? "")
  const i = raw.indexOf(":")
  const kind = (i < 0 ? "" : raw.slice(0, i)) as RootKind
  const id = i < 0 ? "" : raw.slice(i + 1)
  if (!["sess", "proj", "bind", "user", "abs"].includes(kind)) {
    throw fsBadRequest(`未知根类型: ${raw}（形如 sess:<id> / proj:<name> / bind:<agent> / user: / abs:<path>）`)
  }
  if (kind === "sess" && !isValidSessionId(id)) throw fsBadRequest(`非法会话 id: ${id}`)
  if (kind !== "user" && kind !== "sess" && kind !== "abs" && !id.trim()) throw fsBadRequest(`根缺少标识: ${raw}`)
  if (kind === "abs" && !id.trim()) throw fsBadRequest("abs: 根缺少路径")
  return { kind, id }
}

/** 会话工作区绝对路径（会话 tmp/）。 */
export function sessionTmpRoot(ctx: RootContext, sessionId: string): string {
  if (!isValidSessionId(sessionId)) throw fsBadRequest(`非法会话 id: ${sessionId}`)
  return join(sessionPath(ctx.home, ctx.user, sessionId), "tmp")
}

/** 解析 root 为绝对路径（含写权限判定）。 */
export function resolveRoot(rootId: string, ctx: RootContext): ResolvedRoot {
  const { kind, id } = parseRootId(rootId)
  const exists = ctx.isDir ?? dirExists
  if (kind === "sess") {
    return { id: rootId, kind, abs: sessionTmpRoot(ctx, id), writable: ctx.writable }
  }
  if (kind === "user") {
    return { id: rootId, kind, abs: join(ctx.home, "users", ctx.user), writable: ctx.writable }
  }
  if (kind === "proj") {
    const p = ctx.projects.find((x) => x.name === id)
    if (!p) throw new FsError(404, `未知预置项目: ${id}`)
    if (!exists(p.path)) throw new FsError(404, `项目目录不存在: ${id}（${p.path}）`)
    return { id: rootId, kind, abs: resolve(p.path), writable: ctx.writable }
  }
  if (kind === "bind") {
    const b = ctx.binds.find((x) => x.agent === id)
    if (!b) throw new FsError(404, `未绑定项目: ${id}`)
    if (!exists(b.root)) throw new FsError(404, `绑定项目目录不存在: ${id}（${b.root}）`)
    return { id: rootId, kind, abs: resolve(b.root), writable: ctx.writable }
  }
  // abs: 绝对路径根——服务模式（沙箱启用）一律拒绝；本地模式放行
  if (ctx.sandboxed) throw fsForbidden("沙箱模式下不允许绝对路径根（请使用 sess: / proj: / bind: / user: 根）")
  const abs = resolve(id)
  if (!exists(abs)) throw new FsError(404, `目录不存在: ${abs}`)
  // 显式白名单根（GEBAI_FS_ROOTS）在服务模式下的可写性由条目自身决定，本地模式恒可写
  const extra = ctx.extraRoots.find((r) => r.id === rootId)
  return { id: rootId, kind, abs, writable: extra ? extra.writable && ctx.writable : ctx.writable }
}

/** 本地模式的额外根：主目录 / 服务工作目录 / Windows 盘符。 */
export function localExtraRoots(): FileRoot[] {
  const out: FileRoot[] = []
  const home = homedir()
  if (home) out.push({ id: `abs:${home}`, kind: "abs", name: "主目录", path: home, writable: true })
  const cwd = process.cwd()
  if (cwd && resolve(cwd) !== resolve(home)) out.push({ id: `abs:${cwd}`, kind: "abs", name: "服务工作目录", path: cwd, writable: true })
  if (isWindowsFs()) {
    for (const letter of "CDEFGHIJKLMNOPQRSTUVWXYZ") {
      const drive = `${letter}:\\`
      try {
        if (existsSync(drive)) out.push({ id: `abs:${drive}`, kind: "abs", name: `${letter} 盘`, path: drive, writable: true })
      } catch {
        /* 无访问权限：跳过 */
      }
    }
  } else if (existsSync("/workspaces")) {
    out.push({ id: "abs:/workspaces", kind: "abs", name: "workspaces", path: "/workspaces", writable: true })
  }
  return out
}

/** 解析 GEBAI_FS_ROOTS 配置（JSON 数组：字符串或 {name,path,description,writable}）。 */
export function parseExtraRoots(raw: string | undefined, sandboxed: boolean): FileRoot[] {
  if (!raw || !raw.trim()) return []
  let list: unknown
  try {
    list = JSON.parse(raw)
  } catch {
    return []
  }
  if (!Array.isArray(list)) return []
  const out: FileRoot[] = []
  for (const item of list) {
    const o = typeof item === "string" ? { path: item } : (item as Record<string, unknown> | null)
    if (!o || typeof o !== "object") continue
    let p = typeof o.path === "string" ? o.path.trim() : ""
    if (!p) continue
    if (!isAbsolute(p)) p = resolve(p)
    // 服务模式：白名单根必须落在用户数据目录内（否则视为配置错误忽略），本地模式放开
    if (sandboxed && !p.replace(/\\/g, "/").includes("/users/")) {
      try {
        statSync(p)
      } catch {
        continue
      }
    }
    const name = typeof o.name === "string" && o.name.trim() ? o.name.trim() : p
    out.push({
      id: `abs:${p}`,
      kind: "abs",
      name,
      path: p,
      description: typeof o.description === "string" ? o.description : undefined,
      writable: o.writable === undefined ? true : !!o.writable,
    })
  }
  return out
}

/**
 * 根清单（前端左栏根选择器）：会话工作区（最近 N 个）+ 预置项目 + 绑定项目 + 用户目录 + 额外根。
 * 同一路径多来源时按 id 去重（预置项目优先展示，避免同名路径重复列）。
 */
export function rootCatalog(ctx: RootContext, opts: { sessionLimit?: number } = {}): FileRoot[] {
  const limit = opts.sessionLimit ?? 20
  const seen = new Set<string>()
  const out: FileRoot[] = []
  const push = (r: FileRoot) => {
    if (seen.has(r.id)) return
    seen.add(r.id)
    out.push(r)
  }
  for (const s of ctx.sessions ?? []) {
    const abs = sessionTmpRoot(ctx, s.id)
    push({ id: `sess:${s.id}`, kind: "sess", name: s.name || s.id.slice(0, 8), path: abs, sessionId: s.id, writable: ctx.writable })
  }
  for (const p of ctx.projects) push({ id: `proj:${p.name}`, kind: "proj", name: p.name, path: p.path, description: p.description, writable: ctx.writable })
  for (const b of ctx.binds) push({ id: `bind:${b.agent}`, kind: "bind", name: `${b.agent} 绑定项目`, path: b.root, writable: ctx.writable })
  push({ id: "user:", kind: "user", name: "用户目录", path: join(ctx.home, "users", ctx.user), writable: ctx.writable })
  for (const r of ctx.extraRoots) push(r)
  return out.slice(0, limit + ctx.projects.length + ctx.binds.length + ctx.extraRoots.length + 1)
}

/** 会话 id 是否属于该用户（root 解析已按用户目录拼接，此处提供显式校验供路由复用）。 */
export function sessionTmpCandidates(home: string, user: string, sessionId: string): string[] {
  return [join(sessionPath(home, user, sessionId), "tmp")]
}

/** 相对 root 的逻辑路径（POSIX 风格；用于前端展示与审计）。 */
export function logicalPath(rootAbs: string, abs: string): string {
  const rel = relative(rootAbs, abs).replace(/\\/g, SEP)
  return rel === "." ? "" : rel
}
