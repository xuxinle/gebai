#pragma once

/// **文本选中与复制**——「一段可选文本」的公共模型，分两层：
///
/// | 层 | 内容 | 为什么要分层 |
/// |---|---|---|
/// | 纯函数层（`Caret` / `word_range_at` / `nearest_codepoint` / `slice_line`） | 「端点怎么比」「哪端近」「词界在哪」 | 与界面无关，可在无端口环境下钉死边界（UTF-8、反向拖选、空区间） |
/// | 行为层（`TextSelectionModel`） | 拖选状态机、双击选词、`Ctrl+A`、**高亮带几何** | 与组件形态无关，只有「坐标 ↔ 端点」映射因组件而异 |
///
/// ## 为什么要有这一层（2026-10-14，用户报「高亮还是没对上」之后）
///
/// 需求是「框架里**每个**文字渲染组件都要能选中与复制」。此前是**每个组件各写一套**：
/// `CodeEditor` / `MarkdownView` / `Terminal` / `Input`+`TextArea` / `Text`+`Heading`
/// 五套各自实现拖选、双击选词、`Ctrl+A/C`、`Esc`、高亮绘制——而框架里渲染文字的
/// 组件有二十多个，每接一个就要把同一批坑重踩一遍。
///
/// **实测代价**：`MarkdownView` 那套把「高亮带的垂直范围」算错两轮
///（先用了行高，后是基准差一个 `padding.top`），而正确几何在 `line_layout.hpp`
/// 里**早就写好了**（`layout_line` 的 `glyph_top`/`glyph_height`）。
/// 病根不是算错，是**同一件事存在多条实现**（`line_layout.hpp` 开篇那句话）。
///
/// ## 行为层收拢了什么
///
/// | 收在这里（与组件无关） | 由组件提供（随形态而异） |
/// |---|---|
/// | 拖选状态机（按下 / 移动 / 抬起、拖拽归属） | 控件坐标 → 端点（`hit_test`） |
/// | 双击选词、三击整行 | 行号 → 该行文本（`line_text`） |
/// | `Ctrl+A` 全选、`Esc` 清选 | 端点 → x 区间（`span_x`） |
/// | 选中文本切片（行间补 `\n`、行尾哨兵） | 行数、行在控件内的顶与盒高、字号角色 |
/// | **高亮带几何**（一律走 `layout_line` 的墨迹带） | — |
///
/// 三种坐标系（**字节偏移** / `{行,行内偏移}` / `{行,列}`）都能映射到端点
/// `{row, offset}` 这个公共超集上，所以一套实现盖得住全部形态：
/// `Input`/`TextArea` 的“全串偏移”换算一次即可；`Terminal` 的列乘 `cell_width` 即得 x。
///
/// ## 用法（组件持有一个成员即可）
///
/// ```cpp
/// TextSelectionModel selection_;
/// // 初始化一次（构造或首次 layout）：
/// selection_.set_host(TextSelectionHost{
///     .port = &text_port_of(context),
///     .line_count = [this] { return rows_.size(); },
///     .line_text  = [this](std::size_t r) -> std::string_view { return rows_[r].flat; },
///     .hit_test   = [this](st::math::Point p) { return caret_from_point(p); },
///     .span_x     = [this](std::size_t r, std::size_t a, std::size_t b) { ... },
///     .line_box   = [this](std::size_t r) { return std::pair{row_top(r), row_h(r)}; },
///     .line_font  = [this](std::size_t r) { return std::pair{size_of(r), role_of(r)}; },
/// });
/// ```
/// 然后：`on_event` 里转发鼠标/键盘 → `bands()` 的返回值直接交给 `fill_rect`。

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {

// —————————————————————— 纯函数层 ——————————————————————
//
// 与界面无关，可独立测（见 `tests/ui_text_selection_test.cpp`）。此前这些函数位于
// `src/ui/components/components_internal.hpp`（组件内部私有头），应用与子 Agent
// 拿不到、只能各抄一份——这正是“同一件事多条实现”的入口，故提升到公共层。

/// 车符位：`row` = 行下标（含义由组件定：视觉行 / 逻辑行 / 网格行），
/// `offset` = **行内字节偏移**（UTF-8 安全，总落在码点边界上）。
struct Caret {
  int row{-1};
  std::size_t offset{0U};
  [[nodiscard]] auto valid() const noexcept -> bool { return row >= 0; }
  friend auto operator==(const Caret& a, const Caret& b) noexcept -> bool {
    return a.row == b.row && a.offset == b.offset;
  }
};

/// 文档序比较（先比行、行同比偏移）。返回 <0 / ==0 / >0。
[[nodiscard]] inline auto compare_caret(const Caret& a, const Caret& b) noexcept -> int {
  if (a.row != b.row) return a.row < b.row ? -1 : 1;
  if (a.offset != b.offset) return a.offset < b.offset ? -1 : 1;
  return 0;
}

/// 选择区间（**已归一化**：`begin <= end`）。
struct SelectionRange {
  Caret begin{};
  Caret end{};
  [[nodiscard]] auto empty() const noexcept -> bool { return begin == end; }
  /// 单行内的选中区间（跨行时该行取整行）。
  [[nodiscard]] auto span_on_line(int row, std::size_t line_length) const
      -> std::pair<std::size_t, std::size_t> {
    if (row < begin.row || row > end.row) return {0U, 0U};
    const std::size_t from = row == begin.row ? begin.offset : 0U;
    const std::size_t to = row == end.row ? end.offset : line_length;
    return from <= to ? std::make_pair(from, to) : std::make_pair(to, from);
  }
};

/// 把两个端点归一化（拖选方向任意）。
[[nodiscard]] inline auto normalize_selection(const Caret& anchor, const Caret& focus)
    -> SelectionRange {
  return compare_caret(anchor, focus) <= 0 ? SelectionRange{anchor, focus}
                                           : SelectionRange{focus, anchor};
}

/// 行内**前缀宽表**查找：给定「每码点起始 x」表与目标 x，返回最近的**码点下标**。
///
/// 语义（与浏览器一致）：落在两码点边界之间时**靠哪端近就取哪端**。
/// `prefix` 长度 = 码点数 + 1（尾元素 = 行尾 x）。
[[nodiscard]] inline auto nearest_codepoint(const std::vector<float>& prefix, float x) noexcept
    -> std::size_t {
  if (prefix.size() <= 1U) return 0U;
  const std::size_t count = prefix.size() - 1U;
  if (x <= prefix.front()) return 0U;
  if (x >= prefix.back()) return count;
  std::size_t low = 0U;
  std::size_t high = count;
  while (low < high) {
    const std::size_t mid = (low + high) / 2U;
    if (prefix[mid] < x) low = mid + 1U;
    else high = mid;
  }
  if (low == 0U) return 0U;
  const float left = prefix[low - 1U];
  const float right = prefix[low];
  return (x - left) <= (right - x) ? low - 1U : low;
}

/// 词边界（双击选词用）：从 `offset` 出发向左/右扩展到词界，返回**字节偏移** `[begin, end)`。
///
/// 词字符定义：**非空白、非标点**（字母 / 数字 / 下划线；非 ASCII 一律算词字符，
/// 于是中文双击取连跑的一段汉字——与浏览器一致）。落在非词字符上时向**右**找最近词
/// （与浏览器“双击空格选中右词”一致）。
[[nodiscard]] inline auto word_range_at(std::string_view text, std::size_t offset) noexcept
    -> std::pair<std::size_t, std::size_t> {
  const std::size_t start = std::min(offset, text.size());
  // 退化到码点边界（调用方可能给到码点中间）。
  std::size_t pos = start;
  while (pos > 0U && pos < text.size() &&
         (static_cast<unsigned char>(text[pos]) & 0xC0U) == 0x80U) {
    --pos;
  }
  const auto is_word_byte = [](std::string_view s, std::size_t index) noexcept -> bool {
    const unsigned char byte = static_cast<unsigned char>(s[index]);
    if (byte < 0x80U) {
      return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
             (byte >= 'A' && byte <= 'Z') || byte == '_';
    }
    return true;   // 非 ASCII（含 CJK）：算词字符
  };
  std::size_t probe = pos;
  while (probe < text.size() && !is_word_byte(text, probe)) {
    const std::size_t next = st::utf8_next(text, probe);
    if (next <= probe) break;
    probe = next;
  }
  if (probe >= text.size()) return {pos, pos};
  std::size_t begin = probe;
  std::size_t end = probe;
  while (begin > 0U) {
    const std::size_t prev = st::utf8_prev(text, begin);
    if (!is_word_byte(text, prev)) break;
    begin = prev;
  }
  while (end < text.size() && is_word_byte(text, end)) {
    const std::size_t next = st::utf8_next(text, end);
    if (next <= end) break;
    end = next;
  }
  return {begin, end};
}

/// 按选择区间切片一行（`trim_trailing` = 行尾空白是否裁掉）。
[[nodiscard]] inline auto slice_line(std::string_view line, std::size_t from, std::size_t to,
                                     bool trim_trailing = false) -> std::string {
  const std::size_t safe_from = std::min(from, line.size());
  const std::size_t safe_to = std::min(std::max(to, safe_from), line.size());
  std::string_view piece = line.substr(safe_from, safe_to - safe_from);
  if (trim_trailing) {
    while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t')) {
      piece.remove_suffix(1U);
    }
  }
  return std::string(piece);
}

