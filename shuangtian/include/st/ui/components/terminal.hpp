/// 终端（Terminal）：**真终端**面板——伪终端 + ANSI 屏幕模型 + 字节级输入。
///
/// ## 什么叫"真终端"
///
/// 判据不是"看起来像"，而是**技术上是不是**：
///
/// * 子进程连着**伪终端**（`isatty()` 为真 ⇒ `ls` 上色、`vim` 能开、行规程做行内编辑）
/// * 输出是**原始字节流**（ANSI 转义序列），经 `st::text::AnsiScreen` 解释成屏幕网格
/// * 输入是**字节**——按键原样写给 PTY，**行内编辑/补全/历史全交给 shell**
/// * 光标是**屏幕状态**（`AnsiScreen` 报位置，渲染时画出来），不是一个独立的输入框光标
/// * 窗口尺寸变化**上报**给子进程（TUI 据此重排）
///
/// 与之相对的是"输出区 + 输入框"：那是**假的**终端——提示符是应用拼的、
/// 行内编辑是应用做的、`vim` 直接报错。本组件不做那种形态。
///
/// ## 分层
///
/// ```
/// Terminal（本组件：标签 / 屏幕渲染 / 键盘 → 字节 / 尺寸上报）
///    ├─ st::process::PtySession   （平台层：ConPTY / openpty）
///    ├─ st::text::AnsiScreen      （解释字节流为屏幕）
///    └─ st::exec::Channel         （可插拔执行通道：本地 PTY / SSH / 假通道）
/// ```
///
/// ## 两条模式
///
/// 1. **PTY 模式（默认）**：`open_shell()` 起一条真 shell。这是终端该有的样子。
/// 2. **行模式**：`run()` + `append()`，给**不需要 tty** 的场景（"跑一次构建看退出码"）
///    ——那些场景用 PTY 反而多一层（行规程会改写输出、退出码要穿透 shell）。
///
/// 宿主按需选；同一条会话不会同时用两种模式。

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "st/core/channel.hpp"
#include "st/core/pty.hpp"
#include "st/math/geometry.hpp"
#include "st/text/ansi_screen.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

class ScrollView;
class Tabs;
class Text;

/// 终端会话（一个标签）。
struct TerminalSession {
  /// 标签文字。
  std::string title{};

  // —— PTY 模式 ——
  /// 伪终端（nullptr = 本会话不在 PTY 模式）。
  ///
  /// `shared_ptr` 而不是 `unique_ptr`：读线程要持有它（线程活过会话容器的重排），
  /// 且 `terminate` 之后读线程还要靠它拿退出码。引用计数在这里是最省心的所有权。
  std::shared_ptr<st::process::PtySession> pty{};
  /// 读线程（`jthread`：析构 join，不用裸 `detach`）。
  std::jthread reader{};
  /// **屏幕模型**：PTY 的字节流解释成的网格（含光标、样式、备用屏）。
  std::shared_ptr<st::text::AnsiScreen> screen{};
  /// **字节缓冲（按会话分）**：读线程 → 主线程。多会话各自的输出互不相混。
  ///
  /// 旧实现是组件级单缓冲，两个标签各自起 shell 后字节会互相串台
  ///（实测：后台标签的输出喂进前台标签的屏幕——乱码）。锁是组件级的
  /// `pending_mutex_`（锁粒度小、争用可忽略，不值得每会话一把）。
  std::vector<std::string> pty_pending{};
  /// PTY 是否已退出、退出码多少（读线程置位，`pump` 收尾）。
  bool pty_exited{false};
  int pty_exit_code{0};
  /// 子进程/作业是否在跑。
  ///
  /// PTY 模式下它的含义是**shell 还活着**（不是“命令在跑”）——真终端里
  /// “当前有没有命令在执行”由 shell 自己的状态决定，组件看不到（
  /// 那要读 shell 的提示符/类型），因此 `busy()` 在这里回答的是
  /// “这条会话能不能用”。`send_stop` 发 `Ctrl+C` 不会把它置假。
  bool running{false};
  /// 运行中的命令名（PTY 模式下是 shell 名；行模式下是命令）。
  std::string running_name{};
  /// 子进程退出码（退出后可读）。
  int exit_code{0};
  /// 用户主动往上滚过（暂停贴底跟随）。
  bool user_scrolled{false};

