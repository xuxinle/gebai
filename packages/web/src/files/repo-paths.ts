/**
 * 文件工作台 · **仓库相对路径 ↔ 根相对路径**的换算（纯函数，可单测）。
 *
 * 为什么单独成模块：工作台里同时存在两套路径坐标，而它们只差一个前缀，错起来**都不报错**：
 * - **Git 侧**（status/diff/log/paths、提交里的文件清单）说**仓库相对**路径（`packages/web/src/a.ts`）；
 * - **文件侧**（fs 端点、编辑器标签、资源管理器）说**根相对**路径（`src/a.ts`，根是 `proj:…`，
 *   可能指向仓库的某个子目录）。
 *
 * 二者在「根 == 仓库根」时恰好重合，子目录根（会话工作区常是项目仓库的子目录）时才分岔：
 * 少了前缀 → 找不到文件（把 `sub/a.txt` 当根内路径用，实际去找 `<根>/sub/a.txt`）；
 * 多了前缀 → 同样找不到（把 `a.txt` 当仓库路径用）。而变更面板还要显示**根之外**的改动
 * （打开「整仓库」范围时）——那条路径在根内根本无法表达，必须换一个覆盖它的根来打开。
 *
 * 所以这里只放「算」的部分（前缀、归属、换算），DOM/请求在调用方；每条规则都有单测。
 */

/** 根清单条目的最小字段面（服务端 `/api/v1/roots` 的子集）。 */
export interface PathRoot {
  id: string
  kind: string
  path: string
}

/** 路径比较用归一：反斜杠转正斜杠、去掉尾部斜杠；Windows 下大小写不敏感。 */
export function normPath(p: string, isWin = false): string {
  const s = String(p ?? "")
    .replace(/\\/g, "/")
    .replace(/\/+$/, "")
  return isWin ? s.toLowerCase() : s
}

/** 仓库相对路径 → 绝对路径（`repoRootAbs` 为空或路径本身是绝对路径时原样返回）。 */
export function absOfRepo(repoRootAbs: string, repoRel: string, isWin = false): string {
  const rel = String(repoRel ?? "").replace(/^\/+/, "")
  if (!repoRootAbs) return normPath(rel, isWin)
  const base = normPath(repoRootAbs, false)
  return rel ? `${base}/${rel}` : base
}

/**
 * `abs` 在 `absDir` 之内时的相对路径；不在其内返回 null（含 `absDir === abs` → `""`）。
 * 三个参数都应是绝对路径；**比较**用 {@link normPath}（Windows 下不敏感），
 * 但**返回值取自原串**（只按长度截取）——归一化会小写整条路径，拿它当相对路径会把文件名的大小写改掉
 * （磁盘上通常无碍，但标签标题/路径回显会变成另一个名字，且把"我传给你什么"变成了"我以为你叫什么"）。
 */
export function relWithin(absDir: string, abs: string, isWin = false): string | null {
  const dir = normPath(absDir, isWin)
  const target = normPath(abs, isWin)
  if (!dir || !target) return null
  if (target === dir) return ""
  if (!target.startsWith(`${dir}/`)) return null
  // 反斜杠→斜杠是 1:1 替换、尾斜杠只在末尾被去掉，故归一后的长度可用于原串下标
  return String(abs ?? "").slice(dir.length + 1)
}

/**
 * 当前根在仓库内的前缀（仓库相对形式）。
 *
 * 返回 `null` 与返回 `""` **不是一回事**：`""` = 根本身就是仓库根（前缀为空但确实在仓库里），
 * `null` = 根不在这个仓库里（或仓库根未知）——调用方据此决定是「根相对就是仓库相对」还是「没得换算」。
 * 早期实现把两者混在一起（都返回 `""`），于是子目录根被当成仓库根：前缀算不出来，
 * 面板按「整仓库」展示、行的路径也没人补前缀，点开就是「文件不存在」。
 */
export function repoPrefixOfAbs(rootAbs: string, repoRootAbs: string, isWin = false): string | null {
  if (!rootAbs || !repoRootAbs) return null
  return relWithin(repoRootAbs, rootAbs, isWin)
}

/**
 * 根相对路径 → 仓库相对路径（`prefix` 为 `""` 时原样返回）。
 * 用于把树/编辑器那侧的路径交给 Git 侧（日志过滤、文件历史）。
 */
export function toRepoRel(prefix: string, rootRel: string): string {
  const rel = String(rootRel ?? "").replace(/^\/+/, "")
  if (!prefix) return rel
  return rel ? `${prefix}/${rel}` : prefix
}

