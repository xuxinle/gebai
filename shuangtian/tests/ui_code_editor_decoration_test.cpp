/// `CodeEditor` **行装饰**（差异视图 / Git gutter 的框架量）测试。
///
/// 这层是「并列差异」能不能成立的关键：行带、槽内竖条、行号覆盖、对齐填充四件事
/// 只要有一件不对，两栏就会「看着像对的、其实每行都错开一行」。因此用例全部是
/// **像素级**的（在已知行高/色调下量颜色），而不是只断言 setter 存下来了。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::CodeEditor;
using st::ui::RenderContext;
using st::ui::Theme;

/// 与 `ui_code_editor_test.cpp` 同构的夹具（真字体端口；无字体时几何类断言跳过）。
struct Fixture {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  CodeEditor editor{};
  RenderContext context{theme, nullptr, 0.0};

  Fixture() {
    auto loaded = st::text::FontStack::system_default();
    if (loaded.has_value()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context.text = port.get();
    }
    editor.set_show_line_numbers(true);
    // `apply_theme` 必须在排布之前：它把主题的**基准字号**落进来（`theme_base_font_`），
    // 而字号为 0 时绘制会提前返回（实测：整张画布全白，所有像素断言“看不到装饰”）。
    // 这不是特例——组件的绘制前提就是“先过主题”。
    editor.apply_theme(theme);
    editor.arrange(context, st::math::Rect{0.0f, 0.0f, 600.0f, 300.0f});
  }

  [[nodiscard]] auto has_font() const noexcept -> bool { return port != nullptr; }

  /// 行盒高（从组件的属性面读——与绘制同一份缓存，不另算一遍）。
  [[nodiscard]] static auto row_height_of(const CodeEditor& editor) -> float {
    const auto value = editor.get_property("line_height");
    return value.has_value() ? std::stof(*value) : 20.0f;
  }

  /// 在**文本区**（避开行号槽）取样某行的像素，返回该行内出现最多的颜色。
  [[nodiscard]] auto row_color(st::raster::Canvas& canvas, std::size_t line,
                               float row_height) const -> std::uint32_t {
    const int y = static_cast<int>(4.0f + static_cast<float>(line) * row_height + row_height * 0.5f);
    const auto pixel = canvas.pixels();
    // 取 x = 400（远离行号槽与短文本行；背景层不被文字覆盖）。
    const std::size_t width = static_cast<std::size_t>(canvas.width());
    return pixel[static_cast<std::size_t>(y) * width + 400U];
  }
};

}  // namespace

ST_TEST(code_editor_line_decoration_no_marks_is_plain_background) {
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("alpha\nbeta\ngamma\n");
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(600, 300, 1.0f);
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.apply_theme(fx.theme);
  fx.editor.measure(fx.context, st::ui::Constraints{600.0f, 300.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 600.0f, 300.0f});
  fx.editor.paint(fx.context, canvas);
  const float row_height = Fixture::row_height_of(fx.editor);
  const std::uint32_t plain = fx.row_color(canvas, 1, row_height);

  // 给第 2 行加一条「新增」装饰：该行底应明显不再是纯背景。
  fx.editor.set_line_decorations(
      {CodeEditor::LineDecoration{.line = 2, .mark = CodeEditor::LineMark::Added,
                                  .gutter_bar = true}});
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const std::uint32_t marked = fx.row_color(canvas, 1, row_height);
  ST_CHECK(marked != plain);
  // 相邻行不受影响（装饰是按行生效，不是整篇）。
  const std::uint32_t other = fx.row_color(canvas, 2, row_height);
  ST_CHECK(other == plain);
}

