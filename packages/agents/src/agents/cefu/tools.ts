/**
 * 册府工具面：桥函数 → 标准工具（懒启动控制器，第一次调用才 spawn 无头进程）。
 *
 * 七个工具与霜天应用的桥函数一一对应（`cefu.stats/categories/list/get/search/
 * put/remove/confirm/review`；工具面去掉了 categories——list 已覆盖浏览需求，
 * 工具数量本身就是上下文税）。
 *
 * 参数经 JSON 序列化拼成 eval 表达式（`app.cefu.search("...", 5)`）——
 * 参数是结构化 JSON、桥函数收 JSON 数组，序列化往返无损。
 */

import type { Tool, ToolResult, ToolSet } from "@gebai/sdk"
import { cefuCall, ensureCefu, type CefuControllerOptions } from "./controller"

function textResult(output: string, data?: unknown): ToolResult {
  return data === undefined ? { output } : { output, data }
}

/** 词条摘要渲染（给模型看的自然形态，不是原始 JSON）。 */
function renderHit(hit: Record<string, unknown>): string {
  const id = String(hit.id ?? "?")
  const title = String(hit.title ?? "?")
  const category = String(hit.category ?? "?")
  const score = typeof hit.score === "number" ? `（score ${hit.score.toFixed(3)}）` : ""
  const why = Array.isArray(hit.why) ? ` 为什么召回：${hit.why.map(String).join("、")}` : ""
  return `[${id}] ${title}（${category}）${score}${why}`
}

function renderEntry(entry: Record<string, unknown>): string {
  const lines: string[] = []
  const id = String(entry.id ?? "?")
  const title = String(entry.title ?? "?")
  const category = String(entry.category ?? "?")
  lines.push(`【${title}】[${id}]（分类：${category}）`)
  if (entry.symptoms) lines.push(`现象：${String(entry.symptoms)}`)
  if (Array.isArray(entry.causes) && entry.causes.length) lines.push(`可能原因：${entry.causes.map(String).join("；")}`)
  if (Array.isArray(entry.steps) && entry.steps.length) {
    lines.push("步骤（按序执行）：")
    entry.steps.forEach((s, i) => lines.push(`  ${i + 1}. ${String(s)}`))
  }
  if (Array.isArray(entry.commands) && entry.commands.length) {
    lines.push("命令：")
    for (const c of entry.commands) lines.push(`  $ ${String(c)}`)
  }
  if (Array.isArray(entry.facts) && entry.facts.length) {
    lines.push("硬事实：")
    for (const f of entry.facts) {
      if (f && typeof f === "object") {
        const fact = f as Record<string, unknown>
        lines.push(`  - ${String(fact.k ?? fact.key ?? "?")}：${String(fact.v ?? fact.value ?? "?")}`)
      } else lines.push(`  - ${String(f)}`)
    }
  }
  if (entry.source_ref) lines.push(`出处：${String(entry.source_ref)}`)
  if (entry.confidence) lines.push(`置信度：${String(entry.confidence)}${entry.needs_review ? "（待确认）" : ""}`)
  return lines.join("\n")
}