  // —— 行模式（非 PTY）——
  /// 滚回：**分块**存（追加 O(新增)，截断只丢头几块）。
  std::vector<std::string> chunks{};
  /// 命令历史（行模式下由组件记；PTY 模式下 shell 自己记，这里不用）。
  std::vector<std::string> history{};

  /// 生效的工作目录（PTY 起 shell 时用；`cd` 在 PTY 模式下归 shell 管）。
  std::string cwd{};

  /// 回看滚动偏移（**行数**，0 = 贴底看屏幕现在；>0 = 往上翻了多少行）。
  ///
  /// 与 `user_scrolled` 的关系：它记录精确位置（滚轮事件推进/回退；
  /// `user_scrolled` 仍是"是否不贴底"的布尔，供行为模式判断——两个状态同步维护）。
  int scrollback_offset{0};

  [[nodiscard]] auto in_pty_mode() const noexcept -> bool { return pty != nullptr; }
};

class Terminal : public Element {
 public:
  /// 行模式滚回上限（**行**）。
  static constexpr std::size_t kMaxLines{4000};

  Terminal();
  ~Terminal() override;
  Terminal(const Terminal&) = delete;
  auto operator=(const Terminal&) -> Terminal& = delete;
  Terminal(Terminal&&) = delete;
  auto operator=(Terminal&&) -> Terminal& = delete;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Terminal"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Code; }

  // ────────────────────────────────────────────────────────────────────────
  // 会话（标签）
  // ────────────────────────────────────────────────────────────────────────

  /// 会话数（构造时自动建第一个，**至少为 1**）。
  [[nodiscard]] auto session_count() const noexcept -> std::size_t { return sessions_.size(); }
  /// 新建会话；返回序号。新会话继承当前会话的工作目录。
  auto add_session(std::string title = {}) -> std::size_t;
  /// 关闭会话。**拒绝关闭有作业在跑的**（返回 false，理由经 `on_error`）。
  /// 关掉最后一个不留空面板（补一个全新的）；是否收起面板由宿主定。
  auto close_session(std::size_t index) -> bool;
  void set_active_session(std::size_t index);
  [[nodiscard]] auto active_session() const noexcept -> std::size_t { return active_; }
  [[nodiscard]] auto session(std::size_t index) -> TerminalSession*;
  [[nodiscard]] auto session(std::size_t index) const -> const TerminalSession*;
  void set_session_title(std::size_t index, std::string title);

  // ────────────────────────────────────────────────────────────────────────
  // PTY 模式（真终端）
  // ────────────────────────────────────────────────────────────────────────

  /// 在当前会话里起一条**真 shell**（伪终端 + 屏幕模型）。
  ///
  /// 真实尺寸由 `size` 决定（TUI 程序据此排版）；不传则用上次布局量到的尺寸。
  /// 重复调用会被忽略（已有一条在跑）。
  ///
  /// 失败经 `on_error` 报出（如老系统上 ConPTY 不可用）。
  void open_shell(st::process::PtySize size = {});

  /// 当前会话是否在 PTY 模式且子进程活着。
  [[nodiscard]] auto pty_active() const -> bool;

  /// 直接绑定一个已经建好的 PTY（宿主要自己控制创建过程时用；少见）。
  void attach_pty(std::shared_ptr<st::process::PtySession> pty);

  /// 键盘输入（**原始字节**，逐键写回 PTY）。
  ///
  /// 这是真终端的输入方式：`Tab`/方向键/`Ctrl+C` 都只是字节，
  /// 补全与行内编辑由 shell 与内核行规程做——组件不模拟它们。
  void send_bytes(std::string_view bytes);

  /// 上报窗口尺寸（布局变化时调用；子进程收到 `SIGWINCH`）。
  void set_terminal_size(st::process::PtySize size);

  // ────────────────────────────────────────────────────────────────────────
  // 行模式（非 PTY：不需要 tty 的场景）
  // ────────────────────────────────────────────────────────────────────────

  /// 通道工厂（缺省 = 本机子进程管道）。换它就能换执行方式（SSH / 测试桩）。
  void set_channel_factory(st::exec::ChannelFactory factory);
  [[nodiscard]] auto channel_factory() const -> const st::exec::ChannelFactory& {
    return factory_;
  }
  /// 通道描述（宿主改它来换目标主机/类型）。
  [[nodiscard]] auto channel_spec() -> st::exec::ChannelSpec& { return spec_; }
  [[nodiscard]] auto channel_spec() const -> const st::exec::ChannelSpec& { return spec_; }

