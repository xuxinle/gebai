# 内置子 Agent 详解

> 本册是 `DESIGN.md` 的「架构」章节中**内置子 Agent 逐个详解**部分，**章节号与主文件保持一致**。
> 索引见 [`../DESIGN.md`](../DESIGN.md)；总纲与工程结构见 [`01-overview.md`](01-overview.md)。

### 更多内置子Agent

按同一模式补充以下实用子Agent，构建时选择性打包：



#### `vision`（视觉能力，图片分析型）

实现于 `packages/agents/src/agents/vision/`（@gebai/agents 包；`vision.ts` 定义入口 + `vision.md` 系统提示词 + `vision.test.ts`），视觉能力的**通用层收拢**（图片输入型）与其他子代理的复用入口——全局 `vision` 工具已移除，本子代理是视觉语义分析的**唯一入口**（主会话 agent_load 装载/路由自愈，子Agent 依赖声明连带装载）（分层与两种复用方式的边界判定见「小模型识别」末条「视觉能力分层与子代理复用边界」）。**跨语言合并形态**（边车协议 v2 请求级 ctx 的首个内置样例）：

- **客卿侧（`keqing/python/vision/`，onnxruntime 原生推理）贡献**：`ocr`/`locate`/`locate_image`/`detect` 四个本地识别工具——重计算迁边车（原生推理比 wasm 快、模型进程级加载一次全会话复用），manifest `requiresApproval:false`（只读免审批）；算法与 TS `core/cv` 纯函数同源移植（det/rec 前后处理、DB 后处理、CTC 解码、零均值 NCC 模板匹配、letterbox+v8/v5 双形态后处理、ultralytics ONNX 元数据自适应），模型资产复用同一套（GEBAI_CV_MODELS_DIR / assets/cv-models / {GEBAI_HOME}/models/detect）；图片路径经请求级 `ctx.cwd` 解析；依赖（onnxruntime/numpy/pillow）缺失时工具返回安装提示不拋栈，探测延迟到调用时（模块加载阶段缺依赖不阻断工具集上报）
- **TS 侧贡献**：仅 `analyze`（多模态语义分析）——provider 抽象属宿主 LLM 层（三协议流式适配、GEBAI_VISION_* 与任务级 env 覆盖、多模态回落链经 `getVisionProvider`），凭证不下传边车；description 留空不贡献，识别工具描述与决策序提示词由客卿侧 PROMPT.md 提供，合并层拼接两侧：

- **工具集**（跨语言合并后共 8 个：客卿侧贡献 `ocr`/`locate`/`locate_image`/`detect` 四项，与语言基础工具 `run`/`pip`/`status` 合并后随 `analyze` 一并装载；全部只读免审批；**受客卿门控**——客卿仅本地形态启用、沙箱启用即整体禁用，故服务端部署下只有 TS 侧 `analyze` 可用）：`analyze`（多模态模型语义分析——理解图像内容/布局含义/非文字元素，支持 png/jpg/jpeg/gif/webp；复用 `makeVisionTool` 工厂注入 `getVisionProvider`（同一 provider 解析：GEBAI_VISION_* 外挂 → 多模态主模型回落））、`ocr`（本地 OCR 读文字带图片像素坐标）、`locate`（定位文字坐标）、`locate_image`（模板匹配定位图标/图形）、`detect`（自备 YOLO 目标检测，默认配对 OCR 文本）——后四者复用 `createCvAnalysisTools`/`createDetectTool` 共享工厂，注入「无缺省源（image 必填）+ 无闸门」形态
- **边界（不截图）**：输入一律为图片文件路径（PNG；analyze 另支持 jpg/jpeg/gif/webp）——本子Agent 不提供缺省图像源，需要现截屏幕/页面由 desktop/playwright 子代理完成；坐标为图片像素系，消费方自行映射到目标环境（屏幕加窗口原点、页面用 elementFromPoint）
- **决策序（硬性，按性能从高到低）**：①读文字/找文字/判断可 OCR 状态 → `ocr`/`locate`（本地 OCR，毫秒~秒级、精确坐标、离线不耗配额）——禁止用 analyze 回答 ocr 能答的问题；②找图标/图形/UI 元素 → `locate_image`（本地模板匹配）；③找对象/框目标 → `detect`（本地 YOLO）；④`analyze`（外部多模态模型，秒级延迟、耗配额、可能超时）仅语义理解（图像内容/布局含义/非文字元素）或 1~3 无结果时兜底——超时不原样重试，先回到 1~3
- **依赖复用（方式一消费方）**：`self_optimize` 声明 `dependencies: ["code", "vision"]`——subsession_run 隔离子会话即使不继承全局工具也有视觉能力（page_capture 截图后 `vision_analyze` 分析、`vision_ocr` 读图文字）；desktop/playwright 的本地识别推理也消费 vision 边车（见下条「推理委托」）；语义分析兜底按需 agent_load 装载；全局 `vision` 工具已移除（降级指引文案统一改指 vision_analyze）
- **推理委托（desktop/playwright 复用边车推理，sidecar-first）**：desktop_ocr/desktop_locate/desktop_detect、playwright_ocr/playwright_locate/playwright_detect 与 desktop_wait_for 的 text 模式，推理优先经 `ToolContext.registry` 调用 `vision_ocr`/`vision_detect`（即 vision Python 边车——onnxruntime 原生推理、模型进程级缓存），未装载/报错回落进程内 wasm（`@gebai/agents` core/shared/cv-analysis 内 ocrInfer/detectInfer 统一封装）。跨进程图像传递：RgbaImage 经 `encodePng`（`@gebai/agents` core/cv/image，与 decodePng 对称）落会话工作区**每次调用唯一**的临时文件（`tmp/cv_sidecar_in_<序号>_<随机>.png`，用后即删）后以相对路径调用，边车经请求级 ctx.cwd 解析——并发 CV 调用（js 编排 Promise.all 并行 ocr/locate/detect）不共用路径，不会互相覆盖或读到瞬时半写文件；坐标系完全同构（边车返回坐标相对传入 PNG=裁剪后图像像素系，与 wasm 一致，消费层 offX/offY 回加不变）——纯 drop-in。经注册表而非直连边车进程：复用装载门控/审批策略/ctx 组装，不绕过任何治理层；边车未装载时零开销回落（registry.resolve 不到即 wasm）
- **缺省源截图**：desktop/playwright 的缺省图像源（现截屏幕/当前页视口）同样用每次调用唯一的临时文件名（`cv_capture.png` / `pw_cv_capture_<序号>_<随机>.png`）并读后即删——同一会话内并发取图不互相覆盖
- **大图与小字（OCR 分块）**：整图长边超过 `OCR_TILE_SIDE` 时按块检测（相邻块重叠固定像素、重叠区同行按框 IoU 去重取高分），每块以原生分辨率送模型——整图等比缩到长边上限会把小字号压到不可辨识；检测输入尺寸对耗时影响很小（FHD 960→1920 仅 +20% 耗时，小字高分命中 2/7→7/7），故默认原生尺度送检
- **平坦窗口（模板匹配）**：粗扫与精化两阶段的 NCC 分母均设下限（窗口方差≈0 时 NCC 无定义，记 0 分）——否则浮点残差会被放大成伪高分，挤占精化候选名额而使真匹配拿不到
- **降采样语义一致**：两侧（TS `core/cv/template.ts` 与 客卿 `vision/tools.py`）均为 box 块均值降采样（边缘块按实际像素数平均），同一模板在两侧得到一致分数
- **定位失败回落（`locate`）**：目标文字精确匹配落空时，按相似度给出最相近的若干识别行（含文字、坐标与相似度）——OCR 对小字/艺术字的误读会让精确匹配落空，直接报「未找到」把可继续的失败变成死路（模型只能撞词重试）；确无相近行时才给出换通道建议，两条失败路径都指向可行动的下一步
- **检测类别缺标签（`detect`）**：模型无内嵌 `names` 元数据时类别以 `class_N` 占位并显式提示（可用 `GEBAI_CV_DETECT_LABELS` 指向每行一个类名的文本文件补齐），占位类别名即 `class_N`（`data.objects` 里直接可辨）——占位类别无法据此判断检测到的是什么，静默输出会让模型误读

#### `desktop`（桌面控制）

```ts
export const name = "desktop"
export const description = "涉及宿主机桌面操作时装载本子Agent（仅本地模式，服务端部署不可用）：截图（全屏=虚拟屏幕覆盖多显示器）、本地 OCR 识别与文字/模板定位（小模型离线推理）、UIA 语义树枚举（uia_inspect，控件角色/状态/值，Chromium/原生应用可选语义通道）、窗口控制（激活/移动/最小化/最大化/关闭/置顶，HWND 精确定位多窗口）、键盘鼠标输入（输入/点击/滚动/拖拽，修饰键组合/按住分离/中键三击）与剪贴板读写（文本/图片）、界面条件等待（文字/模板/画面变化）；窗口优先 PID/HWND 定位，输入/点击类操作需审批。输入：操作目标；输出：操作结果、屏幕文字坐标与屏幕图片。"
export const systemPrompt = "你是桌面控制助手，直接操作宿主机桌面（仅本地模式；服务端部署下所有工具拒绝执行）。工作流程：1) 明确目标：先 window_list 确认目标窗口（PID/进程名/前台标记*/窗口位置/标题；窗口优先用 PID 定位——标题可能重复，前台标记可确认当前焦点窗口），截图前先说明截图用途；2) 准备：截图/坐标操作前先 screen_info 确认显示器分辨率与主屏原点（主屏左上角为坐标原点，副屏坐标可为负；全屏截图=虚拟屏幕覆盖所有显示器，OCR/locate 返回的坐标已映射回主屏原点像素系，可直接用于点击），避免 region/坐标错位；3) 定位：点击屏幕元素优先取精确坐标再 mouse_click——文字元素用 desktop_locate，图标/图形元素用 desktop_locate_image（模板匹配，template 为既有截图裁剪的模板 PNG 或 template_region 在当前截图内取区域；同尺寸匹配，模板需与目标显示尺寸一致）；读取屏幕文字用 desktop_ocr（本地小模型，快且带精确坐标，离线不耗配额），仅语义理解/非文字内容才装载 vision 子代理用 vision_analyze 分析截图；desktop_detect 检测 UI 组件/图标（自备 ultralytics YOLO ONNX 即插即用——输入尺寸与类别自动读模型元数据，GPU 可用时自动走 sidecar 加速），输出组件框并默认配对 OCR 文本，适合结构感知与无文字元素定位，找特定文字按钮仍以 desktop_locate 为主；4) 执行：输入/点击/窗口控制类操作（window_focus/window_move/window_state/type_text/key_press/mouse_move/mouse_click/mouse_scroll/mouse_drag/clipboard_write）需审批，操作前先说明操作意图与目标窗口；滚动页面/列表用 mouse_scroll，拖放/滑块用 mouse_drag，窗口最小化/最大化/关闭用 window_state（close 走优雅关闭）；执行前必须先用 window_focus 激活目标窗口，仅当输出「已激活」才继续输入/点击——输出「激活失败」时不得继续（会把内容输入到错误窗口），请用户手动点击目标窗口后重试；type_text 剪贴板模式输出「输入失败」时不要原样重试（改 mode=\"keys\" 输 ASCII 或提示用户检查剪贴板管理软件）；只读类（screenshot/window_list/screen_info/clipboard_read/desktop_ocr/desktop_locate/desktop_locate_image/desktop_detect/desktop_wait_for）免审批可直接执行；5) 反馈与等待：执行后反馈结果——截图返回图片，mouse_click 坐标基于截图实际尺寸判断（desktop_locate/desktop_locate_image 返回的坐标已相对屏幕原点，可直接使用），说明截图位置与关键结论；操作后等待界面就绪用 desktop_wait_for（等文字出现/消失或画面变化，默认 20s 超时），不要自己反复截图轮询；6) 约束：只执行用户明确要求的操作，不做额外破坏性动作（不改系统设置、不删除文件、不触发危险快捷键）。验证多通道：不依赖单一验证通道——截图黑屏/纯色（工具会主动提示）时立即切换通道，不要反复重试截图：用 window_list 确认窗口是否在前台、用 desktop_ocr/clipboard_read 验证界面文本与剪贴板状态，或经 subsession_run 委托 code 子Agent 读取应用数据文件断言结果；任何通道失效即降级并明确告知用户当前采用的验证方式。"
export const tools: ToolSet = { screenshot, window_list, window_focus, window_move, window_state, type_text, key_press, mouse_move, mouse_click, mouse_scroll, mouse_drag, clipboard_read, clipboard_write, screen_info, ocr, locate, locate_image, detect, wait_for, uia_inspect }
export const requiresApproval = { window_focus: true, window_move: true, window_state: true, type_text: true, key_press: true, mouse_move: true, mouse_click: true, mouse_scroll: true, mouse_drag: true, clipboard_write: true }
export const preload = false
```

- 实现于 `packages/agents/src/agents/desktop/` 目录（@gebai/agents 包）（`desktop.ts` 定义 + `desktop_tools.ts` 工具集 + `desktop_cv_tools.ts` 本地识别工具集 + `desktop_uia_tools.ts` UIA 语义工具 + 各自 `*.test.ts` 测试），工具不注册为全局工具，仅经子Agent 命名空间暴露（只声明桌面操控独有工具；验证多通道经全局 `subsession_run` 委托 code 子Agent 读取应用数据文件断言结果——编排用全局名，def 不复刻全局工具）
- **提示词纪律（js 编排优先）**：多步操作（窗口定位→聚焦→识别→点击/输入→验证，含分支/重试）默认经全局 `js` 编排为一段脚本执行——脚本内各 desktop 工具像函数一样 await（按注册名），按中间结果分支与重试，不逐步往返；js 保持默认审批（一次审批覆盖脚本内全部工具调用，含输入/点击类——免审运行时内部需审批工具被拒）；先 window_focus 复核「已激活」再输入/点击；OCR/locate 限定目标窗口 region 小区域识别（屏幕坐标=窗口原点+区域内坐标）；等待一律 desktop_wait_for，不反复截图轮询；vision 仅语义理解兜底，定位一律本地 OCR/locate；**vision 硬性兑底序**：读文字/找元素/判断界面状态（按钮文字是否变化、处于哪一页、是否弹提示）一律 desktop_ocr（find 过滤）/desktop_locate 先行，禁止用 vision 回答 OCR 能答的问题；仅当 OCR/locate 无结果或需理解非文字内容（图像内容/布局含义/报错图标）才 vision；**uia_inspect 决策序**：需要控件语义/状态（禁用/勾选/值/焦点控件——OCR 判不了）或控件树交叉校验时用 desktop_uia_inspect（仅 Windows；空树/浅树=Flutter/游戏/自绘立即回落像素通道），定位坐标仍以 desktop_locate/locate_image 为主通道；**等待纪律**：等非文字元素（加载动画/图标）用 wait_for 的 image/image_gone 模板模式，非 change（画面变化误报率高）
- **权限拦截降级（UIPI）**：模拟输入对管理员权限（elevated）目标会被 Windows UIPI 静默拦截——症状为「点击/输入报告成功但界面毫无变化」（window_focus 已确认前台）。提示词约定：连续 2 次无效即停止重试，立即降级换通道（目标应用自带的本地 API/CLI（游戏客户端多带本地控制接口，经全局 sh/js 调用）、键盘/剪贴板间接操作，或建议用户以普通权限重启目标/提权运行歌白）；佐证判据：Get-Process/Win32_Process 查询目标进程路径/命令行为空 = 完整性级别高于当前会话（whoami /groups 查自身，Medium 即不可读写 elevated 进程信息），模拟输入与进程信息查询均注定失败
- 跨平台：Windows 走内置 PowerShell（截图/窗口/输入，无外部依赖）；macOS 走 `screencapture` + `osascript`（鼠标需额外 `cliclick`）；Linux 依赖 `xdotool`/`wmctrl`/`scrot`（缺失时明确报错）。**Windows 坐标一致性**：全部涉及像素坐标的 PowerShell 脚本（截图/screen_info/window_list/window_move/mouse_*）开头声明 `SetProcessDPIAware`——坐标统一物理像素，防高 DPI 缩放（150% 等）下截图/窗口/鼠标坐标落入逻辑像素空间错位；**多显示器**：全屏截图与 cv 现截均取**虚拟屏幕**（`SystemInformation.VirtualScreen`，覆盖所有显示器，副屏可为负坐标），`region` 参数 x/y 支持负值（副屏在主屏左侧/上方），OCR/locate/locate_image 返回坐标一律映射回主屏原点像素系（虚拟屏幕原点/区域原点偏移），可直接用于 mouse_click；`window_list` 输出含**前台标记**（`GetForegroundWindow` 比对，`*` 行 + 「当前前台」汇总行）与**窗口 bounds**（`GetWindowRect` 物理像素 `x,y,w,h`）
- **Windows PowerShell 执行通道（临时 .ps1 文件 + -File）**：所有 PowerShell 脚本（desktop_tools/desktop_cv_tools/desktop_uia_tools 三文件共用 `psCmd()` 助手）不再用 `-EncodedCommand` base64 内联——旧通道把整个脚本内联进命令行，受 cmd 单条命令行 8191 字符上限约束（截图/UIA 类长脚本必超——实测 ≥4KB 脚本即「command line is too long」静默 exit 1 且无任何输出），且企业管控软件会静默拦截长内联命令；改为脚本写入会话 `tmp/ps-scripts/` 下临时 .ps1，**UTF-8 带 BOM**（PS 5.1 对无 BOM 文件按 ANSI 解码，中文必乱码），经 `powershell -NoProfile -ExecutionPolicy Bypass -NonInteractive -File "<绝对路径>"` 执行（路径双引号包裹——Windows 参数解析不认单引号，单引号混入路径致 -File 打开失败、PS 回退交互模式污染 stdout；内嵌双引号反引号转义），执行后延迟 60s 删除临时文件（执行中的 .ps1 被 Windows 锁定；unref 不阻塞进程退出；删除失败留在会话目录无害）；`ctx.writeFile` 不可用/失败时兑底回 EncodedCommand 内联（短脚本行为同旧通道）；工具 run() 失败输出 stderr+stdout 合并展示（原来只显示其一，排障信息丢失）
- 截图返回 `image` 内容块实时展示，并**自动做黑帧/纯色检测**（平均亮度极低提示显示器休眠/锁屏，暗色单色提示非真实画面）与**尺寸/原点元数据**（`STAT` 行 / sips / ImageMagick——原点供图片坐标 → 屏幕坐标换算）；`screen_info` 列出全部显示器（分辨率/位置/主屏）供 region 与坐标参考；文本输入默认剪贴板粘贴法（中文/符号可靠，**写入回验重试**——剪贴板管理软件拦截写入时明确报错不粘贴；粘贴后**延时恢复**原剪贴板——目标应用异步消费剪贴板，立即恢复会粘出旧内容），`mode="keys"` 纯按键模式（绕剪贴板，仅 ASCII；中文 IME 激活时部分标点可能丢失）；`window_focus` **激活后复核前台窗口**（`GetForegroundWindow` 比对），前台锁定拦截时经模拟 Alt 击键缓解重试，仍失败明确报错（不虚报成功）；`window_state`（最小化/最大化/还原/关闭，close 经 `WM_CLOSE` 优雅关闭可弹保存确认）、`mouse_scroll`（滚轮四方向，每格 120 单位）、`mouse_drag`（左键按住 + 12 步插值移动 + 抬起，适配依赖真实鼠标轨迹的目标）与 `clipboard_write`（写入回验 + 敏感值**告警不中止**——复制密钥供本人粘贴是常见需求）为 Windows 实现，macOS/Linux 部分明确输出「暂未支持」；`type_text`/`clipboard_read` 输入或读取前自动做**敏感值扫描**（`sk-` 前缀、KEY/SECRET/TOKEN 赋值、Bearer 令牌，长令牌启发式 40+ 字母数字混合且非纯十六进制——纯英文长串/git commit sha 不误判），命中即中止（type_text）/告警（clipboard_read）；**服务端部署（沙箱启用）下所有工具拒绝执行**；**Windows 输入增强（2024 批次）**：`key_press` 支持 `action: press/down/up`（按住/抬起分离——按住 Shift 点选、按住 W 前进）与 `hold_ms`（press 按住时长），系统级虚拟键经 vk 名直发 keybd_event 扫描码（`win+r`/`volume_up`/`media_next`/`vk_left`/`vk_XX` 任意十六进制码——SendKeys 覆盖不了的媒体/Win/箭头键；`win` 在组合中是修饰键，`vk_shift` 纯修饰键组合仅 down/up 有意义，裸 `shift` 仍走 SendKeys `+` 语义）；`mouse_click` 支持 `modifiers`（ctrl/shift/alt 以 + 组合，按住期间点击后逆序抬起）与 `button: middle/triple`（中键/三击段落选择，双击/三击 50ms 间隔时序）；`mouse_drag` 同样支持 `modifiers`（Ctrl+拖拽复制文件）；**窗口控制增强**：`window_list` 经 EnumWindows 枚举全部顶层可见窗口（替代 Get-Process MainWindowHandle——后者每进程只见一个主窗口，浏览器多窗口/多开应用漏窗口），输出末列 HWND；`window_focus/move/state` 新增 `hwnd` 参数精确指向多窗口进程中的具体窗口（优先于 pid/title，winLocate 助手统一注入——注意 PS 条件须用 `$true` 字面量，裸 `true` 在 PS5.1 被当命令调用恒走 else）；`window_state` 新增 `topmost/notopmost`（SetWindowPos HWND_TOPMOST/-2 + SWP_NOMOVE|NOSIZE）；**剪贴板图片**：`clipboard_write` 的 `image` 参数（PNG 路径 → 系统剪贴板位图，FromFile+Dispose——FromStream+`::new` 内联在 PS5.1 有类型解析陷阱；写入后回验尺寸一致，截图交给用户 Ctrl+V 粘贴的高频路径），与 `text` 二选一
- **本地识别五工具（只读免审批，`desktop_cv_tools.ts`，基建见「小模型识别」；其中 ocr/locate/locate_image/detect 四工具均经共享工厂 `@gebai/agents` core/shared/cv-analysis（playwright 与 vision 子代理复用同工厂，见「视觉能力分层与子代理复用边界」），wait_for 为 desktop 独有实现）**：`desktop_ocr`（读屏幕/图片文字：image 省略现截全屏、region 限定区域（负坐标覆盖副屏）、find 关键词过滤；返回行文本+框+中心坐标+置信度，超 200 行截断提示收窄；data 携带结构化 lines）、`desktop_locate`（按目标文字定位：匹配分级 完全相等 > 行包含目标 > 目标包含行，返回最佳匹配中心坐标（直接可 `mouse_click`）+ 候选清单；未找到时给出多通道建议）、`desktop_locate_image`（**模板匹配定位图标/图形**：`template` 模板 PNG 路径或 `template_region` 在搜索图坐标系内取模板区域；NCC 同尺寸匹配（模板需与目标同一显示环境同尺寸），threshold 默认 0.8；返回最佳匹配中心坐标 + 候选；未找到建议降阈值/改 desktop_locate/vision）、`desktop_detect`（自备 YOLO 检测：`GEBAI_CV_DETECT_MODEL` 配置**或放入 `{GEBAI_HOME}/models/detect/` 唯一 .onnx 自动发现**（`GEBAI_CV_DETECT_LABELS` 可选——ultralytics 导出 ONNX 自动读取 names/imgsz 元数据）；**默认与 OCR 配对输出「组件类型+文本」**（`pair_text` 可关——检测只给类别不含语义，找特定文字按钮仍以 desktop_locate 为主）；`conf`/`iou`（NMS 阈值，密集小控件建议 0.1）可调；**推理分层**——GPU sidecar（GEBAI_CV_DETECT_BACKEND/EP，Windows DirectML 等任意 DX12 显卡）可用即用，输出如实标注实际后端，不可用/失败自动回落 wasm；未配置返回指引）、`desktop_wait_for`（**界面条件等待**：mode=text/text_gone 等待文字出现/消失（本地 OCR 轮询）、image/image_gone 等待模板图像出现/消失（NCC 模板匹配，`template` PNG 路径 + `threshold` 默认 0.8——加载动画/图标等非文字元素等待）、change 等待画面变化（灰度采样均差 > 2/255 对比基线）；timeout_s 默认 20 上限 120、interval_s 默认 2；超时不视为错误，返回最后观察状态）；坐标一律**映射回主屏原点像素系**（虚拟屏幕原点/区域偏移 + 缩放还原——识别在降采样图像上进行，返回坐标对准原屏幕）；现截复用固定文件 `cv_capture.png`（覆盖写，不随调用累积）；输入图仅支持 PNG（desktop/playwright 截图生态一致，其他格式明确报错）；模型/运行时缺失时错误输出为中文配置指引（不中断会话）
- **UIA 语义树（`desktop_uia_inspect`，`desktop_uia_tools.ts`，仅 Windows、只读免审批）**：枚举目标窗口的 UI Automation 控件树——控件角色/名称/AutomationId/类名/bounds/状态（disabled/focused）/值模式文本（输入框内容），中心坐标可直接 mouse_click；定位窗口按 `hwnd`（window_list 结果最后一列，同进程多窗口精确指向）/pid/title（可组合）——只给 pid 时同进程多顶层窗口（explorer 的桌面与任务栏、浏览器多窗口）会命中多个，取面积最大者为主窗口并在输出中报告候选数（按枚举顺序取首个会命中任务栏等辅助窗口）；`find` 关键词只输出名称/ID 匹配控件（含路径）、`depth`（默认 6 上限 12）/`max`（默认 200 上限 400）控制规模，超限标记 truncated。**Chromium/Electron/WebView2 无障碍树惰性构建已内置双查询**（首查触发构建 → 400ms → 正式收集——实测 Edge 首查 50 节点、复查 598），**未显式指定 depth 时浅树（<50 节点）自动加深到 12 重查一次**（这类应用的容器节点与真实语义常跨多层：VSCode depth=6 仅 12 节点、depth=12 有 75 节点真实语义）；BoundingRectangle 可为 Infinity/NaN（屏幕外/未布局元素）已归零防 Int32 转换异常；**空树/浅树（<5 节点）= Flutter/游戏/自绘框架不暴露语义**——输出明确提示回落像素通道（desktop_ocr/locate/locate_image），提示词同源纪律：控件状态（禁用/勾选/值）OCR 判不了才用 uia_inspect，与像素通道互补而非替代；服务端部署拒绝（同域闸门）

#### `feishu_docs`（飞书云文档）

> **提示词载入形态（实测修复，影响全部子Agent）**：子Agent 系统提示词以 `.md` 独立维护，但 **Bun 默认对 `.md` 走 html loader**——此前仅 `styles/index.ts` 显式声明 `with { type: "text" }`，11 个子Agent 的 `{name}.ts` 直接 `import x from "./{name}.md"`，导致**提示词以 HTML 形态进入上下文**（`<p>/<li>` 标签白占字符并割裂列表结构）；`packages/agents` 当时也无 `bunfig.toml`（仅 server/desktop 有，而 bunfig loader 的生效范围按包界定）。**修复**：全部子Agent 导入统一加 `with { type: "text" }`（逐文件声明，跨包/跨构建形态一致）+ `packages/agents/bunfig.toml` 声明 `".md" = "text"`（直跑场景第二道保险）+ `src/core/subagent-prompt-loader.test.ts` 三条防回归断言（导入属性齐备 / bunfig 声明存在 / 实测各子Agent systemPrompt 不含块级 HTML 标签）。

实现于 `packages/agents/src/agents/feishu_docs/`（`styles/` 排版 SKILL + `styles/index.ts` 聚合 + `docx_xml.ts` XML 转换 + `feishu_api.ts` 工具集 + `feishu_docs.md` 系统提示词 + `feishu_docs.ts` 定义入口），全面对接飞书开放平台云文档能力（docx v1 / drive v1 / sheets v2·v3 / bitable v1 / wiki v2）：

