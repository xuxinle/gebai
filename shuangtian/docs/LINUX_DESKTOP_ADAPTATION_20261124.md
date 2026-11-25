# Linux 桌面适配：实测驱动的能力补齐与缺陷清单（2026-11-24）

> 本文记录一轮**真机驱动**的 Linux 适配：在 Ubuntu 24.04 + GNOME 46（Wayland 会话，
> 应用走 X11/XWayland 与原生 Wayland 两条路）上把霜天全部 9 个应用真跑起来、
> 从**桌面截图**取证，并把过程中暴露的缺陷逐条修掉。
>
> 与前几轮的区别：此前 Linux 侧的证据**全部来自无头**（离屏渲染 + 控制通道 capture）。
> 那一层证明"我画对了"，但证明不了"像素到了屏幕上"——两者之间还有上屏、窗口管理器
> 合成、DPI 三跳。本轮补的就是这条链。

## 1. 验收口径：为什么必须看桌面截图

| | 无头 `--shots` / `capture` | 真实桌面截图 |
|---|---|---|
| 渲染路径 | 软件光栅离屏 | 与用户看到的**同一条** |
| 窗口管理器 | 无 | 真实（去装饰 / 最大化 / Z 序全生效）|
| DPI | 自报 | 系统真实 |
| 能发现 | 绘制缺陷 | 绘制 + **上屏** + 合成 + 窗口语义缺陷 |

**实测证明这条区分不是理论**：`jianmu` 的进程内 `capture` 平均亮度 **56.8（完全正常）**，
而同一时刻的桌面截图是 **7.1（纯黑，只有一张飘过的系统通知卡）**。
只做无头验收，这个缺陷永远不会被发现。

## 2. 修掉的四个真缺陷

### 2.1 X11 剪贴板：`XSetSelectionOwner` / `XConvertSelection` 参数顺序反了

`Xlib.h` 的真实签名（**`selection` 在前、`owner` 在后**）：

```c
int XSetSelectionOwner(Display*, Atom selection, Window owner, Time);
int XConvertSelection(Display*, Atom selection, Atom target, Atom property,
                      Window requestor, Time);
```

旧实现写的是 `(display, window, atom, time)` —— 把 `selection` 与 `owner` 对调，
`requestor` 也放到了第二位。后果是服务器拿 atom 当窗口号 → `BadWindow`，
剪贴板**永远写不进去**（`get_selection_owner` 恒回 `None`）。

**为什么长期没被发现**：错误处理器把 `XEvent*` 当 `XErrorEvent*` 读错了字段偏移
（见 2.2），报出来的错误码是 `192`——那不是任何合法 X 错误码（合法范围 1~17），
于是排查时一直在追一个不存在的问题。

### 2.2 X11 错误处理器的 `XErrorEvent` 字段偏移错位

旧码把 `XEvent*` 当字节数组读 `raw[8]` / `raw[9]` 当错误码与请求号。
`XErrorEvent` 的真实布局（x86_64）：

| 字段 | 偏移 |
|---|---|
| `int type` + 4 字节填充 | 0..7 |
| `Display* display` | 8..15 |
| `XID resourceid` | 16..23 |
| `unsigned long serial` | 24..31 |
| `unsigned char error_code` | **32** |
| `unsigned char request_code` | 33 |
| `unsigned char minor_code` | 34 |

修法：用**结构体**而不是手算偏移，并给错误码/请求号配名字表
（`BadWindow` / `SetSelectionOwner` 而不是 `3` / `22`）。偏移写错不会编译失败，
只会让日志长期骗人——这是"错误处理本身出错"的典型案例。

### 2.3 首帧不上屏（影响所有应用，Windows 之外）

**现象**：`jianmu` 桌面全黑、进程内 capture 正常；其余应用"看起来正常"只是因为
它们首帧之前就有别的重绘时机（凑巧）。

**根因是两件事叠加**：

1. `show_when_ready()` 里补的那次 `blit()` 用的是**启动卡尺寸**的画布
   （应用刚 `resize_hidden_for_startup` 到真实尺寸，但画布还没重建）；