/** 从根 id 解析绝对路径（仅 `abs:` 自带路径；其余类型（sess/proj/bind/user）必须查根清单）。 */
export function rootAbsFromId(id: string): string | null {
  const m = /^abs:(.+)$/s.exec(String(id ?? ""))
  if (!m) return null
  const p = normPath(m[1] ?? "")
  return p || null
}

/** 根类型在「同样覆盖目标」时的优先级（与 deeplink.ts 的 KIND_RANK 同口径：项目类最优先，任意目录最后）。 */
const KIND_RANK: Record<string, number> = { proj: 0, sess: 1, user: 2, abs: 3 }
const rankOf = (kind: string): number => KIND_RANK[kind] ?? 9

export interface ResolvedRepoPath {
  /** 打开该文件用的根 id（可能与传入的当前根不同）。 */
  root: string
  /** 该根内的相对路径。 */
  rel: string
  /** 非 null = 需要把这个临时根加入根清单（`abs:` 型；调用方负责 push 与后续复用）。 */
  create?: { id: string; kind: "abs"; name: string; path: string; writable: boolean }
}

/**
 * 仓库相对路径 → 「用哪个根、根内什么路径」才能打开它。
 *
 * 顺序：① **当前根**内 → 用它（不动根，标签/树上下文都不变）；
 * ② 根清单里**覆盖它**的根（最长前缀优先，同长按类型优先级）→ 用那个根；
 * ③ 都没有 → 以**仓库根**建一个 `abs:` 临时根（仓库根是这批路径天然的家；
 *    比「文件所在目录」更稳：同一批改动的多个文件能落在同一个根下）。
 *
 * `repoRootAbs` 未知时返回 null（调用方给明确提示，而不是拼一条必然 404 的路径）。
 */
export function resolveRepoPath(o: {
  repoRel: string
  rootId: string
  rootAbs: string
  repoRootAbs: string
  roots: readonly PathRoot[]
  isWin?: boolean
  /** 临时根的 writable（继承当前根的可写性；服务端仍会二次校验）。 */
  writable?: boolean
}): ResolvedRepoPath | null {
  const { repoRel, rootId, rootAbs, repoRootAbs, isWin = false } = o
  if (!repoRootAbs || !repoRel) return null
  const abs = absOfRepo(repoRootAbs, repoRel, isWin)
  // ① 当前根内（含根本身）：保持使用同一个根
  const inRoot = relWithin(rootAbs, abs, isWin)
  if (inRoot !== null) return { root: rootId, rel: inRoot }
  // ② 覆盖它的已有根：最长前缀优先
  let best: { root: PathRoot; rel: string; len: number } | null = null
  for (const r of o.roots) {
    const rel = relWithin(r.path, abs, isWin)
    if (rel === null) continue
    const len = normPath(r.path, isWin).length
    if (!best || len > best.len || (len === best.len && rankOf(r.kind) < rankOf(best.root.kind))) {
      best = { root: r, rel, len }
    }
  }
  if (best) return { root: best.root.id, rel: best.rel }
  // ③ 临时 abs 根：指向仓库根
  const base = normPath(repoRootAbs, false)
  return {
    root: `abs:${base}`,
    rel: String(repoRel).replace(/^\/+/, ""),
    create: { id: `abs:${base}`, kind: "abs", name: base.split("/").pop() || base, path: base, writable: o.writable !== false },
  }
}

/* ------------------------------ 工作区外（库文件）绝对路径 ------------------------------ */

/**
 * 库文件的「锚点目录名」（小写）：从文件所在目录向上找，命中即用该层做临时根。
 *
 * 为什么需要锚点：跳到库文件时目标不在任何工作台根内，只能临时建一个 `abs:` 根。若直接取文件父目录，
 * 标准库头（`/usr/include/c++/13/string`）的根名会是 `13`，紧接着跳 `vector` 又建一个同名根；而按锚点
 * 归到 `include`，整批标准库头共用**一个**根，根清单不会被逐个目录刷屏、名字也能看懂。
 *
 * **分两级**（不是一组）：强锚点（几乎只出现在库/依赖里）先扫一遍；全没命中再用弱锚点 `src`。
 * 两级之差是实测出来的：rust-src 的路径 `…/library/core/src/num/mod.rs` 里 `src` 比 `library` **更近**，
 * 只按「就近」判会让整个标准库归到 `library/core/src`；分两级后 `library` 胜出，而 Go 的
 * `$GOROOT/src/fmt`（只有 `src` 可认）仍能归到 `src`。
 */
export const LIBRARY_ANCHORS: readonly string[] = [
  "include",
  "site-packages",
  "dist-packages",
  "typeshed-fallback",
  "node_modules",
  "library",
  "rustlib",
  "vendor",
]

/** 弱锚点：名太通用，**仅当强锚点一个都没命中时**才用（见上）。 */
export const LIBRARY_WEAK_ANCHORS: readonly string[] = ["src"]

