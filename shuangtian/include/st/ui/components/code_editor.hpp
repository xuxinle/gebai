#pragma once

/// 代码编辑器组件：**可编辑 + 语法高亮 + 行号 + 选择/撤销**，面向"文件编辑器"场景。
///
/// 能力
/// - **语法高亮**：内置 30 种主流语言（C/C++/Rust/Go/Python/JS/TS/Java/C#/Kotlin/Swift/Ruby/
///   PHP/Lua/SQL/HTML/XML/CSS/SCSS/JSON/YAML/TOML/INI/Shell/Dockerfile/Makefile/CMake/
///   Markdown/Diff/Protobuf…，按语言名/别名/扩展名识别）；
/// - **自定义语言**：`set_language_spec()` 直接绑定一份自定义规则，或向进程注册表
///   （`text::global_languages().register_language()`）注册后按名字使用——两条路径走同一台扫描器；
/// - **编辑**：插入/删除、UTF-8 光标移动（含 Ctrl+词移动）、Home/End、跨行选择（鼠标拖选 /
///   双击选词 / 三击选行 / Shift+方向键）、全选、复制剪切粘贴、撤销重做、Tab 缩进/
///   Shift+Tab 反缩进、Ctrl+/ 注释切换、回车自动缩进、括号配对高亮；
/// - **查找替换**：`set_find` 全部命中高亮、`find_next` 环绕跳转（选中+滚入视野）、
///   `replace_current`/`replace_all`（走撤销栈，Ctrl+Z 可回滚）；
/// - **只读模式**：`set_read_only(true)` 即"带高亮的代码查看器"（选择与复制仍可用）；
/// - **滚动**：垂直 + 水平（代码不折行），行号槽固定；
/// - **控制通道**：属性面（text/language/cursor/selection/read_only/…）与动作
///   （focus/select_all/undo/redo/goto_line/insert/copy/cut/paste/comment/
///   find/find_next/replace/replace_all）齐全，
///   智能体可据此读写编辑器内容。
///
/// 坐标系：全部为**逻辑像素**（与协议一致）；文本索引为 **UTF-8 字节偏移**且落在码点边界。

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/text/highlight.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class CodeEditor : public Element {
 public:
  CodeEditor();
  ~CodeEditor() override;
  CodeEditor(const CodeEditor&) = delete;
  auto operator=(const CodeEditor&) -> CodeEditor& = delete;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CodeEditor"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Code; }

  // —— 内容与外观 ——

  /// 设定全文；光标与选择会夹取到合法位置，撤销栈清空。
  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  /// 按语言名（规范名/别名/扩展名）设定高亮语言；空串 = 关闭高亮。
  void set_language(std::string language);
  [[nodiscard]] auto language() const noexcept -> const std::string& { return language_; }
  /// 直接绑定一份自定义语言规则（优先于 `language_`；传空规则 = 回到按名字查表）。
  void set_language_spec(text::LanguageSpec spec);
  /// 从文件路径推断语言（等价于 `set_language(*language_from_path(path))`）。
  void set_language_from_path(std::string_view path);
  /// 只读：仍可选中/复制，但拒绝一切修改（含粘贴与动作）。
  void set_read_only(bool value) {
    read_only_ = value;
    mark_dirty();
  }
  [[nodiscard]] auto read_only() const noexcept -> bool { return read_only_; }
  void set_show_line_numbers(bool value) {
    show_line_numbers_ = value;
    mark_layout_dirty();
  }
  [[nodiscard]] auto show_line_numbers() const noexcept -> bool { return show_line_numbers_; }
  void set_highlight_enabled(bool value) {
    highlight_enabled_ = value;
    mark_highlight_dirty();
  }
  [[nodiscard]] auto highlight_enabled() const noexcept -> bool { return highlight_enabled_; }
  void set_tab_width(int width);
  [[nodiscard]] auto tab_width() const noexcept -> int { return tab_width_; }
  /// 插入空格而非制表符（默认 true）。
  void set_insert_spaces(bool value) noexcept { insert_spaces_ = value; }

  /// 缩进参考线（每级缩进一条竖线；默认 true）——
  /// 小字号下的层次感主要靠它，缩进宽度折叠成**视觉列**（Tab 按 `tab_width` 展开）后对齐。
  void set_indent_guides(bool value);
  [[nodiscard]] auto indent_guides() const noexcept -> bool { return indent_guides_; }
  /// 自动配对（输入 `(`/`[`/`{`/引号自动补右半、选中文本被包裹、
  /// 光标已在闭符前再输入同符则**跳过**、`(` 后回车展开三行；默认 true）。
  /// 关闭后输入完全照原样落盘（面向「不要替我改」的场景）。
  void set_auto_pairs(bool value) noexcept { auto_pairs_ = value; }
  [[nodiscard]] auto auto_pairs() const noexcept -> bool { return auto_pairs_; }
  [[nodiscard]] auto insert_spaces() const noexcept -> bool { return insert_spaces_; }
  /// 字号**档位倍数**：实际字号 = `theme.metrics().font_size`（基准） × 本值。
  ///
  /// 为什么不是绝对像素：绝对字号（曾经硬写 13.0）会**与主题脱钩**——
  /// 用户把 `--ui-font-scale` 调大后，普通文字跟着缩放、编辑器纹丝不动
  /// （实测：`--ui-font-scale 1.5` 时 UI 文字 15→22.5，而编辑器恒为 13.5）。
  /// 档位表达还能让「编辑器字体大小」设置面板直接列出 0.85/1.0/1.15…。
  ///
  /// **行距倍数**（相对字体度量给出的自然行高）。默认 `1.15`——代码行比正文更需要
  /// 透气：注释、字符串、嵌套结构在密排下容易糊成一片，适度加宽的行距对**扫读**帮助很大
  /// （很多编辑器默认行距都在 1.1~1.5 之间）。`1.0` = 字体自然行高（最紧）。
  ///
  /// 传 <=0 或非有限值会被忽略（行距必须为正；`set_font_scale` 同姿态）。
  void set_line_spacing(float spacing);
  [[nodiscard]] auto line_spacing() const noexcept -> float { return line_spacing_; }

  /// 默认 `1.0` = **与正文同级**（`theme.metrics().font_base`）。
  /// 「比正文小一号」曾作为默认试过（0.85），实际观感偏小——代码区字号与界面文字
  /// 不一致时，眼睛要在两种字号间来回适应。要更小/更大就显式 `set_font_scale`。
  void set_font_scale(float scale);
  [[nodiscard]] auto font_scale() const noexcept -> float { return font_scale_; }

  /// 显式指定**绝对**字号（逻辑像素）；负值恢复“跟随主题”。
  ///
  /// 两个入口都有存在的理由：控件协议/宿主想要“就这个像素值”时用 `set_font_size`，
  /// 而想要“跟着主题走”时用 `set_font_scale`（或负值复位）。
  void set_font_size(float size);
  /// **实际字号**（逻辑像素）——= 显式值（若设过）否则 `主题 base × 档位`。
  /// 这是“字号”一词的自然含义（“这行字多大”），绘制/度量/测试都读它。
  [[nodiscard]] auto font_size() const noexcept -> float {
    return font_size_px_ > 0.0f ? font_size_px_ : theme_base_font_ * font_scale_;
  }
  /// 显式绝对字号（<=0 = 未设，跟主题）。
  ///
  /// 与 `font_size()` 分开是因为“输入”与“结果”是两件事：`font_size()` 跟着主题变，
  /// 而本值只在调用方真的写过 `set_font_size` 时才非负。
  [[nodiscard]] auto font_size_override() const noexcept -> float { return font_size_px_; }

  // —— 光标与选择 ——

  [[nodiscard]] auto cursor_index() const noexcept -> std::size_t { return cursor_; }
  void set_cursor_index(std::size_t index);
  /// 选择区间 `[begin, end)`（无选择时 `begin == end == cursor`）。
  [[nodiscard]] auto selection() const -> std::pair<std::size_t, std::size_t>;
  [[nodiscard]] auto has_selection() const noexcept -> bool { return anchor_ != cursor_; }
  [[nodiscard]] auto selected_text() const -> std::string;
  void select_all();
  void clear_selection() { anchor_ = cursor_; }
  /// 选中给定区间（自动按码点边界夹取）。
  void set_selection(std::size_t begin, std::size_t end);
  [[nodiscard]] auto cursor_line() const -> std::size_t;
  [[nodiscard]] auto cursor_column() const -> std::size_t;
  [[nodiscard]] auto line_count() const -> std::size_t;
  /// 跳到第 `line` 行（从 1 起）并把该行滚入视野。
  void goto_line(std::size_t line);

  // —— 编辑 ——

  /// 在光标处插入文本（有选择时先替换）。
  void insert_text(std::string_view inserted);
  /// 用给定文本替换当前选择（无选择时等价于插入）。
  void replace_selection(std::string_view replacement);
  auto undo() -> bool;
  auto redo() -> bool;
  [[nodiscard]] auto can_undo() const noexcept -> bool { return !undo_stack_.empty(); }
  [[nodiscard]] auto can_redo() const noexcept -> bool { return !redo_stack_.empty(); }
  /// 选中行的缩进（Tab）、反缩进（Shift+Tab）。
  void indent_selection(bool reverse);
  /// 注释/取消注释选中行（用语言的第一个行注释标记；无行注释的语言返回 false）。
  auto toggle_comment() -> bool;

  // —— 查找与替换（VSCode 同族语义；`case_sensitive=false` 大小写不敏感）——

  /// 设置查找词：重建命中表并绘制高亮（不移动光标）。返回命中数。
  auto set_find(std::string needle, bool case_sensitive = false) -> std::size_t;
  /// 清除查找态（高亮/命中表/当前命中）。
  void clear_find();
  [[nodiscard]] auto find_needle() const -> const std::string& { return find_needle_; }
  [[nodiscard]] auto find_match_count() const noexcept -> std::size_t { return find_matches_.size(); }
  /// 当前命中序号（无命中为 `kNoFindMatch`）。
  [[nodiscard]] auto find_active_index() const noexcept -> std::size_t { return find_active_; }
  /// 跳到下一/上一命中（环绕；选中该命中并滚入视野）。返回命中序号，无命中返回 npos。
  auto find_next(bool backward = false) -> std::size_t;
  /// 替换**当前命中**（无当前命中时先 `find_next`）；替换后跳下一命中。返回是否发生替换。
  auto replace_current(std::string_view replacement) -> bool;
  /// 替换全部命中。返回替换处数。
  auto replace_all(std::string_view replacement) -> std::size_t;

  static constexpr std::size_t kNoFindMatch = static_cast<std::size_t>(-1);

  /// 默认行距倍数（见 `set_line_spacing`）。**必须声明在 `line_spacing_` 之前**：
  /// 成员初始化器要用它，而成员默认值是按声明顺序求值的。
  static constexpr float kDefaultLineSpacing = 1.15f;

  // —— 滚动 ——

  void set_scroll_offset(float x, float y);
  [[nodiscard]] auto scroll_offset() const noexcept -> math::Point { return {scroll_x_, scroll_y_}; }
  /// 滚动增量（越界夹取到内容边界；正 dy 向下）。
  void scroll_by(float dx, float dy);
  /// 把第 `line` 行（从 1 起）滚到视野**顶部**（越界夹取，不会滚过内容末尾）。
  void scroll_to_line(std::size_t line);
  /// 首行可见行号（**从 1 起**；空文档为 1）——控制通道据此断言「视口停在哪一行」。
  [[nodiscard]] auto first_visible_line(const RenderContext& context) const -> std::size_t;
  /// 当前视口能完整展示的行数（至少 1）——断言视口容量用。
  [[nodiscard]] auto visible_line_count(const RenderContext& context) const -> std::size_t;
  /// 末行可见行号（**从 1 起**）——与 `first_visible_line` 配对断言「内容末尾在视口内」。
  [[nodiscard]] auto last_visible_line(const RenderContext& context) const -> std::size_t;
  /// 光标 x（相对控件左缘的逻辑像素；即“屏幕上那根竖线在哪”）。
  ///
  /// 与绘制同源（同一条量宽路径）；存在的理由是可断言：
  /// 「光标画在 x」与「按 x 反查落点」必须回环到同一个字符索引。
  [[nodiscard]] auto caret_offset_x(const RenderContext& context) const -> float;

  // —— 滚动条几何（与绘制/命中同一份；暴露出来是为了可断言）——

  /// 垂直滚动条轨道矩形（内容未溢出时为空）。
  [[nodiscard]] auto v_scroll_bar_rect(const RenderContext& context) const -> math::Rect;
  /// 垂直滚动条滑块当前矩形（空 = 无滚动条）。
  [[nodiscard]] auto v_scroll_thumb_rect(const RenderContext& context) const -> math::Rect;
  /// 水平滚动条轨道矩形（内容未溢出时为空）。
  [[nodiscard]] auto h_scroll_bar_rect() const -> math::Rect;

  // —— 回调 ——

  std::function<void(std::string_view)> on_change{};       ///< 内容变化（传最新全文）
  std::function<void()> on_cursor_change{};                ///< 光标/选择变化
  std::function<void(std::string_view)> on_submit{};       ///< Ctrl+Enter 提交
  /// 右键按下（参数为事件坐标，逻辑像素）。
  ///
  /// 为何必须有（2026-10-06，来自 codeeditor 右键菜单实战）：`on_event` 对
  /// **任何按钮**的 `MouseDown` 都走同一个“把光标搬到点的位置”的分支并返回 `true`，
  /// 于是宿主既拿不到右键（被组件吃掉）、也无法阻止“右键改了光标位置”。
  /// 结果是一个**能做到却不能做对**的局面：宿主只有两条路——接管整个命中路径，
  /// 或者放弃组件自带的鼠标行为。这个回调把“右键点了这里”作为一件事告诉宿主，
  /// 同时**不动光标**（右键不改选择，与主流编辑器一致）。
  std::function<void(math::Point)> on_context_menu{};

  // —— Element 覆写 ——

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  auto invoke_action(std::string_view action, std::string_view argument) -> bool override;

  // —— 静态信息（UI 语言选择器/控制通道枚举用）——

  /// 当前已注册的语言名（含内置与运行时注册的自定义语言，升序）。
  [[nodiscard]] static auto available_languages() -> std::vector<std::string>;

 private:
  /// 一行的 token 切片（偏移相对行起点）。
  using LineTokens = std::vector<text::Token>;

  /// 编辑快照（撤销/重做）。
  struct Snapshot {
    std::string text{};
    std::size_t cursor{0};
    std::size_t anchor{0};
  };

  [[nodiscard]] auto current_spec() const -> std::shared_ptr<const text::LanguageSpec>;
  /// 上下标量滚动夹取（垂直/水平共用一处；`scroll_to_line`/滚轮/动作面全部经此）。
  void clamp_scroll(const RenderContext& context);
  /// 智能 Home：光标不在行首非空列时先跳该列，已在则跳列 0（VSCode 同款两段式）。
  void smart_home(bool extend);
  /// 按词删除：`backward` = Ctrl+Backspace（删到上一词首），否则 Ctrl+Delete（删到下一词尾）。
  void erase_word(bool backward);
  /// 自动配对落笔：返回 true 表示本次输入已被配对逻辑接管（调用方不再原样插入）。
  auto insert_with_pairs(std::string_view inserted) -> bool;
  void mark_highlight_dirty();
  /// 重建**纯文本行索引**（一趟扫描，便宜）。
  void rebuild_spans() const;
  /// 重建**高亮 token 缓存**（依赖文本与语言规则，较贵；只在绘制/需要时做）。
  void rebuild_tokens() const;
  /// 逐行变换 `[first_line, last_line]` 行区间并写回文本（缩进/注释等批量操作共用）。
  void transform_lines(std::size_t first_line, std::size_t last_line,
                       const std::function<std::string(std::string_view)>& transform);
  auto rebuild_line_geometry(const RenderContext& context) const -> void;
  [[nodiscard]] auto gutter_width(const RenderContext& context) const -> float;
  [[nodiscard]] auto line_height(const RenderContext& context) const -> float;
  [[nodiscard]] auto text_origin(const RenderContext& context) const -> math::Point;
  [[nodiscard]] auto content_height() const noexcept -> float;
  [[nodiscard]] auto content_width(const RenderContext& context) const -> float;
  [[nodiscard]] auto visible_line_range(const RenderContext& context) const
      -> std::pair<std::size_t, std::size_t>;
  [[nodiscard]] auto index_at_point(const RenderContext& context, math::Point point) const
      -> std::size_t;
  [[nodiscard]] auto x_for_index(const RenderContext& context, std::size_t index) const -> float;
  /// 制表符展开结果：`text` 是 `\t` 按制表位补空格的形态，`map[i]` 是
  /// 「原始行内偏移 i → 展开后偏移」的映射（长度 = 原始长度 + 1）。
  struct TabExpansion {
    std::string text{};
    std::vector<std::size_t> map{};
  };
  /// 把一行按 `tab_width_` 展开（逻辑列以**码点**计，`\t` 补到下一个制表位）。
  ///
  /// 为什么必须展开而不是把 `\t` 当普通字形去量/去画：量宽与绘制是两条独立路径，
  /// 字体对 `\t` 的处理（宽度 0 / 小方块 / 按制表位）与「一个字符宽」都不一致，
  /// 于是**光标、选择、缩进参考线、鼠标命中与真实字位全部错位**，而且随列号累积。
  /// 展开成空格后两条路径天然同源（都是普通字形度量）。
  [[nodiscard]] auto expand_tabs(std::string_view row) const -> TabExpansion;
  /// 垂直滚动条拖拽：`pointer_y` → 新滚动偏移。
  void apply_v_scroll_drag(const RenderContext& context, float pointer_y);
  [[nodiscard]] auto line_of_index(std::size_t index) const -> std::size_t;
  [[nodiscard]] auto line_start(std::size_t line) const -> std::size_t;
  [[nodiscard]] auto line_end(std::size_t line) const -> std::size_t;
  [[nodiscard]] auto line_range(std::size_t line) const -> std::pair<std::size_t, std::size_t>;
  [[nodiscard]] auto column_of(std::size_t index) const -> std::size_t;
  [[nodiscard]] auto index_at_column(std::size_t line, std::size_t column) const -> std::size_t;
  [[nodiscard]] auto word_bounds(std::size_t index) const -> std::pair<std::size_t, std::size_t>;
  [[nodiscard]] auto matching_bracket() const -> std::optional<std::pair<std::size_t, std::size_t>>;
  /// 鼠标位置落在第几行（不在文本区则 -1）。
  [[nodiscard]] auto hover_line_at(const RenderContext& context, math::Point point) const -> int;
  /// 可达最大水平偏移。
  [[nodiscard]] auto max_scroll_x(const RenderContext& context) const -> float;
  void apply_h_scroll_drag(const RenderContext& context, float pointer_x);
  [[nodiscard]] auto line_indent(std::size_t line) const -> std::string;

  void push_undo(bool coalesce);
  void notify_change();
  void notify_cursor();
  void ensure_cursor_visible(const RenderContext& context);
  /// 处理按键：返回是否为本编辑器认识的键（false = 未识别，UiRoot 侧继续冒泡/下沉，
  /// 全局快捷键由此获得落点）。
  [[nodiscard]] auto handle_key(const RenderContext& context, const Event& event) -> bool;
  /// 覆写基类：**只标自己**，不把 layout 脏标记冒泡到根。
  /// 本组件的几何只由自身 `bounds_` + 字号决定，父容器无需重新 measure/arrange。
  /// 冒泡的代价实测极大：根一变脏 → `UiRoot::layout()` → `pending_full_ = true`
  /// → 每次编辑都整帧重绘（1280×800 下 11.5 ms），增量重绘完全失效。
  void mark_layout_dirty() override;
  void insert_newline();
  void erase_backward();
  void erase_forward();
  void delete_selection();
  void move_horizontal(int direction, bool extend);
  void move_vertical(const RenderContext& context, int direction, bool extend);
  void move_word(int direction, bool extend);
  void move_to_edge(bool to_end, bool extend);
  void move_document_edge(bool to_end, bool extend);
  void clamp_cursor();
  [[nodiscard]] auto indent_unit() const -> std::string;
  [[nodiscard]] auto indent_columns(const std::string& line) const -> std::size_t;

  std::string text_{};
  std::string language_{};
  std::shared_ptr<const text::LanguageSpec> custom_spec_{};
  std::size_t cursor_{0};
  std::size_t anchor_{0};
  float scroll_x_{0.0f};
  float scroll_y_{0.0f};
  float font_scale_{1.0f};
  /// 行距倍数（见 `set_line_spacing`）。
  float line_spacing_{kDefaultLineSpacing};
  /// 显式绝对字号（<=0 = 未设，按 `font_scale_` 跟主题）。
  float font_size_px_{-1.0f};
  /// 主题的基准字号（`apply_theme` 记下）。实际字号由它 **惰性算出**：
  ///
  /// 不能把结果缓存成快照——`apply_theme` 只在 `UiRoot::layout()` 里跑，
  /// 而改 `font_scale` 只标了元素脏（本组件故意不让布局脏冒泡到根）→
  /// 快照会停在旧值上（实测：档位 1.15→1.0 后 `font_size` 仍报 17.25）。
  float theme_base_font_{15.0f};
  int tab_width_{4};
  bool insert_spaces_{true};
  bool read_only_{false};
  bool show_line_numbers_{true};
  bool highlight_enabled_{true};
  bool indent_guides_{true};
  bool auto_pairs_{true};
  /// 拖选进行中（Mouse(左)Down 置位、MouseUp 清除；Move 期间扩选）。
  ///
  /// 不用 `event.button` 判定：Win32 的 `WM_MOUSEMOVE` 不携带按键状态（后端恒传 0），
  /// 按它判定拖选在真窗口永远不触发（实测发现；协议驱动的 move 默认 button=1 才"能用"）。
  bool selecting_{false};

  // 渲染缓存（`mutable`：`paint_content` 为 const，但需要惰性重建）。
  //
  // **分两层**：行索引（`line_spans_`）只随文本变、一趟扫描即可；高亮 token 还要看语言规则，
  // 贵一个数量级。分开后"取行号/算列"这类高频操作不会触发高亮重算（否则批量编辑退化为 O(n²)）。
  mutable bool spans_dirty_{true};
  mutable bool tokens_dirty_{true};
  mutable bool geometry_dirty_{true};  mutable std::vector<std::pair<std::size_t, std::size_t>> line_spans_{};
  mutable std::vector<LineTokens> line_tokens_{};
  mutable float line_height_cache_{0.0f};
  mutable float gutter_cache_{0.0f};
  mutable float max_line_width_cache_{0.0f};
  /// 悬停行（鼠标所在行；-1 = 无）——行底纹用，不参与内容缓存。
  int hover_line_{-1};
  /// 水平滚动条拖拽中（左键在滑块/轨道上按下后跟 move）。
  bool h_dragging_{false};
  /// 垂直滚动条拖拽中（同上，垂直方向）。
  bool v_dragging_{false};
  float v_drag_offset_{0.0f};
  float h_drag_offset_{0.0f};
  mutable std::shared_ptr<const text::LanguageSpec> spec_cache_{};
  mutable bool spec_resolved_{false};

  std::vector<Snapshot> undo_stack_{};
  std::vector<Snapshot> redo_stack_{};
  std::int64_t last_edit_ms_{0};

  // —— 查找态（命中表为字符区间，绘制与跳转共用；文本变化即失效重建）——
  std::string find_needle_{};
  bool find_case_{false};
  bool find_active_set_{false};                 ///< find_active_ 是否有效（空命中表也有"无选中"态）
  std::vector<std::pair<std::size_t, std::size_t>> find_matches_{};  ///< 升序 [begin,end)
  std::size_t find_active_{kNoFindMatch};
  bool find_enabled_{false};                    ///< set_find 后为 true（clear_find 置 false）

  void rebuild_find_matches();
  void select_find_match(std::size_t index);
  /// 文本被编辑后保持查找态：重建命中表、当前命中夹取。
  void refresh_find_after_edit();
};

}  // namespace st::ui
