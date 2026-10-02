import { isAbsolute, resolve } from "node:path"
import type { SubAgentDef } from "@gebai/sdk"
import { BINARIES_DIR_ENV, BROWSER_EXECUTABLE_ENV } from "./external"
import { projectTool } from "./project"
import { renderTool } from "./render"
import { setupTool } from "./setup"
import { voiceTool } from "./voice"
// 系统提示词独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./reel.md"

export const name = "reel"
export const description =
  "产品视频制作（电影感宣传片/demo reel，也支持单镜头动效复刻）：把前端项目/网页/桌面产品做成成片——分镜、2.5D 真实页面运镜、节奏卡点与声音设计，配音与字幕全程本地（离线语音引擎 + 字幕烧入与 SRT 交付，不联网）。创作能力内置于本包（设计 token、18 个镜头原语、2.5D 相机、时间线骨架），reel_project init 落位成可编辑工程。浏览器可执行文件与原生二进制目录可配置（参数/环境变量/.reel.json），内网/离线不依赖联网下载。" +
  "输入：产品/页面/素材与视频需求；输出：成片、静帧/预览与渲染档报告。"
export const systemPrompt = systemPromptBase
export const tools = {
  setup: setupTool,
  project: projectTool,
  render: renderTool,
  voice: voiceTool,
}
export const preload = false

export const envVars = [
  { name: "REEL_LIBRARY_DIR", description: "视频制作库根目录（默认 {GEBAI_HOME}/vendor/reel：runtime/ 共享 Remotion 运行时、state/ 实测调优与渲染作业）" },
  { name: "REEL_SHARED_RUNTIME", description: "显式指定可复用的 Remotion 运行时目录（内含 node_modules）：缺省会扫描 {GEBAI_HOME}/vendor/<其他库根>/runtime，发现同版本即目录联接复用；本子Agent 不依赖任何具体库根，无命中则自主安装" },
  { name: "REEL_PROJECT", description: "默认视频工程目录：未指定 project 参数的渲染与工程操作以它为基准（会话级环境变量同样生效）" },
  { name: "REEL_GPU", description: "GPU 策略：auto（默认，探测到可用 GPU 即启用硬件编码与 GPU 光栅化）/ off（强制软件档）" },
  {
    name: BROWSER_EXECUTABLE_ENV,
    description:
      "浏览器可执行文件（Chrome/Chromium 路径）：指定后渲染不再查缓存、不联网下载（离线/内网环境必配）。也可写入 .reel.json 的 browserExecutable，或用 reel_render 的 chrome_executable 参数（参数 > 环境变量 > 清单）",
  },
  {
    name: BINARIES_DIR_ENV,
    description:
      "原生二进制目录（目录内需含 remotion/ffmpeg/ffprobe）：整体替换 Remotion 内置 compositor 与 ffmpeg，用于换用带硬件编码器的 ffmpeg 构建。也可写入 .reel.json 的 binariesDirectory，或 reel_render 的 binaries_directory 参数",
  },
  {
    name: "GEBAI_REEL_BROWSER_TIMEOUT_MS",
    description:
      "渲染准备阶段「浏览器」子阶段时限（毫秒，默认 180000）：超时即报出浏览器现状与修复动作，不再静默挂着；内网慢网或首次下载可放宽",
  },
  {
    name: "GEBAI_REEL_BUNDLE_TIMEOUT_MS",
    description: "渲染准备阶段「打包」子阶段时限（毫秒，默认 300000）：工程过大时放宽",
  },
  { name: "TTS_VOICE", description: "配音默认音色（reel_voice 未传 voice 时使用；可写完整名或其一词，如 慧慧）" },
  { name: "TTS_ENGINE", description: "配音语音引擎：auto（默认，WinRT 优先 SAPI 回退）/ winrt / sapi" },
]

/** 默认工程根兜底：REEL_PROJECT 配置时即判定为项目绑定（提示词注记、子会话工作目录、文件工具默认根同源）。 */
export const projectRoot = (env: Record<string, string>): string | undefined => {
  const value = env.REEL_PROJECT
  if (!value) return undefined
  return isAbsolute(value) ? value : resolve(process.cwd(), value)
}

export const def: SubAgentDef = { name, description, systemPrompt, tools, preload, envVars, projectRoot }
