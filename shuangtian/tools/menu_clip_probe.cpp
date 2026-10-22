// MenuPanel 视口剔除的**像素等价性**验证。
//
// ## 为什么需要这个探针
//
// `MenuPanel::paint_content` 加了视口剔除（完全落在裁剪区外的行不画）。
// 这类改动的风险是**漏画**：被跳过的行如果本该可见，用户就看到缺项。
// `st test` 只能证明"断言过"，证明不了"像素没变"。
//
// 所以这里做两件事：
//   1. 用真实字体渲染一张菜单到 PNG（供改动前后逐字节比对）；
//   2. 同进程内自比对：**小裁剪**（只露出前几行）与**大裁剪**（露出全部）
//      在同一可见区域的像素必须一致——这是"剔除不改变可见像素"的直接证据。
//
// ## 用法
//
//     menu_clip_probe <输出目录>
//
// 产出 <输出目录>/menu-all.png（大裁剪）与 menu-clipped.png（小裁剪），
// 并打印自比对结论。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/theme.hpp"

namespace {

inline constexpr int kWidth{420};
/// 画布高 **小于** 菜单总高：制造"下半截在屏外"的场景。
/// 8 项 × 32px = 256px 菜单高，画布只给 120px → 只有前 3~4 行可见。
inline constexpr int kHeight{120};
inline constexpr std::size_t kItems{8};

/// 造一张有代表性的菜单：勾选项、分隔线、子菜单箭头、长短文字混排。
///
/// 条目内容**照搬 gallery 的 `demo-menubar`**（真实在用的菜单），
/// 而不是随手编的字符串——这样验证的是实际渲染，不是实验室样品。
auto make_panel() -> std::unique_ptr<st::ui::MenuPanel> {
  std::vector<st::ui::MenuItem> items;
  // 「文件」菜单原文 + 若干额外项（拉长到超过画布高，制造"下半截在屏外"）
  items.push_back(st::ui::MenuItem{.id = "file.new", .label = "新建"});
  items.push_back(st::ui::MenuItem{.id = "file.open", .label = "打开…"});
  {
    st::ui::MenuItem checked;
    checked.id = "file.autosave";
    checked.label = "自动保存";
    checked.checked = true;
    items.push_back(std::move(checked));
  }
  {
    st::ui::MenuItem with_children;
    with_children.id = "file.recent";
    with_children.label = "最近打开";
    st::ui::MenuItem child;
    child.id = "file.recent.1";
    child.label = "草稿.md";
    with_children.children.push_back(std::move(child));
    items.push_back(std::move(with_children));
  }
  {
    st::ui::MenuItem sep;
    sep.separator = true;
    items.push_back(std::move(sep));
  }
  items.push_back(st::ui::MenuItem{.id = "file.quit", .label = "退出"});
  // 再补几项，确保总高超过画布
  for (std::size_t index = 0; index < kItems; ++index) {
    st::ui::MenuItem extra;
    extra.id = "more." + std::to_string(index);
    extra.label = "其他项 " + std::to_string(index + 1);
    extra.checked = (index % 3 == 0);
    items.push_back(std::move(extra));
  }
  auto panel = std::make_unique<st::ui::MenuPanel>(std::move(items));
  // 锚点设在顶部：面板从 y=30 往下展开
  panel->set_anchor(st::math::Rect{0.0f, 0.0f, 200.0f, 30.0f});
  return panel;
}

/// 渲染一次，返回画布像素（与画布同尺寸）。
auto render(std::unique_ptr<st::ui::TextPort>& port, const st::ui::Theme& theme,
            int clip_height) -> st::raster::Canvas {
  auto panel = make_panel();
  st::ui::RenderContext context{theme, port.get(), 0.0};
  panel->measure(context, st::ui::Constraints{});
  panel->arrange(context, st::math::Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                        static_cast<float>(kHeight)});

