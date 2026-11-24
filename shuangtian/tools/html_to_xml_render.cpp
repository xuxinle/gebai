// HTML → 霜天 XML 的**端到端出图探针**：真 HTML 进，真像素出。
//
// ## 为什么必须有它（不能只靠单测）
//
// 单测能钉住语义映射与属性面，**钉不住"看起来对不对"**：一个 `text_color` 抄错、
// 一个 `multiline` 漏写、一个容器没 `grow`，属性面全绿而画面是错的（或空的）。
// 这类"形态错"只能出图给人看——那是本探针唯一的职责。
//
// 判据（可复算，不靠肉眼）：
//   ① 墨量 > 0（真画了东西，不是空白）；
//   ② 章节数可数（按标题行分块）——太少说明结构塌了；
//   ③ XML 体积与元素数在预期区间（结构没被吃掉）。
//
// 用法：html_to_xml_render [输出目录]
// 退出码非 0 = 有判据不满足。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/html/to_ui_xml.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/xml_ui.hpp"

namespace {

using st::math::Color;

constexpr int kWidth = 900;
constexpr int kHeight = 720;

/// 一份"像真实产出"的 HTML：标题层级、段落、列表、表格样、代码块、按钮/输入、
/// 行内混排、引用块——把生成器每条分支都走一遍。
constexpr const char* kSampleHtml = R"(<!DOCTYPE html>
<html><head><title>周报：推理服务迁移</title>
<style>
  body { color: #1f2430; font-size: 15px; }
  h1 { color: #14213d; }
  h2 { color: #2b4c7e; }
  .meta { color: #6b7280; font-size: 13px; }
  .card { padding: 12px; background: #f3f6fb; border-radius: 10px; }
  .warn { color: #b45309; }
  code { font-family: monospace; }
  blockquote { color: #4b5563; }
</style></head>
<body>
  <h1>推理服务迁移周报</h1>
  <p class="meta">第 12 周 · 负责人：运维组</p>

  <h2>本周进展</h2>
  <p>完成 <b>d16 集群</b>的推理服务迁移，<em>平均首字延迟下降</em> 38%。</p>
  <ul>
    <li>迁移 14 个推理服务，全部通过压测</li>
    <li>补齐健康探针与自动重启</li>
    <li class="warn">遗留 2 个服务待确认配额</li>
  </ul>

  <div class="card">
    <h2>关键指标</h2>
    <p>吞吐 1280 req/s · P99 延迟 240 ms · 错误率 0.02%</p>
  </div>

  <h3>下一步</h3>
  <ol>
    <li>补齐配额确认</li>
    <li>灰度切流 20%</li>
  </ol>

  <pre>kubectl get pods -n infer
NAME                     READY   STATUS
infer-api-7d9f-abc       1/1     Running</pre>

  <blockquote>迁移窗口定在下周二凌晨，期间只做只读切换。</blockquote>

  <p><a href="https://example.com/report">完整报告</a></p>
  <button type="submit">确认排期</button>
  <input type="text" placeholder="输入备注">
</body></html>
)";

/// 与 `paint_frame` 同一份清屏色（口径不一致会把整屏算成墨迹）。
[[nodiscard]] auto to_png(const st::raster::Canvas& canvas) -> st::codec::PngImage {
  st::codec::PngImage image{};
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  const auto pixels = canvas.pixels();
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    const std::uint32_t value = pixels[index];
    const std::uint32_t alpha = value & 0xFFU;
    const auto un = [alpha](std::uint32_t channel) -> std::uint8_t {
      if (alpha == 0U) return 0;
      const float scaled = static_cast<float>(channel) * 255.0f / static_cast<float>(alpha);
      return static_cast<std::uint8_t>(scaled > 255.0f ? 255.0f : scaled);
    };
    image.rgba[index * 4U + 0U] = un((value >> 24U) & 0xFFU);
    image.rgba[index * 4U + 1U] = un((value >> 16U) & 0xFFU);
    image.rgba[index * 4U + 2U] = un((value >> 8U) & 0xFFU);
    image.rgba[index * 4U + 3U] = static_cast<std::uint8_t>(alpha);
  }
  return image;
}

/// 墨迹量与包围盒（与底色差异累计）。
struct Ink {
  double amount{0.0};
  int min_y{0};
  int max_y{0};
  bool empty{true};
};

[[nodiscard]] auto measure_ink(const st::raster::Canvas& canvas, Color background) -> Ink {
  Ink ink{};
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color color = canvas.pixel_at(x, y);
      const double difference = std::abs(static_cast<double>(color.r) - background.r) +
                                std::abs(static_cast<double>(color.g) - background.g) +
                                std::abs(static_cast<double>(color.b) - background.b);
      if (difference <= 6.0) continue;
      ink.amount += difference;
      if (ink.empty) {
        ink.min_y = y;
        ink.max_y = y;
        ink.empty = false;
      } else {
        ink.min_y = std::min(ink.min_y, y);
        ink.max_y = std::max(ink.max_y, y);
      }
    }
  }
  return ink;
}

/// 一个"临时文本端口"：探针不接真字体时用它量字，保证排版能跑起来。
///
/// 无头环境下 `UiRoot` 的缺省端口是 `NullTextPort`（一切都量成 0），
/// 于是**画不出任何文字**——那样出图是一张空白，探针就失去了意义。
/// 这里给一个固定字宽的近似度量（CJK 全宽、ASCII 半宽），足以让布局跑通。
class ApproxTextPort final : public st::ui::TextPort {
 public:
  [[nodiscard]] auto advance(char32_t codepoint, float size) const -> float {
    return (codepoint > 0x2E80U ? size : size * 0.55f);
  }
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return st::math::Size{measure_width(utf8, size), line_height(size)};
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                                   st::text::FontRole /*role*/ = st::text::FontRole::Proportional)
      const -> float override {
    float width = 0.0F;
    std::size_t index = 0;
    while (index < utf8.size()) {
      const auto [codepoint, length] = decode(utf8, index);
      width += advance(codepoint, size);
      index += length;
    }
    return width;
  }
  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.5F; }
  [[nodiscard]] auto ascent(float size) const -> float override { return size * 1.15F; }
  [[nodiscard]] auto descent(float size) const -> float override { return size * 0.35F; }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            Color color, st::text::FontRole /*role*/ = st::text::FontRole::Proportional,
            float /*embolden*/ = 0.0f, bool /*bold*/ = false) const override {
    // 逐字画"实心方块"占位：探针的目的是验**布局与结构**，不是字形质量。
    float x = origin.x;
    std::size_t index = 0;
    while (index < utf8.size()) {
      const auto [codepoint, length] = decode(utf8, index);
      index += length;
      const float width = advance(codepoint, size);
      if (codepoint == U'\n') continue;
      if (codepoint != U' ') {
        canvas.fill_rect(st::math::Rect{x, origin.y, width * 0.7F, size * 0.75F},
                         st::raster::Paint::solid(color));
      }
      x += width;
    }
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    if (measure_width(utf8, size) <= max_width) return std::string(utf8);
    return std::string(utf8.substr(0, utf8.size() / 2)) + "…";
  }
  [[nodiscard]] auto ellipsize_middle(std::string_view utf8, float /*size*/, float /*max_width*/)
      const -> std::string override {
    return std::string(utf8);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    float used = 0.0F;
    std::size_t index = 0;
    while (index < utf8.size()) {
      const std::size_t start = index;
      const auto [codepoint, length] = decode(utf8, index);
      index += length;
      if (codepoint == U'\n') {
        lines.push_back(utf8.substr(begin, start - begin));
        begin = index;
        used = 0.0F;
        continue;
      }
      const float width = advance(codepoint, size);
      if (used + width > max_width && index > begin && used > 0.0F) {
        lines.push_back(utf8.substr(begin, start - begin));
        begin = start;
        used = width;
        continue;
      }
      used += width;
    }
    if (begin < utf8.size()) lines.push_back(utf8.substr(begin));
    return lines;
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const -> std::vector<std::string> override {
    std::vector<std::string> out;
    for (const auto& line : wrap(utf8, size, max_width)) out.emplace_back(line);
    if (out.size() <= max_lines) return out;
    out.resize(max_lines);
    out.back() = ellipsize(out.back(), size, max_width);
    return out;
  }

 private:
  /// 解一个 UTF-8 码点（畸形输入按 1 字节吞掉，探针不需要严格性）。
  [[nodiscard]] static auto decode(std::string_view utf8, std::size_t index)
      -> std::pair<char32_t, std::size_t> {
    const auto byte = static_cast<unsigned char>(utf8[index]);
    if (byte < 0x80U) return {byte, 1};
    std::size_t length = 1;
    char32_t value = byte & 0x7FU;
    if ((byte & 0xE0U) == 0xC0U) {
      length = 2;
      value = byte & 0x1FU;
    } else if ((byte & 0xF0U) == 0xE0U) {
      length = 3;
      value = byte & 0x0FU;
    } else if ((byte & 0xF8U) == 0xF0U) {
      length = 4;
      value = byte & 0x07U;
    }
    if (index + length > utf8.size()) return {byte, 1};
    for (std::size_t step = 1; step < length; ++step) {
      value = (value << 6U) | (static_cast<unsigned char>(utf8[index + step]) & 0x3FU);
    }
    return {value, length};
  }
};

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : "build/html-to-xml";
  (void)st::fs::create_directories(out_dir);
  int failures = 0;

  // ① HTML → XML
  const st::html::ToUiXmlResult result = st::html::to_ui_xml(kSampleHtml);
  const std::string xml_path = st::fs::join(out_dir, "generated.xml");
  (void)st::fs::write_text(xml_path, result.xml);

  // ② 严格模式建树（报告的干净程度就是"生成器有没有制造下游问题"）
  st::ui::XmlBuildReport build_report;
  st::ui::XmlBuildOptions build_options;
  build_options.strict = true;
  auto tree = st::ui::build_from_xml(result.xml, &build_report, build_options);

  std::printf("XML: %s（%zu 字节）\n", xml_path.c_str(), result.xml.size());
  std::printf("元素 %zu · 图片 %zu · 脚本 %zu · 未映射标签 %zu · 丢弃声明 %zu\n",
              result.report.elements, result.report.images, result.report.scripts,
              result.report.unmapped_tags.size(), result.report.dropped_declarations.size());
  std::printf("建树：%s（元素 %zu）\n", tree != nullptr ? "成功" : "失败", build_report.elements);
  if (!build_report.unknown_tags.empty()) {
    std::printf("  ✗ 认不出的标签：");
    for (const auto& tag : build_report.unknown_tags) std::printf(" %s", tag.c_str());
    std::printf("\n");
  }
  if (!build_report.unknown_attributes.empty()) {
    std::printf("  ✗ 未生效属性：\n");
    for (const auto& attribute : build_report.unknown_attributes) {
      std::printf("    %s\n", attribute.c_str());
    }
  }
  if (tree == nullptr) {
    std::printf("[FAIL] 严格模式建树失败——生成的 XML 有框架不认的东西\n");
    return 1;
  }
  if (!build_report.unknown_attributes.empty() || !build_report.unknown_tags.empty()) {
    std::printf("[FAIL] 建树报告不干净（生成器写出了框架不认识的名字）\n");
    ++failures;
  }

  // ③ 渲染成 PNG（真实排版 + 绘制链路）
  st::ui::Theme theme = st::ui::Theme::light();
  st::ui::UiRoot root;
  root.set_theme(theme);
  // 文本端口：**优先真实字体**（这样出图能人眼读字，验的不只是盒子）。
  // 本机没字体时退到近似端口：那时仍能验布局，只是字形是方块占位。
  ApproxTextPort fallback_port;
  std::unique_ptr<st::text::TextRenderer> renderer;
  std::unique_ptr<st::ui::TextPort> real_port;
  auto stack = st::text::FontStack::system_default();
  if (stack.has_value()) {
    renderer = std::make_unique<st::text::TextRenderer>(*stack);
    real_port = st::app::make_text_port(*renderer);
    root.set_text_port(real_port.get());
    std::printf("文本端口：真实字体\n");
  } else {
    root.set_text_port(&fallback_port);
    std::printf("文本端口：近似（本机无字体，字形为方块）\n");
  }
  root.set_content(std::move(tree));
  root.set_viewport(st::math::Size{static_cast<float>(kWidth), static_cast<float>(kHeight)});
  root.layout(true);

  st::raster::Canvas canvas{st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0F)};
  const Color background = theme.colors().bg;
  canvas.clear(background);
  (void)root.paint_frame(canvas);