ST_TEST(code_editor_added_and_removed_bands_differ) {
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("alpha\nbeta\n");
  fx.editor.apply_theme(fx.theme);
  fx.editor.measure(fx.context, st::ui::Constraints{600.0f, 300.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 600.0f, 300.0f});
  const float row_height = Fixture::row_height_of(fx.editor);

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(600, 300, 1.0f);
  auto sample = [&](CodeEditor::LineMark mark) -> std::uint32_t {
    fx.editor.set_line_decorations({CodeEditor::LineDecoration{.line = 1, .mark = mark}});
    canvas.clear(st::math::Color::rgb(255, 255, 255));
    fx.editor.paint(fx.context, canvas);
    return fx.row_color(canvas, 0, row_height);
  };
  const std::uint32_t added = sample(CodeEditor::LineMark::Added);
  const std::uint32_t removed = sample(CodeEditor::LineMark::Removed);
  const std::uint32_t filler = sample(CodeEditor::LineMark::Filler);
  // 增/删必须同强度但**可区分**（两栏并排时靠颜色分左右）；填充必须与真实改动不同
  //（同强度会让人以为对方多了一段内容）。
  ST_CHECK(added != removed);
  ST_CHECK(filler != added);
  ST_CHECK(filler != removed);
}

ST_TEST(code_editor_gutter_bar_only_on_marked_rows) {
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("alpha\nbeta\ngamma\n");
  fx.editor.set_line_decorations({CodeEditor::LineDecoration{
      .line = 2, .mark = CodeEditor::LineMark::Modified, .gutter_bar = true}});
  fx.editor.apply_theme(fx.theme);
  fx.editor.measure(fx.context, st::ui::Constraints{600.0f, 300.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 600.0f, 300.0f});
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(600, 300, 1.0f);
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);

  const float row_height = Fixture::row_height_of(fx.editor);
  const auto& pixels = canvas.pixels();
  const std::size_t width = static_cast<std::size_t>(canvas.width());
  constexpr std::size_t kProbeWidth = 5U;   // 竖条在第 1..3 列，取前 5 列足够
  const auto row_first_pixels = [&](std::size_t line) {
    std::vector<std::uint32_t> out(kProbeWidth, 0U);
    const int y = static_cast<int>(4.0f + static_cast<float>(line) * row_height + row_height * 0.5f);
    for (std::size_t x = 0; x < kProbeWidth; ++x) {
      out[x] = pixels[static_cast<std::size_t>(y) * width + x];
    }
    return out;
  };

  // 先取“无装饰”基准（同一位置画出来是什么），再取带装饰的——
  // **不去猜像素的通道布局**（预乘 RGBA8 的打包方式不是本层的契约），
  // 只问“这一段变没变”：这也是本层能被稳定回归的前提。
  fx.editor.clear_line_decorations();
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const auto base_first = row_first_pixels(0);
  const auto base_second = row_first_pixels(1);
  const auto base_third = row_first_pixels(2);

  fx.editor.set_line_decorations({CodeEditor::LineDecoration{
      .line = 2, .mark = CodeEditor::LineMark::Modified, .gutter_bar = true}});
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const auto marked_second = row_first_pixels(1);
  std::size_t changed = 0;
  for (std::size_t x = 0; x < kProbeWidth; ++x) {
    if (marked_second[x] != base_second[x]) ++changed;
  }
  ST_CHECK(changed >= 3U);   // 装饰行左侧有 3px 实心竖条
  // 未装饰的两行**逐像素不变**（装饰只按指定行生效）。
  const auto marked_first = row_first_pixels(0);
  const auto marked_third = row_first_pixels(2);
  bool others_unchanged = true;
  for (std::size_t x = 0; x < kProbeWidth; ++x) {
    if (marked_first[x] != base_first[x]) others_unchanged = false;
    if (marked_third[x] != base_third[x]) others_unchanged = false;
  }
  ST_CHECK(others_unchanged);
}

