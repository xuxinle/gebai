/// **折行溢出量尺**：排版期算出来的行宽，和绘制期真实占的宽度对不上多少。
///
/// ## 这条线为什么会断
///
/// “量宽与绘制必须同一角色/同一口径”在这份代码里是**写在注释里的约定**
/// （见 `draw_spans` 的注释），但排版期违反了它两处：
///
/// 1. **角色不同**：排版 `push_spans` 一律用默认 `FontRole::Proportional` 量，
///    而绘制按 `Span::font_role()`（行内码/代码块 = Monospace）量。
///    真实字体下等宽拉丁比比例宽（`inline_code` 11 字符可差 ~20px）；
/// 2. **行内码芯片的内边距没算进排版**：绘制时 x 前进 `width + 2×pad`
///    （芯片比文字两侧各宽 pad），排版只加了 `width`。
///
/// 两者的后果是同一件事：**行尾溢出 → 绘制期 `ellipsize` 截断**，
/// 屏幕上凭空出现一个 `…`，而内容其实还有（本该折到下一行）。
///
/// ## 判据
///
/// · `slack ≤ 0`：每行的**绘制右缘**不得超过所在行盒右缘（超了 = 会截断）；
/// · `role_gap`：同一段文字在两种角色下的宽度差（量级参考，不是判据）；
/// · 截断次数必须为 0（含省略号的绘制调用）。
///
/// 用法：`st build md_wrap_overflow_probe --profile dev && build/dev/bin/md_wrap_overflow_probe`
#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::text::FontRole;
using st::ui::MarkdownView;
using st::ui::Theme;

/// 用户报的那一段（逐字复刻）：行内码 + 加粗 + 斜体 + 链接 + 长句折行。
constexpr std::string_view kSample =
    "正文里混着 `行内码 inline_code`、**加粗**、*斜体*、[链接](https://example.com) 与中文标点，"
    "还有一行足够长的句子用来触发自动折行，方便看行距与折行后的节奏是否统一。\n";

struct DrawCall {
  std::string text{};
  float x{0.0f};
  float y{0.0f};
  float size{0.0f};
  FontRole role{FontRole::Proportional};
};

class RecordingPort final : public st::ui::TextPort {
 public:
  explicit RecordingPort(const st::ui::TextPort& inner) : inner_(inner) {}

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return inner_.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                                   FontRole role = FontRole::Proportional) const
      -> float override {
    return inner_.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return inner_.line_height(size);
  }
  [[nodiscard]] auto ascent(float size) const -> float override { return inner_.ascent(size); }
  [[nodiscard]] auto descent(float size) const -> float override { return inner_.descent(size); }
  [[nodiscard]] auto shaped_ascent(std::string_view utf8, float size,
                                   FontRole role = FontRole::Proportional) const
      -> float override {
    return inner_.shaped_ascent(utf8, size, role);
  }
  [[nodiscard]] auto ink_metrics(std::string_view utf8, float size,
                                 FontRole role = FontRole::Proportional) const
      -> std::optional<InkMetrics> override {
    return inner_.ink_metrics(utf8, size, role);
  }
  [[nodiscard]] auto has_real_bold() const -> bool override { return inner_.has_real_bold(); }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color, FontRole role = FontRole::Proportional, float embolden = 0.0f,
            bool bold = false) const override {
    calls.push_back(DrawCall{std::string(utf8), origin.x, origin.y, size, role});
    inner_.draw(canvas, utf8, origin, size, color, role, embolden, bold);
  }
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
  [[nodiscard]] auto text_width(std::string_view utf8, float size, FontRole role) const -> float {
    return inner_.measure_width(utf8, size, role);
  }

  mutable std::vector<DrawCall> calls{};

 private:
  const st::ui::TextPort& inner_;
};

/// 行盒（内容坐标 y / 高 / 右缘）。
struct Box {
  float y{0.0f};
  float height{0.0f};
  float right{0.0f};
};

