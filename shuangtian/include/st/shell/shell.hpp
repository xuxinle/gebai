#pragma once

/// 窗口与事件循环后端：`headless`（离屏，默认）/ `win32` / `x11` / `wayland`。
///
/// 硬约束：平台后端一律**运行时探测**（不产生链接期依赖），缺失或未实现时如实报 `Unsupported`，
/// 无桌面环境自动落到 `headless` —— 无头模式下开发/验证闭环完全可用（控制通道承担输入）。
///
/// 实现状态（**如实**）：`headless` 与 `win32` 已实现；`x11`/`wayland` 目前只做探测与
/// 明确的 `Unsupported` 答复（软件光栅器与 UI 层与平台无关，补后端是纯粹的窗口层工作量）。

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/element.hpp"
#include "st/ui/window_control.hpp"  // 窗框契约（WindowEdge / 缩放带）——定义在 ui 层，理由见该头文件

namespace st::shell {

struct WindowOptions {
  int width{1280};      ///< 逻辑宽（UI 坐标单位）
  int height{720};      ///< 逻辑高
  /// 显示器工作区逻辑尺寸覆盖（0 = 问后端/不知道）。
  ///
  /// 无头后端的唯一用途：**复现“小屏 + 高 DPI”**——那个场景下可用逻辑空间不够，
  /// 窗口必须收缩。真显示器上无需设置（后端自己查）。
  int work_area_width{0};
  int work_area_height{0};
  float scale{1.0f};    ///< DPI 缩放（物理像素 = 逻辑 × scale；1.0 = 传统 96dpi）
  std::string title{"霜天应用"};
  bool headless{false};
  /// 是否可缩放（拖边改尺寸）。
  ///
  /// ⚠ 长期是个**静默失效字段**（只有声明、无人读取，建窗恒用 `WS_OVERLAPPEDWINDOW`）；
  /// 自绘窗框落地时一并接上：`false` = 不带 `WS_THICKFRAME`、边缘不参与缩放命中。
  bool resizable{true};
  /// **建窗时就铺到工作区**（而不是先按 `width`×`height` 建、再改尺寸）。
  ///
  /// ## 为何需要它（2026-11-08，建窗一次到位）
  ///
  /// 旧路径让启动期画布被分配 **4 次**（实测日志逐行）：
  /// ```
  /// 1920x1080   ← 按请求尺寸建窗
  /// 3533x1921   ← fit_work_area 铺开（重分配一次 27MB）
  /// 510x483     ← 启动期小窗
  /// 3533x1922   ← 恢复真实尺寸
  /// 3533x1922   ← 又一次（重复）
  /// ```
  /// 前两次完全可以合并：工作区尺寸在建窗**前**就能算（`MonitorFromPoint` + `rcWork`，
  /// 不需要窗口存在）。本字段为 `true` 时后端先算好目标尺寸再建窗。
  ///
  /// ## 为何做成可选而不是直接改默认
  ///
  /// “建窗尺寸”是 **黑框/白闪历史缺陷高发区**（见 `create_window` 里多处注释）。
  /// 改默认值需要真窗口响应性探针 + 截图对照；做成开关可以先量收益、再决定。
  bool fill_work_area_on_create{false};
  /// 是否让窗口系统画标题栏/边框。
  ///
  /// **默认 `false`（窗框一律自绘）**——这是框架契约（`CONVENTIONS.md` §10 第 7 条）：
  /// 三平台自带标题栏的字号/高度/圆角/配色各不相同，"一块代码三平台外观一致"会从窗框
  /// 处漏掉；而自绘窗框才与 UI 共用同一套设计令牌、同一套 DPI 口径与**同一份无头截图**。
  /// `true` 仅留给"确实想要系统窗框"的宿主（如系统级调试工具），不是常规路径。
  bool decorations{false};
  /// 是否给窗口挂**系统外阴影**（DWM 阴影），让窗口与背景分层。
  ///
  /// **默认 `true`**：自绘窗框拿掉了系统标题栏/边框，也就丢掉了 DWM 给"框架窗口"
  /// 画的那圈阴影。后果不是"少一点装饰"——浅色背景上窗口边缘与桌面融为一体，
  /// 看上去像没有窗口；用户报的就是这个（"和其他窗口不区分、没有层次"）。
  ///
  /// 落地方式（Win32）：`DwmExtendFrameIntoClientArea`（只触发阴影、**不改变客户区尺寸**），
  /// 同时关掉 Win11 随之而来的圆角与 1px 系统描边（否则系统会去改自绘窗框）。
  /// 它与 `decorations` 无关：要系统窗框时 DWM 本来就画阴影，不再重复挂。
  /// 关掉它只为两种场合：远程会话/无合成环境（本来就无阴影），或做 A/B 对比。
  bool window_shadow{true};
  /// 先给软件画布把窗口开出来，GPU 在后台备好后再换型（见 `AppOptions::defer_gpu`）。
  bool defer_gpu{true};
  /// 关窗动作：`true` 且托盘图标已建立时，用户点 × 只**隐藏窗口**（SW_HIDE），
  /// 进程继续常驻（托盘图标还在）；`false`（默认）= 关窗即请求退出。
  ///
  /// 常驻类应用（托盘 + 后台等事件）用它实现“窗口关了但服务还在”。
  bool tray_minimize_on_close{false};
  /// 渲染器：`auto`（按实测性能选）/ `gpu` / `software`。
  ///
  /// 缺省 `auto`，但 **headless 后端把空串/`auto` 解析为 `software`**（见
  /// `backend.cpp` 的 `HeadlessBackend`）：无头（CI / 截图 / 自动化）根本不需要 GPU，
  /// 而 `auto` 的探测会真的建一个 D3D11 设备——实测代价 +53MB 进程内存、+55 线程、
  /// +4.6GB 虚拟地址（NVIDIA 驱动进程内常驻），且它在**任何像素产生之前**就付掉了。
  /// 想要无头 GPU 时显式传 `gpu` / `auto`，探测照旧。
  std::string renderer{"auto"};
  /// **窗口背景磨砂（亚克力）**：让合成器在窗口后面做真模糊，窗口自身画得半透明处透出它。
  ///
  /// ## 为什么是布尔而不是半径/浓度
  ///
  /// 模糊由**合成器**做（Windows: DWM Acrylic；macOS: `NSVisualEffectView`），
  /// 我们的光栅器一次都不参与——实测（`tmp/backdrop_probe3.py`，本仓窗口形态：
  /// `WS_POPUP` + `WM_NCCALCSIZE` 归零 + 自绘窗框）Acrylic 档在窗口内确实透出背景并有
  /// 过渡带（一行 219 个色值），而 Mica/Tabbed 档只采样桌面壁纸、**看不见身后的窗口**。
  /// 半径与浓度不归我们管，暴露它们只会造出一组"设了没反应"的假旋钮。
  ///
  /// ## 何时该开
  ///
  /// 只在"窗口里有一大块半透明区域"时才有意义——当前唯一的调用方是启动卡
  ///（`Application` 的启动首帧）。主界面全程不透明时开着它只是白付合成成本。
  bool backdrop_blur{false};
};

/// 窗口背景能力查询结果（给应用层决定"要不要做磨砂启动卡"）。
enum class BackdropSupport {
  Unsupported,  ///< 该后端没有这条通路（无头 / 未实现 / 旧系统无合成器）
  Requested,    ///< 请求了但**尚未确认生效**（真的生效与否只有合成器知道，见 `set_backdrop_blur`）
  Active,       ///< 已确认生效（读回属性成功）
};

/// 窗口边缘/角落枚举由 `st/ui/window_control.hpp` 定义并在本命名空间**转发**：
///
/// 为什么不在这里另立一个：平台中立的判定规则必须只有一份（Win32 的 `HT*` 命中码、
/// X11 自算、Wayland 交合成器，API 各不相同——"同一次拖拽在两平台差几像素"是最难查的
/// 那类幽灵）。而**两端都要用它**：后端起窗/命中要用，UI 层（`ui::TitleBar`）决定
/// "这次按下是拖动还是缩放"也要用。依赖方向决定了落点——`shell.hpp` 已包含 ui 头，
/// 反向包含会成环，故定义住 ui，这里只转发（调用方仍可写 `shell::WindowEdge`）。
using ui::WindowEdge;
/// 缩放命中带（逻辑像素）：与 UI 层同一常量（见 `ui::kWindowResizeBorder`）。
inline constexpr float k_resize_border = ui::kWindowResizeBorder;

class Backend {
 public:
  virtual ~Backend() = default;

