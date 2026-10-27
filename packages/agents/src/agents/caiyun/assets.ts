/**
 * 裁云的**资产库工具**：把"验证过的优质素材与示例"变成可检索的一等资源。
 *
 * ## 为什么需要它（与 `style_guide` 的分工）
 *
 * `style_guide` 回答"**用哪种叙事结构**"（方法论，15 KB 文本）。
 * 本工具回答"**有没有现成能用的东西**"——具体到：
 *
 * - **示例脚本**：某种版式/动效已经有人写好了，读它比从零试快一个数量级；
 * - **素材图**：背景/纹理/主图，省一次图像生成，且尺寸与配色已配合过文案；
 * - **可复用片段**：脚本里已调好的光照参数、镜头运动、`layout` 写法。
 *
 * 实测代价（做一条 10 秒三维短片）：材质与光照试了 **10 轮**才定下来，
 * 而最终那组参数（`metallic=0`、`rim=0.24`、`ambient=0.20`…）本可以
 * 从既有示例里抄到。**从零调参去复现一个已经调好的东西，是最贵的返工。**
 *
 * ## 关键设计：只收录**验证过**的条目
 *
 * 库里混进没验证的样例，比没有库更糟——模型会把它当正面参照。
 * 所以每条资产都带 `verified` 字段，含义是本仓实测过：
 *
 * | 状态 | 含义 |
 * |---|---|
 * | `verified` | 在 `--strict` 下能渲染 + 抽帧核过 + 有量化结论（静止占比等） |
 * | `draft` | 能渲染但未核观感；只可用于"结构参考"，不可当成品参照 |
 *
 * 数据存在 `assets/library.json`，由 `python tools/caiyun_audit_assets.py`
 * 实测产出后人工补注（`why` / `use_for` / `tags`）——**量化由脚本给，判断由人给**。
 * 这样"验证"是可复现的动作，而不是一句承诺。
 *
 * ## 为什么不直接把资产塞进提示词
 *
 * 提示词里写"有个 bg-aurora 素材"不解决问题：模型不知道它多亮、适合什么文案、
 * 和哪个模板搭。真正的价值在**元数据**（亮度/是否压暗过/适用场景），
 * 而元数据必须按需查、不能常驻上下文。
 */

import { existsSync } from "node:fs"
import { readFile } from "node:fs/promises"
import { join, resolve } from "node:path"
import type { Tool, ToolContext } from "@gebai/sdk"
import { schema } from "@gebai/sdk/node"

/** 资产库条目的形状（与 `assets/library.json` 一一对应）。 */
interface AssetEntry {
  /** 资产 id（模型按它引用），如 `sample-3d` / `bg-aurora`。 */
  id: string
  /** 类别：`script`（示例脚本）/ `media`（素材图）/ `snippet`（可复用片段）。 */
  kind: "script" | "media" | "snippet"
  /** 仓库内相对路径（相对 shuangtian/）。`snippet` 为内联知识，此字段为空。 */
  path?: string
  /** true = 内容直接写在 highlights 里，没有独立文件（模型不必去 read）。 */
  inline?: boolean
  /** 一句话说明它能干什么。 */
  summary: string
  /** 用在什么场合（模型据此判断"我这个需求能不能复用"）。 */
  use_for: string
  /** 检索标签。 */
  tags: string[]
  /** **验证状态**：只有 verified 的可当成品参照。 */
  verified: "verified" | "draft"
  /** 实测数据（由审计脚本产出）：静止占比、变化密度、亮度、帧数。 */
  measured?: {
    still_ratio?: number
    change_density?: number
    bright_ratio?: number
    mean_luma?: number
    frames?: number
  }
  /** 值得抄的具体东西：字段值、写法、坑。 */
  highlights?: string[]
  /** 已知限制（避免模型把它当万灵药）。 */
  caveats?: string[]
}

interface Library {
  version: number
  note: string
  assets: AssetEntry[]
}

/** 框架根（与 production.ts 同一判据）。 */
function frameworkRoot(ctx: ToolContext): string {
  const explicit = String(ctx.env.SHUANGTIAN_PROJECT ?? "").trim()
  if (explicit) return resolve(explicit)
  return resolve(process.cwd(), "shuangtian")
}

async function loadLibrary(ctx: ToolContext): Promise<Library | null> {
  const p = join(frameworkRoot(ctx), "examples", "caiyun", "assets", "library.json")
  if (!existsSync(p)) return null
  try {
    return JSON.parse(await readFile(p, "utf8")) as Library
  } catch {
    return null
  }
}

