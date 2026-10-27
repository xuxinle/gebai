/**
 * 裁云的**制作闭环工具组**：让模型不必拼 exe 命令行、也不必翻源码查字段。
 *
 * ## 为什么需要这组工具（实测代价）
 *
 * 封装之前，做一条 10 秒片子要模型自己完成这些动作，每一件都是一轮往返：
 *
 * 1. **翻源码查字段**：`model_rot` 是不是 `model_spin`？`y` 是顶边还是基线？
 *    `align` 0 是居中还是左？——每次都要 `grep` 渲染器 `.cpp`，而**答案本该由工具回答**。
 * 2. **拼命令行**：`caiyun.exe --headless --script x.json --render out.mp4 --keep-frames
 *    --render-frames 0`；路径、引号、工作目录、`--strict` 默认值都得记对。
 * 3. **轮询完成**：渲染是异步的，只能反复 `ls` 产物或读日志。
 * 4. **抽帧核验**：自己调 ffmpeg、自己挑时间点、自己读图。
 * 5. **量化验收**：自己跑 measure 脚本再解读数字。
 *
 * 30 轮里约 18 轮花在这些脚手架上。这组工具把它们各自收成**一次调用**：
 *
 * | 工具 | 取代的手工动作 |
 * |---|---|
 * | `script_schema` | `grep` 源码查字段名/类型/范围/默认/可用别名 |
 * | `script_check` | 写脚本后盲猜"这样写对吗"（**不渲染**，秒级返回错误与建议） |
 * | `film_render` | 拼命令行 + 轮询 + 抽帧 + 量验收，一次做完 |
 *
 * ## 关键设计：错误在**同一次调用**里返回
 *
 * `script_check` 走 `--strict`（默认开）的同一判据，返回的是
 * `{ok, problems[], hints[]}`——模型拿到就能直接改，不需要再追一轮。
 * 这是"生成即校验"在工具层的落地：**写完立刻知道自己写错了没**。
 *
 * ## 为何直接调 exe 而不经控制通道
 *
 * 控制通道要应用先启动、要连 token、要处理"同一应用只允许一个实例"。
 * 而"写脚本 → 验证 → 出片"是**离线批处理**，`exe --headless` 更直接、
 * 且天然带退出码（严格模式的拦截就靠它）。控制通道留给"交互式调参"场景
 * （`shuangtian_*` 那套仍然可用）。
 */

import { spawn } from "node:child_process"
import { existsSync } from "node:fs"
import { mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises"
import { dirname, isAbsolute, join, resolve } from "node:path"
import type { Tool, ToolContext } from "@gebai/sdk"
import { schema } from "@gebai/sdk/node"

const IS_WINDOWS = process.platform === "win32"

/** 框架根：`SHUANGTIAN_PROJECT` 优先，缺省歌白仓库下的 `shuangtian/`。 */
function frameworkRoot(ctx: ToolContext): string {
  const explicit = String(ctx.env.SHUANGTIAN_PROJECT ?? "").trim()
  if (explicit) return resolve(explicit)
  return resolve(process.cwd(), "shuangtian")
}

/** 裁云可执行文件（按档位；缺省 dev）。 */
function exePath(ctx: ToolContext, profile = "dev"): string {
  const name = IS_WINDOWS ? "caiyun.exe" : "caiyun"
  return join(frameworkRoot(ctx), "build", profile, "bin", name)
}

/** 会话工作目录内的相对路径解析（模型给的路径一律按此基准）。 */
function inWorkdir(ctx: ToolContext, p: string): string {
  return isAbsolute(p) ? p : resolve(ctx.workdir ?? process.cwd(), p)
}

interface RunResult {
  code: number
  stdout: string
  stderr: string
}

/** 跑一个子进程并收全部输出（带超时；超时视为失败，不留孤儿进程）。 */
function runProc(cmd: string, args: string[], cwd: string, timeoutMs: number): Promise<RunResult> {
  return new Promise((resolvePromise) => {
    const child = spawn(cmd, args, { cwd, windowsHide: true })
    let stdout = ""
    let stderr = ""
    let settled = false
    const timer = setTimeout(() => {
      if (settled) return
      settled = true
      // 超时要把进程树带走：裁云内部有 16 路并行渲染线程，
      // 只 kill 父进程会留下占着产物文件的子进程（下次链接/写文件失败）。
      try {
        child.kill("SIGKILL")
      } catch {
        /* 已退出 */
      }
      resolvePromise({ code: -1, stdout, stderr: stderr + `\n[超时 ${timeoutMs}ms，已终止]` })
    }, timeoutMs)

    child.stdout?.on("data", (b: Buffer) => {
      stdout += b.toString("utf8")
    })
    child.stderr?.on("data", (b: Buffer) => {
      stderr += b.toString("utf8")
    })
    child.on("error", (err) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      resolvePromise({ code: -1, stdout, stderr: stderr + `\n[启动失败] ${err.message}` })
    })
    child.on("close", (code) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      resolvePromise({ code: code ?? -1, stdout, stderr })
    })
  })
}