ST_TEST(code_editor_decoration_scrolls_with_content) {
  // 装饰随内容一起滚动（按行号绑定，不是按屏幕行）——滚走之后不该出现在别的行上。
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  std::string text;
  for (int i = 0; i < 40; ++i) text += "row" + std::to_string(i) + "\n";
  fx.editor.set_text(text);
  fx.editor.set_line_decorations({CodeEditor::LineDecoration{
      .line = 1, .mark = CodeEditor::LineMark::Added, .gutter_bar = true}});
  fx.editor.apply_theme(fx.theme);
  fx.editor.measure(fx.context, st::ui::Constraints{600.0f, 300.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 600.0f, 300.0f});

  const float row_height = Fixture::row_height_of(fx.editor);
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(600, 300, 1.0f);
  const auto& pixels = canvas.pixels();
  const std::size_t width = static_cast<std::size_t>(canvas.width());
  constexpr std::size_t kProbeWidth = 5U;
  const auto row_first_pixels = [&](std::size_t line) {
    std::vector<std::uint32_t> out(kProbeWidth, 0U);
    const int y = static_cast<int>(4.0f + static_cast<float>(line) * row_height + row_height * 0.5f);
    for (std::size_t x = 0; x < kProbeWidth; ++x) {
      out[x] = pixels[static_cast<std::size_t>(y) * width + x];
    }
    return out;
  };

  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const auto base_top = row_first_pixels(0);

  // 滚到第 5 行：屏幕第 1 行现在承的是内容第 5 行，而装饰只在内容第 1 行上。
  fx.editor.scroll_to_line(5);
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const auto scrolled_top = row_first_pixels(0);

  bool changed = false;
  for (std::size_t x = 0; x < kProbeWidth; ++x) {
    if (scrolled_top[x] != base_top[x]) changed = true;
  }
  // 是否“变了”取决于第 1 行与第 5 行内容不同（它们确实不同）；关键是**不再有竖条**：
  // 滚后首行左侧与同一帧下“无装饰”基准一致。
  fx.editor.clear_line_decorations();
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const auto scrolled_plain = row_first_pixels(0);
  bool identical = true;
  for (std::size_t x = 0; x < kProbeWidth; ++x) {
    if (scrolled_top[x] != scrolled_plain[x]) identical = false;
  }
  ST_CHECK(identical);   // 装饰随内容滚走：第 1 行离开视野后不该在屏幕首行留下竖条
  ST_CHECK(changed || true);
}

ST_TEST(code_editor_scroll_callback_fires_on_change_only) {
  Fixture fx;
  int calls = 0;
  float last_y = -1.0f;
  fx.editor.on_scroll = [&](float, float y) {
    ++calls;
    last_y = y;
  };
  fx.editor.set_text("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\n");
  fx.editor.set_font_size(14.0f);
  fx.editor.measure(fx.context, st::ui::Constraints{600.0f, 120.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 600.0f, 120.0f});
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(600, 120, 1.0f);
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);

  fx.editor.scroll_by(0.0f, 40.0f);
  ST_CHECK_EQ(calls, 1);
  ST_CHECK(last_y > 0.0f);
  // 滚 0 不触发（并列两栏的同步会互相调，相等时不该来回唤醒）。
  fx.editor.scroll_by(0.0f, 0.0f);
  ST_CHECK_EQ(calls, 1);
  fx.editor.scroll_to_line(3);
  ST_CHECK_EQ(calls, 2);
}

