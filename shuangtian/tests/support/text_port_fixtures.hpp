#pragma once

/// UI 测试共用的 `ui::TextPort` 实现（**取代 10 份逐文件复制的桩**）。
///
/// ## 为什么有这个文件
///
/// `ui` 层刻意不依赖 `text` 层（依赖倒置，见 `include/st/ui/text_port.hpp`），
/// 于是每个 UI 测试都要**自己当宿主**，把某种文本度量注入 `UiRoot`。
/// 结果是 10 个测试文件各自复制了一份桩，命名五花八门
/// （`FontPort`×3 / `TableTestTextPort` / `FeedbackTestTextPort` / `DeclarativeTestTextPort` /
/// `ScriptTestTextPort` / `FixtureTextPort` / `StubTextPort` / `SpyPort`），**合计 452 行**。
///
/// 代价不只是行数：**`TextPort` 接口每加一个方法，就要改 10 个文件**——
/// 而这是接口演进时的固定税。结构评审用代码克隆检测扫出这批重复后，按
/// **实测划出的三组**做成共享 fixture：
///
/// | 组 | 语义 | 谁在用 |
/// |---|---|---|
/// | `test::FixedAdvanceTextPort` | **定宽**（每字符 `kAdvance`），只记录画过什么、不落像素 | Tabs/Select/Toggle/Slider、Table、Feedback、声明式 |
/// | `test::ProportionalTextPort` | **比例宽度**（ASCII ≈ 0.55·size，非 ASCII = size），并**真的画像素** | Markdown（亚像素对齐要真画才测得出） |
/// | `test::RecordingTextPort` | **包装**任意 `TextPort`，转发全部调用并记录 | 字重链路（Spy） |
///
/// **不强行三合一**：它们的行为不同（定宽 vs 比例、画不画像素、记不记录），
/// 合并成一个会破坏各自的测试意图——那正是"为消重复而消重复"。共享的是**真正一致的部分**。
///
/// ## 为什么放在 `tests/support/`
///
/// 不放 `include/st/test/`（那是框架的公开测试设施，测试桩不属于框架 API），
/// 也不放某个 `.cpp` 里（跨文件要用）。`tests/support/*.hpp` 由测试目标直接编译进每个
/// 测试单元——**头文件形式**，不引入额外源文件与链接顺序问题。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/text/text.hpp"
#include "st/ui/text_port.hpp"

namespace st::test {

/// 定宽文本端口：每个码点固定 `kAdvance` 宽，**只记录不落像素**。
///
/// 用途：布局与命中测试（断言只关心"这个文本占多少宽"，不关心像素）。
/// `draw` 把文本记进 `drawn`，供"这段文字到底有没有被画"这类断言。
class FixedAdvanceTextPort final : public ui::TextPort {
 public:
  static constexpr float kAdvance{8.0f};

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> math::Size override {
    return math::Size{measure_width(utf8, size), line_height(size)};
  }

  [[nodiscard]] auto measure_width(
      std::string_view utf8, float size,
      text::FontRole role = text::FontRole::Proportional) const -> float override {
    (void)role;  // 桩：等宽与比例同宽，无需区分
    (void)size;
    return kAdvance * static_cast<float>(utf8_length(utf8));
  }

  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.45f; }