/** 确保 exe 存在，否则给出**可执行**的提示（而不是一句"找不到"）。 */
function requireExe(ctx: ToolContext, profile: string): string | null {
  const exe = exePath(ctx, profile)
  if (existsSync(exe)) return exe
  return null
}

function missingExeMessage(ctx: ToolContext, profile: string): string {
  const root = frameworkRoot(ctx)
  return (
    `找不到裁云可执行文件：${exePath(ctx, profile)}\n`
    + `先构建（一次约 15 秒，之后增量 2 秒）：\n`
    + `  shuangtian_run(action="build", framework="${root}", target="caiyun", profile="${profile}")\n`
    + `若框架根不对，设 SHUANGTIAN_PROJECT 或确认仓库内有 shuangtian/ 目录。`
  )
}

// ============================================================ script_schema

/**
 * 字段查询：**取代"翻源码"**。
 *
 * 返回字段的类型/范围/默认/说明/别名，以及"写错了会被建议成什么"。
 * 支持按关键词过滤：`query="model_"` 只列三维相关字段。
 */
export const scriptSchemaTool: Tool = {
  name: "script_schema",
  safeMode: false,
  description:
    "查裁云脚本的**字段定义**（名/类型/范围/说明/常见写错时的正确名），不必翻源码。" 
    + "可按作用域（script/scene/layer/...）与关键词过滤，如 query='model_spin' 或 'metallic'。"
    + "写任何不确定的字段前先查这里——裁云的字段名写错会被严格模式拒绝，"
    + "而'猜一个近似的名字'是本代理最贵的返工来源。",
  parameters: schema(
    {
      query: { type: "string", description: "关键词过滤（子串，不区分大小写）；省略则列全部" },
      scope: {
        type: "string",
        description: "作用域：script / audio / scene / layer / keyframe / track / obj（省略=全部）",
      },
      profile: { type: "string", description: "构建档位（缺省 dev），只影响找哪个 exe" },
    },
    [],
  ),
  async execute(args, ctx) {
    const profile = String(args.profile ?? "dev").trim() || "dev"
    const query = String(args.query ?? "").trim().toLowerCase()
    const scopeWanted = String(args.scope ?? "").trim().toLowerCase()

    const exe = requireExe(ctx, profile)
    if (!exe) return { output: missingExeMessage(ctx, profile) }

    // 关键：用 `--dump-schema` 一次性拿全表（不需要启动应用/连控制通道）。
    const r = await runProc(exe, ["--dump-schema"], frameworkRoot(ctx), 30_000)
    if (r.code !== 0 || !r.stdout.trim()) {
      return {
        output:
          `读取 schema 失败（退出码 ${r.code}）。\n`
          + `若提示未知参数，说明这个版本的裁云还没有 --dump-schema；`
          + `改用控制通道：启动应用后 get director schema_json。\n${r.stderr.slice(0, 800)}`,
      }
    }

    let parsed: {
      scopes?: Record<string, Array<Record<string, unknown>>>
    }
    try {
      parsed = JSON.parse(r.stdout)
    } catch {
      return { output: `schema 不是合法 JSON：${r.stdout.slice(0, 400)}` }
    }

    const lines: string[] = []
    let total = 0
    for (const [scope, fields] of Object.entries(parsed.scopes ?? {})) {
      if (scopeWanted && scope !== scopeWanted) continue
      const hits = (fields as Array<Record<string, unknown>>).filter((f) => {
        if (!query) return true
        const name = String(f.name ?? "").toLowerCase()
        const summary = String(f.summary ?? "").toLowerCase()
        const aliases = ((f.aliases as string[] | undefined) ?? []).join(" ").toLowerCase()
        return name.includes(query) || summary.includes(query) || aliases.includes(query)
      })
      if (hits.length === 0) continue
      lines.push(`\n## ${scope}（${hits.length} 个）`)
      for (const f of hits) {
        total += 1
        const type = String(f.type ?? "")
        const range = f.min !== undefined ? `  范围 ${f.min}~${f.max}` : ""
        const clamped = f.clamped === false ? "（越界报错）" : f.min !== undefined ? "（越界夹取）" : ""
        const aliases = ((f.aliases as string[] | undefined) ?? []).filter(Boolean)
        const aliasNote = aliases.length ? `  勿写 ${aliases.map((a) => `'${a}'`).join("/")}` : ""
        lines.push(`- \`${String(f.name)}\` (${type})${range}${clamped}${aliasNote}`)
        const summary = String(f.summary ?? "").trim()
        if (summary) lines.push(`    ${summary}`)
      }
    }

    if (total === 0) {
      return {
        output:
          `没有匹配的字段（query='${query}', scope='${scopeWanted || "全部"}'）。\n`
          + "提示：省略 query 可列出全部，便于确认名字怎么写。",
      }
    }
    return {
      output:
        `共 ${total} 个字段。字段名必须逐字一致——写成近似的名字会被严格模式拒绝，`
        + `或被静默忽略（历史版本行为）。\n${lines.join("\n")}`,
    }
  },
}