/** 向上找锚点的层数上限（库路径通常不深，超出就退到父目录）。 */
export const LIBRARY_ANCHOR_MAX_LEVELS = 12

/**
 * 绝对路径 → 「用哪个根、根内什么路径」才能打开它（**工作区外/库文件**用）。
 *
 * 顺序：① 根清单里已覆盖它的根（最长前缀优先，同长按类型优先级）→ 用它；
 * ② 否则向上找**库锚点**（先强 `LIBRARY_ANCHORS`、再弱 `LIBRARY_WEAK_ANCHORS`），在其上建 `abs:` 临时根；
 * ③ 都没有 → 用文件父目录建临时根。
 *
 * 非绝对路径（相对路径，或既无前导斜杠也无盘符）返回 null——调用方给明确提示，而不是拼一条必然 404 的路径。
 */
export function resolveAbsPath(o: {
  abs: string
  roots: readonly PathRoot[]
  isWin?: boolean
  /** 临时根的 writable（继承全局可写性；服务端仍会二次校验）。 */
  writable?: boolean
  /** 强锚点名单（测试注入用；缺省 `LIBRARY_ANCHORS`）。 */
  anchors?: readonly string[]
  /** 弱锚点名单（测试注入用；缺省 `LIBRARY_WEAK_ANCHORS`）。 */
  weakAnchors?: readonly string[]
}): ResolvedRepoPath | null {
  const { isWin = false } = o
  // 保留原始大小写：根 id / 相对路径要拿去请求，归一化小写会把文件名改成另一个名字（同 relWithin 的注释）
  const abs = String(o.abs ?? "").replace(/\\/g, "/").replace(/\/+$/, "")
  if (!abs) return null
  // 必须看起来是绝对路径：POSIX `/…` / Windows `C:/…` / UNC `//server/share/…`
  const absolute = abs.startsWith("/") || /^[A-Za-z]:\//.test(abs)
  if (!absolute) return null
  // ① 已有根覆盖它：直接用（最长前缀优先，同长按类型优先级）
  let best: { root: PathRoot; rel: string; len: number } | null = null
  for (const r of o.roots) {
    const rel = relWithin(r.path, abs, isWin)
    if (rel === null) continue
    const len = normPath(r.path, isWin).length
    if (!best || len > best.len || (len === best.len && rankOf(r.kind) < rankOf(best.root.kind))) {
      best = { root: r, rel, len }
    }
  }
  if (best) return { root: best.root.id, rel: best.rel }
  // ② / ③ 临时 abs 根：强锚点 → 弱锚点 → 父目录（两级之差见 LIBRARY_ANCHORS 的注释）
  const base =
    anchorDirOf(abs, o.anchors ?? LIBRARY_ANCHORS) ?? anchorDirOf(abs, o.weakAnchors ?? LIBRARY_WEAK_ANCHORS) ?? parentDirOf(abs)
  if (!base) return null
  return { root: `abs:${base}`, rel: relWithin(base, abs, isWin) ?? "", create: tempAbsRoot(base, o.writable !== false) }
}

/** 从文件所在目录向上找最近的库锚点（找不到返回 null）；比较用 basename，不区分大小写。 */
function anchorDirOf(absFile: string, anchors: readonly string[]): string | null {
  const set = new Set(anchors.map((a) => a.toLowerCase()))
  let dir = parentDirOf(absFile)
  for (let level = 0; dir && level <= LIBRARY_ANCHOR_MAX_LEVELS; level++) {
    const name = dir.replace(/[/\\]+$/, "").split(/[/\\]/).pop()?.toLowerCase() ?? ""
    if (set.has(name)) return dir
    const up = parentDirOf(dir)
    if (!up || up === dir) return null
    dir = up
  }
  return null
}

/** 父目录（正反斜杠都认；已到根时返回 null）。 */
function parentDirOf(absPath: string): string | null {
  const p = String(absPath ?? "").replace(/\\/g, "/").replace(/\/+$/, "")
  const i = p.lastIndexOf("/")
  if (i < 0) return null
  if (i === 0) return "/"
  // Windows 盘符根（`C:/a` → `C:/`）：保留盘符前的字符
  const head = p.slice(0, i)
  if (/^[A-Za-z]:$/.test(head)) return `${head}/`
  return head
}

/** 临时 `abs:` 根的根对象（名字取目录名；根名与 id 保持一致）。 */
function tempAbsRoot(dir: string, writable: boolean): NonNullable<ResolvedRepoPath["create"]> {
  const base = dir.replace(/\/+$/, "") || dir
  return { id: `abs:${base}`, kind: "abs", name: base.split("/").pop() || base, path: base, writable }
}
