/**
 * 相期窗口拉起的**路径/端口/数据根**决策（`boot/xiangqi-launch`）。
 *
 * 为什么这些必须有测试：这段逻辑以前直接写在 compose 里、只能用日志人肉核对，
 * 结果「从服务自身目录上溯找仓库根」少算了一级，落到 `C:\Users\Administrator\code\shuangtian\...`
 * 这种不存在的路径上——而失败路径只是 warn 一句，服务照常启动，**没人会注意到**。
 *
 * 三条形态（源码 / dist / 自定义 GEBAI_HOME）各自的期望落点都在这里钉死；
 * 另外钉住两件容易与实现漂移的约定：
 * - `--ui` 必须给**绝对路径**（exe 自己的资源探测是相对 cwd 的，宿主 cwd 与仓库树无关）；
 * - `--home` 必须与 agents 侧 `homeFromEnv()` 算出同一个目录（否则文件兼底各写各的）。
 */
import { describe, expect, test, afterAll } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import {
  XINGQI_EXE_NAME,
  planXiangqiLaunch,
  resolveXiangqiBin,
  resolveXiangqiUiDir,
  xiangqiHomeFor,
} from "./xiangqi-launch"
import { xiangqiHomeFromEnv } from "@gebai/agents"

const roots: string[] = []
afterAll(() => {
  for (const r of roots) rmSync(r, { recursive: true, force: true })
})

function tmpRoot(tag: string): string {
  const dir = mkdtempSync(join(tmpdir(), `gebai-xq-${tag}-`))
  roots.push(dir)
  return dir
}

/** 造一棵「源码仓库」：仓库标记 + 指定档位的 exe + UI 目录。 */
function makeRepo(tag: string, opts: { profile?: string; ui?: boolean } = {}): string {
  const repo = tmpRoot(tag)
  const profile = opts.profile ?? "dev"
  mkdirSync(join(repo, "packages", "server", "src", "boot"), { recursive: true })
  mkdirSync(join(repo, "shuangtian", "examples", "xiangqi", "ui"), { recursive: true })
  mkdirSync(join(repo, "shuangtian", "build", profile, "bin"), { recursive: true })
  writeFileSync(join(repo, "shuangtian", "build", profile, "bin", XINGQI_EXE_NAME), "fake")
  if (opts.ui !== false) writeFileSync(join(repo, "shuangtian", "examples", "xiangqi", "ui", "main.xml"), "<ui/>")
  return repo
}

const exeOf = (repo: string, profile = "dev"): string => join(repo, "shuangtian", "build", profile, "bin", XINGQI_EXE_NAME)