- **文档 docx（读写双通道：XML 主 / Markdown 快）**：读 `fetch_doc`（XML round-trip）/ `get_doc_text`（缺省纯文本；`format=markdown` 读成 Markdown，见「读写双通道」节）/`get_doc_meta` 元信息；改 `update_doc`（七指令族）/ `set_table_width` 表格列宽；建 `create_doc`/`import_xml`/`import_markdown`；体检 `lint_doc`；导出 `export_doc`。块级底层细节（XML→块归一管线、图片/图表素材回填、导入转换规则）见下层各节。**Markdown 导入**（`import_markdown`：本地转换引擎——**多级标题（#~#########，1~9 级）/段落/有序无序列表（缩进嵌套：每 2 空格或 1 tab 一级，跳级缩进归一为 +1 级、最深 9 级，子项作为父块 children 经 descendant 通道一次创建；**有序列表逐项下发 `ordered.style.sequence`**——按 CommonMark 语义段首项决定起始编号、其后自增，全写 `1.` 时为 1/2/3…）/任务列表/代码块/引用/表格/分割线/**图片（独立成行的 `![说明](本地路径或URL)` → image 占位块 + 素材上传回填：经 descendant 的 `block_id_relations` 把临时 id 映射为真实块 id 后上传素材并 PATCH `replace_image`，单张 ≤20MB，单张失败只提示不中断整篇导入）**/**图表围栏（```mermaid / ```plantuml / ```d2 / ```echarts）→ 调 `ctx.renderDiagram`（engine 注入，与 `show` 的 render=backend 同一渲染器：PlantUML TeaVM / Mermaid + happy-dom / D2 WASM / ECharts）渲染 PNG，同样经占位块+素材回填插入为图片；渲染器缺失（受限环境）或渲染失败时就地把占位块降级为同语言代码块（源码不丢、导入不中断），失败原因折成单行回传；`diagram_source=keep` 在图下再留一份源码代码块（独立 `diagsrc*` id 块组，避免与占位块数字 id 冲突）**/行内样式（加粗/斜体/粗斜体/删除线/行内代码/链接）**；段落行首两个全角空格（或 `&emsp;&emsp;`）→ `text.style.indentation_level=OneLevelIndent` 首行缩进；引用内代码围栏转行内代码元素（**实测 quote 块不支持子块**，无法内嵌列表/代码块）；新建文档时首行 H1 与 title 相同自动去重，GitHub 告示语法 `> [!NOTE]`/`[!TIP]`/`[!IMPORTANT]`/`[!WARNING]`/`[!CAUTION]` 自动转 **callout 高亮块**（浅色系背景+同色系边框枚举+emoji，标题粗体置首）；经 descendant 通道写入，自动分批 ≤1000；**代码块 language 按飞书官方枚举表（75 项）映射**（语言标识归一化查表：ts→TypeScript 63、js→JavaScript 30、py→Python 49、cpp→C++ 9，未知回退 PlainText），默认 `wrap=true` 自动换行（长行不溢出）；`engine="official"` 走官方 `blocks/convert` 转换通道，**复杂组合（代码块+表格）转换失败时自动回退本地转换**并提示）导出（`export_tasks` 创建+轮询——**轮询查询只携带 `token` query 参数**（实测多余 file_extension/type 报 field validation failed），任务终态失败（job_status 3/107~123）提前返回 job_error_msg；**导出 token 必须是文档级 token**——bitable 传 app_token，传数据表 table_id 报 1069914 file token invalid；**bitable/sheet 导出 csv 必须指定 `sub_id` 子表 ID**（官方接口要求，xlsx 不需要）→ `download_file`）
- **块层级能力矩阵（实测）**：**quote 块不支持子块**（带 children 报 1770041 open schema mismatch）——引用内列表/代码块只能文本化；**普通文本块（text/heading）也不支持子块**（块层级只由列表嵌套与容器表达）；列表块可嵌套列表块（bullet+bullet/ordered+ordered/bullet+todo）、callout 可带 text 子块、**table_cell 可带多个 text 子块与列表子块**；`folded` 折叠需子块，实际只对列表块有意义
- **嵌套块写操作语义（update_doc 块级指令族 / delete_blocks）**：飞书列表无容器块（连续同类项是同级平铺的独立块，fetch_doc 读出时聚合成单个 `<ol>/<ul>`，容器 id 即首项块 id、其余项 id 落在各 `<li>`），故：① 锚点 id 的插入/替换位置按锚块真实父块的 children 下标计算并透传给 descendant 接口的 `index`（嵌套子块锚插到其父内锚后位置，多批插入按批累加 index），不再回落文档末尾；② block_replace/block_delete/delete_blocks 拿列表首项 id（聚合容器 id）操作时扩展为同父连续同类整段（含嵌套子孙，API 级联删除），非首项 `<li>` id 按单项处理；③ block_move_after 多块删除按同父下标降序（防前移错删）；④ 父块不在索引/下标无法定位时明确报错而非默默插到文档根；⑤ delete_blocks 支持 `block_id`（待删块自身 id，免算下标）与 `parent_block_id`+`start_index`（下标相对指定父块）两种形态
- **图形化制图能力边界（实测，防止重复探测）**：飞书**不支持经 API 把 mermaid/PlantUML/UML 导入为可编辑图形**——① diagram 块（46，文档「绘图」块）不可创建（`1770029 block not support to create`）；② 官方 `blocks/convert` 对 ` ```mermaid `/` ```plantuml ` 围栏只产出 code 块（14），不转图；③ 画板（mindnote 块 43 + `board/v1`）半可行：mindnote 块可创建且平台自动分配画板 token，**`POST /open-apis/board/v1/whiteboards/{token}/nodes` 可写入图形节点**（composite_shape 的 rect/round_rect/ellipse/diamond + text + style，已验证渲染正确；style 为字符串枚举——`border_width:"narrow"`/`font_weight:"regular"`/`horizontal_align:"center"`，数字会报 99992402；写入为最终一致，立即 GET 可能为空），**但连线无法锚定节点**——connector 节点可创建（`connector:{start,end}` 正确写法，`connector:{start_object..}` 报 4005072 connector info empty），然而 start/end 内的节点引用字段（id/object_id/attached_object_id 等 10 种均试）被服务端静默忽略，连线不落地（下载画板图片可见图形无连线）；此外无节点更新/删除接口（DELETE/PATCH/PUT nodes 均 404），写接口未见于官方公开文档（公开的仅 `GET nodes` 与 `download_as_image`）。**可行替代（已实现）**：本地渲染→插入为图片——`import_markdown` 与 `import_xml`（`<whiteboard type="mermaid|plantuml|d2|echarts">`）统一经 `insertWithMedia` 共享管线：调 `ctx.renderDiagram`（与 show render=backend 同一渲染器）渲染 PNG 后经 image 占位块+素材上传回填插入（已验证：图可下载为非空 PNG）；渲染失败/渲染器缺失降级为代码块，`diagram_source=keep` 可保留源码；`download_as_image` 可用于画板内容取图
- **导入/导出**：`import_markdown`（Markdown → 块，见下）、**`import_xml`（XML 排版语法 → 块，整篇创作首选）**、`export_doc`（`export_tasks` 创建+轮询——**轮询查询只携带 `token` query 参数**（实测多余 file_extension/type 报 field validation failed），任务终态失败（job_status 3/107~123）提前返回 job_error_msg；**导出 token 必须是文档级 token**——bitable 传 app_token，传数据表 table_id 报 1069914 file token invalid；**bitable/sheet 导出 csv 必须指定 `sub_id` 子表 ID**（官方接口要求，xlsx 不需要）→ `download_file`）
- **文档创作与排版（排版 SKILL + 两阶段工作流）**：根因是 Markdown 只能表达飞书块模型子集（自动编号/分栏/高亮块配色/表格列宽/题注均无语法），故排版能力分三层落地——
  - **排版 SKILL 资源**（`styles/`：`style.md` 排版总纲 + `xml.md` XML 语法 + `genres/*.md` 15 类体裁契约（备忘简报/周报/方案/执行计划/PRD/技术文档/SOP/复盘/会议纪要/研究报告/数据报告/商业分析/白皮书/正式文档/法定公文），`style.md` 内置体裁选择表（读者任务与排除信号优先，关键词仅用于召回）；**入口 `styles/index.ts` 以 `with { type: "text" }` 显式 text loader 导入**（Bun 默认对 .md 走 html loader，会包 `<p>` 标签，显式属性使技能文档拿到原始 Markdown、不依赖 bunfig loader 生效范围），dev 与 dist/二进制均由打包器内联）；经 `style_guide` 工具**按需读取**（不占用常驻上下文）：无 `name` 列出全部文档，`name` 返回全文，未知名回可读清单；`styles.test.ts` 断言 genres 目录与登记表一一对应，防漏登记/死登记
  - **整篇创作（`import_xml`）**：类 HTML 标签 → 块描述（`docx_xml.ts` 纯转换层，零 HTTP 依赖、可独立单测）→ 与 `update_doc`/`import_markdown` 共用的同一套块归一流程（callout 配色/子块、code 语言枚举、todo done、表格列宽）+ `prepareGridColumns` 列补全 → descendant 通道写入。语法覆盖：`<title>`（首个同名 H1 自动去重）、`<h1>~<h9 seq="auto">`（**自动编号 1 / 1.1 / 1.1.1**，跳级补 1 并提示）、`<p align>`、`<ul>/<ol seq>/<li>`（嵌套写在 `<li>` 内，有序起始序号落 `ordered.style.sequence`）、`<checkbox done>`、`<blockquote>`、`<hr/>`、`<br/>`、`<pre lang caption>`（**代码内容按字面文本处理**：`<pre>` 内标签先转义再解析，代码示例里的 `<h1>`/`<div>` 不会被当块消费）、`<table><colgroup><col width>`（→ `table.column_width`，数量不符回落内容自适应并提示）+`<thead>/<th>`（→ `header_row`）、`<callout emoji background-color border-color text-color>`（→ callout 顶层字段；`light-*` 用基础色相、`medium-*` 用 +7 偏移枚举，`medium-*` 边框回落基础色并提示；非段落/列表子块移出并提示）、`<grid><column width-ratio>`（2~5 列）、`<img path href caption>`（题注落斜体段落）、`<whiteboard type="mermaid|plantuml|d2|echarts">`（内联源码或 `path="@./a.mmd"` 引用本地文件：`xmlToBlocks` 接受注入的 `readFile`（工具侧用 `readFileSync`+`ctx.resolvePath`），读取失败只记说明不阻断整篇 → `ctx.renderDiagram` 渲染 PNG 插入）、`<whiteboard type="svg">`（本地文件直传图片、内联转字节后走同一媒体回填管线；飞书画板无法从 SVG 创建故不可编辑）、`<cite type="user" user-id="ou_…"/>`（→ `mention_user` 元素，**@人可用**；经实测只用真实 open_id）+ 行内 `<b>/<em>/<u>/<del>/<code>/<a>/<span text-color background-color>/<latex>`；不支持项（`<bookmark>`/`<button>`/`<time>`/`<task>`/`<sheet>`/`<cite type="doc">`（**@文档实测 1770038**）/`<source>`/`<html5-block>`/`<okr>`、单元格底色与合并、图片宽高（**image.size 只读，实测 1770001**）、公式块）**逐条给出降级说明而非静默丢弃**；XML 语法/标签未闭合就地报可读错误（含标签名）供模型局部修正
  - **写入前预检（`import_xml` 的 `dry_run`）**：只解析不写入（**零请求——既不建文档也不写块**，避免历史上「语法错也先建出空文档」）；输出块画像（`xmlProfile`：顶层块/总块含子树/字数/类型分布）+ `precheckMedia` 问题清单（本地图片存在与否与 20MB 上限、URL 图片可达性与 content-length、内联 SVG 提示、图表试渲染结果）——把错在产生半成品之前暴露
  - **回查精修（`lint_doc`）**：读取块结构后按文档流（page 根块深度优先，子块 id 解析）逐条体检并**输出 block_id + 修复动作**（可 `limit` 截断）：标题层级跳级/连续标题/空标题/层级过深、空段落与超长段落（>300 字或 >6 行）、表格列宽过窄（<100px）/缺表头/列数过多、代码块缺语言、列表嵌套过深（>3 层）、分割线过密、高亮块滥用、缺标题结构、纯文字墙；无问题时给出明确结论。修法：表格 `set_table_width`、文本 `update_doc block_replace`、长段拆分 `block_insert_after`+`block_delete`，每轮改后重新体检（**不沿用旧 block_id**）。`lintBlocks` 为纯函数（`feishu_api.ts` 导出，可单测）
  - **定位与批量改写（收编为 update_doc 内部能力，旧工具已删）**：旧八件套（`get_doc_blocks`/`list_blocks`/`find_blocks`/`add_blocks`/`update_block`/`delete_blocks`/`replace_text`/`insert_image`）破 fetch_doc/update_doc 完全覆盖后已删除（工具 49→41，消除双通道选择成本与提示词维护双份）——目录定位收编为 `fetch_doc scope=outline`，文本反查收编为 `scope=keyword`，块增删改收编为 update_doc 指令族，跨块替换收编为 `str_replace`（`replaceInBlocks` 纯函数**逐 text_run 替换保留行内样式与链接**，命中跨 run 单列 crossRun，`PATCH blocks/batch_update` 每批 20 块）；`compactBlocks`/`docOutline` 等纯函数随工具删除一并移除
  - **XML round-trip 查看与编辑（`fetch_doc`/`update_doc`，官方 lark-doc skill 的 +fetch/+update 对标）**：`docx_fetch.ts` 纯函数层（与 `docx_xml.ts` 互为逆操作）把块树反向序列化为与 `import_xml` 同一套 XML 排版语法——**读出即可改、改后可写回，不跨格式转换**。`fetch_doc`：scope×detail 正交（outline 目录/section 整节/range 区间/keyword 多词 `|` OR 定位+上下文/缺省整篇 × simple/with-ids/full 三档详细度），keyword 命中容器内时整块输出并标 `<excerpt top-block-id>`（可作回读锚点）；行内样式还原为 `<b>/<em>/<u>/<del>/<code>/<a>/<span>` 与 mention `<cite>`，表格还原 `<colgroup>` 列宽与 `<thead>`（**服务端结构实测：column_size/row_size/column_width/header_row 都在 `table.property` 下**，cells 与 children 同为单元格 id）；资源块以 token 占位（`<img src>`/`<whiteboard token>` 等——改动时原样保留，看图走 `download_file` 传 `extra={document_id, block_id}` 走 medias 接口）；未知块类型给注释占位不静默丢块。**列表聚合（round-trip 对称性实测修复）**：飞书列表模型是一块一项，逐块序列化会把多条目列表读出为多个独立 `<ol>`（与写入端 `<ol><li>a</li><li>b</li></ol>` 不对称）——`seqToXml`/`seqSegments` 把**连续同类列表块聚合成单个 `<ul>`/`<ol>`**（容器 id 取首项、其余条目 id 落在各自 `<li>` 上可单独定位；有 `block_type` 过滤时不聚合，逐块过滤语义优先），section/range/keyword/整篇四条链路统一走聚合。`update_doc` 七指令：`str_replace`（复用 replaceInBlocks 管线）/`block_insert_after`（block_id=0 文首、-1 文末、锚块后——**descendant 接口实测支持 index 精确插入**）/`block_replace`（单块或同父连续闭区间——先删后插保位置）/`block_delete`（单块或区间）/`block_move_after`（块序列化→重建到锚点→删原块；**docx v1 无 move 接口（官方 MCP 全量路径清单无此接口），用重建实现**）/`block_copy_after`（同构重建，源块不变）/`append`；写入内容复用 `xmlToBlocks`→块归一→`insertWithMedia`（图片/图表自动回填）同一套管线。安全规则内置于提示词：每轮写后 block_id 已变不沿用、同一块多处修改合并一次 block_replace、资源占位不改纯文本（对应官方「Observe→Diagnose→Patch Plan→Patch→Verify」五步）；`fetch_doc` 免审批（读操作）
  - **实测平台限制（对照官方 skill 逐项验证，不再尝试）**：**文档历史版本/回滚不可用**——`docx/v1/documents/{id}/versions`、`drive/v1/files/{id}/versions`、`docx/v1/documents/{id}/history` 均 404（公开 OpenAPI 清单中 docx 域无版本接口，仅 `drive.v1.fileVersion` 手动打版本无回滚；官方 CLI 仓库无 history 实现文件）；**无块移动接口**（`children/move`、`children/batch_update` 均 404）；**图片显示尺寸只读**（`image.size`/`image.width` 两写法均 1770001）；**@文档（mention_doc）不可用**（obj_type 1/3/8 与带 url 组合均 1770038），只能改用普通链接
  - **实测修正（grid 分栏原实现必然失败，已重写）**：descendant 接口要求**每个 grid_column 带整数 `width_ratio` 且至少一个子块**——缺 `width_ratio` 或空列报 1770041 open schema mismatch，`width_ratio` 传小数（0.5）报 **9499 Invalid parameter value**（只接受整数）；`widthRatioWeights` 把 0~1 小数权重整体 ×100 取整后按最大公约数约分（0.5/0.5→1/1、0.6/0.4→3/2、0.25/0.75→1/3），`prepareGridColumns` 补默认权重与空子块并**保留列内内容随分栏一次创建**（旧实现剥离列内容与列宽、引导逐块回填，必被接口拒——已删）
  - **实测修正（分栏列宽：`width_ratio` 绝对值 = 占页宽百分比，旧实现把它当相对权重约分导致列窄到不可读）**：导出 docx 逆向验证（`<w:gridCol w:w>`，A4 可用宽 8280 twips）——2 列各传 1 时导出 `82+82`（占页宽 1%，列窄到 1.4mm 无法阅读）；传 50/50 → `4140+4140`（铺满）；传 100/100 → `8280+8280`（溢出）。旧 `widthRatioWeights` 按最大公约数约分（`0.6/0.4→3/2`、`0.5/0.5→1/1`），恰好把等分意图压成 1%，是缺陷根因。**修复**：改为按比例**归一为总和 100**（尺度无关：`1:1` / `50:50` / `0.5:0.5` 等价 → `50:50`；`3:1` → `75:25`；`1:1:1` → `34:33:33`），取整误差补到最大一份保证求和恰为 100（铺满且不溢出）。修复后实测：2 列等分 `4140+4140`、3 列等分 8279、2 列 2:1 → `5547+2732`，均占页宽 100%。另给 `update_block` 增 `grid_property`（`{widths:[2,1]}` 传相对比例自动归一）以便修已有分栏的列宽。
  - **实测能力校准（字段枚举）**：callout `background_color` 1~14（1~7 基础色相 + 8~14 medium 浅色）、`border_color` 1~7、emoji_id 接受 51 种内置表情名（bulb/warning/pushpin/heart/thumbsup/check/rocket/…，任意名不报错但仅内置名有图；**原生 emoji 字符（❗💡）不被接受——实测 1770006 schema mismatch，仅 ASCII 架名可用，非法名自动忽略并提示**）；文本/标题 `style.align` 1/2/3（左/中/右）；表格 `column_width` 指定列宽与 `header_row` 均原值生效；`todo.style.done` 可写
- **错误可诊断性**：API 错误统一携带 `method path` 与常见错误码提示（token 失效/过期、scope 未开通、参数不合法）；**权限类错误码（9999166x/9999167x，含实测 99991668/99991672 等未逐一收录的码）自动附「按接口路径特征推断的所需 scope + 开发者后台授权链接」（`https://open.feishu.cn/app/{app_id}/auth?q={scope}`，app_id 非敏感）**，减少 AI 反复试错、引导用户快速授权；docx 块操作失败时本地探测诊断，区分 `DOC_NOT_FOUND` / `BLOCK_NOT_FOUND` / 叶子块不支持子块，避免「invalid param」式无信息错误码的盲猜重试
- **用户授权（user_access_token，创建用户所有权资源）**：默认应用身份（tenant_access_token，资源归应用所有）；`auth_user_authorize`（生成 OAuth 授权链接：`accounts.feishu.cn/open-apis/authen/v1/authorize`，参数 scopes/redirect_uri/state/prompt=consent，缺省 scope `docx:document offline_access auth:user.id:read`）→ 用户授权 → **默认自动回调**（回调地址 = `GEBAI_PUBLIC_URL`（缺省 `http://localhost:{GEBAI_PORT|3000}`）+ `/api/v1/oauth/feishu/callback`，首次使用需在开发者后台「安全设置 → 重定向 URL」登记）→ **REST 回调端点自动兑换并写回发起授权的会话**（无需粘贴 code）；回调失败时保留手动路径：粘贴带 `code` 的回调地址 → `auth_user_token`（**OAuth v3 token 端点 `accounts.feishu.cn/oauth/v3/token` 兑换**（grant_type=authorization_code，携带 client_id/client_secret）；`code` 从完整回调地址自动提取，state 与授权时比对防混淆；尽力绑定用户信息）→ `auth_user_status` 查看/`auth_user_clear` 清除。**令牌按会话存储**（会话目录 `feishu_user_token.json`，`UserTokenStore` 依赖注入可测试，绝不输出明文）；**配置后资源类接口（docx/drive/sheets/bitable/wiki/board/搜索）自动以用户身份调用**（创建资源归用户所有、读写用户文档无需再添加应用协作）；access 过期**自动用 refresh_token 刷新**（单次有效、轮换落盘），刷新失败（授权超 365 天等 OAuth 错误 20037/20064/20073 附提示）回退应用身份并提示重新授权；**99991679（用户令牌缺权限）自动附 `permission_violations` 缺失 scope 清单 + 重新授权引导，且失效缓存重读一次自动重试**（重新授权后新令牌自动生效）；OAuth 错误码（20003/20004/20065 授权码失效、20071 回调不匹配、20074 未开启刷新开关等）附可读提示；**REST 回调端点 `GET /api/v1/oauth/feishu/callback`**（公开免鉴权，state 即会话关联凭证：随机不可猜、一次性消费防重放；兑换成功发布 `event oauth.completed` 并回显绑定用户；失败页附可读原因）
- **云空间 drive**：文件清单/文件夹创建/**元信息（走 `metas/batch_query`，`files/{token}` 对 docx 返回 404；缺省 type 自动识别失败时自动按 `doc_type=file` 补查）**/上传（multipart，支持 base64）/下载（落会话 tmp/，文件名经 batch_query 获取；**下载请求携带 `Range: bytes=0-` 头**——导出文件缺 Range 返回 403；**导出产物是 media 类型 token，`files` 接口 403/404 时自动回退 `medias` 下载接口**）/删除
- **电子表格**：创建（v3）、工作表信息（**合并返回完整工作表列表 `sheets/query`**）、读写（v2 values，`range` 前缀**支持工作表名称自动解析为 sheet_id**（用名称读写本须 sheet_id，直接用报 90215）；**前缀与任何工作表名/sheet_id 均不匹配时本地直接报错并列出可用工作表清单**——常见误因：误用表格标题作前缀，替代接口侧 90215 盲错；sheet_id 按 `sheets/query` 返回字段匹配透传，不依赖 oVs 前缀形态）、追加行（**单格 range 自动扩展为覆盖全部数据行的区域**，只传起始格追加多行不再报 wrong range）
- **多维表格**：创建（**支持 `fields` 字段定义数组创建后自动建自定义字段**——默认表只有基础字段，写入自定义字段名报 FieldNameNotFound；type 枚举：1 多行文本/2 数字/3 单选/4 多选/5 日期/7 复选框/11 人员/13 电话/15 超链接，单选多选需 `property.options`，逐个 POST `/tables/{id}/fields`；**创建后平台默认自动生成 10 条空占位记录（平台行为）**，写入时直接更新/追加即可）、数据表列表、记录增删改查（批量新增/更新/**删除走 `records/batch_delete`（POST，body 为字符串数组 `records: [\"rec1\",...]`，对象数组实测报错；DELETE 的 body 不生效）**、查询支持 filter——**filter 走 `records/search`（POST body），GET `?filter=` 报 InvalidFilter**）
- **知识库**：空间列表、创建节点（创建后可用 Markdown 写入正文）、按 token 查询节点
- **搜索**：云文档搜索（`suite/docs-api/search/object`，需开通「云文档搜索」权限）
- **权限**：添加协作者（`drive/v1/permissions`）、**链接分享设置（`set_link_share`：**PATCH** `permissions/{token}/public`（实测修正：方法为 PATCH 非 PUT，PUT 404），type 必填缺失 404，link_share_entity 枚举 tenant_readable（缺省组织内可读）/tenant_editable/anyone_readable/anyone_editable/closed）**
- **目标文件夹（让用户直接拥有资源权限）**：`FEISHU_DOCS_FOLDER_URL`（或全局 `GEBAI_FEISHU_FOLDER_URL`）配置用户的飞书文件夹（URL 或 folder token；路径含 /folder/{token} 自动解析）——创建类接口（create_doc/import_markdown 新建/create_sheet/create_bitable/create_folder/upload_file）未显式传 `folder_token` 时缺省落在该文件夹下（显式传参优先），用户在飞书里对该文件夹有权限即自动拥有其中资源（无需逐个分享/转移）；未配置则落应用云空间根目录（归应用所有，创建结果附落位说明）；配置值无法识别（既非 URL 也非 token）时报错提示修正而非静默忽略；权限类操作受限（`add_permission` 9999166x/403 等）自动附「文件夹方案」引导
- **兜底**：`api_call` 直接调用任意 `/open-apis/` 接口（自动携带 tenant_access_token，路径白名单校验），保证新接口零等待可用
- **读写三通道（XML 主 + Markdown 快 + 块级底层）**：文档读写提供三条 round-trip 通道——**XML 通道**（`fetch_doc` 读 / `update_doc` 改 / `import_xml` 建，同一套排版语法，保真度最高）与 **Markdown 通道**（`get_doc_text format=markdown` 读 / `import_markdown` 写，轻量快捷）。`docx_markdown.ts` 纯函数层（与 `markdownToBlocks` 严格对称）把块树反向序列化为 Markdown：标题层级/段落（含行内加粗斜体删除线行内代码链接与首行缩进）/列表与嵌套（连续同类列表项聚合、有序编号递增、todo 勾选态）/代码块（围栏语言由枚举名反推，再导入映射回同一枚举）/引用/分割线/表格（含列宽丢弃但行列与转义保留）/高亮块（→ GitHub 告示语法，emoji 映射告示类型）；**超出 Markdown 子集的块（分栏/高亮块配色/表格列宽/画板/图片素材等）按近似形式降级并在输出尾部附说明（`<!-- 说明：… -->`），不静默丢内容**。**两侧转义与解析互为逆**（实测修复）：`escapeMd` 行内元字符转义 + `escapeMdLine` 行首块级标记转义（仅对内容，不对自己生成的语法标记转义），解析端 `textElements` 新增**反斜杠转义支持**（私有区占位 → 末尾还原）——否则读出的 `\*x\*` 会被误读为斜体；块片段间以空行分隔（缺空行会让再导入时相邻块被合并/误判：列表并入段落、表格粘连）。选型原则写入提示词：需富排版（自动编号/分栏/配色/列宽/题注）或改已有文档 → XML；纯文字/简单结构、或要一段可直接复制的 Markdown → Markdown；要读原始块字段或按 block_id 做点操作 → 块级底层工具。图片插入三条路径统一（XML `<img path>` / Markdown 图片语法 / `insert_image` 独立工具，后者走 `insertWithMedia` 同源素材回填）；写入失败统一走 `docxCall` 本地诊断（区分文档不存在/无权限/参数不合法，`update_doc` 经 `insertGroups` 同样受益）。**块级底层工具（第三条通道，与前两条并存）**：`get_doc_blocks`（outline 大纲 / compact 一行一块带 id / full 全量块 JSON，`outline`→`docOutline`、`compact`→`compactBlocks` 纯函数）、`list_blocks`（指定父块子块列表，`page_all` 自动翻页 ≤2000）、`find_blocks`（按文本反查 block_id + `context_before`/`context_after` 上下文）、`add_blocks`（直接写块 JSON，含 `table.rows` 简化写法与 callout/grid 容器归一）、`update_block`（单块文本/插入/表格属性，`insert_text` 失败自动降级整体替换）、`delete_blocks`（父块下标半开区间 `[start,end)`）、`replace_text`（`replaceInBlocks` 纯函数逐 run 替换保留行内样式，跨 run 命中单列 crossRun）、`insert_image`（三步流程：空 image 块 → medias 上传 → replace_image）。**定位**：XML 通道负责「整篇成型与整体改写」（表达力最全、一次成型）；块级工具负责「拿到 block_id 后的精确点操作」与原始字段读取（样式/配色/合并单元格等 XML 未暴露的细节）

凭证从环境变量读取（子Agent 前缀规范）：`FEISHU_DOCS_APP_ID` / `FEISHU_DOCS_APP_SECRET`（兼容全局 `GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`，见「常量与环境变量」表）；**目标文件夹 `FEISHU_DOCS_FOLDER_URL`（兼容全局 `GEBAI_FEISHU_FOLDER_URL`）配置用户自己的文件夹后，创建类接口缺省落在该文件夹下**——用户在飞书里对该文件夹有权限即自动拥有其中资源，未配置时落应用云空间根目录（归应用所有，创建结果附落位说明）；统一使用 `tenant_access_token`（应用身份），仅能访问**应用自有资源**（应用云空间），访问用户文档需文档所有者授权；token 模块级缓存（有效期提前 200s 刷新）；写操作（创建/修改/删除/上传/授权）全部 `requiresApproval`；工具经 `createFeishuTools(deps)` 依赖注入工厂（fetch/token 缓存可注入），`markdownToBlocks`/`textElements`/`stripTableMergeInfo` 为纯函数，可独立单测（行覆盖率 95%）。**提示词纪律**：先确认落位（创建类操作前明确资源落在配置的目标文件夹还是应用云空间，未配置且用户本人要用时给出配置方式或先 `add_permission` 分享）；先读后写（修改/插入前先 `fetch_doc` 读目标区域确认当前内容（官方五步：Observe→Diagnose→Patch Plan→Patch→Verify），防基于过期内容操作）；写操作前说明改动点与影响范围（审批预期）；批量写入自动分批，限频 3 次/秒失败等待重试，不并发轰炸同一接口；**文档创作与排版**（`feishu_docs.md` 独立章节）：整篇创作走两阶段——① 读规范（`style_guide` 读 `style` 总纲与对应体裁契约，用 XML 时补读 `xml`）② 一次成型（整篇 `import_xml`，Markdown 草稿/简单追加用 `import_markdown`）③ 回查（`fetch_doc` outline/section/keyword 定位读取）④ 精修（`lint_doc` 体检报告逐条最小范围修复，每轮重查、不沿用旧 block_id）；排版原则（读者本位/结构先行/视觉服从语义/克制连贯/编号体系唯一/颜色表达语义）与 Markdown→飞书块对照表保留在提示词内（摘要），完整规范与自检清单在 `style_guide` 技能文档内（按需读取，不占常驻上下文）。

#### `playwright`（浏览器自动化）

实现于 `packages/agents/src/agents/playwright/`（目录形式：`playwright.ts` 定义入口 + `playwright.md` 系统提示词 + `playwright_tools.ts` 工具集 + `playwright_session_tools.ts` 会话类工具集 + `playwright_cv_tools.ts` 本地识别工具集；桥接基建与 node 驱动在 `core/browser/`，见「桥接架构」），基于无头 Chromium（playwright）提供浏览器自动化：

- **工具集**（30 个）＝ 导航与内容（`playwright_tools.ts`）+ 会话类（`playwright_session_tools.ts`）+ 本地识别（`playwright_cv_tools.ts`）：
  - 导航/内容/交互（`playwright_tools.ts`，20 个）：`open`（导航，支持 http/https 与 **file:// 本地文件**，可配 wait_until；失败输出「打开失败: …」+ 排查建议）、`content`（text/html/both 读取，超长自动截断落盘）、`screenshot`（页面/元素/整页，保存会话 `tmp/` 并返回图片块与绝对路径）、`click`/`fill`/`press`/`select`/`check`（交互与表单；`check` 的 checked 非布尔值直接报错，不当 true 静默处理）、`hover`（悬停展开菜单）/`dblclick`（双击）/`drag`（元素拖拽，locator.dragTo）/`upload`（文件上传 setInputFiles，files 经 `resolvePath` 沙箱解析）、`wait_for`（元素/URL/网络空闲等待）、`evaluate`（页面内 JS 执行，JSON 序列化结果；**表达式与函数字面量 `() => …`均支持**（后者自动调用，否则求值得函数对象而序列化为 undefined）；失败提示改用 `content` 观察 DOM；**求值失败时输出保留页面内错误原文并附求值形态与表达式首行**——页面内 `fetch` 取 JSON 前先验响应（`r.ok`/`content-type`），对非 JSON 响应（如 404 的 `Not Found` 文本）调 `r.json()` 会抛 JSON 解析错误，那是响应体问题而不是本工具异常）、`pages`/`new_page`/`switch_page`/`close_page`（多标签页管理；**浏览器内部页**（edge://downloads-hub 等 WebUI）不进标签页索引，`close_page` 有界等待 5s，超时转「忽略」而不拖死桥接请求）、`close`（关闭会话浏览器上下文）、`serve_dir`（**内置静态文件服务器**：服务本地目录返回 http 地址，解决本地 HTML 需手工起服务的场景；服务端部署仅可服务预置项目目录）
  - 会话类（`playwright_session_tools.ts`，7 个）：`pdf`（页面导出 PDF 到会话 `tmp/`）、`downloads`（**下载捕获**：页面触发的下载自动存到系统临时目录，list 查看 / save 复制进会话目录）、`dialogs`（**对话框管理**：alert/confirm/prompt 记录 list + 自动应答 auto（accept/dismiss，prompt 可配文本）+ clear；无配置时默认 dismiss 防页面冻结）、`emulate`（**环境仿真**：视口/UA/语言/时区/移动端模式（isMobile+hasTouch），action=reset 恢复默认）、`cookies`（cookie list/set/clear，输出真实凭证）、`local_storage`（当前页 origin 的 localStorage 读写，driver 定式 evaluate 非任意脚本）、`storage_state`（**登录态持久化**：save/restore playwright storageState 格式（cookie+各 origin localStorage）文件，跨会话/跨任务免重复登录；restore 逐 origin 开临时页写 localStorage，不可达 origin 告警不中断）
  - 本地识别（`playwright_cv_tools.ts`，3 个，只读免审批）：`ocr`/`locate`/`locate_image`——**复用 desktop 同款 core/cv 小模型基建**（共享工厂 `@gebai/agents` core/shared/cv-analysis，见「小模型识别」），缺省图像源注入为「共享桥接截当前页视口」（与 playwright 工具同一桥接单例与会话锁；临时文件**每次调用唯一命名**（序号+随机，并发调用不互相覆盖）且用后即删；image 参数传 PNG 时同 desktop 语义）：`playwright_ocr`（读当前页面/图片文字：region 限定区域、find 过滤，返回行文本+框+中心坐标+置信度）、`playwright_locate`（按目标文字定位视口像素坐标）、`playwright_locate_image`（模板匹配定位图标/图形——验证图标/logo 真实渲染）；坐标一律**视口像素系**（`evaluate` 的 `document.elementFromPoint(x, y)` 定位元素后直接操作）；浏览器为隔离环境，**无 desktop 的本地模式闸门，服务端部署（沙箱）可用**；定位 canvas/WebGL 渲染文字、图片化文字/验证码等 DOM 读不到内容的兜底通道（普通文本/元素仍优先 content/选择器），语义理解按需装载 vision 子代理（vision_analyze）；**环境耦合的缺省源便利留在域内（复用方式二），不依赖 vision 子代理**（边界判定见「视觉能力分层与子代理复用边界」）
- **选择器穿透 iframe**：selector 支持 `iframe选择器 >> 子iframe选择器 >> 目标选择器`（`>>` 分段，前段逐级 `Locator.contentFrame()`），click/fill/content/screenshot 等全部带 selector 的工具生效；CSS 引擎天然穿透开放 shadow DOM
- **桥接架构（core/browser/bridge.ts，平台级基建）**：Bun 运行时与 playwright driver 的 pipe 通信存在兼容问题（chromium 启动超时，node 环境正常），因此 **Bun 进程内不直接 import playwright**——桥接 spawn 常驻 `node driver.mjs` 子进程（driver 在 `core/browser/driver.mjs`），经 stdin/stdout 行分隔 JSON-RPC 通信（请求 id 匹配、180s 超时杀进程重启、stderr 环形缓冲诊断；**kill 按 pid 整树终止**——Windows 无进程组，`proc.kill()` 只终止 driver，其拉起的浏览器孙进程会成孤儿驻留（占内存与 profile 临时目录），故 Windows 下先 `taskkill /pid <pid> /t /f`；**driver 回传的错误信息剥除 ANSI 转义码**（playwright 的 Call log 自带颜色码，回传模型是噪音））；driver 内 Browser 单例（断开自动重建），BrowserContext 按 sessionId 隔离（多用户/多会话互不串扰，`acceptDownloads: true` 创建），空闲 10 分钟惰性回收；页面级监听（dialog/download/websocket 帧）经 `context.on("page")` 统一挂接；Bun 侧按会话串行化工具调用（同会话防并发操作同一页面，`withSessionLock`）。桥接进程与浏览器会话**全进程共享单例**（`createLazyBridge()` 惰性构造——首次工具调用才解析 playwright 模块并启动桥接进程，playwright 与 reverse_site 两命名空间及透明浏览器代理操作同一浏览器，页面状态一致；解析失败降级为工具级报错，不影响服务启动）。基建属 core 域（`core/browser/`），**不依赖任何子Agent 定义的存在**——playwright 被裁剪/停用时子Agent 工具面消失，但桥接与透明浏览器代理照常可用
- **网络录制（driver 扩展能力，reverse_site 使用）**：driver 额外提供 `network_*` 操作——按会话录制浏览器上下文内的全部请求（方法/URL/类型/请求头/请求体/响应状态/响应头/响应体预览，单会话上限 500 条，响应体预览上限 20KB，entry 保留 Response 对象供完整响应体提取），**录制时自动脱敏**（`authorization`/`cookie`/`x-api-key`/`set-cookie` 等敏感头与 postData 中 `token`/`password`/`secret` 等键值一律 `***`）；`network_list` 支持按 method/url（正则或子串）/status 过滤、detail 详情开关与 limit 条数上限（HAR 全量导出用）
- **透明浏览器代理（`GEBAI_BROWSER_PROXY=1`，服务启动时 `boot/compose.ts` 安装、重启生效）**：内网「仅限浏览器访问」（非浏览器进程直连被拒）场景的进程级 fetch 垫片——`core/browser/fetch-proxy.ts` 替换 `globalThis.fetch`，**工具执行作用域内**（引擎 `runToolInterruptible` 统一进入 `runInToolFetchScope(sessionId)`，原语在 `core/support/fetch-scope.ts` 的 AsyncLocalStorage）的 http(s) 请求自动改经共享浏览器会话的 `context.request` 发出（携带浏览器 UA 与会话 cookie/登录态），由 driver `http_fetch` 操作承载（响应体经系统临时文件中转——二进制安全、绕开桥接 JSON 结果上限，读毕即删；返回**未脱敏**响应头，垫片构造回标准 `Response`，模型可见输出的脱敏仍由各工具层自行负责；单次请求超时 110s）。**平台级能力：不依赖任何子Agent**——安装点在组合根（boot/compose），桥接基建在 core/browser，playwright 被裁剪/停用不影响代理可用；工具代码与模型零感知（工具 schema/系统提示词均不变），任何子Agent（含 self_optimize 生成的新Agent）用普通 `fetch` 即可访问受限内网站点。边界：作用域外（LLM 请求/webhook/调度/启动逻辑）不受影响；嵌套引擎（`subsession_run` 在工具内跑完整引擎）的 LLM 流式请求经 `runWithoutFetchProxy` 豁免（`core/llm/llm.ts` fetchRetry 单点包裹）恒直连；sh/py/js **子进程**内网络不经此垫片；`Request` 对象输入与 FormData/Blob 等非字符串请求体回退直连（multipart 上传等复杂形态）；`redirect:"manual"` 映射 `maxRedirects=0`（`fetchWithRedirectGuard` 逐跳 SSRF 校验语义保留——沙箱公网限制不因代理放宽）；AbortSignal 语义保留（触发即 AbortError）。未开启（缺省）时安装为 no-op 零开销
- **仿真档案语义**：UA/视口/locale/timezone 为上下文级选项，只在 newContext 时生效——`emulate` 存档后**立即重建上下文应用**（已打开页面/cookie/录制状态清空，拦截规则自动重挂；工具描述与提示词均注明「需保留登录态先 storage_state save」）
- **依赖与部署**：运行时需宿主机可执行 `node`。playwright 模块解析顺序：**单二进制形态**优先物化内嵌 playwright-core（`packages/server/scripts/build-pwcore-embed.ts` 整树 gzip base64 内嵌 → 运行时物化 `{GEBAI_HOME}/vendor/playwright-core/`，版本不一致自动重建）；回退 `playwrightModuleUrl()` 经 `Bun.resolveSync` 解析 node_modules 的 playwright 包（源码/服务端部署形态）。浏览器本体不内嵌：Windows 默认 `channel=msedge` 驱动系统自带 Edge（Win10/11 必装，免下载浏览器），`GEBAI_PLAYWRIGHT_CHANNEL` 覆写（如 `chromium`/`chrome`，留空 = 不指定 channel、用默认 chromium——需 `bunx playwright install chromium`，Linux 服务端部署即此路径）。模块缺失/解析失败降级为**工具级报错**，不影响服务启动与其他子Agent。构建时 `driver.mjs` 由 `build-subagents.ts` 复制到 `dist/` 与产物同目录（不能被 bundle 内联）
- **审批**：导航/交互/脚本/服务/凭证/仿真类（`open`/`click`/`fill`/`press`/`select`/`check`/`hover`/`dblclick`/`drag`/`upload`/`evaluate`/`new_page`/`serve_dir`/`emulate`/`cookies`/`local_storage`/`storage_state`）默认需审批——服务端部署下可被诱导访问内网/提交表单，防 SSRF 与任意脚本滥用；**凭证类（cookies/local_storage/storage_state）输出真实值，审批门控与 evaluate 同级**；只读类（`content`/`screenshot`/`pdf`/`downloads`/`dialogs`/`pages`/`wait_for`/`switch_page`/`ocr`/`locate`/`locate_image`）与清理类（`close_page`/`close`）免审批
- **提示词纪律**：等待一律 `wait_for` 目标条件（不用固定 sleep——异步渲染时序不可靠）；被 `code`/`self_optimize` 委托做浏览器端验证时明确验证目标→操作→content/screenshot 取证→给出结论与证据位置；交互类操作前先说明意图（审批预期）；凭证输出只用于用户明确要求的登录态分析/注入，不外传不入文档

#### `reverse_site`（网站/接口逆向）

实现于 `packages/agents/src/agents/reverse_site/`（目录形式：`reverse_site.ts` 定义入口 + `reverse_site.md` 系统提示词 + `reverse_site_tools.ts` 工具集 + `reverse_site_net_tools.ts` 网络增强工具集 + 各自 `*.test.ts` 测试），面向「逆向网站与接口」场景——摸清站点结构、还原接口契约、产出分析文档，并可与 `self_optimize` 联动把逆向结果转化为新的子Agent：

- **依赖 playwright（`dependencies: ["playwright"]`，装载/预加载自动连带，见「子Agent 依赖与自动装载」）**：浏览器自动化全套由 playwright def 提供（`playwright_` 前缀：导航/内容/交互/截图/多标签 + 会话类 pdf/downloads/dialogs/emulate/cookies/local_storage/storage_state，审批映射亦由 playwright def 单源维护）——本 def 不复刻任何浏览器工具；capture_*/route 经共享桥接单例（`createLazyBridge` 全进程共享）与 playwright 操作**同一浏览器会话**，页面操作与网络录制天然一致；playwright 被启停名单移除/构建裁剪时跳过（本 Agent 仅剩接口直连探测）
- **工具集**（11 个，全部 `reverse_site_` 前缀）＝ 接口捕获与重放（`reverse_site_tools.ts`：`capture_start`/`capture_stop`/`capture_clear`/`capture_list`/`capture_body`/`capture_replay`/`capture_curl`）+ 网络增强（`reverse_site_net_tools.ts`：`capture_har`/`capture_ws`/`route`）+ 接口探测（`http_request`）；文件读写与编排（write/read/fetch_url/subsession_run）为全局工具直接用全局名（提示词开头列明三段命名空间映射）
- **接口捕获**（`capture_*`）：录制浏览器网络请求还原接口——浏览前 `capture_start`，浏览/操作页面让 XHR/fetch 自然发生，`capture_list` 分析（默认摘要，`detail=true` 含请求头/体与响应头/体预览，可按 method/url/status 过滤，`file` 参数把完整记录导出会话 `tmp/` JSON）；`capture_body` 取指定请求的**完整响应体**（文本预览 / `file` 落盘支持二进制与大文件，绕过桥接结果上限直接写盘）；`capture_har` 把录制导出 **HAR 1.2**（Chrome DevTools 可打开，脱敏口径与录制一致）；`capture_ws` 查看录制的 **WebSocket 帧**（行情/聊天/通知类实时接口，随录制开关联动，单会话 300 帧/单帧 4KB 预览）；录制在 driver 侧完成（见 playwright 网络录制），**敏感字段（cookie/token/密码等）自动脱敏**
- **带登录态重放**（`capture_replay`）：以 `context.request.fetch` 重放录制的请求——**自动携带浏览器 cookie/存储状态，可重放需登录接口**（`http_request` 做不到）；`method`/`url`/`headers`（覆盖合并原始请求头）/`body`/`params`（合并进 URL 查询串）为覆盖项，改参试边界一步到位；沙箱模式目标 URL 过公网校验且**不自动跟随重定向**（3xx 返回 Location 由模型逐跳改 url 重放，防跳板 SSRF）；`capture_curl` 从**原始未脱敏请求**生成可直接运行的重放命令（curl/fetch/python，`buildReplayCommand` 纯函数）——与 `http_request` 免密直连互补，含真实凭证故审批门控
- **请求拦截**（`route`）：`context.route` 拦截浏览器内全部请求（持续到 clear/会话结束，单会话上限 32 条）——mode=block 中止匹配请求（屏蔽广告/埋点/干扰资源）、mode=mock 伪造响应（status/content_type/body——前端联调、绕过客户端 gating 验证）、mode=modify 改写请求头后放行；上下文重建（emulate）后自动重挂；**作用于浏览器上下文内流量，无新增网络出口**，list/clear 免审批、add 需审批（工具级函数按 action 判定）
- **接口探测**（`http_request`）：直接发送 HTTP 请求验证逆向出的接口（任意方法/请求头/请求体/查询参数），返回状态码、响应头（敏感字段脱敏）与响应体（50KB 内截断展示，超长走截断保护）；服务端部署限公网地址（复用 `assertPublicHttpUrl` 防 SSRF，默认路径另带重定向逐跳校验）；fetch 可注入（测试替身）；默认需审批。`GEBAI_BROWSER_PROXY=1` 时本工具与一切进程内工具 fetch 透明经浏览器代理（见 playwright「透明浏览器代理」）——受限内网站点照常探测，沙箱公网限制不放宽
- **编排闭环**：分析文档写会话 `tmp/`（`site_map.md` + `api_docs.md`/`api_docs.json`，示例请求可用 capture_curl 产物）后经全局 `subsession_run` 把文档交给 `self_optimize`，由 `self_optimize` 生成/修改子Agent 定义文件（`packages/agents/src/agents/{name}.ts` + 可选 `{name}.md`）并通过测试验证——「网站 → 逆向文档 → 新子Agent」全链路
- **审批**：`http_request`/`capture_replay`（直连探测）/`capture_curl`（真实凭证）默认需审批；浏览器交互类与凭证类（cookies/local_storage/storage_state）随 playwright def（连带装载生效）；`capture_start/stop/clear/list/body/har/ws` 与 `route list/clear` 只读免审批
- **提示词约束**：只逆向用户授权/自有网站，敏感信息不扩散，不爆破/不拖库/不高频恶意请求；`capture_list` 先摘要定位候选再 `detail=true` 细看（超长记录 `file` 导出后 `read` 分块分析）；验证优先级 replay（带登录态最快）> curl 命令交付 > http_request 直连；route 用完即清（防干扰后续捕获）；验证多通道——截图/读取失效切 evaluate/capture_list/replay/http_request，失效即切换并告知用户

