# 小白（xiaobai）

> 桌面上的极简 AI 对话窗口。一个模型、一条对话、一次配置——别的都不要。

## 它是什么

小白只做一件事：**跟一个模型聊天**。

| 功能 | 说明 |
|---|---|
| 对话 | 消息列表（Markdown 渲染）+ 多行输入框 + 发送/停止 |
| 流式 | 回复逐块到达，按打字机效果追加（不是等整段返回） |
| 配置 | OpenAI 兼容端点 + API Key；**没有顶栏**，点标题栏左侧的模型名开配置面板；**不写配置文件** |
| 模型 | **单一模型**，随构建预置（见下），界面上不提供模型选择 |

模型服务**仅支持 OpenAI `/v1/chat/completions` 规范**（任意兼容端点：OpenAI、
DeepSeek、通义、vLLM、Ollama 的 OpenAI 兼容层、LM Studio…）。

## 形态

``` 
apps/xiaobai/
  main.cpp                       整应用（声明式界面 + HTTP 流式客户端 + 配置持久化）
  assets/xiaobai-icon.png        窗口/任务栏图标（tools/make_icons.py 生成）
```

- **界面**：C++ 声明式（`st::ui::dsl`）——`WindowFrame` 外壳 + `mount_into` 一块声明式页，
  与裁云/镂月同一模式。
- **传输**：`st::net::connect_tcp` + `st::net::TlsStream`（Windows 走系统 Schannel，零依赖）。
  不用 `HttpClient`：它是「整份读完才返回」的模型，拿不到流式增量。
- **线程**：请求在后台线程，只把增量文本塞进互斥保护的缓冲区；主循环每帧取走并写
  State（跨线程写 UI 状态会与遍历中的元素树竞争——框架硬约束）。

## 配置：构建期预置 + 面板现填（**不落盘**）

本应用**不写任何配置文件**。每次打开都是新的：关掉窗口，设置就没了；
预置值下次启动会自动回来。

| 来源 | 说明 |
|---|---|
| **构建期宏**（推荐） | `st build --define 'NAME=\"value\"'`，条件编译进二进制，产物开箱即用 |
| 环境变量（同名） | 临时调试用；优先级高于宏 |
| 设置面板现填 | 仅本次运行有效（内存态） |

三个宏各自独立，给哪个预置哪个：

| 宏 | 含义 |
|---|---|
| `XIAOBAI_BASE_URL` | 兼容端点（到 `/v1`） |
| `XIAOBAI_MODEL` | 模型名；预置后设置面板**不展示**该字段（展示了也改不动） |
| `XIAOBAI_API_KEY` | Bearer 令牌 |

### 打包开箱即用的产物

```bash
st build xiaobai --profile release \
  --define 'XIAOBAI_BASE_URL="http://aic.byd.com/model-gateway/v1"' \
  --define 'XIAOBAI_MODEL="deepseek-v4.1-flash"'
```

`--define` 可重复、只作用于本次构建、**同名覆盖清单里的**，所以部署参数
（尤其密钥）不会提交进仓库。

> ⚠ 宏进的是编译命令行，**改宏会触发全量重编**（宏是对象缓存键的一部分）。
> 这是刻意的：否则换了宏却复用旧对象，产物里的配置就是上一个宏的值。

### 没预置时

**启动即弹配置面板**，填完点「开始使用」才能用；面板会自动回显
已预置/已填的值：

- Base URL（如 `http://aic.byd.com/model-gateway/v1`）
- API Key（Bearer 令牌，输入框掩码）
- 模型名（仅当**没有**被宏/环境变量预置时出现）

> 没有顶栏设置入口是刻意的：点**标题栏左侧的模型名**就能打开这个面板。

## 界面

- **没有顶栏**：聊天区从窗口顶部直接开始；模型名放在**标题栏**（标题之后的 trailing 槽）——
  它本就是「这个窗口在跑哪个模型」这类窗口级信息
- **状态不常驻**：只有生成中/出错时在消息流末尾出现一行；就绪态置空，界面上没有状态条
- **气泡**：按内容自适应宽度（短句是小气泡，长文才展开），上限 `kMaxBubbleWidth`
- **输入胶囊**：随内容长高（1~6 行，超出内部滚动）；单行是胶囊、多行自动变圆角矩形

## 启动

```bash
st build xiaobai --profile dev
./build/dev/bin/xiaobai.exe           # 有窗口
./build/dev/bin/xiaobai.exe --headless --control-port 0   # 无头（自动化）
```

## 验证

```bash
# 1) 起一个本地 mock OpenAI 流式端点（仅标准库，不需要真 API Key）
py -3 tools/xiaobai_mock.py 8765

# 2) 无头启动应用，配置指向 mock
./build/dev/bin/xiaobai.exe --headless --control-port 0
#    配置窗里填 base_url=http://127.0.0.1:8765/v1，api_key 任意、模型 mock
```