/** 一条资产的展示文本（列表与详情共用，避免两处措辞分叉）。 */
function renderEntry(entry: AssetEntry, verbose: boolean): string {
  const badge = entry.verified === "verified" ? "✓verified" : "○draft"
  const m = entry.measured ?? {}
  const measured: string[] = []
  if (m.still_ratio !== undefined) measured.push(`静止 ${m.still_ratio}%`)
  if (m.change_density !== undefined) measured.push(`变化 ${m.change_density}/s`)
  if (m.mean_luma !== undefined) measured.push(`亮度 ${m.mean_luma}`)
  if (m.frames !== undefined) measured.push(`${m.frames} 帧`)

  const lines: string[] = []
  lines.push(`- **${entry.id}** [${entry.kind}/${badge}] ${entry.summary}`)
  if (measured.length) lines.push(`    实测：${measured.join(" · ")}`)
  if (verbose) {
    if (entry.inline) {
      lines.push(`    形式：内联知识（无独立文件，内容就在下面的“值得抄”里）`)
    } else if (entry.path) {
      lines.push(`    路径：${entry.path}`)
    }
    lines.push(`    适用：${entry.use_for}`)
    if (entry.tags.length) lines.push(`    标签：${entry.tags.join(", ")}`)
    if (entry.highlights?.length) {
      lines.push(`    值得抄：`)
      for (const h of entry.highlights) lines.push(`      · ${h}`)
    }
    if (entry.caveats?.length) {
      lines.push(`    限制：`)
      for (const c of entry.caveats) lines.push(`      · ${c}`)
    }
  }
  return lines.join("\n")
}

export const assetsTool: Tool = {
  name: "assets",
  safeMode: true,
  description:
    "查裁云**资产库**：验证过的示例脚本、素材图、可复用片段（光照/镜头/layout 写法）。"
    + "写脚本前先查这里——某种版式或材质组合可能已经调好了，抄现成的比从零试快一个数量级。 "
    + "不传 query 列出全部（按类别分组，带实测数据与验证状态）；"
    + "传 query 按关键词/标签过滤；传 id 看单条详情（含'值得抄的具体字段值'与'已知限制'）。 "
    + "**只有标 `✓verified` 的才能当成品参照**；`○draft` 只可用于结构参考。"
    + "素材图可用 read 直接看。",
  parameters: schema(
    {
      query: { type: "string", description: "关键词（匹配 id/说明/适用/标签，不区分大小写）" },
      kind: {
        type: "string",
        description: "类别过滤：script（示例脚本）/ media（素材图）/ snippet（可复用片段）",
      },
      id: { type: "string", description: "给定时返回该条详情（含完整 highlights 与 caveats）" },
    },
    [],
  ),
  async execute(args, ctx) {
    const lib = await loadLibrary(ctx)
    if (!lib) {
      return {
        output:
          "资产库尚未建立（缺 `examples/caiyun/assets/library.json`）。\n"
          + "可以直接看目录：`shuangtian/examples/caiyun/assets/`（示例脚本与 media/ 素材）、\n"
          + "`shuangtian/examples/caiyun/styles/`（七个风格模板的一镜示例）。",
      }
    }

    const id = String(args.id ?? "").trim()
    if (id) {
      const hit = lib.assets.find((a) => a.id === id)
      if (!hit) {
        const near = lib.assets
          .filter((a) => a.id.includes(id) || id.includes(a.id))
          .slice(0, 5)
          .map((a) => a.id)
        return {
          output:
            `未知资产 id "${id}"。` + (near.length ? `相近的有：${near.join("、")}` : "不传 id 可列出全部。"),
        }
      }
      return { output: renderEntry(hit, true) }
    }

    const query = String(args.query ?? "").trim().toLowerCase()
    const kindWanted = String(args.kind ?? "").trim().toLowerCase()

    const hits = lib.assets.filter((a) => {
      if (kindWanted && a.kind !== kindWanted) return false
      if (!query) return true
      const hay = [a.id, a.summary, a.use_for, ...(a.tags ?? []), ...(a.highlights ?? [])]
        .join(" ")
        .toLowerCase()
      return hay.includes(query)
    })

    if (hits.length === 0) {
      return {
        output:
          `没有匹配的资产（query='${query}', kind='${kindWanted || "全部"}'）。\n`
          + `不传 query 可列出全部 ${lib.assets.length} 条。`,
      }
    }

    const groups: Array<[string, string]> = [
      ["script", "示例脚本（可直接 read 全文对照写法）"],
      ["media", "素材图（用 read 看，脚本里写 media/<名>.png）"],
      ["snippet", "可复用片段（字段值/写法，抄进自己的脚本）"],
    ]
    const lines: string[] = []
    for (const [kind, title] of groups) {
      const sub = hits.filter((a) => a.kind === kind)
      if (sub.length === 0) continue
      lines.push(`\n## ${title}`)
      for (const a of sub) lines.push(renderEntry(a, false))
    }

    const verifiedCount = hits.filter((a) => a.verified === "verified").length
    return {
      output:
        `资产库（${hits.length} 条，其中 ${verifiedCount} 条 ✓verified）：\n`
        + `${lines.join("\n")}\n\n`
        + `查详情：\`assets id=<id>\`（给出可抄的具体字段值）。\n`
        + `**只有 ✓verified 能当成品参照**——draft 可能静止过多或未核观感。`,
    }
  },
}
