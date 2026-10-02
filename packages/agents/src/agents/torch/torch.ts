/**
 * torch 子Agent：PyTorch Profiler（Chrome Trace / Kineto）trace 分析与代码问题定位。
 *
 * 报告面（全部工具在 torch-tools.ts）：
 * - trace 索引（reports：列出工作目录/工程内的 trace 与事实缓存状态）；
 * - 总览（事件规模与采集开关、CPU/GPU 忙碌占比、前向/反向拆分、步级耗时与抖动、算子/内核/CUDA API 排行、显存峰值与碎片率、用户代码热点）；
 * - 算子/内核下钻（按名称筛选、张量形状与 dtype、内核几何、内核→发起算子归属（correlation / 流事件）与发起 Python 位置）；
 * - 显存分析（峰值已分配/已保留、碎片率、最大单次分配、按设备分布）；
 * - 问题诊断（同步/CPU 受限/Python 开销/算子碎片化/autograd/小内核/显存/步抖动/精度与布局/反向占比/用户代码热点），可选定位到源码 文件:行；
 * - 采集（capture：生成可直接运行的 PyTorch Profiler 采集脚本，mode=run 可执行（需审批））。
 *
 * 与 `nsight` 子Agent 的分工（两面互不依赖，可同时装载）：本面解释「哪个算子、哪行 Python、显存怎么用」；
 * GPU 内核级时间线（Nsight Systems）与单内核硬件计数器（Nsight Compute）由 `nsight` 提供。
 * 注意：Windows 上 PyTorch 的 CUPTI 采集不可用（实测显式启用 CUDA activity 仍无 kernel 事件），
 * 因此本面在 Windows 下通常只含 CPU/内存维度，GPU 侧需用 nsys 采集——工具输出会明确说明这一点。
 *
 * 设计取向：单趟流式扫描（不整文件 JSON.parse，内存与 trace 规模解耦）、结果条数有界、
 * 事实按文件指纹缓存（内存 + 落盘），因此 GB 级 trace 仍可实时分析；
 * 超出时间预算的扫描会返回「未完成」与可后台执行的命令（不抛超时错误）。
 */
import { isAbsolute, resolve } from "node:path"
import type { SubAgentDef } from "@gebai/sdk"
import { projectAware } from "@gebai/sdk/node"
import systemPromptBase from "./torch.md"
import { torchTools } from "./torch-tools"

export const name = "torch"

export const description =
  "PyTorch Profiler trace（Chrome Trace / Kineto）分析：解析 .pt.trace.json(.gz) 给出算子与内核热点（自身耗时、张量形状与 dtype）、前向/反向拆分、步级耗时与抖动、显存峰值与碎片率、GPU 空闲缝与 CPU 受限判定，输出量化问题清单并把热点定位到工程源码 文件:行；可生成并执行 Profiler 采集脚本（需审批）。超大 trace 走流式扫描与缓存，超预算转后台。" +
  "输入：trace 路径（可带 gz）+ 源码工程。"

export const systemPrompt = systemPromptBase

/** trace 路径常位于工程内，故统一加 project 参数（预置项目名/路径/保留名 tmp）以按根解析。 */
export const tools = {
  reports: projectAware(torchTools.reports!, { workdir: true }),
  overview: projectAware(torchTools.overview!, { workdir: true }),
  ops: projectAware(torchTools.ops!, { workdir: true }),
  memory: projectAware(torchTools.memory!, { workdir: true }),
  findings: projectAware(torchTools.findings!, { workdir: true }),
  compare: projectAware(torchTools.compare!, { workdir: true }),
  capture: projectAware(torchTools.capture!, { workdir: true }),
}

// capture 的审批由工具自身的 requiresApproval 判定（mode=script 只生成脚本免审批；mode=run 执行需审批）
export const requiresApproval = {}
export const preload = false

export const envVars = [
  { name: "TORCH_TRACE_PROJECT", description: "默认源码工程根：未指定 project 参数时以其为基准（trace 路径与源码定位的搜索范围）" },
  { name: "TORCH_CACHE_DIR", description: "事实落盘缓存目录（默认 {GEBAI_HOME}/cache/torch：按 trace 指纹存聚合事实，重复分析直接命中）" },
  { name: "TORCH_NATIVE", description: "原生聚合后端开关（默认开启；off 时固定走 JS 流式实现，用于对照与排障）" },
  { name: "TORCH_NATIVE_THREADS", description: "原生聚合的并行度（默认自动：小文件单趟、大文件 = 可用核数；1 = 单趟路径，可用于 A/B 对照）" },
]

/** 默认工程根兜底：TORCH_TRACE_PROJECT 配置时即视为项目绑定。 */
export const projectRoot = (env: Record<string, string>): string | undefined => {
  const value = env.TORCH_TRACE_PROJECT
  if (!value) return undefined
  return isAbsolute(value) ? value : resolve(process.cwd(), value)
}

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars, projectRoot }