  /// 后端名（"headless" / "x11" / "wayland" / "win32"）。
  [[nodiscard]] virtual auto name() const noexcept -> std::string_view = 0;
  [[nodiscard]] virtual auto headless() const noexcept -> bool = 0;

  virtual auto create_window(const WindowOptions& options) -> Status = 0;
  /// 告知后端**本帧被改动的区域**（逻辑像素；空矩形 = 整帧）。
  ///
  /// 为什么由应用告知而不是后端自己猜：只有应用知道“本帧究竟重画了多少”
  /// （`UiRoot::paint_frame` 会按损坏区大小自动选全量/局部，且会在重排、
  /// 显式整帧标记时保守回落全量）。后端只能看到最终像素，看不出哪些是新的。
  ///
  /// 契约：`present()` 之前的最近一次调用为准；后端**可以忽略它**（默认空实现），
  /// 但不得把它当“必须局部上屏”的命令——传输范围只影响快慢，不影响正确性，
  /// 而后端比应用更清楚自己的上屏机制（比如共享内存上屏要的是“哪些段需要重传”）。
  ///
  /// 受益者：窗口后端把“整屏重排 + 整屏送显”缩到该区域（X11 实测：1080p 下
  /// 重排 0.96 → 0.05 ms、送显 2.3 → 0.85 ms）。无头写入离屏缓冲，默认忽略即可。
  virtual void set_present_region(math::IntRect region) { (void)region; }

