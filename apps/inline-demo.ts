/**
 * 源码内联托管应用示例：**一个文件就是一个应用**（不必建目录、不必建静态文件）。
 *
 * 两种机制在这里同时出现，正是它们各自擅长的地方：
 * - `files`：文件内容是字符串——几行 HTML 就是界面；
 * - `backend`：后端是函数——同进程直接调用，不需要 `server.js`、不需要动态 import。
 *
 * 放进 `{仓库根}/apps/*.ts` 即被扫描到（重启生效；文件型后端与目录应用则是改文件即生效）。
 * 构建期 `scripts/build-apps-bundle.ts` 会把 `files` 的数据面内嵌进产物（函数型后端不进表，
 * 二进制形态需要后端时用目录应用的 `server.js`）。
 */
import type { InlineApp } from "@gebai/sdk"

export const apps: InlineApp[] = [
  {
    id: "inline-demo",
    title: "内联示例",
    description: "源码内联托管应用：HTML 是字符串、后端是函数（同进程调用）",
    version: "0.1.0",
    files: {
      "index.html": `<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>内联示例 · 托管应用</title>
<style>
  :root{color-scheme:light dark}
  body{font-family:system-ui,sans-serif;margin:0;min-height:100vh;display:grid;place-items:center;background:#0f1115;color:#e8e8ed}
  .card{width:min(520px,92vw);background:#171a21;border:1px solid #262b36;border-radius:16px;padding:26px}
  h1{font-size:19px;margin:0 0 6px}.sub{color:#8b93a3;font-size:13px;margin:0 0 18px}
  button{font:inherit;background:#3b82f6;border:0;border-radius:10px;padding:9px 16px;color:#fff;cursor:pointer}
  pre{margin:14px 0 0;background:#0f1115;border:1px solid #2c323e;border-radius:10px;padding:14px;font-size:12.5px;color:#9ae6b4;overflow:auto}
</style></head>
<body><div class="card">
  <h1>内联示例</h1>
  <p class="sub">这个应用没有目录、没有静态文件：HTML 写在 TS 字符串里，后端是一个函数。</p>
  <button id="go">调用后端</button>
  <pre id="out">（点一下）</pre>
</div>
<script>
  const out = document.getElementById("out")
  document.getElementById("go").onclick = async () => {
    out.textContent = "请求中…"
    const res = await fetch("/api/apps/inline-demo/ping")
    const text = await res.text()
    let data; try { data = JSON.parse(text) } catch { data = text }
    out.textContent = res.status + " " + res.statusText + "\\n\\n" + (typeof data === "string" ? data : JSON.stringify(data, null, 2))
  }
</script></body></html>`,
    },
    backend: (ctx) => {
      if (ctx.path === "ping") {
        return { pong: true, app: ctx.appId, at: new Date().toISOString(), user: ctx.user }
      }
      return ctx.respond({ error: `未定义的路由：${ctx.method} /${ctx.path}` }, { status: 404 })
    },
  },
]