#### `explore`（只读代码探索）

实现于 `packages/agents/src/agents/explore/explore.ts`（目录形式），参照 ZCode Explore 子代理设计的**只读探索专家**——把「大范围扫读」从主上下文中剥离出去，主会话只拿结论：

- **用途**：跨大量文件的代码摸底/架构梳理/多点位定位——`code`（或总Agent）经 `subsession_run` 委托执行，广度优先搜索后返回**结论 + 文件:行号 引用清单**，中间搜索/读取过程留在子会话存档里，不占父上下文（防上下文膨胀的标准委托形态）
- **硬约束（代码级）**：工具集只声明只读独有工具（search_symbols/analyze/git，全部免审批），文件读取与检索（read/ls/grep/glob/fetch_url/todo）复用**全局只读工具**（subsession_run 委托默认继承全局工具）——没有 write/edit/sh/py/file（含 delete/move 动作），探索不可能产生任何修改或命令执行；需要修改时装载/委托 `code`
- **工作流（提示词内置）**：圈定范围（project 参数/任务给定根；ls/glob（支持花括号与 exclude）看结构、Git 项目用 git ls-files 拿已跟踪文件清单（尊重 .gitignore）、git grep 已跟踪文件内容搜索、grep 宽泛定位先 output=files（exclude 排噪、literal 字面搜代码片段））→ 抽查精读（关键文件 read 分段读（默认带行号，直接引用 文件:行号）、search_symbols 定位定义与**调用点**（mode=references——注释/字符串不误报，梳理调用链优先于 grep）、grep 定位文本（output=count 先估命中面）、analyze 结构概览代替通读）→ 汇总结论（**先结论后细节**，关键位置给 文件:行号，未确认点明确标注不猜测）→ 规模纪律（命中面大先 output=count 估面再挑重点；只保留与目标相关的结论）
- **项目路由**：独有工具经 `projectAware` 包装（实现位于 `core/tools/projects.ts`，与全局文件工具共享同一基础设施）——支持 `project` 参数按预置项目名/路径/保留名 tmp 路由，路径相对所选根解析；受限模式（CODE_RESTRICT_PROJECTS）同规则生效
- 与 `code` 的分工：`code` 是「探索→方案→修改→验证」全流程执行者；`explore` 只做其中的「探索」段且产出为结论——大范围摸底先委托 explore 拿地图，再由 code 精确改动，两段各司其职



#### `feishu_group`（飞书群基础能力）

实现于 `packages/agents/src/agents/feishu_group/feishu_group.ts`，以应用身份（tenant_access_token）操作飞书群基础能力——群查询、群维护与任务通知的取材（@ 人 open_id / 群 chat_id）：

- **工具集**（十工具，`feishu_group_` 前缀）：
  - 查询类（免审批）：`chats_list`（机器人所在群分页列表——chat_id/名称/描述）；`chat_info`（群详情：名称/描述/群主/成员数）；`members_list`（群成员分页列表：open_id+姓名+成员类型——**@特定人的 open_id 来源**）；`user_info`（按 open_id 查用户姓名等信息——核对 at 名单）
  - 写操作（需审批）：`message_send`（向群发文本，正文支持 `<at user_id="open_id">` @特定人与 `<at user_id="all">` @所有人——验证通知效果/直接推送）；`chat_create`（建群：名称/描述/初始成员）；`chat_update`（改群名/描述）；`chat_members_add`（拉人，open_id 列表，失败项逐条给原因）；`chat_members_remove`（移出成员）；`chat_disband`（解散群，不可恢复）
- **凭证**：`FEISHU_GROUP_APP_ID`/`FEISHU_GROUP_APP_SECRET`（子Agent 前缀），缺省回落全局 `GEBAI_FEISHU_APP_ID/SECRET`（与机器人桥接/云文档共用）；机器人须已入群；token 缓存复用 feishu_docs 同款机制（提前 200s 刷新）
- **与任务通知联动**（高频场景）：`chats_list` 查到的群 chat_id 直接填任务通知 `feishu` 通道 `target`（指定群以应用身份推送）；`members_list` 查到的 open_id 直接填 `at` 名单（@特定人）
- **权限提示**：查询需 `im:chat:readonly`（或 `im:chat`），写操作需 `im:chat`，发消息需 `im:message:send_as_bot`，用户信息需 `contact:user.base:readonly`——权限不足的错误附开发者后台开通引导
- **预加载**：`preload = false`，按需装载


#### `task`（统一任务管理）

实现于 `packages/agents/src/agents/task/task.ts`（工具名经命名空间为 `task_*`），管理与执行**用户级**任务——定时（`scheduled`）/普通（`manual`）/闲时（`idle`）三类共用一份存储与一条队列（能力实现见「统一任务管理」），支持脚本运行与提示词运行 agent、执行目标（新会话/专用会话/绑定会话）、时区、一次性 `@at`、错过补跑、执行记录（按文件落盘，见「统一任务管理 → 执行记录」）、连续失败自动停用、飞书群与 Webhook 通知（含模型主动推送 `task_notify`）、任务资源文件：

- **工具集**（八工具，命名空间内单字 `add`/`list`/`update`/`run`/`cancel`/`remove`/`files`/`notify`）：
  - `add`（`runner` 必填，`kind` 缺省按是否给 `schedule` 推断）：创建任务；可选 `name`/`script`/`prompt`/`schedule`/`timezone`/`misfire`/`target`/`session_id`/`agents`/`timeout_ms`/`env`/`notify`/`notify_on`/`max_consecutive_errors`/`enabled`/`run_now`/`front`，返回任务行与资源目录提示
  - `list`：查看**当前用户全部**任务（ID/名称/类别/执行体/启用状态/运行态/周期/下次执行/次数/最近错误）+ 队列概览（并发额度、排队顺序与等待原因、运行中条目）
  - `update`（`id`）：按 id 修改（全部可变字段，含 `env`——传空对象 `{}` 清除），定时任务改后重算下次执行时间；`notify` 的 `secret` 传 `***` 表示保持原值；重新启用重置连续失败计数
  - `run`（`id`，可选 `front`）：手动执行一次（入队；不改动既定调度节奏），返回队列位置
  - `cancel`（`id`，`mode=dequeue|stop`）：出队（排队中）或终止运行中的那次执行
  - `remove`（`id`）：按 id 删除（不可恢复；任务资源目录文件保留）
  - `files`（`id`，`op=list|read|write|delete`）：任务资源目录（脚本/文档）读写，越界路径拒绝
  - `notify`（`text` 必填，可选 `title`/`id`/`at`）：主动推送一条通知——自撰 markdown 正文投递到任务配置的通道（未配则全局默认通道）；`id` 缺省时按当前会话反查正在运行的任务（任务执行中调用无需传 id）；投递目标限定为用户已配置的通道（不接受任意 URL），无可用通道时返回配置指引
- **审批**：`add`/`update`/`run`/`cancel`/`remove`/`files` **默认需审批**（任务 = 无人值守的任意命令/会话执行，创建/修改/删除/执行/资源文件写入均须用户确认，服务模式下防普通用户绕过审批边界创建后门任务）；`list` 与 `notify` 免审批——无人值守执行等不到人工审批，且通知的投递目标被限定为用户已配置的通道（任务创建时已经过审批，不新增任意外发面）。**执行期**（调度器触发的 prompt 型任务，`target=ephemeral/sticky`）需审批工具**一律自动通过**（含服务模式）——管理动作逐次确认、执行职责由任务授权承担（见「统一任务管理 → 执行免审批」）
- **能力开关**：`GEBAI_TASKS_ENABLED` 默认 `true`；显式 `false` 时 `task` 子Agent 不注册（定义从子Agent 清单移除——`agent_list`/`agent_load`/`subsession_run` 均不可见，与调度器、REST 管理面一致完全隐藏）；`ctx.tasks` 未注入（引擎未挂调度器）时工具返回「能力未启用」提示
- **用户级绑定**：任务经 ToolContext 绑定**当前用户**（与会话解耦——任何会话创建后该用户全局可见可管，`TaskManager` 校验用户归属，跨用户不可见不可操作；`originSessionId` 仅记录创建来源会话供结果消息写回）；执行记录同样按用户归属校验（读非本人任务记录报「任务不存在」）
- **环境变量**：任务级 `env`（可选，两类执行体通用）由工具参数 / REST body 传入并服务端持久化；敏感键名的值 AES-256-GCM 加密落盘（非敏感明文）、回显为掩码 `***`（回传掩码即保留原值）；无人值守任务不依赖浏览器本地 env 注入（见「统一任务管理 → 任务级环境变量」）
- **预加载**：`preload = false`，按需装载（与其余子Agent 一致）


#### `wps`（Office 文档处理）

实现于 `packages/agents/src/agents/wps/`（目录形式：`wps.ts` 定义入口 + `wps.md` 系统提示词 + 按格式拆分的工具 `word.ts`/`excel.ts`/`batch.ts`/`ppt.ts`/`pdf.ts` + 共享基础 `ooxml.ts`/`markdown.ts`/`shared.ts`），Word/Excel/PowerPoint（Office Open XML：.docx/.xlsx/.pptx）与 PDF 的创建/读取/追加/编辑与富排版：

- **文档库**（非 AI 依赖）：`docx`（Word 生成——标题样式/编号列表/表格/图片/页眉页脚/目录域）、`exceljs`（Excel 读写——公式/样式/合并/冻结/筛选）、`pptxgenjs`（PPT 生成——版式/图表/表格/形状/备注；**Bun 严格模式下文本数组必须归一为 `{text, options}` 对象形态**，裸字符串数组触发只读属性赋值错误）、`fflate`（ZIP 解包——docx/pptx 是 ZIP 容器，读取与 word_append 重打包共用）、`pdf-lib`+`@pdf-lib/fontkit`（PDF 生成与页面编辑）、`unpdf`（PDF 文本提取——内嵌 pdf.js）；XML 部件解析复用 `happy-dom` 的 `DOMParser`（`text/xml` 模式）
- **正文输入双形态**（word_create/word_append/pdf_create 共用，`markdown.ts` 迷你解析器与 blocks JSON 归一化（`toRuns`/`normalizeBlocks`/`bodyInput`）自实现——文档生成只需受控子集，不引入 markdown 库）：markdown 文本（`#` 标题/行内样式（`**粗** *斜* ~~删~~ \`码\`）/`[链接](url)`/有序无序列表（缩进分级）/表格（对齐）/引用/围栏代码块/`![图](路径)` 嵌入/`<!--pagebreak-->` 分页/`<!--toc-->` 目录）或 blocks JSON（结构化块数组，逐 run 样式与图片定宽）
- **工具集**（十四工具，均 `projectAware` 包装——project 参数路由项目内文件）：
  - `word_create`：富排版建 .docx；`style` 调页面（A4/letter、横竖向、页边距 cm）与正文（字体/字号，默认微软雅黑 10.5pt）、页眉页脚（`{page}`/`{pages}` 页码占位）、文档属性（title）
  - `word_read`：解析回 markdown（标题→`#`、编号/列表、表格、内嵌图片 `[图片]` 占位；首行块数/图片数/页面尺寸摘要；`offset`/`limit` 按块分页读长文档）
  - `word_append`：**原 XML 拼接**——原文档格式/样式/图片原样保留，新内容以直接格式化 XML 插入 body 级 `sectPr` 之前（fflate 解包 → 生成片段 → 图片/超链接补 `document.xml.rels` 关系、媒体入包、`[Content_Types].xml` 补扩展声明 → 重打包；**写前回读校验**，失败不落盘防损坏用户文档）
  - `excel_write` / `excel_read` / `excel_edit`：全量建表（多工作表；单元格标量或 `{value, bold, italic, color, fill, font_size, align, wrap, number_format, border, hyperlink}`——hyperlink 为值形态 `{text, hyperlink}` 统一写入；`=` 开头字符串自动按公式，**写入不含缓存计算值**（Office 打开重算，读取默认模式无缓存值时回显公式原文并标注「（未计算）」——写入侧不计算，错误缓存值比无缓存值更误导）；列宽/合并/冻结 `A2`/自动筛选；**行容错**：单格行以对象/标量直传（未包数组）按单格行收敛并回报提示，不静默丢弃）；读取（不传 sheet 返回概览；markdown（默认）/json（rows 入 data 供 js 编排）/csv 三种输出；`range`（A1:D20 或 A:D）/`max_rows`/`formulas`；.csv/.tsv 文本表格兼容）；ops 批量编辑（`set` 设值设样式 / 行列增删 / `add_sheet`·`rename_sheet`·`delete_sheet` / `merge`·`unmerge` / `col_width`·`row_height` / `freeze`·`autofilter`，各项可带 sheet 指定目标表；改完回读校验）
  - `excel_batch`（`batch.ts`，批量表格数据管道——**去 Python/pandas 依赖**：多文件导入合并/批量导出/清洗/统计/透视一次声明式完成，数据全程 文件→文件 不进上下文）：源（`files` glob 模式/路径数组（xlsx/xlsm/csv/tsv）或 `rows` 内联小表，二选一）；多文件**按表头名自动对齐合并**（列序不同无碍、新列补空），`sort_files`（mtime/name/size）与 `limit_files` 控制取舍；csv 编码自动探测（BOM/UTF-8/GBK 兜底，`encoding` 显式指定）；**数值保守转换**（前导零 "0012"、≥11 位数字（手机号/证件号）保文本，千分位/百分比归数——编号误转数字是批量导入最高频事故）；动作管道 `actions` 依次应用：`filter`（列×操作符 eq/ne/gt/gte/lt/lte/between/in/not_in/contains/starts/ends/empty/not_empty）、`select_cols`/`rename_cols`/`fill`/`sort`（多列升降序，稳定排序）/`dedupe`/`top`/`slice`/`compute`（**微型表达式引擎**（安全递归下降求值，无 eval）：列引用 `{列名}`/`[列名]`，四则/比较/逻辑 + IF/ROUND/ABS/MIN/MAX/INT/FLOOR/CEIL/SQRT/LEN/CONCAT/UPPER/LOWER/TRIM/LEFT/RIGHT/TEXT/NUMBER/AND/OR/NOT/ISBLANK，同批多列**累进可见**——后列可引用前列）/`aggregate`（汇总行）/`join`（inner/left/outer，on 或 left_on/right_on，多匹配取首个、重名列加「_右」后缀）/`group`（多列分组×多聚合 sum/avg/count/min/max/distinct/first/last）/`pivot`（行×列透视+合计行）；列引用统一支持表头名/1 起始序号/字母、忽略大小写；输出 `outputs` 多目标同时写：xlsx（表头加粗+冻结+筛选+CJK 感知自适应列宽，数值/日期原生类型）/csv（utf-8/utf-8-sig/gbk——**GBK 编码器运行时反向构建**（TextDecoder("gbk") 双字节区解码反查表，无 iconv/Python 依赖，不可编码字符替换 ？并报告））/json（对象数组）；**管道原子性**（任一动作失败不写任何输出）+ 各阶段行数统计 + 头部预览（模型可自查）；守卫同 excel_write（writeGuard+防盲覆盖，outputs 预检先行）；`outputSchema` 声明 headers/rows/cols/inputs/outputs/stats 供 js 编排
  - `ppt_create` / `ppt_read`：简式页（title/subtitle/bullets/notes/background）与自由元素版式（`elements`：text 文本框/image 嵌图（会话/项目内路径，自动探测像素尺寸）/table 表格/chart 图表（bar/hbar/line/area/pie/doughnut/scatter，data `[{name, labels, values}]`）/shape 形状（shape 缺省 rect），坐标英寸）；**元素容错**：type 缺失/无法识别时按字段签名推断（path→image、chart_type/data→chart、rows→table、形状名/fill→shape、text→text）并在输出提示；页级误传元素对象（带 type）、或携带元素级字段（chart_type/data/rows/path）未包 elements 数组时自动按附加元素处理并提示；layout wide（16:9 13.33×7.5 默认）/4x3/自定义尺寸，theme 全局字体字号色；读取回每页文本（首个文本框按标题标记）/表格/备注（剔除页码占位符）/图表与图片计数
  - `pdf_create`：markdown/blocks → 排版 PDF（语法同 word_create——标题/行内样式/列表/表格（对齐/列宽/跨页重绘表头）/引用/代码块（灰底）/图片（仅 png/jpg，居中等比缩放）/`<!--pagebreak-->`/`<!--toc-->`）；**CJK 字体自动发现**（按平台扫描系统字体目录：win 微软雅黑/等线/黑体/宋体、mac 苹方/冬青黑体、linux Noto/文泉驿/Droid，`style.base_font` 可传字体族别名或 .ttf/.ttc 文件路径（会话/项目内或系统绝对路径；**仅支持 TrueType 轮廓**——pdf-lib 将自定义字体一律按 TrueType 位置嵌入（FontFile2/CIDFontType2），CFF 轮廓（OTTO 头 OTF，Noto/思源/PingFang 官方发行版）在该结构下不规范、渲染器拒绝光栅化（文本层完好），sfnt 魔数检测拒绝并跳过该候选）），TTC 先按 name 表挑 Regular/Bold 子字体再**二进制重建独立 TTF**（embedFont 只收字节），**子集化嵌入**（仅用到字形，产物小）；纯西文走标准 14 字体（Helvetica 族，探测到非 WinAnsi 字符自动回退自定义字体），粗体优先伴随字体（msyhbd.ttc/Dengb.ttf 或单 TTC 多字重子字体）无则回退常规；字体目录扫描为目录 + 一层子目录（`readdirSync recursive:true` 对 Windows Fonts 触发 EPERM）；`style.subset_font`（默认 true）子集化逃生舱——pyftsubset 等工具产出的字体再经 pdf-lib 二次子集化会损坏 glyf（渲染空白、文本层完好），字形异常时关闭改整字嵌入；**排版引擎自实现**（绘制指令两遍：CJK 逐字/拉丁按词折行、超宽原子硬切、keepNext 标题防孤行、表格逐行分页）；**目录两遍布局**（pass1 虚拟布局记标题页码（目录占 0 页）→ 按条目折行估目录页数 k → pass2 渲染目录（标题+页码+虚线引导）独占页、其后标题页码=虚拟+k）；页眉页脚 `{page}`/`{pages}` 逐页替换居中；style 同 word（page_size/orientation/margins cm/base_font/base_size/header/footer/title）
  - `pdf_read`：pdf.js（unpdf `getDocumentProxy` 低层 API）逐页提取文本层（`## 第 N 页` 分节）+ 首行元数据摘要（页数/标题/作者/生成器）；`pages` 区间选页（"1-3,5,8-"）、`max_pages` 上限（默认 30）、加密 PDF 传 `password`；文本层为空的页标注（扫描件/图片型引导）；内容经截断保护
  - `pdf_merge`：多 PDF 合并（inputs 路径字符串或 `{path, pages}` 选页抽取），copyPages 按序拼接
  - `pdf_split`：ranges（每区间一文件）/every（每 N 页）/single（逐页）三模式，输出 `{prefix}-p{起}-{止}.pdf` 至 outdir（默认会话工作目录）
  - `pdf_edit`：ops 按序原地编辑（页码指当前状态）——`delete` 删页/`rotate` 旋转（±90/±180/±270，相对现有角度累加）/`move` 页序移动（from/to）/`metadata` 元数据（title/author/subject/keywords/creator）/`watermark` 水印（中文自动嵌系统字体，fontSize/color/opacity/angle/pages 可调）；**写前产物自校验**（重新 load，失败不落盘）；内置 pdf-lib `removePage` 不失效 pageCache 的补丁（删除后失效页缓存，防后续 rotate/move 取幽灵页）
- **阅读视图预览（前端内联渲染）**：pdf 产物由前端文件卡/弹窗以 iframe 直接加载 `files/preview` 原文件（浏览器原生 PDF 渲染，无需服务端阅读视图）；docx/xlsx/xlsm/pptx 的文件卡与「原文件」弹窗不再是二进制占位——前端按 office 类型取数 `files/preview?path=…&render=office`（服务端 `wps/preview.ts` 复用读取模型渲染结构化 HTML：Word 标题/列表/表格/图片按出现顺序 data URI 内嵌（单图 ≤2MB、总量 ≤8MB 超额降级为标记）、Excel 按工作表出带样式表格（字体/底色/对齐、合并单元格 rowspan/colspan，500 行 × 64 列截断）、PPT 逐页大纲（标题/表格/图表标记/备注）），沙箱 iframe 承载（同 html 分支）；内容全量 HTML 转义，**不还原精确分页与版式**（阅读视图口径，头部 meta 行明示）；渲染失败（损坏文件/非 office 扩展名 → 422）回退二进制占位与下载引导；渲染器在 app 层**惰性引入**（解析较重不拖启动），解析单一真相源仍在 wps
- **审批与安全**：全部免审批（本地文件产物无外部副作用，与全局文件工具姿态一致）；**防盲覆盖守卫在工具体内**（目标已存在且本会话未读取过 → 拒绝；读取类工具与全局 read 共享会话已读追踪 `fileGuard`，exceljs 类修改工具天然先读后写）；写类工具（word_create/word_append/excel_write/excel_edit/ppt_create/pdf_create/pdf_merge/pdf_split/pdf_edit）显式 `safeMode: false`（安全模式不提供——文档生成必然落盘），读类工具默认注册（实现只读）；路径经 `resolvePath` 沙箱约束、写前经 `writeGuard`（self_optimize 等装载期写范围政策同规则生效）
- **工作流（提示词内置）**：明确需求（素材先浏览，不编造）→ 大纲先行（正式/大型文档 ask 确认）→ 分段生成（超长 Word 先 create 后 append，避免单次输出截断）→ 读回校验 → 交付路径与摘要；排版规范内置（Word 标题不超三级、Excel 首行表头+冻结+筛选、PPT 一页一主题 3~5 要点、图表选型指引）；批量数据需求（导入合并/导出/清洗/统计/透视）优先 excel_batch 声明式管道（不写 py/pandas），仅表达式不够用的定制计算才落 py/js 加工
- **限制**：旧版二进制格式 .doc/.xls/.ppt 不支持（提示在 Office/WPS 另存为 OOXML）；OOXML→PDF 转换（保真导出）仍经 sh 检测并调用 LibreOffice `soffice --headless --convert-to pdf`（宿主机存在时；无则 word_read 读回内容后 pdf_create 重建，版式不保真）；pdf_create 图片仅 png/jpg、自定义字体仅 TrueType 轮廓（OTF/CFF 不支持，见 pdf_create 条目）、加密 PDF 仅 pdf_read 可凭密码读（merge/split/edit 拒绝）；exceljs 不计算公式——读取时无缓存值回显公式原文
- **预加载**：`preload = false`，按需装载


#### `tts`（语音合成）

实现于 `packages/agents/src/agents/tts/`（@gebai/agents 包；`tts.ts` 定义与语音工具 + `sfx.ts` 音效与音频处理工具 + `tts.md` 系统提示词 + `tts.test.ts`/`sfx.test.ts`/`neural.test.ts` 用例）与 `packages/agents/src/core/tts/`（基建域：`speech.ts` 系统语音引擎、`neural.ts` 神经语音合成、`audio.ts` 音效合成与音频处理、`vocoder.ts` 相位声码器），**纯本机离线**（不联网、零第三方依赖、零安装——语音走系统语音栈，音效与后处理是纯 PCM 计算）。

**两档语音引擎**（同一工具的两个分支，按场合选）：

| 档 | 引擎 | 音质 | 依赖 | 适用 |
|---|---|---|---|---|
| **神经（`engine=neural`）** | sherpa-onnx + VITS 声学模型（ONNX，onnxruntime CPU 推理） | 接近云端合成；22.05kHz；单模型可含几十到上百个音色 | Python + `sherpa-onnx`（wheel 内含 onnxruntime，**无 torch**）+ 模型文件 | 宣传片配音、口播、有声内容、交付给用户的成品 |
| 系统（`engine=auto/winrt/sapi`） | WinRT OneCore / SAPI5 | 拼接式韵律，明显机械 | 无（Windows 内置） | 快速草稿；没装模型/依赖且不愿安装；需要 `pitch`/`volume` 精细控制 |

**神经引擎的实现要点**：① **不依赖 PyTorch**——VITS 导出 ONNX 后只用 onnxruntime CPU；实测 4.45 秒语音 2.8 秒合成完（含进程启动与模型加载），模型约 120MB/个；② **模型按「文件齐全」扫描**（目录内恰好一个 `.onnx` + `tokens.txt` + `lexicon.txt`），不维护硬编码清单——用户可直接放任意 sherpa-onnx 兼容的 VITS 中文模型，无需改代码；③ 模型目录 `{GEBAI_HOME}/resources/models/tts/<模型名>/`，中文模型用词典式音素化 + `.fst` 后处理（**不需要 espeak-ng**）；④ 脚本经**临时 .py 文件**交给 python（**不能用 `-c`**：Windows shell 会把换行的转义形式当续行符，python 收到满屏 `\n` 字面量直接 SyntaxError——实测踩到）；⑤ 待合成文本走 **base64** 传参（中文/引号/换行不被 shell 拆开）；⑥ 解释器按**客卿共享 venv → vendor 形态 → 系统 python** 逐候选实测（失败原因多样，逐个试比猜哪个能用可靠）；⑦ **失败说明区分真因**——只有真缺 `sherpa_onnx` 才提示安装，解释器没起来说「启动失败」、推理期出错指向模型版本并保留原文（早先把所有失败都映射成「缺依赖」，把真因埋了）；⑧ `pitch`/`volume` 在 neural 分支不生效（VITS 模型无这些参数），要变调用 `tts_effect` 后处理；`rate` 转成模型 `speed`。

- **引擎链**：Windows 两个系统语音栈——WinRT OneCore（`Windows.Media.SpeechSynthesis`，Windows 10+ 标准，质量优于 SAPI5）优先，SAPI5（`System.Speech`）回退；`engine` 参数可显式指定（auto/winrt/sapi）。非 Windows 平台当前没有等价的内置离线引擎，工具如实报错并指明本能力**不做联网合成**（不回落在线服务），系统提示词同步约束（不要去在线 API / 下载语音模型）
- **实现要点（三处实机踩坑）**：① 脚本经 PowerShell 5.1 的 `-EncodedCommand`（UTF-16LE base64）调起——PowerShell 按系统 ANSI 代码页解码 `.ps1` 文件，无 BOM 的中文脚本会解析失败，编码进命令行彻底规避且不落脚本文件；② 待合成文本与运行结果都走 UTF-8 文件（命令行与单个环境变量都有长度上限与转义负担，PowerShell 的 stderr 是 CLIXML、stdout 编码随宿主漂移——文件是唯一稳定接口）；③ SSML **必须带 `xml:lang`**（缺失时 WinRT 直接报错），故语言随所选音色在脚本内确定；文本的 `& < >` 与换行在 TS 侧完成 XML 转义（脚本只做拼接）。时长经脚本解析 WAV 头得出（不把整段音频读回内存）
- **工具集（七工具）**：`speak`（text 必填 + engine/voice/model/speaker/rate/pitch/volume/out/play；产物 WAV 落会话 `tmp/tts/voice-<时间戳>.wav`（`out` 可指定），结果附 file 块——聊天内原生播放器直接播放、可直接交付；单次文本上限 4000 字符，超限提示拆分而非截断；参数越界钳制，语速极端时提示试听确认）；`models`（列出已安装的神经模型与音色数量，并探测 `sherpa-onnx` 依赖是否就绪——只 import 不加载模型，毫秒级）；`install`（`model` 可选：pip 装 sherpa-onnx + 从镜像下载中文 VITS 模型约 120MB）；`voices`（find/limit：列出系统语音音色（名称/语言/性别）供 `speak` 的 voice 取值）；`sfx`（音效合成：`preset` 预设名或中文关键词（`list=true` 列出 27 个预设）/ 自定义 `wave`+`freq`（可 `freqTo` 滑频）+`duration`+`decay` 等；`repeat`+`repeatGapMs` 连响；`volume`/`sampleRate`/`out`）；`effect`（音频效果：`input` 必填 + `tempo`/`pitch`/`speed`/`gainDb`/`normalize`/`fadeIn`/`fadeOut`/`reverse`/`trimStart`/`trimEnd`/`echoDelayMs`(+`echoFeedback`/`echoMix`)/`reverbMix`(+`reverbSize`)/`lowpass`/`highpass`/`robot`）；`mix`（`tracks` 数组（`path` + `delayMs`/`gain`/`loop`）+ `mode` mix|sequence + `gapMs`/`durationSec`/`sampleRate`）
- **引擎为基建、两条链路共用**：合成引擎在 `packages/agents/src/core/tts/speech.ts`（基建域）——脚本、SSML 构造、结果解析、失败分类、分片（`splitText`）与 WAV 拼接（`concatWav`）同一份实现；执行通道（runCommand/文件读写）经 `TtsDeps` 注入，故子Agent 工具（会话沙箱内、产物落会话）与 REST 朗读接口（进程级子进程、产物不落盘）共用而不复制
- **助手回复朗读（Web）**：`POST /api/v1/tts` + 前端消息操作按钮组内的朗读按钮——助手回复一点即听，不必先合成再播放；合成服务不落会话产物（朗读是「听一下」，不该在会话文件里堆积音频），并补子Agent 不需要的两件事：**长文本分片拼接**（助手回复常超过单次合成上限，按句边界分片后拼 WAV，格式不一致则如实报错而非交付残缺音频）与**进程内缓存**（同文同参重复点击不再调系统引擎，按字节上限淘汰最旧）；平台无内置离线引擎时返回 503 并说明不做联网合成，前端据此不渲染死按钮
- **本机扬声器播报（`play`）**：`play=true` 时除落盘外把产物送到**运行 GEBAI 这台机器**的默认音频设备——经 `Start-Process` 派生独立的隐藏播放进程（`SoundPlayer.PlaySync`）后立即返回，播报不阻塞工具返回（长文本可播十几分钟），也不会因父进程退出而中断；**仅本地模式提供**（服务端部署下播报的是服务器机器的音频设备，会干扰同机其他用户，故拒绝并写明原因——产物照常落盘，可下载后自行播放）；播报失败不影响合成结果交付，原因进注意项
- **音色匹配与回落**：引擎按「精确名 → 名称包含（如「Kangkang」→「Microsoft Kangkang」）→ 中文优先默认音色」三级选择；第三级回落时在结果里写明实际音色（**不静默换声**），关键词命中不算回落
- **失败分类**：无引擎（指定 winrt 时不回退 SAPI vs 两者皆无）/ 无音色 / 超时（120 秒，与用户取消区分）/ 非零退出（CLIXML 噪音清洗后透出根因）/ 未产出音频——各自给出可行动的下一步指引
- **音效与音频处理（`sfx`/`effect`/`mix`，内核 `core/tts/audio.ts` + `vocoder.ts`）**：三条能力都是**纯 TS 计算**（不依赖系统语音引擎，非 Windows 平台同样可用）——① 合成：波形（正弦/方波/三角/锯齿/噪声）× 频率（可滑频）× 包络（起振/收尾/指数衰减）组合，内置 27 个预设（提示/交互/氛围/系统音四类，含视频收尾句式所需的 `riser`/`impact`/`sparkle`），也支持自定义单音与连响；噪声层用固定种子伪随机（同参数输出逐样本可复现），其 `freq` 为该层低通截止；② 效果：**变速不变调**（`tempo`，相位声码器——`vocoder.ts` 的 STFT + 相位推进 + 相位锁定，时长随之变化而音高保持；实测约实时 160 倍（16kHz 语音 217 秒用时 1.3 秒），真正约束是产物缓冲内存，故按**产物时长**限 600 秒、放慢倍率相应收紧输入上限，超限提示先裁剪或分段）、变调（OLA 时间伸缩 + 重采样，时长不变）、变速（`speed` 重采样，音高随时长一起变）、回声、混响（四路梳状简化模型）、低通/高通（RBJ biquad）、环形调制、淡入淡出、反转、增益/归一化/裁剪，按**固定顺序**施加（裁剪→反转→变调→变速不变调→变速→滤波→调制→回声→混响→淡入淡出→增益→归一化）使同参数结果可复现；③ 组合：多轨混音（轨级延迟/增益/循环铺底——总长以非循环轨为基准，`durationSec` 只用于把铺底垫得更长、不截断内容）与顺序拼接（段间静音），采样率自动统一到最高轨；输入只接受未压缩 WAV（PCM 8/16/24/32 与 float 32/64，其余格式如实说明需先转 WAV），输出统一 16bit 单声道 WAV；叠加超峰自动等比限制而非削波（不静默交付失直音频），输入时长上限 30 分钟、混音轨数上限 16
- **相位声码器为独立数值模块**：`packages/agents/src/core/tts/vocoder.ts` 只用内置类型（不依赖项目内其它模块）——radix-2 FFT（旋转因子按帧长缓存复用）、帧长按采样率取约 46 毫秒的 2 的幂（钳制 512~4096）、分析 hop = 帧长/4、合成 hop 按速度倍率缩放；**相位锁定**（Laroche & Dolson：只有谱峰累积相位、其余频点跟随所在区域的峰的相对相位）抑制纯相位声码器典型的「相位散乱」噪声；瞬态（爆破音、鼓点）仍有涂抹——这是相位声码器的固有限制，非实现缺陷
- **审批**：无（与 `wps` 同级——产物落会话内，属于生成本机文件）；安全模式不提供（`safeMode: false`：写文件并发起脚本子进程）
- **环境变量**：`TTS_VOICE`（默认系统音色，可写完整名或关键词）、`TTS_ENGINE`（默认引擎 neural/auto/winrt/sapi）、`TTS_MODEL`（默认神经模型 id，缺省自动选最优）——经 def 的 `envVars` 汇总进前端环境变量面板白名单
- **预加载**：`preload = false`，按需装载

