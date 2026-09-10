/**
 * @gebai/agents 入口：13 个 TS 子代理（code/self_optimize/hsh/cron/desktop/explore/
 * feishu_docs/feishu_group/playwright/reverse_site/vision/widgets/wps）+ 子代理专属基建
 * （cv 全家 / browser 桥接 / analyzer 符号分析 / widgets 存储 / shared 公共件）。
 *
 * 边界规则（DESIGN「TS 子代理抽包解耦」）：本包零 import @gebai/server（编译期强制——
 * tsconfig 无 server 引用）；server 单向依赖本包（boot 接线/discover 动态扫描/
 * 构建脚本 bundle/路由薄引用）。
 * 每个子代理模块约定：export const name / description / def（SubAgentDef，含 tools）——
 * 与 server 侧 sub-agents 目录的模块形状一致（index 不做 star re-export：13 个模块
 * 同名导出会冲突，统一经下面 allAgents 注册表消费）。
 */
import type { SubAgentDef } from "@gebai/sdk"

import * as code from "./code"
import * as self_optimize from "./self_optimize"
import * as hsh from "./hsh"
import * as cron from "./cron"
import * as desktop from "./desktop"
import * as explore from "./explore"
import * as feishu_docs from "./feishu_docs"
import * as feishu_group from "./feishu_group"
import * as playwright from "./playwright"
import * as reverse_site from "./reverse_site"
import * as vision from "./vision"
import * as widgets from "./widgets"
import * as wps from "./wps"

/** 全部 TS 子代理模块（name/description/def），目录序与 server 侧 sub-agents 一致。 */
export const allAgents: Array<{
  name: string
  description: string
  def: SubAgentDef
}> = [
  code, self_optimize, hsh, cron, desktop, explore,
  feishu_docs, feishu_group, playwright, reverse_site, vision, widgets, wps,
]

export { code, self_optimize, hsh, cron, desktop, explore, feishu_docs, feishu_group, playwright, reverse_site, vision, widgets, wps }