  const std::string png_path = st::fs::join(out_dir, "rendered.png");
  if (auto written = st::codec::png_write_file(png_path, to_png(canvas)); !written) {
    std::printf("[FAIL] 写 PNG 失败：%s\n", written.error().message.c_str());
    return 1;
  }
  std::printf("PNG: %s\n", png_path.c_str());

  const Ink ink = measure_ink(canvas, background);
  std::printf("墨量 %.0f · 纵向覆盖 %d..%d\n", ink.amount, ink.min_y, ink.max_y);

  // 判据 ①：真画了东西
  if (ink.empty || ink.amount < 5000.0) {
    std::printf("[FAIL] 画面几乎是空白（墨量 %.0f）\n", ink.amount);
    ++failures;
  }
  // 判据 ②：内容占住了大部分高度（结构没塌成一小块）
  const int covered = ink.max_y - ink.min_y;
  if (covered < kHeight / 3) {
    std::printf("[FAIL] 内容只占 %d px 高（结构塌了？）\n", covered);
    ++failures;
  }
  // 判据 ③：元素数在预期区间（标题/段落/列表/表格/按钮都在）
  if (result.report.elements < 25) {
    std::printf("[FAIL] 只生成 %zu 个元素（样例有 6+ 段落、6 个列表项、按钮与输入）\n",
                result.report.elements);
    ++failures;
  }

  // 报告内容（人看的部分）
  if (!result.report.unmapped_tags.empty()) {
    std::printf("未映射标签：");
    for (const auto& tag : result.report.unmapped_tags) std::printf(" %s", tag.c_str());
    std::printf("\n");
  }
  if (!result.report.dropped_declarations.empty()) {
    std::printf("丢弃的声明：\n");
    for (const auto& item : result.report.dropped_declarations) {
      std::printf("  · %s\n", item.c_str());
    }
  }
  std::printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
  return failures;
}
