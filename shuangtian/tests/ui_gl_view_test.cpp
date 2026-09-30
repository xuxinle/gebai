/// `GlView` 组件：把三维场景嵌进 2D 界面。
///
/// 这些用例界定的**只是静态帧**行为（渲染一次、像素正确、属性面自洽）。
/// **动画（spin）在真实应用里的 CPU 行为尚未验证**——实测把它接进画廊后进程 CPU 打满
/// （4 分钟耗 313s CPU）且控制通道超时，因此**没有接入画廊**。
/// 未定位的问题不该用"能编过"来掩盖：组件保留、默认不接应用，等查清再接。

#include "st/test/test.hpp"

#include <memory>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/gpu.hpp"
#include "st/ui/components/gl_view.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::GlShape;
using st::ui::GlView;
using st::ui::Theme;

inline constexpr int kWidth = 240;
inline constexpr int kHeight = 180;

[[nodiscard]] auto inked_ratio(const Canvas& canvas, Color background) -> double {
  const std::uint32_t bg = st::math::premultiply(background);
  std::size_t inked = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    if (pixel != bg) ++inked;
  }
  return canvas.pixels().empty()
             ? 0.0
             : static_cast<double>(inked) / static_cast<double>(canvas.pixels().size());
}

struct Harness {
  Theme theme{Theme::light()};
  st::ui::RenderContext context{theme, nullptr, 0.0};
  Canvas canvas{kWidth, kHeight};
  GlView view{GlShape::Cube};

  Harness() {
    view.measure(context, st::ui::Constraints{.max_width = static_cast<float>(kWidth),
                                              .max_height = static_cast<float>(kHeight)});
    view.arrange(context, Rect{0.0f, 0.0f, kWidth, kHeight});
    canvas.clear(Color{0x0A, 0x0E, 0x16, 0xFF});
  }
  /// 渲染一帧（可给时间戳，用于观察旋转是否推进）。
  void paint(double time) {
    canvas.clear(Color{0x0A, 0x0E, 0x16, 0xFF});
    view.paint(st::ui::RenderContext{theme, nullptr, time}, canvas);
  }
};

/// GPU 画布版：`pixels()` 会触发回读，所以只在断言处调用一次。
[[nodiscard]] auto inked_ratio_gpu(st::raster::Surface& canvas, Color background) -> double {
  const std::span<const std::uint32_t> pixels = canvas.pixels();
  const std::uint32_t bg = st::math::premultiply(background);
  std::size_t inked = 0;
  for (const std::uint32_t pixel : pixels) {
    if (pixel != bg) ++inked;
  }
  return pixels.empty() ? 0.0 : static_cast<double>(inked) / static_cast<double>(pixels.size());
}

}  // namespace

ST_TEST(gl_view_reports_property_surface) {
  // 属性面要自洽：`property_names()` 里声明的名字都必须**读得到**，
  // 否则控制通道会出现"声明了但读不出"的静默分裂（统一快照会跳过 nullopt）。
  Harness harness;
  const std::vector<std::string_view> names = harness.view.property_names();
  ST_CHECK(!names.empty());
  for (const std::string_view name : names) {
    const auto value = harness.view.get_property(name);
    ST_CHECK(value.has_value());
  }
  ST_CHECK_EQ(harness.view.get_property("shape").value_or(""), std::string("cube"));
  ST_CHECK_EQ(harness.view.get_property("gl_ready").value_or(""), 
              std::string(GlView::rendering_ready() ? "true" : "false"));
}

ST_TEST(gl_view_properties_are_writable_and_validated) {
  Harness harness;
  ST_CHECK(harness.view.set_property("shape", "sphere"));
  ST_CHECK_EQ(harness.view.get_property("shape").value_or(""), std::string("sphere"));
  ST_CHECK(harness.view.set_property("spin", "false"));
  ST_CHECK_EQ(harness.view.get_property("spin").value_or(""), std::string("false"));
  // 非法输入必须被拒绝（返回 false）而不是静默接受——否则调用方以为改好了
  ST_CHECK(!harness.view.set_property("shape", "torus"));
  ST_CHECK(!harness.view.set_property("spin_speed", "not-a-number"));
  ST_CHECK(!harness.view.set_property("no-such-property", "1"));
}

