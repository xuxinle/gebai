#pragma once

/// 组件库 · 差异视图（`DESIGN.md` §4.2.11）：`DiffView`（并列 / 内联 + hunk 导航）。
///
/// ## 它解决什么
///
/// 差异的**数据**（`st/text/diff`）与**单行的渲染量**（`CodeEditor` 行装饰）已经在框架里，
/// 但把「两栏拼起来 + 两侧行号对齐 + 滚到一起 + 能一个差异一个差异地跳」这件事
/// 在应用里重写一遍是必然走样的：两栏对齐要按 `DiffRow` 的交错**逐行预插/删**，
/// 行号要按两侧各自的号覆盖，滚动要同步且不能互相唤醒成死循环。
/// 这三件事都属于「差异视图」这个概念本身，因此收进框架。
///
/// ## 语义模型
///
/// 文本按**整行**表示差异（不折行——折行会让行号映射失效）：
/// 每一侧各有一个 `CodeEditor`，但它们的文本是**同一个序列的两种取舍**：
///
/// | 行 | 左侧文本 | 右侧文本 |
/// |---|---|---|
/// | 上下文 | 原行 | 原行 |
/// | 删除 | 原行 | **空行**（占位，让两侧行号对齐） |
/// | 新增 | **空行** | 原行 |
///
/// 于是「两栏的第 i 个显示行」天然一一对应，行号靠 `LineDecoration::number` 覆盖：
/// 左侧显示旧侧行号（占位行不给号），右侧显示新侧行号。**一栏可编辑、两栏同源**
/// 就是靠这套约定成立的。
///
/// 内联模式用**同一个左侧编辑器**：删除行照原样、新增行照原样，只是没有占位行。
/// 分隔条（hunk 之间的 `@@ … @@`）在两种模式下都画，并可点击跳转。
///
/// ## 不做的事（刻意）
///
/// - **不做折行**：折行需要"显示行 → 逻辑行"的映射，而差异的定位粒度是逻辑行；
///   混在一起会让"跳到第 7 个差异"变成一件要重算的事（主流工具也都默认不折行）。
/// - **不做行内字符级高亮**（同一行内的字符差异）：它是另一层（`text::diff` 能给出
///   行级块，字符级需要再加一层行内比较）。留作后续，接口不变。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/text/diff.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 差异视图的布局模式。
enum class DiffLayout : std::uint8_t {
  SideBySide,  ///< 并列两栏（默认）
  Unified,     ///< 内联单栏
};

/// 差异视图：把一份 `text::FileDiff` 渲染成可导航的两栏/内联视图。
class DiffView : public Element {
 public:
  DiffView();
  ~DiffView() override;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "DiffView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  /// 装载一份差异（**整体替换**）。空差异 = 清空（显示「无差异」占位）。
  ///
  /// 为什么收 `FileDiff` 而不是两段文本：文件级信息（新增/删除/重命名/二进制的文案、
  /// 路径）是差异视图头部要显示的东西，而它们在结构里已经解析好了——
  /// 让调用方再传一遍路径字符串等于把同一份事实存两处。
  void set_diff(const text::FileDiff& diff);

  /// 便捷：直接给两段文本（内部走 `text::make_file_diff`）。
  void set_texts(std::string_view old_text, std::string_view new_text);

  /// 清空（无差异）。
  void clear();

  // —— 布局与禁用（禁用的行不占位置、不出现在编辑器的文本里）——

  void set_layout(DiffLayout layout);
  [[nodiscard]] auto layout() const noexcept -> DiffLayout { return layout_; }
  void set_show_line_numbers(bool value);
  [[nodiscard]] auto show_line_numbers() const noexcept -> bool { return show_line_numbers_; }
  /// 去重的**禁用行集合**（两侧各自的 1 起行号）。对齐两栏共用。
  void set_disabled_lines(std::vector<std::size_t> old_lines, std::vector<std::size_t> new_lines);
  [[nodiscard]] auto disabled_old_lines() const noexcept -> const std::vector<std::size_t>& {
    return disabled_old_;
  }
  [[nodiscard]] auto disabled_new_lines() const noexcept -> const std::vector<std::size_t>& {
    return disabled_new_;
  }

  /// 只读（默认 true）。**左栏恒只读**（它是对照基线：HEAD / 另一个提交），
  /// 传 false 只放开**右栏**——右栏才是「当前内容」那一侧。
  /// 两侧都可写会让「同一份内容有两个真值源」（改哪边算改？）。
  void set_read_only(bool value);
  [[nodiscard]] auto read_only() const noexcept -> bool { return read_only_; }

  /// 字号档位/绝对字号透传到两侧编辑器（`0` = 跟随主题）。
  void set_font_size(float size);

  // —— 导航 ——

