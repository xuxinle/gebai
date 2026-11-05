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

  /// **整文件模式**：给两侧全文，视图按“整份文件”排（而不是只排差异块）。
  ///
  /// 为何需要它（2026-11-05，VSCode/IDEA 同款）：差异块模式只能看到 `@@` 附近
  /// 几行上下文，于是右栏根本不是“文件的当前内容”——它是一个片段。两件事因此做不成：
  /// - **就地编辑**（用户要求“编辑器与对比一体、改了自动对比”）：在片段里改，
  ///   改的是片段，回算差异必然把整篇算成改动；
  /// - **当文件阅读**：想看差异块之间的内容就得另行打开文件。
  ///
  /// 传空串 = 回到差异块模式（旧行为，保留：只想要变更区域时用）。
  ///
  /// `path` 只用于**头部摘要与可观测面**（`summary()` / `left_text` 的标识）——
  /// 不传的话摘要里没有文件名，而“现在是哪个文件的差异”是差异视图必须回答的事
  ///（跨文件导航就是按它看变化）。
  ///
  /// `keep_numbers`：两侧都显示**真实行号**（空白文本对比用）。默认 `false` ——
  /// 文件差异的两栏是“同一份文件的两次呈现”，行号要能单独归零（删行/填行不编号）；
  /// 而两段独立文本没有“文件行号”这回事，空缺的数字看着就像出错。
  void set_full_texts(std::string old_full, std::string new_full, std::string path = {},
                      bool keep_numbers = false);
  [[nodiscard]] auto full_texts_enabled() const noexcept -> bool { return full_file_; }

  /// 是否画 `@@ … @@` 分隔条（默认是）。
  ///
  /// 整文件模式下应当关掉：VSCode/IDEA 的差异编辑器**不画** `@@`——
  /// 整份文件都在眼前时，“这一块从第几行开始”是多余的噪声（而且每个块多条 22px
  /// 的带子会把文件拉得很长）。导航仍可用：`hunk_rows` 不依赖分隔条。
  void set_show_hunk_separators(bool value) { show_separators_ = value; }
  [[nodiscard]] auto show_hunk_separators() const noexcept -> bool { return show_separators_; }

  /// 是否画顶部摘要带（默认是）。
  ///
  /// 宿主自己已经在工具条里写了同样的摘要时（gbcode 的对比/差异工具条），
  /// 这里再画一遍就是**同一句话出现两次**（截图上很显眼）。是否该显示宿主最清楚，
  /// 所以开关交给它——而不是组件自作主张把摘要藏起来。
  void set_show_header(bool value) { show_header_ = value; mark_layout_dirty(); }
  [[nodiscard]] auto show_header() const noexcept -> bool { return show_header_; }

  /// 右缘**变更缩略条**（IDEA 同款）：整篇高度按比例压缩，改动处画彩色短横，
  /// 可见区是一个可拖的框——拖它 = 平移滚动。
  ///
  /// 为何需要它（不只是“好看”）：几千行的差异文件里，
  /// “改动集中在哪里、我现在看的是整体哪一段”在逐行阅读时完全看不出来。
  /// 缩略条把两个正交的量（**分布**与**位置**）压到一条竖条上。
  ///
  /// 默认开：它是差异视图的标配部件，不需要宿主每次记得打开。
  void set_show_overview(bool value);
  [[nodiscard]] auto show_overview() const noexcept -> bool { return show_overview_; }
  /// 缩略条宽度（逻辑像素）。宿主缩得很窄时可以关掉它（见上）。
  void set_overview_width(float value);
  [[nodiscard]] auto overview_width() const noexcept -> float { return overview_width_; }

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

  /// 右栏内容被编辑（传最新全文）——宿主据它重算差异（“改了就自动对比”）。
  ///
  /// 为何是回调而不是让 DiffView 自己重算：“与什么比”是宿主的语义
  ///（可能是暂存区、可能是 HEAD、可能是磁盘上的文件）——组件只知道“文本变了”。
  std::function<void(std::string_view)> on_text_edit{};

  /// **左栏**编辑（同上，传左栏的逻辑文本）。
  ///
  /// 为何要分左右：文件差异只放右栏可写（左栏是对照基线），而**空白文本对比**
  /// 两侧都是用户自己的内容——两边都得能改、且各自要知道是谁变了。
  std::function<void(std::string_view)> on_left_text_edit{};

  /// 让**两侧都可编辑**（空白对比用；文件差异不要用它——左栏是基线）。
  ///
  /// 与 `set_read_only(false)` 的区别：那个只开右栏（刻意），这个是“两份都是我的”。
  void set_both_sides_editable(bool value) { allow_left_edit_ = value; push_to_editors(); }

  /// **显式请求**“下一次排完就跳到第一个差异”（一次性）。
  ///
  /// 为何要做成“待办”而不是当场 `goto_hunk(0)`：`scroll_to_line` 要行高，
  /// 而行高在首次 `arrange` 之前还是 0——当场滚是**算术上必然失败**的
  ///（滚到 0×行高 = 原地），而且失败得静悄悄：画面停在第一行，看着只是“没跳”。
  ///
  /// 为何不让 `set_diff` 自己置位：它会被“右栏编辑后自动重算”调到，
  /// 那时把视口拽回第一处等于用户正在打第 200 行、画面每 180ms 跳回第 1 行。
  /// “打开就跳第一处”是宿主的意图，所以由宿主在**真的打开**时调本函数。
  void request_focus_first_hunk() { (void)goto_hunk(0); }
  /// 是否有待执行的“跳第一处”（自动化/测试可断言）。
  [[nodiscard]] auto focus_first_hunk_pending() const noexcept -> bool {
    return pending_scroll_hunk_ != kNoHunk;
  }

  // —— 导航 ——

  /// 字号档位/绝对字号透传到两侧编辑器（`0` = 跟随主题）。
  void set_font_size(float size);

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

  /// **逻辑文本**（去掉对齐填充行与 `@@` 分隔行后的真实内容）。
  ///
  /// 为何需要它：显示文本是“同一份内容的两次呈现”（见类头文件的行表），
  /// 里面混着为对齐而插的空行与分隔条。拿它去重新算差异，那些行会被当成
  /// “真实换行”——结果整篇都成了改动（而画面看着像“什么都没变”）。
  /// 自动对比（编辑后重算）必须以真实内容为输入。
  ///
  /// 新键入的行（超出上一轮重建范围）一律算真实内容。
  [[nodiscard]] auto logical_text(bool new_side) const -> std::string;

 private:
  /// 一侧的文本 + 行装饰（渲染前的中间产物）。
  struct Side {
    std::string text{};
    std::size_t lines{0};
    std::vector<CodeEditor::LineDecoration> marks{};
    /// 每个显示行 → 该侧的真实行号（占位/分隔行 = 0，即「不给号」）。
    std::vector<std::size_t> numbers{};
    /// 每个显示行是否承载**真实内容**（0 = 对齐填充行 / `@@` 分隔行）。
    ///
    /// 与 `numbers` **同址推入**（同一个 `begin_row`）。之所以不直接看 `numbers`：
    /// 语义上“有没有行号”与“是不是真内容”是两件事（将来允许“真的不显示行号”时会分家），
    /// 而 `logical_text()` 靠这一位筛掉造出来的行——拿显示文本重算差异时若带进
    /// 填充行，整篇会被比成“全改了”。
    std::vector<char> real{};
    /// 可滚动的差异块下标（显示行序）。
    std::vector<std::size_t> hunk_rows{};
  };

  /// 重建两侧文本（差异 → 显示行 + 装饰 + 行号）。
  void rebuild();
  void rebuild_side_by_side();
  void rebuild_unified();
  /// 整文件模式的并列重建（块间空隙按纯上下文逐行排）。
  void rebuild_side_by_side_full();
  /// 往一侧追加一个显示行（**唯一的写入口**：装饰行号与文本行必须同步递增）。
  /// `inline_ranges` 是行内改动区间（字节偏移；见 `DiffLine::inline_old`）。
  ///
  /// `real` = 这一行**是否承载真实内容**（`@@` 分隔行与对齐填充行都是 `false`）。
  ///
  /// 为何必须是显式参数而不是“行号非零”:行号与“是不是内容”是两件事——
  /// 空白文本对比时填充行也要有行号（否则右栏数字断断续续，看着像错），
  /// 而它**仍然不是内容**：`logical_text()` 靠这一位筛掉造出来的行，
  /// 拿显示文本重算差异时若带进填充行，整篇会被比成“全改了”。
  static void begin_row(Side& side, std::size_t number, CodeEditor::LineMark mark,
                        std::string_view text, bool gutter_bar = false,
                        std::vector<std::pair<std::size_t, std::size_t>> inline_ranges = {},
                        bool real = true);
  /// 把重建结果推给两侧编辑器（文本 + 装饰 + 只读位）。
  void push_to_editors();
  /// 行高是否真的量出来了（`scroll_to_line` 能不能算出非零偏移）。
  ///
  /// 为何不能只看 `bounds().height > 0`：那是**本组件**的矩形，它非零不代表内部
  /// 编辑器已经把行高算出来了（还没 measure/arrange 过时行高缓存是 0）。
  [[nodiscard]] auto line_height_ready() const -> bool;
  /// 缩略条拖拽 → 把滚动位置搬到指针所指的比例处。
  void scroll_from_overview(float pointer_y);
  [[nodiscard]] static auto is_disabled(const std::vector<std::size_t>& list,
                                       std::size_t number) -> bool;
  /// 切行（唯一口径：去掉行尾 `\r`；末尾无换行也算一行；空文本是一行空行）。
  static auto split_lines_of(std::string_view text) -> std::vector<std::string_view>;
  [[nodiscard]] auto step_hunk(int delta) -> std::size_t;
  /// 滚动同步（避免互相唤醒：用自己的挂起标记）。
  void sync_scroll_from(CodeEditor& source);
  [[nodiscard]] static auto join_numbers(const std::vector<std::size_t>& values) -> std::string;
  [[nodiscard]] static auto parse_numbers(std::string_view text)
      -> std::optional<std::vector<std::size_t>>;

  text::FileDiff diff_{};
  /// 整文件模式的两侧全文（空 = 差异块模式；见 `set_full_texts`）。
  std::string full_old_{};
  std::string full_new_{};
  /// 整文件模式的路径（只用于摘要展示；空 = 不显示文件名）。
  std::string full_path_{};
  bool full_file_{false};
  /// 整文件模式是否给对齐填充行编号（见 `set_full_texts` 的 `keep_numbers`）。
  bool keep_numbers_{false};
  /// 是否画 `@@` 分隔条（见 `set_show_hunk_separators`）。
  bool show_separators_{true};
  /// 是否画顶部摘要带（见 `set_show_header`）。
  bool show_header_{true};
  /// 是否画右缘变更缩略条（见 `set_show_overview`）。
  bool show_overview_{true};
  float overview_width_{12.0f};
  /// 缩略条当前按下的状态（拖动中）：按下位置与当时的滚动值。
  bool overview_dragging_{false};
  float overview_grab_dy_{0.0f};
  /// 缩略条的滚动框矩形（`arrange` 算，`paint` 画，`on_event` 命中）。
  bool overview_box_valid_{false};
  math::Rect overview_box_{};
  math::Rect overview_track_{};
  DiffLayout layout_{DiffLayout::SideBySide};
  bool show_line_numbers_{true};
  bool read_only_{true};
  /// “待执行的滚动到某块”（行高未就绪时置位，`arrange` 消费）。
  std::size_t pending_scroll_hunk_{static_cast<std::size_t>(-1)};
  /// 左栏是否允许编辑（默认否；见 `set_read_only`）。
  bool allow_left_edit_{false};
  float font_size_{0.0f};
  /// 去重的禁用行集合（两侧各自的 1 起行号；见 `set_disabled_lines`）。
  std::vector<std::size_t> disabled_old_{};
  std::vector<std::size_t> disabled_new_{};

  Side old_side_{};
  Side new_side_{};
  /// 上一次真的推给编辑器的两份 `real` 表（保留光标用的锚点）。
  ///
  /// 为何要单独存一份：`old_side_`/`new_side_` 在 `rebuild()` 里刚被重建，
  /// 而“编辑器里现在那份文本对应的 `real` 表”是**上一轮**的——两份不是同一个东西。
  std::vector<char> pushed_real_left_{};
  std::vector<char> pushed_real_right_{};  std::size_t current_hunk_{kNoHunk};

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