#### `nsight`（NVIDIA Nsight 报告分析与 GPU 性能问题定位）

实现于 `packages/agents/src/agents/nsight/`（`nsight.ts` 定义入口 + `nsight.md` 系统提示词 + 分析/解析/定位分层模块 + 五个测试文件：诊断规则、多卡与开销口径、原生等价性、报告与环境解析、合成事件库聚合），面向「报告 → 问题 → 代码」的完整链路：解析 Nsight Systems（`.nsys-rep`/`.qdstrm`，时间线：GPU 活动、内核、显存传输、CUDA API/同步、NVTX）与 Nsight Compute（`.ncu-rep`，单内核硬件计数器）报告，产出量化证据 + 根因 + 修复方向的问题清单，并把报告符号映射到工程源码 `文件:行`；无现成报告时可对目标程序做 nsys/ncu 采集。

- **工具集（12 个，含原生聚合后端）**：`doctor`（工具链/GPU/计数器权限/缓存自检）、`reports`（报告索引 list·info·import）、`overview`（利用率·紧凑时间线·**每卡分解**·**采集开销**·**CUDA Graph**·热点·传输/API/同步/流）、`kernels`（排行与单内核下钻）、`timeline`（空闲缝含前后邻接活动·启动间隔·流并行度）、`query`（事件库只读 SQL：schema/run——标准报告未覆盖的维度精确取数）、`findings`（诊断清单，可选自动定位、**可选导出 Markdown**）、`compare`（**两份报告改前改后对比**：度量与问题清单差异）、`kernel_detail`（ncu 单内核深查；传 `.nsys-rep` 时降级为调用模式分析并说明差异）、`locate`（符号→源码）、`capture`（采集）、`aggregate`（客卿 Rust 边车贡献的原生聚合后端，分析类工具自动选用；一般无需直接调用）。`overview`/`timeline`/`findings` 另支持 **`time_from_ms`/`time_to_ms` 时间窗**（相对报告首个活动，与活动区间相交即命中，下推到 SQL）。**审批：仅 `capture`**（执行被分析程序）需审批，其余只读免审批；报告路径类工具带 `project` 参数（预置项目/路径/保留名 `tmp`），`locate` 的搜索范围即该项目根。
- **报告解析与缓存**：nsys 经 `nsys export --type sqlite` 落事件库（Bun 内置 `bun:sqlite` 只读打开）；ncu 经 `ncu --import --page <页> --csv` 落指标页（details/raw/source，流式解析）。缓存根 `{GEBAI_HOME}/cache/nsight/{报告名}-{大小}-{mtime}/`（内容指纹寻址——报告重新采集自动换目录，不误用旧数据）；**导出的事件库默认无索引**，故导入后对超过 20 万行的事件表按查询模式建**覆盖索引**（热扫描用到的列全部纳入：普通索引每行仍需回表随机读）——实测 1000 万行事件库下同样的逐行扫描由 18.9s 降至 11.2s。**扫描的活动表**：内核、显存传输、**显存 memset**（三者按首时间戳归并，缺表即该维度为空而不中断分析）、**`deviceId` 列**（多卡分流；旧版导出缺该列时按单卡处理）。
- **超大报告（GB 级、千万级事件）的实时分析**，三条纪律（内存与报告规模解耦）：① **不物化全量事件**——游标流式消费，内核与显存传输两路各自走索引归并（避免 `UNION` 的全局排序与临时落盘）；② **结果有界**——排行用 Top-K、分位数用**固定种子的受控采样**（超出容量转蓄水池抽样并标注估计；固定种子使同一报告重复分析得到同一组分位数，与原生边车的固定种子同语义）、时间线用自适应分辨率分桶（跨度增长只倍粗分辨率，桶数恒定，故无需第二遍扫描）、空闲缝上限截断；③ **一次扫描多次复用**——聚合事实按「报告指纹 + 阈值」进程级缓存，概览/热点/诊断/定位共享同一份扫描结果（首次调用付扫描成本、后续命中秒回，工具如实回报耗时、聚合来源与缓存命中情况）；API/同步/NVTX 等维度用 `GROUP BY … ORDER BY … LIMIT` 下推 SQLite，只把有界结果带回 JS。
- **原生聚合后端（客卿 Rust 边车，可选加速路径）**：`keqing/rust/nsight/`（cargo bin crate，依赖语言目录共享框架 + rusqlite（bundled 特性内嵌 SQLite —— 不依赖系统 sqlite3））以同名子Agent 跨语言合并贡献一个 `nsight_aggregate` 工具，把**同一套单趟聚合**在原生执行（游标流式、固定容量聚合器、分组下标记忆化、逐行只写 Copy 类型、JSON 只在输出阶段构造一次）。TS 侧 `resolveTimelineFacts` **原生优先、自动回退**：边车不可用（未构建/服务端部署下客卿整体禁用）或调用出错/返回值不合约即回退 JS 流式实现，回退原因随工具输出如实回报（不静默降级）；`NSIGHT_NATIVE=off` 可显式关闭原生通道。两条实现的**输出同构由测试锁定**（同一合成事件库逐字段比对规模/耗时/并集/空闲缝/并发/传输/分组）。
- **实测（千万级事件，本机 RTX 4080 SUPER 宿主）**：事件库 1000 万内核 + 20 万传输（含覆盖索引，1038 MB）下，JS 流式聚合 16.2s、原生边车 11.5s（**1.4×**），峰值内存 RSS 143 MB → 83 MB（1.7×）；结果核心指标逐字段一致。**与语言无关的真实瓶颈**：扫描循环内采样计时显示耗时几乎全在「行读取」（≈ 1 μs/行 × 15 列，两种实现都在付 SQLite 行解码这笔成本），聚合逻辑本身只占零头；且实测 SQL 侧聚合并不更快（`GROUP BY` 多聚合 9.3s、窗口函数求并集 12.4s/最大并发 19.4s，均于 1000 万行），故**进一步提速的杠杆是减少需解码的列/行（窄列扫描：15 列 8.7s → 2 列 2.0s，4.3×）而非再换语言**。
- **导入预算与后台衔接**：单次工具调用受引擎时间上限约束，导出超出预算时返回「导入进行中」+ 可直接后台执行的完整命令（nsys 不支持续传，不重复启动导出）；下次调用以「SQLite 可打开且含事件表」判定产物是否可用——用户/后台在同路径自行导出的产物同样被复用。
- **诊断规则**：时间线侧——GPU 空闲占比、同步阻塞（含阻塞型 API 归因）、热点内核集中、小内核启动开销、网格规模不足、传输包偏小、单流无并发、寄存器/共享内存占用压力；阈值集中定义在 `findings.ts`（`FINDING_THRESHOLDS`）便于复核。ncu 侧——NVIDIA 官方规则（含预估收益，按收益分级）、SOL 瓶颈单元（带宽/计算/缓存谁饱和）、占用率上限与**限制因素**（寄存器/共享内存/块数）、停顿主因分解（源采样汇总）、访存效率（越界扇区=非合并访问、共享内存 bank 冲突）、分支发散。
- **采集开销与 CUDA Graph 维度**：读 `PROFILER_OVERHEAD`（采集插桩给被测程序引入的开销）与 `CUPTI_ACTIVITY_KIND_GRAPH`/`GRAPH_NODE`。开销**按活动窗口求交**：只有落在窗口内的部分才构成对被测区间的扰动，启动阶段（进程初始化，实测最长可达窗口前 3.1s）与退出 flush 单列并注明不影响窗口内结论——实测某报告 39 个开销点全部在窗口之外，若按总数除以窗口会得出误导性的 82.3%（窗口内实为 0.4%）。Graph 维度未采集时**不判定为「没有用图」**，而是如实列入未分析维度并给出开启参数（`--cuda-graph-trace=node`）；图占比高时提示「图内部依赖不在逐内核视图里」。
- **多卡正确性**：GPU 活动按 `deviceId` 分流——`TimelineFacts.devices[]` 给出每张卡的忙碌/利用率/卡内空闲缝/最大并发/占用序列；顶层字段保持「任一卡在忙」的合并口径（单卡行为不变）。**合并口径只回答「机器有活干吗」，回答「哪张卡被困住」必须看每卡分解**——实测构造场景：卡 0 忙 900µs、卡 1 只忙 100µs 时合并利用率 100%，而卡 1 实际闲置 89%。诊断据此新增 `device-imbalance` 规则（卡间利用率差超 `FINDING_THRESHOLDS.deviceUtilSpread` 即单独指出并给出每卡证据）。JS 与原生边车两侧同实现，等价性测试逐字段比对 `devices`。
- **报告间对比**（`compare` 工具 + `core/perf/compare.ts`）：两份报告（改前改后/两次采集）的度量按同名对齐、问题清单按 `id` 对齐，输出改善/退化/新增/消失/代价变化。三条防误导规则：**单位不同不比数值**（`3.9 MB` 与 `679.0 KB` 按裸数字比大小会得出相反结论）、**相对变化低于显著性阈值记「持平（阈值内）」**（`DEFAULT_SIGNIFICANCE_PCT`=1%，避免噪声级差异淹没真实变化）、**标记只说好坏、百分比只说变化量**（早期用 ↑/↓ 表示数值升降与好坏混排，读起来自相矛盾如「↑ 退化（-75.9%）」）。净变化按两侧各自全量求和——只算共有问题会看不见「修好的问题省了多少」。
- **结果导出**（`core/perf/export.ts`）：`findings` 支持 `save`/`save_dir` 落 Markdown（含证据、根因、建议与源码定位），路径按 `project` 根解析、文件名带时间戳（重跑不覆盖上一版）；导出失败不影响分析结果（以提示形式附在输出末尾）。
- **证据纪律**（系统提示词硬约束）：时间线证据（何时忙/闲、谁等谁、调用模式）与硬件计数器证据（SOL/占用率/stall）严格区分，不用其一断言另一维度；抽样与截断如实标注；报告未采集的维度明确列出并给出补采参数（NVTX、源码关联 `--import-source` 等）。
- **符号→源码定位**：报告符号以 **demangled 名**入清单（mangled 名不可作源码搜索词，命中该形态时给出改用 demangled 的提示）；归一为函数基名（去模板参数/参数列表/命名空间），搜索内核定义（`__global__`/`__device__`）、启动点（`<<<>>>`）、NVTX 打点与名称引用，按「定义 → 启动点 → 打点 → 一般引用」排序输出 `文件:行` 与上下文。**词边界匹配 + 通用标识符停用表**（避免 `at` 命中 `path` 一类噪声）；NVTX 名支持 `StringIds.textId` 与 `NVTX_EVENTS.text` 两种存储形态（torch 等运行时用后者）——框架工作负载据此落到用户代码行。扫描设文件数/单文件大小/总字节上限，超限与超大文件跳过均标记为「扫描不完整」；符号来自预编译库（cuBLAS/cuDNN/PyTorch 内置算子）时明确提示优化点在上层调用方式而非内核源码。
- **采集与权限**：`capture` 构造 nsys（`--trace` 项前置校验，Windows 默认关闭 cuda-event-trace 以减小开销）/ncu（`--set`/`--launch-count`/内核筛选/源码关联）命令并执行，失败按错误模式分类给修复动作（计数器权限、注入失败、trace 项非法、路径不存在）；ncu 采集前先探性能计数器权限——**受限时立即返回开启方法**（管理员权限或 NVIDIA 控制面板开发者设置），不消耗采集时间。nsys 采集与全部报告分析**不需要**该权限。
- **与 `torch` 子Agent 的分工（两个面互不依赖、可同时装载）**：本子Agent 负责 Nsight 报告（Systems 时间线 + Compute 单内核）；PyTorch Profiler trace（`.pt.trace.json(.gz)`）由 `torch` 子Agent 负责（算子级/Python 级/显存级解释）——两者共用 `src/core/perf/` 基建但**零互相引用**（守护测试 `core/perf/coexist.test.ts` 固定三条：工具命名空间无交集、任一面不得引用另一面、core 不得反向依赖分析面）。Windows 上 PyTorch 的 CUPTI 采集不可用，故 PyTorch 场景的 GPU 内核级时间线靠本子Agent 的 nsys 采集补齐。

#### `torch`（PyTorch Profiler trace 分析与代码问题定位）

实现于 `packages/agents/src/agents/torch/`（`torch.ts` 定义入口 + `torch.md` 系统提示词 + 解析/聚合/诊断/工具分层 + `torch.test.ts`）：
解析 `torch.profiler.profile(...).export_chrome_trace()` 导出的 Chrome Trace（Kineto），面向「哪个算子、哪行 Python、显存怎么用」，
产出量化问题清单并把热点定位到工程源码 `文件:行`。与 `nsight` 相互独立、可同时装载。

- **PyTorch Profiler trace（Chrome Trace / Kineto）**：本子Agent 的第二个报告面（工具 `torch_overview`/`torch_ops`/`torch_memory`/`torch_findings`），面向「哪个算子/哪行 Python/显存怎么用」。格式依**真实 trace 核实**：顶层 `{"traceEvents":[…], "profile_memory":1, "with_stack":1, …}`，`ts`/`dur` 单位为**微秒**且 `ts` 为绝对纪元时间；事件类别 `cpu_op`/`python_function`（名称内嵌 `文件(行): 函数`）/`user_annotation`（`ProfilerStep#N`）/`kernel`/`cuda_runtime`/`gpu_memcpy`/`gpu_memset`/`cpu_instant_event`（含 `[memory]` 分配器事件，`Bytes` 负数为释放、`Total Allocated`/`Total Reserved` 为累计快照）。
- **超大 trace 的流式解析**：`jsonstream.ts` **不整文件 `JSON.parse`**（内存会与文件同阶），而是增量扫描 `traceEvents` 数组——状态机定位后逐个元素切片、交由调用方单独解析与聚合，内存与文件规模解耦；原生支持 gzip（`*.pt.trace.json.gz`，TensorBoard trace handler 的默认产物），解码与解析流水化、不落中间文件。**单位边界**：µs 在入参处换算为 ns 再交给共享时间线组件（并集/并发/自适应分桶按 ns 语义设计，含 50 µs 空闲缝阈值），出参再换算回 µs——否则空隙阈值被当成 0.05 µs、所有空闲缝漏检。
- **单趟聚合产出**：类别统计、算子/内核/CUDA API/用户标注排行（**自身耗时**与总耗时分开——同类嵌套做减法、跨类不做，避免无定义的归因）、内核几何与「内核 → 发起算子」归属（`correlation`）、传输按方向聚合、显存（峰值已分配/已保留、碎片率、最大分配、按设备分布、地址追踪超限降级标记）、时间线（CPU/GPU 双路并集与占用序列、GPU 空闲缝及**缝内 CPU 是否在忙**、CPU/GPU 重叠）、步级统计与 python 位置热点；上限集中在 `TORCH_LIMITS`。
- **诊断规则**（`torch-findings.ts`，阈值集中在 `TORCH_THRESHOLDS`、测试锁定）：同步阻塞（`.item()`/主机往返）、CPU 受限（GPU 利用率低且空闲缝内 CPU 在忙）、Python 开销、算子碎片化、autograd 引擎开销、小内核启动受限、单内核主导、占用率压力、显存碎片与分配 churn、步时抖动、float64 混入、布局/拷贝转换、用户代码热点。**缺维度的规则不做判定，只如实说明缺什么、怎么补**（例：无 GPU 事件时不给内核级结论，而是指向 `nsight_capture kind=nsys`）——这是证据纪律在 torch 侧的延续。
- **平台事实（实测）**：**Windows 上 PyTorch 的 CUPTI GPU 采集不可用**——显式启用 `ProfilerActivity.CUDA` 仍无任何 `kernel`/`gpu_memcpy` 事件。因此 torch trace 侧专注 CPU/算子/内存维度，GPU 内核级时间线由 nsys 提供（Windows 可用）；两者用同一套「量化证据 → 源码位置」口径互补。
- **工具集（7 个）**：`torch_reports`（trace 索引：list/info，含是否已有事实缓存）、`torch_overview`（总览，含前向/反向拆分）、`torch_ops`（算子/内核下钻，含筛选与内核归属）、`torch_memory`（显存分析）、`torch_findings`（诊断，可选 `locate=true` 落到源码 `文件:行`、可选 `save=true` 导出 Markdown）、`torch_compare`（两份 trace 改前改后对比）、`torch_capture`（生成/执行采集脚本）。**审批**：仅 `torch_capture mode=run`（执行被分析脚本）需审批，其余只读免审批；报告路径类参数带 `project` 包装。
- **时间预算与落盘事实缓存**（对齐 nsight 的 pending 分支）：扫描带时间预算（`DEFAULT_SCAN_BUDGET_MS` 5 分钟，依据引擎单次工具调用上限留渲染/定位余量），超预算**中止扫描并返回未完成事实**（不抛超时错误），工具层给出「本次未完成 + 已扫多少 + 可复制的后台命令 + 跑完再调即命中落盘缓存」；事实缓存为**内存→落盘→扫描**三级（`{GEBAI_HOME}/cache/torch`，`TORCH_CACHE_DIR` 可覆盖，键=路径+大小+mtime），落盘只在完整扫描且耗时 ≥2s 时发生（未完成的结果不缓存，避免把半份事实当结论）。实测：285.7 MB / 200 万事件触发 pending → 后台 18s 落盘 → 再调用 **5 ms** 命中。
- **前向/反向拆分（fwdbwd）**：**格式依真实 trace 核实**——`cat:"fwdbwd"` 的 `ph:"s"` 挂在**前向算子**上（`ts` 等于该算子起点）、同 `id` 的 `ph:"f"` 挂在**其反向算子**上（并非「前向段/反向段」的区间标记，`name` 就是 `fwdbwd`）。故口径是「前向算子耗时合计 vs 其反向算子耗时合计」与逐步反向占比，而不是整段时间的划分；无 fwdbwd 事件时如实说明未采集。
- **内核归属**：`correlation` 链（内核 → 最内层 `cpu_op` → python 帧）为主，`ac2g` 流事件（CUDA API ↔ 内核）为第二通道补全无 correlation 的内核；无流事件时如实说明归属仅依赖 correlation。Windows 无内核事件时退化为「CUDA API → 发起算子/发起位置」视图。
- **文件变更防护**：扫描前后校验存在性与大小/mtime，不一致与 `ENOENT` 类错误都重写为可操作提示（「分析过程中被修改/删除，请重试」），不把原始系统错误抛给模型。
- **实测性能（本机 RTX 4080 SUPER 宿主）**：**按事件数是可跨格式比较的口径**（事件平均大小随字段密度变化，按 MB 口径不能直接对比）。JS 路径在**真实 torch 输出格式**（平均 ~350 字节/事件，事件带缩进与嵌套 args）下约 **30 MB/s**（≈12 万事件/s）；原生后端（见下）**62 MB/s**（扫描阶段独占 **300 MB/s**）。历史记录（1158 MB / 605 万事件 38.5s 等）出自**简化字段的合成 trace**（89 字节/事件），按事件口径约 15.7 万/秒——不代表真实格式耗时，真实格式的字段密度高约 4 倍，同一实现会慢一个量级。JS 路径的优化手段全部由对照实验选定：`charCodeAt` 状态机（384 MB/s）而非 `buf[i]`（26 MB/s）或「5 定界符 indexOf 跳转」（36 MB/s）、元素按批产出、两级分组表（免每事件字符串拼接）、字段按偏移量提取（不构造中间对象）。剩余成本在「逐元素文本物化」与流式缓冲的逐块拼接（实测：同一段循环跑在扁平整串上 389 ms vs 跑在流式缓冲上 1327 ms）。
- **原生后端（客卿 Rust 边车）**：`keqing/rust/torch/` 以同一套聚合在原生执行——**整文件读入内存**（GB 级内存可接受，这是明确取舍）、**字节级扫描**（不经过 JS 层，也没有「逐块拼接缓冲」的开销）。实测 259.7 MB / 105 万事件：原生 scanMs 1.85s vs JS 8.70s（**4.7×**；真实 0.4 MB trace 上 **13.0×**、gz 形态 **5.6×**），**逐字段精确一致**（9212 项零差异），峰值 RSS 268 MB 对 JS 侧约 1.5 GB（原生连内存都更省：约 1× 文件大小 vs 约 5.8×）。分阶段实测（同一文件，按事件记）：仅扫描 710 ms（**366 MB/s**）、叠加字段提取 1182 ms（220 MB/s）、叠加聚合 1847 ms（**140.6 MB/s**，优化前 62 MB/s）。
- **原生的热点与优化原则**：扫描阶段已到 366 MB/s（与 Go 重写同量级），剩余耗时在**逐事件的字符串分配**与 **HashMap 重查**——优化手段是把字段提取改为**借用切片**（`Cow` / `&[u8]`，仅在新分组插入时才 `to_owned`）、查表用借用键、复用键拼接缓冲。实现内保留基准探针 `TORCH_BENCH_PROBE=noop|parse`（只扫描 / 叠加字段提取）与 `TORCH_BENCH_PHASES=1`（打印并行分块的阶段耗时：块起点摘要/块内采集/归并/输出组装）——性能回归靠它定位到阶段，不靠盲猜。
- **原生回退语义**：`resolveTorchFacts` 原生优先、失败**自动回退 JS**（边车未构建/未装载——服务端部署下客卿整体禁用/调用报错），回退原因如实回报；原生返回值经 `coerceNativeFacts` **严格校验**（字段缺失/类型不符即抛错回退，绝不让半份事实进入诊断——诊断据此给出「没问题」比慢更危险）；`TORCH_NATIVE=off` 可显式固定走 JS。工具输出在原生路径标注「｜原生聚合」、回退且曾尝试原生时标注「｜JS 聚合（原生不可用）」；JS 常态输出保持原样。等价性由 `native.test.ts` 锁定（真实字段结构合成 trace 上原生 vs JS 逐字段比对，含步级/算子/内核/fwdbwd/flows/归属/显存/时间线；未构建边车时原生侧自动跳过）。**时间预算只作用于 JS 回退路径**——原生不设流式中止点，可用时不会出现「未完成」。
- **原生并行（rayon 分块 + 有序归并）**：分块**不能**是「切段各自聚合再相加」——聚合状态里有一半带**顺序依赖**（浅栈自身耗时、采样器蓄水池的逐次替换、时间线并集的 running-max 语义、显存活跃集/峰值/TopK、flow 配对与全局 `last_activity`、correlation 启发表、分组首现序与 `MAX_GROUPS`/`SAMPLER_GROUPS` 分配序）。做法：① **精确块起点**——先并行算「块间结构摘要」（字符串态 + 括号深度增量），再按块序顺序合成（K 步、与字节数无关）得到每块起点的**精确**扫描器状态；猜起点是不行的：猜错会把嵌套对象当顶层元素、静默污染结果。② 块内沿用同一套状态机（元素按「起点落在块内」归属，跨块元素由本块扫完、下一块跳过起点前已开始的片段），只算**可交换**的部分（计数/求和/min/max、分组统计、类别统计、python 位置、传输聚合），顺序依赖部分只记**紧凑日志**（栈事件流 24 B/条、区间 16 B/条、采样时长 8 B/条、显存/flow/correlation 记录）。③ 归并阶段按全局序回放日志，并与单趟路径**共用同一批 helper**（`close_frame_at`/`pair_flow`/`pair_fwd_bwd`/`upsert_attr`/`Union::add`/`Bins::add`/`Sampler::add`/`apply_mem_instant`）——语义只有一份实现，两条路径不会漂移；浅栈按**（进程,线程）并行回放**（自身耗时 + cuda_runtime 发起帧解析，与顺序路径同一套懒关闭/包含/清栈规则）。两处配套优化：**结构摘要用 SSE2 掩码跳纯文本段**（16 字节一组，`"`/`\`/`[`/`{`/`]`/`}` 的掩码等价类——只允许多报不漏报，非 x86_64 回退 SWAR）；**非 gz 大文件分片并行读**（每片独立句柄 + seek，I/O 并行度与核数解耦）。开关 `TORCH_NATIVE_THREADS`（未设 = 自动：< 4 MB 走单趟，否则 = 可用核数；`=1` 强制单趟路径，即改造前行为）。
- **原生并行的实测（本机容器 cgroup 限 4 核 CPU，宿主 64 逻辑核；多轮取 min）**：224 MB / 94 万事件 **743ms → 408ms（1.82×）**；672 MB / 279 万事件 **2175ms → 1201ms（1.81×）**（2 线程 1.36×、4 线程 1.78×、8 线程 1.82×——**4 线程即饱和**，因为 CPU 配额就是 4）。CPU 时间 2.09s → 3.25s：多出来的是「精确块起点的那一遍结构摘要」（≈ 一次额外全文件扫描，用 SSE2 掩码跳纯文本段）与块内日志/归并回放。**加速比受 Amdahl 限制**，以 672 MB 为例（scanMs 分解）：读文件（已分片并行）＋块起点摘要（并行）320ms、块内扫描与聚合（并行）420ms、归并回放 400ms（全序串行：并集/分桶 120ms、correlation 启发表 94ms、flow 44ms、显存 22ms、采样器 6ms、可交换统计与分组表合并 4ms）、输出组装 100ms为串行段——真正可并行的扫描+解析在 4 核上只摊到 420ms。峰值 RSS 826 MB → 1096 MB（≈ 1.6× 文件大小；日志约 24–48 B/事件）。测量噪声：共享宿主下同一配置的 min/median 可差 20%+（实测区间回放 121ms 与 278ms 都出现过），故上述数字均为**多轮取 min**。
- **原生并行的等价性防线**：`native-parallel.test.ts` 用**同一二进制改块数**（1 vs 2/8/64 块）逐字段比对（数值按相对 1e-9、其余精确）——真实结构夹具、对抗夹具（跨块嵌套帧、乱序清栈、flow/fwdbwd、显存、多线程/进程）、gz 形态都覆盖；块内小计 vs 逐事件累加的求和顺序差异实测最大相对偏差 ~5e-13（容差内）。未回放的状态、漏携带的跨块帧/活动都会在这里变成字段差异，而不是靠「看起来对」。
- **原生并行的已知顺序剩余（后续可继续压）**：归并回放里仍有全序串行段——并集/分桶回放（约 120ms/672MB，Union 的 running-max 与 gap 记录语义就取决于事件序，Bins 虽可证明与到达序无关但共用同一遍历）、correlation 启发表回放（约 94ms，launch→kernel 配对跨块，且有 `MAX_FLOWS` 上限这个全局序条件）、flow 回放（约 44ms）、输出组装（约 100ms，`steps` 等逐项构 JSON 的分配成本）。另外块起点的结构摘要预扫是额外一遍全文件读（无它就必须串行扫描才能得到块起点状态，而猜起点会把嵌套对象当顶层元素）。当前选择是「先保证逐字段一致，再谈吞吐」；继续压需要把 Union 改成并行前缀/分段摘要（`1/(0.4+0.6/4)` 量级的上限已接近，收益递减）。
- **跨平台**：nsys/ncu 解析顺序为 环境变量 → 安装目录扫描（Windows `Program Files\NVIDIA Corporation\Nsight *`（2025.x 的 `target-windows-x64`/旧版 `bin`）、Linux `/opt/nvidia`·`/usr/local/cuda`·`/usr`、macOS `/opt/nvidia`·应用包）→ `PATH`；命令构造按**宿主 shell 语义**（Windows 经 PowerShell 时带调用运算符 `&` 且路径含空格必须引号、`GEBAI_SH_SHELL=cmd` 回落双引号、POSIX 单引号）——nsys/ncu 的安装路径恒含空格，裸拼必失败。
- **环境变量**：`NSIGHT_SYSTEMS_BIN`/`NSIGHT_COMPUTE_BIN`（显式指定可执行文件）、`NSIGHT_CACHE_DIR`（缓存根）、`NSIGHT_PROJECT`（默认工程根，用于报告路径与源码定位基准）、`NSIGHT_NATIVE`（`off` 时显式关闭原生聚合通道、固定走 JS 流式实现）——经 def 的 `envVars` 汇总进前端环境变量面板。
- **预加载**：`preload = false`，按需装载（无 GPU 的机器上报告分析同样可用，只是采集不可用）。

- **解耦与共装载**：共用基建置于 `src/core/perf/`（流式聚合原语 `agg.ts`、格式化与工具构造 `format.ts`、符号定位 `locate.ts`、计时 `timing.ts`、输入指纹 `input.ts`、测试桩 `test-ctx.ts`），
  任何一面都不引用另一面的模块；宿主注册表按 `{agent}_{tool}` 命名，两面工具命名空间天然无交集（`nsight_*` / `torch_*`）。
  守护测试 `core/perf/coexist.test.ts` 把「可同时装载」与「零互相引用」固定为断言——工具面演进若破坏约束会直接测试失败。
- **环境变量**：`TORCH_TRACE_PROJECT`（默认工程根，用于 trace 路径与源码定位基准）。
- **预加载**：`preload = false`，按需装载；与 `nsight` 同时装载时各自工具面完整可用。

#### `triage`（两级研判：小模型批量粗筛 + 大模型引擎精审兜底）

实现于 `packages/agents/src/agents/triage/`（`triage.ts` 定义入口 + `triage.md` 系统提示词），编排与**参数契约**在 `packages/agents/src/core/triage/`（`params.ts` 参数表 + `pipeline.ts` 管线 + `scheduler.ts` 全局公平调度 + `preset.ts` 任务配置预设 + `types.ts` 契约）——**与 REST 接口（`POST /api/v1/triage/analyze`）共用同一份实现与同一份参数面**（同 `core/tts` 的「共用逻辑 + 执行通道注入」范式）。

两条通道**只允许两处差异**：① 兜底执行器注入方式（会话内派隔离子会话 / REST 走 `engine.run`）；② `mode`（同步 / 后台）**与 `sync_timeout_ms`**（同步整体超时）是 REST 的外部调用能力，工具调用天然同步。其余——参数名、别名、缺省值、校验报错、目标解析、schema 信封、分流规则、落盘布局、结论数组形状、并发与调度——全部同源。

场景无关的「先快速分流、再重点深挖」模式：整体成本 = 廉价的 O(N) + 昂贵的 O(k)，k ≪ N。

```
全量信息数组（N 条）→ 小模型批量粗筛（结构化输出 + 置信度）
                        │  按阈值/证据分流
                        ▼
                    （escalate 开）大模型精审：装领域工具主动取证，输出证据链
                        ▼
                    结论数组（逐条含置信度与 schema 定义的结论，附小模型初判供校准）
