/** 文件工作台 · 写操作审计：用户本人直操文件系统与 Git 的留痕（DESIGN「文件工作台」）。
 *
 *  与「工具审批」的分工：审批是**事前拦截**（约束模型），审计是**事后可查**（覆盖用户直操）。
 *  只读操作默认不记录（避免噪声）；写操作一律记录。落盘 `{GEBAI_HOME}/audit-fs.jsonl`（JSONL 追加，
 *  超限轮转保留 5 份）。写入失败绝不影响主流程（审计是旁路）。
 */
import { appendFileSync, mkdirSync, renameSync, statSync } from "node:fs"
import { join } from "node:path"

export interface FsAuditEntry {
  ts: number
  user: string
  /** 操作来源：web（文件工作台）/ api（REST/SDK）/ agent（工具链路） */
  source: "web" | "api" | "agent"
  /** 动作：fs.write / fs.delete / git.commit / … */
  action: string
  root: string
  path?: string
  /** 附加信息（大小/分支/提交摘要等；禁止放敏感内容） */
  detail?: Record<string, unknown>
  ok: boolean
  error?: string
  ip?: string
}

const MAX_BYTES = 10 * 1024 * 1024
const KEEP = 5

export class FsAudit {
  private dir: string
  private file: string
  private enabled: boolean
  private queue: Promise<void> = Promise.resolve()

  constructor(home: string, enabled = true) {
    this.dir = home
    this.file = join(home, "audit-fs.jsonl")
    this.enabled = enabled
  }

  record(entry: FsAuditEntry): void {
    if (!this.enabled) return
    // 串行追加（并发写同一文件需保序；失败静默——审计不可影响主流程）
    this.queue = this.queue
      .then(() => this.append(entry))
      .catch(() => {
        /* 忽略 */
      })
  }

  private async append(entry: FsAuditEntry): Promise<void> {
    try {
      mkdirSync(this.dir, { recursive: true })
      await this.rotateIfNeeded()
      appendFileSync(this.file, `${JSON.stringify(entry)}\n`, "utf8")
    } catch {
      /* 磁盘满/权限：静默 */
    }
  }

  private async rotateIfNeeded(): Promise<void> {
    let size = 0
    try {
      size = statSync(this.file).size
    } catch {
      return
    }
    if (size < MAX_BYTES) return
    // 轮转：audit-fs.jsonl → audit-fs.1.jsonl（逐级后移，超 KEEP 删除）
    for (let i = KEEP - 1; i >= 1; i--) {
      const from = join(this.dir, i === 1 ? "audit-fs.jsonl" : `audit-fs.${i - 1}.jsonl`)
      const to = join(this.dir, `audit-fs.${i}.jsonl`)
      try {
        renameSync(from, to)
      } catch {
        /* 不存在 */
      }
    }
  }
}