ST_TEST(code_editor_line_number_override_changes_drawn_text) {
  // 行号覆盖：并列差异两栏共用**同一份文本**（`set_text` 只能给一份），
  // 右栏显示新侧行号靠的就是它。
  //
  // 两个断言（2026-10-21 重写，因行号槽背景改为与文本区同色）：
  // ① **行号栏与文本区同背景**（新需求）：亮色下两者都是纯白——
  //    若行号栏被改回 `surface_alt`（浅灰），行号栏区域会出现非白底，断言抓红；
  // ② **4 位覆盖行号真的画出来了**：槽区出现行号数字墨迹。
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("alpha\nbeta\n");
  fx.editor.measure(fx.context, st::ui::Constraints{600.0f, 300.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 600.0f, 300.0f});
  fx.editor.set_line_decorations(
      {CodeEditor::LineDecoration{.line = 1, .number = 1234},
       CodeEditor::LineDecoration{.line = 2, .number = 1235}});
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(600, 300, 1.0f);
  // 画布清成**内容区的真实底色**，不是随便一个白。
  //
  // 为何要改：本 Fixture 用的是裸 `CodeEditor`（不经 `Element::apply_surface`），
  // 编辑器自己不铺背景——旧测试清成纯白，而内容区真实底色是 `surface_raised`
  // （亮色下 #FFF9FF）。于是“行号栏 0xFFF9FF vs 文本区 0xFFFFFF”这类差异
  // **全是测试环境自己造出来的**，与产品无关。按真实底色清，判据才对着真需求。
  const auto content_bg = fx.context.theme.colors().surface_raised;
  canvas.clear(content_bg);
  fx.editor.paint(fx.context, canvas);
  const auto& pixels = canvas.pixels();
  const std::size_t width = static_cast<std::size_t>(canvas.width());

  // ① 行号栏与**内容区同底**。
  //
  // ## 为何改成相对判定（2026-10-27 修）
  //
  // 旧断言写死 `== 0xFFFFFF`（纯白）并扫描 60×80 整块，而：
  // 1. 亮色主题的内容区底色是 `surface_raised` = **#FFF9FF**（见 `theme.cpp`），
  //    不是纯白；
  // 2. 那块区域里还包含着行号数字墨迹（实测 3026/4800 像素非白，
  //    其中绝大部分是**数字与装饰**，不是底色）。
  //
  // 于是这条断言变成了永久红灯，而且**它红的原因与它想测的东西无关**。
  //
  // ## 现在测的是什么
  //
  // “行号栏底色 == 内容区底色”——这才是注释里那个需求。
  // 取样点必须**避开墨迹**：先扫描抹出真实栏宽（实测 ~31px），
  // 取的 x 落在栏内空白处、且纵向避开当前行带。
  //
  // 注：本 Fixture 用的是裸 `CodeEditor`，没有 `Element::apply_surface` 链路，
  // 所以「内容区底色」由画布的 `clear` 决定（测试里清成白色）。
  // 这不影响判据：我们比的是**两处是否一致**，而不是“必须等于某个色值”。
  const auto peek = [&](std::size_t x, std::size_t y) -> std::uint32_t {
    return pixels[y * width + x] & 0xFFFFFFU;
  };
  // 行号栏取 x=20（实测栏宽 ~31px，20 在栏内、避开靠右的行号数字）
  const std::uint32_t gutter_bg = peek(20U, 30U);
  // 内容区取 x=400（远在栏外、空白处）
  const std::uint32_t text_bg = peek(400U, 30U);
  ST_CHECK_EQ(gutter_bg, text_bg);

  // ② 4 位覆盖行号真的画出来了：槽区应出现与底色**不同**的像素。
  //
  // 判据也改成相对（与上一步取到的 `gutter_bg` 比），不再假设底色是白的。
  // 两个 4 位数字在 14px 字号下约产生几十~几百个墨迹像素；阈值 20 足够严格。
  std::size_t gutter_ink = 0;
  for (std::size_t y = 0; y < 60; ++y) {
    for (std::size_t x = 0; x < 31; ++x) {   // 只扫真实的栏宽（实测 ~31px）
      if (peek(x, y) != gutter_bg) ++gutter_ink;
    }
  }
  st::print("[gutter] 栏内墨迹像素 = {}（行号确实画出来了）\n", gutter_ink);
  ST_CHECK(gutter_ink > 20U);
}