2. 之后画布被尺寸同步重建（内容变空白），而"要不要再画"两处判据
   （`impl_->repaint || root_.dirty()`）都是 false —— 上一帧刚画完。

X11 与 Win32 的真实差异：`XMapWindow` 之后 WM 才真正映射窗口，
**映射前**的 `XShmPutImage` 写的像素会被 WM 的背景填充盖掉；
而 Win32 的 `ShowWindow` 之后 GDI 直写是立即生效的。
旧代码的注释"软件路径的 present 本就是 GDI 直写，所以那一帧已经在屏幕上"
**只对 Win32 成立**。

**修法（两处，各自都对，合起来才不丢帧）**：

- 后端（`platform_x11.cpp`）：`show_when_ready` 里先 `sync_buffers_to_client()`
  再 `blit()`——以窗口实际客户区重建画布与段，补上屏送的才是真实尺寸的内容；
- 应用层（`app.cpp`）：`show_when_ready()` 之后**无条件**标脏并重绘一帧
  （含 `splash_shrunk` 分支）。代价实测 ~12 ms，换来"窗口一露就是完整界面"。

### 2.4 X11 双缓冲槽复用导致"两帧前画面"

**现象**：局部上屏时屏幕闪烁/停留在旧内容。

**根因**：双缓冲的每块槽是**独立的共享内存**，局部上屏只写损坏区——
槽里其余部分是**本槽上一次被用时的内容**，而两块槽是**交替**用的，
所以"上一次"是两帧前。首次时槽是零填充，于是全黑。

Win32 不需要这一步：那边 `CopySubresourceRegion` 拷到**同一块**后备缓冲
（天然保留上一帧内容，`Present` 也不改变它）。

**修法**：每块槽记一个 `slot_primed_`。槽**尚未有过完整帧**时整屏拷一次
（实测 ≈0.9 ms @1706×964，只在每块槽的第一次付），之后只拷损坏区。
段重建（`destroy_shm_slots` / `destroy_backbuffer`）时把位清零。

### 2.5 附带：`present_region_` 从不复位

`set_present_region` 写入、`present` 里读，但读后不清。于是"某一帧设了脏区、
下一帧应用没调 `set_present_region`"时，那次上屏会拿**上一帧的脏区**当范围。
修：`present` 里取完即清（用后即清=一次性的语义）。

## 3. X11 后端补齐的能力（与 Win32 契约对齐）

此前 X11 相对 Win32 缺一整套窗口语义，应用只能降级（日志里一堆"该后端不支持"）。

| 能力 | Win32 | X11（本轮前） | X11（本轮后）|
|---|---|---|---|
| 窗口图标 | `WM_SETICON` | 未实现 | ✅ `_NET_WM_ICON`（4 档尺寸，ARGB 非预乘）|
| 跨进程剪贴板 | 打开/写/读 | **进程内闭环** | ✅ ICCCM 往返（双向真通）|
| 指针形态 | `LoadCursor(OCR_*)` | 未实现 | ✅ `XCreateFontCursor(XC_*)`，11 种形态 |
| 工作区 | `rcWork` | 未实现 | ✅ `_NET_WORKAREA`（按当前桌面号取组）|
| 铺开到工作区 92% | `SetWindowPos` | 未实现 | ✅ 映射后应用（见 §4）|
| 铺满显示器 | `rcMonitor` | 未实现 | ✅ 根窗口尺寸 |
| `set_size` | `SetWindowPos` | 未实现 | ✅ `XResizeWindow` |
| 置底 | `HWND_BOTTOM` 持续监督 | 未实现 | ✅ `_NET_WM_STATE_BELOW`（**一个状态**，不需监督）|
| `restore` | 三层抢前台 | 未实现 | ✅ `XMapWindow` + `_NET_ACTIVE_WINDOW` + `WM_CHANGE_STATE` |
| 启动期原地改尺寸 | 先藏再改 | 未实现 | ✅ 未映射直接改（X11 无需藏）|
| 系统任务栏 | `Shell_TrayWnd` | — | 如实 `Unsupported`（X11 无此概念）|
| 分层上屏 / 系统托盘 / 子窗口嵌入 | 有 | — | 如实 `Unsupported`，见 BACKLOG |