describe("exe 路径探测：三种部署形态", () => {
  test("源码形态：服务在 packages/server/src/boot，exe 在仓库根的 shuangtian/build/dev/bin", () => {
    const repo = makeRepo("src")
    const r = resolveXiangqiBin({ gebaiHome: repo, moduleDir: join(repo, "packages", "server", "src", "boot") })
    expect(r.bin).toBe(exeOf(repo))
    expect(r.repoRoot).toBe(repo)
    expect(r.fromOverride).toBe(false)
  })

  test("源码形态且 GEBAI_HOME 是仓库子目录（如 .gebai-local）：靠上溯仍能找到仓库根", () => {
    const repo = makeRepo("srchome")
    const home = join(repo, ".gebai-local")
    mkdirSync(home, { recursive: true })
    // 服务自身目录故意给一个不存在的深层路径：只能靠 GEBAI_HOME 上溯收敛
    const r = resolveXiangqiBin({ gebaiHome: home, moduleDir: join(tmpRoot("nope"), "deep", "dir") })
    expect(r.bin).toBe(exeOf(repo))
    expect(r.repoRoot).toBe(repo)
  })

  test("dist 形态：服务在 packages/server/dist，exe 在同一个仓库树里", () => {
    const repo = makeRepo("dist")
    const r = resolveXiangqiBin({ gebaiHome: repo, moduleDir: join(repo, "packages", "server", "dist") })
    expect(r.bin).toBe(exeOf(repo))
  })

  test("自定义 GEBAI_HOME 与仓库不同根：优先取 {GEBAI_HOME}/shuangtian/build/<档>/bin（部署副本）", () => {
    const outside = tmpRoot("outside")
    mkdirSync(join(outside, "shuangtian", "build", "quick", "bin"), { recursive: true })
    writeFileSync(join(outside, "shuangtian", "build", "quick", "bin", XINGQI_EXE_NAME), "fake")
    const r = resolveXiangqiBin({ gebaiHome: outside, moduleDir: join(tmpRoot("nowhere"), "x") })
    expect(r.bin).toBe(exeOf(outside, "quick"))
    // 部署副本的候选顺序：dev → quick → release → debug（dev 不存在才轮到 quick）
    expect(r.candidates[0]).toBe(exeOf(outside, "dev"))
    expect(r.candidates[1]).toBe(exeOf(outside, "quick"))
  })

  test("XIANGQI_BIN 覆盖：存在则直接用，不再探测别的候选", () => {
    const repo = makeRepo("ovr")
    const custom = join(tmpRoot("custombin"), XINGQI_EXE_NAME)
    writeFileSync(custom, "fake")
    const r = resolveXiangqiBin({ gebaiHome: repo, moduleDir: join(repo, "packages", "server", "src", "boot"), override: custom })
    expect(r.bin).toBe(custom)
    expect(r.fromOverride).toBe(true)
    expect(r.candidates).toEqual([custom])
  })

  test("XIANGQI_BIN 指向不存在的文件时不静默回落到探测（用户显式指定就该如实失败）", () => {
    const repo = makeRepo("ovrmiss")
    const missing = join(tmpRoot("nodir"), "gone", XINGQI_EXE_NAME)
    const r = resolveXiangqiBin({ gebaiHome: repo, moduleDir: join(repo, "packages", "server", "src", "boot"), override: missing })
    expect(r.bin).toBeNull()
    expect(r.fromOverride).toBe(true)
    expect(r.candidates).toEqual([missing])
  })

  test("一个都找不到时：candidates 列出**全部**试过的路径（只报一个会把排查带偏）", () => {
    const empty = tmpRoot("empty")
    const r = resolveXiangqiBin({ gebaiHome: empty, moduleDir: join(empty, "packages", "server", "src", "boot") })
    expect(r.bin).toBeNull()
    expect(r.candidates.length).toBeGreaterThan(1)
    expect(r.candidates).toContain(exeOf(empty, "dev"))
    expect(r.candidates).toContain(exeOf(empty, "quick"))
  })
})

describe("UI 资源目录", () => {
  test("仓库根下取 shuangtian/examples/xiangqi/ui（main.xml 存在才算）", () => {
    const repo = makeRepo("ui")
    expect(resolveXiangqiUiDir(repo)).toBe(join(repo, "shuangtian", "examples", "xiangqi", "ui"))
  })

  test("缺 main.xml 时视为没有（exe 会以「装载失败」退出，不能假装有）", () => {
    const repo = makeRepo("noui", { ui: false })
    expect(resolveXiangqiUiDir(repo)).toBeNull()
  })

  test("仓库根未知时返回 null，不抛", () => {
    expect(resolveXiangqiUiDir(null)).toBeNull()
  })
})