ST_TEST(gl_view_draws_something_when_opengl_is_available) {
  if (!GlView::rendering_ready()) return;   // 非 Windows（无 GL 上下文）时本用例无意义（不是失败）
  Harness harness;
  harness.paint(0.0);
  const double inked = inked_ratio(harness.canvas, Color{0x0A, 0x0E, 0x16, 0xFF});
  ST_CHECK(inked > 0.02);   // 画面里真有东西（不是空帧、不是纯占位块）
  ST_CHECK(harness.view.get_property("gl_frames").value_or("0") != "0");
}

ST_TEST(gl_view_degrades_to_placeholder_without_opengl) {
  // 没有 GL 时必须画**占位块**而不是空白：空白最难排查
  // （用户会以为是布局问题，实际是能力缺失）。
  if (GlView::rendering_ready()) return;
  Harness harness;
  harness.paint(0.0);
  const double inked = inked_ratio(harness.canvas, Color{0x0A, 0x0E, 0x16, 0xFF});
  ST_CHECK(inked > 0.02);
}

ST_TEST(gl_view_static_frame_is_stable) {
  // 同一时间戳重复绘制，结果必须一致——否则回归截图会每次都不同。
  // （`spin` 只在**时间推进**时改变角度，静态帧不该自己转。）
  if (!GlView::rendering_ready()) return;
  Harness harness;
  harness.paint(1.0);
  const std::vector<std::uint32_t> first(harness.canvas.pixels().begin(),
                                         harness.canvas.pixels().end());
  harness.paint(1.0);
  const std::vector<std::uint32_t> second(harness.canvas.pixels().begin(),
                                          harness.canvas.pixels().end());
  ST_CHECK_EQ(first.size(), second.size());
  std::size_t differing = 0;
  for (std::size_t index = 0; index < first.size(); ++index) {
    if (first[index] != second[index]) ++differing;
  }
  ST_CHECK_EQ(static_cast<int>(differing), 0);
}

ST_TEST(gl_view_composites_into_gpu_canvas_without_hanging) {
  // 这条是**主线程卡死**的回归测试。
  //
  // 早先 `GlScene::composite` 逐像素调 `Surface::set_pixel`，而 GPU 画布的
  // `set_pixel` 每次写入都会触发一次**全屏回读**（2560×1600 = 16 MB）——
  // 192k 像素就是 192k 次全屏回读。表现：主线程 CPU 打满、帧数不增（等价于卡死）。
  //
  // 所以必须**拿 GPU 画布当目标**测：拿软件画布测这条路径永远不会暴露问题
  // （软件 set_pixel 是 O(1) 内存写）。
  if (!st::raster::Scene3D::available()) return;
  auto target = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  if (!target.has_value()) return;   // 本机没有 GPU 画布：本用例不适用
  st::raster::Surface& surface = **target;
  surface.clear(Color{0x0A, 0x0E, 0x16, 0xFF});

  Theme theme = Theme::light();
  GlView view(GlShape::Sphere);
  view.measure(st::ui::RenderContext{theme, nullptr, 0.0},
               st::ui::Constraints{.max_width = static_cast<float>(kWidth),
                                   .max_height = static_cast<float>(kHeight)});
  view.arrange(st::ui::RenderContext{theme, nullptr, 0.0}, Rect{0.0f, 0.0f, kWidth, kHeight});
  // 能返回就说明没有卡死（早先这里会永远不返回）
  view.paint(st::ui::RenderContext{theme, nullptr, 1.0}, surface);

  // 而且真的画进去了（不是"跑完了但什么都没画"）
  const double inked = inked_ratio_gpu(surface, Color{0x0A, 0x0E, 0x16, 0xFF});
  ST_CHECK(inked > 0.02);
}

ST_TEST(gl_view_can_display_an_external_mesh) {
  // 外部模型（如 OBJ 加载结果）必须能显示——否则"能加载"没有落点。
  if (!GlView::rendering_ready()) return;
  constexpr const char* kTriangle = "v 0 0.6 0\nv 0.7 -0.4 0\nv -0.7 -0.4 0\nf 1 2 3\n";
  auto mesh = st::raster::Mesh::load_obj(kTriangle, Color{0x7A, 0xD3, 0x8A, 0xFF});
  ST_CHECK(mesh.has_value());
  if (!mesh.has_value()) return;

  Harness harness;
  harness.view.set_mesh(std::make_shared<const st::raster::Mesh>(std::move(*mesh)));
  ST_CHECK_EQ(harness.view.get_property("mesh_vertices").value_or("0"), std::string("3"));
  harness.paint(0.0);
  const double inked = inked_ratio(harness.canvas, Color{0x0A, 0x0E, 0x16, 0xFF});
  ST_CHECK(inked > 0.01);   // 真的画出了那个三角形
}
