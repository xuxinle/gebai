/**
 * `shuangtian_doc` 工具测试：**对着真实的霜天仓库文档树**跑（不是夹具）。
 *
 * ## 为什么用真文档而不是造一个假文档树
 *
 * 这个工具的全部价值取决于"文档树的形状"与"路径能不能解析"。用夹具测，
 * 测的只是解析器；而真正会坏的形态是：
 *
 * - 入口页的意图索引指向了不存在的路径（模型第一跳就撞墙）；
 * - 组件页漏了几个组件（"查不到"= 能力不可见）；
 * - 两份页互相指向同一个父页，形成环（模型在环里打转，永远到不了细节）；
 * - 某页正文超过预算上限却没有切块提示（被输出护栏静默截断）。
 *
 * 这四类都只能对**真文件**测出来。所以本文件直接读仓库里的 `docs/agent-docs/`。
 */
import { describe, expect, test } from "bun:test"
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, statSync } from "node:fs"
import { tmpdir } from "node:os"
import { isAbsolute, join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { tools } from "./shuangtian_tools"

/** 仓库内的霜天框架根（本测试与被测代码同仓，直接定位到源树）。 */
const FRAMEWORK = join(import.meta.dir, "..", "..", "..", "..", "..", "shuangtian")
const TREE = join(FRAMEWORK, "docs", "agent-docs")

function makeCtx(home: string, env: Record<string, string> = {}): ToolContext {
  const workdir = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(workdir, { recursive: true })
  return {
    projects: [],
    resolveProjectPath: (name: string) => join(home, name),
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => ({ choice: null, cancelled: true }) as never,
    waitForEnv: async () => false,
    waitForDraw: async () => null,
    user: "default",
    sessionId: "s1",
    workdir,
    sessionWorkdir: workdir,
    home,
    env,
    sandboxed: false,
    resolvePath: (p: string) => (isAbsolute(p) ? p : join(workdir, p)),
  } as unknown as ToolContext
}

function ctxWithFramework(): ToolContext {
  const home = mkdtempSync(join(tmpdir(), "st-doc-"))
  return makeCtx(home, { SHUANGTIAN_PROJECT: FRAMEWORK })
}

/** 递归列出文档树里的 .md（相对树根的路径）。 */
function treeFiles(dir = TREE, prefix = ""): string[] {
  const out: string[] = []
  for (const name of readdirSync(dir)) {
    const full = join(dir, name)
    if (statSync(full).isDirectory()) {
      out.push(...treeFiles(full, `${prefix}${name}/`))
    } else if (name.endsWith(".md")) {
      out.push(`${prefix}${name}`)
    }
  }
  return out
}

describe("文档树自身（结构不变量）", () => {
  test("框架根与文档树存在（测试前提）", () => {
    expect(existsSync(join(FRAMEWORK, "st.pkg"))).toBe(true)
    expect(existsSync(join(TREE, "index.md"))).toBe(true)
  })

  test("入口页提到 overview/ 与 components/ 两个子层（分层不只是目录，要能被发现）", () => {
    const index = readFileSync(join(TREE, "index.md"), "utf8")
    expect(index).toContain("overview/")
    expect(index).toContain("components/")
    // 入口必须给出"我要做什么 → 读哪页"的意图索引，否则模型第一步还是得自己逛
    expect(index).toContain("我该读哪一页")
  })

  test("每个 .md 都能被工具按自己的树内路径读到（无孤儿页）", async () => {
    const ctx = ctxWithFramework()
    const files = treeFiles()
    expect(files.length).toBeGreaterThan(10)
    for (const rel of files) {
      const result = await tools.doc.execute({ path: rel }, ctx)
      expect((result.data as { ok: boolean }).ok).toBe(true)
      // 页头必须回显真实路径（而不是报错文案）
      expect(result.output.startsWith("# ")).toBe(true)
      expect(result.output).toContain(`路径：${rel}`)
    }
  })

  test("每页都有 front matter 的 title 摘要（页头能一行说清这页值不值得读）", async () => {
    const ctx = ctxWithFramework()
    for (const rel of treeFiles()) {
      const result = await tools.doc.execute({ path: rel, outline: true }, ctx)
      const title = (result.data as { title: string }).title
      expect(title.length).toBeGreaterThan(0)
    }
  })

  test("没有页超过单次预算上限一倍（否则必须靠 offset 续读，且入口页要能容忍）", () => {
    // 组件页由生成器产出，体量可控；手写页也不该写成第二个 DESIGN.md。
    for (const rel of treeFiles()) {
      const text = readFileSync(join(TREE, rel), "utf8")
      expect(text.length).toBeLessThan(14000)
    }
  })

  test("组件树覆盖注册表里的全部组件（漏一个 = 该组件对模型不可见）", () => {
    const hppDir = join(FRAMEWORK, "include", "st", "ui", "components")
    const registered = new Set<string>()
    for (const name of readdirSync(hppDir).filter((n) => n.endsWith(".hpp"))) {
      const text = readFileSync(join(hppDir, name), "utf8")
      for (const m of text.matchAll(/ST_UI_COMPONENT(?:_WITH)?\(\s*::st::ui::\w+\s*,\s*"([^"]+)"/g)) {
        registered.add(m[1])
      }
    }
    expect(registered.size).toBeGreaterThan(60)
    const pages = new Set(treeFiles().filter((p) => p.startsWith("components/") && p.split("/").length === 3).map((p) => p.split("/")[2].replace(/\.md$/, "")))
    const missing = [...registered].filter((name) => !pages.has(name))
    expect(missing).toEqual([])
  })

  test("族页都存在，且每个组件页都能在族页里被提到（下钻路径不断）", () => {
    const familyPages = treeFiles().filter((p) => p.startsWith("components/") && p.split("/").length === 2 && p !== "components/index.md")
    expect(familyPages.length).toBeGreaterThan(8)
    const pages = treeFiles().filter((p) => p.startsWith("components/") && p.split("/").length === 3)
    const mentioned = new Set<string>()
    for (const family of familyPages) {
      const text = readFileSync(join(TREE, family), "utf8")
      for (const m of text.matchAll(/components\/([a-z0-9]+)\/([A-Za-z0-9_]+)\.md/g)) {
        expect(existsSync(join(TREE, "components", m[1], `${m[2]}.md`))).toBe(true)
        mentioned.add(m[2])
      }
    }
    const orphans = pages.map((p) => p.split("/")[2].replace(/\.md$/, "")).filter((name) => !mentioned.has(name))
    expect(orphans).toEqual([])
  })})

describe("doc 工具：读取与切块", () => {
  test("省略 path 读入口", async () => {
    const result = await tools.doc.execute({}, ctxWithFramework())
    expect(result.output).toContain("路径：index.md")
    expect(result.output).toContain("入口")
  })

  test("超预算时按行切块，并明确告出续读方式（不让护栏静默截断）", async () => {
    const ctx = ctxWithFramework()
    const result = await tools.doc.execute({ path: "DESIGN.md", max_chars: 800 }, ctx)
    expect(result.output).toContain("字符没读")
    const data = result.data as { next: number; complete: boolean; chars: number }
    expect(data.complete).toBe(false)
    expect(data.next).toBeGreaterThan(0)
    expect(data.next).toBeLessThan(data.chars)
    // 续读要能真的接上：下一块的 offset 必须与上一块的 next 一致
    const next = await tools.doc.execute({ path: "DESIGN.md", max_chars: 800, offset: data.next }, ctx)
    expect(next.output).toContain(`offset=${data.next}`)
  })

  test("小页一次读完并标明已完整返回", async () => {
    const result = await tools.doc.execute({ path: "components/input/Slider.md" }, ctxWithFramework())
    expect(result.output).toContain("已完整返回")
    expect(result.data as object).toMatchObject({ complete: true })
  })

  test("预算被夹取到上限（不允许逼近输出护栏）", async () => {
    const ctx = ctxWithFramework()
    // 三个巨型文档 + 缺省预算/极大预算都要落在 12000 以内，且续读指令不能被自己截掉
    for (const args of [
      { path: "DESIGN.md" },
      { path: "DESIGN.md", max_chars: 999999 },
      { path: "docs/PITFALLS.md", max_chars: 11000 },
      { path: "docs/BACKLOG.md", max_chars: 11000 },
    ]) {
      const result = await tools.doc.execute(args, ctx)
      expect(result.output.length).toBeLessThan(12000)
      // 未读完时续读指令必须活下来（否则调用方不知道还有内容）
      const data = result.data as { complete: boolean; next: number }
      if (!data.complete) expect(result.output).toContain(`offset=${data.next}`)
      // 页脚必须是**完整**的：下一跳块不能被切在半截（预算要按实测开销反算，而不是先拼后截）
      if (((result.data as { hops: string[] }).hops ?? []).length > 0) {
        expect(result.output).toContain("更细（可直接作为下一次 path）")
      }
    }
  })

  test("大纲模式只回标题与偏移，不回正文", async () => {
    const result = await tools.doc.execute({ path: "components/index.md", outline: true }, ctxWithFramework())
    const data = result.data as { headings: Array<{ title: string; offset: number }> }
    expect(data.headings.length).toBeGreaterThan(2)
    // 大纲要能做“值不值得读”的判断：必须列出各组的小节名
    expect(result.output).toContain("基础展示")
    expect(result.output).toContain("编辑器与文档视图")
    // 但不得把正文倒出来（正文里才有这句）
    expect(result.output).not.toContain("先看下面分组表")
  })

  test("页尾给出可直接回填的下一跳路径（且都已验证可解析）", async () => {
    const ctx = ctxWithFramework()
    const result = await tools.doc.execute({ path: "index.md" }, ctx)
    const hops = (result.data as { hops: string[] }).hops
    expect(hops.length).toBeGreaterThan(3)
    for (const hop of hops) {
      const followed = await tools.doc.execute({ path: hop }, ctx)
      expect((followed.data as { ok: boolean }).ok).toBe(true)
    }
  })

  test("旧文档路径同样可读（DESIGN.md / docs/PITFALLS.md 是本树的下一跳目标）", async () => {
    const ctx = ctxWithFramework()
    for (const path of ["DESIGN.md", "docs/PITFALLS.md", "CONVENTIONS.md"]) {
      const result = await tools.doc.execute({ path, max_chars: 600 }, ctx)
      expect(result.output).not.toContain("文档不存在")
      expect((result.data as { ok: boolean }).ok).toBe(true)
    }
  })
})

describe("doc 工具：拒绝与纠偏", () => {
  test("越界路径直接拒绝，且说明是越界不是拼错", async () => {
    const ctx = ctxWithFramework()
    for (const path of ["../../../../etc/passwd", "../st.pkg", "../../gebai/AGENTS.md"]) {
      const result = await tools.doc.execute({ path }, ctx)
      expect(result.output).toContain("越界")
      expect((result.data as { ok: boolean }).ok).toBe(false)
    }
  })

  test(
    "不存在的路径给出近似建议（而不是只说没有）",
    async () => {
      const result = await tools.doc.execute({ path: "components/input/slidr.md" }, ctxWithFramework())
      expect(result.output).toContain("文档不存在")
      expect(result.output).toContain("可能想读的")
      expect(result.output).toContain("components/input/Slider.md")
    },
  )

  test("大小写不一致也能读到（路径容错：Windows 文件系统本身就不区分）", async () => {
    const result = await tools.doc.execute({ path: "components/input/slider.md" }, ctxWithFramework())
    expect((result.data as { ok: boolean }).ok).toBe(true)
  })

  test("目录路径自动落到 index.md", async () => {
    const result = await tools.doc.execute({ path: "components" }, ctxWithFramework())
    expect((result.data as { path: string }).path).toBe("components/index.md")
  })

  test("框架根不存在时如实报告找过哪些位置", async () => {
    const home = mkdtempSync(join(tmpdir(), "st-doc-"))
    const ctx = makeCtx(home, { SHUANGTIAN_PROJECT: join(home, "no-such-framework") })
    const result = await tools.doc.execute({}, ctx)
    expect(result.output).toContain("不存在")
    expect(result.output).toContain("找过的位置")
  })

  test("未配 SHUANGTIAN_PROJECT 时回落 `{GEBAI_HOME}/shuangtian`（单仓布局）", async () => {
    // 真实形态：框架就在歌白主仓根下，而会话里通常没人配环境变量。
    // 这条错过了会表现为“工具永远说文档不存在”——而框架明明在仓里。
    const home = mkdtempSync(join(tmpdir(), "st-doc-home-"))
    const { cpSync } = await import("node:fs")
    cpSync(TREE, join(home, "shuangtian", "docs", "agent-docs"), { recursive: true })
    const ctx = makeCtx(home) // env 为空
    const result = await tools.doc.execute({ path: "components/index.md" }, ctx)
    expect((result.data as { ok: boolean }).ok).toBe(true)
  })
})

describe("doc 工具：装配", () => {
  test("免审批（只读文档）", () => {
    expect(tools.doc.requiresApproval ?? false).toBe(false)
  })

  test("工具名与注册表一致（提示词与 def.tools 用同一个名字）", async () => {
    const mod = await import("./shuangtian")
    expect(Object.keys(mod.def.tools ?? {})).toContain("doc")
  })
})