describe("拉起决策 planXiangqiLaunch", () => {
  test("端口透传：URL 用**实际监听端口**（port=0 时由 OS 分配，不能拿配置值凑）", () => {
    const repo = makeRepo("port")
    const plan = planXiangqiLaunch({
      gebaiHome: repo,
      moduleDir: join(repo, "packages", "server", "src", "boot"),
      port: 45678,
      env: {},
    })
    expect(plan.action).toBe("launch")
    if (plan.action !== "launch") return
    expect(plan.url).toBe("ws://127.0.0.1:45678/xiangqi")
    expect(plan.args).toContain("ws://127.0.0.1:45678/xiangqi")
  })

  test("--ui 传绝对路径（exe 的自身探测是相对 cwd 的，宿主 cwd 不可依赖）", () => {
    const repo = makeRepo("uiarg")
    const plan = planXiangqiLaunch({
      gebaiHome: repo,
      moduleDir: join(repo, "packages", "server", "src", "boot"),
      port: 3000,
      env: {},
    })
    expect(plan.action).toBe("launch")
    if (plan.action !== "launch") return
    const i = plan.args.indexOf("--ui")
    expect(i).toBeGreaterThanOrEqual(0)
    expect(plan.args[i + 1]).toBe(join(repo, "shuangtian", "examples", "xiangqi", "ui"))
    expect(plan.uiDir).toBe(join(repo, "shuangtian", "examples", "xiangqi", "ui"))
  })

  test("XIANGQI_AUTO=off：不拉起（大小写与空格都认）", () => {
    const repo = makeRepo("autooff")
    for (const value of ["off", "OFF", " off "]) {
      const plan = planXiangqiLaunch({
        gebaiHome: repo,
        moduleDir: join(repo, "packages", "server", "src", "boot"),
        port: 3000,
        env: { XIANGQI_AUTO: value },
      })
      expect(plan).toEqual({ action: "skip", reason: "auto-off" })
    }
  })

  test("找不到 exe：skip 且带全部候选与上溯起点（warn 要靠它们说清为什么）", () => {
    const empty = tmpRoot("skip")
    const plan = planXiangqiLaunch({
      gebaiHome: empty,
      moduleDir: join(empty, "packages", "server", "src", "boot"),
      port: 3000,
      env: {},
    })
    expect(plan.action).toBe("skip")
    if (plan.action !== "skip" || plan.reason !== "bin-not-found") throw new Error("expected bin-not-found")
    expect(plan.candidates.length).toBeGreaterThan(1)
    expect(plan.searchedFrom.length).toBe(2)
  })

  test("--home 与 agents 侧 homeFromEnv() 算出同一个目录（三种来源逐一核对）", () => {
    const repo = makeRepo("home")
    const cases: Array<Record<string, string | undefined>> = [
      {}, // 都未设 → 回落到 cwd
      { GEBAI_HOME: join(repo, "gh") }, // 只有 GEBAI_HOME
      { GEBAI_HOME: join(repo, "gh"), XIANGQI_HOME: join(repo, "explicit") }, // 二者都有
    ]
    for (const env of cases) {
      const saved = { GEBAI_HOME: process.env.GEBAI_HOME, XIANGQI_HOME: process.env.XIANGQI_HOME }
      try {
        if (env.GEBAI_HOME === undefined) delete process.env.GEBAI_HOME
        else process.env.GEBAI_HOME = env.GEBAI_HOME
        if (env.XIANGQI_HOME === undefined) delete process.env.XIANGQI_HOME
        else process.env.XIANGQI_HOME = env.XIANGQI_HOME
        const expected = xiangqiHomeFromEnv()
        const plan = planXiangqiLaunch({
          gebaiHome: env.GEBAI_HOME ?? process.cwd(),
          moduleDir: join(repo, "packages", "server", "src", "boot"),
          port: 3000,
          env: { ...env },
        })
        expect(plan.action).toBe("launch")
        if (plan.action !== "launch") return
        expect(plan.home).toBe(expected)
        expect(plan.args[plan.args.indexOf("--home") + 1]).toBe(expected)
      } finally {
        for (const [k, v] of Object.entries(saved)) {
          if (v === undefined) delete process.env[k]
          else process.env[k] = v
        }
      }
    }
  })

  test("xiangqiHomeFor：XIANGQI_HOME 优先，其次 {GEBAI_HOME}/xiangqi", () => {
    expect(xiangqiHomeFor("/h", { XIANGQI_HOME: "/x" })).toBe("/x")
    expect(xiangqiHomeFor("/h", {})).toBe(join("/h", "xiangqi"))
    // 空串按「未设置」处理（不能算出相对路径）
    expect(xiangqiHomeFor("/h", { XIANGQI_HOME: "  " })).toBe(join("/h", "xiangqi"))
  })

  test("数据根目录名是纯 ASCII（中文目录名会作为 --home 命令行参数传出去）", () => {
    const home = xiangqiHomeFor("/h", {})
    const leaf = home.split(/[\\/]/).pop() ?? ""
    expect(/^[\x20-\x7e]+$/.test(leaf)).toBe(true)
  })
})