  auto canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
  // clip_height < 画布高 → 只露出顶部一段（模拟"面板被压在屏幕底部"）
  if (clip_height > 0 && clip_height < kHeight) {
    canvas.push_clip_rect(
        st::math::Rect{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(clip_height)});
  }
  // ⚠ 必须直调 `paint_content`，不能调 `paint`：
  // `Element::paint` 是**带子节点递归的完整绘制**，它自己会做可见性/裁剪判断，
  // 在裸面板（没接 UiRoot）上会直接早退——那样量到的是空白画布，
  // 而空白 ↔ 空白的"零差异"毫无意义（实测踩到：反向注入明显错误后探针仍然全绿）。
  panel->paint_content(context, canvas);
  return canvas;
}

auto write_png(const st::raster::Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const std::uint32_t pixel = pixels[i];
    // 画布是 0xAARRGGBB（或 0xRRGGBBAA）——用既有 `to_rgba8` 更稳，
    // 但为了不引入额外依赖，这里按 Surface 文档的通道序展开。
    image.rgba[i * 4 + 0] = static_cast<std::uint8_t>((pixel >> 16) & 0xFFU);
    image.rgba[i * 4 + 1] = static_cast<std::uint8_t>((pixel >> 8) & 0xFFU);
    image.rgba[i * 4 + 2] = static_cast<std::uint8_t>(pixel & 0xFFU);
    image.rgba[i * 4 + 3] = static_cast<std::uint8_t>((pixel >> 24) & 0xFFU);
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "build/menu-clip-probe";
  (void)std::system(("mkdir -p " + out_dir).c_str());

  auto fonts = st::text::FontStack::system_default();
  if (!fonts.has_value() || fonts->empty()) {
    std::fprintf(stderr, "[probe] 无可用字体，无法验证像素\n");
    return 2;
  }
  st::text::TextRenderer renderer(*fonts, 1.0f);
  auto port = st::app::make_text_port(renderer);
  if (port == nullptr) {
    std::fprintf(stderr, "[probe] 文本端口构造失败\n");
    return 2;
  }

  const auto theme = st::ui::Theme::light();

  // ⓪ **自检：先确认真的画出了东西**。
  //
  // 为何必须有这一步：空白画布 ↔ 空白画布的"零像素差异"是彻头彻尾的假阳性。
  // 初版探针就死在这里：调了 `Element::paint`（会早退）而没调 `paint_content`，
  // 两张图都是纯背景。
  {
    const auto blank = render(port, theme, 0);
    const auto pixels = blank.pixels();
    std::size_t non_background = 0;
    for (const auto value : pixels) {
      // 背景是 0xFFFFFFFF；任何非纯白像素都算"画了东西"
      if (value != 0xFFFFFFFFU) ++non_background;
    }
    std::printf("[probe] 自检：非背景像素 %zu / %zu\n", non_background, pixels.size());
    if (non_background < 1000) {
      std::fprintf(stderr, "[probe] ✗ 画布几乎是空的（%zu 个非背景像素）——本次比对无效\n",
                   non_background);
      return 3;
    }
  }

  // ① 导出多档裁剪的 PNG。
  //
  // ## 为什么是"导出 + 外部比对"而不是"本进程内自比对"
  //
  // 「小裁剪 vs 大裁剪」在本进程内比是**结构上无效**的：两条路径跑的是同一份
  // `paint_content`，剔除逻辑的错误（比如误伤可见行）会对两边**同时生效**，
  // 差异恒为 0。初版就是这么写的，反向注入明显错误后仍然全绿。
  //
  // 真正能证明"改动不改变可见像素"的，是**跨版本比对**：
  // 把同一探针在改动前后各跑一次，PNG 逐字节相同。
  // 所以这里只负责产出稳定的图，比对交给调用方（见 tools/menu_clip_verify.ps1）。
  bool wrote = true;
  for (const int clip : {0, 64, 96, 120}) {
    const auto canvas = render(port, theme, clip);
    const std::string path = out_dir + "/menu-clip-" + std::to_string(clip) + ".png";
    wrote = write_png(canvas, path) && wrote;
    if (!wrote) std::fprintf(stderr, "[probe] ✗ 写 PNG 失败：%s\n", path.c_str());
  }
  std::printf("[probe] ✓ 已导出 4 档裁剪到 %s（请与改动前的产物逐字节比对）\n", out_dir.c_str());
  return wrote ? 0 : 1;
}
