/**
 * 本地图像生成工具（sd-turbo / SD2.1-Turbo）。
 *
 * ## 为什么是"包一层 Python 脚本"而不是在 TS 里跑推理
 *
 * 推理栈是 PyTorch + diffusers（Python 生态），而 GEBAI 是 TS 宿主。强行用 node 绑定
 * 只会把两边都塞进一个进程、互相拖住。所以边界划在**文件**上：
 * 工具调 `tools/imagegen.py` 出 PNG，产物落进素材目录，裁云/镂月按普通素材使用。
 * 应用不需要知道有模型这回事，换模型/升版本也不影响已编译的应用。
 *
 * ## 为什么默认 2 步
 *
 * sd-turbo 是**蒸馏模型**：1~4 步就该出图，且必须 `guidance_scale=0`（关 CFG）。
 * 按普通 SD 的 30 步/7.5 guidance 跑会得到噪点——这不是"效果差一点"，是完全坏掉。
 * 实测 2 步 512² 约 1.2 秒（RTX 4080 SUPER），3 步 768×512 约 0.29 秒（模型已加载）。
 */
import { spawn } from "node:child_process"
import { existsSync } from "node:fs"
import { mkdir, readdir, stat } from "node:fs/promises"
import { join, resolve } from "node:path"
import type { Tool } from "@gebai/sdk"

function repoRoot(): string {
  // dev 模式路径：<repo>/packages/agents/src/agents/imagegen
  // 到仓库根要上 **5** 层（imagegen → agents → src → agents 包 → packages → repo）。
  // 少一层就会指到 packages/，于是 tools/imagegen.py 找不到 →
  // imagegen_check 报"不可用"而模型其实装好了（实测踩到：误报比不报更费时间）。
  return resolve(import.meta.dirname, "..", "..", "..", "..", "..")
}

function pythonBin(env: Record<string, string> | undefined): string {
  if (env?.GEBAI_IMAGEGEN_PYTHON) return env.GEBAI_IMAGEGEN_PYTHON
  // ⚠ 不能直接返回 "python"：Windows 上 PATH 里的 python 常是
  // **WindowsApps 的商店占位程序**（0 字节的 stub），执行它只会打开微软商店、
  // 不跑脚本也不报错——于是 imagegen_check 得到空输出、误判"环境不可用"（实测踩到）。
  // 先按已知安装位置找真实解释器，再退回 PATH。
  const candidates =
    process.platform === "win32"
      ? [
          join(process.env.LOCALAPPDATA ?? "", "Programs", "Python", "Python312", "python.exe"),
          join(process.env.LOCALAPPDATA ?? "", "Programs", "Python", "Python311", "python.exe"),
          join(process.env.ProgramFiles ?? "", "Python312", "python.exe"),
          join(process.env.ProgramFiles ?? "", "Python311", "python.exe"),
          "py",
        ]
      : ["python3", "python"]
  for (const candidate of candidates) {
    if (candidate === "py" || candidate.startsWith("/") || existsSync(candidate)) return candidate
  }
  return process.platform === "win32" ? "py" : "python3"
}

function defaultModel(env: Record<string, string> | undefined): string {
  const from_env = env?.GEBAI_IMAGEGEN_MODEL
  if (from_env) return from_env
  return join(repoRoot(), "resources", "models", "imagegen", "sd-turbo")
}

/** 默认素材落点：裁云的 media 目录（镂月也读这里，两边共用一批素材）。 */
function defaultAssetDir(): string {
  return join(repoRoot(), "shuangtian", "examples", "caiyun", "assets", "media")
}

interface RunResult {
  code: number | null
  stdout: string
  stderr: string
}

function runPython(args: string[], cwd: string): Promise<RunResult> {
  return new Promise((resolvePromise) => {
    const child = spawn(pythonBin(process.env as Record<string, string>), args, {
      cwd,
      env: { ...process.env, PYTHONIOENCODING: "utf-8" },
    })
    let stdout = ""
    let stderr = ""
    child.stdout.on("data", (chunk) => {
      stdout += String(chunk)
    })
    child.stderr.on("data", (chunk) => {
      stderr += String(chunk)
    })
    child.on("error", (error) => {
      resolvePromise({ code: null, stdout, stderr: `${stderr}${String(error)}` })
    })
    child.on("close", (code) => {
      resolvePromise({ code, stdout, stderr })
    })
  })
}

export const imagegenCheckTool: Tool = {
  name: "imagegen_check",
  description:
    "检查本地图像生成环境是否可用：torch/CUDA/diffusers 版本、模型目录与体积、" +
    "已安装的模型列表。出图失败时先跑它——" +
    "缺什么（CUDA/模型/依赖）会如实报，不要靠猜。",
  parameters: { type: "object", properties: {} },
  requiresApproval: false,
  async execute() {
    const script = join(repoRoot(), "tools", "imagegen.py")
    const check = await runPython([script, "--check"], repoRoot())
    const list = await runPython([script, "--list-models"], repoRoot())
    const ok = check.code === 0
    return {
      output:
        `${ok ? "可用" : "不可用"}\n${check.stdout.trim()}${check.stderr.trim()}\n` +
        `模型：\n${list.stdout.trim()}`,
      data: {
        available: ok,
        detail: `${check.stdout}${check.stderr}`.trim(),
        models: list.stdout.trim(),
        model_dir: defaultModel(process.env as Record<string, string>),
      },
    }
  },
}