  // ── 窗口底色与背景磨砂（启动路径专用，两个都是"可选能力"）──

  /// 设置**窗口底色**（画布尚未画出内容时露出来的那一层）。
  ///
  /// 为何必须有它：底色原先在后端里**硬编码成亮色 `0xF7F7FA`**，于是暗色主题的
  /// 应用启动时会先闪一块浅色底、~400ms 后才跳成深色界面（实测见
  /// `docs/STARTUP_UX_STUDY_20261108.md`）。底色是**产品口径**（跟主题走），
  /// 不是平台细节——所以由应用层告知，而不是后端自己拍。
  ///
  /// 默认空实现：无头后端没有窗口可涂。
  virtual void set_window_background(math::Color color) { (void)color; }

  /// 请求**窗口背景磨砂**（亚克力），返回该后端能做到什么程度。
  ///
  /// 语义：合成器把窗口后面的内容模糊后垫在窗口底下；窗口自己画成半透明的地方透出它。
  /// 我们这边**不需要**分层窗口、不需要第二套上屏通道——画布 alpha=0 的像素会原样透出去
  /// （实测：整块 32bpp DIB `BitBlt` 到窗口后，alpha=0 处仍是背景的模糊像、alpha=255 处不透明；
  /// 见 `tmp/backdrop_probe4.py`）。
  ///
  /// 返回 `Active` 表示**读回属性确认生效**；`Requested` 表示调用了但无法确认；
  /// `Unsupported` 表示这条通路不存在（旧系统/无合成器/无头）——调用方据此降级
  ///（不透明卡代替半透明卡），而不是画出一块"以为透明其实不透明"的怪东西。
  [[nodiscard]] virtual auto set_backdrop_blur(bool enabled) -> BackdropSupport {
    (void)enabled;
    return BackdropSupport::Unsupported;
  }

  /// 启动动画用：把**应用已经画进画布**的内容立刻推到窗口上（不经过 UI 树的排版/绘制）。
  /// 为何单独一个入口而不复用 `present()`：`present()` 的语义是"一帧画完了"，
  /// 它会置 `canvas_painted_`（= 画布里有可信内容，`WM_PAINT` 可以拿它重贴）。
  /// 启动卡是**临时内容**：它后面还要被真正的首帧整屏覆盖；提前置位会让
  /// "黑屏守卫"在动画期失去作用（那段正是最容易被贴出黑块的时刻）。
  /// 所以这条通路**只上屏、不改任何状态位**。
  ///
  /// 默认空实现：无头后端没有窗口。
  virtual void refresh_early() {}

