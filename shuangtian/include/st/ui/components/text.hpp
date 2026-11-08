#pragma once

/// @file text.hpp
/// @brief `Text`：文本标签（单行省略，或 `set_multiline` 折行显示）。。
///
/// 从 `basic.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `basic.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 文本标签（单行省略，或 `set_multiline` 折行显示）。
class Text : public Element {
 public:
  explicit Text(std::string content = {});
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Text"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Text; }

  void set_content(std::string content);
  [[nodiscard]] auto content() const -> const std::string& { return content_; }
  /// 事件：仅在 `set_selectable(true)` 时处理拖选 / `Ctrl+A` / `Ctrl+C`（其余一律放行）。
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void set_multiline(bool multiline);
  void set_max_lines(std::size_t lines);
  void set_tone(Tone tone);

  /// 字号档位（**语义化**：跟随主题令牌，而不是写死像素值）。
  ///
  /// 为何需要它（而不是让调用方写 `set_font_size(12.0f)`）：
  /// 字号是**主题的尺度**——同一份界面在 11pt 与 12pt 基准下应当整体等比，
  /// 而字面量不会跟。实测代价（2026-10-27）：`gallery` 里 49/150 个文本节点
  /// 用了阶梯外的 11/11.5/12/12.5px，四种散值意味着排版没有节奏；
  /// 根因是**阶梯缺 11~12px 档**（`font_xs` 曾是 13），调用方只能写字面量。
  /// 补上 `font_2xs=11` 并把 `font_xs` 降到 12 后，本枚举让这些地方有语义名可用。
  ///
  /// `Override` 是默认值（不改写，仍由 `set_font_size` 说了算），
  /// 所以加这个枚举对既有代码零影响。
  enum class Scale : std::uint8_t {
    Override,  ///< 不按档位：用 `set_font_size` 的值（否则 `font_base`）
    Xs2,       ///< `Metrics::font_2xs`（11）——版本号/分组名/最次要标注
    Xs,        ///< `Metrics::font_xs`（12）——卡片说明/状态行
    Sm,        ///< `Metrics::font_sm`（14）——次要正文
    Base,      ///< `Metrics::font_base`（15）——正文
    Lg,        ///< `Metrics::font_lg`（17）
    Xl,        ///< `Metrics::font_xl`（20）
  };
  /// 按语义档位取字号（可在 `apply_theme` 里跟随主题重算）。
  void set_scale(Scale scale);
  [[nodiscard]] auto scale() const noexcept -> Scale { return scale_; }

  /// 档位 → 主题令牌（`Override` 返回 0）。
  ///
  /// 公开而不仅内部用：测试需要拿**同一份**映射去断言“档位真的落到那个令牌上”，
  /// 否则测试里再写一遍 switch 就成了测自己的逻辑（回退生产代码仍会全绿）。
  [[nodiscard]] static auto font_size_for_scale(const Metrics& metrics, Scale scale) -> float;

  /// 显式字号（像素）。设了它就盖过 `set_scale`（记在 `font_size_override_`）。
  void set_font_size(float size);
  void set_weight(FontWeight weight);

  /// **可选**（默认关）：开启后文本可用鼠标拖选、`Ctrl+A` 全选、`Ctrl+C` 复制选中。
  ///
  /// 为何默认关（而不是像 MarkdownView 那样默认开）：
  /// 静态标签遍布界面（表单说明、列表副标题、状态文字…），若默认抢拖选手势，
  /// 用户想拖动窗口/划选整个区域时会被逐个标签截断——那不是“能力”，是干扰。
  /// 需要“能选中某一小段”的展示型文本显式打开。
  void set_selectable(bool selectable);
  [[nodiscard]] auto selectable() const noexcept -> bool { return selectable_; }
  /// 选中文本（无选区时为空串）。
  [[nodiscard]] auto selected_text() const -> std::string;
  /// 是否有非空选区。
  [[nodiscard]] auto has_selection() const -> bool;
  /// 清空选区 / 全选（自动化与宿主动作面用）。
  void clear_selection() noexcept;
  void select_all();

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return content_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  /// 只读属性（`selection`）：`get` 能读、`set` 不生效。
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  /// `copy`：把文本内容写进宿主剪贴板（2026-10-12——文本标签此前完全无法复制：
  /// 键盘、动作面、协议全无路径，右键菜单也无从谈起）。
  /// 控制通道 `invoke <id> copy` 即可达；写失败（无宿主）返回 false 如实上报。
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 一条可见行（含它在本控件内的 x 区间）——选区与绘制共用同一份。
  struct RenderedLine {
    std::string text{};   ///< 本行的**显示文本**（折行/截断后的结果，不是原文切片）
    float x{0.0f};        ///< 行首 x（受 `text_align` 影响）
    float width{0.0f};
    float y{0.0f};
  };
  /// 算出当前应当显示的每一行（与 `paint_content` / `measure` 同一套口径）。
  ///
  /// 为何要抽出来：选区命中必须建在**实际显示的行**上，而不是原文字符串的
  /// 下标——折行与 `max_lines_` 截断会让两者彻底不同（截断后多出的字根本不展示，
  /// 选中它就会出现“选了但看不见”）。
  [[nodiscard]] auto rendered_lines(const RenderContext& context) const -> std::vector<RenderedLine>;
  /// 坐标 → `{行下标, 行内字节偏移}`（越界夹到首/末行）。
  [[nodiscard]] auto caret_at(const RenderContext& context, math::Point point) const
      -> std::pair<std::size_t, std::size_t>;
  /// `{行下标, 行内偏移}` → 屏幕 x。
  [[nodiscard]] auto offset_to_x(const RenderContext& context, std::size_t line,
                                 std::size_t offset) const -> float;

