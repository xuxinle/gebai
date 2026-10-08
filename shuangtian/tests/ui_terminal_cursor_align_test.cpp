/// 终端**光标块与文字行对齐**回归测试。
///
/// ## 为什么需要这组用例
///
/// 用户报的现象：**「光标位置还是不对——是在实际行上一行」**。
///
/// 根因是**坐标口径不一致**（不是位置算错）：
///
/// * `TextPort::draw` 的 `origin` 契约是**行左上角**（`include/st/ui/text_port.hpp` 写明，
///   实现里 `TextRenderer::draw` 自己算 `origin.y + ascent(size)` 作基线）；
/// * 终端渲染传的是 `y + ascent`——**ascent 被加了两次**，文字每行整体下移约半行
///   （实测 8px，`tests` 外部用无头截图 + 像素分析量出：光标块 y=72..94、文字 y=80..101）。
///
/// 于是"光标按行顶画、文字低半行"看起来就像**光标停在了上一行**。
///
/// ## 判据
///
/// **传给 `draw` 的 `origin.y` 必须等于光标块顶边 y**（两者都是"行顶"，同一坐标口径）。
/// 这条判据不涉及任何字体度量换算——它钉的就是那个被破坏的契约本身。
///
/// 桩里 `ascent` 刻意远大于 `line_height - ascent`，且 `line_height` 也不等于 `ascent`：
/// 若实现再次把 `ascent` 加进 origin，偏差 ≈ ascent（14px），本用例必然变红。

#include "st/test/test.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "st/text/ansi_screen.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Terminal;

/// 记录每次 `draw` 的 `origin` 的桩端口（**真的画一条实心墨带**，以便像素对照）。
///
/// 墨带高度 = `ink_height`，从 `origin.y` 起画——与"origin 是行左上角"的契约一致：
/// 真实 `TextRenderer` 会把基线放在 `origin.y + ascent`，这里简化为从 origin 起的实心块，
/// 因此**墨带上沿 == 传入的 origin.y**，像素可测。
class BandPort final : public st::ui::TextPort {
 public:
  static constexpr float kCellW{8.0f};
  static constexpr float kLineH{20.0f};
  static constexpr float kAscent{16.0f};      ///< 刻意大：双重相加会偏 16px
  static constexpr float kInkHeight{12.0f};

  struct Draw {
    std::string text;
    float x{0.0f};
    float y{0.0f};
  };
  mutable std::vector<Draw> draws{};

  [[nodiscard]] auto measure(std::string_view utf8, float) const -> st::math::Size override {
    return st::math::Size{width_of(utf8), kLineH};
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float,
                                   st::text::FontRole) const -> float override {
    return width_of(utf8);
  }
  [[nodiscard]] auto line_height(float) const -> float override { return kLineH; }
  [[nodiscard]] auto ascent(float) const -> float override { return kAscent; }
  [[nodiscard]] auto descent(float) const -> float override { return kLineH - kAscent; }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float,
            st::math::Color color, st::text::FontRole, float, bool) const override {
    draws.push_back(Draw{std::string{utf8}, origin.x, origin.y});
    canvas.fill_rect(st::math::Rect{origin.x, origin.y, width_of(utf8), kInkHeight},
                     st::raster::Paint::solid(color));
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float) const
      -> std::string override {
    return std::string{utf8};
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float, std::size_t) const
      -> std::vector<std::string> override {
    return {std::string{utf8}};
  }

 private:
  [[nodiscard]] static auto width_of(std::string_view utf8) -> float {
    return kCellW * static_cast<float>(utf8.size());
  }
};

/// 第一行文字的绘制起点 y（取 y 最小的那条 draw 记录）。
[[nodiscard]] auto first_text_origin_y(const BandPort& port) -> float {
  float best = 1.0e9f;
  for (const auto& d : port.draws) best = std::min(best, d.y);
  return best;
}

}  // namespace