  /// 设 shell（行模式用；PTY 模式另见 `set_pty_shell`）。
  void set_shell(std::string program, std::vector<std::string> args = {});
  [[nodiscard]] auto shell_program() const noexcept -> const std::string& { return shell_; }

  /// 默认工作目录（会话没设 `cwd` 时用它）。
  void set_working_directory(std::string dir);
  [[nodiscard]] auto working_directory() const -> std::string;

  /// 行模式：在当前会话里起一条命令（异步，输出经 `pump` 回流）。
  void run(std::string command);
  /// 行模式：直接起一个程序（宿主自己做了解析/白名单时用）。
  void run_program(std::string program, std::vector<std::string> args, std::string display);

  /// 中止：PTY 模式杀子进程；行模式终止通道。
  void send_stop();
  /// 清屏（PTY 模式清屏幕模型；行模式清滚回）。
  void clear();

  /// **当前会话**是否忙。⚠ 只回答"眼前这个标签"；问"还有没有活"用 `any_busy()`。
  ///
  /// PTY 模式下语义是“shell 还活着”（见 `TerminalSession::running`）；
  /// 行模式下是“作业在跑”。
  [[nodiscard]] auto busy() const -> bool;
  /// **任意**会话是否有作业在跑。
  [[nodiscard]] auto any_busy() const -> bool;

  /// 命令过滤钩子（**行模式**）：返回非空 = 拒绝执行，字符串作为理由显示。
  /// PTY 模式下不经过它——真 shell 里一切都能跑，策略该由 shell 层的权限模型管。
  std::function<std::string(const std::string& command)> before_run{};

  // ────────────────────────────────────────────────────────────────────────
  // 输出（行模式 / 宿主自己写文本）
  // ────────────────────────────────────────────────────────────────────────

  void append(std::string line);
  void append_to(std::size_t index, std::string line);
  /// 会话的**纯文本**（PTY 模式取屏幕内容；行模式拼接滚回）。
  [[nodiscard]] auto session_text(std::size_t index) const -> std::string;
  [[nodiscard]] auto line_count(std::size_t index) const -> std::size_t;
  /// 屏幕模型（PTY 模式下非空；渲染与断言用）。
  [[nodiscard]] auto screen(std::size_t index) const
      -> const st::text::AnsiScreen*;

  // ────────────────────────────────────────────────────────────────────────
  // 回调
  // ────────────────────────────────────────────────────────────────────────

  std::function<void(std::size_t)> on_session_change{};
  std::function<void(std::size_t)> on_session_close{};
  /// 关闭**是否关掉了最后一个**（宿主据此决定收不收面板）。
  ///
  /// 为何要单独一个回调：`on_session_close` 里判不出——`apply_pending_close`
  /// 会立即补一个全新会话，于是 `session_count()` 恒为 1。
  /// 两个标签关一个不该收面板，关到没标签了才收；这个布尔就是那个区别。
  std::function<void(bool last)> on_close_last_session{};
  std::function<void(bool)> on_busy_change{};
  std::function<void(const std::string&)> on_error{};
  /// 行模式的回车（宿主自己分发命令时接）。PTY 模式下**不装**——
  /// 回车就是 `\r` 字节，shell 自己处理。
  std::function<void(const std::string&)> on_submit{};
  /// PTY 模式的窗口标题变化（`OSC 0/2`；shell 会设成当前命令/目录）。
  std::function<void(std::size_t, const std::string&)> on_title_change{};

  // ────────────────────────────────────────────────────────────────────────
  // 每帧
  // ────────────────────────────────────────────────────────────────────────

  /// 把读线程收到的字节喂进屏幕模型、收尾已退出的会话。
  ///
  /// **必须每帧调**。不能放 `paint` 里：`paint` 可能被视口剔除跳过
  ///（元素滚出可视区就不画），那样"看不见时屏幕永远不刷新"。
  void pump();

  /// 关闭会话**已受理**但还没执行（下一帧 `pump` 做）——测试与宿主可据此判断。
  [[nodiscard]] auto close_pending() const noexcept -> bool {
    return pending_close_.has_value();
  }

  // ────────────────────────────────────────────────────────────────────────
  // 外观
  // ────────────────────────────────────────────────────────────────────────