  /// 启动动画期间**暂停延迟 GPU 换型**。
  ///
  /// 为何必须有它：`defer_gpu` 的换型发生在 `framebuffer()` 里（后台线程把 D3D11
  /// 设备备好后的第一个帧边界）。而启动卡必须画在**软件画布**上（`raster::Canvas`）——
  /// 一旦换型，`framebuffer()` 返回的就是 GPU 画布，启动卡就再也画不出去了。
  /// 实测（未加这个接口时）：首帧前第 12 帧（≈410ms，正是 GPU 就绪时刻）起卡消失，
  /// 屏上只剩磨砂底——而帧数与日志都正常，不逐帧打印像素根本查不到。
  ///
  /// 默认空实现：无头后端没有换型这回事。
  virtual void pause_deferred_upgrade(bool paused) { (void)paused; }
  /// 设置窗口指针形态（语义值，各后端自己映射到平台 API）。
  ///
  /// 默认**空实现**：无头后端与不关心的后端不必覆写，也不会因此编译失败。
  /// 这与 `set_present_region` 同一姿态（可选能力，缺省无害）。
  virtual void set_cursor(ui::MouseCursor cursor) { (void)cursor; }
  /// 告知后端**文本插入点在哪**（逻辑像素，客户区坐标）——输入法的候选窗/组合窗贴它。
  ///
  /// 默认空实现：无头后端与不处理输入法的平台不必覆写。
  ///
  /// 为何由**宿主**上报而不是让后端读 UI 焦点树：后端不依赖 `ui` 层
  ///（依赖方向是 shell → ui，但反查会把后端的生命周期绑到 UI 树上）。
  /// 只要界面知道“现在光标在哪”，把它当一条**状态**告诉后端就够了。
  virtual void set_ime_caret(math::Rect logical_rect) { (void)logical_rect; }
  /// 提交一帧（headless 时写入离屏缓冲）。
  virtual void present() = 0;
  /// 取回输入事件（无则空）。headless 恒为空——输入由控制通道直接投递。
  [[nodiscard]] virtual auto poll_event() -> std::optional<ui::Event> = 0;
  /// 泵一次系统消息队列（不取事件、不派发），仅用于**启动慢阶段保活**。
  ///
  /// 为何需要单独的入口：Windows 会把"有窗口但长时间不回消息"的进程标为未响应，
  /// 并显示等待光标（用户看到的"启动时转圈"）。启动时（字体首次光栅化 / 画布分配 /
  /// GPU 设备就绪，实测累计超过 1s）主线程一直忙，消息队列无人处理。
  /// 在各慢阶段边界调一次本函数就能把"未响应"变成"在忙但还活着"。
  /// 不取事件是因为此时界面尚未就位（`started_` 还是 false），派发事件没有意义。
  ///
  /// 默认空实现：后端没有消息队列（headless）时无需做什么。
  virtual void pump_events() {}
  /// 首帧完成**之前**就把窗口显出来（用纯底色填一帧，别让用户看到黑块）。
  ///
  /// 为何需要它：首帧渲染实测 **~1.4 s**（字体首次光栅化 + 图元缓存建表）。
  /// 若等首帧画完才显窗，用户在这 1.4 s 里什么都看不到。
  ///
  /// 与 `show_when_ready` 的分工：
  /// - `show_early`：启动阶段把窗口映射出来 + 填主题底色（避免白底/黑块）；
  /// - `show_when_ready`：首帧画完后正常显示（幂等；已显示时只补一次 present）。
  ///
  /// ⚠ **它不消除等待光标**：首帧仍是一整块不行分割的工作，主线程期间不处理消息。
  /// 要消除等待圈得降首帧耗时（文字首次光栅化占大头），那是独立议题。
  ///
  /// 默认空实现：无头后端没有窗口，无需做什么。
  virtual void show_early() {}
  virtual void set_title(std::string_view title) = 0;
  /// 设置**窗口/任务栏图标**（PNG 字节；失败时如实返回错误，不静默）。
  ///
  /// 为何是 PNG 字节而不是文件路径：图标必须随可执行文件走（应用是单文件交付的，
  /// 旁边不一定有资源目录），而“编译期嵌入”后的形态就是一段内存字节。
  /// 传路径会把“资源在哪”这个部署问题推给后端。
  ///
  /// 为何是 PNG 而不是预乘 RGBA：框架自带 PNG 解码器（`st::codec`），
  /// 而 PNG 保留了"多尺寸候选"的可能（各后端需要不同尺寸时自己扣）。
  ///
  /// 默认实现返回 `Unsupported`——**无头后端与未实现窗口图标的平台如实拒绝**，
  /// 而不是假装成功（与 `supports_window_control` 同一姿态）。
  [[nodiscard]] virtual auto set_window_icon(std::span<const std::uint8_t> png) -> Status {
    (void)png;
    return unexpected(ErrorCode::Unsupported, "该后端不支持设置窗口图标");
  }
  /// 后端是否真能设置窗口图标（供宿主决定是否上报/提示）。
  [[nodiscard]] virtual auto supports_window_icon() const noexcept -> bool { return false; }

  // ── 系统托盘（可选能力，默认不支持）──
  //
  // 设计姿态与 `set_window_icon` 一致：**未实现就如实返回 Unsupported**，
  // 不假装成功（否则调用方以为托盘出来了，实际用户看不到任何东西）。
  //
  // 为何做成后端能力而不是应用自己调 Win32 API：
  // 1. 托盘是**桌面环境概念**（Windows 通知区 / Linux StatusNotifier / macOS 菜单栏），
  //    各平台机制完全不同；让每个应用自己写会把可移植性彻底丢掉。
  // 2. 它需要跟窗口生命周期一起管理（窗口销毁前必须移除图标，否则会留下幽灵图标，
  //    鼠标划过才消失）——这正是后端的职责。

  /// 创建/更新托盘图标。`png` 为图标字节（空 span = 用当前图标）。
  ///
  /// **幂等**：重复调用只是更新提示文字/图标，不会创建第二个图标。
  /// 默认实现返回 `Unsupported`。
  [[nodiscard]] virtual auto set_tray_icon(std::span<const std::uint8_t> png,
                                           std::string_view tooltip) -> Status {
    (void)png;
    (void)tooltip;
    return unexpected(ErrorCode::Unsupported, "该后端不支持系统托盘");
  }

  /// 移除托盘图标（无图标时为空操作）。
  virtual void remove_tray_icon() {}

  /// 后端是否支持系统托盘。
  [[nodiscard]] virtual auto supports_tray_icon() const noexcept -> bool { return false; }