// ============================================================ 共用：诊断解析

/**
 * 从进程输出里抠出严格模式的 schema 诊断，格式化成给模型看的清单。
 *
 * 严格模式把诊断分两行打印：
 * ```
 * --strict：N 条错误级 schema 诊断，拒绝启动：
 * [{"severity":"warning","path":"scenes[0].layers[0]","key":"model_rot",...}]
 * ```
 * 因此**不能**只找“以 `[` 开头的行”——行首可能带日志前缀（实测：
 * `[2026-...] info 日志落盘` 也在 stdout 里，且它同样以 `[` 开头）。
 * 正确做法是找 `[{"` 子串再往后解析。
 *
 * 两个入口（`script_check` / `film_render`）共用本函数：
 * 各写一份必然漂移——而漂移的表现是“一个工具能报出建议字段名、另一个不能”。
 */
function extractDiagnostics(text: string): string | null {
  for (const line of text.split(/\r?\n/)) {
    const at = line.indexOf("[{")
    if (at < 0) continue
    try {
      const parsed = JSON.parse(line.slice(at)) as Array<{
        path?: string
        key?: string
        message?: string
        suggestion?: string
      }>
      if (!Array.isArray(parsed) || parsed.length === 0) continue
      return parsed
        .map((d, i) => {
          const suggest = d.suggestion ? `\n     → 改成 \`${d.suggestion}\`` : ""
          return `${i + 1}. ${d.path ?? "(未知位置)"}\n     ${d.message ?? ""}${suggest}`
        })
        .join("\n")
    } catch {
      /* 不是诊断 JSON，继续找下一行 */
    }
  }
  return null
}

// ============================================================ script_check

/**
 * 脚本校验：**不渲染**，秒级返回"能不能用、哪里错、该怎么改"。
 *
 * 走的是渲染器 `--strict` 的同一判据（未知字段 / 类型不符 / 越界），
 * 但用一次极短的渲染（1 帧）换来退出码——这是最省事的复用方式：
 * 判据只有一份，不会与真正出片时不一致。
 */
