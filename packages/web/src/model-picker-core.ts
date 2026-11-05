/**
 * 标题栏模型配置选择器的纯逻辑（无 DOM 依赖，可单测）。
 * DOM 绑定与网络在同名 .ts（model-picker.ts）。
 */
import type { ModelConfigSummary } from "@gebai/sdk"

/**
 * 会话当前生效的配置名：选了且清单里仍在 → 用它；未选 / 选的名字已不在清单
 * （清单改过、会话从别处导入）→ 回落默认配置（清单第一条）。
 *
 * 为什么不保留「选了个不存在的名字」：服务端解析器对未知名字会回落默认配置，
 * 入口若照样显示这个名字，界面说的和实际跑的就成了两回事。
 */
export function resolveConfigName(list: ModelConfigSummary[], sessionModelConfig: string | undefined): string {
  if (sessionModelConfig && list.some((c) => c.name === sessionModelConfig)) return sessionModelConfig
  return list[0]?.name ?? ""
}

/** 上下文窗口：命中配置声明则用它，否则 0（不覆盖，由服务端快照给默认窗口）。 */
export function configCtxWindow(list: ModelConfigSummary[], name: string): number {
  return list.find((c) => c.name === name)?.maxContextTokens ?? 0
}

/** 接口地址的主机名（配置清单里用：同一模型可能有多份配置指向不同端点，主机名是最短的可辨识部分）。 */
export function apiHost(apiBase: string): string {
  if (!apiBase) return ""
  try {
    return new URL(apiBase).host
  } catch {
    // 非法/相对地址：去掉协议头与原查询串，原样展示（宁可多给一点，也不要空着让人去猜）
    return apiBase.replace(/^[a-z]+:\/\//i, "").split("/")[0]
  }
}

/** 清单条目的副标题（一行）：模型标识 + 多模态能力 + 接口主机——不必切过去就能判断这份配置能干什么、指向哪里。 */
export function configMenuDetail(cfg: ModelConfigSummary): string {
  const vision = cfg.multimodal ? (cfg.visionModel ? "图片+视觉模型" : "支持图片") : "仅文本"
  const host = apiHost(cfg.apiBase)
  return [cfg.model, vision, host].filter(Boolean).join(" · ")
}
