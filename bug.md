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


