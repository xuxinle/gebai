/// 镂月裁云的**笔画合并**测试：一笔 = 一条历史。
///
/// 这是 C5 的落点，而它解决的问题是量化过的：高频指针下一次长笔画会产生几百次
/// `MouseMove`（框架的帧内合并已经把"每帧一次"做到了，见 `ui_input_coalescer_test`），
/// 而每次 `stroke()` 都会推一条历史条目——不合并的话历史面板里是几百条「画笔」，
/// 用户按一次撤销只退一小段，判定"撤销坏了"。
///
/// 合并正确与否不能只看"条数变少了"：并集矩形的 `before_` 拼错会让**撤销后画布上
/// 留着一截笔迹**（撤销看起来"部分生效"）。所以本文件断言的是
/// **撤销后画布逐像素回到原样**——那才是"一条历史"该有的语义。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "st/test/test.hpp"

#include "examples/louyue/document.hpp"
#include "examples/louyue/document.cpp"

namespace {

using louyue::Canvas;
using louyue::Color;
using louyue::Document;
using louyue::Rect;

/// 造一个 64x64、单图层、底色全透明的文档。
[[nodiscard]] auto make_document() -> std::unique_ptr<Document> {
  auto document = std::make_unique<Document>(64, 64);
  document->add_layer("图层 1");
  return document;
}

/// 直接在画布上画一个实心圆（跳过 `CanvasView` 的工具链——本文件要测的是**历史**，
/// 不是笔刷算法；用最直接的写像素让断言与"哪些像素被改了"完全对应）。
void paint_disc(Canvas& canvas, float center_x, float center_y, float radius, Color color) {
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const float dx = static_cast<float>(x) - center_x;
      const float dy = static_cast<float>(y) - center_y;
      if (dx * dx + dy * dy <= radius * radius) {
        canvas.set_pixel(x, y, color);
      }
    }
  }
}

/// 一次"落笔"：取补丁前后 → 推入（与 `CanvasView::stroke` 同一套顺序）。
void do_stroke(Document& document, const Rect& area, float center_x, float center_y,
               const std::string& label = "画笔") {
  louyue::PixelPatchCommand patch{document.active_layer(), area, label};
  patch.capture_before(document);
  paint_disc(document.active_pixels(), center_x, center_y, 3.0f, Color{255, 0, 0, 255});
  patch.capture_after(document);
  if (!patch.changed()) return;
  document.push(std::make_unique<louyue::PixelPatchCommand>(std::move(patch)));
}

/// 画布像素快照（整层），用于"撤销后是否逐像素回到原样"。
[[nodiscard]] auto snapshot(Document& document) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  const Canvas& canvas = document.active_pixels();
  out.reserve(static_cast<std::size_t>(canvas.physical_width()) * canvas.physical_height() * 4U);
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      out.push_back(pixel.r);
      out.push_back(pixel.g);
      out.push_back(pixel.b);
      out.push_back(pixel.a);
    }
  }
  return out;
}

}  // namespace

ST_TEST(louyue_continuous_strokes_merge_into_one_history_entry) {
  auto document = make_document();
  const auto blank = snapshot(*document);
  ST_CHECK_EQ(document->history_size(), std::size_t{0});

  // 一次拖动：20 次落笔，每次往前挪 2px（模拟帧内合并后的"每帧一次"）。
  for (int step = 0; step < 20; ++step) {
    const float x = 12.0f + static_cast<float>(step) * 2.0f;
    do_stroke(*document, Rect{x - 5.0f, 27.0f, 12.0f, 12.0f}, x, 32.0f);
  }
  // **一笔 = 一条历史**（而不是 20 条）。
  ST_CHECK_EQ(document->history_size(), std::size_t{1});

  const auto drawn = snapshot(*document);
  ST_CHECK(drawn != blank);

  // 一次撤销必须**逐像素**回到原样（这是"一条历史"的硬判据：
  // 若并集矩形的 before 拼错，撤销后会留下一截笔迹，而条数断言看不出来）。
  ST_CHECK(document->undo());
  ST_CHECK(snapshot(*document) == blank);
  // 重做回到画完的样子（同样逐像素）。
  ST_CHECK(document->redo());
  ST_CHECK(snapshot(*document) == drawn);
}

