/// 字形缓存的一致性：**有缓存与无缓存必须逐像素一致**。
///
/// 起因：用户截图里出现"合法但错误的字"（概览→影来、win32→wio32），
/// 而**排版完全不变**（CJK 全角等宽，所以错误的同号字形宽度相同，版面看不出差别）。
/// 这类症状只有一个来源：**取到了别的字形**——也就是缓存/索引串味。
///
/// `TextRenderer::glyph_bitmap` 的缓存键是把几个字段移位后**纯 XOR** 拼成的
/// （路径哈希 ^ face_index<<8 ^ glyph<<24 ^ size<<48 ^ supersample<<60）。
/// XOR 拼装只有在位段**互不重叠**时才等价于元组——一旦重叠，
/// 不同的 (face, glyph, size) 会落到同一个键，于是"取到别人的字形"。
/// 这个测试从**外部行为**验证这件事：缓存永远不该改变结果。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "st/app/app.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontRole;
using st::text::FontStack;
using st::text::TextRenderer;

/// 有字体的环境才跑（无字体容器里这些断言没有意义）。
struct FontFixture {
  std::unique_ptr<FontStack> stack{};
  bool ok{false};
  FontFixture() {
    if (auto loaded = FontStack::system_default(); loaded.has_value()) {
      stack = std::make_unique<FontStack>(std::move(*loaded));
      ok = !stack->empty();
    }
  }
};

/// 把一段文本画到画布上，返回像素（用于逐像素比较）。
[[nodiscard]] auto render_text(const FontStack& stack, std::string_view text, float size)
    -> std::vector<std::uint32_t> {
  Canvas canvas{900, 64};
  canvas.clear(Color{0, 0, 0, 0});
  TextRenderer renderer(stack, 1.0f);
  renderer.draw(canvas, text, Point{4.0f, 40.0f}, size, Color{0xFF, 0xFF, 0xFF, 0xFF});
  return std::vector<std::uint32_t>(canvas.pixels().begin(), canvas.pixels().end());
}

[[nodiscard]] auto count_differing(const std::vector<std::uint32_t>& a,
                                   const std::vector<std::uint32_t>& b) -> std::size_t {
  if (a.size() != b.size()) return a.size() + b.size();
  std::size_t differing = 0;
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (a[index] != b[index]) ++differing;
  }
  return differing;
}

/// 覆盖界面里真实出现过的字（导航项、卡片标题、状态栏片段）。
constexpr std::string_view kUiText =
    "概览 组件 数据 控制通道 关于 按钮与图标 表单 密码 提交 重置 进度与状态 "
    "光栅器覆盖率 字体缓存命中 刷新指标 渲染器 组件节点 自绘 主要操作 次要 轻量 柔和 危险 "
    "win32 DPI 1.0 1280x800 第 1 帧 已切到「数据」";

/// 同一个渲染器重复渲染必须稳定（缓存命中不能改变结果）。
constexpr std::string_view kLongText =
    "霜天 自绘 UI 跨平台 软硬件渲染兼容 无头可控 DPI 感知 布局 光栅化 着色器 深度缓冲 "
    "路径填充 模板缓冲 非零环绕 字形图集 字形缓存 等宽字体 代码块 高亮 语义树 控制通道 "
    "反锯齿 扫描线 覆盖率 SIMD 快路径 帧预算 脏矩形 送显 交换链 双缓冲 垂直同步 "
    "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789";

}  // namespace

ST_TEST(text_cache_does_not_change_glyphs) {
  FontFixture fixture;
  if (!fixture.ok) return;

  const std::vector<std::uint32_t> fresh = render_text(*fixture.stack, kUiText, 15.0f);
  // 同一个渲染器再来一次（这次全部命中缓存）：必须与首次**逐像素一致**
  TextRenderer renderer(*fixture.stack, 1.0f);
  Canvas canvas{900, 64};
  canvas.clear(Color{0, 0, 0, 0});
  renderer.draw(canvas, kUiText, Point{4.0f, 40.0f}, 15.0f, Color{0xFF, 0xFF, 0xFF, 0xFF});
  const std::vector<std::uint32_t> cached(canvas.pixels().begin(), canvas.pixels().end());
  ST_CHECK_EQ(static_cast<int>(count_differing(fresh, cached)), 0);
}