  std::string content_{};
  bool multiline_{false};
  std::size_t max_lines_{0};
  Tone tone_{Tone::Default};
  Scale scale_{Scale::Override};
  float font_size_override_{0.0f};
  FontWeight weight_{FontWeight::Regular};

  // —— 选区（2026-10-14）——
  // 用 `{行, 行内字节偏移}`（而不是全串字节偏移）：`Text` 的显示行是**离散字符串
  // 数组**（折行/截断产生），没有“全串偏移 ↔ 显示位置”的线性映射。
  bool selectable_{false};
  /// 上次排版得到的**显示行文本**（`measure`/`paint_content` 时写）。
  ///
  /// 为何缓存：① `selected_text()` 无上下文、无从折行；② 更重要的是
  /// “选中的字”必须与“屏幕上看到的字”是同一份——现场重折行会在窗口尺寸
  /// 变化后选出别处的字。
  /// `mutable`：在 `paint_content`（const）里也要写。
  mutable std::vector<std::string> display_lines_{};
  /// 显示行的**行宽缓存**（与 `display_lines_` 一一对应）。
  ///
  /// 为何需要（2026-10-14，实测）：多行 `Text` 的 `paint_content` 过去每帧
  /// 都 `port.wrap(content_)` **重新折行全部内容**、并逐行 `measure_width`、
  /// 再逐行 `draw`——全不看视口。把整份日志交给一个多行 `Text` 的场景
  /// （`Terminal` 行模式就是这么做的）因而以**内容长度**为每帧成本：
  /// 实测稳态帧 500 行 **47 ms** / 2000 行 **705 ms** / 20000 行 **3299 ms**
  /// （`tools/ui_big_editors_bench.cpp`）。
  ///
  /// 修法：折行结果与行宽都**按 (`content_`, `bounds_.width`, 字号) 缓存**，
  /// 三者不变就复用（滚动时它们不变——滚动是内容顶的偏移，不是几何变化），
  /// 且 `paint_content` 按裁剪区**只画可见那几行**。
  ///
  /// 为何缓存的三元组是这几个：折行宽度只取决于可用宽度与字号
  /// （`wrap(content, size, max_width)`），内容变了才需重折；对齐只影响行首 x
  /// （不参与缓存，每帧现算）。
  ///
  /// ⚠ **第四维：`TextPort`**（2026-10-14 合并前复核补）。折行结果**也取决于字宽**，
  /// 而字宽来自当时的端口/字体。漏掉这一维的话，任何“只换端口、其余全不变”的
  /// 路径（热重载字体、无头/窗口双端口切换、多窗口共享组件）都会**静默复用
  /// 旧字体折的行**（换行位置/行数错，且不报错）。
  ///
  /// 当前生产代码只有启动时 `set_text_port` 一次，所以是**潜伏脆弱点而非活缺陷**
  /// ——但 `CodeEditor` 早就把端口纳入自己的缓存判据（`layout_port_`），
  /// 这里是**对齐遗漏**；补上后两者口径一致。
  mutable std::vector<float> display_line_widths_{};
  mutable std::string layout_cache_content_{};
  mutable float layout_cache_width_{-1.0F};
  mutable float layout_cache_font_size_{-1.0F};
  mutable const st::ui::TextPort* layout_cache_port_{nullptr};
  /// 选区两端（已归一）；`line == npos` = 无选区。
  std::size_t select_begin_line_{std::string::npos};
  std::size_t select_begin_offset_{0};
  std::size_t select_end_line_{0};
  std::size_t select_end_offset_{0};
  bool selecting_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Text, "Text")