export const scriptCheckTool: Tool = {
  name: "script_check",
  safeMode: false,
  description:
    "校验导演脚本（**不真正出片**，秒级返回）：字段名是否写错、类型是否相符、数值是否越界。"
    + "写脚本后先调它，不要直接渲染——渲染一次 100 秒起，而这一步约 3 秒。"
    + "返回 ok 时再渲；不 ok 时按 problems 逐条改（每条带路径与建议字段名）。"
    + "脚本可以是文件路径，也可以直接给 JSON 文本。",
  parameters: schema(
    {
      script: { type: "string", description: "脚本文件路径（相对会话工作目录）或直接给 JSON 文本" },
      profile: { type: "string", description: "构建档位（缺省 dev）" },
    },
    ["script"],
  ),
  async execute(args, ctx) {
    const profile = String(args.profile ?? "dev").trim() || "dev"
    const raw = String(args.script ?? "").trim()
    if (!raw) return { output: "需要 script（文件路径或 JSON 文本）" }

    const exe = requireExe(ctx, profile)
    if (!exe) return { output: missingExeMessage(ctx, profile) }

    // 判断是路径还是内联 JSON：以 `{` 开头即视为 JSON 文本。
    let scriptPath: string
    let tempScript: string | null = null
    if (raw.startsWith("{")) {
      try {
        JSON.parse(raw) // 先确认是合法 JSON，避免把语法错报成"字段错"
      } catch (err) {
        return { output: `脚本不是合法 JSON：${(err as Error).message}` }
      }
      // 写到会话目录（与脚本同基准，素材相对路径才对得上）
      tempScript = join(ctx.workdir ?? process.cwd(), `.caiyun-check-${Date.now()}.json`)
      await writeFile(tempScript, raw, "utf8")
      scriptPath = tempScript
    } else {
      scriptPath = inWorkdir(ctx, raw)
      if (!existsSync(scriptPath)) return { output: `脚本文件不存在：${scriptPath}` }
    }

    // `--render-frames 1` + 输出到临时目录：只为拿到退出码与诊断，不留产物负担。
    const outDir = join(ctx.workdir ?? process.cwd(), ".caiyun-check-out")
    await mkdir(outDir, { recursive: true })
    const r = await runProc(
      exe,
      [
        "--headless",
        "--strict",
        "--script",
        scriptPath,
        "--render",
        join(outDir, "check.mp4"),
        "--render-frames",
        "1",
      ],
      dirname(scriptPath),
      120_000,
    )

    if (tempScript) {
      // 临时脚本用完即删：留在工作目录会被下一次 `read` 误当成作品。
      try {
        const { unlink } = await import("node:fs/promises")
        await unlink(tempScript)
      } catch {
        /* 删不掉不影响结论 */
      }
    }

    // 严格模式的报错分两行打印：
    //   `--strict：N 条错误级 schema 诊断，拒绝启动：`
    //   `[{...}, {...}]`                     ← 诊断 JSON 单独一行
    // 解析统一走 `extractDiagnostics`（与 `film_render` 同一套）。
    if (r.code === 0) {
      return { output: "✓ 脚本通过校验（字段名/类型/范围都合法），可以开始渲染。" }
    }

    const problems = extractDiagnostics(r.stdout) ?? extractDiagnostics(r.stderr)
    if (problems) {
      return {
        output:
          `✗ 脚本有问题（这些行当前**没有生效**）：\n\n${problems}\n\n`
          + `逐条改完再调本工具复核，不要直接渲染。`,
      }
    }
    return {
      output:
        `✗ 校验未通过（退出码 ${r.code}）。原文：\n`
        + `${(r.stdout + r.stderr).slice(0, 2000)}`,
    }
  },
}

// ============================================================ film_render

/**
 * 渲染：**一次调用完成**"起进程 → 等完成 → 抽帧 → 量验收"。
 *
 * 取代的手工流程：拼命令行、轮询产物、ffmpeg 抽帧、跑 measure 脚本、解读数字。
 * 返回里同时给出成片路径、实测时长/尺寸、抽帧路径、以及量化结论。
 */
