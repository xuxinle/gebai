/**
 * 文件工作台深层链接解析（`?root=&project=&path=&line=&session=`）——纯函数、可注入参数、无 DOM 依赖。
 *
 * 消息流里的产物链接只带 `?path=`（会话相对或项目绝对路径），**所属根由本模块定位**：
 * 主界面不复制根清单与匹配规则，解析逻辑只此一份（工作台页面在启动时调用）。
 *
 * 三档优先级：
 *   1. 显式 `?root=` / `?project=` → 直接用该根；
 *   2. `?path=` 为绝对路径 → 根清单中**最长前缀匹配**的根（项目文件命中项目根、会话 tmp 命中会话根）；
 *   3. 其余（相对路径或无 path）→ `?session=` 指向的会话根；
 * 无参数时默认项目根（手工打开 `/files` 看代码），无项目则退回第一个根。
 */

/** 根清单条目中最小的字段面（服务端 `/api/v1/roots` 的子集；便于测试注入）。 */
export interface DeepLinkRoot {
  id: string
  kind: string
  path: string
}

export interface DeepLinkTarget {
  rootId: string
  /** 文件所在目录（根内相对，供资源管理器展开定位；无文件时为 ""） */
  dir: string
  /** 根内相对文件路径（无 path 参数时为空串） */
  file: string
  /** 1 起始行号 */
  line?: number
}

export interface DeepLinkOptions {
  /** Windows 客户端：盘符/路径大小写不敏感（前缀匹配按小写比较）；默认 false（POSIX 敏感）。 */
  isWin?: boolean
}

/** 绝对路径判定（POSIX 根、Windows 盘符、UNC）。 */
export function isAbsPath(p: string): boolean {
  return p.startsWith("/") || /^[a-zA-Z]:[\\/]/.test(p) || p.startsWith("\\\\")
}

function normAbs(p: string, isWin: boolean): string {
  const s = p.replace(/\\/g, "/").replace(/\/+$/, "")
  return isWin ? s.toLowerCase() : s
}

function dirOf(file: string): string {
  const i = file.lastIndexOf("/")
  return i > 0 ? file.slice(0, i) : ""
}

/**
 * 同长前缀时的根优先级：项目类根（服务端注册/绑定） > 会话工作区 > 用户目录 > 本地任意目录。
 * 本地模式下 `abs:` 白名单根会与注册项目**指向同一目录**（如服务工作目录 = 项目根），
 * 两者前缀长度相同——按类型定优先级，避开「明明是项目文件却落到临时目录根」的观感偏差。
 */
const KIND_RANK: Record<string, number> = { proj: 0, bind: 0, sess: 1, user: 2, abs: 3 }
const rankOf = (k: string): number => KIND_RANK[k] ?? 9

/**
 * 解析分层链接。`search` 为 location.search（含或不含 `?` 均可），测试可直接传入。
 * 返回 null 表示根清单为空（无可用根）。
 */
export function resolveDeepLink(roots: DeepLinkRoot[], search: string, opts: DeepLinkOptions = {}): DeepLinkTarget | null {
  const isWin = opts.isWin === true
  const params = new URLSearchParams(search.startsWith("?") ? search.slice(1) : search)
  const project = params.get("project") ?? ""
  const wantRoot = params.get("root") || (project ? `proj:${project}` : "")
  // 会话根指向 tmp 本身，产物路径常带 tmp/ 前缀（服务端解析后的逻辑路径）——剥掉以免多一层
  const wantPath = (params.get("path") ?? "").replace(/^\.\//, "").replace(/^tmp\//, "")
  const line = Number(params.get("line")) || undefined
  const sessId = params.get("session") ?? ""
  const byId = (id: string): DeepLinkRoot | undefined => roots.find((r) => r.id === id)
  const pickDefault = (): DeepLinkRoot | undefined =>
    // 显式指定会话时以该会话工作区为先（入口按钮语义：看本会话 Agent 产物）；否则项目类根优先
    (sessId ? byId(`sess:${sessId}`) : undefined) ??
    roots.find((r) => r.kind === "proj") ??
    roots.find((r) => r.kind === "bind") ??
    roots[0]

  // 1) 显式根
  if (wantRoot) {
    const r = byId(wantRoot)
    if (r) return { rootId: r.id, dir: dirOf(wantPath), file: wantPath, line }
  }
  // 2) 绝对路径 → 最长前缀匹配（多根重叠时取最精确的那个）
  if (wantPath && isAbsPath(wantPath)) {
    const target = normAbs(wantPath, isWin)
    let best: { root: DeepLinkRoot; rel: string; len: number } | null = null
    for (const r of roots) {
      const base = normAbs(r.path, isWin)
      if (!base) continue
      if (target === base || target.startsWith(`${base}/`)) {
        const better =
          !best || base.length > best.len || (base.length === best.len && rankOf(r.kind) < rankOf(best.root.kind))
        if (better) {
          // 根内相对路径统一为 POSIX 分隔符（工作台内部路径一律 `/`，Windows 客户端回传的反斜杠在此归一）
          const rel = wantPath
            .slice(r.path.replace(/[\\/]+$/, "").length)
            .replace(/^[\\/]+/, "")
            .replace(/\\/g, "/")
          best = { root: r, rel, len: base.length }
        }
      }
    }
    // 命中根：绝对路径换算为根内相对路径（越出前缀时 rel 兜底为文件名）
    if (best) return { rootId: best.root.id, dir: dirOf(best.rel), file: best.rel || wantPath.split(/[\\/]/).pop() || "", line }
  }
  // 3) 会话根（相对路径或仅指定 session）
  const fallback = pickDefault()
  if (!fallback) return null
  return { rootId: fallback.id, dir: dirOf(wantPath), file: wantPath, line }
}