ST_TEST(text_cache_survives_eviction_without_substituting_glyphs) {
  FontFixture fixture;
  if (!fixture.ok) return;
  const std::vector<std::uint32_t> baseline = render_text(*fixture.stack, kUiText, 15.0f);

  // 灌入远超缓存上限（512 条）的不同字形，逼出淘汰——淘汰之后重渲染必须仍然正确。
  // 这条是"缓存淘汰后取到别人的字形"的探针。
  TextRenderer renderer(*fixture.stack, 1.0f);
  Canvas scratch{1400, 64};
  for (int round = 0; round < 40; ++round) {
    scratch.clear(Color{0, 0, 0, 0});
    const std::string filler = std::to_string(round) + " " + std::string(kLongText) + " " +
                               std::string(kUiText);
    renderer.draw(scratch, filler, Point{4.0f, 40.0f}, 13.0f + static_cast<float>(round % 5),
                  Color{0xFF, 0xFF, 0xFF, 0xFF});
  }

  Canvas canvas{900, 64};
  canvas.clear(Color{0, 0, 0, 0});
  renderer.draw(canvas, kUiText, Point{4.0f, 40.0f}, 15.0f, Color{0xFF, 0xFF, 0xFF, 0xFF});
  const std::vector<std::uint32_t> after(canvas.pixels().begin(), canvas.pixels().end());
  ST_CHECK_EQ(static_cast<int>(count_differing(baseline, after)), 0);
}

ST_TEST(text_glyphs_at_different_sizes_do_not_collide) {
  FontFixture fixture;
  if (!fixture.ok) return;
  // 缓存键把字号放在 <<48 位段。若与字形号（<<24，CJK 字形号可达 16 位以上）重叠，
  // 就会出现"某个字号下取到别的字形"。构造**大量**字号 × 大量字形来碰撞。
  TextRenderer renderer(*fixture.stack, 1.0f);
  std::vector<std::vector<std::uint32_t>> reference;
  const std::vector<float> sizes{11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f,
                                18.0f, 20.0f, 24.0f, 30.0f};
  for (const float size : sizes) {
    reference.push_back(render_text(*fixture.stack, kUiText, size));
  }
  Canvas canvas{900, 64};
  for (std::size_t index = 0; index < sizes.size(); ++index) {
    // 交叉渲染（不同的字号交替），让缓存同时持有多个字号的同批字形
    canvas.clear(Color{0, 0, 0, 0});
    renderer.draw(canvas, kUiText, Point{4.0f, 40.0f}, sizes[index],
                  Color{0xFF, 0xFF, 0xFF, 0xFF});
    const std::vector<std::uint32_t> actual(canvas.pixels().begin(), canvas.pixels().end());
    ST_CHECK_EQ(static_cast<int>(count_differing(reference[index], actual)), 0);
  }
}

ST_TEST(text_distinct_codepoints_render_distinct_glyphs) {
  FontFixture fixture;
  if (!fixture.ok) return;
  // 更强的形状判据：**不同码点必须画出不同字形**。
  // 若 cmap/字形查表把多个码点映到同一个字形（"合法但错误"），这里会暴露成重复。
  // 用形状差异极大的字，且都是界面里真实出现过的字。
  const std::vector<std::string> samples{"一", "丨", "口", "日", "十", "八", "人",
                                         "大", "小", "上", "下", "中", "田", "目"};
  std::vector<std::vector<std::uint32_t>> bitmaps;
  for (const auto& sample : samples) {
    bitmaps.push_back(render_text(*fixture.stack, sample, 32.0f));
  }
  std::size_t duplicates = 0;
  for (std::size_t i = 0; i < bitmaps.size(); ++i) {
    for (std::size_t j = i + 1; j < bitmaps.size(); ++j) {
      if (count_differing(bitmaps[i], bitmaps[j]) == 0) ++duplicates;
    }
  }
  ST_CHECK_EQ(static_cast<int>(duplicates), 0);
}

