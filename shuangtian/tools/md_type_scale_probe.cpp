/// **Markdown 各格式字号量尺**：同一份样张里每类结构（正文/标题/行内码/代码块/
/// 表格/列表符号/引用/语言标签）**实际用多少 px 绘制**，以及它们随正文基准
/// （`set_base_font_size`）怎么变。
///
/// ## 为什么需要它
///
/// “markdown 各种格式的字体大小差距过大”这句话不是可判定的量。可判定的等价形式是：
/// 把样张里每类结构的**绘制字号**抓出来，看
///
/// ① **绝对差**：最大与最小之间差几个 px（用户感知的“跳”）；
/// ② **比值**：各格式 / 正文基准（决定它是不是**跟着调用方走**）；
/// ③ **随基准的单调性**：正文从 11 变到 20 时，各格式是否同向变化
///    （只有正文跟着动、其余钉死 ⇒ 小字号下代码/表格反而**比正文大**）。
///
/// 旧实现只让正文与标题随基准缩放，行内码/代码块取主题绝对档（14）、表格取 12、
/// 语言标签写死 11——于是同一个组件在 11px 与 20px 两种正文下呈现出**完全不同**的
/// 层级关系（11 正文/14 代码；20 正文/12 表格）。
///
/// ## 判据（本探针自检，退出码非 0 = 不合格）
///
/// · `ratio`  ∈ [0.70, 1.15]：任何格式与正文的比值不许越界
///   （代码/表格略小是正常的，**大于正文**不正常）；
/// · `drift`  ≤ 0.06：同一格式在**不同基准**下与正文的比值漂移不许超过 6 个百分点
///   （这就是“差距随字号变化”的量化形式）；
/// · 单调：每个格式的字号都必须随基准**严格增大**。
///
/// 用法：`st build md_type_scale_probe --profile dev && build/dev/bin/md_type_scale_probe`
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
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

/// 样张：每一类结构都带一个**全局唯一**的标记词，便于从绘制记录里认领字号。
constexpr std::string_view kSample = R"MD(# H1标题

###### H6标题

正文样本

行内样本`行内码`结束

- 无序项

1. 有序项

> 引用项

```cpp
代码行
```

| 表头列 | 第二列 |
| --- | --- |
| 单元列 | 3 |
)MD";

/// 第二份样张：**标题里含行内码**——看行内码是否按“所在块的字号”派生。
///
/// 为何单独一份：字号若对行内码取全局常量，标题里的 `code` 就会掉回正文字号，
/// 于是标题内部出现一条“字号断裂”（标题 1.35×，里面那两个字 0.93×）。
/// 两行的码用不同标记词，否则认领时只拿得到第一处。
constexpr std::string_view kHeadingInlineSample = R"MD(# 大标题`码甲`

###### 末级标题`码乙`
)MD";

/// 一次 `draw` 的记录（**只记字号**，不看像素——本探针量的是排版口径而非字形质量）。
struct DrawCall {
  std::string text{};
  float size{0.0f};
  float y{0.0f};
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
    calls.push_back(DrawCall{std::string(utf8), size, origin.y, role});
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

  mutable std::vector<DrawCall> calls{};

 private:
  const st::ui::TextPort& inner_;
};

/// 一个基准下量到的整套字号。
struct Row {
  float base{0.0f};
  float body{0.0f};
  std::array<float, 2> heading{};
  /// 标题**内部**行内码的字号（H1 / H6）。必须与标题同量级——
  /// 否则标题里出现“字号断裂”（大字里嵌小字）。
  std::array<float, 2> heading_inline{};
  float inline_code{0.0f};
  float code{0.0f};
  float table{0.0f};
  float language{0.0f};
  float list_marker{0.0f};
  float quote{0.0f};
  /// 本基准下**字号大于所在行行高**的绘制次数（挤行/行间重叠的直接判据）。
  std::size_t size_over_row_height{0};
  /// 其中最大的“字号 / 行高”比（1 以上 = 字形比行盒还高）。
  float worst_size_over_row_height{0.0f};
};

