import { describe, expect, test } from "bun:test"
import { existsSync } from "node:fs"
import { mkdtemp } from "node:fs/promises"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import { assetsTool } from "./assets"

const FRAMEWORK = resolve(process.cwd(), "shuangtian")
const LIB = join(FRAMEWORK, "examples/caiyun/assets/library.json")
const hasLib = existsSync(LIB)

async function makeCtx(): Promise<unknown> {
  const workdir = await mkdtemp(join(tmpdir(), "caiyun-assets-"))
  return { workdir, env: { SHUANGTIAN_PROJECT: FRAMEWORK } }
}

async function out(args: Record<string, unknown>, ctx?: unknown): Promise<string> {
  const c = ctx ?? (await makeCtx())
  const r = (await assetsTool.execute(args, c as never)) as { output?: string }
  return String(r?.output ?? "")
}

describe("assets 工具", () => {
  test.skipIf(!hasLib)("不传参时列出全部，并带验证徽标", async () => {
    const txt = await out({})
    expect(txt).toContain("资产库")
    // 分组标题（三类都该出现，因为库里三类都有）
    expect(txt).toContain("示例脚本")
    expect(txt).toContain("素材图")
    expect(txt).toContain("可复用片段")
    // 徽标必须出现——它是"能不能当成品参照"的唯一判据
    expect(txt).toContain("verified")
  })

  test.skipIf(!hasLib)("按关键词过滤能命中光照配方", async () => {
    const txt = await out({ query: "lighting" })
    expect(txt).toContain("lighting-dark-stage")
  })

  test.skipIf(!hasLib)("按 kind 过滤只留该类别", async () => {
    const txt = await out({ kind: "media" })
    expect(txt).toContain("bg-deep")
    expect(txt).not.toContain("sample-cinematic")
  })

  test.skipIf(!hasLib)("详情含可抄的具体字段值与已知限制", async () => {
    const txt = await out({ id: "lighting-dark-stage" })
    // 具体的字段值（而非"调一下光照"）
    expect(txt).toContain("ambient")
    expect(txt).toContain("rim")
    // 路径要给出，模型才能去 read
    // 限制也要给出——否则模型会把它当万灵药
    expect(txt).toContain("metallic")
    expect(txt).toContain("限制")
  })

  test.skipIf(!hasLib)("未知 id 给出相近项而不是空结果", async () => {
    const txt = await out({ id: "lighting" })
    // 应提示相近的 id（lighting-dark-stage 包含 lighting）
    expect(txt).toMatch(/lighting-dark-stage|未知资产/)
  })

  test.skipIf(!hasLib)("实测数据出现在列表里（不带 id 也能看到静止占比）", async () => {
    const txt = await out({ query: "sample-cinematic" })
    expect(txt).toContain("实测")
    expect(txt).toMatch(/静止/)
  })

  test("库缺失时给出可执行的替代路径（不是一句报错）", async () => {
    const workdir = await mkdtemp(join(tmpdir(), "caiyun-nolib-"))
    const txt = await out({}, { workdir, env: { SHUANGTIAN_PROJECT: join(workdir, "nope") } })
    expect(txt).toContain("assets")
    expect(txt).toContain("styles")
  })
})

describe("资产库数据的自洽性", () => {
  test.skipIf(!hasLib)("每条都有 id/kind/path/summary/use_for/verified", async () => {
    const lib = JSON.parse(await Bun.file(LIB).text()) as {
      assets: Array<Record<string, unknown>>
    }
    expect(lib.assets.length).toBeGreaterThan(5)
    for (const a of lib.assets) {
      // path 对 snippet 可为空（内联知识无独立文件），其余必填
      const required = a.kind === "snippet" ? ["id", "kind", "summary", "use_for", "verified"]
        : ["id", "kind", "path", "summary", "use_for", "verified"]
      for (const key of required) {
        expect(a[key]).toBeTruthy()
      }
      expect(["script", "media", "snippet"]).toContain(String(a.kind))
      expect(["verified", "draft"]).toContain(String(a.verified))
    }
  })

  test.skipIf(!hasLib)("script/media 类资产的 path 必须真实存在", async () => {
    // 库里写了路径但文件被删/改名，是"资产库"最典型的腐烂方式：
    // 模型读它得到 404，然后自己去找别的，等于白建库。
    //
    //  只查 script/media：`snippet` 是**内联知识**（字段值、公式、写法），
    //  本来就没有独立文件——给它编一个假路径比不写更坑（模型会去 read 它）。
    const lib = JSON.parse(await Bun.file(LIB).text()) as {
      assets: Array<{ id: string; kind: string; path: string }>
    }
    const missing: string[] = []
    for (const a of lib.assets) {
      if (a.kind === "snippet") continue
      // 模板路径带占位符，跳过
      if (a.path.includes("<")) continue
      const p = join(FRAMEWORK, a.path)
      if (!existsSync(p)) missing.push(`${a.id} → ${a.path}`)
    }
    if (missing.length) console.log("缺失：\n" + missing.join("\n"))
    expect(missing).toEqual([])
  })

  test.skipIf(!hasLib)("snippet 不得写一个装成路径的字符串", async () => {
    // 反向约束：snippet 的 path 若存在，就不能是一个会被 read 到的“像路径”的串。
    // 实测踩过：写成 "docs 内联（无独立文件）"，既不是真实文件、
    // 又会让模型拿去 read，报一个莫名其妙的 ENOENT。
    const lib = JSON.parse(await Bun.file(LIB).text()) as {
      assets: Array<{ id: string; kind: string; path?: string }>
    }
    for (const a of lib.assets) {
      if (a.kind !== "snippet") continue
      // 允许省略 path；写了就應該是空或明确的非路径标记
      if (!a.path) continue
      expect(a.path.includes("/") && !a.path.includes(" " )).toBe(false)
    }
  })

  test.skipIf(!hasLib)("measured 里给出的数值必须是数字", async () => {
    // 量化数据由审计脚本产出、人工补注时容易写成字符串（"87.9%"）或遗漏。
    // 检查**所有**数值字段，不只 still_ratio——多镜条目用的是 carry_score /
    // moving_pair_ratio / still_ratio_trajectory，只查一个会放过其余三种。
    const lib = JSON.parse(await Bun.file(LIB).text()) as {
      assets: Array<{ id: string; measured?: Record<string, unknown> }>
    }
    const bad: string[] = []
    for (const a of lib.assets) {
      if (!a.measured) continue
      for (const [k, v] of Object.entries(a.measured)) {
        if (typeof v !== "number" || !Number.isFinite(v)) bad.push(`${a.id}.${k} = ${JSON.stringify(v)}`)
      }
    }
    if (bad.length) console.log("非数字：\n" + bad.join("\n"))
    expect(bad).toEqual([])
  })
})