```

- **工具**：`triage_run`（跑一轮）、`triage_view`（读产物目录看汇总/逐条/未定案：`status`|`results`|`failed`）
- **参数契约（`core/triage/params.ts`，两条通道共用）**：规范形态是**扁平 snake_case** 的 `TRIAGE_PARAM_SPECS` 字典——工具 schema 由它生成、归一与校验也读它，「参数说明漂移」在结构上不可能发生。核心五项：`items`/`items_file`（全量信息数组）、`target`（推理目标）、`schema`（结构化输出 schema）、`threshold`（置信度阈值）、`escalate`（低置信度是否上升兜底）；执行模式 `mode`（`sync` 缺省 / `async`）；细项 `l1_*`（模型/提示词/并发/token/温度/提醒/超时/思维链）与 `l2_*`（agents/model/api_base/api_key/system/max_items/batch_size/timeout_ms），以及 `job_id`/`job_dir`/`result_limit`。**兼容输入**：嵌套写法（`l1.target`/`l2.enabled`）与旧键名（`l1_target`/`l1.base_url`/`l2_enabled`/`enable_thinking`）都接受，规范名优先（一致性由 `params.test.ts` 把「工具 schema 的 properties == 规范名全集」固定为断言）
- **异步执行（`mode: "async"`，两条通道同义）**：走**通用后台任务注册表**（id 前缀 `j`）——工具通道绑当前会话（`ctx.bgJobs`，可在 `bg_task` status/wait/stop/list 里管理），REST 通道绑 `api:<user>`（用既有轮询端点），**同一份注册表与同一份执行**，只是不等结果。进度经管线 `onProgress` 上报（阶段/已完成/总数/采纳·待审·失败），`stop` 经 `signal` 协作中止——**已落盘的部分结论保留**；产物目录始终是**跨重启的权威口径**（后台任务记录随进程消失，`triage_view` 仍可读）
- **推理目标解析同源**：两条通道都走 `llm_infer` 的 `resolveTarget`——`local` 按服务状态文件定位端口（不硬编码 8080）、命名目标查 `GEBAI_LLM_INFER_TARGETS`、URL 直连；未传 `target` 时缺省目标由 `GEBAI_LLM_INFER_DEFAULT_TARGET` 指定（未配置即 `local`，指向不存在的命名目标显式报错不静默回退）；解析结果随响应回传（`target.kind`/`base_url`，便于追溯本批结果出自哪个端点）
- **`escalate` 语义**：`true`（缺省）= 低置信度交大模型精审；`false` = **只跑小模型，低置信度结论按原样输出**（`ok=true`、`layer=L1`、置信度如实偏低，不标失败/待精审）。「上升但本通道无可用执行器」**显式报错**，不静默降级成小模型直出（工具侧提示改用 `escalate=false` 或走 REST；REST 侧永远有引擎执行器）
- **结论数组**：`results[]` 逐条 `{id, layer, ok, result(schema 定义), confidence, reason, evidence_index, evidence_chain?, revised?, action?, l1?, model?, error?}`；`result_limit` 缺省 **0 = 全量**（两条通道同一口径，不做静默截断；工具的文字呈现另受 `TRIAGE_TEXT_RENDER_MAX` 约束，`data.results` 始终是完整数组）
- **小模型输出通道（工具式结构化输出）**：schema 注册为 function tool（缺省 `submit_result`），模型**必须调用该工具**产出结果——不依赖服务端对 `response_format`/GBNF 的支持（异构端点与老引擎通用），且「调用与否」本身是可审计证据；未调用时按 `l1_reminders`（缺省 3）重新提醒，参数不合法则回灌错误重试，**提醒耗尽仍未调用 → 该条失败并带 `tool_call_missing`、强制上升兜底**（采集失败的条目往往最难，不能丢）。实现在 `llm_infer/api.ts` 的 `chatWithOutputTool`（`generate`/`batch` 的 `output_tool` 参数同源）
- **schema 信封**：调用方 schema 描述业务结果；顶层未含 `confidence` 时自动外包 `{result, confidence, reason, evidence_index}`，已含则原样使用；`label_enum` 注入业务 schema 的 `label.enum`
- **分流规则**（`classifyL1`）：白名单标签命中 → 采纳；未取到结论（未调用工具/解析失败/请求失败）→ 上升；置信度 0 或 < 阈值 → 上升；无有效证据 → 上升。**证据质量下限**（`min_evidence_chars`，缺省 6 字符，0 = 关闭）：至少一条证据达该长度才算有效——挡住弱模型把「任务失败」这类无信息量文本当证据、以高置信度击穿「证据非空」防线（实测 1.5B 会这么做）
- **L2 精审**：注入 L1 初判/判据/证据索引 + 原始信息，要求主动调用领域工具取证、允许明确输出「证据不足」并给待查清单、可推翻或修正 L1；输出 = 同业务结果 + `evidence_chain`（工具名 + 关键发现原文）/`revised`/`action`。**精审铁律随提示词前置送达**（子会话与引擎会话各自带自己的系统提示，不前置会丢失「取证/允许说证据不足」约定）。**id 回填抗改写**：条目块只给 `id=xxx`（不再另加序号，否则模型会把序号当 id 回填），且解析侧有**位置对齐兜底**（无任何 id 命中 + 条数一致时按序回填；条数也不一致则如实报错而不猜）
- **L1 全局公平调度（`core/triage/scheduler.ts` 的 FairGate，根治「大任务饿死小任务」）**：并发缺省按端点分档（远程 4 / 本机 1），但**真实在途上限是全局的**——同一推理端点上挂着的多个研判任务共享一个闸门，按任务**轮转**（round-robin）派发。三个关键取舍：① **上限取各任务并发度的最大值而非相加**（两个 c8 任务共享 8 个额度、各占一半，而不是把网关打到 16）——实测网关（sglang/llama.cpp）**总吞吐守恒**（c1=75.3s / c8=51.3s / c4≈c1），并发只换单条延迟；② **等待不是拒绝**（拿不到槽位就在队里等，不占算力也不烧 token，`signal` 中止即出队不泄漏槽位）；③ **不做「等待超时降级放行」**（单条时长已由 `l1_timeout_ms` 兜住，再加一条放行通路等于把上限悄悄捅破）。`GEBAI_TRIAGE_L1_GATE` 可显式设上限（内网放开），任务跑完自动卸载并按剩余成员重算上限。**侧证（假网关模型：单条延迟随在跑数线性上升）**：4 个 c8 大任务占满后，20 条小任务在旧行为下等 43s、有闸门后 12.8s；同场景大任务吞吐不变（总吞吐守恒），即小任务的份额是从**多占多占**里拿回来的，不是从效率里挤的
- **L1 失败重试（只重试「重试有意义」的失败）**：`l1_retries` 缺省 1，退避 500ms×n（上限 10s）。区分两类失败是省算力的关键——**传输类**（网络/超时/429/5xx，按 `http_status` 或错误文案判定）重试往往就好；**「模型不调输出工具」与「结构化校验不过」不重试**（再跑一次大概率同样失败，却要再付一次完整生成）。`runBatch` 因此新增 `retryOn` 谓词、`retryDelayMs` 退避与 `slotGate` 注入点（`BatchResult` 带上 `http_status`——OpenAI 兼容端点常把 5xx 响应体写成 `{error:{message}}`，解析后文案里没状态码）
- **落盘与续跑**：`{GEBAI_HOME}/users/{user}/triage/{job_id}/` 下 `items.jsonl`/`l1.jsonl`/`l2.jsonl`（精审原始输出）/`results.jsonl`（统一逐条结果）/`job.json`（状态 + 汇总 + 选项快照）；同 `job_id` 再次调用**复用已有 L1 结果**（断点续跑）。结果含 `layer`（L1/L2）/`confidence`/`evidence_index`/`evidence_chain`/`revised`/`action`/`l1`（初判留档，供校准对比）。`results.jsonl` **按输入顺序收尾重写**（不按完成顺序）——断点续跑与「tail 一部分先看」的语义基础
- **L2 限界**：`l2_max_items`（缺省 20，超出部分标「待精审」返回，可带更大值续跑）、`l2_batch_size`（缺省 5，按轮精审）、`l2_timeout_ms`（缺省 600000）；超时走**收尾**而非硬杀（子会话快速结束 / REST 侧 `engine.windDown`）
- **精审执行形态**：会话内派**隔离子会话**（`inheritContext:false`，不污染父会话，`merge:summary`）；REST 侧 `engine.run` 跑一轮完整会话。领域工具的装载指引统一由 `buildL2Prompt` 前置（单一来源，两条通道同文案）——会话内还会真实预载 `agents`。`l2_model`/`l2_api_base`/`l2_api_key` **两条通道都支持**：工具侧经**子会话自定义环境变量**（`env_mode=inherit` + `GEBAI_LLM_*`）落实、REST 侧经 `envOverride`——能力对齐（子会话 env 直接参与 Provider 解析，见「子会话运行」的环境变量自定义）
- **实测要点（跨场景成立）**：
  - **思考链是小模型的天敌**：推理型模型开着 thinking 会把输出预算吃光导致正文为空，故 `l1_enable_thinking=false` 为缺省
  - **小模型会过度触发「信息不足」**：默认提示词正反两面都写（有线索 → 0.8~0.95 并抄证据；确实无线索 → 0）——只强调后者时 1.5B 会把「没有额外数据」误判为信息不足，对 OOM 这类明确报错也给 0
  - **小模型能力决定这套模式是否成立**：同一批 3 条，1.5B/CPU 用 56.3s 且全部给 0（全量溢到精审，成本结构失效）；35B 用 4.6s、2 条正确采纳（OOM/代码退出 conf=1）、对信息量为零的条目如实给 0 上升
- **任务配置（`{GEBAI_HOME}/triage/preset.json`，`core/triage/preset.ts`）**：把「每次调用都要重复传」的业务面参数（`schema`/`label_enum`/`accept_labels`/`threshold`/`min_evidence_chars`/`l1_system`/`l1_prompt_template`/`l1_*` 调优/`target` 名称/`escalate`/`l2_*`）收进一个文件，调用方从此只需传 `items`。**每次调用重读（改完即生效，不必重启服务）**；**优先级恒为「显式参数 > 预设 > 内置缺省」**（文件不存在时本模块完全不介入，既有调用方行为不变）。四条边界：① **为何是文件不是环境变量**——它们是任务配置而非启动参数，环境变量装它们的代价是引号/中文转义易错、改一次要重启、且会随 `job.json` 选项快照散落到产物里；② **只管业务面**——端点 URL 与密钥仍归 `GEBAI_LLM_INFER_TARGETS` + `GEBAI_LLM_INFER_REMOTE_API_KEY`（密钥只存一处，掩码/审计口径不分裂）；预设的 `target` 只填目标名；③ **不接收调用侧字段**（`items`/`items_file`/`job_id`/`job_dir`/`result_limit`/`mode`/`sync_timeout_ms`）——误填**报错**并说明原因，不静默忽略；④ **预设值同样过校验**（不是绕过校验的后门），未知键忽略但**给告警**，`name`/`description`/`$comment` 作为编排说明键放行。两条通道同源生效（工具侧用 `ctx.home`、REST 侧用 `d.config.gebaiHome`）；可传 `ref` 指定旁处文件（相对基准恒为 `{GEBAI_HOME}`，与 `profiles` 的相对 cwd 不同——配置不能因启动目录不同而找不到）
- **REST 接口**：`POST /api/v1/triage/analyze`（body = 上述参数契约全集，另加通道专属的 `mode=async` 与 `sync_timeout_ms`）、`GET /api/v1/triage/jobs/:id`（进度/汇总；**运行中返回 `partial: true` 与已定案的部分结论**）、`GET /api/v1/triage/jobs/:id/results`（逐条，可按 `layer`/`only=ok|failed` 过滤；**运行中也可读**——增量消费，长任务不必等收尾）。每用户限流（缺省 10 突发 / 2 每秒，`GEBAI_TRIAGE_RATE_BURST` / `GEBAI_TRIAGE_RATE_PER_SEC` 可调，任一为 0 则不限流）
- **sync 整体超时保护（REST 专属）**：`sync_timeout_ms`（缺省读 `GEBAI_TRIAGE_SYNC_TIMEOUT_MS`，再缺省 120s，0 = 关）。超过即**降级为后台**：不中断已跑的任务，注册进后台任务注册表并返 **202**（`state: running` + `job_id` + `poll` + `degraded` 说明），调用方改拿 `job_id` 轮询——降级不是失败，已跑的进度不浪费。这是「外部调用方把 sync 请求感知为卡死」的直接解法（单条 `l1_timeout_ms` 缺省 600s，整批没有上限时一个请求可以挂很久）
- **免审批**：两个工具都不改变服务状态（发请求、派生会话、写产物目录）；`triage_view` 声明 `safeMode`
- **预加载**：`preload = false`，按需装载。

#### 命名与预加载总览

| 子Agent | 工具 | 审批 | 预加载 | 适用 |
|---------|------|------|--------|------|
| `code` | 独有工具 search_symbols/analyze/git/preview_server/env_detect/system_info（文件读写查询与交互编排复用**全局工具**——read/write/edit/sh/py/ls/grep/glob/file/fetch_url/ask/todo/subsession_run，带 project 参数路由项目，不重复注册） | 无（全局工具维持自身姿态） | ✗ | 代码编写与源码分析/修改（非 GEBAI 自身代码：tree-sitter 语法分析与符号双模式搜索（定义 + 引用/调用点）、git 只读核对（status/diff/log/show/branch/ls-files/grep）、浏览器端验证委托、项目内置、验证服务（自全局下沉 preview_server）与环境/工具链探测（env_detect/system_info）；文件读写/定点修改/待办规划/方案确认经全局工具完成） |
| `self_optimize` | 独有工具 read_feedback/run_tests/rollback/journal/backlog/page_capture；**通用工具与工作流直接复用**（装载/`subsession_run` 预加载均连带 code——文件读写用全局工具、分析/验证类操作用 `code_*` 独有工具（含 code_preview_server），code 工作流提示词随连带装载注入，不重复注册；视觉分析经 vision 子代理 `vision_analyze`/`vision_ocr`——依赖连带装载获得，def 不复刻）；声明 `writeGuard` 写范围守卫（核心引擎源码默认只读的代码级强制；可写面 = 子Agent 扩展面三域 + 仓库级文档/配置） | run_tests+rollback | ✗ | 优化歌白自身（tree-sitter/补丁应用/验证服务等通用能力经全局工具与 code；特有：反馈读取、测试准入（test/typecheck/lint 三件套）+回滚（含新建文件清理）、优化日志跨会话沉淀、待优化项暂存与集中全面优化（backlog 离线优化——知识/工具不足导致重复试错时先暂存后集中处理）、项目内置+AGENTS.md 自动注入、**三类子Agent 开发场景与目录（内置/二开/客卿）**、前端页面捕获读取实际 html/截图 + 视觉分析（vision 子代理）、写范围守卫；**装载即连带装载 code 与 vision**） |
| `explore` | 独有工具 search_symbols/analyze/git（全部只读，支持 project 参数路由；文件读取检索复用全局只读工具） | 无（全免审批） | ✗ | 只读代码探索（大范围摸底/架构梳理/多点位定位，subsession_run 委托执行（默认继承全局工具），返回结论与 文件:行号 清单，中间过程不占主上下文；修改用 code） |
| `shengsheng` | 独有工具 state/dispatch/collect/divine（→ `shengsheng_state`/`shengsheng_dispatch`/`shengsheng_collect`/`shengsheng_divine`；会话控制面与任务面经 `ctx.sessions`/`ctx.tasks` 契约消费，不重复注册 `session_*`/`task_*`；`.evolve` 状态由工具托管，不手写 markdown） | dispatch（派工会派生会改仓库的会话）+ state 的 task ensure/pause/resume（任务管理动作；读态/择种/断点与 task status 免审；collect 是回收动作、divine 只读 + 台账追加，均免审） | ✗ | 项目驻留自主演进的调度中枢（**控制平面**：由定时任务周期唤醒 sticky 会话，每息择种→布雨→收藏→观复，自己不跑测试/不改代码；worker 为独立事件循环的普通会话，池化复用（同域 idle 直派）与控制面限时解耦，死一跳不丢全局）。三工具按一息相位分件：`state`（复命读态含各 worker 实时态与待收藏线索 / init 固化项目坐标并写入会话 env / seed_add·seed_done 择种 / note 落断点 / task ensure·pause·resume·status 心跳任务管理）、`dispatch`（一批工单 1~8 单：复用 idle worker 或新建会话，按模板装配自包含派工消息（任务/上下文/交付/验证命令/纪律/预载），注入后立即返回不等 worker，自动登记池与在途；活动 worker 上限 3）、`collect`（review 深观消息尾与处置建议 / collect 入台账（记录键幂等）+ 标 idle + 在途清除 + 建议下一条入种子 / retire idle·stop·delete）；状态固定落 `<项目根>/.evolve/control.md` 与 `ledger.md`（结构化读写，字段结构与对账由代码保证）；项目坐标也可经 env `SHENGSHENG_ROOT` 等注入；无种可播时 pause（记台账 + 通知用户 + 停用驱动任务，藏器于身） |
| `vision` | analyze + run/pip/status + ocr/locate/locate_image/detect（装载后共 8 个：客卿侧四项本地识别与语言基础工具合并，全部只读免审批；image 必填无缺省源——不截图，实现复用 makeVisionTool/cv-analysis 共享工厂） | 无（全免审批） | ✗ | 视觉能力通用层（图片分析型，全局 vision 工具已移除——视觉语义分析唯一入口：主会话 agent_load/路由自愈，子Agent 依赖声明连带装载）：多模态语义分析（analyze）+ 本地 OCR/文字定位/模板匹配/YOLO 检测（**受客卿门控——客卿仅本地形态启用，沙箱启用即整体禁用，故服务端部署下只剩 TS 侧 analyze 可用**；被依赖方——self_optimize 声明 dependencies 复用；desktop/playwright 域内走共享工厂不依赖）；现截屏幕/页面用 desktop/playwright |
| `desktop` | screenshot/window_*/type_text/key_press/mouse_*/clipboard_*/screen_info/ocr/locate/locate_image/detect/wait_for | window_*+type/key/mouse+clipboard_write | ✗ | 桌面控制（截图（虚拟屏幕多显示器）/本地 OCR 识别与文字·模板定位/窗口（激活/移动/状态）/输入（输入/点击/滚动/拖拽）/剪贴板读写/界面条件等待，仅本地模式） |
| `feishu_docs` | auth_status/auth_user_authorize/auth_user_token/auth_user_status/auth_user_clear/create_doc/get_doc_meta/get_doc_text/fetch_doc/update_doc/set_table_width/get_doc_blocks/list_blocks/find_blocks/add_blocks/update_block/replace_text/delete_blocks/insert_image/import_markdown/import_xml/lint_doc/style_guide/export_doc/list_files/create_folder/get_file_meta/upload_file/download_file/delete_file/copy_file/move_file/task_check/list_comments/add_comment/resolve_comment/list_permission_members/search/create_sheet/get_sheet_meta/read_sheet/write_sheet/append_sheet/create_bitable/list_bitable_tables/list_bitable_records/add_bitable_records/update_bitable_record/delete_bitable_records/list_wiki_spaces/create_wiki_node/get_wiki_node/get_board/add_permission/set_link_share/api_call | 写操作全部（创建/修改/删除/上传/授权）；**只读类不审批**（fetch_doc / get_doc_text 读文档、lint_doc 体检、style_guide 读排版规范） | ✗ | 飞书云文档（文档/表格/多维表格/知识库/云空间/搜索/权限/思维导图画板；**可配置 user_access_token 以用户身份操作、创建用户所有权资源**；`FEISHU_DOCS_FOLDER_URL` 配置用户目标文件夹后创建的资源自动落该文件夹（用户自动有全部权限）；**读写三通道：XML 通道（fetch_doc/update_doc/import_xml，保真度最高）+ Markdown 通道（get_doc_text format=markdown/import_markdown）+ 块级底层工具（按 block_id 精确点操作）**；**云空间含复制/移动/异步任务查询，协作含评论读写与协作者列举**；**内置排版 SKILL（排版总纲 + 体裁选择表 + XML 排版语法 + 15 类体裁契约）与排版体检、写入前预检**；需配置 `FEISHU_DOCS_*` 或全局 `GEBAI_FEISHU_*` 凭证；`get_wiki_node` 缺省自动梯度 wiki→docx 重试（仅 not-found 类错误，免二次手动指定 obj_type），全未中时指引用户检查 token 来源） |
| `playwright` | open/content/screenshot/click/fill/press/select/check/hover/dblclick/drag/upload/wait_for/evaluate/pages/new_page/switch_page/close_page/close/serve_dir + pdf/downloads/dialogs/emulate/cookies/local_storage/storage_state + ocr/locate/locate_image | open+click+fill+press+select+check+hover+dblclick+drag+upload+evaluate+new_page+serve_dir+emulate+cookies+local_storage+storage_state（凭证类与 evaluate 同级） | ✗ | 浏览器自动化（无头 Chromium，node 桥接；选择器 `>>` 穿透 iframe；下载/对话框/仿真/登录态管理；页面截图本地 OCR/文字·模板定位（canvas 等无 DOM 内容兜底，沙箱可用）；需宿主机 node + playwright 包 + 浏览器） |
| `reverse_site` | 独有工具 capture_start/capture_stop/capture_clear/capture_list/capture_body/capture_replay/capture_curl/capture_har/capture_ws/route/http_request；**依赖 playwright（`dependencies` 自动连带装载——浏览器自动化全套与审批映射复用 `playwright_` 命名空间，共享同一浏览器会话）**，文件读写与编排走全局工具 | http_request+capture_replay+capture_curl（route add 工具级函数按 action；浏览器交互类随 playwright def） | ✗ | 网站/接口逆向（网络录制+WebSocket 帧还原接口、带登录态改参重放、重放命令生成、HAR 导出、请求拦截 block/mock/modify、直连探测验证、产出 API 文档；可联动 self_optimize 转新子Agent；需宿主机 node + playwright 包 + 浏览器） |
| `feishu_group` | chats_list/chat_info/members_list/user_info/message_send/chat_create/chat_update/chat_members_add/chat_members_remove/chat_disband | 写操作全部（发消息/建群/改群/拉人/移人/解散） | ✗ | 飞书群基础能力（群列表/详情/成员查询（open_id+姓名——@特定人与 cron 通知 at 名单取材）/用户信息/群内发消息（at 标签）/建群改群/成员增删/解散；需 FEISHU_GROUP_APP_ID/SECRET 或全局 GEBAI_FEISHU_* 凭证） |
| `task` | add/list/update/run/cancel/remove/files/notify（→ `task_add`/`task_list`/`task_update`/`task_run`/`task_cancel`/`task_remove`/`task_files`/`task_notify`） | add+update+remove+run+cancel+files | ✗ | 统一任务管理（定时/普通/闲时三类共用一条队列：创建脚本运行/提示词运行 agent 的用户级无人值守任务、查看/修改/手动执行/取消/删除、任务资源文件与模型主动通知 `task_notify`；支持执行目标（独立新会话/专用会话/绑定会话）、时区、@at 一次性、错过补跑、飞书群/webhook 通知、连续失败自动停用） |
| `wps` | word_create/word_read/word_append、excel_read/excel_write/excel_edit/excel_batch、ppt_create/ppt_read、pdf_create/pdf_read/pdf_merge/pdf_split/pdf_edit（projectAware 项目路由；文件浏览与交互编排复用全局工具） | 无（防盲覆盖守卫在工具体内，与全局 write 同语义） | ✗ | Office/PDF 文档处理（.docx/.xlsx/.pptx 读写与富排版：markdown/块结构生成 Word、原 XML 追加保留原文档格式、Excel 多表公式样式与 ops 批量编辑、表格批量数据管道 excel_batch——多文件导入合并/导出/清洗/分组统计/透视/GBK 编码 csv 声明式完成（无 Python 依赖），PPT 版式/图表/图片/备注，csv/tsv 读取；PDF 生成（中文字体自动嵌入子集化）/逐页文本提取/合并/拆分/页面编辑与水印；旧版二进制格式 .doc/.xls/.ppt 不支持） |
| `tts` | speak/models/install/voices/sfx/effect/mix（→ `tts_speak`/`tts_models`/`tts_install`/`tts_voices`/`tts_sfx`/`tts_effect`/`tts_mix`） | 无（会话内产物落盘；安全模式不提供） | ✗ | 语音合成与音效：把文本合成为可播放音频——**两档离线引擎**（`engine=neural`：sherpa-onnx + VITS/MeloTTS，音质接近云端、44.1kHz/22.05kHz、可选上百音色，需 Python 侧车 + `sherpa-onnx` + 模型；`engine=auto/winrt/sapi`：Windows 系统语音，零安装兜底）；均不联网、不耗配额，非 Windows 上系统语音档如实报错不回落在线服务。另含音效合成（27 个预设 + 自定义波形与滑频）、音频效果处理（变速不变调（相位声码器）/变调/变速/回声/混响/滤波/机器人音/淡入淡出等）、多轨混音与拼接（提示音+语音+结束音一次成段，循环铺底）——音效与处理为纯计算，任何平台可用；产物 WAV 落会话 tmp/tts/、聊天内直接播放；`play=true` 同时在运行 GEBAI 这台机器的扬声器上播报（后台播放，仅本地模式）；其引擎基建（`core/tts/speech.ts` 系统语音、`core/tts/neural.ts` 神经合成、`core/tts/audio.ts` 音效）同时被 `caiyun`（裁云配音轨）复用（同一份实现，不复制） |
| `imgproc`（客卿） | info/grayscale/resize/stats（→ `imgproc_info`/`imgproc_grayscale`/`imgproc_resize`/`imgproc_stats`） | 全部 | ✗ | 图像处理（C++ 典型场景：stb 单头库 vendor（`cpp/stb/`，解码/编码/重采样），尺寸与亮度分布探测、Rec.601 灰度化、sRGB 高质量缩放（等比/倍率）、RGB 通道统计与 Otsu 阈值；构建引导自动编译） |
| `disk`（客卿） | tree/du/top/depth/volumes/scan/clean/trash（→ `disk_tree`/`disk_du`/`disk_top`/`disk_depth`/`disk_volumes`/`disk_scan`/`disk_clean`/`disk_trash`） | 全部 | ✗ | 磁盘使用分析与清理（Go 典型场景：goroutine 并发遍历 + 原子在途计数（无死锁收尾），目录树概览/指定深度占用排行/大文件排行（可按扩展名过滤）/结构统计（总量、最大深度、空目录），du 语义子树大小；盘容量总览（linux/darwin/windows 分平台探测）；清理候选扫描（temp/log/backup/dump/cache/empty_dir 类别与 big_file/old_file 阈值）；执行清理 dry-run 预览 / quarantine 隔离（可 trash restore 还原）/ delete，带范围护栏（目标限 dir 内、拒绝卷根·用户主目录·系统目录、符号链接跳过、条目上限）与审计日志；隔离区批次列出/还原/彻底清除；go module 管理） |
| `nsight` | doctor/reports/overview/kernels/timeline/query/findings/compare/kernel_detail/locate/capture + aggregate（客卿 Rust 边车贡献的原生聚合后端；报告路径类工具带 project 参数；全部分析只读） | capture（执行被分析程序） | ✗ | NVIDIA Nsight 报告分析与 GPU 性能问题定位（Nsight Systems 时间线 + Nsight Compute 单内核）：报告 → 问题清单（量化证据 + 根因 + 修复方向）→ 源码 `文件:行`；**多卡按 deviceId 分流**（每卡利用率/空闲/并发 + 不均衡诊断，合并口径会掩盖单卡停滞）；memset 计入 GPU 活动；采集开销按窗口内外区分；CUDA Graph 维度；参数化时间窗；**报告间对比**（改前改后）；结果可导出 Markdown；超大报告流式聚合 + 事实缓存（内存与规模解耦）；聚合有原生（Rust 边车）/ JS 两条同构实现，原生优先、不可用即自动回退；ncu 采集需 GPU 性能计数器权限（工具前置探测并给出开启方法），nsys 采集与报告分析不需要 |
| `torch` | reports/overview/ops/memory/findings/compare/capture（→ `torch_reports`/`torch_overview`/`torch_ops`/`torch_memory`/`torch_findings`/`torch_compare`/`torch_capture`）**+ aggregate（客卿 Rust 边车贡献的原生聚合后端）**；trace 路径类工具带 project 参数；除 `capture mode=run` 外只读 | `capture mode=run`（执行被分析脚本） | ✗ | PyTorch Profiler trace（Chrome Trace / Kineto）分析与代码定位：trace → 算子/内核热点（含自身耗时与张量形状）· 步级耗时与抖动 · **前向/反向拆分（fwdbwd 配对）** · **内核→发起算子归属（correlation + ac2g 流）** · 显存峰值与碎片率 · 问题清单（同步/CPU 受限/Python/碎片化/autograd/小内核/显存/精度与布局）→ 源码 `文件:行`；**原生聚合后端**（整文件读入 + 字节级扫描，实测 4.7–13× 于 JS、峰值内存约为 1× 文件大小，且逐字段一致；不可用时自动回退 JS 并如实回报原因）；**分块并行**（rayon 分块采集 + 有序归并，实测 224 MB/94 万事件 1.5×、672 MB/279 万事件 1.59×，`TORCH_NATIVE_THREADS` 可调/可关，逐字段一致由同二进制改块数的 A/B 锁定）；**时间预算 + 落盘事实缓存**（超预算返回未完成 + 后台命令，跑完再调毫秒级命中）；**trace 间对比**（改前改后）与结果导出 Markdown；超大 trace 流式扫描 + 缓存；与 `nsight` 零互相引用、可同时装载（Windows 上 PyTorch CUPTI 采集不可用，GPU 内核级时间线由 `nsight` 的 nsys 采集补齐） |
| `llm_infer` | engines/engine_fetch/model_fetch + **sources/source_build（内网源码编译）** + status/models/start/stop/restart/logs/bench/inspect + targets/generate/batch/jobs（→ `llm_infer_engines`/`llm_infer_engine_fetch`/`llm_infer_model_fetch` + `llm_infer_sources`/`llm_infer_source_build` + `llm_infer_status`/`llm_infer_models`/`llm_infer_start`/`llm_infer_stop`/`llm_infer_restart`/`llm_infer_logs`/`llm_infer_bench`/`llm_infer_inspect` + `llm_infer_targets`/`llm_infer_generate`/`llm_infer_batch`/`llm_infer_jobs`） | start+stop+restart+bench+engine_fetch+model_fetch+source_build | ✗ | 推理引擎的**全生命周期管理 + 统一调用入口**（子项目 `infer/`）——**推理不绑定操作系统、不绑定 GPU、不强制外网**（引擎矩阵覆盖 Windows/Linux/macOS × CPU/CUDA/Vulkan/Metal/ROCm/SYCL；无 GPU 用 CPU 档 + 小模型；**无外网用源码就地编译**）：**引擎供给**（`engines` 列矩阵×本机平台×已安装状态并给设备探测依据，`engine_fetch` 按资产直下（断点续传 + 校验 + 解压 + **递归定位可执行文件** + 写 `.engine.json` 标记）、`model_fetch` 下载模型（含预置小模型，幂等））；**内网/离线：源码发现与自动编译**（`sources` 发现 `{GEBAI_HOME}/resources/src/` 等根下的源码目录/归档并给离线可行性依据（归档预览：CMakeLists、**是否自带 vendor/ 依赖**——采样会假阴性故全量过滤）、`action=toolchain` 给工具链与各设备就绪 + 内网补齐办法（含各工具绝对路径，便携工具链不必改 PATH）；`source_build` 解压 → 工具链与设备门禁 → cmake 配置 → 编译 → 定位 llama-server → 安装到 `vendor/<engine-id>/bin/`（解引用符号链接）→ 写同格式标记（额外记 `build.{from_source,device,offline,log}`）；`background=true` 后台编译，`status/log/list/cancel` 管理；预编译包需外网，内网走这条）；**进程管理**（跨平台 launcher 为缺省启动路径——detached 拉起 + 日志重定向 + `/health` 就绪轮询，**不依赖 PowerShell**；状态文件 `infer/run/server-<port>.json` 记 PID/档位/模型/引擎/启动方式/日志/argv，status 逐个校验 PID 存活，stop 可按 PID/端口/档位精确终止进程树，restart 换档位/换模型/换引擎，logs 尾读日志并给错误特征摘要；Windows 无引擎时回退 `run-server.ps1`）；**统一推理目标**（`targets` 列本机/命名远端目标并可探活（n_ctx/slot/模型名），`generate`/`batch` 的 `target` 可选本机、直连 URL（可带 api_key）或 `GEBAI_LLM_INFER_TARGETS` 声明的命名目标——同一套调用面可派到本机 CPU 引擎、局域网机器或云端兼容端点，密钥一律掩码）；**批量提交**（条目数组或 JSONL、worker pool 限流（与档位 `parallel`/服务端 slot 比对）、逐条 append 落盘、按 id 断点续跑、`max_items` 分片，`background=true` 后台跑并立即返回 job_id，`jobs` 从 results.jsonl 实时统计并按 `owner_pid` 探活）；**结构化输出**（`response_format.json_schema` 交服务端转 GBNF 约束解码，工具侧再抽取 + 轻量 Schema 校验，失败回灌重试一次；`grammar` 直传兜底、`json_object` 最弱，服务端不认约束时自动降级并给预警）、模型与档位清单、llama-bench 基准、GGUF 结构与张量布局解析；工具复用子项目配置与脚本不复制逻辑；环境变量 `GEBAI_LLM_INFER_HOME`/`GEBAI_LLM_INFER_PROFILE`/`GEBAI_LLM_INFER_PORT`/`GEBAI_LLM_INFER_MODELS_DIR`/`GEBAI_LLM_INFER_TIMEOUT_MS`/`GEBAI_LLM_INFER_BATCH_MAX_ITEMS`/`GEBAI_LLM_INFER_TARGETS`/`GEBAI_LLM_INFER_DEFAULT_TARGET`/`GEBAI_LLM_INFER_REMOTE_API_KEY`/`GEBAI_LLM_INFER_LAUNCH`/`GEBAI_LLM_INFER_SOURCE_DIRS`（未传 `target` 时的缺省目标由 `DEFAULT_TARGET` 指定，未配置即 `local`；指向不存在的命名目标显式报错不静默回退，`targets` 输出标注当前缺省目标） |
| `triage` | run/view（→ `triage_run`/`triage_view`） | 无（会话内产物落盘；view 支持安全模式） | ✗ | 通用的大小模型协同（两级研判）：小模型批量粗筛（工具式结构化输出 + 置信度）→ 阈值/证据分流 → 低置信度条目交大模型精审兜底（会话内派隔离子会话、REST 侧走 Agent 引擎会话，均可装载领域子Agent 主动取证）；场景无关，接新场景只需把领域数据转成统一特征描述（详见上方 `triage` 节） |
| `ai_native_eng` | 无独有工具（知识型子Agent：纯提示词 + 全局工具） | 无 | ✗ | AI 原生软件工程方法论——把「以智能体为执行者、以人为验收者」的开发形态讲清楚：无头闭环（控制通道 + 截图 + 像素断言）、协同自进化（框架问题当场修/写 BACKLOG、沉淀位置四分流）、观感类改动的硬约束与八步循环、智能体工程纪律（记忆落盘/审批留痕/逆向验证/「改了没生效」三变体）、跨平台不可见坑。来自霜天框架的完整实践（含四次返工与 20 类运行期缺陷的实测代价）。需要评审开发流程、沉淀方法论、或给新会话指路时装载 |
| `imagegen` | imagegen/assets/check（→ `imagegen_imagegen`/`imagegen_assets`/`imagegen_check`） | 无（只写自己的产物目录） | ✗ | 本地文生图（sd-turbo / sdxl-turbo，1~4 步出图，512² 约 1 秒）——生成的图直接落进素材库目录，供裁云/镂月按名字引用；多张不同风格时逐次调用（每次独立种子），提示词用英文效果更好，**绝不要求模型画文字**（会画出扭曲字母，文字必须由渲染器画）。环境变量：`GEBAI_IMAGEGEN_MODEL`（切换模型）、`GEBAI_IMAGEGEN_PYTHON`（指定解释器）。**`caiyun`/`louyue` 经 `dependencies: ["imagegen"]` 连带装载它** |
| `shuangtian` | run/tree/find/get/set/invoke/capture/capture_hash/click/type/key/wait/wait_event/visual/visual_diff/metrics/apps（+ call 逃生门） | 无（真实鼠标键盘注入是设计前提） | ✗ | 构建、运行与操控**霜天（Shuangtian）原生桌面应用**：无头模式开发闭环（构建/启动/停止/测试/禁令扫描）、组件树与选择器查询（tree/find）、属性读写与动作触发（get/set/invoke）、真实鼠标键盘注入、截图 PNG（无头唯一视觉通道）、视觉树、条件等待、运行指标（后端/无头/DPI/帧耗时）。坐标一律逻辑像素；无任意代码执行入口。适用于用智能体开发与验证原生界面；真实宿主桌面操作用 `desktop`。依赖：C++ 工具链 + 霜天框架源码 |
| `caiyun` | 零自有工具：依赖 `shuangtian`/`imagegen`/`tts`（→ `shuangtian_*`/`imagegen_*`/`tts_*`） | 导航/交互/凭证类继承霜天姿态 | ✗ | **裁云（Nimbus）**：用霜天框架做 AI 视频：写导演脚本 → 选素材 → 配音与音效 → 出片并核验。裁云真值源 = JSON 导演脚本（自研 C++ 渲染器，纯离线无外部依赖），可直接出 mp4（含配乐/配音/音效，以及景深/光晕/粒子/运镜等影像特效）。**音效默认不加**——裁云只对配乐做 ducking、对音效不做，音效会与旁白抢响度（实测比旁白还响 2.5dB） |
| `louyue` | 零自有工具：依赖 `shuangtian`/`imagegen`（→ `shuangtian_*`/`imagegen_*`） | 同上 | ✗ | **镂月（Lunaris）**：用霜天框架做海报：写海报脚本（`blocks` 块列表）、排文字层级、贴素材图、导出 PNG 并核验。真值源是 JSON 海报脚本，一份脚本能出横版/方版/竖版多种尺寸（**重排版，不是拉伸**）。块类型：background/title/subtitle/body/rule/badge/image/frame/grid/corners/divider + 四个通用图元 line/path/rect/paragraph。**坐标口径**：图元用设计像素（按 1080 高设计，随画幅缩放），`x` 仍是 0~1 比例锚点——两者语义不同、不能混用 |

#### 工具面的两种装配方式（依赖声明 vs 自身内联）

子Agent 复用别人的工具只有两种合法方式，**不存在「两个都做」**（重复注册会多一份审批映射、多一个命名空间，且提示词里该写哪个名字会分叉）：

- **依赖声明**（首选，也是默认）：`dependencies: ["imagegen"]` —— 装载时自动连带，工具以 `{dep}_` 注册，**不在自己命名空间下再多一份**。现有例子：`caiyun` = `shuangtian`+`imagegen`+`tts`、`louyue` = `shuangtian`+`imagegen`（两者均零自有工具）、`reverse_site` = `playwright`、`self_optimize` = `code`+`vision`。
- **自身内联**：`tools = { ...depTools }` —— 工具以**自己**的命名空间注册。仅在依赖声明物理上不可行时使用：工具注册表禁「子Agent 名互为前缀」（`core/base/registry.ts`，`resolve` 与路由自愈的最长前缀匹配靠它保持无歧义），前缀关系下写 `dependencies` 会在装载时抛 `sub-agent name prefix collision`。

**命名前缀是选名的硬约束，不是事后可选**：子代理名与它要依赖的名字不得互为前缀。选名时先确认这一点——撞上前缀就只能把依赖方工具内联进自身（工具名变 `{自己}_{dep}_{tool}`，且提示词里的工具名必须写实际注册名，否则靠引擎路由自愈会被前缀碰撞拦下）。`caiyun`/`louyue` 这两个命名正是为了与 `shuangtian` 不构成前缀而取的；它们零自有工具，霜天 18 个 + imagegen 3 个 + tts 7 个工具全由依赖提供。

#### 客卿（多语言子代理：边车协议 + 自动发现启动注册）

任意语言（Python/C++/Go/…）实现的子代理：**放置即自动发现 → 启动边车进程 → 握手拉取工具清单 → 注册为标准子代理**（`agent_list` 可见、`agent_load` 装载、`subsession_run` 委派——与 TS 子代理完全同构）。设计原则：**实现语言对模型透明**——子代理 = 工具 + 提示词（能力导向命名与描述），语言仅是工程组织维度。**self_optimize 可写**（客卿域 `keqing/` 属子Agent 扩展面，与内置域/二开域并列；改动需重新构建时由 manifest `build` 自动引导）。

- **按语言组织**（仓库根 `keqing/{lang}/`）：语言目录下共享基础框架驱动（协议实现 + 语言生态工具）与运行时资产（venv/构建脚本），每个二级目录一个子代理项目（manifest + 提示词 + 专属工具）——一种语言派生任意多个子代理；发现根即各语言目录（每个语言目录一个扫描根，二级目录即子代理项目），用户自建 `{GEBAI_HOME}/agents/{name}/` 同构（同名覆盖内置）
- **manifest 发现**（`core/agents/keqing.ts`）：扫描内置源 `keqing/`（dist 构建时由 build-subagents 整树复制——过滤 venv/__pycache__/编译产物；二进制形态物化 `{GEBAI_HOME}/vendor/keqing/`）；manifest 字段/占位符（`{python}`/`{driver}`/`{agent_dir}`/`{lang_dir}`/`{agent_name}`/`{exe}`/`{GEBAI_HOME}`）见 `keqing/README.md` 协议规范
- **构建引导**（编译型语言开箱即用）：manifest `build` 字段（`command`/`windows`/`unix` 平台分支）声明编译命令，command 首元素指向的可执行文件不存在时自动执行（占位符同 command，cwd 为 manifest 目录）——C++ 经 `build.bat`（vswhere 定位 MSVC）/Rust 经 `rustc` 直编；缺编译器/构建失败记 loadErrors（模型可见根因），不阻断其他子代理；可执行体已存在则跳过（增量）
- **边车协议 v2**（语言无关，NDJSON over stdio）：`init`（上报 name/protocol，须与 manifest 与宿主一致——锁步升级不做历史兼容，不匹配即注册失败记 loadErrors）→ `tools.list`（工具清单：裸名 + JSON Schema 原样透传）→ `tool.call`（驱动侧执行返回 `{output, data?}`，请求体顶级字段 `tool`/`args`/`ctx`）；**请求级 ctx**（v2 核心）：边车为进程单例跨会话共享，会话上下文随每次 tool.call 传递——`ctx.cwd`（会话工作区绝对路径，相对路径解析基准）/`ctx.env`（任务级 env 覆盖，随消息变随调用传）/`ctx.sessionId`（会话态隔离键，如 REPL 命名空间分桶）/`ctx.user`/`ctx.sandboxed`；四语言框架分发循环串行执行内设「当前请求 ctx」并提供 `ctx_env(key)`（请求 env → 进程 env 回落，**禁止写 os.environ**——并发请求不同会话互踩）与 `ctx_resolve(path)`（相对基准=ctx.cwd）助手；stdout 只写协议行、stderr 自由排障、stdin EOF 即退出防孤儿；**行尾容忍 CRLF**（Windows 驱动 text-mode stdout 默认翻译 `\n` 为 `\r\n`，宿主行解析剥尾部 `\r`——跨语言驱动不因平台换行约定挂起）
- **Python 侧测试基建**（`keqing/python/tests/`）：纯标准库运行器 `run_tests.py`——发现同目录 `*_test.py`、执行 `test_*` 可调用、汇总 pass/fail/skip（**依赖缺失用 `skip()` 显式跳过而非失败**：AI 依赖是可选的，缺 numpy/onnxruntime 不该让 CI 变红）；已由 `bun test` 包装（`packages/server/src/core/agents/keqing-python.test.ts`，缺解释器自动跳过、`GEBAI_TEST_PYTHON` 可指定）。覆盖两类最易隐性失败的面：**纯函数**（区域解析、分块 OCR 去重的 IoU + 阅读序、资源目录回退链）与**协议契约**（`init`/`tools.list` 字段、未知 op 必回一行而非挂起、stdin EOF 即退出）。
- **边车宿主**（`core/agents/sidecar.ts`，进程管理对齐 CV sidecar）：惰性启动/启动串行化/请求 id 配对并发复用；**工具执行时按子Agent 名从活跃注册表取当前边车实例**（装载时的工具对象跨重扫存活——热加载重扫会 dispose 旧实例并登记新实例，闭包直用旧实例会让已装载会话在重扫后全部报「边车已销毁」且不再拉起；取活跃实例后源码改动即「重扫 → 新进程加载新代码 → 已装载会话立即生效」，等效热重载）；**请求级 ctx 组装**（`sidecarTool.execute` 从 ToolContext 取 sessionId/user/sessionWorkdir/env/sandboxed 随每次 tool.call 下发；崩溃重发即原样重写请求行，ctx 不丢）；请求超时杀进程重启；**崩溃自愈**（意外退出自动重启一次 + 在途请求重发一次；连续快速退出 3 次放弃自动重启防抖动风暴，下次调用再拉起）；exit hook + 驱动 EOF 双保险；stderr 环形缓冲
- **生命周期集成**：`SubAgentManager.discover()` 尾部并行启动（boot 显式接线后生效——`setKeqingOpts`，测试不注入零影响；`roots` 选项可覆盖发现根供测试隔离）；**启动不阻塞**：boot 以 `discover({ deferNative: true })` 调用——TS 域扫描（本地文件遍历 + 定义 import）同步完成，客卿发现转后台任务（进程内同时在途只保留一份），就绪后自动重建合并视图；需要完整清单的操作（`load`，即 agent_load/路由自愈/预加载）经 `whenNativeReady()` 等它。为何必须如此：侧车握手与**编译型客卿的构建引导**耗时不可控（实测：受限网络下 go 下载 toolchain 的 TCP 超时把服务启动拉长 30s；`ensureBuilt` 自身的超时上限为 300s），启动不该为可选能力付这个代价；代价是启动瞬间（后台发现未回）的客卿子代理尚未进入清单——TS 侧同名贡献仍可见，下一次操作自动补齐；manifest/驱动/提示词文件变化纳入热加载签名（重扫重注册，**目录删除对账回收**——本次发现名单中消失的从贡献集移除，进程级边车注册表同步回收旧进程）；**TS 签名与 客卿 签名各自判定**——TS 目录未变而仅 客卿 变化时 `refreshIfChanged` 只重拉 客卿（幂等跳过 TS 扫描），REST `GET /api/v1/sub-agents` 响应前惰性调用 `refreshIfChanged`（放置新目录即出现在列表，无需重启）；pip 安装成功后驱动主动退出 → 宿主自愈重启 → 命令工厂重新解析占位符（venv 创建后自动切换 venv 解释器，无需重启服务）
- **跨语言同名合并**（`core/agents/merge.ts` `mergeSubAgentDefs` 纯函数）：TS 侧（`packages/agents/src/agents/{name}.ts`）与 客卿 侧（manifest 目录）同名定义共存为「贡献集」，SubAgentManager 分别维护 tsDefs/nativeDefs、任一变化后重算合并视图（TS 贡献在前）——description/systemPrompt 非空项依次拼接（全空兜底生成）、工具集并集（同名冲突保留 TS 侧并告警）、dependencies/envVars 并集、preload 取或、projectRoot/writeGuard 取首个非空；装载后两侧工具同一 `{name}_` 命名空间，卸载一并注销。配套约定「只在一处定义、其他地方留空」：manifest `description` 可省略、`PROMPT.md` 可缺失（即本侧不贡献，不生成占位文本）；分工样例 `vision`——重计算（onnxruntime 推理）与描述/提示词由 Python 侧写（`keqing/python/vision/`），LLM 耦合（多模态 provider）的 `analyze` 由 TS 侧写（见「视觉能力分层与子代理复用边界」）
- **边车环境**：基于宿主进程 env 继承基础变量（PATH/SYSTEMROOT 等——Windows 下 python 编码/subprocess 初始化依赖 SYSTEMROOT，极小 env 会启动即卡死无报错）+ `GEBAI_HOME` + `GEBAI_AGENT_DIR`（驱动定位子代理项目专属资产，如 Python 驱动加载 `{agent_dir}/tools.py`）+ manifest env 覆盖同名项；manifest `cwd` 语义收敛为驱动自身资产定位（进程工作目录），会话级路径一律经请求 ctx 传递——与进程单例跨会话共享的模型自洽
- **项目工具热重载（Python 驱动）**：`{agent_dir}/tools.py` 不再只在进程启动时加载一次——每次 `tools.list`/`tool.call` 前比对 mtime（一次 `os.stat`，开销可忽略），变更即重新加载（stderr 记 `[driver] 项目工具已热重载`）。**重载从基础集重建再叠加项目工具**（否则追加式合并会让上一版改名/删掉的工具残留）；`tools.py` 被删除 → 回退基础工具集；源码语法错误 → 回退基础集且不影响基础工具（失败安全，进程不退出）。为何需要：旧行为下改完源码旧代码持续生效（除非重启边车），调试本地能力（vision）时极易误判「修改无效」。重载会重置该模块的模块级状态（如惰性加载的模型），故只在源码真的变了才做
- **门控与边界**：仅本地形态（沙箱启用即禁用；`GEBAI_KEQING=off` 显式关闭）；边车工具缺省恒需审批（任意代码执行面），manifest `requiresApproval:false` 可声明只读免审批（如 vision 识别四工具——对齐 TS 侧同能力工具体验）；单项失败（manifest 损坏/启动/握手失败/协议版本不匹配）记 loadErrors 模型可见根因，不阻断其他子代理；工具名驱动侧为裸名（注册表自动加 `{agent}_` 前缀）
- **内置客卿子代理**（`keqing/{python,cpp,rust,go}/`）：`vision`（Python + TS 跨语言合并，见下：本地识别四工具 ocr/locate/locate_image/detect + TS 侧 analyze）；`imgproc`（C++：stb 单头库 vendor 于 `cpp/stb/`，info/grayscale/resize/stats 图像处理）；`disk`（Go：磁盘使用分析与清理——tree/du/top/depth 并发目录分析 + volumes 容量总览 + scan/clean/trash 清理与隔离区，go module 管理）；**Python 语言目录项目同时携带语言框架基础工具** `{agent}_run`（常驻命名空间 REPL：末行独立表达式求值 repr 回显，**缺省按 ctx.sessionId 隔离命名空间**——同会话共享状态跨会话互不可见，显式 session 参数仍可细粒度隔离，stdout/stderr 捕获，timeout 秒默认 300）/`{agent}_pip`（install：packages 或 `-r requirements.txt`，venv 不存在自动创建，装完边车自动重启加载，freeze 快照写回）/`{agent}_status`——现为 vision_run/vision_pip/vision_status，经 `tools.py` 合并模式与项目工具共存；解释器解析 `GEBAI_PYTHON_DIR` → 语言目录 venv（`keqing/python/venv`）→ 系统 PATH；四语言基础框架（Python 共享驱动 + C++ 头文件 + Rust cargo workspace + Go module，均含请求级 ctx 助手）见 `keqing/README.md`
- **协议方向性约束（依「核心不变式：能力外置、权力内守」）**：客卿协议为**单向下行**（宿主 → 驱动 init/tools.list/tool.call，驱动只回响应）——驱动侧不得自取工具、不得自证授权；未来若开反向调用（协议 v3+）必须为**委托式**：驱动只发意图，宿主按该次 `tool.call` 绑定的会话 ctx 与调用者审批姿态重新裁决，并需来源可证不可提权（宿主签发 token，不得转交）+ 回调白名单（默认只读）；自取式回调（驱动自主取用工具/自带授权声明）不予准入

> 全部按需装载（懒加载）；`GEBAI_PRELOAD_SUB_AGENTS` 可指定启动预加载名单，符合「预加载少而精」原则。

#### `shengsheng`（生生：项目驻留自主演进的调度中枢）

实现于 `packages/agents/src/agents/shengsheng/`（目录形式：`shengsheng.ts` 定义入口 + `shengsheng.md` 系统提示词 + `shengsheng.test.ts`）。把任意 git 项目变成「持续变好、无人值守」的演进流水线：**定时任务周期唤醒一个 sticky 控制会话（生生），它只做控制平面——择种、布雨、收藏、观复；重活全在 worker 会话（数据平面）**。

- **提交纪律：宽范围暂存会带走别人的东西（`commit-scope.ts` + `shengsheng.md` 第六节）**：同一个 git 仓上可能同时跑着别的 worker，成果就摊在**同一棵工作树**里。`git add -A` / `.` / `-u` / `commit -a` 会把它们一起提交，**而根因是守卫的缺口而非粗心**：`edit`/`write` 的防陈旧覆盖守卫只管**已跟踪文件**的修改，未跟踪的新文件（`??`）**不受任何守卫保护**——谁 add 谁带走，而新增文件恰是 worker 最常产出的东西。
  - **实测（2026-10-17，本仓）**：worker `shared-lease` 的 `856ba9bb` 带走了另一个**仍在跑**的 worker（`retire-tree`，384 消息）两份未完成交付物——`proc-tree.ts`（**+596**）与 `shengsheng-retire-tree.test.ts`（**+573**），提交说明里只字未提。内容没丢，但：另一个 worker 按「我还没提交」继续干 → 重复劳动或旧基线；台账归属记错；有缺陷时责任人定错。
  - **纪律（写进派工模板，由 `commitProtocolLine` 生成）**：① `git add -- <逐个路径>`；② 提交前 `git diff --cached --name-status` 逐行核对，出现不在工单清单里的文件就**停下**；③ 工作树有 `??` 时先问「这是谁的新文件」。五种宽范围形态（`-A`/`.`/`-u`/`--all`/`commit -a`）逐条列为禁用——**不是风格问题**。纪律文本与 `stagingCommands()` 同源（示例路径取自本单 `deliver`，照抄即合规）。
  - **收单判据（`collect action=review`）**：核「最近一次提交 vs 该 worker 工单提到的路径」。三态必须分开报：**读不到 git ⇒「本项未核」**（不得当成「无异常」——那样等于在最需要警报的环境里安静）；核过无异常 ⇒ 明说已核 N/N；有文件不在工单里 ⇒ 报**疑似宽范围暂存带走了别人的改动**，逐行列 `状态 路径（+N −M）`，并**不要自动 revert**（回滚会把已落盘成果一起抹掉：先核归属、台账记清，再按归属续派给真正的作者）。工单侧路径由任务/方向/预期产出三段自由文本抽取（`mentionPaths`），覆盖判据三档：同路径 / 提到是目录 / 文件名相同。同时把工作树里的 `??` 逐条提醒一遍（那是下一次宽范围暂存会带走的东西）。
  - **读 git 的一个实测细节**：`git log --name-status --numstat` 里 **numstat 行根本不出现**（name-status 胜出），于是「+596」这个事故最重要的特征会被静默丢掉——必须用 `--raw --numstat`。

- **收藏纪律：「加了机制」≠「机制生效」（`shengsheng.md` 第七节，与本节同源）**：worker 声称「加了拦截/自检/闸门/指纹」只等于**它写了一段代码**；机制在真数据上拦不拦得住是另一回事，未在真仓数据或全往返路径上验过的机制即**「绿而无效」**——它绿得理直气壮，且坏时**没有任何信号**提示「刚验过的结论已过期」（比红更难发现，与本节「过窄 = 假绿」判为同一侧错误）。两条可操作口径：① **要一条会红的反证**——改一个**被依赖的模块**（不是根）应触发红；② **走全往返**——走真公开面（`parse→render`、真文档→解析→渲染）而非只调内部函数、「只喂手拼格数组」的半个证人；另加一条**只对机制类单**的收单问句：**它被证伪过吗？**（自检还须杀得死退化：真仓数据恰让机制空转时，退回旧实现仍须被断言杀死）。此纪律源自本仓**四次同型实测**：`9b3cbd86` 闭包拦截**只在拦截侧落地**、指纹侧未落地 → `dfe18f67` 修正 → 真仓闭包恰为**空集**、修复形同虚设（退回旧实现行为等价、无人察觉）→ `20d245fe` 补「防退化自检」才抓出；`37404178` 的「加列只动声明」演练**只喂手拼格数组**、未走 `parse→render` 全往返，三处残留因此存活而无人察觉。
- **独占资源租约（负载敏感的测量与门禁不得互相污染）**：`orders[].resource = "<名>"`（缺省名 `cpu-exclusive`）或简写 `exclusive: true` 声明「本单开工期间该资源不得被其他 worker 持着」；`control.md` 增「资源租约」节（资源名|持有者别名|会话|起租|到期|用途）。语义与「活动 worker 上限」同型——**只挡申请同一资源的人，不挡无关工单**（读码/改文档/查日志照派；当它挡住不该挡的活，那是工单标错了 `exclusive`，改工单不改机制）。
  - **先领租约、后开跑**：冲突检查与写租约在 `mutateControl` 的**同一把锁内**完成，且放在 `svc.run` **之前**——同一批多单、同一息多次 dispatch、两个演进会话同时布雨，都只能有一位持者（两度实证的事故都是「两个 worker 都以为自己唯一在跑」：一次两边都看到 load 降就一起跑、一次一个故意压满 CPU 而另一个在跑门禁；跑完再登记只是把事故记下来）。
  - **占用时顺延**：被**别人**持着 → **不派**、方向退回种子队列、回显三件事缺一不可（**被谁占着 / 何时到期 / 下息再试**——少一件模型就会原地等或换人硬派）。同一持有者（同会话）再申请 = **续期**（`start` 保留、`expireAt` 前推），不挡自己、不并列两条。
  - **资源名归一**（`resourceKey` = trim + 小写）：`"  CPU-Exclusive "` 与 `"cpu-exclusive"` 是同一资源——**否则一个空格就能绕开整条租约**（已用真 dispatch 往返锁死：第一单用带空格大写的名字、第二单用规范名必须被挡）。归一后的名字才是落盘形态，回显与租约表里看到的是 `cpu-exclusive`，不是工单原文。
  - **三条释放路径 + 三条兜底回收**：`collect` / `retire` 自动释放（含**幂等重放**那条：已收过但租约漏释放的历史现场也在此收回）；`state` 读态做超期回收，判据全部基于**真值**——① 持有者会话**已不存在**（外部删除/服务重启）② 持有者**已空闲**（池登记标 idle 当即可信；或会话可达但不在跑**且已过启动宽限** `LEASE_START_GRACE_MS`=3min）③ **已到期**（`expireAt ≤ now`）。不回收就是**永久堵院**，而堵住的恰好是「要独占才敢做」的那类活。回收必须**显式报出**（哪条、原持者、原因）——不报的代价是双向的：报「空着」而其实已回收 → 下息白等一个不存在的持者；不报「回收」而其实释放了 → 以为资源已被自己占着、继续派负载敏感的活。
  - **启动宽限为什么必须有**（`LEASE_START_GRACE_MS`）：`dispatch` 返回后引擎把会话拉起来**有延迟**，而 dispatch 末尾与下一息开头都会立刻读态。若「未运行即回收」，**自己刚领的租约会在同一息内被自己撕掉**，紧接着的第二单就进来了（又两套测量互污染）。宽限只管「空闲」一档：会话已不存在、已到期两档不受它约束。
  - **到期是兜底、不是主路径**：`expireAt = 派发时刻 + 租期`（env `SHENGSHENG_LEASE_MINUTES`，缺省 `DEFAULT_LEASE_MINUTES`=90，下限 1 / 上界 24h）。它防的是「持有者悄悄死掉而登记没被碰」（服务重启、会话被外部删掉），**不防**「活着的 worker 超时」——那种情形由 `collect`/`retire` 与另两条真值判据兜。**已知边界**：到期只在 `state` 读态被消费，**直接 `dispatch` 不看 `expireAt`**（实测：盘上租约已过期、不经 state 直接派第二单 → 仍被挡，回显写「（已到期）…下息再试」）。这是取舍而非缺陷——`dispatch` 不擅自回收，免得把「刚过期但正干着活」的单砍掉；代价是**模型必须每息先读态**，否则一条过期租约能一直挡到下次读态为止（见 `shengsheng.md` 第十五节）。
  - **租约表的读侧归一**：`dedupeLeases` 与写侧 upsert 共用同一套判据（同资源只留一条，胜者取到期最晚者）——手改、旧版并行写、「一边回收一边写回」都可能留下「同一资源两个持者」，不归一的话一个持者释放后另一条死租约还在，**永久堵院**。无资源名的残行一律保留（宁可多留一条登记，不可错杀一个真持者）。
  - **验收口径（本节的证据强度，别只信文字）**：三条反向变异各自被断言杀死——① 冲突判断退回恒 false → 4 条红（同一批双单顺延、A 持租约期间 B 不得开跑、资源名归一、同息并发）；② 回收判据退回恒不回收 → 4 条红（超期回收三档、`SHENGSHENG_LEASE_MINUTES`、宽限两面）；③ 去掉启动宽限 → 2 条红。两条边界也各有对应用例：**持有者会话仍在跑时不得被 idle 判据误回收**（反面：会话一停、过了启动宽限就该回收）见「启动宽限的另一面」；**到期只在 `state` 读态被消费**（盘上租约已过期、不经 state 直接 dispatch 仍被挡）见「到期只在 `state` 读态被消费」。

- **为什么要控制面/数据面分离**：旧形态把「决策 + 干活」塞进同一个驻留会话，三个病灶都会要命——① 单次执行有 timeout，一旦开始重活就滑进超时死亡螺旋；② 驻留会话上下文越滚越肥，每息都要为历史付入场费；③ 干完一轮后自己在原地空转。分离后：控制面永不被长任务拖住（布完雨就收息），worker 是独立事件循环（不受控制面 timeout 约束，死在半路也只丢一跳），上下文各自独立（加不加 worker 只影响并发，不影响控制面体量）。

- **工具集**（四工具，**按一息相位分件**，互不重叠）：
  - `state`：无参=复命读态——项目坐标、呼吸计数、worker 池、在途、种子队列，并**一次摊出全部 worker 的实时态**（运行中/空闲、起始时间、待决交互、工具调用数、消息数、上下文水位），以及**三类截然不同的回收线索**（判据一律「先探会话」——会话是实体、别名只是影子，逐个会话实测，不拿登记猜真值）：待收藏线索（在途、**会话可达且已空闲**——可 `collect`）、**中道而废**（在途且**会话实测不存在**——`review` 取不到消息尾、无交付物可入账，应 `retire mode=idle` 退回种子队列）与**无从核实**（在途无会话线索：旧格式无会话列且别名不属本会话——**既不可当可达、也不可当已死**，单列一栏请先核实再处置；三类混同会把死会话当成可收藏产能喂给模型，也会把活会话当成死单）。池中会话**全部实测不存在**时另给「虚假产能」提示（复用价值已随会话消失，布雨须 `fresh=true`）。其余动作：`init`（固化项目坐标并写入会话 env）、`seed_add`/`seed_done`（择种入队与销账，重复入队拒绝、匹配不唯一拒绝）、`note`（落断点并推进呼吸，每息收尾）、`task op=ensure|pause|resume|status`（心跳驱动任务管理）
  - `dispatch`：一批工单（1~8 单）——同领域 idle worker 直接复用（摸底归零、记忆延续）、`fresh=true`/异领域新建（名 `evolve-w-<别名>`，注入项目坐标 env），按模板装配**自包含派工消息**（任务/上下文/交付/验证命令/纪律/预载/单任务即止与报告四要素——由代码保证不漏项），注入后立即返回**不等 worker**，自动登记池与在途；活动 worker（运行中+空闲）上限 3（**只挡新建**；上限可由会话 env `SHENGSHENG_MAX_WORKERS` 放宽——形如 `6` / `默认|硬顶`。超出部分**本批顺延**而非整批拒绝：可复用与续派的单照派，新建的单如实回给模型下息补，一息不白费）；**续派**——`orders[].session_id` 指向池中既有 worker 会话（只认本演进线派生的 worker，授权面不外扩），沿用同一无人值守授权、重注入项目坐标 env、消息点名「从中断处继续」；续派不新建会话、**不占新建额度**；`SHENGSHENG_DISPATCH=preview` 只出计划不派工
  - `collect`：`review`（深观单个 worker + 逐条处置建议：自化/超时停+抢救/入收藏/半截退役，**只读**）；`collect`（报告要点追加台账、worker 标 idle 留池、在途清除、报告「建议下一条」自动入种子队列——**记录键幂等**，同一 worker 同一轮只入一次账）；`retire mode=idle|stop|delete`（清池并把在途方向退回种子队列 / 终止超时运行 / 删除会话；运行中删除被拒）。**寻址**：`alias` 或 `session_id` 任一即可（同传须同指，矛盾则 `collect` 拒绝不猜）；`session_id` 命中该会话**当前轮**并在返回回工命中的别名与轮次；别名只在在途、池中无对应 worker 的陈旧残留用 `alias=` 定向清理

    **`mode=stop` 的完整语义 = 停会话 + 回收它派生的子孙进程树 + 检查工作树是否停在回退态**（`proc-tree.ts`）。前三者过去只做了第一件：`engine.cancel` 只打断会话的任务循环，而 worker 经 `sh`/`py`/`js` 起的子进程不是会话的一部分——会话死掉后它们照旧在跑、照旧在写同一棵树（两条演进线各自实证到：一个被终止的 worker 起的 `kill_drill` 继续握着源码的回退态）——
    - **归因靠环境标记** `SHENGSHENG_WORKER_SESSION=<会话 id>`（派工时写入 worker 会话 env，子进程继承）+ **子孙递归**（`env -i` 洗掉标记的仍算），**不按命令行/名字猜**：歌白里**没有「会话进程」**（所有会话跑在同一个服务进程内），按名字猜就是「把全机同名的都杀了」。标记之上再叠三条硬边界：保护集（自身 + 自身祖先链 + 1 号进程）永不发信号；标记属**别的会话**或落在其子树里的一律不碰（记 `skipped` 并点名理由，可审计）；**pid 复用防护**——补杀前重读 `/proc/<pid>/stat` 的启动时刻，变了就当它已死（绝不把 SIGKILL 发给复用了该 pid 的无关进程）。
    - **先 SIGTERM、等、仍在者才 SIGKILL**：给它一个跑收尾（写回文件、删临时文件）的窗口；一上来就 KILL 会把正在写文件的进程砍在半截写入上。结果如实报（回收了几个 / 还剩哪些需人工）。
    - **工作树可疑态只告警、不自动恢复**：树里 `*.revbak` 与源文件内容不一致 ⇒ 报「源码可能停在回退态」+ 给`diff` 核对命令（残留备份即未完成的回退，见 `reverse_verify.py` 头部）；自动恢复会盖掉别人**正在验**的那份回退态。
    - **无法定位必须如实报**：拿不到进程表（非 Linux / 无 `/proc` / 权限）或归因不到时，回显写「**无法定位**进程树」，**绝不静默当成「已清理」**——那会让下一息以为院子干净了，而破坏还在继续。`dry_run=true` 只出清单（不终止会话、不发信号、不改 `control.md`），干跑与真杀走**同一份** `planReap` 计划（两份就会「干跑看到的清单不是真杀的清单」）。
  - `divine`：问卦——**择的探索哨兵**（迷茫时起一卦：环境数据入卦、卦象入台账、卦指即落向；详下「生生：问卦」节）。不派 worker、不执行命令、不写 `control.md`
- **状态落盘**：`<项目根>/.evolve/control.md`（调度状态：坐标/呼吸/池/在途/种子/收藏待办/断点）与 `<项目根>/.evolve/ledger.md`（演进台账）由工具**结构化读写**——节结构确定性重写、幂等由记录键保证、池与在途**按会话 id 归一**由代码完成；节点允许人工查看，但不由模型手写 markdown（手写态是幂等与字段完整性的最大失效源）。
- **节结构单源**：control.md 的**规范节标题**（`SECTION_HEADINGS`）是写出侧与解析侧的同一条语料——`renderControl` 经 `H(title)` 写出（标题拼错或漏登一个节即**编译期报错**），`sectionText` 的节边界只认这份清单（**而不是任何 `^##\s` 行**）。后者是必须的：断点是**自由文本节**（正文由模型写、可能是多段 markdown），若把任何 `## ` 行当边界，正文里一行手写小标题（`## 下一步`、粘贴进来的 md 片段）就会被当成节边界，断点被**静默截断**——读进来只剩前半段，下次 `note` 写回时后半段**永久丢失**（与 `fb53b33e` 同一取向：手写文件里多出的非规范标题归到所在节正文，**不静默丢**）。
- **呼吸计数**：`note`（每息收尾）是**唯一**推进点，`state.breath += 1` 与断点同一次落盘——它是「这条河跑了多久」的唯一计数，也是断点叙述之外唯一能自查的机器可读量（读态与 `note` 返回都报它）。
- **项目根解析次序**：显式 `root` 参数 > 会话 env（`init` 固化 `SHENGSHENG_ROOT`）> **本会话 init 记忆**（会话 env 由引擎在 run 开始时解析一次，`init` 内写入的 env 对同一次运行的后续调用不可见，故另有进程内记忆兜底——否则「init 后紧接布雨」会误报未解析）> 工作目录向上探测既有 `.evolve/control.md`。两条硬约束：① **候选一律核实**——参数/env/记忆都须核到既有 `control.md` 才采纳，核不中即回落下一级，故根不存在或 `control.md` 不存在时不会停在陈旧候选上直接判「初生」（那会让写分支与布雨/收藏无基线可依）；② **记忆不外泄**——进程内记忆以**会话工作目录**为键（会话身份投影：同一目录的新会话沿用同目录既存记忆）且仅在 `init` 时写入，任何其他根（参数/env）的解析结果都不落记忆，故不会跨会话把 A 的根当成 B 的基线。未初始化时工具返回区分两种情形：根已解析但无 `control.md`（给 `action=init` 与具体 root）与本会话无从解析（列全四来源）。
- **提示词设计（玄学化的有效边界）**：`shengsheng.md` 以「你是造化，不是执行者」的世界观写就，而非流程手册。这不是修辞偏好——**实测对照**（同一探针仓、同代码、同工具面，n=4 vs n=3）表明：把「观复查表」（running 超 40 分钟→stop；末条完整→入收藏）换成「登记会撒谎，去问这条登记在世上还剩多少是真的」，**模型发现的「登记 vs 真值」裂缝数从均值 4.0 升到 6.7**，且稳定多出两类基线从未触及的裂缝——**时序自相矛盾**（记录的交付早于派发）与**脱节清单**（种子库文档的条目从未进过队列）。机理三条：① 查表隐含「只须看表内」，提问式指令则不指定看什么、只给一个要回答的问题；② 判据升格为价值（「没有证据的成果是幻影的种子」比「同一 worker 只收藏一次」更能驱动批判）；③ 明示授权「可以不做事」（「流产比假产好」），避免为凑产出而空转。**但玄学化不适用于事实呈现**：工具返回的文案必须保持冷冰冰的事实（死会话标「中道而废」、虚假产能警告），模型的洞察正是建立在事实未被污染之上；且不得为散文牺牲事实密度——本轮实测中纯玄学版丢掉了「心跳任务未在册」「坐标漂移」两项发现，合并进事实锛点后才补回。
- **审批**：`dispatch` 需审批（会派生会改仓库的会话）；`state` 按分支动态门控——仅 `task op=ensure|pause|resume`（创建/停用驱动任务）需审批，读态/择种/断点与 `task op=status` 免审（无人值守下每息都要读态与落盘，逐次审批会让心跳无法运转）；`collect` 免审（回收动作不改变项目状态）；`divine` 以**工具级** `requiresApproval: false` 免审（只读 + 台账追加，与 `collect` 同级；不进 def.requiresApproval 映射——那两键是既有契约面）。
- **无人值守语义**：被定时任务唤醒时不向用户提问；需审批工具由任务的执行授权自动通过；异常与暂停经任务通知通道送出（`pause` 内置通知：心跳任务未配通道时报明原因）。
- **派生与管理（控制面 / 数据面）**：
  - **控制面（生生）** 由心跳任务的 `resolveSession` 建立——新建会话时**显式写 `loadedSubAgents = ["shengsheng", 可选 "task"]`**（tasks.ts），故生生拿到自己的完整提示词与四工具；任务型执行以 `autoApprove` 运行（部署层授权）。**已在册的心跳任务其 `prompt` 不随代码升级刷新**（`op=ensure` 的 patch 面只有 `enabled`/`schedule`）——故一息流程指引（含「迷茫时问卦」）以 `shengsheng.md` 为准（系统提示词随装载自动生效），任务 prompt 里的同一句只是冗余提示。
  - **worker（数据面）** 由 `dispatch` 经 `SessionControlService` 建立：`store.createSession` + `setEnv`（项目坐标）+ `run(sessionId, user, message, { autoApprove: true, interactionMode: "none" })`，注入后立即返回不等完成。**派工即授权**——`dispatch` 的审批就是该 worker 全部动作的授权（均属任务型无人值守同一授权面）；不这么做的话 worker 走默认实时审批，无人应答时会挂在「待决交互」直到超时，每个重活 worker 都中道而废（已实测并修复）。派工消息内亦重述无人值守约束（与 task 型 hint 同文案）。
  - **抢救/续跑一律走 `dispatch`（`orders[].session_id`），不得用 `session send`**：`send` 只注入一句话、不带该次派工的执行姿态，worker 会按默认实时审批逐个等待——无人应答时每个需审批工具各挂约 5 分钟审批超时后被**跳过**，步骤静悄悄丢失（实测：一次 js 取证被跳过）。续派必须经 `shengsheng_dispatch` 指定 `session_id`，授权沿调用链随行；三条路（新建/复用/续派）授权一致，授权面不外扩（非池会话拒绝）。**回归固定**：同进程集成测试「续派全链」（`shengsheng.test.ts`）一次锁住——不新建/不占新建额度、resume 消息分支、非池会话 `blocked` 且零副作用、池满只挡新建、续派的池登记后果（idle 条目标回 `running` 且不新增条目）；拒绝文案由「空池变体」测试逐字钉死；边界分支（`session_id`×`fresh` 冲突、`preview` 下的续派标签与授权前置）由「续派边界」测试固定。七个反向变异（改拒绝文案、改清单格式、不标回 running、丢首派时间、别名漂移、让 `fresh` 胜出、放行非池会话）各自被对应断言杀死，非只靠文案匹配。
  - **worker 不继承生生的提示词**：新建会话不设 `loadedSubAgents`，也不预载任何子Agent（全局预载名单 `GEBAI_PRELOAD_SUB_AGENTS` 为空时）——worker 拿到的是全局工具 + 自包含派工消息 + 项目坐标 env。已验证：worker 会话 `loadedSubAgents` 为 `undefined`、消息流无 `loadedAgent` 系统消息。例外是**全局「可选子Agent」目录**对所有会话可见（worker 能看到 shengsheng 的描述行并可用 `agent_load` 主动装载），但不注入人格（worker 自述「only as part of the sub-agent catalog description, not as an active role」）。
  - **管理回路**：池/在途登记在 `.evolve/control.md`（池：别名/会话 id/领域/任务/派发时间/**轮次**/状态；在途：别名/**会话 id**/方向/预期产出/派发时间/轮次），`state` 每息摊出；`collect` 负责收藏（入台账 + 标 idle 留池）与退役（`idle`/`stop`/`delete`）；复用判据为同域 idle，活动 worker 上限 3。以下易错点已有代码与测试固定：
    - **复用不改名**：复用匹配先于别名派生——命中的 worker 沿用**它的既有别名**（否则同一会话会被改名，旧别名立即失效、台账与派工消息也无法回溯同一工人）。
    - **登记主键 = 会话 id**（同一会话只保留最新一轮）：别名是「工人」的显示名、会话是本体——同一会话续派/复用时**取代**旧轮登记（池与在途都按会话 upsert），不并列、不标脏。**写侧与读侧双管齐下**：读侧 `parseControl` 也做一次归一（旧版工具写下的并列条目、人工误改都算），否则裂缝会一直在盘上活着——按别名寻址撞上旧条目、按 `session_id` 命中当前条目却清不掉另一条（本机实测两次）。归一取 round 最大者（并列取靠后者），保持原相对次序故渲染确定、往返幂等；无会话 id 的行**不参与**判定（宁可多留一条登记，不错杀一条真实在途）。在途表带**会话 id 列**（旧格式 ≤5 列自动识别、经池解析别名回落）——不带着会话列，「在途」就无从「先探会话再判生死」。
    - **回收线索三类，判据一律「先探会话」（会话 id 为主键、别名只是影子）**：`awaiting` 待收藏（在途且**会话实测可达且空闲**，不另加「别名仍须在池中」——可达会话就是真在途，多一道影子条件会把真在途误判成中道而废）；`gone` 中道而废（在途且**会话实测不存在**——退回种子队列）；**`unresolved` 无从核实**（在途无会话线索：旧格式无会话列且别名不属本会话——归入哪一边都是假信号，单列一栏请先核实再处置）。池内会话**全部实测不存在**才报「虚假产能」（未核实不得诬告；旧判据「别名不在池中 ⇒ 中道而废」把仍在运行的会话误判为死单，是实测两次的假阳性）。
    - **寻址语义显式化（`collect review`/`retire`）**：`alias` 与 `session_id` 可各传其一，也可同传（同指一条）；`session_id` 命中该会话的**当前轮**条目并在返回里**回工**命中的别名与轮次；别名只在在途、池中无对应 worker 的**陈旧残留**用 `alias=` 定向清理（`session_id` 寻不到残留）；两者指向不同 worker 时读动作提示、写动作（`collect`）**拒绝不猜**（猜错会拿别的会话的报告入账）。
    - **幂等键按轮次**：台账键为 `会话:r<轮次>`，轮次由 `state.seq`（单调递增的派工序号，逐单递增、只有真派出的单占号）来源。不用时间戳：复用场景下 worker 与 inflight 的时间戳不同源，键会漂移（既挡不住真重复也可能误伤）。
    - **台账读改写串行**：`appendLedger` 在本进程内按路径串行（`withLedgerLock`），且用**文件本身**做幂等源（每次读到最新内容）——避免「先判定未入账、后双写」。台账是控制平面唯一的成果记忆，宁可慢也不能写坏。
    - **复用代数提示**：同一会话承接单数达 `REUSE_GENERATION_HINT`（8）时在派工回显提示「宜收藏后退役换新」（上下文渐胖，继续复用质量下降）。
    - **续派四契约（`orders[].session_id`）**：① **续派 ≠ 新建**——不调 `store.createSession`（会话数不变）、`plan.created=false / plan.resume=true`，且**不占新建额度**（池容量判据只数 `created` 的单：池已 2/3 时一批「续派 + 新建」必须放行——若把续派算作新建则为 2+2>3，整批被「上限 3」挡下）；② **消息带 resume 分支**——`renderOrderPrompt(…, { resume })` 在「任务：」之后插入「续派：…从中断处继续…不要从头重做」，否则 worker 会另起一单从头重做或绕开被中断的步骤；同一模板的**新建**单不含此行（分支开合而非恒插）；③ **授权面不外扩**——`session_id` 只认**池中登记**的会话，非池会话（含**确实存在**的会话，如生生自己的控制会话）一律 `blocked` 返回且**零副作用**（不注入、不新建、`control.md` 原封不动）；判定在**计划构造段**，故早于容量检查、也早于 `preview` 返回——预检不得把越权请求摊成一份「看起来能跑」的计划；**拒绝文案是契约**（模板与 `别名=会话id`「、」连接的清单格式已由测试逐字钉死，空池退化为「（空）」为同契约第二步分支），因为它是人机共同的可检索落点（「授权面不外扩」这句必须能被搜到）；④ **池登记后果**——续派打到**已存在但 idle** 的条目时把该条 **`status` 标回 `running`**（而不仅是复用会话），并**取代**旧轮而非新增条目：别名/领域/首派时间沿用、`round` 前推、同一会话在池与在途各恰一条。四条路（新建/复用/续派）授权一致、执行姿态一致（`autoApprove + interactionMode:none`），项目坐标 env 对复用/续派**重注入**（会话 env 内存态，续跑多发生在重启丢 env 之后）。**冲突输入语义**：`session_id` 与 `fresh` 同传时**显式寻址胜出**（`fresh` 是自动匹配阶段的偏好提示，点名了具体会话即不适用）——若让 `fresh` 胜出而另开新会话，用户点名要的 worker 反被晾着，且凭空多占一格额度。
