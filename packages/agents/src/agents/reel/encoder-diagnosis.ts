/**
 * 硬件编码探针失败的**可操作诊断**。
 *
 * 背景：`bench` 的硬件编码探针（`hardwareAcceleration=required` 真渲一帧）失败时，
 * 此前只把 Remotion 的原始报错截断 400 字符存进调优缓存。于是「探针未通过」这件事
 * 只有报错、没有结论——用户看到的是类似
 * `Cannot load libnvidia-encode.so.1 …` 的一行原生库错误，无法判断：
 * 是机器没有 GPU？驱动没装？还是内置 ffmpeg 没编译 NVENC？该装什么、还是根本不可能？
 *
 * 这里把「已知事实 + 原始报错」映射成**结论 + 可执行建议**（纯函数，单测直接覆盖）。
 * 判定只依赖调用方给的事实，不读本机环境——测试因此与真实硬件无关。
 */

import { NVENC_MIN_VERSION, compareVersion, parseVersion } from "./profile"

export interface EncoderDiagnosisInput {
  /** 探针原始报错（Remotion 抛出）。 */
  probeError: string
  platform: string
  /** 探测结论原文（`profile.gpuForm`）：含 `NVIDIA <型号>` 即探测到了 NVIDIA GPU。 */
  gpuForm?: string | null
  /** 已探测到的 Remotion 版本（null = 未知，不做版本门控判断）。 */
  remotionVersion?: string | null
  /** 已配置的原生二进制目录（需含 remotion/ffmpeg/ffprobe 三件套）；null = 用 Remotion 包内自带。 */
  binariesDirectory?: string | null
}

export interface EncoderDiagnosis {
  /** 一句话结论：这台机器**能不能**启用硬件编码。 */
  verdict: string
  /** 可执行建议（按优先级；空数组表示无建议可给）。 */
  hints: string[]
}

/** 报错里指向"驱动/原生库缺失"的关键词。 */
const DRIVER_PATTERNS = ["libnvidia", "nvcuda", "nvenc", "cannot load", "cannot open shared object", "no such file", "driver"]
/** 报错里指向"驱动在但设备/会话不可用"的关键词。 */
const DEVICE_PATTERNS = ["no capable devices", "not available", "openencodesessionex", "no device"]

const BINARIES_HINT =
  "该目录必须是**三件套齐备**（remotion + ffmpeg + ffprobe）——只换 ffmpeg 会让 compositor 查找失败" +
  "（`binaries_directory` / `GEBAI_REEL_BINARIES_DIR` / `.reel.json` 的 `binariesDirectory` 三处任选）"

const CACHE_HINT = "结论会写入本机调优缓存（`reel_render` 的 bench），渲染按软件档回退，不会因探针失败中断"

export function diagnoseEncoderProbe(input: EncoderDiagnosisInput): EncoderDiagnosis {
  const error = (input.probeError ?? "").toLowerCase()
  const gpuForm = input.gpuForm ?? ""
  const hasNvidia = /nvidia/i.test(gpuForm)
  const version = parseVersion(input.remotionVersion ?? null)
  const versionTooLow = version !== null && compareVersion(version, NVENC_MIN_VERSION) < 0

  if (input.platform === "darwin") {
    return {
      verdict: "macOS 的硬件编码走 VideoToolbox（不是 NVENC），探针失败通常与 Remotion 版本或系统媒体栈有关",
      hints: [
        `确认 Remotion ≥ ${NVENC_MIN_VERSION.join(".")}（版本未知时先跑一次 bench 记录）`,
        "仍失败就按软件档（x264）渲染——macOS 上 VideoToolbox 的收益远小于 NVENC，不值得纠缠",
      ],
    }
  }

  if (!hasNvidia) {
    return {
      verdict: "本机未检测到 NVIDIA GPU/驱动 → NVENC 不可能启用（硬件/驱动缺失，不是配置问题）",
      hints: [
        "确认机器有 NVIDIA 显卡且驱动已装（`nvidia-smi` 能正常输出）；容器里还需映射 GPU 设备与驱动库",
        "无 NVIDIA 设备时软件档（x264）就是正确形态，不必继续排查探针",
      ],
    }
  }

  // 探测到 NVIDIA 但探针仍失败：版本门控 / 原生库或 ffmpeg 构建缺 NVENC
  const hints: string[] = []
  if (versionTooLow) {
    hints.push(`工程内 Remotion ${input.remotionVersion} 低于 NVENC 最低版本 ${NVENC_MIN_VERSION.join(".")}：升级后再跑 bench`)
  }
  const driverLike = DRIVER_PATTERNS.some((pattern) => error.includes(pattern))
  const deviceLike = DEVICE_PATTERNS.some((pattern) => error.includes(pattern))
  if (driverLike && !deviceLike) {
    hints.push("驱动/原生库未就位：确认 `libnvidia-encode`（NVENC 运行库）可被加载；容器内需映射 /dev/nvidia*")
  }
  hints.push("内置 ffmpeg 常不含 h264_nvenc（@remotion/compositor-* 的构建差异）：用 `binaries_directory` 指向带 h264_nvenc 的 ffmpeg 目录")
  hints.push(
    input.binariesDirectory
      ? `已配置 binaries_directory=${input.binariesDirectory}：确认其 ffmpeg 确实带 h264_nvenc（\`ffmpeg -encoders | grep nvenc\`）`
      : BINARIES_HINT,
  )
  hints.push(CACHE_HINT)

  const verdict = versionTooLow
    ? `检测到 ${gpuForm} 但 Remotion 版本低于 NVENC 门控 → 当前按软件档`
    : `检测到 ${gpuForm} 但硬件编码探针失败（驱动或 ffmpeg 构建缺 NVENC 支持）→ 当前按软件档`
  return { verdict, hints }
}