**置底为何不需要监督循环**（这是两个平台真实的行为差异，不是实现偷懒）：
Win32 的 `HWND_BOTTOM` 只管当下，任何一次点击/抢前台都会把它顶到别处，
所以要在 `WM_WINDOWPOSCHANGING` 里持续改写；而 `_NET_WM_STATE_BELOW`
是一个**持续的 WM 状态**，WM 会一直维持它。

## 4. GNOME/Mutter 的自动最大化（一个必须在时序上绕开的平台行为）

实测数据（X11 + GNOME 46，工作区 1854×1048）：

| 映射后多久 resize 到 1706×964（工作区 92%）| 结果 |
|---|---|
| 0 ms（同一批请求）| ★ 1854×1048（整个工作区，`_NET_WM_STATE_MAXIMIZED_*`）|
| 1 ms | ★ 1854×1048（被最大化）|
| 10 ms | 1706×964（正常）|
| 50 / 100 ms | 1706×964（正常）|

即 **GNOME 对"映射那一刻尺寸就接近工作区"的窗口有自动最大化策略**。

**为何这必须绕开而不是"白捡个最大化"**：最大化是**另一个语义**——
自绘窗框会按 `maximized()` 把按钮画成"还原"，而用户点"还原"得到的是
92% 铺开尺寸、点"最大化"又回到整屏——**按钮与行为对不上**。

**修法**：`fit_window_to_work_area` 在窗口未映射时只**记下意图**（`fit_pending_`），
由 `show_when_ready` 在 `XSync` + 15 ms 之后（一个 WM 处理回合）才真正应用。

## 5. Wayland 后端

### 5.1 剪贴板：读取已通，写入受安全模型限制

实现了完整 `wl_data_device` 链路（`data_source` / `data_offer` / MIME 协商）：

- **读**：`data_device.selection` → `data_offer.offer`（收集 MIME）→
  `receive(mime, fd)` 从管道读。✅ 实测通过（GNOME 桥接 X11↔Wayland，双向验证）。
- **写**：新建 `data_source` + `offer(mime)` + `set_selection(source, serial)`。
  ⚠ `serial` 必须是**真实输入事件**的序列号——这是 Wayland 的防滥用机制
  （防后台应用抢剪贴板）。控制通道投递的事件不经过合成器，因此自动化场景拿不到；
  **纯后台无输入的应用在 Wayland 下本来也抢不到剪贴板**（安全模型使然）。
  缺 `serial` 时如实返回 `Unsupported` 并留住进程内缓存（组件层复制粘贴仍可用）。

### 5.2 仍如实不做（不假装成功）

| 能力 | 依据 |
|---|---|
| 窗口图标 | `xdg-shell` 无 `set_icon`，需 `xdg-toplevel-icon-v3`（GNOME 46+ 才支持）|
| 分数缩放 | `wp_fractional_scale_v1`，需配合 `wp_viewporter` 的 `set_destination` |
| 输入法 | 需 `text-input-v3` + `input-method-v2` |
| 拖放（DnD）| 回调表的位都留了合法空实现（表长不齐会越界读），但事件不处理 |

## 6. 验证工具（三个，都可独立跑、都能失败）

| 工具 | 验什么 | 关键判据 |
|---|---|---|
| `tools/linux_desktop_shoot.py` | 9 个应用在真桌面能起、**像素真上屏** | 窗口区域平均亮度 ≥ 20（深色主题实测 32~35，缺陷态 7.1）|
| `tools/linux_window_semantics_check.py` | 窗口图标 / 去装饰 / 工作区铺开 / 尺寸一致 / 置底 / 最大化 / 指针形态 | 全部从 **X 服务器侧**读证据（`_NET_WM_ICON` 段长自洽、`_NET_FRAME_EXTENTS` 不存在、客户区 ≈ 92% 工作区、`_NET_WM_STATE` 含 `BELOW`…）|
| `tools/linux_clipboard_check.py` | 跨进程剪贴板**双向** | 对端优先用 `xsel`（与实现无共享代码），载荷含中文 + emoji |

