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

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

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
