/**
 * 路由表自检：把「路由注册成功、但永远匹配不上」这类**静默失效**挡在启动期。
 *
 * ## 起因
 *
 * `POST /api/v1/persona/schedules/:key{.+}/:action` 曾用 `{.+}` 跨斜杠正则匹配整段 key。
 * 这条路由在单测里是通的（测试用的是精简 app，只有这一条路由），但线上永远 404，
 * 前端只能报「未知错误」。原因是一条**没人会想到的链路**：
 *
 * 1. Hono 的 `SmartRouter` 按顺序试 `RegExpRouter` → `TrieRouter`，第一个不抛
 *    `UnsupportedPathError` 的就定下来，**全程静默**；
 * 2. `RegExpRouter` 拒绝「同一方法下静态段与参数段同层」的组合——本仓库的
 *    `GET /tasks/queue` 与 `GET /tasks/:id`、`POST /sessions/import` 与
 *    `use("/sessions/:id")` 正是这种组合，于是**整个应用**落到 `TrieRouter`；
 * 3. `TrieRouter` 不支持 `{.+}`：`getPattern()` 会把 `:key{.+}` 解析成三段
 *    `[label, "key", RegExp{}]`（取的是花括号里的字符串构造空正则），匹配时
 *    `matcher.exec()` 永远 null，该路由**永久不可达**。
 *
 * 三件事叠起来的结果是：一个端点坏了，但没有任何一处报错、没有一条测试变红——
 * 只有用户点按钮时弹出「未知错误」。所以这里把判定前移到**装配期**。
 *
 * ## 判据
 *
 * 只禁一件明确有害的事：**路由路径里含花括号正则参数**。理由是它只在
 * `RegExpRouter` 下有效，而当前应用的路由表结构决定了实际跑的是 `TrieRouter`
 * （见上第 2 点）。若将来路由表变了、`RegExpRouter` 能生效，这条断言也不会
 * 误伤——那时 `{...}` 是可用的，只是当前没有场景需要（跨斜杠参数可以用
 * 「客户端编码成单段」或「多条普通参数段路由」等价表达，且后两者两种路由器都支持）。
 *
 * ## 为什么不直接强制用 RegExpRouter
 *
 * 做不到：`tasks/queue`、`sessions/import` 这些静态段与参数段同层是**既有 API 契约**
 * （外部脚本与前端都在用），改成别的形状是破坏性变更；而 `RegExpRouter` 的这条限制
 * 是它的固有算法约束（trie 节点的正则拼接无法表达同层静态优先）。所以正确做法是
 * 「承认跑在 TrieRouter 上，并禁掉 TrieRouter 不支持的路由写法」，而不是假装没这回事。
 */

import type { Hono } from "hono"

/** 极简路由条目形状（Hono `app.routes` 的元素子集）。 */
interface RouteEntry {
  path: string
  method: string
}

/**
 * 只读地取「实际生效的路由器名 + 路由条数」（不抛错）。
 *
 * 为何要单独一个函数：`/api/health` 会在每次探活时调它，而 `assertRouteTable` 会扫全表并可能抛错——
 * 探活不应因为一条开发期约束失败。
 */
export function describeRouter(app: Hono<never> | Hono<any>): { router: string; total: number } {
  const routes = ((app as unknown as { routes?: RouteEntry[] }).routes ?? []) as RouteEntry[]
  const router = app.router as unknown as { activeRouter?: { name: string } }
  let active: string | undefined
  try {
    active = router.activeRouter?.name
  } catch {
    // 尚未发生过任何请求（SmartRouter 惰性选型）：探活时顺手推一把，拿不到就如实标「未定」
    try {
      ;(app.router as unknown as { match: (m: string, p: string) => unknown }).match("GET", "/__route_guard_probe__")
      active = router.activeRouter?.name
    } catch {
      active = undefined
    }
  }
  return { router: active ? `SmartRouter + ${active}` : "SmartRouter（未定）", total: routes.length }
}

/**
 * 检查路由表，返回诊断信息；有致命写法时抛错（让启动失败而不是带着坏路由上线）。
 *
 * 返回的 `router` 是**实际生效**的路由器名（`SmartRouter + TrieRouter` 这类），
 * 供 `/api/health` 透出——排查「某个端点为什么 404」时第一眼就能看到它。
 */
export function assertRouteTable(app: Hono<never> | Hono<any>): { router: string; total: number } {
  // routes 在 Hono 上是只读数组视图；类型因泛型不便直接取，按结构取一次
  const routes = ((app as unknown as { routes?: RouteEntry[] }).routes ?? []).slice()

  // 1) 花括号正则参数：只有 RegExpRouter 认，而实际生效的是 TrieRouter（见文件头）
  const regexParam = routes.filter((r) => r.path.includes("{"))
  if (regexParam.length) {
    throw new Error(
      [
        "路由表含跨斜杠正则参数（`{...}`），当前路由器不支持，该路由将永远 404：",
        ...regexParam.map((r) => `  ${r.method} ${r.path}`),
        "",
        "改法（两种路由器都支持，择一）：",
        "  · 参数由客户端 URL 编码成单段，路径写普通 `:key`（前端用 encodeURIComponent）",
        "  · 或写多条普通参数段路由（如 `:a/:b` 与 `:a/:b/:c`），在处理器里拼回完整语义",
        "详见 core/base/route-guard.ts 文件头。",
      ].join("\n"),
    )
  }

  // 2) 触发一次 match 让 SmartRouter 惰性定出实际路由器（此后不再变，路由已注册完）
  return describeRouter(app)
}