  /// 输出区是否用等宽字体（默认 true）。等宽是终端的**可读性前提**——
  /// 比例字体下表格化输出（`ls -l` 的列对齐）会散架。
  void set_monospace(bool value);
  [[nodiscard]] auto monospace() const noexcept -> bool { return monospace_; }
  /// 标签栏可见（单会话场景可关；默认 true）。
  void set_tabs_visible(bool value) noexcept { tabs_visible_ = value; }
  [[nodiscard]] auto tabs_visible() const noexcept -> bool { return tabs_visible_; }
  /// 字号**档位倍数**（实际字号 = 主题基准 × 本值）。
  void set_font_scale(float scale);
  [[nodiscard]] auto font_scale() const noexcept -> float { return font_scale_; }

  /// **光标默认形状**（宿主偏好）。
  ///
  /// 只对**接下来新建**的屏幕模型生效，且会被程序的 `DECSCUSR` 覆盖：
  /// `vim`/`htop` 会明确要求竖线（插入模式），用户的偏好不该把程序的话撞掉。
  void set_cursor_shape(st::text::AnsiCursorShape shape) noexcept { cursor_shape_ = shape; }
  [[nodiscard]] auto cursor_shape() const noexcept -> st::text::AnsiCursorShape {
    return cursor_shape_;
  }

  // Element 接口
  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  /// **键盘/文本事件 → 写回 PTY 的字节**（PTY 模式的输入翻译契约）。
  ///
  /// 提出来单列的理由：这是终端对外的**输入合约**（哪些事件对应哪些字节序列），
  /// 而它此前埋在 `on_event` 里——只能靠“起真 shell 再看屏幕”间接验证，
  /// 映射表本身没有可断言的入口。纯函数（不碰 PTY/屏幕/成员状态）使这张表
  /// 可以被逐条钉死。
  ///
  /// 两条不信则：① **可打印字符只认 `TextInput`**（`KeyDown` 里的裸字符返回空串）——
  /// 与 `Input`/`CodeEditor`/`TextArea` 同一份契约；Win32 上一次按键同时产生
  /// `WM_KEYDOWN` 与 `WM_CHAR`，两边都送就会双回显。
  /// ② 修饰 + 方向/Home/End 必须先于裸键判断（裸键按字符串比较，会全抢走）。
  [[nodiscard]] static auto key_bytes(const Event& event) -> std::string;

  /// **滚轮增量 → 回看偏移的变化量**（行，向上为正、向下为负）。
  ///
  /// 提出来单列的理由与 `key_bytes` 同一回事：这是终端的**滚动方向合约**，
  /// 而它此前埋在 `on_event` 里——只能靠“滚一下看画面变没变”间接验证，
  /// 而**单向错**（只能往上或只能往下）恰好是那种很难从现象反推的缺陷。
  /// 纯函数使"方向 + 一格步长 + 半格不动"都能直接断言。
  ///
  /// 不信则：返回值与 `wheel_delta` **同号**（系统口径：向上为正）；
  /// `|delta| < 1/3` 时返回 0（不足一行就不动，不做四舍五入）。
  [[nodiscard]] static auto wheel_scroll_lines(float wheel_delta) -> int;

  /// 光标形状的**可读名**（属性面用；`block`/`underline`/`bar`）。
  [[nodiscard]] static auto cursor_shape_name(st::text::AnsiCursorShape shape) -> std::string_view;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto get_property(std::string_view name) const
      -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  auto invoke_action(std::string_view action, std::string_view argument) -> bool override;
  [[nodiscard]] auto semantics_value() const -> std::string override;

  // —— 几何（测试与命中共用；arrange 后有效）——
  [[nodiscard]] auto tabs_rect() const noexcept -> math::Rect { return tabs_rect_; }
  [[nodiscard]] auto output_rect() const noexcept -> math::Rect { return output_rect_; }

 private:
  /// 会话容器：**`unique_ptr`**——会话里含 `jthread`/`shared_ptr`（不可拷贝），
  /// 而容器会随增删重排。指针保地址稳定，也保住了"会话对象不搬家"这条不变式。
  std::vector<std::unique_ptr<TerminalSession>> sessions_{};
  std::size_t active_{0};
  std::size_t title_seq_{1};

  /// 光标默认形状（宿主偏好；程序可用 `DECSCUSR` 覆盖）。默认**竖线**：
  /// 终端里最常用的是插入点（提示符处、`vim` 插入模式），
  /// 竖线不遮挡字符，而且不会与“反显/背景色”混淆。
  st::text::AnsiCursorShape cursor_shape_{st::text::AnsiCursorShape::Bar};

