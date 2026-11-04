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
 * 下拉若照样显示这个名字，界面说的和实际跑的就成了两回事。
 */
export function resolveConfigName(list: ModelConfigSummary[], sessionModelConfig: string | undefined): string {
  if (sessionModelConfig && list.some((c) => c.name === sessionModelConfig)) return sessionModelConfig
  return list[0]?.name ?? ""
}

/** 上下文窗口：命中配置声明则用它，否则 0（不覆盖，由服务端快照给默认窗口）。 */
export function configCtxWindow(list: ModelConfigSummary[], name: string): number {
  return list.find((c) => c.name === name)?.maxContextTokens ?? 0
}

/** 悬浮提示的文本行：配置的完整接入信息（不打开下拉也能确认选的是哪一份）。 */
export function configTipLines(cfg: ModelConfigSummary): string[] {
  const lines = [`模型配置：${cfg.name}`, `模型 ${cfg.model}（${cfg.apiKind}）`]
  if (cfg.apiBase) lines.push(`接口 ${cfg.apiBase}`)
  lines.push(cfg.multimodal ? "多模态：支持图片输入" : "多模态：仅文本")
  if (cfg.visionModel) lines.push(`视觉模型 ${cfg.visionModel}`)
  return lines
}
