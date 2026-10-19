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
  /// 是否让窗口系统画标题栏/边框。
  ///
  /// **默认 `false`（窗框一律自绘）**——这是框架契约（`CONVENTIONS.md` §10 第 7 条）：
  /// 三平台自带标题栏的字号/高度/圆角/配色各不相同，"一块代码三平台外观一致"会从窗框
  /// 处漏掉；而自绘窗框才与 UI 共用同一套设计令牌、同一套 DPI 口径与**同一份无头截图**。
  /// `true` 仅留给"确实想要系统窗框"的宿主（如系统级调试工具），不是常规路径。
  bool decorations{false};
  /// 渲染器：`auto`（按实测性能选）/ `gpu` / `software`。
  ///
  /// 缺省 `auto`，但 **headless 后端把空串/`auto` 解析为 `software`**（见
  /// `backend.cpp` 的 `HeadlessBackend`）：无头（CI / 截图 / 自动化）根本不需要 GPU，
  /// 而 `auto` 的探测会真的建一个 D3D11 设备——实测代价 +53MB 进程内存、+55 线程、
  /// +4.6GB 虚拟地址（NVIDIA 驱动进程内常驻），且它在**任何像素产生之前**就付掉了。
  /// 想要无头 GPU 时显式传 `gpu` / `auto`，探测照旧。
  std::string renderer{"auto"};
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
  /// 设置窗口指针形态（语义值，各后端自己映射到平台 API）。
  ///
  /// 默认**空实现**：无头后端与不关心的后端不必覆写，也不会因此编译失败。
  /// 这与 `set_present_region` 同一姿态（可选能力，缺省无害）。
  virtual void set_cursor(ui::MouseCursor cursor) { (void)cursor; }
  /// 提交一帧（headless 时写入离屏缓冲）。
  virtual void present() = 0;
  /// 取回输入事件（无则空）。headless 恒为空——输入由控制通道直接投递。
  [[nodiscard]] virtual auto poll_event() -> std::optional<ui::Event> = 0;
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
  virtual auto fit_window_to_work_area() -> Status {
    return unexpected(ErrorCode::Unsupported, "后端不支持窗口自适应");
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
