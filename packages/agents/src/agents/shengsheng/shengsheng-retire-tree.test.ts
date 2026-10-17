/**
 * `retire mode=stop` 的**子孙进程树回收**与**回退态告警**（含真进程场景）。
 *
 * 单独成文件而不是追加进 `shengsheng.test.ts`：后者的桩（`stub()`）与用例体量已经很大，而本单的
 * 判据（进程表/环境标记/pid 复用）与调度逻辑本就分模块（`proc-tree.ts`）——测试与实现同粒度，
 * 用例才读得出「哪一条判据在挡什么」。
 *
 * 三层用例，缺一层就会留下一种假绿：
 * ① **纯函数层**（`planReap`）：把归因与保护集钉死（构造进程表，不碰真进程）；
 * ② **真进程层**（`bun spawn sleep`）：真起子进程与孙进程 → 走 `collect action=retire mode=stop` →
 *    `pgrep`/信号探测确认它们**真被回收**。桩层过了不代表真进程层过（桩会把「谁把 pid 传给谁」抹平）；
 * ③ **反向用例**：另起一个**不属于该会话**的长跑进程，retire 之后它**必须还活着**——
 *    没有这条，一个"把全机同名的都杀了"的实现也能把 ② 跑绿。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { def as shengshengDef, parseControl } from "./shengsheng"
import {
  PROC_MARKER_ENV,
  ROLLBACK_SUFFIX,
  descendantsOf,
  detectRollbackResidue,
  parseProcStat,
  planReap,
  reapProcesses,
  renderPlan,
  renderReapResult,
  renderResidue,
  type ProcRow,
} from "./proc-tree"

const tool = (n: string) => shengshengDef.tools![n]
const isWin = process.platform === "win32"

// ---------------------------------------------------------------------------
// 桩（只做本组用例需要的那几件事；其余与 shengsheng.test.ts 的薄桩同口径）
// ---------------------------------------------------------------------------

interface Stub {
  ctx: ToolContext
  root: string
  home: string
  running: Set<string>
  stopped: string[]
  sessions: Map<string, { id: string; name?: string; messages: unknown[] }>
}

function stub(): Stub {
  const home = mkdtempSync(join(tmpdir(), "gebai-shengsheng-tree-"))
  const sessionDir = join(home, "users", "admin", "sessions", "s1")
  const tmp = join(sessionDir, "tmp")
  mkdirSync(tmp, { recursive: true })
  const root = join(home, "proj")
  mkdirSync(root, { recursive: true })
  const s: Stub = { ctx: undefined as unknown as ToolContext, root, home, running: new Set(), stopped: [], sessions: new Map() }
  const resolveSafe = (p: string): string => resolve(p)
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
    writeFile: async (p: string, content: string) => {
      const abs = resolveSafe(p)
      mkdirSync(dirname(abs), { recursive: true })
      await Bun.write(abs, content)
    },
    listFiles: async () => [],
    listDir: async (p: string) => {
      const { readdirSync, statSync } = await import("node:fs")
      const abs = resolveSafe(p)
      return readdirSync(abs, { withFileTypes: true }).map((e) => ({
        path: e.name,
        size: e.isDirectory() ? 0 : statSync(join(abs, e.name)).size,
        modifiedAt: 0,
        isDir: e.isDirectory(),
      }))
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
        createSession: async (_u: string, name?: string) => {
          const id = `sess${s.sessions.size + 1}`
          s.sessions.set(id, { id, name, messages: [] })
          return { id, name }
        },
        setEnv: async (_id: string, _u: string, vars: Record<string, string | null>) => vars as Record<string, string>,
        load: async (id: string) => s.sessions.get(id) ?? null,
        delete: async (id: string) => {
          s.sessions.delete(id)
        },
      },
      engine: {
        isRunning: (id: string) => s.running.has(id),
        cancel: (id: string) => {
          s.stopped.push(id)
          s.running.delete(id)
        },
        runtimeOf: async () => Object.fromEntries([...s.running].map((id) => [id, { startedAt: Date.now() - 60_000, pending: [], toolCalls: 2 }])),
      },
      run: async (id: string) => {
        s.running.add(id)
      },
    },
    tasks: {
      add: async (input: Record<string, unknown>) => input,
      list: async () => [],
      update: async () => null,
      notify: async () => ({ taskId: "t?", delivered: 0, errors: [] }),
    },
  } as unknown as ToolContext
  s.ctx = ctx
  return s
}

const cleanup = (s: Stub): void => rmSync(s.home, { recursive: true, force: true })

/** 铺一份只含一个 worker 的 control.md（跳过 dispatch：本组用例只验退役那一路）。 */
function seedWorker(s: Stub, sid: string, alias = "w-1", running = true): void {
  mkdirSync(join(s.root, ".evolve"), { recursive: true })
  writeFileSync(
    join(s.root, ".evolve", "control.md"),
    [
      "## 项目坐标",
      `- 项目根：${s.root}`,
      "",
      "## 计数",
      "- 呼吸：3",
      "- 派工序号：7",
      "",
      "## worker 池",
      "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |",
      "| --- | --- | --- | --- | --- | --- | --- |",
      `| ${alias} | ${sid} | tree | 起子进程干活 | 1000 | 1 | running |`,
      "",
      "## 在途",
      "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |",
      "| --- | --- | --- | --- | --- | --- |",
      `| ${alias} | ${sid} | 起子进程干活 |  | 2000 | 1 |`,
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
  s.sessions.set(sid, { id: sid, messages: [] })
  if (running) s.running.add(sid)
}

// ---------------------------------------------------------------------------
// 真进程场景：起进程 / 判活 / 收尾
// ---------------------------------------------------------------------------

interface Spawned {
  proc: { pid: number; kill: () => void; exited: Promise<number> }
  pid: number
}

/** 起一个长跑进程（带指定会话标记或完全不带）。 */
function spawnSleeper(markerSession: string | null, cmd: string[] = ["sleep", "600"]): Spawned {
  const env = { ...process.env }
  if (markerSession) env[PROC_MARKER_ENV] = markerSession
  else delete env[PROC_MARKER_ENV]
  const proc = Bun.spawn(cmd, { env, stdout: "ignore", stderr: "ignore", stdin: "ignore" }) as unknown as Spawned["proc"]
  return { proc, pid: proc.pid }
}

/** 起一个**孙进程**场景：shell 再起一个 sleep，并等它（覆盖「标记被 env -i 洗掉」之外的另一条路径）。 */
function spawnShellWithChild(markerSession: string | null): Spawned {
  return spawnSleeper(markerSession, ["sh", "-c", "sleep 600 & wait"])
}

const alive = (pid: number): boolean => {
  try {
    process.kill(pid, 0)
    return true
  } catch {
    return false
  }
}

/** 等到进程真的不在（最多 ms）。*/
async function waitDead(pid: number, ms = 4000): Promise<boolean> {
  const deadline = Date.now() + ms
  while (Date.now() < deadline) {
    if (!alive(pid)) return true
    await new Promise((r) => setTimeout(r, 50))
  }
  return !alive(pid)
}

/** 收尾：无条件把这些 pid 杀掉（用例失败时不得留 600 秒的 sleep 攒在机器上）。 */
function forceKill(...pids: number[]): void {
  for (const pid of pids) {
    try {
      process.kill(-pid, "SIGKILL")
    } catch {
      /* 不是组长/已退出 */
    }
    try {
      process.kill(pid, "SIGKILL")
    } catch {
      /* 已退出 */
    }
  }
}

// ---------------------------------------------------------------------------
// ① 纯函数层：归因与保护集
// ---------------------------------------------------------------------------

describe("子孙进程树：归因（纯函数层）", () => {
  const row = (pid: number, ppid: number, cmd = `cmd-${pid}`, startTime = 100 + pid): ProcRow => ({
    pid,
    ppid,
    pgid: pid,
    startTime,
    cmd,
  })

  test("按标记认人，并连带认出它洗掉标记的子孙（env -i 也跑不掉）", () => {
    const rows = [
      row(1, 0),
      row(100, 1), // 服务进程（无标记）
      row(200, 100), // 本会话的 shell（带标记）
      row(201, 200), // 它起的 sleep（继承标记）
      row(202, 200, "cmd-202", 302), // 它起的另一条（假设标记被 env -i 洗掉）
      row(300, 100), // 另一个会话的进程（别的标记）
      row(301, 300),
      row(400, 1), // 同机的无关长跑进程（无标记）
    ]
    const markers = new Map([
      [200, "sess-A"],
      [201, "sess-A"],
      [300, "sess-B"],
      [301, "sess-B"],
    ])
    const plan = planReap({ rows, markers, sessionId: "sess-A", protect: [999], source: "procfs /proc" })
    expect(plan.via).toBe("marker")
    expect(plan.targets.map((r) => r.pid)).toEqual([200, 201, 202]) // 202 靠**子孙递归**进来
    expect(plan.targets.some((r) => r.pid === 300 || r.pid === 301)).toBe(false) // 别的会话一律不碰
    expect(plan.targets.some((r) => r.pid === 400)).toBe(false) // 无标记的无关进程不碰
  })

  test("保护集（自身/祖先）永不进 targets，但要在 skipped 里点名（可审计）", () => {
    const rows = [row(1, 0), row(50, 1), row(51, 50), row(52, 51)]
    const markers = new Map([
      [50, "sess-A"],
      [51, "sess-A"],
      [52, "sess-A"],
    ])
    const plan = planReap({ rows, markers, sessionId: "sess-A", protect: [51] })
    expect(plan.targets.map((r) => r.pid)).toEqual([50, 52])
    expect(plan.skipped.map((r) => r.pid)).toEqual([51])
    expect(plan.skipped[0].why).toContain("保护集")
  })

  test("标记指向别的会话的进程，即使落在本会话子树里也只记 skipped（跨线不碰）", () => {
    // 夹具里的命令行**刻意长得像同机常见的长跑命令**（sleep/st build）：一个“按命令行猜归因”
    // 的退化实现（`cmd.includes("sleep")` 就把谁都当成本会话）必须在这里被抓出来——
    // 用 `cmd-12` 这类中性名字的话，它照样绿，而反向用例就成了摆设。
    const rows = [row(10, 1), row(11, 10, "bash -c sleep 600"), row(12, 11, "sleep 600")]
    const markers = new Map([
      [11, "sess-A"],
      [12, "sess-B"],
    ])
    const plan = planReap({ rows, markers, sessionId: "sess-A" })
    expect(plan.targets.map((r) => r.pid)).toEqual([11])
    expect(plan.skipped.map((r) => r.pid)).toEqual([12])
    expect(plan.skipped[0].why).toContain("另一个会话")
  })

  test("归因不到时如实报「无法定位」，绝不静默当成已清理", () => {
    const rows = [row(1, 0), row(400, 1)]
    const plan = planReap({ rows, markers: new Map(), sessionId: "sess-A", source: "procfs /proc" })
    expect(plan.via).toBe("none")
    expect(plan.targets).toHaveLength(0)
    expect(plan.unresolved).toContain(PROC_MARKER_ENV)
    const text = renderPlan(plan)
    expect(text).toContain("无法定位")
    expect(text).toContain("扫过 2 个进程")
  })

  test("解析 /proc/<pid>/stat：comm 含空格与右括号也不串位", () => {
    // 真实形态：comm 可以是 `(a b)`、`(x) y)`——只能从**最后一个** ')' 之后切
    const parsed = parseProcStat(42, "42 (a b) c 7 42 42 0 -1 4194304 100 0 0 0 0 0 0 0 20 0 1 0 9876 0 0")
    expect(parsed).not.toBeNull()
    expect(parsed!.ppid).toBe(7)
    expect(parsed!.startTime).toBe(9876)
  })

  test("后代遍历：环与自指不会把遍历卡死", () => {
    const rows: ProcRow[] = [
      { pid: 2, ppid: 3, pgid: 2, startTime: 0, cmd: "" },
      { pid: 3, ppid: 2, pgid: 2, startTime: 0, cmd: "" },
      { pid: 4, ppid: 3, pgid: 2, startTime: 0, cmd: "" },
    ]
    const set = descendantsOf(rows, [2])
    expect([...set].sort((a, b) => a - b)).toEqual([3, 4])
  })

  test("干跑与真杀共用同一份计划：干跑一个信号都不发", async () => {
    const rows = [row(1, 0), row(20, 1), row(21, 20)]
    const markers = new Map([
      [20, "sess-A"],
      [21, "sess-A"],
    ])
    const plan = planReap({ rows, markers, sessionId: "sess-A" })
    const sent: Array<[number, string]> = []
    const r = await reapProcesses(plan, { dryRun: true, signal: (pid, sig) => sent.push([pid, sig]) })
    expect(r.outcome).toBe("dry-run")
    expect(r.targets.map((t) => t.pid)).toEqual([20, 21])
    expect(sent).toHaveLength(0)
    expect(renderReapResult(r)).toContain("干跑")
  })

  test("SIGTERM 后退了的就不再补 SIGKILL（结论如实：全部回收）", async () => {
    const rows = [row(1, 0), row(30, 1)]
    const markers = new Map([[30, "sess-A"]])
    const plan = planReap({ rows, markers, sessionId: "sess-A" })
    const sent: Array<[number, string]> = []
    let aliveNow = true
    const r = await reapProcesses(plan, {
      signal: (pid, sig) => {
        sent.push([pid, sig])
        aliveNow = false
      },
      stillSame: async () => aliveNow,
      sleep: async () => {},
    })
    expect(sent).toEqual([[30, "SIGTERM"]])
    expect(r.outcome).toBe("reaped")
    expect(renderReapResult(r)).toContain("已回收 1/1 个")
  })

  test("SIGTERM 不退的补 SIGKILL；SIGKILL 也不退的如实报「仍在运行」", async () => {
    const rows = [row(1, 0), row(40, 1), row(41, 1)]
    const markers = new Map([
      [40, "sess-A"],
      [41, "sess-A"],
    ])
    const plan = planReap({ rows, markers, sessionId: "sess-A" })
    const sent: Array<[number, string]> = []
    const stillAlive = new Set([40, 41]) // 两个都还活着
    const stubborn = new Set([41]) // 41 谁都杀不掉（模拟 D 状态/无权限）
    const r = await reapProcesses(plan, {
      signal: (pid, sig) => {
        sent.push([pid, sig])
        if (sig === "SIGTERM" && !stubborn.has(pid)) stillAlive.delete(pid) // 正常进程收到 SIGTERM 就退
      },
      stillSame: async (row) => stillAlive.has(row.pid),
      sleep: async () => {},
    })
    expect(sent).toEqual([
      [40, "SIGTERM"],
      [41, "SIGTERM"],
      [41, "SIGKILL"],
    ]) // 40 收到 SIGTERM 就退了（不再被补刀）；只有 41 被补 SIGKILL
    expect(r.outcome).toBe("partial")
    expect(r.survivors.map((x) => x.pid)).toEqual([41])
    expect(renderReapResult(r)).toContain("仍在运行")
  })

  test("pid 复用防护：启动时刻变了就视作「已不在」，不补第二刀", async () => {
    const rows = [row(1, 0), row(60, 1)]
    const markers = new Map([[60, "sess-A"]])
    const plan = planReap({ rows, markers, sessionId: "sess-A" })
    const sent: Array<[number, string]> = []
    const before = Date.now()
    const r = await reapProcesses(plan, {
      // 第一次问「还活着吗」：它已被别人接手（启动时刻不同）→ 视作已死
      stillSame: async () => false,
      signal: (pid, sig) => sent.push([pid, sig]),
      sleep: async () => {},
    })
    expect(sent).toHaveLength(0) // 连 SIGTERM 都不该发
    expect(r.outcome).toBe("nothing")
    expect(Date.now() - before).toBeLessThan(3000)
  })
})

// ---------------------------------------------------------------------------
// ② 真进程层：retire mode=stop 必须连带回收子进程树
// ---------------------------------------------------------------------------

describe.skipIf(isWin)("子孙进程树：retire mode=stop 的真进程场景", () => {
  test("真场景：会话的子进程 + 孙进程被回收，且**不属于该会话**的长跑进程毫发无损", async () => {
    const s = stub()
    const sid = "0123456789abcdef0123456789abcdef"
    seedWorker(s, sid)
    // ① 该会话派生的：一个带标记的 sleep，一个带标记的 shell（它自己再起 sleep = 孙进程）
    const child = spawnSleeper(sid)
    const shell = spawnShellWithChild(sid)
    // 孙进程：从 shell 的进程树里找出来（真场景，不假设顺序）
    const shellKids = (await Bun.$`pgrep -P ${shell.pid}`.text().catch(() => ""))
      .split(/\s+/)
      .filter(Boolean)
      .map(Number)
    // ② 不属于该会话的：别的会话标记 + 完全无标记
    const foreignMarked = spawnSleeper("ffffffffffffffffffffffffffffffff")
    const foreignPlain = spawnSleeper(null)
    try {
      expect(alive(child.pid)).toBe(true)
      expect(alive(shell.pid)).toBe(true)
      expect(shellKids.length).toBeGreaterThan(0)

      const r = await tool("collect").execute({ action: "retire", session_id: sid, mode: "stop", reason: "超 40 分钟", root: s.root }, s.ctx)

      // 回显要能审计：杀之前先列出 pid + 命令行
      expect(r.output).toContain("已终止并退役")
      expect(r.output).toContain(`${child.pid}｜`)
      expect(r.output).toContain(`${shell.pid}｜`)
      // 子进程与孙进程都必须真没了
      expect(await waitDead(child.pid)).toBe(true)
      expect(await waitDead(shell.pid)).toBe(true)
      for (const kid of shellKids) expect(await waitDead(kid)).toBe(true)
      // 反向断言：不属于该会话的必须还活着（否则就是「把全机同名的都杀了」）
      expect(alive(foreignMarked.pid)).toBe(true)
      expect(alive(foreignPlain.pid)).toBe(true)
      // 结构化输出也要能核（js 编排/断言消费）
      const data = r.data as { reap?: { outcome: string; targets: Array<{ pid: number }> }; treeError?: string | null }
      expect(data.treeError ?? null).toBeNull()
      expect(data.reap?.outcome).toBe("reaped")
      expect(data.reap?.targets.map((t) => t.pid)).toContain(child.pid)
    } finally {
      forceKill(child.pid, shell.pid, ...shellKids, foreignMarked.pid, foreignPlain.pid)
      cleanup(s)
    }
  }, 30_000)

  test("干跑（dry_run）：只出清单，不杀进程、不改 control.md", async () => {
    const s = stub()
    const sid = "11111111111111111111111111111111"
    seedWorker(s, sid)
    const child = spawnSleeper(sid)
    const before = readFileSync(join(s.root, ".evolve", "control.md"), "utf8")
    try {
      const r = await tool("collect").execute({ action: "retire", session_id: sid, mode: "stop", dry_run: true, root: s.root }, s.ctx)
      expect(r.output).toContain("[干跑]")
      expect(r.output).toContain(`${child.pid}｜`)
      expect(alive(child.pid)).toBe(true) // 干跑不杀
      expect(s.stopped).toHaveLength(0) // 也不终止会话
      expect(readFileSync(join(s.root, ".evolve", "control.md"), "utf8")).toBe(before) // 状态分毫未动
      expect(parseControl(before).workers).toHaveLength(1)
    } finally {
      forceKill(child.pid)
      cleanup(s)
    }
  }, 20_000)

  test("该会话已无子进程：明确说「无需回收」，不谎报清理了东西", async () => {
    const s = stub()
    const sid = "22222222222222222222222222222222"
    seedWorker(s, sid)
    try {
      const r = await tool("collect").execute({ action: "retire", session_id: sid, mode: "stop", root: s.root }, s.ctx)
      expect(r.output).toContain("已终止并退役")
      expect(r.output).toContain("无法定位") // 没有带标记的进程 → 如实说归因不到
      expect(r.output).not.toContain("已回收")
    } finally {
      cleanup(s)
    }
  }, 20_000)
})

// ---------------------------------------------------------------------------
// ③ 工作树可疑态：`*.revbak` 残留（只告警，不自动恢复）
// ---------------------------------------------------------------------------

describe("回退态告警：*.revbak 残留（只告警、不恢复）", () => {
  const residueFixture = (): { dir: string; root: string } => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-residue-"))
    const root = join(dir, "proj")
    mkdirSync(join(root, "src"), { recursive: true })
    return { dir, root }
  }

  test("内容不一致 ⇒ 告警「源码可能停在回退态」并给核对命令", async () => {
    const { dir, root } = residueFixture()
    try {
      writeFileSync(join(root, "src", "a.ts"), "const round = (prev?.round ?? 0) + 1\n", "utf8")
      writeFileSync(join(root, "src", "a.ts" + ROLLBACK_SUFFIX), "const round = seq\n", "utf8")
      const scan = await detectRollbackResidue(root)
      expect(scan.found).toHaveLength(1)
      expect(scan.found[0].differs).toBe(true)
      const text = renderResidue(scan)
      expect(text).toContain("源码可能停在回退态")
      expect(text).toContain(`diff '${join(root, "src", "a.ts" + ROLLBACK_SUFFIX)}'`)
      expect(text).toContain("不自动恢复")
      // 只读：文件都还在（自动恢复是刻意不做的）
      expect(readFileSync(join(root, "src", "a.ts"), "utf8")).toContain("prev?.round")
      expect(readFileSync(join(root, "src", "a.ts" + ROLLBACK_SUFFIX), "utf8")).toContain("seq")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("内容一致 ⇒ 只说「上一轮没正常收尾」，不喊成事故", async () => {
    const { dir, root } = residueFixture()
    try {
      writeFileSync(join(root, "a.ts"), "same\n", "utf8")
      writeFileSync(join(root, "a.ts" + ROLLBACK_SUFFIX), "same\n", "utf8")
      const scan = await detectRollbackResidue(root)
      expect(scan.found).toHaveLength(1)
      expect(scan.found[0].differs).toBe(false)
      const text = renderResidue(scan)
      expect(text).not.toContain("源码可能停在回退态")
      expect(text).toContain("一致")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("没有残留 ⇒ 不输出任何噪声（回显里不添无意义的一行）", async () => {
    const { dir, root } = residueFixture()
    try {
      writeFileSync(join(root, "a.ts"), "x\n", "utf8")
      const scan = await detectRollbackResidue(root)
      expect(scan.found).toHaveLength(0)
      expect(renderResidue(scan)).toBe("")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("retire mode=stop 会把回退态告警带进回显（真扫描，经 ctx.listDir/readBinaryFile）", async () => {
    const s = stub()
    const sid = "33333333333333333333333333333333"
    seedWorker(s, sid)
    try {
      mkdirSync(join(s.root, "src"), { recursive: true })
      writeFileSync(join(s.root, "src", "x.ts"), "fixed\n", "utf8")
      writeFileSync(join(s.root, "src", "x.ts" + ROLLBACK_SUFFIX), "rolled-back\n", "utf8")
      const r = await tool("collect").execute({ action: "retire", session_id: sid, mode: "stop", reason: "止损", root: s.root }, s.ctx)
      expect(r.output).toContain("源码可能停在回退态")
      expect(r.output).toContain(join(s.root, "src", "x.ts" + ROLLBACK_SUFFIX))
      // 不自动恢复：两份文件都还在原样
      expect(readFileSync(join(s.root, "src", "x.ts"), "utf8")).toBe("fixed\n")
      expect(readFileSync(join(s.root, "src", "x.ts" + ROLLBACK_SUFFIX), "utf8")).toBe("rolled-back\n")
    } finally {
      cleanup(s)
    }
  }, 20_000)
})