### 6.1 阈值为什么是 20 而不是 6

第一版写 6，结果 `jianmu` **全黑（7.14）也被判 OK**——那 7.14 全部来自
盖在窗口上的系统通知卡（白色），窗口本身是纯黑。6 只能拦住"完全零输入"，
拦不住"只画了一部分/只剩背景"。20 的依据：本仓实测最低的真实界面是
`gedou`（深色游戏界面，34.6）与 `gallery`（深色主题，32.2），
真实缺陷态是 7.1 —— 20 落在两者之间且两边余量都 > 12。

### 6.2 三处逆向验证（证明判据真的会红）

| 注入的缺陷 | 验证脚本的反应 |
|---|---|
| `SetSelectionOwner` 参数顺序改回错的 | `linux_clipboard_check.py` → "应用写剪贴板失败" |
| 去掉 `show_when_ready` 的 WM 回合等待 | `linux_window_semantics_check.py` → "客户区 1854x1048 与期望 92% 工作区 1705x964 不符" |
| 恢复旧的 `splash_shrunk` 不重绘分支 | `linux_desktop_shoot.py` → "窗口内容亮度 7.14 < 20.0——界面没上屏或只画了背景" |

**没有逆向验证的断言不算断言**——这是本仓的既有纪律（见 `CONVENTIONS.md`），
本轮三个工具都按它做了一遍。

## 7. 控制通道的两处补齐（为了能验证）

窗口语义与剪贴板都是**写给窗口管理器/服务器**的状态，进程内属性面看不到。
要从外部验证"真生效了"，必须能**触发**它们：

- `clipboard` 方法（`get` / `set`）：剪贴板是跨进程能力，
  而它最典型的失败模式是"进程内闭环看起来对、跟别的应用不通"——
  验证脚本要能从外面驱动两侧对账。不用 `script` 是因为后者默认关
  （进程内执行代码），拿它做验收等于要求验收环境开一个危险开关。
- `app` 的 `toggle_maximize` / `stay_at_bottom` / `fit_work_area` / `restore`
  四个动作：经 `ui::WindowControl`（不是 `control::Host`——后者是控制通道的
  最小接口面，不该为几个窗口动作撑大；每加一个纯虚函数所有 Host 实现都要跟改）。

## 8. 实测结果

**9 个应用在真实 GNOME 桌面上全部正常显示**（窗口区域平均亮度）：

| 应用 | 亮度 | 应用 | 亮度 |
|---|---|---|---|
| gallery | 32.2 | cefu | 249.1 |
| gbcode | 252.2 | gedou | 34.6 |
| louyue | 101.1 | xiaobai | 235.2 |
| caiyun | 226.2 | xiangqi | 244.3 |
| jianmu | **69.4**（修复前 7.1）| | |

`jianmu` 的 129 个物体的三维场景在软件光栅腿上正常渲染
（日志：`三维：GPU 腿不可用（仅 D3D11）→ 用软件腿`）。

## 9. 环境备忘（复现要点）

```bash
# 真实桌面会话（本机是 Wayland + XWayland，:0 可访问）
export XAUTHORITY=/run/user/1002/.mutter-Xwaylandauth.0Q5NU3
export DISPLAY=:0
# Wayland 原生路径
export XDG_RUNTIME_DIR=/run/user/1002
export WAYLAND_DISPLAY=wayland-0
```

两个坑：

1. **`XAUTHORITY` 不设就拿不到 `:0`**（会报 "Authorization required"）——
   它不在环境变量里，要显式指向 mutter 的 Xwayland auth 文件；
2. **锁屏状态下截图只能拍到锁屏**（GNOME 的 `org.gnome.ScreenSaver` active）。
   自动化前先 `loginctl unlock-session`，否则所有应用截图都是同一张锁屏壁纸
   ——那会把"没测"伪装成"测过了"。