export const filmRenderTool: Tool = {
  name: "film_render",
  safeMode: false,
  description:
    "渲染成片并自动核验：起渲染、等完成、抽 4~6 帧、量静止占比/变化密度/过亮帧，一并返回。"
    + "**一次调用取代**「拼命令行 + 轮询 + 抽帧 + 跑度量脚本」四件事。"
    + "成片与抽帧默认写到会话目录。渲染 300 帧 1080p 约 100~140 秒（含三维）。"
    + "脚本有字段错时会直接返回错误（严格模式），不会白渲一轮。",
  parameters: schema(
    {
      script: { type: "string", description: "脚本路径（相对会话工作目录）" },
      out: { type: "string", description: "成片输出路径（.mp4，相对会话目录；缺省 out/film.mp4）" },
      frames: {
        type: "number",
        description: "只渲前 N 帧（缺省 0 = 全片）。试造型时用 30~60，快 5~10 倍",
      },
      shots: {
        type: "number",
        description: "抽几帧用于核验（缺省 5）。帧会按片长均匀分布，写进同目录 check/",
      },
      size: { type: "string", description: "分辨率 WxH（缺省沿用脚本里的 size）" },
      profile: { type: "string", description: "构建档位（缺省 dev）" },
      timeout_ms: { type: "number", description: "超时毫秒（缺省 600000 = 10 分钟）" },
    },
    ["script"],
  ),
  async execute(args, ctx) {
    const profile = String(args.profile ?? "dev").trim() || "dev"
    const scriptPath = inWorkdir(ctx, String(args.script ?? "").trim())
    if (!existsSync(scriptPath)) return { output: `脚本文件不存在：${scriptPath}` }

    const exe = requireExe(ctx, profile)
    if (!exe) return { output: missingExeMessage(ctx, profile) }

    const frames = Math.max(0, Number(args.frames ?? 0) || 0)
    const outRel = String(args.out ?? "").trim() || "out/film.mp4"
    const outPath = inWorkdir(ctx, outRel)
    const sizeArg = String(args.size ?? "").trim()
    const timeoutMs = Math.max(30_000, Number(args.timeout_ms ?? 600_000) || 600_000)
    const shotCount = Math.max(0, Math.min(8, Number(args.shots ?? 5) || 5))

    await mkdir(dirname(outPath), { recursive: true })

    // `--keep-frames` 让我们能自己抽帧（不必依赖 ffmpeg 的 seek，且能看到任意时刻）。
    // ⚠ **帧目录不可指定**：裁云固定落到 `<成片目录>/frames`（`render.cpp` 的
    // `job.output_dir + "/frames"`）。早先这里传了 `--frames-dir` → `未知参数` →
    // 退出码 2，而报错只说了参数名，很容易误以为是脚本问题。
    const frameDir = join(dirname(outPath), "frames")
    const argv = [
      "--headless",
      "--strict",
      "--script",
      scriptPath,
      "--render",
      outPath,
      "--keep-frames",
      "--render-frames",
      String(frames),
    ]
    if (sizeArg) argv.push("--render-size", sizeArg)

    const started = Date.now()
    const r = await runProc(exe, argv, dirname(scriptPath), timeoutMs)
    const elapsed = ((Date.now() - started) / 1000).toFixed(1)

    if (r.code !== 0) {
      // 严格模式的诊断解析与 `script_check` 共用（两个入口不许各自猜格式）
      const problems = extractDiagnostics(r.stdout) ?? extractDiagnostics(r.stderr)
      const detail = problems ?? (r.stdout + r.stderr).slice(0, 1500)
      return { output: `✗ 渲染未通过（退出码 ${r.code}，${elapsed}s）：\n\n${detail}` }
    }

    if (!existsSync(outPath)) {
      return { output: `渲染声称成功但没有产物：${outPath}\n输出：\n${r.stdout.slice(-1200)}` }
    }

    const info = await stat(outPath)
    const lines: string[] = []
    lines.push(`✓ 成片 ${outRel}（${(info.size / 1e6).toFixed(2)} MB，耗时 ${elapsed}s）`)

    // —— 抽帧 ——（用渲染器自己落盘的帧，比 ffmpeg seek 更准且不依赖 ffmpeg）
    let frameFiles: string[] = []
    try {
      const all = (await readdir(frameDir)).filter((f) => /\.(png|jpg)$/i.test(f)).sort()
      frameFiles = all
    } catch {
      /* 没落帧 */
    }

    if (frameFiles.length > 0) {
      const checkDir = join(dirname(outPath), "check")
      await mkdir(checkDir, { recursive: true })
      const picks: string[] = []
      for (let i = 0; i < shotCount; i += 1) {
        // 均匀分布但要避开首尾（首帧常是淡入黑场、末帧是 fade_out）
        const idx = Math.min(
          frameFiles.length - 1,
          Math.round(((i + 1) / (shotCount + 1)) * (frameFiles.length - 1)),
        )
        const src = join(frameDir, frameFiles[idx]!)
        const dst = join(checkDir, `f${String(i + 1).padStart(2, "0")}-${frameFiles[idx]!}`)
        try {
          const buf = await readFile(src)
          await writeFile(dst, buf)
          picks.push(dst)
        } catch {
          /* 单帧失败不影响整体 */
        }
      }
      lines.push(`  抽帧 ${picks.length} 张（均匀分布）：`)
      for (const p of picks) lines.push(`    ${p}`)
      lines.push(`  提示：用 read 查看这些帧，确认构图/文字/主体是否如你所愿。`)
    } else {
      lines.push(`  未落帧（--keep-frames 没产出？）——只能看 mp4。`)
    }

    // —— 量化 ——
    const quality = parseQualityFromStdout(r.stdout)
    if (quality) {
      lines.push(
        `  量化：静止 ${quality.still_ratio.toFixed(1)}% · 变化密度 ${quality.change_density.toFixed(2)}/s`,
      )
      lines.push(`        过亮帧 ${quality.bright_ratio.toFixed(1)}% · 平均亮度 ${quality.mean_luma.toFixed(0)}`)
      const flags: string[] = []
      if (quality.still_ratio > 40) flags.push(`静止占比 >40%（先缩短镜内静止，或确认这一幕本就该静）`)
      if (quality.bright_ratio > 20) flags.push(`过亮帧 >20%（暗场里嵌白底素材要做多层融合）`)
      if (quality.change_density < 0.5) flags.push(`变化密度 <0.5/s（可能真的卡住了）`)
      if (flags.length) {
        lines.push(`  ⚠ 值得看一眼：`)
        for (const f of flags) lines.push(`    · ${f}`)
      }
      lines.push(`  说明：这些是**参考量不是验收门槛**——静止占比高但主体本就在动是正常的。`)
    }

    return { output: lines.join("\n") }
  },
}

