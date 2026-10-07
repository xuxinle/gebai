/// ANSI 屏幕模型：把终端字节流解释成**一格一格的屏幕**。
///
/// ## 为什么需要它（而不是把输出拼成字符串）
///
/// 终端输出**不是**按行追加的文本，而是一串"往屏幕某处写什么"的指令：
/// 光标跳转（`\e[10;20H`）、覆盖当前行（`\r`）、清屏、滚动区域、颜色切换……
/// 拼成字符串会丢掉全部定位信息——`top`/`vim`/进度条这类**重画屏幕**的程序
/// 拼出来就是一堆乱码叠在一起。
///
/// 所以真终端的渲染链是：
///
/// ```
/// 原始字节 → AnsiScreen（解释成网格 + 样式）→ 渲染器逐格画出来
///                    ↑ 光标位置/可见性也是状态的一部分
/// ```
///
/// ## 覆盖范围（完整档，含全屏程序）
///
/// * **C0 控制字符**：`\n` `\r` `\b` `\t` `\a`（响铃忽略）
/// * **SGR 样式**：16 色 / 256 色 / 真彩色前景背景、粗体、暗淡、斜体、下划线、
///   闪烁、反显、删除线
/// * **光标控制**：相对/绝对移动、行列定位、保存/恢复、显隐、形状
/// * **编辑**：清行/清屏/清字符、插删字符、插删行、滚动区域（`DECSTBM`）
/// * **滚动**：底部换行滚屏、`SU`/`SD`；主屏滚出的行进**回看缓冲**
/// * **备用屏幕缓冲**（`\e[?1049h/l`）：**全屏程序的前提**——`vim`/`htop` 切到
///   备用屏绘制、退出时切回来，主屏内容原样保留
/// * **自动换行**（`DECAWM`）、`OSC 0/2` 窗口标题
///
/// 刻意**不做**的：鼠标上报（`\e[?1000h` 等，需要宿主回传鼠标事件）、
/// 括号粘贴（`\e[?2004h`，需要宿主回传粘贴包装）、六图/Kitty 图像协议。
/// 这些是"宿主与终端模拟器之间的协商"，本层只做**屏幕**；遇到时**安静忽略**
///（不回显、不报错），因为未知序列把它当文本画出来会污染屏幕。
///
/// ## 与渲染/输入的分工
///
/// 本层**不碰**渲染与输入：它只回答"现在屏幕上是什么"（`cell`/`row_text`/
/// `cursor_*`）与"哪些行变了"（`take_dirty_rows`，给增量重绘用）。
/// 输入方向是反的：宿主把按键字节**原样**写回 PTY，本层不参与
///（行内编辑由内核行规程与 shell 自己做——那才叫终端）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace st::text {

/// 终端颜色：默认色 / 256 索引色 / 真彩色。
///
/// 为什么不解析成 RGB：主题是宿主的（用户可能改），把索引色提前落到 RGB 会
/// **丢掉语义**——渲染时才知道该用哪个调色板。真彩色（`38;2;r;g;b`）才带 RGB。
struct AnsiColor {
  enum class Kind : std::uint8_t { Default, Indexed, Rgb };
  Kind kind{Kind::Default};
  std::uint8_t index{0};
  std::uint8_t r{0};
  std::uint8_t g{0};
  std::uint8_t b{0};

  [[nodiscard]] static auto indexed(std::uint8_t value) -> AnsiColor {
    return AnsiColor{.kind = Kind::Indexed, .index = value};
  }
  [[nodiscard]] static auto rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
      -> AnsiColor {
    return AnsiColor{.kind = Kind::Rgb, .r = red, .g = green, .b = blue};
  }
  friend auto operator==(const AnsiColor&, const AnsiColor&) -> bool = default;
};

/// 一个格子的样式。
struct AnsiStyle {
  AnsiColor fg{};
  AnsiColor bg{};
  bool bold{false};
  bool dim{false};
  bool italic{false};
  bool underline{false};
  bool blink{false};
  bool reverse{false};
  bool strike{false};
  friend auto operator==(const AnsiStyle&, const AnsiStyle&) -> bool = default;
};

/// 一个格子。
struct AnsiCell {
  char32_t ch{U' '};
  AnsiStyle style{};
  /// 宽字符（CJK 等占两列）的**右半格**：它不持有字符，渲染时跳过。
  bool continuation{false};
  friend auto operator==(const AnsiCell&, const AnsiCell&) -> bool = default;
};

/// 光标形状（`DECSCUSR`）。
enum class AnsiCursorShape : std::uint8_t { Block, Underline, Bar };

class AnsiScreen {
 public:
  /// 回看缓冲上限（**行**）：终端是"最近发生了什么"的窗口，不是日志归档。
  static constexpr std::size_t kMaxScrollback{4000};

  explicit AnsiScreen(int cols = 80, int rows = 24);

  // —— 喂数据 ——

  /// 喂**原始字节**（含 ANSI 序列与 UTF-8 文本）。
  ///
  /// 字节可以**任意切分**：跨调用的半个转义序列、半个 UTF-8 码点都会被正确接续
  ///（这是必须的——PTY 读到的分片边界与序列边界无关，实测一次读 4096 字节
  /// 经常把一个序列从中间切开）。
  void feed(std::string_view bytes);

  /// 改尺寸（重排：光标夹取、内容按需截断/补空）。
  void resize(int cols, int rows);

  // —— 屏幕内容 ——

