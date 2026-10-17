/**
 * 真场景取证（不是测试，是一次可复现的现场演示）：起一个"会话派生的子进程 + 孙进程"，
 * 再起一个不属于该会话的长跑进程，然后走 collect action=retire mode=stop，
 * 把「retire 前列出的 pid+cmdline」与「retire 后 pgrep 的结果」原样打印出来。
 *
 * 用法：bun run scripts/retire_tree_demo.ts
 */
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import { readFileSync } from "node:fs"
import type { ToolContext } from "@gebai/sdk"
import { def as shengshengDef } from "../src/agents/shengsheng/shengsheng"
import { PROC_MARKER_ENV } from "../src/agents/shengsheng/proc-tree"
import { execSync } from "node:child_process"

const tool = (n: string) => shengshengDef.tools![n]
const SID = "0123456789abcdef0123456789abcdef"
const OTHER = "ffffffffffffffffffffffffffffffff"

function stub(root: string, home: string) {
  const tmp = join(home, "users", "admin", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  const running = new Set<string>([SID])
  const stopped: string[] = []
  const resolveSafe = (p: string) => resolve(p)
  const ctx = {
    user: "admin",
    sessionId: "s1",
    workdir: tmp,
    sessionWorkdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: resolveSafe,
    readFile: async (p: string) => readFileSync(resolveSafe(p), "utf8"),
    readBinaryFile: async (p: string) => new Uint8Array(await Bun.file(resolveSafe(p)).arrayBuffer()),
    writeFile: async (p: string, c: string) => {
      mkdirSync(dirname(resolveSafe(p)), { recursive: true })
      await Bun.write(resolveSafe(p), c)
    },
    listFiles: async () => [],
    listDir: async (p: string) => {
      const { readdirSync, statSync } = await import("node:fs")
      const abs = resolveSafe(p)
      return readdirSync(abs, { withFileTypes: true }).map((e) => ({ path: e.name, size: e.isDirectory() ? 0 : statSync(join(abs, e.name)).size, modifiedAt: 0, isDir: e.isDirectory() }))
    },
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r: { path: string }) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => root,
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
    sessions: {
      store: {
        createSession: async () => ({ id: "x" }),
        setEnv: async (_i: string, _u: string, v: Record<string, string | null>) => v as Record<string, string>,
        load: async (id: string) => ({ id, messages: [] }),
        delete: async () => {},
      },
      engine: {
        isRunning: (id: string) => running.has(id),
        cancel: (id: string) => {
          stopped.push(id)
          running.delete(id)
        },
        runtimeOf: async () => Object.fromEntries([...running].map((id) => [id, { startedAt: Date.now() - 3_600_000, pending: [], toolCalls: 9 }])),
      },
      run: async () => {},
    },
    tasks: { add: async (i: Record<string, unknown>) => i, list: async () => [], update: async () => null, notify: async () => ({ taskId: "t", delivered: 0, errors: [] }) },
  } as unknown as ToolContext
  return { ctx, stopped }
}

function spawn(marker: string | null, cmd: string[]) {
  const env = { ...process.env }
  if (marker) env[PROC_MARKER_ENV] = marker
  else delete env[PROC_MARKER_ENV]
  return Bun.spawn(cmd, { env, stdout: "ignore", stderr: "ignore", stdin: "ignore" })
}

const home = mkdtempSync(join(tmpdir(), "gebai-retire-demo-"))
const root = join(home, "proj")
mkdirSync(join(root, ".evolve"), { recursive: true })
writeFileSync(
  join(root, ".evolve", "control.md"),
  [
    "## 项目坐标",
    `- 项目根：${root}`,
    "",
    "## 计数",
    "- 呼吸：12",
    "- 派工序号：34",
    "",
    "## worker 池",
    "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |",
    "| --- | --- | --- | --- | --- | --- | --- |",
    `| build-1 | ${SID} | build | 十路并发 st build | 1000 | 3 | running |`,
    "",
    "## 在途",
    "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |",
    "| --- | --- | --- | --- | --- | --- |",
    `| build-1 | ${SID} | 十路并发 st build | 补丁 | 2000 | 3 |`,
    "",
    "## 种子队列",
    "（空）",
    "",
    "## 收藏待办",
    "（空）",
    "",
    "## 断点",
    "（无）",
    "",
  ].join("\n"),
  "utf8",
)
// 工作树可疑态：一个停在回退态的源文件（`.revbak` 才是修复版）
mkdirSync(join(root, "src"), { recursive: true })
writeFileSync(join(root, "src", "shengsheng.ts"), "          const round = seq\n", "utf8")
writeFileSync(join(root, "src", "shengsheng.ts.revbak"), "          const round = (prevW?.round ?? 0) + 1\n", "utf8")

const s = stub(root, home)
const child = spawn(SID, ["sleep", "600"])
const shell = spawn(SID, ["sh", "-c", "sleep 600 & wait"])
const orphanOfSession = spawn(SID, ["bash", "-c", `env -i sleep 600`]) // 标记被 env -i 洗掉且父进程也不带标记 ⇒ 归因不到（安全侧：不杀，如实算在扫过里）
const foreignMarked = spawn(OTHER, ["sleep", "600"])
const foreignPlain = spawn(null, ["sleep", "600"])
await Bun.sleep(400)

const shKids = () => execSync(`pgrep -P ${shell.pid} || true`).toString().trim().split(/\s+/).filter(Boolean)

const show = (label: string) => {
  console.log(`\n=== ${label} ===`)
  const pids = [child.pid, shell.pid, orphanOfSession.pid, foreignMarked.pid, foreignPlain.pid]
  for (const pid of pids) {
    let line = ""
    try {
      line = readFileSync(`/proc/${pid}/cmdline`, "utf8").split("\u0000").filter(Boolean).join(" ")
      const mark = readFileSync(`/proc/${pid}/environ`, "utf8").split("\u0000").find((kv) => kv.startsWith(`${PROC_MARKER_ENV}=`))
      console.log(`pid ${pid}｜${line}｜${mark ?? "（无标记）"}`)
    } catch {
      console.log(`pid ${pid}｜（已不在）/proc/${pid}/cmdline 读不到`)
    }
  }
  console.log(`孙进程（pgrep -P ${shell.pid}）：${shKids().join(" ") || "（无）"}`)
}

console.log(`项目根：${root}`)
console.log(`会话标记：${PROC_MARKER_ENV}=${SID}`)
show("retire 前：现场")

const r = await tool("collect").execute({ action: "retire", session_id: SID, mode: "stop", reason: "第 12 息止损：十路并发 st build 写坏共享缓存", root }, s.ctx)
console.log("\n=== retire 动作前打印的计划清单（回显原文） ===")
console.log(r.output)

await Bun.sleep(300)
show("retire 后：现场")
console.log(`\n会话是否被终止：${s.stopped.includes(SID) ? "是（engine.cancel 已调）" : "否"}`)

for (const pid of [child.pid, shell.pid, orphanOfSession.pid, foreignMarked.pid, foreignPlain.pid]) {
  try {
    process.kill(-pid, "SIGKILL")
  } catch {}
  try {
    process.kill(pid, "SIGKILL")
  } catch {}
}
rmSync(home, { recursive: true, force: true })