/// 从绘制记录里按标记词认领字号（`needle` 必须唯一命中）。
[[nodiscard]] auto size_of(const std::vector<DrawCall>& calls, std::string_view needle)
    -> std::optional<float> {
  for (const DrawCall& call : calls) {
    if (call.text.find(needle) != std::string::npos) return call.size;
  }
  return std::nullopt;
}

/// 行盒高度清单（内容坐标 y + 高）：从 `row_at_probe` 里解析。
///
/// 为何借属性面而不另建一套：`row_at_probe` 就是组件自报的排版结果，
/// 探针另算一套就会与应用看到的不一致（“探针与应用不共享同一份实现”）。
struct Box {
  float y{0.0f};
  float height{0.0f};
};

[[nodiscard]] auto parse_boxes(const std::string& probe_text) -> std::vector<Box> {
  std::vector<Box> boxes;
  std::size_t start = 0;
  while (start < probe_text.size()) {
    std::size_t end = probe_text.find('\n', start);
    if (end == std::string::npos) end = probe_text.size();
    const std::string_view line{probe_text.data() + start, end - start};
    // 形如 `3|k=0 box=(0,25,868,16) clip=(...) tx=0 sp=1 ...`
    const std::size_t box_at = line.find("box=(");
    if (box_at != std::string_view::npos) {
      const std::size_t close = line.find(')', box_at);
      if (close != std::string_view::npos) {
        std::string_view nums = line.substr(box_at + 5, close - box_at - 5);
        std::array<float, 4> values{};
        std::size_t field = 0;
        std::size_t index = 0;
        while (field < 4U && index <= nums.size()) {
          const std::size_t comma = nums.find(',', index);
          const std::size_t stop = comma == std::string_view::npos ? nums.size() : comma;
          const std::string_view token = nums.substr(index, stop - index);
          values[field] = static_cast<float>(std::atof(std::string(token).c_str()));
          ++field;
          index = stop + 1;
        }
        boxes.push_back(Box{values[1], values[3]});
      }
    }
    start = end + 1;
  }
  return boxes;
}

[[nodiscard]] auto measure_with(const st::ui::TextPort& port, const Theme& theme, float base)
    -> Row {
  RecordingPort recorder{port};
  MarkdownView view;
  // 关闭按内容收缩：本探针只要字号口径，宽度固定更省排除项。
  view.set_shrink_to_fit(false);
  view.set_base_font_size(base);
  view.set_markdown(std::string(kSample));
  view.apply_theme(theme);
  const st::ui::RenderContext context{theme, &recorder, 0.0};
  st::ui::Constraints constraints;
  constraints.max_width = 900.0f;
  constraints.max_height = 4000.0f;
  constraints.available_width = 900.0f;
  constraints.available_height = 4000.0f;
  view.measure(context, constraints);
  view.arrange(context, st::math::Rect{0.0f, 0.0f, 900.0f, 4000.0f});
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(900, 4000, 1.0f);
  view.paint(context, canvas);

  Row row;
  row.base = base;
  row.body = size_of(recorder.calls, "正文样本").value_or(0.0f);
  row.heading[0] = size_of(recorder.calls, "H1标题").value_or(0.0f);
  row.heading[1] = size_of(recorder.calls, "H6标题").value_or(0.0f);
  row.inline_code = size_of(recorder.calls, "行内码").value_or(0.0f);
  row.code = size_of(recorder.calls, "代码行").value_or(0.0f);
  row.table = size_of(recorder.calls, "表头列").value_or(0.0f);
  row.language = size_of(recorder.calls, "cpp").value_or(0.0f);
  row.list_marker = size_of(recorder.calls, "无序项").value_or(0.0f);
  row.quote = size_of(recorder.calls, "引用项").value_or(0.0f);

  // 字号 vs 行盒高：向上取最近的行盒（绘制的 `origin.y` 落在它里面）。
  // 判据只抓一件事——**字比行高**（逐行叠字、行间粘连的直接原因）；
  // 行高比字高多少是观感取舍，不在本探针管。
  const std::vector<Box> boxes =
      parse_boxes(view.get_property("row_at_probe").value_or(std::string{}));
  for (const DrawCall& call : recorder.calls) {
    const Box* owner = nullptr;
    for (const Box& box : boxes) {
      if (call.y >= box.y - 0.5f && call.y < box.y + box.height + 0.5f) {
        owner = &box;
        break;
      }
    }
    if (owner == nullptr || owner->height <= 0.0f) continue;
    const float over = call.size / owner->height;
    if (over > 1.0f) {
      ++row.size_over_row_height;
      row.worst_size_over_row_height = std::max(row.worst_size_over_row_height, over);
    }
  }

  // 标题里的行内码（另一份样张：同一视图的两个标题行）。
  {
    RecordingPort heading_port{port};
    MarkdownView heading_view;
    heading_view.set_shrink_to_fit(false);
    heading_view.set_base_font_size(base);
    heading_view.set_markdown(std::string(kHeadingInlineSample));
    heading_view.apply_theme(theme);
    const st::ui::RenderContext heading_context{theme, &heading_port, 0.0};
    heading_view.measure(heading_context, constraints);
    heading_view.arrange(heading_context, st::math::Rect{0.0f, 0.0f, 900.0f, 4000.0f});
    st::raster::Canvas heading_canvas = st::raster::Canvas::for_logical_size(900, 4000, 1.0f);
    heading_view.paint(heading_context, heading_canvas);
    row.heading_inline[0] = size_of(heading_port.calls, "码甲").value_or(0.0f);
    row.heading_inline[1] = size_of(heading_port.calls, "码乙").value_or(0.0f);
  }
  return row;
}