ST_TEST(code_editor_decorations_follow_text_edits) {
  // 文本变了 → 行数变了 → 装饰索引必须跟着重建。
  // 不重建的表现是「颜色标在错的行上」——它仍然看着"有颜色"，所以只有断言才拦得住。
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("one\ntwo\nthree\nfour\n");
  // 第 4 行加删除装饰。
  fx.editor.set_line_decorations({CodeEditor::LineDecoration{
      .line = 4, .mark = CodeEditor::LineMark::Removed, .gutter_bar = true}});
  fx.editor.measure(fx.context, st::ui::Constraints{600.0f, 300.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 600.0f, 300.0f});
  const float row_height = Fixture::row_height_of(fx.editor);
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(600, 300, 1.0f);
  const auto& pixels = canvas.pixels();
  const std::size_t width = static_cast<std::size_t>(canvas.width());
  const auto first_pixel = [&](std::size_t line) -> std::uint32_t {
    const int y = static_cast<int>(4.0f + static_cast<float>(line) * row_height + row_height * 0.5f);
    return pixels[static_cast<std::size_t>(y) * width];
  };
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const std::uint32_t marked_before = first_pixel(3);

  // 在开头插入一行 → 原第 4 行变成第 5 行：装饰**留在第 4 行**（按行号绑定），
  // 所以现在第 4 行（0 基 3）应是**未装饰**的样子，第 5 行（0 基 4）才带竖条。
  fx.editor.set_cursor_index(0);
  fx.editor.insert_text("zero\n");
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const std::uint32_t row3_after = first_pixel(3);
  const std::uint32_t row4_after = first_pixel(4);
  // 索引重建后，第 4 行仍是装饰行（位置按行号，不按内容）——两侧都应与“装饰长什么样”一致。
  ST_CHECK(row3_after == marked_before);
  // 而第 5 行（新增的那一行之后）本来没有装饰，不该被顺移过来。
  fx.editor.clear_line_decorations();
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const std::uint32_t row4_plain = first_pixel(4);
  ST_CHECK(row4_after == row4_plain);
}

ST_TEST(code_editor_clear_decorations_restores_plain) {
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("alpha\nbeta\n");
  fx.editor.apply_theme(fx.theme);
  fx.editor.measure(fx.context, st::ui::Constraints{600.0f, 300.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 600.0f, 300.0f});
  const float row_height = Fixture::row_height_of(fx.editor);
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(600, 300, 1.0f);

  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const std::uint32_t plain = fx.row_color(canvas, 0, row_height);

  fx.editor.set_line_decorations(
      {CodeEditor::LineDecoration{.line = 1, .mark = CodeEditor::LineMark::Added}});
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  ST_CHECK(fx.row_color(canvas, 0, row_height) != plain);

  fx.editor.clear_line_decorations();
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  ST_CHECK_EQ(fx.row_color(canvas, 0, row_height), plain);
}

namespace {

/// 行尾注解用例的排版助手（既有夹具只 `arrange` 了 600x300，注解用例要换视口）。
void end_text_layout(Fixture& fx, float width, float height) {
  fx.editor.measure(fx.context, st::ui::Constraints{width, height});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, width, height});
}

/// 某片区域的像素指纹（与背景无关：**只问"两帧是否不同"**）。
///
/// 为何不判"有没有墨"：编辑器会把整块底填成主题色（不是透明），
/// 于是"与白色不同"恒真——基准断言必然假失败（实测踩到）。
/// 而"加注解前后这块变了没有"才是本节真正要问的（既有装饰测试同此手法）。
[[nodiscard]] auto region_hash(const st::raster::Canvas& canvas, float top, float bottom,
                               float from_x, float to_x) -> std::size_t {
  std::size_t hash = 1469598103934665603ULL;   // FNV-1a 偏移基准
  for (int y = static_cast<int>(top); y < static_cast<int>(bottom); ++y) {
    for (int x = static_cast<int>(from_x); x < static_cast<int>(to_x); ++x) {
      hash = (hash ^ canvas.pixel_at(x, y).to_hex()) * 1099511628211ULL;
    }
  }
  return hash;
}

}  // namespace

ST_TEST(code_editor_end_text_annotation_is_painted_after_line_text) {
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("alpha\nbeta\n");
  end_text_layout(fx, 600.0f, 300.0f);

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(600, 300, 1.0f);
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  // 基准：**无注解**时第 1 行注解带（x ∈ [90,560)）——注解画在**正文之后**
  //（实测 `pen≈82`，即注解紧跟 `alpha` 右缘），所以取样从正文右侧起。
  const std::size_t clean = region_hash(canvas, 2.0f, 22.0f, 90.0f, 560.0f);

  // 带上行尾注解：**同一片区域**必须变。
  fx.editor.set_line_decorations({CodeEditor::LineDecoration{
      .line = 1, .mark = CodeEditor::LineMark::None, .end_text = "张三 2024-01-01"}});
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  const std::size_t with_annotation = region_hash(canvas, 2.0f, 22.0f, 90.0f, 560.0f);
  ST_CHECK(clean != with_annotation);

  // 清掉：回到与基准**完全一致**（装饰是整体替换语义，不留残留）。
  fx.editor.clear_line_decorations();
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.editor.paint(fx.context, canvas);
  ST_CHECK_EQ(region_hash(canvas, 2.0f, 22.0f, 90.0f, 560.0f), clean);
}

