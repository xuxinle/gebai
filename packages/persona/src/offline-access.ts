/**
 * 探测「这个飞书应用开没开通长期授权（`offline_access`）」。
 *
 * ## 为什么必须**事前**判定（而不是靠拒绝再降级）
 *
 * 应用没开通 `offline_access` 时，飞书的行为是：把请求打到授权页，
 * **在 accounts.feishu.cn 上直接渲染一张错误页**（「当前应用权限不足 / 错误码 20027」），
 * 根本不会跳回 `redirect_uri`。也就是说客户端**没有任何回跳机会**去改发短期链接——
 * 「先要长期、被拒再降级」这条路在授权页层面就是断的：用户看到的是死页，
 * 点「刷新重试」也永远重试不出来。
 *
 * 唯一能事前知道的办法是读应用的权限清单：`GET /open-apis/application/v6/applications/{app_id}`
 * 的 `data.app.scopes[].scope` 里有没有 `offline_access`。
 *
 * ## 三态，而不是布尔
 *
 * 读清单本身要权限（`application:application:self_manage` 或 `admin:app.info:readonly`），
 * 多数应用没开——那时会回 99991672。这种「读不到」必须与「读到了、确实没有」区分开：
 * 处理策略虽然都是按短期发链接，但日志要说得清原因，否则用户会以为自己后台没配对。
 */

import { OFFLINE_ACCESS_SCOPE } from "./oauth"
import { feishuFetch } from "./tls"

const TENANT_TOKEN_URL = "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal"
const APP_INFO_URL = "https://open.feishu.cn/open-apis/application/v6/applications"

/**
 * 探测结论。
 *
 * - `supported`：清单里有 `offline_access` → 授权链接带上它，能拿到 refresh_token
 * - `unsupported`：清单读到了、里面没有 → 带上必撞 20027 死页，不能带
 * - `unknown`：读不到（应用没开「读取应用信息」权限 / 网络失败 / 超时）
 */
export type OfflineAccessSupport = "supported" | "unsupported" | "unknown"

export interface OfflineAccessProbeResult {
  support: OfflineAccessSupport
  /** 人可读的原因（写进日志；`supported` 时为空）。 */
  reason?: string
  /** 清单里的权限条数（诊断用，拿不到时不填）。 */
  scopeCount?: number
}

/**
 * 探测应用的长期授权支持。
 *
 * **永远不会抛错**：探测失败只是「不知道」，调用方据此按短期处理即可——
 * 让一个探测把授权流程整条打断是更坏的结局（连短期授权都拿不到了）。
 */
export async function probeOfflineAccess(opts: {
  appId: string
  appSecret: string
  fetchImpl?: typeof fetch
  timeoutMs?: number
}): Promise<OfflineAccessProbeResult> {
  const fetchImpl = opts.fetchImpl ?? feishuFetch
  const timeoutMs = opts.timeoutMs ?? 8000
  const signal = typeof AbortSignal?.timeout === "function" ? AbortSignal.timeout(timeoutMs) : undefined

  let tenantToken: string
  try {
    const res = await fetchImpl(TENANT_TOKEN_URL, {
      method: "POST",
      headers: { "Content-Type": "application/json; charset=utf-8" },
      body: JSON.stringify({ app_id: opts.appId, app_secret: opts.appSecret }),
      ...(signal ? { signal } : {}),
    })
    const body = (await res.json().catch(() => ({}))) as { tenant_access_token?: string; msg?: string; code?: number }
    if (!body.tenant_access_token) {
      return { support: "unknown", reason: `取应用身份令牌失败：code=${body.code ?? res.status} ${body.msg ?? ""}`.trim() }
    }
    tenantToken = body.tenant_access_token
  } catch (err) {
    return { support: "unknown", reason: `取应用身份令牌时出错：${String((err as Error)?.message || err)}` }
  }

  try {
    const url = `${APP_INFO_URL}/${encodeURIComponent(opts.appId)}?lang=zh_cn`
    const res = await fetchImpl(url, { headers: { authorization: `Bearer ${tenantToken}` }, ...(signal ? { signal } : {}) })
    const body = (await res.json().catch(() => ({}))) as {
      code?: number
      msg?: string
      data?: { app?: { scopes?: Array<{ scope?: string }> } }
    }
    if (body.code !== 0 || !body.data?.app) {
      return {
        support: "unknown",
        reason:
          `读应用权限清单失败：code=${body.code ?? res.status} ${String(body.msg ?? "").slice(0, 160)}`.trim() +
          "（该应用需开通 application:application:self_manage 或 admin:app.info:readonly）",
      }
    }
    const scopes = (body.data.app.scopes ?? []).map((s) => String(s.scope ?? ""))
    const has = scopes.includes(OFFLINE_ACCESS_SCOPE)
    return has
      ? { support: "supported", scopeCount: scopes.length }
      : {
          support: "unsupported",
          scopeCount: scopes.length,
          reason: `应用权限清单（${scopes.length} 项）里没有 ${OFFLINE_ACCESS_SCOPE}——需在开放平台「权限管理」里开通「持续访问已授权的数据」并发布版本`,
        }
  } catch (err) {
    return { support: "unknown", reason: `读应用权限清单时出错：${String((err as Error)?.message || err)}` }
  }
}
