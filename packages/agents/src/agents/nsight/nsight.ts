/**
 * nsight 子Agent：NVIDIA Nsight 报告分析与代码问题定位。
 *
 * 能力面（全部工具在 tools.ts / ncu-tools.ts / capture.ts）：
 * - 报告分析（只读）：时间线总览、内核热点与下钻、空闲缝与串行化、事件库只读 SQL、问题诊断清单、符号→源码定位；
 * - 单内核深查：SOL/占用率/停顿分解/访存效率/NVIDIA 官方规则（需 .ncu-rep）；
 * - 采集（需审批）：nsys profile / ncu profile，含权限与失败分类诊断。
 *
 * 设计取向：报告解析与聚合全部流式化（内存与报告规模解耦）、结果条数有界、事实按报告指纹缓存，
 * 因此 GB 级报告仍可实时分析；工具链解析与命令构造考虑跨平台（Windows/macOS/Linux 各自的安装位置与 shell 语义）。
 */
import { isAbsolute, resolve } from "node:path"
import type { SubAgentDef } from "@gebai/sdk"
import { projectAware } from "@gebai/sdk/node"
import systemPromptBase from "./nsight.md"
import { analysisTools } from "./tools"
import { kernelDetailTool } from "./ncu-tools"
import { captureTool } from "./capture"

export const name = "nsight"

export const description =
  "NVIDIA Nsight 报告分析与 GPU 性能问题定位（Nsight Systems 时间线 + Nsight Compute 单内核）：解析 .nsys-rep/.ncu-rep 给出瓶颈与问题清单（量化证据 + 根因 + 修复方向），并把报告里的内核/NVTX 符号定位到工程源码的文件:行；支持对目标程序做 nsys/ncu 采集（需审批），超大报告（GB 级、千万级事件）走流式聚合与缓存。" +
  "输入：报告路径或采集需求 + 源码工程。"

export const systemPrompt = systemPromptBase

/** 报告路径常位于工程内，故统一加 project 参数（预置项目名/路径/保留名 tmp）以按根解析。 */
export const tools = {
  doctor: analysisTools.doctor!,
  reports: projectAware(analysisTools.reports!, { workdir: true }),
  overview: projectAware(analysisTools.overview!, { workdir: true }),
  kernels: projectAware(analysisTools.kernels!, { workdir: true }),
  timeline: projectAware(analysisTools.timeline!, { workdir: true }),
  query: projectAware(analysisTools.query!, { workdir: true }),
  findings: projectAware(analysisTools.findings!, { workdir: true }),
  locate: projectAware(analysisTools.locate!, { workdir: true }),
  compare: projectAware(analysisTools.compare!, { workdir: true }),
  kernel_detail: projectAware(kernelDetailTool, { workdir: true }),
  capture: projectAware(captureTool, { workdir: true }),
}

export const requiresApproval = { capture: true }
export const preload = false

export const envVars = [
  { name: "NSIGHT_SYSTEMS_BIN", description: "nsys 可执行文件路径（缺省按安装目录/PATH 自动探测；Windows 通常在 Nsight Systems <版本>\\target-windows-x64\\nsys.exe）" },
  { name: "NSIGHT_COMPUTE_BIN", description: "ncu 可执行文件路径（缺省按安装目录/PATH 自动探测）" },
  { name: "NSIGHT_CACHE_DIR", description: "报告解析缓存根目录（默认 {GEBAI_HOME}/cache/nsight：事件库、CSV 指标页按报告指纹分目录存放）" },
  { name: "NSIGHT_PROJECT", description: "默认源码工程根：未指定 project 参数时以其为基准（报告路径与源码定位的搜索范围）" },
  { name: "NSIGHT_NATIVE", description: "原生聚合后端开关：off 时固定走 JS 流式实现（默认原生优先、不可用自动回退）" },
]

/** 默认工程根兜底：NSIGHT_PROJECT 配置时即视为项目绑定。 */
export const projectRoot = (env: Record<string, string>): string | undefined => {
  const value = env.NSIGHT_PROJECT
  if (!value) return undefined
  return isAbsolute(value) ? value : resolve(process.cwd(), value)
}

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars, projectRoot }
