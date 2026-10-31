# 功能列表 / 桌面端 / 构建 / 路线 / 测试 / 常量

> 本册是 `DESIGN.md` 的分册，**章节号与主文件保持一致**（源码注释按 `DESIGN.md §x.y` 引用章节号，不重编号）。
> 索引与全貌见 [`../DESIGN.md`](../DESIGN.md)。

## 功能列表

### 总Agent全局工具

| 工具 | 功能 | 默认审批 |
|------|------|---------|
| `read` | 读取文件内容（相对路径以会话 `tmp/` 为基准，`tmp/` 前缀可省略；服务端部署受沙箱限制，桌面/本地浏览器不限制，见路径基准）；可选 `offset`（起始行号，1 起始）与 `limit`（行数，正数取 offset 起 N 行、负数取末尾 N 行），按行切片便于大文件分段阅读；**默认每行前缀真实行号**（`line_numbers` 默认 true——cat -n 风格右对齐+制表符，切片后仍对应文件行号，按 文件:行号 引用/定位的基准；不需要可传 false；复制原文给 `edit` 的 old_string 时须去掉行号前缀——edit 检测到行号前缀误拷贝会给明确提示）；`offset`/`limit` 切片读取附尾部位置注记（`（第 X–Y 行，共 N 行）`），模型据此判断剩余内容与下一段 offset；**编码自动识别**（BOM/UTF-16 LE·BE/UTF-8/GBK 按字节探测解码，非 UTF-8 文件不再按 UTF-8 读成乱码，附编码注记；不可识别回落文本通道）；**`encoding` 指定编码解码读取**（如 `gbk`——file info 探测为非 UTF-8 时用，fatal 解码失败给明确报错；仅解码读取，转码改写用 `py`）；**UTF-8 BOM 不进输出**（底层 readFile 用 node utf8 保留 BOM、`Bun.file().text()` 会剥离，工具层统一 `stripBom` 去除——`edit` 匹配用干净正文（BOM 会让首行 oldString 匹配失败）、写回时按原文件有无 BOM 补回，`write` 覆盖写同样保留——BOM 文件（Windows 工具生成常见）往返编辑不丢头）；读目录给出可读引导（用 `ls`/`glob`，不再抛原始 EISDIR）；**图片文件（png/jpg/jpeg/gif/webp）不以文本读取**——主模型多模态时二进制读入并内联进工具结果消息（`ToolResult.images` → 统一 image 块，模型直接可见，无需 vision），非多模态返回说明 + vision 指引；轻量引用随工具消息落盘（`Message.images`），历史重建按引用重读内联（同附件图片的最近窗口与降级规则，见「多模态支持」）；svg 为文本正常读取、bmp 不在白名单给转换引导；读取成功登记「本会话已读」（防盲写守卫依据，见「防盲写守卫」） | 否 |
| `write` | 写入文件（默认整体覆盖；`append:true` 追加模式——内容接在文件末尾，不存在则新建）。相对路径以会话 `tmp/` 为基准（`tmp/` 前缀可省略，受沙箱限制）；目标文件**已存在且本会话未 read 过**时拒绝（防盲覆盖，先 read 再覆盖，覆盖/追加同规则，见「防盲写守卫」）；**已存在但内容自上次读取/写入后被修改过**（并行分支/脚本命令/外部编辑）同样拒绝（防陈旧覆盖，重新 read 后再写）；**非 UTF-8 目标文件（GBK/UTF-16）拒绝整体写入**（write 恒按 UTF-8 落盘会破坏原编码——引导转码或改用 `edit` 按原编码写回）；**内容护栏：拒绝把 `read` 的截断提示文本当内容写回**（content 以 `[输出超长，已截断` 开头即拒——那段文本是提示、不是文件内容，写下去会把整个文件覆盖成一段提示语；标记常量见 `core/support/truncate.ts` 的 `TRUNCATION_MARKER`，与提示文案同源）；**大文件（约 300 行以上）分段写入**——首段普通 write、后续段 `append:true` 续写（每段 200~300 行），防单次模型输出过长被输出上限截断或接口超时；**同一路径的写入按队列串行**（读旧值 → 守卫判定 → 计算 → 落盘 → 登记指纹整段串行）——脚本内 `Promise.all` 并行 append/写同一文件不再互相覆盖丢行（并发 append 按调用顺序全部保留；仅给写盘加锁不够：并发调用各自读到同一份旧值仍会覆盖） | 否 |
| `ls` | 列出目录内容（文件/子目录、大小） | 否 |
| `grep` | 按正则表达式在会话工作目录（`tmp/`）中递归搜索文本内容（返回 文件:行号: 匹配行——路径带 `tmp/` 前缀可直接用于文件工具，限文件大小与匹配数）；`output` 三种结果形态（`content` 逐行内容 / `files` 仅命中文件清单——宽泛摸底定位优先 / `count` 每文件命中行数）、`context` 附匹配行前后上下文（格式同 `grep -n -C`：匹配行 `文件:行号:` 前缀、上下文行 `文件-行号-` 前缀、组间 `--` 分隔；`contextBefore`/`contextAfter` 可指定**非对称**上下文（同 `-B`/`-A`——看定义后的实现体常用），指定时覆盖 `context` 对应侧）、`literal:true` 按字面匹配（正则元字符自动转义——搜索 `foo.bar(` 类代码片段免转义）、`include`/`exclude` 按路径 glob 过滤/排除（逗号分隔多模式、`{a,b}` 花括号交替；无 `/` 的模式按目录/文件名匹配任意层级）、`head_limit` 压低匹配上限先看一部分；node_modules/.git/dist 等大型目录默认跳过（include 原文显式点名除外）（见「grep 内容检索工具」） | 否 |
| `glob` | 按文件名模式（glob：`*`/`**` 跨目录、`?` 单字符、`{a,b}` 花括号交替）在会话工作目录（`tmp/`）递归查找文件（path 可限定子目录，`tmp/` 前缀可省略，与 `read`/`write` 同一路径解析规则；返回路径带 `tmp/` 前缀，可直接用于文件工具）；`exclude` 排除路径模式（与 grep 同语法）；**`**/` 可匹配零层目录**（`**/*` 同时命中根级与子目录文件——`**/test/*.js` 命中根级 `test/`，与 `core/fs/service.ts` 的 glob 语义一致；旧实现把 `**` 后的 `/` 当字面量，根级文件全漏）；node_modules/.git/dist 等大型目录默认跳过（模式显式点名除外）；**本地模式下 `path` 可传 `tmp/` 外路径并实际遍历搜索**（`walkDirFiles`：跳过大型/生成目录、深度上限 10；按给定前缀输出路径可直接用于文件工具；模式与 `exclude` 统一按**相对给定 path 的路径**判定——否则 `exclude: "src/**"` 这类含分隔符的模式对绝对路径永不命中；`path` 指向单个文件时按文件名匹配；路径不存在或无可列文件时**明确报错**，不再与「目录内无匹配」同义）；沙箱模式拒绝越界 | 否 |
| `file` | **文件管理（单工具多动作）**：`copy` 复制文件（`to` 含目标文件名，二进制通道支持任意类型、父目录自动创建，≤100MB）/ `rename` 重命名（同目录改名，`new_name` 仅名字不含路径——含分隔符拒绝防越界，跨目录用 move）/ `move` 移动或跨目录改名（`to` 含目标文件名，父目录不存在自动创建，与 `write` 一致）/ `mkdir` 新建目录（递归，已存在幂等不报错）/ `delete` 删除文件或目录（递归，不可恢复）/ `info` 查看文件信息——**按内容探测**（类似 `file` 命令，读头部 1KB）：魔数识别实际类型（图片/压缩包/Office/PDF/可执行/SQLite/Java class 等）、文本 vs 二进制判定（二进制勿盲 read）、编码检测（UTF-8/BOM/UTF-16/疑似 GBK——GBK 用 `read` 的 `encoding=gbk` 读取）、shebang 解释器；**扩展名与实际内容不符时显式提示**（`data.extMismatch`）；附人类可读大小与修改时间，目录附直接子条目数。写动作（copy/rename/move/mkdir/delete）走写范围守卫；与 `ls`（目录列表）分工——`ls` 单独保留；写动作走写范围守卫 | **否**（`delete` 动作动态需审批） |
| `edit` | **精确修改文件**：`old_string` → `new_string` 定点替换（可多处），或 `pattern`（正则）→ `new_string`——二者二选一，正则项配 `regex_flags`（`g/i/m/s/u/y`，`g` 自动补齐）与 `$&`/`$1..$9`/`$$` 捕获引用，大段原文只改少量字符时用正则省去整段重发；替换前校验匹配与**唯一性**（多处命中报错列出行号，或该项 `replace_all: true` 全部替换），失败即报错不落盘；空 `edits`、项非对象、缺 `new_string`、`old_string` 与 `pattern` 同给、`old_string` 为空或等于 `new_string` 均拒绝；目标文件已存在但本会话未 read 过时拒绝（防盲改守卫，与 write 同规则）；**编码感知**（BOM/UTF-8/UTF-16 LE·BE/GBK 自动探测，按原编码写回——GBK 仅支持命中区域与替换文本均纯 ASCII，否则明确拒绝并引导转码，不再静默写坏）；**行尾健壮**：CRLF/裸 CR 文件 × LF 原文片段在 LF 归一空间匹配（双向自适应），写回按源字符区间拼接——未修改区域字节级保留（混合行尾不被整文件改写），仅替换文本按文件主导行尾；`old_string` 误携 read 行号前缀时自动剥离；匹配失败时检测空白/缩进近似原文并给提示；成功回报各处应用行号（见「edit 修改工具」） | 否 |
| `restart_server` | 重启本服务进程（仅本地模式注入，见「restart_server」；支持 `action=status` 只读查询与重启后续跑 `prompt`；**重启动作执行后本轮任务即结束**——结果不回灌模型，续跑经 `prompt` 接续） | **是** |
| `tool_schemas` | **批量获取工具 schema**：按工具名列表返回输入参数与结构化输出（`data`）的 JSON Schema；省略时返回全部已启用工具的输出结构概要——js 编排前理解输出结构，避免逐个试调 | 否 |
| `agent_list` | 列出可用子Agent（名称/描述/是否已装载；**不列工具名**，工具名以注册的工具集为准）。**不注册进总Agent 全局工具集**——未装载清单已由 `systemPromptInjection` 注入提示词（模型上下文已有，工具冗余且干扰工具选择）；仅在子会话运行环境注入（纯 md 组合子Agent 自动注入编排工具时，见「子Agent文件格式」） | 否 |
| `agent_load` | **装载**子Agent 能力模块（类比 import 子模块：工具并入当前工具集、**完整系统提示词作为 system 消息写入会话记录**（持久化，恢复会话自动还原），**不创建独立上下文**；默认使用方式：装载后直接用其工具，仅在需要干净上下文或防膨胀时才改用 `subsession_run` 子会话运行；装载反馈**不枚举工具清单**——`{agent}_*` 工具 schema 已注册进工具集（下一轮请求即全量下发），再列一遍是冗余） | 否 |
| `subsession_run` | **子会话运行**（无需装载，一套入口覆盖两种形态）：`inherit_context:false`（缺省）= 派生子会话执行独立子任务，`agents` 可省略/为空（不加载任何子Agent），中间过程/推理/内部工具不进父上下文、全程存档可回放，最终结果作为工具结果返回（异步经 `bg_task` 取回）；`inherit_context:true` = 从父会话当前上下文 fork，报告自动合入父上下文。**默认与父会话同构**——`inherit_global_tools` 与 `inherit_global_prompt` 默认均为 true（全局工具同名同参注册 + 父会话全局系统提示词前缀注入，子Agent 只提供独有能力；false 分别裁剪）；**`async` 参数**：true 后台异步执行——立即返回 runId（`s` 前缀）不阻塞，子会话内可 `subsession_merge` 主动合入阶段性成果（长任务先做别的，见「子会话运行」）；默认优先 `agent_load` 装载后直接用其工具 | 否 |
| `subsession_run` | **子会话运行（统一父子会话模型）**：一套入口覆盖两种形态——`inherit_context:false`（缺省）= 派生子会话执行独立子任务（预加载子Agent 可省略；最终结果作为工具结果返回或经 `bg_task` 取回）；`inherit_context:true` = 从**父会话当前上下文** fork 子会话（父消息历史 + 系统提示词 + 工具面快照），最终报告**自动合入父上下文**（合并消息 + 过程存档，父会话下轮即见）——同一任务的并行多路探索/执行，像 git 一样不停 fork/合并摆脱单轮串行的模型服务速度限制。单任务用 `input`（+可选 `agents`/`model`）、多任务并发用 `subsessions`（1-8 项，每项 `{ name?（缺省 s1..sN，批内唯一，≤32 字符不含空白、中文名合法）, input, agents?, model? }`，两形态二选一）；`model` 走**模型路由**（`GEBAI_LLM_ROUTES` 命名路由走独立端点，多路接口并行）；子会话内用 `subsession_merge`（**仅异步运行注入**）双向同步父会话（传 content 交出阶段性成果并继续运行/不传拉取父会话完整增量），父会话与兄弟子会话进展以通知注入**互相感知**（见「子会话运行」）；`merge` 可选合入粒度（缺省 `full` 全文；`summary` 摘要合入——长报告压成结论要点进父会话、全文留过程存档）；**环境变量自定义** `env` + `env_mode`（`inherit` 缺省＝父任务环境叠加自定义项；`clear`＝仅自定义项；同时决定工具可见环境与 **Provider 解析**，可用它把子会话指向另一端点/模型，优先级高于 `model` 路由）；默认阻塞等全部完成（结果为概要，隔离形态附最终结果、继承形态全文在随后的合并消息），`async:true` 后台执行——立即返回 runId（`s` + 8 位 hex），`bg_task`（s 前缀）管理 | 否 |
| `session` | **会话管控（跨会话控制平面，脱管）**：`create`（新建会话，可选 `name`/`env` 预置内存态会话 env）/ `list`（本用户会话清单含运行态）/ `status`（单会话详情：运行态+消息尾部+待决交互+ctx 水位）/ `send`（注入用户消息触发执行，注入即返回；**本轮缺省免审**——`approve:false` 可收紧为按需审批，`interactive:false` 关前端交互能力；目标运行中拒绝）/ `stop`（软终止运行中任务）/ `delete`（进 GC 归档；运行中拒绝）。与 `subsession_run` 分工：目标会话与当前上下文**无父子绑定**（脱管——subsession_run 的合并/等待语义不适用，结果经 status 轮询交互）；**自保护**：对当前会话自身一切动作拒绝；服务模式仅本用户会话可见（见「会话管控」） | 否 |
| `bg_task` | **后台异步任务统一管理**（三类同构管理面合并，**按 id 前缀自动识别**，无需指定类型——旧 `sh_task`/`agent_task` 已合并为本工具）：**命令任务**（`sh` 的统一后台机制：`async:true` 启动或同步等待超时转后台，taskId 形如 `tXXXXXXXX`）、**子会话运行**（`subsession_run async:true` 启动，runId 形如 `sXXXXXXXX`）与**通用后台任务**（如 `triage_run mode=async` 启动，jobId 形如 `jXXXXXXXX`）；见「sh 执行」）——`status` 返回状态与 stdout+stderr 合并日志尾部（`tail` 参数默认 4000 上限 20000 字符，完整日志 `sh-tasks/{id}.log`，相对会话工作目录）；**子会话运行**（`subsession_run async:true` 启动，runId 形如 `sXXXXXXXX`，见「子会话运行」）——`status` 返回进度（已执行轮次/工具调用/最近活动，已结束含最终结果），`wait` 完成时取回最终结果与完整存档（挂执行记录扩展字段供历史回放；继承形态报告完成即自动合入父上下文，无需取回动作），`stop` 终止且该子会话不合入（已执行过程保留在存档）。公共动作：`wait` 阻塞等待完成（`timeout` 秒内未完成返回当前状态，默认/上限 60 秒——「先做别的再回头等结果」，上限 1 分钟强制按进度轮询）；`stop` 终止（命令任务杀进程树、子会话运行协作中止且已执行过程保留在存档）；`list` 三类合并列出本会话全部后台任务（按启动顺序） | 否 |
| `sh` | 执行Shell命令（**Windows 经 PowerShell、POSIX 经 `bash -c`，命令按所在平台的 shell 语法书写；退出码读返回结果的 `exitCode` 字段，无需在命令里输出**；**`workdir` 参数**：命令工作目录（相对路径基于会话工作目录/项目根解析——替代在命令里串联 `cd`；async 后台任务同以该目录为 cwd；非默认工作目录执行时输出末尾标注「（工作目录: …）」）；**`input` 参数**：stdin 输入，对象/数组自动序列化为 JSON 文本（双引号，脚本 `json.loads` 可解析）；**执行语义**：命令统一经后台任务机制执行（见「sh 执行」）——**`timeout` 参数：同步等待窗口秒数，默认 60、上限 120，窗口内结束按同步返回，**超窗口命令不被终止**而是转后台返回 `taskId`（用 `bg_task` 跟踪；`async:true` 时该参数为任务生命周期上限：默认 1800、上限 3600）**；**`strict` 参数**：true 时非 0 退出码抛工具级错误（js 编排「非 0 即中断」，默认 false 非 0 退出作为正常结果返回，exitCode 在结构化输出；同步等待超时转后台时任务未结束、退出码未知，strict 不触发）；**`async` 参数**：true 后台异步执行——立即返回 taskId 不阻塞（长耗时构建/测试先做其他事再回头查询，见「sh 执行」）；**`approval` 参数**：本次调用是否需审批，默认 true，明确安全的只读/幂等命令可传 false 按次免审（见「工具审批」）；可运行 `bun run`/`node`；JS/TS 亦可通过内置运行时 `gebai exec` 自执行，见「脚本执行环境」） | **是**（默认；`approval:false` 按次免审） |
| `py` | 执行Python代码（**`input` 参数同 `sh`**——本地模式工具桥下对象/数组原样注入 `input` 变量（与 js 的 `input` 语义对齐，不再经 schema 强制序列化为 JSON 文本；值为 JSON 对象/数组字面量文本时自动解析，两种传法等价）；**`timeout` 参数**：单次执行超时秒数（默认 300、上限 540，超时杀进程并返回超时结果——与 `sh` 的同步等待窗口语义不同）；**`strict` 参数同 `sh`**；`approval` 为**兼容参数**：`code` 为任意代码、无法静态判定安全性，**免审标记不生效——恒需审批**） | **是**（恒需审批，不接受 `approval:false` 免审） |
| `js` | **执行 JS/TS 脚本（工具动态编程）**：Bun 子进程运行，脚本内工具**像内置函数一样直接调用**——`await read(params)`（已启用工具名即顶层函数，动态名字 `tools.call`）+ `ctx` **注入会话上下文**（user/sessionId/workdir/home/sandboxed/env/projects/messages 最近消息快照）+ `input`（编排传入；值为 JSON 对象/数组字面量文本时自动解析为对象/数组，与 py 同规则）；console 输出即工具输出，`return` 值进 `data.result`；`strict`/`approval` 参数同 `sh`（`timeout` 为单次执行超时秒数：默认 300、上限 540，超时杀进程——与 `sh` 的同步等待窗口语义不同，见「js 脚本工具」） | **是**（默认；`approval:false` 按次免审） |
| `show` | **向用户展示内容的统一入口**（原 `draw`/`render_html`/`show_file` 三工具合并，参数面统一为 `name`/`format`/`content`/`path`，**内容与路径二选一**）：①**图表**——`content` + 图表语言 `format`（必选：`mermaid`/`plantuml`/`d2`/`echarts`，工具描述与参数说明内置选择指南——Mermaid 通用图表首选、PlantUML 标准 UML 严谨建模、D2 美观架构图/对外展示、ECharts 数据可视化/统计图表（option 的严格 JSON，值禁止函数）），渲染成功才返回成功、报错回传模型、5 秒超时判定画图能力受限；Web 前端本地渲染（配色跟随 UI 主题）/飞书桥接后端渲染 PNG/`render=backend` 服务端渲染 PNG（返回 `image` 内容块）；产物保存会话 `tmp/` 并返回 `diagram` 内容块（`format` 字段携带图表语言）；`path` 指向已有图表文件直接渲染（图表名取文件主名、format 按扩展名推断，不重发源码）；**PlantUML 布局规范内置于工具描述**（未设置间距时自动注入 `skinparam ranksep 80`/`nodesep 40` 兜底，见「内容展示」）；②**HTML 页面**——`content`（HTML 源码）+ `format: "html"` 直接在聊天界面渲染展示（沙箱 iframe 域隔离预览：脚本可执行、隔离于宿主页面；适合网页原型/数据报表/可视化组件/带交互脚本的小页面），落盘 `tmp/` 并返回 `html` 内容块，可选微调 `width`/`height` 预览尺寸；③**文件直显**——`path`（与 `content` 二选一）按**文件真实类型**产出直显内容块（显式 `format` 时按该格式解释：图表语言 → 渲染该文件、`html` → 页面预览；与 `name` 无关：图片内联、图表源文件渲染图表、`.html` 页面预览、markdown（`.md`/`.markdown`）**渲染为文档而非源码高亮**、音频/视频在文件卡内联播放（原生控件可拖进度）、其余文本/代码语法高亮内联（无扩展名文件如 `LICENSE`/`Makefile`/`Dockerfile`/`.gitignore` 按内容探测后同样内联；超 4 万字符截断 + 文件卡片取全文）、其余类型（PDF/压缩包/Office）查看/下载卡片）；会话 `tmp/` 内直接引用，会话外文件复制 `tmp/shown/{主名}-{内容哈希}.{扩展名}`（≤100MB）后引用（见「内容展示」）；`card.args="block"` 声明（调用不显示通用卡片，结果直出内容块）；**分支门控**（html 分支仅实时通道、图表分支无交互模式直接引导 `render=backend`、path 分支全模式可用，见「内容展示」） | 否 |
| `fetch_url` | 抓取 URL 内容（网页/API/文档；服务端部署模式限制公网地址防 SSRF 并逐跳校验重定向，响应超阈值截断） | 否 |
| `vision` | **视觉分析**：把图片文件交给多模态（视觉）模型分析（参数 `target` 目标 + `image` 图片文件路径；模型选择 `GEBAI_VISION_*` 额外视觉模型，未配置回落到声明多模态能力的主模型；base64 内联传输，单图 8MB 上限），返回分析文本并携带 `image` 内容块 | 否 |
| `todo` | 待办管理（统一入口）：`entries` 操作列表，每项 `op=add/update/delete`——add 新建（需 title，可带 priority/note/eta），update 按 id 改 status/progress/title，delete 按 id 删除；省略/空数组 = 查询；返回操作摘要与当前全部待办状态。**终态项（completed/cancelled/failed）在操作后的快照里只保留最近 3 条**（其余压成一行计数，避免清单随任务时长无界膨胀），显式查询（空 entries）时全量展开 | 否 |
| `ask` | **向用户询问并阻塞等待回应的统一入口**（原 ask_user/ask_env/plan 三工具合并，分支按专属参数三选一）：①选项询问——`prompt`+`options`（multi 多选、复杂选项 title+description、自定义文本/拒绝均可），用户点选后结果返回模型据此继续，见「用户询问」；②环境变量填值——`name`+`description`+`secret`，前端弹窗填值后注入本次任务 env 并保存浏览器本地（用户拒绝/超时返回失败），见「环境变量」；③计划审批——`title`+`steps`/`content`，计划落盘 `plans/{标题}.md`（相对会话工作目录），`plan` 载荷随事件内嵌进选择卡（审批时直接可见；等待期消息流不重复预览计划卡，决策后落计划卡呈现结果态），阻塞等待批准/拒绝（拒绝可附修改意见），`data` 返回 `{status,title,path,feedback}`；**分支门控**：填值分支仅实时通道、选择/计划分支无交互模式报「无交互能力」（见「用户询问」）；`card.args="none"`（问答记录卡/计划卡/填值卡各有专属 UI） | 否 |

> **`current_time` 已移除**：时间获取经 `sh`/`py`/`js` 脚本按需完成（如 `sh` 执行 `date`），不占全局工具位；不注入时间相关提示词引导——模型自行选用现有工具。

> **全局工具集最小化**：编码工作流专属工具不注册为全局工具——`git`（只读变更核对，`code_git`/`explore_git` 命名空间暴露）与 `preview_server`/`env_detect`/`system_info`（验证服务与环境/工具链探测，`code_preview_server`/`code_env_detect`/`code_system_info`）由 `code`/`explore` 子Agent 按需暴露（`self_optimize` 经连带装载 code 一并获得）；反馈读取（`read_feedback`）下沉 `self_optimize`（`self_optimize_read_feedback`，自我优化专属输入通道）；定时任务（原全局 `cron_add`/`cron_list`/`cron_update`/`cron_remove`）下沉 `cron` 子Agent（命名空间不变并补 `cron_trigger` 手动触发，见「cron 子Agent」）；`delete_file`/`move_file` 合并为全局 `file` 多动作工具——全局集只保留高频基线与编排/交互入口，低频域工具下沉子Agent。

> **路径基准（统一）**：文件工具（`read`/`write`/`edit`/`ls`/`grep`/`glob`/`file`/`show`/`vision` 等）的相对路径统一以**会话 `tmp/` 子目录**为基准——与 `sh`/`py`/`js` 子进程工作目录（cwd）、`glob`/`grep` 搜索范围、UI 文件面板范围完全一致，跨工具传递路径无需考虑前缀（`write("a.txt")` 产物 `sh` 直接可见、`glob`/`grep` 直接命中）；**js 脚本内裸 fs/`Bun.write` 的相对路径同样落在会话 `tmp/`——勿再带 `tmp/` 前缀**（会多套一层写入 `tmp/tmp/…`），`tmp/` 前缀别名仅工具参数层剥离（js 工具描述内置此说明）。带 `tmp/` 前缀的逻辑路径（列表/附件/截断产物契约，如 `tmp/a.txt`）剥离前缀后解析，两种写法等价（`stripTmpPrefix`）；`glob`/`grep` 的 `path`/`include` 过滤同样兼容两种写法（列表坐标双候选匹配）。沙箱模式限定会话 `tmp/` 内（桌面/本地浏览器放开，绝对路径直用）。

> **项目路由（`project` 参数）**：全局文件工具（read/write/edit/ls/grep/glob/file/sh/py）与编码类子Agent 独有工具统一携带可选 `project` 参数——传**预置项目名**（`{AGENT}_PROJECTS` 清单项）或**项目根路径**（自由项目，绝对/相对均可）时，路径参数相对所选项目根解析（沙箱模式限定该根内），`sh`/`py`/`git`/`preview_server` 同时以该根为工作目录；保留名 `tmp` = 会话工作区。未传时相对路径以会话工作目录为基准（上文路径基准语义）。实现：`core/tools/projects.ts` 的 `projectAware` 在全局注册处统一包装（见「项目机制」与 code 章节）。（2026 修订：解析次序 显式 root 参数 > 会话 env `SHENGSHENG_ROOT` > 本会话 init 记忆 > 工作目录向上探测；候选一律核到既有 `.evolve/control.md` 才采纳、核不中即回落下一级，陈旧或已删的根不得直接判「初生」；init 记忆以**会话工作目录**为键、仅 init 时写入，故不跨会话泄漏。）

### `edit` 修改工具

在 `write`（整体写入）之外提供**精确修改**能力，适合代码/配置文件的小步修改：

- **参数**：`path`（目标文件）+ `edits: { old_string | pattern, new_string, regex_flags?, replace_all? }[]`（可一次多处替换；`replace_all` 缺省 false）；每项 `old_string` 与 `pattern` **二选一**，`new_string` 必给（删除内容传空串）
- **正则匹配（`pattern`）**：`old_string` 的替代通道——大段原文只改少量字符时用正则定位，省去整段重发；`regex_flags` 支持 `g/i/m/s/u/y`（`g` 自动补齐），`new_string` 支持 `$&` 整段匹配、`$1..$9` 捕获组（未参与匹配为空串）、`$$` 字面 `$`；匹配在 **LF 归一空间**进行（CRLF 文件同样可用）；**匹配在独立子进程执行**（灾难性回溯防护，20 秒超时强杀，与 `grep` 同机制）；单次匹配上限 1000 处（超限拒绝——无法安全判定替换范围）；零长度匹配（`^`/`a*` 类）明确拒绝
- **唯一性校验（防误改）**：默认要求匹配**唯一命中**——多处命中时整体失败不落盘，错误信息列出各命中行号（至多 8 处）并给出两条出路：扩大匹配范围使其唯一，或确认全部替换时该项传 `replace_all: true`；避免「相同片段出现在多处时改错第一处」的静默误改
- **编辑项按序应用（重叠自动合并）**：多编辑项**逐项在「前面各项应用后的文本」上匹配**——项与项之间不存在「区间重叠」这种状态，只有「匹配不上」与「命中不唯一」两类真错误。这正对两类高频写法：①**项内嵌套**（先改一整段、再补改段内一处，后项 old_string 落在前项 new_string 里）；②**项间同锚点**（两项各自定位同一区域的不同片段，原文区间交错，如先改 `function f() {\n  clear()` 再改 `function f() {`）。实现为**虚拟文档段模型**（`EditSeg`）：段列表让「某编辑项替换生成的内容」与「尚未触碰的源文本」共存，替换区间按当前文本坐标切段替换（区间整个落在既有替换段内时**就地改写该段文本而保留其覆盖的源区间**——后项嵌在前项结果里仍在改同一处原文，语义正确）；每项记源侧锚点，行号回报据此落回原文件可读位置。命中落在前面项的替换结果上时输出注记「与第 N 项的替换结果相接，已按序合并应用」。**代价与边界**：若后项原文确实被前面项改写掉了（不再存在），仍报错，错误信息升级为「本项在**前面 N 项应用后**的内容里未匹配——若原文落在前面项改写的那段里，请把该项的编辑合并进前面项的 new_string，或把本项排到前面」，一步引导自愈（比旧的「区间重叠」直指原因）
- **安全校验**：替换前校验匹配成立，不匹配则整体失败并报错，**不落盘**，避免模型基于过期内容误改；参数前置校验：空 `edits`、项非对象、`old_string` 与 `pattern` 同给或同缺、缺 `new_string`、`replace_all` 非布尔、`old_string` 等于 `new_string`（无改动）均作为工具结果返回拒绝（不抛错）
- **编码感知（字节级）**：目标文件按**字节探测编码**（BOM / UTF-16 LE·BE / UTF-8 / GBK）后解码匹配、按**原编码写回**（BOM 与字节序保持）——非 UTF-8 文件不再被按 UTF-8 误读误写（`core/base/file-text.ts`）；**GBK 无编码表**（`TextEncoder` 只支持 UTF 系）：仅支持「命中区域与替换文本均纯 ASCII」的字节级替换，非 ASCII 场景明确拒绝并引导先转码为 UTF-8（含 GB18030 四字节字符的文件直接拒绝）；不可识别为文本（二进制/未知编码）明确拒绝；`write` 对非 UTF-8 目标文件同样拒绝整体写入（引导转码或改用 `edit`）
- **行尾健壮（区间映射）**：匹配在 **LF 归一空间**进行（CRLF 与裸 CR 都归一，模型给的 LF/CRLF 片段双向自适应），写回按**源字符区间拼接**——未修改区域取自原始字节（混合行尾文件不被整文件改写），仅替换文本按文件**主导行尾**落地；与旧版「整体还原行尾」相比，混合行尾与裸 CR 文件不再被静默改写
- **防盲改守卫（已读前置 + 陈旧拦截）**：目标文件已存在但本会话未 read 过时拒绝（与 `write` 防误覆盖同规则，见「防盲写守卫」）——防模型凭记忆/假设内容构造 `old_string` 盲改（匹配失败白跑一轮往返）；内容自上次读取/写入后被修改过同样拒绝（防陈旧改——内容漂移后再改会误命中错位应用）；`read`/`edit`/`write` 成功过的文件视为已读；指纹按解码文本登记（与 `read` 口径一致，非 UTF-8 文件同样可比）
- **空白容错匹配（自动对齐）与失配诊断**：精确匹配失败后追加**字符级空白容错匹配**——去掉全部空白后的字符序列转正则（字符间 \s*，每字符转义）在 LF 归一空间查找，**唯一命中即采用**（输出注记「空白容错命中」；多打/漏打空格、缩进不一致、行尾差异、CJK 文本内空白差异均双向容错；多处命中不盲替换仍报错）；仍失配时报**首个分歧点**（长行中非空白字符差异的定位上下文，如「分歧起点约在『…也消费 ◀ vision 边车…』」）；行号前缀误拷贝（read 默认行号输出）**自动剥离**后重新匹配（`old_string` 携带 read 默认行号输出的「行号→制表符」前缀）：**自动剥离**后重新匹配，能命中即直接按修正后的原文替换（少一次往返），仍不命中才报错——read 默认带行号后的配套防呆
- **应用行号回报**：成功后输出每处修改的应用行号（如 `已对 a.ts 应用 2 处修改：1) 行 12；2) 行 40`），便于汇报与后续引用；行号由**源侧锚点**映射给出（前面项的替换结果内命中处取该替换覆盖的源区间起点）；内容无变化时不写盘、不产出文件内容卡并注记
- **与 `write` 的关系**：`edit` 用于既有文件的定点修改（保留无关内容）；`write` 用于新建/整体覆盖（受防误覆盖守卫约束）；模型按需选择（js 编排中可混用）
- **审批**：默认无需审批（与 `write` 同级）
- **路径限制**：与 `read`/`write` 同一路径基准与沙箱（相对路径基于会话 `tmp/`；服务端部署限其内，桌面/本地浏览器不限制）

### `grep` 内容检索工具

按正则表达式递归搜索文本内容（`core/tools/fs.ts`），是编码探索的主定位工具。**双引擎**：内置 ripgrep（随包分发、无需系统安装，见「内置 ripgrep」）优先，rg 不可用或其正则语法不支持该 pattern 时自动回退内置遍历引擎——两引擎在非截断场景下结果逐字段一致（有专门的一致性测试守约），`data.engine` 如实反映本次实际所用引擎：

- **参数**：`pattern`（正则，或 `literal:true` 字面匹配）+ 可选 `path`（搜索起点：目录（递归）或单个文件（直接内搜该文件，grep 传文件语义）；默认 `.`，显式传 `.` 与省略等价（经 `resolvePath` 归一化，与 `glob` 同一惯例），相对会话工作目录、`tmp/` 前缀可省略）/`ignore_case`/`output`/`context`/`include`/`exclude`/`head_limit`；返回路径带 `tmp/` 前缀（可直接用于 `read` 等文件工具）
- **literal 固定字符串模式**：`literal:true` 时 `pattern` 按字面字符串匹配（正则元字符自动转义）——搜索含 `.`/`(`/`[` 等字符的代码片段（如 `foo.bar(`）免转义、防「点号通配造成的意外命中」；默认 false 正则模式
- **文件过滤（`include`）与排除（`exclude`）**：按路径 glob 过滤/排除（`*.ts`、`src/**`、`tests/**,*.{json,md}`）——逗号分隔多模式 + `{a,b}` 花括号交替（花括号内的逗号是交替项不分割）；无 `/` 的模式（如 `dist`、`*.log`）按目录/文件名匹配**任意层级**路径段（同 `rg --glob` 语义），含 `/` 的模式按整条相对路径匹配；`exclude` 命中即剔除（优先于 include）
- **默认跳过大型/生成目录**：node_modules/.git/dist/build/.next/.cache/__pycache__/.venv/venv/target/.idea/.vscode/coverage/.turbo（`WALK_SKIP_DIRS` 同源清单）默认不进结果——会话 `tmp/` 列表场景的防噪兜底（项目根遍历 `walkDirFiles` 本就跳过）；include 原文显式点名该目录（如 `node_modules/**`）时不排除（确需搜依赖时的显式通道）；单文件内搜（path 精确命中）不受限
- **搜索范围**：默认会话 `tmp/` 子树（与文件面板一致）；`path` 指向 `tmp/`/项目根**外**时——本地模式（桌面/本地浏览器）与 `read` 同款自由度，按给定 path 前缀**实际遍历**目标搜索（`walkDirFiles`：跳过 node_modules/.git/dist 等大型/生成目录、深度上限 10，**并发 stat 且必须同时取出 size 与 mtime**——`FileEntry.modifiedAt` 恒为 0 会让「按修改时间排序/显示」类工具静默失效（曾出现：两份实现各自都只取了 size，列表全显 1970），故该函数在 `@gebai/sdk/node` 的 `walk.ts` 为**单一来源**、server 侧仅重导出，与 code/explore 项目遍历同一实现），结果路径带给定前缀可直接用于 `read`；沙箱部署模式仍拒绝越界（无匹配）；路径不存在**明确报错**（不再静默「无匹配文件」）
- **二进制内容跳过**：文件内容含 NUL 字节视为二进制跳过（同 grep 二进制检测语义），防乱码匹配行刷进上下文；**不读 `.gitignore`**——忽略规则仅内置目录黑名单（node_modules/dist/.git/target 等），被 git 忽略的文件（如 `*.log`）仍会命中
- **三种结果形态（`output`）**：
  - `content`（默认）：逐条匹配行**按文件分组**（同 `rg --heading`）——文件名独占一行，其下依次 `行号: 匹配行`；路径每文件只出现一次（逐行重复完整路径是纯冗余，在大仓搜索时占匹配行字符近一半）
  - `files`：仅命中文件清单（不刷内容）——**宽泛摸底定位优先用**（「哪些文件涉及 X」先看文件面，再对重点文件精读，避免大量匹配行灌进上下文）
  - `count`：每文件命中行数（按命中数降序）——快速评估命中面大小、决定深入策略
- **上下文行（`context`，0-10，仅 content 模式）**：匹配行前后各附 N 行，格式同 `grep -n -C` 的组语义——匹配行前缀 `行号: `、上下文行前缀 `行号- `、不相邻组之间 `--` 分隔；重叠区间自动合并，一次调用即可看清命中语境（免二次 `read`）；**非对称上下文**：`context_before`/`context_after`（0-10，同 `-B`/`-A`）独立指定前后行数（指定时覆盖 `context` 对应侧）——「只看定义后的实现体」场景不必为看后文付出前文噪音
- **上限**：目录递归搜索跳过超过 1MB 的文件（**被跳过的文件数在输出末尾注明**——使「跳过」与「确实无命中」可区分），`path` **显式指名单个文件**时上限放宽到 8MB（与 `read` 同口径；再超即明确报错引导分段读取，不再静默「（无匹配）」）、全局匹配默认 200 处（三种形态同一口径，达上限附「结果可能不完整」提示；`head_limit` 参数可压低先看一部分——`data.truncated` 标记截断）；输出超长走统一截断保护；**截断时保留哪一条取决于扫描顺序**（两引擎均提前收工，属早停的固有代价，详见「内置 ripgrep」）
- **灾难性回溯防护（内置引擎侧，双通道）**：分界不是「快慢」而是**是否可能存在回溯组合爆炸**——正则路径在**独立子进程**执行，literal（字面量）路径走**进程内匹配**。
  - **为何正则必须隔离**：正则 `test` 是**同步 CPU 操作且不可中断**——一旦进入，连 `setTimeout` 超时回调都要排在后面执行，期间该进程**全部会话**的 WS/HTTP 冻结（「全部会话冻结」的实质）。而真实工作量是「每文件 × 每行」各调一次（数千次），**单次调用的引擎级缓解约束不了整体**：本项目仅部署 Bun（JSC），其回溯预算实测随形态而异——`(a+)+b` ~0.78s、`(a*)*b` ~2.1s、`(a|a)+b` 在长输入达 3.5s（亦有 14ms 级形态，差两个数量级）；**即使单次，2.1s 的同步阻塞对一个多会话服务已不可接受**。累积效应更决定性：实测 **20 次调用即 >10s**（强杀），按 ~0.78s/次外推 1000 行 ≈ 13 分钟、5000 行 ≈ 66 分钟。且该预算属**引擎内部实现**（非文档化契约），把安全性建立在它上面很脆（引擎升级可能改变）。——故隔离到子进程，把「无界挂死」变成「20s 内有界失败 + 引导」。行数据按 4MB 批量经 stdin 送子进程、命中行号回父进程渲染，20 秒超时强杀（Unix 杀进程组/Windows `taskkill /T`）；超时返回「简化 pattern/缩小范围」引导而非回退进程内匹配（回退即重新暴露挂死面）。
  - **为何 literal 可进程内**：回溯的本质是**多条可能路径的指数组合**；转义后的字面量不含量词/交替，**只有一条匹配路径**，不存在组合爆炸——故可安全进程内执行，省掉「内容序列化 + 跨进程搬运 + 反序列化」（实测 108MB 量级省约 1.6s）。
  - 两条通道**读取均按块并发**（限额 16、文件顺序不变）。rg 引擎的 Rust 正则**线性时间**（有限自动机模拟，设计上不存在指数回溯；实测同 pattern 恒为 18–20ms），天然不需要该防护（仍设 20 秒上限防病态 I/O）。
- **结构化输出**：`data = { mode, matches, files, counts, truncated?, engine }`——**三键齐备**（不论 output 选哪种模式，matches（行级）/files（文件清单）/counts（每文件命中数，按命中数降序）同时给出，`mode` 标明本次主形态；无匹配时三键均为空数组），供 js 编排引用——**不要按 mode 猜键名**（曾在校对中因按 `data.matches` 读 files 模式而误判「代码中不存在」）；`engine` 为本次实际使用的引擎（`rg`/`builtin`），用于可观测与回退排查
- **审批**：默认无需审批（纯读取）

### 内置 ripgrep（`rg`）与双引擎选择

**问题**：grep 旧实现把过滤后的**每个候选文件全文读进内存**再整批送子进程正则匹配——gebai 仓库根目录搜索实测读入 2033 文件 / 107.7MB、**读全文 16 秒**（加匹配与序列化共 20 秒+）；同一查询 `rg` 仅需 79ms。慢的不是过滤（遍历 161ms）也不是匹配，而是**全量内容 I/O**。

**解析链**（`core/support/ripgrep.ts`，首个可用者胜出；结果缓存）：

1. `GEBAI_RG_PATH` —— 显式覆盖（运维/内网自备/调试；指向不存在即视为不可用，不静默改用其他来源）
2. `{GEBAI_HOME}/vendor/ripgrep/<platform>-<arch>/rg[.exe]` —— **二进制（`bun --compile`）形态**：运行时物化点，内容来自构建期内嵌产物（`rg.embedded.generated.json`，gzip base64）+ 物化（与 d2js / playwright driver / CV 模型同一套「内嵌 + 物化」闭环，POSIX 补 0755）；单二进制形态在用户机器上既无 node_modules 也不保证装了 rg，故随产物内嵌。**源码/dev 形态不走此路**（无需物化，直接用下面两条）
3. node_modules 的 `@vscode/ripgrep`（`optionalDependencies`，**拉不到不阻断 `bun install`**）—— 实现上问主包要它自己导出的 `rgPath`（不自拼路径：1.18+ 二进制在平台子包 `@vscode/ripgrep-<platform>-<arch>/bin/rg[.exe]`、≤1.15.x 在主包 `bin/` 下，跨包管理器布局由包自身保证——自拼平台包路径在 bun workspace 下解析不到）。现版此包**经 npm registry 分发**（平台子包 + optionalDependencies，无 postinstall 外网下载），内网有 npm 镜像即可；历史版本（≤1.15.x）才是 postinstall 联网下载那套
4. 系统 `PATH` 上的 `rg`
5. 全部缺失 → 内置遍历引擎（**功能不降级、只降速**，`data.engine=builtin`）

**不依赖 `resources/` 资源子仓库**：刻意**不存第二份二进制副本**——来源收敛为「npm 包」与「系统」两条，npm 包已覆盖跨平台分发（12 平台子包）与版本管理，自存副本徒增仓库体积与失同步风险。

**内嵌产物供给**（`packages/server/scripts/build-rg-embed.ts`，已并入 `build`/`typecheck` 链路）：取 rg 本体的顺序与运行时解析链一致——`GEBAI_RG_PATH` → node_modules 的 `@vscode/ripgrep` → 系统 `PATH`；**不落盘任何资源文件、不从网络下载**（只把取到的二进制 gzip 内嵌进生成产物）；取不到时只生成空清单、**不阻断构建**（运行时给配置指引：`bun install` 拉 optional 依赖，或构建机装 rg）。

**双引擎对齐（结果一致性）**——否则两引擎会静默漂移，以下几处是刻意取舍：

- `--hidden --no-ignore --no-ignore-vcs/-parent/-dot/-global`：内置遍历只跳过 `WALK_SKIP_DIRS`，既不读 `.gitignore` 也不跳过隐藏文件；rg 默认两者都做，须显式关闭才能对齐（`GEBAI_GREP_RESPECT_GITIGNORE` 为预留的可选开关）
- `--max-count <head_limit+1>`：多读一条才判定「结果不完整」，与内置匹配器同口径（**超限的那一条不进入结果**）
- `--max-filesize`：与内置的候选文件大小上限**同一取值**（目录递归搜索 1MB；`path` 显式指名单文件时与调用方同步放宽到 8MB——rg 侧不放宽就会「指名文件却无命中」）；二进制文件由 rg 与内置引擎各自按 NUL 字节跳过，行为等价
- 行切分对齐：以换行结尾的文件**不产出末尾幻影空行**（内置引擎原先按 `split("\n")` 会多出一行，导致 `-A` 上下文多渲染一行空行、`^$` 类 pattern 多一处命中——本次一并修正）

**回填与过滤分层**：rg 直接扫真实文件系统，结果按「绝对路径 → 显示路径」allowlist 精确回填——过滤语义（`ctx.listFiles()` 坐标 + include/exclude + 默认跳过目录 + 单文件内搜）**仍由过滤层决定**，rg 视野 ⊇ 过滤结果，回填只做收敛，不会漏命中。候选非空而 rg 一个文件都没扫到（`summary.stats.searches === 0`）时判定「列表与磁盘不一致」（虚拟/陈旧清单），回退内置引擎而**非静默报无匹配**。

**回退与可观测**：`GEBAI_GREP_ENGINE=auto`（默认）/`rg`（强制，不可用则**明确报错**，便于部署期发现内置缺失）/`builtin`（逃生口与性能回归排查）；运行期失败（Rust 正则不支持后向引用/环视、超时、被中断）静默回退内置引擎，`data.engine` 给出实际所用引擎。

**顺带修掉的三处（同源性能/健壮性问题）**：① `core/fs/service.ts` 的 `/api/v1/fs/search` 原用 `Bun.spawnSync`（**同步阻塞整个服务事件循环**——搜索期间全部会话一起卡），改异步 spawn 并共用同一解析器（并去掉重复的 `--max-count 50 -m 50`）；该路径**保留** rg 默认的 `.gitignore` 规则（面向仓库浏览，与 grep 工具的对齐取舍不同，属有意差异）。② `walkDirFiles` 逐文件串行 `stat` → 并发（上限 32），遍历顺序不变。③ grep 匹配子进程 runner 的落盘判定原仅凭内存缓存——runner 位于系统临时目录，被外部清理（tmp 清理任务等）后会反复启动子进程失败，**且失败现象是「超时」误导**；改为带存在性复核、缺失即重写。

**内置引擎（回退路径）自身的两项优化**（2026-09 补充：rg 之外的兜底不应停在秒级）：读取由逐文件串行 await 改为**按块并发**（限额 16、顺序不变）、literal 改**进程内匹配**。实测仓库根 2107 文件 / 108MB（热缓存，含遍历+读+匹配+渲染）：

| 路径 | 改造前 | 改造后 | rg |
| --- | --- | --- | --- |
| 内置 literal（进程内） | 4535ms | **253ms** | 146–184ms |
| 内置正则（子进程隔离） | 4535ms | **3078ms** | 146–184ms |

**为何不把内置正则也改进程内**：正是「灾难性回溯防护」要解决的问题——进程内匹配遇病态正则会长时间**同步阻塞**事件循环（JSC 的单次回溯预算只约束单次调用，不约束「文件×行」整体）且不可中断。若将来要再提速，可行方向是「子进程只传路径、自读自匹配」（实测 1293ms）——代价是放弃 `ctx.readFile` 抽象（编码探测/虚拟文件/测试桩），**不划算而未采用**。

### 防盲写守卫（已读追踪 + 内容指纹）

防止模型对未见过的文件内容盲写（参照 ZCode 的「未读不许覆盖/修改」设计），覆盖 `write`（整体覆盖/追加）与 `edit`（定点替换）两个写通道，规则一致：

- **守卫规则（两档）**：①**防盲写**——目标文件**已存在且本会话未读取过**时拒绝写入，返回引导信息（先 `read` 掌握现有内容再改——`write` 覆盖确认整体内容、`edit` 基于当前原文构造修改）；②**防陈旧覆盖**——文件**读过但内容自本会话上次读取/写入登记后已被修改**（并行子会话、父会话任务、`sh`/`py` 脚本命令、外部编辑均会造成漂移）时同样拒绝，引导重新 `read` 最新内容后再写——防基于旧认知静默覆盖他人改动；**新建文件不受限**；`append:true` 追加模式同规则（追加不破坏已有内容，但同样要求先读且不陈旧——防对未见/已变内容盲目拼接；截断抢救落盘亦受此守卫约束）
- **edit 纳入同一守卫（防盲改/防陈旧改）**：它虽天然带原文校验（幻觉内容匹配不上即失败），但「凭记忆构造 oldString → 匹配失败白跑一轮往返」本身就是高频失败模式——未读前置把失败提前到写入口（一次往返引导 read，与匹配失败等价成本但引导更明确），且杜绝「碰巧匹配成功但模型并未见过全文」的盲改；陈旧拦截保证改的总是**刚读到的**内容；`edit`/`write` 成功即刷新登记，迭代修改自身产物不受阻
- **已读登记（fileGuard）**：引擎维护会话级已读表（`AgentEngine.readFiles`，sessionId → 已读绝对路径 → 读取/写入时内容指纹），经 `ToolContext.fileGuard`（`markRead(path, content?)`/`hasRead`/`staleSinceRead(path, current)`）注入工具上下文；`read`/`edit`/`write` 成功后自动登记（含指纹）——修改过的文件视为已掌握内容，后续写入放行；指纹为**解码文本**（去 BOM，按探测编码解码——GBK/UTF-16 文件同样可比）sha256 前 16 位（BOM 无关——read 登记去 BOM 正文、write 比对含 BOM 原文，边界归一后一致；edit 写回登记按落盘字节的解码文本，行尾归一在守卫之后，不会误判陈旧）
- **子会话 fork 快照隔离**：子会话 fork 时同步拷贝会话级已读表（`runSubSession` 组装 fork 时快照）——子会话的已读基线 = fork 时父会话可见内容；此后父会话/兄弟子会话的读写互不串扰：**fork 后他人读过的文件不视为本子会话已读**（防跨执行流盲写代理），**他人写入造成的指纹漂移在本子会话写时被拦截**（防陈旧覆盖）——与 fork 上下文快照哲学一致（见「子会话运行」）。主循环与隔离子会话沿用会话级共享表（既有语义：隔离子会话虽不继承历史，但其读写在同一会话工作区，与父会话互通登记）
- **生命周期**：登记按会话隔离；REST/WS 删除会话时经 `engine.forgetSession` 释放；单会话登记上限 2000 条（`READ_TRACK_CAP`，超出整表重置——守卫降级为「需重读」，保护语义不破坏）；进程重启后登记为空（重新 read 即可，文件可能已变化，重读本身即正确行为）
- **兼容性**：`fileGuard` 为可选注入——测试桩/无引擎环境不注入时守卫自动放行（行为与旧版一致）；js 编排内的工具调用走同一 ctx，守卫同样生效
- **设计动机**：`write` 整体覆盖**没有任何内容校验**——未读守卫补齐「从未见过」盲区，指纹守卫补齐「见过但已过期」盲区（并发写的最小版本仲裁：写入时点比对，早于合入时点）；拒绝结果作为正常工具输出返回（模型下一轮自行 `read` 后重写，一次往返自愈，不中断任务）

### `diff` 内容块（仅历史会话回放）

`diff` 文本对比工具已移除——对比两段文本改用 `sh` 的 `diff` 命令（需审批）或两次 `read`；`core/base/diff.ts` 仅保留 `splitLines` 与 `inferLang`（read/show 语言推断用）两个纯函数。`diff` 内容块的类型定义与前端并排渲染管线**保留**，仅用于回放历史会话中已持久化的块（见「内容块」）。

将编码工作流收尾环节的 git 只读操作从 `sh`（每次需审批）独立出来，免审批直接查看变更。**不注册为全局工具**（全局集最小化，见功能列表注记）——经 `code`/`explore` 子Agent 以 `code_git`/`explore_git` 命名空间暴露（`self_optimize` 经连带装载 code 获得；写操作 add/commit 等在任何命名空间下仍走 `sh` 需审批）：

- **参数**：`action`（`status` 工作区状态 / `diff` 变更内容 / `log` 最近提交 / `show` 查看某提交或文件的完整内容（`ref` 默认 HEAD）/ `branch` 本地与远程分支列表（附各子会话最新提交）/ `ls-files` 已跟踪文件清单（**自动尊重 `.gitignore`**，项目结构摸底快于 glob；`path` 参数可按前缀/glob 过滤）/ `grep` 已跟踪文件内容搜索）+ 可选 `dir`（仓库目录，默认会话工作目录，经路径沙箱约束）+ `staged`（diff 是否查看暂存区）+ `max_entries`（log 条数，默认 10、上限 50）+ `ref`（**diff/log/show 通用**：提交哈希/分支/tag/HEAD~n/范围（`main..dev`）等，拼为 `git diff <ref>`/`git log <ref>`/`git show <ref>`）+ `path`（**pathspec 通用**：diff/log/show 限定该路径的变更（`-- <path>`）、ls-files 前缀/glob 过滤、grep 搜索范围限定）——`ref`+`path` 可组合（如 `diff ref=HEAD~2 path=src/` 只看某次提交中某目录的变更、`log ref=main..dev path=packages/` 看分支间某包的提交历史、`show ref=abc123 path=src/a.ts` 看某提交中某文件的变更）+ `pattern`（grep 搜索模式，扩展正则 ERE——`|` 为交替）
- **执行**：命令参数由 action 枚举构造（status/branch 无用户字符串拼接）；携带用户字符串的参数（`ref`/`path`/`pattern`）——`ref`/`path` 经元字符黑名单校验（拒绝 `"`/`&`/`|`/`<`/`>`/`^`/`%`/反引号等 cmd/shell 注入形态）后引号包裹拼入，pathspec 以 `--` 分隔防被解读为选项；`grep` 的 `pattern` 用**放宽黑名单**（仅拒 `"`/`%`/`$`/反引号——引号内活动元字符；`&`/`|`/`<`/`>` 在双引号内为字面量**放行**，正则交替 `a|b` 语法需要）并以 `-E`（扩展正则，`|` 交替生效）+ `-e` 定界模式；以 `dir` 为 cwd 执行对应 git 命令；非零退出码附 stderr（非仓库目录等可诊断信息）；输出走统一截断保护，`ls-files` 附结构化 `data.files`
- **只读安全边界**：不提供任何写操作（`add`/`commit`/`checkout` 等仍走 `sh`，需审批且受沙箱约束）——免审批仅限读操作，写操作权限不变
- **审批**：默认无需审批；经 `code`/`explore` 子Agent 的 `projectAware` 包装暴露（`code_git`/`explore_git`，以项目根为工作目录，预置项目形态按项目根执行；`self_optimize` 经连带装载 code 获得）

### `js` 脚本工具（工具动态编程）

**数据流编排的唯一内建方式**（原 `flow` 声明式管道已移除——上下文占用大、使用门槛高，js 完整语言能力可覆盖其全部场景且更直观）：可预判的固定流程与高阶编排逻辑（动态构造参数、按中间结果分支重试、复杂聚合变换、正则/字符串处理、错误分类处理）统一用 `js` 一次编程执行——脚本运行于 Bun 子进程（隔离、可超时终止），进程内注入**工具调用桥**与**会话上下文**，把工具当作函数做动态编程。实现为纯模块 `core/js-tool.ts`（前导生成 + 子进程桥接，可独立单测）；**分发层守卫抽到 `core/exec/tool-bridge.ts` 与 py 桥共用**（两条桥一份实现，漏一条即多一条绕过通道），py 侧见「py 工具桥（仅本地模式）」。

#### 为什么编排层是 `js`（而非 `py`）

**分工结论：编排首选 `js`，计算归 `py`**——这条边界由两边能力边界决定，非口味偏好（`py` 与客卿承载重计算：pandas/numpy/BM25/哈希/图像；`js` 承载编排），也是**默认编排入口落在 `js`** 的依据。`py` 侧的编排能力**仅在本地模式**以「py 工具桥」补齐（见下节）——结论不是「py 不能编排」，而是下表三项成本在沙箱/多用户部署不划算；本地单用户部署下它们可接受，故桥只在该形态启用：

| 编排层的硬要求 | `js` | `py` |
|----------------|------|------|
| **零装配可用** | Bun 运行时已 `--compile` 内嵌进二进制，`gebai exec` 复用自身（边际成本 0，交付即用） | 解释器不可内嵌——需宿主机安装，版本/环境不可控（DESIGN「脚本执行环境」标明宿主机要求为「安装 Python」） |
| **协议通道可独占** | `console.*` 是单一汇聚点，补丁即接管几乎全部输出，stdout 保持纯协议 | 输出路径分散（`print`/`logging`/`warnings`/C 扩展直写 fd/`os.write(1,…)`/子进程继承 fd），**stdout 不可独占**——故 py 桥**不占 stdio，另开通道**：回环 socket + 一次性 token 认证（Windows 无 fd 继承，stdio 之外只有命名管道/loopback socket 两条路，已按后者落地，见下节） |
| **可静态审查（安全模式的只读承诺）** | `scanJsReadOnly` 词元级拒绝 + 子进程 shim（写/进程/网络 API 屏蔽、`Function.prototype.constructor` 中性化）可**做实**「降级为只读运行时」 | 等价承诺不可做：`__import__`/`getattr`/`eval`/`exec`/`ctypes`/`pickle` 遍地逃逸面，字符串可拼出任意调用；进程内 `sys.addaudithook` 也拦不住 `os.system` 起的新进程（新进程无钩子）——**故安全模式不注入 py 桥**（只读运行时形态保持原样） |
| **固化闭环** | 函数一等公民，`fn.toString()` 直接序列化、跨进程重新求值语义干净（defineTool 与编排在同一语言里闭合） | `inspect.getsource` 在闭包/装饰器/模块依赖边界上脆弱，「源码序列化→新进程求值」不干净 |

编排的三个刚需 **`js` 原生即得**（**工具即函数**——模块作用域函数声明 + `defineTool` 注册后同脚本内立即可调；**真并发**——`Promise.all` + 行级分发按 id 配对；**数据模型同构**——工具 schema/工具实现/编排同为 JS，一个语言内闭合），`py` 侧则由「py 工具桥」在**本地模式**现搭（工具名即函数、线程池并行调用、`json.loads` 注入与 `None`/`True` 映射的心智成本）。加上内嵌运行时的毫秒级启动，契合临时脚本这一高频用法。

`py` 不可替代之处（故只作计算/长驻状态，不作编排）：重计算生态、已有 Python 资产与团队技能（写客卿驱动同样进工具面）、**长驻状态**（`vision_run` 的会话级 REPL 命名空间——`js` 是一次性脚本不留状态）。

#### py 工具桥（仅本地模式）

`py` 在本地模式（非沙箱、非安全模式、非经 js 桥调用）下**也具备「工具即函数」编排能力**，调用面与 `js` 对称——代价按上表三项在小规模部署可接受，且不与「编排首选 js」冲突：`js` 仍是默认编排入口（零装配 + `defineTool` 闭环），py 桥服务于已有 Python 资产/技能与「计算与编排同脚本」的本地场景。

- **通信：协议不借用 stdio**（本实现的核心取舍）。Python 输出路径分散，stdout 无法像 `console.*` 那样被单点接管，故桥**另开通道**：父进程 `net.createServer` 监听 `127.0.0.1:0` 随机端口 + 一次性 token（`randomUUID`），**只接受首个连接**（首行 `{"t":"auth",…}` 校验，失败即断开）且连接建立后**立即关闭监听**；`print`/fd 直写/C 扩展/子进程继承的所有输出一律原样进 stdout（父进程按纯文本捕获，与旧 `runCommand` 路径语义一致），stdin 亦不被占用（`input` 以变量注入）——**不存在用户输出污染协议解析的问题，也无需 fd 重定向/特殊标记**
- **调用面**：已启用工具名即函数——`r = read({"path": "a.txt"})`（Python 侧按 `keyword`/`builtins` 过滤非法标识符与内建同名）；动态名用 `tools.call(name, params)` 或 `tools.<工具名>(params)`；返回值为 dict 且支持属性访问（`r["output"]` ≡ `r.output`，含 `data`/`blocks`/`truncated`；**属性访问仅作用于顶层**——嵌套字段是普通 dict，按下标访问，如 `r["data"]["exitCode"]`）；工具失败抛 `_G_ToolError`（异常类**已注入用户命名空间**，可 `except _G_ToolError` 按名捕获——未注入则 NameError，与工具描述承诺不符；`except Exception` 同样可行）。注入 `ctx`（`env` 运行时取 `os.environ`——脚本文件不落盘密钥，与 js 同规则）与 `input`（对象/数组原样注入为 `dict`/`list`；值为 JSON 对象/数组字面量文本时自动 `json.loads` 解包——模型侧常按「无 type 参数即字符串」惯例传 JSON 文本，兜底后两种传法等价；普通字符串原样保留；遮蔽内建 `input()`）；脚本设顶层变量 `result = …` 即作为结构化返回值（进 `data.result`），`data` 为 `{stdout, stderr, exitCode, result?, calls?}`
- **真并发**：应答由后台读线程按 id 派发到各调用方队列，`tools.call` 为**同步阻塞** API——单线程写法自然，`ThreadPoolExecutor` 可并行调用（与 js 的 `Promise.all` 同语义）
- **门控（fail-closed）**：仅 `!ctx.sandboxed && !ctx.safeMode && !bridgeLangs.includes("py")` 才注入桥——沙箱模式（服务端多用户部署）与安全模式走既有 `runCommand` 纯脚本路径（行为不变）；安全模式**刻意不给桥**（只读承诺不因新通道被削弱，需要编排时用 js）；py 桥重入（链中已含 py，链断在该层）同样不注桥。另一种语言首次进入时照常注桥（`js→py` 的 py 带桥，链重入由链封死）
- **降级**：socket/进程不可用（连接未在 8 秒内建立、子进程未能启动）时**不退回有污染风险的 stdio 桥**，改纯脚本执行并在输出首行说明「脚本桥不可用」
- **不支持 `defineTool`**（Python「源码序列化 → 新进程求值」不干净）；**审批恒需**（`code` 为任意代码、无法静态判定，`approval:false` 不生效；默认审批一次覆盖脚本内全部工具调用）
- **守卫与 js 共用**（`core/exec/tool-bridge.ts`）：名称容错、同类桥重入、运行时定义工具 depth 限制、安全模式硬阻断集（cron 类）、调用总数上限、必填参数校验、免审拦截、结果封顶与 `blocks`/子会话存档透传
- **嵌套面：链式重入（同类桥不可重入）**——ctx 携带**脚本桥语言链**（`bridgeLangs`，分发层逐层追加），判定用「已进入链 + 本桥语言」：
  - `py→js`、`js→py`（**另一种语言首次进入**）：**放行**——一层混合编排合法（py 里可借 JS 能力，js 里可借 Python 计算/生态）；链只增不减，任一语言第二次出现即拒，链长上限 2（最多再由 2 层脚本子进程），自然终止
  - `js→…→js` / `py→…→py`（同类重入，含 `js→py→js`、`py→js→py` 与动态工具中转）：**拒绝**——分发层硬拒（脚本侧表现为工具级异常，`try/except` 可捕获）；js 的 execute 另有一道链检查作纵深防御（拦住「工具内部直调桥」的旁路；此处为**返回文本的软拒绝**：主循环下模型读文本自愈，脚本内则由分发层先拦）
  - **为何重入必须硬拒绝**：脚本桥内是**程序化调用**——软拒绝会被脚本当正常返回值继续处理（`try/except` 抓不到），脚本无从判定
- **实现期踩坑（均已回归覆盖）**：① JSON 字面量含 `true/false/null` 不是合法 Python 字面量——注入数据一律经 `json.loads` 解析（`_G_CTX`/`_G_INPUT`/`_G_TOOL_NAMES`）；② 子进程退出（`child close`）与 socket `data` 事件派发顺序不保证——收尾须**排空**（等 in-flight 工具调用结束 + 数据安静 60ms，上限 500ms），否则 `done` 消息丢失（表现为「脚本返回值丢失」）；③ Windows 上 `close()` 时若有 pending `recv` 会触发 RST、对端丢弃尚未派发的 `done`——Python 侧先 `shutdown(SHUT_RDWR)` 再 `close`；④ Python 管道下块缓冲会打乱输出顺序——以 `-u` + `PYTHONUTF8=1` 启动

#### 执行模型

- 生成脚本文件（会话 `tmp/.gebai_js_{uuid}.ts`）= **运行时桥前导**（工具调用/日志重定向/引导执行）+ **工具内置函数声明**（模块作用域，按当前已启用工具名生成）+ 用户代码（包进 async 函数：顶层 await 可用、`return` 返回值），子进程执行后即删
- **子进程命令**：脚本调试模式 `bun <script>`；二进制模式 `[execPath, 入口, "exec", script]`（隐藏子命令复用内嵌 Bun 运行时，exec 段双位置路由，见「脚本执行环境」）；无 shell 参与（参数数组直传，无注入面）
- 子进程环境/工作目录与 `sh` 同规则：cwd=会话 `tmp/`、任务 env 注入；沙箱模式剔除敏感变量（豁免/本地不剔除）；超时/取消按进程树终止（Unix 进程组 / Windows taskkill，`timeout` 默认 300s 上限 540s——`py`/`js` 的单次执行超时，与 `sh` 的同步等待窗口语义不同；超时/中断退出码 124）
- **stdio JSON 行协议桥**：脚本调用工具 → stdout 写 `{"t":"call",id,name,params}`；运行时工具定义（defineTool）→ `{"t":"def",id,name,description,parameters,source,overwrite?}`；运行时工具注销（undefineTool）→ `{"t":"undef",id,name}`；服务端分发执行 → stdin 回 `{"t":"res",id,ok,result|error}`（行级并发：脚本可 `Promise.all` 并行调用，应答按 id 配对）；`console.*` 经补丁转 `{"t":"log",level,text}` 回传（stdout 保持纯协议通道）；结束以 `{"t":"done",value}`（返回值）或 `{"t":"fail",error}`（未捕获异常）收口，非 JSON 行按日志透传兜底

#### 注入脚本的调用面（内置函数风格）

- **工具函数（内置函数风格）**：执行时按当前已启用工具名生成**模块顶层函数声明**——`const r = await read({ path: "a.txt" })`，每个工具名都是一个可直接 await 的函数（含子Agent 命名空间工具）；动态名字用 `await tools.call(name, params)` 或 `await tools.xxx(params)`。返回 `{ output, data, blocks, truncated, filePath }`（`data` 即工具双输出的结构化通道，结构可先用 `tool_schemas` 查询）；工具级异常 = Promise reject（脚本可 try/catch 容错继续）；单字段 RPC 截断 100k 字符。**遮蔽无冲突**：函数声明在模块作用域，用户代码在 `__G_main` 内层——同名 let/const/function 正常遮蔽；非法标识符/保留字/与注入全局（tools/ctx/input/console）冲突的工具名跳过生成（仍可 `tools.call` 动态调用）
- **`ctx`（会话上下文）**：`{ user, sessionId, workdir, home, sandboxed, env, projects, messages }`——`env` 与子进程环境同规则脱敏；`messages` 为最近会话消息快照（默认最近 50 条、单条 2000 字符，文本抽取、跳过 system/空消息）；另有 `input`（编排传入的任意输入，JSON 文本自行 `JSON.parse`）
- **`defineTool(def)`（运行时工具定义）**：与子Agent 工具同写法定义新工具（见「运行时工具定义（defineTool）」），注册成功即注入脚本全局、后续可像内置函数一样调用；**`undefineTool(name)`** 注销已注册的动态工具（注册错版本/用完即弃的一次性工具）

#### 运行时工具定义（defineTool）

脚本可以像在子Agent 文件里写工具一样，把一段能力**固化为会话级新工具**：`await defineTool({ name, description, parameters, execute })`——`execute` 与子Agent 工具同签名 `async (args, ctx) => ({ output, data? })`（方法简写 `async execute(args) {...}` / 箭头 / 具名函数均可，`fn.toString()` 序列化保存）。

- **注册**：定义经 RPC `{t:"def"}` 桥到服务端，引擎校验（命名 `[a-z][a-z0-9_]{0,39}`、重名/子Agent 命名空间碰撞、描述与 execute 源码合法、安全模式拒绝）后并入**会话覆盖层**；注册成功即注入脚本全局——**同脚本内可立即像内置函数一样调用**；后续轮次模型直接按名调用（schema 随下一轮下发，与内置工具一致可被其他 js 脚本调用）。**`overwrite: true`**：覆盖本会话已注册的**同名动态工具**（原地替换、不占新名额，返回 `{registered, overwritten}`）——全局工具与子Agent 命名空间占用名**仍拒绝**（rename 不出的名字只能换名）
- **执行模型**：后续每次调用在子进程求值 execute 源码（复用 js 运行时桥：体内可调任意工具/ctx/内置函数，`runtimeDefined` 标记 + RPC 分发层 depth 守卫——**动态工具内不能再调用动态工具/js**（防递归嵌套子进程））；返回 `{output, data?}`（子Agent 语义，字符串自动映射为 output），失败抛工具级错误（编排「失败即中断」语义）
- **作用域与生命周期**：会话级，定义清单随会话 `chat.json` 落盘（`SessionData.dynamicTools`，execute 源码序列化保存）、`run()` 水合恢复（重启不丢）、`forgetSession` 随会话删除释放（会话删除即文件删除）；子会话运行（subsession_run）内定义的工具进本次运行注册表（运行结束释放，不落盘、不外泄主会话）；单会话上限 50（水合同限）；**`undefineTool(name)`** 注销（从覆盖层与 `chat.json` 同时摘除，重启不再水合；名字不在覆盖层即报错——绝不回落删除全局工具/子Agent 工具）；子会话运行未注入注销通道时 `undefineTool` 返回环境不支持（运行结束自然释放）
- **审批**：`requiresApproval` 可选（**默认 true 需审批**——固化后的每次调用与 sh 同姿态，仅明确安全的只读/幂等工具定义时显式传 false；定义时脚本本身已经过一次审批，代码展示在审批卡）；沙箱模式经 `isRiskyToolName` 拦截（js 整体受限，无定义入口）
- **校验反馈**：RPC 分发层对工具调用做必填参数校验（缺参即时拒绝并列出参数名，近似 TS 类型检查的即时反馈）；execute 源码须自包含（不闭包脚本局部变量——每次调用在全新子进程求值，体内可用 `read(...)` 等工具函数/`ctx`/`input`）

```
// 示例：把多轮要复用的加工流程固化为工具（后续直接 hello_tool({who})，不必重贴脚本）
await defineTool({
  name: "hello_tool",
  description: "向指定对象问好",
  parameters: { type: "object", properties: { who: { type: "string" } }, required: ["who"] },
  async execute(args, ctx) {
    const r = await sh({ command: "date" })
    return { output: `hello, ${args.who}（${r.data.stdout.trim()}）` }
  },
})
```

#### 守卫与限制

- **审批**：与 `sh` 同姿态——默认需审批（脚本=任意代码执行），`approval:false` 按次免审；**默认审批一次覆盖整个脚本含内部工具调用**（一次审批、依次执行）；**免审运行（`approval:false`/免审动态工具）时内部调用需审批的工具在 RPC 分发层被拒**——免审脚本体未经用户审阅，按剥离免审标记后的审批姿态解析（`stripApprovalFlags`，防脚本内再传 `approval:false` 自我免审，与引擎无交互硬门槛同规则；`requiresApproval` 函数异常按需审批处理）
- **安全模式**：`js` 本身**降级为只读运行时**而非禁用——静态扫描（`scanJsReadOnly`：动态 `import()`/`require()`/`eval()`/`Function()`/`import.meta.require`/`process.getBuiltinModule`/`process.binding`/`Bun.fetch`/`Bun.sqlite` 前置拒绝）+ 子进程 shim（Bun 写/进程/网络 API 屏蔽、`Bun.file` 拦写留读、eval/Function/fetch/Worker 全局删除、`Function.prototype.constructor` 中性化）；内部工具调用在 RPC 分发层按硬阻断集（`isToolBlockedInSafeMode`，cron 调度类）拦截（无绕过通道），`sh`/`py`/`write` 等照常可调、由各工具降级规则执行；`defineTool` 与磁盘持久化动态工具同规则降级（execute 源码扫描 + 只读 shim），注册与水合均允许——只读动态工具安全模式下可用
- **内部调用走同一 `ToolContext`**：`writeGuard`（子Agent 写范围）/`fileGuard`（防盲覆盖）等会话级守卫对脚本内工具调用同样生效
- **防嵌套（链式重入）**：脚本桥不可重入——ctx 携带**语言链**（`bridgeLangs`，分发层逐层追加，判定用「已进入链 + 本桥语言」），同类桥再次进入即拒（`js→…→js`、`py→…→py`，含经直执行工具/动态工具的中转）；**另一种语言首次进入放行**（`js→py` / `py→js` 一层混合编排，链长上限 2 自然终止）。**动态工具内不能调用动态工具/js**（`runtimeDefined` + depth 守卫），**动态工具运行器（depth 1）内不能再 `defineTool`**（防递归注册）；`js` 的 execute 另有链检查作纵深防御（拦「工具内部直调桥」旁路，为返回文本的软拒绝）；`sh`/`py` 不受限（脚本本就可 `Bun.spawn`）
- **规模上限**：单次脚本工具调用总数 ≤ 100；RPC 协议行子进程侧 ~2MB / 服务端 2.5MB 截断兜底（防巨对象撑爆内存）；非 JSON 输出行 ≤100k 按日志透传、超长丢弃留注（多为 2MB 截断产生的非法 JSON，防垃圾文本刷屏）
- **密钥不落盘**：`ctx.env` 不嵌入临时脚本文件（历来明文写入合并后的全部环境变量，崩溃残留即密钥泄漏）——子进程内改为运行时引用 `process.env`（spawn 已传同源环境、沙箱脱敏同规则，语义完全一致）；messages 等会话数据仍嵌入（ctx 注入契约，属任务数据）
- **内置函数声明过滤 JS 全局名**：子Agent 工具名为 JS 全局（`fetch`/`JSON`/`Promise`/`Bun` 等）时不生成模块作用域函数声明（会遮蔽全局，`JSON.parse` 莫名变工具调用），仍可经 `tools.call` 动态调用
- **失败语义**：未捕获异常/非 0 退出/超时/中断 → 失败结果（`[脚本失败]` + 错误/退出码，不中断任务）；`strict:true` 时失败抛工具级错误（编排「失败即中断」，与 sh/py 一致）；成功无输出明确提示「（脚本执行成功，无输出）」

#### 结果（双输出）

- 模型可见 `output`：console 输出汇总（warn/error 加前缀）+ `[返回值]` 预览（2000 字符，完整值在 data；返回值须为纯 JSON 结构——Map/Set/Date 经 JSON 序列化会变形或丢失）
- 结构化 `data`：`{ exitCode, logs, result, calls: [{name, ok, error?}], timedOut?, interrupted? }`（logs/result 截断至 100k 字符）
- **blocks 透传**：内部工具产生的图片/图表/文件块去重（`type:path`）限量（10 个）透传到 js 结果——编排 read 图片/产物类工具时图像块直达 UI/模型，不再丢失；file 块另带**来源工具名 `via`**（`collectBridgeBlocks` 写入）——脚本桥外层没有 `card.file` 声明，前端「文件展示方式」据此判定产物卡形态，桥内调用文件工具与直接调用表现一致
- **`subsession_run` 存档透传**：脚本内调用 `subsession_run` 时子会话完整存档（`subSessionArchive`）透传到 js 工具调用记录（历史回放不丢；脚本侧返回值同样携带）；多个调用取最后一个

```
// 动态编程示例：读文件 → 按内容分支重试 → 聚合返回（工具像内置函数一样直接调用）
// js({ code }) — 已启用工具名即顶层函数：read/sh/glob/...，直接 await 调用
const r = await read({ path: "config.json" })
let cfg
for (let i = 0; ; i++) {
  try { cfg = JSON.parse(r.output); break } catch (e) {
    if (i >= 2) throw new Error("配置解析失败: " + e.message)
    await sh({ command: "echo retry", approval: false })
  }
}
const items = await Promise.all(cfg.files.map(f => tools.call("read", { path: f })))
return { total: items.length, sizes: items.map(x => x.output.length) }
```

### 通信协议

**前后端统一在一个端口暴露**：静态 Web UI、WebSocket、REST API 均由同一服务（Hono）在同一监听端口（`GEBAI_PORT`）上提供，按路径路由分发：

| 路径 | 通道 |
|------|------|
| `/`、`/assets/*`、`/vendor/*`、`/fonts/*` | 内置 Web UI 静态资源（浏览器/WebView 直接访问；`/vendor` 为图表引擎等静态伺服的第三方产物、`/fonts` 为内置字体） |
| `/ws` | WebSocket（实时通道） |
| `/api/*` | REST HTTP（同步通道） |
| `/api/docs` | OpenAPI 文档 |

服务端对外提供双通道接口，方便业务系统集成：

- **WebSocket（`/ws`，实时通道）**：双向。客户端发起请求-应答 RPC（会话管理、审批决策等）；同时服务端**主动推送引擎事件**（`event.*` 前缀消息，如 `event.tool.call` / `event.approval.request` / `event.tool.result` / `event.message.delta` / `event.task.done`），每个连接订阅事件总线并按「会话归属」过滤——仅推送该连接用户可访问会话的事件，多用户隔离。SDK 侧 `onEvent(cb)` 接收推送事件。
- **REST HTTP（`/api/*`，同步通道）**：会话管理/文件/审批决策等；`POST /api/v1/sessions/:id/prompt` 为**非流式 JSON**（同步等待任务完成，返回 `{ message: 最终 assistant 消息, error?: 任务错误 }`），供业务系统与无 WS 环境集成；**流式输出统一走 WebSocket**（`/ws`）

消息发送与流式输出**统一走 WebSocket 单通道**：`sendPrompt`（SDK）经 WS `session.prompt` 发起任务，引擎事件（`event.message.delta`/`tool_call`/`tool_result`/`approval`/`task.done` 等）由连接级订阅推送，SDK 将事件流转换为 `ChatChunk` 迭代返回（`wsEventToChunk`，字段与原 SSE 契约一致）。

#### 反向代理支持

前后端同端口暴露，**整体可被反向代理**（Nginx/Caddy/网关等）统一代理到同一域名/路径下：

- **单一 upstream**：只需代理一个后端地址（`{host}:{port}`），无需为前端/API/WebSocket 配置多个 upstream 或端口
- **路径前缀挂载（免配置）**：前端一切请求路径（静态资源、`/api/*`、`/ws`）均以**当前页面 URL 为基准**自动解析——页面在 `/gebai/`、`/gebai` 或 `/gebai/files` 下时请求即自动带 `/gebai` 前缀（`@gebai/sdk` 的 `appBase`/`appPath`/`appWsUrl`，构建产物亦为相对引用），代理只需**剥离前缀**转发到服务端根路径，无需任何环境变量或构建参数；访问形式 `/gebai`（无尾斜杠）也能正确推出基准；**文件工作台页内 URL 走更直接的相对写法**（`files/url-base.ts` 的 `wbUrl`/`wbAbsUrl`：相对当前文档解析、不经过基准推导，见「文件工作台」章节）
- **WebSocket 代理**：代理需开启 Upgrade/Connection 透传（服务端 WS 路径为根路径 `/ws`，浏览器侧由页面基准带前缀），服务端依据标准 WebSocket 握手，可与 HTTP 同一 location 规则转发
- **代理头透传**：**仅消费 `X-Forwarded-For`**（来源 IP 判定，用于登录/注册限流分桶；`GEBAI_TRUST_PROXY` 控制是否信任）与 `X-Forwarded-Host`（仅在 `GEBAI_TRUST_PROXY=true` 时参与同源判定，见下条）；`X-Forwarded-Proto` **无消费点**，对外回调地址由 `GEBAI_PUBLIC_URL` 决定
- **同源校验与代理改写 Host**：本地/桌面免登录形态下带 `Origin` 的请求要求 `Origin.host` 与请求 `Host` 同源（模块脚本与 WS 握手必带 Origin）——代理须**保留原始 Host**（nginx：`proxy_set_header Host $http_host;`，HTTP 与 WS 升级两处都要），否则请求被 403（`cross-origin ws rejected`）；网关无法保留 Host 时设 `GEBAI_TRUST_PROXY=true` 并转发 `X-Forwarded-Host`（判定改按该转发主机认主；跨源伪造转发头需自定义请求头，浏览器先发预检、预检自带 Origin 且不带该头，仍被拦下）
- **HTTPS 终结**：代理侧终结 TLS 后转发明文即可，WebSocket 使用 `wss://`，无需服务端额外证书配置
- **示例（Nginx）**：
  ```
  location = /gebai { return 301 /gebai/; }   # 无尾斜杠访问先补尾斜杠（前端基准由此推出，与路由惯例一致）
  location /gebai/ {
      proxy_pass http://127.0.0.1:3000/;      # 尾斜杠 = 剥离 /gebai 前缀，服务端按根路径受理
      proxy_http_version 1.1;
      proxy_set_header Host $http_host;       # 保留原始 Host（同源校验按它比对；改写成 $proxy_host 会全量 403）
      proxy_set_header Upgrade $http_upgrade;        # WS 升级（同一 location 规则即覆盖 /gebai/ws）
      proxy_set_header Connection $connection_upgrade;
      proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
      proxy_set_header X-Forwarded-Proto $scheme;
  }
  ```

#### WebSocket（实时通道）

面向实时双向交互（内置 Web UI、桌面端、SDK），单连接承载会话操作与流式推送，连接地址为 `ws(s)://{host}:{port}/ws`。

**请求-应答机制（RPC）**：客户端每个请求带自增 `id`，服务端应答回带同一 `id` + `ok`/`error`；SDK 按 `id` 匹配，把应答自动分派到发送时注册的成功/失败回调（`send()` 的 `onOk`/`onError`，应答错误/超时/断线均走失败回调），也提供 Promise 版 `request()`。适用于**一次性操作**（会话管理、审批决策、心跳 ping 等，应答即终态）。**对持续增长的内容不适用**——流式输出（`event.message.delta`/`reasoning` 等）由服务端以无 `id` 的 `event.*` 推送承载（带 seq 可重放、断线补偿），`session.prompt` 的应答仅确认任务接收，内容全部经事件通道回流；**应答语义如实**：启动期失败（会话不存在/任务已在运行/限流）在应答中返回 `ok:false` + 协议错误码（`code: session_not_found | already_running | rate_limited | prompt_required`），客户端按错误码判定（补发幂等识别 `already_running`，不再依赖错误文案正则——跨包隐式契约），任务运行期错误仍经 `event.task.error` 推送。

**心跳保活**：客户端连接建立后周期性发送 `ping`（默认 5s 间隔，构造参数可调）——**心跳复用请求-应答机制**（`send()` 注册回调，`timeoutMs` 取心跳超时），服务端应答 `pong`（payload `{pong:true}`），防止 Nginx 等代理按闲置时间静默断连；pong 应答超时（默认 10s）或应答错误判定连接已死（代理掐断后的半开 TCP），客户端主动断开并触发指数退避自动重连。

**消息按序处理（每连接串行）**：服务端对同一连接的请求**按到达顺序串行处理**（前一条处理完成后再处理下一条）。Bun 对 async `websocket.message` 处理器不保证串行（前一条 await 磁盘/网络 I/O 时下一条即并发进入），因此服务端在 `/ws` 入口以每连接 promise 链强制串行——保证 `auth.login` 的 `conn.set(user)` 先于其后任何请求生效（否则紧跟在认证后的请求会以未登录态并发抢先执行，被拒 `unauthorized: login required`），也保证 `session.switch` 等有状态操作对后续请求可见（SDK 依赖该契约：建连后先发认证、再冲刷离线队列）。

**状态一致性模型（MVC 桌面模式）**：服务端为**权威状态**（Model），连接层为控制器（Controller），SDK/UI 为视图镜像（View）。核心机制：

- **每用户事件日志（seq + 有界环形缓冲，容量 1000）**：服务端级单点订阅事件总线，按会话归属路由到所属用户的日志，分配全局递增 `seq`；在线推送即日志条目的实时投递。**断线期间事件照常入日志**（与连接数无关），重连后客户端按 `seq` 增量重放（`sync.request` → `sync.replay`），离线事件不丢失；缓冲溢出（缺口）返回 `overrun`，客户端走全量重同步（`session.get` 等）收敛。**日志尾部持久化**（`users/{user}/ws-journal.jsonl`，JSONL 追加、定期重写裁剪回尾部 1000 条）：服务重启后 seq 从持久化尾部连续编号——断线×重启重叠窗口内的事件仍可重放（此前重启后 seq 重编号、重叠窗口事件永久丢失且不触发 overrun）。**文本增量合并**（`event.message.delta` 按消息 50ms 窗口合并为单条日志事件，非增量事件先冲刷保持顺序）：快速流式输出不再逐 chunk 高频冲掉日志缓冲（稍长断线即 overrun 降级全量重同步）
- **每用户连接状态持久化**：`session.switch` 的当前会话写入 `{GEBAI_HOME}/conn-state.json`（防抖落盘），新连接/服务端重启后自动恢复（连接级显式覆盖优先）；会话删除时若为当前会话则清空
- **状态快照（`state.snapshot`）**：连接建立（本地模式）/登录成功（服务模式）后服务端自动推送，含当前会话/会话列表/运行中会话/日志基线 `lastSeq`/**模型上下文窗口 `maxContextTokens`**（engine `contextWindow()` 取自 provider capabilities，标题栏上下文占比显示用，0=未知）；客户端作为模型基线，也可主动请求（幂等）；**SDK 快照模型保留 `maxContextTokens` 字段**（重建快照不丢弃，标题栏占比显示的数据源）；**运行态明细 `runtime`**（会话 id → `{ startedAt, pending, toolCalls, bgTasks, subRuns }`，键集即 `running`，引擎 `runtimeOf()` 同一遍历产出 running 与明细）——待决交互明细 + 在途工具调用数 + 后台命令任务（sh async 记录，按会话过滤）+ 子会话运行概要；前端据此把「哪个会话在跑/在等谁」标进会话列表（行内角标：运行中圆点、等待输入沙漏，悬浮说明含后台任务与子会话数），不必逐个附加就能看出后台会话的处境（含非当前会话），任务开始/结束事件另行增量收敛角标（不等下一次快照）。**等待项的取值按是否本页接管分流**（`sessions.ts` 的 `resolveWaitingCount`）：快照只在建连/登录时推送一次、运行期不再更新，而待决卡的出卡→作答→揭卡都在本页实时发生——本页接管着这次运行（`runs` 有它）时角标一律数本地卡，本页没接管（别处发起）时才回落快照的 `pending`（那时它可能是别处的卡，而「哪个会话卡在等人」正是角标要回答的问题）。先信快照会让建连时正在等审批的会话在本页批完后一直显示 ⌛，反之快照为 0 时新来的卡也看不到。所有揭卡路径都要派发 `gebai:pending-change`（选择/填值卡由 `tool-cards.ts` 的 `dropCard` 统一走，直接 `.remove()` 会绕过通知）
- **重连自动重新认证**：WS 连接无法携带 Header，SDK 持有令牌/API Key 时在每次建连后自动发送 `auth.login { token }` / `auth.login { apiKey }` 恢复用户上下文（服务模式重连不掉回未登录态）
- **自动重连的两条入口**：意外断开（含心跳 pong 超时判定的死连、慢客户端被服务端断开）经 `onclose` → 指数退避（1s→30s）重连；**建连本身失败**（页面打开时服务端未就绪、网络刚断、握手被拒）同样退避重连并推送 `disconnected` 状态——建连失败后 `ws` 已置空，随之到来的 `onclose` 会因「不是当前连接」直接返回，不在此处调度就永久停在断开态，只能手动刷新。主动关闭（登录/登出/换令牌）仍不重连。
- **半死连接的主动核对（`verify()`）**：手机后台（系统冻结页面、浏览器挂起定时器）回来时，WS 对象往往仍是 `readyState === OPEN`、也不会发 `close`——后台期间的事件攒在浏览器里没消化、心跳早已过期，界面显示「已连接」而内容不更新；服务端任务也可能已在离线期间跑完而本地还在等。单看本地标志位无法识别，故提供 `verify()` 主动向服务端核对：心跳超期先探一拍（`pingOnce`，不通则断开交退避重连）、拿快照收敛 `running`/`lastSeq`、按 seq 补拉漏消费的事件。前端收敛规则见 `web/src/attach.ts` 的 `verifyConnection`（本地在等而服务端已结束 → 中止 + 丢弃旧气泡 + 重读历史；服务端在跑而本页未接管 → 附加恢复）。
- **流式任务断线恢复**：`sendPrompt` 断线时**挂起**（不抛错），等待自动重连后按 `seq` 重放离线事件无缝续流；任务未确认接收时以快照判定（运行中→恢复；已跑完→从存储合成收尾；未开始→补发一次，遇协议错误码 `already_running` 视为已接受**（仅限补发路径：原请求已被服务端受理、确认帧丢失）**；**首次发送**遇 `already_running` 如实上抛——另一处的任务在跑、本次输入未被受理，前端撤回幻影消息（已上屏但从未进存储/模型）并把内容放回输入框，同时附加该会话运行态；**已受理的任务重连后核对服务端真相**（快照 `running` 不含该会话 → 从存储合成收尾：有助手回复则回填该回复，无回复但有引擎提示（服务中断说明）则展示说明，两者皆无则合成「已结束但无产出」说明——否则要等前端 150s 空闲看门狗才收场，服务模式（无 bootId 自动刷新）尤其明显；受理后引擎登记运行态前的起跑竞态由宽限窗口（`acceptSettleMs`，默认 15s）排除，测试可调小）；`overrun` 走全量重同步（`resume` 重置 + **标记 `reloadHistory`**：前端重读消息列表（缺口期间的工具卡/子会话容器事件已丢失且不可重放）+ 重取待决交互清单重建卡片，重载期间正文只累积、完成后按累积内容重建在途气泡；种子只取**本轮**（当前用户消息之后）的已落盘内容——不过滤会把上一轮回答当本轮在途文本回填）。**重放事件经全局 `onEvent` 分发**（与在线推送同通道）：离线期间的审批/选择/工具事件全局订阅者（前端卡片渲染）也能收到——重连后审批卡/工具卡可恢复，而非只进 `sendPrompt` 的 chunk 通道（此前离线窗口内的审批请求永远不渲染、任务卡死至超时）
- **运行中会话恢复（页面刷新/切换，`session.attach`）**：前端恢复逻辑拆分在 `web/src/attach.ts`（纯副作用模块——模块尾部 `setRunningAttach(attachRunningSession)` 注册钩子，无具名导出），**入口 `main.ts` 须保留 `import "./attach"`**——丢失导入不触发 typecheck/lint 报错、钩子恒为 null 静默跳过（`attach.test.ts` 源扫描守住该导入）；断线恢复覆盖「连接断而页面在」，**页面刷新/切换进入运行中会话**是另一类恢复——页面内存态（运行态 runs、sendPrompt 流、待决交互卡片）全部丢失，事件早已推送过、重放基线（快照 lastSeq）也已越过。恢复链路：引擎在任务态上维护**附加快照**（`TaskState`：`startedAt` 任务开始时刻、`stream` 在途 assistant 回合的累积文本/推理——delta/reasoning 发布点同步累积、消息持久化点清空、含 session 标记路由到子会话容器；审批/选择/填值/画图/捕获的等待项保留**展示载荷**——工具名、prompt/options、env 名与说明、图表源码、捕获参数）→ `session.attach` 返回 `{ running, startedAt, stream, pending, tools, lastSeq }`（归属校验同其余会话操作；lastSeq 为该用户日志基线；**`tools` 为在途工具调用**——已发出尚未产出结果的调用（名称/参数/子会话归属），引擎在事件发布单点拦截维护（tool.call 记入、tool.result 删除），前端据此重建「**等待中的工具卡**」：走实时事件同一渲染入口 `onToolCall`，结果到达时按 toolCallId 配对填充，与在线路径完全同构）→ 前端 `loadMessages` 尾部触发附加：**待决交互卡片重建**（审批/选择/填值/画图/捕获走既有渲染入口，替换式幂等——不重建则任务干等到超时；审批锁输入随之恢复）→ `consumeTaskStream` 接管 SDK `attachStream`（与 `sendPrompt` 同构的 ChatChunk 迭代但不发起任务：订阅就位后请求快照，**在途文本/推理作为种子 chunk 先行**（session 标记路由到惰性重建的子会话折叠容器），按 seq 重放快照之后的事件（与断线恢复同一套机制，缺口 overrun 放弃附加由存储恢复兜底），此后实时续流到任务结束）——信号灯思考闪烁、停止按钮、单轮计时（起点用快照 startedAt）、空闲看门狗、收尾清理（审批卡/配对/自动标题）与发起页完全同构。附加期间再次断线：挂起重连后**重新附加**（`resume` 重置渲染态 + 新快照重播种，防内容重复）；**任务在离线期间结束**（重连后快照 `running=false`）时发 `resume{reloadHistory:true}` 由前端重读消息列表——重附已发出的 `resume` 会清空在途累积，不补这一拍则整段回答从界面上消失（存储里有、页面空白，只能刷新才见）；**任务仍在跑但日志缺口（overrun）**时不结束附加（否则界面停在进行到一半的旧内容上而服务端还在推进），改为 `reloadHistory` 全量重同步 + 重新播种在途文本后继续实时续流；附加竞态（任务已在附加前结束，running=false）立即结束迭代，视图由存储恢复兜底。防重：附加请求在途标记 + `runs` 已接管即跳过；**附加失败退避重试**（1s/3s/9s 共三次）——静默失败会让界面谎报空闲（无信号灯/停止按钮/计时），用户随后的发送还会被 `already_running` 拒结
- **会话消息视图（pane）与后台 DOM 持续维护**：消息列不再是一个全局滚动容器，而是**每个会话一份独立滚动容器**（`web/src/msg-panes.ts`）。`#messages` 降为**容器宿主**（本身不滚动），子容器 `.messages` 才是滚动区——同一时刻只有一个子容器参与布局，其余 `hidden`（DOM、滚动位置、窗口化块表、在途流引用全部保留）。为什么必须按会话分：滚动区、窗口化、粘底跟随、消息导航都是**容器级**几何量（视口高度/scrollTop/块坐标），共用单一容器时切走只能清空重建、后台内容无处安放。保持对外 API 不变的转发层：`msg-window.ts`（`msgWindow` 句柄）与 `jump-bottom.ts`（`scrollIfSticky`/`lockToBottom`/`restoreScroll` 等）内部一律指向**活动 pane** 的实例，调用点无需感知 pane；`msg-nav.ts` 的 scroll/Resize/Mutation 监听随 `onPaneActivate` 重绑到活动容器。
  - **生命周期**：容器为**运行中会话全程持有**（`consumeTaskStream` 入口 `ensurePane` + `setPaneRunning(true)`，收尾置 false）；已停止会话的容器**按 LRU 缓存**——最近用过的 `IDLE_PANE_CACHE`（=12）个继续留着，来回切换是纯显隐（`activatePaneFast`：零网络往返、零重建），超出上限才由 `reapIdlePanes` 从最久未用开始释放，被释放的会话再切回才走 `loadMessages` 一次性加载。回收在**切换完成之后**执行（切前回收会把正要切过去的容器一并收掉，快速路径永不命中）；运行中的容器**永不释放**（后台仍需持续渲染）。内存随「运行中会话数 + 至多 `IDLE_PANE_CACHE`（12）个非活动的已停止会话 + 1 个活动会话」增长，且每个 pane 的块 DOM 仍受窗口化约束（只挂载视口附近）。
  - **后台维护**：工具事件不再对非当前会话直接丢弃（`events.ts` 的 `paneForRender` 三态：有容器→渲染到它、当前会话无容器→默认路径、其余→不渲染且只清理配对），正文/推理渲染的守卫从「是否当前会话」改为「**是否有自己的容器**」——后台会话照常建 DOM（含工具卡、子会话入口行、模型异常记录；子会话过程区在观察面板常驻 DOM，见「子会话入口与观察面板」）。滚动跟进仍按可见性判定（后台容器没人看，不滚），全局唯一的「回到最新」按钮也只由活动 pane 的实例驱动（`createStickyScroll` 的 `isActive`，否则后台内容增长会在当前会话里点亮/熄灭按钮）。按钮点击同样按活动实例分发（`sticky-scroll.ts` 的 `registerJumpOwner`：按按钮汇总全部实例、点击时执行活动实例的跟随）——按实例闭包绑 `onclick` 会让按钮永久归**最后创建**的容器，切回先创建的会话后点击无任何反应（容器被 LRU 释放后更是无主）；实例释放经 `dispose()` 注销（`msg-panes.ts` 的 `destroyPane`/`reapIdlePanes`/`resetPanes` 调用）。
  - **尾部挂载一律按会话归属**（`stream.ts` 的 `applyStreamChunk` 以 `withPane(paneOf(sessionId))` 包住整块渲染）：子会话入口行、模型异常记录、无输出收尾说明都挂在消息流的**尾部活动区**，而该区域缺省指向**当前活动容器**——不显式指到本会话容器，后台会话跑子会话时那条 `🌿 子会话 · …` 会出现在用户正看着的**另一个会话**的输入框上方（切走后仍在上一个会话里残留）。同理，滚动跟进只在目标容器就是活动容器时执行（后台没人看，滚了也没意义）。
  - **会话建立时消息视图即就位**：消息只挂到**当前会话的活动容器**上（挂载规则见 `messages.ts` 的 `mountMsg`）。新会话是懒创建的——草稿页上没有任何容器，首条消息发送时才 `createSession`，因此会话建出后必须先把它的容器建好并激活（`sessions.ts` 的 `enterSessionView`），随后的 `appendMsg` 才有落点；少了这一步，那条消息会被挂到空目标上而**静默丢弃**（服务端已落盘、页面上看不见，刷新由历史回放才出现）。`enterSessionView` 只做显隐切换与随会话 UI 刷新（`syncSessionScopedUi`），不动容器内容——已是当前会话或有历史/在途流时调它无副作用。
  - **无缝切换**：目标会话已持有容器时走快速路径 `activatePaneFast`（`sessions.ts`）——纯显隐 + 按窗口化锚点恢复阅读位置 + 刷新导航，**不调 `loadMessages`**（零网络往返、零重建）；只有没容器时才走原加载路径（从没打开过、或已被 LRU 挤出）。
- **子会话入口与观察面板（`web/src/subsessions.ts`）**：`subsession_run` 的过程不进消息流——一次运行是「推理 + 工具卡 + 多轮回复」，展开后数屏长，插在流里会把父会话叙述顶散、并发多个 run 时几条流互相交错。消息流只留**一行入口**（`.subsession-entry`：`🌿 子会话 · 名（模型）· ⚙ 子Agent` + 状态摘要 + 「查看过程 ›」），点击打开标题栏按钮的**右侧抽屉面板**：左列本次会话全部运行（运行中在前、其余按结束时间倒序，行内状态点/名称/状态摘要/合入次数），右列选中运行的详情（任务指令 / 最终报告（markdown）/ 阶段性合入全文（markdown）/ 执行过程（推理与工具卡，实时跟随））。几条硬约束：
  - **过程区常驻 DOM**：未选中的 run，其过程区挂在面板内的隐藏 staging（不是丢弃）——`isConnected` 必须为真，否则后台 run 的工具卡与流式渲染会静默丢弃（`events.ts`/`stream.ts` 都以可连接为前提）；切换选中只是把过程区在详情区与 staging 之间搬移。
  - **入口不持有状态**：入口行是纯展示 + 跳转（发 `gebai:open-subsession` 事件），因此窗口化卸载/重建、历史回放可反复重建；同 run 状态更新向所有已挂载入口行原地写文案。
  - **历史回放同一形态**：存档（`subSessionArchive`）与旧版逐条过程消息都登记进面板记录，流里只出单行入口；未保存下来的历史运行不编造耗时（无起止时刻就不显示）。
  - **合入通知与面板联动**：合入消息按 markdown 渲染并附「查看子会话过程」；阶段性合入（内容头含「阶段性合入」）计回记录的合入列表（最终合入不计——它就是本 run 的最终报告，详情「最终报告」区已有）。
- **任务开始事件与快照清单（运行态同步的两条腿）**：任务并非总由本页发起——重启续跑/飞书桥接/定时任务/其他标签页都会在页面空闲时开始运行，而「进入会话时探测一次」会漏掉此后的开始（典型：重启后页面先自动刷新、续跑才启动），页面便一直没有信号灯/停止按钮/单轮计时。两条腿分别覆盖两种时序：引擎在 `run` 的用户消息落盘后发布 `event.task.start`（含 `startedAt`）——**刷新早于任务开始**时页面经全局 `onEvent` 实时收到并附加恢复；快照 `running` 清单（连接建立/重连到达）——**刷新晚于任务开始**时事件已投递过且不随快照重放，页面据此发现当前会话在运行而未接管。两处均调 `sessions.attachRunningIfNeeded`（与进入会话时同一附加实现，`runs`/`attaching` 去重，未运行或本页已接管时 no-op）。
- **交互等待心跳与看门狗边界（前端断开不停任务）**：等待用户作答（审批/选择/填值/画图/捕获）期间无任何输出，工具执行心跳也不覆盖——等待注册时启动 25s 周期心跳 `event.interaction.alive`（payload `{kind, id}`，等待结束/超时/取消即停，与工具心跳同款，见「等待期不误判挂起」）。前端空闲看门狗只在「**页面可见** + **已连接** + **无待决交互卡** + **有效空闲（扣除不可见时段）超阈值**」时才中止并 `cancelTask`：断线期间无从判定服务端死活（且取消请求会随重连送达而杀掉仍在跑的任务），页面不可见时定时器被冻结/节流且本地连接标志仍为真（手机切 App/锁屏回来后补触发就会把整段后台当挂起），等待用户作答则本就不是挂起（服务端等待超时会推来结果事件自行刷新活跃）——三者任一命中看门狗都会把后台任务杀掉。不可见时长的真值源是 `web/src/page-idle.ts`（详见「页面不可见期间不误判挂起」）。
- **服务中断留痕（跨进程重启的中断可见）**：任务运行期间在会话目录写 `run.json`（`startedAt`/归属用户/本轮输入摘要），正常收尾（成功/失败/取消）删除；**残留标记 = 上一进程死在任务中途**——启动时扫描（`reportInterruptedRuns`）对这类会话补一条可见说明（user 角色 + `engineNote: "interrupted"`，与其余引擎注入同口径（内容头带 `【智体·任务中断】` 身份标记）；UI 渲染为「任务中断」通知条），并清标记防重复补记（写失败也清）。否则这类会话只剩「用户问了、助手一个字都没有」，既看不出发生过什么也判断不了要不要重发；说明同时进模型上下文（下一轮模型知道上一轮被打断了）。
- **发送背压**：服务端所有 WS 发送经统一 sink（`makeWsSink`）——连接发送缓冲超 16MB 判定慢客户端，主动断开让其走自动重连 + seq 重放收敛（此前 `ws.send()` 返回值被忽略，慢客户端 + 高频流式下发送缓冲无界增长）

WebSocket 消息格式（JSON）：
```ts
// 客户端 → 服务端（请求）
{ type: string; payload?: object; id?: string }
// 服务端 → 客户端（请求响应 + 主动事件）
{ type: string; payload?: object; id?: string; ok?: boolean; error?: string }
// 服务端主动推送的事件（带 seq，幂等序号）
{ type: "event.*"; seq: number; sessionId: string; payload: object; timestamp: number }
```

客户端请求类型：

| type | 说明 |
|------|------|
| `ping` | 心跳（客户端周期发送，复用请求-应答机制，服务端回 `pong`，保持连接穿越代理；pong 超时判定死连自动重连） |
| `auth.login` | 用户登录（服务模式），成功后建立用户上下文 |
| `auth.logout` | 登出，令牌失效 |
| `session.list` | 获取会话列表 |
| `session.create` | 创建新会话 |
| `session.import` | 导入会话（导出文件单会话条目 → 新会话：新 id、导入者为 owner；载荷校验不过 reject，见「会话管理·会话导入/导出」） |
| `session.get` | 获取会话详情 |
| `session.delete` | 删除会话 |
| `session.rename` | 重命名会话 |
| `session.pin` | 置顶/取消置顶会话（`pinned` 布尔；元数据操作不刷新 `updatedAt`） |
| `session.switch` | 切换当前会话 |
| `session.env.get` | 获取会话环境变量（内存态，含来源层级） |
| `session.env.set` | 设置/覆盖/删除会话环境变量（内存写入，不落盘） |
| `session.todo.get` | 获取会话待办清单 |
| `session.tool.get` | 获取当前生效工具集（含启用状态） |
| `sub_agent.list` | 列出可用子Agent |
| `sub_agent.load` | 装载子Agent 模块（按需装载；可选 `sessionId`——传入时装载到该会话：注册工具 + 提示词消息写入会话记录，缺省仅全局注册工具） |
| `session.files.list` | 列出会话临时文件 |
| `session.files.get` | 读取会话临时文件内容 |
| `session.current` | 获取当前会话 |
| `sub_agent.unload` | 卸载子Agent（带 `sessionId` 为会话级；全局形态仅 admin） |
| `session.restore` | 从 `trash/` 恢复已归档会话（归属用户或 admin） |
| `session.cancel` | 取消当前任务 |
| `session.attach` | **运行中会话附加快照**（页面刷新/切换恢复用）：返回 `{ running, startedAt, stream, pending, lastSeq }`——在途流式累积（未持久化部分文本/推理 + messageId/session 标记）、待决交互清单（审批/选择/填值/画图/捕获，含重渲染所需全部载荷）、任务开始时刻与该用户事件日志基线 seq；`running=false` 表示未运行（前端放弃附加）；归属校验同其余会话操作 |
| `session.compact` | 主动压缩会话上下文（支持范围参数，返回压缩条数与摘要） |
| `session.prompt` | 发送对话消息（WS 通道发起任务，流式事件经 `event.*` 推送回流，reply 仅确认；payload 支持 `messageId` 可选字段，语义同 REST prompt） |
| `session.attachment.upload` | 上传附件（WS 通道，base64 整体传输，返回会话内引用路径） |
| `choice.decide` | 提交用户选择（ask 选项询问分支阻塞等待的回应；`option` 单选 / `options` 数组多选 / `refuse: true` 或 option、options 均缺失表示拒绝回答） |
| `env.decide` | 提交用户填写的环境变量值（ask 填值分支阻塞等待的回应，envId + value；value 缺失表示拒绝提供） |
| `draw.result` | 提交前端渲染结果（show 图表分支阻塞等待的渲染回传，renderId + ok + error） |
| `capture.result` | 提交前端页面捕获结果（page_capture 工具阻塞等待的捕获回传，captureId + html + imageBase64[可选，png/jpeg data URL 或裸 base64] + error[可选]） |
| `feedback.list` | 查询反馈（管理员可查全部用户，普通用户仅本人；支持 messageId/sessionId/type 过滤） |
| `user.list` / `user.create` / `user.update` / `user.delete` | 用户管理（服务模式，仅管理员；`user.list` 应答为 `{ users: […] }` 包裹，SDK `listUsers()` 解包后直接返回数组——解包缺失时前端 `for…of` 包裹对象抛错，用户管理/审批列表静默空白） |
| `approval.decide` | 审批决策（通过/拒绝，对应待审批的工具调用）；**决策已失效时返回错误码 `expired`**（等待超时/已在其他地方处理/任务已结束；同规则适用于 `choice.decide`/`env.decide`/`draw.result`/`capture.result`，前端据此提示并撕掉失效卡片，不再假报成功） |
| `feedback.submit` | 提交反馈（点赞/点踩/文字，关联会话与消息；服务端自动补 model/subAgent 关联，见「用户反馈」） |
| `state.snapshot` | 获取状态快照（主动请求，幂等；与自动推送 payload 一致） |
| `sync.request` | 断线补偿：按 `lastSeq` 请求重放离线期间错过的日志事件；`sync.replay`（`{events, lastSeq}`，seq 严格递增、payload 含 sessionId）或 `overrun`（`{events:null, overrun:true, lastSeq}`，缺口→客户端全量重同步） |
| `auth.login` | 登录支持两种形式：`{username, password}`（密码）与 `{token}`（已有令牌，SDK 重连自动重新认证用）；成功后连接级事件订阅自动重绑到新用户并推送状态快照 |

服务端主动推送事件（`id` 为空，`type` 以 `event.` 为前缀，**均携带 `seq`**——每用户日志序号，重连后按 seq 重放补偿；旧服务端不携带 seq）：

| type | 说明 |
|------|------|
| `event.message.delta` | LLM 文本增量（流式）；子会话运行过程的文本增量携带 `subSession: true` + `subSessionId`（前端渲染进观察面板里该 run 的过程区）；`messageId` 每轮刷新（引擎每轮生成新 id），前端据此检测轮界——主循环 text 路径与子会话过程区均按 id 变化封段（后台会话工具事件不渲染卡片从而不在工具调用处封段，轮界检测是唯一分段保障） |
| `event.message.done` | 一条完整消息生成完成（子会话轮的 done 同样携带 `subSession: true` + `subSessionId`） |
| `event.message.reasoning` | 推理内容增量（reasoning_content/thinking，前端思考中展开实时展示、推理段结束自动折叠）；子会话运行过程的推理增量携带 `subSession: true` + `subSessionId` |
| `event.subsession.start` | 子会话 run 开始（含 runId + agents 列表 + input + depth；**每轮重推、同 runId 幂等**——前端已有记录则只刷新元信息，记录/过程区随页面重载丢失后据此重建）；携带 `subsession`（子会话名）+ `model`（模型路由名，未指定缺省），前端入口行与面板标题渲染「🌿 子会话 · 名（模型）」 |
| `event.subsession.done` | 子会话 run 结束（含 runId + agents + output[最终返回]；异常时 output 为空并携带 error），前端定稿输出与耗时（入口行与面板行同步刷新）；`subsession` 标识子会话名。**每轮模型回复结束与循环上限退出都会推送**（与 start 同为每轮重推、同 runId 幂等），前端靠封段而非等 run 结束 |
| `event.subsession.merged` | 子会话报告合入父上下文（subsession_run 继承形态**最终合入**与运行中 `subsession_merge` 交出 content 的**阶段性合入**均推送，含 messageId/runId/name/model/text[合并消息全文]）：前端实时渲染「子会话合入」通知条——**内容按 markdown 渲染**，并附「查看子会话过程」直达观察面板（消息落盘 `role: "user"` + `engineNote: "subsession"`；历史回放由存储中的合并消息承担，阶段性合入同时认回面板的合入记录） |
| `event.tool.call` | 工具开始执行（含名称与参数）；子会话运行过程中的工具调用携带 `subSession: true` + `subSessionId`；**门控说明性结果（缺参/未知工具/通道禁用/安全拦截/无交互拒绝/重复中断/重复终止）同样推送**（与结果事件成对，前端实时建卡，不依赖刷新回看落盘历史） |
| `event.tool.result` | 工具执行结果（含截断标记与文件路径）；子会话运行过程中的结果携带 `subSession: true` + `subSessionId`；**审批拒绝/超时与取消/中断占位补写同样推送**（实时卡片落终态，不停留「执行中」） |
| `event.todo.update` | 待办清单变更（新增/状态/进度） |
| `event.tool.call` / `event.tool.result`（待办续做） | 待办续做注入复用工具事件通道：模拟 todo 查询调用对推送标准工具事件对（name="todo"、toolCallId 前缀 `todo-cont-`），前端自动建待办卡片并刷新为清单——与真实 todo 调用零差异；不再有独立的 `event.todo.continue` 通知条事件（存量会话旧形态 user 提醒由回放兼容） |
| `event.verify.nudge` | 收尾验证提醒（改代码未跑测试的任务结束注入一次，user 软性提醒（`engineNote: "verify"`）；含 messageId/text，前端据此实时渲染「引擎提示」通知条） |
| `event.draw.render` | 画图渲染请求（show 图表分支执行中推送，含 renderId + 图表源码 + **`format` 图表语言**（mermaid/plantuml/d2/echarts），前端按语言本地渲染后经 `draw.result` 回传） |
| `event.env.request` | 环境变量填值请求（ask 填值分支执行中推送，含 envId + name + description + secret[是否敏感值]，前端弹窗填值后经 `env.decide` 回传；值注入本次任务 env 并保存到浏览器本地） |
| `event.capture.request` | 页面捕获请求（page_capture 工具执行中推送，含 captureId + fullPage[是否整页截图]，前端捕获当前页面渲染后 DOM html + 截图后经 `capture.result` 回传） |
| `event.approval.request` | 审批请求（含工具信息、重试次数与**调用参数 `arguments`**——飞书审批卡片据此展示参数摘要） |
| `event.task.queued` | 任务入队（含任务 ID/类别/来源与队列位置） |
| `event.task.start` | 任务开始执行（含类别/执行体/来源/执行会话） |
| `event.task.result` | 任务执行结果（成功/失败/跳过/超时与输出摘要、自动停用标记；prompt 型过程经消息流呈现） |
| `event.task.queue` | 任务队列变化（额度/排队数/运行中数） |
| `event.task.done` | 本轮任务完成 |
| `event.task.start` | 本轮任务开始（前端据此切换运行态/信号灯） |
| `event.choice.request` | ask 选项询问请求（前端弹选择卡片，经 `choice.decide` 回传） |
| `event.tool.result.start` | 工具结果输出开始（长结果分段落屏前的信号） |
| `event.tool.alive` | 长工具执行心跳（默认 25s，刷新前端空闲看门狗，见「等待期不误判挂起」） |
| `event.interaction.alive` | 交互等待心跳（默认 25s，payload `{kind, id}`；等待用户作答期间无输出，看门狗不得据此判定挂起，见「交互等待心跳与看门狗边界」） |
| `event.message.intermediate` | 助手中间轮文本（飞书 `notifyIntermediate` 通道预览用） |
| `event.message.compact` | 上下文压缩 / 护栏降级 / 超限裁剪通知（含 `degraded` 标记——`tool-images`/`user-images`/`user-message` = 溢出护栏降级，`trim` = 消息条数上限裁剪；**前端按此区分标题**：无标记 =「已压缩 N 条历史消息」，`trim` =「历史消息条数超限裁剪」，其余标记 =「上下文溢出护栏」，具体降级类型与后果由 `summary` 承载） |
| `event.task.error` | 本轮任务出错（含错误信息） |
| `event.model.error` | **模型服务异常（非终态，引擎将自动重试）**：接口异常/空响应重试前推送 `{error, retry, maxRetry}`——重试退避期间任务无输出，前端据此在消息流尾部挂**常驻**异常记录块（`model-error.ts`）：每次重试一行「模型服务异常（第 N/M 次重试）：原因」，行尾状态随进展更新（重试中 → 已恢复 / 重试未成功），**不随恢复输出或任务结束消失**（报错信息是排查依据，恢复与否都留痕）；同一条重试的重放原地更新不堆叠；重试耗尽的最终失败仍走 `event.task.error` |
| `event.session.ctx` | 运行中上下文大小更新（每轮模型调用后推送，含 ctxTokens token 计数：真实 usage 基准 + 未发送增量估算，无真值时全量估算 + 工具 schema 段估算兜底；会话列表 k 显示用。**推送同时把展示值与真实 usage 基线落盘**——`store.updateCtxStats` 只重写 meta.json：列表/状态快照/页面刷新与推送同口径，否则运行中反复刷新会在「上次任务结束时的旧值」与当前真值之间来回跳；基线随轮次即时写入，任务中断/重启后压缩判定与展示仍有真值（见「上下文占用口径」）。接口返回缓存字段时携带 `ctxCachedTokens`：同一次调用的提示词缓存命中 tokens，前端上下文圆环悬浮展示命中率；压缩/护栏降级改变上下文时立即补发一次，圆环当场回落） |
| `event.session.tps` | **输出速率帧**（标题栏速率仪表的数据源，`{tps, outTokens, genMs, est, active}`）：`tps = 输出 tokens ÷ 生成窗口`（窗口 = 首个输出 chunk 到末个输出 chunk，**不含首 token 等待**，量的是解码速度而非接口响应延迟）。**一次模型调用内按 1s 节流持续推送生成中帧**（`est: true, active: true`，服务端按 CJK 感知字符折算估算，窗口 ≥300ms 才报）——长回答全长期间数字持续刷新；**调用结束推收尾帧**（`active: false`，接口返回 usage 时 `est: false` 为实测口径，不返回时仍为估算收尾）。产出瞬时完成（收尾窗口 <50ms，非流式）时不推帧。前端只展示不自算，两帧之间保持上一帧（见「输出速率」） |

另有一个**非事件推送**：`state.snapshot`（`id` 为空，建连/登录后自动推送状态快照，客户端更新 MVC 模型并触发 `onSnapshot` 订阅）。

#### REST HTTP（同步通道）

面向业务系统同步集成，同一套核心能力以 HTTP 形式暴露，返回 JSON：

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/health` | GET | 健康检查（**公开端点**，服务模式下也无需鉴权——探活/负载均衡探测） |
| `/api/v1/users` | GET/POST | 用户列表/创建（服务模式，管理员） |
| `/api/v1/users/:id` | PATCH/DELETE | 用户启用/禁用/删除（管理员） |
| `/api/v1/sessions` | GET/POST | 会话列表/创建 |
| `/api/v1/sessions/:id` | GET/DELETE/PATCH | 会话详情/删除/重命名与置顶（PATCH body：`name` / `pinned`） |
| `/api/v1/sessions/:id/prompt` | POST | 发送消息，**非流式 JSON 返回**（同步等待任务完成，`{ message: 最终 assistant 消息, error?: 任务错误 }`）；body 支持附件引用、`env`（浏览器本地环境变量，临时注入仅本次任务生效，不持久化）、`messageId`（可选：客户端生成的用户消息 id，撤回/反馈定位用；非法格式服务端回退自动生成）、`interactionMode`（默认 `none`）与 `stream`（默认 `false` 仅最终响应）、`autoApprove`（可选布尔：`true` 需审批工具自动通过/`false` 无交互通道下直接拒绝，缺省通道默认姿态——本地自动通过、服务模式拒绝，见「交互模式」）；缺 `prompt` 返回 400；**流式输出请走 WebSocket（`/ws`）** |
| `/api/v1/chat` | POST | **单 HTTP 一站式调用**：一次请求完成「建会话（`sessionId` 缺省自动创建，`name` 可选命名）→ 执行任务 → 返回最终回复」，返回 `{ sessionId, message: 最终 assistant 消息, error? }`（`sessionId` 供后续请求续聊多轮）；带 `sessionId` 在既有会话续聊（id 白名单校验 400 / 不存在或非本人 404）；`interactionMode` 固定 `none`，body 其余字段（`prompt` 必填 400、附件/env/messageId/stream）与 `prompt` 端点同规则，`autoApprove` 支持自动审批（与 `prompt` 端点同语义）；与 `prompt` 共用每用户速率限制桶 |
| `/api/v1/sessions/:id/attachments` | POST | 上传附件（multipart，多模态内容），返回会话内引用路径 |
| `/api/v1/auth/me` | GET | 当前登录用户信息（服务模式；本地模式为 admin 用户） |
| `/api/v1/auth/exchange` | POST | **外部身份兑换**（服务模式 + 已配置验证器；body `{username, credential}`）：网站本地登录态 → GEBAI 令牌；400 缺参 / 401 验证失败 / 404 未启用 |
| `/api/v1/auth/external-config` | GET | **外部身份扩展点探测**（公开端点）：`{enabled, storageKey?, autocreate}`，供 Web UI 启动时读取（不泄露密钥） |
| `/api/v1/sessions/:id/truncate` | POST | 撤回消息：删除 before（消息 id，**用户/助手消息均可**）及其后的所有消息（body: `{ before: string }`）；消息 id 为持久化 id——前端发送时携带 `messageId` 后撤回可对齐当前会话刚发的消息，助手最终回复按流式 messageId 落盘同样即时可撤回；**会话有任务运行时 409 拒绝**（先停止或等任务完成） |
| `/api/v1/sessions/:id/cancel` | POST | 取消当前任务 |
| `/api/v1/sessions/:id/approval` | POST | 审批决策（通过/拒绝） |
| `/api/v1/sessions/:id/choice` | POST | 选择决策（ask 选项询问分支等待的用户回应，body: choiceId + option 单选 / options 数组多选 / refuse=true 拒绝，option、options、refuse 至少其一，options 不得为空） |
| `/api/v1/sessions/:id/draw` | POST | 画图渲染结果回传（show 图表分支等待的前端渲染结果，body: renderId + ok + error） |
| `/api/v1/feedback` | POST/GET | 提交/查询反馈（提交自动补 model/subAgent 关联；管理员 GET 可查全部用户并导出分析） |
| `/api/v1/triage/analyze` | POST | **大小模型协同**：body = 与 `triage_run` 工具**同一份参数契约**（`core/triage/params.ts` 的 `TRIAGE_PARAM_SPECS`）——`items`/`items_file`、`target`(+`target_api_key`)、`schema`/`label_enum`、`threshold`/`min_evidence_chars`/`accept_labels`、`escalate`、`l1_*`（含 `l1_concurrency`/`l1_retries`/`l1_gate`）、`l2_*`、`job_id`/`job_dir`/`result_limit`、`mode`（`async` 立即返 202 + job_id + 后台任务 id `job`；用既有轮询端点跟踪）、**REST 专属 `sync_timeout_ms`**（同步整体超时，缺省 120s，0 = 关）；嵌套写法（`l1.target`）与旧键名作兼容输入。**任务配置可经文件预设**（`{GEBAI_HOME}/triage/preset.json`）——配好后调用只需传 `items`（优先级「显式参数 > 预设 > 内置缺省」；每次调用重读，改完即生效）。同步返 `{job_id, job_dir, state, escalate, target, summary, total, returned, results}`；**超出 sync 超时即转后台并返 202**（`state: running` + `poll` + `degraded` 说明——任务不中断，已跑进度不浪费）。每用户限流（缺省 10 突发 / 2 每秒，`GEBAI_TRIAGE_RATE_BURST` / `GEBAI_TRIAGE_RATE_PER_SEC` 可调，任一为 0 则不限流） |
| `/api/v1/triage/jobs/{id}` | GET | 研判任务进度/汇总（未完成时按 l1.jsonl 行数报进度**并附已定案的部分结论**：`partial:true` / `results_ready` / `results`；`?job_dir=` 可指定目录） |
| `/api/v1/triage/jobs/{id}/results` | GET | 研判任务逐条结论（`?layer=L1|L2`、`?only=ok|failed`、`?limit=`；**运行中也可读**——增量消费，`partial:true` 时是已完成部分；全量始终在产物目录的 results.jsonl） |
| `/api/v1/sessions/:id/env` | GET/PUT | 获取/设置会话环境变量（内存态，不落盘） |
| `/api/v1/sessions/:id/compact` | POST | 主动压缩会话上下文（body 可指定范围） |
| `/api/v1/sessions/:id/todos` | GET | 获取会话待办清单 |
| `/api/v1/sessions/:id/files` | GET | 列出会话临时文件（含子目录，返回路径/大小/修改时间） |
| `/api/v1/sessions/:id/files/content` | GET | 读取会话临时文件内容（`?path=`，文本截断预览或原始内容） |
| `/api/v1/sessions/:id/files/download` | GET | 下载会话临时文件（`?path=`，二进制/文本原样下载，`Content-Disposition`） |
| `/api/v1/sessions/:id/files/download` | POST | 多选打包下载（body 指定 paths 列表，返回 zip） |
| `GET /files` | GET | **文件工作台页面**（独立 Vite 入口 `files.html`；`GEBAI_FS_ENABLED=false` 时 404） |
| `/api/v1/roots` | GET | 文件工作台根清单（`sess:` 会话工作区 / `proj:` 预置项目（含内置「歌白」）/ `user:` 用户目录 / `abs:` 白名单根）+ 能力开关（fsEnabled/fsWrite/gitEnabled/gitWrite/gitRemote/writable/sandboxed/上限）；每根附 Git 标记（`isRepo`/`branch`/`repoRoot`）——探测语义（去重 + 浅路径优先父仓库短路 + ≤8 并发 + 60s 缓存）见「文件工作台 → 根清单探测」 |
| `/api/v1/roots/resolve` | GET | 单个根解析（?id=）——根可用性与真实路径校验 |
| `/api/v1/fs/list` \| `/fs/tree` | GET | 目录列举（懒加载，?root&path&sort&order&showHidden）/ 树快照（?root&depth） |
| `/api/v1/fs/stat` \| `/fs/read` | GET | 元信息 + `etag`（乐观锁）/ 文本读取（编码·换行·binary·truncated） |
| `/api/v1/fs/raw` | GET | 原始字节（支持 HTTP Range：图片/视频/PDF/大文件） |
| `/api/v1/fs/office` \| `/fs/archive` | GET | Office 阅读视图转换（docx/xlsx/pptx）/ 压缩包条目列表 |
| `/api/v1/fs/search` | GET | 名称/内容搜索（ripgrep 优先（异步 spawn，不阻塞事件循环），回退内置遍历；mode/glob/regex/maxResults） |
| `/api/v1/fs/watch` | GET | **变更监听（长轮询）**：`?root&dirs&rev&wait&git`——为 `dirs`（`.` = 根本身）挂 `fs.watch`，有变化立即返回 `{changed,rev,paths,git}`，无变化挂到 `wait` 秒回心跳；`rev` 缺省 = 只取基线；服务端中枢 `core/fs/watch.ts`（`GEBAI_FS_WATCH=false` 时返回 `enabled:false`） |
| `/api/v1/fs/download` | GET/POST | 单文件流式下载（?root&path）/ 多选或目录打包 ZIP（body paths，UTF-8 文件名） |
| `/api/v1/fs/write` | PUT | 保存文件（`etag` 乐观锁，冲突返回 409 + 当前磁盘内容；编码/换行保真） |
| `/api/v1/fs/{mkdir,rename,move,copy,delete,upload,archive/extract}` | POST | 新建目录 / 重命名 / 移动 / 复制 / 删除（物理删除，不可恢复） / 上传（multipart） / 解压到工作区 |
| `/api/v1/lsp/servers` | GET | 工作台**语言服务器清单**：本机探测到的 `servers`（语言 / 服务器标识 / 命令 / 参数）+ 已配置但未安装的 `missing` + 覆盖表解析问题 `errors`；`GEBAI_LSP=false` 或服务未注入时回 `enabled:false`（前端静默降级，不注册任何 provider） |
| `/api/v1/git/status` | GET | 仓库状态：`root` = 请求时给的**根 id**（回显）、`repoRoot` = **仓库根绝对路径**（子目录根只能靠它往上定位仓库）、分支、变更分组与计数。两个「根」曾经被写成同一个字段（`{ root: rootId, ...status }` 里 status 自带的 root 是仓库路径，展开后把 rootId 盖掉了）——客户端拿到的 `root` 与类型说明不一致且不报错，子目录根算不出前缀就是从这里开始的，现拆为两个明确字段 |
| `/api/v1/git/diff` \| `/git/compare` | GET | **任意两端差异/对比**（?root&from&to&path&mergeBase；端点：`WORKTREE`/`INDEX`/空串（配合 WORKTREE 表未暂存）/任意 rev） |
| `/api/v1/git/file-diff` \| `/git/content` \| `/git/show` | GET | 单文件差异 / 端点内容（Monaco 并列视图两侧文本）/ 指定提交文件内容 |
| `/api/v1/git/log` \| `/git/commit` \| `/git/file-history` \| `/git/blame` | GET | 提交日志（可路径/作者/关键字/时间范围过滤、正则与大小写开关、分页）/ 提交详情 / 单文件历史 / 逐行追溯 |
| `/api/v1/git/refs` \| `/branches` \| `/tags` \| `/remotes` \| `/stash` \| `/conflicts` | GET | 引用（分支/标签/最近提交/HEAD）/ 分支 / 标签 / 远程 / 储存（stash）列表 / 冲突四方内容 |
| `/api/v1/git/stage-hunks` \| `/unstage-hunks` \| `/discard-hunks` | POST | **按块/按行**暂存 / 取消暂存 / 放弃（`selections: [{hunk, lines?}]`，行序与 `GitFileDiff.hunks[].lines` 一致；`backup` 默认可关，前端传 `false`） |
| `/api/v1/git/stage-content` | POST | 把一份**任意内容**写入暂存区（三向暂存编辑器的保存动作；hash-object + update-index，不碰工作区与 HEAD） |
| `/api/v1/git/history-edit` | GET/POST | 查询未完成的编辑历史计划 / 按计划重放提交改写历史（`steps` 按**应用顺序**；冲突或 `edit` 停顿时计划落盘在仓库的 git 目录） |
| `/api/v1/git/history-edit/{continue,abort}` | POST | 继续未完成的编辑历史 / 中止并把分支恢复到改写前的 HEAD |
| `/api/v1/git/{stage,unstage,discard,commit,branch,tag,checkout,merge,rebase,cherry-pick,revert,reset,stash,remote,fetch,pull,push,init}` | POST | Git 写操作（统一「开关守卫 → 执行 → 审计」；破坏性操作默认先备份并返回可恢复引用；`GEBAI_GIT_WRITE`/`GEBAI_GIT_REMOTE` 可关） |
| `/api/v1/tools` | GET/PATCH | 工具集查询/启停配置 |
| `/api/v1/sub-agents` | GET | 子Agent 能力列表（名称、描述、工具、打包状态） |
| `/api/v1/webhooks` | GET/POST/DELETE | Webhook 注册/管理 |
| `/api/v1/tts` | POST | 语音朗读：`{ text, voice?, rate?, pitch?, volume? }` → `audio/wav` 字节流（不落盘；长文本按句分片合成后拼接，同文同参进程内缓存；平台无内置离线引擎时 503 并说明不做联网合成）；`/api/v1/tts/status` 探测可用性与缓存现状 |

- 认证方式：`Authorization: Bearer <token>`（用户令牌，登录获取）、`Authorization: Basic base64(username:password)`（HTTP Basic 单次请求直验，等价隐式登录——复用密码校验与登录限流，不签发令牌，适合简单单次调用；**base64 非加密，须 HTTPS**），以及部署方经**凭证来源扩展点**增开的载体（缺省另含同源 cookie `gebai.auth.token`，**仅 GET/HEAD**：浏览器原生资源请求无法带 `Authorization` 头，服务端据此取令牌并走同一 `authorize()` 校验）；无独立服务密钥
- 全端点支持 CORS，可通过环境变量配置允许的来源
- 会话操作与 WebSocket 共用同一套归属校验与隔离逻辑
- 消息发送与流式输出统一走 WebSocket：`sendPrompt` 经 WS `session.prompt` 发起任务，引擎事件经连接级订阅推送，SDK 转换为 `ChatChunk` 迭代返回（`wsEventToChunk`，与原 SSE 契约字段一致：文本增量/工具调用/审批请求/任务完成/错误）

#### 事件推送（Webhook）

业务系统可注册 Webhook（`/api/v1/webhooks`，GET 列表 / POST 注册 / DELETE 删除），服务端在关键事件（任务完成、审批请求、工具执行失败，默认三类，可自定义事件白名单）时向指定 URL 推送 JSON 事件，实现异步集成：

- **签名校验**：配置 `secret` 时推送头携带 `X-Gebai-Signature: sha256=<HMAC-SHA256(secret, body)>`，接收方验签防伪造；列表/注册返回的 secret 已脱敏（`***`）
- **重试策略**：投递失败（HTTP 非 2xx 或网络错误）自动重试，最多 3 次、指数退避（0.5s / 1s / 2s）
- **事件过滤**：按注册时的 `events` 白名单过滤；服务模式下事件按「会话归属」过滤——仅推送该 Webhook 注册者可访问会话的事件
- **存储**：配置持久化于 `{GEBAI_HOME}/webhooks.json`，重启保留；注册需校验 URL 为 http(s)
- 与 WebSocket 事件同源（均订阅服务端 EventBus），负载格式与 `AgentEvent` 一致：`{ type, sessionId, payload, timestamp }`

### 业务系统集成

面向其他前端与业务系统对接，提供多层次的集成方式：

- **官方 SDK（`@gebai/sdk`）**：WebSocket 连接管理 + REST 调用封装，开箱即用（`login`、`sendPrompt` 流式消费等）
- **OpenAPI 规范**：`/api/docs` 的**端点表由路由注册自动生成**（请求时遍历 `app.routes`，故注册顺序无关；只取 `/api/` 域、跳过 Hono 派生的 `HEAD` 与中间件 `ALL`，路径参数 `:id` → `{id}`），**摘要取自补充表**（未登记的端点仍列出并标注「未登记摘要」，不隐藏）——手写清单会与实现漂移（新增忘登记、删掉的仍列着），生成器保证「有哪些端点」永远与代码一致；响应含 `x-endpoints-total`/`x-summary-covered` 便于判断摘要覆盖度。业务系统可据此生成任意语言客户端（Java/Go/Python 等）
- **任意前端接入**：任何支持 WebSocket/HTTP 的前端（React/Vue/小程序/App 等）均可直接对接双通道 API，不绑定 UI
- **Web UI 嵌入**：内置 Web UI 支持 iframe 嵌入业务系统页面，可通过 URL 参数指定 UI 风格/自定义主题变量（`gb_style`/`gb_vars`/`gb_cny`）；**无「URL 参数携带令牌免登录」**——登录态只存浏览器本地（`localStorage`），跨系统免登录走外部身份兑换（`gb_ext_username`/`gb_ext_credential`）
- **接口认证**：REST 支持 `Authorization: Bearer <token>`（先登录获取令牌）、HTTP Basic（单次请求直验账号密码，复用登录限流、不签发令牌）与部署方自定义的凭证来源（缺省含同源 cookie，仅 GET/HEAD；见「凭证来源扩展点」）三种形态，WS 统一 `auth.login`；不提供独立服务令牌（原 `X-API-Key` 服务身份机制已移除）
- **外部身份扩展点（同源集成）**：服务模式下网站可复用自身登录态作为 GEBAI 用户——配置 `GEBAI_EXTERNAL_AUTH_*` 后，前端把本地登录态经 URL 参数（`?gb_ext_username=&gb_ext_credential=`）或 localStorage（`GEBAI_EXTERNAL_AUTH_STORAGE_KEY`，同源直读）交给 Web UI，Web UI 启动时自动调 `POST /api/v1/auth/exchange` 兑换令牌（HMAC 验签或 HTTP 回调验证，见「认证与鉴权」）；业务系统也可用 SDK `exchangeExternalUser` 自行对接（React/Vue 等任意前端），无需依赖内置 UI
- **身份对接**：服务模式下支持**外部身份兑换扩展点**（`GEBAI_EXTERNAL_AUTH_SECRET` HMAC / `GEBAI_EXTERNAL_AUTH_URL` 回调，见「多用户隔离与安全」），复用业务系统已有账号体系；**标准 SSO/OIDC 对接未实现**（列于「待实现」）
- **URL 携带提示词自动运行（`gb_prompt`）**：业务系统跳转链接可直接带任务进来——`?gb_prompt=<文本>`（URL 编码）在页面首屏就绪后自动**新建会话并发送该提示词**，随后把地址栏 `history.replaceState` 为会话地址（`?session=<会话 id>`，其余参数保留、`gb_prompt`/`gb_new` 移除）；刷新因此只打开该会话，**不会重复创建会话、重复执行任务**。`gb_new=1` 强制新建（同链接带 `session` 时也新建）；带 `session=<id>` 且会话存在时改为在该会话续接发送，该会话运行中则不抢占（重定向后把提示词回落输入框并提示）；建会话失败时提示词回落输入框且**不重定向**（刷新重试仍会执行）。关闭入口用独立配置文件的 `allowUrlPrompt: false`（默认开）；解析/重定向/编排见 `packages/web/src/url-prompt.ts`
- **二开前端脚本（`custom/web/`）**：二开域内的浏览器端脚本，**先于应用初始化执行**——`packages/web/vite.config.ts` 的 `gebai-custom-web` 插件把已启用的脚本（`gebai.config.js` / `init.js`，出厂为同目录 `*.example.js` 示例模板，复制改名即启用；示例不参与接入，上游更新复制 `custom/` 时只刷新示例、不动已启用文件）带到前端产物根（dev 按产物根路径伺服；构建结束时复制），`index.html` / `files.html` 在入口模块脚本之前引入**存在的那几个**（普通 script 同步执行，而 module 为 deferred，故两者都在应用初始化之前运行；`./` 相对路径，反代子路径下成立）。两个文件分工：`gebai.config.js` 是**配置**——经 `window.__GEBAI_WEB_CONFIG__` 暴露 `env`（预置浏览器环境变量，随消息请求注入，与设置面板同一通道）、`envFromStorage`（环境变量 ← 宿主 localStorage 键，运行时读取）、`storage`（歌白设置键 ← 宿主键/字面值，仅在歌白键**未设置**时写入，`force: true` 才覆盖）、`allowUrlPrompt`、`bootTimeout`（二开初始化脚本异步引导的等待上限，默认 3000ms、0 = 不等待）；`init.js`（产物根名 `gebai.custom.js`）是**初始化脚本**——页面加载即执行，承担二开的本地存储初始化、用户注册与登录等（调 `/api/v1/auth/*` 写 `gebai.auth.token`，或写宿主登录态交给外部身份兑换）；需要 await 的动作赋给 `window.__GEBAI_WEB_BOOT__`（Promise / 返回 Promise 的函数 / 二者组成的数组），`awaitCustomBoot` 在两个页面入口的 init 首行等待其完成（异常与超时只记控制台警告、不阻塞页面）。配置应用时机在最早期（先于主题/低功耗/文件展示等读取 localStorage 的模块），优先级为 URL 参数 > 用户本次手动选择 > 本地存储既有值 > 配置文件 > 服务端全局配置 > 内置默认；读取与容错归一化见 `packages/web/src/boot-config.ts`（配置写错不使页面失效，按空配置处理）。目录内脚本按产物根文件名自动接入（无需清单），升级时整个 `custom/` 拷到新仓库根即完成迁移
- **审批集成**：审批请求可通过 REST/Webhook 转发到业务系统审批流，而非局限于内置 UI

### 飞书机器人集成

支持将 GEBAI 接入**飞书机器人**，用户可在飞书聊天中直接使用 Agent 能力（`GEBAI_FEISHU_BOT_ENABLED=true` 启用，需配置 `GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`）：

- **接入方式**：**长连接模式**（已实现）——服务端主动出站连接飞书，无需公网回调地址，本地桌面/服务端均可使用。协议为飞书现行 protobuf 帧协议（参照官方 SDK `lark_oapi`）：`POST /callback/ws/endpoint` 端点发现（`{AppID, AppSecret}`）→ WebSocket 连接（URL 携带 `device_id`/`service_id`）→ 心跳 ping（服务端下发 `PingInterval`，pong 可动态更新配置）→ 事件 DATA 帧（schema 2.0 JSON，`sum>1` 分片按 `message_id` 合包）→ 处理完回发 ACK 帧（`{"code":200}` + `biz_rt` 耗时）；**卡片回传交互（card.action.trigger，新版卡片回调）经事件帧（`type="event"`，schema 2.0，`header.event_type="card.action.trigger"`）下发**，`onEvent` 回调按事件类型分流：卡片回调同步快速处理并**返回卡片响应体**（toast/card）——ACK 封官方信封 `{"code":200,"data":"<base64(响应JSON)>"}`（对齐 lark_oapi ws/client.py：`resp.data = base64(JSON)`）；普通消息事件 fire-and-forget 立即回 `{"code":200}`；**更新卡片的 `card` 字段必须为官方包装 `{"type":"raw","data":{卡片JSON}}`**——裸卡片 JSON 判错误码 200672「响应体格式错误」，客户端按钮转圈不消、回调失败重推；3 秒内必须响应；旧版 `type="card"` 帧路径保留兼容（官方 SDK 新版对该帧直接丢弃）；`operator` 兼容新版扁平 `{open_id}` 与旧版嵌套 `{operator_id:{open_id}}` 两种形态；**断连韧性**——断连日志携带 close code/reason 与连接存活时长（区分服务端踢线/代理掐断/客户端判死）；判活时钟以「最近任意服务端帧」计（不只 pong，防 pong 丢失误判掐线造成周期性断连），静默超 `3x PingInterval` 主动重连（不依赖 onclose 回调，半开 socket close 可能无回调）；握手 15s 超时（防火墙黑洞下 open/error/close 均不触发，无超时会永久挂起）；重连节奏为断开后立即重连（`ReconnectNonce` 抖动）、失败后前 3 次快速重试（≤5s，网络闪断秒级恢复）再按服务端 `ReconnectInterval`、达 `ReconnectCount` 上限后降频 60s 续试不永久停摆。帧编解码为自研极简 protobuf 实现（`feishu-bot/pb.ts`），不依赖第三方 SDK；**启动时机**：长连接在服务监听建立之后异步发起（`boot/serve.ts`），握手不阻塞服务可用，失败只记 error 日志、服务照常运行
- **身份映射**：本地模式全部映射到 admin 用户（`AuthService.defaultUser`；**旧版默认用户为 `default`——升级后启动时自动迁移遗留归属与旧会话：`feishu/chat-owners.json` 内 `default` 归属改写为当前默认用户、`users/default/sessions/` 下飞书会话目录搬迁至 `users/admin/`、chat.json 的 `userId` 同步改写，幂等可重试、仅本地模式触发**）；服务模式按飞书用户 `open_id` 自动创建 GEBAI 映射用户（用户名 `feishu_{sha256(open_id)前24位}`——open_id 可含大写/超长，直接拼接过不了用户名白名单，哈希派生确定性防碰撞；随机密码不可密码登录，角色普通用户，管理员可在用户管理禁用），10 分钟内存缓存；**映射用户创建失败即中止任务**（绝不以默认 admin 兕底运行——那会让飞书侧任务以沙箱豁免身份执行）
- **会话映射**：每个飞书单聊/群聊自动关联独立 GEBAI 会话（**会话 id = `sha256("feishu:"+chat_id)` 前 32 位 hex**——满足存储层会话 id 白名单 `[0-9a-f]{32}`，`feishu_{chat_id}` 形态会被 `sessionPath` 拒绝；确定性派生，重启不变；会话名取飞书会话名称），消息上下文与 Web UI 完全互通；群聊成员共享同一会话（引擎身份为会话创建者，不因他人发言重建/覆盖）；**会话被删除或 GC 归档后再来消息时，以归属身份（`recreateSession`，清除移除标记）重建空会话**——确定性会话 id 不复用新 id、归属映射（`feishu/chat-owners.json`）跨重启留存
- **消息互通**：文本消息（群聊自动剥离 `@_user_N` 提及占位）→ `session.prompt`，**入站 prompt 带发送者姓名前缀**（`【张三】内容`——经 contact API 尽力而为查询并按 openId 缓存，应用需开通 `contact:user.base:readonly`；查不到/无权限则不带名，不阻断入站，实现 `feishu-bot/api.ts` 的 `getUserName`）；图片消息 → 下载为附件（魔数探测 mime）进入会话；任务完成发最终 **interactive 卡片**（JSON 2.0 markdown 渲染，超长截断并提示 Web UI 查看，**以「回复」引用原用户消息**）；开启 `GEBAI_FEISHU_BOT_NOTIFY_ASSISTANT` 时中间轮文本另发同构 markdown 卡片（见配置节）；最终回复与错误回复同为终态（`finalSent` 互斥标记），任务结束时尚未收到任何终态才补「✅ 任务完成」兜底（结果全在子 Agent/画图输出中的任务在飞书侧也有完成信号；出错/取消后不另发完成提示）；工具调用发「🔧 正在执行」状态消息（同任务至多一条）。**消息保留策略：机器人发出的所有消息一律不撤回**（预览/状态/滚动提示/审批与选择卡片均留痕），仅最终回复引用原用户消息；唯一例外是「Typing」表情反应（非消息本体，输出完成后移除）
- **画图（show 图表分支）后端渲染**：飞书通道不再依赖前端渲染，由桥接**后端直接渲染成图片**——`event.draw.render` 到达后，桥接调用**四语言组合渲染器**（`core/support/diagram-render.ts`：plantuml = `@plantuml/core` TeaVM 引擎本地渲染 SVG（零网络，浏览器 DOM API 以极简 shim 垫层运行）、mermaid = mermaid + happy-dom 垫层（getBBox 几何估算覆盖）、d2 = `@terrastruct/d2` WASM（二进制模式内嵌产物物化到 `{GEBAI_HOME}/vendor/d2js/{version}/`）、echarts = npm 包 SSR 渲染（`ssr:true` + SVGRenderer 零 DOM）→ `@resvg/resvg-js` 栅格化 PNG，浅色主题白底图，单一串行队列防引擎/全局环境冲突，超长按上限等比缩放）→ PNG 落盘会话 `tmp/{name}.png`（与源码文件并列，Web UI 文件面板可见）→ 上传飞书图片（`im/v1/images`，multipart）→ 发送 `image` 消息 → 经 `decideDrawResult` 回传引擎（成功才返回成功；失败把渲染错误回传模型供修正源码；`EngineBotAdapter.onDraw` 透传 format 字段）；**该渲染器同样供 Web 通道 `show` 图表分支 `render=backend` 复用**（引擎经 ToolContext `renderDiagram` 惰性加载，落盘 `tmp/{name}.png` 并返回 `image` 内容块）
- **选择（ask 选项询问分支）交互卡片**：`event.choice.request` 到达后发送**交互式按钮卡片**（选项按钮每行至多 5 个，`value` 携带 `choiceId`+`act`+选项值）替代前端选择卡；按钮点击经卡片交互帧回传——单选立即 `decideChoice`，多选切换勾选（点击回包 `card` 字段更新卡片按钮态与「已选」提示），「✅ 完成选择」提交勾选集合、「❌ 拒绝回答/放弃」提交拒绝；**已决策的卡片经 ACK 响应更新为终态**（「✅ 已选择：X」/「❌ 已放弃回答」，按钮不可再点；`card` 字段为 `{"type":"raw","data":…}` 官方包装）；**仅任务发起者可作答**（与审批授权一致，防群聊成员越权，他人点击回 toast 拒绝）；任务结束（完成/错误）清除待作答状态（卡片保留留痕，状态清除后按钮回调不再决策）
- **接口层桥接（多轮交互 + 仅最终回复）**：飞书 bot 不直接接触 AgentEngine/EventBus（不侵入引擎层），经 `BotPromptAdapter` 接口运行——固定 `interactionMode: "multi_turn"` + `outputMode: "final_only"` + **通道环境注记 `channelNote`**（`FEISHU_CHANNEL_NOTE`：引擎 `run` 选项通道无关地注入系统提示词[主提示词]——模型据此感知「当前对话经飞书机器人通道进行」：回复以 Markdown 卡片渲染且超长会截断（大段产物建议落盘并引导 Web UI 查看）、审批/选择经卡片按钮作答且用户可能回复 /approve 等命令、依赖前端页面的工具本通道不可用、图片自动转附件而其余富媒体不支持）；引擎事件流由 `EngineBotAdapter` 映射为语义回调（onApproval/onChoice/onDraw/onDone/onError/onEnd），**过程事件（工具调用状态、文本增量、推理、子会话运行过程）不推送**，回复仅最终消息（无打字机预览）；**收到消息先给用户消息添加「Typing」表情反应模拟「正在输入」（飞书开放平台无 typing 接口——实测 `POST /im/v1/messages/{id}/typing` 与 `POST /im/v1/chats/{id}/input_status` 均 404；改用 Message Reaction API `POST /im/v1/messages/{message_id}/reactions`，body `{"reaction_type":{"emoji_type":"Typing"}}`——`emoji_type` 必须为[官方表情文案说明](https://open.feishu.cn/document/server-docs/im-v1/message-reaction/emojis-introduce)中的标准 ID（`Typing` 为敲键盘表情，传 emoji 字符会报 231001），响应 `data.reaction_id` 为唯一标识；权限 `im:message` 或 `im:message.reactions:write_only` 任一即可），输出完成（最终回复/出错/兜底）后移除该反应（`DELETE /im/v1/messages/{message_id}/reactions/{reaction_id}`，表情反应非消息本体，不受消息保留策略约束），不发送额外表情/状态消息；**最终回复（interactive 卡片）、错误回复与任务完成兜底提示均以「回复」形式引用原消息发送（`POST /im/v1/messages/{message_id}/reply`，客户端显示引用气泡；message_id 不合法时回落普通发送）**；依赖实时前端的工具（`page_capture`，声明 `interaction: "realtime"`）由引擎按交互模式自动从模型 schema 中移除（含子Agent 命名空间同名工具 `{agent}_{tool}`），被调用时阻止执行并返回「当前通道不可用」说明（见「交互模式」）；`ask`/`show` 全模式可见（ask 选择/计划分支经飞书选择卡片作答、填值分支明确报错；show html 分支明确报错、图表分支经飞书后端渲染出图）；关键操作（requiresApproval）经审批交互卡片询问用户（见「审批交互卡片」）
- **交互命令**：`/help`、`/new`（清空当前对话上下文重建会话——以会话所有者身份经存储层主动重建通道重建（旧目录连同 `tmp/` 附件一并删除），**仅会话创建者可操作**）、`/sessions`、`/cancel`（**仅任务发起者可操作**）、`/approve`、`/reject`（批准/拒绝最近一个待审批工具调用，**仅审批发起者可操作**；审批卡片按钮的主路径兜底——卡片不可用/按钮失效时以命令完成决策（卡片保留留痕））、`/approval-skip`（**仅会话创建者可开启**——群聊成员不得为共享会话开启免审批；写入会话内存态 env，不落盘、服务重启后需重新开启）
- **审批交互卡片**：Agent 审批请求（`event.approval.request`，载荷含**调用参数 `arguments`**——飞书通道无消息流工具卡片，参数须随审批展示供用户判断）推送**交互式审批卡片**：工具名 + **参数摘要**（`formatApprovalArgs`：每参数一行 `key: value`，长值截断[单值 200 字符]+总长兜底 1200 字符防撑爆卡片，提示完整参数可在 Web UI 查看）+ 重复请求提示（`retries>0` 时标注「已被拒绝/超时 N 次」）+「✅ 批准 / ❌ 拒绝」按钮（`value` 携带 `approvalId`+`act`，与选择卡片的 `choiceId` 区分路由）；按钮点击经卡片交互帧回传——**仅任务发起者可操作**（他人点击回 toast 拒绝，与选择卡片/命令授权一致），决策经 `decideApproval` 回传引擎，**ACK 响应更新卡片为终态**（「✅ 已批准，任务继续」绿头 /「❌ 已拒绝，任务已取消」灰头，按钮不可再点；`card` 字段同样为 `{"type":"raw","data":…}` 官方包装）；已决策后再点忽略（幂等）；卡片发送失败回落纯文本命令指引（工具名 + /approve /reject）；新审批覆盖旧待审批（超时残留）时仅替换状态映射（旧卡片保留留痕——按钮回调经映射校验，失效 toolCallId 不会误决策）；任务结束（完成/错误/`/new`）清除待审批状态（卡片保留）
- **富文本（卡片 JSON 2.0）**：全部飞书卡片（最终回复/选择/审批及终态更新卡）统一 **JSON 2.0 结构**（`schema:"2.0"` 显式声明 + `config.update_multi`（卡片多次更新，多选卡逐次回包依赖）+ `header` + `body:{elements}` 包裹，`cardV2` 骨架统一构建；回调更新卡必须与原卡同结构——1.0/2.0 混用报 200830）；**2.0 的 markdown 组件支持完整语法**（1–6 级标题、GFM 管道表格（仅 2.0 支持，单卡最多 4 个、正文最多展示 5 行超出分页）、代码块（60+ 语言高亮）、引用、分割线、删除线等），回复 Markdown **原样透传无需降级转换**——旧 JSON 1.0 `lark_md` 不支持标题与表格（`#`/`|` 原样显示），2.0 升级后消除；**交互按钮同规则升级**：1.0 `action` 组件在 2.0 已废弃（报 200861 unsupported tag action），改为 `column_set`（`flex_mode:"flow"` 流式布局、列宽 auto 按内容收缩）横排承载 + 按钮 `behaviors:[{type:"callback", value}]` 声明回传（`card.action.trigger` 的 `action.value` 原样回传 value，与 1.0 顶层 `value` 同语义；按钮 `type` 枚举 `primary`/`danger` 在 2.0 仍有效）；低版本客户端（<7.20）对 2.0 卡片显示兜底提示；长内容截断附 Web UI 会话提示（会话文件下载依赖 Web UI 访问）
- **配置**：`GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`（与 `feishu_docs` 子Agent 共用全局凭证）+ `GEBAI_FEISHU_BOT_ENABLED`；行为开关三变量（均默认 false 保持现状）：
  - `GEBAI_FEISHU_BOT_NOTIFY_TOOLS`——**工具调用过程推送**：每个工具调用一条消息——开始发「🔧 名字(参数摘要)…」，结束时原地更新为「✔ 名字(参数摘要)（耗时）」（PATCH，消息保留不撤回）；并行工具按 `toolCallId` 各自独立精确配对；适配层 `EngineBotAdapter` 构造注入 `notifyTools`，`event.tool.call`（含 `arguments`）/`event.tool.result`（不受 outputMode 限制、始终发布）转发为 `onToolCall`/`onToolResult` 回调，bot 侧 `ChatOutbox.toolStart`/`toolFinish` 按调用管理消息（参数摘要 `formatToolArgs`：`key="value"` 空格连接，单值 80/总长 200 字符截断）；原地更新经 `PATCH /im/v1/messages/{id}`（`api.patchMessage`，仅同类型可更新），无权限/失败回落结束时发新消息（均保留）
  - `GEBAI_FEISHU_BOT_NOTIFY_ASSISTANT`——**助手中间消息推送**：带工具调用的中间轮过程陈述/阶段结论发 markdown 卡片消息（与最终回复同构——同一 `buildReplyCard`（JSON 2.0 markdown 完整渲染/超长截断），普通发送不引用原消息，保留不撤回；开启后不再发「✍️ 预览」文本）；引擎新增 `run` 选项 `notifyIntermediate`（与 outputMode 正交——final_only 通道也可感知过程），中间轮文本在持久化后随 `event.message.intermediate` 发布（载荷含 `toolCalls` 本轮工具调用数，仅非空文本发布），适配层转发为 `onIntermediate` 回调，bot 侧 `ChatOutbox.assistantNote` 每轮直接发卡（无节流链路——中间轮为轮级事件非字符流）
  - `GEBAI_FEISHU_BOT_AUTO_APPROVE`——**自动审批工具**：需审批工具自动通过（审批卡片不再弹出）；适配层透传引擎任务级 `autoApprove: true`（`approvalPolicy=auto`，含服务模式——部署方为飞书通道整体担责，与映射用户自设审批跳过等价）；仅建议内网/低风险部署开启，公网部署保持审批卡片交互
- **飞书 TLS 策略**：`GEBAI_FEISHU_INSECURE_TLS=true` 时**所有飞书出站请求禁用 TLS 证书校验**（内网代理/中间人证书场景）——机器人桥接 REST 请求（token/消息/图片，`feishu-bot/api.ts`）、长连接 endpoint 发现与 WebSocket（`feishu-bot/conn.ts`，Bun WebSocket `tls.rejectUnauthorized=false`）、`feishu_docs` 子Agent 全部接口与 OAuth 兑换/用户信息（`feishu-bot/tls.ts` 共享 `feishuFetch`/`feishuWsOptions` 助手，注入 `fetch`/`WebSocket` 的 `tls.rejectUnauthorized=false`；仅在可信内网开启）
- **安全**：长连接为出站连接（不暴露回调端口）；凭证仅存环境变量（本地 `.env` 不入版本库）；服务模式按飞书用户隔离数据与权限（映射用户为普通角色）；消息/会话 id 白名单校验（`[A-Za-z0-9_-]{1,64}`）防路径注入；卡片按钮回调的 chatId/openId 同样白名单校验 + 发起者授权；**事件处理异常全程捕获**（async 回调 rejection 不得成为 unhandled rejection）；会话归属/映射用户缺失时中止而非兜底
- **实现**：`packages/server/src/feishu-bot/`（`pb.ts` protobuf 帧编解码、`protocol.ts` 帧协议/合包/ACK、`conn.ts` 长连接客户端（事件 + 卡片交互帧；判活/握手超时/快速重试断连韧性）、`api.ts` 开放平台 API（token 缓存/消息收发/图片上传下载/会话信息）；TLS 策略助手已迁 `@gebai/agents` core/shared/tls.ts（`GEBAI_FEISHU_INSECURE_TLS` 禁用证书校验，机器人/feishu_docs/OAuth 共用，server 侧经包导入）、`plantuml.ts` 后端渲染器（DOM shim + TeaVM 引擎 + resvg）、`bot.ts` 桥接编排（依赖全部注入））；单测 129 用例（真实凭证握手验证通过）；Webhook 回调模式（公网 HTTPS + 签名验签）为后续迭代

### 子Agent文件格式

**单文件形式**（`packages/agents/src/agents/{name}.ts`）：

```ts
export const def: SubAgentDef             // ★ 契约主体：加载器只认 mod.def（未导出即报「须 export const def: SubAgentDef」）
// SubAgentDef 字段：name / description / systemPrompt / tools / requiresApproval? / preload? /
//   dependencies? / envVars? / writeGuard?（工具结构化输出契约写在各 Tool 上：Tool.outputSchema，JSON Schema）
export const name: string                 // 可选：便捷导出
export const description: string
export const systemPrompt: string
export const tools: ToolSet               // 子Agent 自有工具（注册为 {agent}_{tool}）
export const requiresApproval?: Record<string, boolean>
export const preload?: boolean            // 是否预加载（默认 false，按需装载）
export const dependencies?: string[]     // 依赖的其他子Agent 名单（装载/预加载/subsession_run 自动连带装载，
                                         // 工具与提示词按依赖方自身命名空间复用，见「子Agent 依赖与自动装载」）
// 工具级安全模式自主声明（Tool.safeMode，写在各工具定义上）：
//   true  = 作者判定安全模式下可提供（即使短名风险如 xxx_sh——须自行保证实现只读或体内按 ctx.safeMode 校验）
//   false = 作者判定安全模式下不提供（即使名字无风险命中，如内部写文件/外发请求的工具）
//   未声明 = 按短名风险规则默认（{agent}_sh / {agent}_write / {agent}_file / {agent}_delete / {agent}_task_add 等 _risk 后缀命中则不注册）
// 注册期过滤（ToolRegistry({safeMode})）：不注册即 schema 不可见、调用报未知工具，见「安全模型 → 安全模式」
export const envVars?: EnvCatalogVar[]    // 可配置环境变量声明（{AGENT_NAME_UPPER}_ 前缀），汇总进环境变量目录（前端面板白名单，见「环境变量配置」）
export const writeGuard?: (env: Record<string, string>, absPaths: string[]) => string | null
// 写范围守卫声明：会话装载本子Agent（或子会话预加载）后注入 ToolContext.writeGuard——
// 文件写类工具（write/edit/file（rename/move/delete））写入前以解析后的绝对路径调用，
// 返回非空字符串 = 拒绝写入（self_optimize 用它实现「核心引擎源码默认只读」的代码级强制）
```

**目录形式**（`packages/agents/src/agents/{name}/{name}.ts` + 可选 `{name}.md`）：

定义入口按优先级探测：`{name}/{name}.ts` → `{name}/index.ts` → 纯 md。双入口共存时 `{name}.ts` 优先，`index.ts` 静默忽略（不报错）——优先级规则由测试锁定（subagents.test.ts）。

```ts
import systemPromptBase from "./{name}.md"   // Bun 原生 .md 文本导入（bunfig.toml 统一为 text loader）

export const systemPrompt = systemPromptBase
  .replaceAll("{{PLACEHOLDER}}", "替换值")   // 修饰一：占位替换
  .concat("\n补充内容……")                     // 修饰二：内容补充（md 之外追加动态说明）
```

- 目录形式下定义入口固定为 `{dir}/{dir}.ts`，系统提示词可拆为同目录 `{name}.md` 独立维护；ts 文件 import 后自由修饰（占位替换 `{{...}}`、内容补充、条件拼接），修饰完全由 ts 控制，加载器不干预
- 目录内其他辅助文件（如 `tools.ts` 工具集）由入口文件自行 import，仅入口文件被扫描收录
- `description`：总Agent 根据描述判断何时调用该子Agent，是子Agent 能力被发现的关键
- `preload`：声明该子Agent 默认预加载（模块语义，属「装载」的启动期形态：工具与能力描述立即注入总Agent）；未声明者通过 `agent_load` 按需装载，也可用 `GEBAI_PRELOAD_SUB_AGENTS` 统一声明
- 工具的结构化输出契约写在各 `Tool` 上（`Tool.outputSchema?: ToolSchema`，JSON Schema），经 `tool_schemas` 工具批量暴露——**无 `toolSchemas()` 导出、无 `returns` 字段**
- `subsession_run` 派生子会话：输入为 `agents`（子Agent 列表）+ `input`（任务文本），返回为 `output`（文本），隔离子会话（预加载子Agent）自行规划执行（默认阻塞执行返回；`async:true` 后台异步执行，`bg_task` 管理，见「子会话运行」）；继承形态则 fork 父上下文并把报告自动合入父会话
- 子Agent 内无全局可变量，工具函数体为纯函数或依赖注入工厂，天然支持并发加载
 - **复用其他子Agent 的能力用 `dependencies` 声明**（装载/预加载自动连带，工具以依赖方 `{dep}_` 命名空间注册——如 reverse_site 依赖 playwright、self_optimize 依赖 code/vision、caiyun 依赖 shuangtian/imagegen/tts、louyue 依赖 shuangtian/imagegen），不在 `tools` 中展开依赖方的工具实现（旧形态会产生重复注册与审批映射复刻）；**命名前缀是硬约束**：子Agent 名不得与被依赖名互为前缀（否则注册期抛 prefix collision，只能退到内联），如某子代理名以 `shuangtian_` 开头、它又要依赖 `shuangtian`，两者就不兼容——详见 `02b-builtin-agents.md`「工具面的两种装配方式」；机制上 `tools` 仍可直接引用全局工具实现（如 `read`/`write`/`sh`，以 `{agent}_` 前缀暴露），但按「子Agent 只声明独有工具」约定应直接用全局名，不重复声明
- 示例：内置 `desktop` 即目录形式（`packages/agents/src/agents/desktop/desktop.ts` + `desktop_tools.ts`）

**纯提示词简化定义**（`packages/agents/src/agents/{name}/{name}.md` 单独存在，零 TS）：用于简单子Agent 与组合式子Agent

```md
---
description: 一句话能力描述（可选；缺省取正文首行）
dependencies: playwright, code（可选；依赖的子Agent 名，逗号分隔——装载/预加载自动连带装载）
preload: true（可选；启动即装载）
env_vars:（可选；可配置环境变量声明，变量名须以 {NAME 大写}_ 前缀，汇总进前端环境变量面板）
  - name: MY_AGENT_TOKEN
    description: 访问令牌
---
系统提示词正文（说明编排策略/角色职责等）
```

- 目录内无同名 `{name}.ts` 时，加载器直接由 md 构成定义（`name`=目录名）；frontmatter 识别 `description`/`dependencies`/`preload`/`env_vars`（条目须符合命名规则 `[a-z0-9_]+`，非法条目过滤；`env_vars` 的变量名须以 `{NAME 大写}_` 前缀，非前缀条目忽略）——组合式子Agent 声明依赖后编排提示词可直接引用依赖方 `{dep}_` 工具，无需运行时先 `agent_load`；`preload`/`env_vars` 使零 TS 定义具备与 TS 目录形式同等的启动期/面板能力
- 无工具的简化定义在子会话运行环境**自动注入编排工具**（`agent_list`/`agent_load`/`subsession_run`/`bg_task`，原名暴露、无 `{agent}_` 前缀），支持组合式子Agent 编排装载/派生子会话其他子Agent（受递归深度 3 层限制）
- 有工具的子Agent 需要编排能力时，可显式引用导出的 `agentListTool`/`agentLoadTool`/`subSessionRunTool`/`subSessionMergeTool`/`bgTaskTool`（`core/tools/` barrel），注册为 `{agent}_subsession_run` 等带前缀形态
- 示例：纯 md 组合子Agent 即 `packages/agents/src/agents/{name}/{name}.md`（零 TS），编排其他子Agent 产出完整链路的组合能力可由此模式定义

## 桌面端架构

基于 **tao + wry（系统原生 WebView：Windows WebView2 / macOS WKWebView / Linux WebKitGTK）** 实现桌面应用，复用服务端内置 Web UI，无需额外前端框架、无 Tauri 框架依赖。产物为两个独立可分发的 exe：

- **`dist/gebai.exe`（纯 Bun 单文件，浏览器形态）**：`bun build --compile` 产物（入口 `src/index.ts`），同进程启动服务端并自动打开系统默认浏览器（`GEBAI_NO_OPEN=1` 时不打开）；Web UI、子Agent、D2.js、playwright driver 与 playwright-core 等全部内嵌，单文件即可运行，也可作为服务端直接部署（`--server`）
- **`dist/gebai-desktop.exe`（原生 WebView 启动器）**：Rust 单文件（`launcher/src/main.rs`，约 230 行），构建时经 `include_bytes!` **内嵌整个服务端二进制**；启动时物化到用户数据目录（Windows `%LOCALAPPDATA%\{app}\gebai-server.exe`——**长度先行短路，长度相同再比 SipHash**，一致即跳过重写）并 spawn（`GEBAI_NO_OPEN=1` + `CREATE_NO_WINDOW`），从侧车 stdout 解析监听端口（`[gebai] listening on http://HOST:PORT`），WebView 导航到 `http://127.0.0.1:{port}`；窗口关闭（`CloseRequested`/`LoopDestroyed`）时一并 kill 侧车；WebView 用户数据目录固定在 `%LOCALAPPDATA%\{app}\webview\`（不在 exe 旁落地）；WebView 经初始化脚本注入 `window.__GEBAI_DESKTOP__` 桌面形态标记，Web UI（`state.isDesktopApp`）据此在下载（文件卡/图表图片/HTML 源码/会话导出）时以 toast 提示进度位置与保存位置——WebView 内下载指示不明显，浏览器形态有自身下载指示故不提示。**场景变体参数化**（`launcher/build.rs` 构建期环境变量注入，`rustc-env` 烘焙进 `main.rs`；缺省值 = 完整桌面端，行为不变）：`GEBAI_LAUNCHER_SERVER_EXE`（内嵌的服务端二进制路径，缺省 `dist/gebai.exe`）、`GEBAI_LAUNCHER_APP_NAME`（应用标识——物化数据目录名/非 Windows 同目录服务端文件名/WebView 配置目录，缺省 `gebai`）、`GEBAI_LAUNCHER_TITLE`（窗口标题与 exe 资源 ProductName，缺省「歌白」）、`GEBAI_LAUNCHER_PORT`（可选固定端口：spawn 时设 `GEBAI_PORT`，变体与完整桌面端各占独立端口互不冲突且 localStorage origin 各自稳定，缺省不指定）
- **单文件自带 UI**：构建时 `packages/server/scripts/build-web-bundle.ts` 把 `packages/web/dist` 全量资源 base64 内嵌为 `web.bundle.generated.ts`（gitignore），随服务端编译进二进制；二进制模式（`webDist` 不存在）静态路由改从内嵌资源提供，`/` 与 `/assets/*` 均可访问——单文件二进制完整可用（含 Web UI + 全部子Agent）；脚本调试模式该文件缺失时服务端自动回退（不内嵌 UI），无需预先构建
- 桌面端与浏览器访问共用同一套 Web UI、同一套服务端核心，仅宿主不同
- **端口固定（桌面形态）**：桌面形态（`gebai-desktop.exe` 侧车与 `gebai.exe` 浏览器形态）默认固定端口 **47896**（`DESKTOP_PORT`，不易冲突——避开常见开发端口与 Windows 临时端口段 49152+）。核心目的是保持 origin（协议+主机+端口）稳定：浏览器/WebView 的 localStorage 按源隔离，端口每次随机变化会使环境变量（`gebai.ui.env`）、主题、快捷键等全部浏览器本地数据跨重启「凭空丢失」（用户环境变量仅存浏览器本地，服务端零留存，无兜底可恢复，见「环境变量隔离」）。端口被占（如重复启动）**直接报错退出，不回退随机端口**——随机 origin 正是数据丢失的根因；显式 `GEBAI_PORT` 或调用方 overrides 优先。服务端部署形态仍默认 3000
- 数据目录 `GEBAI_HOME` 默认 `~/.gebai/`；侧车物化采用**内容哈希比对**（长度相同再比 SipHash，防同尺寸不同内容的陈旧残留）；两种桌面形态（浏览器形态 exe 与启动器侧车）同为二进制模式，启动时加载 `{GEBAI_HOME}/.env` 作为启动配置（真实环境变量优先，见「启动参数与环境变量」）——模型密钥等配置放 `~/.gebai/.env` 即可生效，无需设置系统环境变量

### 构建与产物

- 构建链：`bun run --cwd packages/desktop build` → `server:build`（图标生成 → web 构建 → web bundle → 子Agent bundle → D2.js 内嵌 → driver 内嵌 → playwright-core 内嵌 → CV 运行时与模型内嵌 → CV sidecar 驱动内嵌 → `--compile` 产出 `dist/gebai.exe`）→ `launcher:build`（cargo release 编译 `launcher/` 并复制为 `dist/gebai-desktop.exe`）
- **品牌图标管线**：canonical 源为 `packages/web/public/favicon.svg`（大脑造型）；`packages/desktop/scripts/gen-icon.ts` 用 resvg 渲染多尺寸打包为 `icons/icon.ico`（gitignore，16/24/32/48/64/128 无压缩 BMP + 256 PNG），并回写 `packages/web/index.html` 的内联 favicon data URI。exe 嵌入**不走 bun `--windows-icon`**（其会改动像素/alpha，与 rcedit 同样不可用）：`gebai.exe` 由 `packages/desktop/scripts/embed-icon.ts` 经 kernel32 `UpdateResource`（bun:ffi）原样写入 RT_GROUP_ICON+RT_ICON 资源（32px 条目排首位，兼容简化提取器）；`gebai-desktop.exe` 经 `winresource`（`launcher/build.rs`，`rerun-if-changed` 聟动重建）——网页 favicon 与两个 exe 图标同源。**运行时窗口图标**（任务栏/标题栏）不属于 exe 资源，需窗口类显式设置：gen-icon 同时产出 `icons/icon32.rgba`（32px 原始 RGBA），launcher `include_bytes` 内嵌后经 tao `with_window_icon` 挂到窗口
- 产物：`packages/desktop/dist/gebai.exe` 与 `packages/desktop/dist/gebai-desktop.exe`（`launcher:build` 依赖 `dist/gebai.exe` 已存在，故构建顺序固定）
- 要求：Rust 工具链（仅启动器需要）、WebView2 运行时（Windows 自带）；`gebai.exe` 单独分发时无需任何 Rust 依赖
- 浏览器桥接驱动 `@gebai/agents` core/browser/driver.mjs（playwright/reverse_site 子Agent 与透明浏览器代理共用）：服务端 dist（非编译）形态由 `packages/server/scripts/build-subagents.ts` 复制到 dist/ 与入口同目录；二进制形态由 `packages/server/scripts/build-driver-embed.ts` 生成内嵌产物 `driver.embedded.generated.json`（gzip base64），运行时物化到 `{GEBAI_HOME}/vendor/playwright/driver.mjs`（桥接仍需运行机器具备 node；playwright 模块源码/部署形态解析 node_modules，二进制形态用内嵌 pwcore 产物）
- 本地 CV 运行时与模型 `@gebai/agents` core/cv/cv.embedded.generated.json（desktop 子Agent 小模型识别）：`packages/server/scripts/build-cv-embed.ts` 把 onnxruntime-web dist 两文件（入口 mjs + wasm 本体）与 PP-OCR 模型三件套（det/rec ONNX + 字典，构建时从 `GEBAI_CV_MODEL_BASE` 下载或取 `{GEBAI_HOME}/resources/models/cv/ocr` 已有自备）整包 gzip base64 内嵌，运行时释放到资源目录——模型 → `{GEBAI_HOME}/resources/models/cv/ocr/`、ort 运行时 → `{GEBAI_HOME}/resources/vendor/cv/`（单二进制形态下 ort 与模型均不可依赖 node_modules，必须内嵌；源码/部署形态 ort 解析 node_modules、模型走 `{GEBAI_HOME}/resources/models/cv/ocr` 或 `GEBAI_CV_MODELS_DIR`；下载失败生成空清单不阻断构建，运行时给配置指引）；**模型资产一律不入 packages 源码树**（集中放资源子仓库 `resources/`，获取方式：`bun run resources:download` 按清单 `scripts/resources.manifest.json` 拉取，或克隆资源子仓库）
- CV GPU sidecar 驱动 `@gebai/agents` core/cv/cv-driver.mjs（检测分层后端，见「小模型识别」）：`packages/server/scripts/build-cvdriver-embed.ts` 生成内嵌产物 `cvdriver.embedded.generated.json`，运行时释放到 `{GEBAI_HOME}/resources/vendor/cv/cv-driver.mjs`；dist 形态由 build-subagents 复制到产物目录。驱动脚本本身仅数 KB 随构建内嵌；**onnxruntime-node 依赖不内嵌**（体积/许可）——运行机安装该依赖或设 `GEBAI_CV_ORT_NODE_DIR` 指向其目录即启用 GPU 推理，缺省回落 wasm（保留现有方案兜底）；sidecar 需运行机具备 node（与 playwright 桥接同前提）

### 启动方式

- 浏览器形态：直接运行 `gebai.exe`，自动打开系统浏览器访问本地服务
- 桌面形态：运行 `gebai-desktop.exe`，启动器物化并拉起内嵌服务端，原生 WebView 窗口加载内置 UI；核心逻辑与浏览器/服务端部署完全一致，仅宿主不同

- 关闭窗口时进程退出，内嵌侧车服务端一并退出（除非以服务端模式单独运行）

## 构建与发布

### 最终构建目标

单二进制可执行文件，通过 `bun build --compile` 产出，支持 Windows / Linux / macOS 三平台。同一二进制承载全部运行形态，分发物仅一个文件，**服务本体零运行时依赖**（Bun 运行时已内嵌，宿主机无需安装 bun/node/python 即可运行服务本身；脚本工具的解释器要求见「脚本执行环境」，playwright 桥接与 CV 边车需宿主 node）：

- 本地桌面应用（WebView 宿主）
- 本地浏览器访问（服务端 + 浏览器）
- 服务模式（多用户公用）

Vite 构建产物（Web UI）、桌面端 WebView 宿主、子Agent 代码一并编译进二进制，运行时按启动参数/环境变量切换形态，无需区分构建，也无需在运行时读取子Agent 文件。

构建时支持**选择性打包**，不同发行规格（桌面全量版、服务端精简版、领域专用镜像）由同一源码产出：

```
GEBAI_BUILD_SUBAGENTS=a,b bun run --cwd packages/server build      # 仅打包指定子Agent（依赖闭包自动展开）
GEBAI_BUILD_EXCLUDE_SUBAGENTS=x,y bun run …                       # 排除指定子Agent（与包含清单互斥）
GEBAI_BUILD_PRELOAD=a bun run …                                   # 烘焙 def.preload=true
GEBAI_BUILD_EXCLUDE_TOOLS=show,fetch_url bun run …                # 全局工具不注册不暴露
bun run --cwd packages/server build                              # 全量（缺省）
# 注：选择性打包是**构建期环境变量**，无 --sub-agents/--exclude-sub-agents CLI；
#     运行时黑名单用 GEBAI_SUB_AGENTS_ENABLE/DISABLE（见「子Agent 启停名单」）
```

排除清单同样校验依赖完整性：保留者依赖了被排除的子Agent 时**构建直接失败**（要求把依赖方一并排除），
不静默产出残缺产物。内嵌资产也可逐项裁剪（`GEBAI_BUILD_WEB_UI`/`CV`/`D2`/`ANALYZER`/`BROWSER`/`RG=0`
时对应脚本写**空产物**，运行期按「本构建未内嵌 X」如实降级或走等价回退）；资产被源码静态 import，
故一律写空产物而非不生成文件。前端 vendor 引擎组（monaco/plantuml/mermaid/echarts/d2js/xterm/
tree_sitter）由 `GEBAI_WEB_VENDOR` 控制，未包含组会被删除，前端各引擎懒加载失败即降级。

### 运行模式区分

代码区分运行模式以 **`package.json` 是否存在**判定二进制形态（`isBinaryMode()`），并据此定 `GEBAI_HOME`（二进制 → `~/.gebai/`，源码 → 仓库根）：

| 模式 | 判定方式 | GEBAI_HOME | 子Agent 来源 |
|------|---------|------------|-------------|
| 脚本调试 (`bun run dev:*`) | `Bun.argv` 含源码路径 | 项目根目录 | `packages/agents/src/agents/` 源码直接引用 |
| 二进制运行 | 无源码路径，编译产物 | `~/.gebai/` | 编译时打包进二进制 |

二进制运行形态（本地模式 / 服务模式）由启动参数与 `GEBAI_MODE` 等环境变量决定，不影响二进制构建。

### 容器镜像（服务模式）

以基础镜像（缺省 Ubuntu 24.04，可经 `image.base` 换成自备/内网镜像）的多阶段构建（`Dockerfile` + `docker/build.sh`/`build.ps1`，用法与边界见 `docker/README.md`）：构建阶段装依赖并跑完整构建链（Web UI 产物、子Agent/工具注册表、D2.js、tree-sitter wasm、playwright 驱动与 pwcore、内置 ripgrep、可选本地 CV），再 `bun build --compile` 产出单文件 Linux 可执行；运行阶段只有系统库加该二进制，**不含 node_modules、不含 bun**。镜像默认 `GEBAI_MODE=server` + 非 root 运行用户（uid/gid 1000 `gebai`）+ 数据根 `/data`（挂卷），因此路径沙箱与会话目录脚本隔离均强制开启。系统依赖按裁剪计划安装：tini（PID 1 收尸）/ curl（健康探针）/ ca-certificates 为固定基础设施，其余分组可裁——git（git 工具与文件工作台面板）/ python3（py 工具、vision_pip）/ bubblewrap（隔离的文件系统层）/ fonts-noto-cjk（PDF 与图表中文）/ tzdata（定时任务与容器时区）/ procps / chromium（浏览器类子Agent）。

- **裁剪与镜像本体定制**：镜像构建支持完整裁剪与定制——**能力层**（子Agent 包含/排除/预加载、全局工具、内嵌资产）、**前端资源层**（vendor 引擎组）、**系统层**（运行期 apt 包组）、**镜像本体**（基础镜像、运行用户与 uid/gid、数据根、端口、时区、额外 apt 包、apt/npm 源与构建代理、镜像标签、健康检查）。入口是 `docker/build.sh --profile <名|路径>`（档案 `docker/profiles/*.json`，预置 `full` / `code` / `minimal` / `intranet`）与字段级覆盖 `--set 键=值` / 对应快捷开关（CLI 优先于档案）。档案 schema 是运行期 `GEBAI_PROFILE`（见「启动裁剪与领域专用模式」）的**超集**：`assets`/`web`/`system`/`image` 四个构建段运行期不解释也不报错，因此同一份领域档案可从构建贯穿到运行，构建面与运行面不会漂移。
- **两条落地路径**（由 `scripts/build-image-plan.ts` 同一份档案算出）：**构建步骤级**（能力裁剪、系统包组、额外包、apt/npm 源）→ 计划文件 `/tmp/gebai-plan.env`，构建阶段 `source` 后驱动各内嵌产物脚本、vendor 拷贝与装包；**指令级**（基础镜像、USER/VOLUME/EXPOSE/ENV/LABEL/HEALTHCHECK）→ build-arg（Docker 指令无法在构建过程中条件化，故由 `build.sh` 在宿主侧先算好传入；缺 bun 时降级为「仅 RUN 级裁剪生效」并告警）。裁剪报告打进构建日志，计划同时留在镜像内 `/etc/gebai/build-plan.env`（裁了什么、定成什么口径随时可查）。
- **两处 Docker 限制的如实口径**（不是本镜像的取舍）：① `HEALTHCHECK` 的 interval/timeout/start-period/retries 在 Dockerfile 解析阶段固定、不接受变量展开（写 `${VAR}` 报 `invalid duration`），镜像用 30s/5s/20s/3，需要调整请在运行期给（compose `healthcheck` 段或 `docker run --health-*`）；探针为独立脚本 `docker/healthcheck.sh`（运行期读 `GEBAI_PORT`/`GEBAI_HEALTHCHECK_PATH`，因此跟随端口与探针路径），`image.healthcheck=false` 时改选 `--target runtime-nohealthcheck` 阶段构建。② 额外镜像标签不能用 `LABEL ... ${VAR}` 追加（会被当键值对解析而报 `can't find =`），改由宿主侧转成 `docker build --label`。
- **健康检查禁用走阶段选择**：镜像默认（最后定义的）阶段带 `HEALTHCHECK`，`--target runtime-nohealthcheck` 得到不带探针的镜像（供用自身探针的编排）。
- **离线供给（浏览器 / bun / node）**：浏览器类子Agent 需要三层，每层都可零网络供给——① 浏览器本体：`browser_source=local` + 预置目录（`docker/browsers/`）；② **node 运行时**：浏览器桥接是 `Bun.spawn(["node", driver])`（`core/browser/bridge.ts`），没有 node 就没有浏览器能力，`node_source` 取 `image`/`local`（预置 `docker/node/node`）/`apt`，缺省 `auto`（启用浏览器则取 node 镜像，否则不装）；③ 浏览器系统依赖：`browser_deps=<清单路径>` + `--apt-mirror`，或基础镜像已含依赖时 `--no-browser-deps`。构建必需的 bun 同样可预置（`bun_source=local`）。三个预置目录（`docker/browsers`、`docker/bun`、`docker/node`）在构建上下文里，**放入即自动识别**；导出侧由 `docker/build.sh --export-browsers` / `--export-runtimes` 从已构建镜像或宿主产出（镜像内保留构建时实测的依赖清单 `/etc/gebai/playwright-deps.txt`）。`FROM` 无法条件化，故离线时 `BUN_IMAGE`/`NODE_IMAGE` 退化为 `BASE_IMAGE` 占位，运行时由预置文件提供；run 阶段装好 node 后跑一次 `node --version` 自检（缺库直接构建失败，不带到运行期）。边界：依赖包本身（`bun install`）仍需 npm 源（`--npm-registry`），不在本通道范围内。
- 可裁剪/可定制矩阵、档案字段与逐项降级口径见 `docker/README.md`「裁剪与镜像本体定制」。

- **架构限制**：二进制内嵌的 `@resvg/resvg-js` 是平台原生模块，跨架构编译会嵌错平台——镜像仅支持「构建机架构 = 目标架构」。
- **脚本文件系统隔离（bubblewrap）的容器前提已实测**：默认 seccomp 下容器内无法创建 user namespace（`unshare: Operation not permitted`），bwrap 不可用——此时自动降级为环境收敛（HOME/TEMP/XDG 仍在会话目录内）；`--cap-add SYS_ADMIN` **不够**（unshare 可用但 bwrap 卡在 `pivot_root: Operation not permitted`）；`--security-opt seccomp=unconfined` 实测可用（系统只读、仅会话目录可写、宿主家目录不可见）。
 - **能力边界**：`desktop`（宿主桌面操控）在服务模式一律不可用；`tts_speak` 的**系统语音档**仅 Windows（神经档需 Python 侧车 + 模型，不受平台限制）；客卿子Agent 在服务模式整体禁用；`shuangtian`/`caiyun`/`louyue` 需 C++ 工具链与霜天框架源码（未预装）；浏览器类子Agent 需 `--with-browser` 构建（且需容器内 node，见上方「离线供给」）；容器内重启用 `docker restart`（而非 `restart_server` 工具）。

架构与体积：`linux/amd64` 全量构建实测镜像 867MB（二进制 244MB），构建耗时主要在前端构建与依赖安装（首次约数分钟，缓存后秒级）；裁剪档案按维度削减同一份产物（`minimal` 档案同时裁掉 Web UI 内嵌产物、本地 CV、后端 D2、语法集、playwright-core、内置 ripgrep 与多个系统包组）。

### 升级与兼容

- **原地升级**：新版本二进制直接替换旧文件重启即可，数据（`GEBAI_HOME`）与配置（环境变量）无需迁移
- **数据兼容**：**存储格式未设版本字段**——兼容靠一次性迁移（`users/default/` → `users/admin/` 目录搬迁、飞书 `default` 归属改写）与「新字段可选、旧数据缺省」的读侧容错
- **API 兼容**：`/api/v1` 语义化版本，破坏性变更升级主版本号；WebSocket 消息类型保持向后兼容（新增类型不影响旧客户端）
- **多实例共处**：单机可并排部署多实例（不同 `GEBAI_PORT` / `GEBAI_HOME`），便于灰度与多租户；容器形态同理，但同一数据卷只应有一个实例（调度主实例锁在同一数据根上互斥）

## 实施路线

按依赖顺序分阶段交付，每阶段可独立验证：

**阶段一：核心闭环（MVP）**
- 服务端骨架：Hono + WS/REST 单端口 + `SessionStore`（分片）+ `EnvManager`（全局 + 会话内存态两层合并）
- `LLMProvider`（OpenAI 兼容接口 + 流式解析 + 能力声明）与 `AgentEngine` 主循环
- 全局工具：`read`/`write`/`sh`/`py`/`flow`/`current_time`/`system_info`
- 审批流 + 工具执行/渲染 + 上下文截断保护
- 会话 CRUD + `session.prompt` 流式 + SDK 基础 + 最小 Web UI（聊天/会话列表）

**阶段二：子Agent 与自我优化**
- 子Agent 装载器（定义扫描/命名空间与前缀解析/碰撞校验/选择性预加载）
- `code` / `self_optimize` + 待办跟踪 + 反馈

**阶段三：多用户与安全**
- 认证/令牌/用户管理 + 路径沙箱 + 限流 + 多模态附件

**阶段四：集成与分发**
- 单二进制构建（Web UI + 子Agent 打包）+ 桌面端 WebView + 反向代理子路径挂载（前端按页面 URL 相对解析，免配置）
- 多套 UI 风格 + 上下文自动压缩 + 飞书机器人 + Webhook

**阶段五：完善**
- 数据生命周期/GC、升级兼容、自我优化产物管理、多实例

### 待实现（已知项，后续迭代）

| 待实现项 | 现状与影响 | 计划方案 |
|---------|-----------|----------|
| 服务端消息分页 | 会话消息**全量加载**（`session.get` 返回全部正文），前端已做 **DOM 窗口化**：DOM 只保留视口附近的块（见「消息区窗口化」），布局/渲染开销不随历史条数增长——未做的是**传输侧**分页，长会话打开仍一次性传输完整正文（网络与解析成本随历史增长；已渲染块容器的 JS 引用也常驻，用于重挂零重建） | ① `session.get` 支持窗口/游标参数 + 前端上滚按需拉取更早页，替代一次性返回全部正文（窗口化已具备按需挂载路径，接上「未加载区间」即可）；② 极端长会话的已渲染节点 LRU 释放（需重建渲染 + 运行态引用迁移） |

## 测试策略

统一使用 `bun test`，测试文件与被测代码同目录（`*.test.ts`）。

```bash
bun run --cwd packages/server test
bun run --cwd packages/sdk test
```

**变异台（开发期复核工具，一条命令）**：`bun run mutate:status`——把「改坏源码 → 跑测试 → 还原」的变异验证固化成可重跑的命令（约 30 秒，退出码 0 = 全部符合期望），跑成功会刷新证据指纹 `tools/mutation/status-core/stamp.json`。它**不必记得跑**：常规测试里的 `files/status-mutation-gate.test.ts` 每次都用当前源码对账这份指纹，改了判据面却忘了重跑变异台就当场红。定位、判据与工程纪律见同章节下文「变异验证 → 怎么跑变异台」与「防忘闸门」。

### 测试执行速度

全套测试是 AI 编码迭代与 `self_optimize` 优化闭环的准入凭证，执行速度直接影响迭代效率，三项结构化保障：

- **server 分片并行**：`packages/server` 的 `"test"` 脚本走 `packages/server/scripts/test-parallel.ts`——全部 `*.test.ts` 按**文件字节数降序的最长处理时间优先（LPT）装箱**分成 N 个分片并行启动 `bun test` 子进程（bun test 单进程内测试文件串行执行，是全套件耗时主因；测试耗时与文件规模强相关，轮转分配会把大文件堆在少数分片形成长尾）。默认 `min(8, max(2, CPU 核数))` 分片，`--shards=N` 参数或 `GEBAI_TEST_SHARDS` 环境变量覆盖；带文件路径/`-t`/`--coverage` 等参数时自动退回单进程透传（定向运行分片无收益）。输出逐行加 `[i/N]` 前缀流式透传。`test:serial` 保留串行入口
- **失败复验（区分并行抖动与真失败）**：任一分片失败时，该分片文件列表**单进程串行重跑**——重跑通过判定为并行抖动（跨分片共享资源竞态，机器满载下真实 spawn 类用例也会触达用例超时），以 `⚠` 警告列出并给出精确复现命令、整体视为通过；重跑同样失败才是真失败（非零退出）。避免把环境/竞态抖动误当代码回归反复排查
- **超时余量**：分片子进程统一带 `--timeout`（默认 15000ms，`GEBAI_TEST_TIMEOUT` 可覆盖）。bun 默认 5s，而 `spawn`/真实网络/WS 往返类用例在**并行满载**下会显著变慢（实测同一用例并行触顶 5s、串行 1.8s）——余量不足时每次全量都带一条「并行抖动」噪声告警，噪声多了就会掩盖真回归
- **测试进程环境净化**：`bunfig.toml` 的 `[test] preload` 挂载 `packages/server/scripts/test-preload.ts`，启动时清除全部 `GEBAI_`/`CODE_` 前缀变量——Bun 在 `bun test` 时按 cwd 自动加载 `.env`（仓库根 `.env` 的 `GEBAI_APPROVAL_SKIP`/`GEBAI_SELF_MODIFY`/`GEBAI_LLM_*` 会直接改变引擎审批、写守卫、Provider 解析等断言结果），宿主 shell 亦可能残留调试变量；配套 `loadConfig` 的 `loadDotEnv` 在 `NODE_ENV === "test"` 时跳过读仓库 `.env`（防用例内首次 loadConfig 重新注入）。测试结果不依赖开发者本地配置
- **跨平台用例约定**：涉及平台分支的用例显式注入平台参数（如 `restart_server` 的 `platform: "win32"`），不随宿主平台漂移；安全判定对 Windows 形态绝对路径（盘符/UNC）在非 win32 平台一律按越界拒绝（fail-closed）
- **turbo `test` 不依赖 `^build`**：各包 `main` 均指向 `src/*.ts`（bun 直接执行 TS 源码），测试无需先构建依赖包——冷缓存/改动后跑测试不再先付 vite 构建 + wasm 内嵌 + `bun build --compile` 的构建成本
- **turbo 缓存键必须覆盖跨包源码**：`build` 与 `test` 的 `inputs` 显式声明 `$TURBO_DEFAULT$` + `../../keqing/**`、`../../custom/**`、`../../packages/*/src/**`。为什么必须声明：构建脚本（`build-subagents`）在**构建期**读取 `packages/agents/src/agents`、`custom/agents`、`keqing/`（子Agent 定义被 bundle 内联、客卿源整树拷进 `dist/keqing/`），测试也直接读 `keqing/`（客卿驱动用例）；不声明时只改这些目录不会改变 hash，于是 turbo 拿缓存**回放旧日志**——`dist/keqing/` 停留在旧内容，更危险的是 `test` 会回放一个**陈旧的 PASS**（实测：改 `keqing/python/vision/tools.py` 后构建报 `cache hit, replaying logs`，`dist` 与源不一致；而 `test` 也命中缓存未真跑）。只按文件字节数无法发现这类问题（输入集里没有这些路径）。
- **缓存命中即回放，不是“跑过了”**：`bun run test` 经 turbo，命中时 650ms 返回并重放历史输出；要确信真跑用 `bun run --cwd <pkg> test` 或 `turbo run test --force`。
- **构建产物幂等（内容不变不写盘 + 跨平台确定性）**：`packages/server/scripts/build-*.ts` 统一经 `write-if-changed.ts` 落盘——内容与磁盘一致则跳过写入，`typecheck`/`build` 反复重跑不再刷新已提交生成产物的 mtime 与字节（此前跑一次 typecheck 工作区就变脏，AI 每轮都要先确认「是否既有改动」）；内嵌产物 gzip 经 `gzip-deterministic.ts` 固定头部 XFL/OS 字节（zlib 默认随平台变化，同一源码在 Windows/Linux 生成不同字节）；`src/types/generated-json.d.ts` 通配声明 `*.generated.json`（`@gebai/server` 与 `@gebai/agents` 各一份），构建产物缺失时 tsc 不再报 TS2307（运行时仍走各模块「缺失→回退/引导」分支）
- **子Agent 发现进程级缓存（目录签名校验）**：`SubAgentManager.discover()` 首次扫描 `packages/agents/src/agents/` 后缓存定义列表与**目录签名**（递归 `路径:mtime` 拼接，~30 次 stat），测试中每个用例新建 manager 重复 discover 时签名未变直接水合缓存、跳过目录扫描/动态 import；签名变化（新增/修改/删除子Agent 文件）即失效重扫（热加载，见「子Agent 热加载」）

### 测试分层

| 层级 | 覆盖范围 | 要求 |
|------|---------|------|
| 单元测试 | 工具函数、命名空间解析、环境变量合并、路径沙箱、分片路径、截断/压缩算法 | 核心模块**必须**单测，零外部依赖（mock 注入） |
| 集成测试 | AgentEngine 主循环、审批/重试状态机、子Agent 调用、会话持久化 | 用 mock LLM Provider 跑完整流程，不触真实网络 |
| 契约测试 | WS/REST 消息格式、SDK 与协议一致性 | 协议变更必须有契约测试兜底 |
| E2E | 端到端（mock LLM + 内存存储）：prompt → 工具调用 → 审批 → 完成 | 阶段一闭环后建立，回归主路径 |

### 变异验证（把「测试绿」与「真守住了」分开）

测试全绿只说明「**没有断言在抱怨**」，不说明「断言覆盖了它该守的每一格」。**变异验证**就是拿这个差集开刀：故意把产品代码改坏（改名成「变异体」），看测试**认不认得出来**。

- **两类数（缺一不可，只看一类都会把人带偏）**：
  - **杀死率**——「把判据/界面改回旧口径」的变异体（`code`）必须被测试杀掉。有不杀的，说明那条规则实际上**没有任何断言在守**（测试绿是假象）；
  - **存活率**——「只改写法不改判据」的良性重构（`benign`）必须照旧通过。被误杀的后果更隐蔽：下一个人遇到红测试会**删断言**而不是改代码，于是守的东西越改越少。
  - 再加一类**类型化新分支**（`branch`：给枚举加成员而不补期望表）必须被拦下——它守的是「以后加东西时矩阵不会静默漏掉」。
- **五类 `kind`、两种期望（判据只有一句：存活 = 回归失败）**：每条变异体的期望由它的 `kind` 唯一决定，**没有第二个地方可以改**（`tools/mutation/status-core/mutants.ts` 的 `expectsKill`）。下表就是「跑出某个结果时我该吞下什么结论」：

  | kind | 变异体是什么 | 期望 | 不符期望的读法 |
  |------|-------------|------|---------------|
  | `code` | 把**判据/文案/禁用态**改回旧口径（历史缺陷的重放） | **被杀** | **存活 = 回归失败**：那条规则**没有任何断言在守** |
  | `benign` | 只**换写法**不改判据（提局部变量/等价三元链/改名/折行） | **存活** | 被杀 = 误杀，下一个人会去删断言 |
  | `branch` | 给判据枚举**加成员**而不补期望表 | **被拦**（`tsc`） | 存活 = 矩阵以后会静默漏掉新分支 |
  | `blind` | 运行期新分支，文案**没进过期望表**（`as` 转型绕过类型） | **被扫出**（矩阵的「渲染函数体字面量闭集」机关） | 存活 = 那道机关坏了 |
  | `residual` | 同上，但文案与 title **全部复用已进表的字面量前缀** | **存活**（如实记录盲区） | 被杀 = 盲区被堵住了，**是好消息**：把它改归 `blind` 并同步「已知盲区」一节 |

  判据落成这一句话（而不是「杀掉率越高越好」）是关键：`residual` 一类的**期望存活**，正是为了让「矩阵有什么抓不到」这件事每次跑都被人重读一遍，而不是被当成「已经全覆盖了」。为何要把 `blind` / `residual` 从 `code` / `branch` 里单列：D 组的 `as` 转型正好命中「类型层拦不住、只能靠字面量闭集机关」那一空档——**能被 `tsc` 拦的变异体与只能靠矩阵扫的变异体，守它们的机制不同**，混为一类会看不出倒底是谁在守（也会把「机关坏了」误读成「代码改坏了」）。

- **接口形态**：变异体是「原串 → 改后串」两张源码片段；执行器逐条写入**副本**、跑测试、再写回原串。这个形状不需要引入变异测试框架（Stryker / mutant 之类），也不碰构建配置——代价是没有 AST 级变异算子（只能手写片段），收益是**变异体本身可读、可当历史缺陷清单传阅**（下面 A 组 21 条逐条都能讲出“当年怎么坏的”）。
- **为什么值得常态化**：变异体本身就是**这单的历史缺陷清单**（下面 status-core 那 21 条里，A8 是「查看态显示只读」、A9 是「编辑态显示中性编辑」、A12/A13 是两颗按钮的禁用态反向要求……逐条都是实测踩过的坑）。拿历史缺陷当变异体，等于每次重构都重放一遍「当年是怎么坏的」。

#### 怎么跑变异台（一条命令）

```bash
bun run mutate:status           # 等价：bun run tools/mutation/status-core/bench.ts（约 30 秒）
bun run mutate:status:why       # 自助定位：指纹过期了没？该不该重跑？（只读，不跑任何变异；0=不必跑、1=要跑）
bun run mutate:status --only A8 # 只跑名字含 A8 的一条（调试用，不作为验收口径，不刷新指纹）
GEBAI_MUTATION_KEEP=1 bun run mutate:status   # 保留临时副本（排查现场）
```

- **落位与分工**：`tools/mutation/status-core/mutants.ts`（**数据**：33 条变异体清单，约 280 行）+ `bench.ts`（**胶水**：约 350 行、其中非注释非空代码约 200 行——其余是会随实践长的「为什么这么写」）+ `stamp.ts`（**证据指纹**：约 160 行）+ `stale.ts`（**自助定位**：约 70 行，`bun run mutate:status:why`）+ `tools/tsconfig.json`（让 `bun run typecheck:tools` 也扫 `tools/`）。为什么数据与胶水分开：变异体是**历史缺陷清单**、只增不减地长；胶水几乎不变。混在一起的后果是「发现新缺陷就补一条变异体」被劝退（要先读懂整个执行器）。加一条只需三步：复制 `status-core.ts` 的一段原文到 `find` → 写出改坏版到 `replace` → 跑 `bun run mutate:status`。
- **只看一张表**：变异台只跑 `status-matrix.test.ts`（＋必要时 `tsc`），不跑全套 `bun test`。它问的是「**这张矩阵**守不守得住判据」，跑全套会让不相干的红稀释结论、也把成本从 30 秒推到分钟级（`src/files/` 全量约 1.3 秒，全仓 `bun test` 是分钟级）。
- **四条工程纪律**（各对应一类真实事故，不是洁癖）：
  1. **产品源码全程只读**——执行器把「源码 + 矩阵」复制到系统临时目录，**只在副本上**改坏。上一版（开发期手写脚本）是「改真文件 → 跑 → `finally` 还原」，中断（Ctrl-C / OOM / 被 kill）后工作区会留下一个**改坏的产品源码**，而它看着像一次正常的本地修改、极易被误提交。只读原文件后这类事故结构上不可能发生；跑完 `git status` 不应出现任何产品文件改动。
  2. **串行执行，不并行**——并行引入的资源竞争会让「测试抖动失败」被记成**变异被杀死**：对 `benign` 是假告警，对 `code` 是**假通过**（以为守住了）。33 条 × ~0.4s + 31 次 `tsc`（每次约 1.8s）≈ **30 秒**（实测 28.6s，Windows），不值得拿判定可信度去换。
  3. **基线先在跑**——施加任何变异之前先跑一次未变异的矩阵，不绿就直接退出并说明原因（否则「全杀」是假的：任何变异都会「被杀」）。同理，`N fail` 解析不出来（子进程超时/没起来）记为**环境异常**、既不算杀也不算存活，直接判整台失败。
  4. **行尾归一**（本单补上的一条，对应一个实测的潜伏缺陷）——读入 `status-core.ts` 后先把 `\r\n` 归一成 `\n`，比对的 `find` 与施加的 `replace` 全在 LF 口径下进行。为何必须：本仓 `core.autocrlf=true`（索引 LF、Windows 工作区 CRLF，见 `.gitattributes`），而变异体的 `find` 是写在 `mutants.ts` 里的**模板字面量**（其中的 `\n` 是裸 LF、绝不可能是 CRLF）——不归一的结果是 **Windows 工作区下 33 条里有 9 条（所有跳行的 `find`）一律「未命中」**，而 WSL/Linux 工作区一切正常，于是它长得像「源码改了而清单没跟」而不是「工具坏了」（实测：收尾时跑 `--only A8` 才把它露出来，报的正是「`find` 未在 status-core.ts 中命中（多半是源码已改而清单没跟）」）。注意它同时是一个**假警报源**：把人引向去改一份没错的清单。同一条归一口径也被 `stamp.ts` 的 `hashFile` 采用（那里的理由是防指纹假红）——两处必须一致，否则会出现「指纹对得上、变异台却跑不动」。该缺陷的回归锁在闸门测试里（四条：归一后逐条命中 / 不归一确实对不上 / 归一后全中 / 源码级契约看住 `bench.ts` 里那一行）。
- **清单自检先于跑（写错不许静默通过）**：每条 `find` 必须在 `status-core.ts` 里**恰好命中一次**（0 次 = 源码改了而清单没跟、多次 = 施加结果不确定）、名字不重复、`find ≠ replace`。任一不满足即报「变异体清单失效」并以退出码 1 结束——**不跑任何变异**（拿失效清单跑出来的「全杀」比不跑更坏）。
- **退出码即判据**：0 = 全部符合期望；1 = 有不符合（逐条打印「哪一条、期望是什么、实际是什么」）。故它可以直接当人工复核的一条命令，或将来接进门禁。
- **成本与取舍（为什么不进 CI）**：一次约 30 秒（实测 28.6s）、需 33 次 `bun test` + 31 次 `tsc` 子进程；且必须**串行**（纪律②）才有可信判定，而 CI 机器满载恰恰是抖动最多的环境。故定位是**按需复核**（判据/文案改动后、重构收尾时手动跑一次），不进每次提交的门禁。**若将来要接门禁**：先量出 CI 机器上的抖动率，只在「零抖动」时把 `code` 组升为必过；否则宁可不接——一个会偶发假红的门禁，最终会被 `--force` 绕过，等于没有。
- **不会干扰常规测试（已实测）**：`bench.ts` / `mutants.ts` 都**不带** `.test` / `.spec` 标记，故 `bun test` 不采集它们（`bun test tools/mutation` → “filters did not match any test files”）；变异台也不会被跑常规测试时误启动。**一个陷阱值得写下来**：不要给变异台的文件起名 `*.test.ts`（Bun 按名字采集，会被当成用例跑起来，而它会 spawn 子进程、跑几十秒，把整包测试拖坏）。同理，`tools/tsconfig.json` 只扫 `tools/**`。

- **基线实测（入库本单，`bun run mutate:status`）**：`A 21/21 杀`、`B 8/8 存活`、`C 1/1 被 tsc 拦`、`D 闭集机关 2/2 扫出`、`残余盲区 1/1 存活`，共 **33 条**，退出码 0，全程 28.6 秒（Windows）；产品源码零改动（`git status` 无产品文件）。自检口径也验过：把预期改反（把真盲区标成应杀）→ 报「回归变异逃逸」退出 1；把 `find` 改成不存在的串 → 报「变异体清单失效」退出 1（**不跑任何变异**）。
- **半自动兜底（可复核但不必跑全台）**：`mutants.ts` 就是一份**可人工执行的历史缺陷清单**——每条 `find` 都是 `status-core.ts` 里的原文、`replace` 是它的「改坏版」。不装环境、不跑脚本也能复核：按条把 `find` 改成 `replace`（或直接在工作区手动改那一行），跑 `bun run --cwd packages/web test src/files/status-matrix.test.ts` 看是否变红，再 `git checkout` 还原。差别只在「逐条手改 + 手动还原」——慢、且还原靠人记；**判据与证据形式全同**（存活＝矩阵仍全绿）。这是「把变异台完全自动化成本过高」时的最小可行形态，而本仓的成本实测并不高（一条命令 28.6 秒），故以全自动为主、半自动作兜底。

#### 防忘闸门：改了判据面却没跑变异台，常规测试会红

上面「怎么跑」回答的是**怎么跑**；这一节回答的是**怎么不必记得跑**。变异台是一条**得有人想起来才跑**的命令，而忘了跑的代价不是「少跑一次」，是**判据再被改坏时没人拦得住**——矩阵照旧全绿，而「绿」已经不再代表守住了（这正是变异验证存在的全部意义）。历史反复证明「靠纪律记得」会失败，所以本仓把它换成一个**结构**：

```bash
bun run mutate:status   # 跑成功 → 自动刷新 tools/mutation/status-core/stamp.json
bun run --cwd packages/web test src/files/   # 常规测试，其中 status-mutation-gate.test.ts 逐次对账指纹
```

- **机制（两半，缺一不可）**：
  1. **指纹（写侧）**：执行器跑完且**全部符合期望**时，把「指纹面 + 清单条数 + 逐 kind 实测结果」写成 `tools/mutation/status-core/stamp.json`（**入库**，随提交走）。指纹面 = 四份**根**：被审的 `status-core.ts`、判它的 `status-matrix.test.ts`、变异体清单 `mutants.ts`、执行器 `bench.ts`——四者任一改动都让上一次的结论失去依据；**再加上它们递归引用到的本仓源码**（依赖闭包，见下「判据面」）。
  2. **闸门（读侧）**：`packages/web/src/files/status-mutation-gate.test.ts` 在**每次常规测试**里用当前源码重算指纹并逐项对账；对不上即红，失败信息里直接给出补跑的那条命令（人不该需要记得它长什么样）。

#### 判据面：到底哪些文件算在内

「判据面」= **指纹面** = 下面这四份**根**文件的并集（每份各自对应一类「改了它，上一次的结论就作废」）**∪ 它们递归引用到的本仓源码**（依赖闭包，见下条）。这张表是**唯一口径**：`stamp.ts` 的 `STAMP_ROOTS`（实现）与闸门测试的 `EXPECTED_ROOTS`（判据）两处都照它写，且两处**必须逐字一致**（不一致即红）。

| # | 文件 | 在判据里充当什么 | 改了不重跑会怎样 |
|---|------|----------------|----------------|
| ① | `packages/web/src/files/status-core.ts` | **被审对象**——33 条变异体全部施加在它身上（临时副本上改，真文件全程只读） | 改坏判据而无人发现：矩阵照旧全绿，而「绿」不再代表守住了 |
| ② | `packages/web/src/files/status-matrix.test.ts` | **判官**——杀/活的唯一判据来源（34 用例 / 511 断言；`code` 类靠它杀、`branch` 类靠它报缺键） | 判官换了，杀/活的判据也换了：同一批变异体可能换结论，旧结论不再可比 |
| ③ | `tools/mutation/status-core/mutants.ts` | **变异体清单**——33 条，本身就是历史缺陷清单 | 新增一条变异体而没跑 = 那条新规则**从未被审过**（最容易忘的一条） |
| ④ | `tools/mutation/status-core/bench.ts` | **执行器**——基线怎么算、怎么判「被杀」、行尾归一在哪一行 | 判定逻辑变了，stamp 里的旧数字不再由同一套判法产生 |

- **上面四份覆盖的导出面与符号**（要点：指纹是**整文件**级的，不区分符号——改了 `status-core.ts` 里一句注释也算，见下「成本」）：
  - `status-core.ts`：**零依赖纯模块**（零 `import`，闸门里有硬检查），导出面 = **3 个类型别名**（`StatusTone` / `ReadOnlyReason` / `SaveRefusal`）+ **4 个接口**（`ModeSlot` / `WritabilityInput` / `SaveItem` / `EditButton`）+ **9 个函数**：`readOnlyReason`（判据本体）、`saveRefusal`（保存放行）、`modeSlot`（模式格徽标）、`editButton`（标签栏按钮）、`saveItem`（轮盘保存项）、`readOnlyWhy` / `readOnlyNext` / `saveRefusalWhy` / `saveRefusalText`（四处取词与拒绝文案）——矩阵的**导出面契约**要求 9 个函数全部被调用（数量与名字都钉死）；`REASON` / `PENDING` 两份表**不导出**，由取词函数间接覆盖。
  - `status-matrix.test.ts`：输入三轴 `statEditable × truncated × rootWritable` 的笛卡尔积 18 格 × 2 模式 = 36 格，加 5 道机关（行名对账 / `Record<判据枚举,…>` 缺键 / `satisfies` 字面量联合 / 导出面契约 / 渲染函数体字面量闭集）。
  - `mutants.ts`：`TARGET_FILE` / `MATRIX_TEST` 两个路径常量（两个都是**仓根相对**路径），`Mutant` 接口，`MutantKind` 五值联合（`code` / `benign` / `branch` / `blind` / `residual`），`expectsKill`（唯一一处「期望」定义）。
  - `bench.ts`：`validate()`（清单自检）、`runMatrix()` / `runTypecheck()`（两个子进程口径）、行尾归一（`norm` 那一行，闸门里有源码级契约盯着）。

- **按内容判定，不按文件名判定**：四份里只要**字节**变了就算（哈希不区分注释与代码）。为啥不做「只哈希语义部分」这种精细化：那要维护一张「哪几行算语义」的口径表，而口径每宽/窄一寸都要付错价（下「成本」）。
- **为何刻意不放进面内**（写清楚，免得后人「顺手补上」）：
  - `status-mutation-gate.test.ts`（闸门自身）与 `stamp.ts`（指纹实现）：它们**不参与判定**、只记录与对账。进面 = 改一句失败文案就要等 30 秒——而会无故报红的门禁很快就被绕过（本仓已在「为什么不进 CI」里写过这个代价）。
  - `status-core.test.ts`（语义契约）与 `style-contract.test.ts`（接线契约）：变异台**不跑它们**（只跑矩阵 + `tsc`），它们变了不影响这台变异台的结论。
  - `DESIGN.md` / `AGENTS.md`：文档随判据一起改是**正常**的；要文档一变就重跑，闸门就成噪声源。
  - `packages/web/scripts/test-preload.ts`（测试 DOM 基线，`bunfig.toml` 的 `[test] preload`）：执行器的 cwd 是 `packages/web`（同常规姿势），故它**确实**被加载（已用探针实测：`document` / `MutationObserver` 就位）；但它装的是一套**与判据无关**的 DOM 桩，改它不会让任何一条变异体换结论。

- **过宽 / 过窄两面都把价写下来（两个方向的错不一样重）**：
  - **过窄 = 假绿**（严重得多）：漏进去的那份改了却不红，让人误以为「刚验过」——而闸门存在的全部意义就是不让这种事发生。
  - **过宽 = 噪声红**：把不影响判定的文件算进来，每改一次无关文件都要等 30 秒——而会无故报红的门禁很快就会被绕过。
  - 取舍结论：这四份是**实测的最小充分面**（见下「依赖闭包」）。两个方向的边界都钉在可自动核验的判据上，而不是靠人记得。

- **依赖闭包：指纹 = 四份根 + 它们递归引用到的本仓源码**（本单单这一半补上了；上一单只做了「根变了要重跑」）。上面那四份不是随手挑的——它们是「判定链上的四环」：`bench.ts` 读 `mutants.ts`、`matrix.test.ts` 读 `status-core.ts`。**而链路不止四份**：一旦 `status-core.ts` 开始引用别的模块（把取词表抽成 `status-labels.ts`、把类型抽成 `status-types.ts`、把取词逻辑挪进 `status-labels/` 子目录），**被引用的那段代码就是判据的一部分**——它被改坏时根的字节没变，指纹若只算根，闸门不红而矩阵照旧全绿，那是一次**假绿**。故闭包**自动递归收录**，不靠人登记（收录算法 `tools/mutation/status-core/closure.ts`）。

  - **缺口证据：指纹原先只哈希四份根文件本身**（`dfe18f67^` 的形态；如实记在这里，免得后人以为它从来就是这样）。**旧实现长什么样**（旧 `stamp.ts`，逐行可核）：`STAMP_INPUTS` 是四个字面路径的数组，`computeFiles()` 就一句 `for (const p of STAMP_INPUTS) out[p] = hashFile(p)`——面就是那四份字符串，**没有第二处来源**。
    - **为何会漏**（三个条件叠在一起，缺一个都不会出事）：① 根**自己没变**——被拆出去的那份模块改了，`status-core.ts` / `status-matrix.test.ts` 一个字节都没动，哈希自然不变；② 闸门当时**确实有一条**「面内文件引用的东西必须在面内或在豁免表里」的检查，但它只在「**新增依赖**时报」（且依赖表 `KNOWN_DEPS` 是写死的）——被引用的那份**事后**被改坏，依赖图一点没变，那条检查无事可报；③ 变异台只跑矩阵 + `tsc`，而「被引用的模块」不在它读写的临时副本集合里。三条叠起来 = 指纹不脏、闸门不红、矩阵照旧全绿。
    - **真实后果**：那是一次**假绿**——而假绿正是这套防忘机制**唯一不能出的错**（它存在的全部理由就是「不让判据被改坏而无人察觉」）。更坏的是它**伪装成正常状态**：没有任何信号提示「刚验过的结论已经过期」，下次读到 `stamp.json` 里那个 21/21 的人会以为它仍然成立。故本仓把它归为「严重得多」的那一侧（过窄，见上「过宽 / 过窄两面都把价写下来」），而不是当成一条待办。
    - **实测复现（两种口径的对照，可重跑）**：拿一口合成仓造「根 → 中 → 深」的依赖链（形状 = 「把取词表抽成独立模块」那一刻），然后改坏**被引用、不是根**的那份，两种面各自跑 `staleFiles`：**旧口径（只哈希根）→ 过期清单为空 → 闸门不红**；**现口径（根 ∪ 闭包）→ 报「`packages/web/src/files/status-core.ts`：内容已变（记录 `bb501ce9` → 现在 `477517cc`）」→ 闸门当场红**。（口径与 `mutate:status` 的换行归一一致。）
    - **为何这次不再靠人记得（合成树那半边的防退化锁）**：`fingerprintInputs()` 有一个**只给闸门侧用**的注入参数（`{ roots, exempt, base }`，同 `staleFiles(recorded, current)` 与 `depClosure(roots, exempt, base)` 一脉），闸门拿合成树走**真入口**断言「键集含闭包成员（含 `status-core` 那一层的依赖与它的传递层、以及**仓根相对字面量**那一支）」，并配一条**对照**：旧的「只哈希根」面对同一份改动必须**看不见**（若它也看得见，说明合成树没在验闭包、上面那条在空转）。实测：把 `fingerprintInputs()` 的闭包临时改成 `[]`（即退回旧口径）→ 这两条当场红；还原后逐字节一致。
    - **真仓侧的另一半（`20d245fe` 之后补的，此前完全没守）**：上面那套用的是**合成树**，而合成树的依赖链总是非空的（那是手造出来的）——于是它看不见「真仓闭包恰好是空集」这种形态。而那正是发生过的：`dfe18f67` 落地后到 `20d245fe` 复核前，真仓闭包**恒为空集**，机制在、但一次都没生效过（根 ∪ 空 = 根，与只哈希根文件行为等价）。故闸门另有一组只盯**真仓**的断言：① 闭包**非空**；② 每个成员**真的会被弄脏指纹**（拿它的真实哈希走 `staleFiles`）；③ 四份根的**可解析依赖一条都不许静默丢掉**（逐条对账面与豁免表）；④ 一条反向对照（「真仓若退回只哈希根，这组必红」）。它与合成树那组**不可互替**：合成树守「算法对不对」，真仓守「机制在真仓里真的生效」。
  - **边界（为何只收本仓 `packages/` 与 `tools/`）**：不划边界的话，第一个 `import` 就会把全个 `node_modules` 拖进来——本仓实测 `node_modules` 下躺着 **6908** 份 `.ts`/`.tsx`、`vendor` 下 **3762** 份（`@babel/parser` / `monaco` 这类**随包进来的第三方**都在里面）。于是：① 路径必须落在 `packages/` 或 `tools/` 下（`tools/` 不能排除——`bench.ts` / `mutants.ts` 就在那儿，它们是**真判据**）；② 且不得命中 `node_modules` / `vendor` / `dist` / `build` / `public` 这五个目录段；③ `node:` / `bun:` 内建与裸包名（`bun:test` / `react`）一律不算。**实测：本仓 `packages/` + `tools/` 下 777 份源文件里，依赖解析落在 `vendor/` 或 `node_modules/` 内的一份也没有**——即这条边界在当下不改变任何事实，它挡的是**将来**某份源码引了打包进来的第三方（否则那会是一次 30 秒的噪声红）。
  - **豁免表 `EXEMPT_FROM_FINGERPRINT`**：目前唯一一条 `→ stamp.ts`（记录层：算哈希 / 写 JSON / 算闭包），不参与「杀掉 / 存活」的判定——改它的哈希口径不会让任何一条变异体换结论，而进面只会带来噪声红。**豁免是逐文件的**（不是连子树一起剪：被豁免者引用的东西该不该进面取决于**谁引用它**，见下条），且剪错了不靠信任拦住：闸门侧有**独立**对账（豁免表与期望逐字写死）。代价如实记着：若记录层真被改成与闸门不一致，闸门会自己报「内容已变」——方向是**假红**（多跑一次），不是假绿。**这个方向是本仓明确选的那一侧**（假红可忍、假绿不可忍，同「为什么不进 CI」的取舍）。

  - **真仓闭包曾恒为空集：两处具体成因与修法（本单；空集 = 机制在、但一次都没生效过）**

    闸门那套「根 ∪ 闭包」的机制在 `dfe18f67` 就落地了，但直到本单之前，**真仓算出来的闭包一直是 `[]`**。这不是「集合刚好很小」而是一个比「忘了跑」更隐蔽的形态：**跑了也白跑**，而且**伪装成正常状态的绿**（四个对账与合成树全绿）。逐条实测下来是两个独立成因叠加，各自都足以把闭包压成空集：

    | # | 成因 | 实测证据（可重跑） | 修法 |
    |---|------|-----------------|------|
    | ① | **仓根相对路径字面量解析不到**：解析只按「本文件所在目录」一个基准试，而判据面里登记被审对象与判官用的就是**仓根相对**常量（`mutants.ts` 的 `TARGET_FILE` / `MATRIX_TEST`）——`"packages/web/src/files/status-core.ts"` 按 `tools/mutation/status-core/` 去拼，什么也找不到 | `staticDeps("tools/mutation/status-core/mutants.ts")` 修前为 `[]`，修后为 `["packages/web/src/files/status-core.ts", "packages/web/src/files/status-matrix.test.ts"]`（见下「直接依赖」表） | `resolveSpecifier` 改成**两个基准按序试**：相对说明符用「本文件目录」；裸文件名 / 仓根相对路径先试「本文件目录」、**再试仓根**（不试任意祖先目录——否则一句注释里的示例路径就能把面撑大，那是过宽侧的噪声红） |
    | ② | **豁免「连子树一起剪」**：`stamp.ts` 被豁免时，它引用的 `closure.ts`（闭包算法本体）跟着被剪——而它是四份根里**唯一**能收到的依赖（`bench.ts` → `stamp.ts` → `closure.ts`） | 修前 `depClosure(四份根, ["…/stamp.ts"])` = `[]`；修后 = `["tools/mutation/status-core/closure.ts"]` | `depClosure` 默认改成**只剪被点名的那份**：被豁免者自己不进面，但它引用的东西仍按「谁引用它」判定；旧口径保留为 `opts.pruneSubtree`（合成树里仍有判据盯着，不是死代码） |

    两个成因叠在一起的效果：**四份根的可解析依赖里，有 3 条被静默丢弃**（`mutants.ts → 两份仓根相对路径`、`bench.ts → closure.ts` 那一支），于是面 = 四份根、指纹与旧口径逐字节相同。本单修完，真仓闭包 = `["tools/mutation/status-core/closure.ts"]`，指纹 `files` 多出这一份。

    - **为何 `closure.ts` 进面是对的（不是「为了非空而拉一份进来」）**：面是 `computeFiles()` → `fingerprintInputs()` → `depClosure()` 算出来的，而这三个函数就在 `closure.ts` 里——**改了它，算出的面就可能变**（退化成 `[]`、或漏掉某个候选扩展名，都会让「上一次的结论覆盖哪几份」与当下不一致），与「根变了要重跑」完全同类。链路逐环可核：`bench.ts`（根）→ `stamp.ts`（豁免：不参与判杀）→ `closure.ts`（在面内）。
    - **这一项的真实代价（如实记）**：改 `closure.ts`（含注释）现在会报「指纹过期」，要重跑一次约 30 秒的变异台。取舍理由：它是**判定链上的一环**（它决定面长什么样），而本仓对过窄（假绿）明确判为「严重得多」的那一侧——宁可多跑一次，也不要「机制看着在、实际没拦」。
    - **实测（本单的四条反向验证，各自跑完即逐字节还原）**：① **改一个被 import 的本仓模块** → 常规测试报「`tools/mutation/status-core/closure.ts`：内容已变（记录 `59f7582f` → 现在 `fe83396a`）」**1 条红**（改的是**注释**也算——哈希不区分注释与代码），`mutate:status:why` 报同一项、退 1；② **让 `status-core.ts` 真 `import` 一份新模块**（模拟「把取词表拆出去」）→ **6 条红**，点名「`status-probe-labels.ts`：新纳入指纹面，从未被那次跑覆盖过」（还原后 `status-core.ts` 字节一致）；③ **只把成因①的仓根基准去掉** → **3 条红**（`KNOWN_DEPS` 对不上 + 合成树那条新加的仓根相对探针）；④ **只把成因②退回「连树剪」** → **8 条红**（含「真仓依赖闭包是**空集**」那条专为此写的断言）。
    - **一条与「防忘」无关但值得记下的形态**：这两个成因都不是「有人写错了」，而是**机制交付后没人拿真仓数据验过效果**——它们各自的单元测试（合成树那组）当时**全绿**。教训是「加了机制 ≠ 机制生效」：每条修复都得在**真仓**下跑一遍它声称要拦的那件事（本单第④ 项就是干这个的）。
  - **闭包算法自己也处在前沿：它**在指纹面内**（这条改了——原来说「不进面」，本单据实修正）**：`closure.ts` 决定**面长什么样**（`computeFiles` → `fingerprintInputs` → `depClosure` 三层都在它里面），所以它是判定链上的一环、进面（见上「成因②」与「为何 `closure.ts` 进面是对的」）。它**算法行为**另有**独立**判据——闸门侧拿**临时目录造一棵合成依赖树**把它验一遍：递归传递、**仓根相对字面量**、豁免剪枝（默认逐文件 / `pruneSubtree` 连树）、环安全、注释安全（行内块注释**之后**的真 import 不得被吞）、六类边界（第三方 / 随包资源 / 构建产物 / 出仓 / 内建 / 裸包名）。这是本仓一贯的分工：**实现一处、判据另一处，两处不得互推**。
  - **闭包收录口径**：只认**可解析的字面量**——`from "…"` / `import("…")` / `import "…"` / 源码里的 `"….ts"` 字面量；**相对说明符**按**本文件所在目录**解析，**裸文件名 / 仓根相对路径**先试本文件目录、再试**仓根**；扩展名补全 `.ts` / `.tsx` / `/index.ts` / `/index.tsx`。**不做完整 AST**：路径一旦装进变量就看不见——这与既有口径一致（按「与蓄意绕过同类」处置，见下「一处如实记下的剩余风险」）。
  - **五条可自动报红的判据**（`status-mutation-gate.test.ts` 的「依赖闭包」「真仓闭包非空」两组）：① **闭包可复算、键集对得上**（`fingerprintInputs()` 与 `stamp.json` 记的键集逐项一致；闭包成员与写死的 `EXPECTED_CLOSURE` 一致；且每个闭包成员都真被面内某份文件——可经**豁免的中间层**——引用）；② **写死的 `KNOWN_DEPS`（逐份直接依赖）与源码实算相同**——新增/丢失依赖都红；③ **面外文件不影响闸门**（过宽侧，且面内不得出现第三方/构建产物段、不得多出测试文件与 `tools/` 成员）；④ **闭包算法在合成树上独立验**（上一条）；⑤ **真仓闭包非空且零漏**（本单新加：闭包非空 + 每个成员真会被弄脏指纹 + 四份根的可解析依赖一条不丢 + 反向对照）。⑤ 是唯一**必须在真仓上跑**的那条：合成树验不出「真仓恰好是空集」。
  - **⑥ 闭包拦截真生效：在本仓副本上真改文件（任务 `bc5a8e72` 那次人工验证的固化形态）**——上面 ①–⑤ 判的都是「面里**有没有**这一份」（**集合**判据），而 `bc5a8e72` 当初是用**人工在真仓改一份被 import 的模块、看常规测试变红**来确认「拦截真生效」的。那条证据是真的，但它**一次性**：验完就还原，没有任何断言在守它——谁把 `fingerprintInputs()` 退回只拼根、或把 `hashFile` 写成「按文件名哈希」，①–⑤ **照旧全绿**。故补上这台机器真正缺的那半：**不碰真仓文件**，把一份**本仓副本**（`[...STAMP_ROOTS, ...depClosure(ROOTS, [])]` 全图 + 一份面外无关文件）逐字节复制到临时目录，在副本上**真改文件**，拿**生产口径**的同一份函数（`computeFiles(base)` / `hashFile(rel, base)` / `staleFiles`）算两遍：
    - **① 改一份被 import 的本仓模块 → 指纹过期**（当前就是 `closure.ts`：不在根里、但在面内），且只报它、报出「记录 X → 现在 Y」；并配**反面**——旧的「只哈希根」面对**同一份改动**必须看不见它（看不见 = 这份成员真不是根，闭包这半边真在起作用）；
    - **② 改一份没被 import 的无关文件 → 不过期**（防误红：过宽同样错，只是方向不同），带一条**正面对照**（同一份文件在副本上单独哈希**必须**变——它变了而指纹比对仍说不出话，才说明「不过期」是因为**它在面外**，不是因为哈希压根没读盘）；
    - **③ 前提 + 真仓零改动**：副本与真仓**同源**（面相同、逐份哈希等于那一次的记录）是本组的地基，单列一条；另有一条钉住「真仓面内文件全程未被碰、临时目录零残留（只对**本次跑新建的**残留报红，以免把别人的崩溃残留算成假红）」。
    - **它为什么不能用注入式假映射代替**：`staleFiles(recorded, current)` 的注入口径（合成树那组在用）**直接给出哈希**，恰好绕过了本组要验的那一环——「哈希函数**真的**读了下盘上的内容」。故本组必须让 `hashFile` / `computeFiles` 真读盘（为此给这两个函数加了 `base` 注入参数，**与 `fingerprintInputs(input)` / `depClosure(roots, exempt, base)` 同脉的口子**，缺省 `REPO`，故执行器与自助定位行为逐字节不变）。
    - **一个踩过的坑（写下来免得后人重走）**：副本**只复制「面」是不够的**——`closure.ts` 之所以在面内，靠的是链路 `bench.ts`（根）→ `stamp.ts`（**豁免**）→ `closure.ts`；`stamp.ts` 不在面内、但它是**依赖图**上的一环。少复制它，副本算出的面就比真仓少一份，`staleFiles` 报的是「已不在指纹面内」而不是「内容已变」——**看似通过、实际验的是另一件事**。故复制范围取不剪豁免的全图，并把「副本与真仓同源」单列成一条前提判据（这条坑正是它被发现的路径：实测 `face(copy) ≠ face(real)`）。
    - **反向验证（5/5 被杀，各自跑完即逐字节还原）**：① 把 `fingerprintInputs()` 退回「只拼根」→ 本组 **6/7 红**；② 把豁免退回「连子树一起剪」（成因②）→ **6/7 红**；③ 把 `hashFile` 改成「按路径给常量哈希」（不读内容）→ **8/8 红**；④ 让 `hashFile` 忽略 `base`（永远读真仓）→ **4/6 红**（本组专有：只有它抓得住）；⑤ 让 `computeFiles` 忽略 `base`（面按副本、哈希按真仓）→ **2/5 红**。
  - **实测反证**（各自跑完即还原，含上一单两条与单两条）：① 往 `status-matrix.test.ts` 里加一行读面外文件的 `readFileSync` → 报「既不在面内也不在豁免表」；② 往 `bench.ts` 里加一条对面外模块的 `import` → 报「依赖与 `KNOWN_DEPS` 不符」；③ **拿合成树端到端验**：改坏**被引用**的那份（不是根）→ `staleFiles` 当场把它报成过期（这就是原来漏掉的那种假绿，现在有了回归锁）。

- **自助定位：改了判据面后，怎么知道该重跑什么**（这是「不靠人记得」的**人**那一半，与闸门的**机器**那一半互补）。三层，从轻到重：
  1. **不用找命令，失败信息自己给**：闸门红时逐份列出「哪份文件、记录什么值 → 现在什么值」，并附上补跑命令（`bun run mutate:status`）与「为什么不能看着没问题就算了」。
  2. **零依赖自查（不跑任何变异，毫秒级）**：`bun run mutate:status:why`（`tools/mutation/status-core/stale.ts`）——只读指纹、只报过期项。退出码刻意是「0 = 不必重跑 / 1 = 要重跑」，可直接写进脚本的 `if`。输出分三档：指纹新鲜（连指纹记着的 21/21、8/8 一起打出来，并逐份列出「哪份是根、哪份是闭包、被谁引用」） / 指纹过期（逐份列「记录 → 现在」，并支持「面被缩/被扩」两个方向） / 没有指纹（先跑一次立证据）。
  3. **真跑那台**：`bun run mutate:status`（约 30 秒，退出码 0 = 全部符合期望并自动刷新指纹）。**只在「跑全集且全部符合期望」时才刷新**——`--only` 的部分跑与失败跑都不许刷新（否则一次调试跑就能把闸门刷绿，见「指纹的三条纪律 ①」）。
  完整判据（**改了其中任何一份就要跑**）：四份根 `status-core.ts` / `status-matrix.test.ts` / `tools/mutation/status-core/mutants.ts` / `tools/mutation/status-core/bench.ts`，**以及它们递归引用到的本仓源码（`packages/` 与 `tools/` 下，除去豁免的记录层）**——当前真仓的闭包就是一份：`tools/mutation/status-core/closure.ts`（闭包算法本体，链路 `bench.ts` → `stamp.ts` → `closure.ts`）。**改了其中任何一份而没重跑，`bun test` 当场红并告诉你跑什么。**

- **为什么这个形状可靠，而「写成清单条目」不可靠**：清单条目只在**人主动去看**时起作用（改代码时不会去看）；闸门在**每次跑测试**时起作用，而跑测试是改代码的**必经**步骤。它把「记得跑变异台」从一个**静默的隐患**变成一个**当场的失败**——而失败带着修复指令，是任何人都能执行的动作。
- **四个候选（本单实际评过的，含为何没选它们）**：

  | 候选 | 为何不选（含已核的事实） |
  |------|---------------------|
  | **① 写进清单/开发指南**（本条是底线，已做：`AGENTS.md` 硬约束一节 + 常用命令 + 提交验证清单） | 只在人**主动翻文档**时生效，而「改代码时不会去翻开发指南」。**实证**：上一单（`fac0aaa0`）已经这么做了（DESIGN 已记「如何跑」）——它没能阻止这次任务存在的理由本身（「仍靠人主动跑」）。清单必颂，但不能只靠它 |
  | **② git 钩子**（`pre-commit` 跑变异台或校验） | 两重问题。事实面：本仓 `core.hooksPath` 未置、`.git/hooks/` 只剩 `.sample`（已核），即没有钩子可挂；结构面：**钩子默认不进版本库**（它长在 `.git/` 里），新克隆/CI/别人的机器上不存在——写进 `AGENTS.md` 也只能是一句「请你在本地自己装一个」，又回到纪律。而且 `pre-commit` 里跑 30 秒串行子进程会让**每次提交都等**，实际结果是 `--no-verify` 绕掉 |
  | **③ CI 门禁直接跑那台变异台** | 本仓**没有 CI 配置**（无 `.github/workflows`，已核）——退一步说，即使有也不该这么接，理由本仓已写在「成本与取舍（为什么不进 CI）」：串行才有可信判定，而 CI 满载恰是抖动最多的环境，抖动会被记成「变异被杀」（`code` 类上就是**假通过**） |
  | **④ 本单选的：指纹 + 常规测试对账** | 落在**版本库**里（随克隆/CI/别人的机器走）、跑在改代码的必经路径上、零子进程零抖动（毫秒级）、不改任何构建配置、失败信息自带修复命令 |

  取舍总结：①是底线（写了），④是机制（做了）；②③不是「成本高」，而是「在这个仓里它们要么不存在、要么会把判定可信度弄坏」——而真正的问题是**忘了跑**、不是**没人拦**，所以答案落在「让忘了跑当场变成一个带修复指令的失败」上。
- **成本**：闸门**只做哈希对账 + 一次依赖闭包扫描**，零子进程、零抖动，毫秒级（`bun test src/files/` 实测：上一单 588 pass / 1.30s → `dfe18f67` 599 pass、1.49~1.59s → `20d245fe` 601 pass / 1.53s → `bc5a8e72` 606 pass / 1.66s → **本单补「闭包拦截真生效」后 610 pass / 0 fail、2347 断言、2.45s**，无感）。**闸门自身的用例数与断言数**（`bun test packages/web/src/files/status-mutation-gate.test.ts` 实测）：`bc5a8e72` 时 **36 pass / 154 expect() calls** → 本单 **40 pass / 188 expect() calls**（**新增那组共 4 条用例、共约 34 断言、实测时长约 0.6s**——每条含一次「复制 7 份本仓文件 + 真改文件 + 算两遍指纹 + 递归删副本」）。

  > 一处**如实修正**：本节原先写「闸门自身 **31 条用例 / 138 断言** → （`bc5a8e72`）**40 条 / 161 断言**」，而现拿 `bun test` 在 `bc5a8e72` 那版文件上实数为 **36 pass / 154 expect() calls**。数字口径以**命令实测**为准（`bun test` 只统计 `test()` 与它实际执行的 `expect()`，与手数**循环里**的断言不同：本文件的断言有写在一个 `for` 里逐项跑的那种，手数会少）。存疑的旧数字不删、就地标注，符合本仓「修正过的错误事实就在原地记下」的一贯做法。

  这与「为什么不进 CI」并不矛盾——不进的是**那台变异台**（30 秒 + 串行要求 + 抖动下的假通过风险），进的是**对它的证据做一次哈希核对**。
- **指纹的三条纪律**：
  1. **只在「跑全集 且 全部符合期望」时刷新**——`--only` 的部分跑与失败的跑都不许刷新（否则一次调试跑就能把闸门刷绿，闸门当场变摆设）；另有 `--no-stamp` 可在「只想看看当前状态」时显式不刷新。
  2. **内容确定（无时间戳、无 HEAD）**：同一份源码跑两次得**逐字节相同**的文件（本仓对构建产物同款纪律：内容不变不写盘）。好处有二：跑一次变异台不平白弄脏工作区；`stamp.json` 出现在 `git diff` 里本身就是一句可读信号——「判据面变了，证据刚重立」。
  3. **哈希前先归一换行（CRLF → LF）**：本仓 `core.autocrlf=true`（索引 LF、Windows 工作区 CRLF），不归一时同一份提交在 Windows 与 WSL/Linux 上算出的**字节不同**，一次 Windows 上的跑会让 Linux checkout 后**假红**。而一个会无故报红的门禁很快就会被绕过——本仓已在「为什么不进 CI」里写过这个代价，这里不能自己踩一遍。
- **它守防忘，不守防伪**（写清楚，免得被当成安全机制）：手改 `stamp.json` 照样能骗过闸门。它要解决的是**忘了跑**这件高频失误；蓄意绕过交给 code review——`stamp.json` 里逐 kind 的条数与实测数就是给 review 看的（例如清单 21 条 `code` 而结果记着杀 20 条，一眼就不对）。闸门里另有一条**独立**检查把这种手改挡住：指纹里的实测结果必须与该 `kind` 的期望相符（`expectsKill`），不是自说自话。
- **反向验证（本单实测五条，证明它不只是会说「全部符合期望」）**：① 把 `status-core.ts` 改一行（还是**注释**）不重跑 → **1 条红**，指名「`status-core.ts`：内容已变（记录 `03a0ab95` → 现在 `b54fad3b`）」；同时 `bun run mutate:status:why` 报同一项并退出 1（而它**新鲜**时退出 0）——**改注释也算**这句话是可复核的，不是口号；② 往 `mutants.ts` 加一条 `benign` 变异体不重跑 → **2 条红**（指纹过期 + 清单条数不符）——这条覆盖的是最容易忘的路径「发现新缺陷补了一条变异体」；③ 手改 `stamp.json` 谎报「`code` 只杀 20/21」→ **1 条红**（实测结果与期望不符）；④ 删掉 `stamp.json` → **6 条红**（闸门把它当「本仓从未跑过那台」）；⑤ 从指纹面偷偷去掉 `status-core.ts` → **2 条红**（实现侧与判据侧两处对不上）。
- **闭包这一单的四条反向验证（各自跑完即还原；前两条在真仓、后两条在临时目录）**：
  1. **新模块进闭包就报（假绿那一半）**：造一份 `status-labels-probe.ts` 并让 `status-core.ts` 真 `import` 它（正是「把取词表拆出去」那个场景）→ 指纹报两条：「`status-core.ts`：内容已变」+「`status-labels-probe.ts`：新纳入指纹面，从未被那次跑覆盖过」；`bun run mutate:status:why` 报同样两项并退出 1。
  2. **写侧真收录（不只是闸门在算）**：让执行器 `bench.ts` 真 `import` 一份本仓探针模块（副作用 import）→ 跑全集，`stamp.json` 里**确实多出一条** `"tools/mutation/status-core/probe-dep.ts": "sha256:450c1f21…"`（即指纹面不仅含根，还含它们引用的东西）。
  3. **端到端（改被引用者）**：合成树里改坏**被引用**的那份（不是根）→ `staleFiles` 当场把它报成过期（回归锁，已入库）。
  4. **算法不得静默退化**：合成树那组验递归、**仓根相对字面量**、豁免剪枝（逐文件 / `pruneSubtree`）、环、注释安全、六类边界；把 `closure.ts` 恒返回 `[]` 会当场红（而不是静默丢掉覆盖）。
- **本单的四条反向验证（各自跑完即逐字节还原；全在真仓）**：① **改了在面内的被 import 模块**（故意改 `closure.ts` 一句**注释**）→ 常规测试 **1 条红**，指名「`tools/mutation/status-core/closure.ts`：内容已变（记录 `59f7582f` → 现在 `fe83396a`）」，`mutate:status:why` 报同一项并退 1——**这就是本任务要的那条「拦截真生效」证据**；② **让 `status-core.ts` 真 `import` 一份新模块**（模拟「把取词表拆出去」）→ **6 条红**，点名「`status-probe-labels.ts`：新纳入指纹面，从未被那次跑覆盖过」（还原后 `status-core.ts` 字节一致）；③ **只退回成因①**（去掉仓根基准）→ **3 条红**；④ **只退回成因②**（连树剪）→ **8 条红**（含「真仓依赖闭包是**空集**」那条专为此写的断言）。反向验证的完整清单见上「真仓闭包曾恒为空集」一节。
- **「闭包拦截真生效」这一组自己的五条反向验证（各自跑完即逐字节还原；本次任务新做）**：这套用的是「把实现退回旧口径 → 看本组是否当场红」的变异验证（沿用本仓变异台的同一套判据：期望被杀就必须红）。逐条锚点替换、跑完还原（并逐字节校回）；计数口径：组内用例名从**源码**抽（4 条），分子按用例名去重（bun 失败时会把失败块重复打印，不按名字去重会把同一条数成多条）：

  | # | 变异体（把实现退回什么样） | 退出码 | 本组（4 条用例）红几条 | 判定 |
  |---|--------------------------|--------|---------------------|------|
  | ① | `fingerprintInputs()` 退回「只拼根」（丢掉闭包那半边） | 1 | **3/4** | 被杀 |
  | ② | 豁免退回「连子树一起剪」（成因②） | 1 | **3/4** | 被杀 |
  | ③ | `hashFile` 改成「按路径给常量哈希」（不读内容） | 1 | **4/4** | 被杀 |
  | ④ | `hashFile` 忽略 `base`（永远读真仓） | 1 | **2/4** | 被杀 |
  | ⑤ | `computeFiles` 忽略 `base`（面按副本算、哈希按真仓算） | 1 | **1/4** | 被杀 |

  - ①② 是本组存在的理由（闭包那半边被丢掉时，本组会当场红）；实测老几组（合成树、防退化自检、真仓非空）**也会红**——两处**互补而非重复**：老几组判「面里**有没有**这一份」（集合判据），本组判「改了它，闸门**会不会报**过期」（行为判据）。
  - ③④⑤ 是**本组专有**的三条：它们不涉及闭包，而是「哈希到底有没有真去读盘上的那份文件」。老几组对这三条**全部保持绿**（面集合没变），只有本组能抓。其中 ④ 是本组**唯一**能抓住的那条（⑤ 抓的是「面按副本算、哈希按真仓算」这种半通不通的形态），它靠的是第 ② 条里那条**正面对照**（面外文件在副本上单独哈希**必须**跟着变）——没有那条对照，「不过期」会因为哈希压根没读盘而同样绿（那是空转）。
  - 一处**如实记下的代价**：本组要真的写盘（复制 7 份本仓文件到临时目录 + 在副本上真改字节），因而比老几组重（实测四条共约 0.6s，而整个闸门文件 1.4s、`src/files/` 全套 2.45s）——换来的是「哈希真的读了盘」这件事从「在实现里看着对」变成「每次跑测试都有断言在守」。为了不让这份代价随时间泄露成残留，末条用例同时断言「真仓面内文件逐字节未变、本次跑未新建临时残留」。
- **上一单的两条反向验证（针对「过窄」，各自跑完即还原）**：
  1. **面内文件新增对面外源码的依赖**：往 `status-matrix.test.ts` 加一行 `readFileSync(join(import.meta.dir, "status-core.test.ts"))` → 闭包那两条报「这些依赖既不在指纹面内、也不在豁免表里：改了它们闸门不会红（假绿）」（另加指纹过期 1 条，共 3 条红）；
  2. **执行器新增对面外模块的 import**：往 `bench.ts` 加一条 `import { … } from "./extra-data"` → 报「`bench.ts` 的依赖与 `KNOWN_DEPS` 里写死的不符——这不是「测试过时」，而是一个必须做的决定」（共 3 条红）。
  两条都指向同一句话：新依赖进不进判据面，是一个**必须**由人做的决定（而不是一个事后才会被发现的事实）。另外还实测了一件反向的事：把**面外**文件（`status-core.test.ts`）改一行，闸门**不红**——这正是「过宽侧」要的（只有进入判据面才会触发重跑）。
- **一处刻意不进指纹面的东西**：闸门自己与 `stamp.ts` / `closure.ts` / `stale.ts` **不入**指纹面——它们不参与判定（只记录、对账、算闭包），改它们不该要求重跑那台（否则改一句失败文案要付 30 秒）。而「把某个文件偷偷从指纹面里去掉」或「把真成员偷偷豁免掉」这两条绕过路径都是堵死的：闸门里**独立**写死了期望的根（`EXPECTED_ROOTS`）、闭包（`EXPECTED_CLOSURE`）与豁免（`EXPECTED_EXEMPT`），少一项即红——两处独立、不得互推（同「判据只有一处」的反面：这里是**实现一处、判据另一处**，恰恰为了不能自己改自己）。
  - **同一件事的另两面（本单补的）**：①「把面外的文件偷偷算进来」（**过宽**）也有独立检查——`面外文件不参与指纹` 那一组逐个点名了不该进面的文件（文档 / `stamp.ts` / `closure.ts` / `stale.ts` / 闸门自身 / 两份面外测试），并验面内不含第三方与构建产物目录段、不含多余的测试文件与 `tools/` 成员；②「面内某份文件新增了对面外源码的依赖」（**过窄**）由上一节的依赖闭包当场红。二者相加，把「面恰好是那几份、不多不少」从「文档里的一句话」变成「四道能自动报红的判据」。
  - 附带一条**非目标**：闸门不保证「面内文件的内容真的被矩阵判到了」（那是**那台变异台**的职责）。参见下一节：这种分工是刻意的——轻的进常规测试、重的按需人工跑。
- **与「半自动兜底」的关系**：半自动教的是「不用变异台也能复核」，本闸门管的是「跑没跑过」。两者互补：半自动供**离线/无环境**时用（人不跑台，也仍能逐条手改成 `replace` 看矩阵变不变红）；闸门供**日常改代码**时用（跑了没有，测试自己会问）。

#### 为什么只有这四份：一次范围审计（本单做的，含结论与反证）

「判据面过宽 / 过窄」这件事不能靠感觉，它与「依赖闭包」是同一个问题：**改了哪份源码会让这台台的结论失真？** 本单逐份核过，结论与证据记在这里（免得后人再摸一遍）。

- **核的方法**：不是「看文件名像不像」，而是问三个问题：① 这台台**真的会读/跑**它吗？② 改了它，33 条变异体的**杀/活结果会变吗**？③ 它是「判定的输入」还是「结论的记录」？
- **逐份结论**：

  | 文件 | 这台台会碰它吗 | 改了会变结论吗 | 判定 |
  |------|------------|------------|------|
  | `status-core.ts` | 会（施加变异的对象，临时副本） | **会**（它就是要被改坏的东西） | **在面内** |
  | `status-matrix.test.ts` | 会（跑它算 pass/fail） | **会**（它是唯一的杀/活判据） | **在面内** |
  | `mutants.ts` | 会（`bench.ts` 直接 `import`） | **会**（清单变了，跑的就是另一批变异） | **在面内** |
  | `bench.ts` | 会（就是执行器本体） | **会**（基线/杀/活的算法都在它手里） | **在面内** |
  | `stamp.ts` | 会（`bench.ts` 调 `writeStamp`） | 不会（只写哈希与 JSON，不参与判杀） | 面外（**显式豁免**，附理由） |
  | `status-mutation-gate.test.ts` | 不会（执行器不跑它） | 不会 | 面外 |
  | `status-core.test.ts` / `style-contract.test.ts` | 不会（执行器只跑矩阵 + `tsc`） | 不会 | 面外 |
  | `packages/web/scripts/test-preload.ts` | **会**（cwd = `packages/web`，`bunfig.toml` 的 `[test] preload` 生效；已用探针实测 `document` 就位） | 不会（装的是与判据无关的 DOM 桩） | 面外（已核、已记） |
  | `main.ts`（消费侧） | 不会（执行器只处理 `status-core.ts` + 矩阵） | 不会 | 面外 |

- **发现与修正**：没有发现「真的过宽」（四份都是判定链上不可替代的一环，没有多余项）；但发现「过窄」这个风险**完全靠人记得**——依赖是散在源码里的（`bench.ts` 的三条 `import`、矩阵的两条 `readFileSync`），新加一条也没人拦。本仓分两单把它关死：上一单先做「面内文件真正引用的东西必须在面内或在豁免表里」，**本单再把它从「检查依赖列表」并为「指纹直接含依赖闭包」**——即被引用的那份改了，指纹当场脏（原来是「新增依赖时报」，现在是「被引用者一改就报」）。`main.ts`、`test-preload.ts` 这两份**曾疑似应进面**的，都在上表里把「已核为何不进」写清楚（前者执行器根本不碰，后者装了也不影响判杀）。
- **一处如实记下的剩余风险**（不假装闭环）：闭包检查认的是**字面依赖**（`import … from "…"` 与源码里的 `"….ts"` 字面量）。若有人用**拼出来的路径**读文件（`readFileSync(join(dir, name))`，`name` 来自变量），闭包看不见——而它同时也绕过了矩阵自己那套「按函数体取源码字面量」的机关，属于同一类刻意规避。**代价与处置**：不为此上完整 AST（成本与它防的风险不对称），而是把它记在这里；真出现时按「与蓄意绕过同类」处理（它已超出「忘了跑」这个机制要防的范围，同本节开头「守防忘，不守防伪」）。
- **闭包那一单的实测代价与取舍**（如实记，免得后人以为是白拿的）——**本单已把其中「当下闭包为空」那一条修掉，原文保留作历史**：
  - **当时真仓闭包为空**（`dfe18f67`…`20d245fe` 期间的实测）：四份根本下零依赖（当时的判断是「`status-core.ts` 零 import、矩阵只引它、清单只写路径常量、执行器除清单只剩被豁免的 `stamp.ts`」）。**这个判断后来被证伪**（两处成因叠加，见上「真仓闭包曾恒为空集」）：清单那两条**仓根相对**依赖与 `bench.ts → closure.ts` 那一支都被静默丢掉了。所以那一单的交付**不是「多哈希了几份文件」**，而是**机制本身 + 它的可测性**；而「机制生效」是**本单**补上的（真仓闭包现为 1 份，另有专盯真仓的四条断言）。**教训**：那次「四份根本下零依赖」的结论是靠**读源码印象**下的，没拿 `staticDeps` 逐份算一遍——而它恰好掩盖了整整一个空集。
  - **为此付的成本**：调一次闸门从「约 5 ms」增到「约 6 ms」（读 4 份文件照旧，多的是递归扫依赖——而依赖通常就几份）；`stamp.json` 的 `files` 里多了闭包成员。**与上一单不同的是：本单它真的多了一份**（`closure.ts`），故本单会刷新指纹（这是「机制真的生效了」在 `git diff` 里的可见形态，不再是「逐字节相同」那句自谦）。
  - **另一处代价写在 `AGENTS.md`**：提交前清单里那句「四份」改成「四份根 + 依赖闭包」，免得人照旧口径判断「我改的是面外文件」而把那半边漏掉。

#### 「漏检 11 例」的教训（为何要改判**数据流**而非展示层接线断言）

背景（`3afa5896`，基于 `cb27f100`）：文件工作台的状态格判据已收口成「一组输入 + 一个函数」（`files/status-core.ts` + `main.ts` 的单一取数口 `writabilityOf`），且有一条源码级**接线契约**（`style-contract.test.ts`）声称盯着「三处消费真的都接了同一组输入」。用变异验证一验，**展示侧漏检 11 例**——即把界面改回旧口径（按钮只判 `editable`、徽标只看 `truncated`）时，测试**全绿**。

- **病因：断言的是「名字出现过」，不是「数据真的流过」**。旧写法查的是字面片断（`toContain("const refusal = saveRefusal(writabilityOf(")`）与某个取数口**出现次数 ≥ 4**。于是：
  - 「真的接了」与「只是恰好写了这个函数名」在断言眼里**长得一样**——把按钮改回自己拼 `t.stat.editable`，计数仍被保存那一行凑够；
  - 一个**计数型**断言（「出现 ≥ N 次」）只说明代码里**有多少处提到**它，完全不代表**哪几处消费真的用了它**。计数是被最不相关的那一行满足的。
- **改法：断言「实参的数据流」，而不是「代码的长相」**。逐**调用点**解析消费函数的每一处调用（括号配平、跨行认），取对象实参里 `readOnly` / `refusal` 的**值表达式**（不是整段实参——否则 `{ readOnly: null }` 这种真另拼输入的写法会被放过去），要求它来自取数口或由其派生的具名变量（闭包上限几轮，容许提取/两跳/改名）。另加两条形态无关的硬检查：消费点**不得自己现拼**三个原始字段；保存处不得再直接读根清单的 writable。
- **结果**：15/15 变异体被杀死、9/9 良性重构照旧通过（改前：致死 4/7、误杀 6）。
- **一条同样重要的反向纪律：不做「位置敏感性」断言**（「某调用第一个实参必须是什么」）。实测那类正则把「把取数口提成局部变量再传」这种纯重构全判红。而真正想防的退化（把取数口拆开单传字段）会撞上 `WritabilityInput` 这个**具名结构类型**，`bun run typecheck` 直接拦下——能用类型表达的约束就别再拿正则写一遍。

#### 落地：`status-core` 判据矩阵（`files/status-matrix.test.ts`）

接线契约守的是「**接上了没有**」；它守不住「**接上之后的每一格取值对不对**」。所以同一手法往下推一层，做成一张**表驱动的判据矩阵**：输入三轴 `statEditable` ∈ {true, false, undefined} × `truncated` ∈ {true, false} × `rootWritable` ∈ {true, false, null} 的**笛卡尔积 18 格**，每格 × 两个模式 = **36 格**，逐格断言**文案 / 配色 / 按钮 disabled / 拒绝文案**四件套（还含按钮图标与按下态、轮盘保存项的禁用态与 title、拒绝 toast 的全文与句内顺序）。

- **与语义契约测试的分工（两者都要有，不是重复）**：既有 `status-core.test.ts` 钉的是以「理由」组织的**不变量**（中性态与警示态必须可区分、编辑态按钮恒可点、三口径同真同假）；矩阵钉的是以「输入组合」组织的**取值表**。不变量测试有个固有盲区——**它只覆盖它枚举到的那几格**：新增一个分支时，只要旧组合上不变量仍成立，测试会继续全绿，而新分支从未被任何一行覆盖。矩阵把「有哪些组合」显式写死，**缺行是看得见的**。
- **缺口靠五道机关暴露（不靠人记得回来补）**：
  1. **行名携带三轴取值**，表与笛卡尔积逐名对账——少一行就报出缺的是哪一格；
  2. **期望表是 `Record<判据枚举, …>`**——给判据枚举加成员而不同步加行，`tsc` 报「Property missing」；
  3. **`satisfies` + 类型断言**——表里**实际用到**的字面量联合必须恰好等于判据联合本身，「声明了却没人用」在编译期就红（`bun test` 不做类型检查，故与② 互补：② 管「加判据缺期望行」、③ 管「加判据没进输入矩阵」）；
  4. **导出面契约**——模块导出的每个函数都必须在本文件被调用过（新增判据函数而忘了排矩阵，红）；
  5. **渲染函数体内的文案字面量闭集**——`modeSlot` / `saveItem` 函数体里所有 `text:` / `title:` 字面量必须在期望表里被断言过。这一道专治**机关③ 的盲区**：给渲染函数加一支「输入类型里构造不出来」的运行期分支（`readOnly as string` 判定 / 新字段）时，那种分支不会出现在表的字面量联合里，②③④ 全都看不见它。**按函数体取、不做关键词筛选**——筛选条件本身就是漏口（本轮实测：一个 `text: "加密"` 的新分支正因为不含「查看/编辑/保存」任一词而静默通过）。
- **实测验收（本单口径）**：`A 回归变异 21/21 被杀`、`B 良性重构 8/8 存活`、`C 类型化新分支 1/1 被 `tsc` 拦下`、`机关⑤ 扫出 2 例运行期新分支`。矩阵自身 **34 用例 / 511 断言**；`src/files/` 全量 **536 → 570 pass / 0 fail**（`bun run --cwd packages/web test src/files/`）。
- **已知盲区（如实记录，不假装守住）**：加一支运行期分支、且它返回的文案与 title **全部复用已在期望表里的字面量前缀**时，机关⑤ 与机关③ 都看不见它（`text: "不可编辑"` + `title: "不可编辑：文件已加密"` 这类）。这是**源码字面量闭集**这种手法的固有上限——它只能保证「新写的字」被审到，管不了「旧字新用」。要真堵住得让分支**在输入类型里可表达**（加字段进 `WritabilityInput`），那属于设计层变更、不在本闸门职责内；下面的变异台把它单列一类（`residual`）并期望它**存活**，为的是让这个上限在每次跑变异时被重申一遍，而不是被当成「已经全覆盖了」。
- **那条「残余 1」不是缺陷，别去修**（这条是给未来的读者写的，免得被当成待办）：变异台输出里那行「已知盲区（应存活，如实记录）: 1/1」——任务描述里就叫它「**残余 1**」——指的就是上面这个上限，它对应用 `D3` 那一条变异体。**它长什么样（逐字）**：

  ```ts
  // 施加在 modeSlot 里（现第 111 行 `const r = REASON[o.readOnly]` 之前）：
  if ((o.readOnly as string) === "encrypted") return { text: "不可编辑", cls: "warn", title: "不可编辑：文件已加密" }
  ```

  一条命令自助复核（约 3 秒，不刷指纹——部分跑不构成证据）：`bun run mutate:status --only D3`，看到「存活（应活）[residual] … pass=34 fail=0」就是它。

- **为何可接受**，四条（前三条是原来的，第四条是本单补的、也是最容易误读的一条）：
  1. **它不是「漏掉的测试」，是「这套手法的边界」**——`D1`/`D2` 已经证明这道机关能看见「新文案」与「新 title」两种新分支，`D3` 只是在探它的**尽头**。要它变红得先写出一个**同时**复用旧文案与旧 title 前缀的真分支，而那已经不属于「随手漏测」的形状。
  2. **它要真发生，得先突破类型层**：造出这条分支必须用 `as` 转型绕过 `ReadOnlyReason` 类型（真实开发里改判据的正常路径是加枚举成员，那会被机关②（`tsc` 报缺键）与 `C1` 拦下）。也就是说，漏网需要「既有意绕过类型、又不写新字」。
  3. **它现在的作用是提醒而不是隐藏**：以 `residual` 单列一类、**期望存活**，于是「矩阵有什么抓不到」每跑一次就被重读一遍；一旦哪天被堵住，它会以「残余盲区已堵住——可把它改归 blind（好消息）」报出来（那时同步改 `mutants.ts` 的 kind 与本节）。
  4. **它不是「等价变异」，也不是「无害」——别把它归错类**（这条更正一个后人一定会踩的误读）：`benign`（B 组那 8 条）才是**真正的等价变异**：只换写法、判据一字不改，所以「期望存活」是对它的正确判据；而 `D3` **是改了行为的**——它真的多出一支分支、真的会返回与 `REASON` 表不同的 title 文案（`不可编辑：文件已加密` 而不是 `不可编辑：该文件类型或权限不支持编辑；可下载后用本地编辑器修改`）。它存活的原因**不是「无害」**，而是「矩阵的判据看不见它」——两者的后续处置完全不同：前者（等价）永久不必管，后者（盲区）**只要有人真写了这种分支，就是一次真的漏测**。把它读成「等价变异」的代价很具体：下一个人遇到同形状的漏测时会因为「这类变异本来就无害」而放过它。措辞上就说「残余盲区 / 已知未杀」，**别说「无害」或「等价」**。

  取舍（为何不现在就堵）：把分支在输入类型里表达出来（加字段进 `WritabilityInput`）意味着给**设计层**加一个当下无人用的输入轴，代价是判据面变宽、后面每个消费点都要多养一支分支；而它防的风险（有人绕类型加分支且不写新字）在本仓无先例。**先如实记着，等真出现第一次再堵**。
- **运行口径**：变异台是**开发期脚本，本仓已入库**（`tools/mutation/status-core/`，`bun run mutate:status` 一条命令跑，见上「怎么跑变异台」）——**但不进 CI 门禁**：它按「改坏源码 → 跑测试 → 还原」工作，在 CI 机器满载的抖动下会把「测试抖动失败」记成「变异被杀」（`code` 类上就是一次**假通过**），而判定的可信度比自动化的收益更重要（成本与取舍见上）。**替代它的是一个轻量防忘闸门**（见上「防忘闸门」）：变异台跑成功时把指纹写进入库的 `stamp.json`，而 `status-mutation-gate.test.ts` 在**每次常规测试**里对账——改了判据面却忘了重跑那台，当场红（零子进程、毫秒级、无抖动，故可以进常规测试）。这两件事不矛盾：不进的是**那台变异台**，进的是**对它的证据做一次哈希核对**。上一版曾判为「不入库」（理由是「能改产品源码的脚本」这种脚留）——入库后这个理由由**结构**而非**纪律**消解：执行器**从不写**产品源码，它把源码与矩阵复制到临时目录、只在副本上改坏（见上「三条工程纪律 ①」）。入库的是**清单 + 执行器 + 指纹**：清单 33 条（A 21 / B 8 / C 1 / D 3）、执行器约 330 行、指纹 1 份。每次跑完务必确认产品代码零改动（`git diff` 对产品文件为空——只读副本机制下它是恒成立的，但仍值得看一眼）。

### 自测工具：fake-llm（假模型端到端自测）

`packages/server/scripts/fake-llm.ts`——无真实模型 Key 的**手工端到端自测**工具：按场景脚本回放 OpenAI `chat/completions` 流式响应（文本分片 / tool_calls 分片 / usage），服务端 provider 无感接入，用于浏览器实测前端行为（流式渲染、subsession_run 执行过程、计划审批滚动等单测覆盖不到的 UI 链路）。

```bash
bun run --cwd packages/server fake-llm [场景]   # 场景缺省 = text；端口 9801（FAKE_LLM_PORT 覆盖）
# 服务端指向假模型联调：
GEBAI_LLM_API_BASE=http://127.0.0.1:9801/v1 GEBAI_LLM_API_KEY=test \
  GEBAI_LLM_MODEL=fake GEBAI_PORT=3900 bun run --cwd packages/server dev
```

| 场景 | 脚本 | 验证点 |
|------|------|--------|
| `text` | 单轮纯文本回复 | 链路冒烟（消息上屏/流式渲染） |
| `subsession_run` | 主会话调 `subsession_run` → 子会话两轮（文本 + `code_system_info` 工具 / 多行结论）→ 主会话收尾 | 子会话运行过程实时渲染、结果 markdown 换行无双倍空行 |
| `ask` | 延迟 4s 调 `ask` 计划分支（阻塞等审批）→ 批准后收尾 | 计划卡片全文、选择卡片弹出时消息流落底、审批后继续 |
| `error` | 每次调用 HTTP 500 | 错误气泡与重试耗尽呈现 |

### 自测工具：e2e-subsession（子会话运行服务级端到端，假模型 + 真实服务）

`packages/server/scripts/e2e-subsession.ts`——服务级全链路验证：**真实歌白服务进程**（独立 `GEBAI_HOME` + 独立端口 + 内嵌脚本化假模型，不污染真实数据）+ SDK（REST/WS），逐场景断言事件流 / 会话落盘 / 子Agent 发现 / 门禁回传。覆盖 7 组场景共 54 项断言：①同步隔离子会话（`subSession` 事件标记、过程存档挂调用记录、父会话不逐条落盘过程、不注入合并工具）②同步继承子会话（fork 上下文、两份报告自动合入、合并消息 `role=user + engineNote=subsession`、下一轮上下文可见）③异步运行（`bg_task` s 前缀 wait 取回结果与存档、`subsession_merge` 阶段性合入 + 兄弟感知 + 事件广播）④待办运行内隔离（不继承/不回流/落盘只含父任务、事件带 `subSession` 标记）⑤门禁与参数校验回传（超限/两形态互斥/未知子Agent 附因）⑥空 `agents` 通用子会话（只给全局工具 + 编排）⑦递归深度（进程树三层，第四层被拒、嵌套存档递归挂载）。

```bash
bun run --cwd packages/server e2e:subsession        # 假模型全场景（约 30s，54 项断言）
bun run --cwd packages/server e2e:subsession:real   # 真实模型（GEBAI_LLM_* 配置）实测模型是否按新语义调用，3 任务 24 项断言
bun run --cwd packages/server e2e:subsession:ui     # 常驻假模型 + 服务（页面手输 S1/S2 触发两形态），供 playwright/浏览器实测 UI 渲染
```

**对已启动实例实测（不自起服务）**：`e2e:subsession:real` 支持外部模式——把 `E2E_REAL_EXTERNAL=1 E2E_REAL_PORT=<端口>` 指向一个正在运行的实例（如验证实例），断言会话消息改经协议读取（不依赖实例的 `GEBAI_HOME` 布局），并额外校验前端主产物含 `subsession-run`（防「后端新版 + 前端旧版」错位验证）：

```bash
E2E_REAL_EXTERNAL=1 E2E_REAL_PORT=3999 bun run --cwd packages/server e2e:subsession:real
```

**验证实例启动**（不动现有实例的数据与外部通道，独立 HOME + 关任务/待办/飞书）：

```bash
set "GEBAI_HOME=%TEMP%\gebai-live-3999" && set "GEBAI_PORT=3999" && set "GEBAI_TASKS_ENABLED=false" && set "GEBAI_IDLE_TODO_ENABLED=false" && set "GEBAI_FEISHU_BOT_ENABLED=false" && bun --preload ./scripts/build-env-embed.ts ./src/index.ts
```

UI 层验证（需 playwright）：打开 `http://127.0.0.1:3991/` 输入 `S1`（隔离形态 → 消息流一行「🌿 子会话 · s1 · ⚙ code」入口，点开右侧「子会话」面板看过程与报告）、`S2`（继承形态 → 两行「🌿 甲/乙」入口 + 两条按 markdown 渲染的「子会话合入」通知条，各带「查看子会话过程」）；对真实模型实例可直接用自然语言（如「用 subsession_run 的 inherit_context:true 并行两个子会话算题并汇总」）实测端到端。

### 自测工具：compact-e2e（压缩接续质量端到端，真实模型）

`packages/server/scripts/compact-e2e.ts`——用**真实模型**量「上下文压缩后能不能接着干活」：压缩的单测只覆盖「摘要输入构造正确」，而接续质量（任务目标 / 文件路径 / 未完成事项是否被摘要保住）只能拿真模型跑。对照设计：A 基线会话（同构长历史不压缩直接问，确认测试本身有效）+ B 压缩会话（先压缩掉含「任务规格」的早期区间再问），判据为 B 的回复能复述出五要素（弱正则匹配）。同时打印生成的摘要——用于评估 `SUMMARY_ITEM_LIMIT` 与块预算是否需要调。

```bash
bun run --cwd packages/server scripts/compact-e2e.ts              # 缺省 320 段 ≈ 7 万字符（约 8 分钟）
COMPACT_E2E_LINES=60 bun run --cwd packages/server scripts/compact-e2e.ts   # 冒烟（约 1 分钟）
```

数据隔离：脚本把 `GEBAI_HOME` 指向临时目录（不碰真实用户数据），LLM 配置仍来自仓库 `.env`；退出码 0 = 压缩后可复述全部五要素。实测（320 段 / 73071 字符 / 323 条）：压缩 303 条耗时 4.7s，摘要 5294 字符**完整保住目标、两个文件路径与两条未完成事项**（并把重复填充段标注为「无新增信息」）；压缩后复述 **5/5**，基线同题也 5/5——即当前参数下接续质量无损失，无需调参。

场景脚本耗尽后回复固定收尾文本（防场景外调用死循环）；`/probe` 页面（`http://127.0.0.1:9801/probe`）连服务端 `/ws` 验证浏览器连通性（标题四态：`WS_OK`/`WS_ERR`/`WS_TIMEOUT`/`WS_CLOSED`）。

### 覆盖率门槛

- 核心引擎（AgentEngine、ToolRegistry、EnvManager、Sandbox、命名空间解析）行覆盖率 ≥ 90%
- 工具函数 ≥ 80%；整体仓库 ≥ 70%（`bun run --cwd packages/server test:coverage` 产出报告）

> 上述为**人工约定的目标值**：无 `coverageThreshold` 配置、仓库亦无 CI 流水线（`.github/workflows` 不存在），门槛不构成强制门禁；提交前跑全量测试靠约定（`run_tests` 工具与 AGENTS.md 纪律）。

#### 实现状态（初始落地快照——部分计数与描述为当时形态，最新口径以正文各节与「常量参考」为准）


- 已实现：`@gebai/server`（AgentEngine 主循环、LLMProvider（OpenAI 兼容 chat/completions + OpenAI Responses + Anthropic 三接口 SSE 解析，**usage 真值解析**（OpenAI `stream_options.include_usage` 末 chunk / Responses `response.usage` / Anthropic `message_start.input_tokens`+`message_delta.output_tokens`，统一挂 done chunk 的 `usage` 字段；服务端不返回时为 undefined → 引擎估算兜底，见「上下文占用口径」），**统一多模态内容块转换**（图片块 base64 内联 → OpenAI `image_url` / Anthropic `image`，`imageMessageBlocks` 助手），**额外模型接口参数 `GEBAI_LLM_EXTRA_PARAMS`（JSON，如推理强度 `reasoning_effort`）Provider 级 + 任务级（浏览器本地注入）两级覆盖，顶层合并进请求体**）、ToolRegistry 命名空间解析（含注册期前缀互斥校验）、SessionStore 分片持久化、EnvManager（全局 + 会话内存态，用户环境变量零留存）、Sandbox 路径沙箱/子进程、AuthService 多用户令牌、**外部身份扩展点**（同源部署集成网站：`POST /api/v1/auth/exchange` 外部身份 → GEBAI 令牌，验证器可插拔——`GEBAI_EXTERNAL_AUTH_SECRET` HMAC 验签（±10 分钟防重放）或 `GEBAI_EXTERNAL_AUTH_URL` HTTP 回调验证，互斥同设报错；`AUTOCREATE` 自动创建/白名单两种映射；Web UI 启动时 URL 参数或 localStorage 同源直读自动兑换，SDK `exchangeExternalUser`/`getExternalAuthConfig`；`external-auth.ts` 单测 13 用例 + 端点集成测试 6 用例）、EventBus、富内容块（text/code/image/file/diagram/diff/html）渲染、**视觉工具 `vision`（后已移除——视觉统一走 vision 子代理）**（额外多模态模型 `GEBAI_VISION_*` 配置，目标 `target` + 图片文件 `image` 参数，base64 内联调用视觉模型，未配置时回落到显式声明多模态能力的主模型，单图 8MB 上限，输出截断保护 + `image` 内容块；`makeVisionTool` 依赖注入可单测）、**图片附件链路**（本地模式附件源路径按会话根解析（修复 CWD 误解析）、`AttachmentRef` 存逻辑路径、多模态主模型 base64 内联/其余降级文本说明 + vision 指引、接口 HTTP 4xx 拒绝图片块时自动降级重试、Web 端 canvas 图片压缩 1280px/2MB）、**内容展示工具 `show` 图表分支（原 `draw`）**（**四种图表语言**——`format` 必选参数 `mermaid`/`plantuml`/`d2`/`echarts`，工具描述与参数说明内置选择指南指导模型按需选择；前端实时渲染确认：成功才返回、报错回传模型、5 秒超时降级；Web 前端本地渲染四语言各自引擎（mermaid npm 包 / `@plantuml/core` / `@terrastruct/d2` WASM / echarts UMD SSR 渲染 SVG，均零网络）；**后端组合渲染器四语言全支持**（飞书与 `render=backend` 通道：plantuml TeaVM 引擎 / mermaid + happy-dom 垫层（getBBox 几何估算覆盖防布局坍缩）/ d2 WASM（二进制模式内嵌产物物化 `{GEBAI_HOME}/vendor/d2js/{version}/`，版本变更自动换目录）/ echarts npm 包 SSR（`ssr:true` 零 DOM，顶层急切导入防 zrender 环境误判）+ 共享 resvg 栅格化（负原点 viewBox 平移归一兼容），全局环境切换 + 单一串行队列，`globalThis.window` 仅临时存在））、**html 分支（原 `render_html`）**（沙箱 iframe 域隔离渲染：`allow-scripts` 不含 `allow-same-origin`，脚本可执行但隔离于 opaque origin，无法访问宿主页面 DOM/存储/顶层导航；落盘会话 `tmp/` 并返回 `html` 块，支持模型显式指定预览尺寸 `width`/`height`，未指定时 iframe 固定铺满消息流宽度、无任何内容宽度反馈）、**path 分支（原 `show_file`）**（模型把文件**直接展示给用户**：按类型产出直显内容块——图片内联 `image`、图表源文件 `diagram` 渲染、`.html` 沙箱预览、文本/代码内联 `code`（≤512KB 读取/4 万字符截断）；无法内联类型回退 `file` 查看卡片（点击按需加载，PDF 内嵌渲染）；会话 `tmp/` 内直接引用，会话外文件复制到 `tmp/shown/{主名}-{内容哈希}.{扩展名}`（≤100MB）后引用）、**全局 diff 工具（后已移除）**（LCS 行级对比原为纯函数 `core/diff.ts`，工具本身已下线——`diff` 内容块仅用于历史会话回放）、5 个内置子Agent（当时；**现为 12 个**，见「更多内置子Agent」）。以下为当时形态（含 `desktop`、**飞书云文档 `feishu_docs`**：42 个工具覆盖文档 docx 创建/读取/块编辑/按文本反查块 id（`find_blocks`）/Markdown 导入导出/**插入图片（`insert_image` 三步流程）**/**思维导图画板读取（`get_board`：mindnote 块自动提取画板 token，结构化提取 PlantUML 源码或重建连接线流程）**/**用户授权（`auth_user_authorize`/`auth_user_token`/`auth_user_status`/`auth_user_clear`：OAuth code 流程配置 user_access_token，会话级存储+自动刷新，配置后资源操作以用户身份执行、创建用户所有权文档；**默认自动回调**——授权后浏览器跳回内置端点 `GET /api/v1/oauth/feishu/callback` 自动兑换写回会话（`GEBAI_PUBLIC_URL` 可配，见「飞书用户授权」））**、云空间、电子表格、多维表格、知识库、搜索、权限与 `api_call` 兜底；块列表附 `type_name` 标注、`page_all` 自动翻页（达上限提示）、小节读取、块操作失败本地诊断、**权限类错误码自动附所需 scope 与授权链接**；`FEISHU_DOCS_*` 凭证 + tenant_access_token 缓存；写操作审批；`createFeishuTools` 依赖注入 + `markdownToBlocks`/`blockText`/`extractBoardContent`/`extractOAuthCode` 纯函数 + 共享 OAuth 模块（`oauth.ts`：兑换/刷新/会话令牌存取/授权状态注册，工具与 REST 回调共用），单测覆盖率 95%）、**浏览器自动化 `playwright`**（无头 Chromium，**当时 15 个工具，现为 30 个**（20 基础 + 7 会话 + 3 CV），覆盖导航/读取/截图/交互/表单/JS 执行/多标签页；**node 桥接架构**——Bun 与 playwright driver pipe 兼容问题用常驻 node 子进程 JSON-RPC 规避，BrowserContext 按会话隔离 + 空闲回收，导航/交互/脚本类默认审批；`createPlaywrightTools` 依赖注入 + Bridge 协议层单测 17 用例，真实 chromium E2E 验证通过）、REST/WS 双通道 API（WS 消息处理独立 `ws.ts`、反馈存取独立 `feedback.ts`；REST 全端点：feedback 查询/导出、多选 zip 打包下载、Webhook CRUD、OpenAPI 文档、auth/me、**飞书 OAuth 回调 `/api/v1/oauth/feishu/callback`**）、**上下文压缩**（当时口径：主动 + 自动 80% 窗口阈值触发、LLM 摘要 + 滚动裁剪降级；**现为「窗口剩余 < 输出预留」水位触发**，见「上下文保护」）、**Webhook 推送**（HMAC 签名、3 次指数退避重试、事件白名单、多用户会话归属过滤）、命名空间注册期碰撞校验、服务模式 admin 密码哈希引导（`GEBAI_ADMIN_PASSWORD_HASH`：设置启用/未设置禁用，启动参数权威配置）、会话操作归属校验）、`@gebai/sdk`（GebaiClient：含 getCurrentSession、webhook/工具启停/打包下载方法）、`@gebai/web`（Vite 聊天 UI，按功能域模块化拆分：state/messages/tool-cards/sessions/composer/attachments/approvals/settings/auth/markdown/diagram/diff/html-view/jump-bottom 等，样式按 base/chat/composer/overlays 分片；**Inter / JetBrains Mono 字体内置**（@font-face 随产物分发，不依赖目标机器字体）；富内容块渲染、历史加载、**交互式图表编辑（Mermaid/PlantUML/D2/ECharts 四语言本地渲染 + 主题适配——PlantUML skinparam 注入与渲染后颜色兜底修正、Mermaid 按 UI 明暗重新 initialize、D2 按 UI 明暗选主题 ID 0/200、ECharts 按 UI 明暗注入 darkMode、标题/图例同顶冲突自动下移避让）**、**diff 并排对比视图（按语言语法高亮，跨行着色平衡）**、**HTML 页面沙箱渲染（预览卡片 + 全屏查看器 + 源码/复制/下载，`render_html` 工具产物）**、**多套 UI 风格（当时 9 套，现 10 套含 `qinhan`；除默认主题外各配画布环境高级特效）**、**低性能模式（纯手动开关，降级动画/毛玻璃特效与图表导出采样，不做硬件自动检测）**、**单轮计时器（任务运行期间消息上实时显示本轮耗时，外观 tab 可关）**、**人民币招财猫（`cny` 主题专属：悬浮可拖动玩偶 + 拖动金币轨迹 + 点击连击爆金币 + 单轮完成大爆发（运行越久越多）；全屏 canvas 粒子引擎（金币 3D 翻转 + 六面额纸币各半，预渲染精灵），低性能模式不发射）**、多用户登录页、会话重命名、压缩入口与压缩通知、设置面板（**浏览器本地环境变量增删改（localStorage，对本浏览器所有会话生效，随消息临时注入服务端、不落盘防泄露；服务端不配模型变量时仅前端配置即可使用；含 `GEBAI_LLM_EXTRA_PARAMS` 建议项，可按任务覆盖模型接口参数；一句话说明与保存按钮固定面板底部）**/外观性能模式/用户管理/反馈列表；工具启停、子Agent 装载与 Webhook 管理不设 UI，保留 SDK/API 方法）、**图表预览卡片随内容自适应**、**发送消息/切换会话自动锁定滚动到底（粘底跟随核心 `sticky-follow.ts` **意图驱动**——主消息列（`sticky-scroll.ts` 工厂 + 「跳到最新」按钮）/ 推理体（`reasoning-scroll.ts`）/ 子会话容器（`scrollSessionSticky`）三处共用同一工厂，可独立单测；「跳到最新」按钮显隐 = 跟随状态完全一致：按钮隐藏 = 跟随中（新内容持续滚动到底），按钮显示 = 用户在阅读历史不打扰。跟随状态只由三类信号翻转，**不做滚动事件位置取证**（目标位置比对 / 程序落位差值）——浏览器 scroll 事件异步合并送达，内容增长/收缩引发的钳制与布局调整同样产生滚动事件，取证式分类必被迟到事件误判为用户滚动、被过期落位误判为用户上翻（「自动滚动滚一段后失灵」的根因）：① **用户输入意图**（同步、必然先于其滚动效果到达，无竞态）：滚轮上滚 / 触摸上滑 / 向上滚动键（PageUp/ArrowUp/Home；输入框内方向键滚动的是文本光标不触发）/ 滚动条拖动（pointerdown 命中滚动条槽区：`offsetWidth-clientWidth` 宽度带）/ 消息导航跳转（`stopFollowing` 显式解除）→ 立即解除；② **几何贴底**：任何滚动事件落在阈值内（主列 64px / 容器 8px / 推理体 4px）→ 恢复跟随（滚回底部 / 收缩钳制到底收敛；小幅上滚在阈值内视为仍贴底继续跟随）；③ **静默窗口兜底**：未贴底且距最近程序滚动/DOM 变化超 80ms → 无法归因为内部动作，视为未知输入（中键自动滚动 / 查找定位 / 覆盖式滚动条拖动）→ 解除；窗口内的未贴底事件视为程序滚动/钳制的迟到事件：保持跟随并回正到底（续滚无需登记目标落位——旧实现须读回 clamp 后实际落位精确比对，且续滚自身不登记导致容器内跟随必现失效）。**窗口时间戳仅在跟随中刷新**——未跟随时内容变更（流式每 120ms 重解析）不再续窗：否则窗口对流式会话永不关闭，非滚轮类滚动输入每滚一下都被回正拽底（「滚动卡死」形态）。**粘底对齐保持（帧预算循环 240 帧）**沿用：内容高度存在不触发 MutationObserver 的异步修正（字体/图片加载等异步布局修正）——跟随期间按帧续查对齐（每帧仅属性读取、贴底即空转）、预算耗尽自停（几何不可用/NaN 时停转防死循环）；新内容（DOM 变化 MutationObserver / 图片加载含 markdown 内嵌 `<img>` 由 `msgEl` 委托捕获阶段 load 监听 / ResizeObserver）触发跟随，rAF 节流每帧至多一次保证流式高频更新性能；会话切回滚动位置恢复走 `restoreScroll`：落位后按几何同步跟随状态，未决跟随回调（排期中的 rAF/对齐保持循环）按执行时状态自然失效——loadMessages 尾部先排期 scrollIfSticky rAF 再 lockToBottom 后恢复历史位置，未决回调不把恢复位置拽到底部）**、**消息质量反馈（助手消息 👍/👎 提交反馈，设置面板反馈页可见）**、**会话导出（Markdown 下载）**、DOM 引用统一集中于 `state.ts` 与代码高亮复用（`highlightedCode`））、`@gebai/desktop`（服务端同进程 + 浏览器兜底宿主）。

- 测试：`bun run --cwd packages/server test`（分片并行）全量用例通过（核心引擎、命名空间、沙箱、压缩、Webhook、ZIP、协议契约、服务模式鉴权、飞书机器人全覆盖）。
- 已实现补充：**原生 WebView 桌面宿主**（`packages/desktop/launcher/`：tao 窗口 + wry WebView，无 Tauri 依赖；`include_bytes!` 内嵌 `gebai.exe`，物化到 `%LOCALAPPDATA%\gebai\` 后 spawn，stdout 解析端口导航 WebView，关窗回收侧车）、**playwright driver 打包闭环**（`packages/server/scripts/build-driver-embed.ts` 生成 `driver.embedded.generated.json` 内嵌产物，二进制运行时物化 `{GEBAI_HOME}/vendor/playwright/driver.mjs`）、**playwright-core 打包闭环与惰性桥接**（`packages/server/scripts/build-pwcore-embed.ts` 整树 gzip base64 内嵌、运行时物化 `{GEBAI_HOME}/vendor/playwright-core/`；桥接经 `createLazyBridge()` 全进程惰性单例，bundle 图内子Agent 模块禁止模块作用域第三方包解析；Windows 默认 `channel=msedge` 驱动系统 Edge，`GEBAI_PLAYWRIGHT_CHANNEL` 覆写）、**数据生命周期 GC**（`core/session/gc.ts`：会话 90 天闲置归档 `trash/`、`trash/` 7 天物理删除、`feedback/` 180 天清理、遗留用户级 `truncated/`（30 天宽限期后删除，非迁移）；启动即跑 + 每日周期，`GEBAI_GC_DISABLED=1` 关闭）、**子Agent 目录化**（`packages/agents/src/agents/{name}/{name}.ts` + `{name}.md` 提示词拆分与 ts 导入修饰）、**子Agent 打包闭环**（`packages/server/scripts/build-subagents.ts` 构建时生成 bundle 注册表，dist/二进制模式 `discover()` 回退加载，md 提示词随静态 import 内联进产物；playwright 的 node 桥接驱动 `driver.mjs` 复制到 dist/ 与产物同目录；**D2.js 打包闭环**——`packages/server/scripts/build-d2js.ts` 生成内嵌产物 `d2js.embedded.generated.json`（node-esm 构建 7 文件 gzip base64，静态 import 随产物打进二进制，运行时物化到 `{GEBAI_HOME}/vendor/d2js/{version}/` 供文件路径 Worker 运行；构建命令 `--external @terrastruct/d2`））、**定时任务**（`core/schedule/cron.ts` + `packages/agents/src/agents/task/task.ts` + `core/schedule/notify.ts`：`GEBAI_CRON_ENABLED` 开关（**默认开启**，显式 false 时 `cron` 子Agent 不注册、`cron_*` 工具完全不注册、REST 503）；`cron` 子Agent 命名空间暴露 `cron_add`/`cron_list`/`cron_update`/`cron_trigger`/`cron_remove` 工具（创建/修改/删除/手动触发默认需审批，自原全局工具下沉并补手动触发）；**用户级存储** `users/{user}/cron.json`（与会话生命周期解耦，会话内保存的旧版布局已弃用、启动遇之忽略）+ 任务专属工作目录；脚本运行 + 提示词运行 agent 两种类型，prompt 型执行目标 ephemeral/sticky/session 三态（独立新会话/专用会话复用/绑定会话，绑定会话删除自愈降级）；5 段 cron（可配 IANA 时区，日周 OR 语义）/`@every n{s,m,h,d}`/`@daily` 等/`@at` 一次性表达式；`misfire` 错过补跑、`timeoutMs` 执行超时（prompt 型 engine.cancel 终止）、`maxConsecutiveErrors` 连续失败自动停用、每任务执行记录按文件保留（上限 200 条）；通知通道（webhook/飞书群自定义机器人加签/飞书应用消息，`notifyOn` 时机，尽力而为不阻塞执行，密钥回显脱敏）；REST `/api/v1/cron` 管理面；30 秒 tick 调度；`event.cron.run`/`event.cron.result` 事件）、**多用户安全加固**（REST 用户管理端点管理员校验（与 WS 同权限，防提权）、`GEBAI_APPROVAL_SKIP` 用户本人可设置自己的会话（会话内存态 env 不落盘，写入只影响本人会话，非管理员仍受沙箱约束；ask 填值分支服务模式拒绝）、沙箱模式脚本子进程环境剔除敏感变量（`*_KEY`/`*_TOKEN`/`*_SECRET`/`PASSWORD`，防服务端密钥经 sh/py/cron 外泄）、`PATCH /api/v1/tools` 服务模式限管理员、**默认用户沙箱豁免**（**admin 用户为特权用户**，`Sandbox.isExempt`/`enforcedFor(user)` 按用户判定——豁免**仅本地模式生效**（`auth === "local"`），服务端部署下 admin 同受沙箱约束；普通用户恒受约束）、`store.load` 按用户索引 + 会话归属记录（修复旧版无 userId 会话跨用户命中））、**前端页面捕获 `page_capture`**（仿 show 图表分支前端配合链路：引擎发布 `event.capture.request` → 前端捕获当前页面渲染后 DOM html（截断 300KB）+ modern-screenshot 截图（png/jpeg，体积压缩 ≤2MB）→ WS `capture.result` 回传 → 落盘会话 `tmp/capture/` 并返回文件/图片块；`self_optimize` 重构为 **code 超集**（继承其全部工具（含并入 code 的 preview_server）+ page_capture/vision），vision provider 经 `setVisionProviderGetter` 注册点与主 Agent 共用解析逻辑；SDK `submitCaptureResult`；页面捕获超时 30 秒、整页截图高度上限 12000px）、**飞书机器人对话桥接**（`feishu-bot/`：自研极简 protobuf 帧编解码（pbbp2.Frame）+ 长连接协议层（endpoint 发现/心跳/分片合包/ACK/自动重连）+ 开放平台 API（tenant_access_token 缓存、消息发送/撤回、图片资源下载、会话信息）+ 桥接编排（会话映射当时为 `feishu_{chat_id}`，**现为 `sha256("feishu:"+chat_id)` 前 32 位 hex**；身份映射（多用户按 open_id 自动建户）、文本/图片消息处理、@提及剥离、流式增量预览（节流合并发新撤旧）+ 最终卡片（lark_md）、斜杠命令、文本命令式审批；`GEBAI_FEISHU_BOT_ENABLED` 开关，依赖全部注入；单测 **134 用例** + 真实凭证长连接握手验证通过）。
- 未完成（后续迭代）：子Agent 选择性打包的黑名单形态（构建期只有白名单 `GEBAI_BUILD_SUBAGENTS`，黑名单为**运行时** `GEBAI_SUB_AGENTS_DISABLE`）、飞书机器人 Webhook 回调模式、OIDC 身份对接。

### 稳定性保障：防「模型误改」安全网

自我优化（`self_optimize`）修改代码后，**测试是唯一准入凭证**：

- **修改前基线**：改动前先跑全量测试确认绿色基线，记录结果
- **修改后门槛**：任何代码变更必须通过相关测试（新增工具 → 工具测试；改提示词 → 契约/E2E 测试；改引擎 → 全量回归）才能落盘
- **回归守护**：全量测试含关键路径断言（审批流转、上下文压缩、命名空间解析、沙箱边界），大模型误改行为（破坏协议、绕过沙箱、错误状态机）会被测试直接捕获
- **自动回滚**：测试失败自动回滚本次改动，返回失败原因给模型引导修正（与「自我优化」章节的测试门槛一致）
- **防呆测试**：对脆弱逻辑（正则解析、路径拼接、哈希分片）编写**边界与对抗用例**（畸形输入、路径穿越、超长名、Unicode），确保模型改代码时不易踩碎隐性契约

### 代码设计原则

- **高度模块化**：核心模块无副作用、依赖显式注入，可独立实例化测试
- **接口优先**：Agent、Session、SubAgent 等核心概念通过接口定义，便于 mock
- **工具可测试**：全局工具和子Agent 工具的函数体为纯函数或可注入依赖的函数工厂
- **Code Agent 自主测试**：每个代码生成任务完成后，Agent 应能自主运行相关测试验证，测试命令明确、零配置（`bun test` 即可）
- **可伪造性**：LLM Provider、时间、文件系统均有测试替身（fake），测试不依赖真实网络/时钟/磁盘状态

## 常量参考

| 常量 | 值 | 说明 |
|------|-----|------|
| 模型调用重试次数 | 2 次 | 引擎层空响应/无产出异常重试上限（`LLM_RETRY_COUNT`）；provider fetch 层网络错误/429/5xx 重试 2 次 |
| 模型调用重试退避 | 800ms 基数 | 引擎层指数退避（800/1600/3200ms，`LLM_RETRY_BACKOFF_MS`，测试可注入 `retryBackoffMs` 加速重试用例）；provider 层 500ms 基数 |
| 工具返回截断阈值 | ~12000 字符 | 超出后按行截头尾（保留完整行，单行巨长按字符兜底），完整内容写入会话工作目录 `truncated/{tool}_{hash}.txt`（物理位置 `{session}/tmp/truncated/`，沙箱内可读），返回的相对路径在文件工具与脚本里都能直接用；**引擎在主循环兜底**：工具未自行截断的超长输出统一走该逻辑（`TRUNCATE_THRESHOLD`）；阈值适配现代大上下文窗口，膨胀防护由上层压缩器兜底 |
| 截断保留首/尾 | 各 4000 字符 | 截断消息保留的 head/tail 长度（`TRUNCATE_HEAD_CHARS`/`TRUNCATE_TAIL_CHARS`） |
| 计划文档目录 | `plans/` | ask 计划分支计划落盘目录（相对会话工作目录，物理位置 `{session}/tmp/plans/`，随会话文件面板可见；文件名按标题清洗（Unicode 字母/数字/下划线/连字符——**中文保留**，空回退 `plan`，长标题截断 60 字符），同标题重提计划覆盖更新） |
| 用户输入落盘阈值 | 12000 字符 | 超长用户输入发送时全文落盘会话工作目录 `user_inputs/{hash}.txt`（原文不丢，read 可读），消息正文保留头尾各 4000 字符 + 文件引用（`USER_INPUT_SPILL_THRESHOLD`/`USER_INPUT_SPILL_HEAD`/`USER_INPUT_SPILL_TAIL`，见「上下文保护」） |
| grep 单文件读取上限 | 1MB | 超出跳过该文件（防大文件/二进制拖慢搜索） |
| 测试分片默认数 | `min(8, max(2, CPU 核数))` | `packages/server/scripts/test-parallel.ts` 分片数（`--shards=N`/`GEBAI_TEST_SHARDS` 覆盖）；LPT 按文件字节数装箱均衡负载 |
| grep 匹配子进程超时 | 20 秒 | 正则匹配在独立子进程执行（灾难性回溯防护），超时强杀并返回引导（`GREP_MATCHER_TIMEOUT_MS`）；行数据按 4MB 批量送子进程 |
| edit 正则匹配超时 | 20 秒 | `edit` 的 `pattern` 项在独立子进程执行（灾难性回溯防护），超时强杀并返回引导（`REGEX_MATCHER_TIMEOUT_MS`）；单次匹配上限 1000 处（`REGEX_MAX_MATCHES`，超限拒绝） |
| read/edit 文件大小上限 | 8MB / 5MB | 全量读入内存前 stat 预检（GB 级文件直接 OOM），超限引导 offset/limit 分段或 grep 定位后小范围修改（`READ_MAX_FILE_BYTES`/`EDIT_MAX_FILE_BYTES`） |
| 文本编码探测 | BOM / UTF-16 LE·BE / UTF-8 / GBK | `core/base/file-text.ts`：BOM 优先→无 BOM UTF-16 启发式（NUL 占比 ≥30% 且解码无控制字符）→UTF-8 严格解码（控制字符占比 ≤30%）→GBK（严格解码成功且含 CJK）；均不命中按二进制拒绝；GBK 无编码表，仅支持纯 ASCII 的字节级替换 |
| fetch_url 流式读取上限 | 10MB | 响应体流式限量读取（下载后截断防不住内存：超大/无限流在截断前已全量入内存，`FETCH_URL_STREAM_MAX_BYTES`） |
| js 子进程日志总量上限 | 2MB 字符 / 4MB 单行缓冲 | 父进程侧日志总量封顶（超时只杀子进程不回收已累积内容，`JS_LOG_TOTAL_CAP`）；无换行巨流的 stdoutBuf 硬上限（`JS_STDOUT_BUF_CAP`，行长检查仅在遇换行时生效） |
| 会话 env 内存缓存 | 256 会话 | `envCache` LRU 上限（命中刷新位置，防长生命周期进程无界增长） |
| 先到决策/选择/环境值排队上限 | 64 条/任务 | `pendingDecisions`/`pendingChoices`/`pendingEnvRequests` 容量（随机 id 无界堆积的内存放大防护，`PENDING_QUEUE_LIMIT`） |
| OAuth pending 授权有效期/上限 | 10 分钟 / 32 条 | 飞书 user_access_token 授权 state 的 TTL 与容量（未完成授权的条目含 appSecret 明文，按 TTL 惰性清理+超容量淘汰） |
| grep 最大匹配行 | 200 行 | 达到即停止，返回结果附「已达匹配上限」提示（content/files/count 三形态同一口径）；`head_limit` 参数可压低先看一部分（`data.truncated` 标记截断）；`context` 参数 0-10 行；`literal:true` 字面匹配（正则元字符自动转义）；`include`/`exclude` 支持逗号多模式与 `{a,b}` 花括号 |
| grep/glob 默认排除目录 | `WALK_SKIP_DIRS` 14 项 | node_modules/.git/dist/build/.next/.cache/__pycache__/.venv/venv/target/.idea/.vscode/coverage/.turbo——grep/glob 结果默认跳过（与 `walkDirFiles` 目录遍历同一清单（**单一来源**：`@gebai/sdk/node` 的 `walk.ts`，server 侧仅重导出）：会话 `tmp/` 列表场景的防噪兜底）；include/pattern 原文显式点名该目录时不排除 |
| 已读登记上限 | 2000 条/会话 | 防盲写守卫（write/edit）的会话级已读集合上限（`READ_TRACK_CAP`，超出整表重置——守卫降级为「需重读」）；会话删除时释放（`engine.forgetSession`） |
| patch 匹配容错 | 裁剪 3 行 / 消歧 30 行 / 候选 200 处 / 诊断 3 条 | 上方 patch 章节的三层参数：头尾上下文裁剪上限（`PATCH_FUZZ_LINES`）、多候选时声明行号的可信距离上限（`PATCH_ANCHOR_TOLERANCE`，超出按歧义报错）、单处 hunk 候选位置收集上限（`PATCH_MAX_CANDIDATES`）、失败诊断中列出的相近位置条数（`PATCH_DIAG_CANDIDATES`） |
| search_symbols 单文件上限 | 1MB | 超出跳过该文件（与 grep 同级） |
| search_symbols 扫描上限 | 500 个文件 | 最多扫描文件数（内容预筛后 tree-sitter 解析）；references 模式同限（叶子节点精确匹配 + 定义名排除，单树遍历节点数上限 20 万防巨文件拖慢） |
| search_symbols 匹配上限 | 50 条 | 达到即停止，精确匹配优先排序 |
| git log 条数 | 10 条 / 50 条 | 默认条数 / 上限（`max_entries` 参数可调） |
| file copy 复制上限 | 100MB | `file` copy 动作的二进制整读整写上限（`FILE_COPY_MAX_BYTES`，超限引导用 sh 复制）；mkdir 递归创建、已存在幂等 |
| URL 抓取响应上限 | 200KB | `fetch_url` 超出截断（含截断落盘），防内存膨胀 |
| URL 抓取超时 | 15 秒 | `fetch_url` 单次请求上限 |
| 重定向跳数上限 | 5 跳 | `fetch_url`/`http_request` 重定向逐跳校验的最大跟随次数（防重定向循环与跳板链）；`capture_replay` 沙箱模式不自动跟随重定向（3xx 返回 Location 逐跳重放） |
| 浏览器网络录制条数 | 500 条/会话 | playwright driver 网络录制上限（超出丢弃最旧；响应体预览 20KB/条，超 200KB 不捕获；`network_list` 默认返回最近 200 条、limit 参数最高 500） |
| 浏览器 WS 帧录制 | 300 帧/会话，单帧预览 4KB | WebSocket 帧录制上限（随 capture 录制开关联动；`capture_ws` last 参数默认 100、上限 300） |
| 浏览器请求拦截规则 | 32 条/会话 | `reverse_site_route` 拦截规则上限（超出提示先 clear；上下文重建后自动重挂） |
| 浏览器响应体提取上限 | 20MB | `capture_body` 完整响应体落盘上限（文本预览 200KB 截断，超长/二进制走 file 参数直接写盘）；透明浏览器代理（`GEBAI_BROWSER_PROXY`）的单响应体临时文件中转同限，单次代理请求超时 110s |
| 浏览器对话框/下载记录 | 100 条/会话 | dialog 记录（list 默认返回最近 50）与下载记录上限（下载文件保留在系统临时目录，仅记录淘汰） |
| 上下文压缩触发 | 窗口剩余 < 一次回复的输出预留 | **不以窗口百分比判定**——真正约束是「留给输出的空间」：窗口剩余（`maxContextTokens - 最近一次真实 input tokens`）不足以支撑一次回复（输出预留 = 模型单次响应输出上限 `maxOutputTokens`，即 `GEBAI_LLM_MAX_OUTPUT_TOKENS`；未声明时缺省 16384，并夹在 `[1024, 窗口一半]` 内）时触发压缩（`outputReserveTokens`/`lacksOutputRoom`）（见「上下文保护」） |
| 上下文压缩目标 | **窗口 × 40%**（下水位，压到即停） | 上水位（触发）= 剩余 < 输出预留（见上行）；下水位（目标）= `窗口 × COMPACT_TARGET_RATIO`（缺省 0.4，`GEBAI_COMPACT_TARGET_RATIO` 可调，合法区间 0.1~0.9），且不高于触发线（配置矛盾时以触发线为准）——需腾出量 = 真实基线 − 目标输入，按 `estTotal/基线` 比率折算为估算口径后逐条累计（比率越界退回 1:1，`COMPACT_EST_SCALE_MIN/MAX`）；可压消息不够（窗口外全压完仍高于目标）时压满窗口外，不足部分交给迭代压缩与溢出恢复；基线已低于目标（如接口已报溢出）时至少腾挪窗口 5%（`COMPACT_MIN_ROOM_RATIO`） |
| 上下文压缩可压缩口径 | 除系统提示词外全部 | `isCompressibleMessage`：**仅 role=system 不可压缩**（系统提示词/装载提示词区间夹带时原位保留）；用户输入、assistant、tool 结果、引擎注入提醒进区间后被摘要替换并**完全移除**（滑动窗口内的近消息不入区间）；子会话运行存档（session/subAgent 标记）不进主上下文也不动。与超限截断口径（`isProtectedMessage`：user/system/存档不做无摘要丢弃）分开 |
| 上下文占用口径 | 模型服务返回的 usage 真值 | 压缩判定只认接口返回的 `input_tokens`（含 system 提示词与工具 schema）：run 前按上次调用持久化基线、任务中途按每轮真实 usage（窗口剩余 < 输出预留时压缩，见上行）、接口上下文长度 4xx 拒绝时压缩重试（溢出恢复）。**基线每轮即时落盘**——每轮真值到达时 `store.updateCtxStats` 一并写入真实 `input_tokens` 与索引锚点（随后任意一次正文落盘即进 chat.json，不只任务结束）：任务中断/进程重启后，下一次 run 的压缩判定与列表展示仍有真值可用，不退回估算。**展示口径与真值同点位同步**：每轮真值到达时推送 `event.session.ctx` 并调 `store.updateCtxStats` 落盘（只写 meta.json，不重写正文）——列表/快照/刷新与推送一致；压缩与护栏降级改写历史时把展示值重算为当前消息估算（旧数不得残留）。估算（`estimateCharsTokens`，CJK 约 1 token/字、ASCII 约 4 字符/token；**多模态图片块按张常量**——`estimateContentTokens`/`IMAGE_TOKEN_ESTIMATE`，内联块携 base64，按序列化长度折算会把一张 1MB 图片算成 30 万 token）用于压缩量规划（`estimateMessageTokens`，真值口径的需腾出量按 `estTotal/基线` 比率折算）与会话列表 ctxTokens 展示的增量补足（`estimateCtxTokens`/`estimateTokens` 均已计入工具 schema 段估算 `estimateSchemasTokens`）（见「上下文保护」）。缓存命中（`cachedTokens`/`ctxCachedTokens`）为纯展示口径，随 usage 基线同点位建立/清除（见「自动压缩」的缓存命中度量） |
| 压缩保留最近下限 | **滑动窗口 12 条消息**（任意角色） | 近消息窗口（`COMPACT_WINDOW_MESSAGES`，`GEBAI_COMPACT_WINDOW` 可调）：最近这么多条消息永不进压缩区间（原样保留），随新消息自然向前滑动；上限为历史一半（`floor(len/2)`）——否则短会话永远压不动。有真实占用基线时窗口仅为**保底下限**（压多少由水位算出的需腾出量决定，实际会话一般不会触及窗口）；无基线（老会话/接口不返回 usage/窗口未知）时退保守口径：只压掉窗口外可压缩消息的一半 |
| 摘要输入/输出上限 | 单块 20000 / 总覆盖 12 万 / 输出 2000 字符；请求预留 8192 tokens | **首选缓存友好前缀请求**（与主循环逐字节同前缀的原文 + 尾部压缩指令，不分块；装不下时才算超预算）；骨架路径：单块输入预算（`SUMMARY_INPUT_LIMIT`）、块数上限 6（`SUMMARY_MAX_CHUNKS`，超出时头尾保留 + 中部省略说明）、输出上限（`SUMMARY_OUTPUT_LIMIT`）、单条骨架 600 字符（`SUMMARY_ITEM_LIMIT`）、降级骨架 15 行 × 120 字符（`SUMMARY_FALLBACK_LINES`/`SUMMARY_FALLBACK_ITEM_LIMIT`）；前缀请求可行性预判用 `SUMMARY_OUTPUT_RESERVE_TOKENS`（8192：摘要输出 + 工具 schema 段开销）
| 工具调用轮次上限 | 不限制 | 单次任务内模型工具调用轮次无上限（超长任务不截停）；失控防护由重复检测终止/用户取消/上下文压缩承担，`rounds` 仅计数回传 |
| 重复检测窗口 | 最近 8 次调用 | 工具调用签名（工具名+参数 JSON）滚动窗口（`MAX_REPEAT_WINDOW`） |
| 重复检测命中阈值 | 连续 3 次 | 窗口**尾部连续**出现相同签名第 3 次（其间无任何其他调用）才判定为无效重复，中断该次执行并注入引导提示（`MAX_REPEAT_HITS`）；间隔其他调用后重发同签名不累积（「改动后复查」合法）；**同批重复签名只记录一次**（同批相同调用是有意扇出，跨轮连续重发才累积） |
| 重复中断上限 | 2 次 | 重复中断超过该值终止工具循环（`MAX_REPEAT_STALLS`），仍返回最后产出文本 |
| 同批工具并行上限 | 8 个 | 单次模型响应返回的多个工具调用并行执行的并发护栏（`MAX_PARALLEL_TOOLS`，超出按调用顺序排队）；需严格串行的操作由模型用 js 脚本编排或拆分多轮（见「同批工具并行执行」） |
| 审批超时 | 5 分钟 | 审批请求等待上限，超时自动拒绝并提示模型调整 |
| 脚本执行超时 | 5 分钟 | `py`/`js` 单次执行上限（超时杀进程并返回 `[timed out after ...]` 结果给模型，不结束任务）；`timeout` 参数可按次调整（秒，默认 300、上限 540，不晚于工具执行超时兜底）。`sh` 的 `timeout` 是**同步等待窗口**而非杀进程上限（见「sh 执行」） |
| sh 同步等待窗口 | 默认 60 秒 / 上限 120 秒 | `sh` 同步调用在后台任务上的等待上限（`shWaitMs`）——窗口到期命令仍在运行则转后台返回 taskId，**不终止命令**；等待轮询 20ms 起步退避到 300ms（`SH_TASK_POLL_MIN_MS`）。同步回退路径（安全模式 / 无后台服务 / 后台并发已满）下同值为杀进程上限 |
| sh 任务生命周期 | 默认 30 分钟 / 上限 60 分钟 | 后台任务生命周期上限（status/wait/list/kill 时惰性检查，超限终止并标记 timed_out；`async:true` 下由 `timeout` 参数调整，`shTaskLifetimeMs`） |
| sh 任务并发上限 | 8 个/会话 | 同时运行的后台任务数上限（`SH_TASK_MAX_CONCURRENT`，超限拒绝新任务并引导清理；同步调用遇限回退同步执行） |
| sh 任务存储 | `tmp/sh-tasks/` | 记录 `tasks.json`（原子写）+ 每任务 `{id}.log` 合并输出（stdout+stderr）；任务 id 形如 `t` + 8 位 hex。**窗口内结束的同步任务不留痕**（记录与日志删除），转后台/输出截断的任务保留 |
| sh 输出旁路捕获上限 | 4MB/流 | 前台等待路径的 `onChunk` 捕获上限（`SH_TASK_CAPTURE_CAP`）——超限停止捕获，且该任务保留记录与日志供回查 |
| bg_task 命令任务输出尾部 | 默认 4000 / 上限 20000 字符 | status/wait 返回的日志尾部字符数（`tail` 参数可调，完整日志 `sh-tasks/{id}.log`，相对会话工作目录） |
| bg_task 等待默认/上限 | 60 / 60 秒 | `action=wait` 阻塞等待上限（三类后台任务同口径，轮询 300ms）——**上限压到 1 分钟强制按进度轮询**：等满上限而不查过程是无收益的空耗，超时返回当前状态与进度、需要继续等再次 wait（子会话运行的完成报告落存档、继承形态自动合入父上下文，都不会因等待超时丢失） |
| 子会话并发上限 | 8 个/会话 | 同时运行的子会话数上限（`SUBSESSION_MAX_CONCURRENT`，**全树合计**——子会话内再派生同样计入，超限拒绝新运行并引导清理；另：单次调用子会话数上限 8（`SUBSESSION_MAX_PER_CALL`）） |
| 子会话终态保留 | 最近 20 条/会话 | 已结束子会话的记录保留上限（`SUBSESSION_KEEP`，超出淘汰最旧；运行中不淘汰；进程内不落盘，重启即中断） |
| 子会话命名与 runId | `s1..sN` / `s` + 8 位 hex | 子会话名缺省按批内序号 `s1..sN`（调用方可传 `name`，批内唯一、≤32 字符不含空白——中文名合法）；runId 形如 `s` + 8 位 hex（`bg_task` 按前缀分发） |
| 子会话报告合入上限 | 16000 字符 | 合入父上下文的子会话报告长度上限（超出保留头尾 + 省略说明，`SUBSESSION_MERGE_MAX_CHARS`——纯上下文保护，无落盘兜底）；**摘要合入阈值** 1500 字符（`SUBSESSION_MERGE_SUMMARY_SKIP_CHARS`，`merge=summary` 时报告超过该值才触发模型摘要，短报告原文合入）；父会话/子会话通知（互相感知注入）上限 2000 字符（`SUBSESSION_NOTICE_MAX_CHARS`，保留头部） |
| 子会话快速结束宽限 | 120 秒（夹取 1s~600s） | 收敛指令注入后留给模型输出结论的时间（`SUBSESSION_FINISH_GRACE_MS` 缺省，传值夹取到 `SUBSESSION_FINISH_GRACE_MIN_MS`~`SUBSESSION_FINISH_GRACE_MAX_MS`）；逾期强制终止；触发入口见「子会话快速结束」 |
| 工具执行超时兜底 | 9 分钟 | 引擎层兜底（`TOOL_TIMEOUT_MS`，可注入）：覆盖不响应超时的工具（如网络请求挂起）；超时不结束任务，结果作为「执行超时」返回模型自行调整（子Agent 内挂起工具同样受此保护） |
| 用户中断宽限期 | 300 毫秒 | 取消后等待工具自身交回中断返回的上限（`TOOL_ABORT_GRACE_MS`，引擎选项 `cancelGraceMs` 可注入）：宽限内交回则其真实返回并入工具结果，未交回则以统一中断标记即时收口（不拖慢取消；见「中断与取消」） |
| 长工具执行心跳 | 25 秒 | 工具执行期间周期发布 `event.tool.alive` 刷新前端空闲看门狗（`TOOL_HEARTBEAT_MS`，测试可注入 `heartbeatMs`；见「等待期不误判挂起」） |
| 交互等待心跳 | 25 秒 | 等待用户作答（审批/选择/填值/画图/捕获）期间周期发布 `event.interaction.alive`（`INTERACTION_ALIVE_MS`，等待结束即停）；前端看门狗另按「已连接 + 无待决交互」两重豁免，断线或长时间思考不得杀后台任务 |
| 任务受理宽限窗口 | 15 秒 | SDK 任务受理后不据快照判定任务已结束（排除引擎登记运行态前的起跑竞态）；构造函数选项 `acceptSettleMs` 可调（测试） |
| 子Agent 调用超时 | 不设 | 子Agent 调用不设整体超时（执行进度实时可见，中止仅由父任务取消传播；`SUBAGENT_TIMEOUT` 已移除） |
| 子Agent 递归深度 | 3 层 | 子Agent 嵌套调用最大深度 |
| 待办续做轮次上限 | 1 轮 | 会话完成时仍有 `pending`/`in_progress` 待办，注入**模拟 todo 查询工具调用对**（assistant(toolCalls)+tool，`engineNote: "todo"` 留在 tool 消息）继续会话的轮次上限（`MAX_TODO_CONTINUE`，达到即停止；模型对注入的纯文本回应视为决策收尾，不再注入） |
| 收尾验证提醒轮次上限 | 1 轮 | 任务修改了代码文件但未识别到验证时，结束注入验证提醒的轮次上限（`MAX_VERIFY_NUDGE`，防反复打扰）；代码文件按扩展名判定，**会话工作区（`tmp/`）内的文件不计入**（临时产物/测试夹具，非代码改动，见「收尾验证提醒」）；验证识别通道（sh/py 命令口径四类——主流 runner/通用构建入口/自研工具链 `st` 子命令/`×test` 形态脚本名、验证类工具名后缀 `run_tests/test(s)/verify/check(s)/lint` 含命名空间形态、js 编排的「命令调用点（含 `tools.×(`）+ 关键词」组合）均按关键词宽匹配（宁漏勿紧——误判已验证只少一次提醒）；子会话内执行经共享通道归入父会话判定 |
| 预览服务就绪超时 | 15 秒 | `preview_server` 启动后 TCP 就绪探测总时限（超时即终止并回显日志尾部） |
| 预览服务轮询间隔 | 300ms | 就绪探测轮询间隔 |
| 预览服务日志/状态 | `os.tmpdir()/gebai-preview-{port}.log`、`gebai-preview.json` | 独立进程 stdout/stderr 日志与运行状态（port/pid/url），状态文件按 PID 存活清理 |
| 垃圾回收保留期 | 7 天 | `trash/` 中删除/过期数据保留时长（已实现：GC 每日执行） |
| 反馈数据保留期 | 180 天 | `feedback/` 反馈保留时长（已实现） |
| 会话闲置过期 | 90 天 | 无活跃会话归档到 `trash/` 的时间（按 `chat.json` mtime 判定，已实现） |
| GC 周期 | 24 小时 | 数据生命周期清理任务执行周期（启动时立即执行一次） |
| WS 事件日志容量 | 1000 条 | 每用户事件日志上限（`JOURNAL_CAP`，超出丢最旧；重放出现缺口时客户端触发全量重同步） |
| 注册端点限流 | 30 突发/0.5每秒 + 10 突发/0.1每秒 | 注册独立令牌桶（全局桶 + 来源桶，`routes/auth.ts`） |
| 消息缓存上限 | 2000 条（超限单次裁到低水位 1800 条） | 会话消息**持久化**上限——**纯存储安全网，不参与压缩判定**（上下文保护只认 token 水位口径）：超限**按批裁剪**（`TRIM_LOW_WATER_MESSAGES`=上限×0.9，一次裁到低水位、单批腾出 `TRIM_BATCH_MESSAGES`=200 条余量）丢最早的非保护消息，按 tool_call 配对原子丢弃（连带丢弃的 tool 结果计入移除数）；系统提示词/用户输入/存档消息原位保留（见「上下文保护」）。按批裁剪是为**少改上下文前缀**（服务端前缀缓存）：逐条挤出等于每条新消息都改一次前缀，按批裁剪下前缀每 200 条消息才变一次。取值高于水位口径可达的条数（128k 窗口、输出预留 16384、system 提示词与工具 schema 约 13k 时，撞水位需平均约 50 token/条以上），正常会话的提前裁剪由压缩承担。**代价**：`store.save` 每条消息全量重写 `chat.json`——实测中位约 2.9KB/条，本上限下单个会话文件可达数 MB（写放大随消息数线性增长） |
| 桌面固定端口 | 47896 | 桌面形态默认监听端口（`DESKTOP_PORT`，见「端口固定」） |
| Session 缓存 LRU | 10 个 | 会话列表 LRU 驱逐上限 |
| 截断内容哈希 | SHA256 | 基于完整返回内容计算，用于去重和文件命名 |
| 图片压缩上限 | 1280px / 2MB | 发送给大模型前的服务端压缩阈值（`@gebai/agents` core/shared/image-resize.ts，长边/体积超限等比缩放后同格式重编码，JPEG/WebP 质量 0.85；原图保存不压缩，见「图片压缩时序」） |
| analyze 超时 | 30 秒（钳制 1~300） | vision 子代理 `analyze` 的 `timeout` 参数默认值；超时返回提示引导改用本地视觉工具 |
| 视觉工具图片上限 | 8MB | 视觉分析（vision_analyze）单张图片大小上限（超出提示压缩后重试） |
| 页面捕获等待超时 | 30 秒 | `page_capture` 工具等待前端捕获回传的最长时间（超时返回「页面捕获失败」提示） |
| 页面捕获 html 上限 | 300KB | 前端捕获 DOM html 的传输/落盘截断长度（超出截取首部，完整结构可用 `read` 分文件读取） |
| 页面捕获截图上限 | 1600px / 2MB | 前端截图输出长边与体积上限（超限 JPEG 重编码/等比缩放降质；png/jpeg） |
| 整页截图高度上限 | 12000px | fullPage 截图最大高度（canvas 尺寸上限保护，超出截取顶部） |
| 附件图片内联上限 | 8MB | 主模型多模态内联附件图片的 base64 大小上限（超出降级为文本说明） |
| 历史图片内联窗口 | 最近 3 组 | 仅最近 3 组含图片的消息（用户图片附件与工具结果图片——read 读取的图片引用）内联进上下文，更早的降级为路径说明（图片不参与压缩，长会话防图片占死窗口；`INLINE_IMAGE_RECENT`） |
| CV 输入图像上限 | 8MB | 本地识别工具（desktop_ocr/locate/detect）输入 PNG 大小上限（同附件内联上限，超出报错引导压缩） |
| CV 输入降采样上限 | 长边 1280px | `GEBAI_CV_MAX_SIDE` 可调；超过等比缩小后识别，坐标仍映射回原始像素系 |
| OCR det 输入上限 | 长边 960（32 倍数） | det 前处理等比缩放 + 零填充（`DET_MAX_SIDE`，wasm CPU 亚秒级） |
| OCR rec 输入尺寸 | 48 高 × 320 宽 | rec 前处理等比缩放到 48 高、右侧零填充至 320 宽（`REC_HEIGHT`/`REC_WIDTH`，CTC 忽略尾部填充时间步）；**形状固定 → 可拼批**（见「OCR rec 批处理」） |
| OCR 输出行数上限 | 200 行 | desktop_ocr 输出超过截断，提示 find 参数过滤收窄 |
| YOLO letterbox / NMS | 640 边、IoU 0.45、conf 0.25 | desktop_detect 前处理边长 / NMS 阈值 / 置信度下限（conf 参数可调） |
| 模板匹配置信度阈值 | 0.8 | desktop_locate_image 默认 NCC 相似度阈值（threshold 参数可调；内部粗扫门限 0.4 仅作精化引导）；返回候选至多 5 个 |
| wait_for 轮询默认 | 超时 20s（上限 120）/ 间隔 2s / 变化判定差 2/255 | desktop_wait_for 默认参数（timeout_s/interval_s 可调）；change 模式灰度采样均差超阈值判定画面变化，超时不视为错误并返回最后观察状态 |
| LLM 流式读空闲超时 | 120 秒 | SSE 建立后连续无 chunk 判定接口假死中止本次调用（`LLM_IDLE_TIMEOUT_MS`，测试可注入 `llmIdleTimeoutMs`） |
| 模型单次响应输出上限 | anthropic 缺省 8192 / 其余接口缺省 | 单次响应输出 token 上限（`GEBAI_LLM_MAX_OUTPUT_TOKENS` 启动/任务级可配）：输出超限截断由引擎检测（`length`/`max_tokens`/Responses `incomplete`）并抢救落盘 + 引导 `write append` 分段续写（见「核心Agent流程」大文件分段写入与截断抢救）；Anthropic 接口强制要求 `max_tokens` 故有内置缺省 |
| 溢出护栏裁剪下限 | 500 字符 | 用户消息超过该长度才可被护栏裁剪为占位（短消息裁剪无收益；最新一条用户消息永不裁剪）；**每次降级经 `event.message.compact` 发布 `degraded` 事件**（UI 可见上下文为何变化，不再只写 console.warn） |
| subsession_run 子Agent 上限 | 5 个 | 单次子会话运行可预加载的子Agent 数量上限（去重后判定，`MAX_AGENTS_PER_RUN`） |
| WS 事件日志持久化 | `users/{user}/ws-journal.jsonl` | 日志尾部 JSONL 追加持久化（每 2000 条重写裁剪），重启后 seq 连续 |
| delta 合并窗口 | 50ms / 200KB 上限 | 同一消息的连续文本增量合并为单条日志事件（`DELTA_MERGE_MS`/`DELTA_MERGE_MAX_CHARS`） |
| WS 发送缓冲上限 | 16MB | 单连接发送缓冲超限判定慢客户端并断开（走自动重连 + seq 重放收敛，`WS_MAX_BUFFERED`） |
| 登录 IP 限流 | 60 突发/2每秒 + 10 突发/0.2每秒 | 登录/兑换端点全局桶与来源桶（`GEBAI_TRUST_PROXY=true` 按 X-Forwarded-For 区分来源） |
| 附件大小上限 | 无强制上限 | 附件上传端点**未做尺寸判定**（原文「20MB」无对应实现；代码中仅飞书图片 20MB 与浏览器桥响应体 20MB 两处，属不同场景） |
| 任务 tick 周期 | 30 秒 | 调度器检查周期（`TASK_TICK_INTERVAL_MS`：到期入队 + 队列推进） |
| 任务并发额度 | 每用户 5 | `GEBAI_TASK_MAX_CONCURRENT` 可调（`TASK_MAX_CONCURRENT_DEFAULT`）；定时/普通任务并行上限，闲时任务另受「队列空闲 + 每用户仅 1 个」约束 |
| 任务执行超时 | 脚本 5 分钟 / 提示词 30 分钟 / 闲时 3 小时 | 单次执行缺省上限（`TASK_SCRIPT_TIMEOUT_MS`/`TASK_PROMPT_TIMEOUT_MS`/`TASK_IDLE_TIMEOUT_MS`，任务 `timeoutMs` 可覆盖，范围 1s~24h；提示词型到时 `engine.windDown`——先快速结束运行中的子会话拿结论，再取消会话任务） |
| 任务超时上下限 | 1 秒 / 24 小时 | `timeoutMs` 合法区间（`TASK_TIMEOUT_MIN_MS`/`TASK_TIMEOUT_MAX_MS`） |
| 任务输出保留 | 4000 / 8000 字符 | 任务记录保留输出长度 / 写入会话消息的脚本输出上限 |
| 任务执行记录 | 每任务 200 条 | 执行记录按文件落盘，保留上限 `TASK_RUNS_KEEP`（超出按时间删最旧）；`TASK_RUNS_HISTORY`（10）仅为展示默认条数；单记录文件读取上限 1 MB（`TASK_RUN_FILE_MAX_BYTES`） |
| 任务名长度上限 | 100 字符 | `TASK_NAME_MAX`（单用户条数上限 500，`TASK_MAX_ITEMS`） |
| 任务资源文件上限 | 4 MB | 单文件写入/读取上限（`TASK_FILE_MAX_BYTES`），递归列目录深度上限 6 |
| 用户待办失败处理 | 一次失败即关闭闲时运行 | 待办闲时自动执行失败（含超时）即 `idle=false`、`idleState=failed`、`idleError` 记因并停用绑定任务——不自动重试，重新开启 ⚡ 重置状态后继续 |
| 任务通知正文/投递 | 2000 字符 / 10 秒 | 通知卡片单字段（执行结果摘要的输出/错误、`task_notify` 主动通知正文）的保留长度（`NOTIFY_TEXT_MAX`；卡片整体限 12000——`NOTIFY_CARD_MAX`，1.0 lark_md / 2.0 markdown 组件上限，与对话桥接 `truncateForFeishu` 同额）/ 通知 HTTP 投递超时（`NOTIFY_TIMEOUT_MS`） |
| show html 预览尺寸上限 | 4000 × 2000 px | `width`/`height` 显式预览尺寸上限，超限忽略回退默认 |
| 脚本桥调用总数上限 | 100 | 单次脚本（js/py 桥）内工具调用总数（`BRIDGE_TOOL_MAX_CALLS`；js 侧别名 `JS_TOOL_MAX_CALLS`） |
| 脚本桥字段截断 | 100k 字符 | 脚本桥单字段（output/data）截断（`BRIDGE_FIELD_CAP`，js 侧别名 `JS_RPC_FIELD_CAP`）；内层 blocks 透传上限 10（`BRIDGE_BLOCKS_CAP`） |
| py 桥常量 | 连接 8s / stdout 2MB / 协议行 2.5MB | 桥连接建立超时（`PY_BRIDGE_CONNECT_TIMEOUT_MS`，超时即降级纯脚本执行）/ stdout 捕获上限（`PY_STDOUT_CAP`，超出丢弃留注）与 stderr 尾部 8k（`PY_STDERR_TAIL`）/ 单条协议行上限（`PY_PROTOCOL_LINE_CAP`）；data 字段截断 100k（`PY_DATA_TEXT_CAP`）、返回值预览 2000（`PY_RESULT_PREVIEW_CHARS`） |
| js data 截断 | 100k 字符 | js 结构化 data 中 logs/result 截断（`JS_DATA_TEXT_CAP`，与 sh/py 对齐）；返回值预览 2000 字符（`JS_RESULT_PREVIEW_CHARS`） |
| js RPC 协议行上限 | 子进程 2MB / 服务端 2.5MB | 协议行截断兜底（防巨对象撑爆内存，超出注明截断） |
| js 会话上下文注入 | 最近 50 条 / 单条 2000 字符 | ctx.messages 快照条数与单条内容上限（`JS_CONTEXT_MESSAGES_MAX`/`JS_CONTEXT_MESSAGE_CHARS`） |
| js 动态工具上限 | 50 / 会话 | 会话内运行时定义工具数量上限（防注册风暴，`DYNAMIC_TOOLS_CAP`）；定义清单随会话 chat.json 落盘、重启恢复 |
| js 动态工具源码上限 | 100k 字符 | execute 源码长度上限（`JS_DYNAMIC_SOURCE_CAP`，源码随会话持久化，防撑爆 chat.json） |
| js 动态工具名 | `[a-z][a-z0-9_]{0,39}` | 运行时定义工具命名约束（与全局工具命名一致，`DYNAMIC_TOOL_NAME_RE`）；execute 源码 ≤ 2000 字符描述 |
| sh/py 结构化 data 文本上限 | 100k 字符 | `data.stdout`/`data.stderr` 超长截断（完整文本以 output 截断文件为准，`SCRIPT_DATA_TEXT_CAP`） |
| 大小模型协同默认值 | 阈值 0.85 / 证据下限 6 字符 / 提醒 3 次 / max_tokens 32768 / 并发按端点分档（远程 4 / 本机 1）/ 重试 1 次（仅传输类）/ **escalate 开** / **结果全量** / **mode=sync** | `core/triage` 缺省（两条通道同源）：置信度阈值（`TRIAGE_DEFAULT_THRESHOLD`）、证据质量下限（`TRIAGE_DEFAULT_MIN_EVIDENCE_CHARS`，挡住「任务失败」这类无信息量证据）、未调用输出工具的提醒上限（`DEFAULT_TOOL_REMINDERS`）、单条输出 token（`TRIAGE_DEFAULT_L1_MAX_TOKENS` = 32768，`GEBAI_TRIAGE_L1_MAX_TOKENS` / `l1_max_tokens` 可配；**实测缺省 300 会因预算不够截断工具参数、整批报「参数不是合法 JSON」**）、**并发**（`TRIAGE_DEFAULT_CONCURRENCY` = 1 本机 / `TRIAGE_DEFAULT_CONCURRENCY_REMOTE` = 4 远程；`GEBAI_TRIAGE_L1_CONCURRENCY` 可全局覆写）、**L1 重试**（`TRIAGE_DEFAULT_L1_RETRIES` = 1，**只重试传输类失败**：网络/超时/429/5xx；退避 `TRIAGE_DEFAULT_L1_RETRY_DELAY_MS` = 500ms）、**全局在途上限**（`GEBAI_TRIAGE_L1_GATE`；缺省取同端点各任务并发度的最大值——单任务 c8 就是 8，两个 c8 任务共享 8 并轮转）、低置信度是否上升兜底（`TRIAGE_DEFAULT_ESCALATE`）、结果数组返回条数（`TRIAGE_DEFAULT_RESULT_LIMIT` = 0 全量）、工具文字呈现上限（`TRIAGE_TEXT_RENDER_MAX` = 100，仅影响文本，`data.results` 始终完整）；**任务配置预设** `{GEBAI_HOME}/triage/preset.json`（优先级「显式参数 > 预设 > 内置缺省」；`preset.ts`，可配字段 = `TRIAGE_PRESET_FIELDS`；每次调用重读，改完即生效）；**REST 专属**：同步整体超时（`GEBAI_TRIAGE_SYNC_TIMEOUT_MS` / `sync_timeout_ms`，缺省 120s，0 = 关）与限流（`GEBAI_TRIAGE_RATE_BURST` = 10 / `GEBAI_TRIAGE_RATE_PER_SEC` = 2，任一为 0 则不限流） |
| 后台任务保留上限 | 50 条/作用域 | `core/session/jobs.ts` 的 `BG_JOB_KEEP`：通用后台任务（j 前缀）终态记录每作用域保留数，超出淘汰最旧（运行中的不淘汰；与子会话 `SUBSESSION_KEEP` 同口径防无界增长） |
| 两级研判 L2 默认上限 | 精审 20 条 / 每轮 5 条 / 超时 600s | `l2_max_items`（超出部分标「待精审」返回，可续跑）、`l2_batch_size`（按轮精审，避免一次提示词过长）、`l2_timeout_ms`（超时走收尾而非硬杀） |
| 研判 RPC 批上限 | 8 条/次 | 单次 `ctx.subSessions.start` 派生子会话数上限（`SUBSESSION_MAX_PER_CALL`）——L2 精审按 batchSize 分批即受此约 |
| 后端图表渲染超时 | 20 秒（仅 plantuml） | `feishu-bot/plantuml.ts` 的 `PLANTUML_TIMEOUT_MS`（可注入）——**mermaid 与 d2 的后端渲染无超时**（`core/support/diagram-render.ts` 直接 await；20 秒超时只在前端本地渲染侧，见下行） |
| 后端图表输出尺寸上限 | 1600 × 2400 px | 后端渲染默认 2x 超采样，超出按比例缩放到该上限（防超大 PNG 超飞书图片限制；`DEFAULT_MAX_WIDTH`/`DEFAULT_MAX_HEIGHT`） |
| 图表语言 | `mermaid` / `plantuml` / `d2` / `echarts` | show 图表分支 `format` 参数四取值（SDK `DiagramFormat`，缺失/非法立即报错）；产物扩展名 `.mmd`/`.puml`/`.d2`/`.echarts`；前端本地渲染与后端组合渲染器（飞书/`render=backend`）均四语言全支持；echarts 源码为 option 的严格 JSON（双引号，容错注释/尾逗号）；服务端合法值域单点真相在 `artifacts.ts`（`DIAGRAM_FORMAT_VALUES` 派生自 `DIAGRAM_EXT_FOR` 键集，show 参数校验/schema enum/飞书桥接透传共用，SDK 新增语言漏项即编译报错） |
| ECharts 画布尺寸 | 默认 960×600，钳制 200-4000 px | echarts SSR 渲染必须显式宽高（无 DOM 测量）；信封 `width`/`height` 超范围钳制、非法值回退默认（前后端一致） |
| ECharts 源码形态 | 严格 JSON（键/字符串双引号），容错注释/尾逗号 | 不支持单引号/裸键名/`…`缩写（对象字面量不可用）；信封 `{"option":…,"width":…,"height":…}` 指定画布尺寸；解析失败错误信息点名常见病因（前后端一致） |
| ECharts 标题/图例避让 | 冲突图例下移至标题底边 +6px；图例底边越界时联动下调未显式设置的 `grid.top` | echarts 6 图例默认在画布底部，但模型常按 v5 习惯显式 `legend.top: 0/'top'/小数值` 置顶——与顶部标题（默认占带 15–46.6）必然压字；高度按默认值折算（标题行高 = 字号×1.2 + padding×2 + 副标题 itemGap+行高；图例单行 30、折行每行 +28，条目总宽按全角 1em/半角 0.6em 估算，条目名缺省取 series 名）；不干预：图例 top 未设置（默认底部）/非顶部区域、标题不在顶部、二者水平分居左右两侧、图例隐藏、显式 `grid.top`/`grid.height`（`fixEchartsLegendOverlap`，前后端解析入口统一应用） |
| show format 校验 | 缺失/非法立即报错 | 不静默回退 plantuml（防漏传 format 时源码被错误语言渲染、报错误导模型）；`path` 模式扩展名可推断时免传 |
| show echarts 通道加固 | 预校验 + 版本错位诊断 | 服务端 `parseEchartsInput` 预校验 JSON（无效立即报错不跑前端）；前端报错引擎与请求语言不符（旧版前端把 echarts 当 PlantUML 渲染）返回「前端渲染器版本过旧，请刷新页面」诊断，**不自动换通道**（前端渲染为默认正确通道） |
| 图表配色 | 跟随 UI 主题明暗 | 亮色主题引擎默认亮色（零注入，与历史一致）；暗色主题注入 UI 主题对齐的暗色色板（`diagram-theme.ts` 统一供给，前端 `diagram.ts` 与工作台 `files/viewers.ts` 一致），主题切换重绘、缓存按色板分键；ECharts 不用 `darkMode`/内置 dark（SSR 链路不生效/系列色写死，改用 option 浅合并注入） |
| D2 主题 ID | 0（Neutral Default） | 前端与后端均固定 `0`；暗色主题 ID `200`（Dark Mauve）已停用（`diagram.ts`/`diagram-render.ts` 常量） |
| 前端本地渲染引擎 | mermaid（懒加载）/ @plantuml/core（懒加载）/ @terrastruct/d2（懒加载）/ echarts（懒加载） | 四语言零网络本地渲染；加载超时 30 秒（echarts 15 秒——约 1MB 体积小）、渲染/编译超时 20 秒（`diagram.ts`，引擎体积大慢机器加载可超 15 秒故放宽）；echarts SSR 模式（`init(null,…,{renderer:"svg",ssr:true})` + `renderToSVGString`）纯计算输出 SVG、无 DOM 挂载（缓存仅按源码，与主题无关）；**未知图表语言显式报错引导改用 `render=backend`（不静默回退 PlantUml——服务端新增语言而前端为旧版本时，回退会把源码当 PlantUML 渲染出误导性错误）**；**D2 前端单一串行队列**（浏览器构建单 Worker 共享 currentResolve，并发调用互相覆盖导致超时，`enqueueD2` 一次一个）；D2 编译错误 JSON 数组转可读文本（`formatD2Error` 提取 errmsg，前后端一致）；**动态分块加载失败自动整页刷新一次**（开发模式重建后旧页面引用旧 hash 分块 404，浏览器报 "Failed to fetch dynamically imported module"）；空闲预热仅限**本机实际用过的引擎**（痕迹记于 localStorage `gebai.diagram.engines`，`renderDiagramSvg` 分派时记录），无图表使用史的会话首屏不下载任何引擎（单引擎数 MB）；D2（8MB WASM）不预热 |
| 后端渲染引擎 | plantuml（TeaVM + DOM shim）/ mermaid + happy-dom / d2（`@terrastruct/d2` WASM）/ echarts（npm 包 SSR） | 四语言后端渲染（`core/support/diagram-render.ts`，飞书与 `render=backend` 共用）；懒加载（echarts 顶层急切导入——zrender 环境探测须先于 DOM 垫层污染）；单一串行队列；`globalThis.window` 仅临时存在（Bun worker_threads 冲突规避）；d2 二进制模式从内嵌产物（`packages/server/scripts/build-d2js.ts` 生成 JSON，gzip base64）物化 `{GEBAI_HOME}/vendor/d2js/{version}/` |
| show 复制上限 | 100MB | 会话 `tmp/` 外文件复制进会话文件区的尺寸上限（`SHOW_MAX_BYTES`），超出引导改为告知路径 |
| show 文本直显上限 | 读取 512KB / 内联 4 万字符 | 文本/代码内联 `code` 块的读取上限（`SHOW_TEXT_DIRECT_BYTES`，超出仅给查看/下载卡片）与截断阈值（`SHOW_TEXT_MAX_CHARS`，超出截断展示 + 附 file 卡片取全文） |
| D2.js 内嵌产物 | `packages/server/src/core/d2js.embedded.generated.json`（gitignore） | 构建时 `packages/server/scripts/build-d2js.ts` 生成；node-esm 构建 7 文件 gzip base64（22MB wasm → 9.9MB）；构建命令 `--external @terrastruct/d2` 保持运行时文件路径 Worker 可用 |
| 粘底阈值 | 64px | 距底 ≤ 该值视为在底部（微调容忍区：`scroll-intent.ts` 的 `threshold`；在此范围内上翻不解除跟随） |
| 程序回正落点 | 底部 − 用户的距底位移 | `sticky-follow.ts` 的 `followOffset` / `pinTarget`：跟随态内用户留下的位移在内容增长时原样保留（未上翻时为 0 = 绝对底部）；窗口化经 `setFollowSource` 取同一落点，不自己写 `scrollHeight` |
| 恢复跟随的距离 | 4px | `scroll-intent.ts` 的 `resumePx`：需**真正到底**才恢复跟随——触摸板/触屏能精确停在容忍区内，在那里恢复会把「往回调一点继续读」误锁为「想追最新」 |
| 上翻确认时长 | 60ms | 位置离开底部且有向上位移后，确认期满仍离开才解除（回弹/过冲豁免；停手无后续事件时靠此定时器复查） |
| 上翻宽限 | 120ms | 收到上翻输入（滚轮上滑/触摸上滑）后，期间不做程序回正（滚动是合成器异步派发的，内容增长的 rAF 可能先行） |
| 特效画布分辨率上限 | 1x | `theme-fx.ts` 的 `FX_MAX_DPR`（相对 CSS 像素）：全屏读写代价随其平方增长，高 dpr 屏按 2x 铺满时矩阵逐帧全屏渐隐需读写 24MB/帧 |
| 特效运行中帧率 | 24fps | `FX_BUSY_FPS`：当前会话有任务在跑（`data-fx-busy`）时降频，与输入档取更慢者 |
| 矩阵全屏渐隐帧率 | ≤30fps | `FX_FADE_FPS`：渐隐量按累计 dt 算，降频视觉等效；全屏半透明读改写是该特效最贵一步 |
| 待办终态项折叠 | 操作后留最近 3 条 | `todo` 返回的快照里，completed/cancelled/failed 项只保留最近 `TODO_COMPLETED_TAIL`=3 条，其余压成一行计数；进行中与待办项始终全量；**显式查询（空 entries）全量展开**——避免清单随任务时长逐轮重发而无界膨胀 |
| 参数解析失败回传片段 | 300 字符 | 引擎回传模型原始参数片段（`ARGS_ERROR_CLIP`）上限：片段只用于让模型定位首个语法破绽，全量回吐会让一次失败吃掉上千字符上下文 |
| 检查项失败输出尾部 | 120 行 | `self_optimize_run_tests` 失败项保留尾部行数（`CHECK_FAIL_TAIL_LINES`，错误与堆栈在最后），前置注明省略行数；通过项只回报用例计数与失败/跳过明细 |