ST_TEST(text_glyphs_are_not_clipped) {
  FontFixture fixture;
  if (!fixture.ok) return;
  // 字形位图必须包含**全部墨迹**。裁切会让字看起来"变了样"——
  // 而字宽（advance）照旧，所以排版完好、只是字不对：
  // 用户实际报的就是这个现象（截图里字形被换掉、版面却分毫不差）。
  //
  // 判据很直接：位图四周必须留出空白（生成时 padding=1）。
  // 墨迹一旦贴到边界，就说明包围盒算小了（例如曲线极值没算进去）。
  TextRenderer renderer(*fixture.stack, 1.0f);
  const std::vector<std::string> samples{
      "霜", "概", "览", "组", "件", "控", "制", "通", "道", "关", "于", "数", "据",
      "危", "险", "柔", "和", "轻", "量", "重", "置", "密", "码",
      "g", "y", "Q", "@", "%", "0", "8", "m", "W", "|", "/", "~",
  };
  std::size_t clipped = 0;
  for (const auto& sample : samples) {
    const std::u32string codepoints = st::utf8_decode(sample);
    if (codepoints.empty()) continue;
    for (const float size : {11.0f, 15.0f, 22.0f, 40.0f}) {
      const auto bitmap = renderer.glyph_bitmap_of(codepoints.front(), size);
      if (bitmap == nullptr || bitmap->width <= 2 || bitmap->height <= 2) continue;
      const auto ink_at = [&](int x, int y) {
        return bitmap->coverage[static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(bitmap->width) +
                                static_cast<std::size_t>(x)] != 0.0f;
      };
      bool touches_border = false;
      for (int x = 0; x < bitmap->width; ++x) {
        if (ink_at(x, 0) || ink_at(x, bitmap->height - 1)) touches_border = true;
      }
      for (int y = 0; y < bitmap->height; ++y) {
        if (ink_at(0, y) || ink_at(bitmap->width - 1, y)) touches_border = true;
      }
      if (touches_border) {
        ++clipped;
        st::print("[glyph] 墨迹贴边（可能被裁切）：{} @ {}px  {}x{}\n", sample, size,
                  bitmap->width, bitmap->height);
      }
    }
  }
  ST_CHECK_EQ(static_cast<int>(clipped), 0);
}

ST_TEST(text_glyphs_stay_correct_across_glyph_cache_trim_on_gpu) {
  // 这条是**"界面文字间歇性变成别的字"**的回归测试（用户实际报的那个缺陷）。
  //
  // 根因：GPU 后端把字形覆盖率纹理**按位图地址**缓存，并假设"字形位图长期存活、
  // 指针稳定"。但字体引擎的缓存超过上限会 `glyphs.clear()` 释放全部位图，
  // 新字形随后**复用同一批地址** → GPU 端"按地址命中"把上一个字形的纹理当成这个的。
  // 症状：`folder` 显示成 `folBer`、`概览` 变成 `外测`，且只在渲染过足够多字形
  // （触发过一次清空）之后才出现——所以单看几行代码或跑几个用例都发现不了。
  //
  // 因此用例必须**走 GPU 画布**且**跨过一次缓存清空**：
  // ① 渲染一小段文字，记下像素；
  // ② 灌入远超上限（512）的**不同字形**，逼出清空；
  // ③ 再渲染同一段文字，要求**逐像素与①一致**。
  // 软件画布永远通不过这条的"检验作用"（它不做纹理缓存），所以必须用 GPU 画布。
  if (!st::raster::gpu::available()) return;
  auto target = st::raster::gpu::create_canvas(600, 60, 1.0f, {});
  if (!target.has_value()) return;
  st::raster::Surface& surface = **target;

  FontFixture fixture;
  if (!fixture.ok) return;
  constexpr std::string_view kProbe = "folder copy download calendar";
  const auto render_probe = [&]() {
    surface.clear(Color{0, 0, 0, 0});
    TextRenderer renderer(*fixture.stack, 1.0f);
    renderer.draw(surface, kProbe, Point{4.0f, 40.0f}, 13.0f, Color{255, 255, 255, 255});
    return std::vector<std::uint32_t>(surface.pixels().begin(), surface.pixels().end());
  };

  const std::vector<std::uint32_t> reference = render_probe();

  // ② 灌入大量不同字形：72 个图标名 + 中文，跨多个字号 → 远超 512 条上限
  {
    TextRenderer filler(*fixture.stack, 1.0f);
    Canvas scratch{1400, 60};
    for (int round = 0; round < 12; ++round) {
      scratch.clear(Color{0, 0, 0, 0});
      const std::string line = std::string(kLongText) + " " + std::string(kUiText) + " " +
                               std::to_string(round) +
                               " folder copy download calendar settings share package warning "
                               "filter circle image send edit upload refresh link lock bell file";
      filler.draw(scratch, line, Point{4.0f, 40.0f}, 10.0f + static_cast<float>(round % 4),
                  Color{255, 255, 255, 255});
      filler.draw(surface, line, Point{4.0f, 40.0f}, 10.0f + static_cast<float>(round % 4),
                  Color{255, 255, 255, 255});
    }
  }

  // ③ 再渲染同一段文字：必须与①逐像素一致
  const std::vector<std::uint32_t> after = render_probe();
  ST_CHECK_EQ(static_cast<int>(count_differing(reference, after)), 0);
}

