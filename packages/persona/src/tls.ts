/**
 * 飞书接口 TLS 策略（本包自持一份，与 `@gebai/agents` 的 `shared/tls.ts` 同构）。
 *
 * ## 为什么复制而不 import
 *
 * `@gebai/persona` 的定位是**零依赖**（不 import `@gebai/server` / `@gebai/agents`），
 * 这样它可以独立单测、被宿主以任意方式装配。为一个 13 行的策略引入包依赖，
 * 会让「事件源插件能独立跑」这条承诺变脆。
 *
 * 两处实现同构是**刻意**的：行为必须一致，否则「渠道能发、人格不能发」
 * 这种现象会变成最难排查的一类（同一个开关，一半生效一半不生效）。
 * 一致性由测试钉住（见 tls.test.ts）。
 *
 * ## 为什么 persona 尤其需要它
 *
 * 拟人模式的授权链走的是 `accounts.feishu.cn`（与 `open.feishu.cn` 不同域）。
 * 企业内网出口设备会把这个域的证书链换成自签 CA 签发——浏览器能访问、
 * 服务端却报 `unable to verify the first certificate`，现象是
 * 「授权页能开、扫码也成功、回调到了服务端才失败」。
 *
 * 实测（本机）：`GEBAI_FEISHU_INSECURE_TLS=true` 下原生 `fetch` 仍然失败、
 * 而附加 `tls.rejectUnauthorized=false` 后返回 200。**所以本包的每一处飞书
 * 出站请求都必须走 `feishuFetch`，不能直接用 `fetch`**。
 */

/** GEBAI_FEISHU_INSECURE_TLS 是否开启（true/1）。 */
export function feishuTlsInsecure(): boolean {
  const v = process.env.GEBAI_FEISHU_INSECURE_TLS
  return v === "true" || v === "1"
}

/** 飞书 fetch 实现：开启时不校验 TLS 证书（注入 tls.rejectUnauthorized=false）。 */
export const feishuFetch = ((input: string | URL | Request, init?: RequestInit): Promise<Response> => {
  if (!feishuTlsInsecure()) return fetch(input, init)
  return fetch(input, { ...init, tls: { rejectUnauthorized: false } })
}) as typeof fetch
