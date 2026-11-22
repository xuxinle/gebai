/// Markdown 多格式样张渲染：把**真实字体**下的完整样张渲染成 PNG，
/// 用来肉眼核验“各格式字号差得是不是合适”（指标之外的必经一步）。
///
/// ## 为什么要这个探针
///
/// `md_type_scale_probe` 只能回答“字号数值是多少、比值漂不漂”。它回答不了
/// “看起来还对不对”：13.3px 的代码放在 15px 正文里是“略小一号”还是“明显缩水”、
/// 行内码芯片会不会顶到上一行——这类判断只有出图能做（观感改动的硬要求）。
///
/// 它还会把**同一份内容在多个正文基准下**并排渲染，正是用户报的那件事的直观形态：
/// 旧实现里 10px 正文时代码**比正文还大**、26px 时只有一半，一眼就能看出“层级反转”。
///
/// 用法：
/// ```
/// st build md_type_scale_render --profile dev
/// build/dev/bin/md_type_scale_render            # 输出 md-type-scale-<base>.png
/// build/dev/bin/md_type_scale_render 13         # 只渲染某个基准
/// ```
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/codec/png.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::MarkdownView;
using st::ui::Theme;

/// 覆盖全部格式的样张（每种结构与其它结构相邻，便于比较相对大小）。
constexpr std::string_view kSample = R"MD(# 一级标题 H1

## 二级标题 H2

### 三级标题 H3

#### 四级标题 H4

正文里混着 `行内码 inline_code`、**加粗**、*斜体*、[链接](https://example.com) 与中文标点，
还有一行足够长的句子用来触发自动折行，方便看行距与折行后的节奏是否统一。

- 无序项一
- 无序项二
  - 嵌套项

1. 有序项一
2. 有序项二

- [x] 已完成任务
- [ ] 未完成任务

> 引用段落：它的字号应当与正文同级，只靠底色与左侧竖条区分层级。

```cpp
int main(int argc, char** argv) {
    const std::string s = "hello";   // 注释
    return argc > 1 ? 42 : 0;
}
```

| 列名 | 数量 | 说明 |
| --- | --- | --- |
| 苹果 | 3 | 短 |
| 香蕉 | 12 | 一格较长的说明文字，用来观察表格折行与行高 |

---

尾段。
)MD";

/// 渲染一份样张，返回写出的路径。
[[nodiscard]] auto render(const st::ui::TextPort& port, float base, int width, int height,
                          const std::string& path, const Theme& theme) -> bool {
  MarkdownView view;
  view.set_shrink_to_fit(false);
  view.set_base_font_size(base);
  view.set_markdown(std::string(kSample));
  view.apply_theme(theme);
  // 内边距给一圈，免得出图贴着边缘。
  view.style().padding = st::math::Insets::symmetric(24.0f, 20.0f);
  const float view_width = static_cast<float>(width);
  const st::ui::RenderContext context{theme, &port, 0.0};
  st::ui::Constraints constraints;
  constraints.max_width = view_width;
  constraints.available_width = view_width;
  constraints.max_height = static_cast<float>(height) * 4.0f;
  constraints.available_height = constraints.max_height;
  view.measure(context, constraints);
  view.arrange(context, st::math::Rect{0.0f, 0.0f, view_width,
                                       std::max(view.content_height(), 1.0f)});
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(width, height, 1.0f);
  canvas.clear(theme.colors().bg);
  view.paint(context, canvas);
  // 画布存的是**预乘**色，PNG 要非预乘——直接拷 `to_rgba8()` 会得到偏暗的图（实测坑）。
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4U);
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    const st::math::Color color = st::math::unpremultiply(pixels[index]);
    image.rgba[index * 4U + 0U] = color.r;
    image.rgba[index * 4U + 1U] = color.g;
    image.rgba[index * 4U + 2U] = color.b;
    image.rgba[index * 4U + 3U] = color.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "Markdown 多格式样张渲染：真实字体出图，核验各格式字号的观感",
      .criterion = "人工看图：层级可辨、代码与表格略小于正文但不缩水、无挤压/截断",
      .usage_args = "[base]",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("md_type_scale_render", spec, opts)) return st::probe::exit_ok;

  auto fonts = st::text::FontStack::system_default();
  if (!fonts) {
    st::log::err("无字体栈：本探针需要真实字体才能出图\n");
    return st::probe::exit_skip;
  }
  st::text::TextRenderer renderer{*fonts};
  const auto port = st::app::make_text_port(renderer);
  const Theme theme = Theme::light();

  std::vector<float> bases{11.0f, 13.0f, 15.0f, 20.0f};
  if (const auto one = opts.positional(0); one.has_value()) {
    bases.assign(1, static_cast<float>(std::atof(std::string(*one).c_str())));
  }
  st::probe::result verdict{};
  for (const float base : bases) {
    // 文件名带基准值（可区分）——固定文件名会让后跑的静默覆盖前一个（量尺坑）。
    const std::string path = std::format("md-type-scale-base{:.0f}.png", static_cast<double>(base));
    const bool ok = render(*port, base, 760, 1180, path, theme);
    st::log::out("base {:>5.1f} → {}（{}）\n", static_cast<double>(base), path,
                 ok ? "已写出" : "写盘失败");
    verdict.add(std::format("base {:.0f} 出图", static_cast<double>(base)), ok,
                ok ? path : "write_png 失败");
  }
  verdict.conclude();
  return st::probe::main_of(spec, opts, verdict);
}
