/**
 * 歌白二开前端初始化脚本 —— **示例模板**（复制为 `init.js` 后生效）。
 *
 * 为什么是示例名：本目录是二开域，上游版本更新时会把 `custom/` 整体复制到新仓库根——生效文件
 * （`init.js` / `gebai.config.js`）不会被覆盖，示例文件（`*.example.js`）随上游刷新，供对照参考。
 *
 * 启用：把本文件复制/改名为 `init.js`（产物根名 `gebai.custom.js`），构建（或 vite dev）时自动
 * 带到前端产物并在入口模块脚本**之前**引入——页面解析到此行即同步执行，早于歌白的一切模块初始化
 * （主题、低功耗、文件展示、登录态恢复等均在其后），「本地存储初始化、用户注册与登录」这类必须先于
 * 应用初始化的动作放这里才可靠。未启用（无 `init.js`）时页面不引用它，歌白按内置默认行为运行。
 * 纯前端文件，改完刷新页面即生效（无需重启服务）。
 *
 * 运行环境：普通浏览器脚本（非 ES 模块、不参与打包），可用 window / document / localStorage /
 * sessionStorage / fetch 等浏览器 API；不支持 import（需要模块化组织时用立即执行函数）。
 *
 * 三个协作面：
 *   ① 本地存储初始化：直接读写 localStorage——歌白设置键（`gebai.ui.*`）、会话记忆键、外层业务系统键等。
 *      应用初始化期读同一批键，因此这里写入的值先于应用生效（用户此后的设置面板改动照常覆盖）。
 *   ② 用户注册与登录：调歌白 REST 接口（`/api/v1/auth/login`、`/api/v1/auth/register`、
 *      `/api/v1/auth/exchange`），拿到令牌写入 `localStorage["gebai.auth.token"]` 即完成登录
 *      （歌白启动时从该键恢复登录态）；也可只写宿主登录态，交给同源「外部身份兑换」流程自动兑换。
 *   ③ 配置预置：给 `window.__GEBAI_WEB_CONFIG__` 赋值，语义与 `gebai.config.js` 完全一致（见
 *      `gebai.config.example.js` 头注释）。两个文件可同时使用：`gebai.config.js`（配置）先执行，
 *      本脚本（初始化逻辑）后执行。
 *
 * 异步初始化（登录换取令牌、拉取远端配置等需要 await 的动作）：
 *   把 Promise 赋给 `window.__GEBAI_WEB_BOOT__`（支持 Promise、返回 Promise 的函数、或二者组成的数组），
 *   歌白会在应用初始化最早期等待它完成（默认最多 3000ms，可用配置项 `bootTimeout` 调整；超时或异常只记
 *   控制台警告、不阻塞页面）——保证登录态、预置设置赶在应用读取本地存储之前就位。
 *
 * 键名与接口清单见 DESIGN.md「业务系统集成」与 `custom/README.md`。
 */

/* ── ① 本地存储初始化 ─────────────────────────────────────────────────────────
 * 例：首次访问写入部署方默认 UI 风格与低功耗偏好（仅在该键尚无值时写入，不覆盖用户既有选择；
 *    需要强制统一口径时改用 gebai.config.js 的 storage 规则 + force: true）。
 */
// ;(function () {
//   try {
//     if (localStorage.getItem("gebai.ui.style") === null) localStorage.setItem("gebai.ui.style", "aether")
//     if (localStorage.getItem("gebai.ui.lowPower") === null) localStorage.setItem("gebai.ui.lowPower", "on")
//   } catch (e) {
//     /* 隐私模式 / 配额满：静默跳过（与歌白各模块同口径） */
//   }
// })()

/* ── ② 用户注册与登录 ─────────────────────────────────────────────────────────
 * 例 1（推荐，免改上游）：只写宿主登录态，交给歌白「外部身份兑换」自动换令牌。
 *   需服务端配置 GEBAI_EXTERNAL_AUTH_*；键名为 GEBAI_EXTERNAL_AUTH_STORAGE_KEY 指定的宿主键，
 *   值支持 JSON `{"username":"u","credential":"c"}` 或字符串 `"u:c"`。歌白启动时自动兑换并持久化令牌。
 */
// ;(function () {
//   try {
//     const sso = JSON.parse(localStorage.getItem("myapp.sso") || "null")
//     if (sso && sso.user && sso.ticket) localStorage.setItem("myapp.gebaiCred", JSON.stringify({ username: sso.user, credential: sso.ticket }))
//   } catch (e) { /* 宿主登录态不可用：静默跳过 */ }
// })()

/**
 * 例 2：直接调登录接口换取令牌（服务模式；用户名密码登录）。
 * 成功即写入 `gebai.auth.token`——歌白启动时据此恢复登录态，无需用户再填登录框。
 */
// async function gebaiLogin(username, password) {
//   const res = await fetch("/api/v1/auth/login", {
//     method: "POST",
//     headers: { "Content-Type": "application/json" },
//     body: JSON.stringify({ username, password }),
//   })
//   if (!res.ok) throw new Error("登录失败：" + res.status)
//   localStorage.setItem("gebai.auth.token", (await res.json()).token)
// }

/**
 * 例 3：注册账号（仅服务模式开放；signupMode=approval 时返回 pending: true，须管理员审批后方可登录）。
 */
// async function gebaiRegister(username, password) {
//   const res = await fetch("/api/v1/auth/register", {
//     method: "POST",
//     headers: { "Content-Type": "application/json" },
//     body: JSON.stringify({ username, password }),
//   })
//   const data = await res.json()
//   if (!res.ok) throw new Error(data.error || "注册失败")
//   if (data.pending) return { pending: true }
//   localStorage.setItem("gebai.auth.token", data.token)
//   return { pending: false }
// }

/* ── ③ 异步初始化入口 ─────────────────────────────────────────────────────────
 * 把需要 await 的初始化交给歌白等待：下面示例在宿主令牌可用时自动登录，失败不影响页面加载。
 * （也可直接赋值单个 Promise 或返回 Promise 的函数；值为非 Promise 时忽略并记警告。）
 */
// window.__GEBAI_WEB_BOOT__ = async () => {
//   try {
//     const sso = JSON.parse(localStorage.getItem("myapp.sso") || "null")
//     if (!sso || localStorage.getItem("gebai.auth.token")) return
//     await gebaiLogin(sso.user, sso.ticket)
//   } catch (err) {
//     console.warn("[gebai custom] 自动登录未完成，回落登录页：", err)
//   }
// }

/* ── ④ 配置预置（等价于 gebai.config.js）─────────────────────────────────────
 * 需要把宿主 localStorage 映射成歌白设置 / 环境变量时，赋这个键即可（字段说明见 gebai.config.example.js）：
 */
// window.__GEBAI_WEB_CONFIG__ = {
//   env: { GEBAI_LLM_MODEL: "local-qwen" },
//   storage: { "gebai.ui.approvalSkip": { from: "myapp.approvalSkip", force: true } },
// }