- **环境变量**（`SHENGSHENG_` 前缀，见 def.envVars）：`SHENGSHENG_ROOT`（项目根，`init` 写入会话 env / 派工写入 worker 会话 env）、`SHENGSHENG_TASK_NAME`（心跳任务名，缺省 `SHENGSHENG_EVOLVE`）、`SHENGSHENG_VERIFY`/`SHENGSHENG_SEED_DOC`/`SHENGSHENG_RULES`（验证命令/种子库/硬约束文档的项目坐标，参与派工消息模板）、`SHENGSHENG_DISPATCH`（`preview` 只预检不派工）、`SHENGSHENG_MAX_WORKERS`（活动 worker 上限，缺省 3；`6` 或 `默认|硬顶` 如 `3|16`——放宽并发用）、`SHENGSHENG_MODEL_CTX_WINDOW`（**真实模型上下文窗口**，缺省 `DEFAULT_MODEL_CONTEXT_WINDOW`=512000；引擎 `contextWindow()` 自报优先，`0`/`off` = 未知且回显明说未知）、`SHENGSHENG_REUSE_ADVISORY_CTX`（**复用质量建议线**，缺省 `DEFAULT_REUSE_ADVISORY_LIMIT`=150000；`0`/`off` = 关掉该判据）。

#### 生生：上下文水位的两个量（“一个数字两个量”第三例）