/// **核心契约**：文字绘制 `origin.y` == 光标块顶边 y（同一"行顶"口径）。
///
/// 做法：驱动组件渲染一屏内容，然后比较两件事——
/// 1. 桩记录到的文字 `origin.y`；
/// 2. 组件的行顶几何（`output_rect_.y + 4`，即 `paint_content` 里的 `origin_y`）。
///
/// 两者必须相等。若实现多加了 `ascent`，第 1 项会比第 2 项大 `kAscent`。
ST_TEST(terminal_text_origin_y_equals_line_top) {
  BandPort port{};
  st::ui::Theme theme{st::ui::Theme::dark()};
  st::ui::UiRoot root{};
  root.set_theme(theme);
  root.set_viewport(st::math::Size{800.0f, 400.0f});
  root.set_text_port(&port);

  auto owned = std::make_unique<Terminal>();
  Terminal* terminal = owned.get();
  root.set_content(std::move(owned));
  root.layout(true);

  // PTY 模式才有逐格屏幕渲染；起真 shell（老系统跳过）。
  if (!st::process::PtySession::supported()) return;
  terminal->open_shell();

  // 等一帧渲染：`pump` 把字节喂进屏幕，随后 paint 才有内容。
  st::raster::Canvas canvas{800, 400, 1.0f};
  for (int i = 0; i < 60 && port.draws.empty(); ++i) {
    terminal->pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    root.paint(canvas);
  }
  ST_REQUIRE(!port.draws.empty());

  const float text_y = first_text_origin_y(port);
  // 行顶（组件内 `origin_y = output_rect_.y + 4`）。
  const float line_top = terminal->output_rect().y + 4.0f;
  ST_CHECK_NEAR(text_y, line_top, 0.5f);
  // 反向护栏：文字起点**不能**比行顶低一整段 ascent（那就是双重相加）。
  ST_CHECK(text_y < line_top + BandPort::kAscent * 0.5f);
  terminal->send_stop();
}

/// 光标块与文字**同一行顶**：`cursor_rect` 的 y == 文字 draw 的 origin.y。
///
/// 这条是用户可见现象的直接护栏——"光标在上一行"就是两者不等。
/// 判据走组件的 `cursor_rect` 属性（像素矩形），不扫截图：
/// 扫像素会命中标签栏/边框等其他 UI，量不准（本用例第一版就踩到）。
ST_TEST(terminal_cursor_block_starts_at_same_y_as_text) {
  BandPort port{};
  st::ui::Theme theme{st::ui::Theme::dark()};
  st::ui::UiRoot root{};
  root.set_theme(theme);
  root.set_viewport(st::math::Size{800.0f, 400.0f});
  root.set_text_port(&port);

  auto owned = std::make_unique<Terminal>();
  Terminal* terminal = owned.get();
  terminal->set_id("term-align");
  root.set_content(std::move(owned));
  root.layout(true);
  if (!st::process::PtySession::supported()) return;
  terminal->open_shell();

  st::raster::Canvas canvas{800, 400, 1.0f};
  for (int i = 0; i < 60; ++i) {
    terminal->pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    root.layout(false);
    root.paint(canvas);
    if (!port.draws.empty()) break;
  }
  ST_REQUIRE(!port.draws.empty());

  // 光标块矩形（组件属性面：x,y,w,h）
  const std::string rect = terminal->get_property("cursor_rect").value_or("");
  ST_REQUIRE(!rect.empty());
  const std::size_t first_comma = rect.find(',');
  const std::size_t second_comma = rect.find(',', first_comma + 1);
  ST_REQUIRE(second_comma != std::string::npos);
  const float cursor_y = std::stof(rect.substr(first_comma + 1, second_comma - first_comma - 1));

  // 光标行 = 屏幕光标行；该行文字的 draw.y（同一行顶口径）
  const float cell_h = std::stof(rect.substr(rect.rfind(',') + 1));
  const float text_y = first_text_origin_y(port);
  // 文字第一行（row 0）与光标若不同行，按行高换算到同一行再比。
  const auto* screen = terminal->screen(0);
  ST_REQUIRE(screen != nullptr);
  const float line_top = terminal->output_rect().y + 4.0f;
  const float expected_cursor_y = line_top + static_cast<float>(screen->cursor_row()) * cell_h;
  ST_CHECK_NEAR(cursor_y, expected_cursor_y, 0.6f);
  // 反向护栏：光标行顶与文字行顶用的是**同一个起点**（差值为整数行）。
  const float delta = text_y - line_top;
  ST_CHECK(std::fabs(delta) < 0.6f);   // 第一行文字从 line_top 起（不多加 ascent）
  ST_CHECK(cell_h > 0.0f);
  terminal->send_stop();
}
