import { spawn, type Subprocess } from "bun"
import { join } from "node:path"

/**
 * 开发模式热刷新（`bun run dev --reload` / `GEBAI_DEV_RELOAD=1`）：
 * - 启动 `bun run build:watch` 子进程——先经 scripts/clean-dist.ts 安全清空 dist
 *   （Windows 上 vite 内置 emptyDir 无重试，删除瞬时占用文件会抛 ENOTEMPTY），
 *   再 `vite build --watch` 增量重建，Web 源码变更自动更新 dist
 * - 每次构建完成（stdout 出现 "built"）触发 onChange（防抖），服务端据此广播页面刷新
 * 仅服务端开发模式使用；二进制/生产模式不启用。
 */
export class DevReloadManager {
  private proc: Subprocess | null = null
  private timer: ReturnType<typeof setTimeout> | null = null
  private stopped = false

  constructor(
    private webRoot: string,
    private onChange: () => void,
    private cmd: string[] = ["bun", "run", "build:watch"],
  ) {}

  /** 启动构建 watcher（默认 vite build --watch）；子进程异常退出仅记 stderr，不崩溃服务。 */
  start(): void {
    if (this.proc) return
    this.stopped = false
    const proc = spawn({
      cmd: this.cmd,
      cwd: this.webRoot,
      stdout: "pipe",
      stderr: "pipe",
    })
    this.proc = proc
    ;(async () => {
      const reader = proc.stdout.getReader()
      const dec = new TextDecoder()
      while (!this.stopped) {
        const { value, done } = await reader.read()
        if (done) break
        const text = dec.decode(value)
        // vite build --watch 每轮构建完成输出 "✓ built in xxx"（或 "built in"）
        if (text.includes("built")) this.schedule()
      }
    })().catch(() => {})
    ;(async () => {
      const reader = proc.stderr.getReader()
      const dec = new TextDecoder()
      while (!this.stopped) {
        const { value, done } = await reader.read()
        if (done) break
        console.error("[dev-reload]", dec.decode(value).trimEnd())
      }
    })().catch(() => {})
    proc.exited.then((code) => {
      if (!this.stopped && code !== 0) console.error(`[dev-reload] bun run build:watch 退出码 ${code}`)
    })
  }

  /** 防抖触发 onChange（多 chunk 写入合并为一次刷新）。 */
  private schedule(): void {
    if (this.timer) clearTimeout(this.timer)
    this.timer = setTimeout(() => {
      this.timer = null
      if (!this.stopped) this.onChange()
    }, 200)
  }

  stop(): void {
    this.stopped = true
    if (this.timer) {
      clearTimeout(this.timer)
      this.timer = null
    }
    this.proc?.kill()
    this.proc = null
  }
}

/** Web 源码根目录（packages/web）。 */
export function webRootOf(gebaiHome: string): string {
  // 脚本调试模式：GEBAI_HOME 为仓库根，web 包在 packages/web
  return join(gebaiHome, "packages", "web")
}