// —————————————————————— 行为层 ——————————————————————

/// 「行尾」哨兵：全选把末端偏移置它，避免为拿行尾去折行
///（`selected_text()` 是 const 无上下文接口，无从折行）。
inline constexpr std::size_t kLineEndSentinel = static_cast<std::size_t>(-2);

/// 高亮带：x 区间相对**控件左缘**，`top`/`height` 相对**控件顶**（墨迹带，非行盒）。
struct SelectionBand {
  float x0{0.0F};
  float x1{1.0F};
  float top{0.0F};
  float height{0.0F};
};

/// 组件形态适配（接入选区时实现这几个回调；只需一次 `set_host`）。
struct TextSelectionHost {
  /// 文本端口（算墨迹带用；报不出 `ink_metrics` 时退化为行盒的 0.8 倍高）。
  const TextPort* port{nullptr};
  /// 行数。
  std::function<std::size_t()> line_count;
  /// 行号 → 该行纯文本（越界返回空）。
  std::function<std::string_view(std::size_t)> line_text;
  /// 控件坐标 → 端点（落在字符边界之间取最近端；`offset` 用**字节偏移**）。
  std::function<Caret(math::Point)> hit_test;
  /// 行内 `[from, to)` → x 区间（相对控件左缘）。
  std::function<std::pair<float, float>(std::size_t, std::size_t, std::size_t)> span_x;
  /// 行在**控件内**的顶 + 行盒高。
  std::function<std::pair<float, float>(std::size_t)> line_box;
  /// 该行的字号与角色（必须与**该行文字绘制时**用的值一致——基线由它决定）。
  std::function<std::pair<float, text::FontRole>(std::size_t)> line_font;
};