export const imagegenTool: Tool = {
  name: "imagegen",
  description:
    "用本地 sd-turbo 模型生成图片（离线、GPU，1~4 步出图，512² 约 1 秒）。" +
    "生成的图直接落进素材库目录，可用 imagegen_assets 列出、在裁云/镂月的海报与视频场景里按名字引用。" +
    "提示词用英文效果更好；不要写「文字/watermark」类要求（模型会画出扭曲字母）。" +
    "需要多张不同风格时逐次调用（每次独立种子），不要指望一次出多张。",
  parameters: {
    type: "object",
    properties: {
      prompt: { type: "string", description: "提示词（英文更佳）。描述画面内容与风格，不要求文字。" },
      name: {
        type: "string",
        description:
          "产品文件名（不含扩展名）。省略则按时间戳自动命名。" +
          "写可读名字（如 bg-tech-01）便于后续在脚本里引用。",
      },
      size: { type: "string", description: "尺寸 WxH，默认 768x512（16:10）；竖版常用 512x768。" },
      steps: { type: "integer", description: "推理步数 1~4（默认 2）。sd-turbo 超过 4 步反而变糊。" },
      seed: { type: "integer", description: "随机种子（同种子 + 同提示词 = 同图，便于复现）。" },
      out_dir: {
        type: "string",
        description: "输出目录（默认素材库 resources 旁的 examples/caiyun/assets/media）。",
      },
      background: {
        type: "boolean",
        description: "true 时模型生成透明/纯色背景图（适合做贴图，而不是照片）。",
      },
    },
    required: ["prompt"],
  },
  requiresApproval: false,
  async execute(input: Record<string, unknown>) {
    const prompt = String(input.prompt ?? "").trim()
    if (!prompt) throw new Error("imagegen 需要 prompt")
    const size = String(input.size ?? "768x512")
    const steps = Math.max(1, Math.min(4, Number(input.steps ?? 2)))
    const seed = input.seed === undefined ? Math.floor(Math.random() * 1_000_000) : Number(input.seed)
    const outDir = input.out_dir ? String(input.out_dir) : defaultAssetDir()
    await mkdir(outDir, { recursive: true })
    const name =
      String(input.name ?? "").trim() || `gen-${Date.now().toString(36)}`
    const outPath = join(outDir, `${name.replace(/[^\w.-]/g, "_")}.png`)

    const script = join(repoRoot(), "tools", "imagegen.py")
    if (!existsSync(script)) {
      throw new Error(`找不到出图脚本：${script}（仓库不完整？）`)
    }
    const args = [
      script,
      "--prompt",
      prompt,
      "--out",
      outPath,
      "--size",
      size,
      "--steps",
      String(steps),
      "--seed",
      String(seed),
      "--model",
      defaultModel(process.env as Record<string, string>),
    ]
    const result = await runPython(args, repoRoot())
    if (result.code !== 0) {
      // 如实报错：环境不可用/模型缺失时给出可操作提示，不吞掉 stderr。
      const detail = `${result.stderr}`.trim() || `${result.stdout}`.trim()
      throw new Error(`图像生成失败：${detail.slice(0, 600)}`)
    }
    const info = await stat(outPath)
    const line = result.stdout
      .split("\n")
      .filter((text) => text.includes(".png"))
      .pop()
    return {
      output: `已生成 ${outPath}（${Math.round(info.size / 1024)} KB）\n${line ?? ""}`,
      data: {
        path: outPath,
        name,
        prompt,
        size,
        steps,
        seed,
        bytes: info.size,
        elapsed: line ?? "",
      },
    }
  },
}

export const imagegenAssetsTool: Tool = {
  name: "imagegen_assets",
  description:
    "列出素材库里的图片（名字/尺寸/大小/路径）。裁云与镂月都用这个目录——" +
    "脚本里按名字引用（`\"image\": \"bg-aurora\"`）即可。生成完新图先看这里确认落盘。",
  parameters: {
    type: "object",
    properties: {
      dir: { type: "string", description: "目录（默认 examples/caiyun/assets/media）。" },
    },
  },
  requiresApproval: false,
  async execute(input: Record<string, unknown>) {
    const dir = input.dir ? String(input.dir) : defaultAssetDir()
    if (!existsSync(dir)) {
      return { output: `素材目录不存在：${dir}`, data: { dir, items: [] } }
    }
    const files = (await readdir(dir)).filter((name) => name.toLowerCase().endsWith(".png"))
    const items = []
    for (const file of files.sort()) {
      const full = join(dir, file)
      const info = await stat(full)
      items.push({
        name: file.replace(/\.png$/i, ""),
        path: full,
        bytes: info.size,
        modified: info.mtime.toISOString(),
      })
    }
    const lines = items.map(
      (item) => `${item.name}  ${Math.round(item.bytes / 1024)} KB  ${item.modified.slice(0, 10)}`,
    )
    return {
      output: `${items.length} 项（${dir}）\n${lines.join("\n")}`,
      data: { dir, items },
    }
  },
}

export const tools: Record<string, Tool> = {
  imagegen: imagegenTool,
  imagegen_check: imagegenCheckTool,
  imagegen_assets: imagegenAssetsTool,
}

export const requiresApproval: Record<string, boolean> = {
  imagegen: false,
  imagegen_check: false,
  imagegen_assets: false,
}
