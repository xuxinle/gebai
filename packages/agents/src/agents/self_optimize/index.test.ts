import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync, existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname, isAbsolute, resolve } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { makeVisionTool, getVisionProvider, setVisionProviderGetter } from "../../core/shared/vision"
import { def as selfOptimizeDef } from "./index"
import { def as codeDef } from "../code/index"

function ctx(home: string, overrides: Partial<ToolContext> = {}): ToolContext {
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  const base: ToolContext = {
    user: "default",
    sessionId: "s1",
    workdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => (isAbsolute(p) ? p : join(tmp, p)),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: (name) => {
      throw new Error(`未知预置项目: ${name}`)
    },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
  }
  return { ...base, ...overrides }
}

function cleanup(home: string) {
  rmSync(home, { recursive: true, force: true })
}

describe("self_optimize sub-agent", () => {
  test("工具集只含 code 没有的独有工具（复用 code 通用能力，不重复注册）", () => {
    const names = Object.keys(selfOptimizeDef.tools!)
    // 独有能力：反馈读取、测试准入、回滚、待优化暂存、页面捕获（preview_server 已并入 code 随连带装载获得；
    // 视觉分析为全局工具 vision——index.ts 注册、新会话随全局工具继承，def 不复刻）
    for (const t of ["read_feedback", "run_tests", "rollback", "backlog", "page_capture"]) {
      expect(names).toContain(t)
    }
    expect(names).not.toContain("vision")
    // 不复刻 code 的通用工具（装载/预加载时连带装载 code，文件/分析类直接用 code_* 命名空间）
    for (const t of Object.keys(codeDef.tools!)) {
      expect(names).not.toContain(t)
    }
    // 审批：仅自优化专属的 run_tests/rollback（code_* 写类工具的审批由 code 声明承接）
    expect(selfOptimizeDef.requiresApproval).toEqual({ run_tests: true, rollback: true })
    expect(selfOptimizeDef.preload).toBe(false)
  })

  test("page_capture executes and writes captured html to session tmp/capture/", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-"))
    // waitForCapture 服务桩（引擎按任务注入；agents 包测试模拟前端回传 html + 截图）
    const c = ctx(home, {
      waitForCapture: async () => ({
        html: "<html><body><h1>captured</h1></body></html>",
        imageBase64: "data:image/png;base64," + Buffer.from("fake-png").toString("base64"),
      }),
    })
    const r = await selfOptimizeDef.tools!.page_capture.execute({}, c)
    expect(r.output).toContain("已捕获当前页面")
    expect(r.output).toContain("tmp/capture/page-")
    expect(r.blocks!.some((b) => b.type === "image")).toBe(true)
    cleanup(home)
  })

  test("vision tool falls back to a friendly hint when no vision provider is registered", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-vision-"))
    const c = ctx(home)
    // 模块级 vision provider 可能被其他测试（如 integration 启动真实服务）注册污染：
    // 该用例语义即「未注册时降级」，显式清空后断言（测试环境本就期望 null）
    setVisionProviderGetter(null)
    // vision 已是全局工具（def 不复刻）：按 index.ts 的注册方式构造同款实例验证降级提示
    const r = await makeVisionTool({ vision: getVisionProvider }).execute({ target: "描述页面", image: "tmp/capture/page-1.png" }, c)
    expect(r.output).toContain("视觉能力不可用")
    expect(r.output).toContain("GEBAI_VISION_MODEL")
    cleanup(home)
  })

  test("默认项目根兜底（def.projectRoot）：SELF_OPTIMIZE_PROJECT 优先，未配置时 dev 模式自动推导源码仓库根", () => {
    // 显式配置优先（引擎在环境变量命中时不会走兜底，此处验证解析器自身语义）
    const explicit = join(tmpdir(), "gebai-selfopt-explicit-")
    expect(selfOptimizeDef.projectRoot!({ SELF_OPTIMIZE_PROJECT: explicit })).toBe(explicit)
    // 未配置：脚本调试（dev）模式按模块路径推导歌白仓库根（与写范围守卫/run_tests 同源）——
    // 测试环境即源码检出形态；二进制模式此兜底返回 undefined（须显式配置）
    // 层级：src/agents/self_optimize/ → 上溯五级即仓库根（物理分域后比旧布局多一层，改动目录层级须同步）
    const auto = selfOptimizeDef.projectRoot!({})
    expect(auto).toBe(resolve(import.meta.dirname, "..", "..", "..", "..", ".."))
  })

  test("系统提示词内置项目名称与项目根指引（注记「项目根:」由引擎动态注入）", () => {
    expect(selfOptimizeDef.systemPrompt).toContain("项目名称：歌白（GEBAI Agent）")
    expect(selfOptimizeDef.systemPrompt).toContain("项目根以系统提示词动态注记「项目根:」为准")
    expect(selfOptimizeDef.systemPrompt).toContain("SELF_OPTIMIZE_PROJECT")
  })

  test("系统提示词含产物纯净原则（产物只述当前能力与限制，历史注记归 journal/git）", () => {
    expect(selfOptimizeDef.systemPrompt).toContain("产物纯净")
    expect(selfOptimizeDef.systemPrompt).toContain("只述当前完整的能力与限制")
    expect(selfOptimizeDef.systemPrompt).toContain("不留历史痕迹")
  })

  test("写范围**实际状态**行跟随进程环境（策略写在正文、状态写在末尾）", async () => {
    /* 实测过的坑：提示词正文写的是策略（“默认只读…”），而它是否生效取决于启动级变量
       GEBAI_SELF_MODIFY——不把状态说清楚时，`.env` 已配 true 的会话里模型仍以为核心源码不可写，
       白白绕路。两个分支都验：用带查询串的动态 import 拿一份**重新按当前环境求值**的模块。 */
    const saved = process.env.GEBAI_SELF_MODIFY
    try {
      process.env.GEBAI_SELF_MODIFY = "true"
      const on = await import(`./index.ts?scope=on-${Date.now()}`)
      expect(on.systemPrompt).toContain("当前写范围状态：已放开")
      expect(on.systemPrompt).toContain("仓库内任意路径可写")
      process.env.GEBAI_SELF_MODIFY = ""
      const off = await import(`./index.ts?scope=off-${Date.now()}`)
      expect(off.systemPrompt).toContain("当前写范围状态：默认只读")
      expect(off.systemPrompt).toContain("GEBAI_SELF_MODIFY=true 并重启")
    } finally {
      if (saved === undefined) delete process.env.GEBAI_SELF_MODIFY
      else process.env.GEBAI_SELF_MODIFY = saved
    }
  })

  test("系统提示词含三类子Agent 开发场景与目录（内置域/二开域/客卿域，与写范围守卫口径一致）", () => {
    const p = selfOptimizeDef.systemPrompt
    // 三个域与入口布局
    expect(p).toContain("packages/agents/src/agents/{name}/")
    expect(p).toContain("custom/agents/{name}/")
    expect(p).toContain("keqing/{lang}/{name}/")
    expect(p).toContain("{GEBAI_HOME}/agents/{name}/")
    // 各域关键机制（选型依据）
    expect(p).toContain("typecheck:custom")
    expect(p).toContain("跨语言合并")
    expect(p).toContain("能力外置、权力内守")
    expect(p).toContain("GEBAI_KEQING=off")
    expect(p).toContain("改写内置行为而不动上游代码")
    // 写范围口径与守卫一致（三域可写、核心引擎只读）
    expect(p).toContain("二开域 custom/、客卿域 keqing/")
    expect(p).toContain("核心引擎源码（core/engine/app/ws 等）写入会被拒绝")
  })

  test("提示词含歌白仓定位定制（步骤 0），通用取证规则不自复刻而引用 code", () => {
    const p = selfOptimizeDef.systemPrompt
    expect(p).toContain("0) 定位定制（歌白仓活体自查）")
    expect(p).toContain("前端/交互类缺陷先开页面复现")
    expect(p).toContain("改了 `packages/web/src` 必须重建")
    expect(p).toContain("独立实例 + 独立 GEBAI_HOME")
    expect(p).toContain("直接遵循 code 子Agent 提示词")
    expect(p).not.toContain("先取证、后推断")
  })

  test("提示词把霜天框架的 BACKLOG 列为一等输入（协同：霜天侧记录 → self_optimize 取用）", () => {
    const p = selfOptimizeDef.systemPrompt
    // 协同闭环的“回读”端：霜天子Agent 写进去的积压，self_optimize 必须知道去读它。
    expect(p).toContain("shuangtian/docs/BACKLOG.md")
    expect(p).toContain("一等输入")
    // 与歌白自身的 backlog 明确区分（两者是不同清单，不能混用）
    expect(p).toContain("self_optimize_backlog")
  })
})