/** 从渲染输出里抠出质检摘要（裁云把 `frame_stats` 的 summary 行打到 stdout）。 */
function parseQualityFromStdout(stdout: string): {
  still_ratio: number
  change_density: number
  bright_ratio: number
  mean_luma: number
} | null {
  // 渲染器打印的真实格式（`frame_stats.cpp` 的 `out.summary`）：
  //   `300 帧｜参考量 变化像素 1.89% / 格均差峰值 70（无及格线，仅供定位）
  //    ｜静止 39.5%（帧差 <1.2，299 对）｜过亮 0.0%（>180）
  //    ｜平均亮度 37.7｜变化 18.10/s`
  // 各指标位置固定但中间的括注长度可变——用**弱约束**逐项抠，不写整行正则：
  // 整行正则一旦格式微调就全失配，而这是纯展示信息，不该因格式变化而消失。
  const pick = (re: RegExp): number | null => {
    const m = stdout.match(re)
    return m ? Number(m[1]) : null
  }
  const still = pick(/静止\s*([\d.]+)%/)
  const density = pick(/变化\s*([\d.]+)\/s/)
  const bright = pick(/过亮\s*([\d.]+)%/)
  const luma = pick(/平均亮度\s*([\d.]+)/)
  if (still === null || density === null) return null
  return {
    still_ratio: still,
    change_density: density,
    bright_ratio: bright ?? 0,
    mean_luma: luma ?? 0,
  }
}

export const productionTools: Record<string, Tool> = {
  script_schema: scriptSchemaTool,
  script_check: scriptCheckTool,
  film_render: filmRenderTool,
}