**问题形态（真误报，本仓实测）**：上下文水位只有一个 `WORKER_CTX_LIMIT`=150000，它既是「肥到不值得复用」的**建议线**，又在 `review`/`collect` 回显里被写成**「超上限」**。于是 ctx 224K 的 worker 被判「上下文已过肥（149%），超上限」——而真实模型窗口是 512K，224K 只占 **44%**，**它没有超任何硬限**。危害不在文案：这个错理由直接生出错的退役决策，并被写进台账，下一息的人会照错理由做决策。

拆成两个各自命名的量，**各用各的分母、各给各的处置**：

| 量 | 缺省 | env | 过线语义 |
|---|---|---|---|
| `modelContextWindow` | 512000 | `SHENGSHENG_MODEL_CTX_WINDOW`（引擎自报优先；`0`/`off`=未知） | 过 ⇒ **必须退役**（再续派会截断/报错） |
| `reuseAdvisoryLimit` | 150000 | `SHENGSHENG_REUSE_ADVISORY_CTX`（`0`/`off`=关掉此判据） | 过 ⇒ **可复用，但收益递减**（建议换新，不强制）；80%↑ ⇒ 本轮收完宜换新 |

回显口径（`review` / `collect` / `state` 一律两个量都报）：`ctx 224.0K｜复用建议线 150.0K（149%）｜模型窗口 512.0K（44%）`；`state` 读态头部另有 `上下文口径` 一行说明两量与数源（引擎自报 / env / 缺省）；两量**未知时明说未知**（不编造、不拿另一个量顶上）。实现面：`ctxScale()`（解析两量 + 数源）、`ctxVerdict()`（两量 + 四档判据 + 分档处置）、`ctxLine()`/`ctxScaleLine()`，工具 `data.ctx` 同步给出结构化字段（`level`/`advisoryPct`/`windowPct`），调用方不必解析文案。

**同型前三例**：`round`/`seq`（一个数字既当全局派工序号又当会话轮次）、`ctxRatio` 退回裸数字（分子标了、分母没说是什么量）。通则见 `shengsheng.md` 第十一节：**凡是“上限/比例”类数字，必须同时说清分子与分母各自是什么量**。反向验证（已固化在 `reverse_verify.py`）：把两量合并回一个（`window := advisory`）⇒ 3 条新用例变红；把「过建议线」判成 `over-window` ⇒ 2 条变红；两轮均逐字节复原源码。
#### 生生：`pgrep -f` 自匹配活锁（等待纪律 + `review` 的自动检测）

**问题形态（实测两次，代价可量化）**：`pgrep -f <pattern>` 匹配的是**整条命令行**。等待循环若把 pattern 原样写在自己的命令里（`sh -c "for i in …; do pgrep -f 'x.py' …; done"`），`pgrep` 每次都命中**发起它的那条 shell 自己** ⇒ 条件恒真 ⇒ 「等它结束」变成**永远等下去**。

| 现场 | 表现 | 代价 |
|---|---|---|
| 会话 `a8319257`（别名 `ctx-units-1`） | 反复执行 `pgrep -f "reverse_verify.py"` 等门；7 次等待全部判「还在跑」（当时真实验证进程 **0** 个） | **11:34:51 → 11:46:19（12 分钟）空转 / 159 条消息 / 106K ctx / 一行实现都没写** |
| 生生自己的命令行 | `pgrep -c -f 'python3.*reverse_verify'` 同样命中自己；改成 `'python3.*reverse_verify[.]py'` 后正确返回 0 | 一次误判（当场识别） |

**为何必须自动判、不能只写纪律**：两次都是「worker 以为自己在守规矩地等别人」，自律完全不管用；而「它反复等同一个 gate、那 gate 现在没有进程」这件事**只有外部观测看得见**——自省看不见自己的命令行。

**纪律（`shengsheng.md` §六「等待纪律」+ 派工模板逐单注入，同源 `waitProtocolLine()`）**：禁止 `pgrep -f` 去匹配一个写在自己命令里的串；三条正确写法按优先级：① 有 pid 就别用 pgrep（`while kill -0 <pid> 2>/dev/null`）；② `pgrep -x <进程名>`（不扫命令行）；③ 仍要 `-f` 时破字串 + 排除自身与父（`pgrep -f 'x[.]py' | grep -vx -e $$ -e $PPID`）。任何等待循环必须有上限（否则判据错一次就是无限期空转）。

**自动检测（`collect action=review`，实现 `pgrep-lock.ts`）**：从该 worker 会话最近 60 条消息里筛出**真的被执行过的**等待类命令（只取 `sh`/`py`/`js` 这类会真跑的调用参数；只看循环形态 `for i in`/`while`/`until`——单次 `pgrep x && echo` 不构成活锁），再两路判：

1. **自匹配**（纯文本，单条即报）：把 pattern 当**正则**（`pgrep` 的语义）去匹配该命令自己的命令行。这一路的正确性有一条可证的自洽面：`x.py` 匹配上自己（恒真），`x[.]py` 匹配不上（命令行上是字面 `[.]`，正则要的是 `.`）——于是**「破字串为何有效」与「判据为何不假报」是同一个判据**。
2. **活锁惯性**（进程表，阈值 3 次）：同一 pattern 在最近被连续等待 ≥3 次，而**此刻进程表里没有任何进程匹配它**。

进程表这一路用**字符类断开自身**（`x.py` → `x.p[y]`；正则语义不变而命令行上的字面不再精确重合）+ 排除自身与祖先链——否则本判据自己就会复刻同一个坑（`verify_guard.py` 已记着这一型的另一面：`pgrep -af reverse_verify` 不排除自己就「永远有人在跑」）。

**三种结论严格分开**（PITFALLS：0 命中 ≠ 读不到）：无等待类命令 ⇒ 不出这一行；有等待类命令但拿不到进程表 ⇒ 自匹配仍可判（纯文本）、明说「活锁惯性这一路本项未核」；核过无异常 ⇒ 给一句已核过的肯定。

**回归**：`shengsheng.test.ts` 新增 7 条（纯函数判据 2 条 / 活锁惯性单独可报 1 条 / 端到端 1 条 / 反向不误报 1 条 / 进程表不可读 1 条 / 派工消息带纪律 1 条）；`reverse_verify.py` 新增 3 条回退用例（自匹配判据退回恒假 / `staleGates` 退回永不产生 / 派工消息退回不带纪律），逐条实测变红后逐字节复原。


#### 生生：登记身份键与跨模块全场景回归（池/在途 × 续派 × 回收）

**问题形态（本单实测的真实缺陷）**：池与在途的「同一会话至多一条」在**写侧与回收侧用的主键不同**——写侧按会话 id `upsert`，`collect`/`retire` 却按**别名**从在途里找工作。别名是**显示名**、会漂移（旧版 `ensureUniqueAlias` 不认 `revived`，续派同一会话时会给它派一个新别名；人工误改与旧版工具残行同理），于是同一会话在途可以同时存在两条**不同别名**的登记：写侧过滤不掉旧名那条（谓词是 `x.alias !== plan.alias`），读侧归一也认不出它们同源（旧版归一按别名键），`collect`/`retire` 更是按 worker 别名只删得掉一条。后果三条：① 旧名那条因 `round` 更大而在归一里**胜出**，反把本轮新登记顶掉；② 收藏后残留被读态重新报成「待收藏」，**收藏过的东西又冒出来**；③ 退役后池已空、在途仍挂着一条——控制面永远收不回它。

**修复：身份键单一化——会话 id 优先，别名只在「无从核会话」时兜底**

- 新增 `inflightIdentity(i, workers) = inflightSession(i, workers) ?? i.alias`：会话判定**直接复用 `inflightSession`**（自带会话列 → 经池解析别名），使**读态「这条在途是谁的」与写侧「该清哪条」走同一判据**——各自实现一分就会出现「读态认得出、收藏清不掉」这类裂缝。
- 三个共用取数口：`inflightOf(state, worker)`（该工人的全部在途登记）、`takeInflight(state, worker)`（取走并就地移除）、`returnToSeeds(state, rows, prefix, reason, constraint)`（方向退回种子队列，按方向去重，并**回工是否真推入**——调用方据此选「已退回种子队列 / 已在种子队列（未重复）」文案）。写侧 upsert（`dispatch`）、在途归一（`parseControl`/`dedupeState`）、在途清除（`collect`）、在途取出（`retire` 的 `idle`/`stop`/`delete` 三支）全部改走它们。
- **别名优先在寻址处也是错的**：`alias`/`session_id` 同传时命中改为 `inflightBySession ?? inflightByAlias`。别名不是身份——它可能属于**别的会话**的在途残留，按别名优先会清掉别人的那条、留下自己那条（越权且没清干净）。
- **旧格式在途**（旧版工具写的表没有「会话」列）现在能被正确归到会话名下（旧行靠池解析别名换回会话 id）：否则同会话在新旧两代格式的行会因键不同（会话 id vs 别名）而**并列两条**——正是本单要堵的裂缝。
- **幂等键取值收敛为 `max(在途轮次, 池轮次)`**（原为 `在途 ?? 池`）：在途清掉后再收、或旧版残留轮次低于池时，回落到池轮次会造出一个**更小的新键**，同一轮于是被当成新轮重复入账——幂等键一旦回退就不再幂等。

**跨模块全场景回归（`shengsheng.test.ts` 新增 1 条）**：把「续派路径」与「池去重」两个测试视角合并成一条同进程测试，因为分开写时两个机制各自都能被**局部**满足（池按会话 upsert 了、在途按轮次归了一了），裂缝只卡在夹缝里——**「同一会话两轮派工」是唯一能同时打到写侧、读侧与回收侧的场景**。场景链：真实派第一轮 → 盘上铺旧版写到一半的漂移残留（池 `a-9@3` / 在途 `a-1@9`，在途轮次**高于**本次续派轮次，对修复最不利）→ 第二轮续派（不新建、消息带 resume 分支、标回 running、池/在途各只一条且别名与轮次同源）→ 读态（无「中道而废」假阳性 / 无「虚假产能」/ 待收藏恰一条）→ 按 `session_id` 收藏（在途清空）→ 按 `session_id` 退役（池/在途无任何痕迹）→ 退役后越权续派仍被拒且零副作用。另补齐几格**无人续派**的现场（生生开一息直接回收）：漂移残留直接走收藏与 `mode=stop` 退役、旧格式残留、新旧格式并存、别名与会话 id 指向不同在途时的取舍。授权面未放宽（最后一段仍钉「授权面不外扩」与空池文案）。

**变异验证（6/6 杀，1 项有意存活）**：逐个回退修复元素确认测试真能检住，而非「恰好绿」——① 写侧只按别名过滤 → 杀；② `collect` 在途清除只按别名 → 杀；③ `retire` 在途取出只按别名 → 杀；④ 身份键改为「别名优先」 → 杀；⑤ 身份键收敛为「只认自带会话列」（丢掉旧格式经池解析那一档） → 杀；⑥ 寻址改为「别名优先」 → 杀。**有意存活**：去掉「无会话线索时退化为别名」——该退化是保护性宽容（会话线索全缺时别名是唯一可用的归并依据，多留一条不错杀），不影响会话级一致性，故不入断言。

#### 生生：在途登记格式兼容（旧行迁移面）

身份键单一化让「在途自带会话列」成了主路，而**盘上的存量行没有这一列**——工具升级不迁移数据，旧行会一直躺到被收藏/退役（本仓 `.evolve/control.md` 的在途表就是这样）。存量旧行有两个形态会把解析带偏，两者都指向同一条假阳性：**把可达会话误判为「中道而废」**。

**形态一：旧行靠池解析别名换回会话。** 旧表（`别名 \| 方向 \| 预期产出 \| 派发时间 \| 轮次`，4~5 列）的会话线索只能在池里。若身份键只认自带列，同一条在途在新旧两代读法下会得到**不同的键**（会话 id vs 别名）→ 同一会话并列两条。

**形态二：旧行的「方向」是工单原文，常带 `|`。** 历代渲染器把正文的 `|` 写成 `\|`，而**旧版渲染器根本不转义**——历史内容就这样写在盘上。按裸 `|` 切分时，含一个竖线的字段就**多切一格**，5 列的旧行于是长得像 6 列的新版行：会话列被填成方向的碎片（**伪会话线索**）→ 读态拿它去探一个不存在的会话 → `gone`（中道而废）假阳性 → 可达的 worker 被贴上「收不回的死单」。同一条病还在**池表**：任务列（含工单原文）带竖线时，尾列被顶成正文→轮次/时间/状态读错。

**修复：转义感知的切分 ＋ 会话列按「证据」辨识（不再只看格数）**

- `cells()` 改为**转义感知**：`\|` 是正文里的字面竖线、不是分隔符。**只认 `\|` 一种转义、不认 `\\`**——历代渲染器只转义竖线，正文里的反斜杠是原样写下的；若一律吃掉反斜杠，`C:\Users` 会被蚀成 `C:Users`。而「只转义 `|`」与「只认 `\|`」恰好互为逆运算，任何字段（含反斜杠、含竖线）都能无损往返，历史内容也无须改写。
- `tableBlock(md, heading)` 取一段表时**连表头声明的列数一并取出**（`declared`）：表头是同一个渲染器写的，与行的代次同源，比行自身的格数稳——旧表 5 列、新表 6 列。
- 在途的**会话列按证据辨识**（不看整行格数），四档任一成立即认：
  ① **形状**——`^[0-9a-f]{32}$`（存储层白名单就是它；真实会话 id 只有两路：`randomUUID()` 去连字符、飞书桥接的 sha256 前 32 位）；
  ② **同源**——这一格**就是同文件池里某个 worker 的会话**（池是在途的登记源，两侧同源写入）；
  ③ **对位**——表头声明 6 列且本行恰 6 格（无溢格可言，裸竖线没有把方向顶上去的余地）；
  ④ **空值**——新版行的会话列可以为空（此时方向不会顶到第 2 格上）。
  其余（表头说旧版、格 2 既非 id 也非空、少格）一律按旧版读。
- **为什么保守一侧必须是旧版**：**旧版读法也是新版行缺列时的正确读法**（会话列丢了，方向就落到第 2 格，恰好对位）。而认错的代价**不对称**：把方向碎片当会话 → 拿伪线索探一个不存在的会话 → 可达会话被判「中道而废」（假阳性，且会误导控制面去退役它在跑的 worker）；不认（丢掉真 id）→ 回落到池按别名解析同一会话，**值一模一样**。
- **尾列取量后两位、正文列吃中间**：方向/预期产出（在途）、任务（池）是自由文本，溢格只可能落在它们身上；派发时间/轮次/状态总在行尾——行越长（被竖线撑开）越不能按固定下标取，否则取到的是正文（同一病灶的池表一侧）。

**诚实边界：一处不可能两头对的“两难格”**（已写进测试并钉死取舍）：同一份文本（新表头、6 格、第 2 格是自由文本）既可能是「旧行的方向被裸竖线切成两格」（实例：本仓现存 `control.md`），也可能是「手写/外部工具写的新格式行、会话 id 不是 32hex」。两种读法不可能同时正确，上表选**按旧版读**（代价小的一侧）。代价：后者会被丢回池解析会话（**在途值不变**，因为池里就是同一个会话）；影响仅限于「池里也没有它的极特殊夹缝」，而那种情况下它能被读态报为「无从核实」、并由 `retire alias=… mode=idle` 定向清理+方向回种子——**不丢方向、不误判、不崩溃**，满足本条的「不误判、不静默丢弃」。

**回归（`shengsheng.test.ts` 新增 2 条）**：①「旧格式在途按池解析认帐」——夹具形态先钉（池表任务列不被截断、尾列不被顶掉、夹具往返恒等、含裸竖线的旧行**确按裸分隔符会多出格**），再端到端走一息：读态把可达会话的报**待收藏**（**无**「中道而废」）、无线索的行报「无从核实」、四条一条不少；`review` 按 `alias` 与按 `session_id` 都能命中同一个 worker；再把池里那条的会话删掉——同一行**必须**被报为「中道而废」（有会话可探时认得出、无会话可探时按中道而废而非崩溃），`retire alias=… mode=idle` 清干净且方向回种子；无线索那条走同一路径也不丢方向。末段反向对照：**新版行**（自带会话列、池外）仍按新版读、会话可达时进「待收藏」而非「无从核实」。②「两难格」——九个判别格把四档证据各自钉死（形状档、同源档、对位档、旧表头档、溢格新行、尾列取量位置、新旧格式并存归一），并把「同类病也在池表任务列」单列一格。

**只读夹具（`fixtures/control-legacy-inflight.md`）**：把存量旧行的两个形态固定下来（旧表头＋无会话列、方向含裸竖线的工单原文、池表任务列含裸竖线、新旧两代并存的写法、以及一条真·无线索行）；夹具**只读**，用例只读它、在临时目录重建现场——**不碰任何用户既有的 `control.md`**。夹具顶部写明「这不是运行态状态文件」，随用例一起演进。

**变异验证（12/12 杀）**：逐个回退修复元素，每个变异体都被对应断言杀死——① `cells()` 退回朴素 `split("|")` → 杀；② 表头列数不采集（`declared` 恒 `undefined`，即丢掉「旧表 5 列」这一判据） → 杀；③ 会话列只看列数（回到 `e1e803ce` 的判据，即退回本单要修的 bug） → 杀；④ 旧表头判定去掉（`legacyHeader` 恒 false） → 杀；⑤ 会话列识别去掉「形状」档 → 杀；⑥ 去掉「同文件池」档 → 杀；⑦ 去掉「表头正对位」档 → 杀；⑧ 在途尾列退回固定下标 → 杀；⑨ 旧行不再把溢格并回预期产出 → 杀；⑩ 池任务列不再并回 → 杀；⑪ 新版行不再并回 → 杀；⑫ 池尾列退回固定下标 → 杀。

**本单改动面与验证**：`packages/agents/src/agents/shengsheng/`（**只此一处**）；回归测试 30 pass / 475 expect、`bun run typecheck` 全绿；未碰任何用户既有的 `control.md`（夹具只读）。

**落盘注记（2026-10-16）**：本节文本在写入时被同仓库的并发提交（`6283fa68`）意外带入其 DESIGN 改动统计（两单同时改同一文件，其 `git add` 扫到了本节的未暂存改动）——**代码与测试从未受影响**，本节内容与验证结果以本注记为准；不为此改写那段历史。

#### 生生：在途登记格式与版本化解析（`control-format.ts`）

`e1e803ce`（在途按会话 id 认帐）与 `fb53b33e`（存量旧行无会话列不误判、不静默丢弃）两次修复后，兼容逻辑**散落在解析各处**——判代靠 `declared === 6`、`c.length >= 6` 这类字面量，位置靠散落的 `c[i]`，升级靠各分支自己补字段。**再加一列就要逐处改，且各处的判据会各自漂移**（本仓已实测两次：身份键半途、旧行误判）。本单把「**格式版本 + 升级函数**」抽成显式小模块 `packages/agents/src/agents/shengsheng/control-format.ts`。

**两张登记表的版本序**（只有池与在途两表加过列；种子/待办列数从未变，不纳入版本化）：

| 表 | 代次 | 列 | 声明列数 | 自带会话列 | 引入 |
|----|------|----|---------|-----------|------|
| 在途 | v1 | 别名 \| 方向 \| 预期产出 \| 派发时间 | 4 | — | `c5b90529` 及之前 |
| 在途 | v2 | 别名 \| 方向 \| 预期产出 \| 派发时间 \| 轮次 | 5 | — | `74a2e6b9` |
| 在途 | **v3（现行）** | 别名 \| **会话** \| 方向 \| 预期产出 \| 派发时间 \| 轮次 | 6 | 自带 | `e1e803ce` 起 |
| 池 | v1 | 别名 \| 会话 \| 领域 \| 任务 \| 派发时间 \| 状态 | 6 | — | `c5b90529` 及之前 |
| 池 | **v2（现行）** | 别名 \| 会话 \| 领域 \| 任务 \| 派发时间 \| **轮次** \| 状态 | 7 | — | `74a2e6b9` 起 |

**解析流水线**（版本信息在每一跳都显式可见）：

```
cells(line)                          转义感知的格切分（`\|` 是正文里的字面竖线，见 fb53b33e）
tableBlock(md, heading)              取一段表：数据行 + 该表**表头声明的列数**（代次标志）
selectInflightVer(cells, ctx)        这一行是**哪一代**——按证据判，不看格数（四档证据，见下）
parseInflightRow(cells, ctx)         按该代的**位置声明**读取 → `{ ver, fields }`（ver 显式、可断言）
migrateInflight({ ver, fields })     升级为**现行形态**：该代没有的列在此补空、数字在此定型
fieldOrder(read, ncols)              声明 → **列序**（下表是写出侧与声明同源的枢轴）
writeRow(decl, getters, value)       按声明列序写一行（缺取值器/未逐列覆盖 → **当场报错**）
inflightHeader()/inflightCells()     现行形态 → 表头/格（与版本声明**同源**）
```

**「加一列只动声明」不是口号，是结构**：

- **读入侧表驱动**：判代（`selectInflightVer`）、位置读取（`readFields`）、升级（`migrateInflight`）**都不含版本号字面量**——版本次序、「装得下哪一代」、旧版上界、溢出边界全部由**声明表**（`INFLIGHT_VERSIONS` / `POOL_VERSIONS`）推出。每代的字段位置用 `FieldSpec` 声明：`at`（自左）/ `fromEnd`（自尾，1 = 行尾）/ `overflow`（自由文本列，吃溢出的碎格）。
- **写出侧也已表驱动**（`dfe18f67` 之后的加固）：`inflightCells` / `poolCells` 不再手写数组字面量，而是 
  「说清**怎么取值**」，**列序由 `fieldOrder` 从位置声明推出**（`writeRow` 入口）。原先表头从声明取、行次序另拄一份，两处一漂就是**整行错位**（列名与值对不上，且读回来是「合法的错值」）——静默数据损坏比报错危险得多，故 `writeRow` 另加一道**逐列覆盖校验**（字段数 = 列数、下标恰为 `0..n-1`，否则拒写）。
- **加一列 = 三处声明 + 一处取值器**：① 版本号类型（TS 联合类型，加得新版本）；② 声明表加一条（列名 + 位置）；③ 现行版本标记**不必动**（= **声明表末条**，自动跟上）；④ 若新列要落盘：域对象接口加字段（`{[K in keyof Required<T>]}` 的取值器表**会因此在 typecheck 阶段报缺**）+ 取值器表补一行。**判代 / 读取 / 升级 / 列序推导一行不改。**- **表头识别也不再按列名**：`tableBlock` 原先靠「首格是不是『别名』」认表头——**前置一列**（新列插在最前）就让首格不再是任何已知列名，于是表头认不出、表头行被当数据行读进来。现改为按**结构**认（**分隔行紧邻的上一条表格行即表头**），与列名/列序无关，天生随声明表演进。
- **这条主张有可执行证人**（用例「加一列只动声明」）：不靠文字声称“日后加列很容易”，而是真的在运行时给声明表加一族版本（尾部加「心跳」/ 前置加「序号」），用**原封不动的** `parseInflightRow` / `migrateInflight` / `writeRow` / `inflightHeader` 走完 **parse → migrate → render → 再解析**，断言往返等价、旧代行仍认得出；并把**声明表写乱**（v4 排在 v3 之前）再读一次——仍须判对（判代按列数降序试，不靠声明次序）。这同时把「有人把版本号/列名列表/列序写死回实现里」变成一条会红的测试。
- **判代仍是「证据优先于格数」**（`fb53b33e` 的判据原样保留）：自带会话列的一族按**形状**（`^[0-9a-f]{32}$`）/ **同源**（就是同文件池里某个 worker 的会话）/ **对位**（表头声明列数一致且本行恰那么多格）/ **空值** 四档任一成立即认；其余一律按**旧版**读（保守一侧），旧版一族再按本行格数取「装得下」的最高一代（≥ 5 格 → v2；4 格 → v1）。**代价不对称**的理由见 `fb53b33e` 一节，未变。
- **池表判代同样去字面量**：`selectPoolVer` 从「`declared === 6 ? 1 : 2`」改为「声明列数命中哪条声明就取哪个版本，都没命中则取**末条**」——加一列时它无需改动（命中不了就回落到最新一代，而不是硬编码撞出另一代）。
- **只做抽取与收敛，不改语义**：池表 v1 行的历史读法（两代共用同一组尾锚点，故 6 格 v1 的「状态」位落在第 6 格）原样保留，并用一条对照用例**钉死**——要修正须单独一单。**授权面未放宽**（本模块不碰 dispatch/collect 的任何判据）；**已落盘 control.md 的写入等价性未变**（列序推导对现行两代逐位等同原硬编码，且由等价性基线用例逐字节比对照看）。

**回归（`shengsheng.test.ts` 33 pass / 658 expect）**：