/// 可选文本的**状态与交互**（组件持有一个成员，不必继承）。
class TextSelectionModel {
 public:
  void set_host(TextSelectionHost host) { host_ = std::move(host); }

  /// 是否允许选中（默认**关**：静态标签遍布界面，默认抢拖选手势会截断用户
  /// 想划选一大片的意图；需要「能选中一小段」的展示型文本显式打开）。
  void set_enabled(bool value) {
    enabled_ = value;
    if (!value) clear();
  }
  [[nodiscard]] auto enabled() const noexcept -> bool { return enabled_; }

  // —— 状态 ——
  [[nodiscard]] auto has_selection() const -> bool {
    if (!enabled_ || !anchor_.valid() || !focus_.valid()) return false;
    const SelectionRange range = normalize_selection(anchor_, focus_);
    return !range.empty();
  }
  [[nodiscard]] auto range() const -> SelectionRange {
    return normalize_selection(anchor_, focus_);
  }
  [[nodiscard]] auto selecting() const noexcept -> bool { return selecting_; }
  void clear() noexcept {
    anchor_ = Caret{};
    focus_ = Caret{};
    selecting_ = false;
  }
  /// 以端点设置区间（宿主 / 自动化用）。
  void set_range(Caret anchor, Caret focus) {
    anchor_ = anchor;
    focus_ = focus;
  }
  /// 全选（末行用哨兵，不折行）。
  void select_all() {
    if (!enabled_ || !host_.line_count) return;
    const std::size_t count = host_.line_count();
    if (count == 0U) return;
    anchor_ = Caret{0, 0U};
    focus_ = Caret{static_cast<int>(count - 1U), kLineEndSentinel};
  }

  /// 选中文本（行间补 `\n`；哨兵解析为整行）。
  [[nodiscard]] auto selected_text() const -> std::string {
    if (!has_selection() || !host_.line_text) return {};
    const SelectionRange r = range();
    std::string out;
    for (int row = r.begin.row; row <= r.end.row; ++row) {
      const std::string_view line = host_.line_text(static_cast<std::size_t>(row));
      if (row > r.begin.row) out += '\n';
      const std::size_t from =
          row == r.begin.row ? std::min(r.begin.offset, line.size()) : 0U;
      // ⚠ 末端的偏移**只在末行生效**，且哨兵要解析成“到行尾”——
      // 否则 `select_all`（末偏移 = 哨兵）会选出空串（实测在 `Text` 上踩过）。
      std::size_t to = line.size();
      if (row == r.end.row) {
        to = r.end.offset == kLineEndSentinel ? line.size()
                                              : std::min(r.end.offset, line.size());
      }
      if (to > from) out += std::string(line.substr(from, to - from));
    }
    return out;
  }