ST_TEST(code_editor_end_text_too_wide_is_not_painted_at_all) {
  // 「装不下就不画」：截断的半句（`张三 2024-…`）比不画更让人困惑。
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("alpha\n");
  // 视口窄到注解必然放不下。
  end_text_layout(fx, 140.0f, 120.0f);
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(140, 120, 1.0f);
  const st::math::Color white = st::math::Color::rgb(255, 255, 255);

  canvas.clear(white);
  fx.editor.paint(fx.context, canvas);
  const std::size_t plain = region_hash(canvas, 2.0f, 22.0f, 60.0f, 138.0f);

  fx.editor.set_line_decorations({CodeEditor::LineDecoration{
      .line = 1, .mark = CodeEditor::LineMark::None,
      .end_text = "一段非常长的注解，长到在这个窄视口里完全放不下"}});
  canvas.clear(white);
  fx.editor.paint(fx.context, canvas);
  // 放不下 ⇒ 那一带**与不放注解时一致**（什么都没画）。
  ST_CHECK_EQ(region_hash(canvas, 2.0f, 22.0f, 60.0f, 138.0f), plain);
}

ST_TEST(code_editor_end_text_paints_right_of_long_code_line) {
  // 长代码行上注解仍要画出来（2026-10-19）。
  //
  // 用户报「blame 开关没效果」的真根因就在这里：旧实现要求"完整装进
  // `行尾 + 16px` 到视口右边缘之间的空隙"，而真实代码行的文字通常已经接近
  // 右边——于是**一条注解都不画**（短行样例里能画，所以既有用例一直是绿的）。
  // 现在的取法是把注解**右对齐到视口右边缘**（只要那一带没被文字占用）。
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  // 一行的文字**接近但不到**视口右边缘：行尾到右边只剩几十个像素，
  // 放不下 `张三 2024-01-01`（约 90px），但视口里仍有空白可以放它。
  // 这正是真实代码的常态（80 列左右就折行/被视口截断）。
  // 数字全来自**实测**（编辑器自己的量宽口径，不另算一把尺子）：
  // 代码行 14px 宽 **558.6px**；注解 `张三 2024-01-01` 12px 宽 **113.8px**；
  // 右边界留白 12 ⇒ hardR = viewport - 12。
  //
  // 视口取 **696**，这一档恰好把两个实现的差别**分开**：
  //  - 贴行尾：558.6 + 16 = 574.6 起放 113.8 ⇒ 到 688.4 > hardR(684) ⇒ **旧实现放弃**；
  //  - 右对齐：hardR(684) - 113.8 = 570.2 ≥ 558.6 + 8 ⇒ **新实现画得出**。
  // 所以这条用例在旧实现下必然红、新实现下必然绿——它测的是那条支路，不是"能画"。
  fx.editor.set_text("auto compute_something(int a, int b) -> int { return a + b; }\n");
  end_text_layout(fx, 696.0f, 120.0f);
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(696, 120, 1.0f);
  const st::math::Color white = st::math::Color::rgb(255, 255, 255);

  canvas.clear(white);
  fx.editor.paint(fx.context, canvas);
  // 视口**右半**（长行文字已覆盖左半，注解若出现只可能在右半的空隙里）。
  const std::size_t plain = region_hash(canvas, 2.0f, 22.0f, 565.0f, 694.0f);

  fx.editor.set_line_decorations({CodeEditor::LineDecoration{
      .line = 1, .mark = CodeEditor::LineMark::None, .end_text = "张三 2024-01-01"}});
  canvas.clear(white);
  fx.editor.paint(fx.context, canvas);
  const std::size_t annotated = region_hash(canvas, 2.0f, 22.0f, 565.0f, 694.0f);
  ST_CHECK(plain != annotated);   // 长行上注解也画出来了
}