  [[nodiscard]] auto cols() const noexcept -> int { return cols_; }
  [[nodiscard]] auto rows() const noexcept -> int { return rows_; }
  [[nodiscard]] auto cell(int row, int col) const -> const AnsiCell&;
  /// 一行的可读文本（去掉行尾空白；宽字符的右半格跳过）。
  [[nodiscard]] auto row_text(int row) const -> std::string;
  /// 整屏纯文本（行尾去空白、按行拼接）。测试与"复制全部"用。
  [[nodiscard]] auto plain_text() const -> std::string;

  // —— 光标 ——

  [[nodiscard]] auto cursor_row() const noexcept -> int { return cursor_row_; }
  [[nodiscard]] auto cursor_col() const noexcept -> int { return cursor_col_; }
  [[nodiscard]] auto cursor_visible() const noexcept -> bool { return cursor_visible_; }
  [[nodiscard]] auto cursor_shape() const noexcept -> AnsiCursorShape { return cursor_shape_; }
  /// 光标是否停在"待换行"状态（写满一行后未换行的挂起态）。
  ///
  /// 渲染时要知道它：挂起状态下光标**停在最后一个字符上**而不是下一行首，
  /// 否则看起来会多出一个空行。
  [[nodiscard]] auto pending_wrap() const noexcept -> bool { return wrap_pending_; }

  // —— 备用屏幕 ——

  /// 是否在备用屏幕缓冲里（`vim`/`htop` 之类全屏程序运行期间为真）。
  [[nodiscard]] auto in_alt_screen() const noexcept -> bool { return alt_active_; }

  // —— 回看 ——

  [[nodiscard]] auto scrollback_count() const noexcept -> std::size_t {
    return scrollback_.size();
  }
  [[nodiscard]] auto scrollback_line(std::size_t index) const -> std::string;

  // —— 增量重绘 ——

  /// 取走"自上次调用以来变过的行号"（升序去重），并清空标记。
  ///
  /// 为什么按行而不是按格：终端一帧往往只改几行（状态栏、进度条），
  /// 逐格比较整屏是浪费；按行做增量足够，且与渲染器的"行内重排"天然对齐。
  [[nodiscard]] auto take_dirty_rows() -> std::vector<int>;
  /// 强制标记整屏脏（尺寸变化、切屏之后调用）。
  void mark_all_dirty();

  // —— 窗口标题（`OSC 0`/`OSC 2`）——

  [[nodiscard]] auto title() const -> const std::string& { return title_; }

  /// 响铃计数（`\a`）。宿主想给个视觉/声音反馈时读它——本层只计数。
  [[nodiscard]] auto bell_count() const noexcept -> std::size_t { return bell_count_; }

 private:
  // —— 屏幕 ——
  [[nodiscard]] auto line(int row) -> std::vector<AnsiCell>&;
  [[nodiscard]] auto line(int row) const -> const std::vector<AnsiCell>&;
  void clear_row(int row, int from_col);
  void scroll_up(int top, int bottom, int count);
  void scroll_down(int top, int bottom, int count);
  /// 在主屏滚屏时把滚出的行推进回看缓冲（备用屏不进——全屏程序的重绘不是历史）。
  void push_scrollback(const std::vector<AnsiCell>& row);

  // —— 写字符 ——
  void put(char32_t ch);
  void newline();
  void carriage_return();
  void tab();
  void backspace();
  void set_dirty(int row);

  // —— 解析 ——
  enum class State : std::uint8_t { Ground, Escape, Csi, Osc, OscEscape, Charset };
  void handle_csi(char final_byte);
  void handle_escape(char byte);
  void handle_osc(std::string_view payload);
  void reset_style();
  [[nodiscard]] auto param(std::size_t index, int fallback) const -> int;
  [[nodiscard]] auto param_count() const -> std::size_t { return params_.size(); }
  void apply_sgr();

  int cols_{80};
  int rows_{24};
  std::vector<std::vector<AnsiCell>> screen_{};
  /// 备用屏幕缓冲（切进来时主屏整份留着，切回去原样恢复）。
  std::vector<std::vector<AnsiCell>> alt_screen_{};
  bool alt_active_{false};
  /// 切备用屏前的光标位置（`\e[?1049` 的语义里要恢复它）。
  int saved_cursor_row_{0};
  int saved_cursor_col_{0};

  int cursor_row_{0};
  int cursor_col_{0};
  bool cursor_visible_{true};
  AnsiCursorShape cursor_shape_{AnsiCursorShape::Block};
  /// 写满一行后的挂起态（等下一个字符才真换行——`DECAWM` 的语义）。
  bool wrap_pending_{false};
  bool auto_wrap_{true};

  /// 保存的光标与样式（`ESC 7`/`ESC 8`、`CSI s`/`CSI u` 共用一份）。
  int store_row_{0};
  int store_col_{0};
  AnsiStyle store_style_{};

  /// 滚动区域（`DECSTBM`），1 基、含端点；默认整屏。
  int scroll_top_{0};
  int scroll_bottom_{0};

  AnsiStyle style_{};
  std::string title_;
  std::size_t bell_count_{0};

  std::deque<std::vector<AnsiCell>> scrollback_{};
  std::vector<bool> dirty_;

  // —— 解析状态 ——
  State state_{State::Ground};
  std::vector<int> params_{};
  bool param_has_value_{false};
  bool private_marker_{false};   ///< CSI 里的 `?` `>` `<` `=` 前缀
  char private_char_{0};
  std::string intermediate_{};   ///< CSI 的参数与终止符之间的中间字节（`"` `$` ` ` 等）
  std::string osc_buffer_{};
  /// UTF-8 解码的残片（跨 `feed` 调用接续）。
  std::string utf8_pending_{};
};

}  // namespace st::text