  /// 差异块（hunk）数量。
  [[nodiscard]] auto hunk_count() const noexcept -> std::size_t { return diff_.hunks.size(); }
  /// 当前所在差异块下标（无差异为 `kNoHunk`）。
  [[nodiscard]] auto current_hunk() const noexcept -> std::size_t { return current_hunk_; }
  /// 跳到第 `index` 个差异块（夹取；把它滚到视野顶部并高亮分隔条）。返回是否成功。
  auto goto_hunk(std::size_t index) -> bool;
  /// 下一个 / 上一个差异块（环绕）。返回落点下标，无差异返回 `kNoHunk`。
  auto next_hunk() -> std::size_t { return step_hunk(1); }
  auto previous_hunk() -> std::size_t { return step_hunk(-1); }
  /// 该差异块在**本侧文本**里的起始显示行（1 起）。
  [[nodiscard]] auto hunk_first_line(std::size_t index, bool new_side) const -> std::size_t;

  static constexpr std::size_t kNoHunk = static_cast<std::size_t>(-1);

  /// 当前差异块变化（导航或点击分隔条）时回调。
  std::function<void(std::size_t)> on_hunk_change{};
  /// 滚动位置变化（两栏共用一条；`y` = 像素偏移）。
  std::function<void(float)> on_scroll{};

  [[nodiscard]] auto diff() const noexcept -> const text::FileDiff& { return diff_; }
  /// 头部单行摘要（`路径 · +N −M`，重命名/二进制会如实写出来）。
  [[nodiscard]] auto summary() const -> std::string;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  auto invoke_action(std::string_view action, std::string_view argument) -> bool override;

  // —— 给自动化/测试的几何面（与绘制同源）——

  /// 分隔条带（自上而下第 `index` 条）的矩形；越界返回空矩形。
  [[nodiscard]] auto separator_rect(std::size_t index) const -> math::Rect;
  /// 左侧编辑器（内联模式就是唯一的那个）。可能为空（未排布）。
  [[nodiscard]] auto left_editor() noexcept -> CodeEditor* { return left_; }
  [[nodiscard]] auto right_editor() noexcept -> CodeEditor* { return right_; }
  /// 一侧文本里的显示行数（含占位行）。
  [[nodiscard]] auto line_count(bool new_side) const -> std::size_t;

 private:
  /// 一侧的文本 + 行装饰（渲染前的中间产物）。
  struct Side {
    std::string text{};
    std::size_t lines{0};
    std::vector<CodeEditor::LineDecoration> marks{};
    /// 每个显示行 → 该侧的真实行号（占位/分隔行 = 0，即「不给号」）。
    std::vector<std::size_t> numbers{};
    /// 可滚动的差异块下标（显示行序）。
    std::vector<std::size_t> hunk_rows{};
  };

  /// 重建两侧文本（差异 → 显示行 + 装饰 + 行号）。
  void rebuild();
  void rebuild_side_by_side();
  void rebuild_unified();
  /// 往一侧追加一个显示行（**唯一的写入口**：装饰行号与文本行必须同步递增）。
  /// `inline_ranges` 是行内改动区间（字节偏移；见 `DiffLine::inline_old`）。
  static void begin_row(Side& side, std::size_t number, CodeEditor::LineMark mark,
                        std::string_view text, bool gutter_bar = false,
                        std::vector<std::pair<std::size_t, std::size_t>> inline_ranges = {});
  /// 把重建结果推给两侧编辑器（文本 + 装饰 + 只读位）。
  void push_to_editors();
  /// 该侧该行是否被「禁用」（不出现）。
  [[nodiscard]] static auto is_disabled(const std::vector<std::size_t>& list, std::size_t number)
      -> bool;
  [[nodiscard]] auto step_hunk(int delta) -> std::size_t;
  /// 滚动同步（避免互相唤醒：用自己的挂起标记）。
  void sync_scroll_from(CodeEditor& source);
  [[nodiscard]] static auto join_numbers(const std::vector<std::size_t>& values) -> std::string;
  [[nodiscard]] static auto parse_numbers(std::string_view text)
      -> std::optional<std::vector<std::size_t>>;

  text::FileDiff diff_{};
  DiffLayout layout_{DiffLayout::SideBySide};
  bool show_line_numbers_{true};
  bool read_only_{true};
  /// 左栏是否允许编辑（默认否；见 `set_read_only`）。
  bool allow_left_edit_{false};
  float font_size_{0.0f};
  std::vector<std::size_t> disabled_old_{};
  std::vector<std::size_t> disabled_new_{};

  Side old_side_{};
  Side new_side_{};
  std::size_t current_hunk_{kNoHunk};

  CodeEditor* left_{nullptr};
  CodeEditor* right_{nullptr};
  /// 分隔条的显示行号（旧侧行序）。
  std::vector<std::size_t> separator_rows_{};
  /// 分隔条在**当前布局**下的矩形（`arrange` 填；绘制与命中共用）。
  mutable std::vector<math::Rect> separator_rects_{};
  /// 同步滚动用：一次同步里只应由一方驱动（否则两栏互相唤醒）。
  bool syncing_{false};
};

}  // namespace st::ui