/// **整形缓存必须区分 `bold`**：真粗体面与合成加粗都不能被常规面的缓存条目吃掉。
///
/// 起因：`shape_cached` 的键是 `fnv1a64(utf8) ^ size_key<<32 ^ role<<56 ^ fingerprint`——
/// **没有 `bold`**。而 `bold` 决定的是用哪张字体面（`find_face(..., bold)`），
/// `ShapedText::runs` 里存的正是 `(face, glyph id)` 对。
/// 布局阶段先跑的 `measure_width()`（`bold` 默认 `false`）会把**常规面**的整形结果
/// 写成缓存条目，绘制阶段再用 `bold=true` 取时命中同一条——于是
/// 「真粗体字体面」这条路径**从未生效**（Bold 与 Regular 逐位相同），
/// 且 `(face, glyph)` 跨面混搭时取到的是另一张表的位图（字被换成别的）。
///
/// 判据不能用“墨量变大”（那取决于字体设计），而要用**整形结果本身不同**：
/// 同一串文字在 `bold=false/true` 下的 run 数未必不同，但**至少有一个 run 的
/// 字形或字体面不同**（系统有粗体面时）。
ST_TEST(text_shape_cache_distinguishes_bold) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer renderer(*fixture.stack, 1.0f);

  // 先用**常规**档查一次（写进缓存）——这正是布局阶段 measure_width 干的事。
  constexpr std::string_view kSample = "概览 Bold混合ABC";
  const float regular_width = renderer.measure_width(kSample, 15.0f);
  ST_CHECK(regular_width > 0.0f);

  const auto regular = renderer.shape(kSample, 15.0f, FontRole::Proportional, false);
  const auto bold = renderer.shape(kSample, 15.0f, FontRole::Proportional, true);
  ST_REQUIRE(!regular.runs.empty());
  ST_REQUIRE(!bold.runs.empty());
  ST_CHECK_EQ(regular.runs.size(), bold.runs.size());

  // 任何一 run 的字体面或字形不同，就说明两条路径确实各取了自己的表。
  bool differs = false;
  for (std::size_t index = 0; index < regular.runs.size(); ++index) {
    if (regular.runs[index].face != bold.runs[index].face) differs = true;
    if (regular.runs[index].glyph != bold.runs[index].glyph) differs = true;
  }
  // 系统真的提供了粗体面时，两种整形就不可能逐位相同。
  if (fixture.stack->has_bold()) {
    ST_CHECK(differs);
  }

  // 反向顺序也得成立：先查 bold，再查 regular，两条仍各自正确。
  const auto bold_first = renderer.shape(kSample, 15.0f, FontRole::Proportional, true);
  const auto regular_after = renderer.shape(kSample, 15.0f, FontRole::Proportional, false);
  ST_CHECK_EQ(regular_after.runs.size(), regular.runs.size());
  for (std::size_t index = 0; index < regular.runs.size(); ++index) {
    ST_CHECK(regular_after.runs[index].face == regular.runs[index].face);
    ST_CHECK(regular_after.runs[index].glyph == regular.runs[index].glyph);
    ST_CHECK(bold_first.runs[index].face == bold.runs[index].face);
    ST_CHECK(bold_first.runs[index].glyph == bold.runs[index].glyph);
  }

  // **已知遗留**（已记 `docs/BACKLOG.md`）：布局用的是 `measure_width()`，
  // 它**没有 bold 参数**（`TextPort::measure_width` 的接口面就没这一项），
  // 所以布局按常规面量宽、绘制按粗体面画。两面的字宽不一致时，
  // 粗体文本的居中/换行会有偏差。这里把量到的差值打出来做证据。
  st::print("[glyph] measure(bold=false)={:.3f} · shape(bold=true).width={:.3f} · 差 {:.3f}\n",
            static_cast<double>(renderer.measure_width(kSample, 15.0f)),
            static_cast<double>(bold.width),
            static_cast<double>(bold.width - renderer.measure_width(kSample, 15.0f)));
}

