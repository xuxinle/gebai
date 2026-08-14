/* ---------- 外部身份凭证解析（同源集成扩展点；纯函数，无 DOM 依赖，可独立单测） ---------- */

export interface ExternalCredential {
  username: string
  credential: string
}

/**
 * 解析外部身份凭证。来源优先级：URL 参数（?gb_ext_username=&gb_ext_credential=）→
 * 宿主 localStorage（storageKey 配置；值支持 JSON {"username","credential"} 或 "username:credential" 字符串）。
 * 未找到返回 null。
 */
export function parseExternalCredential(
  params: URLSearchParams,
  storage: { getItem(key: string): string | null },
  storageKey: string | null,
): ExternalCredential | null {
  const u = params.get("gb_ext_username")?.trim()
  const c = params.get("gb_ext_credential")?.trim()
  if (u && c) return { username: u, credential: c }
  if (!storageKey) return null
  let raw: string | null = null
  try {
    raw = storage.getItem(storageKey)
  } catch {
    return null
  }
  if (!raw) return null
  let parsedJson = false
  try {
    const obj = JSON.parse(raw) as { username?: unknown; credential?: unknown }
    parsedJson = true
    if (typeof obj.username === "string" && typeof obj.credential === "string") {
      const name = obj.username.trim()
      if (name && obj.credential.trim()) return { username: name, credential: obj.credential.trim() }
    }
  } catch {
    /* 非 JSON，走字符串格式 */
  }
  if (parsedJson) return null // JSON 结构不符：不再按字符串格式误拆
  const idx = raw.indexOf(":")
  if (idx > 0) {
    const name = raw.slice(0, idx).trim()
    const cred = raw.slice(idx + 1).trim()
    if (name && cred) return { username: name, credential: cred }
  }
  return null
}