  /// 当前会话的 PTY 读线程体：读字节 → 喂屏幕模型。
  void start_pty(const std::shared_ptr<st::process::PtySession>& pty,
                 const std::shared_ptr<st::text::AnsiScreen>& screen,
                 const std::string& display);
  /// 收尾某个会话（子进程退出 / 中止之后）。
  void finish_session(TerminalSession& session);
  /// 真正执行关闭（延迟到 `pump`；见 `close_session` 的说明）。
  void apply_pending_close();
  /// 待关闭的会话（只记一个——连点多个关闭按钮时前者先被处理）。
  std::optional<std::size_t> pending_close_{};
  /// 行模式：把滚回同步进 `Text` 子件（只在**值真变了**时写）。
  void sync_screen(TerminalSession& session);

  // —— 行模式的作业（同一时刻只有一个通道）——
  std::unique_ptr<st::exec::Channel> channel_{};
  st::exec::ChannelFactory factory_{};
  st::exec::ChannelSpec spec_{};
  std::jthread reader_{};
  std::vector<std::string> pending_{};
  /// （PTY 字节已改按会话存：`TerminalSession::pty_pending`——多会话不串台。）
  mutable std::mutex pending_mutex_{};
  std::atomic<bool> stop_requested_{false};
  bool job_finished_{false};
  int job_code_{0};
  std::string job_name_{};
  std::size_t job_session_{0};
  bool terminal_dirty_{false};
  /// 待上报的 PTY 尺寸（在 `arrange` 里算、在下一帧 `paint_content` 里报——
  /// 布局阶段不直接改 PTY，免得测量多次导致尺寸抖动）。
  st::process::PtySize pending_size_{80, 24};
  bool pending_size_set_{false};
  /// 生效的通道工厂（未注入时用缺省，惰性初始化）。
  [[nodiscard]] auto default_factory() const -> const st::exec::ChannelFactory&;

  // —— 外观 ——
  std::string shell_{};
  std::vector<std::string> shell_args_{};
  st::process::PtySize pty_size_{80, 24};
  std::string cwd_default_{};
  bool monospace_{true};
  bool tabs_visible_{true};
  float font_scale_{1.0f};
  /// 主题基准字号（`apply_theme` 存下，渲染时 × `font_scale_`）。
  float base_font_size_{13.0f};
  float line_height_{17.0f};
  float cell_width_{8.0f};
  /// **实测格宽缓存**（`measure_width("M")`，布局与渲染共用一个数）。
  ///
  /// 为什么必须缓存：旧实现 `arrange` 用「字号×系数」估列数上报 PTY、
  /// `paint_content` 却用实测格宽渲染——两个数不相等时，shell 认为的换行列与
  /// 实际渲染的换行列错开，光标逐行漂移（实测：输出几行后光标与字符错位）。
  /// 现在 `arrange` 也用实测值（经 `RenderContext::text` 拿端口），缓存到成员、
  /// 两处严格同源。无字体环境（端口为空）时退回系数估算。
  float measured_cell_width_{-1.0f};
  float measured_line_height_{-1.0f};

  // —— 子件（组合而非自绘：标签与滚动是既有组件）——
  Tabs* tabs_{nullptr};
  ScrollView* view_{nullptr};
  Text* text_out_{nullptr};   ///< 行模式的输出（PTY 模式不建）

  math::Rect tabs_rect_{};
  math::Rect output_rect_{};
  math::Rect input_rect_{};
  /// 上次写给 `Text` 的文本（避免每帧无条件 `set_content`）。
  mutable std::string last_output_{};
  mutable std::vector<std::string> last_labels_{};
  /// 屏幕文本的渲染缓存（逐行，避免每帧重建整屏字符串）。
  mutable std::vector<std::string> screen_lines_{};
  /// 刷新实测格宽缓存（字体端口/字号/缩放变化时调用；`arrange` 里先于列数计算）。
  void refresh_cell_metrics(const RenderContext& context);

  /// 取当前会话（容器为空时先补一个——不变式：**永远至少一个会话**）。
  auto current() -> TerminalSession&;
  [[nodiscard]] auto current() const -> const TerminalSession&;
  /// 生效工作目录。
  [[nodiscard]] auto effective_cwd(std::size_t index) const -> std::string;
  /// 懒建子件。
  void ensure_children();
};

}  // namespace st::ui