/// 超采样是**字形位图密度**的一部分：切换它必须让位图重新生成。
///
/// 起因：`Application::set_device_scale` 只改了缓冲尺寸，而超采样在构造时
/// 由解析出的 DPI 定下——运行期切 DPI 时停在旧值，于是“启动即 2.0”（超采样 2）
/// 与“启动 1.0 再切到 2.0”（超采样停在 1）渲染结果不同（实测同一区域
/// 1524 个像素差 >32）。修法是切 DPI 时一并 `set_supersample(scale)`。
///
/// 判据用**超采样变化必须改变像素**：同一字形在 ss=1 与 ss=2 下覆盖率网格不同
/// （边缘多一级采样），渲染结果必然不同——若相同，就是切换没落地。
ST_TEST(text_supersample_change_rebuilds_glyph_bitmaps) {
  FontFixture fixture;
  if (!fixture.ok) return;
  constexpr std::string_view kSample = "概览 A1";

  const auto render_at = [&](float supersample) {
    TextRenderer renderer(*fixture.stack, supersample);
    Canvas canvas{240, 60};
    canvas.clear(Color{0, 0, 0, 0});
    renderer.draw(canvas, kSample, Point{4.0f, 44.0f}, 20.0f, Color{255, 255, 255, 255});
    return std::vector<std::uint32_t>(canvas.pixels().begin(), canvas.pixels().end());
  };

  const std::vector<std::uint32_t> dense = render_at(2.0f);
  const std::vector<std::uint32_t> sparse = render_at(1.0f);
  ST_CHECK(count_differing(dense, sparse) > 0);

  // **同一渲染器上切换**（与 `Application::set_device_scale` 的实际调用形态一致）：
  // 若不切换，第一次画的是 ss=1 的位图，切到 ss=2 后必须不再复用。
  TextRenderer renderer(*fixture.stack, 1.0f);
  const auto paint = [&]() {
    Canvas canvas{240, 60};
    canvas.clear(Color{0, 0, 0, 0});
    renderer.draw(canvas, kSample, Point{4.0f, 44.0f}, 20.0f, Color{255, 255, 255, 255});
    return std::vector<std::uint32_t>(canvas.pixels().begin(), canvas.pixels().end());
  };
  const std::vector<std::uint32_t> before = paint();
  renderer.set_supersample(2.0f);
  const std::vector<std::uint32_t> after = paint();
  ST_CHECK_EQ(static_cast<int>(count_differing(before, after)),
              static_cast<int>(count_differing(sparse, dense)));
}