  /// 取回托盘图标被点击的次数（自上次调用以来的增量）。
  ///
  /// 为何用"计数"而不是事件流：托盘点击的语义极简（就是"把我叫回来"），
  /// 没必要为它引入一套事件类型。宿主每帧取一次，>0 就做对应的窗口操作。
  [[nodiscard]] virtual auto take_tray_clicks() noexcept -> std::uint32_t { return 0; }
  [[nodiscard]] virtual auto clipboard_text() -> Result<std::string> = 0;
  [[nodiscard]] virtual auto set_clipboard_text(std::string_view text) -> Status = 0;
  /// 目标帧缓冲（应用直接绘制到这里）。
  [[nodiscard]] virtual auto framebuffer() -> raster::Surface& = 0;
  /// 已提交帧数。
  [[nodiscard]] virtual auto frame_count() const noexcept -> std::uint64_t = 0;
  /// 当前 DPI 缩放（物理像素 / 逻辑像素）。
  [[nodiscard]] virtual auto device_scale() const noexcept -> float = 0;
  /// 当前实际使用的渲染器名（"software" / "gpu"）与选择理由。
  ///
  /// 为什么必须可上报：`auto` 是**按实测性能**选的，用户有权知道这一帧是谁画的、
  /// 以及为什么——"不知道自己用的是哪个渲染器"会让性能问题无从排查。
  [[nodiscard]] virtual auto renderer_name() const noexcept -> std::string_view {
    return "software";
  }
  [[nodiscard]] virtual auto renderer_note() const -> std::string { return {}; }
  /// 上屏路径的**如实描述**（如 X11 的 `MIT-SHM` 还是 `XPutImage`）。
  ///
  /// 为何必须可上报：上屏是窗口后端单帧最大的一段固定开销（1080p 实测：
  /// 整屏 `XPutImage` 17.9 ms/帧），而它**走哪条路取决于环境**（libXext 在不在、
  /// 服务端支不支持 MIT-SHM、远程 X 转发与否）。回退了就得能看见——
  /// 否则“这台机器为什么掉帧”会变成谜案（与 `font_read_fallbacks` 同一条理由）。
  [[nodiscard]] virtual auto present_note() const -> std::string { return {}; }
  /// “本该上屏却没上屏”的帧数（DIB 建不出来 / 拿不到窗口 DC 等）。
  ///
  /// 为何需要它：**“调了 present 但没送到屏幕”**与“每帧都正常上屏”在上层
  /// 完全不可区分——`present_ms` 照样有值、主循环照样在转，而用户看到的是空白窗口
  /// （2026-10-20 的软件路径空白窗口就是这个形态）。>0 就是真问题，必须能看见。
  ///
  /// 无头后端无需实现（它本来就不“上屏”，默认 0 是对的语义）。
  [[nodiscard]] virtual auto present_skipped() const noexcept -> std::uint64_t { return 0; }
  /// 运行时切换 DPI（重分配帧缓冲；无头与窗口模式一致）。
  virtual auto set_device_scale(float scale) -> Status = 0;
  /// 逻辑视口尺寸（窗口尺寸，不含 DPI 放大）。
  [[nodiscard]] virtual auto logical_size() const noexcept -> math::Size = 0;
  /// 当前显示器上**可用工作区的逻辑尺寸**（已扣除任务栏/系统栏）。
  ///
  /// 为何需要它（DPI 适配的基础）：窗口初始尺寸若写死逻辑值，在高 DPI 的小屏幕上
  /// 会**超屏**——例如 13 寸 1080p 屏开 200% 缩放，可用逻辑区只有 960×540，
  /// 而应用要 1280×800，于是窗口被系统裁切/挤压，界面看着“错乱且不可用”。
  /// 应用拿这个值把初始尺寸**夹到屏幕范围内**，才是跨 DPI 可用的做法。
  ///
  /// 默认实现返回 `logical_size()`（= 当前窗口尺寸），语义是“不知道屏幕有多大，
  /// 就当窗口本身是全部可用空间”——不改变既有后端的行为。
  [[nodiscard]] virtual auto work_area_logical() const noexcept -> math::Size {
    return logical_size();
  }
  /// 这台显示器能给的最小逻辑视口（低于它界面就挤不动了）。
  /// 与 `work_area_logical` 配合：应用取 `min(期望尺寸, 工作区)` 且不低于它。
  [[nodiscard]] virtual auto min_logical_size() const noexcept -> math::Size {
    return math::Size{0.0f, 0.0f};
  }
  /// 把当前窗口**夹到屏幕工作区内**（超屏才缩，不超就不动）。
  ///
  /// 返回 `ok()` = 已经处理（可能未变动）；`Unsupported` = 后端不知道屏幕多大，
  /// 调用方不该把它当错——无头模式就是这种情形。
  ///
  /// 为何是后端职责而非应用自己算：窗口客户区/边框/DPI 换算都在后端手里，
  /// 应用只知道“我希望能有多宽”。把换算放在应用里会变成两套坐标系。
  ///
  /// 本函数**只管尺寸**；位置由 `center_window` 单独负责。两者分开是因为
  /// 调用时机不同：尺寸要按工作区算（主窗口铺开），而位置在任何尺寸下都该居中
  /// （含 `fill_work_area = false` 的小窗口应用——它们跳过尺寸收敛，但仍要居中）。
  virtual auto fit_window_to_work_area() -> Status {
    return unexpected(ErrorCode::Unsupported, "后端不支持窗口自适应");
  }
  /// 把窗口**居中到它所在那块显示器的工作区**（尺寸不动）。
  ///
  /// 返回 `ok()` = 已经处理（可能本来就在中间）；`Unsupported` = 后端无法摆放窗口，
  /// 调用方不该把它当错。
  ///
  /// 为何需要它（而不是让各后端在别处顺手居中）：位置这件事有三个**必须同时成立**
  /// 的口径——① 基准是**窗口所在那块屏**的工作区（多显示器下不是主屏）；
  /// ② 换算在**物理**像素里做，因为窗口系统只认物理坐标；
  /// ③ 只移动不改大小。三者分散到各后端的建窗/改尺寸/自适应路径里，就必然分家。
  /// 实测：X11/Wayland 建窗落在 (0,0)（屏幕左上角），只有 Win32 在
  /// `fit_window_to_work_area` 里顺手居中过——于是同一个应用在不同平台开局位置不同。
  ///
  /// 默认实现返回 `Unsupported`：无头没有“屏幕”，居中是空操作。
  virtual auto center_window() -> Status {
    return unexpected(ErrorCode::Unsupported, "后端不支持窗口定位");
  }
  /// 窗口系统是否请求关闭（用户点关闭按钮）。
  ///
  /// 由平台后端实现；应用主循环据此收尾（不能直接 `exit`——进程内还有控制通道、
  /// 脚本宿主等资源需要正常停止）。
  [[nodiscard]] virtual auto close_requested() const noexcept -> bool { return false; }

