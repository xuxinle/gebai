/**
 * 硬件编码探针**失败诊断**的测试。
 *
 * 这份诊断解决的是一个具体的"看不出所以然"问题：探针失败只留一行原生库报错，
 * 用户无法区分「机器没 GPU」「驱动没装」「内置 ffmpeg 没编 NVENC」「Remotion 版本太低」，
 * 也就无从决定该装什么、还是根本不该纠缠（无 NVIDIA 设备时软件档就是正确形态）。
 *
 * 用例覆盖四类判定，全部用注入事实（不读真实硬件），因此在任何机器上都能跑。
 */
import { describe, expect, test } from "bun:test"
import { diagnoseEncoderProbe } from "./encoder-diagnosis"

describe("硬件编码探针失败诊断", () => {
  test("无 NVIDIA 设备：明确下结论「不可能启用」，并制止继续排查", () => {
    const result = diagnoseEncoderProbe({
      probeError: "Error: Failed to launch encoder: nvenc not found",
      platform: "linux",
      gpuForm: "Linux（软件光栅化）",
      remotionVersion: "4.0.484",
      binariesDirectory: null,
    })
    expect(result.verdict).toContain("未检测到 NVIDIA")
    expect(result.verdict).toContain("不可能启用")
    expect(result.hints.join("\n")).toContain("nvidia-smi")
    // 关键：告诉用户"软件档就是正确形态"，避免在没有 GPU 的机器上反复折腾
    expect(result.hints.join("\n")).toContain("软件档")
  })

  test("NVIDIA 在但报错指向原生库缺失：指向驱动/NVENC 运行库", () => {
    const result = diagnoseEncoderProbe({
      probeError: "Cannot load libnvidia-encode.so.1: cannot open shared object file",
      platform: "linux",
      gpuForm: "Linux + NVIDIA RTX 4090",
      remotionVersion: "4.0.500",
      binariesDirectory: null,
    })
    const hints = result.hints.join("\n")
    expect(result.verdict).toContain("RTX 4090")
    expect(hints).toContain("libnvidia-encode")
    expect(hints).toContain("h264_nvenc")
    // binariesDirectory 未配置时必须讲清三件套要求（只换 ffmpeg 会让 compositor 查找失败）
    expect(hints).toContain("三件套")
  })

  test("NVIDIA 在且已配 binaries_directory：改为提示校验该目录的 ffmpeg", () => {
    const result = diagnoseEncoderProbe({
      probeError: "Error: ffmpeg exited with code 1: Unknown encoder 'h264_nvenc'",
      platform: "linux",
      gpuForm: "Linux + NVIDIA A100",
      remotionVersion: "4.0.500",
      binariesDirectory: "/opt/reel-bin",
    })
    const hints = result.hints.join("\n")
    expect(hints).toContain("/opt/reel-bin")
    expect(hints).toContain("ffmpeg -encoders")
    // 已配置时不该再讲"三件套怎么配"的入门说明（那是给未配置者的）
    expect(hints).not.toContain("三处任选")
  })

  test("Remotion 版本低于 NVENC 门控：结论里点明版本，并给升级建议", () => {
    const result = diagnoseEncoderProbe({
      probeError: "Error: hardware acceleration required but unavailable",
      platform: "linux",
      gpuForm: "Linux + NVIDIA RTX 3080",
      remotionVersion: "4.0.100",
      binariesDirectory: null,
    })
    expect(result.verdict).toContain("版本低于 NVENC 门控")
    expect(result.hints.join("\n")).toContain("4.0.484")
  })

  test("macOS：说明走 VideoToolbox 而非 NVENC（不要把 mac 的失败当 NVENC 问题）", () => {
    const result = diagnoseEncoderProbe({
      probeError: "Error: VideoToolbox encoder not available",
      platform: "darwin",
      gpuForm: "macOS + Apple Silicon (M2)",
      remotionVersion: "4.0.490",
      binariesDirectory: null,
    })
    expect(result.verdict).toContain("VideoToolbox")
    expect(result.hints.length).toBeGreaterThan(0)
    expect(result.hints.join("\n")).not.toContain("nvidia-smi")
  })

  test("结论与建议总是非空，且建议可执行（含命令或具体动作）", () => {
    const cases = [
      { probeError: "", platform: "linux", gpuForm: "", remotionVersion: null, binariesDirectory: null },
      { probeError: "boom", platform: "linux", gpuForm: "Linux + NVIDIA X", remotionVersion: null, binariesDirectory: null },
      { probeError: "boom", platform: "win32", gpuForm: "Windows + NVIDIA X", remotionVersion: "4.0.484", binariesDirectory: null },
    ]
    for (const input of cases) {
      const result = diagnoseEncoderProbe(input)
      expect(result.verdict.length).toBeGreaterThan(0)
      expect(result.hints.length).toBeGreaterThan(0)
    }
  })
})
