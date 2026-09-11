/**
 * 进程启动标识（bootId）：每个服务进程启动时生成一次，重启即变化。
 *
 * 用途——「服务重启后前端自动重新加载」：页面注入脚本（routes/static.ts，仅本地模式）轮询
 * `/api/health` 比对 bootId；一旦变化即说明当前页面来自旧进程（且重启时前端产物可能已重建），
 * 自动 `location.reload()` 重新加载，免除手工 F5——即「重启服务，前端跟着重启」的用户可见效果。
 *
 * 形态说明：随机 UUID（非敏感信息），/api/health 本就免鉴权（集成方健康检查用），服务模式同样暴露
 * ——但页面自动刷新脚本只在本地模式（auth=local）注入，多用户部署不受服务重启打扰。
 */
export const BOOT_ID: string = crypto.randomUUID()

/** 进程启动时刻（ms）：诊断用（与 BOOT_ID 同源，进程内固定）。 */
export const BOOT_AT: number = Date.now()
