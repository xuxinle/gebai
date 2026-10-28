/**
 * 运行时数据根：`{GEBAI_HOME}/data/`——**单一真源**。
 *
 * ## 为什么要有这一层
 *
 * 开发形态下 `GEBAI_HOME` **就是仓库根**（见 server `core/base/config.ts:resolveGebaiHome`
 * 的 dev 分支），于是运行时数据目录直接长在仓库根，和 `packages/ design/ docs/` 混在一起：
 *
 * ```
 * C:\...\gebai\
 *   packages\  design\  docs\              ← 代码
 *   persona\   xiangqi\  knowledge\        ← 运行时数据（就是这里要收走的）
 * ```
 *
 * 收进一层 `data/` 之后，代码目录与数据目录在视觉与语义上都分开了；`.gitignore` 也能用
 * **根锚定** `/data/` 一条规则挡住（裸模式 `data/` 会匹配任意层级同名目录，误伤源码里
 * 叫 data 的目录——本仓被 `xiangqi/` 裸模式坑过一次，见提交 58ca27e8）。
 *
 * ## 迁移策略：启动时幂等搬家 + 读取回退，写一律写新位置
 *
 * **为什么要搬而不是只做"回退读"**：只回退读会出现**读写分裂**——读命中旧位置（有内容），
 * 而写落到新位置，于是下一次读又命中（仍然存在的）旧位置，新写的永远读不到。
 * 实测这类问题表现为"写进去了但读不到"，极难排查。
 *
 * 所以三件事一起做：
 * 1. `migrateLegacyDir`：启动时把旧目录内容搬到 `data/`（**只补缺失文件，不覆盖**，幂等）；
 * 2. `resolveDataPath`：读取时新位置优先、旧位置回退（兜住迁移失败/只读文件系统）；
 * 3. 写入只用 `dataSubdir`，不再看旧位置。
 *
 * 搬家失败**不阻断启动**：调用方一律 catch 后记日志，降级为"回退读"。
 *
 * 例外：相期窗口与册府边车是**独立进程**，只能读环境变量，由宿主显式注入
 * （`XIANGQI_HOME` / `CEFU_DB`），注入值同样是 `data/` 下的路径，公式只有一份。
 */
import { copyFileSync, existsSync, mkdirSync, readdirSync, renameSync, statSync, unlinkSync } from "node:fs"
import { dirname, join } from "node:path"

/** 数据根目录名（`{GEBAI_HOME}/data`）。 */
export const DATA_DIRNAME = "data"

/** 数据根；home 为空串时回退 cwd（与各调用点原有的"就地自足"口径一致）。 */
export function dataDir(home: string): string {
  const base = (home ?? "").trim()
  return join(base === "" ? process.cwd() : base, DATA_DIRNAME)
}

/** `data/` 下某个子目录（如 `data/persona`）。 */
export function dataSubdir(home: string, name: string): string {
  return join(dataDir(home), name)
}

/**
 * 读路径解析：新位置存在则用新位置，否则回退旧位置（若存在），最后回落新位置。
 *
 * 三个分支各有其用：
 * - 新位置命中 → 正常态（绝大多数情况）；
 * - 旧位置命中 → 迁移失败时的**兜底可读**，不会"升级后记忆全空"；
 * - 都没有 → 返回新位置，让写入方在正确的地方创建（后续写入落在新位置）。
 *
 * 注意这**不是**读写分裂的来源：启动期已由 `migrateLegacyDir` 把旧内容搬到新位置，
 * 正常情况下第一个分支就命中。本函数只是搬家失败时的第二道保险。
 *
 * `legacyName` 省略表示没有旧位置（新引入的数据），行为等同 `dataSubdir`。
 */
export function resolveDataPath(home: string, name: string, legacyName?: string): string {
  const next = dataSubdir(home, name)
  if (existsSync(next)) return next
  if (legacyName) {
    const base = (home ?? "").trim()
    const legacy = join(base === "" ? process.cwd() : base, legacyName)
    if (existsSync(legacy)) return legacy
  }
  return next
}

/** 一次搬家动作的结果（供启动日志与测试断言）。 */
export interface LegacyMigration {
  /** 旧位置（源） */
  from: string
  /** 新位置（目标根） */
  to: string
  /** 实际搬过去的文件数（已存在的不计） */
  moved: number
  /** 因目标已存在而跳过（不覆盖）的文件数 */
  skipped: number
  /** 失败原因（成功时为 null） */
  error: string | null
}

/**
 * 把旧目录的内容**补进**新目录（幂等、不覆盖、失败不抛）。
 *
 * 语义是一个朴素的目录合并：
 * - 递归遍历旧目录，目标路径不存在 → 搬过去（同盘 rename，跨盘 copy+unlink 兜底）；
 * - 目标已存在 → **跳过**（新位置的数据永远优先，绝不覆盖已升级后的内容）；
 * - 旧目录搬空后**不删除**——留给人肉眼确认，删不删由用户决定。
 *
 * 为何不整体 `rename`：旧目录可能已被部分使用（比如同时有 `memory.md` 与 `outbox/`），
 * 而目标目录可能已经存在（用户先在新位置写过），整体 rename 会失败或覆盖。
 *
 * 返回结果而非抛错：调用方（启动流程）拿到 `error` 后记日志、降级为回退读即可，
 * **不允许因为搬家失败而阻断服务启动**。
 */
export function migrateLegacyDir(home: string, legacyName: string, dataName = legacyName): LegacyMigration {
  const base = (home ?? "").trim()
  const root = base === "" ? process.cwd() : base
  const from = join(root, legacyName)
  const to = dataSubdir(home, dataName)
  const result: LegacyMigration = { from, to, moved: 0, skipped: 0, error: null }
  if (!existsSync(from)) return result
  try {
    walk(from, to, result)
  } catch (err) {
    result.error = String((err as Error)?.message ?? err)
  }
  return result
}

/** 递归合并目录（`migrateLegacyDir` 的实现细节：不导出）。 */
function walk(fromDir: string, toDir: string, result: LegacyMigration): void {
  mkdirSync(toDir, { recursive: true })
  for (const name of readdirSync(fromDir)) {
    const src = join(fromDir, name)
    const dst = join(toDir, name)
    if (statSync(src).isDirectory()) {
      walk(src, dst, result)
      continue
    }
    if (existsSync(dst)) {
      result.skipped += 1
      continue
    }
    moveFile(src, dst)
    result.moved += 1
  }
}

/**
 * 单文件搬迁：先试 `rename`（同盘原子且廉价），失败再 `copy+unlink`（跨盘）。
 *
 * Windows 上 `rename` 跨卷会直接抛 `EXDEV`；而 `copyFile` 不支持跨设备时也需兜底，
 * 所以两条路径都要有。`copyFile` 默认不覆盖（COPYFILE_EXCL 语义由我们先用
 * `existsSync` 判过，这里再传 0 让它在竞态下覆盖——覆盖的只能是本函数自己刚判过不存在的位置）。
 */
function moveFile(src: string, dst: string): void {
  mkdirSync(dirname(dst), { recursive: true })
  try {
    renameSync(src, dst)
  } catch {
    copyFileSync(src, dst)
    try {
      // 搬完删源：删不掉不算失败（只是旧目录里多留一份）
      unlinkSync(src)
    } catch {
      /* 忽略：留下源文件不影响读取（新位置已生效） */
    }
  }
}