  /// 把窗口显式展示出来（首帧画完之后调）。
  ///
  /// **为什么窗口不能建完就显**：`CreateWindowExW` 后立即 `ShowWindow` 会让窗口
  /// 先露出「系统预备的白底 / 未初始化内容」，而此时画布还在分配（内含渲染器实测
  /// 基准，实测 ~570ms）；`resize_window_to_scale` 放大到真实 DPI 尺寸时，多出来
  /// 的区域又没任何东西可贴 → **启动时右下/底部一大块黑**（实测 0.6 秒后自行消失，
  /// 因为主循环第一帧终于画上了）。
  ///
  /// 约定：`create_window` 只建不显；应用在**首帧 `present()` 之后**调本函数。
  /// 无头后端不需观感，默认空实现即可。
  virtual void show_when_ready() {}

  /// 启动期**原地改窗口尺寸**（不把中间态露给用户）。
  ///
  /// ## 为何需要新接口而不能直接 `set_size`（2026-11-08 实测）
  ///
  /// 启动卡阶段窗口只有卡那么大（~447×403），首帧画完要拉回真实尺寸（~2355×1281）。
  /// 直接用 `set_size` 的后果（用户截图发现的）：
  ///
  /// | 时刻 | 窗口 | 屏幕 |
  /// |---|---|---|
  /// | 251ms | 671×644 卡 | 卡 ✓ |
  /// | **458ms** | **3533×1922** | **整片纯白** |
  /// | 3006ms | 3533×1922 | 仍然纯白（首帧要 366ms） |
  ///
  /// 即“卡 → 一眨眼变全屏白板 → 白板停 366ms → 内容才出来”，比不做启动卡还难看。
  /// 而首帧那 366ms 是**必须**在大画布上完成的（字体首次光栅化 + 缓存建表）。
  ///
  /// 所以这里把“改尺寸”包成：**先藏 → 改 → 由 `show_when_ready` 再显**。
  /// 用户看到的是“小窗里的卡”直接变成“完整界面”，中间的白板一帧也不出现。
  ///
  /// 无头/不支持隐藏的后端退化为“直接改尺寸”（它们本来也没有“看到中间态”这回事）。
  virtual auto resize_hidden_for_startup(int width_logical, int height_logical) -> Status {
    return set_size(width_logical, height_logical);
  }

  /// 让窗口**只开分层通道**（`WS_EX_LAYERED` + 逐像素 alpha），不开磨砂。
  ///
  /// ## 为何单独成接口（2026-11-08，用户“只看到悬浮的动画”）
  ///
  /// 启动卡要让**卡外真透明**（透出桌面），这只能靠逐像素 alpha，
  /// 而它只能经 `WS_EX_LAYERED` + `UpdateLayeredWindow` 送给合成器。
  /// 但磨砂（Acrylic）是**整窗材质**，开了就铺满整个窗口——
  /// 实测窗口角 `#1E1E20` vs 桌面 `#15151A`：用户看到的是一块方块。
  ///
  /// 所以要有一个“只要分层、不要磨砂”的入口。实测此时窗口角与桌面
  /// **完全一致**（`tools/backdrop_level_probe.cpp`）：只有卡悬浮在桌面上。
  ///
  /// 默认实现返回 Unsupported（无头后端不需要这个）。
  [[nodiscard]] virtual auto set_layered_present(bool enabled) -> Status {
    (void)enabled;
    return unexpected(ErrorCode::Unsupported, "该后端不支持分层上屏");
  }