describe("self_optimize 写范围守卫（SubAgentDef.writeGuard，代码级强制而非仅提示词）", () => {
  /** 构造最小歌白仓库结构（子Agent 定义域 + DESIGN.md + core 目录）。 */
  function makeRepo(): { root: string; sub: string } {
    const root = mkdtempSync(join(tmpdir(), "gebai-selfopt-repo-"))
    mkdirSync(join(root, "packages", "agents", "src"), { recursive: true })
    mkdirSync(join(root, "packages", "server", "src", "core"), { recursive: true })
    return { root, sub: join(root, "packages", "agents", "src") }
  }

  /** 模拟引擎注入：ctx.writeGuard 绑定 self_optimize 声明的守卫政策（SELF_OPTIMIZE_PROJECT 指向临时仓库）。 */
  function guardedCtx(root: string): ToolContext {
    const c = ctx(root, { env: { SELF_OPTIMIZE_PROJECT: root } })
    c.writeGuard = (absPaths) => selfOptimizeDef.writeGuard!(c.env, absPaths)
    return c
  }

  test("默认只读模式：子Agent 扩展面（内置域/二开域/客卿域/霜天）+ 仓库级文档放行，核心引擎源码拒绝（writeGuard 政策直测）", () => {
    const { root, sub } = makeRepo()
    mkdirSync(join(root, "custom", "agents", "my_agent"), { recursive: true })
    mkdirSync(join(root, "keqing", "python", "vision"), { recursive: true })
    // 霜天框架：与子Agent 同属「自身扩展面」——用霜天写应用时发现的框架问题属自我优化，
    // 不是“改外部项目”（外部项目走 code 子Agent）。
    mkdirSync(join(root, "shuangtian", "src", "ui"), { recursive: true })
    mkdirSync(join(root, "shuangtian", "docs"), { recursive: true })
    delete process.env.GEBAI_SELF_MODIFY
    const c = guardedCtx(root)
    try {
      // writeGuard 返回 null = 放行；非空字符串 = 拒绝理由（引擎 write 工具以该值拦截）
      expect(c.writeGuard!([join(sub, "new_agent.ts")])).toBeNull()
      expect(c.writeGuard!([join(root, "DESIGN.md")])).toBeNull()
      // 二开域（custom/agents 与 custom/core）与客卿域（keqing/{lang}/{name}）同属子Agent 扩展面：放行
      expect(c.writeGuard!([join(root, "custom", "agents", "my_agent", "my_agent.ts")])).toBeNull()
      expect(c.writeGuard!([join(root, "custom", "core", "my_lib", "index.ts")])).toBeNull()
      expect(c.writeGuard!([join(root, "keqing", "python", "vision", "tools.py")])).toBeNull()
      // 霜天框架：框架源码与它的待优化积压（BACKLOG）都可写
      expect(c.writeGuard!([join(root, "shuangtian", "src", "ui", "dsl.cpp")])).toBeNull()
      expect(c.writeGuard!([join(root, "shuangtian", "docs", "BACKLOG.md")])).toBeNull()
      const denied = c.writeGuard!([join(root, "packages", "server", "src", "core", "engine.ts")])
      expect(denied).toContain("拒绝写入")
      // 核心引擎与前端/包外代码仍只读（放宽仅覆盖子Agent 扩展面）
      expect(c.writeGuard!([join(root, "packages", "web", "src", "main.ts")])).toContain("拒绝写入")
      // 守卫保护的是歌白仓库本身：仓库根外的常规写入（会话 tmp 产物等）不受限
      expect(c.writeGuard!([join(dirname(root), "outside.txt")])).toBeNull()
    } finally {
      cleanup(root)
    }
  })

  test("GEBAI_SELF_MODIFY=true：仓库内任意路径放行（writeGuard 政策直测）", () => {
    const { root } = makeRepo()
    process.env.GEBAI_SELF_MODIFY = "true"
    const c = guardedCtx(root)
    try {
      expect(c.writeGuard!([join(root, "packages", "server", "src", "core", "engine.ts")])).toBeNull()
    } finally {
      delete process.env.GEBAI_SELF_MODIFY
      cleanup(root)
    }
  })

  test("目标不在守卫仓库范围内（未配置 SELF_OPTIMIZE_PROJECT）时放行（writeGuard 政策直测）", () => {
    const { root } = makeRepo()
    delete process.env.GEBAI_SELF_MODIFY
    // 未配置 SELF_OPTIMIZE_PROJECT：守卫的仓库根不含此临时目录（dev 模式自动推导的是真实仓库检出），
    // 目标不在保护范围内即放行——守卫只保护歌白仓库，不约束无关路径
    const c = ctx(root, { env: {} })
    c.writeGuard = (absPaths) => selfOptimizeDef.writeGuard!(c.env, absPaths)
    try {
      expect(c.writeGuard!([join(root, "packages", "server", "src", "core", "engine.ts")])).toBeNull()
    } finally {
      cleanup(root)
    }
  })

  test("run_tests 与 rollback 工具存在且需审批（测试准入凭证 + 回滚恢复路径）", () => {
    expect(selfOptimizeDef.tools!.run_tests).toBeTruthy()
    expect(selfOptimizeDef.tools!.rollback).toBeTruthy()
    expect(selfOptimizeDef.requiresApproval!.run_tests).toBe(true)
    expect(selfOptimizeDef.requiresApproval!.rollback).toBe(true)
  })

  test("run_tests/rollback 路径参数注入防护：引号/shell 元字符条目拒绝且不触命令，合法含空格路径引号包裹", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-inj-"))
    const cmds: string[] = []
    const c = ctx(home, {
      env: { SELF_OPTIMIZE_PROJECT: home },
      runCommand: async (cmd: string) => {
        cmds.push(cmd)
        return { stdout: "", stderr: "", code: 0 }
      },
    })
    // 审批界面展示的是 files/paths 参数而非拼好的命令：元字符条目必须校验前置直接拒绝
    const r1 = await selfOptimizeDef.tools!.run_tests.execute({ files: ["src/a.test.ts; echo pwned"] }, c)
    expect(r1.output).toContain("非法字符")
    const r2 = await selfOptimizeDef.tools!.rollback.execute({ paths: ['x" && curl evil'] }, c)
    expect(r2.output).toContain("非法字符")
    expect(cmds).toHaveLength(0)
    // 合法含空格路径：双引号包裹保持单参数
    await selfOptimizeDef.tools!.run_tests.execute({ files: ["src/my dir/a.test.ts"] }, c)
    expect(cmds[0]).toBe('bun test "src/my dir/a.test.ts"')
    cleanup(home)
  })

  test("run_tests 的 test 项在包目录下执行（bunfig.toml 的 md loader 与环境净化只在包目录生效）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-so-cwd-"))
    const repo = join(home, "repo")
    mkdirSync(join(repo, "packages", "server"), { recursive: true })
    const calls: Array<{ cmd: string; workdir?: string }> = []
    const c = ctx(home, {
      env: { SELF_OPTIMIZE_PROJECT: repo },
      runCommand: async (cmd: string, opts?: { workdir?: string }) => {
        calls.push({ cmd, workdir: opts?.workdir })
        return { stdout: "ok", stderr: "", code: 0 }
      },
    })
    // 文件同属一个包：切到该包目录（bunfig.toml 生效）并把路径转成相对该包
    await selfOptimizeDef.tools!.run_tests.execute({ files: ["packages/server/src/a.test.ts", "packages/server/src/b.test.ts"] }, c)
    expect(calls[0]!.cmd).toBe('bun test "src/a.test.ts" "src/b.test.ts"')
    expect(calls[0]!.workdir).toBe(join(repo, "packages", "server"))
    // 跨包：保持仓库根执行（不猜包目录，避免把路径拼到不存在的位置）
    calls.length = 0
    await selfOptimizeDef.tools!.run_tests.execute({ files: ["packages/server/src/a.test.ts", "packages/web/src/b.test.ts"] }, c)
    expect(calls[0]!.workdir).toBe(repo)
    expect(calls[0]!.cmd).toBe('bun test "packages/server/src/a.test.ts" "packages/web/src/b.test.ts"')
    cleanup(home)
  })

  test("run_tests checks 三件套：按序执行 test/typecheck/lint，首项失败即停（成功亦合并 stderr 明细）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-checks-"))
    const cmds: string[] = []
    const ok = ctx(home, {
      env: { SELF_OPTIMIZE_PROJECT: home },
      runCommand: async (cmd: string) => {
        cmds.push(cmd)
        // Windows 下 bun test 把用例明细/汇总写 stderr（exit 0 亦然）——成功时也必须并入输出
        return { stdout: "", stderr: cmd.startsWith("bun test") ? "13 pass\n0 fail" : "", code: 0 }
      },
    })
    const r = await selfOptimizeDef.tools!.run_tests.execute({ checks: ["test", "typecheck", "lint"], files: ["src/a.test.ts"] }, ok)
    expect(cmds).toEqual(['bun test "src/a.test.ts"', "bun run typecheck", "bun run lint"])
    expect(r.output).toContain("test（bun test")
    expect(r.output).toContain("13 pass") // stderr 明细不丢
    expect(r.output).toContain("typecheck（bun run typecheck） ✅")
    expect(r.output).toContain("lint（bun run lint） ✅")
    // 失败即停：test 失败后不再执行 typecheck/lint
    const cmds2: string[] = []
    const fail = ctx(home, {
      env: { SELF_OPTIMIZE_PROJECT: home },
      runCommand: async (cmd: string) => {
        cmds2.push(cmd)
        return cmds2.length === 1 ? { stdout: "", stderr: "boom", code: 1 } : { stdout: "", stderr: "", code: 0 }
      },
    })
    const r2 = await selfOptimizeDef.tools!.run_tests.execute({ checks: ["test", "typecheck", "lint"], files: ["src/a.test.ts"] }, fail)
    expect(cmds2).toEqual(['bun test "src/a.test.ts"'])
    expect(r2.output).toContain("❌ exit 1")
    expect(r2.output).toContain("boom")
    // 非法检查项与缺参提示
    const bad = await selfOptimizeDef.tools!.run_tests.execute({ checks: ["fmt"] as never, files: ["a.test.ts"] }, ok)
    expect(bad.output).toContain("无效的检查项")
    cleanup(home)
  })

  test("rollback 清理新建（untracked）文件：恢复修改 + 删除新建，输出列出删除清单（真实 git 仓库）", async () => {
    const { exec } = await import("node:child_process")
    const { promisify } = await import("node:util")
    const run = promisify(exec)
    const repo = mkdtempSync(join(tmpdir(), "gebai-selfopt-git-"))
    await run("git init -q", { cwd: repo })
    await run("git config user.email t@t.local && git config user.name t", { cwd: repo })
    writeFileSync(join(repo, "tracked.txt"), "v1")
    mkdirSync(join(repo, "packages", "agents", "src", "agents"), { recursive: true })
    writeFileSync(join(repo, "packages", "agents", "src", "agents", "keeper.ts"), "export const keeper = 1\n")
    await run("git add -A && git commit -qm init", { cwd: repo })
    // 模拟失败的自我修改：改 tracked + 新建 untracked（新子Agent 文件形态）
    writeFileSync(join(repo, "tracked.txt"), "v2-broken")
    const newFile = join(repo, "packages", "agents", "src", "agents", "bad_agent.ts")
    writeFileSync(newFile, "export const broken = true\n")
    const realRun = (cmd: string, opts?: { workdir?: string }) =>
      new Promise<{ stdout: string; stderr: string; code: number }>((resolve) => {
        exec(cmd, { cwd: opts?.workdir }, (err, stdout, stderr) => resolve({ stdout: String(stdout ?? ""), stderr: String(stderr ?? ""), code: err ? 1 : 0 }))
      })
    const c = ctx(repo, { env: { SELF_OPTIMIZE_PROJECT: repo }, runCommand: realRun })
    const r = await selfOptimizeDef.tools!.rollback.execute({ all: true }, c)
    expect(r.output).toContain("已回滚")
    expect(r.output).toContain("bad_agent.ts")
    expect(await Bun.file(join(repo, "tracked.txt")).text()).toBe("v1") // 修改恢复
    expect(existsSync(newFile)).toBe(false) // 新建文件被清理
    rmSync(repo, { recursive: true, force: true })
  })

  test("journal 优化日志：append 持久化（环形上限）与 list 读取（新→旧）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-jrnl-"))
    const c = ctx(home)
    const empty = await selfOptimizeDef.tools!.journal.execute({ action: "list" }, c)
    expect(empty.output).toContain("暂无优化记录")
    const r1 = await selfOptimizeDef.tools!.journal.execute(
      { action: "append", title: "修复任务通知卡片", changes: ["core/notify.ts: 卡片迁移 2.0"], verification: "run_tests 三件套通过", outcome: "applied", lessons: "2.0 at 语法用 id 属性" },
      c,
    )
    expect(r1.output).toContain("已记录")
    await selfOptimizeDef.tools!.journal.execute({ action: "append", title: "尝试改 engine 被守卫拒", outcome: "reverted", lessons: "核心源码需 GEBAI_SELF_MODIFY" }, c)
    const list = await selfOptimizeDef.tools!.journal.execute({ action: "list" }, c)
    expect(list.output).toContain("修复任务通知卡片")
    expect(list.output).toContain("core/notify.ts: 卡片迁移 2.0")
    expect(list.output).toContain("尝试改 engine 被守卫拒")
    // 新→旧：后 append 的在前
    expect(list.output!.indexOf("尝试改 engine")).toBeLessThan(list.output!.indexOf("修复任务"))
    // 落盘位置：users/{user}/self-optimize-journal.json（与 ws-journal 同位）
    expect(existsSync(join(home, "users", "default", "self-optimize-journal.json"))).toBe(true)
    // 缺 title 拒绝
    const noTitle = await selfOptimizeDef.tools!.journal.execute({ action: "append" }, c)
    expect(noTitle.output).toContain("需要 title")
    cleanup(home)
  })

  test("journal/backlog：action 必传——漏传或与专属参数冲突时显式报错且不动数据（不静默按查询处理）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-action-"))
    const c = ctx(home)
    const jfile = join(home, "users", "default", "self-optimize-journal.json")
    const bfile = join(home, "users", "default", "self-optimize-backlog.json")
    // schema 层声明必填（模型的参数清单里有 action，减少漏参）
    expect(selfOptimizeDef.tools!.journal.parameters.required).toEqual(["action"])
    expect(selfOptimizeDef.tools!.backlog.parameters.required).toEqual(["action"])
    // 漏传 action、但带了 append 专属参数：报错 + 明确「未写入」，绝不静默返回一份列表
    const j1 = await selfOptimizeDef.tools!.journal.execute({ title: "漏传 action", changes: ["a.ts: b"], outcome: "applied" }, c)
    expect(j1.output).toContain("缺少 action")
    expect(j1.output).toContain("未写入任何记录")
    expect(existsSync(jfile)).toBe(false)
    // action=list 却带 append 专属参数：参数冲突报错，同样不写盘
    const j2 = await selfOptimizeDef.tools!.journal.execute({ action: "list", title: "误配" }, c)
    expect(j2.output).toContain("冲突")
    expect(existsSync(jfile)).toBe(false)
    // 什么都不传：显式报错（缺省不再当查询）
    const j3 = await selfOptimizeDef.tools!.journal.execute({}, c)
    expect(j3.output).toContain("缺少 action")
    // 合规调用照常工作
    const ok = await selfOptimizeDef.tools!.journal.execute({ action: "append", title: "正常记录" }, c)
    expect(ok.output).toContain("已记录")
    expect(JSON.parse(await Bun.file(jfile).text())).toHaveLength(1)
    // backlog 同口径：ids 属 resolve 参数，不会误报冲突
    const b1 = await selfOptimizeDef.tools!.backlog.execute({ problem: "漏传 action 的暂存" }, c)
    expect(b1.output).toContain("缺少 action")
    expect(b1.output).toContain("未做任何变更")
    expect(existsSync(bfile)).toBe(false)
    const b2 = await selfOptimizeDef.tools!.backlog.execute({ action: "resolve", ids: [1] }, c)
    expect(b2.output).toContain("未找到编号")
    const b3 = await selfOptimizeDef.tools!.backlog.execute({ action: "list", problem: "误配" }, c)
    expect(b3.output).toContain("冲突")
    expect(existsSync(bfile)).toBe(false)
    cleanup(home)
  })

  test("backlog 待优化暂存清单（离线优化）：add 暂存（会话ID自动记/可覆盖）→ list 查看 → resolve 移除", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-backlog-"))
    const c = ctx(home) // ctx 的 sessionId 为 "s1"
    const empty = await selfOptimizeDef.tools!.backlog.execute({ action: "list" }, c)
    expect(empty.output).toContain("暂无待优化项")
    // add：problem 必填 + direction 可选 + 会话ID 缺省自动取当前会话
    const r1 = await selfOptimizeDef.tools!.backlog.execute(
      { action: "add", problem: "playwright 选择器用法反复出错重试 5 次", direction: "提示词补选择器规范" },
      c,
    )
    expect(r1.output).toContain("已暂存待优化项 #1")
    expect(r1.output).toContain("共 1 项待处理")
    // session_id 显式覆盖（问题来源会话与当前会话不同的场景）
    await selfOptimizeDef.tools!.backlog.execute({ action: "add", problem: "缺 PDF 合并工具", session_id: "ab12cd34" }, c)
    const list = await selfOptimizeDef.tools!.backlog.execute({ action: "list" }, c)
    expect(list.output).toContain("共 2 项")
    expect(list.output).toContain("选择器用法反复出错重试 5 次")
    expect(list.output).toContain("方向: 提示词补选择器规范")
    expect(list.output).toContain("会话: s1") // 自动记录
    expect(list.output).toContain("会话: ab12cd34") // 显式覆盖
    // 落盘位置：users/{user}/self-optimize-backlog.json（与 journal 同位）
    expect(existsSync(join(home, "users", "default", "self-optimize-backlog.json"))).toBe(true)
    // resolve：移除指定编号，剩余保留
    const done = await selfOptimizeDef.tools!.backlog.execute({ action: "resolve", ids: [1] }, c)
    expect(done.output).toContain("已移除 1 项")
    const after = await selfOptimizeDef.tools!.backlog.execute({ action: "list" }, c)
    expect(after.output).toContain("共 1 项")
    expect(after.output).not.toContain("选择器用法反复出错")
    expect(after.output).toContain("缺 PDF 合并工具")
    // resolve 不存在的编号：无匹配不动清单
    const miss = await selfOptimizeDef.tools!.backlog.execute({ action: "resolve", ids: [99] }, c)
    expect(miss.output).toContain("未找到编号 99")
    // 缺参与无效 action
    const noProblem = await selfOptimizeDef.tools!.backlog.execute({ action: "add" }, c)
    expect(noProblem.output).toContain("需要 problem")
    const noIds = await selfOptimizeDef.tools!.backlog.execute({ action: "resolve" }, c)
    expect(noIds.output).toContain("需要 ids")
    const bad = await selfOptimizeDef.tools!.backlog.execute({ action: "xx" }, c)
    expect(bad.output).toContain("无效的 action")
    // 描述与提示词含离线优化引导（触发场景 + 暂存→ask 确认时机→全面优化流程）。
    // 分工：description 是**每次请求都注入**的固定成本，只留触发场景（工具名由 systemPrompt 给全）；
    // systemPrompt 在装载时注入，承担精确工具名与流程细节——全名断言因此落在 systemPrompt 上。
    expect(selfOptimizeDef.description).toContain("重复试错")
    expect(selfOptimizeDef.description).toContain("集中全面优化")
    expect(selfOptimizeDef.systemPrompt).toContain("self_optimize_backlog")
    expect(selfOptimizeDef.systemPrompt).toContain("self_optimize_journal")
    expect(selfOptimizeDef.systemPrompt).toContain("self_optimize_read_feedback")
    expect(selfOptimizeDef.systemPrompt).toContain("离线优化（暂存 → 集中全面优化）")
    expect(selfOptimizeDef.systemPrompt).toContain("action=resolve")
    expect(selfOptimizeDef.systemPrompt).toContain("当场修复")
    // add 输出引导 ask 确认处理时机（当场修复/后续集中）
    expect(r1.output).toContain("当场修复")
    expect(r1.output).toContain("后续集中全面优化")
    cleanup(home)
  })

  test("backlog 并发 add 不互相覆盖（读-改-写串行化）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-backlog-race-"))
    const c = ctx(home)
    // 同一回复的多个工具调用（js 编排 Promise.all）会并发 add：读-改-写各自读到同一旧清单、
    // 后写覆盖前写而静默丢条目——互斥后全部落盘
    await Promise.all(
      Array.from({ length: 6 }, (_, i) => selfOptimizeDef.tools!.backlog.execute({ action: "add", problem: `并发问题 ${i}` }, c)),
    )
    const list = await selfOptimizeDef.tools!.backlog.execute({ action: "list" }, c)
    expect(list.output).toContain("共 6 项")
    for (let i = 0; i < 6; i++) expect(list.output).toContain(`并发问题 ${i}`)
    cleanup(home)
  })
})
