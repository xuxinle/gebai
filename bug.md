1. 
根因与修复
根因：packages/server/src/sub-agents/reverse_site/reverse_site.ts:14 在模块作用域执行 new Bridge()，而 Bridge 构造函数会经 playwrightModuleUrl() 调用 Bun.resolveSync("playwright", ...)。编译进 exe 后这类包解析锚定真实 CWD 的 node_modules 可达性：从 repo 目录跑能找到 node_modules/playwright，桌面 launcher 以 %LOCALAPPDATA%\gebai 为 CWD spawn 时找不到 → 抛 ResolveMessage: Cannot find package 'playwright' from 'B:\~BUN\root' → 整个 bundle 注册表初始化失败 → 服务端启动即退出（launcher 报 "exited without reporting a port"）。playwright.ts:14 经工厂默认 new Bridge() 存在同样的隐患（之前的 require/import 改动方向是错的，真正的坑是 bundle 图内模块作用域的包解析）。

修复（4 个文件 + DESIGN.md 同步）：

playwright_tools.ts：新增 createLazyBridge() —— 首次工具调用才构造真实 Bridge（解析失败降级为工具级运行时错误），并作为两个工厂的默认实现。
reverse_site.ts：模块作用域 new Bridge() 改为共享的 createLazyBridge()（仍与 capture 工具共享同一桥接进程/浏览器会话）。
reverse_site_tools.ts：工厂默认同样改惰性。
subagents.ts：移除 debug 插桩，恢复简洁报错（保留注册表加载失败必抛错、绝不静默降级为空列表的语义）。
DESIGN.md「打包闭环」：更正为准确机制——bundle 静态内嵌 + bundle 图内子 Agent 模块禁止模块作用域第三方包解析（playwright 必须经 createLazyBridge 惰性化）。



按消息条数触发的机制：100 条截断（不是摘要）
store.ts:7 的 MAX_CACHE_MESSAGES = 100：chat.json 里消息超过 100 条时，从最早的非受保护消息（assistant/tool）开始丢弃到 100 条（store.ts:248-265）。它不生成摘要、不调 LLM，纯粹是存储上的上限修剪——所以如果你看到旧消息凭空消失、没有"已压缩 N 条"的通知，那就是它干的。

工具调用密集的会话（一次任务往往产生很多轮 assistant+tool 消息对）条数涨得飞快，100 条上限很快触达，这很可能就是你观察到"远没到上下文大小就开始丢东西"的直接原因。



推荐方案：playwright-core 内嵌 + 驱动系统自带 Edge
思路转变关键点：浏览器本体不用内嵌，用 Windows 系统自带的 Edge（Win10/11 必装，Chromium 内核）。playwright 支持 chromium.launch({ channel: "msedge" }) 直接驱动系统 Edge，不需要 playwright install 下载任何浏览器。

只内嵌 playwright-core（12.8 MB，零外部依赖）——比内嵌完整 playwright 还省 4.8 MB 的壳层，它的 API（chromium/page/网络录制）和现在 driver.mjs 用的完全一致，driver 业务逻辑一行业务代码都不用改，只把 init 注入的模块路径换掉 + launch 加一个 channel。
桌面端（Windows）：系统 Edge → 完全离线、零下载、零 CWD 依赖，比之前「库内置 + 在线下载浏览器」的方案更强。
Linux 服务端部署：没有系统 Edge，走真实 chromium（下载到 ~/.cache/ms-playwright）——解析顺序回退即可，两种形态共存。
改造点和你上一个问题的方案完全重合：build 脚本内嵌 playwright-core → playwrightModuleUrl() 解析顺序回退 → 物化到 {GEBAI_HOME}/vendor/。channel 由 Bun 侧按平台注入（Windows 默认 msedge）。



桌面端每次重启监听端口都随机变化，localStorage 按"协议+主机+端口"隔离，新端口下 WebView 相当于换了一个全新的站点，所有浏览器本地数据（环境变量、主题、快捷键等）都"看起来丢失"了。

证据链
环境变量本来就只存在浏览器本地，服务端零留存（这是有意设计）：
packages/server/src/core/env.ts:56-71 cleanupLegacyUserEnv 启动时还会主动清掉旧版服务端 env 文件，注释写明「用户环境变量现仅存浏览器本地（服务端零留存）」；
packages/web/src/env-local.ts:2-5 存在 localStorage 键 gebai.ui.env，发送消息时随 prompt 临时注入；
DESIGN.md「环境变量隔离」确认：浏览器 localStorage（gebai.ui.env）是用户环境变量唯一持久化位置，防密钥在服务端泄露。所以服务端没有任何兜底可恢复。
桌面端每次启动用随机端口：
packages/desktop/src/index.ts:11：runDesktop 默认 port = 0（OS 分配空闲端口），注释点明这是为了「根治与 dev 服务/其他实例的 3000 端口冲突」；
该入口编译为 dist/gebai.exe，由 launcher（packages/desktop/launcher/src/main.rs:161-204）spawn，从 stdout 日志解析实际端口，WebView 再导航到 http://127.0.0.1:{这次随机出的端口}；
下次重启又随机一个新端口 → 整个 origin 不同。
localStorage 按 origin 隔离：WebView2 数据目录（%LOCALAPPDATA%\gebai\webview，main.rs:58）本身是持久化的，但存储键按「协议+主机+端口」划分。端口一变，新 origin 下 localStorage 就是空白，loadLocalEnv() 返回 {}。
为什么开发模式没这问题：bun run dev（packages/server/src/index.ts → startServer()）走默认端口 3000，origin 固定，localStorage 正常保留——所以这个现象只在桌面 WebView 形态出现。
影响范围
不止环境变量：所有 localStorage 用户数据都会丢——主题/黑白切换（gebai.ui.style 等）、快捷按钮、输入历史、侧栏折叠状态、自动审批/极简模式开关等（packages/web/src/theme.ts、composer.ts、sessions.ts 等）。用户最在意的是 env，但根因是同一处。

修复建议
推荐改 packages/desktop/src/index.ts 的端口选择逻辑：端口持久化——首次启动选一个空闲端口写入用户数据目录（如 %LOCALAPPDATA%\gebai\port 或 GEBAI_HOME），之后重启复用；被占用（EADDRINUSE）才回退随机并更新记录。这样保留了"随机端口防冲突"的初衷，又让 origin 稳定，localStorage 自然持久化。launcher 的 Rust 代码不用动（它只解析端口）。