[[nodiscard]] auto parse_boxes(const std::string& text) -> std::vector<Box> {
  std::vector<Box> boxes;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string_view line{text.data() + start, end - start};
    const std::size_t at = line.find("box=(");
    if (at != std::string_view::npos) {
      const std::size_t close = line.find(')', at);
      if (close != std::string_view::npos) {
        const std::string nums{line.substr(at + 5, close - at - 5)};
        std::array<float, 4> values{};
        std::size_t field = 0;
        std::size_t index = 0;
        while (field < 4U) {
          const std::size_t comma = nums.find(',', index);
          const std::size_t stop = comma == std::string::npos ? nums.size() : comma;
          values[field] = std::stof(nums.substr(index, stop - index));
          ++field;
          index = stop + 1;
        }
        boxes.push_back(Box{values[1], values[3], values[0] + values[2]});
      }
    }
    start = end + 1;
  }
  return boxes;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "折行溢出量尺：绘制右缘是否越过行盒（越过就会被 ellipsize 截出 …）",
      .criterion = "每行绘制右缘 ≤ 行盒右缘；省略号截断次数为 0",
      .usage_args = "[base]",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("md_wrap_overflow_probe", spec, opts)) return st::probe::exit_ok;

  auto fonts = st::text::FontStack::system_default();
  if (!fonts) {
    st::log::err("无字体栈：本探针要真实字体（等宽与比例字宽才不同）\n");
    return st::probe::exit_skip;
  }
  st::text::TextRenderer renderer{*fonts};
  const auto port = st::app::make_text_port(renderer);
  const Theme theme = Theme::light();
  const float base = opts.positional(0).has_value()
                         ? static_cast<float>(std::atof(std::string(*opts.positional(0)).c_str()))
                         : 13.0f;
  constexpr int kWidth = 760;
  constexpr int kHeight = 600;

  RecordingPort recorder{*port};
  MarkdownView view;
  view.set_shrink_to_fit(false);
  view.set_base_font_size(base);
  view.set_markdown(std::string(kSample));
  view.apply_theme(theme);
  // **内边距必须归零**：`row_at_probe` 报的行盒是**内容坐标**，而 `draw` 的 `origin`
  // 是**绝对坐标**（已加内容原点）。两侧混着算会让“绘制右缘 vs 行盒右缘”差一个 padding，
  // 量出来的溢出/余量全是假的（本探针第一版就踩了：把 +6px 余量读成了 −17px 溢出）。
  view.style().padding = st::math::Insets::all(0.0f);
  const st::ui::RenderContext context{theme, &recorder, 0.0};
  st::ui::Constraints constraints;
  constraints.max_width = static_cast<float>(kWidth);
  constraints.available_width = static_cast<float>(kWidth);
  constraints.max_height = static_cast<float>(kHeight);
  constraints.available_height = static_cast<float>(kHeight);
  view.measure(context, constraints);
  view.arrange(context, st::math::Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                       std::max(view.content_height(), 1.0f)});
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  canvas.clear(theme.colors().bg);
  view.paint(context, canvas);

  st::probe::result verdict{};
  // 量级参考：同一段文字在两种角色下的宽度（解释“为什么只差一点也会溢出”）。
  const std::string probe_text = "inline_code";
  const float prop = recorder.text_width(probe_text, 13.3f * base / 14.0f, FontRole::Proportional);
  const float mono = recorder.text_width(probe_text, 13.3f * base / 14.0f, FontRole::Monospace);
  st::log::out("角色宽度：`{}` 比例 {:.2f} / 等宽 {:.2f}（差 {:+.2f}px）\n", probe_text,
               static_cast<double>(prop), static_cast<double>(mono),
               static_cast<double>(mono - prop));

  const std::vector<Box> boxes = parse_boxes(view.get_property("row_at_probe").value_or(""));
  st::log::out("\n逐条绘制：右缘 vs 行盒右缘\n");
  // 哨兵用**极大值**（而不是 −1e9）：一个值都没量到时不能报成“巨大溢出”，
  // 那是把“没测”当成“最差”（本仓反复踩的坑）。
  float worst = 1e9f;
  std::size_t measured = 0;
  std::size_t truncated = 0;
  for (const DrawCall& call : recorder.calls) {
    const float right = call.x + recorder.text_width(call.text, call.size, call.role);
    const Box* owner = nullptr;
    for (const Box& box : boxes) {
      if (call.y >= box.y - 0.5f && call.y < box.y + box.height + 0.5f) {
        owner = &box;
        break;
      }
    }
    if (owner == nullptr) continue;
    const float slack = owner->right - right;
    worst = std::min(worst, slack);
    ++measured;
    const bool ellipsized = call.text.find("…") != std::string::npos;
    if (ellipsized) ++truncated;
    st::log::out("  y={:>6.1f} {:<8} x={:>7.2f} w={:>6.2f} right={:>7.2f} box_right={:>7.2f} "
                 "slack={:>+7.2f}{}{}  text=[{}]\n",
                 static_cast<double>(call.y),
                 call.role == FontRole::Monospace ? "mono" : "prop",
                 static_cast<double>(call.x),
                 static_cast<double>(right - call.x), static_cast<double>(right),
                 static_cast<double>(owner->right), static_cast<double>(slack),
                 slack < 0.0f ? "  ← 溢出" : "", ellipsized ? "  ← 被截断" : "", call.text);
  }
  st::log::out("\n最小 slack = {:+.2f}px（量了 {} 条绘制）；截断 {} 次\n",
               static_cast<double>(worst), measured, truncated);
  verdict.add("量尺自己量到了东西", measured >= 5U, std::format("{} 条绘制", measured));
  verdict.add("绘制不越出行盒", measured > 0U && worst >= -0.5f,
              std::format("最小 slack {:+.2f}px", static_cast<double>(worst)));
  verdict.add("未发生省略号截断", truncated == 0U, std::format("{} 次", truncated));

  verdict.conclude();
  for (const auto& check : verdict.checks) {
    if (!check.passed) st::log::err("FAIL {}: {}\n", check.name, check.detail);
  }
  return st::probe::main_of(spec, opts, verdict);
}