  /// 安装「拖动/缩放时立即重绘」的钩子（由 app 层安装；后端不认识 app 层）。
  ///
  /// **为什么必须有它**：用户拖边框走的是窗口系统的模态循环
  /// （Win32：`DefWindowProc` 收 `SC_SIZE` 后自己跑消息循环），它**阻塞应用主循环**——
  /// 期间 `WM_SIZE`/`WM_PAINT` 是唯一的执行机会。不在这里驱动一次渲染，屏幕就只能
  /// 停在旧内容上：实测拖动 30 步、`metrics.frames` 只涨 **0~2**，而"旧尺寸的帧被
  /// DXGI 拉到新客户区"的逐像素差异高达 `mean|Δ|=19.3`（**21.8% 像素明显不同**）——
  /// 这就是用户看到的"拖动时被拉伸扭曲、松手才重画"。
  ///
  /// 语义：后端在"尺寸已变、需要重画"时调用它；实现必须**同步完成一帧**
  /// （重建缓冲 → 布局 → 绘制 → 送显）。后端要保证重入安全（多次调用不会嵌套成环）。
  virtual void set_resize_repaint(std::function<void()> repaint) { (void)repaint; }

  // —— 自绘窗框所需的窗口控制 ——
  //
  // 存在的理由（`CONVENTIONS.md` §10 第 7 条）：窗框一律自绘，而"最小化/最大化/关闭/
  // 拖动/缩放"只有窗口系统能做。两端之间必须有**一份平台中立的契约**，否则每个应用
  // 都要自己写三份平台分支（那正是 §10 第 1 条禁止的）。
  //
  // 与 UI 层的 `ui::WindowControl` 端口是**两件事**：这一层是平台后端的能力，
  // `ui::WindowControl` 是组件看到的抽象（`ui` 不认识 `shell`，依赖方向见
  // `st/ui/window_control.hpp`）。`st::app::Application` 把两者接上（转发 + 把
  // `Status` 折成 `bool`），因此 UI 与后端之间不产生任何直接依赖。
  //
  // 契约：**所有平台语义一致**；不支持时**如实**报 `Unsupported`（不静默无效——
  // "点了没反应"与"压根没这个能力"是两件事，调用方要靠返回值区分）。

  /// 是否具备窗口控制能力（无头后端为 `false`；UI 据此如实禁用按钮而不是画一个点不动的装饰）。
  [[nodiscard]] virtual auto supports_window_control() const noexcept -> bool { return false; }
  /// 最小化。
  [[nodiscard]] virtual auto minimize() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }

  /// 从最小化/后台恢复到前台并取得焦点。
  ///
  /// 常驻应用（托盘类）的核心交互就是这一个动作。默认拒绝（未实现的后端不假装成功）。
  [[nodiscard]] virtual auto restore() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持恢复窗口");
  }

  /// 运行时改变窗口的逻辑尺寸（像素为逻辑坐标）。
  ///
  /// 为何需要：常驻面板要能“静默时缩成一条”、“有事时展开”——若尺寸只能建窗时定死，
  /// 就只能开多个窗口，而多窗口在无头后端/截图验证里都不好办。
  /// 它也是轻提示形态的基础：同一个窗口先缩到小尺寸贴屏幕角落，再展开就回到主界面。
  ///
  /// 默认拒绝；实现方需自行处理画布重建（已有 `WM_SIZE` 路径的后端几乎不需额外工作）。
  [[nodiscard]] virtual auto set_size(int width_logical, int height_logical) -> Status {
    (void)width_logical;
    (void)height_logical;
    return unexpected(ErrorCode::Unsupported, "该后端不支持运行时改变窗口尺寸");
  }
  /// 最大化/还原（切换）。
  [[nodiscard]] virtual auto toggle_maximize() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }
  /// 请求关闭——走与用户点窗口关闭按钮**同一条收尾路径**（置 `close_requested`，
  /// 由应用主循环退出；不能直接 `exit`，进程内还有控制通道/脚本宿主要正常停止）。
  [[nodiscard]] virtual auto request_close() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }
  /// 开始拖动窗口（自绘标题栏按下时调用）：进入窗口系统的移动模态循环。
  [[nodiscard]] virtual auto begin_move() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }
  /// 开始缩放窗口（自绘窗框边缘按下时调用；`edge` 为 `ui::resize_edge_at` 的判定结果）。
  [[nodiscard]] virtual auto begin_resize(WindowEdge edge) -> Status {
    (void)edge;
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }
  /// 窗口当前是否最大化（自绘窗框据此切换按钮形态）。
  [[nodiscard]] virtual auto maximized() const noexcept -> bool { return false; }
};