ST_TEST(louyue_two_strokes_are_two_history_entries) {
  auto document = make_document();
  const auto blank = snapshot(*document);

  // 第一笔。
  do_stroke(*document, Rect{5.0f, 5.0f, 12.0f, 12.0f}, 10.0f, 10.0f);
  ST_CHECK_EQ(document->history_size(), std::size_t{1});
  // 抬起鼠标 = 封笔。
  document->seal_stroke();
  // 第二笔（同一处、同一工具）——**不该**并进第一笔。
  do_stroke(*document, Rect{5.0f, 5.0f, 12.0f, 12.0f}, 11.0f, 11.0f);
  ST_CHECK_EQ(document->history_size(), std::size_t{2});
  ST_CHECK(document->stroke_open());

  // 撤销一次只退第二笔：画布上仍有第一笔的痕迹。
  ST_CHECK(document->undo());
  const auto after_one_undo = snapshot(*document);
  ST_CHECK(after_one_undo != blank);   // 第一笔还在
  // 再撤销一次才回到空白。
  ST_CHECK(document->undo());
  ST_CHECK(snapshot(*document) == blank);
}

ST_TEST(louyue_merge_requires_same_layer_and_tool) {
  auto document = make_document();
  document->add_layer("图层 2");

  // 第 0 层画一笔。
  document->set_active_layer(0);
  do_stroke(*document, Rect{5.0f, 5.0f, 12.0f, 12.0f}, 10.0f, 10.0f, "画笔");
  // 切到第 1 层再画一笔（同位置）：**不同图层不该并**——并了撤销会落到错误的层。
  document->set_active_layer(1);
  do_stroke(*document, Rect{5.0f, 5.0f, 12.0f, 12.0f}, 11.0f, 11.0f, "画笔");
  ST_CHECK_EQ(document->history_size(), std::size_t{2});

  // 同图层但**不同工具**（标签不同）也不并。
  document->set_active_layer(0);
  do_stroke(*document, Rect{5.0f, 5.0f, 12.0f, 12.0f}, 12.0f, 12.0f, "橡皮");
  ST_CHECK_EQ(document->history_size(), std::size_t{3});
}

ST_TEST(louyue_merge_skips_distant_rects) {
  // 位置**不相邻**的两笔不该并：并了会把中间未改的区域也存进前后快照
  //（内存暴涨，而且撤销会"顺带复原"中间那些像素）。
  auto document = make_document();
  do_stroke(*document, Rect{2.0f, 2.0f, 10.0f, 10.0f}, 6.0f, 6.0f);
  ST_CHECK_EQ(document->history_size(), std::size_t{1});
  // 画布另一头（相距远超 1px 的接壤余量）。
  do_stroke(*document, Rect{50.0f, 50.0f, 10.0f, 10.0f}, 55.0f, 55.0f);
  ST_CHECK_EQ(document->history_size(), std::size_t{2});
}

ST_TEST(louyue_merge_does_not_cross_undo_boundary) {
  // 撤销过之后再落笔 = 新的一段（未来被丢弃）。那种情况下**不该**并进栈顶：
  // 并了会让"撤销一次退到别的地方"。
  auto document = make_document();
  do_stroke(*document, Rect{5.0f, 5.0f, 12.0f, 12.0f}, 10.0f, 10.0f);
  document->seal_stroke();
  do_stroke(*document, Rect{5.0f, 5.0f, 12.0f, 12.0f}, 11.0f, 11.0f);
  ST_CHECK_EQ(document->history_size(), std::size_t{2});
  ST_CHECK(document->undo());   // 退掉第二笔

  // 现在落一笔新的（同处、同工具）：光标不在末尾，必须**新增**一条而不是并进旧的。
  do_stroke(*document, Rect{5.0f, 5.0f, 12.0f, 12.0f}, 12.0f, 12.0f);
  ST_CHECK_EQ(document->history_size(), std::size_t{2});   // 3 条被截成 2 条（未来丢弃）

  // 撤销必须回到**第一笔之后**的状态（而不是第三笔之前那种"什么都没有"）。
  //
  // ⚠ 这里断言"撤一次之后仍可再撤"（栈里还有第一笔），而**不能**断言撤到底——
  // 分支落笔把未来截掉之后，栈里剩的是「第一笔 + 新一笔」两条（不是一条）。
  // 第一版断言 `!can_undo()`，白追一轮：那是**我的预期**错了，不是实现错了。
  ST_CHECK(document->undo());
  ST_CHECK(document->can_undo());   // 第一笔还在，可以继续撤
  const auto after_first_undo = snapshot(*document);
  ST_CHECK(document->undo());
  ST_CHECK(!document->can_undo());  // 撤到底
  ST_CHECK(snapshot(*document) != after_first_undo);
}