  // —— 交互（组件从 `on_event` 转发；返回 true = 已处理）——

  auto on_press(math::Point point, bool extend) -> bool {
    if (!enabled_ || !host_.hit_test) return false;
    const Caret caret = host_.hit_test(point);
    if (extend && anchor_.valid()) {
      focus_ = caret;
    } else {
      anchor_ = caret;
      focus_ = caret;
    }
    selecting_ = true;
    return true;
  }
  auto on_drag(math::Point point) -> bool {
    if (!enabled_ || !selecting_ || !host_.hit_test) return false;
    const Caret next = host_.hit_test(point);
    if (next == focus_) return false;
    focus_ = next;
    return true;
  }
  auto on_release() -> bool {
    const bool was = selecting_;
    selecting_ = false;
    return was;
  }
  /// 双击选词。
  auto select_word_at(math::Point point, bool extend = false) -> bool {
    if (!enabled_ || !host_.hit_test || !host_.line_text) return false;
    const Caret caret = host_.hit_test(point);
    const auto [from, to] = word_range_at(host_.line_text(static_cast<std::size_t>(caret.row)),
                                          caret.offset);
    if (!extend) anchor_ = Caret{caret.row, from};
    focus_ = Caret{caret.row, to};
    return true;
  }
  /// 三击整行。
  auto select_line_at(math::Point point) -> bool {
    if (!enabled_ || !host_.hit_test) return false;
    const Caret caret = host_.hit_test(point);
    anchor_ = Caret{caret.row, 0U};
    focus_ = Caret{caret.row, kLineEndSentinel};
    return true;
  }
  /// 键盘：`Ctrl+A` 全选 / `Esc` 清选（有选区才算处理，否则放行给宿主）。
  auto on_key(std::string_view key, bool ctrl) -> bool {
    if (!enabled_) return false;
    if (ctrl && (key == "a" || key == "A")) {
      select_all();
      return true;
    }
    if (key == "Escape" && has_selection()) {
      clear();
      return true;
    }
    return false;
  }

  // —— 绘制 ——
  //
  // 高亮带一律走 `layout_line` 的**墨迹带**（`glyph_top`/`glyph_height`）——
  // 那是「一行文字真占多高」的唯一口径。组件**不要**再自己推 `ascent`/`padding`：
  // 那正是 `MarkdownView` 连错两轮的成因。
  [[nodiscard]] auto bands() const -> std::vector<SelectionBand> {
    std::vector<SelectionBand> out;
    if (!has_selection() || host_.port == nullptr || !host_.span_x || !host_.line_box ||
        !host_.line_font) {
      return out;
    }
    const TextPort& port = *host_.port;
    const SelectionRange r = range();
    for (int row = r.begin.row; row <= r.end.row; ++row) {
      const auto index = static_cast<std::size_t>(row);
      const std::string_view line =
          host_.line_text ? host_.line_text(index) : std::string_view{};
      const std::size_t from = row == r.begin.row ? std::min(r.begin.offset, line.size()) : 0U;
      std::size_t to = line.size();
      if (row == r.end.row) {
        to = r.end.offset == kLineEndSentinel ? line.size()
                                              : std::min(r.end.offset, line.size());
      }
      const auto [box_y, box_h] = host_.line_box(index);
      const auto [size, role] = host_.line_font(index);
      const LineGeometry geom = layout_line(port, kInkReferenceSample, size, box_h, role);
      if (to <= from) {
        // 空行确实被选中时给一个可见窄块（否则“选了几行却有一行没高亮”，像漏画）。
        if (to == 0U && from == 0U && r.begin.row != r.end.row) {
          const auto [x0, x1] = host_.span_x(index, 0U, 0U);
          out.push_back(SelectionBand{.x0 = std::min(x0, x1),
                                      .x1 = std::min(x0, x1) + 6.0F,
                                      .top = box_y + geom.glyph_top,
                                      .height = geom.glyph_height});
        }
        continue;
      }
      const auto [x0, x1] = host_.span_x(index, from, to);
      out.push_back(SelectionBand{.x0 = std::min(x0, x1),
                                  .x1 = std::max(x0, x1),
                                  .top = box_y + geom.glyph_top,
                                  .height = geom.glyph_height});
    }
    return out;
  }

 private:
  TextSelectionHost host_{};
  Caret anchor_{};
  Caret focus_{};
  bool selecting_{false};
  bool enabled_{false};
};

}  // namespace st::ui
