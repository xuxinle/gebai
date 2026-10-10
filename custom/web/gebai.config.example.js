/**
 * 歌白 Web UI 独立配置文件 —— **示例模板**（复制为 `gebai.config.js` 后生效）。
 *
 * 为什么是示例名：本目录是二开域，上游版本更新时会把 `custom/` 整体复制到新仓库根——生效文件
 * （`gebai.config.js` / `init.js`）不会被覆盖，示例文件（`*.example.js`）随上游刷新，供对照参考。
 *
 * 启用：把本文件复制/改名为 `gebai.config.js`，构建（或 vite dev）时自动带到前端产物根，
 * `index.html` / `files.html` 在入口模块脚本之前自动引入（相对路径，反代子路径下成立）。
 * 纯前端文件，改完刷新页面即生效（不需要重启服务）。
 *
 * 与同目录 `init.js` 的分工：本文件是**纯配置**（把宿主素材映射为歌白环境变量与设置），
 * `init.js` 是**初始化脚本**（页面加载即执行，可承担本地存储初始化、用户注册与登录等逻辑，
 * 也可覆盖本文件的配置）。两者可同时使用，执行顺序为先本文件、后 `init.js`；
 * 配置的读取与容错归一化见 `packages/web/src/boot-config.ts`。
 *
 * 生效优先级（本文件不覆盖用户在设置里的选择）：
 *   URL 参数 > 用户本次手动选择 > 浏览器本地存储既有值 > 本文件 > 服务端全局配置 > 内置默认
 * 即：`storage` 里的规则只在歌白对应键**尚未设置**时写入；确需强制统一口径（如部署方锁定主题）
 * 时给该项加 `force: true`。
 */
window.__GEBAI_WEB_CONFIG__ = {
  /**
   * ① 环境变量预置：随消息请求临时注入服务端（与「设置 → 环境变量」面板同一通道；
   *    仅存于本浏览器、不落盘到服务端）。服务端若配置了环境变量目录白名单，目录外的变量会被面板过滤。
   */
  env: {
    // GEBAI_LLM_MODEL: "local-qwen",
    // CODE_PROJECT: "my-project",
  },

  /**
   * ② 环境变量 ← 宿主系统 localStorage 键（运行时读取）：把已有系统的凭据/配置直接带进歌白环境变量。
   *    格式：{ 环境变量名: 宿主 localStorage 键 }。宿主键无值或读取失败时该项跳过。
   */
  envFromStorage: {
    // GEBAI_LLM_API_KEY: "myapp.llmKey",
  },

  /**
   * ③ 歌白设置 ← 宿主素材（沿用宿主系统的用户偏好，免二次配置）。
   *    值的写法：字符串 = 宿主 localStorage 键；或对象 { from } / { value } / { force }。
   *    可用键（歌白前端设置项）：
   *      gebai.ui.style         界面主题（acrylic/aether/cyberpunk/aurora/synthwave/matrix/tokyo-night/ink/cny/qinhan）
   *      gebai.ui.cnyScheme     人民币主题面额配色；值 "reset" 表示显式重置
   *      gebai.ui.acrylicLt     默认主题黑白（浅色/暗色）；值 "reset" 表示显式重置
   *      gebai.ui.lowPower      低性能模式（"on"/"off"）
   *      gebai.ui.fileDisplay   文件工具产物展示方式（"inline" 嵌入 / "popup" 弹窗）
   *      gebai.ui.approvalSkip  自动审批开关（"on"/"off"，仅对子Agent 只读工具生效）
   *      gebai.ui.env           浏览器本地环境变量（JSON 字符串，一般用上面的 env 更直观）
   */
  storage: {
    // "gebai.ui.style": "myapp.theme",
    // "gebai.ui.lowPower": { value: "on" },
    // "gebai.ui.approvalSkip": { from: "myapp.approvalSkip", force: true },
  },

  /**
   * ④ 外部链接携带提示词自动运行（URL 参数 `gb_prompt`，默认开启）：
   *    业务系统跳转链接可带任务进入歌白——自动新建会话并运行该提示词，随后地址栏重定向为会话地址
   *    （刷新只打开该会话，不会重复创建、重复执行）。置 false 关闭该入口。
   */
  // allowUrlPrompt: false,

  /**
   * ⑥ 隐藏内置登录页「注册账号」入口（默认开启）：置 false 后内置登录页不再展示注册切换，
   *    用户注册只能由本目录二开脚本（init.js 的 gebaiRegister 之类）或宿主系统引导完成。
   *    服务端配套 GEBAI_SIGNUP_SOURCE=custom 时，未带头的直调注册请求也会被 403 拒绝（双保险）。
   */
  // allowSignup: false,

  /**
   * ⑤ 二开初始化脚本（`init.js`）异步引导的等待上限（毫秒，默认 3000，0 = 不等待）：
   *    脚本可把 `window.__GEBAI_WEB_BOOT__`（Promise / 返回 Promise 的函数 / 数组）交给歌白，
   *    应用初始化最早期等待其完成；超时或异常只记控制台警告、不阻塞页面。需要更长的登录/拉取
   *    远端配置时间时调大该值（等待发生在启动动画期间）。
   */
  // bootTimeout: 3000,
}
