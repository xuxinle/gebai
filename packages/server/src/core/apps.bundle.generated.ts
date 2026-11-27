// 由 scripts/build-apps-bundle.ts 生成（**入库版本为空态**；构建后为真内容，构建链收尾会还原）
// 空态入库的理由：boot/compose.ts 对本模块做静态 import，缺文件会让新克隆环境启动即崩
// （策略同 core/session/env-embedded.generated.ts）——它不是“产物目录里的缓存”，而是“带默认值的源码”。
import type { InlineApp } from "./apps/inline"

export const BUNDLED_APP_FILES: Record<string, Record<string, string>> = {}
export const BUNDLED_INLINE_APPS: InlineApp[] = []
export const BUNDLED_APPS: { files: Record<string, Record<string, string>>; inline: InlineApp[]; errors: string[] } = { files: {}, inline: [], errors: [] }