  void draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color, text::FontRole role = text::FontRole::Proportional,
            float embolden = 0.0f, bool = false) const override {
    (void)role;
    (void)embolden;  // 桩不模拟字重：断言只关心布局与颜色
    (void)canvas;
    (void)origin;
    (void)size;
    (void)color;
    drawn_.emplace_back(utf8);
  }

  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    if (max_width <= 0.0f) return {};
    if (measure_width(utf8, size) <= max_width) return std::string(utf8);
    const auto capacity = static_cast<std::size_t>(max_width / kAdvance);
    if (capacity <= 1U) return std::string("…");
    return std::string(utf8_slice(utf8, 0, capacity - 1U)) + "…";
  }

  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    // 定宽模型下等宽折行（不截断）。
    std::vector<std::string_view> lines;
    (void)size;
    if (max_width <= 0.0f || utf8.empty()) return lines;
    const auto per_line = static_cast<std::size_t>(std::max(max_width / kAdvance, 1.0f));
    std::size_t offset = 0;
    while (offset < utf8.size()) {
      const std::size_t take = std::min(per_line, utf8.size() - offset);
      lines.push_back(utf8.substr(offset, take));
      offset += take;
    }
    return lines;
  }

  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    // 与原 `TableTestTextPort` / `FeedbackTestTextPort` 的桩同语义：
    // 超出 `max_lines` 时末行以省略号收尾，且**结果永不为空**（空输入也给一行空串）。
    std::vector<std::string> lines;
    (void)size;
    if (max_width <= 0.0f) {
      lines.emplace_back();
      return lines;
    }
    const auto per_line = static_cast<std::size_t>(std::max(max_width / kAdvance, 1.0f));
    std::size_t offset = 0;
    while (offset < utf8.size()) {
      const std::size_t take = std::min(per_line, utf8.size() - offset);
      if (max_lines > 0U && lines.size() + 1U >= max_lines && offset + take < utf8.size()) {
        lines.emplace_back(std::string(utf8.substr(offset)) + "…");
        return lines;
      }
      lines.emplace_back(utf8.substr(offset, take));
      offset += take;
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
  }

  /// 记录所有被绘制的文本（顺序即绘制顺序）——`draw` 的可见副作用。
  [[nodiscard]] auto drawn() const -> const std::vector<std::string>& { return drawn_; }

 private:
  mutable std::vector<std::string> drawn_{};
};

/// 比例文本端口：ASCII ≈ `0.55 * size`，非 ASCII = `size`；**真的画像素**。
///
/// 用途：需要**真实像素**的断言（换行位置、亚像素对齐、文本区块落点）。
/// `draw` 把每个文本段落成**整像素矩形**（端口输出与坐标像素对齐无关，
/// 断言只关心颜色与位置）。
class ProportionalTextPort final : public ui::TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> math::Size override {
    return math::Size{measure_width(utf8, size), line_height(size)};
  }

  [[nodiscard]] auto measure_width(
      std::string_view utf8, float size,
      text::FontRole role = text::FontRole::Proportional) const -> float override {
    (void)role;  // 桩：等宽与比例同宽，无需区分
    float width = 0.0f;
    std::size_t index = 0;
    while (index < utf8.size()) {
      const Codepoint codepoint = decode_utf8(utf8, index);
      width += codepoint.value < 0x80U ? size * 0.55f : size;
    }
    return width;
  }

  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.45f; }

  void draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color, text::FontRole role = text::FontRole::Proportional,
            float embolden = 0.0f, bool = false) const override {
    (void)role;
    (void)embolden;  // 桩不模拟字重
    if (utf8.empty()) return;
    const float x = std::round(origin.x);
    const float y = std::round(origin.y + size * 0.25f);
    const float width = std::max(std::round(measure_width(utf8, size)), 1.0f);
    const float height = std::max(std::round(size * 0.7f), 1.0f);
    canvas.fill_rect(math::Rect{x, y, width, height}, raster::Paint::solid(color));
  }

  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    if (measure_width(utf8, size) <= max_width) return std::string(utf8);
    const float ellipsis_width = size;  // 「…」按一个全角宽度计
    float budget = max_width - ellipsis_width;
    if (budget <= 0.0f) return std::string("…");
    std::size_t offset = 0;
    std::size_t kept = 0;
    while (offset < utf8.size()) {
      const std::size_t start = offset;
      const Codepoint codepoint = decode_utf8(utf8, offset);
      const float advance = codepoint.value < 0x80U ? size * 0.55f : size;
      if (budget - advance < 0.0f) break;
      budget -= advance;
      kept = offset;
      (void)start;
    }
    return std::string(utf8.substr(0, kept)) + "…";
  }

  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    std::vector<std::string_view> lines;
    if (utf8.empty() || max_width <= 0.0f) return lines;
    std::size_t line_start = 0;
    std::size_t offset = 0;
    float width = 0.0f;
    while (offset < utf8.size()) {
      const std::size_t character_start = offset;
      const Codepoint codepoint = decode_utf8(utf8, offset);
      const float advance = codepoint.value < 0x80U ? size * 0.55f : size;
      if (width + advance > max_width && character_start > line_start) {
        lines.push_back(utf8.substr(line_start, character_start - line_start));
        line_start = character_start;
        width = 0.0f;
      }
      width += advance;
    }
    if (line_start < utf8.size()) lines.push_back(utf8.substr(line_start));
    return lines;
  }

  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    const auto pieces = wrap(utf8, size, max_width);
    std::vector<std::string> lines;
    for (std::size_t index = 0; index < pieces.size(); ++index) {
      if (max_lines > 0U && index + 1U == max_lines && pieces.size() > max_lines) {
        lines.emplace_back(std::string(pieces[index]) + "…");
        break;
      }
      lines.emplace_back(pieces[index]);
    }
    return lines;
  }
};

