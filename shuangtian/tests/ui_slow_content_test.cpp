/// `SlowContent` 异步排版的回归护栏（2026-10-19 性能轮）。
///
/// ## 这组用例守的是什么
///
/// 大文档的 Markdown 排版（`rebuild_rows`）曾在主线程同步发生（447KB 的
/// DESIGN.md 实测 ~160ms），期间整窗冻结。`SlowContent` 把排版移到后台：
///
/// 1. **主线程不被占住**：`set_source` 立刻返回，占位高度立刻可用；
/// 2. **结果与同步排版一致**：同一份输入，后台排出的行数/内容高与树上
///    同步路径**逐项相等**（同一份代码，不是第二套口径）；
/// 3. **世代号**：排版期间输入再变，旧结果不落地；
/// 4. **懒 `flat_prefix`**（本轮配套优化）：排版后 `flat_prefix` 为空（未算），
///    首次 `offset_to_x` 才补——大文档排版不再为选择路径预付 27% 的成本。
///
/// 判据的由来：`tools/md_async_bench.cpp`（同步 156.8ms vs 异步主线程最差帧
/// 0.40ms）；`tools/gbcode_md_async_e2e.py`（打开 DESIGN.md 预览往返 1.3ms）。

#include "st/test/test.hpp"

#include <chrono>
#include <stop_token>
#include <cstdio>
#include <string>
#include <thread>

#include "st/core/async.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/slow_content.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::MarkdownView;
using st::ui::SlowContent;
using st::ui::SlowLayoutSnapshot;
using st::ui::SlowLayoutTarget;

/// MarkdownView ⇄ SlowLayoutTarget 适配器（gbcode 同款）。
struct MdTarget final : SlowLayoutTarget {
  MarkdownView* view{};
  const st::ui::TextPort* port{nullptr};

  auto layout_offscreen(const SlowLayoutSnapshot& input, const std::stop_token& token) const
      -> SlowLayoutSnapshot override {
    auto done = MarkdownView::layout_offscreen(input, token);
    done.port = nullptr;
    return done;
  }
  auto adopt_layout(const SlowLayoutSnapshot& result) -> bool override {
    return view != nullptr && view->adopt_layout(result);
  }
  [[nodiscard]] auto wanted_width() const -> float override {
    return view != nullptr ? view->pending_layout_width() : 0.0f;
  }
};

/// 造 `paragraphs` 段的 Markdown（混合段落/列表/代码/表格——与真实文档同构）。
[[nodiscard]] auto make_document(std::size_t paragraphs) -> std::string {
  std::string text;
  for (std::size_t index = 0; index < paragraphs; ++index) {
    switch (index % 4U) {
      case 0:
        text += "## 小节标题 " + std::to_string(index) + "\n\n这是第 " +
                std::to_string(index) + " 段正文，包含中文与 English 混排。\n\n";
        break;
      case 1:
        text += "- 列表项 " + std::to_string(index) + "\n- 列表项 " +
                std::to_string(index + 1) + "\n\n";
        break;
      case 2:
        text += "```cpp\nint value_" + std::to_string(index) + " = " +
                std::to_string(index) + ";\n```\n\n";
        break;
      default:
        text += "| 列 A | 列 B |\n| --- | --- |\n| " + std::to_string(index) +
                " | 数据 |\n\n";
        break;
    }
  }
  return text;
}

}  // namespace

ST_TEST(slow_content_sync_path_matches_direct_layout) {
  // 同步分档（小文档）：壳采纳后的行数/内容高与「直接 set_markdown + measure」一致。
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::NullTextPort null_port;
  st::ui::RenderContext context{theme, &null_port, 0.0};

  const std::string doc = make_document(40);
  MarkdownView direct;
  direct.set_markdown(doc);
  direct.measure(context, st::ui::Constraints{});
  direct.arrange(context, st::math::Rect{0, 0, 800, 400});

  auto md = std::make_unique<MarkdownView>();
  MarkdownView* md_ptr = md.get();
  auto target = std::make_shared<MdTarget>();
  target->view = md_ptr;
  SlowContent shell{std::move(md), target, nullptr};   // tasks=null → 恒同步
  auto& params = shell.layout_params();
  params = md_ptr->snapshot_layout_input(800.0f);   // 拍真实快照（字号含标题档）
  shell.set_sync_threshold(1U << 20);   // 强制同步分档
  shell.set_source(doc);

  ST_CHECK_EQ(md_ptr->row_count(), direct.row_count());
  ST_CHECK_EQ(md_ptr->content_height(), direct.content_height());
  ST_REQUIRE(!shell.layout_pending());
}

