# 相期（Xiāngqī）

> agent 在你桌面上的一个常驻窗口。它**提示你、请你选择、让你待办等待**——
> 把「需要人介入的时刻」从终端搬到桌面上。

名字取「相期」：两端相对而等。agent 那头等你确认，你这头等它交活。

## 它解决什么

agent 在本机跑着，迟早会遇到三类必须有人参与的时刻：

| 时刻 | 相期的形态 | 能否忽略 |
|---|---|---|
| 有事要告诉你 | `notice` 提示卡 | 能（批量忽略） |
| 要你做一个决定 | `choice` 选择卡 | **不能**（必须有答案） |
| 要你去干一件事 | `todo` 待办卡 | **不能**（必须有结果） |

后两类刻意不允许忽略：那两类的价值就是"必须有结果"。允许忽略等于允许 agent
的请求静默消失——**比不弹出来更糟**（它会以为你收到并默许了）。

## 形态

**纯 XML + JS 应用**（与 `examples/xmlapp` 同一开发方式）：

```
examples/xiangqi/
  main.cpp       装配 + 能力暴露（唯一 C++ 部分，零 UI 代码）
  ui/main.xml    界面长什么样
  ui/app.js      界面怎么动
```

改卡片的呈现方式**不必动 C++**——那正是这类界面最常改的部分。

## 传输：WebSocket 客户端

相期是客户端，连 agent 控制器：

```
相期 ──ws://host:port/xiangqi──▶ agent 控制器
     ◀── {"type":"cards","cards":[…]}    下发卡片（权威列表，整批替换）
     ◀── {"type":"card", …}              单张新增/更新（省流量的增量形态）
     ◀── {"type":"dismiss","id":…}       agent 自己撤回
     ── {"type":"answer","id":…,…} ──▶   回传答复（含卡片 kind 与答复值）
```

答复**双写**：WS 送达 + 落盘 `outbox/<id>.json`。两份都要——WS 可能恰好断在这一刻
（send 失败就靠盘），而盘上有记录才能在"我点了但没反应"时查出是哪一环丢的。

### 断线不自动重连（刻意的）

断线把状态**显式**显示为「未连接 + 具体原因」，不做静默自动重连。

常驻应用里"看起来连着其实早就断了"是最坏的状态——用户以为 agent 在盯着，
实际提醒根本没送到。显式断开让这个问题不可能被忽略。

### 降级路径

WS 连不上时仍可从 `inbox/*.json` 读卡片（文件投递），答复仍写 `outbox/`。
理由：agent 可能还没起、可能不支持 WS——那时让人完全看不到提醒，比降级严重得多。

## 启动

```bash
st build xiangqi --profile quick
./build/quick/bin/xiangqi.exe --agent ws://127.0.0.1:7717/xiangqi
```

| 参数 | 说明 |
|---|---|
| `--agent <url>` | 控制器地址（默认 `ws://127.0.0.1:7717/xiangqi`；也可用 `ST_XIANGQI_AGENT`） |
| `--home <dir>` | 数据根（默认 `<cwd>/xiangqi`，内含 `inbox/` `outbox/` `seen.txt`）—— 目录名用 ASCII 而非中文，因为它是命令行参数且会进日志/降级文件路径 |
| `--headless` | 无头运行（e2e/CI 用） |
| `--control-port` / `--control-file` | 控制通道（自动化验证用） |
| `--theme` / `--shots` / `--ui` | 主题 / 截图目录 / 资源目录 |

连不上 agent **不算错误**：应用照常启动，界面显示未连接。

## 验证

```bash
py -3 tools/xiangqi_e2e.py
```

覆盖四条接缝（任一处坏掉应用就不成立）：

1. XML → 界面树（三区、徽标、连接状态在场）
2. **WebSocket → C++ → JS**（卡片真的渲染出来；协议握手/掩码/分帧全自研，mock 掉就测不到）
3. **JS → C++ → WebSocket**（点选项后答复回到控制器，卡片随之移除）
4. 断线如实显示（控制器停掉后界面不再假装连着）

脚本自带一个最小 WS 服务端（`MiniWsServer`，仅标准库），不需要装第三方包。

## 两个踩过的坑（对后续做常驻面板有用）

### 1. `try_read_message` 不读 socket

`st::net::ws::WsSession::try_read_message()` 只消费**已解析的帧**，自身不读 socket
（底层 `pump_frames` 是阻塞 read）。所以正确用法是：

```cpp
if (!tcp.wait_readable(0)) return;   // 先探测，无数据立刻返回
auto msg = session.read_message();    // 已确认可读 → 不会阻塞
```

直接用 `try_read_message()` 的现象是「WS 连上了但永远收不到消息」，
**且没有任何报错**。心跳里那两个计数（探测/可读）就是为了区分这种状态与"真的没有消息"。

### 2. 清空容器 + 同帧大量 append 会让宿主 bad_alloc

对同一容器 `clear()` 后立刻追加十几个子元素，宿主会直接 `std::bad_alloc` 崩掉，
**没有任何 JS 异常**，日志停在上一条业务日志——极易误判成业务代码的问题。

相期的做法是**槽位复用**：卡片区固定容纳 N 个槽位（只增不减），每张卡只在自己的
槽位里建一次结构，之后只 `set` 属性；卡片被答复掉时槽位保留但隐藏。
结构变更只在"卡片数创新高"时发生，且一次只加一个。

代价是槽位不销毁（个位数量级，可忽略），换来的是绝不触发宿主的结构压力。