1. **版本化解析·等价性基线**——多版本语料（只读夹具 `fixtures/control-format-corpus/01/02` + 随语料入库的 `golden.json`，来历见同目录 `README.md`）上，把 `parseControl` / `renderControl` 的结果与**升级前实现捕获的黄金基线**做四维逐字节比对：`state`（解析结果，`toStrictEqual`——**字段存在性也是契约**）、`sessions`（每条在途归哪个会话——身份键不许漂）、`rendered`（**写入等价性**：渲染结果逐字节不变）、`roundTrip`（写入→重读恒等）；并断言旧代行**必带** `sessionId` 键（形状统一由升级函数负责）。这是「与现有行为完全等价」的硬证据，不是「测试恰好绿」。
2. **版本契约**——`{ ver, fields }` 可断言；四档证据各自的判代结果、v1/v2/v3 的边界格、**只 5 格的行不得升格为 v3**（旧行方向恰是一串 hash 的形态）、池 v1 的历史读法对照、声明表自身的契约（版本号递增、只加列不减列、列名不重复、位置声明的键都是已知字段）、判代是**纯函数**。
3. **加一列只动声明**（见下）。

**加列演练用例（真的走一遍往返）**：① 写出侧声明→列序与历代逐列对位；② 表头与声明同源（现行 = 末条）；③ **走真文档**（表头+分隔行+数据行→`tableBlock`→parse→migrate）建往返等价，含**新列空值/非空**两档与多字段边界，正文同时含裸竖线与反斜杠（转义感知往返）；④ **前置列**同样只动声明（表头仍认得出、表头行不得被当数据行——残留①的回归位）；⑤ 声明次序写乱仍判对（读入侧与声明次序无关）；⑥ 写出侧的**失败面**（漏取值器 / 声明未逐列覆盖 → 当场报错）；⑦ 池表加代同理（含中间插列时**尾锚点整体重锤**的写法、以及旧声明表回落现行版）；⑧ 真实现现行版仍是 v3/v2（演练只在旁路扩表，不改现行声明）。

**变异验证（13/14 杀，1 个为真等价变异体）**：逐个回退本次加固的元素，每个都被对应断言杀死——① 表头识别退回按列名认；② `inflightCells` 退回硬编码列序；③ `poolCells` 退回硬编码；④ `selectPoolVer` 退回版本号字面量；⑥ `fieldOrder` 不再按下标排序；⑦ 去掉逐列覆盖校验；⑧ 缺取值器不报错（静默写空）；⑨ `writeRow` 改用取值器表键序；⑩ 在途判代只试到 v3；⑪⑫⑬ 三个写出入口改看导出的 `*_CURRENT` 常量而不看**传入的声明表**（这证明写出侧真的表驱动）；⑭ 末条改取首条。**存活的 ⑤「`INFLIGHT_CURRENT` 硬编码 3」是真等价变异体**：它只影响导出常量（写出侧走的是「传入声明表的末条」），而今天真表末条恰好是 3，故它在当前源码上**行为完全一致**；要杀死它需让真表多加一代——而那是一个**向前兼容的行为变更**（会改变现行写出格式），不属本单（本单限「不改语义」）。`53ffccc1` 原先报的 **18/18 杀**是相对那次抽取的（判代/读取/升级/池历史读法），与本单的 ①–⑭ 不是同一组变异体，两组各自成立、不可互相代替。

**本单改动面与验证**：`packages/agents/src/agents/shengsheng/`（`control-format.ts` / `shengsheng.ts` 两处 `map` 回调 `(w) => poolCells(w)` 防 `Array.map` 把下标当第二参传入）与 `DESIGN.md`；33 pass / 0 fail、`bun run typecheck` 全绿、`packages/agents/` 全量 1646 pass（另 2 条 shuangtian「capture：自然路径」与 shuangtian 本目录 1 条失败——已在干净工作树上复现，与本单无关）；未碰任何用户既有的 `control.md`（夹具均只读）。

- **心跳任务形态**：`state action=task op=ensure` 幂等创建/启用（`kind=scheduled`、`runner=prompt`、`target=sticky`、`agents=["shengsheng"]`、任务级 env 带项目坐标、`notifyOn=model`、连续失败 5 次自动停用）；唤醒消息自带项目坐标，故 sticky 会话重建后仍能自行复位（磁盘是唯一可信长期记忆）。已有**本项目**的心跳任务时只补齐启用与表达式（及缺失的项目根绑定），不新建。

#### 生生：心跳任务的项目归属（跨项目同名撞名）

**问题形态**：任务面**只按名字**反查（`tasks.list()` 逐条比 `name`），而任务名是常量 `SHENGSHENG_EVOLVE`——多个演进项目共存时，`ensure` 会命中**别人的**心跳任务并返回「已存在（幂等，未新建）」（复用它＝唤醒别人的项目、不为自己建驱动），`pause` 更会把**别人的演进停掉**。任务登记里本就带着 `env.SHENGSHENG_ROOT`（归属证据伸手可取），工具却从不比对，返回文案还把「找到了」包装成「已存在｜启用｜周期…」，把歧义证据当结论（与提示词「玄学化的有效边界」明禁的**事实污染**同一病理：返回文案把「找到了」说成「没问题」）。

**修复：任务名按项目根派生 + 四个 op 一律核归属**（实现于 `shengsheng.ts`）：

- **派生名**：`SHENGSHENG_EVOLVE::<根名>-<根哈希>`（FNV-1a 32 位）。根名只为可读，**根哈希才是判据**（跨机、同名目录不撞）；同根多次调用派生同一名字，故幂等。历史名 `SHENGSHENG_EVOLVE` 仍认得（升级不丢既有项目的驱动任务）。
- **归属判据**：`taskRootOf(task)` = 任务级 `env.SHENGSHENG_ROOT`（旧版缺该字段时从唤醒 prompt 的「项目根：」行回读，因旧版任务确实只有 prompt 有根），与当前解析 root 按 `rootKey`（分隔符归一、去尾分隔符、Windows 盘符与大小写归一）比对。
- **ensure**：先按 历史名 / 派生名 / 显式名（`SHENGSHENG_TASK_NAME`）依次探测，**每个命中都核归属**；命中他项即取下一位候选。全落空则新建（显式名未被占用时优先用它，否则用派生名）。命中本项目旧任务时**补写 env 绑定**（`sameRoot(taskEnvRoot, root)` 不成立才写，幂等）。
- **status / resume / pause**：同一套候选与核验；无归属任务时如实报「本项目尚无心跳任务」并**点名同名他项任务**（含其 id 与项目根）；`pause` 只停本项目的那条，他项一字不动。
- **显式 `task_id` 也核归属**：指到他项任务时**直接拒绝**（`data.blocked`），既不采信、不改动，也不顺手另建一份——复用/停用别人的演进任务正是本校验要杜绝的错。
- **返回文案现在始终带归属事实**（「本项目…／该任务属于其他项目」），模型无需再自行回查 `tasks.json`——歧义不再流到决策侧。

**回归**（`shengsheng.test.ts` 新增 2 条）：「跨项目撞名护栏」——盘上预置一条他项旧版同名任务，断言 status 报「尚无心跳任务」且点名他项、ensure **不复用而是新建本项目那条**（名字含 `::`）、重复 ensure 幂等且报「与本项目一致」、pause 后他项 `enabled` 仍为 true 而本项目那条被停、显式 `task_id` 指他项被拒且**他项任务一字未动**（前后 JSON 逐字相等）；「旧版任务升级」——旧版只有 prompt 带根时 ensure 认得出**不另建**，并把 env 绑定补齐。

- **预加载**：`preload = false`，按需装载（由心跳任务的任务级 `agents` 预载到执行会话）。

#### 生生：问卦（`divine`）——择的探索哨兵

**为什么需要它**（与「提示词设计」同源的三件真事）：① **模型自选 = 收敛偏置**——同一模型、同一上下文反复自择会反复选中同类方向（惯性 + 幸存者偏差），河越走越窄，卦是一次**以环境数据为条件的受控随机**（探索-利用困境里的 ε 探索）；② **僵局空耗**——提示词给了方向来源（坏掉的东西 / 将成之势 / 没人看过的地方）却没给**岔口**的解法：几条种子证据平衡、或无种可播又不想停河时，模型只剩空息或无据硬选；③ **停河倾向**——卦把「停」合法化为一种答案（艮=止、困=省、明夷=藏器），「允许什么都不派」因此从纪律条文升为可审计的承诺。一句话：**卦不产生新证据，它把「没有证据时的选择」变得便宜、可审计、不反悔**。

实现于 `shengsheng/hexagrams.ts`（卦象数据与起卦内核：纯数据 + 纯函数，无 I/O、无随机源）与 `shengsheng.ts` 的 `divine` 工具（读环境、落台账、一息一卦的闸）。

- **何时问（三问三不问）**：**问**——岔口（≥2 候选证据平衡，传 `candidates` 定卦）、方向枯（种子空、无待收，河将断未断，不传 `candidates` 开卦）、势乱（大裂痕刚清完）。**不问**——证据明确指向单一方向（证据在场时问卦是渎）、有待收成果（**收比问大**）、裂缝在场（虚假产能/幻影成果/坏门是证据，处置优先）。**枯分两型**：无向可择的探索型枯可问；**有向而不得行**（前提缺失/需人拍板）不是迷茫、是缺门——修门或 pause，不问卦。
- **成卦公式**（梅花易数的时间起卦，把「时间」换成生生自己的呼吸）：
  ```
  上卦序 = (月 + 日 + 时辰序 + 种子数) mod 8          // 外势：天时与粮草
  下卦序 = (呼吸 + 在途×2 + 在跑×3) mod 8             // 内势：河自身
  动爻   = (上卦序 + 下卦序 + 派工序号 + 时辰序 + 掷) mod 6 + 1
  ```
  八卦序依梅花旧制（乾一 兑二 离三 震四 巽五 坎六 艮七 坤八）；三爻自下而上、阳一阴零，该序恰是二进制 7..0，故「序→爻」无需另抄一份表（`trigramOrderOf` 是其逆运算）。**呼吸入卦 = 历史入卦**：用自身的脉搏当这个世界的时间，问出来的卦才带着这条河的履历（只取钟表时间的河与该条河会同卦）。
- **自由度（务必认清，易被「64 卦 = 64 个方向」误读）**：一息之内上下卦的全部输入（呼吸/在途/在跑/种子/月日时辰）皆不变，故**本卦/变卦/体用在这一息里恒定**、**本卦是这一息的「处境」而不是抽出的随机方向**；唯一的随机在**动爻**。一息实际可用空间是 **1 本卦 + 6 变卦**（+ 定卦模式下的卦指），跨息靠「呼吸 +1 → 下卦序 +1」遍历（8 息一圈），长程才铺开 64 卦。
- **随机源**：`crypto.getRandomValues`（**不用 `Date.now()`**——后者的分布由「模型何时决定调用」决定，越犹豫越集中于常用区间，等于让被随机化者提供随机性），掷值**完整记入台账**（`起卦输入（可复算）`一行含全部输入），故「复现靠台账回放」与「随机的质量」同时成立。
- **上下卦与爻位必须自洽**：上卦 = 外势、下卦 = 内势（下卦为内，与《易》「内卦为贞」同向）；爻位语义初=根基（验证门/仓库健康）｜二=内政（池与在途）｜三=门径（派工/续派链）｜四=外势（种子/台账/新域）｜五=君位（当前主线）｜上=极（该退役/该停的旧物）。**体用**：动爻所在之卦为「用」（事之所在），另一卦为「体」（我之势）；**爻位一至三落于下卦（内势）⇒ 事在本家（收/续/清池），四至上落于上卦（外势）⇒ 事在方向库（择新/换域）**——两张表因此互相印证（若反过来把内势放上卦，「二爻=内政」会落到外势卦里，自相矛盾）。
- **定卦与开卦**：传 `candidates` 即**定卦**——`卦指 = ((本卦池序×6 + 动爻-1) + 掷) mod k + 1`，明示「卦指第 k 候选」；迷茫即证据平衡，剩下的就是选择本身，**卦指即落**。不传即**开卦**——卦只给方向域（爻位 + 生生断 + 变卦之势），模型据此 `seed_add` 一条新方向再布雨。**卦指把「掷」也纳入取模**：只用「卦序号+动爻」时可选值恒 6 个，k 不整除 6 则次数不均、**k > 6 时更有候选项永远指不到（结构性死区）**；纳入均匀的掷值后任意 k 都可达（测试枚举 k ∈ 1..9,13 断言「无死区」）。
- **断卦——工具给骨，模型给肉**：工具返回三件（**卦卡**：本卦/动爻/变卦/体用 + 该卦「生生断」+ 爻位语义；**环境快照**：天时/内势/粮草/地气/近收的**原文事实**；**断卦框架**四行：象/境/合参/落向），断在模型。**「生生断」只作提示语**（64 句，每句须命中一个操作动词族：布/派/收/停/修门/清池/挖/拆/深观/退役）——卦→动作**不做硬映射**，否则沦为 64 格老虎机，恰是对「择无法被写进手册」的尊重。「生生断」与爻位语义、64 卦名同在 `hexagrams.ts`（纯数据，不进提示词）。
- **落卦与承诺（本设计最重要的一道闸）**：每卦入台账 `ledger.md`（复用 `appendLedger` 的幂等键机制），键为 **`divine:{breath}:{questionHash}`**——但**一息一卦的闸只看 `divine:{breath}`**：`questionHash` 仅作**记录与回看**的标识、**不参与幂等**（键若掺问句，把「下一步做什么」改成「下一步该做什么？」就能重掷，而「重掷到自己喜欢的答案」正是这道闸要防的全部）。同息再问一律不掷新卦、返回前卦原文（初筮告，再三渎，渎则不告）。`questionHash` **定长 8 位 hex**——`appendLedger` 的判据是 `md.includes(key)` 的**子串**匹配，变长键会让短键被长键吞掉（`divine:3:ab` 误命中 `divine:3:abc`）。**卦落之向本息内不翻悔**；新证据到**下一息**可改道，改道记台账「卦改判」及理由。**台账不可写时卦照掷但明示「未记档」**——未入档的卦不算掷过，下息可重问。
- **卦的边界三条**：① **不豁免验证**（工单的验证门照旧）；② **卦不得覆盖硬约束**——卦指候选违约束文档则**弃卦**、台账记一行理由（留这个口子，否则卦可能指向违约方向）；③ **卦决定方向不等于保证可派**——池满/额度不足时把方向落种子、记「卦落未派」，下息再派，**不因派不出而重掷**（否则模型会以「派不出去」为由绕开一息一卦的闸）。
- **滥用提示（软闸，不拒）**：`divine` 顺带读台账卦史，近 `DIVINE_WARN_WINDOW`(5) 息内已问 ≥ `DIVINE_WARN_COUNT`(3) 卦时附一句提醒（「证据在场时问卦是渎」）。问卦比硬想便宜，「迷茫才问」是容易被自己绕过的纪律，故从「模型自觉」升为「模型被提醒」。
- **地气降级**：只读 `<根>/.git/HEAD` 取**分支名**一行，**不解析 HEAD hash、不跟随 gitdir（worktree）、不回退 packed-refs**——三种布局各有代价而它只用于断卦合参、不入公式；读不到（非 git 仓 / 沙箱外 / 游离 HEAD）一律写「未取」，**不报错、不阻断**。
- **零新执行面**：不派 worker、不执行命令、**不写 `control.md`**（control-format 的版本化机制零触碰），审批面不外扩；`requiresApproval: false`。未初始化（无 `control.md`）时**拒绝问卦**：内势无据，先 `action=init`（与写分支同一纪律）。
- **一息内的调用次序**：卦须落在 `state action=note` **之前**（呼吸在 `note` 推进，顺序颠倒会把这一卦记到下一息，且本息再问会被上一息的键挡住）。一息建议：`state read → 观复 → collect → divine（若迷茫）→ dispatch → note`。
- **测试面（`shengsheng.test.ts` 新增 11 条）**：卦表完整性（64 卦名唯一、断语 64 句齐备且每句命中操作动词族、序爻同源）+ 成卦可复算/变卦/体用归属 + 自由度（同息本卦恒定、呼吸 +1 本卦必不同）+ 卦指无死区（k ∈ 1..9,13 枚举）+ 时辰序跨界 + **零新执行面**（不派会话/不执行命令/不动 control.md）+ **一息一卦**（换问句也绕不过、返回前卦原文、台账只落一卦）+ 定卦卦指落在候选内 + 跨息解除 + 未初始化拒绝 + 键不互吞（定长）+ 卦史滥用提示。

#### 回退型门禁的并发防护（`reverse_verify.py` / 中断演练共用 `verify_guard.py`）

生生的硬门禁 `packages/agents/src/agents/shengsheng/reverse_verify.py`（逐条回退源码→断言变红→回填）
与 `packages/agents/scripts/reverse_verify_retire_tree.py` 等回退型脚本
**共用一份防护实现**：`packages/agents/src/agents/shengsheng/verify_guard.py`（放在门禁同目录：判据里的「同树」就是被保护文件所在的树，
避开「两套标准」；脚本以 `sys.path` 引用，生产脚本部署形态不变）。

| 能力 | 口径 |
|---|---|
| 起跑前并发检测 | 扫 `/proc/*/cmdline`：命中 `*reverse_verify*.py`（本仓全部门禁脚本的命名）或 `bun test` ⇒ **默认拒绝，退出码 3**（区别于验证失败 1/2） |
| 排除自身与亲属 | 排除自己 + 祖先链 + **后代**；否则门禁自己起的 `bun test` 子进程会被当成并发 |
| 排除观察器 | `pgrep`/`grep`/`ps`/`vim` 等把名字当**参数**的进程不算在跑验证（实测：有演进线的等待循环里写着 `pgrep -f "reverse_verify.py"`，曾被当成并发 → 假阻；真门禁自己是独立进程，排除它们不会漏掉真门禁） |
| `--force` 逃生口 | 显式覆盖才跑，警告**打在输出最开头**「已按 --force 跳过并发检测，结果可能在竞态下不可信」 |
| 锁文件 | `<tree>/.reverse_verify.lock`（pid / boot_id / 时间 / 命令行），退出即删；陈旧锁按「pid 是否存活 + 是否同一次开机」自清 |
| 信号收尾通道 | 第一次 SIGINT/SIGTERM 只置标志交给宿主 `finally`；**之后的信号全部忽略**（默认处置会在回填/校验中途直杀进程，`finally` 永远跑不完——正是事故 10:10 的成因）；退出码仍是 130/143 |
| 自检逃生口（非生产） | `REVERSE_VERIFY_PEER_IGNORE`（逗号分隔正则）、`REVERSE_VERIFY_PEER_IGNORE_PIDS`（按 pid 精确）、`REVERSE_VERIFY_PEER_PATTERNS`（额外判据） |

**为什么必须默认拒绝**（两次真实事故）：09:39 两个 `reverse_verify.py` 同时跑、互相回退同一份 `shengsheng.ts`，
一个 worker 报「莫名变红」（门的残留冒充真缺陷）；10:10 主门禁起跑时同树有 `pty_form_reverse_verify.py`，
被手动 stop 后源码停在回退态、留下 `shengsheng.ts.revbak`，测试 62 pass / 1 fail，靠人工恢复。

**演练与自证**（`reverse_verify.py` 的演练入口 + `reverse_verify_retire_tree.py`）：S1 SIGINT / S2 SIGTERM / S3 SIGKILL / S4 复跑，
加上：S6（收尾中途补枪，源码仍须逐字节回原样）/ S7（把信号通道降级 → 事故必须**重现**，否则 S6 是空转）/
S8（真起一个冒充验证进程 → 必须拒绝退出 3、`--force` 必须放行且先喊警告；`--degrade-detect` 把检测
**两条通道**（命令行扫描 + 锁）都退化成恒假 → **同一组断言必须变红**）/ S9（在用例循环第 1/2/3 个用例的
收尾窗口各打断一轮——覆盖「已经跑过若干用例后才被打断」这条真事故路径）。

**防护只覆盖两个脚本**（`reverse_verify.py`、`reverse_verify_retire_tree.py`）：仓内其余回退型脚本
（`shuangtian/tools/*_reverse_verify.py` 等）仍各自实现回退与恢复，未接入 `verify_guard` 的锁与信号通道。
它们被中断时仍可能留下 `*.revbak`；此时以哨兵 `gate_probe()` 的 `.revbak` 与进程栈两道判据兜住。
接入新脚本时走同一入口 `verify_guard.preflight()` + `verify_guard.Traps()`，不要另写一套。

**诚实边界**（会失效的情形）：判据是**命令行文本**——改名的验证进程、容器/另一 PID 命名空间里的进程、
被包成别的可执行文件的验证都看不见；锁是 marker 文件不是内核互斥，两个进程同时起跑的窗口靠「先写锁再干活」
缩小但非原子；pid 被复用时锁会误判为「有人持锁」（安全侧失败，`--force` 可越过）；非 Linux（无 `/proc`）时只剩锁文件兜。
**根治方向**（另一单）：把回退型门禁搬进独立 `git worktree`，共享树字节全程不变。

#### 收单门禁：把「建议」接成「必经路径」

三套判据本来都在（`commit-scope.ts` 核提交范围、哨兵 `--anchors` 核修复锚点、
`verify-gate.ts` 判反向验证真假），但全部只输出**建议**：`review` 是只读的，`collect` 唯一的
拦截条件是「summary 非空」。于是一条纪律是否被遵守，取决于当息那个模型记不记得去看——
那就不是约束。`governance.ts` 把三者接成 `collect` 的必经路径：

| 门 | 判据 | 不可跳 | 读不到时 |
|---|---|---|---|
| `anchors` | 8 条修复锚点全 `present:true` | ✓（缺失只可能是一个成因：修复被回退） | `unknown` |
| `commit-scope` | 最近一次提交的文件全在工单提到的路径内 | ✓（带走了别人的东西必须当场停下） | `unknown` |
| `reverse-verify` | `verify-gate.judge()` 判 `genuine` | ✗（非机制类交付可带理由跳） | `unknown` |

**三态而不是两态**（`pass` / `fail` / `unknown`）：把「读不到/没跑」报成 `pass` 是最坏的一种
（最需要警报的环境恰好是唯一安静的环境）；把它报成 `fail` 会让人去修一个没坏的东西（假退化）。
**总判是合取**：全 `pass` 才 `ok`，任一 `fail`/`unknown` 均拒收入账。

**拒绝时状态原封不动**：不入账、不动在途、不释放租约——门禁只读不写，
落盘仍由 `collect` 在它那把锁里做（两处各写一次会出现「拒绝入账」与「已被改掉」之间的窗口）。
**强制跳过必须留痕**：`force=[{gate,reason}]` 能过，但理由必填（空理由即不成立），
且原判与理由一并写进台账——静默跳过等于没有门禁。

#### 反向验证的机器判据（`verify-gate.ts`）

「加了机制 ≠ 机制生效」的难处不在立规，而在**判真假**：交一条 `verify` 命令，它可能真的验了反向，
也可能是恒绿（断言没判别力）/ 空壳（`exit 1`）/ 验的根本不是这一单。
**字符串黑名单堵不住**（那是「把关键词抹掉就通过」的同型失效），故判据改为**干预**：
让命令在两个由我们控制的状态下必须给出相反结果。

| 档 | 判的是 | 不过时 |
|---|---|---|
| S0 | 命令非空、不是恒真动作；`rollback ≠ defect` | `empty` |
| S1 | `original`（原样）绿 | `broken` |
| S2 | `rolled`（退回缺陷写法）红 | `vacuous`（无判别力） |
| S3 | `restored`（原样放回）又绿——红确由 `rollback` 引起 | `wrong-cause` |
| S4 | 同态复跑一致 | `unstable` |
| S5 | 范围合理性（段过短/落在注释/非源码） | 只提示，不改判决 |

四档全过才 `genuine`。**S3 不可省**：没有它，一个「本来就红」的命令（如拼错文件路径）
会被 S2 当成「退回会红」而通过。**判定只有一份**（纯函数，可穷举），执行器
`reverse-run.ts` 只负责采集观察集——两处各判一套就会「同一条 spec 两个结论」。

**判据自身的判别力**由 `verify-gate.mutation.test.ts` 守住：把每一档逐一退掉，
对应用例必须变红（七条变异全部杀死才算成立）。这一步是必要的：
一个没有判别力的判定器会把空壳判成 `genuine`，然后门禁放行并落进台账——
那是**最坏的一种恒绿**。

#### 定期简报与授权围栏：人仍然控制方向

AI 能自主选择「做哪一件」，但两个前提必须由人掌握：**它只能在你给的范围内动**，
而且**你能定期验收它到底在干什么**。两者分别由 `governance.ts（授权面）` 与 `governance.ts（简报）` 承担。

**授权围栏**（`.evolve/authority.json`，人写）：

| 形态 | 判定 | 派工动作 |
|---|---|---|
| 落在 `allowPaths` 内、不在 `denyPaths` 内 | `in-scope` | 照派 |
| 落在 `denyPaths`（禁区优先） | `out-of-scope` | **拒** |
| 不在授权内但不在禁区 | `out-of-scope` | **暂缓**（方向退回种子，等人批） |
| 方向是自然语言、无路径可判 | `unverified` | **照派 + 留痕** |
| `authority.json` 不存在 | `unverified` | **照派 + 留痕** |

后两行是最容易写错的地方，也是实测代价所在：

· **`unverified` 不能阻断**。大多数方向就是自然语言（「修一下并发丢写」），拦了就把围栏
  从「限制越权」变成「停止演进」——人看到的第一件事是它不干活了。这与门禁 `unknown`
  不阻断是同一条口径：**只能拦住「明确越界」，拦不住「说不清」**。
· **文件不存在 ≠ 授权为空**。把「没声明」归成空授权，会让新接入的项目与所有存量演进线
  在第一次派工时全停（实测：53 条测试一次全红）。只有**文件存在且写了空 `allowPaths`**
  才是「明确什么都不授权」。

路径判据：**按段比较**（`toolshed/x` 不算 `tools` 之下）+ **`*` 通配表文件族**
（`.env*` 才能禁住 `.env.local`；按段比较对文件不适用）。两种语义不靠猜，由授权方写清。

**定期简报**（`shengsheng_state action=brief`；心跳在收息前按 `SHENGSHENG_BRIEF_HOURS` 自触）：

· **“该不该出”由磁盘上的 `<!-- brief:ts -->` 标记算**（`dueForBriefing`）：窗口未到就只回一句
  “未到出简报的时间”并退出，要强制出传 `force=true`。节律由机制判，不由模型记得去问。
· **送达走两条腿**：① **前端弹窗**（`event.notice` → `ui.ts:showNoticeDialog`）——它不依赖任何
  外部通道配置，是“通道没配也能送到人眼前”的唯一路径；② 任务通知通道（webhook/飞书，需配）。
  两条各自报结果，不互相冒充。
· **弹窗而非 toast**：简报可能很长且关键，一行截断与自动消失都会让关键信息没人看到；
  `level` 由待决项性质定（有越界/通道不可达 → `error`，有待决项 → `warn`，全无 → `info`）。
· **只报异常，不堆成功**。全部是成功案例的简报等于「一切正常」的噪音，而人会因此不看——
  那比不报更糟（他以为自己掌握了情况）。
· **待决项只列需要人拍板的事**：在途挂超 48h、有动作但零收单、越界请求、方向枯、通报通道不可达。
  每条带「为什么这得你定」与可选处置。**不给「继续/停止」的建议**——那正是需要人做的部分。
· **没核成的东西单列**（台账读不到、通道可达性未知）：把「读不到」报成「本周无进展」
  是最坏的假绿；报成「出了问题」会让人去修没坏的东西。
· 简报本身入台账（`<!-- brief:ts -->` 机器可读标记），下一份据此算窗口——
  不另存状态文件（多一份状态就多一处不一致）。


#### `shuangtian`（霜天原生桌面框架与智能体操控）

霜天（`shuangtian/`，与 `keqing/` 平级）是歌白**内置的原生桌面应用框架**，也是"智能体躯体的原生形态"：
C++20、全自绘（软硬件渲染兼容）、**支持无头模式**，并对外提供 TCP 控制通道。
它的存在意义是让智能体在**没有桌面的 Linux 服务器上**也能完整地开发、观察、操作原生界面。

> **外部依赖只有两个**（均登记在 `shuangtian/third_party/SOURCES.md`，含版本/来源/许可与改动面）：
> nlohmann/json 3.12.0（JSON，必需）与 quickjs-ng 0.17.0（应用内脚本层，**默认关闭**）。
> 渲染、字体解析与整形、文本布局、Markdown、组件库、包管理器、构建驱动**全部自研**。

- **框架本体**（`shuangtian/`）：分层 `core → math/codec → raster → text → md → ui → app`，`shell` 提供窗口后端，`control` 提供远控，`pkg` 提供自研工具链（`stpm`，`st build/test/lint`）
  - **渲染**：软件光栅器（扫描线覆盖率抗锯齿 + SIMD 快路径）为唯一真相源，无头与有窗口逐像素一致；硬件后端（Vulkan/GL）为可选加速，不做前提
  - **DPI 一等公民**：逻辑/物理像素分离——Canvas 绘制 API 收逻辑坐标、内部按 `device_scale` 在物理分辨率光栅化（字形亦按物理尺寸重栅格化、缓存键含物理尺寸）；支持非整数倍率与运行时切换（`app.set_scale`）
  - **文本与 Markdown**：自研 TTF/OTF/OTC-CFF/CID 字体解析与整形（CJK 回退、SC face 优选）、软换行/省略号、按物理尺寸栅格化与字形缓存；`st::md` 提供解析 + 流式增量 + 零依赖代码高亮，`ui::MarkdownView` 组件支持 `append_chunk` 流式渲染（面向大模型应用）
  - **控制通道**（协议 `st-control/1`，TCP + `uint32` 长度前缀 JSON 帧）：`tree`/`find`/`get`/`set`/`invoke`/`input.mouse`/`input.key`/`input.text`/`capture`/`visual`/`wait`/`metrics`/`events`/`theme`/`app`；**协议内坐标一律逻辑像素**；**默认无任意代码执行入口**（只有数据与动作），可选脚本能力需 `--enable-script` 显式开启（开启后脚本仅能调显式注册的界面宿主函数，并有内存/栈/时长/转换深度四重配额）
  - **扩展层（`ext`）**：`st/ext/json.hpp` 把 nlohmann 的抛异常接口收敛为 `Result` 风格（解析带嵌套深度预检与字节/行列位置诊断；读取永不抛；键序保留），替掉旧自研 `st::json`——旧实现把所有数字存 `double`，`2^53+1` 与雪花 ID 会失真；`st/ext/script.hpp` 提供受限 JS 脚本引擎（quickjs-libc 已剔除，无文件/进程/socket 能力）
  - **构建效率**（自研驱动，不依赖 CMake/Make）：预编译头（**仅标准库**）+ `-MMD` 头依赖增量 + 并行编译 + lld 链接 + 目标级源排除；实测 45 个翻译单元全量 ≈ 55s、改一个文件 ≈ 3s、无改动 ≈ 0.02s（8 核）
  - **示例**（同时作为"框架完备性/易用性/高阶定制"的实证）：`gallery`（组件集与设计令牌巡检）、`mdeditor`（Markdown 编辑器：大纲/编辑/预览/语法高亮源码视图/可拖拽分栏/实时统计/流式生成演示；其中四个自绘组件**不改框架一行代码**，只用 `Element` 的 `measure`/`arrange`/`paint_content`/`on_event` 四个扩展点）与 `codeeditor`（代码编辑器：30 种内置语言语法高亮 + 运行时注册的自定义语言 + 只读查看器 + 脚本驱动演示）
- **子代理**（`packages/agents/src/agents/shuangtian/`：`shuangtian.ts` 定义 + `shuangtian_tools.ts` 工具集 + `shuangtian_client.ts` 控制通道客户端 + `shuangtian.md` 提示词 + 两个测试文件）：把框架当成"可编排的受控进程"
  - **工具集**（15 个）：`run`（build/test/lint/start/stop/status/logs——首次自动自举 `stpm`；`start` 以无头模式脱离会话启动并**轮询控制文件等端口 → 握手确认**）、`apps`（实例清单 + 端口连通性）、`tree`/`find`/`get`/`visual`/`metrics`（只读观察）、`set`/`invoke`（状态与动作）、`click`/`type`/`key`（真实输入注入，逻辑像素坐标）、`capture`（截图 PNG，**结果直接作为图片块回传**）、`wait`（条件等待，拒绝轮询截图）、`call`（协议逃生门）
  - **审批姿态**：只读类免审批；`run`/`set`/`invoke`/`click`/`type`/`key`/`call` 需审批（写与输入面）。`call` 可到达控制通道的**任何**方法——包括可选开启的 `script`（应用侧默认关闭）
  - **环境变量**：`SHUANGTIAN_PROJECT`（框架工程根，缺省仓库 `shuangtian/`，二进制形态回落 `{GEBAI_HOME}/vendor/shuangtian`）、`SHUANGTIAN_TARGET`（默认控制目标）、`SHUANGTIAN_FRAMEWORK`（随包分发位置）
  - **闭环**：`run(build) → run(start) → find/tree → capture 看 → set/invoke/click/type → capture 复验 → run(stop)`；所有"改完界面"的结论都要求有截图支撑（无头模式像素与有窗口一致）
  - **观感类改动的入口**：提示词的"开工前"清单要求，改字体/配色/动画/手感这类**验收标准在用户身上**的功能时，先读 `shuangtian/docs/perceptual_changes.md`（正交量拆解、量尺三坑、可复用的八步协同循环、反模式清单），硬约束在 `shuangtian/CONVENTIONS.md §9.2`。**提示词只作索引、不复述文档内容**——两处同义内容必然漂移；回归用例同时断言"手册被引用"与"未整段复述"（双向）
- **实战校验（框架自身的硬证据）**：用该子代理在无头环境下开发霜天本身，两轮共暴露并修复了 20 类问题：填充未隐式闭合子路径导致 CJK 字形糊块、覆盖率为带符号量被当成透明度导致圆角/阴影/字形全失效、Type2 子程序负索引（bias）误判、字形缓存"插入后淘汰"返回悬垂指针（ASan 定位）、`process::run` 二次 waitpid 失败把失败退出码当成 0（掩盖编译/链接失败）、非阻塞读把空闲客户端误判断开、PCH 与消费端标志不一致导致 PCH 失效、PCH 塞入项目头反而更慢、运行时切 DPI 内容缩在左上 1/4（`Canvas` 移动语义漏搬缩放）、HiDPI 下截图只截到 1/4、2x 屏上卡片内子项整片消失（圆角裁剪未做逻辑→物理换算）、根页面 `grow` 失效、控制通道 `input.mouse` 因 nlohmann `at()` 抛异常而**整进程终止**（现已有 lint 规则 L12 机械拦住同类写法）等——**没有一条能靠读代码发现，全部来自「无头运行 + 控制通道观察 + 截图核验 + sanitizer 复跑」的闭环**（清单见 `shuangtian/DESIGN.md` §8.2）
- **验证手段**（随框架交付）：`st test`（102 项单测 / 2018 断言）、`st test --san`（ASan+UBSan）、`st lint`（11 条禁用特性规则，0 违规 + 7 处登记豁免）、`tools/st_visual_check.py`（dev/san 两档 × 两个示例 × 查询/操作/输入/主题/DPI 全序列 + 截图 + sanitizer 报告检查）、`tools/ft_compare.cpp`（用 FreeType 对照自研字体引擎的轮廓/包围盒）
