# 架构（上）：核心不变式 / 运行形态 / 模块接口 / 子Agent / 子会话

> 本册是 `DESIGN.md` 的分册，**章节号与主文件保持一致**（源码注释按 `DESIGN.md §x.y` 引用章节号，不重编号）。
> 索引与全貌见 [`../DESIGN.md`](../DESIGN.md)。

## 架构

### 核心不变式：能力外置、权力内守（TS 引擎核心地位）

歌白的核心是 TS 引擎里的「对话 → 工具调用 → 审批 → 执行」主循环（`AgentEngine` + `ToolRegistry`）。扩展能力可以外置到任意语言、任意进程，**裁决权力必须留在引擎**——本不变式与「单一真相源」并列，是所有多语言/边车/插件类扩展（客卿、`js` 桥、动态工具、二开）的准入门槛。

| 归属 | 内容 | 现落点 |
|------|------|--------|
| **能力**（可外置） | 计算与算法（BM25/哈希/图像/CV 推理）、协议实现、工具实现细节、只读取当前请求 ctx、声明式上报 | 客卿驱动（Python/C++/Go/Rust）、`js`/动态工具子进程、CV sidecar |
| **权力**（不外放） | 工具可见性（装载门控）、审批姿态判定、安全模式裁决、嵌套/depth 规则、会话 ctx 组装、会话真相（chat.json/动态工具/todo/tasks）、进程生命周期 | `registry.resolve` 只返回 `enabled`；`requiresApproval` 由引擎以 `stripApprovalFlags` 后的参数调用；`isToolBlockedInSafeMode`；`fromJsBridge`/`depth`（标记由宿主内存盖章，子进程伪造不了）；`sidecarTool.execute` 组装请求 ctx；`SubAgentManager` 启停回收 |

**边缘执行体只有两种合法姿态**：

- **声明（declaration）**——上报自己具备什么：客卿 `tools.list` 上报工具清单与 JSON Schema，注册名（`{agent}_` 前缀）、审批姿态、装载可见性均由引擎决定；`manifest.requiresApproval:false` 属静态声明（写 manifest 者本就能写任意代码，可 diff、可审阅，用户还可 `PATCH /api/v1/tools` 覆盖）
- **委托（intent）**——表达想做什么，由引擎裁决后执行：`js` 桥的 `{t:"call"}` 是唯一现役实现，引擎按调用者审批姿态（免审运行拦截需审批工具）、会话 ctx、嵌套规则重新裁决，子进程无法伪造豁免

**两条红线**（违反即稀释核心地位，不予准入）：

- **不得自证授权**：运行期由执行体自证「本次免审 / 我有权限」无效——审批姿态的唯一判定方是引擎，运行期自证不可审阅、不可 diff
- **不得自取工具**：执行体自行取用工具、自行指定会话 ctx（sessionId/user/cwd/env）、自行缓存授权结果——一旦如此，引擎退化为消息总线

**提案判定口径**（新开「外部执行体 → 工具」通道前先过三问）：① 来源可否证明且不可提权（宿主盖章标记 / 签发 token 链）？② 审批姿态由谁判定（须引擎；黑盒执行体不得继承调用者的审批豁免）？③ 能否用既有结构化解替代（TS 工具内 `ctx.registry` 直调、客卿跨语言同名合并、`js` 编排）？三问不齐则不做。参照实现：`packages/agents/src/core/shared/cv-analysis.ts` 的 `ctx.registry.resolve(tool).tool.execute({ ...args, image: rel }, ctx)`——进程内直调其他工具，复用装载门控/审批/ctx 组装，不绕过任何治理层。

### 运行形态

一套核心代码、同一个二进制可执行文件，支持两种运行形态，由启动参数/环境变量切换：

| 形态 | 启动方式 | 说明 |
|------|---------|------|
| **本地模式**（默认） | 直接运行二进制 / 访问 `http://127.0.0.1:{port}` | 本机使用，直接以 **admin 用户**身份工作，**免登录、不受任何权限限制**（超级权限 + 路径沙箱豁免）；桌面 WebView / 浏览器均可 |
| **服务模式** | 后台运行二进制 + `GEBAI_MODE=server`（或 `--server` 参数） | 部署在服务器上，多用户公用，**账号密码登录**（密码仅存加盐哈希）；**开放注册**（注册用户为普通角色；策略由 `GEBAI_SIGNUP_MODE` 控制——`open`（默认）= 自助注册、注册即用免审批，`approval` = 注册后待 admin 审批；来源由 `GEBAI_SIGNUP_SOURCE` 控制——`custom` 时仅二开前端代码可注册，内置登录页入口自动隐藏）；admin 为特权用户，**唯一入口是 `GEBAI_ADMIN_PASSWORD_HASH`**（未设置时 admin 禁用） |

核心逻辑完全一致，仅宿主（WebView / 浏览器 / 远程）与用户机制不同。

#### 启动参数与环境变量

启动配置可来自 `.env` 文件或真实环境变量，**真实环境变量恒优先**（文件只填充未设置的键，`键=值` 逐行解析）：脚本调试模式显式加载仓库根 `.env`；二进制发行（含桌面端两种形态——浏览器形态 exe 与启动器侧车）加载 `{GEBAI_HOME}/.env`（默认 `~/.gebai/.env`，不存在则跳过），部署产物免重建环境即可修改配置。

| 参数/变量 | 说明 | 默认 |
|----------|------|------|
| `GEBAI_HOST` | 监听地址（`127.0.0.1` 本地 / `0.0.0.0` 对外） | `127.0.0.1` |
| `GEBAI_PORT` | 监听端口（桌面形态默认固定 47896，见「端口固定」） | `3000` |
| `GEBAI_PLAYWRIGHT_CHANNEL` | playwright 子Agent 浏览器启动 channel（`chromium.launch` 的 channel 参数，留空 = 不指定用默认 chromium） | 平台默认：Windows=`msedge`（系统 Edge，免下载浏览器），其余空 |
| `GEBAI_MODE` | 运行形态：`local`（本地模式，默认）/ `server`（服务模式，需登录鉴权）；兼容旧 `GEBAI_AUTH`（`none`→local、`multi`→server） | `local` |
| `GEBAI_ADMIN_PASSWORD_HASH` | 服务模式 admin 用户密码哈希（格式 `salt:hash` 均为 hex，salt 16 字节/hash 64 字节，与注册表 scrypt 加盐哈希一致）；**设置则启用 admin（每次启动覆盖其哈希），不设置则禁用 admin 用户**；非法格式启动即报错。生成命令：`bun run --cwd packages/server hash-password -- '密码'`（或管道/交互输入） | 空（admin 禁用） |
| `GEBAI_SERVICE_API_KEY` | ~~已移除~~：原业务系统服务密钥机制已废止——**接口统一账号密码认证**（登录签发令牌），不再有独立服务令牌（避免任何服务端密钥进入 Agent 可达环境） | - |
| `GEBAI_CORS_ORIGINS` | 允许跨域来源（逗号分隔，`*` 表示全部） | `*` |
| `GEBAI_BASE_PATH` | ~~已移除~~：前端请求（静态资源/`/api/*`/`/ws`）一律**按页面 URL 相对解析**，反向代理子路径挂载无需配置，见「反向代理支持」 | - |
| `GEBAI_TRUST_PROXY` | 是否信任 `X-Forwarded-*` 代理头（`true`/`false`）：`X-Forwarded-For` 用于登录/注册限流分桶，`X-Forwarded-Host` 在代理改写 Host 时参与同源判定（见「反向代理支持」） | `false` |
| `GEBAI_SANDBOX` | 路径沙箱：`auto`（**只看运行形态**——服务模式强制启用，本地模式不限制；不按监听地址/IP 判定）/ `on`（强制限制）/ `off`（不限制）；**admin 豁免仅本地模式**（`isExempt` 判 `auth === "local"`；服务端部署下 admin 同受沙箱约束，见「多用户隔离与安全」） | `auto` |
| `GEBAI_SCRIPT_ISOLATION` | 脚本运行根隔离：**服务模式默认且强制开启（`off` 与服务模式互斥，启动即拒绝，同 `GEBAI_SANDBOX=off` 防呆）**——`auto`（环境收敛，bubblewrap 可用时升为文件系统隔离）/ `env`（仅环境收敛，不可再降）/ `bwrap`（强制文件系统隔离，不可用时告警并回落 `env`）；本地模式恒不收敛（`off`/`auto` 同效）；收敛项：`HOME`/`TEMP`/`XDG_*` → 会话目录 `script-env/`；容器内可用性取决于容器是否授予 user namespace，见「数据与执行隔离·脚本运行根」 | `auto` |
| `GEBAI_PRELOAD_SUB_AGENTS` | 启动预载子Agent 名单（逗号分隔）：启动时注册其工具，**每个新会话创建时自动装载**（提示词 system 消息写入会话记录 + 工具注册）；为空 = 默认不预载任何子Agent | 空 |
| `GEBAI_SUB_AGENTS_ENABLE` | 子Agent **白名单**（逗号分隔）：非空时仅保留名单内子Agent（其余全部 `unregister`——`agent_list`/`agent_load`/`subsession_run`/系统提示词注入均不可见，热加载不复活）；与 `GEBAI_SUB_AGENTS_DISABLE` 同时配置**先白后黑**（黑名单最终生效）；名单存为实例策略、**合并视图每次重建时重放**（客卿迟到定义同样收敛） | 空（不裁剪） |
| `GEBAI_SUB_AGENTS_DISABLE` | 子Agent **黑名单**（逗号分隔）：名单内子Agent `unregister`（运行时能力面收敛，与构建期 `GEBAI_BUILD_SUBAGENTS` 打包裁剪互补）；名单未知名启动告警忽略不阻断 | 空 |
| `GEBAI_PROMPT_ENABLE` / `GEBAI_PROMPT_DISABLE` | **全局提示词段落白/黑名单**（逗号分隔的段落键，键表见「启动裁剪与领域专用模式」）：非空白名单仅注入名单内段落、黑名单移除名单内（先白后黑）；未知键启动告警 | 空（全段落注入） |
| `GEBAI_PROMPT_EXTRA` / `GEBAI_PROMPT_EXTRA_FILE` | **领域补充提示词**（内联文本 / 文件路径）：追加在裁剪后的全局提示词末尾；文件缺失启动即报错；`GEBAI_PROMPT_EXTRA_FILE` 相对路径相对进程 cwd | 空 |
| `GEBAI_PROFILE` | **领域专用模式档案**（档案名 → `{GEBAI_HOME}/profiles/{名}.json`；含路径分隔符或以 `.json` 结尾按文件路径）：一个文件同时声明提示词裁剪/补充、工具启停、子Agent 启停与预载（格式与优先级见「启动裁剪与领域专用模式」） | 空（不启用） |
| `GEBAI_UI_STYLE` | 默认 UI 风格（`acrylic`/`aether`/`matrix`/`tokyo-night`/`cyberpunk`/`synthwave`/`aurora`/`ink`/`cny`/`qinhan`）；**服务端白名单为这 10 项**（须与前端主题表 `packages/web/src/theme-core.ts` 同步——主题增删时两处不同步则该主题经环境变量设置会被静默回落为 `acrylic`，参见 `routes/static.ts` 的 `UI_STYLES`）；可被 URL/用户级覆盖 | `acrylic` |
| `GEBAI_TITLE` | 浏览器页面标题（品牌化定制）：设置后服务端渲染入口 HTML（`/` 与 `/files`）时替换 `<title>`（files 页保留「文件工作台 · 」前缀）并注入 `window.__GEBAI_TITLE__`（前端 `updateTitle()` 同源读取）；未设置走内置默认「歌白」。仅影响浏览器 tab 标题，PWA 安装名与站内品牌文字不跟随 | 空（「歌白」） |
| `GEBAI_LOG_LEVEL` | 日志级别：`debug`/`info`/`warn`/`error`——`loadConfig` 后由组合根 `setLogLevel(config.logLevel)` 生效（最小日志器 `@gebai/sdk/node` 的 `log.*`，见「日志系统」）；非法值忽略保持原级别 | `info` |
| `GEBAI_TOOL_ENABLE` | 工具白名单（逗号分隔，配置后仅启用列表内工具；支持 `{agent}_*` 通配）；**注册时判定**——运行期装载/热加载进来的子Agent 工具同样受约束 | 空（全部启用） |
| `GEBAI_TOOL_DISABLE` | 工具黑名单（逗号分隔，排除指定工具；支持 `{agent}_*` 通配），与白名单同时配置**先白后黑** | 空 |
| `GEBAI_FEISHU_*` | 飞书集成配置：全局应用凭证 `GEBAI_FEISHU_APP_ID` / `GEBAI_FEISHU_APP_SECRET`（`feishu_docs`/`feishu_group` 子Agent 的全局兜底 + 机器人桥接凭证 + 任务飞书应用消息通知（指定群 chat_id 推送/@人））、云文档目标文件夹 `GEBAI_FEISHU_FOLDER_URL`（`feishu_docs` 创建的资源落用户文件夹下、用户自动有权限；子Agent 前缀 `FEISHU_DOCS_FOLDER_URL` 优先）、机器人桥接开关 `GEBAI_FEISHU_BOT_ENABLED`（`true` 启用长连接事件订阅，见「飞书机器人集成」）、**机器人行为开关 `GEBAI_FEISHU_BOT_NOTIFY_TOOLS`（工具调用过程滚动状态消息）/ `GEBAI_FEISHU_BOT_NOTIFY_ASSISTANT`（助手中间轮文本预览）/ `GEBAI_FEISHU_BOT_AUTO_APPROVE`（需审批工具自动通过，见「飞书机器人集成 → 配置」）**、TLS 策略 `GEBAI_FEISHU_INSECURE_TLS`（`true`/`1` 时所有飞书出站请求禁用证书校验——内网代理场景：机器人桥接 REST/长连接 WebSocket、`feishu_docs` 子Agent 接口与 OAuth 回调兑换，见「飞书 TLS 策略」）；`feishu_group` 专属前缀 `FEISHU_GROUP_APP_ID`/`FEISHU_GROUP_APP_SECRET` 可独立配置 | 不启用 |
| `GEBAI_PUBLIC_URL` | 对外可访问地址（如 `http://localhost:3000` 或公网域名）：飞书用户授权（user_access_token）自动回调默认取 `{GEBAI_PUBLIC_URL}/api/v1/oauth/feishu/callback`（缺省回落 `http://localhost:{GEBAI_PORT|3000}`），需在开发者后台「安全设置 → 重定向 URL」登记 | 空（回落 localhost） |
| `GEBAI_SELF_MODIFY` | 是否允许 `self_optimize` 修改服务端源码（`true`/`false`） | `false` |
| `GEBAI_SAFE_MODE` | 安全模式（**仅启动时从 .env/环境变量加载，不可在会话/任务级修改**）：风险能力**降级而非禁用**——`sh` 只读命令白名单 + 重定向限用户目录、`py`/`js` 只读运行时（写/进程/网络屏蔽，仅保留文件读取）、`write`/`edit`/`file` 限定用户目录内、`task_*` 调度类硬阻断、子Agent 工具按 `Tool.safeMode` 自主声明过滤注册（详见「安全模型 → 安全模式」） | `false` |
| `GEBAI_LLM_MODEL` / `OPENAI_*` | LLM Provider 与模型配置 | - |
| `GEBAI_LLM_API_BASE` / `GEBAI_LLM_API_KEY` | LLM 服务地址与密钥（等价 `OPENAI_*`） | - |
| `GEBAI_LLM_API_KIND` | LLM 接口类型：`openai`（兼容 chat/completions）/ `responses`（OpenAI Responses API）/ `anthropic` | `openai` |
| `GEBAI_LLM_ROUTES` | **命名模型路由表**（JSON 对象，`subsession_run` 多路接口用）：`{"路由名": {"model": "...", "api_base"?: "...", "api_key"?, "api_kind"?, "max_context"?}}`——子会话运行的 `model` 参数按路由名走独立端点/模型（未指定的项沿用主配置基准），多子会话多路并行分摊单路限流；未命中路由名视为字面模型名；非法 JSON 静默忽略；会话/任务级 env 同样生效（见「子会话运行」） | 空（分支沿用任务级模型） |
| `GEBAI_LLM_EXTRA_PARAMS` | 额外模型接口参数（JSON 对象，如 `{"reasoning_effort":"high"}`），每次请求顶层合并进请求体；非法 JSON 启动即报错 | 空 |
| `GEBAI_LLM_MAX_OUTPUT_TOKENS` | 单次响应输出 token 上限（大文件生成截断防护）：模型输出超限被截断会致工具参数 JSON 不完整——引擎检测截断并抢救落盘 + 引导 `write append` 分段续写（见「核心Agent流程」）；未配置时 openai/responses 用服务端缺省、anthropic 用 8192（接口强制要求 max_tokens） | 空 |
| `GEBAI_LLM_MULTIMODAL` | 主模型多模态能力声明：`true` 时图片附件 base64 内联进消息，视觉分析可回落到主模型；默认 `false`（纯文本模型须配 `GEBAI_VISION_*` 外挂视觉模型；声明了但接口拒绝图片时引擎自动降级为文本说明） | `false` |
| `GEBAI_VISION_MODEL` | **额外多模态（视觉）模型**：配置后启用独立视觉 Provider（vision 子代理 `vision_analyze` 使用）；接口地址/密钥/类型缺省时继承主模型配置；不配置则回落到主模型（须声明多模态能力） | 空 |
| `GEBAI_VISION_API_BASE` / `GEBAI_VISION_API_KEY` | 视觉模型接口地址与密钥（缺省继承主模型） | - |
| `GEBAI_VISION_API_KIND` | 视觉模型接口类型：`openai`（兼容）/ `responses` / `anthropic` | 同主模型 |
| `GEBAI_VISION_MAX_CONTEXT` | 视觉模型上下文 token 预算 | `128000` |
| `GEBAI_VISION_EXTRA_PARAMS` | 视觉模型额外请求体参数（JSON 对象，同 `GEBAI_LLM_EXTRA_PARAMS` 语义） | 空 |
| `GEBAI_CV_MODELS_DIR` | 本地 CV（`desktop_ocr`/`desktop_locate`）模型目录：含 `det.onnx`/`rec.onnx`/`dict.txt`（PP-OCR 中英文）三件套；任务级 env 可覆盖；未设置时按 二进制形态释放的内嵌模型目录 → `{GEBAI_HOME}/resources/models/cv/ocr/`（资源目录）解析（见「小模型识别」） | 空 |
| `GEBAI_CV_MAX_SIDE` | 本地 CV 输入图像降采样上限（像素长边，超过等比缩小后识别；坐标仍映射回原始像素系） | `1280` |
| `GEBAI_CV_DETECT_MODEL` / `GEBAI_CV_DETECT_LABELS` | `desktop_detect` 自备 YOLO ONNX 模型路径与标签文件路径（每行一个类别；**MODEL 缺省自动发现 `{GEBAI_HOME}/resources/models/cv/detect/` 下唯一 `.onnx`——资源目录 drop-in 即用，多个时列出候选要求显式指定**；模型不内嵌，COCO 类别对 UI 无意义；ultralytics 导出的 ONNX 内嵌 imgsz/names 元数据自动读取——LABELS 与 SIZE 可省） | 空 |
| `GEBAI_CV_DETECT_SIZE` | 检测输入边长覆盖（像素，320-4096；缺省取模型元数据 `imgsz`，再回落 graph 首输入静态形状，末级 640） | 空 |
| `GEBAI_CV_BACKEND` | **CV 推理后端全局开关（检测与 OCR 共用）**：`auto`（默认，GPU sidecar 可用即用、失败回落 wasm 并毒化）/ `sidecar`（强制 node 原生，失败不回落）/ `wasm`（强制进程内单线程 CPU） | `auto` |
| `GEBAI_CV_DETECT_BACKEND` / `GEBAI_CV_OCR_BACKEND` | 细分覆盖（优先于全局）：检测/OCR 各自独立选择后端，取值同 `GEBAI_CV_BACKEND` | 空 |
| `GEBAI_CV_EP` | GPU sidecar 执行提供者（全部 CV 推理共用）：`auto`（默认，Windows dml→cuda / macOS coreml→cuda 逐级探测、全失败回落 cpu 兜底并如实上报）/ `dml` / `cuda` / `coreml` / `cpu`（`GEBAI_CV_DETECT_EP` 为早期别名兼容） | `auto` |
| `GEBAI_CV_REC_BATCH` | OCR rec 批处理大小（1..32，缺省 8）：文本行裁剪经 letterbox 后形状一致，拼批一次推理——逐行推理次数 = 文本行数，批处理把这些次数整除并省掉每行 IPC 往返（详见「小模型识别」→「OCR rec 批处理」） | `8` |
| `GEBAI_CV_ORT_NODE_DIR` | onnxruntime-node 解析目录（包根或其父目录；缺省按 `{GEBAI_HOME}/resources/vendor/node_modules/onnxruntime-node`（**资源目录约定位置，放入即生效**——模型/运行时依赖集中存放于独立 git 仓库 `resources/`，见其 README）→ `{GEBAI_HOME}/vendor` → node_modules 解析；不可用时检测自动回落 wasm，见「小模型识别」检测分层后端） | 空 |
| `GEBAI_KEQING` | 客卿总开关：`off`/`false`/`0` 显式禁用（发现器整体不启动，客卿子代理完全不可见）；沙箱启用（服务端部署形态）同样自动禁用——客卿仅限本地使用（见「客卿」） | 空（本地形态默认启用） |
| `GEBAI_PYTHON_DIR` | 内置 python 子代理的 `{python}` 占位解析优先项（目录含解释器）；缺省按 `{GEBAI_HOME}/venv` → 系统 PATH 顺序解析 | 空 |
| `GEBAI_SIGNUP_MODE` | 注册审批模式：`open`（默认，注册即用）/ `approval`（注册待 admin 审批——用户置 `disabled+pending` 待审、不可登录，admin 在用户管理页批准/拒绝） | `open` |
| `GEBAI_SIGNUP_SOURCE` | 注册来源限制：`any`（默认，内置登录页与二开前端均可注册）/ `custom`（**仅二开前端代码可注册**——注册请求须携带 `X-GEBAI-Signup-Source: custom` 头，内置登录页 SDK 调用不带该头被 403 拒绝；前端经 `GET /api/v1/auth/signup-config` 探测 `builtinAllowed=false` 后自动隐藏「注册账号」入口；与二开配置 `allowSignup: false` 语义配套，见 custom/README.md「前端脚本」） | `any` |
| `GEBAI_APPROVAL_SKIP` | 会话级审批跳过（等价 `/approval-skip`，`true` 跳过） | 空 |
| `GEBAI_TASKS_ENABLED` | 是否启用**统一任务能力**（注册 `task` 子Agent（`task_add`/`task_list`/`task_update`/`task_run`/`task_cancel`/`task_remove`/`task_files`/`task_notify` 工具）、启动任务调度器并开放 REST `/api/v1/tasks` 管理面；`false` 时子Agent 不注册、调度器不启动、REST 返回 503，能力完全不可见） | `true` |
| `GEBAI_TASK_MAX_CONCURRENT` | 每用户同时运行的**任务会话数**（定时/普通任务并行上限；闲时任务另受「队列空闲 + 每用户仅 1 个」约束；运行中的任务不因额度不足被中断——新任务排队等待；非法值回落 5） | `5` |
| `GEBAI_IDLE_TODO_ENABLED` | 是否启用**用户级待办**（标题栏轮盘「待办」弹窗 + REST `/api/v1/todos` 管理面；待办随时可手动执行（入队按序跑一次），开启 ⚡ 闲时自动执行的条目绑定一个闲时任务、由任务调度器在队列空闲且无运行中会话时执行）；`false` 时 REST 返回 503 | `true` |
| `GEBAI_SCHEDULER` | 调度器门控（同一 `GEBAI_HOME` 下的跨进程互斥）：`auto`（默认）=主实例锁判定，同库只有一个实例跑任务队列与待办闲时任务，其余退化为从实例（只服务 HTTP/WS，看门狗在主实例退出/租约过期后自动接管）；`on`=强制本实例跑调度（忽略锁，多实例同时 `on` 会重复调度）；`off`=完全不跑调度 | `auto` |
| `GEBAI_FS_ENABLED` | 是否启用**文件工作台**（`/files` 页面与 `/api/v1/fs`、`/api/v1/git`、`/api/v1/roots` 端点）；`false` 时页面 404、端点全部 404、标题栏入口按钮隐藏 | `true` |
| `GEBAI_FS_WRITE` | 工作台写开关：`false` 时纯只读检视（新建/改名/移动/复制/删除/上传/保存全部拒绝；Git 写操作另由下方开关控制） | `true` |
| `GEBAI_FS_WATCH` | 工作台**变更监听**（自动刷新）的 `fs.watch` 开关：`false` 时 `/api/v1/fs/watch` 立即返回 `enabled:false`，前端退化为固定间隔的纯轮询（页面仍会自己变，只是慢一拍且每轮都有实际列举开销） | `true` |
| `GEBAI_LSP` | 工作台**语言服务器（LSP）**开关：`false` 时清单回 `enabled:false` 且 `lsp.*` 消息一律拒绝，不起任何外部语言服务器（Monaco 内置语言服务与符号提取照旧） | `true` |
| `GEBAI_LSP_SERVERS` | 语言服务器覆盖表（JSON 对象，键 = Monaco 语言 id；值 = 命令 / `{command,args}` / 候选数组 / `null` 关闭）；显式列出即启用（含默认不启用的 typescript/json/css/html——那几种已由 Monaco 内置语言服务覆盖） | 不设置（用内置表） |
| `GEBAI_LSP_IDLE_MS` | 语言服务器空闲回收（毫秒）：**无打开文档**的常驻进程超时即退出 | `600000` |
| `GEBAI_LSP_MAX` | 并发语言服务器进程上限（超标时先回收空闲会话，仍满则拒绝新文档） | `4` |
| `GEBAI_FS_ROOTS` | 服务模式下的额外白名单根（JSON 数组：字符串或 `{name,path,description,writable}`）；本地模式自动追加主目录/服务工作目录/盘符，无需配置 | 不设置 |
| `GEBAI_FS_MAX_READ` / `GEBAI_FS_MAX_WRITE` / `GEBAI_FS_MAX_UPLOAD` / `GEBAI_FS_MAX_ZIP` | 工作台单次读取/写入/上传单文件/打包下载上限（字节） | 10MB / 10MB / 100MB / 500MB |
| `GEBAI_FS_MAX_DIFF` / `GEBAI_GIT_MAX_FILE` | 单次 diff 文本总量上限（字节）/ 单个文本文件（内容与逐行差异）上限（字符）——超限降级为「统计 + truncated/tooLarge 标记 + UI 提示」 | 8MB / 1M |
| `GEBAI_FS_HIDDEN` | 目录树默认是否列出隐藏文件（前端在「更多」菜单里可随时切换；请求显式带 `showHidden` 时以该参数为准） | `true` |
| `GEBAI_FS_AUDIT` | 工作台写操作审计（`{GEBAI_HOME}/audit-fs.jsonl`：谁、哪个根、什么动作、成败）——工作台写操作是用户本人直操、不走工具审批，审计是它的留痕等价物 | `true` |
| `GEBAI_GIT_WRITE` | Git 写操作开关（暂存/提交/分支/合并/变基/重置/标签/暂存区/丢弃）；`false` 时仅保留只读查看与对比 | `true` |
| `GEBAI_GIT_REMOTE` | Git 远程操作开关（fetch/pull/push/remote，需网络与凭据） | `true` |
| `GEBAI_TASK_NOTIFY_WEBHOOK` | 任务**全局默认通知 webhook**（http(s) 回调 URL）：任务未配置自己的 `notify` 时自动经该通道推送（任务自配则不叠加）；启动时校验（SSRF 同规则），非法配置告警忽略 | 不设置 |
| `GEBAI_TASK_NOTIFY_FEISHU` | 任务**全局默认飞书通知**：群 chat_id（`oc_` 前缀，以应用身份推送——需 `GEBAI_FEISHU_APP_ID/SECRET`）或群机器人 webhook URL，语义同 `feishu` 通道双形态；任务未配置 `notify` 时自动生效 | 不设置 |
| `GEBAI_EXTERNAL_AUTH_SECRET` | 外部身份扩展点：HMAC 共享密钥（与 `GEBAI_EXTERNAL_AUTH_URL` 互斥，同设启动报错）；网站用密钥对「用户名.过期时间戳」签名（HMAC-SHA256，hex），凭证格式 `{exp}.{sig}`，exp 为毫秒时间戳，±10 分钟有效窗口防重放 | 空（不启用） |
| `GEBAI_EXTERNAL_AUTH_URL` | 外部身份扩展点：HTTP 回调验证 URL（与 `GEBAI_EXTERNAL_AUTH_SECRET` 互斥，同设启动报错；**必须 HTTPS**，localhost/127.0.0.1 例外防中间人伪造）；GEBAI 把 `{username, credential}` POST 给回调（5s 超时），业务系统自行校验（如查自己 localStorage 对应的服务端态），**必须核验 username 与凭证归属一致**，响应 2xx 且 `{"ok":true}` 即通过，可用 `username` 字段覆盖映射（仅应在明确校验后使用） | 空（不启用） |
| `GEBAI_EXTERNAL_AUTH_AUTOCREATE` | 外部用户名不存在时自动创建 GEBAI 用户（普通角色、随机密码不可密码登录）；`false` 时仅允许管理员预建的同名用户 | `true` |
| `GEBAI_EXTERNAL_AUTH_STORAGE_KEY` | Web UI 同源直读宿主 localStorage 的凭证 key（值支持 JSON `{"username","credential"}` 或 `"username:credential"` 字符串）；不设则仅支持 URL 参数注入 | 空 |
| `GEBAI_CREDENTIAL_SOURCES` | REST 凭证来源链（逗号分隔）：`bearer` / `basic` / `cookie:<名>` / `header:<名>`；非法项启动报错。不设 = `bearer,basic,cookie:gebai.auth.token`（cookie 仅 GET/HEAD）；设置为空值即只用代码级来源（`custom/auth/`） | 空 |
| `--server` | 开启服务模式（等价 `GEBAI_MODE=server`，参数优先） | - |

> 以上为**全局层**环境变量（进程注入），会话层可覆盖其中可运行时变更的项（**模型/Provider 配置全量可覆盖**：`GEBAI_LLM_MODEL`/`API_BASE`/`API_KEY`/`API_KIND`/`MAX_CONTEXT`/`MAX_OUTPUT_TOKENS`/`MULTIMODAL`/`ROUTES` 与 `GEBAI_VISION_*` 任务级生效，按任务重建 Provider，见「环境变量配置」）。

#### 启动裁剪与领域专用模式

**一个机制：启动时按声明裁剪能力面。** 全局提示词、全局工具、子Agent、子Agent 预载四组清单同口径（**白名单非空仅保留名单内、黑名单移除名单内，先白后黑**），在**进程启动时**生效、对所有用户/会话共享；**领域专用模式** = 把一组裁剪声明固化成一个可复用档案，启动时一个变量切换。

| 维度 | 环境变量 | 裁剪对象 | 生效点 |
|------|---------|---------|--------|
| **全局提示词** | `GEBAI_PROMPT_ENABLE` / `GEBAI_PROMPT_DISABLE` | 全局系统提示词的命名段落（键表见下） | `buildSystemPrompt`：每次构建提示词时按键过滤 |
| **领域补充提示词** | `GEBAI_PROMPT_EXTRA` / `GEBAI_PROMPT_EXTRA_FILE` | 追加在裁剪后提示词末尾的领域约束（不参与裁剪） | 同上 |
| **全局工具** | `GEBAI_TOOL_ENABLE` / `GEBAI_TOOL_DISABLE` | 工具注册表（全局工具 + 子Agent `{agent}_*`，支持 `{agent}_*` 通配） | `ToolRegistry.setPolicy`：**注册时判定**——运行期装载/热加载进来的工具同样受约束（一次性快照会让白名单在装载后失效） |
| **子Agent** | `GEBAI_SUB_AGENTS_ENABLE` / `GEBAI_SUB_AGENTS_DISABLE` | 子Agent 定义可见性（`agent_list`/装载/子会话运行/提示词注入） | `SubAgentManager.applyEnableDisable`：名单存为实例策略、**合并视图每次重建时重放**（客卿迟到定义、热加载重扫同样收敛） |
| **子Agent 预载** | `GEBAI_PRELOAD_SUB_AGENTS` | 启动即装载的子Agent（工具进工具集、提示词进会话记录） | `discover()` 尾部 + **客卿发现就绪后补跑**（客卿定义迟到，早期预载看不到它们） |
| **领域档案** | `GEBAI_PROFILE` | 上述全部清单的一个命名集合 | `loadConfig`：启动期解析并作为**默认值**，显式环境变量按字段覆盖 |

- **提示词段落键**（`GEBAI_PROMPT_ENABLE`/`GEBAI_PROMPT_DISABLE` 取值；未知键启动告警）：`persona`（身份与智体概念模型）、`workspace`（会话工作目录 + 沙箱注记）、`channel`（通道环境注记）、`safe_mode`（安全模式注记）、`orchestration`（js 脚本编排指引）、`batching`（并行工具调用指引）、`planning`（重大任务计划审批）、`artifact_naming`（产物命名纪律）、`agent_routing`（子Agent 用法路由）、`parallel_sessions`（并行多路推进）、`project_bindings`（子Agent 项目绑定）、`builtin_projects`（内置项目）、`subagent_catalog`（可选子Agent 清单）。正文为空串的段落（如未启用安全模式的 `safe_mode`）本就不注入，裁剪它们无副作用——段落键清单以 `core/engine/prompt.ts` 的 `PROMPT_SECTION_KEYS` 为准
- **领域档案格式**：

```json
{
  "name": "coding",
  "description": "编码助手：极简提示词 + 只留代码类子Agent",
  "prompt": { "disable": ["artifact_naming", "builtin_projects"], "extra_file": "coding-prompt.md" },
  "tools": { "disable": ["sh", "py"] },
  "sub_agents": { "enable": ["code", "explore", "self_optimize"], "preload": ["code"] }
}
```

- **优先级**：显式环境变量 > 领域档案 > 缺省（不裁剪）；同一组清单**整体**取一个来源——如 `sub_agents.preload` 与 `GEBAI_PRELOAD_SUB_AGENTS` 同时声明时后者生效（档案的 `prompt.extra`/`extra_file` 同理，环境变量源存在时档案补充提示词不生效）；同一来源内**文件内容在前、内联文本在后**
- **路径基准**：档案 `prompt.extra_file` 的相对路径相对**档案文件所在目录**（档案与提示词文件可整体迁移）；`GEBAI_PROMPT_EXTRA_FILE` 的相对路径相对进程 cwd；档案或提示词文件缺失、JSON 非法、缺 `name`、含未知字段、字段类型不符 → **启动即报错**（配错档案却按默认能力面运行比启动失败难排查）
- **未知名告警**：提示词段落键未知 → 启动告警；子Agent 启停/预载名单中的未知名 → 启动告警（客卿名单的未知名推迟到客卿发现就绪后判定，避免把正常配置报成拼写错误）
- **与运行时开关的关系**：REST `PATCH /api/v1/tools` 与 WS 工具启停仍可事后改单个工具（策略只定注册基线），但**重启后回到裁剪后的能力面**——部署方声明是启动级事实，运行期开关是临时调整
- **与构建期裁剪的关系**：`GEBAI_BUILD_SUBAGENTS`/`GEBAI_BUILD_EXCLUDE_TOOLS` 在**构建期**把能力移出产物（体积收益、不可运行时恢复）；本节裁剪在**运行时**收敛能力面（不改产物，重启改配置即恢复）

### 服务端
- 服务端通过 WebSocket 与客户端通信，Agent 能力（Chat、工具调用、审批等）均在服务端内部实现
- **单端口暴露**：静态 Web UI、WebSocket（`/ws`）、REST API（`/api/*`）全部由同一端口承载，无多端口部署需求
- 对外提供双通道 API：**WebSocket**（实时双向流式）与 **REST HTTP**（同步请求，业务系统集成）
- 服务端内置 Web UI，通过浏览器可直接访问使用
- LLM 交互、子Agent管理、工具执行等逻辑全部封装在服务端内部
- 模型配置通过环境变量管理（`GEBAI_*` / `OPENAI_*`）；**服务端可不配置任何模型变量**——前端「设置 → 环境变量」配置（浏览器本地、不落盘），随每次发送消息临时注入，任务启动按合并后 env 重建 Provider（见「环境变量配置」）；未配置接口地址时 Provider 抛配置错误（`LLMConfigError`，不重试直接失败），错误信息给出前端配置入口指引
- 支持多种 LLM Provider：支持 OpenAI 兼容 `chat/completions`、OpenAI Responses（`/responses`）与 Anthropic `messages` 三类接口（均流式），通过统一的 `provider.chat()` 抽象封装，自行解析 SSE 流，不依赖第三方 AI SDK
- **额外模型接口参数**：支持自定义非标准请求体参数（如 `reasoning_effort` 推理强度、`temperature`、`thinking` 等），来源两级——Provider 级（`GEBAI_LLM_EXTRA_PARAMS` 环境变量，启动解析失败即报错）+ 任务级（浏览器本地注入同名环境变量，非法 JSON 静默忽略），后者优先，均顶层合并进请求体
- **任务级模型覆盖**：主模型与视觉模型配置（`GEBAI_LLM_*` 全套与 `GEBAI_VISION_*`）支持会话/任务级覆盖——启动时以进程环境变量固化基准配置（`core/llm/llm.ts` 的 `applyModelEnvOverrides`/`resolveVisionProvider`，boot/compose.ts 组装 `AgentEngineOptions.resolveProvider` 与 env 感知的视觉 getter），每次任务启动按合并后 env 解析 Provider（无覆盖键时沿用启动实例零开销）；覆盖项含模型名、接口地址、密钥、接口类型、上下文预算、多模态声明，未覆盖项继承启动配置；作用域覆盖主循环（含上下文压缩阈值/摘要与附件内联判定——**主动压缩（UI/REST 入口）未显式指定 Provider 时同样按合并后 env 解析**）与 `subsession_run` 子会话运行
- 多模态：`provider.chat()` 统一承载文本 + 图片消息（音频/视频为文件工作台查看能力，不投喂模型），按 Provider/模型能力自动组装各自消息格式（统一内部图片块 `{type:"image", mime, data}` → OpenAI 系（chat/completions 与 Responses）`image_url` data URL / Anthropic base64 `image` 块）
- **额外多模态（视觉）模型**：支持独立配置视觉模型（`GEBAI_VISION_*`，缺省继承主模型接口），供 vision 子代理的 `vision_analyze` 将图片文件交给视觉模型分析（目标 + 图片文件参数）；未配置时回落到主模型（须声明多模态能力）
- **模型能力声明**：`LLMProvider.capabilities()` 返回 `{ streaming, toolCalling, multimodal, maxContextTokens, maxOutputTokens }`——`maxOutputTokens`（单次响应输出上限）为上下文压缩的触发基准（窗口剩余必须足够支撑一次回复）；`maxContextTokens` 用于上下文占用判定
- 通过 `GEBAI_MODE`（默认 `local`；兼容旧 `GEBAI_AUTH`，或 CLI `--server`）环境变量切换运行形态：
  - **本地模式**（默认）：无需登录，直接以 **admin 用户**身份工作（**管理员超级权限 + 路径沙箱豁免**，不受任何权限限制），数据仍按用户目录存储
  - **服务模式**：启用登录鉴权（用户名/密码），密码仅存**加盐哈希**（scrypt，不落明文），会话按用户隔离，多用户公用同一服务端；**开放注册**（注册用户恒为普通角色；策略由 `GEBAI_SIGNUP_MODE` 控制，`open` 默认＝自助注册免审批，见「环境变量」表）；**admin 为特权用户**（不受用户权限限制），**唯一入口是 `GEBAI_ADMIN_PASSWORD_HASH`**（未配置时 admin 禁用，但普通用户可注册使用）
- 业务系统集成统一走**账号密码认证**：`POST /api/v1/auth/login` 获取令牌后以 `Authorization: Bearer <token>` 调用 REST；或**单次请求直接带 HTTP Basic**（`Authorization: Basic base64(username:password)`，等价隐式登录，复用密码校验与登录限流、不签发令牌）；WS 用 `auth.login { token }` 建立用户上下文——**无独立服务令牌**（原 `GEBAI_SERVICE_API_KEY` 服务身份机制已移除，避免任何服务端密钥进入 Agent 可达环境）
- **启动加载策略（重依赖惰性化）**：以「服务可用性优先」组织启动——`listening` 之前只加载引擎主循环与装配必需的模块，重第三方依赖一律延迟到首次实际使用（ESM 用 `await import()`，同步 API 用模块级单例 `require` 缓存）：
  - **图表渲染**（`core/support/diagram-render.ts`：echarts/happy-dom/mermaid/plantuml/d2）：引擎（ToolContext `renderDiagram`）与飞书桥接（`FeishuBot.rendererOf`）均惰性取，不在启动路径
  - **Office/PDF 解析与生成**（`agents/wps/*`：exceljs/docx/pptxgenjs/pdf-lib/fontkit/happy-dom）：各工具 `execute`（均 async）内引入；`@gebai/agents` 的 `renderOfficeReadingView` 经 `wps/excel.loadWorkbook()` 复用同一惰性入口（该导出位于 `@gebai/agents` 入口，被 `routes/fs.ts`/`routes/session-files.ts` 经 `await import("@gebai/agents")` 动态引用，不得静态拖入重依赖）
  - **内嵌 Web UI bundle**（`core/web.bundle.generated.ts`，数十 MB）：仅二进制模式读取内嵌资源时加载（`routes/static.ts` 的 `embeddedWebAssets()`）；源码/dev 形态走 `webDist`，不进启动路径
  - **飞书机器人长连接**：监听建立后异步发起（`boot/serve.ts` 于 `listening` 之后 `void feishuBot.start()`，握手失败只记 error 日志、服务照常可用；凭证缺失仍在 compose 装配期抛错）；`new FeishuBot` 构造不触发网络
  - 子Agent 定义模块（`discover()` 逐一 import 以取 `def`）遵循同一约定：**模块顶层不静态引入重第三方库**，实现内延迟到首次工具调用（`def` 的 name/description/tools 组装只依赖轻量模块）

### 核心模块与接口

服务端按接口解耦，全部依赖注入、可独立测试：

```
                    ┌──────────────┐
   WS/Webhook  │  API 层      │  (Hono 路由、鉴权、CORS)
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
   ┌──────┐        │ AgentEngine  │        ┌────────────┐
   │ LLM  │◄──────►│  (核心循环)   │◄──────►│ ToolRegistry│
   │Prov. │        └──────┬───────┘        └────────────┘
   └──────┘               │
              ┌───────────┼───────────┐
       ┌──────▼───┐ ┌─────▼────┐ ┌────▼─────┐
       │SessionStore│ │EnvManager│ │EventBus │
       └──────────┘ └──────────┘ └──────────┘
```

| 接口 | 职责 | 关键方法 |
|------|------|---------|
| `LLMProvider` | 三类接口统一抽象、多模态组装、流式解析；**多模态内容块转换**：统一内部图片块 `{type:"image", mime, data}`（base64）按接口规范转换（OpenAI 系 `image_url` data URL、Anthropic base64 `image` 块），`imageMessageBlocks()` 助手构造文本+图片消息；**Responses API**：消息转 `input`（assistant 工具调用拆独立 `function_call` item + `function_call_output`），工具扁平格式 `{type:"function",name,description,parameters}`，流式事件解析（`output_item.added`/`function_call_arguments.delta|done`/`output_text.delta`/`reasoning_*_text.delta`/`completed`，stop reason 取末条 message `finish_reason`）；**额外模型接口参数**：Provider 级（`GEBAI_LLM_EXTRA_PARAMS`）与调用级（`ChatOptions.extraParams`）请求体参数顶层合并（后者优先）；**接口健壮性**：fetch 层对网络错误/429/5xx 指数退避重试（2 次，500ms 基数，退避可被取消），4xx 与 AbortError 不重试，错误响应体截断 200 字符 | `chat(messages, opts): AsyncIterable<Chunk>`、`capabilities()` |
| `AgentEngine` | 主循环状态机：工具循环（**同批工具并行执行**，见「核心Agent流程」）/审批/重试/压缩/取消；**模型调用健壮性**：空响应（无文本且无工具调用，含只思考未输出）与无产出异常经 `callModel` 指数退避重试（2 次，800ms 基数，注入提示引导），已有产出后断流不重试（避免重复输出），耗尽抛中文错误；**重试过程前端可见**——每次将重试的模型服务异常推送 `event.model.error`（非终态，前端渲染为消息流内**常驻**异常记录，见「事件清单」）；**重复检测**：最近 8 次工具调用签名（工具名+参数 JSON）滚动窗口，相同签名在窗口尾部**连续**出现第 3 次（其间无任何其他调用）才中断执行并注入引导提示（间隔其他调用后重发同签名是「改动后复查」，不判重复），中断超 2 次终止工具循环（避免模型无效重复；同批重复签名只计一次——同批相同调用是有意扇出，跨轮连续重发才累积）；待办续做回复与上轮完全相同（纯文本）时在模拟 todo 查询结果尾部附防复述提示；**会话级已读文件追踪**（fileGuard，防盲写守卫，见「防盲写守卫」） | `run(sessionId, user, prompt, opts)`、`cancel(sessionId)` |
| `ToolRegistry` | 工具注册/命名空间解析/启停/审批声明 | `register(tool)`、`resolve(name)`、`list()` |
| `SessionStore` | 会话/消息/待办/附件持久化（分片路径） | `load(id)`、`save(session)`、`appendMessage()` |
| `EnvManager` | 环境变量合并：全局（进程 env）+ 会话内存态（不落盘）；用户环境变量只存浏览器本地、随 prompt 任务级注入 | `resolve(sessionId, user): Promise<Record<string,string>>` |
| `AuthService` | 用户认证、令牌、归属校验、API Key | `login()`、`authorize(ws/api)` |
| `Sandbox` | 路径沙箱、子进程执行、超时控制（支持取消信号：abort 按进程树终止——Unix 进程组 SIGKILL、Windows `taskkill /T`） | `resolvePath(user, path)`、`exec(cmd, opts)` |
| `EventBus` | 服务端事件分发（WS/Webhook/飞书） | `publish(event)`、`subscribe()` |
| `ContextCompressor` | 上下文压缩与截断落盘（水位触发 / 溢出恢复） | `compactSession(sessionId, user, scope?, provider, opts?)` |

#### 服务端目录结构（领域分层）

`packages/server/src/` 按领域分目录组织，每目录内聚且文件数受控（无大目录平铺）：

```
src/
  index.ts          # 薄入口：startServer = compose + serve；进程主命令分发见 boot/cli.ts
  app.ts            # Hono 应用工厂（CORS/鉴权中间件 + 各域路由装配 + AppDeps）
  boot/             # 启动装配：compose.ts（DI 组合根）/ serve.ts（Bun.serve + WS 运行时）/ cli.ts（exec 子命令与 web dist 自动构建）
  routes/           # REST 路由按域拆分（auth/users/sessions/session-files/tools/tasks/todos/docs/static + **文件工作台：fs/fs-shared/roots/git** + misc(feedback+webhook)），
                    #   每文件 register{Domain}Routes(rc)，契约见 routes/context.ts；装配顺序：sessions 先于 session-files（共享 :id 白名单中间件），fs 先于 git（共享 fs-shared 的根解析与审计）
  ws-handlers/      # WS 消息处理器按域拆分（auth/session/prompt/interaction/admin），每文件导出 Record<消息类型, WsHandler>；
                    #   ws.ts 为分发器（未登录守卫/会话 id 白名单/异常兜底）
  core/             # 领域核心（根目录只放构建生成物，源码按域入子目录）：
    base/           #   基础层：types/config/paths/event-bus/registry + 文本行/补丁纯算法
    llm/            #   LLM 接入（Provider 工厂与三接口实现）+ 多模态解析
    engine/         #   引擎：engine.ts（主循环）+ interactions.ts（五种阻塞交互的等待/决策）+ compressor.ts（上下文压缩/溢出恢复）+ prompt.ts（系统提示词构建）
    tools/          #   全局工具域：注册文件（fs/exec/show/agent/interact/schemas/restart/projects）+ shared.ts（GlobalToolEntry 契约与 schema/parseRegion 助手）+ index.ts（聚合器/barrel）。本地识别共享工厂（cv-analysis）与 vision 工厂已迁 @gebai/agents core/shared，经 registry 直调消费
    support/        #   工具与引擎共用支撑：truncate/walk/artifacts/plan/exec-opts/fetch-scope/diagram-render（analyzer 与 image-resize 已迁 @gebai/agents）
    session/        #   会话域：store（持久化）/env/subsessions（子会话运行）/gc
        schedule/       # 统一任务与用户级待办：tasks（定时/普通/闲时任务 + 队列与执行）+ expr（执行表达式）+ notify（通知通道）+ todos（用户级待办清单与闲时任务绑定）
    exec/           #   脚本执行：js-tool/sh-tasks
    browser/        #   仅剩 fetch-proxy（透明浏览器代理垫片，供 fetch_url/SSRF 路径共用）；完整桥接基建（bridge.ts + driver.mjs + pwcore 内嵌产物）在 @gebai/agents core/browser——playwright/reverse_site 子Agent 与浏览器代理共用的平台级底座，不依赖子Agent 定义存在
    cv/             #   （已整体迁至 @gebai/agents core/cv）本地 CV 推理基建：ort-loader + cv + image/ocr/detect + onnx-meta + template + sidecar/cv-driver.mjs，内嵌产物 cv.embedded.generated.json 同目录——desktop/playwright 子Agent 本地识别的底座（工具消费层=agents core/shared/cv-analysis 共享工厂）
    fs/             #   文件工作台：roots（`sess:`/`proj:`/`user:`/`abs:` 根解析 + 白名单 + 路径防护）/service（列举与自然序、二进制与编码探测、文本读取、Range、打包 ZIP、内容搜索）/write（受保护的写：etag 乐观锁、编码回写、删除即物理删除）/archive（零依赖 ZIP 读）/mime（类型→查看器/语言）/audit（写操作审计）
    git/            #   Git 图形化：service.ts（git CLI 封装：状态/日志/分支/标签/暂存/远程 + **任意两端 compare/diff/contentAt**（WORKTREE/INDEX/任意 rev/mergeBase）+ 写操作串行化与自动备份）
    security/       #   安全：sandbox/safety/ratelimit（ip/fetch-guard 的 SSRF 防护已迁 @gebai/agents core/shared，server 侧经包导入）
    agents/         #   子Agent 装载器：subagents（扫描/热加载）/keqing（客卿 sidecar）/merge/env-catalog（sub-agent-md 已迁 @gebai/agents core/shared）
  feishu-bot/       # 飞书对话桥接
```

**分层规则**：依赖单向——`base` ← `support`/`security` ← `tools`/`exec`/`session`/`schedule` ← `engine` ← `routes`/`ws-handlers` ← `boot`。`core/` 内部模块**禁止 import `core/tools` 聚合 barrel**（聚合器含目录扫描顶层 await，反向依赖会成环导致未初始化绑定）——共用能力直引 `core/support/*`、`core/security/*` 叶子模块；子 Agent 与顶层（routes/boot）可经 barrel 取既有导出。`checkWebhookUrl`/`fetchWithRedirectGuard` 等 SSRF 校验统一在 `@gebai/agents` core/shared/fetch-guard（server 侧经包导入，不新增越层依赖）。

#### 全局工具零注册（文件级自动扩展）

全局工具与子 Agent 同为「丢文件即注册」的扩展模型（`core/tools/` 目录）：

- **注册契约**：目录内凡导出 `export const globalTools: GlobalToolEntry[]` 的文件即被收集——`GlobalToolEntry = { name, tool: Tool | (() => Tool), project?: boolean | "workdir" }`（`project` 声明 `projectAware` 包装，聚合器统一施加）；`shared.ts` 定义契约，纯辅助文件（如 `projects.ts` 等不导出 globalTools 的）不参与注册
- **双通道**：dev 形态由 `core/tools/index.ts` 模块初始化时运行时扫描本目录（重启进程生效，无热加载——self_optimize 只写子 Agent 目录）；dist/`--compile` 形态回退 `packages/server/scripts/build-tools.ts` 生成的静态注册表 `core/tools/bundle.generated.ts`（gitignore）
- **聚合器**：`createAllGlobalTools()`/`createGlobalTools()`/`isGlobalToolExcluded()` 签名与语义不变（构建期排除、重名抛错、engine 与 index.ts 注册面零改动）；`core/tools/index.ts` barrel 具名再导出原单文件时代的全部既有导出（导入方零改动），新增工具请从所在文件直接 import
- 新增全局工具 = 在 `core/tools/` 新建一个导出 `globalTools` 的文件，不改任何中央注册表；命名空间专属工具（git/page_capture/read_feedback 等）落 `@gebai/agents` core/shared（原 server `core/tools/extras.ts` 已整体迁出），由子 Agent def 引用
- **模式限定工具**：个别全局工具仅特定运行形态注入（如 `restart_server` 仅本地模式——compose 注册处按 `config.auth` 过滤，服务模式多用户部署不暴露）；工具仍在全局表（本地模式可用面不变），过滤发生在 compose 注册环节

#### restart_server（重启自身服务，仅本地模式）

重启本服务进程的可靠性设计——「自杀后谁拉起」：重启的最大风险是旧进程死了新进程起不来（进程树连坐），方案是外部拉起器彻底脱离服务进程树：

- **外部拉起器（双平台）**：工具执行时把拉起脚本落盘 `{tmpdir}/gebai-restart/`，并部署为独立于服务进程树的进程：
  - Windows：PowerShell 脚本 `launcher.ps1`（**UTF-8 BOM**——Windows PowerShell 5.1 对无 BOM 文件按 ANSI 解析，脚本内中文会撕裂字符串），二段式启动：服务经短命令 `powershell Start-Process powershell -File launcher.ps1` 部署——外层 powershell（服务的短暂子进程）用 `Start-Process` 创建内层独立窗口进程执行拉起器后立即退出，链路断开、与原服务零亲缘（**WMIC 已废弃**：Win11 24H2+ 默认移除，旧版 wmic 方案在新系统上必然失败——拉起器从未启动，服务自杀后无人拉起；也不可直接 Bun.spawn powershell -File：Bun 子进程经 job object 连坐，服务退出时拉起器一起被杀）；
  - Linux/macOS：bash 脚本 `launcher.sh`（UTF-8 无 BOM，脚本内 mkdir -p 自建工作目录），经 `setsid` 启动（新会话新进程组，防 `kill -PGID` 整杀连坐），标准流重定向 /dev/null——服务退出后拉起器被 init 收养继续运行；
- **拉起器流程（两平台同构）**：等旧进程退出（最长 60s；win `Get-Process` / posix `kill -0`）→ 等端口释放（最长 30s；win TCP 探测 / posix `ss -ltn`）→ **端口占用分类后失败退出**（见下；不动无辜进程）→ 日志轮转（`server.log.*` → `*.prev`：`Start-Process -RedirectStandardOutput` 与 posix `>` 均为覆盖写，连续重启会吞掉上一次诊断日志）→ 同端口/同 cwd/同启动级环境变量（`GEBAI_PORT/HOST/HOME/MODE/BASE_PATH/DEV_RELOAD`，凭据类不复制——`.env` 由 loadConfig 自行加载；DEV_RELOAD 承载「`--reload` 启动的开发热刷新在重启后不丢」：argv 参数不会被拉起器复制，工具侧显式补 `GEBAI_DEV_RELOAD=1`）启动新服务（win `Start-Process` / posix `nohup &`，Windows 无 job object 连坐、posix 由 init 收养）→ 就绪探测（最长 90s；**端口属主 ≠ 旧 PID 且 HTTP 200 双条件**——单看 200 会在旧服务未死时误判；属主探测 win `Get-NetTCPConnection` / posix `ss -ltnp`）→ 写状态文件 `state.json`（成功=新 PID；失败=原因+日志尾部）
- **端口占用的两种情形（必须分开报，处置完全不同）**：端口释放的判据是 **socket 句柄引用计数归零**，不是「拥有者进程是否存活」——
  - `occupied`（真被占用）：属主 PID **存活**且不是本次要替换的旧进程 → 不抢端口、立即失败（先处置该进程或改用其他端口）；
  - `zombie-socket`（僵尸套接字）：属主 PID **已不存在**但端口仍 `LISTENING`（`netstat` 显示 PID、`tasklist` 查不到、`taskkill` 报「找不到进程」）——典型成因是 socket 句柄被其他进程**继承**，内核对象因引用计数未归零而不释放，**用户空间无法直接释放**；
  - 两种情形都**严格失败**（不自动换端口、不动无辜进程），但 state.json 写 `reason`/`ownerPid`，`status` 动作把 `reason` 展开为诊断与处置指引（“注销并重新登录”终止本会话全部进程、释放句柄，比重启电脑快 → 结束可能持有句柄的后代进程 → 重启电脑；或改用其他端口）。中止时先**还原轮转掉的日志**（`.prev` → 原位）**再读其尾部**写 `logTail`——顺序反了会读到已被轮转走的空文件，使 logTail 恒为空（最需诊断信息的场景反而丢信息）；Windows 读日志需显式 `-Encoding UTF8`（服务日志为 UTF-8，PS5.1 的 `Get-Content` 缺省按 ANSI 读会使中文诊断乱码）
- **为何不能只靠 TCP 探测判「端口可用」**：僵尸套接字的监听队列仍会接受连接（`connect` 成功），故 TCP 探测会“看到端口可用”而空跑满 30s——真正的准绳是能否 `bind`；探测只用于等旧进程退出，占用判定看属主及其**存活性**
- **自杀时序**：工具先布置拉起器，再延迟 2.5s `process.exit`（延迟内工具结果先送达飞书/WS，自杀在后）；拉起器部署失败则不退出（服务保持运行）。工具结果为 **`endsTask` 任务终结声明**（见「核心Agent流程」）：本轮任务就此结束——引擎不再把结果回灌模型、不发起下一轮调用，模型不必为注定不会执行的后续动作生成内容（重启前的收尾动作须在调用前完成），后续工作由 `prompt` 续跑接续
- **重启后续跑（`prompt` / `session` 参数）**：重启会中断在途任务、且进程死后无人「接着干」——工具在自杀前把续跑请求写 `{tmpdir}/gebai-restart/continue.json`（会话/用户/角色/提示词/时间戳；**写失败即中止本次重启**，不留「重启成功但指令丢失」的半成品；拉起器部署失败则清理请求）。新服务启动后由 `consumeRestartContinuation`（`index.ts` 监听建立后的后台任务；仅本地模式、非测试进程）消费：等 `state.json` 确认「本次启动由重启拉起器承接」（pid 匹配本进程；拉起器已判失败或等待超时则按兜底仍执行——提示词是调用方明确意图，重启失败后人工恢复正是最需它不丢的场景）→ **先删请求再执行**（一次性语义，重复/并发启动不双跑）→ 校验会话存在 → `engine.run(会话, 用户, 提示词)`：提示词原样落盘为 user 消息并跑起完整 agent 循环（模型据此继续工作，前端页面已自动刷新可见）。请求过期（>10 分钟）则丢弃不执行，防陈旧请求被无关启动误跑；结论写 `continue.result.json`（**先写 `ok:null`「续跑执行中」再跑、完成后覆盖**——续跑任务可能一直跑到下次重启，不先写则中途被杀时 status 无任何痕迹），日志前缀 `[restart] …`
- **状态可查**：`action=status` 读最近一次重启状态（不重启）并附续跑请求/续跑结果；失败时把 `state.json` 的结构化字段展开为**中文诊断 + 处置指引**（`occupied` / `zombie-socket` / `ready-timeout` 三类各给对应措辞，并保留原始 JSON 便于机器读）；新服务日志在同目录 `server.log.*`；状态文件一律 UTF-8 无 BOM（Windows 拉起器用 `[IO.File]::WriteAllText` 写入——PS5.1 的 `Set-Content -Encoding UTF8` 会写入 BOM，消费方按无 BOM 预期解析）；就绪探测走服务自身根路径（直连 127.0.0.1，不经反向代理）
- **平台依赖**：Windows 需 PowerShell（系统内置）；Linux 需 bash + ss（iproute2，各发行版标配）+ curl；`requiresApproval: true`（重启中断在途任务，须用户确认）
- **拉起器代码的“落后一代”特性**：拉起器脚本由**重启那一刻运行中的进程**在内存里生成，因此改动 `restart.ts`（拉起器生成逻辑本身）后，**本次重启生成的脚本仍是改动前的逻辑，需再重启一次才生效**（新进程从磁盘加载新代码，它生成的脚本才是新的）。
  - 影响面仅限本文件自身——改其他任何文件都不受影响（新进程直接读新源码）；但这很容易让「已验证生效」的结论落空（历史上已连续两次因此误判）。
  - 故 `status` 附一行**代码新鲜度**（`launcherCodeFreshness`：比较磁盘上 `restart.ts` 的 mtime 与本进程启动时刻，容差 1s 避同一秒误报）：落后时输出可执行的提醒（“需再重启一次”），最新时一行确认，二进制/编译形态无源码可比则如实标注「无法判定」。
  - 设计取舍：**不**改为「重启时从磁盘现读源码生成脚本」——现行为是“由已验证的代码生成脚本”的**最后已知良好**保护（否则改成一半的 `restart.ts` 会让拉起器本身失效，服务起不来且无法再改）；代价（多一次重启）用一个可靠的可观测提示消除即可

### 核心Agent流程

单次消息处理的主循环（Agent Loop），状态机驱动、全程可观测：

```
session.prompt → 组装上下文（历史+系统提示词+临时文件提示）
    → LLM 流式生成 → 输出文本增量实时推送
    → 模型请求工具调用?
        ├─ 否 → 生成完成，回复落库
        └─ 是 → 解析工具调用（必填参数校验：schema required 缺失即回传缺参提示，不执行）
            → 门控（按调用顺序串行：重复检测/路由解析/审批姿态判定，说明性结果直接回传）
            → 同批并行执行（每个调用独立走 审批等待→执行→落盘；并发上限见常量参考）
            → 结果注入上下文
            → 循环回 LLM 生成（直至无工具调用；轮次不设上限，失控防护见重复检测）
```

关键机制：

- **工具调用循环**：一次 `session.prompt` 内模型可多轮调用工具（轮次不设上限，见常量参考），每轮结果作为消息追加回 LLM，直至生成最终回复。**例外——任务终结声明**：工具结果带 `endsTask: true`（`ToolResult.endsTask`，如 `restart_server` 的重启分支）时本批结果照常执行/落盘/推送，但不再回灌模型——引擎结束任务循环直接收尾（同批任一工具声明即终结，本轮全部调用照常完成），待办续做与收尾验证提醒等续轮同样跳过；本轮文本已随 assistant(toolCalls) 落盘，不再重复落盘为最终回复。供「执行即终结本任务/本进程」类工具使用（模型不必为注定不会执行的后续动作生成内容，免去无意义的一轮模型调用）；只读分支（如 `action=status`）不声明，仍回灌模型继续分析；子会话循环同规则（只结束本运行的循环）
- **同批工具并行执行**：单次模型响应返回的多个工具调用**并行执行**（互不等待——免审批工具不等待同批审批项、慢工具不阻塞快工具）；执行前先按调用顺序**串行门控**（重复检测/参数抢救/路由解析（含自动装载）/审批姿态等判定全部先行完成，缺参/未知工具/禁用/安全拦截等说明性结果直接回传——坏调用不打扰用户审批，**并推送 call+result 事件对**保证运行时卡片实时可见，与落盘历史一致）。并发上限 `MAX_PARALLEL_TOOLS`（超出按序排队）；结果按完成先后落盘（配对按 toolCallId，顺序不影响接口合法性；落盘经 promise 链串行化防并发写交错）；单个调用出错（取消/异常）不中断同批其他调用，池排空后统一补齐占位再上抛（与原串行收口语义一致）；**需严格串行的操作由模型编排**（系统提示词指引）：有依赖/同文件写改的操作用 js 按序编排或拆分多轮，不依赖同批调用顺序（同批共享门控时刻——运行中变更的会话 env（如自动审批开关）自下一轮起生效，不影响已门控的同批项）。主循环与子会话运行循环同规则（子会话存档条目按完成先后追加，仅影响回放顺序）
- **流式体验**：LLM 文本增量、工具调用事件、审批请求、执行结果均以服务端事件实时推送客户端（见「事件推送」），UI 呈现完整过程
- **中断与取消**：客户端可随时取消当前任务，服务端停止本轮循环并保留已生成内容；**停止即时打断进行中的工具执行**（工具执行包装统一收口：任务取消信号传递到 `Sandbox.exec`，脚本类子进程按进程树终止——Unix 杀进程组、Windows `taskkill /T`，工具自身据此交回中断前的输出 + 中断标记）；**用户中断在工具返回里显式体现**——被中断调用的工具结果一律带统一标记 `[interrupted by user]` 与「已被用户取消（任务已停止）」，据此与执行超时、引擎错误区分——中断返回**一律附上该调用被中断时已执行的时间**（取消信号到达时刻计时，`elapsedNote`：秒级一位小数、分钟级带分秒，如「执行被用户中断（已执行 12.3 秒），未取得工具返回」；未及执行/审批等待中被取消的调用不报时长，与「执行中被停」相区分）：① 工具在宽限期（`TOOL_ABORT_GRACE_MS`，默认 300ms，可经 `cancelGraceMs` 注入）内交回自身中断返回（`[interrupted by user]` / `[interrupted]` 标记 + 中断前已产生的输出）时**并入工具结果**（部分输出不丢失，不再被占位文案覆盖丢弃）；② 宽限内未交回（不响应取消信号的挂起工具）以统一标记**即时收口**（取消等待不被拖慢，结果写明「未取得工具返回」）；③ 未及执行/未完成的调用（审批等待中被取消、超出并行上限排队中、本轮收口兜底）同样补写带标记的占位（保持 assistant/tool 配对完整）；④ 等待型工具（`ask` 选项/填值、`show` 渲染、`page_capture` 捕获）被中断时返回「用户中断」文案而非超时文案；**取消统一解开全部挂起等待**（审批/选择/画图/捕获的等待 promise 同步 resolve——仅 abort 信号不会中断 `await`，不 resolve 会让 runLoop 永久挂起、任务收尾不完成、`isRunning` 残留，下一次 prompt 被「task already running」拒绝，表现为**中断后要发两次才能继续**；四个等待函数同时监听取消/超时信号，子Agent 超时同样立即解开）；**审批拒绝同样停止会话生成**（不再让模型调整方案继续执行，前端静默收尾并清理该会话残留审批卡片，取消/拒绝不渲染错误气泡）
- **重复检测**：模型**背靠背重复**调用相同工具（工具名+参数相同，最近 8 次签名窗口**尾部连续**第 3 次——其间没有任何其他调用）时中断该次执行并注入引导提示（「直接重发只会得到相同结果，若在等待外部状态变化请把等待/重试并入同一次调用，否则改用其他方法或直接给出最终回答」）；**间隔其他调用后重发同签名不判重复**——工具结果取决于世界状态而非仅参数（改完文件重跑同一测试、外部变更后复查），间隔操作说明模型在响应结果，是合法的「复查确认」；中断超过 2 次判定模型陷入重复，终止工具循环提前返回（仍返回最后产出文本，保持会话消息序列完整）；待办续做中模型回复与上上轮完全相同（纯文本）时，模拟 todo 查询结果尾部追加「请勿复述」提示。**同批重复签名只计一次**（同批并行发出相同调用是有意扇出——发出时尚未见任何结果，不存在「无视结果重试」，见「同批工具并行执行」）；跨轮连续重发（已见过结果且其间无其他调用）照常累积判定
- **错误恢复**：LLM 请求失败/超时自动退避重试；工具执行失败将错误注入上下文，引导模型自行修正；**工具执行超时不结束任务**——`py`/`js` 由脚本执行超时（`timeout` 参数秒，默认 300、上限 540）杀进程并返回超时结果；`sh` 的 `timeout` 是**同步等待窗口**（默认 60、上限 120），到期转后台而非杀进程（见「sh 执行」）；引擎层 9 分钟兜底（`TOOL_TIMEOUT_MS`）覆盖不响应超时的工具（如网络请求挂起），超时均作为工具结果返回给模型自行调整方案
- **必填参数校验（两循环同规则）**：模型漏传工具参数时（schema `required` 声明的键缺失/null）**不执行工具**，直接回传「工具 X 缺少必填参数: …」——缺失值若进工具会被 `String(undefined)` 成字面量 `"undefined"` 落进路径解析，报出与真实原因无关的 ENOENT（如 `edit` 漏传 `path` → `tmp\undefined`），模型无法从报错定位到「少传了参数」；校验点在审批门**之前**（坏调用不打扰用户审批），主循环与子会话循环一致生效
- **大文件分段写入与截断抢救**：生成大文件时模型被迫单次输出全部内容，易触发接口超时与输出上限截断（参数 JSON 不完整）。三层防护——①**分段写入载体**：`write` 支持 `append:true` 追加模式，工具描述与 code 子Agent 提示词内置指引（约 300 行以上分多段写，每段 200~300 行），单次响应短、不再超时；②**截断检测**：provider 如实传出结束原因（OpenAI `finish_reason=length`、Anthropic `max_tokens`、Responses `response.incomplete`→`length`，不再吞为正常 stop），解析失败的工具参数携带原始全文（`toolCall.raw`，原仅 500 字符片段）；③**抢救落盘**：引擎对 write 类工具（`write`/`{agent}_write`）容错解析原始参数前缀（`salvageWriteArgs`：提取完整 `path` 与已生成 `content` 前缀，尾部不完整转义丢弃、非法转义放弃），先把已生成部分落盘并登记已读，工具结果引导模型 `append:true` 续写剩余部分（从已写入之后继续，不重新生成全量）——失败轮次转化为进度；未抢救（非 write 工具/前缀解析失败/防盲覆盖守卫拒绝）时回传截断专属引导（拆小操作/分段写入），不再笼统提示「重新输出合法 JSON」（重新整体输出只会再次截断）。单次响应输出上限可配（`GEBAI_LLM_MAX_OUTPUT_TOKENS` 任务级覆盖，Anthropic 缺省 8192）
- **防盲写守卫（已读追踪）**：`write` 覆盖/追加、`edit` 修改「已存在但本会话未读过」的文件均被拒绝（会话级 fileGuard 已读追踪，`read`/`edit`/`write` 成功即登记），引导模型先 `read` 再改——拒绝作为工具结果返回，模型一轮自愈，不中断任务（见「防盲写守卫」）
- **上下文管理**：窗口剩余不足以支撑一次回复时自动压缩（工具输出截断 → 旧消息摘要 → 溢出硬护栏，见「上下文保护」），也支持用户主动压缩
- **审批超时**：审批请求超时（见常量参考）自动拒绝该次调用并提示模型调整，避免任务悬挂

### 会话输入队列（前端实现）

会话任务运行期间到达的用户输入**排队等待，当前任务结束（完成/取消/出错）后自动按序发送执行**，无需等待或放弃输入。队列为**前端本地状态**（`packages/web/src/queue.ts`，按会话隔离、纯内存——服务端无感知、无协议扩展，刷新页面/关闭浏览器后队列丢失）：

- **入队**：运行中提交（Enter/发送；运行中发送按钮带草稿时为箭头形态）不打断当前任务，输入进本会话排队条（不进消息流、不发 WS）；附件在入队时即上传会话 `tmp/`（队列条目保存引用，执行时直用）
- **自动执行（drain）**：任务流收尾（`consumeTaskStream` finally，覆盖完成/取消/出错）后由前端按序取队首发送下一条——与直接发送走同一 `sendPrompt` 任务流（用户消息在执行时渲染，`messageId` 入队时生成供撤回/反馈定位）；入队后也触发一次 drain（覆盖「运行恰在入队前结束」的竞态窗口），drain 幂等（会话运行中空转）
- **中断插入（Ctrl+Enter / 排队条 ⚡）**：新输入或已排队输入插队首并取消当前任务（`session.cancel`），当前任务收尾后立即执行队首；空闲时等同直接发送
- **编辑/撤回**：未执行的排队输入可改内容、可撤回（本地队列操作）；已执行的排队输入即普通用户消息（走消息撤回 `truncate`）
- **停止语义**：停止按钮只取消当前任务，队列继续（排队输入是独立意图，撤回由用户逐条控制）；要全部停止先撤回排队项
- **多会话**：队列按会话隔离（切走不丢、切回恢复，随会话删除清理）；**排队条随会话切换重渲染**（`loadMessages` 与发送按钮同点对齐——缺此重渲时排队条停留旧会话内容、后台队列事件触发重渲后又整体消失，切回会话的排队项不可见）；后台会话的排队输入同样在其任务结束后自动执行（发送与流式渲染不依赖当前视图）

### 多模态支持

用户可在会话中发送或引用多模态内容（图片、音频、视频、文档），模型视能力理解处理：

- **消息附件**：`session.prompt` 支持携带附件（图片/音频/视频/PDF 等），附件先上传到会话 `tmp/`，消息中携带引用路径与 MIME 类型（`AttachmentRef.path` 为**会话根相对逻辑路径**，如 `tmp/foo.png`，模型/工具/前端统一按此解析；历史兼容绝对路径附件——沙箱拒绝解析时自动降级为文本说明）
- **附件 → 模型内容**（`AgentEngine.loadHistory` 重建）：图片附件（png/jpeg/gif/webp）且主模型声明多模态能力（`GEBAI_LLM_MULTIMODAL=true`）且 ≤8MB 时**压缩后**（见「图片压缩时序」）base64 内联为统一 `image` 块（携带 `path/name/size` 元数据，size 为原始体积），图片块后附尺寸说明 text 块（原始/压缩后尺寸与体积）；其余（非图片/超限/文件缺失/模型无多模态）降级为文本说明（路径 + MIME + 大小 + 视觉子代理指引），由模型决定装载 vision 子代理（`vision_analyze`）/`read` 等工具处理
- **工具结果图片内联（`read` 图片直读）**：`read` 读取白名单图片（png/jpg/jpeg/gif/webp）**不按文本解码**（乱码无意义）——二进制读入后经 `ToolResult.images` 交引擎处理：主模型多模态时图片以统一 `image` 块**内联进工具结果消息**（模型直接可见，无需 vision 等其他工具），轻量引用（`Message.images`：绝对路径/原始路径/MIME，不含 base64）随工具消息落盘、`loadHistory` 历史重建按引用重读内联；非多模态不内联，工具返回说明 + 视觉子代理指引。svg 为文本按正常读取；bmp 不在白名单（返回转换引导）。**序列化**：OpenAI 兼容 `role:"tool"` 的 content 为块数组（text + `image_url`）；Anthropic `tool_result` content 为块数组（text + image，官方形态）；Responses 的 `function_call_output` 仅接受字符串——文本拼入输出，图片块转为紧随的 user 消息内容部件（同轮可见）
- **接口拒绝自动降级**：主模型声明多模态但接口实际拒绝图片块（HTTP 4xx）时，引擎将图片块一次性降级为文本说明后重试（附件图片与工具结果图片同路径降级，说明文案区分来源；模型可改走 `vision` 工具），实现「无多模态能力自动降级」兜底
- **图片压缩时序（原图保存、发送时压缩）**：粘贴/上传/URL 引用一律按**原图**存入会话 `tmp/`（无损保留，模型/本地 CV 工具/用户取用的都是原图）；仅在**发送给大模型前**由服务端压缩（`@gebai/agents` core/shared/image-resize.ts，Bun.Image 原生编解码）：非 GIF 且长边 >1280px 或体积 >2MB 时等比缩放（长边 1280，体积仍超限按 0.8 倍迭代，下限 0.25 倍）后**同格式**重编码（JPEG/WebP 质量 0.85，PNG 保持 PNG）；未超限/解码失败原样发送。压缩不回写文件（原图不动），并随图片附尺寸说明（`[图片已压缩: 原始 W×H X → w×h x（坐标/细节按比例对应原图）]`，未压缩标注 `[图片 W×H x，未压缩]`）——多模态内联（引擎附件/工具结果图片）与 vision 子代理 analyze 同源共用。GIF 为动画帧格式不压缩
- **多模态消息格式**：统一内部消息模型（文本 + 内容块数组），`provider.chat()` 按接口规范转换：
  - OpenAI 兼容：`content` 为 `[{ type: "text" | "image_url", ... }]` 结构（user/assistant 消息与 tool 结果消息同构）
  - Anthropic：`content` 为 `[{ type: "text" | "image" | "document", ... }]` 结构（`tool_result` 内容同为块数组）
- **图片**：支持本地上传/粘贴/URL 引用，原图保存到会话 `tmp/`（不压缩），发送给大模型前服务端压缩（见「图片压缩时序」），按 Provider 要求编码（base64 data URL 或外链）
- **音频/视频**：上传到会话 `tmp/` 并转为可引用文件；Provider 不支持直接理解时降级为「附文件路径 + 文件名」文本提示（由工具/脚本代处理，如转写）
- **文档（PDF/Office）**：上传后可经工具（如 `read`/脚本）抽取文本喂给模型，或按 Provider 原生 `document` 块传递（能力自动探测）
- **能力探测**：模型声明 `multimodal` 能力（`GEBAI_LLM_MULTIMODAL=true` 显式声明，默认 false），无多模态能力的模型收到附件时提示用户或自动降级（文本说明 + 视觉子代理指引）
- **UI**：消息输入支持拖拽/粘贴/选择附件，预览缩略图，附件随消息一并持久化（会话目录内）
- **工具产物**：Agent 生成的图片/文件同样可回传到会话供查看与下载（见「会话临时文件查看与下载」）

#### 视觉工具 `vision`（已移除——视觉能力统一经 vision 子代理）

**全局 `vision` 工具已移除**（架构决策：视觉相关能力统一收进 vision 子代理，单一入口）：主会话需要视觉分析时 `agent_load` 装载 vision（或模型调用 `vision_*` 时路由自愈自动装载）；子Agent 经 `dependencies: ["vision"]` 依赖声明连带装载（如 self_optimize）；desktop/playwright 域内保留各自的 CV 工具（缺省图像源便利，见「视觉能力分层与子代理复用边界」）。旧形态（`index.ts` 组装层注册全局 vision、subsession_run 子会话继承块同源注册、父会话内 `vision` 与 `vision_analyze` 双份同源 schema 并存）一并退场。

视觉语义分析的实现已迁 `@gebai/agents` core/shared/vision.ts（`makeVisionTool` 工厂 + provider 解析），vision 子代理 def 以 `name="analyze"` 复用——工厂不再是全局注册的专属物，而是子代理工具的实现源：

- **provider 解析**：组装层 `setVisionProviderGetter` 注入（`boot/compose.ts`，`GEBAI_VISION_*` 外挂视觉模型 → 多模态主模型回落；任务级 env 覆盖生效）
- **模型选择**：配置 `GEBAI_VISION_MODEL` 后使用独立视觉 Provider（`GEBAI_VISION_*`，接口地址/密钥/类型缺省继承主模型）；未配置时回落到主模型（须显式声明多模态能力，`GEBAI_LLM_MULTIMODAL=true`，默认 false），两者皆不可用则返回配置提示
- **传输**：图片以 base64 内联（统一内部图片块 → OpenAI `image_url` data URL / Anthropic base64 `image` 块）随文本目标一起调用视觉模型，单图上限见常量参考；**发送前服务端压缩**（见「图片压缩时序」，原图不动）并把原始/压缩尺寸写入文本目标一并告知模型；**超时控制**（`timeout` 参数，默认 30 秒、钳制 1~300）：手动定时器 + AbortSignal 与用户取消信号合并，超时返回提示引导改用本地视觉工具（ocr/locate/locate_image/detect），用户取消原样上抛；返回分析文本（超长走截断保护），并携带 `image` 内容块供 UI 展示
- **降级指引**：图片附件/工具图片块未内联时（模型无多模态能力、接口拒绝图片、超限），文本说明指引模型走 `vision_analyze`（vision 子代理）——engine 的附件说明、read 非多模态分支、图片块降级重试同此措辞

#### 小模型识别（本地 CV 推理，`@gebai/agents` core/cv）

与 `vision`（外部视觉模型服务）互补的**本地小模型图像识别**：非模型服务的 ONNX 推理（PP-OCR 中英文 OCR 为主力，可选自备 YOLO 检测），onnxruntime-web **wasm 进程内**运行——离线可用、不耗模型配额、毫秒~秒级延迟，且能给出**精确像素坐标**（LLM 视觉估坐标不准，这是桌面自动化的刚需）。基建在 `@gebai/agents` core/cv 域，不依赖子Agent 定义存在；**工具消费层为共享工厂 `@gebai/agents` core/shared/cv-analysis.ts**（ocr/locate/locate_image 三工具 `createCvAnalysisTools` + detect 目标检测 `createDetectTool`，均注入缺省图像源与文案）：desktop 子Agent 注入「现截宿主机屏幕 + 本地模式闸门」（`desktop_ocr`/`desktop_locate`/`desktop_locate_image`/`desktop_detect`，见 desktop 小节）；playwright 子Agent 注入「共享桥接截当前页视口 + 无闸门」（`playwright_ocr`/`playwright_locate`/`playwright_locate_image`，见 playwright 小节）；vision 子代理注入「无缺省源（image 必填）+ 无闸门」（`vision_ocr`/`vision_locate`/`vision_locate_image`/`vision_detect`，见 vision 小节）——同一识别逻辑三个消费方零复制（视觉能力分层与子代理复用边界，见「小模型识别」末条与 vision 子Agent 小节）。

- **运行时加载（不进 bundle 图）**：onnxruntime-web 经运行时动态 import dist 入口（拼接包名 + `Bun.resolveSync` + file URL，与 playwright 模块同款规避——bundle 注册表启动安全；裸导入在 node 条件下会指向 onnxruntime-node 原生绑定，故必须走 dist 文件直连）；单线程 wasm（`numThreads=1`，无 SharedArrayBuffer/worker 依赖）。单二进制形态从内嵌产物（`core/cv.embedded.generated.json`，构建脚本 `packages/server/scripts/build-cv-embed.ts` 生成）释放到资源目录——模型 → `{GEBAI_HOME}/resources/models/cv/ocr/`、ort 运行时 → `{GEBAI_HOME}/resources/vendor/cv/`，相对结构与资源子仓库一致（版本 marker + 逐文件写入 + 模型已存在则保留 + 防穿越 + 并发共享，与 pwcore 同思路）；源码/部署形态解析 node_modules
- **模型交付**：构建时下载 PP-OCRv4 mobile（det ~4.7MB + rec ~10.9MB，onnxruntime-web Apache-2.0/MIT 许可）内嵌进产物——下载源 `GEBAI_CV_MODEL_BASE` 可覆写（内网镜像），`{GEBAI_HOME}/resources/models/cv/ocr/` 已有文件跳过下载（资源目录离线自备）；字典从 rec 模型内嵌的 `character` 元数据提取（RapidOCR 约定）。下载失败生成空清单——构建不失败，运行时给配置指引。**资源获取**：主仓库带下载清单与脚本（`scripts/resources.manifest.json` + `scripts/download-resources.ts`，`bun run resources:download`）——逐文件列模型地址（modelscope / hf-mirror / huggingface 多源轮换，`.part` 临时文件 + Range 断点续传，完成后按 size/sha256 校验），按清单相对路径铺开到 `{GEBAI_HOME}/resources/` 即得与资源子仓库同构的目录（`--check` 只校验、`--only` 限定条目、`--source` / `GEBAI_RESOURCE_SOURCE` 指定来源优先级、`--force` 重下、`--skip-vendor` 跳过 vendor 依赖；清单条目可带 `derive` 说明备用获取途径——从 rec 元数据提取字典、从权重导出 ONNX）。运行时模型目录解析：`GEBAI_CV_MODELS_DIR`（任务级 env 可覆盖）→ 二进制形态释放的内嵌模型目录 → `{GEBAI_HOME}/resources/models/cv/ocr/`（资源目录）；目录内固定三件套 `det.onnx`/`rec.onnx`/`dict.txt`
- **推理形态**：惰性共享单例（首次调用初始化 ort + 加载模型，~1-3s）；session 按模型文件路径+大小缓存（文件变更自动重建）；**wasm 路径全进程推理串行**（Promise 链互斥，防同批扇出并发争抢）。wasm CPU 推理同步阻塞事件循环（单次约 0.5-2s）——desktop（本地模式、单管理员）与 playwright 视口截图识别消费可接受（串行互斥保证并发安全）；若未来多租户高并发下成为吞吐瓶颈，OCR 亦可迁入 sidecar（协议为通用 ONNX 会话运行器）
- **分层后端（GPU sidecar ↔ wasm，全部 CV 推理共用，`cv.ts` `runTiered` + `sidecar.ts` + `cv-driver.mjs`）**：检测类重模型（如 YOLO11-L@1280 ≈ 350 GFLOPs）在 wasm 单线程下数十秒级不可用，OCR（det+rec，全屏大图 wasm 单次可达数秒且阻塞事件循环）同样受益——spawn 常驻 `node cv-driver.mjs` 子进程跑 **onnxruntime-node** 原生推理（进程管理仿浏览器桥接：惰性启动、请求超时杀进程重启、stderr 环形缓冲、全进程共享单例）。**EP 逐级探测显式上报**：auto 下 Windows dml（DirectML，任意 DX12 显卡免装依赖）→ cuda、macOS coreml → cuda、全失败回落 cpu（多线程）兜底，session.create 返回实际落地 EP（不静默降级），工具输出如实标注 `后端 sidecar:dml/...`。协议：行分隔 JSON 头 + 定长原始字节帧（张量不走 JSON 化，1280 输入张量 ~20MB/输出 ~32MB 本机管道毫秒级）；前后处理（letterbox/NMS/DB/CTC）留在 Bun 进程，driver 是通用 ONNX 会话运行器（init/session.create/session.run 三 op，`runModel` 按模型键缓存会话——检测与 OCR det/rec 共用同一入口）；**OCR sidecar 路径不加载 onnxruntime-web**（模型目录经 `resolveCvRuntime` 后端无关解析）；模板匹配为纯 JS 计算（非 ONNX）维持进程内。后端选择：`GEBAI_CV_BACKEND` 全局 > `GEBAI_CV_DETECT_BACKEND`/`GEBAI_CV_OCR_BACKEND` 细分覆盖 > auto；工具输出（desktop_ocr/desktop_detect）如实标注实际后端。**onnxruntime-node 不随构建内嵌**（体积/许可）：按 `GEBAI_CV_ORT_NODE_DIR` → `{GEBAI_HOME}/resources/vendor/node_modules/onnxruntime-node`（**资源目录约定位置**——模型/运行时依赖集中存放于项目下独立 git 仓库 `resources/`，主仓库 gitignore 隔离；npm 整包依赖闭包放入即生效，onnxruntime-common 等依赖经 node 标准向上查找可用）→ `{GEBAI_HOME}/vendor` → node_modules 解析，不可解析即 sidecar 不可用；运行期失败（超时/崩溃）**毒化**——本进程生命周期内不再重试，回落 wasm（细分/全局 sidecar 显式指定时不回落、错误如实上抛）；父进程退出时杀子进程（`process.exit` 不保证关闭 stdio 管道，一次性脚本会留孤儿 node 进程持 GPU 会话，实测修复）。二进制形态驱动脚本内嵌（`cvdriver.embedded.generated.json`，`packages/server/scripts/build-cvdriver-embed.ts`）释放到 `{GEBAI_HOME}/resources/vendor/cv/`，dist 形态由 build-subagents 复制到产物目录。**两条 sidecar 通道的后端命名区分（均如实反映「哪个执行体 + 哪个 EP」）**：本条为 node GPU sidecar，后端名 `sidecar:<ep>`（EP 由 `session.create` 返回实际落地值）；消费层 `cv-analysis` 的 sidecar-first 委托（优先调 Python `vision` 边车的 `vision_detect`/`vision_ocr`，见 vision 小节）后端名 `vision-sidecar:<ep>`——EP 由边车从 onnxruntime 会话实际上报（`keqing/python/vision/tools.py` 的 `provider` 字段，如 `cpu`/`cuda`；未上报标 `unknown` 而不臆测）；进程内回落为 `wasm-cpu`。三个前缀互不重叠，避免把 Python 边车误读为 node GPU sidecar
- **OCR rec 批处理（`GEBAI_CV_REC_BATCH`，1..32，缺省 8）**：rec 前处理对每条裁剪做 letterbox（固定 48×320），**所有样本形状一致**，故把 N 条裁剪拼成 `[N,3,48,320]` 一次推理（侧车与进程内两条路径均已批处理）——逐行推理时推理调用次数 = 文本行数（4K 全屏可达数十上百行），批处理把这些次数整除，同时省掉每行的 IPC 往返。实测（本机、真实模型、走 sidecar+DML）：文字图 29 行 **1.81x**（1664ms → 919ms）、密集小块图 420 框 **12.5x**（2542ms → 204ms），**逐行识别文本与逐条推理完全一致**（批维不影响结果）；纯模型层 CPU EP：batch=8 相对逐条 1.91x、batch=16 2.19x。批过大抬高单次延迟与内存，故钉上限 32；批大小经会话级 env 可调（调优/回退用）
- **前后处理（纯函数，`image.ts`/`ocr.ts`/`detect.ts`/`onnx-meta.ts`）**：PNG 解码自研（node:zlib inflate + 逆滤波，覆盖 8-bit 非隔行灰度/RGB/调色板/带 alpha——截图场景全覆盖，16-bit/隔行/JPEG 明确报错）；det 前处理等比缩放（长边 ≤960）+ 32 倍数零填充，DB 后处理（阈值 0.3 → 3x3 膨胀合并碎片 → 8-连通域 → 框内核心像素均值置信度 → unclip 外扩 → 阅读序排序）；rec 前处理 48 高等比缩放 + 320 宽零填充，CTC 贪心解码（blank=index 0，字典 = `[blank] + 6623 字符 + 空格`，对齐 RapidOCR 的 PP-OCR ONNX 部署约定）；YOLO letterbox（缺省 640、114 灰，**尺寸按模型元数据/`GEBAI_CV_DETECT_SIZE` 自适应**）+ NMS（IoU 缺省 0.45，**工具 `iou` 参数可配**——密集小控件场景调低至 0.1），兼容 v8 `[1,4+nc,N]` 与 v5 `[1,N,5+nc]` 两种输出形态（按形状自动识别）；**检测框×OCR 行配对**（`pairObjectsWithText`：行中心落在框内即归属、多行按阅读序拼接——「组件类型+文本」的结构化元素表，完整屏幕解析的本地拼装）；`onnx-meta.ts` 直接从模型字节解析 ONNX metadata_props（顶层 field 14，不依赖 ort 运行时——wasm/sidecar 两条后端同一口径），提取 ultralytics 约定的 `imgsz`（输入尺寸）与 `names`（类别表，省标签文件）；**坐标一律还原到输入图像原始像素系**（region 偏移 + 缩放比例），desktop 供 `mouse_click`、playwright 供视口 `document.elementFromPoint` 直接使用
- **目标检测（`desktop_detect`）**：模型不内嵌——**缺省自动发现 `{GEBAI_HOME}/resources/models/cv/detect/` 下唯一 `.onnx`（资源子仓库整体放入 `{GEBAI_HOME}/resources/` 即零配置可用：检测模型自动发现 + GPU sidecar 原生包自动解析）**，或 `GEBAI_CV_DETECT_MODEL` 显式指定（多模型共存时必须显式，否则列出候选报错；官方 ScreenParser 权重可由 `bun run resources:download` 拉取，`.pt → ONNX` 导出命令见清单条目 `derive`）；ultralytics 导出（YOLO11/v8）即插即用（元数据自适应尺寸与类别）；非 ultralytics 来源设 `GEBAI_CV_DETECT_LABELS`（每行一类别）；COCO 预训练类别对 UI 操作无意义，面向 UI 组件检测模型（如 ScreenParser 类）或自训练图标/控件模型；推理走「检测分层后端」（上一条）
- **模板匹配（`template.ts` 纯函数，非 ONNX 推理）**：NCC（归一化互相关）同尺寸匹配，补「文字走 OCR、检测需自训 YOLO」之间的图标/控件定位空白（`desktop_locate_image`/`playwright_locate_image`/`vision_locate_image` 消费，零训练）；两阶段加速——box 降采样**相位偏移粗扫**（origin {0,s/2}² 多相位防高频内容错位去相关，粗扫门限 0.4 仅引导）+ 全分辨率邻域精化，窗口均值/方差经「和+平方和」积分图 O(1) 求取；非极大值抑制后至多 5 个匹配（2560x1440 全屏 + 48px 模板实测约 350ms）；不做缩放不变（模板需与目标同一显示环境同尺寸）
- **视觉能力分层与子代理复用边界**：视觉能力按「是否与图像源环境耦合」分层，两种复用方式（其他子代理用视觉能力的标准路径）：
  - **方式一（依赖声明，适合通用图片能力）**：`dependencies: ["vision"]` 声明依赖 vision 子代理（见 vision 小节），装载/subsession_run 预加载自动连带，以 `vision_*` 命名空间直接调用（analyze 语义分析 / ocr / locate / locate_image / detect）——能力与实现单源维护在 vision def，依赖方零复刻；适合「输入=图片文件路径、无环境耦合」的能力复用
  - **方式二（自己的工具名 + 共用实现，适合环境耦合能力）**：消费方在域内提供自己的工具（desktop_*/playwright_*），实现经共享工厂（cv-analysis / makeVisionTool）注入各自的**缺省图像源**（现截屏幕/当前页视口）、**闸门**（本地模式）与**文案**（坐标系语义/消费提示）——工具名、坐标系、审批姿态是域内语义，识别/分析实现单源共用；适合「省略 image 有环境含义、坐标映射回特定像素系」的能力
  - **判定标准**：能力离了消费方环境是否仍成立——「对一张 PNG 做 OCR」离了 desktop/playwright 依旧成立（方式一收进 vision 子代理）；「省略 image 现截屏幕、坐标可直接 mouse_click」离了 desktop 无意义（方式二留在域内）。desktop/playwright 域内已提供全套 CV 工具，语义分析兜底按需 agent_load 装载 vision 子代理，**不声明依赖 vision**（避免 4 个 image-必填的重叠工具常驻上下文，徒增选择噪音）；self_optimize 声明依赖（subsession_run 隔离子会话即使不继承全局工具也有视觉能力）；**全局 `vision` 工具已移除**——引擎图片附件/read 非多模态/图片块降级的指引文案统一改指 `vision_analyze`（vision 子代理，agent_load 装载或路由自愈）


### 多用户隔离与安全

服务模式下，所有数据与执行环境按 **用户 → 会话** 两级隔离：

#### 认证与鉴权
- **登录鉴权**：服务模式启用登录（用户名/密码），密码使用加盐哈希存储（scrypt，不落明文）
- **开放注册（可审批）**：服务模式登录页提供「注册账号」入口（`POST /api/v1/auth/register`，公开端点）；**注册用户恒为普通角色**（不可注册 admin，admin 唯一入口是启动参数 `GEBAI_ADMIN_PASSWORD_HASH`）；**用户名经 `normalizeUsername` 规范化**：小写折叠（Windows 文件系统不区分大小写，`Alice` 与 `alice` 不得形成两个注册表键共享同一物理目录）+ 格式白名单 `[a-z0-9][a-z0-9_-]*`（1-32 字符，拒绝 `..`/`.`/路径分隔符/控制字符防用户目录穿越）+ 拒绝 Windows 保留设备名（con/nul/com1 等）；重名/非法返回 400；本地模式不开放（404）。**审批模式由 `GEBAI_SIGNUP_MODE` 控制**：`open`（默认）=注册即登录（签发令牌）；`approval`=注册用户置 `disabled+pending` 待审（不签发令牌、不可登录），登录页提示「等待管理员审批」，admin 在用户管理页**批准**（`PATCH {disabled:false, pending:false}` 启用）或**拒绝**（删除用户）。**来源限制由 `GEBAI_SIGNUP_SOURCE` 控制**：`any`（默认）=内置登录页与二开前端均可注册；`custom`=仅二开前端代码可注册——注册请求须携带 `X-GEBAI-Signup-Source: custom` 头（内置登录页的 SDK 调用不带，403 拒绝；二开脚本直调 REST 带头即放行），公开探测端点 `GET /api/v1/auth/signup-config` 返回 `{ mode, builtinAllowed }`，前端 `builtinAllowed=false` 时隐藏「注册账号」入口（二开配置 `allowSignup: false` 可达同款隐藏，两者任一生效；服务端 403 是硬闸）
- **外部身份扩展点**：服务模式下支持「外部身份 → GEBAI 令牌」兑换（`POST /api/v1/auth/exchange`），同源部署网站可直接复用其本地登录态（localStorage）作为 GEBAI 用户，免二次登录。验证器可插拔（`external-auth.ts`）：配置 `GEBAI_EXTERNAL_AUTH_SECRET` 走 HMAC 验签（凭证 `{exp}.{sig}`，±10 分钟窗口 + **一次性消费**——签名无 nonce，已消费凭证摘要缓存防窗口内重放），配置 `GEBAI_EXTERNAL_AUTH_URL` 走 HTTP 回调（业务系统自行校验凭证真实性；**回调返回的用户名必须与请求一致**——允许规范化去空白，拒绝不一致，防宽松回调被利用接管任意用户）；两者互斥，验证失败统一 401 不泄露原因。**外部身份禁止命中本地 admin 账号**（外部系统同名 `admin` 用户兑换不得继承本地 admin 角色——admin 唯一入口是启动参数口令）。用户映射：`GEBAI_EXTERNAL_AUTH_AUTOCREATE=true`（默认）自动创建普通角色用户（随机密码不可密码登录），`false` 仅允许已存在用户；外部用户名经 `normalizeUsername` 规范化（同上：小写折叠 + 白名单 + 保留名拒绝）。前端注入：Web UI 启动时若本地无令牌，先取 URL 参数 `?gb_ext_username=&gb_ext_credential=`，再按 `GEBAI_EXTERNAL_AUTH_STORAGE_KEY` 直读宿主 localStorage（JSON 或 `username:credential` 字符串）；`GET /api/v1/auth/external-config` 供前端探测启用状态（不泄露密钥）
- **登录限流**：连续失败 5 次锁定该用户名 60 秒（内存计数），防在线爆破；**登录/兑换/注册端点另加令牌桶**（REST：登录/兑换全局桶 60 突发/2 每秒 + 来源桶 10 突发/0.2 每秒，注册独立桶 30/0.5 + 10/0.1；WS `auth.login` 密码路径同款桶 30/1——scrypt 即使异步化仍耗 CPU，轮换用户名即可绕过按用户名锁定，防 CPU DoS 放大器；`GEBAI_TRUST_PROXY=true` 时 REST 按 `X-Forwarded-For` 首段区分来源）
- **令牌机制**：登录后签发会话令牌（HMAC 签名，7 天 TTL），后续 WebSocket 连接携带令牌建立用户上下文；**令牌表持久化到 `{GEBAI_HOME}/auth-tokens.json`**（签发/撤销/过期清理时落盘，进程重启后已签发令牌仍有效——单机部署下重启不掉线；过期令牌在 authorize/保存时顺带清理，不无界增长）
- **凭证来源扩展点（REST 认身份的可插拔入口）**：`resolveUser` 不再写死 Bearer/Basic，而是按**来源链**依次尝试（`credential-sources.ts`）——每个来源只负责「从请求里取出身份」，**校验一律下沉**（令牌型交 `authorize()` 验签+TTL+disabled；用户名型必须经 `userByName()` 命中注册表中**已启用**用户，不得凭空构造身份）。两条提供途径，可同时使用：
  - **数据级**：`GEBAI_CREDENTIAL_SOURCES`（逗号分隔），名称语法 `bearer` / `basic` / `cookie:<名>` / `header:<名>`；非法项**启动期报错**（不静默降级为「少了一道鉴权」）。
  - **代码级**：仓库根 `custom/auth/*.ts|js` 默认导出凭证来源（或来源数组），与 `custom/agents/` 同一套「放文件即注册」模型；单个文件加载失败只记 warning、不影响其余来源（失败隔离）。
  - **缺省链**：`bearer` → `basic` → `cookie:gebai.auth.token`——页面里的图片/视频/iframe/下载是浏览器**原生请求**，前端脚本无法插 `Authorization` 头，不带 cookie 通道时服务模式下消息流的图片一律 401。
  - **安全边界**：cookie 载体**默认仅 GET/HEAD**（cookie 是浏览器自动携带的凭证，写端点若也认即 CSRF）；需全方法生效由来源显式声明 `safeMethodsOnly: false`。来源链顺序即优先级，全部未命中才 401；来源抛错按未命中处理（单个扩展点故障不使鉴权瘫痪）
- **前端凭证契约（`window.__GEBAI_AUTH__`）**：前端一切「带身份去请求」收敛到单一出口（`@gebai/sdk` auth-contract）——读/写/清令牌与请求头构造四个动作，缺省实现就是既有行为（localStorage `gebai.auth.token` + `Authorization: Bearer`），未覆盖时行为不变；部署方在 `custom/web/init.js` 赋值即可整体替换（逐项回落，抛错按未提供处理）。**顺带修复既有缺口**：文件工作台 `FsApi` 原先不携带任何凭证（服务模式下只能靠浏览器 cookie），现与主界面走同一契约。**同 IP 多实例**：cookie 不隔离端口、按 host 共享，多实例并排时可用本契约改 `sessionStorage`/自定义头完全隔离；框架自身**不再在未登录页面写空 cookie**（只登录写、登出清），避免 B 实例页面加载误清 A 实例的登录态
- **WS 未登录拦截**：服务模式下未登录（无令牌）的 WS 连接仅允许 `auth.login`，其余消息一律拒绝
- **跨站来源防护（本地/桌面免登录形态）**：WebSocket 不受同源策略约束且本地模式免登录——恶意网页可直连 `ws://127.0.0.1:*` 以 admin 身份建会话执行命令（REST 通道因 CORS `*` 同样暴露）。防护：WS upgrade 与 REST `/api/*` 均校验 **Origin 与 Host 同源**（浏览器发起的跨站请求必带 Origin，不同源即 403；非浏览器客户端无 Origin 不受限）。**两侧豁免面一致**（`wsOriginAllowed` 与 REST CORS 中间件同规则）：服务模式（令牌鉴权）不拦、显式配置 `GEBAI_CORS_ORIGINS`（不含 `*`）视为有意开放的跨源白名单不拦；仅「本地/桌面免登录形态 + 缺省 `*`」才要求同源——避免「白名单放开了 REST 却连不上 WS」「服务模式异域前端能调 REST 不能开 WS」的配置陷阱
- **WS 全局子Agent 装载/卸载管理员门槛**：`sub_agent.load`/`sub_agent.unload` 不带 `sessionId` 的**全局形态**（变更所有用户的工具注册面）服务模式下仅 admin（与 REST 工具启停同门槛）；带 `sessionId` 的会话级装载/卸载不受限（只影响本人会话）；模型侧 `agent_load` 装载进当前会话（会话级引用，见「子Agent」引用计数）
- **用户管理**：支持管理员创建/禁用用户；每个用户独立命名空间（`users/{user}/`）；服务模式 admin 用户通过启动参数 `GEBAI_ADMIN_PASSWORD_HASH` 引导（**设置则启用并覆盖其密码哈希，不设置则禁用**，启动参数为权威配置每次启动重置；admin 被禁用时**普通用户可经注册页自助注册使用**（普通角色），管理员能力须部署方设置哈希启用）；**REST 与 WS 双通道的用户管理端点均校验管理员角色**（非管理员一律 403，防普通用户提权/越权管理）
- **请求校验**：所有会话操作先解析令牌确定用户，再校验会话归属（含取消任务、审批决策等控制类操作）；**会话 ID 格式白名单**（32 位小写 hex，`randomUUID` 去连字符）在存储层 `sessionPath`/`store` 与 REST 中间件、WS 消息入口四层强制——畸形/穿越形态（`../`、路径分隔符）一律 400/错误应答，从根上杜绝会话 ID 拼路径形成的目录穿越
- **附件安全**：上传/引用的附件文件名仅取 basename 并拒绝路径穿越；**上传落盘不覆盖**——同名文件已存在（含同批重复、磁盘既有）时自动加序号（`a.png` → `a-2.png`，REST/WS 上传端点与引擎 `saveAttachments` 同一规则，多张粘贴图各自独立落盘）；沙箱约束用户附件来源路径限定会话目录内（防任意文件读取与越界写入），豁免用户（admin，特权用户）不限制来源

#### 数据与执行隔离
- **数据隔离**：会话、临时文件、截断内容均存储于 `{GEBAI_HOME}/users/{user}/` 下，用户之间互不可见
- **会话归属校验**：任何会话操作（读取、切换、发送、删除）均校验当前登录用户与会话所有者一致，禁止跨用户访问
- **路径沙箱**：服务端部署模式下，文件类工具（`read`/`write` 等）的相对路径统一以当前会话 `tmp/` 为基准（见「路径基准」），仅允许访问该目录内路径，对目录外路径一律拒绝，防止路径穿越（`../`、绝对路径、符号链接）；**桌面/本地浏览器模式默认不启用目录限制**，可访问本机任意路径（由 `GEBAI_SANDBOX` 控制，默认按运行形态自动选择）；**仅本地模式默认用户（id=admin）豁免路径沙箱**（`Sandbox.isExempt`/`enforcedFor(user)` 按用户判定：本地模式是操作者本人机器、豁免用户即使沙箱启用也按本地模式放开；**服务模式一律沙箱**——admin 也不豁免，多租户边界一致）；**`GEBAI_SANDBOX=off` 与服务模式互斥**（启动即拒绝：多租户 + 关沙箱会让 files 接口回退裸 `resolve` 可 `../` 越界读任意路径，安全配置错误在启动期暴露）
- **脚本隔离**：`sh`/`py`/`js` 脚本以独立子进程运行，工作目录限定为当前会话 `tmp/`（**spawn 前自动创建缺失目录**——纯命令会话无附件/写文件时 `tmp/` 从未创建，否则 `sh`/`js`/`ls` 相对路径全体 ENOENT；`Sandbox.exec`/`spawnBackground` 对 cwd 与日志父目录 `mkdir recursive`，引擎任务启动时同规则兜底），环境变量注入当前用户上下文；**沙箱（服务端部署）模式与安全模式下脚本子进程环境剔除敏感变量**（按完整结尾单词匹配 `_KEY`/`_TOKEN`/`_SECRET`/`_HASH`/`_KEY_ID`/`_CREDENTIAL`/`_AUTH`/`DATABASE_URL` 及裸名 `TOKEN`/`SECRET`/`PASSWORD` 等形态，防止任意用户经脚本 `env`/读取外泄服务端全局密钥，也防安全模式下 js 读 `process.env` 拿到密钥；**豁免用户（admin，特权用户）不剔除**，本地模式不剔除（安全模式仍剔除）；`js` 的会话上下文注入 `ctx.env` 同规则脱敏）；JS/TS 脚本优先通过**内置运行时自执行**（见「脚本执行环境」），不依赖宿主机安装 bun/node；**服务模式脚本运行根收敛到会话目录**（`GEBAI_SCRIPT_ISOLATION`，默认 `auto`，见「多用户隔离与安全·数据与执行隔离」）
- **脚本运行根（会话隔离，服务模式默认且强制开启）**：服务模式下脚本（`sh`/`py`/`js`/后台任务）的执行根是**会话目录**——`cwd` 取会话 `tmp/`，`HOME`/`USERPROFILE`/`TMPDIR`/`TMP`/`TEMP`/`XDG_CONFIG_HOME`/`XDG_CACHE_HOME`/`XDG_DATA_HOME`/`XDG_STATE_HOME` 一律重定向到 `{会话目录}/script-env/{home,tmp,config,cache,data,state}`（spawn 前预创建；覆盖顺序在任务 env 之后，防任务 env 里的 `HOME` 把脚本环境指回宿主）。不改写时脚本的配置/缓存/临时文件落在宿主共享位置（用户家目录、系统临时目录），跨用户、跨会话相互可见可覆盖。**文件系统隔离（Linux + bubblewrap）**：命令包进隔离沙箱——系统目录**只读**绑定（`/usr`/`/bin`/`/sbin`/`/lib`/`/lib64`/`/lib32`/`/libx32`/`/etc`/`/opt`/`/var`/`/run`/`/nix`/`/snap`/`/usr/local` 中**存在者**；usrmerge 系统（`/bin`、`/lib` 等为指向 `usr/*` 的符号链接）必须包含这些链接目录，否则动态链接器路径缺失、命令直接不可执行）、`/proc` 与 `/dev` 重建、**仅会话目录可读写**（`--bind`）、`--chdir` 会话内、`--die-with-parent`（父进程退出即回收），越出会话目录的路径不可见（其他会话/用户数据既不可读也不可写）；`project` 参数与项目绑定的解析基准在服务模式下同样以会话目录为界（跨会话即越界），预置项目（`*_PROJECTS`）与子Agent 项目绑定（`*_PROJECT`）在服务模式下不生效——本地模式（豁免用户）全部保持原行为
- **bwrap 可用性探测（容器内尤其重要）**：不只查 PATH——必须完成一次**与真实执行同参数形态**的隔离启动（`bwrapArgs` 同一套参数 + 真实解释器 + 临时目录充当会话目录）才判定可用：容器内未授予 user namespace（seccomp/AppArmor 拦截）时 bwrap 存在但每次调用都失败，简化形态探测通过也不代表真实形态通过（绑定项与只读/可写划分都可能出问题）。不可用时回落环境收敛：行为始终安全（HOME/TEMP 仍在会话目录内），仅失去文件系统隔离；显式配置 `bwrap` 而不可用时输出一条带**失败原因**（bwrap 的 stderr 摘要）的告警。**容器部署判定**：在目标容器内执行 `unshare --user --map-root-user echo ok` —— 输出 `ok` 则 bwrap 可用（还需容器内存在 `bwrap` 二进制，Debian/Ubuntu 包名 `bubblewrap`）；报 `Operation not permitted`/`Permission denied` 则需放宽容器：`--security-opt seccomp=unconfined`（实测可用）或 `--privileged`——**仅 `--cap-add SYS_ADMIN` 不够**（unshare 可创建，但 bwrap 仍被 seccomp 拦在 `pivot_root`）；也可在宿主放开非特权 user namespace。若一个容器只服务单个用户/会话，容器本身就是隔离边界（无需 bwrap，环境收敛已足够）；只有**一个容器内跑多用户 GEBAI** 时 bwrap 才是必需的隔离层
- **桌面控制隔离**：`desktop`（截图/窗口控制/键盘鼠标输入）是对宿主机桌面的真实操作，**仅本地/桌面模式或沙箱豁免用户可用**——服务端部署（沙箱约束用户）下全部工具一律拒绝执行；输入/点击/窗口控制类工具默认需审批，防远程滥用与误操作。部署警示：**服务模式（`GEBAI_SANDBOX=auto` 时）自动强制启用沙箱**，阻断远程桌面操控（admin 豁免除外）
- **浏览器隔离**：`playwright`（无头浏览器自动化）运行在**隔离的浏览器环境**（独立 Chromium 进程，不触宿主机桌面/文件系统），服务端部署可用；但导航/交互/脚本类工具（`open`/`click`/`fill`/`press`/`select`/`check`/`evaluate`/`new_page`）默认需审批——防远程用户借服务端浏览器探测内网（SSRF）、提交表单或执行任意页面脚本；浏览器上下文按会话隔离，`evaluate` 可读取页面内数据（含表单值/cookie），仅限审批后执行
- **Webhook SSRF 防护**：Webhook 注册默认拒绝回环/链路本地/云元数据地址（`localhost`、`127.*`、`169.254.*`、`::1`、`fe80:*` 及其 IPv4-mapped/尾点 FQDN 等绕过形式），需内网回调时以 `GEBAI_WEBHOOK_ALLOW_PRIVATE=true` 显式放开；**投递同样带逐跳重定向校验**（复用 `fetchWithRedirectGuard`，每跳 Location 重新过 `checkWebhookUrl`，防「注册公网 URL → 302 内网/元数据」跳板绕过注册期校验，与 `fetch_url`/`http_request` 同口径）
- **公网访问守卫（fetch_url/http_request）**：沙箱约束用户（服务端部署模式）仅允许公网地址——拒绝回环/链路本地/私网（RFC1918）与 **ULA**（`fc00::/7`）；主机名判定统一走 `@gebai/agents` core/shared/ip.ts，覆盖常见绕过形式：**IPv4-mapped IPv6**（`[::ffff:127.0.0.1]` 及完整形式）、IPv4-compatible IPv6、**整数/十六进制/八进制 IPv4**（`2130706433`/`0x7f000001`/`0177.0.0.1`，WHATWG URL 已规范化为点分十进制）、**尾点 FQDN**（`localhost.`）；`fetch_url` 与 `http_request`（默认路径）另带**重定向逐跳校验**（`redirect: "manual"` 手动跟随，每跳 Location 重新执行公网校验，跳数上限 5），防「初始公网 → 302 内网」跳板绕过；**域名做 DNS 解析复查**（`assertPublicHttpUrl` 异步解析主机名，任一解析地址命中私网/回环即拒绝——覆盖内网 DNS 名（`kubernetes.default.svc` 等）与「公网域名 A 记录指向内网」的重绑定形态；解析失败/3s 超时放行由 fetch 层超时兜底）；**豁免用户（admin）不限制私网访问**
- **环境变量隔离**：**用户环境变量服务端零留存**（只存浏览器本地 localStorage，服务端不落任何 env 文件；会话内存态 env 不落盘、重启即空），用户间互不可见；`{AGENT_NAME_UPPER}_*` 前缀为**命名约定与前端目录白名单口径**（envVars 声明汇总进环境变量面板），运行时硬边界为脚本子进程/`js` ctx 的**敏感变量剔除**（见「脚本隔离」——敏感判定按完整结尾单词匹配：`*_KEY`/`*_TOKEN`/`*_SECRET`/`*_HASH`/`*_KEY_ID`/`*_CREDENTIAL`/`DATABASE_URL`/`CONNECTION_STRING` 及裸名 `TOKEN`/`SECRET`/`PASSWORD` 等前后缀/裸名形态，防 `GEBAI_ADMIN_PASSWORD_HASH`/`AWS_ACCESS_KEY_ID` 类漏判），敏感变量脱敏显示
- **子Agent共享**：子Agent 为服务端内置代码，构建时编译进二进制，全局共享、只读、无用户差异
- **审批隔离**：`/approval-skip` 为会话级设置，仅作用于当前会话（会话内存态 env，不落盘），不影响其他用户；**用户本人可设置自己的会话**（前端开关、REST `PUT /env`、WS `session.env.set`、飞书 `/approval-skip` 命令——写入只影响本人会话，非管理员仍受路径/脚本/网络沙箱完整约束）；**ask 填值分支服务模式下一律拒绝 `GEBAI_APPROVAL_SKIP`**——模型驱动的写入不得自设审批跳过（防提示词注入诱导），本地/单用户模式不受限
- **安全模式（`GEBAI_SAFE_MODE`）**：部署方可在启动时开启降级运行形态（**仅启动时从 .env/环境变量加载，不进会话/任务级 env，`ask` 填值分支与前端本地 env 注入均无法修改**）——原则是**风险能力降级而非一刀切禁用**，各风险工具在自身 execute 内降级：
  | 能力 | 安全模式降级形态 |
  |------|------------------|
  | `sh` | 只读命令白名单（`cat`/`head`/`tail`/`grep`/`find`/`ls`/`git` 读子命令/`sort`/`diff` 等查看与文本处理类；白名单按命令名匹配——Windows PowerShell 下的别名（`ls`/`cat`/`dir`/`where`）照常命中，cmdlet 全名（`Get-ChildItem`）不命中；`sed`/`awk` 有脚本内写/执行通道、`less`/`more` 有 `!` shell 逃逸，均不入列）；命令解析 fail-closed（反引号/后台执行 `&`/进程替换 `<(cmd)`/未闭合引号等无法识别的结构一律拒绝）；输出重定向 `>`/`>>`/`2>`/`&>` 目标须在**安全写范围**内（`/dev/null`/`NUL`/PowerShell `$null` 放行），fd 复制 `2>&1` 放行；`$(...)` 命令替换与双引号内替换按替换语义**递归校验**（POSIX 双引号内 `$()` 与反引号会执行，单引号为字面量）；`find` 禁 `-exec`/`-delete` 等执行/写动作、`sort` 禁 `-o`/`--output`、`git` 仅读子命令（`status`/`log`/`diff`/`show`/`blame`/`rev-parse`/`ls-files`/`describe` 等）、`env` 仅单独执行、`date` 禁 `-s`/`--set`、`hostname` 仅 flag 参数 |
  | `py` | 子进程内 `sys.addaudithook` 审计钩子（`core/security/safety.ts` `PY_SAFE_BOOTSTRAP`）：写模式 `open`（mode 含 `w`/`a`/`x`/`+` 或 flags 含 `O_WRONLY`/`O_RDWR`/`O_CREAT`/`O_TRUNC`/`O_APPEND`）、进程（`os.system`/`subprocess.*`/`os.posix_spawn`/`os.fork`）、网络（`socket.connect`/`bind`）、文件变更（`os.remove`/`rename`/`mkdir`/`chmod` 等、`shutil.*`）、`ctypes.dlopen`（防绕过钩子的裸系统调用）、`sqlite3.connect` 全部拒绝；钩子堆叠不可移除，**仅保留文件读取** |
  | `js` | 双层降级：静态扫描（`scanJsReadOnly`）**按词元级拒绝**（而非调用形态）——`import`/`require` 任意出现即拒（静态 import 语句提升先于 shim 执行、`const rq = require; rq(...)` 别名形态调用正则拦不住）、`Bun` 仅放行 `Bun.file`（`Bun["fetch"]` 括号访问可躲点号正则，且 fetch/sqlite 为不可覆写 getter、Bun 全局 non-configurable 无法运行时代理，扫描是唯一防线）、`getBuiltinModule` 词元拒绝；运行时 shim（注入子进程）屏蔽其余——`Bun` 对象属性覆写为抛错桩（`write`/`spawn`/`serve`/`$`/`sql` 等）、`Bun.file` 包装拦截 `write`/`writer`/`sink`/`truncate`（读方法照常）、删除 `eval`/`Function`/`fetch`/`WebSocket`/`Worker` 全局、`Function.prototype.constructor` 中性化防 `(fn).constructor` 回收、`process` 仅拦 `binding`/`dlopen`/`getBuiltinModule`/`kill`（桥协议依赖 stdin/stdout/exit）、字符串定时器拒绝、`Reflect.get` 作用于 Bun 对象时拒绝（防反射读取绕过属性覆写）、**模块级 `var require` 中和**（CJS 参数遮蔽/ESM 定义绑定，别名引用拿到拒绝桩）——**仅保留文件读取**；子进程环境同样剔除敏感变量（安全模式下 `process.env` 不暴露密钥）；写文件用 `write` 工具、网络用 `fetch_url` 工具（RPC 调用各工具按其降级规则执行） |
  | `write`/`edit`/`file` | 限定**安全写范围**内（`safeModeWriteCheck`）：沙箱模式=用户数据根（`users/{user}`）；本地模式=OS 用户主目录 + `GEBAI_HOME` + 会话工作目录（Windows 大小写不敏感比较）。越界拒绝并提示，范围内照常 |
  | 动态工具（`js` defineTool） | 与 `js` 同规则降级（execute 源码静态扫描 + 子进程只读 shim），注册与水合**不再跳过**——只读动态工具（数据处理/查询类）安全模式下保持可用 |
  | `task_add`/`task_update`/`task_remove`/`task_run`/`task_cancel` | **维持硬阻断**（任务可延迟触发任意执行，无法降级）：引擎主/子循环、`js` RPC 分发层按 `isToolBlockedInSafeMode` 同规则拦截，模型调用时直接返回限制信息（不执行、不弹审批）。`task_notify`（主动通知）声明 `safeMode: false`——安全模式下不注册（通知=外发请求，与通知投递整体禁用同规则） |
  | 子Agent 工具 | **自主声明 `Tool.safeMode`**：`true`=作者判定安全模式下可提供（即使短名风险如 `{agent}_sh`，须自行保证实现只读或体内按 `ctx.safeMode` 校验）；`false`=判定不提供（即使名字无风险）；未声明=按短名风险规则默认（`isRiskyToolName`：短名集合为 `sh`/`py`/`js`/`write`/`edit`/`file`/`delete`/`cron_add`/`cron_update`/`cron_remove`/`cron_trigger`，按「等于或 `_{risk}` 结尾」匹配——如 `{agent}_sh`/`{agent}_cron_add` 命中则不注册）。注册期过滤（`ToolRegistry({safeMode})`，主注册表与 subsession_run 子会话注册表同规则）——不注册即 schema 不可见、调用报未知工具 |
  **安全写范围**与**降级说明注入**：系统提示词（主循环与子会话运行）在安全模式下追加降级能力说明（模型知晓能力边界）；已创建的 script 型任务触发时跳过（落盘提示、不执行 shell，`nextRunAt` 正常推进）；审批姿态不变（各工具原 `requiresApproval` 规则照常生效）
- **限流保护**：按用户限制并发任务数与消息速率，防止资源滥用；单用户单会话同时仅一个任务运行；**每用户 prompt 令牌桶限流**（REST `POST /sessions/:id/prompt` 与 WS `session.prompt` 同规则：容量 60 突发、30/秒补充，超限返回 429 / error reply）

#### 安全与稳定性强化批次（2026-08，全量审查修复）

除上述各节就地更新的语义外，本批次还包含以下工程级强化（均已落地并带回归测试）：

- **持久化原子性**：`chat.json` 写入改为 tmp+rename 原子替换（崩溃/断电/磁盘满不再留下截断 JSON 致会话静默丢失；Windows 上目标被并发读取时 rename EPERM 回退直接覆写——原子性降级为最佳努力，不因竞态中断任务）
- **任务并发 TOCTOU 防护**：`engine.run()` 检查与注册之间的 await 间隙可让并发请求（WS 重复帧/REST 与 WS 双通道）双双通过检查造成同会话双任务——改为**同步占位再异步校验**（校验失败同步回滚），入口层 `isRunning` 检查不再有竞态窗口
- **会话缓存 LRU 化**：`store.load()` 命中刷新位置（运行中会话不因其他会话活跃被挤出 10 条缓存）；任务流程内的持久化调用（`appendMessage`/`getTodos`/`setTodos`）带归属用户——挤出后仍可从磁盘确定性装载（此前挤出后无 user 的装载失败会中断运行中任务）
- **审批授权竞态（飞书群聊）**：`runOwners` 绑定条件写入（不覆盖既有绑定）+ 解绑校验「自己仍是绑定者」——并发消息竞态下后来者覆盖/误删发起者绑定会使审批 `openId` 变空、跳过「仅发起者可操作」校验（任意群成员可批准高危操作）
- **飞书长连接韧性**：pong 超时检测（连续 3 个 ping 周期无 pong 主动断开走重连——NAT 超时/半开 TCP 下 onclose 不触发、bot 永久失联）；重连达服务端上限后**降频续试**（60s 封顶，不再永久停摆）；事件按 `event_id` 去重（飞书重推防任务重复执行）；`finalSent` 每任务复位（空最终回复的任务在飞书侧得不到完成信号的缺陷）
- **feishu_docs token 管理**：user refresh_token 刷新**会话级单飞**（单次有效的 refresh_token 并发双发必有一个失败静默降级 tenant 身份）；tenant token 远端失效（99991661/99991663）逐出缓存重试一次（此前最长 2 小时持续失败）
- **子Agent 装载引用计数**：`SubAgentManager.load/unload` 按 owner（会话 id/subsession_run 共享标记/全局）解引用——一个会话卸载子Agent 不再注销全局注册表砍掉其他会话正在使用的工具（装载状态按会话建模、注册表全局共享的矛盾以引用计数弥合）
- **SDK 连接韧性**：心跳判死阈值回归 `heartbeatTimeoutMs` 语义（此前下一拍间隔即杀，高延迟链路「连上→误杀→重连」循环）；断线批量 reject 在途请求时清超时 timer（不再二次 onError「WS 请求超时」误导）；建连期无 onerror 的 close 路径 settle 共享 promise（调用方不再永久挂起）；断线恢复后 seq 基线收敛（缺口后方无新事件窗口内不再重复重放旧区间——旧 `task.done` 截断新任务流）；`dispatchEvent` 按 seq 去重（多会话并发恢复同一批重放事件不再双份分发）；WS 附件 `Uint8Array` 序列化为数组（此前 JSON 化为类数组对象、服务端构造抛错）
- **Web 交互修复**：审批 Y/N 快捷键排除文本输入焦点（搜索/重命名框打字误批）；审批等待刷新前端看门狗活跃时间（服务端审批等 5 分钟、前端 150s 看门狗不再误杀）；审批卡按 toolCallId 去重（事件重放不堆叠）；切回会话时运行中工具卡重建过滤历史已完成项；断线全量重同步后子Agent 容器惰性重建（run 输出不再静默丢弃）
- **其他**：`file delete` 递归删除需审批（见「工具审批」）；附件重名自动加序号（上传端点与引擎落盘统一规则：同批重复与磁盘同名均追加序号，粘贴多张同名图不再互相覆盖）；analyzer 的 tree-sitter Tree 用毕显式释放（wasm 原生内存）；`serve_dir` 静态服务 8 个上限（最旧淘汰）；desktop `screenshot` 的 `name` 参数落地（此前声明未实现）；`show`（由已合并的 `draw`/`render_html`/`show_file` 三工具演进而来）的 name 参数补 writeGuard/安全模式写检查（此前可越出子Agent 写范围）；SSE 解析兼容 CRLF/CR 行结尾与多行 data 按规范 join（部分网关 CRLF 行结尾整轮静默无输出）；三家 provider 的 done chunk 语义统一（恰好一次、断流补发）；SSE 消费方提前退出时 cancel 上游响应流（连接不悬挂）；vision 调用透传取消信号

### 总Agent
- 系统提示词**身份行**先声明目标（「目标是融合旧世界IT的所有技术，打造新世界智能的躯体——以极致动态扩展的能力把任意语言、任意进程的外部技术收编为工具」），再携带智能与智体概念模型（「你是智体：智能（模型）负责思考、无状态、可替换，记忆与责任都长在智体——需跨轮次/跨会话保留的结论与状态写入文件或会话记录；你的每次工具调用都是智体的行为，经审批执行、留痕可审计」）——行为化措辞而非装饰（落盘纪律与担责意识直接约束模型行为）。概念定义与五条设计戒律见「定位 → 智能与智体（概念模型）」
- 系统提示词中注入：**未装载**子Agent 的轻量引导列表（名称 + 描述），引导模型通过 `agent_load` 装载后使用（装载 = 工具注册 + 完整提示词写入会话记录）或直接 `subsession_run` 派生子会话（无需装载）。**已装载子Agent 的完整系统提示词不注入总层提示词**——装载时已作为 system 消息写入会话记录（`loadedAgent` 标记，`loadHistory` 透传进模型上下文），此处再注入会双份占用上下文。**不展开工具 schema**——工具名已注册进工具集；但未装载清单的描述**附工具总数**（`agentDescription` 追加工具个数；工具调用前缀 `{agent}_` 由清单表头（`systemPromptInjection`）统一说明一次，不逐条重复——逐条重复随子Agent 数量线性膨胀初始上下文）——未装载的工具名不在 schema 里，描述携带的触发场景是装载前的路由匹配面（`agent_list` 同样不列工具 schema）
- 系统提示词内置**任务类型路由引导**：总层只给「装载 vs 子会话运行语义」机制说明 + 「按任务类型从下方可选子Agent 清单选用（描述即触发场景）」引导；**具体任务→子Agent 映射不硬编码**——由 `systemPromptInjection` 注入的未装载子Agent description（触发场景 + 职责边界）承载（code/self_optimize 互指边界：不处理歌白自身代码/不处理外部项目），新增子Agent 零改动自动进入路由信息（纯文本问答不装载）。**选用原则：默认 `agent_load` 装载（模块语义，工具与完整系统提示词写入当前会话记录、不创建独立执行）后直接用其工具**，仅在需要干净上下文（子任务结果隔离、不污染父上下文）、防止上下文膨胀（子任务中间过程多、输出大）或长任务并行（`async:true` 后台执行）时才用 `subsession_run` 派生子会话（会话语义，**无需装载**：隔离子会话预加载一个或多个子Agent（完整系统提示词与工具）后执行，只把最终结果带回；继承形态 fork 父上下文并行推进，报告自动合入）；执行过程子Agent 的模型回复、推理与工具调用全程实时推送前端
- 系统提示词内置**并行多路引导**：同一任务的并行多路推进（多方案对比、多文件并行修改、多角度调研等多条互不依赖的线）用 `subsession_run` 的 `inherit_context:true`（fork 父会话上下文）——一次 fork 多个子会话同时执行（各子会话掌握父会话全部背景与工具，可各自传 `model` 走不同模型接口并行），报告完成即自动合入父会话，长耗子会话 `async:true` 后台执行；并行多线是摆脱单轮串行等待、加速大体量任务的主要手段（见「子会话运行」）
- 桌面/浏览器子Agent 系统提示词内置**验证多通道降级策略**：截图黑屏/失败时切换 DOM/content、窗口状态、数据文件等通道，任一失效立即降级并告知用户，不盲目重试单一通道
- 浏览器子Agent 系统提示词内置**性能/动画采样的可见性前提**：rAF 帧间隔与 longtask 采样只在可见页有效——后台标签页 rAF 被节流到 ~1fps，采出来是「单帧 800ms+、longtask 0 个」的假数据（极易读成「动画走了合成器」）；多标签先关掉前台页，并把 `visibilityState` 记进采样结果（hidden 即作废），计时用 rAF 自排队而非 setInterval
- **系统提示词中声明会话工作目录**（会话 `tmp/`，如 `{GEBAI_HOME}/users/{user}/sessions/{s0}/{s1}/{session_id}/tmp/`）并说明**所有文件工具的相对路径以此为基准（`tmp/` 前缀可省略）**；服务端部署模式下大模型读写限定在该目录，桌面/本地浏览器模式不限制目录（同路径沙箱规则）。该声明由 `buildWorkspaceSection` 产出、作为**紧随主提示词之后的独立 system 消息**（不并入主提示词——正文含会话 ID，混入会让所有会话的前缀在会话工作目录处即分叉，见「前缀缓存稳定性」）
- 系统提示词中引导模型：复杂/多步操作优先用 `js` 脚本编排一次执行（脚本内工具像内置函数直接 await、可用变量/分支/循环表达任意流程），纯系统操作用 `sh`/`py` 脚本
- 系统提示词中引导模型：**重大任务（多步骤/有风险/不可逆/用户需要把关）先制定计划**——调用 `ask` 的计划审批分支（title+steps）把计划文档写入会话文件并在界面展示，阻塞等待用户批准后再执行（被拒绝则按修改意见修订后重新提交）；简单任务无需计划审批，`todo` 跟踪即可
- 子Agent 装载后，系统提示词实时更新；**声明依赖的子Agent 装载即连带装载其依赖**（`def.dependencies` 驱动的级联，`SubAgentManager.load` 幂等，WS `sub_agent.load`/`agent_load`/预加载所有装载路径均生效，如 `self_optimize`→`code`+`vision`、`reverse_site`→`playwright`，见「子Agent 依赖与自动装载」）；**`subsession_run` 预加载时同样连带预加载依赖**（`normalizeRunAgents` 同规则展开——装载方 def 只声明独有工具，依赖的工具与工作流提示词由依赖方 def 提供，不重复注册）
- **提示词分层职责（严格划分，防止职责越界与重复）**：
   - **总Agent 系统提示词**（`buildSystemPrompt`）只承载：身份、环境边界（工作区/沙箱）、执行策略、任务类型→子Agent 路由（紧凑映射 + 装载/子会话运行语义）、子Agent 能力注册表（`systemPromptInjection`：仅未装载轻量列表——**按会话判定未装载**（其他会话装载过不算本会话已装载，见「装载工具会话可见性」），已装载的完整提示词在会话记录的 `loadedAgent` 消息里）、`{AGENT}_PROJECT` 项目绑定声明（路由信息）。**不注入**子Agent 动态项目注记（预置项目清单、受限模式说明、项目根——属运行期上下文，随子Agent 提示词注入：`subsession_run` 子会话运行与装载写会话记录均注入）；唯一例外：**子Agent 描述动态体现预置项目摘要**（未装载清单/`agent_list` 中，`{AGENT}_PROJECTS` 配置时描述附「预置项目：名称: 说明（路径）」摘要，方便总Agent 按项目名关联任务与代码位置，完整清单注记仍只在子Agent 提示词）
   - **子会话系统提示词**（`runSubSession`/`runSubSessionLoop`）只承载：隔离声明（或 fork 子会话附注）、各预加载子Agent 的完整系统提示词（自包含）+ 职责分隔头（`### {name}（{description}）`，多 Agent 预加载时明确各段提示词对应的工具命名空间与职责域）+ 动态项目上下文（项目根/工作目录、预置项目清单、受限状态、AGENTS.md）；**动态环境注记置于职责分隔头之后、静态提示词之前**（配置信息前置——模型开工先读环境注记确定目标项目与 project 参数，再读工作流）
  - **子Agent 静态系统提示词**只承载自身职责域：本 Agent 的工作流、行为约束、工具协作说明；**不复刻其他子Agent 的内容**（如 self_optimize 不复刻 code 的通用工作流——直接引用连带装载的 code 提示词，自身只写自我优化特有流程与约束），不写实现机制说明（如工具复用/连带装载——机制由代码承担，提示词只写模型行为指令）
  - **工具 schema 描述**自包含（装载/子会话形态通用，工具描述独立成立——模型选择工具的第一信息来源，如 `project` 参数直接说明 CODE_PROJECTS 清单；子Agent 完整提示词随装载写入会话记录，但工具描述仍须自成体系，不依赖提示词解释）
  - **单源去重（上下文经济性铁律）**：schema 每轮随请求全量重发、装载后提示词与工具描述同处一个上下文，同一条信息只允许一处权威副本，其余位置单向指针——参数级语义只写在参数 description（工具描述正文不罗列参数）；领域知识表内置于对应工具描述或技能文档（如飞书 XML 语法表在 `style_guide` 技能文档，系统提示词只留指针）；安全模式降级说明单源于系统提示词（主循环与 `subsession_run` 子会话同规则注入，工具描述不重复）；「装载 vs 子会话运行」选用原则单源于总Agent 路由段（`agent_load`/`subsession_run` 描述只写机制，未装载清单头不解释用法）；子Agent 提示词写工作流取舍，不重述工具描述已有的参数级细节；子Agent 不重复定义全局已有工具（只声明独有工具，见「装载工具会话可见性」）；「无需审批」类姿态说明对模型决策零价值，不写入描述（`sh`/`py` 的 `approval:false` 传参引导除外——模型需主动传参）
  - **出站瘦身（零语义损失的部分引擎集中处理）**：工具定义每轮重发，是固定成本的主体。空 `required: []`（等价于不声明）与空 `properties: {}`（无参工具的空属性表）在**发往模型的序列化层**剔除（`llm.ts` 的 `outboundParameters`，OpenAI/Responses/Anthropic 三形态统一）——一处覆盖全部工具来源（全局/子Agent/客卿/动态），不要求每个 `schema()` 写法自洁；注册表内的 schema 不动（引擎必填校验、`tool_schemas` 查询、动态工具源码生成仍看到完整声明）。非空声明一律原样发送——它们携带工具契约，压缩会改变模型行为

#### 子Agent 提示词编写规范（8 维强化检查清单）

新增/修改子Agent 系统提示词时按以下 8 个维度逐项检查（自我优化审查提示词同样适用）；内置子Agent 参考此清单编写：

| 维度 | 检查项 |
|------|--------|
| 1. 身份与职责边界 | 开头一句话身份自述（「你是 xx 专家/助手」）+ 职责范围与边界（做什么/不做什么/运行形态限制如服务端拒绝） |
| 2. 工作流骨架 | 编号步骤：目标确认 → 探索/读取 → 方案 → 执行 → 验证 → 收尾反馈；多步骤任务用 `todo` 跟踪、重大/有风险任务先 `ask` 计划分支提交计划经用户批准后再执行、方向取舍用 `ask` 选项询问确认 |
| 3. 工具使用纪律 | 每个工具的触发场景与优先级；相似工具分工（如 `fetch_doc` 的 scope 选型：outline 目录 vs keyword 定位）；常见失败规避——**先读后写**（修改前确认当前内容，防基于过期内容操作）、**等待优先**（wait_for 目标条件，不用固定 sleep）、失败分类定位不盲目重试 |
| 4. 安全与审批 | 审批意识（写/交互/导航类操作需审批，操作前先说明方案与影响范围）；敏感数据（密钥/凭据/token 不输出明文、自动脱敏）；权限边界（只操作授权范围） |
| 5. 验证多通道 | 不依赖单一验证通道（截图/内容读取/DOM/请求记录/直连探测）；任一通道失效立即切换并**明确告知用户当前采用的验证方式**，不盲目重试同一通道 |
| 6. 协作与编排 | `subsession_run` 委托形态（委托谁、输入什么、期望产出——如 desktop 委托 code 读数据文件验证、playwright 被 code 委托做浏览器验证）；与 `self_optimize` 联动边界（仅 reverse_site 转交文档） |
| 7. 验证闭环 | 修改/操作后验证（测试/检查/断言）；失败先定位（读错误信息分类）再修，不盲目重复执行 |
| 8. 降级与诊断 | 错误分类 → 针对性处理（元素不存在/导航失败/鉴权失败/限频等）；回退/占位路径说明（如摘要失败降级为骨架行占位、OAuth 刷新失败回退应用身份） |

> 提示词只写**模型行为指令**，不写实现机制（工具内部如何实现由代码承担）；不复刻其他子Agent 的内容；保持精简——每个维度点到即止，防止提示词膨胀稀释注意力。

### 子Agent（扩展机制）

- 定义形式：**单文件** `agents/src/agents/{name}.ts`，或**目录** `{name}/{name}.ts`（`{name}/index.ts` 为回退入口）——入口可另配 `{name}.md` 系统提示词（由入口 ts 导入并修饰）；无 ts 定义时纯 `{name}/{name}.md` 即零 TS 简化定义。子代理定义一律放 `packages/agents/src/agents/`（基建组件在同级 `src/core/`，物理分域）
- 目录形式下，系统提示词 md 由入口 ts 文件**导入并修饰**（见「子Agent文件格式」）：`import systemPrompt from "./{name}.md"`（Bun 原生文本导入，构建时随 ts 一起内联进产物）
- **零注册**：文件/目录即声明，运行时自动扫描收集（跳过 `*.test.ts` 与辅助文件，目录形式只认 `{dir}/{dir}.ts`），无需任何配置文件或代码注册；**包入口 `@gebai/agents` 同样零清单**——入口只导基建工具（发现/装载链路全自动，新增子Agent = 在 `packages/agents/src/` 放定义文件即可，入口与 server 均零改动，无手工清单可漏改漂移）；**二开同理**：`custom/agents/{name}/` 放定义文件即注册（双域扫描自动合并，同名 custom 胜出——见包结构表「custom/ 二开域」）
- **热加载（目录签名失效缓存）**：`@gebai/agents` 包 `src/agents/`（子代理定义域）目录的**新增/修改/删除**在下一次装载（`agent_load`/路由自愈/`subsession_run` 预加载，`load()` 前检查）或新任务（`run()` 前）自动生效，无需重启——`refreshIfChanged()` 比对目录签名（递归 `路径:mtime`，**深度上限 2 层**——更深层的定义文件变化不触发重扫；~30 次 stat 可忽略），变化即重扫：TS 入口带 `?t={mtime}` 查询参数 import **绕过模块缓存**（修改过的文件拿到新代码；新文件本就不在缓存）；扫描域与基建物理分域（子代理在 `src/agents/`、基建在 `src/core/`——目录即语义，无需排除清单，新增基建目录不再需要改任何清单）；包入口零子代理清单（见上「零注册」），发现正确性由 server 侧发现链路测试锁定；**会话删除释放装载者引用**：`forgetSession` 经 `releaseOwner` 按会话全量解引用（owner 引用计数归零才注销工具注册——`ownersByAgent` 随会话删除回收，防长运行服务无界增长；全局装载与 `subsession_run` 共享标记不受影响）；**已装载会话沿用旧定义**（工具注册与注入会话记录的提示词保持稳定，不迁移——防运行中会话行为漂移），新定义对未装载与新会话生效；运行期显式 `unregister` 的子Agent（如 cron 关关）重扫后保持移除（`removedDefs` 过滤，防「复活」）；二进制 bundle 形态源码目录不存在、注册表不可变，无热加载。**加载失败错误透出**：扫描中 import 抛错/缺 `def` 导出/md 解析失败的子Agent 记入 `loadErrors`（随 defs 一同进程级缓存），`agent_load` 与 `subsession_run` 校验的「未知子Agent」错误**附加载失败原因**（模型可见根因——self_optimize 写错文件当场定位修复而非面对无解释的未知名，修复文件后 mtime 变化触发重扫自动恢复注册）；**`agent_load` 装载失败真实报错**（`ctx.loadSubAgent` 装载后仍未注册即抛 `unknownAgentError`——含附因；幂等重装已装载者仍成功，`loadAgentsForSession` 的单失败跳过容错仅用于会话恢复路径）。**辅助模块变更提示（开发循环高频场景）**：热加载只对**入口文件**生效（带 `?t=mtime`），入口以裸相对说明符引用的辅助模块一旦被 import 在进程内无法失效——于是「新入口 + 旧辅助」的混合版本会被当真：改完子Agent 立即自测时，新工具报「未知工具」或旧行为回放。此事实由 `SubAgentManager.hotReloadWarnings()` 给出（扫描时比对辅助模块签名与基线），并**接入三条出口**：① 未知工具报错附「辅助模块在进程运行期间被修改过…请重启服务后再使用」（否则只列旧工具清单，会被误读成「文件没写对/没注册上」而反复改文件排查）；② `agent_load` 成功返回附同一提示；③ **`subsession_run` 成功返回附同一提示**（②③ 同走 `ToolContext.subAgentHotReloadNote` 可选契约，宿主不支持则不加噪声，命中名单进 `data.hotReloadNotes`）。③ 的必要性在于 subsession_run 是子Agent 开发循环里**更常用的验证入口**（隔离上下文、直接看是否收到新提示词），而它的失败是**静默的**——工具成功、子会话也成功，只是服务的是旧版辅助模块（如新增的 `.md` 章节看不到），于是改动者只能得出「改动写错了」的结论。**`.md` 系统提示词的不对称**（实测，Bun 1.3.14）：**新增** `.md` 子Agent 目录即时生效（新文件本就不在模块缓存），**修改**既有 `.md` 则需重启——改完立刻用 subsession_run 验证会拿到旧提示词，此类验证必须结合本条提示判读。价值：self_optimize 生成/修改子Agent 后**当会话内即可 subsession_run 验证成果**，自我优化闭环不再依赖重启；而需要重启时能得到明确指引而非自行猜测。
- **热加载的边界：只对入口文件生效**（实测约束，不以推断为准）：入口 import 带 `?t={mtime}` 可绕模块缓存，但入口以**裸相对说明符**引用的辅助模块一旦被加载就在本进程内无法失效——实测（Bun 1.3.14）：即使入口与辅助同时修改，辅助模块仍返回旧值（对照：辅助模块自身带 `?t` 直接加载能拿到新值，但它被 import 时用的是裸说明符）。这意味着改辅助模块会得到「新入口 + 旧辅助」的混合版本：可能报 `Export named X not found` 这类费解错误，也可能**沉默地按旧逻辑运行**。当前处理：`auxChangeNote` 按基线识别辅助模块变更，把限制说清楚（写入 `hotReloadWarnings()`、告警日志，并在加载失败时附在 `loadErrors` 里）——**不假装能修好它**。「造世代副本」路线曾导致自激事故（副本落在被自身监视的扫描域内 → 签名变化 → 重扫 → 再复制），不再采用。
- **打包闭环**：`bun build` 前由 `packages/server/scripts/build-subagents.ts` 扫描 `packages/agents/src/agents/`（子代理定义域）生成 bundle 注册表（`src/core/subagents.bundle.generated.ts`，gitignore），全部子Agent 定义（含 md 提示词）以静态 import 内联进产物；dist/二进制模式下源码目录不可用，`discover()` 自动回退到 bundle 注册表——子Agent 真正「打包进二进制」，运行时无需读取任何子Agent 文件。**构建期逐代理验证（DESIGN「子代理失败隔离」）**：脚本对每个 TS 定义真 import 一遍，模块顶层抛错/缺 `def` 导出/`def.name` 不一致的代理**剔除出静态 import**（否则运行时顶层静态 import 任一模块失败会炸整个注册表），原因烘焙进 `bundledErrors`（运行时水合进 `loadErrors`，模型侧可见根因）；单代理失败只告警不阻断构建、不连带其他代理。**运行时降级不阻断启动**：bundle 注册表整体缺失/加载失败（构建脚本未跑/生成文件损坏）时降级为无子Agent启动（显眼告警，服务本体与全局工具正常，修复构建链路重启即恢复）——子代理失败不炸主流程，绝不因单模块问题让服务不可用。**例外与配套**：playwright 子Agent 的 `driver.mjs`（node 桥接进程，须保持独立文件）由构建脚本复制到 `dist/` 与产物同目录，运行时按 `import.meta.dir` 定位（`--compile` 形态另经 `packages/server/scripts/build-driver-embed.ts` 内嵌、物化到 `{GEBAI_HOME}/vendor/playwright/`）；playwright-core 包树经 `packages/server/scripts/build-pwcore-embed.ts` 整树 gzip base64 内嵌（`pwcore.embedded.generated.json`，gitignore），运行时物化到 `{GEBAI_HOME}/vendor/playwright-core/`（见 playwright 子Agent「依赖与部署」）。**铁律：bundle 图内的子Agent 模块禁止模块作用域的第三方包解析**（`Bun.resolveSync`、裸 `import`/`require` 等）——编译产物中这类解析锚定真实 CWD 的 node_modules 可达性，此类启动期解析失败已被构建期验证拦截（剔除出 bundle）；此类依赖须延迟到首次工具调用（playwright 经 `createLazyBridge()` 惰性单例，解析失败降级为工具级运行时错误，不影响服务启动）
- **构建期裁剪与预加载指定**（环境变量，二进制形态无法改源码、构建时定死）：`GEBAI_BUILD_SUBAGENTS`（逗号分隔包含清单，缺省 = 全部打包）按需产出精简二进制——**包含清单经依赖闭包自动展开**（include reverse_site 自动带上其 `dependencies` 声明的 playwright——运行时依赖自动装载要求依赖方在产物中存在，漏列会产出能力残缺的二进制；构建脚本动态 import 各 def 读取 `dependencies` 递归补入，依赖指向不存在的名字直接构建失败）；`GEBAI_BUILD_PRELOAD`（逗号分隔预加载清单）烘焙为 `def.preload=true`（启动即装载，运行时 `GEBAI_PRELOAD_SUB_AGENTS` 覆盖仍优先；预载逐个隔离——单个预载失败只记 loadErrors + 告警，不阻断启动与其余代理，见「子代理失败隔离」）；`GEBAI_BUILD_EXCLUDE_TOOLS`（逗号分隔**全局工具排除清单**，`packages/server/scripts/build-tools.ts` 生成 `tools-excluded.generated.ts` 烘焙）——被排除的全局工具不注册不暴露（schema 不可见、调用报未知工具），subsession_run 子会话内建编排工具（tool_schemas/js）同规则过滤（`isGlobalToolExcluded`）；语义注意：全局工具排除是**能力裁剪**——工具实现与工具表同模块仍会打包（无法摇树），体积裁剪主要来自子Agent 包含清单与内嵌产物跳过。三清单中的未知名字构建直接失败并列出可用名单（防产物静默缺失）。`tools-excluded.generated.ts` 与 subagents.bundle 不同——**提交默认空名单入库**（消费方 `core/tools/index.ts` 静态导入：运行时读文件在 `--compile` 单文件形态不可行）；`packages/server/scripts/build-tools.ts` 同时生成全局工具 bundle 注册表 `core/tools/bundle.generated.ts`（见「全局工具零注册」），该文件 gitignore，裁剪构建后为脏属预期，勿提交裁剪态。**模型配置内置**（`GEBAI_BUILD_EMBED_ENV=1`，`packages/server/scripts/build-env-embed.ts`）：把仓库根 `.env`（+进程环境）中 `GEBAI_LLM_*`/`GEBAI_VISION_*` 前缀的模型配置烘焙为二进制**启动默认值**（`env-embedded.generated.ts`，`startServer` 顶部经 `applyEmbeddedEnvDefaults` 仅填充未设置/空串的键——优先级：前端/任务级 env > 运行时环境变量 > `{GEBAI_HOME}/.env` > 内置默认），发行裁剪构建产出「开箱即用」产物；文件策略同 tools-excluded（默认空对象入库、内置构建后脏态勿提交，调用方构建脚本编译后立即还原空态——**该文件必须在版本控制中、不得写入 .gitignore**：`boot/compose.ts` 静态导入，缺文件则新克隆环境 `bun run dev` 启动即 `Cannot find module`（历史踩坑：仓库瘦身时被误随 `*.embedded.generated.json` 二进制内嵌产物一并移出跟踪）；`packages/server` 的 `dev`/`typecheck`/`build` 链前置经本脚本兜底生成，`build` 编译后再以 `--restore` 自动还原空态；并由 `core/generated-artifacts.test.ts` 守卫——`src/**` 中**静态导入**的 `generated` 模块必须存在且已被 git 跟踪，误移出跟踪当场测试报红；动态导入不在此列（构建链接管 + 运行时降级））；**安全边界：内置密钥明文随二进制分发、可被持有者提取**——仅限受信任小范围分发，建议低额度专用 Key，生成/构建日志只输出变量名不输出值
- **裁剪构建样例**（根 `scripts/` 目录）：`bun run build:code`（`build-code-agent.ts`）产出 code 场景精简**服务端**单文件二进制（`packages/server/dist/gebai-code[.exe]`，浏览器形态、内嵌 Web UI）——三层裁剪组合示范：子Agent 包含清单（code+explore，体积收益主来源：未选子Agent 模块整体摇出产物）+ 预加载清单（code 开箱即用）+ 全局工具排除清单（show/fetch_url）。可作为其他场景裁剪构建的模板：复制脚本改清单即可（如 reverse_site 站点逆向、feishu 文档、只读分析）
- 命名规则：仅限小写字母、数字、下划线
- 子代理定义模块的 import 前缀按域区分（内置域 `../../../../agents/src/agents`、二开域 `../../../../../custom/agents`，均相对发现器文件）；域目录存在但无定义文件与域目录缺失是两种状态，后者（无源码树）才回退 bundle 注册表
- 子Agent 定义的工具名无需关注前缀，总Agent 负责在其 schema 中添加 `{agent_name}_` 前缀（命名空间规则见「工具与命名空间」），以及工具调用时的路由和转发，对子Agent 完全透明
- 环境变量作用域：子Agent 配置项按 `{AGENT_NAME_UPPER}_*` 前缀**约定**声明（进入环境变量面板目录与 `envVars` 白名单口径）——**无强制的访问隔离**：TS 子Agent 工具与客卿均拿到完整 `ctx.env`，真正的硬边界是脚本子进程/js ctx 的敏感变量剔除（见「脚本隔离」「环境变量隔离」）
- 全局共享：所有用户使用同一份内置子Agent，无用户差异

#### 装载 vs 子会话运行（概念模型）

「子Agent」一词承载**两种语义迥异的操作**，代码、文档与系统提示词必须严格区分——这是消除模型误解的关键：

| 维度 | **装载**（`agent_load`，模块语义） | **子会话运行**（`subsession_run`，会话语义） |
|------|------|------|
| 类比 | import 子模块 / 静态链接 | **父子进程**：`fork`（继承父上下文）或 `spawn`（隔离新上下文） |
| 前置条件 | 无 | 无（**无需先装载**，两种操作互相独立） |
| 上下文 | **不创建新上下文**：并入父上下文 | `inherit_context:false`（缺省）= **隔离新上下文**（子Agent 提示词 + 全局工具，不继承父消息历史）；`true` = **从当前上下文 fork**（父消息历史 + 父系统提示词 + 工具面快照） |
| 效果 | 工具以 `{agent}_` schema 全名注册进当前工具集（编码类子Agent 只声明独有工具——文件读写查询直接用全局工具，见「装载工具会话可见性」）、**完整系统提示词作为 system 消息写入会话记录**（`loadedAgent` 标记持久化，`loadHistory` 时按 system 角色**前置**进模型上下文——统一置于历史最前，保持 `assistant(tool_calls)` 与其 `tool` 结果相邻，避免装载消息夹在中间被模型接口校验拒绝（实测 DeepSeek 400「tool_calls 后必须紧跟 tool 响应」）） | 派生一个或多个子会话执行任务（`agents` 可省略/为空 = **不加载任何子Agent**；需加载时给出名单，完整系统提示词拼接 + 独有工具并入；**默认继承全局工具与父会话全局提示词**——`inherit_global_tools=false`/`inherit_global_prompt=false` 可分别关闭），默认阻塞执行，`async:true` 转后台运行（`bg_task` 查询进度/等待结果/主动终止） |
| 结果交付 | —（并入父上下文，模型可见每一步） | **继承形态**：最终报告**自动合入父上下文**（合并消息 + 过程存档，无需取回动作）；**隔离形态**：最终结果作为本次工具结果返回（异步则 `bg_task wait` 取回） |
| 隔离/存档 | 无隔离、无存档（全程在父上下文） | 子会话过程**不进父上下文**（实时推送到前端 + 完整过程存档 `SubSessionArchive`）；继承形态的合入只带报告/摘要，过程仍在存档 |
| 幂等 | 幂等（重复装载跳过） | 每次运行 = 一次子会话（runId `s` + 8 位 hex） |
| 生命周期 | 装载/卸载（`sub_agent.load`/`agent_load`/预加载，会话级持久） | 一次运行即结束（运行期可查询/终止，终态记录保留最近 20 条） |

- **装载（模块）**：类比 import 子模块——子Agent 的工具注册进当前工具集（`{agent}_` 前缀）、**完整系统提示词作为 system 消息写入会话记录**（`loadedAgent` 标记，chat.json 持久化；装载后当次会话后续轮次立即进入上下文，恢复历史会话时从会话记录透传），装载后直接调用其 `{agent}_` 工具，全程在主循环/父上下文内完成，无独立执行过程。装载段落的动态环境注记与子会话形态对齐：项目绑定存在时注入「项目根: <根>（访问项目用 project 参数传该根或绝对路径；相对路径仍以会话工作目录为基准）」（限定语必要——装载模式相对路径基准仍是会话目录，不宣称工作目录已切换），无绑定时注入「工作目录: <会话 tmp>」；预置项目清单注记同款注入。**会话记录 `loadedSubAgents` 保存已装载名单**：恢复历史会话时引擎自动按名单重新注册工具（`engine.ensureSessionAgents`，幂等），实现「会话按保存的文件完全恢复状态」。**装载痕迹只看本会话记录、与进程级注册解耦**——子Agent 工具是进程级共享的（任一会话/全局装载过同名者，`SubAgentManager.load` 幂等跳过并返回空集），故 `engine.loadAgentsForSession` 按 `cascade` 闭包逐个比对会话记录补写提示词与名单，不以「本次是否新注册」判定（否则他方已装载时本会话不留任何痕迹，进程重启即丢装载状态）。预加载（`preload`/`GEBAI_PRELOAD_SUB_AGENTS`）即装载的启动期形态——启动预载的子Agent 在每个**新会话创建时自动写入**提示词消息与工具
- **子会话运行（会话）**：一套父子会话模型覆盖两种形态——隔离形态（spawn）把指定的一个或多个子Agent **预加载**进子会话（各自完整系统提示词拼接为系统提示词、独有工具以 `{agent}_` 命名空间并入工具集；**全局工具与父会话全局提示词默认一并继承**，`inherit_global_tools` 与 `inherit_global_prompt` 默认均为 true，子会话与父会话**工具面、行为约定同构**（read/write/grep/sh 等直接用全局名，文件工具带 `project` 参数路由项目），false 时分别关闭（仅预加载子Agent 工具 + 内建编排 tool_schemas/js / 仅子Agent 提示词上下文最省））；继承形态（fork）从当前上下文切片（见「子会话运行」）。执行到结束把结果交回：隔离形态经工具返回值/`bg_task`，继承形态自动合入父上下文
- **多 Agent 预加载**：`agents` 参数支持列表（如 `["code", "playwright"]`）——多个子Agent 的能力同时进入子会话（提示词拼接、工具集叠加）；每个子Agent 提示词前加**职责分隔头**（`### {name}（{description}）`）明确各自职责域与工具命名空间；各自的项目绑定/预置项目注记分别注入，工作目录取首个含项目绑定的 Agent，预置项目全量合并（同名去重）
- **代码对应**：装载 = `SubAgentManager.load`（**进程级**注册，返回本次新注册集合）/ `engine.loadAgentsForSession`（**会话级**痕迹：按 `cascade` 闭包比对会话记录写入提示词与名单，返回 `{added, dirty}`；依赖经 `cascade` 级连带装载，如 self_optimize 连带 code）/ `ToolContext.loadSubAgent` / `engine.loadAgentToSession`（装载并写入会话记录）；子会话运行 = `engine.runSubSession` / `ToolContext.subSessions`（`SubSessionRegistry`，`core/session/subsessions.ts`；工具入口 `subsession_run`）
- **模型侧引导**：`agent_load`/`subsession_run` 工具描述、系统提示词注入均按上述语义措辞，默认引导「装载后用其工具」，仅在需要干净上下文/防上下文膨胀时用隔离子会话，或在同一任务需要并行多路推进时用继承形态子会话（见「子会话运行」）

#### 装载工具会话可见性

装载的工具注册进**进程共享**的 `ToolRegistry`（卸载按 owner 解引用需要全局注册），但可见性按**会话**建模——引擎主循环不直连共享注册表，经 `sessionRegistry` 会话视图解析（每轮现算，任务中途装载下一轮 schema 即生效）：

- **会话级可见性**：`{agent}_*` 工具仅对**装载过该子Agent 的会话**可见（`SubAgentManager.visibleTo`，owner = 会话 id）；**全局装载**（启动预载/admin `sub_agent.load` 不带 sessionId，owner = `GLOBAL_OWNER`）对所有会话可见。其他会话的装载不扩散——修复跨会话泄漏：旧版任一会话装载 code 后，所有会话的请求都背上全部子Agent 工具 schema、且系统提示词目录里 code 凭空消失（`systemPromptInjection` 按进程装载状态过滤，未装载会话既无提示词也无目录引导）。目录注入同步会话化：按「对本会话可见」判定未装载，A 装载后 B 的目录仍列出 code 供 B 装载
- **子Agent 只声明独有工具（重复工具彻底删除）**：文件读写查询类工具（read/write/edit/ls/grep/glob/file/diff/sh/py）与交互编排类工具（fetch_url/todo/ask/subsession_run/bg_task）为**全局工具**——code/explore 等编码类子Agent 不再以 `projectAware` 双胞胎形态重复定义同款，只声明全局集没有的独有工具（code：search_symbols/analyze/git/preview_server/env_detect/system_info；explore：search_symbols/analyze/git）。装载与子会话运行（全局工具默认继承）均直接用全局名，`{agent}_` 前缀名只有独有工具一种形态。**取代旧版「双胞胎合并」机制**：此前子Agent 版与全局版同名共存（code_read 与 read），主会话经会话视图把同名校并为子Agent 版一份、前缀名留作解析别名——工具删重后该合并层（twinMerge/alias）整体移除，历史消息中的 `code_read` 等前缀名调用按未知工具处理（错误信息附该子Agent 可用工具清单，一步引导改用全局名）
- **全局文件工具带 `project` 参数**（`core/tools/projects.ts` 的 `projectAware` 在全局注册处统一包装）：默认会话相对路径（行为不变），操作项目须显式指定——`project` 参数传**预置项目名**或**项目根路径**（自由项目），路径即相对所选根解析（沙箱模式限定该根内）；保留名 `tmp` = 会话工作区。code/explore 的独有工具同规则包装。受限模式（`CODE_RESTRICT_PROJECTS=true`）下未传 project 的自由路径被拒绝（预置项目/绑定根放行）
- **审批姿态归一**：子Agent 不再覆写与全局同名工具的审批——全局 write/edit 维持「默认无需审批」（与总Agent 既有姿态一致，防盲写守卫约束写入安全）；子Agent `requiresApproval` 只对**独有工具**生效

#### 子Agent 依赖与自动装载（`dependencies`）

子Agent 间复用能力走**依赖声明**而非把依赖方的工具展开进自己的 def（旧版 `reverse_site` 静态导入 `createPlaywrightTools` 展开全套浏览器工具并复刻审批映射，`self_optimize`→`code` 是三处硬编码特例——现统一为声明式机制，def 声明、引擎执行）：

- **声明**：`SubAgentDef.dependencies?: string[]`（TS 定义导出 `dependencies`；纯 md 简化定义经 frontmatter `dependencies: a, b` 逗号分隔声明，条目须符合子Agent 命名规则 `[a-z0-9_]+`）。依赖方 def 只声明独有工具；依赖的工具由依赖方 def 以**其自身命名空间**注册（`{dep}_` 前缀）、`requiresApproval`/`safeMode`/`envVars` 等亦由依赖方 def 单源维护——装载方不复制任何依赖方定义
- **级联展开（`SubAgentManager.cascade`）**：返回装载某子Agent 需要的完整名单——**依赖在前、自身在后**（依赖先注册工具、先注入提示词），传递依赖递归展开，共享依赖去重；**循环依赖（含自依赖）抛错**（`子Agent 依赖循环: a → b → a`，定义缺陷暴露给模型/self_optimize 修复）；**依赖缺失跳过并告警**（被启停名单移除/构建裁剪的依赖不阻断装载方——其自身工具照常可用，仅失去该依赖能力，与旧 `self_optimize`→`code` 的 `defs.has` 守卫同语义）
- **生效路径（四条全走 cascade，语义一致）**：装载（`load`——`agent_load`/WS `sub_agent.load`/预加载/路由自愈统一入口，逐个幂等装载并返回实际装载集合，`loadAgentsForSession` 为每个连带装载的依赖同样写入提示词 system 消息）、`subsession_run` 预加载校验（`normalizeRunAgents`——同步 `runSubSession` 与异步 `SubSessionRegistry.start` 的 validate 共用，展开后名单上限检查按展开后计）、子会话循环内装载（`registerIntoRegistry`——路由自愈/显式 `agent_load` 的注册路径）、构建期包含清单（`build-subagents.ts` 依赖闭包展开，见「选择性打包」）
- **引用计数语义**：级联装载的依赖以**装载方同一 owner** 记入引用表（会话 A 装载 `reverse_site` ⇒ A 隐式引用 `playwright`）——卸载装载方**不连带卸载依赖**（A 卸载 reverse_site 后 playwright 的 A 引用仍在，直至显式卸载）；依赖被多方装载时工具注册按 owner 解引用保留（与装载工具会话可见性同机制）
- **文件读写等全局工具不构成依赖**：全局工具对所有会话恒可用（`subsession_run` 默认继承），无需也不应声明为依赖；依赖机制用于**子Agent 独有能力**的复用（如 reverse_site 复用 playwright 的浏览器操控）

#### 路由自愈（未装载命名空间工具自动装载）

模型偶尔跳过 `agent_load` 直接调用未装载子Agent 的 `{agent}_*` 工具（长会话遗忘/弱模型跳步），旧版该调用以「未知工具」失败浪费一轮。现引擎在两个循环的解析失败点做**自动装载自愈**（`engine.subAgentForToolName` 最长 `{agent}_` 前缀匹配）：

- **主循环**：按 `agent_load` 同路径自动装载（`ctx.loadSubAgent`——工具注册进全局注册表 + 提示词写入会话记录 + 拼接进当前上下文系统前置段），随后重解析执行该调用；工具结果前置说明「引擎已自动装载子Agent X」，模型知晓能力已可用
- **子会话循环**：走 `loadAgentIntoRun`——工具注册进**本次运行注册表** + 提示词段落插入临时 messages 系统前置段 + 预置项目并入 `ctx.projects`，**不写全局注册表、不落盘父会话记录**（子会话运行隔离语义：子会话内的装载不外溢）；显式 `agent_load`（纯 md 组合子Agent 注入的编排工具）在子会话内同样双注册（`buildContext.loadIntoRegistry`），装载后工具立即可解析——修复了旧版「子会话内 agent_load 只注册进全局注册表、本次运行不可见」的断层
- **不可恢复时给出恢复面**：自动装载后仍不可解析（拼错工具名等），错误信息附该子Agent 的可用工具全名清单（`unknownToolMsg`），一步引导修正
- **既有防线不放宽**：自动装载后的调用照常走通道禁用/安全模式/审批全部门禁；主动 `agent_load` 仍是首选路径（系统提示词引导不变），自愈仅为兜底

#### 选择性打包

构建时按需选择打进二进制的子Agent 集合，支持三种方式（可组合）：

| 方式 | 配置 | 说明 |
|------|------|------|
| 全部打包 | 默认 | 打包 `@gebai/agents` 下所有子Agent |
| 白名单 | 构建脚本设 `GEBAI_BUILD_SUBAGENTS=a,b,c` | 仅打包指定子Agent（依赖闭包自动展开），控制二进制体积 |
| 预加载 | 构建脚本设 `GEBAI_BUILD_PRELOAD=a` | 烘焙 `def.preload=true`（启动即装载；运行时 `GEBAI_PRELOAD_SUB_AGENTS` 仍优先） |

- 未打包的子Agent 在运行时不可见（`agent_list` 不列出、无法装载/派生子会话），相当于彻底裁剪该能力
- 打包清单支持按发行形态差异化（桌面版全量、服务端精简版等），同一套源码产出不同规格二进制
- 实现状态：三个环境变量均已接通（`GEBAI_BUILD_SUBAGENTS`/`GEBAI_BUILD_PRELOAD`/`GEBAI_BUILD_EXCLUDE_TOOLS`，见「构建期裁剪与预加载指定」）；缺省全量打包，清单里的未知名字构建直接失败（防产物静默缺失）

#### 选择性预加载

- **预加载**：启动时（或会话创建时）自动**装载**的子Agent 模块（模块语义，见「装载 vs 子会话运行」），其工具进入总Agent 工具集、完整系统提示词作为 system 消息写入会话记录（`loadedSubAgents` 名单 + `loadedAgent` 提示词消息持久化）
- **按需装载**：默认未预加载的子Agent 仅注册在目录中，总Agent 通过 `agent_load`（或 WS `sub_agent.load` 带 sessionId）按需装载——装载即写入当前会话记录（工具注册 + 提示词消息；**同名子Agent 已被其他会话装载同样写入**——痕迹判定只看本会话记录），后续恢复会话时自动还原
- 通过 `preload` 字段或环境变量 `GEBAI_PRELOAD_SUB_AGENTS`（逗号分隔）声明预加载集合；未声明者**默认不预载任何子Agent**（按需装载）
- 预加载少而精：控制系统提示词与工具集规模，降低模型选择噪音；高频/核心子Agent 预加载，低频/重型子Agent 按需装载
- **内置子Agent 默认全部不预加载**（`preload = false`），完全按需装载；部署方可用 `GEBAI_PRELOAD_SUB_AGENTS` 声明预加载集合（也可在 `GEBAI_PROFILE` 的 `sub_agents.preload` 中声明）；预载在**客卿发现就绪后补跑一次**——客卿定义后台迟到，早期预载看不到它们，补跑后预载名单里的客卿子Agent 不会落空
- 会话级可通过环境变量（会话内存态或浏览器本地注入）覆盖预加载集合，按会话定制

#### 子Agent 启停名单（运行时可用性收敛）

- 环境变量 `GEBAI_SUB_AGENTS_ENABLE`（**白名单**，逗号分隔）与 `GEBAI_SUB_AGENTS_DISABLE`（**黑名单**）：启动 `discover()` 后经 `SubAgentManager.applyEnableDisable` 收敛——白名单非空时未列出的全部 `unregister`，黑名单移除名单内；两者同时配置**先白后黑**（黑名单最终生效）；名单**存为实例策略**，合并视图每次重建（客卿发现就绪、热加载重扫、运行期 `register`）都重放一次，能力面不因定义到达时序回弹（也可在 `GEBAI_PROFILE` 档案的 `sub_agents` 中声明，见「启动裁剪与领域专用模式」）
- `unregister` 语义复用 `GEBAI_TASKS_ENABLED=false` 的既有机制：已装载/预载的连带卸载工具注册（注册表不残留「模型可见但引擎不可用」的工具）、`agent_list`/系统提示词的「可选子Agent」清单、`subsession_run`/`agent_load` 名字校验、环境变量目录（`envVars` 声明面）随之完全不可见；**热加载重扫后保持移除**（`removedDefs` 过滤防「复活」）
- 与构建期选择性打包（`GEBAI_BUILD_SUBAGENTS`，见「选择性打包」）互补：打包裁剪产出精简二进制（体积收益，不可运行时恢复），启停名单在完整产物上**按部署收敛能力面**（重启改环境变量即恢复）；名单中的未知名（拼写错误、或该形态未打包）启动 `console.warn` 告警忽略，不阻断启动（客卿定义后台迟到，未知名判定推迟到客卿发现就绪后，避免把正常配置报成拼写错误）
- 预载名单（`GEBAI_PRELOAD_SUB_AGENTS`/`def.preload`）命中被移除的子Agent 时启动告警（该子Agent 不再预载；会话装载保障按未知子Agent 跳过，不中断任务）
- **启停过滤是实例级视图，不写进程缓存**：`discover()` 的进程级扫描缓存（`discoveredDefsCache`）存**未过滤全集**，`removedDefs` 过滤仅作用于当前实例的 defs——否则一个实例的启停策略会泄漏给同进程所有后续实例（多管理器场景下跨实例污染）

#### 选用原则：默认「装载」而非「子会话运行」

**选用原则：默认「装载」而非「子会话运行」。** 默认用 `agent_load` 把子Agent 装载进总Agent（工具注册进工具集、完整系统提示词注入当前上下文，模块语义、无独立执行），总Agent 直接使用其 `{agent}_` 工具，过程全程可见、无结果截断损失；`subsession_run`（无需装载，会话语义）仅在下列场景使用：

| 场景 | 形态 | 原因 |
|------|------|------|
| **需要干净上下文** | 隔离（缺省） | 子任务结果隔离：独立执行、只返回最终结果，中间过程与工具调用不进父上下文，避免污染总Agent 的推理链 |
| **防止上下文膨胀** | 隔离（缺省） | 子任务中间过程多、输出大：独立上下文执行并自行截断（结果超阈值按上下文保护规则落盘），父上下文只保留最终结果摘要 |
| **同一任务并行多路** | 继承（`inherit_context:true`） | 多方案对比/多文件并行修改/多角度调研：各子会话掌握父会话全部背景与工具，报告自动合入（详见「子会话运行」） |
| **长任务不阻塞** | 任一形态 + `async:true` | 构建/批量分析等长任务后台执行，父会话先做别的，`bg_task` 回头收结果（见「子会话运行」） |

### 子会话运行（`subsession_run`，统一父子会话模型）

**一个入口、一套注册表、一条执行路径**：`subsession_run` 按 Linux 父子进程语义统一「隔离子会话（spawn）」与「继承子会话（fork）」——两者只是 `inherit_context` 的取值差异，不再是两套机制：

| 形态 | `inherit_context` | 上下文 | 结果交付 |
|------|------|------|------|
| **隔离子会话**（spawn） | `false`（缺省） | 新建上下文：子Agent 提示词（+ 全局提示词） + 全局工具 + 子Agent 工具 + 编排工具 | 同步：最终结果作为工具结果返回；异步：`bg_task`（id 前缀 `s`）取回 |
| **继承子会话**（fork） | `true` | 父消息历史 + 父系统提示词（+ 子会话附注） + 父工具面快照（+ 追加预加载子Agent 工具） | 最终报告**自动合入父上下文**（合并消息 + 过程存档）；同步另回概要与 `bg_task` 均可见 |

- **参数面**：单任务形态 `input`（+ 可选 `agents`/`model`）与多任务并发形态 `subsessions`（每项 `{ name?, input, agents?, model? }`，1-8 项）**二选一**（同时给出报错）；调用级开关 `inherit_context`/`async`/`merge`/`inherit_global_tools`/`inherit_global_prompt`/`timeout`/`env_mode`/`env` 盖章到本批全部子会话（`env_mode`/`env` 可在 `subsessions` 每项单独覆写）；`agents` **可省略/为空 = 不加载任何子Agent**（纯编排子会话：全局工具 + 编排工具即可完成搜索/汇总/改写类任务）；`timeout`（秒）为**运行时限**：到时进入**快速结束**（注入收敛指令给宽限拿结论，逾期才强制终止，见「子会话快速结束」）而非硬杀
- **环境变量自定义（`env_mode` + `env`）**：`inherit`（缺省）= **父任务 env + `env` 自定义项**（后者优先）；`clear` = **仅 `env`**（不继承父任务 env）。它同时决定子会话的**工具可见环境**与 **Provider 解析**（`runSubSession` 内 `resolveProvider(env)`）——因此可用 `env` 把子会话指向另一个端点/模型（`GEBAI_LLM_API_BASE`/`GEBAI_LLM_MODEL`/`GEBAI_LLM_API_KEY`，**优先于 `model` 路由解析**）；`model` 路由仍走 `GEBAI_LLM_ROUTES` 命名解析。两条边界得写清楚：① `clear` 清空的是**工具层可读**的父任务环境（含会话环境与任务级覆盖）；**命令子进程的 OS 基线仍由执行层自 `process.env` 合并**（`sandbox.exec` 的 `{...process.env, ...opts.env}`），所以 PATH/HOME/TEMP 不会丢——`clear` 影响的是工具读到的配置，不是命令的运行环境；② 键名须为标识符形式、值仅允许字符串，非法即报错（**不静默丢弃**——静默丢会让模型以为设了实际没设）；非法 `env_mode` 同样报错不回退。子会话状态行只展示**模式与键名**（值可能含密钥，不回传）。应用点：`engine.buildContext` 的 `subSessions.runner` 回调按 spec 计算有效 env 后交给 `runSubSession`（`core/session/subsessions.ts` 的 `envModeOf`/`envVarsOf` 负责校验）
- **继承形态的 fork 点**：`subsession_run` 工具调用时的父上下文（含已装载子Agent 的提示词与工具）——fork 快照**同步切片**（父任务此刻阻塞在本工具调用内，消息数组稳定，或并发场景下以切片时刻为准）；快照尾部可能带尚未应答的 `assistant(toolCalls)`（本轮工具批处理进行中），为悬空 toolCall **合成占位 tool 结果**（严格校验的接口要求 tool_calls 后紧跟 tool 响应），再追加子会话任务指令（user 消息）。子会话系统提示词 = 父会话同一系统提示词（单源复用）+ 子会话附注（并行职责/并发写提醒——其他子会话可能并行修改同一文件，写入前先读最新内容/不向用户提问/完成后输出最终报告）。工具面 = 父会话注册表视图快照（全局工具 + 已装载子Agent `{agent}_` 工具 + 会话动态工具）；子会话内装载子Agent 只进本子会话（隔离语义，不写父会话记录）；**已读追踪 fork 快照**——子会话拷贝 fork 点的会话级已读表（防盲写/防陈旧覆盖守卫的隔离：fork 后父会话/兄弟子会话的读写互不串扰，见「防盲写守卫」）
- **隔离形态的组装**：开场白说明隔离语义与预加载名单（未预加载时明确「未预加载子Agent」）、全局工具继承说明（`inherit_global_tools`）、编排指引（`inherit_global_prompt=false` 时补兜底版）、安全模式注记、预置项目清单与项目绑定注记
- **多路模型接口（模型路由）**：`model` 参数按名解析——命中 `GEBAI_LLM_ROUTES` 配置的**命名路由**（JSON：`{"路由名": {"model", "api_base"?, "api_key"?, "api_kind"?, "max_context"?}}`）走独立端点/模型；未命中视为字面模型名（主配置基准覆盖）；缺省沿用任务级模型。多子会话各走各的 Provider 并行，分摊单路模型服务的限流与串行速度限制。路由表可配在进程 env 或会话/任务级 env（前端 env 面板同样生效）
- **合并（继承形态，自动）**：子会话正常完成即把最终报告构造为**合并消息**（role=`user` + `engineNote: "subsession"`——思考类模型不接受以 assistant 结尾的历史；内容头 `【智体·子会话「名」已合并】` 身份标记（来源自描述） + `subSessionMerged` 标记（含 runId/名/模型）+ 完整过程存档 `SubSessionArchive`（`subsession` 字段标识，历史回放渲染「🌿 子会话」折叠容器））合入父上下文：任务运行中入**引擎合并队列**（`parentMerges`），runLoop 在**工具批处理边界排空**（位于本轮 tool 结果之后追加——`assistant(toolCalls)`→`tool` 配对完整，父会话下轮模型调用即见）；异步子会话晚于父任务结束时直接落盘（下次 run 经 loadHistory 进上下文），任务收尾（run finally）冲刷队列防丢失。报告超长保留头尾截断（`SUBSESSION_MERGE_MAX_CHARS`，纯上下文保护）。失败/被终止的子会话**不合入**（过程保留在存档，`bg_task` 可取回回放）
- **合入粒度（`merge` 参数，调用级）**：`full`（缺省）=报告全文合入（上列行为）；`summary`=**摘要合入**——报告超过 `SUBSESSION_MERGE_SUMMARY_SKIP_CHARS`（1500 字符）时先经任务级模型压缩为「结论+关键发现+产物清单+建议」要点（`summarizeSubSessionReport`，复用压缩同款读空闲超时防护；短于阈值不值得一次模型调用，原文合入），合并消息头行带「（摘要合入）」标记并附「全文见子会话过程存档」注记——**父上下文只进摘要，全文保留在过程存档/`bg_task` wait 可取回**；摘要失败/空结果全文兜底（合入不因摘要失败而丢失）；子会话多/报告长时保父上下文预算（多轮合入不打爆父会话）。同步等待覆盖合入完成（注册表 done promise 在合入回调 settle 之后——工具结果返回时摘要合并消息必已入队，父会话下一轮即见）
- **双向同步（`subsession_merge` 工具，仅异步子会话运行注册——子会话唯一协作工具，双向一个入口）**：**传 `content` = 交出**——阶段性成果（重要结论/产物清单/对其他子会话有用的发现）立即合入父会话（同一合并队列/落盘路径，内容头行 `阶段性合入`、不带过程存档：子会话仍在执行、存档为活引用，完成时的最终合并才携带完整存档），广播其他子会话，**子会话继续执行不受影响**、可多次调用；**不传 = 拉取**——返回父会话自 fork/上次同步以来的**全部新消息**（父会话用户输入/回复、其他子会话合入全文 `【合并·名】`、父会话工具结果摘要，逐消息 1200 字符、工具摘要 600）。两种用法均返回父会话增量快照（`parentSnapshot`）——**合入即感知**，交出后立刻看到父会话新进展。水位（`forkAt`/`syncedAt`，存储消息数）推进保证增量式（每次只返回新内容）；合并队列中未落盘的合入（同步等待期间/父任务收尾前）一并回显并按消息 id 登记已投递（防其落盘后在下次增量重复出现）；本子会话自己的合入跳过（内容自产）。fork 水位在 fork 组装时记录（此刻存储尾部即 fork 快照的持久化等价——在途 tool 结果尚未落盘，不属父会话可见内容）。**同步运行不注册本工具**（父会话正阻塞等待，阶段性合入既无收益又与最终交付重复；调用返回不可用说明），父会话内/隔离子会话之外无此工具
- **互相感知（父会话通知注入 + 按需同步）**：子会话 fork 快照后父会话仍在演进，双向感知靠**两条通道**——①**通知注入**（自动、紧凑）：任何合入（阶段性/最终）与父会话每轮最终回复（异步场景；同步等待期间父会话阻塞在 `subsession_run` 工具内无此交错）都向**本会话其他运行中子会话**的收件箱（`SubSessionHandle.inbox`）压入通知（`【子会话感知】子会话「名」…合入父会话` / `【父会话进展】父会话回复`，截断 `SUBSESSION_NOTICE_MAX_CHARS`），子会话执行循环**每轮轮首排空**注入为 user 消息（位于上一轮 tool 结果之后，tool_calls 配对不破坏；入子会话存档可回放）；②**`subsession_merge` 按需同步**（完整）：通知只给摘要，需要父会话完整内容（如依赖兄弟子会话详细发现做决策）时增量拉取全文。子会话感知父会话与兄弟子会话的进展后可调整分工（避免重复工作/冲突）；发起方不收到自己的通知（按 runId 排除）
- **同步 / 异步**：默认阻塞等全部子会话终态（工具结果返回概要——各子会话名/状态/轮次/工具调用数，隔离形态附最终结果、继承形态报告在随后的合并消息中）；`async: true` 后台执行——立即返回 runId（`s` + 8 位 hex），父会话继续其他工作，子会话完成自动合入（继承形态）或经 `bg_task` 取回（隔离形态）并实时推送前端
- **父会话控制（`bg_task`，id 前缀 `s` 分发到子会话运行；`t` 前缀走 sh 命令任务分支）**：`status` 立即返回运行状态与**进度快照**（已执行模型回复轮次/工具调用次数/最近一条存档条目尾部——从存档活引用实时推导：引擎创建运行存档后即将同一引用交给运行句柄（`archiveHolder`），**运行中即可见并随执行增长**；`result()` 仍在运行中返回 `undefined`——终态结果语义）；`wait` 阻塞等待完成并取回最终结果与**完整存档**（挂到 `bg_task` 执行记录扩展字段供历史回放；timeout 秒内未完成返回当前进度可再次 wait，默认/上限 60 秒——上限压到 1 分钟强制按进度轮询）；`stop` **主动终止**（abort 传播进执行循环，终止前的执行过程保留在存档可回放）；`finish` **快速结束**（先礼后兵：注入收敛指令让子会话停止扩展性工作、按已有信息给出结论并自然结束——报告照常交付/合入；`reason` 写入指令、`timeout` 作宽限秒数，逾期才强制终止；状态行标「收尾中」）；`list` 列出本会话全部子会话运行（含命令任务混排，按 id 前缀标注形态）
- **待办隔离**：子会话的待办是**运行内内存清单**（不落盘、不回流、不继承父会话待办）——`todo` 工具在子会话内读写自己那份清单（跨轮次可见），`event.todo.update` 携带 `subSession: true` + `subSessionId` 标记，前端**不**把它灌进父会话待办面板（卡片在子会话容器内由工具结果渲染）
- **实时可见**：子会话执行过程与主循环同构推送（`subSession: true` + `subSessionId` = runId 的 delta/reasoning/tool 事件 + `event.subsession.start`（run 开始，含 `subsession`/`agents`/`input`/`model`/`depth`；**每轮重推、同 runId 幂等**，前端记录/过程区重建兜底）与 `event.subsession.done`（run 结束，含最终输出与错误））。**前端展示分离为「消息流单行入口 + 右侧观察面板」**：过程（推理/工具卡/多轮回复）不进消息流（一次运行展开后数屏长，插进流里会把父会话叙述顶散、并发多个 run 时几条流互相交错），消息流只留一行「🌿 子会话 · 名（模型）· ⚙ 子Agent」+ 状态摘要（运行中轮次/工具数/合入数，完成后耗时与输出摘要）+「查看过程 ›」；过程、任务指令、最终报告、阶段性合入全文集中在面板（标题栏按钮打开，左列运行清单（运行中在前）、右列详情），入口与合入通知的「查看子会话过程」都定位到该 run。过程区是**常驻 DOM**（未选中的挂在面板内隐藏 staging，`isConnected` 保持为真——否则后台 run 的工具卡与流式渲染会静默丢弃）；历史回放渲染同一形态（存档/旧版过程消息登记进面板记录，流里只出单行入口）。合并时推送 `event.subsession.merged`（含合并消息全文），前端实时渲染「子会话合入」通知条——**内容按 markdown 渲染**（报告本就是模型产出的 markdown），并附「查看子会话过程」入口；消息**落盘即 `role: "user"` + `engineNote: "subsession"`**（与其余引擎注入同口径——注入点紧贴模型调用，assistant 形态会被思考类模型 400 拒绝；见「引擎注入消息的角色约定」）。渠道层可据此区分「子会话过程」事件（如飞书渠道对子会话的 `done` 不触发最终卡片——任务结束以 `event.task.done` 为准）
- **执行记录扩展字段（存档）**：子会话运行的**全部过程内容**（输入/每轮回复/推理/工具调用与结果，含嵌套子会话存档）收集为 `SubSessionArchive`（`runId`/`agents`/`input`/`output`/`subsession`/`messages`），由 `subsession_run`（继承形态：挂在合并消息上；隔离同步：挂工具结果）或 `bg_task wait`（异步）作为**执行记录的扩展字段**（`Message.subSessionArchive`）随结果落盘——不逐条写独立消息：会话文件只有一条携带完整过程的记录，历史回放据此登记进观察面板（结果卡片输出按 **markdown 渲染**；工具卡片气泡对 markdown 容器恢复 `white-space: normal`——气泡的 pre-wrap 面向纯文本参数/输出，markdown 软换行已是 `<br>`，markdown-it 在 `<br>` 后保留的源换行若再按 pre-wrap 折行，每行之间会多出一个空行）；仅存档与前端回放：`loadHistory` 跳过（**不进入父 LLM 上下文**，上下文隔离不变）、上下文压缩与 ctxTokens 估算同样排除（压缩时带存档的记录原位保留，存档不随压缩丢失）；父会话的 `subsession_run` 执行卡片与最终结果仍属父上下文（模型可见执行与最终结果）；旧版逐条 `subAgent` 存档消息与 `agent_call` 时代存档（`LegacySubAgentRunArchive` 单 Agent 形态）仍兼容回放（历史会话不受影响）
- **实现与生命周期**：`core/session/subsessions.ts` 的 `SubSessionRegistry`（**引擎级共享句柄表**（`engine.subSessionStore`）+ 按视角过滤的薄视图，`buildContext` 注入 `ctx.subSessions`——**子会话内同样注入，指向该子会话自己**（视角 = 自己的直接子会话，孙代不越级可见），因此子会话内可再派生子会话（进程树，深度上限 3）；唯一的执行路径 `engine.runSubSession`（隔离/继承两形态组装后进入 `runSubSessionLoop`，与主循环同构：审批/重复检测/轮次上限/流式推送/截断/事件，差异只在不落盘会话消息而收集进存档、不设独立超时、待办为运行内隔离清单））；进程内异步任务，随服务重启中断（不落盘恢复）；父任务取消信号连带终止子会话（用户停止/审批拒绝，逐级传播）；会话删除连带清理（`forgetSession`——运行中先终止、句柄与合并队列移除）；审批等待遇任务已结束（异步子会话晚于父任务触发审批）按超时跳过而非崩溃；**在途流式快照隔离**：异步子会话与父任务**真正并行**（同步运行期间父任务在等工具、不流式，无此交错）——`noteStream` 对 `subSession` 标记的增量在父任务快照流式期间不写入（按 messageId 开新快照会互相整体替换，attach 恢复可能把子会话文本渲染进父任务气泡；子会话进度已有 event 推送，快照仅为 attach 兜底，父任务流式期间以父任务为准）；`clearStream` 按 `subSessionId` 定向清空（子会话轮末不误清并行父任务的在途快照）；**快速结束（先礼后兵）**见下节

#### 子会话快速结束（先礼后兵）

终止一个正在推进的子会话有两条路：**硬杀**（abort 传播进执行循环：已跑出的结论一并丢掉，只剩过程存档）与**快速结束**（注入收敛指令让模型自己收尾）。后者才留得住产出，故作为超时与收尾的缺省路径：

- **收敛指令（`subSessionFinishPrompt`）**：以 user 消息注入子会话收件箱（与其他通知同一通道——执行循环**轮首排空**，位于上一轮 tool 结果之后、tool_calls 配对完整，子会话下一轮模型调用即见），要求：停止一切新的探索与扩展性工作、不再派生子会话、不再启动长耗时操作，在宽限秒数内**直接输出最终回复**（不再调工具），内容按已有信息组织为「已完成的结论与产物（文件:行号/命令/链接）+ 尚未来得及做的部分 + 若要继续的下一步」，并如实说明不确定性
- **宽限与兜底**：触发即启动宽限计时（`SUBSESSION_FINISH_GRACE_MS` 缺省 120 秒，传值夹取到 1s~600s）——宽限内模型自己收尾则终态 `done`（报告照常交付/合入）；逾期仍在运行则**强制终止**（与 `bg_task stop` 同口径 `cancelled`，过程保留在存档）。已在收尾/已结束的重复触发为幂等（不重复注入）
- **三个触发入口**：① **主会话主动**——`bg_task action=finish`（`reason` 写明结束原因并随指令下发；`timeout` 作宽限秒数；状态行显示「收尾中」）；② **运行时限**——`subsession_run` 的 `timeout`（秒），到时触发快速结束（不是硬杀）；③ **会话任务超时收尾**——任务执行超时（提示词型，`TASK_PROMPT_TIMEOUT_MS`）不直接 `cancel`，而是 `engine.windDown(sessionId, {reason})`：批量快速结束本会话运行中的子会话并等宽限（子会话全部结束或宽限到期即止），再取消会话任务本身——取消必然发生（兜底计时器防个别运行不 settle 拖住），等待期间新派生的子会话不再开第二轮宽限（由父任务停止传播一并终止）
- **不改变主动停止语义**：用户停止（停止按钮 / REST `POST /api/v1/sessions/:id/cancel` / WS `session.cancel`）与 `bg_task stop` 仍是即时硬终止（显式终止要立刻生效）；快速结束是「因超时才收尾」的路径，不与主动停止混用
- **门禁**：单次调用子会话数 ≤ 8（`SUBSESSION_MAX_PER_CALL`）；单会话并发子会话运行 ≤ 8（`SUBSESSION_MAX_CONCURRENT`，**全树合计**——子会话里再派生同样计入本会话并发，超限拒绝并引导清理：`bg_task stop` 终止或等待）；继承深度 ≤ 3（`SUBAGENT_DEPTH`，启动前同步校验）；名字缺省 `s1..sN`、批内唯一、≤32 字符不含空白（中文名合法）；终态记录保留最近 20 条（`SUBSESSION_KEEP`，超出淘汰最旧，运行中不淘汰）；预加载清单**同步校验**（去重/`self_optimize` 连带 `code`/数量与深度上限/未知名检查，`engine.normalizeRunAgents`，同步/异步启动共用——未知名等错误立即回给模型而非留下幽灵运行）；参数与环境问题（形态二选一/数量超限/并发超限/未知名）作为工具结果文本回传，模型可修正；轮次上限与主循环同规则（不设上限）；`bg_task` 管理动作免审批（查询/等待/终止本会话后台任务）

#### 子会话运行安全限制

- **递归深度上限**：子会话内再派生子会话的嵌套深度受限（默认 3 层，进程树深度，主任务为 0），防止无限递归；异步后台运行同规则（启动前同步校验）
- **超时与快速结束**：子会话**缺省不设整体超时**（执行过程实时推送到前端、进度可见，无「无反馈空转」问题）；显式设 `timeout` 时到时走**快速结束**（注入收敛指令给宽限拿结论，逾期才强制终止，见「子会话快速结束」）。中止路径：父任务取消传播（用户停止，异步运行连带终止）、`bg_task action=stop`（即时硬终止）与 `action=finish`（先礼后兵）；挂起工具仍受工具级超时兜底（`TOOL_TIMEOUT_MS`）保护
- **轮次上限**：子会话内部工具调用轮次与主循环同一规则（不设上限，见常量参考；失控防护靠重复检测终止与父任务取消传播）
- **结果大小限制**：合入父上下文的报告按 `SUBSESSION_MERGE_MAX_CHARS` 头尾截断；隔离形态结果作为工具结果超过截断阈值时按上下文保护规则处理
- **并发上限**：单会话并发子会话运行 ≤ 8（`SUBSESSION_MAX_CONCURRENT`，全树合计，超限拒绝新运行并引导清理）；终态记录保留最近 20 条（`SUBSESSION_KEEP`）

#### 工具与命名空间

子Agent 工具注册进总Agent 工具集时采用 **`{agent_name}_{tool_name}`**（单下划线）命名空间，路由按「全局精确匹配 → 子Agent 前缀匹配」两步解析：

**为什么选单下划线（对弱模型最友好）**

- 下划线是工具名中最常见的分隔符，弱模型回显 `agent_tool` 的准确率最高；`--`（双连字符）易被缩写成单 `-`，`.`/`:` 易被改写或直接报错，均不适合低性能模型
- 命名空间分隔符不依赖模型理解，模型只需把 schema 里的名字原样回显即可

**命名约束（消除解析歧义）**

| 名称 | 规则 | 说明 |
|------|------|------|
| 子Agent 名 | `[a-z0-9_]+` | 小写字母/数字/下划线 |
| 子Agent 工具名 | `[a-zA-Z0-9_]+` | 不含 `.`/`-`/`:` |
| 全局工具名 | `[a-z][a-z0-9_]*` | 小写开头，不含 `.`/`-`/`:` |
| 总长度 | ≤ 40 字符 | 弱模型易截断长名，`{agent}_{tool}` 总长严格受控 |

**参数命名契约（蛇形）与模型误差容错**

- **全部工具输入参数名一律蛇形（`[a-z][a-z0-9_]*`）**：模型按 schema 原样回显参数键，驼峰/连字符键（`oldString`/`full-page`）是弱模型的高频出错形态，参数面统一蛇形与工具名同一约定（描述文本中引用参数名同样用蛇形，提示词不再教驼峰名）
- **容错层（`core/base/tool-args.ts`，三派发点共用）**：引擎主循环、子会话循环与 js 脚本 RPC 桥在解析/执行前对模型生成的调用做两层归一——①**工具名**：`.`/`-`/`:` 分隔符与驼峰拆点归一蛇形小写（`agent.run`/`agentRun` → `subsession_run`，已合规名恒等）；②**参数键**：按工具 schema 的属性键做**指纹匹配**（去除非字母数字 + 小写，`oldString` ≡ `old_string`），嵌套对象与数组元素按子 schema 递归，schema 未声明的键原样透传（任意透传参数不受影响）、指纹歧义（两 schema 键同指纹）只认精确匹配
- **归一时机在落盘/入上下文之前**（两循环的 assistant(toolCalls) 持久化前）：历史记录、事件推送与实际执行同用规范名——模型的风格误差一次自愈，不以「未知工具/缺少必填参数」失败浪费往返
- **不在归一范围**：工具输出 `data` 字段（消费侧为代码读取，驼峰无害且为既有契约）、跨端协议键（WS 载荷如捕获 `fullPage`、playwright 桥/driver 协议键）、内部 TS 接口与存量持久化数据（任务的 `timeoutMs` 等接口字段，入参蛇形读取后在工具内映射）；B 级嵌套约定字段（wps style/theme 等仅 description 声明的字段）读取端做「蛇形优先 + 旧驼峰兜底」双读

**两步解析（服务端确定无歧义）**

1. **全局精确匹配**：先查全局工具表，命中即全局工具
2. **子Agent 前缀匹配**：遍历已注册子Agent，取「最长匹配的 `{agent}_` 前缀」，剩余部分为工具名——注册表固定，解析结果确定，任何 `a_b_c` 形态都有唯一解

**注册期碰撞检查**（注册时校验——构建期 bundle 注册走同一路径）

- 全局工具名不得以任何 `{agent}_` 开头（与子Agent 命名空间互斥）
- 子Agent 名不得互为前缀（如 `web` 与 `web_search` 同时存在会引入前缀竞争，构建时报错）

**弱模型容错兜底**

- 精确查找失败时做**分隔符归一化重试**：将工具名中的 `-`、`.`、`:` 全部替换为 `_` 后重查，覆盖弱模型的分隔符误写
- 仍失败则返回明确错误（`unknownToolMsg` 附**该子Agent 可用工具全名清单**），引导模型下一轮修正，不静默吞错

**透明转发**

- 子Agent 内部只见自身工具名 `tool_name`，schema 注入时由总Agent 加 `{agent}_` 前缀，调用时按上述两步解析后透明转发，对子Agent 完全无感
- 同一子Agent 可被多个会话同时加载使用，无共享可变状态（工具函数为纯函数或注入依赖）

**工具卡片展示元数据（`Tool.card`，注册时声明）**

- 工具定义可声明 `card: { titleParams?, args?, codeField?, codeLang?, file?, taskIdParam? }`，经 `session.tool.get` 随 ToolInfo 下发，前端按声明渲染工具调用卡片，**不在前端硬编码工具名**：
  - **卡片头部为结构化布局**（图标：运行中**信号灯闪烁**（accent 圆点，与标题栏信号灯同款 `conn-thinking` 不规则闪烁，低性能模式同样保留）/ 完成 `✓` / 历史调用 `🛠` + 工具名 + 标题参数后缀三个 span）：标题参数**始终入头部**（`taskIdParam` 声明的任务 id 参数除外——移入参数区身份块，见下）——短值直接入标题，超长单值（>48 字符）**智能截断**（路径型保留尾部、URL 保留头部、其余保留首尾，卡片头单行省略号），截断时**悬浮 title 见全文**；实时调用、完成态更新（`appendToolResult` 重建头部）与历史重载三态共用同一渲染入口（`toolHead`）
  - `titleParams`：参数名列表，其值直接拼入卡片标题——**实际存在单参数仅显示值**（`read · src/main.ts`，省略 `key=` 前缀；按实际传入数而非声明数判定——声明多个但本次只传一个时同样裸显），多参数 `key=value`（`·` 连接）；简单工具的关键参数（如 `open` 的 url、`write` 的 path）一目了然（**路径始终可见**——项目绝对路径再长也截断入头部而非整体丢弃，edit/read/write 卡片头始终能看到目标文件）；**`project` 参数自动入卡片头**：`projectAware` 包装的工具（全局文件工具与 code/explore 独有工具）在 titleParams 前插 `project`——模型传了 project 时卡片头显示 `project=todo-app · path=src/main.ts`（相对路径不再缺项目上下文），未传时前端跳过缺失参数、卡片头形态不变（project 同时从参数区移入头部，不重复展示）
  - `args`：参数区模式——**缺省自适应**（扁平标量参数渲染为**键值行**：参数名 + 值 pre-wrap 展示，比 JSON 块更可读；嵌套结构回退 JSON 语法高亮）/ `"json"`（强制完整 JSON 高亮，标题参数不省略）/ `"kv"`（强制键值行，嵌套值紧凑 JSON 单行展示）/ `"none"`（不展示参数区，如 `env_detect`）/ `"code"`（`codeField` 参数渲染为语法高亮代码块，其余参数键值行/JSON 附注，如 `sh` 的 command；**超长内容同样先渲染后自动折叠**，不整块直显）/ `"edits"`（`codeField` 数组参数的 `{old_string,new_string}` 项渲染为**旧（红）/新（绿）对比块**；前端只读蛇形键，风格兼容统一收敛在后端归一层——多处修改编号「修改 i/n」，空串侧省略（纯新增/纯删除），形态不符回退自适应渲染；`edit` 工具因此不再显示 JSON；**edit 工具 card 声明不可用时（工具清单拉取失败/旧服务端未声明）前端按参数形态内建兜底同样渲染对比块**，长短修改一律先渲染后按阈值折叠，不回退直显 JSON）/ `"block"`（**结果直出内容块**：调用不显示通用工具卡片，结果直接渲染 blocks 内容块，如 `show`/`diff`）
  - `file`（文件卡声明，「文件展示方式」设置的**弹窗查看**模式据此渲染）：路径参数名（如 read/write/edit 的 `path`，code 子Agent 同款包装自动继承）——声明本工具的**产物 file 块**（下方文件内容卡）在弹窗查看模式下收敛为**文件链接 chip**（图标 + 文件名 + 路径 + 下载，点击弹窗查看文件）；**参数区与输出不受影响**（write 的 content 代码块、edit 的旧/新对比块、read 的输出照常渲染），图片/图表等视觉产物块照常内联（仅 file 块切换形态）；块路径为服务端解析后的真实路径（会话 `tmp/` 逻辑或项目绝对路径，见「文件预览」）。脚本桥（js/py）透传的 file 块没有可归因的外层工具名，按**块上的来源工具名**（`via`）走同一判定——桥内调用文件工具与直接调用一致，桥内的 `show` 产物照常内联
  - `taskIdParam`（后台任务工具声明，如 `bg_task` 的 `id`）：该参数值为**后台任务 id**——**不入卡片头**（从标题参数中排除，卡片头只留 `bg_task · wait` 这类简洁形态），改由**参数区身份块**单独呈现，**分两行**：首行 `后台任务 <id>`（任务类型 + 任务 id，id 等宽小字、不折行——任务定位用），次行身份文本（`命令 <命令首行>` / `子会话「调研A」`，长内容自己换行不截断）；左强调色描边 + 浅底与键值行区分，位于参数区首位（其余参数键值行随其后）；id 未登记身份时只显示首行（不凭空补），显式 `args: "json"` 声明时完整 JSON 已含 id、不重复渲染身份块。身份由**工具结果文本**登记（`task-labels.ts`：后台启动结果与任务状态行的 `taskId t… / runId s…` 行；实时结果到达即登记，历史加载时先扫一遍会话消息含子会话存档——消息窗口化按需渲染，不能依赖「启动卡先渲染」的顺序兜底；完成态参数区重渲染时自动补全）。**任务 id 本身无信息量（`t`/`s` + 8 位随机）、等待中的卡片尚无输出，身份块让等待中与完成态都能一眼看出在等什么，同时卡片头保持简洁**
  - **标题参数不在参数区重复展示**（titleParams 已入标题的键从参数区省略，如 `read` 参数区只显示 offset/limit；全部参数入标题时无参数区）；**超长参数区折叠只发生在完成态**（>800 字符收敛为「查看参数（N 字符）」折叠块，默认收起，点击展开，与输出折叠同款交互）——**执行/审批等待期完整直显不折叠**（实时调用卡（运行中信号灯闪烁）带全量参数，执行与审批时可查看全貌），结果到达（`✓` 完成态，appendToolResult）时收敛为折叠块，与历史回放同构；subsession_run/subsession_run 专用参数块不参与折叠
  - 已声明示例：全局 `sh`/`py`/`write`（code 模式 + 文件卡）、`edit`（edits 模式 + 文件卡）、`read`（文件卡）、`fetch_url`/`agent_load`/`subsession_run`/`bg_task`/`ls`/`grep`/`glob`/`file`（标题参数，`bg_task` 另声明任务 id）、`show`（block 模式）；code `search_symbols`/`analyze`（标题参数）、`env_detect`/`system_info`（无参数区）；desktop `screenshot`/`window_focus`/`key_press`/`window_list`/`clipboard_read`/`screen_info`/`ocr`/`locate`/`detect`；playwright `open`/`new_page`/`serve_dir`/`switch_page`/`close_page`/`press`/`pages`/`close`/`ocr`/`locate`/`locate_image`
  - **`subsession_run` 专用卡片**：卡片头部直接列出**全部预加载子Agent 名**（`subsession_run · code + playwright`，以 `+` 连接、不截断、省略 `key=` 前缀——后缀 span 带 `wrap` 标记允许多行完整展示）；参数区**只显示输入提示词**（任务指令全文、pre-wrap 展示，不参与自动折叠）；实时卡片完成态（`✓`）保留标题后缀，参数区不因执行完成而消失（执行中与完成后均可见）
  - **`subsession_run` 专用卡片**：卡片头部直接列出**各子会话名**（`subsession_run · 左路 + 右路（fast）`，`+` 连接、带模型路由后缀、wrap 多行；缺省名按服务端规则 `s1..sN` 补，与合并消息命名一致）；参数区**按子会话渲染小节**（每节「🌿 名（模型路由）」头 + 任务指令块，指令块复用子会话输入样式），并附运行形态提示行（继承/隔离上下文、async 后台执行、merge 摘要合入）；**不声明 card 元数据**——`subsessions` 为对象数组，通用 titleParams 路径会渲染 `[object Object]`，专用卡片由前端全权接管（单任务形态 input+agents 同一路径渲染，工具卡片测试覆盖实时/历史/空清单三态）
  - 元数据拉取失败或未声明时按默认渲染（标题仅工具名、参数区按缺省自适应规则），不影响功能

### 内置子Agent 示例：通用源码分析/修改（`code`）

作为子Agent 扩展机制的落地示例，内置「通用源码分析与修改」子Agent，定义于 `packages/agents/src/agents/code/index.ts`（@gebai/agents 包），面向**其他项目**（本地使用场景下不限制目录，可访问本机任意路径）。工作流步骤 0.5「定位路径」定下缺陷定位的顺序：活体现象先取证（能跑/点/执行的构造一次最小控制变量实验二分候选原因，拿到嫌疑位置后再读码）、后推断（读码用于设计修复与影响面，不用于猜因）；纯静态问题直接读码。歌白仓定制见「`self_optimize` 专用子Agent」的步骤 0（活体自查：前端缺陷先开页面复现、改 src 必须重建产物、服务端用独立实例 + 独立 GEBAI_HOME）。

```ts
export const name = "code"
export const description = "涉及代码编写与源码分析时装载本子Agent（不处理歌白自身代码，自我优化用 self_optimize）：新建/修改项目与功能实现、代码分析、问题定位修复；装载后按 探索→方案→修改→验证 流程执行，定点修改用 edit（可一次多处、按序应用）。输入：需求/问题描述；输出：代码修改方案与验证结果。"
export const systemPrompt = "你是源码分析与修改专家（工作流参考 opencode 编码助手）。文件读写查询（read/write/edit/ls/grep/glob/file/sh/py）与交互编排（ask/todo/subsession_run/fetch_url）为全局工具，直接用全局名调用；本子Agent 补充编码专属工具（search_symbols/analyze/git/preview_server/env_detect/system_info，以 code_ 前缀调用）。工作流程：\n0) 环境确认：开工前先读本提示词开头注入的项目环境注记（项目根/工作目录、预置项目清单、受限模式说明）——目标代码所在项目与 project 参数取值由此确定；……清单确无对应项目时用文件工具的 project 参数直接传项目根路径（自由项目），此后相对路径即相对该根解析；\n0.5) 定位路径（先取证、后推断）：缺陷/异常类的活体现象先取证——能在本机跑/点/执行的构造最小控制变量实验二分候选原因，拿到嫌疑位置再读码；读码用于设计修复与影响面，不用于猜因；纯静态问题直接读码。（完整提示词见 code.ts）"
export const tools: ToolSet = { search_symbols, analyze, git, preview_server, env_detect, system_info } // 只声明全局集没有的独有工具——文件读写查询复用全局工具（带 project 参数）
export const preload = false
```

要点：

- **工具集定位（只声明独有工具）**：code 的工具集只含全局集没有的编码专属工具（search_symbols/analyze/git/preview_server/env_detect/system_info）；文件读写查询（read/write/edit/ls/grep/glob/file/sh/py）与交互编排（fetch_url/todo/ask/subsession_run/bg_task）复用**全局工具**——装载到主会话与 subsession_run 子会话运行（全局工具默认继承）均直接用全局名调用，不再重复注册 `{agent}_` 同款（重复工具已彻底删除，见「装载工具会话可见性」）
- **工作流（参考 opencode 编码助手）**：环境确认（先读提示词开头注入的项目环境注记——预置项目清单/项目根/受限模式，确定目标项目与 project 参数，警惕与目标同名的 API 封装/适配层）→ 规划（todo 待办跟踪）→ 探索（grep/glob/search_symbols/ls/analyze/git ls-files 先定位、并行调用；grep 宽泛摸底用 files 形态、锁定后 content+context 看语境、exclude 排噪、literal 字面搜代码片段；大范围摸底可 subsession_run 委托 explore 只读探索，再精确读取——read 默认带行号、分段读附位置注记）→ 定位 → 方案（改动点清单 + `ask` 方向确认）→ 修改（遵循项目既有约定、与周围代码风格一致；`edit` 定点替换为主（可一次多处、按序应用、唯一性校验）、`write` 仅新建/整体覆盖（防盲写守卫统一覆盖 write/edit；新建大文件分段——首段 write、后续 append:true 续写），不添加无关注释/密钥，成功后无需重读验证）→ 验证（测试 + typecheck/lint，失败续修）→ 收尾（`git` 工具只读核对变更后只提交预期文件，不擅自 commit；总结先结论后细节、引用 文件:行号、失败如实报告）
- **分析工具（全局 + 独有分层）**：`read`/`ls`/`grep`/`glob`/`fetch_url` 为全局工具（描述见功能列表；grep files/count 摸底 + content/context 精读、include/exclude 过滤、literal 字面匹配、head_limit 压低上限）；独有：`search_symbols`（tree-sitter 符号搜索双模式：**定义**定位——解析函数/类/方法/类型定义，返回 文件:行号: 类型 名称，精确匹配优先；`mode=references` **引用/调用点**——叶子节点精确匹配 + 排除定义名与注释/字符串（比 grep 文本匹配少误报），梳理调用链/影响面用）、`analyze`（tree-sitter 语法结构概览）、`git`（只读 Git 检查，见「git 版本控制工具」）
- **修改工具（全部为全局工具）**：`edit`（定点替换，可一次多处、按序应用、唯一性校验）/`write`（新建/整体覆盖）两者同受防盲写守卫约束、默认无需审批（与总Agent 既有姿态一致；旧版由 code `requiresApproval` 收紧审批的双胞胎覆写已随工具删重移除）；`sh`/`py`（Python 项目执行测试与脚本）走工具自身动态审批（默认需用户审批，复用统一审批流；`approval:false` 按次免审，见「工具审批」；`sh` 的 `workdir` 参数或 `project` 参数指定命令工作目录）；`bg_task`（后台异步任务统一管理——命令任务/子Agent 运行/子会话运行，见「sh 执行」「子会话运行的异步运行」「子会话运行」）；`file` 文件管理（copy/rename/move/mkdir/delete/info 多动作，delete 动态需审批）
- **协作工具（全局）**：`ask`（需求澄清、方案取舍确认，阻塞等待用户回应）、`todo`（待办增删改查统一入口，entries 列表一次批量操作、空列表即查询，与会话待办联动）、`subsession_run`（派生子会话委托其他子Agent——如 `playwright` 做 Web 项目浏览器端验证；**不得委托 `self_optimize`**，见「职责边界」）
- **项目机制（`project` 参数，无状态逐次指定）**：操作项目必须**显式**指定——全局文件工具与 code 独有工具的 `project` 参数传**预置项目名**或**项目根路径**（自由项目），路径即相对所选根解析（沙箱模式限定该根内）；预置项目清单 = `{AGENT}_PROJECTS` 注册表 + **内置项目「歌白」**（歌白家目录；脚本调试 dev 形态下即歌白仓库根，二进制形态为 `{GEBAI_HOME}`）——本地模式恒在清单中，用户配置同名项目时以其配置为准；未传 `project` 时相对路径以会话工作目录为基准（默认语义不变）。无会话粘性默认根（旧版 `project` 工具的 use/clear 已移除——「每次调用重复传根路径」由预置项目名/相对路径本身消化，机制无状态更简洁）；`grep`/`glob`/`search_symbols` 在项目模式下递归扫描项目根（跳过 `.git`/`node_modules`/`dist` 等大型目录，限深 10 层）；**绑定项目根的子会话（`subsession_run` + `{AGENT}_PROJECT`）内文件清单基准随根切换**——`ctx.listFiles` 与路径解析基准（resolveBase）一致取项目根文件树，`search_symbols` 等以 `listFiles` 为扫描清单的工具默认扫项目根，无需显式 `project` 参数（此前清单恒为会话 tmp 子树，绑定根会话里只能搜到 tmp）；`git`/`sh`/`py`/`preview_server` 以项目根为工作目录
- **保留项目名 `tmp`（会话工作区）**：`project` 参数接受保留名 `tmp`，解析到**会话工作区**（引擎恒定注入 `ctx.sessionWorkdir`——`workdir` 在绑定项目根的会话里是项目根，`tmp` 不随之变化；未注入时回退 `workdir`）。访问会话文件（附件、中间产物、临时脚本）的显式通道——「世界切换」统一到 project 参数语义，不再依赖工具名分叉。**预置项目名不得占用 `tmp`**：启动校验拒绝（`index.ts` 扫描子Agent `envVars` 声明的 `{AGENT}_PROJECTS` 进程环境变量），前端注入的任务级 env 由 `presetProjectsFor` 同规则兜底抛错——防呆在配置期暴露，避免静默遮蔽保留名后无法访问会话文件
- **验证/环境工具（自全局工具集下沉，全局集最小化）**：`preview_server`（临时新端口启动/停止一份独立进程的 GEBAI 服务，`action=start`/`stop`，供用户验证服务端类改动——不中断当前会话，启动返回 URL/PID/日志路径；经 `projectAware` 包装以 `{workdir:true}` 暴露——带 `project` 参数时以项目根为启动目录）、`env_detect`（环境探测：平台/架构、PATH 去重、关键工具链版本 node/bun/python/git/cargo 等（缺失标记不可用）、Windows MSVC 与 WebView2 状态，指导安装缺失组件）、`system_info`（系统基础信息）——三者不注册为全局工具，以 `code_preview_server`/`code_env_detect`/`code_system_info` 命名空间暴露（`self_optimize` 经连带装载 code 一并获得），全部免审批
- **验证对象与前端热重建（`preview_server`）**：预览服务即**歌白自身**（不是任意项目的通用预览器；验证外部项目用项目自己的启动命令 + `sh` 后台任务）；`hot` 参数（缺省开，仅脚本模式）给预览进程注入 `GEBAI_DEV_RELOAD=1`，复用服务端既有开发热刷新（`DevReloadManager` → `vite build --watch`）——**改动 `packages/web` 源码后 dist 自动重建、页面自动刷新，不需要再停止/重启预览服务**（旧形态只服务既有静态产物，对旧 CSS 误判「改动没生效」即此因）；二进制形态无仓库源码，自动不开（`hotDefault = !binary`），`hot:false` 可显式关闭。启动信息与状态文件（`gebai-preview.json` 的 `hot` 字段）如实标注热重建是否开启。**副作用**：热重建写的是仓库共享 `dist`（每次构建先 `clean-dist` 清空再重建，构建窗口内页面产物引用可能短暂为空），因此开启热重建期间主实例与预览实例共用同一份产物；仅服务端改动用 `hot:false` 更稳
- **tree-sitter 语法分析（`analyze`）**：基于 `web-tree-sitter`（wasm）按语言加载语法（JS/TS/TSX/Python/Go/Rust/Java/C/C++/JSON/HTML/CSS/Bash 等 30+ 语言，语法文件来自 `tree-sitter-wasms`），输出结构化概览（导入/导出、函数/类/方法/类型定义及行号、嵌套关系），替代逐行阅读快速定位；首次使用懒加载、按语言缓存 parser；**wasm 加载双通道**——dev 模式 `require.resolve` 读 node_modules，二进制/打包模式回退构建期内嵌注册表（`analyzer-wasm.embedded.generated.json`，`packages/server/scripts/build-analyzer-wasm.ts` 生成，gzip 压缩约 3.6MB）；**报错区分两类**：语言不在映射表报「不支持的语言」（附支持列表），wasm 资源加载失败报「语法分析不可用」（提示依赖缺失/未内嵌并引导改用 read）——不再把资源缺失误报为语言不支持
- **两种项目形态（统一 project 参数，二选一）**：
  - **预置项目**：会话环境变量 `CODE_PROJECTS`（JSON 数组 `[{name, path, description?}]`）声明命名项目注册表——**工具以 `project` 参数（项目名）选择目标项目**，路径参数相对所选项目根解析；预置项目**可携带项目说明**（`description`），清单（名称/说明/路径）注入**子Agent 系统提示词**（engine 组装子Agent 提示词时注入——`subsession_run` 派生子会话与装载写会话记录（`ensureSessionAgents`）均注入，**不注入总Agent 系统提示词**；注记置于职责分隔头之后、静态提示词之前（环境信息前置，模型开工先读清单按名选项目））；**描述动态体现**：engine 的 `agentDescription` 把预置项目摘要（名称: 说明（路径））与装载后工具摘要追加进 code 描述——未装载清单（总Agent 系统提示词）与 `agent_list` 输出均展示，总Agent 装载前即可按项目名/工具能力关联任务与代码位置，完整注记（工具 project 参数用法说明）仍只在子Agent 提示词；非法 JSON 静默忽略（回退自由项目模式），同名项目去重（首个生效）
  - **自由路径项目**：`project` 参数直接传**项目根路径**（绝对/相对；`looksLikePath` 判定——绝对路径、含路径分隔或 `.`/`~` 开头；本地模式绝对直用/相对按进程 cwd 解析，沙箱模式限定用户数据目录内与预置项目同规则）——无需预配置即获得与预置项目一致的「路径相对项目根解析 + 项目根工作目录 + 项目根递归搜索」三重路由
  - 实现：项目机制统一实现在 **`core/tools/projects.ts`**（`projectAware`/`resolveProjectRoot`——引擎级基础设施）；全局文件工具在 `createGlobalTools` 注册处统一经 `projectAware` 包装（code/explore 的独有工具同规则），传入 `project`（预置名/路径形态/保留名 `tmp`，`resolveProjectRoot` 统一解析）时把 `resolvePath`/`workdir`/`listFiles` 基准切换到该根（沙箱模式限定项目内，本地模式放开）
- **项目内置（特定项目绑定）**：会话环境变量 `CODE_PROJECT`（子Agent 大写前缀 `CODE_AGENT_*` 约定）声明默认项目根——子Agent 的工作目录与路径解析以项目根为基准，系统提示词注入项目路径；沙箱模式限定项目内，本地模式放开；未设置时按用户给定路径处理。**沙箱降级**：沙箱模式下越界/绝对路径的项目配置（`{AGENT}_PROJECT` 绑定与 `{AGENT}_PROJECTS` 预置项目）解析失败时**静默跳过/回退工作目录**（不使任务失败，与非法 JSON 忽略一致），避免本机环境变量注入的越界路径拖垮服务端部署。通用机制：任意子Agent 均可通过 `{AGENT_NAME_UPPER}_PROJECT`（项目根绑定）与 `{AGENT_NAME_UPPER}_PROJECTS`（预置项目注册表，engine 解析并注入 ctx/提示词，工具自行决定是否暴露 `project` 参数）使用。**默认项目根兜底（`SubAgentDef.projectRoot`）**：`{AGENT}_PROJECT` 未配置时 def 可声明兜底解析（返回绝对路径即视为绑定）——与环境变量绑定**同语义**（子Agent 提示词「项目根」注记、`subsession_run` 子会话以其为工作目录、项目 AGENTS.md 自动注入、总Agent 侧绑定声明均生效），沙箱模式同规则拒绝（默认根通常在用户目录外，回退工作目录）；如 `self_optimize` 在脚本调试（dev）模式下按模块路径自动推导歌白仓库根（二进制模式无兜底，须显式配置 `SELF_OPTIMIZE_PROJECT`）。**总Agent 侧注入项目绑定（不含预置项目清单）**：`buildSystemPrompt` 汇总所有已注册子Agent 的 `{AGENT}_PROJECT`（「xx 子Agent 项目绑定：<根路径>（subsession_run 子会话运行该子Agent 时以其为项目根；装载模式下路径基准为会话目录，访问项目用 project 参数或绝对路径）」——仅声明绑定，不宣称装载模式下工作目录已切换）注入总Agent 系统提示词；`{AGENT}_PROJECTS` 预置项目说明（含清单）**只注入子Agent 系统提示词**（`subsession_run` 派生子会话形态，见上），总Agent 侧仅经**描述摘要**（`agentDescription`：未装载清单/`agent_list` 中描述附「预置项目：名称: 说明（路径）」）轻量体现，不注入完整清单注记；主循环 `buildContext` 合并全部预置项目供 `project` 参数路由（装载模式直接用其工具时 project 参数照常可用；清单提示词装载模式同样注入——`ensureSessionAgents` 写会话记录提示词消息时动态拼接 presetNote，与 `subsession_run` 子会话一致）
- **受限模式（CODE_RESTRICT_PROJECTS=true）**：全局文件工具与 code 独有工具（read/write/edit/ls/grep/glob/file/sh/py/search_symbols/analyze/git）**仅允许操作预配置项目**——必须携带 `project` 参数（项目名须在 `CODE_PROJECTS` 清单中），自由路径（`path`）被拒绝并提示改用 project 参数；处于 `{AGENT}_PROJECT` 绑定根内（子会话运行模式）时未传 project 放行。**默认关闭（不限制）**，可经会话/用户/进程环境变量开启；受限说明属 code 子Agent 行为约束，**只注入子Agent 系统提示词**（`subsession_run` 子会话运行时，code 静态提示词含开关含义、动态 restrictNote 注入当前开启状态），**不注入总Agent 系统提示词**——装载模式下模型走自由路径时由工具错误提示自愈引导（DESIGN「环境变量配置」）
- **环境变量一览注入提示词**：系统提示词内置「项目与环境变量配置」段落，逐条列出项目相关 env 的名称与作用（`CODE_PROJECTS` 预置项目注册表 / `CODE_PROJECT` 项目根绑定 / `CODE_RESTRICT_PROJECTS` 受限模式），模型无需猜测配置来源即可直接按名使用项目；预置项目实际清单（名称/说明/路径）仍由 engine 动态注入（见上），注记前置（职责分隔头之后、静态提示词之前）保证模型开工先读环境
- **项目约定自动注入（AGENTS.md）**：子Agent 项目根存在 `AGENTS.md`（兼容 `AGENT.md` 命名）时，engine 在 `runSubSession` 组装系统提示词时**自动读取并注入**（`\n\n项目约定（AGENTS.md，编码/维护必须遵守）:\n<内容>`，≤8KB 截断，不存在/不可读静默跳过）——code 绑定项目（`CODE_PROJECT`）与 self_optimize 绑定 GEBAI 仓库（`SELF_OPTIMIZE_PROJECT`，或 dev 模式 `projectRoot` 兜底自动推导的仓库根）均生效，模型无需先 `read` 即可遵守项目约定；预置项目（`project` 参数形态）运行期才确定目标，不自动注入（模型可用 `read` 按需读取）
- **工作区限制（按运行形态）**：
  - **桌面/本地浏览器模式**：**不限制目录**，可直接分析/修改本机任意路径的项目（用户本地工作目录、克隆的仓库等），无需先复制进 `tmp/`
  - **服务端部署**：受路径沙箱约束（见「多用户隔离与安全」），目标项目须位于会话 `tmp/` 或用户数据目录内（`CODE_PROJECT` 同样受限）
  - 任一形态下**不得修改 GEBAI 服务端自身源码**（提示词与权限双重约束，自我优化走 `self_optimize`）
- **验证闭环**：系统提示词要求修改后运行项目测试（按语言选 `bun test`/`npm test`/`pytest` 等，Python 项目用 `py`）与类型检查/lint（`bun run typecheck`/`bun run lint` 等），失败则继续修复；Web 项目需要浏览器端验证时经 `subsession_run` 委托 `playwright`
- **子会话运行模式**：总Agent 以 `subsession_run` 派生子会话（预加载 `code`）时，`code` 自主完成「规划 → 分析 → 方案 → 修改 → 验证」全流程后交回
- **预加载**：`preload` 声明该子Agent 是否默认注入——**内置子Agent 一律 `preload = false`**（按需 `agent_load` / 路由自愈），部署方可用 `GEBAI_PRELOAD_SUB_AGENTS` 指定预载集合
- 新增子Agent 只需照此模式放置定义（目录形态 `{name}/{name}.ts` + `{name}.md`，或纯 `.md` 零 TS 定义），构建期按 `GEBAI_BUILD_SUBAGENTS` 选择性打包（见「选择性打包」）

> **职责边界**：`code` 只处理外部项目。**自我优化（改 GEBAI 自身代码）由独立的 `self_optimize` 子Agent 承担**（见下节），两者工作区、审批策略、提示词均分离，避免普通源码分析流程获得修改服务端代码的权限。`code` 的 `subsession_run` 委托仅用于只读/验证类委托（如 `playwright`），不委托 `self_optimize`（防经子Agent 链间接获得服务端修改能力，边界由总Agent 路由引导「GEBAI 自身代码 → self_optimize」维护）。

### 自我优化（代码级自改进）

Agent 通过修改**自身代码**来持续改进自己，不使用记忆（memory）、技能文件（skill）等运行时注入的不稳定能力：

#### 为什么不用 memory / skill

- **不稳定**：记忆注入、技能文件依赖运行时状态与外部文件，内容漂移、不可复现、难以审计
- **不可审查**：行为改进无法 diff、无法测试、无法回滚
- **冲突**：与「多用户隔离」「单一真相源」原则相悖（记忆按谁存？技能按谁加载？）
- **代码即一切**：子Agent 文件、工具实现、系统提示词本身就是能力载体，改代码 = 改能力，天然可审查/可测试/可回滚

#### 优化对象（均为代码）

| 对象 | 位置 | 说明 |
|------|------|------|
| 子Agent 定义 | `packages/agents/src/agents/`（单文件 `*.ts` 或目录 `{name}/{name}.ts` + `{name}.md`） | 新增/修改子Agent（名称、描述、提示词、工具） |
| 全局工具实现 | 服务端源码 | 工具行为优化、新工具开发 |
| 系统提示词模板 | 服务端源码 | 总Agent/子Agent 提示词迭代 |
| 默认配置 | 环境变量默认值/常量 | 阈值、超时、默认行为的调优 |

#### 优化闭环

```
发现改进点（用户反馈/失败案例/日志分析/自身复盘；任务执行中重复试错低效而不便立即优化的，
    先 self_optimize_backlog add 暂存问题+方向（自动记会话ID），随即 ask 用户确认时机（当场修复/后续）；
    后续 list 取清单集中全面优化；
    开工先 self_optimize_journal 查历史避坑）
    → 生成修改方案（改动点清单 + 预期效果）
    → 审批（需用户确认，高风险改动用/approval）
    → 由 self_optimize 子Agent 修改代码（专用子Agent，见下；新建/修改子Agent 后立即验证注册——
      注册失败错误附带载失败原因，据因修复）
    → 运行验证（self_optimize_run_tests：先相关测试文件，确认后 test/typecheck/lint 三件套，
      失败则修复或 rollback 回滚——恢复修改并删除新建文件）
    → 设计变更回写 DESIGN.md（修改行为/接口/协议/存储布局/常量/命名规则等设计层面时同步对应章节，文档与代码保持一致）
    → 用户验证（ask 确认方式）：UI/前端类修改 → page_capture 捕获当前页面
      （模型 read 实际渲染 html + vision_analyze 分析截图（vision 子代理连带装载），dev 模式自动热更新，先请用户刷新页面）；
      服务端功能类修改 → preview_server 临时新端口验证服务（独立进程不中断当前会话，验证后停止）
    → 优化记录（self_optimize_journal append：改动/验证/结论/教训，跨会话沉淀）
    → 变更落盘：代码变更即持久化，无需额外记忆
    → 构建/重启生效（开发模式直接生效）
```

#### `ai_native_eng`（AI 原生软件工程方法论，知识型）

**知识型子Agent**：不接项目、不写代码、**无任何工具**——职责是把"以智能体为执行者、
以人为验收者"的开发形态讲清楚，并作为流程评审清单。**知识来源是霜天框架的完整实践**
（87 单元 / 760 用例 / 20 类运行期缺陷，以及对照浏览器逐像素打磨字体渲染的四次返工），
每条约束都带**实测代价**，不是推演。

七节骨架：① 为什么"AI 原生"是不同形态（会话易失、文件持久 ⇒ **写进对话等于没写**）；
② **无头闭环**（控制通道 + 截图 + 像素断言，缺一条闭环就断；20 类缺陷无一能靠读代码发现）；
③ 协同自进化（框架问题分类 → 当场修/写 BACKLOG → **沉淀位置四分流**：BACKLOG=待办、
方法论文档= reusable、子Agent 提示词=**只作索引不复述**、提交说明=缘由）；
④ 观感类改动七条硬约束 + 八步循环 + 反模式清单（**本节最贵**：四次返工换来）；
⑤ 智能体工程纪律（记忆落盘、审批留痕、**"改了没生效"三变体**、逆向验证）；
⑥ 跨平台不可见坑；⑦ 汇报纪律 · 边界诚实性。

**选型边界**（description 里写明，防误用）：需要动手改代码 → `code`；改霜天 → `shuangtian`；
本子Agent 只在"评审开发流程 / 沉淀方法论 / 给新会话指路"时装载。
**可迁移性声明**：判据层面的约束（≥2 个正交量、必须带外部参照）可迁移性最强；
具体阈值（γ 值、离散容差）**不可迁移**，各项目须自行标定。

```ts
export const name = "ai_native_eng"
export const description = "AI 原生软件工程方法论（知识型子Agent，不接项目、不写代码）……"
export const systemPrompt = systemPromptBase   // 全部内容在 ai_native_eng.md（纯知识，无工具）
export const preload = false
```

#### `self_optimize` 专用子Agent

自我优化由独立子Agent `self_optimize` 承担（与 `code` 拆分，见「职责边界」）。**工具与提示词直接复用 `code` 与 `vision`**——def 声明 `dependencies: ["code", "vision"]`（依赖自动装载，见「子Agent 依赖与自动装载」），只声明独有能力（反馈读取、测试准入、回滚、优化日志、待优化暂存、页面捕获），通用编码能力（文件/分析/修改/验证工具与「规划→探索→定位→方案→修改→验证→收尾」工作流）由连带装载/预加载的 `code` 提供（验证服务 `preview_server` 亦经 code 的 `code_preview_server` 获得），视觉能力（截图语义分析/读图）由 `vision` 子代理提供（`vision_analyze`/`vision_ocr` 等 `vision_` 前缀），**不重复注册工具、不复刻提示词**：

```ts
export const name = "self_optimize"
export const description = "优化歌白自身（自身代码/子Agent/提示词/配置；外部项目用 code）……知识/工具不足或错误重复试错、低效时也装载：不便立即优化先 self_optimize_backlog add 暂存，后续集中全面优化……修改须过测试（run_tests）并同步 DESIGN.md，失败可 rollback 回滚，优化历史经 self_optimize_journal 沉淀。"
export const tools: ToolSet = { read_feedback, run_tests, rollback, journal, backlog, page_capture }
export const requiresApproval = { run_tests: true, rollback: true }
export const preload = false          // 按需装载，非默认注入
export const dependencies = ["code", "vision"]  // 依赖自动装载：连带装载 code（编码工作流复用）与 vision（视觉能力 vision_* 复用）
export const writeGuard = (env, absPaths) => string | null   // 写范围守卫声明（见下「写范围守卫」）
export const projectRoot = (env) => string | undefined        // 默认项目根兜底（见下「项目名称与项目根」）
```

- **复用 code 与全局工具（两种路径同规则，`dependencies` 声明驱动级联）**：
  - **装载模式**：装载 `self_optimize` 时 `SubAgentManager.load` **连带装载 `code`**（幂等，WS `sub_agent.load`/`agent_load`/预加载所有装载路径均生效）——code 完整提示词写入会话记录；文件读写查询用**全局工具**（read/write/edit/grep/sh 等，带 project 参数可按名操作预置项目），分析/验证类操作用 code 独有工具（`code_search_symbols`/`code_analyze`/`code_git`/`code_preview_server` 等 `code_*` 前缀）
  - **子会话运行（`subsession_run`）**：`normalizeRunAgents` 依赖展开**自动连带预加载 `code`**（code 前置，系统提示词含两段职责分隔头——code 的通用工作流在前，self_optimize 的特有流程与约束在后），子会话工具集为**继承的全局工具** + `code_*` 独有工具 + `self_optimize_*` 独有工具（全局工具默认继承，见「子会话运行的上下文隔离」）
  - 提示词分层：self_optimize 静态提示词**只承载自我优化特有内容**（反馈输入、写范围、**三类子Agent 开发场景与目录**、设计同步铁律+产物纯净、测试准入/回滚、用户验证、git 收尾），通用工作流以「直接遵循 code 子Agent 提示词」引用（两种路径下 code 提示词均在上下文内）——符合「子Agent 静态提示词不复刻其他子Agent 内容」的分层原则
- **项目名称与项目根（内置）**：静态提示词**内置项目名称「歌白（GEBAI Agent）」**并指明「项目根以系统提示词动态注记『项目根:』为准」——具体目录由引擎按绑定动态注入，**装载与子会话两种形态均注入**（装载段落 `loadAgentsForSession` 与 `subsession_run` 的 `buildAgentSection` workNote 同款语义；装载形态附相对路径限定语）；项目根解析三态：`SELF_OPTIMIZE_PROJECT` 环境变量 > dev 模式 `projectRoot` 兜底（按模块路径自动推导源码仓库根，与写范围守卫/`run_tests`/`rollback` 同源）> 二进制模式无兜底（按用户给定路径处理）——**dev 模式无需任何配置提示词即携带仓库根目录**，绑定同时使 subsession_run 子会话以仓库根为工作目录、自动注入仓库 AGENTS.md（沙箱模式同规则拒绝，回退工作目录）
- **反馈读取（`self_optimize_read_feedback`）**：自全局工具集下沉（全局不再注册 `read_feedback`，自我优化为唯一消费方）——按用户反馈按时间倒序读取，声明进 def 同时覆盖装载模式（`self_optimize_read_feedback` 命名空间）与子会话运行环境（全局工具不在注册表，def 声明保证可用）；反馈是自我优化的核心输入通道
- **页面捕获（`page_capture`）**：仿 show 图表分支的前端配合链路——引擎发布 `event.capture.request`（含 captureId + fullPage + delay）→ 前端捕获**响应页面的整个文档**（`documentElement`，不指定元素/区域；同一浏览器打开多个页面或用户切到其它视图（如文件工作台）时，即该页面当时的渲染——验证前先确认页面处于目标视图；渲染后 DOM html 截断 300KB + modern-screenshot 截图，png/jpeg，体积压缩 ≤2MB；fullPage=true 截整页，高度上限 12000px，缺省视口；**delay 为捕获前等待毫秒数**（UI 操作/动画/异步渲染完成后截图，上限 10 秒，前端 sleep 后统一捕获 html 与截图））经 WS `capture.result` 回传 → 服务端落盘会话 `tmp/capture/`（`page-<ts>.html` + `page-<ts>.png|jpg`）并返回文件/图片内容块；模型用 `read`（code_*）读取实际渲染 html，截图主模型多模态时用 `read` 直接查看（图片内联进上下文）、否则用 `vision_analyze`（vision 子代理，agent_load 装载或依赖连带装载）分析（结果文案按能力分流引导）——**UI 修改后模型直接看到真实渲染效果**（dev 模式修改后自动热更新，捕获前提示用户刷新页面；30 秒超时返回失败提示）
- **截图的前端成本控制（长会话不卡死的关键）**：modern-screenshot 逐节点算样式并克隆，成本 ≈ 0.5ms/节点——自页面根（`documentElement`）无过滤截图时，长会话（数千至上万节点）会把**主线程阻塞数秒**（实测 1500 条消息 / 7508 节点阻塞 3.9s，用户表现为「一用就卡死」）。三层防护：① **可见区域过滤**（`makeCaptureFilter`：矩形完全在捕获区域外——上方已滚出或下方未进入——的子树直接排除，长历史里绝大多数消息因此不入渲染）；② **节点预算**（`CAPTURE_MAX_NODES = 3000`，用尽即不再纳入，为整页模式 + 超长历史的硬兜底）；③ **超时兜底**（`CAPTURE_SCREENSHOT_TIMEOUT_MS = 15s`，截图内部不可取消，超时即按截图失败返回 html，不让工具侧无限等待）。实测整页/视口两种模式在 1.5 万节点下均 < 200ms 阻塞（修复前 7508 节点即 3.9s）；html 不参与截图节点预算（自身按 300KB 截断首部，超长页面只有前部）；截图前 `requestAnimationFrame` 让出一帧，避免在掉帧时刻叠加长任务
- **视觉分析（依赖 `vision` 子代理，`dependencies` 声明复用——方式一）**：def 声明依赖后 `vision_analyze`（语义分析截图）/`vision_ocr`（读图文字）随装载/预加载自动可用——`subsession_run` 隔离子会话即使不继承全局工具也有视觉能力；analyze 的 provider 解析与全局基建同一源（组装层 `boot/compose.ts` 注册 `setVisionProviderGetter`，`GEBAI_VISION_*` 外挂视觉模型 → 多模态主模型回落——`makeVisionTool` 工厂单源）；全局 vision 工具已移除，视觉分析统一经此通道
- **写范围守卫（`SubAgentDef.writeGuard`，代码级强制而非仅提示词）**：def 声明 `writeGuard(env, absPaths)`，引擎注入 `ToolContext.writeGuard`——文件写类工具（`write`/`edit`/`file`（rename/move/delete 动作，move/rename 校验源与目标两路径））写入前以**解析后的绝对路径**调用，返回非空字符串即拒绝（作为工具结果返回引导模型调整，不抛错不落盘）。**装载模式按会话装载名单动态收集**（`sessionWriteGuard`：调用时点读会话 `loadedSubAgents`，任务中途 `agent_load` 装载后立即生效）、**子会话形态按预加载名单静态组合**（`defsWriteGuard`）——两个路径一致生效（旧实现仅子会话路径有守卫，装载路径因工具去重丢失守卫副本，现已修复）。政策内容：**默认只读模式仅允许写入 子Agent 扩展面（内置域 `packages/agents/src/`、二开域 `custom/`、客卿域 `keqing/`——三域即「沉淀为子Agent」的全部落点）与仓库级文档/配置（`DESIGN.md`/`AGENTS.md`/`AGENT.md`/`.env.example`/`README.md`）**，核心引擎源码（`core/`/`app`/`ws` 等）拒绝写入（返回拒绝说明引导改用子Agent 扩展或开启开关）；`GEBAI_SELF_MODIFY=true`（启动级环境变量）放开到仓库内任意路径；仓库根解析：`SELF_OPTIMIZE_PROJECT` 优先，dev 模式按模块路径推导，二进制模式必须显式配置；**守卫只保护歌白仓库**——仓库根之外的常规写入（会话 `tmp/` 产物等）不受限（守卫目的是保护服务端源码，不约束无关产物）。**边界（脚本通道不拦）**：守卫拦截的是文件写类工具，`sh`/`py` 脚本内的重定向/写文件不经守卫——防线为「sh 写类命令默认需审批（免审白名单仅只读/测试类，见「工具审批」）+ 提示词明令禁止经脚本写仓库文件」；`GEBAI_APPROVAL_SKIP=true` 跳过审批时仅剩提示词约束（操作者自担）
- **测试准入 + 回滚工具**：`run_tests`（在仓库根执行验证，需审批）——**`checks` 参数选择检查项**（`["test"]` 缺省：`bun test` 指定文件（files 相对仓库根；**同属一个包时自动切到该包目录执行**——`bunfig.toml` 的 md loader 与测试环境净化只在包目录生效，否则 md 被按 HTML 导入、仓库根 `.env` 注入测试进程）或 `bun run test` 全量；`["test","typecheck","lint"]` 三件套与 AGENTS.md 提交准入一致，**一次审批跑全**，按序执行首项失败即停，超时 10 分钟）；输出**始终合并 stdout+stderr**（bun test 在 Windows 把用例明细/汇总写 stderr（exit 0 亦然），只取 stdout 会丢失「跑了哪些用例、几个 pass」——准入判定看 exit code，明细供人核验）；`rollback`（恢复被修改的 tracked 文件 + **删除新建的 untracked 文件**，需审批）——新文件是自我修改的主要产物（如新建子Agent），`git checkout` 只恢复 tracked、残留会被热加载注册为破损 Agent，故先 `git clean -nd` dry-run 列出将删除的新建文件（输出如实展示）再 `git clean -fd` 清理；checkout 对新建文件本就无可恢复（pathspec 不匹配属预期，不报错）；两者 files/paths 路径参数**校验前置**（含引号/shell 元字符/百分号的条目直接拒绝——审批界面展示的是参数而非拼好的命令，不设防的拼接会把注入带过审批门），合法条目双引号包裹拼入命令（空格路径保持单参数）；文件写入经全局 write/edit（默认免审批，受防盲写守卫约束），sh/py 走工具自身动态审批（默认需审批、`approval` 参数按次免审，见「工具审批」）
- **优化日志（`self_optimize_journal`，跨会话优化记忆）**：`append` 记录一次优化（title 必填 + changes 改动清单 + verification 验证方式与结果 + outcome applied/reverted/failed + lessons 经验教训）、`list` 读最近记录（limit 默认 10，新→旧）；**action 必传**——漏传（或带 append 专属参数却传 list）时显式报错且不写盘，不静默按查询处理（缺省当查询会让「想记录」的调用拿到一份正常列表而记录丢失）——**变更管理的补丁记录落地**：git 历史只记代码变更，journal 补「为什么改 + 验证结果 + 教训」，后续优化任务开工先查历史不重复踩坑（提示词固定引导：开工 list、收尾 append）；存储 `users/{user}/self-optimize-journal.json`（与 ws-journal 同位的 gitignored 运行时数据），环形保留最近 100 条，损坏/首次从空开始
- **待优化暂存清单（`self_optimize_backlog`，离线优化）**：任务执行中因自身知识/工具不足或错误导致重复试错、低效，而不便中断当前任务深入优化时 `add` 暂存（problem 问题现象必填 + direction 优化方向；session_id 缺省自动记当前会话，供后续回溯完整上下文）；**action 必传**，漏传或与 add 专属参数冲突时显式报错且不做变更——**优化时机后移、证据先落盘，不打断当前任务；暂存后 ask 向用户确认处理时机（当场修复 / 留待后续集中全面优化）**；后续执行全面优化时 `list` 取待优化项清单（旧→新）按主题归并逐项处理（需更多上下文可读来源会话记录 `users/{user}/sessions/{ID前2位}/{第3-4位}/{会话ID}/chat.json`——本地模式可读，沙箱部署模式会话文件不可读时以暂存文本为准），完成后 `resolve ids=[…]` 移除（优化记录本身仍由 journal append 承载）；存储 `users/{user}/self-optimize-backlog.json`（与 journal 同位的 gitignored 运行时数据；**add/resolve 与 journal append 的读-改-写经进程内互斥串行化**——两者的写入均为「读全量 → 改 → 整文件覆盖」，并发调用（同一回复的多个工具调用、js 编排并行）各自读到同一旧值再写回，后写会静默覆盖前写而丢条目），解决即移除、**不设环形上限**（暂存项是待办不是历史，静默淘汰会丢待办）；触发引导内置于 def description——总Agent 任务执行中重复试错低效时装载 self_optimize 暂存或直接优化
- **验证服务（`preview_server`，经 code 命名空间 `code_preview_server` 使用）**：临时新端口独立进程（不中断当前会话），用于服务端功能类修改的验证；UI/前端类修改优先走 `page_capture` 当前页面验证
- **工作流**：通用段（规划→探索→定位→方案→修改→验证→收尾）直接遵循 code 提示词，其中步骤 0.5「先取证、后推断」在歌白仓的定制见上条；特有段：反馈输入（`self_optimize_read_feedback`）+ 开工查优化日志（`self_optimize_journal list` 相关历史与教训）与待优化清单（`self_optimize_backlog list`——有积压且本次目标就是优化时以此为工作清单）+ 离线优化（任务执行中重复试错低效不便立即优化的先 `backlog add` 暂存，暂存后 ask 用户确认当场修复或留待后续，后续集中全面优化）+ **开发场景与目录**（三类子Agent 扩展的落点与选型：内置域 `packages/agents/src/agents/{name}/`、二开域 `custom/agents/{name}/`（同名覆盖内置、可整体迁移）、客卿域 `keqing/{lang}/{name}/`（manifest + PROMPT.md + 驱动，跨语言同名合并，能力外置·权力内守）——放置即发现、改完立即验证注册）→ **设计同步铁律**（任何行为/接口/协议/存储布局/常量/命名规则等设计层面变更，必须同步更新 `DESIGN.md` 对应章节；**产物纯净**——写出的代码/子Agent 提示词/文档只描述当前完整的能力与限制，不留「何时发现/修复了什么问题」等历史注记（变更缘由归 git 提交说明与 journal，遇既有注记顺手清除））→ 修改（范围由写范围守卫代码级强制；**仓库写入一律用 write/edit 文件工具，禁止经 sh/py 脚本绕过守卫**；新建/修改子Agent 后立即验证注册——失败错误附带载原因）→ 验证（`run_tests` 测试准入，确认后 checks 三件套，失败 `rollback` 回滚（恢复修改 + 删除新建文件））→ 用户验证（UI 类 `page_capture`、服务端类 `preview_server`）→ 收尾（`git` 只读核对、不擅自 commit、`self_optimize_journal append` 记录、解决了待优化项的 `self_optimize_backlog resolve` 移除、总结先结论后细节）
- **定位定制（步骤 0，歌白仓活体自查）**：`code` 的通用步骤 0.5（先取证、后推断）在 self_optimize 中具体化为歌白自身的活体路径——**前端/交互类缺陷先开页面复现**（本地实例或 `code_preview_server` 独立端口共用 `packages/web/dist`，**改 `packages/web/src` 必须重建产物**，否则看的是旧产物、会得出「修复无效」的假结论），用 playwright 注入做一次对照把候选定位到「文件:行」再读码；**服务端类改动用独立实例 + 独立 `GEBAI_HOME`**（不碰用户数据、会话可自由试跑，启动命令见 `packages/server/package.json`），完毕回收进程与临时目录；纯静态问题直接读码。
- **与 `code` 的差异**：
  - 工作区：服务端源码（`packages/agents/src/agents/` 等，`GEBAI_SELF_MODIFY` 开启时更宽） vs 会话 `tmp/` 外部项目
  - 审批：自我优化改动影响所有用户，默认更严格（改动子Agent 定义即影响全局能力）
  - 预加载：`preload = false`，避免普通对话中总Agent 误用自我修改能力
- **协作边界**：`self_optimize` 可经 `subsession_run` 委托 `playwright` 做外部 URL（如 `preview_server` 页面）的浏览器验证；`code` 不得反向委托 `self_optimize`（防经子Agent 链间接获得服务端修改能力），**写权限仅限服务端允许范围**

- **改进点来源**：用户反馈（见「用户反馈」）、审批拒绝原因、工具执行失败、任务超时/中断日志、用户显式指令（如「你下次不要再…」）、任务执行中自身重复试错（知识/工具不足或错误导致的低效——description 引导总Agent 装载 self_optimize 直接优化，不便立即优化的先 `self_optimize_backlog add` 暂存并 ask 用户确认当场修复或后续集中处理）
- **变更管理**：每次自我修改经 `self_optimize_journal` 生成补丁记录（title/changes/verification/outcome/lessons，`users/{user}/self-optimize-journal.json` 环形 100 条，可 list 查看）；回滚走 `rollback` 工具（恢复修改 + 删除新建文件）；代码版本控制（git）即代码层优化历史，journal 补「为什么改 + 验证结果 + 教训」的决策层历史
- **生效方式**：
  - 脚本调试模式：**子Agent 定义热加载**（见「子Agent 热加载」）——新增/修改/删除子Agent 文件在下一次装载/新任务前即时生效（self_optimize 改完子 Agent 后当会话内即可 `subsession_run` 验证成果）；核心引擎源码修改仍需重启进程生效
  - 二进制模式：无源码目录，热加载不适用（bundle 注册表不可变）；修改后的代码进入下次构建；运行期经会话级环境变量可覆盖**可配置项**（模型/路由/CV 路径/`{AGENT}_*` 等）实现即时调优——**子Agent 静态提示词不在可覆盖范围内**（随 bundle 固化，改动须重新构建）
- **安全约束**：自我修改走统一审批流（写操作逐次审批）；修改范围由**写保护闸门代码级强制**——默认只读模式仅限子Agent 扩展面（内置域 `packages/agents/src/`、二开域 `custom/`、客卿域 `keqing/`）与仓库级文档/配置，核心引擎源码默认只读（`GEBAI_SELF_MODIFY=true` 显式开启，见「写保护闸门」）。**提示词末尾附写范围的“实际状态”行**（策略写正文、状态写末尾）：守卫读的是启动级 `process.env.GEBAI_SELF_MODIFY`，而提示词正文是静态策略描述——不附状态行的话，`.env` 已配 `true` 的进程里模型仍以为核心源码不可写而绕路（实测踩过）；状态行由 `selfModifyEnabled()` 在模块加载时求值，两个分支都有单测。
- **测试门槛**：任何自我修改必须通过相关测试（`run_tests`）才能视为完成，防止退化；测试失败用 `rollback` 回滚本次改动（测试策略与覆盖率门槛见「测试策略」）
- **用户验证**：修改通过测试后，`self_optimize` 用 `ask` 询问用户是否启动验证服务（`preview_server`，临时新端口独立进程，不中断当前会话），确认后启动并告知访问 URL；验证结束（或用户拒绝）即停止，避免残留进程

### 会话管理
- **会话管控（`session` 全局工具，跨会话控制平面）**：与 `subsession_run`（同步派生执行，父子绑定）互补的**脱管**会话操作面——目标会话与当前任务无父子关系，创建/操作后由会话自身事件循环驱动，工具只做控制平面动作。**动作面**：`create`（新建会话，可选 `name`/`env`——env 白名单化后经 `store.setEnv` 预置为内存态会话 env，零落盘、服务重启即空）、`list`（本用户会话清单含运行态标记）、`status`（单会话详情：运行态 + 消息尾部（默认 12 条可调，跳过 system）+ 待决交互提示 + ctx 水位）、`send`（注入用户消息触发执行——**注入即返回**：仅同步等待启动早期错误（~2 秒窗口，会话缺失/存储异常如实抛出），之后立即放行目标会话后台运行——控制会话派发长任务 worker 不被拖住；目标运行中拒绝（同会话并发 run 竞态）并指引先 stop/等待；目标空闲才触发 `engine.run`）、`stop`（软终止：`engine.cancel` 解开全部挂起等待，已产生消息均落盘）、`delete`（进 GC 归档；运行中拒绝并指引先 stop）。**自保护**：对当前会话自身（`ctx.sessionId`）的一切动作拒绝（终止自己=工具结果无处落盘；注入自己=与任务循环竞态），`create` 的新会话无亲缘不受此限；服务模式仅本用户会话可见/可操作（`store.load` 兼做归属校验）。**服务注入**：`ToolContext.sessions`（`SessionControlService`：store/engine 双柄 + 脱管 `run` 入口，`buildContext` 统一注入——子会话上下文同注入，管控面不区分父子）；工具层不直接引用引擎内部类型。应用场景：驻留演进（sticky 任务续跑的工作会话观察/干预）、多会话编排（主控 + 工作会话组）、演进会话健康监控
- 会话按用户持久化到 `{GEBAI_HOME}/users/{user}/sessions/{s0}/{s1}/{session_id}/chat.json`（`{s0}`/`{s1}` 为会话 ID 自身 hex 前缀分片——前 2 位/第 3-4 位，肉眼可从 ID 推路径，见目录结构）
- **会话记录保存子Agent 装载状态**：`loadedSubAgents` 字段（已装载名单）+ `loadedAgent` 标记的 system 消息（完整提示词，UI 渲染为简短装载提示）；**会话级装载痕迹以本会话记录为准（与进程级工具注册解耦，见「装载（模块）」）**；恢复历史会话时引擎按 `loadedSubAgents ∪ 装载提示词痕迹 ∪（仅 `undefined` 的新会话/旧格式才落到）启动预载名单` 重新注册工具并补齐提示词与名单（`ensureSessionAgents`，幂等）——**进程重启后装载状态由此自动恢复，无需模型重新 `agent_load`**；卸载到空保留 `[]`（区别于「从未初始化」的 `undefined`），卸载不会因重启被预载名单复活；新会话首次运行按启动预载名单（`GEBAI_PRELOAD_SUB_AGENTS`）初始化，未配置默认不预载任何子Agent
- 会话归属校验：仅会话所有者可访问（服务模式）
- 列表查询不为元信息解析正文：走同目录 `meta.json` 指纹缓存（见下条），`chat.json` 仅在缓存缺失/指纹不符时解析
- **列表元信息缓存（`meta.json`）**：会话列表只消费标题/时间/置顶/上下文用量，若每查询都解析 `chat.json` 全文，开销随历史体量正相关（数十个会话可达数十 MB）。与 `chat.json` 同目录维护 `meta.json`（`id`/`name`/`userId`/`createdAt`/`updatedAt`/`pinned`/`ctxCachedTokens`/`ctxTokensFallback`/`messageCount` + 正文指纹 `source: {size, mtimeMs}`），`save()` 同步刷新；`listSessionInfos()` 命中缓存则直接返回，**文件缺失/损坏/指纹不符（陈旧）时回退读正文并就地重建**——**可重建的缓存、不是真相源**（`chat.json` 恒为准，外部编辑/旧版本写入/中途崩溃均自动纠正），存量会话首次列表自动建立；`listSessions()`（返回完整 `SessionData`）保留给需要正文的调用方（GC/归档等），列表类消费方（WS/REST 列表、快照、文件工作台目录选择、飞书会话命令）走 `listSessionInfos()`
- 支持会话创建、切换、重命名、置顶、删除、导入（见下条「会话导入/导出」）
- **会话导入/导出（往返 JSON，与 Markdown 导出并存）**：跨实例/跨用户迁移会话。**格式「批量包」（version 1）**：单/批量统一为 `{ format: "gebai-sessions", version: 1, exportedAt, sessions: [...] }`，单个 = sessions 数组一项；条目只携带 `name/createdAt/updatedAt/pinned/messages/todos/loadedSubAgents`，**不携带 id/userId/ctx 统计**——导入时服务端分配新 id、导入者成为 owner（`SessionStore.importSession`，校验：name 必填去空白截 80、messages 上限 MAX_CACHE_MESSAGES 超限保留末尾、role 合法枚举、todos 宽容归一、未来时间夹列当前），跨实例导出/导入无 id 冲突、不可指名注入会话 id。**通道**：WS `session.import`（payload `{ session }` 单会话条目，前端批量 = 循环逐个）与 REST `POST /api/v1/sessions/import`（同载荷，注册在 `/sessions/:id` 白名单中间件**之前**——单段路径词 `import` 否则被当 `:id` 拦下 400）；服务模式归属导入者、未登录 401。**前端**（纯逻辑 `web/src/session-io.ts`，可测）：**轮盘「导出」= 当前会话 JSON 往返文件**（与轮盘导入同一格式，两端联动）；右键菜单「导出会话」（单会话 JSON，文件名 `gebai-session-{会话 ID}.json`）+「导出 Markdown」（阅读版，原有，按标题命名）；批量选择条「导出所选」（选中会话打包单文件 `gebai-sessions-{N}会话-{yyyymmdd-HHmmss}.json`——批量无单一 ID 可依，按「条数 + 导出时刻」命名，**时间戳到秒**：只写日期的话同日导两次、条数相同即完全同名，只能被浏览器追加 `(1)`/`(2)`，事后分不清哪批是哪批）；轮盘内圈「导入会话」（多选 JSON + 单文件多会话统一解析）→ confirmDialog 预览清单 → 逐个导入 → toast 汇总 → 刷新列表并自动切到最后导入成功的会话（导入即达）
- **会话移除标记与主动重建**：会话被删除或 GC 归档后，存储层按 id 置移除标记（内存态，进程重启即清）——此后 `save()` 拒绝为该 id 落盘（**fail-closed**：防运行中任务收尾/上下文压缩/任务结果写回等仍持旧 `SessionData` 的陈旧引用把已删/已归档会话连同数据整体复活）；**主动重建走专属通道 `recreateSession(session, { clean? })`**（清除移除标记后落盘，`clean` 先删旧目录含 `tmp/` 附件）——确定性会话 id 的通道在会话消失后以同 id 重新开始时用它（飞书 chat_id → 会话 id 的会话重建、`/new` 清空重建），两类调用方由此区分，而非放宽 `save()`
- **会话置顶**：`pinned` 字段（chat.json 持久化，未定义 = 未置顶，旧格式天然兼容）标记重要会话；列表查询置顶优先、组内按更新时间倒序；置顶/取消为元数据操作，**不刷新 `updatedAt`**（不动排序基线，旧会话置顶不会跳入时间分组）；前端「置顶」独立分组脱离时间分组展示
- 支持会话级的审批跳过（`/approval-skip`）

### 用户反馈

用户可对任意 Agent 回复（总Agent/子Agent 消息）提交反馈，用于质量追踪与改进：

- **反馈类型**：
  - **点赞/点踩**：对单条回复快速评价，前端弹层附**可选原因标签**（👍 优秀/有用/完整，👎 错误/不完整/不符合预期）与**补充说明文字**
  - **文字反馈**：附带详细说明，如指出错误、期望的修正、补充背景（类型保留，经 API 提交）
  - **建议改进**：对功能/产品层面的建议（新工具、子Agent 需求等）（类型保留，经 API 提交）
- **关联上下文**：反馈自动关联所属会话与消息 ID；落盘时服务端按会话记录**自动补 model**（assistant 消息落盘携带的模型名，见「消息模型与数据结构」）与 **subAgent**（会话当前装载的子Agent 名单，逗号连接）——尽力关联不阻断提交（会话不存在/读取异常时反馈照常写入，仅缺关联字段）
- **存储**：持久化到 `{GEBAI_HOME}/users/{user}/feedback/YYYY-MM-DD/{h0}/{h1}/{feedback_id}.json`（按日期 + ID 自身前缀分片，见目录结构），仅本人可见（服务模式）
- **前端**：助手消息操作组内 👍/👎 按钮（与复制/撤回/重新生成同排）点击打开弹层（标签单选可反选 + 补充说明，提交成功禁用防重复、失败恢复可重试）；管理员可在设置面板「反馈」页查看（含模型/子Agent 关联信息）并**导出 JSON**
- **用途**：反馈数据供改进系统提示词、评估模型表现、排查工具问题（`self_optimize_read_feedback` 读取进入自我优化闭环，见「自我优化」）；管理员可导出分析

### 环境变量配置

所有配置统一采用**环境变量**形式（`GEBAI_*` / `OPENAI_*` / 子Agent `{AGENT_NAME_UPPER}_*` 等），无独立配置文件（`.env` 仅是环境变量的文件来源：脚本调试=仓库根、二进制=`{GEBAI_HOME}/.env`，真实环境变量恒优先）。**用户/会话环境变量服务端零留存**（不落任何 env 文件；**例外**：任务级 `env` 是用户显式随任务保存的配置，持久化于 `users/{user}/tasks.json`——敏感键名的值加密落盘，见「统一任务管理 → 任务级环境变量」），层级与存储：

| 层级 | 来源 | 存储 | 说明 |
|------|------|------|------|
| 全局 | 进程环境变量（启动时注入，`.env`/系统环境） | 服务端启动配置 | 服务端默认配置，所有用户/会话共享 |
| 浏览器本地 | 前端设置面板 | **浏览器 localStorage（`gebai.ui.env`）——用户环境变量唯一持久化位置** | 随每条 prompt 临时注入为任务级覆盖，清除站点数据即清除 |
| 会话内存态 | env 接口 / 飞书命令运行中设置 | **服务端内存（不落盘，进程重启即空）** | 仅供运行中即时生效类开关（如自动审批）；由前端每次加载会话自行重新同步所需键 |
| 任务级 | 任务定义（`task_add`/`task_update` 的 `env`、REST body、前端任务表单） | **服务端用户目录 `users/{user}/tasks.json`（用户显式保存的配置；敏感键值 AES-256-GCM 加密落盘）** | 仅该任务的每次执行注入（脚本子进程 / prompt 型执行会话），优先级最高；无人值守运行不依赖浏览器发包（见「统一任务管理 → 任务级环境变量」） |

**admin 密码引导**：服务模式不设注册表引导（**不落明文密码**）——启动参数 `GEBAI_ADMIN_PASSWORD_HASH="salt:hash"`（scrypt 加盐哈希，`bun run --cwd packages/server hash-password` 生成）**设置则启用 admin（覆盖其哈希），不设置则 admin 被禁用**——admin 唯一入口即此参数（不可注册创建）；admin 禁用不影响普通用户（登录页可自助注册，注册用户恒为普通角色）。

#### 前端环境变量（浏览器本地，不保存到服务端）

- **前端设置面板配置的环境变量保存在浏览器本地（localStorage，键 `gebai.ui.env`）**，对本浏览器所有会话生效，不是每个会话单独配置，**不写入服务端**（用户环境变量服务端零留存——不落任何 env 文件）——敏感配置（密钥等）只存在用户自己的浏览器，防服务端侧泄露
- 发送消息时由前端随 `POST /api/v1/sessions/:id/prompt` 请求（body `env`）/ WS `session.prompt`（payload `env`）**临时注入**服务端，仅本次任务生效（引擎合并进本次运行的 env），请求结束即失效，服务端不持久化
- **前端配置模型相关变量即可使用模型**（服务端可不配置任何模型变量）：`GEBAI_LLM_MODEL`（主模型）/`GEBAI_VISION_MODEL`（视觉模型）等 `GEBAI_LLM_*`/`GEBAI_VISION_*` 注入后按任务生效——任务启动时按合并后 env 重建 Provider 覆盖服务端启动配置（见「覆盖规则」），未配置的项沿用服务端启动配置
- 名称与权限宽容过滤：prompt 的 `env`（浏览器本地注入通道）对不支持/非法的变量**直接跳过、不拒绝任务**（`filterEnvInjection`：丢弃非法标识符名/`__proto__`/非 string 值）——前端 localStorage 可能残留旧版目录外键，拒绝整个任务会阻断正常使用；显式管理通道（REST `PUT /env`、WS `session.env.set`）仍严格校验（`validateEnvVars` 非法返回 400/错误应答）
- **目录驱动、不可自定义**：前端设置面板从服务端 `GET /api/v1/env/catalog` 拉取环境变量目录（白名单，含变量作用说明），**按「全局 / 各子Agent」分组展示**（全局：模型相关 `GEBAI_LLM_*`/`GEBAI_VISION_*`、审批跳过、代理/时区等；子Agent 组：`{AGENT_NAME_UPPER}_*` 前缀变量如 `CODE_PROJECTS`/`SELF_OPTIMIZE_PROJECT`/`FEISHU_DOCS_APP_ID`）；**子Agent 组由各子Agent 导出 `envVars` 声明汇总**（`core/env-catalog.ts` 不做子Agent 硬编码，新增子Agent 环境变量只需在子Agent 定义中声明，见「子Agent文件格式」）；**未配置的项显示为空、请求不携带**；鼠标悬停（tip）显示变量作用
- **不支持自定义变量名**：只能配置目录内的项（保存时过滤目录外旧值）；启动级与安全敏感变量（`GEBAI_MODE`、`GEBAI_ADMIN_PASSWORD_HASH`、`GEBAI_SAFE_MODE`、`GEBAI_SANDBOX`、`GEBAI_HOST/PORT` 等）不在目录中，天然不可配置
- 清除浏览器站点数据（localStorage）即清除该配置

#### ask 填值分支（模型驱动的前端填值，原 ask_env）

- **工具缺少必需环境变量时报错给模型**（错误信息含缺失变量名、用途说明与可选的 `ask` 调用引导，如 feishu_docs 缺 `FEISHU_DOCS_APP_ID` 时提示「可调用 ask 工具（name=FEISHU_DOCS_APP_ID，secret=true）请求用户直接填写」）
- **`ask` 填值分支**：模型调用（`name` 变量名 + `description` 用途说明 + `secret` 敏感值掩码）→ 引擎发布 `event.env.request`（含 envId/name/description/secret）并**阻塞等待** → 前端渲染**填值卡片**（变量名与说明展示，secret 时密码框）→ 用户提交后：
  - **提交成功填值卡片随即关闭**（取消/拒绝同样关闭）；提交失败卡片保留并提示重试
  - 值**注入本次任务 env**（任务级 env 引用原地更新，`sh`/`py` 子进程与后续工具读取**立即生效**，不进入模型上下文——密钥不外泄给模型）
  - **变量名校验与敏感键限制**：与其余 env 写入通道同规则（`validateEnvVars`：仅 `[A-Za-z_][A-Za-z0-9_]*` 标识符名、拒绝 `__proto__` 原型污染），**服务模式下一律拒绝 `GEBAI_APPROVAL_SKIP`**（ask 填值分支是模型驱动的第四通道，不得自设审批跳过——用户本人经前端开关/env 接口/飞书命令设置）
  - 同时**保存到浏览器本地**（localStorage，后续任务自动生效）
  - 用户拒绝/超时（审批超时同值）返回失败，工具结果引导模型说明所需配置或改用其他方式
- 填值分支仅实时前端通道（合并型工具分支门控：非 realtime 明确报错并引导用户在设置面板配置）；值与其余 env 写入同规则校验（`validateEnvVars`）

#### 会话内存态环境变量（内部机制 + API）

- 会话 env 为**纯内存态**（服务端零留存）：不落盘、进程重启即空，前端每次加载会话**与每次任务启动前**自行重新同步所需键（如自动审批开关——草稿页首条消息创建的会话不经过消息加载路径，同步点覆盖两处，防子会话首个任务的开关失效）
- 会话级修改**不通过前端设置面板**（面板存浏览器本地），仅经 API（REST `PUT /api/v1/sessions/:id/env` / WS `session.env.set`，内存写入）或内部机制（如 `approval-skip` 自动审批开关按会话同步 `GEBAI_APPROVAL_SKIP`）；**写入响应与读取一致脱敏**（敏感键值以掩码返回，防明文密钥回读）
- 作用范围：会话内 LLM 调用（模型/Provider 配置，任务级生效）、工具执行（`sh`/`py` 子进程环境）、子Agent 环境变量读取，均与浏览器本地注入合并后生效
- 用途：按会话定制（`CODE_PROJECT`/`SELF_OPTIMIZE_PROJECT` 项目绑定、`CODE_PROJECTS` 预置项目注册表、预加载集合覆盖等；服务重启后此类配置由用户浏览器本地随 prompt 重新注入恢复）
- **历史数据清理**：启动时清理遗留的用户级 `users/{user}/env.json`（`cleanupLegacyUserEnv`）；会话目录遗留的 `env.json` 在该会话首次触达 env 读取时惰性删除——迁移后服务端不留存任何 env 文件
- **环境变量目录接口**：`GET /api/v1/env/catalog` 返回可配置变量白名单（按「全局 / 各子Agent」分组 + 变量作用说明），供前端设置面板渲染（不可自定义变量名）；启动级与安全敏感变量（含启动裁剪类 `GEBAI_PROFILE`/`GEBAI_PROMPT_*`/`GEBAI_TOOL_*`/`GEBAI_SUB_AGENTS_*`）不在目录中

#### 覆盖规则

- 生效顺序：**任务级（任务定义） > 浏览器本地注入（本次任务） > 会话内存态 > 全局**，同名字段取最高优先级的非空值（浏览器本地注入仅覆盖当前运行的任务，不修改任何持久化层级；任务级 env 仅在其所属任务的执行中生效）
- **启动裁剪类变量（`GEBAI_PROFILE`/`GEBAI_PROMPT_*`/`GEBAI_TOOL_*`/`GEBAI_SUB_AGENTS_*`/`GEBAI_PRELOAD_SUB_AGENTS`）不参与上述分层**：它们是**启动级事实**（进程启动时解析一次，决定提示词段落、工具表、子Agent 可见面与预载集合），浏览器/会话层同名键不参与裁剪判定，改配置需重启（见「启动裁剪与领域专用模式」）
- **模型相关配置（`GEBAI_LLM_*` 全套与 `GEBAI_VISION_*`）任务级生效**：浏览器本地/会话内存态注入在任务启动时按合并后 env 重建 Provider（`applyModelEnvOverrides`/`resolveVisionProvider`），覆盖 Provider 级（进程环境变量）配置——主循环与 `subsession_run` 子会话运行、上下文压缩阈值/摘要、附件图片内联判定、视觉分析均生效；无覆盖键时沿用启动 Provider 实例；非法值（API_KIND 非三类/MAX_CONTEXT 非正数）忽略回退
- 会话内存态删除某变量 = 恢复为全局的值
- 修改环境变量（任一来源）后，当前正在运行的任务不受影响，新任务使用新值（运行中的会话内存态开关除外——自动审批等实时判定类按次读取）
- 敏感变量（含密钥的 `*_KEY` / `*_TOKEN` 等）在服务端 API/UI 中脱敏显示，仅可覆盖不可回读明文；浏览器本地存储的值存于用户自己的浏览器，按明文编辑

### 会话临时文件查看与下载

会话 `tmp/` 中的文件（Agent 产出、脚本输出、截断内容引用等）支持在 UI 中查看与下载：

- **文件列表**：**经文件工作台浏览**（聊天页无独立会话文件面板）——工作台暴露 `sess:<sessionId>` 根指向该会话 `tmp/` 子树（WS `session.files.list` / REST `/api/v1/sessions/:id/files` 提供数据，SDK 有对应方法）
- **查看**：文本文件（文本/JSON/代码）内嵌预览——消息流文件卡文本上限 **40,000 字符**（超出截断提示），工作台文本读取上限为 `GEBAI_FS_MAX_READ`（默认 10MB）；二进制文件（图片等）可预览，其余提示下载
- **下载**：单文件下载、多选打包下载（zip）；通过 REST 下载端点返回原文件（`Content-Disposition` 指定文件名）
- **安全边界**：文件操作严格限定在会话 `tmp/` 内，路径解析复用路径沙箱（拒绝 `../`、绝对路径、符号链接），仅会话所有者可访问；**列表仅暴露 `tmp/` 子树**（`chat.json` 等会话数据文件不列出），REST/WS 文件接口的路径解析统一以 `tmp/` 为根并兼容 `tmp/` 前缀（旧附件/截断引用路径）
- **文件预览（`files/preview`，文件卡/文件链接取数入口）**：`read`/`write` 产物 file 块的路径为**服务端解析后的真实路径**（工具执行时按会话 `tmp/` 真实绝对路径（`sessionPath` 拼接——项目绑定工具的 workdir 是项目根不能作判定依据）归属判定：会话内 → `tmp/` 逻辑路径，code 项目文件（project 参数/预置项目解析后）→ 绝对路径），前端「文件展示方式=弹窗查看」时产物 file 块收敛为**文件链接 chip**（点击弹窗查看）、嵌入模式下文件内容卡取数同样可用（原始参数路径在项目工具下无法由 files 接口解析的 404 缺陷由此修复）；取数统一走 `GET /sessions/:id/files/preview?path=`：**相对路径以会话 `tmp/` 为根**（与 content 同规则），**绝对路径按用户隔离边界放行**——沙箱用户仅允许本用户数据目录（`users/{user}/`，与文件工具 project 参数经 `resolveInSandbox(root=users/{user})` 的可达范围一致，含符号链接逃逸检查），非沙箱（本地模式操作者本人，与文件工具能力对齐）放开；`?download=1` 以附件形式返回（文件卡/chip 的下载入口），前端不猜测路径解析
- **与截断内容联动**：上下文保护落盘的截断文件也可在 UI 中直接查看/下载
- **Office 阅读视图（`?render=office`）**：docx/xlsx/xlsm/pptx 的文件卡/弹窗内联渲染——前端按 office 类型在 preview URL 上追加 `render=office`，服务端经 wps 子Agent 的读取模型输出结构化 HTML（沙箱 iframe 承载，详见「`wps`（Office 文档处理）」章节「阅读视图预览」条目）；非 office 扩展名或损坏文件返回 422，前端回退二进制占位与下载引导
- **用途**：用户随时检视 Agent 工作产物（生成的报告、脚本、数据文件），无需进入文件系统

#### 接口

- WS：`session.files.list` / `session.files.get`
- REST：`GET /api/v1/sessions/:id/files`、`/files/content`（?path=）、`/files/download`（?path= 单文件 / POST body {paths} 多选 zip 打包）、`/files/preview`（?path= &download= &render=office 文件预览：会话相对/项目绝对路径统一入口，点击弹窗查看用；render=office 返回 docx/xlsx/xlsm/pptx 阅读视图 HTML）
- SDK：`listSessionFiles(sessionId)` / `readSessionFile(sessionId, path)` / `downloadSessionFile(sessionId, path)` / `downloadFilesZip(sessionId, paths)`

### 文件工作台（`/files`）

面向「彻底摆脱 VSCode」的一套自足文件工作台：**独立页面 + 独立路径**（`/files`，Vite 多入口 `files.html`），入口在聊天页标题栏**会话列表按钮右边**（主按钮＝并列，悬浮翻出的副按钮＝**全屏**（右箭头）；文件工作台那一侧活动栏最下方一对：**关闭文件工作台**（左箭头）/ **进入分屏**——文件工作台与会话工作台是同一个窗口里的两个工作台）。面向用户本人直操（不走工具审批，但写操作落审计）；让 Agent 去改仍走工具审批链路。设计稿与实现说明：`docs/file-workbench-design.md`、`docs/file-workbench-implementation.md`。

- **键盘快捷键**：与主界面共用同一套机制（`../keymap.ts` 分发器 + `files/keymap-wb.ts` 建表）——`Ctrl+S`（保存）、`Ctrl+P`（快速打开，VSCode 式模糊搜文件名 + 最近打开）、`Ctrl+Shift+O`（转到符号）、`Alt+W`（关标签）、`Ctrl+E`（查看↔编辑）、`Ctrl+F`（目录过滤）、`Ctrl+B`（左栏）、`Ctrl+Shift+E/F/G`（资源管理器/搜索/变更）、`Ctrl+K`（更多菜单）、`Ctrl+Shift+D`（比较）、``Ctrl+` ``（终端）、`F5`（刷新）、`Alt+G`（Git 工具窗）、`F7`/`Shift+F7`（差异块）、`F8`/`Shift+F8`（变更文件 / 合并冲突）、`Alt+M`（标记为解决）、`Alt+←`/`Alt+→`（编辑位置后退/前进，macOS 另接 `Ctrl+-`/`Ctrl+Shift+-`，见「编辑位置历史」）、`F2`（重命名）、`Ctrl+C`/`Ctrl+V`（资源管理器：复制选中项 / 粘贴到选中目录）、`Alt+Z`、`Esc`；终端面板内 `Ctrl+Shift+C/V`（复制/粘贴）、`Ctrl+F`（搜索）、`Ctrl+=/-/0`（字号）、`Ctrl+C`（中断）、`Ctrl+L`（清屏，降级终端）。接管浏览器默认的键位一律走**捕获阶段**（抢在 Monaco / xterm 之前，也避开 Monaco 把 `Ctrl+K` 当多键前缀、`F7` 当 diffReview）；焦点在终端面板内时全局键一律让位，`Ctrl+W/P/E/K/B` 交还 shell 的 readline。`Ctrl+W` 是 Chromium 保留命令（页面收不到），只在桌面/app 形态生效，浏览器窗口里的误关由工作台 `beforeunload`（有未保存改动时确认）兜底。
- **编辑位置历史（后退/前进，VSCode `navigateBack/Forward` 同款）**：`files/nav-history.ts`（纯函数栈，有单测）——**一条线性栈 + 指针**（不是每文件一条：用户心智里「后退」是沿时间回溯「我到过的地方」；A→B 转定义后后退要回 A 的那一行）。**记录点 = 打开/切换文件与同文件显式跳行**（树单击、快速打开、LSP 转定义、深链接、符号面板——全在 openFile 的出口处）；普通光标移动不入栈，但经 `updateTop` 修正栈顶（「后退回到离开时最后看的位置」）。去重三条：同位置重复 jump 不入栈（激活已有标签不产生假历史）、同文件近距离（<5 行）合并进栈顶、后退后再 jump 截断前进段（与浏览器地址栏同构）；上限 50 条。执行（`main.ts` 的 `navGo`）：命中已有标签则激活并跳行，标签已关则重新打开；回放与启动恢复（restoreSession/restoreFromUrl）由 `navGuard` 静默——恢复是「回到原处」不是导航。标签栏动作区最左一组「← →」按钮（差异视图的导航按钮不在标签栏——它们随差异视图头部走，见「变更文件遍历」条；无历史时整组隐藏，到头禁用）；键位 VSCode 同款（`Alt+←/→` + macOS `Ctrl+-`/`Ctrl+Shift+-`；macOS 的 Option 死键问题由 `keymap.ts` 的 `"_"→"-"` 别名解决，与既有 `"+"→"="` 同构）。**终端焦点内让位**：`Ctrl+-` 在终端里是缩小字号（term-keys 后注册、按声明序命中会被抢走）。不跨刷新持久化（栈是本次浏览的回溯线；刷新回到原处由状态记忆承担，职责不重叠）。
- **编辑器字符宽度测量（字体就绪 / 像素比变化 / 自检校准）**：Monaco 只在 `editor.create` 时量一次字符宽度并长期缓存，光标位置、选区矩形与鼠标点击落点都按「列号 × 该宽度」算。编辑器字体是 `@font-face` + `font-display: swap` 的懒加载 woff2，测量若落在字体落地之前，量到的是**回退字体的前进宽度**（Windows 回退的 Consolas 0.55em 对 JetBrains Mono 0.6em，几十列就偏出一个字符），字体换上前后误差随列号累积且**不自愈**；分数像素比下逐字形取整是另一条同表现路径。`files/editor-metrics.ts` 三件事收口：① 建编辑器前 `ensureEditorFont()`——`fonts.check` 命中即零等待，未就绪则显式 `fonts.load` 并最多等 800ms，超时照常建编辑器（不阻塞打开文件）；② 度量同步 `createMetricsSync()` 在 `document.fonts` 的 `loadingdone`、`matchMedia('(resolution: Xdppx)')` 变化（窗口跨缩放显示器/改系统缩放）与首次布局后复检：先 `remeasureFonts()` 清陈旧缓存，像素比变化另 `layout()`；③ 复检口径是**缓存宽度 vs 实绘前进宽度**（量编辑器自己渲染的行，取不到用同款字体的探针 span）——重测后仍不一致才关掉等宽快速路径（`disableMonospaceOptimizations`，该路径正是「列号 × 缓存宽度」假定的来源）并留一条控制台诊断。编辑器与差异视图共用同一套（字体/字号常量同源），句柄销毁时同步器一并 `dispose()`。
- **快速打开（Ctrl+P，VSCode Quick Open 同款）**：`files/quick-open.ts`（面板与交互）+ `files/quick-open-core.ts`（模糊匹配与排序，纯函数、有单测）+ `files/recents.ts`（最近打开，localStorage 按根隔离）。索引一次取回（`GET /api/v1/fs/files` → `rg --files`，原生尊重 .gitignore；不可用回退内置遍历），之后逐键筛选全在前端；按根缓存 60s，收到 fs 变更事件即失效（新建/删除的文件立刻可搜到）。匹配强调三点：子序列（`smain` 命中 `src/main.ts`）、**文件名命中优先于目录名**、连续/词首/驼峰边界加分而跨段空隙扣分；同分按路径长度再按字典序，**顺序稳定**（键盘操作不能每次换位置）。空查询显示最近打开；`Enter` 开预览标签、`Ctrl+Enter` 固定常驻（与 VSCode 一致）；命中字符高亮（目录段与文件名共用一套下标）。

- **文件内符号跳转**：`F12` / `Ctrl+Click` 跳到光标处标识符**在当前文件内**的定义，`Ctrl+Shift+O` 列出文件符号。符号来源四级仲裁（**有语言服务器时用它的 `textDocument/documentSymbol`**——语义级且与跳转同源；其余按语言分工）：**TypeScript / JavaScript / JSON / CSS 系 / HTML 由 Monaco 内置语言服务**（本地 worker，非 LSP）提供——工作台的 model 就在它的脚本清单里（`getScriptFileNames` / `getNavigationTree` / `getDefinitionAtPosition` 实测正常），这类语言走编辑器自带能力；**有 tree-sitter 语法文件的 15 种语言（python/go/rust/c/cpp/java/kotlin/scala/swift/dart/ruby/php/lua/shell/elixir）走真语法树**；**其余 27 种（csharp/objc/groovy/perl/r/powershell/bat/vb/pascal/haskell/fsharp/clojure/erlang/sol/wgsl/asm/sql/yaml/ini/markdown/latex/graphql/protobuf/hcl/dockerfile/makefile/cmake）词法规则兜底**（也是语法树不可用时的回退）。Ctrl+Shift+O 在两种焦点环境下都可用：编辑器内直接走 Monaco 的大纲动作（`editor.action.quickOutline`），编辑器之外先判定语言归属——内置服务负责的语言把焦点交给编辑器再触发同一个动作，其余语言弹出工作台自己的符号面板（复用「快速打开」的 `.fw-qo-*` 外观与 ↑↓/Enter 语义，状态栏标出结果来自「语言服务器」「语法树」还是「词法规则」）。自研部分三件：`files/symbols-core.ts`（**词法级、纯函数、有单测**——逐行规则 + 作用域栈，注释/字符串/正则字面量先掩码成**等长空格**再匹配，行号列号因此与原文严格对齐）、`files/symbols-ts-rules.ts` + `files/symbols-ts.ts`（**tree-sitter 路径**——声明式节点映射表 + 懒加载运行时，输出同一种 `Sym` 结构）、`files/symbols-extract.ts`（调度：语法树优先、失败回退词法）、`files/symbols.ts`（Monaco 的 `DocumentSymbolProvider` / `DefinitionProvider` 桥，按 **model 版本号**缓存提取结果、FIFO 只留最近 8 个 model；provider 可异步）、`files/symbol-panel.ts`（工作台面板，先开先填）。词法分层三种口径（`LangSpec.scope`）：`brace`（花括号语言按行首 `{}` 深度）、`indent`（Python / YAML / Ruby 按缩进）、`level`（Markdown 标题 / INI 节 / LaTeX section 自带层级）；符号种类再做一次修正（类内函数 → `method`、与容器同名或 `__init__`/`__construct`/`initialize` → `constructor`）。两条路径的语言 id 均取自 `@gebai/sdk` 的 `file-language.ts`（**路径→语言的唯一真相**：服务端 `core/fs/mime.ts` 与前端差异/比较/合并视图都转发到它——早先前端另有一份手写表，导致 `x.mts`/`Cargo.toml`/`Dockerfile` 这类路径在编辑器里有语言、在差异视图里却是 `plaintext`）。并集（升上本机有语言服务器的语言，**再剔除 Monaco 内置语言服务覆盖的语言**）当文档符号 provider 的选择器；**定义跳转** provider 仍用「剔除了有 LSP 的语言」的集合（避免同一个跳转两份候选）。**有意不做的**：类型推断与重载解析（同名多定义按作用域就近 + 出现顺序给候选）、同一行的多个定义只取先命中的、跨文件跳转。
  - **资产与体积**：tree-sitter 运行时（`tree-sitter.js` 147KB + 核心 wasm 201KB）由 `build-vendor.ts` 拷入 `public/vendor/tree-sitter/`（与 monaco/xterm/d2 同一惯例：稳定文件名、静态伺服、离线可用）；**语言语法 wasm 不另存一份**——15 种语言原始体积约 25MB，放进 web 产物会被内嵌进二进制，改为新增静态路径 `/vendor/tree-sitter/lang/<grammar>.wasm` 从服务端**已内嵌的**语法集（分析器那份）按 `Accept-Encoding` 回源（gzip 后单语言 11KB～415KB），并 `Cache-Control: max-age` 允许浏览器长期缓存。语言表 `TREE_SITTER_GRAMMAR` 放在 `@gebai/sdk`（前后端共用一份真相，服务端据此做白名单、web 据以加载）。语言列表、映射细节与取舍见 `files/symbols-ts-rules.ts` 与 `docs/file-workbench-implementation.md` 的 5.34。
- **语言服务器（LSP）：有则用、没有不影响**：编辑器按文件语言在**本机 PATH** 上探测常驻语言服务器（`core/lsp/registry.ts` 内置表：`gopls`→go、`rust-analyzer`→rust、`clangd`→c/cpp/objective-c、`pyright-langserver`/`pylsp`→python，另有 lua / yaml / shell / kotlin / ruby / php / csharp / dart），探到即提供**补全 / 悬停 / 定义跳转 / 引用 / 重命名 / 格式化 / 签名帮助 / 诊断**；**探不到（未安装、`GEBAI_LSP=false`、沙箱非豁免用户）时清单为空，前端不注册任何 provider、不建连接**——Monaco 内置语言服务（TS/JS/JSON/CSS/HTML）与符号提取链路照旧。服务端 `core/lsp/`：`protocol.ts`（`Content-Length` 帧编解码，长度按**字节**计）、`session.ts`（单「根 × 服务器」进程：initialize 握手、文档同步、请求关联、**服务器反向请求兜底应答**——`workspace/configuration` 等不应答会卡住其初始化）、`service.ts`（会话池 + 文档归属 + 事件分发 + 空闲回收）、`ws-handlers/lsp.ts`（`lsp.open/change/save/request/close` + 推送 `lsp.notify/exit/log`）、`routes/lsp.ts`（清单端点）；前端 `files/lsp.ts` + `files/lsp-convert.ts` + `files/lsp-capabilities.ts`（能力协商判定，纯函数 + 单测）+ 通用 WS 请求客户端 `files/ws-client.ts`（终端与 LSP 共用）。三个设计取舍：① 前端**只认 `docId`**（不认识绝对路径）——`didOpen` 的 file uri 由服务端用 `rootAbs + 相对路径` 生成、请求参数里的 `uri === docId` 转发前替换、诊断回来反向换回 docId，路径边界仍由 Root 抽象单点把守；② 变更**一律发全文**（对 LSP 的 Full/Incremental 两种同步模式都合法，省掉前端增量 diff），键入 220ms 节流；③ 会话按 **(用户 × 工程根 × 服务器)** 复用（工程根由 `core/lsp/project-root.ts` 向上探测，见下文；同一工程的多个文件——哪怕来自不同工作台根——共用一个 gopls/rust-analyzer），并发上限 4、无文档会话空闲 10 分钟回收（有打开文档时按 6 倍阈值，避免「文件开着不动就丢能力」）。跨文件跳转由 `registerEditorOpener` 把 `file://` uri 折算回工作台的「根 + 相对路径」再开标签——否则 Monaco 在未打开的 model 上会静默失败。**跳转目标分两态**（`LspJumpTarget`）：命中已挂载根 → 根内相对路径（行为如上）；**未命中任何根**（库文件：`/usr/include/c++/13/string`、GOROOT 标准库、rust-src、site-packages/typeshed）→ 绝对路径交给工作台（`files/repo-paths.ts:resolveAbsPath` 纯函数：已有根优先 → **库锚点**建 `abs:` 临时根 → 父目录兜底；锚点分强/弱两级），并**复用跳转来源那份文档的语言服务器会话**（`lsp.open` 的 `attachTo`，同用户 + 同一服务器二进制才复用）——库文件由原来那个服务器回答（它已有索引与编译参数），也不新建进程、不吃并发槽位；临时库根**不抢左栏**（跳库文件是“看一眼定义”，不把资源管理器搬到 `/usr/include`）；非 `file:` 协议（如 Java `jdt://`）给明确提示而不是静默。与符号提取的分工：某语言有可用服务器时 `files/symbols.ts` 的**定义跳转**选择器剔除它（语义比词法/语法树准，也避免同一跳转出两份候选），而**文档符号/大纲**选择器把它并回来（符号来源在 `symbolsOf` 里按 LSP 优先仲裁，同样不会出两份）。诊断以 `setModelMarkers` 整体替换（owner 按服务器分），进程退出时清诊断并按（每个服务器）30s 限频**重挂全部受影响的文档**；拉起服务器记 `lsp.start` 审计（detail 带工程根与命中的标记）。三条实测约束备查：**客户端能力声明不声明 `workspace.workspaceFolders`**（pyright 声明后会挂起分析准备，补全/悬停/定义均不应答；initialize 参数仍带 workspaceFolders）；**诊断的 uri 按归一化键匹配**（服务器会改写盘符大小写与百分号编码，精确字符串匹配会丢诊断）；**清单拉取失败不固化**（网络波动后下次打开文件重试，deep link 打开的文件会等清单就绪）；另：只读态下 Monaco 不弹补全候选（编辑器行为），切到编辑态即可用，悬停与诊断在只读态照常。另（本轮打磨）：**工程根探测**（`core/lsp/project-root.ts`）——会话的 cwd/`rootUri` 与**复用键**都取「从文件目录向上找到的语言标记所在目录」（`go.mod`/`Cargo.toml`/`compile_commands.json`/`pyproject.toml`… 24 层上限、语言标记优先、`.git` 兜底、找不到回退工作台根；命中永久缓存、未命中 10s TTL），再做一步**工作区根细化**（Cargo workspace 成员 crate 各自有 `Cargo.toml`，取最近的那个会为每个成员各起一份 rust-analyzer；因此再向上找**声明了工作区的**标记——`Cargo.toml` 含 `[workspace]`/`go.work`/`package.json` 含 `workspaces`——命中就用它；这一步要读小的标记文件，仅此三类且命中后永久缓存），因此工作台根落在模块子目录时 gopls/clangd/rust-analyzer/pyright 仍能正确解析（`projectRoot`/`projectMarker` 随 `lsp.open` 回给前端，状态栏 title 可见），同一工程跨工作台根也只起一个进程；**能力协商**——服务器能力表未声明的请求不发、回过 `-32601`（method not found）的方法在本页记住不再问（实测 gopls 不实现 `rangeFormatting` 与 `completionItem/resolve`）；**零宽区间分两处处置**——诊断/悬停撑开一格（否则波浪线不可见），补全/格式化/重命名的零宽编辑**不**扩张（否则接受补全会吃掉光标后的字符；gopls 的自动导入正是两条零宽 `additionalTextEdits`）；**跨文件结果不按本文夹取**、`file://` uri 折算**跨平台正确**（POSIX 保留根斜杠、Windows 去盘符前斜杠）、跳转**带列号**（落在标识符上而非行首）；**进程退出按「每服务器一次」限频并重挂该服务器的全部文档**（此前限频与挂载写在同一循环里，只有第一个文档被重挂）；诊断额外映射 `tags`（未使用/已弃用）、`relatedInformation`（在此处声明）与 `codeDescription`（诊断码文档链接）；服务端转发前兜底补 `textDocument.uri`（缺了 gopls 报 `no package metadata for file `）；补全接 `completionItem/resolve` 与 `additionalTextEdits`，符号侧新增 `textDocument/documentSymbol` 与 `textDocument/rangeFormatting` 两个 provider。配置见下表 `GEBAI_LSP*`。

- **路径 → 语言（单份真相）**：`@gebai/sdk` 的 `file-language.ts` 是 `(路径) → Monaco 语言 id` 的唯一实现（`languageOfPath` / `extOfPath` / `baseNameOfPath`），服务端 `core/fs/mime.ts:languageForPath` 与前端 `files/main.ts:languageOf`、`files/ui.ts:extOf` 全部转发到它。判定顺序是**先扩展名、再特殊名**：`makefile-helper.ts` 这种「特殊名前缀 + 真扩展名」必须以扩展名为准；只有扩展名认不出语言时才看特殊名与变体前缀（`Dockerfile.dev` / `.env.example` / `Makefile.am`）。路径形态的容错也在这一层：正反斜杠、UNC、尾部空格/点（Windows 复制路径常见脏名）都不影响扩展名解析。相对路径与绝对路径一律可用（只取 basename）。测试见 `packages/sdk/src/file-language.test.ts`（51 例，含仓库真实路径抽查）。跨文件跳转的**生效语言**另有一个纯函数 `effectiveLanguageOf(detected, hint)`：路径判不出语言（`plaintext`）时用**跳转来源文档**的语言（无扩展名的库文件：`/usr/include/c++/13/string`），**C 家族歧义**（`.h` 从 C++ 跳过来）也按来源语言（libstdc++ 的 `bits/basic_string.h` 是 C++）；其余一律以路径为准（跳转可能跨语言，无条件覆盖会把目标文件的高亮改错）。
- **形态与入口**：`packages/web/files.html` + `packages/web/src/files/{main,explorer,editor,viewers,git,compare,ui,api}.ts`；入口按钮 `packages/web/src/files-entry.ts` 注入标题栏，URL 透传 `session`（`sess:` 根指向本会话工作区）/`root`/`project`/`path`；**主题不进 URL**——工作台以 `localStorage` 与主界面共享同一份用户级偏好（跨标签页 `storage` 同步 + 嵌入态 `postMessage`，`initTheme({ urlPrefs: false })` 忽略 URL 上的主题参数，见「主题」章节）。为什么新标签而非同页路由：Monaco + Git 面板资源重，「一边让 Agent 改、一边自己核差异」是常态，独立页同时带来故障隔离。
- **页面内 URL 一律相对写法**（`files/url-base.ts`）：页内一切请求与资源路径——REST `/api/v1/*`、vendor 静态资源（monaco / xterm / tree-sitter）、下载与 office/raw iframe、图表引擎——都写成相对当前文档的形式（`./api/v1/…`、`./vendor/…`），由浏览器按页面位置解析：挂到根路径、`/gebai/` 或任意层数前缀下都不改代码，代理**剥离前缀**转发即可，页面侧不做前缀推导。两种形态分工：`wbUrl` 产出相对路径字符串（fetch / DOM 属性 / `window.open`），`wbAbsUrl` 产出按文档基准解析后的绝对 URL（动态 `import()`、Monaco 的 `paths.vs` 与 worker URL、wasm `locateFile`——相对说明符在动态导入里相对**模块文件**而非页面，这些消费方还要在内部二次拼接）。**WebSocket 是例外**：`new WebSocket()` 不接受相对 URL，仍由 SDK 的 `appWsUrl` 按页面基准解析（`files/ws-client.ts`，终端与 LSP 共用同一条 `/ws`）。页面被宿主放在非 `<基准>/files` 位置时（相对解析会落到别处）回落 SDK 的基准前缀写法，与前端其它推导同口径。
- **根抽象（权限边界）**：所有 fs/git 接口只接受 `(root, 相对路径)`——`sess:<id>`（会话 `tmp/`）/`proj:<name>`（预置项目：`{AGENT}_PROJECTS` 注册表 + 内置「歌白」）/`user:`（当前用户目录）/`abs:<path>`（绝对路径，**服务模式沙箱下拒绝**）；根清单 `rootCatalog()` 去重供前端根选择器；`GEBAI_FS_ROOTS` 白名单根（JSON 数组）供服务模式授予指定目录。路径防护三层：词法（拒 `..`/绝对路径）→ realpath（拒软链逃逸）→ 前缀（兄弟目录边界）。
- **两套路径坐标（仓库相对 ↔ 根相对）**：**Git 侧**（status/diff/log/paths、提交里的文件清单）说**仓库相对**路径；**文件侧**（fs 端点、编辑器标签、资源管理器）说**根相对**路径。两者在「根 == 仓库根」时重合，**子目录根**（会话工作区常落在项目仓库的子目录里）时才分岔；而「整仓库」范围下还会出现**根之外**的改动——那条路径在根内根本无法表达。换算集中在一处（`files/repo-paths.ts`，纯函数 + 单测）：
  - **根前缀**由 `repoPrefixOfAbs(根绝对路径, 仓库根绝对路径)` 给出（`""` = 根就是仓库根，`null` = 不在此仓库——两者语义不同，不可混用）；仓库根取 `/git/status` 的 `repoRoot`（**子目录根往往不在根清单里，只有它带得出仓库根**），清单的 `repoRoot` 兜底。根绝对路径优先查清单，其次从 `abs:` 根 id 解析（本页可能被以清单之外的 `abs:` 根打开）。
  - **打开仓库内任意文件**（`openRepoFile` / 冲突合并的落点 / 提交清单里的「打开文件」）走 `resolveRepoPath`：① 在当前根内 → 用当前根；② 否则用清单里**覆盖它**的根（最长前缀优先，同长按项目类 > 会话 > 用户 > 任意目录）；③ 都没有 → 以**仓库根**建一个 `abs:` 临时根并登记进清单。切换根只影响标签自己（打开文件不动左栏），而「在资源管理器中定位」会**换根并在树里选中**——菜单项说的就是「带我去看它」。
  - **Git 路径过滤/文件历史统一用仓库坐标**（服务端跑 git 时 cwd 就是仓库根，paths 因此天然是仓库相对）：资源管理器右键给的是根相对路径，交给日志栏前必须补前缀（`toRepoRel`），否则子目录根下会**静默过滤成空**。
- **根清单探测（`GET /api/v1/roots` 的成本控制）**：根清单要给每个根标出「是不是 Git 仓库 / 当前分支」，早期实现**逐根串行** `repoRoot()` + 全量 `status()`——本地实测 25 个根（其中 20 个会话工作区都落在同一个仓库内）首次请求 **~5.0s**：同一仓库被反复探测，而每个 git 进程在 Windows 上光启动就要 ~65ms。现为四层省成本：① **浅路径优先 + 父仓库短路**（先探浅目录，摸清仓库根后落在其中的其他根直接复用，**逐级查 `.git` 防嵌套仓库被误短路**）；② **按仓库根去重**（同仓库只探一次）；③ **限并发 ≤8**（避免一次 spawn 几十个 git 进程）；④ **轻量 `probeRepo()` 替代 `status()`**（`rev-parse --show-toplevel --git-dir` + 读 `.git/HEAD` 判定分支——**不能用 `--abbrev-ref HEAD`**：未出生分支仓库上它报 `ambiguous argument 'HEAD'` 并非 0 退出，只看退出码会把空仓库误判成非仓库，而不看退出码又无法与 detached（同样输出字面量 `HEAD`）区分），结果缓存 TTL 60s（该缓存只喂左栏「仓库/分支」标签，实时状态另有 `/git/status` 等端点）。实测：git 进程数 71 → 8、冷启动 **~5.0s → 211ms**（真实 HTTP，另起实例）、热请求 1–2ms，25 个根的结果逐根一致（含嵌套仓库边界 6/6）。
- **启动时序（首屏与数据解耦）**：`boot()` 分两阶段——阶段一**同步**搭外壳（不依赖任何网络往返），**双 rAF 首帧后立即抹掉启动遮罩**；阶段二才装配数据（根清单 / URL 恢复 / Git 状态 / 各面板渲染）。为何如此：外壳渲染与 `roots` 之类的往返毫无依赖，早期把遮罩挂到「roots + 目录树 + Git 状态全部返回」之后，等于把服务端的往返整段暴露成白屏等待（实测遮罩 6.2s vs 首帧 0.05s）。空窗期由中央占位（「正在准备工作区…」）与 Git 工具窗的「正在加载…」承担；`nextFrame()` 带 200ms 兜底（后台标签页 rAF 会被暂停）。启动期对同一根重复的 `git/status` 请求做去重（进行中复用 + 500ms 窗口，2 次 → 1 次），Git 面板延到根与状态就绪才建（空根会被判成「不是仓库」并摆出「初始化仓库」按钮）。**Monaco 空闲预热**：**数据装配（根清单/Git 状态）完成之后**才起 `requestIdleCallback`（超时 2.5s 兜底）预取编辑器内核——预热不阻塞首屏，但排在首屏数据请求前面会与之抢带宽；后台标签页（Ctrl+点击打开）先不预热，切到前台再起。首次打开文件不再空等 ~0.5s——`loadMonaco` 的失败/超时**必须复位单例**（早期把 `resolve(null)` 的 promise 永久缓存，一次失败就让本页永远降级为轻量编辑器；并发调用仍共享同一进行中 promise）。启动遮罩退场（色球收拢 + 淡出）与外壳入场均走纯 `opacity/transform`（不引发布局抖动），并尊重 `prefers-reduced-motion`。
- **目录树**：懒加载单层列举（上层 5000 条 + truncated 标记）、目录优先自然序（`file2` 在 `file10` 前）、四种排序、隐藏文件默认列出（可在「更多」菜单里切换）、Git 状态色装饰、新建/改名/复制/移动/删除（物理删除，不可恢复；二次确认把关）/上传（拖拽）/下载。**复制走剪贴板**：右键「复制」（或 `Ctrl+C`）记下条目，到目标目录右键「粘贴」（或 `Ctrl+V`）落盘（目录行与空白处都有「粘贴」）；粘贴**从不覆盖**——同名时落成「xxx - 副本」「xxx - 副本 (2)」，因为粘贴是最高频的写操作而覆盖不可逆（「复制到…」一次性走完同一套落点规则），命名与落点判定在 `files/clipboard-core.ts`（纯函数 + 单测）。三条守卫：目录不能粘进自己的子树（否则递归复制）、跨根不粘贴（`/fs/copy` 的源与目标在同一根内解析，条目在另一根时菜单项直接禁用）、只读模式不粘贴。
- **目录树自动刷新（预取 + 事件驱动的增量同步）**：① **展开即预取下一层**——展开一个目录时顺手列举它的直接子目录（`MAX_PREFETCH = 24`、并发 ≤4、后台标签页不预取、换根即丢弃结果），用户接着展开子目录时命中缓存、不再等一次往返（实测展开 35ms 出内容，未经预取时要一次 HTTP 往返）；根目录总是展开的，它的下一层在 `requestIdleCallback` 里预取（不与首屏请求抢带宽）。**只预取一层**：更深层在用户展开时自然成为新的「下一层」。② **变更事件驱动的增量同步**（`explorer.syncDirs`，由「变更监听」章节的长轮询唤醒）——把变化路径换算成「父目录 + 自己」（`dirsToRefresh`），**只**重新列举已缓存且处于展开链上的目录，与缓存逐项比对（名字/类型），确实变了才换缓存：根列表变了走整树重建，其余目录只换**那一块子树**（删旧行 → 按新缓存重画）。内容修改（保存文件）不会重建任何行——行上显示的是名字，改动本身由 Git 装饰与变更面板表达。「未知变化」（路径太多或监听被关）时退化为重列根 + 展开目录（夹到 24 个，见兜底轮询）。
- **目录树渲染增量化**：① **装饰查表**——把「有变更的路径」一次做成 `Map`（精确命中）、「有变更的祖先目录」做成 `Set`（子项变更），行内只做 O(1) 查（早期每行两轮线性扫 `status.changes`，2000 行× 500 变更 = 每次刷新百万级比较，而且**渲染路径上就会调它**）；变更清单指纹未变则直接复用上一份表；行上记录已应用的装饰，未变的行完全不碰 DOM。② **展开/收起只动局部**——展开时把子树插到该目录行之后、收起时删掉它之后的子树行（扁平渲染下「深度大于本行」的连续行恰好就是它的子树），不再重建整树（大目录里展开一个子目录不再连已展开的全部行一起重建）。③ 整树重建走一次 `replaceChildren(片段)`；选中/活动态由 `refreshSelection()` 定点改「上一个/当前」两行（不再每次点击遍历全行）；`path → 行/条目` 索引表同时供装饰刷新与 reveal 定位复用。④ 名称过滤输入防抖 140ms。⑤ 行上的交互一律用 `on*` **属性赋值**（不是 `addEventListener`）——它们是元素上的普通属性、不产生监听器注册开销，因此**不做事件委托**（审计里「每行 7 个监听器」的结论只对 `h()` 的 `on*` 入参成立）。⑥ **鼠标语义按 IDE 口径分工：单击 = 预览、双击 = 固定**——单击文件行用**预览标签**打开（斜体标题、会被下一个预览就地替换，适合“顺着树往下扫文件”；预览的落地刻意延后 220ms，时序判定在 `files/preview-click.ts`），双击把它**钉成常驻**（目录行的双击仍是展开/折叠，见 `explorer.ts` 的 `onclick`/`ondblclick`；双击时待定的那一发预览被作废，只发一次常驻打开，并顺手收掉预览槽里那个无关的文件）。与标签栏双击（固定 ⇄ 取消预览）、快速打开 `Enter`/`Ctrl+Enter` 同一套状态，细节见「预览标签 ⇄ 常驻标签」条。
- **编辑路径开销**：① 脏标记不再逐击键 `getValue()` 全文比对（大文件 = 每键物化 MB 级字符串），改为击键时先乐观置脏 + 300ms 防抖复核（撤销回原样仍能变回干净）；② 状态栏整条重建前先比对状态快照（光标/标签/编码/EOL/Git/权限/内核等全部输入），未变直接返回；光标与拖选的刷新按帧合并；③ 选区字符数按**行长度累加**算出（同一范围复用结果），不再 `getValueInRange()` 物化选区文本——拖选/全选大文件时那是每次 mousemove 一次 MB 级分配。
- **查看矩阵**：文本/代码 → Monaco（VSCode 同款内核，vendor 静态伺服）；图片/视频/音频（HTTP Range 拖动进度）/PDF/Office（服务端转换阅读视图）/压缩包（零依赖中央目录解析 + 解压）/二进制（hex）/图表源文件。条目 `kind`/`language`/`editable` 由服务端裁决（前端据此选查看器与是否显示编辑按钮）。
- **编辑器配色跟随主题（`files/editor.ts` · `defineTheme`）**：用 `editor.defineTheme` 把当前界面的 CSS 令牌映射成 Monaco 主题（`base: vs/vs-dark` 按合成背景的明暗自适应，换肤时重新 define + setTheme，不硬编码配色）。三个必须守住的点：① **半透明令牌先合成**——主题令牌多为半透明（默认主题 acrylic 的 `--bg-elev` 是 `rgba(18,18,23,.82)`、`--bg-inset` 亮色下是 `rgba(0,0,0,.05)`），直接丢 alpha 只取 rgb 分量会得到与页面无关的假色：亮色主题下编辑器被判成暗色（`vs-dark`）并铺成黑底，再叠上亮色主题的深色前景 `--text` 就是「黑底黑字」（用户可见：切了亮色主题，文本编辑器没跟着换配色）。现按「页面底（body 计算背景）→ 编辑器底」逐层合成出不透明实色（复用 `css-color.ts` 的 `cssVarToHex`；其合成底同时认 `rgb()/rgba()` 与 `#rrggbb`，好让上一层合成结果能当下一层的底）。② **编辑器底取视图面板底色（`--bg-elev`）、不用内凹色**——编辑器坐在工作台视图里，与左栏/工具窗/标签栏/状态栏（均 `--bg-elev`）同层同亮度才是一整块；内凹色 `--bg-inset`（输入框/代码内嵌的语义）比面板暗 **4~19 级亮度**（亮色亚克力：面板 250 里嵌一块 236 的灰块；浪潪主题 31 vs 12），用户可见为「视图背景与编辑器背景亮度差得有点大」。浮层/建议框再用 `--bg-elev-2` 抬一档（缺失则同底、靠 border 分辨）。③ **等过渡停稳再取色**——换肤会带动 body 背景的 CSS 过渡（base.css 的全局过渡名单含 body），过渡途中取色会把中间值固化成编辑器底色（亮色切回后整块偏暗）且此后无事件重定义、不会自愈；现用 `getAnimations()` 等 body 的 `background-color` 过渡结束再定义（第一帧只作让位——过渡要等样式变更后的下一帧才启动，头两帧读到的还是旧值；无过渡时等一帧即定义；`getAnimations` 不可用或超 1s 时兜底定义）。
- **默认只读**：编辑器状态机 `查看态 readOnly ⇄ 编辑态`（工具栏「编辑」/Ctrl+E 解锁，脏标记 ●，Ctrl+S 保存）；默认只读既是习惯也是安全默认（与 Agent 共用文件，误触不得改文件）。
- **自动换行（`Alt+Z` / 动作轮盘）**：换行是**用户级偏好**（`gebai.ui.wordWrap`，默认关闭——代码的横向滚动是默认阅读方式），主编辑器、差异视图（两侧一起）、合并/暂存窗格与降级编辑器（textarea 走 CSS 类）都由 `files/editor.ts` 的**模块级开关 + 活动实例注册**统一施加：切换点有轮盘项与 `Alt+Z` 两处，而实例可能同时有十几个（每个标签一个，合并视图一次三个），把「记住开关」与「应用到全部」收在一处才保证一致（实例 dispose 自行注销）。`Alt+Z` 与 `F7`/`Shift+F7` 同一处置：**捕获阶段**拦下——Monaco 自己绑了这个键（`editor.action.toggleWordWrap`），但它只改实例选项、不动偏好（刷新即回退）也不更新按钮态，而且编辑器获焦时事件到不了冒泡阶段；**表单输入框里不拦**（Mac 上 Option+Z 是输入 Ω 的手势，在提交信息框/搜索框里打字时被快捷键抢走才是真的碍事），但 Monaco 与降级编辑器内部的隐藏输入区也是 textarea，靠「是否在编辑器容器内」区分。
- **小地图（动作轮盘）**：与自动换行是**同一套机制的镜像**（`files/minimap.ts` 偏好 + `editor.ts` 模块级开关 + 活动实例注册），**默认开启**——它是长文件的结构概览（改动落在哪一片一眼看得出），关掉是少数派诉求，所以存储口径也反过来：**关闭才写键**（`gebai.ui.minimap = "0"`），键不存在即开启（默认态不留残留，与 `wrap.ts` 的开启才写 `"1"` 同一取向）。两处与自动换行刻意不同：① **只作用于代码编辑器**——差异视图恒无小地图（那个视图的“地图”职责由右侧概览尺承担，两侧再各挂一张缩略图只是噪声）；② **不覆盖显式传参**——合并 / 暂存窗格自己传了 `minimap: false`（窄栏里的只读对照，真放不下），那几处不听用户偏好（`opts.minimap ?? minimapOn`）。另外**大文件降级优先于偏好**：超过 `LARGE_FILE_CHARS`（1.5M 字符）的编辑器即使开关为开也不给小地图、开了也 `setMinimap` 不会把它顶回来（`enabled: on && !large`）——那是性能取舍，实测 3.1MB 文件下小地图宽度恒为 0。
- **编辑器右键：复制路径（含行号）+ 发送会话 + 宿主自定义组（Git 历史）**（`files/editor.ts` 把内置两项与 `EditorMenuHooks.groups` 给的组一起注册进 Monaco 的 `EditorContext` 菜单；拼字符串的规则全在 `files/editor-ref.ts`，纯函数 + 20 例单测）：
  - **引用格式**（与 `grep` 输出的 `文件:行号` 同口径）：单行 `/abs/path/a.ts:12`、选中块 `/abs/path/a.ts:12-20`。两条容易错、又都不报错的规则：① **拖到下一行行首不算选中那一行**（Monaco 的 selection 语义，拖选到行首时 `endLineNumber` 是下一行）——`normalizeLineRange` 归一，写出去才不会多一行；② 单行不写成 `12-12`（行号后缀分两档）。**绝对路径取时现算**（`absOfRepo(rootAbsOf(tab.root), tab.path)`）：根清单 / 临时 `abs:` 根都可能后到，写死一个字符串会在那些情况下静默写成错的路径。
  - **位置与回收**：分组名 `gebai` 让它落在「剪切 / 复制 / 粘贴」之后、「命令面板 / 转到定义」之前（Monaco 按组名字典序排，`navigation` 被硬编码在最前）。**`addAction` 的句柄必须随编辑器 dispose 手回收**：它往**全局**菜单注册表加一条（带 `editorId` 前提，只在本编辑器弹），而编辑器自己的 `dispose()` 只清内部 action 表、不动那份注册——本页每个文件的查看/编辑态切换与「重新加载」都会重建编辑器，不回收就是“重建几次、右键菜单里多几组重项”（实测：不改之前每次重建都多一条）。降级编辑器（Monaco vendor 缺失）没有 Monaco 菜单、原生右键又被全站屏蔽，两项在那里是**自绘**的（挂容器而非 `pre`/`textarea`：两态互切会换可见元素）。
  - **剪掉自带条目（Peek / Copy / Command Palette）**：这三条对工作台没用——`Peek` 与工作台的跳转方式重叠且更绕，`Copy`（只复制选区）与自绘的「复制路径 / 发送会话」并排时容易点错，`Command Palette` 与工作台自己的 `Ctrl+P` / `Ctrl+K` 两套入口重复。Monaco **没有删菜单项的公开 API**（`MenuRegistry` 不导出，`editor.addAction` 只能加不能删，注册表内部也只增不减），菜单又渲染在编辑器自己的 **shadow root**（`.shadow-root-host`）里——页面 CSS 够不到 shadow 边界，所以只能在 DOM 层处理：`files/context-menu.ts` 盯着那个 shadow root（**host 是懒创建的**，首次弹浮层才出现 → 先盯编辑器 DOM，host 出现再挂）。**「转到定义 / 转到引用 / 转到符号」保留**——LSP 跳转是刚需，也正是这个菜单存在的理由。
    - **只能隐藏（`display: none`），不能 `remove()`**：Monaco 的 `ActionBar` 把焦点记成**子元素序号**，再用这个序号去索引它自己的视图表（`setFocusedItem` 遍历 `actionsList.children` 求 `focusedItem`，`updateFocus` 取 `viewItems[focusedItem]`）。删掉 DOM 元素而内部表不动，两边序号就错开：**悬浮会高亮到另一个条目**（实测：悬浮「侧边溯源」高亮「复制路径」；悬浮被删项原本占的序号则毫无高亮），键盘 `focusNext` 也停在没有高亮的空槽上。隐藏时元素仍在 `children` 里，序号不变，悬浮自然对得上。
    - **键盘导航由本模块接管**：既然序号要对齐，Monaco 的方向键就会走过那些被隐藏的项（`focusOnlyEnabledItems` 只跳过禁用项与分隔线，改不了我们隐藏的真实动作项）。所以接管 `ArrowUp/Down`、`Home`、`End`，只在**可见项**之间移动（劫持后往目标派发 `mouseover`，复用 Monaco 自己的聚焦路径，高亮与滚动照旧）；回车/Esc/快捷键一律留给 Monaco。上下文菜单没有助记键（`enableMnemonics` 未开），所以方向键是唯一会踩到隐藏项的路径。
    - **分隔线也一并隐藏**（同样不能删）：Monaco 的分隔线是 `.action-item.disabled > .action-label.separator` 的空行（11px）；剔掉条目后会出现“相邻两条贴着”与“末条悬空”（实测：Peek 与 Copy 之间那条与 Copy 之后那条贴成两条空行、Command Palette 剔后末尾多一道线），以及**首位分隔线**露出。`menuPrunePlan`（纯函数 + 7 例单测）按“先剔条目、再让每段只留第一条分隔线且首末不悬空”的顺序算隐藏计划，返回的是**原始行序下标**（实现按同一顺序隐藏，序号契约由单测钉住）。
    - 匹配用 `aria-label` 的英文标题（Monaco 单机构建无中文 NLS），对不上最坏是条目照旧出现、不会坏功能。
  - **宿主自定义组（`EditorMenuHooks.groups`）**：菜单项的**内容 / 文案 / 可用性全由宿主给**（编辑器这层不认 Git 这类业务概念），组名取 `gebai.<id>`——字典序排在 `gebai` 之后，于是它与内置两项之间**自带一条组间分隔线**（菜单按组画分隔线，组名相同才并成一组）。组装配移**到句柄建好之后**（`groups(handle)` 拿得到编辑器句柄）：组内容常要按**编辑器能力**定——「行尾 blame / 侧边 blame 列」只在 **Monaco 编辑器**（`kind === "monaco"`）下给，降级实现画不出行装饰（`setBlame` 是空实现），给了就是“点了没反应”。**菜单可重装**：Monaco 的菜单项标题在 `addAction` 那一刻定下，文案带状态的项（`✓ 行尾 blame`）状态一变就得整组重装——`EditorHandle.refreshMenu()`（进函数先摘上一轮再注册；调用点：`applyBlame` / `toggleMode` / `loadTab` 的行装饰清零处）。降级编辑器的自绘菜单同口径渲染（每次弹出前现取，`refreshMenu` 在那里是空操作）。
  - **Git 溯源组（工作台）**：`✓ 行尾溯源` / `✓ 侧边溯源` / `文件历史`（`git log --follow` 弹窗），仅 `gitStatus.isRepo` 时给。**顺序：两项行内溯源在前、文件历史在后**——右键是“对着某一行”发出的手势，默认意图多是问“这行谁写的”，文件级历史退一位（它也是三项里唯一会弹窗的一个）。这三项原先挂在标签栏动作轮盘的**内圈**上（当时是「内弧 = 历史三项」的分组），现移到这里（同属「这行、这文件是什么时候谁改的」，低频，但真要用时人正看着代码、右键就在手边），轮盘因此回到单弧。**编辑态下**「侧边溯源」项不撤掉，改文案「（编辑态不可用）」并在点击时给一句提示——它作为轮盘按钮时是**禁用态**，两种表达同一语义；整项消失会让人以为功能没了。**文案只留三个词**（不带括号说明）：同组三项短且无同名前缀，“行尾 / 侧边 / 文件”三个限定词已够区分（唯一保留的括号 `（编辑态不可用）` 是**状态**不是说明）。**界面文案用「溯源」而不是 blame**（blame 是 git 子命令名，作为用户可见名词太术语；代码标识符与文档里的机制描述仍叫 blame）。
  - **发送会话（仅分屏）**：独立 `/files` 标签页里没有对话可发，所以这一项**只在嵌入（分屏）时注册**（`EMBEDDED`，见宿主入口）；点击后 `postMessage` → 宿主 `files-split.ts` → `composer.insertIntoComposer`。宿主侧三条约定：**追加不覆盖**（输入框里常已有半句话，替掉就是静默丢草稿；空行分隔）、**聚焦并送光标到末尾**（下一步就是接着写“把这里改成…”）、**按 `input` 那一套收尾**（自动高度 + 发送按钮形态 + 清输入历史导航——不清的话接着按 `↑` 会把刚发进来的内容当成“上一条”换掉）。跨窗口消息再卡一道长度上限（64KB；工作台侧已按段截断）。
  - **发送的片段** = 引用行 + 围栏代码块（语言标注只用语言 id）：参考行**不能塞进围栏信息串**（把 `ts src/a.ts:12` 写在围栏上会被 markdown-it 整串当语言名交给 highlight.js，`getLanguage` 必然失败——代码块**静默丢高亮**）；截断提示（200 行 / 20k 字符，防“整文件塞进输入框”）放在代码块**之外**，否则复制走就带上了不属于原文的一行。
  - **无选中时给光标所在整行**（右键“发送”却不选任何东西是很常见的手势，空代码块等于白点一下）。降级编辑器的**只读态没有光标**（textarea 被 `display:none`，且 JS 赋值后 `selectionStart` 停在**文档末尾**——照搬会报出一个与用户所见无关的末行），那里改从 **DOM 选区**算：highlight.js 只套标签、**不增删字符**，按文本节点累加即源码偏移；无选区则**不给行号**（复制得到纯路径），也就没有可发的内容（不摆那一项）。——① **侧边列**（`gutter`）：编辑器左侧独立一列（`.fw-blame-gutter`，在 Monaco 容器**之外**：flex 兄弟，右侧 1px 弱线分隔），逐行显示「作者 · 时间」；行位置用 `ed.getScrolledVisiblePosition()` 随滚动/折行按帧重算（只渲染可见行，行节点池复用），未提交行转警告色；② **光标行行尾**（`inline`）：`after` 注入到光标所在行行尾，样式更弱（淡色 + 斜体 + 半透明），光标移动只更新这一个装饰集合。两者共用一份数据（`/git/blame`），可以只开一个或都开。**只有行尾态的开关记在浏览器本地**（`files/blame-prefs.ts` → `gebai.ui.blameInline`）：它是一条跟随光标的淡色注释，常开不妨碍阅读；侧边列占一行宽度，每次打开文件按关闭处理。两者都是装饰/注入文本，**不进模型**：复制、保存、撤销拿到的仍是原文，文字也**不可选中**（注入文本 `cursorStops: None` 让方向键不在注释里停靠，`user-select: none` 堵住鼠标框选）。**行尾态在查看与编辑态都给**（跟随光标的一行批注，编辑时同样有用），侧边列仅查看态（整列作者会随编辑漂移）。三个必须踩准的点：① 装饰集合用**编辑器实例**的 `editor.createDecorationsCollection()`——`monaco.editor.createDecorationsCollection` 不是静态 API，取值得到 `undefined` 后静默降级为「什么都不画」（按钮照常高亮、不报错）；② 注入文本挂在**空 range** 上时必须显式 `showIfCollapsed: true`（Monaco 取注入文本会按 `showIfCollapsed || !range.isEmpty()` 过滤，否则整条装饰被静默丢弃）；③ 时间取**相对值**（`ui.ts` 的 `timeAgo`）：blame 关心的是「多久前有人动过这里」，绝对时间戳要占 16 字符；作者名截 14 字符。
- **编码/换行保真**：读取探测 UTF-8(BOM)/UTF-16/GBK 回退/换行（lf crlf cr mixed），保存按原编码与目标换行回写；**写乐观锁**：保存携带 `etag`，磁盘已变则 409 返回当前内容 + 「覆盖/另存/放弃」。
- **Git 修改标记（dirty-diff gutter）**：仓库内已跟踪文件在编辑器行号旁标出未提交改动（VSCode 同款）——绿条 = 新增行、蓝条 = 修改行、行号上红三角 = 删除点，滚动条概览标尺与小地图同步落色块（「改了哪里」的地图）。三处一体都由 Monaco **装饰**画出（`EditorHandle.setGitBase`），不进模型：复制/保存/撤销不受污染。差异引擎是一个**隐藏的共享 diff editor**（不进 DOM：`0×0` 隐藏容器，只借差异计算不渲染）：原侧 = HEAD 基线文本，改侧 = 当前编辑器的**活 model**——打字即重算（`onDidUpdateDiff` → 重设装饰），无需自算差异（本页 Monaco 为 AMD min bundle，内部 diff 算法不可 require，但 `getLineChanges()` 是公开 API）；同一时间只跟踪一个 model（与「主编辑器常驻、一个活动标签」形态对齐），惰性单例（非仓库/未跟踪零成本）。基线取自现成端点 `/git/content?ref=HEAD`，按 `(root, repoRel)` 缓存、以 HEAD `oid` 为版本键（`refreshGit` 后 oid 变即作废重取——提交/重置后标记自动跟上新基线）；触发点：标签就绪/重建（`loadTab`）、切标签（`activate`）、git 状态到达（oid 变化）。**不标**的场景与 VSCode 同口径：非仓库、未跟踪（HEAD 侧 `missing`）、二进制/超体量（`binary`/`tooLarge`）、工作区读取被截断（行级差异会全错）、降级编辑器（画不出装饰；资格判定纯函数在 `files/git-gutter.ts`，含 `ILineChange→装饰区间` 的行号换算——空区间两种历史惯例与夹取均钉单测）。删除标记锚在**删除点后的第一行**（删除处没有新行可标，删到文件末尾时夹到最后一行）；概览标尺/小地图色走结构化 `color`（装饰 API 不认 CSS 类），随主题令牌刷新（success/accent/danger），gutter 条色则走 CSS 类。
- **状态跨刷新保留（本标签页）**：打开的标签（root / path / 查看态 / 光标行）、活动标签、当前根、左栏视图与显隐写进 **`sessionStorage`**（`files/session-state.ts`，键 `gebai.ui.fwSession`）——它是**本标签页的会话状态**：独立 `/files` 标签页与分屏 iframe 里的工作台各记各的（同源 iframe 与宿主共享同一份 sessionStorage，用 localStorage 会把两处记忆互相覆盖），刷新（含 dev-reload）后回到原处，关掉标签页即随会话消失；**用户级偏好**（自动换行 / 小地图 / 行尾 blame / 主题 / 面板宽度）仍走 localStorage。只记**普通文件标签**——差异 / 合并 / 暂存 / 比较标签各自需要打开时的上下文（端点对、冲突文件、比较两端），一个路径恢复不出来；有未保存修改的标签按查看态打开并**如实提示一次**（内容不跨刷新，别让人以为改动还在）。恢复与 URL **取并集**（`boot()` 阶段二）：先按记忆把上次的标签恢复出来，再让 URL 落位（深链接 / 前进后退是明确的「看这里」，它决定的是**活动标签**）。不能写成「URL 带 `path` 就整段跳过记忆」——普通刷新后的地址栏里总带着当前文件（`activate` 会同步地址栏），跳过记忆就变成「一次刷新只剩那一个文件」，且随后的写回会把记忆也改成缩水状态（另一个标签从此再也回不来）。该文件已在记忆里时 `openFile` 命中已有标签、只把它激活并跳行，不会重复打开；新建标签页的深链接不受影响（新标签页的 sessionStorage 本就是空的）。**根不在清单里的标签跳过**（项目被移除 / 换了会话），否则刷新的结果是一串打不开的错误页。解析对脏值免疫（形状不对的条目丢弃、标签数上限 24）、无内容时清键不留残留——纯函数带单测（`session-state.test.ts`）。
- **标签的光标与滚动位置记忆**：`activate()` 会把上一个标签的**光标行/列与滚动位置**记在标签对象上（`cursorLine`/`cursorColumn`/`scrollTop`），回到该标签时先落光标、再回滚动（**顺序要紧**：反过来的话 Monaco 会因为“光标仍在原处”而在落光标那一步把视口扭回去，刚回的滚动位置白记）；编辑器（Monaco / 降级）两侧都实现 `getCursorPos()`/`setCursorPos()`（降级侧由 textarea 的 `selectionStart` 反推行/列）。为什么真正起效的地方是 **`loadTab` 重建**而不是切标签：编辑器实例是**常驻**的（切标签只是 `display:none`，位置本来就不会丢），会丢的只有重建——「重新加载当前文件」、「以文本打开」、图表源码↔渲染预览互切、大文件在 Monaco/降级间切换都走 `loadTab` 拆了重装（带 `opts.line` 时不还原：深链接/定位给出的目标优先）。同时每次激活都把状态栏的光标从**这个**编辑器同步过来（`state.cursor` 是“上一次光标事件”留下的值，而 `onCursor` 只认活动标签、`setCursorPos` 又只在位置真变时才触发事件——不校正的话切到一个光标恰好在原位的文件，状态栏会继续显示**上一个文件**的行号；這个陈旧值还会经 `session-state` 写进刷新记忆，下次刷新就把那个行号带到另一个文件上）。
- **下载/上传**：单文件流式下载（`Content-Disposition`）；多选/目录打包 ZIP（`zipPaths`，UTF-8 文件名，超限 413 引导分批）；上传支持拖拽到指定目录、同名冲突处理。
- **Git 图形化（任意两端对比为一等公民）**：差异统一抽象为**端点**——`WORKTREE` / `INDEX` / 空串（与 WORKTREE 搭配 = 未暂存）/ 任意 rev（提交/分支/标签/`HEAD~n`/SHA）；`compare(from,to)` 支持提交↔提交、提交↔工作区、提交↔暂存区、暂存区↔工作区、工作区↔历史提交、分支↔分支（`mergeBase` 三点语义）；`contentAt(ref,path)` 供 Monaco DiffEditor 取 **两侧真实文本**（非解析 patch，CRLF/编码差异下表现正确）；前端「比较」标签页两端各有端点选择器（工作副本/引用/本地分支/远程分支/最近提交/手输 rev）+ 交换 + `...` 开关 + path 过滤。界面为**无标题栏的三区**（IDEA 新 UI 式，见下文「界面结构」）：**左栏工具窗**（变更／资源管理器／搜索，三视图互斥）+ **底部工具窗**（分支／日志／提交内容三栏并排，分界可拖）。变更面板负责「我改了什么、要不要提交」（分组/逐块暂存/放弃更改 + 提交框 sticky 底部），底部三栏负责「历史上发生过什么」（点分支 → 看它的日志 → 点提交 → 右栏看内容）——「分支」栏内再切标签、**储存**（stash）、远程（fetch/pull/push force-with-lease）；破坏性写操作默认先备份并返回可恢复引用；`git blame` 追溯、冲突四方内容（base/ours/theirs/current）查看。**术语严格分开**：**暂存**（index，`git add`）与**储存**（stash，存放工作现场）不是一件事，界面文案与入口都不混用。
- **冲突合并（三窗格）**：对齐 IDEA 的 Merge 工具——冲突文件（Git 面板冲突组的 `merge` 按钮 / 双击 / 右键「解决冲突」）打开合并标签：左「我方（当前分支）」/ 中「合并结果（**可编辑** Monaco）」/ 右「对方（合入分支）」，可选展开「共同祖先（diff3 的 `base` 段，无则提示）」。工具条：冲突导航（上一个/下一个，显示「冲突 n/m + 各段行数」）、采纳我方/采纳对方/两者都留（作用于当前块）、全部我方/全部对方（整文件，二次确认）、保存（`Ctrl+S`，走 fs 写 + **etag 乐观锁**，外部改动 → 409 而非静默覆盖）、标记为解决（`Alt+M`，`git add` 后冲突态结束）。快捷键 `F8`/`Shift+F8` 跳冲突/上一个变更文件。
  - 解析与替换是**纯函数层**（`files/merge.ts`）：`parseConflictBlocks` / `applyResolution` / `hasConflictMarkers`——单独成模块是因为「合并会改写用户文件」，一旦解析漏块或替换错行就是静默损坏，必须能脱离 DOM/Monaco 单测（16 例：两段式/diff3 三段式/多块/未闭合与畸形标记/CRLF/空侧/`both` 顺序/批量解决行号不偏移）。容错取向：**只把完整闭合的块算作冲突**，畸形片段原样保留不参与解决（宁可不识别，也不乱改内容）。
  - 渲染与交互在 `files/merge-view.ts`；三窗格窄屏（<1180px）纵向堆叠。工具条另有**冲突计数**（「N 处冲突」）与「全部两者」（每块都保留两侧）。
- **部分暂存（按块 / 按行）**：差异不必整文件进暂存区——未暂存侧可「暂存此块 / 丢弃此块」，已暂存侧可「取消暂存此块」，逐行则由每行的复选框控制（上下文行不可勾）。
  - 底层是一条**只含选中改动的补丁**：`buildPartialPatch`（纯函数，`core/git/service.ts` 末尾）从单文件 unified diff 里切出选中的 hunk/行——未选中的 `+` 行整行丢弃、未选中的 `-` 行**降级为上下文**，hunk 头按新计数重写（与 `git add -p` 同一套语义）；随后按方向施加：暂存 `git apply --cached`、取消暂存 `--cached --reverse`、放弃 `--reverse`（**不建 stash 备份**：`backup:false`，与整文件放弃同一口径——要留存改动请在「储存」栏显式储存）。
  - **为何构造补丁而不是直接写文件**：写工作区再 `git add` 会改动用户文件（哪怕只是中间态），而 `git apply` 靠上下文匹配、失效时整体失败且不落盘——「行号已经变了」会报错而不是改错地方。构造环节单独成纯函数是因为「拼错了就是静默改错内容」，必须能脱离仓库单测（12 例：整块 / 仅删除行 / 仅新增行 / 单行 / no-newline 标记 / 空选择 / 真实仓库回环）。
  - 入口三处（对齐 IDEA）：差异视图的「逐块操作」开关（`files/partial.ts`；Monaco 是只读阅读器、放不进逐块控件，故整块换成可勾选清单）、变更面板的行内 hover 按钮与右键「逐块暂存…」、「三向暂存编辑器」（`files/staging.ts`：左 HEAD / 中**暂存区内容可编辑** / 右工作区；写入走 `stage-content` 的 hash-object + update-index，不碰工作区与 HEAD）。
- **编辑历史（交互式变基）**：日志条目右键「编辑历史（从这条之后改写）…」——列表**新→旧**展示（与日志一致），每条可选保留 / 改信息 / 压合 / 修补 / 丢弃 / 停下编辑，↑↓ 调顺序，压合与改信息就地编辑提交信息；提交给服务端时翻成**应用顺序（旧→新）**。
  - 服务端**自己重放提交**（切到基准 → 逐条 cherry-pick → 回写分支）而非 `git rebase -i`：`-i` 要交互式序列编辑器（`GIT_SEQUENCE_EDITOR`），跨平台得写临时脚本，而每步的语义（压合时如何合并信息、停在哪条）依旧要自己掌握；重放则把「计划」变成我们自己的数据，冲突/暂停时可**落盘到仓库的 git 目录**（`gebai-history-edit.json`），继续与中止都可控。压合的写法是「先正常 cherry-pick 出提交，再 `reset --soft HEAD~1` 拆回暂存区，最后 `commit --amend` 让上一条吸收」——这样中途出冲突仍走 git 自己的 `CHERRY_PICK_HEAD` 状态。
  - 安全网：动分支前先建 `gebai/backup-<ts>` 备份分支；未完成的计划在变更面板顶部出「继续 / 中止」横幅（中止 = 分支指回原 HEAD，备份分支保留供比对）。限制：要求工作区干净、在分支上（非游离 HEAD）、不重放合并提交（需指定主线父，不在本能力语义内）。
- **工具窗与目录树的 IDE 细节**：
  - **底部停靠**：Git 工具窗在编辑区**下方**（`--git-dock-h` 高度可拖，默认 300px，双击拖条复位，高度记忆在 localStorage；窄屏 <1180px 时三栏纵向堆叠），rail 上的 git 按钮切换展开/收起、展开态高亮；关闭按钮在**面板自己的标题栏**内（与 IDEA 工具窗一致，不在标签栏上）。
- **左栏三视图常驻、只切显隐**（`mountLeftView`）：变更 / 资源管理器 / 搜索挂在同一栏里（隐藏类 `fw-view-hidden`），不再 `clear + appendChild` 搬动子树——早期做法每次切换都要重排整栏，且被摘下的子树**丢失滚动位置**（目录树看到一半去看「变更」再回来，树回到了顶部）。
- **尺寸变化与编辑器重排**：拖动分界条（左栏宽度、工具窗高度）的尺寸写入按帧合并（`requestAnimationFrame`，每个 mousemove 直接写 style 会连带强制布局；工具窗底边在拖动期间恒定，起手量一次），且**只重排活动标签**的编辑器，松手时再一次性补齐全部（`layoutAllEditors`）——Monaco 虽开 `automaticLayout`，隐藏标签在 `display:none` 下量不到尺寸，仍需显式 layout，但拖动过程中每帧给看不见的标签重排纯属浪费。分屏宿主（主界面侧）的 `resize` 回调同样按帧合并。
- **Git 面板取数成本**：① 分支的 ahead/behind 用 `for-each-ref` 的 `%(upstream:track)` 一次带出（早期对每个跟踪分支各跑一次 `rev-list`，上限 50 个 = 最多 50 次进程启动，Windows 上单次 ~65ms；不认识 `:track` 的老 git 回退到旧的逐分支计数路径）；② `compare` 不再额外跑一次 `git diff --stat`（那又是一次完整内容 diff，只为拿一行摘要），`stats` 由已解析出的文件清单**本地汇总**（形状对齐 git：每文件一行 + 末尾汇总）；③ 面板**收起时不刷新**内部三栏（保存/`F5` 链只在展开态刷，展开时 `toggleGitPanel` 补刷），刷新入口做同刻合并（切根 + 保存 + `F5` 常在同一拍里触发）。
- **降级 diff 渲染的行数上限**：Monaco 不可用 / 两侧超体量时走结构化 hunks 渲染，单文件上限 **3000 行**（服务端的字符上限能装下数万行，每行 4 个 span = 十万级节点会让主线程停机数秒），超出部分明确提示「仅渲染前 N 行，另有 M 行未显示」。
- **日志与菜单的细节**：日志「滚动到底自动加载」监听**真正的滚动容器** `.fw-git-col-body`（早期挂在无 overflow 的 `.fw-log-list` 上，事件永不触发，是死代码）；右键/下拉菜单的全局监听**同步注册**并在 `closeMenu()` 里显式卸载（早期延后到下一宏任务注册、只靠 MutationObserver 清理，快速连开菜单会漏掉一对永久 document 监听器）；对话框遮罩去掉 `backdrop-filter`（全屏 blur 每开一次弹窗都让合成器把整个视口快照重模糊），菜单自身的毛玻璃保留（那是主题观感的一部分）。
  - **vendor 引擎脚本与 Monaco 的 AMD loader 冲突**（`files/viewers.ts` 的 `loadScript`）：工作台会加载 Monaco（`vendor/monaco/vs/loader.js` 定义全局 `define`），而 vendor 里的 esbuild/UMD 包（mermaid / echarts / viz）见到 `define` 就走 AMD 分支——与已存在的匿名 define 冲突（`Can only have one anonymous define call per script file`），脚本抱错、全局变量永不挂载（表现为图表预览“渲染失败：Cannot read properties of undefined”）。解法：**加载期间临时摘掉 `window.define`**（结束后成败都立即恢复），迫使它们回落全局挂载。
  - **子菜单坐标用父菜单局部系**：`.fw-menu-pop` 带 `backdrop-filter`，而它会（同 `filter`）把自身变成 fixed 子元素的**包含块**——子菜单写视口坐标会被当成 host 内偏移，于是弹到很远的地方（鼠标根本移不到）。所以子菜单 `left/top` 按 `btn.right - host.left + 2` / `btn.top - host.top` 算（并钓制在父菜单高度内，否则鼠标一出父菜单就丢）；开合用父项/子菜单双向 `mouseenter/mouseleave` + 150ms 延迟收起，且**已展开不重建**（重建会把鼠标正下方的子菜单换掉）。
- **查看器与负载释放**：图片查看器的拖拽 `mousemove/mouseup` 只在拖动期间挂、松手即摘（早期一次注册就永久留在 window 上）；标签加载用**令牌 + 存活校验**（`loadGen`），`await` 回来后若标签已关闭或被新一轮加载取代就当场回收刚建好的编辑器；重建视图前统一 `dispose` 旧编辑器/查看器，比较标签补上 `viewDispose`（早期它从不设，`compareTabs` 里的视图与状态一直留着）；图表「源码 ⇄ 渲染预览」互切先把上一态彻底卸掉（只 append 不清 host 会两态同屏叠着），且标签有未保存修改时拒绝切换。
  - **目录树 VCS 装饰（IDEA 风格）**：文件名下方**彩色下划线**表示状态，同时保留右侧**字母徽标**（精确状态：M/A/D/R/S/U/!）与 hover 提示。颜色与 Git 面板的变更字母**同一套语言**（修改=警告色琥珀 / 新增·未跟踪·已暂存=成功色绿 / 冲突=危险色红 / 删除=红色删除线 / 仅「子项有变更」的目录=中性灰）——不用 `--tool`（那是聊天里工具消息的颜色，部分主题下是灰的，语义不符）。装饰刷新走 `explorer.refreshGitDecorations()`——**只换装饰元素、不重建树**（树的展开态/滚动位置/选中项都在 DOM 里，重建会「折叠回去 + 滚动跳顶」）。
    - 修过的坑①：git 状态是异步到达的，而树在状态到达**之前**就渲染完了；早期只在渲染时取一次 `gitStatus()` → 徽标永远为空（只有手动刷新才出现）。现在 `refreshGit()` 拿到状态后回填装饰。
    - 修过的坑②（**`line-height: 1` 把字母下半部裁掉**）：树容器原为 `line-height: 1`，行盒 = 12.5px 而字体 ascent+descent = 15px——半行距为**负**，基线以下只剩 ~1.75px；而 `g/j/p/q/y` 的尾巴需要 ~3px，文件名 span 的 `overflow: hidden`（横向省略号所必需）会把**纵向溢出一起裁掉**。后果：`keqing` 的 `g` 尾巴被切平，看起来像 `q`（更糟的字体度量下切得更多），且新加的状态下划线（baseline 下 2px）**整条不可见**——computed style 说 `underline`，实际一个像素都没画。改为 `line-height: 1.55`（19.4px，基线以下 ~5.2px）后尾巴与下划线都完整，行高 22px 不变。**教训：`overflow: hidden` 的省略号容器会把 `line-height < 字体度量` 的墨迹（含 descender 与 text-decoration）静默裁掉，且 computed style 不反映——必须看渲染结果或量基线以下空间。**
- **子系统边界（会话工作区 vs 仓库根）**：root 可为仓库子目录（典型：会话 `tmp/` 位于项目仓库内）——服务端返回 `rootPath` + `prefix`，Git 面板与比较视图**默认限定该子目录**（chip 一键切整仓库；限定范围内无差异且整仓库有差异时给引导）。路径语义：**git 侧为仓库相对**（`contentAt`/git 命令）、**fs 侧为根相对**（树/编辑器），前端 `toRootPath()` 负责在打开文件时换算；`routes/git.ts` 仓库定位用独立 `dir` 参数（不复用 `path`——`path` 在这些端点是 pathspec，混用会把文件当目录）。
- **安全与审计**：`GEBAI_FS_ENABLED`（总开关，false → 页面与 fs/git 端点全 404）/`GEBAI_FS_WRITE`（工作台写开关）/`GEBAI_GIT_WRITE`/`GEBAI_GIT_REMOTE`；写操作统一审计（`GEBAI_FS_AUDIT` → `{GEBAI_HOME}/audit-fs.jsonl`）；**删除即物理删除**（不设回收站，破坏性操作由前端二次确认把关）；上限 `GEBAI_FS_MAX_READ|WRITE|UPLOAD|ZIP`，以及差异侧的 `GEBAI_FS_MAX_DIFF` / `GEBAI_GIT_MAX_FILE`（见下文「体量与失败」）。
- **与主界面的集成**：除标题栏轮盘左侧的「文件」按钮外，**消息流里的文件产物也能一键跳工作台**——文件链接 chip、文件卡工具栏、原文件弹窗标题栏三处各有「在文件工作台中打开」（`file-wb-icon`）；产物在哪个会话产生就打开哪个会话的工作区（chip 显式携带渲染时的会话 id，不随当前会话漂移）。
- **深层链接（`files/deeplink.ts`）**：主界面只透传**原始路径**（`/files?session=&path=[&line=]`），**根的定位在工作台侧完成**——解析逻辑只此一份，主界面无需复制根清单与匹配规则，根的增删/改名都不会让入口失配。三档优先级：① 显式 `?root=`/`?project=`；② `?path=` 为绝对路径 → 根清单中**最长前缀匹配**的根（平局时按类型：proj/bind > sess > user > abs——本地模式的 `abs:` 白名单根常与注册项目指向同一目录，不应抢占项目根）；②b 绝对路径不属任何已知根（项目外的 Agent 产物/临时目录，如 `/tmp/x/f.txt`）→ 以**所在目录**为 `abs:` 根直达（服务端在沙箱模式下 403 拒绝，前端不是安全边界）；③ 其余（相对路径或无 path）→ `?session=` 的会话根（无参数时默认项目根 → 绑定根 → 第一个根）。**显式 `?root=abs:<绝对路径>` 即使不在根清单内也接受**（「打开任意文件夹」链接直达）；会话根只接相对路径——绝对路径交给会话根必然越界，故走 ②b。`?line=` 打开后跳行（产物行号直达）；**路径归一按目标根类型**（`normalizeArtifactPath`）——会话根下剥掉 `tmp/` 前缀（会话根本身指向 tmp，再带一层即 `tmp/tmp/…`），而项目/其它根下的 `tmp/` 是正当目录名**不剥**（仓库里真有 tmp 目录）；同一份规则被地址栏恢复（`restoreFromUrlState`）复用——它此前直接用原始 query 路径，与深层链接解析分叉，会出现「同一链接刷新后多一层 tmp 而 404」。纯函数、参数可注入（`search`/`isWin`）、无 DOM 依赖——28 例单测覆盖（显式根/清单外 abs 根/最长前缀与嵌套根/Windows 盘符大小写/会话相对路径与 tmp 前缀按根类型/显式会话根/项目根 tmp 不剥/中文空格路径/无匹配回退）。
- **界面结构（无标题栏三区）**：整体对齐 IDEA **新 UI**——去掉传统标题栏/菜单栏，入口交给**左侧活动栏**与各面板自身：
  - 活动栏分两组（中间以细分隔）：**上组 = 视图切换**（资源管理器 / 搜索 / 变更，三视图**互斥**——同一时刻只看一件事：要么在找文件、要么在看改动）；**下组 = 工具窗与全局入口**（Git 工具窗开关 / **更多**）；**嵌入（嵌入 iframe）时最下方多一对同窗切换按钮**：主按钮 = **关闭文件工作台**（左箭头，与「更多」里那一项同一个图标、同一个动作，走既有的 `requestCloseSplit()`），悬浮时从它右侧弹出的辅助按钮 = **进入分屏**（两栏图标，`gebai:files-enter-split`；只有全屏态才渲染）——面板内最直接的那两个出口，独立标签页不渲染。
  - 「更多」承接原菜单栏的杂项（新建文件·文件夹 / 上传 / 比较 / 刷新根清单 / 快捷键一览 / 服务端开关 / 浏览器全屏 / 关闭文件工作台（独立标签页下是「返回歌白主界面」）），`Ctrl+K` 直开；文件类动作同时在树右键菜单里有（两处入口，不是唯一路径）。**没有面包屑与单独的工具条行**：路径信息由标签标题 + 资源管理器表达。
  - **预览标签 ⇄ 常驻标签（VSCode 同款，双击切换）**：标签有**预览态**（VSCode 说法 preview，本页表达为**标题斜体**，`.fw-tab.preview .fw-tab-title { font-style: italic }`）——预览标签**只有一个**，下一个预览会把它**就地换成新文件**（同一个标签、不堆标签），适合“扫一眼几个文件再决定看哪个”。三处交互对齐 VSCode 的口径：① **资源管理器单击文件 = 预览**（`hooks.openFile(root, path, { preview: true })`）、**双击 = 固定**（`{ preview: false, only: true }`）；② **标签上双击 = 固定 ⇄ 取消预览**（与标签右键菜单「固定/取消预览」同一动作、同一份状态）；③ 快速打开里 `Enter` = 预览、`Ctrl+Enter` = 固定。四条状态机约定：**(a) 单击的预览延后一拍落地**（时序逻辑在 `files/preview-click.ts`，纯函数 + 单测；资源管理器只负责把行元素交给它）。为什么不立即开：双击在浏览器里是“两次 click + 一次 dblclick”，而单击与**双击的第一发**在事件层一模一样（实测里第二发 click 还可能被浏览器合并掉）；而“开预览”在标签层是**就位替换**（旧预览会被顶掉）。立即打开的话，双击 B 就变成“第一发把上一个预览 A 顶掉 → dblclick 把 B 钉住”，A 已经默默没了；延后到双击窗口之后，双击只剩“开 B + 钉住”一个结果（待定的那一发被作废，连请求都不发）。代价是单击的预览晚 220ms 出现（选中/高亮是即时的），换来双击不再产生中间态。**(b) 双击是「只要这一个」**：`openFile` 的 `only`（浏览器在双击时只给一次 click + 一次 dblclick，第二发 click 被吞，于是走的会是“标签已存在”分支、只把它钉住）——双击路径先把预览槽里那个无关的文件收掉，再把目标标签建/钉成常驻。**(c) 离开即落定**：切到别的标签时，被留下的预览标签自动转常驻（`activate` 里做）。不这样做的话，它永远占着唯一的预览槽，下次单击别的文件会把它顶掉，而它早已不在眼前。**(d) 就位替换的旧预览不是“离开”**：替换前先把 `prev.preview` 置假，它被顶掉就是被顶掉（少了这一行，旧预览会被 (c) 误当成“用户切走了”而转成常驻标签永久留下来——实测症状：双击 B 之后，先前的预览 A 变成第二个标签，B 自己反而还是预览）。同理，**命中已有标签时不把它的预览态改回去**（`opts.preview === true` 不清不设）：否则“单击一个已固定的文件”会把它悄悄降回预览，接着双击的第一发又把第二发钉住——两次交互互相抵消，用户看到的是“双击没反应”。预览态**不持久化**（刷新后恢复的标签一律常驻，见 `session-state.ts`：恢复本来就是为了接着用）。双击面板里的文件行（目录行是展开/折叠）、右键「打开」、搜索结果、变更面板等**明确动作**仍走常驻打开（不传 `preview`）。
- **标签栏右侧只留高频动作，其余进轮盘**（与标题栏轮盘同一套交互，共用 `wheel-core.ts` + `css/wheel.css`；动作区最左另有**编辑位置历史的「← →」按钮组**（见「编辑位置历史」条）——全局能力不随标签类型变，无历史时整组隐藏；差异标签在这里没有动作——导航按钮随差异视图头部走（见「变更文件遍历」条）；以下为原动作区的布局）；入口 hover 展开、键盘 Enter/空格/↓ 同样展开，Esc/外点/resize 收起；**圈位**：首圈半径 85px，之后每圈步进 = ceil((边长·√2 + 间隙)/2)·2（**按最坏情况 45° 对角取值**：同角度斜向也不叠），**组内均衡分圈**（先算最少圈数再把项数均分、每圈项数差 ≤ 1——不是贪心填圈，避免只有一枚按钮的**孤项圈**），每圈在**自己的角窗口**内均布（下界 = 右缘不越屏、上界 = 不压入口行；两约束对 90° **不对称**，故围绕**窗口中心**而非全局轴心展开——旧「固定起始角向左侧长」会把若干按钮送出右缘），**跨圈避让**（本圈在窗口内整圈滑动找无碰撞中心角），每圈容量按该圈角窗口算、放不下的自动落到下一圈，可用空间（视口边界 + margin）不足时不再往外开圈而是并进最后一圈；角度由**角窗口**定（见上），上限内的最小张角用二分取「刚好放下」（不硬撑到固定张角：实测 2 项那圈只有 18.9°，否则会空出一大截弧），真放不下才停在上限；每圈一条引导弧线），常驻「编辑/查看」，以及**可渲染文件多一个「渲染/原文」切换**（**markdown（md/markdown/mdx）渲染为文档**、图表源码（mermaid / plantuml / d2 / echarts）画成图；它决定看到的是文档/图还是文本，比其它动作都要紧；图标随之在“切换到渲染预览 / 切换到源码”间切换并标 active）；判定见 `files/preview-kind.ts`（纯函数：markdown 的服务端 kind 是 `text`，不能靠 kind 分派，只能按扩展名；该模块也是图表扩展名清单的唯一真相），markdown 渲染**懒加载**与聊天页共用的 `md-core.ts`（工作台首屏不背 markdown-it + highlight.js 体积）；两类预览容器都带 `.fw-preview`（标签栏按钮与 toggleRendered 靠它判定“当前是预览态”）；轮盘**按空间自动分圈**（历史相关三项已撤出、移入**编辑器右键菜单**自成一组，见上文「编辑器右键」；5 项 @32px 实测排成 3 + 2 两圈，弧线两条；**入口不挂 title**——展开后每个扇形按钮自带 tooltip，入口再弹一条八项清单是重复且正挡在刚展开的扇形上，可访问名称改用 `aria-label`，定位入口用 `.fw-tabbar-actions .fw-wheel-btn`）：**文件本身的动作**（保存 / 重载 / 下载 / 复制路径；保存带脏标记高亮与禁用态）**+ 显示开关「自动换行」**（全局偏好，按钮高亮即开关态，与 `Alt+Z` 同效，见「自动换行」）**+ 显示开关「小地图」**（全局偏好，**默认开启**、按钮高亮即开启态，见「小地图」；6 项 @32px 实测排成 3 + 3 两圈、弧线两条、无重叠）。按钮区**不画分界线**（它紧贴标签栏右端，多一条线只是把两个相邻区域切成两栏），与标签栏底色一致。轮盘里的都是“偶尔用一次”的动作，铺成一排会把编辑区右上角变成按钮墙、也把高频动作淹没。两个常驻按钮与轮盘入口均仅图标、随活动标签类型变化；Git 动作在各自面板里。
  - **轮盘的保持区不吃指针事件**（`wheel-core.ts` + `css/wheel.css`，两个入口共用）：容器是一块覆盖扇形边界盒的矩形，盒下面往往就是真实控件（入口在界面右上角、扇形向下左展开，标签栏与编辑器上缘正在盒内）——容器一旦可命中，展开期间那些控件就都点不到（点击落在容器上：既不触发下方按钮，也不算“点了外面”，页面不报错、只是「点了没反应」）。因此容器一律 `pointer-events: none`（展开态也不恢复），只有扇形按钮自己 `pointer-events: auto`；「指针还在扇形附近就不收起」改由 document 上的 pointermove 按坐标与容器矩形比对（**展开期间才挂**，收起态不跑每帧回调），指针离开整个文档（切窗口）另由文档根的 pointerleave 兜底（文档根不冒泡，捕获监听才收得到；只在事件目标就是文档根时响应——元素级 pointerleave 是“指针从入口滑到扇形按钮上”这类正常移动）。点扇形按钮后收起仍靠容器上的 click 冒泡，与容器是否可命中无关。
  - **标签条溢出时在条内横向滚动，右侧动作永远在位**：标签栏分三段——`.fw-tabstrip`（标签条，`flex: 0 1 auto` + `min-width: 0` + `overflow-x: auto`）、`.fw-tabbar-spacer`（空白区，双击快速打开）、`.fw-tabbar-actions`（动作区，`flex: none`）。**标签 `flex: none`**：宽度只由标题与 220px 上限决定，不随标签数量变窄（压缩标签只会得到一排只剩省略号的窄条，等于没有标签栏）；`min-width: 0` 是标签条能被压缩的前提（flex 项默认的 min-content 底线会顶住不缩，压不动也就滚不起来）。于是宽度不够时唯一的变化是标签条自己滚，被牺牲的也只有它（空白区与动作区各留自己的最小宽度，动作按钮不会被顶出 `overflow: hidden` 之外）。**横向滚动条不画**（34px 的栏里 11px 的条会把标签压矮一档，而它只在溢出时出现、那正是最需要这 34px 的一屏）：滚法由三处承担——滚轮（`main.ts` 显式把 deltaY 交给标签条，Chromium 不把纵向滚轮映射到横向滚动容器上；横向滚动与 Ctrl+滚轮缩放不拦）、触控板与 Shift+滚轮的横向滚动、以及 `scrollActiveTabIntoView`（每次重渲染后按需调 `scrollLeft` 把活动标签带进视野——新标签总长在最右，Ctrl+Tab 也可能切到视野外的那个）。
  - **面板之间的分隔比控件边框更弱一档**（`--fw-divider` = `--border` 降饱和到 55%，见 files.css 的 `.fw-page`）：`--border` 是「内容与背景的分界」（表单/卡片/输入框/图标按钮，需清楚），而外壳各面板只是同一面墙的不同房间，同强度的线会把界面切成方格纸——所以另立一档更弱的分隔色，只给外壳级分界用（控件与列表行分隔仍用 `--border`）。**活动栏右边界（1px `--fw-divider`）属于这一档**：活动栏与左栏虽同为 `--bg-elev`，但职责完全不同（面板开关 vs 内容），没线时两块贴在一起像一块宽面板；左栏收起时它同时充当活动栏与编辑区的分界。另外**左栏首行与标签栏同高（34px）**：两处本是同一条横线，差几像素就会看成两道错位的分界。**但这一档只在“真需要分界”处用**：资源管理器头部与目录树之间**不画线**——它们同属一个面板（一棵树从头部下面长出来是自然的），头部的控件已足够表达“这里是头部”；线反而把两个像素级的孪生区域切开。
  - **底色已不同处不画线（线只在“两侧同底色”时才承担分界）**：相邻区域各自有背景差时，线是重复信息，白添噪声与疲劳；反之两侧同为一种底色时，没有线就分不清是两块区域。按此判据，工作台里保留的线只剩：活动栏右界与页签之间（同为 `--bg-elev`）、可拖的三栏分界（`.fw-col-resizer`）、以及控件/弹层自身的轮廓（按钮、输入框、芯片、菜单、对话框——它们是浮在背景上的实体，没轮廓就糊在一起）。而栏头、工具条、列表分组标题、查看器条、横幅、比较面板头部、对话框标题/底部动作区、提示条、状态栏上界、页签栏下界都属于“两侧底色已不同”→ **一律不画**；**底部工具窗的上界例外**（`.fw-git-dock` 的 border-top 必须保留——它是编辑器与工具窗唯一的分界，两侧同为 `--bg-elev`）。明细见 `css/files.css` 与 `css/terminal.css` 中各选择器的注释。
  - **Git 工具窗没有标题栏（一个都不留）**：面板内不再有任何标题栏/状态带，三栏直接顶到面板上沿——标题栏那一行里每一件都有别的载体（面板身份＝活动栏高亮，「源代码管理」四字本身不是信息；当前分支＝状态栏的分支项以及分支列表的 ✓；「比较」＝活动栏「更多」与 `Ctrl+Shift+D`；刷新＝各栏自己的刷新与 `F5`；关闭＝面板右上角那枚 `panelClose`），而“写操作在途”与“多步操作进行中”这两件**别处说不了**的事改由**页面底部状态栏**承担（`.fw-status-item.git-busy` 带旋转的同步图标与中文在途文案，多步操作格带冲突数）。取舍：进度提示离操作区远了一点，换来的是**面板布局恒定**——标题栏忽隐忽现不再把三栏上下推挤（每触发一次写操作，三栏都要跟着跳一下高度）。
  - **就地关闭入口在面板右上角**（`panelClose`，放在第三栏头部末端——最后一栏的头部右缘就是面板的右上角）：面板没有标题栏，"在这里关掉它"就需要一个就近的东西（活动栏按钮与 `Alt+G` 仍在）。它与提交动作按钮同排，故栏头部的图标按钮一律 `flex: none`，栏被拖窄时不会被挤成一条线。
  - **终端面板的标题栏是常驻的**（Git 面板没有）：会话页签 + 新建/清屏/中断/查找/设置/关闭都是主入口、别处没地方放，因此它保留上下两条线。
  - **「拖动改大小」的分割区（`.fw-resizer` / `.fw-dock-resizer`）：拖条只管交互，面板自己画上下边界**——拖条铺**面板色**，指针进入/拖动中整条转 `--accent-soft`（这就是“可拖”的提示），同时在拖动期间让面板那条上边线让位（否则「1px 线 + 4px 高亮带」又是两道）。拖条**不能透明**：它本质是两块面板之间的 4px 缝，`transparent` 透出的是**页面底**（比面板暗一档：面板 `rgb(16,16,21)` → 缝 `rgb(8,8,8)`），中间会裂开一道比分界线还宽还黑的缝，那才是「分割区很割裂」的主因。也不能改成给父容器 `.fw-body`/`.fw-main-right` 铺色：面板自己的 `--bg-elev` 是半透明的，父级再铺一层会让左栏叠两层、比编辑器亮 2 级。**左栏与编辑器之间不画线**：两侧底色本就不同（左栏 `--bg-elev` / 编辑器 `--bg-inset`），色差已构成边界。
  - **滚动条静止透明**（与主界面 `#session-list`/`#messages` 同构）：树/列表/日志都是常驻长列表，滚动条常显就是一根赖在面板边缘的亮竖条，紧贴分隔线看着像“两条平行分界”（实测 slider 亮度 68 vs 分隔线 33）。WebKit 用 `*:hover::-webkit-scrollbar-thumb`（父元素 hover 对子元素的滚动条生效），Firefox 的 `scrollbar-color` 无“非 hover 透明”写法，降为“静止弱色、hover 强色”。
  - **外壳三区 + 活动栏贯穿全高**：`左栏 | 编辑区` 构成上部（`.fw-body`），底部工具窗在它下方、从**活动栏右侧**一直延伸到窗口右缘；活动栏是唯一贯穿全高的一列。理由：① 底部三栏（分支/日志/提交内容）看的是全局历史信息，与左边在看哪个目录无关，需要完整横宽才摆得下；② 视图切换/工具窗开关这类入口要在任何面板开合时都待在原位——被工具窗截断半截，下方那几个按钮会看起来像工具窗的一部分。左栏随之上收到上半部分（同 IDEA：打开底部工具窗时左侧 Project 工具窗被压矮，而非并排）。
  - **条目行内动作按 VSCode 的改动列表**（从左到右：**打开文件 → 放弃更改 → 暂存更改**；已暂存组是「取消暂存」，冲突组另有「冲突解决」，已删除条目不给自己没文件可开的「打开文件」）：**第一个按钮是「打开文件」（打开文件本身）而不是「打开差异」**——看改动是**点整行**的事，两者混在一个入口上会让人以为按钮只是「另一种看差异的方式」；右键菜单里两项也拆开（未跟踪无 HEAD 侧、已删除无工作区侧，各自按可行性隐藏）。**逐块暂存与三向暂存编辑器不进条目**（前者要勾选、后者是独立编辑器，都不是「一行一个动作」的粒度），统一收在右键菜单里——条目上的按钮越少越不容易点错，而那两件事本来也不是随手点的。
  - **树视图的目录行也有整支动作**（放弃 / 暂存 / 取消暂存）：树一收拢，一个目录就代表它下面那一批文件——「先把 `packages/web` 这一支暂存了」「这一支先都放弃」时逐行点文件名既慢又容易漏，目录行上的一键对着**整棵子树**执行（作用清单由 `collectDirPaths` 给出，含深层子目录里的改动；建树前算一次 `Map<目录, 路径[]>`，避免每个目录行各扫一遍分组）。**按钮集合与文件行逐条对齐**，只少一个「打开文件」（目录没有可打开的文件，它的行内动作就是点行折叠/展开）；顺序也照文件行（放弃在前、暂存在后），跨行读起来一致：
    - **未暂存**：`放弃该目录下的更改（N 个文件）` + `暂存该目录（N 个文件）`；
    - **未跟踪**：`删除该目录下 N 个未跟踪文件` + `暂存该目录（N 个文件）`（未跟踪的「放弃」就是删除，与文件行同一措辞）；
    - **已暂存**：只有 `取消暂存该目录（N 个文件）`（与文件行一致：先取消暂存再谈放弃）；
    - **冲突**：两个都不给（与文件行、组头一致：冲突要先解决再谈暂存/放弃——`git checkout --` 对 unmerged 文件本就报错）。
    - 文案带实际文件数（严格等于该支下的改动数）；**放弃是不可逆动作**，确认框写清范围与条数（`放弃「pkg/」下的 3 个文件的更改？…此操作不可恢复`），未跟踪则明说「不在 Git 版本控制里，删除后无法恢复」。撤销备份走服务端 `backup:false`（不自动 stash，见「放弃更改不带自动 stash 备份」那条），要留存改动请在「储存」栏显式储存。
    - 点按钮**不等于点行**（否则暂存完顺带把目录折上，还得再展开看结果）——`onclick` 里用 `closest("button")` 过滤，与文件行同一手法。目录行同样适用「按钮不可压缩、名字先让位」那条布局契约（目录名 `text-overflow: ellipsis`；实测最窄 180px、两个按钮同在也不越界）。
  - **行内布局：文件名优先、按钮不占常态宽度**。列表行按 VSCode 的改动列表排：`[标记] 文件名 目录 … [行内动作] [状态字母]`——**文件名在前**、目录作为灰色小字后缀跟在后（目录被收缩时它先让位，窄栏下整段消失，文件名仍完整）；M/A/D/R/S 这类**读出来的**状态字母靠右，只有冲突 `!` 与未跟踪 `U` 这两种「要动手的」留在行首。左栏窄（~286px），旧序（目录在前）下实测 46 行有 **43 行的目录被截成 `packages/ser…` 这类认不出的碎片**，而文件名反而被挤到中途才开始。
    - 收缩分工：**按钮组是不可压缩的整体**（`.fw-change-actions` `flex: none`，组内间距 2px）；文件名与目录各自 `text-overflow: ellipsis` + `min-width: 0`，收缩时目录的 shrink 权重远大于文件名（1000 : 1），即先让出目录、再让文件名出省略号；行上再叠一道 `overflow: hidden` 兜底。
    - **行内按钮在常态不占宽度**（`width: 0` + `overflow: hidden`，悬停/聚焦时让回宽度）：只用 `opacity` 藏起按钮时它仍占 60~70px 的行宽，左栏里这等于给每行强行挖走三分之一——实测把按钮改为零宽后，同样 10 行「文件名被截」从 1 行降到 0 行、「目录被截」从 8 行降到 1 行。代价是悬停行里文件名会即时收一点（只在用户正指着那一行时发生）。
  - **行密度**：改动行 **22px**、行左右各内缩 4px 带 5px 圆角（hover 底色是一块卡片而非通栏色带，VSCode 同手法）；行内图标按钮 20px（`pointer: fine` 下）—— 22px 行里放 24px 按钮会把行撑开。光标是指着具体按钮时那枚按钮给一档更实的底（`--bg-active`），否则与行 hover 的 `--bg-hover` 分不出层次。触屏档（`pointer: coarse`）仍是行 40px、按钮 32px、动作常显且占宽度。
  - 树视图缩进走 `--fw-depth`：目录行 `padding-left: 12px + d×14px`，文件行 `8px + d×14px` + 行自身 4px 外边距 —— 两者同层时箭头与文件名对齐在同一竖线上。
  - **行内图标取 VSCode codicon 同形**：「放弃更改 / 删除未跟踪」用 `discard`（U+EAE2）——右上一个环 + 一条从左下斜下去的尾，左侧一个直角箭头钩。旧画法是一个闭口圆环加左侧尖角，在 13px 下与「刷新」难以区分。复刻几何用官方 `codicon.ttf` 的 `discard` 字形做基准（16×16 栅格化后逐像素 IoU：旧图标 0.18、现图标 0.82，剩余差异主要是 16px 栅格量化）；**几何再缩 0.82 倍**（墨迹 13×13 → 11×11）：官方图形在 viewBox 里基本满格，直接摆进行内按钮会比同伴大一圈（实测同行：文档 8×13、加号 9×9）。
  - **头部有标题**：「更改」+ 待提交改动数（胶囊；与 rail 徽标同源、为 0 时整个隐去）。没有标题时左栏首行只剩两个右侧图标、左上角一整片空白，看不出这是哪一块面板；带数字比单写“变更”更有信息量。
  - **计数胶囊只有一份样式**：面板标题的总数与分组头的分组数（未暂存 6 / 未跟踪 1）**共用 `.fw-changes-count, .fw-change-group-count` 这一条规则**（两处是同一个东西的两个粒度，各写一份必然漂移）；两者都是 16px 高胶囊、强调色底。树视图目录行上的数字（`.fw-change-count`）是另一回事：灰字无底、贴着目录名，不能被胶囊规则误伤。
  - **「放弃更改」不带自动 stash 备份**（`discard` 传 `backup:false`）：丢弃就是丢弃（确认框明说不可恢复）。原先的「丢弃前自动 stash」会把每次放弃都变成「储存」栏里的一条记录，把**储存**（stash，用户主动保存工作现场）与**暂存**（index，准备提交的内容）两件事混在一起——现在两者的入口与文案严格分开：条目行上是「暂存更改 / 取消暂存」（index），「分支」栏内的小切换才是「储存」（stash：储存当前更改 / 恢复储存 / 删除储存）。服务端仍保留 `backup` 参数供 API 调用方显式要求备份。
  - **自动刷新**：变更面板与状态栏同源（`main.ts` 的 `refreshGit` 落地后即调 `changesPanel.refresh()`），外部写入（Agent 改文件、终端里跑 git）经「变更监听」唤醒后按 600ms（最长 3s）合并刷新一次 Git 状态，工具窗只在**可见**时连带刷新三栏（分支/日志查询比一次 status 贵得多）。已打开且**没有未保存改动**的文件被外部改写时就地重载（保留滚动位置、不重建编辑器；刚由本页保存的文件在 3s 内忽略回声，见「变更监听」）。
- **变更面板在左栏**（不是底部）：回答「我现在改了什么、要不要提交」——编码时随时要看，放左侧常驻；底部留给「分支 / 日志 / 提交内容」三栏，那是回顾历史时才看的，两者节奏不同。提交框 sticky 在面板底部，改动行内的 stage/unstage/放弃按钮 **hover 才显形**（列表保持干净；触屏媒体查询下常显，否则点不到）。面板**头部一行 34px**（与资源管理器头部、编辑器标签栏同高）：**视图切换**（**一个**图标按钮：点一下在列表 / 树之间切，图标 = 当前视图、title 说明点下去会变成什么——不用一对互斥按钮，那种写法里总有一个是“当前状态”而不可点）+ 刷新（重取状态并重渲染，与 `F5` 同一条路——“改完想立刻看结果”的预期落在面板自己的按钮上，不依赖宿主下一次刷新时机），两个按钮靠右缘。**改动列表一律整仓库**：root 只是仓库子目录时（会话工作区落在项目仓库内是常态）也不缩范围——提交动的是整个仓库，按子目录前缀过滤会让别处的改动从列表里消失且不留提示，用户会以为工作区只改了这几个文件。**改动列表两种视图**（`files/changes-tree.ts` 管数据、`files/changes.ts` 只管画）：**列表**（缺省）= 现有按状态的四个分组内平铺，行内 **文件名在前、目录作为灰字后缀跟在后**；**树** = 各分组内按目录收拢（目录行带后代改动数、可点击折叠，缩进走 `--fw-depth` 行内变量；分组头与目录行的折叠箭头同一套语言——展开＝向下、折叠＝向右，靠折叠类旋转，选择器只选首个直接子图标以免行内按钮跟着转）——两个视图看的是同一批改动，视图选择记在 `gebai.ui.changesView`（未存过 = 列表），切换时带**滚动位置**（滚动容器每次渲染重建，不复位等于把“我看到哪了”丢掉），折叠状态不持久化（刷新即展开）。提交框的动作**全在一行、右对齐**（`修补` + 历史信息在左，`推送` / `提交` 在右；按钮组必须在**最窄左栏**（180px，内容宽 164）里完整排下，“提交并推送”六字会把这一组撑到 185px、栏一拖窄主按钮就被顶出可视区，故按钮文字取“推送”、完整语义进 title）：`.fw-commit-opts` 与 `.fw-commit-btns` 各自不可折、不可压缩，栏宽不够时只在这两组**之间**折行（按钮永远同行同右缘，不会被拆开或顶出可视区）；修正了原先 `flex-wrap: wrap` 把两个按钮拆到两行且“提交”跑到左边的问题。状态文本（几个文件已暂存 / 未暂存将包含全部改动）改由按钮 title 承担：它本来就不该占这一行，而“N 个文件已暂存”在分组表头里本就有。**不再提供「署名」（`--signoff`）开关**：它是仓库/团队的固定习惯而非逐次决策，服务端仍支持该参数（API 未变），只是界面不再摆一个几乎不改的勾选框。**左栏宽度下限 = 提交框动作行的实测宽度**（`--fw-left-min`，面板每次渲染后回调上报；算术在 `files/panel-width.ts`，纯函数带单测）：拖动夹取与 CSS `min-width` 共用同一个值，两个提交按钮永远同行、不会被裁掉——写死的数字在改动文案或主题字体一变就静默失效，而症状只是“按钮被裁半个/挤成两行”。量的时候必须等节点进文档（面板渲染是先建树后 append，建树时量出来是 0），也不计入可省略项（它们本就能压到 0）。
  - 底部三栏从左到右是**一条动线**：点分支 → 看它的日志 → 点某条提交 → 右栏看这条提交改了什么。分支栏与标签栏的**单击即把日志切到这个引用**（当前范围显示在日志栏头部的范围选择器上，并在对应行上高亮；检出仍在右键菜单里），日志栏自己也能换范围。**「已检出」与「正在看它的日志」是两件事，视觉上也必须分开**：前者是仓库状态、靠行首 ✓ 图标（图标转强调色）表达，**不给行底色**；后者是视图状态、是列表里**唯一**带底色的行（浅底 + 左侧色块）。否则同一行既是“当前分支”又是“正在看”，两种含义糊在一起分不清；两个状态的文案也都写进 `aria-label`（视觉靠图标/底色，读屏只能靠文字）。日志栏**只放日志**（提交详情不再就地展开——否则「看列表」与「看内容」互相打断，返回后滚动位置也丢）；当前选中的提交在列表里高亮，栏标题右侧提示当前哈希。
  - **三栏头部各只有一行、且都不画底线**：分支栏头部 = 标题行小切换（分支/标签/储存/远程，**栏头不放「分支」标题**：切换组自己就说明了栏内是什么，标题只占宽度）；日志栏头部 = 标题 + 范围选择器 + 过滤输入 + 过滤芯片 + 刷新（旧实现搜索框独占一条过滤条）；提交内容栏头部 = 标题 + 选中提交的短哈希 + 「与工作区比较 / 整提交差异」+ 就地关闭按钮（后两者选中提交后才出现，详情内不再摆操作条；短哈希也只在选中时出现——**空态不摆「点击日志查看」这类引导文案**，空态不需要教用户怎么用）。头部行内控件统一 20px 高。头部底线全部去掉的依据是上面那条判据：头部底色 `--bg-elev` 与下面列表的 `--bg` 已经不同，再画线就是重复信息。**日志范围选择器**（IDEA 的 `Log: <branch>` 口径；分组清单由 `files/git-refs.ts` 给出）常驻日志栏头部：默认显示「全部分支」（= `git log --all`），点开是带搜索的引用浮层（范围 / 本地分支 / 远程分支 / 标签，行与图标复用 `.fw-ref-row` 口径），↑/↓ 移动高亮、Enter 选中、Esc 或点外关闭；已限定在某个引用上时按钮旁多一个 ×（一键回到全部分支）且边框转强调色。范围**不做成过滤芯片**——选择器已经显示它并自带清除，同一行里再重复一份只占宽度。
  - **过滤条件芯片只给「别处看不见的状态」**：文件路径 / 作者 / 时间范围三者的当前值分别来自资源管理器右键、日志行右键、日期菜单，头部没有它们的载体，所以摆成芯片并自带清除；**提交信息过滤不做芯片**——载体就是那个输入框（写着什么就是在过滤什么），回车即生效、输入框内自带 × 一键清除（清空即取消），再摆一个标签只占宽度（与范围选择器同理）。输入框在过滤生效时加 `filtering` 类亮边框，作为「条件已生效」的指示；清除按钮有内容才显形，并统一把「过滤条件」与「输入框内容」一起置空（两者不同步会残留一个看不见的过滤条件，日志少了却不知道为什么）。
  - **分支栏工具条只放动作图标，不放状态文本**（新建 + 检出 + 删除 + 抓取 + 拉取 + 同步 + 推送 + 刷新；「当前 master → origin/master」这类信息不显示——当前分支在状态栏的分支项、行内 ✓ 勾与高亮已有，重复展示只占宽度）。除刷新外全部左对齐，刷新贴右缘（与三栏头部的刷新同一视觉锚点）。工具条**不画下边线**（它下面就是本栏自己的列表，且两侧底色不同）。同样因为它 sticky，**背景必须垫实**：单层 `--bg-elev` 在 acrylic（0.82）下会让滚过去的行从底下透出来、与图标叠在一起——按本仓已有手法叠两层背景达 ~95%（`style-contract.test.ts` 有一条守卫：凡 `position: sticky` 的规则必须垫实或显式豁免）。**栏宽下限 = 工具条（按钮组 + 计数文本）的实测宽度**（不是写死的数字）：工具条每次重建后量一遍（只排除 `.fw-grow` 占位空白那一个弹性项），写入 `--git-col-a-min` 供 CSS `min-width` 与拖动夹取两处共用（算术在 `files/git-cols.ts`，纯函数带单测）——按钮随 tab 变、还会增减，写死的值在加按钮后静默失效（栏拖窄时按钮被裁掉半个，看不出是设计如此还是坏了）。**计数文本（「N 个标签 / N 条储存」）也计入**：它虽是唯一的可省略项，但只算按钮时栏能拖到「计数被省略成省略号、与按钮挤在一起」的宽度——现在的口径是**下限 ≥ 内容宽度**（+4px：2px 取整余量 + 2px 呼吸位），代价是这几个 tab 的栏不能比工具条更窄；计数文本的字号另降一号（10.5px），它不应与按钮抢视觉重心。检出 / 删除各弹一份本地分支清单菜单（检出勾选当前分支并置灰；删除排除当前分支——git 不允许删自己）；抓取 = `fetch --all --prune`（抓全部远程，与当前分支的上游无关，图标用云+箭头 fetch；单个远程的抓取在远程栏双击/右键）；同步 = 先拉取后推送（拉取失败即停不推，图标用 sync 双向箭头——拉取/推送各用 download/upload 单向箭头）；推送走 `pushBranch`（有上游按上游推、无上游择远程发布），领先数 ↑N 写进 tooltip；拉取/同步需有上游（无则置灰并在 tooltip 说明）；四个远程按钮包在 `.fw-remote-actions` 里，在途禁用与远程栏同一口径。**分支行只在当前分支前留 ✓**：其余行不再逐行摆一个相同的分支图标（列表本来就在「分支」栏里，重复的图标只是噪声），「我站在哪个分支上」才需要一行一个记号。
  - **远程栏的按钮同理只留高频**：抓取 / 拉取 / 推送 三个图标 + 「更多」菜单（仅快进的拉取 / 变基拉取 / 强制推送（`--force-with-lease`）/ 添加远程），共一行、**与分支栏一样左对齐**（只有刷新贴右缘；右对齐时按钮跟在一段空白后面，眼睛要在整栏宽里找它）；变体与低频管理动作本就不是每屏都要看到的东西，平铺七个带文字的按钮会在 200px 宽的栏里折成 169px 高的一堆，把远程列表挤到看不见。状态文本只留「别处说不了」的那一种：**未设置上游**（拉取/同步不可用、推送是首次发布），跟在按钮组后面；已设上游时上游名写进拉取按钮的 tooltip（与分支栏同一口径）——栏宽下限是工具条的实际内容宽，固定占一句“跟踪 origin/…”会把栏白白顶宽。**远程行名称与地址分两行**（`.fw-remote-main` 列容器）：两者都长（地址尤其），挤在一行时地址必被截成看不出是哪个平台的半截；地址**允许折行**（`overflow-wrap: anywhere`，不用省略号）——既然特意给它单起一行就是为了看得完整，截断就白分了，无空格长串不写任何断行规则也断不开。
  - **分支清单的「远程」靠 ref 命名空间判定，不看名字里有没有斜杠**（`%(refname)` 以 `refs/remotes/` 开头才算远程）：本地分支完全可以叫 `feature/alpha`，按斜杠判定会把它们整批归进远程分组（分组、排序、图标全错）。同理，日志行引用标签的颜色分类也要拿**已知的远程名首段**去比，不能用「有斜杠」当远程特征。
  - **日志刷新的「历史未变」判定必须连 refs 一起比**（`loadLog` 的 unchanged 分支）：打标签 / 建分支 / 切 HEAD 不产生新提交（hash 全同），但提交行上的分支/标签芯片已经变了——只比 hash 会把这些变化吞掉（刷新后标签不更新）。refs 变了就走重建路径，芯片随之刷新；hash+refs 全同时保留现有列表（不把翻了几页的位置拽回第一页）。
  - **分支右键菜单里的「推送」不能拼成 `git push <branch>`**：git 把无仓库参数时的第一个参数当成**仓库**（远程名或 URL），不是 refspec——`git push feat-x` 报 `'feat-x' does not appear to be a git repository`。必须显式给远程 + `本地:远程` refspec：有上游时按**上游名**推（上游可以叫别的名字，同名推会新建错分支）、无上游时择远程发布同名分支并 `--set-upstream`（多远程且无 origin 时弹菜单选，不替用户猜）。
  - 三栏与左栏宽度都可拖（分界条 hover 变强调色、双击复位；宽度记 localStorage）；左栏与底部工具窗可整体收起（`Ctrl+B` / `Alt+G`），编辑器随之 `layout()` 重排。
- **根提交的 A 侧 = 空树对象**（`EMPTY_TREE`，`4b825dc6…`）：端点里的 `<rev>^`，若 `<rev>` 是**根提交**（无父提交），`normalizeRev` 把它换成 git 空树——根提交相对什么变化不是「错误」而是「相对空树的全量新增」。不归一时 `git diff <root>^ <root>` 直接报 `ambiguous argument`（422），而提交详情把 A 侧与**文件清单**都挂在 `${hash}^` 上 → 跨文件导航的 `gitCompare` 默默 422 → 按钮凭空消失（用户以为「没有更多文件」）。归一只发生在「`X^` 存在且 X 无父」这一种情况：X 本身不合法时仍报 422（若也当空树，就是拿错数据当差分）。空树参与的比较不再走三点（`A...B` 需两个 commit，空树会报 `is a tree, not a commit`）——语义上空树就是共同祖先。
- **体量与失败：把「撑不住」变成看得见的事**（`GEBAI_FS_MAX_DIFF` 默认 8MB / `GEBAI_GIT_MAX_FILE` 默认 1M 字符）：
  - `trimDiffText` 在**文件边界**截断（不在 hunk 中间剪一刀，免得解析出半截 hunk 当真内容）；被截掉的文件用 `--name-status` + `--numstat` **补齐清单与真实 +N/-N**——「这次动了哪些文件」是提交详情的骨架信息，不该随逐行内容一起丢。
  - `contentAt` 先问 `git cat-file -s`（只读元数据）再决定要不要读 blob，超限直接返回 `tooLarge + size`，**不拉正文**；前端 `mountDiffView` 见到 `tooLarge` 就**不建 Monaco model**，改走服务端结构化 hunks + 一行说明（这正是「点开大文件卡死/空白」的源头）。
  - 降级一律带**原因提示条**（`.fw-hint-bar`）：降级本身不可怕，不说一声的降级才可怕（用户会把「没显示」理解成「没改动」）。两侧端点都不存在该文件时也不再给两片空白，而是说明「可能路径不对 / 被删除后未重建」。
  - `truncated` 只在「逐行内容确实被上限截掉」时标；纯重命名/纯改权限（本来就无内容差异）不标——否则「换了个名字」会显示成「大到看不了」。
- **失败不再静默**：`prepareReview`（取跨文件导航清单）原先 `catch {}`，失败与「只有一个文件」在界面上长得一模一样；现在失败给 warn toast（修正前的实测症状：根提交点文件时 422 被吞，导航按钮直接消失）。`spec.fallback` 也不再依赖调用点是否记得传：`fetchFallback` 拿不到就现取一份单文件差异（文件历史对话框那条路径此前就没传）。
- **变更文件遍历（上一个 / 下一个变更文件）**：导航按钮在**差异视图自己的头部**（`fw-viewer-bar` 内，`mountDiffHeaderNav`——两组：← → 换变更文件、↑ ↓ 换差异块，计数随滚动实时更新；降级渲染（结构化 hunks）同样有头部与文件组，差异块组依赖 Monaco 的 `nav`、降级时不给），不占标签栏——按钮描述的是「这块差异视图」的内容，跟着被导航的内容走，标签栏只承担标签生命周期。语义分两层——**文件内换差异 = `F7`/`Shift+F7`**（与 Monaco 的 diffReview 同义），**跨文件换变更文件 = `F8`/`Shift+F8`**（与合并视图的冲突导航同键，按当前视图分派，见 `files/main.ts` 的 `stepIssue()`）。文件清单按**与当前差异相同的端点对**取（工作区改动 / 某次提交 / 任意两端比较通吃），语义是**同一标签内换文件**（不给每个文件堆一个标签），换文件时给标签改 key、标题保留来源后缀，到头双向回卷。注意两个端点约定**不同**且不能混用：取内容用 `gitContent`（`WORKTREE`/`INDEX`/rev），取文件清单用 `gitCompare`（「未暂存」必须写成 from 留空 + `to=WORKTREE`，写成 `from=INDEX` 会被解释成 `--cached` 而静默返回另一批文件）。另：`F7` 被 Monaco 的 diffReview 占用、`Ctrl+Alt+↓/↑` 是它的多光标组合，故这些键一律**捕获阶段**监听，先于编辑器接管。
- **两种同窗形态（会话工作台 ⇄ 文件工作台）**：**会话侧**（标题栏**左侧**、会话列表按钮右边）入口主按钮 = **并列**（iframe 嵌工作台，缺省**停靠在左**、会话区在右，默认五五开、分界可拖双击复位；图标为**外框+中间竖线**——一眼就是"一窗分两栏"），它 **hover 时右方**弹出的副按钮 = **全屏**（文件工作台独占整个窗口、会话工作台收起但 DOM 与状态全留着；图标为**右箭头**——从会话这一侧往文件工作台去）。**文件侧**（工作台活动栏最下方，仅嵌入态）是同一对按钮的反向：主按钮 = **关闭文件工作台**（**左箭头**，恒回会话工作台），它 **hover 时右方**弹出的辅助按钮 = **进入分屏**（图标为分屏两栏；只有全屏态才给——并列态它就是"再进一次并列"，没有去处；窗口容不下并列时宿主经 `gebai:files-mode` 的 `canSplit` 告知，也不给）。消息流里的文件产物（文件卡 / 链接 / 原文件弹窗）走的是另一条路（新标签打开整个工作台页面，自动带上所属会话）。
  - **同窗三形态**（`SplitMode`：`off` / `split` / `solo`，归一在 `files-split-core.ts`）：会话侧主按钮管 `off ⇄ split`（窗口容不下并列时退化为 `off ⇄ solo`）、副按钮管 `off/split → solo`；文件侧主按钮恒为 `→ off`（关闭文件工作台）、辅助按钮管 `solo → split`。`Ctrl+\` 同一个键管三种（连按两次总能回到会话桌面），**这个键只标在会话侧主按钮上**：副按钮标它会变成“承诺一件它做不到的事”（`Ctrl+\` 在 off 态开的是并列，全屏没有快捷键）。**全屏态的关闭一律回会话工作台**（不再退回进入前的形态——想并列就点那颗“进入分屏”，一个动作一个出口），切形态**从不重载 iframe**（标签、滚动位置、Git 状态照旧）。全屏态下 `header` / `aside` / `main` 整块收起、面板吃满整个窗口（`body.files-split-solo`）、分界拖条隐藏。窄窗口（< 1100px）下会话侧主按钮就是全屏开关、副按钮收起：**分屏按钮不承诺做不到的事**（判据 `files-split-core.splitFitsWindow`，入口与动作同源）。
  - **分屏布局（停靠侧 + 宽度，列序随侧互换）**：`#app` 分屏时列变成 `1fr auto`（面板在右）/ `auto 1fr`（面板在左），面板**宽度由它自己的 `width: var(--files-split-w, 50vw)` 承担**、`auto` 列跟着它走；面板占第**整个窗口高度**（网格 `grid-row: 1 / -1`）——工作台自己是 IDE 式界面（自带顶栏与状态栏），头顶再压一条宿主标题栏只是白占编辑区的垂直高度；标题栏收在**会话区那一列上方**（面板在右 → `grid-column: 1`；面板在左 → `2`），不再横跨整宽。宽度放面板上而不是网格列里：这一个变量同时喂给 iframe（`.files-split-frame` 的 `width`）与主题太阳的定位，只留一处真相；`grid-template-columns` 里写死 var 也不合适（列宽要的是「跟着面板走」，插值在旧 WebView 上会静默退化成瞬变）。折叠/展开**不动宽度**（面板整体平移，见下一条），拖分界时 `auto` 列跟着面板即时跟上、不经过任何插值。
  - **停靠侧可换（会话与文件左右互换）**：`gebai.ui.filesSplitSide`（`left` 缺省 / `right`）持久化，工作台「更多」菜单里「分屏停靠改到左(右)侧」一项即刻换侧——宿主只改 `body[data-files-split-side]`，列序 / 标题栏列 / 分界线朝向 / 拖条贴哪条边 / 主题太阳以哪块为中心全由 CSS 按它分支，**iframe 不重载**（换侧不丢工作台的标签 / 滚动 / Git 状态）；分界线不再由面板边框画（否则 iframe 会被推离面板边缘 1px），只由分界拖条的 1px 线画。宽度换算与停靠侧归一化抽在 `files-split-core.ts`（纯函数 + `files-split-core.test.ts`）：左停靠取「指针 x − 面板左缘」、右停靠取「窗口宽 − 指针 x」。
  - **过渡是 200ms 的合成层动作（并列滑入 / 滑出，涉及全屏的切换淡入 / 淡出）**：`#files-split.anim` 只在过渡期间打开（`transition: transform 0.2s cubic-bezier(.22,.61,.36,1), opacity 0.2s ease` + `will-change: transform, opacity`），栅格宽度**一步到位**——整场过渡只重排一次会话，且那一次落在面板已经盖住（展开）/ 已经让出视线（收起）的时刻。**为什么不做宽度过渡**：面板宽度一变，`1fr` 那一列的会话区每帧都要重排**整段会话**（实测：289 条消息 ≈ 50ms/次、794 条 ≈ 141ms/次），220ms 的过渡因此只渲染得出两三帧——看着一跳一跳，消息越多越明显。**为什么并列滑、全屏淡**：一整屏的东西从窗口边缘横滑进来只是拖时间，而面板尺寸在 `split ↔ solo` 之间必然要跳变一次（宽度没法用位移表达）——跳变藏在**起步 / 收尾的透明**里（进全屏 `opacity 0 → 1` + 自停靠侧轻移 24px，出全屏反向；`solo → split` 借并列那套滑入：面板被推到窗外时尺寸已落到停靠位，跳变不在画面上）。**过渡期面板被提出栅格**（`body.files-mode-anim` + `data-files-anim-geo`，见 `holdPane`）：改成固定定位、几何与目标形态一致，于是在它下面切布局（会话区收或放）一点也带不动它——中途不闪底色也不错位；但面板一旦不在栅格里，分屏那列 `auto` 会塌成 0（会话区先铺满整窗、收尾再缩回，跳一下），故过渡期把那一列**显式按面板宽度留出来**（`.files-mode-anim.files-split #app` 的 `grid-template-columns`，与面板自己的 `width` 同值），**并把会话正文 `main` 显式钉在会话那一列**（面板在左 → 第 2 列，在右 → 第 1 列；与标题栏同列）——否则面板离栅格后自动排布会把正文翻到第 1 列去（实测 x 从 640 跳到 0、收尾再跳回），半透明面板下看得见。**布局何时落地**：并列两条路在起止两侧各自落（面板还盖着 / 已经让开），涉及全屏的那几条留到收尾 `commitLayout()` 才落——那时的会话区收放发生在面板已不透光（或已藏起）时，否则会在半透明面板下被看见。iframe 绝对定位、宽度与高度**都显式给**（`width: var(--files-split-w, 50vw)`、`height: 100%`）——宽度恒取变量、平移期间尺寸自始至终不变，工作台**内部不重排**（Monaco 不会被挤成 0 宽反复 relayout）。**高度为什么不能靠 `top: 0; bottom: 0` 夹**：iframe 是**替换元素**，绝对定位下上下夹 + `height: auto` 不会拉满，而是退回固有尺寸 300×**150**——实测症状：面板 720 高、工作台只剩顶上 150px 一条（状态栏被挤到 y≈140 与树重叠），下面全是面板底色。推开时先把起步态（内联 `transform`：并列推到窗外 `translateX(±100%)`、全屏只轻移 24px，全屏另带 `opacity: 0`）写上并强制一帧再撤掉——否则浏览器只看到「hidden → 显示 + 归位」一次状态变化，没有插值起点，过渡根本不发生。**写起步态时必须先把 `.anim` 摘掉**（写→强制一帧→加 `.anim`→撤值）：面板已可见时（如 `split ↔ solo`），带着过渡写起步态会**自己起一条随即被取消的过渡**，浏览器用 `elapsedTime ≈ 0` 的 transitionend 给它收尾——“过渡已结束”因此被当场判成事实，整段过渡一跳到位（实测症状就是硬切）。收尾由 `transitionend`（只认面板自身的位移：`propertyName === "transform"` 且 `e.target === 面板`——iframe 内部的过渡会冒泡上来；另加**时长守卫**）与兜底定时器共同保证（动画被换侧 / 连点打断时 `.anim` 不能永远留着，否则面板一直挂在合成层上）；拖动分界 / 双击复位 / 窗口缩放都没有过渡（`.anim` 不在场，宽度要跟手）；`prefers-reduced-motion: reduce` 下直接到位；窗口窄到 1100px 以下的自动退出不播动画。
  - **为什么把并列放主按钮**：主按钮是常驻可见的那个，该承担**最常做的动作**——一边让 Agent 改文件、一边自己核对，正是并列的存在理由；而"看一整屏文件"是少数情况，交给悬浮翻出的副按钮（IDEA 工具窗也是常驻优先、窗口化次之）。并列开着时主按钮带 `.icon-btn.active`（全站通用的"已开启"语义，轮盘按钮同款）——标题栏没有 ✕，开关态由按钮自己表达。
  - 关闭同窗形态**不是标题栏上的 ✕**，而是面板内与全局几条路径：工作台活动栏最下方那颗按钮（**关闭文件工作台**，左箭头，并列 / 全屏都是它）、它边上悬浮弹出的「进入分屏」、工作台「更多」菜单同名项、`Ctrl+\`（面板内），以及点标题栏的「会话列表」（并列时它本就被折叠，点它＝把列表拿回来，顺带关并列）；全屏下另有一条 `Esc` 兜底（走键位表的**浮层作用域**，见下）——面板万一没加载出来时手上得有一条退路。关闭图标是**固定的左箭头**（不随停靠侧翻转：它表达的是“回会话工作台”，与宿主那颗右箭头互为反向），与 `split`（进入分屏）、`swap`（左右互换）同住在 `files/ui.ts`；形态与 `canSplit` 由宿主经 `gebai:files-mode` 推给工作台。
  - 同窗用 **iframe 而非组件化**：工作台本就是独立入口（Monaco + Git + 完整状态模型），iframe 保住单实例、故障隔离与状态独立；代价是主题同步、**停靠侧同步**、**形态同步**与「关闭」这几个跨界动作要走 `postMessage` 桥接（`gebai:theme` / `gebai:files-split-side` / `gebai:files-mode`（含 `canSplit`）宿主→子页，`gebai:files-close-split` / `gebai:files-enter-split` / `gebai:files-split-swap` 子页→宿主；嵌入态下工作台的「返回歌白主界面」自动变为「关闭文件工作台」）。关闭同窗**不销毁 iframe**（IDE 里工具窗关掉再开也是原样）。宽度与停靠侧持久化，**形态同样持久化**（`gebai.ui.filesSplitOpen` 存 `split`/`solo`；旧版存的 `"1"` 在 `normalizeSplitMode` 里平移到 `split`）：刷新页面后自动把并列 / 全屏重新拉起来（延到空闲期，不让一个重工作台与主界面首屏数据抢带宽），用户主动关闭时清除记忆，而**因窗口过窄被自动退出时保留**（那是“窗口放不下”而非“不要分屏”）；记忆是 `split` 而窗口窄于 1100px 下限时**不恢复**（用户要的是并列，悄悄开出一整屏文件工作台是另一种意外）。
  - **面板是纯容器，没有标题栏**：只有分界拖条 + iframe。那条 30px 横条要花掉整个编辑区的垂直空间，而它的按钮各有去处——**关闭＝也不在标题栏**（走面板内与全局路径：活动栏最下方那颗、工作台「更多」菜单、`Ctrl+\`、全屏态的 `Esc`、点「会话列表」）；打开＝两侧入口的按钮（会话侧两颗 + 工作台活动栏那对）；重新加载／分屏停靠改到左(右)侧／关闭文件工作台＝工作台自己的「更多」菜单（页面级动作归页面自己）。副按钮只表达一件事：**全屏打开文件工作台**（再点一下就回到会话工作台）。全屏态的 `Esc` 兜底走**键位表的浮层作用域**（`pushEscScope` 而不是自挂 document 监听）：分发器按“后推的优先”匹配，所以全屏态下再打开文件预览 / 设置面板时，`Esc` 仍先归那一层——不会把「关预览」抢成「退出全屏」。
  - 悬浮副按钮有一条 6px 空隙，指针穿过时 `:hover` 会丢失——必须用透明「桥」+ 宽限延迟（`::after` 盖住空隙），否则按钮在真实指针下点不到；自动化里 `click()` 是瞬移，测不出来。宿主入口那对（`#files-solo-btn`）与工作台活动栏那对（`.fw-rail-split`）同一套做法。标题栏贴顶，提示气泡默认落在下方正好压住副按钮，故加了 `data-tip-pos="left"` 让该入口的提示贴左侧。
- **差异视图导航（定位差异是一等操作）**：工具条上一组「上一处 / n / m / 下一处」+ 快捷键 `F7`/`Shift+F7`（同效），并把**概览尺**在差异视图打开——右侧竖条标出全部差异位置，配按钮一眼看清「还剩几处、都在哪」。跳转时选中整块并居中；计数以**视口**判定（滚到哪就显示哪一块）。注意 Monaco 的 diff editor **自带** `F7` 绑定且会 `stopPropagation`，故快捷键以**捕获阶段**挂在 document 上先于 Monaco 接管（否则编辑器一获焦，`F7` 就变成 Monaco 自己的行为且与计数不同步）——语义一致，只是计数与按钮态由歌白统管。
- **主题与主界面共用（`theme-core.ts`）**：主题是**用户级偏好**，主界面（`/`）与工作台（`/files`）两个入口必须完全一致，因此把引擎（定义/解析/按需加载/持久化/生效）抽成**不依赖任何界面模块**的 `theme-core.ts`，`theme.ts` 只在其上叠加「🎨 主题面板」并 `export *` 转出引擎（既有调用点零改动）；两侧入口都调同一个 `initTheme()`。工作台**不再有自己的主题设置入口**（rail 上的主题按钮已删）——它有过的坑正是「只读了 localStorage 的 id，没应用人民币面额配色与默认主题黑白变体」，于是主界面选 50 元配色进工作台会跑成 100 元红。
- **顶层级内容区必须显式加入「内容层」（z-index ≥ 1）**：主题的背景装饰是 **fixed 伪元素**（浪潮的太阳 = `#app::after`、`z-index: 0`、视口居中），它们与面板同处一个堆叠上下文；两者 z-index 都是 0/auto 时，层序只由树的先后决定，而伪元素是 `#app` 的**最后**子节点 → 装饰会画到面板上面（"不透明背景"只决定像素怎么盖，不决定层序）。主题自己就是这套约定（`synthwave main { z-index: 1 }`）+ 面板 `#files-split { z-index: 1 }`。**以后再加顶层级内容区（新面板/弹层/分屏）都要跟这一步。**
  - 顺带：浪潮主题在分屏时把太阳改以**会话区中心**为心——面板在右 `left: calc((100vw - var(--files-split-w, 50vw)) / 2)`，面板在左 `calc((100vw + var(--files-split-w, 50vw)) / 2)`，并给它带上 `left` 过渡（`transition: left 0.22s ease`）——栅格一步到位后太阳自己滑到新中心、不会瞬移（收起时栅格在面板出画后才换，太阳随之稍滞后）；否则视口居中的太阳会被不透明面板切掉一半。
- **窄面板优先（分屏常在"半宽多一点"用）**：工作台要能在 ~600px 的面板里正常工作，为此有三条约定——① **活动栏右侧的包裹类必须叫 `.fw-main-right`**（`.fw-right` 是已废弃的"右侧 Git 面板"用过的名字，CSS 里残留过针对它的 ≤1080px 绝对定位规则，撞名会让整块包裹变成一条窄抽屉、编辑区被压成几十像素；那条死规则已删）；② **Git 三栏的堆叠断点取 700px 而非 1180px**——860px 面板下工具窗还有 ~815px，三栏（200＋弹性＋240）摆得下且好用，按 1180 堆叠反而把三栏拆成一列高塔（原规则还写错了属性：给 flex 容器设 `grid-template-columns`，从未生效）；③ **状态栏按优先级退让**——条目逐条打 `data-pri`（1 最要、3 最先隐），≤900px 隐编码/EOL/大小/内核，≤700px 再隐语言/行列；状态栏自身 `overflow:hidden` 兜底（它是窄面板下真正的横向溢出源：条目全 `nowrap` 且不肯收缩，能独自把文档撑出 159px 滚动）。状态栏**不列 stash 计数**：储存（stash）与暂存（index）是两件事，常驻一个数字会把两者混在一起；要看储存时在 Git 工具窗的「储存」栏（那里才是它的家）。
 - **状态栏条目的语义（四个容易看错/撑坏的地方）**：① **根名格（最左那格）的图标恒为文件夹**，不按“是不是 git 仓库”切成 git 图标——那一格的语义是“我现在在哪个目录下”，而 git 属性紧挨着就有专门表示（右侧分支格只在仓库下出现，已把“是不是仓库 / 在哪条分支”说清楚），在仓库根上挂 git 图标反而像“点它看 git”；图标与资源管理器头部那个根按钮（`.fw-root-btn` 的 `folderOpen`）同一张图。② **根名限宽 160px + 省略号**（`.fw-status-name`）：会话根的 `name` 就是**会话标题**（几十字），而这一格是 pri1（窄面板下也不隐），不设上限就会把它右侧所有条目顶出屏幕（状态栏的 `overflow:hidden` 只能兜住溢出、兜不住挤压）；完整路径在悬浮里。③ **内核格只在“与默认不同”时出现**——默认（Monaco + 该文件没挂语言服务器）**什么都不显示**：编辑器长什么样一眼就知道是 Monaco，常驻一个「Monaco」标签只是噪声；只有挂上了语言服务器（显示服务器名，如 `rust-analyzer`，悬浮给工程根）或 Monaco **真加载失败/超时**落到轻量降级模式（`warn` 色「轻量模式」）时才占位。这两种都是“不说话就会让人误判”的状态：前者说明这个文件有语义能力在支撑，后者说明高亮/符号等能力已受限。判断降级用 `monacoLoadFailed()` 而**不是** `!monacoReady()`：后者在“**还在**预热（那 1MB 脚本正在下）”时也为假，会在页面刚开时闪一个「轻量模式」（实测：只打开目录根、未打开任何文件时状态栏就挂着它，用户会以为 Monaco 坏了）——没加载完就不说话，只有确实失败才提示。**文件最后修改时间格（`stat.mtime`）已移除**：它只在“这份文件什么时候被改过”时有用，而那个问题页内有更直接的载体——Git 工具窗的提交时间（带提交者与 diff）与标签行的脏标记；常驻一个到秒的时间戳占了状态栏最宽的一格，却回答不了一个新问题（一眼能看出“文件是旧的”，但旧于哪一版得看 Git）。④ **分支格的变更计数用中文短词而不是符号**：`1冲突`（`--danger` 色 + 加粗，全格唯一红字）/ `2暂存` / `3改动` / `4新增`，`↑N`/`↓N` 是领先/落后远程几个提交；**只显示非零项**、顺序按严重度（冲突 → 已暂存 → 未暂存 → 未跟踪），悬浮把每项写成完整说法（词根与短词一致，另附分支/游离态与“工作区干净”）。**为何不用 `!` `+` `±` `?`**（初版就是这么写的）：`±` 字形就是 "+−" 叠在一起，与旁边的 `+`（已暂存）并排时靠字形分辨太费劲，语义也不贴切（“未暂存”不是“加减”）；而且符号得先去记一张表才知道什么意思（实测：使用者连问两次“怎么读”）。改中文短词多占约一倍宽度（状态栏实测：四项齐全时分支格 124px，窄面板 620px 下仍不溢出），换来“不用学、不会看错”。三个实测出来的历史缺陷：① **冲突数被漏掉**——服务端 `staged/unstaged` 对冲突行恒为 false（`!conflicted && …`），冲突只进 `conflicted`，而旧模板串只印前三个，于是**一个只有冲突的仓库显示成 `0± 0± 0?`**（看着像干净，实则 merge 卡在半途）；② **零值照印**（`0± 1± 0?` 这种“两个零夹一个 1”，占宽又要靠位置猜哪个是哪个）；③ 已暂存与未暂存**共用 `±`**（旧代码 `staged± unstaged±`，只能靠位置区分）。规则收在 `files/git-counts.ts`（纯函数 + 10 例单测）。
 - **资源管理器头部（单行 + 按需过滤）**：头部只有一行——根选择按钮占主宽（点击弹根清单），右侧只留**刷新 + 更多**两个图标。**不再有常驻的过滤输入框与面包屑**：目录过滤改为按需展开的输入行（`Ctrl+F` 或「更多」菜单入口，`Esc` 收起并清空），排序 / 显示隐藏文件 / 上传 / 折叠全部收进「更多」菜单（当前态用 `✓ ` 前缀标记）——高频动作留图标、低频动作进菜单，省下的三行纵向空间全给树。**新建文件/文件夹不入头部也不入「更多」**：它们是低频动作，而树右键菜单里本就有（选中在哪就在哪建，比头部按钮的动作目标更准），常驻两个「+」只是把头部挤窄。面包屑移除的理由：标签页与树的选中行已分别表达「在看什么文件」与「在哪个目录」，再摆一行面包屑是重复信息。**显示隐藏文件默认开启**（`GEBAI_FS_HIDDEN`，默认 `true`）：根清单（`/api/v1/roots` 的 `showHidden`）到达时宿主把它喂进树（`applyHiddenDefault`），用户一旦在菜单里手动切换过就不再被默认值覆盖——「默认」只管首次，否则「刷新根清单」会把用户刚做的选择弹回去；`/fs/list`、`/fs/tree` 的 `showHidden` 参数**只在显式给出时**覆盖服务端默认（`pickBoolMaybe` 的三态读取），「参数 || 配置」的旧写法会让配置为 `true` 时前端**关不掉**隐藏文件、菜单的勾选态也与实际相反。前端一侧同样得守三态（`files/api.ts` 的 `flag()`）：`true` → `1`、`false` → `0`、不给 → 不带参数——`req()` 会把 `false` 参数**整条丢掉**，直接传布尔值会让「关闭」在请求里退化成「没给」，又回到被默认值接管的原地。
 - **根清单菜单的分组与折叠**（`files/root-menu.ts`，纯函数可单测）：分组顺序 **项目（`proj`，标题就叫「项目」）→ 会话工作区 → 其它**，空分组不显示（未配置预置项目时不会留一个空标题）。**会话组默认只列 5 条**（会话名往往很长，全铺出来会把菜单拉成一长条），其余收进「更多会话（N）」子菜单（hover 展开）；**当前所在的会话永远进首屏**（否则「我在哪个会话」要靠猜），它挤掉首屏最后一个而不是额外插一条。
 - **根清单的项目注册表与模型同源**（`routes/fs-shared.ts` 的 `buildRootContext`）：`engine.workbenchProjects` 直接复用模型侧的项目清单（`{AGENT}_PROJECTS` 注册表 + 内置「歌白」）——工作台列出的项目即模型能用 `project` 参数寻址的项目；子Agent 的项目绑定（`{AGENT}_PROJECT` / `SubAgentDef.projectRoot`）是**执行细节**（子会话工作目录、路径解析基准、AGENTS.md 注入），**不作为工作台根列出**（项目区只出现真正的项目，绑定根落在项目内时从该项目树即可到达）。内置项目「歌白」在总Agent 系统提示词里以一行注记告知名称（`内置项目「歌白」：<路径>（project 参数可按名寻址）`）——子Agent 提示词的预置项目清单同样包含它，而**未装载子Agent 描述摘要不重复列它**（否则同一行会在清单里重复十几次）。**env 三层合并：进程全局（.env / 系统注入）∪ 会话内存态 ∪ 请求携带（`?env=` / `body.env`，浏览器本地 env）**，优先级从左到右递增。必须含进程层：项目注册表通常配在 `服务端 .env`，而模型侧（`EnvManager.resolve`）看得到——漏掉进程层就会出现「提示词里有项目、工作台根选择菜单里没有」。
- **地址栏即界面状态（`files/url-state.ts`）**：进目录/开文件同步到 `?root=&path=[&line=]`，于是**刷新回到原处**、**浏览器后退在目录间穿行**、链接可分享。写入策略：目录切换 `pushState`（后退能回上一个目录），同目录内开文件 `replaceState`（否则连开几个文件要按多次后退）；同期变更节流合并成一条历史。与 `deeplink.ts` 分工：那边**解析并落位**（含根推断规则），这边只负责**写回**与从既有参数恢复，共用同一套参数名，匹配逻辑不重复实现。
- **Git 公共构件（`files/git-shared.ts`）**：变更面板（左栏）与 Git 工具窗（底部）是**两个独立挂载点**，但写操作流程（toast 文案统一、成功后刷新状态 + 各自视图）与非仓库占位只该有一份——各写一份很容易出现「一边刷新了另一边没刷新」。`createOpRunner(hooks, after)` 由调用方给 `after`（各自渲染入口），保证写完成两边都刷新。
 - **窄屏与触摸（≤700px / coarse pointer）**：文件工作台在手机上从「四区并列」改为**单列 + 浮层**——① 左栏变抽屉（`.fw-left` `position: fixed` + `translateX(-105%)`，宽度 `min(300px, 82vw)`，配 `.fw-drawer-scrim` 遮罩，点遮罩/Esc/选中文件自动收起；分屏拖条隐藏）；② 底部 Git 工具窗变**整屏面板**（折叠时 `display:none`）；③ 拖宽/拖高改 **Pointer Events + `setPointerCapture`**（`mousedown` 路径保留，所在元素 `touch-action: none`——否则浏览器接管手势会发 `pointercancel`）；④ 目录树**长按=条目菜单**（`LONG_PRESS_MS` 520ms、位移 >10px 作废，按住期间临时关 `draggable`），菜单里的「移动到…」是触屏的移动入口（HTML5 DnD 在触屏不产生手势）；⑤ 命中目标按 `pointer: coarse` 放大（rail 44、树行 44、菜单项 40、标签关闭常显）。**抽屉开关与桌面「左栏收起」记忆解耦**（`leftPanelShown()`）：窄屏开合抽屉不写回桌面态。终端两版式的手机档写在各自己的 `terminal.css` / `terminal-pty.css`（构建后 CSS 顺序不保证，同特异性规则会互相覆盖）。
- **验证**：`core/fs/service.test.ts`（17 例：列举/自然序/越界与软链防护/编码回环/Range/ZIP 含系统 unzip 交叉验证/搜索/根解析）与 `core/git/service.test.ts`（14 例：任意两端对比含 mergeBase、`contentAt` 三端点、`fileDiff`、refs/status/log）；另经真实浏览器（Playwright）冒烟：树展开 / Monaco 打开与语法 token / 只读→编辑切换 / Git 六视图 / 比较视图 / 并列差异 / 端点选择器 / 提交↔提交 对比，控制台零错误。 终端（PTY）另经真实浏览器（Playwright）逐项验证：多标签新建/关闭（激活态唯一）、shell 原生提示符与回显、**真彩色**（`Write-Host -ForegroundColor` 的绿色）、中文输入与输出、`cd` 后状态持续、Backspace 行内编辑、`Ctrl+F` 搜索栏、`Ctrl+C` 中断 `ping -t` 并重建 shell 后继续可用、拖拽工具窗高度后 xterm 自动重排；单测：`core/exec/pty-session.test.ts`（19 例：协议与尺寸、UTF-8 跨块解码、订阅回放与 detach、缓冲上限、中断与世代号、收尾与空闲回收）。
- **端到端回归脚本（`scripts/e2e/`）**：三套 Playwright 脚本共 84 项断言——`files-e2e.mjs`（面板基础能力：初始尺寸与折行、配色两档、搜索高亮/开关/计数、键位、未读与 OSC 标题、粘贴与复制、设置菜单、关闭确认、dead 标签重启、刷新接管）、`files-tui.mjs`（独占模式：vi / less / watch / htop 的备用屏进出与主屏恢复、TUI 中改尺寸、鼠标上报归属、备用屏搜索高亮）、`files-edit.mjs`（行内编辑：光标移动与前删/后删、中段插入、宽字符与代理对、折行接缝、输入法整段提交、vim）。跑法 `bun run e2e:term[:tui|:edit|:all]`（需一份在跑的服务端；截图落 `/tmp/gebai-e2e`）；只作改完手动回归用，不进 `bun test`（需要真终端会话）。
- **客户端**：工作台页面自用薄客端 `files/api.ts`（**同源 fetch**，复用浏览器登录态；`session`/`env` 经查询参数传递，不引入 WS 通道），**不经 `@gebai/sdk`**——该页是独立入口；REST 契约与服务端同源，SDK 侧如需可后续薄封一层。
- **底部工具窗：Git 与终端同槽互斥**：底部工具窗（`.fw-git-dock`）承载两个视图——Git（分支 | 日志 | 提交内容）与终端，共用同一停靠位、同一高度变量（`--git-dock-h`）与同一拖拽条；两者**实例都保留**，切换只切显隐类（`.fw-dock-hidden`），因此 Git 的滚动位置与终端的滚动缓冲/会话都不会丢。可见性与当前视图持久化（`gebai.ui.dockVisible2` / `gebai.ui.dockView`，无记忆时默认收起）；入口＝活动栏终端/Git 按钮、``Ctrl+` ``（终端）/`Alt+G`（Git）、状态栏分支项。
- **终端面板（真 PTY + xterm）**：分两层——**PTY 通道**（Windows ConPTY / POSIX openpty，双平台）与**降级通道**（管道式持久 shell）。
  - **PTY 通道（首选）**：`core/exec/pty-driver.ts` + `core/exec/pty-session.ts` + `ws-handlers/terminal.ts`。伪控制台/伪终端只能经原生 API 创建，而运行时（Bun）无原生绑定（node-pty 系 N-API addon 在 Bun 下实测不可用：fd 静默无输出、resize EBADF；bun:ffi 直调 libc 拿不下控制终端且段错误风险高）：驱动是**一段自包含原生源码**，由**系统自带**编译器编译成临时小可执行（`$TMPDIR/gebai-pty/driver-<hash>[.exe]`，按平台+源码内容哈希命名、改内容自动换文件；启动后后台预热），**不引入任何第三方原生依赖、仓库不存编译产物**。双平台同协议（行分隔 JSON，stdin/stdout 双向，输出走 base64 避开协议混淆）：`open`（shell 命令行 / cwd / 尺寸）· `in` · `resize` · `close` ⇒ `ready` / `out` / `exit` / `error`。
    - **Windows**：C# + `csc.exe`（.NET Framework）→ ConPTY。三个**必须做对**的点（任一错都会让子进程绕过伪控制台、直接写宿主管道）：`lpValue` 直接传 HPCON 句柄本身（不是指向句柄的指针）、`STARTF_USESTDHANDLES` + 三个标准句柄置空、`bInheritHandles=false`。
    - **POSIX（Linux/macOS）**：C + `cc`/`gcc`/`clang` → `openpty`+`setsid`（哈希含平台前缀，多平台共享 /tmp 不串台；musl 下 openpty 内置于 libc，`-lutil` 重试无副作用）。四个**必须做对**的点：① `openpty` 的 `aslave` 必须传真实指针（glibc 无条件写入，NULL 直接段错误）；② 子进程 `setsid()` 后**重开** slave 路径（新会话 leader 首个 tty open 自动成为控制终端，Linux/BSD/macOS 通用，不用 TIOCSCTTY）；③ 子进程 tty 配足**交互式终端语义**（逐项置位，不依赖 openpty 默认值）——输出侧 `OPOST|ONLCR` 是硬要求（内核不把换行翻成回车+换行时，`ls`/日志/报错这类多行输出会逐行右移成阶梯、列全对不齐），输入侧 `ICRNL|IXON` + 行规程 `ISIG|ICANON|ECHO|IEXTEN`（缺则 `\x03` 不再转 SIGINT、行编辑与回显全失）；④ 启动期竞态：openpty 返回的父侧 slave fd 不能立即 close（bash 重开 slave 前的空窗内 poll master 会报伪 HUP/EIO，会话出生即死），须持有到首笔 master 数据或子进程退出再释放。
  - **PTY 通道（首选）**：`core/exec/pty-driver.ts` + `core/exec/pty-session.ts` + `ws-handlers/terminal.ts`。Windows 的伪控制台只能经原生 API（`CreatePseudoConsole` + `CreateProcess` 的 `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`）创建，而运行时（Bun）无原生绑定：驱动是**一段自包含 C#**，由**系统自带**的 .NET Framework 编译器（`csc.exe`）编译成临时 exe（`%TEMP%\gebai-pty\driver-<hash>.exe`，按源码内容哈希命名、改内容自动换文件；启动后后台预热），**不引入任何第三方原生依赖、仓库存不编译产物**。驱动与宿主用行分隔 JSON 协议（stdin/stdout 双向，输出走 base64 避开协议混淆）：`open`（shell 命令行 / cwd / 尺寸）· `in` · `resize` · `close` ⇒ `ready` / `out` / `exit` / `error`。三个**必须做对**的点（任一错都会让子进程绕过伪控制台、直接写宿主管道）：`lpValue` 直接传 HPCON 句柄本身（不是指向句柄的指针）、`STARTF_USESTDHANDLES` + 三个标准句柄置空、`bInheritHandles=false`。会话模型：服务端持有会话表 + 回放缓冲（订阅即回放，重连 / 刷新不丢屏），驱动世代号（`gen`）作废被杀进程的晚到回调（否则中断后会立刻被旧驱动的退出回调标成「已结束」）。
  - **通信通道**：WS（`/ws` 的 `term.open` / `term.attach` / `term.input` / `term.resize` / `term.interrupt` / `term.close` / `term.list`，推送 `term.out` / `term.exit` / `term.ready` / `term.error`）——终端是**字节级交互**（每次按键 / Tab 补全 / 方向键都要即时进 shell），250ms 轮询做不到。门禁与 REST 终端端点一致（文件工作台开关 / 终端开关 / 沙箱 / 只读）；输入按回车近似切分记入 `audit-fs.jsonl`（`term.exec`，`detail.mode="pty"`）。
  - **中断语义（`term.interrupt`）——分平台**：POSIX 写 `\x03` 进 PTY（termios 行规程转 SIGINT 给前台进程组），**shell 本体存活，cd/变量/历史全保留**，与真实终端 Ctrl+C 完全一致；Windows 只能终止进程树并以原 cwd / 尺寸重建 shell（会话 id、回放缓冲、订阅保留）——ConPTY 下向输入管道写 ETX 不会生成 CTRL_C_EVENT（实测 `ping -t` / `timeout /t` 均不停），代价是 shell 内部状态丢失。平台语义集中在 `PtyPlatformSemantics`（`detached`/`softInterrupt`，测试可注入固定）。
  - **前端渲染**：`files/terminal-pty.ts` 用 **xterm.js**（VSCode 同款内核）+ addon（fit / search / web-links）——真彩色、光标定位、清屏、行内编辑与 TUI 全屏程序都由它处理；键盘输入经 `onData` 原样回传（Tab 补全与方向键历史交给 shell），尺寸经 `FitAddon` 量出后回传。资源走 **vendor 静态伺服**（`/vendor/xterm/*.mjs`，稳定文件名、运行时 ESM 动态 `import()`，与 Monaco / mermaid 同一套约定）；为何不用 UMD 构建：xterm 6 的 UMD 包靠 `for (var s in exports)` 挂全局，而这批导出是不可枚举属性，全局拿不到 `Terminal`。
  - **建会话前先量尺寸（首屏尺寸）**：xterm 建出来是默认 80×24，面板尺寸要等它进入可见布局后才量得到（FitAddon）；先发 `term.open` 会让 PTY 以 80×24 起步、屏幕却按真实列数渲染——`COLUMNS/LINES` 错、长行折行错位、TUI 按 80×24 排版，而且只有窗口 resize 才偶然被纠正（建会话期的 resize 带的是前端占位 id，服务端不认识）。现在：先挂上并选中 → 等一帧 → `fit()` → 带真实 cols/rows 发 `term.open`；拿到真会话 id 后再补发一次尺寸，`term.ready` 亦补一次（对齐 open 往返期间拖面板 / 改窗口造成的偏差）。
  - **ConPTY 语义只在 Windows 开**：`windowsPty:{backend:"conpty"}` 影响 xterm 对包装行与光标重绘的处理，POSIX 上启用会与真实 tty 行为不一致；平台按**服务端给的 shell 路径**判定（终端跑在服务端，不在浏览器那台机器上），UA 兜底。
  - **配色**：背景 / 前景 / 光标 / 选区来自界面主题；**16 色 ANSI 调色板不来自主题**——内置 VSCode 默认的暗 / 亮两套（`files/term-theme.ts`，按背景亮度择一），亮色与常规色必须是两档**不同**取值（旧实现 `brightRed` 与 `red` 同值，`\e[91m` 与 `\e[31m` 的语义区别直接消失）；启用 VSCode 默认的**最小对比度 4.5**（对比不足时 xterm 向可读方向调整该色，实测暗底上的 fg-1 被调整到合规色）。`gebai:theme-change` 到达时重算并下发。
  - **查找**：`Ctrl+F` 开右上角查找框，三开关（`Aa` 区分大小写 / `ab` 全词 / `.*` 正则）与「第 n/m 项」计数与 VSCode 同序同义，开关状态持久化（`gebai.ui.termSearchOpts`）。两个坑都在实测里踩过：① **命中底色得我们自己补**——xterm 6 的 decoration 渲染器不读 addon 传的 `backgroundColor`（只按调用方的 `onRender` 设样式，addon 只给当前命中加了一道 1px outline），故底色走 CSS 变量落到 `.xterm-find-result-decoration`（当前命中按内联 outline 判定再加一档）；② **换开关不会自动重算**——addon 按关键词缓存判定，同一关键词下改选项不重建高亮与计数，所以切开关时先 `clearDecorations()` 再查。正则输入过程中必然出现半截式（`[`、`(`），查找调用一律包 catch → 状态显示「正则无效」，不再抛未捕获异常。
  - **键位**：单一来源 `files/term-keys.ts`（纯数据，单测直接跑 `keymap.validateKeymap`；焦点限定终端、捕获阶段）：`Ctrl+Shift+C` / `Ctrl+Insert` 复制，`Ctrl+Shift+V` / `Shift+Insert` 粘贴（中键粘贴同效）、`Ctrl+F` 查找、`Ctrl+=/-/0` 字号、`Ctrl+Shift+K` 清屏、`Ctrl+Shift+A` 全选、`Ctrl+Shift+`` 新建、`Alt+Shift+W` 关闭、`Ctrl+Shift+↑/↓` 切标签、`Ctrl+Shift+Home/End` 滚到顶 / 底、`Ctrl+C` 无选区则中断（有选区即复制）。三条**不能踩**的线：① **shell 的读行键一个不占**——`Ctrl+K`（readline 删至行尾）/`Ctrl+A`/`Ctrl+E`/`Ctrl+W`/`Ctrl+L`（readline 清屏）全部透传给 shell，xterm 会把它们编成 `^K`/`^A`… 发给程序（见 xterm `Keyboard.ts`），占了就是把用户的 shell 习惯改掉；② `Ctrl+Shift+<字母>` 在 xterm 里**不产生字节**（只有 `Ctrl+Shift+-/2/6` 有映射），是干净的空位——清屏/全选都用这一族；③ VSCode 的切标签键 `Ctrl+PageUp/PageDown` **不能抄**：那是 Chromium 的保留组合，页面收不到事件。另外 `Ctrl+滚轮` 接管为字号缩放（浏览器默认是整页缩放）。
  - **全屏程序（vi / less / watch / htop）**：备用屏由 xterm 处理，终端面板额外做四件事——① `windowsPty` 只在 Windows 开（ConPTY 的包装行/重绘语义对 POSIX 真实 tty 不成立）；② TUI 运行时拖面板/改窗口发 `resize` → 驱动 `TIOCSWINSZ` → 程序收 SIGWINCH 重排（实测 `watch 'stty size'` 逐次跟随）；③ **鼠标上报开启时（`term.modes.mouseTrackingMode !== "none"`）中键与右键都归程序**（vim `set mouse=a`、htop 之类），Shift+右键强制调出面板菜单、Shift+拖动仍可绕开上报选文本（xterm 自带规则）；④ 备用屏里的搜索命中高亮要**显式恢复显示**（xterm 6 的装饰渲染器对备用屏一律 `display:none`；实测位置与文字完全对齐，放开是对的）。
  - **浮层在终端上的 Esc**：菜单（右键 / 标签 / 设置）不搬焦点，所以它的 Esc 必须**含终端焦点且走捕获阶段**（`pushEscScope(..., { includeTerminal: true })`）——xterm 在自己的 textarea 上就 `preventDefault + stopPropagation` 把按键吃掉并向 shell 发 ESC，document 冒泡阶段的绑定根本收不到。修之前的表现有两层：菜单关不掉；同时那枚孤立 ESC 穿透给了 shell——readline 进入 ESC 前缀态，**后续括号粘贴被它吃掉**（回显成 `[200~`），vim 则直接退出插入模式。
  - **偏好**：字号与「跟随当前根」沿用降级实现的两个键（`gebai.ui.termFontSize` / `gebai.ui.termFollowRoot`，两版式共用一份字号），行高 / 光标样式 / 光标闪烁 / 选中即复制 / Ctrl+滚轮缩放收在 `gebai.ui.termPrefs`（`files/term-prefs.ts`，逐项容错解析：缺键回落默认值而不是最小字号）。设置入口是标题栏的轮盘 / 「设置」菜单。
  - **标签**：名字按「用户重命名 > shell 的 OSC 标题 > shell 名」取（`files/term-tabs.ts`），双击或右键重命名，tooltip 带出 shell / 工作目录 / 退出码；非活动标签有新输出点亮未读点，切回即消；进程结束的标签标 dead，按 Enter 就地重启（不用先关再建）。
  - **关闭确认**：有输出在跑的标签关闭前先问一次（与 VSCode `confirmOnKill` 同语义）；判据是**近似**的——最近 1.5s 内有输出（精确判定要看 PTY 里的前台进程组，需驱动侧上报，见已知边界）。
  - **剪贴板**：写剪贴板失败（非安全上下文 / 权限受限）回退 `execCommand("copy")`（隐藏 textarea）；读失败时给出可行路径（原生粘贴事件不经过权限接口，故提示 Ctrl+V / Shift+Insert / 右键）。
  - **降级通道**：驱动不可用（无编译器：Windows 缺 .NET Framework / POSIX 缺 cc·gcc·clang / 编译失败 / 其他平台）时 `/api/v1/terminal/info` 回 `pty:false` + 中文原因（含安装指引：Linux 装 `build-essential`、macOS 执行 `xcode-select --install`），前端门面（`files/terminal.ts`）落到**管道式持久 shell**（`core/exec/term-session.ts` + `routes/terminal.ts` 的 `/api/v1/terminal/{info,create,input,read,interrupt,close,list}`，REST + 250ms 增量轮询）：常驻 shell 子进程（Windows 默认 `pwsh.exe`，探测不到依次回落 `powershell.exe` → `cmd.exe`；POSIX 默认 `bash`），stdin 保持打开 → `cd`/`set` 在会话内生效；PowerShell 家族以 `-NoProfile -NoLogo` 启动（本通道无 TTY，profile 里的交互式假设会让 pwsh 刚起来就退出、命令一条都执行不到；PTY 通道是真控制台，profile 照常加载）；命令边界用**哨兵行**（`echo {TOKEN}%errorlevel%^|%CD%` / `echo "{TOKEN}$?|$PWD"` / PowerShell 等价物），服务端剥离后产出 `{退出码, cwd}`；命令回显与提示符由前端自绘（`files/terminal-legacy.ts` + `terminal-core.ts` 的 ANSI / `\r` / `\b` 解析与缓冲）。POSIX shell 清单：bash → zsh → fish → sh → dash（PATH 探测覆盖 Homebrew 等非系统路径），`$SHELL` 按 id 去重后补充（usrmerge 下 `/bin/bash` 与 `/usr/bin/bash` 同体，按路径比对会重复列出）；shell 以裸路径启动（isatty → 自动进交互模式）。
  - **会话跨刷新接管**：服务端会话与连接解耦（刷新/断线不销毁 shell，订阅即回放缓冲），前端把会话 id 记在 `gebai.ui.termSessions`（`files/term-sessions.ts`，含活动项与实现标识 `pty`/`legacy`）——开/关/切标签时写回，面板激活时先 `term.list` 对账再逐个 `term.attach`，已回收/服务重启造成的失效 id 丢弃，一个都没接管到才新建。不做这件事的后果：并发上限 8，而每刷新一次就新建一条，几轮刷新后新建就全是「已达上限」。接管入口与新建同构做了互斥（`activate()` 会被并发触发：启动阶段恢复工具窗状态 + 用户点活动栏/切视图），否则同一批会话会被 attach 两遍；写回时对 id 去重。降级实现（管道式）同一套规则（`/terminal/list` 对账 + `/terminal/read?since=0` 拉回缓冲）。
  - **安全与资源**：`GEBAI_TERMINAL` 总开关（默认 true）/ `GEBAI_TERMINAL_SHELL`；沙箱启用且非豁免用户 403（终端等同于任意命令执行）；`GEBAI_FS_WRITE=false` 拒绝执行；并发上限 8、空闲 30 分钟回收。
- **Git 面板细节（实现约定）**：① **日志刷新**——`doRefresh` 每次重置到第一页（否则提交后 / `F5` 之后日志停在旧历史），但首页与现有列表一致时不重建 DOM，翻页位置不丢；滚动到底自动续页（监听真正的滚动容器 `colLog`）。② **竞态**——日志加载用代际号 + 根比对判活，切根时旧根的响应被丢弃（早期 `logLoading` 早退会让新根首页整轮不加载）。③ **状态失败 ≠ 不是仓库**——`/git/status` 读失败进 `state.gitStatusError`，面板与状态栏显示可重试的故障态（不再摆出「初始化仓库」误导入口）。④ **在途反馈**——`createOpRunner` 通过 `onBusy` 通知标题栏显示「抓取远程…」等进度，并禁用远程动作按钮。⑤ **栏内状态**——分支/标签/暂存/远程四栏都有加载中 / 读取失败（含重试）与空态，重建内容时保留滚动位置。⑥ **范围与过滤**——日志范围 = 引用（`git log` 的 `ref`，空即 `--all`），由头部范围选择器或分支栏 / 标签栏单击设置，分组与搜索过滤在 `files/git-refs.ts`（纯逻辑、单测覆盖）；过滤支持按文件（资源管理器 / 变更面板 / 提交内容右键「在 Git 日志中筛选」）、按作者（日志行右键）与提交信息关键字，选择器与过滤条常驻不重建（保住输入与焦点）。按文件过滤时范围归零——看单文件历史时再被引用范围裁一刀，多数时候只剩空列表。⑦ **可达性**——列表行 `tabindex` + `role=button` + Enter/Space，分界条可聚焦并用 ←/→ 调宽（Pointer Capture + rAF 合并，窗口 resize 后重新夹宽度）。⑧ **附带补全**——blame 接线（编辑器右键菜单的「Git 历史」组开关，服务端端点与编辑器行装饰早已就位）、远程分支删除（`git push <remote> --delete`）、`push --tags`、覆盖已有标签显式传 `force`。⑨ **提交图（日志栏左侧泳道）**——布局在 `files/git-graph.ts`（纯逻辑、单测覆盖：车道分配 / 连线锚点 / 颜色槽位 / 路径几何），`files/git.ts` 只做像素换算与 SVG 落地。一条车道 = 「还欠着某个提交一条线」（记期待的提交 hash 与颜色槽位）：提交出现时它所在车道成为节点，**首个「后面还会出现」的父**接管该车道（继承颜色）、其余父另开车道，多分支汇入同一提交时在节点处合并；被消费的车道槽位可复用、尾部空槽裁掉——宽度不随历史长度增长。布局**逐行推导、前缀稳定**：日志是顺序追加分页的，「加载更多」不会让已显示的行重新排布。`complete`（= 已加载到末尾）决定「后面不会再出现的父」要不要留线——按路径过滤（`git log -- <path>`）时父链常断裂，否则线会挂满整个列表。渲染：行高**固定 44px**（`.fw-log-row.graph` 的 `height` 与 `ROW_H` 严格同值——它**不能再叠那 1px 行边框**：行边框会让行高成 45px，而 SVG 仍按 44 画，连起来每行漂 1px；行分隔线因而也一并去掉）、一行一个 SVG、图列宽度按全列表最大车道数统一（各行节点左右对齐）、车道多时按图列总宽上限 132px 等比收窄车道宽；节点色取固定 10 色板（**不随主题令牌**——绘图要靠多色区分并行分支，主题里没有这么多稳定语义色），合并提交画空心节点。
- **变更监听（自动刷新：前端长轮询 + 后端 `fs.watch`）**：工作台的目录树、变更面板、Git 装饰与已打开的干净文件都要「自己变」——用户在终端里跑 git、Agent 在另一个会话里改文件、别的编辑器保存，页面此前只能靠手动 `F5`。形态是**前端长轮询 + 后端事件**：前端循环请求 `GET /api/v1/fs/watch?root=&dirs=&rev=N&wait=20&git=1`，把「当前关心的根内目录」（根 + 已展开目录 + 打开文件的父目录，`WATCH_DIR_CAP = 64`）带上去；服务端只给这些目录挂 `fs.watch`，有变化立刻把在途请求唤醒（对前端等价于推送），没变化就挂到 `wait` 秒再回一个心跳。`rev` 不传 = 只取基线（立即返回），`wait=0` = 即时检查（兜底轮询用），`git=0` = 不监听 git 元数据。
  - **成本控制（本机制存在的主要理由）**：① **只监听请求点名的目录**，不做递归全仓监听（大仓的 inotify watch 数量会直接顶到上限）；② 同名目录在多个连接/多轮请求之间**共享同一个 watcher**（引用计数 + 45s 宽限期，前端长轮询循环之间不断流；宽限期到期由定时器回收，不依赖「下一次有人订阅」）；③ **事件合并**（120ms debounce）——一次保存/一次 git 操作往往触发一串事件，合并成一拍通知；④ 目录挂不上（ENOSPC/EMFILE/被删）**记失败并退避** 30s，不每轮重试；⑤ **无变化时零工作**：长轮询挂在 Promise 上，不占 CPU、不 spawn 任何 git 进程、不做任何列举；⑥ 变更路径只在 TTL（15s）内保留、超过 200 条降级为 `paths: null`（前端做一次「可见部分全刷」）；⑦ 在途长轮询超过 16 个时 server 端降级为即时检查（背压，不让连接/fd 被吃光）。前端一侧：后台标签页断开在途请求并停轮询（切回前台续），连续失败 3 次后退化为 20s 间隔的纯轮询（接口不可用/中间层掐长连接时页面仍会自己变）。实测空闲 45s：`watch` 3 次（每次挂满 20s）、`git/status` 1 次（仅启动）、`fs/list` 2 次（根 + 预取）——即「空闲时后端无事可做」。
  - **git 元数据单独监听**：仓库外部的 git 动作（内置终端里 `git commit`、切分支）**不改工作区文件**，工作区目录的 watch 完全看不到「历史变了但文件没变」。故另挂 `.git`（index / HEAD / ORIG_HEAD / MERGE_HEAD / packed-refs 等）与 `.git/refs`（**递归**：分支/远程引用各是一层子目录里的文件）——`.git` 也可能是**文件**（worktree/子模块的 `gitdir: …`），此时按它指到真目录。仓库根探测有 30s 缓存（`gitWatchDirs` 不必每轮 spawn 一次 git）。事件带 `git: true` 标记，前端据此只刷 Git 状态与工具窗、不刷目录树。
  - **前端落地**：`createFsWatcher`（`files/watch.ts`）持一条长轮询，目录集变化（展开/折叠）时 `poke()` 立刻断开重连把新目录带上；唤醒后按 400ms 窗口合并，然后 ① `explorer.syncDirs(paths)` 做目录树的增量同步（见「目录树自动刷新」）、② 命中已打开文件时重载该标签（保留滚动位置、不重建编辑器；刚由本页保存的文件 3s 内忽略回声）、③ 600ms（最长 3s）合并刷新一次 Git 状态与变更面板。目录清单归一、指纹、退避与「变化路径 → 目录」换算在 `files/watch-core.ts`（纯函数 + 单测，含**根目录的线上记号 `.`**：空串在逗号分隔里会被丢掉，不换记号的话根目录永远不会被监听——实测踩过）。服务端中枢在 `core/fs/watch.ts`（`FsWatchHub` + `gitWatchDirs`，带单测）；`GEBAI_FS_WATCH=false` 时端点返回 `enabled:false`，前端退化为纯轮询。
  - **自动刷新是静默的（无变化不重绘）**：刷新机制（变更监听、兜底轮询、Git 状态到达）只保证「数据是新鲜的」，**要不要重绘由数据指纹决定**——各面板渲染前先算一次「本面板渲染所依赖的全部输入」的指纹（`files/refresh-guard.ts`：通用 `fingerprint` + 变更清单 / 日志 / 通用清单三个域构造器，纯函数带单测），与上次渲染相同就直接返回。没有这层判据时，一次心跳也会把活动栏、变更列表、提交框、Git 工具窗整列重建一遍——滚动位置回跳、hover 消失、分组折叠态复位、正在输入的提交信息丢焦点与光标（输入法组合中的字直接断掉）。落地各处：变更面板（`panelKey`：状态/变更清单/视图/范围/分组与目录折叠态/编辑历史计划/写权限）与活动栏（视图/显隐/改动数/面板开关/嵌入态与停靠侧）走指纹短路；Git 工具窗的日志、分支、标签、储存、远程、标题栏、引用范围选择器与过滤芯片各自短路（**日志指纹必须含 refs**——打标签 / 建分支 / 切 HEAD 不产生新提交但芯片会变；**相对时间不进指纹**，否则永远判定为「变了」；远程名也进指纹，引用芯片的配色靠它判定）；目录树与 Git 装饰本就逐项比对（`sameEntries` / `dataset.deco`），状态栏与编辑器重载本就有签名与 etag 守卫。四处配套：① **提交框与变更列表容器改为常驻节点**（提交框从「每次渲染重建」改为只定点更新已暂存数 / 推送可用性 / 按钮文案），自动刷新永不触碰用户正在输入的节点；② 引用栏的「加载中…」只在**还没有清单时**出现（首屏 / 失败重试），有数据时的后台刷新是 stale-while-revalidate，不再每拍闪一次加载条；③ 磁盘上的文件被改而自动重载时，**只有当前活动标签**提示一句（后台标签静默重载——没人看见却弹提示，等于刷新在刷存在感）；④ 分组折叠与列表滚动位置跨重绘保留（刷新一下把用户折上的分组展开回去，与“无变化抖动”是同一类毛病）。
  - ⑩ **引用标签的语义色**（对齐 IDEA）：黄=当前分支头（加粗）/ 绿=本地分支 / 紫=远程 / 青=标签，第十个之后折叠为中性色「+N」（tooltip 列出被折叠的清单）——一个提交上挂十几条分支时全铺出来会把标题行挤成半行；判断远程靠**与已知的远程名首段同名**（本地分支也可以带 `/`，纯粹按「有没有斜杠」分会把 `feat/x` 误判成远程）；当前分支的提交行另加浅底。标签自身 `nowrap + flex:none`、标题行 `flex:none`：两者都不允许被压缩，否则标签多时标题会被挤成半行（字被裁一半）。⑪ **日志过滤三件套**——搜索框旁的 `.*`（开=扩展正则 `--extended-regexp`，**关=字面文本** `--fixed-strings`；不能只加前者，git 默认是 BRE、仍是正则，不加 `--fixed-strings` 时「关掉正则」对用户就是尞设）与 `Cc`（`-i`）开关、`日期` 菜单（今天 / 7 天 / 30 天 / 一年 / 自定义，直接把日期串交给 `--since`）以及原有的按文件 / 按作者 / 关键字；三者都进 `logQueryKey`，否则过滤变更会被「同一查询」短路当作普通刷新而不重建。**提交 ID 过滤**共用同一个输入框：输入 4~40 位十六进制哈希串回车即切换为**精确提交定位**（服务端 `git log <rev> --max-count=1`，不翻页、`hasMore=false`，其余 ref/all/grep 过滤不再叠加；路径过滤可叠加——该提交改过这个文件才命中），与提交信息关键字互斥；另有日志行右键「按此提交 ID 过滤」直接填入完整哈希。因 40 位哈希在输入框里看不全，提交 ID 过滤**生成芯片**（显示短哈希、tooltip 全哈希、点击即清除并清空输入框）——这是它与提交信息关键字（不生成芯片）唯一的展示差异；它同样进 `logQueryKey` 与空态判定（`filtered`）。⑫ **提交框的提交信息历史**（时钟按钮，取最近 30 条 subject 去重后点选填入）、**stash 条目的「查看内容差异」**（与 `stash@{n}^` 对比，直接复用任意两端比较视图）。
- **已知边界**：Office 预览非像素级一致（兜底下载打开）；超大文件只读片段并提示下载；无内置 hex 编辑；冲突合并为**文本级三窗格**（不做语义/结构化合并，二进制冲突只能选一侧或外部处理）；未做 Git 子模块详情、LFS；终端 `Ctrl+C` 语义分平台——POSIX 为真 SIGINT（shell 存活、状态保留），Windows 为「终止当前命令并以原 cwd 重建 shell」（ConPTY 不接受 ETX 字节，见上），shell 内部状态（`set` 变量、`cd` 后的位置）中断后回到会话创建时的 cwd；PTY 驱动依赖系统自带编译器（Windows：.NET Framework 的 csc.exe；POSIX：cc/gcc/clang），缺失时终端自动降级为管道式实现（无 TTY 语义，全屏交互式程序不可用；POSIX 装机指引：Linux `build-essential` / macOS `xcode-select --install`）；终端面板的「跟随当前根」只在切换根时补一条 `cd`，不跟踪 shell 内的 `cd`（无法从无提示符的字节流里可靠地读出 cwd）；不做 **shell integration**（命令装饰 / 滚动条命令标记 / cwd 自动跟踪 / 命令间跳转，需 shell 侧注入与 OSC 序列支持）、**拆分终端**、**标签拖拽重排**、**WebGL（GPU）渲染**与 **iTerm2 / Sixel 图片协议**；关闭确认的忙闲判据是近似的（按最近有无输出，而非 PTY 前台进程组——后者需驱动侧上报），所以 `sleep 30` 这类静默命令关标签不会拦。

- **状态栏「查看 / 编辑」格 = 模式 × 可写性两个维度，配色与文案必须分得开**（判据抽成纯函数 `files/status-core.ts` 的 `readOnlyReason` + `modeSlot`，带单测）：原先这一格只有一行三元表达式——`mode === 'edit'` 显示「编辑」，**其余一律** warn 橙色「只读」。于是打开一份普通源码文件（默认查看态、文件完全可写）时状态栏挂着橙标，第一反应是“这文件不能改”（正确读法是“现在是查看态，Ctrl+E 就能编辑”）；橙色也因此提前用光——等真正不可写时，它与「Git 状态不可用」「多步操作进行中」挤在同一个警告色里，不再有警示力。现在两维分开判：**可写**（`readOnlyReason` 返回 null）走中性色——查看态「查看」（title「本文件可编辑，Ctrl+E 进入编辑」）、编辑态「编辑」；**不可写**一律 warn 色，且按模式分两个方向、措辞对称——查看态「不可编辑」（改不了）、编辑态「不可保存」（存不下），title 里给出**是哪个原因 + 下一步动作**。不可写的三个原因按 `readonlyRoot > truncated > notEditable` 取首个：**根只读优先是刻意的**——它（`roots.writable === false`，即 `GEBAI_FS_WRITE=false`，`saveTab` 会直接拒绝）对**每一个**文件都成立，若被更局部的 `stat.editable === false` 抢先，用户会以为“换个文件就能写”，而实际要改的是服务端开关；同为文件级的截断（只加载前一段，写回会把文件截短）排在类型不可编辑之前。**「编辑态 + 不可写」是真会被走到的路径，必须显式覆盖**：标签栏那颗编辑按钮在不可写时是 disabled，但 `Ctrl+E`（`toggleActiveMode`）不看 `editable` 直接翻模式，截断的大文件按 Ctrl+E 即进编辑态——而编辑器本身仍是只读（`setReadOnly(mode !== 'edit' || truncated)`）、保存会被拒，此时若仍显示中性「编辑」，用户会一直以为改动存得下去。`stat.editable` / `roots.writable` 为 `undefined` 或 `null`（stat 或根清单未就绪）时按可写处理，不误报不可编辑——与标签栏编辑按钮（`renderTabActions` 的 `editable` 三段判据）、`saveTab` 的拒绝口径同一套输入。**pri 恒 1**：这一格回答“我现在能不能改这个文件”，与最左根名格同级，窄面板下不允许被隐掉（`data-pri` 的窄屏让位规则见上条）；状态快照 `statusSig` 相应补入 `stat.editable` 与 `truncated`，否则这两项变化时整条状态栏会被“无变化”判据跳过、徽标停在旧态。
  **收尾（同域后续一轮）：判据与文案收口到一处消费**——① 标签栏那颗编辑按钮与 `saveTab` 的拒绝提示都不再自判一份：按钮改走 `status-core.editButton()`、保存禁用态与 title 改走同一个 `readOnlyReason()`、拒绝 toast 取 `readOnlyWhy()`/`readOnlyNext()`。原先按钮在不可写时一律说「该文件类型不支持编辑」，而不可写有三个来源（根只读 / 截断 / 类型）——用户会去扩展名上找原因，而真正要做的是改服务端开关或下载后编辑；按钮与徽标说得不一样时，两个都不可信。② **编辑态下按钮恒可点**：`Ctrl+E` 不看可写性就能进编辑态，若按钮在不可写时恒 `disabled`，用户进去就**退不出来**（界面上唯一的“回查看”入口是灰的）；此时 title 仍明说“存不回”。③ 窄屏：`data-pri` 退让规则只点名 pri3/pri2，**pri1 恒在**（模式格与最左根名格同档），并由 `style-contract.test.ts` 扫 CSS 钉死「没有任何把 pri1 隐掉的规则」——模式格另带 `data-fw-mode-slot` 自查标记供 DOM 断言；同时打开文件时不再重复显示工作区级的「只读模式」格（根只读已由模式格说清，而状态栏 `overflow:hidden` 下后出现的这格会挤掉同一行的其它 pri1 格）。
  **三口径一致性回归（本域再一轮）：判据收成「一组输入 + 一个函数」，并用回归测试锁住**——上一轮已把三处改走向 `status-core`，但三处仍**各自拼输入**（按钮只判 `editable`、保存只看根 `rootsResp.writable`），于是同一组事实能得出两个结论；实测形态是同屏矛盾：截断的大文件与「类型不可编辑」文件上，徽标写 warn「不可编辑」、按钮是灰的、轮盘保存项也灰着写「存不回」，而 `Ctrl+S` 仍真写下去了——且写下去的是**坏数据**（截断文件回写会把磁盘上的文件截成已加载的那一段，非文本文件则以文本编码往返写坏）。收口方式：新增具名输入类型 `WritabilityInput`（`statEditable` / `truncated` / `rootWritable`）与**唯一取数口** `main.ts` 的 `writabilityOf(t)`，三处消费（`modeSlot` 徽标 / `editButton` 按钮 / 保存侧）都从它取输入；保存侧改走 `status-core.saveRefusal()`（逐因转调 `readOnlyReason`），轮盘保存项的禁用态与 title 改走 `saveItem()`，拒绝 toast 取 `saveRefusalText()`。**展示侧与保存侧唯一允许的分歧是「根可写性未知」**：徽标/按钮按可写算（否则每次冷启动都闪一个假「不可编辑」），而保存无从判断目标根只能拦住（放行后吃 403、或在只读站上真写一次再失败都更难懂）——该分歧由具名结果 `rootsPending` 显式表达，不靠“恰好都返回 null”。回归测试两层：`status-core.test.ts` 新增「三口径一致性」块（同一组输入 × 两个 mode 跑三处，断言「徽标 warn ⇔ 判为不可写 ⇔ 保存拒绝」同真同假、且三处文案同源；矩阵含你要求的三个基准形态：可写查看→中性「查看」+按钮可点、不可写查看→warn「不可编辑」+disabled+title 含原因、不可写编辑→warn「不可保存」），`style-contract.test.ts` 另扫源码钉住**接线**（三处确在调 `writabilityOf`/`saveRefusal`/`saveItem`，且 `saveTab` 不得再直接看 `rootsResp.writable`）——行为测试只能证明函数自洽，证明不了 main.ts 真的调了它们。
  - **`files/status-core.ts` 导出清单（单一来源核查点；本域作过具体值的接口都列在这里，改任何一项都要同步本节）**：类型 `StatusTone`（`"" | "warn"`）/ `ReadOnlyReason`（`readonlyRoot | truncated | notEditable`，顺序即优先级）/ `SaveRefusal`（`ReadOnlyReason | "rootsPending"`，`rootsPending` 仅保存侧有）/ `WritabilityInput`（`statEditable?` / `truncated?` / `rootWritable?: boolean | null`，**null/undefined 表示还没就绪**）/ `ModeSlot` / `EditButton` / `SaveItem`；函数 `readOnlyReason`（可写返回 null）、`modeSlot`（查看/编辑 × 可写/不可写 → 文案+配色+悬浮）、`editButton`（图标/禁用/按下态，**编辑态恒可点**）、`saveRefusal`（可保存返回 null）、`readOnlyWhy` / `readOnlyNext` / `saveRefusalWhy` / `saveRefusalText`（文案单一来源）、`saveItem`（轮盘保存项）。
    > 消歧（本域曾出现过的两个名字）：**`tabSlotState` 与 `readOnlyHint` 并不是本仓导出**（对两个名字做过仓库内代码与设计文档的全历史搜索——`git log -S` 与 `git grep` 于 `packages/` / `docs/` / `DESIGN.md`，命中为零；仅出现在 `.evolve/` 台账与会话记录这类工作记录里）——模式格渲染规格现在的名字是 **`modeSlot`**（早期才叫 `readOnlyHint`，随那一轮实现一起改名后再未出现）；`tabSlotState` 则从未在代码/文档里存在过。写代码/写文档时以上表为准。
  **接线契约的稳定性加固（本轮复核）：把「断言长相」换成「断言数据流」**——复核 `style-contract.test.ts` 的接线用例时用变异测试验了一轮（11 个「把界面改回旧口径」的变异体 × 9 个「只改写法不改判据」的良性重构），暴露出旧写法的两个毛病：① **名字出现过 ≠ 真的接了**——旧用例只查字面片断与 `writabilityOf` 出现次数 ≥ 4，于是「把编辑按钮改回只判 `t.stat.editable`」「把徽标改回只看 `t.truncated`」这些变异体**全部存活**（计数由保存那行凑够了），而界面已经退回三处各说各话的旧样子；② **好代码误杀**——把取数口提成局部变量（`const w2 = writabilityOf(t); saveRefusal(w2)`）是常规重构，却会被字面片断碰红。改法：断言逐**调用点**看**实参的数据流**——解析 `modeSlot` / `editButton` / `saveItem` / `readOnlyReason` / `saveRefusal` 的每一处调用，取对象实参里 `readOnly` / `refusal` 的**值**（不是整段实参，否则 `{ readOnly: null }` 这种真的另拼输入的写法会被放过去），要求它来自 `writabilityOf` 或由它派生的变量（闭包上限 4 轮，容许提取/两跳/局部改名）；另加两条形态无关的硬检查：**消费点不得自己现拼** `statEditable` / `truncated` / `rootWritable`（现拼＝又在自己拼输入），且 `saveTab` 不得再直接读 `rootsResp.writable`。改后重跑同一矩阵：**15/15 变异体被杀死、9/9 良性重构照旧绿**。另：**不做「位置敏感性」断言**（“某调用第一个实参必须是 `writabilityOf(t)`”）——实测那类正则会把局部变量提取类重构全判红；而「把取数口拆开单传字段」这种退化会撞上 `WritabilityInput` 这个**具名结构类型**，`bun run typecheck` 直接拦下，不必再拿脆弱正则去重。回归测试的运行口径也一并钉死：`bun test packages/web/src/files/` **从仓库根跑会缺 web 的测试预置环境**（`packages/web/bunfig.toml` 的 `[test] preload` 不生效，localStorage / DOM 相关用例成片误报——本单实测 37 fail + 5 errors）——正确跑法是 `bun run --cwd packages/web test src/files/`（或 `cd packages/web && bun test`），全量基线 **536 pass / 0 fail**。
- **判据矩阵（本域再一道闸门）：把「接线对不对」与「取值对不对」分开守**——接线契约（`style-contract.test.ts`）只能证明三处消费**调了**同一个取数口，证明不了「接上之后的每一格取值对不对」（比如给 `modeSlot` 加一支新分支，接线照旧、测试照绿）。故另建 **`files/status-matrix.test.ts`**：输入三轴（`statEditable` × `truncated` × `rootWritable`，含 `undefined`/`null` 两个「未知」哨兵）的**笛卡尔积 18 格 × 两个模式 = 36 格**，逐格断言**文案 / 配色 / 按钮 disabled / 拒绝文案**，并把「有哪些组合」显式写死——**缺行一眼可见**（行名自带三轴取值，与笛卡尔积对账；期望表是 `Record<判据枚举,…>`，加判据不同步加行则 `tsc` 报缺键）。实测：**34 用例 / 511 断言**；矩阵对 `status-core` 的**回归变异 21/21 全杀、良性重构 8/8 存活、类型化新分支被 `tsc` 拦下**；这组数已随变异台一并入库，**一条命令可复核**：`bun run mutate:status`（33 条变异体，约 20 秒，退出码 0 = 全符期望）。方法与教训（含「漏检 11 例」为何要改判数据流而非展示层接线断言、以及一道专治「运行期新分支」的字面量闭集机关与它已知的残余盲区）见「测试策略 → **变异验证**」。

### 日志系统
- 日志级别：`debug`、`info`、`warn`、`error`
- **最小日志器**（`@gebai/sdk/node` 的 `logger.ts`）：`log.debug/info/warn/error` 按 `GEBAI_LOG_LEVEL` 过滤（`debug` < `info` 默认 < `warn` < `error`；`setLogLevel` 由组合根在 `loadConfig` 后立即调用，早于任何装配日志）；另导出 `logEnabled(level)` 供调用点短路昂贵的参数构造。**只做级别过滤**，不做结构化日志、不引依赖——保持既有排障习惯（stdout/stderr 分流、宿主重定向到 `server.log.*`）。
- 日志走**标准输出/错误**（`log.*` 内部仍是 `console.log/warn/error`），无文件 sink、无轮转；二进制形态由宿主收集（如桌面侧车重定向到 `server.log.*`）
- **协议性输出不走日志器**：宿主/工具据以解析的行直接 `console.*`——启动就绪行 `[gebai] listening on http://…`（桌面启动器靠它取端口）、拉起器状态文件与 `server.log.*`、`dev-reload` 子进程 stderr 转发、启动期致命错误：它们不是「可调级别」的东西，被级别静默会让外部依赖方失效
- 日志脱敏：不记录密码、令牌、密钥明文，敏感字段以 `***` 替代（会话 env 回显经 `maskEnv` 掩码；任务级 env 回显为固定占位 `***`，与通知 `secret` 同惯例且可往返，见「统一任务管理 → 任务级环境变量」）；会话内容默认不落日志

### 数据生命周期

清理任务（`core/session/gc.ts`）随服务端启动执行一次，之后每日周期运行，失败不影响在线任务；可用 `GEBAI_GC_DISABLED=1` 关闭：

- **垃圾回收**：`trash/` 中删除/过期的会话超过保留期（默认 7 天）自动物理清理（按归档日期目录 `trash/YYYY-MM-DD/` 判定）
- **截断文件**：随会话 `tmp/` 整体清理——会话过期归档到 `trash/` 时一并归档，超保留期随 `trash/` 删除（不再单独设置截断文件保留期）
- **反馈数据**：`feedback/` 按日期分片，超过保留期（默认 180 天）自动清理，管理导出不受影响
- **临时文件**：会话 `tmp/` 在会话删除或会话过期（默认 90 天无活跃）时整体清理（含截断文件、附件、产物）
- **会话过期策略**：本地/服务模式均可配置会话闲置过期时间（默认 90 天，按 `chat.json` 最后修改时间判定），过期会话归档到 `trash/` 后可恢复，超保留期删除；**归档会话自助恢复通道**：`POST /api/v1/sessions/:id/restore`（WS `session.restore` 同权）——归属用户或 admin 可把 `trash/{date}/{id}` 目录整体移回分片存储位置（数据/tmp 附件/env 一并恢复），会话已存在返回 409，未归档/无权限统一 404（不泄露他人会话存在性）。**归档后确定性会话 id 的通道（飞书 chat_id → 会话 id）再来消息时以同 id 主动重建空会话**（存储层 `recreateSession`；回收站副本保留，恢复通道因会话已存在返回 409、不覆盖重建后的内容）
- 历史遗留的用户级 `truncated/` 目录（截断文件并入会话 `tmp/` 前的旧数据）由 GC 在**30 天宽限期后直接删除**（不迁移内容，宽限期防与旧版本进程写入冲突）

### 工具审批
- 全局工具：`sh`、`py` 默认需要审批；`read`/`write`/`edit` 默认无需审批（`file` 的 `delete` 动作**动态需审批**——递归且不可恢复，能力上甚于一次 `sh rm`）
- **免审白名单强制（`approval:false` 不再无条件生效）**：`sh` 的免审标记经服务端强制校验（`shApprovalFreeAllowed`）——只读命令（`validateShCommandSafeMode` 全量解析通过）或测试/静态检查/包管理器 test 子命令（`bun test`/`npm run <脚本名>`/`pytest`/`tsc`/`eslint`/`go test` 等，`run` 仅脚本名形态、禁文件直跑）放行免审，其余（含 `curl`/`wget`、命令替换、输出重定向、无法识别结构）一律仍走审批——防提示词注入诱导模型自行声明免审执行任意命令；**安全模式例外**：sh 在工具内按只读白名单降级（风险已由白名单约束），免审标记直接生效；**`py`/`js` 的 code 为任意代码**：`py` 免审标记不生效（恒需审批）；`js` 按词元扫描放行——纯数据加工/工具编排代码（无 `fetch`/`WebSocket`/`Bun.*`（除 `Bun.file`）/`process.env`/动态 import 等网络外发、进程、敏感读取通道）免审生效，含上述通道仍需审批（fail-closed，字符串误报只是多一次审批）
- 子Agent工具：通过 `requiresApproval` 声明（仅对**独有工具**生效——编码类子Agent 不再重复定义文件工具，全局 write/edit 维持默认免审批姿态；静态 `true` 会覆盖工具自身的函数形态声明，须保留动态判定的工具不要静态声明）；`task` 子Agent 的 `task_add`/`task_update`/`task_remove`/`task_run`/`task_cancel`/`task_files` 默认需要审批（任务 = 无人值守执行，**管理动作**逐次确认；执行期需审批工具在无人值守任务中自动通过，见「统一任务管理 → 执行免审批」）
^- `js`：**默认审批一次覆盖整个脚本含内部工具调用**（代码已经用户审阅；`approval:false` 免审运行时内部需审批工具在 RPC 分发层被拒——脚本体未经审阅不得免审执行审批工具）；**动态审批机制**：`Tool.requiresApproval` 支持函数形态 `(args, ctx) => boolean`，引擎在审批点解析（函数异常按需审批 fail-safe））；嵌套调用的 params 中自带 `approval: false` 不改变外层审批姿态（分发层按剥离免审标记后的姿态解析，防脚本内自我免审）
- 会话级跳过：`/approval-skip` 命令；**会话运行中开启即时生效**——引擎审批点实时判定（任务 env 快照或会话内存态 env 任一为 `true` 即跳过，前端开启时自动通过当前等待中的审批卡片，后续审批直接跳过；关闭需下次任务生效）；**用户本人可设置自己的会话**（`GEBAI_APPROVAL_SKIP` 写入会话内存态 env，不落盘——非管理员仍受路径/脚本/网络沙箱完整约束；ask 填值分支模型驱动通道服务模式下一律拒绝）
- 请求级跳过/收紧（REST `prompt`/`chat` body 的 `autoApprove` 布尔，任务级生效不持久化）：`true` 映射任务级 `approvalPolicy=auto`（需审批工具自动通过，含服务模式——调用方即用户本人，与会话级跳过同一授权面）；`false` 映射 `deny`（无交互通道下需审批工具直接拒绝，本地模式同样生效，不空等超时）；见「交互模式 → 审批策略按模式分级」
- 审批被拒/超时**只计数展示**（`event.approval.request` 的 `retries`），**不设次数上限**；模型连续重复调用由重复检测终止（`MAX_REPEAT_HITS=3`/`MAX_REPEAT_STALLS=2`）

### 拟人模式装配（`GEBAI_PERSONA`）

把「渠道收到的入站事件」变成为「人格分身跑一轮」的完整链路。

**默认关**。关闭时：`PersonaRuntime` / `Catalog` / `Resolver` / `Runner` **均不被实例化**
（无扫描、无文件读写、无副作用），`engine` 不加任何分支，渠道输入面接只记日志的
`LogOnlyDispatcher`——**既有行为逐字节不变**。

#### 三个 Port 的实现

| Port | 实现 | 位置 |
|---|---|---|
| `PersonaLookup` | `PersonaCatalog`（静态定义 + 动态 driver 合流） | `packages/persona/src/catalog.ts` |
| `SessionResolver` | `PersonaSessionResolver`（`(人格, 分身) → 常驻会话`） | `core/session/persona-sessions.ts` |
| `PersonaRunner` | `EnginePersonaRunner`（事件 → prompt → `engine.run`） | `core/session/persona-runner.ts` |

#### 人格定义：代码式，丢文件即注册

```ts
// packages/persona/src/personas/ 或 custom/persona/
export const persona: PersonaDef = {
  id: "xiaoshuang",              // 须与文件名一致
  name: "小霜",
  identity: "你是小霜，……",       // 原样进系统提示词，框架不改写
  role: "训练平台值班工程师",
  drivers: [
    { id: "support", focus: "答支持群", subscriptions: ["message"] },
    { id: "alerts",  focus: "盯告警",   subscriptions: ["webhook", "schedule"] },
  ],
}
```

四套机制与子 Agent / 事件源 / 渠道逐条对齐：双域扫描 · 目录签名热加载 · 单源失败隔离 ·
二开覆盖 · 严格字段校验。

**静态优先于动态**：`DriverRegistry` 里的运行期 driver（模型自建的分身）追加在静态之后，
**同 id 静态胜出**。否则模型可以靠「创建一个同名 driver」把关注面改成 `*`，
等于让人手写的部署配置被模型静默改写。

#### 会话映射：稳定又可扩展

映射写在 `SessionData.persona`（`{ personaId, driverId, boundAt }`，随 chat.json 持久化）：

| 要求 | 做法 |
|---|---|
| 稳定 | 会话自身即真相源：重启/迁移/导入导出不丢；删除会话映射随之消失，**没有悬空索引** |
| 幂等 | 同 `(personaId, driverId)` 永远返回同一 sessionId；并发调用只建一个 |
| 可恢复 | `warm(user)` 启动预热；memo 命中时仍校验会话存活（被删则重建，不返回死 id） |
| 可扩展 | 「一个 driver 多会话」只需改查找侧（取最早 `boundAt`），存储不动 |
| 可观测 | 会话名 `{人格名}·{分身id}`（如 `小霜·support`），侧栏一眼能认 |

为何不用外部索引表：外部表最难的从来不是写入，而是**一致性**——会话被删/被 GC 归档后，
记录就成了悬空指针，且没有任何机制保证它被清理。挂在会话上，「会话没了→映射自然没了」
是存储层的免费保证。

#### 引擎零改动：身份走既有 `channelNote`

`EnginePersonaRunner` 只调 `engine.run(sessionId, user, prompt, { channelNote, attachments, autoApprove })`
—— 全是**已有入参**。人格身份与出站纪律放进 `channelNote`（通道无关的注入注记）。

这比「给 engine 加 persona 入参」安全得多：后者会让所有会话的提示词构建路径都多一个分支。

`personaNote()` 必须写明三件（缺一条就会出真问题）：

| 内容 | 缺了会怎样 |
|---|---|
| 身份原文 + 关注面 | 同一人格的不同分身行为完全一样，分工白设 |
| **出站要走 `act`** | 回复写在正文里，**没有任何人转达**（后果最直接） |
| 不回复也是合法的 | 模型为「有产出」而发无效消息 |

#### 人格运行的审批姿态

`GEBAI_PERSONA_AUTO_APPROVE` 默认 `true`：人格跑在无交互通道（群里来消息→自己判断要不要回），
没有人在审批卡前等着。这与既有 `GEBAI_FEISHU_BOT_AUTO_APPROVE` 同一姿态（部署方整体担责）。
设为 `false` 时需审批工具**直接拒绝**（不空等超时）——人格会如实说「需要确认」而不是卡死。

#### 飞书用户凭据：进程级、自动检查、需要时横幅提示授权

人格要能回消息就得有**可用的飞书凭据**。一套完整的凭据流程：

```
启动检查 ─┬─ 有且有效/可刷新 ─▶ 安静跑
         └─ 缺/过期且刷不了 ─▶ 日志告警 + 前端横幅「去授权」
                                    │
                     用户点开链接 → 飞书授权 → 回调兑换
                                    │
                       写入令牌 → 发布事件 → 横幅自动消失
```

##### 为什么不能用 `feishu_docs` 的会话级令牌

| | 会话级（`feishu_docs`） | 进程级（拟人模式） |
|---|---|---|
| 归属 | 某个对话（`sessionId`） | 整个人格运行时 |
| 生命周期 | 跟随会话 | 跨会话、跨重启 |
| 缺了会怎样 | 该次文档操作失败 | **所有对外动作都做不了** |

人格是常驻的（「小霜一直在线答支持群」），凭据不能绑在某次对话上——
否则「用户开了个新对话，人格就失忆了」。

##### 落盘与刷新

- 位置：`{GEBAI_HOME}/persona/feishu-user-token.json`（0600，与 `drivers.json` 同级）
- 原子写（tmp + rename）：写一半断电会让文件损坏，下次启动就要用户重新授权
- **临期 5 分钟自动刷新**，刷新后**立即落盘**（否则重启后手上还是旧 token）
- **单飞**：多分身同时被唤醒时只发一次刷新请求（并发拿同一 `refresh_token` 去换会互相作废）
- 换应用后旧 token 失效（按 `appId` 比对）——否则会拿着别的应用的 token 去调，报错难懂

##### 授权流程（独立于会话级 OAuth）

| | 会话级 | 拟人模式 |
|---|---|---|
| 回调端点 | `/api/v1/oauth/feishu/callback` | `/api/v1/persona/feishu/callback` |
| pending 归属 | `sessionId` | 拟人运行时（无会话） |
| 触发 | 用户主动发起 | **启动时自动检查** |

申请的 scopes 只要收发消息相关（`im:message` / `im:message:send_as_bot` /
`im:message:readonly` / `im:chat:readonly` / `im:chat.members:read` / `offline_access`）——
**不申请云文档权限**。少要权限的好处是真实的：授权页勾选项越少，用户越愿意点同意。

`offline_access` 必须有：没有它就拿不到 `refresh_token`，2 小时后就要重新授权。

##### 授权提醒：相期卡片（主）+ 网页横幅（副）

授权不是「必须立刻处理、否则继续不了」的事：服务在**无凭据**状态下正常运行（只是人格不回消息），
授权往往在另一台设备（手机飞书）完成。所以不用模态弹窗，而用两个**常驻但不挡路**的通道：

| 通道 | 优点 | 局限 |
|---|---|---|
| **相期卡片**（主） | 桌面常驻窗口，不打开网页也看得到；带可点链接；授权完成自动消失 | 需相期在运行 |
| **网页横幅**（副） | 只要有浏览器就可见 | 需用户先打开歌白页面 |

两者都从同一个 `PersonaCredentialStatus` 读取，不各自判定——`authorizeUrl` 由状态惰性附带
（拿到即可直接渲染，少一次往返）。

##### 相期授权卡（`card_authorize`）

`PersonaCredentials` 的 `onChange` 回调驱动相期上的卡：

```
启动检查 ─┬─ ready ────▶ （有卡则撤）
         └─ 需授权 ──▶ 推 card_authorize（带链接，TTL 10 分钟）
                            │
              授权回调/手动粘贴完成 → onChange → dismiss(id) 撤销
```

为此相期新增三项能力：

| 能力 | 用途 |
|---|---|
| 选项带 `url` | 渲染为**链接按钮**（点击打开浏览器）——`card_ask` 的选项只能选一个，无法把人送出去 |
| `dismiss(id)` | **单卡撤回**——授权完成后只撤那张，不能拿「清全部」代替（那会抹掉别人还在等的事） |
| `expiresAtMs` | 卡片 TTL——链接过期后卡自动消失，不留一个点不开的按钮 |

`card_authorize` **不阻塞**：授权可能很久、也可能在手机上完成，没有理由干等。
完成后由调用方主动 `card_dismiss({id})`。

##### 前端横幅

`GET /api/v1/persona/status` 惰性附带 `authorizeUrl`。
三处刷新：页面加载 · 收到 `event.persona.credentials`（授权完成后立即生效）·
30s 轮询（覆盖「另一端完成了授权」）。

##### 人脸API

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/api/v1/persona/status` | 当前凭据状态（**不返回令牌明文**） |
| POST | `/api/v1/persona/authorize` | 重新生成授权链接 |
| POST | `/api/v1/persona/authorize/complete` | 手动提交 code（回调打不到时的兜底） |
| DELETE | `/api/v1/persona/authorize` | 撤销授权 |
| GET | `/api/v1/persona/feishu/callback` | 飞书回调（公开端点，state 一次性） |

##### 出站身份：`as_user`

飞书渠道的 `send`/`reply` 支持 `as_user: true`——**以用户本人名义**发（群里看到的是「你」在说话，
而不是机器人）。人格的 `personaNote` 明确要求它用 `as_user: true`。

授权不可用时**如实报错而不静默回落到机器人**：否则「人格以为用你的名义发了，实际是机器人」
会成为极难发现的行为偏差。

#### 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `GEBAI_PERSONA` | false | 拟人模式总开关（关闭时 persona 组件**完全实例化**） |
| `GEBAI_PERSONA_AUTO_APPROVE` | true | 人格运行自动通过审批 |
| `GEBAI_PERSONA_DRIVER_TOOLS_APPROVAL` | false | `driver_create`/`driver_retire` 是否需审批 |
| `GEBAI_EVENT_SOURCES_ENABLE` / `_DISABLE` | — | 事件源启停名单（先白后黑，同名机制同子 Agent） |
| `GEBAI_PERSONA_SCOPES` | 基础集 | 授权 scopes 追加（**并集**，非替换） |
| `GEBAI_CHANNELS_FEISHU_APP_ID` / `_APP_SECRET` | — | 拟人专用机器人凭据（**不回落** `GEBAI_FEISHU_*`） |
| `GEBAI_PUBLIC_URL` | — | 公网基址（拼授权回调地址；缺省 `http://localhost:{port}`） |

回调地址需在飞书开放平台「安全设置 → 重定向 URL」中登记：
`{GEBAI_PUBLIC_URL}/api/v1/persona/feishu/callback`

#### 多面通电：只接渠道是不够的

拟人模式启用后**同时**装配五样东西。只看其中一面就会得出错误结论：

| 面 | 组件 | 盖住什么 | 漏了它时的现象 |
|---|---|---|---|
| 入站（消息） | `ChannelInputBridge` → 渠道长连接 | 人群里推上来的消息 | 什么都不会发生 |
| 入站（驱动） | `EventSourceManager` | 定时 / webhook / 文件投递 / 桌面… | `kind:schedule|webhook` 的订阅**永远没有事件到达**（人格看起来「只对聊天有反应」） |
| 自我组织 | `driver_list/create/retire` 工具 | 模型给自己开/撤分工 | 新情况只能等人改配置重启 |
| **派活** | `worker_dispatch/list/cancel` 工具 | 耗时活派给独立会话并行做 | driver 自己查东西时，群里新消息全堵在队列里 |
| 运行态 | `GET /api/v1/persona/state` + 设置→拟人面板 | 「到底在不在干活」 | 只能翻日志拼凑 |

五面的失败**各自隔离**：事件源在渠道之后启动，一方坏不连带另一方（“飞书源配错导致定时任务也不跑了”
是最难排查的耦合）。

#### 派活：driver + worker（一个脑子带多只手）

```
事件 ──▶ driver 会话（串行，可被新事件插入）
              │  worker_dispatch（异步，立刻返回 runId）
              ├──▶ worker A（**独立会话**，隔离上下文）──┐
              └──▶ worker B ──────────────────────────┤
                                                        ▼
                       workerReportEvent（定向回到派它的那个 driver）
```

**worker 为什么不是 `subsession_run`**（两者都能隔离上下文，差别在「谁拥有它」）：

| | subsession | worker |
|---|---|---|
| 归属 | 挂在某个父任务的 `ToolContext` 里，随 run 生命周期 | 独立会话，不依附于 driver 这一轮 |
| 父轮结束 | 连带取消 | **继续干完**（它本该如此） |
| 退出后 | 存档留在父会话 | 会话删除（结论已进 driver 事件） |

四条实作约定（都是真实事故形态，见 `worker.test.ts`）：

1. **结论必带 `verify`**：没有验收标准的活回来是「我看了一下，大概是……」——工具直接拒绝。
2. **跑完无文本结论 → `failed`**，不能当 `done`：否则 driver 会以为「查完了，没发现问题」而漏报。
3. **异步派活的 runId 由工具预分配**：否则结论回来前 `worker_list` / `worker_cancel` 找不到它。
4. **叫停后不改回 done**：模型已经决定不要这个结果了，反过来标成功就是谎报。

#### 热加载：人格定义与事件源都改完即生效

两者**各自节流 5s**（目录不同，合并节流会让其中一个等另一个），挂在 runner 每轮之前——
不放定时器：空闲进程不该白跑目录遍历。

人格定义热加载必须同时做三件（缺一件就是「改了好像没生效」）：

1. `catalog.replace()` 整体替换——下一次读身份/订阅就是新的
2. **新增 driver → `syncDriver(online)`**，删掉的 → `offline`（不调则新分身从不响应）
3. 订阅/并发改过的 → `syncConcurrency`（调度器要知道新额度）

**重扫结果为空时不清空现有的人格**（目录暂不可读 ≠ 人格被删）：把全部人格下线比“配置暂不可读”严重得多。

#### 内置飞书事件源：入站增强，不是重复实现

与 `@gebai/channels` 的 feishu 渠道**分工而不是重复**：

| | 渠道（出站主路径） | 事件源 `feishu`（入站增强） |
|---|---|---|
| 收消息 | 应用身份长连接 | 应用身份长连接（**复用宿主连接，不自己建**） |
| 读群历史 | 只能读机器人所在群（且要模型显式 `observe`） | **用户身份回看，自动附在正文尾部** |
| 出站 | `act(verb:"send"/"reply")` | 不提供 |

本源的全部附加价值就一句话：**让模型看到「这条消息之前发生了什么」**。群里有人说「还是不行」，
只有一条正文时模型只能猜。

三条协议事实决定了它的形态：收消息只能应用身份、读历史必须用户身份、**同一应用只能一条长连接**。
所以本源经 `deps.feishuLongConn` 拿宿主已有连接，自建 = 互踢 = 两边都偶发收不到消息。
宿主侧撞车判定与读历史降级在 `core/session/persona-feishu-deps.ts`：

- 与会话机器人同 AppID 且机器人已启用 → **启动期拒绝注入并告警**（不是静默空转）
- 无用户令牌 → 退回应用身份，**把原因写进日志并标注在正文里**（“可能不全”），
  否则「这个群为什么没历史」永远查不出来——它看起来完全像「这个群就是没历史」

#### 事件的驱动面：`EventSourceManager`

插件只回答「发生了什么」（`IngressEvent`）；「归谁、进哪个会话、什么时候跑」全由 `PersonaRuntime` 裁决。
扫描域：内置 `packages/persona/src/sources/` + 二开 `custom/persona/`（同名二开胜出）——与子 Agent
同款的四套机制：目录签名热加载 · 单源失败隔离 · 二开覆盖 · 严格字段校验。

写新源只看 [`packages/persona/docs/event-sources.md`](../../packages/persona/docs/event-sources.md)，
不必碰任何核心代码。

#### 自我组织：`driver_*` 工具的身份从哪来

`driver_create` 是**写操作**（新增常驻分身 + 落盘 + 参与订阅匹配），所以身份不能由模型自报：

```
SessionData.persona  ──▶  personaOwnerOf(sessionId)  ──▶  { personaId, driverId }
（会话自身即真相源）            （服务端适配层）              （注入内核工具的 resolveContext）
```

| 情形 | 行为 |
|---|---|
| 人格会话（`SessionData.persona` 有值） | 身份取自会话归属；模型传的任何 personaId 都被忽略 |
| 普通会话 | 返回「当前会话不属于任何人格」+ 该去哪做，**不落任何写** |
| 会话不存在 / 读取抛错 | 同上（宁可少给能力，不给错身份） |
| `driver_create` 成功 | `onChange` → `runtime.syncDriver(online)`：不调它新建的分身**从不响应**（调度器没条目，工具却说「已创建」） |

工具是**全局注册 + 执行时自检**（而不是会话级注册/注销）：注册是进程级的，按会话注册要把
「会话删除 / 重建 / 人格热加载」三件事都维护一遍，多一处状态就多一处不一致。代价是普通会话
也看得见这三个工具——所以描述统一带「【仅人格会话可用】」前缀，并在拒绝时给出该怎么做。

默认不弹审批：创建/下线都**写盘留痕 + 可随时回滚**，风险由「可见 + 可回滚」承担；逐次审批会让
「自我组织」退化成「每步问人」。要收紧就设 `GEBAI_PERSONA_DRIVER_TOOLS_APPROVAL=true`。

#### 运行态：一个接口，五项事实

`GET /api/v1/persona/state` 把五个来源汇成一份快照（未启用时 404——**不返回全零的假快照**，
否则「没开」会被误读成「开了但什么都没发生」）：

| 快照字段 | 来自 | 单看它会漏掉什么 |
|---|---|---|
| `personas[].drivers[]` | `PersonaCatalog`（静态 + 动态 driver） | 定义在 ≠ 跑过 |
| `drivers[].queued/running/accepted/failed` | `PersonaRuntime.personaStates()` | 调度器惰性创建，没事件就没条目 |
| `drivers[].sessionId` / `sessionRunning` | `runtime.bindings()` + `engine.isRunning()` | 两者并列：调度计数会滞后，引擎判定更准 |
| `sources[]` / `sourceErrors` | `EventSourceManager.state()/loadErrors()` | 源未启动时计数全 0，看着像「没收到」 |
| `channelInput` | `ChannelInputBridge.allStats()` | `unrouted` 持续涨 = **订阅配错**，不是渠道坏了 |
| `workers[]` | 派活账本（`worker-tools.ts`） | 派了多少活、有几个还在跑（结论正文属 driver 会话） |
| `credentials` | `PersonaCredentials.status()` | 无凭据时人格不回消息而日志全绿（不含令牌明文） |

同形状直接给设置面板「拟人」页签用（凭据 → 事件源 → 渠道入站 → 人格与分身 四段，
与「排查时该先看哪个」的顺序一致）。

诊断用的手动启停：`POST /api/v1/persona/sources/{name}/{start|stop}`——临时打开一个源看它能不能
收到东西，不必改环境变量重启（那会中断正在跑的人格任务）；**不改变启停名单**。

### 对外动作：渠道抽象（`@gebai/channels`）

把「发消息 / 提工单 / 写记忆 / 落产物」收敛成**同一个动作面**，而非按渠道列工具。

**动机**：按渠道分化（`send_message`/`create_issue`/`write_memory`…）每加一个渠道就要加工具、
改提示词、影响全部会话；而它们的调用形状几乎一样，只是**目标**与**动词**不同。

#### 抽象：三个正交量

```
                     ┌─ 目标 target：`channel:address`（谁）
   一次对外动作 = act ┼─ 动词 verb：渠道声明的封闭集合（做什么）
                     └─ 载荷 args：由动词 schema 约束（什么内容）
```

**`submit_*` 不需要单独框架**：它就是 `verb:"submit"`。任何渠道声明了新动词，模型立刻能用
（「抽象是性质而不是名单」的直接结果）。

#### 三层职责

| 层 | 职责 |
|---|---|
| 渠道插件（丢文件即注册） | 只回答「怎么把它做掉」（`ChannelDef`） |
| 注册表 + 执行器（`packages/channels`） | 裁决「能不能做、要不要问人」 |
| 工具层（`core/tools/channels.ts`） | 暴露给模型：`act` / `observe` / `channels_list` |

#### 治理（能力外置、权力内守）

| 裁决项 | 在哪判 |
|---|---|
| 渠道存在 / 已启用 | 执行器 |
| 能力已声明 | 执行器（未声明 → 列出可用能力） |
| 必填参数齐备 | 执行器（缺 → 指出缺哪个） |
| 目标在白名单 | 执行器（**越界直接拒绝**，fail-closed） |
| 是否需要人批准 | 工具层（按 `writes` 声明） |
| 审批 / 留痕 / 审计 | 引擎 |

**白名单越界不弹审批**：发错群是信息泄露，不是「让用户决定」的事——让人去点一个
「是否允许发给无关群」的按钮，等于把泄露的决定权交给一次误点。所以越界既**执行期直接拒绝**，
又让 `requiresApproval` 返回 `false`（用户不会看到一张注定被拒的卡）。

**未配白名单时 `targets()` 返回 `[]` 而非 `null`**：`null` = 不做地址级限制，`[]` = 什么都不允许。
飞书渠道选后者——「没配＝不许发」的失败形态是「还没配好」，「没配＝随便发」是「把内部信息发错群」。

`ChannelContext` 里**没有** engine / store / registry：渠道不需要、也不该有运行任务
或读写会话的能力。

#### 扩展（与子 Agent / 事件源同款）

| 域 | 路径 |
|---|---|
| 内置 | `packages/channels/src/channels/` |
| 二开 | `custom/channels/`（同名覆盖内置） |

四套机制逐条对齐：目录签名热加载 · 单源失败隔离 · 二开覆盖 · 严格字段校验。
**声明面（`verbs`/`observes`）与实现面（`act`/`observe`）必须成对出现**——只有一边在加载期
直接拒绝（否则表现为「调用永远失败」而错误离配置很远）。

启停名单：`GEBAI_CHANNELS_ENABLE` / `GEBAI_CHANNELS_DISABLE`（先白后黑）。

#### 内置渠道

| 渠道 | 写（act） | 读（observe） |
|---|---|---|
| `local` | `append`（记忆分段追加）/ `replace` / `put`（产物落盘） | `all` / `sections` / `section` / `list` / `get` |
| `feishu` | `send` / `reply` | `messages` / `members` |

飞书渠道需 `GEBAI_CHANNELS_FEISHU_APP_ID` / `_APP_SECRET`（**不回落** `GEBAI_FEISHU_*`），
且**必须**配 `GEBAI_CHANNELS_FEISHU_CHATS`（群白名单）——未配时不允许任何目标。

#### 输入面：同一渠道同时声明收发

输出面是**拉**（模型主动调 `act`/`observe`，有返回值、即调即止）；输入面是**推**
（外部世界主动发生、无同步返回、常驻）。两者方向相反，不能混成一个方法，
但**属于同一个渠道**——所以在 `ChannelDef` 上并列声明：

```
verbs?    → act(input, ctx)            写（拉）
observes? → observe(input, ctx)        读（拉）
listens?  → startListen(sink, ctx)     收（推）
```

`stopListen(ctx)` 带上下文：同一份渠道定义可能被多个注册表实例使用（并行测试、
多租户、多用户），实现方按 `(ctx.home, this)` 登记清理器，停止时按同一键取出。

#### 输入面三段职责（必须分开）

| 段 | 归谁 | 混在一起会怎样 |
|---|---|---|
| 「外面发生了什么」 | 渠道（只推 `IngressEvent`） | 每个源重实现幂等与路由，必然分叉 |
| 幂等键 / 渠道标识 / 类别归一 | 桥接（`bridge.ts`，唯一实现） | 同一条消息在不同源里判重规则不同 |
| 路由到哪个人格、进哪个会话 | 调度内核 | 渠道被迫知道人格与会话，插件拿到越权能力 |

桥接层做三件**只有一处**该做的事：

| 职责 | 规则 |
|---|---|
| 幂等键 | 渠道显式 `id` 优先 → `{channel}:{kind}:{内容哈希}` 兜底 |
| 渠道标识 | 用注册表给的名字（**硬写**，渠道伪造无效） |
| 事件类别 | 单条 `kind` → 渠道 `defaultKind` → `message` |

`EmitResult` 五态让渠道能看见内核的裁决：`accepted` / `unrouted`（无人订阅，
不误吞也不自行处理）/ `duplicate`（幂等丢弃）/ `overflow`（队列满）/ `unavailable`。

`PersonaDispatchPort` 只声明一个 `dispatch` 的结构形态，刻意**不 import `@gebai/persona`**
——channels 是能力层，不该反向依赖调度内核（依赖方向反了，「内核可独立单测」就不存在）。

#### 飞书渠道：**独立机器人**，不复用会话机器人

| | `FeishuBot`（会话机器人） | `feishu` 渠道 |
|---|---|---|
| 用途 | 人与歌白对话 | 人格对外的收发 |
| 凭据 | `GEBAI_FEISHU_APP_ID` | `GEBAI_CHANNELS_FEISHU_APP_ID`（**不回落**） |
| 默认 | `GEBAI_FEISHU_BOT_ENABLED` = false | 有凭据即启用 |

飞书对**同一应用**只允许一条长连接。两个机器人是不同 AppID 时互不影响；
若误配成同一个 AppID，`start()` 会**直接拒绝启动**并说明——否则表现是
「其中一边偶发收不到消息」，极难排查。

长连接实现经 `deps.feishuLongConn` 注入（工厂形态），channels 包不 import
`@gebai/server` 的 `FeishuConn`（保持零依赖可单测）。拿不到注入时**如实报错**，
不静默降级成「配了但收不到」——但写/读面照常起（降级而非全损）。

入站治理：机器人自己发的消息忽略（`sender_type=app`，否则会自触发循环）、
非白名单群忽略、非消息事件不归本渠道（卡片回调留给会话机器人）、
非文本消息降级为可读说明（不静默丢弃）、缺 `message_id`/`chat_id` 不投（幂等键与
出站目标都靠它）。

#### 与 `feishu_group` 子 Agent 的分工

| | `feishu_group` 子 Agent | `feishu` 渠道 |
|---|---|---|
| 形态 | 装载后才有的工具 | 常驻动作面（`act`） |
| 何时可见 | 装载时才进上下文 | 始终可调（`channels_list` 可见） |
| 适用 | 一次性运维（建群/拉人/解散） | 持续性收发与观测 |

### 工具选择

工具集支持**全局开关控制**，可按需启用/禁用任意工具（含子Agent 工具），多级配置：

| 层级 | 配置 | 说明 |
|------|------|------|
| 全局 | `GEBAI_TOOL_ENABLE` / `GEBAI_TOOL_DISABLE`（逗号分隔） | 进程启动时生效，所有用户/会话共享（可经 `GEBAI_PROFILE` 领域档案声明） |

- **白名单优先**：`GEBAI_TOOL_ENABLE` 声明后仅启用列表内工具；`GEBAI_TOOL_DISABLE` 排除指定工具；两者同时配置时先白名单后黑名单
- **粒度**：全局工具按名称（`sh`/`read`/`edit`…）；子Agent 工具按 `{agent_name}_{tool_name}` 精确控制，也可按 `{agent_name}_*` 整包禁用（白名单同样支持该通配）
- **策略在注册时判定**（`ToolRegistry.setPolicy`）：启动声明的启停名单是**注册基线**——子Agent 在运行期装载/热加载进来的工具同样受约束，白名单不因注册时序失效；运行期 `setEnabled`（REST/WS）在其上叠加，重启回到裁剪后的能力面（见「启动裁剪与领域专用模式」）
- **禁用效果**：禁用后工具从总Agent schema 中移除，模型不可见、不可调用；已装载子Agent 中被禁用的工具同样不注入
- **管理入口**：经 REST `GET/PATCH /api/v1/tools`（SDK `listTools`/`setToolEnabled`）——**前端暂无图形化开关面板**（设置面板注明「工具启停经 API 使用」）；保存后新任务生效
- **用途**：安全收紧（如生产环境禁用 `sh`/`py`）、裁剪上下文、按业务场景定制能力面
- **权限**：REST `PATCH /api/v1/tools` 与 WS 工具启停为**服务端全局状态**（所有用户/会话共享），服务模式下仅管理员可操作（普通用户返回 403）；环境变量级（`GEBAI_TOOL_ENABLE`/`GEBAI_TOOL_DISABLE`）由部署方配置不受此限

### 交互模式

引擎按接入通道区分三种**交互模式**（`engine.run` 的 `interactionMode` 参数，任务级生效），工具通过 `Tool.interaction` 声明**最低可用模式**（缺省 `none` 即全模式可用）；声明高于当前模式时工具被自动禁用（schema 过滤 + 执行阻止，模型收到「当前通道不可用」说明并改用其他方式），替代旧的「等超时」体验：

| 模式 | 语义 | 通道 | 可用工具 |
|------|------|------|----------|
| `none`（**无交互**） | 单次请求：一次调用执行完返回结果，无前端、无往返；**本地模式需审批工具自动通过**（无人可询问）；**服务模式需审批工具直接拒绝**（防普通用户经 REST 免审批执行敏感工具；**无人值守任务执行例外**——任务经用户审批创建，以 `autoApprove=true` 运行，见「统一任务管理 → 执行免审批」） | REST `POST /sessions/:id/prompt` | 仅 `none` 声明（文件/网络/脚本等） |
| `multi_turn`（**多轮交互**） | 多轮请求-响应往返（非流式），有往返但无前端页面；**仅关键操作（requiresApproval）询问用户**（如飞书审批回调卡片），非关键操作自动 | 飞书机器人 | `none` + `multi_turn` 声明（ask 选择/计划分支走飞书选择卡片；show 图表分支走飞书后端渲染） |
| `realtime`（**实时交互**） | 实时流式交互：完整前端，关键操作询问用户 | WebSocket（Web UI，默认） | 全部 |

- **工具声明**（`interaction: "realtime"` 仅实时前端）：`page_capture`（依赖前端页面配合）；`interaction: "multi_turn"`（至少多轮交互）：无（原 ask_user/plan 的飞书适配由 ask 分支承接）；其余工具缺省 `none`；**合并型工具不做工具级声明**——`show`（图表/HTML/文件三分支）与 `ask`（选择/填值/计划三分支）全模式可见，按 `ctx.interactionMode` 在分支内校验通道能力（show：html 分支仅 realtime、图表分支 none 下引导 `render=backend`；ask：填值分支仅 realtime、选择/计划分支 none 下报「无交互能力」），见「内容展示」/「用户询问」
- **审批策略按模式分级**：无交互模式 `isApprovalSkipped`——**本地模式恒真**（`sh`/`py`/`write`/`edit` 等需审批工具**自动通过**，任务不会卡在审批等待）；**服务模式返回拒绝**（需审批工具在审批点直接拒绝执行，返回「需审批但当前通道无交互」说明，不进入等待——REST 无人可审批，普通用户不得借此免审批执行 shell/任务；管理员可经正式通道设置 `GEBAI_APPROVAL_SKIP` 后执行；**无人值守任务执行例外**：调度器触发时以 `autoApprove=true` 运行——任务经用户审批创建，执行即其授权的无人值守落地，服务模式同样自动通过，见「统一任务管理 → 执行免审批」）；**请求级 `autoApprove` 显式覆盖**（REST `prompt`/`chat` body 布尔字段，映射引擎任务级 `approvalPolicy`）：`true` = 需审批工具自动通过（**含服务模式**——调用方即用户本人，等价其自设 `GEBAI_APPROVAL_SKIP` 会话 env，模型驱动的 ask 填值通道仍拒绝该键，防提示词注入）；`false` = 无交互通道下需审批工具直接拒绝（**本地模式同样生效**——单次调用无人可审批，不空等 5 分钟超时）；缺省 = 通道默认姿态（本地自动/服务拒绝）；`sh`/`py` 的 `approval:false` 按次免审**只作用于交互审批**，服务模式无交互通道按剥离免审标记后的默认审批姿态照常拒绝（引擎 `stripApprovalFlags` 递归删键解析，防模型自行声明免审绕过硬门槛）；多轮交互模式关键操作（requiresApproval）经审批卡片询问用户（飞书审批交互卡片，见「飞书机器人集成 → 审批交互卡片」），非关键操作不打扰；实时交互模式维持询问用户（前端审批卡片）
- **飞书通道** = `interactionMode: "multi_turn"`：realtime 声明的工具（`page_capture`）自动禁用（原 `FEISHU_DISABLED_TOOLS` 名单已移除，由声明统一驱动）；`ask`/`show` 不做工具级禁用（ask 选择/计划分支经飞书选择卡片作答、填值分支明确报错；show html 分支明确报错、图表分支经飞书后端渲染出图），关键操作经审批卡片询问
- **REST 通道** = `interactionMode: "none"`：实时前端工具自动禁用，不再等待至超时；`ask`/`show` 不做工具级禁用（ask 选择/计划分支报「无交互能力」、填值分支引导设置面板；show html 分支明确报错、图表分支直接引导 `render=backend`，均不空等超时）；本地模式需审批工具自动通过，**服务模式需审批工具直接拒绝**（防免审批执行；**无人值守任务执行例外**：调度器触发时以 `autoApprove=true` 运行，任务经用户审批创建即其授权，见「统一任务管理 → 执行免审批」）；审批姿态可经请求级 `autoApprove` 显式覆盖（见上方「审批策略按模式分级」）；需要完整交互能力请走 WS 通道
- **定时/普通/闲时任务执行**（调度器触发的 prompt 型）按**无人值守与否**分流（见「统一任务管理」）：`ephemeral`/`sticky` = `interactionMode: "none"` + `autoApprove: true`——无人盯着执行会话，故按无交互语义运行（需审批工具**自动通过**、不空等 5 分钟审批超时后跳过，本地与服务模式无差别；任务经用户审批创建，即其授权的无人值守落地），同时触发消息注入一行「无人值守执行」上下文告知模型；`page_capture` 自动禁用、`show` 图表分支引导后端渲染、`ask` 直接报「无交互能力」。`target=session`（绑定用户会话，可能有人在场当场审批）= `realtime` 维持现状
- 禁用判定同时匹配子Agent 命名空间工具（`{agent}_page_capture` 等同名工具同样禁用）；与 `disabledTools` 名单（部署方可另行指定）叠加生效

#### 输出方式（与交互模式正交，同样请求层配置）

**交互模式与输出方式均为请求层配置，服务端引擎内部全部支持**，接入方按自身形式自由组合适配（如业务系统 REST 集成、IM 机器人、Web 前端互不影响）：

| 输出方式 | 语义 | 说明 |
|----------|------|------|
| `final_only`（仅最终响应） | 不推送文本增量（`event.message.delta`）与推理流（`event.message.reasoning`） | 结构化事件（工具调用/审批/`event.message.done` 最终响应/子会话运行过程）仍推送；REST 同步响应即最终文本（content 纯正文，推理在消息独立字段 `Message.reasoning`） |
| `streaming`（流式输出，默认） | 推送文本增量与推理流（含子会话运行过程文本） | Web 前端打字机效果 |

- 引擎 `engine.run` 的 `outputMode` 参数（默认 `streaming`，保持现有 Web/飞书行为）；`final_only` 下文本仍完整落盘会话消息（接入方经存储/最终响应获取完整内容）
- **请求层暴露**（默认值保持各通道现状，传入即覆盖）：
  - REST `POST /api/v1/sessions/:id/prompt`：`interactionMode`（默认 `none`）+ `stream`（默认 `false`=仅最终响应；`true`=流式输出，接入方经 WS 事件订阅消费流）
  - WS `session.prompt`：`interactionMode`（默认 `realtime`）+ `stream`（默认 `true`=流式输出；`false`=仅最终响应）
  - 飞书通道经**接口层**（`BotPromptAdapter`/`EngineBotAdapter`）固定 `multi_turn` + `final_only`（多轮交互 + 仅最终回复）：bot 不直接接触引擎/事件总线，任务内过程事件（工具调用/推理/文本增量）不推送，仅最终回复经回调发送；关键操作（审批/选择）与画图经回调询问/渲染
- 非法 `interactionMode` 值返回 400（REST）/ reply error（WS）

### 待办跟踪

复杂任务支持**待办清单（Todo）跟踪**，Agent 拆解任务、逐项推进，用户全程可见：

- **工具**：`todo`（待办增删改查统一入口——`entries` 为操作列表，每项 `op=add/update/delete`：add 需 `title`（可带 `priority`/`note`/`eta`），update/delete 按 `id` 或 `title` 定位（id 优先，无 id 时 title 精确匹配、唯一包含匹配兜底，多个同名提示改用 id；update 改标题需用 id），省略或空数组 = 查询；返回操作摘要与**当前全部待办状态**快照（含 id），模型一次掌握最新清单，无需再查 id。工具描述引导：新增待办开启新任务时，及时 delete 清理与当前任务无关的历史残留待办——待办只跟踪当前任务，陈旧条目徒增干扰与 token 浪费）
- **状态机**：`pending → in_progress → completed`，异常终止为 `failed`/`cancelled`
- **字段**：标题、状态、优先级、进度（0-100%）、预计耗时（分钟）、依赖项（`TodoItem.dependencies` 类型保留，但 `todo` 工具 schema 与 UI 均未开放）、备注
- **持久化**：随 `chat.json` 的 `todos` 字段持久化（会话级，随会话隔离与恢复；无独立 `todo.json`）
- **Agent 引导**：系统提示词引导模型在复杂多步任务开始时先 `todo` 拆解计划（entries 批量建清单），每完成一步 `todo` 更新，任务结束 `todo`（空 entries）汇报
- **UI 展示**：会话消息流侧边栏实时呈现待办面板（状态/进度/依赖），事件推送 `event.todo.update` 驱动增量更新；`todo` 工具在消息流中渲染为**待办清单卡片**（状态图标 + 标题 + 元信息；状态为内联 SVG 描边图标——待处理空圆/进行中环箭头/已完成圈勾/已失败圈叉/已取消禁止符，随主题着色：进行中沿用主题强调色、已完成/已失败用语义色 `--success`/`--danger`，悬浮提示中文状态标签，元信息行只留优先级（中文高/中/低，纯文本不做视觉区分）/进度/备注），替代通用工具卡片，实时与历史会话一致；清单过长（>8 项）时自动**折叠较早的已完成项**（保留最近 3 项完成作上下文，未完成项始终可见），连续隐藏段收敛为一行「已折叠 N 项已完成」按钮，点击展开、展开后可收起
- **失败恢复**：任务中断后基于待办清单继续执行，跳过已 `completed` 项，从剩余项恢复
- **待办续做**：每轮会话完成（模型给出最终回复）后，引擎自动检查待办清单——仍有 `pending`/`in_progress` 项时，注入一对**模拟 todo 查询工具调用消息**（`assistant(content:"", toolCalls:[{name:"todo", arguments:{entries:[]}}])` + `tool` 结果）并再次进入工具调用循环，直至待办全部完成、达到续做轮次上限（见常量参考，默认 **1 轮**——待办提醒一事一议、不反复打扰，模型未续做即视为已决策收尾）或模型决策收尾。注入形态与真实工具循环**同构**（模型下一轮看到的历史是「自己刚查过待办、结果还有未完成项」的自然行为链，而非外部催办）；尾消息是 tool 结果，天然避开思考类模型的尾部 assistant 约束（无需 user 角色绕路；发送前与最终回复合并为单条工具轮并补 `reasoning_content`，见「发送前消息归一化」）；tool 结果尾部附行动指令「以上待办仍未完成：请继续执行，或确认其已无需处理后收尾。」保命令性（防模型看完清单只回总结），清单文案与 todo 工具查询分支同口径（`interact.ts` 的 `todoSnapshot`，含 id）。**落盘形态**：两条消息均落盘（assistant(toolCalls) + tool），**tool 消息带 `Message.engineNote: "todo"` 标记**（引擎合成的调用，非模型真实发起——留痕与展示区分；assistant(toolCalls) 本身不可带 engineNote：loadHistory 会把带标记的 assistant 转 user 丢掉 toolCalls、破坏配对）；模拟 toolCallId 用 `todo-cont-` 前缀 + uuid。**实时可见**：复用工具事件通道推送 `event.tool.call`/`event.tool.result`（前端自动建待办卡片、结果到达刷新为清单，与真实 todo 调用零差异，历史回放同构渲染）；不再有独立的 `event.todo.continue` 通知条事件（存量会话中的旧形态 user 提醒消息由 isEngineNote 回放兼容）。**模型对注入的回应为纯文本（未执行任何工具）视为已决定收尾，不再注入**；`completed`/`cancelled`/`failed` 项视为已了结不再续做；回复与上上轮完全相同时在结果尾部附防复述提示（需 ≥2 轮才可能触发，当前 1 轮上限下为休眠路径）；压缩护栏的「本次任务输入」（最新一条用户消息，永不裁剪）不受影响（模拟对里没有 user 消息，真输入不会被顶替；模拟对本身可被正常压缩）
- **收尾验证提醒**：与待办续做同机制的兜底纪律——任务结束（无未完成待办）时若**本任务修改过代码文件（write/edit 命中代码扩展名且成功落盘）但未识别到针对这些改动的验证**（四条通道任一命中即算已验证：**sh/py 命令**——命中测试/检查类命令口径四类——① 主流测试 runner 与类型检查/lint 工具、② 通用构建入口 make/cmake/ninja/cargo build/msbuild 等（编译型语言的构建即全量类型检查）、③ 自研工具链 `st` 的 build/test/lint/check 子命令、④ 任意 `×test/×check/×verify/×lint` 形态的命令与脚本名（分隔符或扩展名限定，`latest` 这类字面包含不误命中）；**验证类工具**——名称后缀 `run_tests/run_test/test(s)/verify/check(s)/lint`，含命名空间形态 `self_optimize_run_tests`/`st_check`；**js 编排**——脚本同时出现命令调用点（`sh(`/`py(`/`bg_task(`/`tools.×(`/`command:`）与验证关键词；**子会话同理**——子会话内工具执行经共享的 runToolInterruptible 归入父会话判定，父收尾即覆盖子会话改动），追加一条「【智体·收尾验证】（智体运行时自动注入，非用户输入）…请先运行相关测试或检查确认无回归（项目自带的验证命令优先——bun test/typecheck/lint、make/cmake、pytest、go test、cargo test、st test/st lint 等，在子会话/脚本内跑过同样算）；若你确已运行过验证，请指明具体命令与结果；确不适用请说明」消息再续跑一轮（模型跑验证后正常收尾，或说明原因），上限 1 轮防反复打扰；提醒同为 **user 角色软性提醒**（与待办续做同形态：落盘 `role: "user"` + `engineNote: "verify"`，展示为「引擎提示」通知条、回放保持 user），持久化进会话历史并推送 `event.verify.nudge` 事件（含 messageId/text，前端实时渲染）；拒绝/安全模式拦截与 dry_run 不计入修改，md 等非代码文件不触发。**验证识别的实现与坑**（`trackTaskMods`）：① 写类工具取命名空间短名（`code_write` 与全局 `write` 同权），但**验证工具必须用名称后缀正则**——短名取的是「最后一段下划线之后」，`self_optimize_run_tests` 会变成 `tests`，按短名相等判定永远不命中（实测：全部会话 8 次提醒中 5 次是该 bug 引起的误报，模型明明跑过 `self_optimize_run_tests`）；② js 编排是主推的复杂流程通道，脚本内跑验证命令同样计入，但判定要**「命令调用点 + 验证关键词」同时出现**（仅关键词不算——否则 grep/read 搜到 lint/typecheck 字样的任务会被误记为已验证，反向漏掉真该提醒的场景）；③ **会话工作区（`tmp/`）内的文件不计入代码改动**——那是模型的临时产物与测试夹具（为验证某能力随手写的脚本、导出的中间结果），不是需要回归验证的代码改动；计入会让「写夹具测工具」这类任务收尾误报（实测：测试 `show` 工具时写的 `tmp/demo.py` 被当成「1 个代码文件改动」）。判定按 `ctx.resolvePath` 解析后的绝对路径与会话 `tmp/` 真实路径比对（项目绑定子Agent 的相对路径按项目根解析，故项目代码与绝对路径用户代码不受影响）；解析异常按「非会话文件」保守计入，不因解析异常放过真实改动；④ **命令口径要覆盖自研工具链与非 JS 生态**——白名单只认 bun/pytest/go test 这类主流入口时，全程跑 `st build`/`st test` 的任务收尾仍被误报「尚未运行任何测试」（实测：C++/自研工具链任务全程 4 轮全量测试后仍收提醒）；口径按四类收敛（主流 runner、通用构建入口、自研 `st` 子命令、`×test/×check/×verify/×lint` 形态脚本名），提醒文案同步改为「未识别到针对这些改动的验证」并附「若你确已运行过验证，请指明具体命令与结果」出口——判定是启发式，文案不得写成与事实相反的断言
- **引擎注入消息的角色约定（统一 user + engineNote 标记）**：思考类模型（DeepSeek thinking 等）**不接受以 assistant 结尾的请求**（视为前缀续写、要求回传 `reasoning_content` → 400，实测），而引擎/系统写入的合成消息**注入位置往往就是模型下一次调用的前一条**（待办续做、收尾验证、子会话合入、任务结果写回、全量压缩摘要等）。因此凡引擎而非模型产出的消息一律：**落盘与进模型上下文均为 `user` 角色**（落盘即 user，回放同形，不再区分两套形态），并带 `Message.engineNote` 标记供前端渲染为弱化通知条（与用户自己发的消息区分，不提供撤回）。**内容头一律带智体身份标记 `【智体·类别】`**（`core/support/agent-note.ts` 的 `agentNoteHead`：待办提醒 / 收尾验证 / 子会话「名」已合并 / 定时任务「名」执行结果 / 任务中断）——消息与用户输入同角色，模型只能凭内容判来源，身份头让它一眼看出是运行时自动注入；**命令式提醒**（收尾验证要模型去做事；待办续做同为命令式但走模拟工具对——行动指令在 tool 结果尾部）随头附自述行「（智体运行时自动注入，非用户输入）」，避免被当成用户新要求执行。已收口点（全部落 user + 标记）：收尾验证提醒（`verify`）/定时任务结果写回（`cron`）/子会话报告合入（`subsession`）——“引擎提示”/“定时任务”/“子会话合入”通知条；**待办续做例外**——注入为**模拟 todo 查询工具调用对**（assistant(toolCalls) + tool，见「待办续做」）：尾消息是 tool 结果、天然避开尾部 assistant 约束，tool 消息带 `engineNote: "todo"` 留痕（assistant(toolCalls) 不可带——loadHistory 会把带标记的 assistant 转 user 丢掉 toolCalls）；**上下文压缩摘要**（loadHistory 按 user 注入，全量压缩 `scope:"all"` 时摘要会落尾）；**子Agent 装载提示词**（role=system，loadHistory 前置到历史最前，不会落尾）。两个配套细节：①**“本次任务输入”定位与裁剪**——压缩硬护栏的“最新一条用户消息”（永不裁剪）与前端会话自动命名取首条输入，均跳过 `isEngineNote` 命中的消息（否则任务末尾的提醒/写回会顶替真输入）；但**护栏裁剪本身不排除引擎提示**（子会话报告可达数千字符且可再生——全文在过程存档/bg_task，不该永占窗口）。②**总兜底**：`llm.ts` 三个 provider 序列化前统一执行**尾部纯文本 assistant 降级为 user**（`demoteTailAssistant`，带 warn 日志）——未预见的来源（存量数据/撤回截断残留/未来新机制）也会在发送前被归一化，不再让整个会话因尾部形态被 400 卡死（若将来确需前缀续写语义，需带回该 assistant 的 `reasoning_content` 并关闭此兜底）。
- **发送前消息归一化（工具轮形态）**：请求发出前对消息数组另做两步归一化（`llm.ts` 的 `mergeAssistantTurns` 与工具轮 `reasoning_content`），只改**线上形态**、不动会话存储：① **相邻 assistant 合并为单条**——引擎写入的 assistant 文本消息会与紧随的工具轮消息相邻（待办续做的「最终回复 + 模拟工具对」、配对修复补的 assistant 桩），而服务端按**单条 assistant 轮**校验与续写，相邻形态下只有首条的内容生效、第二条的字段（含 `reasoning_content`）被丢弃；合并后与真实工具轮同形（文本 + 调用同条），也镜像 Anthropic 序列化的同角色合并。② **工具轮一律携带 `reasoning_content`**（空串）——思考类模型（DeepSeek thinking，实测）只认**自己签发**的 tool_call id（id 仅改一位即视为客户端构造），其余一切 assistant 工具轮（引擎注入的模拟工具对、配对修复桩、换模型后回放的历史工具轮）缺该字段即整请求 400 `The reasoning_content in the thinking mode must be passed back to the API`；补空串满足字段存在性、零 token 开销，真实推理仍不进模型上下文（同上「推理独立字段」口径）。两条与尾部 assistant 降级同为发送前兜底，覆盖未预见的来源与未来新机制。

#### 用户询问（`ask`）

全局工具 `ask` 是**向用户询问并阻塞等待回应的统一入口**（原 `ask_user`/`ask_env`/`plan` 三工具合并），分支按专属参数三选一：**选项询问**（`prompt`+`options`，原 ask_user——方案确认、方向决策）/ **环境变量填值**（`name`，原 ask_env——模型驱动的凭证索取，见「环境变量」章）/ **计划审批**（`title`+`steps`/`content`，原 plan——复杂任务先获批再执行）；协议层不变（`event.choice.request`/`event.env.request` 与 `choice.decide`/`env.decide` 回传通道共用）；结果文案前缀（「用户选择：」「计划已批准」等）与「请审核计划」prompt 前缀是前端卡片识别契约。

**① 选项询问分支**——向用户提出一组选项并**阻塞等待用户回应**，用户的回应作为工具结果返回给模型，据此继续执行：

- **参数**：`prompt`（问题描述）、`options`（选项数组，至少 1 项）、可选 `multi`（布尔，默认 false，true 为多选）
- **选项形式**：每项可为纯文本字符串，或**复杂选项** `{ title, description? }`（UI 按标题 + 说明展示，提交值取 title）
- **回应方式**（三种，均可）：① 点选选项（`multi=true` 时可勾选多项，提交值为选项集合）；② **输入自定义文本**（直接输入自己的答案，不限于给定选项，多选时追加到已勾选项一并提交）；③ **拒绝回答**（不再追问）
- **执行**：引擎发布 `event.choice.request`（携带 `choiceId`/prompt/options/multi；**计划审批分支另携带 `plan` 载荷**——`{title, content, path}` 计划标题/Markdown 全文/文档逻辑路径）并**阻塞等待**；用户经 UI 选择卡片、WS `choice.decide` 或 REST `POST /sessions/:id/choice` 提交后，ask 选项询问分支返回结果注入模型，任务继续：
  - 选项或自定义文本 → 「用户选择：X」
  - 多选 → 「用户选择：X、Y、Z」（以「、」连接）
  - 拒绝回答（WS/REST `refuse: true` 或 option/options 均缺失，引擎侧传 null）→ 「用户拒绝了本次询问…」，模型停止继续询问、基于现有信息自行决策
- **超时**：等待超时（与审批同值，5 分钟）自动取消，返回降级提示让模型自行决策
- **卡片高度与收缩**：审批容器 `#approvals` 整体限高并内部滚动（卡片再多也不会把会话区压没）；单卡另提供两种收缩操作（`web/src/card-fold.ts`，纯高度模型 + DOM 绑定）：
  - 两种控件都位于**卡片外侧上方**的一条操作带（`--ic-handle-h`，18px）——不占卡片内容区、不压住卡片头与正文；内容确实超高时该带才出现（卡片随带高补 `margin-top`）
  - **折叠按钮**（操作带右端）：箭头指向「点下去往哪走」——展开态朝下（点它收起）、折叠态朝上（点它展开），`aria-expanded`/`aria-label` 同步；折叠态把内容裁到预览高度（`--ic-preview-h`，默认 54px）并底部渐隐——保留卡片头与首行内容（问的是什么仍看得见），其余收起
  - **内容区上限统一走一个变量 `--ic-max`**（由 `card-fold.ts` 写卡片上）：折叠态 = 预览高度；**展开态 = 拖出值，未拖过则 = 本尺寸配额**——展开态同样受配额约束，内容超出部分在内容区内部滚动。不这么收口就会出现「未拖过的卡片按内容自然高度无限长」（实测长卡片 1322px 吃满屏，用户还得在卡片区容器里再滚一次才能看到选项）
  - **拖拽把手**（操作带中段）：向下拖到吸附线以下即折叠、向上拉回即展开，中间为任意高度；键盘可及（把手聚焦后 ↑/↓ 每次 40px）
  - **高度配额分档**：容器宽屏取 60dvh；**窄屏（≤860px）另取一道「给会话区留白」的硬上限**——`min(42dvh, 100dvh − 标题栏 − 288px)`。单卡上限同口径分档（宽屏视口 60% / 窄屏 42%）。手机一屏本就只有几百像素，沿用宽屏配额会把对话内容直接挤得看不见
  - **初始态：全展开**——卡片一出现就是完整可读、可直接作答的内容（问题与计划全文正是用户作答的依据），不要求用户先点一次「展开」；空间约束由展开态配额承担（内容超出部分在内容区内部滚动），收缩是用户的**显式动作**（折叠按钮/拖拽）。折叠态只在两种情形出现：用户手动折叠，或同 reqId 重建时继承上一次的折叠态
  - 内容本来就不超过预览高度 + 24px 时不提供折叠（操作带整条隐藏，不引入多余控件）；折叠态与自定义高度写在卡片元素上（`data-folded`/`data-card-h`），**同 choiceId/envId 重建（断线重连事件重放）时继承**，用户刚调好的高度不被重置
- **UI 渲染**：审批容器中渲染**选择卡片**（问题 + 选项按钮/复杂选项行 + 自定义文本输入框 + 拒绝按钮；**自定义答案输入是多行 `textarea`**——用户作答常带 Markdown 列表/代码块，单行输入会把换行吃掉（粘贴同理）；`Enter` 提交、`Shift+Enter` 换行（输入法组合态的回车是选词确认，不当提交），高度随内容自增（上限与 CSS `max-height` 同口径，超出后输入框内部滚动，`border-box` 下自增高度补上边框高免得多出一条滚动条）；多选时选项可勾选、多一个「确认选择」按钮；**携带 `plan` 载荷时卡内顶部内嵌计划全文**——复用计划卡渲染（标题 + Markdown），限高滚动防长计划撑爆审批容器，审批时直接可见不依赖消息流位置，刷新/切回经 attach 快照恢复；**计划区固定 `flex: none`**——卡片内容区是列向 flex 且被 `--ic-max` 限高，计划区若参与收缩（默认 `shrink: 1`，且 `overflow: auto` 使其自动最小尺寸为 0）会被下方选项区挤成一条线、正文被裁掉只剩标题；不收缩则按自然高度铺开（上限 40vh），超出部分由内容区整体滚动承载；**问题文本按 Markdown 渲染**（与计划卡同口径，模型常带列表/行内代码/粗体；选项按钮文本仍是纯文本）,提交即决策（实时，绑定 choiceId），**提交成功卡片随即关闭**（任务结束清理兜底）；**同一 choiceId 重复推送（断线重连事件 seq 重放）替换旧卡不堆叠**；历史裸调用（异常中断、无结果）回退可交互选择卡，提交降级为作为用户消息（「我选择：X」/「我选择：A、B」/「我拒绝回答」）发送
- **选中态视觉约定**：选项选中态为 **accent 实底 + ✓ 标记 + 加粗**（未选中悬停为浅底描边），两者必须一眼可辨——多选卡片的勾选反馈是用户唯一的“已选”确认。两条 CSS 约束：①规则写在各 hover 规则**之后**（同特异性下后出现者生效，避免悬停时把选中态盖成 hover 样式）；②选择器带 `[data-theme]` 前缀提权到 0,4,0——主题样式普遍以 `[data-theme="x"] .choice-opt`（0,2,0，且在本表之后加载）定义选项基础外观，会同特异性盖掉选中态（`matrix` 主题的 `::before` 勾选框同理会盖掉 ✓）。即**主题可改外观、不可改交互态**，与「窄屏抽屉是布局契约、特异性须压过主题」同一约定
- **消息流问答记录卡（等待期不预览）**：ask 选项询问分支调用时**中断当前文本段**（封段），等待作答期间消息流**不渲染问题预览卡**——交互作答由审批容器选择卡片承载（与消息流展示卡上下堆叠会被视为重复卡片）；**结果到达时**在消息流落**问答记录卡**（问题 + 选项展示态（**用户选中的选项高亮**，与交互卡同款选中态；自定义文本/拒绝不命中选项则无高亮） + 头部按结果文案呈现「✓ 用户回答 / ✕ 用户拒绝 / ⏱ 选择超时」+ 回答文本），**问题与回答文本都按 Markdown 渲染**（同计划卡口径；回答块内字号跟随 `.choice-answer` 的 12.5px，不被工具卡正文的 13.5px 规则提走），模型后续回复另起新气泡——问答交换在会话流中完整可见；**历史重载**按持久化参数 + 结果同构渲染记录卡（带结果不再呈现为可交互的未答状态）；子会话运行过程（subsession_run）内的 ask 选项询问分支同样封段、记录卡渲染进折叠容器
- **等待期不误判挂起**：选择/填值/画图/捕获等交互等待事件刷新前端流活跃时间——空闲超时兜底（150s 无数据，高于服务端 LLM 读空闲超时 120s：模型调用假死先由服务端超时上报明确错误，前端看门狗只兜底服务端检测不到的挂起）只按「无任何数据」判定，等待用户回应的挂起不计入（防误杀最长 5 分钟的选择/填值等待）；看门狗中止且无任何输出时渲染「生成超时」说明气泡，不再静默结束
- **页面不可见期间不计离线也不取消**：前端看门狗只在「**页面可见** + **已连接** + **无待决交互卡** + **有效空闲超阈值**」四条件同时成立时才取消任务。真值源是 `web/src/page-idle.ts` 的可见时长（按时间点求值，不依赖「回前台先跑 visibilitychange 回调」——恢复瞬间被冻结的定时器可能先于回调执行）：不可见时段（后台标签页/手机切 App/锁屏）从空闲里扣除，隐藏中直接不发。判据统一为「前端无权判服务端死活」：断线=连不上，后台=连得上但用户看不见，两者都不替用户下中止令。**关闭浏览器的行为由此可解释**：页面销毁后看门狗不存在（且 WS 断线），任务照常跑完；而切后台时页面仍存活、定时器被冻结/节流、本地连接标志仍为 true（close 事件未被处理），恢复后补触发就会把整段后台当挂起杀掉正在跑的脚本（服务端 `idleTimeout` 240s 之前尤其稳定命中）
- **长工具执行心跳**：工具执行期间引擎按 `TOOL_HEARTBEAT_MS`（默认 25s）周期发布 `event.tool.alive`（payload 含 `name`/`toolCallId`，主循环与子会话循环统一收口在 `runToolInterruptible`；快工具在首个周期前结束不产生事件，前端不渲染仅刷新活跃时间）——阻塞类工具（sh/py 跑构建/长测试等）执行期间无其他事件，无心跳会被前端空闲看门狗误判挂起取消（工具自身 timeout 尚未到点），心跳让看门狗与工具超时各司其职：看门狗抓真正的流挂起/断连，工具超时管进程执行上限

**② 填值分支**：模型调用（`name` 变量名 + `description` 用途说明 + `secret` 敏感值掩码）→ 前端填值卡片，值注入本次任务 env 并保存浏览器本地（完整机制见「环境变量」章「ask 填值分支」节）；仅实时前端通道。

**③ 计划审批分支**：为复杂/多步骤任务制定执行计划——`buildPlanMarkdown`（title+steps 勾选清单或 content 全文）落盘会话工作目录 `plans/{标题清洗}.md`（物理位置 `{session}/tmp/plans/`；writeGuard 预检）→ `waitForChoice` 阻塞等待批准/拒绝（**`plan` 载荷随 `event.choice.request` 到达前端——选择卡内嵌计划全文，审批时直接可见**，不再只给标题 + 文件路径；**等待期消息流不重复渲染计划卡**（与选项询问分支同款延迟落卡，上下两张同款计划卡会被视为重复），决策结果到达时消息流才落计划卡并呈现审批结果态）；批准 → 模型严格按计划逐步执行（todo 跟踪）、拒绝（可附自定义修改意见作为 feedback）→ 修订后重新提交、拒绝审核/超时 → 停止计划相关操作；`data` 返回 `{status, title, path, feedback}`（`tool_schemas` 可查）。

**分支门控（合并型工具的通道能力校验，与 show 同模式）**——`ask` 不声明工具级 `interaction`（全模式可见），按 `ctx.interactionMode` 在分支内校验：选项询问/计划分支 `none` 下明确报错「当前通道无交互能力」（不空等 5 分钟超时）、`multi_turn`（飞书）经选择卡片作答；填值分支仅 `realtime`（飞书/REST 下明确报错并引导用户在设置面板配置）。

### 统一任务管理

**用户级**统一任务：定时（`scheduled`）/ 普通（`manual`）/ 闲时（`idle`）三类任务共用一份用户级存储（`users/{user}/tasks.json`）与一条调度队列，任何会话内经 `task` 子Agent 创建或经 REST 创建。任务属于**用户**而非会话——会话删除/过期后任务仍在，同一用户在任何会话可见可管（跨用户隔离不变）。能力由环境变量 `GEBAI_TASKS_ENABLED`（**默认 `true`**）统一开关：显式 `false` 关闭时 `task` 子Agent 不注册（`task_*` 工具在工具表/schema、`agent_list`/`agent_load`/`subsession_run` 中完全不可见）、调度器不启动、REST 管理面返回 503；开启后按需装载、REST `/api/v1/tasks` 可管（见「task 子Agent」）。

- **类别（`kind`）只决定何时入队**，执行体与其余参数完全通用：
  - `scheduled` **定时**：按 `schedule` 表达式到期**自动插入队首**（优先级最高；不抢占正在运行的任务，无空槽就在队首等待）
  - `manual` **普通**：创建即入队（`runNow`，缺省 true）按序执行，也可随时手动 `run` 再次入队；入队默认排普通任务队尾，`front=true` 置顶
  - `idle` **闲时**：仅当队列中无 scheduled/manual 条目、且没有运行中的任务、且该用户没有运行中的会话时启动，**同时只跑 1 个**（串行推进）；用户级待办开启 ⚡ 闲时自动执行时即绑定此类任务（见「用户级待办」）。闲时条目**一次入队即一次执行、一次执行即终**：无论成功还是失败/超时都自动停用，**失败不重试**（原因留在 `lastError`，重新启用——待办重开 ⚡ 或手动执行——即恢复）；缺省单次执行超时 3 小时（`TASK_IDLE_TIMEOUT_MS`，任务 `timeoutMs` 可覆盖）；排队顺序由外部清单序提供（待办场景按其清单顺序，其他场景按创建时间）
- **执行体（`runner`）**：
  - `script` **脚本运行**：执行 shell 命令（在任务资源目录 `users/{user}/tasks/{task_id}/` 以用户环境运行——目录跨次运行保留产物；环境为进程环境 + 尽力解析的关联会话环境，不依赖会话存活），执行结果（成功/失败 + 输出）写入任务运行历史，并在**来源会话仍存在时**作为消息写回其消息流（`【智体·{类别}「名称」执行结果（成功/失败）】`，如 `【智体·定时任务「日报」执行结果（成功）】`；历史可见、模型可感知；来源会话已删除则静默跳过）。**写回消息为 `role: "user"` + `engineNote: "task"`**（前端渲染为「任务」通知条）——与引擎提醒同规则：思考类模型不接受以 assistant 结尾的请求（写回后它往往成为尾消息，会话下次带工具面的请求会被 400 拒绝，实测）
  - `prompt` **提示词运行 agent**：以指定提示词触发一次完整 Agent 会话（复用主循环），过程与结果在该执行会话的消息流呈现，末条 assistant 消息作为结果摘要进任务记录/通知；**交互模式按执行目标分流**——`ephemeral`/`sticky` 无人值守形态跑 `interactionMode: "none"`（见「交互模式」：需审批工具**自动通过**、不空等 5 分钟审批超时后跳过，脚本模式同样生效；`page_capture`/前端渲染/ask 询问不可用），触发消息附一行「无人值守执行」上下文告知模型；`target=session` 可能有人在场当场审批，保持 `realtime`
- **执行目标（`target`，prompt 型，解耦会话的核心设计）**：
  - `ephemeral`（缺省）：每次执行**新建独立会话**（会话名 `{类别}「名称」`，进入用户会话列表，上下文每次全新不累积、随正常数据生命周期清理）——例行检查/报告类首选；可选 `agents` 预载子Agent 名单（写入新会话 `loadedSubAgents`，装载保障按此注册工具与提示词）
  - `sticky`：**专用会话跨次复用**（首次触发惰性创建并记 `stickySessionId`，后续在同一会话续跑——上下文延续，适合需要记住上次状态的任务）；**绑定丢失时按标题找回**（会话被删/任务改入 sticky 时，从用户会话列表按会话名匹配本任务之前执行过的会话，取最新——继承既有执行上下文减少重复摸底，找回归位 `stickySessionId`；列表失败或无同名时退回新建，不阻断执行）
  - `session`：**绑定既有会话**执行（缺省为创建来源会话；该会话正有任务运行时条目留在队列等待，不打断用户会话）；绑定会话被删除后**自愈降级为 ephemeral**（任务保留，一次性记因），不再随会话消失而丢失
- **定时表达式**：5 段 cron（`分 时 日 月 周`；支持 `*`/`*/n`/`a-b`/`a,b,c`，日与周均受限时任一命中）或 `@every <n>s|m|h|d`、`@daily`/`@hourly`/`@weekly`/`@monthly`、`@at <时间>`（**一次性**：如 `@at 2026-09-01T09:00`，触发后自动停用；创建/修改时拒绝已过去的时间）；可选 `timezone`（IANA 名如 `Asia/Shanghai`，缺省服务器本地时区）——指定时区时按目标时区墙上时钟扫描（UTC 域日历推进 + 双次偏移换算，跨 DST 正确），服务器 UTC 部署也能按用户时区触发；非法表达式/时区创建/修改时即拒绝，永不触发的表达式（如 `0 0 30 2 *`）同样拒绝
- **可靠性参数**：
  - `misfire` 停机错过补跑策略：`skip`（缺省，错过即跳过、下次从当前时间重算）/ `run`（服务启动后发现触发点已过期则立即补跑**一次**——加载时保留过期 `nextRunAt`，首个 tick 执行后按当前时间重算）
  - `timeoutMs` 单次执行超时（缺省脚本 5 分钟（与 `sh`/`py` 同级）/ 提示词 30 分钟；合法区间 1s~24h）——提示词型到时走 `engine.windDown`：先让运行中的子会话**快速结束**拿结论（注入收敛指令 + 宽限，见「子会话快速结束」），再取消会话任务，并记 `status=timeout`（注意超时定时器不可 `unref`：await 挂起的 Promise 不保活事件循环）
  - `maxConsecutiveErrors` 连续失败自动停用阈值（缺省 0 不停用；连续 error/timeout 达阈值即 `enabled=false` 并在 `lastError` 记因——防错误任务无限重试刷屏/刷通知；成功清零，重新启用也清零）
- **任务级环境变量（`env`）**：任务定义内的环境变量表（`Record<string,string>`，可选，两类执行体通用），服务端持久化于 `users/{user}/tasks.json`：
  - **注入点**：脚本型与继承环境（关联会话环境 ?? 进程环境）合并后传入 `sandbox.exec`，提示词型作为 `engine.run` 的 `envOverride` 注入执行会话——模型 Provider 重建、脚本子进程、子Agent 环境读取均生效；优先级**高于**进程全局与会话环境（任务专用配置覆盖宽口径默认值）
  - **无人值守可用性**：浏览器本地 env 只随交互会话的 `prompt` 请求到达服务端（见「前端环境变量」），任务触发时无人发包——任务级 `env` 是无人值守任务获得配置的唯一服务端持久通道（不依赖浏览器、不依赖创建来源会话存活）
  - **校验与持久化边界**：变量名须为标识符（拒绝 `__proto__`）、值须为字符串；创建/修改时非法即拒，启动加载遇外部编辑损坏的条目**整条丢弃**（不阻断任务加载）；修改为**整集覆盖**语义（传空对象 `{}` 清除）
  - **敏感值加密落盘**（`core/support/crypto.ts`）：键名判为敏感的项（`isSensitive`，即 `*_KEY`/`*_TOKEN`/`*_SECRET`/`*_PASSWORD`/`*_HASH` 等）以 **AES-256-GCM 封存**后写入 `tasks.json`（格式 `enc:v1:<iv>:<密文||标签>`，随机 IV，同一明文两次封存结果不同）；非敏感值保持明文（运维直接查看任务配置）。**密钥为内置常量**（随产物分发，部署方可在构建前替换种子）——无人值守任务必须无条件自解密，密钥不可能来自交互输入，故只能「同机常量」；解密失败（换密钥/数据损坏/旧明文）一律**降级为原样返回**，不抛错打断调度
  - **威胁边界（如实口径）**：本机制防的是「随手看到」——文件被浏览/被 grep/随备份或同步外带/被模型经 `read`·`grep` 读进上下文；**不防**已能读取整个数据目录与产物的攻击者（密钥与数据同机，他一起读）。与「内置模型 Key 随二进制分发」同档强度：提高门槛，非密码学保证
  - **掩码往返**：`task_list`/`get`/REST 回显时敏感键值替换为固定占位 `***`——**掩码原样回传即保留原值**（与通知 `secret` 的 `***` 惯例一致，且**必须**如此：前端任务表单是 env 全集视图，回传占位若被当作新值写回就会用掩码覆盖真值）；显式传新值即轮换
  - **归属与口径**：明文（内存态）与密文（磁盘）都落在用户自己的用户目录，属**用户自持数据**——「用户环境变量服务端零留存」约束的是浏览器本地配置不落服务端，任务级 env 是用户显式随任务保存的配置，二者边界见「环境变量配置」
  - **传输面**：`task_add`/`task_update` 的 `env` 参数、REST `POST/PATCH /api/v1/tasks` body 的 `env`、前端任务表单的「环境变量」多行文本框（每行 `NAME=值`）
- **执行免审批**：无人值守执行（`target=ephemeral/sticky`）的需审批工具**一律自动通过**（本地模式与服务模式无差别）——任务经用户审批创建/修改，执行即其授权的无人值守落地，与 REST `autoApprove: true`（调用方即用户本人）同一授权面；`target=session` 绑定用户会话可能有人在场，保持实时审批不改姿态。管理动作（`task_add`/`task_update`/`task_run`/`task_cancel`/`task_remove`/`task_files`）仍逐次需审批（见下「安全」）
- **执行记录**（`users/{user}/task-runs/{task_id}/{时间}.json`，按文件落盘、不内联在任务定义里）：每次运行（含定时到期未启动的 `skipped`）各写一个记录文件，内容为完整 `TaskRunRecord`——触发/结束时间/状态（success/error/skipped/timeout）/耗时/输出摘要/执行会话 id/手动标记/未启动即跳过的原因 `reason`；**文件名为记录时间**（UTC ISO，`:` → `-` 以适配 Windows 文件名；同毫秒多条追加 `_N`），字典序即时序——列表无需读内容排序、清理按名删最旧；单任务保留最近 `TASK_RUNS_KEEP`（200）条，超出按时间删除最旧；**旧数据自愈**：启动加载时把定义文件里遗留的 `runs` 数组一次性导入记录目录（幂等：目录已有记录则跳过），随后经 `forceWrite` 沉降把该字段从定义文件清掉。读取入口：`ctx.tasks.runs`（工具侧）、`GET /api/v1/tasks/:id/runs?limit=`（REST）与前端任务详情「运行历史」（异步拉取，运行次数变化即失效重取）
- **队列与额度**（每个用户一条队列，三类任务与待办手动执行同队列统一调度）：
  - **额度**：每用户 `GEBAI_TASK_MAX_CONCURRENT`（缺省 5）个同时运行的任务会话；额度只约束任务，用户对话会话不占额度；**运行中的任务不因额度不足被中断**（新条目排队等待）
  - **排序**：定时（权重 0）< 普通置顶（999）< 普通（1000）< 闲时（2000），同级按入队顺序 FIFO（同毫秒入队也保序：排序只比优先级与入队时刻，不用 id 兜底）；定时任务到期自动排到队首
  - **闲时自动进场仅由跑调度的实例发起**（调度器主实例锁门控）：从实例只服务请求、不主动领闲时活——从实例自己没有会话、`busyUser` 恒假，不加门控则每次手动执行/新建任务都会顺手把闲时任务拉起来跑
  - **目标会话忙**（`target=session` 指向的会话正在对话或跑任务）：条目留在队列等待，不启动、不占额度，下一轮再评估——不打断用户会话
  - **同一任务不并发**：上次未结束又到点（或排队中又到点）→ 记 `skipped` 运行记录（`lastError` 记因，不计入连续失败计数），不重复入队叠加
  - **一次性定时任务（`@at`）**：到期入队时清 `nextRunAt`（退出到期检查）、**执行结束**才停用——入队前停用会被入队路径以「任务已停用」拒绝，导致一次性任务永不执行；加载时已过期且无未来触发点的直接停用（防每次启动重算为过去值的热循环）
  - **重启恢复**：`queued` 条目重新入队（保持相对顺序）、`running` 条目标记为中断（不自动重跑，`misfire=run` 的定时任务除外）
  - **执行链健壮性（落盘/异常不阻断调度）**：任务是「出队即异步跑」——启动与收尾的落盘失败（目录被移除/磁盘满/权限）只降级为告警（`log.warn`），收尾链照常走完（状态回 idle、`event.task.result` 发布、队列继续推进）；执行链抛出的异常走统一兜底（清运行标记 + 记 `status=error` + 发结果事件 + 推进队列）——不会成为进程级未捕获 rejection，也不会让任务永久停在「运行中」（落盘仅影响重启后的状态恢复）
  - tick 周期 30 秒（到期检查 + 队列推进）；入队/置顶/运行结束均立即触发一次推进
- **通知通道（`notify`，任务内嵌数组可配多条）**——无人值守任务的送达手段（`core/schedule/notify.ts`）：
  - `webhook`：任意 http(s) 回调，POST JSON（载荷即 `task.result` 事件形态 + 通道 `at` 名单：任务信息/状态/输出摘要/耗时/执行会话/停用标记/@ 人名单，接收方系统可据此渲染提及）；URL 直配时经事件 Webhook 同规则 SSRF 校验（回环/链路本地/元数据地址默认拒绝，注册与投递逐跳共用），可选 `secret` 附 `X-Gebai-Signature: sha256=HMAC` 签名头（与事件 Webhook 投递同款，接收方一套校验通吃）；亦可 `webhook_id` **引用 REST `/api/v1/webhooks` 注册的事件 Webhook**（与 `target` 二选一，创建时校验存在性与归属——具名注册仅本人任务可引用、全局注册（admin，`userId` 未记录=部署方集成通道）人人可引用；投递时经注入的解析器（`WebhookManager.of`）解析其 URL 与签名密钥——注册侧改 URL/密钥即时生效，引用消失记 `lastNotifyError` 跳过不影响任务）
  - `feishu`：飞书通知通道，`target` **双形态**——群自定义机器人 webhook（`https://open.feishu.cn/open-apis/bot/v2/hook/…`，域名与路径强校验）或**群 chat_id**（`oc_` 前缀——以应用身份向指定群推送，走 `feishu_chat` 同款应用消息，需服务端配置飞书应用凭证，未配置时创建即拒；chat_id 可经 `feishu_group` 子Agent `chats_list` 查询）；webhook 形态可选 `secret` 加签（`sign = base64(HMAC-SHA256(key=timestamp+\n+secret, msg=""))`）；**消息形态随通道与 `at` 名单自动选择**——webhook（URL）形态默认发 **1.0 卡片**（`msg_type=interactive`：`config.wide_screen_mode` + 头部按状态着色 green/red/orange/grey + 任务名，正文 `div`/`lark_md` 组件（`**粗体**` 字段行：状态/周期/时间/耗时/错误/输出摘要/执行会话/停用标记）+ note 脚注；**自定义机器人 webhook 不支持 2.0 卡片**——schema V2 + note 组件实测被拒 `code=11246`，2.0 卡片仅应用消息接口支持），chat_id（应用消息）形态默认发 **2.0 卡片**（`schema:"2.0"`，与对话桥接同款新版本接口，正文 `markdown` 组件，整体上限 12000 字符）；`at` 含 `"all"` 时降级 **text 消息**（见 `at` 名单条目）；**投递校验飞书业务码**——webhook 业务失败时 HTTP 仍返回 200，解析响应 JSON `code !== 0` 即视为投递失败（记 `lastNotifyError`，防「任务 success 但群内无通知」静默失败）
  - `feishu_chat`：飞书**应用消息**（`target` 为群 chat_id），复用全局 `GEBAI_FEISHU_APP_ID/SECRET` 凭证（与机器人桥接/云文档共用）经 tenant api 发送，消息形态与 `at` 规则同 `feishu`（默认 2.0 markdown 卡片、含 `"all"` 降级 text）；未配置凭证时创建即拒绝
  - `at` **@ 人名单**（可选，全通道）：条目为 open_id（`ou_`/`un_`/`on_` 前缀）或 `"all"`（@所有人），字符串或 `{id,name}` 形态（name 为展示名，缺省由客户端解析真实姓名），存储归一为 `{id,name?}` 并去重（**@特定人**：open_id 不知道时可装载 `feishu_group` 子Agent 用 `members_list` 按姓名查询）；webhook 通道随 JSON 载荷 `at` 字段携带（供接收方解析提及）；飞书通道按名单自动选择消息形态——**仅 @ 具体 open_id 时 markdown 卡片**（webhook 形态为 1.0 卡片 `div`/`lark_md`、应用消息形态为 2.0 卡片 `markdown` 组件，`<at id=…>` 标签均置于正文首行——与 text 消息的 `<at user_id=…>` 属性语法不同，被 @ 用户收到提及通知），**含 `"all"` 时自动降级 text 消息**（@所有人 提及通知以 text 正文标签为可靠路径：1.0 卡片时代卡片内 @所有人 被静默忽略（实测），2.0 markdown 组件虽支持 `<at id=all>` 但提及权限因应用配置而异，通知场景求稳不冒险；降级后格式化为纯文本多行正文，具体 open_id 的 at 标签在 text 中同样生效，群机器人 webhook 与应用消息同规则）；飞书正文输出/错误的尖括号全角化净化（卡片与 text 两形态同规则），防任务输出注入 `<at>`/`<a>` 标签（@ 与链接仅由通道配置产生）
  - `notifyOn` 通知时机（**两种**）：`auto`（缺省，执行结束自动把结果摘要发出——prompt 型即会话最后一条回复、脚本型为输出）/ `model`（**不自动发**——通知完全由执行会话的模型经 `task_notify` 决定与撑写：例行正常保持静默、异常或需要用户知晓时主动推送；脚本型任务无模型参与，配 `model` 则不自动通知）；投递**尽力而为**——失败记 `lastNotifyError` 不影响执行结果与调度，成功清除；通知密钥在 `task_list`/REST 回显中脱敏（`***`），修改时传 `***` 保持原值；安全模式下通知投递（外发网络）跳过
  - **主动通知（`task_notify`）**：任何模式下模型都可主动推送自撑正文——`TaskManager.notify(user, id?, {text,title?,at?}, {sessionId})` 解析投递通道 = 任务 `notify` ?? 全局默认通道（webhookId 引用即时解析、secret 用真值、`at` 可由调用方覆盖通道配置），逐通道在**用户已配置的通道**上投递（不接受调用方传入任意 URL），尽力而为返回 `{taskId, delivered, errors}` 并记 `lastNotifyError`；`id` 缺省时按**执行会话**反查正在运行的任务（引擎适配层注入 sessionId，模型无需回显任务 ID）；安全模式拒绝、无可用通道报配置指引。**执行会话可用性**：prompt 型任务存在可用通道时，会话解析自动把 `task` 追加进执行会话的装载名单（不改任务自身 `agents` 配置），触发消息附任务 ID 与 `task_notify` 用法提示（并限定：执行任务期间不要用 task 的其它工具管理任务）——否则执行中的模型拿不到该工具
  - **全局默认通道**（环境变量 `GEBAI_TASK_NOTIFY_WEBHOOK` / `GEBAI_TASK_NOTIFY_FEISHU`）：任务未配置自己的 `notify` 时自动经全局通道推送（运营兜底——无人值守任务忘配通知不至失联）；**任务自配 `notify` 则只走任务自己的通道、不与全局叠加**（防重复推送）；全局通道在**投递时**解析（不写入任务数据，环境变量改动重启后即时生效），`notifyOn=model` 时不自动发（其余与 at/卡片形态规则同款）；启动构建期逐条校验（SSRF/域名/chat_id 形态、chat_id 形态需飞书应用凭证），非法配置告警忽略不阻断启动
- **工具**（`task` 子Agent 命名空间暴露 `task_*`）：
  - `task_add`：创建任务（`runner` 必填，`kind` 缺省按是否给 `schedule` 推断；可选 `name`/`script`/`prompt`/`schedule`/`timezone`/`misfire`/`target`/`session_id`/`agents`/`timeout_ms`/`env`（任务级环境变量）/`notify`（webhook 支持直配 URL/`webhook_id` 引用注册通道，含全通道 `at` @ 人名单）/`notify_on`/`max_consecutive_errors`/`enabled`/`run_now`/`front`）
  - `task_list`：查看当前用户全部任务（含类别/执行体/运行态/周期/下次执行/次数/最近错误）+ 队列概览（并发额度、排队顺序与等待原因、运行中条目）
  - `task_update`：按 id 修改（全部可变字段，含 `env`——空对象清除）
  - `task_run`：手动执行一次（入队，可置顶；不改动既定调度节奏——`nextRunAt` 不变）
  - `task_cancel`：出队（排队中）或终止运行中的那次执行（`mode=dequeue|stop`）
  - `task_remove`：按 id 删除（不可删资源目录文件）
  - `task_files`：任务资源目录读写（`op=list|read|write|delete`，路径限定目录内）
  - `task_notify`：主动推送通知（`text` 必填，可选 `title`/`id`/`at`；**免审批**——无人值守执行等不到人工审批，投递目标限定为用户已配置的通道）
- **资源文件**：每任务一个资源目录 `users/{user}/tasks/{task_id}/`（脚本型任务的工作目录即它；prompt 型作为资料目录）——`task_files` 与 REST `/api/v1/tasks/:id/files*` 可列/读/写/删（单文件上限 4 MB，越界路径拒绝），任务删除后文件保留
- **REST 管理面**（与工具同源同权，前端任务视图与第三方集成用）：

| 端点 | 语义 |
|---|---|
| `GET /api/v1/tasks?kind=&state=` | 任务清单（可按类别/运行态过滤） |
| `POST /api/v1/tasks` | 创建（body 同 `task_add`，含 `env` 任务级环境变量；可选 `originSessionId`），201 |
| `GET /api/v1/tasks/:id` | 单任务 |
| `PATCH /api/v1/tasks/:id` | 修改（body 为补丁，含 `env`——空对象清除） |
| `DELETE /api/v1/tasks/:id` | 删除 |
| `POST /api/v1/tasks/:id/run` | 手动执行（入队；body `{front?}`）→ `{task, queued, position?, reason?}` |
| `POST /api/v1/tasks/:id/front` | 排队中置顶 |
| `DELETE /api/v1/tasks/:id/queue` | 出队（取消排队中的执行） |
| `POST /api/v1/tasks/:id/stop` | 终止运行中的执行 |
| `GET /api/v1/tasks/queue` | 队列视图（额度/排队顺序/运行中；保留路径段，与 `:id` 通配不冲突） |
| `GET /api/v1/tasks/:id/runs?limit=` | 执行记录（新→旧；`limit` 非法返回 400。记录本身存于 `task-runs/{task_id}/{时间}.json`） |
| `GET /api/v1/tasks/:id/files` | 资源目录文件清单 |
| `GET /api/v1/tasks/:id/files/content?path=` | 读文件 → `{path, content}` |
| `PUT /api/v1/tasks/:id/files/content` | 写文件（body `{path, content}`） |
| `DELETE /api/v1/tasks/:id/files?path=` | 删除文件/目录 |

  写操作已有身份认证边界、不再叠加审批；按认证用户过滤（用户级归属校验），任务 id 走 32 位 hex 格式白名单，能力关闭时 503
- **用户级归属**：任务经 ToolContext/REST 绑定**当前用户**，`task_list`/`task_update`/`task_remove`/`task_run`/`task_cancel` 均校验用户归属，跨用户不可见、不可操作；`originSessionId` 记录创建来源会话（脚本结果写回目标）；任务级 `env` 随任务归属（仅本人可见可改，回显脱敏，明文存于本人用户目录）
- **执行规则**：
  - 调度器每 30 秒 tick 检查到期定时任务 + 推进队列；**入队即推进 `nextRunAt`**（执行排队不阻塞后续调度），入队与运行结束也立即触发一次推进
  - **启动加载校验 schedule/timezone 合法性**——tasks.json 被外部编辑改坏且任务 enabled 时直接禁用该任务并落 `lastError`（否则时间解析失败回退 +30s 会形成每 30 秒触发一次的热循环）；`queued` 条目重建进队列、`running` 条目标记中断
  - **入队失败（存储异常等）同样从当前时间重算下次执行时间**（记录 `lastError`/`lastStatus=error`），防 `nextRunAt` 停留在过去导致每个 tick 无重试热循环；一次性任务（@at）无法重算未来时间，失败直接停用防热循环
  - 手动执行（`task_run`/REST run/待办立即执行）不推进 `nextRunAt`（不打乱既定节奏），执行记录带 `manual` 标记
  - 任务记录保留上次执行状态/输出（输出限 4000 字符）；脚本输出写回会话消息限 8000 字符；`skipped` 不计入连续失败计数；**执行记录落盘失败只告警**（同持久化降级策略：结果与调度不得因记录写入失败而降级）
- **执行结果回写**：执行结束由调度器回调通知待办侧（`recordTaskResult`，仅在任务携带 `todoId` 时）——待办据此自动勾选完成、停用绑定任务或累计失败计次；回写以**落盘后的真值**判定（避免用陈旧计数把失败待办误判为正常）。
- **存储**：用户级 `users/{user}/tasks.json`（任务定义，随用户目录生命周期，不随会话分片清理；**不含执行记录**——记录在 `users/{user}/task-runs/{task_id}/` 下按文件落盘），服务端重启时扫描加载；落盘走 RMW + 跨进程写锁（单条 upsert 合并进磁盘真值，见下「写路径」）；**旧用户级 `cron.json` 启动时一次性迁移**（任务转 `kind=scheduled`、`runner` 取原 `type`，旧文件改名 `cron.json.migrated.bak` 保留；脚本工作目录 `cron-workspace/{id}` 并入 `tasks/{id}`），迁移仅在该用户尚无 `tasks.json` 时执行；旧会话级布局（`sessions/{s0}/{s1}/{id}/cron.json`）不再支持——启动遇之忽略（任务删除后资源目录与执行记录保留，不主动清理）
- **安全**：`task_add`/`task_update`/`task_run`/`task_cancel`/`task_remove`/`task_files` **默认需审批**（任务 = 无人值守的任意命令/会话执行，创建/修改/删除/立即执行/资源文件写入均须用户确认，服务模式下防普通用户绕过审批边界创建后门任务；**管理动作逐次确认，执行期免审批**——无人值守执行的需审批工具自动通过（见「执行免审批」，二者共用同一审批判定面：创建时的确认即执行授权的来源）；`task_list` 与 `task_notify` 免审批（`task_list` 安全模式下仍提供；`task_notify` 无人值守执行等不到人工审批，投递目标限定为用户已配置的通道——任务创建时已经过审批、不新增任意外发面，安全模式下不注册）；REST 管理面已有身份认证边界、写操作不再叠加审批）；脚本以任务所属用户身份、任务资源目录与用户环境运行（与 `sh` 工具同隔离级别，沙箱模式下脚本环境同样剔除敏感变量，见「脚本执行环境」）；安全模式下脚本执行与通知投递均跳过（记 skipped），任务调度类工具（`task_add`/`task_update`/`task_remove`/`task_run`/`task_cancel`）硬阻断；通知 URL 经 SSRF 校验防内网探测；能力整体由 `GEBAI_TASKS_ENABLED` 开关管控（默认开启，显式 false 完全不可见）
- **事件**：入队推 `event.task.queued`（含来源与队列位置）、启动推 `event.task.start`、结束推 `event.task.result`（成功/失败/跳过/超时与输出摘要、prompt 型含执行会话 id、自动停用标记）、队列变化推 `event.task.queue`（额度/排队/运行中计数）——前端任务视图与队列面板据此实时刷新（prompt 型详细过程在该会话消息流）
- **注入链路**：构造顺序为 `AgentEngine` 先建、`TaskManager` 后建（两者互相需要——调度器要 engine 执行 prompt 型任务、engine 要调度器绑定 `task_*` 工具，避免循环构造依赖）——`tasks.attach(engine)` 为**双向绑定**：调度器持有 engine，同时引擎侧 `opts.tasks` 经 `setTasks()` 回填（`task_*` 工具的 ToolContext 绑定源；单向注入不回填会使能力开启下工具仍恒报「能力未启用」）；通知依赖（fetch/飞书应用消息发送器）、子Agent 名校验器（`agentExists`）、每用户额度（`maxConcurrent`）与任务结束回调（待办联动）随构造注入
- **多实例**：定时任务到期判定与队列推进全在**进程内**（内存队列 + 任务状态持久化），同一 `GEBAI_HOME` 下多实例并存会重复调度——由「调度器主实例锁」收敛（见下）：只有主实例跑 tick 与队列，其余实例只加载任务（REST/工具读写正常）不跑调度；闲时任务的**自动进场**另受 `schedulerActive` 门控（从实例不领闲时活，用户显式手动执行仍会在接到请求的实例上执行）

#### 调度器主实例锁（同一 GEBAI_HOME 单实例调度）

闲时任务与定时任务的调度判定都是**进程内**状态（`TaskManager` 的内存队列与 `engine.busyUser()` 会话空闲判定），`tasks.json`/`todos.json` 也只在各自 `start()` 时读入内存镜像、各写各的——多实例并存时每个实例都会跑一份调度：闲时任务被重复领走（**从实例/残留实例自己没有会话 → 永远认为服务端空闲**）、定时任务重复触发、通知重复投递。故同一 `GEBAI_HOME` 下必须只允许一个实例跑调度。

- **主实例锁**（`core/schedule/primary.ts`）：锁文件 `{GEBAI_HOME}/.gebai-primary.json`（内容 `{ pid, port, at }`，`at` = 最近一次续租时刻；写入一律临时文件 + rename 原子落盘）——文件不存在/损坏/持有者 PID 已死/租约过期 → 抢占成为主实例；持有者 PID 是自己 → 续租；他人持有且 PID 存活、租约未过期 → 从实例。租约（`PRIMARY_LEASE_MS` = 5 分钟）把「PID 复用」这一不可判定项收敛为可判定：即使 PID 被无关进程复用，停止续租后租约到期即被接管
- **看门狗**（周期 `PRIMARY_WATCHDOG_INTERVAL_MS` = 30 秒，远小于租约）：主实例每周期续租；从实例在主实例死亡/租约过期后**自动接管**并启动调度（日志 `[scheduler] 已接管主实例调度`）；主实例自身错过续租被他人接管时停调度退让（日志 `[scheduler] 主实例锁已被 PID x 接管`），避免双跑
- **模式**（`GEBAI_SCHEDULER`）：`auto`（默认）=按锁判定（仅此模式启用看门狗）；`on`=强制本实例跑调度（忽略锁、不落锁——多实例同时 `on` 会重复调度，仅用于单实例部署/调试）；`off`=完全不跑调度（只提供 HTTP/WS）
- **退化为从实例时的行为**：数据照常加载（REST `/api/v1/tasks`、`/api/v1/todos` 与工具读写都可用），只是不跑 tick 与队列推进；主实例退出/被接管后由看门狗补上
- **写路径**（`core/support/json-store.ts`，`tasks.json`/`todos.json` 共用）：上述锁只收敛「谁跑调度」，**不收敛「谁写文件」**——落盘因此独立做成 **RMW（读磁盘真值 → 合并本次变更 → 原子写）**：
  - 合并基准是**磁盘真值**而非本进程镜像（待办/任务均按单条 upsert 或变更函数合并进磁盘清单）——镜像陈旧或为空不再具有破坏性（旧实现对镜像整体覆盖：多个实例各写各的，后写者抹掉前者条目；且空镜像一次写回就能把磁盘既有条目连同执行记录清空）；
  - **跨进程写锁**：`<file>.lock`（`O_EXCL` 独占创建 + 内容 `{pid,at}`；持有者 PID 已死/租约（15s）过期→抢占；等待上限 5s 超时**抛错**而不降级为无锁写）；创建与写内容之间的微小窗口用 2s 宽限期避开误抢占；
  - **无实质变更不写**（避免无谓覆盖/备份/mtime 抖动）；`forceWrite` 选项用于**结构沉降**——归一化会剥离旧版遗留字段（如任务定义里内联的 `runs`），但变更函数看到的已是剥离后的真值，恒等变更会被本条跳过、旧字段就永远留在磁盘上，故迁移路径显式要求写回一次；
  - **写前复核**：写前重取 `mtime`/`size`，期间被非协作写者（旧版本进程/手工编辑）改动则重读重试（至多 3 次，仍冲突则抛错）；
  - **原子写 + 滚动备份**：临时文件 + rename（读方永不看到半截 JSON），覆盖前把磁盘现值留存 `<file>.bak`（原内容为空/`[]` 时不覆盖可用备份）；**临时名带进程内序号**（`{file}.{pid}.{seq}.tmp`：同进程并发写者共用一个 tmp 名会互相抢文件——先 rename 的一把抽走、另一个报 ENOENT 半程失败），rename 对 Windows 短暂持有（EPERM/EBUSY/EACCES）退避重试至多 3 次；
  - 锁是**建议性**的：只有经由本模块写入的进程之间才互斥，外部编辑由写前复核兜底；同一进程内对同一文件**不可重入**（变更函数内不得再写同一文件）
- **已知边界**：锁是**建议性**（协作式）的——绕过歌白直接操作锁文件、或两个实例同时抢占一个陈旧锁，都存在短暂双主窗口（抢占后回读校验 + 看门狗每周期收敛，极端情况下最长约一个看门狗周期）；主实例长时间阻塞事件循环（超过租约）也会被接管

### 用户级待办

**用户级**待办清单（标题栏轮盘「待办」按钮打开的可拖动弹窗）：与引擎**会话级**待办（`todo` 工具，agent 自己维护的任务清单，随会话走）语义不同——用户待办属于**用户**（`users/{user}/todos.json`），跨会话/重启保留，供用户自己记事项，可**一键填入输入框**或**入队执行一次**。待办清单与任务清单是**两份独立资源**（待办不等同于任务）：任何待办都随时可手动执行（入队按序跑一次，不要求开启任何开关），只有开启 ⚡ **闲时自动执行**的条目才**绑定一个闲时任务**、由任务调度器在队列空闲且没有运行中的会话时按清单顺序串行执行（见「统一任务管理」→ 闲时类别）。能力由 `GEBAI_IDLE_TODO_ENABLED`（**默认 `true`**）开关：显式 `false` 时 REST 返回 503。

- **存储与归属**：用户级 `users/{user}/todos.json`（**数组顺序即清单顺序**），启动 `walkDir` 扫描加载 + Map 驻留；**落盘走 RMW**（读磁盘真值 → 在真值上应用本次变更函数 → 跨进程写锁内原子写 + 滚动备份，见「统一任务管理」→「多实例」下的写路径；落盘后用真值刷新本地镜像，本进程仅贡献变更）；条目字段 `id`（32 位 hex）/`user`（归属用户）/`text`/`done`/`idle`/`createdAt`/`updatedAt` + `idleTaskId`（绑定的闲时任务 id）+ 执行记录（`idleState` pending|running|done|failed / `idleAttempts` / `idleError` / `idleRunAt` / `idleSessionId` / `idleResult`）；单用户上限 500 条、单条文本上限 2000 字符（也是执行时的提示词）；开启（或重新开启）闲时标记时重置状态与计数、重新排队，取消勾选完成视作重新排队；执行失败（含超时）时闲时标记自动关闭（`idle=false` + `idleState=failed` + `idleError` 记因），重新开启 ⚡ 即重置后继续；启动加载时执行中的状态复位为 pending，并为开启 ⚡ 的未完成待办补齐绑定任务
- **REST 管理面**（前端弹窗与第三方集成共用，写操作不经审批——REST 已有身份认证边界，与任务域同姿态）：`GET /api/v1/todos`（清单）、`POST /api/v1/todos`（新增 `{text, idle?}`，201）、`PATCH /api/v1/todos`（**清单级批量重排** `{ids: [...]}`，拖动排序落库；未列出的条目按原序追加在后防丢失）、`PATCH /api/v1/todos/:id`（`{text?, done?, idle?}`）、`DELETE /api/v1/todos/:id`、`POST /api/v1/todos/:id/run`（**立即执行：入队跑一次**——body `{front?}`，返回 `{todo, queued, position?, reason?, taskId, ephemeral}`；任务能力未启用 503）；条目 id 走 32 位 hex 格式白名单（`:id` 与其子路径 `/run` 同规则），按认证用户过滤（跨用户不可见不可操作），能力关闭时 503
- **执行链路（统一任务队列）**：手动执行时——待办已有关联任务则把该任务入队（必要时恢复启用并同步文本），否则**建一条一次性普通任务**（`kind=manual`、`runner=prompt`、`prompt=待办文本`、`ephemeral=true`、绑定 `todoId`）入队，执行完自动删除（任务清单不被一次性执行塞满）；闲时自动执行 = 绑定闲时任务的调度行为。两种路径的执行结果都由任务调度器回调 `recordTaskResult` 回写：**成功自动勾选完成**（`done=true`、`idleState=done`、记 `idleResult`/`idleSessionId`，并停用绑定任务）；**失败（含超时）关闭闲时运行**（`idle=false`、`idleState=failed`、`idleError` 记因，并停用绑定任务）——不自动重试，用户重新开启 ⚡ 即重置状态、下次队列空闲继续执行。执行会话标题为 `{类别}「任务名」`（如 `普通任务「待办：xxx」`），完整过程与产物在该会话回看
- **与闲时任务的绑定联动**：开启 ⚡ → 创建（或校正）绑定闲时任务（`kind=idle`、`todoId=待办 id`、`prompt=待办文本`、`target=ephemeral`、`timeoutMs=3 小时`，旧任务在重新开启时校正超时）；待办文本变更 → 同步任务提示词；关闭 ⚡ → 删除绑定任务；待办勾选完成 → 停用绑定任务（重新手动执行时自动恢复启用）；待办执行失败 → 待办关闭 ⚡（见上）且绑定任务停用（保留绑定，重新开启时复用并恢复启用）；待办删除 → 连带删除绑定任务
- **弹窗交互（`packages/web/src/todo-pop.ts`）**：
- **弹窗交互（`packages/web/src/todo-pop.ts`）**：
  - **尺寸**：`min(680px, 94vw) × min(78vh, 860px)`（宽/高各有下限保底）——待办正文即模型提示词（可多行长文），窗口要同时容得下“读全文”与“写长文”；长文本在列表内**折叠展示**（超阈值高度截断 + 「展开全文」/「收起」），避免单条长提示词把列表打爆
  - **显隐**：**只有点击弹窗右上角 ✕ 才隐藏**——点轮盘按钮只负责打开/前置（不切换关闭）、点弹窗外不关、Esc 不关（编辑态 Esc 仅取消编辑），避免写长提示词时误触丢失；**打开状态与位置一并持久化**（`gebai.ui.todo.open` / `gebai.ui.todo.pos`），页面刷新（含 dev-reload 自动刷新）后恢复原状
  - **拖动与位置**：**标题栏拖动移动窗口**（pointer capture + 视口钳制——顶部与左右保留最小可见像素），localStorage 记忆位置，脏数据回退默认位
  - **新增/编辑**：都用**多行文本域**，行内编辑区最矮 132px（可拖拽调高）、Ctrl/Cmd+Enter 保存 · Esc 取消 · 「保存/取消」按钮可点，**点击别处不自动保存**（防误触丢失/误存）；删除走确认框
  - **新增区（底部）**：输入框**高度随内容自适应**（`autosizeAdd`：按实际内容高度实时长高，上限视口 30%，到上限后转内部滚动）——多行长提示词不被埋在固定高度里；右侧只有一个**绘制的加号图标按钮**（圆形，非文字）——空内容时置灰禁用、有内容即可点且 hover/active 给激活反馈，点击即提交；**Shift+点击加号 = 直接开启闲时自动执行**，Ctrl/Cmd+Enter 同提交（Ctrl/Cmd+Shift+Enter 同）
  - **列表项操作**：勾选完成、⚡ 闲时自动执行开关（行内显示执行中/完成摘要/失败原因）、**「▶ 执行」加入任务队列按序跑一次**（提示队列位置，进度与结果可在「任务」视图与执行会话查看）、**点击条目文本或「填入」按钮把内容写进对话输入框**（`input.value` + `autosize()` + `syncSendButton()` + `focusInput()`，沿用快捷胶囊的既有做法，不自动发送）、**拖动排序**（HTML5 drag，落点上下半区高亮，drop 后调 `PATCH /api/v1/todos` 落库，失败回滚为服务端真值）；打开期间每 15 秒静默同步状态（不阻塞执行），数据无变化不重绘（不打断滚动与编辑）
  - 纯逻辑（拖动重排/落点换算/位置钳制）拆在 `todo-core.ts` 单测（同 `press-gesture.ts` 的拆法，无 jsdom 依赖）

### 工具执行与渲染
- 工具调用时在 UI 中打印工具名和参数，参数以 JSON 格式展示（脚本类工具渲染为语法高亮代码块）
- `todo` 与 `ask` 渲染为**特别卡片**（待办清单卡 / 问答记录卡 / 计划卡；ask 按参数形态分流——options → 问答记录卡、title → 计划卡、其余（填值）走通用工具卡），不显示通用工具卡片
- 脚本执行类工具（`sh`、`py`）的输出以 Markdown 代码块独立渲染
- **执行超时**：`py`/`js` 子进程设超时上限（默认 5 分钟，可经 `timeout` 参数按次调整，上限 540 秒不晚于引擎 9 分钟兜底），超时按进程树终止；`sh` 走统一后台机制，`timeout` 为同步等待窗口（默认 60 秒、上限 120 秒），到期转后台而非终止（见「sh 执行」；同步回退路径下同值为杀进程上限）
- **用户中断的结果形态**：被中断的工具调用结果统一带 `[interrupted by user]` 标记（脚本类工具另带自身中断标记与中断前已产生的输出；未执行/未完成的调用写明「本次调用未执行」），执行中被停的调用另报**已执行时间**（「（已执行 12.3 秒）」，按取消信号到达时刻计时；未及执行的调用不报）——模型下一轮据此区分「用户停止」与执行超时、引擎错误（见「核心Agent流程 · 中断与取消」）
- **编码**：`py` 强制 UTF-8 模式（`python -X utf8` + 环境变量 `PYTHONUTF8=1`），避免 Windows 下默认 GBK 输出造成乱码/解析问题
- **解释器自适应**：`py` 执行前按 `python3` → `python` → `py` 顺序探测可用命令（`--version` 退出码判定，结果缓存），适配 Linux（多为 `python3`）与 Windows（多为 `python`/`py`），无需配置

#### 脚本执行环境

脚本工具的可用性取决于宿主机环境，分三档：

| 脚本 | 运行方式 | 宿主机要求 |
|------|---------|-----------|
| `sh` | 子进程执行 shell（POSIX 经 `bash -c`——PATH 中的 `bash`，极简环境缺 bash 时回落 `/bin/sh`；**Windows 经 PowerShell**——优先 PATH 中的 `pwsh.exe`（PowerShell 7+，支持 `&&`/`||`），缺失回落系统内置 `powershell.exe`（Windows PowerShell 5.1），`GEBAI_SH_SHELL` 可指定解释器（POSIX 同用此变量指定 bash 或其他 POSIX shell）、值 `cmd` 回落 cmd.exe 分支；PowerShell 形态以 `-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command` 承载命令，命令行前置段切 UTF-8 输出编码（`[Console]::OutputEncoding`/`$OutputEncoding`）并关进度输出，后置段 `exit $LASTEXITCODE` 带出原生命令退出码（`-Command` 默认把退出码归一成 0/1）；cmd 回落分支保持 `chcp 65001 >nul && …` 切代码页；输出自适应解码：UTF-8 优先、含替换字符回退 GBK、ANSI 控制序列剥离；Windows 子进程不设 detached——实测会导致外部程序管道输出丢失；命令成功但无输出时明确提示「无输出」，区分捕获失败；**`timeout` 参数为同步等待窗口（秒，默认 60、上限 120）——窗口内结束按同步返回，到期命令不终止、转后台返回 taskId（见「sh 执行」）**） | 有 shell（各平台自带） |
| `py` | 子进程执行 python（**`timeout` 参数（秒，默认 300、上限 540）**）；**本地模式带工具桥**（工具名即函数 / `tools.call` / `ctx`·`input` 注入，协议走回环 socket，见「py 工具桥（仅本地模式）」） | 安装 Python |
| `js` | 子进程执行 JS/TS（Bun 运行时：脚本调试模式 `bun <script>` 直跑；二进制模式 `[execPath, 入口, "exec", script]` 复用**编译进二进制/打包产物的 Bun 运行时**自执行，见「js 脚本工具」；**`timeout` 参数（秒，默认 300、上限 540）**） | **无需安装**（运行时已内嵌） |
| JS/TS（经 `sh`） | 宿主机已有 `node`/`bun` 时 `sh` 直接调用亦可（`bun run x.ts` / `node x.js`），与 `js` 工具两条路径并存 | 安装 bun/node |

- 二进制编译时已内嵌 Bun 运行时，`gebai exec` 隐藏子命令（`import.meta.main` 入口拦截 `process.argv`，**exec 段双位置路由**：argv[2] 或 argv[3]）让二进制无需宿主机安装 bun/node 即可执行 JS/TS 脚本，同时保持子进程隔离（不回到进程内执行）。子进程命令二进制模式统一**带入口段** `[execPath, 入口, "exec", script]`：编译单文件形态（`gebai.exe`）子进程 argv[1] 自动为内嵌虚拟入口，入口段仅占位（exec 落在 argv[3]）；**容器形态**（execPath=bun 跑 `dist/` 打包产物，包内各模块 `import.meta.path` 一致指向打包产物）入口段是真实 dist 入口（exec 落在 argv[2]）——缺失入口段时 bun 会把 `exec` 当子命令、script 当命令名直接报错（js 工具全挂）
- 解释器自适应：`py` 调用时惰性探测可用命令（`python3` → `python` → `py`，结果缓存），schema 描述**不做启动期可用性标注**；环境/工具链探测能力下沉 code 子Agent 的 `env_detect`（平台/架构、工具链版本、MSVC/WebView2 状态）
- **工作目录标注**：`sh`/`py` 在**非会话默认目录**执行（`workdir` 参数、`project` 参数路由或项目绑定会话）时输出末尾标注「（cwd: …）」——按 cwd 发现目标的工具（`bun test` 等）目录不对时一眼可辨（「命令在哪个目录跑的」不再靠猜）；`sh` 的退出码直接读返回结果的 `exitCode` 字段（无需 `echo $?` / `%errorlevel%` / `$LASTEXITCODE`——Windows 分支已由命令行包装段带出）
- **输出大小**：工具输出超过截断阈值走上下文保护（截断落盘），防止内存膨胀
- **输出纯净化**：`sh`/`py`/`js`/`bg_task` 的脚本输出统一经 `core/support/script-output.ts` 清洗——去 ANSI 控制序列、CRLF → LF、去行尾空白、连续空行压缩、去首尾空行（顺序、缩进、行号与错误文本等正文一律不动）：这些噪声对模型无信息量（Windows 子进程逐行带 `\r`、彩色输出带转义序列）

#### 后台任务（三类同一管理面 `bg_task`）

后台执行的长任务按 id 前缀分三类，**同一份管理面**（`bg_task` 的 status/wait/stop/list 自动识别，无需指定类型）：

| 前缀 | 类别 | 启动方式 | 生命周期与权威状态 |
|------|------|----------|------------------|
| `t` | 命令任务 | `sh async:true` | 真实子进程 + 日志落盘（**跨服务重启可见**），退出码由进程回调回写 |
| `s` | 子会话运行 | `subsession_run async:true` | LLM 循环、过程存存档；进程内，重启即中断；支持 `finish` 快速结束 |
| `j` | 通用后台任务 | 任意工具经 `ctx.bgJobs`（如 `triage_run mode=async`） | 进程内 promise；重启即丢运行记录，**权威进度=被调方自己的产物** |

- **注册表**（`core/session/jobs.ts` 的 `BackgroundJobRegistry`）：与子会话同一分层——**引擎级共享 store（`engine.bgJobStore`）+ 按作用域过滤的薄视图**（`buildContext` 注入 `ctx.bgJobs` 绑定当前会话；REST 等非会话通道用 `engine.backgroundJobs("api:<user>")`，与任何会话隔离，避免跨用户看到对方运行记录）。`start` 立即返回 `{id, status: running, ref}`；任务体经 `onProgress` 上报进度（`phase/done/total/detail`，覆盖式）、经 `signal` 接收协作中止
- **终态语义**：任务体返回值 → `done`（同时作为 `summary`）；抛错 → `failed`（记原因）；中止后正常返回 → `cancelled`（**保留已产出的 `summary`**——部分结果也要如实展示，否则用户以为什么都没跑出来）。终态记录按作用域保留最近 `BG_JOB_KEEP`（50）条，运行中的不淘汰
- **中止是协作式的**（与 `t` 杀进程树、`s` 注入收敛指令都不同）：注册表只发 abort 信号并等收尾，任务体自己在循环/批次边界退出——所以“已落盘的部分结果”始终有效，而**不必**担心硬杀导致产物不一致
- **`finish` 不适用于 `j`**：它不是模型会话、没有可收敛的对话，调用时明确拒绝并指向 `stop`（不静默当成 stop——两者语义不同）
- **上限**：`wait` 默认/上限 60 秒（与另两类同口径：超时返回当前快照，可再 wait 或改 status）；并发不单独限流（任务体自己负责，如 `sh` 的 8 并发、研判的算力约束）
- **sync 请求也可转后台（协同式降级）**：REST 的 `sync_timeout_ms` 到点后，任务被注册成 `j` 任务继续跑、调用方拿到 202 + `job_id`——「同步调用挂死」与「后台任务争抢」因此不再需要上游各自编排（见「大小模型协同」的 sync 整体超时）

#### sh 执行：统一后台机制（同步等待窗口 + 超窗转后台，管理经 `bg_task`）

`sh` 的每条命令都经 `Sandbox.spawnBackground` 起进程并登记为后台任务（`core/exec/sh-tasks.ts`，与 subsession_run 异步运行统一管理面、按 id 前缀分发）——同步调用只是「在前台等一个窗口」，不再有「同步执行超时就杀掉命令」的形态：

- **统一执行路径**（`ShTaskService.run`）：后台启动命令后在等待窗口内等待（默认 **60 秒**、上限 **120 秒**，`timeout` 参数按次调整，`shWaitMs`）——**窗口内结束**按同步语义返回（`stdout`/`stderr` 分离、退出码照旧；输出经 `spawnBackground` 的 `onChunk` 旁路捕获，与日志落盘并行，不读日志文件）；**窗口到期仍在运行**则**不终止命令**，转后台返回 `taskId` + 等待秒数 + 已产出输出尾部（`partialOutput`，stdout/stderr 各留 2000 字符）——慢命令不再因同步等待超时而白跑一场，模型也不必凭猜测重跑
- **`sh async:true`**：跳过窗口直接返回 `taskId`（结构化 `data.taskId`/`pid`），模型先处理其他任务，之后经 `bg_task` 回头处理；已知的长耗时命令（构建/测试/安装）用它省掉一次等待
- **回退路径**：安全模式（只读白名单）与 `shTasks` 服务未注入时走既有 `Sandbox.exec` 同步执行；后台启动失败（单会话并发后台任务已达 8）同样回退同步执行——命令照旧能跑，不因后台资源紧张而失败（回退下同一 `timeout` 值即该次执行的**杀进程上限**：那里没有后台可转）
- **取消语义**：等待窗口内被取消（停止按钮/任务取消经 `ctx.signal`）即按进程树终止命令并返回中断结果（`[interrupted by user]`、`exitCode` 124）；已转后台的任务不再随本次调用取消，用 `bg_task action=stop` 终止
- **记录清理**：窗口内结束且输出未超捕获上限（每流 4MB）的任务**不留痕**（记录与日志一并删除——它从未成为后台任务，留在清单里只会污染 `bg_task list` 并让 `tasks.json` 无限增长）；转后台（未结束）与输出被截断的任务保留（完整输出见日志）
- **启动**：与同步 `exec` 同规则的 shell（Windows PowerShell；`cmd` 回落分支 `chcp 65001`）/环境脱敏（沙箱用户剔除敏感变量）/Unix 进程组语义，但 stdout+stderr **合并持续写入日志文件**（WriteStream 落盘，不占内存）且不等待完成；`async:true` 下 `timeout` 参数为**任务生命周期上限**（默认 1800 秒、上限 3600 秒，防僵尸进程常驻），同步转后台的任务沿用默认生命周期（30 分钟）
- **日志流汇合**（`pipeToSharedLog`）：两条输出管道并入同一日志流，两条约束缺一都会打崩宿主进程——① `pipe(log, { end: false })`：pipe 默认 `end:true`，先 EOF 的那条流会把**共享**日志流关掉，另一条仍在途的数据随即 `write after end`；② 日志流**挂一次 `error` 监听**：`pipe` 内部写入错误无人接就是进程级未捕获异常（日志目录不可写/盘满同样触发），落盘失败只降级（该任务输出不可查 + `log.warn` 告警），任务本身照常完成。日志流的 `end()`/`destroy()` 由 `close` 统一收尾且幂等——`close` 在两条 stdio 全关闭后才发，此时收尾不丢尾
- **状态落盘**（会话 `tmp/sh-tasks/`，跨工具调用与服务重启可见）：`tasks.json` 记录（命令/cwd/pid/起止时间/退出码，原子写 tmp+rename）+ 每任务 `{id}.log` 合并输出日志；引擎按 `user:sessionId` 复用服务实例（`ToolContext.shTasks`），会话删除时释放（`forgetSession`）
- **生命周期**：记录**先落盘、后注册** `exited` 回调——命令可能瞬时退出（`echo` 类），回调先于记录落盘会读不到记录而丢弃退出码，任务永久停在 running；回调落盘回写退出码（同进程内准确）；服务重启后 pid 失活而记录无终态 → 判定 `lost`（已结束、退出码未知，日志尾部仍可读）；**本进程持有该 pid 的句柄时不做 pid 探测**（退出由回调回写，中间态探测与 close 竞态会误判 lost），句柄缺失或 pid 不符才走 pid 兜底；生命周期超限在 status/wait/list/kill 时**惰性检查**——终止进程树（本进程内经句柄精确 kill，重启后按 pid 兜底：Windows `taskkill /T`/Unix 进程组）并标记 `timed_out`
- **查询/等待/终止**（`bg_task`，id 前缀 `t` 分发到本服务）：`status` 立即返回当前状态（running/done/failed/killed/timed_out/lost）与输出尾部；`wait` 阻塞至完成或等待超时（默认/上限 60 秒——上限压到 1 分钟强制按进度轮询，超时返回当前状态可再次 wait）；`stop` 终止进程树并标记；`list` 与子会话运行、通用后台任务合并列出本会话全部后台任务
- **上限**：单会话并发运行任务 ≤ 8（`SH_TASK_MAX_CONCURRENT`，超限拒绝新任务并引导清理，同步调用随即回退同步执行）；输出尾部默认 4000、上限 20000 字符（`tail` 参数）
- **审批与安全模式**：命令本身照常过动态审批（`approval:false` 免审白名单强制）与安全模式只读白名单（`validateShCommandSafeMode`；安全模式命令走同步 `exec` 路径、不进后台）；`bg_task` 管理动作（查询/等待/终止本会话后台任务）免审批

#### 工具双输出（output / data）与输出 Schema

工具结果（`ToolResult`）携带两条相互独立的输出通道（面向「工具即函数、模型动态编程」的编排能力）：

| 通道 | 字段 | 消费方 | 说明 |
|------|------|--------|------|
| 文本输出 | `output` | 模型（进 LLM 上下文） | 面向模型分析的文本，截断保护照常生效 |
| 结构化输出 | `data` | **js 编排**（不进 LLM 上下文） | 供 js 脚本内工具函数返回值引用（`r.data.字段`）、分支判定；运行期在脚本内传递，不落盘、不占上下文词元 |

- **`Tool.outputSchema`**：声明 `data` 的 JSON Schema，经 `tool_schemas` 工具批量暴露给模型——js 编排前先查输出结构，避免逐个试调浪费往返
- **引擎兜底截断保留 `data`**（含 `subSessionArchive` 扩展字段）：截断只作用于模型可见文本
- 已提供结构化输出的全局工具：`ls`（entries）、`glob`（files/total）、`file`（info：path/type/size/isDir/modifiedAt/entries/encoding/text/extMismatch）、`grep`（**三键齐备**：matches/files/counts——不论 output 选哪种模式三键同时给出，主键为本次形态，避免调用方按 `data.matches` 读取时在 files/count 模式静默得到空数组）、`sh`/`py`（stdout/stderr/exitCode，stdout/stderr 在 data 中截断至 100k 字符）、`js`（logs/result/exitCode/calls，logs/result 截断至 100k 字符）、`fetch_url`（ok/status/contentType/error）、`todo`（todos）、`show`、`agent_load`/`subsession_run`/`subsession_run`/`bg_task`（后者均带 `Tool.outputSchema`）；**`agent_list` 未注册进全局工具表**（仅在已装载子Agent 的上下文可见，故不计入本清单）；子Agent 工具可按同一模式声明（`ToolResult.data` + `Tool.outputSchema`，如 `code_git`：status/log、`playwright_*`：读类工具给结构化结果——`content` 的 `{mode, selector?, text?, html?}`、`pages` 的 `{pages}`、`evaluate` 的 `{value}`（页面返回值的 JSON 解析结果，driver 已截断或非 JSON 时省略））
- **`tool_schemas` 工具**（批量查询）：`tools` 传工具名列表返回各工具 `{name, description, parameters, outputSchema}`（未知/未启用标记错误）；省略时返回全部已启用工具的输出结构概要（紧凑一行一个，不含输入参数）

#### 富内容块渲染

工具返回不再限于纯文本，可通过 `ToolResult.blocks` 携带**结构化的内容块**（`ContentBlock`），前端按类型逐块渲染，并**随消息持久化**——历史会话同样可查看：

| 块类型 | 渲染 |
|--------|------|
| `text` | 纯文本段落 |
| `code` | **文件内容卡**（`markdown` 语言渲染 md、其余语法高亮；`path` 附带时工具栏提供复制/原文件查看/下载，见「文件内容卡」） |
| `image` | 内嵌图片（经 `files/content` 加载会话 `tmp/` 内文件；**点击进入全屏查看器**——缩放/平移/复制/下载，与图表查看器同骨架，`show` 图表分支 `render=backend` 产出的 PNG 同样可全屏查看） |
| `file` | **文件内容卡**（统一文件展示形态，按 mime/扩展分派——图片内联点击全屏、音视频内联播放（原生控件，取数支持 Range 可拖进度）、PDF 卡内 iframe、`text/html` 卡内沙箱 iframe、md 渲染、文本/代码语法高亮、二进制占位提示下载；内容**进入视口才经 `files/preview` 按需加载**（会话相对与项目绝对路径统一入口，卡内渲染上限 4 万字符，超出截断引导原文件查看）；工具栏复制/原文件查看（弹窗）/下载。`read`/`write` 等常规工具产物为此形态（默认「弹窗查看」模式下收敛为文件链接 chip，见「文件展示方式」），`show` 的 path 分支对无法内联的类型使用，见「文件内容卡」与「内容展示（`show`）」） |
| `diagram` | **交互式图表**（Mermaid/PlantUML/D2/ECharts 四语言本地渲染，缩略图卡片 + 全屏查看器，源码查看/下载、高清 PNG 下载） |
| `diff` | **并排文本对比**（旧/新两栏行对齐，增删行红/绿着色，按文本类型语法高亮；**长行自动换行不横向溢出**——与工具卡代码块同款断行策略，降级视图同样）——`diff` 工具已移除，此块仅用于历史会话回放 |
| `html` | **HTML 页面沙箱渲染**（iframe 直接预览，全屏查看器 + 源码查看/复制/下载；域隔离 sandbox 内脚本可执行、无法访问宿主页面，见「内容展示（`show`）」） |

- 工具按产出自动生成块：`read`/`write`/`edit`（修改类以**修改后内容**产出，与读写同款；内容无变化时不产出块）依据扩展名识别图片（PNG/JPG/GIF/WebP/SVG/BMP）与图表（`.puml`/`.plantuml`→plantuml、`.mmd`/`.mermaid`→mermaid、`.d2`→d2、`.echarts`→echarts，`diagram` 块 `format` 字段携带图表语言）；`show` 工具按内容源分支产出 `diagram`/`html`/`image`/`code`/`file` 块（见「内容展示」）；`diff` 块（并排对比）仅存在于历史会话（`diff` 工具已移除）
- **markdown 内图表围栏兜底渲染**（`web/markdown.ts`）：聊天页 markdown（助手正文、推理块、计划卡、文件内容卡的 md 预览、子会话报告）中的 ` ```mermaid `/` ```mmd `/` ```plantuml `/` ```puml `/` ```d2 `/` ```echarts ` 代码块**不经 `show` 工具也渲染为图表**——渲染后把该代码块替换为图表卡片（`diagram-embed` 包裹，与 `diagram` 内容块共用同一套本地引擎/卡片/全屏查看器，源码经查看器查看）；**源码空白的块与文本末尾未闭合的围栏（流式半成品）跳过**（闭合后的下一次渲染出图，避免增长中的源码反复渲染）；非图表语言（含 `json`）按普通代码块保留
- **文件内容卡**（`code` 带文件信息与 `file` 块的统一渲染，`web/file-card.ts`）：头部（文件名 + 语言/类型徽标 + hover 渐显工具栏：**复制/原文件查看** + **常驻下载图标**（不随 hover 显隐；描边图标，文件卡头部/文件链接 chip/弹窗共用 `downloadAnchor`，经 `files/preview` 附件形式，桌面 WebView 提示下载位置））+ 内容区按类型渲染——`markdown` 语言**渲染 md**（复制复制源码、弹窗看原文）、其余**语法高亮**（块内 `language` 优先；**缺省时按 `name`、再按 `path` 扩展名回退推断**——历史卡片与展示名不含扩展名的块同样按真实类型渲染而非落 `highlightAuto`；hljs 语言集与前后端 `EXT_LANG` 三表同步，扩充 go/rust/java/kotlin/ruby/c/cpp/csharp/php/sql/lua/swift/dart/scss/less/**yaml**（旧版 `yml`/`yaml` 误映射为 `markdown`，YAML 被当 markdown 文档渲染））、图片内联点击全屏、PDF 卡内 iframe、`text/html` 卡内**沙箱 iframe**（复用 html 块的域隔离管线）、二进制类型占位提示；`file` 块内容**进入视口才 fetch**（`files/preview` 按需加载——会话相对与项目绝对路径统一入口，read/write 产物块路径为解析后真实路径，**code 项目文件的文件卡/下载同样可用**（旧版按原始参数路径经 `files/content` 解析在项目文件下 404）；历史长会话不产生全量请求），卡内文本渲染上限 4 万字符（超出截断引导「原文件」查看）；「原文件查看」弹窗（**尺寸按视口比例**：标准档 `width: min(90vw, 100%)` + `max-height: 88vh`——消息列 `--msg-max-w` = 800px 下旧值 920px 几乎与正文同宽、代码长行/网页预览横向滚动明显；窄屏（≤ 860px）遮罩 padding 收到 12px；标题栏**全宽切换**按钮（`.preview-full`，图标按钮）在「标准比例 / 铺满视口」（`.preview-card.is-full` = width 100% + max-height 94vh）间切换，偏好记忆于 localStorage `gebai.ui.previewFull`；标题栏含**常驻下载按钮**（同 `downloadAnchor`，与关闭同款图标交互）+ 图片直显（宽卡内居中，`.preview-body > img` 不误伤查看器缩放图）/PDF 内嵌/md 渲染/沙箱 html/文本高亮，与文件链接 chip 共用弹窗骨架）承载全文；超长文本截断由 show 服务端产出**单个带 `path` 的 `code` 块**（不再附独立 file 卡片，全文经工具栏获取）
- **文件展示方式（「嵌入展示/弹窗查看」，设置面板「外观」，`web/file-display.ts`）**：`read`/`write` 等文件工具（含 code 子Agent 同款包装，`card.file` 声明）的**产物文件卡**（工具卡下方的 file 内容块）在两种形态间切换——**弹窗查看**（默认，现状：产物 file 块收敛为**文件链接 chip**：📄 图标 + 文件名 + 路径（弱化单行省略）+ 下载图标，点击弹窗查看文件全文；chip 即块位置渲染于 `.msg-body`，路径取块自带的解析后真实路径，经 `files/preview` 取数）或**嵌入展示**（产物 file 块渲染为文件内容卡，进入视口按需加载内联展示）；**参数区与工具输出不受影响**（write 的 content、edit 的旧/新对比、read 的输出照常渲染——展示方式只作用于下方产物文件卡），图片/图表等视觉产物块照常内联（仅 file 块切换形态），show 等非文件卡工具的块不受影响；实时（appendToolResult）与历史（appendMsg）同构分流；localStorage `gebai.ui.fileDisplay` 仅存 `"inline"`，跨标签 storage 事件同步，切换后重载当前会话消息即时生效（渲染是结构性的）。**脚本桥（js/py）编排的产物同样分流**：桥汇集内层块时给 file 块写入来源工具名（`via`），前端按来源工具查同一份 `card.file` 声明（`fileBlockAsLink`）——桥内 read/write 与直接调用一致收敛为 chip，`show` 等主动展示工具的产物照常内联；旧记录（无 `via`）按内联渲染，不误收敛
- 图表渲染走**通道化渲染链路**：`show` 图表分支执行中引擎发布 `event.draw.render`（含 renderId + 源码 + **图表语言 `format`**）→ **Web 通道**前端**本地渲染**（按 format 分派本地引擎、**零网络请求**：mermaid → 官方 `dist/mermaid.min.js` 自包含 UMD、plantuml → 官方 `@plantuml/core` 引擎（TeaVM 编译，含内置 Graphviz 布局）、d2 → 官方 `@terrastruct/d2` WASM 引擎（浏览器构建，Web Worker 内编译渲染）、echarts → 官方 `dist/echarts.min.js` 自包含 UMD（**SSR 模式直接输出 SVG 字符串**——JSON option 纯计算渲染为静态 SVG，无 DOM 挂载、无动画，与其余三语言同一「源码 → SVG」契约）；**引擎经 `public/vendor/` 稳定文件名静态伺服**——无内容 hash，重建后 URL 不变，开发模式旧页面的动态分块 404 从根上消除，见「构建性能」）经 WS `draw.result` 或 REST `POST /sessions/:id/draw` 回传结果（**前端按 payload.format 原样分发**——`onDrawRender` 不做语言归一化，未知语言由 `renderDiagramSvg` 显式报错；echarts 曾被这里的归一化吞掉喂给 PlantUML 引擎，最新前端也报「PlantUML 渲染错误」）；**飞书通道与 `render=backend`** 由服务端**组合后端渲染器四语言全支持**（见「后端图表渲染」）——`show` 图表分支 **`render` 参数选择渲染通道**：`frontend`（**默认首选**，浏览器本地渲染 SVG 可交互缩放、零服务端开销，降低后端性能压力）/ `backend`（**服务端直接渲染成 PNG 图片**：引擎经 ToolContext `renderDiagram` 调用组合渲染器，落盘会话 `tmp/{name}.png` 并返回 `image` 内容块（**前端图片块点击进入全屏查看器**：缩放/平移/复制/下载），**仅导出/分享图片等确需 PNG 文件时使用**，四语言均支持；**前端渲染不可用（收到「画图能力受限」）时提示模型改用 backend 重试**）；**`code` 与 `path` 二选一（文件渲染）**——`path` 指定会话内已存在的图表文件（`.mmd`/`.mermaid`/`.puml`/`.plantuml`/`.d2`/`.echarts`）直接读取渲染（图表名默认取文件主名，format 未传时按扩展名推断，规范化源码落盘 `tmp/{base}.{ext}`，已生成图表文件再渲染/换通道时不重发源码），`code` 缺省仅需 `path`；**渲染成功工具才返回成功**（前端：图表块 + 落盘源码文件；后端：图片块 + 落盘源码/`.png`），渲染报错（语法错误等）把错误文本返回给模型（据此修正源码，错误信息指明失败的语言），**5 秒超时返回「画图能力受限」**（前端/飞书通道渲染端离线或引擎加载失败时降级，不阻塞任务；`render=backend` 不经前端回传，走服务端渲染器自身超时兜底）；**echarts 通道加固（实测复盘，前端渲染为默认正确通道、不自动换通道旁路）**：① 服务端**预校验 JSON**（`parseEchartsInput` 纯解析零渲染开销）——无效 JSON 立即精确报错，不白跑前端一轮；② **前端版本错位明确诊断**：前端报错引擎与请求语言不符（如请求 echarts 却由 PlantUML 引擎报错——旧版前端把未知语言静默当 PlantUML 渲染）时，返回「前端渲染器版本过旧，请刷新页面」的诊断而非误导性的源码错误；新版前端对未知语言自身也显式报错引导（见「前端本地渲染引擎」）
- **后端图表渲染（四语言）**：组合渲染器 `core/support/diagram-render.ts`（show 图表分支 `render=backend` 与飞书通道共用，浅色主题白底图）——**plantuml**：复用 `feishu-bot/plantuml.ts`（TeaVM 引擎 + 极简 DOM 垫层，串行队列）；**mermaid**：`mermaid` npm 包 + **happy-dom DOM 垫层**（固定浅色主题 `htmlLabels: false` 纯 SVG 输出；happy-dom 无布局引擎，`getBBox` 以几何属性估算覆盖——rect/path/line 坐标、文本按**全角 1.0em / 半角 0.6em 逐字符估算**（全角统一按半角 0.6em 会低估约 40%，中文标签经 getBBox→viewBox 传导导致整图偏窄、文字被裁剪），否则 mermaid 布局坍缩为 16px）；**d2**：`@terrastruct/d2` WASM（node-esm 构建，**文件路径 Worker**——bun build 无法内联：dev 直接 import 包；**二进制模式**从内嵌产物（`packages/server/scripts/build-d2js.ts` 生成 `d2js.embedded.generated.json`，gzip base64，随产物打进二进制）物化到 `{GEBAI_HOME}/vendor/d2js/{version}/` 后动态 import，版本目录幂等物化 + 旧版本清理）；**echarts**：`echarts` npm 包 SSR 渲染（`init(null, null, {renderer:"svg", ssr:true, width, height})` + `renderToSVGString()`，零 DOM）——`parseEchartsInput` 解析 JSON option（宽松 JSON：容忍注释/尾逗号）或 `{"option":…,"width":…,"height":…}` 信封（尺寸钳制 200-4000，默认 960×600），注入 `animation:false`（SSR 静态输出必须）；**标题/图例防重叠**（echarts 6 图例默认在底部，模型按 v5 习惯显式 `legend.top: 0/'top'/小数值` 时置顶图例与顶部标题必然压字——纵向冲突时图例自动下移到标题底边之下，越界时联动下调未显式设置的 `grid.top`，规则见常量表「ECharts 标题/图例避让」，前端 `parseEchartsOption` 同规则）；**echarts 求值环境隔离**（zrender 环境探测在模块求值期完成，误判浏览器环境后（浏览器路径文本测量需真 canvas，垫层不支持）一旦垫层卸载全局 window 即抛 `window is not defined`；而 PlantUML/happy-dom 垫层在模块作用域安装全局 window/document 且不还原，谁先加载会决定判定结果——模块顶部对全局作**临时清理后求值（TLA）并立即还原**，使环境判定与加载顺序解耦；diagram-render.ts 经引擎与飞书桥接惰性 import，不增加启动开销）；**SVG → PNG** 共享栅格化（`@resvg/resvg-js`，2x 超采样、1600×2400 上限）：根元素规范化（百分比尺寸显式化、**负原点 viewBox 平移归一**——resvg 对负原点 viewBox panic，以 `translate` 包裹内容平移进正象限且不包裹 `<style>`/`<defs>`/`<title>`）；**全局环境切换**（共享进程全局 DOM/Worker，统一走单一串行队列）：happy-dom 垫层与 PlantUML 垫层相互覆盖全局 document——mermaid 渲染前强制重放 happy-dom 垫层、plantuml 渲染前重放其垫层；**`globalThis.window` 仅临时存在**（mermaid 导入/渲染期临时注入，plantuml 渲染后即删）——实测 Bun 的 `node:worker_threads` 在全局 `window` 存在时 Worker 启动挂起（D2 渲染器依赖）；错误按语言包装回传（`Mermaid 渲染错误：…`/`D2 渲染错误：…`/`ECharts 渲染错误：…`），各引擎按需加载（echarts 在模块求值期以隔离环境加载，不拖慢启动）
- 图表配色：**跟随 UI 主题明暗**（前端 `diagram.ts` 与工作台 `files/viewers.ts` 同规则，色板模块 `diagram-theme.ts` 统一供给）：亮色主题（--bg 亮度 ≥ 0.35）用引擎自带默认亮色，**与历史行为一致零注入**；暗色主题注入由 UI 主题 CSS 变量（--bg/--text/--accent/--accent-2/--border-strong 等）构建的暗色色板，文字/连线/节点底色与 UI 主题对齐；主题切换（含人民币面额/亚克力亮暗）监听 `gebai:theme-change` 重绘已挂载的图表卡片与查看器，同色板重绘为 no-op；渲染缓存键 = 色板 key + 源码（主题切换自然分键，切回零开销）。**用户显式配色与主题注入色的冲突**（一整类问题）统一用可读文字色原语处理：`readableTextOn(bg, prefer)` 从底色向目标端点插值取首个 WCAG ≥ 4.5 的色（保色相，避免纯黑/纯白生硬），亮底阈值 `BRIGHT_BG_LUMINANCE` 与主题明暗判定同口径；原则是**只改文字不改用户选的底色**——配色是设计意图，可读性是硬约束。当前覆盖：Mermaid `classDef`/`style` 浅底节点、PlantUML 节点显式 `#Color` 浅底（两个引擎的后处理均在净化/注入 DOM 前完成，幂等）。ECharts 的 `darkMode` 只是组件 model 字段，**在 ECharts 6 的 SSR 链路上不生效**（实测仅改 `darkMode` 的两次渲染输出逐字节相同；仅第二参数 `init(null, "dark", …)` 能切到暗色，但内置暗色系列色写死与 UI 主题无关，故不用）。各语言暗色机制（全部实测验证）：① **PlantUML**：skinparam 暗色集注入在 `@start` 行之后，覆盖时序（含 boundary/control/entity/queue 特殊参与者形态，各有独立 skinparam）/类/对象/状态/活动/**用例**/组件部署/包围容器/思维导图/WBS/note/legend 全图型；**skinparam 覆盖不到的残留走 SVG 后处理兜底**（`remapPlantUmlSvg`，内部再接 `fixPlantUmlBrightNodes`）——生命线虚线 `LifeLineBorderColor` 在 TeaVM 版无效、类图/ER 图圆点图标无对应 skinparam、**JSON/YAML 数据图为独立渲染器完全不吃 skinparam**、各元素默认黑字黑线；后处理映射：白/浅底→画布底/节点底、黑字黑线→正文色/border（幂等，用户源码显式色不在表内不受影响）；**用户显式节点色的可读性修正**（`fixPlantUmlBrightNodes`）：用户可对节点显式指定颜色（`rectangle "x" #LightYellow`/`usecase … #fff`/`class … #orange`），引擎按用户色填底、再用 skinparam 文字色写字——浅底配浅字对比度可低至 1.1（实测 `#LightYellow` + `#F2F2F5`，节点文字完全不可见）；修法按同级 g 配对形状与文字，亮底时把该组文字改为可读深色（只改文字不改用户选的底色）；② **Mermaid**：`theme: "dark"` + themeVariables 色板映射（含甘特图 `titleColor` 段标题/饼图 pie1..12 扇区等分图型变量；**显式颜色回退 + 可读性最终校验**（`hasExplicitColors` / `enforceReadableText`）：源码含显式颜色（`classDef`/`style`/内嵌 color/`init themeVariables`/节点 `#Color`/`skinparam *Color`/`!theme`/ECharts `backgroundColor`·`textStyle`·`color`/D2 `style`）时**不注入主题色**，回退引擎默认渲染 + 白底（引擎默认配色本身是浅底深字，必然可读；漏检只退化为主题注入、不会更差）；无论注入与否，渲染后统一过可读性最终校验——扫描「文字色 vs 所属形状底色」实际对比度，低于 4.5 就在 SVG 内注入内联覆盖（可读性优先，用户显式 `color` 也覆盖）。**用户自定义配色的可读性修正**（`fixMermaidBrightNodes`）：模型/用户常用 `classDef`/`style` 指定**亮色底**（如 `classDef done fill:#e8f5e9,stroke:#1b5e20`），而注入的主题文字色是浅色——亮底浅字对比度低至 1.0，节点文字完全不可见（实测复现）；修法为渲染后扫描每个节点形状的**实际底色**（含 `!important` 内联样式），亮底节点（相对亮度 ≥ 0.35）逐个按其唯一 g id 注入深字覆盖规则，文字色取该节点的 `stroke` 色（用户选定的同系深色，对比度最佳且贴合原设计），无 stroke 时按底色压暗生成，幂等；暗底节点不受影响；；**固定 `htmlLabels: false` 纯 SVG 文本标签**——默认 true 时标签渲染为 HTML 元素包于 `<foreignObject>`，会被注入 DOM 前的 DOMPurify SVG 净化剥离导致「有框无文字」，前端与后端渲染器一致；**`suppressErrorRendering: true` 抑制内置错误渲染**——默认渲染失败会在 `document.body` 遗留「Syntax error in text」错误图标容器且无法关闭，抑制后错误统一经 Promise 拒绝由调用方处理；主题为全局状态，按色板 key 重新 initialize）；③ **D2**：浏览器版仅内置 8 个亮色装饰主题（themeID 9+ 报 theme not found、vars dark-id 无效，实测），渲染后**颜色重映射**（fill/stroke 属性与 `<style>` 内 CSS 声明同步替换——D2 色板定义在 fill-N 系类规则里，CSS 优先级高于 presentation 属性，只换属性会被原 CSS 白底压回，实测踩坑；不在映射表的色原样保留，用户 style 指定色不受影响）；④ **ECharts**：option 浅合并注入 backgroundColor/textStyle/系列色板/坐标轴色（用户显式字段不被覆盖；轴标签默认灰 #54555a 在暗底对比不足，注入 axisLabel/axisLine/splitLine 默认色）；**暗色下 PNG 导出/剪贴板垫色板底色**（SVG 透明区域落在白色画布上会呈现暗底錊白）；**标题/图例防重叠**（`fixEchartsLegendOverlap`，前后端解析入口统一应用：置顶图例与顶部标题纵向冲突时自动下移避让，见常量表「ECharts 标题/图例避让」）；**JSON option 无法表达函数**（formatter 等用字符串模板 `"{b}: {c}"`，echarts 原生支持；前后端解析一致：宽松 JSON 容忍注释/尾逗号，信封指定画布尺寸）；SVG 按源码+色板缓存，同图同主题重复渲染零开销；引擎空闲预热（PlantUML + mermaid + echarts；D2 的 8MB WASM 开销大且架构图频率低不预热）避免首次调用超时；渲染 SVG 注入 DOM 前经 DOMPurify 净化（PlantUML 支持 `<html>`/`<img>` 嵌入，防脚本注入）
- 消息 `blocks` 字段持久化于 `chat.json`，历史会话加载时以同一渲染管线呈现，保证「历史可看」
- 工具执行事件（`event.tool.result`）携带 `blocks`，UI 实时展示图片/图表等产物

#### 内容展示（`show`）

全局工具 `show` 是**向用户展示内容的统一入口**（原 `draw`/`render_html`/`show_file` 三工具合并），参数面统一为 `name`（展示名）/ `format`（内容格式，`content` 必选、`path` 可选）/ `content`（内容）/ `path`（已有文件），**内容与路径二选一**（`content` 直给内容、`path` 指向已有文件；同时传或都缺均立即报错引导）；`format` 值域 = 四种图表语言 + `html`（页面预览）+ 三种文本型内容（`markdown`/`code`/`text`），`content` 的解释方式完全由它决定，`path` 未传时按文件真实类型/扩展名推断；另有**可选参数** `language`（`code` 分支的高亮语言）与**可选微调参数** `render`（图表渲染通道）/ `width`/`height`（HTML 预览尺寸），不参与内容源选择；产物均落盘会话 `tmp/` 并返回对应内容块；`card.args="block"` 声明（调用不显示通用工具卡片，内容块直接渲染）；各分支均默认无需审批。

**① 图表交互式创作（`content` + 图表语言 `format`，或 `path` 指向图表源文件）**——专门支持**结构化图表的交互式创作**，**四种图表语言由工具描述/参数说明内置选择指南指导模型按需选择**（四语言对比如下）：

| 图表语言 | 首选场景 | 典型触发词 |
|----|------|------|
| **Mermaid** | 通用场景首选：流程图/时序图/状态图/甘特图/用户旅程、Markdown 文档嵌入（README/Wiki/博客）、简单架构，语法最简洁 | 流程图、时序图、状态图、甘特图、文档 |
| **PlantUML** | UML 与严谨建模首选：类图/组件图/部署图/用例图/活动图/ER 图等标准 UML（支持全部 14 种），表达复杂继承/依赖/关联，语义严谨 | 类图、UML、组件图、部署图、用例图、ER图、继承关系、软件设计 |
| **D2** | 美观架构图与对外展示首选：系统架构/云架构/网络拓扑/微服务，PPT/汇报/技术分享，默认布局最现代化 | 架构图、系统架构、云架构、微服务、汇报、美观 |
| **ECharts** | 数据可视化与统计图表首选：柱状/折线/饼图/散点/雷达/仪表盘/热力图/地图等，动画级视觉效果（SSR 静态输出），适合数据报告 | 柱状图、折线图、饼图、数据图表、统计、报表、Dashboard |

- 组合场景指引（内置在 `format` 参数说明中）：系统设计文档 = PlantUML 类图/组件图 + Mermaid 流程图；架构汇报 = D2 全景架构图 + PlantUML 详细组件图；数据分析 = ECharts 统计图表
- 工具参数：**`format` 必选**（content + 图表语言模式）：`mermaid`/`plantuml`/`d2`/`echarts`——**缺失/非法立即报错列出可选值，不静默回退 plantuml**（实测复盘：模型漏传 format 时 ECharts JSON 被当 PlantUML 渲染，「PlantUML 渲染错误」的报错误导模型连续多轮失败；`path` 模式扩展名可推断时免传）；Mermaid 直接给图定义、PlantUML 源码无需 `@startuml`/`@enduml` 包裹自动补全、D2 直接给声明式文本、**ECharts 给 option 的严格 JSON**（键名与字符串一律双引号，不支持单引号/裸键名/`…`省略号缩写，容错 `//` 注释与尾逗号；值禁止函数，格式化用字符串模板如 `"{b}: {c}"`；可选信封 `{"option": {...}, "width": 960, "height": 600}` 指定画布尺寸，默认 960×600；图例默认在画布底部，与标题同顶冲突时渲染器自动下移避让，无需手动设置 `legend.top`）；`path` 指向已有 `.mmd`/`.puml`/`.plantuml`/`.d2`/`.echarts` 文件直接渲染（图表名默认取文件主名、format 按扩展名推断，避免重发源码；显式传 `format` 可按指定语言渲染任意文本文件）；可选微调 `render` 参数：`frontend` **默认首选**（浏览器本地渲染、零服务端开销）、`backend` 服务端渲染成 PNG 图片（仅导出/分享图片场景，四语言均支持）；配套工具 `read`/`write`（参考素材/落盘说明）、`todo`（拆解步骤）
- 流程：澄清需求 → 选语言 → `show` 生成初始图表（实时渲染——Web 前端本地渲染/飞书桥接后端渲染成图片/`render=backend` 服务端渲染成 PNG 图片，渲染成功才返回成功，报错会收到错误信息）→ 展示并说明图意 → 依用户反馈反复迭代（增删节点、改连线、调整布局）
- **PlantUML 布局规范（防图表杂乱）**：① show 工具描述/参数说明显式要求模型控制布局——流程/时序类图表声明方向（横向流程用 `left to right direction`，分层架构保持默认纵向），**不得靠逐条连线上写 `-down->`/`-right->` 硬控全局布局**；② 密集图表间距由 **⑤布局兜底注入**承担——工具描述**不再**建议手动设置 `skinparam ranksep 80`/`nodesep 40`（自动注入已覆盖，手动建议与之矛盾且冗余）；③ 关系紧密的节点用 `together { … }` 保持相邻；④ 控制规模：单图节点 ≤20 个，架构图按层拆包（package），跨层连线过多时拆成多个图表分别展示；⑤ **布局兜底注入**：源码未显式设置 `skinparam ranksep`/`nodesep` 时，服务端（`injectPlantUmlLayout`）与前端（`plantuml-layout.ts` 同规则）在渲染/落盘前自动注入默认间距参数（仅 `@startuml` 类图；mindmap/wbs/gantt/salt/json/yaml 等布局由结构决定不注入，已显式设置者尊重用户布局）；Mermaid/D2 的布局由各自语言的结构决定，不做注入
- - UI 交互：`diagram` 块在消息流中渲染为**缩略图卡片**（**铺满卡片宽度**的较大尺寸预览 + 文件名——svg 按 `width: 100%` 放大至卡片宽（mermaid 内联 `max-width` 限布局宽、d2 无宽高属性默认 300px 导致预览过小，`max-width: none !important` 压过内联样式），高图限 60vh（横排 48vh）防撑爆消息流；**始终默认渲染**（低性能模式同样自动渲染）；**渲染失败不在主页面暴露错误细节**——仅显示「图表渲染失败，点击查看详情」占位（技术信息进查看器/控制台）；点击进入**全屏查看器**；**同一消息内连续多个图表横排**展示——均分宽度自动换行，节省纵向空间）；**块级工具结果封段**：show/diff 等 `card.args="block"` 工具结果到达时封存当前文本段——图表卡片独立展示，**画图后的输出另起新卡片显示在图下方**（防输出继续追加到画图前的那张卡片、在图上方滚动）；查看器**默认自动适应视口**（初始放大至 125% 显示更清晰、图不超屏）并**居中显示**（缩放基准取 SVG 逻辑尺寸——优先 viewBox，mermaid/d2 渲染时已显式化根 `width/height`（`width="100%"`/无宽高属性的 SVG 在 flex 容器内 `getBoundingClientRect` 解析为 0×0 导致查看器空白）），单栏标题栏（文件名 + 图标工具栏：还原视图、−/＋ 锚点缩放、拖拽平移（zoom 盒 `flex:none` 必需——flex 行容器子项默认可收缩，被压回容器宽后水平滚动区消失、放大溢出被 `overflow:hidden` 裁掉，横向永远拖不动）、复制图片、下载图片、查看源码；**缩放锚点**：滚轮**严格以光标为锚点**（溢出态连续缩放指针下的内容点不动）、按钮以视口中心为锚点——锚点坐标经**量测 zoom 盒实际屏幕位置**（`getBoundingClientRect`）推导，不能按 `scrollLeft+视口偏移` 计算：flex 居中 margin/padding 下滚动几何原点与视口原点不重合，「居中适应 → 溢出可滚动」过渡时锚点会跳（指针下的内容点漂移）；无滚动余量的轴自然钳制保持居中）；**下载图片为 3x 超采样高清 PNG**（SVG 矢量无损放大，本地绘制，无网络）；源码弹窗内支持**一键复制**与**按图表语言的扩展名下载**（`.mmd`/`.puml`/`.d2`/`.echarts`）
- 审批：`write` 需审批；图表分支默认无需审批，降低创作摩擦

**② HTML 页面渲染（`content` + `format: "html"`）**——生成 HTML 页面**直接在聊天界面内渲染展示**（类似图表卡片，但面向网页原型/数据报表/卡片徽章/可视化组件等页面型产物）：

- **参数**：`content`（HTML 源码，完整文档或片段均可，自动补全为完整页面）+ `format: "html"`（必传——内容类型由 format 判定）；`name`（可选，页面标题/文件名，默认 `page`，自动剥离 `.html` 后缀）、`width`/`height`（可选微调，预览尺寸 px——由模型按内容设计显式指定，如移动端页面窄高、宽表格页面宽高；非法值（非正数/超上限）忽略回退默认；已有 `.html` 文件经 `path` 分支直显（html 块，显式尺寸同样生效），不重发源码；`path` + `format: "html"` 亦可把任意扩展名文件按页面渲染）
- - **产物**：HTML 落盘会话 `tmp/{name}.html`（UI 文件面板可见、模型可经 `read` 读取），同时返回 `html` 内容块（`{ type: "html"; html; name; width?; height? }`）随消息持久化，历史会话以同一渲染管线重放
- - **UI 渲染**：消息流中渲染为**预览卡片**（标题栏 + 工具栏：全屏查看/查看源码/复制源码/下载 `.html` + iframe 预览，点击预览或标题进入**全屏查看器**，查看器内可查看源码/下载）；与图表分支同属 `card.args="block"` 声明——不渲染通用工具卡片，调用与结果直接呈现 html 块（无内容块时以输出文本兜底）；**预览宽度机制**：iframe 默认**固定 100% 铺满消息流宽度**，**不参与任何内容宽度反馈**（无尺寸上报、无 resize 回路，杜绝越缩越窄）；**预览高度机制**：无显式 `height` 时 iframe 高度 = **会话区域高度的 2/3**（`.chat-wrap` 声明 `container-type: size` 作为尺寸容器，iframe 用容器查询单位 `height: 66.67cqh` 随窗口尺寸自动重算；不支持 cqh 的旧浏览器回退 480px）；含 HTML 卡片的消息脱离 `.msg` 的 `fit-content` 宽度计算（iframe 为 replaced element，其 `width:100%` 在 max-content 计算中会退化回内置 300px 宽把消息压窄）——消息占满消息流宽度且 `.msg-body` 以 `flex:1` 撑满，实测锁定 300px 的塌缩被根治；模型显式传 `width`/`height` 时按指定值渲染（宽于消息流的显式宽度由卡片横向滚动承载）
- - **安全（域隔离）**：模型生成的 HTML 视为不可信输入，渲染于 **sandbox iframe（`allow-scripts`，不含 `allow-same-origin`）**——脚本可执行但运行在**隔离 opaque origin**：无法访问宿主页面 DOM/Cookie/存储（跨域访问抛 SecurityError）、无法顶层导航（无 `allow-top-navigation`）、弹窗（无 `allow-popups`）、表单提交（无 `allow-forms`）、下载（无 `allow-downloads`）；嵌套 iframe 继承沙箱限制；`referrerpolicy="no-referrer"` 防 Referer 泄漏；**CSP meta 注入** srcdoc 文档 head（`default-src * data: blob:; script-src 'unsafe-inline' 'unsafe-eval'; style-src 'unsafe-inline'`）放行内联/外部脚本、样式与网络，页面自带 CSP 与注入策略取交集只会更严
- - **脚本说明**：脚本在沙箱内正常执行（交互/动态效果/外部脚本均可）；跨域网络请求受浏览器 CORS 与第三方 Cookie 限制（opaque origin 请求带 `Origin: null`，仅 `Access-Control-Allow-Origin: *` 响应可读，SameSite 默认 Lax 不携带跨站 Cookie）；需访问宿主页面数据/同源 API 的诉求无法满足
- - **主题跟随**：iframe 预览区边框与背景走主题变量（`--border`/`--bg-inset`），内容页无自带背景时与主题一致（避免亮色突兀块）；srcdoc 注入 `style#gebai-theme` 同步**主题变量集**（`--bg`/`--text`/`--accent`/`--border`/`--radius-*`/`--font-mono` 等 20 个核心变量写入 `:root`，工具/预览页面可用 `var(--x)` 引用主题色）与背景/滚动条色值，并设置 `data-theme` 属性（支持 `[data-theme]` 分支样式）；初始快照注入，宿主主题切换经 postMessage `gebai-host/theme` 广播动态更新——**`gebai:theme-change` 事件驱动立即广播**（免 500ms 轮询延迟，轮询兜底异步加载）；内容页自带背景/变量时其自身规则（文档中靠后）覆盖注入值，不破坏内容设计
- **审批**：默认无需审批（仅展示 + 写入会话 `tmp/`，与图表分支同级）

**③ 文本型内容（`content` + `format: "markdown"|"code"|"text"`，或 `path` + 显式 format）**——把文档/源码/纯文本**直接呈现给用户**（长文档、源码交付不必堆进回复正文）：

- **三种格式**：`markdown` 按 markdown 渲染为排版文档（标题/列表/表格/引用/代码块）；`code` 为源码/配置文件，可选 `language` 指定高亮语言（`typescript`/`python`/`bash`/`json`/`yaml`/`sql` 等，入参归一化为小写，缺省按 `path` 的真实文件扩展名推断、推断不出时由前端自动识别）；`text` 为纯文本（日志/命令输出/配置片段），**不做语法高亮**；`name` 缺省时产物主名取 `doc`（markdown）/ `code` / `text`
- **产物**：落盘会话 `tmp/{主名}-{内容哈希8}.{扩展名}`（`markdown` → `.md`、`text` → `.txt`、`code` → 按语言映射的扩展名（`core/base/diff.ts` 的 `extForLang`，未命中回落 `.txt`））——UI 文件面板可见、模型可经 `read` 读回、可下载；命名带内容哈希，同内容幂等、异内容各存（历史消息里的产物引用不被后续同名产出覆盖）
- **内容块**：统一 `code` 块（`{ type: "code"; text; language?; path; name }`）——`markdown` 由前端文件内容卡**渲染为文档**（而非源码高亮）、`code` 按语言语法高亮并标语言徽标、`text` 走**纯转义呈现**（不自动高亮、不标语言徽标）；超 4 万字符（`SHOW_TEXT_MAX_CHARS`）块内截断，全文留在产物文件（与 `path` 直显同口径）
- **`path` + 显式 format**：可把任意文本文件按指定格式解释（不限扩展名，如 `.txt` 按 `markdown` 渲染/任意文件按 `code` 高亮）；未显式传 format 时仍按扩展名推断（既有行为）
- **通道与审批**：文本型分支不依赖前端渲染能力，实时 / 飞书 / REST 全通道可用（产物路径随输出文本给出）；默认无需审批（仅展示 + 写入会话 `tmp/`，与图表/HTML 分支同级）

**④ 文件主动展示（`path` 内容源）**——把文件**直接展示给用户**（交付产物 / 需要用户过目的文件）——**按文件类型产出直显内容块，内容在消息流内联呈现**：

- **直显矩阵**：图片（PNG/JPG/GIF/WebP/SVG/BMP）→ `image` 块（内联 `<img>` + 点击全屏查看器）；图表源文件（`.puml`/`.plantuml`/`.mmd`/`.mermaid`/`.d2`/`.echarts`）→ 走①的渲染验证管线（format 按扩展名推断，与 `content` 模式同一闭环，重新渲染/换通道不重发源码）产出 `diagram` 块；`.html` → `html` 块（沙箱 iframe 页面预览，显式 `width`/`height` 生效）；文本/代码（txt/md/csv/json/yaml/log/常见代码扩展名，≤512KB）→ `code` 块（附带 `path`/`name`，文件内容卡渲染：md 渲染、源码语法高亮、工具栏复制/原文件/下载；超 4 万字符截断展示，全文经工具栏下载或「原文件」弹窗获取）；**无扩展名/dotfile**（`LICENSE`/`Makefile`/`Dockerfile`/`.gitignore` 等）→ **按内容探测判定**（无 NUL 字节、替换字符与控制字符占比 ≤1% 视为文本）后同样走 `code` 块内联，否则回落 `file` 块——这类文件没有扩展名可信、展开白名单永远追不全，只能按内容判定；探测**刻意只覆盖无扩展名/dotfile**（空扩展名没有「类型承诺」，探测零风险）：`tiny.pdf` 这类有扩展名的格式即使内容恰好是纯 ASCII 也仍按扩展名给卡片（探测它们会把二进制格式误判成文本，已有用例固化）；**语言/类型一律按真实文件路径（`abs`）推断，不用展示名 `name`**——`name` 惯例不含扩展名（参数描述即写「不含扩展名」，实测历史调用 `{path:"xxx.md", name:"调研报告"}`），据它推断 language 为空 → 前端只能 `highlightAuto`，markdown 被当源码高亮而不是渲染成文档（yml/json/ts 等同样丢高亮）；**无法内联的类型**（PDF/压缩包/Office 等）→ `file` 块（文件内容卡按 mime 分派渲染，内容进入视口才按需加载）；**音视频**（`video/*`/`audio/*` 或扩展名 .mp4/.webm/.mov/.m4v/.mkv/.mp3/.wav/.m4a/.aac/.ogg/.flac）→ 同走 `file` 块，卡内渲染浏览器原生 `<video controls>` / `<audio controls>`（取数经 `files/preview`，**服务端 `Response(Bun.file(…))` 自动处理 Range** → 206 + `content-range`，进度条可拖动；不自动播放，`preload="metadata"` 只拉元数据）
- **与普通工具的语义区分**：`show` path 分支是「给用户看」的主动通道（内容直接呈现）；`read` 等常规工具按既有行为返回产物块（文件类产物为查看/下载卡片，不自动展开内容）
- **参数**：`path`（与 `content` 二选一，会话 `tmp/` 相对路径（前缀可省略）；本地模式可为工作区相对/绝对路径）、`name`（可选，展示文件名，默认取文件主名；**仅影响展示名与产物文件名，不参与类型/语言判断**——类型一律按真实文件推断）、`format`（可选，显式指定时按该格式解释文件内容：图表语言 → 渲染该文件、`html` → 页面预览、`markdown`/`code`/`text` → 文本型解释，不再按扩展名推断）、`language`（可选，与 `format:"code"` 搭配的高亮语言；未传按真实文件路径扩展名推断）
- - **路径处理**：会话 `tmp/` 内文件**直接引用**（零复制）；会话外文件（本地模式工作区/绝对路径）**复制一份**到会话 `tmp/shown/{主名}-{内容哈希8}.{扩展名}`（内容哈希命名，重复展示复用同一副本；上限 100MB，超出引导改为告知路径）后引用——前端文件接口只服务会话 `tmp/`，复制保证可见性与文件面板留存。**归属判定用会话 `tmp/` 真实绝对路径**（`sessionPath` 拼接；项目绑定子Agent 的 `resolvePath` 基准是项目根，不能作判定依据），复制目标同样直接写会话 `tmp/` 绝对路径（绕开项目根基准，保证落在真实会话文件区）
- - **mime 推断**：按扩展名映射（图片 + PDF/文本/CSV/JSON/HTML/Office/音视频等常见类型），驱动前端预览入口形态（图片内嵌/PDF 内嵌查看/文本高亮/下载提示）
- **审批**：默认无需审批（只读 + 复制进本会话 `tmp/`，与图表分支同级）；`card.args="block"` 声明（调用不显示通用工具卡片，内容块直接渲染）

**分支门控（合并型工具的通道能力校验）**——`show` 不声明工具级 `interaction`（全模式可见），引擎把当前任务的 `interactionMode` 注入 ToolContext（`ctx.interactionMode`），工具在分支内校验通道能力：`html` 分支（`content` + `format: "html"`，或 `path` 指向 `.html`/显式 `format: "html"`）仅 `realtime`（飞书/REST 下明确报错「当前通道不支持 HTML 页面预览」，引导改用文字描述或产出 `.html` 文件后经 `path` 交付）；图表分支 `realtime` → 前端渲染、`multi_turn` → 飞书后端渲染（同一 `event.draw.render` 通道）、`none` → **不空等 5 秒超时**直接引导 `render=backend`（backend 为服务端能力，无交互模式可用）；`path` 文件分支全模式可用（REST 消费者仍可拿到内容块）；文本型分支（`markdown`/`code`/`text`）不依赖前端渲染，全模式可用。未注入 `interactionMode`（测试桩/无引擎环境）不做分支门控。

### 上下文保护

对话上下文（消息历史 + 工具返回）超出 LLM 上下文窗口时，自动执行压缩；也支持用户主动压缩。

#### 自动压缩

- 触发条件：**窗口剩余不足以支撑一次回复**时自动触发，无需人工干预（不以窗口百分比判定——真正约束是「留给输出的空间」，输出上限由模型与配置（`GEBAI_LLM_MAX_OUTPUT_TOKENS`）决定，并非窗口的固定比例）
- **压缩四原则**（用户口径，逐条落到 `planCompactRange` + `store.compactMessages`）：
  ① **近消息是滑动窗口**：最近 `COMPACT_WINDOW_MESSAGES`（12 条，任意角色；上限为历史一半，避免短会话压不动；`GEBAI_COMPACT_WINDOW` 可调）**永不进**压缩区间（原样保留），随新消息自然向前滑动；
  ② **远消息用模型压缩、原消息完全抛弃**：区间取最早的连续一段（远的先压），区间内**除系统提示词以外**的消息（含用户输入、assistant、tool 结果）全部由摘要替换并**从上下文移除**——不残留原文（会话记录 chat.json 同样只留摘要，原文不另存）；摘要恒为一条且置于消息数组**最前**（模型先读历史摘要再读近期原文，与近期消息不交错：越旧的信息越靠前）；
  ③ **压到目标水位即停**：上水位（触发）= 剩余 < 输出预留（窗口 - 输入 < reserve）；下水位（目标）= 窗口 × `COMPACT_TARGET_RATIO`（缺省 40%，`GEBAI_COMPACT_TARGET_RATIO` 可调，合法区间 0.1~0.9；且**不高于触发线**——比例定得比触发线还高时永远压不到，夹到触发线即停）——需腾出量 = 真实基线 − 目标输入，压到目标即停；可压消息不够（窗口外全压完仍高于目标）时压满窗口外，不足部分交给迭代压缩与溢出恢复；
  ④ **系统提示词不要压缩**：压缩区间只含可压缩消息（`isCompressibleMessage`）——**角色 system 的消息一律不进区间**，夹带时由 `compactMessages` 原位保留（主 system 段消息、子Agent 装载提示词均不被摘要替换）；缓存友好前缀请求会把它们作为主循环同前缀一并发给模型（前缀缓存匹配所需，模型只是重看一眼已看过的内容），但会话记录里的这些消息一字不改、位置不变
- **上下文占用口径：只认模型服务返回的真实大小**（`input_tokens`，含 system 提示词与工具 schema，即「真上下文」）——压缩判定不依赖任何估算，估算容易误判（chars/4 对中文低估 2~4 倍、工具 schema 与图片块又难以折算），一律以接口真值为准：
  - **跨 run**：上次任务最后一次调用返回的真实 `input_tokens` 持久化为基线（`SessionData.ctxInputTokens`）；下次 run 基线本身已致剩余不足一次回复即先压缩（本次调用只会更大），不做增量估算
  - **任务中途**：每轮调用返回的真实 `input_tokens` 使窗口剩余 < 输出预留时压缩最早历史（`makeContextRoom`），长任务不再等接口拒绝
  - **溢出恢复**：接口以上下文长度错误拒绝（4xx = 真实大小的权威信号）时，压缩最早历史后重试（至多 3 次）——无基线会话首次调用即超窗也能收敛，不再任务失败
  - 接口不返回 usage（或压缩替换消息导致索引锚点失效）时不估算预判，由本次调用的真值 / 溢出恢复接管；估算仅用于**压缩量规划**（`estimateMessageTokens`：需腾出多少空间→压缩多少条；真值口径的需腾出量按 `estTotal/基线` 比率**折算为估算口径**再逐条累计——估算实测低于真值 20%~30%，不折算会压过头或压不到目标）与会话列表 ctxTokens 展示的增量补足（**估算口径含工具 schema 段**：schema 不在 messages 里却真实计入 input_tokens，由 `estimateSchemasTokens` 补齐——压缩重建后推送与持久化的估算值因此不再系统性偏低、下一轮真值回来时不跳变）
  - **展示值随真值同步落盘（运行中）**：每轮拿到真值后除推送 `event.session.ctx`，还调 `store.updateCtxStats`（只重写 meta.json，不重写 chat.json）——会话列表 / 状态快照 / 页面刷新读到的与实时推送一致；否则这些读取面只能拿到「上次任务结束时写入的值」，运行中反复刷新会在陈旧值与当前真值之间来回跳（同一会话两个百分比轮流显示）
  - **展示值随上下文改写失效**：压缩（`compactMessages`）与溢出护栏降级（`degradeProtectedMessages`）改写了历史，真值基线与展示值同时失效——清基线并把 `ctxTokens` 重算为当前消息估算（与 `truncateMessages` 同口径）；压缩成功后引擎还**立即补发一次** `event.session.ctx`（压缩后估算 + schema 段），圆环当场回落——不补发则 UI 停在「压缩前」的百分比上直到下一轮调用（观感：压缩了但百分比没掉）
  - **输出预留取值**（`outputReserveTokens`）：模型单次响应输出上限（能力声明 `maxOutputTokens`），未声明时缺省 16384，并夹在 `[1024, 窗口一半]` 内——预留不能超过窗口一半，否则小窗口模型会永远处于「剩余不足」而反复压缩
  - **提示词缓存命中度量（展示口径）**：usage 中的缓存命中字段统一提取为 `LLMUsage.cachedTokens`（OpenAI chat/responses 的 `prompt_tokens_details`/`input_tokens_details.cached_tokens` 已含在 input 内，Anthropic 的 `cache_read_input_tokens` 在 `input_tokens` 之外——pickUsage 折算并入 inputTokens 统一「cached ⊆ input」口径）；随真实 usage 基线同点位流转：每轮经 `event.session.ctx` 携带 `ctxCachedTokens` 推送、任务结束持久化为 `SessionData.ctxCachedTokens`（接口不返回缓存字段时 undefined，撤回/压缩清基线时一并清除），前端上下文圆环悬浮展示「缓存命中 tokens（占比）」。仅度量不改变请求构造——三家接口均未发送缓存控制标记（Anthropic `cache_control` / OpenAI 自动前缀缓存），命中率由服务端自动前缀缓存自然产生

#### 前缀缓存稳定性（请求前缀的字节级契约）

服务端前缀缓存（OpenAI 自动前缀缓存 / DeepSeek 上下文缓存 / Anthropic 等同机制）按**请求前缀逐字节匹配**：任一字节变化即从该点起全部失配。三家接口的请求体拼装顺序均为 `tools`（字段序在前）→ `messages`（首条 system 即主提示词，其后为工作目录、装载提示词、历史）→ Anthropic 侧为 `system`（多条 system 按出现顺序 `\n` 拼接）→ `messages`。因此**主提示词内部的分叉点位置**与**tools 段的插入位置**共同决定缓存命中面积，二者都由本节的顺序契约锁定：

- **主提示词按变更频率分层**（`PROMPT_SECTION_ORDER`，`prompt.ts`）：① 启动常量（persona / safe_mode / orchestration / batching / planning / artifact_naming / agent_routing / parallel_sessions，进程生命周期内逐字节恒定）→ ② per-user（builtin_projects：歌白家目录，同用户多会话恒定）→ 领域补充提示词（`GEBAI_PROMPT_EXTRA`/档案，启动级配置）→ ③ 任务级易变（channel 通道注记、project_bindings 项目绑定）→ ④ 会话级易变（subagent_catalog 未装载清单）。
- **`subagent_catalog` 恒为最后一段**：它是唯一「同一会话内必然变化」的段落（`agent_load` 装载即改写），压尾使装载只截断末尾——先前会让其后所有内容失效。顺序由 `prompt.test.ts` 的守卫用例锁死（断言末段、稳定前缀段不含易变段、不同会话主提示词逐字节相同、未装载清单变化只影响末尾、共享前缀量级下限）。
- **workspace（会话工作目录）不并入主提示词**，由 `buildWorkspaceSection` 独立成紧随其后的第二条 system 消息：其正文含会话 ID、每会话必然唯一，混入主提示词会让所有会话在约第 200 字节处分叉（不足以跨过 OpenAI 的 1024 token 缓存粒度，等于零共享）。外移后同一用户的所有会话主提示词逐字节相同。
- **前置段顺序契约：主提示词 → 工作目录 → 装载提示词 → 历史**。工作目录段由 `loadHistory` 产出并置于历史最前（`agentSystems` 之前），因此 **run 主循环、压缩的「缓存友好前缀请求」、子会话 fork/exec 三处装配同源**——压缩摘要请求与主循环逐字节同前缀自动成立（`compressor.deps.loadHistory`），无需各自维护拼接逻辑。fork 形态的快照自带该段（缺时补一条，防老记录前缀错位）；exec 形态把同一段挂在子会话首条 system 内。
- **tools 段分组排序**（`sessionRegistry.schemas`）：全局工具在前（按名）→ 本会话可见的子Agent 工具按名 → 本会话动态工具（`defineTool`）按定义先后排尾。子Agent 工具名是 `{agent}_*`，若沿用注册表的统一字典序会插进全局工具中间（`code_*` 落在 `bg_task` 之后），装载一个子Agent 即令其后全部 schema 后移、tools 段（通常占请求大头）大面积重算。分组后装载只追加尾部，全局工具段逐字节不变；动态工具同理不插队（新定义不应改写已有工具的相对次序）。**注册表内部顺序不受影响**——`PATCH /api/v1/tools`、管理视图与 `registry.schemas()` 照旧按名排序，排序只在会话视图这一层施加。
- **固定成本紧凑度**（提示词与 schema 的表述纪律）：缓存有效的**前提**是前缀不短于服务商的缓存粒度（OpenAI 1024 token）；由于 tools 段占请求前缀的大头，固定成本压缩既降低全价 token、也保证稳定组跨过粒度。当前口径（同口径估算）：固定成本合计约 **12.0k token** = tools 段 ~8.6k（20 个全局工具；子Agent 工具按装载状态过滤，未装载不进 schema）+ 未装载子Agent 清单 ~2.6k + 主提示词 ~0.79k + 工作目录段 ~0.11k。压缩手段限「同一信息更紧表述、去重复、去实现细节（引擎名/环境变量/内部机制等模型用不到的）」——**不移到按需查询**（查询要多一次往返，与效率优先相悖）；行为护栏（cwd 规则、literal 用法、护栏文案、枚举值语义）一律保留。清单里的「装载后工具」摘要只给总数（能力面由 description 承载，全列会使清单体积随子Agent 数量线性膨胀）。

度量与验收：`ctxCachedTokens / ctxTokens`（上一小节的圆环命中率）是现成指标——固定同一操作序列（新会话 → 首轮 → `agent_load` → 二轮）对比改前改后的每轮命中率曲线即可。
- 压缩策略（按序使用）：
  0. **超长用户输入落盘（预防）**：发送时超过阈值的用户输入自动全文写入会话工作目录 `user_inputs/{内容哈希}.txt`（原文不丢——文件面板可见、模型可经 `read` 工具读取全文；按内容哈希命名，相同输入映射同一文件），消息正文保留头尾预览 + 文件引用，避免大段粘贴撑爆上下文；未超阈值原样不变，落盘失败降级为原样保留（不改变优先）
  1. **工具大输出截断**：工具返回超过截断阈值自动截取头尾摘要（**按行保留完整行**，避免切断半行/半条目；单行巨长如 minified 时该行按字符兜底），完整内容写入文件，截断消息中附带文件路径供大模型后续读取。**引擎兜底（不依赖工具自觉）**：工具未自行截断的超长输出，由引擎在主循环统一截断落盘——凡 `output` 超过截断阈值且未带 `truncated` 标记的结果，自动复用同一截断逻辑（含内容块保留），保证任何第三方/新工具都不会撑爆上下文；已自行截断的工具结果不重复处理
  2. **旧消息摘要（压缩后置于消息数组最前，原消息完全抛弃）**：将最早一段历史消息（**除系统提示词以外全部**：用户输入/assistant/tool）由 LLM 生成摘要，摘要替换这些消息并从上下文中移除（**摘要恒为一条且位于消息数组最前**，不与近期原文交错；原文不另存——摘要即其在会话中的唯一留存形态，模型与 UI 都只看得到摘要）。摘要保留关键信息。**摘要输入优先用「缓存友好前缀请求」**：直接把与主循环**逐字节同前缀**的历史原文发过去（同一 system 提示词 + 同一段历史 + 同一批工具 schema，经 `loadHistory(upToIndex)` 渲染）+ 尾部压缩指令——服务端前缀缓存命中，已处理过的 token 按缓存价计（比把历史重写成骨架再发全价更便宜，且保留完整原文与工具调用史）；不可用时退回「骨架行 + 分块」：每条消息压成一行（assistant 工具调用轮 content 常为空——补上工具名与参数摘要，否则「调用过什么工具」在摘要里彻底消失；tool 结果带工具名与内容前段），单块输入上限 20000 字符、至多 6 块（总覆盖约 12 万字符），超出时**逐块摘要后合并**（map-reduce；单块失败跳过，不影响其余块）；总量超总预算时**头尾保留**（最早的任务背景与最新进度都进摘要）+ 中部省略说明行——不再按 20000 字符一刀切静默丢弃（52 万 token 窗口下一次压缩常覆盖 15 万字符以上，旧实现只有约 1/8 进摘要且无任何省略提示）。**滚动摘要合并**：压缩时既有摘要摘内容作为「此前摘要」并入新摘要并随新摘要替换（摘要恒为一条，不再随压缩次数累积）；**区间端点对齐 `assistant(toolCalls)/tool` 配对边界**（边界处的工具结果一并纳入，避免被当孤儿丢弃）；**摘要失败降级为骨架行**（角色 + 工具名/参数 + 内容首段，而非一句空占位），原文仍在会话记录中可查看。**系统提示词不可压缩**（`isCompressibleMessage`：仅 role=system 被排除；用户输入/assistant/tool/引擎注入消息均可压缩）——不选进压缩区间、区间夹带时原位保留；压缩条数只计实际移除的消息
  3. **摘要超限截断**：摘要本身超长时按 `SUMMARY_OUTPUT_LIMIT`（2000 字符）截断——更早历史的移除由压缩区间（②）与溢出护栏承担，不另设「滚动裁剪」独立步骤
  4. **溢出硬护栏（压缩无法收敛时的最后防线）**：压缩无内容可压时（历史几乎全是系统提示词，或可压缩消息都在滑动窗口内）受保护消息让路——最旧用户消息的图片附件降级为文本说明（可用 vision/read 按需查看）、仍不够将最旧用户消息替换为裁剪占位（占位文本明示「原 N 字符，**原文不再保留**」——压缩与护栏都会改写 chat.json，原文不另存）；**最新一条用户消息（本次任务输入）永不裁剪**；压缩为**迭代执行**（run 前按基线迭代压缩直至收敛，任务中途/溢出恢复经 `makeContextRoom` 压缩 → 护栏降级两步腾挪）
  5. **历史图片内联窗口**：仅最近 3 组含图片的消息（用户图片附件与工具结果图片——read 读取的图片引用）内联进上下文，更早的图片降级为路径说明（图片永久占窗口且不参与压缩，长会话会被历史图片占死窗口）
  6. **LLM 流式读空闲超时**：SSE 建立后连续 120 秒无任何 chunk 判定接口假死，中止本次调用（无产出走重试、有产出上抛为任务错误）——此前网关/上游挂起会无限挂起任务
- 压缩过程对用户透明，UI 显示压缩通知（压缩范围、摘要内容）——**被压缩区间的原文不另存**（chat.json 只留摘要，见原则 ②）
- 压缩后继续原任务流程，不影响进行中的工具调用循环

> **实现**：已落地。`engine.compactSession()` 支持主动（`session.compact` / REST `POST /compact`，scope 指定区间）与自动触发（**窗口剩余 < 输出预留**时压缩，触发与目标口径见「上下文占用口径」与「常量参考」）；摘要由 LLM 生成，**首选「缓存友好前缀请求」**：直接把与主循环**逐字节同前缀**的历史原文（同一 system 提示词、同一段历史、同一批工具 schema，经 `loadHistory(upToIndex)` 渲染）加上尾部压缩指令发给模型（`CACHE_PREFIX_INSTRUCTION`）——服务端前缀缓存（OpenAI 自动前缀缓存 / DeepSeek 上下文缓存 / 智谱・千问等同机制）因此**命中**，已处理过的 token 按缓存价计（多数服务商 10%~50%），且无需把消息重写成骨架（保留完整原文与工具调用史，摘要信息量更高）；不可用时退回「骨架行 + 分块」路径：前缀装不下（`estimateMessageLikeTokens` 超 `窗口 - 8192`）、前缀内有既有摘要无法吸收、上次同模型调用未命中缓存（`usage.cachedTokens=0`，按 provider+model 记忆，不白付全价）、空文本/接口异常。`GEBAI_COMPACT_CACHE_PREFIX` 可强制开关（0/off 强制骨架、1/on 强制原文前缀、缺省自适应）。摘要调用同样带读空闲超时与取消信号（同 `chatWithIdleTimeout` 防假死）——压缩在任务流程内同步等待，无超时会把整个运行中任务永久挂死；失败降级为骨架行占位（保留被裁剪内容的工具/文件脉络，而非一句空话）。摘要消息持久化（`compacted`/`summary` 标记，UI 渲染为压缩通知，历史重载时作为 **user 角色**注入且置于历史最前）；已压缩摘要消息不重复保留（压缩时既有摘要内容作为「此前摘要」并入新摘要输入、随新摘要替换——长会话摘要恒为一条且始终在数组最前）。**手动压缩在会话有任务运行时被拒**（summarize 是秒级 LLM 调用，期间任务持续追加消息，陈旧压缩区间会套删未参与摘要的新落盘内容；自动压缩经 `internal` 标记在任务流程内自身协调，不受此限）。**超长用户输入落盘**（`spillLongUserInput`，run 发送时执行）：超阈值输入全文写入会话工作目录 `user_inputs/`，消息正文保留头尾 + 文件引用。**压缩只不碰系统提示词**（`isCompressibleMessage`：仅 role=system 被排除；用户输入/assistant/tool/引擎注入提醒均可压缩）——系统提示词消息（含装载提示词）不选进压缩区间、区间夹带时由 `store.compactMessages` 原位保留不移动；缓存友好前缀请求会把系统提示词作为**主循环同前缀**一并发给模型（前缀缓存匹配所需），但**会话记录里它一字不改、位置不变**；区间内无可压缩消息时不做任何改动（不创建摘要、不动 usage 基线）；`compactMessages` 的压缩条数只计实际移除消息数。超限截断（`trimToCacheLimit`，2000 条上限，纯存储安全网）**按批裁剪**：超限时一次裁到低水位（`TRIM_LOW_WATER_MESSAGES`=上限×0.9=1800 条，单批腾出 `TRIM_BATCH_MESSAGES`=200 条余量）而非逐条挤出——裁剪会改写送模型的历史前缀（服务端前缀缓存失效），逐条挤出等于每条新消息都改一次前缀，按批裁剪把前缀变化摊薄到每积累 200 条消息一次；同样保护：受保护消息原位保留、从最早的其他消息开始丢弃，受保护消息本身使长度仍高于低水位时按原样保留（软上限，不改变优先），丢弃按 tool_call 配对原子执行——assistant(toolCalls) 被丢弃时连带其后紧邻的 tool 结果（拆散配对会产生孤儿 tool 消息，严格校验的 LLM 接口会拒绝整个请求），实际保留条数可略低于低水位；截断保护同上（`compacted`/`loadedAgent`/用户输入消息在超限截断中原位保留、不重排）。**配对完整性修复（`repairToolPairing`）**：任务取消中断、压缩/截断边界或旧版本缺陷仍可能产生「孤儿 tool 结果」（发起 assistant 已删）或「未应答 toolCalls」（结果缺失）——严格校验的接口（OpenAI tool_calls/tool、Anthropic tool_use/tool_result 配对）会拒绝整个请求，会话自此每次运行 400 卡死。修复分层落地：① 引擎工具循环取消/异常路径为本轮全部 toolCalls 补写占位结果（assistant 先落盘后执行的中断不再缺结果）；② 存储层 `compactMessages` 压缩后、`readFileByPath` 磁盘装载时即时修复（孤儿普通 tool 丢弃、孤儿受保护 tool（subsession_run 存档）补最小 assistant 桩、中途未应答补占位结果；尾部未应答**不在存储层 flush**——正常执行流 assistant 先落盘结果随后到达，提前 flush 会让真实结果反被判孤儿丢弃）；③ `llm.ts` 三家序列化入口兜底（含尾部 flush + Anthropic 相邻同角色 user 消息合并），旧版本已损坏的会话在下一次模型调用自愈。压缩替换消息后真实 usage 基线的索引锚点失效：`store.compactMessages` 自动清除 `ctxInputTokens`/`ctxAtMessage`，并把展示值 `ctxTokens` 重算为当前消息估算（旧展示值不得残留，否则 UI 停畵在压缩前）；引擎包装层在 `compacted > 0` 时立即补发一次 `event.session.ctx`（压缩后估算 + schema 段）并落盘，圆环当场回落。**压缩迭代 + 溢出护栏 + 中途压缩 + 溢出恢复 + 读空闲超时均已落地**（`engine.ts`：`makeContextRoom`/`degradeProtectedMessages`/`callModelWithOverflowRecovery`/`isContextOverflowError`/`chatWithIdleTimeout`；图片内联窗口 `INLINE_IMAGE_RECENT=3` 作用于 `loadHistory`）。**摘要输入保真与滚动合并**（`compressor.ts`：`summarizeMessageLine` 工具调用骨架、`buildSummaryChunks` 分块与头尾保留、`summarizeFallback` 骨架降级、`summarize` 的 map-reduce 与合并、`compactSession` 的既有摘要吸收与区间配对对齐）；**输出预留驱动的压缩规划**（`outputReserveTokens` + `lacksOutputRoom` + `planCompactRange`：触发看「窗口剩余是否还够一次回复（maxOutputTokens）」，目标为「窗口 × `COMPACT_TARGET_RATIO`（缺省 40%，`GEBAI_COMPACT_TARGET_RATIO` 可调），且不高于触发线」——**压到目标即停**，压缩区间不越**近消息滑动窗口**（最近 12 条消息，`COMPACT_WINDOW_MESSAGES`/`GEBAI_COMPACT_WINDOW` 可调，上限为历史一半；**有真实占用基线时窗口仅为保底下限**——压多少由水位算出的需腾出量决定），需腾出量为真值口径、按 `estTotal/基线` 比率折算为估算口径后逐条累计（`estTotal` 含工具 schema 段估算）——确保「最近的保留、远的先压、压到目标即止」）；**截断记录与提示**（`SessionData.trimmed` + `loadHistory` 注入「[历史裁剪]」提示，模型知道更早内容已不在上下文中，而不会把历史从中间开始当作完整历史）；**溢出护栏降级可见**（`degradeProtectedMessages` 经 `event.message.compact` 发布 `degraded` 事件，UI 显示上下文为何变化）；**超限裁剪用户可见**（`engine.notifyTrim`：`SessionData.trimmed` 计数增长时经同通道发布 `degraded: "trim"`，前端标题「历史消息条数超限裁剪」——此前截断只有模型侧的 `[历史裁剪]` 提示、UI 无声，长会话里易被误认为「压缩没生效」）。

#### 主动压缩

- 主动压缩经接口触发（REST `POST /api/v1/sessions/:id/compact`；**无 `/compact` 斜杠命令，UI 不提供压缩按钮**——日常上下文由引擎按水位自动压缩，见上）
- 支持自定义压缩范围：`scope` 取 `"all"`（全部历史）或 `{from,to}`（指定消息区间）——**无「仅工具输出」**（仅 REST/WS 入口可指定 scope）
- 主动压缩同样走上述策略，压缩结果立即生效并持久化

#### 截断保护存储

- 存储路径：`{session}/tmp/truncated/{tool_name}_{content_hash}.txt`（`{session}` 为会话根目录 `{GEBAI_HOME}/users/{user}/sessions/{s0}/{s1}/{session_id}`）
- 消息中返回**会话工作目录内相对路径**（如 `truncated/read_xxx.txt`）：模型可经 `read` 工具直接读取，且同一路径在 `sh`/`py`/`js`（cwd 即会话工作目录）里可直接使用——带 `tmp/` 前缀会被脚本当子目录多套一层（`tmp/tmp/…`）；前端文件面板同步可见/可下载（面板/REST 契约路径仍带 `tmp/` 前缀，文件工具对两种写法等价接受）
- 文件名含工具名可溯源；文件名按内容 SHA256 哈希命名（同内容同路径，重复截断幂等覆盖，写入无存在性短路）
- 生命周期随会话：会话删除/过期时随 `tmp/` 整体清理（见「数据生命周期」）

### 消息模型与数据结构

核心数据结构（SDK 与 UI 共用，作为类型契约）：

```ts
// 内容块（消息内容的结构化呈现，UI 逐块渲染；image/file 的 path 为会话 tmp/ 内逻辑路径）
type ContentBlock =
  | { type: "text"; text: string }
  | { type: "code"; text: string; language?: string; path?: string; name?: string }
  | { type: "image"; path: string; name?: string; mime?: string }
  | { type: "file"; path: string; name: string; mime?: string }
  | { type: "diagram"; format: "plantuml" | "mermaid" | "d2" | "echarts"; code: string; name?: string; version?: number }
  | { type: "diff"; oldText: string; newText: string; language?: string; name?: string; oldName?: string; newName?: string; lines: DiffLine[] }
  | { type: "html"; html: string; name?: string; width?: number; height?: number }

// 行级 diff 结果（diff 内容块；diff 工具已移除，仅历史会话回放）：按顺序排列，每行标注差异类型
type DiffLine = { kind: "equal" | "add" | "del"; text: string }

// 消息（持久化于 chat.json）
interface Message {
  id: string
  role: "user" | "assistant" | "tool" | "system"
  name?: string                        // 工具消息的工具名 / 助手消息名
  images?: Array<{ path: string; mime: string }>
                                       // 工具结果图片引用（轻量、不含 base64）：loadHistory 按内联窗口重读内联
  arguments?: Record<string, unknown>  // assistant 工具调用的参数（与 toolCalls 配对）
  loadedAgent?: string                 // 子Agent 装载提示词的 system 消息标记（loadHistory 透传进模型上下文）
  content: string
  reasoning?: string                   // 推理内容（reasoning_content/thinking）独立字段：assistant 消息持久化时写入，content 保持纯正文；
                                       // 回放给 LLM 时（loadHistory）不携带——推理绝不进模型上下文；UI 历史渲染为折叠推理卡；旧版数据推理内嵌 content 的 <think> 块（兼容展示/剥离）
  model?: string                       // 生成该回复的模型名（assistant 消息落盘时由 provider capabilities 携带；仅存储/UI 展示用，loadHistory 不回放给模型；用户反馈 FeedbackInfo.model 据此自动关联，见「用户反馈」）
  blocks?: ContentBlock[]              // 富内容块（文本/代码/图片/文件/图表），随消息持久化
  attachments?: AttachmentRef[]        // 多模态引用
  toolCalls?: ToolCall[]               // assistant 的工具调用请求
  toolCallId?: string                  // tool 结果关联
  createdAt: number
  compacted?: boolean                  // 上下文压缩摘要消息标记（role=system），UI 渲染为压缩通知
  summary?: string                     // 压缩摘要消息：被压缩的原始区间描述（条数/时间范围）
  session?: boolean                    // 子会话运行过程消息（旧版逐条存档，历史兼容）：完整存档但【不进入主 LLM 上下文】（loadHistory 跳过），前端按 runId 分组折叠渲染
  subSessionId?: string               // 子会话运行 run 标识：同一次 subsession_run 执行过程的消息共享（前端回放按此分组）
  subSessionMeta?: { agents: string[]; input: string }  // 子会话 run 元信息（仅该 run 首条消息携带）：观察面板的记录标题用（预加载子Agent 名与输入）
  subSessionArchive?: SubSessionArchive    // 子会话 run 完整存档（subsession_run 工具调用记录 / 合并消息 / bg_task 结果扩展字段）：执行过程全部内容，历史回放登记进观察面板；
// 子会话运行（subsession_run）合并消息同样携带（subsession 字段标识，面板内展示「🌿 名」运行）
  subSessionMerged?: { runId: string; name: string; model?: string }
                                      // 子会话合并消息标记（role=user + engineNote: "subsession"）：subsession_run 继承形态子会话最终报告/阶段性合入的消息携带；
                                      // 用于子会话互相感知（父会话增量标注来源）、本子会话自身合入跳过判定；前端按 engineNote 渲染为「子会话合入」通知条
                                      // （drainBranchMerges 注入点即下一条模型调用前，assistant 形态会被思考类模型 400 拒绝）
  engineNote?: "todo" | "verify" | "task" | "branch"
// 引擎提示标记：收尾验证提醒/任务结果写回/子会话报告合入（role=user，避开思考类模型的尾 assistant 约束）；
                                      // 待办续做为模拟 todo 查询工具对的 tool 消息携带（引擎合成的调用留痕，非模型真实发起）。
                                      // 标记供 UI 渲染为弱化通知条或带标记的工具卡，与用户自己的消息/真实工具调用区分；详见「引擎注入消息的角色约定」
  // 旧版（agent_call 时代）字段：subAgent/subAgentRunId/subAgentMeta/subAgentRun 兼容历史会话回放，新数据不再写入
}
interface SubSessionArchive {
  runId: string                        // run 标识（与实时事件 subSessionId 同源）
  agents: string[]                     // 预加载进子会话的子Agent 列表
  input: string                        // 任务输入（容器标题展示）
  output: string                       // 最终返回文本（折叠后摘要展示；异常/取消为空串）
  messages: SubSessionEntry[]          // 执行过程全部消息（输入/每轮回复/推理/工具调用与结果）
  branch?: { name: string; model?: string }
                                       // 子会话运行（subsession_run）标识：存档携带（runId/名/模型），前端容器标题按子会话名渲染
}
interface SubSessionEntry {
  role: "user" | "assistant" | "tool"
  name?: string
  content: string
  reasoning?: string                   // 推理内容独立字段（同 Message.reasoning 语义）：assistant 条目持久化时写入，回放展示不回流模型
  toolCalls?: ToolCall[]
  toolCallId?: string
  arguments?: Record<string, unknown>
  blocks?: ContentBlock[]
  subSessionArchive?: SubSessionArchive    // 嵌套子会话（子会话内再派生）存档递归携带
}

interface ToolCall {
  id: string
  name: string                          // 全局名（含命名空间前缀）
  arguments: Record<string, unknown>
}

interface ChatChunk {                   // 流式输出单元
  kind: "text" | "reasoning" | "tool_call" | "tool_result" | "approval" | "done" | "error" | "reset" | "session_start" | "session_done" | "resume" | "model_error"
  messageId?: string                    // text delta 携带本条 assistant 消息 id（前端反馈/操作绑定）
  subSession?: boolean                  // 事件来自子会话运行过程（subsession_run 派生的子会话；父会话主回复不带此标记）
  subSessionId?: string                // 子会话运行 run 标识：同一次 subsession_run/subsession_run 的执行过程事件共享（前端按此分组渲染）
  subSessionMeta?: { agents: string[]; input?: string; output?: string; subsession?: string; model?: string }  // 子会话 run 元信息：start 携带 agents/input，done 携带 agents/output（面板行状态用）；子会话运行（subsession_run）携带 branch（分支名）/model（模型路由名）——容器标题渲染「🌿 子会话 · 名（模型）」
  text?: string
  toolCall?: ToolCall
  approval?: { toolCallId: string; retries: number; tool: string }
  error?: string
  output?: string                       // done 携带的最终输出（含 session 形态）
  blocks?: ContentBlock[]               // tool_result 携带的富内容块（图片/图表/文件）
  retry?: number; maxRetry?: number     // model_error 携带的重试进度（非终态；前端渲染为消息流内常驻异常记录）
}

// 补充语义：
// - `reasoning`：推理内容增量（reasoning_content / thinking），前端渲染为折叠推理块：思考中默认展开实时展示（推理内容可见），推理段结束（正文开始/工具调用封段/流结束）自动折回收起态，用户可点 summary 重新展开；**内容 markdown 完整渲染**（与正文同路径节流——**120ms 尾沿节流是全模式统一路径**，低性能模式不再单独降频）；推理内容超出可视高度（`.reasoning-body` 限高 min(60vh, 720px)）时内部滚动条自动跟随最新内容，用户上翻翻阅历史不打扰（`reasoning-scroll.ts`）；
//   推理**持久化为独立字段**（`Message.reasoning`，content 保持纯正文；历史会话/切回可见，前端默认折叠可展开，内容同样 markdown 渲染），**回放给 LLM 时不携带**（`loadHistory` 仅映射 content——推理绝不进模型上下文）；旧版数据推理内嵌 content 的 `<think>` 块：前端回退解析展示、回放时 `stripThinkTags` 剥离（兼容，不做数据迁移）
// - `text`：文本增量；**携带 `subSession: true` + `subSessionId` 表示文本来自子会话运行过程**（subsession_run 派生会话流式回复），前端渲染进该 run 在观察面板里的过程区（见下）
// - `session_start`：子会话 run 开始（携带 runId + agents/input），前端在消息流建一行入口（点击开观察面板）并建记录/过程区；服务端**每轮重推**（同 runId 幂等，前端已有记录则只刷新元信息），前端记录随页面重载丢失后新一轮 delta 前可据此重建；子会话运行的 start 携带 `subsession`/`model`（入口行/面板标题「名（模型）」）🌿 子会话 · 名（模型）」）
// - `session_done`：子会话 run 结束（携带 runId + agents/output），前端封存过程区的流式文本段，输出与耗时定稿——消息流入口行与面板行同步刷新状态
// - 子会话运行过程（text/reasoning/tool_call/tool_result/approval）事件**全部携带 `subSession: true` + `subSessionId`**（与主循环同构渲染：推理折叠块、工具卡片、流式文本），并**完整落盘**（见「子会话运行存档」）——仅存档与前端回放，`loadHistory` 跳过（不进入主 LLM 上下文）；会话列表 ctxTokens 估算同样排除
// - `error`：任务失败（LLM 接口错误等），前端应渲染错误提示（与流异常中断同等对待）
// - `tool_call`/`tool_result`/`approval`：WS `event.*` 推送，SDK `sendPrompt` 订阅事件流转换为 ChatChunk 迭代（单通道，无 SSE 兑底）

interface AgentEvent {                  // WS event.* / Webhook 统一载荷
  type: string                          // event.message.delta 等
  sessionId: string
  payload: Record<string, unknown>
  timestamp: number
}
```

- 消息持久化上限（1000 条/会话，纯存储安全网，超出截断最早的非保护消息）+ LRU 会话缓存（10 个）+ 会话 env 解析 LRU 缓存（256 个，`MAX_ENV_CACHE_SESSIONS`）
- 附件引用 `AttachmentRef`：`{ path, mime, name, size }`，指向会话 `tmp/` 内文件