## 系统托盘（已实现）

`AppOptions::tray_enabled = true` + 一份 PNG 图标，启动后通知区就有常驻图标。
点击它 → `app.take_tray_clicks() > 0` → `app.window_restore()` 把窗口叫回前台。

```cpp
app_options.tray_enabled = true;
app_options.tray_icon_png = make_tray_icon_png();   // 见 main.cpp（程序内生成，不依赖资源文件）
app_options.tray_tooltip  = "相期 · 有事等你";

// 主循环里：
if (app.take_tray_clicks() > 0) app.window_restore();
if (app.tray_ready()) app.set_tray_tooltip("相期 · " + std::to_string(cards.size()) + " 件");
```

托盘图标在 `WM_DESTROY` 里自动移除（不留僵尸图标）。后端不支持时只告警、不阻止启动，
`app.tray_ready()` 如实反映是否真的建出来了——**不假装成功**。

## 两种形态：轻提示与主界面

「不必每次弹出主界面」是这个应用的核心体验：

| 形态 | 何时 | 窗口 | 内容 |
|---|---|---|---|
| **轻提示** | 只有 `notice` | 360×200 | 最新一条 + 「展开 / 知道了」 |
| **主界面** | 有 `choice` / `todo` | 460×620 | 全部卡片（可滚动） |

## 布局：只有一条栏

```
┌─ 🔔 相期   5 件等你处理  🔗 已连接   全部忽略  ─ □ ✕ ─┐  ← 真标题栏
├─────────────────────────────────────────────────┤
│  卡片区（ScrollView，内容长了自己滚）              │
└─────────────────────────────────────────────────┘
```

徽标 · 连接状态 · 「全部忽略」全部住在**真标题栏**里（`TitleBar::add_trailing`）。

早期版本是三条横向带：窗口标题栏（图标+相期+窗口按钮）、XML 里的 `header`
（又一遍图标+相期+徽标）、底部 `footer`（连接状态+全部忽略）。
同一个信息说了两遍，而且白吃 2×40px——对贴角瞄着的常驻面板是浪费。

标题栏元素与 XML 元素在控制通道里**同样可寻址**（`#badge` / `#link-text` /
`#dismiss-all` 都能 `get`/`set`/`invoke`）：`snapshot()` 从 `UiRoot::content()`
递归，而 `WindowFrame` 自身就是 content 根，标题栏是它的子节点。

判定在 `app.js` 的 `desiredCompact()`：只要出现需要决策或动手的卡片就必须给足界面
（360×200 里连选项都放不下）。用户主动点过「展开」后不再自动缩回——自动收起是打断。

窗口尺寸用 `app.set_window_size(w, h)` 运行时切换（框架能力，见下）。

## 本轮补的框架能力（不是应用层绕路）

| 能力 | 位置 | 为什么该在框架 |
|---|---|---|
| 系统托盘 | `shell::Backend::set_tray_icon/remove_tray_icon/take_tray_clicks` + win32 `Shell_NotifyIcon` | 托盘是**桌面环境概念**（Windows 通知区 / Linux StatusNotifier / macOS 菜单栏），机制完全不同；且生命周期要跟窗口一起管 |
| 窗口恢复 | `shell::Backend::restore` + `Application::window_restore` | 各平台做法不同（win32 要 `SW_RESTORE` + 前台锁、X11 要 `_NET_ACTIVE_WINDOW`、Wayland 基本不允许） |
| 运行时改窗口尺寸 | `shell::Backend::set_size` + `Application::set_window_size` | 常驻面板的「缩成一角 / 展开」基础；多窗口在无头后端与截图验证里都要额外路径 |
| 绑定随元素销毁回收 | `ScriptHost::prune_dead_bindings` | 元素生命周期是框架的事，不能要求应用记得 `off()` |
| 选择器解析缓存 | `ScriptBinding::parsed` | 绑定表规模由应用决定，框架不该在每次事件上重算 |

## 三个踩过的坑（对后续做常驻面板有用）

### 1. `try_read_message` 不读 socket

`st::net::ws::WsSession::try_read_message()` 只消费**已解析的帧**，自身不读 socket
（底层 `pump_frames` 是阻塞 read）。正确用法：

```cpp
if (!tcp.wait_readable(0)) return;   // 先探测，无数据立刻返回
auto msg = session.read_message();    // 已确认可读 → 不会阻塞
```

直接调 `try_read_message()` 的现象是「WS 连上了但永远收不到消息」，**且没有任何报错**。

### 2. 不要在事件回调里重建界面树

```js
// ❌ 崩（std::bad_alloc，无 JS 异常）
$('#btn').on('click', () => { list.clear(); 重建... });

// ✅ 置脏，交给主循环
$('#btn').on('click', () => { list.clear(); 重建...; });  // 见下方说明
```

准确地说：**在事件处理器里 `clear()` 掉正在被派发的那个元素**会让宿主手里的引用悬垂，
回调返回后踩内存。相期的做法是回调里只置脏（`dirty = true`），
由 `tick()`（跑在主循环里）统一渲染。

> 框架侧也有防御：`dispatch_event` 在绑定表超过阈值时回收死绑定
> （`prune_dead_bindings`），但那是规模问题，与这条悬垂问题不同。

### 3. 无头模式下窗口尺寸不变

`--headless` 时 `set_size` 不改变截图尺寸（无头后端没有真窗口），
所以 e2e 能验证**内容形态切换**，但验证不了**窗口像素尺寸**。
真窗口下的尺寸变化需要在有头模式手动确认。