控制通道可断言的关键元素：

| id | 作用 |
|---|---|
| `status-line` | 当前状态（就绪/生成中…N 字符/请求失败） |
| `msg-input` | 输入框（`TextInput` 驱动） |
| `btn-send` | 发送/停止（`busy` 时切「停止」） |
| `cfg-url` / `cfg-key` / `cfg-model` | 配置窗字段（`cfg-model` 仅未预置时存在） |
| `cfg-save` | 「开始使用」 |
| `model-label` | 标题栏上的模型名（预置值或用户填写值） |

## 两条踩过的坑

**① `Json{{{...}}}` 不是对象。** 早先配置写盘用了三重花括号，nlohmann 按初始化列表规则
解成「数组里一个对」，文件里是 `[{...}]`，读回时对象字段全落空——现象是「保存了但重启后
配置丢失」。统一改成 `Json::object()` + 逐字段赋值；`from_json` 另外兼容旧的数组形态。

**② 流式增量不能在 `build()` 里泵。** `build()` 只在重组时执行，而重组靠「状态脏了」触发；
把「取增量」放在 build 里就变成互相等待的静默死锁（实测状态永远停在「正在连接…」）。
正确做法是主循环每帧先泵增量（写 State 标脏），再 `tick_declarative_hosts()`。

**③ 配置面板不能用 `overlay()`。** overlay 挂在 `UiRoot` 顶层，会连 `WindowFrame` 的标题栏
（拖动区/最小化/关闭）一起盖住——现象是「弹窗出来了但窗口拖不动」。改成本页 column 里的
绝对定位子元素（`.absolute` + 四边偏移），就只盖内容区。

**④ 胶囊里的输入框要关掉自己的底/边。** 底色与描边由基类 `paint_box` 按 `style_` 画，
只在 `paint_content` 里跳过是**盖不住**的；而 `sync_control_style` 每帧会把
`border_width` 按主题填回 1px。用两个正交开关解决：

```cpp
area.set_seamless_background(true);  // 不画底色（框架既有语义：只去底色）
area.set_show_border(false);         // 不画描边与焦点环（新增开关）
```

外框视觉完全交给胶囊容器。

## 从应用里沉到框架的能力

做这个应用时发现的框架缺口，都已经补进框架（应用侧不再迁就）：

| 原缺口 | 框架落点 |
|---|---|
| 圆的“胶囊”语义：`radius_pill` 是 999 哨兵值，但绘制层不夹取 | `Canvas::fill_rect` / `draw_shadow_layered` 按 **CSS `border-radius`** 夹到短边一半（`src/raster/canvas.cpp`）|
| 胶囊随高度自适应时圆角会变成大半圆 | `Style::radius_max` + `BoxProps::radius_max`：上限封顶 → 矮时胶囊、长高后圆角矩形 |
| 绝对定位“只给 `bottom`”被误当成纵向拉伸 | `place_absolute` 改为**两端都锚定才拉伸**（CSS 语义）；`Style::left/top` 默认值改 `-1` 表示未锚定 |
| 悬浮子层只能 `overlay()`（会盖住标题栏） | 用 `BoxProps` 的 `absolute` + 四边锚点声明在本页容器内即可（已可直接写）|
| `BoxProps` 缺偏移/圆角上限/描边覆盖 | 补 `absolute/left/top/right/bottom/intercepts/radius_max/border_width/border_color` |
| 输入框要按行数自动增高 | `TextArea::set_auto_grow` + `set_lines_range(min, max)`（超出内部滚动）|

**⑤ 胶囊高度要自己算，且圆角要封顶。** 两条都踩过：

- `place_absolute` 里「`height == kAuto` 且给了 `bottom`」的语义是**纵向拉伸到父高**
  （CSS `top:0;bottom:0`），所以胶囊高度必须显式给——否则被拉满整个内容区。
- `radius_pill` / `RadiusMode::Pill` 都是 **999 哨兵值**且绘制层不夹取，直接用它
  会把胶囊画成一个巨大的椭圆。正确做法是 **`min(半高, 22)`**：矮时是胶囊，
  长成多行后停在 22 = 圆角矩形（半高会让两端一直是巨大半圆）。
- 输入框高度要**显式**给（胶囊高 − 内边距）：Row 容器里 `grow` 只作用于主轴（横向），
  纵向靠 `align_items`，而 `Center`/`End` 都按内容高度对齐、不拉伸 → 实测“胶囊涨到 6 行
  而输入框停在 4 行”（文字被裁）。
- 行数从 `draft` **每帧重算**，不从 `on_change` 推：`set` 属性面写文本不走回调，
  那时胶囊不会跟着涨。
