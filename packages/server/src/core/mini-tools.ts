import { access, mkdir, readFile, readdir, rm, writeFile } from "node:fs/promises"
import { dirname, join } from "node:path"
import { shardPath } from "./paths"

/* ---------- HTML 小工具库（Agent 保存/加载的 HTML 小工具，支持公用与用户私有） ---------- */

export const MINI_TOOL_MAX_HTML = 200 * 1024
/** 工具名：小写字母/数字/下划线/中文，1-40 字符（用于文件名与 UI 列表展示）。 */
export const MINI_TOOL_NAME_RE = /^[a-zA-Z0-9_\u4e00-\u9fff]{1,40}$/

export type MiniToolScope = "public" | "private"

export interface MiniToolInfo {
  name: string
  html: string
  scope: MiniToolScope
  /** 创建者用户 id（公用工具的归属记录；私有工具存于用户自己的目录，天然归属）。 */
  owner?: string
  createdAt: number
  updatedAt: number
}

export interface MiniToolMeta {
  name: string
  scope: MiniToolScope
  owner?: string
  createdAt: number
  updatedAt: number
}

/** 工具名合法性校验：返回错误文案（合法返回空字符串）。 */
export function validateToolName(name: string): string {
  if (!name) return "工具名不能为空"
  if (!MINI_TOOL_NAME_RE.test(name)) {
    return "工具名仅限字母/数字/下划线/中文，长度 1-40 字符（不含 . / : 等分隔符）"
  }
  return ""
}

function toolShardDirs(name: string): [string, string] {
  const [h0, h1] = shardPath(name, 2)
  return [h0, h1]
}

/** 工具存储路径：公用 `{home}/tools/{h0}/{h1}/{name}.json`；私有 `{home}/users/{user}/tools/{h0}/{h1}/{name}.json`。 */
export function miniToolPath(home: string, user: string, name: string, scope: MiniToolScope): string {
  const [h0, h1] = toolShardDirs(name)
  const base = scope === "public" ? join(home, "tools") : join(home, "users", user, "tools")
  return join(base, h0, h1, `${name}.json`)
}

/** 公共工具写/删权限判定：多用户隔离模式下仅管理员可创建/覆盖/删除公共工具（共享资源防投毒）；
 *  本地模式（auth=local，默认用户即 admin）不限制。返回错误文案（允许返回空）。 */
export function publicToolDenied(auth: { mode: string; role?: string } | undefined): string {
  if (auth && auth.mode === "server" && auth.role !== "admin") {
    return "公共小工具仅管理员可创建/修改/删除（共享资源，防投毒）；普通用户请使用 scope=private 私有工具"
  }
  return ""
}

/** 保存（创建/覆盖）小工具；返回完整信息。名称非法或 HTML 超限抛错。 */
export async function saveMiniTool(
  home: string,
  user: string,
  input: { name: string; html: string; scope: MiniToolScope },
  auth?: { mode: string; role?: string },
): Promise<MiniToolInfo> {
  const err = validateToolName(input.name)
  if (err) throw new Error(err)
  if (input.html.length > MINI_TOOL_MAX_HTML) {
    throw new Error(`HTML 内容超限（${input.html.length} 字符，上限 ${MINI_TOOL_MAX_HTML}）`)
  }
  if (input.scope === "public") {
    const denied = publicToolDenied(auth)
    if (denied) throw new Error(denied)
  }
  const now = Date.now()
  // 覆盖更新时保留原 createdAt（创建时间不变，仅 updatedAt 刷新）
  const existing = await readToolFile(miniToolPath(home, user, input.name, input.scope))
  const info: MiniToolInfo = {
    name: input.name,
    html: input.html,
    scope: input.scope,
    owner: input.scope === "public" ? user : undefined,
    createdAt: existing?.createdAt ?? now,
    updatedAt: now,
  }
  const p = miniToolPath(home, user, input.name, input.scope)
  await mkdir(dirname(p), { recursive: true })
  await writeFile(p, JSON.stringify(info))
  return info
}

async function readToolFile(p: string): Promise<MiniToolInfo | null> {
  try {
    const info = JSON.parse(await readFile(p, "utf8")) as MiniToolInfo
    if (!info || typeof info.name !== "string") return null
    return info
  } catch {
    return null
  }
}

/** 列出对用户可见的小工具（公用全部 + 本人私有）；同名时私有覆盖公用。 */
export async function listMiniTools(home: string, user: string): Promise<MiniToolMeta[]> {
  const privateNames = new Set<string>()
  const out: MiniToolMeta[] = []
  const scan = async (scope: MiniToolScope) => {
    const base = scope === "public" ? join(home, "tools") : join(home, "users", user, "tools")
    const h0s: string[] = []
    try {
      h0s.push(...(await readdir(base)))
    } catch {
      return
    }
    for (const h0 of h0s) {
      const h1s: string[] = []
      try {
        h1s.push(...(await readdir(join(base, h0))))
      } catch {
        continue
      }
      for (const h1 of h1s) {
        const files: string[] = []
        try {
          files.push(...(await readdir(join(base, h0, h1))))
        } catch {
          continue
        }
        for (const f of files) {
          if (!f.endsWith(".json")) continue
          const info = await readToolFile(join(base, h0, h1, f))
          if (!info) continue
          if (scope === "private") {
            privateNames.add(info.name)
            out.push({ name: info.name, scope, createdAt: info.createdAt, updatedAt: info.updatedAt })
          } else if (!privateNames.has(info.name)) {
            // 私有覆盖公用：用户已有同名私有工具时，公用条目不展示
            out.push({ name: info.name, scope, owner: info.owner, createdAt: info.createdAt, updatedAt: info.updatedAt })
          }
        }
      }
    }
  }
  await scan("private")
  await scan("public")
  out.sort((a, b) => a.name.localeCompare(b.name))
  return out
}

/** 读取指定工具（含 html）；解析顺序：用户私有 → 公用。不存在返回 null。 */
export async function getMiniTool(home: string, user: string, name: string): Promise<MiniToolInfo | null> {
  const err = validateToolName(name)
  if (err) return null
  const priv = await readToolFile(miniToolPath(home, user, name, "private"))
  if (priv) return priv
  return readToolFile(miniToolPath(home, user, name, "public"))
}

/** 删除工具：私有仅限本人目录（天然归属校验）；公共删除共享条目（多用户模式仅管理员，见 publicToolDenied）。返回是否删除。 */
export async function deleteMiniTool(
  home: string,
  user: string,
  name: string,
  scope: MiniToolScope,
  auth?: { mode: string; role?: string },
): Promise<boolean> {
  const err = validateToolName(name)
  if (err) return false
  if (scope === "public" && publicToolDenied(auth)) return false
  const p = miniToolPath(home, user, name, scope)
  try {
    await access(p)
    await rm(p, { force: true })
    return true
  } catch {
    return false
  }
}
