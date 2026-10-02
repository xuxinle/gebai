/**
 * 凭证来源的**代码级扩展域**：仓库根 `custom/auth/`（二开域，随 `custom/` 整体迁移）。
 *
 * 与 `custom/agents/` 同一套「放文件即注册」模型：目录里每个 `*.ts`（或 `*.js`）**默认导出**的
 * 凭证来源（或来源数组）被收集进鉴权链；目录不存在（未二开）即零迭代，行为与出厂完全一致。
 *
 * ```ts
 * // custom/auth/gateway-header.ts —— 企业网关注入的身份头
 * import type { CredentialSource } from "@gebai/server/credential-sources"
 * const source: CredentialSource = {
 *   name: "gateway-header",
 *   safeMethodsOnly: false, // 声明含写方法；该头由网关剥离外部伪造，故可全方法生效
 *   resolve: (c) => c.userByName(c.header("x-gateway-user") ?? ""),
 * }
 * export default source
 * ```
 *
 * **与数据级（`GEBAI_CREDENTIAL_SOURCES`）的合并语义**：环境变量**显式设置了**清单即以其为准，
 * 代码级来源**前置**（先尝试，便于覆盖内置行为）；未设置则内置缺省链在前、代码级来源追加在后。
 * 两种途径都不改安全边界：来源只负责取出身份，校验仍由 `AuthService` 完成（见 credential-sources.ts）。
 *
 * **失败隔离**：单个文件 import/导出形态出错只记 `loadErrors`（启动日志可见根因），不影响其余来源
 * 与框架可用性——二开脚本写坏不应把服务拖成不可登录。
 */
import { existsSync } from "node:fs"
import { readdir } from "node:fs/promises"
import { join } from "node:path"
import type { CredentialSource } from "./credential-sources"

/** 二开凭证来源域（相对本文件：src/custom-auth.ts → 仓库根 custom/auth）。 */
export const CUSTOM_AUTH_DIR = join(import.meta.dirname, "..", "..", "..", "custom", "auth")

export interface CustomSourceLoad {
  sources: CredentialSource[]
  /** 加载失败的条目（文件: 原因），启动日志输出；不影响其余来源。 */
  errors: string[]
}

/** 导出值的形态校验：凭证来源需有 name 与 resolve。 */
function isCredentialSource(v: unknown): v is CredentialSource {
  if (!v || typeof v !== "object") return false
  const o = v as { name?: unknown; resolve?: unknown }
  return typeof o.name === "string" && o.name.length > 0 && typeof o.resolve === "function"
}

/** 归一导出值：单个来源 / 来源数组均可。 */
function toSources(v: unknown): CredentialSource[] {
  if (Array.isArray(v)) return v.filter(isCredentialSource)
  return isCredentialSource(v) ? [v] : []
}

/**
 * 扫描并加载二开凭证来源。目录不存在返回空结果（未二开是常态，不报错）。
 * `dir` 可注入（测试用）。
 */
export async function loadCustomCredentialSources(dir: string = CUSTOM_AUTH_DIR): Promise<CustomSourceLoad> {
  const out: CustomSourceLoad = { sources: [], errors: [] }
  if (!existsSync(dir)) return out
  let entries: string[]
  try {
    entries = (await readdir(dir, { withFileTypes: true }))
      .filter((e) => e.isFile() && /\.(ts|js|mjs)$/.test(e.name) && !/\.test\./.test(e.name) && !e.name.startsWith("_"))
      .map((e) => e.name)
      .sort()
  } catch (err) {
    out.errors.push(`${dir}: 目录读取失败（${(err as Error).message}）`)
    return out
  }
  for (const name of entries) {
    const file = join(dir, name)
    try {
      const mod = (await import(file)) as { default?: unknown }
      const sources = toSources(mod.default)
      if (!sources.length) {
        out.errors.push(`${name}: 未默认导出凭证来源（需 export default 单个来源或来源数组）`)
        continue
      }
      out.sources.push(...sources)
    } catch (err) {
      out.errors.push(`${name}: ${(err as Error).message}`)
    }
  }
  return out
}
