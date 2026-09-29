/**
 * reel_project：视频工程脚手架与依赖。
 * - init：把内置模板落位成真实可编辑文件（含可直接渲染的示例片），并把共享运行时的 node_modules 以**目录联接**接入
 *   ——依赖整机只装一次，第二次 init 近乎瞬时。
 * - install：确保共享运行时依赖就绪（可 force 重装）并重建项目联接。
 * - status：工程、依赖、Chrome 缓存、实测调优与最近作业一览。
 */
import { existsSync, lstatSync, mkdirSync, readdirSync, readlinkSync, statSync, symlinkSync } from "node:fs"
import { join, relative } from "node:path"
import type { Tool, ToolContext, ToolResult } from "@gebai/sdk"
import { schema } from "@gebai/sdk/node"
import { ensureRuntime, materializeTemplate, readRuntimeLock } from "./library"
import { TEMPLATE_SIGNATURE } from "./template.generated"
import { browserReadiness, expectedChromeVersion, resolveBinariesDirectory, resolveBrowserExecutable } from "./external"
import { describeJob, listJobs, readTuning } from "./jobs"
import { detectEntryPoint, dirStats, readProjectManifest, writeProjectManifest } from "./runtime"
import { resolveProjectDir, runtimeDir } from "./paths"

function bytesText(bytes: number): string {
  if (bytes > 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024 / 1024).toFixed(2)}GB`
  return `${(bytes / 1024 / 1024).toFixed(1)}MB`
}

/** 把共享运行时的 node_modules 以目录联接接入工程（已有独立安装时保留不动）。 */
function linkRuntime(projectDir: string, runtimeRoot: string): { linked: boolean; note: string } {
  const source = join(runtimeRoot, "node_modules")
  const target = join(projectDir, "node_modules")
  if (!existsSync(source)) return { linked: false, note: `共享运行时依赖缺失（${source}）——先执行 action=install` }
  try {
    const st = lstatSync(target)
    if (st.isSymbolicLink()) return { linked: true, note: "已联接共享运行时依赖" }
    return { linked: false, note: "工程已有独立 node_modules（保留，未覆盖）" }
  } catch {
    /* 不存在则创建联接 */
  }
  try {
    if (process.platform === "win32") {
      symlinkSync(source, target, "junction")
    } else {
      symlinkSync(source, target, "dir")
    }
    return { linked: true, note: `已联接共享运行时依赖（${process.platform === "win32" ? "junction" : "symlink"} → ${source}）` }
  } catch (err) {
    return { linked: false, note: `依赖联接失败：${(err as Error).message}——可改用 REEL_LIBRARY_DIR 换库根位置` }
  }
}

/**
 * 浏览器就绪摘要：配置了可执行文件就一行；未配置时按两种 Chrome 形态各报一行
 * （用哪种由渲染档决策，"这条路走不走得通"要能在开工前看见）。
 */
function browserSummary(ctx: ToolContext, projectDir: string, browserExecutable: string | null): string[] {
  const expectedVersion = expectedChromeVersion(runtimeDir(ctx))
  const alsoFrom = [projectDir, runtimeDir(ctx)]
  if (browserExecutable) {
    const state = browserReadiness({ mode: "headless-shell", browserExecutable, expectedVersion, alsoFrom })
    return [`  浏览器：${state.ready ? "就绪" : "未就绪"} —— ${state.note}`]
  }
  return (["headless-shell", "chrome-for-testing"] as const).map((mode) => {
    const state = browserReadiness({ mode, expectedVersion, alsoFrom })
    return `  浏览器（${mode}）：${state.ready ? "就绪" : "未就绪"} —— ${state.note}`
  })
}

export const projectTool: Tool = {
  name: "project",
  description:
    "视频工程脚手架与运行时：init 建工程（落位内置模板——含设计 token、镜头原语、2.5D 相机、时间线与可直接渲染的示例片——并串联共享运行时依赖，依赖整机只装一次）、install（确保/重建运行时依赖）、status（工程、依赖、Chrome 缓存、实测调优与最近渲染作业）。",
  parameters: schema(
    {
      action: { type: "string", enum: ["init", "install", "status"], description: "init 建工程 / install 准备依赖 / status 查看状态" },
      path: { type: "string", description: "视频工程目录（绝对路径，或相对会话工作目录；缺省用 REEL_PROJECT）" },
      force: { type: "boolean", description: "init：目录已有工程时强制覆盖模板文件（改动会被覆盖，慎用）；install：强制重装共享运行时依赖" },
    },
    ["action"],
  ),
  outputSchema: {
    type: "object",
    properties: {
      projectDir: { type: "string" },
      entryPoint: { type: "string" },
      actions: { type: "array", items: { type: "string" } },
    },
  },
  requiresApproval: true,
  async execute(args, ctx): Promise<ToolResult> {
    const action = String(args.action ?? "")
    const projectDir = resolveProjectDir(ctx, args.path ? String(args.path) : undefined)
    const runtimeRoot = runtimeDir(ctx)
    const actions: string[] = []

    if (action === "init") {
      const manifest = readProjectManifest(projectDir)
      const hasSource = existsSync(join(projectDir, "src", "index.ts"))
      if (!args.force && (manifest || hasSource)) {
        return {
          output: [
            `工程目录已存在内容（${projectDir}）——未做改动。`,
            manifest ? `  已有工程清单：入口点 ${manifest.entryPoint ?? "?"} · 模板签名 ${manifest.templateSignature ?? "?"}` : `  检测到 ${join(projectDir, "src", "index.ts")}`,
            "",
            "如需用模板覆盖：action=init force=true（**会覆盖模板文件**，你自己的镜头实现若与模板同名会被替换，建议先复制备份）。",
            "若只是补依赖：action=install。",
          ].join("\n"),
        }
      }

      const ensured = await ensureRuntime(ctx)
      actions.push(...ensured.actions)
      if (!ensured.ok) {
        return { output: `共享运行时准备失败（依赖未就绪，模板未落位）：${ensured.error ?? "未知原因"}\n\n${actions.join("\n")}` }
      }

      const written = materializeProject(projectDir)
      actions.push(`模板落位：${written.count} 个文件 → ${projectDir}`)
      ensureProjectDirs(projectDir)

      const link = linkRuntime(projectDir, runtimeRoot)
      actions.push(link.note)

      const entryPoint = detectEntryPoint(projectDir)
      // 清单里存**相对路径**：绝对路径会让工程被复制/移动后仍指向原目录的源码，
      // 渲染静默出旧片（实测踩过——产物看上去正常，内容却是另一个工程的）。
      const entryRel = relative(projectDir, entryPoint)
      writeProjectManifest(projectDir, {
        entryPoint: entryRel.startsWith("..") ? entryPoint : entryRel,
        // 外部件配置属于本机环境（与脚手架无关）：重复 init 时保留
        ...(manifest?.browserExecutable ? { browserExecutable: manifest.browserExecutable } : {}),
        ...(manifest?.binariesDirectory ? { binariesDirectory: manifest.binariesDirectory } : {}),
        source: "builtin-template",
        templateSignature: TEMPLATE_SIGNATURE,
        createdAt: new Date().toISOString(),
      })

      const lines = [
        `工程已就绪：${projectDir}`,
        ...actions.map((a) => `  - ${a}`),
        "",
        `入口点：${entryPoint} · 合成：Reel（1920×1080 / 30fps / 20s 示例片）`,
        "",
        "下一步：",
        "  1) 出静帧自检：reel_render action=still frame=90（示例片可直接渲染，先确认链路通）",
        "  2) 按产品改造：src/film/theme.ts（色板/字体/缓动，从产品自身提取）→ src/film/timeline.ts（镜头窗口与文案）→ src/film/scenes/（逐镜实现）",
        "  3) 素材放 public/（截图 2–4 倍分辨率；音频放 public/audio/ 并在 timeline.SFX 登记）",
        "  4) 制作流程与纪律见本子Agent系统提示词（模式判断 → 阶段 0–7 → 独立终检）",
      ]
      return { output: lines.join("\n"), data: { projectDir, entryPoint, actions } }
    }

    if (action === "install") {
      const ensured = await ensureRuntime(ctx, { force: args.force === true })
      actions.push(...ensured.actions)
      if (!ensured.ok) return { output: `共享运行时依赖准备失败：${ensured.error ?? "未知原因"}\n\n${actions.join("\n")}` }
      const link = linkRuntime(projectDir, runtimeRoot)
      actions.push(link.note)
      return {
        output: [`依赖已就绪（${runtimeRoot}）`, ...actions.map((a) => `  - ${a}`)].join("\n"),
        data: { projectDir, actions },
      }
    }

    if (action === "status") {
      const manifest = readProjectManifest(projectDir)
      const runtimeLock = readRuntimeLock(ctx)
      const externalNotes: string[] = []
      let browserExec: string | null = null
      let binariesDir: string | null = null
      try {
        browserExec = resolveBrowserExecutable({ ctx, projectDir }).path
      } catch (err) {
        externalNotes.push((err as Error).message)
      }
      try {
        binariesDir = resolveBinariesDirectory({ ctx, projectDir }).path
      } catch (err) {
        externalNotes.push((err as Error).message)
      }
      // Chrome 缓存统计按**实际命中的根**（Remotion 规则根 / 工程目录 / 共享运行时），与渲染的继承口径一致
      const cacheState = browserReadiness({
        mode: "headless-shell",
        browserExecutable: browserExec,
        expectedVersion: expectedChromeVersion(runtimeRoot),
        alsoFrom: [projectDir, runtimeRoot],
      })
      const chromeStats = dirStats(cacheState.cacheRoot)
      const tuning = readTuning(ctx)
      const jobs = listJobs(ctx, 5)
      const lines: string[] = [`工程：${projectDir}`]
      if (manifest) {
        lines.push(`  模板签名：${manifest.templateSignature ?? "?"}${manifest.templateSignature === TEMPLATE_SIGNATURE ? "" : "（与当前内置模板不一致——重新 init force=true 可更新脚手架）"}`)
        lines.push(`  入口点：${manifest.entryPoint ?? detectEntryPoint(projectDir)}`)
      } else {
        lines.push(`  （未找到 .reel.json 工程清单——先执行 action=init）`)
      }
      let depNote = "未接入"
      try {
        const st = lstatSync(join(projectDir, "node_modules"))
        depNote = st.isSymbolicLink() ? `目录联接 → ${readlinkSync(join(projectDir, "node_modules"))}` : "独立安装"
      } catch {
        /* 未接入 */
      }
      lines.push(`  依赖：${depNote}`)
      if (existsSync(join(projectDir, "src"))) {
        const scandir = readdirSync(join(projectDir, "src", "film", "scenes"), { withFileTypes: true }).filter((d) => d.isFile()).length
        lines.push(`  镜头文件：src/film/scenes/ 下 ${scandir} 个`)
      }
      lines.push(`共享运行时：${runtimeLock ? `${runtimeLock.status}（Remotion ${runtimeLock.remotionVersion ?? "?"} · ${runtimeLock.packageManager ?? "?"}）` : "未安装"}`)
      lines.push(`Chrome 缓存：${chromeStats.files ? `已就绪 ${bytesText(chromeStats.bytes)}（${cacheState.cacheRoot}）` : `未下载（首次渲染时自动下载到 ${cacheState.cacheRoot}）`}`)
      for (const line of browserSummary(ctx, projectDir, browserExec)) lines.push(line)
      lines.push(
        binariesDir ? `原生二进制目录：${binariesDir}（替换内置 compositor/ffmpeg）` : "原生二进制目录：未配置（用项目内 @remotion/compositor-*）",
      )
      for (const note of externalNotes) lines.push(`⚠ ${note}`)
      const probeSummary = tuning.encoderProbe
        ? ` · 硬件编码实测：${tuning.encoderProbe.hardware ? "通过" : (tuning.encoderProbe.diagnosis?.verdict ?? `未通过（${tuning.encoderProbe.error ?? ""}）`)}`
        : ""
      lines.push(`调优缓存：${Object.keys(tuning.entries).length} 条实测${probeSummary}`)
      if (jobs.length) {
        lines.push("最近渲染作业：")
        for (const job of jobs) lines.push(`  ${describeJob(job).split("\n")[0]}`)
      }
      return { output: lines.join("\n"), data: { projectDir, actions } }
    }

    return { output: `未知动作：${action}（可用：init / install / status）` }
  },
}

/**
 * 展开内置模板到工程目录（真实文件），返回写入统计。
 * `public/textures`、`public/audio` 与 `out/` 由 `ensureProjectDirs` 另建（空目录不在模板文件表里）。
 */
function materializeProject(projectDir: string): { count: number; bytes: number } {
  const written = materializeTemplate(projectDir)
  let bytes = 0
  for (const rel of written) {
    try {
      bytes += statSync(join(projectDir, rel)).size
    } catch {
      /* 忽略统计失败 */
    }
  }
  return { count: written.length, bytes }
}

/** 建工程时补齐模板文件表之外的必要空目录（素材与产物落点）。 */
function ensureProjectDirs(projectDir: string): void {
  for (const rel of ["public/textures", "public/audio", "out"]) mkdirSync(join(projectDir, rel), { recursive: true })
}
