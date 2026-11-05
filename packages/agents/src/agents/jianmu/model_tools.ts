/**
 * 建木的**制作闭环工具组**：把"翻源码查字段 / 拼命令行出图 / 逐个试场景"收成几次调用。
 *
 * ## 为什么需要这组工具（与裁云那组同一个理由）
 *
 * 建木的交互面（`shuangtian_*`）已经够用，但**写脚本这件事**有三个固定动作：
 *
 * | 固定动作 | 手工做法的代价 |
 * |---|---|
 * | 查"这个三维能力支持到什么程度" | `grep` 三个 `*.cpp`（three_runtime / bpy_runtime / software_scene）才能拼出答案 |
 * | 出图核验（不启应用） | 拼 `three_check` / `align_bench_probe` 的命令行 + 读它的诊断文本 |
 * | 列场景 / 找现成参照 | 记目录路径、猜哪个场景最能体现能力 |
 *
 * 这些**每写一个场景都要重做一遍**，而答案本该由工具回答。
 *
 * ## 关键设计：出图工具走**离线 exe**，不走控制通道
 *
 * 控制通道要应用先启动、要 token、要处理"同一应用只能有一个实例"。
 * 而"脚本 → 一张图 + 判据"是**批处理**，`three_check` 更直接、天然带退出码。
 * 交互式调参（旋转/拾取/热重载）仍走 `shuangtian_*`。
 */

import { spawn } from "node:child_process"
import { existsSync, readdirSync } from "node:fs"
import { isAbsolute, join, resolve } from "node:path"
import type { Tool, ToolContext } from "@gebai/sdk"
import { schema } from "@gebai/sdk/node"

const IS_WINDOWS = process.platform === "win32"

/** 框架根：`SHUANGTIAN_PROJECT` 优先，缺省歌白仓库下的 `shuangtian/`。 */
function frameworkRoot(ctx: ToolContext): string {
  const explicit = String(ctx.env.SHUANGTIAN_PROJECT ?? "").trim()
  if (explicit) return resolve(explicit)
  return resolve(process.cwd(), "shuangtian")
}

