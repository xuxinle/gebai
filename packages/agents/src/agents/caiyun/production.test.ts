import { describe, expect, test } from "bun:test"
import { existsSync } from "node:fs"
import { mkdtemp, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import { filmRenderTool, scriptCheckTool, scriptSchemaTool } from "./production"

/**
 * 这组测试跑的是**真实二进制**（不做桩），因为它们要钉住的正是
 * 「工具与裁云 exe 之间的契约」——参数名、输出格式、退出码语义。
 * 桩会把差异抹平：文件不存在时 `--dump-schema` 与桩返回值恒等，
 * 于是"用错参数"这种错永远测不出来。
 *
 * 二进制不存在时**跳过**（而不是失败）：CI 上不一定构建过 caiyun。
 */
const FRAMEWORK = resolve(process.cwd(), "shuangtian")
const EXE = join(FRAMEWORK, "build/dev/bin", process.platform === "win32" ? "caiyun.exe" : "caiyun")
const hasExe = existsSync(EXE)

/** 造一个最小 ctx（只填这些工具用到的字段）。 */
async function makeCtx(): Promise<{ workdir: string; env: Record<string, string> }> {
  const workdir = await mkdtemp(join(tmpdir(), "caiyun-prod-"))
  return { workdir, env: { SHUANGTIAN_PROJECT: FRAMEWORK } }
}

async function outOf(
  tool: { execute: (a: Record<string, unknown>, c: never) => Promise<unknown> },
  args: Record<string, unknown>,
  ctx: unknown,
): Promise<string> {
  const r = (await tool.execute(args, ctx as never)) as { output?: string }
  return String(r?.output ?? "")
}

describe("script_schema", () => {
  test.skipIf(!hasExe)("能按关键词查出字段，并给出别名警告", async () => {
    const ctx = await makeCtx()
    const out = await outOf(scriptSchemaTool, { query: "model_spin" }, ctx)
    expect(out).toContain("model_spin")
    // 范围要出来（智能体据此判断该写多大）
    expect(out).toMatch(/范围/)
    // 别名警告要出来——这是"写错时该叫什么"的答案
    expect(out).toContain("model_rot")
  })

  test.skipIf(!hasExe)("按作用域过滤时不含别的作用域", async () => {
    const ctx = await makeCtx()
    const out = await outOf(scriptSchemaTool, { scope: "layer", query: "kind" }, ctx)
    expect(out).toContain("## layer")
    expect(out).not.toContain("## audio")
  })

  test.skipIf(!hasExe)("查不到时给出可执行的下一步（而非空结果）", async () => {
    const ctx = await makeCtx()
    const out = await outOf(scriptSchemaTool, { query: "zzz_不存在_zzz" }, ctx)
    expect(out).toContain("没有匹配")
    expect(out).toContain("省略 query")
  })

  test("exe 不存在时报错要给出构建命令（而不是一句找不到）", async () => {
    // 指向一个不存在的框架根，逼它走缺失分支
    const ctx = { workdir: await mkdtemp(join(tmpdir(), "caiyun-none-")), env: {} }
    const out = await outOf(scriptSchemaTool, { profile: "no-such-profile" }, ctx)
    expect(out).toContain("找不到裁云可执行文件")
    expect(out).toContain("shuangtian_run")
    expect(out).toContain("target=\"caiyun\"")
  })
})

describe("script_check", () => {
  const goodScript = {
    name: "t",
    fps: 30,
    size: [320, 180],
    transition: 0,
    fade_in: 0,
    fade_out: 0,
    audio: { enabled: false },
    scenes: [
      { kind: "custom", duration: 0.2, layers: [{ kind: "rect", x: "10%", y: "10%", w: "20%", h: "20%" }] },
    ],
  }

  test.skipIf(!hasExe)("合法脚本返回通过", async () => {
    const ctx = await makeCtx()
    const out = await outOf(scriptCheckTool, { script: JSON.stringify(goodScript) }, ctx)
    expect(out).toContain("✓")
    expect(out).toContain("可以开始渲染")
  })

  test.skipIf(!hasExe)("拼错字段名被拦，且给出路径与建议名", async () => {
    const ctx = await makeCtx()
    const bad = structuredClone(goodScript)
    // 故意写错：model_rot 应为 model_spin
    ;(bad.scenes[0]!.layers[0] as Record<string, unknown>).model_rot = 45
    const out = await outOf(scriptCheckTool, { script: JSON.stringify(bad) }, ctx)
    expect(out).toContain("✗")
    // 路径要能定位到具体图层
    expect(out).toContain("scenes[0].layers[0]")
    expect(out).toContain("model_spin")
  })

  test.skipIf(!hasExe)("类型写错被拦", async () => {
    const ctx = await makeCtx()
    const bad = structuredClone(goodScript)
    ;(bad.scenes[0]!.layers[0] as Record<string, unknown>).opacity = "high"
    const out = await outOf(scriptCheckTool, { script: JSON.stringify(bad) }, ctx)
    expect(out).toContain("✗")
  })

  test.skipIf(!hasExe)("接受文件路径（不是只有内联 JSON）", async () => {
    const ctx = await makeCtx()
    const p = join(ctx.workdir, "s.json")
    await writeFile(p, JSON.stringify(goodScript), "utf8")
    const out = await outOf(scriptCheckTool, { script: "s.json" }, ctx)
    expect(out).toContain("✓")
  })

  test.skipIf(!hasExe)("JSON 语法错不会被误报成字段错", async () => {
    const ctx = await makeCtx()
    const out = await outOf(scriptCheckTool, { script: "{ not json" }, ctx)
    expect(out).toContain("不是合法 JSON")
  })
})

describe("film_render", () => {
  test.skipIf(!hasExe)(
    "渲 6 帧即返回成片/抽帧/量化三样东西",
    async () => {
      const ctx = await makeCtx()
      const p = join(ctx.workdir, "s.json")
      await writeFile(
        p,
        JSON.stringify({
          name: "t",
          fps: 30,
          size: [320, 180],
          transition: 0,
          fade_in: 0,
          fade_out: 0,
          audio: { enabled: false },
          scenes: [
            {
              kind: "custom",
              duration: 0.2,
              layers: [{ kind: "rect", x: "10%", y: "10%", w: "20%", h: "20%", fill: "#4B8CF5" }],
            },
          ],
        }),
        "utf8",
      )
      const out = await outOf(filmRenderTool, { script: "s.json", frames: 6, shots: 3 }, ctx)
      expect(out).toContain("✓ 成片")
      // 抽帧路径必须列出来（否则模型还得自己找）
      expect(out).toContain("抽帧")
      // 成片要真的落盘
      expect(existsSync(join(ctx.workdir, "out/film.mp4"))).toBe(true)
    },
    180_000,
  )

  test.skipIf(!hasExe)("脚本有错时直接报错，不产生成片", async () => {
    const ctx = await makeCtx()
    const p = join(ctx.workdir, "bad.json")
    await writeFile(
      p,
      JSON.stringify({
        name: "t",
        scenes: [{ kind: "custom", duration: 0.1, layers: [{ kind: "rect", model_rot: 5 }] }],
      }),
      "utf8",
    )
    const out = await outOf(filmRenderTool, { script: "bad.json", frames: 2 }, ctx)
    expect(out).toContain("✗")
    expect(out).toContain("model_spin")
    expect(existsSync(join(ctx.workdir, "out/film.mp4"))).toBe(false)
  })
})