export function createTools(deps: CefuControllerOptions = {}): ToolSet {
  const search: Tool = {
    name: "cefu_search",
    description:
      "知识库检索：用自然语言短句查「怎么办 / 找谁 / 在哪查 / 上次怎么解决的」。" +
      "索引含中文 1+2-gram，两字词不失效；返回摘要+分数+为什么召回。分数接近的多条都看，不要只看第一条。无命中会明说。",
    parameters: {
      type: "object",
      properties: {
        query: { type: "string", description: "自然语言短句（如「写入延迟高」「配额找谁」）" },
        limit: { type: "number", description: "返回条数（缺省 5，上限 20）" },
      },
      required: ["query"],
    },
    async execute(args) {
      const query = String(args.query ?? "").trim()
      if (!query) return textResult("query 不能为空")
      const limit = Math.max(1, Math.min(20, Number(args.limit ?? 5) || 5))
      try {
        const hits = (await cefuCall("search", `${JSON.stringify(query)}, ${limit}`, deps)) as Array<Record<string, unknown>>
        if (!Array.isArray(hits) || hits.length === 0) return textResult("册府里没有相关词条（检索无命中）——不要用通用常识冒充知识库内容。")
        return textResult(hits.map(renderHit).join("\n"), { hits })
      } catch (err) {
        return textResult(`cefu_search 失败：${String((err as Error).message || err)}`)
      }
    },
  }

  const get: Tool = {
    name: "cefu_get",
    description: "取词条全文：现象 / 可能原因 / 有序步骤（第 1 条最该先做）/ 命令 / 硬事实 / 出处。处理问题前取全文，别拿摘要指导操作。",
    parameters: {
      type: "object",
      properties: { id: { type: "string", description: "词条 id（cefu_search/cefu_list 返回的 [xxx]）" } },
      required: ["id"],
    },
    async execute(args) {
      const id = String(args.id ?? "").trim()
      if (!id) return textResult("id 不能为空")
      try {
        const entry = (await cefuCall("get", JSON.stringify(id), deps)) as Record<string, unknown>
        return textResult(renderEntry(entry), { entry })
      } catch (err) {
        return textResult(`cefu_get 失败：${String((err as Error).message || err)}`)
      }
    },
  }

  const put: Tool = {
    name: "cefu_put",
    description:
      "写入词条（title/category/source 必填——没有标题与分类的条目无法被成册浏览）。" +
      "写入时机只有三种：用户纠正了你 / 实测出稳定结论 / 同一问题被问第二遍。aliases 是召回率最便宜的投资。",
    parameters: {
      type: "object",
      properties: {
        title: { type: "string", description: "一行能认出来是什么（列表里靠它认人）" },
        category: { type: "string", description: "成册依据（如「值班知识」「环境配置」）" },
        source: { type: "string", description: "出处（文件:行号 / 会话 id / 人名）" },
        symptoms: { type: "string", description: "现象描述（什么情况下用这条）" },
        causes: { type: "array", items: { type: "string" }, description: "可能原因" },
        steps: { type: "array", items: { type: "string" }, description: "有序步骤——第 1 条是最该先做的动作，不是背景介绍" },
        commands: { type: "array", items: { type: "string" }, description: "能直接复制执行的完整命令" },
        facts: { type: "array", items: { type: "object", properties: { k: { type: "string" }, v: { type: "string" } }, required: ["k", "v"] }, description: "硬事实：IP/端口/阈值/chat_id/人名（k-v 对）" },
        aliases: { type: "array", items: { type: "string" }, description: "别名（别人会怎么问：同义词/口语/报错原文/缩写）" },
        confidence: { type: "string", enum: ["verified", "inferred"], description: "verified=人说的或实测；inferred=推断（30 天不确认自动过期）" },
        id: { type: "string", description: "更新已有词条时传它的 id（同册内高度相似会被拒，应沿用 id 更新）" },
      },
      required: ["title", "category", "source"],
    },
    async execute(args) {
      const title = String(args.title ?? "").trim()
      const category = String(args.category ?? "").trim()
      const source = String(args.source ?? "").trim()
      if (!title || !category || !source) {
        return textResult("title / category / source 都是必填——没有标题、分类与出处的知识无法被找到与追溯。")
      }
      const entry: Record<string, unknown> = { title, category, source_ref: source }
      if (args.id) entry.id = String(args.id)
      if (args.symptoms) entry.symptoms = String(args.symptoms)
      if (Array.isArray(args.causes) && args.causes.length) entry.causes = args.causes.map(String)
      if (Array.isArray(args.steps) && args.steps.length) entry.steps = args.steps.map(String)
      if (Array.isArray(args.commands) && args.commands.length) entry.commands = args.commands.map(String)
      if (Array.isArray(args.facts) && args.facts.length) entry.facts = args.facts
      if (Array.isArray(args.aliases) && args.aliases.length) entry.aliases = args.aliases.map(String)
      if (args.confidence) entry.confidence = String(args.confidence)
      try {
        const r = (await cefuCall("put", JSON.stringify(entry), deps)) as Record<string, unknown>
        const id = String(r.id ?? "?")
        const updated = r.updated === true || r.created === false
        return textResult(`${updated ? "已更新" : "已新建"}词条 [${id}] ${title}（分类 ${category}）`, { result: r })
      } catch (err) {
        const msg = String((err as Error).message || err)
        // 相似拒绝：把既有词条带出来，模型据此决定沿用 id 或改标题
        return textResult(`cefu_put 被拒绝：${msg}\n（同册内与既有词条高度相似时会拒绝——沿用它的 id 更新，或让标题体现区别）`)
      }
    },
  }

  const list: Tool = {
    name: "cefu_list",
    description: "翻知识库目录（按分类成册浏览）。想知道「这个库里都记了什么」时用。",
    parameters: {
      type: "object",
      properties: { category: { type: "string", description: "只看这一册（缺省全部）" } },
    },
    async execute(args) {
      const category = args.category ? String(args.category) : ""
      try {
        const entries = (await cefuCall("list", category ? JSON.stringify(category) : "", deps)) as Array<Record<string, unknown>>
        if (!Array.isArray(entries) || entries.length === 0) {
          return textResult(category ? `「${category}」册里没有词条` : "册府是空的（还没有任何词条）")
        }
        return textResult(entries.map(renderHit).join("\n"), { entries })
      } catch (err) {
        return textResult(`cefu_list 失败：${String((err as Error).message || err)}`)
      }
    },
  }

  const review: Tool = {
    name: "cefu_review",
    description: "整理巡检：找重复词条 / 过期未确认 / 从未被命中的冷条目 / 分类名漂移，每条给出该做的动作。用户问「知识库还好吗」或连续写入多条之后跑它。",
    parameters: {
      type: "object",
      properties: {
        merge_threshold: { type: "number", description: "重复判定相似度阈值（缺省 0.6）" },
        stale_days: { type: "number", description: "过期天数（缺省 30）" },
        unused_days: { type: "number", description: "冷条目天数（缺省 90）" },
      },
    },
    async execute(_args) {
      try {
        const r = (await cefuCall("review", "", deps)) as Record<string, unknown>
        const items = (r.items ?? r.review ?? r) as unknown
        return textResult(`巡检结果：\n${JSON.stringify(items, null, 2)}`, { result: r })
      } catch (err) {
        return textResult(`cefu_review 失败：${String((err as Error).message || err)}`)
      }
    },
  }

  const confirm: Tool = {
    name: "cefu_confirm",
    description: "把词条标为「已确认」并延期 180 天。用于人核实过的事实。推断来的知识不确认 30 天自动过期——不确认的知识本就该被怀疑。",
    parameters: {
      type: "object",
      properties: { id: { type: "string", description: "词条 id" } },
      required: ["id"],
    },
    async execute(args) {
      const id = String(args.id ?? "").trim()
      if (!id) return textResult("id 不能为空")
      try {
        const r = await cefuCall("confirm", JSON.stringify(id), deps)
        return textResult(`[${id}] 已确认（延期 180 天）`, { result: r })
      } catch (err) {
        return textResult(`cefu_confirm 失败：${String((err as Error).message || err)}`)
      }
    },
  }

  const stats: Tool = {
    name: "cefu_stats",
    description: "知识库概览：条目数 / 册数 / 置信度分布 / 库文件路径。",
    parameters: { type: "object", properties: {} },
    async execute() {
      try {
        await ensureCefu(deps)
        const r = (await cefuCall("stats", "", deps)) as Record<string, unknown>
        return textResult(
          `条目 ${r.entries} 条 / ${r.categories} 册 · 已验证 ${r.verified} / 推断 ${r.inferred} / 过期 ${r.stale}` +
            `${r.needs_review ? ` · 待确认 ${r.needs_review}` : ""}\n库文件：${r.db_path}`,
          { stats: r },
        )
      } catch (err) {
        return textResult(`cefu_stats 失败：${String((err as Error).message || err)}`)
      }
    },
  }

  return { cefu_search: search, cefu_get: get, cefu_put: put, cefu_list: list, cefu_review: review, cefu_confirm: confirm, cefu_stats: stats }
}