/** 框架里的可执行产物（按档位；缺省 dev）。 */
function exePath(ctx: ToolContext, name: string, profile = "dev"): string {
  const suffix = IS_WINDOWS ? ".exe" : ""
  return join(frameworkRoot(ctx), "build", profile, "bin", name + suffix)
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

/** 跑一个子进程并收全部输出（带超时；超时不留孤儿）。 */
function runProc(cmd: string, args: string[], cwd: string, timeoutMs: number): Promise<RunResult> {
  return new Promise((resolvePromise) => {
    const child = spawn(cmd, args, { cwd, windowsHide: true })
    let stdout = ""
    let stderr = ""
    let settled = false
    const timer = setTimeout(() => {
      if (settled) return
      settled = true
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
function requireExe(ctx: ToolContext, path: string, label: string, profile: string): string | null {
  if (existsSync(path)) return null
  return (
    `找不到 ${label}：${path}\n`
    + `先构建：shuangtian_run(action=build, target=${label}, profile=${profile})`
  )
}

// ============================================================ scenes

/**
 * 列出现有场景脚本（含**每个脚本的定位**，不只是文件名）。
 *
 * 为什么要有"定位"：`01-product.js` 与 `bpy_watch.py` 对模型的意义完全不同——
 * 前者是"最小可跑的 three.js 模板"，后者是"bpy 能造出复杂机械件"的证据。
 * 只给文件名，模型只能靠猜去挑参照，猜错就白写一轮。
 */
export const scenesTool: Tool = {
  name: "scenes",
  safeMode: true,
  description:
    "列出建木的场景脚本目录（`examples/jianmu/scenes/`）：每个脚本的扩展名（.js=three.js / .py=bpy）、"
    + "行数、以及**它是哪一类参照**（模板/资产端/组装端/能力证据）。"
    + "写新场景前先看一下现成的，比猜字段便宜得多。"
    + "可选 `dir` 看别的目录。",
  parameters: schema(
    {
      dir: { type: "string", description: "场景目录（相对框架根；缺省 examples/jianmu/scenes）" },
    },
    [],
  ),
  async execute(args, ctx) {
    const rel = String(args.dir ?? "").trim() || "examples/jianmu/scenes"
    const dir = isAbsolute(rel) ? rel : join(frameworkRoot(ctx), rel)
    if (!existsSync(dir)) return { output: `目录不存在：${dir}` }
    const names = readdirSync(dir)
      .filter((n) => n.endsWith(".js") || n.endsWith(".py"))
      .sort()
    if (names.length === 0) return { output: `目录里没有 .js / .py 场景：${dir}` }

    // 定位靠**文件名约定**（`*_asset.py` = 资产端、`*_assembly.js` = 组装端），
    // 而不是读文件猜内容：约定是稳定的，内容判断会随实现变化。
    const roleOf = (n: string): string => {
      if (n.endsWith("_asset.py")) return "资产端（产几何，配合 inline_assets 组装）"
      if (n.endsWith("_assembly.js")) return "组装端（读资产 JSON 摆位/材质/相机）"
      if (n.endsWith(".py")) return "bpy 场景（建模：bmesh/bpy.ops）"
      return "three.js 场景（组装/布光/动画）"
    }
    const lines = [`场景目录：${dir}`, ""]
    for (const n of names) {
      lines.push(`  · ${n.padEnd(26)} ${roleOf(n)}`)
    }
    lines.push("")
    lines.push("跑起来：shuangtian_run(action=start, target=jianmu)，默认打开 04-watch.js；")
    lines.push("  切场景：script {\"code\":\"app.lab.select(<下标>)\"}（下标 = 上面列表的顺序，从 0 起）")
    lines.push("  ⚠ 清单名带扩展名：同名两种写法（quad.js / quad.py）要靠扩展名区分")
    return { output: lines.join("\n") }
  },
}

// ============================================================ render_check

/**
 * 离线出图 + 判据：**一次调用**完成"校验脚本 → 渲染一张 PNG → 给诊断"。
 *
 * 取代的手工流程：拼 `three_check --three x.js --png out.png`、读它的
 * `PASS/FAIL` 与诊断文本、再自己判断"这算通过吗"。
 *
 * ⚠ 出图工具**认扩展名**吗：`three_check` 只吃 three.js 脚本；bpy 脚本走
 * `align_bench_probe`（它自动识别 `.js` / `.py`，两条表达层同一口径出图）。
 * 本工具按扩展名自动选——调用方不需要记住两个二进制。
 */
export const renderCheckTool: Tool = {
  name: "render_check",
  safeMode: false,
  description:
    "离线出图核验：给任何 `.js`（three.js）或 `.py`（bpy）脚本渲一张 PNG，并回判据"
    + "（PASS/FAIL + 覆盖率/亮度诊断）。**不开应用、不用控制通道**，秒级。"
    + "用于「写完之后先确认不是空画面/相机不在物体里」，再决定要不要起界面调。",
  parameters: schema(
    {
      script: { type: "string", description: "脚本路径（相对会话工作目录或绝对路径）" },
      out: { type: "string", description: "输出 PNG（缺省会话目录下 render-check/<脚本名>.png）" },
      size: { type: "string", description: "分辨率 WxH（three_check 支持，缺省 480x320）" },
      profile: { type: "string", description: "构建档位（缺省 dev）" },
    },
    ["script"],
  ),
  async execute(args, ctx) {
    const profile = String(args.profile ?? "dev").trim() || "dev"
    const scriptPath = inWorkdir(ctx, String(args.script ?? "").trim())
    if (!existsSync(scriptPath)) return { output: `脚本文件不存在：${scriptPath}` }

    const isBpy = scriptPath.toLowerCase().endsWith(".py")
    const outArg = String(args.out ?? "").trim()
    const outPath = outArg ? inWorkdir(ctx, outArg) : inWorkdir(ctx, join("render-check", (isBpy ? "bpy" : "three") + ".png"))

    if (isBpy) {
      // `align_bench_probe`：自动识别 .js/.py，固定 1100x800，输出 PNG。
      const exe = exePath(ctx, "align_bench_probe", profile)
      const missing = requireExe(ctx, exe, "align_bench_probe", profile)
      if (missing) return { output: missing }
      await mkdirOf(outPath)
      const r = await runProc(exe, [scriptPath, outPath], frameworkRoot(ctx), 120_000)
      const ok = r.code === 0 && existsSync(outPath)
      return {
        output:
          (ok ? `✓ bpy 场景出图成功：${outPath}\n` : `✗ bpy 场景出图失败（退出码 ${r.code}）\n`)
          + `${(r.stdout + r.stderr).slice(-1500)}`,
      }
    }

    // three.js：`three_check` 自带"空画面 / 相机在物体里 / 物体全在框外"的诊断。
    const exe = exePath(ctx, "three_check", profile)
    const missing = requireExe(ctx, exe, "three_check", profile)
    if (missing) return { output: missing }
    await mkdirOf(outPath)
    const argv = [exe, "--three", scriptPath, "--png", outPath]
    if (String(args.size ?? "").trim()) argv.push("--size", String(args.size).trim())
    const r = await runProc(argv[0], argv.slice(1), frameworkRoot(ctx), 120_000)
    const text = (r.stdout + r.stderr).trim()
    const ok = r.code === 0
    return {
      output:
        (ok ? `✓ three.js 场景通过核验：${outPath}\n` : `✗ 场景有问题（退出码 ${r.code}）\n`)
        + `${text.slice(-2000)}`,
    }
  },
}

async function mkdirOf(path: string): Promise<void> {
  const { mkdir } = await import("node:fs/promises")
  const dir = path.replace(/[\\/][^\\/]*$/, "")
  if (dir) await mkdir(dir, { recursive: true }).catch(() => {})
}

// ============================================================ capability

/**
 * 两条表达层的**能力边界**：回答"这个能力到底支持吗、写到什么程度"。
 *
 * 为什么这不是一句文档能解决的：三维的失败几乎都不报错，而"我不确定它支持不支持"
 * 会让模型反复试（写三四种写法、每次都要起应用看结果）。把边界做成**可查的表**，
 * 试错就变成查表。
 *
 * 数据来源：`docs/three3d.md` 的语义翻译表、`docs/python.md` 的已知边界、
 * `docs/render_alignment.md` 的量化结论。本工具只**摘录判据**，不复制细节。
 */
export const capabilityTool: Tool = {
  name: "capability",
  safeMode: true,
  description:
    "查三维能力的边界：某个 three.js / bpy 能力**支持还是不支持**、以及「画了但看不见」「看着假」"
    + "这类症状的排查顺序。可选 `topic` 过滤（three / bpy / look / perf）。"
    + "写不确定的字段之前先查，比试错便宜。",
  parameters: schema(
    {
      topic: {
        type: "string",
        description: "过滤主题：three（three.js 面）/ bpy（bpy 面）/ look（观感与「假」）/ perf（性能）",
      },
    },
    [],
  ),
  async execute(args) {
    const topic = String(args.topic ?? "").trim().toLowerCase()
    const sections: Record<string, string[]> = {
      three: [
        "three.js 表达层（.js）",
        "  ✓ 用：Scene/Group/Object3D、全部内置几何生成器、Extrude/Lathe/Tube/Shape+holes、",
        "       Vector2/3、Quaternion、Euler、Matrix4、Box3、Perspective/OrthographicCamera、",
        "       Directional/Point/Spot/Ambient/HemisphereLight、InstancedMesh、AnimationMixer",
        "  ✓ 材质：MeshStandard/Physical/Phong/Lambert 的 color/metalness/roughness/opacity/",
        "       emissive/side/depthWrite；MeshBasic 映射为「自发光 + unlit」",
        "  ✗ 不用：WebGLRenderer（渲染是本框架的 Scene3D）、TextureLoader/ImageLoader/document、",
        "       GLTFLoader/OBJLoader/FBXLoader（外部模型走 JSON 的 gltf 字段）",
        "  ✗ 多材质网格只取第一个材质 → 需要多材质请拆成多个 Mesh",
        "  ✗ SkinnedMesh 的骨骼动画：网格按当前姿态抽出，不是时间轴驱动",
        "",
        "  语义翻译（最容易踩）：",
        "   · 平行光方向是「position → target 射出」，本框架口径是「指向光源」→ 已取反",
        "   · opacity 需要 transparent: true 才混合；没设会被拉回 1.0",
        "   · Color 的 toArray() 已是线性值 → 不要再解码一次（会整体暗 2 倍）",
        "   · 坐标系两边都是右手系 +Y 上、相机看 -Z，无需翻转",
      ],
      bpy: [
        "bpy 兼容层（.py）：MicroPython + Python 实现的 bpy 语义层",
        "  ✓ 用：bpy.data（objects/meshes/materials/cameras/lights/world）、bpy.ops.mesh.primitive_*、",
        "       bmesh（逐顶点建面）、mathutils（Vector/Euler/Matrix）",
        "  ✓ 坐标系是 Z-up（Blender 习惯），Y-up 转换在抽取层做完",
        "  ✓ 色调映射来源默认 AgX（与 Blender 4.x 默认一致）",
        "  ✗ 不是完整 CPython：没有 pip、没有 C 扩展、没有文件系统与网络",
        "  ✗ 已知几何缺陷：primitive_uv_sphere_add 的顶点 x 会超出半径（球被压扁）→",
        "       要精确的球请用 three.js 侧，或用 bmesh 自己建（见 docs/python.md）",
        "  ⚠ 比 js 慢一个数量级（MicroPython + 抽取）：改完 .py 别立刻断言「没变化」",
        "",
        "  擅长 / 不擅长：",
        "   擅长：参数化几何（齿形/轮辐/开孔）、布尔、倒角、挤出 —— bmesh 逐顶点",
        "   不擅长：布局与层级、动画、材质变体 —— 那些交给 three.js 组装",
      ],
      look: [
        "「画了但看不见」的排查顺序（按命中率排）：",
        "  ① 相机在物体里 / 物体全在取景框外 → 读 viewport 的 camera 与 distance",
        "  ② far_z 太小（大场景经典坑：取景距离 122 而 far_z 默认 100 → 整个场景被裁）",
        "  ③ 光方向反了 → 全黑",
        "  ④ scene.add(...) 漏了 / visible=false",
        "  ⑤ 颜色重复解码 → 偏暗，看着像「配色问题」",
        "",
        "「看着假」的常见根因：",
        "  · 金属没有环境反射 → 一片死黑像塑料（给 scene.environmentIntensity 或世界背景）",
        "  · 逐三角形平面法线 → 金属没有方向性，画面上是均匀色块",
        "  · 纯 Lambert、没有高光 → 像贴纸（Material.specular 默认 0，要显式打开）",
        "  · emission_unlit=true 的语义是「丢掉全部受光项」→ 拿它做高亮会变纯色平面",
      ],
      perf: [
        "性能口径：",
        "  · 渲染两条腿：有 D3D11 走显卡，否则软件光栅器（同一份脚本同一个长相）",
        "  · bpy 场景：MicroPython 求值 + JSON 抽取，数百毫秒到数秒（与物体数线性相关）",
        "  · three.js 场景：编译 + 求值约 55ms（three.js 0.160 内嵌），每帧推进约 54µs",
        "  · 顶点数比物体数更能预测耗时；InstancedMesh 是大量重复物体的正解",
      ],
    }
    const chosen = topic && sections[topic] ? [topic] : Object.keys(sections)
    if (topic && !sections[topic]) {
      return { output: `未知主题 ${topic}；可用：${Object.keys(sections).join(" / ")}` }
    }
    const lines: string[] = []
    for (const key of chosen) {
      lines.push(...sections[key], "")
    }
    lines.push("细节出处：docs/three3d.md（three.js 表达层与翻译表）、docs/python.md（bpy 边界）、")
    lines.push("          docs/render_alignment.md（与真 three.js / Blender 的量化差异）")
    return { output: lines.join("\n") }
  },
}

export const modelTools: Record<string, Tool> = {
  scenes: scenesTool,
  render_check: renderCheckTool,
  capability: capabilityTool,
}