[[nodiscard]] auto ratio(float value, float base) -> float {
  return base > 0.0f ? value / base : 0.0f;
}

/// 量“同一 px 下等宽与比例字体的墨迹高度差”。
///
/// 为何要它：代码字号该不该比正文小，取决于**同号时两者看起来谁大**。
/// 这个量只能从字体自己报（`ink_metrics`）拿：等宽面与比例面是不同 face，
/// x-height / cap 高未必一致。若等宽墨迹更高，那“同号更显大”就有实测依据，
/// 代码字号就该略小于正文；反之两边同号才对。
void report_role_ink(const st::ui::TextPort& port, float size) {
  // 同时量中文与拉丁：CJK 的墨迹高与拉丁不是一个量级，混着量会互相抵消。
  constexpr std::string_view kHan = "\xE6\xA0\x87\xE9\xA2\x98\xE6\xAD\xA3\xE6\x96\x87";  // 标题正文
  constexpr std::string_view kLatin = "Hamburgefonts";
  for (const auto& [label, text] : {std::pair{"han", kHan}, std::pair{"latin", kLatin}}) {
    const auto prop = port.ink_metrics(text, size, FontRole::Proportional);
    const auto mono = port.ink_metrics(text, size, FontRole::Monospace);
    if (!prop || !mono) {
      st::log::out("  {} ${:.1f}px：端口报不出墨迹量\n", label, static_cast<double>(size));
      continue;
    }
    const double prop_h = static_cast<double>(prop->above + prop->below);
    const double mono_h = static_cast<double>(mono->above + mono->below);
    st::log::out("  {:<5} {:.1f}px：比例 {:.2f} / 等宽 {:.2f}（等宽/比例 = {:.3f}）\n", label,
                 static_cast<double>(size), prop_h, mono_h,
                 prop_h > 0.0 ? mono_h / prop_h : 0.0);
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "Markdown 各格式字号量尺：每类结构的绘制字号与「各格式/正文」比值",
      .criterion = "比值 ∈ [0.70,1.15]、跨基准漂移 ≤ 0.06、且逐格式随基准严格增大",
      .usage_args = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("md_type_scale_probe", spec, opts)) return st::probe::exit_ok;

  auto fonts = st::text::FontStack::system_default();
  if (!fonts) {
    st::log::err("无字体栈：本探针需要一个真实字体（量的是排版口径里的字号）\n");
    return st::probe::exit_skip;
  }
  st::text::TextRenderer renderer{*fonts};
  const auto port = st::app::make_text_port(renderer);
  const Theme theme = Theme::light();

  // 覆盖真实调用方用到的基准：工具卡 10/11、思考块 11/12、气泡 13、界面正文 14/15、
  // 阅读模式 20（`apps/xiaobai` 与 `apps/gbcode` 的实际取值都在其中）。
  constexpr std::array<float, 7> kBases{10.0f, 11.0f, 12.0f, 13.0f, 15.0f, 20.0f, 26.0f};

  st::probe::result verdict{};
  st::log::out("等宽 vs 比例：同 px 下的墨迹高度对照（决定代码字号该大还是该小）：\n");
  for (const float size : {11.0f, 13.0f, 16.0f, 20.0f}) report_role_ink(*port, size);
  st::log::out("\n");

  st::log::out("{:>5} {:>7} {:>7} {:>7} {:>7} {:>7} {:>7} {:>7} {:>7} {:>7}\n", "base", "body",
               "H1", "H6", "inline", "code", "table", "lang", "list", "quote");

  std::vector<Row> rows;
  rows.reserve(kBases.size());
  for (const float base : kBases) {
    const Row row = measure_with(*port, theme, base);
    rows.push_back(row);
    st::log::out("{:>5.1f} {:>7.2f} {:>7.2f} {:>7.2f} {:>7.2f} {:>7.2f} {:>7.2f} {:>7.2f} "
                 "{:>7.2f} {:>7.2f}\n",
                 static_cast<double>(row.base), static_cast<double>(row.body),
                 static_cast<double>(row.heading[0]), static_cast<double>(row.heading[1]),
                 static_cast<double>(row.inline_code), static_cast<double>(row.code),
                 static_cast<double>(row.table), static_cast<double>(row.language),
                 static_cast<double>(row.list_marker), static_cast<double>(row.quote));
  }

  st::log::out("\n各格式 / 正文 比值（同一列跨行漂移 = “差距随字号变化”的量）：\n");
  st::log::out("{:>5} {:>7} {:>7} {:>7} {:>7} {:>7} {:>7} {:>7} {:>7}\n", "base", "H1", "H6",
               "inline", "code", "table", "lang", "list", "quote");
  // 每类格式的比值区间（用于判漂移）。
  struct Track {
    const char* name;
    float (*pick)(const Row&);
  };
  const std::array<Track, 7> tracks{{
      {"H1", [](const Row& r) { return r.heading[0]; }},
      {"H6", [](const Row& r) { return r.heading[1]; }},
      {"inline", [](const Row& r) { return r.inline_code; }},
      {"code", [](const Row& r) { return r.code; }},
      {"table", [](const Row& r) { return r.table; }},
      {"lang", [](const Row& r) { return r.language; }},
      {"quote", [](const Row& r) { return r.quote; }},
  }};
  std::array<float, 7> lo{};
  std::array<float, 7> hi{};
  for (std::size_t track = 0; track < tracks.size(); ++track) {
    lo[track] = 1e9f;
    hi[track] = -1e9f;
    float previous = 0.0f;
    bool monotonic = true;
    for (const Row& row : rows) {
      const float value = ratio(tracks[track].pick(row), row.base);
      lo[track] = std::min(lo[track], value);
      hi[track] = std::max(hi[track], value);
      const float absolute = tracks[track].pick(row);
      if (absolute <= previous) monotonic = false;
      previous = absolute;
    }
    verdict.add(std::format("{} 随基准增大", tracks[track].name), monotonic,
                monotonic ? "单调不减" : "存在非增档位");
    const float drift = hi[track] - lo[track];
    verdict.add(std::format("{} 比值漂移", tracks[track].name), drift <= 0.06f,
                std::format("ratio {:.3f}..{:.3f}（漂移 {:.3f}）", static_cast<double>(lo[track]),
                            static_cast<double>(hi[track]), static_cast<double>(drift)));
  }
  for (const Row& row : rows) {
    st::log::out("{:>5.1f}", static_cast<double>(row.base));
    for (const Track& track : tracks) {
      st::log::out(" {:>7.3f}", static_cast<double>(ratio(track.pick(row), row.base)));
    }
    st::log::out("\n");
  }

  // 绝对跨度：同一基准下“最大格式 / 最小格式”的字号差（用户看到的“跳”）。
  //
  // 上限取 `base × 0.60`：当前方案（H1 1.35 / 语言标签 0.80）的跨度恒为 0.55×base，
  // 留 0.05 余量是为了不把浮点相等判成失败（实测会在 7.15 vs 7.15 上报红），
  // 同时仍能拦住“再次引入固定绝对字号”（那会让跨度随 base 反向变大）。
  st::log::out("\n同一基准下的绝对跨度（最大格式 − 最小格式，px）：\n");
  for (const Row& row : rows) {
    const std::array<float, 9> values{row.heading[0], row.heading[1], row.inline_code, row.code,
                                      row.table,     row.language,   row.body,       row.list_marker,
                                      row.quote};
    const auto [min_it, max_it] = std::minmax_element(values.begin(), values.end());
    st::log::out("  base {:>5.1f}: {:.2f} .. {:.2f}（差 {:.2f}，上限 {:.2f}）\n",
                 static_cast<double>(row.base), static_cast<double>(*min_it),
                 static_cast<double>(*max_it), static_cast<double>(*max_it - *min_it),
                 static_cast<double>(row.base * 0.60f));
    verdict.add(std::format("base {:.0f} 绝对跨度", static_cast<double>(row.base)),
                (*max_it - *min_it) <= row.base * 0.60f,
                std::format("差 {:.2f}px（上限 {:.2f}）", static_cast<double>(*max_it - *min_it),
                            static_cast<double>(row.base * 0.60f)));
  }

  // 标题内部的行内码：必须与所在标题**同量级**（不然标题里嵌了一行小字）。
  st::log::out("\n标题内行内码 / 标题 字号比（<1 就是“大字里嵌小字”）：\n");
  for (const Row& row : rows) {
    const float h1 = ratio(row.heading_inline[0], row.heading[0]);
    const float h6 = ratio(row.heading_inline[1], row.heading[1]);
    st::log::out("  base {:>5.1f}: H1内码 {:.2f}/{:.2f}={:.3f}  H6内码 {:.2f}/{:.2f}={:.3f}\n",
                 static_cast<double>(row.base), static_cast<double>(row.heading_inline[0]),
                 static_cast<double>(row.heading[0]), static_cast<double>(h1),
                 static_cast<double>(row.heading_inline[1]),
                 static_cast<double>(row.heading[1]), static_cast<double>(h6));
    verdict.add(std::format("base {:.0f} 标题内码不低于标题", static_cast<double>(row.base)),
                h1 >= 0.82f && h6 >= 0.82f,
                std::format("H1 {:.3f} / H6 {:.3f}（下限 0.82）", static_cast<double>(h1),
                            static_cast<double>(h6)));
  }

  // 字号 vs 行盒高：字比行高就是“挤在一起”的直接证据。
  st::log::out("\n字号 > 行盒高 的绘制次数（挤行判据）：\n");
  for (const Row& row : rows) {
    st::log::out("  base {:>5.1f}: {} 次（最大比 {:.3f}）\n", static_cast<double>(row.base),
                 row.size_over_row_height, static_cast<double>(row.worst_size_over_row_height));
    verdict.add(std::format("base {:.0f} 字不高于行盒", static_cast<double>(row.base)),
                row.size_over_row_height == 0U,
                std::format("{} 次，最大比 {:.3f}", row.size_over_row_height,
                            static_cast<double>(row.worst_size_over_row_height)));
  }

  verdict.conclude();
  for (const auto& check : verdict.checks) {
    if (!check.passed) st::log::err("FAIL {}: {}\n", check.name, check.detail);
  }
  return st::probe::main_of(spec, opts, verdict);
}