/// **超采样必须跟着运行期 DPI 走**（走真实的 `Application` 链路）。
///
/// 这是上面那条的**接线版**：上面只证明“`set_supersample` 会重建位图”，
/// 不证明“切 DPI 时真的调了它”。实测踩到：超采样只在构造时由解析出的 DPI 定下，
/// `Application::set_device_scale` 只改缓冲尺寸——于是“启动即 2.0”（超采样 2）
/// 与“启动 1.0 再切到 2.0”（超采样停在 1）**渲染结果不同**，大小号字号的
/// 斜向/弧形边缘出现可见阶梯（实测同区域 1524 个像素差 >32）。
///
/// 判据用**同 DPI 的两条到达路径必须等价**：
/// - 路径 A：启动即 2.0；
/// - 路径 B：启动 1.0，再 `set_device_scale(2.0)`。
/// 两者最终 `device_scale` 相同，画面也应逐像素相同。
ST_TEST(app_runtime_dpi_switch_matches_startup_dpi) {
  if (st::text::FontStack::system_default().has_value() == false) return;

  // 同一份内容：一段中文标题 + 大号数字（大字号的边缘差异最明显）。
  const auto make_content = []() {
    auto panel = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    panel->style().padding = st::math::Insets::all(16.0f);
    panel->style().gap = 10.0f;
    auto title = std::make_unique<st::ui::Heading>("概览", 2);
    title->set_id("dpi-probe-title");
    panel->add_child(std::move(title));
    auto big = std::make_unique<st::ui::Text>("127.0.0.1:53044");
    big->set_font_size(26.0f);
    big->set_weight(st::ui::FontWeight::SemiBold);
    big->set_id("dpi-probe-big");
    panel->add_child(std::move(big));
    return panel;
  };

  const auto render_at = [&](std::uint16_t port) {
    st::app::AppOptions options;
    options.width = 320;
    options.height = 160;
    options.headless = true;
    options.backend = "headless";
    options.control_port = port;
    options.control_file = std::format("{}/st-app-dpi-{}.json", st::fs::temp_dir(), port);
    options.renderer = "software";  // GPU 在 CI/容器里不保证有，且这是字形密度问题
    st::app::Application app("dpi-probe", "0.0", options);
    app.set_content(make_content());
    if (auto status = app.start(); !status) return std::vector<std::uint8_t>{};
    app.render_frame();
    auto pixels = app.capture_pixels({});
    if (!pixels.has_value()) return std::vector<std::uint8_t>{};
    return std::vector<std::uint8_t>(pixels->rgba.begin(), pixels->rgba.end());
  };
  (void)render_at;

  // 路径 A：启动即 2.0。
  st::app::AppOptions options_a;
  options_a.width = 320;
  options_a.height = 160;
  options_a.scale = 2.0f;
  options_a.headless = true;
  options_a.backend = "headless";
  options_a.renderer = "software";
  options_a.control_port = 0;
  options_a.control_file = std::format("{}/st-app-dpi-a.json", st::fs::temp_dir());
  st::app::Application app_a("dpi-a", "0.0", options_a);
  app_a.set_content(make_content());
  if (auto status = app_a.start(); !status) return;
  app_a.render_frame();
  auto pixels_a = app_a.capture_pixels({});
  if (!pixels_a.has_value()) return;
  const std::vector<std::uint8_t> dense(pixels_a->rgba.begin(), pixels_a->rgba.end());

  // 路径 B：启动 1.0，再切到 2.0（与用户在画廊里点 DPI 按钮同一链路）。
  st::app::AppOptions options_b;
  options_b.width = 320;
  options_b.height = 160;
  options_b.scale = 1.0f;
  options_b.headless = true;
  options_b.backend = "headless";
  options_b.renderer = "software";
  options_b.control_port = 0;
  options_b.control_file = std::format("{}/st-app-dpi-b.json", st::fs::temp_dir());
  st::app::Application app_b("dpi-b", "0.0", options_b);
  app_b.set_content(make_content());
  if (auto status = app_b.start(); !status) return;
  app_b.render_frame();
  if (auto status = app_b.set_device_scale(2.0f); !status) return;
  app_b.render_frame();
  auto pixels_b = app_b.capture_pixels({});
  if (!pixels_b.has_value()) return;
  const std::vector<std::uint8_t> switched(pixels_b->rgba.begin(), pixels_b->rgba.end());

  ST_CHECK_EQ(app_a.device_scale(), app_b.device_scale());
  ST_REQUIRE(dense.size() == switched.size());
  std::size_t differing = 0;
  for (std::size_t index = 0; index < dense.size(); ++index) {
    if (dense[index] != switched[index]) ++differing;
  }
  st::print("[app-dpi] 启动即 2.0 vs 1.0→切 2.0：{} / {} 像素不同\n", differing, dense.size());
  ST_CHECK_EQ(static_cast<int>(differing), 0);
}
