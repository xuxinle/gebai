/** 正则匹配子进程执行器（edit 的 pattern 编辑项用，与 grep 同款灾难性回溯防护）。
 *  模型提供的正则可能含嵌套量词等灾难性回溯形态（如 (a+)+b 配超长单行）——同步匹配会挂死 JS 事件循环
 *  且无中断手段（服务端全部会话冻结），故匹配在独立子进程执行并设超时强杀。 */
import { spawn } from "node:child_process"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { jsRuntimeCommand } from "../exec/js-tool"

/** 匹配超时：超时强杀子进程并返回引导（不回退进程内匹配——回退即重新暴露挂死面）。 */
export const REGEX_MATCHER_TIMEOUT_MS = 20_000

/** 单次匹配返回上限（防 replace_all 类通配匹配撑爆内存与输出）。 */
export const REGEX_MAX_MATCHES = 1000

/** 匹配子进程脚本：stdin 收 {pattern, flags, source, maxMatches}，stdout 回 {matches:[{start,end,groups}], truncated}。 */
const REGEX_MATCHER_SCRIPT = [
  "let input = ''",
  "process.stdin.setEncoding('utf8')",
  "process.stdin.on('data', (d) => { input += d })",
  "process.stdin.on('end', () => {",
  "  try {",
  "    const req = JSON.parse(input)",
  "    const re = new RegExp(req.pattern, req.flags || 'g')",
  "    const max = req.maxMatches || 1000",
  "    const matches = []",
  "    let truncated = false",
  "    let m",
  "    while ((m = re.exec(req.source)) !== null) {",
  "      matches.push({ start: m.index, end: m.index + m[0].length, groups: m.slice(1).map((g) => (g === undefined ? null : g)) })",
  "      if (matches.length >= max) {",
  "        truncated = re.exec(req.source) !== null",
  "        break",
  "      }",
  "      if (m[0].length === 0) re.lastIndex++",
  "    }",
  "    process.stdout.write(JSON.stringify({ matches, truncated }))",
  "  } catch (e) {",
  "    process.stdout.write(JSON.stringify({ error: String((e && e.message) || e) }))",
  "  }",
  "})",
].join("\n")

/** 单处匹配：字符区间 [start, end) 与捕获组 1..n（未参与匹配的组为 null）。 */
export interface RegexMatch {
  start: number
  end: number
  groups: Array<string | null>
}

let runnerPath: string | null = null

/** 执行正则匹配：{matches, truncated} 或 {error}（超时/崩溃/输出非法）。 */
export async function runRegexMatcher(req: {
  pattern: string
  flags?: string
  source: string
  maxMatches?: number
}): Promise<{ error?: string; matches?: RegexMatch[]; truncated?: boolean }> {
  const { writeFile } = await import("node:fs/promises")
  if (!runnerPath) {
    runnerPath = join(tmpdir(), `gebai-regex-runner-${process.pid}.js`)
    await writeFile(runnerPath, REGEX_MATCHER_SCRIPT)
  }
  const cmd = jsRuntimeCommand(runnerPath)
  const isWin = process.platform === "win32"
  const child = spawn(cmd[0], cmd.slice(1), { stdio: ["pipe", "pipe", "pipe"], detached: !isWin })
  let settled = false
  const kill = () => {
    if (settled) return
    try {
      if (isWin) spawn("taskkill", ["/pid", String(child.pid), "/T", "/F"])
      else if (child.pid) process.kill(-child.pid, "SIGKILL")
    } catch {
      try {
        child.kill("SIGKILL")
      } catch {
        /* 已退出 */
      }
    }
  }
  const timer = setTimeout(kill, REGEX_MATCHER_TIMEOUT_MS)
  try {
    child.stdin!.end(JSON.stringify(req))
    const stdout = await new Promise<string>((resolve, reject) => {
      let buf = ""
      child.stdout!.setEncoding("utf8")
      child.stdout!.on("data", (d: string) => (buf += d))
      child.stdout!.on("end", () => resolve(buf))
      child.stdout!.on("error", reject)
      child.on("error", reject)
    })
    const parsed = JSON.parse(stdout) as { error?: string; matches?: RegexMatch[]; truncated?: boolean }
    return parsed
  } catch {
    return {
      error: `正则匹配超时或子进程异常（> ${Math.round(REGEX_MATCHER_TIMEOUT_MS / 1000)}s）——模式可能引发灾难性回溯（如嵌套量词 (a+)+b），请简化 pattern 或缩小替换范围`,
    }
  } finally {
    settled = true
    clearTimeout(timer)
    kill()
  }
}