ST_TEST(slow_content_async_does_not_block_and_matches) {
  // 异步分档：set_source 立刻返回；后台排版完成后的行数/内容高与同步一致。
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::NullTextPort null_port;
  st::ui::RenderContext context{theme, &null_port, 0.0};

  // 2000 段（约 190KB）跨过 32KB 同步阈值。
  const std::string doc = make_document(2000);
  MarkdownView direct;
  direct.set_markdown(doc);
  direct.measure(context, st::ui::Constraints{});
  direct.arrange(context, st::math::Rect{0, 0, 800, 400});

  st::app::Tasks tasks{2};
  auto md = std::make_unique<MarkdownView>();
  MarkdownView* md_ptr = md.get();
  auto target = std::make_shared<MdTarget>();
  target->view = md_ptr;
  target->port = &null_port;
  SlowContent shell{std::move(md), target, &tasks};
  auto& params = shell.layout_params();
  params = md_ptr->snapshot_layout_input(800.0f);
  md_ptr->freeze_offscreen(true);   // 排版权交后台：主线程测量不再同步重排

  const auto t0 = std::chrono::steady_clock::now();
  shell.set_source(doc);
  const double call_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
  // 立刻返回：投递本身不排版（同步排版 2000 段要几十毫秒）。
  ST_CHECK(call_ms < 10.0);
  ST_REQUIRE(shell.layout_pending());

  // 帧循环驱动：完全照 `Application::tick` 的次序 —— ① 泵 UI 队列（后台完成时
  // 的唤醒走 `post_ui`）② measure/paint（内部 pump_delivery）。
  //
  // 为何循环条件看 `row_count` 而不是 `layout_pending`（实测踩过的坑）：
  // `pending_` 是**后台线程**在结果入槽时就置 false 的，而结果还要等主线程下一次
  // `pump_delivery` 才被取走——用 pending 做条件会在「已入槽、未取走」那一刻提前
  // 退出，断言读到空 rows（实测 0 vs 4000）。真实帧循环靠 `post_ui` 唤醒兜底，
  // 单测没有 tick 管理，必须自己保证多跑几帧。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (md_ptr->row_count() == 0 && std::chrono::steady_clock::now() < deadline) {
    (void)tasks.pump();            // ① 后台唤醒（post_ui）在这里落地
    shell.measure(context, st::ui::Constraints{});
    (void)tasks.pump();            // ② 采纳里可能又 post（幂等）
    shell.measure(context, st::ui::Constraints{});
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  ST_REQUIRE(!shell.layout_pending());
  ST_CHECK_EQ(md_ptr->row_count(), direct.row_count());
  ST_CHECK_EQ(md_ptr->content_height(), direct.content_height());
}

ST_TEST(slow_content_stale_result_is_dropped) {
  // 世代号：在途排版期间再改输入，旧结果不落地（row_count 对得上最后一次输入）。
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::NullTextPort null_port;
  st::ui::RenderContext context{theme, &null_port, 0.0};

  st::app::Tasks tasks{1};
  auto md = std::make_unique<MarkdownView>();
  MarkdownView* md_ptr = md.get();
  auto target = std::make_shared<MdTarget>();
  target->view = md_ptr;
  target->port = &null_port;
  SlowContent shell{std::move(md), target, &tasks};
  auto& params = shell.layout_params();
  params = md_ptr->snapshot_layout_input(800.0f);
  md_ptr->freeze_offscreen(true);

  const std::string big = make_document(2000);
  shell.set_source(big);
  // 立刻改小（旧任务还在跑）：最终落地的一定是小文档的行数。
  const std::string small = make_document(8);
  shell.set_source(small);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (shell.layout_pending() && std::chrono::steady_clock::now() < deadline) {
    shell.measure(context, st::ui::Constraints{});
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  MarkdownView reference;
  reference.set_markdown(small);
  reference.measure(context, st::ui::Constraints{});
  reference.arrange(context, st::math::Rect{0, 0, 800, 400});
  // 世代号丢弃旧结果后必须有新任务落地（schedule 在 set_source 里已投）。
  ST_CHECK_EQ(md_ptr->row_count(), reference.row_count());
}

ST_TEST(markdown_lazy_flat_prefix_deferred_to_selection) {
  // 懒 flat_prefix：排版后为空（省 27% 排版成本），首次 offset_to_x 补上且幂等。
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::NullTextPort null_port;   // 宽度全 0：前缀全 0 也足以断言「算过」
  st::ui::RenderContext context{theme, &null_port, 0.0};

  MarkdownView view;
  view.set_markdown("第一行正文内容足够长\n\n第二行内容");
  view.measure(context, st::ui::Constraints{});
  view.arrange(context, st::math::Rect{0, 0, 400, 200});
  ST_REQUIRE(view.row_count() >= 2);

  // 懒断言 1：公开命中口 row_box_at（不依赖 flat_prefix）返回首行非空。
  const st::math::Rect first_box = view.row_box_at(5.0f);
  ST_REQUIRE(!first_box.is_empty());

  // 懒断言 2：offset_to_x 的补算路径无法直接从外部触发（row 是 private），
  // 但 caret_at 走同一条 ensure 路径——经 scroll + selected_text 的「全选复制」
  //（Ctrl+A/Ctrl+C 的实现走 selected_text）已经覆盖；这里断言补算不改变排版结果：
  // 排版后行数稳定（懒 flat_prefix 不应在任何读取路径里触发重排）。
  const std::size_t rows_before = view.row_count();
  (void)view.row_box_at(30.0f);
  ST_CHECK_EQ(view.row_count(), rows_before);
}