/// 可复用纯函数：**逻辑坐标的损坏区 → 物理像素的上屏区域**。
///
/// 为什么住这里而不是后端内部：窗口后端（X11/Wayland）都要做这一步，而
/// “弄错一个边界就是漏画一条边”是那类只能在截图上看到、且很难归因的缺陷
/// （全自绘框架里没有系统合成器兜底）。纯函数 + 单测才可能钉住它。
///
/// 语义：
/// - `region` 为空（宽或高 ≤ 0）→ 返回**整块画布**（{0,0,width,height}）——
///   “没说改哪里”最安全的解释就是“哪里都可能改了”；
/// - 否则按 `scale` 换算：左/上**向下取整**、右/下**向上取整**（故意向外取整——
///   宁可多发一列像素也不能漏一条边缘；`round` 会把 100.4 的右缘收成 100，漏掉半像素）；
/// - 再夹到画布内；夹完为空（区域完全在画布外）→ 返回空矩形，调用方跳过本帧。
[[nodiscard]] auto resolve_present_area(math::IntRect region, float scale, int width,
                                        int height) noexcept -> math::IntRect;

/// 窗口系统后端专用：它比无头多一件事——**把画布像素搬进窗口系统的缓冲**。
/// `st::shell::Backend` 已经是那个契约面（含 `set_present_region` / `present_note`），
/// 这里不再另立一个接口。

/// 可复用纯函数：**居中摆放的窗口原点**（物理像素）。
///
/// 为什么抽出来：两个平台后端（Win32 的 `SetWindowPos`、X11 的 `XMoveWindow`）
/// 都要算它，而“居中偏了 1px / 偏到另一块屏”只在真窗口上看得到
/// （与 `resolve_present_area` 同一理由：全自绘框架里没有系统合成器兜底，
/// 纯函数 + 单测才钉得住）。
///
/// 语义：
/// - 返回的是**窗口左上角**的全局坐标（相对那塊屏的工作区原点，含其偏移）；
/// - 摆不下时**夹到工作区左上角**（`max(0, …)`）——不能把窗口推到屏外；
/// - 两个轴的除法都**向下取整**（与 Win32 行为一致；奇差 1px 的那半像素归左/上）。
[[nodiscard]] auto centered_origin(math::IntRect window, math::IntRect work_area) noexcept
    -> math::IntPoint;

/// 是否检测到显示服务（DISPLAY / WAYLAND_DISPLAY）。
[[nodiscard]] auto has_display() noexcept -> bool;
/// 探测可用的图形后端名（无显示时返回 "headless"）。
[[nodiscard]] auto probe_backend() -> std::string;
/// 系统显示缩放（物理像素 / 逻辑像素）：Windows 取主显示器 DPI/96（不声明 DPI 感知
/// 的进程会被虚拟化为 96，因此实现里先声明感知）；其它平台无系统级缩放概念，返回 1.0。
///
/// 用途：无头后端在未显式指定 `scale` 时**跟随系统**——内置通道的截图/坐标
/// 与桌面窗口取同一像素密度，“内置里看着对不对”与“实机跑起来”才不会两套。
[[nodiscard]] auto system_display_scale() noexcept -> float;
/// Windows 实现（platform_win32.cpp）。经 `system_display_scale` 调用，非 Windows 不会触达。
[[nodiscard]] auto win32_display_scale() noexcept -> float;
/// 创建平台专属后端（各 `platform_*.cpp` 提供；未实现的平台在工厂里如实报 `Unsupported`）。
[[nodiscard]] auto create_win32_backend() -> Result<std::unique_ptr<Backend>>;
/// X11 后端（`src/shell/platform_x11.cpp`）：**运行期 `dlopen` 加载 libX11**，
/// 不产生链接期依赖（`CONVENTIONS.md` §10 第 1 条）。无 libX11 / 无 `DISPLAY` 时
/// 如实返回 `Unsupported`，由上层回退 headless。
[[nodiscard]] auto create_x11_backend() -> Result<std::unique_ptr<Backend>>;
/// Wayland 后端（`src/shell/platform_wayland.cpp`）：同上，运行期加载 `libwayland-client`。
[[nodiscard]] auto create_wayland_backend() -> Result<std::unique_ptr<Backend>>;

/// 创建后端（`name` 为空=自动选择：有显示则尝试平台后端，失败回退 headless）。
[[nodiscard]] auto create_backend(std::string_view name = {}) -> Result<std::unique_ptr<Backend>>;

/// 合成负载微基准（真实画布尺寸上跑一组有代表性的原语，返回**中位数**毫秒）。
///
/// 用途：`--renderer auto` 的判据。两个后端共用同一份实现——"更快"的定义只能有一处，
/// 否则窗口与离屏会得出不同结论。
///
/// ⚠ 它**不含 `present()`**：窗口呈现的开销（是否走 swapchain）不在这个数字里，
/// 由 `renderer_note` 如实报出，不混进本基准。
[[nodiscard]] auto benchmark_surface(raster::Surface& target, int runs) -> double;

}  // namespace st::shell