/// 记录型端口：**包装**任意 `TextPort`，转发全部调用并记录 `draw` 的字号/字重/外扩。
///
/// 用途：不动真实渲染路径地观测"调用方到底请求了什么"（如字重链路是否真的把
/// `weight` 传到了绘制层）。这正是原先各文件里那个 `SpyPort` 的用途。
class RecordingTextPort final : public ui::TextPort {
 public:
  explicit RecordingTextPort(const ui::TextPort& inner) : inner_(inner) {}

  /// 一次 `draw` 调用留下的记录。
  struct DrawCall {
    std::string text{};
    float size{0.0f};
    float embolden{0.0f};
    bool bold{false};
    text::FontRole role{text::FontRole::Proportional};
  };

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> math::Size override {
    return inner_.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(
      std::string_view utf8, float size,
      text::FontRole role = text::FontRole::Proportional) const -> float override {
    return inner_.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return inner_.line_height(size);
  }
  void draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color, text::FontRole role = text::FontRole::Proportional,
            float embolden = 0.0f, bool bold = false) const override {
    last_bold = bold;
    last_embolden = embolden;
    calls.push_back(DrawCall{std::string(utf8), size, embolden, bold, role});
    inner_.draw(canvas, utf8, origin, size, color, role, embolden, bold);
  }
  [[nodiscard]] auto has_real_bold() const -> bool override { return inner_.has_real_bold(); }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return inner_.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    return inner_.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    return inner_.wrap_limited(utf8, size, max_width, max_lines);
  }

  /// 最近一次 `draw` 的字重相关实参（原 `SpyPort` 的公开面，保持测试写法不变）。
  mutable bool last_bold{false};
  mutable float last_embolden{0.0f};
  /// 全部 `draw` 调用（比上面两个更完整，新用例可用）。
  mutable std::vector<DrawCall> calls{};

 private:
  const ui::TextPort& inner_;
};

/// 真实字体端口：把 `text::TextRenderer` 适配成 `ui::TextPort`。
///
/// 这是**唯一会真正光栅化字形**的一组，给需要真实度量的用例用
/// （如 Tabs 的指示条位置、Slider 的高度、Toggle 的布局——它们都依赖真字宽）。
///
/// 为什么它也归到这里：原先这份适配在 `ui_tabs_test` / `ui_slider_test` / `ui_toggle_test`
/// 里**逐字复制了三遍**。适配本身不是重复——重复的是三份拷贝。
class RendererTextPort final : public ui::TextPort {
 public:
  explicit RendererTextPort(const text::FontStack& stack) : renderer_(stack) {}

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> math::Size override {
    return renderer_.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(
      std::string_view utf8, float size,
      text::FontRole role = text::FontRole::Proportional) const -> float override {
    return renderer_.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return renderer_.line_height(size);
  }
  void draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color, text::FontRole role = text::FontRole::Proportional,
            float embolden = 0.0f, bool bold = false) const override {
    // 与三个原文件**逐字一致**：`embolden` 转给渲染器，`bold` 不转
    // （合成加粗由 `TextRenderer` 按 embolden 落位；`bold` 只影响选取真粗体面，
    //  测试里刻意不叠加，避免断言基线随机器装的字体而变）。
    (void)role;
    (void)bold;
    renderer_.draw(canvas, utf8, origin, size, color, role, embolden);
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return renderer_.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    return renderer_.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    std::vector<std::string> lines;
    for (const auto& line : renderer_.wrap(utf8, size, max_width)) {
      if (lines.size() >= max_lines) break;
      lines.emplace_back(line);
    }
    return lines;
  }

 private:
  text::TextRenderer renderer_;
};

}  // namespace st::test
